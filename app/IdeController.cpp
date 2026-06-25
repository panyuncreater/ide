#include "IdeController.h"
#include "Logger.h"
#include <QInputDialog>
#include <QLineEdit>
#include <QThread>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <future>
#include <chrono>

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

// D20 fix: 构建跨线程安全的 input() 回调。
// 主线程直接弹对话框；工作线程通过 Qt::QueuedConnection 投递到主线程，
// 用 std::future + wait_for 限时 30 秒，超时返回空串避免 worker 永久阻塞。
std::function<std::string(const std::string&)> IdeController::buildInputCallback() const {
    return [this](const std::string& prompt) -> std::string {
        if (QThread::currentThread() == this->thread()) {
            bool ok = false;
            QString text = QInputDialog::getText(nullptr, "input",
                QString::fromStdString(prompt), QLineEdit::Normal, "", &ok);
            return ok ? text.toStdString() : "";
        }
        // 跨线程：投递到主线程并限时等待
        std::promise<QString> promise;
        std::shared_future<QString> future = promise.get_future().share();
        QMetaObject::invokeMethod(const_cast<IdeController*>(this),
            [this, prompt, &promise]() {
                bool ok = false;
                QString result = QInputDialog::getText(nullptr, "input",
                    QString::fromStdString(prompt), QLineEdit::Normal, "", &ok);
                if (!ok) result = "";
                promise.set_value(result);
            }, Qt::QueuedConnection);
        if (future.wait_for(std::chrono::seconds(30)) == std::future_status::ready) {
            return future.get().toStdString();
        }
        Logger::Warn("input() 超时（主线程 30 秒未响应），返回空串", "IDE");
        return std::string();
    };
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

    // D20 fix: input() 回调统一通过 buildInputCallback 构建（带超时保护）
    auto inputHandler = buildInputCallback();
    interpreter_.setInputCallback(inputHandler);
    vm_.setInputCallback(inputHandler);

    // 转发调试器暂停信号
    connect(debugger_, &DebugController::pausedAt, this, [this](int line) {
        emit pausedAt(line);
    });
}

void IdeController::setupMainCallbacks() {
    // 恢复主线程输出回调
    interpreter_.setOutputCallback([this](const std::string& text) {
        emit outputReady(QString::fromStdString(text));
    });
    // D20 fix: 恢复输入回调统一通过 buildInputCallback 构建（带超时保护）
    interpreter_.setInputCallback(buildInputCallback());
}

