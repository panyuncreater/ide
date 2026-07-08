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
//   - TeachingTheme::textPrimary()   主文本色
//   - TeachingTheme::textSecondary() 次要文本色
//   - TeachingTheme::textHint()      提示文本色
//   - TeachingTheme::surface()       卡片/面板背景
//   - TeachingTheme::border()        边框色
//
// 使用方式：
//   label->setStyleSheet(QString("color: %1;").arg(TeachingTheme::textSecondary().name()));
//   btn->setStyleSheet(QString("background: %1; color: white;").arg(TeachingTheme::primary().name()));
//
// 依赖：QFluentKit Theme（主题色变更自动响应）
//
// 注：深色主题已移除（2026-07-06），所有颜色固定为亮色配色。
//     保留 Theme::isDark() 调用以保持 API 兼容，但因 setThemeMode 强制 LIGHT，
//     isDark() 永远返回 false，三元分支永远走 light 路径。
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

/// 主文本色：深灰（亮色固定）
inline QColor textPrimary() {
    return QColor(0x00, 0x2b, 0x36);   // Solarized base03
}

/// 次要文本色：中灰
inline QColor textSecondary() {
    return QColor(0x58, 0x6e, 0x75);   // Solarized base01
}

/// 提示文本色：比次要更弱
inline QColor textHint() {
    return QColor(0x65, 0x7b, 0x83);   // Solarized base00
}

/// 卡片/面板背景色：白色
inline QColor surface() {
    return QColor(0xfd, 0xf6, 0xe3);   // Solarized base3
}

/// 卡片悬浮态背景色
inline QColor surfaceHover() {
    return QColor(0xee, 0xe8, 0xd5);   // Solarized base2
}

/// 边框色
inline QColor border() {
    return QColor(0x93, 0xa1, 0xa1);   // Solarized base1
}

/// 强调色（用于标题、链接）
inline QColor accent() {
    return Theme::themeColor();
}

/// 成功色（绿色系）
inline QColor success() {
    return QColor(0x85, 0x99, 0x00);   // Solarized green
}

/// 警告色（橙色系）
inline QColor warning() {
    return QColor(0xb5, 0x89, 0x00);   // Solarized yellow
}

/// 错误色（红色系）
inline QColor error() {
    return QColor(0xdc, 0x32, 0x2f);   // Solarized red
}

/// 信息色（Fluent 语义蓝）
inline QColor info() {
    return QColor("#268BD2");
}

/// 提示色（Fluent 语义灰）
inline QColor hint() {
    return QColor("#657B83");
}

/// 学习路径 5 阶段配色（阶段 0-4，失败/未开始用 5）
/// 用于 LearningPathPanel / WelcomeWizard Step 4 / CodeJourneyInfoPanel 5 阶段图
inline QColor learningStageColor(int stage) {
    switch (stage) {
        case 0: return QColor("#859900");  // 阶段零：绿色（首次接触）
        case 1: return QColor("#B58900");  // 阶段一：黄色（编译前端）
        case 2: return QColor("#268BD2");  // 阶段二：蓝色（执行引擎）
        case 3: return QColor("#6C71C4");  // 阶段三：紫色（深入理解）
        case 4: return QColor("#DC322F");  // 阶段四：红色（实战训练）
        default: return QColor("#839496"); // 未开始/失败：灰色
    }
}

// ============================================================
// IDE 主窗口 14 色变量（applyFluentStyle 集中管理）
// 用于 app/ide.cpp::applyFluentStyle() 的 ADS QSS / 文件树 / 状态栏等
// 亮色固定配色（深色主题已移除）
// ============================================================
inline QColor ideBgMain()      { return QColor("#FDF6E3"); }
inline QColor ideBgPanel()     { return QColor("#EEE8D5"); }
inline QColor ideBgSidebar()   { return QColor("#EEE8D5"); }
inline QColor ideFgPrimary()   { return QColor("#002B36"); }
inline QColor ideFgSecondary() { return QColor("#657B83"); }
inline QColor ideBorder()      { return QColor("#93A1A1"); }
inline QColor ideAccent()      { return QColor("#268BD2"); }
inline QColor ideHoverBg()     { return QColor("#EEE8D5"); }
inline QColor ideSelectedBg()  { return QColor("#EEE8D5"); }
inline QColor ideTitleBg()     { return QColor("#FDF6E3"); }
inline QColor ideStatusBg()    { return QColor("#f3f3f3"); }
inline QColor ideEditorBg()    { return QColor("#FDF6E3"); }
inline QColor ideLineNumBg()   { return QColor("#EEE8D5"); }
inline QColor ideLineNumFg()   { return QColor("#657B83"); }

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
