#include "WorkerManager.h"
#include "Logger.h"
#include <QInputDialog>
#include <QLineEdit>
#include <QPointer>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <future>
#include <chrono>
#include <cstdlib>  // std::_Exit — terminate worker 后跳过析构退出进程

// ============================================================
// WorkerManager — Worker 线程管理实现（ARCH-11 拆分自 IdeController）
// ============================================================

// 7.1 fix: QThread 自定义删除器 — 先 quit+wait 再 delete
// QT-R-04 fix: wait 策略对齐 ~WorkerManager 的 5s+terminate，避免 delete running QThread 的 UB。
void WorkerManager::QThreadDeleter::operator()(QThread* thread) const {
    if (thread) {
        if (thread->isRunning()) {
            thread->quit();
            if (!thread->wait(5000)) {
                LOG_ERROR("QThreadDeleter: Worker 未在 5 秒内退出，回退 terminate()（避免 delete running QThread UB）", "IDE");
                thread->terminate();
                thread->wait();
            }
        }
        delete thread;
    }
}

// D20 fix: 构建跨线程安全的 input() 回调。
// 主线程直接弹对话框；工作线程通过 Qt::QueuedConnection 投递到主线程，
// 用 std::future + wait_for 限时 30 秒，超时返回空串避免 worker 永久阻塞。
// A6 bug fix: promise 必须用 shared_ptr 按值捕获，否则超时后主线程 lambda
// 仍持栈上 promise 的悬垂引用，调用 set_value 触发 use-after-free。
// MEM-02 fix: lambda 不再裸捕获 this，改用 QPointer<WorkerManager>。
// 当 WorkerManager 析构后 QPointer 自动置 null，lambda 检测后安全返回空串，
// 避免 worker 线程通过悬垂 this 调用 thread()/invokeMethod 的 use-after-free。
std::function<std::string(const std::string&)> WorkerManager::buildInputCallback() {
    QPointer<WorkerManager> self = this;
    return [self](const std::string& prompt) -> std::string {
        // MEM-02 fix: controller 已析构时直接返回，不访问悬垂指针
        if (!self) return std::string();
        if (QThread::currentThread() == self->thread()) {
            bool ok = false;
            QString text = QInputDialog::getText(nullptr, "input",
                QString::fromStdString(prompt), QLineEdit::Normal, "", &ok);
            return ok ? text.toStdString() : "";
        }
        // 跨线程：投递到主线程并限时等待。promise 用 shared_ptr 持有，
        // worker 超时返回后 lambda 仍可安全调用 set_value（满足一个无人等待的 shared state）。
        auto promisePtr = std::make_shared<std::promise<QString>>();
        std::shared_future<QString> future = promisePtr->get_future().share();
        QMetaObject::invokeMethod(self.data(),
            [self, prompt, promisePtr]() {
                // MEM-02 fix: 主线程执行时 controller 可能已析构，检测后解锁 worker
                if (!self) {
                    promisePtr->set_value(QString());
                    return;
                }
                bool ok = false;
                QString result = QInputDialog::getText(nullptr, "input",
                    QString::fromStdString(prompt), QLineEdit::Normal, "", &ok);
                if (!ok) result = "";
                promisePtr->set_value(result);
            }, Qt::QueuedConnection);
        if (future.wait_for(std::chrono::seconds(30)) == std::future_status::ready) {
            return future.get().toStdString();
        }
        // E3 fix: 超时不再静默返回空串（会导致 input() 调用方拿到看似正常的结果继续执行），
        // 改为抛异常，由 executeSharedInput 捕获并附上调用点行号/列号上抛 RuntimeError。
        // promise 仍由 shared_ptr 持有，主线程稍后 set_value 不会 use-after-free。
        LOG_WARNING("input() 超时（主线程 30 秒未响应），抛出 RuntimeError", "IDE");
        throw std::runtime_error("input() 超时（主线程 30 秒未响应）");
    };
}

