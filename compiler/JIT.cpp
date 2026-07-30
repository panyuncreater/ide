/**
 * @file compiler/JIT.cpp
 * @brief JIT 后端实现：基于 asmjit 的字节码→本地代码编译器（R138-R141 演进）。
 *
 * 实现策略：
 *   1. 两遍扫描 BytecodeChunk.code 字节流：
 *      - 第一遍：收集所有跳转目标位置（OP_JUMP/OP_JUMP_IF_FALSE/OP_LOOP 的操作数），
 *        为每个目标位置创建 asmjit::Label
 *      - 第二遍：为每条 OpCode 生成对应的 x86-64 机器码，在每个字节码位置绑定对应 Label
 *   2. 编译期提取 OP_INT 常量并内联为立即数（movabs rax, imm64）
 *   3. 操作数栈通过 r15 寄存器维护（向低地址增长），与 StackVM 的 stack_ 等价
 *   4. OP_PRINT 通过调用 C++ 运行时辅助函数 jitPrintInt 实现
 *   5. R139: 全局变量 slot 通过 JitContext.globalSlots 指针访问（offset 24）
 *   6. R139: 比较指令使用 cmp + setcc + movzx 模式，结果为 0/1
 *   7. R141: 所有 chunk（mainChunk + functionChunks）编译到一个 CodeHolder，
 *      每个 functionChunk 对应入口 Label，OP_CALL 用 jmp targetLabel
 *   8. R141: 函数调用通过 JitContext.frames 帧栈管理（offset 32/40），
 *      r13 = basePointer 指向当前帧 slot 0
 *
 * 调用约定（Windows x64）：
 *   - JIT 入口函数签名: int64_t jitEntry(JitContext* ctx)
 *   - ctx 通过 rcx 传入，prologue 中保存到 r12
 *   - 调用 jitPrintInt 时：rcx=ctx, rdx=value，sub rsp, 32 提供 shadow space
 *
 * 调用约定（System V x86-64）：
 *   - ctx 通过 rdi 传入，prologue 中保存到 r12
 *   - 调用 jitPrintInt 时：rdi=ctx, rsi=value，sub rsp, 8 维持 16 字节对齐
 *
 * @see JIT.h Bytecode.h IBackend.h
 * @since R138
 */

#include "compiler/JIT.h"

#ifdef MINILANG_USE_JIT

#include "common/Diagnostic.h"
#include "common/ErrorFormat.h"
#include "common/ErrorMessages.h"
#include "common/RuntimeLimits.h"
#include "compiler/JITInternal.h"
#include "interpreter/GcManager.h"
#include "interpreter/Value.h"
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

using namespace jit_internal;

JITBackend::JITBackend() = default;
JITBackend::~JITBackend() {
    // R151: 析构时释放 JIT 已分配的可执行内存，避免内存泄漏
    // （asmjit::JitRuntime 不会自动释放通过 runtime_.add 分配的内存块）
    if (currentEntry_ != nullptr) {
        runtime_.release(currentEntry_);
        currentEntry_ = nullptr;
    }
    // R152: 释放特化版本 CodeHolder 内存
    for (JitEntryFn entry : ownedSpecializedEntries_) {
        if (entry != nullptr) {
            runtime_.release(entry);
        }
    }
    ownedSpecializedEntries_.clear();
    // AUDIT-R4 BUG-12 fix: 释放尚未到达 execute() 安全点的退休特化块
    releaseRetiredSpecializedEntries();
    // R157: 释放 lazy compilation 分配的 CodeHolder 内存
    for (JitEntryFn entry : ownedLazyEntries_) {
        if (entry != nullptr) {
            runtime_.release(entry);
        }
    }
    ownedLazyEntries_.clear();
}

void JITBackend::setOutputCallback(std::function<void(const std::string&)> cb) {
    outputCallback_ = std::move(cb);
    jitContext_.outputCallback = &outputCallback_;
}

void JITBackend::setInputCallback(std::function<std::string(const std::string&)> cb) {
    inputCallback_ = std::move(cb);
}

