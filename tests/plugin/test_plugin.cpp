// ============================================================
// tests/plugin/test_plugin.cpp — 七特性 MVP 阶段 7 测试插件
// ------------------------------------------------------------
// 编译为 SHARED 库（minilang_test_plugin），导出 minilang_plugin_init，
// 供 TestPlugin.cpp 的 minilang_load_plugin 加载测试。
// 插件在 init 中用 minilang_register_function 注册 mini_add(a, b)。
// ============================================================
#include "capi/minilang_capi.h"

// 宿主函数：mini_add(a, b) → a + b（int 相加）
static minilang_value mini_add(const minilang_value* args, int argc, void* /*userdata*/) {
    minilang_value r;
    r.type = MINILANG_VAL_INT;
    r.b = 0;
    r.i = 0;
    r.d = 0.0;
    r.s = nullptr;
    if (argc == 2 && args[0].type == MINILANG_VAL_INT && args[1].type == MINILANG_VAL_INT) {
        r.i = args[0].i + args[1].i;
    }
    return r;
}

// 插件初始化入口（导出符号）。加载器加载后调用，传入 ctx + 注册回调；
// 插件用回调注册能力，无需在链接期依赖 minilang 符号（Windows DLL 要求）。
#if defined(_WIN32)
extern "C" __declspec(dllexport) void minilang_plugin_init(minilang_context* ctx, minilang_register_fn register_fn) {
#else
extern "C" void minilang_plugin_init(minilang_context* ctx, minilang_register_fn register_fn) {
#endif
    register_fn(ctx, "mini_add", &mini_add, nullptr);
}
