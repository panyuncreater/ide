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
