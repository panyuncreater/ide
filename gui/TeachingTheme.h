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
//
// R74 fix (2026-07-10)：背景色从 Solarized 米黄（#FDF6E3/#EEE8D5）回退为
// 中性白/浅灰（#FFFFFF/#F5F5F5），解决跨机器渲染不一致问题。语义强调色
// （success/warning/error/info）保留原 Solarized 配色以保证可读性。
// ============================================================

#pragma once

#include "Theme.h" // QFluentKit
#include <QColor>
#include <QString>

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
    return QColor(0x1e, 0x1e, 0x1e); // 中性深灰（替代 Solarized base03）
}

/// 次要文本色：中灰
inline QColor textSecondary() {
    return QColor(0x5a, 0x5a, 0x5a); // 中性中灰（替代 Solarized base01）
}

/// 提示文本色：比次要更弱
inline QColor textHint() {
    return QColor(0x8c, 0x8c, 0x8c); // 中性浅灰（替代 Solarized base00）
}

/// 卡片/面板背景色：白色
inline QColor surface() {
    return QColor(0xff, 0xff, 0xff); // 纯白（替代 Solarized base3）
}

/// 卡片悬浮态背景色
inline QColor surfaceHover() {
    return QColor(0xf5, 0xf5, 0xf5); // 浅灰（替代 Solarized base2）
}

/// 边框色
inline QColor border() {
    return QColor(0xe0, 0xe0, 0xe0); // 浅灰边框（替代 Solarized base1）
}

/// 强调色（用于标题、链接）
inline QColor accent() {
    return Theme::themeColor();
}

/// 成功色（绿色系）
inline QColor success() {
    return QColor(0x85, 0x99, 0x00); // Solarized green
}

/// 警告色（橙色系）
inline QColor warning() {
    return QColor(0xb5, 0x89, 0x00); // Solarized yellow
}

/// 错误色（红色系）
inline QColor error() {
    return QColor(0xdc, 0x32, 0x2f); // Solarized red
}

/// 信息色（Fluent 语义蓝）
inline QColor info() {
    return QColor("#268BD2");
}

/// 提示色（Fluent 语义灰）
inline QColor hint() {
    return QColor("#8C8C8C"); // R74: 中性浅灰（原 Solarized base00 #657B83）
}

// ============================================================
// Tooltip 配色（修复 Windows 11 黑色 tooltip）
// ------------------------------------------------------------
// 原生 QToolTip 在 Windows 11 上若未显式着色会回退为深色/黑色背景，
// 导致悬浮提示（如调试按钮）文字不可见。以下三色供 QToolTip QSS 使用，
// 保证浅色不透明背景 + 深色文字 + 清晰边框。
// 注：QToolTip 为非半透明顶层弹窗，QSS 不应设置 border-radius，否则
//     Windows 上圆角外区域会渲染成黑色（黑角）。
// ============================================================

/// Tooltip 背景色：近白浅灰（不刺眼且与内容高对比）
inline QColor tooltipBg() {
    return QColor(0xF9, 0xF9, 0xF9); // 与 QFluentKit tool_tip.qss 对齐
}

/// Tooltip 文字色：深灰（在浅背景上清晰可读）
inline QColor tooltipText() {
    return QColor(0x1e, 0x1e, 0x1e);
}

/// Tooltip 边框色：略深于普通边框，增强弹窗轮廓辨识度
inline QColor tooltipBorder() {
    return QColor(0xc8, 0xc8, 0xc8);
}

/// 关闭按钮危险态背景色：Windows 关闭红（与标题栏关闭按钮一致）
inline QColor closeDanger() {
    return QColor(0xE8, 0x11, 0x23);
}

/// 关闭按钮危险态按下背景色：更深的关闭红
inline QColor closeDangerPressed() {
    return QColor(0xC5, 0x0F, 0x1F);
}

// ============================================================
// P3-18 fix: 排查发现的高频缺失语义色补全
// ------------------------------------------------------------
// 2026-07-23 全量排查 gui/ 目录发现 47 个文件、350+ 处硬编码颜色。
// 以下函数补全排查中发现的高频缺失语义色，为后续渐进迁移提供目标。
// 排查报告与迁移优先级见 docs/development.md「硬编码颜色排查」章节。
// ============================================================

/// 弱化文本色：比 textHint 更暗一档的中性灰
/// 排查发现 25+ 处硬编码 #6E6E6E（语义与 textHint 重叠但更暗）
inline QColor textMuted() {
    return QColor(0x6e, 0x6e, 0x6e); // 中性灰（替代散布的 #6E6E6E）
}

/// 主色 hover 背景色：浅蓝（用于按钮/卡片 hover 态背景）
/// 排查发现 6+ 处硬编码 #E5F3FB（TeachingTheme 原仅有 #ECECEC hover 与 #CCE4F7 selected）
inline QColor primaryHoverBg() {
    return QColor(0xE5, 0xF3, 0xFB); // 浅蓝 hover 背景（替代散布的 #E5F3FB）
}

/// 主色上的前景色：白色（用于彩色背景按钮的文字色）
/// 排查发现 30+ 处硬编码 "white"（在 color: white 配合彩色背景的场景）
inline QColor onPrimary() {
    return QColor(0xFF, 0xFF, 0xFF); // 纯白（主色背景上的文字色）
}

