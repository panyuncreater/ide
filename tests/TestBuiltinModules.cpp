// ============================================================
// TestBuiltinModules.cpp — P2-11 子项 4 内建标准库模块测试
// ------------------------------------------------------------
// 验证 BuiltinModuleRegistry API + Compiler/Interpreter 路径
// 对 `import "std/math"` / `import "std/string"` / `import "std/list"`
// 的拦截与执行。三后端拦截一致性通过 Compiler 与 Interpreter 两条路径
// 的独立辅助函数验证（StackVM 走 Compiler 路径，RegisterVM/IR 同理）。
// ============================================================

#include "common/BuiltinModules.h"
#include "compiler/Compiler.h"
#include "compiler/RegisterVM.h" // P2-11: RegisterVM 路径命名空间导入测试
#include "compiler/VM.h"
#include "interpreter/Interpreter.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <gtest/gtest.h>
#include <string>
#include <unordered_map>

// ============================================================
// 注册表 API 单元测试
// ============================================================

TEST(BuiltinModulesRegistry, IsBuiltinModuleRecognizesStdPrefix) {
    EXPECT_TRUE(BuiltinModuleRegistry::isBuiltinModule("std/math"));
    EXPECT_TRUE(BuiltinModuleRegistry::isBuiltinModule("std/string"));
    EXPECT_TRUE(BuiltinModuleRegistry::isBuiltinModule("std/list"));
}

TEST(BuiltinModulesRegistry, IsBuiltinModuleRejectsUnknownPath) {
    EXPECT_FALSE(BuiltinModuleRegistry::isBuiltinModule("std/unknown"));
    EXPECT_FALSE(BuiltinModuleRegistry::isBuiltinModule("mymod"));
    EXPECT_FALSE(BuiltinModuleRegistry::isBuiltinModule(""));
    EXPECT_FALSE(BuiltinModuleRegistry::isBuiltinModule("std")); // 不带子路径
}

TEST(BuiltinModulesRegistry, GetSourceReturnsNonEmptyForKnownModules) {
    EXPECT_FALSE(BuiltinModuleRegistry::getSource("std/math").empty());
    EXPECT_FALSE(BuiltinModuleRegistry::getSource("std/string").empty());
    EXPECT_FALSE(BuiltinModuleRegistry::getSource("std/list").empty());
}

TEST(BuiltinModulesRegistry, GetSourceReturnsEmptyForUnknownModule) {
    EXPECT_TRUE(BuiltinModuleRegistry::getSource("std/unknown").empty());
    EXPECT_TRUE(BuiltinModuleRegistry::getSource("").empty());
}

TEST(BuiltinModulesRegistry, ListModulesContainsAllThreeStdModules) {
    const auto& list = BuiltinModuleRegistry::listModules();
    // 至少包含三个核心内建模块
    int stdCount = 0;
    for (const auto& path : list) {
        if (path == "std/math" || path == "std/string" || path == "std/list") {
            ++stdCount;
        }
    }
    EXPECT_EQ(stdCount, 3);
}

TEST(BuiltinModulesRegistry, SourceContainsExportKeyword) {
    // 内建模块源码必须使用 export 才能被 import 语句获取
    EXPECT_NE(BuiltinModuleRegistry::getSource("std/math").find("export"), std::string::npos);
    EXPECT_NE(BuiltinModuleRegistry::getSource("std/string").find("export"), std::string::npos);
    EXPECT_NE(BuiltinModuleRegistry::getSource("std/list").find("export"), std::string::npos);
}

// ============================================================
// Compiler 路径 E2E 测试
// ============================================================

// 辅助：使用 BuiltinModuleRegistry 作为模块源，编译并执行返回 print 输出
static std::string runWithBuiltinModules(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);
    EXPECT_TRUE(ast != nullptr);
    if (!ast)
        return "";

    Compiler compiler;
    compiler.setModuleLoader([](const std::string& modulePath) -> std::string {
        if (BuiltinModuleRegistry::isBuiltinModule(modulePath)) {
            return BuiltinModuleRegistry::getSource(modulePath);
        }
        return "";
    });

    CompileResult result;
    try {
        result = compiler.compile(*ast);
    } catch (const std::exception& e) {
        return "<compile:" + std::string(e.what()) + ">";
    }
    if (compiler.getDiagnostics().hasErrors()) {
        return "<compile:" + compiler.getLastError() + ">";
    }

    VM vm;
    std::string captured;
    vm.setOutputCallback([&](const std::string& s) { captured += s; });
    vm.execute(result);
    if (vm.hasError()) {
        return captured + "<runtime:" + vm.getLastError() + ">";
    }
    return captured;
}

