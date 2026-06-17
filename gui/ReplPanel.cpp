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
    cursor.insertText(text);
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

void ReplPanel::onReturnPressed() {
    QString line = inputLine_->text().trimmed();
    if (line.isEmpty()) return;

    // 保存到历史
    history_.push_back(line);
    historyIndex_ = history_.size();

    // 显示输入（使用纯文本+颜色格式，避免HTML注入）
    {
        QTextCursor cursor(outputArea_->document());
        cursor.movePosition(QTextCursor::End);
        cursor.insertText("\n");
        QTextCharFormat fmt;
        fmt.setForeground(QColor("#006600"));
        cursor.setCharFormat(fmt);
        cursor.insertText(">>> " + line);
        cursor.setCharFormat(QTextCharFormat());
        outputArea_->setTextCursor(cursor);
        outputArea_->ensureCursorVisible();
    }

    // 特殊命令
    if (line == "clear") {
        outputArea_->clear();
        inputLine_->clear();
        return;
    }
    if (line == "help") {
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
        );
        inputLine_->clear();
        return;
    }

    // 执行代码
    executeLine(line);

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
        Value result = interpreter_->executeRepl(*ast);
        // 保留 AST 所有权，确保 classRegistry_ 中的方法指针持续有效
        interpreter_->retainReplAst(std::move(ast));
        // 显示结果
        if (!result.isNull()) {
            appendOutput(QString::fromStdString(result.toString()));
        }
    } catch (const RuntimeError& e) {
        appendError(QString("运行时错误 (行 %1, 列 %2): %3")
                        .arg(e.line).arg(e.column).arg(e.what()));
    } catch (const std::exception& e) {
        appendError(QString("错误: %1").arg(e.what()));
    }
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
