#include "gui/SyntaxHighlighter.h"
#include <QStringList>

// 静态成员定义（全局共享，避免每次 highlightBlock 重建）
QRegularExpression SyntaxHighlighter::commentRegex_("(//[^\\n]*)|(#[^\\n]*)");
// HL-3 fix: 字符串正则支持单行和多行起始匹配
QRegularExpression SyntaxHighlighter::stringRegex_("\"(?:[^\"\\\\]|\\\\.)*\"?");

// ============================================================
// SyntaxHighlighter 语法高亮器实现
// ============================================================

SyntaxHighlighter::SyntaxHighlighter(QTextDocument* parent)
    : QSyntaxHighlighter(parent) {
    initRules();
}

void SyntaxHighlighter::initRules() {
    // 关键字：蓝色粗体
    keywordFormat_.setForeground(QColor(0, 0, 180));
    keywordFormat_.setFontWeight(QFont::Bold);

    // 字符串：绿色
    stringFormat_.setForeground(QColor(0, 128, 0));

    // 数字：橙色
    numberFormat_.setForeground(QColor(200, 100, 0));

    // 注释：灰色斜体
    commentFormat_.setForeground(QColor(128, 128, 128));
    commentFormat_.setFontItalic(true);

    // 运算符：紫色
    operatorFormat_.setForeground(QColor(128, 0, 128));

    // 布尔值：蓝色粗体
    boolFormat_.setForeground(QColor(0, 0, 180));
    boolFormat_.setFontWeight(QFont::Bold);

    // ---- 添加高亮规则 ----

    // 关键字（合并为单个正则，减少匹配次数）
    HighlightRule keywordRule;
    keywordRule.pattern = QRegularExpression(
        "\\b(?:var|fun|function|func|if|else|while|for|return|print"
        "|and|or|not|int|float|bool|string|class|extends|super|dict|array|null|true|false)\\b");
    keywordRule.format = keywordFormat_;
    rules_.push_back(keywordRule);

    // HL-2 fix: 数字字面量（支持整数、浮点数、前导点 .123、尾点 123.）
    HighlightRule numberRule;
    numberRule.pattern = QRegularExpression("\\b\\d+\\.\\d+\\b|\\b\\d+\\.\\B|\\B\\.\\d+|\\b\\d+\\b");
    numberRule.format = numberFormat_;
    rules_.push_back(numberRule);

    // 运算符（含分隔符，不再匹配字符串内内容——由 highlightBlock 逻辑控制）
    HighlightRule opRule;
    opRule.pattern = QRegularExpression("[+\\-*/%]|==|!=|<=|>=|<|>|=|\\(|\\)|\\{|\\}|\\[|\\]|;|,|:|\\.");
    opRule.format = operatorFormat_;
    rules_.push_back(opRule);
}

void SyntaxHighlighter::highlightBlock(const QString& text) {
    // ---- HL-3 fix: 多行字符串状态传递 ----
    int startIndex = 0;
    bool inString = (previousBlockState() == 1);
    bool currentlyInString = false;

    // 收集本行中字符串覆盖的范围（这些范围内不应用其他高亮规则）
    QList<QPair<int, int>> stringRanges;  // {start, length}

    if (inString) {
        // 上一行有未闭合的字符串，继续匹配
        QRegularExpressionMatch endMatch = stringRegex_.match(text, 0);
        if (endMatch.hasMatch() && text[endMatch.capturedStart()] == '"') {
            // 本行找到了闭合引号
            int endPos = endMatch.capturedEnd();
            stringRanges.append({0, endPos});
            startIndex = endPos;
            currentlyInString = false;
        } else {
            // 本行整个都在字符串内
            setFormat(0, text.length(), stringFormat_);
            setCurrentBlockState(1);
            return;
        }
    }

    // 从 startIndex 开始查找本行中的字符串
    int searchPos = startIndex;
    while (searchPos < text.length()) {
        int quotePos = text.indexOf('"', searchPos);
        if (quotePos == -1) break;

        // 找到字符串起始，用正则匹配完整字符串
        QRegularExpressionMatch match = stringRegex_.match(text, quotePos);
        if (match.hasMatch()) {
            int start = match.capturedStart();
            int length = match.capturedLength();
            QString captured = match.captured();

            // 检查字符串是否闭合（以 " 结尾且不以 \" 结尾）
            if (captured.endsWith('"') && !captured.endsWith("\\\"")) {
                // 完整字符串
                stringRanges.append({start, length});
                searchPos = start + length;
            } else {
                // 未闭合的多行字符串
                stringRanges.append({start, text.length() - start});
                currentlyInString = true;
                break;
            }
        } else {
            searchPos = quotePos + 1;
        }
    }

    // 先应用字符串高亮（优先级最高，覆盖其他格式）
    for (const auto& range : stringRanges) {
        setFormat(range.first, range.second, stringFormat_);
    }

    // 检查注释
    QRegularExpressionMatch commentMatch = commentRegex_.match(text);
    int commentStart = commentMatch.hasMatch() ? commentMatch.capturedStart() : text.length();

    // 修复：如果注释起始位置在字符串范围内，则忽略（如 "http://example.com"）
    for (const auto& range : stringRanges) {
        if (commentStart >= range.first && commentStart < range.first + range.second) {
            commentStart = text.length();
            break;
        }
    }

    // 应用其他规则（跳过字符串范围内的匹配）
    for (const auto& rule : rules_) {
        if (rule.format == stringFormat_) continue;  // 字符串已处理

        QRegularExpressionMatchIterator it = rule.pattern.globalMatch(text);
        while (it.hasNext()) {
            QRegularExpressionMatch match = it.next();
            int matchStart = match.capturedStart();
            int matchEnd = match.capturedEnd();

            // 跳过注释区域内的匹配
            if (matchStart >= commentStart) continue;

            // 跳过字符串范围内的匹配 (HL-1 fix)
            bool inStringRange = false;
            for (const auto& range : stringRanges) {
                if (matchStart >= range.first && matchEnd <= range.first + range.second) {
                    inStringRange = true;
                    break;
                }
            }
            if (inStringRange) continue;

            setFormat(matchStart, match.capturedLength(), rule.format);
        }
    }

    // 注释高亮（最高优先级，覆盖一切）
    if (commentMatch.hasMatch()) {
        setFormat(commentStart, commentMatch.capturedLength(), commentFormat_);
    }

    // HL-3: 设置块状态以支持多行字符串
    setCurrentBlockState(currentlyInString ? 1 : 0);
}
