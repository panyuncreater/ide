// ============================================================
// TestPlugin.cpp — 七特性 MVP 阶段 7：插件系统（宿主函数 + DLL 加载）
// ------------------------------------------------------------
// 覆盖：
//   1. 宿主函数注册后三后端可调用（经 isBuiltinFunction + executeSharedBuiltinFunction）
//   2. 参数/返回值 5 种标量往返（null/bool/int/double/string）
//   3. minilang_load_plugin 加载测试插件 DLL 并调用其注册的 mini_add
//   4. 加载失败路径（不存在的库）
//   5. 沙箱模式拒绝插件加载
// 注：每用例结束 clear() 宿主注册表防止全局单例串扰。
// ============================================================

#include "capi/minilang_capi.h"
#include "common/HostFunctionRegistry.h"
#include "compiler/Compiler.h"
#include "compiler/RegisterVM.h"
#include "compiler/VM.h"
#include "interpreter/Interpreter.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include <gtest/gtest.h>
#include <string>

// 测试插件 DLL 路径由 CMake 通过编译定义注入（TARGET_FILE 生成表达式）
#ifndef MINILANG_TEST_PLUGIN_PATH
#define MINILANG_TEST_PLUGIN_PATH ""
#endif

namespace {

// ---- 宿主函数样例（供注册表直接测试） ----

// double_it(x): int → int，返回 x*2
minilang::HostValue hostDoubleIt(const minilang::HostValue* args, int argc, void* /*ud*/) {
    if (argc == 1 && args[0].type == minilang::HostValueType::Int) {
        return minilang::HostValue::makeInt(args[0].i * 2);
    }
    return minilang::HostValue::makeNull();
}

// echo_types(...): 返回描述参数类型的字符串（验证 5 种标量入参）
minilang::HostValue hostEchoTypes(const minilang::HostValue* args, int argc, void* /*ud*/) {
    std::string desc;
    for (int i = 0; i < argc; ++i) {
        switch (args[i].type) {
        case minilang::HostValueType::Null: desc += "N"; break;
        case minilang::HostValueType::Bool: desc += args[i].b ? "T" : "F"; break;
        case minilang::HostValueType::Int: desc += "I"; break;
        case minilang::HostValueType::Double: desc += "D"; break;
        case minilang::HostValueType::String: desc += "S"; break;
        }
    }
    return minilang::HostValue::makeString(desc);
}

// userdata 透传验证：返回 *(int*)userdata
minilang::HostValue hostReadUserData(const minilang::HostValue* /*args*/, int /*argc*/, void* ud) {
    return minilang::HostValue::makeInt(*static_cast<int*>(ud));
}

// 四后端执行辅助（返回 print 输出，失败返回 <err>）
std::string runInterp(const std::string& src) {
    Lexer lx; auto tk = lx.scan(src);
    Parser p; auto ast = p.parse(tk);
    if (!ast) return "<parse-fail>";
    Interpreter interp;
    std::string out;
    interp.setOutputCallback([&](const std::string& s) { out += s; });
    try { interp.execute(*ast); }
    catch (const std::exception& e) { return out + "<err:" + e.what() + ">"; }
    return out;
}
std::string runStackVM(const std::string& src, bool useIR) {
    Lexer lx; auto tk = lx.scan(src);
    Parser p; auto ast = p.parse(tk);
    if (!ast) return "<parse-fail>";
    Compiler c; c.setUseIR(useIR);
    auto cr = c.compile(*ast);
    if (c.getDiagnostics().hasErrors()) return "<compile:" + c.getLastError() + ">";
    VM vm; std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });
    vm.execute(cr);
    if (vm.hasError()) return out + "<runtime:" + vm.getLastError() + ">";
    return out;
}
std::string runRegVM(const std::string& src) {
    Lexer lx; auto tk = lx.scan(src);
    Parser p; auto ast = p.parse(tk);
    if (!ast) return "<parse-fail>";
    Compiler c; c.setUseRegisterVM(true);
    c.compile(*ast);
    if (c.getDiagnostics().hasErrors()) return "<compile:" + c.getLastError() + ">";
    RegisterVM vm; std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });
    vm.execute(c.getLastRegisterResult());
    if (vm.hasError()) return out + "<runtime:" + vm.getLastError() + ">";
    return out;
}

class PluginTest : public ::testing::Test {
protected:
    void TearDown() override { minilang::HostFunctionRegistry::instance().clear(); }
};

} // namespace

// ============================================================
// 1. 宿主函数注册后四后端可调用
// ============================================================

TEST_F(PluginTest, HostFunctionCallableAllBackends) {
    minilang::HostFunctionRegistry::instance().registerFunction("double_it", &hostDoubleIt, nullptr);
    std::string src = "print(double_it(21));";
    EXPECT_EQ(runInterp(src), "42");
    EXPECT_EQ(runStackVM(src, false), "42");
    EXPECT_EQ(runStackVM(src, true), "42");
    EXPECT_EQ(runRegVM(src), "42");
}

// ============================================================
// 2. 5 种标量参数往返
// ============================================================

TEST_F(PluginTest, ScalarArgTypesRoundTrip) {
    minilang::HostFunctionRegistry::instance().registerFunction("echo_types", &hostEchoTypes, nullptr);
    // null / bool / int / double / string 各一个
    std::string src = "print(echo_types(null, true, 1, 2.5, \"x\"));";
    EXPECT_EQ(runInterp(src), "NTIDS");
    EXPECT_EQ(runStackVM(src, false), "NTIDS");
    EXPECT_EQ(runStackVM(src, true), "NTIDS");
    EXPECT_EQ(runRegVM(src), "NTIDS");
}

