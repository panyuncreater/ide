/**
 * @file compiler/VM.h
 * @brief 栈式字节码虚拟机（MiniLang 三套执行引擎之一）。
 *
 * 执行 Compiler 编译生成的 BytecodeChunk（约 60 种 OpCode）。
 * 与 Interpreter 树遍历后端语义等价，与 RegisterVM 寄存器式后端共享
 * 大量逻辑（BuiltinMethods / NumericUtils / ErrorFormat / Utf8Utils）。
 *
 * 核心特性：
 *   - 操作数栈：VMStack 定长数组 Value[1024]，PERF-13 替代 std::vector
 *   - 调用帧：std::vector<VMCallFrame>，MAX_FRAMES=256
 *   - 闭包 upvalue：openUpvalues_ multimap，按 stackSlot 排序，
 *     closeUpvaluesFrom O(log n + k) 关闭
 *   - 异常处理：tryStack_ 搜索处理器，跨帧传播
 *   - 全局变量：slot-based（编译期分配）+ map-based（运行时 fallback）
 *   - 内联缓存：callCache_ / globalCache_ / methodCache_ 三级缓存
 *   - 单步调试：initExecution + stepOnce + VMStepInfo 回调
 *
 * 性能优化：
 *   - PERF-13: VMStack 替代 vector，零堆分配
 *   - PERF-14: 内联缓存 unordered_map 替代固定数组线性扫描
 *   - PERF-12: Value 8 字节 NaN-boxing，栈缓存局部性 3 倍
 *   - #12 fix: methodsByClass_ 两级索引消除继承链字符串拼接
 *
 * 调试支持：
 *   - getCurrentIP/getCurrentLine/getCurrentChunkName（UI 高亮）
 *   - getCallStack（调用栈面板）
 *   - getStack/getGlobals（变量检视）
 *   - VMStepInfo 回调（每条指令执行后通知）
 *
 * @see IBackend BytecodeChunk Compiler RegisterVM Interpreter
 */
#pragma once

#include "Diagnostic.h"
#include "common/IBackend.h" // ARCH-09 fix: 后端抽象接口
#include "common/Result.h"
#include "common/RuntimeLimits.h"
#include "compiler/Bytecode.h"
#include "debug/DebugTypes.h"           // R161: WriteTarget 共享类型（Watchpoint peekWriteTarget 返回值）
#include "interpreter/BuiltinMethods.h" // #20 fix: BuiltinMethod 枚举 + classifyBuiltinMethod
#include "interpreter/Value.h"
#include <array> // C3: opcode profiling 计数数组
#include <cassert>
#include <cstdlib> // std::abort — Release 构建中 assert 兜底，避免 UB
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================
// VM 虚拟机（简单栈机）
// ============================================================

/// P13 fix: 小缓冲区优化的参数容器，避免方法调用时的堆分配
/// 对于 argCount <= N 使用栈上内联存储，超出时回退到 vector
template <typename T, size_t N = 8> class SmallArgs {
    T inline_[N];
    std::vector<T> heap_;
    size_t sz_ = 0;

public:
    // P0 fix: 显式 value-initialize inline_ 数组——原 `= default` 在某些场景下
    // 跳过元素默认构造，导致栈上 NaNBox 是随机位模式（包括 0xFFFFFFFFFFFFFFFF），
    // 后续被 NaNBox::tag() 误判为合法 FLOAT（NaN）掩盖底层 UB。
    SmallArgs() : sz_(0) {
        for (size_t i = 0; i < N; ++i)
            inline_[i] = T();
    }
    explicit SmallArgs(size_t count) : sz_(count) {
        for (size_t i = 0; i < N; ++i)
            inline_[i] = T();
        if (count > N)
            heap_.resize(count);
    }
    size_t size() const { return sz_; }
    bool empty() const { return sz_ == 0; }
    // P0 fix: assert 在 Release 被剥离，越界访问会读到栈垃圾（可能形成 0xFFFFFFFFFFFFFFFF），
    // 被当作合法 Value 使用。改为运行时 abort，与 VMStack 一致风格。
    T& operator[](size_t i) {
        if (i >= sz_) {
            std::abort();
        }
        return (sz_ <= N) ? inline_[i] : heap_[i];
    }
    const T& operator[](size_t i) const {
        if (i >= sz_) {
            std::abort();
        }
        return (sz_ <= N) ? inline_[i] : heap_[i];
    }
    T* begin() { return (sz_ <= N) ? inline_ : heap_.data(); }
    T* end() { return begin() + sz_; }
    const T* begin() const { return (sz_ <= N) ? inline_ : heap_.data(); }
    const T* end() const { return begin() + sz_; }
    T* data() { return begin(); }
    const T* data() const { return begin(); }
    void push_back(const T& v) {
        if (sz_ < N) {
            inline_[sz_++] = v;
        } else {
            if (sz_ == N) {
                heap_.assign(inline_, inline_ + N);
            }
            heap_.push_back(v);
            sz_++;
        }
    }
};

/// 虚拟机执行结果
enum class VMResult { VM_OK, VM_RUNTIME_ERROR, VM_STACK_OVERFLOW };

/// VM 执行模式
enum class VMExecMode {
    VM_MODE_NONE, // 未初始化
    VM_MODE_RUN,  // 全速运行
    VM_MODE_STEP  // 单步模式
};

/// 每条指令执行后的状态信息（用于调试/可视化）
// P3-1 fix: 加默认成员初始化器，新增字段时不会漏初始化导致未定义行为
struct VMStepInfo {
    size_t ip = 0;                   // 当前指令指针
    OpCode opcode = OpCode::OP_NULL; // 当前操作码
    size_t frameCount = 0;           // A4 fix: 当前调用帧栈深度（用于 step-over/out 语义）
};

// R164 协程/生成器：VM yield 信号（重放模式）
// OP_YIELD 命中目标 yieldId 时抛出，被 VM::callCoroutineNext 捕获。
// 与 Interpreter::YieldSignal 语义一致，但独立定义避免 Interpreter 依赖。
// 异常传播路径：executeCoroutineOps → executeOneInstruction → 内部循环 → callCoroutineNext catch
struct VMYieldSignal {
    Value yieldValue;
    explicit VMYieldSignal(Value v) : yieldValue(std::move(v)) {}
};