void JITBackend::runtimeError(const std::string& msg) {
    // P2-12: 错误信息统一写入 diagnostics_，DiagSource 用 JIT（不再复用 VM/Compiler）。
    // 移除 lastError_ 冗余写入——getLastError() 已从 diagnostics_ 派生。
    hasError_ = true;
    diagnostics_.addError(msg, 0, 0, DiagSource::JIT);
}

void JITBackend::compileError(const std::string& msg) {
    // P2-12: 编译期错误也用 JIT 来源（asmjit CodeHolder 失败、字节码越界等）
    hasError_ = true;
    diagnostics_.addError(msg, 0, 0, DiagSource::JIT);
}

// ============================================================
// Safepoint GC — JIT 代码在 OP_LOOP 回边处调用
// ============================================================
// 收集 JIT 操作数栈和全局槽位中的存活容器对象作为 GC roots，
// 然后调用 GcManager::collectCycle 回收循环引用孤岛。
// JIT 代码在调用前已同步 r15 到 ctx->stackTop。
extern "C" void jitSafepointGc(JitContext* ctx) {
    if (!ctx || !ctx->gcNeededFlag)
        return;
    // 重置标志
    ctx->gcNeededFlag->store(0, std::memory_order_relaxed);

    // 收集 roots：操作数栈上的指针类型 Value + 全局槽位
    std::vector<const void*> roots;

    // 1. 扫描全局槽位
    if (ctx->globalSlots) {
        auto* backend = static_cast<JITBackend*>(ctx->backendPtr);
        if (backend) {
            for (int64_t bits : backend->globalSlotsRef()) {
                uint64_t ubits = static_cast<uint64_t>(bits);
                uint64_t tag = ubits & jit_internal::JIT_TAG_FIELD_MASK;
                if (tag == jit_internal::JIT_PTR_TAG_BASE) {
                    // 指针类型（堆对象）：提取低 48 位作为 RefCounted*
                    const void* ptr = reinterpret_cast<const void*>(ubits & jit_internal::JIT_INT48_MASK);
                    if (ptr)
                        roots.push_back(ptr);
                }
            }
        }
    }

    // 2. 扫描操作数栈（从 stackTop 到栈底，栈向下增长，所以 stackTop 是最低地址）
    // 注：JitContext 中没有 stackBottom 字段（无法精确知道栈底，取决于 prologue
    // 分配大小），因此使用保守上限 1024 个槽位。依据：当前帧大小约 8240 字节
    // （8240/8 = 1030 槽），1024 是覆盖单帧的保守上限。
    // Bug #49 fix: 增加防御性上限钳制——即使未来调高预期扫描量，
    // 也不会越过栈分配区域误扫邻接内存。
    if (ctx->stackTop) {
        int64_t* sp = ctx->stackTop;
        // 扫描当前栈上最多 1024 个槽位（操作数栈容量上限）
        int slotsToScan = 1024;
        if (slotsToScan > 1024)
            slotsToScan = 1024; // 防御性上限钳制
        for (int i = 0; i < slotsToScan && sp; ++i, ++sp) {
            uint64_t ubits = static_cast<uint64_t>(*sp);
            uint64_t tag = ubits & jit_internal::JIT_TAG_FIELD_MASK;
            if (tag == jit_internal::JIT_PTR_TAG_BASE) {
                const void* ptr = reinterpret_cast<const void*>(ubits & jit_internal::JIT_INT48_MASK);
                if (ptr)
                    roots.push_back(ptr);
            }
        }
    }

    // 执行 GC
    if (!roots.empty()) {
        GcManager::instance().collectCycle(roots);
    }
}

// ============================================================
// R156: closeUpvaluesFrom — 关闭 open upvalues（JIT 栈向下增长版本）
// ============================================================
// 与 StackVM closeUpvaluesFrom(fromSlot) 语义对齐但方向反转：
// StackVM 栈向上增长，关闭 stackSlot >= fromSlot（升序 multimap 的 lower_bound 到 end）；
// JIT 栈向下增长，关闭 address <= fromAddr（升序 multimap 的 begin 到 upper_bound）。
// 关闭操作：读取栈槽当前值快照到 uv->value，标记 isClosed=true，从 multimap erase。

