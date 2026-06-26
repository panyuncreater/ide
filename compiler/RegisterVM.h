#pragma once

// ============================================================
// RegisterVM.h — PERF-14: 寄存器式虚拟机
// ------------------------------------------------------------
// 执行 RegBytecodeChunk。与栈式 VM 并存，通过 setUseRegisterVM(true) 启用。
//
// 寄存器帧模型：
//   - 每个调用帧有 registerCount 个寄存器（Value 数组）
//   - R0..R(arity-1)：参数寄存器（调用时填充）
//   - R(arity)..R(localCount-1)：局部变量寄存器
//   - R(localCount)..R(registerCount-1)：临时寄存器
//
// 与栈式 VM 的关键差异：
//   - 无操作数栈，所有操作通过寄存器号寻址
//   - 函数调用时，参数直接放入被调用帧的参数寄存器
//   - 返回值直接放入调用者帧的目标寄存器
// ============================================================

#include "common/IBackend.h"
#include "common/Diagnostic.h"
#include "common/RuntimeLimits.h"
#include "compiler/RegisterBytecode.h"
#include "compiler/VM.h"  // VMResult 枚举复用
#include "interpreter/Value.h"
#include "interpreter/ValueData.h"  // VMUpvalue
#include "interpreter/ValueTypes.h"  // VMClosureData
#include <vector>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <memory>
#include <string>

// 前向声明
class VMClassInfo;

/// 寄存器式 VM 步进信息（用于调试/可视化）
struct RegVMStepInfo {
    size_t ip;
    RegOp opcode;
    size_t frameCount;
};

// ============================================================
// 寄存器式 VM 调用帧
// ============================================================
struct RegCallFrame {
    const RegBytecodeChunk* chunk = nullptr;  // 当前执行的 chunk
    size_t ip = 0;                            // 指令指针
    size_t returnIp = 0;                      // 返回后调用者的 ip
    int returnReg = -1;                       // 返回值写入调用者的寄存器号（-1 = 丢弃）
    std::vector<Value> registers;             // 寄存器窗口（registerCount 个）
    // 方法调用相关
    bool isMethodCall = false;
    bool isInitCall = false;
    int receiverReg = -1;                     // 接收者在调用者帧中的寄存器号
    std::string receiverVarName;              // 接收者变量名（用于写回全局变量）
    int receiverLocalSlot = -1;               // 接收者局部变量槽（用于写回局部变量）
    bool fieldsModified = false;              // 是否修改了实例字段
    // upvalue 支持
    std::vector<std::shared_ptr<VMUpvalue>> upvalues;
};

// ============================================================
// 寄存器式虚拟机
// ============================================================
class RegisterVM : public IBackend {
public:
    RegisterVM();
    ~RegisterVM() override = default;

    /// IBackend 实现
    std::string backendName() const override { return "RegisterVM"; }
    void setOutputCallback(std::function<void(const std::string&)> cb) override;
    void setInputCallback(std::function<std::string(const std::string&)> cb) override;
    const DiagnosticBag& getDiagnostics() const override { return diagnostics_; }

    /// 执行编译结果（全速模式）
    VMResult execute(const RegisterCompileResult& result);

    /// 初始化执行环境（单步模式前置）
    void initExecution(const RegisterCompileResult& result);

    /// 单步执行一条指令
    VMResult stepOnce();

    /// 是否执行完毕
    bool isFinished() const;

    /// 是否已初始化
    bool isInitialized() const { return initialized_; }

    /// 重置状态
    void resetState();

    /// 错误状态
    bool hasError() const { return hasError_; }
    std::string getLastError() const { return lastError_; }
    int getLastErrorLine() const { return lastErrorLine_; }

    /// 调试接口
    std::vector<Value> getRegisters() const;
    std::unordered_map<std::string, Value> getGlobals() const;
    size_t getCurrentIP() const;
    RegOp getCurrentOpCode() const;
    int getCurrentLine() const;
    std::string getCurrentChunkName() const;
    size_t getFrameCount() const { return frames_.size(); }

    struct RegCallStackEntry {
        std::string functionName;
        int line;
        size_t ip;
    };
    std::vector<RegCallStackEntry> getCallStack() const;