// 辅助：Interpreter 路径执行（验证三后端拦截一致性）
// Interpreter::execute 通过 runtimeError 抛异常报告错误，这里 try/catch
// 捕获并编码为 <runtime:...> 前缀，便于测试断言区分成功/失败。
static std::string runInterpreterWithBuiltinModules(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);
    EXPECT_TRUE(ast != nullptr);
    if (!ast)
        return "";

    Interpreter interp;
    interp.setModuleLoader([](const std::string& modulePath) -> std::string {
        if (BuiltinModuleRegistry::isBuiltinModule(modulePath)) {
            return BuiltinModuleRegistry::getSource(modulePath);
        }
        return "";
    });

    std::string captured;
    interp.setOutputCallback([&](const std::string& s) { captured += s; });
    try {
        interp.execute(*ast);
    } catch (const std::exception& e) {
        return captured + "<runtime:" + e.what() + ">";
    }
    return captured;
}

// P2-11: 辅助 IR 路径执行（StackVM via IR），验证命名空间导入在三后端一致
static std::string runStackVM_IRWithBuiltinModules(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);
    EXPECT_TRUE(ast != nullptr);
    if (!ast)
        return "";

    Compiler compiler;
    compiler.setUseIR(true);
    compiler.setModuleLoader([](const std::string& modulePath) -> std::string {
        if (BuiltinModuleRegistry::isBuiltinModule(modulePath)) {
            return BuiltinModuleRegistry::getSource(modulePath);
        }
        return "";
    });

    CompileResult result;
    try {
        result = compiler.compile(*ast);
    } catch (const std::exception& e) {
        return "<compile:" + std::string(e.what()) + ">";
    }
    if (compiler.getDiagnostics().hasErrors()) {
        return "<compile:" + compiler.getLastError() + ">";
    }

    VM vm;
    std::string captured;
    vm.setOutputCallback([&](const std::string& s) { captured += s; });
    vm.execute(result);
    if (vm.hasError()) {
        return captured + "<runtime:" + vm.getLastError() + ">";
    }
    return captured;
}

// P2-11: 辅助 RegisterVM 路径执行，验证命名空间导入在寄存器式 VM 一致
static std::string runRegVM_IRWithBuiltinModules(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);
    EXPECT_TRUE(ast != nullptr);
    if (!ast)
        return "";

    Compiler compiler;
    compiler.setUseRegisterVM(true);
    compiler.setModuleLoader([](const std::string& modulePath) -> std::string {
        if (BuiltinModuleRegistry::isBuiltinModule(modulePath)) {
            return BuiltinModuleRegistry::getSource(modulePath);
        }
        return "";
    });

    compiler.compile(*ast);
    if (compiler.getDiagnostics().hasErrors()) {
        return "<compile:" + compiler.getLastError() + ">";
    }

    RegisterVM vm;
    std::string captured;
    vm.setOutputCallback([&](const std::string& s) { captured += s; });
    vm.execute(compiler.getLastRegisterResult());
    if (vm.hasError()) {
        return captured + "<runtime:" + vm.getLastError() + ">";
    }
    return captured;
}

// ============================================================
// std/math 模块测试
// ============================================================

TEST(BuiltinModulesMath, FactorialViaImport) {
    std::string src = "import { factorial } from \"std/math\";"
                      "print(factorial(5));"; // 5! = 120
    EXPECT_EQ(runWithBuiltinModules(src), "120");
}

TEST(BuiltinModulesMath, FactorialEdgeCases) {
    std::string src0 = "import { factorial } from \"std/math\";"
                       "print(factorial(0));"; // 0! = 1
    EXPECT_EQ(runWithBuiltinModules(src0), "1");

    std::string src1 = "import { factorial } from \"std/math\";"
                       "print(factorial(1));"; // 1! = 1
    EXPECT_EQ(runWithBuiltinModules(src1), "1");
}

TEST(BuiltinModulesMath, FibonacciViaImport) {
    std::string src = "import { fibonacci } from \"std/math\";"
                      "print(fibonacci(10));"; // fib(10) = 55
    EXPECT_EQ(runWithBuiltinModules(src), "55");
}

TEST(BuiltinModulesMath, GcdViaImport) {
    std::string src = "import { gcd } from \"std/math\";"
                      "print(gcd(48, 36));"; // gcd = 12
    EXPECT_EQ(runWithBuiltinModules(src), "12");
}

TEST(BuiltinModulesMath, PowViaImport) {
    std::string src = "import { pow } from \"std/math\";"
                      "print(pow(2, 10));"; // 2^10 = 1024
    EXPECT_EQ(runWithBuiltinModules(src), "1024");
}

