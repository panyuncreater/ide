#include "IdeController.h"
#include "Logger.h"

// ============================================================
// IdeController — 业务逻辑层实现
// ============================================================

// 7.1 fix: QThread 自定义删除器 — 先 quit+wait 再 delete
void IdeController::QThreadDeleter::operator()(QThread* thread) const {
    if (thread) {
        if (thread->isRunning()) {
            thread->quit();
            thread->wait(3000);
        }
        delete thread;
    }
}

IdeController::IdeController(QObject* parent)
    : QObject(parent) {

    debugger_ = new DebugController(this);
    interpreter_.setDebugger(debugger_);

    // 主线程输出回调：emit outputReady 信号
    interpreter_.setOutputCallback([this](const std::string& text) {
        emit outputReady(QString::fromStdString(text));
    });

    vm_.setOutputCallback([this](const std::string& text) {
        emit outputReady(QString::fromStdString(text));
    });

    // 转发调试器暂停信号
    connect(debugger_, &DebugController::pausedAt, this, [this](int line) {
        emit pausedAt(line);
    });
}

IdeController::~IdeController() {
    // #10 fix: 确保工作线程已停止再删除，避免 delete running QThread 的 UB
    // 7.1 fix: 使用 unique_ptr 自动释放，显式 stop 逻辑保留
    if (workerThread_ && workerThread_->isRunning()) {
        debugger_->stop();
        workerThread_->quit();
        if (!workerThread_->wait(3000)) {
            workerThread_->terminate();
            workerThread_->wait();
        }
    }
    // worker_ 先于 workerThread_ 释放（成员声明顺序保证）
    worker_.reset();
    workerThread_.reset();
}

// ============================================================
// 管线操作
// ============================================================

bool IdeController::runLexer(const std::string& source) {
    lastTokens_ = lexer_.scan(source);
    emit diagnosticsReady(lexer_.getDiagnostics());
    return !lexer_.getDiagnostics().hasErrors();
}

bool IdeController::runParser() {
    astRoot_ = parser_.parse(lastTokens_);
    emit diagnosticsReady(parser_.getDiagnostics());
    return astRoot_ != nullptr && !parser_.hasErrors();
}

bool IdeController::runCompiler() {
    if (!astRoot_) return false;
    lastCompileResult_ = compiler_.compile(*astRoot_);
    emit diagnosticsReady(compiler_.getDiagnostics());
    return !compiler_.getDiagnostics().hasErrors();
}

bool IdeController::formatCode(std::string& formatted) {
    if (!astRoot_) return false;
    formatter_.setComments(lexer_.comments());
    formatted = formatter_.format(*astRoot_);
    return true;
}

// ============================================================
// Worker 线程管理
// ============================================================

bool IdeController::prepareRun(bool isDebug, const std::string& source) {
    if (isRunning_) return false;

    Logger::Info(isDebug ? "启动调试运行" : "启动程序运行", "IDE");

    // 词法分析
    try {
        if (!runLexer(source)) return false;
    } catch (const std::exception& e) {
        emit genericError(QString("词法分析异常: %1").arg(e.what()));
        return false;
    }

    // 语法分析
    try {
        if (!runParser()) return false;
    } catch (const std::exception& e) {
        emit genericError(QString("解析异常: %1").arg(e.what()));
        return false;
    }

    if (!astRoot_) return false;

    // GUI-01 fix: 启用 debugMode 使 checkBreak 生效
    interpreter_.setDebugMode(true);
    isDebugRun_ = isDebug;
    isRunning_ = true;

    // R2/D1 fix: 保存 REPL 状态
    interpreter_.saveReplState();

    // 7.1 fix: 使用 unique_ptr 管理生命周期，清理上次运行的 worker
    worker_.reset();
    workerThread_.reset();
    worker_ = std::make_unique<InterpreterWorker>(interpreter_, *astRoot_, debugger_);
    workerThread_.reset(new QThread(this));
    worker_->moveToThread(workerThread_.get());

    // 转发 worker 信号到 IdeController 信号
    connect(worker_.get(), &InterpreterWorker::outputReady, this, &IdeController::outputReady);
    connect(worker_.get(), &InterpreterWorker::finishedOk, this, &IdeController::runOk);
    connect(worker_.get(), &InterpreterWorker::stoppedByUser, this, &IdeController::stoppedByUser);
    connect(worker_.get(), &InterpreterWorker::runtimeError, this, &IdeController::runtimeError);
    connect(worker_.get(), &InterpreterWorker::genericError, this, &IdeController::genericError);

    // 任一终止信号 → 退出线程事件循环
    connect(worker_.get(), &InterpreterWorker::finishedOk, workerThread_.get(), &QThread::quit);
    connect(worker_.get(), &InterpreterWorker::stoppedByUser, workerThread_.get(), &QThread::quit);
    connect(worker_.get(), &InterpreterWorker::runtimeError, workerThread_.get(), &QThread::quit);
    connect(worker_.get(), &InterpreterWorker::genericError, workerThread_.get(), &QThread::quit);

    // 线程结束 → 清理 → 通知 UI
    connect(workerThread_.get(), &QThread::finished, this, [this]() {
        cleanupWorker();
        emit workerFinished(isDebugRun_);
    });

    return true;
}

void IdeController::startWorker() {
    workerThread_->start();
    QMetaObject::invokeMethod(worker_.get(), "run", Qt::QueuedConnection);
}