/// VM 调用帧
struct VMCallFrame {
    const BytecodeChunk* chunk = nullptr; // 当前执行的字节码块
    size_t ip = 0;                        // 当前帧的指令指针
    size_t returnIp = 0;                  // 返回后的 ip
    size_t basePointer = 0;               // 帧基指针（栈中参数起始位置）
    std::string functionName;             // 函数名
    bool isMethodCall = false;            // 是否为方法调用（需要 writeBack）
    bool isInitCall = false;              // 是否为 init 构造函数调用（返回 this 而非 null）
    std::string receiverVarName;          // 方法调用时，接收者的全局变量名（用于 writeBack 到 globals_）
    int receiverLocalSlot = -1;           // 方法调用时，接收者在调用者帧中的局部变量槽号（-1=非局部变量）
    bool fieldsModified = false;          // VM fix: 方法内是否修改了字段（用于跳过只读方法的字段同步）
    // VM-05/06: 闭包 upvalue 列表
    std::vector<std::shared_ptr<VMUpvalue>> upvalues;
};

/// VM 类信息（用于构造函数调用）
struct VMClassInfo {
    std::string name;                                     // 类名
    std::string superClassName;                           // 父类名（空表示无父类）
    std::vector<std::string> fieldOrder;                  // 字段声明顺序（含继承字段）
    std::unordered_map<std::string, Value> fieldDefaults; // 字段默认值（含继承字段）
    // P4 fix: 方法解析缓存（methodName -> chunk 指针），避免每次方法调用都拼接字符串+查继承链
    mutable std::unordered_map<std::string, const BytecodeChunk*> methodCache;
};

// ============================================================
// PERF-13: VMStack — 定长数组 + 栈顶指针，替代 std::vector<Value>
// ------------------------------------------------------------
// Value 从 24 字节降至 8 字节后，1024 元素仅 8KB，可直接内联到 VM 对象。
// 消除 push_back 的容量检查和潜在的堆分配开销。
// API 与 std::vector<Value> 兼容（size/empty/clear/push_back/pop_back/
// back/operator[]/resize/emplace_back/reserve），实现直接替换。
// ============================================================
class VMStack {
public:
    VMStack() : top_(0) {
        // 显式 value-initialize 所有元素为 nullValue。
        // 原 `Value data_[CAPACITY];` 在某些编译器/场景下可能跳过元素默认构造，
        // 导致栈上 NaNBox 是随机位模式（包括 0xFFFFFFFFFFFFFFFF），后续读取
        // 会被 NaNBox::tag() 误判为合法 FLOAT（NaN），掩盖底层 UB。
        for (size_t i = 0; i < CAPACITY; ++i) {
            data_[i] = Value::nullValue();
        }
    }

    // ---- 容量查询 ----
    size_t size() const { return top_; }
    bool empty() const { return top_ == 0; }
    static constexpr size_t capacity() { return CAPACITY; }

    // ---- 元素访问 ----
    // B6/P0 fix: assert 在 Release 构建被剥离，越界读会读到未初始化栈内存
    // （可能形成 0xFFFFFFFFFFFFFFFF 位模式，被 NaNBox 误判为合法 NaN float）。
    // 改为运行时 abort，与 RegisterVM::reg() 的 B3 fix 风格一致——显式失败优于静默继续。
    Value& operator[](size_t i) {
        if (i >= top_) {
            std::abort();
        }
        return data_[i];
    }
    const Value& operator[](size_t i) const {
        if (i >= top_) {
            std::abort();
        }
        return data_[i];
    }
    Value& back() {
        if (top_ == 0) {
            std::abort();
        }
        return data_[top_ - 1];
    }
    const Value& back() const {
        if (top_ == 0) {
            std::abort();
        }
        return data_[top_ - 1];
    }

    // ---- 栈操作 ----
    void push_back(const Value& v) {
        if (top_ >= CAPACITY) {
            std::abort();
        }
        data_[top_++] = v;
    }
    void push_back(Value&& v) {
        if (top_ >= CAPACITY) {
            std::abort();
        }
        data_[top_++] = std::move(v);
    }
    template <typename... Args> void emplace_back(Args&&... args) {
        if (top_ >= CAPACITY) {
            std::abort();
        }
        data_[top_++] = Value(std::forward<Args>(args)...);
    }
    void pop_back() {
        if (top_ == 0) {
            std::abort();
        }
        --top_;
    }

    // ---- 批量操作 ----
    void clear() { top_ = 0; }
    // B6/P0 fix: resize 仅能缩小，原 assert Release 被剥离可能导致 top_ 虚增
    // 读到未初始化槽位（栈垃圾），改为运行时 abort。
    void resize(size_t n) {
        if (n > top_) {
            std::abort();
        }
        top_ = n;
    }
    /// no-op：定长数组无需预分配
    void reserve(size_t) {}

    /// 转换为 vector（用于 GUI 调试视图，按值返回）
    std::vector<Value> toVector() const { return std::vector<Value>(data_, data_ + top_); }

private:
    static constexpr size_t CAPACITY = RuntimeLimits::MAX_STACK_SIZE;
    Value data_[CAPACITY];
    size_t top_;
};

/// 简单栈式虚拟机
class VM : public IBackend {
public:
    VM();

    /// ARCH-09 fix: IBackend 实现 — 后端名称
    std::string backendName() const override { return "VM"; }

    /// 执行编译结果（一次性全部执行，全速模式）
    VMResult execute(const CompileResult& result);

    /// 初始化执行环境但不运行（单步模式前置操作）
    void initExecution(const CompileResult& result);

    /// 单步执行一条指令（需先调用 initExecution）
    VMResult stepOnce();

    /// 是否执行完毕（无帧可执行）
    bool isFinished() const;

    /// 是否已初始化（initExecution 已调用）
    bool isInitialized() const;

    /// 重置 VM 状态（清理栈/帧/变量）
    void resetState();

    /// 设置输出回调（ARCH-09 fix: IBackend override）
    void setOutputCallback(std::function<void(const std::string&)> callback) override;

    /// 设置输入回调（用于 input() 函数）
    /// 回调接收提示字符串，返回用户输入的字符串
    /// ARCH-09 fix: IBackend override
    void setInputCallback(std::function<std::string(const std::string&)> callback) override;

    /// 设置指令级步进回调（每条指令执行后调用）
    void setStepCallback(std::function<void(const VMStepInfo&)> callback);

    /// 设置是否启用步进回调（默认关闭，避免性能开销）
    void setStepCallbackEnabled(bool enabled);

