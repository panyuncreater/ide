// ============================================================
// minilang_capi.cpp — libminilang 嵌入式 C API 实现（拓展二期·平台）
// ------------------------------------------------------------
// 薄封装层：把 C ABI 映射到 BackendExecutionService::execute
// （ARCH-10 统一执行中间层，与 GUI 教学面板共用同一条
// Lexer → Parser → Compiler → Backend 流程，语义天然一致）。
//
// 设计要点：
//   - minilang_context 内部只存后端选择 + 上次执行的输出/错误快照，
//     不持有引擎对象（每次 eval 由服务层创建独立局部对象）
//   - 错误分类：BackendExecResult.errorPrefix 含"运行时错误"→
//     MINILANG_ERR_RUNTIME，其余（词法/语法/编译）→ MINILANG_ERR_COMPILE
//   - 所有入口 NULL 安全（返回 MINILANG_ERR_INVALID / 空串）
// ============================================================

#include "capi/minilang_capi.h"

#include "common/BackendExecutionService.h"
#include "common/HostFunctionRegistry.h"
#include "common/RuntimeLimits.h"

#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

/// 不透明句柄的真实定义（仅本 TU 可见完整布局）
struct minilang_context {
    BackendType backend = BackendType::Interpreter;
    std::string lastOutput;
    std::string lastError;
    // 七特性 MVP 阶段 6：沙箱模式开关（eval 时应用到全局 RuntimeConfig）
    bool sandbox = false;
    // 七特性 MVP 阶段 7：已加载的插件库句柄（destroy 时释放）
#if defined(_WIN32)
    std::vector<HMODULE> pluginHandles;
#else
    std::vector<void*> pluginHandles;
#endif
};

// 七特性 MVP 阶段 7：插件初始化入口签名（插件导出此符号）。
// 传入 ctx + 注册回调，使插件无需在链接期依赖 minilang 符号。
typedef void (*minilang_plugin_init_fn)(minilang_context*, minilang_register_fn);

// ============================================================
// 七特性 MVP 阶段 7：插件宿主函数转接器（C++ 链接，置于 extern "C" 外）
// ------------------------------------------------------------
// 这些 helper 返回 C++ 类型 HostValue（含成员函数），必须为 C++ 链接
//（置于 extern "C" 内会触发 C4190）。
// ============================================================
namespace {
// minilang_value → HostValue（C ABI 结构 → core 中立类型）
minilang::HostValue toHostValue(const minilang_value& v) {
    switch (v.type) {
    case MINILANG_VAL_BOOL:
        return minilang::HostValue::makeBool(v.b != 0);
    case MINILANG_VAL_INT:
        return minilang::HostValue::makeInt(static_cast<int64_t>(v.i));
    case MINILANG_VAL_DOUBLE:
        return minilang::HostValue::makeDouble(v.d);
    case MINILANG_VAL_STRING:
        return minilang::HostValue::makeString(v.s ? std::string(v.s) : std::string());
    case MINILANG_VAL_NULL:
    default:
        return minilang::HostValue::makeNull();
    }
}

// C API 宿主函数 → core HostFn 的转接器：
// core 侧回调传入 HostValue*，需转为 minilang_value* 再调用用户 fn，
// 用户 fn 返回 minilang_value 再转回 HostValue。
// HostFn 是普通函数指针无法捕获，故将用户 fn + userdata 打包到堆上 HostThunk，
// 以其地址作为 core 侧 userdata。
struct HostThunk {
    minilang_host_fn userFn;
    void* userData;
};

minilang::HostValue hostFnShim(const minilang::HostValue* args, int argc, void* userdata) {
    auto* thunk = static_cast<HostThunk*>(userdata);
    std::vector<minilang_value> cargs;
    cargs.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        minilang_value cv;
        cv.type = MINILANG_VAL_NULL;
        cv.b = 0;
        cv.i = 0;
        cv.d = 0.0;
        cv.s = nullptr;
        switch (args[i].type) {
        case minilang::HostValueType::Bool:
            cv.type = MINILANG_VAL_BOOL;
            cv.b = args[i].b ? 1 : 0;
            break;
        case minilang::HostValueType::Int:
            cv.type = MINILANG_VAL_INT;
            cv.i = static_cast<long long>(args[i].i);
            break;
        case minilang::HostValueType::Double:
            cv.type = MINILANG_VAL_DOUBLE;
            cv.d = args[i].d;
            break;
        case minilang::HostValueType::String:
            cv.type = MINILANG_VAL_STRING;
            cv.s = args[i].s.c_str();
            break;
        case minilang::HostValueType::Null:
        default:
            break;
        }
        cargs.push_back(cv);
    }
    minilang_value result = thunk->userFn(cargs.data(), argc, thunk->userData);
    return toHostValue(result);
}
} // namespace

