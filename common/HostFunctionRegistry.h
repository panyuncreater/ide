/**
 * @file common/HostFunctionRegistry.h
 * @brief 七特性 MVP 阶段 7：宿主函数注册表——插件/嵌入方注册的原生 C 函数。
 *
 * 设计：
 *   - core 层中立类型 HostValue（tagged union：null/bool/int64/double/utf8 string），
 *     避免 core 反向依赖 capi 头（capi 依赖 core）。
 *   - HostFn = HostValue(*)(const HostValue* args, int argc, void* userdata)。
 *   - 全局单例注册表（name → fn+userdata），三后端在"未定义函数"回退路径查表调用。
 *   - Value ↔ HostValue 转换器仅支持 5 种标量（容器参数报错，见 BuiltinMethods 调用点）。
 *
 * 线程模型：注册通常在 eval 前（单线程），执行时只读查表。atomic/mutex 非必需
 * （MVP 假设注册与执行不并发；GUI 场景由宿主保证时序）。
 *
 * @see capi/minilang_capi.h（minilang_register_function / minilang_load_plugin）
 * @see interpreter/BuiltinMethods.cpp（isBuiltinFunction / executeSharedBuiltinFunction 查表接入）
 */
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace minilang {

/// 宿主互操作值类型标签（与 capi minilang_value 对齐的 5 种标量）
enum class HostValueType : int {
    Null = 0,
    Bool = 1,
    Int = 2,
    Double = 3,
    String = 4,
};

/// 宿主互操作值（tagged union，仅标量）。
/// string 用 std::string 持有（跨 C ABI 时由 capi 层转换为 const char*）。
struct HostValue {
    HostValueType type = HostValueType::Null;
    bool b = false;
    int64_t i = 0;
    double d = 0.0;
    std::string s;

    static HostValue makeNull() { return HostValue{}; }
    static HostValue makeBool(bool v) {
        HostValue hv;
        hv.type = HostValueType::Bool;
        hv.b = v;
        return hv;
    }
    static HostValue makeInt(int64_t v) {
        HostValue hv;
        hv.type = HostValueType::Int;
        hv.i = v;
        return hv;
    }
    static HostValue makeDouble(double v) {
        HostValue hv;
        hv.type = HostValueType::Double;
        hv.d = v;
        return hv;
    }
    static HostValue makeString(std::string v) {
        HostValue hv;
        hv.type = HostValueType::String;
        hv.s = std::move(v);
        return hv;
    }
};

/// 宿主函数签名：接收标量参数数组，返回标量结果。
/// userdata 由注册方提供，回调时原样传回（无所有权，宿主负责生命周期）。
using HostFn = HostValue (*)(const HostValue* args, int argc, void* userdata);

/// 宿主函数注册表（全局单例）。
class HostFunctionRegistry {
public:
    static HostFunctionRegistry& instance();

    /// 注册宿主函数（同名覆盖）。name 为 MiniLang 侧可调用的函数名。
    void registerFunction(const std::string& name, HostFn fn, void* userdata);

    /// 查询是否已注册（供 isBuiltinFunction 接入）。
    bool has(const std::string& name) const;

    /// 调用宿主函数。调用方需先 has() 确认存在。
    /// @param found [out] 是否命中（未命中时返回值为 Null）
    HostValue call(const std::string& name, const HostValue* args, int argc, bool* found) const;

    /// 清空注册表（测试 tearDown / context 销毁时调用）。
    void clear();

private:
    HostFunctionRegistry() = default;
    HostFunctionRegistry(const HostFunctionRegistry&) = delete;
    HostFunctionRegistry& operator=(const HostFunctionRegistry&) = delete;

    struct Entry {
        HostFn fn = nullptr;
        void* userdata = nullptr;
    };
    std::unordered_map<std::string, Entry> functions_;
};

} // namespace minilang