WorkerManager::WorkerManager(std::shared_ptr<Interpreter> interpreter,
                             std::shared_ptr<DebugController> debugger,
                             QObject* parent)
    : QObject(parent)
    , interpreter_(std::move(interpreter))
    , debugger_(std::move(debugger)) {
}

WorkerManager::~WorkerManager() {
    // #10 fix: 确保工作线程已停止再删除，避免 delete running QThread 的 UB
    // 7.1 fix: 使用 unique_ptr 自动释放，显式 stop 逻辑保留
    // A5 fix: 优先协作式取消，延长等待至 5 秒。terminate() 仅作为析构时的最后手段。
    if (workerThread_ && workerThread_->isRunning()) {
        debugger_->stop();
        workerThread_->quit();
        if (!workerThread_->wait(5000)) {
            LOG_ERROR("析构时 Worker 未在 5 秒内停止，回退到 terminate()（进程退出阶段）", "IDE");
            workerThread_->terminate();
            workerThread_->wait();
        }
    }
    // worker_ 先于 workerThread_ 释放（成员声明顺序保证）
    // AUDIT-P1 fix: 与 stopForClose 行 287 的 BUG-IDE-15 fix 对齐。析构路径 wait(5000)
    // 成功后 finished 信号已发射并投递 QueuedConnection lambda 到主线程队列，
    // workerThread_.reset() 销毁 QThread 对象但已投递的 lambda 仍持 this 指针。
    // 后续 Ide::closeEvent 的 processEvents 可能触发悬垂 lambda → UAF。disconnect 确保安全。
    if (workerThread_) workerThread_->disconnect(this);
    worker_.reset();
    workerThread_.reset();
}

void WorkerManager::setupMainCallbacks() {
    // 恢复主线程输出回调
    interpreter_->setOutputCallback([this](const std::string& text) {
        emit outputReady(QString::fromStdString(text));
    });
    // D20 fix: 恢复输入回调统一通过 buildInputCallback 构建（带超时保护）
    interpreter_->setInputCallback(buildInputCallback());
}

// ============================================================
// Worker 线程管理
// ============================================================