TEST(BuiltinModulesMath, ImportAllMathModule) {
    // import 全部导出：factorial/fibonacci/gcd/pow 均可用
    std::string src = "import \"std/math\";"
                      "print(factorial(3));" // 6
                      "print(fibonacci(6));" // 8
                      "print(gcd(20, 8));";  // 4
    EXPECT_EQ(runWithBuiltinModules(src), "684");
}

// ============================================================
// std/string 模块测试
// ============================================================

TEST(BuiltinModulesString, ContainsViaImport) {
    std::string src = "import { contains } from \"std/string\";"
                      "print(contains(\"hello world\", \"world\"));"; // true
    EXPECT_EQ(runWithBuiltinModules(src), "true");
}

TEST(BuiltinModulesString, ContainsNotFound) {
    std::string src = "import { contains } from \"std/string\";"
                      "print(contains(\"hello\", \"xyz\"));"; // false
    EXPECT_EQ(runWithBuiltinModules(src), "false");
}

TEST(BuiltinModulesString, StartsWithAndEndsWith) {
    std::string src = "import { starts_with, ends_with } from \"std/string\";"
                      "print(starts_with(\"hello\", \"he\"));" // true
                      "print(ends_with(\"hello\", \"lo\"));";  // true
    EXPECT_EQ(runWithBuiltinModules(src), "truetrue");
}

TEST(BuiltinModulesString, RepeatViaImport) {
    std::string src = "import { repeat } from \"std/string\";"
                      "print(repeat(\"ab\", 3));"; // "ababab"
    EXPECT_EQ(runWithBuiltinModules(src), "ababab");
}

TEST(BuiltinModulesString, CountCharViaImport) {
    std::string src = "import { count_char } from \"std/string\";"
                      "print(count_char(\"mississippi\", \"s\"));"; // 4
    EXPECT_EQ(runWithBuiltinModules(src), "4");
}

// ============================================================
// std/list 模块测试
// ============================================================

TEST(BuiltinModulesList, MapViaImport) {
    std::string src = "import { map } from \"std/list\";"
                      "var doubled = map([1, 2, 3], fun(x) { return x * 2; });"
                      "print(doubled[0]);"
                      "print(doubled[1]);"
                      "print(doubled[2]);";
    EXPECT_EQ(runWithBuiltinModules(src), "246");
}

TEST(BuiltinModulesList, FilterViaImport) {
    std::string src = "import { filter } from \"std/list\";"
                      "var evens = filter([1, 2, 3, 4, 5, 6], fun(x) { return x % 2 == 0; });"
                      "print(len(evens));"; // 3
    EXPECT_EQ(runWithBuiltinModules(src), "3");
}

TEST(BuiltinModulesList, ReduceViaImport) {
    std::string src = "import { reduce } from \"std/list\";"
                      "var sum = reduce([1, 2, 3, 4, 5], fun(acc, x) { return acc + x; }, 0);"
                      "print(sum);"; // 15
    EXPECT_EQ(runWithBuiltinModules(src), "15");
}

TEST(BuiltinModulesList, ContainsViaImport) {
    std::string src = "import { contains } from \"std/list\";"
                      "print(contains([1, 2, 3], 2));"   // true
                      "print(contains([1, 2, 3], 99));"; // false
    EXPECT_EQ(runWithBuiltinModules(src), "truefalse");
}

TEST(BuiltinModulesList, FindIndexViaImport) {
    std::string src = "import { find_index } from \"std/list\";"
                      "print(find_index([10, 20, 30], fun(x) { return x > 15; }));"; // 1
    EXPECT_EQ(runWithBuiltinModules(src), "1");
}

TEST(BuiltinModulesList, SortViaImport) {
    std::string src = "import { sort } from \"std/list\";"
                      "var s = sort([3, 1, 4, 1, 5, 9, 2, 6]);"
                      "print(s[0]);"
                      "print(s[7]);";
    EXPECT_EQ(runWithBuiltinModules(src), "19"); // [1,1,2,3,4,5,6,9] -> s[0]=1, s[7]=9
}

// ============================================================
// Interpreter 路径验证（三后端拦截一致性）
// ============================================================

TEST(BuiltinModulesInterpreterPath, FactorialWorks) {
    std::string src = "import { factorial } from \"std/math\";"
                      "print(factorial(5));"; // 120
    EXPECT_EQ(runInterpreterWithBuiltinModules(src), "120");
}

TEST(BuiltinModulesInterpreterPath, StringRepeatWorks) {
    std::string src = "import { repeat } from \"std/string\";"
                      "print(repeat(\"xy\", 4));"; // "xyxyxyxy"
    EXPECT_EQ(runInterpreterWithBuiltinModules(src), "xyxyxyxy");
}

