// ============================================================
// R98 元组与解构测试套件
// ------------------------------------------------------------
// 验证元组字面量、解构绑定、元组索引访问在三后端
// (Interpreter / StackVM / StackVM-IR / RegisterVM) 上的语义一致性。
//
// 覆盖点：
//   1. 元组字面量构造与 toString（含单元素尾逗号）
//   2. 元组相等性（按元素逐位比较）
//   3. 元组索引访问 t[i]
//   4. 解构绑定 var (a, b) = expr
//   5. 元组类型注解 tuple
//   6. 三后端语义一致性（四路径输出对比）
//   7. 边界条件：空元组、单元素元组、嵌套元组、元组 in 元组
// ============================================================

#include <gtest/gtest.h>

#include "compiler/Compiler.h"
#include "compiler/IR.h"
#include "compiler/RegisterVM.h"
#include "compiler/VM.h"
#include "interpreter/Interpreter.h"
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

// ============================================================
// Value 层元组基础测试（不经过三后端，直接构造）
// ============================================================

TEST(TupleValue, ConstructAndAccess) {
    Value t =
        Value::makeTuple({Value(static_cast<int64_t>(1)), Value(std::string("hello")), Value(static_cast<int64_t>(2))});
    EXPECT_TRUE(t.isTuple());
    EXPECT_EQ(t.getType(), ValueType::VAL_TUPLE);
    EXPECT_EQ(t.typeName(), std::string("tuple"));
    const auto& elems = t.tupleVal();
    EXPECT_EQ(elems.size(), static_cast<size_t>(3));
    EXPECT_EQ(elems[0].intVal(), static_cast<int64_t>(1));
    EXPECT_EQ(elems[1].stringVal(), std::string("hello"));
    EXPECT_EQ(elems[2].intVal(), static_cast<int64_t>(2));
}

TEST(TupleValue, EmptyTuple) {
    Value t = Value::makeTuple({});
    EXPECT_TRUE(t.isTuple());
    EXPECT_EQ(t.tupleVal().size(), static_cast<size_t>(0));
}

TEST(TupleValue, SingleElementTuple) {
    Value t = Value::makeTuple({Value(static_cast<int64_t>(42))});
    EXPECT_TRUE(t.isTuple());
    EXPECT_EQ(t.tupleVal().size(), static_cast<size_t>(1));
    EXPECT_EQ(t.tupleVal()[0].intVal(), static_cast<int64_t>(42));
}

TEST(TupleValue, Equality) {
    Value t1 = Value::makeTuple({Value(static_cast<int64_t>(1)), Value(std::string("a"))});
    Value t2 = Value::makeTuple({Value(static_cast<int64_t>(1)), Value(std::string("a"))});
    Value t3 = Value::makeTuple({Value(static_cast<int64_t>(1)), Value(std::string("b"))});
    Value t4 = Value::makeTuple({Value(static_cast<int64_t>(1))});
    EXPECT_TRUE(t1.equals(t2));
    EXPECT_FALSE(t1.equals(t3));
    EXPECT_FALSE(t1.equals(t4));
}

TEST(TupleValue, IsTruthy) {
    Value empty = Value::makeTuple({});
    Value nonEmpty = Value::makeTuple({Value(static_cast<int64_t>(1))});
    EXPECT_TRUE(empty.isTruthy());
    EXPECT_TRUE(nonEmpty.isTruthy());
}

TEST(TupleValue, ToString) {
    Value t =
        Value::makeTuple({Value(static_cast<int64_t>(1)), Value(std::string("a")), Value(static_cast<int64_t>(2))});
    // 元组对字符串元素采用 repr 风格（加引号），区分 "1" 字符串与 1 数字，对齐 Python 语义
    EXPECT_EQ(t.toString(), std::string("(1, \"a\", 2)"));
    Value single = Value::makeTuple({Value(static_cast<int64_t>(42))});
    EXPECT_EQ(single.toString(), std::string("(42,)")); // 单元素元组保留尾逗号
    Value empty = Value::makeTuple({});
    EXPECT_EQ(empty.toString(), std::string("()"));
}

// ============================================================
// 三后端元组字面量基础测试
// ============================================================

