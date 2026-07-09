#pragma once

// ============================================================
// IdeController — 业务逻辑层 Facade（ARCH-11 重构）
// ------------------------------------------------------------
// 原 God Object 已拆分为 4 个独立协作类：
//   - PipelineRunner    词法/语法/编译/格式化管线
//   - WorkerManager     Worker 线程管理 + Interpreter/Debugger 所有权
//   - DebugCoordinator  调试状态/回调/步进控制
//   - VmStepper         VM 单步执行状态机
//
// IdeController 作为 Facade：
//   - 持有 4 个协作类成员
//   - 共享 Interpreter / DebugController 所有权（shared_ptr）
//   - 转发公共 API 到对应协作类（保持 GUI 层 API 不变）
//   - 协调跨协作类操作（如 prepareRun = pipeline + workerMgr + vmStepper）
//   - 转发协作类信号到 GUI 层
// ============================================================

#include <QCoreApplication>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "DebugCoordinator.h"
#include "Diagnostic.h"
#include "PipelineRunner.h"
#include "VmStepper.h"
#include "WorkerManager.h"
#include "debug/DebugController.h"
#include "interpreter/Interpreter.h"
#include "interpreter/Value.h"

class IdeController : public QObject {
    Q_OBJECT

public:
    explicit IdeController(QObject* parent = nullptr);
    ~IdeController();

    // ---- 类型重导出（保持 GUI 层 IdeController::XxxYyy 用法不变）----
    using VmStepResult = VmStepper::VmStepResult;
    using VmStepMode = VmStepper::VmStepMode;
    using PipelineStatus = PipelineRunner::PipelineStatus;
    using PipelineResult = PipelineRunner::PipelineResult;

    // ---- 引擎访问（转发到 PipelineRunner）----
    // D3 fix: 补齐 const 重载，允许 const 上下文（如 DebugCoordinator、
    // const IdeController& 形参）只读访问引擎。与 PipelineRunner.h 的 const/非 const
    // 双重载对齐。
    Formatter& formatter() { return pipeline_.formatter(); }
    Lexer& lexer() { return pipeline_.lexer(); }
    Parser& parser() { return pipeline_.parser(); }
    Compiler& compiler() { return pipeline_.compiler(); }
    const Formatter& formatter() const { return pipeline_.formatter(); }
    const Lexer& lexer() const { return pipeline_.lexer(); }
    const Parser& parser() const { return pipeline_.parser(); }
    const Compiler& compiler() const { return pipeline_.compiler(); }

    // ---- VM 状态访问（转发到 VmStepper）----
    // B6 fix: 语义化的 VM 调试状态快照接口（GUI 仅通过这些方法读取 VM 状态）
    // A1 fix: 栈式 VM 返回操作数栈；RegisterVM 返回寄存器窗口（同形 vector<Value>）
    std::vector<Value> getVmStack() const { return vmStepper_.getStack(); }
    std::unordered_map<std::string, Value> getVmGlobals() const { return vmStepper_.getGlobals(); }
    size_t getVmCurrentIP() const { return vmStepper_.getCurrentIP(); }
    // A1 fix: 操作码统一返回名称字符串，兼容 OpCode (栈式) / RegOp (寄存器式)
    std::string getVmCurrentOpCodeName() const { return vmStepper_.getCurrentOpCodeName(); }
    int getVmCurrentLine() const { return vmStepper_.getCurrentLine(); }
    std::string getVmCurrentChunkName() const { return vmStepper_.getCurrentChunkName(); }
    std::string getVmLastError() const { return vmStepper_.getLastError(); }
    int getVmLastErrorLine() const { return vmStepper_.getLastErrorLine(); }
    size_t getVmFrameCount() const { return vmStepper_.getFrameCount(); }
    // A1 fix: 暴露当前活跃后端模式给 GUI（用于切换栈/寄存器视图）
    bool isVmRegisterMode() const { return vmStepper_.isRegisterMode(); }
    // BUG-DBG-6 fix: 暴露 VM 调用栈快照，供 GUI 在 VM 调试模式下显示调用栈
    std::vector<CallStackEntry> getVmCallStack() const { return vmStepper_.getCallStack(); }

