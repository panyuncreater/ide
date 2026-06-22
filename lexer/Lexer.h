#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include "lexer/Token.h"
#include "Diagnostic.h"

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

    /// 获取扫描过程中收集的诊断信息
    const DiagnosticBag& getDiagnostics() const { return diagnostics_; }

    /// 获取注释 Token 列表（从主 Token 流中分离，供 Formatter 保留注释用）
    const std::vector<Token>& comments() const { return comments_; }

private:
    std::string_view source_;        // P9 fix: string_view 避免全量拷贝（调用方保证生命周期）
    int start_ = 0;                 // 当前 token 起始位置
    int current_ = 0;               // 当前读取位置
    int line_ = 1;                  // 当前行号
    int lineStart_ = 0;             // 当前行起始偏移

    std::vector<Token> tokens_;     // 输出的 Token 列表
    std::vector<Token> comments_;   // 注释 Token 列表（从主流中分离，供 Formatter 使用）
    DiagnosticBag diagnostics_;      // 诊断收集器

    /// 关键字映射表（全局共享，只初始化一次）
    static const std::unordered_map<std::string, TokenType>& keywords();

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
    void addToken(TokenType type, std::string&& text);
    void addToken(TokenType type, std::string&& text, const Value& literal);

    /// 报告词法错误（生成 TK_ERROR Token，继续扫描）
    void errorToken(const std::string& message, int errorLine = -1, int errorCol = -1);

    /// 获取当前列号
    int currentColumn() const;
};
