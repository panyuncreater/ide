#include "interpreter/Interpreter.h"
#include "common/BoundsCheck.h"   // Dedup-7A: inBounds 替代重复的索引检查
#include "common/ErrorFormat.h"   // P3 fix: runtimeErrorFmt 替代 std::to_string 拼接
#include "common/ErrorMessages.h" // R97 #1: 三后端共享错误消息常量
#include "common/Logger.h"
#include "common/TCO.h" // B1 TCO: 与 VM 共享的尾调用识别（identifyTailCall 单一事实源）
#include "debug/DebugController.h"
#include "debug/ExecutionTraceRecorder.h" // R114: 可回放执行时间轴 recorder
#include "interpreter/BuiltinMethods.h"
#include "interpreter/NumericUtils.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include <cassert> // BUG-018 fix: numericBinaryOp 不可达断言
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
//   - 模块系统：import/export 见 InterpreterModules.cpp（路径安全、循环导入延迟加载、mtime 缓存失效）。
//
// 值表示、作用域链、引用计数与循环引用 GC 分别见 Value.h/NaNBox.h、Environment.h、
// RefCounted.h、GcManager.cpp：本文件在这些运行时基础设施之上组合出语言语义。
// ============================================================

Interpreter::Interpreter() : globalEnv_(std::make_shared<Environment>()), currentEnv_(globalEnv_), recursionDepth_(0) {
    outputCallback_ = std::make_shared<const std::function<void(const std::string&)>>([](const std::string&) {});
    // BUG-003 fix: 注册增量 GC 触发回调。GcManager 在 registerTracked 累计到阈值时
    // 调用此回调，由 Interpreter 收集当前根集并触发 collectCycle。
    // 生命周期说明：回调捕获 this 指针，必须在 ~Interpreter 中清除（见析构函数），
    // 否则后续其他后端（如 RegisterVM）COW detach 触发 checkIncrementalGc 时
    // 会调用悬垂 this 指针导致 UAF。
    // AUDIT-R3 P2-9 fix: 以 this 为 owner 令牌注册——多实例并存时后构造者覆盖
    // 前者回调，前者析构时仅清除属于自己的回调，不再误清存活实例的回调。
    GcManager::instance().setGcTriggerCallback([this]() { this->triggerIncrementalGc(); }, this);
}

Interpreter::~Interpreter() {
    // shared_ptr 自动管理环境生命周期，无需手动 delete
    // R157 fix: 清除 GcManager 单例中持有 this 的回调，避免悬垂 this 指针。
    // AUDIT-R3 P2-9 fix: 改用 clearGcTriggerCallbackIfOwner(this)——仅当回调仍属于
    // 本实例时才清除；若已被后构造的实例覆盖，不得清掉存活实例的回调
    // （原无条件 setGcTriggerCallback(nullptr) 会误清后构造者回调，使其丢失增量 GC）。
    // 安全性：析构仅在 execute() 返回后发生，此时无 collectCycle 在进行中。
    GcManager::instance().clearGcTriggerCallbackIfOwner(this);
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
    // R99: 清理 enum 注册表（与 classRegistry_ 同等处理）
    enumRegistry_.clear();
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
            // P0 fix: 语句边界安全触发延迟的增量 GC。
            // registerTracked 在容器构造期间仅设置 pendingIncrementalGc_ 标志，
            // 此处语句执行完毕、容器已加入 roots 后安全触发回收。
            GcManager::instance().checkPendingGc();
        }
    } catch (const ReturnException&) {
        runtimeError("return 只能在函数体内使用", 0, 0);
    } catch (const ThrowException& e) {
        // BUG-IBACKEND-4 fix: 三后端未捕获异常消息一致——统一 toString + 200 字符截断
        // （对齐 StackVM VM.cpp:130-132 与 RegisterVM）
        std::string str = e.thrownValue.toString();
        ErrorFormat::truncateForError(str);
        runtimeError("未捕获的异常: " + str, 0, 0);
    } catch (const RuntimeError& e) {
        // 记录到诊断包后重新抛出，保持原有异常传播机制
        // P2 fix (错误码优先匹配): 透传 RuntimeError 携带的稳定诊断码到 addError
        diagnostics_.addError(e.what(), e.line, e.column, DiagSource::Interpreter, e.code);
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
    // R99: 保存 enum 注册表（与 classRegistry_ 同等处理）
    replState_.savedEnumRegistry = std::move(enumRegistry_);
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
    // R99: 恢复 enum 注册表
    enumRegistry_ = std::move(replState_.savedEnumRegistry);
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
    enumRegistry_.clear(); // R99: 清理 enum 注册表
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

// ============================================================
// L18: Interpreter 状态快照与回滚（回溯调试 / reverse debugging）
// ============================================================

std::shared_ptr<Interpreter::StateSnapshot> Interpreter::captureStateSnapshot() const {
    auto snap = std::make_shared<StateSnapshot>();
    // 环境链结构（shared_ptr 共享所有权，保持 Environment 存活）
    snap->globalEnv = globalEnv_;
    snap->currentEnv = currentEnv_;
    snap->callStack = callStack_; // CallFrame 内部 shared_ptr<Environment> 拷贝

    // 环境链每层变量深拷贝（currentEnv -> parent -> ... -> globalEnv）
    // 必要性：live 执行会修改 Environment::variables（如 x = x + 1），
    // shared_ptr 仅保持对象存活，不隔离变量值。snapshotLocalVariables() 返回
    // variables map 的值拷贝，配合 Value 的 COW 语义实现快照隔离。
    auto env = currentEnv_;
    while (env) {
        snap->envChainVars.emplace_back(env, env->snapshotLocalVariables());
        env = env->parent;
    }

    // 注册表（shared_ptr 共享 AST 所有权）
    snap->funRegistry = funRegistry_;
    snap->funRegistryGen = funRegistryGen_;
    snap->classRegistry = classRegistry_;
    snap->classRegistryGen = classRegistryGen_;
    snap->enumRegistry = enumRegistry_;
    snap->moduleCache = moduleCache_;
    snap->moduleExports = moduleExports_;
    snap->moduleMtimes = moduleMtimes_;
    snap->moduleLoadingStack = moduleLoadingStack_;
    snap->moduleLoadingSet = moduleLoadingSet_;
    snap->exportedNames = exportedNames_;

    // 控制状态
    snap->recursionDepth = recursionDepth_;
    snap->classContextStack = classContextStack_;
    snap->currentFunctionReturnType = currentFunctionReturnType_;
    snap->currentTypeParams = currentTypeParams_;
    snap->loopFlow = static_cast<int>(loopFlow_);
    snap->currentCoroutineTargetYieldId = currentCoroutineTargetYieldId_;
    snap->currentYieldExecutionCount = currentYieldExecutionCount_;

    return snap;
}

bool Interpreter::restoreFromSnapshot(const StateSnapshot& snap) {
    if (!snap.currentEnv || !snap.globalEnv) {
        return false; // 快照无效
    }

    // 1. 恢复环境链结构（shared_ptr 拷贝替换 live 指针）
    globalEnv_ = snap.globalEnv;
    currentEnv_ = snap.currentEnv;
    callStack_ = snap.callStack;

    // 2. 恢复环境链每层变量（整表替换，移除快照后新增的变量，恢复被修改的值）
    //    restoreLocalVariables 会重新锚定 boundInstance_（H2 fix 不变量）
    for (const auto& [env, vars] : snap.envChainVars) {
        if (env) {
            env->restoreLocalVariables(vars);
        }
    }

    // 3. 恢复注册表
    funRegistry_ = snap.funRegistry;
    funRegistryGen_ = snap.funRegistryGen;
    classRegistry_ = snap.classRegistry;
    classRegistryGen_ = snap.classRegistryGen;
    enumRegistry_ = snap.enumRegistry;
    moduleCache_ = snap.moduleCache;
    moduleExports_ = snap.moduleExports;
    moduleMtimes_ = snap.moduleMtimes;
    moduleLoadingStack_ = snap.moduleLoadingStack;
    moduleLoadingSet_ = snap.moduleLoadingSet;
    exportedNames_ = snap.exportedNames;

    // 4. 恢复控制状态
    recursionDepth_ = snap.recursionDepth;
    classContextStack_ = snap.classContextStack;
    currentFunctionReturnType_ = snap.currentFunctionReturnType;
    currentTypeParams_ = snap.currentTypeParams;
    loopFlow_ = static_cast<LoopFlow>(snap.loopFlow);
    currentCoroutineTargetYieldId_ = snap.currentCoroutineTargetYieldId;
    currentYieldExecutionCount_ = snap.currentYieldExecutionCount;

    // 5. 清理诊断与中止标志（回滚后状态干净）
    diagnostics_.clear();
    stopRequested_.store(false, std::memory_order_relaxed);
    evaluationStepCount_ = 0;

    return true;
}

// R161 调试器 REPL 阶段 2：从源 Interpreter 复制函数/类/枚举注册表
void Interpreter::injectRegistriesFrom(const Interpreter& src) {
    // 复制函数注册表（shared_ptr<FunDecl> 共享所有权，AST 在 src 析构前有效）
    funRegistry_ = src.funRegistry_;
    funRegistryGen_ = src.funRegistryGen_;
    // 复制类注册表（ClassInfo.methods 也是 shared_ptr，共享所有权）
    classRegistry_ = src.classRegistry_;
    classRegistryGen_ = src.classRegistryGen_;
    // 复制 enum 注册表（EnumInfo 仅含字符串/向量，值拷贝安全）
    enumRegistry_ = src.enumRegistry_;
}

void Interpreter::setOutputCallback(std::function<void(const std::string&)> callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    // AUDIT-R4 P-3 fix: shared_ptr 包裹，使 output() 热路径锁内仅拷贝指针
    outputCallback_ = std::make_shared<const std::function<void(const std::string&)>>(std::move(callback));
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
    // AUDIT-R4 BUG-15 fix: debugger_ 改为 mutex 保护的 shared_ptr——setDebugger() 在主线程写入，
    // checkBreak()/visit* 在 worker 线程读取，原普通 shared_ptr 构成数据竞争（UB）。
    // 原方案用 std::atomic<std::shared_ptr>，但 macOS Clang libc++ 要求 T 满足
    // is_trivially_copyable（shared_ptr 不满足），编译失败。改用 mutex 保护：
    // debugMode_ atomic<bool> 作为热路径 fast-path 短路，仅调试模式启用时才进入加锁读。
    // 支持运行中动态附加/分离调试器（store nullptr 即分离，worker 下次 checkBreak
    // 读到 null 后跳过调试逻辑）。
    std::lock_guard<std::mutex> lock(debuggerMutex_);
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
        Value::DictMap newEntries; // R97 #2: 改用 DictMap（含 DictKeyEqual 透明比较器）
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

    // Phase 1: 状态保存（先做所有可能 throw 的拷贝，再安装沙箱）
    // Bug #13 fix: setupSandboxEnvironment 移到 SandboxGuard 构造之后，
    // 避免状态保存期间 bad_alloc 导致环境永久损坏。
    std::vector<EnvSnapshot> envSnaps;
    std::map<Value*, InstSnapEntry> instSnaps;

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
    // R99: 沙箱保存 enum 注册表，防止条件求值中 enum 声明污染主程序
    auto savedEnumRegistry = enumRegistry_;
    // Bug #76 fix: 保存协程字段，防止条件求值中协程状态污染主程序
    int savedCoroutineTargetYieldId = currentCoroutineTargetYieldId_;
    int savedYieldExecutionCount = currentYieldExecutionCount_;
    // AUDIT-R6 B2 fix: 沙箱期间抑制 output/input 回调。原实现条件表达式中的
    // print(...) 会真实输出到程序控制台（副作用逃逸沙箱），input() 会阻塞等待
    // 用户输入冻结调试会话。output() 对空回调静默、executeSharedInput 对空回调
    // 不阻塞，故置空即为抑制；由 SandboxGuard 析构加锁恢复。
    std::shared_ptr<const std::function<void(const std::string&)>> savedOutputCb;
    std::function<std::string(const std::string&)> savedInputCb;
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        savedOutputCb = outputCallback_;
        savedInputCb = std::move(inputCallback_);
        outputCallback_.reset();
        inputCallback_ = nullptr;
    }

    // RA-A fix: RAII 守卫统一管理沙箱状态恢复（实例字段 + 局部变量 + 调用栈/环境/深度/调试模式），
    // 消除原 catch(...) + throw; 的 rethrow。正常路径和异常路径恢复逻辑完全一致。
    struct SandboxGuard {
        Interpreter& interp;
        std::vector<EnvSnapshot>& envSnaps;
        std::map<Value*, InstSnapEntry>& instSnaps;
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
        std::unordered_map<std::string, EnumInfo> savedEnumRegistry; // R99
        int savedCoroutineTargetYieldId;                             // Bug #76
        int savedYieldExecutionCount;                                // Bug #76
        // AUDIT-R6 B2 fix: 沙箱抑制的 output/input 回调（析构加锁恢复）
        std::shared_ptr<const std::function<void(const std::string&)>> savedOutputCb;
        std::function<std::string(const std::string&)> savedInputCb;
        ~SandboxGuard() {
            // Phase 3: 沙箱状态恢复（实例字段 → 局部变量，H2 fix 不变量）
            interp.restoreSandboxState(envSnaps, instSnaps);
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
            interp.enumRegistry_ = std::move(savedEnumRegistry); // R99
            // Bug #76 fix: 恢复协程字段
            interp.currentCoroutineTargetYieldId_ = savedCoroutineTargetYieldId;
            interp.currentYieldExecutionCount_ = savedYieldExecutionCount;
            // AUDIT-R6 B2 fix: 恢复 output/input 回调（与 setOutputCallback/setInputCallback
            // 同锁，避免与 GUI 线程的回调安装竞争）
            {
                std::lock_guard<std::mutex> lock(interp.callbackMutex_);
                interp.outputCallback_ = std::move(savedOutputCb);
                interp.inputCallback_ = std::move(savedInputCb);
            }
            // AUDIT-P1-ROUND49 fix: 恢复 evaluationStepCount_ 为 0。
            // evaluateCondition 入口设 evaluationStepCount_=1，evaluate 中递增计数
            // 防止条件求值无限循环。SandboxGuard 析构恢复 16 种状态但遗漏此字段，
            // 返回后 evaluationStepCount_ 仍 > 0，后续正常执行每次 evaluate() 递增计数，
            // 累计超 MAX_CONDITION_STEPS 后抛虚假 RuntimeError，冻结主程序。
            interp.evaluationStepCount_ = 0;
        }
    } sandboxGuard{*this,
                   envSnaps,
                   instSnaps,
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
                   std::move(savedExportedNames),
                   std::move(savedEnumRegistry),
                   savedCoroutineTargetYieldId,
                   savedYieldExecutionCount,
                   std::move(savedOutputCb),
                   std::move(savedInputCb)};

    // Bug #13 fix: 沙箱环境安装移到所有状态保存和 SandboxGuard 构造之后。
    // 若 setupSandboxEnvironment 内部抛异常，SandboxGuard 析构仍能恢复已保存的状态。
    setupSandboxEnvironment(envSnaps, instSnaps);

    // Phase 2: 实际求值
    Value result = evalConditionExpr(node); // BUG-REPL-2 fix: 用拷贝而非 move，sandboxGuard 析构会恢复 lastValue_
    // RA-A fix: sandboxGuard 析构会统一恢复所有状态，无需手动还原
    return result;
}

