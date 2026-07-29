#include "lexer/Lexer.h"
#include "common/ErrorMessages.h" // P2-12: DiagCodes 常量
#include "common/Utf8Utils.h"
#include <algorithm> // PERF: std::lower_bound
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>  // PERF: std::strcmp for keyword lookup
#include <optional> // PERF: lookupKeywordFast return type
#include <string_view> // AUDIT-R4 BUG-07: 浮点上溢/下溢判定的尾数切片

// ============================================================
// Lexer 词法分析器实现
// ============================================================

// D10 fix: locale-independent ASCII 分类函数。
// std::isalpha/isalnum/isdigit 依赖全局 C locale，在非 "C" locale 下行为可能不同
// （例如某些 locale 下 isalpha 对高位字节返回 true）。词法分析仅需识别 ASCII
// 标识符字符与十进制数字，直接用 ASCII 码位比较彻底消除 locale 依赖。
namespace {
inline bool isAsciiDigit(char c) {
    return c >= '0' && c <= '9';
}
inline bool isAsciiAlpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
inline bool isAsciiAlphaNum(char c) {
    return isAsciiAlpha(c) || isAsciiDigit(c);
}
} // namespace

Lexer::Lexer() {
    // 成员（source_/current_/line_/tokens_/diagnostics_ 等）均在 scan() 入口
    // 被重置，构造函数本身无需初始化逻辑——保持为空，避免与 scan() 的初始化重复。
}

const std::unordered_map<std::string, TokenType>& Lexer::keywords() {
    static const auto kw = [] {
        std::unordered_map<std::string, TokenType> m;
        m["var"] = TokenType::TK_VAR;
        m["fun"] = TokenType::TK_FUN;
        m["if"] = TokenType::TK_IF;
        m["else"] = TokenType::TK_ELSE;
        m["while"] = TokenType::TK_WHILE;
        m["for"] = TokenType::TK_FOR;
        m["return"] = TokenType::TK_RETURN;
        m["true"] = TokenType::TK_TRUE;
        m["false"] = TokenType::TK_FALSE;
        m["and"] = TokenType::TK_AND;
        m["or"] = TokenType::TK_OR;
        m["not"] = TokenType::TK_NOT;
        m["print"] = TokenType::TK_PRINT;
        m["break"] = TokenType::TK_BREAK;
        m["continue"] = TokenType::TK_CONTINUE;
        m["try"] = TokenType::TK_TRY;
        m["catch"] = TokenType::TK_CATCH;
        m["throw"] = TokenType::TK_THROW;
        m["finally"] = TokenType::TK_FINALLY; // BUG-AUDIT-FINALLY-1
        m["import"] = TokenType::TK_IMPORT;
        m["from"] = TokenType::TK_FROM;
        m["export"] = TokenType::TK_EXPORT;
        m["as"] = TokenType::TK_AS; // P2-11: import * as ns
        m["int"] = TokenType::TK_INT;
        m["float"] = TokenType::TK_FLOAT;
        m["bool"] = TokenType::TK_BOOL;
        m["string"] = TokenType::TK_STRING_TYPE;
        m["function"] = TokenType::TK_FUN; // function 是 fun 的别名，统一为 TK_FUN
        m["func"] = TokenType::TK_FUN;     // func 是 fun 的别名，统一为 TK_FUN
        m["class"] = TokenType::TK_CLASS;
        m["extends"] = TokenType::TK_EXTENDS;
        m["super"] = TokenType::TK_SUPER;
        m["dict"] = TokenType::TK_DICT;
        m["array"] = TokenType::TK_ARRAY;
        m["null"] = TokenType::TK_NULL;
        // R99 枚举与 ADT + match：新增关键字
        m["enum"] = TokenType::TK_ENUM;
        m["match"] = TokenType::TK_MATCH;
        m["case"] = TokenType::TK_CASE;
        m["default"] = TokenType::TK_DEFAULT;
        // R164 协程/生成器：新增关键字
        m["yield"] = TokenType::TK_YIELD;
        // L18 lang-constfun
        m["const"] = TokenType::TK_CONST;
        return m;
    }();
    return kw;
}

// PERF: 编译期排序关键字数组 + std::lower_bound 二分查找。
// 对 37 个关键字最多 6 次比较，连续内存布局对 L1 cache 友好，
// vs unordered_map 的 hash 计算 + 散列节点遍历（cache-unfriendly）。
namespace {
struct KeywordEntry {
    const char* name;
    TokenType type;
};
// 必须严格按字典序排列（std::lower_bound 前提）
static constexpr KeywordEntry kSortedKeywords[] = {
    {"and", TokenType::TK_AND},
    {"array", TokenType::TK_ARRAY},
    {"as", TokenType::TK_AS},
    {"bool", TokenType::TK_BOOL},
    {"break", TokenType::TK_BREAK},
    {"case", TokenType::TK_CASE},
    {"catch", TokenType::TK_CATCH},
    {"class", TokenType::TK_CLASS},
    {"const", TokenType::TK_CONST},
    {"continue", TokenType::TK_CONTINUE},
    {"default", TokenType::TK_DEFAULT},
    {"dict", TokenType::TK_DICT},
    {"else", TokenType::TK_ELSE},
    {"enum", TokenType::TK_ENUM},
    {"export", TokenType::TK_EXPORT},
    {"extends", TokenType::TK_EXTENDS},
    {"false", TokenType::TK_FALSE},
    {"finally", TokenType::TK_FINALLY},
    {"float", TokenType::TK_FLOAT},
    {"for", TokenType::TK_FOR},
    {"from", TokenType::TK_FROM},
    {"fun", TokenType::TK_FUN},
    {"func", TokenType::TK_FUN},
    {"function", TokenType::TK_FUN},
    {"if", TokenType::TK_IF},
    {"import", TokenType::TK_IMPORT},
    {"int", TokenType::TK_INT},
    {"match", TokenType::TK_MATCH},
    {"not", TokenType::TK_NOT},
    {"null", TokenType::TK_NULL},
    {"or", TokenType::TK_OR},
    {"print", TokenType::TK_PRINT},
    {"return", TokenType::TK_RETURN},
    {"string", TokenType::TK_STRING_TYPE},
    {"super", TokenType::TK_SUPER},
    {"throw", TokenType::TK_THROW},
    {"true", TokenType::TK_TRUE},
    {"try", TokenType::TK_TRY},
    {"var", TokenType::TK_VAR},
    {"while", TokenType::TK_WHILE},
    {"yield", TokenType::TK_YIELD},
};
static constexpr size_t kSortedKeywordsCount = sizeof(kSortedKeywords) / sizeof(kSortedKeywords[0]);

/// PERF: 二分查找关键字，返回 TokenType 或 nullopt。
inline std::optional<TokenType> lookupKeywordFast(const std::string& text) {
    const auto* begin = kSortedKeywords;
    const auto* end = kSortedKeywords + kSortedKeywordsCount;
    auto it = std::lower_bound(begin, end, text,
        [](const KeywordEntry& entry, const std::string& key) {
            return std::strcmp(entry.name, key.c_str()) < 0;
        });
    if (it != end && std::strcmp(it->name, text.c_str()) == 0) {
        return it->type;
    }
    return std::nullopt;
}
} // anonymous namespace

