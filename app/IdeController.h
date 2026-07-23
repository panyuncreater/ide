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
#include <list> // R117: watchAstCache LRU 实现
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "DebugCoordinator.h"
#include "Diagnostic.h"
#include "PipelineRunner.h"
#include "VmStepper.h"
#include "WorkerManager.h"
#include "debug/DebugController.h"
#include "debug/ExecutionTraceRecorder.h" // R114: TraceBackend 定义
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
    std::unordered_map<std::string, Value> getReplGlobals() const {
        if (interpreter_) {
            auto genv = interpreter_->getGlobalEnvironment();
            if (genv) {
                return genv->snapshotLocalVariables();
            }
        }
        return {};
    }
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

    // ---- REPL 管线状态更新（供 ReplPanel 在 REPL 执行后调用）----
    void setReplPipelineState(const std::string& source) { pipeline_.setReplPipelineState(source); }

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
        pipeline_.resetPipelineState();
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

    // ---- R117 Watch 表达式求值（调试器拓展）----
    // 在当前调试暂停上下文中求值表达式，返回完整值信息供 WatchPanel 显示。
    // 沙箱求值，不影响 VM/Interpreter 状态。
    // 支持两种暂停模式：
    //   - VM 模式（isVmInitialized() || isVmRunning()）：注入 VM globals + 当前帧 locals
    //   - Interpreter 调试暂停模式（isDebugPaused()）：注入 debugCoord_ 变量快照
    // 其他状态返回 ok=false + 错误描述。
    struct WatchResult {
        bool ok = false;       // 求值是否成功
        Value value;           // 求值结果值（ok=false 时为 null）
        std::string error;     // 错误信息（ok=false 时填充）
        std::string typeName;  // 类型名（int/float/bool/null/string/array/dict/instance/closure/...）
        std::string valueRepr; // 值的字符串表示（Value::toString 格式）
    };
    /// R117: 在当前调试暂停上下文中求值 watch 表达式。
    /// @param expr 表达式源码（无需分号，自动补全）
    WatchResult evaluateWatchExpression(const std::string& expr);

    // ---- R161 调试器 REPL（暂停时执行任意表达式/语句）----
    // 与 evaluateWatchExpression 的区别：
    //   - 允许多条语句（Block 而非单表达式）
    //   - 允许 var/print/赋值语句（沙箱内副作用不写回主程序，阶段1只读）
    //   - 用 executeRepl 替代 evaluateCondition（不做沙箱恢复，临时 Interpreter 独立实例）
    //   - 捕获 print 输出到 output 成员（供 ReplPanel 显示）
    // 线程安全：主线程同步求值（Interpreter 暂停期 worker 阻塞、VM 暂停期主线程空闲）
    /// R161: 调试器 REPL 求值。返回 ok/value/typeName/valueRepr/output。
    /// @param source 源码（无需分号，自动补全；支持多条语句）
    /// @param[out] output 捕获的 print 输出（每行一个元素）
    WatchResult evaluateDebuggerRepl(const std::string& source, std::string* output = nullptr);

    // ---- R121 调用栈帧切换 ----
    // 让用户在调试暂停时可"上移"到调用栈任意帧查看局部变量（类似 GDB `frame N`）。
    // selectedFrame_ = -1 表示未选/默认栈顶；>= 0 表示选中的帧索引（0=栈底 main）。
    // 由 CallStackPanel::onFrameSelected 触发 setSelectedFrame，所有面板通过 vmStateChanged
    // 监听器自动刷新。VariableInspector / WatchPanel 通过 getSelectedFrameLocals 获取所选帧上下文。
    // 边界：越界时 fallback 到栈顶；stop/reset/prepareRun 时 clear。
    /// 设置选中的调用栈帧索引（-1=未选/默认栈顶）。变化时通知订阅面板刷新。
    void setSelectedFrame(int depth) {
        if (selectedFrame_ == depth)
            return;
        selectedFrame_ = depth;
        notifyVmStateChanged();
    }
    /// 获取当前选中的调用栈帧索引（-1=未选/默认栈顶）
    int getSelectedFrame() const { return selectedFrame_; }
    /// 清除选中帧（在 stop/reset/prepareRun 等状态重置点调用）
    /// @note 不触发 notifyVmStateChanged，由调用方（stop/vmStop/vmReset/prepareRun）
    ///       统一通知，避免双重通知。
    void clearSelectedFrame() { selectedFrame_ = -1; }
    /// 获取所选帧的局部变量（用于 VariableInspector 显示 + Watch 求值注入）。
    /// selectedFrame_ == -1 时返回当前帧（栈顶）locals。
    /// VM 模式 / Interpreter 调试暂停模式均支持。其他状态返回空。
    /// 越界时 fallback 到栈顶（与 GDB `frame N` 行为一致）。
    /// @note R121-build fix: 改为 inline 以避免 VariableInspectorPanel.cpp 依赖
    ///       IdeController.cpp 的非 inline 符号（minilang_tests 不链接 IdeController.cpp）。
    ///       所依赖的 vmStepper_ / debugCoord_ 方法均已 inline。
    std::vector<std::pair<std::string, Value>> getSelectedFrameLocals() const {
        std::vector<std::pair<std::string, Value>> result;
        bool vmMode = vmStepper_.isRunning() || vmStepper_.isInitialized();
        bool interpPaused = debugCoord_.isPaused();
        if (!vmMode && !interpPaused) {
            return result;
        }
        if (vmMode) {
            size_t frameCount = vmStepper_.getFrameCount();
            if (frameCount == 0)
                return result;
            size_t targetIdx = static_cast<size_t>(selectedFrame_);
            // selectedFrame_ == -1 或越界 → fallback 到栈顶（frameCount - 1）
            if (selectedFrame_ < 0 || targetIdx >= frameCount) {
                targetIdx = frameCount - 1;
            }
            auto locals = vmStepper_.getFrameLocalsAt(targetIdx);
            for (const auto& kv : locals) {
                result.emplace_back(kv.first, kv.second);
            }
        } else {
            // Interpreter 调试暂停模式：通过 debugCoord_ 直接访问 CallFrame.env
            // 越界由 debugCoord_.getFrameLocalsAt 内部处理（返回空），此处需手动 fallback 到栈顶
            if (selectedFrame_ < 0) {
                // -1 表示栈顶：用 getDebugVariableSnapshot() 拿当前帧 locals（保持 R117 行为）
                for (const auto& snap : debugCoord_.getDebugVariableSnapshot()) {
                    result.emplace_back(snap.name, snap.value);
                }
            } else {
                auto locals = debugCoord_.getFrameLocalsAt(selectedFrame_);
                if (locals.empty()) {
                    // 越界 fallback：尝试栈顶
                    for (const auto& snap : debugCoord_.getDebugVariableSnapshot()) {
                        result.emplace_back(snap.name, snap.value);
                    }
                } else {
                    result = std::move(locals);
                }
            }
        }
        return result;
    }

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
    // R121: 调试停止时清除选中帧（新调试会话不应继承旧选中状态）
    void stop() {
        debugCoord_.stop();
        clearSelectedFrame();
        notifyVmStateChanged();
    }

    /// R98 runToCursor: 运行到指定行暂停（Facade 分派到 Interpreter 或 VM 路径）。
    /// 分派规则：
    ///   - VM 路径活跃（vmStepper_.isRunning() || vmStepper_.isInitialized()）：
    ///     设置 VM 临时断点后调用 vmStepByMode(RUN) 启动异步分批执行。
    ///   - 否则（Interpreter 调试模式）：设置 Interpreter 临时断点后调用 resume()。
    /// 临时断点是一次性的——命中后自动清除，不影响用户断点。
    /// @param line 目标行号（必须 > 0，否则忽略）
    /// @note 调用方应先检查 isRunning()/isDebugPaused()/isVmInitialized() 之一为 true。
    void runToCursor(int line) {
        if (vmStepper_.isRunning() || vmStepper_.isInitialized()) {
            vmStepper_.setTempBreakpoint(line);
            vmStepByMode(VmStepMode::RUN);
        } else {
            debugCoord_.setTemporaryBreakpoint(line);
            debugCoord_.resume();
        }
    }

    // ---- R104 调试器拓展：Logpoint / Function BP / Exception BP ----
    // Facade 分派：VM 路径活跃时转发到 VmStepper，否则转发到 DebugCoordinator（Interpreter）。
    // 日志回调由 GUI 注入一次，同时设置到两个路径（避免切换后端时丢失）。
    void setLogCallback(std::function<void(const std::string&)> cb) {
        auto cbCopy = cb;
        debugCoord_.setLogCallback(std::move(cb));
        vmStepper_.setLogCallback(std::move(cbCopy));
    }
    void setBreakpointKind(int line, BreakpointKind kind) {
        debugCoord_.setBreakpointKind(line, kind);
        vmStepper_.setBreakpointKind(line, kind);
        notifyVmStateChanged();
    }
    BreakpointKind getBreakpointKind(int line) const {
        if (vmStepper_.isRunning() || vmStepper_.isInitialized())
            return vmStepper_.getBreakpointKind(line);
        return debugCoord_.getBreakpointKind(line);
    }
    void setLogpointMessage(int line, const std::string& msg) {
        debugCoord_.setLogpointMessage(line, msg);
        vmStepper_.setLogpointMessage(line, msg);
        notifyVmStateChanged();
    }
    std::string getLogpointMessage(int line) const {
        if (vmStepper_.isRunning() || vmStepper_.isInitialized())
            return vmStepper_.getLogpointMessage(line);
        return debugCoord_.getLogpointMessage(line);
    }

    void setFunctionBreakpoint(const std::string& name) {
        debugCoord_.setFunctionBreakpoint(name);
        vmStepper_.setFunctionBreakpoint(name);
        notifyVmStateChanged();
    }
    void removeFunctionBreakpoint(const std::string& name) {
        debugCoord_.removeFunctionBreakpoint(name);
        vmStepper_.removeFunctionBreakpoint(name);
        notifyVmStateChanged();
    }
    void setFunctionBreakpoints(const QSet<std::string>& names) {
        debugCoord_.setFunctionBreakpoints(names);
        vmStepper_.setFunctionBreakpoints(names);
        notifyVmStateChanged();
    }
    QSet<std::string> getFunctionBreakpoints() const {
        if (vmStepper_.isRunning() || vmStepper_.isInitialized())
            return vmStepper_.getFunctionBreakpoints();
        return debugCoord_.getFunctionBreakpoints();
    }
    bool hasFunctionBreakpoint(const std::string& name) const {
        if (vmStepper_.isRunning() || vmStepper_.isInitialized())
            return vmStepper_.hasFunctionBreakpoint(name);
        return debugCoord_.hasFunctionBreakpoint(name);
    }
    int getFunctionBreakpointHitCount(const std::string& name) const {
        if (vmStepper_.isRunning() || vmStepper_.isInitialized())
            return vmStepper_.getFunctionBreakpointHitCount(name);
        return debugCoord_.getFunctionBreakpointHitCount(name);
    }

    void setExceptionBreakpointEnabled(bool enabled) {
        debugCoord_.setExceptionBreakpointEnabled(enabled);
        vmStepper_.setExceptionBreakpointEnabled(enabled);
        notifyVmStateChanged();
    }
    bool isExceptionBreakpointEnabled() const {
        if (vmStepper_.isRunning() || vmStepper_.isInitialized())
            return vmStepper_.isExceptionBreakpointEnabled();
        return debugCoord_.isExceptionBreakpointEnabled();
    }
    int getExceptionBreakpointHitCount() const {
        if (vmStepper_.isRunning() || vmStepper_.isInitialized())
            return vmStepper_.getExceptionBreakpointHitCount();
        return debugCoord_.getExceptionBreakpointHitCount();
    }

    // ---- R161 Watchpoint（数据断点）facade ----
    // 添加数据断点（监视变量/字段被修改时暂停）。
    // VM 路径转发到 vmStepper_；Interpreter 路径暂不支持 watchpoint（仅 VM 路径）。
    void setWatchpoint(const WatchpointInfo& wp) {
        vmStepper_.setWatchpoint(wp);
        notifyVmStateChanged();
    }
    void removeWatchpoint(const std::string& varName, const std::string& fieldName = "") {
        vmStepper_.removeWatchpoint(varName, fieldName);
        notifyVmStateChanged();
    }
    void clearWatchpoints() {
        vmStepper_.clearWatchpoints();
        notifyVmStateChanged();
    }
    const QVector<WatchpointInfo>& getWatchpoints() const { return vmStepper_.getWatchpoints(); }
    bool hasWatchpoints() const { return vmStepper_.hasWatchpoints(); }

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
        clearSelectedFrame(); // R121: VM 停止时清除选中帧
        notifyVmStateChanged();
    }
    void vmReset() {
        vmStepper_.reset();
        clearSelectedFrame(); // R121: VM 重置时清除选中帧
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
    /// ROUND-67 P2 fix: 清空所有 vmStateChangedListener 回调。
    /// closeEvent 中调用，防止 closeEvent 后续操作（vmStop/stopForClose 等）触发
    /// notifyVmStateChanged 时，7 个教学面板的 onVmStateChanged 回调访问正在清理的 UI。
    void clearVmStateChangedListeners() {
        for (auto& cb : vmStateChangedListeners_) {
            cb.fn = nullptr;
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

    // R114: 可回放执行时间轴——录制开关转发
    // VM 路径（栈式/寄存器式）通过 vmStepper_ 启用，Interpreter 路径通过 interpreter_ 启用。
    // GUI 层（ExecutionTimelinePanel）切换录制时同时调用两个方法，覆盖三条执行路径。
    // 跨线程安全：vmStepper_ 主线程访问；interpreter_ 内部使用 atomic<bool>。
    void setVmRecordingEnabled(bool enabled, TraceBackend backend) { vmStepper_.setRecordingEnabled(enabled, backend); }
    void setInterpreterRecordingEnabled(bool enabled) {
        if (interpreter_) {
            interpreter_->setRecordingEnabled(enabled);
        }
    }

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

    // R121 调用栈帧切换：用户选中的帧索引（-1=未选/默认栈顶；>= 0=帧索引，0=栈底 main）
    // 由 CallStackPanel::onFrameSelected 调用 setSelectedFrame 更新，
    // VariableInspector/WatchPanel 通过 getSelectedFrameLocals() 消费。
    // stop/reset/prepareRun 时通过 clearSelectedFrame() 重置为 -1。
    int selectedFrame_ = -1;

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

    // ---- R117: Watch 表达式 AST LRU 缓存 ----
    // 与条件断点 AST 缓存（setConditionEvaluator lambda 闭包内）对齐：
    // list + unordered_map 经典 LRU，命中 move_to_front，超上限 pop_back（最久未使用）。
    // 缓存 key 为自动补充分号后的表达式字符串，避免用户输入 i 与 i; 视为不同条目。
    using WatchAstCacheList = std::list<std::pair<std::string, std::shared_ptr<Block>>>;
    using WatchAstCacheLookup = std::unordered_map<std::string, WatchAstCacheList::iterator>;
    WatchAstCacheList watchAstCacheList_;
    WatchAstCacheLookup watchAstCacheLookup_;

    /// VM-IMPORT: 为 Compiler 设置模块加载器（对齐 WorkerManager 为 Interpreter 设置的 loader）
    /// 基于 filePath 的目录解析相对模块路径，自动添加 .mini 后缀
    void setupCompilerModuleLoader(const std::string& filePath);

    /// BUG-REPL-AUDIT-9 fix: 为 REPL 路径补设 Interpreter 模块加载器/mtimeChecker/currentFilePath。
    /// 若 Interpreter 已有 moduleLoader_（先 Run 过），不重复设置，避免覆盖 Run 时建立的
    /// baseDir（防止 Run 后 REPL 用不同的 baseDir 导致模块缓存基准不一致）。
    /// 路径遍历防护（拒绝 ".." 和绝对路径）由 Interpreter::visitImportStmt 前置检查保证。
    void setupReplModuleCallbacks();
};
