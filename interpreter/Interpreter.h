/**
 * @file interpreter/Interpreter.h
 * @brief 树遍历解释器（Visitor 模式）。
 *
 * MiniLang 三套执行引擎之一：直接遍历 AST 执行，无需中间字节码生成。
 * 实现 Visitor 接口的 33 个 visit* 方法，覆盖全部 AST 节点类型。
 *
 * 核心特性：
 *   - Environment 链式作用域（global → block → function）
 *   - 闭包捕获（共享 Environment shared_ptr，自动生命周期管理）
 *   - 类与继承（classRegistry_ + super 查找沿继承链）
 *   - 模块系统（import/export，含路径安全、循环依赖检测、预扫描）
 *   - 异常处理（try/catch/throw，C++ 异常 unwind + 作用域清理）
 *   - REPL 模式（saveReplState/restoreReplState，保留环境）
 *   - 调试器集成（DebugController，AST 节点级单步）
 *
 * 与 VM/RegisterVM 的语义一致性（三后端约束）：
 *   - 整数除法截断向零
 *   - and/or 短路返回操作数原值（非布尔）
 *   - 类型注解强制（typeMatchValue 共享）
 *   - super 调用语义
 *   - 错误消息文本统一
 *
 * @see IBackend Visitor Value Environment VM
 */
#pragma once

#include <atomic>
#include <functional>
#include <map> // evaluateCondition 沙箱 instSnaps 用 std::map 按指针去重
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Diagnostic.h"
#include "ast/ASTNode.h"
#include "common/IBackend.h"   // ARCH-09 fix: 后端抽象接口
#include "common/ModulePath.h" // AUDIT-R5 R6 fix: 模块路径缓存键规范化单一事实源
#include "common/Result.h"     // R98 W2: invokeClosureSync 返回 Result<Value>
#include "common/RuntimeLimits.h"
#include "interpreter/BuiltinMethods.h" // R164 fix: handleCoroutineMethod 返回 BuiltinMethodResult
#include "interpreter/CallFrame.h"      // ARCH-02 fix: CallFrame 拆出，避免传递依赖
#include "interpreter/Environment.h"
#include "interpreter/RuntimeExceptions.h" // S6 fix: 异常类提取到独立头文件
#include "interpreter/Value.h"
#include "interpreter/Visitor.h"
// R164 fix: checkType 模板使用 ErrorFormat::format + ErrorMessages 常量
// 顺序敏感性（项目记忆）：ErrorMessages.h 必须在 Value.h 之后
#include "common/ErrorFormat.h"
#include "common/ErrorMessages.h"

// ============================================================
// Interpreter 解释器
// ============================================================

// ============================================================
// DebugController 前向声明
// ============================================================
class DebugController;

// ============================================================
// 类定义信息
// ============================================================

/// 类定义信息结构
struct ClassInfo {
    std::string name;           // 类名
    std::string superClassName; // 父类名（空表示无父类）
    // A3 fix: shared_ptr 持有方法 AST 所有权，避免 AST 重建后裸指针悬垂
    std::unordered_map<std::string, std::shared_ptr<FunDecl>> methods; // 方法表
    std::unordered_map<std::string, Value> fields;                     // 默认字段值
    std::shared_ptr<Environment> closureEnv;                           // O5: 类定义时的环境（闭包捕获）
    // R163 泛型类：类型参数列表（空 = 非泛型），方法调用时合并到 currentTypeParams_
    std::vector<std::string> typeParams;
    // C6 fix: 方法分派缓存。沿继承链查找是 O(depth)，热路径上每次方法调用重复查找。
    // 缓存 methodName → (shared_ptr<FunDecl>, cacheGen_)。cacheGen_ 与 Interpreter::classRegistryGen_
    // 比较，不匹配则视为未命中（任何类重定义都会递增 gen，使全部缓存条目失效，
    // 解决子类缓存指向已被重定义父类的旧方法指针的悬垂问题）。
    // A3 fix: 缓存值改为 shared_ptr，与 methods 表所有权一致，避免悬垂。
    mutable std::unordered_map<std::string, std::pair<std::shared_ptr<FunDecl>, int>> methodCache_;
    // 注意：不再存储 superClass 裸指针，运行时通过 superClassName 在 classRegistry_ 中查找
    // 避免 unordered_map rehash 导致指针悬空
};

// ============================================================
// 枚举定义信息（R99）
// ============================================================

/// 枚举定义信息结构（与 ClassInfo 同级，便于 StateSnapshot 等外部结构使用）
struct EnumInfo {
    std::vector<std::string> typeParams;                  // 类型参数名（空=非泛型）
    std::vector<EnumVariant> variants;                    // variant 列表（按声明顺序）
    std::unordered_map<std::string, size_t> variantIndex; // variant 名 → 在 variants 中的下标
};

// ============================================================
// Interpreter 解释器
// ============================================================

/// 访问者模式解释执行器
class Interpreter : public Visitor, public IBackend {
public:
    Interpreter();
    ~Interpreter();

    /// ARCH-09 fix: IBackend 实现 — 后端名称
    std::string backendName() const override { return "Interpreter"; }

    /// 执行程序（AST 根节点）
    Value execute(Block& program);

    /// REPL 模式执行（不重置环境，保留变量/函数/类定义）
    Value executeRepl(Block& program);

    /// P1-4 fix: execute/executeRepl 共享的语句执行 + 异常处理逻辑
    Value runStatementsWithExceptionHandling(Block& program);

    /// REPL 模式下保留 AST 所有权（确保 classRegistry_/funRegistry_ 中的裸指针持续有效）
    void retainReplAst(std::unique_ptr<Block> ast);

    /// 保存 REPL 状态（Run 前调用），Run 结束后调用 restoreReplState() 恢复
    void saveReplState();
    void restoreReplState();

    /// 设置输出回调（ARCH-09 fix: IBackend override）
    void setOutputCallback(std::function<void(const std::string&)> callback) override;

    /// 设置输入回调（用于 input() 函数）
    /// 回调接收提示字符串，返回用户输入的字符串
    /// ARCH-09 fix: IBackend override
    void setInputCallback(std::function<std::string(const std::string&)> callback) override;

    /// F12: 设置模块加载回调（用于 import 语句）
    /// 回调接收模块路径，返回模块源代码内容。若模块不存在则返回空字符串。
    void setModuleLoader(std::function<std::string(const std::string&)> loader);

    /// BUG-REPL-AUDIT-9 fix: 检查是否已设置模块加载器。
    /// REPL 路径据此判断是否需要补设 loader（避免覆盖 Run 已建立的 loader/baseDir）。
    bool hasModuleLoader() const;

    /// BUG-REPL-AUDIT-1 fix: 设置模块文件 mtime 检查回调（用于模块缓存失效）
    /// 回调接收模块路径，返回文件的最后修改时间（毫秒时间戳，0 表示文件不存在或无法获取）。
    /// 若未设置，模块缓存不做 mtime 检查（保持原有行为，适用于非 REPL 场景）。
    void setModuleMtimeChecker(std::function<int64_t(const std::string&)> checker);

    /// F12: 设置当前文件路径（用于解析相对 import 路径）
    void setCurrentFilePath(const std::string& path);

    /// 设置调试控制器
    // MEM-01 fix: 改用 shared_ptr 共享所有权，使 worker 线程持有的 Interpreter
    // 保持 debugger 存活，避免 IdeController 析构后 debugger_ 悬垂。
    void setDebugger(std::shared_ptr<DebugController> dbg);

