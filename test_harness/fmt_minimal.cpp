#include <iostream>
#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "ast/ASTNode.h"
#include "formatter/Formatter.h"
#include "Diagnostic.h"

int main() {
    std::cout << "START" << std::endl;

    std::string source = "print(42);";
    std::cout << "Source: " << source << std::endl;

    Lexer lexer;
    auto tokens = lexer.scan(source);
    std::cout << "Tokens: " << tokens.size() << std::endl;

    Parser parser;
    auto ast = parser.parse(tokens);
    std::cout << "AST: " << (ast ? "ok" : "null") << std::endl;

    Formatter fmt;
    std::cout << "Formatter created" << std::endl;

    std::string formatted = fmt.format(*ast);
    std::cout << "Formatted: [" << formatted << "]" << std::endl;

    std::cout << "DONE" << std::endl;
    return 0;
}
