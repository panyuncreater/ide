// ============================================================
// R99 枚举与 ADT + match 表达式测试套件
// ------------------------------------------------------------
// 验证 enum 声明、variant 构造、match 表达式在三后端
// (Interpreter / StackVM / StackVM-IR / RegisterVM) 上的语义一致性。
//
// 覆盖点：
//   1. 简单 enum（无参数 variant）声明与构造
//   2. ADT enum（带参数 variant）构造与字段访问
//   3. match 表达式 WILDCARD/LITERAL/VARIANT 模式匹配
//   4. match 绑定变量作用域
//   5. match default case
//   6. enum variant 类型注解
//   7. 三后端语义一致性（四路径输出对比）
//   8. 边界条件：未匹配、参数数量校验、未定义 enum/variant
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
// Value 层 enum variant 基础测试（不经过三后端，直接构造）
// ============================================================

TEST(EnumValue, ConstructAndAccess) {
    Value v = Value::makeEnumVariant("Color", "Red", {});
    EXPECT_TRUE(v.isEnumVariant());
    EXPECT_EQ(v.getType(), ValueType::VAL_ENUM_VARIANT);
    EXPECT_EQ(v.enumVariantEnumName(), std::string("Color"));
    EXPECT_EQ(v.enumVariantName(), std::string("Red"));
    EXPECT_EQ(v.enumVariantFields().size(), static_cast<size_t>(0));
}

TEST(EnumValue, AdtVariantWithFields) {
    Value v = Value::makeEnumVariant("Option", "Some", {Value(static_cast<int64_t>(42)), Value(std::string("hello"))});
    EXPECT_TRUE(v.isEnumVariant());
    EXPECT_EQ(v.enumVariantEnumName(), std::string("Option"));
    EXPECT_EQ(v.enumVariantName(), std::string("Some"));
    EXPECT_EQ(v.enumVariantFields().size(), static_cast<size_t>(2));
    EXPECT_EQ(v.enumVariantFields()[0].intVal(), static_cast<int64_t>(42));
    EXPECT_EQ(v.enumVariantFields()[1].stringVal(), std::string("hello"));
}

TEST(EnumValue, Equality) {
    Value v1 = Value::makeEnumVariant("Color", "Red", {});
    Value v2 = Value::makeEnumVariant("Color", "Red", {});
    Value v3 = Value::makeEnumVariant("Color", "Green", {});
    Value v4 = Value::makeEnumVariant("Shape", "Red", {});
    EXPECT_TRUE(v1.equals(v2));
    EXPECT_FALSE(v1.equals(v3));
    EXPECT_FALSE(v1.equals(v4));
}

TEST(EnumValue, AdtEquality) {
    Value v1 = Value::makeEnumVariant("Option", "Some", {Value(static_cast<int64_t>(1))});
    Value v2 = Value::makeEnumVariant("Option", "Some", {Value(static_cast<int64_t>(1))});
    Value v3 = Value::makeEnumVariant("Option", "Some", {Value(static_cast<int64_t>(2))});
    EXPECT_TRUE(v1.equals(v2));
    EXPECT_FALSE(v1.equals(v3));
}

TEST(EnumValue, ToString) {
    Value v = Value::makeEnumVariant("Color", "Red", {});
    EXPECT_EQ(v.toString(), std::string("Color.Red"));
    Value adt = Value::makeEnumVariant("Option", "Some", {Value(static_cast<int64_t>(42))});
    EXPECT_EQ(adt.toString(), std::string("Option.Some(42)"));
}

// ============================================================
// 三后端简单 enum 测试（无参数 variant）
// ============================================================

