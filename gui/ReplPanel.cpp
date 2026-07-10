/**
 * @file ReplPanel.cpp
 * @brief REPL 交互面板实现（功能：Read-Eval-Print Loop）
 *
 * 职责：提供交互式单行/多行输入、异步执行（后台 future 轮询）、
 * 输出与错误分区展示，并支持多行输入完整性判断与历史清空。
 */
#include "gui/ReplPanel.h"
#include "app/IdeController.h"       // B6 fix: 通过业务层调用 retainReplAst/executeRepl
#include "common/Logger.h"           // 析构超时日志
#include "gui/ErrorHintEngine.h"     // 功能 12：错误信息友好化增强
#include "gui/GuiTextUtils.h"        // P1-12 fix: 共享文本追加逻辑
#include "gui/MagicCommands.h"       // 功能 11：REPL %magic 命令
#include "interpreter/Interpreter.h" // Value/RuntimeError 类型
#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include <QColor>
#include <QKeyEvent>
#include <QTextCharFormat>
#include <QTextCursor>
#include <cstdlib> // IDE-CLOSE-01 fix: std::_Exit 强制进程退出
#include <future>  // QT-R-06 fix: std::async 异步执行
#include <sstream>
#include <vector> // AUDIT-REPL-1 fix: isInputComplete 插值栈

// ============================================================
// ReplPanel REPL 交互面板实现
// ============================================================

/// 构造 REPL 面板：初始化输出区、输入框与后台执行状态。
ReplPanel::ReplPanel(QWidget* parent) : QWidget(parent) {

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    // 输出区域
    outputArea_ = new QTextEdit(this);
    outputArea_->setReadOnly(true);
    outputArea_->setPlaceholderText("MiniLang REPL 输出\n输入表达式或语句后按回车执行");
    // BUG-REPL-G1 fix (P1): 设置输出区行数上限，避免长时间运行（如 while 循环
    // 大量 print）导致 QTextDocument 内部块链表无限增长、内存膨胀。
    // 与 OutputPanel 保持一致（10000 行）。
    outputArea_->document()->setMaximumBlockCount(10000);
    layout->addWidget(outputArea_);

    // 输入行
    inputLine_ = new QLineEdit(this);
    inputLine_->setPlaceholderText(">>> 输入代码，%help 查看 magic 命令");
    layout->addWidget(inputLine_);

    connect(inputLine_, &QLineEdit::returnPressed, this, &ReplPanel::onReturnPressed);

    // 安装事件过滤器以支持历史浏览
    inputLine_->installEventFilter(this);

    // 欢迎信息
    outputArea_->append("MiniLang REPL v1.0");
    outputArea_->append("输入 MiniLang 表达式或语句，按回车执行。");
    outputArea_->append("输入 'help' 查看帮助，输入 'clear' 清空输出。");
    outputArea_->append("💡 输入 %help 查看 magic 命令（%ast / %ir / %disassemble / %compare / %profile ...）");
    outputArea_->append("");

    // QT-R-06 fix: 用 QTimer 轮询 std::future 状态
    // 替代 QtConcurrent（Qt6::Concurrent 模块未安装）
    pollTimer_ = new QTimer(this);
    pollTimer_->setInterval(50); // 50ms 轮询间隔，足够响应
    connect(pollTimer_, &QTimer::timeout, this, &ReplPanel::pollReplFuture);
}

ReplPanel::~ReplPanel() {
    if (pollTimer_)
        pollTimer_->stop();
    // 深度防御：closeEvent 路径已通过 waitReplFuture() 处理，此处为 no-op。
    // 非 closeEvent 路径（如直接 delete）controller_ 可能已析构，不能调用
    // requestReplStop()（C++ 异常不能捕获 UAF），直接 wait() 阻塞至完成
    // （由 MAX_LOOP_ITERATIONS 兜底）。
    // BUG-REPL-G5 fix (P2): 原实现直接 wait() 无超时，若 closeEvent 路径异常
    // 未调用 waitReplFuture() 且异步任务陷入死循环（且未触发 MAX_LOOP_ITERATIONS），
    // 析构会无限阻塞。改为 wait_for(5s) + 日志告警，超时后仍回退阻塞 wait()
    // 作为最后手段（进程即将退出，由 OS 兜底）。controller_ 已可能析构，不调用
    // requestReplStop()，仅等待异步任务自然完成或被外部中止。
    if (replFuture_.valid()) {
        auto status = replFuture_.wait_for(std::chrono::seconds(5));
        if (status != std::future_status::ready) {
            // 超时未完成：记录告警后回退阻塞等待（避免 future 析构时 anyway 阻塞）
            LOG_WARNING("REPL 析构等待异步任务超时（5s），回退阻塞等待", "REPL");
            replFuture_.wait();
        }
    }
}