    /// 是否发生过运行时错误
    bool hasError() const;

    /// 获取最后的运行时错误
    std::string getLastError() const;

    /// 获取最后错误的源码行号（1-based，0=无位置信息）
    int getLastErrorLine() const;

    /// 获取诊断信息（ARCH-09 fix: IBackend override）
    const DiagnosticBag& getDiagnostics() const override { return diagnostics_; }

    /// 获取诊断信息（简短访问器）
    const DiagnosticBag& diagnostics() const { return diagnostics_; }

    /// 获取栈内容（用于调试，拷贝）
    std::vector<Value> getStack() const;

    /// 获取全局变量表（合并 slot-based + map-based，调试/测试用）
    std::unordered_map<std::string, Value> getGlobals() const {
        std::unordered_map<std::string, Value> result;
        // B4: 从 globalNameToSlot_ 重建逆映射（避免存储冗余的 globalSlotNames_）
        for (const auto& kv : globalNameToSlot_) {
            int slot = kv.second;
            if (slot >= 0 && slot < static_cast<int>(globalSlots_.size()) && !globalSlots_[slot].isNull()) {
                result[kv.first] = globalSlots_[slot];
            }
        }
        // Map-based globals overlay (runtime-defined, class markers, etc.)
        for (const auto& kv : globals_) {
            result[kv.first] = kv.second;
        }
        return result;
    }

    /// 向后兼容：旧版 getGlobalsRef 改为调用 getGlobals
    std::unordered_map<std::string, Value> getGlobalsRef() const { return getGlobals(); }

    /// 获取当前帧的 IP（用于单步调试 UI 高亮）
    size_t getCurrentIP() const;

    /// 获取当前帧的指令操作码（用于单步调试 UI 显示）
    OpCode getCurrentOpCode() const;

    /// 获取当前帧的源码行号（从当前 chunk 的 lines 数组获取）
    int getCurrentLine() const;

    /// 获取当前帧的 chunk 名称（"main" 或函数名）
    std::string getCurrentChunkName() const;

    /// A4 fix: 获取当前调用帧栈深度（用于 step-over/out 判断）
    size_t getFrameCount() const { return frames_.size(); }

#ifdef MINILANG_VM_PROFILING
    // ============================================================
    // C3: VM 解释循环热路径 profiling
    // ------------------------------------------------------------
    // 编译时启用（-DMINILANG_VM_PROFILING=ON）后，每条 opcode 的执行计数被
    // 记录到 opProfileCounts_[static_cast<uint8_t>(op)]。execute() 完成后可通过
    // getOpCodeProfile() 拿到排序后的 (opcode, count) 列表，用于识别热路径
    // 并指导手动内联/特殊化优化。
    // 注：profiling 启用会引入每指令 ~1ns 计数器自增开销（< 1% 性能影响）。
    // ============================================================
    struct OpCodeProfileEntry {
        OpCode op;
        uint64_t count;
    };
    /// 取按计数降序排列的 opcode 使用统计（profiling 启用时有效）
    std::vector<OpCodeProfileEntry> getOpCodeProfile() const;
    /// 重置 opcode 计数器（多次 execute 间分场景统计时使用）
    void resetOpCodeProfile();
#endif

    /// A4 fix: 获取调用栈快照（用于 UI 调用栈面板显示）
    /// 返回从栈底到栈顶的调用帧信息（函数名 + 当前行号 + ip）
    struct VMCallStackEntry {
        std::string functionName; // "main" 或函数名
        int line;                 // 当前源码行号
        size_t ip;                // 当前指令指针
    };
    std::vector<VMCallStackEntry> getCallStack() const;

    /// BUG-IDE-12 fix: 获取当前帧的局部变量名→值映射（用于 VM 条件断点求值）。
    /// 结合当前帧 chunk 的 localSlotNames + 栈槽（basePointer + slot）反查。
    /// 空帧/主程序帧（无 localSlotNames）返回空映射。
    std::unordered_map<std::string, Value> getCurrentFrameLocals() const;

    /// P2-3 fix: 获取指定帧的局部变量名→值映射（用于 VM 调用栈面板显示各帧 locals）。
    /// frameIndex 从 0 开始（0=栈底 main 帧）。越界或无 localSlotNames 返回空映射。
    std::unordered_map<std::string, Value> getFrameLocalsAt(size_t frameIndex) const;

    /// R104 Function Breakpoint：在 OP_CALL / OP_CALL_EXPR 指令执行前查询被调用函数名。
    /// 由 VmStepper 在 pre-execution 检测时调用，命中函数断点则暂停。
    /// @return 若当前指令是 OP_CALL/OP_CALL_EXPR，返回常量池中的函数名；否则返回空字符串。
    std::string peekCalledFunctionName() const;

    /// R104 Exception Breakpoint：检查当前 IP 指向的指令是否为 OP_THROW。
    /// 由 VmStepper 在 pre-execution 检测时调用，命中异常断点则暂停（throw 前）。
    /// @return true 表示当前指令是 OP_THROW
    bool isCurrentThrowInstruction() const;

    // ---- R161 Watchpoint（数据断点）pre-execution peek ----
    /// R161: pre-execution peek 当前 IP 指令的写入目标（不执行指令）。
    /// 用于数据断点（Watchpoint）在写入指令执行前检查。
    /// @return WriteTarget{isWrite=true, ...} 若当前指令是 SET 类指令；否则 isWrite=false
    /// WriteTarget 类型定义在 debug/DebugTypes.h（VM/RegisterVM 共享）
    WriteTarget peekWriteTarget() const;

