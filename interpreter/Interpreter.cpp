#include "interpreter/Interpreter.h"
#include "Logger.h"
#include "common/BoundsCheck.h" // Dedup-7A: inBounds 替代重复的索引检查
#include "debug/DebugController.h"
#include "interpreter/BuiltinMethods.h"
#include "interpreter/ErrorFormat.h" // P3 fix: runtimeErrorFmt 替代 std::to_string 拼接
#include "interpreter/NumericUtils.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include <cctype>
#include <climits>
#include <cmath> // BUG 8.1 fix: std::fmod
#include <cstdint>
#include <map> // #1 fix: 条件断点沙箱化 — 实例字段快照去重
#include <sstream>
#include <unordered_set>

// ============================================================
// Interpreter.cpp — 树遍历解释器（Visitor 模式）主实现
// ------------------------------------------------------------
// MiniLang 三套执行引擎之一：直接遍历 AST 执行，不生成中间字节码。
// 实现 Visitor 接口的 33 个 visit* 方法，对每种 AST 节点执行"语义动作"
// （先 checkBreak 支持断点/中止，再求值并将结果写入 lastValue_，由 evaluate() 取出）。
//
// 本文件按职责分段（见各段注释头）：
//   - 顶层执行入口：execute / executeRepl / runStatementsWithExceptionHandling
//     （状态重置、REPL 状态保存/恢复、Run 起点触发 GC 回收残留循环引用、
//     压入顶层 main 调用帧）。
//   - 安全求值：evaluateExpr / evaluateCondition（条件断点沙箱，深拷贝容器以隔离副作用）。
//   - 数值/比较/类型：numericBinaryOp / compareNumericOrString / typeMatch / checkType。
//   - 左值写回：writeBack / collectAndEvaluateChain / writeBackChain
//     （链式求值从外到内、从内到外，避免对含副作用的子表达式重复求值）。
//   - 闭包与自由变量：visitFunDecl / computeFreeVariables
//     （静态分析 AST，仅捕获函数体实际引用的外层变量，并在定义处注册 open upvalue）。
//   - 函数调用分派：visitFunCall → callClosureValue / callNamedFunction /
//     callBuiltinFunction / callBuiltinConstructor / constructClassInstance
//     （默认参数、闭包环境过期时从 capturedVars 快照重建并回写、递归深度保护、RAII 守卫）。
//   - 类/对象/方法：visitMethodCall / callInstanceMethod（继承链方法查找、super 解析、
//     this 实例绑定、init 构造），类声明/成员/继承见 InterpreterClasses.cpp。
//   - 模块系统：import/export 见 InterpreterModules.cpp（路径安全、循环依赖检测、mtime 缓存失效）。
//
// 值表示、作用域链、引用计数与循环引用 GC 分别见 Value.h/NaNBox.h、Environment.h、
// RefCounted.h、GcManager.cpp：本文件在这些运行时基础设施之上组合出语言语义。
// ============================================================

Interpreter::Interpreter() : globalEnv_(std::make_shared<Environment>()), currentEnv_(globalEnv_), recursionDepth_(0) {
    outputCallback_ = [](const std::string&) {};
}

Interpreter::~Interpreter() {
    // shared_ptr 自动管理环境生命周期，无需手动 delete
}

Value Interpreter::execute(Block& program) {
    // 重置状态
    // BUG-DBG-9 fix: 重置 stopRequested_ 标志，避免上一轮"停止"按钮终止后残留 true
    // 导致本轮立即在首个 checkBreak 抛 DebugStopException。原 execute() 遗漏此重置
    // （executeRepl 有重置但 execute 没有），Run 模式下连续运行会立即中止。
    stopRequested_.store(false, std::memory_order_relaxed);
    // AUDIT-P1-CORRECT fix: 重置条件求值步数计数器，确保正常执行不被误计数
    evaluationStepCount_ = 0;
    // B1 fix: 关闭旧 globalEnv_ 上的闭包捕获，将最终值写回闭包 capturedVars。
    // 旧 globalEnv_ 即将被替换，其上的 weak_ptr 会失效，闭包需依赖 capturedVars 快照。
    if (globalEnv_) {
        globalEnv_->closeCapturedVariables();
    }
    globalEnv_ = std::make_shared<Environment>();
    currentEnv_ = globalEnv_;
    callStack_.clear();
    envPool_.clear(); // PERF-07: 旧环境链已销毁，清空池避免悬垂 parent 引用
    funRegistry_.clear();
    classRegistry_.clear();
    currentFunctionReturnType_.clear();
    recursionDepth_ = 0;
    replAsts_.clear();    // 释放 REPL 保留的 AST
    diagnostics_.clear(); // 清空诊断信息
    // R7 fix: 清理 classContextStack_，防止用户通过"停止"按钮终止运行（terminate）
    // 时 CallFrameGuard 析构器未执行导致的残留——后续 super 调用会用错误类名分派。
    classContextStack_.clear();
    // P0-1 fix: 清理模块缓存，避免 replAsts_.clear() 后缓存中的悬垂指针
    moduleCache_.clear();
    moduleExports_.clear();
    moduleLoadingStack_.clear();
    moduleLoadingSet_.clear(); // D19 fix: 同步清理 set
    exportedNames_.clear();

    // Bug2 fix: 在 resetState 之后、runStatements 之前触发 GcManager mark-sweep。
    // 此时上一轮残留的循环引用容器 refCount>0 仍存活（aliveSet_ 中），
    // 而非循环容器已在上述 clear() 中被自然释放（析构时从 aliveSet_ 移除）。
    // BUG-INT-1 fix: 若 REPL 状态已保存（saveReplState），其 savedGlobalEnv 中的
    // 循环引用容器仍在 aliveSet_ 中存活。传入空根集会误清空这些容器的子元素，
    // 导致 restoreReplState 后 REPL 变量损坏。必须将 savedGlobalEnv 中的容器作为根集传入。
    // AUDIT-P1 fix: 原 GC roots 仅遍历 savedGlobalEnv，遗漏 savedClassRegistry.fields
    // 和 savedModuleCache。类字段默认值（如 var data = [1,2,3]）和模块顶层变量
    // 可能持有堆对象引用，若不作为根集传入，GC 会误判为循环引用孤岛并清空 elements，
    // 导致 restoreReplState 后类字段默认值丢失、模块变量变成空容器。
    std::vector<const void*> gcRoots;
    // AUDIT-P2-CORRECT fix: 无论 REPL 是否激活，都应将 globalEnv_ 顶层变量纳入 GC roots。
    // 原实现仅 REPL 模式收集 roots，非 REPL 模式传入空根集。若 Interpreter 被复用
    // （如测试场景），上一轮残留的循环引用容器在 tracked_ 中，空根集会导致 Phase 2
    // 误判所有 tracked 容器为不可达孤岛并清空子元素。即使 IdeController 每次 Run
    // 创建新 Interpreter（tracked_ 为空，collectCycle 提前返回），此处防御性收集
    // 可保证复用场景的正确性。
    if (!replState_.active) {
        if (globalEnv_) {
            auto vars = globalEnv_->snapshotLocalVariables();
            for (const auto& var : vars) {
                const void* ptr = var.second.gcRootPtr();
                if (ptr)
                    gcRoots.push_back(ptr);
            }
        }
    }
    if (replState_.active) {
        // (1) savedGlobalEnv 顶层变量
        if (replState_.savedGlobalEnv) {
            auto vars = replState_.savedGlobalEnv->snapshotLocalVariables();
            for (const auto& var : vars) {
                const void* ptr = var.second.gcRootPtr();
                if (ptr)
                    gcRoots.push_back(ptr);
            }
        }
        // (2) savedClassRegistry 中每个 ClassInfo.fields 的默认值 + closureEnv
        for (const auto& clsPair : replState_.savedClassRegistry) {
            for (const auto& fldPair : clsPair.second.fields) {
                const void* ptr = fldPair.second.gcRootPtr();
                if (ptr)
                    gcRoots.push_back(ptr);
            }
            if (clsPair.second.closureEnv) {
                auto clsVars = clsPair.second.closureEnv->snapshotLocalVariables();
                for (const auto& var : clsVars) {
                    const void* ptr = var.second.gcRootPtr();
                    if (ptr)
                        gcRoots.push_back(ptr);
                }
            }
        }
        // (3) savedModuleCache 中每个 Environment 的顶层变量
        for (const auto& modPair : replState_.savedModuleCache) {
            if (modPair.second) {
                auto modVars = modPair.second->snapshotLocalVariables();
                for (const auto& var : modVars) {
                    const void* ptr = var.second.gcRootPtr();
                    if (ptr)
                        gcRoots.push_back(ptr);
                }
            }
        }
    }
    // AUDIT-P3-CORRECT fix: 防御性收集 callStack_ 中的环境变量作为 GC roots。
    // 当前 callStack_ 在行 71 已 clear()，此处为空，但防御性收集保证未来安全——
    // 若未来在 callStack_ 非空时触发 collectCycle（如条件断点求值中添加 GC 触发，
    // 或 REPL 模式添加 GC），栈上闭包引用的循环容器需被标记为可达，否则会被误回收。
    for (const auto& frame : callStack_) {
        if (frame.env) {
            auto vars = frame.env->snapshotLocalVariables();
            for (const auto& var : vars) {
                const void* ptr = var.second.gcRootPtr();
                if (ptr)
                    gcRoots.push_back(ptr);
            }
        }
    }
    GcManager::instance().collectCycle(gcRoots);

    // BUG-DBG-5 fix: 压入顶层 main 帧，与 VM 的 mainFrame 对齐。
    // 原实现 callStack_ 在顶层为空，导致调试暂停在顶层代码时调用栈面板无显示，
    // 而 VM 路径显示 "main" 帧。checkBreak (BUG-DBG-1 fix) 会更新顶帧行号为当前执行行。
    // CallFrameGuard 会保护此帧（savedStackDepth 包含 main 帧，函数调用只 push 更深帧）。
    // 下次 execute() 入口 callStack_.clear() 自动清理。
    callStack_.emplace_back("main", globalEnv_, 0, 0);

    // 顶层块不创建新作用域，直接在全局环境中执行语句
    Value result = runStatementsWithExceptionHandling(program);

    return result;
}

Value Interpreter::executeRepl(Block& program) {
    // 不重置环境，保留已有变量/函数/类定义
    // 清除 funRegistry_ 中的 AST 裸指针（旧 AST 可能已被销毁，闭包自带 body 指针不受影响）
    funRegistry_.clear();
    stopRequested_.store(false, std::memory_order_relaxed); // 重置中止标志
    // AUDIT-P1-CORRECT fix: 重置条件求值步数计数器，确保正常执行不被误计数
    evaluationStepCount_ = 0;
    funRegistryGen_++; // H5 fix: 使所有旧缓存的 resolvedDecl 指针失效，防止野指针访问
    // classRegistry_ 不清除 — 类定义需要跨 REPL 行保留（AST 由 replAsts_ 保持存活）
    // 确保当前环境回到全局
    currentEnv_ = globalEnv_;
    recursionDepth_ = 0;
    diagnostics_.clear(); // 清空诊断信息
    // R7 fix: 清理执行栈状态，防止 terminate() 终止后残留（与 execute() 对齐）。
    // 正常路径下 CallFrameGuard RAII 会清空，此处是防御性兜底。
    callStack_.clear();
    classContextStack_.clear();
    currentFunctionReturnType_.clear();
    // AUDIT-P3-CORRECT fix: 清空 envPool_，与 execute() 对齐。
    // 原实现遗漏此清空，导致 REPL 模式下 envPool_ 跨次累积——若某次运行有 N 层嵌套块，
    // pool 回收 N 个 Environment；下次运行若只有 M < N 层，只复用 M 个，剩余 N-M 个残留。
    // 长时间 REPL 会话中 envPool_ 大小单调不降（受限于历史最大嵌套深度）。
    envPool_.clear();
    // BUG-DBG-5 fix: REPL 模式同样压入 main 帧，与 execute() 和 VM 行为对齐
    callStack_.emplace_back("main", globalEnv_, 0, 0);

    return runStatementsWithExceptionHandling(program);
}

// P1-4 fix: execute/executeRepl 共享的语句执行 + 异常处理逻辑。
// 捕获 4 类异常：ReturnException/ThrowException/RuntimeError。
// RA-C fix: BreakException/ContinueException 已改为状态标志，不再走异常路径。
// 顶层出现未消费的 break/continue 标志时，在 for 循环内检查并报错（等价于原兜底 catch）。
Value Interpreter::runStatementsWithExceptionHandling(Block& program) {
    Value result = Value::nullValue();
    try {
        for (auto& stmt : program.statements) {
            result = evaluate(stmt.get());
            // RA-C fix: 顶层出现未消费的 break/continue 标志，报错
            // （等价于原 catch BreakException/ContinueException 兜底）
            if (loopFlow_ != LoopFlow::None) {
                std::string msg =
                    (loopFlow_ == LoopFlow::Break) ? "break 只能在循环体内使用" : "continue 只能在循环体内使用";
                loopFlow_ = LoopFlow::None;
                runtimeError(msg, stmt ? stmt->line : 0, stmt ? stmt->column : 0);
            }
        }
    } catch (const ReturnException&) {
        runtimeError("return 只能在函数体内使用", 0, 0);
    } catch (const ThrowException& e) {
        // BUG-IBACKEND-4 fix: 三后端未捕获异常消息一致——统一 toString + 200 字符截断
        // （对齐 StackVM VM.cpp:130-132 与 RegisterVM）
        std::string str = e.thrownValue.toString();
        if (str.size() > 200)
            str = str.substr(0, 200) + "...";
        runtimeError("未捕获的异常: " + str, 0, 0);
    } catch (const RuntimeError& e) {
        // 记录到诊断包后重新抛出，保持原有异常传播机制
        diagnostics_.addError(e.what(), e.line, e.column, DiagSource::Interpreter);
        throw;
    }
    return result;
}

void Interpreter::retainReplAst(std::unique_ptr<Block> ast) {
    replAsts_.push_back(std::move(ast));
}

void Interpreter::saveReplState() {
    // ARCH-12 fix: 聚合到 ReplState 结构体
    replState_.savedGlobalEnv = globalEnv_;
    replState_.savedClassRegistry = std::move(classRegistry_);
    replState_.savedReplAsts = std::move(replAsts_);
    // BUG7 fix: 保存函数注册表，避免 Run→REPL 切换后 funRegistry_ 丢失
    replState_.savedFunRegistry = std::move(funRegistry_);
    replState_.savedFunRegistryGen = funRegistryGen_;
    // P1-1 fix: 保存模块相关状态，避免 Run→REPL 切换后悬垂指针
    replState_.savedModuleCache = std::move(moduleCache_);
    replState_.savedModuleExports = std::move(moduleExports_);
    replState_.savedExportedNames = exportedNames_;
    replState_.savedModuleLoadingStack = moduleLoadingStack_;
    replState_.savedModuleLoadingSet = moduleLoadingSet_; // D19 fix: 同步保存 set
    // AUDIT-P1 fix: 保存 moduleMtimes_，与 moduleCache_ 保持对称。
    // 原实现遗漏此字段，restoreReplState 用空 map 覆盖 moduleMtimes_，
    // 导致后续 import 命中缓存时跳过 mtime 变更检查，磁盘修改被忽略。
    replState_.savedModuleMtimes = moduleMtimes_;
    replState_.active = true;
}

