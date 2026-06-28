#include "gui/ReplPanel.h"
#include "gui/GuiTextUtils.h"  // P1-12 fix: 共享文本追加逻辑
#include "app/IdeController.h"  // B6 fix: 通过业务层调用 retainReplAst/executeRepl
#include "common/Logger.h"  // 析构超时日志
#include "interpreter/Interpreter.h"  // Value/RuntimeError 类型
#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include <QKeyEvent>
#include <QTextCursor>
#include <QTextCharFormat>
#include <QColor>
#include <future>  // QT-R-06 fix: std::async 异步执行
#include <sstream>

// ============================================================
// ReplPanel REPL 交互面板实现
// ============================================================

ReplPanel::ReplPanel(QWidget* parent)
    : QWidget(parent) {

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    // 输出区域
    outputArea_ = new QTextEdit(this);
    outputArea_->setReadOnly(true);
    outputArea_->setPlaceholderText("MiniLang REPL 输出\n输入表达式或语句后按回车执行");
    layout->addWidget(outputArea_);

    // 输入行
    inputLine_ = new QLineEdit(this);
    inputLine_->setPlaceholderText(">>> 输入 MiniLang 代码...");
    layout->addWidget(inputLine_);

    connect(inputLine_, &QLineEdit::returnPressed, this, &ReplPanel::onReturnPressed);

    // 安装事件过滤器以支持历史浏览
    inputLine_->installEventFilter(this);

    // 欢迎信息
    outputArea_->append("MiniLang REPL v1.0");
    outputArea_->append("输入 MiniLang 表达式或语句，按回车执行。");
    outputArea_->append("输入 'help' 查看帮助，输入 'clear' 清空输出。");
    outputArea_->append("");

    // QT-R-06 fix: 用 QTimer 轮询 std::future 状态
    // 替代 QtConcurrent（Qt6::Concurrent 模块未安装）
    pollTimer_ = new QTimer(this);
    pollTimer_->setInterval(50);  // 50ms 轮询间隔，足够响应
    connect(pollTimer_, &QTimer::timeout, this, &ReplPanel::pollReplFuture);
}

ReplPanel::~ReplPanel() {
    // QT-R-06 fix: 析构时等待异步任务完成，避免悬垂访问
    if (pollTimer_) pollTimer_->stop();
    // 注：std::async(std::launch::async, ...) 返回的 future 析构会阻塞至任务完成
    // （C++ 标准保证），无法真正"超时放弃"——之前的 std::move 到局部变量的写法
    // 仅把阻塞点从成员析构推迟到局部变量析构，5 秒超时形同虚设。
    // 此处显式 wait()，由 Interpreter 的 MAX_LOOP_ITERATIONS (10M) 保护正常程序
    // 不会无限循环；极端死循环场景下进程退出时由 OS 兜底回收。
    //
    // P0-4 fix: 原注释声称"IdeController 在 ReplPanel 之后析构"是错误的——
    // controller_ 是裸指针，由 QObject parent 机制管理（new IdeController(this)），
    // Qt 子对象析构顺序与声明顺序无关，取决于 parent 的 children 列表删除顺序。
    // 但 replFuture_.wait() 在 ReplPanel 析构时阻塞，确保异步任务在 controller_
    // 析构前完成，故异步任务内对 ctrl 的访问是安全的（任务已结束，不再访问 ctrl）。
    // Ide::closeEvent 中的 forceStop 已确保 worker 停止，REPL 任务由
    // MAX_LOOP_ITERATIONS 兜底，~ReplPanel 的 wait() 是最后一道防线。
    if (replFuture_.valid()) {
        replFuture_.wait();
    }
}

void ReplPanel::setController(IdeController* controller) {
    controller_ = controller;
}

void ReplPanel::appendOutput(const QString& text) {
    // P1-12 fix: 委托给 GuiTextUtils::appendLine
    GuiTextUtils::appendLine(outputArea_, text);
}

void ReplPanel::appendError(const QString& text) {
    // P1-12 fix: 委托给 GuiTextUtils::appendLine，使用红色格式
    QTextCharFormat fmt;
    fmt.setForeground(Qt::red);
    GuiTextUtils::appendLine(outputArea_, text, &fmt);
}

void ReplPanel::clearHistory() {
    outputArea_->clear();
    history_.clear();
    historyIndex_ = -1;
}

void ReplPanel::setInputEnabled(bool enabled) {
    inputLine_->setEnabled(enabled);
    if (enabled) {
        inputLine_->setPlaceholderText(QString());
    } else {
        inputLine_->setPlaceholderText(QStringLiteral("程序运行中，REPL 已暂停"));
    }
}

void ReplPanel::onReturnPressed() {
    QString line = inputLine_->text();
    QString trimmedLine = line.trimmed();

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
        history_.push_back(trimmedLine);
        historyIndex_ = history_.size();
    }

    // 显示输入（使用纯文本+颜色格式，避免HTML注入）
    {
        QTextCursor cursor(outputArea_->document());
        cursor.movePosition(QTextCursor::End);
        cursor.insertText("\n");
        QTextCharFormat fmt;
        fmt.setForeground(QColor("#006600"));
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
            outputArea_->append(
                "MiniLang 支持的语法:\n"
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
                "  多行输入: 未闭合的 { ( [ 会自动续行\n"
            );
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
    executeLine(pendingInput_);
    pendingInput_.clear();

    inputLine_->clear();
}