    /// R114 阶段 3：从快照恢复 VM 状态（状态回滚）。
    /// 仅在 initExecution 已调用（initialized_==true）后可用。
    /// 恢复语义：(1) 截断 frames_ 到 targetFrameCount 并设置栈顶帧 ip=targetIp；
    /// (2) 替换 stack_ 为 stackValues（截断到 MAX_STACK_SIZE）；(3) 按 name 更新
    /// globalSlots_/globals_（已有变量覆盖，新变量忽略——回滚不应创建编译期未注册的变量）；
    /// (4) 清理 openUpvalues_（slots >= 新栈大小）、tryStack_（frameIndex >= targetFrameCount）、
    /// pendingJumpStack_（break/continue 跨回滚未定义，保守清空）、lastError_/hasError_/
    /// lastMutatedReceiver_。
    /// @note 不恢复 frames_ 的内部字段（chunk 指针、basePointer、upvalues 等），因
    ///       快照不捕获这些；调用方应保证 targetFrameCount <= 当前 frames_.size()
    ///       （仅截断不扩展）。若 targetFrameCount > frames_.size() 返回 false。
    /// @return true 成功；false 未初始化或 targetFrameCount 越界
    bool restoreFromSnapshot(const std::vector<Value>& stackValues,
                             const std::vector<std::pair<std::string, Value>>& globalsValues, size_t targetIp,
                             size_t targetFrameCount);

private:
    VMStack stack_;                                  // PERF-13: 定长数组操作数栈
    std::unordered_map<std::string, Value> globals_; // 全局变量表（runtime-defined fallback）
    // A2: 全局变量整数槽位存储（编译期分配，vector 直接访问）
    std::vector<Value> globalSlots_; // slot-indexed 存储
    // B4: globalSlotNames_ 已删除（仅调试用，getGlobals 从 globalNameToSlot_ 重建逆映射）
    std::unordered_map<std::string, int> globalNameToSlot_; // name -> slot (runtime lookup + 调试重建)
    std::vector<VMCallFrame> frames_;                       // 调用帧栈
    BytecodeChunk mainChunk_;                               // 主 chunk 副本（VM 自持，避免悬空指针）
    // MEM-03/MEM-04 fix: 改用 std::map（节点式，插入不使引用/迭代器/指针失效）。
    // 原 unordered_map 在 rehash 后会使所有 VMClosureData::chunkPtr 和
    // VMCallFrame::chunk（裸指针）悬垂。std::map 的节点稳定性消除该风险。
    std::map<std::string, BytecodeChunk> functionChunks_; // 函数字节码

    // P3 fix: 函数调用内联缓存（按常量池字符串指针 O(1) 查找）
    // PERF-14 fix: 改用 unordered_map 替代固定 32 项数组线性扫描，
    // 函数数量较多时查找由 O(cache_size) 降为 O(1) 平均。
    // key 是 chunk.constants[idx].stringVal() 的指针（chunk 自持 mainChunk_ 副本，
    // functionChunks_ 是 std::map 节点稳定，故 key 在 chunk 生命周期内有效）。
    std::unordered_map<const std::string*, const BytecodeChunk*> callCache_;

    // P2 fix: 全局变量内联缓存
    // PERF-14 fix: 改用 unordered_map 替代固定 32 项数组线性扫描。
    // MEM-05 fix: generation 由原 bucket_count() 单一维度改为
    // (bucket_count() << 16) ^ size() 组合，erase 不会改变 bucket_count 但会改变 size，
    // 使缓存条目自动失效，避免命中已被 erase 的全局变量槽位。
    struct GlobalCacheEntry {
        Value* valuePtr = nullptr; // 指向 globals_ 中的 Value（rehash 后失效）
        size_t generation = 0;     // globals_ 状态快照（检测 rehash / erase）
    };
    std::unordered_map<const std::string*, GlobalCacheEntry> globalCache_;
    // R97 #3 fix: 移除 lastAsciiStr* 4 字段缓存，改为 StringData::cachedIsAscii 持久缓存。
    // 原实现按 (ptr, size, firstByte) 三重验证缓存上次 ASCII 判定结果，存在堆地址复用
    // 误命中风险（虽然概率极低）。新方案 isAscii 直接存在 StringData 内，与字符串生命周期
    // 绑定，O(1) 读取无碰撞风险，且省去 4 个字段的 initExecution/resetState 重置开销。
    std::unordered_map<std::string, VMClassInfo> classInfo_; // 类信息注册表
    // R99 enum 校验：enum 元信息注册表（enum 名→variant 列表），initExecution 从
    // CompileResult.enumInfos 加载，OP_BUILD_ENUM_VARIANT 校验 variant 名与 arity。
    // 对齐 Interpreter::enumRegistry_ 的运行时校验语义。
    std::unordered_map<std::string, VMEnumInfo> enumRegistry_;
    // #12 fix: 类→方法名→chunk 两级索引，替代 findMethodChunk 冷路径每层继承链
    // 拼 "Class.method" 字符串。在 initExecution 中扫描 functionChunks_ 一次性构建。
    // 仅收录 "Class.method" 格式条目（按首个 '.' 拆分），普通函数名（无 '.'）不入索引。
    // 安全性：functionChunks_ 为 std::map（节点稳定），BytecodeChunk* 在 functionChunks_
    // 生命周期内有效；methodsByClass_ 与 functionChunks_ 在 initExecution/resetState 同步重建。
    std::unordered_map<std::string, std::unordered_map<std::string, BytecodeChunk*>> methodsByClass_;
    // VM-05/06: 闭包支持
    // B5 fix: openUpvalues_ 改用按 stackSlot 排序的有序结构（multimap 允许多个 upvalue 共享同一栈槽），
    // closeUpvaluesFrom 从 O(n) 线性扫描降为 O(log n + k)。value 用 weak_ptr 监视 shared_ptr 生命周期
    // （closure 持有强引用），closure 销毁后 weak_ptr 自动过期，不阻碍 upvalue 释放。
    std::multimap<size_t, std::weak_ptr<VMUpvalue>> openUpvalues_;
    std::unordered_map<std::string, Value> functionClosures_;      // 函数名→闭包值（含 upvalue 绑定）
    std::function<void(const std::string&)> outputCallback_;       // 输出回调
    std::function<std::string(const std::string&)> inputCallback_; // 输入回调（input() 函数）
    // R136 spawn 子线程闭包调用序列化 mutex。VM 的栈/帧非线程安全，
    // spawn 出的子线程若并发调用 invokeClosureSync 会破坏这些共享状态。
    // 通过此 mutex 序列化所有 spawn 出的闭包调用（同一 VM 实例级别）。
    std::mutex spawnMutex_;
    std::function<void(const VMStepInfo&)> stepCallback_; // 步进回调
    bool stepCallbackEnabled_ = false;                    // 是否启用步进回调
    bool initialized_ = false;                            // 是否已初始化执行环境
    std::string lastError_;                               // 最近一次运行时错误
    int lastErrorLine_ = 0;                               // 最近一次运行时错误的源码行号（1-based，0=无位置）
    // P1 fix: mutable 允许 const peek() 在栈下溢时设置错误标志
    mutable bool hasError_ = false;              // 运行时错误标志（用于快速检测）
    DiagnosticBag diagnostics_;                  // 诊断收集器
    Value lastMutatedReceiver_;                  // 变异方法调用后暂存修改后的接收者对象（用于嵌套访问写回）
    std::vector<std::string> pendingFieldOrder_; // M3: OP_INIT_FIELD 执行期间记录的字段声明顺序