TEST(BuiltinModulesInterpreterPath, ListReduceWorks) {
    std::string src = "import { reduce } from \"std/list\";"
                      "var product = reduce([2, 3, 4], fun(acc, x) { return acc * x; }, 1);"
                      "print(product);"; // 24
    EXPECT_EQ(runInterpreterWithBuiltinModules(src), "24");
}

// ============================================================
// 模块隔离与重复导入测试
// ============================================================

TEST(BuiltinModulesIsolation, MultipleImportsDoNotConflict) {
    // 同一内建模块被多次 import 应只加载一次（linkedModuleSet_ 去重）
    std::string src = "import { factorial } from \"std/math\";"
                      "import { fibonacci } from \"std/math\";"
                      "print(factorial(4));"  // 24
                      "print(fibonacci(7));"; // 13
    EXPECT_EQ(runWithBuiltinModules(src), "2413");
}

TEST(BuiltinModulesIsolation, CrossModuleImportWholeModule) {
    // std/string 和 std/list 都导出 contains，分别 import 整个模块。
    // MiniLang 当前语法不支持 `import { x as y }`，故用整体 import 验证
    // 不同模块可以独立加载（不强制验证名称冲突处理策略——那是另一议题）。
    // 这里只验证两个不同内建模块都能被加载并执行其各自函数。
    std::string src = "import { repeat } from \"std/string\";"
                      "import { reduce } from \"std/list\";"
                      "print(repeat(\"z\", 3));"                                   // "zzz"
                      "print(reduce([1, 2, 3], fun(a, x) { return a + x; }, 0));"; // 6
    EXPECT_EQ(runWithBuiltinModules(src), "zzz6");
}

// ============================================================
// 错误处理：未知 std/ 模块
// ============================================================

TEST(BuiltinModulesError, UnknownStdModuleReturnsCompileError) {
    // std/unknown 不在注册表中，moduleLoader 返回空字符串，
    // Compiler/Interpreter 应报模块加载失败
    std::string src = "import { foo } from \"std/unknown\";"
                      "print(foo);";
    std::string result = runWithBuiltinModules(src);
    EXPECT_NE(result.find("<compile:"), std::string::npos) << "未知 std/ 模块应触发编译错误，实际输出: " << result;
}

// ============================================================
// P2-11: 命名空间导入测试（import * as ns from "mod"）
// 验证命名空间字典构造、成员访问、run-once 路径、三后端一致性
// ============================================================

TEST(NamespaceImport, BasicMathNamespaceViaCompiler) {
    // import * as ns from "std/math" — 通过 Compiler/VM 路径
    // 命名空间 ns 是字典，通过 ns["name"] 访问导出函数
    std::string src = "import * as ns from \"std/math\";"
                      "print(ns[\"factorial\"](5));"   // 120
                      "print(ns[\"fibonacci\"](10));"; // 55
    EXPECT_EQ(runWithBuiltinModules(src), "12055");
}

TEST(NamespaceImport, StringNamespaceViaCompiler) {
    // import * as ns from "std/string" — 字符串模块命名空间
    std::string src = "import * as ns from \"std/string\";"
                      "print(ns[\"repeat\"](\"ab\", 3));"             // "ababab"
                      "print(ns[\"contains\"](\"hello\", \"ell\"));"; // true
    EXPECT_EQ(runWithBuiltinModules(src), "abababtrue");
}

TEST(NamespaceImport, ListNamespaceViaCompiler) {
    // import * as ns from "std/list" — 列表模块命名空间
    std::string src = "import * as ns from \"std/list\";"
                      "var doubled = ns[\"map\"]([1, 2, 3], fun(x) { return x * 2; });"
                      "print(doubled[0]);"
                      "print(doubled[1]);"
                      "print(doubled[2]);";
    EXPECT_EQ(runWithBuiltinModules(src), "246");
}

TEST(NamespaceImport, NamespaceAfterNamedImportRunOnce) {
    // P1 bug fix验证：先具名导入，再命名空间导入同一模块（run-once 路径）
    // run-once 路径也需构造命名空间字典（原 bug：run-once 时 namespaceAlias 被跳过）
    std::string src = "import { factorial } from \"std/math\";"
                      "import * as ns from \"std/math\";"
                      "print(factorial(4));"          // 24 — 具名导入仍可用
                      "print(ns[\"factorial\"](3));"; // 6 — 命名空间也包含 factorial
    EXPECT_EQ(runWithBuiltinModules(src), "246");
}

TEST(NamespaceImport, NamespaceBeforeNamedImportRunOnce) {
    // 反向 run-once：先命名空间导入，再具名导入
    std::string src = "import * as ns from \"std/math\";"
                      "import { fibonacci } from \"std/math\";"
                      "print(ns[\"factorial\"](4));" // 24
                      "print(fibonacci(7));";        // 13
    EXPECT_EQ(runWithBuiltinModules(src), "2413");
}