    /// 设置调试模式（启用/禁用 checkBreak 调用）
    void setDebugMode(bool enabled);

    /// 获取当前环境（用于调试面板）
    /// 注意：返回裸指针，仅在同线程或确保 Environment 不被销毁时使用。
    /// 跨线程场景应使用 currentEnvironmentShared()。
    Environment* currentEnvironment() const;

    /// 获取当前环境的 shared_ptr 副本（延长生命周期，用于跨线程安全访问）。
    /// AUDIT-P1 fix: 原 currentEnvironment() 返回裸指针，GUI 线程通过 variableCallback
    /// 调用时 worker 线程可能已修改 currentEnv_ 或析构 Environment → UAF。
    /// 此方法返回 shared_ptr 副本，确保调用方持引用期间 Environment 不被析构。
    std::shared_ptr<Environment> currentEnvironmentShared() const;

    /// 获取调用栈（用于调试面板）
    const std::vector<CallFrame>& getCallStack() const;

    /// AUDIT-P1 fix: 获取调用栈的值拷贝快照（用于跨线程调试回调）。
    /// getCallStack() 返回 const 引用，跨线程遍历期间 worker 可能 push_back/pop_back
    /// 导致迭代器失效。此方法返回值拷贝，调用方可在锁外安全遍历。
    std::vector<CallFrame> getCallStackSnapshot() const;

    /// 在当前环境中求值单个表达式（用于条件断点，不触发调试检查）
    Value evaluateExpr(ASTNode* node);

    /// GUI-03 fix: 安全求值条件断点表达式（保存/恢复所有可变状态，防止重入损坏）
    Value evaluateCondition(ASTNode* node);

    /// #4 fix: 设置全局环境用于 VM 条件断点求值（VM 路径无运行中的 Interpreter）
    void setGlobalEnvironment(std::shared_ptr<Environment> env) {
        globalEnv_ = env;
        currentEnv_ = env;
    }

    /// 获取全局环境（供 REPL %memory 等命令访问）
    std::shared_ptr<Environment> getGlobalEnvironment() const { return globalEnv_; }

    /// 获取诊断信息（ARCH-09 fix: IBackend override）
    const DiagnosticBag& getDiagnostics() const override { return diagnostics_; }

    /// 获取诊断信息（简短访问器）
    const DiagnosticBag& diagnostics() const { return diagnostics_; }

    /// 清空诊断信息
    void clearDiagnostics() { diagnostics_.clear(); }

    /// REPL 模块缓存刷新：清除指定模块的缓存，下次 import 将重新加载源码
    /// 场景：用户修改了模块源文件后希望在 REPL 中获取最新版本
    /// BUG-REPL-1 fix: 原实现直接用 path 查 erase，未做路径规范化。
    /// AUDIT-R5 R6/BUG-M4 fix: 缓存键 = moduleCacheKey(normalizeModulePathKey(path))（
    /// common/ModulePath.h 单一事实源），与 loadModuleOrGetCached 入键一致，
    /// 含连续斜杠折叠与 Windows 小写折叠，否则 reload 无法命中缓存。
    void clearModuleCache(const std::string& path) {
        std::string normalized = moduleCacheKey(normalizeModulePathKey(path));
        moduleCache_.erase(normalized);
        moduleExports_.erase(normalized);
        moduleMtimes_.erase(normalized); // BUG-REPL-AUDIT-1 fix
    }
    /// 清除所有模块缓存
    void clearAllModuleCache() {
        moduleCache_.clear();
        moduleExports_.clear();
        moduleMtimes_.clear(); // BUG-REPL-AUDIT-1 fix
    }

    /// REPL %reset: 完全重置 REPL 环境（清空变量/函数/类/模块缓存/AST）
    /// 语义：回到 IDE 启动时的 REPL 初始状态，所有用户定义丢失。
    /// 安全前置条件：调用方必须确保没有正在执行的 REPL 任务（replRunning_==false），
    /// 且解释器不在 worker 线程中运行（isRunning()==false）。
    void resetReplEnvironment();

    /// R161 调试器 REPL 阶段 2：从源 Interpreter 复制函数/类/枚举注册表，
    /// 使调试器 REPL 中可调用用户定义的函数/类/枚举。
    /// @param src 源 Interpreter（通常是主 Interpreter，持有 Run 期间建立的注册表）
    /// @note shared_ptr<FunDecl> 共享所有权，AST 在源 Interpreter 析构前保持有效；
    ///       ClassInfo 的 methods 也是 shared_ptr，共享所有权；
    ///       EnumInfo 仅含字符串/向量，值拷贝安全。
    /// @note 调用方需确保 src 不在 worker 线程中并发修改（Interpreter 调试暂停期
    ///       worker 阻塞在 pauseCV_，安全；VM 暂停期主 Interpreter 空闲，安全）。
    /// @note 每次调用覆盖当前注册表（不合并），多次调用以最后一次为准。
    void injectRegistriesFrom(const Interpreter& src);

    /// R161 调试器 REPL 阶段 2：查询主 Interpreter 是否有可注入的注册表。
    /// 用于 IdeController 判定是否调用 injectRegistriesFrom（避免空注册表注入开销）。
    bool hasRegistries() const { return !funRegistry_.empty() || !classRegistry_.empty() || !enumRegistry_.empty(); }

    // ---- L18: Interpreter 状态快照与回滚 ----
    /// Interpreter 状态快照（用于回溯调试 / reverse debugging）。
    /// 捕获完整执行状态：环境链 + 调用栈 + 注册表 + 控制标志。
    /// 通过 shared_ptr 共享所有权保持 Environment 对象存活；通过 flat variables map
    /// 保持快照时刻的变量值（防止 live 执行变异污染快照）。
    /// COW 容器（Array/Dict/Instance）通过 Value 拷贝自然隔离——live 执行的变异
    /// 会触发 COW detach，快照持有的 Value 仍指向旧容器数据。
    struct StateSnapshot {
        // 环境链结构（shared_ptr 保持对象存活）
        std::shared_ptr<Environment> globalEnv;
        std::shared_ptr<Environment> currentEnv;
        std::vector<CallFrame> callStack;
        // 环境链每层变量快照（currentEnv -> parent -> ... -> globalEnv）
        // 每个 entry: (env shared_ptr, 该 env 的局部变量深拷贝)
        // 恢复时调用 env->restoreLocalVariables(variables) 整表替换
        std::vector<std::pair<std::shared_ptr<Environment>, std::unordered_map<std::string, Value>>> envChainVars;
        // 注册表（shared_ptr 共享 AST 所有权）
        std::unordered_map<std::string, std::shared_ptr<FunDecl>> funRegistry;
        int funRegistryGen = 0;
        std::unordered_map<std::string, ClassInfo> classRegistry;
        int classRegistryGen = 0;
        // EnumInfo 现已提升为顶层结构（与 ClassInfo 同级）
        std::unordered_map<std::string, EnumInfo> enumRegistry;
        std::unordered_map<std::string, std::shared_ptr<Environment>> moduleCache;
        std::unordered_map<std::string, std::unordered_set<std::string>> moduleExports;
        std::unordered_map<std::string, int64_t> moduleMtimes;
        std::vector<std::string> moduleLoadingStack;
        std::unordered_set<std::string> moduleLoadingSet;
        std::unordered_set<std::string> exportedNames;
        // 控制状态
        int recursionDepth = 0;
        std::vector<std::string> classContextStack;
        std::string currentFunctionReturnType;
        std::vector<std::string> currentTypeParams;
        int loopFlow = 0; // 0=None, 1=Break, 2=Continue
        int currentCoroutineTargetYieldId = -1;
        int currentYieldExecutionCount = 0;
    };