bool WorkerManager::prepareRun(bool isDebug, std::shared_ptr<Block> astRoot, const std::string& filePath) {
    if (isRunning_) return false;

    LOG_INFO(isDebug ? "启动调试运行" : "启动程序运行", "IDE");

    // P0-1 fix: 设置模块加载器，使 F12 模块系统在 IDE 中可用
    // 模块路径解析：相对于当前文件所在目录查找 <modulePath>.mini 文件
    QString baseDir;
    if (!filePath.empty()) {
        QFileInfo fi(QString::fromStdString(filePath));
        baseDir = fi.absolutePath();
    }
    interpreter_->setCurrentFilePath(filePath);
    // BUG-REPL-AUDIT-1 fix: 模块路径解析辅助函数，供 loader 和 mtime checker 共用
    auto resolveModulePath = [baseDir](const std::string& modulePath) -> QString {
        QString qPath = QString::fromStdString(modulePath);
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
            if (QFile::exists(candidate)) {
                return candidate;
            }
        }
        return {};
    };
    interpreter_->setModuleLoader([resolveModulePath](const std::string& modulePath) -> std::string {
        QString resolved = resolveModulePath(modulePath);
        if (resolved.isEmpty()) return "";
        QFile file(resolved);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return QString::fromUtf8(file.readAll()).toStdString();
        }
        return "";
    });
    // BUG-REPL-AUDIT-1 fix: 设置模块 mtime 检查器，REPL 模式下文件修改后自动失效缓存
    interpreter_->setModuleMtimeChecker([resolveModulePath](const std::string& modulePath) -> int64_t {
        QString resolved = resolveModulePath(modulePath);
        if (resolved.isEmpty()) return 0;
        QFileInfo fi(resolved);
        if (!fi.exists()) return 0;
        // 返回文件最后修改时间的毫秒时间戳
        return fi.lastModified().toMSecsSinceEpoch();
    });

    if (!astRoot) return false;

    // BUG-DBG-11 fix: 状态设置移入 try 块内，确保任何抛出都能被 catch 回滚。
    // 原实现将 setDebugMode/isDebugRun_/isRunning_ 设在 try 块外，若 try 块内
    // 第一条语句（setBreakpoints）抛出，catch 能回滚；但若未来在 try 块外
    // 插入其他代码抛出，状态将永久泄漏（UI 卡在"运行中"）。
    // A-P1-1 fix: isRunning_=true 之后的代码若抛出异常，需回滚 isRunning_ 状态，
    // 否则 UI 永久卡在"运行中"
    try {
    // GUI-01 fix: 启用 debugMode 使 checkBreak 生效
    interpreter_->setDebugMode(true);
    isDebugRun_ = isDebug;
    isRunning_ = true;

    // A-P1-1 fix: 非调试运行时清除残留断点，避免普通运行在调试会话后意外暂停
    // （DebugController::reset() 保留断点供下次调试复用，普通运行需显式清除）
    if (!isDebug) {
        debugger_->setBreakpoints(QSet<int>());
    }

    // R2/D1 fix: 保存 REPL 状态
    interpreter_->saveReplState();

    // 7.1 fix: 使用 unique_ptr 管理生命周期，清理上次运行的 worker
    worker_.reset();
    workerThread_.reset();
    // MEM-01/QT-R-03 fix: 向 worker 传递 shared_ptr，共享 Interpreter/AST 所有权，
    // 避免主线程重新 parse 后 worker 持有悬垂引用
    worker_ = std::make_unique<InterpreterWorker>(interpreter_, astRoot);
    // A-P2-5 fix: 不设 parent，由 unique_ptr 独占管理生命周期，避免双重所有权
    workerThread_.reset(new QThread());
    // C16 fix: 增大 worker 线程栈至 4MB。每次 MiniLang 调用展开 6-10 个 C++ 栈帧，
    // MAX_RECURSION_DEPTH=256 对应约 1500-2500 个 C++ 栈帧，接近 Windows 默认 1MB 栈边界。
    workerThread_->setStackSize(4 * 1024 * 1024);
    worker_->moveToThread(workerThread_.get());

    // 转发 worker 信号到 WorkerManager 信号
    // QT-R-07 fix: worker_.get() 生活于 workerThread_，this 生活于主线程，
    // 跨线程信号槽必须显式指定 Qt::QueuedConnection，避免依赖 AutoConnection 推断。
    // 显式 QueuedConnection 确保：信号被投递到接收方线程事件循环，槽函数在接收方线程执行，
    // 消除数据竞争（worker 线程发射信号 vs 主线程访问 WorkerManager 成员）。
    connect(worker_.get(), &InterpreterWorker::outputReady, this, &WorkerManager::outputReady, Qt::QueuedConnection);
    connect(worker_.get(), &InterpreterWorker::finishedOk, this, &WorkerManager::runOk, Qt::QueuedConnection);
    connect(worker_.get(), &InterpreterWorker::stoppedByUser, this, &WorkerManager::stoppedByUser, Qt::QueuedConnection);
    connect(worker_.get(), &InterpreterWorker::runtimeError, this, &WorkerManager::runtimeError, Qt::QueuedConnection);
    connect(worker_.get(), &InterpreterWorker::genericError, this, &WorkerManager::genericError, Qt::QueuedConnection);

    // 任一终止信号 → 退出线程事件循环
    connect(worker_.get(), &InterpreterWorker::finishedOk, workerThread_.get(), &QThread::quit, Qt::QueuedConnection);
    connect(worker_.get(), &InterpreterWorker::stoppedByUser, workerThread_.get(), &QThread::quit, Qt::QueuedConnection);
    connect(worker_.get(), &InterpreterWorker::runtimeError, workerThread_.get(), &QThread::quit, Qt::QueuedConnection);
    connect(worker_.get(), &InterpreterWorker::genericError, workerThread_.get(), &QThread::quit, Qt::QueuedConnection);

    // 线程结束 → 清理 → 通知 UI
    // QT-R-07 fix: workerThread_.finished 在 workerThread_ 线程发射，this 在主线程，需 QueuedConnection
    connect(workerThread_.get(), &QThread::finished, this, [this]() {
        // P2 fix: 在 cleanupWorker 重置 isDebugRun_ 之前保存其值
        bool wasDebug = isDebugRun_;
        // BUG-REPL-AUDIT-8 fix: cleanupWorker 内部 restoreReplState()/debugger_->reset()
        // 理论上可能抛异常（如 forceStop 终止后状态损坏），未捕获会导致 workerFinished
        // 信号永不发射，UI 永久卡死（按钮禁用、编辑器只读、REPL 输入框禁用）。
        // 此处 try/catch 包裹确保 workerFinished 总会发射，UI 状态总能恢复。
        try {
            cleanupWorker();
        } catch (const std::exception& e) {
            LOG_ERROR(std::string("cleanupWorker threw: ") + e.what(), "IDE");
            isRunning_ = false;
            isDebugRun_ = false;
        } catch (...) {
            isRunning_ = false;
            isDebugRun_ = false;
        }
        emit workerFinished(wasDebug);
    }, Qt::QueuedConnection);
    } catch (...) {
        // BUG-DBG-10 fix: 重置顺序与 cleanupWorker() 对齐。
        // 原顺序 worker_.reset() 在 workerThread_.reset() 之前，且 workerThread_
        // 未 quit/wait 就直接 reset。worker 生活于 workerThread_（moveToThread），
        // 线程亲和性规则要求先 quit+wait 再删除 worker。虽然 catch 路径下线程
        // 尚未 start，但与 cleanupWorker 保持一致避免未来回归。
        isRunning_ = false;
        isDebugRun_ = false;
        interpreter_->setDebugMode(false);
        interpreter_->restoreReplState();
        debugger_->reset();
        // A-P2-1 fix: 先确保线程完全退出，再删除 worker（符合 Qt 线程亲和性规则）
        if (workerThread_) {
            workerThread_->quit();
            workerThread_->wait();
            workerThread_.reset();
        }
        worker_.reset();
        setupMainCallbacks();
        throw;
    }

    return true;
}

