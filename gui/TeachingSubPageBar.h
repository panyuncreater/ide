#pragma once

// ============================================================
// TeachingSubPageBar.h — 教学面板子页切换栏统一组件
// ------------------------------------------------------------
// 解决短板 2.8/2.11/2.16/2.17：第七波面板子页按钮三种不一致实现
//   1. 交互模式统一：QButtonGroup 互斥（替代手动 setChecked(false)）
//   2. 视觉统一：主题色高亮（TeachingTheme::primary）+ objectName
//   3. 动画统一：切换后调用 PanelAnimator::slideInWidget
//   4. 文案统一：①② 前缀格式
//
// 用法：
//   auto* bar = new TeachingSubPageBar(this);
//   bar->addPage("① 原理", theoryPage);
//   bar->addPage("② 模拟器", simulatorPage);
//   layout->addWidget(bar->buttonBar());
//   layout->addWidget(bar->stack(), 1);
//   bar->setCurrentIndex(0);
//   // 持久化子页选择（可选）：
//   bar->restoreFromSettings("teachingPanel/gc-visualizer/subPage");
//   connect(bar, &TeachingSubPageBar::currentChanged, [](int){ ... });
// ============================================================

#include <QHBoxLayout>
#include <QPushButton>
#include <QStackedWidget>
#include <QString>
#include <QVector>
#include <QWidget>

class TeachingSubPageBar : public QWidget {
    Q_OBJECT

public:
    explicit TeachingSubPageBar(QWidget* parent = nullptr);

    /// 添加一个子页（按钮文本 + 页面 widget）。返回子页索引。
    int addPage(const QString& buttonText, QWidget* page);

    /// 按钮栏（横向布局，含 stretch），调用方 addToLayout
    QHBoxLayout* buttonBar() { return buttonBar_; }

    /// 子页堆栈，调用方 addToLayout with stretch
    QStackedWidget* stack() { return stack_; }

    /// 当前子页索引
    int currentIndex() const { return stack_->currentIndex(); }

    /// 设置当前子页（触发动画与信号）
    void setCurrentIndex(int idx);

    /// 子页数量
    int count() const { return stack_->count(); }

    /// 获取第 idx 个按钮（供 GuidedTour 定位 targetWidget）
    QPushButton* buttonAt(int idx) const;

    /// 从 QSettings 恢复上次子页选择（key 形如 "teachingPanel/gc-visualizer/subPage"）
    void restoreFromSettings(const QString& key);

    /// 保存当前子页选择到 QSettings
    void saveToSettings(const QString& key) const;

signals:
    /// 子页切换信号（切换动画完成后发射）
    void currentChanged(int idx);

private:
    QHBoxLayout* buttonBar_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QVector<QPushButton*> buttons_;
    bool suppressAnim_ = false; // 初始恢复时不播放动画

    void updateButtonStyles();
};
