/**
 * @file compiler/JITTiering.cpp
 * @brief JIT tiered compilation, OSR, and deoptimization (extracted from JIT.cpp).
 * @see JIT.h JITInternal.h
 * @since P3 (JIT.cpp split)
 */

#include "compiler/JIT.h"

#ifdef MINILANG_USE_JIT

#include "common/RuntimeLimits.h"
#include "compiler/JITInternal.h"
#include "interpreter/Value.h"
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using namespace jit_internal;

extern "C" {
extern "C" int64_t jitTriggerRecompile(JitContext* ctx, int64_t chunkIdx) {
    // PoC: 仅标记已触发，不执行实际重编译
    // recompiledFlags[chunkIdx] 已由 JIT 代码设置为 1
    (void)ctx;
    (void)chunkIdx;
    return 0;
}

// ============================================================
// R161 perf: jitReportStackOverflow — OP_CALL fast path 栈溢出错误报告
// ============================================================
// 快速路径的 MAX_FRAMES 检查触发时调用（罕见路径，C++ 调用开销可忽略）。
// 设置 hasError + errorBuffer，错误消息与 jitCallByName 对齐。
extern "C" int64_t jitTriggerOsrRecompile(JitContext* ctx, int64_t chunkIdx) {
    if (!ctx || !ctx->backendPtr) {
        return -1;
    }
    auto* backend = static_cast<JITBackend*>(ctx->backendPtr);
    return backend->triggerOsrRecompile(chunkIdx);
}

// ============================================================
// R158: jitTriggerOsrMigration — 真正 OSR 栈帧迁移回调
// ============================================================
// OP_LOOP 回边计数超阈值时调用（osrMigrationMode_=true 时）。
// 通过 backendPtr 调用 triggerOsrMigration，执行特化重编译（含 OSR 入口点生成），
// 通过 ctx->osrEntryPoint 邮箱返回 OSR 入口点地址。
// JIT 代码在调用前已将 r13/r15 保存到 ctx->osrSavedBp/osrSavedSp。
// 成功后 JIT 代码 jmp ctx->osrEntryPoint（特化版本的 OSR 入口点）。
extern "C" int64_t jitTriggerOsrMigration(JitContext* ctx, int64_t chunkIdx) {
    if (!ctx || !ctx->backendPtr) {
        return -1;
    }
    auto* backend = static_cast<JITBackend*>(ctx->backendPtr);
    return backend->triggerOsrMigration(chunkIdx);
}

// ============================================================
// R158: jitDeoptimize — 反优化回调
// ============================================================
// R159: 特化版本 emitCheckInt 类型守卫失败时由 JIT 生成的机器码调用。
// 通过 backendPtr 调用 triggerDeoptimize，即时降级 Tier 2→Tier 1：
//   - 恢复 methodEntries_/funcEntries_ 为 baseline 入口
//   - 更新 chunkTiers_ 为 Baseline
//   - 设置 ctx->deoptEntryPoint（供潜在的未来即时栈帧迁移用）
// JIT 代码调用后跳转 generic 路径正确处理当前操作，下次调用自动走 baseline 版本。
// r13/r15 是 callee-saved（Windows x64 ABI），本函数保证不修改。
extern "C" int64_t jitDeoptimize(JitContext* ctx, int64_t chunkIdx) {
    if (!ctx || !ctx->backendPtr) {
        return -1;
    }
    auto* backend = static_cast<JITBackend*>(ctx->backendPtr);
    return backend->triggerDeoptimize(chunkIdx);
}

// ============================================================

} // extern "C"

std::vector<std::pair<std::string, uint64_t>> JITBackend::getHotChunkStats() const {
    std::vector<std::pair<std::string, uint64_t>> stats;
    stats.reserve(chunkNames_.size());
    for (size_t i = 0; i < chunkNames_.size() && i < chunkCallCounts_.size(); ++i) {
        stats.emplace_back(chunkNames_[i], chunkCallCounts_[i]);
    }
    return stats;
}

// ============================================================
// R151: getRecompileStats — 返回 per-chunk 重编译触发统计
// ============================================================
std::vector<std::pair<std::string, uint64_t>> JITBackend::getRecompileStats() const {
    std::vector<std::pair<std::string, uint64_t>> stats;
    stats.reserve(chunkNames_.size());
    for (size_t i = 0; i < chunkNames_.size() && i < recompiledFlags_.size(); ++i) {
        stats.emplace_back(chunkNames_[i], recompiledFlags_[i]);
    }
    return stats;
}

// ============================================================
// R151: setHotThreshold — 设置自定义热点阈值（测试用，须在 execute 前调用）
// ============================================================
void JITBackend::setHotThreshold(const std::string& chunkName, uint64_t threshold) {
    customThresholds_[chunkName] = threshold;
}

// ============================================================
// R152: getTypeFeedback — 返回 per-chunk 类型反馈统计
// ============================================================
std::vector<std::pair<std::string, minilang::TypeFeedback>> JITBackend::getTypeFeedback() const {
    std::vector<std::pair<std::string, minilang::TypeFeedback>> stats;
    stats.reserve(chunkNames_.size());
    for (size_t i = 0; i < chunkNames_.size() && i < typeFeedback_.size(); ++i) {
        stats.emplace_back(chunkNames_[i], typeFeedback_[i]);
    }
    return stats;
}

