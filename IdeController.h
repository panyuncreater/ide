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
    Interpreter& interpreter() { return interpreter_; }
    DebugController* debugger() { return debugger_; }
    VM& vm() { return vm_; }
    Formatter& formatter() { return formatter_; }
    Lexer& lexer() { return lexer_; }
    Parser& parser() { return parser_; }
    Compiler& compiler() { return compiler_; }

    // ---- 管线操作 ----
    /// 执行词法分析，返回 true 表示无错误
    bool runLexer(const std::string& source);
    /// 执行语法分析（使用上次 lastTokens_），返回 true 表示无错误
    bool runParser();
    /// 执行字节码编译，返回 true 表示无错误
    bool runCompiler();
    /// 格式化代码（需先 runLexer + runParser），成功时写入 formatted
    bool formatCode(std::string& formatted);

    // ---- Worker 线程管理 ----
    /// 准备运行（词法+解析+创建 worker），返回 true 表示已就绪
    bool prepareRun(bool isDebug, const std::string& source);
    /// 启动 worker 线程执行
    void startWorker();
    /// 停止 worker（通过调试器 stop）
    void stopWorker();
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
    enum class VmStepResult { OK, FINISHED, ERROR, NOT_READY };
    /// 单步执行 VM，返回结果状态
    VmStepResult vmStep();
    /// 停止 VM 并重置状态
    void vmStop();
    /// 重置 VM 状态（用于重新开始）
    void vmReset() { vm_.resetState(); isVmInitialized_ = false; }
    bool isVmRunning() const { return isVmRunning_; }
    bool isVmInitialized() const { return isVmInitialized_; }

    // ---- 状态访问 ----
    const std::vector<Token>& lastTokens() const { return lastTokens_; }
    const CompileResult& lastCompileResult() const { return lastCompileResult_; }
    Block* astRoot() const { return astRoot_.get(); }
    const DiagnosticBag& lexerDiagnostics() const { return lexer_.getDiagnostics(); }
    const DiagnosticBag& parserDiagnostics() const { return parser_.getDiagnostics(); }
    const DiagnosticBag& compilerDiagnostics() const { return compiler_.getDiagnostics(); }

    /// 设置主线程输出回调（emit outputReady 信号）
    void setupCallbacks();

signals:
    void outputReady(const QString& text);
    void runOk();
    void stoppedByUser();
    void runtimeError(const QString& msg, int line, int column);
    void genericError(const QString& msg);
    void pausedAt(int line);
    void workerFinished(bool wasDebug);
    void vmStepInfo(const VMStepInfo& info);
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

    // ---- 内部方法 ----
    /// Worker 线程结束后的清理（删除 worker/thread、恢复回调、恢复 REPL 状态）
    void cleanupWorker();
};
