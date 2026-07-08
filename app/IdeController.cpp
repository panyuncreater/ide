#include "IdeController.h"
#include "Logger.h"
#include "interpreter/Environment.h" // #4 fix: VM 条件断点求值
#include "interpreter/Interpreter.h" // P0-3 fix: currentEnvironment() 访问
#include "lexer/Lexer.h"             // #4 fix: VM 条件断点求值
#include "parser/Parser.h"           // #4 fix: VM 条件断点求值
// VM-IMPORT: Compiler 模块加载器所需的 Qt 头文件
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <list>          // AUDIT-P2 fix: condAstCache LRU 实现
#include <unordered_set> // P0-3 fix: getReplScopeVariableNames 去重

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
      ,
      interpreter_(std::make_shared<Interpreter>())
      // MEM-01 fix: 无 Qt parent（shared_ptr 独占管理），避免 Qt parent 自动 delete + shared_ptr 析构的双重所有权
      ,
      debugger_(std::make_shared<DebugController>(nullptr))
      // 4 个协作类（成员 QObjects 不设 parent，避免双重释放）
      ,
      pipeline_(), workerMgr_(interpreter_, debugger_, nullptr), debugCoord_(interpreter_, debugger_, nullptr),
      vmStepper_(nullptr) {

    // 绑定 Interpreter 的调试器
    interpreter_->setDebugger(debugger_);

    // WorkerManager 设置主线程输出/输入回调（interpreter_ 的回调）
    workerMgr_.setupMainCallbacks();

    // VmStepper 设置 VM 输出/输入回调（转发到 IdeController 信号 / WorkerManager 的 input 回调）
    vmStepper_.setOutputCallback([this](const std::string& text) { emit outputReady(QString::fromStdString(text)); });
    vmStepper_.setInputCallback(workerMgr_.buildInputCallback());

    // #4 fix: VM 条件断点求值器 — 使用临时 Interpreter + VM 全局变量 + 局部变量求值
    // VM 模式下 Interpreter 空闲，可安全创建临时实例。
    // 求值在沙箱中进行（evaluateCondition 已实现 #1 变量快照/恢复），
    // 条件中的赋值/声明不会影响 VM 状态。
    // BUG-IDE-12 fix: 现已支持局部变量。Compiler/AstIRBuilder 在编译时记录 slot→name 映射，
    // VM/RegisterVM 通过 getCurrentFrameLocals() 反查当前帧的局部变量名→值，注入临时环境。
    // 局部变量遮蔽同名的全局变量（后注入覆盖先注入）。
    // AUDIT fix: 缓存条件 AST，避免每次断点命中都重新 Lexer+Parser（循环内条件断点性能）。
    // BUG-IDE-04 fix: 缓存添加上限（32 条），防止用户反复切换条件表达式无限增长。
    // AUDIT-P2 fix: 原 unordered_map erase(begin()) 是哈希桶首元素，非 LRU 非 FIFO，
    // 可能误淘汰刚插入的条目。改为 list + unordered_map 经典 LRU：
    // 命中时 move_to_front，超上限时 pop_back（最久未使用）。
    constexpr size_t COND_AST_CACHE_MAX = 32;
    using CacheList = std::list<std::pair<std::string, std::shared_ptr<Block>>>;
    using CacheLookup = std::unordered_map<std::string, CacheList::iterator>;
    auto cacheList = std::make_shared<CacheList>();
    auto cacheLookup = std::make_shared<CacheLookup>();
    vmStepper_.setConditionEvaluator([this, cacheList, cacheLookup](const std::string& condition) -> bool {
        try {
            std::shared_ptr<Block> ast;
            auto lookupIt = cacheLookup->find(condition);
            if (lookupIt != cacheLookup->end()) {
                // 命中：move_to_front（最近使用）
                cacheList->splice(cacheList->begin(), *cacheList, lookupIt->second);
                ast = lookupIt->second->second;
            } else {
                Lexer condLexer;
                auto tokens = condLexer.scan(condition);
                Parser condParser;
                auto parsed = condParser.parse(tokens);
                if (!parsed || parsed->statements.empty() || condParser.getDiagnostics().hasErrors()) {
                    return false;
                }
                ast = std::shared_ptr<Block>(std::move(parsed));
                // LRU 淘汰：超上限时移除最久未使用（链表尾部）
                if (cacheList->size() >= COND_AST_CACHE_MAX) {
                    cacheLookup->erase(cacheList->back().first);
                    cacheList->pop_back();
                }
                cacheList->emplace_front(condition, ast);
                (*cacheLookup)[condition] = cacheList->begin();
            }
            Interpreter tempInterp;
            auto env = std::make_shared<Environment>();
            // BUG-IDE-12 fix: 先注入全局变量，再注入当前帧局部变量（局部变量遮蔽同名全局）
            for (const auto& kv : vmStepper_.getGlobals()) {
                env->define(kv.first, kv.second);
            }
            for (const auto& kv : vmStepper_.getCurrentFrameLocals()) {
                env->define(kv.first, kv.second);
            }
            // P2-E fix: 若存在 "this" 变量且为实例，绑定 boundInstance_ 使裸字段名
            // 可回退解析实例字段（与方法体执行语义一致）。
            // 原 Bug：仅注入 localSlotNames 中的变量，动态添加的字段（不在 fieldOrder 中）
            // 无法通过 boundInstance_ 回退解析，条件断点永不触发。
            auto& vars = env->localVariables();
            auto thisIt = vars.find("this");
            if (thisIt != vars.end() && thisIt->second.isInstance()) {
                // unordered_map 是 node-based，rehash 不失效指针，env 生命周期内有效
                env->bindInstance(const_cast<Value*>(&thisIt->second));
            }
            tempInterp.setGlobalEnvironment(env);
            Value result = tempInterp.evaluateCondition(ast->statements[0].get());
            return result.isTruthy();
        } catch (const std::exception& e) {
            std::string msg = e.what();
            // AUDIT-BUG-C8 fix: 改用 LOG_* 宏，先检查级别再构造消息（懒求值）。
            LOG_WARNING("VM 条件断点求值异常: " + msg + "（条件: " + condition + "），视为条件不满足", "VmStepper");
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

    // OPT-1: 将异步状态变更信号桥接到观察者通知，让订阅面板即时刷新而非 500ms 轮询。
    // - vmRunPaused：VM RUN 模式异步暂停（命中断点/结束/错误）
    // - pausedAt：Interpreter 调试模式暂停
    // - workerFinished：运行结束（正常/停止/错误），状态归零
    connect(&vmStepper_, &VmStepper::vmRunPaused, this, [this](VmStepResult) { notifyVmStateChanged(); });
    connect(&debugCoord_, &DebugCoordinator::pausedAt, this, [this](int) { notifyVmStateChanged(); });
    connect(&workerMgr_, &WorkerManager::workerFinished, this, [this](bool) { notifyVmStateChanged(); });
}

IdeController::~IdeController() {
    // 协作类作为成员自动析构。
    // WorkerManager 析构会安全停止 worker 线程（5s 等待 + terminate 兜底）。
    // VmStepper 析构会停止 RUN 模式定时器。
}

// ============================================================
// 管线操作
// ============================================================

/// 执行字节码编译：调用 PipelineRunner 编译 AST，刷新诊断与字节码/IR 视图。
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
        // OPT-1: 编译结果就绪，VM 步进可用，通知订阅面板刷新（如 BytecodeTracePanel）。
        notifyVmStateChanged();
    }
    return ok;
}

// ============================================================
// Worker 线程管理（协调 PipelineRunner + WorkerManager + VmStepper）
// ============================================================

/// 准备运行：失效管线缓存、运行前端管线产出 AST，供 WorkerManager 启动解释器。
bool IdeController::prepareRun(bool isDebug, const std::string& source, const std::string& filePath) {
    // E2 fix: 各静默 return false 路径补 emit genericError，避免 UI 已 clearAll 后
    // 用户看不到任何反馈（onRun/onDebug 调用 prepareRun 前已 clearAll 输出面板）。
    if (workerMgr_.isRunning()) {
        emit genericError("已有运行在进行，请先停止当前运行");
        return false;
    }
    // BUG-IDE-02 fix: 检查 VM 是否正在运行。VM RUN 模式异步执行期间 isRunning() 仅反映
    // workerMgr 状态，VM 步进/RUN 仍可能活跃。若不同步停止，runCompiler/编译结果同步
    // 会让 frame.chunk 悬垂，且 workerMgr 启动新 worker 时 VM 仍在动旧 CompileResult。
    if (vmStepper_.isRunning()) {
        emit genericError("VM RUN 模式正在执行，请先停止 VM 再启动新运行");
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
                                  .arg(pipelineResult.status == PipelineRunner::PipelineStatus::LexerFailed
                                           ? "词法分析异常"
                                           : "解析异常")
                                  .arg(QString::fromStdString(pipelineResult.errorMessage)));
        }
        return false;
    }

    if (!pipeline_.astRoot()) {
        emit genericError("内部错误：前端管线返回成功但 AST 为空");
        return false;
    }

    // BUG-IDE-PREP-1 fix: prepareRun 必须重新编译当前源码，否则同步到 VmStepper 的
    // 是上一次编译结果（pipeline_.lastCompileResult() / getLastRegisterResult()），
    // 与本次源码不对应——用户修改源码后不点"编译"直接点"运行/调试"会执行旧字节码。
    // 注意：runCompiler 内部已检查 vmStepper_.isRunning()（上方 BUG-IDE-02 已先拦截），
    // 且编译错误通过 diagnosticsReady 信号报告，此处不再 emit 重复错误。
    if (!runCompiler()) {
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

    // OPT-1: 新运行已就绪，通知订阅面板刷新（CallStack/VariableInspector 等显示运行状态）。
    notifyVmStateChanged();
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

// ============================================================
// BUG-REPL-AUDIT-9 fix: REPL 模块加载器补设
// ------------------------------------------------------------
// 原 executeRepl 仅裸转发 interpreter_->executeRepl，而 Interpreter 的
// moduleLoader_/moduleMtimeChecker_/currentFilePath_ 仅在 WorkerManager::prepareRun
// （Run/Debug 路径）中被设置。用户启动 IDE 后直接在 REPL 输入 import 会命中
// "未设置模块加载器，无法执行 import"。
//
// 修复策略：executeRepl 执行前调用 setupReplModuleCallbacks()。若 Interpreter 已有
// moduleLoader_（先 Run 过），不重复设置（避免覆盖 Run 建立的 baseDir）；否则基于
// currentFilePath_（由 GUI 通过 setActiveFilePath 通知，或 prepareRun 设置）建立
// loader/mtimeChecker，复用 WorkerManager::prepareRun 的路径解析逻辑。
//
// 路径遍历防护（拒绝 ".." 和绝对路径）由 Interpreter::visitImportStmt 前置检查
// 保证（InterpreterModules.cpp 行 38-56），loader 不会收到非法路径。
// ============================================================
Value IdeController::executeRepl(Block& program) {
    setupReplModuleCallbacks();
    return interpreter_->executeRepl(program);
}

// P0-3 fix (F11): 实现 getReplScopeVariableNames，供 GUI ErrorHintEngine 拼写建议使用
std::vector<std::string> IdeController::getReplScopeVariableNames() const {
    std::vector<std::string> names;
    Environment* env = interpreter_->currentEnvironment();
    if (!env)
        return names;
    auto vars = env->allVariables(); // 父作用域在前，子作用域追加末尾
    names.reserve(vars.size());
    // 去重（保留首次出现，即更外层作用域的同名变量；拼写建议无强顺序要求）
    std::unordered_set<std::string> seen;
    seen.reserve(vars.size() * 2);
    for (const auto& kv : vars) {
        if (seen.insert(kv.first).second) {
            names.push_back(kv.first);
        }
    }
    return names;
}

// N1 fix: 同步运行 MiniLang 源码并捕获 print 输出（用于 EXPECTED_OUTPUT 自动判分）
std::string IdeController::runStringCaptureOutput(const std::string& source) {
    try {
        Lexer lexer;
        auto tokens = lexer.scan(source);
        if (lexer.getDiagnostics().hasErrors()) {
            return std::string("!ERROR: Lexer error");
        }
        Parser parser;
        auto ast = parser.parse(tokens);
        if (!ast || parser.hasErrors()) {
            return std::string("!ERROR: Parse error");
        }
        Interpreter interp;
        std::string captured;
        interp.setOutputCallback([&captured](const std::string& text) { captured += text; });
        interp.execute(*ast);
        return captured;
    } catch (const std::exception& e) {
        return std::string("!ERROR: ") + e.what();
    } catch (...) {
        return std::string("!ERROR: Unknown exception");
    }
}

/// 设置 REPL 模块加载回调：将相对 import 路径解析到当前活动文件所在目录。
void IdeController::setupReplModuleCallbacks() {
    // 若 Interpreter 已有 moduleLoader_（先 Run 过），不覆盖，保持 Run 时建立的
    // baseDir 与模块缓存基准一致（避免 Run 后 REPL 用不同的 baseDir）。
    if (interpreter_->hasModuleLoader()) {
        return;
    }

    // 基于 currentFilePath_ 解析相对模块路径（对齐 WorkerManager::prepareRun 逻辑）
    const std::string& filePath = currentFilePath_;
    QString baseDir;
    if (!filePath.empty()) {
        QFileInfo fi(QString::fromStdString(filePath));
        baseDir = fi.absolutePath();
    }
    interpreter_->setCurrentFilePath(filePath);

    // 模块路径解析辅助函数，供 loader 和 mtime checker 共用
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
        if (resolved.isEmpty())
            return "";
        QFile file(resolved);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return QString::fromUtf8(file.readAll()).toStdString();
        }
        return "";
    });
    interpreter_->setModuleMtimeChecker([resolveModulePath](const std::string& modulePath) -> int64_t {
        QString resolved = resolveModulePath(modulePath);
        if (resolved.isEmpty())
            return 0;
        QFileInfo fi(resolved);
        if (!fi.exists())
            return 0;
        return fi.lastModified().toMSecsSinceEpoch();
    });
}

// ============================================================
// OPT-1: VM 操作实现说明
// ------------------------------------------------------------
// vmStep / vmStepByMode / vmStop / vmReset / setBreakpoints /
// setVmBreakpoints / setBreakpointCondition 均保持 inline（定义在头文件），
// 在调用底层后追加 notifyVmStateChanged() 通知订阅面板。
//
// 保持 inline 的理由：MagicCommands.cpp（minilang_core）依赖这些方法的内联符号，
// 测试目标 minilang_tests 仅链接 minilang_core，不链接 app/IdeController.cpp。
// notifyVmStateChanged() 也是 inline（纯 std::vector<std::function> 迭代），
// 无 moc 依赖，面板 .cpp 可安全编译进测试目标。
// ============================================================
