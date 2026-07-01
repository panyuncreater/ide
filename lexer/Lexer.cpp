#include "lexer/Lexer.h"
#include "common/Utf8Utils.h"
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>

// ============================================================
// Lexer 词法分析器实现
// ============================================================

// D10 fix: locale-independent ASCII 分类函数。
// std::isalpha/isalnum/isdigit 依赖全局 C locale，在非 "C" locale 下行为可能不同
// （例如某些 locale 下 isalpha 对高位字节返回 true）。词法分析仅需识别 ASCII
// 标识符字符与十进制数字，直接用 ASCII 码位比较彻底消除 locale 依赖。
namespace {
inline bool isAsciiDigit(char c)    { return c >= '0' && c <= '9'; }
inline bool isAsciiAlpha(char c)    { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
inline bool isAsciiAlphaNum(char c) { return isAsciiAlpha(c) || isAsciiDigit(c); }
} // namespace

Lexer::Lexer() {
}

const std::unordered_map<std::string, TokenType>& Lexer::keywords() {
    static const auto kw = [] {
        std::unordered_map<std::string, TokenType> m;
        m["var"]      = TokenType::TK_VAR;
        m["fun"]      = TokenType::TK_FUN;
        m["if"]       = TokenType::TK_IF;
        m["else"]     = TokenType::TK_ELSE;
        m["while"]   = TokenType::TK_WHILE;
        m["for"]     = TokenType::TK_FOR;
        m["return"]  = TokenType::TK_RETURN;
        m["true"]    = TokenType::TK_TRUE;
        m["false"]   = TokenType::TK_FALSE;
        m["and"]     = TokenType::TK_AND;
        m["or"]      = TokenType::TK_OR;
        m["not"]     = TokenType::TK_NOT;
        m["print"]   = TokenType::TK_PRINT;
        m["break"]   = TokenType::TK_BREAK;
        m["continue"]= TokenType::TK_CONTINUE;
        m["try"]     = TokenType::TK_TRY;
        m["catch"]   = TokenType::TK_CATCH;
        m["throw"]   = TokenType::TK_THROW;
        m["import"]  = TokenType::TK_IMPORT;
        m["from"]    = TokenType::TK_FROM;
        m["export"]  = TokenType::TK_EXPORT;
        m["int"]     = TokenType::TK_INT;
        m["float"]   = TokenType::TK_FLOAT;
        m["bool"]    = TokenType::TK_BOOL;
        m["string"]  = TokenType::TK_STRING_TYPE;
        m["function"] = TokenType::TK_FUN;  // function 是 fun 的别名，统一为 TK_FUN
        m["func"]     = TokenType::TK_FUN;  // func 是 fun 的别名，统一为 TK_FUN
        m["class"]   = TokenType::TK_CLASS;
        m["extends"] = TokenType::TK_EXTENDS;
        m["super"]   = TokenType::TK_SUPER;
        m["dict"]    = TokenType::TK_DICT;
        m["array"]   = TokenType::TK_ARRAY;
        m["null"]    = TokenType::TK_NULL;
        return m;
    }();
    return kw;
}

std::vector<Token> Lexer::scan(const std::string& source) {
    // P0 fix: 源码大小上限检查，防止恶意大文件导致 DoS
    if (source.size() > MAX_SOURCE_SIZE) {
        diagnostics_.addError("源代码过大（" + std::to_string(source.size() / 1024 / 1024) +
                              "MB），超过上限 " + std::to_string(MAX_SOURCE_SIZE / 1024 / 1024) + "MB",
                              1, 1, DiagSource::Lexer);
        tokens_.clear();
        tokens_.emplace_back(TokenType::TK_EOF, "", std::monostate{}, 1, 1);
        return tokens_;
    }

    source_ = source;
    start_ = 0;
    current_ = 0;
    line_ = 1;
    lineStart_ = 0;
    // Perf-Finding1: 失效 columnAt 缓存（source_ 已更换，旧 byteOffset 失效）
    cachedLineStart_ = -1;
    cachedByteOffset_ = -1;
    cachedColumn_ = 1;
    tokens_.clear();
    comments_.clear();
    diagnostics_.clear();
    interpDepth_ = 0;  // L-P1-1: 重置插值嵌套深度
    // PERF-19 fix: 预分配 tokens_ 容量，避免大文件场景多次 realloc。
    // 估算：平均每 8 字符产生 1 个 token（关键字/标识符/字面量/运算符）
    // P0 fix: 限制 reserve 在 MAX_TOKEN_COUNT 内——原 `source.size()/8 + 16` 对 10MB 源码
    // 会预分配 1.25M 槽位（~110MB），超过 MAX_TOKEN_COUNT (1M) 25%，单次大额堆分配极易
    // 抛 bad_alloc（特别是已运行过其他工具的内存压力下）。token 上限检查会强制 break，
    // 预分配超过上限纯属浪费。
    size_t reserveCap = source.size() / 8 + 16;
    if (reserveCap > MAX_TOKEN_COUNT) reserveCap = MAX_TOKEN_COUNT;
    tokens_.reserve(reserveCap);

    // 跳过 UTF-8 BOM（字节序标记）
    if (source_.size() >= 3 &&
        static_cast<unsigned char>(source_[0]) == 0xEF &&
        static_cast<unsigned char>(source_[1]) == 0xBB &&
        static_cast<unsigned char>(source_[2]) == 0xBF) {
        current_ = 3;
        start_ = 3;
        lineStart_ = 3;
    }

    while (!isAtEnd()) {
        start_ = current_;
        scanToken();
        // P0 fix: Token 数量上限检查，防止 Token 爆炸导致 OOM
        if (tokens_.size() > MAX_TOKEN_COUNT) {
            diagnostics_.addError("Token 数量超过上限 " + std::to_string(MAX_TOKEN_COUNT) +
                                  "，源代码可能包含过多 token",
                                  line_, currentColumn(), DiagSource::Lexer);
            break;
        }
    }

    // 添加 EOF Token
    tokens_.emplace_back(TokenType::TK_EOF, "", std::monostate{}, line_, currentColumn());

    // 从 TK_ERROR Token 中提取诊断信息，并将注释 Token 分离到 comments_
    // P0 fix: 不再 reserve(tokens_.size())——原代码会瞬时分配与 tokens_ 等大的内存，
    // 峰值达 tokens_ + cleanTokens 双倍（~220MB）。改为让 cleanTokens 自然增长，
    // 注释 Token 通常很少，cleanTokens.size() ≈ tokens_.size() - comments_.size()，
    // move 后 tokens_ 释放，最终 cleanTokens 占用 ≈ 原大小。略多几次 realloc 但避免峰值翻倍。
    std::vector<Token> cleanTokens;
    for (auto& tok : tokens_) {
        if (tok.type == TokenType::TK_ERROR) {
            diagnostics_.addError(tok.lexeme, tok.line, tok.column, DiagSource::Lexer);
            cleanTokens.push_back(std::move(tok));
        } else if (tok.type == TokenType::TK_LINE_COMMENT || tok.type == TokenType::TK_BLOCK_COMMENT) {
            comments_.push_back(std::move(tok));
        } else {
            cleanTokens.push_back(std::move(tok));
        }
    }
    tokens_ = std::move(cleanTokens);

    return std::move(tokens_);
}

char Lexer::peek() const {
    if (isAtEnd()) return '\0';
    return source_[current_];
}

char Lexer::peekNext() const {
    if (current_ + 1 >= static_cast<int>(source_.size())) return '\0';
    return source_[current_ + 1];
}

char Lexer::advance() {
    if (current_ >= static_cast<int>(source_.size())) return '\0';  // 防御性边界检查
    char c = source_[current_];
    current_++;
    if (c == '\n') {
        line_++;
        lineStart_ = current_;
    } else if (c == '\r') {
        // \r\n 视为单个换行；裸 \r（旧 Mac 格式）也触发换行
        if (current_ < static_cast<int>(source_.size()) && source_[current_] == '\n') {
            current_++;
        }
        line_++;
        lineStart_ = current_;
        return '\n';  // LEX-01/02/03 fix: 统一返回 '\n'，确保注释循环和字符串构建正确感知换行
    }
    return c;
}

bool Lexer::isAtEnd() const {
    return current_ >= static_cast<int>(source_.size());
}

bool Lexer::match(char expected) {
    if (isAtEnd()) return false;
    if (source_[current_] != expected) return false;
    advance();  // 使用 advance() 确保行号跟踪正确
    return true;
}

void Lexer::scanToken() {
    // P0 fix: 在 scanToken 入口检查 token 上限，防止插值字符串路径（string() 中
    // 递归调用 scanToken）绕过 scan() 外层循环的 MAX_TOKEN_COUNT 检查。
    // 原检查仅在 scan() 外层 while 循环中，但 string() 的插值表达式循环会递归
    // 调用 scanToken()，单次 string() 调用可能产生数百万 Token，远超上限，
    // 导致 tokens_ 容器指数级 realloc 触发 bad_alloc。
    // Hard Constraint #27: Lexer must check MAX_TOKEN_COUNT at start of scanToken().
    if (tokens_.size() >= MAX_TOKEN_COUNT) {
        diagnostics_.addError("Token 数量超过上限 " + std::to_string(MAX_TOKEN_COUNT) +
                              "，源代码可能包含过多 token",
                              line_, currentColumn(), DiagSource::Lexer);
        return;
    }

    char c = advance();

    switch (c) {
    // 空白字符
    case ' ':
    case '\t':
    case '\n':
    case '\f':   // L3 fix: form feed
    case '\v':   // L3 fix: vertical tab
        // 注：'\r' 不会出现在此处，advance() 已将 \r 和 \r\n 统一返回 '\n'
        break;

    // 分隔符
    case '(': addToken(TokenType::TK_LPAREN); break;
    case ')': addToken(TokenType::TK_RPAREN); break;
    case '{': addToken(TokenType::TK_LBRACE); break;
    case '}': addToken(TokenType::TK_RBRACE); break;
    case ';': addToken(TokenType::TK_SEMICOLON); break;
    case ',': addToken(TokenType::TK_COMMA); break;
    // 新增分隔符
    case '[': addToken(TokenType::TK_LBRACKET); break;
    case ']': addToken(TokenType::TK_RBRACKET); break;
    case ':': addToken(TokenType::TK_COLON); break;
    case '.':
        // .123 → 浮点数前导点
        if (isAsciiDigit(peek())) {
            number();
        } else {
            addToken(TokenType::TK_DOT);
        }
        break;

    // 运算符
    case '+': addToken(TokenType::TK_PLUS); break;
    case '-': addToken(TokenType::TK_MINUS); break;
    case '*': addToken(TokenType::TK_STAR); break;
    case '/':
        // 单行注释
        if (match('/')) {
            // 捕获注释文本（含 // 前缀）
            size_t commentStart = start_;
            // P0-2 fix: 同时检查 \r 和 \n，避免 CRLF 下 advance() 消耗 \r\n 后
            // peek() 跳过 \n 导致注释吞掉下一行内容
            while (!isAtEnd() && peek() != '\n' && peek() != '\r') advance();
            std::string commentText(source_, commentStart, current_ - commentStart);
            Token tok;
            tok.type = TokenType::TK_LINE_COMMENT;
            tok.lexeme = commentText;
            tok.line = line_;
            tok.column = static_cast<int>(commentStart - lineStart_) + 1;
            tokens_.push_back(tok);
        } else if (match('*')) {
            // 块注释 /* ... */（支持嵌套）
            size_t commentStart = start_;
            int startLine = line_;
            int startCol = static_cast<int>(commentStart - lineStart_) + 1;
            int depth = 1;  // 嵌套深度
            while (!isAtEnd() && depth > 0) {
                if (peek() == '/' && peekNext() == '*') {
                    advance(); advance();  // 消耗 /*
                    depth++;
                } else if (peek() == '*' && peekNext() == '/') {
                    advance(); advance();  // 消耗 */
                    depth--;
                } else {
                    advance();
                }
            }
            if (depth > 0) {
                errorToken("未终止的块注释", startLine, startCol);
                return;
            }
            std::string commentText(source_, commentStart, current_ - commentStart);
            Token tok;
            tok.type = TokenType::TK_BLOCK_COMMENT;
            tok.lexeme = commentText;
            tok.line = startLine;
            tok.column = startCol;
            tokens_.push_back(tok);
        } else {
            addToken(TokenType::TK_SLASH);
        }
        break;
    case '%': addToken(TokenType::TK_PERCENT); break;

    // 可能是双字符运算符（使用查找表 O(1) 分派）
    case '=': case '!': case '<': case '>': case '&': case '|': {
        // 双字符运算符查找表
        struct TwoCharOp {
            char first;
            char second;        // 匹配的第二字符（'\0' 表示无匹配）
            TokenType dualType; // 双字符类型
            TokenType soloType; // 单字符类型
            const char* hint;   // 错误提示（soloType 为 TK_ERROR 时使用）
        };
        static const TwoCharOp twoCharOps[] = {
            {'=', '=', TokenType::TK_EQ,     TokenType::TK_ASSIGN, nullptr},
            {'!', '=', TokenType::TK_NEQ,    TokenType::TK_NOT,    nullptr},
            {'<', '=', TokenType::TK_LEQ,    TokenType::TK_LT,     nullptr},
            {'>', '=', TokenType::TK_GEQ,    TokenType::TK_GT,     nullptr},
            {'&', '&', TokenType::TK_ERROR,  TokenType::TK_ERROR,  "请使用 'and' 关键字代替 '&'"},
            {'|', '|', TokenType::TK_ERROR,  TokenType::TK_ERROR,  "请使用 'or' 关键字代替 '|'"},
        };
        // #28 fix: [256] 查找表按首字符 O(1) 定位条目，替代 6 项线性扫描。
        // 首字符仅这 6 个有条目，其余槽位为 nullptr。首次调用时初始化，后续零开销。
        static const auto firstCharTable = []() {
            std::array<const TwoCharOp*, 256> t{};
            for (const auto& op : twoCharOps) {
                t[static_cast<unsigned char>(op.first)] = &op;
            }
            return t;
        }();
        const TwoCharOp* entry = firstCharTable[static_cast<unsigned char>(c)];
        if (entry) {
            if (entry->second != '\0' && match(entry->second)) {
                if (entry->dualType != TokenType::TK_ERROR)
                    addToken(entry->dualType);
                else
                    errorToken(std::string("'") + c + c + "'（" + entry->hint + "）");
            } else if (entry->soloType != TokenType::TK_ERROR) {
                addToken(entry->soloType);
            } else {
                errorToken(std::string("意外字符 '") + c + "'（" + entry->hint + "）");
            }
        }
        break;
    }

    // 字符串字面量
    case '"': string(); break;

    default:
        // 数字
        if (isAsciiDigit(c)) {
            number();
        }
        // 标识符或关键字
        else if (isAsciiAlpha(c) || c == '_') {
            identifier();
        }
        // 无法识别的字符
        else {
            errorToken(std::string("意外字符 '") + c + "'");
        }
        break;
    }
}

void Lexer::identifier() {
    while (!isAtEnd() && (isAsciiAlphaNum(peek()) || peek() == '_')) {
        advance();
    }
    std::string text(source_, start_, current_ - start_);

    // #10 fix: 保留 __ 前缀给编译器内部使用（__blk_save_*, __wb_idx_*）
    if (text.size() >= 2 && text[0] == '_' && text[1] == '_') {
        errorToken("标识符 '" + text + "' 使用了保留前缀 '__'（编译器内部使用）");
        return;
    }

    // 查关键字表
    auto it = keywords().find(text);
    if (it != keywords().end()) {
        TokenType type = it->second;
        // true 和 false 有字面量值
        if (type == TokenType::TK_TRUE) {
            addToken(type, std::move(text), true);  // A1 fix: variant bool
        } else if (type == TokenType::TK_FALSE) {
            addToken(type, std::move(text), false);  // A1 fix: variant bool
        } else if (type == TokenType::TK_NULL) {
            // null 关键字有字面量值
            addToken(type, std::move(text), std::monostate{});  // A1 fix: variant monostate
        } else {
            addToken(type, std::move(text));
        }
    } else {
        addToken(TokenType::TK_IDENTIFIER, std::move(text));
    }
}

void Lexer::number() {
    // 前导点浮点（.123）：scanToken 已消耗 '.'，start_ 指向 '.'，跳过整数部分
    bool isFloat = (source_[start_] == '.');

    // M6 fix: 检测 0x/0b/0o 前缀，发出明确错误（而非静默分为两个 token）
    if (!isFloat && !isAtEnd() && source_[start_] == '0' &&
        (peek() == 'x' || peek() == 'X' || peek() == 'b' || peek() == 'B' ||
         peek() == 'o' || peek() == 'O')) {
        char prefix = peek();
        advance(); // 消耗前缀字母
        // P2-2 fix: 用 isalnum 消费整个非法字面量，避免 isxdigit 对 0b/0o 前缀的语义错误
        // （isxdigit 会消费 a-f，但 0bff 中 ff 不是合法二进制位；此处统一消费字母数字即可）
        while (!isAtEnd() && isAsciiAlphaNum(peek())) {
            advance();
        }
        std::string text(source_, start_, current_ - start_);
        errorToken("不支持 " + std::string(1, prefix) + " 前缀字面量: " + text + "，请使用十进制表示");
        return;
    }

    while (!isAtEnd() && isAsciiDigit(peek())) {
        advance();
    }

    // 浮点数：小数部分（仅当 '.' 后紧跟数字时才视为浮点，避免 123.foo 被误分词）
    if (!isFloat && !isAtEnd() && peek() == '.' &&
        (static_cast<size_t>(current_) + 1 < source_.size()) && isAsciiDigit(source_[current_ + 1])) {
        isFloat = true;
        advance(); // 消耗 '.'
        while (!isAtEnd() && isAsciiDigit(peek())) {
            advance();
        }
    }

    // M5 fix: 整数后直接跟 .e/.E 形式（如 123.e5），消耗 '.' 后进入科学计数法
    if (!isFloat && !isAtEnd() && peek() == '.' &&
        (static_cast<size_t>(current_) + 1 < source_.size()) &&
        (source_[current_ + 1] == 'e' || source_[current_ + 1] == 'E')) {
        isFloat = true;
        advance(); // 消耗 '.'
    }

    // #15: 科学计数法（如 1e5, 3.14e-2, 2E+10, 123.e5）
    if (!isAtEnd() && (peek() == 'e' || peek() == 'E')) {
        isFloat = true;
        advance(); // 消耗 'e'/'E'
        if (!isAtEnd() && (peek() == '+' || peek() == '-')) {
            advance(); // 消耗符号
        }
        if (isAtEnd() || !isAsciiDigit(peek())) {
            errorToken("科学计数法格式错误: " + std::string(source_, start_, current_ - start_));
            return;
        }
        while (!isAtEnd() && isAsciiDigit(peek())) {
            advance();
        }
    }

    std::string text(source_, start_, current_ - start_);

    if (isFloat) {
        // L-P1-3 fix: 使用 std::from_chars 替代 std::stod，locale-independent，
        // 避免系统 locale 用 ',' 作小数点时 "1.5" 解析错误
        double val = 0.0;
        auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), val);
        if (ec == std::errc::result_out_of_range) {
            errorToken("浮点数溢出: " + text);
        } else if (ec != std::errc() || ptr != text.data() + text.size()) {
            errorToken("浮点数格式错误: " + text);
        } else {
            addToken(TokenType::TK_FLOAT_LIT, std::move(text), val);  // A1 fix: variant double
        }
    } else {
        // L-P1-3 fix: 使用 std::from_chars 替代 std::stoll，locale-independent
        int64_t val = 0;
        auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), val);
        if (ec == std::errc::result_out_of_range) {
            errorToken("整数溢出: " + text);
        } else if (ec != std::errc() || ptr != text.data() + text.size()) {
            errorToken("整数格式错误: " + text);
        } else {
            addToken(TokenType::TK_INT_LIT, std::move(text), val);  // A1 fix: variant int64_t
        }
    }
}

