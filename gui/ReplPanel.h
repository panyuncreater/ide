#pragma once

#include <QWidget>
#include <QLineEdit>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QStringList>
#include <memory>

class Interpreter;
class Lexer;
class Parser;

// ============================================================
// ReplPanel REPL 交互面板
// ============================================================

/// REPL（Read-Eval-Print Loop）交互面板
class ReplPanel : public QWidget {
    Q_OBJECT

public:
    explicit ReplPanel(QWidget* parent = nullptr);
    ~ReplPanel();

    /// 设置解释器实例
    void setInterpreter(Interpreter* interp);

    /// 追加输出文本
    void appendOutput(const QString& text);

    /// 追加错误文本
    void appendError(const QString& text);

    /// 清空历史
    void clearHistory();

    /// 启用/禁用输入（Run 期间禁用，防止与 worker 线程并发访问 Interpreter）
    void setInputEnabled(bool enabled);

private slots:
    /// 处理输入
    void onReturnPressed();

private:
    QTextEdit* outputArea_ = nullptr;   // 输出区域
    QLineEdit* inputLine_ = nullptr;    // 输入行
    Interpreter* interpreter_ = nullptr; // 解释器指针（不拥有）

    QStringList history_;               // 命令历史
    int historyIndex_ = -1;             // 历史浏览索引

    QString pendingInput_;              // R4: 多行累积输入缓冲
    bool inContinuation_ = false;       // R4: 是否在续行模式

    /// 执行单行代码
    void executeLine(const QString& line);

    /// R4: 检查输入是否完整（括号/花括号/方括号是否匹配）
    static bool isInputComplete(const QString& input);

    /// 键盘事件过滤（支持上下键浏览历史）
    bool eventFilter(QObject* obj, QEvent* event) override;
};
