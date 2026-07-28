#pragma once

#include "common/Logger.h" // BUG-001/002 fix: 降级路径需记录日志
#include <cassert>
#include <cstdint>
#include <cstdio>  // std::snprintf — BUG-002 fix 指针格式化
#include <cstdlib> // std::abort — Release 构建中 assert 兜底，避免 UB
#include <cstring>
#include <string>

// ============================================================
// NaNBox — 8 字节 NaN-boxing 值编码（PERF-12 基础设施）
// ------------------------------------------------------------
// 将多种类型编码到单个 64 位字中，消除 shared_ptr 引用计数开销
// 和 variant 的 24 字节内存开销。
//
// 编码方案（基于 IEEE 754 double 的 quiet NaN 空间）：
//
//   1. VAL_FLOAT: 直接存储 double 的 64 位原始位（非 NaN-boxed 时）
//   2. VAL_INT:   tag = 0x7FF8..., payload = int48 值（低 48 位）
//   3. VAL_BOOL:  tag = 0x7FF9..., payload = 0/1
//   4. VAL_NULL:  0x7FFA000000000000
//   5. 指针类型:   tag = 0x7FFB..., payload = 48 位指针
//
// 布局：高 16 位（bits 63-48）为 tag field，低 48 位（bits 47-0）为 payload。
// tag 与 payload 完全不重叠，避免 payload 满载时污染 tag。
// 所有 tag base 设置 IEEE 754 quiet NaN 位（bit 51 = 1）+ exponent 全 1。
//
// 限制：
//   - int64_t 超出 int48 范围（|v| >= 2^47）时无法内联，调用者需 fallback
//   - 指针类型使用 48 位（x86-64 用户态虚拟地址空间）
//   - 指针类型不管理引用计数，调用者需确保指针有效性
// ============================================================

class NaNBox {
public:
    /// 类型标签
    enum class Tag : uint8_t {
        FLOAT = 0,   // 直接存储 double 原始位
        INT = 1,     // int48 内联
        BOOL = 2,    // bool 内联
        NUL = 3,     // null
        POINTER = 4, // 48 位指针
    };

    /// 构造 null
    NaNBox() : bits_(NULL_BITS) {}

    // ---- 编码工厂方法 ----

    static NaNBox fromFloat(double d) {
        NaNBox box;
        std::memcpy(&box.bits_, &d, sizeof(double));
        // 如果 double 恰好落入 NaN-boxed tag 范围，需要规范化
        // 罕见情况：double 是一个 quiet NaN 且高 16 位与我们的 tag 冲突
        // （INT_TAG_BASE=0x7FF8, BOOL_TAG_BASE=0x7FF9, NULL_BITS=0x7FFA, PTR_TAG_BASE=0x7FFB）
        // 必须将整个 64 位替换为 NAN_BOXED_FLOAT_MARKER（0x7FFC...），
        // 丢弃原始 NaN payload——否则低 48 位可能形成合法 int/bool/null/ptr 位模式，
        // 导致 tag() 误判。NaN != NaN（IEEE 754），payload 丢弃不影响语义正确性。
        if (isBoxedNaN(box.bits_)) {
            box.bits_ = NAN_BOXED_FLOAT_MARKER;
        }
        return box;
    }

    static NaNBox fromInt(int64_t i) {
        // BUG-001 fix: 超范围值不再 abort 整个 IDE 进程，而是降级为 float 编码。
        // 设计权衡：
        //   - 唯一合法调用方 Value(int64_t) 已在 canEncodeInt 检查后走 BoxedIntData 路径，
        //     直接调用 fromInt 的代码若超范围属于编程错误。
        //   - 但 abort 会让 IDE 进程整体崩溃（用户代码 bug 不应导致 IDE 退出），
        //     故采用 BUG_REPORT.md 方案 B：降级为 float，损失精度但不崩溃。
        //   - 调用方可用 isInt() 区分降级情形；正常运行路径不会触发此分支。
        if (!canEncodeInt(i)) {
            LOG_WARNING("NaNBox::fromInt 超范围降级为 float: value=" + std::to_string(i), "NaNBox");
            return fromFloat(static_cast<double>(i));
        }
        NaNBox box;
        // 将 int64 截断为 int48（保留符号位扩展）
        uint64_t payload = static_cast<uint64_t>(i) & INT48_MASK;
        box.bits_ = INT_TAG_BASE | payload;
        return box;
    }

    static NaNBox fromBool(bool b) {
        NaNBox box;
        box.bits_ = BOOL_TAG_BASE | (b ? 1ULL : 0ULL);
        return box;
    }

