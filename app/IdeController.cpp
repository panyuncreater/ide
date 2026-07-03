#include "IdeController.h"
#include "Logger.h"
#include "lexer/Lexer.h"      // #4 fix: VM 条件断点求值
#include "parser/Parser.h"     // #4 fix: VM 条件断点求值
#include "interpreter/Environment.h"  // #4 fix: VM 条件断点求值
// VM-IMPORT: Compiler 模块加载器所需的 Qt 头文件
#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QStringList>

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

    // #4 fix: VM 条件断点求值器 — 使用临时 Interpreter + VM 全局变量求值
    // VM 模式下 Interpreter 空闲，可安全创建临时实例。
    // 求值在沙箱中进行（evaluateCondition 已实现 #1 变量快照/恢复），
    // 条件中的赋值/声明不会影响 VM 状态。
    // 限制：仅支持引用全局变量（VM 局部变量在寄存器/栈中，无法按名访问）。
    // AUDIT fix: 缓存条件 AST，避免每次断点命中都重新 Lexer+Parser（循环内条件断点性能）。
    auto condAstCache = std::make_shared<std::unordered_map<std::string, std::shared_ptr<Block>>>();
    vmStepper_.setConditionEvaluator([this, condAstCache](const std::string& condition) -> bool {
        try {
            std::shared_ptr<Block> ast;
            auto cacheIt = condAstCache->find(condition);
            if (cacheIt != condAstCache->end()) {
                ast = cacheIt->second;
            } else {
                Lexer condLexer;
                auto tokens = condLexer.scan(condition);
                Parser condParser;
                auto parsed = condParser.parse(tokens);
                if (!parsed || parsed->statements.empty() || condParser.getDiagnostics().hasErrors()) {
                    return false;
                }
                ast = std::shared_ptr<Block>(std::move(parsed));
                condAstCache->emplace(condition, ast);
            }
            Interpreter tempInterp;
            auto env = std::make_shared<Environment>();
            for (const auto& kv : vmStepper_.getGlobals()) {
                env->define(kv.first, kv.second);
            }
            tempInterp.setGlobalEnvironment(env);
            Value result = tempInterp.evaluateCondition(ast->statements[0].get());
            return result.isTruthy();
        } catch (const std::exception& e) {
            // AUDIT-BUG-C8 fix: 改用 LOG_* 宏，先检查级别再构造消息（懒求值）。
            LOG_WARNING("VM 条件断点求值异常: " + std::string(e.what()) +
                        "（条件: " + condition + "），视为条件不满足", "VmStepper");
            return false;
        } catch (...) {
            return false;
        }
    });

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
    // B1 fix: VM RUN 模式活跃时拒绝 runCompiler，避免替换 CompileResult 致 frame.chunk 悬垂 UAF。
    // 调用方（onShowBytecode/onShowIR）在 VM 运行时应先 vmStop()。
    if (vmStepper_.isRunning()) {
        emit genericError("VM RUN 模式正在执行，请先停止 VM 再重新编译");
        return false;
    }
    // VM-IMPORT: 编译前为 Compiler 设置模块加载器（使 VM 路径支持 import 语句）
    setupCompilerModuleLoader(currentFilePath_);
    bool ok = pipeline_.runCompiler();
    // 编译成功后同步更新 VmStepper 的编译结果，使 VM 步进可用
    if (ok) {
        // A1 fix: 双后端编译结果同步。Compiler 根据 useRegisterVM_ 选择走栈式
        // 还是寄存器式路径，分别产生 CompileResult 和 RegisterCompileResult。
        // VmStepper 需要拿到对应后端的结果才能 initExecution。
        if (pipeline_.compiler().getUseRegisterVM()) {
            vmStepper_.setRegisterCompileResult(pipeline_.compiler().getLastRegisterResult());
        } else {
            vmStepper_.setCompileResult(pipeline_.lastCompileResult());
        }
    }
    return ok;
}

// ============================================================
// Worker 线程管理（协调 PipelineRunner + WorkerManager + VmStepper）
// ============================================================

bool IdeController::prepareRun(bool isDebug, const std::string& source, const std::string& filePath) {
    // E2 fix: 各静默 return false 路径补 emit genericError，避免 UI 已 clearAll 后
    // 用户看不到任何反馈（onRun/onDebug 调用 prepareRun 前已 clearAll 输出面板）。
    if (workerMgr_.isRunning()) {
        emit genericError("已有运行在进行，请先停止当前运行");
        return false;
    }

    // VM-IMPORT: 保存文件路径，并为 Compiler 设置模块加载器（VM 编译路径需要）
    currentFilePath_ = filePath;
    setupCompilerModuleLoader(filePath);

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

    if (!pipeline_.astRoot()) {
        emit genericError("内部错误：前端管线返回成功但 AST 为空");
        return false;
    }

    // 委托 WorkerManager 设置模块加载器、调试模式、创建 worker 线程
    if (!workerMgr_.prepareRun(isDebug, pipeline_.astRoot(), filePath)) {
        // WorkerManager 内部已 emit 错误或抛异常（catch 内回滚），此处不重复
        return false;
    }

    // 同步编译结果到 VmStepper（供 VM 模式调试使用）
    // A1 fix: 根据 useRegisterVM_ 选择同步栈式或寄存器式结果
    // BUG-DBG-2 fix: 先重置 VM 状态，防止旧 isVmInitialized_=true 时 frame.chunk 悬垂
    vmStepper_.reset();
    if (pipeline_.compiler().getUseRegisterVM()) {
        vmStepper_.setRegisterCompileResult(pipeline_.compiler().getLastRegisterResult());
    } else {
        vmStepper_.setCompileResult(pipeline_.lastCompileResult());
    }

    return true;
}

// ============================================================
// VM-IMPORT: Compiler 模块加载器设置
// ------------------------------------------------------------
// 对齐 WorkerManager::prepareRun 中为 Interpreter 设置的模块加载器逻辑，
// 使 VM 编译路径（栈式 / IR / 寄存器式）也能处理 import 语句。
// 模块路径解析策略：
//   1. 以当前文件所在目录为基准查找 <modulePath>.mini
//   2. 若 baseDir 为空，则按字面路径查找
//   3. 自动补充 .mini 后缀
// 找不到时返回空字符串，由 Compiler::visitImportStmt 报错
// ============================================================
void IdeController::setupCompilerModuleLoader(const std::string& filePath) {
    QString baseDir;
    if (!filePath.empty()) {
        QFileInfo fi(QString::fromStdString(filePath));
        baseDir = fi.absolutePath();
    }
    // 设置当前文件路径（Compiler 用于相对路径解析）
    pipeline_.compiler().setCurrentFilePath(filePath);
    // 捕获 baseDir 副本到 lambda（独立于 IdeController 生命周期，
    // Compiler 持有 std::function 直到下次设置或析构）
    pipeline_.compiler().setModuleLoader([baseDir](const std::string& modulePath) -> std::string {
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
            QFile file(candidate);
            if (file.exists() && file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                return QString::fromUtf8(file.readAll()).toStdString();
            }
        }
        return "";
    });
}
