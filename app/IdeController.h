#pragma once

// ============================================================
// IdeController — 业务逻辑层
// ------------------------------------------------------------
// 从原 Ide 上帝对象拆分而来，负责：
//   - 编译管线（词法/语法/字节码）
//   - 解释器工作线程管理
//   - 调试器状态与回调
//   - VM 单步执行状态
//   - 代码格式化
//
// GUI 交互层（Ide）通过 Qt 信号/槽接收业务层通知。
// Worker 任务处理层（InterpreterWorker）已拆分至独立文件。
// ============================================================

#include <QObject>
#include <QString>
#include <QSet>
#include <QMap>
#include <QThread>
#include <memory>
#include <functional>
#include <string>

#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "interpreter/Interpreter.h"
#include "compiler/Compiler.h"
#include "compiler/VM.h"
#include "formatter/Formatter.h"
#include "debug/DebugController.h"
#include "Diagnostic.h"
#include "InterpreterWorker.h"

class IdeController : public QObject {
    Q_OBJECT

public:
    explicit IdeController(QObject* parent = nullptr);
    ~IdeController();

    // ---- 引擎访问 ----
    // B6 fix: 移除 interpreter()/debugger()/vm() 原始引用暴露，GUI 不直接接触引擎内部。
    // formatter/lexer/parser/compiler 仍供 IdeController 内部调用（GUI 不直接使用）。
    Formatter& formatter() { return formatter_; }
    Lexer& lexer() { return lexer_; }
    Parser& parser() { return parser_; }
    Compiler& compiler() { return compiler_; }

    // B6 fix: 语义化的 VM 调试状态快照接口（GUI 仅通过这些方法读取 VM 状态）
    // B6 bug fix: getVmStack 改返回 by value，避免返回 vm_.stack_ 引用导致调用方
    // 缓存引用后步进 VM 触发悬垂/use-after-free（与 getVmGlobals 保持一致的快照语义）
    std::vector<Value> getVmStack() const { return vm_.getStack(); }
    std::unordered_map<std::string, Value> getVmGlobals() const { return vm_.getGlobalsRef(); }
    size_t getVmCurrentIP() const { return vm_.getCurrentIP(); }
    OpCode getVmCurrentOpCode() const { return vm_.getCurrentOpCode(); }
    int getVmCurrentLine() const { return vm_.getCurrentLine(); }
    std::string getVmCurrentChunkName() const { return vm_.getCurrentChunkName(); }
    std::string getVmLastError() const { return vm_.getLastError(); }
    int getVmLastErrorLine() const { return vm_.getLastErrorLine(); }
    // A4 fix: 暴露 VM 调用栈深度（用于 step-over/out 判断）
    // B6 bug fix: 移除未使用的 getVmCallStack（GUI 无调用点，死 API）
    size_t getVmFrameCount() const { return vm_.getFrameCount(); }

    // B6 fix: 语义化的调试器接口（GUI 仅通过这些方法操作调试状态）
    void setBreakpointCondition(int line, const std::string& condition) {
        debugger_->setBreakpointCondition(line, condition);
    }
    bool hasBreakpoints() const { return !debugger_->getBreakpoints().isEmpty(); }
    std::vector<VariableSnapshot> getDebugVariableSnapshot() const {
        return debugger_->getVariableSnapshot();
    }
    std::vector<CallStackEntry> getDebugCallStack() const {
        return debugger_->getCallStack();
    }

    // B6 fix: 语义化的关键字接口（替代 GUI 直接访问 lexer().keywords()）
    const std::unordered_map<std::string, TokenType>& getKeywords() const { return lexer_.keywords(); }

    // B6 fix: 语义化的 REPL 执行接口（替代 ReplPanel 直接持有 Interpreter*）
    /// 保留 REPL AST 引用（防止类/闭包 body 指针悬空）
    void retainReplAst(std::unique_ptr<Block> ast) {
        interpreter_.retainReplAst(std::move(ast));
    }
    /// 执行 REPL 程序，返回求值结果（异常向上传播由调用方处理）
    Value executeRepl(Block& program) { return interpreter_.executeRepl(program); }

    // ---- 管线操作 ----
    /// 执行词法分析，返回 true 表示无错误
    bool runLexer(const std::string& source);
    /// 执行语法分析（使用上次 lastTokens_），返回 true 表示无错误
    bool runParser();
    /// 执行字节码编译，返回 true 表示无错误
    bool runCompiler();
    /// 格式化代码（需先 runLexer + runParser），成功时写入 formatted
    bool formatCode(std::string& formatted);

    /// C9 fix: 前端管线结果状态码
    enum class PipelineStatus {
        OK,            ///< Lexer+Parser 均成功
        LexerFailed,   ///< Lexer 异常或错误
        ParserFailed   ///< Parser 异常或错误
    };

    /// C9 fix: 前端管线结果
    struct PipelineResult {
        PipelineStatus status = PipelineStatus::OK;
        std::string errorMessage;  ///< 异常消息（status != OK 时有效）
        const DiagnosticBag* diagnostics = nullptr;  ///< 错误诊断包指针
    };

    /// C9 fix: 统一前端管线（Lexer + Parser），消除 onFormat/onShowBytecode/prepareRun 三处重复。
    /// 调用后 lastTokens_/astRoot_ 已就绪，caller 根据 status 决定后续动作。
    /// 错误诊断已 emit diagnosticsReady，caller 仅需更新 UI。
    PipelineResult runFrontendPipeline(const std::string& source);

