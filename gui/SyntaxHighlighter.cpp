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
        // 深色主题（VS Code Dark+ 2024 风格）
        keywordFormat_.setForeground(QColor(0x56, 0x9c, 0xd6));   // 蓝色关键字
        keywordFormat_.setFontWeight(QFont::Bold);
        stringFormat_.setForeground(QColor(0xce, 0x91, 0x78));    // 橙棕色字符串
        numberFormat_.setForeground(QColor(0xb5, 0xce, 0xa8));    // 浅绿色数字
        commentFormat_.setForeground(QColor(0x6a, 0x99, 0x55));   // 绿色注释
        commentFormat_.setFontItalic(true);
        operatorFormat_.setForeground(QColor(0xd4, 0xd4, 0xd4));  // 浅灰色运算符
    } else {
        // 浅色主题（VS Code Light+ 2024 风格 — 更鲜艳、层次更清晰）
        keywordFormat_.setForeground(QColor(0x26, 0x8b, 0xd2));   // 深蓝色关键字
        keywordFormat_.setFontWeight(QFont::Bold);
        stringFormat_.setForeground(QColor(0x2a, 0xa1, 0x98));    // 深绿色字符串
        numberFormat_.setForeground(QColor(0xb5, 0x89, 0x00));     // 绿色数字
        commentFormat_.setForeground(QColor(0x58, 0x6e, 0x75));     // 绿色注释
        commentFormat_.setFontItalic(true);
        operatorFormat_.setForeground(QColor(0x65, 0x7b, 0x83));   // 深灰色运算符
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
    // 状态编码:
    //   0: 正常
    //   1: 字符串体内
    //   2: 块注释（向后兼容，深度=1）
    //   100 + depth (depth >= 1): 嵌套块注释（支持 /* /* */ */ 嵌套）
    //   200 + braceDepth (braceDepth >= 1): 字符串插值表达式内（支持跨行插值）
    //   300 + braceDepth*10 + blockCommentDepth: 插值表达式内且处于块注释中
    //       （BUG-SH-1 fix：原编码 blockCommentDepth > 0 优先于 interpBraceDepth > 0，
    //       导致插值内块注释跨行时 interpBraceDepth 上下文丢失）
    //   400 + braceDepth (braceDepth >= 1): 插值表达式内的嵌套字符串体内（跨行）
    //       （BUG-SH-2 fix：原实现嵌套字符串为单行扫描，未闭合时下一行错误处理）
    int pos = 0;
    int len = text.length();

    int prevState = previousBlockState();
    if (prevState < 0) prevState = 0;

    bool inString = false;
    bool inNestedString = false;  // BUG-SH-2: 插值内嵌套字符串跨行
    int blockCommentDepth = 0;
    int interpBraceDepth = 0;
    // 解码顺序：高范围优先（400 > 300 > 200 > 100），避免误匹配
    if (prevState == 1) inString = true;
    else if (prevState == 2) blockCommentDepth = 1;  // 向后兼容
    else if (prevState >= 400) { inNestedString = true; interpBraceDepth = prevState - 400; }
    else if (prevState >= 300) {
        // BUG-SH-1 fix: 插值内块注释组合状态
        interpBraceDepth = (prevState - 300) / 10;
        blockCommentDepth = (prevState - 300) % 10;
    }
    else if (prevState >= 200) interpBraceDepth = prevState - 200;
    else if (prevState >= 100) blockCommentDepth = prevState - 100;

    // PERF-22 fix: 用 per-character 掩码数组标记字符串/注释范围，
    // 将范围检查从 O(ranges) 线性扫描降为 O(1) 数组查找
    // Perf-Finding3: thread_local 复用底层数组容量，避免每次按键的堆分配。
    static thread_local std::vector<char> mask;
    mask.clear();
    if (len > 0) mask.resize(len, 0);

    QList<QPair<int, int>> stringRanges;
    QList<QPair<int, int>> commentRanges;

    int stringStart = inString ? 0 : -1;
    int commentStart = (blockCommentDepth > 0) ? 0 : -1;
    // BUG-SH-2 fix: 嵌套字符串跨行时，起始位置为行首（0）
    int nestedStringStart = inNestedString ? 0 : -1;

    // ---- 第一遍：扫描字符串/注释，填充 mask + ranges ----
    while (pos < len) {
        // ---- 块注释内（depth > 0）：跟踪嵌套 /* */ ----
        if (blockCommentDepth > 0) {
            mask[pos] = 1;
            if (pos + 1 < len && text[pos] == '/' && text[pos + 1] == '*') {
                mask[pos + 1] = 1;
                pos += 2;
                blockCommentDepth++;
                continue;
            }
            if (pos + 1 < len && text[pos] == '*' && text[pos + 1] == '/') {
                mask[pos + 1] = 1;
                pos += 2;
                blockCommentDepth--;
                if (blockCommentDepth == 0) {
                    commentRanges.append({commentStart, pos - commentStart});
                    commentStart = -1;
                }
                continue;
            }
            pos++;
            continue;
        }

        // ---- 字符串插值表达式内（braceDepth > 0）：扫描为代码 ----
        if (interpBraceDepth > 0) {
            // BUG-SH-2 fix: 处理跨行嵌套字符串（从上一行延续的未闭合嵌套字符串）
            if (inNestedString) {
                while (pos < len) {
                    mask[pos] = 1;
                    if (text[pos] == '\\' && pos + 1 < len) {
                        mask[pos + 1] = 1;
                        pos += 2;
                        continue;
                    }
                    if (text[pos] == '"') {
                        pos++;
                        inNestedString = false;
                        break;
                    }
                    pos++;
                }
                stringRanges.append({nestedStringStart, pos - nestedStringStart});
                continue;
            }
            // 嵌套字符串：扫描至闭合 " 或行末
            if (text[pos] == '"') {
                nestedStringStart = pos;
                mask[pos] = 1;
                pos++;
                bool closed = false;
                while (pos < len) {
                    mask[pos] = 1;
                    if (text[pos] == '\\' && pos + 1 < len) {
                        mask[pos + 1] = 1;
                        pos += 2;
                        continue;
                    }
                    if (text[pos] == '"') {
                        pos++;
                        closed = true;
                        break;
                    }
                    pos++;
                }
                // BUG-SH-2 fix: 未闭合则标记跨行状态，下一行继续扫描
                if (!closed) {
                    inNestedString = true;
                }
                stringRanges.append({nestedStringStart, pos - nestedStringStart});
                continue;
            }
            // 嵌套块注释
            if (pos + 1 < len && text[pos] == '/' && text[pos + 1] == '*') {
                commentStart = pos;
                mask[pos] = 1; mask[pos + 1] = 1;
                pos += 2;
                blockCommentDepth = 1;
                continue;
            }
            if (text[pos] == '{') {
                interpBraceDepth++;
                pos++;
                continue;
            }
            if (text[pos] == '}') {
                interpBraceDepth--;
                if (interpBraceDepth == 0) {
                    mask[pos] = 1;  // } 标记为字符串颜色（插值分隔符）
                    pos++;
                    inString = true;
                    stringStart = pos;
                } else {
                    pos++;
                }
                continue;
            }
            pos++;
            continue;
        }

        // ---- 字符串体内 ----
        if (inString) {
            if (text[pos] == '\\' && pos + 1 < len) {
                mask[pos] = 1;
                pos++;
                mask[pos] = 1;
                pos++;
                continue;
            }
            if (text[pos] == '"') {
                mask[pos] = 1;
                pos++;
                stringRanges.append({stringStart, pos - stringStart});
                inString = false;
                stringStart = -1;
                continue;
            }
            // 识别字符串插值 {expr}，进入插值模式（支持跨行）
            if (text[pos] == '{') {
                stringRanges.append({stringStart, pos - stringStart});
                mask[pos] = 1;  // { 标记为字符串颜色
                pos++;
                interpBraceDepth = 1;
                continue;
            }
            mask[pos] = 1;
            pos++;
            continue;
        }

        // ---- 正常模式 ----
        if (pos + 1 < len && text[pos] == '/' && text[pos + 1] == '/') {
            commentRanges.append({pos, len - pos});
            for (int i = pos; i < len; ++i) mask[i] = 1;
            break;
        }

        if (pos + 1 < len && text[pos] == '/' && text[pos + 1] == '*') {
            commentStart = pos;
            blockCommentDepth = 1;
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
    if (blockCommentDepth > 0 && commentStart >= 0) {
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
            // BUG-SH-4 fix: 移除 wordLen <= 20 上限检查。原检查跳过过长标识符的
            // keywordSet_ 查找，但 QSet::contains 对任意长度都是 O(1)+平均 O(L) 哈希，
            // 无性能问题。上限 20 会漏掉理论上的长关键字（虽当前无，但为防御性修复）。
            if (!keywordSet_.isEmpty() &&
                keywordSet_.contains(text.mid(start, wordLen))) {
                setFormat(start, wordLen, keywordFormat_);
            }
            continue;
        }

        // ---- 数字字面量 ----
        // 支持：整数(123)、浮点(1.23)、科学计数(1e5/1.23e-5)、
        //       前导点(.123)、尾点(123.)
        // 判定条件：首字符是数字，或者首字符是 '.' 且下一个字符是数字
        // BUG-SH-3 fix: 识别 0x/0b/0o 前缀。MiniLang 不支持这些前缀，将整个
        // 非法字面量高亮为错误格式（红色），避免被当作普通数字 0 处理后剩余
        // 字符（如 xFF）被误识别为标识符。
        if (c == '0' && pos + 1 < len &&
            (text[pos + 1] == 'x' || text[pos + 1] == 'X' ||
             text[pos + 1] == 'b' || text[pos + 1] == 'B' ||
             text[pos + 1] == 'o' || text[pos + 1] == 'O')) {
            int start = pos;
            pos += 2;
            while (pos < len && (text[pos].isLetterOrNumber() || text[pos] == '_')) ++pos;
            QTextCharFormat errFmt;
            errFmt.setForeground(QColor(0xdc, 0x32, 0x2f));
            setFormat(start, pos - start, errFmt);
            continue;
        }
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

    // 设置块状态（支持多行字符串、嵌套块注释、跨行插值、插值内嵌套字符串/块注释）
    // BUG-SH-1 fix: 插值内块注释跨行时用 300+braceDepth*10+blockCommentDepth
    //   编码同时保留两个上下文。原编码 blockCommentDepth > 0 优先导致
    //   interpBraceDepth 丢失，下一行块注释结束后无法回到插值模式。
    // BUG-SH-2 fix: 插值内嵌套字符串跨行用 400+braceDepth 编码。
    //   原实现嵌套字符串仅单行扫描，未闭合时下一行按普通代码处理。
    // 注：inString 在 interpBraceDepth > 0 期间恒为 true（进入插值时未清除，
    //   退出插值时显式重置），无需单独编码，200+braceDepth 即可覆盖。
    int newState = 0;
    if (inNestedString) newState = 400 + interpBraceDepth;
    else if (blockCommentDepth > 0 && interpBraceDepth > 0)
        newState = 300 + interpBraceDepth * 10 + blockCommentDepth;
    else if (blockCommentDepth > 0) newState = 100 + blockCommentDepth;
    else if (interpBraceDepth > 0) newState = 200 + interpBraceDepth;
    else if (inString) newState = 1;
    setCurrentBlockState(newState);
}
