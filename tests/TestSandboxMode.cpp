// ============================================================
// TestSandboxMode.cpp — 七特性 MVP 阶段 6：能力级沙箱模式
// ------------------------------------------------------------
// RuntimeConfig 新增沙箱开关（sandboxEnabled/AllowInput/AllowImport）。
// 拦截点：
//   - input：executeSharedInput 共享层单点（三后端覆盖）
//   - import：三后端 import 路径仅放行 std/ 内建模块
// 每用例结束 resetToDefaults 防止全局单例状态串扰后续测试。
// C API：minilang_set_sandbox 开关生效验证。
// ============================================================

#include "capi/minilang_capi.h"
#include "common/BuiltinModules.h"
#include "common/RuntimeLimits.h"
#include "compiler/Compiler.h"
#include "compiler/RegisterVM.h"
#include "compiler/VM.h"
#include "interpreter/Interpreter.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include <gtest/gtest.h>
#include <string>

namespace {

std::string sandboxLoader(const std::string& modulePath) {
    if (BuiltinModuleRegistry::isBuiltinModule(modulePath)) {
        return BuiltinModuleRegistry::getSource(modulePath);
    }
    // 模拟一个文件模块 "mymod"（沙箱应拦截，非沙箱应放行）
    if (modulePath == "mymod") {
        return "export fun greet() { return \"hi\"; }";
    }
    return "";
}

// 带 input 回调 + moduleLoader 的四后端执行辅助
struct RunResult {
    std::string output;
    bool ok = true;
};

RunResult runInterp(const std::string& src, const std::string& inputReply) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    RunResult r;
    if (!ast) {
        r.ok = false;
        return r;
    }
    Interpreter interp;
    interp.setModuleLoader(sandboxLoader);
    interp.setInputCallback([&](const std::string&) { return inputReply; });
    interp.setOutputCallback([&](const std::string& s) { r.output += s; });
    try {
        interp.execute(*ast);
    } catch (const std::exception& e) {
        r.ok = false;
        r.output += std::string("<err:") + e.what() + ">";
    }
    return r;
}

RunResult runVM(const std::string& src, bool useRegVM) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    RunResult r;
    if (!ast) {
        r.ok = false;
        return r;
    }
    Compiler c;
    if (useRegVM)
        c.setUseRegisterVM(true);
    else
        c.setUseIR(true);
    c.setModuleLoader(sandboxLoader);
    auto cr = c.compile(*ast);
    if (c.getDiagnostics().hasErrors()) {
        r.ok = false;
        r.output = "<compile:" + c.getLastError() + ">";
        return r;
    }
    if (useRegVM) {
        RegisterVM vm;
        vm.setOutputCallback([&](const std::string& s) { r.output += s; });
        vm.execute(c.getLastRegisterResult());
        if (vm.hasError()) {
            r.ok = false;
            r.output += "<runtime:" + vm.getLastError() + ">";
        }
    } else {
        VM vm;
        vm.setOutputCallback([&](const std::string& s) { r.output += s; });
        vm.execute(cr);
        if (vm.hasError()) {
            r.ok = false;
            r.output += "<runtime:" + vm.getLastError() + ">";
        }
    }
    return r;
}

// 测试夹具：每个用例结束重置 RuntimeConfig（含沙箱开关）
class SandboxModeTest : public ::testing::Test {
protected:
    void TearDown() override { RuntimeLimits::RuntimeConfig::instance().resetToDefaults(); }
};

} // namespace

// ============================================================
// 1. 默认（非沙箱）：input / 文件 import 均放行
// ============================================================

TEST_F(SandboxModeTest, DefaultAllowsInput) {
    auto r = runInterp("print(input(\"?\"));", "hello");
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.output, "hello");
}

TEST_F(SandboxModeTest, DefaultAllowsFileImport) {
    auto r = runInterp("import { greet } from \"mymod\"; print(greet());", "");
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.output, "hi");
}

// ============================================================
// 2. 沙箱开启：input 被拒（三后端一致）
// ============================================================

TEST_F(SandboxModeTest, SandboxBlocksInputInterpreter) {
    RuntimeLimits::RuntimeConfig::instance().setSandboxEnabled(true);
    RuntimeLimits::RuntimeConfig::instance().setSandboxAllowInput(false);
    auto r = runInterp("print(input(\"?\"));", "hello");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.output.find("沙箱"), std::string::npos) << r.output;
}

TEST_F(SandboxModeTest, SandboxBlocksInputStackVM) {
    RuntimeLimits::RuntimeConfig::instance().setSandboxEnabled(true);
    RuntimeLimits::RuntimeConfig::instance().setSandboxAllowInput(false);
    auto r = runVM("print(input(\"?\"));", false);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.output.find("沙箱"), std::string::npos) << r.output;
}

