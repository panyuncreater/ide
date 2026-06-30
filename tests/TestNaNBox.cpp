// ============================================================
// NaNBox 单元测试（PERF-12）
// ------------------------------------------------------------
// 验证 8 字节 NaN-boxing 编码/解码的正确性：
//   1. double 编码/解码（含边界值）
//   2. int48 编码/解码（含符号扩展，tag 与 payload 不重叠）
//   3. bool 编码/解码
//   4. null 编码/解码
//   5. 指针编码/解码
//   6. 类型互斥性
//   7. sizeof 断言
// ============================================================

#include <gtest/gtest.h>
#include "interpreter/NaNBox.h"
#include <limits>
#include <cmath>

// ---- sizeof 断言 ----

TEST(NaNBoxTest, SizeIs8Bytes) {
    EXPECT_EQ(sizeof(NaNBox), 8u);
}

// ---- double 编码/解码 ----

TEST(NaNBoxTest, EncodeDecodeDouble) {
    double values[] = {
        0.0, -0.0, 1.0, -1.0, 3.14159, -2.71828,
        1e100, -1e-100, 1e308, -1e308,
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
    };
    for (double v : values) {
        NaNBox box = NaNBox::fromFloat(v);
        EXPECT_TRUE(box.isFloat()) << "value=" << v;
        EXPECT_EQ(box.asFloat(), v) << "value=" << v;
    }
}

TEST(NaNBoxTest, EncodeDecodeDoubleZero) {
    NaNBox box = NaNBox::fromFloat(0.0);
    EXPECT_TRUE(box.isFloat());
    EXPECT_EQ(box.asFloat(), 0.0);
}

TEST(NaNBoxTest, EncodeDecodeDoubleNegative) {
    NaNBox box = NaNBox::fromFloat(-42.5);
    EXPECT_TRUE(box.isFloat());
    EXPECT_EQ(box.asFloat(), -42.5);
}

// ---- int48 编码/解码 ----
// tag field（高 16 位）与 payload（低 48 位）完全不重叠

TEST(NaNBoxTest, EncodeDecodeInt) {
    int64_t values[] = {
        0, 1, -1, 42, -42, 255, -255,
        1024, -1024, 65535, -65535,
        1000000, -1000000,
        // int48 范围边界附近
        (1LL << 40), -(1LL << 40),
        (1LL << 46), -(1LL << 46),
        (1LL << 47) - 1, -(1LL << 47),  // max 与 min 边界
    };
    for (int64_t v : values) {
        ASSERT_TRUE(NaNBox::canEncodeInt(v)) << "value=" << v;
        NaNBox box = NaNBox::fromInt(v);
        EXPECT_TRUE(box.isInt()) << "value=" << v;
        EXPECT_EQ(box.asInt(), v) << "value=" << v;
    }
}

TEST(NaNBoxTest, Int48MaxValue) {
    // int48 最大值：2^47 - 1
    int64_t maxInt48 = (1LL << 47) - 1;
    ASSERT_TRUE(NaNBox::canEncodeInt(maxInt48));
    NaNBox box = NaNBox::fromInt(maxInt48);
    EXPECT_TRUE(box.isInt());
    EXPECT_EQ(box.asInt(), maxInt48);
}

TEST(NaNBoxTest, Int48MinValue) {
    // int48 最小值：-2^47
    int64_t minInt48 = -(1LL << 47);
    ASSERT_TRUE(NaNBox::canEncodeInt(minInt48));
    NaNBox box = NaNBox::fromInt(minInt48);
    EXPECT_TRUE(box.isInt());
    EXPECT_EQ(box.asInt(), minInt48);
}

TEST(NaNBoxTest, Int48OverflowCannotEncode) {
    // 超出 int48 范围
    EXPECT_FALSE(NaNBox::canEncodeInt(1LL << 47));        // 刚好越界（max+1）
    EXPECT_FALSE(NaNBox::canEncodeInt(-(1LL << 47) - 1)); // 刚好越界（min-1）
    EXPECT_FALSE(NaNBox::canEncodeInt(1LL << 60));
    EXPECT_FALSE(NaNBox::canEncodeInt(std::numeric_limits<int64_t>::max()));
    EXPECT_FALSE(NaNBox::canEncodeInt(std::numeric_limits<int64_t>::min()));
}

// ---- bool 编码/解码 ----

TEST(NaNBoxTest, EncodeDecodeBoolTrue) {
    NaNBox box = NaNBox::fromBool(true);
    EXPECT_TRUE(box.isBool());
    EXPECT_TRUE(box.asBool());
}

TEST(NaNBoxTest, EncodeDecodeBoolFalse) {
    NaNBox box = NaNBox::fromBool(false);
    EXPECT_TRUE(box.isBool());
    EXPECT_FALSE(box.asBool());
}

