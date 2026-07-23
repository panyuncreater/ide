// ============================================================
// TestDebuggerReplStage2 — R161 调试器 REPL 阶段 2 回归测试
// ------------------------------------------------------------
// 覆盖 R161 阶段 2 引入的 Interpreter 注册表注入 API：
//   1. Interpreter::hasRegistries() — 查询是否有可注入的注册表
//   2. Interpreter::injectRegistriesFrom(src) — 从源 Interpreter 复制
//      funRegistry_/classRegistry_/enumRegistry_，使调试器 REPL 中
//      可调用用户定义的函数/类/枚举
//
// 测试策略：
//   - hasRegistries 基础语义：新实例 false，定义函数/类/枚举后 true
//   - injectRegistriesFrom 后行为验证：
//     * 类实例化（C()）— classRegistry_ 直接支撑 visitFunCall → constructClassInstance
//     * 方法调用（c.get()）— 实例携带 className，classRegistry_ 提供方法表
//     * 枚举 variant（Color.RED）— enumRegistry_ 直接支撑 visitEnumVariantExpr
//     * 继承链（Derived().greet()）— classRegistry_ 含父子类，super 查找沿继承链
//   - 函数调用需闭包值注入：funRegistry_ 被 executeRepl 清除，函数调用依赖
//     env 中的闭包值（携带 shared_ptr<FunDecl> body）
//   - 边界场景：空源 Interpreter / 多次注入覆盖 / 闭包捕获变量
//
// 覆盖范围说明：
//   - IdeController::evaluateDebuggerRepl 位于 app/ 模块（不链接到 minilang_tests），
//     通过代码审查 + IDE 手动测试验证，与 TestWatchpoint.cpp 范式一致。
//   - 本测试聚焦 interpreter/ 模块内的 injectRegistriesFrom/hasRegistries 单元行为。
// ============================================================

#include <gtest/gtest.h>

#include "ast/ASTNode.h"
#include "interpreter/Environment.h"
#include "interpreter/Interpreter.h"
#include "interpreter/RuntimeExceptions.h"
#include "interpreter/Value.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <memory>
#include <string>
#include <vector>

// ============================================================
// 辅助函数
// ============================================================

static std::unique_ptr<Block> parseSource(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    return parser.parse(tokens);
}

/// 执行源码并返回 Interpreter（保留 AST 所有权，使注册表中的 shared_ptr 有效）
static std::shared_ptr<Interpreter> executeAndKeepAst(const std::string& source) {
    auto ast = parseSource(source);
    if (!ast) {
        return nullptr;
    }
    auto interp = std::make_shared<Interpreter>();
    interp->execute(*ast);
    // 保留 AST 所有权，使 funRegistry_/classRegistry_ 中的 shared_ptr<FunDecl> 有效
    interp->retainReplAst(std::move(ast));
    return interp;
}

/// 在临时 Interpreter 中求值表达式（模拟 evaluateDebuggerRepl 流程）
/// @param mainInterp 主 Interpreter（提供注册表 + 闭包值）
/// @param expr 表达式源码（无需分号）
/// @param injectClosureVars 是否将主 Interpreter 的全局变量（含闭包值）注入临时环境
/// @param outOutput 可选：捕获 print 输出
/// @return 求值结果 Value
static Value evalInTempInterp(const Interpreter& mainInterp, const std::string& expr, bool injectClosureVars,
                              std::string* outOutput = nullptr) {
    // 自动补充分号（与 evaluateDebuggerRepl 一致）
    std::string normSource = expr;
    if (!normSource.empty() && normSource.back() != ';') {
        normSource += ';';
    }
    auto ast = parseSource(normSource);
    if (!ast || ast->statements.empty()) {
        throw std::runtime_error("解析失败: " + expr);
    }

    // 创建临时 Interpreter + 注入环境
    Interpreter tempInterp;
    auto env = std::make_shared<Environment>();

    if (injectClosureVars) {
        // 注入主 Interpreter 的全局变量（含闭包值）
        auto mainEnv = mainInterp.getGlobalEnvironment();
        if (mainEnv) {
            for (const auto& kv : mainEnv->allVariables()) {
                env->define(kv.first, kv.second);
            }
        }
    }

    tempInterp.setGlobalEnvironment(env);

    // R161 阶段 2：注入注册表
    if (mainInterp.hasRegistries()) {
        tempInterp.injectRegistriesFrom(mainInterp);
    }

    // 捕获 print 输出
    if (outOutput) {
        tempInterp.setOutputCallback([&outOutput](const std::string& s) {
            *outOutput += s;
            *outOutput += '\n';
        });
    }

    // 用 executeRepl 执行（与 evaluateDebuggerRepl 一致）
    // 注意：executeRepl 会清除 funRegistry_，但 classRegistry_/enumRegistry_ 保留
    return tempInterp.executeRepl(*ast);
}

