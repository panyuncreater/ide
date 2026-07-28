#pragma once

#include <QMap>
#include <QPlainTextEdit>
#include <QSet>
#include <QTextBlock>
#include <QTimer>
#include <QWidget>
#include <string>
#include <utility>
#include <vector>

#include "debug/DebugTypes.h"      // R104: BreakpointKind
#include "gui/CodeSnippetEngine.h" // 功能 13：代码模板系统

// ============================================================
// CodeEditor 代码编辑器
// ============================================================

class QCompleter;
class QStringListModel;
class QDialog;
class QListWidget;

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
    struct ErrorRange {
        int line;
        int column;
        int length;
    };
    void setErrorRanges(const std::vector<ErrorRange>& ranges);

    /// 清除错误行
    void clearErrorLines();

    /// 设置当前执行行
    void setCurrentLine(int line);

    /// 清除当前执行行
    void clearCurrentLine();

    /// 跳转到指定行并居中显示
    void gotoLine(int line);

    /// 高亮指定源码行（蓝色背景，区别于调试黄色）。
    /// 用于 IR/字节码面板点击时标记对应源码位置。
    void highlightSourceLine(int line);

    /// 清除源码行高亮
    void clearSourceHighlight();

    /// 获取断点集合
    QSet<int> getBreakpoints() const;

    /// 设置断点集合
    void setBreakpoints(const QSet<int>& breakpoints);

    /// 获取断点条件
    std::string getBreakpointCondition(int line) const;

    /// R104 Logpoint：获取/设置断点类型（Line / Logpoint）
    BreakpointKind getBreakpointKind(int line) const;
    void setBreakpointKind(int line, BreakpointKind kind);

    /// R104 Logpoint：获取/设置日志消息模板（仅 Logpoint 模式下使用）
    std::string getLogpointMessage(int line) const;
    void setLogpointMessage(int line, const std::string& msg);

    /// F8: 切换折叠状态（供 LineNumberArea 调用）
    void toggleFold(int blockNumber);

    /// F9: 设置深色主题（影响行号区域和当前行高亮配色）
    void setDarkTheme(bool dark);

    /// 字号调节：delta 为正放大，为负缩小，范围限制 [8, 32]
    /// 同步更新行号区域宽度（依赖 fontMetrics）
    void changeFontSize(int delta);

    /// 获取当前字号
    int fontSize() const;

    /// BUG 4.2 fix: 设置查找高亮（独立存储，不覆盖编辑器自身 ExtraSelections）
    void setFindSelections(const QList<QTextEdit::ExtraSelection>& selections);

    /// BUG 4.2 fix: 清除查找高亮
    void clearFindSelections();

    /// F13: 设置补全单词列表（关键字 + 内置函数 + 用户符号）
    void setCompletionWords(const QStringList& words);

signals:
    /// 用户通过右键菜单设置断点条件时发射
    void breakpointConditionRequested(int line, const QString& condition);

    /// R104 Logpoint：用户通过对话框修改断点类型 / 日志消息时发射。
    /// @param line 行号
    /// @param kind 新的断点类型（Line / Logpoint）
    /// @param logMessage 日志消息模板（kind==Logpoint 时非空）
    void breakpointKindRequested(int line, BreakpointKind kind, const QString& logMessage);

    /// 右键上下文菜单触发项目特化操作时发射（如 toggleComment / format /
    /// gotoLine / find / replace / toggleBreakpoint / editBreakpointCondition /
    /// runToCursor 等），由上层（Ide）连接后路由到对应处理逻辑
    void contextActionRequested(const QString& action);

    /// 用户通过行号区点击切换断点时发射（AUDIT-P2-CORRECT fix）
    /// 用于调试暂停期间同步断点到 DebugController，原实现仅 mousePressEvent
    /// 修改 breakpoints_ 不发射信号，导致 Interpreter 调试会话期间新增/删除断点
    /// 不同步到 DebugController.breakpoints_，新断点不生效，已删除断点仍触发。
    void breakpointsChanged();

    /// 拓展二期·调试：hover 表达式求值——鼠标悬停在标识符（含 obj.field
    /// 点链）上时发射。上层（Ide）判定调试暂停态后调
    /// IdeController::evaluateWatchExpression 并用 QToolTip 展示结果；
    /// 非暂停态上层直接忽略（不弹提示）。
    void hoverEvaluateRequested(const QString& expression, const QPoint& globalPos);