// ============================================================
// R152: compileChunkSpecialized — INT 特化重编译单个 chunk
// ============================================================
// 在 execute() 返回后对 recompiledFlags 标记的 chunk 调用。
// 根据类型反馈决定是否特化：
//   - TypeFeedback.otherCount == 0 且 floatCount == 0 → INT 特化（跳过 emitCheckInt）
//   - 否则 → 不特化（返回 nullptr）
//
// 实现策略：复用 compileAllChunks 的全部逻辑，通过 specializeIntMode_ 标志控制
// emitCheckInt 不生成类型检查代码。compileAllChunks 编译所有 chunk 为特化版本，
// 从中提取目标 chunk 的入口并丢弃其他 chunk 的特化版本。
//
// 内存管理：特化版本的 CodeHolder 内存由 ownedSpecializedEntries_ 持有，析构时释放。
// 入口切换：更新 methodEntries_[chunkName].entryPtr 为特化版本入口，下次 execute()
// 时 emitMethodCall 自动 jmp 到特化版本。
JitEntryFn JITBackend::compileChunkSpecialized(const CompileResult& result, size_t chunkIdx) {
    if (currentResult_ == nullptr) {
        return nullptr;
    }
    if (chunkIdx >= chunkNames_.size()) {
        return nullptr;
    }

    // 检查类型反馈：仅当 otherCount==0 && floatCount==0 时才特化
    if (chunkIdx >= typeFeedback_.size()) {
        return nullptr;
    }
    const auto& tf = typeFeedback_[chunkIdx];
    if (tf.otherCount > 0 || tf.floatCount > 0) {
        // 类型反馈显示非纯 INT，不特化（保持原版本）
        return nullptr;
    }

    // 保存原 methodEntries_ 和 currentEntry_（compileAllChunks 会覆盖）
    auto savedMethodEntries = methodEntries_;
    // Bug #51 note: savedCurrentEntry 仅保存函数指针（指向 asmjit runtime 分配的
    // 可执行内存）。当前安全性依赖于 asmjit::JitRuntime::add() 仅追加分配、
    // 不释放已有代码这一事实——因此重编译期间 savedCurrentEntry 不会悬垂。
    // 若未来引入代码回收/重用机制，需改用 RAII guard 或在恢复前重新验证指针有效性。
    JitEntryFn savedCurrentEntry = currentEntry_;
    // R152 fix: 保存 recompiledFlags_/chunkCallCounts_/typeFeedback_/hotThresholds_/chunkNames_
    // compileAllChunks 会重置这些数组，导致 R151 测试在 execute() 返回后检查 recompiledFlags_ 时
    // 发现已被重置为 0。必须保存并在特化后恢复。
    auto savedRecompiledFlags = recompiledFlags_;
    auto savedChunkCallCounts = chunkCallCounts_;
    auto savedTypeFeedback = typeFeedback_;
    auto savedHotThresholds = hotThresholds_;
    auto savedChunkNames = chunkNames_;
    auto savedGlobalNameToSlot = globalNameToSlot_;
    auto savedClassInfo = classInfo_;
    auto savedPendingFieldOrder = pendingFieldOrder_;
    auto savedMethodLabels = std::move(specializedChunks_); // 保存已特化列表（临时清空避免干扰）

    // 设置特化模式标志
    specializeIntMode_ = true;

    // 调用 compileAllChunks 编译所有 chunk 为特化版本
    // 注意：compileAllChunks 会调用 runtime_.add 分配新的可执行内存
    JitEntryFn specializedEntry = compileAllChunks(result);

    // 恢复特化模式标志
    specializeIntMode_ = false;

    if (!specializedEntry) {
        // 特化编译失败，恢复原状态
        methodEntries_ = std::move(savedMethodEntries);
        currentEntry_ = savedCurrentEntry;
        recompiledFlags_ = std::move(savedRecompiledFlags);
        chunkCallCounts_ = std::move(savedChunkCallCounts);
        typeFeedback_ = std::move(savedTypeFeedback);
        hotThresholds_ = std::move(savedHotThresholds);
        chunkNames_ = std::move(savedChunkNames);
        globalNameToSlot_ = std::move(savedGlobalNameToSlot);
        classInfo_ = std::move(savedClassInfo);
        pendingFieldOrder_ = std::move(savedPendingFieldOrder);
        specializedChunks_ = std::move(savedMethodLabels);
        return nullptr;
    }

    // 特化版本的 methodEntries_ 现在包含所有方法 chunk 的特化入口
    // 提取目标 chunk 的特化入口（chunkNames_ 已被 compileAllChunks 重建，内容相同）
    const std::string targetChunkName = (chunkIdx < chunkNames_.size()) ? chunkNames_[chunkIdx] : std::string{};
    auto it = methodEntries_.find(targetChunkName);
    void* specializedMethodEntry = nullptr;
    if (it != methodEntries_.end()) {
        specializedMethodEntry = it->second.entryPtr;
    }

    // 恢复原状态（compileAllChunks 重置了这些数组）
    methodEntries_ = std::move(savedMethodEntries);
    currentEntry_ = savedCurrentEntry;
    recompiledFlags_ = std::move(savedRecompiledFlags);
    chunkCallCounts_ = std::move(savedChunkCallCounts);
    typeFeedback_ = std::move(savedTypeFeedback);
    hotThresholds_ = std::move(savedHotThresholds);
    chunkNames_ = std::move(savedChunkNames);
    globalNameToSlot_ = std::move(savedGlobalNameToSlot);
    classInfo_ = std::move(savedClassInfo);
    pendingFieldOrder_ = std::move(savedPendingFieldOrder);
    specializedChunks_ = std::move(savedMethodLabels);

    if (!specializedMethodEntry) {
        // 目标 chunk 不是方法 chunk（无 methodEntries_ 条目），无法切换入口
        // 释放特化版本的 CodeHolder 内存
        runtime_.release(specializedEntry);
        return nullptr;
    }

    // 更新原 methodEntries_[targetChunkName].entryPtr 为特化版本入口
    auto origIt = methodEntries_.find(targetChunkName);
    if (origIt != methodEntries_.end()) {
        origIt->second.entryPtr = specializedMethodEntry;
    }

    // R153: 持久化特化入口映射，下次 execute() compileAllChunks 后重新应用
    // （compileAllChunks 每次调用会 methodEntries_.clear() 并重新填充，覆盖特化入口）
    specializedMethodEntries_[targetChunkName] = specializedMethodEntry;

    // 保存特化版本 CodeHolder 内存所有权，析构时释放
    registerSpecializedOwnership(targetChunkName, specializedEntry); // AUDIT-R4 BUG-12: 旧块退休+新块登记

    // 记录已特化的 chunk 名称
    specializedChunks_.push_back(targetChunkName);

    // 保存特化入口指针（按 chunkIdx 索引，测试验证用）
    if (specializedEntries_.size() <= chunkIdx) {
        specializedEntries_.resize(chunkIdx + 1, nullptr);
    }
    specializedEntries_[chunkIdx] = specializedEntry;

    return specializedEntry;
}

// ============================================================
// AUDIT-R4 BUG-12 fix: 特化块所有权登记与退休释放
// ------------------------------------------------------------
// 同一 chunk 被重复特化时，旧块不再被 specializedMethodEntries_ 引用，
// 但原实现仅在析构时释放，长会话反复去优化/再特化导致可执行内存
// 单调增长。旧块移入退休列表，在下次 execute() 入口（无 JIT 代码
// 运行的安全点）与析构时统一释放。
// ============================================================
void JITBackend::registerSpecializedOwnership(const std::string& chunkName, JitEntryFn specializedEntry) {
    auto oldIt = ownedSpecializedByChunk_.find(chunkName);
    if (oldIt != ownedSpecializedByChunk_.end() && oldIt->second != specializedEntry) {
        retiredSpecializedEntries_.push_back(oldIt->second);
        auto vecIt = std::find(ownedSpecializedEntries_.begin(), ownedSpecializedEntries_.end(), oldIt->second);
        if (vecIt != ownedSpecializedEntries_.end())
            ownedSpecializedEntries_.erase(vecIt);
    }
    ownedSpecializedByChunk_[chunkName] = specializedEntry;
    ownedSpecializedEntries_.push_back(specializedEntry);
}

void JITBackend::releaseRetiredSpecializedEntries() {
    for (JitEntryFn entry : retiredSpecializedEntries_) {
        if (entry != nullptr) {
            runtime_.release(entry);
        }
    }
    retiredSpecializedEntries_.clear();
}

