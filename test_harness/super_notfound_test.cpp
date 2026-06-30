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
    Lexer lx; auto tk = lx.scan(src);
    Parser p; auto ast = p.parse(tk);
    if (!ast) { std::cout << "PARSE FAIL" << std::endl; return 0; }
    // Interpreter
    { Interpreter interp; std::string out; interp.setOutputCallback([&](const std::string& s){ out+=s; });
      try { interp.execute(*ast); } catch(const std::exception& e){ out += std::string("<runtime:") + e.what() + ">"; }
      std::cout << "Interpreter: " << out << std::endl; }
    // StackVM IR
    { Compiler c; c.setUseIR(true); auto cr = c.compile(*ast);
      VM vm; std::string out; vm.setOutputCallback([&](const std::string& s){ out+=s; }); vm.execute(cr);
      if (vm.hasError()) out += std::string("<runtime:") + vm.getLastError() + ">";
      std::cout << "StackVM-IR: " << out << std::endl; }
    // RegVM IR
    { Compiler c; c.setUseRegisterVM(true); c.compile(*ast);
      RegisterVM vm; std::string out; vm.setOutputCallback([&](const std::string& s){ out+=s; }); vm.execute(c.getLastRegisterResult());
      if (vm.hasError()) out += std::string("<runtime:") + vm.getLastError() + ">";
      std::cout << "RegVM-IR: " << out << std::endl; }
    return 0;
}
