#pragma once

// ============================================================
// TeachingPanelHeader.h — 教学面板统一标题栏组件
// ------------------------------------------------------------
// 解决问题：教学面板无说明、无引导，新手点进来不知道是什么。
// 每个教学面板顶部使用此组件：
//   [标题]  [这是什么？]  [跳转学习路径]
//
// 「这是什么？」按钮弹出对话框，包含：
//   - 面板用途（1-2 句话）
//   - 推荐使用顺序
//   - 关联概念
//
// 「跳转学习路径」按钮发 learningPathRequested 信号，由 Ide slot 路由
// ============================================================

#include <QWidget>
#include <QString>

class QPushButton;
class StrongBodyLabel;   // QFluentKit（titleLabel_ 实际类型）

class TeachingPanelHeader : public QWidget {
    Q_OBJECT
public:
    /// 构造：panelId 用于查找帮助文案，title 为显示标题
    explicit TeachingPanelHeader(const QString& panelId,
                                  const QString& title,
                                  QWidget* parent = nullptr);

    /// 设置标题文本
    void setTitle(const QString& title);

    /// 设置是否显示「跳转学习路径」按钮（默认显示）
    void setShowLearningPathButton(bool show);

signals:
    /// 用户点击「跳转学习路径」按钮
    void learningPathRequested();

private:
    QString panelId_;
    QString title_;
    StrongBodyLabel* titleLabel_ = nullptr;
    QPushButton* helpBtn_ = nullptr;
    QPushButton* learningPathBtn_ = nullptr;

    void showHelpDialog();
};