// ============================================================
// R153: compileChunkSpecializedFloat — FLOAT 特化重编译单个 chunk
// ============================================================
// 在 execute() 返回后对 recompiledFlags 标记的 chunk 调用。
// 根据类型反馈决定是否特化：
//   - TypeFeedback.floatCount > 0 且 otherCount == 0 → FLOAT 特化
//     （emitCheckInt 生成无条件 jmp 跳过 INT 原生路径，emitRecordTypeFeedback 不生成代码）
//   - 否则 → 不特化（返回 nullptr）
//
// 实现策略：复用 compileAllChunks 的全部逻辑，通过 specializeFloatMode_ 标志控制
// emitCheckInt 生成无条件 jmp 跳过 INT 原生路径，emitRecordTypeFeedback 不生成代码。
// compileAllChunks 编译所有 chunk 为 FLOAT 特化版本，从中提取目标 chunk 的入口并丢弃其他 chunk。
//
// 性能提升：跳过 INT 类型检查（6→2 条 jmp）+ 跳过类型反馈收集（7→0 条指令）+
// 跳过 INT 原生路径溢出检查（运行时不执行）。操作数均走 genericXXX 路径调用 C++ 辅助函数。
//
// 内存管理：与 INT 特化版本一致，特化版本 CodeHolder 内存由 ownedSpecializedEntries_ 持有。
JitEntryFn JITBackend::compileChunkSpecializedFloat(const CompileResult& result, size_t chunkIdx) {
    if (currentResult_ == nullptr) {
        return nullptr;
    }
    if (chunkIdx >= chunkNames_.size()) {
        return nullptr;
    }

    // 检查类型反馈：仅当 floatCount>0 && otherCount==0 时才特化
    if (chunkIdx >= typeFeedback_.size()) {
        return nullptr;
    }
    const auto& tf = typeFeedback_[chunkIdx];
    if (tf.floatCount == 0 || tf.otherCount > 0) {
        // 类型反馈显示非纯 FLOAT，不特化（保持原版本）
        return nullptr;
    }

    // 保存原 methodEntries_ 和 currentEntry_（compileAllChunks 会覆盖）
    auto savedMethodEntries = methodEntries_;
    // Bug #51 note: savedCurrentEntry 仅保存函数指针（指向 asmjit runtime 分配的
    // 可执行内存）。当前安全性依赖于 asmjit::JitRuntime::add() 仅追加分配、
    // 不释放已有代码这一事实——因此重编译期间 savedCurrentEntry 不会悬垂。
    // 若未来引入代码回收/重用机制，需改用 RAII guard 或在恢复前重新验证指针有效性。
    JitEntryFn savedCurrentEntry = currentEntry_;
    // 保存所有 compileAllChunks 会重置的数组成员（与 INT 特化保持一致）
    auto savedRecompiledFlags = recompiledFlags_;
    auto savedChunkCallCounts = chunkCallCounts_;
    auto savedTypeFeedback = typeFeedback_;
    auto savedHotThresholds = hotThresholds_;
    auto savedChunkNames = chunkNames_;
    auto savedGlobalNameToSlot = globalNameToSlot_;
    auto savedClassInfo = classInfo_;
    auto savedPendingFieldOrder = pendingFieldOrder_;
    auto savedMethodLabels = std::move(specializedChunks_); // 保存已特化列表（临时清空避免干扰）

    // 设置 FLOAT 特化模式标志
    specializeFloatMode_ = true;

    // 调用 compileAllChunks 编译所有 chunk 为 FLOAT 特化版本
    JitEntryFn specializedEntry = compileAllChunks(result);

    // 恢复特化模式标志
    specializeFloatMode_ = false;

    if (!specializedEntry) {
        // 特化编译失败，恢复原状态
        methodEntries_ = std::move(savedMethodEntries);
        currentEntry_ = savedCurrentEntry;
        recompiledFlags_ = std::move(savedRecompiledFlags);
        chunkCallCounts_ = std::move(savedChunkCallCounts);
        typeFeedback_ = std::move(savedTypeFeedback);
        hotThresholds_ = std::move(savedHotThresholds);
        chunkNames_ = std::move(savedChunkNames);
        globalNameToSlot_ = std::move(savedGlobalNameToSlot);
        classInfo_ = std::move(savedClassInfo);
        pendingFieldOrder_ = std::move(savedPendingFieldOrder);
        specializedChunks_ = std::move(savedMethodLabels);
        return nullptr;
    }

    // 特化版本的 methodEntries_ 现在包含所有方法 chunk 的特化入口
    // 提取目标 chunk 的特化入口
    const std::string targetChunkName = (chunkIdx < chunkNames_.size()) ? chunkNames_[chunkIdx] : std::string{};
    auto it = methodEntries_.find(targetChunkName);
    void* specializedMethodEntry = nullptr;
    if (it != methodEntries_.end()) {
        specializedMethodEntry = it->second.entryPtr;
    }

    // 恢复原状态（compileAllChunks 重置了这些数组）
    methodEntries_ = std::move(savedMethodEntries);
    currentEntry_ = savedCurrentEntry;
    recompiledFlags_ = std::move(savedRecompiledFlags);
    chunkCallCounts_ = std::move(savedChunkCallCounts);
    typeFeedback_ = std::move(savedTypeFeedback);
    hotThresholds_ = std::move(savedHotThresholds);
    chunkNames_ = std::move(savedChunkNames);
    globalNameToSlot_ = std::move(savedGlobalNameToSlot);
    classInfo_ = std::move(savedClassInfo);
    pendingFieldOrder_ = std::move(savedPendingFieldOrder);
    specializedChunks_ = std::move(savedMethodLabels);

    if (!specializedMethodEntry) {
        // 目标 chunk 不是方法 chunk（无 methodEntries_ 条目），无法切换入口
        runtime_.release(specializedEntry);
        return nullptr;
    }

    // 更新原 methodEntries_[targetChunkName].entryPtr 为特化版本入口
    auto origIt = methodEntries_.find(targetChunkName);
    if (origIt != methodEntries_.end()) {
        origIt->second.entryPtr = specializedMethodEntry;
    }

    // R153: 持久化特化入口映射，下次 execute() compileAllChunks 后重新应用
    // （compileAllChunks 每次调用会 methodEntries_.clear() 并重新填充，覆盖特化入口）
    specializedMethodEntries_[targetChunkName] = specializedMethodEntry;

    // 保存特化版本 CodeHolder 内存所有权，析构时释放
    registerSpecializedOwnership(targetChunkName, specializedEntry); // AUDIT-R4 BUG-12: 旧块退休+新块登记

    // 记录已特化的 chunk 名称
    specializedChunks_.push_back(targetChunkName);

    // 保存特化入口指针（按 chunkIdx 索引，测试验证用）
    if (specializedEntries_.size() <= chunkIdx) {
        specializedEntries_.resize(chunkIdx + 1, nullptr);
    }
    specializedEntries_[chunkIdx] = specializedEntry;

    return specializedEntry;
}

