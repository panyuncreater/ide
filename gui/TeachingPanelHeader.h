#pragma once

// ============================================================
// TeachingPanelHeader.h — 教学面板统一标题栏组件
// ------------------------------------------------------------
// 解决问题：教学面板无说明、无引导，新手点进来不知道是什么。
// 每个教学面板顶部使用此组件：
//   [标题]  [这是什么？]  [新手引导]  [学习路径]  [返回编辑器]
//
// 「这是什么？」按钮弹出对话框，包含：
//   - 面板用途（1-2 句话）
//   - 推荐使用顺序
//   - 关联概念
//
// 「跳转学习路径」按钮发 learningPathRequested 信号，由 Ide slot 路由
// 「返回编辑器」按钮发 returnToEditorRequested 信号，切回代码编辑区
// ============================================================

#include <QPointer>
#include <QString>
#include <QWidget>

class QPushButton;
class StrongBodyLabel; // QFluentKit（titleLabel_ 实际类型）
class QDialog;         // showHelpDialog / adjustHelpDialogSize 用到
class QTextBrowser;    // adjustHelpDialogSize 形参类型

class TeachingPanelHeader : public QWidget {
    Q_OBJECT
public:
    /// 构造：panelId 用于查找帮助文案，title 为显示标题
    explicit TeachingPanelHeader(const QString& panelId, const QString& title, QWidget* parent = nullptr);

    /// 设置标题文本
    void setTitle(const QString& title);

    /// 设置是否显示「跳转学习路径」按钮（默认显示）
    void setShowLearningPathButton(bool show);

    // UX-R fix: 对外公开帮助文案查询，供 Ide 的通用兜底引导（createGenericPanelTour）
    // 复用同一份文案数据源，避免帮助文档与引导内容两头维护。
    struct HelpDocView {
        QString purpose;          // 面板用途（Markdown 子集）
        QString recommendedOrder; // 推荐使用顺序（有序列表文本）
        QString relatedConcepts;  // 关联概念
    };

    /// 按 panelId 查询帮助文案；未收录返回 false（out 不变）
    static bool helpDocFor(const QString& panelId, HelpDocView* out);

    /// 「这是什么？」按钮（通用引导的锚点 widget，可能为 nullptr）
    QWidget* helpButton() const;

    /// 「学习路径」按钮（通用引导的锚点 widget，可能为 nullptr）
    QWidget* learningPathButton() const;

signals:
    /// 用户点击「跳转学习路径」按钮
    void learningPathRequested();

    /// 用户点击「新手引导」按钮，panelId 标识发起的面板
    void guidedTourRequested(const QString& panelId);

    /// 用户点击「返回编辑器」按钮，切回代码编辑区
    void returnToEditorRequested();

private:
    QString panelId_;
    QString title_;
    StrongBodyLabel* titleLabel_ = nullptr;
    QPushButton* helpBtn_ = nullptr;
    QPushButton* tourBtn_ = nullptr; // 「新手引导」按钮
    QPushButton* learningPathBtn_ = nullptr;
    QPushButton* backBtn_ = nullptr; // 「返回编辑器」按钮
    // 非模态帮助对话框（复用，避免重复创建）
    QPointer<QDialog> helpDlg_;

    void showHelpDialog();

    /// 根据 QTextBrowser 文档实际高度调整对话框高度，
    /// 钳制在 [minimum, maximum] 区间内，宽度保持当前值不变。
    void adjustHelpDialogSize(QDialog* dlg, QTextBrowser* browser);
};