extern "C" {

minilang_context* minilang_create(void) {
    // 嵌入方无异常语义约定：分配失败返回 NULL 而非抛 bad_alloc
    return new (std::nothrow) minilang_context();
}

void minilang_destroy(minilang_context* ctx) {
    // 七特性 MVP 阶段 7：释放已加载的插件库句柄
    if (ctx) {
        for (auto handle : ctx->pluginHandles) {
            if (handle) {
#if defined(_WIN32)
                FreeLibrary(handle);
#else
                dlclose(handle);
#endif
            }
        }
    }
    delete ctx;
}

minilang_status minilang_set_backend(minilang_context* ctx, minilang_backend backend) {
    if (!ctx)
        return MINILANG_ERR_INVALID;
    switch (backend) {
    case MINILANG_BACKEND_INTERPRETER:
        ctx->backend = BackendType::Interpreter;
        return MINILANG_OK;
    case MINILANG_BACKEND_STACKVM:
        ctx->backend = BackendType::StackVM_IR;
        return MINILANG_OK;
    case MINILANG_BACKEND_REGISTERVM:
        ctx->backend = BackendType::RegisterVM_IR;
        return MINILANG_OK;
    }
    return MINILANG_ERR_INVALID;
}

minilang_status minilang_set_sandbox(minilang_context* ctx, int enabled) {
    if (!ctx)
        return MINILANG_ERR_INVALID;
    ctx->sandbox = (enabled != 0);
    return MINILANG_OK;
}

minilang_status minilang_eval(minilang_context* ctx, const char* source) {
    if (!ctx || !source)
        return MINILANG_ERR_INVALID;
    ctx->lastOutput.clear();
    ctx->lastError.clear();

    // 七特性 MVP 阶段 6：沙箱模式——eval 前设置全局 RuntimeConfig、eval 后恢复。
    // RuntimeConfig 是单例（跨 context 共享），保存旧值保证不污染其他同线程 context。
    auto& cfg = RuntimeLimits::RuntimeConfig::instance();
    bool savedEnabled = cfg.sandboxEnabled();
    bool savedInput = cfg.sandboxAllowInput();
    bool savedImport = cfg.sandboxAllowImport();
    if (ctx->sandbox) {
        cfg.setSandboxEnabled(true);
        cfg.setSandboxAllowInput(false);
        cfg.setSandboxAllowImport(false);
    }

    BackendExecResult result = BackendExecutionService::execute(source, ctx->backend);

    // 恢复沙箱开关（无论是否启用，均回写旧值，确保幂等）
    cfg.setSandboxEnabled(savedEnabled);
    cfg.setSandboxAllowInput(savedInput);
    cfg.setSandboxAllowImport(savedImport);

    ctx->lastOutput = result.output;
    if (result.success) {
        return MINILANG_OK;
    }
    // 错误消息带阶段前缀（"运行时错误: ..." / "语法错误: ..."），
    // 与 CLI/GUI 呈现口径一致
    ctx->lastError = result.errorPrefix.empty() ? result.errorMsg : (result.errorPrefix + ": " + result.errorMsg);
    return (result.errorPrefix == "运行时错误") ? MINILANG_ERR_RUNTIME : MINILANG_ERR_COMPILE;
}

const char* minilang_get_output(const minilang_context* ctx) {
    return ctx ? ctx->lastOutput.c_str() : "";
}

const char* minilang_get_error(const minilang_context* ctx) {
    return ctx ? ctx->lastError.c_str() : "";
}

minilang_backend minilang_get_backend(const minilang_context* ctx) {
    if (!ctx)
        return MINILANG_BACKEND_INTERPRETER;
    switch (ctx->backend) {
    case BackendType::StackVM_IR:
        return MINILANG_BACKEND_STACKVM;
    case BackendType::RegisterVM_IR:
        return MINILANG_BACKEND_REGISTERVM;
    case BackendType::Interpreter:
    default:
        return MINILANG_BACKEND_INTERPRETER;
    }
}

const char* minilang_version(void) {
    return "libminilang 0.1.0";
}

// ============================================================
// 七特性 MVP 阶段 7：插件系统
// ============================================================

minilang_status minilang_register_function(minilang_context* ctx, const char* name, minilang_host_fn fn,
                                           void* userdata) {
    if (!ctx || !name || !fn)
        return MINILANG_ERR_INVALID;
    // 将用户 fn+userdata 打包为堆上 HostThunk（生命周期随进程；MVP 不回收，
    // 与全局 HostFunctionRegistry 一致，注册通常在进程级一次性完成）。
    auto* thunk = new HostThunk{fn, userdata};
    minilang::HostFunctionRegistry::instance().registerFunction(name, &hostFnShim, thunk);
    return MINILANG_OK;
}

minilang_status minilang_load_plugin(minilang_context* ctx, const char* path) {
    if (!ctx || !path)
        return MINILANG_ERR_INVALID;
    ctx->lastError.clear();
    // 七特性 MVP 阶段 6/7：沙箱模式下拒绝插件加载
    if (ctx->sandbox) {
        ctx->lastError = "沙箱模式禁止加载插件";
        return MINILANG_ERR_INVALID;
    }
#if defined(_WIN32)
    // UTF-8 path → 宽字符（LoadLibraryW）。简化：用 MultiByteToWideChar。
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    if (wlen <= 0) {
        ctx->lastError = "插件路径编码转换失败";
        return MINILANG_ERR_INVALID;
    }
    std::wstring wpath(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath.data(), wlen);
    HMODULE handle = LoadLibraryW(wpath.c_str());
    if (!handle) {
        ctx->lastError = std::string("无法加载插件库: ") + path;
        return MINILANG_ERR_INVALID;
    }
    auto initFn = reinterpret_cast<minilang_plugin_init_fn>(
        reinterpret_cast<void*>(GetProcAddress(handle, "minilang_plugin_init")));
    if (!initFn) {
        FreeLibrary(handle);
        ctx->lastError = "插件缺少导出符号 minilang_plugin_init";
        return MINILANG_ERR_INVALID;
    }
    initFn(ctx, &minilang_register_function);
    ctx->pluginHandles.push_back(handle);
    return MINILANG_OK;
#else
    void* handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char* err = dlerror();
        ctx->lastError = std::string("无法加载插件库: ") + (err ? err : path);
        return MINILANG_ERR_INVALID;
    }
    auto initFn = reinterpret_cast<minilang_plugin_init_fn>(dlsym(handle, "minilang_plugin_init"));
    if (!initFn) {
        dlclose(handle);
        ctx->lastError = "插件缺少导出符号 minilang_plugin_init";
        return MINILANG_ERR_INVALID;
    }
    initFn(ctx, &minilang_register_function);
    ctx->pluginHandles.push_back(handle);
    return MINILANG_OK;
#endif
}

} // extern "C"
