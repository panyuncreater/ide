/**
 * @file compiler/JIT.h
 * @brief JIT 后端：基于 asmjit 的第四套执行引擎（R138-R141 演进）。
 *
 * 将 BytecodeChunk 编译为本地机器码（x86-64），直接在 CPU 上执行。
 * 与 StackVM 共享编译管线（输入 BytecodeChunk），是 StackVM 的"加速器"。
 *
 * 阶段 1 PoC 范围（R138）：
 *   - 仅处理 mainChunk（不支持函数调用）
 *   - 仅支持整数算术子集：OP_INT / OP_ADD / OP_SUBTRACT / OP_MULTIPLY /
 *     OP_NEGATE / OP_PRINT / OP_POP / OP_RETURN / OP_NULL / OP_TRUE / OP_FALSE
 *   - JIT 内部以 raw int64_t 表示 Value（不做 NaN-boxing），简化 PoC
 *
 * 阶段 2a 扩展范围（R139）：
 *   - 控制流：OP_JUMP / OP_JUMP_IF_FALSE / OP_LOOP（绝对偏移跳转，两遍扫描绑定 Label）
 *   - 全局变量 slot：OP_DEFINE_GLOBAL / OP_GET_GLOBAL / OP_SET_GLOBAL
 *   - 比较指令：OP_EQUAL / OP_NOT_EQUAL / OP_LESS / OP_GREATER /
 *     OP_LESS_EQUAL / OP_GREATER_EQUAL（cmp + setcc，结果为 0/1）
 *   - 逻辑取反：OP_NOT（test + sete，结果为 0/1）
 *
 * 阶段 2a+ 算术完善（R140）：
 *   - 整数除法：OP_DIVIDE（cqo + idiv，截断向零）
 *   - 整数取模：OP_MODULO（cqo + idiv，余数在 rdx）
 *   - 除零错误路径：test + jz → jitReportError → jmp epilogue
 *
 * 阶段 2b 函数调用（R141，后续版本持续扩展）：
 *   - 所有 chunk（mainChunk + functionChunks）编译到一个 CodeHolder
 *   - 每个 chunk 对应一个入口 Label，OP_CALL 用 jmp targetLabel
 *   - 局部变量：OP_GET_LOCAL / OP_SET_LOCAL（r13 = basePointer，[r13 - slot*8]）
 *   - 函数调用：OP_CALL（保存调用者帧 → 设置新 r13 → jmp 函数 Label）
 *   - 函数返回：OP_RETURN（恢复调用者帧 → jmp 返回地址；mainChunk return 跳 epilogue）
 *   - 闭包创建：OP_CLOSURE（R156 起支持 upvalue 捕获，含 closed/open 两种状态）
 *   - 闭包值调用：OP_CALL_EXPR（R155 实现，含 upvalue 绑定与默认参数补全）
 *   - 方法调用：OP_METHOD_CALL / OP_SUPER_CALL（R149 实现，含 init 自动调用与字段 writeBack）
 *   - 默认参数：编译期收集 defaultConstIndices，运行时按 missingCount 补全（R149/R155）
 *   - 递归深度限制：RuntimeLimits::MAX_FRAMES（256），超出报 recursion-depth-exceeded
 *
 * 阶段 2c 浮点原生与类型反馈（R152）：
 *   - 浮点原生：发射 SSE2 指令（movsd/addsd/subsd/mulsd/divsd），三层分派
 *     （INT 原生 → FLOAT 原生 → C++ helper 兜底），避免浮点走 C++ 辅助函数调用开销
 *   - 类型反馈：per-chunk TypeFeedback 计数器，重编译时决策 INT/FLOAT 特化策略
 *
 * 阶段 2d 异常处理（R162）：
 *   - try/catch/throw 结构化异常处理（OP_TRY_BEGIN/OP_TRY_END/OP_THROW）
 *   - finally 块与 break/continue 续跳（OP_PUSH_JUMP_TARGET/OP_FINALLY_END）
 *   - C++ 辅助函数 jitThrow 实现栈展开：搜索 tryStack_ → 截断操作数栈 → 跨帧传播
 *   - 跨帧异常传播：弹出 JitFrame、关闭 upvalue、恢复 r13/r15
 *   - JIT 代码通过 lea label 获取 catch 块绝对地址，jitThrow 返回该地址供 jmp
 *
 * GC 集成（P2-9 fix，R166）：
 *   - JIT 持有的 Value 引用（操作数栈/帧栈/全局槽位）以 NaN-boxing raw bits 表示，
 *     未注册为 GC roots。若 JIT 执行期间 Interpreter 的增量 GC 回调被触发
 *     （jitBuildArray 等 helper 分配容器累计达 8192 阈值），会以不完整 roots 集
 *     误回收 JIT 存活容器，导致 UAF。
 *   - 解决方案：JITBackend::execute() 入口构造 GcManager::CallbackSuppressor，
 *     原子保存并置空 gcTriggerCallback_，析构时恢复。JIT 期间增量 GC 被完全抑制，
 *     产生的循环引用孤岛由下一轮 Interpreter execute() 入口的兜底 collectCycle 回收
 *     （跨 execute 边界自动清理）。
 *   - 未实现 safepoint 轮询机制（在 OP_LOOP 回边处轮询 GC 请求标志并收集 JIT roots）：
 *     当前方案对教学场景足够（JIT 执行短任务，GC 延迟到下一轮 Interpreter 执行），
 *     长期运行场景的内存峰值可由 Interpreter 兜底回收控制。
 *
 * 尚不支持（截至 R166）：
 *   - 运行时错误不可捕获：除零/溢出/类型错误等 runtimeError 不触发 try/catch
 *     （与 StackVM 行为一致：try/catch 仅捕获显式 throw，不捕获运行时错误）
 *   - ARM64 移植（当前仅 x86-64）
 *
 * 教学价值：
 *   - 展示 JIT 编译原理：字节码 → 本地代码的直接映射
 *   - 与 StackVM dispatch loop 对比，演示 switch-based 解释执行 vs 本地代码性能差异
 *   - 展示寄存器分配、常量折叠（内联）、调用约定、跳转标签绑定等 JIT 核心概念
 *
 * 寄存器分配（R141 更新）：
 *   - r12 = JitContext* ctx（callee-saved，跨调用保留）
 *   - r13 = current frame basePointer（callee-saved，R141 新增，指向当前函数帧的第一个参数）
 *   - r15 = operand stack top（callee-saved，操作数栈顶，向低地址增长）
 *   - rax/rcx/rdx = 临时寄存器（算术运算、调用参数、比较结果）
 *   - rsp = 原生栈（仅用于函数调用 shadow space）
 *
 * 栈帧布局（rbp 相对寻址）：
 *   [rbp]      = saved rbp
 *   [rbp-8]    = saved r12
 *   [rbp-16]   = saved r13
 *   [rbp-24]   = saved r14
 *   [rbp-32]   = saved r15
 *   [rbp-40]   = operand stack base（r15 初始值，空栈）
 *   ...        = operand stack（向下增长，8KB = 1024 值，R141 扩大以支持函数调用）
 *
 * JitContext 内存布局（JIT 代码通过 r12 + offset 访问）：
 *   offset 0   : outputCallback (8B, std::function*)
 *   offset 8   : hasError       (8B, bool*)
 *   offset 16  : errorBuffer    (8B, std::string*)
 *   offset 24  : globalSlots    (8B, int64_t* 元素数组首地址) — R139 新增
 *   offset 32  : frames         (8B, JitFrame* 帧栈数组) — R141 新增
 *   offset 40  : frameCount     (8B, size_t* 当前帧深度指针) — R141 新增
 *   offset 48  : stackTop       (8B, int64_t* 操作数栈顶指针) — R146 新增
 *   offset 56  : classInfoPtr   (8B, unordered_map<string,JitClassInfo>*) — R148 新增
 *   offset 64  : pendingFieldOrderPtr (8B, vector<string>*) — R148 新增
 *   offset 72  : methodEntryPtr (8B, void* 方法入口地址邮箱) — R149 新增
 *   offset 80  : methodLocalCount (8B, int64_t 方法 localCount 邮箱) — R149 新增
 *   offset 88  : methodEntriesPtr (8B, unordered_map<string,JitMethodInfo>*) — R149 新增
 *   offset 96  : callerBp       (8B, int64_t* 调用者 r13 邮箱) — R149 新增
 *   offset 104 : chunkCallCounts (8B, uint64_t* per-chunk 调用计数数组) — R150 新增
 *   offset 112 : hotThresholds  (8B, uint64_t* per-chunk 热点阈值数组) — R151 新增
 *   offset 120 : recompiledFlags(8B, uint64_t* per-chunk 重编译标志数组) — R151 新增
 *   offset 128 : typeFeedback   (8B, TypeFeedback* per-chunk 类型反馈数组) — R152 新增
 *   offset 136 : backendPtr     (8B, JITBackend* this 指针) — R152 新增
 *   offset 144 : lastMutatedReceiverPtr (8B, int64_t* 嵌套左值赋值链中转邮箱) — R154 新增
 *   offset 152 : funcEntriesPtr   (8B, unordered_map<string,JitMethodInfo>*) — R155 新增
 *   offset 160 : currentBp        (8B, int64_t* 当前帧 basePointer r13) — R156 新增
 *   offset 232 : memberGetICPtr   (8B, MemberGetInlineCacheEntry*) — R160 新增
 *   offset 240 : globalsPtr       (8B, unordered_map<string,Value>*) — R162 新增
 *   JIT 代码访问 globalSlots[slot]: mov rcx, [r12+24]; mov rax, [rcx + slot*8]
 *   JIT 代码访问帧栈: mov rax, [r12+32]; mov rcx, [r12+40]; mov rcx, [rcx]
 *   R146 数组辅助函数访问 JIT 栈: JIT 代码在调用前 mov [r12+48], r15 更新栈顶指针，
 *   辅助函数通过 ctx->stackTop 读写栈，调用后 JIT 代码 mov r15, [r12+48] 恢复
 *
 * R141 帧栈布局（JitFrame 数组，R149 扩展为 72 字节）：
 *   [frames + idx*72 + 0]  : callerBp (int64_t*, 调用者 r13)
 *   [frames + idx*72 + 8]  : callerSp (int64_t*, 调用者 r15，调用前状态)
 *   [frames + idx*72 + 16] : returnAddr (void*, 返回地址)
 *   R149 方法调用扩展字段（isMethodCall=true 时有效）：
 *   [frames + idx*72 + 24] : receiverSlotPtr (int64_t*, 接收者 slot 地址，null=无 writeBack)
 *   [frames + idx*72 + 32] : methodFieldOrder (const std::vector<std::string>*, 方法 chunk 的 fieldOrder)
 *   [frames + idx*72 + 40] : methodBp (int64_t*, 方法帧 basePointer = this 位置)
 *   [frames + idx*72 + 48] : isMethodCall (int64_t, 0/1)
 *   [frames + idx*72 + 56] : isInitCall (int64_t, 0/1)
 *   [frames + idx*72 + 64] : fieldCount (int64_t, 方法 chunk 的字段数量)
 *
 * @see IBackend BytecodeChunk VM Compiler
 * @since R138
 */