// ============================================================
// R157: compileSingleChunkLazy — 按需编译单个 chunk 到独立 CodeHolder
// ------------------------------------------------------------
// 在 lazyMode_ 为 true 且函数/方法首次调用时，由 jitCallByName 通过
// backendPtr 触发。保存所有 compileAllChunks 会重置的状态 → 调用
// compileAllChunks 编译所有 chunk → 提取目标 chunk 入口 → 恢复原状态 →
// 更新原 funcEntries_/methodEntries_ 的 entryPtr → 保存内存所有权到
// ownedLazyEntries_ → 记录到 lazyCompiledChunksList_。
//
// 简化版策略：lazy 编译仍编译全部 chunk（避免 chunk 间引用解析复杂度），
// 但只提取目标 chunk 入口并更新原映射，使原 jitCallByName 查找返回
// 非空入口。后续同 chunk 调用直接走已更新映射，无再次 lazy 触发。
// ============================================================
JitEntryFn JITBackend::compileSingleChunkLazy(const CompileResult& result, const std::string& chunkName) {
    if (!currentResult_) {
        return nullptr;
    }

    // R157: 保存所有 compileAllChunks 会重置的状态（与 compileChunkSpecialized 保持一致）
    // compileAllChunks 会重置 funcEntries_/methodEntries_/specializedMethodEntries_/
    // chunkCallCounts_/chunkNames_/hotThresholds_/recompiledFlags_/typeFeedback_/
    // osrLoopCounts_/osrLoopThresholds_/osrRecompiledFlags_ 等。
    // 不保存/恢复会导致运行时统计丢失（如 OSR 回边计数被清零后重复触发）。
    auto savedFuncEntries = funcEntries_;
    auto savedMethodEntries = methodEntries_;
    auto savedSpecializedMethodEntries = specializedMethodEntries_;
    JitEntryFn savedCurrentEntry = currentEntry_;
    auto savedRecompiledFlags = recompiledFlags_;
    auto savedChunkCallCounts = chunkCallCounts_;
    auto savedTypeFeedback = typeFeedback_;
    auto savedHotThresholds = hotThresholds_;
    auto savedChunkNames = chunkNames_;
    auto savedSpecializedChunks = specializedChunks_;
    auto savedOsrLoopCounts = osrLoopCounts_;
    auto savedOsrLoopThresholds = osrLoopThresholds_;
    auto savedOsrRecompiledFlags = osrRecompiledFlags_;
    auto savedChunkTiers = chunkTiers_; // R159: 保存 chunkTiers_ 防止 inner compileAllChunks 覆盖

    // 调用 compileAllChunks 编译全部 chunk 到新 CodeHolder
    // 注意：compileAllChunks 会重置上述数组成员并重新填充
    // R157 关键：临时禁用 lazyMode_，使新编译的 funcEntries_/methodEntries_
    // 包含真实 entryPtr（而非 null），否则无法提取目标 chunk 入口。
    bool savedLazyMode = lazyMode_;
    lazyMode_ = false;
    JitEntryFn newEntry = compileAllChunks(result);
    lazyMode_ = savedLazyMode;
    if (newEntry == nullptr) {
        // 编译失败，恢复原状态
        funcEntries_ = std::move(savedFuncEntries);
        methodEntries_ = std::move(savedMethodEntries);
        specializedMethodEntries_ = std::move(savedSpecializedMethodEntries);
        currentEntry_ = savedCurrentEntry;
        recompiledFlags_ = std::move(savedRecompiledFlags);
        chunkCallCounts_ = std::move(savedChunkCallCounts);
        typeFeedback_ = std::move(savedTypeFeedback);
        hotThresholds_ = std::move(savedHotThresholds);
        chunkNames_ = std::move(savedChunkNames);
        specializedChunks_ = std::move(savedSpecializedChunks);
        osrLoopCounts_ = std::move(savedOsrLoopCounts);
        osrLoopThresholds_ = std::move(savedOsrLoopThresholds);
        osrRecompiledFlags_ = std::move(savedOsrRecompiledFlags);
        chunkTiers_ = std::move(savedChunkTiers);
        return nullptr;
    }

    // 从新编译的 funcEntries_ 提取目标 chunk 入口（普通函数名查找）
    JitEntryFn targetEntry = nullptr;
    auto funcIt = funcEntries_.find(chunkName);
    if (funcIt != funcEntries_.end()) {
        targetEntry = reinterpret_cast<JitEntryFn>(funcIt->second.entryPtr);
    } else {
        // 也尝试 methodEntries_（"Class.method" 格式）
        auto methodIt = methodEntries_.find(chunkName);
        if (methodIt != methodEntries_.end()) {
            targetEntry = reinterpret_cast<JitEntryFn>(methodIt->second.entryPtr);
        }
    }

    if (targetEntry == nullptr) {
        // 未找到目标 chunk，恢复原状态
        funcEntries_ = std::move(savedFuncEntries);
        methodEntries_ = std::move(savedMethodEntries);
        specializedMethodEntries_ = std::move(savedSpecializedMethodEntries);
        runtime_.release(newEntry);
        currentEntry_ = savedCurrentEntry;
        recompiledFlags_ = std::move(savedRecompiledFlags);
        chunkCallCounts_ = std::move(savedChunkCallCounts);
        typeFeedback_ = std::move(savedTypeFeedback);
        hotThresholds_ = std::move(savedHotThresholds);
        chunkNames_ = std::move(savedChunkNames);
        specializedChunks_ = std::move(savedSpecializedChunks);
        osrLoopCounts_ = std::move(savedOsrLoopCounts);
        osrLoopThresholds_ = std::move(savedOsrLoopThresholds);
        osrRecompiledFlags_ = std::move(savedOsrRecompiledFlags);
        chunkTiers_ = std::move(savedChunkTiers);
        return nullptr;
    }

    // 保存新 CodeHolder 内存所有权到 ownedLazyEntries_（避免被下次 compileAllChunks 释放）
    ownedLazyEntries_.push_back(newEntry);

    // 恢复原状态
    funcEntries_ = std::move(savedFuncEntries);
    methodEntries_ = std::move(savedMethodEntries);
    specializedMethodEntries_ = std::move(savedSpecializedMethodEntries);
    currentEntry_ = savedCurrentEntry;
    recompiledFlags_ = std::move(savedRecompiledFlags);
    chunkCallCounts_ = std::move(savedChunkCallCounts);
    typeFeedback_ = std::move(savedTypeFeedback);
    hotThresholds_ = std::move(savedHotThresholds);
    chunkNames_ = std::move(savedChunkNames);
    specializedChunks_ = std::move(savedSpecializedChunks);
    osrLoopCounts_ = std::move(savedOsrLoopCounts);
    osrLoopThresholds_ = std::move(savedOsrLoopThresholds);
    osrRecompiledFlags_ = std::move(savedOsrRecompiledFlags);
    chunkTiers_ = std::move(savedChunkTiers);

    // R157 fix: 恢复状态后必须重新同步 jitContext_ 中所有指向 data() 的字段。
    // compileAllChunks 内部会为 chunkCallCounts_/hotThresholds_/recompiledFlags_/
    // typeFeedback_/osrLoopCounts_/osrLoopThresholds_/osrRecompiledFlags_ 重新分配
    // 内存并设置 jitContext_ 指向新 data()。恢复旧 vector 后这些指针变为悬空，
    // 导致后续 JIT 代码通过 jitContext_ 访问到无效内存（崩溃根因）。
    jitContext_.chunkCallCounts = chunkCallCounts_.data();
    jitContext_.hotThresholds = hotThresholds_.data();
    jitContext_.recompiledFlags = recompiledFlags_.data();
    jitContext_.typeFeedback = typeFeedback_.data();
    jitContext_.osrLoopCountsPtr = osrLoopCounts_.data();
    jitContext_.osrLoopThresholdsPtr = osrLoopThresholds_.data();
    jitContext_.osrRecompiledFlagsPtr = osrRecompiledFlags_.data();
    jitContext_.backendPtr = this;

    // 更新原映射中目标 chunk 的 entryPtr
    auto origFuncIt = funcEntries_.find(chunkName);
    if (origFuncIt != funcEntries_.end()) {
        origFuncIt->second.entryPtr = reinterpret_cast<void*>(targetEntry);
    } else {
        auto origMethodIt = methodEntries_.find(chunkName);
        if (origMethodIt != methodEntries_.end()) {
            origMethodIt->second.entryPtr = reinterpret_cast<void*>(targetEntry);
        }
    }

    // 记录已 lazy 编译的 chunk 名称（去重）
    if (std::find(lazyCompiledChunksList_.begin(), lazyCompiledChunksList_.end(), chunkName) ==
        lazyCompiledChunksList_.end()) {
        lazyCompiledChunksList_.push_back(chunkName);
    }

    // R159: Tier 0→Tier 1 自动升级 — lazy compile 成功后更新 chunkTiers_
    // 分层编译模式下，非 main chunk 初始为 Tier 0 (Interpreter)，首次调用触发
    // lazy compilation 升级到 Tier 1 (Baseline)。chunkNames_ 与 chunkTiers_ 在
    // compileAllChunks 中按相同顺序构建，此处按名查找对应索引并升级。
    if (tieredMode_) {
        for (size_t i = 0; i < chunkNames_.size() && i < chunkTiers_.size(); ++i) {
            if (chunkNames_[i] == chunkName && chunkTiers_[i] == minilang::JitTier::Interpreter) {
                chunkTiers_[i] = minilang::JitTier::Baseline;
                break;
            }
        }
    }

    return targetEntry;
}