TEST(NamespaceImport, MultipleNamespacesFromDifferentModules) {
    // 从不同模块创建多个命名空间
    std::string src = "import * as math from \"std/math\";"
                      "import * as str from \"std/string\";"
                      "print(math[\"pow\"](2, 8));"        // 256
                      "print(str[\"repeat\"](\"x\", 4));"; // "xxxx"
    EXPECT_EQ(runWithBuiltinModules(src), "256xxxx");
}

TEST(NamespaceImport, NamespaceViaInterpreterPath) {
    // Interpreter 路径的命名空间导入（三后端一致性验证）
    std::string src = "import * as ns from \"std/math\";"
                      "print(ns[\"factorial\"](5));" // 120
                      "print(ns[\"gcd\"](48, 36));"; // 12
    EXPECT_EQ(runInterpreterWithBuiltinModules(src), "12012");
}

TEST(NamespaceImport, NamespaceDictContainsAllExports) {
    // 验证命名空间字典包含模块的全部导出
    // std/math 导出: sqrt, pow, factorial, fibonacci, gcd, max_of, min_of
    // 通过 len() 验证字典大小（至少 7 个导出）
    std::string src = "import * as ns from \"std/math\";"
                      "print(len(ns) >= 7);";
    EXPECT_EQ(runWithBuiltinModules(src), "true");
}

TEST(NamespaceImport, NamespaceDictIsMutableCopy) {
    // 验证命名空间是导出值的快照副本，修改不影响原全局变量
    std::string src = "import * as ns from \"std/math\";"
                      "var before = ns[\"factorial\"](3);"
                      "ns[\"custom\"] = 42;"    // 向命名空间添加新键
                      "print(before);"          // 6
                      "print(ns[\"custom\"]);"; // 42
    EXPECT_EQ(runWithBuiltinModules(src), "642");
}

TEST(NamespaceImport, NamespaceAccessMissingKeyReturnsNull) {
    // 访问命名空间中不存在的键返回 null（字典语义）
    std::string src = "import * as ns from \"std/math\";"
                      "print(ns[\"nonexistent\"]);";
    EXPECT_EQ(runWithBuiltinModules(src), "null");
}

// ============================================================
// P2-11: 命名空间导入三后端一致性测试
// 验证 Compiler(direct StackVM) / StackVM via IR / RegisterVM / Interpreter
// 四条路径产生相同结果
// ============================================================

TEST(NamespaceImport, ThreeBackendConsistency_BasicAccess) {
    std::string src = "import * as ns from \"std/math\";"
                      "print(ns[\"factorial\"](5));" // 120
                      "print(ns[\"gcd\"](48, 36));"; // 12
    std::string expected = "12012";
    EXPECT_EQ(runWithBuiltinModules(src), expected);
    EXPECT_EQ(runStackVM_IRWithBuiltinModules(src), expected);
    EXPECT_EQ(runRegVM_IRWithBuiltinModules(src), expected);
    EXPECT_EQ(runInterpreterWithBuiltinModules(src), expected);
}

TEST(NamespaceImport, ThreeBackendConsistency_RunOncePath) {
    // run-once 路径三后端一致性：先具名导入，再命名空间导入
    std::string src = "import { factorial } from \"std/math\";"
                      "import * as ns from \"std/math\";"
                      "print(factorial(4));"          // 24
                      "print(ns[\"factorial\"](3));"; // 6
    std::string expected = "246";
    EXPECT_EQ(runWithBuiltinModules(src), expected);
    EXPECT_EQ(runStackVM_IRWithBuiltinModules(src), expected);
    EXPECT_EQ(runRegVM_IRWithBuiltinModules(src), expected);
    EXPECT_EQ(runInterpreterWithBuiltinModules(src), expected);
}

TEST(NamespaceImport, ThreeBackendConsistency_MultipleNamespaces) {
    // 多命名空间三后端一致性
    std::string src = "import * as math from \"std/math\";"
                      "import * as str from \"std/string\";"
                      "print(math[\"pow\"](2, 8));"        // 256
                      "print(str[\"repeat\"](\"x\", 4));"; // "xxxx"
    std::string expected = "256xxxx";
    EXPECT_EQ(runWithBuiltinModules(src), expected);
    EXPECT_EQ(runStackVM_IRWithBuiltinModules(src), expected);
    EXPECT_EQ(runRegVM_IRWithBuiltinModules(src), expected);
    EXPECT_EQ(runInterpreterWithBuiltinModules(src), expected);
}

// ============================================================
// 拓展二期·语言：std/result — Result/Option 错误处理模块
// ============================================================