#pragma once

#include "common/Diagnostic.h"
#include "common/IBackend.h"

#ifdef MINILANG_USE_JIT

#include "compiler/Bytecode.h"
#include <asmjit/asmjit.h>
#include <atomic>
#include <cstddef> // offsetof
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace minilang {

/// R151: 默认热点阈值（方法 chunk 达到此调用次数触发重编译回调）
/// 值较小以便测试快速触发；生产场景可调高（如 10000）
inline constexpr uint64_t kDefaultHotThreshold = 1000;

/// R152: 类型反馈计数器（per-chunk）
/// 在算术指令的 genericXXX 路径递增对应计数器，重编译时根据反馈决定特化策略：
/// - intCount == totalArithOps && otherCount == 0 → INT 特化（跳过 emitCheckInt）
/// - floatCount > 0 && otherCount == 0 → FLOAT 特化（未来支持）
/// - otherCount > 0 → 不特化（保持类型分派）
struct TypeFeedback {
    uint64_t intCount = 0;   ///< INT 类型观测次数（emitCheckInt 通过的次数，可选统计）
    uint64_t floatCount = 0; ///< FLOAT 类型观测次数（genericXXX 路径递增）
    uint64_t otherCount = 0; ///< 其他类型（STRING/OBJ/NULL）观测次数（genericXXX 路径递增）
};

/// R158: JIT 分层编译 Tier 枚举
/// 实现 V8 风格的分层编译（Tiered Compilation）：
/// - Tier 0 (Interpreter): 树遍历解释器，快速启动，无 JIT 开销
/// - Tier 1 (Baseline): 非特化 JIT，快速编译，包含类型反馈收集
/// - Tier 2 (Specialized): 特化 JIT，基于类型反馈的 INT/FLOAT 特化版本
/// 升级链：Tier 0 → Tier 1 → Tier 2（基于热点计数）
/// 降级链：Tier 2 → Tier 1（反优化 deoptimization，类型反馈失效时）
enum class JitTier : uint8_t {
    Interpreter = 0, ///< Tier 0: 解释器（JIT 不直接管理，仅作为概念层）
    Baseline = 1,    ///< Tier 1: Baseline JIT（非特化版本，含类型反馈收集）
    Specialized = 2, ///< Tier 2: 特化 JIT（INT/FLOAT 特化版本）
};

/// R158: OSR 栈帧快照（真正 OSR 栈帧迁移用）
/// OP_LOOP 回边达到 OSR 阈值时，JIT 代码将当前栈帧状态保存到此结构体，
/// 然后调用特化重编译生成含 OSR 入口点的特化版本，
/// 从 OSR 入口点恢复栈帧状态并继续执行（真正 OSR，非 R157 的"下次 execute 生效"）。
struct OsrFrameSnapshot {
    int64_t* frameBp = nullptr; ///< OSR 触发时的 r13（当前帧 basePointer）
    int64_t* frameSp = nullptr; ///< OSR 触发时的 r15（操作数栈顶）
};

} // namespace minilang

// ============================================================
// JIT 执行结果
// ============================================================
enum class JitResult {
    OK,           // 执行成功
    RuntimeError, // 运行时错误（JIT 代码执行期间）
    CompileError, // JIT 编译失败（不支持的指令等）
};

/// JIT 运行时帧（R141 新增，用于函数调用帧栈管理）
/// 每个 JitFrame 记录调用者的状态，供 OP_RETURN 恢复。
/// R149 扩展为 72 字节，添加方法调用 writeBack 相关字段。
/// 字段顺序固定，JIT 代码通过硬编码偏移访问（72 字节 per frame）。
struct JitFrame {
    int64_t* callerBp = nullptr;        ///< offset 0:  调用者 basePointer（r13）
    int64_t* callerSp = nullptr;        ///< offset 8:  调用者操作数栈顶（r15，调用前状态）
    void* returnAddr = nullptr;         ///< offset 16: 返回地址（JIT 代码内的指令地址）
    int64_t* receiverSlotPtr = nullptr; ///< offset 24: 接收者 slot 地址（null=无 writeBack，如临时表达式）
    const std::vector<std::string>* methodFieldOrder =
        nullptr;                 ///< offset 32: 方法 chunk 的 fieldOrder（writeBack 时按序同步字段槽）
    int64_t* methodBp = nullptr; ///< offset 40: 方法帧 basePointer（this 位置，writeBack 时读取修改后的 this）
    int64_t isMethodCall = 0;    ///< offset 48: 是否方法调用（1=是，需 writeBack）
    int64_t isInitCall = 0;      ///< offset 56: 是否 init 调用（1=是，返回 this 而非返回值）
    int64_t fieldCount = 0;      ///< offset 64: 方法 chunk 的字段数量
};

/// R148 JIT 运行时类信息
struct JitClassInfo {
    std::string name;                                     ///< 类名
    std::string superClassName;                           ///< 父类名（空表示无父类）
    std::vector<std::string> fieldOrder;                  ///< 字段声明顺序（含继承字段）
    std::unordered_map<std::string, Value> fieldDefaults; ///< 字段默认值（含继承字段）
};

/// R149 JIT 方法元信息（编译期收集，运行时 jitMethodCall 沿继承链查找用）
/// 键格式："ClassName.method"（与 StackVM functionChunks_ 键一致）
struct JitMethodInfo {
    void* entryPtr = nullptr;                                   ///< 方法入口地址（JIT 代码 jmp 用）
    int localCount = 0;                                         ///< 方法帧局部变量总槽位数
    int arity = 0;                                              ///< 含默认参数的总参数数
    int requiredArity = 0;                                      ///< 必需参数个数
    const std::vector<std::string>* fieldOrder = nullptr;       ///< 方法 chunk 的 fieldOrder（writeBack 用）
    const std::vector<uint16_t>* defaultConstIndices = nullptr; ///< 默认参数常量索引
    const std::vector<Value>* constants = nullptr;              ///< 方法 chunk 的常量池（默认参数值）
};

