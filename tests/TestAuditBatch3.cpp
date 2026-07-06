// ============================================================
// 审计批次 3 回归测试 — finally 语法贯穿下游模块 + GC 泄漏修复
// ------------------------------------------------------------
// 覆盖审计发现的 5 个 Bug 修复（1 P1 + 4 P2）：
//   BUG-FE-AUDIT-1 (P1): Formatter visitTryStmt 忽略 finallyBlock 且无条件输出 catch
//   BUG-FE-AUDIT-2 (P2): ModuleIsolation 未递归 finallyBlock
//   BUG-FE-AUDIT-3 (P2): TypeChecker 未覆盖 try/catch/finally 块的 VarDecl 检查
//   BUG-FE-AUDIT-4 (P2): TypeChecker 未覆盖 export 包装声明的 VarDecl 检查
//   BUG-INTR-AUDIT-1 (P2): GcManager collectCycle Phase 3 tracked_.clear() 导致泄漏
//
// 注：BUG-GUI-AUDIT-2（CodeEditor 断点偏移）需要 Qt GUI 环境和交互模拟，
//     不在单元测试覆盖范围（与 TestFormatterAudit.cpp 同一约定）。
// ============================================================

#include <gtest/gtest.h>

#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "formatter/Formatter.h"
#include "common/TypeChecker.h"
#include "compiler/Compiler.h"
#include "compiler/Bytecode.h"
#include "compiler/VM.h"
#include "compiler/RegisterVM.h"
#include "compiler/IR.h"
#include "interpreter/Value.h"
#include "interpreter/Environment.h"
#include "interpreter/Interpreter.h"
#include "interpreter/GcManager.h"
#include "interpreter/RuntimeExceptions.h"
#include "common/Diagnostic.h"

#include <string>
#include <memory>
#include <unordered_map>

// ============================================================
// 辅助函数
// ============================================================

static std::string formatSource(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);
    if (!ast) return "";
    Formatter formatter;
    formatter.setComments(lexer.comments());
    return formatter.format(*ast);
}

static std::string formatTwice(const std::string& source) {
    std::string first = formatSource(source);
    return formatSource(first);
}

static std::string runInterpreter(const std::string& src) {
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

// 带 module loader 的 VM 执行（用于 ModuleIsolation 测试）
static std::string runVMWithModules(
    const std::string& source,
    const std::unordered_map<std::string, std::string>& modules) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);
    if (!ast) return "<parse-fail>";
    Compiler compiler;
    compiler.setModuleLoader([&](const std::string& path) -> std::string {
        auto it = modules.find(path);
        if (it == modules.end()) return "";
        return it->second;
    });
    CompileResult result = compiler.compile(*ast);
    if (compiler.getDiagnostics().hasErrors()) {
        return "<compile:" + compiler.getLastError() + ">";
    }
    VM vm;
    std::string captured;
    vm.setOutputCallback([&](const std::string& s) { captured += s; });
    vm.execute(result);
    if (vm.hasError()) {
        if (captured.empty()) return "<runtime:" + vm.getLastError() + ">";
        return captured + "<runtime:" + vm.getLastError() + ">";
    }
    return captured;
}

static std::string runVMWithModulesIR(
    const std::string& source,
    const std::unordered_map<std::string, std::string>& modules) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);
    if (!ast) return "<parse-fail>";
    Compiler compiler; compiler.setUseIR(true);
    compiler.setModuleLoader([&](const std::string& path) -> std::string {
        auto it = modules.find(path);
        if (it == modules.end()) return "";
        return it->second;
    });
    CompileResult result = compiler.compile(*ast);
    if (compiler.getDiagnostics().hasErrors()) {
        return "<compile:" + compiler.getLastError() + ">";
    }
    VM vm;
    std::string captured;
    vm.setOutputCallback([&](const std::string& s) { captured += s; });
    vm.execute(result);
    if (vm.hasError()) {
        if (captured.empty()) return "<runtime:" + vm.getLastError() + ">";
        return captured + "<runtime:" + vm.getLastError() + ">";
    }
    return captured;
}

