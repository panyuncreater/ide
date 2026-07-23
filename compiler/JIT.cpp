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
#include "common/Utf8Utils.h" // R161 fixup: UTF-8 字符串索引慢路径
#include "interpreter/Value.h"
#include <asmjit/asmjit.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================
// NaN-boxing 常量（R142 阶段 3a：与 interpreter/NaNBox.h 保持一致）
// ============================================================
// JIT 代码通过硬编码这些常量生成机器码，无法直接引用 NaNBox.h 的 private 常量。
// 修改 NaNBox.h 时必须同步更新此处。static_assert 验证一致性（见文件末尾）。
namespace {
constexpr uint64_t JIT_INT_TAG_BASE = 0x7FF8000000000000ULL;   ///< INT tag 基址
constexpr uint64_t JIT_BOOL_TAG_BASE = 0x7FF9000000000000ULL;  ///< BOOL tag 基址
constexpr uint64_t JIT_NULL_BITS = 0x7FFA000000000000ULL;      ///< NULL 完整 64 位表示
constexpr uint64_t JIT_PTR_TAG_BASE = 0x7FFB000000000000ULL;   ///< POINTER tag 基址
constexpr uint64_t JIT_TAG_FIELD_MASK = 0xFFFF000000000000ULL; ///< 高 16 位 tag 提取掩码
constexpr uint64_t JIT_INT48_MASK = 0x0000FFFFFFFFFFFFULL;     ///< 低 48 位 payload 提取掩码
constexpr uint64_t JIT_INT47_SIGN_BIT = 0x0000800000000000ULL; ///< int48 符号位（bit 47）
// R143 阶段 3b：浮点 NaN 规范化标记（与 NaNBox.h NAN_BOXED_FLOAT_MARKER 一致）
// 当 double 的位模式落入 boxed NaN 范围（tag field 在 0x7FF8..0x7FFB）时，
// 用此值替代以避免与 INT/BOOL/NULL/POINTER tag 冲突。
constexpr uint64_t JIT_NAN_BOXED_FLOAT_MARKER = 0x7FFC000000000000ULL;

/// 编译期将 int64_t 编码为 NaN-boxing INT（仅支持 int48 范围）
/// @return 编码后的 64 位原始位；超 int48 范围返回 false（调用方报错）
bool encodeNanBoxInt(int64_t value, uint64_t& outEncoded) {
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
uint64_t encodeNanBoxFloat(double value) {
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
} // namespace

// ============================================================
// JIT 运行时辅助函数（extern "C" 链接，简化 calling convention）
// ============================================================
// 这些函数由 JIT 生成的本地代码通过 call 指令调用，故使用 extern "C"
// 避免名称修饰，并确保 C 调用约定。函数本身用 C++ 实现，可使用标准库。

// 注意：bitsToValue/valueToBits 必须放在 extern "C" 块之外，因为它们返回 C++ 类型 Value，
// 若在 extern "C" 块内会触发 C4190 警告（C 链接但返回与 C 不兼容的 UDT）。
namespace {
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
} // namespace

extern "C" {

/// 输出 int64_t 值到 outputCallback（R142 之前用，保留用于向后兼容）
/// @param ctx JitContext 指针（r12 寄存器值）
/// @param value 待输出的整数值
void jitPrintInt(JitContext* ctx, int64_t value) {
    if (ctx && ctx->outputCallback && *ctx->outputCallback) {
        (*ctx->outputCallback)(std::to_string(value));
    }
}

/// 输出 NaN-boxing Value 到 outputCallback（R142 阶段 3a：OP_PRINT 用）
/// @param ctx JitContext 指针（r12 寄存器值）
/// @param rawBits 待输出的 NaN-boxing Value 原始 8 字节位
void jitPrintValue(JitContext* ctx, uint64_t rawBits) {
    if (!ctx || !ctx->outputCallback || !*ctx->outputCallback) {
        return;
    }
    // R143 修复：使用 bitsToValue（fromBitsBorrowed）正确管理引用计数，
    // 避免 StringData 在 toString() 期间被提前释放。
    Value v = bitsToValue(rawBits);
    (*ctx->outputCallback)(v.toString());
}

/// 报告运行时错误（PoC 阶段未使用，预留给阶段 2 的除零/溢出检查）
/// @param ctx JitContext 指针
/// @param msg 错误消息（C 字符串，必须为静态字符串）
void jitReportError(JitContext* ctx, const char* msg) {
    if (ctx && ctx->hasError && ctx->errorBuffer) {
        *ctx->hasError = true;
        if (msg) {
            *ctx->errorBuffer = msg;
        }
    }
}

// ============================================================
// R143 阶段 3b：浮点 + 字符串类型支持（C++ 辅助函数）
// ============================================================
// 设计策略："INT 快速路径 + C++ 辅助慢速路径"
//   - INT 运算保留 R142 原生 JIT 代码（解码-运算-重编码，最优性能）
//   - FLOAT/STRING 运算通过 C++ 辅助函数（保证语义正确，简化 JIT 代码生成）
//   - JIT 代码在运算开头做类型分派：若两操作数均为 INT 走原生路径，否则调用辅助函数
//
// 辅助函数约定：
//   - 参数为 raw bits（uint64_t），通过 memcpy 构造 Value 处理
//   - 返回值为结果的 raw bits（uint64_t）
//   - 错误通过 ctx->hasError 标志报告（除零等）
//   - 调用约定：extern "C"，Windows x64 (rcx, rdx, r8, r9) / System V (rdi, rsi, rdx, rcx)

/// 通用 truthiness 检查（R143 修复 FLOAT/STRING 的 truthiness 错误）
/// @param bits 待检查的 NaN-boxing Value 原始位
/// @return 1 (truthy) 或 0 (falsy)
extern "C" int64_t jitTruthy(uint64_t bits) {
    Value v = bitsToValue(bits);
    return v.isTruthy() ? 1 : 0;
}

/// 通用加法（FLOAT/FLOAT, STRING/STRING, STRING/其他, 其他/STRING）
/// @param leftBits  左操作数 raw bits
/// @param rightBits 右操作数 raw bits
/// @return 结果 raw bits（FLOAT 或 STRING）
extern "C" uint64_t jitAddGeneric(uint64_t leftBits, uint64_t rightBits) {
    Value left = bitsToValue(leftBits);
    Value right = bitsToValue(rightBits);
    // 字符串拼接（StackVM numericOp OP_ADD_INT 路径对齐）
    if (left.isString() && right.isString()) {
        const auto& ls = left.stringVal();
        const auto& rs = right.stringVal();
        std::string concat;
        concat.reserve(ls.size() + rs.size());
        concat.append(ls).append(rs);
        return jitReturn(Value(std::move(concat)));
    }
    if (left.isString() || right.isString()) {
        std::string result;
        if (left.isString()) {
            const auto& ls = left.stringVal();
            auto rs = right.toString();
            result.reserve(ls.size() + rs.size());
            result.append(ls).append(rs);
        } else {
            auto ls = left.toString();
            const auto& rs = right.stringVal();
            result.reserve(ls.size() + rs.size());
            result.append(ls).append(rs);
        }
        return jitReturn(Value(std::move(result)));
    }
    // R145 修复：整数加法（INT + INT，INT48 溢出时从 JIT 原生路径降级到此）
    // StackVM 的 OverflowCheck 检查 INT64 范围，INT48 范围由 Value(int64_t) 自动装箱
    if (left.isInt() && right.isInt()) {
        int64_t l = left.intVal();
        int64_t r = right.intVal();
        if (OverflowCheck::addOverflow(l, r)) {
            // INT64 溢出（防御性，JIT 当前无法加载 INT64 边界值但未来可能支持）
            // 无法报错（没有 ctx 参数），返回 0 作为降级
            return jitReturn(Value(0));
        }
        return jitReturn(Value(l + r)); // Value(int64_t) 自动处理 int48 装箱
    }
    // 浮点加法（StackVM computeArith 路径对齐）
    if (left.isFloat() || right.isFloat()) {
        return jitReturn(Value(left.toDouble() + right.toDouble()));
    }
    // 不支持的类型组合
    return jitReturn(Value(0.0));
}

/// 通用减法（FLOAT/FLOAT，含 INT+INT 降级路径）
extern "C" uint64_t jitSubGeneric(uint64_t leftBits, uint64_t rightBits) {
    Value left = bitsToValue(leftBits);
    Value right = bitsToValue(rightBits);
    // R145 修复：整数减法（INT + INT，INT48 溢出时从 JIT 原生路径降级到此）
    if (left.isInt() && right.isInt()) {
        int64_t l = left.intVal();
        int64_t r = right.intVal();
        if (OverflowCheck::subOverflow(l, r)) {
            return jitReturn(Value(0));
        }
        return jitReturn(Value(l - r));
    }
    return jitReturn(Value(left.toDouble() - right.toDouble()));
}

/// 通用乘法（FLOAT/FLOAT，含 INT+INT 降级路径）
extern "C" uint64_t jitMulGeneric(uint64_t leftBits, uint64_t rightBits) {
    Value left = bitsToValue(leftBits);
    Value right = bitsToValue(rightBits);
    // R145 修复：整数乘法（INT + INT，INT48 溢出时从 JIT 原生路径降级到此）
    if (left.isInt() && right.isInt()) {
        int64_t l = left.intVal();
        int64_t r = right.intVal();
        if (OverflowCheck::mulOverflow(l, r)) {
            return jitReturn(Value(0));
        }
        return jitReturn(Value(l * r));
    }
    return jitReturn(Value(left.toDouble() * right.toDouble()));
}

/// 通用除法（FLOAT/FLOAT，含除零检查）
extern "C" uint64_t jitDivGeneric(JitContext* ctx, uint64_t leftBits, uint64_t rightBits) {
    Value left = bitsToValue(leftBits);
    Value right = bitsToValue(rightBits);
    double r = right.toDouble();
    if (r == 0.0) {
        if (ctx && ctx->hasError && ctx->errorBuffer) {
            *ctx->hasError = true;
            *ctx->errorBuffer = "除零错误";
        }
        return jitReturn(Value(0.0));
    }
    return jitReturn(Value(left.toDouble() / r));
}

/// 通用取模（FLOAT/FLOAT，含除零检查）
/// StackVM 语义：浮点取模用 std::fmod
extern "C" uint64_t jitModGeneric(JitContext* ctx, uint64_t leftBits, uint64_t rightBits) {
    Value left = bitsToValue(leftBits);
    Value right = bitsToValue(rightBits);
    double r = right.toDouble();
    if (r == 0.0) {
        if (ctx && ctx->hasError && ctx->errorBuffer) {
            *ctx->hasError = true;
            *ctx->errorBuffer = "除零错误";
        }
        return jitReturn(Value(0.0));
    }
    return jitReturn(Value(std::fmod(left.toDouble(), r)));
}

/// 通用取负（FLOAT，含 INT 降级路径）
extern "C" uint64_t jitNegateGeneric(uint64_t bits) {
    Value v = bitsToValue(bits);
    // R145 修复：整数取负（INT，INT48 溢出时从 JIT 原生路径降级到此）
    if (v.isInt()) {
        int64_t i = v.intVal();
        if (OverflowCheck::negateOverflow(i)) {
            return jitReturn(Value(0));
        }
        return jitReturn(Value(-i));
    }
    return jitReturn(Value(-v.toDouble()));
}

/// 通用相等比较（FLOAT/FLOAT 含 +0.0/-0.0/NaN 处理，STRING/STRING 内容比较，跨类型 INT/FLOAT）
/// @return NaN-boxing BOOL raw bits
extern "C" uint64_t jitEqualGeneric(uint64_t leftBits, uint64_t rightBits) {
    Value left = bitsToValue(leftBits);
    Value right = bitsToValue(rightBits);
    bool result = left.equals(right);
    return jitReturn(Value(result));
}

/// 通用不相等比较
extern "C" uint64_t jitNotEqualGeneric(uint64_t leftBits, uint64_t rightBits) {
    Value left = bitsToValue(leftBits);
    Value right = bitsToValue(rightBits);
    bool result = !left.equals(right);
    return jitReturn(Value(result));
}

/// 通用有序比较（FLOAT/FLOAT, STRING/STRING）
/// @param cmpType 0=less, 1=greater, 2=less_equal, 3=greater_equal
/// @return NaN-boxing BOOL raw bits
extern "C" uint64_t jitOrderedCompare(uint64_t leftBits, uint64_t rightBits, int64_t cmpType) {
    Value left = bitsToValue(leftBits);
    Value right = bitsToValue(rightBits);
    bool result = false;
    if (left.isString() && right.isString()) {
        const auto& ls = left.stringVal();
        const auto& rs = right.stringVal();
        switch (cmpType) {
        case 0:
            result = ls < rs;
            break;
        case 1:
            result = ls > rs;
            break;
        case 2:
            result = ls <= rs;
            break;
        case 3:
            result = ls >= rs;
            break;
        default:
            result = false;
            break;
        }
    } else {
        double l = left.toDouble();
        double r = right.toDouble();
        switch (cmpType) {
        case 0:
            result = l < r;
            break;
        case 1:
            result = l > r;
            break;
        case 2:
            result = l <= r;
            break;
        case 3:
            result = l >= r;
            break;
        default:
            result = false;
            break;
        }
    }
    return jitReturn(Value(result));
}

// ============================================================
// R146 阶段 4：数组类型支持（C++ 辅助函数）
// ============================================================
// 设计策略：数组涉及堆对象（ArrayData）+ COW（Copy-On-Write）+ 引用计数，
// 原生 JIT 机器码无法高效处理，全部走 C++ 辅助路径。
//
// JIT 栈访问机制：JIT 代码在调用数组辅助函数前，用 `mov [r12+48], r15`
// 把当前操作数栈顶指针写入 ctx->stackTop。辅助函数通过 ctx->stackTop
// 读写 JIT 栈（向低地址增长：push = sp -= 1; *sp = val; pop = val = *sp; sp += 1）。
// 调用后 JIT 代码用 `mov r15, [r12+48]` 恢复 r15（辅助函数可能修改了栈顶）。
//
// 辅助函数约定：
//   - 错误通过 ctx->hasError + ctx->errorBuffer 报告（越界、类型不匹配等）
//   - 返回值为结果的 raw bits（OP_INDEX_GET 返回元素，OP_INDEX_SET 返回变异后的容器）
//   - 引用计数：bitsToValue addRef，jitReturn detach 转移所有权

/// OP_BUILD_ARRAY: 从 JIT 栈 pop count 个元素，构造 ArrayData，push 数组 raw bits
/// @param ctx JitContext 指针（访问 stackTop 读写 JIT 栈）
/// @param count 元素个数（1 字节，最多 255）
extern "C" void jitBuildArray(JitContext* ctx, uint8_t count) {
    if (!ctx || !ctx->stackTop) {
        return;
    }
    int64_t* sp = ctx->stackTop;
    std::vector<Value> elements(count);
    // 栈顶是最后一个元素，逆序 pop 填入正确位置
    for (int i = count - 1; i >= 0; --i) {
        elements[i] = bitsToValue(*sp);
        sp += 1; // pop（向高地址移动）
    }
    Value arr(std::move(elements));
    // push 数组 raw bits
    sp -= 1;
    *sp = valueToBits(arr); // valueToBits(detach) 转移所有权，arr 被置 null 避免析构 release
    ctx->stackTop = sp;     // 写回更新后的栈顶
}

/// OP_INDEX_GET: 读取容器元素（支持 array/dict/string/tuple 多态 + 越界检查）
/// @param ctx JitContext 指针（错误报告用）
/// @param objBits 容器 raw bits
/// @param idxBits 索引 raw bits
/// @return 元素 raw bits；错误时设置 hasError 并返回 NULL bits
extern "C" uint64_t jitIndexGet(JitContext* ctx, uint64_t objBits, uint64_t idxBits) {
    Value obj = bitsToValue(objBits);
    Value idx = bitsToValue(idxBits);
    // 数组索引读取（最常见的路径）
    if (obj.isArray() && idx.isInt()) {
        const auto& arr = obj.arrayVal(); // const 重载，不触发 COW
        int64_t i = idx.intVal();
        if (i < 0 || static_cast<size_t>(i) >= arr.size()) {
            if (ctx && ctx->hasError && ctx->errorBuffer) {
                *ctx->hasError = true;
                *ctx->errorBuffer =
                    "数组索引越界: " + std::to_string(i) + ", 有效范围 [0, " + std::to_string(arr.size()) + ")";
            }
            return jitReturn(Value()); // NULL
        }
        return jitReturn(arr[static_cast<size_t>(i)]); // addRef by bitsToValue in jitReturn
    }
    // 字符串索引读取（R161 fixup: 对齐 StackVM 的 ASCII 快速路径 + UTF-8 慢路径）
    if (obj.isString() && idx.isInt()) {
        const auto& str = obj.stringVal();
        int64_t i = idx.intVal();

        // ASCII 快速路径：纯 ASCII 字符串直接按字节索引 O(1)
        if (obj.isAsciiString()) {
            if (i < 0 || static_cast<size_t>(i) >= str.size()) {
                if (ctx && ctx->hasError && ctx->errorBuffer) {
                    *ctx->hasError = true;
                    *ctx->errorBuffer =
                        "字符串索引越界: " + std::to_string(i) + ", 有效范围 [0, " + std::to_string(str.size()) + ")";
                }
                return jitReturn(Value());
            }
            return jitReturn(Value(std::string(1, str[static_cast<size_t>(i)])));
        }

        // UTF-8 慢路径：遍历码位对齐 StackVM executeIndexGet
        size_t charCount = 0;
        size_t bytePos = 0;
        size_t targetBytePos = 0;
        size_t targetByteLen = 0;
        bool found = false;
        while (bytePos < str.size()) {
            size_t charLen = Utf8::byteLength(static_cast<unsigned char>(str[bytePos]));
            if (static_cast<size_t>(i) == charCount) {
                targetBytePos = bytePos;
                targetByteLen = charLen;
                found = true;
            }
            bytePos += charLen;
            charCount++;
        }
        if (i < 0 || !found) {
            if (ctx && ctx->hasError && ctx->errorBuffer) {
                *ctx->hasError = true;
                *ctx->errorBuffer = "字符串索引越界: " + std::to_string(i) + ", 有效范围 [0, " +
                                    std::to_string(static_cast<long long>(charCount)) + ")";
            }
            return jitReturn(Value());
        }
        return jitReturn(Value(str.substr(targetBytePos, targetByteLen)));
    }
    // R147 阶段 4b：字典索引读取（键支持 string/int/bool/float，与 StackVM 对齐）
    if (obj.isDict()) {
        auto dk = Value::dictKeyFromValue(idx);
        if (!dk) {
            if (ctx && ctx->hasError && ctx->errorBuffer) {
                *ctx->hasError = true;
                *ctx->errorBuffer = ErrorMessages::kDictKeyInvalidType;
            }
            return jitReturn(Value());
        }
        const auto& dict = obj.dictVal(); // const 重载，不触发 COW
        auto it = dict.find(*dk);
        if (it == dict.end()) {
            return jitReturn(Value()); // 不存在键返回 null（与 StackVM 一致）
        }
        return jitReturn(it->second);
    }
    // R147 阶段 4b：元组索引读取（immutable，与数组语义一致）
    if (obj.isTuple() && idx.isInt()) {
        const auto& tup = obj.tupleVal(); // const 重载
        int64_t i = idx.intVal();
        if (i < 0 || static_cast<size_t>(i) >= tup.size()) {
            if (ctx && ctx->hasError && ctx->errorBuffer) {
                *ctx->hasError = true;
                *ctx->errorBuffer =
                    "元组索引越界: " + std::to_string(i) + ", 有效范围 [0, " + std::to_string(tup.size()) + ")";
            }
            return jitReturn(Value());
        }
        return jitReturn(tup[static_cast<size_t>(i)]);
    }
    if (obj.isArray()) {
        if (ctx && ctx->hasError && ctx->errorBuffer) {
            *ctx->hasError = true;
            *ctx->errorBuffer = ErrorMessages::kArrayIndexMustBeInt;
        }
        return jitReturn(Value());
    }
    if (obj.isString()) {
        if (ctx && ctx->hasError && ctx->errorBuffer) {
            *ctx->hasError = true;
            *ctx->errorBuffer = ErrorMessages::kStringIndexMustBeInt;
        }
        return jitReturn(Value());
    }
    if (obj.isTuple()) {
        if (ctx && ctx->hasError && ctx->errorBuffer) {
            *ctx->hasError = true;
            *ctx->errorBuffer = "元组索引必须是整数";
        }
        return jitReturn(Value());
    }
    // 其他类型组合暂不支持
    if (ctx && ctx->hasError && ctx->errorBuffer) {
        *ctx->hasError = true;
        *ctx->errorBuffer = ErrorMessages::kTypeNotIndexable;
    }
    return jitReturn(Value());
}

/// OP_INDEX_SET_LOCAL: 局部变量数组/字典索引赋值（COW detach + 写入 + 写回栈槽）
/// @param ctx JitContext 指针（访问 stackTop 读写 JIT 栈，错误报告用）
/// @param slot 局部变量槽位号
/// @param frameBase 当前帧的 basePointer（r13 值，指向 slot 0）
/// 栈布局（调用前）：[..., idx, val]（val 在栈顶）
/// 栈布局（调用后）：[...]（pop idx 和 val，不 push）
/// 语义：obj = frameBase[-slot]; obj[idx] = val; frameBase[-slot] = obj（COW detach 后写回）
extern "C" void jitIndexSetLocal(JitContext* ctx, uint8_t slot, int64_t* frameBase) {
    if (!ctx || !ctx->stackTop || !frameBase) {
        return;
    }
    int64_t* sp = ctx->stackTop;
    Value val = bitsToValue(*sp);
    sp += 1; // pop val
    Value idx = bitsToValue(*sp);
    sp += 1;            // pop idx
    ctx->stackTop = sp; // 更新栈顶（pop 2 个，不 push）

    // 读取局部变量栈槽（JIT 栈向下增长，slot 0 在 frameBase，slot N 在 frameBase - N）
    // 注意：frameBase - slot 是 int64_t* 指针运算（元素单位），等价于 frameBase - slot*8 字节
    int64_t* slotPtr = frameBase - slot;
    Value obj = bitsToValue(*slotPtr);

    if (obj.isArray() && idx.isInt()) {
        int64_t i = idx.intVal();
        // 越界检查用 const 重载避免 COW detach
        const auto& arr = obj.arrayVal();
        if (i < 0 || static_cast<size_t>(i) >= arr.size()) {
            if (ctx->hasError && ctx->errorBuffer) {
                *ctx->hasError = true;
                *ctx->errorBuffer =
                    "数组索引越界: " + std::to_string(i) + ", 有效范围 [0, " + std::to_string(arr.size()) + ")";
            }
            return;
        }
        // 触发 COW detach（非 const arrayVal 重载调用 ensureUnique<ArrayData>）
        obj.arrayVal()[static_cast<size_t>(i)] = val;
        // 写回栈槽（detach 后可能产生新 ArrayData，需更新栈槽的 raw bits）
        // obj 的内部 box_ 已被 ensureUnique 更新，提取新的 raw bits
        *slotPtr = valueToBits(obj); // detach 转移所有权到栈槽
        return;
    }
    // R147 阶段 4b：字典索引赋值（键支持 string/int/bool/float，与 StackVM 对齐）
    if (obj.isDict()) {
        auto dk = Value::dictKeyFromValue(idx);
        if (!dk) {
            if (ctx->hasError && ctx->errorBuffer) {
                *ctx->hasError = true;
                *ctx->errorBuffer = ErrorMessages::kDictKeyInvalidType;
            }
            return;
        }
        // 触发 COW detach（非 const dictVal 重载调用 ensureUnique<DictData>）
        obj.dictVal()[*dk] = val;
        *slotPtr = valueToBits(obj); // 写回栈槽
        return;
    }
    if (obj.isArray()) {
        if (ctx->hasError && ctx->errorBuffer) {
            *ctx->hasError = true;
            *ctx->errorBuffer = ErrorMessages::kArrayIndexMustBeInt;
        }
        return;
    }
    // 字符串/元组等索引赋值（StackVM 不支持，与三后端一致报错）
    if (ctx->hasError && ctx->errorBuffer) {
        *ctx->hasError = true;
        *ctx->errorBuffer = ErrorMessages::kTypeNotIndexAssignable;
    }
}

/// R147 阶段 4b：OP_INDEX_SET_VAR 全局变量数组/字典索引赋值
/// 与 jitIndexSetLocal 类似，但目标从 frameBase-slot 变成 globalSlots[slot]
/// @param ctx JitContext 指针
/// @param slotPtr 全局变量槽位指针（globalSlots_ + slot*8，编译期解析 nameIdx→slot）
/// 栈布局（调用前）：[..., idx, val]（val 在栈顶）
/// 栈布局（调用后）：[...]（pop idx 和 val，不 push）
extern "C" void jitIndexSetGlobal(JitContext* ctx, int64_t* slotPtr) {
    if (!ctx || !ctx->stackTop || !slotPtr) {
        return;
    }
    int64_t* sp = ctx->stackTop;
    Value val = bitsToValue(*sp);
    sp += 1; // pop val
    Value idx = bitsToValue(*sp);
    sp += 1;            // pop idx
    ctx->stackTop = sp; // 更新栈顶（pop 2 个，不 push）

    Value obj = bitsToValue(*slotPtr);

    if (obj.isArray() && idx.isInt()) {
        int64_t i = idx.intVal();
        const auto& arr = obj.arrayVal();
        if (i < 0 || static_cast<size_t>(i) >= arr.size()) {
            if (ctx->hasError && ctx->errorBuffer) {
                *ctx->hasError = true;
                *ctx->errorBuffer =
                    "数组索引越界: " + std::to_string(i) + ", 有效范围 [0, " + std::to_string(arr.size()) + ")";
            }
            return;
        }
        obj.arrayVal()[static_cast<size_t>(i)] = val; // COW detach
        *slotPtr = valueToBits(obj);                  // 写回全局槽
        return;
    }
    if (obj.isDict()) {
        auto dk = Value::dictKeyFromValue(idx);
        if (!dk) {
            if (ctx->hasError && ctx->errorBuffer) {
                *ctx->hasError = true;
                *ctx->errorBuffer = ErrorMessages::kDictKeyInvalidType;
            }
            return;
        }
        obj.dictVal()[*dk] = val; // COW detach
        *slotPtr = valueToBits(obj);
        return;
    }
    if (obj.isArray()) {
        if (ctx->hasError && ctx->errorBuffer) {
            *ctx->hasError = true;
            *ctx->errorBuffer = ErrorMessages::kArrayIndexMustBeInt;
        }
        return;
    }
    if (ctx->hasError && ctx->errorBuffer) {
        *ctx->hasError = true;
        *ctx->errorBuffer = ErrorMessages::kTypeNotIndexAssignable;
    }
}

/// R154: OP_LEN — pop 容器值，push 其长度（array/dict/string/tuple 多态）
/// 与 StackVM executeMiscStackOps OP_LEN 语义一致（VMContainers.cpp:948-970）
/// 字符串长度按字节数计算（与 StackVM 现有索引语义一致）
/// @param ctx JitContext 指针（访问 stackTop 读写 JIT 栈，错误报告用）
/// 栈布局（调用前）：[..., v]（v 在栈顶）
/// 栈布局（调用后）：[..., len]（pop 1 + push 1）
extern "C" void jitLen(JitContext* ctx) {
    if (!ctx || !ctx->stackTop) {
        return;
    }
    int64_t* sp = ctx->stackTop;
    Value v = bitsToValue(*sp);
    sp += 1; // pop v（向高地址移动）
    int64_t len = 0;
    if (v.isArray()) {
        len = static_cast<int64_t>(v.arrayVal().size()); // const 重载，不触发 COW
    } else if (v.isDict()) {
        len = static_cast<int64_t>(v.dictVal().size()); // const 重载
    } else if (v.isString()) {
        len = static_cast<int64_t>(v.stringVal().size()); // UTF-8 字节数（与 StackVM 一致）
    } else if (v.isTuple()) {
        len = static_cast<int64_t>(v.tupleVal().size()); // const 重载
    } else {
        if (ctx->hasError && ctx->errorBuffer) {
            *ctx->hasError = true;
            *ctx->errorBuffer = "OP_LEN: 操作数必须是 array/dict/string/tuple，实际为 " + v.typeName();
        }
        // 错误路径：push NULL 保持栈平衡（JIT 后续 epilogue 退出）
        sp -= 1;
        *sp = static_cast<int64_t>(JIT_NULL_BITS);
        ctx->stackTop = sp;
        return;
    }
    // push Value(len) raw bits（INT 类型，无引用计数）
    sp -= 1;
    *sp = jitReturn(Value(len));
    ctx->stackTop = sp;
}

/// R154: OP_INDEX_SET — 嵌套索引赋值（arr[i][j] = val 形态）
/// 与 StackVM executeIndexSet 语义一致（VMContainers.cpp:322-355）
/// 栈序: [..., obj, innerIdx, val]（val 在栈顶）
/// 弹出 val, innerIdx, obj → 修改 obj[innerIdx]=val → 变异后 obj 存入 lastMutatedReceiver_
/// 由后续 OP_WRITEBACK_* 整体写回到全局变量/栈槽
/// @param ctx JitContext 指针（访问 stackTop 和 lastMutatedReceiverPtr）
/// 栈布局（调用前）：[..., obj, innerIdx, val]（val 在栈顶）
/// 栈布局（调用后）：[...]（pop 3 个，不 push）
extern "C" void jitIndexSet(JitContext* ctx) {
    if (!ctx || !ctx->stackTop || !ctx->lastMutatedReceiverPtr) {
        return;
    }
    int64_t* sp = ctx->stackTop;
    Value val = bitsToValue(*sp);
    sp += 1; // pop val
    Value innerIdx = bitsToValue(*sp);
    sp += 1; // pop innerIdx
    Value obj = bitsToValue(*sp);
    sp += 1;            // pop obj
    ctx->stackTop = sp; // 更新栈顶（pop 3 个，不 push）

    if (obj.isArray() && innerIdx.isInt()) {
        int64_t i = innerIdx.intVal();
        // 越界检查用 const 重载避免 COW detach（与 StackVM std::as_const 模式一致）
        const auto& arr = obj.arrayVal();
        if (i < 0 || static_cast<size_t>(i) >= arr.size()) {
            if (ctx->hasError && ctx->errorBuffer) {
                *ctx->hasError = true;
                *ctx->errorBuffer =
                    "数组索引越界: " + std::to_string(i) + ", 有效范围 [0, " + std::to_string(arr.size()) + ")";
            }
            return;
        }
        // 触发 COW detach（非 const arrayVal 重载调用 ensureUnique<ArrayData>）
        obj.arrayVal()[static_cast<size_t>(i)] = val;
    } else if (obj.isDict()) {
        // 字典键支持 string/int/bool/float（与 StackVM 对齐）
        auto dk = Value::dictKeyFromValue(innerIdx);
        if (!dk) {
            if (ctx->hasError && ctx->errorBuffer) {
                *ctx->hasError = true;
                *ctx->errorBuffer = ErrorMessages::kDictKeyInvalidType;
            }
            return;
        }
        obj.dictVal()[*dk] = val; // COW detach
    } else if (obj.isArray()) {
        if (ctx->hasError && ctx->errorBuffer) {
            *ctx->hasError = true;
            *ctx->errorBuffer = ErrorMessages::kArrayIndexMustBeInt;
        }
        return;
    } else {
        if (ctx->hasError && ctx->errorBuffer) {
            *ctx->hasError = true;
            *ctx->errorBuffer = ErrorMessages::kTypeNotIndexAssignable;
        }
        return;
    }
    // 变异后的容器存入 lastMutatedReceiver_（valueToBits detach 转移所有权）
    // 与 StackVM lastMutatedReceiver_ = std::move(obj) 等价
    *ctx->lastMutatedReceiverPtr = valueToBits(obj);
}

/// R156: OP_CLOSURE — 创建闭包值（含 upvalue 捕获）
/// 与 StackVM executeClosure 对齐（VMCalls.cpp:1022-1083）
/// @param ctx JitContext 指针（读取 backendPtr、frameCount、currentBp）
/// @param funName 函数名（C 字符串，编译期嵌入 movabs，需静态生命期）
/// @param chunkPtr 函数 chunk 指针（编译期从 result.functionChunks 获取）
/// @param upvalueCount upvalue 数量
/// @param upvalueDescs 指向字节码中 upvalue 描述符的指针（[isLocal0, index0, isLocal1, index1, ...]）
/// @return NaN-boxed 闭包指针 bits（push 到 JIT 栈）
extern "C" int64_t jitCreateClosure(JitContext* ctx, const char* funName, const void* chunkPtr, uint8_t upvalueCount,
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
/// @param ctx JitContext 指针（读取 backendPtr、frameCount）
/// @param uvIdx upvalue 索引
/// @return NaN-boxed Value bits
extern "C" int64_t jitGetUpvalue(JitContext* ctx, uint8_t uvIdx) {
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
        // valueToBits = Value::detach 会转移所有权并置源 Value 为 null，
        // 直接对 uv->value 调用会导致后续读取得到 null。
        // StackVM OP_GET_UPVALUE 使用 push(uv->value) 拷贝构造（addRef），
        // 此处 tmp 拷贝构造后 detach 仅影响 tmp，uv->value 保持有效。
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
/// @param ctx JitContext 指针（读取 backendPtr、frameCount）
/// @param uvIdx upvalue 索引
/// @param valueBits 栈顶值的 NaN-boxing bits
extern "C" void jitSetUpvalue(JitContext* ctx, uint8_t uvIdx, int64_t valueBits) {
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
        // V-P1-6 fix 的 fieldsModified 同步在 JIT 中暂不支持（JitFrame 无 fieldsModified 字段）
        // 已知限制：闭包内修改捕获的方法实例字段不会自动同步回 receiver
    }
}

/// R156: OP_CLOSE_UPVALUE / OP_RETURN — 关闭 open upvalues
/// 与 StackVM closeUpvaluesFrom 对齐（VM.cpp:189-217）
/// @param ctx JitContext 指针（读取 backendPtr）
/// @param fromAddr 起始地址（JIT 栈槽地址，关闭所有 <= fromAddr 的 open upvalues）
extern "C" void jitCloseUpvalues(JitContext* ctx, int64_t* fromAddr) {
    if (!ctx || !ctx->backendPtr || !fromAddr) {
        return;
    }

    auto* backend = static_cast<JITBackend*>(ctx->backendPtr);
    backend->closeUpvaluesFrom(fromAddr);
}

/// R156: OP_CALL / OP_METHOD_CALL — 推入空 upvalues 到 frameUpvaluesStack_
/// 与 StackVM setupFunctionCallFrame 设置 newFrame.upvalues = {} 对齐。
/// OP_CALL（命名函数调用）和 OP_METHOD_CALL（方法调用）不携带闭包 upvalues，
/// 但必须 push 空 vector 以保持 frameUpvaluesStack_ 与 frameStack_ 索引对齐，
/// 否则 passthrough（OP_CLOSURE isLocal=false）会读到错误帧的 upvalues。
/// 在 *frameCount += 1 之后调用，新帧索引 = *frameCount - 1。
/// @param ctx JitContext 指针（读取 backendPtr、frameCount）
extern "C" void jitPushEmptyFrameUpvalues(JitContext* ctx) {
    if (!ctx || !ctx->backendPtr || !ctx->frameCount) {
        return;
    }

    auto* backend = static_cast<JITBackend*>(ctx->backendPtr);
    // *frameCount 已 increment，新帧索引 = *frameCount - 1
    size_t newFrameCount = *ctx->frameCount;
    if (newFrameCount == 0) {
        return; // 不应发生（调用前 *frameCount >= 0，increment 后 >= 1）
    }
    size_t idx = newFrameCount - 1;
    if (backend->frameUpvaluesStack_.size() <= idx) {
        backend->frameUpvaluesStack_.resize(idx + 1);
    }
    backend->frameUpvaluesStack_[idx].clear();
}

/// R156: OP_CALL — 推入闭包 upvalues 到 frameUpvaluesStack_
/// 与 StackVM executeCallFunction（VMCalls.cpp:632-637）对齐：
/// 从 functionClosures_ 查找闭包值，提取 upvalues 传递给新帧。
/// 如果找不到闭包值（如 upvalueCount=0 的普通函数），推入空 vector。
/// 在 *frameCount += 1 之后调用，新帧索引 = *frameCount - 1。
/// @param ctx JitContext 指针（读取 backendPtr、frameCount）
/// @param funName 函数名（C 字符串，编译期嵌入 movabs，需静态生命期）
extern "C" void jitPushClosureUpvalues(JitContext* ctx, const char* funName) {
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
    // 从 functionClosures_ 查找闭包值，提取 upvalues（与 StackVM executeCallFunction 对齐）
    auto closureIt = backend->functionClosures_.find(funName ? funName : "");
    if (closureIt != backend->functionClosures_.end() && closureIt->second.vmClosure()) {
        backend->frameUpvaluesStack_[idx] = closureIt->second.vmClosure()->upvalues;
    } else {
        backend->frameUpvaluesStack_[idx].clear();
    }
}

/// R156: OP_RETURN — 关闭当前帧的 open upvalues + pop frameUpvaluesStack_
/// 在 *frameCount -= 1 之前调用，r13 仍是当前帧 basePtr（作为 fromAddr）
/// 同时清理 frameUpvaluesStack_ 中属于当前帧的 upvalues（resize 到 newFrameCount）
/// @param ctx JitContext 指针
/// @param fromAddr 当前帧 basePtr（r13），关闭所有 <= fromAddr 的 open upvalues
/// @param newFrameCount 返回后的新 frameCount（旧 *frameCount - 1）
extern "C" void jitReturnCloseUpvalues(JitContext* ctx, int64_t* fromAddr, size_t newFrameCount) {
    if (!ctx || !ctx->backendPtr) {
        return;
    }
    auto* backend = static_cast<JITBackend*>(ctx->backendPtr);
    backend->closeUpvaluesFrom(fromAddr);
    // R157 fix: 不使用 resize（Debug 模式下迭代器检查可能导致崩溃），
    // 改为 clear 对应位置的元素内容。frameUpvaluesStack_ 的大小保持不变，
    // 但被返回的帧位置的内容被清空，下次复用时通过 jitCallByName 的 clear() 重置。
    if (backend->frameUpvaluesStack_.size() > newFrameCount) {
        for (size_t i = newFrameCount; i < backend->frameUpvaluesStack_.size(); ++i) {
            backend->frameUpvaluesStack_[i].clear();
        }
        backend->frameUpvaluesStack_.resize(newFrameCount);
    }
}

/// R155: OP_CALL_EXPR — 闭包值调用（含 upvalue 绑定，R156 扩展）
/// 与 StackVM executeCallExprValue 对齐（VMCalls.cpp:647-709）
/// 栈布局（调用前）：[..., closure, arg0, arg1, ..., argN-1]（closure 在参数下方）
/// 栈布局（调用后）：新帧栈 [..., arg0, arg1, ..., argN-1, extraSlots...]，调用者 r15 = callerSpAfterPop
/// @param ctx JitContext 指针（访问 stackTop/frames/frameCount/funcEntriesPtr/callerBp）
/// @param argCount 实际参数个数
/// 通过邮箱返回：ctx->methodEntryPtr = 入口地址, ctx->methodLocalCount = localCount
extern "C" void jitCallExpr(JitContext* ctx, uint8_t argCount) {
    if (!ctx || !ctx->stackTop) {
        return;
    }

    int64_t* sp = ctx->stackTop;

    // 1. pop args（借用，不 detach）
    Value args[256];
    for (int i = argCount - 1; i >= 0; --i) {
        args[i] = bitsToValue(*sp); // 借用
        sp += 1;                    // pop
    }

    // 2. pop closure（借用）
    Value callee = bitsToValue(*sp); // 借用
    sp += 1;                         // pop closure
    int64_t* callerSpAfterPop = sp;  // 调用者恢复 r15 用

    // 3. 类型检查
    if (!callee.isClosure()) {
        if (ctx->hasError && ctx->errorBuffer) {
            *ctx->hasError = true;
            *ctx->errorBuffer = "表达式调用需要函数值";
        }
        ctx->stackTop = callerSpAfterPop;
        return;
    }

    // 4. 从 funcEntries_ 查找 JIT 入口地址 + localCount
    if (!ctx->funcEntriesPtr) {
        if (ctx->hasError && ctx->errorBuffer) {
            *ctx->hasError = true;
            *ctx->errorBuffer = "JIT 内部错误: funcEntriesPtr 为空";
        }
        ctx->stackTop = callerSpAfterPop;
        return;
    }
    std::string funName = callee.closureName();
    auto it = ctx->funcEntriesPtr->find(funName);

    // R157: lazy compilation — entryPtr 为 null 时触发按需编译
    if (it == ctx->funcEntriesPtr->end() || it->second.entryPtr == nullptr) {
        auto* backend = static_cast<JITBackend*>(ctx->backendPtr);
        if (backend) {
            JitEntryFn lazyEntry = backend->triggerLazyCompile(funName);
            if (lazyEntry) {
                // 编译成功，重新查找（entryPtr 已更新）
                it = ctx->funcEntriesPtr->find(funName);
            }
        }
    }

    if (it == ctx->funcEntriesPtr->end()) {
        if (ctx->hasError && ctx->errorBuffer) {
            *ctx->hasError = true;
            *ctx->errorBuffer = "未找到函数: " + funName;
        }
        ctx->stackTop = callerSpAfterPop;
        return;
    }
    const JitMethodInfo* foundFunc = &it->second;

    if (foundFunc->entryPtr == nullptr) {
        if (ctx->hasError && ctx->errorBuffer) {
            *ctx->hasError = true;
            *ctx->errorBuffer = "函数 " + funName + " 入口未绑定（lazy compilation 失败）";
        }
        ctx->stackTop = callerSpAfterPop;
        return;
    }

    // 5. 参数校验
    if (argCount < static_cast<uint8_t>(foundFunc->requiredArity) ||
        argCount > static_cast<uint8_t>(foundFunc->arity)) {
        if (ctx->hasError && ctx->errorBuffer) {
            *ctx->hasError = true;
            *ctx->errorBuffer = "函数 " + funName + " 期望 " + std::to_string(foundFunc->requiredArity) + "-" +
                                std::to_string(foundFunc->arity) + " 个参数，但传入了 " + std::to_string(argCount) +
                                " 个";
        }
        ctx->stackTop = callerSpAfterPop;
        return;
    }

    // 6. 默认参数填充
    Value defaults[64];
    int defaultCount = 0;
    if (argCount < static_cast<uint8_t>(foundFunc->arity)) {
        int missingCount = foundFunc->arity - argCount;
        int defaultStartIdx = static_cast<int>(foundFunc->defaultConstIndices->size()) - missingCount;
        if (defaultStartIdx < 0 ||
            static_cast<size_t>(defaultStartIdx + missingCount) > foundFunc->defaultConstIndices->size()) {
            if (ctx->hasError && ctx->errorBuffer) {
                *ctx->hasError = true;
                *ctx->errorBuffer = "函数 " + funName + " 默认参数索引越界";
            }
            ctx->stackTop = callerSpAfterPop;
            return;
        }
        for (int i = defaultStartIdx; i < defaultStartIdx + missingCount; ++i) {
            uint16_t constIdx = (*foundFunc->defaultConstIndices)[static_cast<size_t>(i)];
            if (constIdx == 0xFFFF || constIdx >= foundFunc->constants->size()) {
                if (ctx->hasError && ctx->errorBuffer) {
                    *ctx->hasError = true;
                    *ctx->errorBuffer = "函数 " + funName + " 默认参数常量索引越界";
                }
                ctx->stackTop = callerSpAfterPop;
                return;
            }
            defaults[defaultCount++] = (*foundFunc->constants)[constIdx];
        }
    }

    // 7. 构造新帧栈：push args + push defaults + push extraSlots
    //    JIT 栈向下增长：push = sp -= 1; *sp = valueToBits(val)
    // push args
    for (int i = 0; i < argCount; ++i) {
        sp -= 1;
        *sp = valueToBits(args[i]); // detach
    }
    // push defaults
    for (int i = 0; i < defaultCount; ++i) {
        sp -= 1;
        *sp = valueToBits(defaults[i]); // detach
    }
    // push extraSlots (null)
    int preAllocated = foundFunc->arity; // 函数 chunk 没有 this/fields
    int extraSlots = foundFunc->localCount - preAllocated;
    for (int i = 0; i < extraSlots; ++i) {
        sp -= 1;
        *sp = static_cast<int64_t>(JIT_NULL_BITS);
    }

    // 8. 构造 JitFrame
    int64_t* callerBp = ctx->callerBp; // JIT 代码在调用前存入 r13
    size_t idx = *ctx->frameCount;
    if (idx >= RuntimeLimits::MAX_FRAMES) {
        if (ctx->hasError && ctx->errorBuffer) {
            *ctx->hasError = true;
            *ctx->errorBuffer = ErrorFormat::format(ErrorMessages::kRecursionDepthExceededFmt,
                                                    static_cast<int>(RuntimeLimits::MAX_FRAMES));
        }
        ctx->stackTop = sp;
        return;
    }
    JitFrame* frame = &ctx->frames[idx];
    frame->callerBp = callerBp;
    frame->callerSp = callerSpAfterPop;
    // returnAddr 由 JIT 代码在调用后设置（jitCallExpr 无法访问 JIT Label）
    frame->receiverSlotPtr = nullptr; // 非方法调用，无 writeBack
    frame->methodFieldOrder = nullptr;
    frame->methodBp = nullptr;
    frame->isMethodCall = 0;
    frame->isInitCall = 0;
    frame->fieldCount = 0;

    *ctx->frameCount = idx + 1;

    // R156: 推入闭包值的 upvalues 到 frameUpvaluesStack_（与 StackVM setupFunctionCallFrame 对齐）
    // 普通函数调用（OP_CALL）推入空 vector；闭包值调用（OP_CALL_EXPR）从闭包值提取 upvalues
    auto* backend = static_cast<JITBackend*>(ctx->backendPtr);
    if (backend) {
        if (backend->frameUpvaluesStack_.size() <= idx) {
            backend->frameUpvaluesStack_.resize(idx + 1);
        }
        if (callee.vmClosure()) {
            backend->frameUpvaluesStack_[idx] = callee.vmClosure()->upvalues;
        } else {
            backend->frameUpvaluesStack_[idx].clear();
        }
    }

    // 9. 通过邮箱返回函数入口地址 + localCount
    ctx->methodEntryPtr = foundFunc->entryPtr;
    ctx->methodLocalCount = foundFunc->localCount;

    // 10. 更新栈顶
    ctx->stackTop = sp;
}

/// R147 阶段 4b：OP_BUILD_DICT 构造字典
/// 从 JIT 栈 pop 2*pairCount 个元素（键值对，逆序），构造 DictData，push raw bits
/// @param ctx JitContext 指针
/// @param pairCount 键值对数量（1 字节，最多 255 对 = 510 个元素）
extern "C" void jitBuildDict(JitContext* ctx, uint8_t pairCount) {
    if (!ctx || !ctx->stackTop) {
        return;
    }
    int64_t* sp = ctx->stackTop;
    Value::DictMap dict;
    dict.reserve(pairCount);
    // 栈顶是最后一个 value，逆序 pop（key, val）对
    for (int i = pairCount - 1; i >= 0; --i) {
        Value val = bitsToValue(*sp);
        sp += 1; // pop val
        Value key = bitsToValue(*sp);
        sp += 1; // pop key
        auto dk = Value::dictKeyFromValue(key);
        if (!dk) {
            if (ctx->hasError && ctx->errorBuffer) {
                *ctx->hasError = true;
                *ctx->errorBuffer = ErrorMessages::kDictKeyInvalidType;
            }
            // 清理栈上剩余未处理的键值对
            sp += static_cast<int64_t>(i) * 2;
            ctx->stackTop = sp;
            return;
        }
        dict.emplace(std::move(*dk), std::move(val));
    }
    ctx->stackTop = sp; // 已 pop 2*pairCount 个
    Value d(std::move(dict));
    sp -= 1;              // push dict raw bits
    *sp = valueToBits(d); // detach 转移所有权
    ctx->stackTop = sp;
}

/// R147 阶段 4b：OP_BUILD_TUPLE 构造元组（immutable）
/// 与 jitBuildArray 类似，但构造 TupleData
extern "C" void jitBuildTuple(JitContext* ctx, uint8_t count) {
    if (!ctx || !ctx->stackTop) {
        return;
    }
    int64_t* sp = ctx->stackTop;
    std::vector<Value> elements(count);
    for (int i = count - 1; i >= 0; --i) {
        elements[i] = bitsToValue(*sp);
        sp += 1; // pop
    }
    Value tup = Value::makeTuple(std::move(elements));
    sp -= 1;
    *sp = valueToBits(tup);
    ctx->stackTop = sp;
}

// ============================================================
// R148 阶段 4c-1+2：类支持（基础：OP_DEFINE_CLASS/INIT_FIELD/CLASS_NEW/MEMBER_GET/SET_VAR/SET_LOCAL）
// ============================================================
// 设计策略：全部走 C++ 辅助路径（与 R146 数组/R147 dict 一致）
//   - InstanceData 是 RefCounted 堆对象 + COW，原生 JIT 机器码无法高效处理
//   - classInfo_ / pendingFieldOrder_ 通过 JitContext.classInfoPtr / pendingFieldOrderPtr 访问
//   - 类名/字段名通过 const char* 传递（编译期从 chunk.constants 解析，生命期与 CompileResult 一致）
//   - 本轮不支持：方法调用（OP_METHOD_CALL/SUPER_CALL）、init 自动调用、嵌套赋值（OP_MEMBER_SET + OP_WRITEBACK_*）

/// OP_CLASS_NEW：创建实例（R149 扩展支持 init 方法调用）
/// @param ctx JitContext
/// @param className 类名 C 字符串（编译期从常量池解析）
/// @param argCount 构造参数个数
/// 行为：
///   - 类未注册（OP_DEFINE_CLASS 之前的模板创建）：argCount 必须 0，创建空实例
///   - 类已注册 + 无 init 方法：argCount 必须 0，复制 fieldDefaults 创建实例
///   - 类已注册 + 有 init 方法：
///     * argCount > 0：调用 init（支持默认参数填充）
///     * argCount == 0 + init.requiredArity == 0：自动调用 init（与 StackVM 一致）
///     * argCount == 0 + init.requiredArity > 0：报错（init 期望至少 N 个参数）
///   - 类已注册 + 无 init + argCount > 0：报错（无 init 但传入了参数）
/// 栈布局（调用前）：[argN-1]...[arg0] ← ctx->stackTop 指向 argN-1
/// 栈布局（无 init 调用，返回后）：[instance]
/// 栈布局（有 init 调用，返回前）：[extraSlots...][defaults...][args...][fields...][this]
///   ctx->methodEntryPtr 非 null 表示已设置 init 帧，JIT 代码需 jmp 到入口
extern "C" void jitClassNew(JitContext* ctx, const char* className, uint8_t argCount) {
    if (!ctx || !ctx->stackTop || !ctx->classInfoPtr || !className) {
        return;
    }
    // 默认：methodEntryPtr = null（无 init 调用），JIT 代码检测此字段决定是否 jmp
    ctx->methodEntryPtr = nullptr;
    ctx->methodLocalCount = 0;

    int64_t* sp = ctx->stackTop;

    // 1. pop args（反向填充：栈顶是 argN-1）
    Value args[256];
    for (int i = argCount - 1; i >= 0; --i) {
        args[i] = bitsToValue(*sp); // 借用
        sp += 1;                    // pop
    }
    int64_t* callerSpAfterPop = sp; // 调用者恢复 r15 用

    std::string name(className);
    auto classIt = ctx->classInfoPtr->find(name);
    if (classIt == ctx->classInfoPtr->end()) {
        // 类尚未注册（OP_DEFINE_CLASS 之前的模板创建）：argCount 必须 0
        if (argCount > 0) {
            if (ctx->hasError && ctx->errorBuffer) {
                *ctx->hasError = true;
                *ctx->errorBuffer = "类 " + name + " 尚未定义，不能带参数构造";
            }
            ctx->stackTop = callerSpAfterPop;
            return;
        }
        Value instance = Value::makeInstance(name);
        sp -= 1;
        *sp = valueToBits(instance);
        ctx->stackTop = sp;
        return;
    }

    // 2. 类已注册：复制 fieldDefaults 创建实例
    const JitClassInfo& cls = classIt->second;
    Value instance = Value::makeInstance(cls.name);
    instance.fields() = cls.fieldDefaults; // 拷贝默认值（含继承字段）

    // 3. 沿继承链查找 init 方法
    const JitMethodInfo* initMethod = nullptr;
    std::string searchClassName = cls.name;
    for (int guard = 0; guard < 64 && !searchClassName.empty(); ++guard) {
        std::string key = searchClassName + ".init";
        auto it = ctx->methodEntriesPtr->find(key);
        if (it != ctx->methodEntriesPtr->end()) {
            initMethod = &it->second;
            break;
        }
        auto clsIt2 = ctx->classInfoPtr->find(searchClassName);
        if (clsIt2 == ctx->classInfoPtr->end())
            break;
        searchClassName = clsIt2->second.superClassName;
    }

    // 4. 决定是否调用 init（与 StackVM executeClassNew 一致）
    bool shouldCallInit = false;
    if (initMethod != nullptr) {
        if (argCount > 0) {
            shouldCallInit = true;
        } else if (initMethod->requiredArity == 0) {
            shouldCallInit = true; // 0 必需参数（含全默认参数 init），自动构造时调用
        } else {
            // init 存在但参数不匹配：报错
            if (ctx->hasError && ctx->errorBuffer) {
                *ctx->hasError = true;
                *ctx->errorBuffer = "类 " + cls.name + " 的 init 期望至少 " +
                                    std::to_string(initMethod->requiredArity) + " 个参数，但传入了 0 个";
            }
            ctx->stackTop = callerSpAfterPop;
            return;
        }
    } else if (argCount > 0) {
        // 无 init 但有参数：报错
        if (ctx->hasError && ctx->errorBuffer) {
            *ctx->hasError = true;
            *ctx->errorBuffer = "类 " + cls.name + " 没有 init 方法，但传入了 " + std::to_string(argCount) + " 个参数";
        }
        ctx->stackTop = callerSpAfterPop;
        return;
    }

    if (!shouldCallInit) {
        // 无 init 或不应调用：直接 push instance
        sp -= 1;
        *sp = valueToBits(instance);
        ctx->stackTop = sp;
        return;
    }

    // 5. 调用 init：参数校验 + 默认参数填充
    if (argCount < static_cast<uint8_t>(initMethod->requiredArity) ||
        argCount > static_cast<uint8_t>(initMethod->arity)) {
        if (ctx->hasError && ctx->errorBuffer) {
            *ctx->hasError = true;
            *ctx->errorBuffer = "构造函数 init 期望 " + std::to_string(initMethod->requiredArity) + "-" +
                                std::to_string(initMethod->arity) + " 个参数，但传入了 " + std::to_string(argCount) +
                                " 个";
        }
        ctx->stackTop = callerSpAfterPop;
        return;
    }

    Value defaults[64];
    int defaultCount = 0;
    if (argCount < static_cast<uint8_t>(initMethod->arity)) {
        int missingCount = initMethod->arity - argCount;
        int defaultStartIdx = static_cast<int>(initMethod->defaultConstIndices->size()) - missingCount;
        if (defaultStartIdx < 0 ||
            static_cast<size_t>(defaultStartIdx + missingCount) > initMethod->defaultConstIndices->size()) {
            if (ctx->hasError && ctx->errorBuffer) {
                *ctx->hasError = true;
                *ctx->errorBuffer = "方法 init 默认参数索引越界";
            }
            ctx->stackTop = callerSpAfterPop;
            return;
        }
        for (int i = defaultStartIdx; i < defaultStartIdx + missingCount; ++i) {
            uint16_t constIdx = (*initMethod->defaultConstIndices)[static_cast<size_t>(i)];
            if (constIdx == 0xFFFF || constIdx >= initMethod->constants->size()) {
                if (ctx->hasError && ctx->errorBuffer) {
                    *ctx->hasError = true;
                    *ctx->errorBuffer = "方法 init 默认参数常量索引越界";
                }
                ctx->stackTop = callerSpAfterPop;
                return;
            }
            defaults[defaultCount++] = (*initMethod->constants)[constIdx];
        }
    }

    // 6. 构造新帧：push this + push fields + push args + push defaults + push extraSlots

    // R149 fix: valueToBits 会 detach（转移所有权置 v 为 null），所以 push this 后
    // 不能再从 instance 读 fields。修复：先拷贝 instance 用于 push this（detach 拷贝），
    // 原 instance 保留用于读 fields，函数结束时自动析构。
    Value instanceForPush = instance; // 拷贝（addRef）
    sp -= 1;
    *sp = valueToBits(instanceForPush); // detach instanceForPush，原 instance 仍有效

    // push fields（按 init chunk 的 fieldOrder 顺序）
    int fieldCount = 0;
    if (initMethod->fieldOrder && !initMethod->fieldOrder->empty()) {
        const Value& instanceConst = instance;
        for (const auto& fieldName : *initMethod->fieldOrder) {
            auto fieldIt = instanceConst.fields().find(fieldName);
            sp -= 1;
            if (fieldIt != instanceConst.fields().end()) {
                Value fieldCopy = fieldIt->second; // 拷贝（addRef），valueToBits 需要 non-const
                *sp = valueToBits(fieldCopy);      // detach（转移所有权到 bits）
            } else {
                *sp = static_cast<int64_t>(JIT_NULL_BITS);
            }
        }
        fieldCount = static_cast<int>(initMethod->fieldOrder->size());
    }

    // push args
    for (int i = 0; i < argCount; ++i) {
        sp -= 1;
        *sp = valueToBits(args[i]);
    }
    // push defaults
    for (int i = 0; i < defaultCount; ++i) {
        sp -= 1;
        *sp = valueToBits(defaults[i]);
    }

    // push extraSlots (null)
    int preAllocated = 1 + fieldCount + initMethod->arity;
    int extraSlots = initMethod->localCount - preAllocated;
    if (extraSlots < 0) {
        if (ctx->hasError && ctx->errorBuffer) {
            *ctx->hasError = true;
            *ctx->errorBuffer = "类 " + cls.name +
                                " 的 init 方法帧布局损坏: localCount=" + std::to_string(initMethod->localCount) +
                                " < preAllocated=" + std::to_string(preAllocated);
        }
        ctx->stackTop = callerSpAfterPop;
        return;
    }
    for (int i = 0; i < extraSlots; ++i) {
        sp -= 1;
        *sp = static_cast<int64_t>(JIT_NULL_BITS);
    }

    // 7. 设置 JitFrame
    int64_t* callerBp = ctx->callerBp; // JIT 代码在调用前存入
    size_t idx = *ctx->frameCount;
    if (idx >= RuntimeLimits::MAX_FRAMES) {
        if (ctx->hasError && ctx->errorBuffer) {
            *ctx->hasError = true;
            *ctx->errorBuffer = ErrorFormat::format(ErrorMessages::kRecursionDepthExceededFmt,
                                                    static_cast<int>(RuntimeLimits::MAX_FRAMES));
        }
        ctx->stackTop = sp;
        return;
    }
    JitFrame* frame = &ctx->frames[idx];
    frame->callerBp = callerBp;
    frame->callerSp = callerSpAfterPop;
    frame->receiverSlotPtr = nullptr; // init 调用无 writeBack（this 即新实例）
    frame->methodFieldOrder = initMethod->fieldOrder;
    frame->methodBp = sp + (initMethod->localCount - 1); // this 在 slot 0 = 最高地址
    frame->isMethodCall = 1;
    frame->isInitCall = 1; // init 返回 this 而非返回值
    frame->fieldCount = fieldCount;

    *ctx->frameCount = idx + 1;

    // R156: 推入空 upvalues（init 调用不携带闭包 upvalues，但需保持 frameUpvaluesStack_ 对齐）
    {
        auto* backend = static_cast<JITBackend*>(ctx->backendPtr);
        if (backend) {
            if (backend->frameUpvaluesStack_.size() <= idx) {
                backend->frameUpvaluesStack_.resize(idx + 1);
            }
            backend->frameUpvaluesStack_[idx].clear();
        }
    }

    // 8. 通过 ctx 邮箱返回 init 入口地址 + localCount
    ctx->methodEntryPtr = initMethod->entryPtr;
    ctx->methodLocalCount = initMethod->localCount;

    // 9. 更新栈顶
    ctx->stackTop = sp;
}

/// OP_INIT_FIELD：栈顶实例写字段 + 记录字段名到 pendingFieldOrder_
/// 栈布局：[instance, val]（val 在栈顶），pop val 后修改栈顶 instance
extern "C" void jitInitField(JitContext* ctx, const char* fieldName) {
    if (!ctx || !ctx->stackTop || !ctx->pendingFieldOrderPtr || !fieldName) {
        return;
    }
    int64_t* sp = ctx->stackTop;
    Value val = bitsToValue(*sp);
    sp += 1;                           // pop val
    Value instance = bitsToValue(*sp); // 栈顶现在是 instance（借用，addRef）
    if (!instance.isInstance()) {
        if (ctx->hasError && ctx->errorBuffer) {
            *ctx->hasError = true;
            *ctx->errorBuffer = "OP_INIT_FIELD: 栈顶不是实例";
        }
        // 恢复栈
        sp -= 1;
        *sp = valueToBits(val);
        ctx->stackTop = sp;
        return;
    }
    instance.fields()[fieldName] = val; // 非 const 触发 COW detach
    // 用 detach 后的新 bits 覆盖栈顶 instance
    *sp = valueToBits(instance);
    ctx->pendingFieldOrderPtr->push_back(std::string(fieldName));
    ctx->stackTop = sp;
}

/// OP_DEFINE_CLASS：从栈顶模板实例提取字段，构建 JitClassInfo
/// 栈布局：[templateInstance]（pop 消费）
/// @param className 类名
/// @param superClassName 父类名（nullptr 或空字符串表示无父类）
extern "C" void jitDefineClass(JitContext* ctx, const char* className, const char* superClassName) {
    if (!ctx || !ctx->stackTop || !ctx->classInfoPtr || !ctx->pendingFieldOrderPtr || !className) {
        return;
    }
    int64_t* sp = ctx->stackTop;
    Value templateInstance = bitsToValue(*sp);
    sp += 1; // pop templateInstance
    ctx->stackTop = sp;
    if (!templateInstance.isInstance()) {
        if (ctx->hasError && ctx->errorBuffer) {
            *ctx->hasError = true;
            *ctx->errorBuffer = "OP_DEFINE_CLASS: 栈顶不是模板实例";
        }
        return;
    }
    std::string clsName(className);
    std::string superName(superClassName ? superClassName : "");

    // 从 pendingFieldOrder_ 提取当前类字段（按声明顺序）
    std::vector<std::string> ownFieldOrder = std::move(*ctx->pendingFieldOrderPtr);
    ctx->pendingFieldOrderPtr->clear();

    std::unordered_map<std::string, Value> ownFieldDefaults;
    for (const auto& fieldName : ownFieldOrder) {
        auto it = templateInstance.fields().find(fieldName);
        if (it != templateInstance.fields().end()) {
            ownFieldDefaults[fieldName] = it->second;
        }
    }

    // 沿继承链合并父类字段（父类字段在前，子类覆盖同名）
    std::vector<std::string> mergedOrder;
    std::unordered_map<std::string, Value> mergedDefaults;

    // 构建直接父类→根祖先链
    std::vector<std::string> chain;
    std::string cur = superName;
    while (!cur.empty()) {
        auto it = ctx->classInfoPtr->find(cur);
        if (it == ctx->classInfoPtr->end())
            break;
        chain.push_back(cur);
        cur = it->second.superClassName;
        if (chain.size() > 64)
            break; // 防止循环继承
    }
    // AUDIT-BUG-C3 fix 对齐：正序遍历 chain（直接父类在前，根祖先在后），
    // 配合 "if not found then add" 去重——直接父类同名字段先入表，根祖先被跳过。
    // 原倒序遍历导致根祖先先入表，直接父类同名字段被跳过，三后端不一致。
    // 与 StackVM executeDefineClass 第 1322 行语义对齐。
    for (auto it = chain.begin(); it != chain.end(); ++it) {
        auto clsIt = ctx->classInfoPtr->find(*it);
        if (clsIt == ctx->classInfoPtr->end())
            continue;
        for (const auto& fieldName : clsIt->second.fieldOrder) {
            if (mergedDefaults.find(fieldName) == mergedDefaults.end()) {
                mergedOrder.push_back(fieldName);
                auto defIt = clsIt->second.fieldDefaults.find(fieldName);
                if (defIt != clsIt->second.fieldDefaults.end()) {
                    mergedDefaults[fieldName] = defIt->second;
                }
            }
        }
    }
    // 当前类字段最后处理（覆盖父类同名）
    for (const auto& fieldName : ownFieldOrder) {
        if (mergedDefaults.find(fieldName) == mergedDefaults.end()) {
            mergedOrder.push_back(fieldName);
        }
        mergedDefaults[fieldName] = ownFieldDefaults[fieldName];
    }

    JitClassInfo info;
    info.name = clsName;
    info.superClassName = superName;
    info.fieldOrder = std::move(mergedOrder);
    info.fieldDefaults = std::move(mergedDefaults);
    (*ctx->classInfoPtr)[clsName] = std::move(info);
}

/// OP_MEMBER_GET：访问 obj.field
/// 栈布局：[obj]，pop obj + push fieldValue
/// instance: 查 fields map，未找到报错（JIT 不支持方法标记字符串）
/// dict: 查 dict，未找到返回 null
extern "C" void jitMemberGet(JitContext* ctx, const char* fieldName) {
    if (!ctx || !ctx->stackTop || !fieldName) {
        return;
    }
    int64_t* sp = ctx->stackTop;
    Value obj = bitsToValue(*sp);
    sp += 1; // pop obj
    if (obj.isInstance()) {
        const auto& fields = obj.fields(); // const 引用，不触发 COW
        auto it = fields.find(fieldName);
        if (it != fields.end()) {
            Value result = it->second; // 拷贝（addRef）
            sp -= 1;
            *sp = valueToBits(result);
            ctx->stackTop = sp;
            return;
        }
        // JIT 不支持方法标记字符串（不支持方法调用）
        if (ctx->hasError && ctx->errorBuffer) {
            *ctx->hasError = true;
            *ctx->errorBuffer = "类 " + obj.className() + " 没有字段 '" + std::string(fieldName) + "'";
        }
        return;
    }
    if (obj.isDict()) {
        auto dk = Value::dictKeyFromValue(Value(std::string(fieldName)));
        const auto& dict = obj.dictVal(); // const 引用
        auto it = dict.find(*dk);
        Value result = (it != dict.end()) ? it->second : Value();
        sp -= 1;
        *sp = valueToBits(result);
        ctx->stackTop = sp;
        return;
    }
    if (ctx->hasError && ctx->errorBuffer) {
        *ctx->hasError = true;
        *ctx->errorBuffer = ErrorMessages::kTypeNotMemberAccessible;
    }
}

/// R160: OP_MEMBER_GET 带 inline cache 版本
/// 单态（monomorphic）inline cache：缓存 (Instance 内部指针, 字段 Value 指针)。
/// cache 命中时直接从 cachedFieldValuePtr 读取并 push，跳过 unordered_map lookup。
/// cache 未命中时走原 jitMemberGet 慢速路径，并更新 cache。
///
/// @param callSiteId 编译期分配的 per-call-site ID（索引 JITBackend::memberGetIC_）
/// @param backendPtr JITBackend* this 指针（用于访问 memberGetIC_/icHitCount_/icMissCount_）
extern "C" void jitMemberGetWithIC(JitContext* ctx, const char* fieldName, uint64_t callSiteId, void* backendPtr) {
    if (!ctx || !ctx->stackTop || !fieldName || !backendPtr) {
        return;
    }
    int64_t* sp = ctx->stackTop;
    Value obj = bitsToValue(*sp); // peek（不 pop，命中时原地替换，未命中时 pop）

    // 仅对 Instance 类型尝试 IC（dict 的 string key 不稳定，跳过）
    if (obj.isInstance()) {
        // 提取 InstanceData 原始地址作为 cache key（低 48 位）
        // 注意：必须使用 const 版本 fields() 避免 ensureUnique COW 复制，
        // 否则 obj 析构后 COW 副本被释放，cache 指针悬垂。
        const void* instPtr = static_cast<const void*>(obj.gcRootPtr());
        auto* backend = static_cast<JITBackend*>(backendPtr);
        if (callSiteId < backend->memberGetIC_.size()) {
            auto& entry = backend->memberGetIC_[callSiteId];
            if (entry.cachedInstancePtr == instPtr && entry.cachedFieldValuePtr != nullptr) {
                // cache 命中：直接从字段指针读取，跳过 map lookup
                Value result = *entry.cachedFieldValuePtr; // 拷贝（addRef）
                sp += 1;                                   // pop obj
                sp -= 1;                                   // push result
                *sp = valueToBits(result);
                ctx->stackTop = sp;
                backend->icHitCount_++;
                return;
            }
        }
        // cache 未命中：走慢速路径并更新 cache
        backend->icMissCount_++;
        // 关键：使用 std::as_const(obj).fields() 调用 const 重载，避免 ensureUnique COW
        const auto& fields = std::as_const(obj).fields();
        auto it = fields.find(fieldName);
        if (it != fields.end()) {
            Value result = it->second; // 拷贝（addRef）
            sp += 1;                   // pop obj
            sp -= 1;                   // push result
            *sp = valueToBits(result);
            ctx->stackTop = sp;
            // 更新 cache：记录 (Instance 地址, 字段 Value 指针)
            // 安全性：InstanceData 由全局变量/局部变量持有，不会被释放；
            // unordered_map node 在 erase 前地址稳定；MiniLang 字段不被 erase。
            if (callSiteId < backend->memberGetIC_.size()) {
                auto& entry = backend->memberGetIC_[callSiteId];
                entry.cachedInstancePtr = instPtr;
                entry.cachedFieldValuePtr = &(it->second);
            }
            return;
        }
        // 字段不存在：错误路径
        if (ctx->hasError && ctx->errorBuffer) {
            *ctx->hasError = true;
            *ctx->errorBuffer = "类 " + obj.className() + " 没有字段 '" + std::string(fieldName) + "'";
        }
        return;
    }

    // 非 Instance 类型（dict/null/...）：回退到原 jitMemberGet 逻辑
    sp += 1; // pop obj
    if (obj.isDict()) {
        auto dk = Value::dictKeyFromValue(Value(std::string(fieldName)));
        const auto& dict = obj.dictVal();
        auto it = dict.find(*dk);
        Value result = (it != dict.end()) ? it->second : Value();
        sp -= 1;
        *sp = valueToBits(result);
        ctx->stackTop = sp;
        return;
    }
    if (ctx->hasError && ctx->errorBuffer) {
        *ctx->hasError = true;
        *ctx->errorBuffer = ErrorMessages::kTypeNotMemberAccessible;
    }
}

/// OP_MEMBER_SET_VAR：全局变量.field = val
/// @param slotPtr 全局变量槽位指针（编译期解析 nameIdx→slot，运行时计算地址）
/// @param fieldName 字段名
extern "C" void jitMemberSetVar(JitContext* ctx, int64_t* slotPtr, const char* fieldName) {
    if (!ctx || !ctx->stackTop || !slotPtr || !fieldName) {
        return;
    }
    int64_t* sp = ctx->stackTop;
    Value val = bitsToValue(*sp);
    sp += 1; // pop val
    ctx->stackTop = sp;
    Value obj = bitsToValue(*slotPtr); // 借用（addRef）
    if (obj.isInstance()) {
        obj.fields()[fieldName] = val; // 非 const 触发 COW detach
        *slotPtr = valueToBits(obj);   // 写回新 bits
        return;
    }
    if (obj.isDict()) {
        auto dk = Value::dictKeyFromValue(Value(std::string(fieldName)));
        obj.dictVal()[*dk] = val; // 非 const 触发 COW detach
        *slotPtr = valueToBits(obj);
        return;
    }
    if (ctx->hasError && ctx->errorBuffer) {
        *ctx->hasError = true;
        *ctx->errorBuffer = ErrorMessages::kTypeNotMemberAssignable;
    }
}

/// OP_MEMBER_SET_LOCAL：局部变量.field = val
/// @param slot 局部变量栈槽（相对 frameBase）
/// @param frameBase r13 值（当前帧 basePointer）
/// @param fieldName 字段名
extern "C" void jitMemberSetLocal(JitContext* ctx, uint8_t slot, int64_t* frameBase, const char* fieldName) {
    if (!ctx || !ctx->stackTop || !frameBase || !fieldName) {
        return;
    }
    int64_t* sp = ctx->stackTop;
    Value val = bitsToValue(*sp);
    sp += 1; // pop val
    ctx->stackTop = sp;
    int64_t* slotPtr = frameBase - slot; // int64_t* 指针算术（等价于 frameBase - slot*8 字节）
    Value obj = bitsToValue(*slotPtr);   // 借用
    if (obj.isInstance()) {
        obj.fields()[fieldName] = val; // 非 const 触发 COW detach
        *slotPtr = valueToBits(obj);   // 写回新 bits
        return;
    }
    if (obj.isDict()) {
        auto dk = Value::dictKeyFromValue(Value(std::string(fieldName)));
        obj.dictVal()[*dk] = val;
        *slotPtr = valueToBits(obj);
        return;
    }
    if (ctx->hasError && ctx->errorBuffer) {
        *ctx->hasError = true;
        *ctx->errorBuffer = ErrorMessages::kTypeNotMemberAssignable;
    }
}

// ============================================================
// R149 阶段 5：方法调用支持（OP_METHOD_CALL / OP_SUPER_CALL / init 调用）
// ============================================================
// 设计要点（与 StackVM executeInstanceMethodCall/executeReturn 对齐）：
//   - JIT 栈向下增长：push = sp -= 1; pop = sp += 1（int64_t* 指针算术）
//   - 方法帧布局（向低地址）：
//       [extraSlots...][args...][fields...][this]
//                                        ^methodBp (slot 0 = r13)
//     this 在 methodBp[0]（slot 0，最高地址）
//     field_i 在 methodBp[-(i+1)]（slot i+1）
//   - writeBack：方法返回时，字段槽同步回 this.fields()，再写回 receiverSlotPtr
//   - init 返回 this 而非返回值
//   - super 调用：从父类开始沿继承链查找方法

/// OP_RETURN 方法调用路径：字段槽同步回 this + init 返回 this + writeBack 到接收者
/// @param ctx JIT 上下文
/// @param framePtr 当前返回的帧指针（frames[*frameCount] 已减 1）
/// @param retvalBits 原始返回值的 raw bits
/// @return 最终返回值的 raw bits（init 返回 this，其他返回 retvalBits）
extern "C" int64_t jitMethodReturn(JitContext* ctx, JitFrame* framePtr, int64_t retvalBits) {
    if (!ctx || !framePtr) {
        return retvalBits;
    }

    int64_t* methodBp = framePtr->methodBp;
    int64_t fieldCount = framePtr->fieldCount;
    const std::vector<std::string>* fieldOrder = framePtr->methodFieldOrder;

    // 1. 将方法帧内字段槽（methodBp[-1..-fieldCount]）同步回 this（methodBp[0]）
    //    JIT 栈向下增长：this 在 slot 0（methodBp[0]），field_i 在 slot i+1（methodBp[-(i+1)]）
    if (methodBp && fieldOrder && fieldCount > 0) {
        Value thisVal = bitsToValue(methodBp[0]); // 借用 this
        if (thisVal.isInstance()) {
            for (int64_t i = 0; i < fieldCount && i < static_cast<int64_t>(fieldOrder->size()); ++i) {
                Value fieldVal = bitsToValue(methodBp[-(i + 1)]);                   // 借用字段槽值
                thisVal.fields()[(*fieldOrder)[static_cast<size_t>(i)]] = fieldVal; // COW detach
            }
            methodBp[0] = valueToBits(thisVal); // 写回 this 到栈槽
        }
    }

    // 2. 确定返回值（init 调用返回 this 而非返回值）
    int64_t finalRetval = retvalBits;
    if (framePtr->isInitCall && methodBp) {
        finalRetval = methodBp[0]; // 返回 this
    }

    // 3. writeBack 到接收者（如果 receiverSlotPtr 非空）
    if (framePtr->receiverSlotPtr && methodBp) {
        Value modifiedThis = bitsToValue(methodBp[0]); // 借用修改后的 this
        if (modifiedThis.isInstance()) {
            int64_t* slotPtr = framePtr->receiverSlotPtr;
            Value receiver = bitsToValue(*slotPtr); // 借用接收者
            if (receiver.isInstance()) {
                // 同步所有字段（COW detach）
                for (const auto& field : modifiedThis.fields()) {
                    receiver.fields()[field.first] = field.second;
                }
                // R149 fix: 如果 receiver 是 caller 的 this slot（slotPtr == callerBp），
                // 必须同步 caller frame 的字段槽，否则 caller return 时字段槽（旧值）
                // 会覆盖 this.fields()（新值）。
                // 场景：Derived.init 调用 super.init(av)：
                //   1. Base.init 修改字段槽 a=10 → return 时同步到 Base.init 的 this
                //   2. writeBack 把 Base.init 的 this（a=10）同步到 Derived.init 的 this slot
                //   3. 但 Derived.init 的字段槽 a 仍是旧值 0（未更新）
                //   4. Derived.init 继续 b=bv → 字段槽 b=20
                //   5. Derived.init return 时字段槽 a=0, b=20 同步回 this → this.a=0（覆盖了 10）
                // 修复：writeBack 后同步更新 caller frame 的字段槽
                // 必须在 *slotPtr = valueToBits(receiver) detach 之前进行（detach 后 receiver 为 null）
                if (slotPtr == framePtr->callerBp && framePtr->callerBp) {
                    // receiver 是 caller 的 this slot（slot 0），同步 caller frame 的字段槽
                    // caller 帧 = framePtr - 1（JitFrame 指针算术，减 72 字节）
                    // 安全性：slotPtr == callerBp 且 callerBp 非 null → caller 是方法调用（有 this slot）
                    // → caller 帧存在（idx > 0），framePtr - 1 有效
                    JitFrame* callerFrame = framePtr - 1;
                    if (callerFrame->methodFieldOrder && callerFrame->fieldCount > 0) {
                        int64_t* callerMethodBp = framePtr->callerBp;
                        for (int64_t i = 0; i < callerFrame->fieldCount &&
                                            i < static_cast<int64_t>(callerFrame->methodFieldOrder->size());
                             ++i) {
                            const std::string& fieldName = (*callerFrame->methodFieldOrder)[static_cast<size_t>(i)];
                            auto fieldIt = receiver.fields().find(fieldName);
                            if (fieldIt != receiver.fields().end()) {
                                Value fieldCopy = fieldIt->second;                 // 拷贝（addRef）
                                callerMethodBp[-(i + 1)] = valueToBits(fieldCopy); // detach
                            }
                        }
                    }
                }
                *slotPtr = valueToBits(receiver); // 写回
            }
        }
    }

    return finalRetval;
}

/// OP_METHOD_CALL：方法调用辅助函数
/// 栈布局（调用前，向低地址增长）：
///   [argN-1]...[arg0][receiver]  ← ctx->stackTop 指向 argN-1
/// 栈布局（调用后，向低地址增长）：
///   [extraSlots...][defaults...][args...][fields...][this]  ← ctx->stackTop 指向最后一个 extraSlot
/// @param ctx JIT 上下文
/// @param methodName 方法名（C 字符串，静态生命期）
/// @param packedArgs 打包参数：
///        - bit 0-7: argCount
///        - bit 8-15: receiverLocalSlotByte（0xFF=无局部变量接收者）
///        - bit 16: isInitCall（1=init 调用，返回 this）
///        - bit 17: isSuperCall（1=super 调用，需 superClassName）
/// @param receiverSlotPtr 接收者 slot 指针（null=无 writeBack，如临时表达式）
/// @param superClassName 父类名（仅 isSuperCall=1 时有效，null=非 super 调用）
extern "C" void jitMethodCall(JitContext* ctx, const char* methodName, int64_t packedArgs, int64_t* receiverSlotPtr,
                              const char* superClassName) {
    if (!ctx || !ctx->stackTop || !methodName) {
        return;
    }

    uint8_t argCount = static_cast<uint8_t>(packedArgs & 0xFF);
    uint8_t receiverLocalSlotByte = static_cast<uint8_t>((packedArgs >> 8) & 0xFF);
    bool isInitCall = (packedArgs & (1LL << 16)) != 0;
    bool isSuperCall = (packedArgs & (1LL << 17)) != 0;

    int64_t* sp = ctx->stackTop;

    // 1. pop argCount 个参数（反向填充：栈顶是 argN-1）
    //    JIT 栈向下增长，sp += 1 是 pop（向高地址移动）
    Value args[256]; // 假设最多 256 参数
    for (int i = argCount - 1; i >= 0; --i) {
        args[i] = bitsToValue(*sp); // 借用
        sp += 1;                    // pop
    }
    // 2. pop receiver
    Value receiver = bitsToValue(*sp); // 借用
    sp += 1;                           // pop
    int64_t* callerSpAfterPop = sp;    // 调用者恢复 r15 用（pop 完 args+receiver 后的栈顶）

    if (!receiver.isInstance()) {
        if (ctx->hasError && ctx->errorBuffer) {
            *ctx->hasError = true;
            *ctx->errorBuffer = "类型 " + receiver.typeName() + " 不支持方法 " + std::string(methodName);
        }
        ctx->stackTop = callerSpAfterPop; // 更新栈顶（已 pop args + receiver）
        return;
    }

    // 3. 沿继承链查找方法
    //    super 调用：从父类开始查找
    //    普通调用：从实例类开始查找
    std::string searchClassName;
    if (isSuperCall && superClassName) {
        // super 调用：通过编译时编码的父类名查找
        std::string superName(superClassName);
        auto clsIt = ctx->classInfoPtr->find(superName);
        if (clsIt == ctx->classInfoPtr->end() || clsIt->second.superClassName.empty()) {
            if (ctx->hasError && ctx->errorBuffer) {
                *ctx->hasError = true;
                *ctx->errorBuffer = "类 " + superName + " 没有父类，不能使用 super";
            }
            ctx->stackTop = callerSpAfterPop;
            return;
        }
        searchClassName = clsIt->second.superClassName;
    } else {
        searchClassName = receiver.className();
    }

    const JitMethodInfo* foundMethod = nullptr;
    for (int guard = 0; guard < 64 && !searchClassName.empty(); ++guard) {
        std::string key = searchClassName + "." + methodName;
        auto it = ctx->methodEntriesPtr->find(key);
        if (it != ctx->methodEntriesPtr->end()) {
            foundMethod = &it->second;
            break;
        }
        // 查找父类
        auto clsIt = ctx->classInfoPtr->find(searchClassName);
        if (clsIt == ctx->classInfoPtr->end())
            break;
        searchClassName = clsIt->second.superClassName;
    }

    if (!foundMethod) {
        if (ctx->hasError && ctx->errorBuffer) {
            *ctx->hasError = true;
            *ctx->errorBuffer = "类 " + receiver.className() + " 没有方法 " + std::string(methodName);
        }
        ctx->stackTop = callerSpAfterPop;
        return;
    }

    // R157: lazy compilation — entryPtr 为 null 时触发按需编译
    // lazyMode_ 下 compileAllChunks 将方法 chunk 的 entryPtr 置空，
    // 首次方法调用时通过 backendPtr 回调 triggerLazyCompile 编译该 chunk。
    if (foundMethod->entryPtr == nullptr) {
        auto* backend = static_cast<JITBackend*>(ctx->backendPtr);
        if (backend) {
            // 用完整 key（"Class.method"）触发 lazy compilation
            std::string fullKey = searchClassName + "." + methodName;
            JitEntryFn lazyEntry = backend->triggerLazyCompile(fullKey);
            if (lazyEntry) {
                // 编译成功，重新查找（entryPtr 已更新）
                std::string rekey = searchClassName + "." + methodName;
                auto reIt = ctx->methodEntriesPtr->find(rekey);
                if (reIt != ctx->methodEntriesPtr->end()) {
                    foundMethod = &reIt->second;
                }
            }
        }
        if (foundMethod->entryPtr == nullptr) {
            if (ctx->hasError && ctx->errorBuffer) {
                *ctx->hasError = true;
                *ctx->errorBuffer = "方法 " + std::string(methodName) + " 入口未绑定（lazy compilation 失败）";
            }
            ctx->stackTop = callerSpAfterPop;
            return;
        }
    }

    // 4. 参数校验 + 默认参数填充
    if (argCount < static_cast<uint8_t>(foundMethod->requiredArity) ||
        argCount > static_cast<uint8_t>(foundMethod->arity)) {
        if (ctx->hasError && ctx->errorBuffer) {
            *ctx->hasError = true;
            *ctx->errorBuffer = "方法 " + std::string(methodName) + " 期望 " +
                                std::to_string(foundMethod->requiredArity) + "-" + std::to_string(foundMethod->arity) +
                                " 个参数，但传入了 " + std::to_string(argCount) + " 个";
        }
        ctx->stackTop = callerSpAfterPop;
        return;
    }

    Value defaults[64];
    int defaultCount = 0;
    if (argCount < static_cast<uint8_t>(foundMethod->arity)) {
        int missingCount = foundMethod->arity - argCount;
        int defaultStartIdx = static_cast<int>(foundMethod->defaultConstIndices->size()) - missingCount;
        if (defaultStartIdx < 0 ||
            static_cast<size_t>(defaultStartIdx + missingCount) > foundMethod->defaultConstIndices->size()) {
            if (ctx->hasError && ctx->errorBuffer) {
                *ctx->hasError = true;
                *ctx->errorBuffer = "方法 " + std::string(methodName) + " 默认参数索引越界";
            }
            ctx->stackTop = callerSpAfterPop;
            return;
        }
        for (int i = defaultStartIdx; i < defaultStartIdx + missingCount; ++i) {
            uint16_t constIdx = (*foundMethod->defaultConstIndices)[static_cast<size_t>(i)];
            if (constIdx == 0xFFFF || constIdx >= foundMethod->constants->size()) {
                if (ctx->hasError && ctx->errorBuffer) {
                    *ctx->hasError = true;
                    *ctx->errorBuffer = "方法 " + std::string(methodName) + " 默认参数常量索引越界";
                }
                ctx->stackTop = callerSpAfterPop;
                return;
            }
            defaults[defaultCount++] = (*foundMethod->constants)[constIdx];
        }
    }

    // 5. 构造新帧：push this + push fields + push args + push defaults + push extraSlots
    //    JIT 栈向下增长：push = sp -= 1; *sp = valueToBits(val)

    // R149 fix: valueToBits 会 detach（转移所有权置 v 为 null），所以 push this 后
    // 不能再从 receiver 读 fields。修复：先拷贝 receiver 用于 push this（detach 拷贝），
    // 原 receiver 保留用于读 fields，函数结束时自动析构。
    Value receiverForPush = receiver; // 拷贝（addRef）
    sp -= 1;
    *sp = valueToBits(receiverForPush); // detach receiverForPush，原 receiver 仍有效

    // push fields（按方法 chunk 的 fieldOrder 顺序，从 receiver.fields 读取）
    int fieldCount = 0;
    if (foundMethod->fieldOrder && !foundMethod->fieldOrder->empty()) {
        // 借用原 receiver 读取字段（不修改）
        const Value& receiverConst = receiver;
        for (const auto& fieldName : *foundMethod->fieldOrder) {
            auto fieldIt = receiverConst.fields().find(fieldName);
            sp -= 1;
            if (fieldIt != receiverConst.fields().end()) {
                Value fieldCopy = fieldIt->second; // 拷贝（addRef），valueToBits 需要 non-const
                *sp = valueToBits(fieldCopy);      // detach 字段值副本
            } else {
                *sp = static_cast<int64_t>(JIT_NULL_BITS);
            }
        }
        fieldCount = static_cast<int>(foundMethod->fieldOrder->size());
    }

    // push args
    for (int i = 0; i < argCount; ++i) {
        sp -= 1;
        *sp = valueToBits(args[i]); // detach
    }
    // push defaults
    for (int i = 0; i < defaultCount; ++i) {
        sp -= 1;
        *sp = valueToBits(defaults[i]); // detach
    }

    // push extraSlots (null)
    int preAllocated = 1 + fieldCount + foundMethod->arity;
    int extraSlots = foundMethod->localCount - preAllocated;
    for (int i = 0; i < extraSlots; ++i) {
        sp -= 1;
        *sp = static_cast<int64_t>(JIT_NULL_BITS);
    }

    // 6. 设置 JitFrame
    int64_t* callerBp = ctx->callerBp; // JIT 代码在调用前存入
    size_t idx = *ctx->frameCount;
    if (idx >= RuntimeLimits::MAX_FRAMES) {
        if (ctx->hasError && ctx->errorBuffer) {
            *ctx->hasError = true;
            *ctx->errorBuffer = ErrorFormat::format(ErrorMessages::kRecursionDepthExceededFmt,
                                                    static_cast<int>(RuntimeLimits::MAX_FRAMES));
        }
        ctx->stackTop = sp;
        return;
    }
    JitFrame* frame = &ctx->frames[idx];
    frame->callerBp = callerBp;
    frame->callerSp = callerSpAfterPop; // 调用者恢复 r15 用
    // returnAddr 由 JIT 代码在调用后设置（jitMethodCall 无法访问 JIT Label）
    frame->receiverSlotPtr = receiverSlotPtr;
    frame->methodFieldOrder = foundMethod->fieldOrder;
    // methodBp = sp + (localCount - 1)（this 在 slot 0 = 最高地址）
    frame->methodBp = sp + (foundMethod->localCount - 1);
    frame->isMethodCall = 1;
    frame->isInitCall = isInitCall ? 1 : 0;
    frame->fieldCount = fieldCount;

    // *frameCount += 1
    *ctx->frameCount = idx + 1;

    // R156: 推入空 upvalues（方法调用不携带闭包 upvalues，但需保持 frameUpvaluesStack_ 对齐）
    {
        auto* backend = static_cast<JITBackend*>(ctx->backendPtr);
        if (backend) {
            if (backend->frameUpvaluesStack_.size() <= idx) {
                backend->frameUpvaluesStack_.resize(idx + 1);
            }
            backend->frameUpvaluesStack_[idx].clear();
        }
    }

    // 7. 通过 ctx 邮箱返回方法入口地址 + localCount
    ctx->methodEntryPtr = foundMethod->entryPtr;
    ctx->methodLocalCount = foundMethod->localCount;

    // 8. 更新栈顶
    ctx->stackTop = sp;
}

// ============================================================
// R151: jitTriggerRecompile — 热点阈值重编译回调
// ============================================================
// 调用此函数表示 chunkIdx 对应的 chunk 调用计数已达到阈值。
// JIT 代码在调用前已设置 recompiledFlags[chunkIdx] = 1，避免重复触发。
//
// 当前 PoC 阶段（R151）：仅验证热点检测机制正确工作，不执行实际重编译。
// 未来（R152+）：可在此处重新编译该 chunk，使用更激进的优化策略
// （如跳过 emitCheckInt 类型检查、循环展开等），并更新 methodEntries_ 中的 entryPtr。
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
extern "C" void jitReportStackOverflow(JitContext* ctx) {
    if (!ctx) {
        return;
    }
    if (ctx->hasError && ctx->errorBuffer) {
        *ctx->hasError = true;
        *ctx->errorBuffer =
            ErrorFormat::format(ErrorMessages::kRecursionDepthExceededFmt, static_cast<int>(RuntimeLimits::MAX_FRAMES));
    }
}

// ============================================================
// R157: jitCallByName — 命名函数调用（lazy compilation 触发点）
// ============================================================
// 与 jitCallExpr 对齐，但通过函数名（而非闭包值）查找入口。
// 栈布局（调用前）: [..., arg0, arg1, ..., argN-1]（无 closure，参数直接在栈顶）
// 栈布局（调用后）: 新帧栈 [..., arg0, arg1, ..., argN-1, defaults..., extraSlots...]
//
// lazy compilation: entryPtr 为 null 时，通过 backendPtr 调用 triggerLazyCompile
// 编译单个 chunk 到独立 CodeHolder，编译完成后 entryPtr 更新。
extern "C" void jitCallByName(JitContext* ctx, const char* funName, uint8_t argCount) {
    if (!ctx || !ctx->stackTop || !funName) {
        return;
    }

    int64_t* sp = ctx->stackTop;

    // 1. pop args（借用，不 detach）
    Value args[256];
    for (int i = argCount - 1; i >= 0; --i) {
        args[i] = bitsToValue(*sp); // 借用
        sp += 1;                    // pop
    }
    int64_t* callerSpAfterPop = sp; // 调用者恢复 r15 用

    // 2. 从 funcEntries_ 查找 JIT 入口地址 + localCount
    if (!ctx->funcEntriesPtr) {
        if (ctx->hasError && ctx->errorBuffer) {
            *ctx->hasError = true;
            *ctx->errorBuffer = "JIT 内部错误: funcEntriesPtr 为空";
        }
        ctx->stackTop = callerSpAfterPop;
        return;
    }
    std::string name(funName);
    auto it = ctx->funcEntriesPtr->find(name);

    // 3. lazy compilation: entryPtr 为 null 时触发编译
    if (it == ctx->funcEntriesPtr->end() || it->second.entryPtr == nullptr) {
        auto* backend = static_cast<JITBackend*>(ctx->backendPtr);
        if (backend) {
            JitEntryFn lazyEntry = backend->triggerLazyCompile(name);
            if (lazyEntry) {
                // 编译成功，重新查找（entryPtr 已更新）
                it = ctx->funcEntriesPtr->find(name);
            }
        }
    }

    if (it == ctx->funcEntriesPtr->end()) {
        if (ctx->hasError && ctx->errorBuffer) {
            *ctx->hasError = true;
            *ctx->errorBuffer = "未找到函数: " + name;
        }
        ctx->stackTop = callerSpAfterPop;
        return;
    }
    const JitMethodInfo* foundFunc = &it->second;

    if (foundFunc->entryPtr == nullptr) {
        if (ctx->hasError && ctx->errorBuffer) {
            *ctx->hasError = true;
            *ctx->errorBuffer = "函数 " + name + " 入口未绑定（lazy compilation 失败）";
        }
        ctx->stackTop = callerSpAfterPop;
        return;
    }

    // 4. 参数校验
    if (argCount < static_cast<uint8_t>(foundFunc->requiredArity) ||
        argCount > static_cast<uint8_t>(foundFunc->arity)) {
        if (ctx->hasError && ctx->errorBuffer) {
            *ctx->hasError = true;
            *ctx->errorBuffer = "函数 " + name + " 期望 " + std::to_string(foundFunc->requiredArity) + "-" +
                                std::to_string(foundFunc->arity) + " 个参数，但传入了 " + std::to_string(argCount) +
                                " 个";
        }
        ctx->stackTop = callerSpAfterPop;
        return;
    }

    // 5. 默认参数填充
    Value defaults[64];
    int defaultCount = 0;
    if (argCount < static_cast<uint8_t>(foundFunc->arity)) {
        int missingCount = foundFunc->arity - argCount;
        int defaultStartIdx = static_cast<int>(foundFunc->defaultConstIndices->size()) - missingCount;
        if (defaultStartIdx < 0 ||
            static_cast<size_t>(defaultStartIdx + missingCount) > foundFunc->defaultConstIndices->size()) {
            if (ctx->hasError && ctx->errorBuffer) {
                *ctx->hasError = true;
                *ctx->errorBuffer = "函数 " + name + " 默认参数索引越界";
            }
            ctx->stackTop = callerSpAfterPop;
            return;
        }
        for (int i = defaultStartIdx; i < defaultStartIdx + missingCount; ++i) {
            uint16_t constIdx = (*foundFunc->defaultConstIndices)[static_cast<size_t>(i)];
            if (constIdx == 0xFFFF || constIdx >= foundFunc->constants->size()) {
                if (ctx->hasError && ctx->errorBuffer) {
                    *ctx->hasError = true;
                    *ctx->errorBuffer = "函数 " + name + " 默认参数常量索引越界";
                }
                ctx->stackTop = callerSpAfterPop;
                return;
            }
            defaults[defaultCount++] = (*foundFunc->constants)[constIdx];
        }
    }

    // 6. 构造新帧栈：push args + push defaults + push extraSlots
    // push args
    for (int i = 0; i < argCount; ++i) {
        sp -= 1;
        *sp = valueToBits(args[i]); // detach
    }
    // push defaults
    for (int i = 0; i < defaultCount; ++i) {
        sp -= 1;
        *sp = valueToBits(defaults[i]); // detach
    }
    // push extraSlots (null)
    int preAllocated = foundFunc->arity;
    int extraSlots = foundFunc->localCount - preAllocated;
    for (int i = 0; i < extraSlots; ++i) {
        sp -= 1;
        *sp = static_cast<int64_t>(JIT_NULL_BITS);
    }

    // 7. 构造 JitFrame
    int64_t* callerBp = ctx->callerBp; // JIT 代码在调用前存入 r13
    size_t idx = *ctx->frameCount;
    if (idx >= RuntimeLimits::MAX_FRAMES) {
        if (ctx->hasError && ctx->errorBuffer) {
            *ctx->hasError = true;
            *ctx->errorBuffer = ErrorFormat::format(ErrorMessages::kRecursionDepthExceededFmt,
                                                    static_cast<int>(RuntimeLimits::MAX_FRAMES));
        }
        ctx->stackTop = sp;
        return;
    }
    JitFrame* frame = &ctx->frames[idx];
    frame->callerBp = callerBp;
    frame->callerSp = callerSpAfterPop;
    frame->receiverSlotPtr = nullptr; // 非方法调用，无 writeBack
    frame->methodFieldOrder = nullptr;
    frame->methodBp = nullptr;
    frame->isMethodCall = 0;
    frame->isInitCall = 0;
    frame->fieldCount = 0;

    *ctx->frameCount = idx + 1;

    // R156: 推入闭包 upvalues（OP_CALL 命名函数调用从 functionClosures_ 查找）
    auto* backend = static_cast<JITBackend*>(ctx->backendPtr);
    if (backend) {
        if (backend->frameUpvaluesStack_.size() <= idx) {
            backend->frameUpvaluesStack_.resize(idx + 1);
        }
        // 检查是否有注册的闭包值（OP_CLOSURE 创建）
        auto closureIt = backend->functionClosures_.find(name);
        if (closureIt != backend->functionClosures_.end() && closureIt->second.vmClosure()) {
            backend->frameUpvaluesStack_[idx] = closureIt->second.vmClosure()->upvalues;
        } else {
            backend->frameUpvaluesStack_[idx].clear();
        }
    }

    // 8. 通过邮箱返回函数入口地址 + localCount
    ctx->methodEntryPtr = foundFunc->entryPtr;
    ctx->methodLocalCount = foundFunc->localCount;

    // 9. 更新栈顶
    ctx->stackTop = sp;
}

// ============================================================
// R157: jitTriggerOsrRecompile — 循环回边 OSR 特化重编译回调
// ============================================================
// OP_LOOP 回边计数超阈值时调用。通过 backendPtr 调用 triggerOsrRecompile，
// 根据类型反馈对当前 chunk 特化重编译（INT 或 FLOAT），更新 methodEntries_/funcEntries_。
// osrRecompiledFlags[chunkIdx] 已由 JIT 代码设置为 1，避免重复触发。
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

} // extern "C"

// ============================================================
// JITBackend 实现
// ============================================================

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
    hasError_ = true;
    lastError_ = msg;
    diagnostics_.addError(msg, 0, 0, DiagSource::VM);
}

void JITBackend::compileError(const std::string& msg) {
    hasError_ = true;
    lastError_ = msg;
    diagnostics_.addError(msg, 0, 0, DiagSource::Compiler);
}

// ============================================================
// R156: closeUpvaluesFrom — 关闭 open upvalues（JIT 栈向下增长版本）
// ============================================================
// 与 StackVM closeUpvaluesFrom(fromSlot) 语义对齐但方向反转：
// StackVM 栈向上增长，关闭 stackSlot >= fromSlot（升序 multimap 的 lower_bound 到 end）；
// JIT 栈向下增长，关闭 address <= fromAddr（升序 multimap 的 begin 到 upper_bound）。
// 关闭操作：读取栈槽当前值快照到 uv->value，标记 isClosed=true，从 multimap erase。
void JITBackend::closeUpvaluesFrom(int64_t* fromAddr) {
    // 升序 multimap（地址从小到大）：upper_bound(fromAddr) 找到第一个 > fromAddr 的条目，
    // [begin, upper_bound) 范围内所有条目都 <= fromAddr（JIT 栈向下增长，当前帧 slot 地址 <= r13）。
    // 等价于 StackVM 的 closeUpvaluesFrom(fromSlot) 关闭 [fromSlot, ∞)（StackVM 栈向上增长）。
    auto endIt = openUpvalues_.upper_bound(fromAddr);
    auto it = openUpvalues_.begin();
    while (it != endIt) {
        if (auto uv = it->second.lock()) {
            if (!uv->isClosed) {
                // 快照当前栈槽值到 uv->value
                // uv->stackSlot 存储的是 int64_t* 指针（reinterpret_cast 自 size_t）
                auto* slotPtr = reinterpret_cast<int64_t*>(uv->stackSlot);
                uv->value = bitsToValue(*slotPtr);
                uv->isClosed = true;
            }
        }
        // erase 返回下一个迭代器；endIt 不受 erase 影响（std::multimap 语义保证）
        it = openUpvalues_.erase(it);
    }
}

// ============================================================
// compileAllChunks: 多 chunk 编译到单个 CodeHolder（R141 重写）
// ============================================================
// 阶段 2b：编译 mainChunk + 所有 functionChunks 到一个 CodeHolder。
// 每个 functionChunk 绑定入口 Label，OP_CALL 通过 jmp targetLabel 实现调用。
// 函数调用帧栈存储在 JitContext.frames（offset 32），由 JIT 代码运行时维护。
JitEntryFn JITBackend::compileAllChunks(const CompileResult& result) {
    using namespace asmjit;

    CodeHolder code;
    Error err = code.init(runtime_.environment());
    if (err != kErrorOk) {
        compileError(std::string("asmjit CodeHolder::init 失败: ") + DebugUtils::error_as_string(err));
        return nullptr;
    }

    x86::Assembler a(&code);

    // R160: 统计所有 chunk 的 OP_MEMBER_GET 总数，预分配 inline cache 数组
    // callSiteId 跨所有 chunk 统一编号，运行时通过 id 索引 memberGetIC_
    uint64_t nextCallSiteId = 0;
    auto countMemberGet = [&nextCallSiteId](const BytecodeChunk& chunk) {
        for (size_t i = 0; i < chunk.code.size();) {
            uint8_t op = chunk.code[i];
            if (op == static_cast<uint8_t>(OpCode::OP_MEMBER_GET)) {
                nextCallSiteId++;
            }
            i += chunk.instructionSizeAt(i);
        }
    };
    countMemberGet(result.mainChunk);
    for (const auto& [name, fc] : result.functionChunks) {
        countMemberGet(fc);
    }
    memberGetIC_.assign(nextCallSiteId > 0 ? nextCallSiteId : 1, MemberGetInlineCacheEntry{});
    nextCallSiteId = 0; // 重置，编译时按顺序分配

    // ---- 函数 prologue ----
    // Windows x64 ABI: 进入时 rsp = 16k+8（call 压入返回地址）
    // push rbp → rsp = 16k（16 字节对齐）
    a.push(x86::rbp);
    a.mov(x86::rbp, x86::rsp);
    // R161 fixup: 栈空间扩大到 48B（callee-saved: r12/r13/r14/r15/rbx + 8B 对齐填充）
    // + 8KB（操作数栈 1024 个 int64），共 8240 字节，保持 16 字节对齐。
    // 原 32B 仅保存 r12-r15，遗漏 rbx（OP_RETURN 路径使用 rbx 7 处但未保存，ABI 违规）。
    a.sub(x86::rsp, 48 + 8192);

    // 保存 callee-saved 寄存器到 [rbp-8..rbp-40]
    a.mov(x86::qword_ptr(x86::rbp, -8), x86::r12);
    a.mov(x86::qword_ptr(x86::rbp, -16), x86::r13);
    a.mov(x86::qword_ptr(x86::rbp, -24), x86::r14);
    a.mov(x86::qword_ptr(x86::rbp, -32), x86::r15);
    a.mov(x86::qword_ptr(x86::rbp, -40), x86::rbx);

    // 初始化 r12 = JitContext* ctx
#ifdef _WIN32
    a.mov(x86::r12, x86::rcx);
#else
    a.mov(x86::r12, x86::rdi);
#endif

    // 初始化 r15 = operand stack top（向低地址增长）
    // 空栈时 r15 指向栈基址 [rbp-48]（48 = 5 个 callee-saved × 8 + 8B 对齐填充），
    // push: sub r15, 8; mov [r15], rax; pop: mov rax, [r15]; add r15, 8
    a.lea(x86::r15, x86::qword_ptr(x86::rbp, -48));

    // R141: 初始化 r13 = 0（mainChunk 无参数，basePointer 无意义）
    a.xor_(x86::r13, x86::r13);

    // R161 perf: 用 callee-saved 寄存器缓存算术运算热路径常量，消除 6 处 OP_ADD/SUB/MUL/DIV/MOD/NEGATE
    // 各 2 条 movabs（共 12 条 movabs = 120 字节代码）。rbx/r14 仅在 OP_RETURN 瞬态使用，
    // OP_RETURN 入口保存 rbx 到 [rbp-48] 暂存槽、出口恢复；r14 returnAddr 改用 rdx（已死值）。
    // rbx = JIT_INT48_MASK（and 操作掩码），r14 = JIT_INT_TAG_BASE（or 操作 tag 基址）
    a.movabs(x86::rbx, JIT_INT48_MASK);
    a.movabs(x86::r14, JIT_INT_TAG_BASE);

    // 用于 OP_RETURN 末帧返回的 epilogue 标签
    Label epilogue = a.new_label();

    // ---- R143 阶段 3b：C++ 辅助函数调用代码生成 lambda ----
    // 生成"peek 栈顶两个值 → 设置参数 → 调用辅助函数 → pop 两个 → push 结果"的机器码
    // @param fnPtr 辅助函数指针（签名: uint64_t fn(uint64_t left, uint64_t right) 或
    //              uint64_t fn(JitContext* ctx, uint64_t left, uint64_t right)）
    // @param needCtx 是否传递 JitContext* 作为第一个参数（用于除法/取模的错误报告）
    auto emitCallBinaryHelper = [&a, &epilogue](void* fnPtr, bool needCtx, bool checkError) {
        // 设置参数
        if (needCtx) {
#ifdef _WIN32
            a.mov(x86::rcx, x86::r12);                    // arg1 = ctx
            a.mov(x86::rdx, x86::qword_ptr(x86::r15, 8)); // arg2 = left
            a.mov(x86::r8, x86::qword_ptr(x86::r15));     // arg3 = right
#else
            a.mov(x86::rdi, x86::r12);
            a.mov(x86::rsi, x86::qword_ptr(x86::r15, 8));
            a.mov(x86::rdx, x86::qword_ptr(x86::r15));
#endif
        } else {
#ifdef _WIN32
            a.mov(x86::rcx, x86::qword_ptr(x86::r15, 8)); // arg1 = left
            a.mov(x86::rdx, x86::qword_ptr(x86::r15));    // arg2 = right
#else
            a.mov(x86::rdi, x86::qword_ptr(x86::r15, 8));
            a.mov(x86::rsi, x86::qword_ptr(x86::r15));
#endif
        }
        // 调用辅助函数（r15/r12 是 callee-saved，安全）
#ifdef _WIN32
        a.sub(x86::rsp, 32);
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(fnPtr));
        a.call(x86::rax);
        a.add(x86::rsp, 32);
#else
        a.sub(x86::rsp, 8);
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(fnPtr));
        a.call(x86::rax);
        a.add(x86::rsp, 8);
#endif
        // pop 两个操作数
        a.add(x86::r15, 16);
        // push 结果（rax = raw bits）
        a.sub(x86::r15, 8);
        a.mov(x86::qword_ptr(x86::r15), x86::rax);
        // 可选：检查错误标志（除法/取模的除零检查）
        if (checkError) {
            // R143 修复：hasError 是 bool（1 字节），不能用 mov rax,[rax] 读取 8 字节，
            // 否则会读到 bool 之后的 padding/垃圾数据，误判为有错误导致提前跳转 epilogue。
            // 必须用 movzx 读取 1 字节并零扩展到 64 位。
            a.mov(x86::rax, x86::qword_ptr(x86::r12, 8)); // rax = hasError 指针 (bool*)
            a.movzx(x86::rax, x86::byte_ptr(x86::rax));   // rax = *hasError (1 字节零扩展)
            a.test(x86::rax, x86::rax);
            a.jnz(epilogue);
        }
    };

