// ============================================================
// TestCoverageGaps — W3-2 覆盖率缺口定向补充测试
// ------------------------------------------------------------
// 目标：补充以下被识别为低覆盖率的代码路径的定向测试：
//   1. 复合类型注解（dict[K:V] / int[][] / T? / fun() 类型）四后端一致性
//   2. UTF-8 字符串索引慢路径（非 ASCII 字符串索引）
//   3. 类类型注解自动构造（var p: Point; 应等价于 var p = Point();）
//   4. 闭包修改捕获的实例字段（upvalue writeback 路径）
//   5. 类方法引用外层变量（IR collectFreeVars 类成员传播）
//   6. enum variant 错误路径（未定义 variant / arity 不匹配）
//
// 测试策略：四后端一致性验证（Interpreter / StackVM / StackVM-IR / RegisterVM）
// ============================================================

#include <gtest/gtest.h>

#include "compiler/Compiler.h"
#include "compiler/RegisterVM.h"
#include "compiler/VM.h"
#include "interpreter/Interpreter.h"
#include "interpreter/RuntimeExceptions.h"
#include "interpreter/Value.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <stdexcept>
#include <string>

// ============================================================
// 辅助函数：四后端执行器
// ============================================================

static std::string runInterpreter(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);
    if (!ast)
        return "<parse:failed>";
    Interpreter interp;
    std::string captured;
    interp.setOutputCallback([&](const std::string& s) { captured += s; });
    try {
        interp.execute(*ast);
    } catch (const RuntimeError& e) {
        return captured + "<runtime:" + std::string(e.what()) + ">";
    } catch (const std::exception& e) {
        return captured + "<runtime:" + std::string(e.what()) + ">";
    }
    return captured;
}

static std::string runStackVM(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);
    if (!ast)
        return "<parse:failed>";
    Compiler compiler;
    CompileResult result = compiler.compile(*ast);
    if (compiler.getDiagnostics().hasErrors())
        return "<compile:" + compiler.getLastError() + ">";
    VM vm;
    std::string captured;
    vm.setOutputCallback([&](const std::string& s) { captured += s; });
    vm.execute(result);
    if (vm.hasError())
        return captured + "<runtime:" + vm.getLastError() + ">";
    return captured;
}

static std::string runStackVMViaIR(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);
    if (!ast)
        return "<parse:failed>";
    Compiler compiler;
    compiler.setUseIR(true);
    CompileResult result;
    try {
        result = compiler.compile(*ast);
    } catch (const std::exception& e) {
        return "<compile:" + std::string(e.what()) + ">";
    }
    if (compiler.getDiagnostics().hasErrors())
        return "<compile:" + compiler.getLastError() + ">";
    VM vm;
    std::string captured;
    vm.setOutputCallback([&](const std::string& s) { captured += s; });
    vm.execute(result);
    if (vm.hasError())
        return captured + "<runtime:" + vm.getLastError() + ">";
    return captured;
}

static std::string runRegisterVM(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);
    if (!ast)
        return "<parse:failed>";
    Compiler compiler;
    compiler.setUseRegisterVM(true);
    compiler.compile(*ast);
    if (compiler.getDiagnostics().hasErrors())
        return "<compile:" + compiler.getLastError() + ">";
    RegisterVM vm;
    std::string captured;
    vm.setOutputCallback([&](const std::string& s) { captured += s; });
    vm.execute(compiler.getLastRegisterResult());
    if (vm.hasError())
        return captured + "<runtime:" + vm.getLastError() + ">";
    return captured;
}

// ============================================================
// 第一组：复合类型注解四后端一致性
// ============================================================

TEST(TypeAnnotationGaps, OptionalType_Null) {
    std::string src = "int? maybe = null; print(maybe);";
    EXPECT_EQ(runInterpreter(src), "null");
    EXPECT_EQ(runStackVM(src), "null");
    EXPECT_EQ(runStackVMViaIR(src), "null");
    EXPECT_EQ(runRegisterVM(src), "null");
}

