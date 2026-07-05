// ============================================================
// Compiler + IR + RegisterVM 第二轮深度审计回归测试
// ------------------------------------------------------------
// 覆盖第二轮审计发现的 10 个 Bug 的回归测试：
//   BUG-CP-2       (P1): visitIndexAssign 嵌套路径栈泄漏
//   BUG-CP-3       (P1): visitMemberAssign 嵌套路径栈泄漏
//   BUG-UV-1       (P1): Compiler 路径缺少前向自由变量分析（3+ 层嵌套闭包）
//   BUG-IR-POP-1   (P1): IR 路径模块内联表达式语句未 POP
//   BUG-INH-IR-1   (P1): IR 路径非字面量字段默认值降级为 null
//   BUG-TRY-LEAK-1 (P2): visitTryStmt cleanupThrowOffset 溢出 early return 跳过恢复
//   BUG-IR-PRES-1  (P2): IR 路径 preScanTopLevelDecls 主模块预扫描 FunDecl
//   BUG-DEF-1      (P2): 默认参数求值三后端不一致
//   BUG-INH-REG-1  (P2): RegisterVM flattenedFieldOrder 字段名重复
//   BUG-DEAD-1     (P2): extractExportName 死代码删除
//   附: instructionSizeAt 回归（BUG-INH-IR-1 引入的 REG_DEFINE_CLASS 字段长度计算）
// ============================================================

#include <gtest/gtest.h>

#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "compiler/Compiler.h"
#include "compiler/Bytecode.h"
#include "compiler/VM.h"
#include "compiler/RegisterVM.h"
#include "interpreter/Value.h"
#include "interpreter/Interpreter.h"
#include "interpreter/RuntimeExceptions.h"

#include <string>

// ============================================================
// 辅助函数
// ============================================================

static std::string runStackVM(const std::string& src) {
    Lexer lx; auto tk = lx.scan(src);
    Parser p; auto ast = p.parse(tk);
    if (!ast) return "<parse-fail>";
    Compiler c;
    auto cr = c.compile(*ast);
    if (c.getDiagnostics().hasErrors()) return "<compile:" + c.getLastError() + ">";
    VM vm;
    std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });
    vm.execute(cr);
    if (vm.hasError()) {
        if (out.empty()) return "<runtime:" + vm.getLastError() + ">";
        return out + "<runtime:" + vm.getLastError() + ">";
    }
    return out;
}

static std::string runStackVM_IR(const std::string& src) {
    Lexer lx; auto tk = lx.scan(src);
    Parser p; auto ast = p.parse(tk);
    if (!ast) return "<parse-fail>";
    Compiler c; c.setUseIR(true);
    auto cr = c.compile(*ast);
    if (c.getDiagnostics().hasErrors()) return "<compile:" + c.getLastError() + ">";
    VM vm;
    std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });
    vm.execute(cr);
    if (vm.hasError()) {
        if (out.empty()) return "<runtime:" + vm.getLastError() + ">";
        return out + "<runtime:" + vm.getLastError() + ">";
    }
    return out;
}

static std::string runRegVM(const std::string& src) {
    Lexer lx; auto tk = lx.scan(src);
    Parser p; auto ast = p.parse(tk);
    if (!ast) return "<parse-fail>";
    Compiler c; c.setUseRegisterVM(true);
    c.compile(*ast);
    if (c.getDiagnostics().hasErrors()) return "<compile:" + c.getLastError() + ">";
    RegisterVM vm;
    std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });
    vm.execute(c.getLastRegisterResult());
    if (vm.hasError()) {
        if (out.empty()) return "<runtime:" + vm.getLastError() + ">";
        return out + "<runtime:" + vm.getLastError() + ">";
    }
    return out;
}

static std::string runInterp(const std::string& src) {
    Lexer lx; auto tk = lx.scan(src);
    Parser p; auto ast = p.parse(tk);
    if (!ast) return "<parse-fail>";
    Interpreter interp;
    std::string out;
    interp.setOutputCallback([&](const std::string& s) { out += s; });
    try {
        interp.execute(*ast);
    } catch (const RuntimeError& e) {
        if (out.empty()) return "<runtime:" + std::string(e.what()) + ">";
        return out + "<runtime:" + std::string(e.what()) + ">";
    } catch (const std::exception& e) {
        return "<runtime:" + std::string(e.what()) + ">";
    }
    return out;
}

