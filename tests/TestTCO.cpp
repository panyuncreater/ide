// ============================================================
// R109/L15 TCO（尾调用优化）测试套件
// ------------------------------------------------------------
// 验证 `return f(args)` 和 `return this.method(args)` 形态的自递归调用
// 在 VM 三路径（StackVM 直接路径 / StackVM IR 路径 / RegisterVM 路径）上
// 被优化为"参数赋值 + JUMP 函数入口"，跳过 OP_RETURN 的帧弹出，
// 复用当前帧执行下一轮递归——深度无界。
//
// Interpreter 路径是树遍历，无 TCO 能力，必须依赖 MAX_RECURSION_DEPTH 保护。
// 这是架构性设计差异：Interpreter 触发深度限制，VM 三路径触发指令数限制
// 或正常返回。本测试套件的核心断言是"VM 三路径不再触发深度限制"。
//
// L15 扩展后的识别条件（必须同时满足才允许 TCO，缺一不可）：
//   (1) AST 结构：return f(args) 或 return this.method(args)
//       （TCO::identifyTailCall 返回 SelfFunction 或 SelfMethod）
//   (2) 不在 try 块内（tryDepth_ == 0）
//   (3) currentFunctionDecl_ 非空
//   (4) args.size() <= params.size()（超出视为非尾调用）
//
// L15 放宽的限制：
//   - 闭包 upvalue：SelfFunction/SelfMethod 安全（upvalue 指向外层栈，不变）
//   - 默认参数：args.size() < params.size() 时用默认值填充
//   - 类方法：SelfMethod 保留 slot 0 (this) 和字段槽，仅覆盖参数槽
//
// 仍不支持的形态（需 VM 层 OP_TAIL_CALL，留作后续工作）：
//   - 互递归 return g(args)（g != 当前函数名，跨 chunk 跳转）
//   - 链式调用 return f(x)(y)（callee != nullptr，闭包目标编译期未知）
//
// 测试覆盖：
//   1. 基础尾递归：累加 / 阶乘（深度 > 256 时 VM 三路径正常返回，Interpreter 报错）
//   2. 非尾递归仍走深度限制：var x = f(n-1); return x; 不应被 TCO 优化
//   3. 互递归不启用 TCO：return other(n-1) 跨函数调用，需 OP_TAIL_CALL（未实现）
//   4. try 块内不启用 TCO：return f(args) 在 try 内仍触发深度限制
//   5. 类方法自调用启用 TCO（L15）：method 内 return this.method(args) 不再触发深度限制
//   6. 闭包函数自调用启用 TCO（L15）：捕获 upvalue 的函数内 return f(args) 不再触发深度限制
//   7. 默认参数自调用启用 TCO（L15）：return f() （f 有默认参数）不再触发深度限制
//   8. 三后端一致性：小规模 N 下 TCO 路径与 Interpreter 结果一致
// ============================================================

#include <gtest/gtest.h>

#include "compiler/Compiler.h"
#include "compiler/IR.h"
#include "compiler/RegisterVM.h"
#include "compiler/VM.h"
#include "interpreter/Interpreter.h"
#include "interpreter/RuntimeExceptions.h"
#include "interpreter/Value.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <string>

// ============================================================
// 辅助：四后端执行器
// ============================================================

static std::string runInterpreter(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    if (!ast)
        return "<parse-fail>";
    Interpreter interp;
    std::string out;
    interp.setOutputCallback([&](const std::string& s) { out += s; });
    try {
        interp.execute(*ast);
    } catch (const RuntimeError& e) {
        if (out.empty())
            return "<runtime:" + std::string(e.what()) + ">";
        return out + "<runtime:" + std::string(e.what()) + ">";
    } catch (const std::exception& e) {
        return "<runtime:" + std::string(e.what()) + ">";
    }
    return out;
}

