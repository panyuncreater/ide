// ============================================================
// tests/TestOperatorOverload.cpp - 运算符重载测试（拓展二期·语言）
// ------------------------------------------------------------
// instance 算术 dunder 分派（__add/__sub/__mul/__div/__mod）：
//   - Interpreter: numericBinaryOp 报错前分派（tryOperatorOverload）
//   - StackVM: executeArithOps 帧注入（栈布局 [left,right]=[this,arg]）
//   - RegisterVM: executeArith 经 executeCallImpl 注入（returnReg=dst）
// 四后端一致性（EXPECT_ALL_BACKENDS 覆盖 Interpreter/StackVM/
// StackVM-IR/RegisterVM）+ 回退语义回归锁（无 dunder 仍报数值类型错误）。
// ============================================================
#include "common/ThreeBackends.h"

#include <gtest/gtest.h>

#include <string>

using minilang_test::runInterpreter;
using minilang_test::runRegVM;
using minilang_test::runStackVM;
using minilang_test::runStackVM_IR;

namespace {

/// 共享的 Vec 类声明（__add/__sub/__mul 返回新实例，__div/__mod 返回标量）
const char* kVecClass = "class Vec {\n"
                        "    var x = 0;\n"
                        "    fun init(x) { this.x = x; }\n"
                        "    fun __add(other) { return Vec(this.x + other.x); }\n"
                        "    fun __sub(other) { return Vec(this.x - other.x); }\n"
                        "    fun __mul(k) { return Vec(this.x * k); }\n"
                        "    fun __div(k) { return this.x / k; }\n"
                        "    fun __mod(k) { return this.x % k; }\n"
                        "}\n";

} // namespace

TEST(OperatorOverload, AddDispatchesDunderMethod) {
    std::string src = std::string(kVecClass) + "var a = Vec(1);\n"
                                               "var b = Vec(2);\n"
                                               "var c = a + b;\n"
                                               "print(c.x);\n";
    EXPECT_ALL_BACKENDS(src, "3");
}

TEST(OperatorOverload, AllFiveOperators) {
    std::string src = std::string(kVecClass) + "var a = Vec(10);\n"
                                               "var b = Vec(3);\n"
                                               "print((a + b).x);\n"  // 13
                                               "print((a - b).x);\n"  // 7
                                               "print((a * 4).x);\n"  // 40（右操作数为普通 int）
                                               "print(a / 3);\n"      // 3（int 截断除法，dunder 内）
                                               "print(a % 3);\n";     // 1
    EXPECT_ALL_BACKENDS(src, "1374031"); // 13,7,40,3,1
}

TEST(OperatorOverload, ChainedAddition) {
    // a + b + c：左结合，(a+b) 的结果实例继续分派 __add
    std::string src = std::string(kVecClass) + "var r = Vec(1) + Vec(2) + Vec(3);\n"
                                               "print(r.x);\n";
    EXPECT_ALL_BACKENDS(src, "6");
}

TEST(OperatorOverload, InheritedDunderMethod) {
    // 子类未定义 __add：沿继承链找到父类的 __add
    std::string src = std::string(kVecClass) + "class Vec2 extends Vec {\n"
                                               "    fun init(x) { this.x = x; }\n"
                                               "}\n"
                                               "var r = Vec2(5) + Vec2(7);\n"
                                               "print(r.x);\n";
    EXPECT_ALL_BACKENDS(src, "12");
}

TEST(OperatorOverload, DunderCanUseFieldsAndConditions) {
    // dunder 方法体内可访问 this 字段、使用条件逻辑（非平凡方法体）
    std::string src = "class Clamp {\n"
                      "    var v = 0;\n"
                      "    fun init(v) { this.v = v; }\n"
                      "    fun __add(n) {\n"
                      "        var s = this.v + n;\n"
                      "        if (s > 100) { return 100; }\n"
                      "        return s;\n"
                      "    }\n"
                      "}\n"
                      "print(Clamp(95) + 3);\n"
                      "print(Clamp(95) + 30);\n";
    EXPECT_ALL_BACKENDS(src, "98100");
}

TEST(OperatorOverload, NoDunderFallsBackToTypeError) {
    // 回退语义回归锁：无 __add 的实例做算术 → 仍报“算术运算需要数值类型”
    std::string src = "class Plain { var x = 0; }\n"
                      "var p = Plain();\n"
                      "var r = p + 1;\n"
                      "print(r);\n";
    EXPECT_NE(runInterpreter(src).find("算术运算需要数值类型"), std::string::npos);
    EXPECT_NE(runStackVM(src).find("算术运算需要数值类型"), std::string::npos);
    EXPECT_NE(runStackVM_IR(src).find("算术运算需要数值类型"), std::string::npos);
    EXPECT_NE(runRegVM(src).find("算术运算需要数值类型"), std::string::npos);
}

TEST(OperatorOverload, WrongArityRejected) {
    // 运算符方法参数数不为 1 → 四后端同文案报错
    std::string src = "class Bad {\n"
                      "    fun __add(a, b) { return 0; }\n"
                      "}\n"
                      "var r = Bad() + 1;\n";
    EXPECT_NE(runInterpreter(src).find("必须恰好接受 1 个参数"), std::string::npos);
    EXPECT_NE(runStackVM(src).find("必须恰好接受 1 个参数"), std::string::npos);
    EXPECT_NE(runStackVM_IR(src).find("必须恰好接受 1 个参数"), std::string::npos);
    EXPECT_NE(runRegVM(src).find("必须恰好接受 1 个参数"), std::string::npos);
}

TEST(OperatorOverload, DunderReservedPrefixStillRejectedForOtherNames) {
    // Lexer 白名单回归锁：白名单外的 __ 前缀标识符仍被 Lexer 拒绝，
    // 白名单内的 __add 正常通过（直接验证 Lexer 诊断，因 errorToken 不会
    // 传播到 run* helper 的返回值）。
    {
        Lexer lx;
        (void)lx.scan("var __secret = 1;");
        EXPECT_TRUE(lx.getDiagnostics().hasErrors());
    }
    {
        Lexer lx;
        (void)lx.scan("var a = __add;");
        EXPECT_FALSE(lx.getDiagnostics().hasErrors());
    }
}
