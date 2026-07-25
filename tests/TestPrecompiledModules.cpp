// ============================================================
// TestPrecompiledModules.cpp — P2-11 预编译模块测试
// ------------------------------------------------------------
// 验证 .minic 预编译模块的完整生命周期：
//   1. compileModule 独立编译生成 CompileResult（含 moduleExports）
//   2. BytecodeCache::storeToFile / tryLoadFromFile 序列化往返
//   3. visitImportStmt 优先加载 .minic + 全局槽位重定位
//   4. 端到端：预编译模块 → 主程序 import → VM 执行结果正确
//   5. 非导出函数名隔离（前缀化避免与主程序同名冲突）
//
// 注：MiniLang 语法要求 var/赋值/print/import 语句以 ';' 结尾，
// fun 声明以 '}' 结尾不需要分号。print 不自动追加换行。
// ============================================================

#include "compiler/BytecodeCache.h"
#include "compiler/Compiler.h"
#include "compiler/RegisterVM.h"
#include "compiler/VM.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <thread>

namespace fs = std::filesystem;

// ============================================================
// 测试辅助：临时目录 RAII 守卫
// ============================================================
class TempDir {
public:
    explicit TempDir(const std::string& prefix = "minilang_test_") {
        auto tmp = fs::temp_directory_path();
        // 生成唯一目录名
        std::string name = prefix + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) + "_" +
                           std::to_string(counter_++);
        path_ = tmp / name;
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    const fs::path& path() const { return path_; }
    std::string str() const { return path_.string(); }

private:
    fs::path path_;
    static inline std::atomic<int> counter_{0};
};

// ============================================================
// 1. compileModule 基本功能测试
// ============================================================

TEST(PrecompiledModules, CompileModuleProducesNonEmptyResult) {
    std::string modSource = R"(
export var x = 42;
export var y = "hello";
export fun add(a, b) { return a + b; }
)";
    Compiler compiler;
    auto result = compiler.compileModule(modSource, "testmod");

    // 编译成功
    EXPECT_FALSE(compiler.getDiagnostics().hasErrors()) << "compileModule error: " << compiler.getLastError();

    // 全局槽位非空
    EXPECT_GT(result.globalSlotCount, 0);

    // 导出列表包含 x, y, add
    ASSERT_EQ(result.moduleExports.size(), 3u);
    // moduleExports 已排序
    EXPECT_EQ(result.moduleExports[0], "add");
    EXPECT_EQ(result.moduleExports[1], "x");
    EXPECT_EQ(result.moduleExports[2], "y");
}

TEST(PrecompiledModules, CompileModuleMainChunkHasReturn) {
    std::string modSource = "export var x = 1;";
    Compiler compiler;
    auto result = compiler.compileModule(modSource, "testmod");

    ASSERT_FALSE(compiler.getDiagnostics().hasErrors()) << "compileModule error: " << compiler.getLastError();

    // mainChunk 应有字节码且以 OP_RETURN 结尾
    EXPECT_GT(result.mainChunk.code.size(), 0u);
    OpCode lastOp = static_cast<OpCode>(result.mainChunk.code.back());
    EXPECT_EQ(lastOp, OpCode::OP_RETURN);
}

TEST(PrecompiledModules, CompileModuleNoExportsProducesEmptyExports) {
    std::string modSource = R"(
var internal = 42;
fun helper() { return internal; }
)";
    Compiler compiler;
    auto result = compiler.compileModule(modSource, "testmod");

    EXPECT_FALSE(compiler.getDiagnostics().hasErrors()) << "compileModule error: " << compiler.getLastError();
    EXPECT_TRUE(result.moduleExports.empty());
}

// ============================================================
// 2. BytecodeCache 序列化往返测试
// ============================================================

TEST(PrecompiledModules, StoreAndLoadFromFileRoundTrip) {
    TempDir tmpDir;
    std::string minicPath = (tmpDir.path() / "testmod.minic").string();

    std::string modSource = R"(
export var x = 42;
export var y = "hello";
export fun multiply(a, b) { return a * b; }
)";
    Compiler compiler;
    auto result = compiler.compileModule(modSource, "testmod");
    ASSERT_FALSE(compiler.getDiagnostics().hasErrors());

    // 存储到文件
    BytecodeCache cache;
    bool stored = cache.storeToFile(minicPath, result, "testmod");
    EXPECT_TRUE(stored) << "storeToFile failed";
    EXPECT_TRUE(fs::exists(minicPath));

    // 从文件加载
    auto loaded = cache.tryLoadFromFile(minicPath);
    ASSERT_TRUE(loaded.has_value());

    // 验证导出列表一致
    ASSERT_EQ(loaded->moduleExports.size(), result.moduleExports.size());
    for (size_t i = 0; i < result.moduleExports.size(); ++i) {
        EXPECT_EQ(loaded->moduleExports[i], result.moduleExports[i]);
    }

    // 验证全局槽位一致
    EXPECT_EQ(loaded->globalSlotCount, result.globalSlotCount);
    ASSERT_EQ(loaded->globalSlotNames.size(), result.globalSlotNames.size());
    for (size_t i = 0; i < result.globalSlotNames.size(); ++i) {
        EXPECT_EQ(loaded->globalSlotNames[i], result.globalSlotNames[i]);
    }
}