TEST(EnumThreeEngines, SimpleEnumPrint) {
    std::string src = "enum Color { Red, Green, Blue }"
                      "var c = Color.Red;"
                      "print(c);";
    std::string expected = "Color.Red";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(EnumThreeEngines, EnumVariantEquality) {
    std::string src = "enum Color { Red, Green, Blue }"
                      "var a = Color.Red;"
                      "var b = Color.Red;"
                      "var c = Color.Green;"
                      "print(a == b);"
                      "print(\";\");"
                      "print(a == c);";
    std::string expected = "true;false";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ============================================================
// 三后端 ADT enum 测试（带参数 variant）
// ============================================================

TEST(EnumThreeEngines, AdtVariantPrint) {
    std::string src = "enum Option<T> { Some(T), None }"
                      "var s = Option.Some(42);"
                      "var n = Option.None;"
                      "print(s);"
                      "print(\";\");"
                      "print(n);";
    std::string expected = "Option.Some(42);Option.None";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(EnumThreeEngines, AdtVariantWithStringArg) {
    std::string src = "enum Result<T, E> { Ok(T), Err(E) }"
                      "var ok = Result.Ok(\"success\");"
                      "print(ok);";
    std::string expected = "Result.Ok(\"success\")";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ============================================================
// 三后端 match 表达式测试
// ============================================================

TEST(MatchThreeEngines, SimpleVariantMatch) {
    std::string src = "enum Color { Red, Green, Blue }"
                      "var c = Color.Green;"
                      "var r = match (c) {"
                      "    case Color.Red => \"r\";"
                      "    case Color.Green => \"g\";"
                      "    case Color.Blue => \"b\";"
                      "};"
                      "print(r);";
    std::string expected = "g";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(MatchThreeEngines, MatchWithDefault) {
    std::string src = "enum Color { Red, Green, Blue }"
                      "var c = Color.Blue;"
                      "var r = match (c) {"
                      "    case Color.Red => \"r\";"
                      "    default => \"other\";"
                      "};"
                      "print(r);";
    std::string expected = "other";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(MatchThreeEngines, MatchLiteralPattern) {
    std::string src = "var x = 2;"
                      "var r = match (x) {"
                      "    case 1 => \"one\";"
                      "    case 2 => \"two\";"
                      "    case 3 => \"three\";"
                      "    default => \"other\";"
                      "};"
                      "print(r);";
    std::string expected = "two";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(MatchThreeEngines, MatchWildcardPattern) {
    std::string src = "var x = 999;"
                      "var r = match (x) {"
                      "    case 1 => \"one\";"
                      "    case _ => \"anything\";"
                      "};"
                      "print(r);";
    std::string expected = "anything";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(MatchThreeEngines, MatchAdtVariantBindings) {
    std::string src = "enum Option<T> { Some(T), None }"
                      "var s = Option.Some(42);"
                      "var r = match (s) {"
                      "    case Option.Some(x) => x + 1;"
                      "    case Option.None => 0;"
                      "};"
                      "print(r);";
    std::string expected = "43";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(MatchThreeEngines, MatchAdtVariantMultiBindings) {
    std::string src = "enum Pair<A, B> { P(A, B) }"
                      "var pr = Pair.P(10, 20);"
                      "var r = match (pr) {"
                      "    case Pair.P(a, b) => a + b;"
                      "};"
                      "print(r);";
    std::string expected = "30";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(MatchThreeEngines, MatchAdtVariantWildcardBinding) {
    // 使用 _ 跳过某些绑定
    std::string src = "enum Triple<A, B, C> { T(A, B, C) }"
                      "var t = Triple.T(1, 2, 3);"
                      "var r = match (t) {"
                      "    case Triple.T(a, _, c) => a * 100 + c;"
                      "};"
                      "print(r);";
    std::string expected = "103";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(MatchThreeEngines, MatchBlockBody) {
    // match case body 为 Block
    std::string src = "enum Color { Red, Green, Blue }"
                      "var c = Color.Red;"
                      "var r = match (c) {"
                      "    case Color.Red => { var x = 100; x + 1; }"
                      "    case Color.Green => 200;"
                      "    default => 0;"
                      "};"
                      "print(r);";
    std::string expected = "101";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(MatchThreeEngines, MatchNestedMatch) {
    // 嵌套 match 表达式
    std::string src = "enum Color { Red, Green, Blue }"
                      "var c1 = Color.Red;"
                      "var c2 = Color.Green;"
                      "var r = match (c1) {"
                      "    case Color.Red => match (c2) {"
                      "        case Color.Green => \"Red+Green\";"
                      "        default => \"Red+other\";"
                      "    };"
                      "    default => \"other\";"
                      "};"
                      "print(r);";
    std::string expected = "Red+Green";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ============================================================
// enum variant 类型注解测试
// ============================================================

TEST(EnumTypeAnnotation, VarDeclEnumType) {
    std::string src = "enum Color { Red, Green, Blue }"
                      "var c: Color = Color.Red;"
                      "print(c);";
    std::string expected = "Color.Red";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(EnumTypeAnnotation, GenericEnumAnnotation) {
    // "enum" 泛型注解匹配任意 enum variant
    std::string src = "enum Color { Red, Green, Blue }"
                      "var c: enum = Color.Red;"
                      "print(c);";
    std::string expected = "Color.Red";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ============================================================
// 边界条件测试
// ============================================================

TEST(EnumBoundary, UndefinedEnum) {
    std::string src = "var c = NoSuchEnum.Foo;";
    // 四后端均应报错
    EXPECT_TRUE(runInterpreter(src).find("<runtime:") != std::string::npos);
    EXPECT_TRUE(runStackVM(src).find("<runtime:") != std::string::npos);
    EXPECT_TRUE(runStackVM_IR(src).find("<runtime:") != std::string::npos);
    EXPECT_TRUE(runRegVM(src).find("<runtime:") != std::string::npos);
}

TEST(EnumBoundary, UndefinedVariant) {
    std::string src = "enum Color { Red, Green, Blue }"
                      "var c = Color.NoSuchVariant;";
    // L5 fix: 四后端均通过 enumRegistry_ 校验 variant 名，报运行时错误。
    // StackVM (VMContainers.cpp OP_BUILD_ENUM_VARIANT) / RegisterVM (RegisterVM.cpp
    // REG_BUILD_ENUM_VARIANT) 在运行时检查 enum 已声明、variant 存在、arity 一致。
    EXPECT_TRUE(runInterpreter(src).find("<runtime:") != std::string::npos);
    EXPECT_TRUE(runStackVM(src).find("<runtime:") != std::string::npos);
    EXPECT_TRUE(runStackVM_IR(src).find("<runtime:") != std::string::npos);
    EXPECT_TRUE(runRegVM(src).find("<runtime:") != std::string::npos);
}

TEST(EnumBoundary, WrongArity) {
    std::string src = "enum Option<T> { Some(T), None }"
                      "var s = Option.Some(1, 2);"; // Some 期望 1 参数，传 2
    // L5 fix: 四后端均通过 enumRegistry_ 校验 arity，报运行时错误。
    EXPECT_TRUE(runInterpreter(src).find("<runtime:") != std::string::npos);
    EXPECT_TRUE(runStackVM(src).find("<runtime:") != std::string::npos);
    EXPECT_TRUE(runStackVM_IR(src).find("<runtime:") != std::string::npos);
    EXPECT_TRUE(runRegVM(src).find("<runtime:") != std::string::npos);
}

TEST(EnumBoundary, MatchNoCaseMatched) {
    // L6 fix: match 无 default 且无 case 匹配 → 四后端均报运行时错误
    // StackVM 路径 (Compiler.cpp visitMatchExpr): OP_POP + OP_STRING + OP_THROW
    // IR 路径 (IR.cpp visitMatchExpr): LOAD_CONST + THROW
    // Interpreter: runtimeError("match 表达式没有匹配的 case")
    std::string src = "enum Color { Red, Green, Blue }"
                      "var c = Color.Blue;"
                      "var r = match (c) {"
                      "    case Color.Red => \"r\";"
                      "    case Color.Green => \"g\";"
                      "};"
                      "print(r);";
    EXPECT_TRUE(runInterpreter(src).find("<runtime:") != std::string::npos);
    EXPECT_TRUE(runStackVM(src).find("<runtime:") != std::string::npos);
    EXPECT_TRUE(runStackVM_IR(src).find("<runtime:") != std::string::npos);
    EXPECT_TRUE(runRegVM(src).find("<runtime:") != std::string::npos);
}

// ============================================================
// 综合场景：函数 + enum + match
// ============================================================

TEST(EnumIntegration, FunctionWithMatch) {
    std::string src = "enum Option<T> { Some(T), None }"
                      "fun unwrap(opt) {"
                      "    return match (opt) {"
                      "        case Option.Some(v) => v;"
                      "        case Option.None => -1;"
                      "    };"
                      "}"
                      "print(unwrap(Option.Some(99)));"
                      "print(\";\");"
                      "print(unwrap(Option.None));";
    std::string expected = "99;-1";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

TEST(EnumIntegration, RecursiveAdt) {
    // 简单的递归 ADT 模拟（链表）
    std::string src = "enum List<T> { Nil, Cons(T) }"
                      "var empty = List.Nil;"
                      "var one = List.Cons(1);"
                      "var r = match (one) {"
                      "    case List.Nil => \"empty\";"
                      "    case List.Cons(x) => \"elem\";"
                      "};"
                      "print(r);";
    std::string expected = "elem";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}
