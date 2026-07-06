#pragma once

// ============================================================
// MarkdownRenderer.h — 轻量 Markdown → HTML 渲染器（教学面板共用）
// ------------------------------------------------------------
// 用于把教学面板的散文式说明文本统一渲染为 QTextBrowser 可显示的 HTML，
// 替代各面板手写的 "<p><b>...</b></p>" 模板字符串。
//
// 支持的 Markdown 子集（覆盖 LabManualContent.cpp + 各 Library 散文字段）：
//   - 标题：# / ## / ### / #### / ##### / ######
//   - 粗体：**text**
//   - 斜体：*text*
//   - 行内代码：`code`
//   - 围栏代码块：```lang\n...\n```（lang 作为语言标签显示在代码块上方）
//   - 无序列表：- / * 开头
//   - 有序列表：1. / 2. 开头
//   - 任务列表：- [ ] / - [x] 开头（渲染为带复选框的列表项）
//   - 引用块：> text（渲染为带左边框的引用块）
//   - 表格：| col1 | col2 | + 分隔行 |---|---|
//   - 水平分割线：--- / ***（单独成行）
//   - 段落：空行分隔
//
// CSS 样式注入：输出 HTML 包含 <style> 块，让标题/列表/代码块/表格/引用块
// 有美观的视觉效果（颜色、间距、圆角、边框等），无需调用方额外加样式。
//
// 不依赖第三方库，单遍扫描实现，~400 行。
// ============================================================

#include <QString>

namespace MarkdownRenderer {

/// 将 Markdown 文本渲染为 HTML。
/// @param markdown 原始 Markdown 文本
/// @param codeBlockBg 代码块背景色（hex，如 "#f5f5f5"）；为空时使用默认浅灰
/// @return 可直接传给 QTextBrowser::setHtml 的 HTML 字符串
QString markdownToHtml(const QString& markdown, const QString& codeBlockBg = QString());

/// 便捷重载：从 std::string 直接渲染（避免调用点反复 QString::fromUtf8）。
QString markdownToHtml(const std::string& markdown, const QString& codeBlockBg = QString());

/// 将 Markdown 渲染为 HTML 片段（不含 <html><body> 包裹），
/// 便于面板把散文式说明嵌入已有的 HTML 模板字符串中。
QString markdownToHtmlFragment(const QString& markdown, const QString& codeBlockBg = QString());

/// std::string 重载的片段版本。
QString markdownToHtmlFragment(const std::string& markdown, const QString& codeBlockBg = QString());

} // namespace MarkdownRenderer