void Lexer::string() {
    string(false);
}

void Lexer::string(bool isInterp) {
    int startLine = line_;
    int startCol = static_cast<int>(start_ - lineStart_) + 1;
    std::string value;
    // P-07 fix: 预估字符串容量，避免逐字符 += 反复 realloc
    // P0 fix: 限制 reserve 上限为 1MB，防止未闭合字符串触发 GB 级内存分配
    size_t reserveCap = current_ < source_.size() ? (source_.size() - current_) : 0;
    if (reserveCap > 1024 * 1024) reserveCap = 1024 * 1024;
    value.reserve(reserveCap);

    while (!isAtEnd() && peek() != '"') {
        // F7: 检测插值起始 {
        if (peek() == '{') {
            // L-P1-1: 插值嵌套深度检查，防止栈溢出
            if (interpDepth_ >= MAX_INTERP_DEPTH) {
                errorToken("字符串插值嵌套过深（最大 " + std::to_string(MAX_INTERP_DEPTH) + " 层）", startLine, startCol);
                return;
            }
            interpDepth_++;
            // 发出前面的文本片段（TK_STRING_PART 表示插值字符串的一部分）
            TokenType partType = isInterp ? TokenType::TK_STRING_PART : TokenType::TK_STRING_LIT;
            // 如果是插值字符串的第一个片段，用 TK_STRING_LIT；后续片段用 TK_STRING_PART
            // 但为简化 Parser 逻辑，统一：插值字符串中所有文本片段都用 TK_STRING_PART，
            // 仅当整个字符串无插值时用 TK_STRING_LIT（由下方闭合处判断）
            std::string text(source_, start_, current_ - start_);
            tokens_.emplace_back(partType, std::move(text), std::move(value), startLine, startCol);  // A1 fix: variant string

            // 消耗 {
            advance();
            // 发出 TK_INTERP_START
            int braceLine = line_;
            int braceCol = static_cast<int>(start_ - lineStart_) + 1;
            tokens_.emplace_back(TokenType::TK_INTERP_START, "{", std::monostate{}, braceLine, braceCol);  // A1 fix: variant monostate

            // 扫描表达式直到 }（支持嵌套大括号，如对象字面量）
            // 更新 start_ 到表达式起始位置，确保 scanToken() 的 addToken() 正确提取 lexeme
            start_ = current_;
            int braceDepth = 1;
            while (!isAtEnd() && braceDepth > 0) {
                // 跳过空白
                if (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r') {
                    advance();
                    continue;
                }
                if (peek() == '{') {
                    braceDepth++;
                    start_ = current_;  // 更新 start_ 以便 scanToken 正确提取
                    scanToken();
                } else if (peek() == '}') {
                    braceDepth--;
                    if (braceDepth == 0) {
                        advance();  // 消耗 }
                        int endLine = line_;
                        int endCol = static_cast<int>(start_ - lineStart_) + 1;
                        tokens_.emplace_back(TokenType::TK_INTERP_END, "}", std::monostate{}, endLine, endCol);  // A1 fix: variant monostate
                        break;
                    }
                    start_ = current_;  // 更新 start_ 以便 scanToken 正确提取
                    scanToken();
                } else if (peek() == '"') {
                    // 嵌套字符串（可能含插值）
                    // AUDIT-BUG-F10 fix: advance 前更新 start_，与 { } 默认分支一致。
                    // 原实现缺少 start_=current_，导致嵌套字符串首 token 列号/lexeme 错误。
                    start_ = current_;
                    advance();
                    string(false);  // 嵌套字符串作为独立 TK_STRING_LIT，非插值片段
                    start_ = current_;  // 更新 start_ 以便后续 scanToken 正确提取
                } else {
                    start_ = current_;  // 更新 start_ 以便 scanToken 正确提取
                    scanToken();
                }
            }
            if (braceDepth > 0) {
                interpDepth_--;  // L-P1-1: 错误退出时也减少深度，保持计数器一致
                errorToken("未终止的插值表达式（缺少 }）", startLine, startCol);
                return;
            }

            // L-P1-1: 本层插值已闭合，减少深度
            interpDepth_--;

            // 继续扫描字符串剩余部分（标记为插值片段）
            start_ = current_;
            startLine = line_;
            startCol = static_cast<int>(start_ - lineStart_) + 1;
            value.clear();
            isInterp = true;
            continue;
        }

        // 换行处理由 advance() 统一完成（line_++ 和 lineStart_ 更新），
        // 此处不再手动递增，否则会导致行号双重递增。
        if (peek() == '\\') {
            advance(); // 消耗反斜杠
            if (isAtEnd()) {
                errorToken("未终止的字符串", startLine, startCol);
                return;
            }
            char esc = advance();
            switch (esc) {
            case 'n':  value += '\n'; break;
            case 't':  value += '\t'; break;
            case 'r':  value += '\r'; break;
            case '\\': value += '\\'; break;
            case '"':  value += '"';  break;
            case '\'': value += '\''; break;
            case '0':  value += '\0'; break;
            case 'b':  value += '\b'; break;
            case 'f':  value += '\f'; break;
            case 'a':  value += '\a'; break;
            case 'v':  value += '\v'; break;
            default:
                // AUDIT-BUG-L1 fix: 未知转义序列应报错，而非静默接受为字面字符。
                // 原实现将 \q 等存储为 "\\q" 两字符，违反"非法输入应被拒绝"原则。
                errorToken(std::string("未知转义序列 '\\") + esc + "'", startLine, startCol);
                // 恢复：跳过到字符串结束或 EOF，避免级联产生误导性错误
                while (!isAtEnd() && peek() != '"') {
                    if (peek() == '\\') { advance(); if (!isAtEnd()) advance(); }
                    else advance();
                }
                if (!isAtEnd()) advance(); // 消耗闭合的 '"'
                return;
            }
        } else {
            value += advance();
        }
    }

    if (isAtEnd()) {
        errorToken("未终止的字符串", startLine, startCol);
        return;
    }

    advance(); // 消耗闭合的 '"'

    // F7: 如果是插值字符串的后续片段，用 TK_STRING_PART；否则用 TK_STRING_LIT
    TokenType finalType = isInterp ? TokenType::TK_STRING_PART : TokenType::TK_STRING_LIT;
    std::string text(source_, start_, current_ - start_);
    tokens_.emplace_back(finalType, std::move(text), std::move(value), startLine, startCol);  // A1 fix: variant string
}