    /// 捕获当前 Interpreter 完整状态快照（用于回溯调试）。
    /// @return shared_ptr<StateSnapshot>，可存入 TraceSnapshot::interpreterState
    /// @note 调用方需确保调用时无其他线程修改 Interpreter（debugger 暂停期安全）
    std::shared_ptr<StateSnapshot> captureStateSnapshot() const;

    /// 从快照恢复 Interpreter 状态（回溯调试核心）。
    /// @param snap captureStateSnapshot 返回的快照
    /// @return true 成功；false 快照无效
    /// @note 恢复后 Interpreter 状态等价于快照时刻，可继续步进。
    ///       已输出的 print 副作用无法撤回（与 VM 路径一致的语义限制）。
    bool restoreFromSnapshot(const StateSnapshot& snap);

    /// 请求中止当前执行（REPL 超时/关闭时调用）
    /// checkBreak 会在每个语句节点检查此标志并抛异常，实现协作式中止
    void requestStop() { stopRequested_.store(true, std::memory_order_relaxed); }
    bool isStopRequested() const { return stopRequested_.load(std::memory_order_relaxed); }

    // ---- R114 可回放执行时间轴：recorder 集成 ----
    /// 启用/禁用执行轨迹录制。启用后 checkBreak 在每个语句节点入口采集快照。
    /// @note 跨线程安全——recordingEnabled_ 为 atomic，checkBreak 在 worker 线程读，
    ///       主线程 setRecordingEnabled() 写。recorder 内部 mutex 保护快照 deque。
    ///       采集开销：每次 checkBreak 调用 captureInterpreterStep，含 Environment
    ///       shared_ptr 拷贝 + allVariablesMap() 遍历 + Value toString，约 5-20μs。
    ///       若性能敏感可仅在调试模式启用（setRecordingEnabled(debugMode)）。
    void setRecordingEnabled(bool enabled) { recordingEnabled_.store(enabled, std::memory_order_relaxed); }
    bool isRecordingEnabled() const { return recordingEnabled_.load(std::memory_order_relaxed); }

    // ---- 25 个 visit 方法实现 ----

    void visitBinaryOp(BinaryOp& node) override;
    void visitUnaryOp(UnaryOp& node) override;
    void visitNumberLiteral(NumberLiteral& node) override;
    void visitStringLiteral(StringLiteral& node) override;
    void visitBoolLiteral(BoolLiteral& node) override;
    void visitVarDecl(VarDecl& node) override;
    void visitAssignment(Assignment& node) override;
    void visitVarRef(VarRef& node) override;
    void visitIfStmt(IfStmt& node) override;
    void visitWhileStmt(WhileStmt& node) override;
    void visitForStmt(ForStmt& node) override;
    void visitFunDecl(FunDecl& node) override;
    void visitFunCall(FunCall& node) override;
    void visitReturnStmt(ReturnStmt& node) override;
    void visitPrintStmt(PrintStmt& node) override;
    void visitBlock(Block& node) override;

    // 新增 9 个 visit 方法
    void visitArrayLiteral(ArrayLiteral& node) override;
    void visitDictLiteral(DictLiteral& node) override;
    void visitIndexAccess(IndexAccess& node) override;
    void visitIndexAssign(IndexAssign& node) override;
    void visitClassDecl(ClassDecl& node) override;
    void visitMemberAccess(MemberAccess& node) override;
    void visitMemberAssign(MemberAssign& node) override;
    void visitMethodCall(MethodCall& node) override;
    void visitNullLiteral(NullLiteral& node) override;
    void visitSuperExpr(SuperExpr& node) override;
    void visitBreakStmt(BreakStmt& node) override;
    void visitContinueStmt(ContinueStmt& node) override;
    void visitTryStmt(TryStmt& node) override;
    void visitThrowStmt(ThrowStmt& node) override;
    void visitImportStmt(ImportStmt& node) override;
    void visitExportStmt(ExportStmt& node) override;
    void visitInterpolatedString(InterpolatedString& node) override; // C5 fix
    // R98 元组与解构
    void visitTupleLiteral(TupleLiteral& node) override;
    void visitDestructureBinding(DestructureBinding& node) override;
    // R99 枚举与 ADT + match
    void visitEnumDecl(EnumDecl& node) override;
    void visitEnumVariantExpr(EnumVariantExpr& node) override;
    void visitMatchExpr(MatchExpr& node) override;
    // R164 协程/生成器：yield 表达式求值（重放模式）
    void visitYieldExpr(YieldExpr& node) override;

    // R99 辅助方法
    /// match case 体的求值（Block 或单表达式）
    Value evaluateMatchBody(ASTNode* body);
    /// R134 递归 pattern 匹配 helper
    /// 在 caseEnv 中绑定变量；返回是否匹配成功。
    /// 支持 WILDCARD/LITERAL/VARIABLE/VARIANT/TUPLE/OR 六种 pattern 递归匹配。
    bool tryMatchPattern(const MatchPattern& p, const Value& scrutinee, Environment& caseEnv);
    /// 判断类型名是否是 enum 的类型参数（运行时擦除，跳过类型校验）
    bool isTypeParameter(const std::string& typeName, const std::vector<std::string>& typeParams) const;

private:
    // 运行时限制常量 — 统一引用 common/RuntimeLimits.h
    // S1 fix: 消除散布在 7 个文件的重复定义，避免对齐遗漏
    static constexpr int MAX_RECURSION_DEPTH = RuntimeLimits::MAX_RECURSION_DEPTH;
    static constexpr int MAX_INHERITANCE_DEPTH = RuntimeLimits::MAX_INHERITANCE_DEPTH;
    static constexpr int64_t MAX_LOOP_ITERATIONS = RuntimeLimits::MAX_LOOP_ITERATIONS;