TEST(PrecompiledModules, TryLoadFromFileReturnsNulloptForMissingFile) {
    BytecodeCache cache;
    auto result = cache.tryLoadFromFile("nonexistent_file.minic");
    EXPECT_FALSE(result.has_value());
}

TEST(PrecompiledModules, TryLoadFromFileReturnsNulloptForCorruptedFile) {
    TempDir tmpDir;
    std::string badPath = (tmpDir.path() / "bad.minic").string();

    // 写入垃圾数据
    {
        std::ofstream ofs(badPath, std::ios::binary);
        ofs << "not a valid minic file";
    }

    BytecodeCache cache;
    auto result = cache.tryLoadFromFile(badPath);
    EXPECT_FALSE(result.has_value());
}

// ============================================================
// 3. 端到端：预编译模块 → import → VM 执行
// ============================================================

TEST(PrecompiledModules, E2E_PrecompiledModuleImportMatchesSourceImport) {
    TempDir tmpDir;

    // 模块源码
    std::string modSource = R"(
export var PI = 3;
export fun double(x) { return x * 2; }
export var greeting = "world";
)";

    // 主程序源码：import 模块并使用导出名
    std::string mainSource = R"(
import { PI, double, greeting } from "mymod";
print(PI);
print(double(21));
print(greeting);
)";

    // --- 路径 A：源码编译（baseline）---
    std::string sourceResult = [&]() {
        Lexer lexer;
        auto tokens = lexer.scan(mainSource);
        Parser parser;
        auto ast = parser.parse(tokens);
        if (!ast)
            return std::string("<parse:null>");
        Compiler compiler;
        compiler.setModuleLoader([&](const std::string& modPath) -> std::string {
            if (modPath == "mymod")
                return modSource;
            return "";
        });
        CompileResult result;
        try {
            result = compiler.compile(*ast);
        } catch (const std::exception& e) {
            return std::string("<compile:" + std::string(e.what()) + ">");
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
    }();

    // --- 路径 B：预编译模块 ---
    // 1. 预编译模块
    Compiler modCompiler;
    auto modResult = modCompiler.compileModule(modSource, "mymod");
    ASSERT_FALSE(modCompiler.getDiagnostics().hasErrors()) << "compileModule error: " << modCompiler.getLastError();

    // 2. 存储为 .minic
    std::string minicPath = (tmpDir.path() / "mymod.minic").string();
    BytecodeCache cache;
    ASSERT_TRUE(cache.storeToFile(minicPath, modResult, "mymod"));

    // 3. 主程序使用 .minic 导入
    std::string precompiledResult = [&]() {
        Lexer lexer;
        auto tokens = lexer.scan(mainSource);
        Parser parser;
        auto ast = parser.parse(tokens);
        if (!ast)
            return std::string("<parse:null>");
        Compiler compiler;
        // 设置预编译模块解析器
        compiler.setPrecompiledModuleResolver([&](const std::string& modPath) -> std::string {
            if (modPath == "mymod")
                return minicPath;
            return "";
        });
        // 也设置 moduleLoader 作为 fallback（.minic 加载失败时使用）
        compiler.setModuleLoader([&](const std::string& modPath) -> std::string {
            if (modPath == "mymod")
                return modSource;
            return "";
        });
        CompileResult result;
        try {
            result = compiler.compile(*ast);
        } catch (const std::exception& e) {
            return std::string("<compile:" + std::string(e.what()) + ">");
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
    }();

    // 两条路径的结果应该一致
    EXPECT_EQ(sourceResult, precompiledResult) << "Source: " << sourceResult << "\nPrecompiled: " << precompiledResult;

    // 验证输出包含预期值（print 不自动加换行，输出为 "342world"）
    EXPECT_NE(sourceResult.find("3"), std::string::npos);     // PI = 3
    EXPECT_NE(sourceResult.find("42"), std::string::npos);    // double(21) = 42
    EXPECT_NE(sourceResult.find("world"), std::string::npos); // greeting = "world"
}

// ============================================================
// 4. 命名空间导入 + 预编译模块
// ============================================================

TEST(PrecompiledModules, E2E_NamespaceImportFromPrecompiledModule) {
    TempDir tmpDir;

    std::string modSource = R"(
export var x = 10;
export var y = 20;
export fun sum() { return x + y; }
)";

    // 主程序：命名空间导入 + 具名导入（函数通过具名导入调用，变量通过命名空间访问）
    // 注：ns.sum() 是方法调用语法（OP_METHOD_CALL），字典仅支持内建方法；
    //     函数值通过 ns["sum"]() 索引访问 + OP_CALL_EXPR 调用。
    std::string mainSource = R"(
import * as ns from "nsmod";
import { sum } from "nsmod";
print(ns.x);
print(ns.y);
print(sum());
)";

    // --- 路径 A：源码编译（baseline）---
    std::string sourceResult = [&]() {
        Lexer lexer;
        auto tokens = lexer.scan(mainSource);
        Parser parser;
        auto ast = parser.parse(tokens);
        if (!ast)
            return std::string("<parse:null>");
        Compiler compiler;
        compiler.setModuleLoader([&](const std::string& modPath) -> std::string {
            if (modPath == "nsmod")
                return modSource;
            return "";
        });
        CompileResult result;
        try {
            result = compiler.compile(*ast);
        } catch (const std::exception& e) {
            return std::string("<compile:" + std::string(e.what()) + ">");
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
    }();

    // --- 路径 B：预编译模块 ---
    Compiler modCompiler;
    auto modResult = modCompiler.compileModule(modSource, "nsmod");
    ASSERT_FALSE(modCompiler.getDiagnostics().hasErrors()) << "compileModule error: " << modCompiler.getLastError();

    std::string minicPath = (tmpDir.path() / "nsmod.minic").string();
    BytecodeCache cache;
    ASSERT_TRUE(cache.storeToFile(minicPath, modResult, "nsmod"));

    std::string precompiledResult = [&]() {
        Lexer lexer;
        auto tokens = lexer.scan(mainSource);
        Parser parser;
        auto ast = parser.parse(tokens);
        if (!ast)
            return std::string("<parse:null>");
        Compiler compiler;
        compiler.setPrecompiledModuleResolver([&](const std::string& modPath) -> std::string {
            if (modPath == "nsmod")
                return minicPath;
            return "";
        });
        compiler.setModuleLoader([&](const std::string& modPath) -> std::string {
            if (modPath == "nsmod")
                return modSource;
            return "";
        });
        CompileResult result;
        try {
            result = compiler.compile(*ast);
        } catch (const std::exception& e) {
            return std::string("<compile:" + std::string(e.what()) + ">");
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
    }();

    // 两条路径的结果应该一致
    EXPECT_EQ(sourceResult, precompiledResult) << "Source: " << sourceResult << "\nPrecompiled: " << precompiledResult;

    // print 不加换行：ns.x=10, ns.y=20, sum()=30 → "102030"
    EXPECT_EQ(precompiledResult, "102030") << "Output: " << precompiledResult;
}

// ============================================================
// 5. 非导出函数名隔离测试
// ============================================================

TEST(PrecompiledModules, NonExportedFunctionNamesAreIsolated) {
    TempDir tmpDir;

    // 模块有一个非导出的 helper 函数，和一个导出的 main 函数
    std::string modSource = R"(
fun helper() { return 999; }
export fun main() { return helper(); }
)";

    // 主程序也有一个 helper 函数（不同实现）
    std::string mainSource = R"(
import { main } from "isolationmod";
fun helper() { return 111; }
print(main());
print(helper());
)";

    // 预编译模块
    Compiler modCompiler;
    auto modResult = modCompiler.compileModule(modSource, "isolationmod");
    ASSERT_FALSE(modCompiler.getDiagnostics().hasErrors()) << "compileModule error: " << modCompiler.getLastError();

    std::string minicPath = (tmpDir.path() / "isolationmod.minic").string();
    BytecodeCache cache;
    ASSERT_TRUE(cache.storeToFile(minicPath, modResult, "isolationmod"));

    // 使用预编译模块执行
    std::string output = [&]() {
        Lexer lexer;
        auto tokens = lexer.scan(mainSource);
        Parser parser;
        auto ast = parser.parse(tokens);
        if (!ast)
            return std::string("<parse:null>");
        Compiler compiler;
        compiler.setPrecompiledModuleResolver([&](const std::string& modPath) -> std::string {
            if (modPath == "isolationmod")
                return minicPath;
            return "";
        });
        compiler.setModuleLoader([&](const std::string& modPath) -> std::string {
            if (modPath == "isolationmod")
                return modSource;
            return "";
        });
        CompileResult result;
        try {
            result = compiler.compile(*ast);
        } catch (const std::exception& e) {
            return std::string("<compile:" + std::string(e.what()) + ">");
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
    }();

    // 模块的 main() 调用模块的 helper() → 999
    // 主程序的 helper() → 111
    // print 不加换行 → "999111"
    EXPECT_EQ(output, "999111") << "Output: " << output;
}