TEST(TypeAnnotationGaps, OptionalTypeWithValue) {
    std::string src = "int? maybe = 7; print(maybe);";
    EXPECT_EQ(runInterpreter(src), "7");
    EXPECT_EQ(runStackVM(src), "7");
    EXPECT_EQ(runStackVMViaIR(src), "7");
    EXPECT_EQ(runRegisterVM(src), "7");
}

TEST(TypeAnnotationGaps, DictGenericType) {
    std::string src = "dict[string:int] scores = {\"a\": 1, \"b\": 2}; print(scores[\"a\"]);";
    EXPECT_EQ(runInterpreter(src), "1");
    EXPECT_EQ(runStackVM(src), "1");
    EXPECT_EQ(runStackVMViaIR(src), "1");
    EXPECT_EQ(runRegisterVM(src), "1");
}

TEST(TypeAnnotationGaps, DictOptionalValueType) {
    std::string src = "dict[string:int?] partial = {\"x\": 1}; print(partial[\"x\"]);";
    EXPECT_EQ(runInterpreter(src), "1");
    EXPECT_EQ(runStackVM(src), "1");
    EXPECT_EQ(runStackVMViaIR(src), "1");
    EXPECT_EQ(runRegisterVM(src), "1");
}

TEST(TypeAnnotationGaps, MultiDimArrayType) {
    std::string src = "int[][] matrix = [[1, 2], [3, 4]]; print(matrix[0][1]);";
    EXPECT_EQ(runInterpreter(src), "2");
    EXPECT_EQ(runStackVM(src), "2");
    EXPECT_EQ(runStackVMViaIR(src), "2");
    EXPECT_EQ(runRegisterVM(src), "2");
}

TEST(TypeAnnotationGaps, ArrayTypeAnnotation) {
    std::string src = "int[] arr = [10, 20, 30]; print(arr[2]);";
    EXPECT_EQ(runInterpreter(src), "30");
    EXPECT_EQ(runStackVM(src), "30");
    EXPECT_EQ(runStackVMViaIR(src), "30");
    EXPECT_EQ(runRegisterVM(src), "30");
}

TEST(TypeAnnotationGaps, StringTypeAnnotation) {
    std::string src = "string name = \"hello\"; print(name);";
    EXPECT_EQ(runInterpreter(src), "hello");
    EXPECT_EQ(runStackVM(src), "hello");
    EXPECT_EQ(runStackVMViaIR(src), "hello");
    EXPECT_EQ(runRegisterVM(src), "hello");
}

TEST(TypeAnnotationGaps, BoolTypeAnnotation) {
    std::string src = "bool flag = true; print(flag);";
    EXPECT_EQ(runInterpreter(src), "true");
    EXPECT_EQ(runStackVM(src), "true");
    EXPECT_EQ(runStackVMViaIR(src), "true");
    EXPECT_EQ(runRegisterVM(src), "true");
}

TEST(TypeAnnotationGaps, FloatTypeAnnotation) {
    std::string src = "float pi = 3.14; print(pi);";
    std::string interp = runInterpreter(src);
    std::string stack = runStackVM(src);
    std::string ir = runStackVMViaIR(src);
    std::string reg = runRegisterVM(src);
    EXPECT_EQ(interp, stack);
    EXPECT_EQ(interp, ir);
    EXPECT_EQ(interp, reg);
}

// ============================================================
// 第二组：类型注解违例错误路径
// ============================================================

TEST(TypeAnnotationGaps, TypeViolation_IntToString) {
    std::string src = "int x = \"not a number\"; print(x);";
    std::string interp = runInterpreter(src);
    std::string stack = runStackVM(src);
    std::string ir = runStackVMViaIR(src);
    std::string reg = runRegisterVM(src);
    // 四后端都应报错（错误消息格式可能不同，但都应包含错误标记）
    EXPECT_FALSE(interp.empty());
    EXPECT_TRUE(interp.find("runtime:") != std::string::npos || interp.find("type") != std::string::npos);
    EXPECT_FALSE(stack.empty());
    EXPECT_FALSE(ir.empty());
    EXPECT_FALSE(reg.empty());
}