/// JIT 运行时上下文（传给 JIT 生成的本地代码）
/// 生命期由 JITBackend 拥有，JIT 代码通过 r12 寄存器访问此结构。
/// 字段顺序固定，JIT 代码通过硬编码偏移访问（offset 0/8/16/.../96）。
struct JitContext {
    std::function<void(const std::string&)>* outputCallback = nullptr; // offset 0:  print 输出回调
    bool* hasError = nullptr;                                          // offset 8:  错误标志
    std::string* errorBuffer = nullptr;                                // offset 16: 错误消息缓冲
    int64_t* globalSlots = nullptr; // offset 24: 全局变量 slot 元素数组首地址（R139 新增）
    JitFrame* frames = nullptr;     // offset 32: 帧栈数组首地址（R141 新增）
    size_t* frameCount = nullptr;   // offset 40: 当前帧深度指针（R141 新增）
    int64_t* stackTop = nullptr;    // offset 48: 操作数栈顶指针（R146 新增，数组辅助函数用）
    std::unordered_map<std::string, JitClassInfo>* classInfoPtr = nullptr; // offset 56: 运行时类注册表（R148 新增）
    std::vector<std::string>* pendingFieldOrderPtr = nullptr; // offset 64: OP_INIT_FIELD 字段顺序收集（R148 新增）
    void* methodEntryPtr = nullptr; // offset 72: R149 方法调用返回的入口地址（JIT 代码 jmp 用）
    int64_t methodLocalCount = 0;   // offset 80: R149 方法调用返回的 localCount（JIT 代码设置 r13 用）
    std::unordered_map<std::string, JitMethodInfo>* methodEntriesPtr =
        nullptr;                 // offset 88: R149 方法名→元信息映射（"Class.method" 键）
    int64_t* callerBp = nullptr; // offset 96: R149 调用者帧 basePointer（jitMethodCall 设置 JitFrame.callerBp 用）
    uint64_t* chunkCallCounts = nullptr; // offset 104: R150 per-chunk 调用计数数组（热点检测用）
    uint64_t* hotThresholds = nullptr;   // offset 112: R151 per-chunk 热点阈值数组（达到阈值触发重编译回调）
    uint64_t* recompiledFlags =
        nullptr; // offset 120: R151 per-chunk 重编译标志数组（0=未触发，1=已触发，避免重复触发）
    minilang::TypeFeedback* typeFeedback =
        nullptr;                // offset 128: R152 per-chunk 类型反馈计数器数组（特化重编译决策依据）
    void* backendPtr = nullptr; // offset 136: R152 JITBackend* this 指针（jitTriggerRecompile 访问私有成员用）
    int64_t* lastMutatedReceiverPtr = nullptr; // offset 144: R154 lastMutatedReceiver_ 指针（嵌套左值赋值链用）
    std::unordered_map<std::string, JitMethodInfo>* funcEntriesPtr =
        nullptr;                          // offset 152: R155 函数名→JIT入口映射（OP_CALL_EXPR 闭包值调用用）
    int64_t* currentBp = nullptr;         // offset 160: R156 当前帧 basePointer（r13 同步邮箱，jitCreateClosure 用）
    uint64_t* osrLoopCountsPtr = nullptr; // offset 168: R157 per-chunk 循环回边计数数组指针（OSR 用）
    uint64_t* osrLoopThresholdsPtr = nullptr;  // offset 176: R157 per-chunk OSR 阈值数组指针
    uint64_t* osrRecompiledFlagsPtr = nullptr; // offset 184: R157 per-chunk OSR 已触发标志数组指针
    // R158: 真正 OSR 栈帧迁移邮箱字段
    int64_t* osrSavedBp = nullptr; // offset 192: OSR 保存的 r13（JIT 代码写入，OSR 入口点读取恢复）
    int64_t* osrSavedSp = nullptr; // offset 200: OSR 保存的 r15（JIT 代码写入，OSR 入口点读取恢复）
    void* osrEntryPoint = nullptr; // offset 208: OSR 入口点地址（特化重编译返回，JIT 代码 jmp 用）
    // R158: 反优化 deoptimization 邮箱字段
    void* deoptEntryPoint = nullptr; // offset 216: 反优化 Tier 1 入口点（jitDeoptimize 设置，JIT 代码 jmp 用）
    int64_t deoptChunkIdx = 0;       // offset 224: 反优化 chunk 索引（jitDeoptimize 设置）
    // R160: inline cache 字段（OP_MEMBER_GET 属性访问缓存）
    void* memberGetICPtr = nullptr; // offset 232: MemberGetInlineCacheEntry 数组指针（per-call-site）
    // R162: 名称变量映射（OP_DEFINE_VAR/OP_GET_VAR/OP_SET_VAR/OP_DELETE_VAR）
    // 指向 JITBackend::globals_（unordered_map<string, Value>），catch 变量等无 slot 的名称变量走此路径
    void* globalsPtr = nullptr; // offset 240: unordered_map<string, Value>* 名称变量表
    // Safepoint GC 轮询机制（替代原 CallbackSuppressor 完全抑制方案）
    // JIT 代码在 OP_LOOP 回边处轮询此标志，非零时调用 jitSafepointGc 收集 roots 并执行 GC。
    // GcManager 的增量触发回调被重定向为设置此标志（而非直接调用 collectCycle）。
    std::atomic<int>* gcNeededFlag = nullptr; // offset 248: 原子 GC 请求标志指针
};

// ============================================================
// JIT codegen named constants
// ------------------------------------------------------------
// JitContext 字段偏移与 prologue 栈布局的命名常量。
// JITCodeGen.cpp 通过硬编码偏移访问 JitContext 字段，此处提供命名常量
// 替代魔法数字。下方 static_assert 在编译期验证与 offsetof 的一致性，
// 任何 JitContext 字段重排都会立即在此捕获。
// ============================================================
namespace jit_offset {
    constexpr int hasError = 8;
    constexpr int globalSlots = 24;
    constexpr int frames = 32;
    constexpr int frameCount = 40;
    constexpr int stackTop = 48;
    constexpr int methodEntryPtr = 72;
    constexpr int methodLocalCount = 80;
    constexpr int callerBp = 96;
    constexpr int chunkCallCounts = 104; // R150: per-chunk 调用计数数组指针
    constexpr int hotThresholds = 112;   // R151: per-chunk 热点阈值数组指针
    constexpr int recompiledFlags = 120; // R151: per-chunk 重编译标志数组指针
    constexpr int typeFeedback = 128;
    constexpr int lastMutatedReceiverPtr = 144;
    constexpr int currentBp = 160;
    constexpr int osrLoopCountsPtr = 168;      // R157: per-chunk 循环回边计数数组指针
    constexpr int osrLoopThresholdsPtr = 176;  // R157: per-chunk OSR 阈值数组指针
    constexpr int osrRecompiledFlagsPtr = 184; // R157: per-chunk OSR 已触发标志数组指针
    constexpr int osrSavedBp = 192;
    constexpr int osrSavedSp = 200;
    constexpr int osrEntryPoint = 208;     // R158: OSR 入口点地址邮箱
    constexpr int deoptEntryPoint = 216;   // R158: 反优化 Tier 1 入口点邮箱
    constexpr int memberGetICPtr = 232;    // R160: inline cache 数组指针
    constexpr int globalsPtr = 240;        // R162: 名称变量表指针
    constexpr int gcNeededFlag = 248;      // Safepoint GC 请求标志指针
} // namespace jit_offset
static_assert(offsetof(JitContext, hasError) == jit_offset::hasError, "hasError offset");
static_assert(offsetof(JitContext, globalSlots) == jit_offset::globalSlots, "globalSlots offset");
static_assert(offsetof(JitContext, frames) == jit_offset::frames, "frames offset");
static_assert(offsetof(JitContext, frameCount) == jit_offset::frameCount, "frameCount offset");
static_assert(offsetof(JitContext, stackTop) == jit_offset::stackTop, "stackTop offset");
static_assert(offsetof(JitContext, methodEntryPtr) == jit_offset::methodEntryPtr, "methodEntryPtr offset");
static_assert(offsetof(JitContext, methodLocalCount) == jit_offset::methodLocalCount, "methodLocalCount offset");
static_assert(offsetof(JitContext, callerBp) == jit_offset::callerBp, "callerBp offset");
static_assert(offsetof(JitContext, chunkCallCounts) == jit_offset::chunkCallCounts, "chunkCallCounts offset");
static_assert(offsetof(JitContext, hotThresholds) == jit_offset::hotThresholds, "hotThresholds offset");
static_assert(offsetof(JitContext, recompiledFlags) == jit_offset::recompiledFlags, "recompiledFlags offset");
static_assert(offsetof(JitContext, typeFeedback) == jit_offset::typeFeedback, "typeFeedback offset");
static_assert(offsetof(JitContext, lastMutatedReceiverPtr) == jit_offset::lastMutatedReceiverPtr, "lastMutatedReceiverPtr offset");
static_assert(offsetof(JitContext, currentBp) == jit_offset::currentBp, "currentBp offset");
static_assert(offsetof(JitContext, osrLoopCountsPtr) == jit_offset::osrLoopCountsPtr, "osrLoopCountsPtr offset");
static_assert(offsetof(JitContext, osrLoopThresholdsPtr) == jit_offset::osrLoopThresholdsPtr, "osrLoopThresholdsPtr offset");
static_assert(offsetof(JitContext, osrRecompiledFlagsPtr) == jit_offset::osrRecompiledFlagsPtr, "osrRecompiledFlagsPtr offset");
static_assert(offsetof(JitContext, osrSavedBp) == jit_offset::osrSavedBp, "osrSavedBp offset");
static_assert(offsetof(JitContext, osrSavedSp) == jit_offset::osrSavedSp, "osrSavedSp offset");
static_assert(offsetof(JitContext, osrEntryPoint) == jit_offset::osrEntryPoint, "osrEntryPoint offset");
static_assert(offsetof(JitContext, deoptEntryPoint) == jit_offset::deoptEntryPoint, "deoptEntryPoint offset");
static_assert(offsetof(JitContext, memberGetICPtr) == jit_offset::memberGetICPtr, "memberGetICPtr offset");
static_assert(offsetof(JitContext, globalsPtr) == jit_offset::globalsPtr, "globalsPtr offset");
static_assert(offsetof(JitContext, gcNeededFlag) == jit_offset::gcNeededFlag, "gcNeededFlag offset");

// Prologue 栈布局常量
// 48 = 5 个 callee-saved 寄存器 (r12/r13/r14/r15/rbx) × 8B + 8B 对齐填充
// 8192 = 操作数栈 1024 × 8B
constexpr int kJitCalleeSavedArea = 48;
constexpr int kJitOperandStackBytes = 8192;
// Windows x64 ABI shadow space / Linux 对齐填充
constexpr int kJitWinShadowSpace = 32;
constexpr int kJitLinuxCallAlign = 8;

