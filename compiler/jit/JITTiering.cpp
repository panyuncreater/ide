/**
 * @file compiler/jit/JITTiering.cpp
 * @brief JIT tiered compilation, OSR, and deoptimization (extracted from JIT.cpp).
 * @see JIT.h JITInternal.h
 * @since P3 (JIT.cpp split)
 */

#include "compiler/jit/JIT.h"

#ifdef MINILANG_USE_JIT

#include "common/RuntimeLimits.h"
#include "compiler/jit/JITInternal.h"
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
// ADR-008 阶段 1b: compileSingleChunkBlock —— 真单 chunk 编译
// ------------------------------------------------------------
// 仅发射目标 chunk 的独立代码块（块首入口 Label + 块体 + 错误退出桩；无
// prologue/epilogue/闭包跳板——它作为"被调块"经既有 mailbox/导入槽协议进入，
// r12/r13/r15 与共享操作数栈由调用方建立，与 lazy 模式既有跨块事实一致）。
// 与全量编译（compileAllChunks）的关键差异：
//   ① 不触碰 per-chunk 统计数组 / chunkTiers_ / classNameSet_ 等全局状态；
//   ② 不覆写 ctx->errorExit / closureTrampoline_（无 main 块，继承外层值）；
//   ③ IC callSiteId 追加编号（从既有容量起计，老块槽位永不失效）；
//   ④ 跨块 fast path 仅在被调 chunk 已有 entryPtr 时走（singleChunkCompileMode_
//      门控，见 emitCallDispatch；未编译被调回落 mailbox 运行期 lazy 触发）；
//   ⑤ 导入槽表跨代合并追加（patchBegin 起回填，见 ADR-008 阶段 1a）。
// 所有权：返回块基址（非 main 块块首即入口，entryPtr == 块基址），调用方恰好
// 登记一次（ownedLazyEntries_ 或 registerSpecializedOwnership）。
// ============================================================
JitEntryFn JITBackend::compileSingleChunkBlock(const CompileResult& result, const std::string& chunkName) {
    using namespace asmjit;

    auto chunkIt = result.functionChunks.find(chunkName);
    if (chunkIt == result.functionChunks.end()) {
        return nullptr;
    }
    const BytecodeChunk& chunk = chunkIt->second;

    // 全局 chunkIdx（per-chunk 统计数组寻址用，与全量编译索引严格一致）
    size_t chunkIdx = static_cast<size_t>(-1);
    for (size_t i = 1; i < chunkNames_.size(); ++i) {
        if (chunkNames_[i] == chunkName) {
            chunkIdx = i;
            break;
        }
    }
    if (chunkIdx == static_cast<size_t>(-1)) {
        return nullptr;
    }

    // funcTable 元数据（chunkId 按全局索引，供同块递归直连判定）
    std::unordered_map<std::string, JitFuncInfo> funcTable;
    for (const auto& [name, fc] : result.functionChunks) {
        JitFuncInfo info;
        info.localCount = fc.localCount;
        info.arity = fc.arity;
        info.requiredArity = fc.requiredArity;
        info.chunk = &fc;
        const auto& chunkCode = fc.code;
        for (size_t i = 0; i < chunkCode.size();) {
            if (static_cast<OpCode>(chunkCode[i]) == OpCode::OP_CLOSURE) {
                info.hasInnerClosures = true;
                break;
            }
            i += fc.instructionSizeAt(i);
        }
        funcTable.emplace(name, std::move(info));
    }
    for (size_t i = 1; i < chunkNames_.size(); ++i) {
        auto fit = funcTable.find(chunkNames_[i]);
        if (fit != funcTable.end()) {
            fit->second.chunkId = i;
        }
    }

    const size_t patchBegin = callImportPatches_.size(); // 本次编译代 patch 区段起点
    const size_t memberIcBegin = memberGetIC_.size();    // memberGet IC 追加编号起点
    const uint64_t methodIcBegin = nextMethodCallSiteId_;

    CodeHolder code;
    Error err = code.init(runtime_.environment());
    if (err != kErrorOk) {
        compileError(std::string("asmjit CodeHolder::init 失败: ") + DebugUtils::error_as_string(err));
        return nullptr;
    }
    x86::Assembler a(&code);
    Label entryLabel = a.new_label();
    Label epilogueLabel = a.new_label();
    currentBlockEntryLabel_ = entryLabel;

    // IC 追加编号；跨块 fast path 按被调已编译与否分派（lazyMode_ 局部置 false）
    nextCallSiteId_ = memberIcBegin;
    nextMethodCallSiteId_ = methodIcBegin;
    singleChunkCompileMode_ = true;
    const bool savedLazyMode = lazyMode_;
    lazyMode_ = false;
    const bool ok = emitChunkBody(a, chunk, chunkIdx, epilogueLabel, funcTable);
    lazyMode_ = savedLazyMode;
    singleChunkCompileMode_ = false;
    if (!ok) {
        return nullptr;
    }

    // 错误退出桩（非 main 块，见 ADR-008 阶段 1a）
    a.bind(epilogueLabel);
    a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::errorExit));
    a.jmp(x86::rax);

    JitEntryFn blockEntry = nullptr;
    Error addErr = runtime_.add(&blockEntry, &code);
    if (addErr != kErrorOk) {
        compileError(std::string("asmjit JitRuntime::add 失败: ") + DebugUtils::error_as_string(addErr));
        return nullptr;
    }

    // 仅更新目标 chunk 的映射条目（不 clear/重建映射——外层其他块条目原样保留）
    const uintptr_t entryAddr = reinterpret_cast<uintptr_t>(blockEntry) + code.label_offset_from_base(entryLabel);
    if (chunkName.find('.') != std::string::npos) {
        auto it = methodEntries_.find(chunkName);
        if (it != methodEntries_.end()) {
            it->second.entryPtr = reinterpret_cast<void*>(entryAddr);
        }
    } else {
        auto it = funcEntries_.find(chunkName);
        if (it != funcEntries_.end()) {
            it->second.entryPtr = reinterpret_cast<void*>(entryAddr);
        }
    }

    // memberGetIC_ 扩容到追加后的总量；重同步 ctx 指针（resize 可能 realloc）
    if (memberGetIC_.size() < nextCallSiteId_) {
        memberGetIC_.resize(nextCallSiteId_);
    }
    jitContext_.memberGetICPtr = memberGetIC_.data();

    // 回填本次编译代的跨块导入槽（合并表追加区段，见 ADR-008 阶段 1a）
    for (size_t p = patchBegin; p < callImportPatches_.size(); ++p) {
        const CallImportPatch& patchInfo = callImportPatches_[p];
        void* target = nullptr;
        if (patchInfo.isMethod) {
            auto it = methodEntries_.find(patchInfo.calleeName);
            if (it != methodEntries_.end()) {
                target = it->second.entryPtr;
            }
        } else {
            auto it = funcEntries_.find(patchInfo.calleeName);
            if (it != funcEntries_.end()) {
                target = it->second.entryPtr;
            }
        }
        if (target == nullptr) {
            compileError("JIT 导入槽回填失败（被调 chunk 入口缺失）: " + patchInfo.calleeName);
            runtime_.release(blockEntry);
            return nullptr;
        }
        callImportSlots_[patchInfo.slotIdx] = target;
    }
    jitContext_.callImportSlots = callImportSlots_.data();

    // OSR 入口点（仅 osrEntryGenMode_ 且 Label 已在本块生成；add 后偏移有效）
    if (osrEntryGenMode_ && osrEntryLabelGenerated_ && osrEntryPointOut_ != nullptr) {
        *osrEntryPointOut_ = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(blockEntry) +
                                                     code.label_offset_from_base(osrEntryLabel_));
    }

    return blockEntry;
}