/// 阻塞等待当前 REPL 异步任务结束，避免重复执行。
void ReplPanel::waitReplFuture() {
    if (!replFuture_.valid())
        return;
    // REPL-TIMEOUT fix: 协作中止 + 超时等待，避免 closeEvent 永久阻塞。
    // 原 wait() 无超时，死循环场景下 closeEvent 卡死。现改为：
    // 1. 设置 stopRequested_ 标志（checkBreak 在每个语句节点检查并抛异常）
    // 2. wait_for(5s) 等待协作中止生效
    // 3. 超时则强制进程退出（IDE-CLOSE-01 fix: 原 replFuture_.wait() 无超时阻塞
    //    会导致 closeEvent 永远不返回，IDE 无法正常关闭。改为 std::_Exit(1)
    //    强制进程退出，与 forceStop 的 std::_Exit(0) 语义一致）
    if (controller_) {
        controller_->requestReplStop();
        auto status = replFuture_.wait_for(std::chrono::seconds(5));
        if (status == std::future_status::ready)
            return;
        // AUDIT-BUG-C8 fix: 改用 LOG_* 宏，先检查级别再构造消息（懒求值）。
        LOG_WARNING("REPL 异步任务未在 5 秒内响应中止请求，强制退出进程", "REPL");
    }
    // IDE-CLOSE-01 fix: 超时后强制进程退出，避免 closeEvent 卡死。
    // replFuture_ 析构时若 future 未 ready 会阻塞 wait()，无法安全返回。
    std::_Exit(1);
}

/// 绑定 IDE 控制器以获取执行后端。
void ReplPanel::setController(IdeController* controller) {
    controller_ = controller;
}

/// 向输出区追加普通文本。
void ReplPanel::appendOutput(const QString& text) {
    // P1-12 fix: 委托给 GuiTextUtils::appendLine
    GuiTextUtils::appendLine(outputArea_, text);
}

/// 向输出区追加错误文本（错误样式）。
void ReplPanel::appendError(const QString& text) {
    // P1-12 fix: 委托给 GuiTextUtils::appendLine，使用红色格式
    QTextCharFormat fmt;
    fmt.setForeground(Qt::red);
    GuiTextUtils::appendLine(outputArea_, text, &fmt);
}

/// 清空 REPL 历史与输出。
void ReplPanel::clearHistory() {
    outputArea_->clear();
    history_.clear();
    historyIndex_ = -1;
    // AUDIT-BUG-R1 fix: 重置续行状态，防止 clearHistory 后遗留脏状态
    pendingInput_.clear();
    inContinuation_ = false;
}

/// 启用/禁用输入框（执行中禁用）。
void ReplPanel::setInputEnabled(bool enabled) {
    inputLine_->setEnabled(enabled);
    if (enabled) {
        inputLine_->setPlaceholderText(QString());
    } else {
        inputLine_->setPlaceholderText(QStringLiteral("程序运行中，REPL 已暂停"));
    }
}