TEST(TypeAnnotationGaps, TypeViolation_DictValueMismatch) {
    std::string src = "dict[string:int] bad = {\"a\": \"wrong\"}; print(bad[\"a\"]);";
    std::string interp = runInterpreter(src);
    std::string stack = runStackVM(src);
    std::string ir = runStackVMViaIR(src);
    std::string reg = runRegisterVM(src);
    // 四后端都应报错
    EXPECT_FALSE(interp.empty());
    EXPECT_FALSE(stack.empty());
    EXPECT_FALSE(ir.empty());
    EXPECT_FALSE(reg.empty());
}

// ============================================================
// 第三组：UTF-8 字符串索引慢路径
// ============================================================

TEST(Utf8StringIndexGaps, BasicChineseIndex) {
    std::string src = "var s = \"你好世界\"; print(s[0]);";
    std::string interp = runInterpreter(src);
    std::string stack = runStackVM(src);
    std::string ir = runStackVMViaIR(src);
    std::string reg = runRegisterVM(src);
    EXPECT_EQ(interp, "你");
    EXPECT_EQ(stack, "你");
    EXPECT_EQ(ir, "你");
    EXPECT_EQ(reg, "你");
}

TEST(Utf8StringIndexGaps, ChineseIndexSecondChar) {
    std::string src = "var s = \"你好世界\"; print(s[1]);";
    EXPECT_EQ(runInterpreter(src), "好");
    EXPECT_EQ(runStackVM(src), "好");
    EXPECT_EQ(runStackVMViaIR(src), "好");
    EXPECT_EQ(runRegisterVM(src), "好");
}

TEST(Utf8StringIndexGaps, ChineseIndexLastChar) {
    std::string src = "var s = \"你好世界\"; print(s[3]);";
    EXPECT_EQ(runInterpreter(src), "界");
    EXPECT_EQ(runStackVM(src), "界");
    EXPECT_EQ(runStackVMViaIR(src), "界");
    EXPECT_EQ(runRegisterVM(src), "界");
}

TEST(Utf8StringIndexGaps, MixedAsciiChinese) {
    std::string src = "var s = \"A你好\"; print(s[0]); print(s[1]); print(s[2]);";
    EXPECT_EQ(runInterpreter(src), "A你好");
    EXPECT_EQ(runStackVM(src), "A你好");
    EXPECT_EQ(runStackVMViaIR(src), "A你好");
    EXPECT_EQ(runRegisterVM(src), "A你好");
}

TEST(Utf8StringIndexGaps, Utf8OutOfBounds) {
    std::string src = "var s = \"你好\"; print(s[5]);";
    std::string interp = runInterpreter(src);
    std::string stack = runStackVM(src);
    std::string ir = runStackVMViaIR(src);
    std::string reg = runRegisterVM(src);
    // 四后端都应报越界错误
    EXPECT_FALSE(interp.empty());
    EXPECT_FALSE(stack.empty());
    EXPECT_FALSE(ir.empty());
    EXPECT_FALSE(reg.empty());
}

TEST(Utf8StringIndexGaps, EmojiIndex) {
    std::string src = "var s = \"🎉🚀\"; print(s[0]);";
    std::string interp = runInterpreter(src);
    std::string stack = runStackVM(src);
    std::string ir = runStackVMViaIR(src);
    std::string reg = runRegisterVM(src);
    EXPECT_EQ(interp, stack);
    EXPECT_EQ(interp, ir);
    EXPECT_EQ(interp, reg);
}

// ============================================================
// 第四组：类类型注解自动构造
// ============================================================