static std::string runStackVM(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    if (!ast)
        return "<parse-fail>";
    Compiler c;
    auto cr = c.compile(*ast);
    if (c.getDiagnostics().hasErrors())
        return "<compile:" + c.getLastError() + ">";
    VM vm;
    std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });
    vm.execute(cr);
    if (vm.hasError()) {
        if (out.empty())
            return "<runtime:" + vm.getLastError() + ">";
        return out + "<runtime:" + vm.getLastError() + ">";
    }
    return out;
}

static std::string runStackVM_IR(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    if (!ast)
        return "<parse-fail>";
    Compiler c;
    c.setUseIR(true);
    auto cr = c.compile(*ast);
    if (c.getDiagnostics().hasErrors())
        return "<compile:" + c.getLastError() + ">";
    VM vm;
    std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });
    vm.execute(cr);
    if (vm.hasError()) {
        if (out.empty())
            return "<runtime:" + vm.getLastError() + ">";
        return out + "<runtime:" + vm.getLastError() + ">";
    }
    return out;
}

static std::string runRegVM(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    if (!ast)
        return "<parse-fail>";
    Compiler c;
    c.setUseRegisterVM(true);
    c.compile(*ast);
    if (c.getDiagnostics().hasErrors())
        return "<compile:" + c.getLastError() + ">";
    RegisterVM vm;
    std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });
    vm.execute(c.getLastRegisterResult());
    if (vm.hasError()) {
        if (out.empty())
            return "<runtime:" + vm.getLastError() + ">";
        return out + "<runtime:" + vm.getLastError() + ">";
    }
    return out;
}

static bool isRuntimeError(const std::string& s) {
    return s.find("<runtime:") != std::string::npos;
}

static bool isRecursionDepthError(const std::string& s) {
    // 三后端统一消息：包含"递归深度超过限制"
    return s.find("<runtime:") != std::string::npos && s.find("递归深度") != std::string::npos;
}

/// 四后端一致性验证宏（小规模 N，TCO 与非 TCO 都应一致）
#define EXPECT_FOUR_BACKENDS(src, expected)                                                                            \
    do {                                                                                                               \
        EXPECT_EQ(runInterpreter(src), expected) << "Interpreter failed";                                              \
        EXPECT_EQ(runStackVM(src), expected) << "StackVM failed";                                                      \
        EXPECT_EQ(runStackVM_IR(src), expected) << "StackVM_IR failed";                                                \
        EXPECT_EQ(runRegVM(src), expected) << "RegisterVM failed";                                                     \
    } while (0)

// ============================================================
// 1. 基础尾递归：TCO 在 VM 三路径正确启用
// ============================================================

TEST(TCOBasic, TailRecursionAccumulator) {
    // 经典尾递归累加：return f(n-1, acc+n) 是尾调用
    // 小规模 N：四后端结果一致
    std::string src = "func sum(n, acc) { if (n == 0) { return acc; } return sum(n - 1, acc + n); }\n"
                      "print(sum(10, 0));\n"; // 1+2+...+10 = 55
    EXPECT_FOUR_BACKENDS(src, "55");
}

TEST(TCOBasic, TailRecursionDeepVMNoDepthLimit) {
    // 深度 1000 > MAX_RECURSION_DEPTH (256)
    // Interpreter 触发深度限制；VM 三路径因 TCO 不触发深度限制
    std::string src = "func sum(n, acc) { if (n == 0) { return acc; } return sum(n - 1, acc + n); }\n"
                      "print(sum(1000, 0));\n"; // 1+2+...+1000 = 500500
    EXPECT_TRUE(isRecursionDepthError(runInterpreter(src))) << "Interpreter should hit depth limit";
    EXPECT_EQ(runStackVM(src), "500500") << "StackVM should TCO and return result";
    EXPECT_EQ(runStackVM_IR(src), "500500") << "StackVM_IR should TCO and return result";
    EXPECT_EQ(runRegVM(src), "500500") << "RegisterVM should TCO and return result";
}

