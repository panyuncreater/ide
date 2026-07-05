#pragma once

#include <QPlainTextEdit>
#include <QWidget>
#include <QSet>
#include <QMap>
#include <QTextBlock>
#include <QTimer>
#include <string>

// ============================================================
// CodeEditor 代码编辑器
// ============================================================

class QCompleter;
class QStringListModel;

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
    QPlainTextEdit* editor_ = nullptr;
};

/// 代码编辑器：支持行号、断点标记、错误下划线、当前执行行高亮、代码折叠、自动补全
class CodeEditor : public QPlainTextEdit {
    Q_OBJECT

public:
    explicit CodeEditor(QWidget* parent = nullptr);

    /// 行号区域宽度
    int lineNumberAreaWidth();

    /// 设置错误行集合
    void setErrorLines(const QSet<int>& lines);

    /// 设置精确错误范围（行+列+长度）
    struct ErrorRange { int line; int column; int length; };
    void setErrorRanges(const std::vector<ErrorRange>& ranges);

    /// 清除错误行
    void clearErrorLines();

    /// 设置当前执行行
    void setCurrentLine(int line);

    /// 清除当前执行行
    void clearCurrentLine();

    /// 跳转到指定行并居中显示
    void gotoLine(int line);

    /// 获取断点集合
    QSet<int> getBreakpoints() const;

    /// 设置断点集合
    void setBreakpoints(const QSet<int>& breakpoints);

    /// 获取断点条件
    std::string getBreakpointCondition(int line) const;

    /// F8: 切换折叠状态（供 LineNumberArea 调用）
    void toggleFold(int blockNumber);

    /// F9: 设置深色主题（影响行号区域和当前行高亮配色）
    void setDarkTheme(bool dark);

    /// BUG 4.2 fix: 设置查找高亮（独立存储，不覆盖编辑器自身 ExtraSelections）
    void setFindSelections(const QList<QTextEdit::ExtraSelection>& selections);

    /// BUG 4.2 fix: 清除查找高亮
    void clearFindSelections();

    /// F13: 设置补全单词列表（关键字 + 内置函数 + 用户符号）
    void setCompletionWords(const QStringList& words);

signals:
    /// 用户通过右键菜单设置断点条件时发射
    void breakpointConditionRequested(int line, const QString& condition);

protected:
    /// 行号区域重绘时触发
    void resizeEvent(QResizeEvent* event) override;
    /// F13: 键盘事件处理（Ctrl+Space 触发补全、补全弹窗导航）
    void keyPressEvent(QKeyEvent* event) override;
    /// F13: 焦点进入时将 completer 的 widget 设为当前编辑器
    void focusInEvent(QFocusEvent* event) override;
    /// F13: 失去焦点时隐藏补全弹窗
    void focusOutEvent(QFocusEvent* event) override;

private slots:
    void updateLineNumberAreaWidth(int newBlockCount);
    void highlightCurrentLine();
    void updateLineNumberArea(const QRect& rect, int dy);
    /// F13: 插入选中的补全项
    void insertCompletion(const QString& completion);
    /// BUG-CE-2/CE-3 fix: 文档内容变化时调整断点/折叠块号偏移
    void onContentsChange(int position, int charsRemoved, int charsAdded);

private:
    LineNumberArea* lineNumberArea_ = nullptr;
    QSet<int> errorLines_;      // 错误行号
    QSet<int> breakpoints_;     // 断点行号
    QMap<int, std::string> breakpointConditions_;  // 断点条件表达式
    int currentLine_ = -1;      // 当前执行行号
    QList<QTextEdit::ExtraSelection> cachedErrorSelections_;  // 缓存的错误行选择（仅 errorLines_ 变化时重建）
    QList<QTextEdit::ExtraSelection> findSelections_;  // BUG 4.2 fix: 查找高亮（独立存储，不覆盖编辑器自身 selections）

    // F8: 代码折叠相关
    QSet<int> foldedBlocks_;    // 已折叠的块号（blockNumber，从0开始）

    // F9: 主题状态
    bool isDarkTheme_ = false;  // 当前是否深色主题

    // QT-R-08 fix: 光标行高亮防抖定时器，避免快速移动光标时频繁 setExtraSelections
    QTimer* lineHighlightTimer_ = nullptr;

    // F13: 自动补全
    QCompleter* completer_ = nullptr;           // 补全器
    QStringListModel* completionModel_ = nullptr; // 补全单词模型

    // BUG-CE-2 fix: 文档内容变化监听，用于断点行号偏移补偿
    QMetaObject::Connection contentsChangeConn_;
    int lastBlockCount_ = 0;  // 上次文档块数（用于计算 delta）

    /// F13: 获取光标下的单词前缀
    QString textUnderCursor() const;
    /// F13: 触发补全弹窗
    void triggerCompletion();
    /// F13: 更新补全弹窗位置和前缀过滤
    void updateCompletionPopup();

    /// F8: 判断指定块是否可折叠（块以 { 结尾或下一行缩进更深）
    bool isFoldable(const QTextBlock& block) const;

    /// F8: 判断指定块是否在折叠区域内
    bool isFolded(int blockNumber) const;

    /// F8: 获取块的折叠范围（返回结束块号，-1 表示不可折叠）
    int foldEndBlock(const QTextBlock& startBlock) const;

    friend class LineNumberArea;
};