void Lexer::addToken(TokenType type) {
    std::string text(source_, start_, current_ - start_);
    int col = columnAt(start_);
    tokens_.emplace_back(type, std::move(text), std::monostate{}, line_, col);  // A1 fix: variant monostate
}

void Lexer::addToken(TokenType type, TokenLiteral literal) {
    std::string text(source_, start_, current_ - start_);
    int col = columnAt(start_);
    tokens_.emplace_back(type, std::move(text), std::move(literal), line_, col);
}

void Lexer::addToken(TokenType type, std::string&& text) {
    int col = columnAt(start_);
    tokens_.emplace_back(type, std::move(text), std::monostate{}, line_, col);  // A1 fix: variant monostate
}

void Lexer::addToken(TokenType type, std::string&& text, TokenLiteral literal) {
    int col = columnAt(start_);
    tokens_.emplace_back(type, std::move(text), std::move(literal), line_, col);
}

void Lexer::errorToken(const std::string& message, int errorLine, int errorCol) {
    int col = (errorCol > 0) ? errorCol : columnAt(start_);
    int ln = (errorLine > 0) ? errorLine : line_;
    tokens_.emplace_back(TokenType::TK_ERROR, message, std::monostate{}, ln, col);  // A1 fix: variant monostate
}

int Lexer::currentColumn() const {
    return columnAt(current_);
}