/// R160 → PIC 升级: Per-call-site polymorphic inline cache for OP_MEMBER_GET
/// 4 路多态 inline cache：缓存最多 4 个 (Instance 内部指针, 字段 Value 指针) 对。
/// 当同一 call site 遇到超过 4 种不同 Instance 类型时，标记为 megamorphic，
/// 后续所有查找直接走慢速路径（unordered_map lookup），避免缓存抖动。
///
/// 安全性论证（同原 monomorphic 版本）：
///   - cachedInstancePtr 是 InstanceData 的原始地址（从 NaN-boxed Value 的低 48 位提取）
///   - cachedFieldValuePtr 指向 unordered_map<string,Value> 中某个 node 的 value
///   - C++ 标准：unordered_map 的 node 在 erase 之前地址稳定（rehash 不移动 node）
///   - MiniLang 语义：字段只在 OP_INIT_FIELD 时插入，不会被 erase → 指针始终有效
///   - COW 安全性：若 Instance 被 COW 复制，receiver 的 InstanceData 指针会改变
///     （指向新副本），cache 自动 miss（cachedInstancePtr != 实际指针），不会读到旧数据
///   - 字段值修改：若字段值被 OP_MEMBER_SET 修改，cachedFieldValuePtr 指向的 Value
///     会被原地更新（unordered_map operator[] 修改现有 node 的 value，不 invalidate 指针）
struct MemberGetInlineCacheEntry {
    static constexpr int kPICSlots = 4; ///< PIC 槽位数（4 路多态）

    struct Slot {
        const void* cachedInstancePtr = nullptr;    ///< cache key: InstanceData 原始地址
        const Value* cachedFieldValuePtr = nullptr; ///< cache value: 字段 Value 指针（直接读取）
    };

    Slot slots[kPICSlots];       ///< 4 路 PIC 槽位
    uint8_t slotCount = 0;       ///< 已使用的槽位数（0~kPICSlots）
    bool megamorphic = false;    ///< 超过 kPICSlots 种类型后置 true，永久走慢速路径

    /// 查找 instPtr 对应的缓存字段指针（命中返回 Value*，未命中返回 nullptr）
    const Value* lookup(const void* instPtr) const {
        for (uint8_t i = 0; i < slotCount; ++i) {
            if (slots[i].cachedInstancePtr == instPtr)
                return slots[i].cachedFieldValuePtr;
        }
        return nullptr;
    }

    /// 插入新条目。若已满则标记 megamorphic 并返回 false。
    bool insert(const void* instPtr, const Value* fieldPtr) {
        if (megamorphic) return false;
        // 检查是否已存在（避免重复插入）
        for (uint8_t i = 0; i < slotCount; ++i) {
            if (slots[i].cachedInstancePtr == instPtr) {
                slots[i].cachedFieldValuePtr = fieldPtr; // 更新
                return true;
            }
        }
        if (slotCount >= kPICSlots) {
            megamorphic = true; // 类型过多，退化为 megamorphic
            return false;
        }
        slots[slotCount].cachedInstancePtr = instPtr;
        slots[slotCount].cachedFieldValuePtr = fieldPtr;
        ++slotCount;
        return true;
    }
};

/// R162: JIT try/catch 异常处理器（运行时栈结构）
/// OP_TRY_BEGIN 时 push 到 tryStack_，OP_TRY_END 时 pop，OP_THROW 时搜索。
/// 与 StackVM VM::TryHandler 对齐，但 catchIp 改为 void*（JIT 代码绝对地址）。
struct JitTryHandler {
    void* catchAddr = nullptr;    ///< catch 块的 JIT 代码绝对地址（lea label 解析）
    int64_t* stackBase = nullptr; ///< try 开始时 r15（操作数栈顶），catch 时恢复
    size_t frameIndex = 0;        ///< 所属帧索引（*frameCount 值），用于跨帧异常传播
};

/// 方法调用 Inline Cache：缓存 (className, methodName) → JitMethodInfo* 的解析结果。
/// 避免每次方法调用都沿继承链做 string 拼接 + map lookup。
/// 4 路 PIC：同一 call site 最多缓存 4 个不同类的方法解析结果。
/// 安全性：MiniLang 方法表在运行时不可变（类定义后方法集合固定），缓存永久有效。
struct MethodCallICEntry {
    static constexpr int kSlots = 4;

    struct Slot {
        const char* classNamePtr = nullptr;   ///< cache key: receiver.className() 的内部指针
        const JitMethodInfo* method = nullptr; ///< cache value: 解析到的方法信息
    };

    Slot slots[kSlots];
    uint8_t slotCount = 0;
    bool megamorphic = false;

    const JitMethodInfo* lookup(const char* classNamePtr) const {
        for (uint8_t i = 0; i < slotCount; ++i) {
            if (slots[i].classNamePtr == classNamePtr)
                return slots[i].method;
        }
        return nullptr;
    }

    bool insert(const char* classNamePtr, const JitMethodInfo* method) {
        if (megamorphic) return false;
        for (uint8_t i = 0; i < slotCount; ++i) {
            if (slots[i].classNamePtr == classNamePtr) {
                slots[i].method = method;
                return true;
            }
        }
        if (slotCount >= kSlots) {
            megamorphic = true;
            return false;
        }
        slots[slotCount].classNamePtr = classNamePtr;
        slots[slotCount].method = method;
        ++slotCount;
        return true;
    }
};

/// JIT 编译后的入口函数签名
/// @param ctx JitContext 指针（通过 r12 传递给 JIT 代码）
/// @return int64_t（最后一个表达式的值，PoC 不使用返回值）
using JitEntryFn = int64_t (*)(JitContext* ctx);

// ============================================================
// JITBackend — JIT 后端
// ============================================================
class JITBackend : public IBackend {
public:
    JITBackend();
    ~JITBackend() override;

    /// IBackend: 后端名称
    std::string backendName() const override { return "JIT"; }

    /// IBackend: 设置输出回调（print 语句输出通道）
    void setOutputCallback(std::function<void(const std::string&)> cb) override;

    /// IBackend: 设置输入回调（input 函数输入通道，PoC 未使用）
    void setInputCallback(std::function<std::string(const std::string&)> cb) override;

    /// IBackend: 获取诊断包
    const DiagnosticBag& getDiagnostics() const override { return diagnostics_; }

    /// 执行编译结果（与 StackVM::execute 签名一致）
    /// 阶段 2b：编译 mainChunk + functionChunks 到一个 CodeHolder
    JitResult execute(const CompileResult& result);

    /// 是否发生过错误
    /// P2-12: override IBackend::hasError()，签名一致
    bool hasError() const override { return hasError_ || diagnostics_.hasErrors(); }

    /// 获取最后的错误消息
    /// P2-12: override IBackend::getLastError()，返回值类型对齐（消除协变返回类型错误）
    std::string getLastError() const override {
        // 优先从 diagnostics_ 派生（结构化错误），无诊断时回退到 lastError_（JIT 内部错误）
        const auto& diags = diagnostics_.all();
        for (auto it = diags.rbegin(); it != diags.rend(); ++it) {
            if (it->isError())
                return it->message;
        }
        return lastError_;
    }

    /// 获取最后错误的源码行号（P2-12: 新增 override）
    int getLastErrorLine() const override {
        const auto& diags = diagnostics_.all();
        for (auto it = diags.rbegin(); it != diags.rend(); ++it) {
            if (it->isError())
                return it->line;
        }
        return 0;
    }

    /// R150: 获取 per-chunk 调用计数统计（热点检测用）
    /// @return 按 chunk 索引顺序的 (chunkName, callCount) 列表，mainChunk 索引 0 不计数（恒为 0）
    std::vector<std::pair<std::string, uint64_t>> getHotChunkStats() const;

    /// R151: 获取 per-chunk 重编译触发统计（热点阈值重编译用）
    /// @return 按 chunk 索引顺序的 (chunkName, recompiledFlag) 列表，recompiledFlag=1 表示已达阈值触发过重编译
    std::vector<std::pair<std::string, uint64_t>> getRecompileStats() const;

    /// R151: 设置 per-chunk 热点阈值（测试用，须在 execute 前调用）
    /// @param chunkName chunk 名称（mainChunk 用 "main"，函数 chunk 用函数名，方法 chunk 用 "Class.method"）
    /// @param threshold 阈值（0 表示不触发重编译）
    void setHotThreshold(const std::string& chunkName, uint64_t threshold);

    /// R152: 获取 per-chunk 类型反馈统计（特化重编译决策依据）
    /// @return 按 chunk 索引顺序的 (chunkName, TypeFeedback) 列表
    std::vector<std::pair<std::string, minilang::TypeFeedback>> getTypeFeedback() const;

    /// R152: 获取已特化重编译的 chunk 列表
    /// @return 已特化的 chunk 名称列表（按特化顺序）
    const std::vector<std::string>& getSpecializedChunks() const { return specializedChunks_; }

    /// R157: 设置 per-chunk OSR 阈值（测试用，须在 execute 前调用）
    /// @param chunkName chunk 名称（mainChunk 用 "main"，函数 chunk 用函数名）
    /// @param threshold OSR 阈值（0 表示不触发 OSR）
    void setOsrThreshold(const std::string& chunkName, uint64_t threshold);

    /// R157: 获取 per-chunk OSR 循环回边计数统计
    /// @return 按 chunk 索引顺序的 (chunkName, loopCount) 列表
    std::vector<std::pair<std::string, uint64_t>> getOsrLoopStats() const;

    /// R157: 获取 per-chunk OSR 触发统计
    /// @return 按 chunk 索引顺序的 (chunkName, osrRecompiledFlag) 列表
    std::vector<std::pair<std::string, uint64_t>> getOsrRecompileStats() const;

    /// R157: 获取已 lazy 编译的 chunk 名称列表
    /// @return 已通过 lazy compilation 编译的 chunk 名称列表
    const std::vector<std::string>& getLazyCompiledChunks() const { return lazyCompiledChunksList_; }

    /// R157: 启用/禁用 lazy compilation 模式（须在 execute 前调用）
    /// @param enabled true 启用 lazy compilation（函数/方法 chunk 首次调用时按需编译）
    void setLazyCompilation(bool enabled) { lazyMode_ = enabled; }

