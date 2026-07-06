// ============================================================
// TestMarkdownRendererAudit.cpp — MarkdownRenderer 单元测试
// ------------------------------------------------------------
// 验证 MarkdownRenderer::markdownToHtml 的核心功能：
//   1. 标题渲染（# / ## / ###）
//   2. 粗体 **text**
//   3. 斜体 *text*
//   4. 行内代码 `code`
//   5. 围栏代码块 ```
//   6. 无序列表 - item
//   7. 有序列表 1. item
//   8. 任务列表 - [ ] / - [x]
//   9. 引用块 > text
//  10. 表格 | col | col |
//  11. 水平分割线 ---
//  12. HTML 转义（&, <, >）
//  13. 段落分隔（空行）
//  14. 空输入
//  15. std::string 重载
//  16. LabManualContent 真实章节渲染（集成验证）
//  17. 自定义代码块背景色
//  18. 未闭合代码块（容错）
//  19. CSS 样式注入（<style> 块存在）
// ============================================================

#include <gtest/gtest.h>
#include "gui/MarkdownRenderer.h"
#include <QString>
#include <string>

// ---- 1. 标题 ----

TEST(MarkdownRendererAudit, Heading1) {
    QString html = MarkdownRenderer::markdownToHtml(QString("# 标题一"));
    EXPECT_TRUE(html.contains("<h1>标题一</h1>")) << html.toStdString();
}

TEST(MarkdownRendererAudit, Heading2) {
    QString html = MarkdownRenderer::markdownToHtml(QString("## 标题二"));
    EXPECT_TRUE(html.contains("<h2>标题二</h2>")) << html.toStdString();
}

TEST(MarkdownRendererAudit, Heading3) {
    QString html = MarkdownRenderer::markdownToHtml(QString("### 标题三"));
    EXPECT_TRUE(html.contains("<h3>标题三</h3>")) << html.toStdString();
}

// ---- 2. 粗体 ----

TEST(MarkdownRendererAudit, Bold) {
    QString html = MarkdownRenderer::markdownToHtml(QString("这是 **粗体** 文本"));
    EXPECT_TRUE(html.contains("<b>粗体</b>")) << html.toStdString();
}

// ---- 3. 行内代码 ----

TEST(MarkdownRendererAudit, InlineCode) {
    QString html = MarkdownRenderer::markdownToHtml(QString("使用 `var x = 1;` 声明"));
    EXPECT_TRUE(html.contains("<code>var x = 1;</code>")) << html.toStdString();
}

// ---- 4. 围栏代码块 ----

TEST(MarkdownRendererAudit, CodeBlock) {
    QString md = QString("```\nvar x = 42;\nprint(x);\n```");
    QString html = MarkdownRenderer::markdownToHtml(md);
    EXPECT_TRUE(html.contains("<pre")) << html.toStdString();
    EXPECT_TRUE(html.contains("var x = 42;")) << html.toStdString();
    EXPECT_TRUE(html.contains("print(x);")) << html.toStdString();
}

TEST(MarkdownRendererAudit, CodeBlockEscapesHtml) {
    // 代码块内的 < > & 应被转义
    QString md = QString("```\nif (a < b && c > d) {}\n```");
    QString html = MarkdownRenderer::markdownToHtml(md);
    EXPECT_TRUE(html.contains("&lt;")) << html.toStdString();
    EXPECT_TRUE(html.contains("&gt;")) << html.toStdString();
    EXPECT_TRUE(html.contains("&amp;")) << html.toStdString();
}

// ---- 5. 无序列表 ----

TEST(MarkdownRendererAudit, UnorderedList) {
    QString md = QString("- 第一项\n- 第二项\n- 第三项");
    QString html = MarkdownRenderer::markdownToHtml(md);
    EXPECT_TRUE(html.contains("<ul>")) << html.toStdString();
    EXPECT_TRUE(html.contains("</ul>")) << html.toStdString();
    EXPECT_TRUE(html.contains("<li>第一项</li>")) << html.toStdString();
    EXPECT_TRUE(html.contains("<li>第二项</li>")) << html.toStdString();
}

// ---- 6. 有序列表 ----

