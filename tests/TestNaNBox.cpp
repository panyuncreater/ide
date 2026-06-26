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
