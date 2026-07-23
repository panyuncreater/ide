#pragma once

#include "Diagnostic.h"
#include "common/RuntimeLimits.h"
#include "lexer/Token.h"
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

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
    std::string_view source_; // P9 fix: string_view 避免全量拷贝（调用方保证生命周期）
    int start_ = 0;           // 当前 token 起始位置
    int current_ = 0;         // 当前读取位置
    int line_ = 1;            // 当前行号
    int lineStart_ = 0;       // 当前行起始偏移

    // Perf-Finding1: columnAt() 单调前进缓存。advance() 仅向前推进 current_/lineStart_，
    // 行内 columnAt 调用 byteOffset 单调非递减。缓存 (lineStart, byteOffset, col)，
    // 下次调用若 lineStart_ 未变且 byteOffset >= cachedByteOffset_ 则从缓存点续走，
    // 将逐 token 列号计算从 O(L²) 降为 O(L)。mutable 因 columnAt 是 const。
    mutable int cachedLineStart_ = -1;
    mutable int cachedByteOffset_ = -1;
    mutable int cachedColumn_ = 1;

    std::vector<Token> tokens_;   // 输出的 Token 列表
    std::vector<Token> comments_; // 注释 Token 列表（从主流中分离，供 Formatter 使用）
    DiagnosticBag diagnostics_;   // 诊断收集器

    // DoS 防护：源码大小上限和 Token 数量上限（统一引用 RuntimeLimits）
    static constexpr size_t MAX_SOURCE_SIZE = RuntimeLimits::MAX_SOURCE_SIZE;
    static constexpr size_t MAX_TOKEN_COUNT = RuntimeLimits::MAX_TOKEN_COUNT;
    static constexpr int MAX_INTERP_DEPTH = RuntimeLimits::MAX_INTERP_DEPTH;
    int interpDepth_ = 0; // L-P1-1: 当前插值嵌套深度

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

    // ---- R132-B fix: scanToken 229 行拆为 thin dispatcher + 5 个返回 bool 的字符类 helper
    // + 1 个默认分支 helper（每个 < 60 行）----
    /// 处理空白字符（' '、'\t'、'\n'、'\f'、'\v'）：直接丢弃。
    /// 返回 true=已处理；false=非空白字符（交由后续 helper）。
    bool scanWhitespaceToken(char c);
    /// 处理分隔符与 '.'：'(' ')' '{' '}' ';' ',' '[' ']' ':' '?' '.'。
    /// '.' 后接数字时降级到 number()。返回 true=已处理；false=非分隔符。
    bool scanDelimiterToken(char c);
    /// 处理 '/' 及其衍生（单行注释 //、块注释 /*...*/、TK_SLASH 除法）。
    /// 返回 true=已处理；false=非 '/' 字符。
    bool scanSlashToken(char c);
    /// 处理简单算术运算符 '+' '-' '*' '%'（无第二字符判断）。
    /// 返回 true=已处理；false=非此类运算符。
    bool scanArithOperatorToken(char c);
    /// 处理双字符运算符 '=' '!' '<' '>' '&' '|'（含 => 箭头特例）。
    /// 使用 O(1) 查找表分派。返回 true=已处理；false=非此类运算符。
    bool scanTwoCharOperatorToken(char c);
    /// 处理默认分支：'"'（字符串字面量）+ 数字 + 标识符/关键字 + 未知字符（UTF-8 感知）。
    /// 总会消费当前字符并产生 Token 或错误，无 fallthrough。
    void scanLiteralOrUnknownToken(char c);

    /// 扫描标识符或关键字
    void identifier();

    /// 扫描数字字面量
    void number();

    /// 扫描字符串字面量
    void string();

    /// F7: 扫描字符串字面量（支持插值）
    /// isInterp=true 表示当前处于插值字符串的后续片段（由 } 触发）
    void string(bool isInterp);

    // ---- R131 fix: string() 448 行拆为 thin entry + 4 个 helper（每个 < 170 行）----
    /// 处理字符串中的 '{' 插值起始：发射 TK_STRING_PART/TK_INTERP_START，
    /// 嵌套扫描表达式直到匹配 '}'（支持嵌套大括号/嵌套字符串），
    /// 发射 TK_INTERP_END。返回 true=继续外层循环，false=终止 string()。
    /// interpDepth_ 在此处自增/自减。
    bool handleInterpolation(int& startLine, int& startCol, std::string& value, bool& isInterp);
    /// 处理字符串中的 '\\' 转义序列：switch 分发到简单转义（\n/\t/\r/\\/\"/\'/\0/\b/\f/\a/\v）
    /// 或调用 handleHexEscape / handleUnicodeEscape。返回 true=继续外层循环，false=终止 string()。
    /// 错误恢复：未知转义 / 十六进制错误 / Unicode 错误时跳过到字符串结束或 EOF 后 return false。
    bool handleEscape(int startLine, int startCol, std::string& value);
    /// 处理 \xNN 十六进制字节转义：读取 2 位十六进制 → 编码为单字节。
    /// 错误恢复：非法十六进制字符时跳过到字符串结束或 EOF 后 return false。
    bool handleHexEscape(int startLine, int startCol, std::string& value);
    /// 处理 \u Unicode 转义：若 peek()=='{' 走 \u{XXXXXX} 扩展语法（1-6 位十六进制），
    /// 否则走 \uXXXX 标准 4 位语法。验证码点范围 [0, 0x10FFFF] 且非代理码点 [0xD800, 0xDFFF]，
    /// 编码为 UTF-8（1-4 字节）。错误恢复时跳过到字符串结束或 EOF 后 return false。
    bool handleUnicodeEscape(int startLine, int startCol, std::string& value);

    /// 添加 Token
    void addToken(TokenType type);
    // A1 fix: literal 参数改用 TokenLiteral（variant），不再依赖 Value
    void addToken(TokenType type, TokenLiteral literal);
    void addToken(TokenType type, std::string&& text);
    void addToken(TokenType type, std::string&& text, TokenLiteral literal);

    /// 报告词法错误（生成 TK_ERROR Token，继续扫描）
    void errorToken(const std::string& message, int errorLine = -1, int errorCol = -1);

    /// 获取当前列号
    int currentColumn() const;

    /// D11 fix: 根据字节偏移计算 UTF-8 码位列号（1-based）。
    /// 从 lineStart_ 到 byteOffset 之间的字节数按 UTF-8 首字节解码为码位数，
    /// 避免多字节字符（如中文）导致列号偏移过大。
    int columnAt(int byteOffset) const;
};
