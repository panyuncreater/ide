/**
 * @file compiler/JITRuntime.cpp
 * @brief JIT runtime helper functions (extracted from JIT.cpp).
 * Contains extern "C" functions called by JIT-generated native code.
 * @see JIT.h JITInternal.h
 * @since P3 (JIT.cpp split)
 */

#include "compiler/JIT.h"

#ifdef MINILANG_USE_JIT

#include "compiler/JITInternal.h"
#include "common/Diagnostic.h"
#include "common/ErrorFormat.h"
#include "common/ErrorMessages.h"
#include "common/RuntimeLimits.h"
#include "common/Utf8Utils.h"
#include "interpreter/Value.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

using namespace jit_internal;

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
extern "C" uint64_t jitAddGeneric(JitContext* ctx, uint64_t leftBits, uint64_t rightBits) {
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
            // INT64 溢出：通过 ctx 报告错误（与 jitDivGeneric 除零路径一致），
            // 由 emitCallBinaryHelper 的 checkError 路径转为可捕获异常，不再静默返回 0
            if (ctx && ctx->hasError && ctx->errorBuffer) {
                *ctx->hasError = true;
                *ctx->errorBuffer = "整数运算溢出";
            }
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
extern "C" uint64_t jitSubGeneric(JitContext* ctx, uint64_t leftBits, uint64_t rightBits) {
    Value left = bitsToValue(leftBits);
    Value right = bitsToValue(rightBits);
    // R145 修复：整数减法（INT + INT，INT48 溢出时从 JIT 原生路径降级到此）
    if (left.isInt() && right.isInt()) {
        int64_t l = left.intVal();
        int64_t r = right.intVal();
        if (OverflowCheck::subOverflow(l, r)) {
            if (ctx && ctx->hasError && ctx->errorBuffer) {
                *ctx->hasError = true;
                *ctx->errorBuffer = "整数运算溢出";
            }
            return jitReturn(Value(0));
        }
        return jitReturn(Value(l - r));
    }
    return jitReturn(Value(left.toDouble() - right.toDouble()));
}

/// 通用乘法（FLOAT/FLOAT，含 INT+INT 降级路径）
extern "C" uint64_t jitMulGeneric(JitContext* ctx, uint64_t leftBits, uint64_t rightBits) {
    Value left = bitsToValue(leftBits);
    Value right = bitsToValue(rightBits);
    // R145 修复：整数乘法（INT + INT，INT48 溢出时从 JIT 原生路径降级到此）
    if (left.isInt() && right.isInt()) {
        int64_t l = left.intVal();
        int64_t r = right.intVal();
        if (OverflowCheck::mulOverflow(l, r)) {
            if (ctx && ctx->hasError && ctx->errorBuffer) {
                *ctx->hasError = true;
                *ctx->errorBuffer = "整数运算溢出";
            }
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
    std::array<Value, RuntimeLimits::MAX_JIT_ARGS> args;
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
    std::array<Value, RuntimeLimits::MAX_DEFAULT_PARAMS> defaults;
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
            if (constIdx == RuntimeLimits::NO_INDEX || constIdx >= foundFunc->constants->size()) {
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
            *ctx->errorBuffer = ErrorFormat::formatStd(ErrorMessages::kRecursionDepthExceededFmtStd,
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
    std::array<Value, RuntimeLimits::MAX_JIT_ARGS> args;
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

    std::array<Value, RuntimeLimits::MAX_DEFAULT_PARAMS> defaults;
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
            if (constIdx == RuntimeLimits::NO_INDEX || constIdx >= initMethod->constants->size()) {
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
            *ctx->errorBuffer = ErrorFormat::formatStd(ErrorMessages::kRecursionDepthExceededFmtStd,
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
///        - bit 8-15: receiverLocalSlotByte（NO_SLOT=无局部变量接收者）
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
    // W4 fix: receiverLocalSlotByte（bits 8-15）由 emitMethodCall 打包但 jitMethodCall
    // 通过 receiverSlotPtr 参数完成写回，无需在此解包。
    bool isInitCall = (packedArgs & (1LL << 16)) != 0;
    bool isSuperCall = (packedArgs & (1LL << 17)) != 0;

    int64_t* sp = ctx->stackTop;

    // 1. pop argCount 个参数（反向填充：栈顶是 argN-1）
    //    JIT 栈向下增长，sp += 1 是 pop（向高地址移动）
    //    argCount 为 uint8_t（上限 255），MAX_JIT_ARGS=256 覆盖全部合法值
    std::array<Value, RuntimeLimits::MAX_JIT_ARGS> args;
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

    std::array<Value, RuntimeLimits::MAX_DEFAULT_PARAMS> defaults;
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
            if (constIdx == RuntimeLimits::NO_INDEX || constIdx >= foundMethod->constants->size()) {
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
            *ctx->errorBuffer = ErrorFormat::formatStd(ErrorMessages::kRecursionDepthExceededFmtStd,
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
extern "C" void jitReportStackOverflow(JitContext* ctx) {
    if (!ctx || !ctx->hasError || !ctx->errorBuffer) {
        return;
    }
    *ctx->hasError = true;
    // P0 fix: use ErrorFormat::format (snprintf-based, ~200B stack) instead of
    // ErrorFormat::formatStd (std::format-based, ~8KB stack in MSVC Debug).
    // The JIT entry() function allocates 8KB operand stack on the native stack
    // (sub rsp, 8240) without __chkstk. When jitReportStackOverflow is called
    // from the overflow path (below the 8KB allocation), std::format's large
    // stack frame exceeds the committed stack region → access violation.
    // Error paths are rare; snprintf's performance is irrelevant here.
    *ctx->errorBuffer =
        ErrorFormat::format(ErrorMessages::kRecursionDepthExceededFmt, static_cast<int>(RuntimeLimits::MAX_FRAMES));
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
    std::array<Value, RuntimeLimits::MAX_JIT_ARGS> args;
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
    std::array<Value, RuntimeLimits::MAX_DEFAULT_PARAMS> defaults;
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
            if (constIdx == RuntimeLimits::NO_INDEX || constIdx >= foundFunc->constants->size()) {
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
            *ctx->errorBuffer = ErrorFormat::formatStd(ErrorMessages::kRecursionDepthExceededFmtStd,
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
// R162: 异常处理辅助函数（try/catch/throw + finally 续跳）
// ============================================================

/// R162: push try handler 到 tryStack_
/// JIT 代码在 OP_TRY_BEGIN 处调用，传入 catch 块地址（lea label）和当前栈顶。
/// frameIndex 从 ctx->frameCount 读取，用于跨帧异常传播时判断 handler 归属。
extern "C" void jitPushTryHandler(JitContext* ctx, void* catchAddr, int64_t* stackBase) {
    if (!ctx || !ctx->backendPtr)
        return;
    auto* backend = static_cast<JITBackend*>(ctx->backendPtr);
    size_t frameIdx = ctx->frameCount ? *ctx->frameCount : 0;
    backend->tryStack_.push_back({catchAddr, stackBase, frameIdx});
}

/// R162: pop try handler（OP_TRY_END）
/// 仅当栈顶 handler 属于当前帧时弹出，与 StackVM 语义对齐。
extern "C" void jitPopTryHandler(JitContext* ctx) {
    if (!ctx || !ctx->backendPtr)
        return;
    auto* backend = static_cast<JITBackend*>(ctx->backendPtr);
    size_t currentFrameIdx = ctx->frameCount ? *ctx->frameCount : 0;
    if (!backend->tryStack_.empty() && backend->tryStack_.back().frameIndex == currentFrameIdx) {
        backend->tryStack_.pop_back();
    }
}

/// R162: push jump target 到 pendingJumpStack_（OP_PUSH_JUMP_TARGET）
/// JIT 代码通过 lea label 获取目标地址，传入此函数存储。
/// OP_FINALLY_END 从此栈 pop 地址并跳转，实现 break/continue 续跳。
extern "C" void jitPushJumpTarget(JitContext* ctx, void* targetAddr) {
    if (!ctx || !ctx->backendPtr)
        return;
    auto* backend = static_cast<JITBackend*>(ctx->backendPtr);
    // AUDIT-R7 F1 fix: 附带当前帧索引（防跨帧误跳，与三 VM 同构）
    size_t frameIdx = ctx->frameCount ? *ctx->frameCount : 0;
    backend->pendingJumpStack_.push_back({targetAddr, frameIdx});
}

/// R162: pop jump target（OP_FINALLY_END）
/// 返回目标地址供 JIT 代码 jmp，栈空时返回 nullptr（正常完成路径）。
/// AUDIT-R7 F1 fix: 只消费本帧条目，惰性丢弃已返回深帧残留；栈顶属更浅帧
/// （调用方在途续跳）时不消费，返回 nullptr 视为正常完成。
extern "C" void* jitPopJumpTarget(JitContext* ctx) {
    if (!ctx || !ctx->backendPtr)
        return nullptr;
    auto* backend = static_cast<JITBackend*>(ctx->backendPtr);
    size_t curFrameIdx = ctx->frameCount ? *ctx->frameCount : 0;
    while (!backend->pendingJumpStack_.empty() && backend->pendingJumpStack_.back().frameIndex > curFrameIdx) {
        backend->pendingJumpStack_.pop_back();
    }
    if (backend->pendingJumpStack_.empty() || backend->pendingJumpStack_.back().frameIndex != curFrameIdx)
        return nullptr;
    void* target = backend->pendingJumpStack_.back().addr;
    backend->pendingJumpStack_.pop_back();
    return target;
}

/// R162: 抛出异常 — 搜索 tryStack_、截断操作数栈、跨帧传播
///
/// 算法（与 StackVM VM::throwException 对齐）：
/// 1. 清空 pendingJumpStack_（异常中断 break/continue 续跳链）
/// 2. 搜索 tryStack_ 顶部当前帧的 handler：
///    - 找到：截断栈到 handler.stackBase，push thrownValue，返回 catchAddr
///    - 未找到且当前帧非 main：pop JitFrame、关闭 upvalue、恢复 r13，继续搜索
///    - 未找到且当前帧为 main：未捕获异常，设置错误标志，返回 nullptr
///
/// JIT 函数共享主 chunk 的栈帧（无独立 prologue/epilogue），因此 rbp/r14/rbx
/// 跨函数调用不变，只有 r13（basePointer）和 r15（stackTop）需要恢复。
///
/// @param ctx JitContext 指针
/// @param thrownValueBits 异常值的 NaN-boxing 原始位
/// @param currentR13 当前帧的 r13（用于关闭 upvalue）
/// @return catch 块地址（找到时）或 nullptr（未捕获）
extern "C" void* jitThrow(JitContext* ctx, uint64_t thrownValueBits, int64_t* currentR13) {
    if (!ctx || !ctx->backendPtr) {
        return nullptr;
    }
    auto* backend = static_cast<JITBackend*>(ctx->backendPtr);

    // 1. 清空 pendingJumpStack_（异常中断 break/continue 续跳链）
    backend->pendingJumpStack_.clear();

    int64_t* r13 = currentR13;

    while (true) {
        size_t currentFrameIdx = ctx->frameCount ? *ctx->frameCount : 0;

        // 2. 搜索 tryStack_ 顶部当前帧的 handler
        while (!backend->tryStack_.empty()) {
            auto& handler = backend->tryStack_.back();
            if (handler.frameIndex == currentFrameIdx) {
                // 找到 handler：截断栈、push thrownValue、返回 catchAddr
                //
                // JIT 栈向下增长（push: sub r15,8; mov [r15],val；pop: mov rax,[r15]; add r15,8），
                // r15/stackTop 指向栈顶值（空栈时指向栈基址）。
                // handler.stackBase = try_begin 时的 r15（指向当时栈顶）。
                // 截断 + push thrownValue：新栈顶 = handler.stackBase - 1（向低地址移动一槽），
                // [新栈顶] = thrownValueBits。
                if (handler.stackBase) {
                    // AUDIT-R5 R3 fix: 截断前先关闭指向被丢弃区域（地址 < stackBase，
                    // 即 try 开始后压入的槽位）的 open upvalue，对齐 StackVM
                    // throwException 的 P0-5 fix（closeUpvaluesFrom(handler.stackBase)）。
                    // 常规捕获目标是 r13-relative 局部变量槽（地址高于 stackBase，
                    // 不受影响），此处为防御性对齐：若未来有 upvalue 指向操作数栈
                    // 临时区，截断后不至于悬垂（UAF 读脏栈）。
                    backend->closeUpvaluesFrom(handler.stackBase - 1);
                    int64_t* newSp = handler.stackBase - 1;
                    *newSp = static_cast<int64_t>(thrownValueBits);
                    ctx->stackTop = newSp;
                }
                ctx->currentBp = r13; // 同帧：r13 不变；跨帧：已更新为调用者 r13
                backend->tryStack_.pop_back();
                return handler.catchAddr;
            }
            if (handler.frameIndex < currentFrameIdx) {
                break; // handler 在外层帧，需弹出当前帧
            }
            backend->tryStack_.pop_back(); // handler.frameIndex > currentFrameIdx：残留 handler
        }

        // 3. 当前帧无 handler
        if (currentFrameIdx == 0) {
            // main 帧未捕获异常
            Value thrownValue = bitsToValue(thrownValueBits);
            std::string str = thrownValue.toString();
            ErrorFormat::truncateForError(str);
            if (ctx->hasError)
                *ctx->hasError = true;
            if (ctx->errorBuffer)
                *ctx->errorBuffer = "未捕获的异常: " + str;
            return nullptr;
        }

        // 4. 弹出当前帧，传播到调用者
        size_t frameIdx = currentFrameIdx - 1;
        JitFrame& frame = ctx->frames[frameIdx];

        // 关闭当前帧的 open upvalues（addr <= r13）
        backend->closeUpvaluesFrom(r13);

        // 清理属于被弹出帧的 tryStack_ handler
        while (!backend->tryStack_.empty() && backend->tryStack_.back().frameIndex >= currentFrameIdx) {
            backend->tryStack_.pop_back();
        }

        // 弹出帧
        *ctx->frameCount = currentFrameIdx - 1;

        // 恢复调用者的 r13
        r13 = frame.callerBp;
    }
}

/// L14: 运行时错误转异常 — 将错误消息字符串转换为 Value 并调用 jitThrow。
/// 用于除零、索引越界等运行时错误路径，使 try/catch 能捕获这些错误。
/// @param ctx JitContext 指针
/// @param msg 错误消息（C 字符串，静态字符串）
/// @param currentR13 当前帧的 r13（用于关闭 upvalue）
/// @return catch 块地址（找到时）或 nullptr（未捕获，hasError 已由 jitThrow 设置）
extern "C" void* jitRuntimeThrow(JitContext* ctx, const char* msg, int64_t* currentR13) {
    if (!ctx)
        return nullptr;
    std::string str(msg ? msg : "runtime error");
    Value thrownValue(std::move(str));
    uint64_t thrownValueBits = valueToBits(thrownValue);
    return jitThrow(ctx, thrownValueBits, currentR13);
}

/// L14: 检查 hasError 并转为可捕获异常 — 用于 C++ 辅助函数（jitDivGeneric 等）
/// 在返回后检查错误标志，若已设置则将 errorBuffer 中的消息转为异常抛出。
/// @param ctx JitContext 指针
/// @param currentR13 当前帧的 r13
/// @return catch 块地址（找到时）或 nullptr（未捕获或无错误）
extern "C" void* jitCheckAndRethrow(JitContext* ctx, int64_t* currentR13) {
    if (!ctx || !ctx->hasError || !*ctx->hasError)
        return nullptr;
    std::string msg;
    if (ctx->errorBuffer)
        msg = *ctx->errorBuffer;
    if (msg.empty())
        msg = "运行时错误";
    // 清除 hasError — 异常将被捕获或不捕获，但不应保留旧的 hasError 标志
    // （jitThrow 在未捕获时会重新设置 hasError）
    *ctx->hasError = false;
    if (ctx->errorBuffer)
        ctx->errorBuffer->clear();
    Value thrownValue(std::move(msg));
    uint64_t thrownValueBits = valueToBits(thrownValue);
    return jitThrow(ctx, thrownValueBits, currentR13);
}

// ============================================================
// R162: 名称变量辅助函数（OP_DEFINE_VAR/OP_GET_VAR/OP_SET_VAR/OP_DELETE_VAR）
// 与 StackVM VM::executeVarNameOps 语义对齐，但无 inline cache（JIT 走 C++ helper 慢路径）
// ============================================================

/// R162: OP_DEFINE_VAR — pop 栈顶值，存入 globals_[name]（或 globalSlots_ 若 slot 存在）
/// 栈布局：[val] → []（pop val）
extern "C" void jitDefineVar(JitContext* ctx, const char* name) {
    if (!ctx || !ctx->stackTop || !name) {
        if (ctx && ctx->hasError) {
            *ctx->hasError = true;
            if (ctx->errorBuffer)
                *ctx->errorBuffer = "jitDefineVar: 无效参数";
        }
        return;
    }
    auto* backend = static_cast<JITBackend*>(ctx->backendPtr);
    if (!backend) {
        return;
    }
    // pop 栈顶值（JIT 栈向下增长，pop = sp 上移 +1）
    int64_t* sp = ctx->stackTop;
    uint64_t bits = static_cast<uint64_t>(*sp);
    sp += 1;
    ctx->stackTop = sp;
    Value val = bitsToValue(bits);
    // 检查 slot 快速路径（使用访问器避免 private 成员访问问题）
    auto& globalNameToSlot = backend->globalNameToSlotMut();
    auto& globalSlots = backend->globalSlotsMut();
    auto gsIt = globalNameToSlot.find(name);
    if (gsIt != globalNameToSlot.end() && gsIt->second < static_cast<int>(globalSlots.size())) {
        globalSlots[gsIt->second] = static_cast<int64_t>(bits);
    } else {
        backend->globals_[name] = std::move(val);
    }
}

/// R162: OP_GET_VAR — 查找 globals_[name]（或 globalSlots_），返回 raw bits 供 JIT 代码 push
/// @return raw bits of the value（错误时返回 JIT_NULL_BITS 并设置 hasError）
extern "C" uint64_t jitGetVar(JitContext* ctx, const char* name) {
    if (!ctx || !name) {
        return JIT_NULL_BITS;
    }
    auto* backend = static_cast<JITBackend*>(ctx->backendPtr);
    if (!backend) {
        return JIT_NULL_BITS;
    }
    // slot 快速路径（使用访问器避免 private 成员访问问题）
    auto& globalNameToSlot = backend->globalNameToSlotMut();
    auto& globalSlots = backend->globalSlotsMut();
    auto gsIt = globalNameToSlot.find(name);
    if (gsIt != globalNameToSlot.end() && gsIt->second < static_cast<int>(globalSlots.size())) {
        return static_cast<uint64_t>(globalSlots[gsIt->second]);
    }
    auto it = backend->globals_.find(name);
    if (it != backend->globals_.end()) {
        uint64_t bits;
        std::memcpy(&bits, &it->second, sizeof(uint64_t));
        return bits;
    }
    // 未找到
    if (ctx->hasError)
        *ctx->hasError = true;
    if (ctx->errorBuffer) {
        if (std::string(name) == "this") {
            *ctx->errorBuffer = ErrorMessages::kSuperOutsideMethod;
        } else {
            *ctx->errorBuffer = "未定义的变量: " + std::string(name);
        }
    }
    return JIT_NULL_BITS;
}

/// R162: OP_SET_VAR — pop 栈顶值，更新 globals_[name]（或 globalSlots_）
/// 栈布局：[val] → []（pop val）
extern "C" void jitSetVar(JitContext* ctx, const char* name) {
    if (!ctx || !ctx->stackTop || !name) {
        if (ctx && ctx->hasError) {
            *ctx->hasError = true;
            if (ctx->errorBuffer)
                *ctx->errorBuffer = "jitSetVar: 无效参数";
        }
        return;
    }
    auto* backend = static_cast<JITBackend*>(ctx->backendPtr);
    if (!backend) {
        return;
    }
    // pop 栈顶值（JIT 栈向下增长，pop = sp 上移 +1）
    int64_t* sp = ctx->stackTop;
    uint64_t bits = static_cast<uint64_t>(*sp);
    sp += 1;
    ctx->stackTop = sp;
    Value val = bitsToValue(bits);
    // slot 快速路径（使用访问器避免 private 成员访问问题）
    auto& globalNameToSlot = backend->globalNameToSlotMut();
    auto& globalSlots = backend->globalSlotsMut();
    auto gsIt = globalNameToSlot.find(name);
    if (gsIt != globalNameToSlot.end() && gsIt->second < static_cast<int>(globalSlots.size())) {
        globalSlots[gsIt->second] = static_cast<int64_t>(bits);
        return;
    }
    auto it = backend->globals_.find(name);
    if (it == backend->globals_.end()) {
        if (ctx->hasError)
            *ctx->hasError = true;
        if (ctx->errorBuffer)
            *ctx->errorBuffer = "未定义的变量: " + std::string(name);
        return;
    }
    it->second = std::move(val);
}

/// R162: OP_DELETE_VAR — 从 globals_ 删除 name（或 globalSlots_ 置 null）
/// 无栈效应
extern "C" void jitDeleteVar(JitContext* ctx, const char* name) {
    if (!ctx || !name) {
        return;
    }
    auto* backend = static_cast<JITBackend*>(ctx->backendPtr);
    if (!backend) {
        return;
    }
    // 使用访问器避免 private 成员访问问题
    auto& globalNameToSlot = backend->globalNameToSlotMut();
    auto& globalSlots = backend->globalSlotsMut();
    auto gsIt = globalNameToSlot.find(name);
    if (gsIt != globalNameToSlot.end() && gsIt->second < static_cast<int>(globalSlots.size())) {
        globalSlots[gsIt->second] = static_cast<int64_t>(JIT_NULL_BITS);
    } else {
        backend->globals_.erase(name);
    }
}


} // extern "C"

#endif // MINILANG_USE_JIT
