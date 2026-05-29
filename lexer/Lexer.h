#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include "lexer/Token.h"

// ============================================================
// Lexer 词法分析器
// ============================================================

/// 手写扫描器，将源代码文本转换为 Token 流
class Lexer {
public:
    /// 构造函数
    Lexer();

    /// 扫描源代码，返回 Token 列表
    std::vector<Token> scan(const std::string& source);

private:
    std::string source_;            // 源代码文本
    int start_ = 0;                 // 当前 token 起始位置
    int current_ = 0;               // 当前读取位置
    int line_ = 1;                  // 当前行号
    int column_ = 1;                // 当前列号（行首为 1）
    int lineStart_ = 0;             // 当前行起始偏移

    std::vector<Token> tokens_;     // 输出的 Token 列表

    /// 关键字映射表
    std::unordered_map<std::string, TokenType> keywords_;

    /// 初始化关键字表
    void initKeywords();

    /// 获取当前字符（不前进）
    char peek() const;

    /// 看下一个字符（不前进）
    char peekNext() const;

    /// 前进一个字符，返回当前字符
    char advance();

    /// 是否到达源代码末尾
    bool isAtEnd() const;

    /// 匹配当前字符，若匹配则前进
    bool match(char expected);

    /// 扫描一个 Token
    void scanToken();

    /// 扫描标识符或关键字
    void identifier();

    /// 扫描数字字面量
    void number();

    /// 扫描字符串字面量
    void string();

    /// 添加 Token
    void addToken(TokenType type);
    void addToken(TokenType type, const Value& literal);

    /// 报告词法错误（生成 TK_ERROR Token，继续扫描）
    void errorToken(const std::string& message);

    /// 获取当前列号
    int currentColumn() const;
};