/// 回车回调：按输入完整性决定立即执行或继续多行输入。
void ReplPanel::onReturnPressed() {
    // AUDIT-REPL-7 fix: 在任何状态变更前检查异步执行状态。
    // 原实现 executeLine 内部检查后仍清空 pendingInput_，导致续行累积输入丢失。
    // 改为入口拒绝并保留 inputLine_ 内容，用户可调整输入或等待。
    if (replRunning_) {
        appendError("上一次执行尚未完成，请稍候...");
        return;
    }

    QString line = inputLine_->text();
    QString trimmedLine = line.trimmed();

    // AUDIT-REPL-3 fix: 续行模式下空行中止续行（等价 Ctrl+C 中断）。
    // 用户误触续行（如多打 `{`）时无需闭合，直接回车即可丢弃并重启。
    if (inContinuation_ && trimmedLine.isEmpty()) {
        appendOutput("[续行已中止]");
        pendingInput_.clear();
        inContinuation_ = false;
        inputLine_->clear();
        return;
    }

    // R4: 续行模式 — 累积输入
    if (inContinuation_) {
        pendingInput_ += "\n" + line;
    } else {
        // G-P2-8 fix: 空输入时清空输入框（用户可能输入了纯空白），避免残留显示
        if (trimmedLine.isEmpty()) {
            inputLine_->clear();
            return;
        }
        pendingInput_ = trimmedLine;
    }

    // 保存到历史（仅首次行）
    if (!inContinuation_) {
        // AUDIT-P3-ROUND53 fix: 历史去重——若与最后一条相同则不追加，
        // 对齐主流 REPL（Python/Node/bash）行为。避免重复命令多次出现，
        // 按 Up 键需翻越多次才能到达上一条不同命令。
        if (history_.empty() || history_.back() != trimmedLine) {
            history_.push_back(trimmedLine);
        }
        historyIndex_ = history_.size();
    }

    // 显示输入（使用纯文本+颜色格式，避免HTML注入）
    {
        QTextCursor cursor(outputArea_->document());
        cursor.movePosition(QTextCursor::End);
        cursor.insertText("\n");
        QTextCharFormat fmt;
        fmt.setForeground(QColor("#859900"));
        cursor.setCharFormat(fmt);
        QString prompt = inContinuation_ ? "... " : ">>> ";
        cursor.insertText(prompt + (inContinuation_ ? line : trimmedLine));
        cursor.setCharFormat(QTextCharFormat());
        outputArea_->setTextCursor(cursor);
        outputArea_->ensureCursorVisible();
    }

    // PANEL-05 fix: clear 在续行模式下也可退出续行
    if (trimmedLine == "clear") {
        outputArea_->clear();
        pendingInput_.clear();
        inContinuation_ = false;
        inputLine_->clear();
        return;
    }
    // 特殊命令（仅在非续行模式）
    if (!inContinuation_) {
        if (trimmedLine == "help") {
            outputArea_->append("MiniLang 支持的语法:\n"
                                "  var x = 10;          变量声明\n"
                                "  int a = 5;           类型注解变量\n"
                                "  fun f(x) { ... }     函数声明\n"
                                "  if (cond) { ... }    条件语句\n"
                                "  while (cond) { ... } 循环语句\n"
                                "  for (init; cond; upd) { ... }  for循环\n"
                                "  print(expr);         输出\n"
                                "  [1, 2, 3]            数组字面量\n"
                                "  {\"key\": val}        字典字面量\n"
                                "  null                 空值\n"
                                "  class Name { ... }   类声明\n"
                                "  import \"mod\" { f }; 模块导入\n"
                                "  多行输入: 未闭合的 { ( [ 或未闭合字符串会自动续行\n"
                                "  续行中按回车(空行)可中止续行\n"
                                "REPL 行为说明:\n"
                                "  - 表达式语句(如 '1 + 2;')自动求值并打印结果\n"
                                "  - 'clear' 仅清空输出区与续行缓冲,不重置已定义变量/函数/类\n"
                                "  - 'reload \"mod\"' 清除模块缓存,下次 import 重新加载源码\n"
                                "  - 'reload all' 清除所有模块缓存\n"
                                "  - '%reset' 重置 REPL 环境(清除所有变量/函数/类/模块缓存)\n"
                                "─── Magic 命令 ─────────────────────────────\n"
                                "输入 %help 查看完整 magic 命令列表与详细说明。\n"
                                "常用 magic 命令速查:\n"
                                "  %ast <expr>       查看 AST\n"
                                "  %ir <expr>        查看 IR\n"
                                "  %disassemble      反汇编当前字节码\n"
                                "  %compare <expr>   三后端对比\n"
                                "  %profile <expr>   性能剖析\n"
                                "  %memory           内存模型\n"
                                "  %tokens <expr>    词法分析结果\n"
                                "  %reset            重置 REPL 环境\n"
                                "  %version          查看 MiniLang 版本\n"
                                "────────────────────────────────────────────\n");
            pendingInput_.clear();
            inputLine_->clear();
            return;
        }
        // 模块缓存刷新命令：reload "mod" 或 reload all
        // 场景：用户修改了模块源文件后，需在 REPL 中获取最新版本
        // AUDIT-BUG-F2 fix: 用精确匹配 + 空格前缀，避免误匹配 reloadable/reloadX 等标识符，
        // 原 startsWith("reload") 会命中这些标识符并清空用户输入。
        if (trimmedLine == "reload" || trimmedLine.startsWith("reload ")) {
            // BUG-REPL-G2 fix (P2): reload 命令绕过 isRunning() 检查。
            // reload 会清空模块缓存，若 worker 线程正在执行 import，缓存失效会导致
            // worker 后续访问已加载模块时崩溃或行为不一致。executeLine 入口虽已检查
            // isRunning()，但 reload 在 onReturnPressed 中提前 return，绕过该检查。
            // 此处显式拦截，与 executeLine 的检查语义一致。
            if (controller_ && controller_->isRunning()) {
                appendError("程序正在运行，请先停止后再使用 reload");
                pendingInput_.clear();
                inputLine_->clear();
                return;
            }
            QString arg = trimmedLine == "reload" ? QString() : trimmedLine.mid(7).trimmed();
            if (arg.isEmpty()) {
                appendError("用法: reload \"模块路径\" 或 reload all");
            } else if (arg == "all") {
                controller_->clearAllModuleCache();
                appendOutput("[已清除所有模块缓存，下次 import 将重新加载源码]");
            } else if (arg.startsWith("\"") && arg.endsWith("\"") && arg.length() >= 2) {
                std::string path = arg.mid(1, arg.length() - 2).toStdString();
                controller_->clearModuleCache(path);
                appendOutput(QString::fromStdString("[已清除模块 " + path + " 缓存，下次 import 将重新加载]"));
            } else {
                appendError("用法: reload \"模块路径\" 或 reload all");
            }
            pendingInput_.clear();
            inputLine_->clear();
            return;
        }
        // ROUND-69 P1-1 fix: magic 命令在 isInputComplete 之前拦截。
        // 原代码在 executeLine 中检测 % 前缀，但 isInputComplete() 先被调用，
        // "%ast fun f() {" 因 { 未闭合被误判为"不完整"进入续行模式，用户被迫
        // 输入 } 闭合后才能触发 magic 命令。magic 命令是单行命令，不应走续行逻辑。
        if (trimmedLine.startsWith('%')) {
            if (replRunning_) {
                appendError("上一次执行尚未完成，请稍候...");
                pendingInput_.clear();
                inputLine_->clear();
                return;
            }
            if (controller_ && controller_->isRunning()) {
                appendError("程序正在运行，请先停止后再使用 REPL magic 命令");
                pendingInput_.clear();
                inputLine_->clear();
                return;
            }
            if (controller_ && controller_->isVmRunning()) {
                appendError("VM RUN 模式正在执行，请先停止 VM 再使用 REPL magic 命令");
                pendingInput_.clear();
                inputLine_->clear();
                return;
            }
            std::string result = MagicCommands::handle(trimmedLine.toStdString(), controller_);
            if (!result.empty()) {
                appendOutput(QString::fromStdString(result));
            }
            pendingInput_.clear();
            inputLine_->clear();
            return;
        }
    }

    // R4: 检查输入是否完整
    if (!isInputComplete(pendingInput_)) {
        inContinuation_ = true;
        inputLine_->clear();
        return;
    }

    // 输入完整 — 执行并重置续行状态
    inContinuation_ = false;
    // AUDIT-P2-ROUND53 fix: 多行输入历史仅保存首行，Up 键无法恢复完整多行输入。
    // 执行前用完整 pendingInput_（含 \n 拼接的续行）替换之前存入的首行。
    if (!history_.empty() && historyIndex_ == static_cast<int>(history_.size())) {
        history_.back() = pendingInput_;
    }
    executeLine(pendingInput_);
    pendingInput_.clear();

    inputLine_->clear();
}