    /// 步进回调
    void setStepCallback(std::function<void(const RegVMStepInfo&)> cb) { stepCallback_ = cb; }
    void setStepCallbackEnabled(bool enabled) { stepCallbackEnabled_ = enabled; }

private:
    // 寄存器帧
    std::vector<RegCallFrame> frames_;
    RegBytecodeChunk mainChunk_;
    std::map<std::string, RegBytecodeChunk> functionChunks_;

    // 全局变量
    std::unordered_map<std::string, Value> globals_;
    std::vector<Value> globalSlots_;
    std::unordered_map<std::string, int> globalNameToSlot_;

    // 函数闭包（函数名 → 闭包值）
    std::unordered_map<std::string, Value> functionClosures_;

    // 类信息
    struct RegClassInfo {
        std::string name;
        std::string parent;
        std::vector<std::string> fieldOrder;
        std::map<std::string, std::string> methods;  // 方法名 → 函数名
    };
    std::unordered_map<std::string, RegClassInfo> classInfo_;

    // upvalue 支持
    std::multimap<size_t, std::weak_ptr<VMUpvalue>> openUpvalues_;

    // 回调
    std::function<void(const std::string&)> outputCallback_;
    std::function<std::string(const std::string&)> inputCallback_;
    std::function<void(const RegVMStepInfo&)> stepCallback_;
    bool stepCallbackEnabled_ = false;

    // 状态
    bool initialized_ = false;
    bool hasError_ = false;
    std::string lastError_;
    int lastErrorLine_ = 0;
    DiagnosticBag diagnostics_;
    int64_t instructionCount_ = 0;

    // 异常处理
    struct RegTryHandler {
        size_t catchIp;
        size_t frameIndex;
    };
    std::vector<RegTryHandler> tryStack_;

    // 常量
    static constexpr size_t MAX_FRAMES = RuntimeLimits::MAX_FRAMES;
    static constexpr int64_t MAX_INSTRUCTIONS = RuntimeLimits::MAX_INSTRUCTIONS;

    // 辅助方法
    RegCallFrame& currentFrame() { return frames_.back(); }
    const RegCallFrame& currentFrame() const { return frames_.back(); }
    Value& reg(uint8_t r);
    const Value& reg(uint8_t r) const;

    VMResult runtimeError(const std::string& msg);
    VMResult executeOneInstruction();
    VMResult throwException(Value thrownValue);
    void closeUpvaluesFrom(size_t fromSlot);

    // 指令执行分类
    VMResult executeConstants(RegOp op, size_t& ip);
    VMResult executeArith(RegOp op, size_t& ip);
    VMResult executeCompare(RegOp op, size_t& ip);
    VMResult executeVars(RegOp op, size_t& ip);
    VMResult executeCalls(RegOp op, size_t& ip);
    VMResult executeContainers(RegOp op, size_t& ip);
    VMResult executeControl(RegOp op, size_t& ip);
    VMResult executeMisc(RegOp op, size_t& ip);

    // 调用辅助
    VMResult executeCallImpl(size_t& ip, const std::string& funName,
                             uint8_t argCount, uint8_t dstReg,
                             const std::vector<uint8_t>& argRegs);
    VMResult executeReturnImpl(size_t& ip, Value result);
    VMResult executeMethodCallImpl(size_t& ip, const std::string& methodName,
                                   uint8_t argCount, uint8_t dstReg,
                                   uint8_t objReg, const std::vector<uint8_t>& argRegs);
    VMResult executeClosureImpl(size_t& ip, const std::string& name,
                                uint8_t uvCount, const std::vector<uint8_t>& uvSpecs,
                                uint8_t dstReg);
    VMResult executeClassNewImpl(size_t& ip, const std::string& className,
                                 uint8_t argCount, uint8_t dstReg,
                                 const std::vector<uint8_t>& argRegs);
    bool fillDefaultArgs(const RegBytecodeChunk& chunk, uint8_t& argCount,
                         const std::string& funName, std::vector<Value>& defaults);

    // 内建方法
    VMResult callBuiltinMethod(const Value& obj, const std::string& methodName,
                               std::vector<Value>& args, Value& result);
};