    /// R159: 启用/禁用分层编译模式（须在 execute 前调用）
    /// 启用后非 main chunk 初始为 Tier 0 (Interpreter)，首次调用时自动升级到 Tier 1 (Baseline)。
    /// 内部自动启用 lazy compilation（tiered mode implies lazy mode）。
    /// @param enabled true 启用分层编译（Tier 0→Tier 1 自动升级）
    void setTieredCompilation(bool enabled) {
        tieredMode_ = enabled;
        if (enabled) {
            lazyMode_ = true; // 分层编译依赖 lazy compilation 基础设施
        }
    }

    /// R159: 是否已启用分层编译模式
    bool isTieredCompilation() const { return tieredMode_; }

    /// R158: 启用/禁用真正 OSR 栈帧迁移模式（须在 execute 前调用）
    /// @param enabled true 启用真正 OSR（保存/恢复 r13/r15 + jmp OSR 入口点）
    void setOsrMigrationMode(bool enabled) { osrMigrationMode_ = enabled; }

    /// R158: 获取已 OSR 迁移的 chunk 名称列表
    const std::vector<std::string>& getOsrMigratedChunks() const { return osrMigratedChunks_; }

    /// R158: 获取反优化触发计数
    uint64_t getDeoptCount() const { return deoptCount_; }

    /// R158: 获取已反优化的 chunk 名称列表
    const std::vector<std::string>& getDeoptimizedChunks() const { return deoptimizedChunks_; }

    /// R160: 获取 inline cache 统计（OP_MEMBER_GET 属性访问缓存）
    /// @return (hitCount, missCount) — cache 命中/未命中次数
    /// PIC 统计：命中/未命中/megamorphic 回退计数（教学统计用）
    struct ICStats {
        uint64_t hit = 0;
        uint64_t miss = 0;
        uint64_t megamorphic = 0;
    };
    ICStats getInlineCacheStats() const { return {icHitCount_, icMissCount_, icMegamorphicCount_}; }

    /// R160: 重置 inline cache 统计（测试用，在 execute 前调用）
    void resetInlineCacheStats() {
        icHitCount_ = 0;
        icMissCount_ = 0;
        icMegamorphicCount_ = 0;
    }

    /// 拓展二期·教学（字节码↔汇编对照）：启用汇编文本捕获。
    /// 须在 execute 前调用；启用后 compileAllChunks 会在 CodeHolder 上挂
    /// asmjit::StringLogger，捕获全部发射的 x86-64 汇编文本（含标签/注释）。
    /// 仅覆盖主编译路径（lazy/特化重编译的独立 CodeHolder 不捕获，
    /// 教学展示以首次全量编译为准）。
    void setAsmCapture(bool enabled) { asmCaptureEnabled_ = enabled; }

    /// 拓展二期：获取捕获的汇编文本（setAsmCapture(true) + execute 后有效）
    const std::string& getCapturedAsm() const { return capturedAsm_; }

    /// R157: 触发 lazy compilation（jitCallByName 通过 backendPtr 调用）
    /// 在 lazyMode_ 为 true 且函数首次调用时触发，编译单个 chunk 到独立 CodeHolder。
    /// @param chunkName 待 lazy 编译的 chunk 名称
    /// @return 入口函数指针（失败返回 nullptr）
    JitEntryFn triggerLazyCompile(const std::string& chunkName) {
        if (!lazyMode_ || !currentResult_)
            return nullptr;
        return compileSingleChunkLazy(*currentResult_, chunkName);
    }

    /// R157: 触发 OSR 特化重编译（jitTriggerOsrRecompile 通过 backendPtr 调用）
    /// 循环回边计数超阈值时触发，根据类型反馈对当前 chunk 特化重编译。
    /// @param chunkIdx 待特化的 chunk 索引
    /// @return 0=成功，非 0=失败
    int64_t triggerOsrRecompile(int64_t chunkIdx);

    /// R158: 触发真正 OSR 栈帧迁移（jitTriggerOsrMigration 通过 backendPtr 调用）
    /// 循环回边计数超阈值时触发，保存当前栈帧状态 → 特化重编译（含 OSR 入口点）→
    /// 从 OSR 入口点恢复栈帧状态继续执行（真正 OSR，非 R157 的"下次 execute 生效"）。
    /// @param chunkIdx 待特化的 chunk 索引
    /// @return 0=成功（osrEntryPoint 已设置），非 0=失败
    int64_t triggerOsrMigration(int64_t chunkIdx);

    /// R158: 获取 per-chunk 当前 Tier（分层编译状态查询）
    /// @return 按 chunk 索引顺序的 (chunkName, JitTier) 列表
    std::vector<std::pair<std::string, minilang::JitTier>> getChunkTiers() const;

    /// R158: 触发反优化（jitDeoptimize 通过 backendPtr 调用）
    /// 特化版本运行时类型检查失败时触发，保存当前栈帧状态 → 查找 Tier 1 入口 →
    /// 设置 deoptEntryPoint 供 JIT 代码 jmp 回 Tier 1。
    /// @param chunkIdx 反优化的 chunk 索引
    /// @return 0=成功（deoptEntryPoint 已设置），非 0=失败
    int64_t triggerDeoptimize(int64_t chunkIdx);

    /// R162: 名称变量访问器（供 jitDefineVar/jitGetVar/jitSetVar/jitDeleteVar 等
    /// extern "C" helper 访问 private 成员，避免 friend 与 extern "C" 链接规范冲突）
    ///@{
    std::vector<int64_t>& globalSlotsMut() { return globalSlots_; }
    std::unordered_map<std::string, int>& globalNameToSlotMut() { return globalNameToSlot_; }
    std::unordered_map<std::string, Value>& globalsMut() { return globals_; }
    const std::vector<int64_t>& globalSlotsRef() const { return globalSlots_; }
    const std::unordered_map<std::string, int>& globalNameToSlotRef() const { return globalNameToSlot_; }
    const std::unordered_map<std::string, Value>& globalsRef() const { return globals_; }
    ///@}

private:
    /// 编译所有 chunk（mainChunk + functionChunks）到一个 CodeHolder
    /// @param result 编译结果（含 mainChunk 和 functionChunks）
    /// @return 入口函数指针（mainChunk 入口），失败返回 nullptr
    JitEntryFn compileAllChunks(const CompileResult& result);

    /// R152: 特化重编译单个 chunk（INT 特化版本）
    /// 在 execute() 返回后对 recompiledFlags 标记的 chunk 调用，根据类型反馈决定是否特化：
    /// - TypeFeedback.otherCount == 0 且 floatCount == 0 → INT 特化（跳过 emitCheckInt）
    /// - 否则 → 不特化（保持原版本）
    /// @param result 编译结果（用于访问 chunk 数据）
    /// @param chunkIdx 待特化的 chunk 索引
    /// @return 特化版本入口函数指针（失败返回 nullptr）
    JitEntryFn compileChunkSpecialized(const CompileResult& result, size_t chunkIdx);

    /// R153: 特化重编译单个 chunk（FLOAT 特化版本）
    /// 在 execute() 返回后对 recompiledFlags 标记的 chunk 调用，根据类型反馈决定是否特化：
    /// - TypeFeedback.floatCount > 0 且 otherCount == 0 → FLOAT 特化
    ///   （emitCheckInt 生成无条件 jmp 跳过 INT 原生路径，emitRecordTypeFeedback 不生成代码）
    /// - 否则 → 不特化（保持原版本）
    /// @param result 编译结果（用于访问 chunk 数据）
    /// @param chunkIdx 待特化的 chunk 索引
    /// @return 特化版本入口函数指针（失败返回 nullptr）
    JitEntryFn compileChunkSpecializedFloat(const CompileResult& result, size_t chunkIdx);

    /// R157: lazy compilation — 按需编译单个 chunk 到独立 CodeHolder
    /// 在 lazyMode_ 为 true 时，OP_CALL 首次调用某函数/方法时触发。
    /// 从 currentResult_ 提取目标 chunk 编译到新 CodeHolder，生成独立入口。
    /// 编译完成后更新 funcEntries_/methodEntries_ 的 entryPtr，后续调用直接 jmp。
    /// @param result 编译结果（用于访问 chunk 数据）
    /// @param chunkName 待 lazy 编译的 chunk 名称（函数名或 "Class.method"）
    /// @return 入口函数指针（失败返回 nullptr）
    JitEntryFn compileSingleChunkLazy(const CompileResult& result, const std::string& chunkName);

    /// R158: 特化重编译单个 chunk（含 OSR 入口点生成）
    /// 与 R152 compileChunkSpecialized 类似，但在 OP_LOOP 位置生成 OSR 入口 Label，
    /// OSR 入口点从邮箱恢复 r13/r15 后跳到循环回边点继续执行。
    /// 通过 ctx->osrEntryPoint 邮箱返回 OSR 入口点地址。
    /// @param result 编译结果
    /// @param chunkIdx 待特化的 chunk 索引
    /// @param osrEntryPointPtr 输出参数：OSR 入口点地址（失败时为 nullptr）
    /// @return 特化版本入口函数指针（失败返回 nullptr）
    JitEntryFn compileChunkSpecializedWithOsr(const CompileResult& result, size_t chunkIdx, void** osrEntryPointPtr);