void Interpreter::restoreReplState() {
    // ARCH-12 fix: 防御性检查 — 未 save 就 restore 是调用方 bug，静默返回避免状态损坏
    if (!replState_.active)
        return;
    globalEnv_ = replState_.savedGlobalEnv;
    currentEnv_ = globalEnv_;
    classRegistry_ = std::move(replState_.savedClassRegistry);
    replAsts_ = std::move(replState_.savedReplAsts);
    // BUG7 fix: 恢复函数注册表
    funRegistry_ = std::move(replState_.savedFunRegistry);
    funRegistryGen_ = replState_.savedFunRegistryGen;
    // P1-1 fix: 恢复模块相关状态
    moduleCache_ = std::move(replState_.savedModuleCache);
    moduleExports_ = std::move(replState_.savedModuleExports);
    exportedNames_ = std::move(replState_.savedExportedNames);
    moduleLoadingStack_ = std::move(replState_.savedModuleLoadingStack);
    moduleLoadingSet_ = std::move(replState_.savedModuleLoadingSet); // D19 fix: 同步恢复 set
    // P2-A fix: 恢复 moduleMtimes_，与 moduleCache_ 保持一致的时间戳基准
    moduleMtimes_ = std::move(replState_.savedModuleMtimes);
    replState_.savedGlobalEnv.reset();
    replState_.active = false;
    // R7 fix: 清理执行栈状态，防止 Run 被 terminate() 终止后残留——
    // saveReplState 在 terminate 后被调用，此时 classContextStack_/callStack_
    // 可能残留未展开的条目，恢复 REPL 状态后必须清空。
    callStack_.clear();
    classContextStack_.clear();
    currentFunctionReturnType_.clear();
    recursionDepth_ = 0;
}

void Interpreter::resetReplEnvironment() {
    stopRequested_.store(false, std::memory_order_relaxed);
    evaluationStepCount_ = 0;
    if (globalEnv_) {
        globalEnv_->closeCapturedVariables();
    }
    globalEnv_ = std::make_shared<Environment>();
    currentEnv_ = globalEnv_;
    callStack_.clear();
    envPool_.clear();
    funRegistry_.clear();
    funRegistryGen_ = 0;
    classRegistry_.clear();
    classRegistryGen_ = 0;
    currentFunctionReturnType_.clear();
    recursionDepth_ = 0;
    replAsts_.clear();
    diagnostics_.clear();
    classContextStack_.clear();
    moduleCache_.clear();
    moduleExports_.clear();
    moduleMtimes_.clear();
    moduleLoadingStack_.clear();
    moduleLoadingSet_.clear();
    exportedNames_.clear();
    loopFlow_ = LoopFlow::None;
    replState_.savedGlobalEnv.reset();
    replState_.savedClassRegistry.clear();
    replState_.savedReplAsts.clear();
    replState_.savedFunRegistry.clear();
    replState_.savedFunRegistryGen = 0;
    replState_.savedModuleCache.clear();
    replState_.savedModuleExports.clear();
    replState_.savedExportedNames.clear();
    replState_.savedModuleLoadingStack.clear();
    replState_.savedModuleLoadingSet.clear();
    replState_.savedModuleMtimes.clear();
    replState_.active = false;
}

void Interpreter::setOutputCallback(std::function<void(const std::string&)> callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    outputCallback_ = callback;
}

void Interpreter::setInputCallback(std::function<std::string(const std::string&)> callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    inputCallback_ = callback;
}

void Interpreter::setModuleLoader(std::function<std::string(const std::string&)> loader) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    moduleLoader_ = loader;
}

// BUG-REPL-AUDIT-9 fix: 检查是否已设置模块加载器
bool Interpreter::hasModuleLoader() const {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    return static_cast<bool>(moduleLoader_);
}

// BUG-REPL-AUDIT-1 fix: 模块文件 mtime 检查器设置
void Interpreter::setModuleMtimeChecker(std::function<int64_t(const std::string&)> checker) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    moduleMtimeChecker_ = checker;
}

void Interpreter::setCurrentFilePath(const std::string& path) {
    currentFilePath_ = path;
}

void Interpreter::setDebugger(std::shared_ptr<DebugController> dbg) {
    // #26 fix: 加锁与 setModuleLoader/setOutputCallback 等 setter 保持一致。
    // checkBreak 在 worker 线程读取 debugger_ 不加锁——安全前提是 debugger_ 在
    // execute() 开始后不再变更（调用方 IdeController 在启动 worker 前设置，执行期间不变）。
    std::lock_guard<std::mutex> lock(callbackMutex_);
    debugger_ = std::move(dbg);
}

void Interpreter::setDebugMode(bool enabled) {
    debugMode_ = enabled;
}

Environment* Interpreter::currentEnvironment() const {
    return currentEnv_.get();
}

std::shared_ptr<Environment> Interpreter::currentEnvironmentShared() const {
    // AUDIT-P1 fix: 返回 shared_ptr 副本，延长 Environment 生命周期，
    // 供跨线程调用方（如 DebugCoordinator 的 variableCallback）安全持有。
    return currentEnv_;
}

const std::vector<CallFrame>& Interpreter::getCallStack() const {
    return callStack_;
}

std::vector<CallFrame> Interpreter::getCallStackSnapshot() const {
    return callStack_;
}

Value Interpreter::evaluateExpr(ASTNode* node) {
    if (!node)
        return Value::nullValue();
    // RAII 守卫：确保异常时也能恢复 debugMode_
    // QT-R-02 fix: debugMode_ 是 atomic<bool>，guard 使用 atomic 引用
    struct DebugModeGuard {
        std::atomic<bool>& ref;
        bool saved;
        DebugModeGuard(std::atomic<bool>& r) : ref(r), saved(r.load()) { ref.store(false); }
        ~DebugModeGuard() { ref.store(saved); }
    } guard(debugMode_);
    // A1 fix: accept 返回 void，结果通过 lastValue_ 传递。
    // A1 bug fix: 预先清空 lastValue_，避免未覆盖的节点类型返回陈旧值（对齐 Formatter F-P2-6）
    lastValue_ = Value::nullValue();
    node->accept(*this);
    return std::move(lastValue_);
}

// GUI-03 fix + DBG-A fix + #1 fix: 安全条件断点求值
// 沙箱化：求值前快照整个作用域链的变量绑定 + 绑定实例字段，求值后恢复，
// 防止条件中的赋值（x = 5）、声明（var y = ...）、实例字段写（this.f = v）
// 修改程序状态。
//
// AUDIT-SANDBOX-DEEP fix: 原实现仅浅拷贝 Value（递增 refCount），依赖 COW ensureUnique
// 在变异时触发深拷贝。但对嵌套容器（如 obj["arr"].push(1)），若字典索引访问未触发
// dict 的 COW（例如通过 const 引用链获取内部数组引用），数组 refCount 仍为 1，
// push 会原地修改共享 ArrayData，污染程序状态。
// 修复：快照时对所有 array/dict Value 递归深拷贝，确保沙箱内操作的是独立副本。
// 恢复时直接用深拷贝副本替换，无论条件是否变异容器都能正确还原。
// AUDIT-P0-ROUND49 fix: 添加环检测（visited 集合）和深度保护，防止自环容器
// （如 a.push(a)）触发无限递归栈溢出崩溃。对齐 Value::cloneImpl 的环检测模式。
namespace {
// 环检测深度上限（对齐 Value::MAX_CLONE_DEPTH）
constexpr int SANDBOX_CLONE_MAX_DEPTH = 256;

Value deepCloneForSandboxImpl(const Value& v, int depth, std::unordered_set<const void*>& visited) {
    // 深度保护：超限时返回浅拷贝（保留原引用），避免栈溢出
    if (depth >= SANDBOX_CLONE_MAX_DEPTH) {
        return v;
    }
    if (v.isArray()) {
        const void* ptr = v.gcRootPtr();
        // 环检测：遇到已访问节点返回浅拷贝（打断 back-edge）
        if (auto [it, inserted] = visited.insert(ptr); !inserted) {
            return v;
        }
        const auto& arr = v.arrayVal();
        std::vector<Value> newElements;
        newElements.reserve(arr.size());
        for (const auto& elem : arr) {
            newElements.push_back(deepCloneForSandboxImpl(elem, depth + 1, visited));
        }
        return Value(std::move(newElements));
    }
    if (v.isDict()) {
        const void* ptr = v.gcRootPtr();
        if (auto [it, inserted] = visited.insert(ptr); !inserted) {
            return v;
        }
        const auto& entries = v.dictVal();
        std::unordered_map<std::string, Value> newEntries;
        newEntries.reserve(entries.size());
        for (const auto& [k, val] : entries) {
            newEntries[k] = deepCloneForSandboxImpl(val, depth + 1, visited);
        }
        return Value(std::move(newEntries));
    }
    // BUG-INTP-1 fix: instance 递归深拷贝 fields。原实现浅拷贝（共享嵌套 InstanceData），
    // 条件中的 this.inner.field = 99 会通过 boundInstance_ 链找到原嵌套 InstanceData
    // 原地修改，SandboxGuard 析构恢复外层 fields map 但内层实例仍被污染。
    // 注意：深拷贝破坏引用语义（条件前 var x = this.inner，沙箱后 this.inner 指向新副本），
    // 这是沙箱隔离的固有矛盾，与 array/dict 深拷贝行为一致，已在文档中标注为已知行为。
    if (v.isInstance()) {
        const void* ptr = v.gcRootPtr();
        if (auto [it, inserted] = visited.insert(ptr); !inserted) {
            return v;
        }
        Value newInst = Value::makeInstance(v.className());
        const auto& oldFields = v.fields();
        auto& newFields = newInst.fields();
        newFields.reserve(oldFields.size());
        for (const auto& [k, val] : oldFields) {
            newFields.emplace(k, deepCloneForSandboxImpl(val, depth + 1, visited));
        }
        return newInst;
    }
    // AUDIT-P2-CORRECT fix: 闭包的 capturedVars 需要 COW detach + 递归深拷贝，
    // 隔离沙箱内 writeBackCapturedVars 的副作用（writeBack 通过 const_cast 绕过 COW
    // 直接修改原始 capturedVars map）。body 只读可共享。
    // closureEnv（weak_ptr）不能共享——沙箱内调用闭包修改闭包变量会影响外层环境。
    // 从 capturedVars 重建 closureEnv，断开与外层环境的共享。
    if (v.isClosure()) {
        const void* ptr = v.gcRootPtr();
        if (auto [it, inserted] = visited.insert(ptr); !inserted) {
            return v;
        }
        Value newClosure = v; // 浅拷贝（共享 body/closureEnv，refCount=2）
        // 非 const capturedVars() 触发 ensureUnique<ClosureData>，refCount>1 时创建独立副本
        auto& captured = newClosure.capturedVars();
        for (auto& [k, val] : captured) {
            val = deepCloneForSandboxImpl(val, depth + 1, visited); // 递归深拷贝容器值
        }
        // AUDIT-P2-CORRECT fix: 从 capturedVars 重建 closureEnv，断开与外层环境的共享。
        // 沙箱内调用闭包时，callClosureValue/callNamedFunction 使用 closureEnv 作为父级环境，
        // 若仍指向外层 Environment，闭包变量修改会通过 Environment::set 影响外层。
        // 重建为独立 Environment（parent=nullptr），仅含 capturedVars 的副本，
        // writeBackCapturedVars 将修改写回沙箱副本的 capturedVars，不影响外层。
        auto origEnv = v.closureEnv();
        if (origEnv) {
            auto newEnv = std::make_shared<Environment>(nullptr);
            for (const auto& kv : captured) {
                newEnv->define(kv.first, kv.second);
            }
            newClosure.setClosureEnv(newEnv);
        }
        return newClosure;
    }
    // 非容器类型（int/float/bool/null/string）：
    // - 标量：值语义，拷贝即独立
    // - string：不可变（BuiltinMethods 的 replace/substr 返回新串而非原地修改）
    return v;
}

Value deepCloneForSandbox(const Value& v) {
    std::unordered_set<const void*> visited;
    return deepCloneForSandboxImpl(v, 0, visited);
}
} // anonymous namespace