// ============================================================
// 6. run-once 语义：同一模块多次 import 只初始化一次
// ============================================================

TEST(PrecompiledModules, RunOnceMultipleImportsOnlyInitOnce) {
    TempDir tmpDir;

    // 模块有一个可变计数器，每次初始化 +1
    std::string modSource = R"(
export var counter = 0;
counter = counter + 1;
)";

    // 两次导入同一模块（具名 + 命名空间），验证 run-once 语义
    // 注：import { counter as c2 } 的 as 别名语法不受支持，改用命名空间导入验证
    std::string mainSource = R"(
import { counter } from "countermod";
import * as ns from "countermod";
print(counter);
print(ns.counter);
)";

    // 预编译模块
    Compiler modCompiler;
    auto modResult = modCompiler.compileModule(modSource, "countermod");
    ASSERT_FALSE(modCompiler.getDiagnostics().hasErrors()) << "compileModule error: " << modCompiler.getLastError();

    std::string minicPath = (tmpDir.path() / "countermod.minic").string();
    BytecodeCache cache;
    ASSERT_TRUE(cache.storeToFile(minicPath, modResult, "countermod"));

    std::string output = [&]() {
        Lexer lexer;
        auto tokens = lexer.scan(mainSource);
        Parser parser;
        auto ast = parser.parse(tokens);
        if (!ast)
            return std::string("<parse:null>");
        Compiler compiler;
        compiler.setPrecompiledModuleResolver([&](const std::string& modPath) -> std::string {
            if (modPath == "countermod")
                return minicPath;
            return "";
        });
        compiler.setModuleLoader([&](const std::string& modPath) -> std::string {
            if (modPath == "countermod")
                return modSource;
            return "";
        });
        CompileResult result;
        try {
            result = compiler.compile(*ast);
        } catch (const std::exception& e) {
            return std::string("<compile:" + std::string(e.what()) + ">");
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
    }();

    // 两次 import 应该得到相同的值（run-once：模块只初始化一次）
    // counter = 0 + 1 = 1
    // print 不加换行 → "11"
    EXPECT_EQ(output, "11") << "Output: " << output;
}

