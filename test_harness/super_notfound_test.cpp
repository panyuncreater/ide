// super_notfound_test.cpp — super 方法解析失败时的三后端一致性测试
// 定义 class A { func foo() { super.bar(); } }，其中 A 无父类，
// 因此 super.bar() 必然解析失败。分别用解释器、栈式 VM(IR)、寄存器 VM(IR)
// 执行同一源码，预期三者都报运行时/解析错误（输出均带 <runtime:...>），
// 用于核对三后端对"super 找不到父类方法"的错误处理口径一致。

#include <gtest/gtest.h>
#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "compiler/Compiler.h"
#include "compiler/VM.h"
#include "compiler/RegisterVM.h"
#include "interpreter/Interpreter.h"
#include "interpreter/RuntimeExceptions.h"
#include <iostream>
#include <string>
int main(int argc, char** argv) {
    std::string src =
        "class A {\n"
        "  func foo() { super.bar(); }\n"
        "}\n"
        "var a = A();\n"
        "a.foo();\n";
    // 共享前端：先对错误源码做词法扫描与语法解析，得到 AST（此处 super 调用本身语法合法，错误发生在运行/编译期）。
    Lexer lx; auto tk = lx.scan(src);
    Parser p; auto ast = p.parse(tk);
    // 若连解析都失败则提前退出（本用例预期解析可通过，错误应发生在后端执行阶段）。
    if (!ast) { std::cout << "PARSE FAIL" << std::endl; return 0; }
    // 后端1：解释器执行同一源码；捕获 super.bar() 因 A 无父类而找不到方法时抛出的运行时异常，期望输出含 <runtime:...>。
    { Interpreter interp; std::string out; interp.setOutputCallback([&](const std::string& s){ out+=s; });
      try { interp.execute(*ast); } catch(const std::exception& e){ out += std::string("<runtime:") + e.what() + ">"; }
      std::cout << "Interpreter: " << out << std::endl; }
    // 后端2：栈式 VM（IR 模式）编译并执行；同样期望因 super 解析失败而带 <runtime:...> 错误标记。
    { Compiler c; c.setUseIR(true); auto cr = c.compile(*ast);
      VM vm; std::string out; vm.setOutputCallback([&](const std::string& s){ out+=s; }); vm.execute(cr);
      if (vm.hasError()) out += std::string("<runtime:") + vm.getLastError() + ">";
      std::cout << "StackVM-IR: " << out << std::endl; }
    // 后端3：寄存器 VM（IR 模式）编译并执行；期望与另两个后端保持同一错误口径（均输出 <runtime:...>）。
    { Compiler c; c.setUseRegisterVM(true); c.compile(*ast);
      RegisterVM vm; std::string out; vm.setOutputCallback([&](const std::string& s){ out+=s; }); vm.execute(c.getLastRegisterResult());
      if (vm.hasError()) out += std::string("<runtime:") + vm.getLastError() + ">";
      std::cout << "RegVM-IR: " << out << std::endl; }
    return 0;
}
