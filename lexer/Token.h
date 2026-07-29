#pragma once

#include <string>
#include <variant>
// A1 fix: Token.h 不再 include interpreter/Value.h。
// lexer 层是比 interpreter 层更底层的模块，不应反向依赖。
// 字面量值用 std::variant 直接存储标量，由 Parser 在构造 AST 节点时转换为 Value。

// ============================================================
// Token 类型与结构体
// ============================================================

/// Token 类型枚举
enum class TokenType {
    // 关键字
    TK_VAR,
    TK_FUN,
    TK_IF,
    TK_ELSE,
    TK_WHILE,
    TK_FOR,
    TK_RETURN,
    TK_TRUE,
    TK_FALSE,
    TK_AND,
    TK_OR,
    TK_NOT,
    TK_PRINT,
    TK_BREAK,
    TK_CONTINUE,
    TK_TRY,
    TK_CATCH,
    TK_THROW,
    TK_FINALLY, // BUG-AUDIT-FINALLY-1: finally 块语法
    TK_IMPORT,
    TK_FROM,
    TK_EXPORT,
    TK_AS, // P2-11: import * as ns 中的 as 关键字
    // 类型关键字
    TK_INT,
    TK_FLOAT,
    TK_BOOL,
    TK_STRING_TYPE,
    // 新增关键字
    TK_FUNCTION, // [已废弃] Lexer 已统一为 TK_FUN，保留枚举值避免编号变化
    TK_FUNC,     // [已废弃] Lexer 已统一为 TK_FUN，保留枚举值避免编号变化
    TK_CLASS,    // class 关键字
    TK_EXTENDS,  // extends 关键字
    TK_SUPER,    // super 关键字
    TK_DICT,     // dict 关键字
    TK_ARRAY,    // array 关键字
    TK_NULL,     // null 关键字
    // R99 枚举与 ADT + match：新增关键字
    TK_ENUM,     // enum 关键字
    TK_MATCH,    // match 关键字
    TK_CASE,     // case 关键字
    TK_DEFAULT,  // default 关键字
    // R164 协程/生成器：新增关键字
    TK_YIELD,    // yield 关键字（生成器挂起）
    // L18 lang-constfun：const fun 编译期求值
    TK_CONST, // const 关键字（修饰 fun 声明）
    // 字面量
    TK_IDENTIFIER,
    TK_INT_LIT,
    TK_FLOAT_LIT,
    TK_STRING_LIT,
    // 运算符
    TK_PLUS,
    TK_MINUS,
    TK_STAR,
    TK_SLASH,
    TK_PERCENT,
    TK_EQ,
    TK_NEQ,
    TK_LT,
    TK_GT,
    TK_LEQ,
    TK_GEQ,
    // 赋值
    TK_ASSIGN,
    // 分隔符
    TK_LPAREN,
    TK_RPAREN,
    TK_LBRACE,
    TK_RBRACE,
    TK_SEMICOLON,
    TK_COMMA,
    // 新增分隔符
    TK_LBRACKET, // [
    TK_RBRACKET, // ]
    TK_COLON,    // :
    TK_DOT,      // .
    TK_ARROW,    // => （R99 match case 分隔符）
    // 特殊
    TK_EOF,
    TK_ERROR,
    TK_LINE_COMMENT,
    TK_BLOCK_COMMENT,
    // F7: 字符串插值 "Hello {name}" 的分隔标记
    TK_INTERP_START, // {
    TK_INTERP_END,   // }
    TK_STRING_PART   // 插值字符串的文本片段（非终结字符串字面量）
};

/// A1 fix: Token 字面量值类型。
/// 用 variant 替代 Value，使 lexer 层不依赖 interpreter 层。
/// 仅存储字面量所需的标量类型。
using TokenLiteral = std::variant<std::monostate, int64_t, double, std::string, bool>;

/// Token 结构体
struct Token {
    TokenType type;
    std::string lexeme;
    TokenLiteral literal; // A1 fix: 字面量值（variant 替代 Value）
    int line;             // 行号（从 1 开始）
    int column;           // 列号（从 1 开始）

    Token() : type(TokenType::TK_EOF), line(0), column(0) {}

    Token(TokenType t, const std::string& lex, TokenLiteral lit, int ln, int col)
        : type(t), lexeme(lex), literal(std::move(lit)), line(ln), column(col) {}

    // A1 fix: 字面量类型查询（替代原 Value::isXxx()）
    bool literalIsInt() const { return std::holds_alternative<int64_t>(literal); }
    bool literalIsFloat() const { return std::holds_alternative<double>(literal); }
    bool literalIsString() const { return std::holds_alternative<std::string>(literal); }
    bool literalIsBool() const { return std::holds_alternative<bool>(literal); }
    bool literalIsNull() const { return std::holds_alternative<std::monostate>(literal); }

    // A1 fix: 字面量值访问（替代原 Value::xxxVal()）
    int64_t literalInt() const { return std::get<int64_t>(literal); }
    double literalFloat() const { return std::get<double>(literal); }
    const std::string& literalString() const { return std::get<std::string>(literal); }
    bool literalBool() const { return std::get<bool>(literal); }

