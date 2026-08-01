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

#include "common/Diagnostic.h"
#include "common/IBackend.h"
#include "common/RuntimeLimits.h"
#include "compiler/RegisterBytecode.h"
#include "compiler/VM.h" // VMResult 枚举复用
#include "interpreter/Value.h"
#include "interpreter/ValueData.h"  // VMUpvalue
#include "interpreter/ValueTypes.h" // VMClosureData
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <mutex>     // R136 spawnMutex_
#include <stdexcept> // B3 fix: std::runtime_error 用于越界抛出
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// 前向声明
struct VMClassInfo;             // D-1: 与 VM.h 定义一致
enum class BuiltinMethod : int; // 用于 callBuiltinMethod 子函数签名（定义见 interpreter/BuiltinMethods.h）

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
    static constexpr size_t MAX_REGISTERS = 32; // 与 RegisterBytecodeBackend 的上限对齐

    const RegBytecodeChunk* chunk = nullptr;      // 当前执行的 chunk
    size_t ip = 0;                                // 指令指针
    size_t returnIp = 0;                          // 返回后调用者的 ip
    int returnReg = -1;                           // 返回值写入调用者的寄存器号（-1 = 丢弃）
    std::array<Value, MAX_REGISTERS> registers{}; // 寄存器窗口（定长，零堆分配）
    uint8_t registerCount = 0;                    // 实际激活的寄存器数（≤ MAX_REGISTERS）
    // 方法调用相关
    bool isMethodCall = false;
    bool isInitCall = false;
    int receiverReg = -1;        // 接收者在调用者帧中的寄存器号
    std::string receiverVarName; // 接收者变量名（用于写回全局变量）
    int receiverLocalSlot = -1;  // 接收者局部变量槽（用于写回局部变量）
    bool fieldsModified = false; // 是否修改了实例字段
    // upvalue 支持
    std::vector<std::shared_ptr<VMUpvalue>> upvalues;
};

// ============================================================
// 寄存器式虚拟机
// ============================================================
class RegisterVM : public IVmBackend {
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

    /// P1-5 fix: IVmBackend override — 单步执行一条指令
    VMResult stepOnce() override;

    /// P1-5 fix: IVmBackend override — 是否执行完毕
    bool isFinished() const override;

    /// 是否已初始化
    bool isInitialized() const { return initialized_; }

    /// 重置状态
    void resetState();

    /// 错误状态
    /// P2-12: hasError_/getLastError()/getLastErrorLine() 从 diagnostics_ 派生，
    /// 消除 lastError_/lastErrorLine_ 字段冗余（与 VM 改造对齐）。
    bool hasError() const override { return hasError_ || diagnostics_.hasErrors(); }
    std::string getLastError() const override {
        const auto& diags = diagnostics_.all();
        for (auto it = diags.rbegin(); it != diags.rend(); ++it) {
            if (it->isError())
                return it->message;
        }
        return {};
    }
    int getLastErrorLine() const override {
        const auto& diags = diagnostics_.all();
        for (auto it = diags.rbegin(); it != diags.rend(); ++it) {
            if (it->isError())
                return it->line;
        }
        return 0;
    }

    /// 调试接口
    std::vector<Value> getRegisters() const;
    /// P1-5 fix: IVmBackend override — 获取操作数栈快照。
    /// RegisterVM 无操作数栈，返回当前帧寄存器窗口值（与 getRegisters 语义一致）。
    std::vector<Value> getStack() const override { return getRegisters(); }
    std::unordered_map<std::string, Value> getGlobals() const override;
    size_t getCurrentIP() const override;
    RegOp getCurrentOpCode() const;
    int getCurrentLine() const override;
    std::string getCurrentChunkName() const;
    size_t getFrameCount() const override { return frames_.size(); }