    // F11: 异常处理
    struct TryHandler {
        size_t catchIp;    // catch 块的 IP
        size_t stackBase;  // try 开始时的栈大小（catch 时恢复）
        size_t frameIndex; // 所属调用帧索引
    };
    std::vector<TryHandler> tryStack_; // try 处理器栈
    // AUDIT-P1.1 fix: break/continue finally 续跳机制。
    // break/continue 在 try-finally 内时，先 push 真实跳转目标到此栈，
    // 再 jump 到 finally 入口；finally 末尾的 OP_FINALLY_END 从此栈 pop 目标并跳转。
    // 异常传播时（throwException）清空此栈，因为异常中断了 break/continue 续跳链。
    std::vector<size_t> pendingJumpStack_;
    // S1 fix: 统一引用 common/RuntimeLimits.h，消除重复定义
    static constexpr size_t MAX_STACK_SIZE = RuntimeLimits::MAX_STACK_SIZE;
    static constexpr size_t MAX_FRAMES = RuntimeLimits::MAX_FRAMES;
    static constexpr int64_t MAX_INSTRUCTIONS = RuntimeLimits::MAX_INSTRUCTIONS;
    // P1 fix: stepOnce 累计指令计数器，防止通过循环调用 stepOnce 绕过 DoS 防护
    int64_t stepInstructionCount_ = 0;
    static constexpr int MAX_INHERITANCE_DEPTH = RuntimeLimits::MAX_INHERITANCE_DEPTH;

    // R164 协程/生成器：重放模式状态
    // currentCoroutineTargetYieldId_ >= 0 表示当前在协程重放上下文中（OP_YIELD 据此判定）。
    // 每次 .next() 开始时设为 cd->currentYieldId，callCoroutineNext 结束时恢复为 -1。
    int currentCoroutineTargetYieldId_ = -1;
    // 运行时 yield 执行计数器：每次 OP_YIELD 递增，用于区分循环内同一 yield 节点的多次执行。
    // 每次 .next() 重放开始时重置为 0（对齐 Interpreter::currentYieldExecutionCount_）。
    int currentYieldExecutionCount_ = 0;

#ifdef MINILANG_VM_PROFILING
    // C3: opcode 执行计数数组，索引 = static_cast<uint8_t>(OpCode)
    // 256 项覆盖所有可能的 opcode（uint8_t 范围），未使用 opcode 计数为 0。
    // 数组在 VM 构造时零初始化（std::array<uint64_t, 256> 默认 value-init 为 0）。
    std::array<uint64_t, 256> opProfileCounts_{};
#endif

    /// 栈操作
    void push(const Value& val);
    void push(Value&& val);
    Value pop();
    const Value& peek(size_t distance = 0) const;
    // PERF-12 fix: 引用语义 + 栈槽复用优化
    /// 非 const peek：返回栈顶引用，允许直接修改栈顶元素，避免 pop+push 往返原子操作
    Value& peekRef(size_t distance = 0);

    // Dedup-7B: 解析可变全局变量引用。先查 globalSlots_（编译期槽位），
    // 找不到再 fallback 到 globals_（runtime-defined）。失败返回 nullptr。
    // 消除 VMContainers.cpp 中 4 处 OP_*_VAR 操作码的重复 lookup 模式。
    Value* resolveMutableGlobal(const std::string& varName) {
        auto gsIt = globalNameToSlot_.find(varName);
        if (gsIt != globalNameToSlot_.end() && gsIt->second >= 0 &&
            gsIt->second < static_cast<int>(globalSlots_.size())) {
            return &globalSlots_[gsIt->second];
        }
        auto it = globals_.find(varName);
        if (it == globals_.end())
            return nullptr;
        return &it->second;
    }
    /// 批量 pop：一次 resize 替代多次 pop_back，避免多次析构 + 容量抖动。
    /// 不返回弹出值（调用方已通过 peek 读过），用于函数调用参数清理等场景。
    void popN(size_t n);
    /// 在栈顶直接 emplace 构造 Value，避免临时 Value 构造+拷贝。
    /// PERF-13: VMStack 使用定长数组，emplace_back 直接写入栈槽。
    template <typename... Args> void emplace(Args&&... args) {
        if (stack_.size() >= MAX_STACK_SIZE) {
            runtimeError("栈溢出");
            return;
        }
        stack_.emplace_back(std::forward<Args>(args)...);
    }

    /// 运行时错误
    VMResult runtimeError(const std::string& msg);

    /// F11: 抛出异常，搜索 try 处理器或跨帧传播
    VMResult throwException(Value thrownValue);

    /// F11-fix: 关闭指向 [fromSlot, stack_.size()) 范围内栈槽的 open upvalues
    /// 用于异常展开和帧弹出时防止悬垂指针
    /// 返回 VM_RUNTIME_ERROR 表示检测到 slot 越界（hasError_ 已设置，调用方应立即 return 传播错误）
    VMResult closeUpvaluesFrom(size_t fromSlot);

    /// F10-fix: 为 init 方法填充缺失的默认参数，返回 true 表示成功
    /// argCount 会被更新为填充后的参数数量，默认值追加到 defaults 向量
    bool fillDefaultArgs(const BytecodeChunk& chunk, uint8_t& argCount, const std::string& funName,
                         std::vector<Value>& defaults);

    /// 数值二元运算（枚举分发）
    VMResult numericOp(int opType);

    /// 比较运算结果写回（消除 6 个比较运算符的重复代码）
    /// ip 按引用传入，结果写入后自动 +1（所有比较指令均为 1 字节）
    VMResult pushCompareResult(bool result, size_t& ip, OpCode opcode);

    /// 写回变异方法调用后的接收者（统一数组/字典/实例三处写回逻辑）
    /// receiverVarIdx: 接收者的全局变量索引（0xFFFF 表示无）
    /// receiverLocalSlotByte: 接收者的本地槽字节（0xFF 表示无）
    /// mutatedObj: 被修改的对象引用（将被 std::move）
    /// fieldsModified: 是否修改了字段（影响实例字段同步）
    VMResult writeBackReceiver(uint16_t receiverVarIdx, uint8_t receiverLocalSlotByte, Value& mutatedObj,
                               bool fieldsModified);