// ============================================================
// 7. 回退测试：.minic 不存在时回退到源码编译
// ============================================================

TEST(PrecompiledModules, FallsBackToSourceWhenMinicMissing) {
    std::string modSource = R"(
export var x = 42;
)";

    std::string mainSource = R"(
import { x } from "fallbackmod";
print(x);
)";

    std::string output = [&]() {
        Lexer lexer;
        auto tokens = lexer.scan(mainSource);
        Parser parser;
        auto ast = parser.parse(tokens);
        if (!ast)
            return std::string("<parse:null>");
        Compiler compiler;
        // 设置 resolver 返回不存在的路径
        compiler.setPrecompiledModuleResolver([&](const std::string& modPath) -> std::string {
            if (modPath == "fallbackmod")
                return "/nonexistent/path/fallbackmod.minic";
            return "";
        });
        // 设置 moduleLoader 作为 fallback
        compiler.setModuleLoader([&](const std::string& modPath) -> std::string {
            if (modPath == "fallbackmod")
                return modSource;
            return "";
        });
        CompileResult result;
        try {
            result = compiler.compile(*ast);
        } catch (const std::exception& e) {
            return std::string("<compile:" + std::string(e.what()) + ">");
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
    }();

    // print 不加换行 → "42"
    EXPECT_EQ(output, "42") << "Output: " << output;
}

// ============================================================
// 8. L12: 源码 mtime + content hash 失效校验
// ============================================================

// 辅助：写源码文件
static void writeSourceFile(const std::string& path, const std::string& content) {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(ofs.good()) << "无法写入: " << path;
    ofs << content;
}

// 辅助：等待文件系统 mtime 粒度（避免写入太快 mtime 未更新）
static void waitMtimeGranularity() {
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
}

TEST(PrecompiledModules, L12_StoreWithSourcePath_LoadUnchangedSource_Succeeds) {
    TempDir tmpDir;
    std::string sourcePath = (tmpDir.path() / "l12_ok.mini").string();
    std::string minicPath = (tmpDir.path() / "l12_ok.minic").string();

    writeSourceFile(sourcePath, "export var x = 1;\n");

    Compiler compiler;
    auto result = compiler.compileModule("export var x = 1;\n", "l12_ok");
    ASSERT_FALSE(compiler.getDiagnostics().hasErrors());

    BytecodeCache cache;
    ASSERT_TRUE(cache.storeToFile(minicPath, result, "l12_ok", sourcePath));

    // 源码未修改，加载应成功
    auto loaded = cache.tryLoadFromFile(minicPath, sourcePath);
    ASSERT_TRUE(loaded.has_value()) << "源码未修改，应加载成功";
    EXPECT_EQ(loaded->moduleExports.size(), 1u);
}