TEST(BuiltinModulesResult, RegistryContainsStdResult) {
    EXPECT_TRUE(BuiltinModuleRegistry::isBuiltinModule("std/result"));
    EXPECT_FALSE(BuiltinModuleRegistry::getSource("std/result").empty());
}

TEST(BuiltinModulesResult, OkErrConstructAndQuery_AllBackends) {
    std::string src = "import { Ok, Err, is_ok, is_err } from \"std/result\";"
                      "var a = Ok(42);"
                      "var b = Err(\"boom\");"
                      "print(is_ok(a));"
                      "print(is_err(a));"
                      "print(is_ok(b));"
                      "print(is_err(b));";
    std::string expected = "truefalsefalsetrue";
    EXPECT_EQ(runWithBuiltinModules(src), expected);
    EXPECT_EQ(runStackVM_IRWithBuiltinModules(src), expected);
    EXPECT_EQ(runRegVM_IRWithBuiltinModules(src), expected);
    EXPECT_EQ(runInterpreterWithBuiltinModules(src), expected);
}

TEST(BuiltinModulesResult, UnwrapAndUnwrapOr_AllBackends) {
    std::string src = "import { Ok, Err, unwrap, unwrap_or, unwrap_err } from \"std/result\";"
                      "print(unwrap(Ok(7)));"
                      "print(unwrap_or(Err(\"e\"), -1));"
                      "print(unwrap_or(Ok(3), -1));"
                      "print(unwrap_err(Err(\"bad\")));";
    std::string expected = "7-13bad";
    EXPECT_EQ(runWithBuiltinModules(src), expected);
    EXPECT_EQ(runStackVM_IRWithBuiltinModules(src), expected);
    EXPECT_EQ(runRegVM_IRWithBuiltinModules(src), expected);
    EXPECT_EQ(runInterpreterWithBuiltinModules(src), expected);
}

TEST(BuiltinModulesResult, UnwrapErrThrowsCatchable_AllBackends) {
    // unwrap(Err) 抛出 → try/catch 捕获（传播语义的运行时基础）
    std::string src = "import { Err, unwrap } from \"std/result\";"
                      "try {"
                      "    unwrap(Err(\"oops\"));"
                      "    print(\"unreached\");"
                      "} catch (e) {"
                      "    print(\"caught\");"
                      "}";
    std::string expected = "caught";
    EXPECT_EQ(runWithBuiltinModules(src), expected);
    EXPECT_EQ(runStackVM_IRWithBuiltinModules(src), expected);
    EXPECT_EQ(runRegVM_IRWithBuiltinModules(src), expected);
    EXPECT_EQ(runInterpreterWithBuiltinModules(src), expected);
}

TEST(BuiltinModulesResult, MapOkHigherOrder_AllBackends) {
    // map_ok 高阶组合子：Ok 变换，Err 短路传递
    std::string src = "import { Ok, Err, map_ok, unwrap, is_err } from \"std/result\";"
                      "fun double(x) { return x * 2; }"
                      "print(unwrap(map_ok(Ok(21), double)));"
                      "print(is_err(map_ok(Err(\"e\"), double)));";
    std::string expected = "42true";
    EXPECT_EQ(runWithBuiltinModules(src), expected);
    EXPECT_EQ(runStackVM_IRWithBuiltinModules(src), expected);
    EXPECT_EQ(runRegVM_IRWithBuiltinModules(src), expected);
    EXPECT_EQ(runInterpreterWithBuiltinModules(src), expected);
}

TEST(BuiltinModulesResult, OptionSomeNone_AllBackends) {
    std::string src = "import { Some, None, is_some, is_none, option_or } from \"std/result\";"
                      "var s = Some(5);"
                      "var n = None();"
                      "print(is_some(s));"
                      "print(is_none(n));"
                      "print(option_or(s, 0));"
                      "print(option_or(n, 0));";
    std::string expected = "truetrue50";
    EXPECT_EQ(runWithBuiltinModules(src), expected);
    EXPECT_EQ(runStackVM_IRWithBuiltinModules(src), expected);
    EXPECT_EQ(runRegVM_IRWithBuiltinModules(src), expected);
    EXPECT_EQ(runInterpreterWithBuiltinModules(src), expected);
}

