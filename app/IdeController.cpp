#include "IdeController.h"
#include "Logger.h"

// ============================================================
// IdeController — 业务逻辑层 Facade 实现（ARCH-11 重构）
// ------------------------------------------------------------
// 原 God Object 逻辑已拆分到 4 个协作类：
//   - PipelineRunner    管线操作（runLexer/runParser/runCompiler/formatCode/runFrontendPipeline）
//   - WorkerManager     Worker 线程管理（prepareRun/startWorker/stopForClose/forceStop/cleanupWorker）
//   - DebugCoordinator  调试状态/回调（setupDebug/stepIn/Over/Out/Resume/Stop）
//   - VmStepper         VM 步进状态机（step/stepByMode/stop/runBatch）
//
// IdeController 仅负责：
//   1. 构造时创建共享引擎 + 4 个协作类 + 信号转发
//   2. runCompiler 后同步 VmStepper 编译结果
//   3. prepareRun 协调 PipelineRunner → WorkerManager → VmStepper
// ============================================================

IdeController::IdeController(QObject* parent)
    : QObject(parent)
    // MEM-01 fix: shared_ptr 共享所有权，worker 线程持有 Interpreter 期间 IdeController 析构不会悬垂
    , interpreter_(std::make_shared<Interpreter>())
    // MEM-01 fix: 无 Qt parent（shared_ptr 独占管理），避免 Qt parent 自动 delete + shared_ptr 析构的双重所有权
    , debugger_(std::make_shared<DebugController>(nullptr))
    // 4 个协作类（成员 QObjects 不设 parent，避免双重释放）
    , pipeline_()
    , workerMgr_(interpreter_, debugger_, nullptr)
    , debugCoord_(interpreter_, debugger_, nullptr)
    , vmStepper_(nullptr) {

    // 绑定 Interpreter 的调试器
    interpreter_->setDebugger(debugger_);

    // WorkerManager 设置主线程输出/输入回调（interpreter_ 的回调）
    workerMgr_.setupMainCallbacks();

    // VmStepper 设置 VM 输出/输入回调（转发到 IdeController 信号 / WorkerManager 的 input 回调）
    vmStepper_.setOutputCallback([this](const std::string& text) {
        emit outputReady(QString::fromStdString(text));
    });
    vmStepper_.setInputCallback(workerMgr_.buildInputCallback());

    // ---- 转发协作类信号到 IdeController 信号（GUI 层连接 IdeController 信号）----
    connect(&pipeline_, &PipelineRunner::diagnosticsReady, this, &IdeController::diagnosticsReady);
    connect(&workerMgr_, &WorkerManager::outputReady, this, &IdeController::outputReady);
    connect(&workerMgr_, &WorkerManager::runOk, this, &IdeController::runOk);
    connect(&workerMgr_, &WorkerManager::stoppedByUser, this, &IdeController::stoppedByUser);
    connect(&workerMgr_, &WorkerManager::runtimeError, this, &IdeController::runtimeError);
    connect(&workerMgr_, &WorkerManager::genericError, this, &IdeController::genericError);
    connect(&workerMgr_, &WorkerManager::workerFinished, this, &IdeController::workerFinished);
    connect(&debugCoord_, &DebugCoordinator::pausedAt, this, &IdeController::pausedAt);
    connect(&vmStepper_, &VmStepper::vmRunPaused, this, &IdeController::vmRunPaused);
}

IdeController::~IdeController() {
    // 协作类作为成员自动析构。
    // WorkerManager 析构会安全停止 worker 线程（5s 等待 + terminate 兜底）。
    // VmStepper 析构会停止 RUN 模式定时器。
}

// ============================================================
// 管线操作
// ============================================================

bool IdeController::runCompiler() {
    bool ok = pipeline_.runCompiler();
    // 编译成功后同步更新 VmStepper 的编译结果，使 VM 步进可用
    if (ok) {
        vmStepper_.setCompileResult(pipeline_.lastCompileResult());
    }
    return ok;
}

// ============================================================
// Worker 线程管理（协调 PipelineRunner + WorkerManager + VmStepper）
// ============================================================

bool IdeController::prepareRun(bool isDebug, const std::string& source, const std::string& filePath) {
    if (workerMgr_.isRunning()) return false;

    // C9 fix: 使用统一前端管线（Lexer + Parser）
    auto pipelineResult = pipeline_.runFrontendPipeline(source);
    if (pipelineResult.status != PipelineRunner::PipelineStatus::OK) {
        if (!pipelineResult.errorMessage.empty()) {
            emit genericError(QString("%1: %2")
                .arg(pipelineResult.status == PipelineRunner::PipelineStatus::LexerFailed ? "词法分析异常" : "解析异常")
                .arg(QString::fromStdString(pipelineResult.errorMessage)));
        }
        return false;
    }

    if (!pipeline_.astRoot()) return false;

    // 委托 WorkerManager 设置模块加载器、调试模式、创建 worker 线程
    if (!workerMgr_.prepareRun(isDebug, pipeline_.astRoot(), filePath)) {
        return false;
    }

    // 同步编译结果到 VmStepper（供 VM 模式调试使用）
    vmStepper_.setCompileResult(pipeline_.lastCompileResult());

    return true;
}