Value Interpreter::evaluateCondition(ASTNode* node) {
    if (!node)
        return Value::nullValue();

    // AUDIT-P1-CORRECT fix: 开启条件断点求值步数计数（非 0 表示"正在条件求值"）。
    // evaluate 中检查此计数器，超过 MAX_CONDITION_STEPS 则抛 RuntimeError（while(true){} 等无限循环）。
    // 非条件求值时 evaluationStepCount_ 为 0，evaluate 不计数。
    evaluationStepCount_ = 1;

    // #1 fix: 快照作用域链所有变量绑定（深拷贝容器）
    struct EnvSnapshot {
        Environment* env;
        std::unordered_map<std::string, Value> variables;
    };
    std::vector<EnvSnapshot> envSnaps;
    // #1 fix: 快照绑定实例字段（防止 this.field = val）
    // boundInstance_ 沿链继承（同一实例可能出现多次），用指针去重
    // BUG-INTP-2 fix: 同时记录原始 InstanceData 指针，析构时比较。
    // 若条件中 this 被重赋值为另一实例（仍 isInstance==true），inst 指向的
    // Value 内容已变（InstanceData 指针不同），不应把旧字段快照写入新实例。
    // AUDIT-P2-CORRECT fix: 存储 Environment* 以便析构时重新获取 inst 指针，
    // 避免条件求值期间 variables map rehash 导致 inst 悬垂。
    struct InstSnapEntry {
        Environment* env;
        const void* gcRoot;
        std::unordered_map<std::string, Value> fields;
    };
    std::map<Value*, InstSnapEntry> instSnaps;
    Environment* snapEnv = currentEnv_.get();
    while (snapEnv) {
        auto locals = snapEnv->snapshotLocalVariables();
        // AUDIT-SANDBOX-DEEP: 深拷贝容器值，防止条件中的容器变异污染程序状态
        for (auto& [k, v] : locals) {
            v = deepCloneForSandbox(v);
        }
        envSnaps.push_back({snapEnv, std::move(locals)});
        Value* inst = snapEnv->getBoundInstance();
        if (inst && inst->isInstance() && instSnaps.find(inst) == instSnaps.end()) {
            // AUDIT-BUG-I2 fix: 用 const 访问避免触发 COW 分离——非 const fields()
            // 在 refCount>1 时会 ensureUnique，使 inst 与其他共享变量分离，
            // 破坏后续程序中 b=a 后修改 a.field b 可见的引用语义。
            auto fields = static_cast<const Value*>(inst)->fields();
            // AUDIT-SANDBOX-DEEP: 深拷贝实例字段中的容器值
            for (auto& [k, v] : fields) {
                v = deepCloneForSandbox(v);
            }
            instSnaps[inst] = {snapEnv, inst->gcRootPtr(), std::move(fields)};
        }
        snapEnv = snapEnv->parent.get();
    }

    // P1-1 fix: 在执行条件表达式之前，将每个 Environment 的 variables 替换为
    // 深拷贝版本。原实现仅用快照在析构时恢复变量绑定，但条件中的容器变异
    // （arr.push/dict.set/实例字段修改）作用在原始堆对象上，restoreLocalVariables
    // 只替换绑定不恢复容器内容。通过在求值前替换为深拷贝副本，条件求值完全在
    // 副本上操作，原始容器不受影响。SandboxGuard 析构时再次 restoreLocalVariables
    // 恢复原始值。
    // 注：envSnaps 中的深拷贝 locals 同时用于恢复，此处将其写入环境作为求值副本。
    for (auto& snap : envSnaps) {
        snap.env->restoreLocalVariables(snap.variables);
    }
    // 实例字段也需在求值前替换为深拷贝
    for (auto& [oldInst, snap] : instSnaps) {
        Value* inst = snap.env->getBoundInstance();
        if (inst && inst->isInstance() && inst->gcRootPtr() == snap.gcRoot) {
            inst->fields() = snap.fields;
        }
    }

    auto savedCallStack = callStack_;
    auto savedClassCtx = classContextStack_;
    auto savedEnv = currentEnv_;
    int savedDepth = recursionDepth_;
    bool savedDebugMode = debugMode_;
    debugMode_ = false;
    // BUG-DBG-7 fix: 沙箱求值前重置 recursionDepth_ 为 0。
    // 原实现仅保存不重置，条件中调用函数会从当前深度（可能已接近 MAX_RECURSION）
    // 起递增，触发"递归深度超限"假阳性。条件求值应视为独立调用栈上下文。
    recursionDepth_ = 0;
    // BUG-DBG-8 fix: 沙箱未保存/恢复 funRegistry_/classRegistry_ 及其代数计数器。
    // 条件中 `fun foo() {} true` 或 `class X {} true` 会污染注册表，使主程序
    // 后续 FunCall/ClassNew 命中沙箱新增的函数/类。代数不恢复还会使缓存失效逻辑错乱。
    auto savedFunRegistry = funRegistry_;
    auto savedFunRegistryGen = funRegistryGen_;
    auto savedClassRegistry = classRegistry_;
    auto savedClassRegistryGen = classRegistryGen_;
    // BUG-REPL-2 fix: 沙箱未保存/恢复 lastValue_。
    // 沙箱内 node->accept 会覆盖 lastValue_，主程序表达式求值中间状态丢失
    // （若条件断点在表达式子节点求值过程中触发，主程序的 lastValue_ 会被污染）。
    Value savedLastValue = lastValue_;
    // AUDIT-P2 fix: 沙箱未保存/恢复 loopFlow_ 和模块相关状态。
    // 条件中调用含 import/export 的函数会污染 moduleCache_/moduleExports_/
    // moduleMtimes_/exportedNames_，使主程序后续 import 命中沙箱注入的缓存。
    auto savedLoopFlow = loopFlow_;
    auto savedModuleCache = moduleCache_;
    auto savedModuleExports = moduleExports_;
    auto savedModuleMtimes = moduleMtimes_;
    auto savedExportedNames = exportedNames_;

    // RA-A fix: RAII 守卫统一管理沙箱状态恢复（实例字段 + 局部变量 + 调用栈/环境/深度/调试模式），
    // 消除原 catch(...) + throw; 的 rethrow。正常路径和异常路径恢复逻辑完全一致。
    struct SandboxGuard {
        Interpreter& interp;
        std::map<Value*, InstSnapEntry>& instSnaps;
        std::vector<EnvSnapshot>& envSnaps;
        std::vector<CallFrame> savedCallStack;
        std::vector<std::string> savedClassCtx;
        std::shared_ptr<Environment> savedEnv;
        int savedDepth;
        bool savedDebugMode;
        std::unordered_map<std::string, std::shared_ptr<FunDecl>> savedFunRegistry;
        int savedFunRegistryGen;
        std::unordered_map<std::string, ClassInfo> savedClassRegistry;
        int savedClassRegistryGen;
        Value savedLastValue;
        LoopFlow savedLoopFlow;
        std::unordered_map<std::string, std::shared_ptr<Environment>> savedModuleCache;
        std::unordered_map<std::string, std::unordered_set<std::string>> savedModuleExports;
        std::unordered_map<std::string, int64_t> savedModuleMtimes;
        std::unordered_set<std::string> savedExportedNames;
        ~SandboxGuard() {
            // H2 fix: 必须先恢复实例字段，再恢复局部变量。
            // inst 指针指向旧 variables["this"] 条目，restoreLocalVariables
            // 整表替换 variables 会使 inst 悬垂。先恢复字段可保证 inst 仍有效。
            // restoreLocalVariables 内部会重新锚定 boundInstance_ 到新 map。
            // BUG-INT-3 fix: 条件可能重新赋值 this（如 this = 5），使 inst 指向的
            // Value 不再是实例。调用 fields() 会 std::abort。跳过非实例的 inst，
            // 后续 restoreLocalVariables 会恢复 variables["this"] 到原始实例。
            // BUG-INTP-2 fix: 即使 inst 仍是实例，也可能 this 被重赋值为另一实例
            // （InstanceData 指针不同）。比较 gcRootPtr()，不匹配则跳过字段恢复，
            // 避免把旧 A 字段快照写入新 B 实例污染新对象。
            // AUDIT-P2-CORRECT fix: 不使用快照时的 inst 指针（可能因 variables rehash 悬垂），
            // 改为从 snap.env 重新获取 boundInstance_，确保指针有效。
            for (auto& [oldInst, snap] : instSnaps) {
                Value* inst = snap.env->getBoundInstance();
                if (inst && inst->isInstance() && inst->gcRootPtr() == snap.gcRoot) {
                    inst->fields() = snap.fields;
                }
            }
            // #1 fix: 恢复变量绑定（撤销条件中的赋值/声明副作用）
            for (auto& snap : envSnaps) {
                snap.env->restoreLocalVariables(snap.variables);
            }
            interp.callStack_ = std::move(savedCallStack);
            interp.classContextStack_ = std::move(savedClassCtx);
            interp.currentEnv_ = savedEnv;
            interp.recursionDepth_ = savedDepth;
            interp.debugMode_ = savedDebugMode;
            // BUG-DBG-8 fix: 恢复函数/类注册表及代数
            interp.funRegistry_ = std::move(savedFunRegistry);
            interp.funRegistryGen_ = savedFunRegistryGen;
            interp.classRegistry_ = std::move(savedClassRegistry);
            interp.classRegistryGen_ = savedClassRegistryGen;
            // BUG-REPL-2 fix: 恢复 lastValue_
            interp.lastValue_ = std::move(savedLastValue);
            // AUDIT-P2 fix: 恢复 loopFlow_ 和模块相关状态
            interp.loopFlow_ = savedLoopFlow;
            interp.moduleCache_ = std::move(savedModuleCache);
            interp.moduleExports_ = std::move(savedModuleExports);
            interp.moduleMtimes_ = std::move(savedModuleMtimes);
            interp.exportedNames_ = std::move(savedExportedNames);
            // AUDIT-P1-ROUND49 fix: 恢复 evaluationStepCount_ 为 0。
            // evaluateCondition 入口设 evaluationStepCount_=1，evaluate 中递增计数
            // 防止条件求值无限循环。SandboxGuard 析构恢复 16 种状态但遗漏此字段，
            // 返回后 evaluationStepCount_ 仍 > 0，后续正常执行每次 evaluate() 递增计数，
            // 累计超 MAX_CONDITION_STEPS 后抛虚假 RuntimeError，冻结主程序。
            interp.evaluationStepCount_ = 0;
        }
    } sandboxGuard{*this,
                   instSnaps,
                   envSnaps,
                   std::move(savedCallStack),
                   std::move(savedClassCtx),
                   savedEnv,
                   savedDepth,
                   savedDebugMode,
                   std::move(savedFunRegistry),
                   savedFunRegistryGen,
                   std::move(savedClassRegistry),
                   savedClassRegistryGen,
                   std::move(savedLastValue),
                   savedLoopFlow,
                   std::move(savedModuleCache),
                   std::move(savedModuleExports),
                   std::move(savedModuleMtimes),
                   std::move(savedExportedNames)};

    lastValue_ = Value::nullValue();
    node->accept(*this);
    Value result = lastValue_; // BUG-REPL-2 fix: 用拷贝而非 move，sandboxGuard 析构会恢复 lastValue_
    // RA-A fix: sandboxGuard 析构会统一恢复所有状态，无需手动还原
    return result;
}

// ---- 辅助方法 ----

Value Interpreter::evaluate(ASTNode* node) {
    if (!node)
        return Value::nullValue();
    // AUDIT-P1-CORRECT fix: 条件断点求值步数上限，防止无限循环冻结 UI。
    // evaluationStepCount_ > 0 表示正在条件求值（evaluateCondition 开头设为 1）。
    // 非条件求值时 evaluationStepCount_ 为 0，短路求值不递增不计数。
    // 超限时抛 RuntimeError，被 DebugCoordinator/IdeController 的 catch(std::exception) 捕获，
    // 返回 false（条件不满足），避免 while(true){} 等无限循环永久冻结主线程。
    if (evaluationStepCount_ > 0 && ++evaluationStepCount_ > MAX_CONDITION_STEPS) {
        runtimeError("条件断点求值步数超过限制 (" + std::to_string(MAX_CONDITION_STEPS) + ")，可能存在无限循环", 0, 0);
    }
    // AUDIT-P1.2 fix: 条件求值期间检查 stopRequested_，提供比步数上限更快的响应。
    // checkBreak 中的 stopRequested_ 检查仅在语句节点入口触发，而 evaluate 是所有
    // 表达式求值的入口（包括循环条件、二元运算子表达式等），在此检查可实现
    // 表达式级别的细粒度中止。用户点击停止后，requestStop() 设置 stopRequested_=true，
    // 条件求值中下一次 evaluate 调用即抛 DebugStopException，无需等待步数上限。
    if (evaluationStepCount_ > 0 && stopRequested_.load(std::memory_order_relaxed)) {
        throw DebugStopException();
    }
    // A1 fix: accept 返回 void，结果通过 lastValue_ 传递。
    // A1 bug fix: 预先清空 lastValue_，避免未覆盖的节点类型返回陈旧值
    lastValue_ = Value::nullValue();
    node->accept(*this);
    return std::move(lastValue_);
}

void Interpreter::checkBreak(ASTNode* node) {
    // REPL 协作中止：closeEvent 超时路径设置 stopRequested_，
    // 在每个语句节点检查，抛异常中断执行（被异步 lambda 的 catch 捕获）
    // RA-C fix: 改抛 DebugStopException（原 std::runtime_error）——类型更明确，
    // 顶层 catch 可区分用户错误（RuntimeError）与中止信号（DebugStopException），
    // 中止信号静默处理（stoppedByUser），不再误报为 genericError。
    if (stopRequested_.load(std::memory_order_relaxed)) {
        throw DebugStopException();
    }
    // BUG-DBG-1 fix: 更新调用栈顶帧行号为当前执行行号。
    // 原实现仅压栈时记录调用点行号（node.line of caller），帧压栈后从不更新，
    // 导致暂停时顶帧显示调用点行而非当前执行行。VM 路径通过 frame.ip 读取当前行，
    // 此处对齐 VM 行为，使 Interpreter 调用栈顶帧也显示当前执行行。
    if (node && node->line > 0 && !callStack_.empty()) {
        callStack_.back().line = node->line;
    }
    if (debugMode_ && debugger_) {
        // 同步调用深度到调试控制器（Step Over 依赖此值判断是否进入函数）
        debugger_->setCurrentDepth(recursionDepth_);
        debugger_->checkBreak(node);
    }
}

void Interpreter::output(const std::string& text) {
    // A6 fix: 加锁拷贝 callback 后解锁调用，避免持锁回调导致死锁
    std::function<void(const std::string&)> cb;
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        cb = outputCallback_;
    }
    if (cb)
        cb(text);
}

void Interpreter::runtimeError(const std::string& msg, int line, int col) {
    // P1-9 fix: 使用 ErrorFormat::formatWithLocation 替代 std::to_string + operator+，
    // 内部用 std::to_chars 写入栈缓冲区，零堆分配。
    Logger::Error(ErrorFormat::formatWithLocation(msg, line, col), "Interpreter");
    throw RuntimeError(msg, line, col);
}

// ---- 数值二元运算 ----

// P1-2 fix: 4 个比较运算（LT/GT/LTE/GTE）共用模板，消除重复样板。
// 支持字符串字典序比较与数值比较，类型不匹配时抛 RuntimeError。
template <typename Cmp> Value Interpreter::compareNumericOrString(BinaryOp& node, Cmp cmp) {
    Value left = evaluate(node.left.get());
    Value right = evaluate(node.right.get());

    // C12 fix: 调用共享比较逻辑，消除与 VM 的重复实现
    using namespace NumericOps;
    // 根据 cmp 仿函数确定比较类型（std::less→Less, std::greater→Greater, 等）
    CompareOp op;
    if (std::is_same_v<Cmp, std::less<>>)
        op = CompareOp::Less;
    else if (std::is_same_v<Cmp, std::greater<>>)
        op = CompareOp::Greater;
    else if (std::is_same_v<Cmp, std::less_equal<>>)
        op = CompareOp::LessEqual;
    else
        op = CompareOp::GreaterEqual;

    // 字符串字典序比较
    if (left.isString() && right.isString()) {
        auto r = computeCompare(op, true, left.stringVal(), true, right.stringVal(), 0, 0);
        return Value(r.value);
    }
    // 数值比较
    if (!left.isNumber() || !right.isNumber())
        runtimeError("比较运算需要数值或字符串类型", node.line, node.column);
    auto r = computeCompare(op, false, {}, false, {}, left.toDouble(), right.toDouble());
    return Value(r.value);
}

Value Interpreter::numericBinaryOp(BinOpType opType, Value left, Value right, int line, int col) {
    // 字符串拼接（仅加法）
    if (opType == BinOpType::BIN_ADD) {
        if (left.isString() && right.isString()) {
            // PERF-06 fix: 若 left 独占 StringData（典型场景：左操作数是临时值，
            // 如 `((s + "x") + "y")` 中内层 `s + "x"` 的结果），原地 append
            // 避免分配新 string + 拷贝。链式拼接 `a + b + c + d` 由 amortized O(n²) 降为 O(n)。
            if (std::string* lhs = left.tryGetMutableString()) {
                const std::string& rhs = right.stringVal();
                lhs->reserve(lhs->size() + rhs.size());
                *lhs += rhs;
                return std::move(left);
            }
            return Value(left.stringVal() + right.stringVal());
        }
        if (left.isString() || right.isString()) {
            // 一侧为字符串、一侧为其他类型 → 用 toString() 双向转换后拼接
            // PERF-06: 同样优先 left 独占时原地 append
            if (std::string* lhs = left.tryGetMutableString()) {
                std::string rhs = right.toString();
                lhs->reserve(lhs->size() + rhs.size());
                *lhs += rhs;
                return std::move(left);
            }
            return Value(left.toString() + right.toString());
        }
    }

    // 非数值类型检查（字符串拼接已在上面处理）
    if (!left.isNumber() || !right.isNumber()) {
        runtimeError("算术运算需要数值类型", line, col);
    }

    // C12 fix: 调用共享算术运算逻辑，消除与 VM 的重复实现
    using namespace NumericOps;
    ArithOp op;
    switch (opType) {
    case BinOpType::BIN_ADD:
        op = ArithOp::Add;
        break;
    case BinOpType::BIN_SUB:
        op = ArithOp::Sub;
        break;
    case BinOpType::BIN_MUL:
        op = ArithOp::Mul;
        break;
    case BinOpType::BIN_DIV:
        op = ArithOp::Div;
        break;
    case BinOpType::BIN_MOD:
        op = ArithOp::Mod;
        break;
    default:
        runtimeError("不支持的算术运算符", line, col);
    }

    ArithResult r = computeArith(op, left.isInt(), left.isInt() ? left.intVal() : 0, left.toDouble(), right.isInt(),
                                 right.isInt() ? right.intVal() : 0, right.toDouble());
    switch (r.status) {
    case ArithStatus::DivByZero:
        runtimeError("除零错误", line, col);
    case ArithStatus::IntOverflow:
        runtimeError("整数运算溢出", line, col);
    case ArithStatus::NotNumeric:
        runtimeError("算术运算需要数值类型", line, col);
    case ArithStatus::OK:
        return r.isIntResult ? Value(r.intVal) : Value(r.floatVal);
    // Bug-6 同型修复：ArithStatus 是封闭枚举，落空表示内部错误。
    // 原代码 return Value::nullValue() 会让上层静默拿到 null 结果，
    // 改为抛出明确错误（与 VM.cpp:389 / RegisterVM.cpp:421 对齐）。
    default:
        runtimeError("内部错误: 未知算术状态", line, col);
    }
}

