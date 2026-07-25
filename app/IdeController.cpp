#include "IdeController.h"
#include "common/BuiltinModules.h" // P2-11: std/* 内建模块拦截
#include "common/Logger.h"
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
    // P2-2 fix: 捕获 vmStepper_ 裸指针而非 this。lambda 存储在 vmStepper_ 的成员
    // vmConditionEvaluator_ 中，其生命周期不超过 vmStepper_，而 vmStepper_ 是
    // IdeController 的成员（同生命周期），因此 stepptr 在 lambda 存活期间始终有效。
    // 改为捕获 stepptr 而非 this 避免隐式依赖整个 IdeController 析构顺序。
    VmStepper* stepptr = &vmStepper_;
    vmStepper_.setConditionEvaluator([stepptr, cacheList, cacheLookup](const std::string& condition) -> bool {
        try {
            // P2-1 fix: 检查 VM 停止请求，快速中止条件求值（对齐 Interpreter 路径
            // evaluateCondition 中检查 stopRequested_ 的机制）。
            if (stepptr->isCondStopRequested())
                return false;
            std::shared_ptr<Block> ast;
            // 条件断点修复(R86): 自动补充分号——用户输入的条件表达式（如 i%2==1）
            // 通常不带分号，但 Parser::parse() 的 expressionStatement() 要求分号，
            // 缺失分号导致解析失败 → 条件求值返回 false → 断点永不触发。
            std::string condExpr = condition;
            if (!condExpr.empty() && condExpr.back() != ';') {
                condExpr += ';';
            }
            auto lookupIt = cacheLookup->find(condExpr);
            if (lookupIt != cacheLookup->end()) {
                // 命中：move_to_front（最近使用）
                cacheList->splice(cacheList->begin(), *cacheList, lookupIt->second);
                ast = lookupIt->second->second;
            } else {
                Lexer condLexer;
                auto tokens = condLexer.scan(condExpr);
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
                cacheList->emplace_front(condExpr, ast);
                (*cacheLookup)[condExpr] = cacheList->begin();
            }
            Interpreter tempInterp;
            auto env = std::make_shared<Environment>();
            // BUG-IDE-12 fix: 先注入全局变量，再注入当前帧局部变量（局部变量遮蔽同名全局）
            for (const auto& kv : stepptr->getGlobals()) {
                env->define(kv.first, kv.second);
            }
            for (const auto& kv : stepptr->getCurrentFrameLocals()) {
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

    // R121: 新运行启动时清除上一轮选中帧（不应继承到新调试会话）
    clearSelectedFrame();

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
        // P2-11: 内建标准库模块拦截（std/math, std/string, std/list）
        if (BuiltinModuleRegistry::isBuiltinModule(modulePath)) {
            return BuiltinModuleRegistry::getSource(modulePath);
        }
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
                // P3.4 fix: 区分"文件不存在"与"空文件"。文件存在但内容为空时
                // 返回 "\n"（合法空白源码，Lexer 产出 [TK_EOF]，Parser 产出空 Block），
                // 而非 ""（被三后端视为加载失败）。这样 import 一个 0 字节 .mini 文件
                // 会得到一个合法的空模块（无导出），而非 ModuleNotFound 错误。
                std::string content = QString::fromUtf8(file.readAll()).toStdString();
                return content.empty() ? "\n" : content;
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
// R53-1 fix: 对齐 DebugCoordinator 的 AUDIT-P1 修复模式，使用 currentEnvironmentShared()
// 替代裸 currentEnvironment()。该方法可能在 worker 线程 emit runtimeError 后被主线程
// QueuedConnection 处理器调用，裸指针遍历 parent 链违反 Interpreter.h 跨线程契约。
// shared_ptr 副本通过赋值延长 parent 链每个节点生命周期，防止 worker 线程修改或析构
// Environment 时主线程解引用悬挂指针。
std::vector<std::string> IdeController::getReplScopeVariableNames() const {
    std::vector<std::string> names;
    auto env = interpreter_->currentEnvironmentShared();
    if (!env)
        return names;
    // 手动遍历 parent 链（用 shared_ptr 赋值延长每个节点生命周期），
    // 顺序与 allVariables() 一致：父作用域在前，子作用域追加末尾。
    std::unordered_set<std::string> seen;
    while (env) {
        const auto& locals = env->localVariables();
        for (const auto& kv : locals) {
            if (seen.insert(kv.first).second) {
                names.push_back(kv.first);
            }
        }
        env = env->parent; // shared_ptr 赋值，延长 parent 生命周期
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
        // R62-4 fix: 每个 print 语句的输出后追加换行，对齐 IDE 主输出面板
        // (QTextEdit::append) 的行为。此前回调仅拼接 raw text，多条 print
        // 输出会被粘在一起（如 print(1);print(2); → "12" 而非 "1\n2"），
        // 导致 lab-07/lab-08 等多 print 预期输出题永远判分失败。
        interp.setOutputCallback([&captured](const std::string& text) {
            captured += text;
            captured += "\n";
        });
        interp.execute(*ast);
        return captured;
    } catch (const std::exception& e) {
        return std::string("!ERROR: ") + e.what();
    } catch (...) {
        return std::string("!ERROR: Unknown exception");
    }
}

// ============================================================
// R117 Watch 表达式求值（调试器拓展）
// ------------------------------------------------------------
// 在当前调试暂停上下文中求值用户输入的表达式，返回完整 Value 信息供
// WatchPanel 显示。实现策略与 setConditionEvaluator 一致：
//   1. 自动补充分号（用户输入通常不带分号）
//   2. LRU 缓存 AST（32 条上限），避免循环内重复 Lexer+Parser
//   3. 创建临时 Interpreter + 沙箱 Environment，注入 globals/locals
//   4. 若存在 this 且为实例，绑定 boundInstance_ 使裸字段名可访问
//   5. evaluateCondition 返回 Value（沙箱化，状态自动恢复），
//      WatchPanel 用完整 Value 而非 isTruthy() 展示类型+值
//
// 与条件断点求值的区别：
//   - 条件断点求值仅返回 bool（isTruthy），Watch 返回完整 Value
//   - 条件断点求值仅覆盖 VM 路径（VmStepper 回调），Watch 同时覆盖
//     VM 路径与 Interpreter 调试暂停路径（直接调用，无 worker 线程冲突）
//   - 条件断点求值的 LRU 缓存在 lambda 闭包内（VmStepper 持有），
//     Watch 的 LRU 缓存在 IdeController 成员中（多次调用共享）
//
// 线程安全：本方法在 GUI 线程调用，访问的 vmStepper_/debugCoord_ 状态
// 在暂停期间稳定（VM 暂停 / Interpreter 在 worker 线程暂停中），
// 与 setConditionEvaluator 的线程模型一致。
// ============================================================

// 静态 LRU 缓存上限（与条件断点 AST 缓存对齐）
namespace {
constexpr size_t WATCH_AST_CACHE_MAX = 32;
}

// ============================================================
// R121 调用栈帧切换：获取所选帧的局部变量
// ------------------------------------------------------------
// 实现已移至 IdeController.h 内联定义（R121-build fix）。
// 原因：VariableInspectorPanel.cpp 调用此方法，但 minilang_tests 不链接
// IdeController.cpp。改为 inline 使符号在调用方编译单元内可见。
// ============================================================

IdeController::WatchResult IdeController::evaluateWatchExpression(const std::string& expr) {
    WatchResult result;
    if (expr.empty()) {
        result.error = "表达式为空";
        return result;
    }

    // 判定当前暂停模式，收集要注入的变量
    bool vmMode = vmStepper_.isRunning() || vmStepper_.isInitialized();
    bool interpPaused = debugCoord_.isPaused();
    if (!vmMode && !interpPaused) {
        result.error = "未在调试暂停状态（请先调试运行并命中断点）";
        return result;
    }

    try {
        // ---- 1. 自动补充分号 + LRU AST 缓存 ----
        std::string normExpr = expr;
        if (!normExpr.empty() && normExpr.back() != ';') {
            normExpr += ';';
        }
        std::shared_ptr<Block> ast;
        auto lookupIt = watchAstCacheLookup_.find(normExpr);
        if (lookupIt != watchAstCacheLookup_.end()) {
            // 命中：move_to_front（最近使用）
            watchAstCacheList_.splice(watchAstCacheList_.begin(), watchAstCacheList_, lookupIt->second);
            ast = lookupIt->second->second;
        } else {
            Lexer lexer;
            auto tokens = lexer.scan(normExpr);
            Parser parser;
            auto parsed = parser.parse(tokens);
            if (!parsed || parsed->statements.empty() || parser.getDiagnostics().hasErrors()) {
                result.error = "表达式解析失败";
                return result;
            }
            ast = std::shared_ptr<Block>(std::move(parsed));
            // LRU 淘汰：超上限时移除最久未使用（链表尾部）
            if (watchAstCacheList_.size() >= WATCH_AST_CACHE_MAX) {
                watchAstCacheLookup_.erase(watchAstCacheList_.back().first);
                watchAstCacheList_.pop_back();
            }
            watchAstCacheList_.emplace_front(normExpr, ast);
            watchAstCacheLookup_[normExpr] = watchAstCacheList_.begin();
        }

        // ---- 2. 创建临时 Interpreter + 注入变量 ----
        Interpreter tempInterp;
        auto env = std::make_shared<Environment>();
        if (vmMode) {
            // VM 模式：注入 VM globals + 所选帧 locals（局部变量遮蔽同名全局）
            for (const auto& kv : vmStepper_.getGlobals()) {
                env->define(kv.first, kv.second);
            }
            // R121: 用 getSelectedFrameLocals() 替代 getCurrentFrameLocals()，
            // 支持用户切换到调用栈任意帧查看该帧上下文。selectedFrame_ == -1 时
            // 返回栈顶 locals（与原 getCurrentFrameLocals 行为一致，向后兼容）。
            for (const auto& kv : getSelectedFrameLocals()) {
                env->define(kv.first, kv.second);
            }
        } else {
            // Interpreter 调试暂停模式：
            // - 注入全局 + 栈顶 upvalue/locals 快照（保持 R117 行为）
            // - 若 selectedFrame_ >= 0，用所选帧 locals 覆盖（R121 帧切换）
            //   env->define 同名变量后写覆盖前写，实现"切换到帧 N 时看到帧 N 的 locals"
            for (const auto& snap : debugCoord_.getDebugVariableSnapshot()) {
                env->define(snap.name, snap.value);
            }
            if (selectedFrame_ >= 0) {
                for (const auto& kv : getSelectedFrameLocals()) {
                    env->define(kv.first, kv.second);
                }
            }
        }
        // this 绑定：若存在 this 变量且为实例，绑定 boundInstance_ 使裸字段名
        // 可回退解析实例字段（与方法体执行语义一致）。
        auto& vars = env->localVariables();
        auto thisIt = vars.find("this");
        if (thisIt != vars.end() && thisIt->second.isInstance()) {
            env->bindInstance(const_cast<Value*>(&thisIt->second));
        }
        tempInterp.setGlobalEnvironment(env);

        // ---- 3. 沙箱求值，返回完整 Value ----
        Value v = tempInterp.evaluateCondition(ast->statements[0].get());
        result.ok = true;
        result.value = v;
        result.typeName = v.typeName();
        result.valueRepr = v.toString();
        return result;
    } catch (const std::exception& e) {
        result.error = std::string("求值异常: ") + e.what();
        LOG_WARNING("Watch 表达式求值异常: " + result.error + "（表达式: " + expr + "）", "IdeController");
        return result;
    } catch (...) {
        result.error = "求值异常: 未知错误";
        return result;
    }
}

// ---- R161 调试器 REPL 求值 ----
// 与 evaluateWatchExpression 的核心区别：
// 1. 允许多条语句（Block 而非单表达式），用 executeRepl 执行整个 Block
// 2. 允许 var/print/赋值语句，临时 Interpreter 独立实例天然隔离副作用
// 3. 捕获 print 输出到 output 参数（供 ReplPanel 显示）
// 4. 不做沙箱恢复（evaluateCondition 的 SandboxGuard 会撤销赋值，调试器 REPL 不需要）
IdeController::WatchResult IdeController::evaluateDebuggerRepl(const std::string& source, std::string* output) {
    WatchResult result;
    if (source.empty()) {
        result.error = "输入为空";
        return result;
    }

    // 判定当前暂停模式，收集要注入的变量
    bool vmMode = vmStepper_.isRunning() || vmStepper_.isInitialized();
    bool interpPaused = debugCoord_.isPaused();
    if (!vmMode && !interpPaused) {
        result.error = "未在调试暂停状态（请先调试运行并命中断点）";
        return result;
    }

    try {
        // ---- 1. 自动补充分号 + LRU AST 缓存 ----
        std::string normSource = source;
        if (!normSource.empty() && normSource.back() != ';') {
            normSource += ';';
        }
        std::shared_ptr<Block> ast;
        auto lookupIt = watchAstCacheLookup_.find(normSource);
        if (lookupIt != watchAstCacheLookup_.end()) {
            watchAstCacheList_.splice(watchAstCacheList_.begin(), watchAstCacheList_, lookupIt->second);
            ast = lookupIt->second->second;
        } else {
            Lexer lexer;
            auto tokens = lexer.scan(normSource);
            Parser parser;
            auto parsed = parser.parse(tokens);
            if (!parsed || parsed->statements.empty() || parser.getDiagnostics().hasErrors()) {
                // 解析失败时尝试提取诊断信息
                const auto& diags = parser.getDiagnostics();
                if (!diags.all().empty()) {
                    result.error = "语法错误: " + diags.all()[0].message;
                } else {
                    result.error = "语法解析失败";
                }
                return result;
            }
            ast = std::shared_ptr<Block>(std::move(parsed));
            if (watchAstCacheList_.size() >= WATCH_AST_CACHE_MAX) {
                watchAstCacheLookup_.erase(watchAstCacheList_.back().first);
                watchAstCacheList_.pop_back();
            }
            watchAstCacheList_.emplace_front(normSource, ast);
            watchAstCacheLookup_[normSource] = watchAstCacheList_.begin();
        }

        // ---- 2. 创建临时 Interpreter + 注入变量（与 evaluateWatchExpression 一致）----
        Interpreter tempInterp;
        auto env = std::make_shared<Environment>();
        if (vmMode) {
            for (const auto& kv : vmStepper_.getGlobals()) {
                env->define(kv.first, kv.second);
            }
            for (const auto& kv : getSelectedFrameLocals()) {
                env->define(kv.first, kv.second);
            }
        } else {
            for (const auto& snap : debugCoord_.getDebugVariableSnapshot()) {
                env->define(snap.name, snap.value);
            }
            if (selectedFrame_ >= 0) {
                for (const auto& kv : getSelectedFrameLocals()) {
                    env->define(kv.first, kv.second);
                }
            }
        }
        // this 绑定
        auto& vars = env->localVariables();
        auto thisIt = vars.find("this");
        if (thisIt != vars.end() && thisIt->second.isInstance()) {
            env->bindInstance(const_cast<Value*>(&thisIt->second));
        }
        tempInterp.setGlobalEnvironment(env);

        // ---- R161 阶段 2：注入函数/类/枚举注册表 ----
        // 从主 Interpreter 复制注册表，使调试器 REPL 中可调用用户定义的
        // 函数/类/枚举。shared_ptr<FunDecl> 共享所有权，AST 在主 Interpreter
        // 析构前有效（主 Interpreter 由 IdeController 持有，生命期长于 tempInterp）。
        //
        // 线程安全：Interpreter 调试暂停期 worker 阻塞在 pauseCV_，
        // VM 暂停期主 Interpreter 空闲，复制注册表安全。
        //
        // 边界：主 Interpreter 未运行过（如直接用 VM 调试从未 Run Interpreter）
        // 时 hasRegistries()=false，跳过注入，REPL 仅能访问变量不能调用函数。
        if (interpreter_ && interpreter_->hasRegistries()) {
            tempInterp.injectRegistriesFrom(*interpreter_);
        }

        // ---- 3. 捕获 print 输出 ----
        std::string capturedOutput;
        if (output) {
            tempInterp.setOutputCallback([&capturedOutput](const std::string& s) {
                capturedOutput += s;
                capturedOutput += '\n';
            });
        }

        // ---- 4. 用 executeRepl 执行整个 Block（多条语句，不做沙箱恢复）----
        // 临时 Interpreter 是独立实例，赋值/var 声明的副作用仅影响 tempInterp 的 env，
        // 不写回主程序的 VM/Interpreter 状态（阶段1只读语义）。
        Value v = tempInterp.executeRepl(*ast);
        result.ok = true;
        result.value = v;
        result.typeName = v.typeName();
        result.valueRepr = v.toString();
        if (output) {
            *output = capturedOutput;
        }
        return result;
    } catch (const RuntimeError& e) {
        result.error = std::string("运行时错误: ") + e.what();
        return result;
    } catch (const std::exception& e) {
        result.error = std::string("求值异常: ") + e.what();
        LOG_WARNING("调试器 REPL 求值异常: " + result.error + "（源码: " + source + "）", "IdeController");
        return result;
    } catch (...) {
        result.error = "求值异常: 未知错误";
        return result;
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
        // P2-11: 内建标准库模块拦截（std/math, std/string, std/list）
        if (BuiltinModuleRegistry::isBuiltinModule(modulePath)) {
            return BuiltinModuleRegistry::getSource(modulePath);
        }
        QString resolved = resolveModulePath(modulePath);
        if (resolved.isEmpty())
            return "";
        QFile file(resolved);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            // P3.4 fix: 区分"文件不存在"与"空文件"——空文件返回 "\n"（合法空模块）
            std::string content = QString::fromUtf8(file.readAll()).toStdString();
            return content.empty() ? "\n" : content;
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