    // ---- R143 阶段 3b：一元运算 C++ 辅助函数调用 lambda ----
    // 生成"peek 栈顶值 → 设置参数 → 调用辅助函数 → pop → push 结果"的机器码
    auto emitCallUnaryHelper = [&a](void* fnPtr) {
#ifdef _WIN32
        a.mov(x86::rcx, x86::qword_ptr(x86::r15)); // arg1 = value
#else
        a.mov(x86::rdi, x86::qword_ptr(x86::r15));
#endif
#ifdef _WIN32
        a.sub(x86::rsp, 32);
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(fnPtr));
        a.call(x86::rax);
        a.add(x86::rsp, 32);
#else
        a.sub(x86::rsp, 8);
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(fnPtr));
        a.call(x86::rax);
        a.add(x86::rsp, 8);
#endif
        // pop 操作数
        a.add(x86::r15, 8);
        // push 结果
        a.sub(x86::r15, 8);
        a.mov(x86::qword_ptr(x86::r15), x86::rax);
    };

    // ---- R143 阶段 3b：有序比较 C++ 辅助函数调用 lambda（三参数） ----
    // 生成"peek 栈顶两个值 → 设置 (left, right, cmpType) 参数 → 调用 → pop 两个 → push 结果"的机器码
    // 签名: uint64_t jitOrderedCompare(uint64_t left, uint64_t right, int64_t cmpType)
    auto emitCallOrderedCompare = [&a](void* fnPtr, int64_t cmpType) {
#ifdef _WIN32
        a.mov(x86::rcx, x86::qword_ptr(x86::r15, 8));  // arg1 = left
        a.mov(x86::rdx, x86::qword_ptr(x86::r15));     // arg2 = right
        a.mov(x86::r8, static_cast<int32_t>(cmpType)); // arg3 = cmpType
#else
        a.mov(x86::rdi, x86::qword_ptr(x86::r15, 8));
        a.mov(x86::rsi, x86::qword_ptr(x86::r15));
        a.mov(x86::rdx, static_cast<int32_t>(cmpType));
#endif
#ifdef _WIN32
        a.sub(x86::rsp, 32);
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(fnPtr));
        a.call(x86::rax);
        a.add(x86::rsp, 32);