void Interpreter::setupSandboxEnvironment(std::vector<EnvSnapshot>& envSnaps,
                                          std::map<Value*, InstSnapEntry>& instSnaps) {
    // #1 fix: 快照作用域链所有变量绑定（深拷贝容器）
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
}

Value Interpreter::evalConditionExpr(ASTNode* node) {
    // Phase 2: 实际求值——假定沙箱已就绪，状态保存/恢复由调用方负责。
    lastValue_ = Value::nullValue();
    node->accept(*this);
    return lastValue_;
}

void Interpreter::restoreSandboxState(std::vector<EnvSnapshot>& envSnaps, std::map<Value*, InstSnapEntry>& instSnaps) {
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
        // R97 #14 fix: 用 ErrorFormat::format 替代 std::to_string 拼接（避免 locale 查询 + 堆分配）
        runtimeError(ErrorFormat::formatStd("条件断点求值步数超过限制 ({})，可能存在无限循环",

                                            static_cast<int>(MAX_CONDITION_STEPS)),
                     0, 0);
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
    // R114 可回放执行时间轴：在每个语句节点入口采集快照。
    // 跨线程安全：recordingEnabled_ 为 atomic，recorder 内部 mutex 保护 deque。
    // 仅在 node 非空时采集（实际所有 checkBreak 调用都传 &node，但防御性检查）。
    // 注：此处采集的是"语句执行前"的状态快照，与 VM 路径"指令执行后"的快照
    // 语义略有差异——Interpreter 是 AST 节点级，VM 是字节码级。时间轴 UI 通过
    // backend 字段区分，回放时按后端类型展示不同的步进粒度。
    // R114 阶段 2: astNodePath 填充 "<nodeName>#<nodeId>"（nodeId 由 ASTNode 构造时分配），
    // 用于 UI 快速定位 AST 节点。
    if (recordingEnabled_.load(std::memory_order_relaxed) && node) {
        std::string path = node->nodeName();
        path += "#";
        path += std::to_string(node->nodeId);
        traceRecorder().captureInterpreterStep(*this, node->line, node->column, node->nodeName(), path);
    }
    if (debugMode_) {
        // AUDIT-R4 BUG-15 fix: atomic load 一次到局部变量，避免 TOCTOU + 减少原子操作
        auto dbg = debugger();
        if (dbg) {
            // BUG-DBG-1 fix: 更新调用栈顶帧行号为当前执行行号。
            // P-4 perf: 移入 debugMode_ 分支——非调试模式下 callStack_.line 无消费者，
            // 省去每语句一次条件判断 + 内存写入。recording 路径直接使用 node->line。
            if (node && node->line > 0 && !callStack_.empty()) {
                callStack_.back().line = node->line;
            }
            // 同步调用深度到调试控制器（Step Over 依赖此值判断是否进入函数）
            dbg->setCurrentDepth(recursionDepth_);
            dbg->checkBreak(node);
        }
    }
}

void Interpreter::output(const std::string& text) {
    // A6 fix: 加锁拷贝 callback 后解锁调用，避免持锁回调导致死锁
    // AUDIT-R4 P-3 fix: 拷贝 shared_ptr（refcount++，无堆分配）而非整个
    // std::function，消除每次 print 的闭包拷贝/潜在堆分配开销。
    std::shared_ptr<const std::function<void(const std::string&)>> cb;
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        cb = outputCallback_;
    }
    if (cb && *cb)
        (*cb)(text);
}

void Interpreter::runtimeError(const std::string& msg, int line, int col) {
    // P1-9 fix: 使用 ErrorFormat::formatWithLocation 替代 std::to_string + operator+，
    // 内部用 std::to_chars 写入栈缓冲区，零堆分配。
    Logger::Error(ErrorFormat::formatWithLocation(msg, line, col), "Interpreter");
    throw RuntimeError(msg, line, col);
}

void Interpreter::runtimeError(const std::string& msg, int line, int col, const std::string& diagCode) {
    // P2 fix (错误码优先匹配): 抛出携带稳定诊断码的 RuntimeError，
    // 由 runStatementsWithExceptionHandling 的 catch 块透传 code 到 addError。
    Logger::Error(ErrorFormat::formatWithLocation(msg, line, col), "Interpreter");
    throw RuntimeError(msg, line, col, diagCode);
}

const ClassInfo& Interpreter::lookupClassSafely(const std::string& name, const std::string& notFoundMsg, int line,
                                                int col) {
    // R97 #10 fix: 集中处理 classRegistry_.find + end() + runtimeError 三步模式。
    // 由于 runtimeError 是 [[noreturn]]，失败路径不会返回，调用方无需额外检查。
    auto it = classRegistry_.find(name);
    if (it == classRegistry_.end()) {
        runtimeError(notFoundMsg, line, col);
    }
    return it->second;
}

// ---- 数值二元运算 ----