// ---- null 编码/解码 ----

TEST(NaNBoxTest, EncodeDecodeNull) {
    NaNBox box = NaNBox::null();
    EXPECT_TRUE(box.isNull());
}

TEST(NaNBoxTest, DefaultConstructorIsNull) {
    NaNBox box;
    EXPECT_TRUE(box.isNull());
}

// ---- 指针编码/解码 ----

TEST(NaNBoxTest, EncodeDecodePointer) {
    int x = 42;
    NaNBox box = NaNBox::fromPtr(&x);
    EXPECT_TRUE(box.isPointer());
    int* recovered = box.asPtr<int>();
    EXPECT_EQ(recovered, &x);
    EXPECT_EQ(*recovered, 42);
}

TEST(NaNBoxTest, EncodeDecodeNullPointer) {
    NaNBox box = NaNBox::fromPtr(nullptr);
    EXPECT_TRUE(box.isPointer());
    void* recovered = box.asVoidPtr();
    EXPECT_EQ(recovered, nullptr);
}

TEST(NaNBoxTest, EncodeDecodeStringPointer) {
    std::string s = "hello";
    const std::string* ptr = &s;
    NaNBox box = NaNBox::fromPtr(ptr);
    EXPECT_TRUE(box.isPointer());
    const std::string* recovered = box.asPtr<std::string>();
    EXPECT_EQ(recovered, ptr);
    EXPECT_EQ(*recovered, "hello");
}

// ---- 类型互斥性 ----

TEST(NaNBoxTest, TypeMutualExclusivity) {
    NaNBox f = NaNBox::fromFloat(3.14);
    NaNBox i = NaNBox::fromInt(42);
    NaNBox b = NaNBox::fromBool(true);
    NaNBox n = NaNBox::null();
    int x = 0;
    NaNBox p = NaNBox::fromPtr(&x);

    EXPECT_TRUE(f.isFloat() && !f.isInt() && !f.isBool() && !f.isNull() && !f.isPointer());
    EXPECT_TRUE(!i.isFloat() && i.isInt() && !i.isBool() && !i.isNull() && !i.isPointer());
    EXPECT_TRUE(!b.isFloat() && !b.isInt() && b.isBool() && !b.isNull() && !b.isPointer());
    EXPECT_TRUE(!n.isFloat() && !n.isInt() && !n.isBool() && n.isNull() && !n.isPointer());
    EXPECT_TRUE(!p.isFloat() && !p.isInt() && !p.isBool() && !p.isNull() && p.isPointer());
}

// ---- 相等性 ----

TEST(NaNBoxTest, Equality) {
    NaNBox a = NaNBox::fromInt(42);
    NaNBox b = NaNBox::fromInt(42);
    NaNBox c = NaNBox::fromInt(43);
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);

    NaNBox d = NaNBox::fromFloat(3.14);
    NaNBox e = NaNBox::fromFloat(3.14);
    EXPECT_EQ(d, e);

    EXPECT_NE(NaNBox::fromInt(1), NaNBox::fromFloat(1.0));  // 不同类型不等
}

// ---- rawBits 稳定性 ----

TEST(NaNBoxTest, RawBitsStable) {
    NaNBox i = NaNBox::fromInt(42);
    NaNBox j = NaNBox::fromInt(42);
    EXPECT_EQ(i.rawBits(), j.rawBits());

    NaNBox n = NaNBox::null();
    // null 的 rawBits 应该是固定值
    EXPECT_GT(n.rawBits(), 0ULL);
}

// ============================================================
// NaN-boxing 编码安全审计测试（AUDIT-NANBOX）
// ------------------------------------------------------------
// 验证 NaN-boxing 最经典的 bug：某个合法 double 的位模式与 tag 冲突，
// 被误判为 int/bool/null/pointer。fromFloat 的规范化必须阻止这种冲突。
// ============================================================

namespace {
// 从原始 64 位位模式构造 double（用于测试特定位模式的 double）
double doubleFromBits(uint64_t bits) {
    double d;
    std::memcpy(&d, &bits, sizeof(double));
    return d;
}
// 规范化后的 float NaN 标记值（必须与 NaNBox.h 私有常量一致）
constexpr uint64_t NAN_BOXED_FLOAT_MARKER = 0x7FFC000000000000ULL;
}  // namespace