#else
        a.sub(x86::rsp, 8);
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(fnPtr));
        a.call(x86::rax);
        a.add(x86::rsp, 8);
#endif
        // pop 两个操作数
        a.add(x86::r15, 16);
        // push 结果
        a.sub(x86::r15, 8);
        a.mov(x86::qword_ptr(x86::r15), x86::rax);
    };

    // ---- R146 阶段 4：数组辅助函数调用 lambda ----
    // 数组辅助函数通过 ctx->stackTop (offset 48) 访问 JIT 栈。
    // 调用前 JIT 代码更新 ctx->stackTop = r15，调用后从 ctx->stackTop 恢复 r15。
    // 这是因为数组操作（OP_BUILD_ARRAY）涉及动态 count 个元素，无法用固定参数列表。

    // OP_BUILD_ARRAY: void jitBuildArray(JitContext* ctx, uint8_t count)
    // 辅助函数内部 pop count 个 + push 1 个，通过 ctx->stackTop 操作栈
    auto emitBuildArray = [&a, &epilogue](uint8_t count) {
        // 更新 ctx->stackTop = r15
        a.mov(x86::qword_ptr(x86::r12, 48), x86::r15);
        // 设置参数
#ifdef _WIN32
        a.mov(x86::rcx, x86::r12);                    // arg1 = ctx
        a.mov(x86::rdx, static_cast<int32_t>(count)); // arg2 = count
#else
        a.mov(x86::rdi, x86::r12);
        a.mov(x86::rsi, static_cast<int32_t>(count));
#endif
#ifdef _WIN32
        a.sub(x86::rsp, 32);
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitBuildArray));
        a.call(x86::rax);
        a.add(x86::rsp, 32);
