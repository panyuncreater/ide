#pragma once

// ============================================================
// InterpreterWorker — Worker 任务处理层
// ------------------------------------------------------------
// 从原 ide.h 拆分而来。负责在独立线程中执行解释器，
// 通过 Qt 信号将输出/结果/错误跨线程传递回主线程。
// ============================================================

#include <QObject>
#include <QString>

#include "interpreter/Interpreter.h"
#include "ast/ASTNode.h"

class InterpreterWorker : public QObject {
    Q_OBJECT
public:
    // A-P2-9 fix: 移除未使用的 debugger_ 参数（debugger 已通过 interpreter_.setDebugger 设置）
    InterpreterWorker(Interpreter& interp, Block& ast)
        : interp_(interp), ast_(ast) {}

public slots:
    void run();

signals:
    void outputReady(const QString& text);
    void finishedOk();
    void stoppedByUser();
    void runtimeError(const QString& msg, int line, int column);
    void genericError(const QString& msg);

private:
    Interpreter& interp_;
    Block& ast_;
};