IdeController::~IdeController() {
    // #10 fix: 确保工作线程已停止再删除，避免 delete running QThread 的 UB
    // 7.1 fix: 使用 unique_ptr 自动释放，显式 stop 逻辑保留
    // A5 fix: 优先协作式取消，延长等待至 5 秒。terminate() 仅作为析构时的最后手段
    // （进程退出阶段，资源泄漏可接受，但避免 UI 永久卡死）。
    if (workerThread_ && workerThread_->isRunning()) {
        debugger_->stop();
        workerThread_->quit();
        if (!workerThread_->wait(5000)) {
            Logger::Error("析构时 Worker 未在 5 秒内停止，回退到 terminate()（进程退出阶段）", "IDE");
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

// C9 fix: 统一前端管线实现
IdeController::PipelineResult IdeController::runFrontendPipeline(const std::string& source) {
    PipelineResult result;

    // 词法分析
    try {
        if (!runLexer(source)) {
            result.status = PipelineStatus::LexerFailed;
            result.diagnostics = &lexer_.getDiagnostics();
            return result;
        }
    } catch (const std::exception& e) {
        result.status = PipelineStatus::LexerFailed;
        result.errorMessage = e.what();
        return result;
    }

    // 语法分析
    try {
        if (!runParser()) {
            result.status = PipelineStatus::ParserFailed;
            result.diagnostics = &parser_.getDiagnostics();
            return result;
        }
    } catch (const std::exception& e) {
        result.status = PipelineStatus::ParserFailed;
        result.errorMessage = e.what();
        return result;
    }

    return result;  // OK
}

// ============================================================
// Worker 线程管理
// ============================================================

bool IdeController::prepareRun(bool isDebug, const std::string& source, const std::string& filePath) {
    if (isRunning_) return false;

    Logger::Info(isDebug ? "启动调试运行" : "启动程序运行", "IDE");

    // P0-1 fix: 设置模块加载器，使 F12 模块系统在 IDE 中可用
    // 模块路径解析：相对于当前文件所在目录查找 <modulePath>.mini 文件
    QString baseDir;
    if (!filePath.empty()) {
        QFileInfo fi(QString::fromStdString(filePath));
        baseDir = fi.absolutePath();
    }
    interpreter_.setCurrentFilePath(filePath);
    interpreter_.setModuleLoader([baseDir](const std::string& modulePath) -> std::string {
        // 尝试解析模块路径：优先作为相对路径，其次在 baseDir 下查找
        QString qPath = QString::fromStdString(modulePath);
        // 如果没有 .mini 后缀，自动添加
        if (!qPath.endsWith(".mini", Qt::CaseInsensitive)) {
            qPath += ".mini";
        }
        QStringList candidates;
        if (baseDir.isEmpty()) {
            candidates << qPath;
        } else {
            candidates << QDir(baseDir).filePath(qPath) << qPath;
        }
        for (const QString& candidate : candidates) {
            QFile file(candidate);
            if (file.exists() && file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                return QString::fromUtf8(file.readAll()).toStdString();
            }
        }
        return "";
    });

    // C9 fix: 使用统一前端管线
    auto pipelineResult = runFrontendPipeline(source);
    if (pipelineResult.status != PipelineStatus::OK) {
        if (!pipelineResult.errorMessage.empty()) {
            emit genericError(QString("%1: %2")
                .arg(pipelineResult.status == PipelineStatus::LexerFailed ? "词法分析异常" : "解析异常")
                .arg(QString::fromStdString(pipelineResult.errorMessage)));
        }
        return false;
    }

    if (!astRoot_) return false;

    // GUI-01 fix: 启用 debugMode 使 checkBreak 生效
    interpreter_.setDebugMode(true);
    isDebugRun_ = isDebug;
    isRunning_ = true;

    // A-P1-1 fix: isRunning_=true 之后的代码若抛出异常，需回滚 isRunning_ 状态，
    // 否则 UI 永久卡在"运行中"
    try {
    // A-P1-1 fix: 非调试运行时清除残留断点，避免普通运行在调试会话后意外暂停
    // （DebugController::reset() 保留断点供下次调试复用，普通运行需显式清除）
    if (!isDebug) {
        debugger_->setBreakpoints(QSet<int>());
    }

    // R2/D1 fix: 保存 REPL 状态
    interpreter_.saveReplState();

    // 7.1 fix: 使用 unique_ptr 管理生命周期，清理上次运行的 worker
    worker_.reset();
    workerThread_.reset();
    worker_ = std::make_unique<InterpreterWorker>(interpreter_, *astRoot_);
    // A-P2-5 fix: 不设 parent，由 unique_ptr 独占管理生命周期，避免双重所有权
    workerThread_.reset(new QThread());
    // C16 fix: 增大 worker 线程栈至 4MB。每次 MiniLang 调用展开 6-10 个 C++ 栈帧，
    // MAX_RECURSION_DEPTH=256 对应约 1500-2500 个 C++ 栈帧，接近 Windows 默认 1MB 栈边界。
    workerThread_->setStackSize(4 * 1024 * 1024);
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
        // P2 fix: 在 cleanupWorker 重置 isDebugRun_ 之前保存其值
        bool wasDebug = isDebugRun_;
        cleanupWorker();
        emit workerFinished(wasDebug);
    });
    } catch (...) {
        isRunning_ = false;
        isDebugRun_ = false;
        interpreter_.setDebugMode(false);
        interpreter_.restoreReplState();
        debugger_->reset();
        worker_.reset();
        workerThread_.reset();
        setupMainCallbacks();
        throw;
    }

    return true;
}

void IdeController::startWorker() {
    workerThread_->start();
    QMetaObject::invokeMethod(worker_.get(), "run", Qt::QueuedConnection);
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
        // A5 fix: 协作式取消 — 通过 debugger_->stop() 设置 stopped_ 标志，
        // 解释器/VM 在 checkBreak() 检测到后抛出 DebugStopException 正常退出。
        // 不再使用 QThread::terminate()（会导致 UB：互斥锁未释放、堆损坏、悬垂信号）。
        debugger_->stop();
        if (workerThread_->wait(5000)) {
            // Worker 已正常停止，安全释放
            worker_.reset();
            workerThread_.reset();
        } else {
            // A5 fix: Worker 未在 5 秒内停止（仅在解释器/VM 存在未检查 stopped_ 的
            // 死循环时发生，属于 bug）。不调用 terminate()，保留 worker/thread 对象
            // 避免删除运行中对象的 UB。UI 状态已在下方恢复，用户可继续操作。
            // Worker 最终会因 MAX_RECURSION_DEPTH/迭代上限退出，届时由 workerFinished 清理。
            Logger::Error("Worker 未在 5 秒内响应取消请求，保留运行中线程（未调用 terminate 避免 UB）", "IDE");
        }
    }
    // A-P1-2 fix: 补全状态清理（与 cleanupWorker 一致），避免下次运行因 stopped_=true 立即终止
    isRunning_ = false;
    isDebugRun_ = false;
    interpreter_.setDebugMode(false);
    interpreter_.restoreReplState();
    debugger_->reset();
    setupMainCallbacks();
}

void IdeController::cleanupWorker() {
    isRunning_ = false;
    isDebugRun_ = false;
    interpreter_.setDebugMode(false);
    interpreter_.restoreReplState();
    debugger_->reset();

    // A-P2-1 fix: 先确保线程完全退出，再删除 worker（符合 Qt 线程亲和性规则）
    if (workerThread_) {
        workerThread_->quit();
        workerThread_->wait();
        workerThread_.reset();
    }
    worker_.reset();

    // 恢复主线程输出+输入回调（worker 的回调 lambda 捕获了已删除的 worker this 指针）
    setupMainCallbacks();
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

    // A-P1-4 fix: 异常安全保护，确保 isVmRunning_/isVmInitialized_ 在异常时回滚
    try {
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
    } catch (...) {
        // 异常时重置所有状态，避免永久卡死
        isVmInitialized_ = false;
        isVmRunning_ = false;
        throw;
    }
}

void IdeController::vmStop() {
    vm_.resetState();
    isVmInitialized_ = false;
    isVmRunning_ = false;
}