protected:
    /// 拓展二期·调试：拦截 QEvent::ToolTip 实现 hover 表达式求值
    bool event(QEvent* e) override;
    /// 行号区域重绘时触发
    void resizeEvent(QResizeEvent* event) override;
    /// F13: 键盘事件处理（Ctrl+Space 触发补全、补全弹窗导航）
    void keyPressEvent(QKeyEvent* event) override;
    /// F13: 焦点进入时将 completer 的 widget 设为当前编辑器
    void focusInEvent(QFocusEvent* event) override;
    /// F13: 失去焦点时隐藏补全弹窗
    void focusOutEvent(QFocusEvent* event) override;
    /// 右键上下文菜单：在 QPlainTextEdit 默认菜单（Undo/Redo/Cut/Copy/Paste/
    /// Select All）基础上追加项目特化操作（注释切换 / 格式化 / 跳行 / 查找替换 /
    /// 断点 / 运行到光标），通过 contextActionRequested 信号交由上层执行
    void contextMenuEvent(QContextMenuEvent* event) override;

private slots:
    void updateLineNumberAreaWidth(int newBlockCount);
    void highlightCurrentLine();
    void updateLineNumberArea(const QRect& rect, int dy);
    /// F13: 插入选中的补全项
    void insertCompletion(const QString& completion);
    /// BUG-CE-2/CE-3 fix: 文档内容变化时调整断点/折叠块号偏移
    void onContentsChange(int position, int charsRemoved, int charsAdded);
    /// H2: 括号匹配高亮（光标停在 ([{ 时高亮对应 )]}）
    void highlightBracketMatch();