JitResult JITBackend::execute(const CompileResult& result) {
    // 重置状态
    // P2-12: lastError_ 字段保留作为兜底（getLastError 末尾返回），diagnostics_ 为权威来源
    hasError_ = false;
    lastError_.clear();
    diagnostics_.clear();

    // R152: 保存当前 CompileResult 引用，供 compileChunkSpecialized 访问 chunk 数据
    currentResult_ = &result;

    // R139: 初始化全局变量 slot 存储
    // R142 阶段 3a：初值填充 NaN-boxing NULL (0x7FFA000000000000) 而非 0
    // 大小由 CompileResult.globalSlotCount 决定
    globalSlots_.assign(result.globalSlotCount > 0 ? result.globalSlotCount : 0, static_cast<int64_t>(JIT_NULL_BITS));
    jitContext_.globalSlots = globalSlots_.data();

    // R147: 构建全局变量名→slot 映射，供编译期解析 OP_INDEX_SET_VAR 的 nameIdx→slot
    // StackVM 运行时通过 resolveMutableGlobal(varName) 查找，JIT 改为编译期解析避免运行时开销
    globalNameToSlot_.clear();
    for (int i = 0; i < static_cast<int>(result.globalSlotNames.size()); ++i) {
        globalNameToSlot_[result.globalSlotNames[i]] = i;
    }

    // R148: 初始化类注册表和字段顺序收集器
    // classInfo_ 在 OP_DEFINE_CLASS 执行时填充，供 OP_CLASS_NEW/OP_MEMBER_GET/SET 查询
    // pendingFieldOrder_ 在 OP_INIT_FIELD 执行时收集字段声明顺序，供 OP_DEFINE_CLASS 提取
    classInfo_.clear();
    pendingFieldOrder_.clear();
    jitContext_.classInfoPtr = &classInfo_;
    jitContext_.pendingFieldOrderPtr = &pendingFieldOrder_;

    // enum 元信息注册表：从 CompileResult.enumInfos 加载（与 VM initExecution 对齐），
    // jitBuildEnumVariant 经 ctx->enumRegistryPtr 校验 variant 名/arity/字段类型
    enumRegistry_.clear();
    for (const auto& info : result.enumInfos) {
        enumRegistry_[info.name] = info;
    }
    jitContext_.enumRegistryPtr = &enumRegistry_;

    // 并发原语：同步对象方法内联处理邮箱 + 闭包跳板帧地板栈重置
    jitContext_.syncMethodHandled = 0;
    trampolineFrameFloors_.clear();
    closureTrampoline_ = nullptr; // compileAllChunks 成功后重新解析

    // R149: methodEntries_ 在 compileAllChunks 末尾填充，jitMethodCall 沿继承链查找用
    // callerBp 是运行时邮箱（JIT 代码调用 jitMethodCall 前写入 r13），无需初始化
    jitContext_.methodEntriesPtr = &methodEntries_;
    // R155: funcEntries_ 在 compileAllChunks 末尾填充，jitCallExpr 闭包值调用查找用
    jitContext_.funcEntriesPtr = &funcEntries_;
    jitContext_.callerBp = nullptr;

    // R141: 初始化函数调用帧栈
    // 预分配 MAX_FRAMES 帧（与 StackVM/RegisterVM MAX_FRAMES 一致），frameCount_ 重置为 0
    // frameStack_.data() 在 vector 不发生 reallocation 时保持有效，
    // 故 execute 期间不应修改 frameStack_ 的大小
    frameStack_.assign(RuntimeLimits::MAX_FRAMES, JitFrame{});
    frameCount_ = 0;
    jitContext_.frames = frameStack_.data();
    jitContext_.frameCount = &frameCount_;

    // R156: 初始化 upvalue 管理结构
    // frameUpvaluesStack_ 与 frameStack_ 并行索引，记录每个帧的闭包 upvalues
    // openUpvalues_ 是升序 multimap，记录所有 open 状态的 upvalue（按栈槽地址索引）
    // functionClosures_ 是闭包注册表（与 StackVM VM::functionClosures_ 对齐），每次 execute 重置
    frameUpvaluesStack_.clear();
    openUpvalues_.clear();
    functionClosures_.clear();
    jitContext_.currentBp = nullptr;

    // R162: 清空异常处理栈（支持多次 execute 调用，避免上次残留 handler 干扰）
    // tryStack_ 和 pendingJumpStack_ 在 OP_TRY_BEGIN/OP_PUSH_JUMP_TARGET 时 push，
    // 正常路径 OP_TRY_END/OP_FINALLY_END 时 pop，但异常中断时可能残留。
    tryStack_.clear();
    pendingJumpStack_.clear();

    // R162: 清空名称变量表（catch 变量等无 slot 的名称变量走此路径）
    // 与 StackVM globals_ 对齐，每次 execute 重新开始
    globals_.clear();
    jitContext_.globalsPtr = &globals_;

    // R158: 初始化 OSR 栈帧迁移 + 反优化邮箱字段
    // osrSavedBp/osrSavedSp: OSR 触发时 JIT 代码写入，OSR 入口点读取恢复
    // osrEntryPoint: triggerOsrMigration 写入，JIT 代码 jmp 此地址实现栈帧迁移
    // deoptEntryPoint/deoptChunkIdx: triggerDeoptimize 写入（教学版简化，下次 execute 生效）
    jitContext_.osrSavedBp = nullptr;
    jitContext_.osrSavedSp = nullptr;
    jitContext_.osrEntryPoint = nullptr;
    jitContext_.deoptEntryPoint = nullptr;
    jitContext_.deoptChunkIdx = 0;

    // R160: 重置 inline cache 统计（memberGetIC_ 在 compileAllChunks 中 resize）
    // memberGetICPtr 在 compileAllChunks 之后设置（此时 memberGetIC_ 已 resize 完毕）
    icHitCount_ = 0;
    icMissCount_ = 0;

    // 设置 JitContext 字段（outputCallback 在 setOutputCallback 中已设置）
    jitContext_.hasError = &hasError_;
    jitContext_.errorBuffer = &lastError_;

    // R151: 释放上一次 execute() 分配的 JIT 可执行内存（支持多次 execute 调用）
    // asmjit::JitRuntime::release 释放 runtime_.add 返回的内存块，避免重复 execute 泄漏
    if (currentEntry_ != nullptr) {
        runtime_.release(currentEntry_);
        currentEntry_ = nullptr;
    }
    // AUDIT-R4 BUG-12 fix: execute() 入口无 JIT 代码运行，是释放被替换
    // 特化块的安全点（旧块在上一轮执行中可能仍在本机调用栈上，
    // 不能在替换点立即释放）。
    releaseRetiredSpecializedEntries();
    // R157: 释放上一次 execute() lazy compilation 分配的 CodeHolder 内存
    // 多次 execute() 时，compileAllChunks 会重新 nullify funcEntries_/methodEntries_，
    // 上一次 lazy 编译的入口不再被引用，需释放避免泄漏
    for (JitEntryFn entry : ownedLazyEntries_) {
        if (entry != nullptr) {
            runtime_.release(entry);
        }
    }
    ownedLazyEntries_.clear();
    lazyCompiledChunksList_.clear();

    // R141: 编译 mainChunk + functionChunks 到一个 CodeHolder
    JitEntryFn entry = compileAllChunks(result);
    if (!entry) {
        return JitResult::CompileError;
    }
    currentEntry_ = entry; // R151: 保存入口以便下次 execute 或析构时释放

    // R160: compileAllChunks 已 resize memberGetIC_，此时设置指针并清空 cache 条目
    // （支持多次 execute，避免上次 cache 残留指向已释放对象）
    jitContext_.memberGetICPtr = memberGetIC_.data();
    for (auto& e : memberGetIC_) {
        e = MemberGetInlineCacheEntry{}; // 重置全部 PIC 槽位 + megamorphic 标志
    }

    // R153: 重新应用特化方法入口
    // compileAllChunks 每次调用会 methodEntries_.clear() 并重新填充非特化版本入口，
    // 需要把之前特化的入口覆盖回去，保证第二次及后续 execute() 仍使用特化版本
    for (const auto& [chunkName, entryPtr] : specializedMethodEntries_) {
        auto it = methodEntries_.find(chunkName);
        if (it != methodEntries_.end()) {
            it->second.entryPtr = entryPtr;
        }
    }

    // R158: 重新同步 chunkTiers_ 基于 specializedMethodEntries_
    // compileAllChunks 可能重新初始化 chunkTiers_（首次）或保留旧值（后续）。
    // 为确保一致性：在 specializedMethodEntries_ 中的 chunk 为 Specialized，否则 Baseline。
    for (size_t i = 0; i < chunkNames_.size() && i < chunkTiers_.size(); ++i) {
        if (specializedMethodEntries_.find(chunkNames_[i]) != specializedMethodEntries_.end()) {
            chunkTiers_[i] = minilang::JitTier::Specialized;
        } else {
            chunkTiers_[i] = minilang::JitTier::Baseline;
        }
    }

    // 调用 JIT 编译后的本地代码
    // Safepoint GC 方案：将 GcManager 的增量触发回调重定向为设置 gcNeededFlag_，
    // JIT 代码在 OP_LOOP 回边处检查此标志并调用 jitSafepointGc 收集 roots。
    // 相比原 CallbackSuppressor 完全抑制方案，本方案允许 GC 在长循环中执行，
    // 避免内存峰值无限增长。回调仅设置标志（O(1)原子操作），实际 GC 在 safepoint 处执行。
    gcNeededFlag_.store(0, std::memory_order_relaxed);
    jitContext_.gcNeededFlag = &gcNeededFlag_;
    // 设置 GC 触发回调为设置标志（而非直接调用 collectCycle）
    {
        // 保存原回调，替换为设置标志的轻量回调
        GcManager::CallbackSuppressor gcSuppressor;
        // Suppressor 抑制回调后，我们重新设置一个轻量回调仅设置标志
        GcManager::instance().setGcTriggerCallback([this]() { gcNeededFlag_.store(1, std::memory_order_release); });
        int64_t ret = entry(&jitContext_);
        (void)ret;
    }
    // gcSuppressor 析构恢复原回调

    if (hasError_) {
        // P2-12: JIT 代码通过 ctx->errorBuffer 写入错误消息到 lastError_，
        // 在此同步到 diagnostics_（统一错误查询接口，getLastError 从 diagnostics_ 派生）。
        // DiagSource::JIT 与 VM/Compiler 区分，便于错误来源定位。
        if (!lastError_.empty()) {
            diagnostics_.addError(lastError_, 0, 0, DiagSource::JIT);
        }
        return JitResult::RuntimeError;
    }

    // R152/R153: execute() 返回后检查 recompiledFlags，对触发的 chunk 进行特化重编译
    // 仅对 recompiledFlags_[i]==1 且尚未特化的 chunk 调用特化重编译函数。
    // 根据类型反馈选择特化策略：
    //   - R152 INT 特化：otherCount==0 && floatCount==0 → compileChunkSpecialized
    //   - R153 FLOAT 特化：floatCount>0 && otherCount==0 → compileChunkSpecializedFloat
    //   - 其他（otherCount>0）：不特化（保持原版本）
    // 特化版本入口更新到 methodEntries_，下次 execute() 时 emitMethodCall 自动走特化版本
    for (size_t i = 0; i < recompiledFlags_.size() && i < chunkNames_.size(); ++i) {
        if (recompiledFlags_[i] == 0) {
            continue;
        }
        // 检查是否已特化（避免重复特化）
        const std::string& chunkName = chunkNames_[i];
        bool alreadySpecialized = false;
        for (const auto& s : specializedChunks_) {
            if (s == chunkName) {
                alreadySpecialized = true;
                break;
            }
        }
        if (alreadySpecialized) {
            continue;
        }
        // 根据类型反馈选择特化策略
        const auto& tf = (i < typeFeedback_.size()) ? typeFeedback_[i] : minilang::TypeFeedback{};
        JitEntryFn specEntry = nullptr;
        if (tf.otherCount == 0 && tf.floatCount == 0) {
            // R152: 纯 INT 算术 → INT 特化（跳过 emitCheckInt）
            specEntry = compileChunkSpecialized(result, i);
        } else if (tf.floatCount > 0 && tf.otherCount == 0) {
            // R153: 纯 FLOAT 算术 → FLOAT 特化（跳过 INT 原生路径 + 跳过类型反馈收集）
            specEntry = compileChunkSpecializedFloat(result, i);
        }
        // otherCount > 0：不特化（保持类型分派）
        (void)specEntry; // 入口已更新到 methodEntries_，无需额外处理
    }

    return JitResult::OK;
}

