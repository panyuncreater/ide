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

// ============================================================
// UX-R2 fix: 语义卡片色系（warning / info / success / error）
// ------------------------------------------------------------
// 教学面板中大量状态标签/提示卡片使用硬编码背景+边框+文字三色组合，
// 以下函数提供统一的语义卡片配色，消除硬编码并保证视觉一致性。
// ============================================================

/// 警告卡片背景色：米黄
inline QColor warningBg() {
    return QColor(0xFF, 0xFA, 0xEC); // 替代散布的 #FFFAEC / #FFF8E0
}

/// 警告卡片边框色：金棕
inline QColor warningBorder() {
    return QColor(0xE0, 0xC0, 0x70); // 替代散布的 #E0C070
}

/// 警告卡片文字色：深棕
inline QColor warningText() {
    return QColor(0x5A, 0x45, 0x00); // 替代散布的 #5A4500
}

/// 信息卡片背景色：浅蓝
inline QColor infoBg() {
    return QColor(0xEE, 0xF6, 0xFF); // 替代散布的 #EEF6FF
}

/// 信息卡片边框色：中蓝
inline QColor infoBorder() {
    return QColor(0x88, 0xB0, 0xE0); // 替代散布的 #88B0E0
}

/// 信息卡片文字色：深蓝
inline QColor infoText() {
    return QColor(0x1A, 0x3A, 0x6A); // 替代散布的 #1A3A6A
}

/// 成功卡片背景色：浅绿
inline QColor successBg() {
    return QColor(0xF0, 0xF9, 0xEB); // 浅绿背景
}

/// 成功卡片边框色：中绿
inline QColor successBorder() {
    return QColor(0xA3, 0xD9, 0xA5); // 中绿边框
}

/// 成功卡片文字色：深绿
inline QColor successText() {
    return QColor(0x2D, 0x6A, 0x2F); // 深绿文字
}

/// 错误卡片背景色：浅红
inline QColor errorBg() {
    return QColor(0xFD, 0xED, 0xED); // 浅红背景
}

/// 错误卡片边框色：中红
inline QColor errorBorder() {
    return QColor(0xE0, 0x88, 0x88); // 中红边框
}

/// 错误卡片文字色：深红
inline QColor errorText() {
    return QColor(0x8B, 0x1A, 0x1A); // 深红文字
}

/// 状态标签背景色：浅灰（用于 VM 状态/OpCode 等中性状态标签）
inline QColor statusBg() {
    return QColor(0xF5, 0xF5, 0xF5); // 替代散布的 #F5F5F5 状态标签背景
}

