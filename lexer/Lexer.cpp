#include "lexer/Lexer.h"
#include <cctype>

// ============================================================
// Lexer 词法分析器实现
// ============================================================

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
    source_ = source;
    start_ = 0;
    current_ = 0;
    line_ = 1;
    lineStart_ = 0;
    tokens_.clear();
    diagnostics_.clear();

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
    }

    // 添加 EOF Token
    tokens_.emplace_back(TokenType::TK_EOF, "", Value::nullValue(), line_, currentColumn());

    // 从 TK_ERROR Token 中提取诊断信息
    for (const auto& tok : tokens_) {
        if (tok.type == TokenType::TK_ERROR) {
            diagnostics_.addError(tok.lexeme, tok.line, tok.column, DiagSource::Lexer);
        }
    }

    return tokens_;
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
    char c = advance();

    switch (c) {
    // 空白字符
    case ' ':
    case '\t':
    case '\r':
    case '\n':
    case '\f':   // L3 fix: form feed
    case '\v':   // L3 fix: vertical tab
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
        if (std::isdigit(static_cast<unsigned char>(peek()))) {
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
            while (!isAtEnd() && peek() != '\n') advance();
            std::string commentText(source_.substr(commentStart, current_ - commentStart));
            Token tok;
            tok.type = TokenType::TK_LINE_COMMENT;
            tok.lexeme = commentText;
            tok.line = line_;
            tok.column = static_cast<int>(commentStart - lineStart_) + 1;
            tokens_.push_back(tok);
        } else {
            addToken(TokenType::TK_SLASH);
        }
        break;
    case '%': addToken(TokenType::TK_PERCENT); break;

    // 可能是双字符运算符（使用查找表 O(1) 分派）
    case '=': case '!': case '<': case '>': case '&': case '|': {
        // 双字符运算符查找表: [首字符索引][后继字符] → TokenType
        // 首字符映射: '='→0, '!'→1, '<'→2, '>'→3, '&'→4, '|'→5
        static const struct {
            char first;
            char second;        // 匹配的第二字符（'\0' 表示无匹配）
            TokenType dualType; // 双字符类型
            TokenType soloType; // 单字符类型
            const char* hint;   // 错误提示（soloType 为 TK_ERROR 时使用）
        } twoCharOps[] = {
            {'=', '=', TokenType::TK_EQ,     TokenType::TK_ASSIGN, nullptr},
            {'!', '=', TokenType::TK_NEQ,    TokenType::TK_NOT,    nullptr},
            {'<', '=', TokenType::TK_LEQ,    TokenType::TK_LT,     nullptr},
            {'>', '=', TokenType::TK_GEQ,    TokenType::TK_GT,     nullptr},
            {'&', '&', TokenType::TK_ERROR,  TokenType::TK_ERROR,  "请使用 'and' 关键字代替 '&'"},
            {'|', '|', TokenType::TK_ERROR,  TokenType::TK_ERROR,  "请使用 'or' 关键字代替 '|'"},
        };
        // 查找表项（6 项，编译期初始化）
        for (const auto& entry : twoCharOps) {
            if (entry.first == c) {
                if (entry.second != '\0' && match(entry.second)) {
                    if (entry.dualType != TokenType::TK_ERROR)
                        addToken(entry.dualType);
                    else
                        errorToken(std::string("'") + c + c + "'（" + entry.hint + "）");
                } else if (entry.soloType != TokenType::TK_ERROR) {
                    addToken(entry.soloType);
                } else {
                    errorToken(std::string("意外字符 '") + c + "'（" + entry.hint + "）");
                }
                break;
            }
        }
        break;
    }

    // 字符串字面量
    case '"': string(); break;

    default:
        // 数字
        if (std::isdigit(static_cast<unsigned char>(c))) {
            number();
        }
        // 标识符或关键字
        else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
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
    while (!isAtEnd() && (std::isalnum(static_cast<unsigned char>(peek()))
                          || peek() == '_')) {
        advance();
    }
    std::string text(source_.substr(start_, current_ - start_));

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
            addToken(type, std::move(text), Value(true));
        } else if (type == TokenType::TK_FALSE) {
            addToken(type, std::move(text), Value(false));
        } else if (type == TokenType::TK_NULL) {
            // null 关键字有字面量值
            addToken(type, std::move(text), Value::nullValue());
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
        while (!isAtEnd() && std::isxdigit(static_cast<unsigned char>(peek()))) {
            advance();
        }
        std::string text(source_.substr(start_, current_ - start_));
        errorToken("不支持 " + std::string(1, prefix) + " 前缀字面量: " + text + "，请使用十进制表示");
        return;
    }

    while (!isAtEnd() && std::isdigit(static_cast<unsigned char>(peek()))) {
        advance();
    }

    // 浮点数：小数部分（仅当 '.' 后紧跟数字时才视为浮点，避免 123.foo 被误分词）
    if (!isFloat && !isAtEnd() && peek() == '.' &&
        (static_cast<size_t>(current_) + 1 < source_.size()) && std::isdigit(static_cast<unsigned char>(source_[current_ + 1]))) {
        isFloat = true;
        advance(); // 消耗 '.'
        while (!isAtEnd() && std::isdigit(static_cast<unsigned char>(peek()))) {
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
        if (isAtEnd() || !std::isdigit(static_cast<unsigned char>(peek()))) {
            errorToken("科学计数法格式错误: " + std::string(source_.substr(start_, current_ - start_)));
            return;
        }
        while (!isAtEnd() && std::isdigit(static_cast<unsigned char>(peek()))) {
            advance();
        }
    }

    std::string text(source_.substr(start_, current_ - start_));

    if (isFloat) {
        try {
            double val = std::stod(text);
            addToken(TokenType::TK_FLOAT_LIT, std::move(text), Value(val));
        } catch (const std::out_of_range&) {
            errorToken("浮点数溢出: " + text);
        } catch (const std::invalid_argument&) {
            errorToken("浮点数格式错误: " + text);
        }
    } else {
        try {
            long long val = std::stoll(text);
            addToken(TokenType::TK_INT_LIT, std::move(text), Value(static_cast<int64_t>(val)));
        } catch (const std::out_of_range&) {
            errorToken("整数溢出: " + text);
        } catch (const std::invalid_argument&) {
            errorToken("整数格式错误: " + text);
        }
    }
}