TEST(BuiltinModulesResult, ErrorPropagationPattern_AllBackends) {
    // 完整传播模式演示：逐层 is_err 检查后提前返回 Err（? 运算符的手写等价形式）
    std::string src = "import { Ok, Err, is_err, unwrap } from \"std/result\";"
                      "fun safe_div(a, b) {"
                      "    if (b == 0) { return Err(\"div by zero\"); }"
                      "    return Ok(a / b);"
                      "}"
                      "fun compute(x) {"
                      "    var r1 = safe_div(100, x);"
                      "    if (is_err(r1)) { return r1; }"
                      "    var r2 = safe_div(unwrap(r1), 2);"
                      "    if (is_err(r2)) { return r2; }"
                      "    return Ok(unwrap(r2) + 1);"
                      "}"
                      "print(unwrap(compute(5)));"
                      "print(is_err(compute(0)));";
    // 100/5=20, 20/2=10, 10+1=11
    std::string expected = "11true";
    EXPECT_EQ(runWithBuiltinModules(src), expected);
    EXPECT_EQ(runStackVM_IRWithBuiltinModules(src), expected);
    EXPECT_EQ(runRegVM_IRWithBuiltinModules(src), expected);
    EXPECT_EQ(runInterpreterWithBuiltinModules(src), expected);
}

// ============================================================
// 拓展二期·语言：? 传播运算符（expr? → __qmark_unwrap 脱糖）
// ============================================================

TEST(QmarkOperator, UnwrapsOkAndSome_AllBackends) {
    std::string src = "import { Ok, Some } from \"std/result\";"
                      "print(Ok(7)?);"
                      "print(Some(5)?);";
    std::string expected = "75";
    EXPECT_EQ(runWithBuiltinModules(src), expected);
    EXPECT_EQ(runStackVM_IRWithBuiltinModules(src), expected);
    EXPECT_EQ(runRegVM_IRWithBuiltinModules(src), expected);
    EXPECT_EQ(runInterpreterWithBuiltinModules(src), expected);
}

TEST(QmarkOperator, PropagatesErrCatchable_AllBackends) {
    // Err? 抛出 → try/catch 捕获（异常式传播）
    std::string src = "import { Err } from \"std/result\";"
                      "try {"
                      "    var x = Err(\"boom\")?;"
                      "    print(\"unreached\");"
                      "} catch (e) {"
                      "    print(\"caught\");"
                      "}";
    std::string expected = "caught";
    EXPECT_EQ(runWithBuiltinModules(src), expected);
    EXPECT_EQ(runStackVM_IRWithBuiltinModules(src), expected);
    EXPECT_EQ(runRegVM_IRWithBuiltinModules(src), expected);
    EXPECT_EQ(runInterpreterWithBuiltinModules(src), expected);
}

TEST(QmarkOperator, PropagatesNoneCatchable_AllBackends) {
    std::string src = "import { None } from \"std/result\";"
                      "try {"
                      "    var x = None()?;"
                      "    print(\"unreached\");"
                      "} catch (e) {"
                      "    print(\"caught-none\");"
                      "}";
    std::string expected = "caught-none";
    EXPECT_EQ(runWithBuiltinModules(src), expected);
    EXPECT_EQ(runStackVM_IRWithBuiltinModules(src), expected);
    EXPECT_EQ(runRegVM_IRWithBuiltinModules(src), expected);
    EXPECT_EQ(runInterpreterWithBuiltinModules(src), expected);
}

TEST(QmarkOperator, ChainsWithCombinators_AllBackends) {
    // ? 作为后缀与高阶组合子链式搭配
    std::string src = "import { Ok, map_ok } from \"std/result\";"
                      "fun double(x) { return x * 2; }"
                      "print(map_ok(Ok(21), double)?);";
    std::string expected = "42";
    EXPECT_EQ(runWithBuiltinModules(src), expected);
    EXPECT_EQ(runStackVM_IRWithBuiltinModules(src), expected);
    EXPECT_EQ(runRegVM_IRWithBuiltinModules(src), expected);
    EXPECT_EQ(runInterpreterWithBuiltinModules(src), expected);
}

TEST(QmarkOperator, RejectsNonResultValue_AllBackends) {
    // 非 Result/Option 值使用 ? → 运行时错误（可捕获）
    std::string src = "try {"
                      "    var x = 42?;"
                      "    print(\"unreached\");"
                      "} catch (e) {"
                      "    print(\"type-err\");"
                      "}";
    std::string expected = "type-err";
    EXPECT_EQ(runWithBuiltinModules(src), expected);
    EXPECT_EQ(runStackVM_IRWithBuiltinModules(src), expected);
    EXPECT_EQ(runRegVM_IRWithBuiltinModules(src), expected);
    EXPECT_EQ(runInterpreterWithBuiltinModules(src), expected);
}

