// ============================================================
// R98 W2: 内建高阶函数 map/filter/reduce/forEach/find 测试套件
// ------------------------------------------------------------
// 验证 5 个内建高阶函数在三后端 + IR 路径（共 4 路径）上的语义一致性：
//   - Interpreter（树遍历解释器）
//   - StackVM（栈式字节码 VM 直接路径）
//   - StackVM_IR（栈式 VM 经 IR 路径）
//   - RegisterVM（寄存器式 VM）
//
// 覆盖点：
//   1. map：基本映射、空数组、单元素、字符串元素
//   2. filter：基本过滤、全保留、全过滤、空数组
//   3. reduce：基本归约、单元素、空数组（返回 initial）、字符串拼接
//   4. forEach：基本遍历、空数组、副作用累积
//   5. find：查找命中、查找未命中（返回 null）、空数组
//   6. 闭包捕获 upvalue：高阶函数中的闭包访问外层变量
//   7. 错误处理：非数组参数、非函数参数、参数数量错误
//   8. 用户覆盖：用户定义同名函数 map/filter 优先于内置
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

/// 四后端一致性验证宏（减少重复代码）
#define EXPECT_FOUR_BACKENDS(src, expected)                                                                            \
    do {                                                                                                               \
        EXPECT_EQ(runInterpreter(src), expected) << "Interpreter failed";                                              \
        EXPECT_EQ(runStackVM(src), expected) << "StackVM failed";                                                      \
        EXPECT_EQ(runStackVM_IR(src), expected) << "StackVM_IR failed";                                                \
        EXPECT_EQ(runRegVM(src), expected) << "RegisterVM failed";                                                     \
    } while (0)

// ============================================================
// map 测试
// ============================================================

TEST(HigherOrderMap, BasicSquare) {
    std::string src = "fun double(x) { return x * 2; }"
                      "var arr = [1, 2, 3, 4, 5];"
                      "var result = map(arr, double);"
                      "print(result);";
    EXPECT_FOUR_BACKENDS(src, "[2, 4, 6, 8, 10]");
}

TEST(HigherOrderMap, EmptyArray) {
    std::string src = "fun double(x) { return x * 2; }"
                      "var arr = [];"
                      "var result = map(arr, double);"
                      "print(result);";
    EXPECT_FOUR_BACKENDS(src, "[]");
}

TEST(HigherOrderMap, SingleElement) {
    std::string src = "fun inc(x) { return x + 1; }"
                      "var arr = [41];"
                      "var result = map(arr, inc);"
                      "print(result);";
    EXPECT_FOUR_BACKENDS(src, "[42]");
}

TEST(HigherOrderMap, StringElements) {
    std::string src = "fun upper(s) { return s.upper(); }"
                      "var arr = [\"abc\", \"de\", \"fghi\"];"
                      "var result = map(arr, upper);"
                      "print(result);";
    // 字符串数组格式化时元素带引号（与 Value::toString 行为一致）
    EXPECT_FOUR_BACKENDS(src, "[\"ABC\", \"DE\", \"FGHI\"]");
}

TEST(HigherOrderMap, CapturesUpvalue) {
    // 闭包捕获外层变量 multiplier
    std::string src = "fun main() {"
                      "  var multiplier = 10;"
                      "  fun scale(x) { return x * multiplier; }"
                      "  var arr = [1, 2, 3];"
                      "  var result = map(arr, scale);"
                      "  print(result);"
                      "}"
                      "main();";
    EXPECT_FOUR_BACKENDS(src, "[10, 20, 30]");
}

TEST(HigherOrderMap, NestedFunctionAsArg) {
    // 嵌套函数直接作为参数（W1 fix 路径与 W2 协同）
    std::string src = "fun main() {"
                      "  fun square(x) { return x * x; }"
                      "  var arr = [1, 2, 3, 4];"
                      "  var result = map(arr, square);"
                      "  print(result);"
                      "}"
                      "main();";
    EXPECT_FOUR_BACKENDS(src, "[1, 4, 9, 16]");
}

// ============================================================
// filter 测试
// ============================================================

TEST(HigherOrderFilter, EvenNumbers) {
    std::string src = "fun isEven(x) { return x % 2 == 0; }"
                      "var arr = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10];"
                      "var result = filter(arr, isEven);"
                      "print(result);";
    EXPECT_FOUR_BACKENDS(src, "[2, 4, 6, 8, 10]");
}