    /// 有序比较：类型检查 + 比较 + 结果写回（<, >, <=, >= 共用模板）
    template <typename Cmp> VMResult orderedCompare(Cmp cmp, size_t& ip, OpCode opcode) {
        if (stack_.size() < 2)
            return runtimeError("栈下溢：比较运算需要两个操作数");
        const Value& right = stack_.back();
        const Value& left = stack_[stack_.size() - 2];
        // V2 fix: 支持字符串字典序比较，与解释器 M4 fix 一致
        if (left.isString() && right.isString()) {
            return pushCompareResult(cmp(left, right), ip, opcode);
        }
        if (!left.isNumber() || !right.isNumber())
            return runtimeError("比较运算需要数值或字符串类型");
        return pushCompareResult(cmp(left, right), ip, opcode);
    }

    // ---- B7 fix: 内建方法分发（从 executeCallOps 提取，降低圈复杂度）----
    /// 数组内建方法分发。返回 VM_OK 表示已处理（caller 应 break），VM_RUNTIME_ERROR 表示出错。
    VMResult dispatchArrayBuiltin(const Value& obj, BuiltinMethod method, const std::string& methodName,
                                  uint8_t argCount, uint16_t receiverVarIdx, uint8_t receiverLocalSlotByte, size_t& ip,
                                  OpCode op, int instrLen);
    /// 字典内建方法分发。语义同上。
    VMResult dispatchDictBuiltin(const Value& obj, BuiltinMethod method, const std::string& methodName,
                                 uint8_t argCount, uint16_t receiverVarIdx, uint8_t receiverLocalSlotByte, size_t& ip,
                                 OpCode op, int instrLen);
    /// 字符串内建方法分发（全部非变异，无需 writeBack 参数）。语义同上。
    VMResult dispatchStringBuiltin(const Value& obj, BuiltinMethod method, const std::string& methodName,
                                   uint8_t argCount, size_t& ip, OpCode op, int instrLen);
    /// R136 同步对象方法分发（channel/mutex/rwlock/thread）。
    /// 同步对象内部状态通过 shared_ptr<Inner> 共享，方法调用不修改 Value 本身（无需 writeBack）。
    VMResult dispatchSyncObjectBuiltin(const Value& obj, const std::string& methodName, uint8_t argCount, size_t& ip,
                                       OpCode op, int instrLen);
    /// R164 协程/生成器方法分发（.next() / .done()）。
    /// 协程内部状态通过 CoroutineData* 共享指针修改，无需 writeBack。
    VMResult dispatchCoroutineBuiltin(Value& obj, const std::string& methodName, uint8_t argCount, size_t& ip,
                                      OpCode op, int instrLen);
    /// R164 D.5: 生成器函数调用拦截——从栈弹出参数，创建协程值 push 到栈顶。
    /// 被 executeCallFunction（OP_CALL）和 executeCallExprValue（OP_CALL_EXPR）共用。
    VMResult createCoroutineValue(const BytecodeChunk& genChunk, const std::string& funName, uint8_t argCount,
                                  Value closureVal, size_t& ip, int instrLen);
    /// R164 D.5: 协程 .next() 重放执行——设置帧、运行内部循环、捕获 VMYieldSignal。
    Value callCoroutineNext(Value& coroVal);

    // ---- P0-3 fix: 共享内置方法分派样板提取 ----
    /// 非变异方法完成：检查错误 → pop 接收者 → push 结果 → 推进 ip。
    /// 用于 dispatchArrayBuiltin/dispatchDictBuiltin 的非变异路径。
    VMResult finishSharedBuiltin(Result<Value>&& sr, size_t& ip, OpCode op, int instrLen);

    /// 提取共享方法结果：检查错误 → 移动值到 out。
    /// 用于 dispatchStringBuiltin（pop/push 延迟到函数末尾统一执行）。
    /// @return true 成功；false 失败（已设置 hasError_，caller 应 return VM_RUNTIME_ERROR）
    bool extractSharedBuiltin(Result<Value>&& sr, Value& out);

    // ---- P2-9 fix: COW 变异模式提取 ----
    /// 获取数组的可变引用：若独占拥有数据（refcount==1）直接返回；
    /// 否则通过非 const arrayVal() 触发 COW detach 后返回。
    /// 消除 dispatchArrayBuiltin 中 4 处重复的 tryGetMutableArray 模式。
    std::vector<Value>& getMutableArrayRef(Value& obj) {
        auto* arr = obj.tryGetMutableArray();
        if (arr)
            return *arr;
        return obj.arrayVal();
    }

    /// 获取字典的可变引用：同上，针对字典类型。
    /// L4 fix: 返回类型改为 DictKey-keyed map（dictVal/tryGetMutableDict 已同步）
    /// R97 #2 fix: 返回类型改为 Value::DictMap（含 DictKeyEqual 透明比较器）
    Value::DictMap& getMutableDictRef(Value& obj) {
        auto* dict = obj.tryGetMutableDict();
        if (dict)
            return *dict;
        return obj.dictVal();
    }

    /// 通知步进回调（内联：禁用时直接返回，避免函数调用开销）
    void notifyStep(size_t ip, OpCode opcode) {
        if (!stepCallbackEnabled_)
            return;
        VMStepInfo info;
        info.ip = ip;
        info.opcode = opcode;
        info.frameCount = frames_.size(); // A4 fix: 暴露调用深度供 step-over/out 判断
        stepCallback_(info);
    }

    /// 获取当前帧
    // D3 fix: 补 const 重载，对齐 RegisterVM::currentFrame() const，便于调试器只读访问
    VMCallFrame& currentFrame();
    const VMCallFrame& currentFrame() const;

    /// 获取当前 chunk
    // D3 fix: 标记为 const（已返回 const 引用，方法本身应 const，与 findMethodChunk 一致）
    const BytecodeChunk& currentChunk() const;

    /// 沿继承链查找方法 chunk（返回 nullptr 表示未找到）
    /// 先在 className 对应类查 methodName，未命中则查 superClass，递归到根。
    const BytecodeChunk* findMethodChunk(const std::string& className, const std::string& methodName) const;

