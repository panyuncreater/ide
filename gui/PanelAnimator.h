#pragma once

// ============================================================
// PanelAnimator — 面板弹出/关闭动画工具
// ------------------------------------------------------------
// 150ms 平滑淡入+滑动动画，对齐 VS Code 面板动效节奏。
// 使用 QPropertyAnimation 驱动 maximumHeight + windowOpacity。
// ============================================================

#include <QPropertyAnimation>
#include <QParallelAnimationGroup>
#include <QEasingCurve>
#include <QGraphicsOpacityEffect>
#include <QWidget>

namespace PanelAnimator {

/// 动画持续时间（毫秒），对齐 VS Code 面板节奏
constexpr int DURATION_MS = 150;

/// 子页面切换淡入持续时间（毫秒），略长于面板弹出节奏，过渡更柔和
constexpr int FADE_DURATION_MS = 220;

/// 滑动展开动画：从 0 高度滑动到 targetHeight，同时淡入
/// 适用于底部面板/右侧面板的弹出
inline void animateShow(QWidget* panel, int targetHeight) {
    if (!panel) return;

    panel->show();
    panel->setMaximumHeight(0);
    panel->adjustSize();

    auto* group = new QParallelAnimationGroup(panel);
    group->setProperty("targetHeight", targetHeight);

    // 高度滑动
    auto* heightAnim = new QPropertyAnimation(panel, "maximumHeight");
    heightAnim->setDuration(DURATION_MS);
    heightAnim->setStartValue(0);
    heightAnim->setEndValue(targetHeight);
    heightAnim->setEasingCurve(QEasingCurve::OutCubic);

    // 透明度淡入（通过 GraphicsEffect 或直接用 windowOpacity 不适用于子控件，
    // 改用 QWidget 的可见性即可，高度动画已足够流畅）
    group->addAnimation(heightAnim);

    // 动画结束后恢复最大高度约束（允许后续手动调整）
    QObject::connect(group, &QParallelAnimationGroup::finished, panel, [panel, targetHeight]() {
        panel->setMaximumHeight(16777215); // QWIDGETSIZE_MAX
    });

    group->start(QAbstractAnimation::DeleteWhenStopped);
}

/// 滑动收起动画：从当前高度滑动到 0，结束后隐藏
/// 适用于底部面板/右侧面板的关闭
inline void animateHide(QWidget* panel, std::function<void()> onFinished = nullptr) {
    if (!panel) return;

    int startHeight = panel->height();
    if (startHeight <= 0) {
        panel->hide();
        if (onFinished) onFinished();
        return;
    }

    auto* heightAnim = new QPropertyAnimation(panel, "maximumHeight");
    heightAnim->setDuration(DURATION_MS);
    heightAnim->setStartValue(startHeight);
    heightAnim->setEndValue(0);
    heightAnim->setEasingCurve(QEasingCurve::InCubic);

    QObject::connect(heightAnim, &QPropertyAnimation::finished, panel, [panel, onFinished]() {
        panel->hide();
        panel->setMaximumHeight(16777215); // 恢复约束
        if (onFinished) onFinished();
    });

    heightAnim->start(QAbstractAnimation::DeleteWhenStopped);
}

/// 宽度展开动画（用于右侧面板水平展开）
inline void animateShowHorizontal(QWidget* panel, int targetWidth) {
    if (!panel) return;

    panel->show();
    panel->setMaximumWidth(0);
    panel->adjustSize();

    auto* widthAnim = new QPropertyAnimation(panel, "maximumWidth");
    widthAnim->setDuration(DURATION_MS);
    widthAnim->setStartValue(0);
    widthAnim->setEndValue(targetWidth);
    widthAnim->setEasingCurve(QEasingCurve::OutCubic);

    QObject::connect(widthAnim, &QPropertyAnimation::finished, panel, [panel]() {
        panel->setMaximumWidth(16777215);
    });

    widthAnim->start(QAbstractAnimation::DeleteWhenStopped);
}

/// 宽度收起动画（用于右侧面板水平关闭）
inline void animateHideHorizontal(QWidget* panel, std::function<void()> onFinished = nullptr) {
    if (!panel) return;

    int startWidth = panel->width();
    if (startWidth <= 0) {
        panel->hide();
        if (onFinished) onFinished();
        return;
    }

    auto* widthAnim = new QPropertyAnimation(panel, "maximumWidth");
    widthAnim->setDuration(DURATION_MS);
    widthAnim->setStartValue(startWidth);
    widthAnim->setEndValue(0);
    widthAnim->setEasingCurve(QEasingCurve::InCubic);

    QObject::connect(widthAnim, &QPropertyAnimation::finished, panel, [panel, onFinished]() {
        panel->hide();
        panel->setMaximumWidth(16777215);
        if (onFinished) onFinished();
    });

    widthAnim->start(QAbstractAnimation::DeleteWhenStopped);
}

/// 子页面切换淡入动画：通过 QGraphicsOpacityEffect 驱动 opacity 0→1
/// 适用于 QStackedWidget 子页切换、列表选中详情刷新等场景
/// 注：QGraphicsOpacityEffect 由 widget parent 自动释放，无需手动管理
inline void fadeInWidget(QWidget* widget, int duration = FADE_DURATION_MS) {
    if (!widget) return;
    auto* effect = qobject_cast<QGraphicsOpacityEffect*>(widget->graphicsEffect());
    if (!effect) {
        effect = new QGraphicsOpacityEffect(widget);
        widget->setGraphicsEffect(effect);
    }
    effect->setOpacity(0.0);
    auto* anim = new QPropertyAnimation(effect, "opacity", widget);
    anim->setDuration(duration);
    anim->setStartValue(0.0);
    anim->setEndValue(1.0);
    anim->setEasingCurve(QEasingCurve::OutCubic);
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

} // namespace PanelAnimator
