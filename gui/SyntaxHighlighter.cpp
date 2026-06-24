#include "gui/SyntaxHighlighter.h"
#include <QStringList>

// 静态成员定义（全局共享，避免每次 highlightBlock 重建）
// 匹配单行注释 //... 或块注释 /*...*/（同一行内的块注释）
QRegularExpression SyntaxHighlighter::commentRegex_("(//[^\\n]*|/\\*.*?\\*/)");
// HL-3 fix: 字符串正则支持单行和多行起始匹配
QRegularExpression SyntaxHighlighter::stringRegex_("\"(?:[^\"\\\\]|\\\\.)*\"?");

// ============================================================
// SyntaxHighlighter 语法高亮器实现
// ============================================================

SyntaxHighlighter::SyntaxHighlighter(QTextDocument* parent)
    : QSyntaxHighlighter(parent) {
    initRules();
}

void SyntaxHighlighter::setDarkTheme(bool dark) {
    isDarkTheme_ = dark;
    initRules();  // 重新初始化颜色规则
    rehighlight();  // 重新高亮整个文档
}

void SyntaxHighlighter::initRules() {
    if (isDarkTheme_) {
        // 深色主题（VS Code Dark+ 风格）
        keywordFormat_.setForeground(QColor(0x56, 0x9c, 0xd6));   // 蓝色
        keywordFormat_.setFontWeight(QFont::Bold);
        stringFormat_.setForeground(QColor(0xce, 0x91, 0x78));    // 橙棕色
        numberFormat_.setForeground(QColor(0xb5, 0xce, 0xa8));    // 浅绿色
        commentFormat_.setForeground(QColor(0x6a, 0x99, 0x55));   // 绿色
        commentFormat_.setFontItalic(true);
        operatorFormat_.setForeground(QColor(0xd4, 0xd4, 0xd4));  // 浅灰色
    } else {
        // 浅色主题（原有配色）
        keywordFormat_.setForeground(QColor(0, 0, 180));           // 蓝色
        keywordFormat_.setFontWeight(QFont::Bold);
        stringFormat_.setForeground(QColor(0, 128, 0));            // 绿色
        numberFormat_.setForeground(QColor(200, 100, 0));          // 橙色
        commentFormat_.setForeground(QColor(128, 128, 128));       // 灰色
        commentFormat_.setFontItalic(true);
        operatorFormat_.setForeground(QColor(128, 0, 128));        // 紫色
    }

    // ---- 添加高亮规则 ----

    // 关键字（合并为单个正则，减少匹配次数）
    HighlightRule keywordRule;
    keywordRule.pattern = QRegularExpression(
        "\\b(?:var|fun|function|func|if|else|while|for|return|print|break|continue"
        "|and|or|not|int|float|bool|string|class|extends|super|dict|array|null|true|false"
        "|try|catch|throw|import|from|export)\\b");
    keywordRule.format = keywordFormat_;
    rules_.push_back(keywordRule);

    // HL-2 + HL-4 fix: 数字字面量（整数、浮点数、前导点、尾点、科学计数法）
    HighlightRule numberRule;
    numberRule.pattern = QRegularExpression(
        "\\b\\d+\\.\\d+(?:[eE][+-]?\\d+)?\\b"  // 1.23, 1.23e5, 1.23e-5
        "|\\b\\d+\\.\\B(?:[eE][+-]?\\d+)?"      // 123., 123.e5
        "|\\B\\.\\d+(?:[eE][+-]?\\d+)?\\b"      // .123, .123e5
        "|\\b\\d+(?:[eE][+-]?\\d+)?\\b"          // 123, 123e5
    );
    numberRule.format = numberFormat_;
    rules_.push_back(numberRule);

    // 运算符（含分隔符，不再匹配字符串内内容——由 highlightBlock 逻辑控制）
    HighlightRule opRule;
    opRule.pattern = QRegularExpression("[+\\-*/%]|==|!=|<=|>=|<|>|=|\\(|\\)|\\{|\\}|\\[|\\]|;|,|:");
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
        // GUI-05 fix: 在本行任意位置搜索闭合引号（不要求从行首开始）
        int closePos = text.indexOf('"');
        if (closePos >= 0) {
            // 检查引号是否被转义：计算前面连续反斜杠数量
            int backslashCount = 0;
            for (int k = closePos - 1; k >= 0 && text[k] == '\\'; --k) {
                backslashCount++;
            }
            if (backslashCount % 2 == 0) {
                // 引号未被转义，字符串在此闭合
                int endPos = closePos + 1;
                stringRanges.append({0, endPos});
                startIndex = endPos;
                currentlyInString = false;
            } else {
                // 引号被转义，本行整个都在字符串内
                setFormat(0, text.length(), stringFormat_);
                setCurrentBlockState(1);
                return;
            }
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

            // 检查字符串是否闭合（以 " 结尾且引号未被转义）
            // M12 fix: 计算末尾连续反斜杠数量，偶数个则引号未转义（已闭合），奇数个则引号被转义（未闭合）
            if (captured.endsWith('"')) {
                int backslashCount = 0;
                for (int k = captured.length() - 2; k >= 0 && captured[k] == '\\'; --k) {
                    backslashCount++;
                }
                if (backslashCount % 2 == 0) {
                    // 引号未被转义，字符串已闭合
                    stringRanges.append({start, length});
                    searchPos = start + length;
                } else {
                    // 引号被转义，未闭合的多行字符串
                    stringRanges.append({start, text.length() - start});
                    currentlyInString = true;
                    break;
                }
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