// ============================================================
// R152: compileChunkSpecialized — INT 特化重编译单个 chunk（ADR-008 阶段 1b）
// ============================================================
// 在 execute() 返回后对 recompiledFlags 标记的 chunk 调用。
// 类型反馈 otherCount==0 && floatCount==0 时以 specializeIntMode_ 门控重编译
// 目标 chunk（emitCheckInt 跳过类型检查）。真单 chunk 编译：仅目标块重编译，
// 不再全量重编译、不再 save/restore 外层状态（单 chunk 路径不触碰全局状态）。
// 所有权经 registerSpecializedOwnership 登记退休链（重复特化时退休旧块）。
JitEntryFn JITBackend::compileChunkSpecialized(const CompileResult& result, size_t chunkIdx) {
    if (currentResult_ == nullptr || chunkIdx >= chunkNames_.size() || chunkIdx >= typeFeedback_.size()) {
        return nullptr;
    }
    const auto& tf = typeFeedback_[chunkIdx];
    if (tf.otherCount > 0 || tf.floatCount > 0) {
        return nullptr; // 类型反馈显示非纯 INT，不特化（保持原版本）
    }
    const std::string targetChunkName = chunkNames_[chunkIdx];
    if (targetChunkName.find('.') == std::string::npos) {
        return nullptr; // 仅方法 chunk 参与特化入口切换（与既有行为一致）
    }

    specializeIntMode_ = true;
    JitEntryFn targetBlock = compileSingleChunkBlock(result, targetChunkName);
    specializeIntMode_ = false;
    if (targetBlock == nullptr) {
        return nullptr;
    }

    // R153: 持久化特化入口映射，下次 execute() 重新应用
    specializedMethodEntries_[targetChunkName] =
        reinterpret_cast<void*>(targetBlock); // 函数指针→void*：GCC 需显式转换（MSVC 静默放行）
    // 所有权登记（AUDIT-R4 BUG-12: 旧块退休+新块登记，恰好一次）
    registerSpecializedOwnership(targetChunkName, targetBlock);
    // 记录已特化的 chunk 名称
    specializedChunks_.push_back(targetChunkName);
    // 保存特化入口指针（按 chunkIdx 索引，测试验证用）
    if (specializedEntries_.size() <= chunkIdx) {
        specializedEntries_.resize(chunkIdx + 1, nullptr);
    }
    specializedEntries_[chunkIdx] = targetBlock;
    return targetBlock;
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
// R153: compileChunkSpecializedFloat — FLOAT 特化重编译单个 chunk（ADR-008 阶段 1b）
// ============================================================
// 类型反馈 floatCount>0 && otherCount==0 时以 specializeFloatMode_ 门控重编译
// 目标 chunk（emitCheckInt 无条件跳过 INT 原生路径 + 不收集类型反馈）。
// 其余与 compileChunkSpecialized 一致（真单 chunk 编译 + 所有权退休链）。
JitEntryFn JITBackend::compileChunkSpecializedFloat(const CompileResult& result, size_t chunkIdx) {
    if (currentResult_ == nullptr || chunkIdx >= chunkNames_.size() || chunkIdx >= typeFeedback_.size()) {
        return nullptr;
    }
    const auto& tf = typeFeedback_[chunkIdx];
    if (tf.floatCount == 0 || tf.otherCount > 0) {
        return nullptr; // 类型反馈显示非纯 FLOAT，不特化（保持原版本）
    }
    const std::string targetChunkName = chunkNames_[chunkIdx];
    if (targetChunkName.find('.') == std::string::npos) {
        return nullptr; // 仅方法 chunk 参与特化入口切换（与既有行为一致）
    }

    specializeFloatMode_ = true;
    JitEntryFn targetBlock = compileSingleChunkBlock(result, targetChunkName);
    specializeFloatMode_ = false;
    if (targetBlock == nullptr) {
        return nullptr;
    }

    specializedMethodEntries_[targetChunkName] =
        reinterpret_cast<void*>(targetBlock); // 函数指针→void*：GCC 需显式转换（MSVC 静默放行）
    registerSpecializedOwnership(targetChunkName, targetBlock);
    specializedChunks_.push_back(targetChunkName);
    if (specializedEntries_.size() <= chunkIdx) {
        specializedEntries_.resize(chunkIdx + 1, nullptr);
    }
    specializedEntries_[chunkIdx] = targetBlock;
    return targetBlock;
}

// ============================================================
// R157: compileSingleChunkLazy — 按需编译单个 chunk（ADR-008 阶段 1b 真单 chunk）
// ------------------------------------------------------------
// 在 lazyMode_ 为 true 且函数/方法首次调用时，由 jitCallByName 通过
// backendPtr 触发。仅编译目标 chunk 的独立代码块并更新其映射条目，
// 其余 chunk 状态零触碰（原实现重编译全部 chunk + 14 向量 save/restore）。
// 所有权移交 ownedLazyEntries_，存活至下次 execute() 入口安全点释放。
// ============================================================
JitEntryFn JITBackend::compileSingleChunkLazy(const CompileResult& result, const std::string& chunkName) {
    if (!currentResult_) {
        return nullptr;
    }

    JitEntryFn targetBlock = compileSingleChunkBlock(result, chunkName);
    if (targetBlock == nullptr) {
        return nullptr;
    }

    // lazy 编译块移交 ownedLazyEntries_（下次 execute() 入口释放）
    ownedLazyEntries_.push_back(targetBlock);

    // R159: Tier 0→Tier 1 自动升级 — lazy compile 成功后更新 chunkTiers_
    if (tieredMode_) {
        for (size_t i = 0; i < chunkNames_.size() && i < chunkTiers_.size(); ++i) {
            if (chunkNames_[i] == chunkName && chunkTiers_[i] == minilang::JitTier::Interpreter) {
                chunkTiers_[i] = minilang::JitTier::Baseline;
                break;
            }
        }
    }

    // baseline 入口备份（反优化回退目标；等价原全量备份对该 chunk 的语义，
    // 且不再被内层全量编译连带覆写其他 chunk 的备份）
    if (chunkName.find('.') != std::string::npos) {
        baselineMethodEntries_[chunkName] = reinterpret_cast<void*>(targetBlock);
    } else {
        baselineFuncEntries_[chunkName] = reinterpret_cast<void*>(targetBlock);
    }

    // 记录已 lazy 编译的 chunk 名称（去重，教学统计用）
    if (std::find(lazyCompiledChunksList_.begin(), lazyCompiledChunksList_.end(), chunkName) ==
        lazyCompiledChunksList_.end()) {
        lazyCompiledChunksList_.push_back(chunkName);
    }

    return targetBlock;
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

    // R157 fix: 特化重编译不再重置 osrRecompiledFlags_（ADR-008 阶段 1b
    // 真单 chunk 编译不触碰统计数组）；此处保留幂等置 1 防御。
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
// R158: compileChunkSpecializedWithOsr — INT 特化重编译（含 OSR 入口点，阶段 1b）
// ------------------------------------------------------------
// 与 R152 类似但启用 OSR 入口点生成模式：目标块在其第一个 OP_LOOP 位置生成
// OSR 入口 Label（恢复 r13/r15 + 重载缓存常量 + jmp 循环回边），入口地址经
// osrEntryPointOut_ 邮箱返回，实现真正 OSR 栈帧迁移。
// ============================================================
JitEntryFn JITBackend::compileChunkSpecializedWithOsr(const CompileResult& result, size_t chunkIdx,
                                                      void** osrEntryPointPtr) {
    if (currentResult_ == nullptr || osrEntryPointPtr == nullptr || chunkIdx >= chunkNames_.size() ||
        chunkIdx >= typeFeedback_.size()) {
        return nullptr;
    }
    const auto& tf = typeFeedback_[chunkIdx];
    if (tf.otherCount > 0 || tf.floatCount > 0) {
        return nullptr;
    }
    const std::string targetChunkName = chunkNames_[chunkIdx];
    if (targetChunkName.find('.') == std::string::npos) {
        return nullptr;
    }

    // 仅 OSR 模式状态需暂存/恢复（单 chunk 路径不触碰其余全局状态）
    bool savedOsrEntryGenMode = osrEntryGenMode_;
    size_t savedOsrTargetChunkIdx = osrTargetChunkIdx_;
    bool savedOsrEntryLabelGenerated = osrEntryLabelGenerated_;
    void** savedOsrEntryPointOut = osrEntryPointOut_;

    specializeIntMode_ = true;
    osrEntryGenMode_ = true;
    osrTargetChunkIdx_ = chunkIdx;
    osrEntryLabelGenerated_ = false;
    osrEntryPointOut_ = osrEntryPointPtr;
    *osrEntryPointPtr = nullptr; // 初始化为 null

    JitEntryFn targetBlock = compileSingleChunkBlock(result, targetChunkName);

    specializeIntMode_ = false;
    osrEntryGenMode_ = savedOsrEntryGenMode;
    osrTargetChunkIdx_ = savedOsrTargetChunkIdx;
    osrEntryPointOut_ = savedOsrEntryPointOut;

    if (targetBlock == nullptr) {
        osrEntryLabelGenerated_ = savedOsrEntryLabelGenerated;
        return nullptr;
    }
    // osrEntryLabelGenerated_ 保持 true（仅 osrEntryGenMode_ 会消费该标志，
    // 下次 OSR 编译前由触发方显式清零）

    specializedMethodEntries_[targetChunkName] =
        reinterpret_cast<void*>(targetBlock); // 函数指针→void*：GCC 需显式转换（MSVC 静默放行）
    registerSpecializedOwnership(targetChunkName, targetBlock);
    specializedChunks_.push_back(targetChunkName);
    if (specializedEntries_.size() <= chunkIdx) {
        specializedEntries_.resize(chunkIdx + 1, nullptr);
    }
    specializedEntries_[chunkIdx] = targetBlock;

    // OSR 入口点地址已由 compileSingleChunkBlock 写入 *osrEntryPointPtr
    return targetBlock;
}

// ============================================================
// R158: compileChunkSpecializedFloatWithOsr — FLOAT 特化重编译（含 OSR 入口点，
//       ADR-008 阶段 1b 真单 chunk 编译）
// ============================================================
JitEntryFn JITBackend::compileChunkSpecializedFloatWithOsr(const CompileResult& result, size_t chunkIdx,
                                                           void** osrEntryPointPtr) {
    if (currentResult_ == nullptr || osrEntryPointPtr == nullptr || chunkIdx >= chunkNames_.size() ||
        chunkIdx >= typeFeedback_.size()) {
        return nullptr;
    }
    const auto& tf = typeFeedback_[chunkIdx];
    if (tf.floatCount == 0 || tf.otherCount > 0) {
        return nullptr;
    }
    const std::string targetChunkName = chunkNames_[chunkIdx];
    if (targetChunkName.find('.') == std::string::npos) {
        return nullptr;
    }

    bool savedOsrEntryGenMode = osrEntryGenMode_;
    size_t savedOsrTargetChunkIdx = osrTargetChunkIdx_;
    bool savedOsrEntryLabelGenerated = osrEntryLabelGenerated_;
    void** savedOsrEntryPointOut = osrEntryPointOut_;

    specializeFloatMode_ = true;
    osrEntryGenMode_ = true;
    osrTargetChunkIdx_ = chunkIdx;
    osrEntryLabelGenerated_ = false;
    osrEntryPointOut_ = osrEntryPointPtr;
    *osrEntryPointPtr = nullptr;

    JitEntryFn targetBlock = compileSingleChunkBlock(result, targetChunkName);

    specializeFloatMode_ = false;
    osrEntryGenMode_ = savedOsrEntryGenMode;
    osrTargetChunkIdx_ = savedOsrTargetChunkIdx;
    osrEntryPointOut_ = savedOsrEntryPointOut;

    if (targetBlock == nullptr) {
        osrEntryLabelGenerated_ = savedOsrEntryLabelGenerated;
        return nullptr;
    }

    specializedMethodEntries_[targetChunkName] =
        reinterpret_cast<void*>(targetBlock); // 函数指针→void*：GCC 需显式转换（MSVC 静默放行）
    registerSpecializedOwnership(targetChunkName, targetBlock);
    specializedChunks_.push_back(targetChunkName);
    if (specializedEntries_.size() <= chunkIdx) {
        specializedEntries_.resize(chunkIdx + 1, nullptr);
    }
    specializedEntries_[chunkIdx] = targetBlock;

    // OSR 入口点地址已由 compileSingleChunkBlock 写入 *osrEntryPointPtr
    return targetBlock;
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
