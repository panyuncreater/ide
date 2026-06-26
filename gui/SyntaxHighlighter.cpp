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

    // PERF-22 fix: 数字和运算符的识别已完全移入 highlightBlock 的单遍扫描器，
    // 不再需要 QRegularExpression 规则。rules_ 向量保持空（保留成员以维持 ABI 兼容）。
    // 原 2 次 globalMatch 调用（数字正则 + 运算符正则）被消除，复杂度从
    // O(n × regex回溯) 降为 O(n) 单遍字符扫描。
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

    // ---- 第一遍：扫描字符串/注释，填充 mask + ranges ----
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

    // ---- 第二遍：单遍扫描识别关键字/数字/运算符，完全消除 regex globalMatch ----
    // PERF-22 fix: 原实现用 2 次 QRegularExpression::globalMatch（数字正则 + 运算符正则），
    // 每次匹配都需 regex 引擎回溯 + 对每个匹配检查 mask。
    // 改造后：单遍字符扫描，按首字符分派到 keyword/number/operator 分支，
    // 总复杂度从 O(n × regex回溯) 降为 O(n)。
    pos = 0;
    while (pos < len) {
        // 跳过字符串/注释内的字符（mask 标记）
        if (mask[pos]) {
            pos++;
            continue;
        }

        QChar c = text[pos];

        // ---- 标识符/关键字 ----
        // 首字符：字母或下划线；后续：字母/数字/下划线
        if (c.isLetter() || c == '_') {
            int start = pos;
            while (pos < len && (text[pos].isLetterOrNumber() || text[pos] == '_')) {
                pos++;
            }
            int wordLen = pos - start;
            // 关键字不会太长，跳过过长的标识符
            if (wordLen <= 20 && !keywordSet_.isEmpty() &&
                keywordSet_.contains(text.mid(start, wordLen))) {
                setFormat(start, wordLen, keywordFormat_);
            }
            continue;
        }

        // ---- 数字字面量 ----
        // 支持：整数(123)、浮点(1.23)、科学计数(1e5/1.23e-5)、
        //       前导点(.123)、尾点(123.)
        // 判定条件：首字符是数字，或者首字符是 '.' 且下一个字符是数字
        if (c.isDigit() || (c == '.' && pos + 1 < len && text[pos + 1].isDigit())) {
            int start = pos;
            bool hasDot = false;
            bool hasExp = false;

            // 整数部分（如果有，前导点场景无整数部分）
            while (pos < len && text[pos].isDigit()) {
                pos++;
            }

            // 小数点 + 小数部分
            if (pos < len && text[pos] == '.' && !hasDot) {
                hasDot = true;
                pos++;
                while (pos < len && text[pos].isDigit()) {
                    pos++;
                }
            }

            // 指数部分 e/E[+-]?digits
            if (pos < len && (text[pos] == 'e' || text[pos] == 'E')) {
                int expPos = pos;
                pos++;
                if (pos < len && (text[pos] == '+' || text[pos] == '-')) {
                    pos++;
                }
                if (pos < len && text[pos].isDigit()) {
                    hasExp = true;
                    while (pos < len && text[pos].isDigit()) {
                        pos++;
                    }
                } else {
                    // 回退：'e' 后无数字，不是合法指数，回退到 e 前
                    pos = expPos;
                }
            }

            setFormat(start, pos - start, numberFormat_);
            continue;
        }

        // ---- 运算符/分隔符 ----
        // 两字符运算符优先匹配：== != <= >=
        // 单字符运算符：+ - * / % < > = ( ) { } [ ] ; , :
        if (pos + 1 < len) {
            QChar c2 = text[pos + 1];
            if ((c == '=' && c2 == '=') || (c == '!' && c2 == '=') ||
                (c == '<' && c2 == '=') || (c == '>' && c2 == '=')) {
                setFormat(pos, 2, operatorFormat_);
                pos += 2;
                continue;
            }
        }
        if (c == '+' || c == '-' || c == '*' || c == '/' || c == '%' ||
            c == '<' || c == '>' || c == '=' ||
            c == '(' || c == ')' || c == '{' || c == '}' ||
            c == '[' || c == ']' || c == ';' || c == ',' || c == ':') {
            setFormat(pos, 1, operatorFormat_);
            pos++;
            continue;
        }

        // 其他字符（空白等），跳过
        pos++;
    }

    setCurrentBlockState(inString ? 1 : (inBlockComment ? 2 : 0));
}
