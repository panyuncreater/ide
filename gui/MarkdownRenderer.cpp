// ============================================================
// MarkdownRenderer.cpp — 轻量 Markdown → HTML 渲染器实现
// ============================================================
//
// 支持的 Markdown 子集（覆盖 LabManualContent.cpp + 各 Library 散文字段）：
//   - 标题：# / ## / ### / #### / ##### / ######
//   - 粗体：**text**
//   - 斜体：*text*
//   - 行内代码：`code`
//   - 围栏代码块：```lang\n...\n```
//   - 无序列表：- / * 开头
//   - 有序列表：1. / 2. 开头
//   - 任务列表：- [ ] / - [x] 开头
//   - 表格：| col1 | col2 | + 分隔行 |---|---|
//   - 引用块：> text
//   - 水平分割线：--- / ***（单独成行）
//   - 段落：空行分隔
//
// CSS 样式注入：输出 HTML 包含 <style> 块，让标题/列表/代码块/表格/引用块
// 有美观的视觉效果（颜色、间距、圆角、边框等），无需调用方额外加样式。
// ============================================================

#include "gui/MarkdownRenderer.h"

#include <QStringList>
#include <QRegularExpression>
#include <QRegularExpressionMatch>

namespace MarkdownRenderer {

// ---- 内部辅助 ----

// PERF: 正则表达式编译为 static const，避免每次 markdownToHtml 调用都重新编译。
// QRegularExpression 构造会调用 pcre2_compile，单次约 10-50μs，多个正则累计可省数百 μs/次。
// 对 LabManualPanel 章节切换、各教学面板 detail 刷新有显著收益。
static const QRegularExpression kReInlineCode(QStringLiteral("`([^`]+)`"));
static const QRegularExpression kReBold(QStringLiteral("\\*\\*([^*]+)\\*\\*"));
static const QRegularExpression kReItalic(QStringLiteral("(?<!\\*)\\*([^*]+)\\*(?!\\*)"));
static const QRegularExpression kReHeading(QStringLiteral("^(#{1,6})\\s+(.+)$"));
static const QRegularExpression kReUnorderedList(QStringLiteral("^[-*]\\s+(.+)$"));
static const QRegularExpression kReOrderedList(QStringLiteral("^\\d+\\.\\s+(.+)$"));
// 任务列表：- [ ] 或 - [x] / - [X]
static const QRegularExpression kReTaskList(QStringLiteral("^[-*]\\s+\\[([ xX])\\]\\s+(.+)$"));
// 引用块：> text
static const QRegularExpression kReBlockquote(QStringLiteral("^>\\s?(.*)$"));
// 表格行：| cell | cell |
static const QRegularExpression kReTableRow(QStringLiteral("^\\|(.+)\\|$"));

/// 检测表格分隔行：行只包含 | - : 空白，且至少有一个 -
/// 避免使用字符类正则（[\s:-|] 中的 - 会被解释为范围）
static bool isTableSeparatorLine(const QString& line) {
    QString t = line.trimmed();
    if (t.isEmpty()) return false;
    bool hasDash = false;
    for (const QChar& c : t) {
        if (c == '|' || c == ':') continue;
        if (c == '-') { hasDash = true; continue; }
        if (c.isSpace()) continue;
        return false;  // 包含其他字符，不是分隔行
    }
    return hasDash;
}

/// 转义 HTML 特殊字符（&, <, >），但不破坏已存在的 HTML 实体。
/// 用于把原始 Markdown 文本安全嵌入 HTML 之前。
static QString escapeHtml(const QString& s) {
    QString out = s;
    out.replace('&', QStringLiteral("&amp;"));
    out.replace('<', QStringLiteral("&lt;"));
    out.replace('>', QStringLiteral("&gt;"));
    return out;
}

/// 渲染行内格式：粗体 **x** / 斜体 *x* / 行内代码 `x`。
/// 输入应已 escapeHtml 转义过。
static QString renderInline(const QString& s) {
    QString out = s;
    // 行内代码 `code`（先处理，避免 code 内的 ** 被后续规则误处理）
    out.replace(kReInlineCode, QStringLiteral("<code>\\1</code>"));
    // 粗体 **text**
    out.replace(kReBold, QStringLiteral("<b>\\1</b>"));
    // 斜体 *text*（避免与粗体冲突，要求 * 两侧非 *）
    out.replace(kReItalic, QStringLiteral("<i>\\1</i>"));
    return out;
}

/// 生成嵌入 HTML 的 <style> 块，让 Markdown 输出有美观的视觉效果。
/// 颜色方案与 TeachingTheme 保持一致（亮色主题）。
static QString buildStylesheet(const QString& codeBlockBg) {
    static const QString kStylesheet = QStringLiteral(R"(
        <style>
        body { font-family: 'Segoe UI', 'Microsoft YaHei', sans-serif; font-size: 14px; color: #1e1e1e; line-height: 1.6; }
        h1 { font-size: 22px; color: #0078d4; border-bottom: 2px solid #e5e5e5; padding-bottom: 6px; margin: 16px 0 10px; font-weight: 600; }
        h2 { font-size: 18px; color: #0078d4; border-bottom: 1px solid #e5e5e5; padding-bottom: 4px; margin: 14px 0 8px; font-weight: 600; }
        h3 { font-size: 16px; color: #0078d4; margin: 12px 0 6px; font-weight: 600; }
        h4 { font-size: 14px; color: #1e1e1e; margin: 10px 0 4px; font-weight: 600; }
        h5, h6 { font-size: 13px; color: #5a5a5a; margin: 8px 0 4px; font-weight: 600; }
        p { margin: 6px 0; }
        ul, ol { margin: 6px 0; padding-left: 24px; }
        li { margin: 3px 0; }
        code { background: #f3f3f3; color: #c7254e; padding: 2px 5px; border-radius: 3px; font-family: Consolas, 'Courier New', monospace; font-size: 13px; }
        pre { background: %1; padding: 10px 12px; border-radius: 6px; border: 1px solid #e5e5e5; font-family: Consolas, 'Courier New', monospace; font-size: 13px; white-space: pre-wrap; margin: 8px 0; }
        pre code { background: transparent; color: #1e1e1e; padding: 0; border-radius: 0; font-size: 13px; }
        blockquote { border-left: 4px solid #0078d4; background: #f8f9fa; padding: 8px 12px; margin: 8px 0; color: #5a5a5a; border-radius: 0 4px 4px 0; }
        blockquote p { margin: 4px 0; }
        hr { border: none; border-top: 1px solid #e5e5e5; margin: 12px 0; }
        table { border-collapse: collapse; width: 100%; margin: 8px 0; font-size: 13px; }
        th { background: #f3f3f3; border: 1px solid #e5e5e5; padding: 6px 10px; text-align: left; font-weight: 600; color: #1e1e1e; }
        td { border: 1px solid #e5e5e5; padding: 6px 10px; vertical-align: top; }
        tr:nth-child(even) td { background: #fafafa; }
        .task-list-item { list-style: none; margin-left: -18px; }
        .task-checkbox { display: inline-block; width: 14px; height: 14px; border: 1.5px solid #999; border-radius: 2px; margin-right: 6px; vertical-align: middle; }
        .task-checkbox.checked { background: #107d58; border-color: #107d58; color: white; text-align: center; font-size: 10px; line-height: 14px; }
        b, strong { color: #1e1e1e; font-weight: 600; }
        i, em { color: #5a5a5a; }
        </style>
    )");
    return kStylesheet.arg(codeBlockBg.isEmpty() ? QStringLiteral("#f5f5f5") : codeBlockBg);
}

// ---- 公共 API ----

QString markdownToHtml(const QString& markdown, const QString& codeBlockBg) {
    if (markdown.isEmpty()) {
        return QStringLiteral("<html><head></head><body></body></html>");
    }

    const QString bg = codeBlockBg.isEmpty() ? QStringLiteral("#f5f5f5") : codeBlockBg;
    const QString codeBlockStyle =
        QStringLiteral("background:%1; padding:10px 12px; border-radius:6px; "
                       "border:1px solid #e5e5e5; "
                       "font-family:Consolas, 'Courier New', monospace; "
                       "font-size:13px; "
                       "white-space:pre-wrap;").arg(bg);

    // 按行扫描，识别块级结构
    const QStringList lines = markdown.split('\n');
    QStringList html;
    html << QStringLiteral("<html><head>");
    html << buildStylesheet(bg);
    html << QStringLiteral("</head><body>");

    bool inCodeBlock = false;
    QString codeBlockContent;
    QString codeBlockLang;
    bool inUl = false;  // 无序列表
    bool inOl = false;  // 有序列表
    bool inBlockquote = false;  // 引用块
    QStringList blockquoteLines;  // 引用块累积行
    QStringList paragraph;  // 当前段落累积的行

    // 表格状态
    bool inTable = false;
    QStringList tableHeader;  // 表头单元格
    QStringList tableRows;    // 数据行（每行一个字符串列表）

    auto closeLists = [&]() {
        if (inUl) { html << QStringLiteral("</ul>"); inUl = false; }
        if (inOl) { html << QStringLiteral("</ol>"); inOl = false; }
    };
    auto flushParagraph = [&]() {
        if (!paragraph.isEmpty()) {
            QString joined = paragraph.join(QStringLiteral("\n"));
            html << QStringLiteral("<p>") << renderInline(joined)
                 << QStringLiteral("</p>");
            paragraph.clear();
        }
    };
    auto flushBlockquote = [&]() {
        if (!blockquoteLines.isEmpty()) {
            QString joined = blockquoteLines.join(QStringLiteral("<br>"));
            html << QStringLiteral("<blockquote>") << renderInline(joined)
                 << QStringLiteral("</blockquote>");
            blockquoteLines.clear();
            inBlockquote = false;
        }
    };
    auto flushTable = [&]() {
        if (inTable && !tableHeader.isEmpty()) {
            html << QStringLiteral("<table>");
            // 表头
            html << QStringLiteral("<tr>");
            for (const auto& h : tableHeader) {
                html << QStringLiteral("<th>") << renderInline(h.trimmed())
                     << QStringLiteral("</th>");
            }
            html << QStringLiteral("</tr>");
            // 数据行
            for (const auto& row : tableRows) {
                QStringList cells = row.split(QStringLiteral("|"));
                html << QStringLiteral("<tr>");
                for (const auto& c : cells) {
                    html << QStringLiteral("<td>") << renderInline(c.trimmed())
                         << QStringLiteral("</td>");
                }
                html << QStringLiteral("</tr>");
            }
            html << QStringLiteral("</table>");
        }
        inTable = false;
        tableHeader.clear();
        tableRows.clear();
    };

    for (int i = 0; i < lines.size(); ++i) {
        const QString& line = lines[i];

        // ---- 围栏代码块 ----
        if (line.trimmed().startsWith(QStringLiteral("```"))) {
            if (!inCodeBlock) {
                // 进入代码块：先关闭段落/列表/引用/表格
                flushParagraph();
                closeLists();
                flushBlockquote();
                flushTable();
                inCodeBlock = true;
                codeBlockContent.clear();
                codeBlockLang = line.trimmed().mid(3).trimmed();
            } else {
                // 退出代码块
                QString escaped = escapeHtml(codeBlockContent);
                // 去掉尾部多余换行
                while (escaped.endsWith('\n')) escaped.chop(1);
                // 代码块语言标签（仅作为注释显示在代码上方，不渲染为单独元素）
                if (!codeBlockLang.isEmpty()) {
                    html << QStringLiteral("<div style=\"font-size:11px;color:#999;margin-bottom:2px;\">")
                         << escapeHtml(codeBlockLang)
                         << QStringLiteral("</div>");
                }
                html << QStringLiteral("<pre style=\"%1\">").arg(codeBlockStyle)
                     << escaped
                     << QStringLiteral("</pre>");
                inCodeBlock = false;
                codeBlockContent.clear();
                codeBlockLang.clear();
            }
            continue;
        }
        if (inCodeBlock) {
            // 代码块内容原样保留（最后整体 escapeHtml）
            codeBlockContent += line + QStringLiteral("\n");
            continue;
        }

        // ---- 空行：段落分隔 ----
        if (line.trimmed().isEmpty()) {
            flushParagraph();
            closeLists();
            flushBlockquote();
            flushTable();
            continue;
        }

        // ---- 水平分割线 --- （单独成行，至少 3 个 -） ----
        QString trimmed = line.trimmed();
        if (trimmed == QStringLiteral("---") || trimmed == QStringLiteral("***")) {
            flushParagraph();
            closeLists();
            flushBlockquote();
            flushTable();
            html << QStringLiteral("<hr>");
            continue;
        }

        // ---- 标题 # / ## / ### ----
        QRegularExpressionMatch hm = kReHeading.match(line);
        if (hm.hasMatch()) {
            flushParagraph();
            closeLists();
            flushBlockquote();
            flushTable();
            int level = hm.captured(1).length();
            QString text = renderInline(escapeHtml(hm.captured(2)));
            html << QStringLiteral("<h%1>%2</h%1>").arg(level).arg(text);
            continue;
        }

        // ---- 任务列表 - [ ] / - [x] ----
        QRegularExpressionMatch tm = kReTaskList.match(line);
        if (tm.hasMatch()) {
            flushParagraph();
            if (inOl) { html << QStringLiteral("</ol>"); inOl = false; }
            if (!inUl) { html << QStringLiteral("<ul>"); inUl = true; }
            QString checked = tm.captured(1).toLower();
            QString text = renderInline(escapeHtml(tm.captured(2)));
            QString checkboxClass = (checked == QStringLiteral("x"))
                ? QStringLiteral("task-checkbox checked")
                : QStringLiteral("task-checkbox");
            QString checkboxSymbol = (checked == QStringLiteral("x"))
                ? QStringLiteral("✓")
                : QString();
            html << QStringLiteral("<li class=\"task-list-item\">")
                 << QStringLiteral("<span class=\"%1\">%2</span>").arg(checkboxClass, checkboxSymbol)
                 << renderInline(escapeHtml(tm.captured(2)))
                 << QStringLiteral("</li>");
            continue;
        }

        // ---- 无序列表 - / * 开头（避免与粗体冲突：要求行首是 - 或 *，后跟空格） ----
        QRegularExpressionMatch ulm = kReUnorderedList.match(line);
        if (ulm.hasMatch()) {
            flushParagraph();
            flushBlockquote();
            flushTable();
            if (inOl) { html << QStringLiteral("</ol>"); inOl = false; }
            if (!inUl) { html << QStringLiteral("<ul>"); inUl = true; }
            html << QStringLiteral("<li>") << renderInline(escapeHtml(ulm.captured(1)))
                 << QStringLiteral("</li>");
            continue;
        }

        // ---- 有序列表 1. / 2. 开头 ----
        QRegularExpressionMatch olm = kReOrderedList.match(line);
        if (olm.hasMatch()) {
            flushParagraph();
            flushBlockquote();
            flushTable();
            if (inUl) { html << QStringLiteral("</ul>"); inUl = false; }
            if (!inOl) { html << QStringLiteral("<ol>"); inOl = true; }
            html << QStringLiteral("<li>") << renderInline(escapeHtml(olm.captured(1)))
                 << QStringLiteral("</li>");
            continue;
        }

        // ---- 引用块 > text ----
        QRegularExpressionMatch bqm = kReBlockquote.match(line);
        if (bqm.hasMatch()) {
            flushParagraph();
            closeLists();
            flushTable();
            inBlockquote = true;
            blockquoteLines << escapeHtml(bqm.captured(1));
            continue;
        }
        // 非引用行出现，关闭引用块
        if (inBlockquote) {
            flushBlockquote();
        }

        // ---- 表格 | col1 | col2 | ----
        QRegularExpressionMatch trm = kReTableRow.match(line);
        if (trm.hasMatch()) {
            // 检查下一行是否是分隔行（如果是，说明这是表头）
            bool isNextSeparator = (i + 1 < lines.size())
                && isTableSeparatorLine(lines[i + 1]);
            if (isNextSeparator && !inTable) {
                // 这是表头行
                flushParagraph();
                closeLists();
                flushBlockquote();
                inTable = true;
                tableHeader = trm.captured(1).split(QStringLiteral("|"));
                // 跳过分隔行
                ++i;
                continue;
            } else if (inTable) {
                // 数据行
                tableRows << trm.captured(1);
                continue;
            }
            // 如果不是表格上下文，当作普通段落处理
        }
        // 非表格行出现，关闭表格
        if (inTable) {
            flushTable();
        }

        // ---- 普通段落行 ----
        // 先 escape，再累积（段落末尾统一 renderInline）
        paragraph << escapeHtml(line);
    }

    // 收尾：处理文件末尾未关闭的代码块/段落/列表/引用/表格
    if (inCodeBlock) {
        // 代码块未闭合（用户输入不完整），按代码块输出
        QString escaped = escapeHtml(codeBlockContent);
        while (escaped.endsWith('\n')) escaped.chop(1);
        html << QStringLiteral("<pre style=\"%1\">").arg(codeBlockStyle)
             << escaped
             << QStringLiteral("</pre>");
    }
    flushParagraph();
    closeLists();
    flushBlockquote();
    flushTable();

    html << QStringLiteral("</body></html>");
    return html.join(QString());
}

QString markdownToHtml(const std::string& markdown, const QString& codeBlockBg) {
    return markdownToHtml(QString::fromUtf8(markdown.c_str()), codeBlockBg);
}

QString markdownToHtmlFragment(const QString& markdown, const QString& codeBlockBg) {
    QString full = markdownToHtml(markdown, codeBlockBg);
    // 剥离 <html><head>...</head><body>...</body></html> 包裹，返回 body 内部片段
    const QString headStart = QStringLiteral("<html><head>");
    const QString bodyStart = QStringLiteral("</head><body>");
    const QString bodyEnd = QStringLiteral("</body></html>");
    if (full.startsWith(headStart) && full.endsWith(bodyEnd)) {
        int bodyStartIdx = full.indexOf(bodyStart);
        if (bodyStartIdx >= 0) {
            int contentStart = bodyStartIdx + bodyStart.length();
            int contentEnd = full.length() - bodyEnd.length();
            return full.mid(contentStart, contentEnd - contentStart);
        }
    }
    // 兼容旧版无 <head> 的格式
    const QString prefix = QStringLiteral("<html><body>");
    const QString suffix = QStringLiteral("</body></html>");
    if (full.startsWith(prefix) && full.endsWith(suffix)) {
        return full.mid(prefix.length(), full.length() - prefix.length() - suffix.length());
    }
    return full;
}

QString markdownToHtmlFragment(const std::string& markdown, const QString& codeBlockBg) {
    return markdownToHtmlFragment(QString::fromUtf8(markdown.c_str()), codeBlockBg);
}

} // namespace MarkdownRenderer