    // P1-5 扩展: RegCallStackEntry 改为 VmCallStackEntry 别名，统一到 IVmBackend 接口。
    // 旧代码引用 RegisterVM::RegCallStackEntry 仍可编译（别名透明）。
    using RegCallStackEntry = VmCallStackEntry;
    std::vector<VmCallStackEntry> getCallStack() const override;

    /// BUG-IDE-12 fix: 获取当前帧的局部变量名→值映射（用于 RegisterVM 条件断点求值）。
    /// 结合当前帧 chunk 的 localRegNames + 寄存器窗口反查。
    /// 空帧/主程序帧（无 localRegNames）返回空映射。
    std::unordered_map<std::string, Value> getCurrentFrameLocals() const;

    /// P2-3 fix: 获取指定帧的局部变量名→值映射（用于调用栈面板显示各帧 locals）。
    /// frameIndex 从 0 开始（0=栈底 main 帧）。越界或无 localRegNames 返回空映射。
    std::unordered_map<std::string, Value> getFrameLocalsAt(size_t frameIndex) const;

    // ---- 拓展·调试器 setVariable：调试暂停时写变量（与 VM.h 镜像）----
    /// 写入已存在的全局变量（globalSlots_ 优先，globals_ fallback）。
    /// 不新建变量。@return true 写入成功；false 变量不存在
    bool setGlobalValue(const std::string& name, const Value& val);

    /// 写入指定帧的局部变量（按 resolveSlotName 反查寄存器，与 getFrameLocalsAt
    /// 镜像）；未命中时回退 upvalue 写入。@return true 写入成功
    bool setFrameLocalAt(size_t frameIndex, const std::string& name, const Value& val);

    /// R104 Function Breakpoint：在 REG_CALL 指令执行前查询被调用函数名。
    /// @return 若当前指令是 REG_CALL，返回常量池中的函数名；否则返回空字符串。
    std::string peekCalledFunctionName() const;

    /// R104 Exception Breakpoint：检查当前 IP 指向的指令是否为 REG_THROW。
    /// @return true 表示当前指令是 REG_THROW
    bool isCurrentThrowInstruction() const;

    /// R161 Watchpoint：pre-execution peek 当前 IP 指令的写入目标（不执行指令）。
    /// 用于数据断点（Watchpoint）在写入指令执行前检查。
    /// @return WriteTarget{isWrite=true, ...} 若当前指令是 SET 类指令；否则 isWrite=false
    /// WriteTarget 类型定义在 debug/DebugTypes.h（VM/RegisterVM 共享）
    WriteTarget peekWriteTarget() const;

    /// R114 阶段 3：从快照恢复 RegisterVM 状态（状态回滚）。
    /// 仅在 initExecution 已调用（initialized_==true）后可用。
    /// 恢复语义：(1) 截断 frames_ 到 targetFrameCount 并设置栈顶帧 ip=targetIp；
    /// (2) 将 registerValues 写入栈顶帧的 registers[]（截断到 MAX_REGISTERS=32）；
    /// (3) 按 name 更新 globalSlots_/globals_（已有变量覆盖，新变量忽略）；
    /// (4) 清理 openUpvalues_（slots >= MAX_REGISTERS 的 open upvalue 已悬垂——
    ///     RegisterVM 寄存器帧定长 32，按 registerIndex 索引 openUpvalues_）；
    /// (5) 清理 tryStack_（frameIndex >= targetFrameCount）、pendingJumpStack_、
    /// pendingException_、lastMutatedReceiverReg_、hasError_/diagnostics_。
    /// @note 与 StackVM 不同，RegisterVM 的 openUpvalues_ 按 registerIndex 而非
    ///       栈绝对位置索引；回滚后所有 openUpvalues_ 应清理（因寄存器被覆盖，
    ///       原 open upvalue 的 stackSlot 已指向新值）。
    /// @return true 成功；false 未初始化或 targetFrameCount 越界
    bool restoreFromSnapshot(const std::vector<Value>& registerValues,
                             const std::vector<std::pair<std::string, Value>>& globalsValues, size_t targetIp,
                             size_t targetFrameCount);