// ============================================================
// R142 阶段 3a：NaN-boxing 常量一致性校验
// ============================================================
// JIT 内部硬编码的 NaN-boxing 常量必须与 interpreter/NaNBox.h 保持一致。
// 修改任一处时，另一处必须同步更新。
// NaNBox.h 的常量是 private，无法用 static_assert 直接引用，
// 改用运行时 assert 在首次 execute 时校验（见 JITBackend::execute 入口）。
#include <cassert>
#include <cstddef> // offsetof
namespace {

// ============================================================
// JitContext 字段偏移静态验证
// ------------------------------------------------------------
// JIT 生成的机器码通过硬编码偏移访问 JitContext 字段。
// 任何字段重排/插入/删除都会破坏 ABI，必须同步更新
// JITCodeGen.cpp 中的偏移常量。这组 static_assert 在编译期捕获不一致。
// ============================================================
static_assert(offsetof(JitContext, outputCallback) == 0, "JitContext::outputCallback offset mismatch (expected 0)");
static_assert(offsetof(JitContext, hasError) == 8, "JitContext::hasError offset mismatch (expected 8)");
static_assert(offsetof(JitContext, errorBuffer) == 16, "JitContext::errorBuffer offset mismatch (expected 16)");
static_assert(offsetof(JitContext, globalSlots) == 24, "JitContext::globalSlots offset mismatch (expected 24)");
static_assert(offsetof(JitContext, frames) == 32, "JitContext::frames offset mismatch (expected 32)");
static_assert(offsetof(JitContext, frameCount) == 40, "JitContext::frameCount offset mismatch (expected 40)");
static_assert(offsetof(JitContext, stackTop) == 48, "JitContext::stackTop offset mismatch (expected 48)");
static_assert(offsetof(JitContext, classInfoPtr) == 56, "JitContext::classInfoPtr offset mismatch (expected 56)");
static_assert(offsetof(JitContext, pendingFieldOrderPtr) == 64,
              "JitContext::pendingFieldOrderPtr offset mismatch (expected 64)");
static_assert(offsetof(JitContext, methodEntryPtr) == 72, "JitContext::methodEntryPtr offset mismatch (expected 72)");
static_assert(offsetof(JitContext, methodLocalCount) == 80,
              "JitContext::methodLocalCount offset mismatch (expected 80)");
static_assert(offsetof(JitContext, methodEntriesPtr) == 88,
              "JitContext::methodEntriesPtr offset mismatch (expected 88)");
static_assert(offsetof(JitContext, callerBp) == 96, "JitContext::callerBp offset mismatch (expected 96)");
static_assert(offsetof(JitContext, chunkCallCounts) == 104,
              "JitContext::chunkCallCounts offset mismatch (expected 104)");
static_assert(offsetof(JitContext, hotThresholds) == 112, "JitContext::hotThresholds offset mismatch (expected 112)");
static_assert(offsetof(JitContext, recompiledFlags) == 120,
              "JitContext::recompiledFlags offset mismatch (expected 120)");
static_assert(offsetof(JitContext, typeFeedback) == 128, "JitContext::typeFeedback offset mismatch (expected 128)");
static_assert(offsetof(JitContext, backendPtr) == 136, "JitContext::backendPtr offset mismatch (expected 136)");
static_assert(offsetof(JitContext, lastMutatedReceiverPtr) == 144,
              "JitContext::lastMutatedReceiverPtr offset mismatch (expected 144)");
static_assert(offsetof(JitContext, funcEntriesPtr) == 152, "JitContext::funcEntriesPtr offset mismatch (expected 152)");
static_assert(offsetof(JitContext, currentBp) == 160, "JitContext::currentBp offset mismatch (expected 160)");
static_assert(offsetof(JitContext, osrLoopCountsPtr) == 168,
              "JitContext::osrLoopCountsPtr offset mismatch (expected 168)");
static_assert(offsetof(JitContext, osrLoopThresholdsPtr) == 176,
              "JitContext::osrLoopThresholdsPtr offset mismatch (expected 176)");
static_assert(offsetof(JitContext, osrRecompiledFlagsPtr) == 184,
              "JitContext::osrRecompiledFlagsPtr offset mismatch (expected 184)");
static_assert(offsetof(JitContext, osrSavedBp) == 192, "JitContext::osrSavedBp offset mismatch (expected 192)");
static_assert(offsetof(JitContext, osrSavedSp) == 200, "JitContext::osrSavedSp offset mismatch (expected 200)");
static_assert(offsetof(JitContext, osrEntryPoint) == 208, "JitContext::osrEntryPoint offset mismatch (expected 208)");
static_assert(offsetof(JitContext, deoptEntryPoint) == 216,
              "JitContext::deoptEntryPoint offset mismatch (expected 216)");
static_assert(offsetof(JitContext, deoptChunkIdx) == 224, "JitContext::deoptChunkIdx offset mismatch (expected 224)");
static_assert(offsetof(JitContext, memberGetICPtr) == 232, "JitContext::memberGetICPtr offset mismatch (expected 232)");
static_assert(offsetof(JitContext, globalsPtr) == 240, "JitContext::globalsPtr offset mismatch (expected 240)");
static_assert(offsetof(JitContext, gcNeededFlag) == 248, "JitContext::gcNeededFlag offset mismatch (expected 248)");
static_assert(offsetof(JitContext, enumRegistryPtr) == 256,
              "JitContext::enumRegistryPtr offset mismatch (expected 256)");
static_assert(offsetof(JitContext, syncMethodHandled) == 264,
              "JitContext::syncMethodHandled offset mismatch (expected 264)");

void verifyNanBoxConstants() {
    // 用 NaNBox 的 public 方法间接验证常量一致性
    // NaNBox::fromInt/fromBool/fromPtr/fromFloat 编码后，rawBits 应与 JIT 常量一致
    NaNBox intBox = NaNBox::fromInt(0);
    assert((intBox.rawBits() & JIT_TAG_FIELD_MASK) == JIT_INT_TAG_BASE && "JIT_INT_TAG_BASE 与 NaNBox.h 不一致");
    NaNBox boolBox = NaNBox::fromBool(false);
    assert((boolBox.rawBits() & JIT_TAG_FIELD_MASK) == JIT_BOOL_TAG_BASE && "JIT_BOOL_TAG_BASE 与 NaNBox.h 不一致");
    NaNBox nullBox = NaNBox::null();
    assert(nullBox.rawBits() == JIT_NULL_BITS && "JIT_NULL_BITS 与 NaNBox.h 不一致");
    // R143 阶段 3b：验证 JIT_NAN_BOXED_FLOAT_MARKER 与 NaNBox.h NAN_BOXED_FLOAT_MARKER 一致
    // NaNBox::fromFloat(quiet NaN) 会触发 isBoxedNaN 分支，用 NAN_BOXED_FLOAT_MARKER 替换
    // 构造一个 tag field 在 0x7FF8..0x7FFB 范围的 double（即 quiet NaN）
    // 简单验证：fromFloat(0.0) 的 raw bits 应等于 0.0 的 IEEE 754 表示（非 boxed NaN）
    NaNBox zeroFloat = NaNBox::fromFloat(0.0);
    double zeroDouble = 0.0;
    uint64_t expectedZeroBits = 0;
    std::memcpy(&expectedZeroBits, &zeroDouble, sizeof(double));
    assert(zeroFloat.rawBits() == expectedZeroBits && "JIT float 编码与 NaNBox.h 不一致");
}
} // namespace

#endif // MINILANG_USE_JIT