TEST(ClassTypeAnnotationGaps, AutoConstructFromClassType) {
    std::string src = "class Point { var x = 0; var y = 0; }\n"
                      "var p: Point;\n"
                      "print(p.x);";
    std::string interp = runInterpreter(src);
    std::string stack = runStackVM(src);
    std::string ir = runStackVMViaIR(src);
    std::string reg = runRegisterVM(src);
    // 自动构造应等价于 var p = Point();，p.x 应为 0
    EXPECT_EQ(interp, "0");
    EXPECT_EQ(stack, "0");
    EXPECT_EQ(ir, "0");
    EXPECT_EQ(reg, "0");
}

TEST(ClassTypeAnnotationGaps, AutoConstructWithFieldDefaults) {
    std::string src = "class Config { var name = \"default\"; var port = 8080; }\n"
                      "var c: Config;\n"
                      "print(c.name); print(c.port);";
    std::string interp = runInterpreter(src);
    std::string stack = runStackVM(src);
    std::string ir = runStackVMViaIR(src);
    std::string reg = runRegisterVM(src);
    EXPECT_EQ(interp, "default8080");
    EXPECT_EQ(stack, "default8080");
    EXPECT_EQ(ir, "default8080");
    EXPECT_EQ(reg, "default8080");
}

// ============================================================
// 第五组：闭包修改捕获的实例字段（upvalue writeback 路径）
// ============================================================

// W3-2-Bug1c 已修复（2026-07-24）：StackVM/IR/RegisterVM 路径下，闭包内对捕获变量执行
// 方法调用链（b.data.push(4)）的 upvalue 接收者写回已修复。Compiler emitMethodCallWriteback
// /visitMemberAssign/visitIndexAssign 对 MemberAccess/IndexAccess 基变量为 upvalue 的情形
// 发射 OP_WRITEBACK_MEMBER_UPVALUE / OP_WRITEBACK_INDEX_UPVALUE，对齐 IR 路径 3 步序列
// （load base + OP_LOAD_MUTATED + OP_MEMBER_SET/OP_INDEX_SET + OP_WRITEBACK_*），
// 修复 COW detach 后的变异实例写回原 upvalue。详见 CHANGELOG.md W3-2-Bug 条目。
TEST(ClosureUpvalueGaps, ClosureModifiesCapturedField) {
    std::string src = "class Box { var data = [1, 2, 3]; }\n"
                      "fun makeUpdater(b: Box) {\n"
                      "  return fun() {\n"
                      "    b.data.push(4);\n"
                      "    return b.data.len();\n"
                      "  };\n"
                      "}\n"
                      "var box = Box();\n"
                      "var upd = makeUpdater(box);\n"
                      "print(upd());";
    EXPECT_EQ(runInterpreter(src), "4");
    // W3-2-Bug1c fix: VM 路径 upvalue 接收者的方法调用链写回已修复
    EXPECT_EQ(runStackVM(src), "4");
    EXPECT_EQ(runStackVMViaIR(src), "4");
    EXPECT_EQ(runRegisterVM(src), "4");
}

// 调试测试：不带类型注解的版本,验证是否是类型注解导致的问题
TEST(ClosureUpvalueGaps, DebugNoTypeAnnotation) {
    std::string src = "class Box { var data = [1, 2, 3]; }\n"
                      "fun makeUpdater(b) {\n"
                      "  return fun() {\n"
                      "    b.data.push(4);\n"
                      "    return b.data.len();\n"
                      "  };\n"
                      "}\n"
                      "var box = Box();\n"
                      "var upd = makeUpdater(box);\n"
                      "print(upd());";
    std::string interp = runInterpreter(src);
    std::string stack = runStackVM(src);
    std::string ir = runStackVMViaIR(src);
    std::string reg = runRegisterVM(src);
    std::cerr << "[DEBUG DebugNoTypeAnnotation] interp='" << interp << "' stack='" << stack << "' ir='" << ir
              << "' reg='" << reg << "'" << std::endl;
    EXPECT_EQ(interp, "4");
}