    /// 步进回调
    void setStepCallback(std::function<void(const RegVMStepInfo&)> cb) { stepCallback_ = cb; }
    void setStepCallbackEnabled(bool enabled) { stepCallbackEnabled_ = enabled; }

    /// P-7 perf: 内联步进通知（对齐 StackVM::notifyStep 的单 bool 快速路径）。
    /// 仅检查 stepCallbackEnabled_（bool），省去 std::function 非空测试。
    /// 调用方须保证 setStepCallback 在 setStepCallbackEnabled(true) 之前调用。
    void notifyStep(size_t ip, RegOp opcode) {
        if (!stepCallbackEnabled_)
            return;
        stepCallback_(RegVMStepInfo{ip, opcode, frames_.size()});
    }

private:
    // 寄存器帧
    std::vector<RegCallFrame> frames_;
    RegBytecodeChunk mainChunk_;
    std::map<std::string, RegBytecodeChunk> functionChunks_;
    // PERF-AUDIT-5 fix: 内联缓存，避免每次 REG_CALL 的 O(log n) 红黑树字符串查找。
    // 与 StackVM 的 callCache_ 模式一致：用指向 functionChunks_ key 的裸指针作键
    // （std::map 节点稳定，key 地址在 chunk 生命周期内不变），O(1) 哈希 + 指针比较。
    std::unordered_map<const std::string*, const RegBytecodeChunk*> callCache_;

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
        std::map<std::string, std::string> methods; // 方法名 → 函数名
        // BUG-INH-1 fix: 字段默认值（字面量），对齐 StackVM 的 OP_INIT_FIELD 路径。
        // 无默认值或非字面量表达式的字段为 nullValue()。
        std::vector<Value> fieldDefaults;
        // perf2 fix: 预计算缓存，避免每次构造实例时重复遍历继承链。
        // 懒计算：首次 REG_CLASS_NEW 时填充（此时所有父类必已定义，因构造前
        // 所有顶层 REG_DEFINE_CLASS 已执行完毕）。REG_DEFINE_CLASS 重新定义时复位。
        mutable bool flattenedComputed = false;
        mutable std::vector<std::string> flattenedFieldOrder; // 父类字段在前，子类在后
        // BUG-INH-1 fix: 与 flattenedFieldOrder 并行存储的字段默认值。
        // 沿继承链合并（父类在前），子类同名字段覆盖父类。对齐 StackVM 的
        // mergedDefaults 语义，避免 IR 路径所有字段被硬编码为 null。
        mutable std::vector<Value> flattenedFieldDefaults;
        mutable std::string resolvedInitFunName; // 沿继承链解析的 init 函数名
        mutable bool hasInit = false;            // 继承链中是否存在 init
        // R133 Inline Cache: per-class method dispatch cache。镜像 StackVM
        // VMClassInfo::methodCache 的设计：key=methodName,value=funName
        // (空字符串表示"沿继承链未找到",作为负缓存避免重复查找)。
        // 命中后跳过 executeMethodCallImpl 中沿继承链 O(n) × methods map O(log n)
        // 的查找路径。失效条件：父类重定义时由 REG_DEFINE_CLASS 遍历子类置
        // methodCache.clear()（对齐 BUG-INH-AUDIT-7 fix 的 flattenedComputed 失效模式）。
        mutable std::unordered_map<std::string, std::string> methodCache;
    };
    std::unordered_map<std::string, RegClassInfo> classInfo_;
    // R99 enum 校验：enum 元信息注册表（enum 名→variant 列表），initExecution 从
    // RegisterCompileResult.enumInfos 加载，REG_BUILD_ENUM_VARIANT 校验 variant 名与 arity。
    // 对齐 Interpreter::enumRegistry_ / VM::enumRegistry_ 的运行时校验语义。
    std::unordered_map<std::string, VMEnumInfo> enumRegistry_;

    // upvalue 支持
    // AUDIT-R5 R1 fix: 条目携带单调递增的开启序号（upvalueOpenSeq_）。
    // REG_TRY_BEGIN 记录当时序号水位，异常展开时仅关闭 try 之后开启的 upvalue，
    // 避免 Bug #47 的 regBase=0 方案把 try 之前创建、catch 后仍存活的闭包
    // upvalue 一并提前关闭为快照（与 Interpreter/StackVM 的共享语义不一致）。
    // 序号方案同时覆盖 Bug #47 的槽位复用场景（try 内嵌套作用域复用低槽位）。
    struct OpenUpvalueEntry {
        std::weak_ptr<VMUpvalue> uv;
        uint64_t seq = 0; ///< 开启序号（插入时取 upvalueOpenSeq_++）
    };
    std::multimap<size_t, OpenUpvalueEntry> openUpvalues_;
    uint64_t upvalueOpenSeq_ = 0; ///< AUDIT-R5 R1 fix: open upvalue 全局开启序号计数器

    // W3-2-Bug2 fix: 函数内定义的类的方法捕获的 upvalue（key = funName "Class.method"）。
    // 在 REG_DEFINE_CLASS 执行时（此时仍在定义类的外层函数帧中），为有 upvalue 描述符的方法
    // 创建 VMClosureData 捕获当前帧的寄存器槽（isLocal=true）或当前帧的 upvalue（isLocal=false）。
    // REG_METHOD_CALL 时通过 executeCallImpl 的 populateUpvalues 从此 map 读取并填充方法帧的
    // upvalues，使方法体内 REG_LOAD_UPVALUE 能正确访问。
    std::unordered_map<std::string, std::shared_ptr<VMClosureData>> methodUpvalues_;

    // C-1 fix: 跟踪最近一次 INDEX_SET/MEMBER_SET 的接收者寄存器号，
    // 供紧随其后的 WRITEBACK_* 指令读取变异后的容器并写回到变量槽。
    // （镜像栈式 VM 的 lastMutatedReceiver_ 机制；IR 保证 WRITEBACK 紧跟 SET）
    uint8_t lastMutatedReceiverReg_ = 0;

    // 回调
    std::function<void(const std::string&)> outputCallback_;
    std::function<std::string(const std::string&)> inputCallback_;
    // R136 spawn 子线程闭包调用序列化 mutex。RegisterVM 的寄存器帧非线程安全，
    // spawn 出的子线程若并发调用 invokeClosureSync 会破坏这些共享状态。
    std::mutex spawnMutex_;
    std::function<void(const RegVMStepInfo&)> stepCallback_;
    bool stepCallbackEnabled_ = false;

    // 状态
    bool initialized_ = false;
    // P2-12: lastError_/lastErrorLine_ 字段已移除，getLastError()/getLastErrorLine()
    // 改为从 diagnostics_ 派生（消除双通道冗余）。
    // hasError_ 保留作为快速路径标志（与 VM 对齐），权威来源为 diagnostics_。
    bool hasError_ = false;
    DiagnosticBag diagnostics_;
    int64_t instructionCount_ = 0;

    // 异常处理
    struct RegTryHandler {
        size_t catchIp;
        size_t frameIndex;
        // AUDIT-R5 R1 fix: 替换原 registerBase 字段。历史方案演进：
        //   · BUG-EXC-5：registerBase = try 开始时的 registerCount，但 try 内嵌套
        //     作用域可能复用低于该水位的槽位，展开时漏关（Bug #47）；
        //   · Bug #47：改 regBase=0 全关，又把 try 之前创建的闭包 upvalue 误关
        //     为快照（catch 后闭包与变量不再共享，三后端语义不一致）。
        // 现方案：记录 try 开始时的 upvalueOpenSeq_ 水位，catch 命中时仅关闭
        // 本帧内序号 >= 水位（即 try 期间开启）的 upvalue，与槽位号无关，
        // 同时解决两个历史缺陷。已知窄边缘：try 内创建的闭包若捕获 try 之前
        // 的变量并在 throw 前逃逸，其 upvalue 会被关闭（StackVM 保持 open），
        // 该边缘远窄于原 regBase=0 的全量误关。
        uint64_t upvalueSeqFloor = 0;
    };
    std::vector<RegTryHandler> tryStack_;
    // P1-4 fix: 待捕获的异常值。throwException 设置，REG_LOAD_EXCEPTION 读取。
    // 替代原方案（固定写 R0 覆盖用户变量），避免破坏调用者寄存器。
    Value pendingException_;
    // AUDIT-P1.1 fix: break/continue finally 续跳机制（与 StackVM 对齐）。
    // AUDIT-R7 F1 fix: 条目附带 frameIndex，REG_FINALLY_END 只消费本帧条目并惰性
    // 丢弃已返回深帧残留（与 StackVM PendingJump 同构，防跨帧误跳）。
    struct PendingJump {
        size_t target;
        size_t frameIndex;
    };
    std::vector<PendingJump> pendingJumpStack_;

    // R164 协程/生成器：重放模式状态（与 StackVM::VM 对齐，独立字段避免状态串扰）
    // currentCoroutineTargetYieldId_ >= 0 表示当前在协程重放上下文中（REG_YIELD 据此判定）。
    // 每次 .next() 开始时设为 cd->currentYieldId，callCoroutineNext 结束时恢复为 -1。
    int currentCoroutineTargetYieldId_ = -1;
    // 运行时 yield 执行计数器：每次 REG_YIELD 递增，用于区分循环内同一 yield 节点的多次执行。
    // 每次 .next() 重放开始时重置为 0（对齐 Interpreter::currentYieldExecutionCount_）。
    int currentYieldExecutionCount_ = 0;
    // R164 D.7 fix: 生成器函数体 REG_RETURN 的返回值捕获。
    // callCoroutineNext 设置生成器帧 returnReg=-1（不写回调用者寄存器），
    // executeReturnImpl 在 returnReg<0 时将返回值保存到此邮箱，供 callCoroutineNext 读取。
    Value coroutineReturnValue_;

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
    // PERF-AUDIT-5 fix: reg() 内联到头文件。原实现定义在 .cpp 中，每次寄存器访问
    // 是跨翻译单元函数调用，阻断编译器内联优化（寄存器分配/常量传播/死存储消除）。
    // 在 fib(24) 基准中约 150-225 万次非内联调用，是 RegisterVM 慢于 StackVM 的主因。
    // 热路径（合法访问）内联，冷路径（越界）调用 runtimeError 后抛异常。
    Value& reg(uint8_t r) {
        if (frames_.empty()) [[unlikely]] {
            runtimeError("RegisterVM::reg() on empty frames");
            throw std::runtime_error("RegisterVM: reg() on empty frames");
        }
        auto& frame = frames_.back();
        if (r >= frame.registerCount) [[unlikely]] {
            runtimeError("RegisterVM: register index out of range");
            throw std::runtime_error("RegisterVM: register index out of range");
        }
        return frame.registers[r];
    }
    const Value& reg(uint8_t r) const {
        if (frames_.empty()) [[unlikely]] {
            throw std::runtime_error("RegisterVM: reg() on empty frames (const)");
        }
        const auto& frame = frames_.back();
        if (r >= frame.registerCount) [[unlikely]] {
            throw std::runtime_error("RegisterVM: register index out of range (const)");
        }
        return frame.registers[r];
    }

    /// @param diagCode P2 fix: 稳定诊断码（如 "division-by-zero"），透传到 addError
    VMResult runtimeError(const std::string& msg, const std::string& diagCode = "");
    VMResult executeOneInstruction();
    VMResult throwException(Value thrownValue);
    /// C-3 fix: 关闭指向 [fromSlot, ∞) 范围（全局编码 stackSlot）的 open upvalues，
    /// 从对应帧读取当前值并标记为已关闭。用于帧弹出/异常展开时防止悬垂引用。
    /// 返回 VM_RUNTIME_ERROR 表示检测到 slot 越界（hasError_ 已设置，调用方应立即 return 传播错误）
    VMResult closeUpvaluesFrom(size_t fromSlot);
    /// AUDIT-R5 R1 fix: 关闭 frameIdx 帧内开启序号 >= seqFloor 的 open upvalues
    /// （即 try 开始后创建的），用于同帧 catch 命中时的精确关闭。
    /// try 之前开启的 upvalue 保持 open，catch 后闭包与变量继续共享（对齐 StackVM）。
    VMResult closeFrameUpvaluesSince(size_t frameIdx, uint64_t seqFloor);
    /// C-3 fix: 解码 uv->stackSlot（编码为 frameIdx*32+slot）返回指向目标寄存器的指针。
    /// 失败时调用 runtimeError 并返回 nullptr。仅用于 open upvalue（isClosed=false）。
    Value* resolveOpenUpvalueSlot(struct VMUpvalue& uv);

    // 指令执行分类
    // PERF: HOT 方法标记 MINILANG_FORCE_INLINE，等效 computed goto 在 MSVC 上的替代方案。
    VMResult executeConstants(RegOp op, size_t& ip);
    VMResult executeArith(RegOp op, size_t& ip);
    VMResult executeCompare(RegOp op, size_t& ip);
    VMResult executeVars(RegOp op, size_t& ip);
    VMResult executeCalls(RegOp op, size_t& ip);
    VMResult executeContainers(RegOp op, size_t& ip);
    VMResult executeControl(RegOp op, size_t& ip);
    VMResult executeMisc(RegOp op, size_t& ip);

    // executeMisc 子分类（按 RegOp 类别拆分，避免单函数过长）
    // 子函数自包含 stepCallback_ 调用，主函数 dispatch 后直接 return。
    VMResult executeTryThrowOps(RegOp op, size_t& ip);
    VMResult executeWritebackOps(RegOp op, size_t& ip);
    VMResult executeTypeCheckOps(RegOp op, size_t& ip);
    VMResult executeSuperCallOps(RegOp op, size_t& ip);
    /// R164 协程/生成器：REG_YIELD 指令执行（重放模式）
    VMResult executeCoroutineOps(RegOp op, size_t& ip);

    // executeContainers 子分类（按容器操作类别拆分）
    VMResult executeArrayOps(RegOp op, size_t& ip);
    VMResult executeDictOps(RegOp op, size_t& ip);
    VMResult executeMemberOps(RegOp op, size_t& ip);

    // ---- R120 fix: executeArrayOps 拆分为 3 个独立方法（原 224 行 → 每个方法 < 100 行）----
    /// 容器构造指令：REG_BUILD_ARRAY / REG_BUILD_TUPLE / REG_BUILD_ENUM_VARIANT
    VMResult executeArrayBuildOps(RegOp op, size_t& ip);
    /// 索引访问指令：REG_INDEX_GET / REG_INDEX_SET（跨 array/dict/string/tuple 多态）
    VMResult executeArrayIndexOps(RegOp op, size_t& ip);
    /// 枚举 variant 查询指令：REG_ENUM_VARIANT_NAME / REG_ENUM_VARIANT_FIELD
    VMResult executeArrayEnumQueryOps(RegOp op, size_t& ip);

    // executeCalls 子分类（按调用类型拆分）
    VMResult executeCallOps(RegOp op, size_t& ip);
    VMResult executeMethodCallOps(RegOp op, size_t& ip);
    VMResult executeNewOps(RegOp op, size_t& ip);

    // 调用辅助
    // C-2 fix: closureValue 非空时从其 vmClosure 提取 upvalues 填入新帧，
    // 使被调用的闭包能访问捕获的外层变量。REG_CALL_EXPR 传 &callee，REG_CALL 传 nullptr。
    // C-8 fix: returnOffset 为调用者指令的总长度（字节数），用于计算 returnIp。
    //   原代码硬编码 ip+5+argCount 假定 REG_CALL 格式，对 REG_CALL_EXPR(4+argCount)
    //   和 REG_CLASS_NEW init(5+原argCount，但传入 argCount+1) 各偏移 +1，导致返回后 ip 错位。
    // R133 fix: isMethodCall 标志方法调用路径（REG_METHOD_CALL/REG_SUPER_CALL/init 调用）。
    //   方法调用路径的 funName 来自 methods map/classInfo_/局部变量,不是常量池稳定指针,
    //   不应使用 callCache_（用 &funName 作 key 时栈帧复用会导致错误命中）。
    //   详见 IC10 测试用例的 bug 分析。
    VMResult executeCallImpl(size_t& ip, const std::string& funName, uint8_t argCount, uint8_t dstReg,
                             const SmallArgs<uint8_t>& argRegs, size_t returnOffset,
                             const Value* closureValue = nullptr, bool isMethodCall = false);

    /// 拓展二期·语言（运算符重载）：instance 算术 dunder 分派。
    /// reg(s1) 为 instance 且类（含继承链）定义了对应 dunder 方法时，
    /// 经 executeCallImpl 注入方法帧（this=reg(s1)，参数=reg(s2)，
    /// returnReg=dst，returnOffset=4 即 REG_ADD 系列指令长）。
    /// @return true 表示已分派（outResult 有效）；false 未分派（回退报错）。
    /// 限制（三后端一致）：接收者不写回（receiverReg 保持 -1）；不支持 super。
    bool tryOperatorOverload(size_t& ip, RegOp op, uint8_t dst, uint8_t s1, uint8_t s2, VMResult& outResult);

    /// executeCallImpl 子阶段：函数名在 functionChunks_ 未命中时，
    /// 依次尝试 input/higher-order(spawn,map,filter,...)/isBuiltinFunction/classInfo_ 构造调用，
    /// 全部未命中则返回"未定义的函数"错误。始终返回 VM_OK 或 VM_RUNTIME_ERROR。
    /// 仅在 !cachedChunk && it == functionChunks_.end() 时调用。
    VMResult tryCallBuiltinOrClass(size_t& ip, const std::string& funName, uint8_t argCount, uint8_t dstReg,
                                   const SmallArgs<uint8_t>& argRegs, size_t returnOffset);

    /// executeCallImpl 子阶段：填充新帧的 upvalues。
    /// 闭包调用（closureData 非空）从 closureData->upvalues 复制；
    /// 方法调用（isMethodCall）从 methodUpvalues_ 查找并复制。
    /// 两者均不命中则不填充（newFrame.upvalues 保持空）。
    void populateCallFrameUpvalues(RegCallFrame& newFrame, const std::shared_ptr<VMClosureData>& closureData,
                                   const std::string& funName, bool isMethodCall);

    /// executeCallImpl 子阶段：格式化参数计数错误消息。
    /// 方法调用时减去 this（首个寄存器），并使用"方法 X"/"构造函数 init"措辞；
    /// 普通函数调用使用"函数 X"措辞。与 StackVM 路径错误消息对齐。
    std::string formatParamError(bool isMethodCall, const std::string& funName, uint8_t argCount,
                                 const RegBytecodeChunk& calleeChunk) const;

    VMResult executeReturnImpl(Value result);
    /// R98 W2: 高阶函数闭包同步调用。手动构造 RegCallFrame + 内部指令循环，
    /// 执行闭包体直到帧弹出。返回值通过 returnReg=dstReg 写入调用者寄存器，
    /// 循环结束后从 reg(dstReg) 读取到 result。
    /// 镜像 VM::invokeClosureSync 的设计（StackVM 用栈，RegisterVM 用寄存器）。
    VMResult invokeClosureSync(const Value& closure, const Value* args, size_t argCount, uint8_t dstReg, int line,
                               int column, Value& result);
    VMResult executeMethodCallImpl(size_t& ip, const std::string& methodName, uint8_t argCount, uint8_t dstReg,
                                   uint8_t objReg, const SmallArgs<uint8_t>& argRegs);
    VMResult executeClosureImpl(size_t& ip, const std::string& name, uint8_t uvCount, const SmallArgs<uint8_t>& uvSpecs,
                                uint8_t dstReg);
    VMResult executeClassNewImpl(size_t& ip, const std::string& className, uint8_t argCount, uint8_t dstReg,
                                 const SmallArgs<uint8_t>& argRegs);
    bool fillDefaultArgs(const RegBytecodeChunk& chunk, uint8_t& argCount, const std::string& funName,
                         std::vector<Value>& defaults);

    /// W3-2-Bug2 fix: 在类定义时为有 upvalue 的方法创建 upvalue 捕获。
    /// 遍历 classInfo_[className].methods 中的方法，查 functionChunks_ 获取 chunk，
    /// 对 chunk->upvalues 非空的方法创建 VMClosureData，从当前帧捕获寄存器槽
    /// （isLocal=true）或 upvalue（isLocal=false）。镜像 StackVM VM::captureMethodUpvalues。
    void captureMethodUpvalues(const std::string& className);

    // R164 协程/生成器：与 StackVM::VM 对称的协程支持
    /// 生成器函数调用拦截——从寄存器读参数，创建协程值写入 dstReg。
    /// 被 executeCallOps（REG_CALL）和 executeMethodCallOps 共用。
    VMResult createCoroutineValue(const RegBytecodeChunk& genChunk, const std::string& funName, uint8_t argCount,
                                  uint8_t dstReg, const SmallArgs<uint8_t>& argRegs, const Value* closureValue);
    /// 协程 .next() 重放执行——设置帧、运行内部循环、捕获 RegVMYieldSignal。
    Value callCoroutineNext(Value& coroVal);
    /// 协程方法分发（.next() / .done()）。
    /// 返回 true=已处理（caller 应 return，检查 hasError_）；false=未匹配协程方法（caller 继续查找）。
    bool dispatchCoroutineBuiltin(Value& obj, const std::string& methodName, SmallArgs<Value>& args, Value& result);

    // 内建方法
    // C-9 fix: 返回 bool 而非 VMResult。true=已处理（caller 应 return，检查 hasError_），
    // false=未匹配内建方法（caller 继续查找用户定义方法）。
    // 原实现无论是否匹配都返回 VM_OK，导致实例方法调用被静默吞掉（callBuiltinMethod
    // 对 instance 类型 fallthrough 到末尾 return VM_OK，caller 误以为已处理）。
    bool callBuiltinMethod(Value& obj, const std::string& methodName, SmallArgs<Value>& args, Value& result);

    // callBuiltinMethod 子分类（按 obj 类型拆分，避免单函数过长）
    // method 由主函数 classifyBuiltinMethod 一次性确定后传入，避免子函数重复分类。
    // methodName 仅用于错误消息（"X 没有方法 Y"）。
    bool callArrayBuiltinMethod(Value& obj, BuiltinMethod method, const std::string& methodName, SmallArgs<Value>& args,
                                Value& result);
    bool callDictBuiltinMethod(Value& obj, BuiltinMethod method, const std::string& methodName, SmallArgs<Value>& args,
                               Value& result);
    bool callStringBuiltinMethod(Value& obj, BuiltinMethod method, const std::string& methodName,
                                 SmallArgs<Value>& args, Value& result);
};