// ---- 经典 NaN-boxing 冲突：double 位模式落入 INT tag 范围 ----
// 0x7FF8000000000001 是一个 quiet NaN，高 16 位 = 0x7FF8 = INT_TAG_BASE。
// 若不规范化，tag() 会误判为 INT，asInt() 返回 1。
// fromFloat 必须将其规范化为 0x7FFC000000000000（float NaN 标记）。
TEST(NaNBoxTest, AuditNaNConflictWithIntTag) {
    double d = doubleFromBits(0x7FF8000000000001ULL);
    NaNBox box = NaNBox::fromFloat(d);
    EXPECT_TRUE(box.isFloat()) << "quiet NaN with INT tag bits must be float, not int";
    EXPECT_FALSE(box.isInt());
    EXPECT_FALSE(box.isPointer());
    // 规范化后位模式必须是标记值
    EXPECT_EQ(box.rawBits(), NAN_BOXED_FLOAT_MARKER);
    // 读回应是 NaN
    EXPECT_TRUE(std::isnan(box.asFloat()));
}

// ---- 经典 NaN-boxing 冲突：double 位模式落入 BOOL tag 范围 ----
// 0x7FF9000000000001 高 16 位 = 0x7FF9 = BOOL_TAG_BASE，看起来像 bool true。
TEST(NaNBoxTest, AuditNaNConflictWithBoolTag) {
    double d = doubleFromBits(0x7FF9000000000001ULL);
    NaNBox box = NaNBox::fromFloat(d);
    EXPECT_TRUE(box.isFloat()) << "quiet NaN with BOOL tag bits must be float, not bool";
    EXPECT_FALSE(box.isBool());
    EXPECT_EQ(box.rawBits(), NAN_BOXED_FLOAT_MARKER);
}

// ---- 经典 NaN-boxing 冲突：double 位模式落入 NULL tag 范围 ----
// 0x7FFA000000000000 高 16 位 = 0x7FFA = NULL_BITS，看起来像 null。
TEST(NaNBoxTest, AuditNaNConflictWithNullTag) {
    double d = doubleFromBits(0x7FFA000000000000ULL);
    NaNBox box = NaNBox::fromFloat(d);
    EXPECT_TRUE(box.isFloat()) << "quiet NaN with NULL tag bits must be float, not null";
    EXPECT_FALSE(box.isNull());
    EXPECT_EQ(box.rawBits(), NAN_BOXED_FLOAT_MARKER);
}

// ---- 经典 NaN-boxing 冲突：double 位模式落入 POINTER tag 范围 ----
// 0x7FFB000000000000 高 16 位 = 0x7FFB = PTR_TAG_BASE，看起来像空指针。
TEST(NaNBoxTest, AuditNaNConflictWithPtrTag) {
    double d = doubleFromBits(0x7FFB000000000000ULL);
    NaNBox box = NaNBox::fromFloat(d);
    EXPECT_TRUE(box.isFloat()) << "quiet NaN with PTR tag bits must be float, not pointer";
    EXPECT_FALSE(box.isPointer());
    EXPECT_EQ(box.rawBits(), NAN_BOXED_FLOAT_MARKER);
}

// ---- tag 范围上界：0x7FFC 不在 boxed 范围内，原样存储 ----
// 0x7FFC000000000000 是规范化标记本身，作为 double 输入时不应被二次规范化。
TEST(NaNBoxTest, AuditNaNAboveTagRangeNotCanonicalized) {
    double d = doubleFromBits(0x7FFC000000000001ULL);  // quiet NaN，高 16 位 = 0x7FFC
    NaNBox box = NaNBox::fromFloat(d);
    EXPECT_TRUE(box.isFloat());
    // 不在 [0x7FF8, 0x7FFB] 范围，不规范化，原样保留
    EXPECT_EQ(box.rawBits(), 0x7FFC000000000001ULL);
    EXPECT_TRUE(std::isnan(box.asFloat()));
}

// ---- signaling NaN 不在 tag 范围，原样存储 ----
// signaling NaN bit 51 = 0，高 16 位在 [0x7FF0, 0x7FF7]，不与任何 tag 冲突。
TEST(NaNBoxTest, AuditSignalingNaNStoredAsFloat) {
    double d = doubleFromBits(0x7FF0000000000001ULL);  // signaling NaN
    NaNBox box = NaNBox::fromFloat(d);
    EXPECT_TRUE(box.isFloat());
    EXPECT_FALSE(box.isInt());
    EXPECT_FALSE(box.isPointer());
    // 原样保留
    EXPECT_EQ(box.rawBits(), 0x7FF0000000000001ULL);
    EXPECT_TRUE(std::isnan(box.asFloat()));
}