TEST(MarkdownRendererAudit, OrderedList) {
    QString md = QString("1. 步骤一\n2. 步骤二\n3. 步骤三");
    QString html = MarkdownRenderer::markdownToHtml(md);
    EXPECT_TRUE(html.contains("<ol>")) << html.toStdString();
    EXPECT_TRUE(html.contains("</ol>")) << html.toStdString();
    EXPECT_TRUE(html.contains("<li>步骤一</li>")) << html.toStdString();
    EXPECT_TRUE(html.contains("<li>步骤二</li>")) << html.toStdString();
}

// ---- 7. 任务列表 ----

TEST(MarkdownRendererAudit, TaskListUnchecked) {
    QString md = QString("- [ ] 未完成任务");
    QString html = MarkdownRenderer::markdownToHtml(md);
    EXPECT_TRUE(html.contains("task-list-item")) << html.toStdString();
    EXPECT_TRUE(html.contains("task-checkbox")) << html.toStdString();
    EXPECT_TRUE(html.contains("未完成任务")) << html.toStdString();
}

TEST(MarkdownRendererAudit, TaskListChecked) {
    QString md = QString("- [x] 已完成任务");
    QString html = MarkdownRenderer::markdownToHtml(md);
    EXPECT_TRUE(html.contains("task-checkbox checked")) << html.toStdString();
    EXPECT_TRUE(html.contains("✓")) << html.toStdString();
}

// ---- 8. 引用块 ----

TEST(MarkdownRendererAudit, Blockquote) {
    QString md = QString("> 这是一条引用\n> 第二行引用");
    QString html = MarkdownRenderer::markdownToHtml(md);
    EXPECT_TRUE(html.contains("<blockquote>")) << html.toStdString();
    EXPECT_TRUE(html.contains("这是一条引用")) << html.toStdString();
    EXPECT_TRUE(html.contains("第二行引用")) << html.toStdString();
}

// ---- 9. 表格 ----

TEST(MarkdownRendererAudit, Table) {
    QString md = QString(
        "| 类型 | 描述 |\n"
        "| --- | --- |\n"
        "| int | 整数 |\n"
        "| str | 字符串 |");
    QString html = MarkdownRenderer::markdownToHtml(md);
    EXPECT_TRUE(html.contains("<table>")) << html.toStdString();
    EXPECT_TRUE(html.contains("<th>类型</th>")) << html.toStdString();
    EXPECT_TRUE(html.contains("<th>描述</th>")) << html.toStdString();
    EXPECT_TRUE(html.contains("<td>整数</td>")) << html.toStdString();
    EXPECT_TRUE(html.contains("<td>字符串</td>")) << html.toStdString();
}

// ---- 10. 水平分割线 ----

TEST(MarkdownRendererAudit, HorizontalRule) {
    QString html = MarkdownRenderer::markdownToHtml(QString("上文\n---\n下文"));
    EXPECT_TRUE(html.contains("<hr>")) << html.toStdString();
}

// ---- 11. HTML 转义 ----

TEST(MarkdownRendererAudit, EscapesHtmlInParagraph) {
    QString html = MarkdownRenderer::markdownToHtml(QString("a < b > c & d"));
    EXPECT_TRUE(html.contains("&lt;")) << html.toStdString();
    EXPECT_TRUE(html.contains("&gt;")) << html.toStdString();
    EXPECT_TRUE(html.contains("&amp;")) << html.toStdString();
}

// ---- 12. 段落分隔 ----

TEST(MarkdownRendererAudit, ParagraphSplit) {
    QString md = QString("第一段\n\n第二段");
    QString html = MarkdownRenderer::markdownToHtml(md);
    EXPECT_TRUE(html.contains("<p>第一段</p>")) << html.toStdString();
    EXPECT_TRUE(html.contains("<p>第二段</p>")) << html.toStdString();
}

// ---- 13. 空输入 ----

TEST(MarkdownRendererAudit, EmptyInput) {
    QString html = MarkdownRenderer::markdownToHtml(QString(""));
    // 新实现输出 <html><head></head><body></body></html>
    EXPECT_TRUE(html.contains("<html>")) << html.toStdString();
    EXPECT_TRUE(html.contains("<body></body>")) << html.toStdString();
}

