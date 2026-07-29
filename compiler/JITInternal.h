#pragma once

/**
 * @file compiler/JITInternal.h
 * @brief JIT 后端内部共享工具 — NaN-boxing 常量与 Value 位转换。
 *
 * 拆分自 JIT.cpp，供 JITRuntime.cpp / JITClosure.cpp / JITTiering.cpp /
 * JITCodeGen.cpp / JIT.cpp 共享使用。所有内容为 inline，header-only。
 *
 * @see JIT.h JIT.cpp
 * @since P3（JIT.cpp 拆分）
 */

#ifdef MINILANG_USE_JIT

#include "interpreter/Value.h"
#include <cstdint>
#include <cstring>
#include <utility>

namespace jit_internal {

// ============================================================
// NaN-boxing 常量（R142 阶段 3a：与 interpreter/NaNBox.h 保持一致）
// ============================================================
// JIT 代码通过硬编码这些常量生成机器码，无法直接引用 NaNBox.h 的 private 常量。
// 修改 NaNBox.h 时必须同步更新此处。static_assert 验证一致性（见 JIT.cpp 文件末尾）。
inline constexpr uint64_t JIT_INT_TAG_BASE = 0x7FF8000000000000ULL;   ///< INT tag 基址
inline constexpr uint64_t JIT_BOOL_TAG_BASE = 0x7FF9000000000000ULL;  ///< BOOL tag 基址
inline constexpr uint64_t JIT_NULL_BITS = 0x7FFA000000000000ULL;      ///< NULL 完整 64 位表示
inline constexpr uint64_t JIT_PTR_TAG_BASE = 0x7FFB000000000000ULL;   ///< POINTER tag 基址
inline constexpr uint64_t JIT_TAG_FIELD_MASK = 0xFFFF000000000000ULL; ///< 高 16 位 tag 提取掩码
inline constexpr uint64_t JIT_INT48_MASK = 0x0000FFFFFFFFFFFFULL;     ///< 低 48 位 payload 提取掩码
inline constexpr uint64_t JIT_INT47_SIGN_BIT = 0x0000800000000000ULL; ///< int48 符号位（bit 47）
// R143 阶段 3b：浮点 NaN 规范化标记（与 NaNBox.h NAN_BOXED_FLOAT_MARKER 一致）
// 当 double 的位模式落入 boxed NaN 范围（tag field 在 0x7FF8..0x7FFB）时，
// 用此值替代以避免与 INT/BOOL/NULL/POINTER tag 冲突。
inline constexpr uint64_t JIT_NAN_BOXED_FLOAT_MARKER = 0x7FFC000000000000ULL;

// ============================================================
// NaN-boxing 编码工具函数
// ============================================================

/// 编译期将 int64_t 编码为 NaN-boxing INT（仅支持 int48 范围）
/// @return 编码后的 64 位原始位；超 int48 范围返回 false（调用方报错）
inline bool encodeNanBoxInt(int64_t value, uint64_t& outEncoded) {
    // int48 范围：-(2^47) <= value < 2^47
    if (value < -(1LL << 47) || value >= (1LL << 47)) {
        return false;
    }
    outEncoded = JIT_INT_TAG_BASE | (static_cast<uint64_t>(value) & JIT_INT48_MASK);
    return true;
}

/// R143 阶段 3b：编译期将 double 编码为 NaN-boxing FLOAT
/// 直接存储 double 的 64 位原始位，若落入 boxed NaN 范围则替换为 NAN_BOXED_FLOAT_MARKER
/// @return 编码后的 64 位原始位
inline uint64_t encodeNanBoxFloat(double value) {
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(double));
    // 检查是否落入 boxed NaN 范围（tag field 在 0x7FF8..0x7FFB）
    uint64_t tagField = bits & JIT_TAG_FIELD_MASK;
    if (tagField >= JIT_INT_TAG_BASE && tagField <= JIT_PTR_TAG_BASE) {
        return JIT_NAN_BOXED_FLOAT_MARKER;
    }
    return bits;
}