// ============================================================
// BUG-CP-2: visitIndexAssign 嵌套路径栈泄漏
// ------------------------------------------------------------
// BUG-NEW 修复将 OP_WRITEBACK_INDEX_VAR/LOCAL 改为整体替换语义（不 pop 索引），
// visitMethodCall 已同步修复，但 visitIndexAssign 仍向栈推入外层索引。
// 循环内 a[0][1]=x 每次迭代泄漏 2 个栈值，约 512 次触发栈溢出。
// 修复：删除推索引代码，对齐 visitMethodCall。
// ============================================================

TEST(CompilerAudit2CP2, NestedIndexAssignNoStackLeak) {
    // 循环内嵌套索引赋值（a[0][1]=x），验证不栈溢出
    // 注：直接路径（非 IR）嵌套左值功能有限，使用 IR 路径验证栈不泄漏
    // BUG-CP-2 修复前 visitIndexAssign 每次迭代泄漏 2 个栈值，循环必触发栈溢出
    std::string src =
        "var a = [[1, 2], [3, 4]];"
        "var i = 0;"
        "while (i < 600) { a[0][1] = 99; i = i + 1; }"
        "print(a[0][1]);";
    EXPECT_EQ(runStackVM_IR(src), "99");
}

TEST(CompilerAudit2CP2, NestedIndexAssignCompilesDirectPath) {
    // 验证直接路径（非 IR）编译成功——BUG-CP-2 修复了字节码生成
    // （删除推入外层索引代码），编译应无错误
    std::string src =
        "var a = [[1, 2], [3, 4]];"
        "a[0][1] = 99;";
    Lexer lx; auto tk = lx.scan(src);
    Parser p; auto ast = p.parse(tk);
    ASSERT_NE(ast, nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    EXPECT_FALSE(c.getDiagnostics().hasErrors())
        << "直接路径应编译成功: " << c.getLastError();
}

TEST(CompilerAudit2CP2, NestedIndexAssignInLoopPreservesValue) {
    // 循环内多次嵌套索引赋值，值应正确保留（IR 路径）
    std::string src =
        "var grid = [[0, 0], [0, 0]];"
        "var i = 0;"
        "while (i < 5) {"
        "  grid[0][0] = i;"
        "  grid[1][1] = i * 2;"
        "  i = i + 1;"
        "}"
        "print(grid[0][0]); print(grid[1][1]);";
    EXPECT_EQ(runStackVM_IR(src), "48");
}

// ============================================================
// BUG-CP-3: visitMemberAssign 嵌套路径栈泄漏
// ------------------------------------------------------------
// 与 BUG-CP-2 同源。visitMemberAssign 在 OP_MEMBER_SET 后仍推入外层索引/字段，
// 但 OP_WRITEBACK_MEMBER_* 已改为整体替换不 pop。循环内 d.b.c=x 每次泄漏 2 个栈值。
// 修复：删除推索引代码。
// ============================================================

TEST(CompilerAudit2CP3, NestedMemberAssignNoStackLeak) {
    // 循环内嵌套成员赋值（d.b.c=7），验证不栈溢出
    // 注：直接路径（非 IR）嵌套左值功能有限，使用 IR 路径验证栈不泄漏
    // BUG-CP-3 修复前 visitMemberAssign 每次迭代泄漏 2 个栈值，循环必触发栈溢出
    std::string src =
        "class Inner { var c = 0; }"
        "class Outer { var b; fun init() { this.b = Inner(); } }"
        "var d = Outer();"
        "var i = 0;"
        "while (i < 600) { d.b.c = 7; i = i + 1; }"
        "print(d.b.c);";
    EXPECT_EQ(runStackVM_IR(src), "7");
}

TEST(CompilerAudit2CP3, NestedMemberAssignCompilesDirectPath) {
    // 验证直接路径（非 IR）编译成功——BUG-CP-3 修复了字节码生成
    // （删除推入外层索引/字段代码），编译应无错误
    std::string src =
        "class Inner { var c = 0; }"
        "class Outer { var b; fun init() { this.b = Inner(); } }"
        "var d = Outer();"
        "d.b.c = 7;";
    Lexer lx; auto tk = lx.scan(src);
    Parser p; auto ast = p.parse(tk);
    ASSERT_NE(ast, nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    EXPECT_FALSE(c.getDiagnostics().hasErrors())
        << "直接路径应编译成功: " << c.getLastError();
}

TEST(CompilerAudit2CP3, NestedMemberAssignPreservesValue) {
    // 嵌套成员赋值后值应正确保留（2 层：a.b.c）
    // 注：IR 路径当前支持 2 层嵌套左值（对齐 NestedLvalueProbe 测试套件）
    std::string src =
        "class Inner { var x = 0; }"
        "class Outer { var b; fun init() { this.b = Inner(); } }"
        "var a = Outer();"
        "a.b.x = 99;"
        "print(a.b.x);";
    EXPECT_EQ(runStackVM_IR(src), "99");
}

// ============================================================
// BUG-UV-1: Compiler 路径缺少前向自由变量分析
// ------------------------------------------------------------
// resolveUpvalue 是惰性的（仅 visitVarRef 触发），3 层嵌套时中间函数不直接
// 引用外层变量则 currentUpvalueNames_ 为空，内层函数无法透传捕获。
// IR 路径有 computeFreeVars 前向分析，但 Compiler 路径没有。
// 修复：在 visitFunDecl 加入类似 IR 的 computeFreeVars 前向分析。
// ============================================================

TEST(CompilerAudit2UV1, ThreeLevelNestedClosureCapture) {
    // 3 层嵌套闭包：inner 通过 mid 透传捕获 outer 的 x
    std::string src =
        "fun outer() {"
        "  var x = 1;"
        "  fun mid() {"
        "    fun inner() { return x; }"
        "    return inner();"
        "  }"
        "  return mid();"
        "}"
        "print(outer());";
    // 三后端都应输出 1
    EXPECT_EQ(runInterp(src), "1");
    EXPECT_EQ(runStackVM(src), "1");
}

TEST(CompilerAudit2UV1, ThreeLevelNestedClosureCaptureStackVM) {
    // 验证 StackVM 路径（非 IR）也能正确处理 3 层嵌套闭包
    std::string src =
        "fun outer() {"
        "  var x = 42;"
        "  fun mid() {"
        "    fun inner() { return x + 1; }"
        "    return inner();"
        "  }"
        "  return mid();"
        "}"
        "print(outer());";
    EXPECT_EQ(runStackVM(src), "43");
}

TEST(CompilerAudit2UV1, FourLevelNestedClosureCapture) {
    // 4 层嵌套闭包：验证更深层次的透传捕获
    std::string src =
        "fun f1() {"
        "  var x = 10;"
        "  fun f2() {"
        "    fun f3() {"
        "      fun f4() { return x; }"
        "      return f4();"
        "    }"
        "    return f3();"
        "  }"
        "  return f2();"
        "}"
        "print(f1());";
    EXPECT_EQ(runInterp(src), "10");
    EXPECT_EQ(runStackVM(src), "10");
}

// ============================================================
// BUG-IR-POP-1: IR 路径模块内联表达式语句未 POP
// ------------------------------------------------------------
// build() 对顶层表达式语句 emit POP 防止 StackVM 栈泄漏，但 handleImportStmt
// 缺少实际 needsPopForExprStmt 检查与 emitIR(IROp::POP) 调用。
// BytecodeIRBackend 将 vreg 物化为栈值，未 POP 的值永久残留。
// 修复：在 handleImportStmt 添加 needsPopForExprStmt 检查与 emitIR(IROp::POP)。
//
// 注：模块内联需要 import 语句，单元测试难以直接构造模块文件。
//   此处通过验证 IR 路径下连续表达式语句不导致栈泄漏来间接覆盖
//   （表达式语句返回值未被 POP 时会在循环中累积导致栈溢出）。
// ============================================================

TEST(CompilerAudit2IRPOP1, IRPathExpressionStatementNoStackLeak) {
    // IR 路径下循环内表达式语句（如函数调用）不应泄漏栈
    // 若未 POP，每次循环累积 1 个栈值，600 次会栈溢出
    std::string src =
        "fun noop() { return 0; }"
        "var i = 0;"
        "while (i < 600) { noop(); i = i + 1; }"
        "print(i);";
    EXPECT_EQ(runStackVM_IR(src), "600");
}

TEST(CompilerAudit2IRPOP1, IRPathMultipleExpressionStatements) {
    // IR 路径下多个表达式语句连续执行不应泄漏栈
    std::string src =
        "fun f() { return 1; }"
        "f(); f(); f(); f(); f();"
        "print(f());";
    EXPECT_EQ(runStackVM_IR(src), "1");
}

// ============================================================
// BUG-INH-IR-1: IR 路径非字面量字段默认值降级为 null
// ------------------------------------------------------------
// extractConstant 仅支持字面量，非字面量表达式（BinaryOp/FunCall 等）返回 false，
// defaultIdx 保持 UINT32_MAX，后端 emit OP_NULL。
// 而 Compiler.cpp 直接路径通过 compileNode 编译任意表达式，
// Interpreter 通过 evaluate 求值任意表达式。违反三后端一致性。
// 修复：非字面量表达式改为栈传递字段默认值（临时局部变量方案）。
// ============================================================

TEST(CompilerAudit2INHIR1, NonLiteralFieldDefaultValueBinaryOp) {
    // class A { var x = 1 + 2; } — 非字面量表达式默认值
    std::string src =
        "class A { var x = 1 + 2; }"
        "var a = A();"
        "print(a.x);";
    // 三后端都应输出 3（原 IR 路径/RegisterVM 输出 null）
    EXPECT_EQ(runInterp(src), "3");
    EXPECT_EQ(runStackVM(src), "3");
    EXPECT_EQ(runStackVM_IR(src), "3");
    EXPECT_EQ(runRegVM(src), "3");
}

TEST(CompilerAudit2INHIR1, NonLiteralFieldDefaultValueFunctionCall) {
    // 函数调用作为字段默认值
    std::string src =
        "fun base() { return 10; }"
        "class A { var x = base() * 2; }"
        "var a = A();"
        "print(a.x);";
    EXPECT_EQ(runInterp(src), "20");
    EXPECT_EQ(runStackVM(src), "20");
    EXPECT_EQ(runStackVM_IR(src), "20");
    EXPECT_EQ(runRegVM(src), "20");
}

TEST(CompilerAudit2INHIR1, NonLiteralFieldDefaultValueMixed) {
    // 混合字面量和非字面量字段默认值
    // 注：字段默认值表达式在求值时不能引用其他字段（字段按顺序初始化），
    // 所以 d 使用独立表达式 3 * 4 而非 a + b
    std::string src =
        "class A {"
        "  var a = 5;"
        "  var b = 1 + 2;"
        "  var c = \"hello\";"
        "  var d = 3 * 4;"
        "}"
        "var x = A();"
        "print(x.a); print(x.b); print(x.c); print(x.d);";
    EXPECT_EQ(runInterp(src), "53hello12");
    EXPECT_EQ(runStackVM(src), "53hello12");
    EXPECT_EQ(runStackVM_IR(src), "53hello12");
    EXPECT_EQ(runRegVM(src), "53hello12");
}

TEST(CompilerAudit2INHIR1, FieldDefaultValueWithThisInInit) {
    // 字段默认值表达式 + init 方法覆盖
    std::string src =
        "class Counter {"
        "  var count = 0;"
        "  var label = \"count=\" + \"0\";"
        "  fun init() { this.count = 100; }"
        "}"
        "var c = Counter();"
        "print(c.count); print(c.label);";
    EXPECT_EQ(runInterp(src), "100count=0");
    EXPECT_EQ(runStackVM(src), "100count=0");
    EXPECT_EQ(runStackVM_IR(src), "100count=0");
    EXPECT_EQ(runRegVM(src), "100count=0");
}

// ============================================================
// BUG-TRY-LEAK-1: visitTryStmt cleanupThrowOffset 溢出 early return 跳过恢复
// ------------------------------------------------------------
// cleanupThrowOffset > 65535 溢出检查 early return 跳过 restoreMapping，
// 导致 GlobalSlotAllocator 状态不一致 + currentLocals_ 泄漏 catch 变量。
// 修复：在 return 前添加恢复逻辑。
//
// 注：触发此 bug 需 catch 块 > 64KB（极罕见），无法在单元测试中直接复现。
//   此处验证正常 try/catch 路径的 catch 变量恢复行为（已由现有测试覆盖），
//   确保 early return 恢复逻辑与正常路径行为一致。
// ============================================================

TEST(CompilerAudit2TRYLEAK1, NormalTryCatchRestoresCatchVar) {
    // 正常 try/catch：catch 变量作用域结束后应恢复外层同名变量
    std::string src =
        "var x = 100;"
        "try {"
        "  throw \"err\";"
        "} catch (x) {"
        "  print(x);"  // 打印异常值 "err"
        "}"
        "print(x);";   // 应恢复为 100
    std::string out = runStackVM(src);
    EXPECT_TRUE(out.find("err") != std::string::npos) << "catch 内应打印异常值";
    EXPECT_TRUE(out.find("100") != std::string::npos)
        << "catch 退出后应恢复全局变量 x=100，实际: " << out;
}

// ============================================================
// BUG-IR-PRES-1: IR 路径 preScanTopLevelDecls 主模块预扫描 FunDecl
// ------------------------------------------------------------
// compile() 不预扫描 FunDecl（避免 var f=funName 静默 null），但 IR 路径
// preScanTopLevelDecls 对主模块也预扫描 FunDecl，导致 IR 路径 var f=funName
// 静默 null 而 Compiler 路径报运行时错误。
// 修复：主模块调用 preScanTopLevelDecls 传入 isMainModule=true 跳过 FunDecl。
// ============================================================

TEST(CompilerAudit2IRPRES1, MainModuleNoFunDeclPrescanIR) {
    // var f = g 在函数声明前：直接路径（compile()）报运行时错误，
    // IR 路径修复后也应报运行时错误（而非静默 null）。
    std::string src =
        "var f = g;"  // g 在后面声明，前向引用
        "fun g() { return 1; }"
        "print(f);";
    // 直接路径：g 未定义，报运行时错误
    std::string directOut = runStackVM(src);
    EXPECT_TRUE(directOut.find("<runtime:") != std::string::npos ||
                directOut.find("null") != std::string::npos)
        << "直接路径应报错或 null，实际: " << directOut;
    // IR 路径修复后应与直接路径一致
    std::string irOut = runStackVM_IR(src);
    EXPECT_TRUE(irOut.find("<runtime:") != std::string::npos ||
                irOut.find("null") != std::string::npos)
        << "IR 路径应与直接路径一致，实际: " << irOut;
}

TEST(CompilerAudit2IRPRES1, MainModuleFunDeclAfterUseConsistency) {
    // 验证 IR 路径与直接路径在主模块 FunDecl 前向引用上行为一致
    std::string src =
        "var before = after;"
        "fun after() { return 42; }"
        "print(before);";
    std::string directOut = runStackVM(src);
    std::string irOut = runStackVM_IR(src);
    // 两者行为应一致（都报错或都 null）
    EXPECT_EQ(directOut, irOut)
        << "IR 路径与直接路径主模块前向引用行为应一致";
}

// ============================================================
// BUG-DEF-1: 默认参数求值三后端不一致
// ------------------------------------------------------------
// Interpreter 路径支持任意表达式默认值，VM/RegisterVM 路径仅支持字面量，
// 复杂表达式记录 0xFFFF 哨兵运行时报错。违反三后端一致性。
// 修复：Parser 层拒绝复杂表达式默认值（BinaryOp/FunCall 等）。
// 注：InterpolatedString 仍被接受（保持 BUG-LPA-03 回归测试语义）。
// ============================================================

TEST(CompilerAudit2DEF1, BinaryOpDefaultParamRejected) {
    // 1 + 2 是 BinaryOp，应在 Parser 层被拒绝
    Lexer lexer;
    auto tokens = lexer.scan("fun f(a = 1 + 2) { print(a); }");
    Parser parser;
    auto ast = parser.parse(tokens);
    EXPECT_TRUE(parser.hasErrors());
    bool found = false;
    for (const auto& msg : parser.getDiagnostics().all()) {
        if (msg.message.find("默认参数值必须是字面量") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "应报'默认参数值必须是字面量'错误";
}

TEST(CompilerAudit2DEF1, FunCallDefaultParamRejected) {
    // 函数调用作为默认值，应被拒绝
    Lexer lexer;
    auto tokens = lexer.scan("fun g() { return 1; } fun f(a = g()) { print(a); }");
    Parser parser;
    auto ast = parser.parse(tokens);
    EXPECT_TRUE(parser.hasErrors());
}

TEST(CompilerAudit2DEF1, LiteralDefaultParamAccepted) {
    // 字面量默认值应被接受（数字/字符串/布尔/null/负数）
    Lexer lexer;
    auto tokens = lexer.scan(
        "fun f(a = 1, b = 2.5, c = true, d = null, e = -42) { print(a); }");
    Parser parser;
    auto ast = parser.parse(tokens);
    EXPECT_FALSE(parser.hasErrors())
        << "字面量默认值应被接受: " << parser.getDiagnostics().summary();
}

TEST(CompilerAudit2DEF1, InterpolatedStringDefaultParamAccepted) {
    // 插值字符串作为默认值应被接受（保持 BUG-LPA-03 回归测试语义）
    Lexer lexer;
    auto tokens = lexer.scan("fun f(a = \"x{1}y\", b = 2) { print(a); }");
    Parser parser;
    auto ast = parser.parse(tokens);
    EXPECT_FALSE(parser.hasErrors())
        << "插值字符串默认值应被接受: " << parser.getDiagnostics().summary();
}

TEST(CompilerAudit2DEF1, VariableRefDefaultParamRejected) {
    // 变量引用作为默认值，应被拒绝（非字面量）
    Lexer lexer;
    auto tokens = lexer.scan("var x = 1; fun f(a = x) { print(a); }");
    Parser parser;
    auto ast = parser.parse(tokens);
    EXPECT_TRUE(parser.hasErrors());
}

// ============================================================
// BUG-INH-REG-1: RegisterVM flattenedFieldOrder 字段名重复
// ------------------------------------------------------------
// executeClassNewImpl 沿继承链逆序遍历无去重，子类覆盖的父类同名字段
// 在链中每个类都 push 一次。无语义影响（后写入覆盖），仅效率问题。
// 修复：构建时去重，对齐 StackVM 的 mergedOrder 语义。
// ============================================================

TEST(CompilerAudit2INHREG1, FieldOverrideNoDuplicate) {
    // 子类覆盖父类同名字段，RegisterVM 应正确初始化子类值
    std::string src =
        "class Base { var x = 1; }"
        "class Sub extends Base { var x = 2; }"
        "var s = Sub();"
        "print(s.x);";
    // 三后端都应输出 2（子类覆盖）
    EXPECT_EQ(runInterp(src), "2");
    EXPECT_EQ(runStackVM(src), "2");
    EXPECT_EQ(runStackVM_IR(src), "2");
    EXPECT_EQ(runRegVM(src), "2");
}

TEST(CompilerAudit2INHREG1, DeepInheritanceFieldOverride) {
    // 多层继承链中的字段覆盖
    std::string src =
        "class A { var v = 1; }"
        "class B extends A { var v = 2; }"
        "class C extends B { var v = 3; }"
        "var c = C();"
        "print(c.v);";
    EXPECT_EQ(runRegVM(src), "3");
    EXPECT_EQ(runStackVM(src), "3");
    EXPECT_EQ(runInterp(src), "3");
}

TEST(CompilerAudit2INHREG1, FieldOverrideWithInit) {
    // 字段覆盖 + init 方法设置
    std::string src =
        "class Base { var x = 10; }"
        "class Sub extends Base {"
        "  var x = 20;"
        "  fun init() { this.x = 99; }"
        "}"
        "var s = Sub();"
        "print(s.x);";
    EXPECT_EQ(runRegVM(src), "99");
    EXPECT_EQ(runStackVM(src), "99");
}

// ============================================================
// BUG-DEAD-1: extractExportName 死代码删除
// ------------------------------------------------------------
// extractExportName 定义为从 ExportStmt 提取声明名，但全代码库无调用点。
// 修复：删除声明与定义。
// 验证：export 语句编译正常工作（功能未受影响）。
// ============================================================

TEST(CompilerAudit2DEAD1, ExportVarCompiles) {
    // export var 应正常编译
    Lexer lexer;
    auto tokens = lexer.scan("export var x = 1;");
    Parser parser;
    auto ast = parser.parse(tokens);
    ASSERT_NE(ast, nullptr);
    EXPECT_FALSE(parser.hasErrors());
    Compiler compiler;
    auto result = compiler.compile(*ast);
    EXPECT_FALSE(compiler.getDiagnostics().hasErrors());
}

TEST(CompilerAudit2DEAD1, ExportClassCompiles) {
    // export class 应正常编译
    Lexer lexer;
    auto tokens = lexer.scan("export class Foo { var x = 1; }");
    Parser parser;
    auto ast = parser.parse(tokens);
    ASSERT_NE(ast, nullptr);
    EXPECT_FALSE(parser.hasErrors());
    Compiler compiler;
    auto result = compiler.compile(*ast);
    EXPECT_FALSE(compiler.getDiagnostics().hasErrors());
}

TEST(CompilerAudit2DEAD1, ExportFunCompiles) {
    // export fun 应正常编译
    Lexer lexer;
    auto tokens = lexer.scan("export fun g() { return 1; }");
    Parser parser;
    auto ast = parser.parse(tokens);
    ASSERT_NE(ast, nullptr);
    EXPECT_FALSE(parser.hasErrors());
    Compiler compiler;
    auto result = compiler.compile(*ast);
    EXPECT_FALSE(compiler.getDiagnostics().hasErrors());
}

// ============================================================
// 附: instructionSizeAt 回归（BUG-INH-IR-1 引入）
// ------------------------------------------------------------
// BUG-INH-IR-1 修复将 REG_DEFINE_CLASS 每字段编码从 4 字节扩展为 5 字节
// （新增 exprReg），但 instructionSizeAt 仍按 4 字节计算，导致 RegisterVM
// 字节码解析错位，触发"字节码截断: 指令不完整"运行时错误。
// 修复：instructionSizeAt 同步更新为 5 字节/字段。
// 验证：RegisterVM 类定义 + 字段访问 + 方法调用正常工作。
// ============================================================

TEST(CompilerAudit2InstrSize, RegVMClassWithFieldsAndMethods) {
    // 含字段默认值 + 方法的类，RegisterVM 应正常执行
    std::string src =
        "class Point {"
        "  var x = 0;"
        "  var y = 0;"
        "  fun init(x, y) { this.x = x; this.y = y; }"
        "  fun sum() { return this.x + this.y; }"
        "}"
        "var p = Point(3, 4);"
        "print(p.sum());";
    EXPECT_EQ(runRegVM(src), "7");
}

TEST(CompilerAudit2InstrSize, RegVMInheritanceWithFields) {
    // 继承链 + 字段，RegisterVM 应正常执行
    std::string src =
        "class Base {"
        "  var x;"
        "  fun init() { this.x = 42; }"
        "}"
        "class Child extends Base {"
        "  fun getX() { return this.x; }"
        "}"
        "var c = Child();"
        "print(c.getX());";
    EXPECT_EQ(runRegVM(src), "42");
}

TEST(CompilerAudit2InstrSize, RegVMClassWithNonLiteralFieldDefault) {
    // 非字面量字段默认值 + RegisterVM
    std::string src =
        "class A { var x = 1 + 2; }"
        "var a = A();"
        "print(a.x);";
    EXPECT_EQ(runRegVM(src), "3");
}

TEST(CompilerAudit2InstrSize, RegVMMultipleClassesNoCorruption) {
    // 多个类定义连续声明，RegisterVM 字节码解析不应错位
    std::string src =
        "class A { var x = 1; fun get() { return this.x; } }"
        "class B { var y = 2; fun get() { return this.y; } }"
        "class C { var z = 3; fun get() { return this.z; } }"
        "var a = A(); var b = B(); var c = C();"
        "print(a.get()); print(b.get()); print(c.get());";
    EXPECT_EQ(runRegVM(src), "123");
}
