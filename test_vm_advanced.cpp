#include <iostream>
#include <string>
#include "compiler/VM.h"
#include "compiler/Compiler.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

void testCase(const std::string& name, const std::string& code) {
    std::cout << "\n=== Test: " << name << " ===" << std::endl;
    
    Lexer lexer;
    auto tokens = lexer.scan(code);
    
    Parser parser;
    auto ast = parser.parse(tokens);
    if (parser.hasErrors()) {
        std::cerr << "Parser errors:" << std::endl;
        for (const auto& err : parser.getErrors()) {
            std::cerr << "  Line " << err.line << ": " << err.what() << std::endl;
        }
        return;
    }
    
    Compiler compiler;
    auto compileResult = compiler.compile(*ast);
    
    VM vm;
    std::vector<std::string> outputs;
    vm.setOutputCallback([&outputs](const std::string& output) {
        outputs.push_back(output);
        std::cout << "  Output: " << output << std::endl;
    });
    
    auto result = vm.execute(compileResult);
    if (result != VMResult::VM_OK) {
        std::cerr << "VM error: " << vm.getLastError() << std::endl;
        return;
    }
    
    std::cout << "✓ Passed" << std::endl;
}

int main() {
    // 测试 1: 基本运算
    testCase("Basic Arithmetic", R"(
var a = 100;
var b = 50;
print(a + b);
print(a - b);
print(a * b);
print(a / b);
)");

    // 测试 2: 条件语句
    testCase("If Statement", R"(
var x = 10;
if (x > 5) {
    print(x);
}
)");

    // 测试 3: 循环
    testCase("While Loop", R"(
var i = 0;
while (i < 3) {
    print(i);
    i = i + 1;
}
)");

    // 测试 4: 函数
    testCase("Function", R"(
fun add(a, b) {
    return a + b;
}
print(add(3, 7));
)");

    std::cout << "\n=== All tests completed ===" << std::endl;
    return 0;
}
