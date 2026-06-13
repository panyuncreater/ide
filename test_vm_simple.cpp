#include <iostream>
#include <string>
#include "compiler/VM.h"
#include "compiler/Compiler.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

int main() {
    // 测试代码 - 使用正确的 MiniLang 语法
    std::string code = R"(
var x = 10;
var y = 20;
print(x + y);
)";
    
    // 词法分析
    Lexer lexer;
    auto tokens = lexer.scan(code);
    
    // 语法分析
    Parser parser;
    auto ast = parser.parse(tokens);
    if (parser.hasErrors()) {
        std::cerr << "Parser errors:" << std::endl;
        for (const auto& err : parser.getErrors()) {
            std::cerr << "  Line " << err.line << ": " << err.what() << std::endl;
        }
        return 1;
    }
    
    // 编译
    Compiler compiler;
    auto compileResult = compiler.compile(*ast);
    
    // VM 执行
    VM vm;
    vm.setOutputCallback([](const std::string& output) {
        std::cout << "Output: " << output << std::endl;
    });
    
    auto result = vm.execute(compileResult);
    if (result != VMResult::VM_OK) {
        std::cerr << "VM error: " << vm.getLastError() << std::endl;
        return 1;
    }
    
    std::cout << "VM execution completed successfully!" << std::endl;
    return 0;
}