    // ---- 调试接口（转发到 DebugCoordinator）----
    // OPT-1: setBreakpointCondition / setBreakpoints 变更后通知订阅面板。
    // 保持 inline：MagicCommands.cpp（minilang_core）依赖内联符号。
    void setBreakpointCondition(int line, const std::string& condition) {
        debugCoord_.setBreakpointCondition(line, condition);
        notifyVmStateChanged();
    }
    bool hasBreakpoints() const { return debugCoord_.hasBreakpoints(); }
    // P1-2: 断点查询接口（供 BreakpointConditionPanel 消费）
    QSet<int> getBreakpoints() const { return debugCoord_.getBreakpoints(); }
    std::string getBreakpointCondition(int line) const { return debugCoord_.getBreakpointCondition(line); }
    // BUG-DBG-AUDIT-2 fix: VM 模式活跃时分派到 VmStepper 的 hitCount，
    // 否则转发到 Interpreter 模式的 debugCoord_。原实现无条件转发到 debugCoord_，
    // 导致 VM 模式调试时 BreakpointConditionPanel::refreshLive 获取的 hitCount 始终为 0。
    // VM 模式活跃判定：vmStepper_.isRunning()（RUN 异步执行中）||
    //                 vmStepper_.isInitialized()（STEP 间歇期，VM 已初始化）。
    int getBreakpointHitCount(int line) const {
        if (vmStepper_.isRunning() || vmStepper_.isInitialized()) {
            return vmStepper_.getBreakpointHitCount(line);
        }
        return debugCoord_.getBreakpointHitCount(line);
    }
    std::vector<VariableSnapshot> getDebugVariableSnapshot() const { return debugCoord_.getDebugVariableSnapshot(); }
    std::vector<CallStackEntry> getDebugCallStack() const { return debugCoord_.getDebugCallStack(); }
    /// AUDIT-P1 fix: 暴露调试暂停状态，供面板 autoTimer 在 resume 期间停止并发访问。
    /// 调试运行中（resume 后 worker 活跃）时调用 getDebugCallStack/getDebugVariableSnapshot
    /// 会与 worker 线程并发访问解释器内部数据结构（unordered_map/vector），导致 UB。
    /// 面板应在 autoTimer 触发前检查 isDebugPaused() == true 才安全调用快照接口。
    bool isDebugPaused() const { return debugCoord_.isPaused(); }

    // ---- 关键字接口（转发到 PipelineRunner.lexer）----
    const std::unordered_map<std::string, TokenType>& getKeywords() const { return pipeline_.lexer().keywords(); }

    // ---- REPL 接口（转发到 Interpreter）----
    /// 保留 REPL AST 引用（防止类/闭包 body 指针悬空）
    void retainReplAst(std::unique_ptr<Block> ast) { interpreter_->retainReplAst(std::move(ast)); }
    /// BUG-REPL-AUDIT-9 fix: 执行 REPL 程序，返回求值结果（异常向上传播由调用方处理）。
    /// 非内联实现（见 IdeController.cpp）：执行前调用 setupReplModuleCallbacks() 确保
    /// Interpreter 已设置模块加载器，使 REPL 中 import 语句可用。
    Value executeRepl(Block& program);
    /// 清除指定模块的缓存（REPL reload 命令使用，下次 import 重新加载源码）
    void clearModuleCache(const std::string& path) { interpreter_->clearModuleCache(path); }
    /// 清除所有模块缓存
    void clearAllModuleCache() { interpreter_->clearAllModuleCache(); }
    /// 请求中止当前 REPL 异步执行（closeEvent 超时路径使用）
    void requestReplStop() { interpreter_->requestStop(); }

    /// ROUND-69 fix: REPL %reset 完整重置 REPL 环境（清空变量/函数/类/模块/AST）
    /// 前置条件：调用方已确保无正在执行的任务（isRunning()==false && !replRunning）
    void resetReplEnvironment() {
        interpreter_->resetReplEnvironment();
        vmStepper_.reset();
    }

    // P0-3 fix (F11): 暴露 Interpreter 当前作用域变量名列表，
    // 供 GUI 错误增强（ErrorHintEngine 拼写建议）使用。
    /// 返回当前 Interpreter 作用域链上所有可见变量名（去重，子作用域优先）。
    /// 若 Interpreter 未运行或环境为空，返回空向量。
    std::vector<std::string> getReplScopeVariableNames() const;

    /// N1 fix: 同步运行 MiniLang 源码并捕获所有 print 输出。
    /// 用于 LabManualPanel EXPECTED_OUTPUT 练习自动判分。
    /// 创建独立的 Lexer/Parser/Interpreter 管线，不影响 IDE 当前状态。
    /// @return 捕获的输出文本；若词法/语法/运行时错误，返回 "!ERROR: <描述>"
    std::string runStringCaptureOutput(const std::string& source);