#else
        a.sub(x86::rsp, 8);
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitBuildArray));
        a.call(x86::rax);
        a.add(x86::rsp, 8);
#endif
        // 恢复 r15 = ctx->stackTop（辅助函数可能修改了栈顶）
        a.mov(x86::r15, x86::qword_ptr(x86::r12, 48));
        // 检查错误标志
        a.mov(x86::rax, x86::qword_ptr(x86::r12, 8)); // rax = hasError 指针
        a.movzx(x86::rax, x86::byte_ptr(x86::rax));
        a.test(x86::rax, x86::rax);
        a.jnz(epilogue);
    };

    // OP_INDEX_GET: uint64_t jitIndexGet(JitContext* ctx, uint64_t objBits, uint64_t idxBits)
    // 栈布局：[obj, idx]（idx 在栈顶），辅助函数 pop 2 + push 1
    auto emitIndexGet = [&a, &epilogue]() {
        a.mov(x86::qword_ptr(x86::r12, 48), x86::r15);
#ifdef _WIN32
        a.mov(x86::rcx, x86::r12);                    // arg1 = ctx
        a.mov(x86::rdx, x86::qword_ptr(x86::r15, 8)); // arg2 = obj
        a.mov(x86::r8, x86::qword_ptr(x86::r15));     // arg3 = idx
#else
        a.mov(x86::rdi, x86::r12);
        a.mov(x86::rsi, x86::qword_ptr(x86::r15, 8));
        a.mov(x86::rdx, x86::qword_ptr(x86::r15));
#endif
#ifdef _WIN32
        a.sub(x86::rsp, 32);
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitIndexGet));
        a.call(x86::rax);
        a.add(x86::rsp, 32);
#else
        a.sub(x86::rsp, 8);
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitIndexGet));
        a.call(x86::rax);
        a.add(x86::rsp, 8);
#endif
        // 恢复 r15 + pop 2 + push 1（辅助函数未操作栈，JIT 代码自己处理）
        a.add(x86::r15, 16);
        a.sub(x86::r15, 8);
        a.mov(x86::qword_ptr(x86::r15), x86::rax);
        // 检查错误标志
        a.mov(x86::rax, x86::qword_ptr(x86::r12, 8));
        a.movzx(x86::rax, x86::byte_ptr(x86::rax));
        a.test(x86::rax, x86::rax);
        a.jnz(epilogue);
    };

    // OP_INDEX_SET_LOCAL: void jitIndexSetLocal(JitContext* ctx, uint8_t slot, int64_t* frameBase)
    // 栈布局：[idx, val]（val 在栈顶），辅助函数 pop 2，不 push
    // frameBase = r13（当前帧 basePointer）
    auto emitIndexSetLocal = [&a, &epilogue](uint8_t slot) {
        a.mov(x86::qword_ptr(x86::r12, 48), x86::r15);
#ifdef _WIN32
        a.mov(x86::rcx, x86::r12);                   // arg1 = ctx
        a.mov(x86::rdx, static_cast<int32_t>(slot)); // arg2 = slot
        a.mov(x86::r8, x86::r13);                    // arg3 = frameBase (r13)
#else
        a.mov(x86::rdi, x86::r12);
        a.mov(x86::rsi, static_cast<int32_t>(slot));
        a.mov(x86::rdx, x86::r13);
#endif
#ifdef _WIN32
        a.sub(x86::rsp, 32);
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitIndexSetLocal));
        a.call(x86::rax);
        a.add(x86::rsp, 32);
#else
        a.sub(x86::rsp, 8);
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitIndexSetLocal));
        a.call(x86::rax);
        a.add(x86::rsp, 8);
#endif
        // 恢复 r15 = ctx->stackTop（辅助函数 pop 了 2 个元素）
        a.mov(x86::r15, x86::qword_ptr(x86::r12, 48));
        // 检查错误标志
        a.mov(x86::rax, x86::qword_ptr(x86::r12, 8));
        a.movzx(x86::rax, x86::byte_ptr(x86::rax));
        a.test(x86::rax, x86::rax);
        a.jnz(epilogue);
    };

    // ---- R147 阶段 4b：dict/tuple + OP_INDEX_SET_VAR 辅助 lambda ----
    // OP_BUILD_DICT: void jitBuildDict(JitContext* ctx, uint8_t pairCount)
    // 辅助函数内部 pop 2*pairCount 个 + push 1 个，通过 ctx->stackTop 操作栈
    auto emitBuildDict = [&a, &epilogue](uint8_t pairCount) {
        a.mov(x86::qword_ptr(x86::r12, 48), x86::r15);
#ifdef _WIN32
        a.mov(x86::rcx, x86::r12);                        // arg1 = ctx
        a.mov(x86::rdx, static_cast<int32_t>(pairCount)); // arg2 = pairCount
#else
        a.mov(x86::rdi, x86::r12);
        a.mov(x86::rsi, static_cast<int32_t>(pairCount));
#endif
#ifdef _WIN32
        a.sub(x86::rsp, 32);
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitBuildDict));
        a.call(x86::rax);
        a.add(x86::rsp, 32);
#else
        a.sub(x86::rsp, 8);
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitBuildDict));
        a.call(x86::rax);
        a.add(x86::rsp, 8);
#endif
        a.mov(x86::r15, x86::qword_ptr(x86::r12, 48));
        a.mov(x86::rax, x86::qword_ptr(x86::r12, 8));
        a.movzx(x86::rax, x86::byte_ptr(x86::rax));
        a.test(x86::rax, x86::rax);
        a.jnz(epilogue);
    };

    // OP_BUILD_TUPLE: void jitBuildTuple(JitContext* ctx, uint8_t count)
    // 与 emitBuildArray 等价，只是调用 jitBuildTuple
    auto emitBuildTuple = [&a, &epilogue](uint8_t count) {
        a.mov(x86::qword_ptr(x86::r12, 48), x86::r15);
#ifdef _WIN32
        a.mov(x86::rcx, x86::r12);
        a.mov(x86::rdx, static_cast<int32_t>(count));
#else
        a.mov(x86::rdi, x86::r12);
        a.mov(x86::rsi, static_cast<int32_t>(count));
#endif
#ifdef _WIN32
        a.sub(x86::rsp, 32);
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitBuildTuple));
        a.call(x86::rax);
        a.add(x86::rsp, 32);
#else
        a.sub(x86::rsp, 8);
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitBuildTuple));
        a.call(x86::rax);
        a.add(x86::rsp, 8);
#endif
        a.mov(x86::r15, x86::qword_ptr(x86::r12, 48));
        a.mov(x86::rax, x86::qword_ptr(x86::r12, 8));
        a.movzx(x86::rax, x86::byte_ptr(x86::rax));
        a.test(x86::rax, x86::rax);
        a.jnz(epilogue);
    };

    // OP_INDEX_SET_VAR: void jitIndexSetGlobal(JitContext* ctx, int64_t* slotPtr)
    // slotPtr = globalSlots + slot*8（编译期解析 nameIdx→slot，运行时计算地址）
    auto emitIndexSetGlobal = [&a, &epilogue](int slot) {
        a.mov(x86::qword_ptr(x86::r12, 48), x86::r15);
#ifdef _WIN32
        a.mov(x86::rcx, x86::r12); // arg1 = ctx
        // arg2 = slotPtr = [r12+24]（globalSlots） + slot*8
        a.mov(x86::rdx, x86::qword_ptr(x86::r12, 24));
        a.lea(x86::rdx, x86::qword_ptr(x86::rdx, static_cast<int32_t>(slot) * 8));
#else
        a.mov(x86::rdi, x86::r12);
        a.mov(x86::rsi, x86::qword_ptr(x86::r12, 24));
        a.lea(x86::rsi, x86::qword_ptr(x86::rsi, static_cast<int32_t>(slot) * 8));
#endif
#ifdef _WIN32
        a.sub(x86::rsp, 32);
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitIndexSetGlobal));
        a.call(x86::rax);
        a.add(x86::rsp, 32);
#else
        a.sub(x86::rsp, 8);
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitIndexSetGlobal));
        a.call(x86::rax);
        a.add(x86::rsp, 8);
#endif
        a.mov(x86::r15, x86::qword_ptr(x86::r12, 48));
        a.mov(x86::rax, x86::qword_ptr(x86::r12, 8));
        a.movzx(x86::rax, x86::byte_ptr(x86::rax));
        a.test(x86::rax, x86::rax);
        a.jnz(epilogue);
    };

    // ---- R148 阶段 4c-1+2：类支持辅助 lambda ----
    // 所有类操作通过 C++ 辅助函数实现（InstanceData 是 RefCounted + COW，
    // 原生 JIT 机器码无法高效处理）。const char* 类名/字段名通过 movabs 加载，
    // 其地址在 CompileResult 生命期内稳定（chunk.constants[nameIdx].stringVal().c_str()）。

    // OP_CLASS_NEW: void jitClassNew(JitContext* ctx, const char* className, uint8_t argCount)
    // R149 扩展：argCount > 0 时 jitClassNew 内部调用 init 方法，并设置 ctx->methodEntryPtr。
    // JIT 代码检测 methodEntryPtr 非 null 时设置 returnAddr + r13 + jmp 入口；否则继续。
    // 栈布局（调用前）：[argN-1]...[arg0]（argCount 个参数）
    // 栈布局（无 init，返回后）：[instance]
    // 栈布局（有 init，返回前）：[extraSlots...][defaults...][args...][fields...][this]
    auto emitClassNew = [&a, &epilogue](const char* className, uint8_t argCount) {
        // R149: 设置 ctx->callerBp = r13（init 帧需记录调用者 basePointer）
        a.mov(x86::qword_ptr(x86::r12, 96), x86::r13);
        // 清空 ctx->methodEntryPtr（jitClassNew 内部会按需设置）
        a.mov(x86::qword_ptr(x86::r12, 72), 0);

        a.mov(x86::qword_ptr(x86::r12, 48), x86::r15);
        a.movabs(x86::r10, reinterpret_cast<uint64_t>(className)); // r10 = className ptr
#ifdef _WIN32
        a.mov(x86::rcx, x86::r12);                      // arg1 = ctx
        a.mov(x86::rdx, x86::r10);                      // arg2 = className
        a.mov(x86::r8, static_cast<int32_t>(argCount)); // arg3 = argCount
#else
        a.mov(x86::rdi, x86::r12);
        a.mov(x86::rsi, x86::r10);
        a.mov(x86::rdx, static_cast<int32_t>(argCount));
#endif
#ifdef _WIN32
        a.sub(x86::rsp, 32);
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitClassNew));
        a.call(x86::rax);
        a.add(x86::rsp, 32);
#else
        a.sub(x86::rsp, 8);
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitClassNew));
        a.call(x86::rax);
        a.add(x86::rsp, 8);
#endif
        a.mov(x86::r15, x86::qword_ptr(x86::r12, 48));
        a.mov(x86::rax, x86::qword_ptr(x86::r12, 8));
        a.movzx(x86::rax, x86::byte_ptr(x86::rax));
        a.test(x86::rax, x86::rax);
        a.jnz(epilogue);

        // R149: 检查 ctx->methodEntryPtr 是否非 null（init 帧已设置）
        Label noInit = a.new_label();
        a.mov(x86::rax, x86::qword_ptr(x86::r12, 72)); // rax = methodEntryPtr
        a.test(x86::rax, x86::rax);
        a.jz(noInit);

        // init 帧已设置：写 returnAddr + 设置 r13 + jmp 入口
        Label returnLabel = a.new_label();
        // 找到新 frame（frames[*frameCount - 1]）并写入 returnAddr
        a.mov(x86::rcx, x86::qword_ptr(x86::r12, 40)); // rcx = &frameCount_
        a.mov(x86::rdx, x86::qword_ptr(x86::rcx));     // rdx = *frameCount
        a.sub(x86::rdx, 1);                            // rdx = idx（新 frame 索引）
        a.mov(x86::rcx, x86::qword_ptr(x86::r12, 32)); // rcx = frames base
        a.imul(x86::rdx, x86::rdx, 72);
        a.add(x86::rcx, x86::rdx); // rcx = &frames[idx]
        a.lea(x86::rax, x86::qword_ptr(returnLabel));
        a.mov(x86::qword_ptr(x86::rcx, 16), x86::rax); // frame->returnAddr = returnLabel

        // 设置 r13 = r15 + (methodLocalCount - 1) * 8
        a.mov(x86::rax, x86::qword_ptr(x86::r12, 80)); // rax = methodLocalCount
        a.dec(x86::rax);
        a.imul(x86::rax, x86::rax, 8);
        a.lea(x86::r13, x86::qword_ptr(x86::r15, x86::rax));

        // jmp methodEntryPtr
        a.mov(x86::rax, x86::qword_ptr(x86::r12, 72));
        a.jmp(x86::rax);

        a.bind(returnLabel);
        a.bind(noInit);
    };

    // OP_INIT_FIELD: void jitInitField(JitContext* ctx, const char* fieldName)
    // 栈布局：[instance, val]（val 在栈顶），辅助函数 pop val 后修改栈顶 instance
    auto emitInitField = [&a, &epilogue](const char* fieldName) {
        a.mov(x86::qword_ptr(x86::r12, 48), x86::r15);
        a.movabs(x86::r10, reinterpret_cast<uint64_t>(fieldName)); // r10 = fieldName ptr
#ifdef _WIN32
        a.mov(x86::rcx, x86::r12); // arg1 = ctx
        a.mov(x86::rdx, x86::r10); // arg2 = fieldName
#else
        a.mov(x86::rdi, x86::r12);
        a.mov(x86::rsi, x86::r10);
#endif
#ifdef _WIN32
        a.sub(x86::rsp, 32);
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitInitField));
        a.call(x86::rax);
        a.add(x86::rsp, 32);
#else
        a.sub(x86::rsp, 8);
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitInitField));
        a.call(x86::rax);
        a.add(x86::rsp, 8);
#endif
        a.mov(x86::r15, x86::qword_ptr(x86::r12, 48));
        a.mov(x86::rax, x86::qword_ptr(x86::r12, 8));
        a.movzx(x86::rax, x86::byte_ptr(x86::rax));
        a.test(x86::rax, x86::rax);
        a.jnz(epilogue);
    };

    // OP_DEFINE_CLASS: void jitDefineClass(JitContext* ctx, const char* className, const char* superClassName)
    // 栈布局：[templateInstance]（pop 消费）
    auto emitDefineClass = [&a, &epilogue](const char* className, const char* superClassName) {
        a.mov(x86::qword_ptr(x86::r12, 48), x86::r15);
        a.movabs(x86::r10, reinterpret_cast<uint64_t>(className));      // r10 = className
        a.movabs(x86::r11, reinterpret_cast<uint64_t>(superClassName)); // r11 = superClassName
#ifdef _WIN32
        a.mov(x86::rcx, x86::r12); // arg1 = ctx
        a.mov(x86::rdx, x86::r10); // arg2 = className
        a.mov(x86::r8, x86::r11);  // arg3 = superClassName
#else
        a.mov(x86::rdi, x86::r12);
        a.mov(x86::rsi, x86::r10);
        a.mov(x86::rdx, x86::r11);
#endif
#ifdef _WIN32
        a.sub(x86::rsp, 32);
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitDefineClass));
        a.call(x86::rax);
        a.add(x86::rsp, 32);
#else
        a.sub(x86::rsp, 8);
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitDefineClass));
        a.call(x86::rax);
        a.add(x86::rsp, 8);
#endif
        a.mov(x86::r15, x86::qword_ptr(x86::r12, 48));
        a.mov(x86::rax, x86::qword_ptr(x86::r12, 8));
        a.movzx(x86::rax, x86::byte_ptr(x86::rax));
        a.test(x86::rax, x86::rax);
        a.jnz(epilogue);
    };

    // OP_MEMBER_GET: void jitMemberGet(JitContext* ctx, const char* fieldName)
    // 栈布局：[obj]，辅助函数 pop obj + push fieldValue
    // R160: OP_MEMBER_GET 带 inline cache 版本
    // 调用 jitMemberGetWithIC(ctx, fieldName, callSiteId, backendPtr)
    // callSiteId 编译期分配，运行时索引 memberGetIC_[callSiteId]
    auto emitMemberGet = [&a, &epilogue, this, &nextCallSiteId](const char* fieldName) {
        uint64_t callSiteId = nextCallSiteId++;
        a.mov(x86::qword_ptr(x86::r12, 48), x86::r15);
        a.movabs(x86::r10, reinterpret_cast<uint64_t>(fieldName)); // r10 = fieldName
#ifdef _WIN32
        a.mov(x86::rcx, x86::r12);                           // arg1 = ctx
        a.mov(x86::rdx, x86::r10);                           // arg2 = fieldName
        a.movabs(x86::r8, callSiteId);                       // arg3 = callSiteId
        a.movabs(x86::r9, reinterpret_cast<uint64_t>(this)); // arg4 = backendPtr
#else
        a.mov(x86::rdi, x86::r12);
        a.mov(x86::rsi, x86::r10);
        a.movabs(x86::rdx, callSiteId);
        a.movabs(x86::rcx, reinterpret_cast<uint64_t>(this));
#endif
#ifdef _WIN32
        a.sub(x86::rsp, 32);
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitMemberGetWithIC));
        a.call(x86::rax);
        a.add(x86::rsp, 32);
#else
        a.sub(x86::rsp, 8);
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitMemberGetWithIC));
        a.call(x86::rax);
        a.add(x86::rsp, 8);
#endif
        a.mov(x86::r15, x86::qword_ptr(x86::r12, 48));
        a.mov(x86::rax, x86::qword_ptr(x86::r12, 8));
        a.movzx(x86::rax, x86::byte_ptr(x86::rax));
        a.test(x86::rax, x86::rax);
        a.jnz(epilogue);
    };

    // OP_MEMBER_SET_VAR: void jitMemberSetVar(JitContext* ctx, int64_t* slotPtr, const char* fieldName)
    // slotPtr = globalSlots + slot*8（编译期解析 varIdx→slot）
    // 栈布局：[val]，辅助函数 pop val 写入 *slotPtr.field
    auto emitMemberSetVar = [&a, &epilogue](int slot, const char* fieldName) {
        a.mov(x86::qword_ptr(x86::r12, 48), x86::r15);
        a.movabs(x86::r10, reinterpret_cast<uint64_t>(fieldName)); // r10 = fieldName
#ifdef _WIN32
        a.mov(x86::rcx, x86::r12); // arg1 = ctx
        // arg2 = slotPtr = [r12+24]（globalSlots） + slot*8
        a.mov(x86::rdx, x86::qword_ptr(x86::r12, 24));
        a.lea(x86::rdx, x86::qword_ptr(x86::rdx, static_cast<int32_t>(slot) * 8));
        a.mov(x86::r8, x86::r10); // arg3 = fieldName
#else
        a.mov(x86::rdi, x86::r12);
        a.mov(x86::rsi, x86::qword_ptr(x86::r12, 24));
        a.lea(x86::rsi, x86::qword_ptr(x86::rsi, static_cast<int32_t>(slot) * 8));
        a.mov(x86::rdx, x86::r10);
#endif
#ifdef _WIN32
        a.sub(x86::rsp, 32);
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitMemberSetVar));
        a.call(x86::rax);
        a.add(x86::rsp, 32);
#else
        a.sub(x86::rsp, 8);
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitMemberSetVar));
        a.call(x86::rax);
        a.add(x86::rsp, 8);
#endif
        a.mov(x86::r15, x86::qword_ptr(x86::r12, 48));
        a.mov(x86::rax, x86::qword_ptr(x86::r12, 8));
        a.movzx(x86::rax, x86::byte_ptr(x86::rax));
        a.test(x86::rax, x86::rax);
        a.jnz(epilogue);
    };

    // OP_MEMBER_SET_LOCAL: void jitMemberSetLocal(JitContext* ctx, uint8_t slot, int64_t* frameBase, const char*
    // fieldName) frameBase = r13（当前帧 basePointer） 栈布局：[val]，辅助函数 pop val 写入 *(frameBase - slot).field
    auto emitMemberSetLocal = [&a, &epilogue](uint8_t slot, const char* fieldName) {
        a.mov(x86::qword_ptr(x86::r12, 48), x86::r15);
        a.movabs(x86::r10, reinterpret_cast<uint64_t>(fieldName)); // r10 = fieldName
#ifdef _WIN32
        a.mov(x86::rcx, x86::r12);                   // arg1 = ctx
        a.mov(x86::rdx, static_cast<int32_t>(slot)); // arg2 = slot
        a.mov(x86::r8, x86::r13);                    // arg3 = frameBase (r13)
        a.mov(x86::r9, x86::r10);                    // arg4 = fieldName
#else
        a.mov(x86::rdi, x86::r12);
        a.mov(x86::rsi, static_cast<int32_t>(slot));
        a.mov(x86::rdx, x86::r13);
        a.mov(x86::rcx, x86::r10);
#endif
#ifdef _WIN32
        a.sub(x86::rsp, 32);
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitMemberSetLocal));
        a.call(x86::rax);
        a.add(x86::rsp, 32);
#else
        a.sub(x86::rsp, 8);
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitMemberSetLocal));
        a.call(x86::rax);
        a.add(x86::rsp, 8);
#endif
        a.mov(x86::r15, x86::qword_ptr(x86::r12, 48));
        a.mov(x86::rax, x86::qword_ptr(x86::r12, 8));
        a.movzx(x86::rax, x86::byte_ptr(x86::rax));
        a.test(x86::rax, x86::rax);
        a.jnz(epilogue);
    };

    // ---- R149: OP_METHOD_CALL / OP_SUPER_CALL 代码生成辅助 lambda ----
    // 栈布局（调用前，向低地址增长）：[argN-1]...[arg0][receiver] ← r15 指向 argN-1
    // 栈布局（调用后）：[extraSlots...][defaults...][args...][fields...][this] ← r15 指向最后一个 extraSlot
    // 调用 jitMethodCall 前需设置 ctx->callerBp (offset 96) = r13（jitMethodCall 读取此值填入 frame->callerBp）
    // 调用后从 ctx 邮箱读取 methodEntryPtr (offset 72) 和 methodLocalCount (offset 80)
    // 参数说明：
    //   methodName       方法名（C 字符串，需静态生命期，嵌入 JIT 机器码）
    //   argCount         实际参数个数
    //   receiverLocalSlotByte  0xFF=无局部接收者；否则为当前帧局部 slot 索引
    //   receiverGlobalSlot     -1=无全局接收者；否则为 globalSlots 中的 slot 索引
    //   isSuperCall     是否 super 调用
    //   superClassName  super 调用的父类名（C 字符串，nullptr=非 super 调用）
    auto emitMethodCall = [&a, &epilogue](const char* methodName, uint8_t argCount, uint8_t receiverLocalSlotByte,
                                          int receiverGlobalSlot, bool isSuperCall, const char* superClassName) {
        Label returnLabel = a.new_label();

        // 1. 计算 receiverSlotPtr → r10
        if (receiverLocalSlotByte != 0xFF) {
            // 接收者是当前帧的局部变量：r10 = r13 - receiverLocalSlotByte * 8
            a.lea(x86::r10, x86::qword_ptr(x86::r13, -static_cast<int32_t>(receiverLocalSlotByte) * 8));
        } else if (receiverGlobalSlot >= 0) {
            // 接收者是全局变量：r10 = globalSlots + receiverGlobalSlot * 8
            a.mov(x86::r10, x86::qword_ptr(x86::r12, 24)); // r10 = globalSlots base
            a.lea(x86::r10, x86::qword_ptr(x86::r10, static_cast<int32_t>(receiverGlobalSlot) * 8));
        } else {
            a.xor_(x86::r10, x86::r10); // nullptr（无 writeBack）
        }

        // 2. 打包 packedArgs → r11
        //    bit 0-7: argCount | bit 8-15: receiverLocalSlotByte | bit 16: isInitCall(0) | bit 17: isSuperCall
        int64_t packedArgs = static_cast<int64_t>(argCount) | (static_cast<int64_t>(receiverLocalSlotByte) << 8) |
                             (static_cast<int64_t>(isSuperCall ? 1 : 0) << 17);
        a.mov(x86::r11, packedArgs);

        // 3. 存储 callerBp (r13) 到 ctx->callerBp (offset 96)
        a.mov(x86::qword_ptr(x86::r12, 96), x86::r13);

        // 4. 同步栈顶到 ctx->stackTop (offset 48)
        a.mov(x86::qword_ptr(x86::r12, 48), x86::r15);

        // 5. 调用 jitMethodCall(ctx, methodName, packedArgs, receiverSlotPtr, superClassName)
        //    Win32 ABI: rcx, rdx, r8, r9, [rsp+32] (5th arg on stack)
        //    SysV ABI:  rdi, rsi, rdx, rcx, r8
        //    R149 fix: Windows x64 ABI 要求第 5 参数在 sub rsp 之后的 [rsp+32] 位置，
        //    原代码顺序"先写 [rsp+32] 再 sub rsp"导致 callee 读到 sub 前的位置
        //    （即 [rsp+72]），是栈上残留垃圾数据（恰好是 JIT 机器码字节），
        //    superClassName 读到 "UH\x89\xE5..." 报"类 X 没有父类"错误。
        //    修复：先 sub rsp 调整栈帧，再写第 5 参数。
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(methodName));
#ifdef _WIN32
        a.sub(x86::rsp, 40);       // shadow space (32) + arg5 slot (8)
        a.mov(x86::rcx, x86::r12); // arg1 = ctx
        a.mov(x86::rdx, x86::rax); // arg2 = methodName
        a.mov(x86::r8, x86::r11);  // arg3 = packedArgs
        a.mov(x86::r9, x86::r10);  // arg4 = receiverSlotPtr
        if (isSuperCall) {
            a.movabs(x86::rax, reinterpret_cast<uint64_t>(superClassName));
            a.mov(x86::qword_ptr(x86::rsp, 32), x86::rax); // arg5 = superClassName (stack)
        } else {
            a.mov(x86::qword_ptr(x86::rsp, 32), 0);
        }
#else
        a.sub(x86::rsp, 8);        // 16-byte alignment
        a.mov(x86::rdi, x86::r12); // arg1 = ctx
        a.mov(x86::rsi, x86::rax); // arg2 = methodName
        a.mov(x86::rdx, x86::r11); // arg3 = packedArgs
        a.mov(x86::rcx, x86::r10); // arg4 = receiverSlotPtr
        if (isSuperCall) {
            a.movabs(x86::rax, reinterpret_cast<uint64_t>(superClassName));
            a.mov(x86::r8, x86::rax); // arg5 = superClassName
        } else {
            a.xor_(x86::r8, x86::r8);
        }
#endif
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitMethodCall));
        a.call(x86::rax);
#ifdef _WIN32
        a.add(x86::rsp, 40);
#else
        a.add(x86::rsp, 8);