// ---- 类型检查辅助方法 ----

bool Interpreter::typeMatch(const Value& val, const std::string& annotation) const {
    if (annotation.empty())
        return true;
    // AUDIT-P2-CORRECT fix: 可选类型 T?（后缀 ?）
    // 放在最前：T? 的 base 类型可能是任何类型（包括 dict[K:V]、fun()、数组等），
    // 需先剥离 ? 后递归匹配。null 兼容可选类型。
    if (annotation.size() >= 2 && annotation.back() == '?') {
        if (val.isNull())
            return true; // null 兼容可选类型
        return typeMatch(val, annotation.substr(0, annotation.size() - 1));
    }
    // 2026-06-29 fix: null 兼容所有类型注解（必须在所有具体类型检查之前）
    // 理由：null 作为"无值"标记应可赋给任何类型（如 string s = null, int x = null），
    // 与常见动态语言惯例一致。M7 fix 使 null 仅匹配 "null" 注解过于严格。
    // 原回退将 null 检查放在函数末尾，但 INT/FLOAT/BOOL/STRING 的早返回使 null 永远
    // 到不了末尾检查，导致 int a = null 抛错。此处移到最前修复。
    if (val.isNull())
        return true;
    // AUDIT-P2-CORRECT fix: dict[K:V] 泛型字典类型注解
    // 格式 "dict[keyType:valType]"，key 总是 string，valType 递归 typeMatch 每个值。
    if (annotation.size() >= 7 && annotation.substr(0, 5) == "dict[" && annotation.back() == ']') {
        if (!val.isDict())
            return false;
        std::string inner = annotation.substr(5, annotation.size() - 6); // 如 "string:int"
        size_t colonPos = inner.find(':');
        if (colonPos == std::string::npos)
            return false;
        std::string keyType = inner.substr(0, colonPos);
        std::string valType = inner.substr(colonPos + 1);
        for (const auto& kv : val.dictVal()) {
            if (keyType != "string")
                return false; // 字典 key 总是 string
            if (!typeMatch(kv.second, valType))
                return false;
        }
        return true;
    }
    // AUDIT-P2-CORRECT fix: 函数类型 fun(params):ret
    // 闭包或 null（null 已在上方返回 true）匹配函数类型注解。
    if (annotation.size() >= 5 && annotation.substr(0, 4) == "fun(") {
        return val.isClosure() || val.isNull();
    }
    // P2-8 fix: 使用 TypeName 常量替代硬编码字符串
    if (annotation == TypeName::INT)
        return val.isInt();
    if (annotation == TypeName::FLOAT)
        return val.isFloat() || val.isInt();
    if (annotation == TypeName::BOOL)
        return val.isBool();
    if (annotation == TypeName::STRING)
        return val.isString();
    if (annotation == TypeName::ARRAY)
        return val.isArray();
    if (annotation == TypeName::DICT)
        return val.isDict();
    // 数组元素类型注解，如 "int[]", "int[][]", "int[][][]"
    // AUDIT-P2-CORRECT fix: 支持多维数组类型注解。原实现用 annotation[size-2]=='['
    // 仅匹配单维 "int[]"，对 "int[][]"（末尾两字符 "]]"）不匹配导致报错。
    // 改用 substr 后缀比较 "[]"，剥离末尾 "[]" 后递归 typeMatch 每个元素，
    // 天然支持多维（int[][] → 剥离得 int[] → 递归剥离得 int）。
    if (annotation.size() >= 2 && annotation.substr(annotation.size() - 2) == "[]") {
        if (!val.isArray())
            return false;
        std::string elemType = annotation.substr(0, annotation.size() - 2);
        for (const auto& elem : val.arrayVal()) {
            if (!typeMatch(elem, elemType))
                return false;
        }
        return true;
    }
    if (val.isInstance()) {
        // H1 fix: 遍历继承链，使子类实例可以匹配父类类型注解
        auto classIt = classRegistry_.find(val.className());
        int depth = 0;
        while (classIt != classRegistry_.end()) {
            if (++depth > MAX_INHERITANCE_DEPTH)
                break;
            if (classIt->second.name == annotation)
                return true;
            if (!classIt->second.superClassName.empty()) {
                classIt = classRegistry_.find(classIt->second.superClassName);
            } else {
                break;
            }
        }
    }
    return false;
}

// checkType 已模板化移至 Interpreter.h（P20 fix）

const std::string* Interpreter::findTypeAnnotation(const std::string& varName) const {
    // B2 fix: 沿作用域链查找类型注解（不再使用 flat map）
    return currentEnv_->getTypeAnnotation(varName);
}

// ---- writeBack 辅助方法 ----

Interpreter::ChainInfo Interpreter::collectAndEvaluateChain(ASTNode* objectNode, bool errorOnNonVarRef, int line,
                                                            int col) {
    ChainInfo info;
    // PERF-09 fix: 预分配 chain 容量，避免链式访问深度 >5 时多次 realloc。
    // 绝大多数链式访问深度 ≤ 8（如 a.b.c[i].d），预分配 8 足以覆盖常见场景。
    info.chain.reserve(8);
    info.vals.reserve(8);
    info.idxs.reserve(8);
    ASTNode* cur = objectNode;
    while (cur->nodeType == NodeType::NODE_MEMBER_ACCESS || cur->nodeType == NodeType::NODE_INDEX_ACCESS) {
        info.chain.push_back(cur);
        if (cur->nodeType == NodeType::NODE_MEMBER_ACCESS) {
            cur = static_cast<MemberAccess*>(cur)->object.get();
        } else {
            cur = static_cast<IndexAccess*>(cur)->object.get();
        }
    }
    info.chain.push_back(cur); // 最外层 VarRef

    int n = static_cast<int>(info.chain.size());

    // 从外到内逐级求值，收集每级的值和索引
    info.vals.resize(n);
    info.idxs.resize(n);

    if (info.chain[n - 1]->nodeType != NodeType::NODE_VAR_REF) {
        if (errorOnNonVarRef) {
            runtimeError("赋值目标必须是变量引用", line, col);
        }
        info.varRef = nullptr;
        return info;
    }
    info.varRef = static_cast<VarRef*>(info.chain[n - 1]);
    const Value* baseVal = currentEnv_->get(info.varRef->name);
    if (!baseVal) {
        runtimeError("未定义的变量: " + info.varRef->name, line, col);
    }
    info.vals[n - 1] = *baseVal; // #18 fix: 明确报错而非静默nullValue

    for (int i = n - 2; i >= 0; i--) {
        ASTNode* nd = info.chain[i];
        const Value& parent = info.vals[i + 1];
        if (nd->nodeType == NodeType::NODE_MEMBER_ACCESS) {
            auto* ma = static_cast<MemberAccess*>(nd);
            if (parent.isInstance()) {
                // Perf-Finding: 缓存 const 引用避免 fields() 二次调用与同键二次 hash 查找
                const auto& flds = parent.fields();
                auto it = flds.find(ma->fieldName);
                info.vals[i] = (it != flds.end()) ? it->second : Value::nullValue();
            } else if (parent.isDict()) {
                const auto& entries = parent.dictVal();
                auto it = entries.find(ma->fieldName);
                info.vals[i] = (it != entries.end()) ? it->second : Value::nullValue();
            } else {
                runtimeError("该类型不支持成员访问", line, col);
            }
        } else if (nd->nodeType == NodeType::NODE_INDEX_ACCESS) {
            auto* ia = static_cast<IndexAccess*>(nd);
            info.idxs[i] = evaluate(ia->index.get());
            const Value& indexVal = info.idxs[i];
            if (parent.isArray() && indexVal.isInt()) {
                if (indexVal.intVal() < 0 ||
                    static_cast<size_t>(indexVal.intVal()) >= std::as_const(parent).arrayVal().size())
                    runtimeError(ErrorFormat::format("数组索引越界: %lld, 有效范围 [0, %zu)",
                                                     static_cast<long long>(indexVal.intVal()),
                                                     std::as_const(parent).arrayVal().size()),
                                 line, col);
                info.vals[i] = std::as_const(parent).arrayVal()[indexVal.intVal()];
            } else if (parent.isDict() && indexVal.isString()) {
                auto it = std::as_const(parent).dictVal().find(indexVal.stringVal());
                info.vals[i] = (it != std::as_const(parent).dictVal().end()) ? it->second : Value::nullValue();
            } else {
                runtimeError("该类型不支持索引访问", line, col);
            }
        }
    }

    return info;
}

void Interpreter::writeBackChain(ChainInfo& info, Value innermost, int line, int col) {
    int n = static_cast<int>(info.chain.size());
    Value currentVal = std::move(innermost);
    for (int i = 0; i < n - 1; i++) {
        ASTNode* nd = info.chain[i];
        Value parentVal = std::move(info.vals[i + 1]); // #19: move避免深拷贝
        if (nd->nodeType == NodeType::NODE_MEMBER_ACCESS) {
            auto* ma = static_cast<MemberAccess*>(nd);
            if (parentVal.isInstance()) {
                // S1 fix: 优先使用 tryGetMutableFields 跳过 COW 深拷贝
                if (auto* fields = parentVal.tryGetMutableFields()) {
                    (*fields)[ma->fieldName] = currentVal;
                } else {
                    parentVal.fields()[ma->fieldName] = currentVal;
                }
            } else if (parentVal.isDict()) {
                // S1 fix: 优先使用 tryGetMutableDict 跳过 COW 深拷贝
                if (auto* entries = parentVal.tryGetMutableDict()) {
                    (*entries)[ma->fieldName] = currentVal;
                } else {
                    parentVal.dictVal()[ma->fieldName] = currentVal;
                }
            } else {
                runtimeError("该类型不支持成员赋值", line, col);
            }
        } else if (nd->nodeType == NodeType::NODE_INDEX_ACCESS) {
            const Value& indexVal = info.idxs[i];
            if (parentVal.isArray() && indexVal.isInt()) {
                if (indexVal.intVal() < 0 || static_cast<size_t>(indexVal.intVal()) >= parentVal.arrayVal().size())
                    runtimeError(ErrorFormat::format("数组索引越界: %lld, 有效范围 [0, %zu)",
                                                     static_cast<long long>(indexVal.intVal()),
                                                     parentVal.arrayVal().size()),
                                 line, col);
                // S1 fix: 优先使用 tryGetMutableArray 跳过 COW 深拷贝
                if (auto* arr = parentVal.tryGetMutableArray()) {
                    (*arr)[indexVal.intVal()] = currentVal;
                } else {
                    parentVal.arrayVal()[indexVal.intVal()] = currentVal;
                }
            } else if (parentVal.isDict() && indexVal.isString()) {
                // S1 fix: 优先使用 tryGetMutableDict 跳过 COW 深拷贝
                if (auto* entries = parentVal.tryGetMutableDict()) {
                    (*entries)[indexVal.stringVal()] = currentVal;
                } else {
                    parentVal.dictVal()[indexVal.stringVal()] = currentVal;
                }
            } else {
                runtimeError("该类型不支持索引赋值", line, col);
            }
        }
        currentVal = std::move(parentVal); // #19: move
    }

    currentEnv_->set(info.varRef->name, std::move(currentVal)); // #19: move
}

// ---- writeBack 写回左值 ----
// 链式求值方式：从最外层到最内层逐级求值（每级仅一次），在最内层执行赋值，
// 再从内到外逐级写回，避免对含副作用的子表达式重复求值。

Value Interpreter::writeBack(ASTNode* objectNode, bool isIndexAssign, ASTNode* indexNode, const std::string& fieldName,
                             ASTNode* valueNode, int line, int col) {
    ChainInfo info = collectAndEvaluateChain(objectNode, true, line, col);

    // 链式求值完成，现在按左到右顺序求值 index 和 value
    Value idx;
    if (isIndexAssign && indexNode) {
        idx = evaluate(indexNode);
    }
    Value val;
    if (valueNode) {
        val = evaluate(valueNode);
    }

    // 在最内层对象上执行赋值
    Value modifiedObj = std::move(info.vals[0]); // A2: move 而非拷贝，保持 refcount=1 跳过 COW detach
    if (isIndexAssign) {
        if (modifiedObj.isArray() && idx.isInt()) {
            if (idx.intVal() < 0 || static_cast<size_t>(idx.intVal()) >= std::as_const(modifiedObj).arrayVal().size())
                runtimeError(ErrorFormat::format("数组索引越界: %lld, 有效范围 [0, %zu)",
                                                 static_cast<long long>(idx.intVal()),
                                                 std::as_const(modifiedObj).arrayVal().size()),
                             line, col);
            modifiedObj.arrayVal()[idx.intVal()] = val;
        } else if (modifiedObj.isDict() && idx.isString()) {
            modifiedObj.dictVal()[idx.stringVal()] = val;
        } else {
            runtimeError("该类型不支持索引赋值", line, col);
        }
    } else {
        if (modifiedObj.isInstance()) {
            modifiedObj.fields()[fieldName] = val;
        } else if (modifiedObj.isDict()) {
            modifiedObj.dictVal()[fieldName] = val;
        } else {
            runtimeError("该类型不支持成员赋值", line, col);
        }
    }

    writeBackChain(info, std::move(modifiedObj), line, col);
    return val;
}

// ---- writeBack 重载：写回已修改的值 ----
// 用于方法调用等已自行修改对象的场景，链式求值避免重复求值副作用

void Interpreter::writeBack(ASTNode* objectNode, const Value& modifiedValue, int line, int col) {
    ChainInfo info = collectAndEvaluateChain(objectNode, false, line, col);
    if (!info.varRef) {
        // O2 fix: 临时值（如 Foo(1).setX(99)）方法正常执行但修改不写回
        return;
    }
    writeBackChain(info, modifiedValue, line, col); // const ref, 不能move
}

// ---- 16 个原有 visit 方法 ----