// ============================================================
// R157: triggerOsrRecompile — OSR 重编译触发器
// ------------------------------------------------------------
// 当 OP_LOOP 回边计数达到阈值时由 JIT 代码通过 jitTriggerOsrRecompile
// 回调。根据 chunk 的类型反馈决定特化策略：
//   - otherCount==0 && floatCount==0 → INT 特化（compileChunkSpecialized）
//   - floatCount>0 && otherCount==0 → FLOAT 特化（compileChunkSpecializedFloat）
//   - otherCount>0 → 不特化（返回 -1）
// 教学版简化：触发后下次调用走特化版本，非真正 OSR 栈帧迁移。
// ============================================================
int64_t JITBackend::triggerOsrRecompile(int64_t chunkIdx) {
    if (!currentResult_ || chunkIdx < 0 || static_cast<size_t>(chunkIdx) >= osrRecompiledFlags_.size()) {
        return -1;
    }

    // 已触发过则跳过（避免重复触发）
    if (osrRecompiledFlags_[static_cast<size_t>(chunkIdx)] != 0) {
        return 0;
    }

    // 标记为已触发
    osrRecompiledFlags_[static_cast<size_t>(chunkIdx)] = 1;

    // 根据类型反馈决定特化策略
    const auto& tf = typeFeedback_[static_cast<size_t>(chunkIdx)];
    JitEntryFn specializedEntry = nullptr;

    if (tf.otherCount == 0 && tf.floatCount == 0) {
        // INT 特化
        specializedEntry = compileChunkSpecialized(*currentResult_, static_cast<size_t>(chunkIdx));
    } else if (tf.floatCount > 0 && tf.otherCount == 0) {
        // FLOAT 特化
        specializedEntry = compileChunkSpecializedFloat(*currentResult_, static_cast<size_t>(chunkIdx));
    } else {
        // 不特化
        return -1;
    }

    // R157 fix: compileChunkSpecialized/Float 内部调用 compileAllChunks 会重置
    // osrRecompiledFlags_ 为全 0（compileAllChunks 末尾 assign(size, 0)），
    // 导致 JIT 代码后续循环迭代看到 flag==0 重新触发 OSR。
    // 特化成功后重新设置标志，避免重复触发。
    if (specializedEntry != nullptr) {
        if (static_cast<size_t>(chunkIdx) < osrRecompiledFlags_.size()) {
            osrRecompiledFlags_[static_cast<size_t>(chunkIdx)] = 1;
        }
    }

    return specializedEntry != nullptr ? 1 : -1;
}

// ============================================================
// R157: setOsrThreshold — 设置 per-chunk OSR 阈值
// ============================================================
void JITBackend::setOsrThreshold(const std::string& chunkName, uint64_t threshold) {
    customOsrThresholds_[chunkName] = threshold;
}

// ============================================================
// R157: getOsrLoopStats — 获取 per-chunk 循环回边计数统计
// ============================================================
std::vector<std::pair<std::string, uint64_t>> JITBackend::getOsrLoopStats() const {
    std::vector<std::pair<std::string, uint64_t>> result;
    for (size_t i = 0; i < chunkNames_.size() && i < osrLoopCounts_.size(); ++i) {
        result.emplace_back(chunkNames_[i], osrLoopCounts_[i]);
    }
    return result;
}

// ============================================================
// R157: getOsrRecompileStats — 获取 per-chunk OSR 触发统计
// ============================================================
std::vector<std::pair<std::string, uint64_t>> JITBackend::getOsrRecompileStats() const {
    std::vector<std::pair<std::string, uint64_t>> result;
    for (size_t i = 0; i < chunkNames_.size() && i < osrRecompiledFlags_.size(); ++i) {
        result.emplace_back(chunkNames_[i], osrRecompiledFlags_[i]);
    }
    return result;
}