    std::shared_ptr<Environment> globalEnv_;  // 全局环境
    std::shared_ptr<Environment> currentEnv_; // 当前环境
    std::vector<CallFrame> callStack_;        // 调用栈
    // PERF-07 fix: Environment 对象池。visitBlock 退出时若块作用域未被闭包捕获
    // （use_count==1），回收并 reset 后供下次 visitBlock 复用，避免重复堆分配。
    // execute() 开头清空（旧环境链已销毁，池中 Environment 可能被新链引用作 parent）。
    std::vector<std::shared_ptr<Environment>> envPool_;
    // MEM-01 fix: shared_ptr 共享所有权，worker 线程持有的 Interpreter 保持 debugger 存活
    // AUDIT-R4 BUG-15 fix: setDebugger() 在主线程写入，checkBreak()/visit* 在 worker 线程读取，
    // 原普通 shared_ptr 构成数据竞争（UB）。原方案用 std::atomic<std::shared_ptr>，
    // 但 macOS Clang libc++ 要求 T 满足 is_trivially_copyable（shared_ptr 不满足），
    // 编译失败。改用 mutex 保护的 shared_ptr——debugMode_ atomic<bool> 作为热路径 fast-path
    // 短路，仅调试模式启用时才进入加锁读，热路径无锁开销。
    std::shared_ptr<DebugController> debugger_; // 调试控制器（可为 nullptr）
    mutable std::mutex debuggerMutex_;          // 保护 debugger_ 的读写
    // QT-R-02 fix: debugMode_ 改为 atomic，消除主线程 setDebugMode() 与 worker 线程
    // checkBreak() 读操作之间的数据竞争。A6 fix 已确保主线程不在 worker 运行时
    // 调用 setDebugMode，但 atomic 提供额外的内存可见性保证和防御性保护。
    std::atomic<bool> debugMode_{false}; // 是否处于调试模式（快速跳过 checkBreak）
    // REPL 协作中止标志：closeEvent 超时路径设置，checkBreak 检查并抛异常
    std::atomic<bool> stopRequested_{false};
    // R114 可回放执行时间轴：录制启用标志。checkBreak 在每个语句节点入口读取此标志，
    // 为 true 时调用 traceRecorder().captureInterpreterStep() 采集快照。
    // atomic<bool> 保证跨线程可见性（worker 线程读，主线程 setRecordingEnabled 写）。
    std::atomic<bool> recordingEnabled_{false};
    // AUDIT-P1-CORRECT fix: 条件断点求值步数上限，防止无限循环（如 while(true){}）冻结 UI。
    // 0 表示非条件求值（不计数）；>0 表示正在条件求值（evaluate 中递增并检查上限）。
    // evaluateCondition 开头设为 1（开始计数），execute/executeRepl 开头重置为 0。
    // AUDIT-P1.2 fix: MAX_CONDITION_STEPS 迁移到 RuntimeLimits.h 统一管理。
    size_t evaluationStepCount_ = 0;
    static constexpr size_t MAX_CONDITION_STEPS = RuntimeLimits::MAX_CONDITION_STEPS;
    // AUDIT-R4 P-3 fix: outputCallback_ 改用 shared_ptr 包裹——output() 每次 print
    // 都需“锁内拷贝、锁外调用”（A6 防死锁模式），原 std::function 整体拷贝
    // 含潜在堆分配，循环 print 密集的教学程序上每行输出一次。改为锁内拷贝
    // shared_ptr（仅 refcount 递增，无堆分配），调用语义不变。
    std::shared_ptr<const std::function<void(const std::string&)>> outputCallback_; // 输出回调
    std::function<std::string(const std::string&)> inputCallback_;                  // 输入回调（input() 函数）
    std::function<std::string(const std::string&)> moduleLoader_;                   // F12: 模块加载回调
    // BUG-REPL-AUDIT-1 fix: 模块文件 mtime 检查回调
    std::function<int64_t(const std::string&)> moduleMtimeChecker_;
    // A6 fix: callback 跨线程 mutex 保护。同一 Interpreter 实例被 worker 线程（execute）
    // 和主线程（REPL/条件断点求值/callback 设置）同时访问，std::function 成员无 mutex
    // 保护会导致数据竞争。setter 加锁写入，invocation 加锁拷贝后解锁调用（避免持锁回调）。
    mutable std::mutex callbackMutex_;
    // R136 spawn 子线程闭包调用序列化 mutex。Interpreter 的 callStack_/currentEnv_
    // 非线程安全，spawn 出的子线程若并发调用 invokeClosureSync 会破坏这些共享状态。
    // 通过此 mutex 序列化所有 spawn 出的闭包调用（同一 Interpreter 实例级别）。
    // 注：这本质上是"用户级并发受限"的折衷——真正并发需重构 Interpreter 为可重入。
    // StackVM/RegisterVM 同理（操作数栈/寄存器帧非线程安全）。
    std::mutex spawnMutex_;
    std::string currentFilePath_; // F12: 当前文件路径
    // R97 #3 fix: 移除 lastAsciiStr* 4 字段缓存，改为 StringData::cachedIsAscii 持久缓存。
    // 原实现按 (ptr, len, firstByte) 三重验证缓存上次 ASCII 判定结果，存在堆地址复用
    // 误命中风险。新方案 isAscii 直接存在 StringData 内，与字符串生命周期绑定，
    // O(1) 读取无碰撞风险。
    std::unordered_map<std::string, std::shared_ptr<Environment>> moduleCache_; // F12: 模块缓存
    // BUG-REPL-AUDIT-1 fix: 模块文件 mtime 缓存，用于检测文件修改后缓存失效
    std::unordered_map<std::string, int64_t> moduleMtimes_;
    std::unordered_map<std::string, std::unordered_set<std::string>> moduleExports_; // F12: 模块导出名称缓存
    std::vector<std::string> moduleLoadingStack_; // F12: 模块加载栈（顺序管理 + 深度保护）
    std::unordered_set<std::string>
        moduleLoadingSet_; // D19 fix: 模块加载集合（O(1) 循环依赖检测，与 moduleLoadingStack_ 同步维护）
    std::unordered_set<std::string> exportedNames_; // F12: 当前模块的导出名称集合
    DiagnosticBag diagnostics_;                     // 诊断收集器
    int recursionDepth_ = 0;                        // 递归深度
    // A3 fix: shared_ptr 持有函数 AST 所有权，避免 AST 重建后裸指针悬垂
    std::unordered_map<std::string, std::shared_ptr<FunDecl>> funRegistry_; // 函数注册表
    int funRegistryGen_ = 0;                                   // M7: 注册表代数，函数重定义时递增使 FunCall 缓存失效
    std::unordered_map<std::string, ClassInfo> classRegistry_; // 类注册表
    int classRegistryGen_ = 0;                     // C6 fix: 类注册表代数，任何类定义/重定义时递增，使方法分派缓存失效
    std::vector<std::string> classContextStack_;   // super 解析用：当前执行的方法所属类名栈
    std::string currentFunctionReturnType_;        // 当前函数的返回类型
    std::vector<std::string> currentTypeParams_;   // R163 当前函数/方法的泛型类型参数（空=非泛型）
    std::vector<std::unique_ptr<Block>> replAsts_; // REPL 模式下保留 AST，确保 funRegistry_/classRegistry_ 指针有效

    // R99 枚举与 ADT: enum 注册表
    // key=enum 名，value=EnumInfo（variant 列表 + 类型参数名 + AST 指针）
    // visitEnumDecl 时插入；visitEnumVariantExpr/visitMatchExpr 时查询。
    // AST 指针（shared_ptr）保证 REPL 重解析后旧引用仍有效。
    // L18: EnumInfo 已提升为顶层结构（与 ClassInfo 同级），便于 StateSnapshot 使用
    std::unordered_map<std::string, EnumInfo> enumRegistry_;

    // RA-C fix: break/continue 改用状态标志而非 C++ 异常。
    // 仅在循环结构（visitWhileStmt/visitForStmt）内有效，传播路径短。
    // visitBlock/visitIfStmt 在 evaluate 后检查此标志并提前退出，
    // 避免后续语句覆盖标志或执行不该执行的副作用。
    // ReturnException 仍保留异常机制（跨函数非局部跳转，状态标志侵入性大）。
    // 用户级 try-catch 不受影响：break/continue 是标志不是异常，
    // 绝不会被 catch (ThrowException&) 误捕，自然穿透 try 块到达循环。
    enum class LoopFlow { None, Break, Continue };
    LoopFlow loopFlow_ = LoopFlow::None;