/// 提交一行/一段 MiniLang 代码到后端异步执行。
void ReplPanel::executeLine(const QString& line) {
    // ROUND-70 fix: 移除 executeLine 中的 magic 命令检测（死代码）。
    // onReturnPressed 已在 isInputComplete 之前拦截 magic 命令（行 303-329），
    // executeLine 永远不会收到以 % 开头的输入。保留检测会让读者误以为
    // executeLine 可能被其他路径直接调用并收到 magic 命令，增加维护负担。

    if (!controller_) {
        appendError("解释器未初始化");
        return;
    }

    // QT-R-06 fix: 若上一次异步执行尚未完成，拒绝新输入
    if (replRunning_) {
        appendError("上一次执行尚未完成，请稍候...");
        return;
    }

    // 2026-06-29 审计修复 R1/R2: 第二道防线——显式检查 worker 是否正在运行。
    // 原互斥是单向的（onRun 检查 isReplRunning，但 ReplPanel 不检查 isRunning），
    // 仅依赖 setInputEnabled(false) 禁用输入框。若 UI 禁用因异常路径未生效，
    // 或未来重构绕过 UI 直接调用 executeLine，会导致 worker 线程与 REPL 异步任务
    // 并发访问同一 Interpreter 的 globalEnv_/classRegistry_ 等共享状态 → 数据竞争。
    // 此处显式检查 isRunning() 作为第二道防线，不依赖 UI 状态。
    if (controller_->isRunning()) {
        appendError("程序正在运行，请先停止后再使用 REPL");
        return;
    }
    // BUG-REPL-AUDIT-14 fix: VM RUN 模式异步执行期间 isRunning() 仅反映 workerMgr 状态，
    // VM RUN 仍可能活跃。prepareRun 已检查 vmStepper_.isRunning() 并拒绝，REPL 路径
    // 需对称检查，避免 VM RUN 期间 REPL 启动并发访问 Compiler/CompileResult 共享状态。
    if (controller_->isVmRunning()) {
        appendError("VM RUN 模式正在执行，请先停止 VM 再使用 REPL");
        return;
    }

    // AUDIT-REPL-5 fix: 移除误导性死代码注释"确保语句以分号结尾（简单表达式除外）"。
    // 实际无任何分号补全逻辑——Parser::expressionStatement 强制 consume(TK_SEMICOLON)，
    // 故裸表达式必须以 ';' 结尾否则报"期望 ';'"。用户需显式输入分号。
    std::string source = line.toStdString();

    // 词法分析（主线程，轻量）
    Lexer lexer;
    std::vector<Token> tokens;
    try {
        tokens = lexer.scan(source);
    } catch (const std::exception& e) {
        appendError(QString("词法错误: %1")
                        .arg(QString::fromStdString(ErrorHintEngine::enrichErrorMessage(e.what(), "parse", {}))));
        return;
    }

    // 检查词法错误
    for (const auto& tok : tokens) {
        if (tok.type == TokenType::TK_ERROR) {
            appendError(QString("词法错误 (行 %1, 列 %2): %3")
                            .arg(tok.line)
                            .arg(tok.column)
                            .arg(QString::fromStdString(ErrorHintEngine::enrichErrorMessage(tok.lexeme, "parse", {}))));
            return;
        }
    }

    // 语法分析（主线程，轻量）
    Parser parser;
    std::unique_ptr<Block> ast;
    try {
        ast = parser.parse(tokens);
    } catch (const ParseError& e) {
        appendError(QString("语法错误 (行 %1, 列 %2): %3")
                        .arg(e.line)
                        .arg(e.column)
                        .arg(QString::fromStdString(ErrorHintEngine::enrichErrorMessage(e.what(), "parse", {}))));
        return;
    }

    if (!ast)
        return;

    // REPL-MAGIC fix: 成功解析后更新管线状态（tokens/AST），使无参 magic 命令
    // （%ast/%tokens）能显示最近 REPL 输入的结果，而非上次 Run(F5) 的陈旧数据。
    // setReplPipelineState 内部静默执行 Lex+Parse（不发 diagnostics 信号），
    // 编译结果清空（REPL 不走字节码编译）。
    controller_->setReplPipelineState(source);

    // QT-R-06 fix: 异步执行解释器，避免主线程阻塞。
    // 词法/语法分析在主线程（轻量，<1ms），解释器执行可能耗时（如 while 循环）放后台。
    // 用 std::async + QTimer 轮询替代 QtConcurrent（Qt6::Concurrent 未安装）。
    // 异常在异步任务中捕获，通过 QMetaObject::invokeMethod 投递到主线程信号链。
    // PANEL-02 fix: 先保留 AST 再执行，确保异常时 classRegistry_/闭包 body 指针不悬空
    Block* rawAst = ast.get();
    controller_->retainReplAst(std::move(ast));

    // 标记执行中，禁用输入
    // Bug fix: std::async 可能在资源耗尽或线程数限制时抛 std::system_error，
    // 此时若已设置 replRunning_=true / 禁用输入但未启动轮询定时器，
    // REPL 将永久卡死（replRunning_ 早期返回守卫阻止后续输入）。
    // 改为：先成功启动 async，再切换状态。AST 已在 replAsts_ 中保留，
    // async 失败时该 AST 不会被使用（executeRepl 未执行），仅造成轻微内存占用。
    IdeController* ctrl = controller_; // 显式捕获
    // AUDIT fix: 重置错误标志，避免上次执行的残余状态影响本次输出判断
    hadReplError_.store(false);
    std::atomic<bool>* errFlag = &hadReplError_;
    // BUG-REPL-G6 fix (P2): 捕获 this 以便将错误同时投递到 REPL 输出区。
    // 原实现仅 emit ctrl->runtimeError/genericError 信号（由 Ide 连接到主错误面板），
    // REPL 用户看不到错误反馈。
    // AUDIT-P1 fix: 原用裸 ReplPanel* self，~ReplPanel 的 wait() 只保证 future 完成，
    // 但 invokeMethod(QueuedConnection) 投递的 lambda 在主线程事件队列中，析构期间
    // 主线程阻塞在 wait() 不会 dispatch，析构后回到事件循环才 dispatch → UAF。
    // 改用 QPointer + qApp 作为 invokeMethod context：qApp 永远存活，lambda 在主线程
    // 执行时检查 self 是否为 null，避免悬垂访问。
    QPointer<ReplPanel> self(this);
    std::future<Value> newFuture;
    try {
        newFuture = std::async(std::launch::async, [ctrl, rawAst, errFlag, self]() -> Value {
            try {
                return ctrl->executeRepl(*rawAst);
            } catch (const RuntimeError& e) {
                errFlag->store(true);
                // BUG-REPL-G6 fix (P2): 同时投递到 REPL 输出区，让用户在 REPL 中
                // 直接看到错误反馈（原有 emit ctrl->runtimeError 仍保留，由 Ide
                // 连接到主错误面板）
                // P0-3 fix (F11): 在 worker 线程中收集 scopeVars（此时 Interpreter
                // 状态仍可访问，主线程在 pollReplFuture 阻塞等待），用于拼写建议
                std::vector<std::string> scopeVars;
                if (ctrl) {
                    scopeVars = ctrl->getReplScopeVariableNames();
                }
                std::string enriched = ErrorHintEngine::enrichErrorMessage(e.what(), "runtime", scopeVars);
                std::string msg = enriched;
                QMetaObject::invokeMethod(
                    qApp,
                    [self, msg]() {
                        if (self)
                            self->appendError(QString::fromStdString(msg));
                    },
                    Qt::QueuedConnection);
                QMetaObject::invokeMethod(
                    ctrl,
                    [ctrl, msg = std::string(e.what()), line = e.line, col = e.column]() {
                        emit ctrl->runtimeError(QString::fromStdString(msg), line, col);
                    },
                    Qt::QueuedConnection);
                return Value::nullValue();
            } catch (const DebugStopException&) {
                // RA-C fix: REPL 中止（closeEvent 超时 / 用户停止）——静默退出，不报错
                return Value::nullValue();
            } catch (const std::exception& e) {
                errFlag->store(true);
                // BUG-REPL-G6 fix (P2): 同理投递到 REPL 输出区
                // P0-3 fix (F11): 通用异常也通过 ErrorHintEngine 增强
                std::vector<std::string> scopeVars;
                if (ctrl) {
                    scopeVars = ctrl->getReplScopeVariableNames();
                }
                std::string enriched = ErrorHintEngine::enrichErrorMessage(e.what(), "runtime", scopeVars);
                std::string msg = enriched;
                QMetaObject::invokeMethod(
                    qApp,
                    [self, msg]() {
                        if (self)
                            self->appendError(QString::fromStdString(msg));
                    },
                    Qt::QueuedConnection);
                QMetaObject::invokeMethod(
                    ctrl,
                    [ctrl, msg = std::string(e.what())]() { emit ctrl->genericError(QString::fromStdString(msg)); },
                    Qt::QueuedConnection);
                return Value::nullValue();
            }
        });
    } catch (const std::exception& e) {
        // async 启动失败：报告错误，REPL 保持可用状态（不切换 replRunning_）
        appendError(QString("无法启动异步执行: %1").arg(e.what()));
        return;
    }
    // async 启动成功，切换状态并启动轮询
    replFuture_ = std::move(newFuture);
    replRunning_ = true;
    setInputEnabled(false);
    pollTimer_->start();
}

