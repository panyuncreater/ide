#include <iostream>
#include <string>
#include <sstream>
#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "interpreter/Interpreter.h"

int main() {
    std::ostringstream output;
    Interpreter interpreter;
    interpreter.setOutputCallback([&output](const std::string& text) {
        output << text << "\n";
    });

    // Simple closure test
    std::string source = R"(
func makeCounter() {
    var count = 0;
    func inc() {
        count = count + 1;
        return count;
    }
    return inc;
}
var c = makeCounter();
print(c);
)";

    try {
        Lexer lexer;
        auto tokens = lexer.scan(source);
        Parser parser;
        auto ast = parser.parse(tokens);
        interpreter.execute(*ast);
    } catch (const RuntimeError& e) {
        std::cout << "RUNTIME_ERROR(L" << e.line << "): " << e.what() << "\n";
    } catch (const std::exception& e) {
        std::cout << "EXCEPTION: " << e.what() << "\n";
    }

    std::cout << "Output: " << output.str() << "\n";
    return 0;
}