void WorkerManager::startWorker() {
    workerThread_->start();
    QMetaObject::invokeMethod(worker_.get(), "run", Qt::QueuedConnection);
}

bool WorkerManager::stopForClose(int timeoutMs) {
    debugger_->stop();
    if (workerThread_) {
        workerThread_->quit();
        if (!workerThread_->wait(timeoutMs)) {
            return false;  // 超时，需强制终止
        }
        // BUG-IDE-15 fix: stopForClose 成功路径需断开 QThread::finished 信号到
        // cleanupWorker lambda 的连接，否则 workerThread_.reset() 后已投递到主线程
        // 事件队列的 QueuedConnection 回调仍会执行 cleanupWorker，导致
        // restoreReplState/debugger_->reset/setupMainCallbacks 被调用两次
        // （一次在下方手动清理，一次在异步 lambda），造成 REPL 状态二次恢复、
        // debugger 重复 reset 等不一致。disconnectAll 确保线程对象的所有信号不再触发。
        workerThread_->disconnect(this);
        // 7.1 fix: unique_ptr 自动释放，无需手动 delete
        worker_.reset();
        workerThread_.reset();
    }
    // AUDIT fix: stopForClose 成功路径需与 forceStop 对齐执行完整状态清理，
    // 否则 interpreter 残留 debugMode、REPL 状态未恢复、debugger 未 reset，
    // 下次运行时可能读到脏状态。
    isRunning_ = false;
    isDebugRun_ = false;
    interpreter_->setDebugMode(false);
    interpreter_->restoreReplState();
    debugger_->reset();
    setupMainCallbacks();
    return true;
}