/// 轮询异步执行结果，完成后回写输出并恢复输入。
void ReplPanel::pollReplFuture() {
    // QT-R-06 fix: 轮询 std::future 状态，完成则显示结果并恢复输入
    if (!replFuture_.valid())
        return;

    // 检查是否完成（非阻塞）
    auto status = replFuture_.wait_for(std::chrono::seconds(0));
    if (status != std::future_status::ready)
        return; // 仍在执行

    pollTimer_->stop();

    // AUDIT-REPL-8 fix: get() 可能抛未捕获异常（如 std::bad_alloc 不属 std::exception 派生，
    // 或异步 lambda 内漏捕获的异常类型）。外层 try/catch 兜底防止传播到 Qt 事件循环致崩溃。
    Value result;
    try {
        result = replFuture_.get();
    } catch (const std::exception& e) {
        appendError(QString("REPL 执行异常: %1").arg(e.what()));
        replRunning_ = false;
        setInputEnabled(true);
        inputLine_->setFocus();
        return;
    } catch (...) {
        appendError("REPL 执行未知异常");
        replRunning_ = false;
        setInputEnabled(true);
        inputLine_->setFocus();
        return;
    }

    // ROUND-70 fix: 跳过 null 结果的打印。
    // 原行为（PANEL-03 fix）：null 结果也打印，导致 print/var/fun/class 等语句
    // 执行后多输出一行 "null"，与 Python/Node REPL 行为不一致。
    // 改为仅打印非 null 结果——表达式语句（如 1+2;）的结果正常显示，
    // 声明语句（var/fun/class）和 print 语句返回 null 不再产生多余输出。
    // AUDIT fix: 若异步执行已通过信号显示错误，跳过结果输出避免重复显示
    if (!hadReplError_.load() && !result.isNull()) {
        appendOutput(QString::fromStdString(result.toString()));
    }

    // 恢复输入
    replRunning_ = false;
    setInputEnabled(true);
    inputLine_->setFocus();
}