/// R143 阶段 3b：检查 raw bits 是否是 INT 类型（tag field == INT_TAG_BASE）
inline bool isIntBits(uint64_t bits) {
    return (bits & JIT_TAG_FIELD_MASK) == JIT_INT_TAG_BASE;
}

// ============================================================
// Value 位转换工具
// ============================================================
// R143 修复：Value 不是 POD（有用户定义的拷贝/析构管理引用计数），不能直接 memcpy。
// 必须使用 Value::fromBitsBorrowed / Value::detach 这对"借用-转移"语义接口：
//   - bitsToValue（fromBitsBorrowed）：从 raw bits 构造 Value，对指针类型 addRef，
//     使新 Value 在 RAII 下正确管理引用计数（析构会 release）。
//   - valueToBits（detach）：提取 Value 的 raw bits 并转移所有权（将 Value 置 null 避免析构 release）。
//     调用方拿到的 raw bits 成为唯一引用，必须最终通过 fromBitsBorrowed 恢复为 Value（或由 JIT 栈持有）。
// 这避免了两个陷阱：
//   1. memcpy 构造 Value 不 addRef → 析构 release 导致引用计数下溢（UAF）
//   2. memcpy 提取 bits 后 Value 析构 release → 返回的 raw bits 是悬垂指针（UAF）
// 详见 Value.h 中 fromBitsBorrowed/detach 的注释。

inline Value bitsToValue(uint64_t bits) {
    return Value::fromBitsBorrowed(bits);
}

inline uint64_t valueToBits(Value& v) {
    return Value::detach(v);
}

/// R143 修复：从表达式构造 Value 并转移所有权到 raw bits。
/// 用于辅助函数返回堆类型（STRING/ARRAY/...）时，避免 Value 析构释放对象。
/// 用法：`return jitReturn(Value(std::move(concat)));`
/// 标量类型（INT/FLOAT/BOOL/NULL）也安全：detach 只是复制 bits 并置 null，无引用计数副作用。
template <typename T> inline uint64_t jitReturn(T&& v) {
    Value tmp(std::forward<T>(v));
    return valueToBits(tmp);
}

} // namespace jit_internal

// ============================================================
// Forward declarations of extern "C" JIT runtime helper functions
// ============================================================
// These are defined across JITRuntime.cpp, JITClosure.cpp, JITTiering.cpp
// and called by JIT-generated code (via function pointer) and by
// compileAllChunks (via reinterpret_cast<uint64_t>(&func)).

struct JitContext;