// ---- 无穷大与负零：位模式不与 tag 冲突 ----
TEST(NaNBoxTest, AuditInfinityAndNegativeZero) {
    // +inf = 0x7FF0000000000000，-inf = 0xFFF0000000000000
    NaNBox posInf = NaNBox::fromFloat(std::numeric_limits<double>::infinity());
    NaNBox negInf = NaNBox::fromFloat(-std::numeric_limits<double>::infinity());
    EXPECT_TRUE(posInf.isFloat());
    EXPECT_TRUE(negInf.isFloat());
    EXPECT_EQ(posInf.asFloat(), std::numeric_limits<double>::infinity());

    // -0.0 = 0x8000000000000000，高 16 位 = 0x8000，不与 tag 冲突
    NaNBox negZero = NaNBox::fromFloat(-0.0);
    EXPECT_TRUE(negZero.isFloat());
    EXPECT_EQ(negZero.asFloat(), -0.0);
    // -0.0 与 +0.0 位模式不同但数值比较相等（IEEE 754）
    EXPECT_EQ(negZero.rawBits(), 0x8000000000000000ULL);
}

// ---- 两个不同的 in-range NaN 被规范化为同一位模式 ----
// 这是有意行为：冲突 NaN 的 payload 被丢弃，统一为标记值。
// NaN != NaN（IEEE 754），所以语义上仍正确。
TEST(NaNBoxTest, AuditDistinctNaNsCanonicalizedEqual) {
    double nan1 = doubleFromBits(0x7FF8000000000001ULL);
    double nan2 = doubleFromBits(0x7FFB0000DEADBEEFULL);
    NaNBox b1 = NaNBox::fromFloat(nan1);
    NaNBox b2 = NaNBox::fromFloat(nan2);
    // 两者都被规范化为同一位模式
    EXPECT_EQ(b1.rawBits(), b2.rawBits());
    EXPECT_EQ(b1.rawBits(), NAN_BOXED_FLOAT_MARKER);
    // 位模式相等（NaNBox operator== 比较原始位）
    EXPECT_EQ(b1, b2);
}

// ---- int48 边界值符号扩展正确性 ----
TEST(NaNBoxTest, AuditInt48SignExtension) {
    // -1 的 int48 payload = 0x0000FFFFFFFFFFFF，bit 47 = 1 → 符号扩展
    NaNBox negOne = NaNBox::fromInt(-1);
    EXPECT_EQ(negOne.asInt(), -1);
    EXPECT_EQ(negOne.rawBits() & 0x0000FFFFFFFFFFFFULL, 0x0000FFFFFFFFFFFFULL);

    // 最大正 int48 = 2^47 - 1 = 0x00007FFFFFFFFFFF，bit 47 = 0 → 无符号扩展
    int64_t maxInt48 = (1LL << 47) - 1;
    NaNBox maxBox = NaNBox::fromInt(maxInt48);
    EXPECT_EQ(maxBox.asInt(), maxInt48);

    // 最小负 int48 = -2^47，payload = 0x0000800000000000，bit 47 = 1 → 符号扩展
    int64_t minInt48 = -(1LL << 47);
    NaNBox minBox = NaNBox::fromInt(minInt48);
    EXPECT_EQ(minBox.asInt(), minInt48);

    // 关键：最小负 int48 的 payload bit 47 = 1，但不能与 tag 重叠
    // tag 在 bits 48-63，payload bit 47 在 payload 域内，无重叠
    EXPECT_EQ(minBox.rawBits() & 0xFFFF000000000000ULL, 0x7FF8000000000000ULL)
        << "INT tag must occupy high 16 bits, not overlap with int48 sign bit";
}

// ---- 类型互斥性：所有 tag 的 rawBits 高 16 位互不重叠 ----
TEST(NaNBoxTest, AuditTagBitsMutuallyExclusive) {
    NaNBox i = NaNBox::fromInt(0);
    NaNBox b = NaNBox::fromBool(false);
    NaNBox n = NaNBox::null();
    int x = 0;
    NaNBox p = NaNBox::fromPtr(&x);

    uint64_t iTag = i.rawBits() & 0xFFFF000000000000ULL;
    uint64_t bTag = b.rawBits() & 0xFFFF000000000000ULL;
    uint64_t nTag = n.rawBits() & 0xFFFF000000000000ULL;
    uint64_t pTag = p.rawBits() & 0xFFFF000000000000ULL;

    EXPECT_EQ(iTag, 0x7FF8000000000000ULL);
    EXPECT_EQ(bTag, 0x7FF9000000000000ULL);
    EXPECT_EQ(nTag, 0x7FFA000000000000ULL);
    EXPECT_EQ(pTag, 0x7FFB000000000000ULL);
    // 四个 tag 互不相同
    EXPECT_NE(iTag, bTag);
    EXPECT_NE(iTag, nTag);
    EXPECT_NE(iTag, pTag);
    EXPECT_NE(bTag, nTag);
    EXPECT_NE(bTag, pTag);
    EXPECT_NE(nTag, pTag);
}