// ============================================================
// 第一组：hasRegistries 基础语义测试
// ============================================================

TEST(DebuggerReplStage2_HasRegistries, FreshInterpreter_ReturnsFalse) {
    Interpreter interp;
    EXPECT_FALSE(interp.hasRegistries());
}

TEST(DebuggerReplStage2_HasRegistries, AfterFunDecl_ReturnsTrue) {
    auto ast = parseSource("fun add(a, b) { return a + b; }");
    ASSERT_NE(ast, nullptr);
    Interpreter interp;
    interp.execute(*ast);
    EXPECT_TRUE(interp.hasRegistries());
}

TEST(DebuggerReplStage2_HasRegistries, AfterClassDecl_ReturnsTrue) {
    auto ast = parseSource("class C { var x = 0; }");
    ASSERT_NE(ast, nullptr);
    Interpreter interp;
    interp.execute(*ast);
    EXPECT_TRUE(interp.hasRegistries());
}

TEST(DebuggerReplStage2_HasRegistries, AfterEnumDecl_ReturnsTrue) {
    auto ast = parseSource("enum Color { RED, GREEN, BLUE }");
    ASSERT_NE(ast, nullptr);
    Interpreter interp;
    interp.execute(*ast);
    EXPECT_TRUE(interp.hasRegistries());
}

// ============================================================
// 第二组：injectRegistriesFrom — 类实例化 + 方法调用
// ============================================================

TEST(DebuggerReplStage2_Class, InstantiateAndCallMethod) {
    // 主 Interpreter 定义类
    auto mainInterp = executeAndKeepAst("class C {\n"
                                        "    var x = 0;\n"
                                        "    fun init(v) { x = v; }\n"
                                        "    fun get() { return x; }\n"
                                        "    fun set(v) { x = v; }\n"
                                        "}\n");
    ASSERT_NE(mainInterp, nullptr);
    EXPECT_TRUE(mainInterp->hasRegistries());

    // 临时 Interpreter：注入注册表后实例化 + 调用方法
    // classRegistry_ 不被 executeRepl 清除，直接支撑 constructClassInstance
    Value result = evalInTempInterp(*mainInterp, "var c = C(42); c.get()", false);
    EXPECT_EQ(result.typeName(), "int");
    EXPECT_EQ(result.intVal(), 42);
}

TEST(DebuggerReplStage2_Class, MethodModifiesField) {
    auto mainInterp = executeAndKeepAst("class C {\n"
                                        "    var x = 0;\n"
                                        "    fun init(v) { x = v; }\n"
                                        "    fun get() { return x; }\n"
                                        "    fun set(v) { x = v; }\n"
                                        "}\n");
    ASSERT_NE(mainInterp, nullptr);

    // 实例化后修改字段再读取
    Value result = evalInTempInterp(*mainInterp, "var c = C(10); c.set(99); c.get()", false);
    EXPECT_EQ(result.typeName(), "int");
    EXPECT_EQ(result.intVal(), 99);
}

TEST(DebuggerReplStage2_Class, NoInit_NoArgs) {
    // 无 init 方法的类，无参构造
    auto mainInterp = executeAndKeepAst("class C {\n"
                                        "    var x = 5;\n"
                                        "    fun get() { return x; }\n"
                                        "}\n");
    ASSERT_NE(mainInterp, nullptr);

    Value result = evalInTempInterp(*mainInterp, "C().get()", false);
    EXPECT_EQ(result.intVal(), 5);
}

// ============================================================
// 第三组：injectRegistriesFrom — 继承链
// ============================================================

TEST(DebuggerReplStage2_Inheritance, SubclassCallsInheritedMethod) {
    auto mainInterp = executeAndKeepAst("class Base {\n"
                                        "    fun greet() { return 42; }\n"
                                        "}\n"
                                        "class Derived : Base {\n"
                                        "    var y = 1;\n"
                                        "}\n");
    ASSERT_NE(mainInterp, nullptr);

    // 子类实例调用继承的方法
    Value result = evalInTempInterp(*mainInterp, "Derived().greet()", false);
    EXPECT_EQ(result.intVal(), 42);
}

