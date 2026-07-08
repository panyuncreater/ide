#pragma once

// ============================================================
// InterpreterWorker — Worker 任务处理层
// ------------------------------------------------------------
// 从原 ide.h 拆分而来。负责在独立线程中执行解释器，
// 通过 Qt 信号将输出/结果/错误跨线程传递回主线程。
// ============================================================

#include <QObject>
#include <QString>
#include <memory>

#include "ast/ASTNode.h"
#include "interpreter/Interpreter.h"

class InterpreterWorker : public QObject {
    Q_OBJECT
public:
    // MEM-01 fix: 改用 shared_ptr 持有 Interpreter，使 worker 线程共享所有权，
    // IdeController 析构后 Interpreter 仍存活直到 worker 释放引用，避免悬垂引用。
    // QT-R-03 fix: 改用 shared_ptr 持有 AST，避免主线程重新 parse 导致 worker 持有悬垂引用。
    InterpreterWorker(std::shared_ptr<Interpreter> interp, std::shared_ptr<Block> ast)
        : interp_(std::move(interp)), ast_(std::move(ast)) {}

    // AUDIT-P2-CORRECT fix: 析构兜底清空 interp_ 回调，防止未来回归——
    // 若 worker 异常终止未走 run() 末尾的清空路径，interp_ 仍持有捕获 this 的悬垂 lambda。
    ~InterpreterWorker() {
        if (interp_) {
            interp_->setOutputCallback([](const std::string&) {});
            interp_->setInputCallback([](const std::string&) { return std::string(); });
        }
    }

public slots:
    void run();

signals:
    void outputReady(const QString& text);
    void finishedOk();
    void stoppedByUser();
    void runtimeError(const QString& msg, int line, int column);
    void genericError(const QString& msg);

private:
    std::shared_ptr<Interpreter> interp_;
    std::shared_ptr<Block> ast_;
};