#endif

        // 6. 恢复 r15（jitMethodCall 修改了栈）
        a.mov(x86::r15, x86::qword_ptr(x86::r12, 48));

        // 7. 检查错误
        a.mov(x86::rax, x86::qword_ptr(x86::r12, 8));
        a.movzx(x86::rax, x86::byte_ptr(x86::rax));
        a.test(x86::rax, x86::rax);
        a.jnz(epilogue);

        // 8. 在新 frame 上设置 returnAddr（jitMethodCall 无法访问 JIT Label，需在调用后写入）
        //    新 frame 在 frames[*frameCount - 1]
        a.mov(x86::rcx, x86::qword_ptr(x86::r12, 40)); // rcx = &frameCount_
        a.mov(x86::rdx, x86::qword_ptr(x86::rcx));     // rdx = *frameCount
        a.sub(x86::rdx, 1);                            // rdx = idx（新 frame 索引）
        a.mov(x86::rcx, x86::qword_ptr(x86::r12, 32)); // rcx = frames base
        a.imul(x86::rdx, x86::rdx, 72);
        a.add(x86::rcx, x86::rdx); // rcx = &frames[idx]
        a.lea(x86::rax, x86::qword_ptr(returnLabel));
        a.mov(x86::qword_ptr(x86::rcx, 16), x86::rax); // frame->returnAddr = returnLabel

        // 9. 设置 r13 = r15 + (methodLocalCount - 1) * 8（this 在 slot 0 = 最高地址）
        a.mov(x86::rax, x86::qword_ptr(x86::r12, 80)); // rax = methodLocalCount
        a.dec(x86::rax);
        a.imul(x86::rax, x86::rax, 8);
        a.lea(x86::r13, x86::qword_ptr(x86::r15, x86::rax));

        // 10. jmp methodEntryPtr
        a.mov(x86::rax, x86::qword_ptr(x86::r12, 72)); // rax = methodEntryPtr
        a.jmp(x86::rax);

        // 11. 绑定返回 Label（方法 OP_RETURN 后跳回此处）
        a.bind(returnLabel);
    };

    // ---- R144 优化：INT 类型标签检查辅助 lambda ----
    // R143 用 movabs 加载 64 位常量到 r10/r11（每个检查点 2 条 movabs，性能下降 10-15x）。
    // R144 优化：用 shr rdx, 48 取高 16 位 tag，然后 cmp edx, 0x7FF8（imm32 范围内）。
    // 这消除了所有 movabs 指令，每个检查点从 5 条指令减少到 3 条指令。
    // 等价性证明：(bits & TAG_FIELD_MASK) == INT_TAG_BASE 等价于 (bits >> 48) == 0x7FF8，
    // 因为 INT_TAG_BASE 的低 48 位是 0，高 16 位是 0x7FF8。
    // 使用 rdx 作为临时寄存器（运算路径中 rdx 不保存跨 OpCode 状态）。
    // R152: specializeIntMode_=true 时跳过类型检查（INT 特化版本，由类型反馈保证安全性）。
    // R153: specializeFloatMode_=true 时无条件跳转到 failLabel（跳过 INT 原生路径）。
    // R159: 特化模式生成类型守卫 + 即时反优化路径（Tier 2→Tier 1）。
    //   类型不匹配时调用 jitDeoptimize 触发即时降级，然后跳转 generic 路径正确处理当前操作。
    //   r13/r15 是 callee-saved（Windows x64 ABI），jitDeoptimize 保证不修改，无需手动恢复。
    size_t currentChunkIdx = 0; // R159: 当前编译的 chunk 索引（loop 内更新，lambda 通过引用读取）
    auto emitCheckInt = [&a, this, &currentChunkIdx](asmjit::x86::Gp val, Label failLabel) {
        if (specializeIntMode_) {
            // R159: INT 特化模式 — 类型守卫 + 即时反优化
            // 检查 tag == 0x7FF8 (INT)，不匹配则触发反优化降级到 Tier 1
            Label notInt = a.new_label();
            Label intPath = a.new_label();
            a.mov(x86::rdx, val);
            a.shr(x86::rdx, 48);
            a.cmp(x86::edx, 0x7FF8);
            a.jne(notInt);
            a.jmp(intPath); // INT → 跳过 deopt 代码块，进入 INT 原生路径
            a.bind(notInt);
            // 即时反优化：保存帧状态 → 调用 jitDeoptimize → 跳转 generic 路径
            a.mov(x86::qword_ptr(x86::r12, 192), x86::r13);         // osrSavedBp = r13
            a.mov(x86::qword_ptr(x86::r12, 200), x86::r15);         // osrSavedSp = r15
            a.mov(x86::qword_ptr(x86::r12, 48), x86::r15);          // 同步栈顶
            a.mov(x86::rcx, x86::r12);                              // arg1 = ctx
            a.mov(x86::rdx, static_cast<int32_t>(currentChunkIdx)); // arg2 = chunkIdx
#ifdef _WIN32
            a.sub(x86::rsp, 40); // shadow space (32) + 8B 对齐填充
#else
            a.sub(x86::rsp, 8); // 16-byte alignment
#endif
            a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitDeoptimize));
            a.call(x86::rax);
#ifdef _WIN32
            a.add(x86::rsp, 40);
#else
            a.add(x86::rsp, 8);
#endif
            // r13/r15 由 callee-saved 保证，无需恢复
            a.jmp(failLabel); // 当前操作走 generic 路径正确处理
            a.bind(intPath);
            return;
        }
        if (specializeFloatMode_) {
            // R159: FLOAT 特化模式 — 类型守卫 + 即时反优化
            // FLOAT 判定：tag < 0x7FF8（普通 double）或 tag > 0x7FFB（NAN_BOXED_FLOAT_MARKER 0x7FFC）
            // 非 FLOAT（tag in [0x7FF8, 0x7FFB] = INT/BOOL/NULL/POINTER）则触发反优化降级
            a.mov(x86::rdx, val);
            a.shr(x86::rdx, 48);
            a.cmp(x86::edx, 0x7FF8);
            a.jb(failLabel); // tag < 0x7FF8 → FLOAT → generic 路径
            a.cmp(x86::edx, 0x7FFB);
            a.ja(failLabel); // tag > 0x7FFB → FLOAT → generic 路径
            // tag in [0x7FF8, 0x7FFB] → INT/BOOL/NULL/POINTER → 即时反优化
            a.mov(x86::qword_ptr(x86::r12, 192), x86::r13);         // osrSavedBp = r13
            a.mov(x86::qword_ptr(x86::r12, 200), x86::r15);         // osrSavedSp = r15
            a.mov(x86::qword_ptr(x86::r12, 48), x86::r15);          // 同步栈顶
            a.mov(x86::rcx, x86::r12);                              // arg1 = ctx
            a.mov(x86::rdx, static_cast<int32_t>(currentChunkIdx)); // arg2 = chunkIdx
#ifdef _WIN32
            a.sub(x86::rsp, 40); // shadow space (32) + 8B 对齐填充
#else
            a.sub(x86::rsp, 8); // 16-byte alignment
#endif
            a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitDeoptimize));
            a.call(x86::rax);
#ifdef _WIN32
            a.add(x86::rsp, 40);
#else
            a.add(x86::rsp, 8);
#endif
            a.jmp(failLabel); // 反优化后走 generic 路径正确处理
            return;
        }
        a.mov(x86::rdx, val);
        a.shr(x86::rdx, 48);
        a.cmp(x86::edx, 0x7FF8);
        a.jne(failLabel);
    };

    // ---- R152: 类型反馈收集 lambda ----
    // 在算术/比较指令的 genericXXX 路径入口调用，统计操作数类型分布。
    // 通过 ctx->typeFeedback[chunkIdx] 递增对应计数器：
    //   - FLOAT（普通 double 位模式或 NAN_BOXED_FLOAT_MARKER 0x7FFC）→ floatCount++
    //   - BOOL/NULL/POINTER（tag 0x7FF9..0x7FFB）→ otherCount++
    // INT 类型不会走到 genericXXX 路径（emitCheckInt 通过即走原生路径），所以 intCount 不在此统计。
    // FLOAT 判断依据（NaN-boxing 设计，见文件头部常量）：
    //   - tag == 0x7FF8 → INT（emitCheckInt 通过，不会走 genericXXX）
    //   - tag in [0x7FF9, 0x7FFB] → BOOL/NULL/POINTER（otherCount）
    //   - 其他（含 0x7FFC NAN_BOXED_FLOAT_MARKER 与所有普通 double 位模式）→ FLOAT（floatCount）
    // 使用 r10/r11 作为临时寄存器（caller-saved，运算路径中不保存跨 OpCode 状态）。
    // 仅在 genericXXX 路径执行（非热路径），性能开销可忽略。
    auto emitRecordTypeFeedback = [&a, this](asmjit::x86::Gp val, size_t chunkIdx) {
        // R153: FLOAT 特化模式下不生成类型反馈收集代码（类型反馈已收集完毕）
        if (specializeFloatMode_) {
            (void)val;
            (void)chunkIdx;
            return;
        }
        Label isFloat = a.new_label();
        Label done = a.new_label();
        // r10 = ctx->typeFeedback 数组指针（offset 128）
        a.mov(x86::r10, x86::qword_ptr(x86::r12, 128));
        // r11 = val 的高 16 位 tag
        a.mov(x86::r11, val);
        a.shr(x86::r11, 48);
        // 判断 tag 是否在 [0x7FF9, 0x7FFB] 范围（BOOL/NULL/POINTER）
        // tag < 0x7FF9 → FLOAT（含普通 double 位模式，如 1.5 的 tag=0x3FF8）
        a.cmp(x86::r11d, 0x7FF9);
        a.jb(isFloat);
        // tag > 0x7FFB → FLOAT（含 NAN_BOXED_FLOAT_MARKER 0x7FFC）
        a.cmp(x86::r11d, 0x7FFB);
        a.ja(isFloat);
        // tag in [0x7FF9, 0x7FFB] → otherCount++ (TypeFeedback offset 16)
        a.inc(x86::qword_ptr(x86::r10, static_cast<int32_t>(chunkIdx * 24 + 16)));
        a.jmp(done);
        // FLOAT → floatCount++ (TypeFeedback offset 8)
        a.bind(isFloat);
        a.inc(x86::qword_ptr(x86::r10, static_cast<int32_t>(chunkIdx * 24 + 8)));
        a.bind(done);
    };

    // ---- R161 perf: FLOAT 原生算术路径 lambda ----
    // 在 emitCheckInt 失败后（非特化模式）调用，检查两个操作数是否都是 FLOAT，
    // 若是则执行原生 SSE2 算术（addsd/subsd/mulsd/divsd），避免 C++ 辅助函数调用开销。
    // 前提：rax = right raw bits, rcx = left raw bits（emitCheckInt 非特化模式仅修改 rdx）
    // FLOAT 判定（NaN-boxing 设计）：
    //   - tag < 0x7FF8（普通 double 位模式）→ FLOAT
    //   - tag > 0x7FFB（含 NAN_BOXED_FLOAT_MARKER 0x7FFC）→ FLOAT
    //   - tag in [0x7FF8, 0x7FFB] → INT/BOOL/NULL/POINTER → 非 FLOAT
    // raw bits 到 double 的转换：
    //   - 普通 double：raw bits 就是 double 的原始位模式，直接 movq 到 xmm
    //   - NAN_BOXED_FLOAT_MARKER (0x7FFC...)：本身是一个 NaN 位模式，直接 movq 即可
    //     （丢失原始 NaN 尾数位，但 NaN 运算结果仍为 NaN，最终再次规范化为 NAN_BOXED_FLOAT_MARKER）
    // 结果 NaN 规范化：
    //   - 若结果的 tag field in [0x7FF8, 0x7FFB]，替换为 JIT_NAN_BOXED_FLOAT_MARKER
    //   - 否则直接存储结果位模式
    // @param op 0=add, 1=sub, 2=mul, 3=div
    // @param failLabel 非 FLOAT 类型时跳转的目标（genericXXX 标签）
    // @param endLabel 成功后跳转的目标（xxxEnd 标签）
    // @param chunkIdx 当前 chunk 索引（类型反馈收集用）
    // @param divByZeroLabel 除零错误标签（仅 op=3 使用）
    auto emitFloatBinaryArith = [&a, &emitRecordTypeFeedback, this](int op, Label failLabel, Label endLabel,
                                                                    size_t chunkIdx, Label divByZeroLabel = Label()) {
        Label checkLeft = a.new_label();
        Label doArith = a.new_label();
        Label storeResult = a.new_label();
        Label replaceNan = a.new_label();

        // 检查 rax (right) 是否是 FLOAT
        a.mov(x86::rdx, x86::rax);
        a.shr(x86::rdx, 48);
        a.cmp(x86::edx, 0x7FF8);
        a.jb(checkLeft); // tag < 0x7FF8 → FLOAT
        a.cmp(x86::edx, 0x7FFB);
        a.ja(checkLeft);  // tag > 0x7FFB → FLOAT
        a.jmp(failLabel); // tag in [0x7FF8, 0x7FFB] → 非FLOAT → genericXXX

        a.bind(checkLeft);
        // 检查 rcx (left) 是否是 FLOAT
        a.mov(x86::rdx, x86::rcx);
        a.shr(x86::rdx, 48);
        a.cmp(x86::edx, 0x7FF8);
        a.jb(doArith); // tag < 0x7FF8 → FLOAT
        a.cmp(x86::edx, 0x7FFB);
        a.ja(doArith);    // tag > 0x7FFB → FLOAT
        a.jmp(failLabel); // tag in [0x7FF8, 0x7FFB] → 非FLOAT → genericXXX

        a.bind(doArith);
        // R161 perf: 收集类型反馈（FLOAT 操作走原生路径也需收集，以触发 FLOAT 特化重编译）
        // emitRecordTypeFeedback 检查 rax 的 tag 并递增 floatCount（FLOAT 类型），仅修改 r10/r11
        emitRecordTypeFeedback(x86::rax, chunkIdx);
        // 从 raw bits 提取 double 到 xmm 寄存器
        a.movq(x86::xmm0, x86::rcx); // xmm0 = left as double
        a.movq(x86::xmm1, x86::rax); // xmm1 = right as double

        // 除零检查（仅除法）
        if (op == 3) {
            // 检查 xmm1 (right/除数) 是否为 0.0（包括 +0.0 和 -0.0）
            // 清除符号位后检查是否为 0
            a.movq(x86::rax, x86::xmm1);
            a.movabs(x86::rdx, 0x7FFFFFFFFFFFFFFFULL);
            a.and_(x86::rax, x86::rdx);
            a.test(x86::rax, x86::rax);
            a.jz(divByZeroLabel);
        }

        // 执行算术运算
        switch (op) {
        case 0:
            a.addsd(x86::xmm0, x86::xmm1);
            break; // xmm0 = left + right
        case 1:
            a.subsd(x86::xmm0, x86::xmm1);
            break; // xmm0 = left - right
        case 2:
            a.mulsd(x86::xmm0, x86::xmm1);
            break; // xmm0 = left * right
        case 3:
            a.divsd(x86::xmm0, x86::xmm1);
            break; // xmm0 = left / right
        }

        // 检查结果是否落入 boxed NaN 范围
        a.movq(x86::rax, x86::xmm0); // rax = result raw bits
        a.mov(x86::rdx, x86::rax);
        a.shr(x86::rdx, 48);
        a.cmp(x86::edx, 0x7FF8);
        a.jb(storeResult); // tag < 0x7FF8 → 普通 double
        a.cmp(x86::edx, 0x7FFB);
        a.jbe(replaceNan); // tag in [0x7FF8, 0x7FFB] → 替换
        // tag > 0x7FFB → 普通 double（如 -inf），fall through 到 storeResult

        a.bind(storeResult);
        a.add(x86::r15, 16); // pop 两个操作数
        a.sub(x86::r15, 8);  // push 结果
        a.mov(x86::qword_ptr(x86::r15), x86::rax);
        a.jmp(endLabel);

        a.bind(replaceNan);
        a.movabs(x86::rax, JIT_NAN_BOXED_FLOAT_MARKER);
        a.jmp(storeResult);
    };

    // ---- R154 阶段 5d：嵌套左值赋值链 + OP_LEN + OP_DUP_N 辅助 lambda ----
    // 设计要点：
    //   1. OP_LEN/OP_INDEX_SET 涉及动态类型分派 + COW，通过 C++ 辅助函数实现（与 emitBuildArray 模式一致）
    //   2. OP_DUP_N/OP_LOAD_MUTATED/OP_WRITEBACK_* 语义简单，纯 inline 机器码实现（无需 helper call 开销）
    //   3. lastMutatedReceiver_ 通过 JitContext.lastMutatedReceiverPtr (offset 144) 间接访问：
    //      - load: mov rax, [r12+144]; mov rax, [rax]（先取指针，再解引用得到 int64_t 值）
    //      - store: mov rax, [r12+144]; mov qword ptr [rax], imm（通过指针写入）
    //      - clear: 写入 JIT_NULL_BITS 防止后续误用（与 StackVM lastMutatedReceiver_ = nullValue() 对齐）

    // OP_LEN: void jitLen(JitContext* ctx)
    // 辅助函数内部 pop 1 + push 1，通过 ctx->stackTop 操作栈
    auto emitLen = [&a, &epilogue]() {
        a.mov(x86::qword_ptr(x86::r12, 48), x86::r15);
#ifdef _WIN32
        a.mov(x86::rcx, x86::r12); // arg1 = ctx
#else
        a.mov(x86::rdi, x86::r12);
#endif
#ifdef _WIN32
        a.sub(x86::rsp, 32);
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitLen));
        a.call(x86::rax);
        a.add(x86::rsp, 32);
#else
        a.sub(x86::rsp, 8);
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitLen));
        a.call(x86::rax);
        a.add(x86::rsp, 8);
#endif
        a.mov(x86::r15, x86::qword_ptr(x86::r12, 48));
        a.mov(x86::rax, x86::qword_ptr(x86::r12, 8));
        a.movzx(x86::rax, x86::byte_ptr(x86::rax));
        a.test(x86::rax, x86::rax);
        a.jnz(epilogue);
    };

    // OP_DUP_N: inline peek(depth) + push（无需 C++ helper）
    // 栈布局（调用前）：[..., v_depth, ..., v_1, v_0]（v_0 在栈顶）
    // 栈布局（调用后）：[..., v_depth, ..., v_1, v_0, v_depth_copy]（push v_depth 的副本）
    // JIT 栈向下增长：peek(depth) = [r15 + depth*8]（v_0 在 [r15]，v_depth 在 [r15 + depth*8]）
    auto emitDupN = [&a](uint8_t depth) {
        a.mov(x86::rax, x86::qword_ptr(x86::r15, static_cast<int32_t>(depth) * 8)); // rax = peek(depth)
        a.sub(x86::r15, 8);                                                         // push
        a.mov(x86::qword_ptr(x86::r15), x86::rax);
    };

    // OP_LOAD_MUTATED: inline push(lastMutatedReceiver_)（无需 C++ helper）
    // lastMutatedReceiverPtr 在 [r12+144]，指向 lastMutatedReceiver_（int64_t）
    auto emitLoadMutated = [&a]() {
        a.mov(x86::rax, x86::qword_ptr(x86::r12, 144)); // rax = lastMutatedReceiverPtr
        a.mov(x86::rax, x86::qword_ptr(x86::rax));      // rax = *lastMutatedReceiverPtr = lastMutatedReceiver_ bits
        a.sub(x86::r15, 8);                             // push
        a.mov(x86::qword_ptr(x86::r15), x86::rax);
    };

    // OP_INDEX_SET: void jitIndexSet(JitContext* ctx)
    // 辅助函数内部 pop 3 个（obj/innerIdx/val），不 push；变异后容器存入 lastMutatedReceiver_
    auto emitIndexSet = [&a, &epilogue]() {
        a.mov(x86::qword_ptr(x86::r12, 48), x86::r15);
#ifdef _WIN32
        a.mov(x86::rcx, x86::r12); // arg1 = ctx
#else
        a.mov(x86::rdi, x86::r12);
#endif
#ifdef _WIN32
        a.sub(x86::rsp, 32);
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitIndexSet));
        a.call(x86::rax);
        a.add(x86::rsp, 32);
#else
        a.sub(x86::rsp, 8);
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitIndexSet));
        a.call(x86::rax);
        a.add(x86::rsp, 8);
#endif
        a.mov(x86::r15, x86::qword_ptr(x86::r12, 48));
        a.mov(x86::rax, x86::qword_ptr(x86::r12, 8));
        a.movzx(x86::rax, x86::byte_ptr(x86::rax));
        a.test(x86::rax, x86::rax);
        a.jnz(epilogue);
    };

    // OP_WRITEBACK_*_VAR: inline *globalSlots[slot] = lastMutatedReceiver_; lastMutatedReceiver_ = NULL
    // globalSlots 基址在 [r12+24]，slot 是编译期解析的索引
    // 与 StackVM writebackToGlobalVar 语义一致（整体替换 + 清空 lastMutatedReceiver_）
    auto emitWritebackVar = [&a](int slot) {
        // 1. 加载 lastMutatedReceiver_ raw bits
        a.mov(x86::rax, x86::qword_ptr(x86::r12, 144)); // rax = lastMutatedReceiverPtr
        a.mov(x86::rax, x86::qword_ptr(x86::rax));      // rax = *lastMutatedReceiverPtr = lastMutatedReceiver_ bits
        // 2. 写入 globalSlots[slot]
        a.mov(x86::rcx, x86::qword_ptr(x86::r12, 24)); // rcx = globalSlots base
        a.mov(x86::qword_ptr(x86::rcx, static_cast<int32_t>(slot) * 8), x86::rax);
        // 3. 清空 lastMutatedReceiver_ = JIT_NULL_BITS
        a.mov(x86::rax, x86::qword_ptr(x86::r12, 144));                       // rax = lastMutatedReceiverPtr
        a.mov(x86::qword_ptr(x86::rax), static_cast<int64_t>(JIT_NULL_BITS)); // *lastMutatedReceiverPtr = NULL
    };

    // OP_WRITEBACK_*_LOCAL: inline stack_[bp - slot*8] = lastMutatedReceiver_; lastMutatedReceiver_ = NULL
    // JIT 栈向下增长：slot 0 在 [r13]，slot N 在 [r13 - N*8]
    // 与 StackVM writebackToStackSlot 语义一致（整体替换 + 清空 lastMutatedReceiver_）
    // 注意：JIT 的 jitMethodReturn 总是在方法返回时同步字段槽到 this.fields()，
    // 故无需像 StackVM 那样设置 fieldsModified 标志（行为等价，只是 JIT 无条件同步）
    auto emitWritebackLocal = [&a](uint8_t slot) {
        // 1. 加载 lastMutatedReceiver_ raw bits
        a.mov(x86::rax, x86::qword_ptr(x86::r12, 144)); // rax = lastMutatedReceiverPtr
        a.mov(x86::rax, x86::qword_ptr(x86::rax));      // rax = *lastMutatedReceiverPtr = lastMutatedReceiver_ bits
        // 2. 写入 [r13 - slot*8]（局部变量栈槽）
        a.mov(x86::qword_ptr(x86::r13, -static_cast<int32_t>(slot) * 8), x86::rax);
        // 3. 清空 lastMutatedReceiver_ = JIT_NULL_BITS
        a.mov(x86::rax, x86::qword_ptr(x86::r12, 144));                       // rax = lastMutatedReceiverPtr
        a.mov(x86::qword_ptr(x86::rax), static_cast<int64_t>(JIT_NULL_BITS)); // *lastMutatedReceiverPtr = NULL
    };

    // ---- 建立函数表：functionName → (entryLabel, localCount, arity) ----
    // R141: 编译期需要知道每个函数的 localCount（用于预分配局部变量栈槽）
    // 和入口 Label（用于 OP_CALL 的 jmp 指令）
    struct FuncInfo {
        Label entryLabel;
        int localCount = 0;
        int arity = 0;
        int requiredArity = 0;                // R149: 必需参数个数（默认参数支持）
        const BytecodeChunk* chunk = nullptr; // R155: OP_CLOSURE 构建闭包值时获取 chunkPtr 用
        bool hasInnerClosures = false;        // R161 perf: 函数体是否含 OP_CLOSURE（fast path 门控）
    };
    std::unordered_map<std::string, FuncInfo> funcTable;
    // R149: 同时收集方法 chunk（name 含 '.'）的元信息，供 runtime_.add 后填充 methodEntries_
    struct MethodLabelInfo {
        std::string fullName; // "Class.method"
        Label entryLabel;
        int localCount = 0;
        int arity = 0;
        int requiredArity = 0;
        const BytecodeChunk* chunk = nullptr;
    };
    std::vector<MethodLabelInfo> methodLabels;
    for (const auto& [name, chunk] : result.functionChunks) {
        FuncInfo info;
        info.entryLabel = a.new_label();
        info.localCount = chunk.localCount;
        info.arity = chunk.arity;
        info.requiredArity = chunk.requiredArity;
        info.chunk = &chunk; // R155: OP_CLOSURE 构建闭包值时获取 chunkPtr 用
        // R161 perf: 扫描函数体是否含 OP_CLOSURE（用于 OP_CALL fast path 门控）
        // 若函数体无 OP_CLOSURE，则 frameUpvaluesStack_ 在此帧不会被访问，
        // 可跳过 jitCallByName 的 frameUpvaluesStack_ 初始化
        {
            const auto& code = chunk.code;
            for (size_t i = 0; i < code.size();) {
                OpCode op = static_cast<OpCode>(code[i]);
                if (op == OpCode::OP_CLOSURE) {
                    info.hasInnerClosures = true;
                    break;
                }
                i += chunk.instructionSizeAt(i);
            }
        }
        funcTable.emplace(name, std::move(info));
        // R149: 方法 chunk（name 含 '.'）单独收集，用于 methodEntries_ 填充
        if (name.find('.') != std::string::npos) {
            MethodLabelInfo mli;
            mli.fullName = name;
            mli.entryLabel = funcTable[name].entryLabel; // 复制 Label 引用
            mli.localCount = chunk.localCount;
            mli.arity = chunk.arity;
            mli.requiredArity = chunk.requiredArity;
            mli.chunk = &chunk;
            methodLabels.push_back(std::move(mli));
        }
    }

    // R149: 编译期扫描所有 chunk 的 OP_DEFINE_CLASS 指令，收集类名到 classNameSet_
    // 用途：OP_CALL handler 中 funcTable 找不到时检查此集合，命中则走 emitClassNew 路径
    // （MiniLang 语法 `Point(3, 4)` 编译为 OP_CALL 而非 OP_CLASS_NEW，需 fallback）
    // 与 StackVM executeCallByName 中 classInfo_.find(funName) fallback 对齐
    classNameSet_.clear();
    auto collectClassNames = [this](const BytecodeChunk& c) {
        const auto& code = c.code;
        for (size_t i = 0; i < code.size();) {
            OpCode op = static_cast<OpCode>(code[i]);
            if (op == OpCode::OP_DEFINE_CLASS && i + 4 < code.size()) {
                uint16_t nameIdx = static_cast<uint16_t>(code[i + 1]) | (static_cast<uint16_t>(code[i + 2]) << 8);
                if (nameIdx < c.constants.size()) {
                    classNameSet_.insert(c.constants[nameIdx].stringVal());
                }
            }
            i += c.instructionSizeAt(i);
        }
    };
    collectClassNames(result.mainChunk);
    for (const auto& [name, chunk] : result.functionChunks) {
        collectClassNames(chunk);
    }

    // ---- 收集所有 chunk：mainChunk 在前，functionChunks 在后 ----
    // 编译顺序不影响正确性（通过 Label 跳转），但 mainChunk 必须先编译
    // 作为 JIT 入口（prologue 后第一条指令属于 mainChunk）
    std::vector<const BytecodeChunk*> allChunks;
    allChunks.push_back(&result.mainChunk);
    for (const auto& [name, chunk] : result.functionChunks) {
        allChunks.push_back(&chunk);
    }

    // R150: 初始化 per-chunk 调用计数数组与名称映射（热点检测用）
    // 索引 0 = mainChunk（不计数），索引 1..N = functionChunks
    // chunkCallCounts_ 在 execute 入口清零，此处仅设置大小与名称
    chunkNames_.clear();
    chunkNames_.reserve(allChunks.size());
    for (const auto* c : allChunks) {
        chunkNames_.push_back(c->name);
    }
    chunkCallCounts_.assign(allChunks.size(), 0);
    jitContext_.chunkCallCounts = chunkCallCounts_.data();

    // R151: 初始化 per-chunk 热点阈值与重编译标志数组
    // 默认仅方法 chunk（name 含 '.'）启用热点检测，阈值 kDefaultHotThreshold
    // mainChunk 和普通函数 chunk 阈值 0（不触发）；customThresholds_ 覆盖默认值
    hotThresholds_.assign(allChunks.size(), 0);
    for (size_t i = 0; i < allChunks.size(); ++i) {
        const std::string& name = allChunks[i]->name;
        // 方法 chunk 的 name 格式 "ClassName.method"（含 '.'），普通函数无 '.'
        if (name.find('.') != std::string::npos) {
            hotThresholds_[i] = minilang::kDefaultHotThreshold;
        }
        // 应用测试用自定义阈值覆盖
        auto it = customThresholds_.find(name);
        if (it != customThresholds_.end()) {
            hotThresholds_[i] = it->second;
        }
    }
    recompiledFlags_.assign(allChunks.size(), 0);
    jitContext_.hotThresholds = hotThresholds_.data();
    jitContext_.recompiledFlags = recompiledFlags_.data();

    // R152: 初始化 per-chunk 类型反馈计数器数组（特化重编译决策依据）
    // 索引与 chunkCallCounts_ 对齐，在算术指令 genericXXX 路径递增
    typeFeedback_.assign(allChunks.size(), minilang::TypeFeedback{});
    jitContext_.typeFeedback = typeFeedback_.data();
    // R152: 设置 backendPtr 为 this，供 jitTriggerRecompile 访问私有成员（未来扩展）
    jitContext_.backendPtr = this;

    // R154: 初始化 lastMutatedReceiver_ 为 NaN-boxing NULL（嵌套左值赋值链中转邮箱）
    // OP_INDEX_SET 将变异后容器存入此，OP_LOAD_MUTATED push 此到栈顶，
    // OP_WRITEBACK_*_LOCAL/VAR 整体替换栈槽/全局变量为此值
    lastMutatedReceiver_ = static_cast<int64_t>(JIT_NULL_BITS);
    jitContext_.lastMutatedReceiverPtr = &lastMutatedReceiver_;

    // R157: 初始化 per-chunk OSR 循环回边计数/阈值/标志数组
    // 索引与 chunkCallCounts_ 对齐，OP_LOOP 回边时递增 osrLoopCounts_
    // 默认阈值 0（不触发 OSR），测试用 setOsrThreshold 设置 customOsrThresholds_
    osrLoopCounts_.assign(allChunks.size(), 0);
    osrLoopThresholds_.assign(allChunks.size(), 0);
    for (size_t i = 0; i < allChunks.size(); ++i) {
        const std::string& name = allChunks[i]->name;
        auto osrIt = customOsrThresholds_.find(name);
        if (osrIt != customOsrThresholds_.end()) {
            osrLoopThresholds_[i] = osrIt->second;
        }
    }
    osrRecompiledFlags_.assign(allChunks.size(), 0);
    jitContext_.osrLoopCountsPtr = osrLoopCounts_.data();
    jitContext_.osrLoopThresholdsPtr = osrLoopThresholds_.data();
    jitContext_.osrRecompiledFlagsPtr = osrRecompiledFlags_.data();

    // ---- 循环编译每个 chunk ----
    for (size_t chunkIdx = 0; chunkIdx < allChunks.size(); ++chunkIdx) {
        const BytecodeChunk& chunk = *allChunks[chunkIdx];
        const bool isMain = (chunkIdx == 0);
        currentChunkIdx = chunkIdx; // R159: 更新 emitCheckInt lambda 使用的 chunk 索引

        // ---- 第一遍扫描：收集所有跳转目标位置，为每个位置创建 Label ----
        const auto& bytecodes = chunk.code;
        std::unordered_map<size_t, Label> jumpLabels;
        {
            size_t scanIp = 0;
            while (scanIp < bytecodes.size()) {
                OpCode scanOp = static_cast<OpCode>(bytecodes[scanIp]);
                if (scanOp == OpCode::OP_JUMP || scanOp == OpCode::OP_JUMP_IF_FALSE || scanOp == OpCode::OP_LOOP) {
                    if (scanIp + 2 < bytecodes.size()) {
                        uint16_t target = static_cast<uint16_t>(bytecodes[scanIp + 1]) |
                                          (static_cast<uint16_t>(bytecodes[scanIp + 2]) << 8);
                        if (target < bytecodes.size() && jumpLabels.find(target) == jumpLabels.end()) {
                            jumpLabels[target] = a.new_label();
                        }
                    }
                }
                scanIp += chunk.instructionSizeAt(scanIp);
            }
        }

        // 非 mainChunk：绑定函数入口 Label（OP_CALL 通过此 Label 跳转）
        if (!isMain) {
            auto it = funcTable.find(chunk.name);
            if (it != funcTable.end()) {
                a.bind(it->second.entryLabel);
                // R150: 注入 per-chunk 调用计数器递增指令（热点检测用）
                // JitContext.chunkCallCounts (offset 104) 是指向 uint64_t 数组的指针，
                // 不能直接 inc [r12 + 104 + chunkIdx*8]（那会写穿 JitContext 结构体）。
                // 正确做法：先加载指针到临时寄存器，再通过指针递增数组元素。
                // 与 globalSlots 访问模式一致：mov rcx, [r12+24]; mov rax, [rcx + slot*8]
                // rcx 在函数入口处是空闲的临时寄存器（后续字节码会自行设置寄存器状态）。
                a.mov(x86::rcx, x86::qword_ptr(x86::r12, 104));
                a.inc(x86::qword_ptr(x86::rcx, static_cast<int32_t>(chunkIdx * 8)));

                // R151: 热点阈值检查（协作式安全点）
                // 检查当前 chunk 调用计数是否达到阈值，且尚未触发过重编译。
                // 仅当 hotThresholds_[chunkIdx] > 0 时注入阈值检查代码（阈值为 0 时
                // 完全跳过代码生成，运行时零开销）。默认仅方法 chunk（name 含 '.'）
                // 启用热点检测，mainChunk 和普通函数 chunk 阈值为 0。
                if (chunkIdx < hotThresholds_.size() && hotThresholds_[chunkIdx] > 0) {
                    Label skipRecompile = a.new_label();
                    // 1. 检查是否已触发过重编译（recompiledFlags[chunkIdx] == 0 才继续）
                    a.mov(x86::rcx, x86::qword_ptr(x86::r12, 120)); // rcx = recompiledFlags 指针
                    a.mov(x86::rax, x86::qword_ptr(x86::rcx, static_cast<int32_t>(chunkIdx * 8)));
                    a.test(x86::rax, x86::rax);
                    a.jnz(skipRecompile); // 已触发过，跳过
                    // 2. 比较调用计数与阈值
                    a.mov(x86::rcx, x86::qword_ptr(x86::r12, 104)); // rcx = chunkCallCounts 指针
                    a.mov(x86::rax, x86::qword_ptr(x86::rcx, static_cast<int32_t>(chunkIdx * 8)));
                    a.mov(x86::rcx, x86::qword_ptr(x86::r12, 112)); // rcx = hotThresholds 指针
                    a.cmp(x86::rax, x86::qword_ptr(x86::rcx, static_cast<int32_t>(chunkIdx * 8)));
                    a.jb(skipRecompile); // 未达阈值，跳过
                    // 3. 达到阈值：标记为已触发（避免重复触发）
                    a.mov(x86::rcx, x86::qword_ptr(x86::r12, 120)); // rcx = recompiledFlags 指针
                    a.mov(x86::qword_ptr(x86::rcx, static_cast<int32_t>(chunkIdx * 8)), 1);
                    // 4. 同步栈顶到 JitContext.stackTop（辅助函数可能读栈）
                    a.mov(x86::qword_ptr(x86::r12, 48), x86::r15);
                    // 5. 调用 jitTriggerRecompile(ctx, chunkIdx)
                    //    Windows x64: rcx=ctx, rdx=chunkIdx; 需要 shadow space 32B
                    //    栈对齐：方法 chunk 入口 rsp%16==8，sub 40 后 rsp%16==0，
                    //    call 后 rsp%16==8（标准入口对齐），与 emitMethodCall 一致
                    a.mov(x86::rcx, x86::r12);                       // arg1 = ctx
                    a.mov(x86::rdx, static_cast<int32_t>(chunkIdx)); // arg2 = chunkIdx
#ifdef _WIN32
                    a.sub(x86::rsp, 40); // shadow space (32) + 8B 对齐填充
#else
                    a.sub(x86::rsp, 8); // 16-byte alignment
#endif
                    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitTriggerRecompile));
                    a.call(x86::rax);
#ifdef _WIN32
                    a.add(x86::rsp, 40);
#else
                    a.add(x86::rsp, 8);
#endif
                    // 6. 恢复栈顶（辅助函数可能修改了 stackTop）
                    a.mov(x86::r15, x86::qword_ptr(x86::r12, 48));
                    a.bind(skipRecompile);
                }
            }
        }

        // ---- 第二遍扫描：字节码遍历与机器码生成 ----
        size_t ip = 0;

        while (ip < bytecodes.size()) {
            // 如果当前 ip 是跳转目标，绑定对应的 Label
            auto labelIt = jumpLabels.find(ip);
            if (labelIt != jumpLabels.end()) {
                a.bind(labelIt->second);
            }

            OpCode op = static_cast<OpCode>(bytecodes[ip]);

            switch (op) {
            case OpCode::OP_INT: {
                // R142 阶段 3a：编译期编码为 NaN-boxing INT
                // OP_INT + 2 字节常量池索引（小端序）
                if (ip + 2 >= bytecodes.size()) {
                    compileError("OP_INT 操作数越界");
                    return nullptr;
                }
                uint16_t idx =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                if (idx >= chunk.constants.size()) {
                    compileError("OP_INT 常量池索引越界: " + std::to_string(idx));
                    return nullptr;
                }
                int64_t value = chunk.constants[idx].intVal();
                uint64_t encoded;
                if (!encodeNanBoxInt(value, encoded)) {
                    compileError("JIT 阶段 3a 不支持超 int48 范围的整数: " + std::to_string(value));
                    return nullptr;
                }
                a.movabs(x86::rax, encoded);
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                ip += 3;
                break;
            }

            case OpCode::OP_FLOAT: {
                // R143 阶段 3b：编译期编码为 NaN-boxing FLOAT
                // 直接存储 double 的 64 位原始位，若落入 boxed NaN 范围则替换为 NAN_BOXED_FLOAT_MARKER
                if (ip + 2 >= bytecodes.size()) {
                    compileError("OP_FLOAT 操作数越界");
                    return nullptr;
                }
                uint16_t idx =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                if (idx >= chunk.constants.size()) {
                    compileError("OP_FLOAT 常量池索引越界: " + std::to_string(idx));
                    return nullptr;
                }
                double value = chunk.constants[idx].floatVal();
                uint64_t encoded = encodeNanBoxFloat(value);
                a.movabs(x86::rax, encoded);
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                ip += 3;
                break;
            }

            case OpCode::OP_STRING: {
                // R143 阶段 3b：编译期编码为 NaN-boxing STRING（POINTER tag + StringData* 指针）
                // 常量池中的 Value 已是 NaN-boxing 指针编码，直接提取 raw bits 作为立即数
                if (ip + 2 >= bytecodes.size()) {
                    compileError("OP_STRING 操作数越界");
                    return nullptr;
                }
                uint16_t idx =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                if (idx >= chunk.constants.size()) {
                    compileError("OP_STRING 常量池索引越界: " + std::to_string(idx));
                    return nullptr;
                }
                // 从 Value 提取 raw bits（Value 是 8 字节 POD-like，memcpy 安全）
                const Value& strVal = chunk.constants[idx];
                uint64_t encoded;
                std::memcpy(&encoded, &strVal, sizeof(uint64_t));
                a.movabs(x86::rax, encoded);
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                ip += 3;
                break;
            }

            case OpCode::OP_NULL: {
                // R142 阶段 3a：NaN-boxing NULL (0x7FFA000000000000)
                a.movabs(x86::rax, JIT_NULL_BITS);
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                ip += 1;
                break;
            }

            case OpCode::OP_TRUE: {
                // R142 阶段 3a：NaN-boxing BOOL true (0x7FF9000000000001)
                a.movabs(x86::rax, JIT_BOOL_TAG_BASE | 1ULL);
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                ip += 1;
                break;
            }

            case OpCode::OP_FALSE: {
                // R142 阶段 3a：NaN-boxing BOOL false (0x7FF9000000000000)
                a.movabs(x86::rax, JIT_BOOL_TAG_BASE);
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                ip += 1;
                break;
            }

            case OpCode::OP_ADD: {
                // R143 阶段 3b：类型分派——INT 走原生路径，FLOAT/STRING 走 C++ 辅助
                // R145 修复 Bug 2：INT 原生路径增加 INT64 + INT48 溢出检测，溢出时走 C++ 辅助
                //   （StackVM 通过 Value(int64_t) 自动装箱 BoxedIntData 保留完整值）
                // R161 perf: 非特化模式下 INT 检查失败先走 FLOAT 原生 SSE2 路径，非 FLOAT 再走 C++ 辅助
                // 栈布局：[left, right]（right 在栈顶）
                Label genericAdd = a.new_label();
                Label addEnd = a.new_label();
                // peek right(rax)/left(rcx) 不弹出
                a.mov(x86::rax, x86::qword_ptr(x86::r15));    // rax = right
                a.mov(x86::rcx, x86::qword_ptr(x86::r15, 8)); // rcx = left
                bool useFloatPathAdd = !specializeIntMode_ && !specializeFloatMode_;
                Label floatCheckAdd = useFloatPathAdd ? a.new_label() : Label();
                if (useFloatPathAdd) {
                    emitCheckInt(x86::rax, floatCheckAdd);
                    emitCheckInt(x86::rcx, floatCheckAdd);
                } else {
                    emitCheckInt(x86::rax, genericAdd);
                    emitCheckInt(x86::rcx, genericAdd);
                }
                // INT 原生路径：先解码运算（不 pop），检查溢出，再 pop + push
                // 栈上保留原始 raw bits，溢出时 emitCallBinaryHelper 会从栈重新 peek
                a.shl(x86::rax, 16);
                a.sar(x86::rax, 16);
                a.shl(x86::rcx, 16);
                a.sar(x86::rcx, 16);
                a.add(x86::rax, x86::rcx);
                a.jo(genericAdd); // INT64 溢出 → C++ 辅助（OverflowCheck::addOverflow）
                // INT48 范围检查：sign_extend(low48(result)) == result ?
                a.mov(x86::rcx, x86::rax);
                a.shl(x86::rcx, 16);
                a.sar(x86::rcx, 16);
                a.cmp(x86::rcx, x86::rax);
                a.jne(genericAdd); // INT48 溢出 → C++ 辅助（Value(int64_t) 自动 BoxedIntData）
                // 不溢出：pop 两个 + 重编码 + push
                a.add(x86::r15, 16);
                // R161 perf: 用缓存的 rbx(JIT_INT48_MASK)/r14(JIT_INT_TAG_BASE) 替代 movabs
                a.and_(x86::rax, x86::rbx);
                a.or_(x86::rax, x86::r14);
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                a.jmp(addEnd);
                // R161 perf: FLOAT 原生 SSE2 路径
                if (useFloatPathAdd) {
                    a.bind(floatCheckAdd);
                    emitFloatBinaryArith(0 /*add*/, genericAdd, addEnd, chunkIdx);
                }
                // C++ 辅助路径：调用 jitAddGeneric(left, right)
                a.bind(genericAdd);
                emitRecordTypeFeedback(x86::rax, chunkIdx);
                emitCallBinaryHelper(reinterpret_cast<void*>(&jitAddGeneric), /*needCtx*/ false, /*checkError*/ false);
                a.bind(addEnd);
                ip += 1;
                break;
            }

            case OpCode::OP_SUBTRACT: {
                // R145 修复 Bug 2：INT 原生路径增加 INT64 + INT48 溢出检测
                // R161 perf: 非特化模式下 INT 检查失败先走 FLOAT 原生 SSE2 路径
                Label genericSub = a.new_label();
                Label subEnd = a.new_label();
                a.mov(x86::rax, x86::qword_ptr(x86::r15));    // rax = right
                a.mov(x86::rcx, x86::qword_ptr(x86::r15, 8)); // rcx = left
                bool useFloatPathSub = !specializeIntMode_ && !specializeFloatMode_;
                Label floatCheckSub = useFloatPathSub ? a.new_label() : Label();
                if (useFloatPathSub) {
                    emitCheckInt(x86::rax, floatCheckSub);
                    emitCheckInt(x86::rcx, floatCheckSub);
                } else {
                    emitCheckInt(x86::rax, genericSub);
                    emitCheckInt(x86::rcx, genericSub);
                }
                // INT 原生路径：先解码运算（不 pop），检查溢出
                a.shl(x86::rax, 16);
                a.sar(x86::rax, 16);
                a.shl(x86::rcx, 16);
                a.sar(x86::rcx, 16);
                a.sub(x86::rcx, x86::rax); // rcx = left - right，设置 OF
                a.jo(genericSub);          // INT64 溢出 → C++ 辅助
                a.mov(x86::rax, x86::rcx); // 结果移到 rax
                // INT48 范围检查
                a.mov(x86::rcx, x86::rax);
                a.shl(x86::rcx, 16);
                a.sar(x86::rcx, 16);
                a.cmp(x86::rcx, x86::rax);
                a.jne(genericSub); // INT48 溢出 → C++ 辅助
                // 不溢出：pop 两个 + 重编码 + push
                a.add(x86::r15, 16);
                // R161 perf: 用缓存的 rbx(JIT_INT48_MASK)/r14(JIT_INT_TAG_BASE) 替代 movabs
                a.and_(x86::rax, x86::rbx);
                a.or_(x86::rax, x86::r14);
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                a.jmp(subEnd);
                // R161 perf: FLOAT 原生 SSE2 路径
                if (useFloatPathSub) {
                    a.bind(floatCheckSub);
                    emitFloatBinaryArith(1 /*sub*/, genericSub, subEnd, chunkIdx);
                }
                a.bind(genericSub);
                emitRecordTypeFeedback(x86::rax, chunkIdx);
                emitCallBinaryHelper(reinterpret_cast<void*>(&jitSubGeneric), /*needCtx*/ false, /*checkError*/ false);
                a.bind(subEnd);
                ip += 1;
                break;
            }

            case OpCode::OP_MULTIPLY: {
                // R145 修复 Bug 2：INT 原生路径增加 INT64 + INT48 溢出检测
                // R161 perf: 非特化模式下 INT 检查失败先走 FLOAT 原生 SSE2 路径
                Label genericMul = a.new_label();
                Label mulEnd = a.new_label();
                a.mov(x86::rax, x86::qword_ptr(x86::r15));    // rax = right
                a.mov(x86::rcx, x86::qword_ptr(x86::r15, 8)); // rcx = left
                bool useFloatPathMul = !specializeIntMode_ && !specializeFloatMode_;
                Label floatCheckMul = useFloatPathMul ? a.new_label() : Label();
                if (useFloatPathMul) {
                    emitCheckInt(x86::rax, floatCheckMul);
                    emitCheckInt(x86::rcx, floatCheckMul);
                } else {
                    emitCheckInt(x86::rax, genericMul);
                    emitCheckInt(x86::rcx, genericMul);
                }
                // INT 原生路径：先解码运算（不 pop），检查溢出
                a.shl(x86::rax, 16);
                a.sar(x86::rax, 16);
                a.shl(x86::rcx, 16);
                a.sar(x86::rcx, 16);
                a.imul(x86::rax, x86::rcx); // rax = right * left，设置 OF
                a.jo(genericMul);           // INT64 溢出 → C++ 辅助
                // INT48 范围检查
                a.mov(x86::rcx, x86::rax);
                a.shl(x86::rcx, 16);
                a.sar(x86::rcx, 16);
                a.cmp(x86::rcx, x86::rax);
                a.jne(genericMul); // INT48 溢出 → C++ 辅助
                // 不溢出：pop 两个 + 重编码 + push
                a.add(x86::r15, 16);
                // R161 perf: 用缓存的 rbx(JIT_INT48_MASK)/r14(JIT_INT_TAG_BASE) 替代 movabs
                a.and_(x86::rax, x86::rbx);
                a.or_(x86::rax, x86::r14);
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                a.jmp(mulEnd);
                // R161 perf: FLOAT 原生 SSE2 路径
                if (useFloatPathMul) {
                    a.bind(floatCheckMul);
                    emitFloatBinaryArith(2 /*mul*/, genericMul, mulEnd, chunkIdx);
                }
                a.bind(genericMul);
                emitRecordTypeFeedback(x86::rax, chunkIdx);
                emitCallBinaryHelper(reinterpret_cast<void*>(&jitMulGeneric), /*needCtx*/ false, /*checkError*/ false);
                a.bind(mulEnd);
                ip += 1;
                break;
            }

            case OpCode::OP_DIVIDE: {
                // R143 阶段 3b：类型分派——INT 走原生路径，FLOAT 走 C++ 辅助
                // R145 修复 Bug 2/3：INT 原生路径增加 INT64_MIN/-1 + INT48 溢出检测
                // R161 perf: 非特化模式下 INT 检查失败先走 FLOAT 原生 SSE2 路径（含除零检查）
                Label genericDiv = a.new_label();
                Label divEnd = a.new_label();
                Label divByZero = a.new_label();
                Label divSkipOvf = a.new_label();
                a.mov(x86::rax, x86::qword_ptr(x86::r15));    // rax = right(除数) raw bits
                a.mov(x86::rcx, x86::qword_ptr(x86::r15, 8)); // rcx = left(被除数) raw bits
                bool useFloatPathDiv = !specializeIntMode_ && !specializeFloatMode_;
                Label floatCheckDiv = useFloatPathDiv ? a.new_label() : Label();
                if (useFloatPathDiv) {
                    emitCheckInt(x86::rax, floatCheckDiv);
                    emitCheckInt(x86::rcx, floatCheckDiv);
                } else {
                    emitCheckInt(x86::rax, genericDiv);
                    emitCheckInt(x86::rcx, genericDiv);
                }
                // INT 原生路径：先解码运算（不 pop），检查溢出
                a.shl(x86::rax, 16);
                a.sar(x86::rax, 16);
                a.shl(x86::rcx, 16);
                a.sar(x86::rcx, 16);
                // 除零检查：必须检查 rax（除数）
                a.test(x86::rax, x86::rax);
                a.jz(divByZero);
                // Bug 3 修复：INT64_MIN / -1 溢出检查（idiv 会触发 #DE 异常）
                a.cmp(x86::rax, -1);
                a.jne(divSkipOvf);
                a.movabs(x86::rdx, 0x8000000000000000ULL); // INT64_MIN
                a.cmp(x86::rcx, x86::rdx);
                a.je(genericDiv); // INT64_MIN / -1 → C++ 辅助
                a.bind(divSkipOvf);
                // 执行除法：被除数 / 除数
                a.xchg(x86::rax, x86::rcx); // rax=被除数, rcx=除数
                a.cqo();
                a.idiv(x86::rcx);
                // Bug 2 修复：INT48 范围检查（商在 rax）
                a.mov(x86::rcx, x86::rax);
                a.shl(x86::rcx, 16);
                a.sar(x86::rcx, 16);
                a.cmp(x86::rcx, x86::rax);
                a.jne(genericDiv); // INT48 溢出 → C++ 辅助
                // 不溢出：pop 两个 + 重编码 + push
                a.add(x86::r15, 16);
                // R161 perf: 用缓存的 rbx(JIT_INT48_MASK)/r14(JIT_INT_TAG_BASE) 替代 movabs
                a.and_(x86::rax, x86::rbx);
                a.or_(x86::rax, x86::r14);
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                a.jmp(divEnd);
                // R161 perf: FLOAT 原生 SSE2 路径（含除零检查 → divByZero）
                if (useFloatPathDiv) {
                    a.bind(floatCheckDiv);
                    emitFloatBinaryArith(3 /*div*/, genericDiv, divEnd, chunkIdx, divByZero);
                }
                // 除零错误路径（栈未 pop，但直接 jmp epilogue 退出，不影响）
                a.bind(divByZero);
#ifdef _WIN32
                a.mov(x86::rcx, x86::r12);
                a.movabs(x86::rdx, reinterpret_cast<uint64_t>("除零错误"));
                a.sub(x86::rsp, 32);
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitReportError));
                a.call(x86::rax);
                a.add(x86::rsp, 32);
#else
                a.mov(x86::rdi, x86::r12);
                a.movabs(x86::rsi, reinterpret_cast<uint64_t>("除零错误"));
                a.sub(x86::rsp, 8);
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitReportError));
                a.call(x86::rax);
                a.add(x86::rsp, 8);