    // ---- 管线操作（转发到 PipelineRunner）----
    bool runLexer(const std::string& source) { return pipeline_.runLexer(source); }
    bool runParser() { return pipeline_.runParser(); }
    /// 执行字节码编译，返回 true 表示无错误。
    /// 编译成功后同步更新 VmStepper 的编译结果，使 VM 步进可用。
    bool runCompiler();
    bool formatCode(std::string& formatted) { return pipeline_.formatCode(formatted); }
    PipelineResult runFrontendPipeline(const std::string& source) { return pipeline_.runFrontendPipeline(source); }

    // ---- Worker 线程管理（协调 PipelineRunner + WorkerManager + VmStepper）----
    /// 准备运行（词法+解析+创建 worker），返回 true 表示已就绪
    /// filePath: 当前文件路径（用于模块加载的相对路径解析），为空表示未保存文件
    bool prepareRun(bool isDebug, const std::string& source, const std::string& filePath = "");

    /// BUG-REPL-AUDIT-9 fix: 通知当前活动文件路径（GUI 在文件切换/加载/保存时调用）。
    /// 使 REPL 在未 Run 的情况下也能基于当前文件目录解析 import 相对路径。
    /// prepareRun 会覆盖此值（Run 时以传入 filePath 为准）。
    void setActiveFilePath(const std::string& path) { currentFilePath_ = path; }
    void startWorker() { workerMgr_.startWorker(); }
    bool stopForClose(int timeoutMs = 3000) { return workerMgr_.stopForClose(timeoutMs); }
    void forceStop() { workerMgr_.forceStop(); }
    bool isRunning() const { return workerMgr_.isRunning(); }
    bool isDebugRun() const { return workerMgr_.isDebugRun(); }

    /// ROUND-66 P0 fix: 清空 IdeController 及其 4 个协作成员（pipeline_/workerMgr_/
    /// debugCoord_/vmStepper_）的所有待处理 Qt 事件。
    /// Qt6 的 disconnect() 不移除已投递的 QMetaCallEvent（queued slot lambda），
    /// 若这些残留事件在析构链中被 dispatch（如 ADS QSS 重算触发 repaint），
    /// 会访问已析构的成员 → UAF（读取访问权限冲突）。closeEvent 和 ~Ide 中调用。
    void clearPendingEvents() {
        QCoreApplication::removePostedEvents(this);
        QCoreApplication::removePostedEvents(&pipeline_);
        QCoreApplication::removePostedEvents(&workerMgr_);
        QCoreApplication::removePostedEvents(&debugCoord_);
        QCoreApplication::removePostedEvents(&vmStepper_);
    }

    // ---- 调试操作（转发到 DebugCoordinator）----
    void setupDebug(const QSet<int>& breakpoints, const QMap<int, std::string>& conditions) {
        debugCoord_.setupDebug(breakpoints, conditions);
        notifyVmStateChanged();
    }
    void setBreakpoints(const QSet<int>& breakpoints) {
        debugCoord_.setBreakpoints(breakpoints);
        notifyVmStateChanged();
    }
    void stepIn() { debugCoord_.stepIn(); }
    void stepOver() { debugCoord_.stepOver(); }
    void stepOut() { debugCoord_.stepOut(); }
    void resume() { debugCoord_.resume(); }
    void stop() { debugCoord_.stop(); }

    // ---- VM 操作（转发到 VmStepper）----
    // OPT-1: 在状态变更后调用 notifyVmStateChanged() 通知订阅面板，
    // 替代面板的 500ms QTimer 轮询（降低 CPU 开销 + 提升响应即时性）。
    // 保持 inline：MagicCommands.cpp（minilang_core）依赖内联符号，测试目标不链接 IdeController.cpp。
    VmStepResult vmStep() {
        auto result = vmStepper_.step();
        notifyVmStateChanged();
        return result;
    }
    VmStepResult vmStepByMode(VmStepMode mode) {
        auto result = vmStepper_.stepByMode(mode);
        // RUN 模式返回 RUNNING 后状态变更由 vmRunPaused 信号桥接通知；
        // 同步步进模式（STEP_IN/OVER/OUT）在此立即通知。
        if (result != VmStepResult::RUNNING) {
            notifyVmStateChanged();
        }
        return result;
    }
    void setVmBreakpoints(const QSet<int>& breakpoints) {
        vmStepper_.setBreakpoints(breakpoints);
        notifyVmStateChanged();
    }
    /// #4 fix: 设置 VM 模式条件断点
    void setVmBreakpointConditions(const QMap<int, std::string>& conditions) {
        vmStepper_.setBreakpointConditions(conditions);
        notifyVmStateChanged();
    }
    void vmStop() {
        vmStepper_.stop();
        notifyVmStateChanged();
    }
    void vmReset() {
        vmStepper_.reset();
        notifyVmStateChanged();
    }
    bool isVmRunning() const { return vmStepper_.isRunning(); }
    bool isVmInitialized() const { return vmStepper_.isInitialized(); }