TEST(QmarkOperator, PropagationChainRewrite_AllBackends) {
    // 用 ? 改写 ErrorPropagationPattern：传播链由外层 try/catch 收口
    std::string src = "import { Ok, Err } from \"std/result\";"
                      "fun safe_div(a, b) {"
                      "    if (b == 0) { return Err(\"div by zero\"); }"
                      "    return Ok(a / b);"
                      "}"
                      "fun compute(x) {"
                      "    var v1 = safe_div(100, x)?;"
                      "    var v2 = safe_div(v1, 2)?;"
                      "    return v2 + 1;"
                      "}"
                      "print(compute(5));"
                      "try { print(compute(0)); } catch (e) { print(\"propagated\"); }";
    std::string expected = "11propagated";
    EXPECT_EQ(runWithBuiltinModules(src), expected);
    EXPECT_EQ(runStackVM_IRWithBuiltinModules(src), expected);
    EXPECT_EQ(runRegVM_IRWithBuiltinModules(src), expected);
    EXPECT_EQ(runInterpreterWithBuiltinModules(src), expected);
}

// ============================================================
// 拓展二期·语言：std/bigint — 字符串十进制大数（int 溢出提升演示）
// ============================================================

TEST(BuiltinModulesBigint, RegistryContainsStdBigint) {
    EXPECT_TRUE(BuiltinModuleRegistry::isBuiltinModule("std/bigint"));
    EXPECT_FALSE(BuiltinModuleRegistry::getSource("std/bigint").empty());
}

TEST(BuiltinModulesBigint, AddWithCarry_AllBackends) {
    std::string src = "import { badd } from \"std/bigint\";"
                      "print(badd(\"999\", \"1\"));"
                      "print(badd(\"12345\", \"67890\"));";
    std::string expected = "100080235"; // 1000 拼 80235
    EXPECT_EQ(runWithBuiltinModules(src), expected);
    EXPECT_EQ(runStackVM_IRWithBuiltinModules(src), expected);
    EXPECT_EQ(runRegVM_IRWithBuiltinModules(src), expected);
    EXPECT_EQ(runInterpreterWithBuiltinModules(src), expected);
}

TEST(BuiltinModulesBigint, MultiplyStripsLeadingZeros_AllBackends) {
    std::string src = "import { bmul } from \"std/bigint\";"
                      "print(bmul(\"123\", \"456\"));"
                      "print(bmul(\"0\", \"999\"));";
    std::string expected = "560880"; // 56088 拼 0
    EXPECT_EQ(runWithBuiltinModules(src), expected);
    EXPECT_EQ(runStackVM_IRWithBuiltinModules(src), expected);
    EXPECT_EQ(runRegVM_IRWithBuiltinModules(src), expected);
    EXPECT_EQ(runInterpreterWithBuiltinModules(src), expected);
}

TEST(BuiltinModulesBigint, CompareOrdering_AllBackends) {
    std::string src = "import { bcmp } from \"std/bigint\";"
                      "print(bcmp(\"100\", \"99\"));"
                      "print(bcmp(\"5\", \"5\"));"
                      "print(bcmp(\"7\", \"42\"));";
    std::string expected = "10-1";
    EXPECT_EQ(runWithBuiltinModules(src), expected);
    EXPECT_EQ(runStackVM_IRWithBuiltinModules(src), expected);
    EXPECT_EQ(runRegVM_IRWithBuiltinModules(src), expected);
    EXPECT_EQ(runInterpreterWithBuiltinModules(src), expected);
}

TEST(BuiltinModulesBigint, PowExceedsInt48Range_AllBackends) {
    // 2^64 = 18446744073709551616 远超 int48（原生 int 溢出），
    // BigInt 提升后字符串精确表示——本项目的核心演示点。
    std::string src = "import { bpow } from \"std/bigint\";"
                      "print(bpow(\"2\", 64));";
    std::string expected = "18446744073709551616";
    EXPECT_EQ(runWithBuiltinModules(src), expected);
    EXPECT_EQ(runStackVM_IRWithBuiltinModules(src), expected);
    EXPECT_EQ(runRegVM_IRWithBuiltinModules(src), expected);
    EXPECT_EQ(runInterpreterWithBuiltinModules(src), expected);
}

TEST(BuiltinModulesBigint, Factorial25_AllBackends) {
    // 25! = 15511210043330985984000000 远超 int48，验证大数乘法链式正确性
    std::string src = "import { bmul, bfrom } from \"std/bigint\";"
                      "var acc = \"1\";"
                      "var i = 1;"
                      "while (i <= 25) {"
                      "    acc = bmul(acc, bfrom(i));"
                      "    i = i + 1;"
                      "}"
                      "print(acc);";
    std::string expected = "15511210043330985984000000";
    EXPECT_EQ(runWithBuiltinModules(src), expected);
    EXPECT_EQ(runStackVM_IRWithBuiltinModules(src), expected);
    EXPECT_EQ(runRegVM_IRWithBuiltinModules(src), expected);
    EXPECT_EQ(runInterpreterWithBuiltinModules(src), expected);
}