// ============================================================
// R158: compileChunkSpecializedWithOsr — INT 特化重编译（含 OSR 入口点）
// ------------------------------------------------------------
// 与 R152 compileChunkSpecialized 类似，但启用 OSR 入口点生成模式。
// compileAllChunks 在目标 chunk 的第一个 OP_LOOP 位置生成 OSR 入口 Label，
// 该 Label 恢复 r13/r15 从邮箱后跳转到循环回边目标。
// OSR 入口点地址通过 osrEntryPointOut_ 输出。
//
// 设计要点：
//   1. 保存/恢复模式与 compileChunkSpecialized 一致（12+ 成员）
//   2. 额外保存/恢复 osrEntryGenMode_/osrTargetChunkIdx_/osrEntryLabelGenerated_/osrEntryPointOut_
//   3. compileAllChunks 内部检测 osrEntryGenMode_ 并生成 OSR 入口 Label
//   4. OSR 入口点地址在 runtime_.add 后由 compileAllChunks 写入 *osrEntryPointOut_
// ============================================================
JitEntryFn JITBackend::compileChunkSpecializedWithOsr(const CompileResult& result, size_t chunkIdx,
                                                      void** osrEntryPointPtr) {
    if (currentResult_ == nullptr || osrEntryPointPtr == nullptr) {
        return nullptr;
    }
    if (chunkIdx >= chunkNames_.size()) {
        return nullptr;
    }

    // 检查类型反馈：仅当 otherCount==0 && floatCount==0 时才 INT 特化
    if (chunkIdx >= typeFeedback_.size()) {
        return nullptr;
    }
    const auto& tf = typeFeedback_[chunkIdx];
    if (tf.otherCount > 0 || tf.floatCount > 0) {
        return nullptr;
    }

    // 保存原状态（与 compileChunkSpecialized 保持一致）
    auto savedMethodEntries = methodEntries_;
    JitEntryFn savedCurrentEntry = currentEntry_;
    auto savedRecompiledFlags = recompiledFlags_;
    auto savedChunkCallCounts = chunkCallCounts_;
    auto savedTypeFeedback = typeFeedback_;
    auto savedHotThresholds = hotThresholds_;
    auto savedChunkNames = chunkNames_;
    auto savedGlobalNameToSlot = globalNameToSlot_;
    auto savedClassInfo = classInfo_;
    auto savedPendingFieldOrder = pendingFieldOrder_;
    auto savedMethodLabels = std::move(specializedChunks_);
    auto savedFuncEntries = funcEntries_;
    auto savedSpecializedMethodEntries = specializedMethodEntries_;
    auto savedOsrLoopCounts = osrLoopCounts_;
    auto savedOsrLoopThresholds = osrLoopThresholds_;
    auto savedOsrRecompiledFlags = osrRecompiledFlags_;
    // R158: 保存 OSR 入口点生成模式状态
    bool savedOsrEntryGenMode = osrEntryGenMode_;
    size_t savedOsrTargetChunkIdx = osrTargetChunkIdx_;
    bool savedOsrEntryLabelGenerated = osrEntryLabelGenerated_;
    void** savedOsrEntryPointOut = osrEntryPointOut_;

    // 设置 INT 特化 + OSR 入口点生成模式
    specializeIntMode_ = true;
    osrEntryGenMode_ = true;
    osrTargetChunkIdx_ = chunkIdx;
    osrEntryLabelGenerated_ = false;
    osrEntryPointOut_ = osrEntryPointPtr;
    *osrEntryPointPtr = nullptr; // 初始化为 null

    // 调用 compileAllChunks 编译所有 chunk 为 INT 特化版本（含 OSR 入口点）
    JitEntryFn specializedEntry = compileAllChunks(result);

    // 恢复特化模式标志
    specializeIntMode_ = false;
    osrEntryGenMode_ = false;

    if (!specializedEntry) {
        // 特化编译失败，恢复原状态
        methodEntries_ = std::move(savedMethodEntries);
        currentEntry_ = savedCurrentEntry;
        recompiledFlags_ = std::move(savedRecompiledFlags);
        chunkCallCounts_ = std::move(savedChunkCallCounts);
        typeFeedback_ = std::move(savedTypeFeedback);
        hotThresholds_ = std::move(savedHotThresholds);
        chunkNames_ = std::move(savedChunkNames);
        globalNameToSlot_ = std::move(savedGlobalNameToSlot);
        classInfo_ = std::move(savedClassInfo);
        pendingFieldOrder_ = std::move(savedPendingFieldOrder);
        specializedChunks_ = std::move(savedMethodLabels);
        funcEntries_ = std::move(savedFuncEntries);
        specializedMethodEntries_ = std::move(savedSpecializedMethodEntries);
        osrLoopCounts_ = std::move(savedOsrLoopCounts);
        osrLoopThresholds_ = std::move(savedOsrLoopThresholds);
        osrRecompiledFlags_ = std::move(savedOsrRecompiledFlags);
        osrEntryGenMode_ = savedOsrEntryGenMode;
        osrTargetChunkIdx_ = savedOsrTargetChunkIdx;
        osrEntryLabelGenerated_ = savedOsrEntryLabelGenerated;
        osrEntryPointOut_ = savedOsrEntryPointOut;
        // R157 教训：恢复状态后重新同步 jitContext_ 指针
        jitContext_.chunkCallCounts = chunkCallCounts_.data();
        jitContext_.hotThresholds = hotThresholds_.data();
        jitContext_.recompiledFlags = recompiledFlags_.data();
        jitContext_.typeFeedback = typeFeedback_.data();
        jitContext_.osrLoopCountsPtr = osrLoopCounts_.data();
        jitContext_.osrLoopThresholdsPtr = osrLoopThresholds_.data();
        jitContext_.osrRecompiledFlagsPtr = osrRecompiledFlags_.data();
        jitContext_.backendPtr = this;
        return nullptr;
    }

    // 提取目标 chunk 的特化入口
    const std::string targetChunkName = (chunkIdx < chunkNames_.size()) ? chunkNames_[chunkIdx] : std::string{};
    auto it = methodEntries_.find(targetChunkName);
    void* specializedMethodEntry = nullptr;
    if (it != methodEntries_.end()) {
        specializedMethodEntry = it->second.entryPtr;
    }

    // 恢复原状态
    methodEntries_ = std::move(savedMethodEntries);
    currentEntry_ = savedCurrentEntry;
    recompiledFlags_ = std::move(savedRecompiledFlags);
    chunkCallCounts_ = std::move(savedChunkCallCounts);
    typeFeedback_ = std::move(savedTypeFeedback);
    hotThresholds_ = std::move(savedHotThresholds);
    chunkNames_ = std::move(savedChunkNames);
    globalNameToSlot_ = std::move(savedGlobalNameToSlot);
    classInfo_ = std::move(savedClassInfo);
    pendingFieldOrder_ = std::move(savedPendingFieldOrder);
    specializedChunks_ = std::move(savedMethodLabels);
    funcEntries_ = std::move(savedFuncEntries);
    specializedMethodEntries_ = std::move(savedSpecializedMethodEntries);
    osrLoopCounts_ = std::move(savedOsrLoopCounts);
    osrLoopThresholds_ = std::move(savedOsrLoopThresholds);
    osrRecompiledFlags_ = std::move(savedOsrRecompiledFlags);
    osrEntryGenMode_ = savedOsrEntryGenMode;
    osrTargetChunkIdx_ = savedOsrTargetChunkIdx;
    osrEntryLabelGenerated_ = savedOsrEntryLabelGenerated;
    osrEntryPointOut_ = savedOsrEntryPointOut;

    // R157 教训：恢复状态后重新同步 jitContext_ 中所有指向 data() 的字段
    jitContext_.chunkCallCounts = chunkCallCounts_.data();
    jitContext_.hotThresholds = hotThresholds_.data();
    jitContext_.recompiledFlags = recompiledFlags_.data();
    jitContext_.typeFeedback = typeFeedback_.data();
    jitContext_.osrLoopCountsPtr = osrLoopCounts_.data();
    jitContext_.osrLoopThresholdsPtr = osrLoopThresholds_.data();
    jitContext_.osrRecompiledFlagsPtr = osrRecompiledFlags_.data();
    jitContext_.backendPtr = this;

    if (!specializedMethodEntry) {
        runtime_.release(specializedEntry);
        return nullptr;
    }

    // 更新原 methodEntries_[targetChunkName].entryPtr 为特化版本入口
    auto origIt = methodEntries_.find(targetChunkName);
    if (origIt != methodEntries_.end()) {
        origIt->second.entryPtr = specializedMethodEntry;
    }

    // R153: 持久化特化入口映射
    specializedMethodEntries_[targetChunkName] = specializedMethodEntry;

    // 保存特化版本 CodeHolder 内存所有权
    registerSpecializedOwnership(targetChunkName, specializedEntry); // AUDIT-R4 BUG-12: 旧块退休+新块登记

    // 记录已特化的 chunk 名称
    specializedChunks_.push_back(targetChunkName);

    // 保存特化入口指针
    if (specializedEntries_.size() <= chunkIdx) {
        specializedEntries_.resize(chunkIdx + 1, nullptr);
    }
    specializedEntries_[chunkIdx] = specializedEntry;

    // OSR 入口点地址已由 compileAllChunks 写入 *osrEntryPointPtr
    return specializedEntry;
}

