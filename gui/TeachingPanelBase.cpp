// ============================================================
// TeachingPanelBase.cpp — 教学面板公共基类实现（见 .h 迁移路径说明）
// ============================================================

#include "gui/TeachingPanelBase.h"

#include "Theme.h" // QFluentKit（onThemeModeChanged 信号）

#include <QVBoxLayout>

TeachingPanelBase::TeachingPanelBase(QWidget* parent) : QWidget(parent) {
    // 注意：构造函数不创建根布局——子类构造函数体在基类之后执行，若子类
    // 自行 new QVBoxLayout(this)，此处抢先创建会使其被 Qt 拒绝。根布局
    // 延迟到子类首次调用 rootLayout() 时惰性创建/采纳。

    // 主题切换 → applyTheme() 虚函数分发。receiver=this 保证生命周期安全，
    // 析构自动断开；子类不再各自手工接线（原 6 处样板收敛于此）。
    Theme::onThemeModeChanged(this, [this](Fluent::ThemeMode) { applyTheme(); });
}

QVBoxLayout* TeachingPanelBase::rootLayout() {
    if (!rootLayout_) {
        // 采纳子类已自建的顶层布局（历史面板的标准写法）；仅当完全无布局时
        // 才创建标准零边距容器，杜绝同一 widget 上的二次布局创建。
        if (auto* existing = qobject_cast<QVBoxLayout*>(QWidget::layout())) {
            rootLayout_ = existing;
        } else if (QWidget::layout() == nullptr) {
            rootLayout_ = new QVBoxLayout(this);
            rootLayout_->setContentsMargins(0, 0, 0, 0);
            rootLayout_->setSpacing(0);
        }
    }
    return rootLayout_;
}

void TeachingPanelBase::applyTheme() {}