/// 判断当前输入是否为完整可执行的程序（括号/引号配平等）。
bool ReplPanel::isInputComplete(const QString& input) {
    int braceDepth = 0;   // {}
    int parenDepth = 0;   // ()
    int bracketDepth = 0; // []
    bool inString = false;
    int blockCommentDepth = 0; // AUDIT-BUG-R2 fix: 跟踪嵌套块注释深度（与 Lexer 一致）
    bool inLineComment = false;
    int tryCount = 0; // BUG-R1 fix: 跟踪 try/catch 配对
    int catchCount = 0;
    int finallyCount = 0; // P2-C fix: 跟踪 finally 块

    // AUDIT-REPL-1 fix: 字符串插值栈。MiniLang 字符串支持 "...{expr}..." 插值，
    // { 在字符串内开启表达式上下文（可能含嵌套字符串/字典/数组），} 闭合插值回到字符串模式。
    // 栈元素 true = 该 { 由插值开启（闭合时回到字符串模式），false = 普通代码 {。
    // 原 bug: inString 状态下跳过所有字符，不识别 { 的插值语义，导致
    // `var x = "{";` 被判定为完整（实际 { 开启插值，" 开启嵌套字符串，EOF 未闭合）。
    std::vector<bool> interpOpens;

    for (int i = 0; i < input.length(); ++i) {
        QChar c = input[i];

        // 处理块注释（跳过内部字符，支持嵌套）
        if (blockCommentDepth > 0) {
            if (c == '*' && i + 1 < input.length() && input[i + 1] == '/') {
                --blockCommentDepth;
                ++i;
            } else if (c == '/' && i + 1 < input.length() && input[i + 1] == '*') {
                ++blockCommentDepth;
                ++i;
            }
            continue;
        }

        // 处理行注释（跳过内部字符）
        if (inLineComment) {
            // AUDIT-BUG-R3 fix: \r 也标志行注释结束（Windows \r\n / 旧 Mac \r）
            if (c == '\n' || c == '\r') {
                inLineComment = false;
            }
            continue;
        }

        // 处理字符串字面量（跳过内部字符，识别插值 { ）
        if (inString) {
            if (c == '\\' && i + 1 < input.length()) {
                ++i; // 跳过转义字符
                continue;
            }
            if (c == '"') {
                inString = false;
                continue;
            }
            // AUDIT-REPL-1 fix: { 在字符串内开启插值表达式上下文
            if (c == '{') {
                interpOpens.push_back(true); // 标记此 { 为插值开启
                ++braceDepth;
                inString = false; // 退出字符串模式，进入表达式扫描
                continue;
            }
            continue; // 字符串内其他字符跳过
        }

        if (c == '"') {
            inString = true;
            continue;
        }

        // 注释起始检测
        if (c == '/' && i + 1 < input.length()) {
            if (input[i + 1] == '/') {
                inLineComment = true;
                ++i;
                continue;
            }
            if (input[i + 1] == '*') {
                ++blockCommentDepth;
                ++i;
                continue;
            }
        }

        // BUG-R1 fix: 识别 try/catch 关键字以检测缺失的 catch
        if (c.isLetter() || c == '_') {
            int start = i;
            while (i < input.length() && (input[i].isLetterOrNumber() || input[i] == '_')) {
                ++i;
            }
            int len = i - start;
            // 仅匹配完整单词 "try" / "catch" / "finally"，避免匹配 "trying" / "catcher"
            if (len == 3 && input[start] == 't' && input[start + 1] == 'r' && input[start + 2] == 'y') {
                ++tryCount;
            } else if (len == 5 && input[start] == 'c' && input[start + 1] == 'a' && input[start + 2] == 't' &&
                       input[start + 3] == 'c' && input[start + 4] == 'h') {
                ++catchCount;
            } else if (len == 7 && input[start] == 'f' && input[start + 1] == 'i' && input[start + 2] == 'n' &&
                       input[start + 3] == 'a' && input[start + 4] == 'l' && input[start + 5] == 'l' &&
                       input[start + 6] == 'y') {
                ++finallyCount;
            }
            --i; // 补偿 for 循环的 ++i
            continue;
        }

        switch (c.toLatin1()) {
        case '{':
            interpOpens.push_back(false); // 普通代码 {
            ++braceDepth;
            break;
        case '}':
            --braceDepth;
            // BUG-REPL-G3 fix (P2): 负 braceDepth 表示括号不匹配（如多余 }），
            // 视为输入不完整避免误判完整后送入 Parser 触发无法定位的语法错误。
            // 原 isInputComplete 仅在末尾检查 braceDepth!=0，负深度会被末尾的
            // "!= 0" 判断捕获，但中途负深度可能因后续 { 重新归零而漏判
            // （如 "}{" 末尾 braceDepth=0 误判完整）。此处提前返回更准确。
            if (braceDepth < 0)
                return false;
            // AUDIT-REPL-1 fix: 若此 } 闭合的是插值 {，回到字符串模式
            if (!interpOpens.empty()) {
                bool wasInterp = interpOpens.back();
                interpOpens.pop_back();
                if (wasInterp) {
                    inString = true;
                }
            }
            break;
        case '(':
            ++parenDepth;
            break;
        case ')':
            --parenDepth;
            // BUG-REPL-G3 fix (P2): 同理负 parenDepth 视为不完整
            if (parenDepth < 0)
                return false;
            break;
        case '[':
            ++bracketDepth;
            break;
        case ']':
            --bracketDepth;
            // BUG-REPL-G3 fix (P2): 同理负 bracketDepth 视为不完整
            if (bracketDepth < 0)
                return false;
            break;
        }
    }

    // AUDIT-BUG-R2 fix: 未闭合的嵌套块注释视为输入不完整
    if (blockCommentDepth > 0)
        return false;
    // 未闭合的字符串（含插值内未闭合的嵌套字符串）
    if (inString)
        return false;
    // 括号不匹配
    if (braceDepth != 0 || parenDepth != 0 || bracketDepth != 0)
        return false;
    // BUG-R1 fix: try 缺少 catch 或 finally 视为输入不完整
    // P2-C fix: try-finally（无 catch）也是合法结构，catch 或 finally 至少其一即可配对
    if (tryCount > catchCount + finallyCount)
        return false;

    return true;
}

