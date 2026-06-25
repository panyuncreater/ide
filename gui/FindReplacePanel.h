#pragma once

#include <QWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QCheckBox>
#include <QLabel>

class CodeEditor;

// ============================================================
// FindReplacePanel 查找替换面板
// ============================================================
// 提供查找/替换功能，嵌入在 CodeEditor 上方
// 快捷键: Ctrl+F 显示查找, Ctrl+H 显示替换, F3 查找下一个, Shift+F3 查找上一个
// Esc 关闭面板

class FindReplacePanel : public QWidget {
    Q_OBJECT
public:
    explicit FindReplacePanel(CodeEditor* editor, QWidget* parent = nullptr);

    /// 显示查找模式（仅查找行可见）
    void showFind();
    /// 显示替换模式（查找+替换行可见）
    void showReplace();
    /// 关闭面板并清除高亮
    void closePanel();

protected:
    void keyPressEvent(QKeyEvent* event) override;

private slots:
    void onFindNext();
    void onFindPrev();
    void onReplace();
    void onReplaceAll();
    void onFindTextChanged(const QString& text);

private:
    CodeEditor* editor_ = nullptr;
    QLineEdit* findEdit_ = nullptr;
    QLineEdit* replaceEdit_ = nullptr;
    QPushButton* findNextBtn_ = nullptr;
    QPushButton* findPrevBtn_ = nullptr;
    QPushButton* replaceBtn_ = nullptr;
    QPushButton* replaceAllBtn_ = nullptr;
    QPushButton* closeBtn_ = nullptr;
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
