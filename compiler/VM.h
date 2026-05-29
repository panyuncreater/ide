#pragma once

#include <vector>
#include <string>
#include <functional>
#include <unordered_map>
#include "compiler/Bytecode.h"
#include "interpreter/Value.h"

// ============================================================
// VM 虚拟机（简单栈机）
// ============================================================

/// 虚拟机执行结果
enum class VMResult {
    VM_OK,
    VM_RUNTIME_ERROR,
    VM_STACK_OVERFLOW
};

/// 简单栈式虚拟机
class VM {
public:
    VM();

    /// 执行字节码
    VMResult execute(const BytecodeChunk& chunk);

    /// 设置输出回调
    void setOutputCallback(std::function<void(const std::string&)> callback);

    /// 获取最后的运行时错误
    std::string getLastError() const;

    /// 获取栈内容（用于调试）
    std::vector<Value> getStack() const;

    /// 获取全局变量（用于调试）
    std::unordered_map<std::string, Value> getGlobals() const;

private:
    std::vector<Value> stack_;                     // 操作数栈
    std::unordered_map<std::string, Value> globals_; // 全局变量表
    std::function<void(const std::string&)> outputCallback_; // 输出回调
    std::string lastError_;                         // 最近一次运行时错误
    static constexpr size_t MAX_STACK_SIZE = 1024;  // 栈最大深度

    /// 栈操作
    void push(const Value& val);
    Value pop();
    Value& peek(size_t distance = 0);

    /// 运行时错误
    VMResult runtimeError(const std::string& msg);

    /// 数值二元运算
    VMResult numericOp(const std::string& op, int line);
};