TEST(TCOBasic, TailRecursionVeryDeep) {
    // 深度 10000，远超 MAX_RECURSION_DEPTH (256)
    // 验证 TCO 后 VM 三路径仍正常返回（无栈溢出）
    std::string src = "func sum(n, acc) { if (n == 0) { return acc; } return sum(n - 1, acc + n); }\n"
                      "print(sum(10000, 0));\n"; // 1+2+...+10000 = 50005000
    EXPECT_TRUE(isRecursionDepthError(runInterpreter(src))) << "Interpreter should hit depth limit";
    EXPECT_EQ(runStackVM(src), "50005000") << "StackVM should TCO";
    EXPECT_EQ(runStackVM_IR(src), "50005000") << "StackVM_IR should TCO";
    EXPECT_EQ(runRegVM(src), "50005000") << "RegisterVM should TCO";
}

TEST(TCOBasic, TailRecursionFactorial) {
    // 尾递归阶乘：return fact(n-1, n*acc) 是尾调用
    std::string src = "func fact(n, acc) { if (n <= 1) { return acc; } return fact(n - 1, n * acc); }\n"
                      "print(fact(5, 1));\n"; // 5! = 120
    EXPECT_FOUR_BACKENDS(src, "120");
}

TEST(TCOBasic, TailRecursionSingleParam) {
    // 单参数尾递归：return f(n-1) 是尾调用
    // 倒数到 0 返回 "done"
    std::string src = "func countdown(n) { if (n == 0) { return \"done\"; } return countdown(n - 1); }\n"
                      "print(countdown(500));\n"; // 深度 500 > 256
    EXPECT_TRUE(isRecursionDepthError(runInterpreter(src)));
    EXPECT_EQ(runStackVM(src), "done");
    EXPECT_EQ(runStackVM_IR(src), "done");
    EXPECT_EQ(runRegVM(src), "done");
}

// ============================================================
// 2. 非尾递归仍走深度限制（TCO 不应误触发）
// ============================================================

TEST(TCONotApplied, NonTailRecursionStillHitsDepthLimit) {
    // var x = f(n-1); return x; —— return 后还有赋值，非尾调用
    // 三后端都应触发深度限制
    std::string src = "func f(n) { if (n == 0) { return 0; } var x = f(n - 1); return x; }\n"
                      "print(f(500));\n";
    EXPECT_TRUE(isRecursionDepthError(runInterpreter(src)));
    EXPECT_TRUE(isRecursionDepthError(runStackVM(src)));
    EXPECT_TRUE(isRecursionDepthError(runStackVM_IR(src)));
    EXPECT_TRUE(isRecursionDepthError(runRegVM(src)));
}

TEST(TCONotApplied, ReturnWithArithmeticAfterCall) {
    // return f(n-1) + 0; —— return 表达式不是直接 FunCall，是 BinaryOp
    // TCO::isTailRecursiveReturn 检查 node->value->nodeType == NODE_FUN_CALL，BinaryOp 不匹配
    std::string src = "func f(n) { if (n == 0) { return 0; } return f(n - 1) + 0; }\n"
                      "print(f(500));\n";
    EXPECT_TRUE(isRecursionDepthError(runInterpreter(src)));
    EXPECT_TRUE(isRecursionDepthError(runStackVM(src)));
    EXPECT_TRUE(isRecursionDepthError(runStackVM_IR(src)));
    EXPECT_TRUE(isRecursionDepthError(runRegVM(src)));
}

// ============================================================
// 3. 互递归不启用 TCO（return other(args) 不是自递归）
// ============================================================

TEST(TCONotApplied, MutualRecursionNotTCO) {
    // even/odd 互递归：return odd(n-1) 不是对 even 自身的调用
    // 三后端都应触发深度限制
    std::string src = "func even(n) { if (n == 0) { return 1; } return odd(n - 1); }\n"
                      "func odd(n) { if (n == 0) { return 0; } return even(n - 1); }\n"
                      "print(even(1000));\n";
    EXPECT_TRUE(isRecursionDepthError(runInterpreter(src)));
    EXPECT_TRUE(isRecursionDepthError(runStackVM(src)));
    EXPECT_TRUE(isRecursionDepthError(runStackVM_IR(src)));
    EXPECT_TRUE(isRecursionDepthError(runRegVM(src)));
}