    /// 获取 Token 类型的字符串表示
    static std::string typeToString(TokenType t) {
        switch (t) {
        case TokenType::TK_VAR:
            return "VAR";
        case TokenType::TK_FUN:
            return "FUN";
        case TokenType::TK_IF:
            return "IF";
        case TokenType::TK_ELSE:
            return "ELSE";
        case TokenType::TK_WHILE:
            return "WHILE";
        case TokenType::TK_FOR:
            return "FOR";
        case TokenType::TK_RETURN:
            return "RETURN";
        case TokenType::TK_TRUE:
            return "TRUE";
        case TokenType::TK_FALSE:
            return "FALSE";
        case TokenType::TK_AND:
            return "AND";
        case TokenType::TK_OR:
            return "OR";
        case TokenType::TK_NOT:
            return "NOT";
        case TokenType::TK_PRINT:
            return "PRINT";
        case TokenType::TK_BREAK:
            return "BREAK";
        case TokenType::TK_CONTINUE:
            return "CONTINUE";
        case TokenType::TK_TRY:
            return "TRY";
        case TokenType::TK_CATCH:
            return "CATCH";
        case TokenType::TK_THROW:
            return "THROW";
        case TokenType::TK_FINALLY:
            return "FINALLY";
        case TokenType::TK_IMPORT:
            return "IMPORT";
        case TokenType::TK_FROM:
            return "FROM";
        case TokenType::TK_EXPORT:
            return "EXPORT";
        case TokenType::TK_AS: // P2-11
            return "AS";
        case TokenType::TK_INT:
            return "INT_TYPE";
        case TokenType::TK_FLOAT:
            return "FLOAT_TYPE";
        case TokenType::TK_BOOL:
            return "BOOL_TYPE";
        case TokenType::TK_STRING_TYPE:
            return "STRING_TYPE";
        case TokenType::TK_FUNCTION:
            return "FUNCTION";
        case TokenType::TK_FUNC:
            return "FUNC";
        case TokenType::TK_CLASS:
            return "CLASS";
        case TokenType::TK_EXTENDS:
            return "EXTENDS";
        case TokenType::TK_SUPER:
            return "SUPER";
        case TokenType::TK_DICT:
            return "DICT";
        case TokenType::TK_ARRAY:
            return "ARRAY";
        case TokenType::TK_NULL:
            return "NULL";
        case TokenType::TK_ENUM:
            return "ENUM";
        case TokenType::TK_MATCH:
            return "MATCH";
        case TokenType::TK_CASE:
            return "CASE";
        case TokenType::TK_DEFAULT:
            return "DEFAULT";
        case TokenType::TK_YIELD:
            return "YIELD";
        case TokenType::TK_CONST:
            return "CONST";
        case TokenType::TK_IDENTIFIER:
            return "IDENTIFIER";
        case TokenType::TK_INT_LIT:
            return "INT_LIT";
        case TokenType::TK_FLOAT_LIT:
            return "FLOAT_LIT";
        case TokenType::TK_STRING_LIT:
            return "STRING_LIT";
        case TokenType::TK_PLUS:
            return "PLUS";
        case TokenType::TK_MINUS:
            return "MINUS";
        case TokenType::TK_STAR:
            return "STAR";
        case TokenType::TK_SLASH:
            return "SLASH";
        case TokenType::TK_PERCENT:
            return "PERCENT";
        case TokenType::TK_EQ:
            return "EQ";
        case TokenType::TK_NEQ:
            return "NEQ";
        case TokenType::TK_LT:
            return "LT";
        case TokenType::TK_GT:
            return "GT";
        case TokenType::TK_LEQ:
            return "LEQ";
        case TokenType::TK_GEQ:
            return "GEQ";
        case TokenType::TK_ASSIGN:
            return "ASSIGN";
        case TokenType::TK_LPAREN:
            return "LPAREN";
        case TokenType::TK_RPAREN:
            return "RPAREN";
        case TokenType::TK_LBRACE:
            return "LBRACE";
        case TokenType::TK_RBRACE:
            return "RBRACE";
        case TokenType::TK_SEMICOLON:
            return "SEMICOLON";
        case TokenType::TK_COMMA:
            return "COMMA";
        case TokenType::TK_LBRACKET:
            return "LBRACKET";
        case TokenType::TK_RBRACKET:
            return "RBRACKET";
        case TokenType::TK_COLON:
            return "COLON";
        case TokenType::TK_DOT:
            return "DOT";
        case TokenType::TK_ARROW:
            return "ARROW";
        case TokenType::TK_EOF:
            return "EOF";
        case TokenType::TK_ERROR:
            return "ERROR";
        case TokenType::TK_LINE_COMMENT:
            return "COMMENT";
        case TokenType::TK_BLOCK_COMMENT:
            return "BLOCK_COMMENT";
        case TokenType::TK_INTERP_START:
            return "INTERP_START";
        case TokenType::TK_INTERP_END:
            return "INTERP_END";
        case TokenType::TK_STRING_PART:
            return "STRING_PART";
        }
        return "UNKNOWN";
    }

    /// A1 fix: 字面量的字符串表示（替代原 Value::toString()）
    /// 仅供调试输出使用，不引入 Value 依赖
    std::string literalToString() const {
        if (literalIsNull())
            return "null";
        if (literalIsInt())
            return std::to_string(std::get<int64_t>(literal));
        if (literalIsFloat())
            return std::to_string(std::get<double>(literal));
        if (literalIsBool())
            return std::get<bool>(literal) ? "true" : "false";
        if (literalIsString())
            return "\"" + std::get<std::string>(literal) + "\"";
        return "";
    }

    /// 调试输出
    std::string toString() const {
        return typeToString(type) + " '" + lexeme + "' " + literalToString() + " @L" + std::to_string(line) + ":C" +
               std::to_string(column);
    }
};