private:
    LineNumberArea* lineNumberArea_ = nullptr;
    QSet<int> errorLines_;                                   // 错误行号
    QSet<int> breakpoints_;                                  // 断点行号
    QMap<int, std::string> breakpointConditions_;            // 断点条件表达式
    QMap<int, BreakpointKind> breakpointKinds_;              // R104: 断点类型（默认 Line）
    QMap<int, std::string> logpointMessages_;                // R104: Logpoint 日志模板
    int currentLine_ = -1;                                   // 当前执行行号
    int sourceHighlightLine_ = -1;                           // IR/字节码面板点击高亮的源码行号
    QList<QTextEdit::ExtraSelection> cachedErrorSelections_; // 缓存的错误行选择（仅 errorLines_ 变化时重建）
    QList<QTextEdit::ExtraSelection> findSelections_; // BUG 4.2 fix: 查找高亮（独立存储，不覆盖编辑器自身 selections）
    QList<QTextEdit::ExtraSelection> bracketSelections_; // H2: 括号匹配高亮（由 highlightCurrentLine 合并）

    // F8: 代码折叠相关
    QSet<int> foldedBlocks_; // 已折叠的块号（blockNumber，从0开始）

    // F9: 主题状态
    bool isDarkTheme_ = false; // 当前是否深色主题

    // QT-R-08 fix: 光标行高亮防抖定时器，避免快速移动光标时频繁 setExtraSelections
    QTimer* lineHighlightTimer_ = nullptr;

    // F13: 自动补全
    QCompleter* completer_ = nullptr;             // 补全器
    QStringListModel* completionModel_ = nullptr; // 补全单词模型

    // BUG-CE-2 fix: 文档内容变化监听，用于断点行号偏移补偿
    QMetaObject::Connection contentsChangeConn_;
    int lastBlockCount_ = 0; // 上次文档块数（用于计算 delta）

    /// F13: 获取光标下的单词前缀
    QString textUnderCursor() const;
    /// 拓展二期·调试：取视口坐标处的 hover 表达式（标识符含左侧 obj. 点链）；
    /// 坐标不在标识符上返回空串
    QString hoverExpressionAt(const QPoint& viewportPos) const;
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

    // ========================================================
    // 功能 13：代码模板 / Snippets 系统
    // ========================================================
    /// 当前展开的占位符绝对位置列表 [start, end)（文档字符偏移）
    std::vector<std::pair<int, int>> currentPlaceholders_;
    int currentPlaceholderIdx_ = -1;   ///< 当前占位符索引，-1 表示未在占位符导航模式
    bool isSnippetNavigating_ = false; ///< 标记程序化光标移动（避免触发"光标移出占位符"清理）

    /// 尝试展开 snippet：检测光标前触发词，匹配则删除触发词并插入展开文本
    /// \return true 表示已展开（事件已消费），false 表示无匹配（事件应继续传播）
    bool tryExpandSnippet();

    /// 选中当前占位符（currentPlaceholders_[currentPlaceholderIdx_]）
    void selectCurrentPlaceholder();

    /// Tab 跳到下一个占位符；已是最后一个则退出占位符模式
    void jumpToNextPlaceholder();

    /// Shift+Tab 跳到上一个占位符；已是第一个则保持不动
    void jumpToPrevPlaceholder();

    /// 清除占位符导航状态
    void clearSnippetState();

    /// 文档内容变化时同步更新占位符范围（基于 position/charsRemoved/charsAdded 增量）
    void updatePlaceholderRanges(int position, int charsRemoved, int charsAdded);

    /// 打开 Ctrl+T 模板列表对话框，选中后插入到光标位置
    void showSnippetListDialog();

    // ========================================================
    // H1/H4: 编辑器增强（缩进/注释切换）
    // ========================================================
    /// H1: 对选中范围每行行首插入或移除 4 空格（addIndent=true 缩进，false 反缩进）
    void indentSelection(QTextCursor& tc, bool addIndent);
    /// H1: 移除光标所在行行首最多 4 空格（或 1 Tab）
    void unindentLine(QTextCursor& tc);
    /// H4: 对选中范围切换 // 注释（行首有 // 则移除，否则插入）
    void toggleCommentSelection(QTextCursor& tc);

    /// H4: 对选中范围切换 /* */ 块注释（选区首尾包裹或去除）
    void toggleBlockComment(QTextCursor& tc);

    // ---- R131 fix: keyPressEvent 314 行拆为 thin dispatcher + 4 helper（每个 < 100 行）----
    /// Snippet 系统键盘交互：Ctrl+T 打开模板列表 / Tab 占位符导航 or 触发词展开 or 缩进 /
    /// Shift+Tab 反向占位符 or 反向缩进 / Escape 退出占位符导航
    /// 返回 true=已处理（不再继续 keyPressEvent 后续逻辑），false=未处理
    bool handleSnippetKeys(QKeyEvent* event);
    /// 编辑动作快捷键：Ctrl+Space/J 触发补全 / Ctrl+/ 行注释切换 / Ctrl+Shift+/ 块注释切换 /
    /// Ctrl+D 选中下一个相同单词 / Ctrl+Shift+K 删除当前行 / Ctrl+Shift+D 复制当前行
    /// 返回 true=已处理，false=未处理
    bool handleEditorActionKeys(QKeyEvent* event);
    /// Alt+Up/Down 移动当前行（或选区）上/下，对齐 VSCode 快捷键
    /// 返回 true=已处理，false=未处理
    bool handleLineMoveKeys(QKeyEvent* event);
    /// 回车自动缩进（无选区时复制上一行缩进，上一行以 { 结尾则加一级 4 空格）
    /// 返回 true=已处理，false=未处理
    bool handleEnterAutoIndent(QKeyEvent* event);

    /// H2 辅助：判断位置 pos 是否在字符串或注释内（避免匹配字符串内的括号）
    /// 通过从文档开头扫描到 pos，统计字符串/注释状态实现
    bool isInsideStringOrComment(int pos) const;

    /// H2 辅助：从 fromPos 开始扫描，找到下一个非字符串/注释内的字符 c 的位置
    /// 返回 -1 表示未找到
    int findCharOutsideStringComment(QChar c, int fromPos, bool forward) const;

    friend class LineNumberArea;
};