#endif
                a.jmp(epilogue);
                // C++ 辅助路径：调用 jitDivGeneric(ctx, left, right)（含除零检查）
                a.bind(genericDiv);
                emitRecordTypeFeedback(x86::rax, chunkIdx);
                emitCallBinaryHelper(reinterpret_cast<void*>(&jitDivGeneric), /*needCtx*/ true, /*checkError*/ true);
                a.bind(divEnd);
                ip += 1;
                break;
            }

            case OpCode::OP_MODULO: {
                // R143 阶段 3b：类型分派——INT 走原生路径，FLOAT 走 C++ 辅助（std::fmod）
                // R145 修复 Bug 2/3：INT 原生路径增加 INT64_MIN/-1 + INT48 溢出检测
                Label genericMod = a.new_label();
                Label modEnd = a.new_label();
                Label modByZero = a.new_label();
                Label modSkipOvf = a.new_label();
                a.mov(x86::rax, x86::qword_ptr(x86::r15));    // rax = right(除数) raw bits
                a.mov(x86::rcx, x86::qword_ptr(x86::r15, 8)); // rcx = left(被除数) raw bits
                emitCheckInt(x86::rax, genericMod);
                emitCheckInt(x86::rcx, genericMod);
                // INT 原生路径：先解码运算（不 pop），检查溢出
                a.shl(x86::rax, 16);
                a.sar(x86::rax, 16);
                a.shl(x86::rcx, 16);
                a.sar(x86::rcx, 16);
                a.test(x86::rax, x86::rax);
                a.jz(modByZero);
                // Bug 3 修复：INT64_MIN % -1 溢出检查（idiv 会触发 #DE 异常，虽然余数为 0）
                a.cmp(x86::rax, -1);
                a.jne(modSkipOvf);
                a.movabs(x86::rdx, 0x8000000000000000ULL); // INT64_MIN
                a.cmp(x86::rcx, x86::rdx);
                a.je(genericMod); // INT64_MIN % -1 → C++ 辅助
                a.bind(modSkipOvf);
                a.xchg(x86::rax, x86::rcx); // rax=被除数, rcx=除数
                a.cqo();
                a.idiv(x86::rcx);
                a.mov(x86::rax, x86::rdx); // 余数在 rdx
                // Bug 2 修复：INT48 范围检查（余数在 rax）
                a.mov(x86::rcx, x86::rax);
                a.shl(x86::rcx, 16);
                a.sar(x86::rcx, 16);
                a.cmp(x86::rcx, x86::rax);
                a.jne(genericMod); // INT48 溢出 → C++ 辅助
                // 不溢出：pop 两个 + 重编码 + push
                a.add(x86::r15, 16);
                // R161 perf: 用缓存的 rbx(JIT_INT48_MASK)/r14(JIT_INT_TAG_BASE) 替代 movabs
                a.and_(x86::rax, x86::rbx);
                a.or_(x86::rax, x86::r14);
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                a.jmp(modEnd);
                a.bind(modByZero);
#ifdef _WIN32
                a.mov(x86::rcx, x86::r12);
                a.movabs(x86::rdx, reinterpret_cast<uint64_t>("除零错误"));
                a.sub(x86::rsp, 32);
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitReportError));
                a.call(x86::rax);
                a.add(x86::rsp, 32);
#else
                a.mov(x86::rdi, x86::r12);
                a.movabs(x86::rsi, reinterpret_cast<uint64_t>("除零错误"));
                a.sub(x86::rsp, 8);
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitReportError));
                a.call(x86::rax);
                a.add(x86::rsp, 8);
#endif
                a.jmp(epilogue);
                // C++ 辅助路径：调用 jitModGeneric(ctx, left, right)（含除零检查）
                a.bind(genericMod);
                emitRecordTypeFeedback(x86::rax, chunkIdx);
                emitCallBinaryHelper(reinterpret_cast<void*>(&jitModGeneric), /*needCtx*/ true, /*checkError*/ true);
                a.bind(modEnd);
                ip += 1;
                break;
            }

            case OpCode::OP_NEGATE: {
                // R143 阶段 3b：类型分派——INT 走原生路径，FLOAT 走 C++ 辅助
                // R145 修复 Bug 2：INT 原生路径增加 INT64 + INT48 溢出检测
                //   （INT64_MIN 取负会 INT64 溢出；INT48 边界值取负会 INT48 溢出）
                Label genericNeg = a.new_label();
                Label negEnd = a.new_label();
                a.mov(x86::rax, x86::qword_ptr(x86::r15)); // rax = value raw bits
                emitCheckInt(x86::rax, genericNeg);
                // INT 原生路径：先解码运算（不 pop），检查溢出
                a.shl(x86::rax, 16);
                a.sar(x86::rax, 16);
                a.neg(x86::rax);  // rax = -value，设置 OF
                a.jo(genericNeg); // INT64 溢出（INT64_MIN 取负）→ C++ 辅助
                // INT48 范围检查
                a.mov(x86::rcx, x86::rax);
                a.shl(x86::rcx, 16);
                a.sar(x86::rcx, 16);
                a.cmp(x86::rcx, x86::rax);
                a.jne(genericNeg); // INT48 溢出 → C++ 辅助
                // 不溢出：重编码 + 原地写回栈顶
                // R161 perf: 用缓存的 rbx(JIT_INT48_MASK)/r14(JIT_INT_TAG_BASE) 替代 movabs
                a.and_(x86::rax, x86::rbx);
                a.or_(x86::rax, x86::r14);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                a.jmp(negEnd);
                // C++ 辅助路径：调用 jitNegateGeneric(value)
                a.bind(genericNeg);
                emitRecordTypeFeedback(x86::rax, chunkIdx);
                emitCallUnaryHelper(reinterpret_cast<void*>(&jitNegateGeneric));
                a.bind(negEnd);
                ip += 1;
                break;
            }

            // ---- R143 阶段 3b: 比较指令（INT 原生路径 + FLOAT/STRING C++ 辅助路径） ----
            case OpCode::OP_EQUAL: {
                // R143: INT/BOOL/NULL 走 raw bits 比较路径，FLOAT/STRING 走 C++ 辅助
                // （FLOAT 需处理 +0.0/-0.0/NaN，STRING 需比较内容）
                // R144 优化：用 shr 48 + cmp edx, imm32 替代 movabs + and + cmp（消除 3 条 movabs）
                Label genericEq = a.new_label();
                Label eqEnd = a.new_label();
                a.mov(x86::rax, x86::qword_ptr(x86::r15));    // right
                a.mov(x86::rcx, x86::qword_ptr(x86::r15, 8)); // left
                // 检查是否都是 boxed NaN（INT/BOOL/NULL/POINTER）—— FLOAT 不是 boxed NaN
                a.mov(x86::rdx, x86::rax);
                a.shr(x86::rdx, 48);
                a.cmp(x86::edx, 0x7FF8);
                a.jb(genericEq); // tag < 0x7FF8 → FLOAT → 走 C++ 辅助
                a.mov(x86::rdx, x86::rcx);
                a.shr(x86::rdx, 48);
                a.cmp(x86::edx, 0x7FF8);
                a.jb(genericEq); // left 是 FLOAT → 走 C++ 辅助
                // INT/BOOL/NULL 路径：raw bits 比较（注意 STRING 也是 POINTER tag=0x7FFB，
                // 但 STRING 比较需要内容比较，故 POINTER 也走 C++ 辅助）
                a.mov(x86::rdx, x86::rax);
                a.shr(x86::rdx, 48);
                a.cmp(x86::edx, 0x7FFB);
                a.je(genericEq); // POINTER → 可能是 STRING → 走 C++ 辅助
                a.mov(x86::rdx, x86::rcx);
                a.shr(x86::rdx, 48);
                a.cmp(x86::edx, 0x7FFB);
                a.je(genericEq);
                // INT/BOOL/NULL 原生路径：raw bits 比较
                a.add(x86::r15, 16); // pop 两个
                a.cmp(x86::rcx, x86::rax);
                a.sete(x86::cl);
                a.movzx(x86::rax, x86::cl);
                a.movabs(x86::rcx, JIT_BOOL_TAG_BASE);
                a.or_(x86::rax, x86::rcx);
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                a.jmp(eqEnd);
                // C++ 辅助路径：调用 jitEqualGeneric(left, right)
                a.bind(genericEq);
                emitRecordTypeFeedback(x86::rax, chunkIdx);
                emitCallBinaryHelper(reinterpret_cast<void*>(&jitEqualGeneric), /*needCtx*/ false,
                                     /*checkError*/ false);
                a.bind(eqEnd);
                ip += 1;
                break;
            }

            case OpCode::OP_NOT_EQUAL: {
                // R144 优化：用 shr 48 + cmp edx, imm32 替代 movabs + and + cmp（消除 3 条 movabs）
                Label genericNeq = a.new_label();
                Label neqEnd = a.new_label();
                a.mov(x86::rax, x86::qword_ptr(x86::r15));
                a.mov(x86::rcx, x86::qword_ptr(x86::r15, 8));
                a.mov(x86::rdx, x86::rax);
                a.shr(x86::rdx, 48);
                a.cmp(x86::edx, 0x7FF8);
                a.jb(genericNeq);
                a.mov(x86::rdx, x86::rcx);
                a.shr(x86::rdx, 48);
                a.cmp(x86::edx, 0x7FF8);
                a.jb(genericNeq);
                a.mov(x86::rdx, x86::rax);
                a.shr(x86::rdx, 48);
                a.cmp(x86::edx, 0x7FFB);
                a.je(genericNeq);
                a.mov(x86::rdx, x86::rcx);
                a.shr(x86::rdx, 48);
                a.cmp(x86::edx, 0x7FFB);
                a.je(genericNeq);
                // INT/BOOL/NULL 原生路径
                a.add(x86::r15, 16);
                a.cmp(x86::rcx, x86::rax);
                a.setne(x86::cl);
                a.movzx(x86::rax, x86::cl);
                a.movabs(x86::rcx, JIT_BOOL_TAG_BASE);
                a.or_(x86::rax, x86::rcx);
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                a.jmp(neqEnd);
                a.bind(genericNeq);
                emitRecordTypeFeedback(x86::rax, chunkIdx);
                emitCallBinaryHelper(reinterpret_cast<void*>(&jitNotEqualGeneric), /*needCtx*/ false,
                                     /*checkError*/ false);
                a.bind(neqEnd);
                ip += 1;
                break;
            }

            case OpCode::OP_LESS: {
                // R143: INT 走原生路径，FLOAT/STRING 走 C++ 辅助（cmpType=0）
                Label genericLt = a.new_label();
                Label ltEnd = a.new_label();
                a.mov(x86::rax, x86::qword_ptr(x86::r15));
                a.mov(x86::rcx, x86::qword_ptr(x86::r15, 8));
                emitCheckInt(x86::rax, genericLt);
                emitCheckInt(x86::rcx, genericLt);
                // INT 原生路径
                a.add(x86::r15, 16);
                a.shl(x86::rax, 16);
                a.sar(x86::rax, 16);
                a.shl(x86::rcx, 16);
                a.sar(x86::rcx, 16);
                a.cmp(x86::rcx, x86::rax);
                a.setl(x86::cl);
                a.movzx(x86::rax, x86::cl);
                a.movabs(x86::rcx, JIT_BOOL_TAG_BASE);
                a.or_(x86::rax, x86::rcx);
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                a.jmp(ltEnd);
                // C++ 辅助路径：调用 jitOrderedCompare(left, right, 0)
                a.bind(genericLt);
                emitRecordTypeFeedback(x86::rax, chunkIdx);
                emitCallOrderedCompare(reinterpret_cast<void*>(&jitOrderedCompare), 0);
                a.bind(ltEnd);
                ip += 1;
                break;
            }

            case OpCode::OP_GREATER: {
                Label genericGt = a.new_label();
                Label gtEnd = a.new_label();
                a.mov(x86::rax, x86::qword_ptr(x86::r15));
                a.mov(x86::rcx, x86::qword_ptr(x86::r15, 8));
                emitCheckInt(x86::rax, genericGt);
                emitCheckInt(x86::rcx, genericGt);
                a.add(x86::r15, 16);
                a.shl(x86::rax, 16);
                a.sar(x86::rax, 16);
                a.shl(x86::rcx, 16);
                a.sar(x86::rcx, 16);
                a.cmp(x86::rcx, x86::rax);
                a.setg(x86::cl);
                a.movzx(x86::rax, x86::cl);
                a.movabs(x86::rcx, JIT_BOOL_TAG_BASE);
                a.or_(x86::rax, x86::rcx);
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                a.jmp(gtEnd);
                a.bind(genericGt);
                emitRecordTypeFeedback(x86::rax, chunkIdx);
                emitCallOrderedCompare(reinterpret_cast<void*>(&jitOrderedCompare), 1);
                a.bind(gtEnd);
                ip += 1;
                break;
            }

            case OpCode::OP_LESS_EQUAL: {
                Label genericLe = a.new_label();
                Label leEnd = a.new_label();
                a.mov(x86::rax, x86::qword_ptr(x86::r15));
                a.mov(x86::rcx, x86::qword_ptr(x86::r15, 8));
                emitCheckInt(x86::rax, genericLe);
                emitCheckInt(x86::rcx, genericLe);
                a.add(x86::r15, 16);
                a.shl(x86::rax, 16);
                a.sar(x86::rax, 16);
                a.shl(x86::rcx, 16);
                a.sar(x86::rcx, 16);
                a.cmp(x86::rcx, x86::rax);
                a.setle(x86::cl);
                a.movzx(x86::rax, x86::cl);
                a.movabs(x86::rcx, JIT_BOOL_TAG_BASE);
                a.or_(x86::rax, x86::rcx);
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                a.jmp(leEnd);
                a.bind(genericLe);
                emitRecordTypeFeedback(x86::rax, chunkIdx);
                emitCallOrderedCompare(reinterpret_cast<void*>(&jitOrderedCompare), 2);
                a.bind(leEnd);
                ip += 1;
                break;
            }

            case OpCode::OP_GREATER_EQUAL: {
                Label genericGe = a.new_label();
                Label geEnd = a.new_label();
                a.mov(x86::rax, x86::qword_ptr(x86::r15));
                a.mov(x86::rcx, x86::qword_ptr(x86::r15, 8));
                emitCheckInt(x86::rax, genericGe);
                emitCheckInt(x86::rcx, genericGe);
                a.add(x86::r15, 16);
                a.shl(x86::rax, 16);
                a.sar(x86::rax, 16);
                a.shl(x86::rcx, 16);
                a.sar(x86::rcx, 16);
                a.cmp(x86::rcx, x86::rax);
                a.setge(x86::cl);
                a.movzx(x86::rax, x86::cl);
                a.movabs(x86::rcx, JIT_BOOL_TAG_BASE);
                a.or_(x86::rax, x86::rcx);
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                a.jmp(geEnd);
                a.bind(genericGe);
                emitRecordTypeFeedback(x86::rax, chunkIdx);
                emitCallOrderedCompare(reinterpret_cast<void*>(&jitOrderedCompare), 3);
                a.bind(geEnd);
                ip += 1;
                break;
            }

            case OpCode::OP_NOT: {
                // R143 阶段 3b：使用 jitTruthy 修复 FLOAT/STRING 的 truthiness 错误
                // jitTruthy 返回 1 (truthy) 或 0 (falsy)，取反后编码为 BOOL
#ifdef _WIN32
                a.mov(x86::rcx, x86::qword_ptr(x86::r15)); // arg1 = value
                a.sub(x86::rsp, 32);
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitTruthy));
                a.call(x86::rax);
                a.add(x86::rsp, 32);
#else
                a.mov(x86::rdi, x86::qword_ptr(x86::r15));
                a.sub(x86::rsp, 8);
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitTruthy));
                a.call(x86::rax);
                a.add(x86::rsp, 8);
#endif
                // rax = truthiness (0/1)，取反
                a.xor_(x86::rax, 1);
                a.movzx(x86::rax, x86::al);
                a.movabs(x86::rcx, JIT_BOOL_TAG_BASE);
                a.or_(x86::rax, x86::rcx);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                ip += 1;
                break;
            }

            case OpCode::OP_PRINT: {
                // R142 阶段 3a：调用 jitPrintValue(ctx, rawBits) 输出 NaN-boxing Value
                a.mov(x86::rax, x86::qword_ptr(x86::r15));
                a.add(x86::r15, 8);
#ifdef _WIN32
                a.mov(x86::rcx, x86::r12);
                a.mov(x86::rdx, x86::rax);
                a.sub(x86::rsp, 32);
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitPrintValue));
                a.call(x86::rax);
                a.add(x86::rsp, 32);
#else
                a.mov(x86::rdi, x86::r12);
                a.mov(x86::rsi, x86::rax);
                a.sub(x86::rsp, 8);
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitPrintValue));
                a.call(x86::rax);
                a.add(x86::rsp, 8);