TEST(HigherOrderFilter, EmptyArray) {
    std::string src = "fun isEven(x) { return x % 2 == 0; }"
                      "var arr = [];"
                      "var result = filter(arr, isEven);"
                      "print(result);";
    EXPECT_FOUR_BACKENDS(src, "[]");
}

TEST(HigherOrderFilter, AllKept) {
    std::string src = "fun isPositive(x) { return x > 0; }"
                      "var arr = [1, 2, 3];"
                      "var result = filter(arr, isPositive);"
                      "print(result);";
    EXPECT_FOUR_BACKENDS(src, "[1, 2, 3]");
}

TEST(HigherOrderFilter, AllFiltered) {
    std::string src = "fun isNegative(x) { return x < 0; }"
                      "var arr = [1, 2, 3];"
                      "var result = filter(arr, isNegative);"
                      "print(result);";
    EXPECT_FOUR_BACKENDS(src, "[]");
}

TEST(HigherOrderFilter, CapturesUpvalue) {
    std::string src = "fun main() {"
                      "  var threshold = 5;"
                      "  fun aboveThreshold(x) { return x > threshold; }"
                      "  var arr = [1, 6, 3, 8, 5, 10];"
                      "  var result = filter(arr, aboveThreshold);"
                      "  print(result);"
                      "}"
                      "main();";
    EXPECT_FOUR_BACKENDS(src, "[6, 8, 10]");
}

// ============================================================
// reduce 测试
// ============================================================

TEST(HigherOrderReduce, Sum) {
    std::string src = "fun add(a, b) { return a + b; }"
                      "var arr = [1, 2, 3, 4, 5];"
                      "var result = reduce(arr, add, 0);"
                      "print(result);";
    EXPECT_FOUR_BACKENDS(src, "15");
}

TEST(HigherOrderReduce, Product) {
    std::string src = "fun mul(a, b) { return a * b; }"
                      "var arr = [1, 2, 3, 4, 5];"
                      "var result = reduce(arr, mul, 1);"
                      "print(result);";
    EXPECT_FOUR_BACKENDS(src, "120");
}

TEST(HigherOrderReduce, EmptyArrayReturnsInitial) {
    std::string src = "fun add(a, b) { return a + b; }"
                      "var arr = [];"
                      "var result = reduce(arr, add, 42);"
                      "print(result);";
    EXPECT_FOUR_BACKENDS(src, "42");
}

TEST(HigherOrderReduce, SingleElement) {
    std::string src = "fun add(a, b) { return a + b; }"
                      "var arr = [10];"
                      "var result = reduce(arr, add, 5);"
                      "print(result);";
    EXPECT_FOUR_BACKENDS(src, "15");
}

TEST(HigherOrderReduce, StringConcat) {
    std::string src = "fun concat(a, b) { return a + b; }"
                      "var arr = [\"Hello\", \", \", \"World\", \"!\"];"
                      "var result = reduce(arr, concat, \"\");"
                      "print(result);";
    EXPECT_FOUR_BACKENDS(src, "Hello, World!");
}

TEST(HigherOrderReduce, CapturesUpvalue) {
    std::string src = "fun main() {"
                      "  var offset = 100;"
                      "  fun addOffset(a, b) { return a + b + offset; }"
                      "  var arr = [1, 2, 3];"
                      "  var result = reduce(arr, addOffset, 0);"
                      "  print(result);"
                      "}"
                      "main();";
    // 0 + 1 + 100 + 2 + 100 + 3 + 100 = 306
    EXPECT_FOUR_BACKENDS(src, "306");
}

// ============================================================
// forEach 测试
// ============================================================

TEST(HigherOrderForEach, BasicPrint) {
    std::string src = "fun printNum(x) { print(x); }"
                      "var arr = [1, 2, 3];"
                      "forEach(arr, printNum);";
    EXPECT_FOUR_BACKENDS(src, "123");
}

TEST(HigherOrderForEach, EmptyArray) {
    std::string src = "fun printNum(x) { print(x); }"
                      "var arr = [];"
                      "forEach(arr, printNum);"
                      "print(\"done\");";
    EXPECT_FOUR_BACKENDS(src, "done");
}

TEST(HigherOrderForEach, ReturnsNull) {
    std::string src = "fun printNum(x) { print(x); }"
                      "var arr = [1, 2];"
                      "var result = forEach(arr, printNum);"
                      "print(result);";
    // 输出顺序：printNum(1)→"1", printNum(2)→"2", print(result)→"null"
    EXPECT_FOUR_BACKENDS(src, "12null");
}