static std::string runRegVMWithModules(
    const std::string& source,
    const std::unordered_map<std::string, std::string>& modules) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);
    if (!ast) return "<parse-fail>";
    Compiler compiler; compiler.setUseRegisterVM(true);
    compiler.setModuleLoader([&](const std::string& path) -> std::string {
        auto it = modules.find(path);
        if (it == modules.end()) return "";
        return it->second;
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
        if (captured.empty()) return "<runtime:" + vm.getLastError() + ">";
        return captured + "<runtime:" + vm.getLastError() + ">";
    }
    return captured;
}

// ============================================================
// BUG-FE-AUDIT-1: Formatter finally 块幂等性（P1）
// ============================================================

// ---- try-finally 无 catch：格式化应输出 "finally" 且不输出 "catch" ----
TEST(AuditBatch3FormatterFinally, TryFinallyNoCatch_Idempotent) {
    std::string src =
        "try {\n"
        "    print(\"try\");\n"
        "} finally {\n"
        "    print(\"cleanup\");\n"
        "}";
    std::string first = formatSource(src);
    std::string second = formatTwice(src);
    // 幂等性：格式化两次结果相同
    EXPECT_EQ(first, second);
    // 必须包含 "finally"，不应包含 "catch"
    EXPECT_NE(first.find("finally"), std::string::npos);
    EXPECT_EQ(first.find("catch"), std::string::npos);
}

// ---- try-catch-finally 三段式：格式化应输出全部三段 ----
TEST(AuditBatch3FormatterFinally, TryCatchFinally_Idempotent) {
    std::string src =
        "try {\n"
        "    doWork();\n"
        "} catch (e) {\n"
        "    print(e);\n"
        "} finally {\n"
        "    cleanup();\n"
        "}";
    std::string first = formatSource(src);
    std::string second = formatTwice(src);
    EXPECT_EQ(first, second);
    EXPECT_NE(first.find("try"), std::string::npos);
    EXPECT_NE(first.find("catch (e)"), std::string::npos);
    EXPECT_NE(first.find("finally"), std::string::npos);
}

// ---- try-catch 无 finally：保持原行为不回归 ----
TEST(AuditBatch3FormatterFinally, TryCatchNoFinally_Idempotent) {
    std::string src =
        "try {\n"
        "    risky();\n"
        "} catch (e) {\n"
        "    handle(e);\n"
        "}";
    std::string first = formatSource(src);
    std::string second = formatTwice(src);
    EXPECT_EQ(first, second);
    EXPECT_NE(first.find("catch (e)"), std::string::npos);
    // 不应输出 finally
    EXPECT_EQ(first.find("finally"), std::string::npos);
}

// ---- 往返等价性：format 后重新 parse 应成功 ----
TEST(AuditBatch3FormatterFinally, TryFinallyRoundTripParseable) {
    std::string src =
        "try {\n"
        "    print(\"a\");\n"
        "} finally {\n"
        "    print(\"b\");\n"
        "}";
    std::string formatted = formatSource(src);
    // 重新解析格式化后的输出应成功
    Lexer lx; auto tk = lx.scan(formatted);
    Parser p; auto ast = p.parse(tk);
    EXPECT_NE(ast, nullptr);
}