/// 事件过滤器：拦截输入框特殊按键（如上下历史）。
bool ReplPanel::eventFilter(QObject* obj, QEvent* event) {
    if (obj == inputLine_ && event->type() == QEvent::KeyPress) {
        QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Up) {
            // 上一条历史
            if (historyIndex_ > 0) {
                historyIndex_--;
                inputLine_->setText(history_[historyIndex_]);
            }
            return true;
        }
        if (keyEvent->key() == Qt::Key_Down) {
            // 下一条历史
            if (historyIndex_ < static_cast<int>(history_.size()) - 1) {
                historyIndex_++;
                inputLine_->setText(history_[historyIndex_]);
            } else {
                historyIndex_ = history_.size();
                inputLine_->clear();
            }
            return true;
        }
        // R53-UX8 fix: Esc 中止续行。原续行模式下用户只能继续输入完整代码，
        // 无法中止——多行结构（如未闭合的 fun 定义）一旦开始就必须完成。
        // Esc 在续行模式下中止当前 pendingInput_，回到单行模式。
        // 非续行模式下不拦截 Esc（保持 QLineEdit 默认行为，允许清空输入框）。
        if (keyEvent->key() == Qt::Key_Escape && inContinuation_) {
            inContinuation_ = false;
            pendingInput_.clear();
            inputLine_->clear();
            appendOutput(QString::fromUtf8("[续行已中止]"));
            return true;
        }
        // R53-UX8 fix: Ctrl+L 清屏（清空输出区，保留 pendingInput_/history/变量状态）。
        // 与 'clear' 命令行为一致，但快捷键更便捷。运行中拒绝以避免与 worker 输出竞争。
        if ((keyEvent->key() == Qt::Key_L) && (keyEvent->modifiers() & Qt::ControlModifier)) {
            if (controller_ && controller_->isRunning()) {
                appendError("程序正在运行，无法清屏");
                return true;
            }
            outputArea_->clear();
            return true;
        }
    }
    return QWidget::eventFilter(obj, event);
}