    /// 按指令类别执行指令（executeOneInstruction 内部转发）
    VMResult executeConstantOps(OpCode op, size_t& ip);
    VMResult executeArithOps(OpCode op, size_t& ip);
    VMResult executeCompareOps(OpCode op, size_t& ip);
    VMResult executeVarOps(OpCode op, size_t& ip);
    VMResult executeCallOps(OpCode op, size_t& ip);
    VMResult executeContainerOps(OpCode op, size_t& ip);
    VMResult executeWritebackOps(OpCode op, size_t& ip);
    VMResult executeMiscOps(OpCode op, size_t& ip);
    /// R164 协程/生成器：OP_YIELD 指令执行（重放模式）
    VMResult executeCoroutineOps(OpCode op, size_t& ip);

    // ---- R132-D fix: executeWritebackOps 200 行拆为 thin dispatcher + 3 helper ----
    // 按"写回目标"分组：全局变量/栈槽/upvalue 各一个 helper，每 helper 处理 MEMBER+INDEX 两种 op。
    // MEMBER 与 INDEX 在同一存储类下语义等价（BUG-NEW fix 后均为整体替换 lastMutatedReceiver_），
    // 区别仅在操作数编码（MEMBER 多 2B fieldIdx 用于反汇编）。
    /// 写回到全局变量：OP_WRITEBACK_MEMBER_VAR / OP_WRITEBACK_INDEX_VAR。
    /// 通过 resolveMutableGlobal 取可变引用，整体替换为 lastMutatedReceiver_。
    VMResult writebackToGlobalVar(OpCode op, size_t& ip);
    /// 写回到栈槽：OP_WRITEBACK_MEMBER_LOCAL / OP_WRITEBACK_INDEX_LOCAL。
    /// 写入 stack_[bp+slot]，若 slot 是字段槽则标记 fieldsModified。
    VMResult writebackToStackSlot(OpCode op, size_t& ip);
    /// 写回到 upvalue：OP_WRITEBACK_MEMBER_UPVALUE / OP_WRITEBACK_INDEX_UPVALUE。
    /// 已关闭 upvalue 写入 uv->value；open upvalue 写入 stack_[uv->stackSlot]
    /// 并检查 owningFrame 字段槽范围，标记该帧 fieldsModified。
    VMResult writebackToUpvalue(OpCode op, size_t& ip);

    // ---- R131 fix: executeMiscOps 218 行二次拆分为 4 个 helper（原 218 行 → 每个方法 < 70 行）----
    /// 栈操作 + 字段初始化指令：OP_PRINT/OP_POP/OP_DUP/OP_SWAP/OP_LOAD_MUTATED/OP_DUP_N/OP_INIT_FIELD
    /// 共 7 个 case，均围绕栈顶元素操作（消费/复制/交换/字段写入栈顶实例）
    VMResult executeMiscStackOps(OpCode op, size_t& ip);
    /// 运行时类型注解检查（OP_TYPE_CHECK）：精确类型匹配 + 实例继承链查找（最深 64 层）
    VMResult executeMiscTypeCheck(OpCode op, size_t& ip);
    /// R134: 软类型测试（OP_TYPE_TEST）：与 OP_TYPE_CHECK 同语义但 push bool 而非抛错。
    /// 用于 TUPLE pattern 类型检查（不匹配时 fall through 而非抛错）。
    VMResult executeMiscTypeTest(OpCode op, size_t& ip);
    /// 无条件/条件跳转指令：OP_JUMP/OP_JUMP_IF_FALSE/OP_LOOP（共享 jump 目标越界检查模式）
    VMResult executeMiscJumpOps(OpCode op, size_t& ip);
    /// 异常处理 + finally 跳转栈指令：OP_TRY_BEGIN/OP_TRY_END/OP_THROW/OP_PUSH_JUMP_TARGET/OP_FINALLY_END
    /// 共 5 个 case，围绕 tryStack_/pendingJumpStack_ 两个异常机制栈管理
    VMResult executeMiscExceptionOps(OpCode op, size_t& ip);

    // ---- R118 fix: executeContainerOps 拆分为 4 个独立方法（原 524 行 → 每个方法 < 220 行）----
    /// 容器构造指令：OP_BUILD_ARRAY / OP_BUILD_DICT / OP_BUILD_TUPLE / OP_BUILD_ENUM_VARIANT
    VMResult executeContainerBuildOps(OpCode op, size_t& ip);
    /// 索引访问指令：OP_INDEX_GET / OP_INDEX_SET / OP_INDEX_SET_VAR / OP_INDEX_SET_LOCAL
    VMResult executeIndexOps(OpCode op, size_t& ip);

    // ---- R122 fix: executeIndexOps 拆分为 4 个独立方法（原 223 行 → 每个方法 < 95 行）----
    /// 索引读取（OP_INDEX_GET，跨 array/dict/string/tuple 多态含 ASCII 快速路径）
    VMResult executeIndexGet(size_t& ip);
    /// 嵌套索引赋值（OP_INDEX_SET，栈顶 obj+idx+val 存入 lastMutatedReceiver_）
    VMResult executeIndexSet(size_t& ip);
    /// 全局变量索引赋值（OP_INDEX_SET_VAR，通过 resolveMutableGlobal 取引用）
    VMResult executeIndexSetVar(size_t& ip);
    /// 局部变量索引赋值（OP_INDEX_SET_LOCAL，修改 stack_[bp+slot] 含 fieldsModified 同步）
    VMResult executeIndexSetLocal(size_t& ip);
    /// 成员访问指令：OP_SUPER_MEMBER_GET / OP_MEMBER_GET / OP_MEMBER_SET / OP_MEMBER_SET_VAR / OP_MEMBER_SET_LOCAL
    VMResult executeMemberOps(OpCode op, size_t& ip);
    /// 枚举查询指令：OP_ENUM_VARIANT_NAME / OP_ENUM_VARIANT_FIELD
    VMResult executeEnumOps(OpCode op, size_t& ip);