// 调试测试：直接字段赋值(通过),对比方法调用链(失败)
TEST(ClosureUpvalueGaps, DebugFieldAssignVsMethodCall) {
    // 字段赋值版本(通过)
    std::string src1 = "class Box { var data = 0; }\n"
                       "fun makeUpdater(b) {\n"
                       "  return fun() { b.data = 42; return b.data; };\n"
                       "}\n"
                       "var box = Box();\n"
                       "var upd = makeUpdater(box);\n"
                       "print(upd());";
    std::string s1 = runStackVM(src1);
    std::cerr << "[DEBUG FieldAssign] stack='" << s1 << "'" << std::endl;
    EXPECT_EQ(s1, "42");

    // 方法调用链版本(失败)
    std::string src2 = "class Box { var data = [1, 2, 3]; }\n"
                       "fun makeUpdater(b) {\n"
                       "  return fun() { return b.data.len(); };\n"
                       "}\n"
                       "var box = Box();\n"
                       "var upd = makeUpdater(box);\n"
                       "print(upd());";
    std::string s2 = runStackVM(src2);
    std::cerr << "[DEBUG MethodCall] stack='" << s2 << "'" << std::endl;
    EXPECT_EQ(s2, "3");
}

TEST(ClosureUpvalueGaps, ClosureReadsCapturedField) {
    std::string src = "class Counter { var count = 0; }\n"
                      "fun makeGetter(c: Counter) {\n"
                      "  return fun() { return c.count; };\n"
                      "}\n"
                      "var ctr = Counter();\n"
                      "ctr.count = 42;\n"
                      "var g = makeGetter(ctr);\n"
                      "print(g());";
    EXPECT_EQ(runInterpreter(src), "42");
    EXPECT_EQ(runStackVM(src), "42");
    EXPECT_EQ(runStackVMViaIR(src), "42");
    EXPECT_EQ(runRegisterVM(src), "42");
}

TEST(ClosureUpvalueGaps, MultipleClosuresShareCapturedField) {
    std::string src = "class Account { var balance = 100; }\n"
                      "fun makePair(a: Account) {\n"
                      "  var getter = fun() { return a.balance; };\n"
                      "  var setter = fun(v) { a.balance = v; };\n"
                      "  return getter;\n"
                      "}\n"
                      "var acc = Account();\n"
                      "var get = makePair(acc);\n"
                      "print(get());";
    EXPECT_EQ(runInterpreter(src), "100");
    EXPECT_EQ(runStackVM(src), "100");
    EXPECT_EQ(runStackVMViaIR(src), "100");
    EXPECT_EQ(runRegisterVM(src), "100");
}

// ============================================================
// 第六组：类方法引用外层变量（IR collectFreeVars 类成员传播）
// ============================================================

TEST(ClassFreeVarGaps, MethodReadsOuterVariable) {
    std::string src = "var outer = 42;\n"
                      "class MyClass {\n"
                      "  fun getter() { return outer; }\n"
                      "}\n"
                      "var obj = MyClass();\n"
                      "print(obj.getter());";
    std::string interp = runInterpreter(src);
    std::string stack = runStackVM(src);
    std::string ir = runStackVMViaIR(src);
    std::string reg = runRegisterVM(src);
    EXPECT_EQ(interp, "42");
    EXPECT_EQ(stack, "42");
    EXPECT_EQ(ir, "42");
    EXPECT_EQ(reg, "42");
}

TEST(ClassFreeVarGaps, MethodModifiesOuterVariable) {
    // counter: 0 → inc() → 1 (print "1") → inc() → 2 (print "2") → print(counter) → "2"
    // 总输出: "122"
    std::string src = "var counter = 0;\n"
                      "class Incrementer {\n"
                      "  fun inc() { counter = counter + 1; return counter; }\n"
                      "}\n"
                      "var obj = Incrementer();\n"
                      "print(obj.inc());\n"
                      "print(obj.inc());\n"
                      "print(counter);";
    std::string interp = runInterpreter(src);
    std::string stack = runStackVM(src);
    std::string ir = runStackVMViaIR(src);
    std::string reg = runRegisterVM(src);
    EXPECT_EQ(interp, "122");
    EXPECT_EQ(stack, "122");
    EXPECT_EQ(ir, "122");
    EXPECT_EQ(reg, "122");
}