void Lexer::string() {
    int startLine = line_;
    int startCol = static_cast<int>(start_ - lineStart_) + 1;
    std::string value;

    while (!isAtEnd() && peek() != '"') {
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
                value += '\\';
                value += esc;
                break;
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
    // 使用字符串起始位置（startLine/startCol），避免多行字符串行号/列号错误
    std::string text(source_.substr(start_, current_ - start_));
    tokens_.emplace_back(TokenType::TK_STRING_LIT, std::move(text), Value(value), startLine, startCol);
}

void Lexer::addToken(TokenType type) {
    std::string text(source_.substr(start_, current_ - start_));
    int col = static_cast<int>(start_ - lineStart_) + 1;
    tokens_.emplace_back(type, text, Value::nullValue(), line_, col);
}

void Lexer::addToken(TokenType type, const Value& literal) {
    std::string text(source_.substr(start_, current_ - start_));
    int col = static_cast<int>(start_ - lineStart_) + 1;
    tokens_.emplace_back(type, text, literal, line_, col);
}

void Lexer::addToken(TokenType type, std::string&& text) {
    int col = static_cast<int>(start_ - lineStart_) + 1;
    tokens_.emplace_back(type, std::move(text), Value::nullValue(), line_, col);
}

void Lexer::addToken(TokenType type, std::string&& text, const Value& literal) {
    int col = static_cast<int>(start_ - lineStart_) + 1;
    tokens_.emplace_back(type, std::move(text), literal, line_, col);
}

void Lexer::errorToken(const std::string& message, int errorLine, int errorCol) {
    int col = (errorCol > 0) ? errorCol : static_cast<int>(start_ - lineStart_) + 1;
    int ln = (errorLine > 0) ? errorLine : line_;
    tokens_.emplace_back(TokenType::TK_ERROR, message, Value::nullValue(), ln, col);
}

int Lexer::currentColumn() const {
    return static_cast<int>(current_ - lineStart_) + 1;
}