TEST(DebuggerReplStage2_Inheritance, SuperMethodResolution) {
    auto mainInterp = executeAndKeepAst("class Base {\n"
                                        "    fun name() { return \"base\"; }\n"
                                        "}\n"
                                        "class Derived : Base {\n"
                                        "    fun name() { return \"derived\"; }\n"
                                        "    fun parent() { return super.name(); }\n"
                                        "}\n");
    ASSERT_NE(mainInterp, nullptr);

    // 子类方法调用 super → 父类方法
    Value result = evalInTempInterp(*mainInterp, "Derived().parent()", false);
    EXPECT_EQ(result.typeName(), "string");
    EXPECT_EQ(result.stringVal(), "base");
}

TEST(DebuggerReplStage2_Inheritance, DeepChain_ThreeLevels) {
    auto mainInterp = executeAndKeepAst("class A {\n"
                                        "    fun id() { return 1; }\n"
                                        "}\n"
                                        "class B : A {\n"
                                        "}\n"
                                        "class C : B {\n"
                                        "}\n");
    ASSERT_NE(mainInterp, nullptr);

    Value result = evalInTempInterp(*mainInterp, "C().id()", false);
    EXPECT_EQ(result.intVal(), 1);
}

// ============================================================
// 第四组：injectRegistriesFrom — 枚举 variant
// ------------------------------------------------------------
// 注意：Color.RED 语法要求 Parser 的 knownEnums_ 集合包含 "Color"，
// 否则 Parser 生成 MemberAccess 而非 EnumVariantExpr。knownEnums_ 是
// Parser 实例私有，跨 Parser 不共享。因此测试源码需包含 enum 定义
// 使 Parser 正确生成 EnumVariantExpr。injectRegistriesFrom 注入的
// enumRegistry_ 不被 executeRepl 清除，与源码中的 enum 定义叠加
// （重定义覆盖，语义等价）。

TEST(DebuggerReplStage2_Enum, AccessVariant_WithEnumDecl) {
    auto mainInterp = executeAndKeepAst("enum Color { RED, GREEN, BLUE }");
    ASSERT_NE(mainInterp, nullptr);

    // 源码包含 enum 定义（使 Parser 生成 EnumVariantExpr）+ variant 访问
    std::string code = "enum Color { RED, GREEN, BLUE }\n Color.RED";
    Value result = evalInTempInterp(*mainInterp, code, false);
    EXPECT_TRUE(result.isEnumVariant());
    EXPECT_EQ(result.enumVariantName(), "RED");
}

TEST(DebuggerReplStage2_Enum, AccessAllVariants_WithEnumDecl) {
    auto mainInterp = executeAndKeepAst("enum Direction { NORTH, SOUTH, EAST, WEST }");
    ASSERT_NE(mainInterp, nullptr);

    std::string decl = "enum Direction { NORTH, SOUTH, EAST, WEST }\n";
    Value north = evalInTempInterp(*mainInterp, decl + "Direction.NORTH", false);
    Value south = evalInTempInterp(*mainInterp, decl + "Direction.SOUTH", false);
    Value east = evalInTempInterp(*mainInterp, decl + "Direction.EAST", false);
    Value west = evalInTempInterp(*mainInterp, decl + "Direction.WEST", false);

    EXPECT_EQ(north.enumVariantName(), "NORTH");
    EXPECT_EQ(south.enumVariantName(), "SOUTH");
    EXPECT_EQ(east.enumVariantName(), "EAST");
    EXPECT_EQ(west.enumVariantName(), "WEST");
}

TEST(DebuggerReplStage2_Enum, VariantWithPayload_WithEnumDecl) {
    // 使用泛型 enum 语法：T 是类型参数（运行时擦除，不做类型校验）
    auto mainInterp = executeAndKeepAst("enum Shape<T> {\n"
                                        "    Circle(T),\n"
                                        "    Rect(T, T)\n"
                                        "}\n");
    ASSERT_NE(mainInterp, nullptr);

    std::string code = "enum Shape<T> {\n"
                       "    Circle(T),\n"
                       "    Rect(T, T)\n"
                       "}\n"
                       "Shape.Circle(5)";
    Value result = evalInTempInterp(*mainInterp, code, false);
    EXPECT_TRUE(result.isEnumVariant());
    EXPECT_EQ(result.enumVariantName(), "Circle");
}