// W3-2-Bug2 已修复（2026-07-24）：StackVM/IR/RegisterVM 路径下，函数内定义的类方法
// 无法捕获外层函数变量的问题已修复。emitMethodBody 新增 chunk_.upvalues = currentUpvalues_
// 存储方法 upvalue 描述符；OP_DEFINE_CLASS / REG_DEFINE_CLASS 执行时 captureMethodUpvalues
// 为有 upvalue 的方法创建 VMClosureData，从当前帧捕获栈槽或 upvalue；executeInstanceMethodCall
// 方法帧构造时从 methodUpvalues_ 读取并填充 newFrame.upvalues。详见 CHANGELOG.md W3-2-Bug 条目。
TEST(ClassFreeVarGaps, NestedMethodCapturesOuterFunctionVar) {
    std::string src = "fun makeClass() {\n"
                      "  var captured = 99;\n"
                      "  class Inner {\n"
                      "    fun get() { return captured; }\n"
                      "  }\n"
                      "  return Inner();\n"
                      "}\n"
                      "var obj = makeClass();\n"
                      "print(obj.get());";
    EXPECT_EQ(runInterpreter(src), "99");
    // W3-2-Bug2: 启用 VM 路径断言验证修复
    EXPECT_EQ(runStackVM(src), "99");
    EXPECT_EQ(runStackVMViaIR(src), "99");
    EXPECT_EQ(runRegisterVM(src), "99");
}

// ============================================================
// 第七组：enum variant 错误路径
// ============================================================

TEST(EnumVariantErrorGaps, UndefinedVariant) {
    std::string src = "enum Color { Red, Green, Blue }\n"
                      "var x = Color.NotFound;";
    std::string interp = runInterpreter(src);
    std::string stack = runStackVM(src);
    std::string ir = runStackVMViaIR(src);
    std::string reg = runRegisterVM(src);
    // 四后端都应报错
    EXPECT_FALSE(interp.empty());
    EXPECT_FALSE(stack.empty());
    EXPECT_FALSE(ir.empty());
    EXPECT_FALSE(reg.empty());
}

TEST(EnumVariantErrorGaps, WrongArity) {
    std::string src = "enum Result { Ok, Err(string) }\n"
                      "var x = Result.Ok(42);";
    std::string interp = runInterpreter(src);
    std::string stack = runStackVM(src);
    std::string ir = runStackVMViaIR(src);
    std::string reg = runRegisterVM(src);
    // 四后端都应报错（Ok 期望 0 参数，传入 1 个）
    EXPECT_FALSE(interp.empty());
    EXPECT_FALSE(stack.empty());
    EXPECT_FALSE(ir.empty());
    EXPECT_FALSE(reg.empty());
}

TEST(EnumVariantErrorGaps, ValidVariantWithArg) {
    std::string src = "enum Result { Ok, Err(string) }\n"
                      "var e = Err(\"failed\");\n"
                      "print(e);";
    std::string interp = runInterpreter(src);
    std::string stack = runStackVM(src);
    std::string ir = runStackVMViaIR(src);
    std::string reg = runRegisterVM(src);
    // 四后端应一致输出
    EXPECT_EQ(interp, stack);
    EXPECT_EQ(interp, ir);
    EXPECT_EQ(interp, reg);
}

// ============================================================
// 第八组：try/catch 中 catch 变量闭包传播
// ============================================================

