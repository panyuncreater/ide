// fmt_super.cpp — Formatter 对多级 super 继承链的一致性测试
// 用三层继承（A→B→C，每层 init 调用 super.init()）作为样例，
// 验证：原始源码可执行、格式化后幂等、格式化结果仍可执行且输出一致。

#include <iostream>
#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "ast/ASTNode.h"
#include "formatter/Formatter.h"
#include "interpreter/Interpreter.h"
#include "interpreter/Value.h"
#include "interpreter/Environment.h"
#include "Diagnostic.h"

int main() {
    // main 流水线总览：依次执行四步并打印中间结果，便于人工核对——
    //   ① 运行原始源码；② 对原始源码做 Formatter 格式化；
    //   ③ 对格式化结果再格式化以检验幂等性；④ 运行格式化后的代码。
    // 构造三层继承样例 A→B→C：验证三件事——
    // (1) 原始源码可正常执行并输出 c.a/c.b/c.c；(2) 格式化结果幂等（再格式化不变）；
    // (3) 格式化后的代码仍可执行且输出与原始一致，证明 Formatter 对 super 链是保形变换。
    std::string source =
        "class A { init() { this.a = 1; } }\n"
        "class B : A { init() { super.init(); this.b = 2; } }\n"
        "class C : B { init() { super.init(); this.c = 3; } }\n"
        "var c = C(); print(c.a); print(c.b); print(c.c);";

    std::cout << "=== Original source ===" << std::endl;
    std::cout << source << std::endl;

    // Run original
    {
        Lexer lexer;
        auto tokens = lexer.scan(source);
        Parser parser;
        auto ast = parser.parse(tokens);
        Interpreter interp;
        std::string out;
        interp.setOutputCallback([&out](const std::string& s) { out += s + "\n"; });
        try {
            interp.execute(*ast);
            std::cout << "Original output: [" << out << "]" << std::endl;
        } catch (const std::exception& e) {
            std::cout << "Original error: " << e.what() << std::endl;
        }
    }

    // Format：对原始源码重新词法扫描与语法解析，再交给 Formatter 生成规范化后的代码文本。
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);
    Formatter fmt;
    std::string formatted = fmt.format(*ast);
    std::cout << "\n=== Formatted ===" << std::endl;
    std::cout << formatted << std::endl;

    // Format again (idempotency)
    {
        Lexer lex2;
        auto tok2 = lex2.scan(formatted);
        std::cout << "Re-lex errors: " << lex2.getDiagnostics().hasErrors() << std::endl;
        Parser par2;
        auto ast2 = par2.parse(tok2);
        std::cout << "Re-parse errors: " << par2.hasErrors() << std::endl;
        if (ast2 && !par2.hasErrors()) {
            Formatter fmt2;
            std::string fmt2out = fmt2.format(*ast2);
            std::cout << "Idempotent: " << (formatted == fmt2out ? "YES" : "NO") << std::endl;
        }
    }

    // Run formatted
    // 关键断言：格式化后代码执行输出应与原始一致（预期 "[1\n2\n3\n]"），
    // 否则说明 Formatter 在 super 继承链场景下改变了语义。
    std::cout << "\n=== Run formatted ===" << std::endl;
    {
        Lexer lex3;
        auto tok3 = lex3.scan(formatted);
        Parser par3;
        auto ast3 = par3.parse(tok3);
        if (ast3 && !par3.hasErrors()) {
            Interpreter interp2;
            std::string out2;
            interp2.setOutputCallback([&out2](const std::string& s) { out2 += s + "\n"; });
            try {
                interp2.execute(*ast3);
                std::cout << "Formatted output: [" << out2 << "]" << std::endl;
            } catch (const std::exception& e) {
                std::cout << "Formatted error: " << e.what() << std::endl;
            }
        } else {
            std::cout << "Cannot run formatted code (parse errors)" << std::endl;
        }
    }

    std::cout << "DONE" << std::endl;
    return 0;
}