// P1-2 fix: 4 个比较运算（LT/GT/LTE/GTE）共用模板，消除重复样板。
// 支持字符串字典序比较与数值比较，类型不匹配时抛 RuntimeError。
template <typename Cmp> Value Interpreter::compareNumericOrString(BinaryOp& node, Cmp /*cmp*/) {
    // W4 fix: cmp 运行时值未使用——仅其类型 Cmp 通过 std::is_same_v 分派 CompareOp。
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

// ============================================================
// 拓展二期·语言：运算符重载（instance 算术 dunder 分派）
// ------------------------------------------------------------
// 仿 invokeClosureSync 的简化执行模式：方法有类定义时的 closureEnv，
// 无快照重建问题。this 绑定后 bindInstance 使裸字段名可解析。
// 限制（与 VM/RegisterVM 一致）：不支持方法内 super 调用（不压
// classContextStack_）；方法必须恰好 1 个参数。
// ============================================================
bool Interpreter::tryOperatorOverload(BinOpType opType, Value& left, Value& right, int line, int col, Value& out) {
    if (!left.isInstance()) {
        return false;
    }
    const char* dunderName = nullptr;
    switch (opType) {
    case BinOpType::BIN_ADD:
        dunderName = "__add";
        break;
    case BinOpType::BIN_SUB:
        dunderName = "__sub";
        break;
    case BinOpType::BIN_MUL:
        dunderName = "__mul";
        break;
    case BinOpType::BIN_DIV:
        dunderName = "__div";
        break;
    case BinOpType::BIN_MOD:
        dunderName = "__mod";
        break;
    default:
        return false;
    }
    auto clsIt = classRegistry_.find(left.className());
    if (clsIt == classRegistry_.end()) {
        return false;
    }
    FunDecl* method = findMethod(clsIt->second, dunderName);
    if (!method) {
        return false;
    }
    if (method->params.size() != 1) {
        runtimeError(std::string("运算符方法 ") + dunderName + " 必须恰好接受 1 个参数", line, col);
    }

    if (recursionDepth_ + 1 >= MAX_RECURSION_DEPTH) {
        runtimeError(ErrorFormat::formatStd(ErrorMessages::kRecursionDepthExceededFmtStd, MAX_RECURSION_DEPTH), line,
                     col, DiagCodes::kRecursionDepth);
    }
    RecursionGuard recursionGuard{recursionDepth_};
    // 合并类泛型参数（与 invokeMethod 一致），方法体内 T 注解跳过校验
    std::vector<std::string> mergedTypeParams = clsIt->second.typeParams;
    for (const auto& tp : method->typeParams) {
        mergedTypeParams.push_back(tp);
    }
    CallFrameGuard frameGuard{*this, method->returnType, /*manageCtx=*/false, mergedTypeParams};

    auto prevEnv = currentEnv_;
    auto parentEnv = clsIt->second.closureEnv ? clsIt->second.closureEnv : currentEnv_;
    auto methodEnv = std::make_shared<Environment>(parentEnv);

    struct OpEnvGuard {
        Interpreter& interp;
        std::shared_ptr<Environment>& env;
        std::shared_ptr<Environment>& prev;
        bool popCallStack = false;
        ~OpEnvGuard() {
            if (popCallStack && !interp.callStack_.empty())
                interp.callStack_.pop_back();
            if (env)
                env->closeCapturedVariables();
            interp.currentEnv_ = prev;
        }
    } envGuard{*this, methodEnv, prevEnv};

    methodEnv->define("this", left);
    // 参数类型注解校验（与 invokeMethod 绑定循环一致）
    if (!method->paramTypes.empty() && !method->paramTypes[0].empty()) {
        checkType(
            right, method->paramTypes[0],
            [&] { return std::string("方法 ") + dunderName + " 的参数 " + method->params[0]; }, line, col);
    }
    methodEnv->define(method->params[0], right);
    // H-新2 fix 同模式：bindInstance 必须在全部 define 之后（避免 rehash 悬空）
    Value* thisInEnv = const_cast<Value*>(methodEnv->get("this"));
    if (thisInEnv) {
        methodEnv->bindInstance(thisInEnv);
    }

    callStack_.emplace_back(left.className() + "." + dunderName, methodEnv, line, recursionDepth_);
    envGuard.popCallStack = true;
    currentEnv_ = methodEnv;

    // 非蹦床上下文：运算符方法内 return this.__add(...) 不做 TCO
    TcoScopeGuard tcoGuard{*this, nullptr, std::string(), /*isMethod=*/false, /*enabled=*/false};
    try {
        executeFunctionBody(static_cast<Block&>(*method->body));
        out = std::move(lastValue_);
    } catch (ReturnException& e) {
        // 方法体 return 抵达此处（executeFunctionBody 不捕获 ReturnException，
        // 与 invokeClosureSync 的 catch 模式一致）
        out = std::move(e.returnValue);
    }
    return true;
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
    // 拓展二期·语言（运算符重载）：报错前先尝试 instance dunder 分派——
    // 左操作数为类实例且定义了 __add 等方法时调用之（与 VM/RegisterVM
    // 的同名分派点保持三后端一致）。
    if (!left.isNumber() || !right.isNumber()) {
        Value overloadResult;
        if (tryOperatorOverload(opType, left, right, line, col, overloadResult)) {
            return overloadResult;
        }
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
    // BUG-018 审计结论（2026-07-18）：ArithStatus 是 4 值封闭枚举（OK/DivByZero/
    // IntOverflow/NotNumeric，见 NumericUtils.h:78-83），switch 已穷举 + default 兜底。
    // runtimeError 已标记 [[noreturn]]（Interpreter.h:423），编译器知晓各 case 终止，
    // 不会发生 fall-through。但为防御性编程——若未来 [[noreturn]] 被移除或
    // runtimeError 改为非抛异常版本——此处显式注释无 break 的不变量。
    // 新增 ArithStatus 枚举值时，编译器在 -Wswitch-enum 下会发出未处理 case 警告，
    // 强制开发者在此添加对应 case；default 分支作为运行时兜底，报告"未知算术状态"。
    // BUG_REPORT 建议 [[fallthrough]] 是错误的——[[fallthrough]] 用于标注"有意"
    // 落空，而此处各 case 之间绝不应落空（runtimeError 抛异常后下一行不可达）。
    switch (r.status) {
    case ArithStatus::DivByZero:
        runtimeError("除零错误", line, col, DiagCodes::kDivisionByZero); // [[noreturn]] throws RuntimeError
    case ArithStatus::IntOverflow:
        runtimeError("整数运算溢出", line, col); // [[noreturn]] throws RuntimeError
    case ArithStatus::NotNumeric:
        runtimeError("算术运算需要数值类型", line, col); // [[noreturn]] throws RuntimeError
    case ArithStatus::OK:
        return r.isIntResult ? Value(r.intVal) : Value(r.floatVal);
    // Bug-6 同型修复：ArithStatus 是封闭枚举，落空表示内部错误。
    // 原代码 return Value::nullValue() 会让上层静默拿到 null 结果，
    // 改为抛出明确错误（与 VM.cpp:389 / RegisterVM.cpp:421 对齐）。
    default:
        runtimeError("内部错误: 未知算术状态", line, col); // [[noreturn]] throws RuntimeError
        // 运行时不可达——runtimeError 已标记 [[noreturn]]（Interpreter.h:514）。
        // 所有 case 均 return 或调用 [[noreturn]]，编译器可推导所有路径返回。
    }
}

// ---- 类型检查辅助方法 ----

// BUG-003 fix: 增量 GC 触发回调实现
// 收集当前 callStack_ + globalEnv_ + classRegistry_ 中的堆对象指针作为根集，
// 调用 collectCycle。本方法在 GcManager::registerTracked 累计达到阈值时被回调。
// 安全性：
//   - 单线程：与 execute() 入口的 collectCycle 相同，Interpreter 在 worker 线程执行
//   - 不会在 collectCycle 进行中被重入（GcManager::gcInProgress_ 守卫）
//   - 收集 roots 时若 callStack_ 被修改（如回调内构造函数），仅影响 roots 完整性，
//     不影响正确性（漏掉的 root 让本应存活的容器被误判为不可达，但 collectCycle
//     仅清空子元素不释放对象，refCount 仍 >0，对象仍存活，仅元素被清空——
//     这是 BUG-INT-1 fix 中已分析的"GC roots 不完整"风险，此处保守包含全部可能根）
void Interpreter::triggerIncrementalGc() {
    std::vector<const void*> gcRoots;
    // AUDIT-R3 P1-5 fix: 环境链全层遍历 helper——原实现仅收集各 env 本层变量
    // （snapshotLocalVariables 不含子块环境），块作用域变量/模块加载期环境
    // 中的活容器不在根集内，sweep 会静默清空其元素（数据损坏）。
    // 现沿 parent 链全层收集；重复根无害（mark 阶段 marked 集去重）。
    auto collectEnvChainRoots = [&gcRoots](const Environment* env) {
        int depth = 0;
        constexpr int MAX_CHAIN_DEPTH = 1024;
        while (env && depth++ < MAX_CHAIN_DEPTH) {
            auto vars = env->snapshotLocalVariables();
            for (const auto& var : vars) {
                const void* ptr = var.second.gcRootPtr();
                if (ptr)
                    gcRoots.push_back(ptr);
            }
            env = env->parent.get();
        }
    };
    // (0) AUDIT-R3 P1-5 fix: 当前环境链（覆盖块作用域/模块顶层执行期环境，
    //     链尾自然包含 globalEnv_ 或模块父环境）
    collectEnvChainRoots(currentEnv_.get());
    // (1) 全局环境顶层变量（currentEnv_ 链可能不经过 globalEnv_，保留）
    collectEnvChainRoots(globalEnv_.get());
    // (2) 调用栈各帧的环境变量（AUDIT-R3 P1-5 fix: 改为全链遍历，
    //     覆盖帧 env 的闭包父环境链）
    for (const auto& frame : callStack_) {
        collectEnvChainRoots(frame.env.get());
    }
    // (2b) AUDIT-R3 P1-5 fix: 模块缓存环境（含加载中的部分 env——P2-14 延迟
    //      加载会先入缓存再执行模块顶层，执行期分配的容器需可达）
    for (const auto& modPair : moduleCache_) {
        if (modPair.second) {
            collectEnvChainRoots(modPair.second.get());
        }
    }
    // (3) 类注册表中字段的默认值（可能持有循环容器）
    for (const auto& clsPair : classRegistry_) {
        for (const auto& fldPair : clsPair.second.fields) {
            const void* ptr = fldPair.second.gcRootPtr();
            if (ptr)
                gcRoots.push_back(ptr);
        }
    }
    // (4) REPL 暂存状态（若 active，savedGlobalEnv/savedClassRegistry 中的容器仍存活）
    if (replState_.active) {
        collectEnvChainRoots(replState_.savedGlobalEnv.get());
        for (const auto& clsPair : replState_.savedClassRegistry) {
            for (const auto& fldPair : clsPair.second.fields) {
                const void* ptr = fldPair.second.gcRootPtr();
                if (ptr)
                    gcRoots.push_back(ptr);
            }
        }
        // AUDIT-R3 P1-5 fix: 同步覆盖 REPL 暂存的模块缓存
        for (const auto& modPair : replState_.savedModuleCache) {
            if (modPair.second) {
                collectEnvChainRoots(modPair.second.get());
            }
        }
    }
    // Bug #43 fix: lastValue_ 可能持有容器对象，必须纳入 GC 根集。
    if (lastValue_.isPointer()) {
        const void* ptr = lastValue_.gcRootPtr();
        if (ptr)
            gcRoots.push_back(ptr);
    }
    GcManager::instance().collectCycle(gcRoots);
}

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
    // L4 fix: keyType 支持 string/int/bool/float 四种，valType 递归 typeMatch 每个值。
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
            // L4 fix: 键可能是 string/int/bool/float，按 keyType 匹配
            bool keyOk = false;
            if (keyType == "string")
                keyOk = std::holds_alternative<std::string>(kv.first);
            else if (keyType == "int")
                keyOk = std::holds_alternative<int64_t>(kv.first);
            else if (keyType == "bool")
                keyOk = std::holds_alternative<bool>(kv.first);
            else if (keyType == "float")
                keyOk = std::holds_alternative<double>(kv.first);
            if (!keyOk)
                return false;
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
    // R98 元组与解构：tuple 类型注解匹配任何元组
    // 结构化注解如 "(int,string)" 暂不支持（Parser 未解析元组类型语法）。
    if (annotation == TypeName::TUPLE)
        return val.isTuple();
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
    // R99 枚举与 ADT：enum variant 类型注解匹配
    // - "enum"：泛型注解，匹配任意 enum variant
    // - "<EnumName>"：精确 enum 名匹配（如 "Color" 匹配 Color.Red/Color.Green 等）
    if (annotation == TypeName::ENUM) {
        return val.isEnumVariant();
    }
    if (val.isEnumVariant()) {
        return val.enumVariantEnumName() == annotation;
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
        runtimeError("未定义的变量: " + info.varRef->name, line, col, DiagCodes::kUndefinedVariable);
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
            } else if (parent.isNull()) {
                // P2 fix (null-access): null 值成员访问给出明确的 null-access 诊断码
                runtimeError("不能在 null 值上访问属性或调用方法", line, col, DiagCodes::kNullAccess);
            } else {
                runtimeError(ErrorMessages::kTypeNotMemberAccessible, line, col);
            }
        } else if (nd->nodeType == NodeType::NODE_INDEX_ACCESS) {
            auto* ia = static_cast<IndexAccess*>(nd);
            info.idxs[i] = evaluate(ia->index.get());
            const Value& indexVal = info.idxs[i];
            if (parent.isArray() && indexVal.isInt()) {
                if (indexVal.intVal() < 0 ||
                    static_cast<size_t>(indexVal.intVal()) >= std::as_const(parent).arrayVal().size())
                    runtimeError(ErrorFormat::formatStd("数组索引越界: {}, 有效范围 [0, {})",

                                                        static_cast<long long>(indexVal.intVal()),

                                                        std::as_const(parent).arrayVal().size()),
                                 line, col, DiagCodes::kIndexOutOfBounds);
                info.vals[i] = std::as_const(parent).arrayVal()[indexVal.intVal()];
            } else if (parent.isDict()) {
                // L4 fix: 字典键支持 string/int/bool/float
                auto dk = Value::dictKeyFromValue(indexVal);
                if (!dk)
                    runtimeError(ErrorMessages::kDictKeyInvalidType, line, col, DiagCodes::kTypeMismatch);
                auto it = std::as_const(parent).dictVal().find(*dk);
                info.vals[i] = (it != std::as_const(parent).dictVal().end()) ? it->second : Value::nullValue();
            } else {
                runtimeError(ErrorMessages::kTypeNotIndexable, line, col, DiagCodes::kTypeMismatch);
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
                    // L4 fix: 字典 member-set 用 DictKey{string}
                    (*entries)[Value::DictKey{ma->fieldName}] = currentVal;
                } else {
                    parentVal.dictVal()[Value::DictKey{ma->fieldName}] = currentVal;
                }
            } else {
                runtimeError(ErrorMessages::kTypeNotMemberAssignable, line, col, DiagCodes::kTypeMismatch);
            }
        } else if (nd->nodeType == NodeType::NODE_INDEX_ACCESS) {
            const Value& indexVal = info.idxs[i];
            if (parentVal.isArray() && indexVal.isInt()) {
                if (indexVal.intVal() < 0 || static_cast<size_t>(indexVal.intVal()) >= parentVal.arrayVal().size())
                    runtimeError(ErrorFormat::formatStd("数组索引越界: {}, 有效范围 [0, {})",

                                                        static_cast<long long>(indexVal.intVal()),

                                                        parentVal.arrayVal().size()),
                                 line, col, DiagCodes::kIndexOutOfBounds);
                // S1 fix: 优先使用 tryGetMutableArray 跳过 COW 深拷贝
                if (auto* arr = parentVal.tryGetMutableArray()) {
                    (*arr)[indexVal.intVal()] = currentVal;
                } else {
                    parentVal.arrayVal()[indexVal.intVal()] = currentVal;
                }
            } else if (parentVal.isDict()) {
                // L4 fix: 字典键支持 string/int/bool/float
                auto dk = Value::dictKeyFromValue(indexVal);
                if (!dk)
                    runtimeError(ErrorMessages::kDictKeyInvalidType, line, col, DiagCodes::kTypeMismatch);
                // S1 fix: 优先使用 tryGetMutableDict 跳过 COW 深拷贝
                if (auto* entries = parentVal.tryGetMutableDict()) {
                    (*entries)[*dk] = currentVal;
                } else {
                    parentVal.dictVal()[*dk] = currentVal;
                }
            } else {
                runtimeError(ErrorMessages::kTypeNotIndexAssignable, line, col, DiagCodes::kTypeMismatch);
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
                runtimeError(ErrorFormat::formatStd("数组索引越界: {}, 有效范围 [0, {})",

                                                    static_cast<long long>(idx.intVal()),

                                                    std::as_const(modifiedObj).arrayVal().size()),
                             line, col, DiagCodes::kIndexOutOfBounds);
            modifiedObj.arrayVal()[idx.intVal()] = val;
        } else if (modifiedObj.isDict()) {
            // L4 fix: 字典键支持 string/int/bool/float
            auto dk = Value::dictKeyFromValue(idx);
            if (!dk)
                runtimeError(ErrorMessages::kDictKeyInvalidType, line, col);
            modifiedObj.dictVal()[*dk] = val;
        } else {
            runtimeError(ErrorMessages::kTypeNotIndexAssignable, line, col);
        }
    } else {
        if (modifiedObj.isInstance()) {
            modifiedObj.fields()[fieldName] = val;
        } else if (modifiedObj.isDict()) {
            // L4 fix: 字典 member-set 用 DictKey{string}
            modifiedObj.dictVal()[Value::DictKey{fieldName}] = val;
        } else {
            runtimeError(ErrorMessages::kTypeNotMemberAssignable, line, col);
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
        return;
    }
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

    // L19 Watchpoint（pre-execution 语义，与 VM OP_DEFINE_VAR 对齐）：
    // 变量声明也视为写入，在求值初始化表达式之前检查。
    // AUDIT-R4 BUG-15 fix: atomic load 到局部变量
    auto dbg = debugger();
    if (dbg && dbg->hasWatchpoints()) {
        dbg->checkWatchpointHit(node.name, false, "", node.line);
    }

    // Phase 1: 求值变量初始化表达式（含类类型自动构造）
    Value initVal = evalVarDeclValue(node);

    // Phase 2: 绑定变量到环境（含类型检查、类型注解记录、原子性 tryDefineNew）
    bindVarDecl(node, std::move(initVal));
}