/// 状态标签边框色：中灰
inline QColor statusBorder() {
    return QColor(0xCC, 0xCC, 0xCC); // 替代散布的 #CCC 状态标签边框
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

// ============================================================
// OPT-11：GUI 硬编码颜色全量迁移补充色板
// ------------------------------------------------------------
// 背景：gui/*.cpp 曾散布 500+ 处硬编码色值，同一色值在多个面板各存副本，
// 调色需全仓搜索。以下为迁移中发现的、TeachingTheme 尚未覆盖的语义角色。
// 约束：返回值与被替代的字面量逐位一致（迁移不改变任何视觉）；
//       同值多角色表示不同语义，禁止合并或改色。
// 用法：setStyleSheet(QString("color:%1;").arg(TeachingTheme::dangerRed().name()));
// ============================================================

// --- 中性表面 / 文本补充 ---
/// 说明区/示例块浅底（替代 #F8F9FA）
inline QColor surfaceAlt() {
    return QColor(0xF8, 0xF9, 0xFA);
}
/// 表格内嵌/场景代码浅底（替代 #F7F7F7）
inline QColor surfaceSunken() {
    return QColor(0xF7, 0xF7, 0xF7);
}
/// 自绘表格表头背景（替代 #F0F0F0）
inline QColor tableHeaderBg() {
    return QColor(0xF0, 0xF0, 0xF0);
}
/// 碎片区容器背景（替代 #FAFAFA）
inline QColor shuffledAreaBg() {
    return QColor(0xFA, 0xFA, 0xFA);
}
/// 滚动条滑块（替代 #C8C8C8）
inline QColor scrollbarHandle() {
    return QColor(0xC8, 0xC8, 0xC8);
}
/// 默认正文灰（替代 #333333）
inline QColor textDefault() {
    return QColor(0x33, 0x33, 0x33);
}
/// 辅助说明灰（替代 #666666）
inline QColor textAuxiliary() {
    return QColor(0x66, 0x66, 0x66);
}
/// 弱化标注灰（替代 #707070）
inline QColor textSubtle() {
    return QColor(0x70, 0x70, 0x70);
}
/// 极弱文本灰（替代 #555555）
inline QColor textFaint() {
    return QColor(0x55, 0x55, 0x55);
}
/// 主色加深（按钮 hover，替代 #1E6FA3）
inline QColor primaryDeep() {
    return QColor(0x1E, 0x6F, 0xA3);
}
/// 按钮浅蓝 hover 背景（替代 #D0E0FF）
inline QColor buttonHoverBg() {
    return QColor(0xD0, 0xE0, 0xFF);
}

// --- 语义强调色 ---
/// 危险/错误强调红（替代 #C0392B）
inline QColor dangerRed() {
    return QColor(0xC0, 0x39, 0x2B);
}
/// 深危险红（错误边框，替代 #C03030）
inline QColor dangerDeep() {
    return QColor(0xC0, 0x30, 0x30);
}
/// 强错误文本红（替代 #CC0000）
inline QColor errorStrong() {
    return QColor(0xCC, 0x00, 0x00);
}
/// 成功强调绿（替代 #27AE60）
inline QColor successGreen() {
    return QColor(0x27, 0xAE, 0x60);
}
/// 成功徽标浅绿背景（替代 #E8F5E9）
inline QColor successSoftBg() {
    return QColor(0xE8, 0xF5, 0xE9);
}
/// 信息强调蓝（替代 #3498DB）
inline QColor infoBlue() {
    return QColor(0x34, 0x98, 0xDB);
}
/// 深信息蓝（小节标题，替代 #2980B9）
inline QColor deepBlue() {
    return QColor(0x29, 0x80, 0xB9);
}
/// 深错误红（替代 #C62828）
inline QColor deepRed() {
    return QColor(0xC6, 0x28, 0x28);
}
/// 深成功绿（替代 #2E7D32）
inline QColor deepGreen() {
    return QColor(0x2E, 0x7D, 0x32);
}
/// 深警告橙（替代 #F57F17）
inline QColor amberDeep() {
    return QColor(0xF5, 0x7F, 0x17);
}
/// 强调橙（替代 #E67E22）
inline QColor orangeAccent() {
    return QColor(0xE6, 0x7E, 0x22);
}
/// 深强调橙（替代 #D35400）
inline QColor orangeDeep() {
    return QColor(0xD3, 0x54, 0x00);
}
/// 浅危险红（替代 #E74C3C）
inline QColor redLight() {
    return QColor(0xE7, 0x4C, 0x3C);
}
/// 警告橙边框（替代 #F39C12）
inline QColor amberBorder() {
    return QColor(0xF3, 0x9C, 0x12);
}
/// 青绿强调（替代 #16A085）
inline QColor teal() {
    return QColor(0x16, 0xA0, 0x85);
}
/// 紫色强调（替代 #8E44AD）
inline QColor purple() {
    return QColor(0x8E, 0x44, 0xAD);
}
/// 作答正确绿（替代 #4CAF50）
inline QColor answerCorrect() {
    return QColor(0x4C, 0xAF, 0x50);
}
/// 作答错误红（替代 #F44336）
inline QColor answerWrong() {
    return QColor(0xF4, 0x43, 0x36);
}
/// 导航高亮边框蓝（替代 #2196F3）
inline QColor navHighlight() {
    return QColor(0x21, 0x96, 0xF3);
}
/// 课程卡片强调蓝（替代 #4A90D9）
inline QColor courseAccent() {
    return QColor(0x4A, 0x90, 0xD9);
}

// --- Solarized 扩展色 ---
/// 深蓝灰标题色（替代 #2C3E50）
inline QColor midnight() {
    return QColor(0x2C, 0x3E, 0x50);
}
/// 中性浅灰底（替代 #ECF0F1）
inline QColor clouds() {
    return QColor(0xEC, 0xF0, 0xF1);
}
/// 中性浅灰边框（替代 #BDC3C7）
inline QColor silver() {
    return QColor(0xBD, 0xC3, 0xC7);
}
/// Solarized 橙（字节码阶段，替代 #CB4B16）
inline QColor solarizedOrange() {
    return QColor(0xCB, 0x4B, 0x16);
}
/// Solarized 青（IR 阶段，替代 #2AA198）
inline QColor solarizedCyan() {
    return QColor(0x2A, 0xA1, 0x98);
}
/// Solarized 紫（IR 阶段，替代 #6C71C4）
inline QColor solarizedViolet() {
    return QColor(0x6C, 0x71, 0xC4);
}

// --- Markdown 渲染 ---
/// Markdown 二/三级标题蓝（替代 #0078D4）
inline QColor markdownHeading() {
    return QColor(0x00, 0x78, 0xD4);
}
/// Markdown 四级标题深墨（替代 #073642）
inline QColor markdownInk() {
    return QColor(0x07, 0x36, 0x42);
}

// --- 语法高亮 ---
/// 语法：字符串（替代 #2AA198）
inline QColor syntaxString() {
    return QColor(0x2A, 0xA1, 0x98);
}
/// 语法：数字字面量（替代 #D33682）
inline QColor syntaxNumber() {
    return QColor(0xD3, 0x36, 0x82);
}
/// 语法：注释（替代 #6A9955）
inline QColor syntaxComment() {
    return QColor(0x6A, 0x99, 0x55);
}
/// 语法：IR 操作码（替代 #268BD2）
inline QColor syntaxOpcode() {
    return QColor(0x26, 0x8B, 0xD2);
}
/// 语法：IR 虚拟寄存器（替代 #2AA198）
inline QColor syntaxRegister() {
    return QColor(0x2A, 0xA1, 0x98);
}
/// 语法：IR 常量/字面量（替代 #CB4B16）
inline QColor syntaxLiteral() {
    return QColor(0xCB, 0x4B, 0x16);
}
/// 语法：运算符/标点（替代 #8C8C8C）
inline QColor syntaxOperator() {
    return QColor(0x8C, 0x8C, 0x8C);
}
/// 语法：标识符（替代 #1E1E1E）
inline QColor syntaxIdentifier() {
    return QColor(0x1E, 0x1E, 0x1E);
}
/// 语法：IR 基本块标签（替代 #6C71C4）
inline QColor syntaxBlockLabel() {
    return QColor(0x6C, 0x71, 0xC4);
}
/// 语法：运算符/标点深灰（替代 QColor(0x65,0x7b,0x83)）
inline QColor syntaxPunctuation() {
    return QColor(0x65, 0x7B, 0x83);
}

// --- 执行后端系列色 ---
/// 解释器系列蓝（替代 #3498DB）
inline QColor backendInterpreter() {
    return QColor(0x34, 0x98, 0xDB);
}
/// 栈 VM 系列绿（替代 #27AE60）
inline QColor backendStackVm() {
    return QColor(0x27, 0xAE, 0x60);
}
/// 寄存器 VM 系列橙（替代 #E67E22）
inline QColor backendRegisterVm() {
    return QColor(0xE6, 0x7E, 0x22);
}
/// 未知后端灰（替代 #888888）
inline QColor backendUnknown() {
    return QColor(0x88, 0x88, 0x88);
}

// --- 图表 / 表格系列 ---
/// 图表系列 1 蓝（替代 QColor(102,153,204)）
inline QColor chartSeries1() {
    return QColor(0x66, 0x99, 0xCC);
}
/// 图表系列 2 橙（替代 QColor(204,153,102)）
inline QColor chartSeries2() {
    return QColor(0xCC, 0x99, 0x66);
}
/// 图表系列 3 绿（替代 QColor(153,204,102)）
inline QColor chartSeries3() {
    return QColor(0x99, 0xCC, 0x66);
}
/// 图表网格线（替代 QColor(200,200,200)）
inline QColor chartGrid() {
    return QColor(0xC8, 0xC8, 0xC8);
}
/// 图表坐标轴文字（替代 QColor(80,80,80)）
inline QColor chartAxisText() {
    return QColor(0x50, 0x50, 0x50);
}
/// 图表刻度文字（替代 QColor(160,160,160)）
inline QColor chartTickText() {
    return QColor(0xA0, 0xA0, 0xA0);
}
/// 图表主墨色（替代 QColor(0,0,0)）
inline QColor chartInk() {
    return QColor(0x00, 0x00, 0x00);
}
/// 表格行选中高亮（替代 QColor(0x66,0x99,0xCC)）
inline QColor tableRowHighlight() {
    return QColor(0x66, 0x99, 0xCC);
}

// --- 异常传播 / 调用栈阶段色 ---
/// 传播步骤：中性背景（替代 #ECF0F1）
inline QColor exceptionNeutralBg() {
    return QColor(0xEC, 0xF0, 0xF1);
}
/// 传播步骤：中性边框（替代 #BDC3C7）
inline QColor exceptionNeutralBorder() {
    return QColor(0xBD, 0xC3, 0xC7);
}
/// 传播步骤：抛出背景（替代 #FADBD8）
inline QColor exceptionThrowBg() {
    return QColor(0xFA, 0xDB, 0xD8);
}
/// 传播步骤：捕获背景（替代 #D5F5E3）
inline QColor exceptionCatchBg() {
    return QColor(0xD5, 0xF5, 0xE3);
}
/// 传播步骤：展开背景（替代 #FDEBD0）
inline QColor exceptionUnwindBg() {
    return QColor(0xFD, 0xEB, 0xD0);
}
/// 传播步骤：终止背景（替代 #E8DAEF）
inline QColor exceptionTerminateBg() {
    return QColor(0xE8, 0xDA, 0xEF);
}
/// 传播步骤：终止边框（替代 #9B59B6）
inline QColor exceptionTerminateBorder() {
    return QColor(0x9B, 0x59, 0xB6);
}
/// 传播步骤：finally 背景（替代 #D6EAF8）
inline QColor exceptionFinallyBg() {
    return QColor(0xD6, 0xEA, 0xF8);
}
/// 调用栈：已销毁背景（替代 #F2F3F4）
inline QColor exceptionDestroyedBg() {
    return QColor(0xF2, 0xF3, 0xF4);
}
/// 调用栈：已销毁文本（替代 #95A5A6）
inline QColor exceptionDestroyedText() {
    return QColor(0x95, 0xA5, 0xA6);
}
/// 调用栈：异常帧背景（替代 #FDF2E9）
inline QColor exceptionErrorBg() {
    return QColor(0xFD, 0xF2, 0xE9);
}
/// 调用栈：搜索中背景（替代 #FEF9E7）
inline QColor exceptionSearchBg() {
    return QColor(0xFE, 0xF9, 0xE7);
}
/// 调用栈：搜索中边框（替代 #F1C40F）
inline QColor exceptionSearchBorder() {
    return QColor(0xF1, 0xC4, 0x0F);
}
/// 调用栈：已恢复背景（替代 #E8F8F5）
inline QColor exceptionResolvedBg() {
    return QColor(0xE8, 0xF8, 0xF5);
}
/// 调用栈：已恢复边框（替代 #1ABC9C）
inline QColor exceptionResolvedBorder() {
    return QColor(0x1A, 0xBC, 0x9C);
}

// --- 模块系统可视化 ---
/// 模块加载成功绿（替代 #0A7A28）
inline QColor moduleOkFg() {
    return QColor(0x0A, 0x7A, 0x28);
}
/// 模块循环/预扫描警告橙（替代 #B8860B）
inline QColor moduleCycleFg() {
    return QColor(0xB8, 0x86, 0x0B);
}
/// 模块节点：成功浅绿（替代 #E8F5E9）
inline QColor moduleNodeGreenBg() {
    return QColor(0xE8, 0xF5, 0xE9);
}
/// 模块节点：依赖浅蓝（替代 #E3F2FD）
inline QColor moduleNodeBlueBg() {
    return QColor(0xE3, 0xF2, 0xFD);
}
/// 模块节点：依赖蓝边框（替代 #1565C0）
inline QColor moduleNodeBlueBorder() {
    return QColor(0x15, 0x65, 0xC0);
}
/// 模块节点：循环浅橙（替代 #FFF3E0）
inline QColor moduleNodeOrangeBg() {
    return QColor(0xFF, 0xF3, 0xE0);
}
/// 模块节点：循环橙边框（替代 #EF6C00）
inline QColor moduleNodeOrangeBorder() {
    return QColor(0xEF, 0x6C, 0x00);
}
/// 模块节点：预扫描浅黄（替代 #FFF8E1）
inline QColor moduleNodeYellowBg() {
    return QColor(0xFF, 0xF8, 0xE1);
}
/// 模块节点：缺失浅红（替代 #FFEBEE）
inline QColor moduleNodeErrorBg() {
    return QColor(0xFF, 0xEB, 0xEE);
}

// --- 内存模型可视化 ---
/// 内存布局：指针 tag 粉红（替代 #FFB3BA）
inline QColor memoryTagBg() {
    return QColor(0xFF, 0xB3, 0xBA);
}
/// 内存布局：payload 浅蓝（替代 #BAE1FF）
inline QColor memoryPayloadBg() {
    return QColor(0xBA, 0xE1, 0xFF);
}
/// GC 步骤：标记中蓝（替代 #3079C0）
inline QColor memoryStateMarking() {
    return QColor(0x30, 0x79, 0xC0);
}
/// GC 步骤：回收中红（替代 #C03030）
inline QColor memoryStateReclaim() {
    return QColor(0xC0, 0x30, 0x30);
}
/// GC 步骤：复位绿（替代 #709030）
inline QColor memoryStateReset() {
    return QColor(0x70, 0x90, 0x30);
}
/// GC 步骤：空闲灰（替代 #909090）
inline QColor memoryStateIdle() {
    return QColor(0x90, 0x90, 0x90);
}
/// 沙箱失败反馈浅红底（替代 #FFF0F0）
inline QColor feedbackErrorBg() {
    return QColor(0xFF, 0xF0, 0xF0);
}
/// 沙箱成功反馈浅绿底（替代 #F0FFF0）
inline QColor feedbackOkBg() {
    return QColor(0xF0, 0xFF, 0xF0);
}
/// 沙箱成功反馈绿边（替代 #30A030）
inline QColor feedbackOkBorder() {
    return QColor(0x30, 0xA0, 0x30);
}
/// upvalue 未关闭橙（替代 #C07030）
inline QColor upvalueOpenFg() {
    return QColor(0xC0, 0x70, 0x30);
}
/// upvalue 已关闭绿（替代 #2A7A2A）
inline QColor upvalueClosedFg() {
    return QColor(0x2A, 0x7A, 0x2A);
}
/// IR 视图行高亮底（替代 #DCD0B0）
inline QColor irHighlightBg() {
    return QColor(0xDC, 0xD0, 0xB0);
}

// --- 编辑器（CodeEditor）亮色分支 ---
/// 编辑器行号槽背景（替代 QColor(0xF5,0xF5,0xF5)）
inline QColor editorGutterBg() {
    return QColor(0xF5, 0xF5, 0xF5);
}
/// 编辑器行号文字（替代 QColor(0x65,0x7b,0x83)）
inline QColor editorLineNumFg() {
    return QColor(0x65, 0x7B, 0x83);
}
/// 编辑器当前行背景（替代 QColor(0xF0,0xF0,0xF0)）
inline QColor editorCurrentLineBg() {
    return QColor(0xF0, 0xF0, 0xF0);
}
/// 调试执行行背景（替代 QColor(0xFF,0xF4,0xD4)）
inline QColor editorExecLineBg() {
    return QColor(0xFF, 0xF4, 0xD4);
}
/// 源码定位高亮背景（替代 QColor(0xbf,0xdb,0xfe)）
inline QColor editorSourceHighlightBg() {
    return QColor(0xBF, 0xDB, 0xFE);
}
/// 错误作答高亮底（替代 QColor(255,200,200)）
inline QColor answerWrongBg() {
    return QColor(0xFF, 0xC8, 0xC8);
}

// --- 固定深色终端 / 代码预览 ---
/// 运行输出深色底（替代 #101820）
inline QColor consoleBg() {
    return QColor(0x10, 0x18, 0x20);
}
/// 运行输出荧光绿字（替代 #90FFD0）
inline QColor consoleFg() {
    return QColor(0x90, 0xFF, 0xD0);
}
/// 深色代码预览底（替代 #1E1E1E）
inline QColor darkCodeBg() {
    return QColor(0x1E, 0x1E, 0x1E);
}
/// 深色代码预览字（替代 #D4D4D4）
inline QColor darkCodeFg() {
    return QColor(0xD4, 0xD4, 0xD4);
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
