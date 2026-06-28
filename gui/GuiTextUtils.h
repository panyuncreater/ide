#pragma once

// ============================================================
// GuiTextUtils.h — GUI 文本追加共享工具
// ------------------------------------------------------------
// P1-12 fix: 提取 OutputPanel/ReplPanel 中 4 处重复的文本追加逻辑。
// 统一处理：移动光标到末尾 → 插入分隔换行 → 去除尾部换行 → 可选着色 → 插入 → 滚动可见。
// Dedup-4A fix: 提取 11 处重复的 QFont("Consolas", N) 构造为 monospaceFont() 共享工厂。
// ============================================================

#include <QString>
#include <QTextCursor>
#include <QTextCharFormat>
#include <QTextEdit>
#include <QFont>
#include <map>
#include <utility>  // std::pair

namespace GuiTextUtils {

/// 向 QTextEdit 追加一行文本（自动处理首行换行与尾部换行去除）。
/// @param edit 目标 QTextEdit
/// @param text 待追加文本
/// @param fmt  可选字符格式（错误用红色）；为 nullptr 时使用默认格式
inline void appendLine(QTextEdit* edit, const QString& text,
                       const QTextCharFormat* fmt = nullptr) {
    QTextCursor cursor(edit->document());
    cursor.movePosition(QTextCursor::End);
    if (!edit->document()->isEmpty()) {
        cursor.insertText("\n");
    }
    // 去除尾部换行，避免多余空行
    QString trimmed = text;
    while (trimmed.endsWith('\n') || trimmed.endsWith('\r')) trimmed.chop(1);
    if (fmt) cursor.setCharFormat(*fmt);
    cursor.insertText(trimmed);
    if (fmt) cursor.setCharFormat(QTextCharFormat());
    edit->setTextCursor(cursor);
    edit->ensureCursorVisible();
}

/// Dedup-4A: 获取共享的等宽字体实例。
/// 缓存按 (pointSize, bold) 组合，避免每次构造 QFont 触发的字体度量查询。
/// @param pointSize 字号（默认 10，与多数调用点一致）
/// @param bold      是否加粗
inline const QFont& monospaceFont(int pointSize = 10, bool bold = false) {
    // 用 std::pair<int,bool> 作为键，自带 operator<，避免自定义比较器
    using Key = std::pair<int, bool>;
    static std::map<Key, QFont> cache;
    Key key{pointSize, bold};
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    QFont f("Consolas", pointSize);
    if (bold) f.setBold(true);
    auto emplaced = cache.emplace(key, std::move(f));
    return emplaced.first->second;
}

} // namespace GuiTextUtils