void Interpreter::visitBinaryOp(BinaryOp& node) {
    checkBreak(&node);

    // 使用预计算的枚举类型进行快速分发（避免运行时字符串比较）
    switch (node.opType) {
    case BinOpType::BIN_AND: {
        Value left = evaluate(node.left.get());
        if (!left.isTruthy()) {
            lastValue_ = std::move(left);
            return;
        } // M1 fix: 返回原始左值而非 Value(false)
        // AUDIT-ANDOR fix: evaluate() 内部 return std::move(lastValue_) 会将 lastValue_
        // 置为 moved-from (null) 状态。原代码 evaluate(node.right.get()); return; 丢弃了
        // 返回值，导致外层 evaluate 返回 null。必须捕获返回值并赋给 lastValue_。
        lastValue_ = evaluate(node.right.get());
        return;
    }
    case BinOpType::BIN_OR: {
        Value left = evaluate(node.left.get());
        if (left.isTruthy()) {
            lastValue_ = std::move(left);
            return;
        } // M1 fix: 返回原始左值而非 Value(true)
        // AUDIT-ANDOR fix: 同 BIN_AND，必须捕获 evaluate 返回值。
        lastValue_ = evaluate(node.right.get());
        return;
    }
    case BinOpType::BIN_EQ: {
        Value left = evaluate(node.left.get());
        Value right = evaluate(node.right.get());
        lastValue_ = Value(left.equals(right));
        return;
    }
    case BinOpType::BIN_NEQ: {
        Value left = evaluate(node.left.get());
        Value right = evaluate(node.right.get());
        lastValue_ = Value(!left.equals(right));
        return;
    }
    case BinOpType::BIN_LT: {
        // P1-2 fix: 4 个比较运算共用 compareNumericOrString 模板
        lastValue_ = compareNumericOrString(node, std::less<>());
        return;
    }
    case BinOpType::BIN_GT: {
        lastValue_ = compareNumericOrString(node, std::greater<>());
        return;
    }
    case BinOpType::BIN_LTE: {
        lastValue_ = compareNumericOrString(node, std::less_equal<>());
        return;
    }
    case BinOpType::BIN_GTE: {
        lastValue_ = compareNumericOrString(node, std::greater_equal<>());
        return;
    }
    case BinOpType::BIN_ADD:
    case BinOpType::BIN_SUB:
    case BinOpType::BIN_MUL:
    case BinOpType::BIN_DIV:
    case BinOpType::BIN_MOD: {
        Value left = evaluate(node.left.get());
        Value right = evaluate(node.right.get());
        // PERF-06 fix: move 传入使 numericBinaryOp 可检测独占所有权做原地 append
        lastValue_ = numericBinaryOp(node.opType, std::move(left), std::move(right), node.line, node.column);
        return;
    }
    default:
        runtimeError("未知运算符: " + std::string(BinaryOp::opTypeStr(node.opType)), node.line, node.column);
        lastValue_ = Value::nullValue();
        return;
    }
}

void Interpreter::visitUnaryOp(UnaryOp& node) {
    checkBreak(&node);

    Value operand = evaluate(node.operand.get());

    switch (node.opType) {
    case UnaryOp::UnaryOpType::UOP_NEGATE:
        if (operand.isInt()) {
            if (OverflowCheck::negateOverflow(operand.intVal())) {
                runtimeError("整数溢出：无法对最小值取负", node.line, node.column);
                break;
            }
            lastValue_ = Value(-operand.intVal());
            return;
        }
        if (operand.isFloat()) {
            lastValue_ = Value(-operand.floatVal());
            return;
        }
        runtimeError("一元减运算需要数值类型", node.line, node.column);
        break;
    case UnaryOp::UnaryOpType::UOP_NOT:
        lastValue_ = Value(!operand.isTruthy());
        return;
    case UnaryOp::UnaryOpType::UOP_PLUS:
        lastValue_ = std::move(operand);
        return; // 一元 + 恒等操作
    default:
        runtimeError("未知一元运算符: " + std::string(UnaryOp::opTypeStr(node.opType)), node.line, node.column);
        break;
    }
    lastValue_ = Value::nullValue();
    return;
}

void Interpreter::visitNumberLiteral(NumberLiteral& node) {
    checkBreak(&node);
    // 数值字面量：将 AST 节点中保存的数值原样作为求值结果返回（getValue() 按需构造 Value）。
    lastValue_ = node.getValue();
    return; // A1 fix: getValue() 按需构造
}

void Interpreter::visitStringLiteral(StringLiteral& node) {
    checkBreak(&node);
    // 字符串字面量：返回 AST 中保存的字符串值。
    lastValue_ = node.getValue();
    return;
}

// C5 fix: 插值字符串求值 — 交替拼接字面量片段和表达式结果
void Interpreter::visitInterpolatedString(InterpolatedString& node) {
    checkBreak(&node);
    // literals.size() == expressions.size() + 1
    // 结果: literals[0] + str(expressions[0]) + literals[1] + ... + str(expressions[n-1]) + literals[n]
    // PERF-11 fix: 用 node.literalsTotalLen 预计算 reserve，避免循环中多次 realloc
    std::string result = node.literals.empty() ? std::string() : node.literals[0];
    result.reserve(node.literalsTotalLen + node.expressions.size() * 8);
    for (size_t i = 0; i < node.expressions.size(); ++i) {
        Value exprVal = evaluate(node.expressions[i].get());
        result += exprVal.toString();
        if (i + 1 < node.literals.size()) {
            result += node.literals[i + 1];
        }
    }
    lastValue_ = Value(std::move(result));
    return;
}

void Interpreter::visitBoolLiteral(BoolLiteral& node) {
    checkBreak(&node);
    // 布尔字面量：返回 AST 中保存的布尔值。
    lastValue_ = Value(node.value);
    return;
}

void Interpreter::visitVarDecl(VarDecl& node) {
    checkBreak(&node);

    Value initVal = Value::nullValue();
    if (node.initializer) {
        initVal = evaluate(node.initializer.get());
    } else if (!node.typeAnnotation.empty()) {
        // 无初始化表达式但有类型注解 — 检查是否是类名
        auto classIt = classRegistry_.find(node.typeAnnotation);
        if (classIt != classRegistry_.end()) {
            // 自动创建类实例（无参构造）
            ClassInfo& cls = classIt->second;
            Value instance = Value::makeInstance(cls.name);

            // 复制类默认字段值（含继承链）— B8 fix: 加入循环检测
            ClassInfo* curCls = &cls;
            std::unordered_set<std::string> visitedClasses;
            while (curCls) {
                if (!visitedClasses.insert(curCls->name).second)
                    break; // 检测到循环继承
                for (const auto& kv : curCls->fields) {
                    // Perf-Finding: 缓存 find 迭代器，避免 operator[] 二次 hash 查找同键
                    auto& flds = instance.fields();
                    auto it = flds.find(kv.first);
                    if (it == flds.end()) {
                        flds.emplace(kv.first, kv.second);
                    }
                }
                if (!curCls->superClassName.empty()) {
                    auto superIt = classRegistry_.find(curCls->superClassName);
                    curCls = (superIt != classRegistry_.end()) ? &superIt->second : nullptr;
                } else {
                    curCls = nullptr;
                }
            }

            // 如果有 init 方法（0 必需参数，含全默认参数），执行它
            FunDecl* initMethod = findMethod(cls, "init");
            // P1-2 fix: 使用 requiredParamCount==0 判断，支持全默认参数 init
            if (initMethod && initMethod->requiredParamCount == 0) {
                auto parentEnv = cls.closureEnv ? cls.closureEnv : currentEnv_;
                auto initEnv = std::make_shared<Environment>(parentEnv);
                initEnv->define("this", instance);

                // AUDIT-P1 fix: 绑定 init 的默认参数到 initEnv（对齐 constructClassInstance）。
                // 原实现直接 executeFunctionBody，未绑定参数，init 体引用参数时穿透到父环境。
                for (size_t i = 0; i < initMethod->params.size(); ++i) {
                    if (i < initMethod->defaultValues.size() && initMethod->defaultValues[i]) {
                        // 默认值在类定义的闭包环境中求值（与 constructClassInstance 一致）
                        struct EnvGuard {
                            Interpreter& interp;
                            std::shared_ptr<Environment> prev;
                            ~EnvGuard() { interp.currentEnv_ = prev; }
                        } guard{*this, currentEnv_};
                        if (cls.closureEnv) {
                            currentEnv_ = cls.closureEnv;
                        }
                        Value defaultVal = evaluate(initMethod->defaultValues[i].get());
                        if (i < initMethod->paramTypes.size() && !initMethod->paramTypes[i].empty()) {
                            checkType(
                                defaultVal, initMethod->paramTypes[i],
                                [&] { return "类 " + cls.name + " 的 init 参数 " + initMethod->params[i]; }, node.line,
                                node.column);
                        }
                        initEnv->define(initMethod->params[i], std::move(defaultVal));
                    } else {
                        initEnv->define(initMethod->params[i], Value::nullValue());
                    }
                }

                auto prevEnv = currentEnv_;
                currentEnv_ = initEnv;

                // H-新2 fix: bindInstance 在 define 之后（此路径 0 参数无 rehash 风险，但保持一致性）
                Value* thisInEnv = const_cast<Value*>(initEnv->get("this"));
                if (thisInEnv)
                    initEnv->bindInstance(thisInEnv);

                // H3 fix: 与 visitFunCall 类构造路径保持一致的状态管理
                // B3 fix: CallFrameGuard 自动管理 currentFunctionReturnType_ + callStack_ + classContextStack_
                CallFrameGuard frameGuard{*this, initMethod->returnType, /*manageCtx=*/true};
                callStack_.emplace_back(cls.name + ".init", initEnv, node.line, recursionDepth_ + 1);
                // AUDIT-P1-CORRECT fix: 压入"init 方法定义所在类"而非"实例类"。
                // 当实例类未定义 init 而继承父类的 init 时，压入实例类名会导致
                // init 体内的 super 调用从错误的类开始搜索（应为定义 init 的类的父类）。
                classContextStack_.push_back(findMethodDefiningClassName(cls, "init"));

                // #8 fix: recursionDepth_ guard for auto-construction
                // S2 fix: 统一使用 RecursionGuard RAII 管理递归深度
                if (recursionDepth_ + 1 >= MAX_RECURSION_DEPTH) {
                    runtimeError(ErrorFormat::format("递归深度超过限制 (%d)", MAX_RECURSION_DEPTH), node.line,
                                 node.column);
                }
                RecursionGuard guard{recursionDepth_};

                // AUDIT-P1 fix: 析构时调用 closeCapturedVariables（对齐 constructClassInstance 的 InitEnvGuard）。
                // 原 PrevEnvGuard 只恢复 currentEnv_，未关闭 upvalue，init 体内创建的闭包
                // 捕获的局部变量在 initEnv 析构后丢失（weak_ptr 过期，capturedVars 未写入）。
                struct InitEnvGuard {
                    Interpreter& interp;
                    std::shared_ptr<Environment>& env;
                    std::shared_ptr<Environment>& prev;
                    bool dismissed = false;
                    ~InitEnvGuard() {
                        if (!dismissed) {
                            env->closeCapturedVariables();
                            interp.currentEnv_ = prev;
                        }
                    }
                } envGuard{*this, initEnv, prevEnv};

                try {
                    executeFunctionBody(static_cast<Block&>(*initMethod->body));
                } catch (const ReturnException&) {
                }
                // RA-A fix: 不再需要 catch(...) + throw; — envGuard 析构统一恢复 currentEnv_

                // B3 fix: callStack_/returnType/classContext 由 CallFrameGuard 自动恢复
                // RA-A fix: 由 envGuard 析构统一恢复 currentEnv_，此处 dismiss 避免重复
                envGuard.dismissed = true;
                initEnv->closeCapturedVariables();
                currentEnv_ = prevEnv;

                auto* thisPtr = initEnv->get("this");
                if (thisPtr) {
                    instance = *thisPtr;
                }
                // M3 fix: 移除冗余的局部变量→字段同步。P5 bindInstance 使 init 中的字段赋值
                // 直接写入 instance.fields()，此处同步是多余的且会因同名局部变量覆盖字段值。
            }

            initVal = instance;
        }
        // 其他类型注解（int/float/bool/string/dict/array）无初始化则保持 null
    }

    // 类型检查：如果有类型注解且有初始化表达式
    if (!node.typeAnnotation.empty() && node.initializer) {
        checkType(
            initVal, node.typeAnnotation, [&] { return "变量 " + node.name + " 的类型"; }, node.line, node.column);
    }
    // 记录类型注解（B2 fix: 存入当前作用域环境）
    if (!node.typeAnnotation.empty()) {
        currentEnv_->defineTypeAnnotation(node.name, node.typeAnnotation);
    }

    // AUDIT-P2-CORRECT fix: var 重声明捕获变量时，从 capturedVarNames_ 移除标记，
    // 防止 writeBackCapturedVars 将重声明的局部变量写回 capturedVars（污染下次调用）。
    // 在 define 之前调用 unmarkCaptured：若 tryDefineNew 因变量已存在而抛 RuntimeError，
    // 异常传播使函数调用栈展开，writeBackCapturedVars 不会执行，unmark 无副作用。
    currentEnv_->unmarkCaptured(node.name);
    // P21 fix: 原子性检查+插入，消除 find+define 双次查找
    if (!currentEnv_->tryDefineNew(node.name, initVal)) {
        runtimeError("变量 '" + node.name + "' 已在当前作用域中定义", node.line, node.column);
    }

    lastValue_ = std::move(initVal);
    return;
}

void Interpreter::visitAssignment(Assignment& node) {
    checkBreak(&node);

    Value val = evaluate(node.value.get());

    // 类型检查
    const std::string* typeAnn = findTypeAnnotation(node.name);
    if (typeAnn) {
        checkType(val, *typeAnn, [&] { return std::string("赋值给 ") + node.name; }, node.line, node.column);
    }

    if (!currentEnv_->set(node.name, val)) {
        runtimeError("未定义的变量: " + node.name, node.line, node.column);
    }
    lastValue_ = std::move(val);
    return;
}

void Interpreter::visitVarRef(VarRef& node) {
    checkBreak(&node);

    // C7: get() 返回指针，nullptr 表示变量未定义，消除 hasVariable() 双重遍历
    const Value* val = currentEnv_->get(node.name);
    if (!val) {
        runtimeError("未定义的变量: " + node.name, node.line, node.column);
    }
    lastValue_ = *val;
    return;
}

void Interpreter::visitIfStmt(IfStmt& node) {
    checkBreak(&node);

    Value cond = evaluate(node.condition.get());
    if (cond.isTruthy()) {
        evaluate(node.thenBranch.get());
        return;
    } else if (node.elseBranch) {
        evaluate(node.elseBranch.get());
        return;
    }
    lastValue_ = Value::nullValue();
    return;
}

void Interpreter::visitWhileStmt(WhileStmt& node) {
    checkBreak(&node);

    Value result = Value::nullValue();
    int64_t iterationCount = 0; // S-01 fix: 循环迭代计数
    while (evaluate(node.condition.get()).isTruthy()) {
        // S-01 fix: 防止无限循环导致 DoS
        if (++iterationCount > MAX_LOOP_ITERATIONS) {
            runtimeError(ErrorFormat::format("循环迭代次数超过上限 %lld，疑似无限循环",
                                             static_cast<long long>(MAX_LOOP_ITERATIONS)),
                         node.line, node.column);
        }
        // 每次迭代重新检查断点（MODE_RUN 下确保 while 行断点每次迭代都能命中；
        // STEP_IN/STEP_OVER 下 lastPausedLine_ 机制保证同行不重复暂停）
        checkBreak(&node);
        result = evaluate(node.body.get());
        // RA-C fix: 检查 break/continue 状态标志（替代原 catch BreakException/ContinueException）
        if (loopFlow_ == LoopFlow::Break) {
            loopFlow_ = LoopFlow::None; // 消费标志，恢复执行
            break;
        }
        if (loopFlow_ == LoopFlow::Continue) {
            loopFlow_ = LoopFlow::None; // 消费标志，跳到下次条件检查
            continue;
        }
        // ReturnException 仍按异常穿透（不在标志检查范围内）
    }
    lastValue_ = std::move(result);
    return;
}

