// super_init_test.cpp — 多级继承 super.init() 链的正确性测试
// 构造 A→B→C 三层继承，每层 init 显式调用 super.init() 初始化自身字段，
// 预期最终对象同时持有 a=1、b=2、c=3，验证 super 调用链不丢字段、不重排。

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

    // 逐阶段执行：词法扫描 → 语法解析 → 解释执行；任一步出错立即报错退出。
    // getA/getB/getC 用于把三层继承链上各自初始化的字段暴露出来，验证 super 调用不丢字段。
    // 阶段1：词法扫描。将源码字符串切分为 token 序列；若出现词法错误立即报错退出。
    Lexer lexer;
    auto tokens = lexer.scan(source);
    if (lexer.getDiagnostics().hasErrors()) {
        std::cerr << "Lex error: " << lexer.getDiagnostics().all()[0].message << "\n";
        return 1;
    }

    // 阶段2：语法解析。把 token 序列构造成 AST；语法错误同样立即退出。
    Parser parser;
    auto ast = parser.parse(tokens);
    if (parser.hasErrors()) {
        std::cerr << "Parse error: " << parser.getDiagnostics().all()[0].message << "\n";
        return 1;
    }

    // 阶段3：解释执行。运行 AST，并通过输出回调把每次 print 的内容收集到 output 向量中。
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

    // 校验预期输出：super.init() 链依次初始化 a=1、b=2、c=3，故按 A→B→C 顺序打印 1/2/3；
    // 若 super 链中任一层字段丢失或顺序错乱，输出将不等于 "1""2""3"。
    // Verify expected output: 1, 2, 3
    // 不变量断言：恰好 3 行输出且依次为 "1""2""3"，才说明三级继承链上字段无一丢失。
    bool ok = (output.size() == 3 &&
               output[0] == "1" &&
               output[1] == "2" &&
               output[2] == "3");
    std::cout << (ok ? "SUPER.INIT TEST: PASS" : "SUPER.INIT TEST: FAIL") << "\n";
    return ok ? 0 : 1;
}