    static NaNBox null() {
        NaNBox box;
        box.bits_ = NULL_BITS;
        return box;
    }

    static NaNBox fromPtr(const void* ptr) {
        NaNBox box;
        uint64_t ptrBits = reinterpret_cast<uint64_t>(ptr);
        // BUG-002 fix: 高 16 位非零的指针（内核地址 / ASLR 极端布局 / 自定义分配器）
        // 无法用 48 位 payload 编码。原实现直接 abort 让 IDE 整体崩溃，对用户不友好。
        // 改为：先记录详细错误日志（指针值 + 来源），再 abort。
        //   - 指针无法"降级"为其他类型（不同于 fromInt 可降级为 float），故仍需 abort
        //     防止后续 asPtr 解码出无效地址触发更难诊断的访问冲突。
        //   - 日志记录后 abort，开发者可在日志中定位首个触发点。
        //   - 正常 x86-64 用户空间地址高 16 位为 0，此分支仅在异常环境下触发。
        //
        // L9 fix 审计结论（2026-07-19）：
        //   评估为"已知限制但实际不可触发"，保留 abort 作为防御性编程最后防线。
        //   理由：(1) x86-64 用户空间虚拟地址范围 [0x0, 0x0000_7FFF_FFFF_FFFF] 由
        //   硬件规范保证（Windows/Linux/macOS 均遵循），高 16 位始终为 0；
        //   (2) 侵入式 RefCounted 通过 new 分配，必在用户空间堆内，高 16 位为 0；
        //   (3) 改造为 BoxedPtrData 堆包装路径需新增 ValueType + 修改所有 asPtr
        //   调用点（GcManager/Value.cpp/MemoryModelPanel 等约 20+ 处）+ 三后端同步，
        //   改造风险高于 abort 本身（教学场景下不可触发）。
        //   与 L10（PCH 全量重编译）的处理策略一致——硬件规范保证的不变量保留
        //   防御性 abort，文档化说明而非改造代码。
        if ((ptrBits & ~PTR_MASK) != 0) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "0x%016llx", static_cast<unsigned long long>(ptrBits));
            LOG_ERROR("NaNBox::fromPtr 高位非零指针无法编码: " + std::string(buf) +
                          "（48 位 payload 范围内才能安全编码）",
                      "NaNBox");
            Logger::instance().flush();
            std::abort();
        }
        box.bits_ = PTR_TAG_BASE | (ptrBits & PTR_MASK);
        return box;
    }

    /// 从原始 bits 构造（用于 JIT/序列化等需要位级操作的路径，如 Value::fromBitsBorrowed）。
    /// 内部对 uint64_t 成员 bits_ 做 memcpy（uint64_t 可平凡拷贝），
    /// 避免在 Value.h 中直接 memcpy 整个 NaNBox 对象触发 GCC -Wclass-memaccess。
    /// 调用方需保证 bits 来自合法的 rawBits()/bitsOf()，否则解码行为未定义。
    static NaNBox fromBits(uint64_t bits) {
        NaNBox box;
        std::memcpy(&box.bits_, &bits, sizeof(uint64_t));
        return box;
    }

    // ---- 类型查询 ----

    Tag tag() const {
        if (!isBoxedNaN(bits_))
            return Tag::FLOAT;
        uint64_t tagField = bits_ & TAG_FIELD_MASK;
        if (tagField == INT_TAG_BASE)
            return Tag::INT;
        if (tagField == BOOL_TAG_BASE)
            return Tag::BOOL;
        if (bits_ == NULL_BITS)
            return Tag::NUL;
        if (tagField == PTR_TAG_BASE)
            return Tag::POINTER;
        // 不认识的 tag（规范化后的 float NaN），当作 float
        return Tag::FLOAT;
    }

    bool isFloat() const { return tag() == Tag::FLOAT; }
    bool isInt() const { return tag() == Tag::INT; }
    bool isBool() const { return tag() == Tag::BOOL; }
    bool isNull() const { return tag() == Tag::NUL; }
    bool isPointer() const { return tag() == Tag::POINTER; }

    bool isScalar() const {
        // 所有 NaNBox 值都是"标量"（8 字节，无 shared_ptr）
        return true;
    }

    // ---- 值提取 ----
    // P0 fix: 所有 asXxx 的 assert 在 Release 被剥离，类型不匹配时静默返回
    // 垃圾值（如把指针当 int 解读），掩盖底层 bug。改为运行时 abort。
    // 调用方应先调用 isXxx() 检查类型后再调用 asXxx()。

    double asFloat() const {
        if (!isFloat()) {
            std::abort();
        }
        double d;
        std::memcpy(&d, &bits_, sizeof(double));
        return d;
    }

    int64_t asInt() const {
        if (!isInt()) {
            std::abort();
        }
        // int48 符号扩展：将 bit 47 扩展到高位
        uint64_t payload = bits_ & INT48_MASK;
        if (payload & INT47_SIGN_BIT) {
            payload |= ~INT48_MASK; // 高位填 1
        }
        return static_cast<int64_t>(payload);
    }

    bool asBool() const {
        if (!isBool()) {
            std::abort();
        }
        return (bits_ & 1ULL) != 0;
    }

    template <typename T> T* asPtr() const {
        if (!isPointer()) {
            std::abort();
        }
        // 零扩展：x86-64 用户空间指针高 16 位始终为 0，直接取低 48 位即可。
        // 不能使用符号扩展——Windows x64 用户空间地址（如 0x00007FFD...）的
        // bit 47 = 1，符号扩展会错误地将高 16 位填为 0xFFFF，产生无效内核地址。
        uint64_t ptrBits = bits_ & PTR_MASK;
        return reinterpret_cast<T*>(ptrBits);
    }

    void* asVoidPtr() const { return asPtr<void>(); }

    // ---- 容量查询 ----

    /// 检查 int64 值是否可以内联编码为 int48
    static bool canEncodeInt(int64_t i) {
        // int48 范围：-(2^47) 到 (2^47 - 1)
        return i >= -(static_cast<int64_t>(1) << 47) && i < (static_cast<int64_t>(1) << 47);
    }

    /// 原始 bits（用于调试和哈希）
    uint64_t rawBits() const { return bits_; }

    // ---- 比较 ----

    bool operator==(const NaNBox& other) const { return bits_ == other.bits_; }
    bool operator!=(const NaNBox& other) const { return bits_ != other.bits_; }

