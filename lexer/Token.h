#pragma once

#include <string>
#include "interpreter/Value.h"

// ============================================================
// Token 类型与结构体
// ============================================================

/// Token 类型枚举
enum class TokenType {
    // 关键字
    TK_VAR, TK_FUN, TK_IF, TK_ELSE, TK_WHILE, TK_FOR, TK_RETURN,
    TK_TRUE, TK_FALSE, TK_AND, TK_OR, TK_NOT, TK_PRINT,
    // 类型关键字
    TK_INT, TK_FLOAT, TK_BOOL, TK_STRING_TYPE,
    // 新增关键字
    TK_FUNCTION,    // [已废弃] Lexer 已统一为 TK_FUN，保留枚举值避免编号变化
    TK_FUNC,        // [已废弃] Lexer 已统一为 TK_FUN，保留枚举值避免编号变化
    TK_CLASS,       // class 关键字
    TK_EXTENDS,     // extends 关键字
    TK_SUPER,       // super 关键字
    TK_DICT,        // dict 关键字
    TK_ARRAY,       // array 关键字
    TK_NULL,        // null 关键字
    // 字面量
    TK_IDENTIFIER, TK_INT_LIT, TK_FLOAT_LIT, TK_STRING_LIT,
    // 运算符
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
    TK_EQ, TK_NEQ, TK_LT, TK_GT, TK_LEQ, TK_GEQ,
    // 赋值
    TK_ASSIGN,
    // 分隔符
    TK_LPAREN, TK_RPAREN, TK_LBRACE, TK_RBRACE,
    TK_SEMICOLON, TK_COMMA,
    // 新增分隔符
    TK_LBRACKET,    // [
    TK_RBRACKET,    // ]
    TK_COLON,       // :
    TK_DOT,         // .
    // 特殊
    TK_EOF, TK_ERROR, TK_LINE_COMMENT
};

/// Token 结构体
struct Token {
    TokenType type;
    std::string lexeme;
    Value literal;      // 字面量值（仅 TK_INT_LIT, TK_FLOAT_LIT, TK_STRING_LIT, TK_TRUE, TK_FALSE）
    int line;           // 行号（从 1 开始）
    int column;         // 列号（从 1 开始）

    Token()
        : type(TokenType::TK_EOF), line(0), column(0) {}

    Token(TokenType t, const std::string& lex, const Value& lit, int ln, int col)
        : type(t), lexeme(lex), literal(lit), line(ln), column(col) {}

    /// 获取 Token 类型的字符串表示
    static std::string typeToString(TokenType t) {
        switch (t) {
        case TokenType::TK_VAR:         return "VAR";
        case TokenType::TK_FUN:         return "FUN";
        case TokenType::TK_IF:          return "IF";
        case TokenType::TK_ELSE:        return "ELSE";
        case TokenType::TK_WHILE:       return "WHILE";
        case TokenType::TK_FOR:         return "FOR";
        case TokenType::TK_RETURN:      return "RETURN";
        case TokenType::TK_TRUE:        return "TRUE";
        case TokenType::TK_FALSE:       return "FALSE";
        case TokenType::TK_AND:         return "AND";
        case TokenType::TK_OR:          return "OR";
        case TokenType::TK_NOT:         return "NOT";
        case TokenType::TK_PRINT:       return "PRINT";
        case TokenType::TK_INT:         return "INT_TYPE";
        case TokenType::TK_FLOAT:       return "FLOAT_TYPE";
        case TokenType::TK_BOOL:        return "BOOL_TYPE";
        case TokenType::TK_STRING_TYPE: return "STRING_TYPE";
        case TokenType::TK_FUNCTION:    return "FUNCTION";
        case TokenType::TK_FUNC:        return "FUNC";
        case TokenType::TK_CLASS:       return "CLASS";
        case TokenType::TK_EXTENDS:     return "EXTENDS";
        case TokenType::TK_SUPER:       return "SUPER";
        case TokenType::TK_DICT:        return "DICT";
        case TokenType::TK_ARRAY:       return "ARRAY";
        case TokenType::TK_NULL:        return "NULL";
        case TokenType::TK_IDENTIFIER:  return "IDENTIFIER";
        case TokenType::TK_INT_LIT:     return "INT_LIT";
        case TokenType::TK_FLOAT_LIT:   return "FLOAT_LIT";
        case TokenType::TK_STRING_LIT:  return "STRING_LIT";
        case TokenType::TK_PLUS:        return "PLUS";
        case TokenType::TK_MINUS:       return "MINUS";
        case TokenType::TK_STAR:        return "STAR";
        case TokenType::TK_SLASH:       return "SLASH";
        case TokenType::TK_PERCENT:     return "PERCENT";
        case TokenType::TK_EQ:          return "EQ";
        case TokenType::TK_NEQ:         return "NEQ";
        case TokenType::TK_LT:          return "LT";
        case TokenType::TK_GT:          return "GT";
        case TokenType::TK_LEQ:         return "LEQ";
        case TokenType::TK_GEQ:         return "GEQ";
        case TokenType::TK_ASSIGN:      return "ASSIGN";
        case TokenType::TK_LPAREN:      return "LPAREN";
        case TokenType::TK_RPAREN:      return "RPAREN";
        case TokenType::TK_LBRACE:      return "LBRACE";
        case TokenType::TK_RBRACE:      return "RBRACE";
        case TokenType::TK_SEMICOLON:   return "SEMICOLON";
        case TokenType::TK_COMMA:       return "COMMA";
        case TokenType::TK_LBRACKET:    return "LBRACKET";
        case TokenType::TK_RBRACKET:    return "RBRACKET";
        case TokenType::TK_COLON:       return "COLON";
        case TokenType::TK_DOT:         return "DOT";
        case TokenType::TK_EOF:         return "EOF";
        case TokenType::TK_ERROR:       return "ERROR";
        case TokenType::TK_LINE_COMMENT: return "COMMENT";
        }
        return "UNKNOWN";
    }

    /// 调试输出
    std::string toString() const {
        return typeToString(type) + " '" + lexeme + "' " + literal.toString()
               + " @L" + std::to_string(line) + ":C" + std::to_string(column);
    }
};