void WorkerManager::forceStop() {
    if (workerThread_) {
        // A5 fix: 协作式取消 — 通过 debugger_->stop() 设置 stopped_ 标志，
        // 解释器/VM 在 checkBreak() 检测到后抛出 DebugStopException 正常退出。
        // 不再使用 QThread::terminate()（会导致 UB：互斥锁未释放、堆损坏、悬垂信号）。
        debugger_->stop();
        if (workerThread_->wait(5000)) {
            // Worker 已正常停止，安全释放
            // AUDIT-P1 fix: 与 stopForClose/析构路径对齐，reset 前断开 finished 信号，
            // 避免已投递的 QueuedConnection lambda 在 forceStop 返回后触发悬垂 this。
            workerThread_->disconnect(this);
            worker_.reset();
            workerThread_.reset();
        } else {
            // 崩溃修复: Worker 未在 5 秒内停止（死循环未检查 stopped_）。
            // 必须 terminate + join，否则 forceStop 返回后 isRunning_=false，
            // 后续 closeEvent 会 accept 并销毁 Ide，而 worker 线程仍通过
            // QueuedConnection 回写已析构的 IdeController/WorkerManager 成员 →
            // use-after-free → 栈损坏。
            // terminate 风险（pauseMutex_ 可能被持有）：仅当 worker 卡在 checkBreak()
            // 才会持有该锁，但卡在 checkBreak 的 worker 会检测到 stopped_ 并正常退出，
            // 因此到达此分支的 worker 通常在执行用户代码（未持有 DebugController 锁）。
            LOG_ERROR("Worker 未在 5 秒内响应取消请求，回退 terminate() + join（确保 close 路径安全）", "IDE");
            workerThread_->terminate();
            // AUDIT-LIFECYCLE fix: 原 wait() 无超时。Windows 上 TerminateThread 立即生效，
            // 但 Unix 上 pthread_cancel 为延迟取消（PTHREAD_CANCEL_DEFERRED），
            // 若 worker 卡在无取消点的纯 CPU 循环则永不生效，wait() 永久阻塞，
            // std::_Exit(0) 无法触发。加 2 秒超时，超时后直接 _Exit 退出进程。
            if (!workerThread_->wait(2000)) {
                LOG_ERROR("Worker terminate 后 2 秒仍未退出，强制 _Exit", "IDE");
                Logger::instance().flush();
                std::_Exit(0);
            }
            worker_.reset();
            workerThread_.reset();
            // P0 fix: terminate() 在任意指令处杀死 worker，留下损坏的 Interpreter 状态：
            // (1) Value::operator= 的 release→assign 窗口被中断 → box_ 指向已释放 RefCounted
            //     → restoreReplState 析构时 double-release UAF
            // (2) unordered_map rehash / vector realloc 中途被中断 → 容器半重组
            //     → restoreReplState 析构时 double-free 或野指针解引用
            // (3) CRT 堆 critical section 被死线程永久持有 → 后续堆操作崩溃
            // 这三种损坏状态都会被 NaNBox::tag() 合法化为 NaN float（0xFFFFFFFFFFFFFFFF），
            // 掩盖底层 UB。继续执行 restoreReplState/setupMainCallbacks 会立即触发崩溃。
            //
            // 修复策略：terminate 是不可恢复的平台级故障，立即调用 std::_Exit(0) 退出进程，
            // 跳过所有析构（损坏状态由操作系统回收）。这是关闭场景，进程即将退出，
            // 跳过析构是安全的——比尝试析构损坏状态导致崩溃要好得多。
            // 先 flush Logger 确保诊断信息写入文件/控制台。
            Logger::instance().flush();
            std::_Exit(0);
        }
    }
    // A-P1-2 fix: worker 已停止（正常或 terminate），安全执行完整状态清理
    isRunning_ = false;
    isDebugRun_ = false;
    interpreter_->setDebugMode(false);
    interpreter_->restoreReplState();
    debugger_->reset();
    setupMainCallbacks();
}

void WorkerManager::cleanupWorker() {
    isRunning_ = false;
    isDebugRun_ = false;
    interpreter_->setDebugMode(false);
    interpreter_->restoreReplState();
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
