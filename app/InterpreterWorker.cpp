#include "InterpreterWorker.h"
#include <typeinfo>

// ============================================================
// InterpreterWorker 实现（OP-1 fix）
// ============================================================

void InterpreterWorker::run() {
    interp_.setOutputCallback([this](const std::string& text) {
        emit outputReady(QString::fromStdString(text));
    });

    try {
        interp_.execute(ast_);
        emit finishedOk();
    } catch (const RuntimeError& e) {
        emit runtimeError(QString::fromStdString(e.what()), e.line, e.column);
    } catch (const DebugStopException&) {
        emit stoppedByUser();
    } catch (const std::runtime_error& e) {
        emit genericError(QString::fromStdString(e.what()));
    } catch (const std::exception& e) {
        // S-07 fix: 保留异常类型信息，便于诊断
        emit genericError(QString("未预期的错误 [%1]: %2")
                          .arg(QString::fromStdString(typeid(e).name()))
                          .arg(QString::fromStdString(e.what())));
    } catch (...) {
        // S-07 fix: catch(...) 兜底，无法获取类型信息但明确标注
        emit genericError("未预期的未知异常（无法获取类型信息）");
    }

    // A-P2-2 fix: 返回前恢复空回调，避免 interp_ 持有指向已销毁 worker 的悬垂 lambda
    // （cleanupWorker 会在主线程恢复主线程回调，此处仅清除 worker 侧的捕获 this 的回调）
    interp_.setOutputCallback([](const std::string&) {});
    interp_.setInputCallback([](const std::string&) { return std::string(); });
}
