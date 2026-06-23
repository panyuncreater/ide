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
#include "debug/DebugController.h"

class InterpreterWorker : public QObject {
    Q_OBJECT
public:
    InterpreterWorker(Interpreter& interp, Block& ast, DebugController* dbg)
        : interp_(interp), ast_(ast), debugger_(dbg) {}

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
    DebugController* debugger_;
};