    // ---- B1 TCO：Interpreter 尾调用蹦床上下文 ----
    // 对齐三条 VM 路径的 TCO 帧复用：visitReturnStmt 在识别到自尾调用
    // （TCO::identifyTailCall，与 VM 同一单一事实源）且不在 try 块内时，
    // 求值实参后抛 TailCallSignal，由 callNamedFunction/invokeMethod 的蹦床
    // 循环捕获并帧复用重新执行函数体，C++ 递归深度恒定。
    // 安全不变量：
    //   1. 仅蹦床调用点启用（tcoEnabled_=true）；其他 executeFunctionBody
    //      调用点（init/生成器/callClosureValue/invokeClosureSync）置为
    //      禁用，保证信号不跨边界逃逸。
    //   2. tcoTryDepth_ > 0（return 在 try/catch/finally 内）时不 TCO，
    //      保留 finally 语义路径（与 VM 的 tryDepth_==0 判据一致）。
    //   3. 运行时重绑定校验：SelfFunction 要求调用名当前仍解析到正在
    //      执行的 FunDecl；SelfMethod 要求动态分派（this 的类继承链）
    //      仍命中当前 decl（子类 override 时回退普通调用，保持虚分派）。
    FunDecl* tcoDecl_ = nullptr; // 当前蹦床执行的函数/方法声明
    std::string tcoName_;        // 尾调用识别用简单名（不含类名前缀）
    bool tcoIsMethod_ = false;   // 当前是否在类方法体内
    bool tcoEnabled_ = false;    // 仅蹦床调用点为 true
    int tcoTryDepth_ = 0;        // 当前函数体内 try 嵌套深度

    // RAII：进入函数体前保存/设置 TCO 上下文，退出（含异常路径）恢复。
    // 新函数体的 tcoTryDepth_ 从 0 开始（外层 try 不影响内层函数的 TCO，
    // 与 VM 每函数独立编译的 tryDepth 语义一致）。
    struct TcoScopeGuard {
        Interpreter& in;
        FunDecl* savedDecl;
        std::string savedName;
        bool savedIsMethod;
        bool savedEnabled;
        int savedTryDepth;
        TcoScopeGuard(Interpreter& i, FunDecl* decl, const std::string& name, bool isMethod, bool enabled)
            : in(i), savedDecl(i.tcoDecl_), savedName(std::move(i.tcoName_)), savedIsMethod(i.tcoIsMethod_),
              savedEnabled(i.tcoEnabled_), savedTryDepth(i.tcoTryDepth_) {
            in.tcoDecl_ = decl;
            in.tcoName_ = name;
            in.tcoIsMethod_ = isMethod;
            in.tcoEnabled_ = enabled;
            in.tcoTryDepth_ = 0;
        }
        ~TcoScopeGuard() {
            in.tcoDecl_ = savedDecl;
            in.tcoName_ = std::move(savedName);
            in.tcoIsMethod_ = savedIsMethod;
            in.tcoEnabled_ = savedEnabled;
            in.tcoTryDepth_ = savedTryDepth;
        }
        TcoScopeGuard(const TcoScopeGuard&) = delete;
        TcoScopeGuard& operator=(const TcoScopeGuard&) = delete;
    };

    // R164 协程/生成器：当前重放的目标 yieldId。
    // -1 表示不在协程重放上下文（普通函数执行）；>=0 表示当前正在重放生成器函数体，
    // 当 visitYieldExpr 遇到 node.yieldId == currentCoroutineTargetYieldId_ 时抛出 YieldSignal。
    // 由 callCoroutineNext 设置（保存旧值→设置目标→执行→恢复旧值），支持嵌套（生成器调用生成器）。
    int currentCoroutineTargetYieldId_ = -1;

    // R164 协程/生成器：运行时 yield 执行计数器。
    // 重放模式下，每次 visitYieldExpr 被调用时递增。用于区分循环内同一 yield 节点的多次执行
    // （编译期 yieldId 对循环内 yield 只分配一次，但运行时可能执行 N 次）。
    // callCoroutineNext 每次重放开始时重置为 0；visitYieldExpr 比较
    // currentYieldExecutionCount_ 与 currentCoroutineTargetYieldId_ 决定是否抛出 YieldSignal。
    int currentYieldExecutionCount_ = 0;

    // ARCH-12 fix: REPL 状态暂存聚合为 ReplState 结构体（原为 10 个散布的 saved* 字段）。
    // 将 REPL 状态管理的完整边界集中在一处，便于理解和未来进一步提取为独立类。
    // 语义：saveReplState() 将当前 REPL 状态 move 到 ReplState，restoreReplState() 反向 move 回。
    // active 标志用于防御性检查，避免未 save 就 restore 或重复 restore。
    struct ReplState {
        std::shared_ptr<Environment> savedGlobalEnv;
        std::unordered_map<std::string, ClassInfo> savedClassRegistry;
        std::vector<std::unique_ptr<Block>> savedReplAsts;
        // BUG7 fix: 保存 funRegistry_ 以避免 Run→REPL 切换后函数注册表丢失
        // A3 fix: 与 funRegistry_ 一致，使用 shared_ptr 持有所有权
        std::unordered_map<std::string, std::shared_ptr<FunDecl>> savedFunRegistry;
        int savedFunRegistryGen = 0;
        // P1-1 fix: 模块相关状态暂存（避免 Run→REPL 切换后悬垂指针）
        std::unordered_map<std::string, std::shared_ptr<Environment>> savedModuleCache;
        std::unordered_map<std::string, std::unordered_set<std::string>> savedModuleExports;
        std::unordered_set<std::string> savedExportedNames;
        std::vector<std::string> savedModuleLoadingStack;
        std::unordered_set<std::string> savedModuleLoadingSet; // D19 fix: 与 savedModuleLoadingStack 配对
        // P2-A fix: 与 moduleCache_ 同步保存/恢复 mtime，避免 Run→REPL 切换后
        // 缓存失效检测错位（mtime 与 cache 内容不一致导致使用陈旧缓存）
        std::unordered_map<std::string, int64_t> savedModuleMtimes;
        // R99: enum 注册表保存（与 classRegistry_ 同等处理）
        std::unordered_map<std::string, EnumInfo> savedEnumRegistry;
        bool active = false; // 是否有暂存的状态（避免未 save 就 restore）
    } replState_;

    // S2 fix: RAII 递归深度守卫 — 统一 constructClassInstance/callNamedFunction/callInstanceMethod
    // 的递归深度管理，消除手动递减在异常路径下的遗漏风险
    struct RecursionGuard {
        int& depth;
        bool dismissed = false;
        explicit RecursionGuard(int& d) : depth(d) { ++depth; }
        ~RecursionGuard() {
            if (!dismissed)
                --depth;
        }
        void dismiss() { dismissed = true; }
    };

