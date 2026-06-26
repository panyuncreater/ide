#include "gui/SyntaxHighlighter.h"
#include "lexer/Lexer.h"

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
    // P1 fix: 清空已有规则，避免 setDarkTheme 多次调用导致规则累积
    rules_.clear();
    keywordSet_.clear();

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

    // PERF-22 fix: 关键字用 QSet 替代大正则 alternation
    // C13 fix: 关键字列表从 Lexer::keywords() 单一来源派生
    for (const auto& kv : Lexer::keywords()) {
        keywordSet_.insert(QString::fromStdString(kv.first));
    }

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
    // P1 fix: 使用扫描器方式正确跟踪多行块注释状态
    // 块状态: 0 = 正常, 1 = 字符串内, 2 = 块注释内
    int pos = 0;
    int len = text.length();

    bool inString = (previousBlockState() == 1);
    bool inBlockComment = (previousBlockState() == 2);

    // PERF-22 fix: 用 per-character 掩码数组标记字符串/注释范围，
    // 将范围检查从 O(ranges) 线性扫描降为 O(1) 数组查找
    std::vector<char> mask;
    if (len > 0) mask.resize(len, 0);

    QList<QPair<int, int>> stringRanges;
    QList<QPair<int, int>> commentRanges;

    int stringStart = inString ? 0 : -1;
    int commentStart = inBlockComment ? 0 : -1;

    while (pos < len) {
        if (inBlockComment) {
            mask[pos] = 1;
            if (pos + 1 < len && text[pos] == '*' && text[pos + 1] == '/') {
                pos++;
                mask[pos] = 1;
                pos++;
                commentRanges.append({commentStart, pos - commentStart});
                inBlockComment = false;
                commentStart = -1;
                continue;
            }
            pos++;
            continue;
        }

        if (inString) {
            mask[pos] = 1;
            if (text[pos] == '\\' && pos + 1 < len) {
                pos++;
                mask[pos] = 1;
                pos++;
                continue;
            }
            if (text[pos] == '"') {
                pos++;
                stringRanges.append({stringStart, pos - stringStart});
                inString = false;
                stringStart = -1;
                continue;
            }
            pos++;
            continue;
        }

        if (pos + 1 < len && text[pos] == '/' && text[pos + 1] == '/') {
            commentRanges.append({pos, len - pos});
            for (int i = pos; i < len; ++i) mask[i] = 1;
            break;
        }

        if (pos + 1 < len && text[pos] == '/' && text[pos + 1] == '*') {
            commentStart = pos;
            inBlockComment = true;
            mask[pos] = 1;
            mask[pos + 1] = 1;
            pos += 2;
            continue;
        }

        if (text[pos] == '"') {
            stringStart = pos;
            inString = true;
            mask[pos] = 1;
            pos++;
            continue;
        }

        pos++;
    }

    if (inString && stringStart >= 0) {
        stringRanges.append({stringStart, len - stringStart});
    }
    if (inBlockComment && commentStart >= 0) {
        commentRanges.append({commentStart, len - commentStart});
    }

    // 应用字符串/注释格式
    for (const auto& range : stringRanges) {
        setFormat(range.first, range.second, stringFormat_);
    }
    for (const auto& range : commentRanges) {
        setFormat(range.first, range.second, commentFormat_);
    }

    // PERF-22 fix: 关键字用 QSet 查找替代大正则 globalMatch
    // 逐词扫描，检查 QSet membership (O(1) 平均) 替代 regex 回溯
    if (!keywordSet_.isEmpty()) {
        int wordStart = -1;
        for (int i = 0; i <= len; ++i) {
            bool isWordChar = (i < len) && (text[i].isLetterOrNumber() || text[i] == '_');
            if (isWordChar) {
                if (wordStart < 0) wordStart = i;
            } else {
                if (wordStart >= 0) {
                    int wordLen = i - wordStart;
                    // 关键字不会太长，跳过过长的标识符
                    if (wordLen <= 20 && !mask[wordStart]) {
                        if (keywordSet_.contains(text.mid(wordStart, wordLen))) {
                            setFormat(wordStart, wordLen, keywordFormat_);
                        }
                    }
                    wordStart = -1;
                }
            }
        }
    }

    // PERF-22 fix: 数字和运算符仍用 regex，但范围检查用 mask 数组 O(1) 替代线性扫描
    for (const auto& rule : rules_) {
        QRegularExpressionMatchIterator it = rule.pattern.globalMatch(text);
        while (it.hasNext()) {
            QRegularExpressionMatch match = it.next();
            int matchStart = match.capturedStart();

            // O(1) 掩码检查：matchStart 处的字符若在字符串/注释内则跳过
            if (matchStart < len && mask[matchStart]) continue;

            setFormat(matchStart, match.capturedLength(), rule.format);
        }
    }

    setCurrentBlockState(inString ? 1 : (inBlockComment ? 2 : 0));
}