// ============================================================
// R158: compileChunkSpecializedFloatWithOsr — FLOAT 特化重编译（含 OSR 入口点）
// ============================================================
JitEntryFn JITBackend::compileChunkSpecializedFloatWithOsr(const CompileResult& result, size_t chunkIdx,
                                                           void** osrEntryPointPtr) {
    if (currentResult_ == nullptr || osrEntryPointPtr == nullptr) {
        return nullptr;
    }
    if (chunkIdx >= chunkNames_.size()) {
        return nullptr;
    }

    // 检查类型反馈：仅当 floatCount>0 && otherCount==0 时才 FLOAT 特化
    if (chunkIdx >= typeFeedback_.size()) {
        return nullptr;
    }
    const auto& tf = typeFeedback_[chunkIdx];
    if (tf.floatCount == 0 || tf.otherCount > 0) {
        return nullptr;
    }

    // 保存原状态（与 compileChunkSpecializedFloat 保持一致）
    auto savedMethodEntries = methodEntries_;
    JitEntryFn savedCurrentEntry = currentEntry_;
    auto savedRecompiledFlags = recompiledFlags_;
    auto savedChunkCallCounts = chunkCallCounts_;
    auto savedTypeFeedback = typeFeedback_;
    auto savedHotThresholds = hotThresholds_;
    auto savedChunkNames = chunkNames_;
    auto savedGlobalNameToSlot = globalNameToSlot_;
    auto savedClassInfo = classInfo_;
    auto savedPendingFieldOrder = pendingFieldOrder_;
    auto savedMethodLabels = std::move(specializedChunks_);
    auto savedFuncEntries = funcEntries_;
    auto savedSpecializedMethodEntries = specializedMethodEntries_;
    auto savedOsrLoopCounts = osrLoopCounts_;
    auto savedOsrLoopThresholds = osrLoopThresholds_;
    auto savedOsrRecompiledFlags = osrRecompiledFlags_;
    bool savedOsrEntryGenMode = osrEntryGenMode_;
    size_t savedOsrTargetChunkIdx = osrTargetChunkIdx_;
    bool savedOsrEntryLabelGenerated = osrEntryLabelGenerated_;
    void** savedOsrEntryPointOut = osrEntryPointOut_;

    // 设置 FLOAT 特化 + OSR 入口点生成模式
    specializeFloatMode_ = true;
    osrEntryGenMode_ = true;
    osrTargetChunkIdx_ = chunkIdx;
    osrEntryLabelGenerated_ = false;
    osrEntryPointOut_ = osrEntryPointPtr;
    *osrEntryPointPtr = nullptr;

    JitEntryFn specializedEntry = compileAllChunks(result);

    specializeFloatMode_ = false;
    osrEntryGenMode_ = false;

    if (!specializedEntry) {
        methodEntries_ = std::move(savedMethodEntries);
        currentEntry_ = savedCurrentEntry;
        recompiledFlags_ = std::move(savedRecompiledFlags);
        chunkCallCounts_ = std::move(savedChunkCallCounts);
        typeFeedback_ = std::move(savedTypeFeedback);
        hotThresholds_ = std::move(savedHotThresholds);
        chunkNames_ = std::move(savedChunkNames);
        globalNameToSlot_ = std::move(savedGlobalNameToSlot);
        classInfo_ = std::move(savedClassInfo);
        pendingFieldOrder_ = std::move(savedPendingFieldOrder);
        specializedChunks_ = std::move(savedMethodLabels);
        funcEntries_ = std::move(savedFuncEntries);
        specializedMethodEntries_ = std::move(savedSpecializedMethodEntries);
        osrLoopCounts_ = std::move(savedOsrLoopCounts);
        osrLoopThresholds_ = std::move(savedOsrLoopThresholds);
        osrRecompiledFlags_ = std::move(savedOsrRecompiledFlags);
        osrEntryGenMode_ = savedOsrEntryGenMode;
        osrTargetChunkIdx_ = savedOsrTargetChunkIdx;
        osrEntryLabelGenerated_ = savedOsrEntryLabelGenerated;
        osrEntryPointOut_ = savedOsrEntryPointOut;
        jitContext_.chunkCallCounts = chunkCallCounts_.data();
        jitContext_.hotThresholds = hotThresholds_.data();
        jitContext_.recompiledFlags = recompiledFlags_.data();
        jitContext_.typeFeedback = typeFeedback_.data();
        jitContext_.osrLoopCountsPtr = osrLoopCounts_.data();
        jitContext_.osrLoopThresholdsPtr = osrLoopThresholds_.data();
        jitContext_.osrRecompiledFlagsPtr = osrRecompiledFlags_.data();
        jitContext_.backendPtr = this;
        return nullptr;
    }

    const std::string targetChunkName = (chunkIdx < chunkNames_.size()) ? chunkNames_[chunkIdx] : std::string{};
    auto it = methodEntries_.find(targetChunkName);
    void* specializedMethodEntry = nullptr;
    if (it != methodEntries_.end()) {
        specializedMethodEntry = it->second.entryPtr;
    }

    methodEntries_ = std::move(savedMethodEntries);
    currentEntry_ = savedCurrentEntry;
    recompiledFlags_ = std::move(savedRecompiledFlags);
    chunkCallCounts_ = std::move(savedChunkCallCounts);
    typeFeedback_ = std::move(savedTypeFeedback);
    hotThresholds_ = std::move(savedHotThresholds);
    chunkNames_ = std::move(savedChunkNames);
    globalNameToSlot_ = std::move(savedGlobalNameToSlot);
    classInfo_ = std::move(savedClassInfo);
    pendingFieldOrder_ = std::move(savedPendingFieldOrder);
    specializedChunks_ = std::move(savedMethodLabels);
    funcEntries_ = std::move(savedFuncEntries);
    specializedMethodEntries_ = std::move(savedSpecializedMethodEntries);
    osrLoopCounts_ = std::move(savedOsrLoopCounts);
    osrLoopThresholds_ = std::move(savedOsrLoopThresholds);
    osrRecompiledFlags_ = std::move(savedOsrRecompiledFlags);
    osrEntryGenMode_ = savedOsrEntryGenMode;
    osrTargetChunkIdx_ = savedOsrTargetChunkIdx;
    osrEntryLabelGenerated_ = savedOsrEntryLabelGenerated;
    osrEntryPointOut_ = savedOsrEntryPointOut;

    jitContext_.chunkCallCounts = chunkCallCounts_.data();
    jitContext_.hotThresholds = hotThresholds_.data();
    jitContext_.recompiledFlags = recompiledFlags_.data();
    jitContext_.typeFeedback = typeFeedback_.data();
    jitContext_.osrLoopCountsPtr = osrLoopCounts_.data();
    jitContext_.osrLoopThresholdsPtr = osrLoopThresholds_.data();
    jitContext_.osrRecompiledFlagsPtr = osrRecompiledFlags_.data();
    jitContext_.backendPtr = this;

    if (!specializedMethodEntry) {
        runtime_.release(specializedEntry);
        return nullptr;
    }

    auto origIt = methodEntries_.find(targetChunkName);
    if (origIt != methodEntries_.end()) {
        origIt->second.entryPtr = specializedMethodEntry;
    }
    specializedMethodEntries_[targetChunkName] = specializedMethodEntry;
    registerSpecializedOwnership(targetChunkName, specializedEntry); // AUDIT-R4 BUG-12: 旧块退休+新块登记
    specializedChunks_.push_back(targetChunkName);
    if (specializedEntries_.size() <= chunkIdx) {
        specializedEntries_.resize(chunkIdx + 1, nullptr);
    }
    specializedEntries_[chunkIdx] = specializedEntry;

    return specializedEntry;
}

