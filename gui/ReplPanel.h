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

private slots:
    /// 处理输入
    void onReturnPressed();

private:
    QTextEdit* outputArea_ = nullptr;   // 输出区域
    QLineEdit* inputLine_ = nullptr;    // 输入行
    Interpreter* interpreter_ = nullptr; // 解释器指针（不拥有）

    QStringList history_;               // 命令历史
    int historyIndex_ = -1;             // 历史浏览索引

    /// 执行单行代码
    void executeLine(const QString& line);

    /// 键盘事件过滤（支持上下键浏览历史）
    bool eventFilter(QObject* obj, QEvent* event) override;
};