// ============================================================
// 4. try 块内不启用 TCO
// ============================================================

TEST(TCONotApplied, TailCallInTryBlockNotTCO) {
    // try 块内的 return f(args) 不应被 TCO 优化（tryStack_ handler 不清理 + finally/catch 语义破坏）。
    //
    // 函数无 base case：若 TCO 错误启用，n 递减至负无穷永不返回（测试挂起超时）。
    // TCO 正确不启用时：递归到 MAX_RECURSION_DEPTH/MAX_FRAMES（256），抛出深度限制错误。
    //
    // L14 语义变更后：try/catch 捕获 runtimeError（含递归深度错误），catch 块返回 0。
    // 因此四后端均输出 "0"——这验证了 TCO 未启用（否则会无限递归挂起），
    // 同时验证了 L14 的 try/catch 能捕获递归深度错误。
    std::string src = "func f(n) { try { return f(n - 1); } catch (e) { return 0; } }\n"
                      "print(f(500));\n";
    EXPECT_EQ(runInterpreter(src), "0");
    EXPECT_EQ(runStackVM(src), "0");
    EXPECT_EQ(runStackVM_IR(src), "0");
    EXPECT_EQ(runRegVM(src), "0");
}

// ============================================================
// 5. 类方法自调用启用 TCO（L15 扩展：SelfMethod）
// ============================================================

TEST(TCOApplied, MethodTailCallSelfCall) {
    // L15: 类方法 self_call 内的 return this.self_call() 现在可被 TCO 优化。
    // 保留 slot 0 (this) 和字段槽，仅覆盖参数槽，JUMP 回方法入口。
    // VM 三路径不触发深度限制；Interpreter 仍触发（树遍历无 TCO）。
    std::string src = "class Counter {\n"
                      "  func self_call(n) { if (n == 0) { return 0; } return this.self_call(n - 1); }\n"
                      "}\n"
                      "var c = Counter();\n"
                      "print(c.self_call(500));\n";
    EXPECT_TRUE(isRecursionDepthError(runInterpreter(src)));
    EXPECT_EQ(runStackVM(src), "0");
    EXPECT_EQ(runStackVM_IR(src), "0");
    EXPECT_EQ(runRegVM(src), "0");
}

// ============================================================
// 6. 闭包函数自调用启用 TCO（L15 扩展：放宽 upvalue 限制）
// ============================================================

TEST(TCOApplied, ClosureTailCallSelfRecursion) {
    // L15: 捕获 upvalue 的函数 inner 内 return inner(args) 是自递归。
    // upvalue 指向外层函数 outer 的栈槽，递归不覆盖外层栈槽，安全启用 TCO。
    std::string src = "func outer() {\n"
                      "  var x = 100;\n"
                      "  func inner(n) { if (n == 0) { return x; } return inner(n - 1); }\n"
                      "  return inner(500);\n"
                      "}\n"
                      "print(outer());\n";
    EXPECT_TRUE(isRecursionDepthError(runInterpreter(src)));
    EXPECT_EQ(runStackVM(src), "100");
    EXPECT_EQ(runStackVM_IR(src), "100");
    EXPECT_EQ(runRegVM(src), "100");
}

// ============================================================
// 7. 默认参数自调用启用 TCO（L15 扩展：默认参数填充）
// ============================================================

TEST(TCOApplied, DefaultParamTailCallSelfRecursion) {
    // L15: f 有默认参数；return f(n-1) 参数数量 < 形参数量，
    // TCO 路径用默认值 m=0 填充缺失参数，JUMP 回函数入口。
    std::string src = "func f(n, m = 0) { if (n == 0) { return m; } return f(n - 1); }\n"
                      "print(f(500));\n";
    EXPECT_TRUE(isRecursionDepthError(runInterpreter(src)));
    EXPECT_EQ(runStackVM(src), "0");
    EXPECT_EQ(runStackVM_IR(src), "0");
    EXPECT_EQ(runRegVM(src), "0");
}