private:
    uint64_t bits_;

    // ---- 常量定义 ----
    // IEEE 754 quiet NaN：sign(1) + exponent_all_ones(11) + quiet_nan_bit(1) = 0x7FF8
    // 高 16 位（bits 63-48）作为 tag field，低 48 位（bits 47-0）作为 payload
    // tag 与 payload 完全不重叠，确保 payload 满载不会污染 tag
    //
    // Tag 编码（高 16 位，均含 quiet NaN bit 51）：
    //   0x7FF8 = INT     (payload = int48 在低 48 位)
    //   0x7FF9 = BOOL    (payload = 0/1 在最低位)
    //   0x7FFA = NULL    (无 payload)
    //   0x7FFB = POINTER (payload = 48 位指针)

    static constexpr uint64_t TAG_FIELD_MASK = 0xFFFF000000000000ULL; // 高 16 位
    static constexpr uint64_t INT48_MASK = 0x0000FFFFFFFFFFFFULL;     // 低 48 位
    static constexpr uint64_t INT47_SIGN_BIT = 0x0000800000000000ULL; // bit 47（int48 符号位）
    static constexpr uint64_t PTR_MASK = 0x0000FFFFFFFFFFFFULL;       // 低 48 位

    // Tag 基址（高 16 位，均含 quiet NaN bit 51）
    static constexpr uint64_t INT_TAG_BASE = 0x7FF8000000000000ULL;  // int48
    static constexpr uint64_t BOOL_TAG_BASE = 0x7FF9000000000000ULL; // bool
    static constexpr uint64_t NULL_BITS = 0x7FFA000000000000ULL;     // null
    static constexpr uint64_t PTR_TAG_BASE = 0x7FFB000000000000ULL;  // 指针

    // 规范化的 float NaN 标记：当 double 的位模式落入 NaN-boxed 范围时，
    // 用此值替代（仍是一个 quiet NaN，读取时按 double 解码）
    static constexpr uint64_t NAN_BOXED_FLOAT_MARKER = 0x7FFC000000000000ULL;

    /// 检查 64 位值是否是 NaN-boxed 值（高 16 位在我们的 tag 范围内）
    static bool isBoxedNaN(uint64_t bits) {
        uint64_t tagField = bits & TAG_FIELD_MASK;
        return tagField >= INT_TAG_BASE && tagField <= PTR_TAG_BASE;
    }
};

// 编译期断言：NaNBox 必须是 8 字节
static_assert(sizeof(NaNBox) == 8, "NaNBox must be 8 bytes");
