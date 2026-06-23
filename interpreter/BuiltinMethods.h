#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "interpreter/Value.h"

// Forward declaration - RuntimeError is defined in Interpreter.h
class RuntimeError;

// ============================================================
// BuiltinMethods - 内置方法分发辅助类
// ============================================================
// 处理数组、字典、字符串的内置方法调用。
// 从 Interpreter::visitMethodCall 中提取，降低 Interpreter.cpp 复杂度。
// 错误通过直接抛出 RuntimeError 传达（与 Interpreter::runtimeError 语义一致）。

/// 内置方法调用结果
struct BuiltinMethodResult {
    Value result;              // 方法返回值
    bool objectModified;       // 对象是否被修改（需要 writeBack）

    BuiltinMethodResult() : result(Value::nullValue()), objectModified(false) {}
    BuiltinMethodResult(Value r, bool modified = false)
        : result(std::move(r)), objectModified(modified) {}
};

// ============================================================
// 共享纯函数层（供 Interpreter 和 VM 共用，不抛异常）
// ============================================================
// 保守重构：目前仅提取 len / contains / has 三个最简单、最常用的方法
// 作为共享纯函数，消除 Interpreter 与 VM 间的双重实现。
// 其余内置方法（push/pop/remove/keys/values/get/join/upper/lower/split/
// replace/trim/substr/indexOf/startsWith/endsWith 等）仍由 BuiltinMethods 类
// （Interpreter 侧，抛 RuntimeError）和 VM 内联代码（VM 侧，返回错误码）各自维护。
// TODO: 后续逐步将其余方法迁移到共享纯函数层。
//
// 设计要点：
//   1. 不抛异常 —— 错误通过 SharedBuiltinResult 字段传达，便于 VM 使用返回码语义；
//      Interpreter 侧的 handle*Method 在调用后自行转换为 RuntimeError 抛出。
//   2. 参数使用 const Value* + size_t 而非 const std::vector<Value>& —— 避免 VM
//      从 SmallArgs 构造 vector 的额外分配（零拷贝传递栈上参数）。
//   3. 非变异方法 —— 接收者均为 const Value&，不触发 COW detach。

/// 共享纯函数执行结果（不抛异常，错误通过字段传达，便于 VM 使用返回码语义）
struct SharedBuiltinResult {
    Value result;              // 返回值（出错时为 null）
    bool isError;              // 是否出错
    std::string errorMessage;  // 错误信息
    int errorLine;             // 错误行号
    int errorColumn;           // 错误列号

    SharedBuiltinResult()
        : result(Value::nullValue()), isError(false),
          errorLine(0), errorColumn(0) {}
};

/// 共享纯函数：执行 len 方法（适用于数组/字典/字符串）
/// - 数组：返回元素个数
/// - 字典：返回键值对个数
/// - 字符串：返回 UTF-8 码位个数（M6 fix: 按码位而非字节计数）
/// @param obj       目标对象（必须是数组/字典/字符串）
/// @param args       参数列表首指针（argCount==0 时可为 nullptr）
/// @param argCount   参数数量（len 期望 0）
/// @param line       调用行号（用于错误报告）
/// @param column     调用列号（用于错误报告）
SharedBuiltinResult executeSharedLen(const Value& obj,
                                     const Value* args, size_t argCount,
                                     int line = 0, int column = 0);

/// 共享纯函数：执行数组 contains 方法
/// 线性扫描数组，使用 Value::equals 判断相等（与 Interpreter/VM 原实现一致）
/// @param arr       数组对象
/// @param args       参数列表首指针
/// @param argCount   参数数量（contains 期望 1）
/// @param line       调用行号
/// @param column     调用列号
SharedBuiltinResult executeSharedArrayContains(const Value& arr,
                                               const Value* args, size_t argCount,
                                               int line = 0, int column = 0);

/// 共享纯函数：执行字典 has / contains 方法
/// 检查字典中是否存在指定键（键通过 Value::toString 转换为字符串）
/// @param dict      字典对象
/// @param method     方法名（"has" 或 "contains"，仅用于错误消息）
/// @param args       参数列表首指针
/// @param argCount   参数数量（has/contains 期望 1）
/// @param line       调用行号
/// @param column     调用列号
SharedBuiltinResult executeSharedDictHas(const Value& dict,
                                         const std::string& method,
                                         const Value* args, size_t argCount,
                                         int line = 0, int column = 0);

/// 内置方法辅助类（全静态方法，无状态）
class BuiltinMethods {
public:
    /// 处理数组内置方法（push, pop, len, remove, contains, join）
    /// @param method  方法名
    /// @param obj     数组值（可变引用，push/pop/remove 会修改）
    /// @param args    已求值的参数列表
    /// @param line    调用行号（用于错误报告）
    /// @param col     调用列号（用于错误报告）
    /// @return        方法结果 + 是否修改了对象
    /// @throws RuntimeError 参数错误或方法不存在时
    static BuiltinMethodResult handleArrayMethod(
        const std::string& method, Value& obj,
        const std::vector<Value>& args, int line, int col);

    /// 处理字典内置方法（len, keys, values, has/contains, get, remove）
    static BuiltinMethodResult handleDictMethod(
        const std::string& method, Value& obj,
        const std::vector<Value>& args, int line, int col);

    /// 处理字符串内置方法（len, upper, lower, split, replace, trim）
    /// 字符串是不可变的，所以 obj 为 const 引用
    static BuiltinMethodResult handleStringMethod(
        const std::string& method, const Value& obj,
        const std::vector<Value>& args, int line, int col);
};