    // ---- Worker 线程管理 ----
    /// 准备运行（词法+解析+创建 worker），返回 true 表示已就绪
    /// filePath: 当前文件路径（用于模块加载的相对路径解析），为空表示未保存文件
    bool prepareRun(bool isDebug, const std::string& source, const std::string& filePath = "");
    /// 启动 worker 线程执行
    void startWorker();
    /// 关闭前安全停止，返回 false 表示超时需强制终止
    bool stopForClose(int timeoutMs = 3000);
    /// 强制终止 worker 线程
    void forceStop();
    bool isRunning() const { return isRunning_; }
    bool isDebugRun() const { return isDebugRun_; }

    // ---- 调试操作 ----
    /// 设置断点及条件表达式
    void setupDebug(const QSet<int>& breakpoints,
                    const QMap<int, std::string>& conditions);
    void setBreakpoints(const QSet<int>& breakpoints) {
        debugger_->setBreakpoints(breakpoints);
    }
    void stepIn()  { debugger_->stepIn(); }
    void stepOver() { debugger_->stepOver(); }
    void stepOut() { debugger_->stepOut(); }
    void resume()  { debugger_->resume(); }
    void stop()    { debugger_->stop(); }

    // ---- VM 操作 ----
    enum class VmStepResult { OK, FINISHED, ERROR, NOT_READY, PAUSED_AT_BREAKPOINT };
    /// A4 fix: VM 步进模式（与 Interpreter DebugController::StepMode 对齐）
    enum class VmStepMode { STEP_IN, STEP_OVER, STEP_OUT, RUN };

    /// 单步执行 VM（等价于 stepIn），返回结果状态
    VmStepResult vmStep();

    /// A4 fix: 按指定模式执行 VM 步进
    /// - STEP_IN: 执行一条指令即返回
    /// - STEP_OVER: 执行直到帧深度 <= 起始深度且行号变化
    /// - STEP_OUT: 执行直到帧深度 < 起始深度
    /// - RUN: 全速执行直到命中断点/结束/错误
    /// 返回 PAUSED_AT_BREAKPOINT 表示命中断点需 UI 更新
    VmStepResult vmStepByMode(VmStepMode mode);

    /// A4 fix: 设置 VM 模式断点（复用 Interpreter 的 breakpoint 行号集合）
    /// 在 vmStepByMode(RUN) 时检查命中
    void setVmBreakpoints(const QSet<int>& breakpoints) { vmBreakpoints_ = breakpoints; }

    /// A4 fix: 停止 VM 并重置状态
    void vmStop();
    /// A4 fix: 重置 VM 状态（用于重新开始）
    void vmReset() { vm_.resetState(); isVmInitialized_ = false; vmStepMode_ = VmStepMode::STEP_IN; }
    bool isVmRunning() const { return isVmRunning_; }
    bool isVmInitialized() const { return isVmInitialized_; }

    // ---- 状态访问 ----
    const std::vector<Token>& lastTokens() const { return lastTokens_; }
    const CompileResult& lastCompileResult() const { return lastCompileResult_; }
    Block* astRoot() const { return astRoot_.get(); }
    const DiagnosticBag& lexerDiagnostics() const { return lexer_.getDiagnostics(); }
    const DiagnosticBag& parserDiagnostics() const { return parser_.getDiagnostics(); }
    const DiagnosticBag& compilerDiagnostics() const { return compiler_.getDiagnostics(); }

signals:
    void outputReady(const QString& text);
    void runOk();
    void stoppedByUser();
    void runtimeError(const QString& msg, int line, int column);
    void genericError(const QString& msg);
    void pausedAt(int line);
    void workerFinished(bool wasDebug);
    // B6 bug fix: 移除未使用的 vmStepInfo 信号（全代码库无 emit，连接为死代码）
    void diagnosticsReady(const DiagnosticBag& bag);

private:
    // ---- 引擎组件 ----
    Lexer lexer_;
    Parser parser_;
    Interpreter interpreter_;
    Compiler compiler_;
    VM vm_;
    Formatter formatter_;
    DebugController* debugger_ = nullptr;

    // ---- 管线状态 ----
    std::unique_ptr<Block> astRoot_;
    std::vector<Token> lastTokens_;
    CompileResult lastCompileResult_;

    // ---- Worker 状态 ----
    bool isRunning_ = false;
    bool isDebugRun_ = false;
    // 7.1 fix: 使用 unique_ptr 替代裸 new/delete，确保异常安全和资源释放
    // QThread 需自定义删除器：先 quit+wait 再 delete，避免 delete running QThread 的 UB
    struct QThreadDeleter {
        void operator()(QThread* thread) const;
    };
    std::unique_ptr<QThread, QThreadDeleter> workerThread_;
    std::unique_ptr<InterpreterWorker> worker_;

    // ---- VM 状态 ----
    bool isVmRunning_ = false;
    bool isVmInitialized_ = false;
    // A4 fix: VM 步进状态机（轻量版，不依赖 DebugController 的跨线程机制）
    VmStepMode vmStepMode_ = VmStepMode::STEP_IN;
    size_t vmStepStartFrameCount_ = 0;  // step-over/out 起始帧深度
    int vmLastPausedLine_ = 0;          // 上次暂停的行号（防同行重复触发）
    QSet<int> vmBreakpoints_;          // VM 模式断点行号集合（复用 Editor 断点）

    // ---- 内部方法 ----
    /// Worker 线程结束后的清理（删除 worker/thread、恢复回调、恢复 REPL 状态）
    void cleanupWorker();
    /// 设置 interpreter_ 的输出+输入回调（主线程 REPL 模式）
    void setupMainCallbacks();
    /// D20 fix: 构建跨线程安全的 input() 回调（带 30 秒超时保护，防止主线程无响应时 worker 永久阻塞）
    std::function<std::string(const std::string&)> buildInputCallback() const;
};
