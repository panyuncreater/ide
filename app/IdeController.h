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

#include <QObject>
#include <QString>
#include <QSet>
#include <QMap>
#include <memory>
#include <string>

#include "interpreter/Interpreter.h"
#include "interpreter/Value.h"
#include "debug/DebugController.h"
#include "Diagnostic.h"
#include "PipelineRunner.h"
#include "WorkerManager.h"
#include "DebugCoordinator.h"
#include "VmStepper.h"

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
    Formatter& formatter() { return pipeline_.formatter(); }
    Lexer& lexer() { return pipeline_.lexer(); }
    Parser& parser() { return pipeline_.parser(); }
    Compiler& compiler() { return pipeline_.compiler(); }

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

    // ---- 调试接口（转发到 DebugCoordinator）----
    void setBreakpointCondition(int line, const std::string& condition) {
        debugCoord_.setBreakpointCondition(line, condition);
    }
    bool hasBreakpoints() const { return debugCoord_.hasBreakpoints(); }
    std::vector<VariableSnapshot> getDebugVariableSnapshot() const {
        return debugCoord_.getDebugVariableSnapshot();
    }
    std::vector<CallStackEntry> getDebugCallStack() const {
        return debugCoord_.getDebugCallStack();
    }

    // ---- 关键字接口（转发到 PipelineRunner.lexer）----
    const std::unordered_map<std::string, TokenType>& getKeywords() const { return pipeline_.lexer().keywords(); }

    // ---- REPL 接口（转发到 Interpreter）----
    /// 保留 REPL AST 引用（防止类/闭包 body 指针悬空）
    void retainReplAst(std::unique_ptr<Block> ast) {
        interpreter_->retainReplAst(std::move(ast));
    }
    /// 执行 REPL 程序，返回求值结果（异常向上传播由调用方处理）
    Value executeRepl(Block& program) { return interpreter_->executeRepl(program); }

    // ---- 管线操作（转发到 PipelineRunner）----
    bool runLexer(const std::string& source) { return pipeline_.runLexer(source); }
    bool runParser() { return pipeline_.runParser(); }
    /// 执行字节码编译，返回 true 表示无错误。
    /// 编译成功后同步更新 VmStepper 的编译结果，使 VM 步进可用。
    bool runCompiler();
    bool formatCode(std::string& formatted) { return pipeline_.formatCode(formatted); }
    PipelineResult runFrontendPipeline(const std::string& source) {
        return pipeline_.runFrontendPipeline(source);
    }

    // ---- Worker 线程管理（协调 PipelineRunner + WorkerManager + VmStepper）----
    /// 准备运行（词法+解析+创建 worker），返回 true 表示已就绪
    /// filePath: 当前文件路径（用于模块加载的相对路径解析），为空表示未保存文件
    bool prepareRun(bool isDebug, const std::string& source, const std::string& filePath = "");
    void startWorker() { workerMgr_.startWorker(); }
    bool stopForClose(int timeoutMs = 3000) { return workerMgr_.stopForClose(timeoutMs); }
    void forceStop() { workerMgr_.forceStop(); }
    bool isRunning() const { return workerMgr_.isRunning(); }
    bool isDebugRun() const { return workerMgr_.isDebugRun(); }

    // ---- 调试操作（转发到 DebugCoordinator）----
    void setupDebug(const QSet<int>& breakpoints,
                    const QMap<int, std::string>& conditions) {
        debugCoord_.setupDebug(breakpoints, conditions);
    }
    void setBreakpoints(const QSet<int>& breakpoints) { debugCoord_.setBreakpoints(breakpoints); }
    void stepIn()  { debugCoord_.stepIn(); }
    void stepOver() { debugCoord_.stepOver(); }
    void stepOut() { debugCoord_.stepOut(); }
    void resume()  { debugCoord_.resume(); }
    void stop()    { debugCoord_.stop(); }

    // ---- VM 操作（转发到 VmStepper）----
    VmStepResult vmStep() { return vmStepper_.step(); }
    VmStepResult vmStepByMode(VmStepMode mode) { return vmStepper_.stepByMode(mode); }
    void setVmBreakpoints(const QSet<int>& breakpoints) { vmStepper_.setBreakpoints(breakpoints); }
    void vmStop() { vmStepper_.stop(); }
    void vmReset() { vmStepper_.reset(); }
    bool isVmRunning() const { return vmStepper_.isRunning(); }
    bool isVmInitialized() const { return vmStepper_.isInitialized(); }

    // A1 fix: 启用/禁用 RegisterVM 后端（同步 Compiler 与 VmStepper）
    // 启用后 compile() 走 AST → IR → RegisterBytecode 路径，VmStepper 转发到 regVm_。
    // 切换时 VmStepper 自动 reset 防止状态污染；调用方应在 VM 未运行时切换。
    void setUseRegisterVM(bool enabled) {
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
};