TEST_F(SandboxModeTest, SandboxBlocksInputRegisterVM) {
    RuntimeLimits::RuntimeConfig::instance().setSandboxEnabled(true);
    RuntimeLimits::RuntimeConfig::instance().setSandboxAllowInput(false);
    auto r = runVM("print(input(\"?\"));", true);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.output.find("沙箱"), std::string::npos) << r.output;
}

// ============================================================
// 3. 沙箱开启：文件 import 被拒，std/ 内建模块放行（三后端一致）
// ============================================================

TEST_F(SandboxModeTest, SandboxBlocksFileImportInterpreter) {
    RuntimeLimits::RuntimeConfig::instance().setSandboxEnabled(true);
    RuntimeLimits::RuntimeConfig::instance().setSandboxAllowImport(false);
    auto r = runInterp("import { greet } from \"mymod\"; print(greet());", "");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.output.find("沙箱"), std::string::npos) << r.output;
}

TEST_F(SandboxModeTest, SandboxBlocksFileImportStackVM) {
    RuntimeLimits::RuntimeConfig::instance().setSandboxEnabled(true);
    RuntimeLimits::RuntimeConfig::instance().setSandboxAllowImport(false);
    auto r = runVM("import { greet } from \"mymod\"; print(greet());", false);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.output.find("沙箱"), std::string::npos) << r.output;
}

TEST_F(SandboxModeTest, SandboxBlocksFileImportRegisterVM) {
    RuntimeLimits::RuntimeConfig::instance().setSandboxEnabled(true);
    RuntimeLimits::RuntimeConfig::instance().setSandboxAllowImport(false);
    auto r = runVM("import { greet } from \"mymod\"; print(greet());", true);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.output.find("沙箱"), std::string::npos) << r.output;
}

TEST_F(SandboxModeTest, SandboxAllowsStdModuleInterpreter) {
    RuntimeLimits::RuntimeConfig::instance().setSandboxEnabled(true);
    RuntimeLimits::RuntimeConfig::instance().setSandboxAllowImport(false);
    // std/math 是内建模块，沙箱下仍放行（pow 是 std/math 导出函数）
    auto r = runInterp("import { pow } from \"std/math\"; print(pow(2, 3));", "");
    EXPECT_TRUE(r.ok) << r.output;
    EXPECT_EQ(r.output, "8");
}

// ============================================================
// 4. 关闭沙箱后恢复放行（验证 resetToDefaults / 开关切换）
// ============================================================

TEST_F(SandboxModeTest, DisablingSandboxRestoresInput) {
    auto& cfg = RuntimeLimits::RuntimeConfig::instance();
    cfg.setSandboxEnabled(true);
    cfg.setSandboxAllowInput(false);
    auto blocked = runInterp("print(input(\"?\"));", "x");
    EXPECT_FALSE(blocked.ok);
    // 关闭沙箱
    cfg.setSandboxEnabled(false);
    auto allowed = runInterp("print(input(\"?\"));", "x");
    EXPECT_TRUE(allowed.ok);
    EXPECT_EQ(allowed.output, "x");
}

// ============================================================
// 5. C API：minilang_set_sandbox 开关生效
// ============================================================

TEST_F(SandboxModeTest, CApiSandboxBlocksImport) {
    minilang_context* ctx = minilang_create();
    ASSERT_NE(ctx, nullptr);
    minilang_set_sandbox(ctx, 1);
    // 文件 import 在沙箱下应失败（C API 走 BackendExecutionService，
    // 其内部 moduleLoader 拦截 std/；文件模块 "mymod" 无源码，但沙箱先于加载拦截）
    minilang_status st = minilang_eval(ctx, "import { x } from \"mymod\";");
    EXPECT_NE(st, MINILANG_OK);
    std::string err = minilang_get_error(ctx);
    EXPECT_NE(err.find("沙箱"), std::string::npos) << err;
    minilang_destroy(ctx);
}

TEST_F(SandboxModeTest, CApiSandboxDisabledByDefault) {
    minilang_context* ctx = minilang_create();
    ASSERT_NE(ctx, nullptr);
    // 默认非沙箱：普通算术正常执行
    minilang_status st = minilang_eval(ctx, "print(1 + 2);");
    EXPECT_EQ(st, MINILANG_OK);
    EXPECT_EQ(std::string(minilang_get_output(ctx)), "3");
    minilang_destroy(ctx);
}

TEST_F(SandboxModeTest, CApiSandboxToggleRestoresGlobalConfig) {
    // eval 后全局 RuntimeConfig 应恢复（不污染后续非 C API 执行）
    minilang_context* ctx = minilang_create();
    ASSERT_NE(ctx, nullptr);
    minilang_set_sandbox(ctx, 1);
    minilang_eval(ctx, "print(1);");
    // eval 结束后全局开关应已恢复为 false
    EXPECT_FALSE(RuntimeLimits::RuntimeConfig::instance().sandboxEnabled());
    minilang_destroy(ctx);
}