int Lexer::columnAt(int byteOffset) const {
    // D11 fix: 按 UTF-8 码位计算列号，避免多字节字符（如中文）导致列号偏移过大。
    // 从 lineStart_ 遍历到 byteOffset，按首字节判断码位字节数。
    // Perf-Finding1: 单调前进缓存。行内 columnAt 调用的 byteOffset 单调非递减
    // （start_/current_ 仅向前推进），命中时从缓存点续走，消除 O(n²) 重复扫描。
    if (byteOffset <= lineStart_) {
        // byteOffset 等于行首（或异常小于行首）：列为 1，并刷新缓存
        cachedLineStart_ = lineStart_;
        cachedByteOffset_ = byteOffset;
        cachedColumn_ = 1;
        return 1;
    }

    int i;
    int col;
    // 缓存命中条件：同一行（lineStart_ 未变）且 byteOffset 不回退
    if (cachedLineStart_ == lineStart_ && cachedByteOffset_ >= lineStart_ && byteOffset >= cachedByteOffset_) {
        i = cachedByteOffset_;
        col = cachedColumn_;
    } else {
        // 缓存未命中：从行首重新计算
        i = lineStart_;
        col = 1;
    }

    while (i < byteOffset && i < static_cast<int>(source_.size())) {
        unsigned char c = static_cast<unsigned char>(source_[i]);
        int len = Utf8::byteLength(c);
        // 防御：若剩余字节不足完整码位，按 1 字节前进避免越界
        if (i + len > byteOffset || i + len > static_cast<int>(source_.size())) {
            len = 1;
        }
        i += len;
        col++;
    }

    // 更新缓存（行未变 + 单调前进）
    cachedLineStart_ = lineStart_;
    cachedByteOffset_ = byteOffset;
    cachedColumn_ = col;
    return col;
}