TEST(PrecompiledModules, L12_StoreWithSourcePath_ModifySource_LoadReturnsNullopt) {
    TempDir tmpDir;
    std::string sourcePath = (tmpDir.path() / "l12_stale.mini").string();
    std::string minicPath = (tmpDir.path() / "l12_stale.minic").string();

    writeSourceFile(sourcePath, "export var x = 1;\n");

    Compiler compiler;
    auto result = compiler.compileModule("export var x = 1;\n", "l12_stale");
    ASSERT_FALSE(compiler.getDiagnostics().hasErrors());

    BytecodeCache cache;
    ASSERT_TRUE(cache.storeToFile(minicPath, result, "l12_stale", sourcePath));

    // 修改源码（确保 mtime 推进 + 内容变更）
    waitMtimeGranularity();
    writeSourceFile(sourcePath, "export var x = 999;\n");

    // 加载应失败（源码已修改，.minic 失效）
    auto loaded = cache.tryLoadFromFile(minicPath, sourcePath);
    EXPECT_FALSE(loaded.has_value()) << "源码已修改，应返回 nullopt";
}

TEST(PrecompiledModules, L12_StoreWithSourcePath_SourceDeleted_LoadReturnsNullopt) {
    TempDir tmpDir;
    std::string sourcePath = (tmpDir.path() / "l12_del.mini").string();
    std::string minicPath = (tmpDir.path() / "l12_del.minic").string();

    writeSourceFile(sourcePath, "export var x = 1;\n");

    Compiler compiler;
    auto result = compiler.compileModule("export var x = 1;\n", "l12_del");
    ASSERT_FALSE(compiler.getDiagnostics().hasErrors());

    BytecodeCache cache;
    ASSERT_TRUE(cache.storeToFile(minicPath, result, "l12_del", sourcePath));

    // 删除源码文件
    std::error_code ec;
    ASSERT_TRUE(fs::remove(sourcePath, ec));

    // 加载应失败（源码不存在，.minic 失效）
    auto loaded = cache.tryLoadFromFile(minicPath, sourcePath);
    EXPECT_FALSE(loaded.has_value()) << "源码已删除，应返回 nullopt";
}

TEST(PrecompiledModules, L12_StoreWithoutSourcePath_LoadWithSourcePath_Succeeds_BackwardCompat) {
    TempDir tmpDir;
    std::string sourcePath = (tmpDir.path() / "l12_old.mini").string();
    std::string minicPath = (tmpDir.path() / "l12_old.minic").string();

    writeSourceFile(sourcePath, "export var x = 1;\n");

    Compiler compiler;
    auto result = compiler.compileModule("export var x = 1;\n", "l12_old");
    ASSERT_FALSE(compiler.getDiagnostics().hasErrors());

    // 旧式存储：不传 sourcePath（头部 mtime=0）
    BytecodeCache cache;
    ASSERT_TRUE(cache.storeToFile(minicPath, result, "l12_old"));

    // 即使提供 sourcePath，由于头部 mtime=0，应跳过校验，加载成功
    auto loaded = cache.tryLoadFromFile(minicPath, sourcePath);
    ASSERT_TRUE(loaded.has_value()) << "旧 .minic 头部 mtime=0，应跳过校验";
}

TEST(PrecompiledModules, L12_StoreWithSourcePath_LoadWithoutSourcePath_Succeeds) {
    TempDir tmpDir;
    std::string sourcePath = (tmpDir.path() / "l12_nosrc.mini").string();
    std::string minicPath = (tmpDir.path() / "l12_nosrc.minic").string();

    writeSourceFile(sourcePath, "export var x = 1;\n");

    Compiler compiler;
    auto result = compiler.compileModule("export var x = 1;\n", "l12_nosrc");
    ASSERT_FALSE(compiler.getDiagnostics().hasErrors());

    BytecodeCache cache;
    ASSERT_TRUE(cache.storeToFile(minicPath, result, "l12_nosrc", sourcePath));

    // 加载时不传 sourcePath → 跳过校验，加载成功
    auto loaded = cache.tryLoadFromFile(minicPath);
    ASSERT_TRUE(loaded.has_value()) << "加载未传 sourcePath，应跳过校验";
}

// ============================================================
// 9. L12 端到端：源码修改后 .minic 失效，Compiler 回退到源码编译
// ============================================================