    /// R158: 特化重编译单个 chunk FLOAT 版本（含 OSR 入口点生成）
    /// 与 R153 compileChunkSpecializedFloat 类似，但在 OP_LOOP 位置生成 OSR 入口 Label。
    /// @param result 编译结果
    /// @param chunkIdx 待特化的 chunk 索引
    /// @param osrEntryPointPtr 输出参数：OSR 入口点地址（失败时为 nullptr）
    /// @return 特化版本入口函数指针（失败返回 nullptr）
    JitEntryFn compileChunkSpecializedFloatWithOsr(const CompileResult& result, size_t chunkIdx,
                                                   void** osrEntryPointPtr);

    /// 设置运行时错误（JIT 代码执行期间）
    void runtimeError(const std::string& msg);

    /// 设置编译错误（JIT 编译期间）
    void compileError(const std::string& msg);

    // ============================================================
    // JitFuncInfo：compileAllChunks 的 funcTable 条目类型
    // ------------------------------------------------------------
    // 原 compileAllChunks 局部 struct，提取到类定义供 emitCallDispatch helper 使用。
    // 含入口 Label（OP_CALL jmp 目标）+ 元信息（localCount/arity/hasInnerClosures）。
    // ============================================================
    struct JitFuncInfo {
        asmjit::Label entryLabel;
        int localCount = 0;
        int arity = 0;
        int requiredArity = 0;                ///< R149: 必需参数个数（默认参数支持）
        const BytecodeChunk* chunk = nullptr; ///< R155: OP_CLOSURE 构建闭包值时获取 chunkPtr 用
        bool hasInnerClosures = false;        ///< R161 perf: 函数体是否含 OP_CLOSURE（fast path 门控）
    };

    // ============================================================
    // JIT codegen 辅助函数（从 compileAllChunks 的 lambda 提取为成员函数）
    // ------------------------------------------------------------
    // 原 compileAllChunks 内嵌 25+ 个 lambda（~1100 行），新增 OpCode 支持需在
    // 3000+ 行函数中定位，维护性差。现提取为独立成员函数，实现移至 JITCodeGenHelpers.cpp。
    // 约定：所有 emit* 函数接收 asmjit::x86::Assembler& 和必要的 Label 参数
    //（Label 是轻量 ID 包装，按值传递）。错误处理用的 epilogue 标签按值传递。
    // 依赖成员状态的函数（emitCheckInt 用 currentChunkIdx_、emitMemberGet 用 nextCallSiteId_）
    // 通过成员变量共享，无需额外参数。
    // ============================================================
    ///@{
    void emitCallBinaryHelper(asmjit::x86::Assembler& a, asmjit::Label epilogue, void* fnPtr, bool needCtx,
                              bool checkError);
    void emitCallUnaryHelper(asmjit::x86::Assembler& a, void* fnPtr);
    void emitCallOrderedCompare(asmjit::x86::Assembler& a, void* fnPtr, int64_t cmpType);
    void emitBuildArray(asmjit::x86::Assembler& a, asmjit::Label epilogue, uint8_t count);
    void emitIndexGet(asmjit::x86::Assembler& a, asmjit::Label epilogue);
    void emitIndexSetLocal(asmjit::x86::Assembler& a, asmjit::Label epilogue, uint8_t slot);
    void emitBuildDict(asmjit::x86::Assembler& a, asmjit::Label epilogue, uint8_t pairCount);
    void emitBuildTuple(asmjit::x86::Assembler& a, asmjit::Label epilogue, uint8_t count);
    void emitIndexSetGlobal(asmjit::x86::Assembler& a, asmjit::Label epilogue, int slot);
    void emitClassNew(asmjit::x86::Assembler& a, asmjit::Label epilogue, const char* className, uint8_t argCount);
    void emitInitField(asmjit::x86::Assembler& a, asmjit::Label epilogue, const char* fieldName);
    void emitDefineClass(asmjit::x86::Assembler& a, asmjit::Label epilogue, const char* className,
                         const char* superClassName);
    void emitMemberGet(asmjit::x86::Assembler& a, asmjit::Label epilogue, const char* fieldName);
    void emitMemberSetVar(asmjit::x86::Assembler& a, asmjit::Label epilogue, int slot, const char* fieldName);
    void emitMemberSetLocal(asmjit::x86::Assembler& a, asmjit::Label epilogue, uint8_t slot, const char* fieldName);
    void emitMethodCall(asmjit::x86::Assembler& a, asmjit::Label epilogue, const char* methodName, uint8_t argCount,
                        uint8_t receiverLocalSlotByte, int receiverGlobalSlot, bool isSuperCall,
                        const char* superClassName);
    void emitCheckInt(asmjit::x86::Assembler& a, asmjit::x86::Gp val, asmjit::Label failLabel);
    void emitRecordTypeFeedback(asmjit::x86::Assembler& a, asmjit::x86::Gp val, size_t chunkIdx);
    void emitFloatBinaryArith(asmjit::x86::Assembler& a, int op, asmjit::Label failLabel, asmjit::Label endLabel,
                              size_t chunkIdx, asmjit::Label divByZeroLabel = {});
    void emitLen(asmjit::x86::Assembler& a, asmjit::Label epilogue);
    void emitDupN(asmjit::x86::Assembler& a, uint8_t depth);
    void emitLoadMutated(asmjit::x86::Assembler& a);
    void emitIndexSet(asmjit::x86::Assembler& a, asmjit::Label epilogue);
    void emitWritebackVar(asmjit::x86::Assembler& a, int slot);
    void emitWritebackLocal(asmjit::x86::Assembler& a, uint8_t slot);
    /// 扫描 chunk 统计 OP_MEMBER_GET 数量，累加到 nextCallSiteId_
    void countMemberGetInChunk(const BytecodeChunk& chunk);
    /// 扫描 chunk 收集 OP_DEFINE_CLASS 类名到 classNameSet_
    void collectClassNamesFromChunk(const BytecodeChunk& chunk);

    // ============================================================
    // P0 重构：大型 OpCode case 提取为 emit* 子方法
    // ------------------------------------------------------------
    // 原 compileAllChunks 单函数 ~3930 行，圈复杂度 >200。按 OpCode 类别提取
    // 大型 case（>70 行）为独立成员函数，实现移至 JITCodeGenHelpers.cpp。
    // 约定：验证（操作数越界/常量池越界）保留在 case body，helper 仅负责 codegen。
    // 需要编译期上下文（funcTable/jumpLabels/chunkIdx）的 helper 通过参数传入。
    // ============================================================
    ///@{
    /// OP_CALL 代码生成（含 fast path / mailbox path 双路径分派）
    /// @param funcIt funcTable 中命中的条目（调用前已验证非 end）
    /// @param argCount 实参个数
    /// @return true 走快速路径（ip += 4 由调用方处理），false 走 mailbox 路径
    bool emitCallDispatch(asmjit::x86::Assembler& a, asmjit::Label epilogue, const JitFuncInfo& info,
                          uint8_t argCount);
    /// OP_CALL mailbox 路径（lazy mode / 有默认参数 / 有 upvalues / 有内部闭包）
    void emitCallMailbox(asmjit::x86::Assembler& a, asmjit::Label epilogue, const char* funNamePtr, uint8_t argCount);
    /// OP_CALL_EXPR 代码生成（闭包值调用，邮箱模式）
    void emitCallExpr(asmjit::x86::Assembler& a, asmjit::Label epilogue, uint8_t argCount);
    /// OP_RETURN 代码生成（含 close upvalue + 方法调用 writeBack + 帧恢复）
    void emitReturn(asmjit::x86::Assembler& a, asmjit::Label epilogue);
    /// OP_LOOP 代码生成（含 safepoint GC 轮询 + OSR 回边计数 + OSR 入口点生成）
    /// @param jumpTarget 循环回边目标 Label（调用前已验证存在）
    /// @param chunkIdx 当前 chunk 索引（OSR 计数器寻址用）
    void emitLoop(asmjit::x86::Assembler& a, asmjit::Label epilogue, asmjit::Label jumpTarget, size_t chunkIdx);
    /// OP_DIVIDE 代码生成（INT 原生路径 + FLOAT SSE2 路径 + 除零错误 + C++ 辅助回退）
    void emitDivide(asmjit::x86::Assembler& a, asmjit::Label epilogue, size_t chunkIdx);
    /// OP_MODULO 代码生成（INT 原生路径 + 除零错误 + C++ 辅助回退）
    void emitModulo(asmjit::x86::Assembler& a, asmjit::Label epilogue, size_t chunkIdx);
    /// OP_CLOSURE 代码生成（调用 jitCreateClosure 创建闭包值并压栈）
    /// @param funNamePtr 函数名 C 字符串（指向 chunk.name，生命期跨 execute）
    /// @param chunkPtr BytecodeChunk 指针（注册到闭包值）
    /// @param upvalueCount upvalue 个数
    /// @param upvalueDescs upvalue 描述符指针（指向字节码 ip+4）
    void emitClosure(asmjit::x86::Assembler& a, asmjit::Label epilogue, const char* funNamePtr,
                     const void* chunkPtr, uint8_t upvalueCount, const uint8_t* upvalueDescs);
    ///@}

    /// codegen 共享状态（原 compileAllChunks 局部变量，提取为成员供 helper 函数访问）
    ///@{
    uint64_t nextCallSiteId_ = 0; ///< OP_MEMBER_GET callSiteId 分配计数器（编译期，先统计后重置再分配）
    size_t currentChunkIdx_ = 0;  ///< 当前编译的 chunk 索引（emitCheckInt/emitRecordTypeFeedback 反优化用）
    ///@}

