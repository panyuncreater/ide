#include "gui/ReplPanel.h"
#include "interpreter/Interpreter.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include <QKeyEvent>
#include <QTextCursor>
#include <QTextCharFormat>
#include <QColor>
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
}

ReplPanel::~ReplPanel() {}

void ReplPanel::setInterpreter(Interpreter* interp) {
    interpreter_ = interp;
}

void ReplPanel::appendOutput(const QString& text) {
    QTextCursor cursor(outputArea_->document());
    cursor.movePosition(QTextCursor::End);
    if (!outputArea_->document()->isEmpty()) {
        cursor.insertText("\n");
    }
    // L-新3 fix: 去除尾部换行，避免多余空行（与 OutputPanel PANEL-01 一致）
    QString trimmed = text;
    if (trimmed.endsWith(QChar(10))) trimmed.chop(1);
    cursor.insertText(trimmed);
    outputArea_->setTextCursor(cursor);
    outputArea_->ensureCursorVisible();
}

void ReplPanel::appendError(const QString& text) {
    QTextCursor cursor(outputArea_->document());
    cursor.movePosition(QTextCursor::End);
    if (!outputArea_->document()->isEmpty())
        cursor.insertText("\n");
    QTextCharFormat fmt;
    fmt.setForeground(Qt::red);
    cursor.setCharFormat(fmt);
    cursor.insertText(text);
    cursor.setCharFormat(QTextCharFormat());
    outputArea_->setTextCursor(cursor);
    outputArea_->ensureCursorVisible();
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
        inputLine_->setPlaceholderText(QString::fromUtf8("\xe7\xa8\x8b\xe5\xba\x8f\xe8\xbf\x90\xe8\xa1\x8c\xe4\xb8\xad\xef\xbc\x8cREPL \xe5\xb7\xb2\xe6\x9a\x82\xe5\x81\x9c"));
    }
}

void ReplPanel::onReturnPressed() {
    QString line = inputLine_->text();
    QString trimmedLine = line.trimmed();

    // R4: 续行模式 — 累积输入
    if (inContinuation_) {
        pendingInput_ += "\n" + line;
    } else {
        if (trimmedLine.isEmpty()) return;
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
    if (!interpreter_) {
        appendError("解释器未初始化");
        return;
    }

    // 确保语句以分号结尾（简单表达式除外）
    std::string source = line.toStdString();

    // 词法分析
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

    // 语法分析
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

    // 执行
    try {
        // PANEL-02 fix: 先保留 AST 再执行，确保异常时 classRegistry_/闭包 body 指针不悬空
        Block* rawAst = ast.get();
        interpreter_->retainReplAst(std::move(ast));
        Value result = interpreter_->executeRepl(*rawAst);
        // 显示结果
        // PANEL-03 fix: null 结果也打印
        appendOutput(QString::fromStdString(result.toString()));
    } catch (const RuntimeError& e) {
        appendError(QString("运行时错误 (行 %1, 列 %2): %3")
                        .arg(e.line).arg(e.column).arg(e.what()));
    } catch (const std::exception& e) {
        appendError(QString("错误: %1").arg(e.what()));
    }
}

bool ReplPanel::isInputComplete(const QString& input) {
    int braceDepth = 0;   // {}
    int parenDepth = 0;   // ()
    int bracketDepth = 0; // []
    bool inString = false;

    for (int i = 0; i < input.length(); ++i) {
        QChar c = input[i];

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

        // 跳过行注释
        if (c == '/' && i + 1 < input.length() && input[i + 1] == '/') {
            // 跳到行尾
            while (i < input.length() && input[i] != '\n') ++i;
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

    return braceDepth == 0 && parenDepth == 0 && bracketDepth == 0;
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