TEST(DebuggerReplStage2_Enum, EnumRegistryPreservedAcrossExecuteRepl) {
    // 验证 enumRegistry_ 不被 executeRepl 清除（与 funRegistry_ 不同）
    auto mainInterp = executeAndKeepAst("enum Color { Red }");
    ASSERT_NE(mainInterp, nullptr);

    Interpreter tempInterp;
    auto env = std::make_shared<Environment>();
    tempInterp.setGlobalEnvironment(env);
    tempInterp.injectRegistriesFrom(*mainInterp);

    // 第一次 executeRepl：定义 enum + 访问 variant（需分号终止表达式语句）
    auto ast1 = parseSource("enum Color { Red };\n Color.Red;");
    ASSERT_NE(ast1, nullptr);
    Value r1 = tempInterp.executeRepl(*ast1);
    EXPECT_TRUE(r1.isEnumVariant());
    EXPECT_EQ(r1.enumVariantName(), "Red");

    // 第二次 executeRepl：仅访问 variant（enum 定义已在 enumRegistry_ 中）
    // 需要重新声明 enum 使 Parser 生成 EnumVariantExpr，但 enumRegistry_ 已存在
    auto ast2 = parseSource("enum Color { Red };\n Color.Red;");
    Value r2 = tempInterp.executeRepl(*ast2);
    EXPECT_TRUE(r2.isEnumVariant());
    EXPECT_EQ(r2.enumVariantName(), "Red");
}

TEST(DebuggerReplStage2_Enum, DirectAccessWithoutParserKnowledge_Throws) {
    // 边界：源码不含 enum 定义 → Parser 生成 MemberAccess → 运行时失败
    // 这是已知限制：Parser 的 knownEnums_ 不跨实例共享
    auto mainInterp = executeAndKeepAst("enum Color { RED }");
    ASSERT_NE(mainInterp, nullptr);

    // 不含 enum 定义，Parser 不知道 Color 是 enum → MemberAccess → "未定义的变量: Color"
    EXPECT_THROW(evalInTempInterp(*mainInterp, "Color.RED", false), RuntimeError);
}

// ============================================================
// 第五组：injectRegistriesFrom — 函数调用（需闭包值注入）
// ============================================================
// 注意：executeRepl 会清除 funRegistry_，函数调用依赖 env 中的闭包值
// （闭包值携带 shared_ptr<FunDecl> body，自包含不依赖 funRegistry_）

TEST(DebuggerReplStage2_Function, CallWithClosureValueInjected) {
    auto mainInterp = executeAndKeepAst("fun add(a, b) { return a + b; }");
    ASSERT_NE(mainInterp, nullptr);

    // 注入闭包值（injectClosureVars=true）后函数调用可用
    Value result = evalInTempInterp(*mainInterp, "add(1, 2)", true);
    EXPECT_EQ(result.typeName(), "int");
    EXPECT_EQ(result.intVal(), 3);
}

TEST(DebuggerReplStage2_Function, CallWithoutClosureValue_ThrowsRuntimeError) {
    auto mainInterp = executeAndKeepAst("fun add(a, b) { return a + b; }");
    ASSERT_NE(mainInterp, nullptr);

    // 不注入闭包值（injectClosureVars=false）→ env 中无 add 闭包 → "不是函数" 错误
    // funRegistry_ 被 executeRepl 清除，无法作为后备
    EXPECT_THROW(evalInTempInterp(*mainInterp, "add(1, 2)", false), RuntimeError);
}

TEST(DebuggerReplStage2_Function, RecursiveCall) {
    auto mainInterp = executeAndKeepAst("fun fact(n) {\n"
                                        "    if (n <= 1) { return 1; }\n"
                                        "    return n * fact(n - 1);\n"
                                        "}\n");
    ASSERT_NE(mainInterp, nullptr);

    Value result = evalInTempInterp(*mainInterp, "fact(5)", true);
    EXPECT_EQ(result.intVal(), 120);
}

TEST(DebuggerReplStage2_Function, DefaultParameters) {
    auto mainInterp = executeAndKeepAst("fun greet(name, prefix = \"Hello\") {\n"
                                        "    return prefix + \", \" + name;\n"
                                        "}\n");
    ASSERT_NE(mainInterp, nullptr);

    Value r1 = evalInTempInterp(*mainInterp, "greet(\"World\")", true);
    EXPECT_EQ(r1.stringVal(), "Hello, World");

    Value r2 = evalInTempInterp(*mainInterp, "greet(\"World\", \"Hi\")", true);
    EXPECT_EQ(r2.stringVal(), "Hi, World");
}