void IdeController::stopWorker() {
    debugger_->stop();
    // 状态清理由 workerFinished 信号触发
}

bool IdeController::stopForClose(int timeoutMs) {
    debugger_->stop();
    if (workerThread_) {
        workerThread_->quit();
        if (!workerThread_->wait(timeoutMs)) {
            return false;  // 超时，需强制终止
        }
        // 7.1 fix: unique_ptr 自动释放，无需手动 delete
        worker_.reset();
        workerThread_.reset();
    }
    isRunning_ = false;
    return true;
}

void IdeController::forceStop() {
    if (workerThread_) {
        workerThread_->terminate();
        workerThread_->wait();
        // 7.1 fix: unique_ptr 自动释放，无需手动 delete
        worker_.reset();
        workerThread_.reset();
    }
    isRunning_ = false;
}

void IdeController::cleanupWorker() {
    isRunning_ = false;
    isDebugRun_ = false;
    interpreter_.setDebugMode(false);
    interpreter_.restoreReplState();
    debugger_->reset();

    // 7.1 fix: unique_ptr 自动释放，无需手动 delete
    worker_.reset();

    if (workerThread_) {
        workerThread_->quit();
        workerThread_->wait();
        workerThread_.reset();
    }

    // 恢复主线程输出回调（worker 的回调 lambda 捕获了已删除的 worker this 指针）
    interpreter_.setOutputCallback([this](const std::string& text) {
        emit outputReady(QString::fromStdString(text));
    });
}

// ============================================================
// 调试操作
// ============================================================

void IdeController::setupDebug(const QSet<int>& breakpoints,
                               const QMap<int, std::string>& conditions) {
    debugger_->setBreakpoints(breakpoints);

    // 同步断点条件
    for (int line : breakpoints) {
        auto it = conditions.find(line);
        if (it != conditions.end() && !it.value().empty()) {
            debugger_->setBreakpointCondition(line, it.value());
        }
    }

    // GUI-03 fix: 使用 evaluateCondition 安全求值条件断点
    debugger_->setConditionEvaluator([this](const std::string& condition) -> bool {
        try {
            Lexer condLexer;
            auto tokens = condLexer.scan(condition);
            Parser condParser;
            auto block = condParser.parse(tokens);
            if (!condParser.getDiagnostics().hasErrors() && block && !block->statements.empty()) {
                Value result = interpreter_.evaluateCondition(block->statements[0].get());
                return result.isTruthy();
            }
        } catch (...) {
            // 条件求值失败视为 false（不暂停）
        }
        return false;
    });

    // 调试变量回调
    debugger_->setVariableCallback([this]() -> std::vector<VariableSnapshot> {
        std::vector<VariableSnapshot> result;
        Environment* env = interpreter_.currentEnvironment();
        if (env) {
            int depth = 0;
            Environment* current = env;
            while (current) {
                const auto& locals = current->localVariables();
                for (const auto& kv : locals) {
                    VariableSnapshot snap;
                    snap.name = kv.first;
                    snap.value = kv.second;
                    snap.scope = (depth == 0) ? "局部" : (current->parent ? "外层" : "全局");
                    result.push_back(snap);
                }
                current = current->parent.get();
                depth++;
            }
        }
        return result;
    });

    debugger_->setCallStackCallback([this]() -> std::vector<CallStackEntry> {
        std::vector<CallStackEntry> result;
        const auto& stack = interpreter_.getCallStack();
        for (const auto& frame : stack) {
            CallStackEntry entry;
            entry.functionName = frame.functionName;
            entry.line = frame.line;
            entry.depth = frame.depth;
            if (frame.env) {
                for (const auto& kv : frame.env->localVariables()) {
                    entry.locals.emplace_back(kv.first, kv.second);
                }
            }
            result.push_back(entry);
        }
        return result;
    });

    debugger_->reset();
    debugger_->stepIn();
}

// ============================================================
// VM 操作
// ============================================================

IdeController::VmStepResult IdeController::vmStep() {
    if (isVmRunning_) return VmStepResult::NOT_READY;
    if (lastCompileResult_.mainChunk.code.empty()) return VmStepResult::NOT_READY;

    // 首次点击：初始化 VM 执行环境
    if (!isVmInitialized_) {
        vm_.initExecution(lastCompileResult_);
        isVmInitialized_ = true;
    }

    isVmRunning_ = true;

    // 单步执行时启用回调
    vm_.setStepCallbackEnabled(true);
    vm_.setStepCallback([this](const VMStepInfo& info) {
        emit vmStepInfo(info);
    });

    VMResult result = vm_.stepOnce();

    if (result == VMResult::VM_RUNTIME_ERROR) {
        isVmInitialized_ = false;
        isVmRunning_ = false;
        return VmStepResult::ERROR;
    }

    if (vm_.isFinished()) {
        isVmInitialized_ = false;
        isVmRunning_ = false;
        return VmStepResult::FINISHED;
    }

    isVmRunning_ = false;
    return VmStepResult::OK;
}

void IdeController::vmStop() {
    vm_.resetState();
    isVmInitialized_ = false;
    isVmRunning_ = false;
}

// ============================================================
// 输出回调设置
// ============================================================

void IdeController::setupCallbacks() {
    interpreter_.setOutputCallback([this](const std::string& text) {
        emit outputReady(QString::fromStdString(text));
    });
    vm_.setOutputCallback([this](const std::string& text) {
        emit outputReady(QString::fromStdString(text));
    });
}
