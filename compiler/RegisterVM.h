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
#include <array>
#include <vector>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <memory>
#include <stdexcept>  // B3 fix: std::runtime_error 用于越界抛出
#include <string>

// 前向声明
struct VMClassInfo;  // D-1: 与 VM.h 定义一致

/// 寄存器式 VM 步进信息（用于调试/可视化）
// P3-1 fix: 加默认成员初始化器，新增字段时不会漏初始化导致未定义行为
struct RegVMStepInfo {
    size_t ip = 0;
    RegOp opcode = RegOp::REG_RETURN_NULL;
    size_t frameCount = 0;
};

// ============================================================
// 寄存器式 VM 调用帧
// ============================================================
// #8 fix: 将 registers 从 std::vector<Value>（每次调用堆分配）改为
// std::array<Value, MAX_REGISTERS>（栈内联缓冲，零堆分配）。
// 寄存器号上限为 32（见 RegisterBytecodeBackend 的 vreg 映射检查），
// 故定长 32 元素数组足够。registerCount 记录实际激活的寄存器数，
// 用于 reg() 的边界检查（避免读/写未初始化的尾部槽位）。
struct RegCallFrame {
    static constexpr size_t MAX_REGISTERS = 32;  // 与 RegisterBytecodeBackend 的上限对齐

    const RegBytecodeChunk* chunk = nullptr;  // 当前执行的 chunk
    size_t ip = 0;                            // 指令指针
    size_t returnIp = 0;                      // 返回后调用者的 ip
    int returnReg = -1;                       // 返回值写入调用者的寄存器号（-1 = 丢弃）
    std::array<Value, MAX_REGISTERS> registers{};  // 寄存器窗口（定长，零堆分配）
    uint8_t registerCount = 0;                // 实际激活的寄存器数（≤ MAX_REGISTERS）
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

    /// BUG-IDE-12 fix: 获取当前帧的局部变量名→值映射（用于 RegisterVM 条件断点求值）。
    /// 结合当前帧 chunk 的 localRegNames + 寄存器窗口反查。
    /// 空帧/主程序帧（无 localRegNames）返回空映射。
    std::unordered_map<std::string, Value> getCurrentFrameLocals() const;

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

    // BUG-REGVM-3 fix: 删除 functionClosures_ 死代码字段。该字段无任何写入点，
    // 仅 resetState clear 和 executeCallImpl find，是死代码。
    // 闭包调用通过 REG_CALL_EXPR + MAKE_CLOSURE 正确处理。

    // 类信息
    struct RegClassInfo {
        std::string name;
        std::string parent;
        std::vector<std::string> fieldOrder;
        std::map<std::string, std::string> methods;  // 方法名 → 函数名
        // BUG-INH-1 fix: 字段默认值（字面量），对齐 StackVM 的 OP_INIT_FIELD 路径。
        // 无默认值或非字面量表达式的字段为 nullValue()。
        std::vector<Value> fieldDefaults;
        // perf2 fix: 预计算缓存，避免每次构造实例时重复遍历继承链。
        // 懒计算：首次 REG_CLASS_NEW 时填充（此时所有父类必已定义，因构造前
        // 所有顶层 REG_DEFINE_CLASS 已执行完毕）。REG_DEFINE_CLASS 重新定义时复位。
        mutable bool flattenedComputed = false;
        mutable std::vector<std::string> flattenedFieldOrder;  // 父类字段在前，子类在后
        // BUG-INH-1 fix: 与 flattenedFieldOrder 并行存储的字段默认值。
        // 沿继承链合并（父类在前），子类同名字段覆盖父类。对齐 StackVM 的
        // mergedDefaults 语义，避免 IR 路径所有字段被硬编码为 null。
        mutable std::vector<Value> flattenedFieldDefaults;
        mutable std::string resolvedInitFunName;  // 沿继承链解析的 init 函数名
        mutable bool hasInit = false;             // 继承链中是否存在 init
    };
    std::unordered_map<std::string, RegClassInfo> classInfo_;

    // upvalue 支持
    std::multimap<size_t, std::weak_ptr<VMUpvalue>> openUpvalues_;