// ---- 三后端执行 finally 块语义正确（回归保护）----
TEST(AuditBatch3FormatterFinally, TryFinallyThreeBackendSemantics) {
    std::string src =
        "try {\n"
        "    print(\"try\");\n"
        "} finally {\n"
        "    print(\"finally\");\n"
        "}";
    std::string expected = "tryfinally";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ============================================================
// BUG-FE-AUDIT-2: ModuleIsolation finally 块递归（P2）
// ============================================================

// ---- finally 块引用模块非导出顶层变量：三后端应正确重命名并执行 ----
// 注：不在 try 块中使用 return，因为 return+finally 的语义在三后端间存在已知差异
// （BUG-INTR-AUDIT-2 设计限制）。本测试聚焦于 finally 块内的变量引用是否被正确重命名。
TEST(AuditBatch3ModuleIsolationFinally, FinallyReferencesModuleTopLevelVar) {
    std::string src =
        "import { run } from \"mymod\";"
        "print(run());";
    std::unordered_map<std::string, std::string> modules = {
        {"mymod",
         "var internalLog = \"module-var-accessible\";"
         "fun doWork() {"
         "    try {"
         "        print(internalLog);"  // try 块引用模块顶层变量
         "    } finally {"
         "        print(internalLog);"  // finally 块也引用同一变量（BUG-FE-AUDIT-2 核心）
         "    }"
         "}"
         "export fun run() { doWork(); return \"\"; }"}
    };
    std::string expected = "module-var-accessiblemodule-var-accessible";
    EXPECT_EQ(runVMWithModules(src, modules), expected);
    EXPECT_EQ(runVMWithModulesIR(src, modules), expected);
    EXPECT_EQ(runRegVMWithModules(src, modules), expected);
}

// ---- finally 块修改模块非导出顶层变量：三后端应正确同步 ----
TEST(AuditBatch3ModuleIsolationFinally, FinallyModifiesModuleTopLevelVar) {
    std::string src =
        "import { run, getCounter } from \"mymod\";"
        "print(run());"
        "print(getCounter());";
    std::unordered_map<std::string, std::string> modules = {
        {"mymod",
         "var counter = 0;"
         "fun doWork() {"
         "    try {"
         "        counter = 100;"
         "    } finally {"
         "        counter = counter + 1;"  // finally 修改模块顶层变量
         "    }"
         "}"
         "export fun run() { doWork(); return counter; }"
         "export fun getCounter() { return counter; }"}
    };
    // counter 先被 try 设为 100，finally 中 +1 → 101
    std::string expected = "101101";
    EXPECT_EQ(runVMWithModules(src, modules), expected);
    EXPECT_EQ(runVMWithModulesIR(src, modules), expected);
    EXPECT_EQ(runRegVMWithModules(src, modules), expected);
}

// ============================================================
// BUG-FE-AUDIT-3: TypeChecker try/catch/finally 覆盖（P2）
// ============================================================

// ---- try 块内 VarDecl 类型注解冲突应产生警告 ----
TEST(AuditBatch3TypeCheckerCoverage, TryBlockVarDeclTypeMismatchWarning) {
    std::string src =
        "try {"
        "    int x = \"not-a-number\";"  // int 注解 vs string 字面量 → 警告
        "} catch (e) {"
        "    bool y = 42;"  // bool 注解 vs int 字面量 → 警告
        "} finally {"
        "    string z = true;"  // string 注解 vs bool 字面量 → 警告
        "}";
    Lexer lx; auto tk = lx.scan(src);
    Parser p; auto ast = p.parse(tk);
    ASSERT_NE(ast, nullptr);
    minilang::MiniLangTypeChecker checker;
    auto diags = checker.check(*ast);
    // 应产生至少 3 条警告（try/catch/finally 各 1 条）
    EXPECT_GE(diags.warningCount(), 3)
        << "TypeChecker 应在 try/catch/finally 三个块中各检测到至少 1 条类型注解冲突";
}

// ---- try 块内类型匹配的 VarDecl 不应产生警告 ----
TEST(AuditBatch3TypeCheckerCoverage, TryBlockVarDeclTypeMatchNoWarning) {
    std::string src =
        "try {"
        "    int x = 42;"  // 匹配
        "} catch (e) {"
        "    string y = \"ok\";"  // 匹配
        "} finally {"
        "    bool z = true;"  // 匹配
        "}";
    Lexer lx; auto tk = lx.scan(src);
    Parser p; auto ast = p.parse(tk);
    ASSERT_NE(ast, nullptr);
    minilang::MiniLangTypeChecker checker;
    auto diags = checker.check(*ast);
    EXPECT_EQ(diags.warningCount(), 0)
        << "类型匹配的 VarDecl 不应产生警告";
}

// ---- throw 表达式中的字面量不应导致类型检查遗漏 ----
TEST(AuditBatch3TypeCheckerCoverage, ThrowExpressionRecursed) {
    std::string src =
        "try {"
        "    throw \"error-msg\";"
        "} catch (e) {"
        "    print(e);"
        "}";
    Lexer lx; auto tk = lx.scan(src);
    Parser p; auto ast = p.parse(tk);
    ASSERT_NE(ast, nullptr);
    minilang::MiniLangTypeChecker checker;
    auto diags = checker.check(*ast);
    // throw 表达式是字符串字面量，无类型注解冲突，不应产生警告
    // 主要验证 visitThrowStmt 不会 crash 且能正常遍历
    EXPECT_EQ(diags.warningCount(), 0);
}

// ============================================================
// BUG-FE-AUDIT-4: TypeChecker export 包装声明覆盖（P2）
// ============================================================

// ---- export var 类型注解冲突应产生警告 ----
TEST(AuditBatch3TypeCheckerCoverage, ExportVarDeclTypeMismatchWarning) {
    std::string src =
        "export int config = \"not-a-number\";"  // int 注解 vs string 字面量 → 警告
        "export fun helper() { return 1; }";
    Lexer lx; auto tk = lx.scan(src);
    Parser p; auto ast = p.parse(tk);
    ASSERT_NE(ast, nullptr);
    minilang::MiniLangTypeChecker checker;
    auto diags = checker.check(*ast);
    EXPECT_GE(diags.warningCount(), 1)
        << "export var 类型注解冲突应产生至少 1 条警告";
    // 验证警告内容提及变量名
    bool foundConfig = false;
    for (const auto& w : diags.all()) {
        if (w.message.find("config") != std::string::npos) {
            foundConfig = true;
            break;
        }
    }
    EXPECT_TRUE(foundConfig) << "警告消息应提及变量名 config";
}

// ---- export var 类型匹配不应产生警告 ----
TEST(AuditBatch3TypeCheckerCoverage, ExportVarDeclTypeMatchNoWarning) {
    std::string src =
        "export int count = 42;"  // 匹配
        "export string name = \"ok\";";  // 匹配
    Lexer lx; auto tk = lx.scan(src);
    Parser p; auto ast = p.parse(tk);
    ASSERT_NE(ast, nullptr);
    minilang::MiniLangTypeChecker checker;
    auto diags = checker.check(*ast);
    EXPECT_EQ(diags.warningCount(), 0);
}

// ---- export fun 内部 VarDecl 类型注解冲突应通过递归检查被发现 ----
TEST(AuditBatch3TypeCheckerCoverage, ExportFunBodyVarDeclTypeMismatchWarning) {
    std::string src =
        "export fun compute() {"
        "    int result = \"mismatch\";"  // int 注解 vs string 字面量 → 警告
        "    return result;"
        "}";
    Lexer lx; auto tk = lx.scan(src);
    Parser p; auto ast = p.parse(tk);
    ASSERT_NE(ast, nullptr);
    minilang::MiniLangTypeChecker checker;
    auto diags = checker.check(*ast);
    EXPECT_GE(diags.warningCount(), 1)
        << "export fun 内部的 VarDecl 类型冲突应通过递归检查被发现";
}

// ============================================================
// BUG-INTR-AUDIT-1: GcManager 循环引用容器跨 collectCycle 周期泄漏（P2）
// ============================================================
// 直接测试 GcManager 的 collectCycle 行为：存活的循环容器应在 tracked_ 中保留，
// 以便下一轮 collectCycle 能检查到它是否变为不可达。
// ----------------------------------------------------------------

// ---- 第一次 collectCycle（标记为可达）：存活的循环容器应保留在 tracked_ 中 ----
TEST(AuditBatch3GcLeak, CyclicContainerRetainedInTrackedWhenReachable) {
    GcManager::instance().reset();
    // 创建一个自引用数组：a = [a]
    Value arr(std::vector<Value>{});
    // 通过 push 自身形成循环引用
    arr.arrayVal().push_back(arr);  // arr.refCount 现在为 2（arr 自身 + 数组内的引用）
    // arr 是 GcManager tracked_ 中的存活对象

    size_t trackedBefore = GcManager::instance().trackedCount();
    EXPECT_GE(trackedBefore, 1u) << "循环数组应在 tracked_ 中";

    // 第一次 collectCycle：以 arr 为根，标记 arr 为可达
    const void* root = arr.gcRootPtr();
    ASSERT_NE(root, nullptr);
    std::vector<const void*> roots = {root};
    GcManager::instance().collectCycle(roots);

    // BUG-INTR-AUDIT-1 fix: 存活的循环容器应保留在 tracked_ 中
    // 原实现会 tracked_.clear()，导致循环容器丢失，后续无法回收
    size_t trackedAfter = GcManager::instance().trackedCount();
    EXPECT_GE(trackedAfter, 1u)
        << "BUG-INTR-AUDIT-1: 标记为可达的循环容器应保留在 tracked_ 中";

    GcManager::instance().reset();
}

// ---- 第二次 collectCycle（不再可达）：循环容器应被回收 ----
TEST(AuditBatch3GcLeak, CyclicContainerCollectedWhenUnreachable) {
    GcManager::instance().reset();

    // 创建循环引用并标记为可达（模拟第一次 collectCycle）
    Value arr(std::vector<Value>{});
    arr.arrayVal().push_back(arr);
    const void* root = arr.gcRootPtr();
    ASSERT_NE(root, nullptr);
    std::vector<const void*> roots = {root};
    GcManager::instance().collectCycle(roots);

    size_t trackedAfterFirst = GcManager::instance().trackedCount();
    EXPECT_GE(trackedAfterFirst, 1u) << "第一次 collectCycle 后循环容器应在 tracked_ 中";

    // 第二次 collectCycle：空根集，循环容器不再可达
    // BUG-INTR-AUDIT-1 fix: 由于第一次保留了 tracked_ 条目，第二次能检查到循环容器
    // 并将其标记为不可达孤岛，sweep 清空其子元素打破循环
    std::vector<const void*> emptyRoots;
    GcManager::instance().collectCycle(emptyRoots);

    // 循环容器应已被 sweep（从 tracked_ 中移除，因为子元素被清空后 refCount 降为 0）
    // 注：sweep 清空子元素后，arr 的 refCount 从 2 降到 1（自身），arr 仍存活
    // 但其子元素已空，tracked_ 应不再保留无效条目
    // 关键是：如果不修复（tracked_.clear()），第二次 collectCycle 根本看不到循环容器
    size_t trackedAfterSecond = GcManager::instance().trackedCount();
    // 修复后，第二次 collectCycle 能看到循环容器并处理它
    // tracked_ 应小于第一次（孤岛被处理）
    EXPECT_LT(trackedAfterSecond, trackedAfterFirst)
        << "BUG-INTR-AUDIT-1: 第二次 collectCycle 应能处理第一次保留的循环容器";

    GcManager::instance().reset();
}

// ---- 多次 collectCycle 不应导致 tracked_ 无限增长 ----
TEST(AuditBatch3GcLeak, TrackedCountStableAcrossMultipleCycles) {
    GcManager::instance().reset();
    size_t baseline = GcManager::instance().trackedCount();

    // 模拟 5 轮 collectCycle，每轮创建一个循环容器并标记为可达
    for (int i = 0; i < 5; ++i) {
        // 创建循环引用
        Value arr(std::vector<Value>{});
        arr.arrayVal().push_back(arr);
        const void* root = arr.gcRootPtr();
        std::vector<const void*> roots = {root};
        // 第一次 collectCycle：标记为可达
        GcManager::instance().collectCycle(roots);
        // 第二次 collectCycle：不再可达，应被回收
        std::vector<const void*> emptyRoots;
        GcManager::instance().collectCycle(emptyRoots);
    }

    size_t afterRuns = GcManager::instance().trackedCount();
    // tracked_ 不应无限增长（每轮的循环容器都应被回收）
    EXPECT_LE(afterRuns, baseline + 1)
        << "多次 collectCycle 后 tracked_ 不应无限增长";

    GcManager::instance().reset();
}
