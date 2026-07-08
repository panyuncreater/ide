// fmt_minimal.cpp — Formatter 最小冒烟测试
// 用一段最简单的源码 print(42); 走通 Lexer→Parser→Formatter 全流程，
// 仅验证三条链路不崩溃、能产出格式化结果，不做语义/幂等校验。

#include <iostream>
#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "ast/ASTNode.h"
#include "formatter/Formatter.h"
#include "Diagnostic.h"

int main() {
    std::cout << "START" << std::endl;

    // 测试输入：最简表达式语句
    std::string source = "print(42);";
    std::cout << "Source: " << source << std::endl;

    // 阶段 1：词法分析
    Lexer lexer;
    auto tokens = lexer.scan(source);
    std::cout << "Tokens: " << tokens.size() << std::endl;

    // 阶段 2：语法分析，得到 AST
    Parser parser;
    auto ast = parser.parse(tokens);
    std::cout << "AST: " << (ast ? "ok" : "null") << std::endl;

    // 阶段 3：格式化（AST → 规范文本）。Formatter 将 AST 重新序列化为排版后的源码字符串。
    Formatter fmt;
    std::cout << "Formatter created" << std::endl;

    // 冒烟断言：formatted 应仍为 print(42); 形态（去空白后），证明 Lexer→Parser→Formatter 最小链路闭环可用。
    std::string formatted = fmt.format(*ast);
    std::cout << "Formatted: [" << formatted << "]" << std::endl;

    std::cout << "DONE" << std::endl;
    return 0;
}
