#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "interpreter/Interpreter.h"
#include <iostream>
#include <string>
#include <vector>

int main() {
    std::string source =
        "class A {\n"
        "    init() {\n"
        "        this.a = 1;\n"
        "    }\n"
        "    getA() { return this.a; }\n"
        "}\n"
        "class B : A {\n"
        "    init() {\n"
        "        super.init();\n"
        "        this.b = 2;\n"
        "    }\n"
        "    getB() { return this.b; }\n"
        "}\n"
        "class C : B {\n"
        "    init() {\n"
        "        super.init();\n"
        "        this.c = 3;\n"
        "    }\n"
        "    getC() { return this.c; }\n"
        "}\n"
        "var obj = C();\n"
        "print(obj.getA());\n"
        "print(obj.getB());\n"
        "print(obj.getC());\n";

    Lexer lexer;
    auto tokens = lexer.scan(source);
    if (lexer.getDiagnostics().hasErrors()) {
        std::cerr << "Lex error: " << lexer.getDiagnostics().all()[0].message << "\n";
        return 1;
    }

    Parser parser;
    auto ast = parser.parse(tokens);
    if (!parser.getErrors().empty()) {
        std::cerr << "Parse error: " << parser.getErrors()[0].what() << "\n";
        return 1;
    }

    Interpreter interp;
    std::vector<std::string> output;
    interp.setOutputCallback([&output](const std::string& s) {
        output.push_back(s);
    });

    try {
        interp.execute(*ast);
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
        return 1;
    }

    std::cout << "Output lines: " << output.size() << "\n";
    for (size_t i = 0; i < output.size(); i++) {
        std::cout << "  [" << i << "] = \"" << output[i] << "\"\n";
    }

    // Verify expected output: 1, 2, 3
    bool ok = (output.size() == 3 &&
               output[0] == "1" &&
               output[1] == "2" &&
               output[2] == "3");
    std::cout << (ok ? "SUPER.INIT TEST: PASS" : "SUPER.INIT TEST: FAIL") << "\n";
    return ok ? 0 : 1;
}