    // ---- OPT-1: VM 状态变更观察者 API ----
    // 纯 C++ 观察者模式（非 Qt 信号），用于替代 5 个面板的 500ms QTimer 轮询。
    // 设计理由：Qt 信号需要 IdeController 的 moc 产物（staticMetaObject），
    // 而测试目标 minilang_tests 不链接 app/IdeController.cpp（仅链接 minilang_core），
    // 会导致链接失败。std::function 监听器为纯头文件内联，无 moc 依赖，面板 .cpp
    // 可安全编译进测试目标（setController 在测试中传 nullptr，不会注册监听器）。
    //
    // 状态变更点（notifyVmStateChanged 调用位置）：
    //   - vmStep / vmStepByMode 返回后（步进/暂停/结束/错误）
    //   - vmStop / vmReset 后（状态清空）
    //   - setBreakpoints / setVmBreakpoints / setVmBreakpointConditions 后（断点变化）
    //   - setBreakpointCondition 后（条件变化）
    //   - prepareRun 成功后（新运行启动）
    //   - runCompiler 成功后（VM 步进就绪）
    //   - vmStepper_::vmRunPaused 信号（RUN 模式异步暂停）
    //   - debugCoord_::pausedAt 信号（Interpreter 调试暂停）
    //   - workerMgr_::workerFinished 信号（运行结束）
    using VmStateChangedCallback = std::function<void()>;
    /// AUDIT-P0 fix: 增加 owner 参数用于反注册。
    /// owner 通常是注册面板的 this 指针，面板析构时调用 removeVmStateChangedListener(owner) 注销。
    void addVmStateChangedListener(void* owner, VmStateChangedCallback cb) {
        // PERF-ROUND53 fix: 预留容量避免 push_back 触发 realloc，使 notifyVmStateChanged
        // 的索引迭代在回调期间不会因 push_back 导致 vector 重新分配。
        if (vmStateChangedListeners_.capacity() == vmStateChangedListeners_.size())
            vmStateChangedListeners_.reserve(vmStateChangedListeners_.size() * 2 + 16);
        vmStateChangedListeners_.push_back({owner, std::move(cb)});
    }
    /// AUDIT-P0 fix: 反注册 VM 状态变更监听器。
    /// 6 个面板（BytecodeTrace/CallStack/VariableInspector/BreakpointCondition/
    /// MemoryModel/VmStackSandbox）在 setController 中注册捕获裸 this 的 lambda，
    /// 面板析构后 controller 仍存活期间任何 notifyVmStateChanged 调用都会 UAF。
    /// owner 为注册时传入的 this 指针，用于匹配注销。
    /// 实现采用延迟清除：标记为空 std::function，notifyVmStateChanged 跳过空 cb。
    void removeVmStateChangedListener(void* owner) {
        for (auto& cb : vmStateChangedListeners_) {
            if (cb.owner == owner) {
                cb.fn = nullptr;
            }
        }
    }

    // A1 fix: 启用/禁用 RegisterVM 后端（同步 Compiler 与 VmStepper）
    // 启用后 compile() 走 AST → IR → RegisterBytecode 路径，VmStepper 转发到 regVm_。
    // 切换时 VmStepper 自动 reset 防止状态污染；调用方应在 VM 未运行时切换。
    void setUseRegisterVM(bool enabled) {
        // BUG-IDE-23 fix: VM 运行中切换后端会导致 frame.chunk/RegChunk 悬垂，
        // 必须先停止 VM 再切换引擎。
        if (vmStepper_.isRunning()) {
            emit genericError("VM 正在运行，请先停止再切换引擎");
            return;
        }
        pipeline_.compiler().setUseRegisterVM(enabled);
        vmStepper_.setUseRegister(enabled);
    }
    bool getUseRegisterVM() const { return pipeline_.compiler().getUseRegisterVM(); }