TEST_F(PluginTest, StringReturnValue) {
    minilang::HostFunctionRegistry::instance().registerFunction(
        "greet", [](const minilang::HostValue*, int, void*) { return minilang::HostValue::makeString("hello"); },
        nullptr);
    EXPECT_EQ(runInterp("print(greet());"), "hello");
    EXPECT_EQ(runRegVM("print(greet());"), "hello");
}

TEST_F(PluginTest, UserdataPassthrough) {
    static int magic = 777;
    minilang::HostFunctionRegistry::instance().registerFunction("read_ud", &hostReadUserData, &magic);
    EXPECT_EQ(runInterp("print(read_ud());"), "777");
    EXPECT_EQ(runStackVM("print(read_ud());", true), "777");
}

// ============================================================
// 3. 容器参数报错（MVP 仅支持标量）
// ============================================================

TEST_F(PluginTest, ContainerArgRejected) {
    minilang::HostFunctionRegistry::instance().registerFunction("double_it", &hostDoubleIt, nullptr);
    // 传数组 → 宿主函数参数类型错误（四后端一致报错）
    std::string src = "double_it([1, 2]);";
    EXPECT_NE(runInterp(src).find("标量"), std::string::npos);
}

// ============================================================
// 4. 用户自定义函数优先于宿主函数
// ============================================================

TEST_F(PluginTest, UserFunctionOverridesHost) {
    minilang::HostFunctionRegistry::instance().registerFunction("double_it", &hostDoubleIt, nullptr);
    // 用户定义同名函数 → 优先调用用户函数（返回 x+100 而非 x*2）
    std::string src = "fun double_it(x) { return x + 100; } print(double_it(5));";
    EXPECT_EQ(runInterp(src), "105");
}

// ============================================================
// 5. C API：minilang_register_function 直接注册
// ============================================================

namespace {
minilang_value capiTriple(const minilang_value* args, int argc, void*) {
    minilang_value r; r.type = MINILANG_VAL_INT; r.b = 0; r.i = 0; r.d = 0.0; r.s = nullptr;
    if (argc == 1 && args[0].type == MINILANG_VAL_INT) r.i = args[0].i * 3;
    return r;
}
} // namespace

TEST_F(PluginTest, CApiRegisterFunction) {
    minilang_context* ctx = minilang_create();
    ASSERT_NE(ctx, nullptr);
    ASSERT_EQ(minilang_register_function(ctx, "triple", &capiTriple, nullptr), MINILANG_OK);
    ASSERT_EQ(minilang_eval(ctx, "print(triple(14));"), MINILANG_OK);
    EXPECT_EQ(std::string(minilang_get_output(ctx)), "42");
    minilang_destroy(ctx);
}

TEST_F(PluginTest, CApiRegisterNullArgsRejected) {
    minilang_context* ctx = minilang_create();
    ASSERT_NE(ctx, nullptr);
    EXPECT_EQ(minilang_register_function(ctx, nullptr, &capiTriple, nullptr), MINILANG_ERR_INVALID);
    EXPECT_EQ(minilang_register_function(ctx, "x", nullptr, nullptr), MINILANG_ERR_INVALID);
    minilang_destroy(ctx);
}

// ============================================================
// 6. DLL 加载：加载测试插件并调用 mini_add
// ============================================================

TEST_F(PluginTest, LoadPluginAndCall) {
    const char* pluginPath = MINILANG_TEST_PLUGIN_PATH;
    if (std::string(pluginPath).empty()) {
        GTEST_SKIP() << "测试插件路径未注入（MINILANG_TEST_PLUGIN_PATH 为空）";
    }
    minilang_context* ctx = minilang_create();
    ASSERT_NE(ctx, nullptr);
    minilang_status st = minilang_load_plugin(ctx, pluginPath);
    ASSERT_EQ(st, MINILANG_OK) << minilang_get_error(ctx);
    // 插件注册了 mini_add(a, b)
    ASSERT_EQ(minilang_eval(ctx, "print(mini_add(40, 2));"), MINILANG_OK) << minilang_get_error(ctx);
    EXPECT_EQ(std::string(minilang_get_output(ctx)), "42");
    minilang_destroy(ctx);
}

// ============================================================
// 7. 加载失败路径
// ============================================================

TEST_F(PluginTest, LoadNonexistentPluginFails) {
    minilang_context* ctx = minilang_create();
    ASSERT_NE(ctx, nullptr);
    minilang_status st = minilang_load_plugin(ctx, "no_such_plugin_xyz.dll");
    EXPECT_EQ(st, MINILANG_ERR_INVALID);
    EXPECT_NE(std::string(minilang_get_error(ctx)).find("无法加载"), std::string::npos);
    minilang_destroy(ctx);
}

// ============================================================
// 8. 沙箱模式拒绝插件加载
// ============================================================

TEST_F(PluginTest, SandboxRejectsPluginLoad) {
    minilang_context* ctx = minilang_create();
    ASSERT_NE(ctx, nullptr);
    minilang_set_sandbox(ctx, 1);
    minilang_status st = minilang_load_plugin(ctx, MINILANG_TEST_PLUGIN_PATH);
    EXPECT_EQ(st, MINILANG_ERR_INVALID);
    EXPECT_NE(std::string(minilang_get_error(ctx)).find("沙箱"), std::string::npos);
    minilang_destroy(ctx);
}