#endif
                ip += 1;
                break;
            }

            case OpCode::OP_POP: {
                a.add(x86::r15, 8);
                ip += 1;
                break;
            }

            case OpCode::OP_DUP: {
                a.mov(x86::rax, x86::qword_ptr(x86::r15));
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                ip += 1;
                break;
            }

            // ---- R146 阶段 4：数组类型支持（C++ 辅助路径） ----
            // 设计：数组操作涉及动态 count 个元素 + 堆对象 COW，统一通过 C++ 辅助函数实现。
            // JIT 代码在调用前更新 ctx->stackTop = r15，辅助函数通过 ctx->stackTop 读写栈，
            // 调用后 JIT 代码从 ctx->stackTop 恢复 r15（辅助函数可能修改了栈顶）。
            case OpCode::OP_BUILD_ARRAY: {
                if (ip + 1 >= bytecodes.size()) {
                    compileError("OP_BUILD_ARRAY 操作数越界");
                    return nullptr;
                }
                uint8_t count = bytecodes[ip + 1];
                emitBuildArray(count);
                ip += 2;
                break;
            }
            case OpCode::OP_INDEX_GET: {
                emitIndexGet();
                ip += 1;
                break;
            }
            case OpCode::OP_INDEX_SET_LOCAL: {
                if (ip + 1 >= bytecodes.size()) {
                    compileError("OP_INDEX_SET_LOCAL 操作数越界");
                    return nullptr;
                }
                uint8_t slot = bytecodes[ip + 1];
                emitIndexSetLocal(slot);
                ip += 2;
                break;
            }
            // ---- R147 阶段 4b：dict/tuple + OP_INDEX_SET_VAR ----
            case OpCode::OP_BUILD_DICT: {
                if (ip + 1 >= bytecodes.size()) {
                    compileError("OP_BUILD_DICT 操作数越界");
                    return nullptr;
                }
                uint8_t pairCount = bytecodes[ip + 1];
                emitBuildDict(pairCount);
                ip += 2;
                break;
            }
            case OpCode::OP_BUILD_TUPLE: {
                if (ip + 1 >= bytecodes.size()) {
                    compileError("OP_BUILD_TUPLE 操作数越界");
                    return nullptr;
                }
                uint8_t count = bytecodes[ip + 1];
                emitBuildTuple(count);
                ip += 2;
                break;
            }
            case OpCode::OP_INDEX_SET_VAR: {
                // 3B 操作数：opcode + nameIdx(2B)
                // nameIdx 是常量池索引，指向变量名字符串
                // 编译期通过 globalNameToSlot_ 解析 nameIdx→slot
                if (ip + 2 >= bytecodes.size()) {
                    compileError("OP_INDEX_SET_VAR 操作数越界");
                    return nullptr;
                }
                uint16_t nameIdx =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                if (nameIdx >= chunk.constants.size()) {
                    compileError("OP_INDEX_SET_VAR 常量池索引越界");
                    return nullptr;
                }
                const std::string& varName = chunk.constants[nameIdx].stringVal();
                auto it = globalNameToSlot_.find(varName);
                if (it == globalNameToSlot_.end()) {
                    compileError("OP_INDEX_SET_VAR 未定义的全局变量: " + varName);
                    return nullptr;
                }
                emitIndexSetGlobal(it->second);
                ip += 3;
                break;
            }

            // ---- R148 阶段 4c-1+2：类支持（基础，不含方法调用） ----
            case OpCode::OP_CLASS_NEW: {
                // 4B 操作数：opcode + nameIdx(2B) + argCount(1B)
                if (ip + 3 >= bytecodes.size()) {
                    compileError("OP_CLASS_NEW 操作数越界");
                    return nullptr;
                }
                uint16_t nameIdx =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                uint8_t argCount = bytecodes[ip + 3];
                if (nameIdx >= chunk.constants.size()) {
                    compileError("OP_CLASS_NEW 常量池索引越界");
                    return nullptr;
                }
                emitClassNew(chunk.constants[nameIdx].stringVal().c_str(), argCount);
                ip += 4;
                break;
            }
            case OpCode::OP_INIT_FIELD: {
                // 3B 操作数：opcode + fieldNameIdx(2B)
                if (ip + 2 >= bytecodes.size()) {
                    compileError("OP_INIT_FIELD 操作数越界");
                    return nullptr;
                }
                uint16_t fieldIdx =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                if (fieldIdx >= chunk.constants.size()) {
                    compileError("OP_INIT_FIELD 常量池索引越界");
                    return nullptr;
                }
                emitInitField(chunk.constants[fieldIdx].stringVal().c_str());
                ip += 3;
                break;
            }
            case OpCode::OP_DEFINE_CLASS: {
                // 5B 操作数：opcode + nameIdx(2B) + superNameIdx(2B)
                // superNameIdx=0xFFFF 表示无父类
                if (ip + 4 >= bytecodes.size()) {
                    compileError("OP_DEFINE_CLASS 操作数越界");
                    return nullptr;
                }
                uint16_t nameIdx =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                uint16_t superIdx =
                    static_cast<uint16_t>(bytecodes[ip + 3]) | (static_cast<uint16_t>(bytecodes[ip + 4]) << 8);
                if (nameIdx >= chunk.constants.size()) {
                    compileError("OP_DEFINE_CLASS 类名常量池索引越界");
                    return nullptr;
                }
                // R148 fix: 直接引用 chunk.constants 中的字符串，避免局部 std::string
                // 离开作用域后 c_str() 悬垂（emitDefineClass 通过 movabs 将指针嵌入
                // JIT 机器码，execute() 运行时才访问，chunk 随 CompileResult 存活）
                const char* className = chunk.constants[nameIdx].stringVal().c_str();
                const char* superClassName = nullptr;
                if (superIdx != 0xFFFF) {
                    if (superIdx >= chunk.constants.size()) {
                        compileError("OP_DEFINE_CLASS 父类名常量池索引越界");
                        return nullptr;
                    }
                    superClassName = chunk.constants[superIdx].stringVal().c_str();
                }
                emitDefineClass(className, superClassName);
                ip += 5;
                break;
            }
            case OpCode::OP_MEMBER_GET: {
                // 3B 操作数：opcode + fieldNameIdx(2B)
                if (ip + 2 >= bytecodes.size()) {
                    compileError("OP_MEMBER_GET 操作数越界");
                    return nullptr;
                }
                uint16_t fieldIdx =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                if (fieldIdx >= chunk.constants.size()) {
                    compileError("OP_MEMBER_GET 常量池索引越界");
                    return nullptr;
                }
                emitMemberGet(chunk.constants[fieldIdx].stringVal().c_str());
                ip += 3;
                break;
            }
            case OpCode::OP_MEMBER_SET_VAR: {
                // 5B 操作数：opcode + varIdx(2B) + fieldNameIdx(2B)
                // varIdx 是常量池索引（变量名），编译期通过 globalNameToSlot_ 解析为 slot
                if (ip + 4 >= bytecodes.size()) {
                    compileError("OP_MEMBER_SET_VAR 操作数越界");
                    return nullptr;
                }
                uint16_t varIdx =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                uint16_t fieldIdx =
                    static_cast<uint16_t>(bytecodes[ip + 3]) | (static_cast<uint16_t>(bytecodes[ip + 4]) << 8);
                if (varIdx >= chunk.constants.size() || fieldIdx >= chunk.constants.size()) {
                    compileError("OP_MEMBER_SET_VAR 常量池索引越界");
                    return nullptr;
                }
                const std::string& varName = chunk.constants[varIdx].stringVal();
                auto varIt = globalNameToSlot_.find(varName);
                if (varIt == globalNameToSlot_.end()) {
                    compileError("OP_MEMBER_SET_VAR 未定义的全局变量: " + varName);
                    return nullptr;
                }
                emitMemberSetVar(varIt->second, chunk.constants[fieldIdx].stringVal().c_str());
                ip += 5;
                break;
            }
            case OpCode::OP_MEMBER_SET_LOCAL: {
                // 4B 操作数：opcode + slot(1B) + fieldNameIdx(2B)
                if (ip + 3 >= bytecodes.size()) {
                    compileError("OP_MEMBER_SET_LOCAL 操作数越界");
                    return nullptr;
                }
                uint8_t slot = bytecodes[ip + 1];
                uint16_t fieldIdx =
                    static_cast<uint16_t>(bytecodes[ip + 2]) | (static_cast<uint16_t>(bytecodes[ip + 3]) << 8);
                if (fieldIdx >= chunk.constants.size()) {
                    compileError("OP_MEMBER_SET_LOCAL 常量池索引越界");
                    return nullptr;
                }
                emitMemberSetLocal(slot, chunk.constants[fieldIdx].stringVal().c_str());
                ip += 4;
                break;
            }

            // ---- R148: OP_TYPE_CHECK 兜底（仅跳过，不做实际类型检查） ----
            // JIT 性能优先，类型错误在编译期已部分检查；运行时类型检查是兜底，
            // JIT 跳过以支持带类型注解的类实例化（var p: Foo; 触发 OP_CLASS_NEW）。
            // 栈语义：peek 栈顶值不弹出，JIT 不做任何操作直接跳过 3 字节操作数。
            case OpCode::OP_TYPE_CHECK: {
                if (ip + 2 >= bytecodes.size()) {
                    compileError("OP_TYPE_CHECK 操作数越界");
                    return nullptr;
                }
                ip += 3;
                break;
            }

            // ---- R141: 局部变量指令（r13 = basePointer，[r13 - slot*8]） ----
            // StackVM 语义：bp + slot 向上增长；JIT 栈向下增长，
            // r13 指向 slot 0（最高地址），slot N 在 [r13 - N*8]
            case OpCode::OP_GET_LOCAL: {
                if (ip + 1 >= bytecodes.size()) {
                    compileError("OP_GET_LOCAL 操作数越界");
                    return nullptr;
                }
                uint8_t slot = bytecodes[ip + 1];
                // mov rax, [r13 - slot*8]; push rax
                a.mov(x86::rax, x86::qword_ptr(x86::r13, -static_cast<int32_t>(slot) * 8));
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                ip += 2;
                break;
            }

            case OpCode::OP_SET_LOCAL: {
                // StackVM 语义：peek(0) 写入，不弹出（编译器后续生成 OP_POP）
                if (ip + 1 >= bytecodes.size()) {
                    compileError("OP_SET_LOCAL 操作数越界");
                    return nullptr;
                }
                uint8_t slot = bytecodes[ip + 1];
                // mov rax, [r15]; mov [r13 - slot*8], rax（不调整 r15）
                a.mov(x86::rax, x86::qword_ptr(x86::r15));
                a.mov(x86::qword_ptr(x86::r13, -static_cast<int32_t>(slot) * 8), x86::rax);
                ip += 2;
                break;
            }

            // ---- R141: 跳转指令（2B 绝对偏移，使用第一遍收集的 Label） ----
            case OpCode::OP_JUMP: {
                if (ip + 2 >= bytecodes.size()) {
                    compileError("OP_JUMP 操作数越界");
                    return nullptr;
                }
                uint16_t target =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                auto targetIt = jumpLabels.find(target);
                if (targetIt == jumpLabels.end()) {
                    compileError("OP_JUMP 目标位置无 Label: " + std::to_string(target));
                    return nullptr;
                }
                a.jmp(targetIt->second);
                ip += 3;
                break;
            }

            case OpCode::OP_JUMP_IF_FALSE: {
                // R143 阶段 3b：使用 jitTruthy 修复 FLOAT/STRING 的 truthiness 错误
                // StackVM 语义：不弹出条件值，编译器在后续显式生成 OP_POP
                if (ip + 2 >= bytecodes.size()) {
                    compileError("OP_JUMP_IF_FALSE 操作数越界");
                    return nullptr;
                }
                uint16_t target =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                auto targetIt = jumpLabels.find(target);
                if (targetIt == jumpLabels.end()) {
                    compileError("OP_JUMP_IF_FALSE 目标位置无 Label: " + std::to_string(target));
                    return nullptr;
                }
                // 调用 jitTruthy(value) → rax = 0 (falsy) 或 1 (truthy)
#ifdef _WIN32
                a.mov(x86::rcx, x86::qword_ptr(x86::r15)); // arg1 = value
                a.sub(x86::rsp, 32);
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitTruthy));
                a.call(x86::rax);
                a.add(x86::rsp, 32);
#else
                a.mov(x86::rdi, x86::qword_ptr(x86::r15));
                a.sub(x86::rsp, 8);
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitTruthy));
                a.call(x86::rax);
                a.add(x86::rsp, 8);
#endif
                // rax = truthiness，若 0 (falsy) 则跳转
                a.test(x86::rax, x86::rax);
                a.jz(targetIt->second);
                ip += 3;
                break;
            }

            case OpCode::OP_LOOP: {
                if (ip + 2 >= bytecodes.size()) {
                    compileError("OP_LOOP 操作数越界");
                    return nullptr;
                }
                uint16_t target =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                auto targetIt = jumpLabels.find(target);
                if (targetIt == jumpLabels.end()) {
                    compileError("OP_LOOP 目标位置无 Label: " + std::to_string(target));
                    return nullptr;
                }

                // R157: OSR 回边计数器注入（仅当 osrLoopThresholds_[chunkIdx] > 0 时）
                // 每次循环回边递增计数器，超阈值时触发特化重编译。
                // 编译期过滤：阈值为 0 时完全跳过代码生成，运行时零开销（R151 教训）。
                if (chunkIdx < osrLoopThresholds_.size() && osrLoopThresholds_[chunkIdx] > 0) {
                    Label skipOsr = a.new_label();

                    // 1. 递增 osrLoopCounts_[chunkIdx]
                    a.mov(x86::rcx, x86::qword_ptr(x86::r12, 168)); // rcx = osrLoopCountsPtr
                    a.inc(x86::qword_ptr(x86::rcx, static_cast<int32_t>(chunkIdx * 8)));

                    // 2. 检查 osrRecompiledFlags_[chunkIdx] == 0（避免重复触发）
                    a.mov(x86::rcx, x86::qword_ptr(x86::r12, 184)); // rcx = osrRecompiledFlagsPtr
                    a.mov(x86::rax, x86::qword_ptr(x86::rcx, static_cast<int32_t>(chunkIdx * 8)));
                    a.test(x86::rax, x86::rax);
                    a.jnz(skipOsr); // 已触发过，跳过

                    // 3. 比较 osrLoopCounts_[chunkIdx] 与 osrLoopThresholds_[chunkIdx]
                    a.mov(x86::rcx, x86::qword_ptr(x86::r12, 168)); // rcx = osrLoopCountsPtr
                    a.mov(x86::rax, x86::qword_ptr(x86::rcx, static_cast<int32_t>(chunkIdx * 8)));
                    a.mov(x86::rcx, x86::qword_ptr(x86::r12, 176)); // rcx = osrLoopThresholdsPtr
                    a.cmp(x86::rax, x86::qword_ptr(x86::rcx, static_cast<int32_t>(chunkIdx * 8)));
                    a.jb(skipOsr); // 未达阈值，跳过

                    // 4. 达到阈值：标记为已触发
                    a.mov(x86::rcx, x86::qword_ptr(x86::r12, 184)); // rcx = osrRecompiledFlagsPtr
                    a.mov(x86::qword_ptr(x86::rcx, static_cast<int32_t>(chunkIdx * 8)), 1);

                    // 5. 根据 osrMigrationMode_ 选择 OSR 策略
                    //    栈对齐：chunk 内部 rsp%16==8，sub 40 后 rsp%16==0（R151 教训）
                    if (osrMigrationMode_) {
                        // R158: 真正 OSR 栈帧迁移
                        // a. 保存 r13/r15 到邮箱（OSR 入口点将从此恢复栈帧）
                        a.mov(x86::qword_ptr(x86::r12, 192), x86::r13); // osrSavedBp = r13
                        a.mov(x86::qword_ptr(x86::r12, 200), x86::r15); // osrSavedSp = r15
                        // b. 同步栈顶
                        a.mov(x86::qword_ptr(x86::r12, 48), x86::r15);
                        // c. 调用 jitTriggerOsrMigration(ctx, chunkIdx)
                        //    触发特化重编译（含 OSR 入口点生成），通过 ctx->osrEntryPoint 返回
                        a.mov(x86::rcx, x86::r12);                       // arg1 = ctx
                        a.mov(x86::rdx, static_cast<int32_t>(chunkIdx)); // arg2 = chunkIdx
#ifdef _WIN32
                        a.sub(x86::rsp, 40); // shadow space (32) + 8B 对齐填充
#else
                        a.sub(x86::rsp, 8); // 16-byte alignment
#endif
                        a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitTriggerOsrMigration));
                        a.call(x86::rax);
#ifdef _WIN32
                        a.add(x86::rsp, 40);
#else
                        a.add(x86::rsp, 8);
#endif
                        // d. 检查返回值（0=成功）
                        a.test(x86::rax, x86::rax);
                        a.jnz(skipOsr); // 失败，跳过迁移走正常回边
                        // e. 加载 osrEntryPoint（offset 208）
                        a.mov(x86::rax, x86::qword_ptr(x86::r12, 208));
                        a.test(x86::rax, x86::rax);
                        a.jz(skipOsr); // osrEntryPoint 为 null，跳过迁移
                        // f. jmp osrEntryPoint（真正 OSR 栈帧迁移，跳到特化版本的 OSR 入口点）
                        a.jmp(x86::rax);
                    } else {
                        // R157: 简化版 OSR（仅触发特化重编译，不迁移栈帧，下次 execute 生效）
                        a.mov(x86::qword_ptr(x86::r12, 48), x86::r15);   // 同步栈顶
                        a.mov(x86::rcx, x86::r12);                       // arg1 = ctx
                        a.mov(x86::rdx, static_cast<int32_t>(chunkIdx)); // arg2 = chunkIdx
#ifdef _WIN32
                        a.sub(x86::rsp, 40); // shadow space (32) + 8B 对齐填充
#else
                        a.sub(x86::rsp, 8); // 16-byte alignment
#endif
                        a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitTriggerOsrRecompile));
                        a.call(x86::rax);
#ifdef _WIN32
                        a.add(x86::rsp, 40);
#else
                        a.add(x86::rsp, 8);
#endif
                        a.mov(x86::r15, x86::qword_ptr(x86::r12, 48)); // 恢复栈顶
                    }

                    a.bind(skipOsr);
                }

                a.jmp(targetIt->second);

                // R158: OSR 入口点生成（仅 osrEntryGenMode_ 且目标 chunk 匹配时）
                // 在正常 jmp loop 回边之后生成 OSR 入口 Label，仅可通过外部 jmp 到达。
                // OSR 入口点恢复 r13/r15 从邮箱，然后跳转到循环回边目标（特化版本中的循环起点）。
                if (osrEntryGenMode_ && chunkIdx == osrTargetChunkIdx_ && !osrEntryLabelGenerated_) {
                    osrEntryLabel_ = a.new_label();
                    a.bind(osrEntryLabel_);
                    // 恢复 r13/r15 从邮箱（OSR 触发时由 JIT 代码保存）
                    a.mov(x86::r13, x86::qword_ptr(x86::r12, 192)); // r13 = osrSavedBp
                    a.mov(x86::r15, x86::qword_ptr(x86::r12, 200)); // r15 = osrSavedSp
                    // 跳转到循环回边目标（特化版本中的循环起点）
                    a.jmp(targetIt->second);
                    osrEntryLabelGenerated_ = true;
                }

                ip += 3;
                break;
            }

            // ---- R139: 全局变量 slot 指令 ----
            case OpCode::OP_GET_GLOBAL: {
                if (ip + 2 >= bytecodes.size()) {
                    compileError("OP_GET_GLOBAL 操作数越界");
                    return nullptr;
                }
                uint16_t slot =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                a.mov(x86::rcx, x86::qword_ptr(x86::r12, 24));
                a.mov(x86::rax, x86::qword_ptr(x86::rcx, static_cast<int32_t>(slot) * 8));
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                ip += 3;
                break;
            }

            case OpCode::OP_SET_GLOBAL: {
                if (ip + 2 >= bytecodes.size()) {
                    compileError("OP_SET_GLOBAL 操作数越界");
                    return nullptr;
                }
                uint16_t slot =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                a.mov(x86::rax, x86::qword_ptr(x86::r15));
                a.add(x86::r15, 8);
                a.mov(x86::rcx, x86::qword_ptr(x86::r12, 24));
                a.mov(x86::qword_ptr(x86::rcx, static_cast<int32_t>(slot) * 8), x86::rax);
                ip += 3;
                break;
            }

            case OpCode::OP_DEFINE_GLOBAL: {
                // 语义与 OP_SET_GLOBAL 相同
                if (ip + 2 >= bytecodes.size()) {
                    compileError("OP_DEFINE_GLOBAL 操作数越界");
                    return nullptr;
                }
                uint16_t slot =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                a.mov(x86::rax, x86::qword_ptr(x86::r15));
                a.add(x86::r15, 8);
                a.mov(x86::rcx, x86::qword_ptr(x86::r12, 24));
                a.mov(x86::qword_ptr(x86::rcx, static_cast<int32_t>(slot) * 8), x86::rax);
                ip += 3;
                break;
            }

            // ---- R141: 函数调用指令（OP_CALL，4B: op + nameIdx(2B) + argCount(1B)） ----
            // StackVM 语义：参数已按序 push（arg0 在底，argN-1 在顶），按名查找 functionChunks_
            // JIT 实现：保存调用者帧 → 预分配 extraSlots → 设置 r13 → jmp 函数 Label
            case OpCode::OP_CALL: {
                if (ip + 3 >= bytecodes.size()) {
                    compileError("OP_CALL 操作数越界");
                    return nullptr;
                }
                uint16_t nameIdx =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                uint8_t argCount = bytecodes[ip + 3];
                if (nameIdx >= chunk.constants.size()) {
                    compileError("OP_CALL 常量池索引越界: " + std::to_string(nameIdx));
                    return nullptr;
                }
                std::string funName = chunk.constants[nameIdx].stringVal();
                auto funcIt = funcTable.find(funName);
                if (funcIt == funcTable.end()) {
                    // R149: funcTable 找不到时检查是否是类名构造（`Point(3, 4)` 语法）
                    // MiniLang 编译器将 `ClassName(args)` 编译为 OP_CALL 而非 OP_CLASS_NEW
                    // 与 StackVM executeCallByName 中 classInfo_.find(funName) fallback 对齐
                    if (classNameSet_.find(funName) != classNameSet_.end()) {
                        // 是类名构造：栈布局与 OP_CLASS_NEW 一致（[argN-1]...[arg0]）
                        // 直接复用 emitClassNew 路径（含 R149 init 方法调用支持）
                        emitClassNew(chunk.constants[nameIdx].stringVal().c_str(), argCount);
                        ip += 4;
                        break;
                    }
                    compileError("JIT 阶段 2b 不支持的函数调用（未注册）: " + funName);
                    return nullptr;
                }

                // R161 perf: 混合模式优化 — 非 lazy 模式 + 无默认参数 + 无 upvalues + 无内部闭包时
                // 走编译期 jmp entryLabel 快速路径（恢复 R141 风格），消除 jitCallByName 的
                // C++ 调用 + unordered_map 查找 + std::string 构造 + arg pop/push 开销。
                // fib(30) 约 1.6M 次 OP_CALL，邮箱模式导致 0.60x 加速比，快速路径恢复到 >1x。
                // 条件说明：
                //   !lazyMode_           — eager 模式所有 chunk 已编译，entryLabel 已绑定
                //   defaultConstIndices.empty() — 无默认参数，argCount == arity（编译器保证）
                //   upvalues.empty()     — 函数不捕获 upvalue，无 OP_GET_UPVALUE/OP_SET_UPVALUE
                //   !hasInnerClosures    — 函数体无 OP_CLOSURE，不访问 frameUpvaluesStack_
                // 不满足条件时走下方 R157 邮箱模式（支持 lazy compilation + 默认参数 + 闭包）
                if (!lazyMode_ && funcIt->second.chunk && funcIt->second.chunk->defaultConstIndices.empty() &&
                    funcIt->second.chunk->upvalues.empty() && !funcIt->second.hasInnerClosures) {

                    Label returnLabel = a.new_label();
                    Label overflowLabel = a.new_label();
                    int localCount = funcIt->second.localCount;
                    int arityVal = funcIt->second.arity;
                    int extraSlots = localCount - arityVal;

                    // 1. 存储 callerBp (r13) 到 ctx->callerBp (offset 96)
                    a.mov(x86::qword_ptr(x86::r12, 96), x86::r13);

                    // 2. MAX_FRAMES 检查（与 jitCallByName 对齐）
                    a.mov(x86::rcx, x86::qword_ptr(x86::r12, 40)); // rcx = &frameCount
                    a.mov(x86::rdx, x86::qword_ptr(x86::rcx));     // rdx = *frameCount
                    a.mov(x86::rax, static_cast<int32_t>(RuntimeLimits::MAX_FRAMES));
                    a.cmp(x86::rdx, x86::rax);
                    a.jae(overflowLabel);

                    // 3. 预分配 extraSlots（填充 JIT_NULL_BITS）
                    //    参数已在栈上（r15 指向 argN-1），只需在参数下方 push extraSlots
                    if (extraSlots > 0) {
                        a.sub(x86::r15, static_cast<int32_t>(extraSlots * 8));
                        a.movabs(x86::rax, static_cast<int64_t>(JIT_NULL_BITS));
                        for (int i = 0; i < extraSlots; ++i) {
                            a.mov(x86::qword_ptr(x86::r15, static_cast<int32_t>(i * 8)), x86::rax);
                        }
                    }

                    // 4. 构造 JitFrame at frames[*frameCount]
                    //    rdx 仍持有 *frameCount（idx），重新加载（被 cmp 覆盖过）
                    a.mov(x86::rcx, x86::qword_ptr(x86::r12, 40)); // rcx = &frameCount
                    a.mov(x86::rdx, x86::qword_ptr(x86::rcx));     // rdx = idx
                    a.mov(x86::rax, x86::qword_ptr(x86::r12, 32)); // rax = frames base
                    a.imul(x86::rdx, x86::rdx, 72);
                    a.add(x86::rax, x86::rdx); // rax = &frames[idx]

                    // frame->callerBp (offset 0) = r13
                    a.mov(x86::qword_ptr(x86::rax, 0), x86::r13);

                    // frame->callerSp (offset 8) = callerSpAfterPop = r15 + (extraSlots + argCount) * 8
                    //    当前 r15 指向 first extraSlot（或 argN-1 若 extraSlots==0）
                    //    callerSpAfterPop 是参数上方地址（OP_RETURN 恢复调用者 r15 用）
                    int callerSpOff = (extraSlots + argCount) * 8;
                    a.lea(x86::rcx, x86::qword_ptr(x86::r15, static_cast<int32_t>(callerSpOff)));
                    a.mov(x86::qword_ptr(x86::rax, 8), x86::rcx);

                    // frame->returnAddr (offset 16) = returnLabel
                    a.lea(x86::rcx, x86::qword_ptr(returnLabel));
                    a.mov(x86::qword_ptr(x86::rax, 16), x86::rcx);

                    // frame->receiverSlotPtr/methodFieldOrder/methodBp = nullptr (offset 24/32/40)
                    a.mov(x86::qword_ptr(x86::rax, 24), 0);
                    a.mov(x86::qword_ptr(x86::rax, 32), 0);
                    a.mov(x86::qword_ptr(x86::rax, 40), 0);
                    // frame->isMethodCall/isInitCall/fieldCount = 0 (offset 48/56/64)
                    a.mov(x86::qword_ptr(x86::rax, 48), 0);
                    a.mov(x86::qword_ptr(x86::rax, 56), 0);
                    a.mov(x86::qword_ptr(x86::rax, 64), 0);

                    // 5. *frameCount = idx + 1
                    a.mov(x86::rcx, x86::qword_ptr(x86::r12, 40)); // rcx = &frameCount
                    a.mov(x86::rdx, x86::qword_ptr(x86::rcx));     // rdx = idx
                    a.inc(x86::rdx);
                    a.mov(x86::qword_ptr(x86::rcx), x86::rdx);

                    // 6. 设置 r13 = r15 + (localCount - 1) * 8
                    //    普通函数帧无 this/fields，slot 0 是 arg0（最高地址）
                    a.mov(x86::rax, static_cast<int32_t>((localCount - 1) * 8));
                    a.lea(x86::r13, x86::qword_ptr(x86::r15, x86::rax));

                    // 7. jmp entryLabel（直接跳转，0 次 C++ 调用！）
                    a.jmp(funcIt->second.entryLabel);

                    // 8. 栈溢出错误路径（必须在 returnLabel 之前，避免 fall-through 误触发）
                    //    罕见路径，C++ 调用开销可忽略；错误消息与 jitCallByName 对齐
                    a.bind(overflowLabel);
                    a.mov(x86::rcx, x86::r12); // arg1 = ctx
                    a.sub(x86::rsp, 40);       // shadow space + 对齐
                    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitReportStackOverflow));
                    a.call(x86::rax);
                    a.add(x86::rsp, 40);
                    a.jmp(epilogue);

                    // 9. 绑定返回 Label（函数 OP_RETURN 后跳回此处）
                    a.bind(returnLabel);

                    ip += 4;
                    break;
                }

                // R157: OP_CALL 改为运行时邮箱模式（支持 lazy compilation）
                // 原 R141 实现使用编译期 a.jmp(info.entryLabel)，要求所有 chunk 在同一 CodeHolder。
                // R157 改为调用 jitCallByName C++ 辅助函数（通过邮箱返回入口地址 + localCount），
                // 然后设置 returnAddr + r13 + jmp 入口（参考 emitMethodCall/OP_CALL_EXPR 模式）。
                // lazyMode_ 下，jitCallByName 在 entryPtr 为 null 时触发 compileSingleChunkLazy。
                // R161 perf: 仅在 lazyMode_ 或有默认参数/upvalues/内部闭包时走此路径
                Label returnLabel = a.new_label();
                const char* funNamePtr = chunk.constants[nameIdx].stringVal().c_str();

                // 1. 存储 callerBp (r13) 到 ctx->callerBp (offset 96)
                a.mov(x86::qword_ptr(x86::r12, 96), x86::r13);

                // 2. 同步栈顶到 ctx->stackTop (offset 48)
                a.mov(x86::qword_ptr(x86::r12, 48), x86::r15);

                // 3. 调用 jitCallByName(ctx, funName, argCount)
                //    辅助函数通过 ctx->stackTop 操作栈（pop args + push args/defaults/extraSlots）
                //    通过邮箱返回：ctx->methodEntryPtr = 入口地址, ctx->methodLocalCount = localCount
                //    lazyMode_ 下 entryPtr 为 null 时触发 compileSingleChunkLazy
#ifdef _WIN32
                a.mov(x86::rcx, x86::r12);                                  // arg1 = ctx
                a.movabs(x86::rdx, reinterpret_cast<uint64_t>(funNamePtr)); // arg2 = funName
                a.mov(x86::r8, static_cast<int32_t>(argCount));             // arg3 = argCount
                a.sub(x86::rsp, 40);                                        // shadow space (32) + 8B 对齐填充
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitCallByName));
                a.call(x86::rax);
                a.add(x86::rsp, 40);
#else
                a.mov(x86::rdi, x86::r12);
                a.movabs(x86::rsi, reinterpret_cast<uint64_t>(funNamePtr));
                a.mov(x86::edx, static_cast<int32_t>(argCount));
                a.sub(x86::rsp, 8); // 16-byte alignment
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitCallByName));
                a.call(x86::rax);
                a.add(x86::rsp, 8);
#endif

                // 4. 恢复 r15（jitCallByName 修改了栈）
                a.mov(x86::r15, x86::qword_ptr(x86::r12, 48));

                // 5. 检查错误
                a.mov(x86::rax, x86::qword_ptr(x86::r12, 8));
                a.movzx(x86::rax, x86::byte_ptr(x86::rax));
                a.test(x86::rax, x86::rax);
                a.jnz(epilogue);

                // 6. 在新 frame 上设置 returnAddr（jitCallByName 无法访问 JIT Label，需在调用后写入）
                //    新 frame 在 frames[*frameCount - 1]
                a.mov(x86::rcx, x86::qword_ptr(x86::r12, 40)); // rcx = &frameCount_
                a.mov(x86::rdx, x86::qword_ptr(x86::rcx));     // rdx = *frameCount
                a.sub(x86::rdx, 1);                            // rdx = idx（新 frame 索引）
                a.mov(x86::rcx, x86::qword_ptr(x86::r12, 32)); // rcx = frames base
                a.imul(x86::rdx, x86::rdx, 72);
                a.add(x86::rcx, x86::rdx); // rcx = &frames[idx]
                a.lea(x86::rax, x86::qword_ptr(returnLabel));
                a.mov(x86::qword_ptr(x86::rcx, 16), x86::rax); // frame->returnAddr = returnLabel

                // 7. 设置 r13 = r15 + (methodLocalCount - 1) * 8
                //    普通函数帧无 this/fields，slot 0 是 arg0（最高地址）
                a.mov(x86::rax, x86::qword_ptr(x86::r12, 80)); // rax = methodLocalCount
                a.dec(x86::rax);
                a.imul(x86::rax, x86::rax, 8);
                a.lea(x86::r13, x86::qword_ptr(x86::r15, x86::rax));

                // 8. jmp methodEntryPtr
                a.mov(x86::rax, x86::qword_ptr(x86::r12, 72)); // rax = methodEntryPtr
                a.jmp(x86::rax);

                // 9. 绑定返回 Label（函数 OP_RETURN 后跳回此处）
                a.bind(returnLabel);

                ip += 4;
                break;
            }

            // ---- R149: 方法调用指令（OP_METHOD_CALL，7B） ----
            // 操作数：opcode(1) + nameIdx(2) + argCount(1) + receiverVarIdx(2) + receiverLocalSlotByte(1)
            //   nameIdx              方法名常量池索引
            //   argCount             实际参数个数
            //   receiverVarIdx       接收者变量名常量池索引（0xFFFF=无全局接收者 writeBack）
            //   receiverLocalSlotByte 接收者局部 slot（0xFF=无局部接收者 writeBack）
            // 栈布局（调用前）：[argN-1]...[arg0][receiver] ← r15 指向 argN-1
            // 栈布局（调用后）：[extraSlots...][defaults...][args...][fields...][this]
            case OpCode::OP_METHOD_CALL: {
                if (ip + 6 >= bytecodes.size()) {
                    compileError("OP_METHOD_CALL 操作数越界");
                    return nullptr;
                }
                uint16_t nameIdx =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                uint8_t argCount = bytecodes[ip + 3];
                uint16_t receiverVarIdx =
                    static_cast<uint16_t>(bytecodes[ip + 4]) | (static_cast<uint16_t>(bytecodes[ip + 5]) << 8);
                uint8_t receiverLocalSlotByte = bytecodes[ip + 6];
                if (nameIdx >= chunk.constants.size()) {
                    compileError("OP_METHOD_CALL 方法名常量池索引越界");
                    return nullptr;
                }
                // R148 教训：直接引用 chunk.constants 中的字符串，避免局部 std::string
                // 析构导致 movabs 嵌入的指针悬垂
                const char* methodName = chunk.constants[nameIdx].stringVal().c_str();

                // 编译期解析 receiverVarIdx → 全局 slot（若 receiverVarIdx != 0xFFFF）
                int receiverGlobalSlot = -1;
                if (receiverLocalSlotByte == 0xFF && receiverVarIdx != 0xFFFF) {
                    if (receiverVarIdx >= chunk.constants.size()) {
                        compileError("OP_METHOD_CALL 接收者变量名常量池索引越界");
                        return nullptr;
                    }
                    const std::string& varName = chunk.constants[receiverVarIdx].stringVal();
                    auto it = globalNameToSlot_.find(varName);
                    if (it == globalNameToSlot_.end()) {
                        compileError("OP_METHOD_CALL 未定义的全局接收者: " + varName);
                        return nullptr;
                    }
                    receiverGlobalSlot = it->second;
                }

                emitMethodCall(methodName, argCount, receiverLocalSlotByte, receiverGlobalSlot,
                               /*isSuperCall=*/false, /*superClassName=*/nullptr);
                ip += 7;
                break;
            }

            // ---- R149: super 方法调用指令（OP_SUPER_CALL，9B） ----
            // 操作数：opcode(1) + nameIdx(2) + argCount(1) + receiverVarIdx(2) +
            //         receiverLocalSlotByte(1) + classIdx(2)
            //   classIdx 父类名常量池索引（编译期编码，避免运行时实例类名查找死循环）
            case OpCode::OP_SUPER_CALL: {
                if (ip + 8 >= bytecodes.size()) {
                    compileError("OP_SUPER_CALL 操作数越界");
                    return nullptr;
                }
                uint16_t nameIdx =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                uint8_t argCount = bytecodes[ip + 3];
                uint16_t receiverVarIdx =
                    static_cast<uint16_t>(bytecodes[ip + 4]) | (static_cast<uint16_t>(bytecodes[ip + 5]) << 8);
                uint8_t receiverLocalSlotByte = bytecodes[ip + 6];
                uint16_t classIdx =
                    static_cast<uint16_t>(bytecodes[ip + 7]) | (static_cast<uint16_t>(bytecodes[ip + 8]) << 8);
                if (nameIdx >= chunk.constants.size()) {
                    compileError("OP_SUPER_CALL 方法名常量池索引越界");
                    return nullptr;
                }
                if (classIdx >= chunk.constants.size()) {
                    compileError("OP_SUPER_CALL 父类名常量池索引越界");
                    return nullptr;
                }
                const char* methodName = chunk.constants[nameIdx].stringVal().c_str();
                const char* superClassName = chunk.constants[classIdx].stringVal().c_str();

                int receiverGlobalSlot = -1;
                if (receiverLocalSlotByte == 0xFF && receiverVarIdx != 0xFFFF) {
                    if (receiverVarIdx >= chunk.constants.size()) {
                        compileError("OP_SUPER_CALL 接收者变量名常量池索引越界");
                        return nullptr;
                    }
                    const std::string& varName = chunk.constants[receiverVarIdx].stringVal();
                    auto it = globalNameToSlot_.find(varName);
                    if (it == globalNameToSlot_.end()) {
                        compileError("OP_SUPER_CALL 未定义的全局接收者: " + varName);
                        return nullptr;
                    }
                    receiverGlobalSlot = it->second;
                }

                emitMethodCall(methodName, argCount, receiverLocalSlotByte, receiverGlobalSlot,
                               /*isSuperCall=*/true, superClassName);
                ip += 9;
                break;
            }

            // ---- R156: 闭包创建指令（OP_CLOSURE，变长: 4 + 2*upvalueCount） ----
            // R156 完整版：支持 upvalue 捕获（isLocal 直接捕获 + passthrough 透传）
            // 与 StackVM executeClosure 对齐（VMCalls.cpp:1022-1083）
            case OpCode::OP_CLOSURE: {
                if (ip + 3 >= bytecodes.size()) {
                    compileError("OP_CLOSURE 操作数越界");
                    return nullptr;
                }
                uint16_t nameIdx =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                uint8_t upvalueCount = bytecodes[ip + 3];
                if (static_cast<size_t>(ip + 4 + 2 * upvalueCount) > bytecodes.size()) {
                    compileError("OP_CLOSURE upvalue 描述符越界");
                    return nullptr;
                }
                if (nameIdx >= chunk.constants.size()) {
                    compileError("OP_CLOSURE 常量池索引越界");
                    return nullptr;
                }
                std::string funName = chunk.constants[nameIdx].stringVal();
                auto funcIt = funcTable.find(funName);
                if (funcIt == funcTable.end() || !funcIt->second.chunk) {
                    compileError("OP_CLOSURE 未注册的函数: " + funName);
                    return nullptr;
                }
                // R148 教训：直接引用 chunk.name（result 生命期跨越 execute），避免局部 string 析构
                const char* funNamePtr = funcIt->second.chunk->name.c_str();
                const void* chunkPtr = funcIt->second.chunk;
                // R156: upvalue 描述符指针（指向字节码中第一个 [isLocal, index] 对）
                const uint8_t* upvalueDescs = bytecodes.data() + ip + 4;

                // 同步栈顶 + 当前帧 basePointer 到 ctx
                a.mov(x86::qword_ptr(x86::r12, 48), x86::r15);  // stackTop
                a.mov(x86::qword_ptr(x86::r12, 160), x86::r13); // currentBp（R156）

                // 调用 jitCreateClosure(ctx, funName, chunkPtr, upvalueCount, upvalueDescs)
                // → rax = 闭包值 bits
                a.movabs(x86::r10, reinterpret_cast<uint64_t>(funNamePtr));
                a.movabs(x86::r11, reinterpret_cast<uint64_t>(chunkPtr));
#ifdef _WIN32
                // Win32: rcx=ctx, rdx=funName, r8=chunkPtr, r9=upvalueCount, [rsp+32]=upvalueDescs
                a.mov(x86::rcx, x86::r12); // arg1 = ctx
                a.mov(x86::rdx, x86::r10); // arg2 = funName
                a.mov(x86::r8, x86::r11);  // arg3 = chunkPtr
                a.xor_(x86::r9, x86::r9);
                a.mov(x86::r9b, upvalueCount); // arg4 = upvalueCount
                a.sub(x86::rsp, 40);           // shadow space (32) + 8B arg5（R151 教训）
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(upvalueDescs));
                a.mov(x86::qword_ptr(x86::rsp, 32), x86::rax); // arg5 = upvalueDescs
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitCreateClosure));
                a.call(x86::rax);
                a.add(x86::rsp, 40);