    // ---- 状态访问（转发到 PipelineRunner）----
    const std::vector<Token>& lastTokens() const { return pipeline_.lastTokens(); }
    const CompileResult& lastCompileResult() const { return pipeline_.lastCompileResult(); }
    Block* astRoot() const { return pipeline_.astRootPtr(); }
    const DiagnosticBag& lexerDiagnostics() const { return pipeline_.lexerDiagnostics(); }
    const DiagnosticBag& parserDiagnostics() const { return pipeline_.parserDiagnostics(); }
    const DiagnosticBag& compilerDiagnostics() const { return pipeline_.compilerDiagnostics(); }

    /// 方向三：获取最近一次 IR 构建结果（用于 IR 可视化）。
    /// 仅当 compiler().setUseIR(true) 且 compile() 成功后有效。
    /// 返回 nullptr 表示未启用 IR 路径或构建失败。
    const IRFunction* lastIR() const { return pipeline_.compiler().getLastIR(); }

    /// 方向四：获取 main 函数的 IR→字节码偏移映射（IR 调试器集成）。
    const std::vector<std::pair<size_t, size_t>>& lastIRToBytecodeOffset() const {
        return pipeline_.compiler().getLastIRToBytecodeOffset();
    }

signals:
    void outputReady(const QString& text);
    void runOk();
    void stoppedByUser();
    void runtimeError(const QString& msg, int line, int column);
    void genericError(const QString& msg);
    void pausedAt(int line);
    void workerFinished(bool wasDebug);
    void vmRunPaused(VmStepResult result);
    void diagnosticsReady(const DiagnosticBag& bag);

private:
    // ---- 共享引擎（WorkerManager 和 DebugCoordinator 共享所有权）----
    // MEM-01 fix: shared_ptr 共享所有权，worker 线程持有 Interpreter 期间 IdeController 析构不会悬垂
    std::shared_ptr<Interpreter> interpreter_;
    // MEM-01 fix: 无 Qt parent（shared_ptr 独占管理），避免双重所有权
    std::shared_ptr<DebugController> debugger_;

    // ---- 4 个协作类（ARCH-11 拆分）----
    // 注意：成员 QObjects 不设 parent（避免 QObject 自动删除 + 成员析构的双重释放）
    PipelineRunner pipeline_;
    WorkerManager workerMgr_;
    DebugCoordinator debugCoord_;
    VmStepper vmStepper_;

    // VM-IMPORT: 当前文件路径（供 runCompiler 设置 Compiler 模块加载器的相对路径基准）
    std::string currentFilePath_;

    // ---- OPT-1: VM 状态变更观察者订阅者列表 ----
    // 6 个面板（CallStack/VariableInspector/BytecodeTrace/MemoryModel/BreakpointCondition/
    // VmStackSandbox）在 setController 时注册回调，替代 500ms QTimer 轮询。
    // AUDIT-P0 fix: 增加 owner 字段支持反注册，避免面板析构后 UAF。
    struct VmStateChangedListener {
        void* owner = nullptr;
        VmStateChangedCallback fn;
    };
    std::vector<VmStateChangedListener> vmStateChangedListeners_;
    void notifyVmStateChanged() {
        // PERF-ROUND53 fix: 原实现每次拷贝整个 vector（含 std::function，可能触发堆分配）。
        // removeVmStateChangedListener 仅将 fn 置 null（延迟清除，不 erase），
        // addVmStateChangedListener 已 reserve 防止 realloc，迭代期间 vector 大小不变。
        // 改用索引迭代避免拷贝。防御性检查 i < size() 应对理论上的回调期间 push_back。
        const size_t n = vmStateChangedListeners_.size();
        for (size_t i = 0; i < n; ++i) {
            if (i < vmStateChangedListeners_.size() && vmStateChangedListeners_[i].fn)
                vmStateChangedListeners_[i].fn();
        }
    }

    /// VM-IMPORT: 为 Compiler 设置模块加载器（对齐 WorkerManager 为 Interpreter 设置的 loader）
    /// 基于 filePath 的目录解析相对模块路径，自动添加 .mini 后缀
    void setupCompilerModuleLoader(const std::string& filePath);

    /// BUG-REPL-AUDIT-9 fix: 为 REPL 路径补设 Interpreter 模块加载器/mtimeChecker/currentFilePath。
    /// 若 Interpreter 已有 moduleLoader_（先 Run 过），不重复设置，避免覆盖 Run 时建立的
    /// baseDir（防止 Run 后 REPL 用不同的 baseDir 导致模块缓存基准不一致）。
    /// 路径遍历防护（拒绝 ".." 和绝对路径）由 Interpreter::visitImportStmt 前置检查保证。
    void setupReplModuleCallbacks();
};
