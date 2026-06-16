#pragma once

#include <QPlainTextEdit>
#include <QWidget>
#include <QSet>
#include <QMap>
#include <string>

// ============================================================
// CodeEditor 代码编辑器
// ============================================================

/// 行号区域部件
class LineNumberArea : public QWidget {
    Q_OBJECT

public:
    LineNumberArea(QPlainTextEdit* editor, QWidget* parent = nullptr);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    QPlainTextEdit* editor_;
};

/// 代码编辑器：支持行号、断点标记、错误下划线、当前执行行高亮
class CodeEditor : public QPlainTextEdit {
    Q_OBJECT

public:
    explicit CodeEditor(QWidget* parent = nullptr);

    /// 行号区域宽度
    int lineNumberAreaWidth();

    /// 设置错误行集合
    void setErrorLines(const QSet<int>& lines);

    /// 清除错误行
    void clearErrorLines();

    /// 设置当前执行行
    void setCurrentLine(int line);

    /// 清除当前执行行
    void clearCurrentLine();

    /// 获取断点集合
    QSet<int> getBreakpoints() const;

    /// 设置断点集合
    void setBreakpoints(const QSet<int>& breakpoints);

    /// 获取断点条件
    std::string getBreakpointCondition(int line) const;

signals:
    /// 用户通过右键菜单设置断点条件时发射
    void breakpointConditionRequested(int line, const QString& condition);

protected:
    /// 行号区域重绘时触发
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void updateLineNumberAreaWidth(int newBlockCount);
    void highlightCurrentLine();
    void updateLineNumberArea(const QRect& rect, int dy);

private:
    LineNumberArea* lineNumberArea_;
    QSet<int> errorLines_;      // 错误行号
    QSet<int> breakpoints_;     // 断点行号
    QMap<int, std::string> breakpointConditions_;  // 断点条件表达式
    int currentLine_ = -1;      // 当前执行行号
    QList<QTextEdit::ExtraSelection> cachedErrorSelections_;  // 缓存的错误行选择（仅 errorLines_ 变化时重建）

    friend class LineNumberArea;
};