// ============================================================
// R158: triggerOsrMigration — 真正 OSR 栈帧迁移触发器
// ------------------------------------------------------------
// 当 OP_LOOP 回边计数超阈值且 osrMigrationMode_=true 时，由 JIT 代码通过
// jitTriggerOsrMigration 回调。执行流程：
//   1. JIT 代码保存 r13/r15 到邮箱（osrSavedBp/osrSavedSp）
//   2. 调用 triggerOsrMigration(chunkIdx)
//   3. 本函数根据类型反馈调用 compileChunkSpecializedWithOsr/Float
//   4. 特化版本生成 OSR 入口 Label（恢复 r13/r15 + jmp 循环回边）
//   5. OSR 入口点地址通过 ctx->osrEntryPoint 邮箱返回
//   6. JIT 代码 jmp osrEntryPoint（真正 OSR 栈帧迁移）
//   7. OSR 入口点恢复 r13/r15，跳转到特化版本的循环回边继续执行
//
// 与 R157 triggerOsrRecompile 的区别：
//   - R157: 仅触发特化重编译，下次 execute 生效（非真正 OSR）
//   - R158: 生成 OSR 入口点，当前 execute 立即迁移到特化版本（真正 OSR）
// ============================================================
int64_t JITBackend::triggerOsrMigration(int64_t chunkIdx) {
    if (!currentResult_ || chunkIdx < 0 || static_cast<size_t>(chunkIdx) >= osrRecompiledFlags_.size()) {
        return -1;
    }

    // 注意：JIT 代码在调用本函数前已通过 step 2 (jnz skipOsr) 检查 osrRecompiledFlags==0，
    //       并在 step 4 设置 osrRecompiledFlags=1。因此此处不再重复检查，
    //       否则会因 flag 已被 JIT 代码置 1 而提前返回 0（未实际迁移）。

    // 读取 OSR 栈帧快照（JIT 代码已通过邮箱写入 osrSavedBp/osrSavedSp）
    osrFrameSnapshot_.frameBp = jitContext_.osrSavedBp;
    osrFrameSnapshot_.frameSp = jitContext_.osrSavedSp;

    // 根据类型反馈决定特化策略
    const auto& tf = typeFeedback_[static_cast<size_t>(chunkIdx)];
    void* osrEntryPoint = nullptr;
    JitEntryFn specializedEntry = nullptr;

    if (tf.otherCount == 0 && tf.floatCount == 0) {
        // INT 特化 + OSR 入口点
        specializedEntry =
            compileChunkSpecializedWithOsr(*currentResult_, static_cast<size_t>(chunkIdx), &osrEntryPoint);
    } else if (tf.floatCount > 0 && tf.otherCount == 0) {
        // FLOAT 特化 + OSR 入口点
        specializedEntry =
            compileChunkSpecializedFloatWithOsr(*currentResult_, static_cast<size_t>(chunkIdx), &osrEntryPoint);
    } else {
        // 不特化
        return -1;
    }

    if (specializedEntry == nullptr || osrEntryPoint == nullptr) {
        // 特化或 OSR 入口点生成失败
        return -1;
    }

    // 设置 osrEntryPoint 邮箱（JIT 代码将 jmp 此地址）
    jitContext_.osrEntryPoint = osrEntryPoint;

    // 更新 chunk Tier 为 Specialized
    if (static_cast<size_t>(chunkIdx) < chunkTiers_.size()) {
        chunkTiers_[static_cast<size_t>(chunkIdx)] = minilang::JitTier::Specialized;
    }

    // 记录已 OSR 迁移的 chunk 名称
    const std::string chunkName = (static_cast<size_t>(chunkIdx) < chunkNames_.size())
                                      ? chunkNames_[static_cast<size_t>(chunkIdx)]
                                      : std::string{};
    if (!chunkName.empty()) {
        osrMigratedChunks_.push_back(chunkName);
    }

    // R157 fix: 重新设置 osrRecompiledFlags_（compileAllChunks 可能重置）
    if (static_cast<size_t>(chunkIdx) < osrRecompiledFlags_.size()) {
        osrRecompiledFlags_[static_cast<size_t>(chunkIdx)] = 1;
    }

    return 0; // 成功
}

// ============================================================
// R158: triggerDeoptimize — 反优化触发器
// ------------------------------------------------------------
// 将指定 chunk 从 Tier 2 (Specialized) 回退到 Tier 1 (Baseline)。
// 恢复 methodEntries_/funcEntries_ 中的 entryPtr 为 baseline 版本入口。
// R159: 即时反优化 — 特化版本的 emitCheckInt 类型守卫失败时通过 jitDeoptimize
//   回调触发此函数，在当前 execute() 执行期间即时降级。JIT 代码调用后跳转 generic
//   路径正确处理当前操作，后续调用自动走 baseline 版本（methodEntries_ 已更新）。
//
// 与 V8 的区别：
//   - V8: 即时反优化，重构解释器帧从 deopt 点继续执行
//   - R159: 即时降级 + generic 路径兜底，当前操作走 C++ 辅助路径，下次调用走 baseline
// ============================================================
int64_t JITBackend::triggerDeoptimize(int64_t chunkIdx) {
    if (chunkIdx < 0 || static_cast<size_t>(chunkIdx) >= chunkTiers_.size()) {
        return -1;
    }

    // 检查当前 Tier 是否为 Specialized（只有特化版本才能反优化）
    if (chunkTiers_[static_cast<size_t>(chunkIdx)] != minilang::JitTier::Specialized) {
        return -1; // 未特化，无需反优化
    }

    // 获取 chunk 名称
    const std::string chunkName = (static_cast<size_t>(chunkIdx) < chunkNames_.size())
                                      ? chunkNames_[static_cast<size_t>(chunkIdx)]
                                      : std::string{};
    if (chunkName.empty()) {
        return -1;
    }

    // 从 baseline 备份查找入口
    void* baselineEntry = nullptr;
    auto methodIt = baselineMethodEntries_.find(chunkName);
    if (methodIt != baselineMethodEntries_.end()) {
        baselineEntry = methodIt->second;
    } else {
        auto funcIt = baselineFuncEntries_.find(chunkName);
        if (funcIt != baselineFuncEntries_.end()) {
            baselineEntry = funcIt->second;
        }
    }

    if (baselineEntry == nullptr) {
        return -1; // 未找到 baseline 入口
    }

    // 设置 deoptEntryPoint 邮箱（如果 JIT 代码需要即时跳转）
    jitContext_.deoptEntryPoint = baselineEntry;
    jitContext_.deoptChunkIdx = chunkIdx;

    // 恢复 methodEntries_/funcEntries_ 的 entryPtr 为 baseline 版本
    auto origMethodIt = methodEntries_.find(chunkName);
    if (origMethodIt != methodEntries_.end()) {
        origMethodIt->second.entryPtr = baselineEntry;
    } else {
        auto origFuncIt = funcEntries_.find(chunkName);
        if (origFuncIt != funcEntries_.end()) {
            origFuncIt->second.entryPtr = baselineEntry;
        }
    }

    // 从 specializedMethodEntries_ 中移除（不再使用特化版本）
    specializedMethodEntries_.erase(chunkName);

    // 更新 chunk Tier 为 Baseline
    chunkTiers_[static_cast<size_t>(chunkIdx)] = minilang::JitTier::Baseline;

    // 递增反优化计数
    ++deoptCount_;

    // 记录已反优化的 chunk 名称
    deoptimizedChunks_.push_back(chunkName);

    return 0; // 成功
}

// ============================================================
// R158: getChunkTiers — 获取 per-chunk 当前 Tier（分层编译状态）
// ============================================================
std::vector<std::pair<std::string, minilang::JitTier>> JITBackend::getChunkTiers() const {
    std::vector<std::pair<std::string, minilang::JitTier>> result;
    for (size_t i = 0; i < chunkNames_.size() && i < chunkTiers_.size(); ++i) {
        result.emplace_back(chunkNames_[i], chunkTiers_[i]);
    }
    return result;
}

// ============================================================
// execute: 编译 + 执行
// ============================================================

#endif // MINILANG_USE_JIT