// ---- 14. std::string 重载 ----

TEST(MarkdownRendererAudit, StdStringOverload) {
    std::string md = "## 标题";
    QString html = MarkdownRenderer::markdownToHtml(md);
    EXPECT_TRUE(html.contains("<h2>标题</h2>")) << html.toStdString();
}

// ---- 15. 复合：标题 + 列表 + 代码块（LabManual 真实片段） ----

TEST(MarkdownRendererAudit, CompositeLabManualSnippet) {
    QString md = QString(
        "# 实验 1：词法分析\n\n"
        "## 关键概念\n"
        "- **Token 类型**：关键字 / 标识符\n"
        "- **注释分离**：从主流分离\n\n"
        "## 示例\n"
        "```\nvar x = 42;\n```\n\n"
        "---\n\n"
        "✅ 完成");
    QString html = MarkdownRenderer::markdownToHtml(md);
    EXPECT_TRUE(html.contains("<h1>实验 1")) << html.toStdString();
    EXPECT_TRUE(html.contains("<h2>关键概念</h2>")) << html.toStdString();
    EXPECT_TRUE(html.contains("<b>Token 类型</b>")) << html.toStdString();
    EXPECT_TRUE(html.contains("<ul>")) << html.toStdString();
    EXPECT_TRUE(html.contains("<pre")) << html.toStdString();
    EXPECT_TRUE(html.contains("<hr>")) << html.toStdString();
    EXPECT_TRUE(html.contains("✅ 完成")) << html.toStdString();
}

// ---- 16. 自定义代码块背景色 ----

TEST(MarkdownRendererAudit, CustomCodeBlockBg) {
    QString md = QString("```\ncode\n```");
    QString html = MarkdownRenderer::markdownToHtml(md, QString("#abcdef"));
    // 代码块 <pre> 的 style 属性中包含 background:#abcdef
    EXPECT_TRUE(html.contains("background:#abcdef")) << html.toStdString();
}

// ---- 17. 未闭合代码块（容错） ----

TEST(MarkdownRendererAudit, UnterminatedCodeBlock) {
    QString md = QString("```\nvar x = 1;");
    QString html = MarkdownRenderer::markdownToHtml(md);
    // 未闭合也应输出 <pre>
    EXPECT_TRUE(html.contains("<pre")) << html.toStdString();
    EXPECT_TRUE(html.contains("var x = 1;")) << html.toStdString();
}

// ---- 18. 斜体 ----

TEST(MarkdownRendererAudit, Italic) {
    QString html = MarkdownRenderer::markdownToHtml(QString("这是 *斜体* 文本"));
    EXPECT_TRUE(html.contains("<i>斜体</i>")) << html.toStdString();
}

// ---- 19. CSS 样式注入 ----

TEST(MarkdownRendererAudit, StylesheetInjected) {
    QString html = MarkdownRenderer::markdownToHtml(QString("# 标题"));
    EXPECT_TRUE(html.contains("<style>")) << html.toStdString();
    EXPECT_TRUE(html.contains("font-family")) << html.toStdString();
    EXPECT_TRUE(html.contains("border-radius")) << html.toStdString();
}

// ---- 20. Fragment API 剥离外壳 ----

TEST(MarkdownRendererAudit, FragmentStripsWrapper) {
    QString md = QString("# 标题\n\n段落");
    QString fragment = MarkdownRenderer::markdownToHtmlFragment(md);
    EXPECT_FALSE(fragment.startsWith("<html>")) << fragment.toStdString();
    EXPECT_TRUE(fragment.contains("<h1>标题</h1>")) << fragment.toStdString();
    EXPECT_TRUE(fragment.contains("<p>段落</p>")) << fragment.toStdString();
}

// ---- 21. 代码块语言标签 ----

TEST(MarkdownRendererAudit, CodeBlockLanguageLabel) {
    QString md = QString("```js\nvar x = 1;\n```");
    QString html = MarkdownRenderer::markdownToHtml(md);
    EXPECT_TRUE(html.contains("js")) << html.toStdString();
    EXPECT_TRUE(html.contains("<pre")) << html.toStdString();
}