    asmjit::JitRuntime runtime_;        ///< JIT 内存分配器
    JitEntryFn currentEntry_ = nullptr; ///< R151: 当前已加载的 JIT 入口（execute 重复调用时释放旧代码）
    DiagnosticBag diagnostics_;         ///< 诊断包
    std::function<void(const std::string&)> outputCallback_;       ///< print 输出回调
    std::function<std::string(const std::string&)> inputCallback_; ///< input 输入回调

    std::vector<JitFrame> frameStack_; ///< R141: 帧栈存储（预分配，JitContext.frames 指向此）
    size_t frameCount_ = 0;            ///< R141: 当前帧深度（JitContext.frameCount 指向此）

    /// R148: 运行时类注册表（与 StackVM classInfo_ 等价，简化版无 methodCache）
    /// OP_DEFINE_CLASS 执行时填充，OP_CLASS_NEW/OP_MEMBER_GET/SET 查询
    std::unordered_map<std::string, JitClassInfo> classInfo_;

    /// R148: OP_INIT_FIELD 执行期间收集字段声明顺序，供 OP_DEFINE_CLASS 提取
    std::vector<std::string> pendingFieldOrder_;

    /// R149: 方法名→元信息映射（"Class.method" 键），compileAllChunks 编译完成后填充
    /// jitMethodCall 沿继承链查找时使用，与 StackVM methodsByClass_ + findMethodChunk 对齐
    std::unordered_map<std::string, JitMethodInfo> methodEntries_;

    /// R149: 编译期收集的所有类名集合（扫描 OP_DEFINE_CLASS 指令提取）
    /// OP_CALL handler 中 funcTable 找不到时检查此集合，命中则走 emitClassNew 路径
    /// （与 StackVM executeCallByName 中 classInfo_.find(funName) fallback 对齐）
    std::unordered_set<std::string> classNameSet_;

    /// R150: per-chunk 调用计数数组（热点检测用）
    /// 索引 0 = mainChunk（不计数，恒为 0），索引 1..N = functionChunks
    /// compileAllChunks 中按 allChunks 顺序分配索引并构建 chunkNames_
    std::vector<uint64_t> chunkCallCounts_;
    std::vector<std::string> chunkNames_; ///< R150: chunk 索引→名称映射（getHotChunkStats 用）

    /// R151: per-chunk 热点阈值数组（达到阈值触发重编译回调）
    /// 默认阈值 kDefaultHotThreshold（如 1000），mainChunk 索引 0 设为 0（不触发）
    std::vector<uint64_t> hotThresholds_;
    /// R151: per-chunk 重编译标志数组（0=未触发，1=已触发，避免重复触发）
    std::vector<uint64_t> recompiledFlags_;
    /// R151: 测试用自定义阈值覆盖（name→threshold），compileAllChunks 初始化时应用
    std::unordered_map<std::string, uint64_t> customThresholds_;

    /// R152: per-chunk 类型反馈计数器数组（特化重编译决策依据）
    /// 索引与 chunkCallCounts_ 对齐，在算术指令 genericXXX 路径递增
    std::vector<minilang::TypeFeedback> typeFeedback_;

    /// R152: 已特化重编译的 chunk 名称列表（按特化顺序，测试验证用）
    std::vector<std::string> specializedChunks_;

    /// R152: 特化版本入口指针（按 chunkIdx 索引，nullptr 表示未特化）
    /// execute() 返回后对 recompiledFlags 标记的 chunk 进行特化重编译，
    /// 生成的入口存入此数组；下次 execute() 时通过 methodEntries_ 切换入口
    std::vector<JitEntryFn> specializedEntries_;

    /// R152: 特化版本 CodeHolder 持有的 JitEntryFn 内存所有权列表
    /// 析构时通过 runtime_.release 释放，避免内存泄漏
    std::vector<JitEntryFn> ownedSpecializedEntries_;

    /// AUDIT-R4 BUG-12 fix: chunkName → 当前持有的特化代码块入口。
    /// 同一 chunk 被重复特化时，旧块从 ownedSpecializedEntries_ 移入
    /// retiredSpecializedEntries_，在下次 execute() 入口（无 JIT 代码运行的
    /// 安全点）统一释放——原实现旧块滞留至析构，长会话反复去优化/再特化
    /// 导致可执行内存单调增长。不在替换点立即释放：OSR 路径的特化发生
    /// 在 JIT 执行期间，旧代码可能仍在本机调用栈上。
    std::unordered_map<std::string, JitEntryFn> ownedSpecializedByChunk_;
    std::vector<JitEntryFn> retiredSpecializedEntries_;

    /// AUDIT-R4 BUG-12 fix: 登记新特化块所有权；若同名 chunk 已有旧块，
    /// 将旧块退休（移入 retiredSpecializedEntries_ 待安全点释放）。
    void registerSpecializedOwnership(const std::string& chunkName, JitEntryFn specializedEntry);

    /// AUDIT-R4 BUG-12 fix: 在安全点（execute() 入口/析构）释放退休特化块。
    void releaseRetiredSpecializedEntries();

    /// R152: 当前 CompileResult 引用（特化重编译时访问 chunk 数据用）
    /// execute() 入口赋值，compileChunkSpecialized 读取
    const CompileResult* currentResult_ = nullptr;

    /// R152: INT 特化编译模式标志（compileChunkSpecialized 设置）
    /// 为 true 时，emitCheckInt 不生成类型检查代码，算术指令直接走 INT 原生路径。
    /// compileAllChunks 在此模式下编译所有 chunk 为特化版本，compileChunkSpecialized
    /// 从中提取目标 chunk 入口并丢弃其他 chunk 的特化版本。
    bool specializeIntMode_ = false;

    /// R153: FLOAT 特化编译模式标志（compileChunkSpecializedFloat 设置）
    /// 为 true 时：
    /// - emitCheckInt 生成无条件 jmp 到 failLabel（跳过 INT 原生路径）
    /// - emitRecordTypeFeedback 不生成代码（类型反馈已收集完毕）
    /// 性能提升来自跳过 INT 类型检查 + 跳过类型反馈收集 + 跳过 INT 原生路径溢出检查。
    /// 类型反馈 floatCount>0 && otherCount==0 保证操作数均为 FLOAT，走 genericXXX 路径
    /// 调用 C++ 辅助函数（jitAddGeneric 等内部 isFloat 分支）。
    bool specializeFloatMode_ = false;

    /// R153: 特化方法入口持久化映射（chunkName → 特化方法入口地址）
    /// compileAllChunks 每次会 clear methodEntries_，导致特化入口丢失。
    /// 此映射在 compileChunkSpecialized/compileChunkSpecializedFloat 中填充，
    /// execute() 中 compileAllChunks 后重新应用到 methodEntries_，确保第二次及后续
    /// execute() 仍使用特化版本入口（R153 修复 R152 的入口丢失问题）。
    std::unordered_map<std::string, void*> specializedMethodEntries_;

    /// R154: lastMutatedReceiver_ 存储嵌套左值赋值链中变异后的容器（NaN-boxing Value）
    /// OP_INDEX_SET pop obj+idx+val → 修改 obj[idx] → 存入此；OP_LOAD_MUTATED push 此到栈顶；
    /// OP_WRITEBACK_*_LOCAL/VAR 整体替换栈槽/全局变量为此值。
    /// 与 StackVM VM::lastMutatedReceiver_ 对齐，JitContext.lastMutatedReceiverPtr 指向此。
    /// 初始化为 NaN-boxing NULL 完整 64 位表示（0x7FFA000000000000，与 JIT.cpp JIT_NULL_BITS 一致）
    int64_t lastMutatedReceiver_ = static_cast<int64_t>(0x7FFA000000000000ULL);

    /// R155: 函数名→JIT入口映射（OP_CALL_EXPR 闭包值调用用）
    /// 键是普通函数名（不含 '.'），值含 entryPtr/localCount/arity/requiredArity
    /// 在 compileAllChunks 末尾从 funcTable 构建，jitCallExpr 运行时查询
    std::unordered_map<std::string, JitMethodInfo> funcEntries_;

    /// R156: open upvalues multimap（与 StackVM openUpvalues_ 对齐）
    /// 键是 JIT 栈槽地址（int64_t*），值为 weak_ptr<VMUpvalue>。
    /// 升序排列（std::less<int64_t*>，地址从小到大）。
    /// JIT 栈向下增长：高地址 = 旧 slot（slot 编号小），低地址 = 新 slot（slot 编号大）。
    /// OP_CLOSURE(isLocal=true) 时 emplace(slotAddr, uv)；OP_RETURN/OP_CLOSE_UPVALUE 时 close。
    /// closeUpvaluesFrom(fromAddr) 用 upper_bound(fromAddr) 关闭所有 addr <= fromAddr 的 upvalue
    /// （等价于 StackVM 的 [fromSlot, ∞)，因为 JIT slot 编号越大地址越小）。
public:
    std::multimap<int64_t*, std::weak_ptr<VMUpvalue>> openUpvalues_;

    /// R156: 每帧 upvalues 向量（与 StackVM VMCallFrame.upvalues 对齐）
    /// 与 frameStack_ 并行索引：frameUpvaluesStack_[idx] 对应 frameStack_[idx] 的 upvalues。
    /// OP_CALL: push 闭包值的 upvalues（从 functionClosures_ 查找，与 StackVM executeCallFunction 对齐）
    /// OP_CALL_EXPR: push 闭包值的 upvalues（jitCallExpr 内部填充）
    /// OP_METHOD_CALL: push 空 vector（方法不捕获 upvalue）
    /// OP_RETURN: pop
    /// OP_CLOSURE(isLocal=false): 从 frameUpvaluesStack_[当前帧] 读取透传 upvalue
    std::vector<std::vector<std::shared_ptr<VMUpvalue>>> frameUpvaluesStack_;