TEST(PrecompiledModules, L12_E2E_StaleMinicFallsBackToSourceCompile) {
    TempDir tmpDir;
    std::string sourcePath = (tmpDir.path() / "l12_e2e.mini").string();
    std::string minicPath = (tmpDir.path() / "l12_e2e.minic").string();

    // 初始源码：导出 x = 1
    std::string initialSource = "export var x = 1;\n";
    writeSourceFile(sourcePath, initialSource);

    // 预编译 .minic（嵌入源码 mtime+hash）
    {
        Compiler modCompiler;
        auto modResult = modCompiler.compileModule(initialSource, "l12_e2e");
        ASSERT_FALSE(modCompiler.getDiagnostics().hasErrors());
        BytecodeCache cache;
        ASSERT_TRUE(cache.storeToFile(minicPath, modResult, "l12_e2e", sourcePath));
    }

    // 修改源码：x = 999（.minic 现在已过时）
    waitMtimeGranularity();
    std::string modifiedSource = "export var x = 999;\n";
    writeSourceFile(sourcePath, modifiedSource);

    // 主程序导入 x 并打印
    std::string mainSource = R"(
import { x } from "l12_e2e";
print(x);
)";

    std::string output = [&]() {
        Lexer lexer;
        auto tokens = lexer.scan(mainSource);
        Parser parser;
        auto ast = parser.parse(tokens);
        if (!ast)
            return std::string("<parse:null>");
        Compiler compiler;
        // resolver 返回 .minic 路径；Compiler.loadPrecompiledModule 会自动推导
        // 同 stem 的 .mini 源码路径并校验 mtime/hash
        compiler.setPrecompiledModuleResolver([&](const std::string& modPath) -> std::string {
            if (modPath == "l12_e2e")
                return minicPath;
            return "";
        });
        compiler.setModuleLoader([&](const std::string& modPath) -> std::string {
            if (modPath == "l12_e2e")
                return modifiedSource; // 返回修改后的源码（fallback 路径使用）
            return "";
        });
        CompileResult result;
        try {
            result = compiler.compile(*ast);
        } catch (const std::exception& e) {
            return std::string("<compile:" + std::string(e.what()) + ">");
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
    }();

    // .minic 已过时（源码 mtime 不匹配）→ 回退到源码编译 → x = 999
    EXPECT_EQ(output, "999") << "Output: " << output;
}

// ============================================================
// 10. L12 端到端：源码未修改时 .minic 命中（正常路径）
// ============================================================

TEST(PrecompiledModules, L12_E2E_FreshMinicHitsCache) {
    TempDir tmpDir;
    std::string sourcePath = (tmpDir.path() / "l12_fresh.mini").string();
    std::string minicPath = (tmpDir.path() / "l12_fresh.minic").string();

    std::string modSource = "export var x = 42;\n";
    writeSourceFile(sourcePath, modSource);

    // 预编译 .minic
    {
        Compiler modCompiler;
        auto modResult = modCompiler.compileModule(modSource, "l12_fresh");
        ASSERT_FALSE(modCompiler.getDiagnostics().hasErrors());
        BytecodeCache cache;
        ASSERT_TRUE(cache.storeToFile(minicPath, modResult, "l12_fresh", sourcePath));
    }

    // 主程序导入（源码未修改，.minic 应命中）
    std::string mainSource = R"(
import { x } from "l12_fresh";
print(x);
)";

    std::string output = [&]() {
        Lexer lexer;
        auto tokens = lexer.scan(mainSource);
        Parser parser;
        auto ast = parser.parse(tokens);
        if (!ast)
            return std::string("<parse:null>");
        Compiler compiler;
        compiler.setPrecompiledModuleResolver([&](const std::string& modPath) -> std::string {
            if (modPath == "l12_fresh")
                return minicPath;
            return "";
        });
        // moduleLoader 返回一个明显错误的"假"源码，验证 .minic 命中而非 fallback
        compiler.setModuleLoader([&](const std::string& modPath) -> std::string {
            if (modPath == "l12_fresh")
                return "export var x = 999;\n"; // 假源码
            return "";
        });
        CompileResult result;
        try {
            result = compiler.compile(*ast);
        } catch (const std::exception& e) {
            return std::string("<compile:" + std::string(e.what()) + ">");
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
    }();

    // .minic 命中 → x = 42（不是假源码的 999）
    EXPECT_EQ(output, "42") << "Output: " << output;
}

// ============================================================
// L11 预编译模块（RegisterVM 路径）测试
// ============================================================

// --- L11 RegisterVM: compileModuleViaRegisterIR 基本功能 ---

TEST(PrecompiledModules, L11_Register_CompileModuleProducesNonEmptyResult) {
    std::string modSource = R"(
export var x = 10;
export fun add(a, b) { return a + b; }
)";
    Compiler compiler;
    compiler.setUseRegisterVM(true);
    auto result = compiler.compileModuleViaRegisterIR(modSource, "testmod");
    ASSERT_FALSE(compiler.getDiagnostics().hasErrors()) << compiler.getLastError();
    EXPECT_GT(result.mainChunk.code.size(), 0u);
    EXPECT_GT(result.globalSlotCount, 0);
    EXPECT_GE(result.functionChunks.size(), 1u); // add 函数
    // moduleExports 应包含 x 和 add
    EXPECT_EQ(result.moduleExports.size(), 2u);
    EXPECT_NE(std::find(result.moduleExports.begin(), result.moduleExports.end(), "x"), result.moduleExports.end());
    EXPECT_NE(std::find(result.moduleExports.begin(), result.moduleExports.end(), "add"), result.moduleExports.end());
}

// --- L11 RegisterVM: 序列化往返 ---