std::vector<Token> Lexer::scan(const std::string& source) {
    // P0 fix: 源码大小上限检查，防止恶意大文件导致 DoS
    if (source.size() > MAX_SOURCE_SIZE) {
        diagnostics_.addError("源代码过大（" + std::to_string(source.size() / 1024 / 1024) + "MB），超过上限 " +
                                  std::to_string(MAX_SOURCE_SIZE / 1024 / 1024) + "MB",
                              1, 1, DiagSource::Lexer, DiagCodes::kSourceTooLarge);
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
    interpDepth_ = 0; // L-P1-1: 重置插值嵌套深度
    // PERF-19 fix: 预分配 tokens_ 容量，避免大文件场景多次 realloc。
    // 估算：平均每 8 字符产生 1 个 token（关键字/标识符/字面量/运算符）
    // P0 fix: 限制 reserve 在 MAX_TOKEN_COUNT 内——原 `source.size()/8 + 16` 对 10MB 源码
    // 会预分配 1.25M 槽位（~110MB），超过 MAX_TOKEN_COUNT (1M) 25%，单次大额堆分配极易
    // 抛 bad_alloc（特别是已运行过其他工具的内存压力下）。token 上限检查会强制 break，
    // 预分配超过上限纯属浪费。
    size_t reserveCap = source.size() / 8 + 16;
    if (reserveCap > MAX_TOKEN_COUNT)
        reserveCap = MAX_TOKEN_COUNT;
    tokens_.reserve(reserveCap);

    // 跳过 UTF-8 BOM（字节序标记）
    if (source_.size() >= 3 && static_cast<unsigned char>(source_[0]) == 0xEF &&
        static_cast<unsigned char>(source_[1]) == 0xBB && static_cast<unsigned char>(source_[2]) == 0xBF) {
        current_ = 3;
        start_ = 3;
        lineStart_ = 3;
    }

    while (!isAtEnd()) {
        start_ = current_;
        scanToken();
        // P0 fix: Token 数量上限检查，防止 Token 爆炸导致 OOM
        // AUDIT-R3 P0 fix: 比较运算符 > 改为 >=，与 scanToken 入口检查（>=）对齐。
        // 原不一致导致 token 数恰好等于上限时：scanToken 拒绝消费输入（不推进
        // current_）而本循环又不 break，形成死循环且每轮追加一条诊断（内存无界增长）。
        if (tokens_.size() >= MAX_TOKEN_COUNT) {
            diagnostics_.addError("Token 数量超过上限 " + std::to_string(MAX_TOKEN_COUNT) +
                                      "，源代码可能包含过多 token",
                                  line_, currentColumn(), DiagSource::Lexer, DiagCodes::kTooManyTokens);
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

/// 前瞻当前字符（不前进）；到达末尾返回 '哨兵，供双字符运算符/浮点判断使用。
char Lexer::peek() const {
    // 返回"下一个将被扫描"的字符但不前进指针（供 scanToken 做前瞻判断，
    // 例如 '.' 后是数字则按浮点处理）。到达末尾时返回 '\0' 哨兵，
    // 与 advance() 在 isAtEnd() 时的返回值保持一致。
    if (isAtEnd())
        return '\0';
    return source_[current_];
}

/// 前瞻下个字符（不前进）；用于判断 /* 后的 * 是否构成闭合及双字符运算符上下文。
char Lexer::peekNext() const {
    // 返回再下一个字符（前瞻两位）。用于判断 /* 后的 * 是否构成闭合 */，
    // 以及 '=' 后是否接 '=' 等双字符运算符上下文。越界返回 '\0'。
    if (current_ + 1 >= static_cast<int>(source_.size()))
        return '\0';
    return source_[current_ + 1];
}

/// 前进一步并返回当前字符；维护行号/行首偏移，将 \r\n 与裸 \n 统一为单个 \n。
char Lexer::advance() {
    if (current_ >= static_cast<int>(source_.size()))
        return '\0'; // 防御性边界检查
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
        return '\n'; // LEX-01/02/03 fix: 统一返回 '\n'，确保注释循环和字符串构建正确感知换行
    }
    return c;
}

/// 判断是否到达源代码末尾（current_ 越过末尾）。
bool Lexer::isAtEnd() const {
    return current_ >= static_cast<int>(source_.size());
}

/// 若当前字符匹配 expected 则前进并返回 true，否则不前进返回 false（用于选择性消费）。
bool Lexer::match(char expected) {
    if (isAtEnd())
        return false;
    if (source_[current_] != expected)
        return false;
    advance(); // 使用 advance() 确保行号跟踪正确
    return true;
}

/// 扫描单个 Token：扫描器状态机分派入口。根据当前字符的大类（空白/分隔符/运算符/
/// 字符串/数字/标识符/注释）走对应分支，并处理双字符运算符查表与插值字符串递归。
/// 入口处强制检查 MAX_TOKEN_COUNT，防止插值路径绕过主循环的 Token 上限防护。
void Lexer::scanToken() {
    // P0 fix: 在 scanToken 入口检查 token 上限，防止插值字符串路径（string() 中
    // 递归调用 scanToken）绕过 scan() 外层循环的 MAX_TOKEN_COUNT 检查。
    // 原检查仅在 scan() 外层 while 循环中，但 string() 的插值表达式循环会递归
    // 调用 scanToken()，单次 string() 调用可能产生数百万 Token，远超上限，
    // 导致 tokens_ 容器指数级 realloc 触发 bad_alloc。
    // Hard Constraint #27: Lexer must check MAX_TOKEN_COUNT at start of scanToken().
    if (tokens_.size() >= MAX_TOKEN_COUNT) {
        // Bug #41 fix: 不在此处报告诊断，由 scan() 主循环统一报告，避免重复。
        return;
    }

    char c = advance();

    // R132-B fix: 按字符类别分组到 5 个返回 bool 的 helper + 1 个默认分支 helper，
    // 替代原 213 行 switch。每个 helper 处理一类字符，返回 true 表示已处理（外层
    // 短路）；最后一个 scanLiteralOrUnknownToken 总会消费字符，无 fallthrough。
    if (scanWhitespaceToken(c))
        return;
    if (scanDelimiterToken(c))
        return;
    if (scanSlashToken(c))
        return;
    if (scanArithOperatorToken(c))
        return;
    if (scanTwoCharOperatorToken(c))
        return;
    scanLiteralOrUnknownToken(c);
}

// ============================================================
// R132-B fix: scanToken 拆分 helper
// ============================================================

bool Lexer::scanWhitespaceToken(char c) {
    // 空白字符：直接丢弃（advance 已消费）
    // 注：'\r' 不会出现在此处，advance() 已将 \r 和 \r\n 统一返回 '\n'
    switch (c) {
    case ' ':
    case '\t':
    case '\n':
    case '\f': // L3 fix: form feed
    case '\v': // L3 fix: vertical tab
        return true;
    default:
        return false;
    }
}

bool Lexer::scanDelimiterToken(char c) {
    switch (c) {
    // 分隔符
    case '(':
        addToken(TokenType::TK_LPAREN);
        return true;
    case ')':
        addToken(TokenType::TK_RPAREN);
        return true;
    case '{':
        addToken(TokenType::TK_LBRACE);
        return true;
    case '}':
        addToken(TokenType::TK_RBRACE);
        return true;
    case ';':
        addToken(TokenType::TK_SEMICOLON);
        return true;
    case ',':
        addToken(TokenType::TK_COMMA);
        return true;
    // 新增分隔符
    case '[':
        addToken(TokenType::TK_LBRACKET);
        return true;
    case ']':
        addToken(TokenType::TK_RBRACKET);
        return true;
    case ':':
        addToken(TokenType::TK_COLON);
        return true;
    // AUDIT-P2.7 fix: '?' 用于可选类型注解 T?（如 int?, string?）。
    //   Token.h 无法修改（不在允许修改的文件清单内），复用已完全废弃的 TK_FUNC 槽位。
    //   TK_FUNC 原为 "func" 关键字，Lexer 已将 "func" 统一映射到 TK_FUN（见 keywords()），
    //   TK_FUNC 枚举值从未被任何代码产生或检查（仅存在于 Token.h 定义与 typeToString）。
    //   复用此死槽位避免新增 Token.h 枚举值，Parser.parseTypeAnnotation 通过
    //   check(TK_FUNC) 识别 '?' 并追加 "?" 后缀到类型注解字符串。
    case '?':
        addToken(TokenType::TK_FUNC);
        return true;
    case '.':
        // .123 → 浮点数前导点
        if (isAsciiDigit(peek())) {
            number();
        } else {
            addToken(TokenType::TK_DOT);
        }
        return true;
    default:
        return false;
    }
}

bool Lexer::scanSlashToken(char c) {
    if (c != '/')
        return false;

    // 单行注释
    if (match('/')) {
        // 捕获注释文本（含 // 前缀）
        size_t commentStart = start_;
        // P0-2 fix: 同时检查 \r 和 \n，避免 CRLF 下 advance() 消耗 \r\n 后
        // peek() 跳过 \n 导致注释吞掉下一行内容
        while (!isAtEnd() && peek() != '\n' && peek() != '\r')
            advance();
        std::string commentText(source_, commentStart, current_ - commentStart);
        Token tok;
        tok.type = TokenType::TK_LINE_COMMENT;
        tok.lexeme = commentText;
        tok.line = line_;
        // BUG-LEX-AUDIT-3 fix: 统一用 UTF-8 感知 columnAt 计算列号，
        // 与 addToken() 路径保持一致（D11 fix 遗漏了注释 token 路径）。
        tok.column = columnAt(static_cast<int>(commentStart));
        tokens_.push_back(tok);
        return true;
    }
    if (match('*')) {
        // 块注释 /* ... */（支持嵌套）
        size_t commentStart = start_;
        int startLine = line_;
        // BUG-LEX-AUDIT-3 fix: 同行注释，统一用 columnAt
        int startCol = columnAt(static_cast<int>(commentStart));
        int depth = 1; // 嵌套深度
        while (!isAtEnd() && depth > 0) {
            if (peek() == '/' && peekNext() == '*') {
                advance();
                advance(); // 消耗 /*
                depth++;
            } else if (peek() == '*' && peekNext() == '/') {
                advance();
                advance(); // 消耗 */
                depth--;
            } else {
                advance();
            }
        }
        if (depth > 0) {
            errorToken("未终止的块注释", startLine, startCol);
            return true;
        }
        std::string commentText(source_, commentStart, current_ - commentStart);
        Token tok;
        tok.type = TokenType::TK_BLOCK_COMMENT;
        tok.lexeme = commentText;
        tok.line = startLine;
        tok.column = startCol;
        tokens_.push_back(tok);
        return true;
    }
    addToken(TokenType::TK_SLASH);
    return true;
}

bool Lexer::scanArithOperatorToken(char c) {
    switch (c) {
    case '+':
        addToken(TokenType::TK_PLUS);
        return true;
    case '-':
        addToken(TokenType::TK_MINUS);
        return true;
    case '*':
        addToken(TokenType::TK_STAR);
        return true;
    case '%':
        addToken(TokenType::TK_PERCENT);
        return true;
    default:
        return false;
    }
}

bool Lexer::scanTwoCharOperatorToken(char c) {
    // R99 枚举与 ADT + match：`=>` 箭头分隔符（match case 专用）。
    // 优先检查 `=>` 以避免被 `==` 查找表吞掉 `=`。
    if (c == '=' && match('>')) {
        addToken(TokenType::TK_ARROW);
        return true;
    }

    // 双字符运算符查找表
    struct TwoCharOp {
        char first;
        char second;        // 匹配的第二字符（'\0' 表示无匹配）
        TokenType dualType; // 双字符类型
        TokenType soloType; // 单字符类型
        const char* hint;   // 错误提示（soloType 为 TK_ERROR 时使用）
    };
    static const TwoCharOp twoCharOps[] = {
        {'=', '=', TokenType::TK_EQ, TokenType::TK_ASSIGN, nullptr},
        {'!', '=', TokenType::TK_NEQ, TokenType::TK_NOT, nullptr},
        {'<', '=', TokenType::TK_LEQ, TokenType::TK_LT, nullptr},
        {'>', '=', TokenType::TK_GEQ, TokenType::TK_GT, nullptr},
        {'&', '&', TokenType::TK_ERROR, TokenType::TK_ERROR, "请使用 'and' 关键字代替 '&'"},
        {'|', '|', TokenType::TK_ERROR, TokenType::TK_ERROR, "请使用 'or' 关键字代替 '|'"},
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
    if (!entry)
        return false;
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
    return true;
}

void Lexer::scanLiteralOrUnknownToken(char c) {
    // 字符串字面量
    if (c == '"') {
        string();
        return;
    }
    // 数字
    if (isAsciiDigit(c)) {
        number();
        return;
    }
    // 标识符或关键字
    if (isAsciiAlpha(c) || c == '_') {
        identifier();
        return;
    }
    // 无法识别的字符
    // BUG-LPA-06 fix: 检测 UTF-8 多字节字符首字节，消费完整码位生成单个错误。
    //   原实现按单字节处理，3 字节 UTF-8 字符（如中文）产生 3 个错误，
    //   且错误消息中字符显示为单字节乱码，无法识别原字符。
    unsigned char uc = static_cast<unsigned char>(c);
    int codepointLen = Utf8::byteLength(uc);
    if (codepointLen > 1) {
        std::string utf8char(1, c);
        for (int i = 1; i < codepointLen && !isAtEnd(); ++i) {
            utf8char += advance();
        }
        errorToken("意外字符 '" + utf8char + "'（标识符仅支持 ASCII）");
    } else {
        errorToken(std::string("意外字符 '") + c + "'");
    }
}

/// 识别标识符或关键字：消费 [A-Za-z0-9_]*，先拦截保留前缀 '__'，
/// 再查关键字表（命中则按字面量类型发 Token），否则发 TK_IDENTIFIER。
void Lexer::identifier() {
    while (!isAtEnd() && (isAsciiAlphaNum(peek()) || peek() == '_')) {
        advance();
    }
    std::string text(source_, start_, current_ - start_);

    // #10 fix: 保留 __ 前缀给编译器内部使用（__blk_save_*, __wb_idx_*）
    // 拓展二期·语言（运算符重载）：dunder 方法名白名单——__add/__sub/__mul/
    // __div/__mod 允许用户在类中定义，供 instance 算术运算分派调用。
    // 白名单与编译器内部名（__blk_save_*/__wb_idx_*/__mod_*/__qmark_unwrap）
    // 无交集，保留前缀的防冲突目的不受影响。
    if (text.size() >= 2 && text[0] == '_' && text[1] == '_') {
        static const char* kDunderWhitelist[] = {"__add", "__sub", "__mul", "__div", "__mod"};
        bool whitelisted = false;
        for (const char* w : kDunderWhitelist) {
            if (text == w) {
                whitelisted = true;
                break;
            }
        }
        if (!whitelisted) {
            errorToken("标识符 '" + text + "' 使用了保留前缀 '__'（编译器内部使用）");
            return;
        }
    }

    // 查关键字表（PERF: 使用二分查找替代 hash map）
    auto kwType = lookupKeywordFast(text);
    if (kwType.has_value()) {
        TokenType type = *kwType;
        // true 和 false 有字面量值
        if (type == TokenType::TK_TRUE) {
            addToken(type, std::move(text), true); // A1 fix: variant bool
        } else if (type == TokenType::TK_FALSE) {
            addToken(type, std::move(text), false); // A1 fix: variant bool
        } else if (type == TokenType::TK_NULL) {
            // null 关键字有字面量值
            addToken(type, std::move(text), std::monostate{}); // A1 fix: variant monostate
        } else {
            addToken(type, std::move(text));
        }
    } else {
        addToken(TokenType::TK_IDENTIFIER, std::move(text));
    }
}

/// 解析数字字面量：处理前导点浮点、拒绝 0x/0b/0o 前缀与前导零，
/// 识别小数与科学计数法，最后用 locale-independent 的 std::from_chars 转换为 int64/double，
/// 溢出或格式错误时发出错误 Token 而非崩溃。
void Lexer::number() {
    // 前导点浮点（.123）：scanToken 已消耗 '.'，start_ 指向 '.'，跳过整数部分
    bool isFloat = (source_[start_] == '.');

    // M6 fix: 检测 0x/0b/0o 前缀，发出明确错误（而非静默分为两个 token）
    if (!isFloat && !isAtEnd() && source_[start_] == '0' &&
        (peek() == 'x' || peek() == 'X' || peek() == 'b' || peek() == 'B' || peek() == 'o' || peek() == 'O')) {
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

    // AUDIT-P2-CORRECT fix: 拒绝前导零（如 007, 008, 099），避免教学歧义与
    // 未来八进制解析风险。MiniLang 作为教学语言，前导零在 C 中被误解为八进制、
    // Python3 中报错，静默接受会误导学习者。例外：单独的 "0" 合法；
    // "0." 开头的浮点数由下方 isFloat 分支处理（source_[start_]=='.' 为 false，
    // 但 peek()=='.' 不会进入此分支，因 isAsciiDigit('.') 为 false）。
    if (!isFloat && !isAtEnd() && source_[start_] == '0' && isAsciiDigit(peek())) {
        // 前导零：0 后跟数字，报错并消费整个数字序列
        while (!isAtEnd() && isAsciiDigit(peek())) {
            advance();
        }
        std::string text(source_, start_, current_ - start_);
        // 提供等价的合法值建议
        int64_t suggestion = 0;
        auto [ptr, ec] = std::from_chars(text.data() + 1, text.data() + text.size(), suggestion);
        std::string hint = (ec == std::errc()) ? "，请使用 " + std::to_string(suggestion) : "";
        errorToken("不允许前导零: " + text + hint);
        return;
    }

    while (!isAtEnd() && isAsciiDigit(peek())) {
        advance();
    }

    // 浮点数：小数部分（仅当 '.' 后紧跟数字时才视为浮点，避免 123.foo 被误分词）
    if (!isFloat && !isAtEnd() && peek() == '.' && (static_cast<size_t>(current_) + 1 < source_.size()) &&
        isAsciiDigit(source_[current_ + 1])) {
        isFloat = true;
        advance(); // 消耗 '.'
        while (!isAtEnd() && isAsciiDigit(peek())) {
            advance();
        }
    }

    // M5 fix: 整数后直接跟 .e/.E 形式（如 123.e5），消耗 '.' 后进入科学计数法
    if (!isFloat && !isAtEnd() && peek() == '.' && (static_cast<size_t>(current_) + 1 < source_.size()) &&
        (source_[current_ + 1] == 'e' || source_[current_ + 1] == 'E')) {
        isFloat = true;
        advance(); // 消耗 '.'
    }

    // R53-LEX-REVERT fix: 回滚 AUDIT-P3-ROUND53 的"123. 报错"逻辑。
    // 该 fix 让 "1." 和 "123.foo" 报错"数字字面量小数点后需有数字"，但
    // TestLexer::Number_IntegerFollowedByDotAndIdentifier 和
    // LexerAudit::TrailingDecimalPoint 明确期望分词为 INT + DOT（+ IDENTIFIER），
    // 这是项目的设计契约——MiniLang 不支持方法调用语法 123.foo()，但 Lexer
    // 应保持宽容分词，将语义判断交给 Parser。原 fix 改变了既定行为，导致回归。
    // 现恢复"123. 后无数字/指数 → 分词为 TK_INT_LIT + TK_DOT"的行为。

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
            // AUDIT-R3 P2 fix: 区分下溢与上溢——from_chars 对两者均报 result_out_of_range。
            // 下溢（如 1e-400、0.000...01）主流语言语义为钳制到 0.0，误报"溢出"会
            // 拒绝合法的极小字面量。
            // AUDIT-R4 BUG-07 fix: 原启发式"负指数或以 '0' 开头即下溢"会把 0.1e999
            // 这类真实上溢字面量误判为下溢并静默钳制为 0.0。改为解析十进制数量级：
            // 有效指数 = 显式指数 E + 首个非零尾数位相对小数点的幂次 p，
            // 有效指数 < 0 → 下溢（钳制 0.0），否则 → 上溢（报错）。
            // out_of_range 保证 |有效指数| > ~308，符号判定无歧义。
            long long expVal = 0;
            size_t ePos = text.find_first_of("eE");
            if (ePos != std::string::npos) {
                bool negExp = false;
                size_t di = ePos + 1;
                if (di < text.size() && (text[di] == '+' || text[di] == '-')) {
                    negExp = (text[di] == '-');
                    ++di;
                }
                for (; di < text.size() && isAsciiDigit(text[di]); ++di) {
                    if (expVal < 1000000000) // 钳制防指数数字串本身溢出，足够判定符号
                        expVal = expVal * 10 + (text[di] - '0');
                }
                if (negExp)
                    expVal = -expVal;
            }
            // 首个非零尾数位相对小数点的幂次 p（"123.4" 的 '1' → p=2；"0.001" 的 '1' → p=-3）
            std::string_view mant = std::string_view(text).substr(0, ePos == std::string::npos ? text.size() : ePos);
            size_t dotPos = mant.find('.');
            long long p = 0;
            size_t firstSig = std::string::npos;
            for (size_t i = 0; i < mant.size(); ++i) {
                if (mant[i] >= '1' && mant[i] <= '9') {
                    firstSig = i;
                    break;
                }
            }
            if (firstSig != std::string::npos) {
                if (dotPos == std::string_view::npos || firstSig < dotPos) {
                    size_t intEnd = (dotPos == std::string_view::npos) ? mant.size() : dotPos;
                    p = static_cast<long long>(intEnd - firstSig) - 1;
                } else {
                    p = -static_cast<long long>(firstSig - dotPos);
                }
            }
            if (expVal + p < 0) {
                addToken(TokenType::TK_FLOAT_LIT, std::move(text), 0.0); // 下溢钳制到 0.0
            } else {
                errorToken("浮点数溢出: " + text);
            }
        } else if (ec != std::errc() || ptr != text.data() + text.size()) {
            errorToken("浮点数格式错误: " + text);
        } else {
            addToken(TokenType::TK_FLOAT_LIT, std::move(text), val); // A1 fix: variant double
        }
    } else {
        // L-P1-3 fix: 使用 std::from_chars 替代 std::stoll，locale-independent
        int64_t val = 0;
        auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), val);
        if (ec == std::errc::result_out_of_range) {
            // Bug #40 fix: 9223372036854775808 是 INT64_MIN 的绝对值，
            // 存储为 INT64_MIN，后续一元负号会正确处理。
            if (text == "9223372036854775808") {
                addToken(TokenType::TK_INT_LIT, std::move(text), INT64_MIN);
            } else {
                errorToken("整数溢出: " + text);
            }
        } else if (ec != std::errc() || ptr != text.data() + text.size()) {
            errorToken("整数格式错误: " + text);
        } else {
            addToken(TokenType::TK_INT_LIT, std::move(text), val); // A1 fix: variant int64_t
        }
    }
}

void Lexer::string() {
    string(false);
}

/// 解析字符串字面量（支持 {expr} 插值）：消费转义序列，遇 { 递归扫描插值表达式并维护嵌套深度与 Token 上限。
void Lexer::string(bool isInterp) {
    int startLine = line_;
    // BUG-LEX-AUDIT-3 fix: 统一用 UTF-8 感知 columnAt 计算列号，
    // 与 addToken() 路径保持一致（D11 fix 仅改了 addToken，遗漏直接 emplace_back 路径）。
    int startCol = columnAt(start_);
    std::string value;
    // P-07 fix: 预估字符串容量，避免逐字符 += 反复 realloc
    // P0 fix: 限制 reserve 上限，防止未闭合字符串触发 GB 级内存分配
    // BUG-LEX-AUDIT-5 fix: 上限从 1MB 降为 64KB。原 1MB 在 MAX_INTERP_DEPTH=64 嵌套场景
    // 峰值 reserved 达 64MB 但大多未使用；64KB 上限下嵌套峰值仅 4MB，平衡 OOM 防护与内存压力。
    size_t reserveCap = current_ < source_.size() ? (source_.size() - current_) : 0;
    if (reserveCap > 64 * 1024)
        reserveCap = 64 * 1024;
    value.reserve(reserveCap);

    while (!isAtEnd() && peek() != '"') {
        // F7: 检测插值起始 {
        if (peek() == '{') {
            if (!handleInterpolation(startLine, startCol, value, isInterp))
                return;
            continue;
        }

        // 换行处理由 advance() 统一完成（line_++ 和 lineStart_ 更新），
        // 此处不再手动递增，否则会导致行号双重递增。
        if (peek() == '\\') {
            if (!handleEscape(startLine, startCol, value))
                return;
            continue;
        }

        // PERF-ROUND53 fix: 批量扫描连续普通字符（直到遇 \、{、"、\r、\n 或 EOF）。
        // 原实现逐字符调用 advance() + value += char，对长字符串（如 1KB 文本）
        // 产生 1024 次 advance() 调用 + 1024 次 value += char（可能触发多次 realloc）。
        // 批量扫描用 value.append(ptr, len) 一次性追加，减少函数调用与 realloc 次数。
        // 注：\r/\n 必须通过 advance() 处理以维护行号（advance 将 \r/\r\n 规范化为 \n），
        // 故批量扫描在遇到它们时停止，由下次循环迭代调用 advance()。
        // 多字节 UTF-8 字节（0x80-0xFF）均不与特殊字符（\=0x5C、{=0x7B、"=0x22、
        // \r=0x0D、\n=0x0A）冲突，可安全批量扫描。
        size_t runStart = current_;
        while (!isAtEnd()) {
            char c = source_[current_];
            if (c == '"' || c == '\\' || c == '{' || c == '\r' || c == '\n')
                break;
            current_++;
        }
        if (current_ > runStart) {
            value.append(source_.data() + runStart, current_ - runStart);
        } else {
            // 首个字符即为 \r/\n（其他特殊字符已被外层 if 拦截），
            // 调用 advance() 维护行号并将规范化后的字符（\n）加入 value
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
    // BUG-LEX-AUDIT-4 fix: 末尾片段也需检查 MAX_TOKEN_COUNT
    if (tokens_.size() >= MAX_TOKEN_COUNT) {
        diagnostics_.addError("Token 数量超过上限 " + std::to_string(MAX_TOKEN_COUNT) + "，源代码可能包含过多 token",
                              startLine, startCol, DiagSource::Lexer, DiagCodes::kTooManyTokens);
        return;
    }
    std::string text(source_, start_, current_ - start_);
    tokens_.emplace_back(finalType, std::move(text), std::move(value), startLine, startCol); // A1 fix: variant string
}

// ============================================================
// R131 fix: handleInterpolation - 处理字符串中的 '{' 插值起始
// 提取自 string() 主循环的 '{' 分支（117 行），含插值深度检查、
// TK_STRING_PART/TK_INTERP_START 发射、嵌套表达式扫描、TK_INTERP_END 发射
// 返回 true=继续外层 string 循环，false=终止 string()
// ============================================================
bool Lexer::handleInterpolation(int& startLine, int& startCol, std::string& value, bool& isInterp) {
    // L-P1-1: 插值嵌套深度检查，防止栈溢出
    if (interpDepth_ >= MAX_INTERP_DEPTH) {
        errorToken("字符串插值嵌套过深（最大 " + std::to_string(MAX_INTERP_DEPTH) + " 层）", startLine, startCol);
        return false;
    }
    interpDepth_++;
    // AUDIT-R3 P2 fix: RAII 守卫替代手动配对递减——原实现中 4 处 MAX_TOKEN_COUNT
    // 早退路径（return false）遗漏递减，与"错误退出时也减少深度"的契约矛盾。
    struct InterpDepthGuard {
        int& d;
        ~InterpDepthGuard() { --d; }
    } depthGuard{interpDepth_};
    // 发出前面的文本片段（TK_STRING_PART 表示插值字符串的一部分）
    TokenType partType = isInterp ? TokenType::TK_STRING_PART : TokenType::TK_STRING_LIT;
    // 如果是插值字符串的第一个片段，用 TK_STRING_LIT；后续片段用 TK_STRING_PART
    // 但为简化 Parser 逻辑，统一：插值字符串中所有文本片段都用 TK_STRING_PART，
    // 仅当整个字符串无插值时用 TK_STRING_LIT（由下方闭合处判断）
    // BUG-LEX-AUDIT-4 fix: 直接 emplace_back 绕过 scanToken() 的 MAX_TOKEN_COUNT 检查，
    // 需在此显式检查，防止含大量小插值的字符串绕过 DoS 防护。
    if (tokens_.size() >= MAX_TOKEN_COUNT) {
        diagnostics_.addError("Token 数量超过上限 " + std::to_string(MAX_TOKEN_COUNT) + "，源代码可能包含过多 token",
                              startLine, startCol, DiagSource::Lexer, DiagCodes::kTooManyTokens);
        return false;
    }
    std::string text(source_, start_, current_ - start_);
    tokens_.emplace_back(partType, std::move(text), std::move(value), startLine,
                         startCol); // A1 fix: variant string

    // 发出 TK_INTERP_START
    // BUG-LEX-AUDIT-2 fix: 在 advance() 消耗 '{' 之前记录列号，
    // 原实现 advance 后用 start_ 计算，但 start_ 仍指向片段起始而非 '{' 位置。
    int braceLine = line_;
    int braceCol = columnAt(current_);
    // 消耗 {
    advance();
    if (tokens_.size() >= MAX_TOKEN_COUNT) { // BUG-LEX-AUDIT-4
        diagnostics_.addError("Token 数量超过上限 " + std::to_string(MAX_TOKEN_COUNT) + "，源代码可能包含过多 token",
                              braceLine, braceCol, DiagSource::Lexer, DiagCodes::kTooManyTokens);
        return false;
    }
    tokens_.emplace_back(TokenType::TK_INTERP_START, "{", std::monostate{}, braceLine,
                         braceCol); // A1 fix: variant monostate

    // 扫描表达式直到 }（支持嵌套大括号，如对象字面量）
    // 更新 start_ 到表达式起始位置，确保 scanToken() 的 addToken() 正确提取 lexeme
    start_ = current_;
    int braceDepth = 1;
    // BUG-LEX-AUDIT-1 fix: 内层插值循环必须检查 MAX_TOKEN_COUNT。
    // 原实现仅 scanToken() 入口检查，但检查触发时 scanToken() "返回不前进"，
    // 而 '{' 与 default 分支只调用 scanToken() 不调用 advance()，形成无限循环。
    // 修复：循环体首行检查并 return，让 string() 退出，scan() 主循环也 break。
    while (!isAtEnd() && braceDepth > 0) {
        if (tokens_.size() >= MAX_TOKEN_COUNT) {
            diagnostics_.addError("Token 数量超过上限 " + std::to_string(MAX_TOKEN_COUNT) +
                                      "，源代码可能包含过多 token",
                                  line_, currentColumn(), DiagSource::Lexer, DiagCodes::kTooManyTokens);
            return false;
        }
        // 跳过空白
        if (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r') {
            advance();
            continue;
        }
        if (peek() == '{') {
            braceDepth++;
            start_ = current_; // 更新 start_ 以便 scanToken 正确提取
            scanToken();
        } else if (peek() == '}') {
            braceDepth--;
            if (braceDepth == 0) {
                // BUG-LEX-AUDIT-2 fix: 在 advance() 消耗 '}' 之前记录列号。
                int endLine = line_;
                int endCol = columnAt(current_);
                advance();                               // 消耗 }
                if (tokens_.size() >= MAX_TOKEN_COUNT) { // BUG-LEX-AUDIT-4
                    diagnostics_.addError("Token 数量超过上限 " + std::to_string(MAX_TOKEN_COUNT) +
                                              "，源代码可能包含过多 token",
                                          endLine, endCol, DiagSource::Lexer, DiagCodes::kTooManyTokens);
                    return false;
                }
                tokens_.emplace_back(TokenType::TK_INTERP_END, "}", std::monostate{}, endLine,
                                     endCol); // A1 fix: variant monostate
                break;
            }
            start_ = current_; // 更新 start_ 以便 scanToken 正确提取
            scanToken();
        } else if (peek() == '"') {
            // 嵌套字符串（可能含插值）
            // AUDIT-BUG-F10 fix: advance 前更新 start_，与 { } 默认分支一致。
            // 原实现缺少 start_=current_，导致嵌套字符串首 token 列号/lexeme 错误。
            start_ = current_;
            advance();
            string(false);     // 嵌套字符串作为独立 TK_STRING_LIT，非插值片段
            start_ = current_; // 更新 start_ 以便后续 scanToken 正确提取
        } else {
            start_ = current_; // 更新 start_ 以便 scanToken 正确提取
            scanToken();
        }
    }
    if (braceDepth > 0) {
        // AUDIT-R3 P2 fix: interpDepth_ 递减由 InterpDepthGuard 析构统一处理
        errorToken("未终止的插值表达式（缺少 }）", startLine, startCol);
        return false;
    }

    // L-P1-1: 本层插值已闭合（深度由 InterpDepthGuard 析构递减）

    // 继续扫描字符串剩余部分（标记为插值片段）
    start_ = current_;
    startLine = line_;
    // AUDIT-P2 fix: 与同文件其他路径（addToken/columnAt）保持一致，使用 UTF-8 码位列号。
    // 原实现用字节偏移 (start_ - lineStart_) + 1，若本行之前含多字节 UTF-8 字符
    // （如中文），字节偏移 > 码位列号，导致插值片段诊断列号偏移、编辑器高亮位置错位。
    startCol = columnAt(start_);
    value.clear();
    isInterp = true;
    return true;
}

// ============================================================
// R131 fix: handleEscape - 处理字符串中的 '\\' 转义序列
// 提取自 string() 主循环的 '\\' 分支（267 行）。
// switch 分发简单转义（\n/\t/\r/\\/\"/\'/\0/\b/\f/\a/\v）直接 inline，
// \x 调用 handleHexEscape，\u 调用 handleUnicodeEscape，default 错误恢复。
// 返回 true=继续外层循环，false=终止 string()
// ============================================================
bool Lexer::handleEscape(int startLine, int startCol, std::string& value) {
    advance(); // 消耗反斜杠
    if (isAtEnd()) {
        errorToken("未终止的字符串", startLine, startCol);
        return false;
    }
    char esc = advance();
    switch (esc) {
    case 'n':
        value += '\n';
        break;
    case 't':
        value += '\t';
        break;
    case 'r':
        value += '\r';
        break;
    case '\\':
        value += '\\';
        break;
    case '"':
        value += '"';
        break;
    case '\'':
        value += '\'';
        break;
    case '0':
        value += '\0';
        break;
    case 'b':
        value += '\b';
        break;
    case 'f':
        value += '\f';
        break;
    case 'a':
        value += '\a';
        break;
    case 'v':
        value += '\v';
        break;
    case 'x':
        return handleHexEscape(startLine, startCol, value);
    case 'u':
        return handleUnicodeEscape(startLine, startCol, value);
    default:
        // AUDIT-BUG-L1 fix: 未知转义序列应报错，而非静默接受为字面字符。
        // 原实现将 \q 等存储为 "\\q" 两字符，违反"非法输入应被拒绝"原则。
        errorToken(std::string("未知转义序列 '\\") + esc + "'", startLine, startCol);
        // 恢复：跳过到字符串结束或 EOF，避免级联产生误导性错误
        while (!isAtEnd() && peek() != '"') {
            if (peek() == '\\') {
                advance();
                if (!isAtEnd())
                    advance();
            } else
                advance();
        }
        if (!isAtEnd())
            advance(); // 消耗闭合的 '"'
        return false;
    }
    return true;
}

// ============================================================
// R131 fix: handleHexEscape - 处理 \xNN 十六进制字节转义
// 提取自 handleEscape 的 case 'x'（39 行）。
// 读取 2 位十六进制 → 编码为单字节。错误恢复跳过到字符串结束或 EOF。
// 返回 true=继续外层循环，false=终止 string()
// ============================================================
bool Lexer::handleHexEscape(int startLine, int startCol, std::string& value) {
    // AUDIT-P2 fix: \xNN — 2 位十六进制字节转义
    if (isAtEnd()) {
        errorToken("未终止的字符串", startLine, startCol);
        return false;
    }
    char h1 = advance();
    if (isAtEnd()) {
        errorToken("未终止的字符串", startLine, startCol);
        return false;
    }
    char h2 = advance();
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    int v1 = hexVal(h1), v2 = hexVal(h2);
    if (v1 < 0 || v2 < 0) {
        errorToken(std::string("无效的十六进制转义 '\\x") + h1 + h2 + "'", startLine, startCol);
        while (!isAtEnd() && peek() != '"') {
            if (peek() == '\\') {
                advance();
                if (!isAtEnd())
                    advance();
            } else
                advance();
        }
        if (!isAtEnd())
            advance();
        return false;
    }
    value += static_cast<char>((v1 << 4) | v2);
    return true;
}

// ============================================================
// R131 fix: handleUnicodeEscape - 处理 \u Unicode 转义
// 提取自 handleEscape 的 case 'u'（170 行）。
// 若 peek()=='{' 走 \u{XXXXXX} 扩展语法（1-6 位十六进制），
// 否则走 \uXXXX 标准 4 位语法。验证码点范围 [0, 0x10FFFF] 且非代理码点
// [0xD800, 0xDFFF]，编码为 UTF-8（1-4 字节）。
// 返回 true=继续外层循环，false=终止 string()
// ============================================================
bool Lexer::handleUnicodeEscape(int startLine, int startCol, std::string& value) {
    // AUDIT-P3.12 fix: \u{XXXXXX} 扩展语法支持非 BMP 字符（如 emoji）。
    //   保留现有 4 位 \uXXXX 语法向后兼容。当 \u 后紧跟 '{' 时，读取
    //   1-6 位十六进制直到 '}'，验证码点范围 0x000000-0x10FFFF 且非
    //   代理码点 0xD800-0xDFFF，编码为 UTF-8（1-4 字节）。
    if (peek() == '{') {
        advance(); // 消费 '{'
        uint32_t codepoint = 0;
        int hexCount = 0;
        bool hexError = false;
        while (peek() != '}' && peek() != '\0' && !isAtEnd()) {
            char hc = advance();
            int val = 0;
            if (hc >= '0' && hc <= '9')
                val = hc - '0';
            else if (hc >= 'a' && hc <= 'f')
                val = hc - 'a' + 10;
            else if (hc >= 'A' && hc <= 'F')
                val = hc - 'A' + 10;
            else {
                errorToken("无效的 Unicode 转义序列: 非法十六进制字符", startLine, startCol);
                hexError = true;
                break;
            }
            codepoint = (codepoint << 4) | static_cast<uint32_t>(val);
            hexCount++;
            if (hexCount > 6) {
                errorToken("Unicode 转义序列最多 6 位十六进制", startLine, startCol);
                hexError = true;
                break;
            }
        }
        if (hexError) {
            while (!isAtEnd() && peek() != '"') {
                if (peek() == '\\') {
                    advance();
                    if (!isAtEnd())
                        advance();
                } else
                    advance();
            }
            if (!isAtEnd())
                advance();
            return false;
        }
        if (peek() != '}') {
            errorToken("Unicode 转义序列缺少闭合 '}'", startLine, startCol);
            return false;
        }
        advance(); // 消费 '}'
        // AUDIT-P2-ROUND49 fix: 三处码点验证失败路径（空转义/码点超出范围/代理码点）
        // 直接 return 未跳过到字符串结束，产生垃圾错误。改为统一标记 codepointError
        // 后复用与 hexError 相同的跳过到字符串结束逻辑，对齐 \xNN/\uXXXX 错误恢复。
        bool codepointError = false;
        if (hexCount == 0) {
            errorToken("Unicode 转义序列不能为空", startLine, startCol);
            codepointError = true;
        } else if (codepoint > 0x10FFFF) {
            errorToken("Unicode 码点超出范围 (最大 0x10FFFF)", startLine, startCol);
            codepointError = true;
        } else if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
            errorToken("无效的 Unicode 代理码点", startLine, startCol);
            codepointError = true;
        }
        if (codepointError) {
            while (!isAtEnd() && peek() != '"') {
                if (peek() == '\\') {
                    advance();
                    if (!isAtEnd())
                        advance();
                } else
                    advance();
            }
            if (!isAtEnd())
                advance();
            return false;
        }
        // 编码为 UTF-8（1-4 字节）
        if (codepoint <= 0x7F) {
            value += static_cast<char>(codepoint);
        } else if (codepoint <= 0x7FF) {
            value += static_cast<char>(0xC0 | (codepoint >> 6));
            value += static_cast<char>(0x80 | (codepoint & 0x3F));
        } else if (codepoint <= 0xFFFF) {
            value += static_cast<char>(0xE0 | (codepoint >> 12));
            value += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
            value += static_cast<char>(0x80 | (codepoint & 0x3F));
        } else {
            value += static_cast<char>(0xF0 | (codepoint >> 18));
            value += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
            value += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
            value += static_cast<char>(0x80 | (codepoint & 0x3F));
        }
        return true;
    }
    // AUDIT-P2 fix: \uXXXX — 4 位十六进制 Unicode 码点，编码为 UTF-8
    char d[4];
    for (int i = 0; i < 4; ++i) {
        if (isAtEnd()) {
            errorToken("未终止的字符串", startLine, startCol);
            return false;
        }
        d[i] = advance();
    }
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    int cp = 0;
    bool valid = true;
    for (int i = 0; i < 4; ++i) {
        int v = hexVal(d[i]);
        if (v < 0) {
            valid = false;
            break;
        }
        cp = (cp << 4) | v;
    }
    if (!valid) {
        errorToken(std::string("无效的 Unicode 转义 '\\u") + d[0] + d[1] + d[2] + d[3] + "'", startLine, startCol);
        while (!isAtEnd() && peek() != '"') {
            if (peek() == '\\') {
                advance();
                if (!isAtEnd())
                    advance();
            } else
                advance();
        }
        if (!isAtEnd())
            advance();
        return false;
    }
    // AUDIT-P2-CORRECT fix: 拒绝 Unicode 代理码点（0xD800-0xDFFF）。
    // 代理码点不是合法的 Unicode 标量值，将其编码为 UTF-8 会产生
    // 非标准 "WTF-8"（3 字节序列），严格 UTF-8 校验器会拒绝。
    if (cp >= 0xD800 && cp <= 0xDFFF) {
        errorToken(std::string("无效的 Unicode 代理码点 '\\u") + d[0] + d[1] + d[2] + d[3] +
                       "'（代理码点不能直接编码）",
                   startLine, startCol);
        while (!isAtEnd() && peek() != '"') {
            if (peek() == '\\') {
                advance();
                if (!isAtEnd())
                    advance();
            } else
                advance();
        }
        if (!isAtEnd())
            advance();
        return false;
    }
    // 编码为 UTF-8
    if (cp <= 0x7F) {
        value += static_cast<char>(cp);
    } else if (cp <= 0x7FF) {
        value += static_cast<char>(0xC0 | (cp >> 6));
        value += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        value += static_cast<char>(0xE0 | (cp >> 12));
        value += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        value += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return true;
}

/// 构造并追加一个 Token：用 UTF-8 感知的 columnAt 计算列号，字面量默认为 monostate。
void Lexer::addToken(TokenType type) {
    std::string text(source_, start_, current_ - start_);
    int col = columnAt(start_);
    tokens_.emplace_back(type, std::move(text), std::monostate{}, line_, col); // A1 fix: variant monostate
}

void Lexer::addToken(TokenType type, TokenLiteral literal) {
    std::string text(source_, start_, current_ - start_);
    int col = columnAt(start_);
    tokens_.emplace_back(type, std::move(text), std::move(literal), line_, col);
}

void Lexer::addToken(TokenType type, std::string&& text) {
    int col = columnAt(start_);
    tokens_.emplace_back(type, std::move(text), std::monostate{}, line_, col); // A1 fix: variant monostate
}

void Lexer::addToken(TokenType type, std::string&& text, TokenLiteral literal) {
    int col = columnAt(start_);
    tokens_.emplace_back(type, std::move(text), std::move(literal), line_, col);
}

/// 报告词法错误：生成 TK_ERROR Token（携带行列），扫描器继续后续扫描而非中止。
void Lexer::errorToken(const std::string& message, int errorLine, int errorCol) {
    int col = (errorCol > 0) ? errorCol : columnAt(start_);
    int ln = (errorLine > 0) ? errorLine : line_;
    tokens_.emplace_back(TokenType::TK_ERROR, message, std::monostate{}, ln, col); // A1 fix: variant monostate
}

/// 返回当前读取位置的列号（经 columnAt 按 UTF-8 码位计算）。
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