    // B3 fix: RAII 调用帧守卫 — 统一 5 处调用帧状态管理（currentFunctionReturnType_ +
    // callStack_ + classContextStack_），消除异常路径和成功路径中重复的手动恢复代码。
    // 构造时保存 currentFunctionReturnType_ 并设置新值；析构时恢复 returnType，
    // 并将 callStack_/classContextStack_ 弹出到构造时的深度（处理异常路径自动清理）。
    // currentEnv_ 不由此守卫管理（各调用点的 env 保存/恢复时机不同）。
    // R163 泛型扩展：同时管理 currentTypeParams_（泛型函数/方法的类型参数列表）。
    struct CallFrameGuard {
        Interpreter& interp;
        std::string savedReturnType;
        std::vector<std::string> savedTypeParams;
        size_t savedStackDepth;
        size_t savedClassContextDepth;
        bool manageClassContext;
        CallFrameGuard(Interpreter& i, const std::string& newReturnType, bool manageCtx = false,
                       const std::vector<std::string>& typeParams = {})
            : interp(i), savedReturnType(i.currentFunctionReturnType_), savedTypeParams(i.currentTypeParams_),
              savedStackDepth(i.callStack_.size()), savedClassContextDepth(i.classContextStack_.size()),
              manageClassContext(manageCtx) {
            interp.currentFunctionReturnType_ = newReturnType;
            interp.currentTypeParams_ = typeParams;
        }
        ~CallFrameGuard() {
            while (interp.callStack_.size() > savedStackDepth) {
                interp.callStack_.pop_back();
            }
            if (manageClassContext) {
                while (interp.classContextStack_.size() > savedClassContextDepth) {
                    interp.classContextStack_.pop_back();
                }
            }
            interp.currentFunctionReturnType_ = std::move(savedReturnType);
            interp.currentTypeParams_ = std::move(savedTypeParams);
        }
        CallFrameGuard(const CallFrameGuard&) = delete;
        CallFrameGuard& operator=(const CallFrameGuard&) = delete;
    };

    /// 执行单个节点
    // A1 fix: Visitor::accept 返回 void，evaluate() 通过 lastValue_ 获取结果。
    // evaluate() 保持返回 Value 的签名，所有调用方无需修改。
    Value evaluate(ASTNode* node);
    Value lastValue_; // A1 fix: visit 方法的结果载体（替代 accept 返回值）

    /// 检查调试断点
    void checkBreak(ASTNode* node);

    /// 输出字符串
    void output(const std::string& text);

    /// 数值二元运算（含类型提升）
    // PERF-06 fix: 改为按值接收，使调用方 move 临时 Value 进入，
    // 函数内可检测独占所有权（tryGetMutableString）做原地 append。
    Value numericBinaryOp(BinOpType opType, Value left, Value right, int line, int col);

    /// 拓展二期·语言（运算符重载）：instance 算术分派 __add/__sub/__mul/__div/__mod。
    /// 左操作数为 instance 且类（含继承链）定义了对应 dunder 方法时调用
    /// obj.__op(right) 并将返回值写入 out，返回 true；未定义返回 false
    /// （调用方回退到原“算术运算需要数值类型”报错）。
    /// 限制（MVP，三后端一致）：方法内不支持 super 调用；方法必须恰好 1 参。
    bool tryOperatorOverload(BinOpType opType, Value& left, Value& right, int line, int col, Value& out);

    /// P1-2 fix: 比较运算（LT/GT/LTE/GTE）共用模板，消除 4 处重复样板
    template <typename Cmp> Value compareNumericOrString(BinaryOp& node, Cmp cmp);

    /// 报告运行时错误
    [[noreturn]] void runtimeError(const std::string& msg, int line, int col);

    /// P2 fix (错误码优先匹配): 带 code 的重载——抛出携带稳定诊断码的 RuntimeError，
    /// 由 runStatementsWithExceptionHandling 的 catch 块透传到 addError。
    [[noreturn]] void runtimeError(const std::string& msg, int line, int col, const std::string& diagCode);

    /// @brief 安全查找类——找不到时调用 runtimeError（[[noreturn]]）。
    ///
    /// R97 #10 fix: 替代 `classRegistry_.find + end() 检查 + runtimeError` 三步重复模式。
    /// 调用方负责构造 notFoundMsg（如 "未定义的父类: X" 或 "类 X 在 ... 期间被重定义并删除"）。
    /// 由于 runtimeError 是 [[noreturn]]，函数在成功路径返回引用，失败路径不会返回。
    ///
    /// @param name 类名
    /// @param notFoundMsg 找不到类时的错误消息
    /// @param line 源码行号（用于错误定位）
    /// @param col 源码列号（用于错误定位）
    /// @return 类信息的 const 引用（成功路径）
    /// @note 失败路径抛出 RuntimeError 异常，不会返回
    const ClassInfo& lookupClassSafely(const std::string& name, const std::string& notFoundMsg, int line, int col);

    /// 线程安全地获取 debugger_ 的 shared_ptr 副本。
    /// AUDIT-R4 BUG-15 fix: 替代 std::atomic<std::shared_ptr>::load（macOS Clang 不支持）。
    /// 调用方持引用期间 debugger 不会被分离。checkBreak 热路径由 debugMode_ atomic<bool>
    /// 短路，仅调试模式启用时才调用此方法加锁读取。
    std::shared_ptr<DebugController> debugger() const {
        std::lock_guard<std::mutex> lock(debuggerMutex_);
        return debugger_;
    }

    /// 查找类的方法（含继承链）
    FunDecl* findMethod(const ClassInfo& cls, const std::string& methodName);

    /// 查找方法实际定义所在的类名（含继承链）。
    /// 用于 classContextStack_ 压入"方法定义所在类"而非"搜索起始类"，
    /// 确保 super 调用从方法定义类的父类开始搜索（而非搜索起始类的父类）。
    /// 若未找到方法，返回 startCls.name（fallback，不应到达）。
    std::string findMethodDefiningClassName(const ClassInfo& startCls, const std::string& methodName);

    /// 查找类的字段默认值（含继承链）
    Value findFieldDefault(const ClassInfo& cls, const std::string& fieldName);

    /// 写回左值（链式求值，避免重复求值副作用）
    /// isIndexAssign=true 时为索引赋值，indexNode 为索引表达式节点；否则为成员赋值，fieldName 为字段名
    /// valueNode 为赋值右值表达式节点，在 writeBack 内部按 object→index→value 顺序求值
    Value writeBack(ASTNode* objectNode, bool isIndexAssign, ASTNode* indexNode, const std::string& fieldName,
                    ASTNode* valueNode, int line, int col);
    /// 写回已修改的值（用于方法调用等已自行修改对象的场景，链式求值避免重复求值）
    void writeBack(ASTNode* objectNode, const Value& modifiedValue, int line, int col);

    /// 链收集与求值结果
    struct ChainInfo {
        std::vector<ASTNode*> chain;
        std::vector<Value> vals;
        std::vector<Value> idxs;
        VarRef* varRef; // nullptr if root is not a VarRef
    };

    /// 收集从 objectNode 到 VarRef 的节点链，并从外到内逐级求值
    /// errorOnNonVarRef=true 时，非 VarRef 根节点报错；否则 varRef 设为 nullptr
    ChainInfo collectAndEvaluateChain(ASTNode* objectNode, bool errorOnNonVarRef, int line, int col);

    /// 从内到外逐级写回修改后的值，最终写回变量
    // P1 fix: 改为非 const 引用，使 std::move 真正生效（const T&& 退化为拷贝）
    void writeBackChain(ChainInfo& info, Value innermost, int line, int col);

    /// 检查值是否匹配类型注解
    bool typeMatch(const Value& val, const std::string& annotation) const;