TEST(PrecompiledModules, L11_Register_StoreAndLoadRoundTrip) {
    TempDir tmpDir;
    std::string sourcePath = (tmpDir.path() / "rt_reg.mini").string();
    std::string minicPath = (tmpDir.path() / "rt_reg.minic").string();

    std::string modSource = "export var val = 99;\nexport fun ident(x) { return x; }\n";
    writeSourceFile(sourcePath, modSource);

    // 编译 + 存储
    {
        Compiler compiler;
        compiler.setUseRegisterVM(true);
        auto result = compiler.compileModuleViaRegisterIR(modSource, "rt_reg");
        ASSERT_FALSE(compiler.getDiagnostics().hasErrors()) << compiler.getLastError();
        BytecodeCache cache;
        ASSERT_TRUE(cache.storeRegisterToFile(minicPath, result, "rt_reg", sourcePath));
    }

    // 加载
    BytecodeCache cache;
    auto loaded = cache.tryLoadRegisterFromFile(minicPath, sourcePath);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_GT(loaded->mainChunk.code.size(), 0u);
    EXPECT_EQ(loaded->moduleExports.size(), 2u);
    EXPECT_GT(loaded->functionChunks.size(), 0u);
}

// --- L11 RegisterVM: 端到端 import + RegisterVM 执行 ---

TEST(PrecompiledModules, L11_Register_E2E_PrecompiledModuleImport) {
    TempDir tmpDir;
    std::string sourcePath = (tmpDir.path() / "e2e_reg.mini").string();
    std::string minicPath = (tmpDir.path() / "e2e_reg.minic").string();

    std::string modSource = R"(
export var PI = 3;
export fun double(x) { return x * 2; }
export var greeting = "world";
)";
    writeSourceFile(sourcePath, modSource);

    // 1. 预编译模块（RegisterVM 路径）
    {
        Compiler modCompiler;
        modCompiler.setUseRegisterVM(true);
        auto modResult = modCompiler.compileModuleViaRegisterIR(modSource, "e2e_reg");
        ASSERT_FALSE(modCompiler.getDiagnostics().hasErrors()) << "compileModule error: " << modCompiler.getLastError();
        BytecodeCache cache;
        ASSERT_TRUE(cache.storeRegisterToFile(minicPath, modResult, "e2e_reg", sourcePath));
    }

    // 2. 主程序 import + RegisterVM 执行
    std::string mainSource = R"(
import { PI, double, greeting } from "e2e_reg";
print(PI);
print(double(21));
print(greeting);
)";

    std::string output = [&]() {
        Lexer lexer;
        auto tokens = lexer.scan(mainSource);
        Parser parser;
        auto ast = parser.parse(tokens);
        if (!ast)
            return std::string("<parse:null>");
        Compiler compiler;
        compiler.setUseRegisterVM(true);
        compiler.setPrecompiledModuleResolver([&](const std::string& modPath) -> std::string {
            if (modPath == "e2e_reg")
                return minicPath;
            return "";
        });
        // moduleLoader 作为 fallback
        compiler.setModuleLoader([&](const std::string& modPath) -> std::string {
            if (modPath == "e2e_reg")
                return modSource;
            return "";
        });
        try {
            compiler.compile(*ast);
        } catch (const std::exception& e) {
            return std::string("<compile:" + std::string(e.what()) + ">");
        }
        if (compiler.getDiagnostics().hasErrors())
            return "<compile:" + compiler.getLastError() + ">";
        RegisterVM vm;
        std::string captured;
        vm.setOutputCallback([&](const std::string& s) { captured += s; });
        vm.execute(compiler.getLastRegisterResult());
        if (vm.hasError())
            return captured + "<runtime:" + vm.getLastError() + ">";
        return captured;
    }();

    // 验证输出：PI=3, double(21)=42, greeting="world"
    EXPECT_NE(output.find("3"), std::string::npos) << "Output: " << output;
    EXPECT_NE(output.find("42"), std::string::npos) << "Output: " << output;
    EXPECT_NE(output.find("world"), std::string::npos) << "Output: " << output;
    EXPECT_EQ(output.find("<"), std::string::npos) << "Should have no error markers: " << output;
}

// --- L11 RegisterVM: 预编译模块结果与源码编译一致 ---

TEST(PrecompiledModules, L11_Register_E2E_MatchesSourceCompile) {
    TempDir tmpDir;
    std::string sourcePath = (tmpDir.path() / "cmp_reg.mini").string();
    std::string minicPath = (tmpDir.path() / "cmp_reg.minic").string();

    std::string modSource = R"(
export var base = 100;
export fun compute(a, b) { return (a + b) * base; }
)";
    writeSourceFile(sourcePath, modSource);

    // 预编译 .minic
    {
        Compiler modCompiler;
        modCompiler.setUseRegisterVM(true);
        auto modResult = modCompiler.compileModuleViaRegisterIR(modSource, "cmp_reg");
        ASSERT_FALSE(modCompiler.getDiagnostics().hasErrors());
        BytecodeCache cache;
        ASSERT_TRUE(cache.storeRegisterToFile(minicPath, modResult, "cmp_reg", sourcePath));
    }

    std::string mainSource = R"(