TEST(TCOApplied, MethodTailCallPreservesFields) {
    // L15: 方法自调用期间字段值应保持不变（TCO 不覆盖 this 和字段槽）。
    // Counter.value 在递归过程中被读取，验证未被破坏。
    std::string src = "class Counter {\n"
                      "  var count = 0;\n"
                      "  func recurse(n) { if (n == 0) { return this.count; } return this.recurse(n - 1); }\n"
                      "}\n"
                      "var c = Counter();\n"
                      "c.count = 42;\n"
                      "print(c.recurse(500));\n";
    EXPECT_TRUE(isRecursionDepthError(runInterpreter(src)));
    EXPECT_EQ(runStackVM(src), "42");
    EXPECT_EQ(runStackVM_IR(src), "42");
    EXPECT_EQ(runRegVM(src), "42");
}

TEST(TCOApplied, MethodTailCallWithAccumulator) {
    // L15: 方法自调用带累加器参数，验证参数槽正确覆盖。
    std::string src = "class Accumulator {\n"
                      "  func sum(n, acc) { if (n == 0) { return acc; } return this.sum(n - 1, acc + n); }\n"
                      "}\n"
                      "var a = Accumulator();\n"
                      "print(a.sum(500, 0));\n"; // 1+2+...+500 = 125250
    EXPECT_TRUE(isRecursionDepthError(runInterpreter(src)));
    EXPECT_EQ(runStackVM(src), "125250");
    EXPECT_EQ(runStackVM_IR(src), "125250");
    EXPECT_EQ(runRegVM(src), "125250");
}

// ============================================================
// 8. 三后端一致性：小规模 N 下 TCO 路径与 Interpreter 结果一致
// ============================================================

TEST(TCOConsistency, SmallNResultsMatch) {
    // 小规模 N（< 256）：四后端都不触发深度限制，结果应一致
    std::string src = "func sum(n, acc) { if (n == 0) { return acc; } return sum(n - 1, acc + n); }\n"
                      "print(sum(100, 0));\n"; // 1+2+...+100 = 5050
    EXPECT_FOUR_BACKENDS(src, "5050");
}

TEST(TCOConsistency, StringAccumulator) {
    // 字符串累加尾递归（小规模 N，避免指令数限制）
    std::string src = "func concat(n, acc) { if (n == 0) { return acc; } return concat(n - 1, acc + \"x\"); }\n"
                      "print(concat(10, \"\"));\n"; // "xxxxxxxxxx"
    EXPECT_FOUR_BACKENDS(src, "xxxxxxxxxx");
}

TEST(TCOConsistency, BooleanReturn) {
    // 尾递归返回布尔值
    std::string src = "func isEven(n) { if (n == 0) { return true; } if (n == 1) { return false; } "
                      "return isEven(n - 2); }\n"
                      "print(isEven(20));\n";
    EXPECT_FOUR_BACKENDS(src, "true");
}

// ============================================================
// 9. TCO 不破坏 TYPE_CHECK（返回类型注解）
// ============================================================

TEST(TCOTypeCheck, AnnotatedReturnStillTypeChecked) {
    // 有返回类型注解的尾递归函数：TCO 路径跳过 TYPE_CHECK，
    // 但递归调用的 return 会再次触发检查——最终结果类型不符应报错
    std::string src = "func f(n) -> int { if (n == 0) { return \"wrong\"; } return f(n - 1); }\n"
                      "print(f(10));\n";
    // 四后端都应报类型检查错误
    EXPECT_TRUE(isRuntimeError(runInterpreter(src)));
    EXPECT_TRUE(isRuntimeError(runStackVM(src)));
    EXPECT_TRUE(isRuntimeError(runStackVM_IR(src)));
    EXPECT_TRUE(isRuntimeError(runRegVM(src)));
}
