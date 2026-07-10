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
// P2-2 fix (F3/F4) 增强：
//   - 标题生成 anchor id（slug），支持章节锚点目录跳转
//   - 围栏代码块 ```minilang / ```ml / ```mlang 触发 MiniLang 语法高亮
//     （关键字/字符串/注释/数字着色，基于简单正则，不调用真实 Lexer）
//
// CSS 样式注入：输出 HTML 包含 <style> 块，让标题/列表/代码块/表格/引用块
// 有美观的视觉效果（颜色、间距、圆角、边框等），无需调用方额外加样式。
// ============================================================

#include "gui/MarkdownRenderer.h"

#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QStringList>

namespace MarkdownRenderer {

// ---- 内部辅助 ----

// PERF: 正则表达式编译为 static const，避免每次 markdownToHtml 调用都重新编译。
// QRegularExpression 构造会调用 pcre2_compile，单次约 10-50μs，多个正则累计可省数百 μs/次。
// 对 LabManualPanel 章节切换、各教学面板 detail 刷新有显著收益。
static const QRegularExpression kReInlineCode(QStringLiteral("`([^`]+)`"));
static const QRegularExpression kReBold(QStringLiteral("\\*\\*([^*]+)\\*\\*"));
static const QRegularExpression kReItalic(QStringLiteral("(?<!\\*)\\*([^*]+)\\*(?!\\*)"));
// P1-4 fix (F16): 行内链接 [text](url) — 支持 panel: 协议跳转教学面板
static const QRegularExpression kReLink(QStringLiteral("\\[([^\\]]+)\\]\\(([^)]+)\\)"));
static const QRegularExpression kReHeading(QStringLiteral("^(#{1,6})\\s+(.+)$"));
static const QRegularExpression kReUnorderedList(QStringLiteral("^[-*]\\s+(.+)$"));
static const QRegularExpression kReOrderedList(QStringLiteral("^\\d+\\.\\s+(.+)$"));
// 任务列表：- [ ] 或 - [x] / - [X]
static const QRegularExpression kReTaskList(QStringLiteral("^[-*]\\s+\\[([ xX])\\]\\s+(.+)$"));
// 引用块：> text
static const QRegularExpression kReBlockquote(QStringLiteral("^>\\s?(.*)$"));
// 表格行：| cell | cell |
static const QRegularExpression kReTableRow(QStringLiteral("^\\|(.+)\\|$"));

// P2-2 fix (F4): MiniLang 语法高亮正则
// 关键字列表（与 lexer/Keywords.cpp 保持一致，按字母序，便于审阅）
static const QStringList kMiniLangKeywords = {
    QStringLiteral("var"),     QStringLiteral("const"),    QStringLiteral("fun"),    QStringLiteral("return"),
    QStringLiteral("if"),      QStringLiteral("else"),     QStringLiteral("while"),  QStringLiteral("for"),
    QStringLiteral("break"),   QStringLiteral("continue"), QStringLiteral("true"),   QStringLiteral("false"),
    QStringLiteral("null"),    QStringLiteral("and"),      QStringLiteral("or"),     QStringLiteral("not"),
    QStringLiteral("print"),   QStringLiteral("input"),    QStringLiteral("class"),  QStringLiteral("super"),
    QStringLiteral("this"),    QStringLiteral("init"),     QStringLiteral("try"),    QStringLiteral("catch"),
    QStringLiteral("finally"), QStringLiteral("throw"),    QStringLiteral("import"), QStringLiteral("export"),
    QStringLiteral("as"),      QStringLiteral("in"),       QStringLiteral("is")};
// 字符串字面量："..." 或 '...'
static const QRegularExpression kReMlString(QStringLiteral("\"([^\"\\\\]|\\\\.)*\"|'([^'\\\\]|\\\\.)*'"));
// 行注释 // ...（保留到行尾）
static const QRegularExpression kReMlLineComment(QStringLiteral("//[^\\n]*"));
// 块注释 /* ... */（含换行，非贪婪）
static const QRegularExpression kReMlBlockComment(QStringLiteral("/\\*[\\s\\S]*?\\*/"));
// 数字字面量（整数 / 浮点 / 0x 十六进制）
static const QRegularExpression kReMlNumber(QStringLiteral("\\b\\d+(\\.\\d+)?([eE][+-]?\\d+)?\\b|0x[0-9a-fA-F]+"));

/// P2-2 fix (F3): 把标题文本转为 HTML anchor id（slug）
/// 规则：非字母数字字符 → 连字符；保留中文；小写；去首尾连字符
static QString slugify(const QString& text) {
    QString slug;
    for (const QChar& c : text) {
        if (c.isLetterOrNumber()) {
            slug += c.toLower();
        } else if (c == '_' || c == '-' || c.isSpace()) {
            if (!slug.isEmpty() && !slug.endsWith('-'))
                slug += '-';
        }
        // 其他字符忽略
    }
    while (slug.startsWith('-'))
        slug.remove(0, 1);
    while (slug.endsWith('-'))
        slug.chop(1);
    return slug;
}

/// P2-2 fix (F4): 对 MiniLang 代码做简单语法高亮
/// 输入应已 escapeHtml 转义过。基于正则顺序替换：注释 > 字符串 > 关键字 > 数字
/// 用 <span class="..."> 包裹，CSS 类定义在 buildStylesheet 中
///
/// R51-1 fix: 关键字 "class" 会腐蚀已生成的 span 标签的 class="..." 属性
/// （\bclass\b 匹配属性中的 class）。改用"提取-替换-还原"法：注释/字符串
/// 标记后先提取到 spans 列表并用唯一占位符替换，关键字/数字替换仅作用于
/// 剩余纯文本，最后还原占位符。占位符使用 \uE000-\uE0FF 私用区字符（不会
/// 出现在 MiniLang 源码中，避免与关键字冲突）。
static QString highlightMiniLang(const QString& escapedCode) {
    QString result = escapedCode;
    QStringList spans;

    // 占位符：\uE000 + 序号 + \uE001，私用区字符不会出现在源码或关键字中
    auto makePlaceholder = [&](int idx) { return QChar(0xE000) + QString::number(idx) + QChar(0xE001); };
    auto replaceWithPlaceholder = [&](const QRegularExpression& re, const QString& cls) {
        int from = 0;
        QRegularExpressionMatch m;
        while ((m = re.match(result, from)).hasMatch()) {
            QString captured = m.captured(0);
            int idx = static_cast<int>(spans.size());
            spans.push_back(QStringLiteral("<span class=\"%1\">%2</span>").arg(cls, captured));
            QString placeholder = makePlaceholder(idx);
            result.replace(m.capturedStart(), m.capturedLength(), placeholder);
            from = m.capturedStart() + placeholder.length();
        }
    };

    // 1. 注释（先处理，避免注释内的关键字/字符串被误高亮）
    replaceWithPlaceholder(kReMlBlockComment, QStringLiteral("ml-comment"));
    replaceWithPlaceholder(kReMlLineComment, QStringLiteral("ml-comment"));

    // 2. 字符串
    replaceWithPlaceholder(kReMlString, QStringLiteral("ml-string"));

    // 3. 关键字（用 \b 边界避免误命中标识符子串）
    // 此时注释/字符串已被替换为占位符，关键字正则不会匹配到它们的内容
    for (const QString& kw : kMiniLangKeywords) {
        QRegularExpression re(QStringLiteral("\\b%1\\b").arg(kw));
        result.replace(re, QStringLiteral("<span class=\"ml-keyword\">%1</span>").arg(kw));
    }

    // 4. 数字
    result.replace(kReMlNumber, QStringLiteral("<span class=\"ml-number\">\\0</span>"));

    // 5. 还原占位符
    for (int i = static_cast<int>(spans.size()) - 1; i >= 0; --i) {
        result.replace(makePlaceholder(i), spans[i]);
    }

    return result;
}

/// 检测表格分隔行：行只包含 | - : 空白，且至少有一个 -
/// 避免使用字符类正则（[\s:-|] 中的 - 会被解释为范围）
static bool isTableSeparatorLine(const QString& line) {
    QString t = line.trimmed();
    if (t.isEmpty())
        return false;
    bool hasDash = false;
    for (const QChar& c : t) {
        if (c == '|' || c == ':')
            continue;
        if (c == '-') {
            hasDash = true;
            continue;
        }
        if (c.isSpace())
            continue;
        return false; // 包含其他字符，不是分隔行
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
    // P1-4 fix (F16): 行内链接 [text](url) — 最后处理，避免 code/bold/italic
    // 内的方括号被误匹配。url 中的 & 已被 escapeHtml 转成 &amp;，
    // 需在嵌入 href 时反转回来（HTML 属性值中 &amp; 合法但 QTextBrowser
    // 解析 href 时会再次 unescape，导致 &amp;amp; 双重转义）。
    QRegularExpressionMatch it;
    int pos = 0;
    QString result;
    while ((it = kReLink.match(out, pos)).hasMatch()) {
        result += out.mid(pos, it.capturedStart() - pos);
        QString text = it.captured(1);
        QString url = it.captured(2);
        url.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
        // R51-6 fix: URL 中的双引号会破坏 href 属性边界，需转义为 &quot;
        url.replace(QChar('"'), QStringLiteral("&quot;"));
        result += QStringLiteral("<a href=\"%1\">%2</a>").arg(url, text);
        pos = it.capturedEnd();
    }
    result += out.mid(pos);
    return result;
}

/// 生成嵌入 HTML 的 <style> 块，让 Markdown 输出有美观的视觉效果。
/// 颜色方案与 TeachingTheme 保持一致（亮色主题）。
static QString buildStylesheet(const QString& codeBlockBg) {
    static const QString kStylesheet = QStringLiteral(R"(
        <style>
        body { font-family: 'Segoe UI', 'Microsoft YaHei', sans-serif; font-size: 14px; color: #1E1E1E; line-height: 1.6; }
        h1 { font-size: 22px; color: #268BD2; border-bottom: 2px solid #E0E0E0; padding-bottom: 6px; margin: 16px 0 10px; font-weight: 600; }
        h2 { font-size: 18px; color: #0078d4; border-bottom: 1px solid #E0E0E0; padding-bottom: 4px; margin: 14px 0 8px; font-weight: 600; }
        h3 { font-size: 16px; color: #0078d4; margin: 12px 0 6px; font-weight: 600; }
        h4 { font-size: 14px; color: #073642; margin: 10px 0 4px; font-weight: 600; }
        h5, h6 { font-size: 13px; color: #5A5A5A; margin: 8px 0 4px; font-weight: 600; }
        p { margin: 6px 0; }
        ul, ol { margin: 6px 0; padding-left: 24px; }
        li { margin: 3px 0; }
        code { background: #F5F5F5; color: #DC322F; padding: 2px 5px; border-radius: 3px; font-family: Consolas, 'Courier New', monospace; font-size: 13px; }
        pre { background: %1; padding: 10px 12px; border-radius: 6px; border: 1px solid #E0E0E0; font-family: Consolas, 'Courier New', monospace; font-size: 13px; white-space: pre-wrap; margin: 8px 0; }
        pre code { background: transparent; color: #1E1E1E; padding: 0; border-radius: 0; font-size: 13px; }
        blockquote { border-left: 4px solid #268BD2; background: #F5F5F5; padding: 8px 12px; margin: 8px 0; color: #5A5A5A; border-radius: 0 4px 4px 0; }
        blockquote p { margin: 4px 0; }
        hr { border: none; border-top: 1px solid #E0E0E0; margin: 12px 0; }
        table { border-collapse: collapse; width: 100%; margin: 8px 0; font-size: 13px; }
        th { background: #F5F5F5; border: 1px solid #E0E0E0; padding: 6px 10px; text-align: left; font-weight: 600; color: #1E1E1E; }
        td { border: 1px solid #E0E0E0; padding: 6px 10px; vertical-align: top; }
        tr:nth-child(even) td { background: #FFFFFF; }
        .task-list-item { list-style: none; margin-left: -18px; }
        .task-checkbox { display: inline-block; width: 14px; height: 14px; border: 1.5px solid #8C8C8C; border-radius: 2px; margin-right: 6px; vertical-align: middle; }
        .task-checkbox.checked { background: #859900; border-color: #859900; color: white; text-align: center; font-size: 10px; line-height: 14px; }
        b, strong { color: #1E1E1E; font-weight: 600; }
        i, em { color: #5A5A5A; }
        /* P2-2 fix (F4): MiniLang 语法高亮配色（中性风格） */
        .ml-keyword { color: #859900; font-weight: 600; }
        .ml-string { color: #2AA198; }
        .ml-comment { color: #E0E0E0; font-style: italic; }
        .ml-number { color: #D33682; }
        /* P2-2 fix (F3): 章节锚点目录样式 */
        .toc-box { background: #F5F5F5; border-left: 3px solid #268BD2; padding: 8px 12px; margin: 8px 0; border-radius: 0 4px 4px 0; }
        .toc-title { font-weight: 600; color: #073642; margin-bottom: 4px; font-size: 13px; }
        .toc-list { margin: 0; padding-left: 16px; font-size: 13px; }
        .toc-list li { margin: 2px 0; }
        .toc-list a { color: #268BD2; text-decoration: none; }
        .toc-list a:hover { text-decoration: underline; }
        </style>
    )");
    return kStylesheet.arg(codeBlockBg.isEmpty() ? QStringLiteral("#F5F5F5") : codeBlockBg);
}

// ---- 公共 API ----

QString markdownToHtml(const QString& markdown, const QString& codeBlockBg) {
    if (markdown.isEmpty()) {
        return QStringLiteral("<html><head></head><body></body></html>");
    }

    const QString bg = codeBlockBg.isEmpty() ? QStringLiteral("#F5F5F5") : codeBlockBg;
    const QString codeBlockStyle = QStringLiteral("background:%1; padding:10px 12px; border-radius:6px; "
                                                  "border:1px solid #E0E0E0; "
                                                  "font-family:Consolas, 'Courier New', monospace; "
                                                  "font-size:13px; "
                                                  "white-space:pre-wrap;")
                                       .arg(bg);

    // 按行扫描，识别块级结构
    const QStringList lines = markdown.split('\n');
    QStringList html;
    html << QStringLiteral("<html><head>");
    html << buildStylesheet(bg);
    html << QStringLiteral("</head><body>");

    bool inCodeBlock = false;
    QString codeBlockContent;
    QString codeBlockLang;
    bool inUl = false;           // 无序列表
    bool inOl = false;           // 有序列表
    bool inBlockquote = false;   // 引用块
    QStringList blockquoteLines; // 引用块累积行
    QStringList paragraph;       // 当前段落累积的行

    // 表格状态
    bool inTable = false;
    QStringList tableHeader; // 表头单元格
    QStringList tableRows;   // 数据行（每行一个字符串列表）

    auto closeLists = [&]() {
        if (inUl) {
            html << QStringLiteral("</ul>");
            inUl = false;
        }
        if (inOl) {
            html << QStringLiteral("</ol>");
            inOl = false;
        }
    };
    auto flushParagraph = [&]() {
        if (!paragraph.isEmpty()) {
            QString joined = paragraph.join(QStringLiteral("\n"));
            html << QStringLiteral("<p>") << renderInline(joined) << QStringLiteral("</p>");
            paragraph.clear();
        }
    };
    auto flushBlockquote = [&]() {
        if (!blockquoteLines.isEmpty()) {
            QString joined = blockquoteLines.join(QStringLiteral("<br>"));
            html << QStringLiteral("<blockquote>") << renderInline(joined) << QStringLiteral("</blockquote>");
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
                // R51-3 fix: 表格单元格需先 escapeHtml 再 renderInline（对齐列表/标题/引用块）
                html << QStringLiteral("<th>") << renderInline(escapeHtml(h.trimmed())) << QStringLiteral("</th>");
            }
            html << QStringLiteral("</tr>");
            // 数据行
            for (const auto& row : tableRows) {
                QStringList cells = row.split(QStringLiteral("|"));
                html << QStringLiteral("<tr>");
                for (const auto& c : cells) {
                    html << QStringLiteral("<td>") << renderInline(escapeHtml(c.trimmed())) << QStringLiteral("</td>");
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
                while (escaped.endsWith('\n'))
                    escaped.chop(1);
                // P2-2 fix (F4): MiniLang 语法高亮
                // 触发条件：codeBlockLang 为 minilang / ml / mlang / mini（不区分大小写）
                QString lowerLang = codeBlockLang.toLower();
                bool isMiniLang = (lowerLang == QStringLiteral("minilang") || lowerLang == QStringLiteral("ml") ||
                                   lowerLang == QStringLiteral("mlang") || lowerLang == QStringLiteral("mini"));
                if (isMiniLang) {
                    escaped = highlightMiniLang(escaped);
                }
                // 代码块语言标签（仅作为注释显示在代码上方，不渲染为单独元素）
                if (!codeBlockLang.isEmpty()) {
                    html << QStringLiteral("<div style=\"font-size:11px;color:#999;margin-bottom:2px;\">")
                         << escapeHtml(codeBlockLang) << QStringLiteral("</div>");
                }
                html << QStringLiteral("<pre style=\"%1\">").arg(codeBlockStyle) << escaped << QStringLiteral("</pre>");
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
            QString rawText = hm.captured(2);
            // P2-2 fix (F3): 标题生成 anchor id（slug），用于章节锚点目录跳转
            QString anchor = slugify(rawText);
            QString text = renderInline(escapeHtml(rawText));
            if (!anchor.isEmpty()) {
                html << QStringLiteral("<h%1 id=\"%2\">%3</h%1>").arg(level).arg(anchor, text);
            } else {
                html << QStringLiteral("<h%1>%2</h%1>").arg(level).arg(text);
            }
            continue;
        }

        // ---- 任务列表 - [ ] / - [x] ----
        QRegularExpressionMatch tm = kReTaskList.match(line);
        if (tm.hasMatch()) {
            flushParagraph();
            if (inOl) {
                html << QStringLiteral("</ol>");
                inOl = false;
            }
            if (!inUl) {
                html << QStringLiteral("<ul>");
                inUl = true;
            }
            QString checked = tm.captured(1).toLower();
            // R51-7 fix: 移除未使用的 text 变量，直接在输出处调用 renderInline
            QString checkboxClass = (checked == QStringLiteral("x")) ? QStringLiteral("task-checkbox checked")
                                                                     : QStringLiteral("task-checkbox");
            QString checkboxSymbol = (checked == QStringLiteral("x")) ? QStringLiteral("✓") : QString();
            html << QStringLiteral("<li class=\"task-list-item\">")
                 << QStringLiteral("<span class=\"%1\">%2</span>").arg(checkboxClass, checkboxSymbol)
                 << renderInline(escapeHtml(tm.captured(2))) << QStringLiteral("</li>");
            continue;
        }

        // ---- 无序列表 - / * 开头（避免与粗体冲突：要求行首是 - 或 *，后跟空格） ----
        QRegularExpressionMatch ulm = kReUnorderedList.match(line);
        if (ulm.hasMatch()) {
            flushParagraph();
            flushBlockquote();
            flushTable();
            if (inOl) {
                html << QStringLiteral("</ol>");
                inOl = false;
            }
            if (!inUl) {
                html << QStringLiteral("<ul>");
                inUl = true;
            }
            html << QStringLiteral("<li>") << renderInline(escapeHtml(ulm.captured(1))) << QStringLiteral("</li>");
            continue;
        }

        // ---- 有序列表 1. / 2. 开头 ----
        QRegularExpressionMatch olm = kReOrderedList.match(line);
        if (olm.hasMatch()) {
            flushParagraph();
            flushBlockquote();
            flushTable();
            if (inUl) {
                html << QStringLiteral("</ul>");
                inUl = false;
            }
            if (!inOl) {
                html << QStringLiteral("<ol>");
                inOl = true;
            }
            html << QStringLiteral("<li>") << renderInline(escapeHtml(olm.captured(1))) << QStringLiteral("</li>");
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
            bool isNextSeparator = (i + 1 < lines.size()) && isTableSeparatorLine(lines[i + 1]);
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
        while (escaped.endsWith('\n'))
            escaped.chop(1);
        html << QStringLiteral("<pre style=\"%1\">").arg(codeBlockStyle) << escaped << QStringLiteral("</pre>");
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

// ============================================================
// P2-2 fix (F3): 章节锚点目录（TOC）
// ============================================================

std::vector<HeadingEntry> extractHeadings(const QString& markdown) {
    std::vector<HeadingEntry> headings;
    if (markdown.isEmpty())
        return headings;

    const QStringList lines = markdown.split('\n');
    bool inCodeBlock = false;
    for (const QString& line : lines) {
        // 跟踪围栏代码块状态，代码块内的 # 不算标题
        if (line.trimmed().startsWith(QStringLiteral("```"))) {
            inCodeBlock = !inCodeBlock;
            continue;
        }
        if (inCodeBlock)
            continue;

        QRegularExpressionMatch hm = kReHeading.match(line);
        if (hm.hasMatch()) {
            HeadingEntry e;
            e.level = hm.captured(1).length();
            e.text = hm.captured(2);
            e.anchor = slugify(e.text);
            if (!e.anchor.isEmpty()) {
                headings.push_back(std::move(e));
            }
        }
    }
    return headings;
}

QString buildTableOfContents(const QString& markdown, const QString& tocTitle, int maxLevel) {
    auto headings = extractHeadings(markdown);
    if (headings.empty())
        return QString();

    QStringList html;
    html << QStringLiteral("<div class=\"toc-box\">");
    if (!tocTitle.isEmpty()) {
        html << QStringLiteral("<div class=\"toc-title\">") << escapeHtml(tocTitle) << QStringLiteral("</div>");
    }
    html << QStringLiteral("<ul class=\"toc-list\">");
    for (const auto& h : headings) {
        if (h.level > maxLevel)
            continue;
        // 根据级别缩进（h1 不缩进，h2 缩进 1 级，h3 缩进 2 级）
        int indent = h.level - 1;
        if (indent < 0)
            indent = 0;
        QString style = (indent > 0) ? QStringLiteral(" style=\"margin-left:%1px;\"").arg(indent * 12) : QString();
        // 内部锚点链接使用 #anchor 格式（QTextBrowser 支持）
        html << QStringLiteral("<li%1><a href=\"#%2\">%3</a></li>")
                    .arg(style, h.anchor, renderInline(escapeHtml(h.text)));
    }
    html << QStringLiteral("</ul>");
    html << QStringLiteral("</div>");
    return html.join(QString());
}

} // namespace MarkdownRenderer