Value Interpreter::evalVarDeclValue(VarDecl& node) {
    if (node.initializer) {
        return evaluate(node.initializer.get());
    }
    if (node.typeAnnotation.empty()) {
        return Value::nullValue();
    }

    // 无初始化表达式但有类型注解 — 检查是否是类名
    auto classIt = classRegistry_.find(node.typeAnnotation);
    if (classIt == classRegistry_.end()) {
        // 其他类型注解（int/float/bool/string/dict/array）无初始化则保持 null
        return Value::nullValue();
    }

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
        // R163 泛型扩展：合并类泛型参数 + init 方法泛型参数，使 init 体内的类型参数注解跳过类型校验
        std::vector<std::string> mergedTypeParams = cls.typeParams;
        for (const auto& tp : initMethod->typeParams) {
            mergedTypeParams.push_back(tp);
        }
        CallFrameGuard frameGuard{*this, initMethod->returnType, /*manageCtx=*/true, mergedTypeParams};
        callStack_.emplace_back(cls.name + ".init", initEnv, node.line, recursionDepth_ + 1);
        // AUDIT-P1-CORRECT fix: 压入"init 方法定义所在类"而非"实例类"。
        // 当实例类未定义 init 而继承父类的 init 时，压入实例类名会导致
        // init 体内的 super 调用从错误的类开始搜索（应为定义 init 的类的父类）。
        classContextStack_.push_back(findMethodDefiningClassName(cls, "init"));

        // #8 fix: recursionDepth_ guard for auto-construction
        // S2 fix: 统一使用 RecursionGuard RAII 管理递归深度
        if (recursionDepth_ + 1 >= MAX_RECURSION_DEPTH) {
            // P3-16 fix: 示范迁移——硬编码字面量改为 ErrorMessages 常量 + formatStd（std::format 风格）
            runtimeError(ErrorFormat::formatStd(ErrorMessages::kRecursionDepthExceededFmtStd, MAX_RECURSION_DEPTH),
                         node.line, node.column, DiagCodes::kRecursionDepth);
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
            // B1 TCO: init 构造路径非蹦床，禁用尾调用上下文
            TcoScopeGuard tcoGuard{*this, nullptr, std::string(), /*isMethod=*/false, /*enabled=*/false};
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

    return instance;
}

void Interpreter::bindVarDecl(VarDecl& node, Value initVal) {
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
}

void Interpreter::visitAssignment(Assignment& node) {
    checkBreak(&node);

    // L19 Watchpoint（pre-execution 语义，与 VM peekWriteTarget 对齐）：
    // 在 evaluate(node.value) 之前检查，用户看到的是写入前的旧值。
    // AUDIT-R4 BUG-15 fix: atomic load 到局部变量
    auto dbg = debugger();
    if (dbg && dbg->hasWatchpoints()) {
        dbg->checkWatchpointHit(node.name, false, "", node.line);
    }

    Value val = evaluate(node.value.get());

    // 类型检查
    const std::string* typeAnn = findTypeAnnotation(node.name);
    if (typeAnn) {
        checkType(val, *typeAnn, [&] { return std::string("赋值给 ") + node.name; }, node.line, node.column);
    }

    if (!currentEnv_->set(node.name, val)) {
        runtimeError("未定义的变量: " + node.name, node.line, node.column, DiagCodes::kUndefinedVariable);
    }
    lastValue_ = std::move(val);
    return;
}

void Interpreter::visitVarRef(VarRef& node) {
    checkBreak(&node);

    // C7: get() 返回指针，nullptr 表示变量未定义，消除 hasVariable() 双重遍历
    const Value* val = currentEnv_->get(node.name);
    if (!val) {
        runtimeError("未定义的变量: " + node.name, node.line, node.column, DiagCodes::kUndefinedVariable);
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
    // L7 fix: 改为读取 RuntimeConfig 运行时配置（教学场景可调）
    const int64_t dynMaxLoop = RuntimeLimits::RuntimeConfig::instance().maxLoopIterations();
    while (evaluate(node.condition.get()).isTruthy()) {
        // S-01 fix: 防止无限循环导致 DoS
        if (++iterationCount > dynMaxLoop) {
            runtimeError(
                ErrorFormat::formatStd("循环迭代次数超过上限 {}，疑似无限循环", static_cast<long long>(dynMaxLoop)),
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

    // PERF-07 fix: for 作用域 Environment 对象池复用（与 visitBlock 同模式）。
    // 优先从 envPool_ 取出已回收的 Environment 并 reset，避免 make_shared 堆分配。
    // forEnv 持有循环变量（如 i），退出时若未被闭包捕获（use_count==1）则回收至池。
    std::shared_ptr<Environment> forEnv;
    if (!envPool_.empty()) {
        forEnv = std::move(envPool_.back());
        envPool_.pop_back();
        forEnv->resetForReuse(currentEnv_);
    } else {
        forEnv = std::make_shared<Environment>(currentEnv_);
    }
    currentEnv_ = forEnv;

    // RA-A fix: RAII 守卫统一管理 forEnv 的 closeCapturedVariables 与 currentEnv_ 恢复，
    // 消除原 3 处 catch(...) + throw; 的 rethrow（初始化器、循环体 ReturnException、外层兜底）。
    // 守卫在正常路径和异常路径都执行清理，逻辑与原代码严格一致。
    // PERF-07: 异常路径不回收（与 visitBlock 一致），让 shared_ptr 自然销毁。
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
    // L7 fix: 改为读取 RuntimeConfig 运行时配置（教学场景可调）
    const int64_t dynMaxLoop = RuntimeLimits::RuntimeConfig::instance().maxLoopIterations();
    while (true) {
        // S-01 fix: 防止无限循环导致 DoS
        if (++iterationCount > dynMaxLoop) {
            runtimeError(
                ErrorFormat::formatStd("循环迭代次数超过上限 {}，疑似无限循环", static_cast<long long>(dynMaxLoop)),
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
    // PERF-07: 先记录是否有捕获（close 会清空列表），用于判断是否可回收。
    bool hadCaptures = forEnv->hasClosureCaptures();
    forEnv->closeCapturedVariables();
    currentEnv_ = forEnv->parent;

    // PERF-07: 若 forEnv 独占所有权（未被闭包/子作用域捕获），回收至池复用。
    // 判定条件与 visitBlock 一致：use_count==1 + 无捕获 + 无闭包 env weak_ptr 引用。
    if (forEnv.use_count() == 1 && !hadCaptures && !forEnv->hasClosureEnvRef()) {
        if (envPool_.size() < 64) {
            envPool_.push_back(std::move(forEnv));
        }
    }

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
        // R98 W3: 匿名 lambda（name 为空）不插入空名到作用域——空名无意义且可能
        // 干扰 isDefinedInScopes 判断（虽然空名不会匹配真实变量，但保持集合干净）
        if (!nestedFn.name.empty()) {
            nestedScopes.back().insert(nestedFn.name);
        }
        std::unordered_set<std::string> nestedFree;
        if (nestedFn.body)
            collectFreeVars(*nestedFn.body, nestedScopes, nestedFree);
        // 嵌套函数的自由变量若不在外层作用域定义，则为外层自由变量
        for (const auto& name : nestedFree) {
            if (!isDefinedInScopes(scopes, name)) {
                freeVars.insert(name);
            }
        }
        // 嵌套函数名是外层作用域的局部变量（匿名 lambda 跳过——不注册到外层作用域）
        if (!nestedFn.name.empty()) {
            scopes.back().insert(nestedFn.name);
        }
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
    // R98 W3: 匿名 lambda（node.name 为空）使用 `<lambda>` 作为 closureName，
    // 便于错误消息显示。匿名 lambda 不注册到 environment/funRegistry_，
    // 仅作为表达式求值结果（lastValue_）。
    bool isLambda = node.name.empty();
    std::string closureName = isLambda ? std::string("<lambda>") : node.name;
    Value funVal = Value::makeClosure(closureName, currentEnv_, node.params, nodeShared);

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

    // R98 W3: 匿名 lambda 不注册到 environment/funRegistry_——闭包值仅作为
    // 表达式求值结果（lastValue_）。注册到 environment 会导致 `define("", funVal)`
    // 污染作用域（空名变量），注册到 funRegistry_ 会导致空键覆盖。
    if (!isLambda) {
        currentEnv_->define(node.name, funVal);

        // 保留 funRegistry_ 作为后备（处理闭包 body 缺失的场景）
        // A3 fix: 存储 shared_ptr 而非裸指针
        funRegistry_[node.name] = nodeShared;
        funRegistryGen_++; // M7: 函数注册/重定义时递增代数，使旧缓存失效
    }

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
        // P0 fix: 语句边界安全触发延迟的增量 GC（同 runStatementsWithExceptionHandling）
        GcManager::instance().checkPendingGc();
    }
}

void Interpreter::visitReturnStmt(ReturnStmt& node) {
    checkBreak(&node);

    // B1 TCO: 自尾调用识别（与 VM 共享 TCO::identifyTailCall 单一事实源）。
    // 命中时求值实参后抛 TailCallSignal，由蹦床循环帧复用执行，不经 C++ 递归。
    // 中间轮次跳过返回值类型检查——最终非尾 return 的值即整个递归的返回值，
    // 类型在基例返回处检查一次，观测语义与逐层检查等价（同 VM TCO）。
    if (tcoEnabled_ && tcoTryDepth_ == 0 && tcoDecl_ && node.value) {
        auto info = TCO::identifyTailCall(&node, tcoName_, tcoIsMethod_);
        if (info.kind == TCO::TailCallInfo::Kind::SelfFunction) {
            // 运行时重绑定校验：局部同名变量可能遮蔽函数名（var f = other;），
            // 调用名当前必须仍解析到正在执行的 FunDecl 才可帧复用。
            const Value* callee = currentEnv_->get(info.call->name);
            if (callee && callee->isClosure() && callee->closureBody() == tcoDecl_) {
                size_t argCount = info.call->arguments.size();
                if (argCount >= static_cast<size_t>(tcoDecl_->requiredParamCount) &&
                    argCount <= tcoDecl_->params.size()) {
                    std::vector<Value> args;
                    args.reserve(argCount);
                    for (auto& a : info.call->arguments)
                        args.push_back(evaluate(a.get()));
                    throw TailCallSignal(std::move(args));
                }
            }
        } else if (info.kind == TCO::TailCallInfo::Kind::SelfMethod) {
            // 动态分派校验：沿 this 实际类的继承链解析 methodName 仍须命中
            // 当前 decl（子类 override 时回退普通调用，保持虚分派正确性）。
            const Value* thisVal = currentEnv_->get("this");
            if (thisVal && thisVal->isInstance()) {
                auto clsIt = classRegistry_.find(thisVal->className());
                if (clsIt != classRegistry_.end() &&
                    findMethod(clsIt->second, info.methodCall->methodName) == tcoDecl_) {
                    size_t argCount = info.methodCall->arguments.size();
                    if (argCount >= static_cast<size_t>(tcoDecl_->requiredParamCount) &&
                        argCount <= tcoDecl_->params.size()) {
                        std::vector<Value> args;
                        args.reserve(argCount);
                        for (auto& a : info.methodCall->arguments)
                            args.push_back(evaluate(a.get()));
                        throw TailCallSignal(std::move(args));
                    }
                }
            }
        } else if (info.kind == TCO::TailCallInfo::Kind::GeneralCall && !tcoIsMethod_ &&
                   currentFunctionReturnType_.empty()) {
            // L18 eng-tailcall: 互递归/一般尾调用 return g(args)。目标为普通
            // 函数闭包（非生成器/非类构造）时帧复用：蹦床循环切换目标 decl，
            // 信号携带目标闭包值供蹦床重建环境（closureEnv/capturedVars）。
            // 条件与 VM 路径对齐：非方法体/无返回类型注解；生成器/类构造/
            // 内建同名目标不抛信号，回退普通递归（VM 侧同样降级）。
            const Value* callee = currentEnv_->get(info.call->name);
            if (callee && callee->isClosure() && callee->closureBody() && callee->closureBody() != tcoDecl_ &&
                !callee->closureBody()->isGenerator && classRegistry_.find(info.call->name) == classRegistry_.end()) {
                FunDecl* targetDecl = callee->closureBody();
                size_t argCount = info.call->arguments.size();
                if (argCount >= static_cast<size_t>(targetDecl->requiredParamCount) &&
                    argCount <= targetDecl->params.size()) {
                    std::vector<Value> args;
                    args.reserve(argCount);
                    for (auto& a : info.call->arguments)
                        args.push_back(evaluate(a.get()));
                    throw TailCallSignal(std::move(args), callee->closureBodyShared(),
                                         const_cast<const Value&>(*callee).closureName(), *callee);
                }
            }
        }
    }

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
    // R104 Exception Breakpoint：throw 前检查异常断点（对齐 GDB `catch throw`）
    // AUDIT-R4 BUG-15 fix: atomic load 到局部变量
    if (auto dbg = debugger()) {
        dbg->checkExceptionBreakpoint(node.line);
    }
    Value val = evaluate(node.expression.get());
    throw ThrowException(std::move(val));
}

void Interpreter::visitTryStmt(TryStmt& node) {
    checkBreak(&node);
    // B1 TCO: try/catch/finally 全程禁用尾调用蹦床（return 需经 finally
    // 语义路径传播，与 VM 的 tryDepth_ == 0 判据一致）。RAII 保证异常路径配对递减。
    struct TcoTryDepthGuard {
        int& d;
        explicit TcoTryDepthGuard(int& dd) : d(dd) { ++d; }
        ~TcoTryDepthGuard() { --d; }
    } tcoTryGuard{tcoTryDepth_};
    // BUG-AUDIT-FINALLY-1: finally 块语义
    // - 正常退出（try/catch 正常完成）：执行 finally
    // - 异常退出（try/catch 抛出未捕获异常）：执行 finally 后 re-throw
    // - return：执行 finally 后再传播 ReturnException（L4 fix，对齐 Java/Python 主流语义）
    // - break/continue：三后端均执行 finally（AUDIT-P1.1 修复：Interpreter 走 loopFlow_
    //   状态标志，try 块正常完成后继续执行 finally；VM/RegisterVM 通过 pendingJumpStack_
    //   先 push 真实跳转目标再跳 finally 入口，OP_FINALLY_END 取出目标续跳）。
    //   原“三后端不一致”已不成立。
    // - try-finally（无 catch）：异常不被捕获，finally 执行后 re-throw
    bool finallyRun = false;
    // AUDIT-R3 P1-4 fix: finally 求值前保存并清除 loopFlow_，完成后恢复。
    // 根因：break/continue 用 loopFlow_ 状态标志实现，try 块内 break 后标志已
    // 置位；若不清除，finally 进入 visitBlock 后每条语句检查 loopFlow_ != None
    // 即中断——finally 块只执行第一条语句就退出，其余被静默跳过。
    // 语义：finally 自身产生的 break/continue 覆盖外层（新控制流优先）；
    // finally 抛异常时不恢复（异常优先于控制流转移，对齐 VM 路径
    // throwException 清空 pendingJumpStack_ 的语义）。
    auto runFinallyBlock = [&]() {
        LoopFlow savedFlow = loopFlow_;
        loopFlow_ = LoopFlow::None;
        evaluate(node.finallyBlock.get());
        if (loopFlow_ == LoopFlow::None)
            loopFlow_ = savedFlow;
    };
    try {
        if (!node.catchVarName.empty()) {
            // try-catch(-finally)：有 catch 子句，捕获异常
            try {
                if (node.tryBlock) {
                    evaluate(node.tryBlock.get());
                }
            } catch (ThrowException& e) {
                // 在 catch 块的新作用域中绑定异常变量
                // PERF-07 fix: catchEnv 对象池复用（与 visitBlock/visitForStmt 同模式）。
                std::shared_ptr<Environment> catchEnv;
                if (!envPool_.empty()) {
                    catchEnv = std::move(envPool_.back());
                    envPool_.pop_back();
                    catchEnv->resetForReuse(currentEnv_);
                } else {
                    catchEnv = std::make_shared<Environment>(currentEnv_);
                }
                auto savedEnv = currentEnv_;
                currentEnv_ = catchEnv;
                // P2-1 fix: 使用 std::move 避免不必要的 Value 拷贝
                currentEnv_->define(node.catchVarName, std::move(e.thrownValue));

                // RA-A fix: RAII 守卫统一管理 catchEnv 的 closeCapturedVariables 与 currentEnv_ 恢复，
                // 消除原 catch(...) + throw; 的 rethrow。
                // catch 块内若抛出 return/break/continue/throw，envGuard 析构恢复 catchEnv 后异常自然传播。
                // PERF-07: 异常路径不回收（与 visitBlock 一致），让 shared_ptr 自然销毁。
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
                // PERF-07: 先记录是否有捕获（close 会清空列表），用于判断是否可回收。
                bool hadCaptures = catchEnv->hasClosureCaptures();
                catchEnv->closeCapturedVariables();
                currentEnv_ = savedEnv;
                // PERF-07: 若 catchEnv 独占所有权则回收至池复用（判定条件同 visitBlock）。
                if (catchEnv.use_count() == 1 && !hadCaptures && !catchEnv->hasClosureEnvRef()) {
                    if (envPool_.size() < 64) {
                        envPool_.push_back(std::move(catchEnv));
                    }
                }
            } catch (const RuntimeError& e) {
                // L14: try/catch 捕获 runtimeError（如除零、索引越界、类型不匹配等）。
                // 将 RuntimeError 转换为 ThrowException（string Value 包含错误消息），
                // 复用 catch 块逻辑绑定 catchVar 并执行 catchBlock。
                // 对齐 StackVM/RegisterVM 的 runtimeError → throwException 转换。
                // 注意：RuntimeError 不设置 hasError_/diagnostics_（仅 Logger 记录），
                // 故被捕获后无需清理状态，与 Interpreter::runtimeError 不写 diagnostics_ 一致。
                // PERF-07 fix: catchEnv 对象池复用（与上方 ThrowException catch 同模式）。
                std::shared_ptr<Environment> catchEnv;
                if (!envPool_.empty()) {
                    catchEnv = std::move(envPool_.back());
                    envPool_.pop_back();
                    catchEnv->resetForReuse(currentEnv_);
                } else {
                    catchEnv = std::make_shared<Environment>(currentEnv_);
                }
                auto savedEnv = currentEnv_;
                currentEnv_ = catchEnv;
                currentEnv_->define(node.catchVarName, Value(std::string(e.what())));

                struct CatchEnvGuard {
                    Interpreter& interp;
                    std::shared_ptr<Environment>& env;
                    std::shared_ptr<Environment>& saved;
                    bool dismissed = false;
                    ~CatchEnvGuard() {
                        if (!dismissed) {
                            env->closeCapturedVariables();
                            interp.currentEnv_ = saved;
                        }
                    }
                } envGuard{*this, catchEnv, savedEnv};

                if (node.catchBlock) {
                    evaluate(node.catchBlock.get());
                }
                envGuard.dismissed = true;
                bool hadCaptures = catchEnv->hasClosureCaptures();
                catchEnv->closeCapturedVariables();
                currentEnv_ = savedEnv;
                if (catchEnv.use_count() == 1 && !hadCaptures && !catchEnv->hasClosureEnvRef()) {
                    if (envPool_.size() < 64) {
                        envPool_.push_back(std::move(catchEnv));
                    }
                }
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
            runFinallyBlock(); // AUDIT-R3 P1-4 fix: 统一走 loopFlow 保存/恢复包装
        } else {
            finallyRun = true;
        }
    } catch (const ThrowException&) {
        // throw 语句异常：执行 finally 后 re-throw
        if (!finallyRun && node.finallyBlock) {
            runFinallyBlock(); // AUDIT-R3 P1-4 fix
            // AUDIT-R6 B1 fix: finally 内的 break/continue 丢弃待处理异常（Java 式语义，
            // 三后端统一）。原实现无条件 rethrow 且 loopFlow_ 残留，外层 catch 块被
            // 截断（只执行第一条语句）后循环退出——既非异常优先也非控制流优先的
            // 损坏态。现在：loopFlow_ 被 finally 置位时吞掉异常，让 break/continue 生效
            // （finally 内 return 经 ReturnException 从 runFinallyBlock 传播，天然覆盖原异常）。
            if (loopFlow_ != LoopFlow::None) {
                lastValue_ = Value::nullValue();
                return;
            }
        }
        throw;
    } catch (const ReturnException&) {
        // L4 fix: return 时执行 finally（对齐 Java/Python 主流语义，三后端一致）
        // 原"return 跳过 finally"行为已被废弃，现在 return 在 try-finally 内时会先执行
        // finally 块再传播 ReturnException。finally 块内若抛出新异常（throw/return），
        // 新异常覆盖原 ReturnException（与 VM 路径 OP_FINALLY_END + OP_RETURN 一致）。
        if (!finallyRun && node.finallyBlock) {
            finallyRun = true;
            runFinallyBlock(); // AUDIT-R3 P1-4 fix
            // AUDIT-R6 B1 fix: finally 内的 break/continue 丢弃待传播的 return（Java 式语义）
            if (loopFlow_ != LoopFlow::None) {
                lastValue_ = Value::nullValue();
                return;
            }
        }
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
            runFinallyBlock(); // AUDIT-R3 P1-4 fix
            // AUDIT-R6 B1 fix: finally 内的 break/continue 丢弃待处理的运行时错误（Java 式语义）
            if (loopFlow_ != LoopFlow::None) {
                lastValue_ = Value::nullValue();
                return;
            }
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

// R98 元组与解构：元组字面量求值
// 语义：依次求值各 elements，组装为 immutable 元组 Value。
// 与数组区别：返回 VAL_TUPLE 类型，元素不可变，支持解构绑定。
void Interpreter::visitTupleLiteral(TupleLiteral& node) {
    checkBreak(&node);
    std::vector<Value> elements;
    elements.reserve(node.elements.size());
    for (auto& elem : node.elements) {
        elements.push_back(evaluate(elem.get()));
    }
    lastValue_ = Value::makeTuple(std::move(elements));
    return;
}

// R98 元组与解构：解构绑定 var (a, b, c) = expr
// 语义：求值 initializer（必须为 tuple），按位置绑定到 names 中的各变量名。
// 错误处理：
//   - initializer 非 tuple → runtimeError
//   - 元组元素数量与 names 不匹配 → runtimeError（严格匹配，不允许省略）
//   - 单个变量名重复 → 由 tryDefineNew 检测报错
void Interpreter::visitDestructureBinding(DestructureBinding& node) {
    checkBreak(&node);

    Value initVal = evaluate(node.initializer.get());

    if (!initVal.isTuple()) {
        runtimeError("解构绑定的右侧表达式必须是元组（得到 " + initVal.typeName() + "）", node.line, node.column);
    }

    const auto& tup = initVal.tupleVal();
    if (tup.size() != node.names.size()) {
        runtimeError(
            ErrorFormat::formatStd("解构绑定变量数 ({}) 与元组元素数 ({}) 不匹配", node.names.size(), tup.size()),
            node.line, node.column);
    }

    // 按位置依次绑定各变量到当前作用域
    for (size_t i = 0; i < node.names.size(); ++i) {
        // L20: per-name 类型注解运行时校验（与 VarDecl.bindVarDecl 语义对齐）
        const std::string& nameAnn = node.nameTypeAt(i);
        if (!nameAnn.empty()) {
            checkType(tup[i], nameAnn, [&] { return "解构变量 " + node.names[i] + " 的类型"; }, node.line, node.column);
            // 记录类型注解到当前作用域（与 VarDecl 一致，供后续赋值时校验）
            currentEnv_->defineTypeAnnotation(node.names[i], nameAnn);
        }
        // 解构绑定不预定义类型注解（tupleTypeAnnotation 暂未启用运行时检查）
        currentEnv_->unmarkCaptured(node.names[i]);
        if (!currentEnv_->tryDefineNew(node.names[i], tup[i])) {
            runtimeError("变量 '" + node.names[i] + "' 已在当前作用域中定义", node.line, node.column);
        }
    }

    // lastValue_ 设为整个元组（支持 var t = (var (a, b) = expr; t) 等链式场景）
    lastValue_ = std::move(initVal);
    return;
}

// ============================================================
// R99 枚举与 ADT + match 表达式
// ============================================================

// enum 声明：注册到 enumRegistry_，将 enum 名作为值绑定到当前环境。
// 注册的 EnumInfo 含 variant 列表与索引表，供后续 EnumVariantExpr/MatchExpr 查询。
// 重复声明同名 enum 覆盖旧定义（与 classRegistry_ 一致）。
void Interpreter::visitEnumDecl(EnumDecl& node) {
    checkBreak(&node);

    EnumInfo info;
    info.typeParams = node.typeParams;
    info.variants = node.variants;
    for (size_t i = 0; i < node.variants.size(); ++i) {
        info.variantIndex[node.variants[i].name] = i;
    }

    // 注册到 enumRegistry_
    enumRegistry_[node.name] = std::move(info);

    // 将 enum 名作为特殊的标记值绑定到当前环境（类似 class:Name 约定）
    // 这样 var x = EnumName; 不会引用到未定义变量。
    // 用 enum:前缀与 class:区分（也可用于运行时检查）。
    currentEnv_->define(node.name, Value(std::string("enum:") + node.name));

    lastValue_ = Value::nullValue();
    return;
}

// 枚举 variant 构造：EnumName.VariantName(arg1, arg2, ...)
// 语义：
//   1. 查找 enumRegistry_ 中的 EnumInfo
//   2. 验证 variant 存在且参数数量匹配
//   3. 构造 EnumVariantData 并返回 Value
void Interpreter::visitEnumVariantExpr(EnumVariantExpr& node) {
    checkBreak(&node);

    auto it = enumRegistry_.find(node.enumName);
    if (it == enumRegistry_.end()) {
        // AUDIT-R6 F9 fix: 补齐诊断码（StackVM/RegisterVM 同消息已带 undefined-function）
        runtimeError("未定义的 enum: " + node.enumName, node.line, node.column, "undefined-function");
    }
    const EnumInfo& info = it->second;

    auto varIt = info.variantIndex.find(node.variantName);
    if (varIt == info.variantIndex.end()) {
        // AUDIT-R6 F9 fix: 补齐诊断码（对齐 VM 侧）
        runtimeError("enum '" + node.enumName + "' 没有 variant '" + node.variantName + "'", node.line, node.column,
                     "undefined-function");
    }
    const EnumVariant& varInfo = info.variants[varIt->second];

    // 求值构造参数
    std::vector<Value> fields;
    fields.reserve(node.arguments.size());
    for (auto& arg : node.arguments) {
        fields.push_back(evaluate(arg.get()));
    }

    // 参数数量校验
    if (fields.size() != varInfo.paramTypes.size()) {
        runtimeError(ErrorFormat::formatStd("enum variant '{}.{}' 期望 {} 个参数，得到 {} 个", node.enumName,

                                            node.variantName, varInfo.paramTypes.size(), fields.size()),
                     node.line, node.column, DiagCodes::kArityMismatch);
    }

    // 类型注解校验（与 visitVarDecl 的 typeAnnotation 一致：宽松匹配）
    // 对每个 paramType 进行 typeMatch 检查，类型不匹配报错。
    // 注意：泛型类型参数运行时擦除（无法校验），仅校验具体类型。
    for (size_t i = 0; i < fields.size(); ++i) {
        const std::string& expectedType = varInfo.paramTypes[i];
        if (!expectedType.empty() && !isTypeParameter(expectedType, info.typeParams)) {
            if (!typeMatch(fields[i], expectedType)) {
                // AUDIT-R6 F9 fix: 补齐 kTypeMismatch 诊断码（VM 侧 F3 新增同码校验，三后端一致）
                runtimeError(ErrorFormat::formatStd("enum variant '{}.{}' 第 {} 个参数类型不匹配：期望 {}，得到 {}",

                                                    node.enumName, node.variantName, i + 1,

                                                    expectedType, fields[i].typeName()),
                             node.line, node.column, DiagCodes::kTypeMismatch);
            }
        }
    }

    lastValue_ = Value::makeEnumVariant(node.enumName, node.variantName, std::move(fields));
    return;
}

// match 表达式：求值 scrutinee，按顺序匹配 cases，执行第一个匹配的 case 体。
// 匹配语义（R134 扩展：递归 pattern 匹配 + guard）：
//   - WILDCARD：匹配任意值，不绑定
//   - LITERAL：与 scrutinee 相等（用 Value::equals）
//   - VARIANT：scrutinee 必须是 enum variant，enum 名 + variant 名必须匹配；
//              子 pattern 数量必须与字段数一致，递归匹配每个字段。
//   - VARIABLE：匹配任意值，绑定整个 scrutinee 到 variableName 变量
//   - TUPLE：scrutinee 必须是 tuple，元素数必须与子 pattern 数一致，递归匹配每个元素
//   - OR：依次尝试每个子 pattern，任一匹配则成功（绑定副作用回滚到失败的子 pattern）
//   - default：匹配任意值（永远匹配）
//   - guard：pattern 匹配成功后求值 guard 表达式，guard 为 false 视为不匹配继续下一 case
// 未匹配任何 case 时 runtimeError。
void Interpreter::visitMatchExpr(MatchExpr& node) {
    checkBreak(&node);

    Value scrutinee = evaluate(node.scrutinee.get());

    // PERF-07 fix: case 作用域 Environment 对象池复用（与 visitBlock/visitForStmt 同模式）。
    // match 不调用 closeCapturedVariables（case 体返回即结束，无后续修改需要写回），
    // 仅需在丢弃 caseEnv 前检查可回收性。recycleCaseEnv lambda 统一 4 处回收点。
    auto recycleCaseEnv = [this](std::shared_ptr<Environment>& env) {
        // 判定条件同 visitBlock：use_count==1 + 无捕获 + 无闭包 env weak_ptr 引用。
        // match case 不调用 closeCapturedVariables，故无 hadCaptures 前置记录——
        // 若有闭包捕获（registerClosureCapture 被调用），hasClosureCaptures() 仍为 true，
        // 且 hasClosureEnvRef() 在闭包创建时被标记，两者均阻止回收，语义安全。
        if (env && env.use_count() == 1 && !env->hasClosureCaptures() && !env->hasClosureEnvRef()) {
            if (envPool_.size() < 64) {
                envPool_.push_back(std::move(env));
            }
        }
    };

    // 为每个 case 创建独立作用域（绑定变量不污染外层）
    // 使用临时 childEnv 评估 case 体，匹配失败则丢弃。
    for (auto& mc : node.cases) {
        // default case
        if (mc.isDefault || !mc.pattern) {
            std::shared_ptr<Environment> caseEnv;
            if (!envPool_.empty()) {
                caseEnv = std::move(envPool_.back());
                envPool_.pop_back();
                caseEnv->resetForReuse(currentEnv_);
            } else {
                caseEnv = std::make_shared<Environment>(currentEnv_);
            }
            // R134: default case 也可能有 guard
            if (mc.guard) {
                auto savedEnv = currentEnv_;
                currentEnv_ = caseEnv;
                Value guardVal = evaluate(mc.guard.get());
                currentEnv_ = savedEnv;
                if (!guardVal.isTruthy()) {
                    recycleCaseEnv(caseEnv); // PERF-07: guard 失败，回收 caseEnv
                    continue;                // guard 为 false，继续下一 case
                }
            }
            auto savedEnv = currentEnv_;
            currentEnv_ = caseEnv;
            Value result = evaluateMatchBody(mc.body.get());
            currentEnv_ = savedEnv;
            recycleCaseEnv(caseEnv); // PERF-07: 命中并执行完 body，回收 caseEnv
            lastValue_ = std::move(result);
            return;
        }

        const MatchPattern& p = *mc.pattern;
        std::shared_ptr<Environment> caseEnv;
        if (!envPool_.empty()) {
            caseEnv = std::move(envPool_.back());
            envPool_.pop_back();
            caseEnv->resetForReuse(currentEnv_);
        } else {
            caseEnv = std::make_shared<Environment>(currentEnv_);
        }

        bool matched = tryMatchPattern(p, scrutinee, *caseEnv);

        // R134: guard 求值（pattern 匹配成功后）
        if (matched && mc.guard) {
            auto savedEnv = currentEnv_;
            currentEnv_ = caseEnv;
            Value guardVal = evaluate(mc.guard.get());
            currentEnv_ = savedEnv;
            matched = guardVal.isTruthy();
        }

        if (matched) {
            auto savedEnv = currentEnv_;
            currentEnv_ = caseEnv;
            Value result = evaluateMatchBody(mc.body.get());
            currentEnv_ = savedEnv;
            recycleCaseEnv(caseEnv); // PERF-07: 命中并执行完 body，回收 caseEnv
            lastValue_ = std::move(result);
            return;
        }
        recycleCaseEnv(caseEnv); // PERF-07: 未匹配，回收 caseEnv
    }

    // 所有 case 均未匹配
    // L6 fix: 简化消息为固定文本（不含 scrutinee 类型），与 StackVM/IR 路径的 OP_THROW
    // 抛出的字符串保持完全一致，确保三后端错误消息文本统一。
    (void)scrutinee; // 避免未使用变量警告
    runtimeError("match 表达式没有匹配的 case", node.line, node.column);
}

// R164 协程/生成器：yield 表达式求值（Interpreter 重放模式）
// ============================================================
// 重放模式核心思路：
//   - 生成器函数调用时，Interpreter 不直接执行函数体，而是返回 Coroutine 值
//   - 每次调用 .next() 时，从头重新执行函数体，用运行时 yield 执行计数器
//     （currentYieldExecutionCount_）跳过已返回的 yield，到达目标时返回
//   - 当 currentYieldExecutionCount_ == currentCoroutineTargetYieldId_ 时，
//     求值 yield 表达式的 value 并通过 YieldSignal 异常返回
//   - 当 currentYieldExecutionCount_ < currentCoroutineTargetYieldId_ 时，跳过该 yield
//   - 当所有 yield 都已耗尽时，函数体自然结束，.done() 返回 true
//
// 为什么用运行时计数器而非编译期 yieldId？
//   编译期 yieldId 对循环内的 yield 节点只分配一次（如 while 循环内的 yield i），
//   但运行时该节点可能执行 N 次（每次循环迭代）。若用编译期 yieldId 比较，
//   则循环内 yield 永远匹配同一目标，无法正确重放。
//   运行时计数器在每次 visitYieldExpr 调用时递增，正确区分同一节点的多次执行。
//
// 不变量：currentCoroutineTargetYieldId_ >= 0 表示当前在协程重放上下文中。
// 普通函数执行期间此字段为 -1，visitYieldExpr 在非协程上下文被调用属于语义错误。
// ============================================================
void Interpreter::visitYieldExpr(YieldExpr& node) {
    if (currentCoroutineTargetYieldId_ < 0) {
        // 防御性检查：yield 在非生成器函数体中出现（Parser 已在 primary() 拦截，
        // 但闭包路径或动态构造的 AST 可能绕过——此处兜底报错而非崩溃）
        runtimeError("yield 只能在 fun* 生成器函数体内出现", node.line, node.column);
    }

    // 运行时 yield 执行计数：每次 visitYieldExpr 调用递增
    // 用于区分循环内同一 yield 节点的多次执行（编译期 yieldId 无法区分）
    int thisExecutionId = currentYieldExecutionCount_++;

    // 求值 yield 表达式的值（无 value 时为 null，如 `yield;`）
    Value yieldValue = node.value ? evaluate(node.value.get()) : Value::nullValue();

    if (thisExecutionId == currentCoroutineTargetYieldId_) {
        // 命中目标 yield：抛出 YieldSignal 携带值，被 callCoroutineNext 捕获
        throw YieldSignal(std::move(yieldValue));
    }
    // thisExecutionId < currentCoroutineTargetYieldId_：跳过此 yield（重放模式核心）
    // 不可能 thisExecutionId > currentCoroutineTargetYieldId_（计数器从 0 单调递增，
    // callCoroutineNext 设置 target 后第一次遇到 yield 必然 thisExecutionId==target 或更小）
    // 跳过的 yield 仍要求值其表达式以保留副作用语义（与 StackVM 真挂起模式一致：
    // 真挂起模式下 yield 表达式只在挂起点求值一次；重放模式下每次重放都会重新求值，
    // 用户不应在 yield 表达式中放置有副作用的操作——这是重放模式的已知限制）
    lastValue_ = std::move(yieldValue);
}

// R134 模式匹配扩展：递归 pattern 匹配 helper。
// 在 caseEnv 中绑定变量；匹配失败时已绑定的变量留在 caseEnv 中（caseEnv 会被调用方丢弃，无需手动清理）。
// 递归语义：
//   - WILDCARD：永真
//   - LITERAL：Value::equals 字面量
//   - VARIABLE：永真 + 绑定整个 scrutinee 到 variableName
//   - VARIANT：enum variant 名匹配 + 子 pattern 递归匹配字段
//   - TUPLE：tuple 类型 + 元素数匹配 + 子 pattern 递归匹配元素
//   - OR：依次尝试子 pattern，第一个成功的为准（任一成功即整体成功）
bool Interpreter::tryMatchPattern(const MatchPattern& p, const Value& scrutinee, Environment& caseEnv) {
    switch (p.kind) {
    case MatchPatternKind::WILDCARD:
        return true;
    case MatchPatternKind::LITERAL: {
        Value litVal = evaluate(p.literal.get());
        return scrutinee.equals(litVal);
    }
    case MatchPatternKind::VARIABLE: {
        // 绑定整个 scrutinee 到 variableName
        caseEnv.unmarkCaptured(p.variableName);
        if (!caseEnv.tryDefineNew(p.variableName, scrutinee)) {
            runtimeError("match 绑定变量 '" + p.variableName + "' 已在 case 作用域中定义", p.line, p.column);
        }
        return true;
    }
    case MatchPatternKind::VARIANT: {
        if (!scrutinee.isEnumVariant()) {
            return false;
        }
        if (scrutinee.enumVariantEnumName() != p.enumName || scrutinee.enumVariantName() != p.variantName) {
            return false;
        }
        const auto& fields = scrutinee.enumVariantFields();
        // 子 pattern 数量必须等于字段数（无子 pattern = 0 字段 variant，匹配 0 字段）
        if (p.subPatterns.size() != fields.size()) {
            runtimeError(ErrorFormat::formatStd("match variant 模式 '{}.{}' 子 pattern 数 ({}) 与字段数 ({}) 不匹配",

                                                p.enumName, p.variantName, p.subPatterns.size(),

                                                fields.size()),
                         p.line, p.column);
        }
        // 递归匹配每个字段
        for (size_t i = 0; i < fields.size(); ++i) {
            if (!tryMatchPattern(*p.subPatterns[i], fields[i], caseEnv)) {
                return false;
            }
        }
        return true;
    }
    case MatchPatternKind::TUPLE: {
        if (!scrutinee.isTuple()) {
            return false;
        }
        const auto& elems = scrutinee.tupleVal();
        if (p.subPatterns.size() != elems.size()) {
            return false; // 元素数不匹配
        }
        for (size_t i = 0; i < elems.size(); ++i) {
            if (!tryMatchPattern(*p.subPatterns[i], elems[i], caseEnv)) {
                return false;
            }
        }
        return true;
    }
    case MatchPatternKind::OR: {
        // 依次尝试子 pattern，第一个成功的为准
        // AUDIT-R7 F4 fix: 每个备选先在临时环境试绑，成功后再合并入 caseEnv。
        // 原实现失败备选的部分绑定残留在 caseEnv，后续备选绑定同名变量时
        // tryDefineNew 拒绝重名 → 误报"绑定变量已在 case 作用域中定义"（实证：
        // `case P.A(x, 2) or P.A(1, x)` 对 P.A(1,7) Interpreter 报错而 VM 路径返回 7）。
        for (auto& sub : p.subPatterns) {
            Environment trialEnv(nullptr);
            if (tryMatchPattern(*sub, scrutinee, trialEnv)) {
                for (auto& [name, val] : trialEnv.snapshotLocalVariables()) {
                    caseEnv.unmarkCaptured(name);
                    if (!caseEnv.tryDefineNew(name, val)) {
                        runtimeError("match 绑定变量 '" + name + "' 已在 case 作用域中定义", p.line, p.column);
                    }
                }
                return true;
            }
            // 失败备选的绑定随 trialEnv 丢弃，不污染 caseEnv
        }
        return false;
    }
    }
    return false; // 不可达
}

// match case 体的求值：body 可能是 Block 或单表达式。
// 若为 Block，执行其中所有语句，返回最后一条语句的值（与 lambda 体一致）。
// 若为单表达式，直接求值返回。
Value Interpreter::evaluateMatchBody(ASTNode* body) {
    if (!body) {
        return Value::nullValue();
    }
    if (body->nodeType == NodeType::NODE_BLOCK) {
        // 执行 Block 中所有语句，返回 lastValue_
        // 注意：不能直接调用 visitBlock（它会创建新的 child env），
        // 因为我们已经在 case 作用域中。需手动遍历。
        Block* blk = static_cast<Block*>(body);
        Value last = Value::nullValue();
        for (auto& stmt : blk->statements) {
            last = evaluate(stmt.get());
            // break/continue 在 match 体中视为循环控制（与 lambda 体一致）
            if (loopFlow_ != LoopFlow::None) {
                break;
            }
        }
        return last;
    }
    return evaluate(body);
}

// 辅助：判断类型名是否是 enum 的类型参数（如 T、E、K、V）
// 用于 visitEnumVariantExpr 的类型校验——类型参数运行时擦除，跳过校验。
bool Interpreter::isTypeParameter(const std::string& typeName, const std::vector<std::string>& typeParams) const {
    for (const auto& tp : typeParams) {
        if (typeName == tp) {
            return true;
        }
    }
    return false;
}

void Interpreter::visitDictLiteral(DictLiteral& node) {
    checkBreak(&node);
    // L4 fix: 字典字面量——键支持 string/int/bool/float 四种类型。
    // 原 B6 fix 强制 string 键已废弃；非 string/int/bool/float 键（如 array/dict/instance）报错。
    // R97 #2 fix: 改用 Value::DictMap（含 DictKeyEqual 透明比较器）
    Value::DictMap dict;
    dict.reserve(node.pairs.size());
    for (auto& pair : node.pairs) {
        Value key = evaluate(pair.first.get());
        Value val = evaluate(pair.second.get());
        auto dk = Value::dictKeyFromValue(key);
        if (!dk) {
            runtimeError(ErrorMessages::kDictKeyInvalidType, node.line, node.column, DiagCodes::kTypeMismatch);
        }
        dict.emplace(std::move(*dk), std::move(val));
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
            runtimeError(ErrorMessages::kArrayIndexMustBeInt, node.line, node.column, DiagCodes::kTypeMismatch);
        }
        int64_t i = idx.intVal();
        const auto& arr = objC.arrayVal();
        if (i < 0 || static_cast<size_t>(i) >= arr.size()) {
            runtimeError(
                ErrorFormat::formatStd("数组索引越界: {}, 有效范围 [0, {})", static_cast<long long>(i), arr.size()),
                node.line, node.column, DiagCodes::kIndexOutOfBounds);
        }
        lastValue_ = arr[static_cast<size_t>(i)];
        return;
    }

    // 字典索引访问
    if (objC.isDict()) {
        // L4 fix: 字典键支持 string/int/bool/float
        auto dk = Value::dictKeyFromValue(idx);
        if (!dk) {
            runtimeError(ErrorMessages::kDictKeyInvalidType, node.line, node.column, DiagCodes::kTypeMismatch);
        }
        const auto& dict = objC.dictVal();
        auto it = dict.find(*dk);
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
            runtimeError(ErrorMessages::kStringIndexMustBeInt, node.line, node.column);
        }
        int64_t i = idx.intVal();
        const std::string& s = objC.stringVal();

        // #13 fix: ASCII 快速路径（镜像 VM 的 P7 fix）。
        // 纯 ASCII 字符串每码位 1 字节，可直接按字节索引 O(1)。
        // R97 #3 fix: 用 StringData 内缓存的 isAscii 标志替代 lastAsciiStr* 4 字段缓存。
        // isAscii 与字符串生命周期绑定，O(1) 读取，无堆地址复用误命中风险。
        // 原实现每次访问都 O(i) 扫描到目标码位，循环退化 O(n²)。
        if (BoundsCheck::inBounds(i, s.size()) && objC.isAsciiString()) {
            lastValue_ = Value(s.substr(static_cast<size_t>(i), 1));
            return;
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
            runtimeError(ErrorFormat::formatStd("字符串索引越界: {}, 有效范围 [0, {})", static_cast<long long>(i),

                                                static_cast<long long>(charCount)),
                         node.line, node.column, DiagCodes::kIndexOutOfBounds);
        }
        lastValue_ = Value(s.substr(targetBytePos, targetByteLen));
        return;
    }

    // R98 元组与解构：元组索引访问（按位置返回元素，与数组语义一致但 immutable）
    if (objC.isTuple()) {
        if (!idx.isInt()) {
            runtimeError("元组索引必须是整数", node.line, node.column);
        }
        int64_t i = idx.intVal();
        const auto& tup = objC.tupleVal();
        if (i < 0 || static_cast<size_t>(i) >= tup.size()) {
            // AUDIT-R5 BUG-08 fix: 补传 kIndexOutOfBounds 诊断码，对齐本文件数组/字符串
            // 越界路径与 RegisterVM 元组越界路径（RegisterVMExec.cpp REG_LOAD_INDEX）。
            runtimeError(
                ErrorFormat::formatStd("元组索引越界: {}, 有效范围 [0, {})", static_cast<long long>(i), tup.size()),
                node.line, node.column, DiagCodes::kIndexOutOfBounds);
        }
        lastValue_ = tup[static_cast<size_t>(i)];
        return;
    }

    runtimeError(ErrorMessages::kTypeNotIndexable, node.line, node.column);
}

void Interpreter::visitIndexAssign(IndexAssign& node) {
    checkBreak(&node);
    // L19 Watchpoint（pre-execution 语义，与 VM OP_INDEX_SET 对齐）：
    // 索引写入视为修改变量本身（与 VmStepper::checkWatchpointHit 语义一致）。
    // 仅当 node.object 是简单 VarRef 时检查根变量名，复杂链式访问跳过（避免副作用）。
    // AUDIT-R4 BUG-15 fix: atomic load 到局部变量
    auto dbg = debugger();
    if (dbg && dbg->hasWatchpoints() && node.object->nodeType == NodeType::NODE_VAR_REF) {
        dbg->checkWatchpointHit(static_cast<VarRef*>(node.object.get())->name, false, "", node.line);
    }
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

    // ---- R136 同步对象方法（channel/mutex/rwlock/thread）----
    // 同步对象内部状态通过 shared_ptr<Inner> 共享，方法调用不修改 Value 本身
    // （objectModified=false），无需 writeBack
    if (obj.isChannel() || obj.isMutex() || obj.isRwLock() || obj.isThread()) {
        auto argValues = evaluateArguments(node.arguments);
        auto builtinResult = handleSyncObjectMethod(node.methodName, obj, argValues, node.line, node.column);
        lastValue_ = std::move(builtinResult.result);
        return;
    }

    // ---- R164 协程/生成器方法（.next() / .done()）----
    // .next() 修改协程内部状态（currentYieldId/done/currentValue），返回 objectModified=true
    // 触发 writeBackChain 将修改写回变量引用（如 `var g = gen(); g.next();`）
    if (obj.isCoroutine()) {
        auto argValues = evaluateArguments(node.arguments);
        auto builtinResult = handleCoroutineMethod(node.methodName, obj, argValues, node.line, node.column);
        if (builtinResult.objectModified && info.varRef) {
            writeBackChain(info, std::move(obj), node.line, node.column);
        }
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

    // P2 fix (null-access): null 值方法调用给出明确的 null-access 诊断码，
    // 供 ErrorHintEngine 按 code 精确匹配教学提示（而非依赖子串匹配）。
    if (obj.isNull()) {
        runtimeError("不能在 null 值上访问属性或调用方法", node.line, node.column, DiagCodes::kNullAccess);
    }
    // 2026-06-29 BUG-1 fix (与 StackVM/RegisterVM 对齐): 加入方法名，
    // 三后端统一为"类型 X 不支持方法 Y"格式（原消息缺少方法名）。
    runtimeError("类型 " + obj.typeName() + " 不支持方法 " + node.methodName, node.line, node.column);
}

// ---- P1 重构：类实例方法调用（含 super.method() 处理）----
Value Interpreter::callInstanceMethod(MethodCall& node, Value& obj) {
    // Phase 1: 在类继承链中查找方法（含 super.method() 解析）
    const ClassInfo* searchClass = nullptr;
    std::string searchClassName;
    bool isSuperCall = false;
    FunDecl* method = nullptr;
    std::shared_ptr<Environment> cachedParentEnv;
    if (!findMethodInClass(obj, node, searchClass, searchClassName, isSuperCall, method, cachedParentEnv)) {
        runtimeError("类 " + obj.className() + " 没有方法 " + node.methodName, node.line, node.column);
    }

    // Phase 2: 求值参数列表（含 F10 默认参数填充、A3/ROUND49 重定义重查找）
    auto argValues = evaluateMethodArguments(node, searchClass, searchClassName, method, cachedParentEnv);

    // Phase 3: 设置方法调用环境（this 绑定、参数入栈）并执行方法体
    return invokeMethod(node, obj, searchClass, searchClassName, isSuperCall, method, std::move(cachedParentEnv),
                        std::move(argValues));
}

bool Interpreter::findMethodInClass(Value& obj, MethodCall& node, const ClassInfo*& searchClass,
                                    std::string& searchClassName, bool& isSuperCall, FunDecl*& method,
                                    std::shared_ptr<Environment>& cachedParentEnv) {
    auto classIt = classRegistry_.find(obj.className());
    if (classIt == classRegistry_.end()) {
        return false;
    }
    // O1: super.method() — 从父类开始查找方法
    isSuperCall = (node.object && node.object->nodeType == NodeType::NODE_SUPER_EXPR);
    // R97 #10 fix: 改为 const 指针，与 lookupClassSafely 返回的 const ClassInfo& 兼容
    searchClass = &classIt->second;
    // A3 bug fix: 缓存 searchClass 名称，参数求值后重新查找避免悬垂
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
        // R97 #10 fix: 用 lookupClassSafely 替代 find + end() + runtimeError 三步模式
        const ClassInfo& superCls = lookupClassSafely(
            ctxIt->second.superClassName, "未定义的父类: " + ctxIt->second.superClassName, node.line, node.column);
        searchClassName = superCls.name;
        searchClass = &superCls;
    } else {
        searchClassName = searchClass->name;
    }
    method = findMethod(*searchClass, node.methodName);
    if (!method) {
        return false;
    }
    // #2 fix: 缓存 closureEnv
    // H4 fix: 对 super.method() 调用，使用方法实际定义所在类（searchClass）的环境，
    // 而非实例所属类（classIt）的环境，确保继承链中跨作用域的变量绑定正确
    cachedParentEnv = searchClass->closureEnv;
    return true;
}

std::vector<Value> Interpreter::evaluateMethodArguments(MethodCall& node, const ClassInfo*& searchClass,
                                                        const std::string& searchClassName, FunDecl*& method,
                                                        std::shared_ptr<Environment>& cachedParentEnv) {
    // 求值参数
    std::vector<Value> argValues;
    // F10: 支持默认参数
    size_t argCount = node.arguments.size();
    if (argCount < static_cast<size_t>(method->requiredParamCount) || argCount > method->params.size()) {
        runtimeError(ErrorFormat::formatStd("方法 {} 期望 {}-{} 个参数，但传入了 {} 个", node.methodName,

                                            method->requiredParamCount, method->params.size(), argCount),
                     node.line, node.column, DiagCodes::kArityMismatch);
    }
    argValues.reserve(argCount);

    for (auto& arg : node.arguments) {
        argValues.push_back(evaluate(arg.get()));
    }

    // A3 bug fix: 参数求值可能触发类重定义（如参数中调用重定义类的函数），
    // 导致 classRegistry_ 中 ClassInfo 被替换、searchClass 悬垂。
    // 对齐 constructClassInstance 的 #2 fix：参数求值后重新查找。
    {
        // R97 #10 fix: 用 lookupClassSafely 替代 find + end() + runtimeError 三步模式
        const ClassInfo& reCls = lookupClassSafely(
            searchClassName, "类 " + searchClassName + " 在方法调用期间被重定义并删除", node.line, node.column);
        searchClass = &reCls;
        method = findMethod(*searchClass, node.methodName);
        if (!method) {
            runtimeError("类 " + searchClassName + " 在方法调用期间被重定义，方法 " + node.methodName + " 不再存在",
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
        // R97 #10 fix: 用 lookupClassSafely 替代 find + end() + runtimeError 三步模式
        const ClassInfo& reCls = lookupClassSafely(
            searchClassName, "类 " + searchClassName + " 在默认参数求值期间被重定义并删除", node.line, node.column);
        searchClass = &reCls;
        method = findMethod(*searchClass, node.methodName);
        if (!method) {
            runtimeError("类 " + searchClassName + " 在默认参数求值期间被重定义，方法 " + node.methodName + " 不再存在",
                         node.line, node.column);
        }
        cachedParentEnv = searchClass->closureEnv;
    }

    return argValues;
}

Value Interpreter::invokeMethod(MethodCall& node, Value& obj, const ClassInfo* searchClass,
                                const std::string& /*searchClassName*/, bool isSuperCall, FunDecl* method,
                                std::shared_ptr<Environment> cachedParentEnv, std::vector<Value> argValues) {
    // B3 fix: CallFrameGuard 自动管理 currentFunctionReturnType_ + callStack_ + classContextStack_
    // R163 泛型扩展：合并类泛型参数 + 方法泛型参数，使方法体内的类型参数注解（如 x: T）跳过类型校验
    std::vector<std::string> mergedTypeParams = searchClass->typeParams;
    for (const auto& tp : method->typeParams) {
        mergedTypeParams.push_back(tp);
    }
    CallFrameGuard frameGuard{*this, method->returnType, /*manageCtx=*/true, mergedTypeParams};
    auto prevEnv = currentEnv_;

    std::shared_ptr<Environment> methodEnv; // 声明在try外，使catch后可访问
    Value result = Value::nullValue();

    // S2 fix: 统一使用 RecursionGuard RAII 管理递归深度
    if (recursionDepth_ + 1 >= MAX_RECURSION_DEPTH) {
        // P3-16 fix: 示范迁移——硬编码字面量改为 ErrorMessages 常量 + formatStd（std::format 风格）
        runtimeError(ErrorFormat::formatStd(ErrorMessages::kRecursionDepthExceededFmtStd, MAX_RECURSION_DEPTH),
                     node.line, node.column, DiagCodes::kRecursionDepth);
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
        // B1 TCO 蹦床循环：`return this.m(args)` 自尾调用抛 TailCallSignal 触发
        // 帧复用（取回本轮更新后的 this，重建 methodEnv 重新执行方法体），
        // ReturnException 结束整个调用。动态分派校验在 visitReturnStmt（子类
        // override 时不抛信号，回退普通调用保持虚分派）。
        int64_t tcoIterations = 0;
        // 方法定义类名每轮相同（同一 method decl），循环外计算一次
        const std::string definingClassName = findMethodDefiningClassName(*searchClass, node.methodName);
        // 默认参数填充（在类定义闭包环境中求值，与入口路径一致），
        // 供蹦床后续轮次使用（入口轮的默认值已由 evaluateMethodArguments 填充）
        auto fillMethodDefaults = [&](std::vector<Value>& argsInOut) {
            if (argsInOut.size() >= method->params.size())
                return;
            struct EnvGuard {
                Interpreter& interp;
                std::shared_ptr<Environment> prev;
                ~EnvGuard() { interp.currentEnv_ = prev; }
            } guard{*this, currentEnv_};
            if (cachedParentEnv) {
                currentEnv_ = cachedParentEnv;
            }
            for (size_t i = argsInOut.size(); i < method->params.size(); ++i) {
                if (method->defaultValues[i]) {
                    argsInOut.push_back(evaluate(method->defaultValues[i].get()));
                } else {
                    argsInOut.push_back(Value::nullValue());
                }
            }
        };
        while (true) {
            // O5: 使用类定义时捕获的环境作为父级（闭包），而非调用者的环境。
            // B1 TCO: 回退分支用 prevEnv（调用入口环境）而非 currentEnv_，
            // 后续轮次 currentEnv_ 已是上一轮 methodEnv。
            auto parentEnv = cachedParentEnv ? cachedParentEnv : prevEnv; // #2 fix: 使用缓存值
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
            classContextStack_.push_back(definingClassName);

            try {
                // B1 TCO: 启用蹦床上下文（isMethod=true，识别 return this.m(args)）
                TcoScopeGuard tcoGuard{*this, method, node.methodName, /*isMethod=*/true, /*enabled=*/true};
                // B1 fix: 直接在 methodEnv 中执行方法体，避免 visitBlock 创建嵌套块作用域
                // 导致 envPool_ 碰撞（与 callClosureValue 同理）。
                executeFunctionBody(static_cast<Block&>(*method->body));
                result = std::move(lastValue_);
                break;
            } catch (TailCallSignal& sig) {
                // 帧复用：取回本轮更新后的 this（与真实递归中内层读取最新 this
                // 的语义一致），清理本轮 env 与栈条目后用新实参重建。
                if (++tcoIterations > RuntimeLimits::MAX_LOOP_ITERATIONS) {
                    runtimeError(
                        ErrorFormat::formatStd("尾调用迭代次数超过限制 ({})", RuntimeLimits::MAX_LOOP_ITERATIONS),
                        node.line, node.column);
                }
                auto* thisNow = methodEnv->get("this");
                Value updatedNow = thisNow ? *thisNow : Value::nullValue();
                methodEnv->closeCapturedVariables();
                callStack_.pop_back();
                classContextStack_.pop_back();
                obj = std::move(updatedNow);
                argValues = std::move(sig.args);
                fillMethodDefaults(argValues);
            }
        }
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

void Interpreter::visitNullLiteral(NullLiteral& node) {
    checkBreak(&node);
    // null 字面量：返回 null 值（MiniLang 中用于表示"无值"，可赋给任意类型注解）。
    lastValue_ = Value::nullValue();
    return;
}

// ============================================================
// R164 协程/生成器：实现已拆分到 InterpreterCoroutine.cpp
// ============================================================