TEST(HigherOrderForEach, SideEffectAccumulator) {
    // forEach 通过闭包捕获的外层局部变量累积副作用（upvalue 变异）
    std::string src = "fun main() {"
                      "  var total = 0;"
                      "  fun addToTotal(x) { total = total + x; }"
                      "  var arr = [1, 2, 3, 4, 5];"
                      "  forEach(arr, addToTotal);"
                      "  print(total);"
                      "}"
                      "main();";
    EXPECT_FOUR_BACKENDS(src, "15");
}

// ============================================================
// find 测试
// ============================================================

TEST(HigherOrderFind, FoundFirst) {
    std::string src = "fun isEven(x) { return x % 2 == 0; }"
                      "var arr = [1, 3, 5, 4, 7, 8];"
                      "var result = find(arr, isEven);"
                      "print(result);";
    EXPECT_FOUR_BACKENDS(src, "4");
}

TEST(HigherOrderFind, NotFoundReturnsNull) {
    std::string src = "fun isNegative(x) { return x < 0; }"
                      "var arr = [1, 2, 3];"
                      "var result = find(arr, isNegative);"
                      "print(result);";
    EXPECT_FOUR_BACKENDS(src, "null");
}

TEST(HigherOrderFind, EmptyArray) {
    std::string src = "fun isEven(x) { return x % 2 == 0; }"
                      "var arr = [];"
                      "var result = find(arr, isEven);"
                      "print(result);";
    EXPECT_FOUR_BACKENDS(src, "null");
}

TEST(HigherOrderFind, FoundAtEnd) {
    std::string src = "fun isLarge(x) { return x > 100; }"
                      "var arr = [1, 50, 100, 150, 200];"
                      "var result = find(arr, isLarge);"
                      "print(result);";
    EXPECT_FOUR_BACKENDS(src, "150");
}

TEST(HigherOrderFind, CapturesUpvalue) {
    std::string src = "fun main() {"
                      "  var target = 7;"
                      "  fun isTarget(x) { return x == target; }"
                      "  var arr = [1, 3, 5, 7, 9];"
                      "  var result = find(arr, isTarget);"
                      "  print(result);"
                      "}"
                      "main();";
    EXPECT_FOUR_BACKENDS(src, "7");
}

// ============================================================
// 错误处理测试
// ============================================================

TEST(HigherOrderError, MapNonArrayArg) {
    std::string src = "fun double(x) { return x * 2; }"
                      "var result = map(42, double);"
                      "print(result);";
    // 四后端均应报"map 第 1 个参数必须是数组"错误
    std::string expectedErr = "<runtime:map 第 1 个参数必须是数组>";
    EXPECT_EQ(runInterpreter(src), expectedErr);
    EXPECT_EQ(runStackVM(src), expectedErr);
    EXPECT_EQ(runStackVM_IR(src), expectedErr);
    EXPECT_EQ(runRegVM(src), expectedErr);
}

TEST(HigherOrderError, MapNonFunctionArg) {
    std::string src = "var arr = [1, 2, 3];"
                      "var result = map(arr, 42);"
                      "print(result);";
    std::string expectedErr = "<runtime:map 第 2 个参数必须是函数>";
    EXPECT_EQ(runInterpreter(src), expectedErr);
    EXPECT_EQ(runStackVM(src), expectedErr);
    EXPECT_EQ(runStackVM_IR(src), expectedErr);
    EXPECT_EQ(runRegVM(src), expectedErr);
}

TEST(HigherOrderError, MapWrongArgCount) {
    std::string src = "fun double(x) { return x * 2; }"
                      "var arr = [1, 2, 3];"
                      "var result = map(arr);"
                      "print(result);";
    // 参数数量错误：Interpreter 走 visitFunCall → callHigherOrderBuiltin 会用节点参数数检查
    // 各后端错误消息可能略有不同（Interpreter 由 callHigherOrderBuiltin 检查 argCount，
    // VM/RegisterVM 由拦截点检查 argCount），这里只验证"报错"语义
    EXPECT_NE(runInterpreter(src).find("<runtime"), std::string::npos);
    EXPECT_NE(runStackVM(src).find("<runtime"), std::string::npos);
    EXPECT_NE(runStackVM_IR(src).find("<runtime"), std::string::npos);
    EXPECT_NE(runRegVM(src).find("<runtime"), std::string::npos);
}