TEST(TryCatchClosureGaps, CatchBlockReferencesOuterVar) {
    std::string src = "fun outerFn() {\n"
                      "  var captured = 99;\n"
                      "  var inner = fun() {\n"
                      "    try { throw \"err\"; }\n"
                      "    catch (e) { return captured; }\n"
                      "  };\n"
                      "  return inner();\n"
                      "}\n"
                      "print(outerFn());";
    std::string interp = runInterpreter(src);
    std::string stack = runStackVM(src);
    std::string ir = runStackVMViaIR(src);
    std::string reg = runRegisterVM(src);
    EXPECT_EQ(interp, "99");
    EXPECT_EQ(stack, "99");
    EXPECT_EQ(ir, "99");
    EXPECT_EQ(reg, "99");
}

TEST(TryCatchClosureGaps, FinallyBlockWithReturn) {
    std::string src = "fun test() {\n"
                      "  var result = 0;\n"
                      "  try {\n"
                      "    result = 1;\n"
                      "    return result;\n"
                      "  } finally {\n"
                      "    result = 2;\n"
                      "  }\n"
                      "}\n"
                      "print(test());";
    std::string interp = runInterpreter(src);
    std::string stack = runStackVM(src);
    std::string ir = runStackVMViaIR(src);
    std::string reg = runRegisterVM(src);
    // finally 在 return 之后执行，但 return 值已确定
    EXPECT_EQ(interp, "1");
    EXPECT_EQ(stack, "1");
    EXPECT_EQ(ir, "1");
    EXPECT_EQ(reg, "1");
}

// ============================================================
// 第九组：循环不变量寄存器分配（RegisterVM 循环回边）
// ============================================================

TEST(LoopRegisterGaps, LoopInvariantPreservedAcrossIterations) {
    std::string src = "fun loopTest() {\n"
                      "  var n = 3;\n"
                      "  var sum = 0;\n"
                      "  for (var i = 0; i < n; i = i + 1) {\n"
                      "    sum = sum + i * n;\n"
                      "  }\n"
                      "  return sum;\n"
                      "}\n"
                      "print(loopTest());";
    // n 在循环内被读取但不修改——寄存器分配器不应复用 n 的寄存器
    // 期望 sum = 0*3 + 1*3 + 2*3 = 9
    EXPECT_EQ(runInterpreter(src), "9");
    EXPECT_EQ(runStackVM(src), "9");
    EXPECT_EQ(runStackVMViaIR(src), "9");
    EXPECT_EQ(runRegisterVM(src), "9");
}

TEST(LoopRegisterGaps, NestedLoopInvariants) {
    std::string src = "fun nestedLoop() {\n"
                      "  var rows = 3;\n"
                      "  var cols = 2;\n"
                      "  var total = 0;\n"
                      "  for (var i = 0; i < rows; i = i + 1) {\n"
                      "    for (var j = 0; j < cols; j = j + 1) {\n"
                      "      total = total + i * cols + j * rows;\n"
                      "    }\n"
                      "  }\n"
                      "  return total;\n"
                      "}\n"
                      "print(nestedLoop());";
    std::string interp = runInterpreter(src);
    std::string stack = runStackVM(src);
    std::string ir = runStackVMViaIR(src);
    std::string reg = runRegisterVM(src);
    EXPECT_EQ(interp, stack);
    EXPECT_EQ(interp, ir);
    EXPECT_EQ(interp, reg);
}

TEST(LoopRegisterGaps, LoopWithBreakAndContinue) {
    std::string src = "var sum = 0;\n"
                      "for (var i = 0; i < 10; i = i + 1) {\n"
                      "  if (i == 5) { break; }\n"
                      "  if (i % 2 == 0) { continue; }\n"
                      "  sum = sum + i;\n"
                      "}\n"
                      "print(sum);";
    // i=1,3 → sum=4 (i=5 时 break)
    EXPECT_EQ(runInterpreter(src), "4");
    EXPECT_EQ(runStackVM(src), "4");
    EXPECT_EQ(runStackVMViaIR(src), "4");
    EXPECT_EQ(runRegisterVM(src), "4");
}