#else
                // Linux: rdi=ctx, rsi=funName, rdx=chunkPtr, rcx=upvalueCount, r8=upvalueDescs
                a.mov(x86::rdi, x86::r12);
                a.mov(x86::rsi, x86::r10);
                a.mov(x86::rdx, x86::r11);
                a.xor_(x86::rcx, x86::rcx);
                a.mov(x86::cl, upvalueCount);
                a.movabs(x86::r8, reinterpret_cast<uint64_t>(upvalueDescs));
                a.sub(x86::rsp, 8); // 16-byte alignment
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitCreateClosure));
                a.call(x86::rax);
                a.add(x86::rsp, 8);
#endif
                // push 闭包值 bits（rax）
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                // 检查错误
                a.mov(x86::rax, x86::qword_ptr(x86::r12, 8));
                a.movzx(x86::rax, x86::byte_ptr(x86::rax));
                a.test(x86::rax, x86::rax);
                a.jnz(epilogue);
                // OP_CLOSURE(1) + nameIdx(2) + upvalueCount(1) + 2*upvalueCount = 4 + 2*upvalueCount
                ip += 4 + 2 * upvalueCount;
                break;
            }

            // ---- R156: OP_CLOSE_UPVALUE（2B: op + slotBase(1B)） ----
            // StackVM 语义：关闭所有指向 slot >= basePointer+slotBase 的 open upvalues
            // JIT 语义：关闭所有 address <= (r13 - slotBase*8) 的 open upvalues（栈向下增长方向反转）
            case OpCode::OP_CLOSE_UPVALUE: {
                if (ip + 1 >= bytecodes.size()) {
                    compileError("OP_CLOSE_UPVALUE 操作数越界");
                    return nullptr;
                }
                uint8_t slotBase = bytecodes[ip + 1];
                // 同步栈顶（jitCloseUpvalues 不操作栈，保持邮箱一致性）
                a.mov(x86::qword_ptr(x86::r12, 48), x86::r15);
                // 计算 fromAddr = r13 - slotBase*8
                // lea rdx, [r13 - slotBase*8]
                a.lea(x86::rdx, x86::qword_ptr(x86::r13, -static_cast<int32_t>(slotBase) * 8));
#ifdef _WIN32
                // Win32: rcx=ctx, rdx=fromAddr
                a.mov(x86::rcx, x86::r12);
                // rdx 已是 fromAddr
                a.sub(x86::rsp, 40); // shadow space + alignment
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitCloseUpvalues));
                a.call(x86::rax);
                a.add(x86::rsp, 40);
#else
                // Linux: rdi=ctx, rsi=fromAddr
                a.mov(x86::rdi, x86::r12);
                a.mov(x86::rsi, x86::rdx);
                a.sub(x86::rsp, 8);
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitCloseUpvalues));
                a.call(x86::rax);
                a.add(x86::rsp, 8);
#endif
                ip += 2;
                break;
            }

            // ---- R156: OP_GET_UPVALUE（2B: op + uvIdx(1B)） ----
            // StackVM 语义：push frame.upvalues[uvIdx] 的值（closed 读 value，open 读栈槽）
            case OpCode::OP_GET_UPVALUE: {
                if (ip + 1 >= bytecodes.size()) {
                    compileError("OP_GET_UPVALUE 操作数越界");
                    return nullptr;
                }
                uint8_t uvIdx = bytecodes[ip + 1];
                // 同步栈顶
                a.mov(x86::qword_ptr(x86::r12, 48), x86::r15);
#ifdef _WIN32
                a.mov(x86::rcx, x86::r12); // arg1 = ctx
                a.xor_(x86::rdx, x86::rdx);
                a.mov(x86::dl, uvIdx); // arg2 = uvIdx
                a.sub(x86::rsp, 40);
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitGetUpvalue));
                a.call(x86::rax);
                a.add(x86::rsp, 40);
#else
                a.mov(x86::rdi, x86::r12);
                a.xor_(x86::rsi, x86::rsi);
                a.mov(x86::sil, uvIdx);
                a.sub(x86::rsp, 8);
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitGetUpvalue));
                a.call(x86::rax);
                a.add(x86::rsp, 8);
#endif
                // rax = upvalue value bits，push 到 JIT 栈
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                // 检查错误
                a.mov(x86::rax, x86::qword_ptr(x86::r12, 8));
                a.movzx(x86::rax, x86::byte_ptr(x86::rax));
                a.test(x86::rax, x86::rax);
                a.jnz(epilogue);
                ip += 2;
                break;
            }

            // ---- R156: OP_SET_UPVALUE（2B: op + uvIdx(1B)） ----
            // StackVM 语义：peek(0) 写入 upvalue（不消费栈顶，与 OP_SET_LOCAL 一致）
            case OpCode::OP_SET_UPVALUE: {
                if (ip + 1 >= bytecodes.size()) {
                    compileError("OP_SET_UPVALUE 操作数越界");
                    return nullptr;
                }
                uint8_t uvIdx = bytecodes[ip + 1];
                // 同步栈顶
                a.mov(x86::qword_ptr(x86::r12, 48), x86::r15);
                // 读取栈顶值（peek，不 pop）
                a.mov(x86::rax, x86::qword_ptr(x86::r15));
#ifdef _WIN32
                // Win32: rcx=ctx, rdx=uvIdx, r8=valueBits
                a.mov(x86::rcx, x86::r12); // arg1 = ctx
                a.xor_(x86::rdx, x86::rdx);
                a.mov(x86::dl, uvIdx);    // arg2 = uvIdx
                a.mov(x86::r8, x86::rax); // arg3 = valueBits（peek 值）
                a.sub(x86::rsp, 40);
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitSetUpvalue));
                a.call(x86::rax);
                a.add(x86::rsp, 40);
#else
                // Linux: rdi=ctx, rsi=uvIdx, rdx=valueBits
                a.mov(x86::rdi, x86::r12);
                a.xor_(x86::rsi, x86::rsi);
                a.mov(x86::sil, uvIdx);
                a.mov(x86::rdx, x86::rax); // arg3 = valueBits
                a.sub(x86::rsp, 8);
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitSetUpvalue));
                a.call(x86::rax);
                a.add(x86::rsp, 8);
#endif
                // 检查错误
                a.mov(x86::rax, x86::qword_ptr(x86::r12, 8));
                a.movzx(x86::rax, x86::byte_ptr(x86::rax));
                a.test(x86::rax, x86::rax);
                a.jnz(epilogue);
                ip += 2;
                break;
            }

            // ---- R155: 闭包值调用指令（OP_CALL_EXPR，2B: op + argCount(1B)） ----
            // 栈布局（调用前）：[..., closure, arg0, arg1, ..., argN-1]（closure 在参数下方）
            // 栈布局（调用后）：新帧栈 [..., arg0, arg1, ..., argN-1, defaults..., extraSlots...]
            // 与 StackVM executeCallExprValue 对齐（VMCalls.cpp:647-709）
            // R155 简化版：仅支持 upvalueCount=0 的闭包值（无 upvalue 绑定），upvalue 捕获推迟到 R156
            // 实现模式：调用 jitCallExpr C++ 辅助函数（通过邮箱返回入口地址 + localCount），
            //          然后设置 returnAddr + r13 + jmp 入口（参考 emitMethodCall 模式）
            case OpCode::OP_CALL_EXPR: {
                if (ip + 1 >= bytecodes.size()) {
                    compileError("OP_CALL_EXPR 操作数越界");
                    return nullptr;
                }
                uint8_t argCount = bytecodes[ip + 1];

                Label returnLabel = a.new_label();

                // 1. 存储 callerBp (r13) 到 ctx->callerBp (offset 96)
                a.mov(x86::qword_ptr(x86::r12, 96), x86::r13);

                // 2. 同步栈顶到 ctx->stackTop (offset 48)
                a.mov(x86::qword_ptr(x86::r12, 48), x86::r15);

                // 3. 调用 jitCallExpr(ctx, argCount)
                //    辅助函数通过 ctx->stackTop 操作栈（pop args + pop closure + push args/defaults/extraSlots）
                //    通过邮箱返回：ctx->methodEntryPtr = 入口地址, ctx->methodLocalCount = localCount
#ifdef _WIN32
                a.mov(x86::rcx, x86::r12); // arg1 = ctx
                a.mov(x86::dl, argCount);  // arg2 = argCount (uint8_t)
                a.sub(x86::rsp, 40);       // shadow space (32) + 8B 对齐填充（R151 教训）
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitCallExpr));
                a.call(x86::rax);
                a.add(x86::rsp, 40);
#else
                a.mov(x86::rdi, x86::r12);
                a.mov(x86::sil, argCount);
                a.sub(x86::rsp, 8); // 16-byte alignment
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitCallExpr));
                a.call(x86::rax);
                a.add(x86::rsp, 8);
#endif

                // 4. 恢复 r15（jitCallExpr 修改了栈）
                a.mov(x86::r15, x86::qword_ptr(x86::r12, 48));

                // 5. 检查错误
                a.mov(x86::rax, x86::qword_ptr(x86::r12, 8));
                a.movzx(x86::rax, x86::byte_ptr(x86::rax));
                a.test(x86::rax, x86::rax);
                a.jnz(epilogue);

                // 6. 在新 frame 上设置 returnAddr（jitCallExpr 无法访问 JIT Label，需在调用后写入）
                //    新 frame 在 frames[*frameCount - 1]
                a.mov(x86::rcx, x86::qword_ptr(x86::r12, 40)); // rcx = &frameCount_
                a.mov(x86::rdx, x86::qword_ptr(x86::rcx));     // rdx = *frameCount
                a.sub(x86::rdx, 1);                            // rdx = idx（新 frame 索引）
                a.mov(x86::rcx, x86::qword_ptr(x86::r12, 32)); // rcx = frames base
                a.imul(x86::rdx, x86::rdx, 72);
                a.add(x86::rcx, x86::rdx); // rcx = &frames[idx]
                a.lea(x86::rax, x86::qword_ptr(returnLabel));
                a.mov(x86::qword_ptr(x86::rcx, 16), x86::rax); // frame->returnAddr = returnLabel

                // 7. 设置 r13 = r15 + (methodLocalCount - 1) * 8
                //    普通函数帧无 this/fields，slot 0 是 arg0（最高地址）
                a.mov(x86::rax, x86::qword_ptr(x86::r12, 80)); // rax = methodLocalCount
                a.dec(x86::rax);
                a.imul(x86::rax, x86::rax, 8);
                a.lea(x86::r13, x86::qword_ptr(x86::r15, x86::rax));

                // 8. jmp methodEntryPtr
                a.mov(x86::rax, x86::qword_ptr(x86::r12, 72)); // rax = methodEntryPtr
                a.jmp(x86::rax);

                // 9. 绑定返回 Label（函数 OP_RETURN 后跳回此处）
                a.bind(returnLabel);

                ip += 2;
                break;
            }

            // ---- R141: 函数返回指令（OP_RETURN，1B） ----
            // StackVM 语义：pop 返回值; frames_.pop_back(); stack_.resize(savedBp); push 返回值
            // JIT 实现：
            //   - 末帧返回（*frameCount == 0）→ jmp epilogue（不 push 返回值）
            //   - 普通返回 → 检查 isMethodCall → 调用 jitMethodReturn 处理 writeBack + 返回值替换
            //              → 恢复 r13/r15 + push 返回值 + jmp returnAddr
            case OpCode::OP_RETURN: {
                // pop 返回值到 rax
                a.mov(x86::rax, x86::qword_ptr(x86::r15));
                a.add(x86::r15, 8);

                // 检查 *frameCount == 0 → epilogue（mainChunk 末帧返回）
                a.mov(x86::rcx, x86::qword_ptr(x86::r12, 40)); // rcx = &frameCount_
                a.mov(x86::rdx, x86::qword_ptr(x86::rcx));     // rdx = *frameCount
                a.test(x86::rdx, x86::rdx);
                a.jz(epilogue);

                // R161 perf: 保存缓存的 rbx(JIT_INT48_MASK) 到 [rbp-48] 暂存槽。
                // [rbp-48] 是操作数栈基址边界（非栈元素），OP_RETURN 期间安全覆写。
                // r14(JIT_INT_TAG_BASE) 不被本路径覆写（returnAddr 改用 rdx），无需保存。
                a.mov(x86::qword_ptr(x86::rbp, -48), x86::rbx);

                // R156: close upvalue（在 *frameCount -= 1 之前，r13 仍是当前帧 basePtr）
                // P0 UAF 修复：关闭当前帧的 open upvalues，防止栈槽被回收后悬垂引用
                // 同时 pop frameUpvaluesStack_ 中属于当前帧的 upvalues
                a.mov(x86::rbx, x86::rax);                     // 保存返回值到 rbx（callee-saved）
                a.mov(x86::qword_ptr(x86::r12, 48), x86::r15); // 同步栈顶
#ifdef _WIN32
                // Win32: rcx=ctx, rdx=fromAddr(r13), r8=newFrameCount(*frameCount-1)
                a.mov(x86::rcx, x86::r12);
                a.mov(x86::rdx, x86::r13);
                a.mov(x86::rax, x86::qword_ptr(x86::r12, 40)); // rax = &frameCount_
                a.mov(x86::r8, x86::qword_ptr(x86::rax));      // r8 = *frameCount
                a.dec(x86::r8);                                // r8 = *frameCount - 1
#else
                // Linux: rdi=ctx, rsi=fromAddr(r13), rdx=newFrameCount
                a.mov(x86::rdi, x86::r12);
                a.mov(x86::rsi, x86::r13);
                a.mov(x86::rax, x86::qword_ptr(x86::r12, 40)); // rax = &frameCount_
                a.mov(x86::rdx, x86::qword_ptr(x86::rax));     // rdx = *frameCount
                a.dec(x86::rdx);
#endif
                a.sub(x86::rsp, 32);
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitReturnCloseUpvalues));
                a.call(x86::rax);
                a.add(x86::rsp, 32);
                a.mov(x86::r15, x86::qword_ptr(x86::r12, 48)); // 恢复栈顶
                a.mov(x86::rax, x86::rbx);                     // 恢复返回值

                // *frameCount -= 1，rdx = 新的 frameCount（重新加载，close upvalue 破坏了 rcx/rdx）
                a.mov(x86::rcx, x86::qword_ptr(x86::r12, 40)); // rcx = &frameCount_
                a.mov(x86::rdx, x86::qword_ptr(x86::rcx));     // rdx = *frameCount
                a.sub(x86::rdx, 1);
                a.mov(x86::qword_ptr(x86::rcx), x86::rdx);

                // 读取 frames[rdx] 的字段
                a.mov(x86::rcx, x86::qword_ptr(x86::r12, 32)); // rcx = frames base
                a.imul(x86::rdx, x86::rdx, 72);                // R149: 72 字节偏移
                a.add(x86::rcx, x86::rdx);                     // rcx = &frames[返回帧]

                // R149: 检查 isMethodCall，如果是方法调用则调用 jitMethodReturn 处理 writeBack
                //   jitMethodReturn(ctx, framePtr, retvalBits) → 返回最终的返回值 bits
                //   辅助函数内部处理：字段槽同步回 this + this 同步回 receiverSlotPtr + init 返回 this
                Label notMethodCall = a.new_label();
                a.mov(x86::rdx, x86::qword_ptr(x86::rcx, 48)); // rdx = isMethodCall
                a.test(x86::rdx, x86::rdx);
                a.jz(notMethodCall);

                // 方法调用路径：调用 jitMethodReturn(ctx, framePtr, retvalBits)
                // 保存返回值 rax 到 rbx（callee-saved，跨调用保留）
                a.mov(x86::rbx, x86::rax);
                // 同步栈顶到 ctx->stackTop（虽然 jitMethodReturn 不操作栈，保持模式一致）
                a.mov(x86::qword_ptr(x86::r12, 48), x86::r15);
#ifdef _WIN32
                // Win32: rcx=ctx, rdx=framePtr, r8=retvalBits
                a.mov(x86::rcx, x86::r12);
                a.mov(x86::rdx, x86::rcx); // rdx = framePtr（rcx 已被覆盖，需先保存）
                // 重新计算 framePtr（rcx 已被覆盖）
                a.mov(x86::rcx, x86::qword_ptr(x86::r12, 40)); // rcx = &frameCount_
                a.mov(x86::rdx, x86::qword_ptr(x86::rcx));     // rdx = *frameCount（已被减 1）
                a.mov(x86::rcx, x86::qword_ptr(x86::r12, 32)); // rcx = frames base
                a.imul(x86::rdx, x86::rdx, 72);
                a.add(x86::rcx, x86::rdx); // rcx = framePtr
                a.mov(x86::rdx, x86::rcx); // rdx = framePtr
                a.mov(x86::rcx, x86::r12); // rcx = ctx
                a.mov(x86::r8, x86::rbx);  // r8 = retvalBits
#else
                // Linux: rdi=ctx, rsi=framePtr, rdx=retvalBits
                a.mov(x86::rdi, x86::r12);
                a.mov(x86::rsi, x86::rcx); // rsi = framePtr
                a.mov(x86::rdx, x86::rbx); // rdx = retvalBits
#endif
                a.sub(x86::rsp, 32); // shadow space
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitMethodReturn));
                a.call(x86::rax);
                a.add(x86::rsp, 32);
                // rax = 最终返回值 bits
                a.mov(x86::rbx, x86::rax); // 保存到 rbx
                // 检查错误（R149 fix: 用 movzx byte_ptr 读取 bool，原 mov qword_ptr 读取 8 字节
                //   会读到相邻 errorBuffer 指针的低 7 字节，导致误判 hasError=true）
                a.mov(x86::rax, x86::qword_ptr(x86::r12, 8)); // hasError ptr
                a.movzx(x86::rax, x86::byte_ptr(x86::rax));   // *hasError (1 字节零扩展)
                a.test(x86::rax, x86::rax);
                a.jnz(epilogue);
                // 恢复 r15（辅助函数可能未修改，但保持模式一致）
                a.mov(x86::r15, x86::qword_ptr(x86::r12, 48));
                // 最终返回值在 rbx
                a.mov(x86::rax, x86::rbx);

                a.bind(notMethodCall);

                // r14 = frames[idx].returnAddr（先读取，避免被后续覆盖）
                // rcx 仍指向 &frames[返回帧]（notMethodCall 路径）或已被覆盖（方法调用路径需重新计算）
                // 为简化，重新计算 framePtr
                a.mov(x86::rcx, x86::qword_ptr(x86::r12, 40)); // rcx = &frameCount_
                a.mov(x86::rdx, x86::qword_ptr(x86::rcx));     // rdx = *frameCount（已减 1）
                a.mov(x86::rcx, x86::qword_ptr(x86::r12, 32)); // rcx = frames base
                a.imul(x86::rdx, x86::rdx, 72);
                a.add(x86::rcx, x86::rdx); // rcx = &frames[返回帧]
                // R161 perf: returnAddr 改用 rdx（rdx 在 imul+add 后已死，可安全复用），
                // 释放 r14 用于缓存 JIT_INT_TAG_BASE 常量
                a.mov(x86::rdx, x86::qword_ptr(x86::rcx, 16)); // rdx = returnAddr
                // 恢复 r13 = frames[idx].callerBp
                a.mov(x86::r13, x86::qword_ptr(x86::rcx));
                // 恢复 r15 = frames[idx].callerSp（丢弃当前帧的参数+局部变量）
                a.mov(x86::r15, x86::qword_ptr(x86::rcx, 8));

                // push 返回值到调用者栈
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);

                // R161 perf: 恢复缓存的 rbx(JIT_INT48_MASK)，调用者算术运算依赖此值
                a.mov(x86::rbx, x86::qword_ptr(x86::rbp, -48));

                // jmp returnAddr
                a.jmp(x86::rdx);

                ip += 1;
                break;
            }

                // ---- R154 阶段 5d：嵌套左值赋值链 + OP_LEN + OP_DUP_N ----
                // 设计：补全 StackVM 容器/栈操作子集，使 JIT 支持嵌套索引赋值（arr[i][j]=v）
                // 及 len()/dup_n 操作。OP_CALL_EXPR/upvalue 推迟到 R155（需完整闭包基础设施）。

            case OpCode::OP_LEN: {
                // 1B 操作数：opcode
                // 栈布局：[..., v] → [..., len]（pop 1 + push 1）
                emitLen();
                ip += 1;
                break;
            }

            case OpCode::OP_DUP_N: {
                // 2B 操作数：opcode + depth(1B)
                // 栈布局：[..., v_depth, ..., v_0] → [..., v_depth, ..., v_0, v_depth_copy]
                if (ip + 1 >= bytecodes.size()) {
                    compileError("OP_DUP_N 操作数越界");
                    return nullptr;
                }
                uint8_t depth = bytecodes[ip + 1];
                emitDupN(depth);
                ip += 2;
                break;
            }

            case OpCode::OP_LOAD_MUTATED: {
                // 1B 操作数：opcode
                // 栈布局：[...] → [..., lastMutatedReceiver_]
                emitLoadMutated();
                ip += 1;
                break;
            }

            case OpCode::OP_INDEX_SET: {
                // 1B 操作数：opcode
                // 栈布局：[..., obj, innerIdx, val] → [...]（pop 3，变异后容器存入 lastMutatedReceiver_）
                emitIndexSet();
                ip += 1;
                break;
            }

            case OpCode::OP_WRITEBACK_MEMBER_VAR: {
                // 5B 操作数：opcode + varIdx(2B) + fieldIdx(2B)
                // fieldIdx 仅用于反汇编/调试，运行时不读取（整体替换语义）
                if (ip + 4 >= bytecodes.size()) {
                    compileError("OP_WRITEBACK_MEMBER_VAR 操作数越界");
                    return nullptr;
                }
                uint16_t varIdx =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                uint16_t fieldIdx =
                    static_cast<uint16_t>(bytecodes[ip + 3]) | (static_cast<uint16_t>(bytecodes[ip + 4]) << 8);
                if (varIdx >= chunk.constants.size() || fieldIdx >= chunk.constants.size()) {
                    compileError("OP_WRITEBACK_MEMBER_VAR 常量池索引越界");
                    return nullptr;
                }
                // fieldIdx 仅用于反汇编/调试，运行时不需要（整体替换语义）
                (void)chunk.constants[fieldIdx];
                {
                    const std::string& varName = chunk.constants[varIdx].stringVal();
                    auto it = globalNameToSlot_.find(varName);
                    if (it == globalNameToSlot_.end()) {
                        compileError("OP_WRITEBACK_MEMBER_VAR 未定义的全局变量: " + varName);
                        return nullptr;
                    }
                    emitWritebackVar(it->second);
                }
                ip += 5;
                break;
            }

            case OpCode::OP_WRITEBACK_INDEX_VAR: {
                // 3B 操作数：opcode + varIdx(2B)
                if (ip + 2 >= bytecodes.size()) {
                    compileError("OP_WRITEBACK_INDEX_VAR 操作数越界");
                    return nullptr;
                }
                uint16_t varIdx =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                if (varIdx >= chunk.constants.size()) {
                    compileError("OP_WRITEBACK_INDEX_VAR 常量池索引越界");
                    return nullptr;
                }
                {
                    const std::string& varName = chunk.constants[varIdx].stringVal();
                    auto it = globalNameToSlot_.find(varName);
                    if (it == globalNameToSlot_.end()) {
                        compileError("OP_WRITEBACK_INDEX_VAR 未定义的全局变量: " + varName);
                        return nullptr;
                    }
                    emitWritebackVar(it->second);
                }
                ip += 3;
                break;
            }

            case OpCode::OP_WRITEBACK_MEMBER_LOCAL: {
                // 4B 操作数：opcode + slot(1B) + fieldIdx(2B)
                // fieldIdx 仅用于反汇编/调试，运行时不读取（整体替换语义）
                if (ip + 3 >= bytecodes.size()) {
                    compileError("OP_WRITEBACK_MEMBER_LOCAL 操作数越界");
                    return nullptr;
                }
                uint8_t slot = bytecodes[ip + 1];
                uint16_t fieldIdx =
                    static_cast<uint16_t>(bytecodes[ip + 2]) | (static_cast<uint16_t>(bytecodes[ip + 3]) << 8);
                if (fieldIdx >= chunk.constants.size()) {
                    compileError("OP_WRITEBACK_MEMBER_LOCAL 常量池索引越界");
                    return nullptr;
                }
                // fieldIdx 仅用于反汇编/调试，运行时不需要（整体替换语义）
                (void)chunk.constants[fieldIdx];
                emitWritebackLocal(slot);
                ip += 4;
                break;
            }

            case OpCode::OP_WRITEBACK_INDEX_LOCAL: {
                // 2B 操作数：opcode + slot(1B)
                if (ip + 1 >= bytecodes.size()) {
                    compileError("OP_WRITEBACK_INDEX_LOCAL 操作数越界");
                    return nullptr;
                }
                uint8_t slot = bytecodes[ip + 1];
                emitWritebackLocal(slot);
                ip += 2;
                break;
            }

            default:
                // 阶段 2b 仍不支持的指令（OP_CALL_EXPR 闭包值调用、upvalue、类、容器等）
                compileError("JIT 阶段 2b 不支持的 OpCode: " + std::string(opCodeName(op)) +
                             " (ip=" + std::to_string(ip) + ")");
                return nullptr;
            }
        }
    }

    // ---- 函数 epilogue ----
    // 所有末帧 OP_RETURN 跳转到此处
    a.bind(epilogue);

    // 设置返回值为 0（PoC 不使用返回值）
    a.xor_(x86::rax, x86::rax);

    // 恢复 callee-saved 寄存器
    a.mov(x86::r12, x86::qword_ptr(x86::rbp, -8));
    a.mov(x86::r13, x86::qword_ptr(x86::rbp, -16));
    a.mov(x86::r14, x86::qword_ptr(x86::rbp, -24));
    a.mov(x86::r15, x86::qword_ptr(x86::rbp, -32));
    a.mov(x86::rbx, x86::qword_ptr(x86::rbp, -40));

    // 释放栈帧并返回
    a.mov(x86::rsp, x86::rbp);
    a.pop(x86::rbp);
    a.ret();

    // ---- 编译并加入 runtime ----
    JitEntryFn entry = nullptr;
    Error addErr = runtime_.add(&entry, &code);
    if (addErr != kErrorOk) {
        compileError(std::string("asmjit JitRuntime::add 失败: ") + DebugUtils::error_as_string(addErr));
        return nullptr;
    }

    // R149: 填充 methodEntries_（"Class.method" → JitMethodInfo）
    // runtime_.add 内部调用 code.flatten()，之后 label_offset_from_base 返回正确偏移
    // 方法入口运行时地址 = (char*)entry + code.label_offset_from_base(entryLabel)
    methodEntries_.clear();
    for (const auto& mli : methodLabels) {
        JitMethodInfo info;
        uint64_t offset = code.label_offset_from_base(mli.entryLabel);
        info.entryPtr = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(entry) + offset);
        info.localCount = mli.localCount;
        info.arity = mli.arity;
        info.requiredArity = mli.requiredArity;
        info.fieldOrder = &mli.chunk->fieldOrder;
        info.defaultConstIndices = &mli.chunk->defaultConstIndices;
        info.constants = &mli.chunk->constants;
        methodEntries_.emplace(mli.fullName, std::move(info));
    }

    // R155: 填充 funcEntries_（普通函数名 → JitMethodInfo，OP_CALL_EXPR 闭包值调用用）
    // 仅包含不含 '.' 的函数名（方法 chunk 由 methodEntries_ 处理）
    // 键是普通函数名，jitCallExpr 通过 callee.closureName() 查找
    funcEntries_.clear();
    for (const auto& [name, finfo] : funcTable) {
        if (name.find('.') != std::string::npos) {
            continue; // 跳过方法 chunk
        }
        if (!finfo.chunk) {
            continue; // 跳过无 chunk 指针的条目
        }
        JitMethodInfo jmi;
        uint64_t offset = code.label_offset_from_base(finfo.entryLabel);
        jmi.entryPtr = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(entry) + offset);
        jmi.localCount = finfo.localCount;
        jmi.arity = finfo.arity;
        jmi.requiredArity = finfo.requiredArity;
        jmi.fieldOrder = nullptr; // 普通函数无 fieldOrder
        jmi.defaultConstIndices = &finfo.chunk->defaultConstIndices;
        jmi.constants = &finfo.chunk->constants;
        funcEntries_.emplace(name, std::move(jmi));
    }

    // R157: lazy compilation 模式 — 将函数/方法 chunk 的 entryPtr 置空，
    // 使 jitCallByName/jitCallExpr 在首次调用时发现 entryPtr 为 null，
    // 通过 backendPtr 回调 triggerLazyCompile 按需编译该 chunk。
    // mainChunk（索引 0）不参与 lazy 编译，始终保留正常入口。
    // 注意：lazy 编译触发后 entryPtr 会被 compileSingleChunkLazy 更新为真实入口，
    // 后续同 chunk 调用直接走已更新映射，无再次 lazy 触发。
    if (lazyMode_) {
        for (auto& [name, info] : funcEntries_) {
            info.entryPtr = nullptr;
        }
        for (auto& [name, info] : methodEntries_) {
            info.entryPtr = nullptr;
        }
    }

    // R158: OSR 入口点地址计算（仅 osrEntryGenMode_ 且 Label 已生成时）
    // runtime_.add 后 code.flatten() 已完成，label_offset_from_base 返回正确偏移。
    // OSR 入口点运行时地址 = (char*)entry + code.label_offset_from_base(osrEntryLabel_)
    if (osrEntryGenMode_ && osrEntryLabelGenerated_ && osrEntryPointOut_ != nullptr) {
        uint64_t osrOffset = code.label_offset_from_base(osrEntryLabel_);
        *osrEntryPointOut_ = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(entry) + osrOffset);
    }

    // R158: 初始化 per-chunk Tier 跟踪（分层编译状态）
    // 首次编译时所有 chunk 为 Tier 1 (Baseline)，OSR 迁移后升级为 Tier 2 (Specialized)。
    // 反优化时回退为 Tier 1。
    // R159: 分层编译模式下，非 main chunk 初始为 Tier 0 (Interpreter)，
    // 首次调用时通过 lazy compilation 自动升级到 Tier 1 (Baseline)。
    if (chunkTiers_.size() != allChunks.size()) {
        if (tieredMode_) {
            chunkTiers_.assign(allChunks.size(), minilang::JitTier::Interpreter);
            chunkTiers_[0] = minilang::JitTier::Baseline; // mainChunk 始终为 Baseline
        } else {
            chunkTiers_.assign(allChunks.size(), minilang::JitTier::Baseline);
        }
    }

    // R158: 备份 Tier 1 baseline 入口（反优化用）
    // 在特化重编译前备份 baseline 入口，反优化时从备份恢复。
    // 仅在非特化模式（specializeIntMode_==false && specializeFloatMode_==false）时备份，
    // 避免特化版本入口覆盖 baseline 备份。
    if (!specializeIntMode_ && !specializeFloatMode_) {
        for (const auto& [name, info] : methodEntries_) {
            baselineMethodEntries_[name] = info.entryPtr;
        }
        for (const auto& [name, info] : funcEntries_) {
            baselineFuncEntries_[name] = info.entryPtr;
        }
    }

    return entry;
}

// ============================================================
// R150: getHotChunkStats — 返回 per-chunk 调用计数统计（热点检测用）
// ============================================================
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
    ownedSpecializedEntries_.push_back(specializedEntry);

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
    ownedSpecializedEntries_.push_back(specializedEntry);

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
    ownedSpecializedEntries_.push_back(specializedEntry);

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
    ownedSpecializedEntries_.push_back(specializedEntry);
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
JitResult JITBackend::execute(const CompileResult& result) {
    // 重置状态
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
        e.cachedInstancePtr = nullptr;
        e.cachedFieldValuePtr = nullptr;
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
    int64_t ret = entry(&jitContext_);
    (void)ret; // PoC 不使用返回值

    if (hasError_) {
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
namespace {
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