    /// R156: 闭包注册表（与 StackVM VM::functionClosures_ 对齐）
    /// OP_CLOSURE 时注册（jitCreateClosure 内部），OP_CALL 时查找提取 upvalues。
    /// 键是函数名，值是闭包值（含 VMClosureData.upvalues）。
    /// StackVM executeCallFunction（VMCalls.cpp executeCallFunction）通过此表传递 upvalues。
    std::unordered_map<std::string, Value> functionClosures_;

    /// R157: lazy compilation 模式标志（setLazyCompilation 设置）
    /// 为 true 时，compileAllChunks 只编译 mainChunk，函数/方法 chunk 在首次 OP_CALL 时
    /// 通过 jitCallByName 触发 compileSingleChunkLazy 按需编译。
    bool lazyMode_ = false;

    /// R159: 分层编译模式标志（setTieredCompilation 设置）
    /// 为 true 时，非 main chunk 初始为 Tier 0 (Interpreter)，首次调用时通过 lazy
    /// compilation 自动升级到 Tier 1 (Baseline)。chunkTiers_ 追踪 Tier 0→1→2 转换。
    bool tieredMode_ = false;

    /// R157: 已 lazy 编译的 chunk 名称列表（按编译顺序，测试验证用）
    std::vector<std::string> lazyCompiledChunksList_;

    /// R157: lazy 编译的 CodeHolder 内存所有权列表
    /// 析构时通过 runtime_.release 释放，避免内存泄漏
    std::vector<JitEntryFn> ownedLazyEntries_;

    /// R157: per-chunk 循环回边计数数组（OSR 用）
    /// 索引与 chunkCallCounts_ 对齐，OP_LOOP 回边时递增
    std::vector<uint64_t> osrLoopCounts_;

    /// R157: per-chunk OSR 阈值数组（达到阈值触发特化重编译）
    /// 默认阈值 0（不触发），测试用 setOsrThreshold 设置
    std::vector<uint64_t> osrLoopThresholds_;

    /// R157: per-chunk OSR 已触发标志数组（0=未触发，1=已触发，避免重复触发）
    std::vector<uint64_t> osrRecompiledFlags_;

    /// R157: 测试用自定义 OSR 阈值覆盖（name→threshold）
    std::unordered_map<std::string, uint64_t> customOsrThresholds_;

    /// R158: per-chunk 当前 Tier（分层编译状态跟踪）
    /// 索引与 chunkCallCounts_ 对齐，记录每个 chunk 的当前编译层级。
    /// 初始为 Tier 1 (Baseline)，热点触发特化重编译后升级为 Tier 2 (Specialized)。
    /// 反优化时回退为 Tier 1。
    std::vector<minilang::JitTier> chunkTiers_;

    /// R158: Tier 1 baseline 入口备份（反优化用）
    /// 键是 chunk 名称（函数名或 "Class.method"），值是 baseline 版本入口地址。
    /// 特化重编译前备份 baseline 入口，反优化时恢复。
    std::unordered_map<std::string, void*> baselineMethodEntries_;

    /// R158: Tier 1 baseline 函数入口备份（反优化用，普通函数）
    std::unordered_map<std::string, void*> baselineFuncEntries_;

    /// R158: 真正 OSR 栈帧迁移模式标志
    /// 为 true 时，OP_LOOP OSR 触发执行真正栈帧迁移（保存/恢复 r13/r15 + jmp OSR 入口点）。
    /// 为 false 时，保持 R157 简化版行为（仅触发特化重编译供下次 execute 生效）。
    bool osrMigrationMode_ = false;

    /// R158: OSR 栈帧快照（JIT 代码通过邮箱写入，triggerOsrMigration 读取）
    minilang::OsrFrameSnapshot osrFrameSnapshot_;

    /// R158: 已 OSR 迁移的 chunk 名称列表（测试验证用）
    std::vector<std::string> osrMigratedChunks_;

    /// R158: 反优化触发计数（测试验证用）
    uint64_t deoptCount_ = 0;

    /// R158: 已反优化的 chunk 名称列表（测试验证用）
    std::vector<std::string> deoptimizedChunks_;

    /// R158: OSR 入口点生成模式标志（compileChunkSpecializedWithOsr 设置）
    /// 为 true 时，compileAllChunks 在 OP_LOOP 位置生成 OSR 入口 Label，
    /// 该 Label 恢复 r13/r15 后跳转到循环回边目标（特化版本中的循环起点）。
    bool osrEntryGenMode_ = false;

    /// R158: OSR 入口点生成的目标 chunk 索引
    size_t osrTargetChunkIdx_ = 0;

    /// R158: OSR 入口 Label（compileAllChunks 中创建，OP_LOOP 后绑定）
    /// JIT 代码 jmp 此 Label 地址实现真正 OSR 栈帧迁移。
    asmjit::Label osrEntryLabel_;

    /// R158: OSR 入口 Label 是否已生成（compileAllChunks 中设置）
    bool osrEntryLabelGenerated_ = false;

    /// R158: OSR 入口点地址输出参数（compileChunkSpecializedWithOsr 设置）
    /// compileAllChunks 编译完成后将 OSR 入口点地址写入此指针。
    void** osrEntryPointOut_ = nullptr;

    /// R160: inline cache 数据（per-call-site，跨所有 chunk 统一编号）
    /// 编译期为每个 OP_MEMBER_GET 分配唯一 callSiteId，
    /// 运行时 jitMemberGetWithIC 通过 callSiteId 索引此数组。
    std::vector<MemberGetInlineCacheEntry> memberGetIC_;

    /// R160: inline cache 命中/未命中/megamorphic 计数（教学统计用）
    uint64_t icHitCount_ = 0;
    uint64_t icMissCount_ = 0;
    uint64_t icMegamorphicCount_ = 0;

    /// 方法调用 IC 数据（per-call-site，跨所有 chunk 统一编号）
    /// 编译期为每个 OP_METHOD_CALL 分配唯一 methodCallSiteId，
    /// 运行时 jitMethodCall 通过 callSiteId 索引此数组。
    std::vector<MethodCallICEntry> methodCallIC_;
    uint64_t nextMethodCallSiteId_ = 0;

    /// 方法调用 IC 统计
    uint64_t methodIcHitCount_ = 0;
    uint64_t methodIcMissCount_ = 0;

    /// R162: try/catch 异常处理器栈（与 StackVM tryStack_ 对齐）
    /// OP_TRY_BEGIN push，OP_TRY_END pop，OP_THROW 搜索。
    /// jitThrow 通过 backendPtr 访问此栈，支持跨帧异常传播。
    std::vector<JitTryHandler> tryStack_;

    /// R162: finally 续跳地址栈（与 StackVM pendingJumpStack_ 对齐）
    /// OP_PUSH_JUMP_TARGET push（JIT 代码地址），OP_FINALLY_END pop 并跳转。
    /// 异常传播时清空（异常中断 break/continue 续跳链）。
    /// AUDIT-R7 F1 fix: 条目附带 frameIndex，jitPopJumpTarget 只消费本帧条目并
    /// 惰性丢弃已返回深帧残留（与三 VM 同构，防跨帧误跳——JIT 条目是机器地址，
    /// 跨帧误跳后果更严重）。
    struct JitPendingJump {
        void* addr;
        size_t frameIndex;
    };
    std::vector<JitPendingJump> pendingJumpStack_;

    /// R162: 名称变量表（与 StackVM globals_ 对齐）
    /// OP_DEFINE_VAR/OP_GET_VAR/OP_SET_VAR/OP_DELETE_VAR 操作此表
    /// （catch 变量、顶层 if 块内 var 等无 slot 分配的名称变量）
    /// extern "C" helper 通过 backendPtr 直接访问，故声明在 public 区
    std::unordered_map<std::string, Value> globals_;

    /// R162: extern "C" helper（jitDefineVar 等）通过 backendPtr 访问，故声明在 public 区
    std::vector<int64_t> globalSlots_; ///< R139: 全局变量 slot 存储（与 StackVM globalSlots_ 等价）
    std::unordered_map<std::string, int> globalNameToSlot_; ///< R147: 名称→slot 映射

    /// R156: 关闭所有指向 address <= fromAddr 的 open upvalues
    /// 与 StackVM closeUpvaluesFrom(fromSlot) 语义对齐但方向反转：
    /// StackVM 栈向上增长，关闭 stackSlot >= fromSlot（升序 multimap 的 lower_bound 到 end）；
    /// JIT 栈向下增长，关闭 address <= fromAddr（升序 multimap 的 begin 到 upper_bound）。
    /// @param fromAddr 帧基址 r13（高地址端），关闭当前帧所有 slot 的 upvalues（addr <= r13）
    void closeUpvaluesFrom(int64_t* fromAddr);

    JitContext jitContext_; ///< 运行时上下文（传给 JIT 代码）
    std::string lastError_; ///< 最后的错误消息
    bool hasError_ = false; ///< 错误标志
    std::atomic<int> gcNeededFlag_{0}; ///< Safepoint GC 请求标志（OP_LOOP 回边轮询）
    // 拓展二期·教学：汇编文本捕获（setAsmCapture 启用，compileAllChunks 填充）
    bool asmCaptureEnabled_ = false; ///< 是否在 CodeHolder 上挂 StringLogger
    std::string capturedAsm_;        ///< 捕获的 x86-64 汇编文本
};

#endif // MINILANG_USE_JIT