    // C-1 fix: 跟踪最近一次 INDEX_SET/MEMBER_SET 的接收者寄存器号，
    // 供紧随其后的 WRITEBACK_* 指令读取变异后的容器并写回到变量槽。
    // （镜像栈式 VM 的 lastMutatedReceiver_ 机制；IR 保证 WRITEBACK 紧跟 SET）
    uint8_t lastMutatedReceiverReg_ = 0;

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
        // BUG-EXC-5 fix: try 块开始时的寄存器数，catch 命中时关闭
        // [registerBase, registerCount) 范围的 open upvalues，对齐 StackVM
        // closeUpvaluesFrom(handler.stackBase)。try 块中声明的局部变量若被闭包捕获，
        // 需在 catch 块覆盖前关闭 upvalue（拷贝值到 heap），防止 catch 块复用寄存器时
        // 闭包读到错误值。
        uint8_t registerBase = 0;
    };
    std::vector<RegTryHandler> tryStack_;
    // P1-4 fix: 待捕获的异常值。throwException 设置，REG_LOAD_EXCEPTION 读取。
    // 替代原方案（固定写 R0 覆盖用户变量），避免破坏调用者寄存器。
    Value pendingException_;

    // 常量
    static constexpr size_t MAX_FRAMES = RuntimeLimits::MAX_FRAMES;
    static constexpr int64_t MAX_INSTRUCTIONS = RuntimeLimits::MAX_INSTRUCTIONS;

    // 辅助方法
    // 调用方契约：execute/stepOnce 在调用前已检查 frames_.empty()
    // B3 fix: 越界时调用 runtimeError（设置 hasError_ + 诊断）后抛 std::runtime_error，
    // 替代原 std::abort()。调用方（VmStepper::stepByMode / runBatch）已用 try/catch 包裹，
    // 抛出会被捕获并转化为 ERROR 状态，避免 IDE 整个进程崩溃。
    RegCallFrame& currentFrame() {
        if (frames_.empty()) {
            runtimeError("currentFrame() on empty frames");
            throw std::runtime_error("RegisterVM: currentFrame() on empty frames");
        }
        return frames_.back();
    }
    const RegCallFrame& currentFrame() const {
        if (frames_.empty()) {
            throw std::runtime_error("RegisterVM: currentFrame() on empty frames");
        }
        return frames_.back();
    }
    Value& reg(uint8_t r);
    const Value& reg(uint8_t r) const;

    VMResult runtimeError(const std::string& msg);
    VMResult executeOneInstruction();
    VMResult throwException(Value thrownValue);
    /// C-3 fix: 关闭指向 [fromSlot, ∞) 范围（全局编码 stackSlot）的 open upvalues，
    /// 从对应帧读取当前值并标记为已关闭。用于帧弹出/异常展开时防止悬垂引用。
    void closeUpvaluesFrom(size_t fromSlot);
    /// C-3 fix: 解码 uv->stackSlot（编码为 frameIdx*32+slot）返回指向目标寄存器的指针。
    /// 失败时调用 runtimeError 并返回 nullptr。仅用于 open upvalue（isClosed=false）。
    Value* resolveOpenUpvalueSlot(struct VMUpvalue& uv);

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
    // C-2 fix: closureValue 非空时从其 vmClosure 提取 upvalues 填入新帧，
    // 使被调用的闭包能访问捕获的外层变量。REG_CALL_EXPR 传 &callee，REG_CALL 传 nullptr。
    // C-8 fix: returnOffset 为调用者指令的总长度（字节数），用于计算 returnIp。
    //   原代码硬编码 ip+5+argCount 假定 REG_CALL 格式，对 REG_CALL_EXPR(4+argCount)
    //   和 REG_CLASS_NEW init(5+原argCount，但传入 argCount+1) 各偏移 +1，导致返回后 ip 错位。
    VMResult executeCallImpl(size_t& ip, const std::string& funName,
                             uint8_t argCount, uint8_t dstReg,
                             const SmallArgs<uint8_t>& argRegs,
                             size_t returnOffset,
                             const Value* closureValue = nullptr);
    VMResult executeReturnImpl(size_t& ip, Value result);
    VMResult executeMethodCallImpl(size_t& ip, const std::string& methodName,
                                   uint8_t argCount, uint8_t dstReg,
                                   uint8_t objReg, const SmallArgs<uint8_t>& argRegs);
    VMResult executeClosureImpl(size_t& ip, const std::string& name,
                                uint8_t uvCount, const SmallArgs<uint8_t>& uvSpecs,
                                uint8_t dstReg);
    VMResult executeClassNewImpl(size_t& ip, const std::string& className,
                                 uint8_t argCount, uint8_t dstReg,
                                 const SmallArgs<uint8_t>& argRegs);
    bool fillDefaultArgs(const RegBytecodeChunk& chunk, uint8_t& argCount,
                         const std::string& funName, std::vector<Value>& defaults);

    // 内建方法
    // C-9 fix: 返回 bool 而非 VMResult。true=已处理（caller 应 return，检查 hasError_），
    // false=未匹配内建方法（caller 继续查找用户定义方法）。
    // 原实现无论是否匹配都返回 VM_OK，导致实例方法调用被静默吞掉（callBuiltinMethod
    // 对 instance 类型 fallthrough 到末尾 return VM_OK，caller 误以为已处理）。
    bool callBuiltinMethod(Value& obj, const std::string& methodName,
                           SmallArgs<Value>& args, Value& result);
};
