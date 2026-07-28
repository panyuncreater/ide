/**
 * @file compiler/JITClosure.cpp
 * @brief JIT 后端 — 闭包/upvalue 管理（拆分自 JIT.cpp）。
 *
 * 包含闭包创建、upvalue 读写、upvalue 关闭等运行时辅助函数，
 * 以及 JITBackend::closeUpvaluesFrom 方法。
 *
 * @see JIT.h JITInternal.h
 * @since P3（JIT.cpp 拆分）
 */

#include "compiler/JIT.h"

#ifdef MINILANG_USE_JIT

#include "compiler/JITInternal.h"
#include "interpreter/Value.h"
#include <cstdint>
#include <memory>
#include <string>

using namespace jit_internal;

// ============================================================
// extern "C" 闭包/upvalue 运行时辅助函数
// ============================================================

extern "C" {

/// R156: OP_CLOSURE — 创建闭包值
/// 与 StackVM executeClosure（VM.cpp:1035-1088）对齐
int64_t jitCreateClosure(JitContext* ctx, const char* funName, const void* chunkPtr, uint8_t upvalueCount,
                                    const uint8_t* upvalueDescs) {
    if (!ctx || !funName) {
        return static_cast<int64_t>(JIT_NULL_BITS);
    }

    auto* backend = static_cast<JITBackend*>(ctx->backendPtr);
    if (!backend) {
        return static_cast<int64_t>(JIT_NULL_BITS);
    }

    // 创建闭包值（env=nullptr, params=空，与 StackVM makeClosure(funName, nullptr, {}) 一致）
    Value closure = Value::makeClosure(funName, nullptr, {});

    // 创建 VMClosureData 并绑定 chunkPtr（M3 fix: OP_CALL_EXPR 优先使用 chunkPtr 查找）
    auto vmClosureData = std::make_shared<VMClosureData>();
    vmClosureData->functionName = funName;
    vmClosureData->chunkPtr = static_cast<const BytecodeChunk*>(chunkPtr);
    vmClosureData->upvalues.resize(upvalueCount);

    // 当前帧索引（与 StackVM frames_.size()-1 对齐）
    size_t currentFrameIdx = *ctx->frameCount > 0 ? *ctx->frameCount - 1 : 0;
    // 当前帧 basePointer（r13 同步到 ctx->currentBp）
    int64_t* frameBasePtr = ctx->currentBp;

    // 遍历 upvalue 描述符（与 StackVM executeClosure 对齐）
    for (uint8_t i = 0; i < upvalueCount; ++i) {
        uint8_t isLocal = upvalueDescs[i * 2];
        uint8_t uvIndex = upvalueDescs[i * 2 + 1];

        if (isLocal) {
            // 直接捕获：创建新 upvalue 指向当前帧的栈槽
            // JIT 栈布局：slot N 在 [frameBasePtr - N]（int64_t* 指针算术自动 *sizeof(int64_t)=8）
            // 注意：frameBasePtr 是 int64_t*，指针算术 frameBasePtr - N 已减去 N*8 字节，
            //       不能再手动 *8（否则减去 N*64 字节，多乘一次 sizeof）
            int64_t* slotAddr = frameBasePtr - static_cast<int64_t>(uvIndex);
            auto uv = std::make_shared<VMUpvalue>();
            uv->stackSlot = reinterpret_cast<size_t>(slotAddr);
            uv->isClosed = false;
            uv->owningFrameIdx = currentFrameIdx;
            vmClosureData->upvalues[i] = uv;
            backend->openUpvalues_.emplace(slotAddr, uv);
        } else {
            // 透传：复用当前帧的 upvalue（与 StackVM frame.upvalues[uvIndex] 对齐）
            if (currentFrameIdx < backend->frameUpvaluesStack_.size() &&
                uvIndex < backend->frameUpvaluesStack_[currentFrameIdx].size()) {
                vmClosureData->upvalues[i] = backend->frameUpvaluesStack_[currentFrameIdx][uvIndex];
            } else {
                // 降级：创建空 upvalue（与 StackVM 降级路径对齐）
                auto uv = std::make_shared<VMUpvalue>();
                uv->value = Value::nullValue();
                uv->isClosed = true;
                vmClosureData->upvalues[i] = uv;
            }
        }
    }
    closure.vmClosure() = vmClosureData;

    // R156: 注册到 functionClosures_（与 StackVM executeClosure 第 1076 行对齐）
    // OP_CALL 按名调用时通过此表查找闭包值，提取 upvalues 传递给新帧
    backend->functionClosures_[funName] = closure; // 拷贝（addRef）

    // detach 转移所有权到 JIT 栈（调用者负责 push）
    return valueToBits(closure);
}

/// R156: OP_GET_UPVALUE — 读取 upvalue 值
/// 与 StackVM OP_GET_UPVALUE 对齐（VM.cpp:2015-2032）
int64_t jitGetUpvalue(JitContext* ctx, uint8_t uvIdx) {
    if (!ctx || !ctx->backendPtr || !ctx->frameCount) {
        return static_cast<int64_t>(JIT_NULL_BITS);
    }

    auto* backend = static_cast<JITBackend*>(ctx->backendPtr);
    size_t currentFrameIdx = *ctx->frameCount > 0 ? *ctx->frameCount - 1 : 0;

    if (currentFrameIdx >= backend->frameUpvaluesStack_.size() ||
        uvIdx >= backend->frameUpvaluesStack_[currentFrameIdx].size()) {
        if (ctx->hasError && ctx->errorBuffer) {
            *ctx->hasError = true;
            *ctx->errorBuffer = "内部错误: upvalue 索引越界 (" + std::to_string(static_cast<int>(uvIdx)) + ")";
        }
        return static_cast<int64_t>(JIT_NULL_BITS);
    }

    auto& uv = backend->frameUpvaluesStack_[currentFrameIdx][uvIdx];
    if (uv->isClosed) {
        // R156 fix: 使用 copy-and-detach 模式，保持 uv->value 不变。
        Value tmp = uv->value;   // 拷贝构造（堆类型 addRef）
        return valueToBits(tmp); // detach tmp，uv->value 不变
    } else {
        // open 状态：从 JIT 栈槽读取当前值
        auto* slotPtr = reinterpret_cast<int64_t*>(uv->stackSlot);
        return *slotPtr; // 返回原始 bits（已经是 NaN-boxing 编码）
    }
}

/// R156: OP_SET_UPVALUE — 写入 upvalue 值（peek 不消费，与 OP_SET_LOCAL 一致）
/// 与 StackVM OP_SET_UPVALUE 对齐（VM.cpp:2035-2067）
void jitSetUpvalue(JitContext* ctx, uint8_t uvIdx, int64_t valueBits) {
    if (!ctx || !ctx->backendPtr || !ctx->frameCount) {
        return;
    }

    auto* backend = static_cast<JITBackend*>(ctx->backendPtr);
    size_t currentFrameIdx = *ctx->frameCount > 0 ? *ctx->frameCount - 1 : 0;

    if (currentFrameIdx >= backend->frameUpvaluesStack_.size() ||
        uvIdx >= backend->frameUpvaluesStack_[currentFrameIdx].size()) {
        if (ctx->hasError && ctx->errorBuffer) {
            *ctx->hasError = true;
            *ctx->errorBuffer = "内部错误: upvalue 索引越界 (" + std::to_string(static_cast<int>(uvIdx)) + ")";
        }
        return;
    }

    auto& uv = backend->frameUpvaluesStack_[currentFrameIdx][uvIdx];
    if (uv->isClosed) {
        uv->value = bitsToValue(valueBits);
    } else {
        // open 状态：写入 JIT 栈槽
        auto* slotPtr = reinterpret_cast<int64_t*>(uv->stackSlot);
        *slotPtr = valueBits;
    }
}

/// R156: OP_CLOSE_UPVALUE / OP_RETURN — 关闭 open upvalues
void jitCloseUpvalues(JitContext* ctx, int64_t* fromAddr) {
    if (!ctx || !ctx->backendPtr || !fromAddr) {
        return;
    }

    auto* backend = static_cast<JITBackend*>(ctx->backendPtr);
    backend->closeUpvaluesFrom(fromAddr);
}

/// R156: OP_CALL / OP_METHOD_CALL — 推入空 upvalues 到 frameUpvaluesStack_
void jitPushEmptyFrameUpvalues(JitContext* ctx) {
    if (!ctx || !ctx->backendPtr || !ctx->frameCount) {
        return;
    }

    auto* backend = static_cast<JITBackend*>(ctx->backendPtr);
    size_t newFrameCount = *ctx->frameCount;
    if (newFrameCount == 0) {
        return;
    }
    size_t idx = newFrameCount - 1;
    if (backend->frameUpvaluesStack_.size() <= idx) {
        backend->frameUpvaluesStack_.resize(idx + 1);
    }
    backend->frameUpvaluesStack_[idx].clear();
}

/// R156: OP_CALL — 推入闭包 upvalues 到 frameUpvaluesStack_
void jitPushClosureUpvalues(JitContext* ctx, const char* funName) {
    if (!ctx || !ctx->backendPtr || !ctx->frameCount) {
        return;
    }

    auto* backend = static_cast<JITBackend*>(ctx->backendPtr);
    size_t newFrameCount = *ctx->frameCount;
    if (newFrameCount == 0) {
        return;
    }
    size_t idx = newFrameCount - 1;
    if (backend->frameUpvaluesStack_.size() <= idx) {
        backend->frameUpvaluesStack_.resize(idx + 1);
    }
    auto closureIt = backend->functionClosures_.find(funName ? funName : "");
    if (closureIt != backend->functionClosures_.end() && closureIt->second.vmClosure()) {
        backend->frameUpvaluesStack_[idx] = closureIt->second.vmClosure()->upvalues;
    } else {
        backend->frameUpvaluesStack_[idx].clear();
    }
}

/// R156: OP_RETURN — 关闭当前帧的 open upvalues + pop frameUpvaluesStack_
void jitReturnCloseUpvalues(JitContext* ctx, int64_t* fromAddr, size_t newFrameCount) {
    if (!ctx || !ctx->backendPtr) {
        return;
    }
    auto* backend = static_cast<JITBackend*>(ctx->backendPtr);
    backend->closeUpvaluesFrom(fromAddr);
    if (backend->frameUpvaluesStack_.size() > newFrameCount) {
        for (size_t i = newFrameCount; i < backend->frameUpvaluesStack_.size(); ++i) {
            backend->frameUpvaluesStack_[i].clear();
        }
        backend->frameUpvaluesStack_.resize(newFrameCount);
    }
}

} // extern "C"

// ============================================================
// R156: closeUpvaluesFrom — 关闭 open upvalues（JIT 栈向下增长版本）
// ============================================================
void JITBackend::closeUpvaluesFrom(int64_t* fromAddr) {
    auto endIt = openUpvalues_.upper_bound(fromAddr);
    auto it = openUpvalues_.begin();
    while (it != endIt) {
        if (auto uv = it->second.lock()) {
            if (!uv->isClosed) {
                auto* slotPtr = reinterpret_cast<int64_t*>(uv->stackSlot);
                uv->value = bitsToValue(*slotPtr);
                uv->isClosed = true;
            }
        }
        it = openUpvalues_.erase(it);
    }
}

#endif // MINILANG_USE_JIT