void Interpreter::visitForStmt(ForStmt& node) {
    checkBreak(&node);

    // 在新作用域中执行初始化
    auto forEnv = std::make_shared<Environment>(currentEnv_);
    currentEnv_ = forEnv;

    // RA-A fix: RAII 守卫统一管理 forEnv 的 closeCapturedVariables 与 currentEnv_ 恢复，
    // 消除原 3 处 catch(...) + throw; 的 rethrow（初始化器、循环体 ReturnException、外层兜底）。
    // 守卫在正常路径和异常路径都执行清理，逻辑与原代码严格一致。
    struct ForEnvGuard {
        Interpreter& interp;
        std::shared_ptr<Environment>& env;
        bool dismissed = false;
        ~ForEnvGuard() {
            if (!dismissed) {
                env->closeCapturedVariables();
                interp.currentEnv_ = env->parent;
            }
        }
    } envGuard{*this, forEnv};

    if (node.initializer) {
        // RA-A fix: envGuard 已覆盖异常恢复，无需 try/catch + throw
        evaluate(node.initializer.get());
    }

    Value result = Value::nullValue();
    int64_t iterationCount = 0; // S-01 fix: 循环迭代计数
    while (true) {
        // S-01 fix: 防止无限循环导致 DoS
        if (++iterationCount > MAX_LOOP_ITERATIONS) {
            runtimeError(ErrorFormat::format("循环迭代次数超过上限 %lld，疑似无限循环",
                                             static_cast<long long>(MAX_LOOP_ITERATIONS)),
                         node.line, node.column);
        }
        // 每次迭代重新检查断点（同 visitWhileStmt 的修复原因）
        // #6 注：此处 checkBreak 暂停时，变量快照反映的是上一轮 upd 执行后的状态
        // （当前轮的 cond/body/upd 均未执行）。即循环变量 i 的值是上一轮更新后的值，
        // 而非"即将进入本轮 body 时的值"——由于 cond 通常是只读判断，两者实际等价。
        checkBreak(&node);

        // 条件检查
        if (node.condition) {
            Value cond = evaluate(node.condition.get());
            if (!cond.isTruthy())
                break;
        }

        // 执行循环体
        result = evaluate(node.body.get());
        // RA-C fix: 检查 break/continue 状态标志（替代原 catch BreakException/ContinueException）
        if (loopFlow_ == LoopFlow::Break) {
            loopFlow_ = LoopFlow::None; // 消费标志，跳出循环
            break;
        }
        if (loopFlow_ == LoopFlow::Continue) {
            loopFlow_ = LoopFlow::None; // 消费标志，跳到更新步骤
            // 注意：不 continue，落到 update 步骤（与原 catch ContinueException 后空体一致）
        }
        // RA-A fix: ReturnException 仍按异常穿透（不在标志检查范围内）

        // 更新
        if (node.update) {
            evaluate(node.update.get());
        }
    }

    // B1 fix: 关闭捕获 — 循环正常退出时将循环变量终值写回闭包 capturedVars。
    // 这是关键：循环变量 i 在此关闭为终值（如 3），使所有捕获 i 的闭包返回终值。
    // RA-A fix: 由 envGuard 析构统一执行，此处 dismiss 避免重复
    envGuard.dismissed = true;
    forEnv->closeCapturedVariables();
    currentEnv_ = forEnv->parent;
    lastValue_ = std::move(result);
    return;
}

// ============================================================
// B1 fix: 闭包仅捕获自由变量（静态分析 AST）
// ============================================================

namespace {
/// 在作用域栈中查找变量是否已定义（从内到外搜索所有作用域）
bool isDefinedInScopes(const std::vector<std::unordered_set<std::string>>& scopes, const std::string& name) {
    for (const auto& scope : scopes) {
        if (scope.count(name))
            return true;
    }
    return false;
}
} // anonymous namespace

std::unordered_set<std::string> Interpreter::computeFreeVariables(const FunDecl& fn) {
    // PERF-08 fix: 同一 FunDecl 的自由变量集仅依赖函数体 AST 结构（纯函数），
    // 重复捕获闭包时无需重新遍历整个 AST。首次计算后缓存到 fn.cachedFreeVars_。
    // AST 重建（execute/REPL 重解析）会构造全新 FunDecl 对象，缓存自动失效。
    if (fn.cachedFreeVars_) {
        return *fn.cachedFreeVars_;
    }

    // 函数作用域：参数 + 函数自身名称（允许递归自引用）
    std::vector<std::unordered_set<std::string>> scopes;
    scopes.emplace_back();
    for (const auto& param : fn.params)
        scopes.back().insert(param);
    scopes.back().insert(fn.name);

    std::unordered_set<std::string> freeVars;

    // 默认参数值在外层作用域求值（不在函数作用域内）
    for (const auto& dv : fn.defaultValues) {
        if (dv)
            collectFreeVars(*dv, scopes, freeVars);
    }

    // 分析函数体
    if (fn.body)
        collectFreeVars(*fn.body, scopes, freeVars);

    fn.cachedFreeVars_ = freeVars; // 缓存结果
    return freeVars;
}

void Interpreter::collectFreeVars(const ASTNode& node, std::vector<std::unordered_set<std::string>>& scopes,
                                  std::unordered_set<std::string>& freeVars) {
    switch (node.nodeType) {
    case NodeType::NODE_VAR_REF: {
        const auto& ref = static_cast<const VarRef&>(node);
        if (!isDefinedInScopes(scopes, ref.name)) {
            freeVars.insert(ref.name);
        }
        break;
    }
    case NodeType::NODE_ASSIGNMENT: {
        const auto& assign = static_cast<const Assignment&>(node);
        if (!isDefinedInScopes(scopes, assign.name)) {
            freeVars.insert(assign.name);
        }
        if (assign.value)
            collectFreeVars(*assign.value, scopes, freeVars);
        break;
    }
    case NodeType::NODE_VAR_DECL: {
        const auto& decl = static_cast<const VarDecl&>(node);
        // 初始化器在变量声明前求值（变量尚未进入作用域）
        if (decl.initializer)
            collectFreeVars(*decl.initializer, scopes, freeVars);
        scopes.back().insert(decl.name);
        break;
    }
    case NodeType::NODE_FUN_DECL: {
        // 嵌套函数：用全新作用域栈分析其自由变量，再合并到外层
        const auto& nestedFn = static_cast<const FunDecl&>(node);
        // 默认参数值在外层（当前）作用域求值
        for (const auto& dv : nestedFn.defaultValues) {
            if (dv)
                collectFreeVars(*dv, scopes, freeVars);
        }
        // 用全新作用域栈分析嵌套函数体
        std::vector<std::unordered_set<std::string>> nestedScopes;
        nestedScopes.emplace_back();
        for (const auto& p : nestedFn.params)
            nestedScopes.back().insert(p);
        nestedScopes.back().insert(nestedFn.name);
        std::unordered_set<std::string> nestedFree;
        if (nestedFn.body)
            collectFreeVars(*nestedFn.body, nestedScopes, nestedFree);
        // 嵌套函数的自由变量若不在外层作用域定义，则为外层自由变量
        for (const auto& name : nestedFree) {
            if (!isDefinedInScopes(scopes, name)) {
                freeVars.insert(name);
            }
        }
        // 嵌套函数名是外层作用域的局部变量
        scopes.back().insert(nestedFn.name);
        break;
    }
    case NodeType::NODE_FUN_CALL: {
        const auto& call = static_cast<const FunCall&>(node);
        if (call.callee) {
            collectFreeVars(*call.callee, scopes, freeVars);
        } else if (!call.name.empty()) {
            // 命名调用：函数名视为变量引用（可能是用户函数/类/闭包变量）
            if (!isDefinedInScopes(scopes, call.name)) {
                freeVars.insert(call.name);
            }
        }
        for (const auto& arg : call.arguments) {
            if (arg)
                collectFreeVars(*arg, scopes, freeVars);
        }
        break;
    }
    case NodeType::NODE_BLOCK: {
        const auto& block = static_cast<const Block&>(node);
        scopes.emplace_back(); // 块作用域
        for (const auto& stmt : block.statements) {
            if (stmt)
                collectFreeVars(*stmt, scopes, freeVars);
        }
        scopes.pop_back();
        break;
    }
    case NodeType::NODE_FOR_STMT: {
        const auto& forStmt = static_cast<const ForStmt&>(node);
        scopes.emplace_back(); // for 循环作用域（含循环变量）
        if (forStmt.initializer)
            collectFreeVars(*forStmt.initializer, scopes, freeVars);
        if (forStmt.condition)
            collectFreeVars(*forStmt.condition, scopes, freeVars);
        if (forStmt.update)
            collectFreeVars(*forStmt.update, scopes, freeVars);
        if (forStmt.body)
            collectFreeVars(*forStmt.body, scopes, freeVars);
        scopes.pop_back();
        break;
    }
    case NodeType::NODE_TRY_STMT: {
        const auto& tryStmt = static_cast<const TryStmt&>(node);
        if (tryStmt.tryBlock)
            collectFreeVars(*tryStmt.tryBlock, scopes, freeVars);
        if (tryStmt.catchBlock) {
            scopes.emplace_back(); // catch 块作用域（含异常变量）
            if (!tryStmt.catchVarName.empty())
                scopes.back().insert(tryStmt.catchVarName);
            collectFreeVars(*tryStmt.catchBlock, scopes, freeVars);
            scopes.pop_back();
        }
        break;
    }
    case NodeType::NODE_CLASS_DECL: {
        const auto& cls = static_cast<const ClassDecl&>(node);
        // 类名是当前作用域的局部变量
        scopes.back().insert(cls.name);
        // 不分析类成员：类方法通过 closureEnv 访问外层变量，不依赖 capturedVars
        break;
    }
    default:
        // 其他节点：递归遍历子节点（BinaryOp/UnaryOp/字面量/IfStmt/WhileStmt/
        // ReturnStmt/PrintStmt/MemberAccess/MemberAssign/MethodCall/IndexAccess/
        // IndexAssign/ThrowStmt/ImportStmt/ExportStmt/SuperExpr/BreakStmt/
        // ContinueStmt/NullLiteral 等）
        for (auto* child : node.children()) {
            if (child)
                collectFreeVars(*child, scopes, freeVars);
        }
        break;
    }
}

void Interpreter::visitFunDecl(FunDecl& node) {
    checkBreak(&node);

    // A3 fix: 通过 shared_from_this() 获取 shared_ptr<FunDecl>，使闭包和 funRegistry_
    // 共享 AST 节点所有权，避免 AST 重建后裸指针悬垂。
    // AST 节点由 Block::statements（vector<shared_ptr<ASTNode>>）持有，此处共享所有权。
    // A3 bug fix: shared_from_this() 要求对象已被 shared_ptr 管理，否则抛 bad_weak_ptr。
    // 当前所有调用路径均满足此约束，但为防御未来误用（如栈上构造的 FunDecl），
    // 捕获异常并回退到 funRegistry_ 查找（仅失去自包含闭包语义，不致进程终止）。
    std::shared_ptr<FunDecl> nodeShared;
    try {
        nodeShared = std::static_pointer_cast<FunDecl>(node.shared_from_this());
    } catch (const std::bad_weak_ptr&) {
        runtimeError("内部错误: FunDecl 节点未被 shared_ptr 管理，无法注册函数 " + node.name, node.line, node.column);
    }

    // 创建闭包值，捕获当前环境并存储函数体指针（自包含，不依赖 funRegistry_）
    Value funVal = Value::makeClosure(node.name, currentEnv_, node.params, nodeShared);

    // AUDIT-BUG-I1 fix: 标记当前 env 被闭包引用（env weak_ptr 目标），
    // 防止 visitBlock 退出时将 blockEnv 回收到 envPool_——即使闭包仅捕获父级变量
    // （hasClosureCaptures()=false），blockEnv 也不能回收，否则 resetForReuse
    // 会清空 variables 并替换 parent，导致闭包调用时变量查找失败。
    currentEnv_->markClosureEnvRef();

    // B1 fix: 仅捕获自由变量（函数体实际引用的外层变量），而非整个环境快照。
    // 原 C1 fix 复制 allVariablesMap() 的全部可见变量，REPL 模式下随变量积累越来越慢；
    // 现通过静态分析 AST 仅捕获实际需要的变量。getVariableOnly 确保不捕获实例字段
    // （与原 collectVariables 行为一致）。
    //
    // B1 open/close upvalue: 同时在"定义该变量的 env"上注册闭包捕获，
    // 作用域退出时 closeCapturedVariables() 将最终值写回 capturedVars。
    // 这使循环变量 i 在 for 退出时关闭为终值 3，循环体变量在 block 退出时关闭为当次值。
    auto freeVars = computeFreeVariables(node);
    auto& captured = funVal.capturedVars();
    captured.reserve(freeVars.size());
    for (const auto& name : freeVars) {
        if (const Value* val = currentEnv_->getVariableOnly(name)) {
            captured.emplace(name, *val);
        }
        // B1 fix: 在定义该变量的 env 上注册闭包捕获（等价于 VM 的 open upvalue）。
        // 作用域退出时 closeCapturedVariables() 将最终值写回 capturedVars。
        Environment* definingEnv = currentEnv_.get();
        while (definingEnv) {
            if (definingEnv->getLocalVariable(name))
                break;
            definingEnv = definingEnv->parent.get();
        }
        if (definingEnv) {
            definingEnv->registerClosureCapture(funVal, name);
        }
    }

    currentEnv_->define(node.name, funVal);

    // 保留 funRegistry_ 作为后备（处理闭包 body 缺失的场景）
    // A3 fix: 存储 shared_ptr 而非裸指针
    funRegistry_[node.name] = nodeShared;
    funRegistryGen_++; // M7: 函数注册/重定义时递增代数，使旧缓存失效

    lastValue_ = std::move(funVal);
    return;
}

void Interpreter::executeFunctionBody(Block& body) {
    // B1 fix: 直接在 currentEnv_（funEnv）中执行函数体语句，不创建嵌套块作用域。
    // 异常处理（ReturnException 等）由调用方（callClosureValue 等）的
    // try-catch 负责，closeCapturedVariables 也由调用方在 funEnv 上调用。
    // RA-C fix: break/continue 改用标志后，函数体顶层出现未消费的标志属于语义错误
    // （break/continue 只能在循环体内使用）。原代码靠 BreakException/ContinueException
    // 穿透到 runStatementsWithExceptionHandling 的兜底 catch 报错，此处等价检查标志。
    for (auto& stmt : body.statements) {
        evaluate(stmt.get());
        if (loopFlow_ != LoopFlow::None) {
            // 清理标志后报错（与原 runStatementsWithExceptionHandling 兜底语义一致）
            std::string msg =
                (loopFlow_ == LoopFlow::Break) ? "break 只能在循环体内使用" : "continue 只能在循环体内使用";
            loopFlow_ = LoopFlow::None;
            runtimeError(msg, stmt ? stmt->line : 0, stmt ? stmt->column : 0);
        }
    }
}

void Interpreter::visitReturnStmt(ReturnStmt& node) {
    checkBreak(&node);

    Value val = Value::nullValue();
    if (node.value) {
        val = evaluate(node.value.get());
    }

    // 返回类型检查
    if (!currentFunctionReturnType_.empty()) {
        checkType(val, currentFunctionReturnType_, [] { return std::string("返回值"); }, node.line, node.column);
    }

    throw ReturnException(std::move(val));
}