import { base, compute } from "cmp_reg";
print(compute(3, 4));
)";

    // 路径 A：源码编译（RegisterVM）
    std::string sourceOutput = [&]() {
        Lexer lexer;
        auto tokens = lexer.scan(mainSource);
        Parser parser;
        auto ast = parser.parse(tokens);
        if (!ast)
            return std::string("<parse:null>");
        Compiler compiler;
        compiler.setUseRegisterVM(true);
        compiler.setModuleLoader([&](const std::string& modPath) -> std::string {
            if (modPath == "cmp_reg")
                return modSource;
            return "";
        });
        compiler.compile(*ast);
        if (compiler.getDiagnostics().hasErrors())
            return "<compile:" + compiler.getLastError() + ">";
        RegisterVM vm;
        std::string out;
        vm.setOutputCallback([&](const std::string& s) { out += s; });
        vm.execute(compiler.getLastRegisterResult());
        if (vm.hasError())
            return out + "<runtime:" + vm.getLastError() + ">";
        return out;
    }();

    // 路径 B：预编译模块（RegisterVM）
    std::string precompiledOutput = [&]() {
        Lexer lexer;
        auto tokens = lexer.scan(mainSource);
        Parser parser;
        auto ast = parser.parse(tokens);
        if (!ast)
            return std::string("<parse:null>");
        Compiler compiler;
        compiler.setUseRegisterVM(true);
        compiler.setPrecompiledModuleResolver([&](const std::string& modPath) -> std::string {
            if (modPath == "cmp_reg")
                return minicPath;
            return "";
        });
        compiler.setModuleLoader([&](const std::string& modPath) -> std::string {
            if (modPath == "cmp_reg")
                return modSource;
            return "";
        });
        compiler.compile(*ast);
        if (compiler.getDiagnostics().hasErrors())
            return "<compile:" + compiler.getLastError() + ">";
        RegisterVM vm;
        std::string out;
        vm.setOutputCallback([&](const std::string& s) { out += s; });
        vm.execute(compiler.getLastRegisterResult());
        if (vm.hasError())
            return out + "<runtime:" + vm.getLastError() + ">";
        return out;
    }();

    EXPECT_EQ(sourceOutput, precompiledOutput) << "Source: " << sourceOutput << "\nPrecompiled: " << precompiledOutput;
    // compute(3, 4) = (3+4)*100 = 700
    EXPECT_EQ(sourceOutput, "700") << "Output: " << sourceOutput;
}

// --- L11 RegisterVM: 非导出函数名隔离 ---

TEST(PrecompiledModules, L11_Register_NonExportedFunctionsAreIsolated) {
    TempDir tmpDir;
    std::string sourcePath = (tmpDir.path() / "iso_reg.mini").string();
    std::string minicPath = (tmpDir.path() / "iso_reg.minic").string();

    // 模块定义非导出 helper 和导出 caller
    std::string modSource = R"(
fun helper() { return 42; }
export fun caller() { return helper(); }
)";
    writeSourceFile(sourcePath, modSource);

    {
        Compiler modCompiler;
        modCompiler.setUseRegisterVM(true);
        auto modResult = modCompiler.compileModuleViaRegisterIR(modSource, "iso_reg");
        ASSERT_FALSE(modCompiler.getDiagnostics().hasErrors()) << modCompiler.getLastError();
        BytecodeCache cache;
        ASSERT_TRUE(cache.storeRegisterToFile(minicPath, modResult, "iso_reg", sourcePath));
    }

    // 主程序也有同名 helper 函数，不应冲突
    std::string mainSource = R"(
fun helper() { return 999; }
import { caller } from "iso_reg";
print(caller());
print(helper());
)";

    std::string output = [&]() {
        Lexer lexer;
        auto tokens = lexer.scan(mainSource);
        Parser parser;
        auto ast = parser.parse(tokens);
        if (!ast)
            return std::string("<parse:null>");
        Compiler compiler;
        compiler.setUseRegisterVM(true);
        compiler.setPrecompiledModuleResolver([&](const std::string& modPath) -> std::string {
            if (modPath == "iso_reg")
                return minicPath;
            return "";
        });
        compiler.setModuleLoader([&](const std::string& modPath) -> std::string {
            if (modPath == "iso_reg")
                return modSource;
            return "";
        });
        compiler.compile(*ast);
        if (compiler.getDiagnostics().hasErrors())
            return "<compile:" + compiler.getLastError() + ">";
        RegisterVM vm;
        std::string out;
        vm.setOutputCallback([&](const std::string& s) { out += s; });
        vm.execute(compiler.getLastRegisterResult());
        if (vm.hasError())
            return out + "<runtime:" + vm.getLastError() + ">";
        return out;
    }();

    // caller() 调用模块的 helper → 42
    // 主程序 helper() → 999
    EXPECT_EQ(output, "42999") << "Output: " << output;
    EXPECT_EQ(output.find("<"), std::string::npos) << "No error markers: " << output;
}
