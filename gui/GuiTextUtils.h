#pragma once

// ============================================================
// GuiTextUtils.h — GUI 文本追加共享工具
// ------------------------------------------------------------
// P1-12 fix: 提取 ReplPanel 等面板中 4 处重复的文本追加逻辑。
// 统一处理：移动光标到末尾 → 插入分隔换行 → 去除尾部换行 → 可选着色 → 插入 → 滚动可见。
// Dedup-4A fix: 提取 11 处重复的 QFont("Consolas", N) 构造为 monospaceFont() 共享工厂。
// ============================================================

#include <QFont>
#include <QScrollBar> // BUG-REPL-G4 fix: verticalScrollBar() 智能滚动判断
#include <QString>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextEdit>
#include <map>
#include <utility> // std::pair

namespace GuiTextUtils {

// ============================================================
// 跨机器字体一致性方案（R75 fix, 2026-07-10）
// ------------------------------------------------------------
// 问题根因：
//   1. main.cpp 原 setPixelSize(14) + 字体族 {"Microsoft YaHei",
//      "PingFang SC", "Segoe UI"} 回退链不完整，目标机器缺中文字体时
//      中文 UI 回退到非中文字体，显示不清/方块。
//   2. monospaceFont 原硬编码 QFont("Consolas", n)，Consolas 在精简版
//      Windows/Server 可能缺失，回退到系统默认（可能非等宽/难看）。
//
// 解决方案：
//   - 用 QFont::setFamilies 一次性设置完整回退链，Qt 自动按序匹配首个
//     可用字体族，保证中英文均有合适字体。
//   - 统一用 setPointSize（物理单位 1/72 英寸），跨 DPI 自适应，避免
//     setPixelSize 在不同缩放设置下物理大小不一致。
//   - 等宽字体额外 setStyleHint(QFont::TypeWriter)，确保 Qt 在所有候选
//     缺失时仍能选择系统等宽字体而非比例字体。
// ============================================================

/// UI 字体完整回退链（中文优先 → 英文 UI → 通用回退）。
/// 顺序保证：目标机器无论 Windows/macOS/Linux、中文或英文环境，
/// 均能找到支持中文的可用字体，避免中文 UI 显示方块/乱码。
inline QStringList uiFontFallbackChain() {
    return {
        "Microsoft YaHei",    // Windows 中文（首选，渲染清晰）
        "PingFang SC",        // macOS 中文
        "Noto Sans CJK SC",   // Linux 中文（常见发行版预装）
        "Source Han Sans SC", // 思源黑体（跨平台）
        "Segoe UI",           // Windows UI 英文
        "SF Pro Text",        // macOS UI 英文
        "Arial Unicode MS",   // 跨平台 Unicode 兜底
        "Arial"               // 最终通用回退
    };
}

/// 等宽字体完整回退链（现代等宽 → 传统等宽 → 通用回退）。
/// 保证代码编辑器/AST/字节码等面板在任何机器上都能用合适的等宽字体。
inline QStringList monoFontFallbackChain() {
    return {
        "Cascadia Code",    // Windows 11 / VS 现代等宽
        "Cascadia Mono",    // Cascadia 无连字变体
        "Consolas",         // Windows 默认等宽
        "JetBrains Mono",   // 常见 IDE 字体
        "Source Code Pro",  // Adobe 开源等宽
        "Menlo",            // macOS 等宽
        "DejaVu Sans Mono", // Linux 等宽
        "Courier New"       // 最终通用回退（几乎所有系统都有）
    };
}

/// 获取跨机器可用的 UI 字体。
/// 使用 pointSize（物理单位），随屏幕 DPI 自适应，保证不同机器物理大小一致。
/// @param pointSize 字号（默认 10pt ≈ 13.3px @ 96dpi，接近原 14px）
inline QFont uiFont(int pointSize = 10) {
    QFont f;
    f.setFamilies(uiFontFallbackChain());
    f.setPointSize(pointSize);
    f.setStyleHint(QFont::SansSerif);
    return f;
}

// ============================================================
// QSS / HTML font-family 字符串工厂（R75 fix）
// ------------------------------------------------------------
// QFont::setFamilies 仅对 QFont 对象生效，QSS 与 HTML 内联样式中的
// font-family 声明需要字符串形式。这里提供与 monoFontFallbackChain() /
// uiFontFallbackChain() 完全对应的 QSS 字符串，作为单一真相源，避免
// 散落各处的 font-family 硬编码与回退链不一致。
// ============================================================

/// 等宽字体的 QSS/HTML font-family 字符串（含完整回退链 + monospace 通用族）。
/// 用于 QSS 的 font-family 与 HTML style 的 font-family 声明。
/// 示例：QString qss = QStringLiteral("QPlainTextEdit { font-family: %1; }")
///                       .arg(GuiTextUtils::monoFontFamilyQss());
inline QString monoFontFamilyQss() {
    return QStringLiteral("\"Cascadia Code\",\"Cascadia Mono\",\"Consolas\","
                          "\"JetBrains Mono\",\"Source Code Pro\",\"Menlo\","
                          "\"DejaVu Sans Mono\",\"Courier New\",monospace");
}

/// UI 字体的 QSS/HTML font-family 字符串（含完整中英文回退链 + sans-serif 通用族）。
/// 用于 QSS 的 font-family 与 HTML style 的 font-family 声明。
inline QString uiFontFamilyQss() {
    return QStringLiteral("\"Microsoft YaHei\",\"PingFang SC\",\"Noto Sans CJK SC\","
                          "\"Source Han Sans SC\",\"Segoe UI\",\"SF Pro Text\","
                          "\"Arial Unicode MS\",\"Arial\",sans-serif");
}

/// 向 QTextEdit 追加一行文本（自动处理首行换行与尾部换行去除）。
/// @param edit 目标 QTextEdit
/// @param text 待追加文本
/// @param fmt  可选字符格式（错误用红色）；为 nullptr 时使用默认格式
inline void appendLine(QTextEdit* edit, const QString& text, const QTextCharFormat* fmt = nullptr) {
    if (!edit)
        return; // BUG-REPL-G4 fix: 防御性空指针检查
    // BUG-REPL-G4 / BUG-OUT-G2 fix: 智能滚动——仅当用户已滚动到底部时才自动跟随。
    // 原实现无条件 ensureCursorVisible()，会强制把用户手动滚到上方查看历史的视图
    // 拉回底部，破坏阅读体验。追加新内容前先记录是否在底部，仅在底部时才滚动。
    bool atBottom = edit->verticalScrollBar()->value() == edit->verticalScrollBar()->maximum();
    QTextCursor cursor(edit->document());
    cursor.movePosition(QTextCursor::End);
    if (!edit->document()->isEmpty()) {
        cursor.insertText("\n");
    }
    // 去除尾部换行，避免多余空行
    QString trimmed = text;
    while (trimmed.endsWith('\n') || trimmed.endsWith('\r'))
        trimmed.chop(1);
    if (fmt)
        cursor.setCharFormat(*fmt);
    cursor.insertText(trimmed);
    if (fmt)
        cursor.setCharFormat(QTextCharFormat());
    edit->setTextCursor(cursor);
    if (atBottom) {
        edit->ensureCursorVisible();
    }
}

/// Dedup-4A: 获取共享的等宽字体实例。
/// 缓存按 (pointSize, bold) 组合，避免每次构造 QFont 触发的字体度量查询。
/// R75 fix: 改用完整回退链（setFamilies）替代硬编码 "Consolas"，
/// 确保 Consolas 缺失的精简版 Windows/Server 仍能用合适等宽字体。
/// @param pointSize 字号（默认 10，与多数调用点一致）
/// @param bold      是否加粗
inline const QFont& monospaceFont(int pointSize = 10, bool bold = false) {
    // 用 std::pair<int,bool> 作为键，自带 operator<，避免自定义比较器
    using Key = std::pair<int, bool>;
    static std::map<Key, QFont> cache;
    Key key{pointSize, bold};
    auto it = cache.find(key);
    if (it != cache.end())
        return it->second;
    QFont f;
    f.setFamilies(monoFontFallbackChain());
    f.setPointSize(pointSize);
    f.setStyleHint(QFont::TypeWriter); // 确保回退到系统等宽而非比例字体
    if (bold)
        f.setBold(true);
    auto emplaced = cache.emplace(key, std::move(f));
    return emplaced.first->second;
}

} // namespace GuiTextUtils
