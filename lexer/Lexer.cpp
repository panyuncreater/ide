#include "lexer/Lexer.h"
#include <cctype>

// ============================================================
// Lexer 词法分析器实现
// ============================================================

Lexer::Lexer() {
    initKeywords();
}

void Lexer::initKeywords() {
    keywords_["var"]      = TokenType::TK_VAR;
    keywords_["fun"]      = TokenType::TK_FUN;
    keywords_["if"]       = TokenType::TK_IF;
    keywords_["else"]     = TokenType::TK_ELSE;
    keywords_["while"]   = TokenType::TK_WHILE;
    keywords_["for"]     = TokenType::TK_FOR;
    keywords_["return"]  = TokenType::TK_RETURN;
    keywords_["true"]    = TokenType::TK_TRUE;
    keywords_["false"]   = TokenType::TK_FALSE;
    keywords_["and"]     = TokenType::TK_AND;
    keywords_["or"]      = TokenType::TK_OR;
    keywords_["not"]     = TokenType::TK_NOT;
    keywords_["print"]   = TokenType::TK_PRINT;
    keywords_["int"]     = TokenType::TK_INT;
    keywords_["float"]   = TokenType::TK_FLOAT;
    keywords_["bool"]    = TokenType::TK_BOOL;
    keywords_["string"]  = TokenType::TK_STRING_TYPE;
    // 新增关键字
    keywords_["function"] = TokenType::TK_FUNCTION;
    keywords_["class"]   = TokenType::TK_CLASS;
    keywords_["extends"] = TokenType::TK_EXTENDS;
    keywords_["dict"]    = TokenType::TK_DICT;
    keywords_["null"]    = TokenType::TK_NULL;
}

std::vector<Token> Lexer::scan(const std::string& source) {
    source_ = source;
    start_ = 0;
    current_ = 0;
    line_ = 1;
    column_ = 1;
    lineStart_ = 0;
    tokens_.clear();

    while (!isAtEnd()) {
        start_ = current_;
        scanToken();
    }

    // 添加 EOF Token
    tokens_.emplace_back(TokenType::TK_EOF, "", Value::nullValue(), line_, currentColumn());
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
    char c = source_[current_];
    current_++;
    if (c == '\n') {
        line_++;
        lineStart_ = current_;
    }
    return c;
}

bool Lexer::isAtEnd() const {
    return current_ >= static_cast<int>(source_.size());
}

bool Lexer::match(char expected) {
    if (isAtEnd()) return false;
    if (source_[current_] != expected) return false;
    current_++;
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
    case '.': addToken(TokenType::TK_DOT); break;

    // 运算符
    case '+': addToken(TokenType::TK_PLUS); break;
    case '-': addToken(TokenType::TK_MINUS); break;
    case '*': addToken(TokenType::TK_STAR); break;
    case '/':
        // 单行注释
        if (match('/')) {
            while (!isAtEnd() && peek() != '\n') advance();
        } else {
            addToken(TokenType::TK_SLASH);
        }
        break;
    case '%': addToken(TokenType::TK_PERCENT); break;

    // 可能是双字符运算符
    case '=':
        addToken(match('=') ? TokenType::TK_EQ : TokenType::TK_ASSIGN);
        break;
    case '!':
        addToken(match('=') ? TokenType::TK_NEQ : TokenType::TK_NOT);
        break;
    case '<':
        addToken(match('=') ? TokenType::TK_LEQ : TokenType::TK_LT);
        break;
    case '>':
        addToken(match('=') ? TokenType::TK_GEQ : TokenType::TK_GT);
        break;

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
    std::string text = source_.substr(start_, current_ - start_);

    // 查关键字表
    auto it = keywords_.find(text);
    if (it != keywords_.end()) {
        TokenType type = it->second;
        // true 和 false 有字面量值
        if (type == TokenType::TK_TRUE) {
            addToken(type, Value(true));
        } else if (type == TokenType::TK_FALSE) {
            addToken(type, Value(false));
        } else if (type == TokenType::TK_NULL) {
            // null 关键字有字面量值
            addToken(type, Value::nullValue());
        } else {
            addToken(type);
        }
    } else {
        addToken(TokenType::TK_IDENTIFIER);
    }
}

void Lexer::number() {
    while (!isAtEnd() && std::isdigit(static_cast<unsigned char>(peek()))) {
        advance();
    }

    // 浮点数：小数部分
    bool isFloat = false;
    if (!isAtEnd() && peek() == '.' &&
        std::isdigit(static_cast<unsigned char>(peekNext()))) {
        isFloat = true;
        advance(); // 消耗 '.'
        while (!isAtEnd() && std::isdigit(static_cast<unsigned char>(peek()))) {
            advance();
        }
    }

    std::string text = source_.substr(start_, current_ - start_);
    int col = static_cast<int>(start_ - lineStart_) + 1;

    if (isFloat) {
        double val = std::stod(text);
        addToken(TokenType::TK_FLOAT_LIT, Value(val));
    } else {
        int val = std::stoi(text);
        addToken(TokenType::TK_INT_LIT, Value(val));
    }
}

void Lexer::string() {
    std::string value;

    while (!isAtEnd() && peek() != '"') {
        if (peek() == '\n') {
            // 字符串内换行也算行号
            line_++;
            lineStart_ = current_ + 1;
        }

        if (peek() == '\\') {
            advance(); // 消耗反斜杠
            if (isAtEnd()) {
                errorToken("未终止的字符串");
                return;
            }
            char esc = advance();
            switch (esc) {
            case 'n':  value += '\n'; break;
            case 't':  value += '\t'; break;
            case '\\': value += '\\'; break;
            case '"':  value += '"';  break;
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
        errorToken("未终止的字符串");
        return;
    }

    advance(); // 消耗闭合的 '"'
    addToken(TokenType::TK_STRING_LIT, Value(value));
}

void Lexer::addToken(TokenType type) {
    std::string text = source_.substr(start_, current_ - start_);
    int col = static_cast<int>(start_ - lineStart_) + 1;
    tokens_.emplace_back(type, text, Value::nullValue(), line_, col);
}

void Lexer::addToken(TokenType type, const Value& literal) {
    std::string text = source_.substr(start_, current_ - start_);
    int col = static_cast<int>(start_ - lineStart_) + 1;
    tokens_.emplace_back(type, text, literal, line_, col);
}

void Lexer::errorToken(const std::string& message) {
    int col = currentColumn();
    tokens_.emplace_back(TokenType::TK_ERROR, message, Value::nullValue(), line_, col);
}

int Lexer::currentColumn() const {
    return static_cast<int>(current_ - lineStart_) + 1;
}