    // ---- R119 fix: executeVarOps 拆分为 4 个独立方法（原 289 行 → 每个方法 < 140 行）----
    /// 按名称变量指令：OP_DEFINE_VAR / OP_GET_VAR / OP_SET_VAR / OP_DELETE_VAR（含 slot 快速路径 + 内联缓存 + fallback
    /// globals_）
    VMResult executeVarNameOps(OpCode op, size_t& ip);
    /// 整数槽全局变量指令：OP_GET_GLOBAL / OP_SET_GLOBAL / OP_DEFINE_GLOBAL / OP_DELETE_GLOBAL
    VMResult executeGlobalSlotOps(OpCode op, size_t& ip);
    /// 局部变量指令：OP_GET_LOCAL / OP_SET_LOCAL（含 fieldsModified 标记同步）
    VMResult executeLocalOps(OpCode op, size_t& ip);
    /// Upvalue 闭包指令：OP_GET_UPVALUE / OP_SET_UPVALUE / OP_CLOSE_UPVALUE（含 open/closed 双路径 + owningFrameIdx
    /// 字段同步）
    VMResult executeUpvalueOps(OpCode op, size_t& ip);

    // ---- S1 fix: executeCallOps 拆分为 8 个独立方法（原 978 行 → 每个方法 < 200 行）----
    /// OP_RETURN 执行：方法调用返回、字段同步、栈帧弹出
    VMResult executeReturn(size_t& ip);
    /// OP_CALL / OP_CALL_EXPR 执行：函数调用（thin dispatcher，按 isExpr 分发到 executeCallByName /
    /// executeCallExprValue）
    VMResult executeCall(size_t& ip, bool isExpr);

    // ---- R123 fix: executeCall 470 行二次拆分为 7 个 helper（原 470 行 → 每个方法 < 130 行）----
    /// OP_CALL 路径分发器：按函数名查找缓存/classInfo_/input/higher-order/builtin/functionChunks_，
    /// 命中后分发到对应 callee 类型 helper（constructor/input/higher-order/builtin/function）
    VMResult executeCallByName(size_t& ip, OpCode op);
    /// 类构造调用（OP_CALL 路径 classInfo_ 命中）：创建实例 + findMethodChunk("init") + 默认参数填充 + 帧构造
    VMResult executeCallConstructor(size_t& ip, OpCode op, VMClassInfo& cls, const std::string& funName,
                                    uint8_t argCount);
    /// input() 内置函数（OP_CALL 路径 funName=="input"）：调用 executeSharedInput
    VMResult executeCallBuiltinInput(size_t& ip, OpCode op, uint8_t argCount, const BytecodeChunk& chunk);
    /// 高阶函数（OP_CALL 路径 isHigherOrderBuiltin 命中）：map/filter/reduce/forEach/find 分派
    VMResult executeCallHigherOrder(size_t& ip, OpCode op, const std::string& funName, uint8_t argCount,
                                    const BytecodeChunk& chunk);
    /// 内置函数（OP_CALL 路径 isBuiltinFunction 命中）：调用 executeSharedBuiltinFunction
    VMResult executeCallBuiltinFunction(size_t& ip, OpCode op, const std::string& funName, uint8_t argCount,
                                        const BytecodeChunk& chunk);
    /// R136 spawn(fn, args...) 内置函数（OP_CALL 路径 funName=="spawn"）：
    /// 构造 ClosureInvoker（加 spawnMutex_ 序列化）后调用 executeSharedSpawn
    VMResult executeCallSpawn(size_t& ip, OpCode op, uint8_t argCount, const BytecodeChunk& chunk);
    /// 普通函数调用（OP_CALL 路径 functionChunks_ 命中）：默认参数范围检查 + 调用 setupFunctionCallFrame
    VMResult executeCallFunction(size_t& ip, OpCode op, const std::string& funName, uint8_t argCount,
                                 const BytecodeChunk& targetChunk);
    /// 闭包值调用（OP_CALL_EXPR 路径 callee.isClosure）：栈布局调整 + chunkPtr 获取 + 调用 setupFunctionCallFrame
    VMResult executeCallExprValue(size_t& ip, OpCode op);
    /// 共享帧构造 helper：默认参数填充 + MAX_FRAMES 检查 + extraSlots 预分配 + newFrame 构造 + push frame
    /// 被 executeCallFunction 和 executeCallExprValue 共享，统一两路径的帧构造逻辑
    VMResult setupFunctionCallFrame(const BytecodeChunk& targetChunk, const std::string& functionName,
                                    uint8_t& argCount, size_t returnIp, size_t savedIp, OpCode op,
                                    const std::vector<std::shared_ptr<VMUpvalue>>& upvalues);
    /// OP_SUPER_CALL / OP_METHOD_CALL 执行：方法调用（thin dispatcher，按接收者类型分发到
    /// dispatchArrayBuiltin/dispatchDictBuiltin/dispatchStringBuiltin/executeInstanceMethodCall）
    VMResult executeMethodCall(size_t& ip, OpCode op);

    // ---- R131 fix: executeMethodCall 211 行二次拆分为 thin dispatcher + 1 个 helper ----
    /// 类实例方法调用：super 调用解析 + findMethodChunk 沿继承链查找 + 默认参数填充 +
    /// 字段槽位预填 + 局部变量 extraSlots 预分配 + 方法帧构造（含 writeBack 元信息）
    /// 提取自 executeMethodCall 的 instance 分支（145 行），是方法调用最复杂路径
    VMResult executeInstanceMethodCall(size_t& ip, OpCode op, Value obj, const std::string& methodName,
                                       uint8_t argCount, uint16_t receiverVarIdx, uint8_t receiverLocalSlotByte);
    /// OP_CLOSURE 执行：创建闭包值
    VMResult executeClosure(size_t& ip, OpCode op);
    /// OP_CLASS_NEW 执行：构造类实例
    VMResult executeClassNew(size_t& ip, OpCode op);
    /// OP_DEFINE_CLASS 执行：注册类信息
    VMResult executeDefineClass(size_t& ip, OpCode op);

    /// R98 W2: 同步调用闭包值（供高阶函数共享层回调）
    /// 手动构造调用帧（模拟 OP_CALL_EXPR 的帧设置），压入 frames_，
    /// 运行内部指令循环直到该帧弹出，从栈顶读取返回值。
    /// @param closure   闭包值
    /// @param args      参数列表首指针
    /// @param argCount  参数数量
    /// @param line      调用行号（错误报告）
    /// @param column    调用列号（错误报告）
    /// @param[out] result 闭包返回值（仅成功时有效）
    /// @return VM_OK 成功 / VM_RUNTIME_ERROR 失败（hasError_ 已设置）
    VMResult invokeClosureSync(const Value& closure, const Value* args, size_t argCount, int line, int column,
                               Value& result);

    /// 执行单条指令的内部实现（供 execute() 和 stepOnce() 共用）
    VMResult executeOneInstruction();
};