TEST(HigherOrderError, FilterNonArrayArg) {
    std::string src = "fun isEven(x) { return x % 2 == 0; }"
                      "var result = filter(\"hello\", isEven);"
                      "print(result);";
    std::string expectedErr = "<runtime:filter 第 1 个参数必须是数组>";
    EXPECT_EQ(runInterpreter(src), expectedErr);
    EXPECT_EQ(runStackVM(src), expectedErr);
    EXPECT_EQ(runStackVM_IR(src), expectedErr);
    EXPECT_EQ(runRegVM(src), expectedErr);
}

TEST(HigherOrderError, ReduceNonArrayArg) {
    std::string src = "fun add(a, b) { return a + b; }"
                      "var result = reduce(42, add, 0);"
                      "print(result);";
    std::string expectedErr = "<runtime:reduce 第 1 个参数必须是数组>";
    EXPECT_EQ(runInterpreter(src), expectedErr);
    EXPECT_EQ(runStackVM(src), expectedErr);
    EXPECT_EQ(runStackVM_IR(src), expectedErr);
    EXPECT_EQ(runRegVM(src), expectedErr);
}

TEST(HigherOrderError, FindNonFunctionArg) {
    std::string src = "var arr = [1, 2, 3];"
                      "var result = find(arr, \"hello\");"
                      "print(result);";
    std::string expectedErr = "<runtime:find 第 2 个参数必须是函数>";
    EXPECT_EQ(runInterpreter(src), expectedErr);
    EXPECT_EQ(runStackVM(src), expectedErr);
    EXPECT_EQ(runStackVM_IR(src), expectedErr);
    EXPECT_EQ(runRegVM(src), expectedErr);
}

// ============================================================
// 用户覆盖测试：用户定义同名函数优先于内置
// ============================================================

TEST(HigherOrderUserOverride, UserMapOverridesBuiltin) {
    // 用户定义 map 函数应优先于内置高阶函数 map
    // （对齐 isBuiltinFunction 的"用户优先"语义）
    std::string src = "fun map(arr, fn) { return \"user-map\"; }"
                      "var arr = [1, 2, 3];"
                      "fun double(x) { return x * 2; }"
                      "var result = map(arr, double);"
                      "print(result);";
    EXPECT_FOUR_BACKENDS(src, "user-map");
}

TEST(HigherOrderUserOverride, UserFilterOverridesBuiltin) {
    std::string src = "fun filter(arr, fn) { return \"user-filter\"; }"
                      "var arr = [1, 2, 3];"
                      "fun isEven(x) { return x % 2 == 0; }"
                      "var result = filter(arr, isEven);"
                      "print(result);";
    EXPECT_FOUR_BACKENDS(src, "user-filter");
}

// ============================================================
// 综合场景：高阶函数组合使用
// ============================================================

TEST(HigherOrderComposition, MapThenFilter) {
    std::string src = "fun double(x) { return x * 2; }"
                      "fun isLarge(x) { return x > 5; }"
                      "var arr = [1, 2, 3, 4, 5];"
                      "var doubled = map(arr, double);"
                      "var result = filter(doubled, isLarge);"
                      "print(result);";
    // doubled = [2, 4, 6, 8, 10]，filter > 5 = [6, 8, 10]
    EXPECT_FOUR_BACKENDS(src, "[6, 8, 10]");
}

TEST(HigherOrderComposition, MapThenReduce) {
    std::string src = "fun double(x) { return x * 2; }"
                      "fun add(a, b) { return a + b; }"
                      "var arr = [1, 2, 3, 4, 5];"
                      "var doubled = map(arr, double);"
                      "var result = reduce(doubled, add, 0);"
                      "print(result);";
    // doubled = [2, 4, 6, 8, 10]，sum = 30
    EXPECT_FOUR_BACKENDS(src, "30");
}

TEST(HigherOrderComposition, FilterThenFind) {
    std::string src = "fun isEven(x) { return x % 2 == 0; }"
                      "fun isLarge(x) { return x > 5; }"
                      "var arr = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10];"
                      "var evens = filter(arr, isEven);"
                      "var result = find(evens, isLarge);"
                      "print(result);";
    // evens = [2, 4, 6, 8, 10]，find > 5 = 6
    EXPECT_FOUR_BACKENDS(src, "6");
}