    /// 类型检查，不匹配则报运行时错误
    /// P20 fix: 模板化 contextBuilder 消除 std::function 堆分配
    /// R164 fix: 三后端消息统一（fuzz mutate 发现的分歧）——放弃 contextBuilder 上下文前缀，
    /// 统一用 ErrorMessages::kTypeAnnotationViolationFmt 对齐 StackVM/RegisterVM。
    /// contextBuilder 参数保留（避免改调用方签名），但不用于错误消息。
    /// 上下文可通过 runtimeError 的 line/col 定位。
    template <typename ContextBuilder>
    void checkType(const Value& val, const std::string& annotation, ContextBuilder&& contextBuilder, int line,
                   int col) {
        (void)contextBuilder; // R164 fix: 不再用于错误消息，保留参数避免改调用方
        // R163 泛型扩展：类型参数运行时擦除（跳过校验），与 enum variant 的 isTypeParameter 模式一致
        if (!currentTypeParams_.empty() && isTypeParameter(annotation, currentTypeParams_)) {
            return;
        }
        if (!typeMatch(val, annotation)) {
            runtimeError(
                ErrorFormat::formatStd(ErrorMessages::kTypeAnnotationViolationFmtStd, annotation, val.typeName()), line,
                col, DiagCodes::kTypeMismatch);
        }
    }

    /// 查找变量的类型注解（返回指针，避免字符串拷贝）
    const std::string* findTypeAnnotation(const std::string& varName) const;

    /// BUG-003 fix: 增量 GC 触发器。收集当前 callStack_ / globalEnv_ / classRegistry_
    /// 中的堆对象指针作为根集，调用 GcManager::collectCycle。由 GcManager 在分配
    /// 阈值达到时回调。无堆对象时仍调用 collectCycle（其内部 tracked_.empty() 提前返回）。
    void triggerIncrementalGc();

    // ---- P1 重构：visitFunCall 分派器辅助方法 ----

    /// 链式调用 / 表达式调用 callee(args)
    Value callClosureValue(FunCall& node);

    /// 内置构造函数 dict() / array()
    Value callBuiltinConstructor(FunCall& node);

    /// 顶层内置函数 len/type/str/int/abs/min/max/range/sum
    Value callBuiltinFunction(FunCall& node);

    /// R98 W2: 高阶内置函数 map/filter/reduce/forEach/find
    /// 拦截这 5 个名字，调用共享算法层（executeSharedMap 等），
    /// 通过 invokeClosureSync 回调执行用户传入的闭包。
    Value callHigherOrderBuiltin(FunCall& node);

    /// R136 spawn(fn, args...) 内置函数
    /// 拦截 spawn 调用，构造 ClosureInvoker（加 spawnMutex_ 序列化）后
    /// 调用 executeSharedSpawn 启动新线程。
    Value callSpawnBuiltin(FunCall& node);

    /// R98 W2: 同步调用闭包值（供高阶函数共享层回调）
    /// 从闭包提取 AST body + env，构造新 Environment 绑定参数，
    /// 执行函数体并捕获 ReturnException 返回值。
    /// 与 callClosureValue 的区别：不需要 FunCall AST 节点，
    /// 直接接收 Value 闭包 + 参数列表，供高阶函数算法层回调。
    Result<Value> invokeClosureSync(const Value& closure, const Value* args, size_t argCount, int line, int column);

    /// 类构造调用 ClassName(args)
    Value constructClassInstance(FunCall& node);

    // ---- R133-D fix: constructClassInstance 205 行拆为 thin orchestrator + 3 helper
    // （按子任务分组：字段复制 / init 环境准备 / init 体执行，与 R132-C visitImportStmt 模式同构）----
    /// 复制类继承链上的默认字段值到实例（含循环继承检测）
    void copyInheritedClassFields(Value& instance, ClassInfo* cls);
    /// 准备 init 方法执行环境：创建 initEnv、绑定 this、绑定参数（含类型检查 + 默认值求值）、bindInstance
    std::shared_ptr<Environment> setupInitEnvironment(ClassInfo* cls, FunDecl* initMethod,
                                                      std::vector<Value>& argValues, Value& instance, FunCall& node);
    /// 执行 init 方法体：压入调用帧、切换环境、压入类上下文、执行函数体、读回 this、关闭捕获
    void runInitMethodBody(ClassInfo* cls, FunDecl* initMethod, std::shared_ptr<Environment> initEnv, Value& instance,
                           FunCall& node);

    /// 普通函数/闭包调用 name(args)
    Value callNamedFunction(FunCall& node);

    // ---- P1 重构：visitMethodCall 辅助方法 ----

    /// 类实例方法调用（含 super.method() 处理）
    Value callInstanceMethod(MethodCall& node, Value& obj);

    /// 在类继承链中查找方法（含 super.method() 解析）。
    /// 输出 searchClass / searchClassName / isSuperCall / method / cachedParentEnv。
    /// 成功找到方法返回 true；类不存在或方法未找到返回 false
    /// （调用方负责抛出 runtimeError）。
    bool findMethodInClass(Value& obj, MethodCall& node, const ClassInfo*& searchClass, std::string& searchClassName,
                           bool& isSuperCall, FunDecl*& method, std::shared_ptr<Environment>& cachedParentEnv);

    /// 求值方法调用参数列表（含 F10 默认参数填充）。
    /// 含 A3 fix（实参求值后重新查找类避免悬垂）和
    /// AUDIT-P2-ROUND49 fix（默认参数求值后再次重新查找）。
    /// 通过引用更新 searchClass / method / cachedParentEnv。
    std::vector<Value> evaluateMethodArguments(MethodCall& node, const ClassInfo*& searchClass,
                                               const std::string& searchClassName, FunDecl*& method,
                                               std::shared_ptr<Environment>& cachedParentEnv);

    /// 设置方法调用环境（this 绑定、参数入栈）并执行方法体。
    /// 含 B3 CallFrameGuard、S2 RecursionGuard、RA-A MethodEnvGuard、
    /// AUDIT-P1-CORRECT classContextStack 压入"方法实际定义所在类"等 fix。
    /// 执行结束后写回 obj（按引用传递），并在 super 调用时写回 currentEnv_ 中的 this。
    Value invokeMethod(MethodCall& node, Value& obj, const ClassInfo* searchClass, const std::string& searchClassName,
                       bool isSuperCall, FunDecl* method, std::shared_ptr<Environment> cachedParentEnv,
                       std::vector<Value> argValues);

    /// 求值参数列表（消除 visitMethodCall 中重复的参数求值逻辑）
    std::vector<Value> evaluateArguments(const std::vector<std::shared_ptr<ASTNode>>& args);

    // ---- R132-C fix: visitImportStmt 208 行拆为 thin orchestrator + 3 helper ----

    /// 解析并校验模块路径：'\\'→'/'、去除 "./" 前缀、拒绝空路径/绝对路径/'..' 段。
    /// 包含 SEC-1 路径遍历防护与 BUG-MOD-1 Windows 驱动器路径修复。
    /// 通过 runtimeError 抛出错误，正常路径返回规范化后的路径。
    std::string resolveModulePath(ImportStmt& node);