/// 主色 disabled 态：中灰（用于禁用按钮背景）
/// 排查发现 primaryButtonStyle() 内部及 WelcomeWizard 等处硬编码 #888
inline QColor primaryDisabled() {
    return QColor(0x88, 0x88, 0x88); // 中灰（禁用态背景，替代散布的 #888）
}

/// 锁定/禁用态背景色：极浅灰
/// 排查发现 5+ 处硬编码 #EDEDED（AstBuilderToy/BugHunt/LabManual/TokenPuzzle/VmStackSandbox 的 [locked='true'] 样式）
inline QColor lockedBg() {
    return QColor(0xED, 0xED, 0xED); // 极浅灰（锁定态背景，替代散布的 #EDEDED）
}

/// 代码块/预格式化文本背景色：浅灰（与 surfaceHover 一致，单独命名以表达语义）
/// 排查发现 10+ 处硬编码 #F5F5F5 用于 <pre> 标签背景
inline QColor codeBlockBg() {
    return QColor(0xF5, 0xF5, 0xF5); // 浅灰（代码块背景，与 surfaceHover 同值但语义独立）
}

/// 学习路径 5 阶段配色（阶段 0-4，失败/未开始用 5）
/// 用于 LearningPathPanel / WelcomeWizard Step 4 / CodeJourneyInfoPanel 5 阶段图
inline QColor learningStageColor(int stage) {
    switch (stage) {
    case 0:
        return QColor("#859900"); // 阶段零：绿色（首次接触）
    case 1:
        return QColor("#B58900"); // 阶段一：黄色（编译前端）
    case 2:
        return QColor("#268BD2"); // 阶段二：蓝色（执行引擎）
    case 3:
        return QColor("#6C71C4"); // 阶段三：紫色（深入理解）
    case 4:
        return QColor("#DC322F"); // 阶段四：红色（实战训练）
    default:
        return QColor("#839496"); // 未开始/失败：灰色
    }
}

// ============================================================
// IDE 主窗口 14 色变量（applyFluentStyle 集中管理）
// 用于 app/ide.cpp::applyFluentStyle() 的 ADS QSS / 文件树 / 状态栏等
// 亮色固定配色（深色主题已移除）
// R74: 背景回退为中性白/浅灰，解决跨机器渲染问题
// ============================================================
inline QColor ideBgMain() {
    return QColor("#FFFFFF"); // 主背景：纯白（原 Solarized base3 #FDF6E3）
}
inline QColor ideBgPanel() {
    return QColor("#F5F5F5"); // 面板背景：浅灰（原 Solarized base2 #EEE8D5）
}
inline QColor ideBgSidebar() {
    return QColor("#F5F5F5"); // 侧栏背景：浅灰（原 Solarized base2 #EEE8D5）
}
inline QColor ideFgPrimary() {
    return QColor("#1E1E1E"); // 主文本：中性深灰（原 Solarized base03 #002B36）
}
inline QColor ideFgSecondary() {
    return QColor("#8C8C8C"); // 次要文本：中性浅灰（原 Solarized base00 #657B83）
}
inline QColor ideBorder() {
    return QColor("#E0E0E0"); // 边框：浅灰（原 Solarized base1 #93A1A1）
}
inline QColor ideAccent() {
    return QColor("#268BD2"); // 强调色：保留 Solarized blue
}
inline QColor ideHoverBg() {
    return QColor("#ECECEC"); // hover：浅灰（原 Solarized base2 #EEE8D5）
}
inline QColor ideSelectedBg() {
    return QColor("#CCE4F7"); // 选中：浅蓝（原 Solarized base2 #EEE8D5）
}
inline QColor ideTitleBg() {
    return QColor("#FFFFFF"); // 标题栏：纯白（原 Solarized base3 #FDF6E3）
}
inline QColor ideStatusBg() {
    return QColor("#F5F5F5"); // 状态栏：浅灰
}
inline QColor ideEditorBg() {
    return QColor("#FFFFFF"); // 编辑器：纯白（原 Solarized base3 #FDF6E3）
}
inline QColor ideLineNumBg() {
    return QColor("#F5F5F5"); // 行号区：浅灰（原 Solarized base2 #EEE8D5）
}
inline QColor ideLineNumFg() {
    return QColor("#8C8C8C"); // 行号字：浅灰（原 Solarized base00 #657B83）
}

/// 主按钮样式表（用于 QPushButton 模拟 PrimaryPushButton 视觉）
/// 注：优先使用 QFluentKit PrimaryPushButton；此函数仅用于无法替换的旧代码
inline QString primaryButtonStyle() {
    // P3-18 fix: 内部硬编码 #888 / white 替换为语义色函数
    return QString("QPushButton { background: %1; color: %2; border: none;"
                   "  border-radius: 5px; padding: 8px 20px; font-size: 13px; }"
                   "QPushButton:hover { background: %3; }"
                   "QPushButton:pressed { background: %4; }"
                   "QPushButton:disabled { background: %5; }")
        .arg(primary().name(), onPrimary().name(), primaryHover().name(), primaryPressed().name(),
             primaryDisabled().name());
}

/// 次按钮样式表（透明背景 + 边框）
inline QString secondaryButtonStyle() {
    return QString("QPushButton { background: transparent; color: %1; border: 1px solid %2;"
                   "  border-radius: 5px; padding: 8px 20px; font-size: 13px; }"
                   "QPushButton:hover { background: %3; }"
                   "QPushButton:pressed { background: %4; }")
        .arg(textPrimary().name(), border().name(), surfaceHover().name(), surfaceHover().darker(110).name());
}

} // namespace TeachingTheme