void Interpreter::visitBreakStmt(BreakStmt& node) {
    checkBreak(&node);
    // RA-C fix: 改用状态标志替代 C++ 异常。循环边界（visitWhileStmt/visitForStmt）检查标志。
    // 标志会向上传播：visitBlock/visitIfStmt 在 evaluate 后检查 loopFlow_ 并提前退出。
    loopFlow_ = LoopFlow::Break;
    lastValue_ = Value::nullValue();
    return;
}

void Interpreter::visitContinueStmt(ContinueStmt& node) {
    checkBreak(&node);
    // RA-C fix: 改用状态标志替代 C++ 异常。循环边界（visitWhileStmt/visitForStmt）检查标志。
    // 标志会向上传播：visitBlock/visitIfStmt 在 evaluate 后检查 loopFlow_ 并提前退出。
    loopFlow_ = LoopFlow::Continue;
    lastValue_ = Value::nullValue();
    return;
}

void Interpreter::visitThrowStmt(ThrowStmt& node) {
    checkBreak(&node);
    Value val = evaluate(node.expression.get());
    throw ThrowException(std::move(val));
}

void Interpreter::visitTryStmt(TryStmt& node) {
    checkBreak(&node);
    // BUG-AUDIT-FINALLY-1: finally 块语义
    // - 正常退出（try/catch 正常完成）：执行 finally
    // - 异常退出（try/catch 抛出未捕获异常）：执行 finally 后 re-throw
    // - return：不执行 finally（与 VM 路径一致，三后端一致）
    // - break/continue：Interpreter 执行 finally（loopFlow_ 状态标志路径，try 块正常完成后继续执行 finally），
    //   VM 跳过 finally（OP_TRY_END 弹出 handler 后直接跳转），三后端不一致为已知限制
    // - try-finally（无 catch）：异常不被捕获，finally 执行后 re-throw
    bool finallyRun = false;
    try {
        if (!node.catchVarName.empty()) {
            // try-catch(-finally)：有 catch 子句，捕获异常
            try {
                if (node.tryBlock) {
                    evaluate(node.tryBlock.get());
                }
            } catch (ThrowException& e) {
                // 在 catch 块的新作用域中绑定异常变量
                auto catchEnv = std::make_shared<Environment>(currentEnv_);
                auto savedEnv = currentEnv_;
                currentEnv_ = catchEnv;
                // P2-1 fix: 使用 std::move 避免不必要的 Value 拷贝
                currentEnv_->define(node.catchVarName, std::move(e.thrownValue));

                // RA-A fix: RAII 守卫统一管理 catchEnv 的 closeCapturedVariables 与 currentEnv_ 恢复，
                // 消除原 catch(...) + throw; 的 rethrow。
                // catch 块内若抛出 return/break/continue/throw，envGuard 析构恢复 catchEnv 后异常自然传播。
                struct CatchEnvGuard {
                    Interpreter& interp;
                    std::shared_ptr<Environment>& env;
                    std::shared_ptr<Environment>& saved;
                    bool dismissed = false;
                    ~CatchEnvGuard() {
                        if (!dismissed) {
                            // B1 fix: 关闭捕获 — catch 块退出时将最终值写回闭包 capturedVars
                            env->closeCapturedVariables();
                            interp.currentEnv_ = saved;
                        }
                    }
                } envGuard{*this, catchEnv, savedEnv};

                if (node.catchBlock) {
                    evaluate(node.catchBlock.get());
                }
                // RA-A fix: 不再需要 catch(...) + throw; — envGuard 析构统一恢复
                // B1 fix: 关闭捕获 — catch 块正常退出
                envGuard.dismissed = true;
                catchEnv->closeCapturedVariables();
                currentEnv_ = savedEnv;
            }
        } else {
            // try-finally（无 catch）：不捕获异常，让异常传播到外层 catch
            if (node.tryBlock) {
                evaluate(node.tryBlock.get());
            }
        }
        // 正常退出：执行 finally
        if (node.finallyBlock) {
            // BUG-AUDIT-FINALLY-DOUBLE fix: finallyRun 必须在 evaluate(finallyBlock) 之前设置，
            // 否则 finally 块自身抛 ThrowException 时 finallyRun 仍为 false，
            // 外层 catch 会误判为"finally 未执行"并再次执行 finally，导致双重执行。
            // 三后端一致性：VM/RegisterVM 路径中 finally 块抛 throw 时只执行一次（新异常覆盖原异常）。
            finallyRun = true;
            evaluate(node.finallyBlock.get());
        } else {
            finallyRun = true;
        }
    } catch (const ThrowException&) {
        // throw 语句异常：执行 finally 后 re-throw
        if (!finallyRun && node.finallyBlock) {
            evaluate(node.finallyBlock.get());
        }
        throw;
    } catch (const ReturnException&) {
        // return 不执行 finally（与 VM 一致，三后端一致）
        throw;
    } catch (const DebugStopException&) {
        // 调试中止信号不执行 finally
        throw;
    } catch (...) {
        // AUDIT-P1-CORRECT fix: 其他异常（RuntimeError 如数组越界/类型不匹配/除零等）
        // 执行 finally 后 re-throw，对齐 VM 路径（OP_TRY_BEGIN 捕获所有运行时错误会执行 finally）。
        // 原实现仅 catch (const ThrowException&)，RuntimeError 不被捕获直接传播跳过 finally，
        // 导致三后端不一致（Interpreter 跳过 finally，VM 执行 finally）。
        // 注：BreakException/ContinueException 在 Interpreter 中用 loopFlow_ 状态标志实现，
        // 不会以异常形式传播，故 catch(...) 不会捕获它们。
        if (!finallyRun && node.finallyBlock) {
            evaluate(node.finallyBlock.get());
        }
        throw;
    }
    lastValue_ = Value::nullValue();
    return;
}

void Interpreter::visitPrintStmt(PrintStmt& node) {
    checkBreak(&node);

    std::string result;
    // P-08 fix: 预估结果字符串容量
    result.reserve(node.values.size() * 16);
    for (size_t i = 0; i < node.values.size(); ++i) {
        Value val = evaluate(node.values[i].get());
        if (i > 0)
            result += " ";
        result += val.toString();
    }
    output(result);
    lastValue_ = Value::nullValue();
    return;
}

void Interpreter::visitBlock(Block& node) {
    checkBreak(&node);

    // 快速路径：空块直接返回
    if (node.statements.empty()) {
        lastValue_ = Value::nullValue();
        return;
    }

    // PERF-07 fix: 块作用域 Environment 对象池复用。
    // 优先从 envPool_ 取出已回收的 Environment 并 reset，避免 make_shared 堆分配。
    // 退出时若 blockEnv 未被闭包捕获（use_count==1），回收至池供下次复用。
    std::shared_ptr<Environment> blockEnv;
    if (!envPool_.empty()) {
        blockEnv = std::move(envPool_.back());
        envPool_.pop_back();
        blockEnv->resetForReuse(currentEnv_);
    } else {
        blockEnv = std::make_shared<Environment>(currentEnv_);
    }
    auto savedEnv = currentEnv_;
    currentEnv_ = blockEnv;

    Value result = Value::nullValue();

    // RA-A fix: RAII 守卫统一管理异常路径下的 currentEnv_ 恢复与 closeCapturedVariables，
    // 消除原 catch(...) + throw; 的 rethrow。正常路径通过 dismiss 跳过守卫清理，
    // 走完整的池化回收逻辑。
    struct BlockEnvGuard {
        Interpreter& interp;
        std::shared_ptr<Environment>& env;
        std::shared_ptr<Environment>& saved;
        bool dismissed = false;
        ~BlockEnvGuard() {
            if (!dismissed) {
                interp.currentEnv_ = saved;
                // B1 fix: 异常路径也需关闭捕获 — 将最终值写回闭包 capturedVars
                env->closeCapturedVariables();
                // 异常路径不回收（blockEnv 可能已被闭包捕获，安全起见让 shared_ptr 自然销毁）
            }
        }
    } envGuard{*this, blockEnv, savedEnv};

    for (auto& stmt : node.statements) {
        result = evaluate(stmt.get());
        // RA-C fix: break/continue 标志需穿透 block 边界到达循环。
        // 若不提前退出，后续语句会执行不该执行的副作用，且标志可能在循环外被误消费。
        if (loopFlow_ != LoopFlow::None)
            break;
    }

    // RA-A fix: 正常路径，dismiss 守卫后手动执行完整清理（含池化回收）
    envGuard.dismissed = true;
    currentEnv_ = savedEnv;

    // B1 fix: 关闭捕获 — 将 block 内声明的变量的最终值写回闭包 capturedVars。
    // 必须在检查 hasClosureCaptures 之前调用（close 会清空列表）。
    bool hadCaptures = blockEnv->hasClosureCaptures();
    blockEnv->closeCapturedVariables();

    // PERF-07: 若 blockEnv 独占所有权（未被闭包/子作用域捕获），回收至池复用。
    // use_count()==1 表示仅 blockEnv 本地变量持有，可安全 reset。
    // B1 fix: 有捕获的 env 不回收 — weak_ptr 仍指向它，回收后 resetForReuse 清空
    // variables 会导致闭包调用时闭包环境"看似存活但内容陈旧"，绕过 capturedVars 回退。
    // AUDIT-BUG-I1 fix: 也检查 hasClosureEnvRef — 闭包 env weak_ptr 指向本 env 时
    // 不能回收（即使 hasClosureCaptures=false，闭包可能仅捕获父级变量）。
    if (blockEnv.use_count() == 1 && !hadCaptures && !blockEnv->hasClosureEnvRef()) {
        // P3.3 fix: 限制 envPool_ 大小，防止异常场景下无界增长。
        // 正常场景下 pool 大小受源码嵌套深度限制（Parser 深度上限 512），
        // execute()/executeRepl() 入口已 clear()。cap=64 作为防御性上限——
        // 超过时直接析构（shared_ptr 引用计数归零），不回收至池。
        if (envPool_.size() < 64) {
            envPool_.push_back(std::move(blockEnv));
        }
    }

    lastValue_ = std::move(result);
    return;
}

// ---- 新增 9 个 visit 方法 ----

void Interpreter::visitArrayLiteral(ArrayLiteral& node) {
    checkBreak(&node);
    // 数组字面量：从左到右依次求值各元素（副作用按书写顺序发生），构造 Value 数组返回。

    std::vector<Value> elements;
    elements.reserve(node.elements.size());
    for (auto& elem : node.elements) {
        elements.push_back(evaluate(elem.get()));
    }
    lastValue_ = Value(std::move(elements));
    return;
}

void Interpreter::visitDictLiteral(DictLiteral& node) {
    checkBreak(&node);
    // 字典字面量：依次求值每个键值对（键必须为字符串，否则报错），构造 Value 字典返回。

    std::unordered_map<std::string, Value> dict;
    dict.reserve(node.pairs.size());
    for (auto& pair : node.pairs) {
        Value key = evaluate(pair.first.get());
        Value val = evaluate(pair.second.get());
        // B6 fix: 对齐 RegisterVM REG_BUILD_DICT——非 string 键显式报错，
        // 不再静默 toString() 转换（与索引访问 d[k] 要求 string 键一致）。
        if (!key.isString()) {
            runtimeError("字典键必须是字符串", node.line, node.column);
        }
        dict[key.stringVal()] = std::move(val);
    }
    lastValue_ = Value(std::move(dict));
    return;
}

void Interpreter::visitIndexAccess(IndexAccess& node) {
    checkBreak(&node);

    Value obj = evaluate(node.object.get());
    Value idx = evaluate(node.index.get());

    // P0-3 fix: 使用 const 引用避免在只读访问时触发 COW 深拷贝
    const Value& objC = obj;

    // 数组索引访问
    if (objC.isArray()) {
        if (!idx.isInt()) {
            runtimeError("数组索引必须是整数", node.line, node.column);
        }
        int64_t i = idx.intVal();
        const auto& arr = objC.arrayVal();
        if (i < 0 || static_cast<size_t>(i) >= arr.size()) {
            runtimeError(
                ErrorFormat::format("数组索引越界: %lld, 有效范围 [0, %zu)", static_cast<long long>(i), arr.size()),
                node.line, node.column);
        }
        lastValue_ = arr[static_cast<size_t>(i)];
        return;
    }

    // 字典索引访问
    if (objC.isDict()) {
        if (!idx.isString()) {
            runtimeError("字典键必须是字符串", node.line, node.column);
        }
        const auto& dict = objC.dictVal();
        auto it = dict.find(idx.stringVal());
        if (it == dict.end()) {
            lastValue_ = Value::nullValue();
            return;
        }
        lastValue_ = it->second;
        return;
    }

    // 字符串索引访问：返回单字符字符串（M6 fix: 基于 UTF-8 码位而非字节）
    if (objC.isString()) {
        if (!idx.isInt()) {
            runtimeError("字符串索引必须是整数", node.line, node.column);
        }
        int64_t i = idx.intVal();
        const std::string& s = objC.stringVal();

        // #13 fix: ASCII 快速路径（镜像 VM 的 P7 fix）。
        // 纯 ASCII 字符串每码位 1 字节，可直接按字节索引 O(1)。
        // 按 StringData 指针缓存 ASCII 判定，循环 s[i] 访问时仅首次 O(n) 扫描，后续 O(1)。
        // 原实现每次访问都 O(i) 扫描到目标码位，循环退化 O(n²)。
        if (BoundsCheck::inBounds(i, s.size())) {
            const void* strPtr = static_cast<const void*>(&s);
            size_t strLen = s.size();
            bool isAscii;
            // BUG-INTP-3 fix: 缓存 key 包含 (ptr, len)，原地 append 后 len 变化使缓存失效
            if (lastAsciiStrPtr_ == strPtr && lastAsciiStrLen_ == strLen) {
                isAscii = lastAsciiStrIsAscii_;
            } else {
                isAscii = true;
                for (size_t b = 0; b < s.size(); ++b) {
                    if (static_cast<unsigned char>(s[b]) >= 0x80) {
                        isAscii = false;
                        break;
                    }
                }
                lastAsciiStrPtr_ = strPtr;
                lastAsciiStrLen_ = strLen;
                lastAsciiStrIsAscii_ = isAscii;
            }
            if (isAscii) {
                lastValue_ = Value(s.substr(static_cast<size_t>(i), 1));
                return;
            }
        }

        // 非 ASCII 慢路径：逐码位扫描
        size_t charCount = 0;
        size_t bytePos = 0;
        size_t targetBytePos = 0;
        size_t targetByteLen = 0;
        bool found = false;
        while (bytePos < s.size()) {
            unsigned char c = static_cast<unsigned char>(s[bytePos]);
            size_t charLen = (c < 0x80)             ? 1
                             : ((c & 0xE0) == 0xC0) ? 2
                             : ((c & 0xF0) == 0xE0) ? 3
                             : ((c & 0xF8) == 0xF0) ? 4
                                                    : 1;
            if (static_cast<size_t>(i) == charCount) {
                targetBytePos = bytePos;
                targetByteLen = charLen;
                found = true;
            }
            bytePos += charLen;
            charCount++;
        }
        if (i < 0 || !found) {
            runtimeError(ErrorFormat::format("字符串索引越界: %lld, 有效范围 [0, %lld)", static_cast<long long>(i),
                                             static_cast<long long>(charCount)),
                         node.line, node.column);
        }
        lastValue_ = Value(s.substr(targetBytePos, targetByteLen));
        return;
    }

    runtimeError("该类型不支持索引访问", node.line, node.column);
}

