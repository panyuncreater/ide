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

    // Format
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