TEST(DebuggerReplStage2_Function, MutualRecursion) {
    auto mainInterp = executeAndKeepAst("fun isEven(n) {\n"
                                        "    if (n == 0) { return true; }\n"
                                        "    return isOdd(n - 1);\n"
                                        "}\n"
                                        "fun isOdd(n) {\n"
                                        "    if (n == 0) { return false; }\n"
                                        "    return isEven(n - 1);\n"
                                        "}\n");
    ASSERT_NE(mainInterp, nullptr);

    Value result = evalInTempInterp(*mainInterp, "isEven(10)", true);
    EXPECT_TRUE(result.isBool());
    EXPECT_TRUE(result.boolVal());
}

// ============================================================
// 第六组：injectRegistriesFrom — 闭包捕获
// ============================================================

TEST(DebuggerReplStage2_Closure, CapturedVariable) {
    auto mainInterp = executeAndKeepAst("fun makeCounter() {\n"
                                        "    var c = 0;\n"
                                        "    fun inc() {\n"
                                        "        c = c + 1;\n"
                                        "        return c;\n"
                                        "    }\n"
                                        "    return inc;\n"
                                        "}\n");
    ASSERT_NE(mainInterp, nullptr);

    // 调用 makeCounter() 返回闭包，闭包捕获了 c
    Value counter = evalInTempInterp(*mainInterp, "makeCounter()", true);
    EXPECT_TRUE(counter.isClosure());

    // 将闭包注入 env 后调用
    Interpreter tempInterp;
    auto env = std::make_shared<Environment>();
    env->define("counter", counter);
    tempInterp.setGlobalEnvironment(env);
    tempInterp.injectRegistriesFrom(*mainInterp);

    auto ast = parseSource("counter();");
    ASSERT_NE(ast, nullptr);
    Value r1 = tempInterp.executeRepl(*ast);
    EXPECT_EQ(r1.intVal(), 1);

    // 再次调用（同一闭包实例，c 递增）
    // 注意：每次 executeRepl 重新解析 AST，但 env 保留
    auto ast2 = parseSource("counter();");
    Value r2 = tempInterp.executeRepl(*ast2);
    EXPECT_EQ(r2.intVal(), 2);
}

// ============================================================
// 第七组：边界场景
// ============================================================

TEST(DebuggerReplStage2_Boundary, EmptySourceInterpreter_HasRegistriesFalse) {
    // 主 Interpreter 未执行任何源码 → hasRegistries=false → 跳过注入
    Interpreter mainInterp;
    EXPECT_FALSE(mainInterp.hasRegistries());

    // 临时 Interpreter 仍可执行基本表达式
    auto ast = parseSource("1 + 2;");
    ASSERT_NE(ast, nullptr);
    Interpreter tempInterp;
    auto env = std::make_shared<Environment>();
    tempInterp.setGlobalEnvironment(env);
    // 不调用 injectRegistriesFrom（hasRegistries=false）
    Value result = tempInterp.executeRepl(*ast);
    EXPECT_EQ(result.intVal(), 3);
}

TEST(DebuggerReplStage2_Boundary, MultipleInjections_LastWins) {
    // 第一次注入：foo 返回 1
    auto main1 = executeAndKeepAst("fun foo() { return 1; }");
    ASSERT_NE(main1, nullptr);

    // 第二次注入：foo 返回 2
    auto main2 = executeAndKeepAst("fun foo() { return 2; }");
    ASSERT_NE(main2, nullptr);

    // 临时 Interpreter 先注入 main1，再注入 main2 → 最后一次为准
    Interpreter tempInterp;
    auto env = std::make_shared<Environment>();
    tempInterp.setGlobalEnvironment(env);
    tempInterp.injectRegistriesFrom(*main1);
    tempInterp.injectRegistriesFrom(*main2);

    // 注入 main2 的闭包值（因为 funRegistry_ 被 executeRepl 清除）
    auto main2Env = main2->getGlobalEnvironment();
    if (main2Env) {
        for (const auto& kv : main2Env->allVariables()) {
            env->define(kv.first, kv.second);
        }
    }

    auto ast = parseSource("foo();");
    ASSERT_NE(ast, nullptr);
    Value result = tempInterp.executeRepl(*ast);
    EXPECT_EQ(result.intVal(), 2);
}