void Interpreter::visitIndexAssign(IndexAssign& node) {
    checkBreak(&node);
    // 左到右求值：object → index → value（由 writeBack 内部按序求值）
    lastValue_ = writeBack(node.object.get(), true, node.index.get(), "", node.value.get(), node.line, node.column);
    return;
}

void Interpreter::visitMethodCall(MethodCall& node) {
    checkBreak(&node);

    // P0-4 fix: 通过 collectAndEvaluateChain 一次性求值对象链，
    // 避免对含副作用的子表达式（如 arr[sideEffect()].push(1)）重复求值
    ChainInfo info = collectAndEvaluateChain(node.object.get(), false, node.line, node.column);
    Value obj;
    if (info.varRef) {
        obj = std::move(info.vals[0]);
    } else {
        obj = evaluate(node.object.get());
    }

    // ---- 数组内置方法 ----
    if (obj.isArray()) {
        auto argValues = evaluateArguments(node.arguments);
        auto builtinResult = BuiltinMethods::handleArrayMethod(node.methodName, obj, argValues, node.line, node.column);
        // P2-7 fix: obj 此后不再使用，使用 std::move 避免不必要的拷贝
        if (builtinResult.objectModified && info.varRef) {
            writeBackChain(info, std::move(obj), node.line, node.column);
        }
        lastValue_ = std::move(builtinResult.result);
        return;
    }

    // ---- 字典内置方法 ----
    if (obj.isDict()) {
        auto argValues = evaluateArguments(node.arguments);
        auto builtinResult = BuiltinMethods::handleDictMethod(node.methodName, obj, argValues, node.line, node.column);
        // P2-7 fix: obj 此后不再使用，使用 std::move 避免不必要的拷贝
        if (builtinResult.objectModified && info.varRef) {
            writeBackChain(info, std::move(obj), node.line, node.column);
        }
        lastValue_ = std::move(builtinResult.result);
        return;
    }

    // ---- 字符串内置方法 ----
    if (obj.isString()) {
        auto argValues = evaluateArguments(node.arguments);
        auto builtinResult =
            BuiltinMethods::handleStringMethod(node.methodName, obj, argValues, node.line, node.column);
        lastValue_ = std::move(builtinResult.result);
        return;
    }

    // 类实例的方法调用
    if (obj.isInstance()) {
        Value result = callInstanceMethod(node, obj);
        // P2-7 fix: obj 此后不再使用，使用 std::move 避免不必要的拷贝
        if (info.varRef) {
            writeBackChain(info, std::move(obj), node.line, node.column);
        }
        lastValue_ = std::move(result);
        return;
    }

    // 2026-06-29 BUG-1 fix (与 StackVM/RegisterVM 对齐): 加入方法名，
    // 三后端统一为"类型 X 不支持方法 Y"格式（原消息缺少方法名）。
    runtimeError("类型 " + obj.typeName() + " 不支持方法 " + node.methodName, node.line, node.column);
}

// ---- P1 重构：类实例方法调用（含 super.method() 处理）----
Value Interpreter::callInstanceMethod(MethodCall& node, Value& obj) {
    auto classIt = classRegistry_.find(obj.className());
    if (classIt != classRegistry_.end()) {
        // O1: super.method() — 从父类开始查找方法
        bool isSuperCall = (node.object && node.object->nodeType == NodeType::NODE_SUPER_EXPR);
        ClassInfo* searchClass = &classIt->second;
        // A3 bug fix: 缓存 searchClass 名称，参数求值后重新查找避免悬垂
        std::string searchClassName;
        if (isSuperCall) {
            // 使用 classContextStack_ 确定当前执行类的父类（修复多层 super.init() 递归）
            std::string currentClassName;
            if (!classContextStack_.empty()) {
                currentClassName = classContextStack_.back();
            } else {
                currentClassName = obj.className();
            }
            auto ctxIt = classRegistry_.find(currentClassName);
            if (ctxIt == classRegistry_.end() || ctxIt->second.superClassName.empty()) {
                runtimeError("类 " + currentClassName + " 没有父类，不能使用 super", node.line, node.column);
            }
            auto superIt = classRegistry_.find(ctxIt->second.superClassName);
            if (superIt == classRegistry_.end()) {
                runtimeError("未定义的父类: " + ctxIt->second.superClassName, node.line, node.column);
            }
            searchClassName = superIt->second.name;
            searchClass = &superIt->second;
        } else {
            searchClassName = searchClass->name;
        }
        FunDecl* method = findMethod(*searchClass, node.methodName);
        if (method) {
            // 求值参数
            std::vector<Value> argValues;
            // F10: 支持默认参数
            size_t argCount = node.arguments.size();
            if (argCount < static_cast<size_t>(method->requiredParamCount) || argCount > method->params.size()) {
                runtimeError(ErrorFormat::format("方法 %s 期望 %d-%zu 个参数，但传入了 %zu 个", node.methodName.c_str(),
                                                 method->requiredParamCount, method->params.size(), argCount),
                             node.line, node.column);
            }
            argValues.reserve(argCount);

            // #2 fix: 缓存closureEnv
            // H4 fix: 对 super.method() 调用，使用方法实际定义所在类（searchClass）的环境，
            // 而非实例所属类（classIt）的环境，确保继承链中跨作用域的变量绑定正确
            auto cachedParentEnv = searchClass->closureEnv;
            for (auto& arg : node.arguments) {
                argValues.push_back(evaluate(arg.get()));
            }

            // A3 bug fix: 参数求值可能触发类重定义（如参数中调用重定义类的函数），
            // 导致 classRegistry_ 中 ClassInfo 被替换、searchClass 悬垂。
            // 对齐 constructClassInstance 的 #2 fix：参数求值后重新查找。
            {
                auto reIt = classRegistry_.find(searchClassName);
                if (reIt == classRegistry_.end()) {
                    runtimeError("类 " + searchClassName + " 在方法调用期间被重定义并删除", node.line, node.column);
                }
                searchClass = &reIt->second;
                method = findMethod(*searchClass, node.methodName);
                if (!method) {
                    runtimeError("类 " + searchClassName + " 在方法调用期间被重定义，方法 " + node.methodName +
                                     " 不再存在",
                                 node.line, node.column);
                }
                // 重新缓存 closureEnv（类重定义后环境可能变化）
                cachedParentEnv = searchClass->closureEnv;
            }

            // F10: 为缺失的参数填充默认值（在类定义的闭包环境中求值）
            // AUDIT-P2 fix: 用 RAII guard 恢复 currentEnv_，对齐 constructClassInstance/
            // callNamedFunction/callClosureValue 的 EnvGuard 模式。原实现手动 save/restore，
            // evaluate(defaultValues[i]) 抛异常时 currentEnv_ 残留为 cachedParentEnv。
            if (argCount < method->params.size()) {
                struct EnvGuard {
                    Interpreter& interp;
                    std::shared_ptr<Environment> prev;
                    ~EnvGuard() { interp.currentEnv_ = prev; }
                } guard{*this, currentEnv_};
                if (cachedParentEnv) {
                    currentEnv_ = cachedParentEnv;
                }
                for (size_t i = argCount; i < method->params.size(); ++i) {
                    if (method->defaultValues[i]) {
                        Value defaultVal = evaluate(method->defaultValues[i].get());
                        // AUDIT-P2 fix: 默认值即时类型检查，对齐 constructClassInstance。
                        // 原实现延迟到统一参数绑定循环检查，导致第一个默认值类型错误时
                        // 后续默认值仍被评估（可能有副作用）。改为即时检查，在第一个
                        // 类型错误处停止，与 constructClassInstance 行为一致。
                        if (i < method->paramTypes.size() && !method->paramTypes[i].empty()) {
                            checkType(
                                defaultVal, method->paramTypes[i],
                                [&] { return "方法 " + node.methodName + " 的参数 " + method->params[i]; }, node.line,
                                node.column);
                        }
                        argValues.push_back(std::move(defaultVal));
                    } else {
                        argValues.push_back(Value::nullValue());
                    }
                }
            }

            // AUDIT-P2-ROUND49 fix: 默认参数求值可能触发类重定义（如默认值表达式调用
            // 重定义类的函数），导致 searchClass/method 悬垂。对齐 #2 fix（实参求值后重新查找），
            // 在默认参数求值后再次重新查找。注意：argValues 已基于旧 method 的 params 填充，
            // 若新 method 参数签名不同，后续绑定可能不匹配——但此为极端边界场景，
            // 重新查找至少保证 method 指针有效，避免 UAF 崩溃。
            {
                auto reIt = classRegistry_.find(searchClassName);
                if (reIt == classRegistry_.end()) {
                    runtimeError("类 " + searchClassName + " 在默认参数求值期间被重定义并删除", node.line, node.column);
                }
                searchClass = &reIt->second;
                method = findMethod(*searchClass, node.methodName);
                if (!method) {
                    runtimeError("类 " + searchClassName + " 在默认参数求值期间被重定义，方法 " + node.methodName +
                                     " 不再存在",
                                 node.line, node.column);
                }
                cachedParentEnv = searchClass->closureEnv;
            }

            // B3 fix: CallFrameGuard 自动管理 currentFunctionReturnType_ + callStack_ + classContextStack_
            CallFrameGuard frameGuard{*this, method->returnType, /*manageCtx=*/true};
            auto prevEnv = currentEnv_;

            std::shared_ptr<Environment> methodEnv; // 声明在try外，使catch后可访问
            Value result = Value::nullValue();

            // S2 fix: 统一使用 RecursionGuard RAII 管理递归深度
            if (recursionDepth_ + 1 >= MAX_RECURSION_DEPTH) {
                runtimeError(ErrorFormat::format("递归深度超过限制 (%d)", MAX_RECURSION_DEPTH), node.line, node.column);
            }
            RecursionGuard recursionGuard{recursionDepth_};

            // RA-A fix: RAII 守卫统一管理 methodEnv 的 closeCapturedVariables 与 currentEnv_ 恢复，
            // 消除原 catch(...) + throw; 的 rethrow。
            struct MethodEnvGuard {
                Interpreter& interp;
                std::shared_ptr<Environment>& env;
                std::shared_ptr<Environment>& prev;
                bool dismissed = false;
                ~MethodEnvGuard() {
                    if (!dismissed) {
                        if (env)
                            env->closeCapturedVariables();
                        interp.currentEnv_ = prev;
                    }
                }
            } envGuard{*this, methodEnv, prevEnv};

            try {
                // O5: 使用类定义时捕获的环境作为父级（闭包），而非调用者的环境
                auto parentEnv = cachedParentEnv ? cachedParentEnv : currentEnv_; // #2 fix: 使用缓存值
                methodEnv = std::make_shared<Environment>(parentEnv);

                // 绑定 this
                methodEnv->define("this", obj);

                // 绑定参数（参数覆盖同名字段）
                for (size_t i = 0; i < method->params.size(); ++i) {
                    // AUDIT-P1-ROUND50 fix: 绑定循环 argValues 越界保护。
                    // ROUND49 第二次重新查找（默认参数求值后）可能找到参数更多的新 method，
                    // 此时 argValues.size() < method->params.size()，越界访问 argValues[i] 是 UB。
                    // 对齐 constructClassInstance（InterpreterCalls.cpp L405-406）的越界保护。
                    Value argVal = (i < argValues.size()) ? std::move(argValues[i]) : Value::nullValue();
                    // P1-4 fix: 补充参数类型检查（与 callNamedFunction 一致）
                    if (i < method->paramTypes.size() && !method->paramTypes[i].empty()) {
                        checkType(
                            argVal, method->paramTypes[i],
                            [&] { return "方法 " + node.methodName + " 的参数 " + method->params[i]; }, node.line,
                            node.column);
                    }
                    methodEnv->define(method->params[i], std::move(argVal));
                }

                // H-新2 fix: bindInstance 必须在所有 define 之后，避免 map rehash 使指针悬空
                Value* thisInEnv = const_cast<Value*>(methodEnv->get("this"));
                if (thisInEnv)
                    methodEnv->bindInstance(thisInEnv);

                // 压入调用栈
                callStack_.emplace_back(obj.className() + "." + node.methodName, methodEnv, node.line, recursionDepth_);

                // 切换环境
                currentEnv_ = methodEnv;

                // 压入类上下文（super 解析用）
                // AUDIT-P1-CORRECT fix: 压入"方法实际定义所在类"而非"搜索起始类"。
                // 当中间类未定义方法时，findMethod 沿继承链向上找到祖先类的方法，
                // 但栈中压入中间类名会导致后续 super 调用从错误的类开始搜索，
                // 可能找到同一个方法形成无限递归（C←B←A，B 无 greet，C.greet 调用
                // super.greet 找到 A.greet，A.greet 的 super 又从 B 搜索再次找到 A.greet）。
                classContextStack_.push_back(findMethodDefiningClassName(*searchClass, node.methodName));

                // B1 fix: 直接在 methodEnv 中执行方法体，避免 visitBlock 创建嵌套块作用域
                // 导致 envPool_ 碰撞（与 callClosureValue 同理）。
                executeFunctionBody(static_cast<Block&>(*method->body));
                result = std::move(lastValue_);
            } catch (ReturnException& e) {
                result = std::move(e.returnValue);
            }
            // RA-A fix: 不再需要 catch(...) + throw; — envGuard 析构统一恢复

            // 从方法环境中读取 this 的更新值
            auto* thisPtr = methodEnv->get("this");
            Value updatedThis = thisPtr ? *thisPtr : Value::nullValue();

            // B1 fix: 关闭捕获 — 方法正常退出时将局部变量最终值写回闭包 capturedVars
            // RA-A fix: 由 envGuard 析构统一执行，此处 dismiss 避免重复
            envGuard.dismissed = true;
            if (methodEnv)
                methodEnv->closeCapturedVariables();
            currentEnv_ = prevEnv;

            // P0-4 fix: 不再调用 writeBack（会重复求值对象链），
            // 而是将更新后的 this 写回 obj（按引用传递），由 visitMethodCall 统一通过 writeBackChain 写回
            obj = std::move(updatedThis);

            // super.method() 调用后，需将更新后的 this 写回调用者的环境
            // （writeBackChain 无法处理 SuperExpr，因为它不是 VarRef）
            // P2-6 fix: obj 此后不再使用，使用 std::move 避免不必要的拷贝
            // AUDIT-P2 fix: Environment::set(name, Value&&) 契约要求调用方检查返回值：
            //   - true:  val 已 move 入目标
            //   - false: val 未 move（caller 仍持有所有权）
            // 原实现未检查，若 "this" 在 currentEnv_ 链中找不到（如 super 在顶层调用，
            // 理论不该发生但防御性处理），obj 被"悬空" move 语义不一致。改为检查并报错。
            if (isSuperCall) {
                if (!currentEnv_->set("this", std::move(obj))) {
                    runtimeError("super 调用无法写回 this：当前作用域链中未找到 this 变量", node.line, node.column);
                }
            }

            return result;
        }
    }

    runtimeError("类 " + obj.className() + " 没有方法 " + node.methodName, node.line, node.column);
}

void Interpreter::visitNullLiteral(NullLiteral& node) {
    checkBreak(&node);
    // null 字面量：返回 null 值（MiniLang 中用于表示"无值"，可赋给任意类型注解）。
    lastValue_ = Value::nullValue();
    return;
}
