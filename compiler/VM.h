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

/// 每条指令执行后的状态快照（用于调试/可视化）
struct VMStepInfo {
    size_t ip;                              // 当前指令指针
    OpCode opcode;                          // 当前操作码
    std::vector<Value> stackSnapshot;        // 操作数栈快照
    std::unordered_map<std::string, Value> globalsSnapshot;  // 全局变量快照
};

/// VM 调用帧
struct VMCallFrame {
    const BytecodeChunk* chunk = nullptr;   // 当前执行的字节码块
    size_t ip = 0;                          // 当前帧的指令指针
    size_t returnIp = 0;                    // 返回后的 ip
    size_t basePointer = 0;                 // 帧基指针（栈中参数起始位置）
    std::string functionName;               // 函数名
};

/// 简单栈式虚拟机
class VM {
public:
    VM();

    /// 执行编译结果（一次性全部执行）
    VMResult execute(const CompileResult& result);

    /// 设置输出回调
    void setOutputCallback(std::function<void(const std::string&)> callback);

    /// 设置指令级步进回调（每条指令执行后调用）
    void setStepCallback(std::function<void(const VMStepInfo&)> callback);

    /// 设置是否启用步进回调（默认关闭，避免性能开销）
    void setStepCallbackEnabled(bool enabled);

    /// 获取最后的运行时错误
    std::string getLastError() const;

    /// 获取栈内容（用于调试）
    std::vector<Value> getStack() const;

    /// 获取全局变量（用于调试）
    std::unordered_map<std::string, Value> getGlobals() const;

private:
    std::vector<Value> stack_;                     // 操作数栈
    std::unordered_map<std::string, Value> globals_; // 全局变量表
    std::vector<VMCallFrame> frames_;              // 调用帧栈
    std::unordered_map<std::string, BytecodeChunk> functionChunks_; // 函数字节码
    std::function<void(const std::string&)> outputCallback_; // 输出回调
    std::function<void(const VMStepInfo&)> stepCallback_;    // 步进回调
    bool stepCallbackEnabled_ = false;              // 是否启用步进回调
    std::string lastError_;                         // 最近一次运行时错误
    static constexpr size_t MAX_STACK_SIZE = 1024;  // 栈最大深度
    static constexpr size_t MAX_FRAMES = 256;       // 调用帧最大深度

    /// 栈操作
    void push(const Value& val);
    Value pop();
    Value& peek(size_t distance = 0);

    /// 运行时错误
    VMResult runtimeError(const std::string& msg);

    /// 数值二元运算
    VMResult numericOp(const std::string& op, int line);

    /// 通知步进回调
    void notifyStep(size_t ip, OpCode opcode);

    /// 获取当前帧
    VMCallFrame& currentFrame();

    /// 获取当前 chunk
    const BytecodeChunk& currentChunk();
};