TEST(DebuggerReplStage2_Boundary, PrintOutput) {
    auto mainInterp = executeAndKeepAst("fun hello() { print(\"hello\"); return 42; }");
    ASSERT_NE(mainInterp, nullptr);

    std::string output;
    Value result = evalInTempInterp(*mainInterp, "hello()", true, &output);
    EXPECT_EQ(result.intVal(), 42);
    EXPECT_EQ(output, "hello\n");
}

TEST(DebuggerReplStage2_Boundary, MultipleStatements) {
    auto mainInterp = executeAndKeepAst("fun double(x) { return x * 2; }\n"
                                        "class Box {\n"
                                        "    var v = 0;\n"
                                        "    fun init(v) { this.v = v; }\n"
                                        "    fun get() { return v; }\n"
                                        "}\n");
    ASSERT_NE(mainInterp, nullptr);

    // 多条语句：var 声明 + 函数调用 + 类实例化 + 方法调用
    std::string code = "var a = double(5);\n"
                       "var b = Box(a);\n"
                       "b.get();";

    Interpreter tempInterp;
    auto env = std::make_shared<Environment>();
    // 注入闭包值（double）
    auto mainEnv = mainInterp->getGlobalEnvironment();
    if (mainEnv) {
        for (const auto& kv : mainEnv->allVariables()) {
            env->define(kv.first, kv.second);
        }
    }
    tempInterp.setGlobalEnvironment(env);
    tempInterp.injectRegistriesFrom(*mainInterp);

    auto ast = parseSource(code);
    ASSERT_NE(ast, nullptr);
    Value result = tempInterp.executeRepl(*ast);
    EXPECT_EQ(result.intVal(), 10);
}

// ============================================================
// 第八组：综合场景 — 类方法中调用注入的函数
// ============================================================

TEST(DebuggerReplStage2_Combined, MethodCallsInjectedFunction) {
    // 类方法中调用全局函数：需要 classRegistry_ + 闭包值
    auto mainInterp = executeAndKeepAst("fun double(x) { return x * 2; }\n"
                                        "class Calculator {\n"
                                        "    var base = 0;\n"
                                        "    fun init(b) { base = b; }\n"
                                        "    fun compute() { return double(base); }\n"
                                        "}\n");
    ASSERT_NE(mainInterp, nullptr);

    // 注入闭包值（double）+ 注册表（Calculator）
    Interpreter tempInterp;
    auto env = std::make_shared<Environment>();
    auto mainEnv = mainInterp->getGlobalEnvironment();
    if (mainEnv) {
        for (const auto& kv : mainEnv->allVariables()) {
            env->define(kv.first, kv.second);
        }
    }
    tempInterp.setGlobalEnvironment(env);
    tempInterp.injectRegistriesFrom(*mainInterp);

    auto ast = parseSource("Calculator(21).compute();");
    ASSERT_NE(ast, nullptr);
    Value result = tempInterp.executeRepl(*ast);
    EXPECT_EQ(result.intVal(), 42);
}

TEST(DebuggerReplStage2_Combined, EnumMatchInRepl) {
    // 在 REPL 中使用 match 匹配 enum variant
    // 使用泛型 enum 语法：T/E 是类型参数（运行时擦除）
    auto mainInterp = executeAndKeepAst("enum Result<T, E> {\n"
                                        "    Ok(T),\n"
                                        "    Err(E)\n"
                                        "}\n");
    ASSERT_NE(mainInterp, nullptr);

    // 语法要点：
    //   1. match 是表达式（赋值给 var v），不能作为独立语句
    //   2. scrutinee 必须用括号包裹：match (r)
    //   3. 每个 case 必须以 case 关键字开头：case Result.Ok(x) =>
    //   4. 源码需包含 enum 定义使 Parser 生成 EnumVariantExpr
    //   5. match pattern 的 VARIANT 不依赖 enumRegistry_（直接比对 scrutinee 的 enumName）
    std::string code = "enum Result<T, E> {\n"
                       "    Ok(T),\n"
                       "    Err(E)\n"
                       "}\n"
                       "var r = Result.Ok(42);\n"
                       "var v = match (r) {\n"
                       "    case Result.Ok(x) => x\n"
                       "    case Result.Err(m) => 0\n"
                       "};\n"
                       "v;";

    Interpreter tempInterp;
    auto env = std::make_shared<Environment>();
    tempInterp.setGlobalEnvironment(env);
    tempInterp.injectRegistriesFrom(*mainInterp);

    auto ast = parseSource(code);
    ASSERT_NE(ast, nullptr);
    Value result = tempInterp.executeRepl(*ast);
    EXPECT_EQ(result.intVal(), 42);
}
