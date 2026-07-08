/**
 * @file FindReplacePanel.h
 * @brief 查找/替换面板的类声明（编辑器内联查找替换）
 *
 * 依赖 CodeEditor 提供查找/替换能力；以非模态浮层形式呈现。
 */
#pragma once

#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QWidget>

class CodeEditor;

// QFluentKit 前向声明（PushButton / PrimaryPushButton 为全局类，不在 Fluent 命名空间）
class PushButton;
class PrimaryPushButton;

// ============================================================
// FindReplacePanel 查找替换面板
// ============================================================
// 提供查找/替换功能，嵌入在 CodeEditor 上方
// 快捷键: Ctrl+F 显示查找, Ctrl+H 显示替换, F3 查找下一个, Shift+F3 查找上一个
// Esc 关闭面板

class FindReplacePanel : public QWidget {
    Q_OBJECT
public:
    /// 构造查找/替换面板；editor 为关联编辑器。
    explicit FindReplacePanel(CodeEditor* editor, QWidget* parent = nullptr);

    /// 显示查找模式（仅查找行可见）
    void showFind();
    /// 显示替换模式（查找+替换行可见）
    void showReplace();
    /// 关闭面板并清除高亮
    void closePanel();
    // AUDIT-BUG-E5 fix: 公开 onFindNext/onFindPrev 供 Ide 快捷键直接调用
    /// 查找下一个。
    void onFindNext();
    /// 查找上一个。
    void onFindPrev();

protected:
    /// 键盘事件处理（Esc/回车）。
    void keyPressEvent(QKeyEvent* event) override;

private slots:
    /// 执行替换。
    void onReplace();
    /// 全部替换。
    void onReplaceAll();
    /// 查找文本变化回调。
    void onFindTextChanged(const QString& text);

private:
    /// P1-13 fix: showFind/showReplace 共用逻辑
    void showPanel(bool showReplace);

    CodeEditor* editor_ = nullptr;
    QLineEdit* findEdit_ = nullptr;
    QLineEdit* replaceEdit_ = nullptr;
    PushButton* findNextBtn_ = nullptr;
    PushButton* findPrevBtn_ = nullptr;
    PrimaryPushButton* replaceBtn_ = nullptr;
    PrimaryPushButton* replaceAllBtn_ = nullptr;
    PushButton* closeBtn_ = nullptr;
    QCheckBox* caseSensitiveCheck_ = nullptr;
    QCheckBox* wholeWordCheck_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    bool replaceVisible_ = false;

    /// 查找文本，从当前位置向后/向前查找
    /// forward=true 向后查找, false 向前查找
    /// 返回是否找到
    bool findText(bool forward);

    /// 更新高亮所有匹配项
    void highlightMatches(const QString& text);

    /// 清除高亮
    void clearHighlights();
};