void ReplPanel::executeLine(const QString& line) {
    if (!controller_) {
        appendError("解释器未初始化");
        return;
    }

    // QT-R-06 fix: 若上一次异步执行尚未完成，拒绝新输入
    if (replRunning_) {
        appendError("上一次执行尚未完成，请稍候...");
        return;
    }

    // 确保语句以分号结尾（简单表达式除外）
    std::string source = line.toStdString();

    // 词法分析（主线程，轻量）
    Lexer lexer;
    std::vector<Token> tokens;
    try {
        tokens = lexer.scan(source);
    } catch (const std::exception& e) {
        appendError(QString("词法错误: %1").arg(e.what()));
        return;
    }

    // 检查词法错误
    for (const auto& tok : tokens) {
        if (tok.type == TokenType::TK_ERROR) {
            appendError(QString("词法错误 (行 %1, 列 %2): %3")
                            .arg(tok.line).arg(tok.column)
                            .arg(QString::fromStdString(tok.lexeme)));
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
                        .arg(e.line).arg(e.column).arg(e.what()));
        return;
    }

    if (!ast) return;

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
    IdeController* ctrl = controller_;  // 显式捕获
    std::future<Value> newFuture;
    try {
        newFuture = std::async(std::launch::async, [ctrl, rawAst]() -> Value {
            try {
                return ctrl->executeRepl(*rawAst);
            } catch (const RuntimeError& e) {
                QMetaObject::invokeMethod(ctrl,
                    [ctrl, msg = std::string(e.what()), line = e.line, col = e.column]() {
                        emit ctrl->runtimeError(QString::fromStdString(msg), line, col);
                    }, Qt::QueuedConnection);
                return Value::nullValue();
            } catch (const std::exception& e) {
                QMetaObject::invokeMethod(ctrl,
                    [ctrl, msg = std::string(e.what())]() {
                        emit ctrl->genericError(QString::fromStdString(msg));
                    }, Qt::QueuedConnection);
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

void ReplPanel::pollReplFuture() {
    // QT-R-06 fix: 轮询 std::future 状态，完成则显示结果并恢复输入
    if (!replFuture_.valid()) return;

    // 检查是否完成（非阻塞）
    auto status = replFuture_.wait_for(std::chrono::seconds(0));
    if (status != std::future_status::ready) return;  // 仍在执行

    pollTimer_->stop();
    Value result = replFuture_.get();

    // PANEL-03 fix: null 结果也打印
    appendOutput(QString::fromStdString(result.toString()));

    // 恢复输入
    replRunning_ = false;
    setInputEnabled(true);
    inputLine_->setFocus();
}

bool ReplPanel::isInputComplete(const QString& input) {
    int braceDepth = 0;   // {}
    int parenDepth = 0;   // ()
    int bracketDepth = 0; // []
    bool inString = false;
    bool inBlockComment = false;  // BUG-R2 fix: 跟踪块注释状态
    bool inLineComment = false;
    int tryCount = 0;     // BUG-R1 fix: 跟踪 try/catch 配对
    int catchCount = 0;

    for (int i = 0; i < input.length(); ++i) {
        QChar c = input[i];

        // 处理块注释（跳过内部字符）
        if (inBlockComment) {
            if (c == '*' && i + 1 < input.length() && input[i + 1] == '/') {
                inBlockComment = false;
                ++i;
            }
            continue;
        }

        // 处理行注释（跳过内部字符）
        if (inLineComment) {
            if (c == '\n') {
                inLineComment = false;
            }
            continue;
        }

        // 处理字符串字面量（跳过内部字符）
        if (inString) {
            if (c == '\\' && i + 1 < input.length()) {
                ++i; // 跳过转义字符
                continue;
            }
            if (c == '"') {
                inString = false;
            }
            continue;
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
                inBlockComment = true;
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
            // 仅匹配完整单词 "try" / "catch"，避免匹配 "trying" / "catcher"
            if (len == 3 && input[start] == 't' && input[start + 1] == 'r' && input[start + 2] == 'y') {
                ++tryCount;
            } else if (len == 5 && input[start] == 'c' && input[start + 1] == 'a' &&
                       input[start + 2] == 't' && input[start + 3] == 'c' && input[start + 4] == 'h') {
                ++catchCount;
            }
            --i; // 补偿 for 循环的 ++i
            continue;
        }

        switch (c.toLatin1()) {
        case '{': ++braceDepth; break;
        case '}': --braceDepth; break;
        case '(': ++parenDepth; break;
        case ')': --parenDepth; break;
        case '[': ++bracketDepth; break;
        case ']': --bracketDepth; break;
        }
    }

    // BUG-R2 fix: 未闭合的块注释视为输入不完整
    if (inBlockComment) return false;
    // 未闭合的字符串
    if (inString) return false;
    // 括号不匹配
    if (braceDepth != 0 || parenDepth != 0 || bracketDepth != 0) return false;
    // BUG-R1 fix: try 缺少 catch 视为输入不完整
    if (tryCount > catchCount) return false;

    return true;
}

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
    }
    return QWidget::eventFilter(obj, event);
}
