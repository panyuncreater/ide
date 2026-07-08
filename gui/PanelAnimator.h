#pragma once

// ============================================================
// PanelAnimator — 面板弹出/关闭动画工具
// ------------------------------------------------------------
// 150ms 平滑淡入+滑动动画，对齐 VS Code 面板动效节奏。
// 使用 QPropertyAnimation 驱动 maximumHeight + windowOpacity。
// ============================================================

#include <QEasingCurve>
#include <QGraphicsOpacityEffect>
#include <QParallelAnimationGroup>
#include <QPropertyAnimation>
#include <QWidget>

namespace PanelAnimator {

/// 动画持续时间（毫秒），对齐 VS Code 面板节奏
constexpr int DURATION_MS = 150;

/// 子页面切换淡入持续时间（毫秒），略长于面板弹出节奏，过渡更柔和
constexpr int FADE_DURATION_MS = 220;

/// 滑动展开动画：从 0 高度滑动到 targetHeight，同时淡入
/// 适用于底部面板/右侧面板的弹出
// AUDIT-P2 fix: 动画去重——快速连续点击时停止旧动画，避免多个 QPropertyAnimation
// 同时写入 maximumHeight 导致抖动、目标高度不正确。
inline void animateShow(QWidget* panel, int targetHeight) {
    if (!panel)
        return;

    // 停止正在运行的 maximumHeight 动画
    const auto anims = panel->findChildren<QPropertyAnimation*>();
    for (auto* a : anims) {
        if (a->propertyName() == "maximumHeight" && a->targetObject() == panel) {
            a->stop();
            a->deleteLater();
        }
    }

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
// AUDIT-P2 fix: 动画去重——与 animateShow 对称
inline void animateHide(QWidget* panel, std::function<void()> onFinished = nullptr) {
    if (!panel)
        return;

    // 停止正在运行的 maximumHeight 动画
    const auto anims = panel->findChildren<QPropertyAnimation*>();
    for (auto* a : anims) {
        if (a->propertyName() == "maximumHeight" && a->targetObject() == panel) {
            a->stop();
            a->deleteLater();
        }
    }

    int startHeight = panel->height();
    if (startHeight <= 0) {
        panel->hide();
        if (onFinished)
            onFinished();
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
        if (onFinished)
            onFinished();
    });

    heightAnim->start(QAbstractAnimation::DeleteWhenStopped);
}

/// 宽度展开动画（用于右侧面板水平展开）
// AUDIT-P2 fix: 动画去重——与 animateShow 对称，针对 maximumWidth
inline void animateShowHorizontal(QWidget* panel, int targetWidth) {
    if (!panel)
        return;

    // 停止正在运行的 maximumWidth 动画
    const auto anims = panel->findChildren<QPropertyAnimation*>();
    for (auto* a : anims) {
        if (a->propertyName() == "maximumWidth" && a->targetObject() == panel) {
            a->stop();
            a->deleteLater();
        }
    }

    panel->show();
    panel->setMaximumWidth(0);
    panel->adjustSize();

    auto* widthAnim = new QPropertyAnimation(panel, "maximumWidth");
    widthAnim->setDuration(DURATION_MS);
    widthAnim->setStartValue(0);
    widthAnim->setEndValue(targetWidth);
    widthAnim->setEasingCurve(QEasingCurve::OutCubic);

    QObject::connect(widthAnim, &QPropertyAnimation::finished, panel, [panel]() { panel->setMaximumWidth(16777215); });

    widthAnim->start(QAbstractAnimation::DeleteWhenStopped);
}

/// 宽度收起动画（用于右侧面板水平关闭）
// AUDIT-P2 fix: 动画去重——与 animateHide 对称，针对 maximumWidth
inline void animateHideHorizontal(QWidget* panel, std::function<void()> onFinished = nullptr) {
    if (!panel)
        return;

    // 停止正在运行的 maximumWidth 动画
    const auto anims = panel->findChildren<QPropertyAnimation*>();
    for (auto* a : anims) {
        if (a->propertyName() == "maximumWidth" && a->targetObject() == panel) {
            a->stop();
            a->deleteLater();
        }
    }

    int startWidth = panel->width();
    if (startWidth <= 0) {
        panel->hide();
        if (onFinished)
            onFinished();
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
        if (onFinished)
            onFinished();
    });

    widthAnim->start(QAbstractAnimation::DeleteWhenStopped);
}

/// 子页面切换淡入动画：通过 QGraphicsOpacityEffect 驱动 opacity 0→1
/// 适用于 QStackedWidget 子页切换、列表选中详情刷新等场景
/// 注：QGraphicsOpacityEffect 由 widget parent 自动释放，无需手动管理
/// 安全改进：先停止 widget 上正在运行的 opacity 动画，避免多动画堆积
/// 导致 opacity 属性被多个 QPropertyAnimation 同时写入（引发抖动/崩溃）
inline void fadeInWidget(QWidget* widget, int duration = FADE_DURATION_MS) {
    if (!widget)
        return;
    auto* effect = qobject_cast<QGraphicsOpacityEffect*>(widget->graphicsEffect());
    if (!effect) {
        effect = new QGraphicsOpacityEffect(widget);
        widget->setGraphicsEffect(effect);
    }
    // 停止该 widget 子树中正在运行的 opacity 动画，防止动画堆积
    const auto anims = widget->findChildren<QPropertyAnimation*>();
    for (auto* a : anims) {
        if (a->propertyName() == "opacity" && a->targetObject() == effect) {
            a->stop();
            a->deleteLater();
        }
    }
    effect->setOpacity(0.0);
    auto* anim = new QPropertyAnimation(effect, "opacity", widget);
    anim->setDuration(duration);
    anim->setStartValue(0.0);
    anim->setEndValue(1.0);
    anim->setEasingCurve(QEasingCurve::OutCubic);
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

/// 轻量子页面滑入动画：通过 QPropertyAnimation 驱动 widget 的 pos 属性
/// 从右侧偏移 24px 滑入到目标位置。相比 fadeInWidget：
///   - 不使用 QGraphicsOpacityEffect，无离屏 pixmap 合成（O(1) vs O(n)）
///   - 不受子 widget 数量影响，适合复杂面板（含滚动区域/列表/表格）的过渡
/// 适用于 QStackedWidget 切页时的内容过渡，与 VS Code 编辑器切换节奏一致。
/// 注：调用方需确保 widget 已被 QStackedWidget 设为 currentWidget，
/// 其 geometry 已由 stacked layout 就位后再调用本函数。
inline void slideInWidget(QWidget* widget, int duration = FADE_DURATION_MS) {
    if (!widget)
        return;
    // 停止该 widget 上正在运行的 pos 动画，防止动画堆积
    const auto anims = widget->findChildren<QPropertyAnimation*>();
    for (auto* a : anims) {
        if (a->propertyName() == "pos" && a->targetObject() == widget) {
            a->stop();
            a->deleteLater();
        }
    }
    const QPoint finalPos = widget->pos();
    const int offset = 24;
    widget->move(finalPos.x() + offset, finalPos.y());
    auto* anim = new QPropertyAnimation(widget, "pos", widget);
    anim->setDuration(duration);
    anim->setStartValue(QPoint(finalPos.x() + offset, finalPos.y()));
    anim->setEndValue(finalPos);
    anim->setEasingCurve(QEasingCurve::OutCubic);
    // 动画结束后确保位置精确归位（防止动画中途被 resize 打断）
    QObject::connect(anim, &QPropertyAnimation::finished, widget, [widget, finalPos]() { widget->move(finalPos); });
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

} // namespace PanelAnimator
