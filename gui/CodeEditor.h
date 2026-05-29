#pragma once

#include <QPlainTextEdit>
#include <QWidget>
#include <QSet>

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
    int currentLine_ = -1;      // 当前执行行号

    friend class LineNumberArea;
};

/// 行号区域点击事件处理：切换断点
/// 鼠标点击行号区域时发射此信号
