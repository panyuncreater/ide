// ============================================================
// TeachingTheme.h — 教学面板统一主题色板
// ------------------------------------------------------------
// 解决问题：教学相关组件散布硬编码颜色（#0078d4 / #5a5a5a / #666 等），
// 暗色主题下对比度不足、与 QFluentKit 主题色脱节。
//
// 提供：
//   - TeachingTheme::primary()    主色（跟随 Fluent Theme::themeColor）
//   - TeachingTheme::primaryHover() hover 态
//   - TeachingTheme::primaryPressed() pressed 态
//   - TeachingTheme::textPrimary()   主文本色（暗色自适应）
//   - TeachingTheme::textSecondary() 次要文本色
//   - TeachingTheme::textHint()      提示文本色
//   - TeachingTheme::surface()       卡片/面板背景
//   - TeachingTheme::border()        边框色
//
// 使用方式：
//   label->setStyleSheet(QString("color: %1;").arg(TeachingTheme::textSecondary().name()));
//   btn->setStyleSheet(QString("background: %1; color: white;").arg(TeachingTheme::primary().name()));
//
// 依赖：QFluentKit Theme（主题模式切换 + 主题色变更自动响应）
// ============================================================

#pragma once

#include <QColor>
#include <QString>
#include "Theme.h"  // QFluentKit

namespace TeachingTheme {

/// 主色：跟随 Fluent 主题色（用户可在设置中切换）
inline QColor primary() {
    return Theme::themeColor();
}

/// 主色 hover 态：主色加深 12%
inline QColor primaryHover() {
    QColor c = Theme::themeColor();
    return c.darker(112);
}

/// 主色 pressed 态：主色加深 22%
inline QColor primaryPressed() {
    QColor c = Theme::themeColor();
    return c.darker(122);
}

/// 主文本色：暗色主题白字，亮色主题深灰
inline QColor textPrimary() {
    return Theme::isDark() ? QColor(255, 255, 255) : QColor(32, 32, 32);
}

/// 次要文本色：暗色浅灰，亮色中灰
inline QColor textSecondary() {
    return Theme::isDark() ? QColor(180, 180, 180) : QColor(90, 90, 90);
}

/// 提示文本色：比次要更弱
inline QColor textHint() {
    return Theme::isDark() ? QColor(150, 150, 150) : QColor(120, 120, 120);
}

/// 卡片/面板背景色：暗色深灰，亮色白
inline QColor surface() {
    return Theme::isDark() ? QColor(45, 45, 45) : QColor(255, 255, 255);
}

/// 卡片悬浮态背景色
inline QColor surfaceHover() {
    return Theme::isDark() ? QColor(56, 56, 56) : QColor(249, 249, 249);
}

/// 边框色
inline QColor border() {
    return Theme::isDark() ? QColor(64, 64, 64) : QColor(234, 234, 234);
}

/// 强调色（用于标题、链接）
inline QColor accent() {
    return Theme::themeColor();
}

/// 成功色（绿色系）
inline QColor success() {
    return QColor(16, 124, 88);  // 暗亮色通用
}

/// 警告色（橙色系）
inline QColor warning() {
    return QColor(200, 130, 30);
}

/// 错误色（红色系）
inline QColor error() {
    return QColor(196, 49, 75);
}

/// 主按钮样式表（用于 QPushButton 模拟 PrimaryPushButton 视觉）
/// 注：优先使用 QFluentKit PrimaryPushButton；此函数仅用于无法替换的旧代码
inline QString primaryButtonStyle() {
    return QString(
        "QPushButton { background: %1; color: white; border: none;"
        "  border-radius: 5px; padding: 8px 20px; font-size: 13px; }"
        "QPushButton:hover { background: %2; }"
        "QPushButton:pressed { background: %3; }"
        "QPushButton:disabled { background: #888; }"
    ).arg(primary().name(), primaryHover().name(), primaryPressed().name());
}

/// 次按钮样式表（透明背景 + 边框）
inline QString secondaryButtonStyle() {
    return QString(
        "QPushButton { background: transparent; color: %1; border: 1px solid %2;"
        "  border-radius: 5px; padding: 8px 20px; font-size: 13px; }"
        "QPushButton:hover { background: %3; }"
        "QPushButton:pressed { background: %4; }"
    ).arg(textPrimary().name(), border().name(),
          surfaceHover().name(), surfaceHover().darker(110).name());
}

} // namespace TeachingTheme