    /// 加载或获取缓存的模块环境。命中缓存时检查文件 mtime，失效则重新加载。
    /// 未命中时调用 loader 加载源码、Lexer/Parser 解析、隔离 env 执行模块顶层语句。
    /// 使用 ModuleEnvGuard RAII 守卫统一管理异常路径的状态恢复。
    std::shared_ptr<Environment> loadModuleOrGetCached(const std::string& loaderPath, const std::string& cacheKey,
                                                       ImportStmt& node,
                                                       const std::function<std::string(const std::string&)>& loader,
                                                       const std::function<int64_t(const std::string&)>& mtimeChecker);

    /// 将模块导出名称导入到 currentEnv_。importAll=true 时导入全部导出名称；
    /// 否则按 node.names 列表原子性导入（先全验证后全定义，避免部分失败导致环境不一致）。
    /// BUG-M4 fix: cacheKey 与 loadModuleOrGetCached 入键一致。
    void importNamesFromModule(const std::string& cacheKey, ImportStmt& node, std::shared_ptr<Environment>& moduleEnv);

    // ---- B1 fix: 闭包仅捕获自由变量（静态分析 AST）----

    /// 计算函数的自由变量集合（函数体引用但未在函数内定义的变量）。
    /// 用于 visitFunDecl 时仅捕获实际需要的变量，而非整个环境快照。
    /// 递归处理嵌套函数：嵌套函数的自由变量若在外层函数内定义则不算外层自由变量，
    /// 否则归入外层自由变量。类方法不分析（使用 closureEnv 而非 capturedVars）。
    std::unordered_set<std::string> computeFreeVariables(const FunDecl& fn);

    /// collectFreeVars 的递归辅助函数（作用于作用域栈）。
    void collectFreeVars(const ASTNode& node, std::vector<std::unordered_set<std::string>>& scopes,
                         std::unordered_set<std::string>& freeVars);

    /// B1 fix: 在当前环境中直接执行函数体语句（不创建嵌套块作用域）。
    /// 函数体本身就是 Block，若通过 evaluate→visitBlock 执行会创建额外块作用域，
    /// 该块作用域可能从 envPool_ 取得与闭包 env weak_ptr 指向相同的 Environment 对象，
    /// resetForReuse 会修改其 parent 指针，形成 funEnv→blockEnv→funEnv 的父指针环，
    /// 导致变量查找死循环（MAX_SCOPE_DEPTH 后返回"未定义的变量"）。
    /// 直接在 funEnv 中执行语句可避免此问题，且符合参数与函数体变量同作用域的标准语义。
    void executeFunctionBody(Block& body);

    // ---- P1 重构：evaluateCondition 沙箱辅助方法 ----

    /// 条件断点沙箱快照：单个 Environment 的局部变量深拷贝。
    /// #1 fix: 快照作用域链所有变量绑定（深拷贝容器）。
    /// AUDIT-SANDBOX-DEEP: 深拷贝容器值，防止条件中的容器变异污染程序状态。
    struct EnvSnapshot {
        Environment* env;
        std::unordered_map<std::string, Value> variables;
    };

    /// 条件断点沙箱快照：实例字段深拷贝。
    /// #1 fix: 快照绑定实例字段（防止 this.field = val）。
    /// BUG-INTP-2 fix: 同时记录原始 InstanceData 指针，析构时比较 gcRootPtr()。
    /// AUDIT-P2-CORRECT fix: 存储 Environment* 以便析构时重新获取 inst 指针，
    /// 避免条件求值期间 variables map rehash 导致 inst 悬垂。
    struct InstSnapEntry {
        Environment* env;
        const void* gcRoot;
        std::unordered_map<std::string, Value> fields;
    };

    /// Phase 1: 沙箱环境创建——快照作用域链变量 + 绑定实例字段（深拷贝），
    /// 并将深拷贝副本安装到环境中作为求值副本。
    /// 保留 R82 P1 #3 fix（深拷贝变量/字段致沙箱失效修复）。
    void setupSandboxEnvironment(std::vector<EnvSnapshot>& envSnaps, std::map<Value*, InstSnapEntry>& instSnaps);

    /// Phase 2: 实际求值——假定沙箱已就绪，调用 node->accept 并返回结果。
    /// 不管理任何状态，状态保存/恢复由调用方负责。
    Value evalConditionExpr(ASTNode* node);

    /// Phase 3: 沙箱状态恢复——按"实例字段 → 局部变量"顺序恢复（H2 fix 不变量）。
    /// 由 SandboxGuard 析构调用以保证异常路径同样恢复。
    void restoreSandboxState(std::vector<EnvSnapshot>& envSnaps, std::map<Value*, InstSnapEntry>& instSnaps);

    // ---- P1 重构：visitVarDecl 辅助方法 ----

    /// 求值 VarDecl 初始化值：若有 initializer 则求值；
    /// 否则若有类类型注解则自动构造实例（含继承字段拷贝、0 必需参数 init 调用）；
    /// 其他情况返回 null。
    /// 保留 B8 fix（循环继承检测）、P1-2 fix（全默认参数 init）、
    /// AUDIT-P1 fix（init 默认参数绑定 + closeCapturedVariables）、
    /// AUDIT-P1-CORRECT fix（压入 init 方法定义所在类）。
    Value evalVarDeclValue(VarDecl& node);

    /// 绑定变量到当前环境（含类型检查、类型注解记录、capturedVarNames 清理、
    /// 原子性 tryDefineNew）。最后将 initVal 写入 lastValue_。
    /// 保留 AUDIT-P2-CORRECT fix（重声明捕获变量清理）、P21 fix（原子性检查+插入）。
    void bindVarDecl(VarDecl& node, Value initVal);

    // ---- R164 协程/生成器（重放模式）----

    /// 生成器函数调用拦截：检测 FunDecl.isGenerator，构造 Coroutine 值而非执行函数体。
    /// 在 callNamedFunction/callClosureValue 中调用，参数已求值完成。
    /// @param generatorDecl  生成器函数 AST（shared_ptr 共享所有权）
    /// @param closureEnv     定义时环境（闭包捕获）
    /// @param argValues      已求值的调用参数（含默认参数填充）
    /// @return Coroutine 值
    Value makeCoroutineValue(std::shared_ptr<FunDecl> generatorDecl, std::shared_ptr<Environment> closureEnv,
                             std::vector<Value> argValues);

    /// 协程 .next() 方法实现：重放模式核心。
    /// 设置 currentCoroutineTargetYieldId_ = cd->currentYieldId，从头执行生成器函数体。
    /// 捕获 YieldSignal 时递增 currentYieldId 并返回 yield 值；
    /// 函数体自然结束或 ReturnException 时标记 done=true 并返回最终值。
    /// @param coroVal  Coroutine 值（可变引用，修改其 currentYieldId/done/currentValue）
    /// @return 本次 yield 的值（或函数返回值，或 null 若已 done）
    Value callCoroutineNext(Value& coroVal);

    /// 协程方法分发：处理 .next() / .done() 方法调用。
    /// @param method  方法名（"next" 或 "done"）
    /// @param obj     Coroutine 值（可变引用，.next() 会修改状态）
    /// @param args    已求值参数列表（next/done 期望 0 个参数）
    /// @param line    调用行号
    /// @param col     调用列号
    /// @return 方法返回值 + 是否修改对象（.next() 修改 → true，.done() 不修改 → false）
    /// @throws RuntimeError 方法名未知或参数数量错误时
    BuiltinMethodResult handleCoroutineMethod(const std::string& method, Value& obj, const std::vector<Value>& args,
                                              int line, int col);
};