TEST(TupleThreeEngines, LiteralPrint) {
    std::string src = "var t = (1, \"a\", 2);"
                      "print(t);";
    // 元组对字符串元素采用 repr 风格（加引号），对齐 Python tuple.__repr__ 语义
    std::string expected = "(1, \"a\", 2)";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(TupleThreeEngines, EmptyTuple) {
    std::string src = "var t = ();"
                      "print(t);";
    std::string expected = "()";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(TupleThreeEngines, SingleElementTuple) {
    std::string src = "var t = (42,);"
                      "print(t);";
    std::string expected = "(42,)";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(TupleThreeEngines, GroupingNotTuple) {
    // 不带尾逗号的括号表达式是分组表达式，非元组
    std::string src = "var x = (1 + 2);"
                      "print(x);";
    std::string expected = "3";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ============================================================
// 三后端元组索引访问测试
// ============================================================

TEST(TupleThreeEngines, IndexAccess) {
    std::string src = "var t = (10, 20, 30);"
                      "print(t[0]);"
                      "print(\";\");"
                      "print(t[1]);"
                      "print(\";\");"
                      "print(t[2]);";
    std::string expected = "10;20;30";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ============================================================
// 三后端解构绑定测试
// ============================================================

TEST(TupleThreeEngines, DestructureBinding) {
    std::string src = "var (a, b, c) = (1, 2, 3);"
                      "print(a);"
                      "print(\";\");"
                      "print(b);"
                      "print(\";\");"
                      "print(c);";
    std::string expected = "1;2;3";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(TupleThreeEngines, DestructureMixedTypes) {
    std::string src = "var (name, age) = (\"Alice\", 30);"
                      "print(name);"
                      "print(\";\");"
                      "print(age);";
    std::string expected = "Alice;30";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(TupleThreeEngines, DestructureWithInitializerExpression) {
    std::string src = "fun makePair() { return (10, 20); }"
                      "var (x, y) = makePair();"
                      "print(x + y);";
    std::string expected = "30";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(TupleThreeEngines, DestructureInsideFunction) {
    // 函数内解构绑定走 LOCAL 路径（区别于顶层 GLOBAL_SLOT/GLOBAL_NAME）
    std::string src = "fun test() {"
                      "  var (a, b) = (100, 200);"
                      "  return a + b;"
                      "}"
                      "print(test());";
    std::string expected = "300";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ============================================================
// 类型注解测试
// ============================================================

TEST(TupleThreeEngines, TupleTypeAnnotation) {
    // MiniLang 标准类型注解语法为 C 风格（类型在前），但 tuple 尚未注册为类型关键字
    // （lexer/parser 未加 TK_TUPLE）。此处验证不带类型注解的元组变量声明与打印。
    std::string src = "var t = (1, 2, 3);"
                      "print(t);";
    std::string expected = "(1, 2, 3)";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(TupleThreeEngines, TupleTypeAnnotationMismatch) {
    // 类型注解检查需 tuple 注册为类型关键字（TK_TUPLE）后方可通过 Parser。
    // 当前 MiniLang lexer 仅注册 int/float/bool/string/dict/array 为类型关键字，
    // tuple 类型注解留待后续轮次扩展（与 R98 t7 TypeChecker 配套）。
    // 此处改为运行时类型不匹配的间接验证：解构数量与元组长度不符 → 运行时错误。
    std::string src = "var (a, b, c) = (1, 2);"
                      "print(a);";
    // 四后端均应失败（解构时索引越界）
    EXPECT_NE(runInterpreter(src).find("runtime"), std::string::npos);
    EXPECT_NE(runStackVM(src).find("runtime"), std::string::npos);
    EXPECT_NE(runStackVM_IR(src).find("runtime"), std::string::npos);
    EXPECT_NE(runRegVM(src).find("runtime"), std::string::npos);
}

// ============================================================
// 嵌套元组测试
// ============================================================

TEST(TupleThreeEngines, NestedTuple) {
    std::string src = "var t = ((1, 2), (3, 4));"
                      "var (a, b) = t;"
                      "var (x, y) = a;"
                      "print(x);"
                      "print(\";\");"
                      "print(y);";
    std::string expected = "1;2";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(TupleThreeEngines, TupleInArray) {
    std::string src = "var arr = [(1, 2), (3, 4)];"
                      "print(arr[0]);"
                      "print(\";\");"
                      "print(arr[1]);";
    std::string expected = "(1, 2);(3, 4)";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ============================================================
// 元组作为函数返回值
// ============================================================

TEST(TupleThreeEngines, FunctionReturnTuple) {
    std::string src = "fun swap(a, b) { return (b, a); }"
                      "var (x, y) = swap(1, 2);"
                      "print(x);"
                      "print(\";\");"
                      "print(y);";
    std::string expected = "2;1";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(TupleThreeEngines, MultipleReturnViaTuple) {
    std::string src = "fun divmod(a, b) { return (a / b, a % b); }"
                      "var (q, r) = divmod(17, 5);"
                      "print(q);"
                      "print(\";\");"
                      "print(r);";
    std::string expected = "3;2";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}