extern "C" {
// --- Output/Error (JITRuntime.cpp) ---
void jitPrintInt(JitContext* ctx, int64_t value);
void jitPrintValue(JitContext* ctx, uint64_t rawBits);
void jitReportError(JitContext* ctx, const char* msg);
void jitReportStackOverflow(JitContext* ctx);

// --- Arithmetic/Comparison (JITRuntime.cpp) ---
int64_t jitTruthy(uint64_t bits);
uint64_t jitAddGeneric(JitContext* ctx, uint64_t leftBits, uint64_t rightBits);
uint64_t jitSubGeneric(JitContext* ctx, uint64_t leftBits, uint64_t rightBits);
uint64_t jitMulGeneric(JitContext* ctx, uint64_t leftBits, uint64_t rightBits);
uint64_t jitDivGeneric(JitContext* ctx, uint64_t leftBits, uint64_t rightBits);
uint64_t jitModGeneric(JitContext* ctx, uint64_t leftBits, uint64_t rightBits);
uint64_t jitNegateGeneric(uint64_t bits);
uint64_t jitEqualGeneric(uint64_t leftBits, uint64_t rightBits);
uint64_t jitNotEqualGeneric(uint64_t leftBits, uint64_t rightBits);
uint64_t jitOrderedCompare(uint64_t leftBits, uint64_t rightBits, int64_t cmpType);

// --- Container operations (JITRuntime.cpp) ---
void jitBuildArray(JitContext* ctx, uint8_t count);
uint64_t jitIndexGet(JitContext* ctx, uint64_t objBits, uint64_t idxBits);
void jitIndexSetLocal(JitContext* ctx, uint8_t slot, int64_t* frameBase);
void jitIndexSetGlobal(JitContext* ctx, int64_t* slotPtr);
void jitLen(JitContext* ctx);
void jitIndexSet(JitContext* ctx);
void jitBuildDict(JitContext* ctx, uint8_t pairCount);
void jitBuildTuple(JitContext* ctx, uint8_t count);

// --- OOP (JITRuntime.cpp) ---
void jitClassNew(JitContext* ctx, const char* className, uint8_t argCount);
void jitInitField(JitContext* ctx, const char* fieldName);
void jitDefineClass(JitContext* ctx, const char* className, const char* superClassName);
void jitMemberGet(JitContext* ctx, const char* fieldName);
void jitMemberGetWithIC(JitContext* ctx, const char* fieldName, uint64_t callSiteId, void* backendPtr);
void jitMemberSetVar(JitContext* ctx, int64_t* slotPtr, const char* fieldName);
void jitMemberSetLocal(JitContext* ctx, uint8_t slot, int64_t* frameBase, const char* fieldName);
int64_t jitMethodReturn(JitContext* ctx, JitFrame* framePtr, int64_t retvalBits);
void jitMethodCall(JitContext* ctx, const char* methodName, int64_t packedArgs, int64_t* receiverSlotPtr,
                  const char* superClassName, uint64_t callSiteId, void* backendPtr);

// --- Function calls (JITRuntime.cpp) ---
void jitCallExpr(JitContext* ctx, uint8_t argCount);
void jitCallByName(JitContext* ctx, const char* funName, uint8_t argCount);

// --- Exception handling (JITRuntime.cpp) ---
void jitPushTryHandler(JitContext* ctx, void* catchAddr, int64_t* stackBase);
void jitPopTryHandler(JitContext* ctx);
void jitPushJumpTarget(JitContext* ctx, void* targetAddr);
void* jitPopJumpTarget(JitContext* ctx);
void* jitThrow(JitContext* ctx, uint64_t thrownValueBits, int64_t* currentR13);
void* jitRuntimeThrow(JitContext* ctx, const char* msg, int64_t* currentR13);
void* jitCheckAndRethrow(JitContext* ctx, int64_t* currentR13);

// --- Name variables (JITRuntime.cpp) ---
void jitDefineVar(JitContext* ctx, const char* name);
uint64_t jitGetVar(JitContext* ctx, const char* name);
void jitSetVar(JitContext* ctx, const char* name);
void jitDeleteVar(JitContext* ctx, const char* name);

// --- Closure/Upvalue (JITClosure.cpp) ---
int64_t jitCreateClosure(JitContext* ctx, const char* funName, const void* chunkPtr, uint8_t upvalueCount,
                         const uint8_t* upvalueDescs);
int64_t jitGetUpvalue(JitContext* ctx, uint8_t uvIdx);
void jitSetUpvalue(JitContext* ctx, uint8_t uvIdx, int64_t valueBits);
void jitCloseUpvalues(JitContext* ctx, int64_t* fromAddr);
void jitPushEmptyFrameUpvalues(JitContext* ctx);
void jitPushClosureUpvalues(JitContext* ctx, const char* funName);
void jitReturnCloseUpvalues(JitContext* ctx, int64_t* fromAddr, size_t newFrameCount);

// --- Tiering/OSR/Deopt (JITTiering.cpp) ---
int64_t jitTriggerRecompile(JitContext* ctx, int64_t chunkIdx);
int64_t jitTriggerOsrRecompile(JitContext* ctx, int64_t chunkIdx);
int64_t jitTriggerOsrMigration(JitContext* ctx, int64_t chunkIdx);
int64_t jitDeoptimize(JitContext* ctx, int64_t chunkIdx);

// --- Safepoint GC (JIT.cpp) ---
void jitSafepointGc(JitContext* ctx);
} // extern "C"

#endif // MINILANG_USE_JIT
