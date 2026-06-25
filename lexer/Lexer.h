#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include "lexer/Token.h"
#include "Diagnostic.h"
#include "common/RuntimeLimits.h"

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

    /// F13: 获取关键字映射表（供自动补全使用）
    static const std::unordered_map<std::string, TokenType>& keywords();

private:
    std::string_view source_;        // P9 fix: string_view 避免全量拷贝（调用方保证生命周期）
    int start_ = 0;                 // 当前 token 起始位置
    int current_ = 0;               // 当前读取位置
    int line_ = 1;                  // 当前行号
    int lineStart_ = 0;             // 当前行起始偏移

    std::vector<Token> tokens_;     // 输出的 Token 列表
    std::vector<Token> comments_;   // 注释 Token 列表（从主流中分离，供 Formatter 使用）
    DiagnosticBag diagnostics_;      // 诊断收集器

    // DoS 防护：源码大小上限和 Token 数量上限（统一引用 RuntimeLimits）
    static constexpr size_t MAX_SOURCE_SIZE = RuntimeLimits::MAX_SOURCE_SIZE;
    static constexpr size_t MAX_TOKEN_COUNT = RuntimeLimits::MAX_TOKEN_COUNT;
    static constexpr int MAX_INTERP_DEPTH = RuntimeLimits::MAX_INTERP_DEPTH;
    int interpDepth_ = 0;                                         // L-P1-1: 当前插值嵌套深度

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

    /// F7: 扫描字符串字面量（支持插值）
    /// isInterp=true 表示当前处于插值字符串的后续片段（由 } 触发）
    void string(bool isInterp);

    /// 添加 Token
    void addToken(TokenType type);
    void addToken(TokenType type, const Value& literal);
    void addToken(TokenType type, std::string&& text);
    void addToken(TokenType type, std::string&& text, const Value& literal);

    /// 报告词法错误（生成 TK_ERROR Token，继续扫描）
    void errorToken(const std::string& message, int errorLine = -1, int errorCol = -1);

    /// 获取当前列号
    int currentColumn() const;

    /// D11 fix: 根据字节偏移计算 UTF-8 码位列号（1-based）。
    /// 从 lineStart_ 到 byteOffset 之间的字节数按 UTF-8 首字节解码为码位数，
    /// 避免多字节字符（如中文）导致列号偏移过大。
    int columnAt(int byteOffset) const;
};
