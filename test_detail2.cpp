#include <iostream>
#include <sstream>
#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "interpreter/Interpreter.h"

std::string run(const std::string& src) {
    std::ostringstream out;
    Interpreter interp;
    interp.setOutputCallback([&](const std::string& t) { out << t << "\n"; });
    try {
        Lexer l; auto t = l.scan(src);
        Parser p; auto a = p.parse(t);
        if (a) interp.execute(*a);
    } catch (const RuntimeError& e) {
        return "RUNTIME_ERROR(L" + std::to_string(e.line) + "): " + e.what();
    } catch (const std::exception& e) {
        return std::string("EXCEPTION: ") + e.what();
    }
    return out.str();
}

int main() {
    // Test: typed params
    std::cout << "T3.4 typed params:\n  " << run("func add(int a, int b) -> int { return a + b; }\nprint(add(3, 4));") << "\n\n";

    // Test: dict() call
    std::cout << "T4.4 dict() call:\n  " << run("var d = dict();\nd.name = \"test\";\nprint(d.name);") << "\n\n";

    // Test: return type check
    std::cout << "T7.3 return type:\n  " << run("func bad() -> int { return \"hello\"; }\nbad();") << "\n\n";

    // Test: func with typed params (simple)
    std::cout << "T3.4b simpler:\n  " << run("func add(a, b) { return a + b; }\nprint(add(3,4));") << "\n";
}
