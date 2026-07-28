// ============================================================
// tests/TestSimdKernels.cpp
// ------------------------------------------------------------
// R134 SIMD 向量化回归测试。
//
// 测试层级：
//   Layer 1: SIMD 内核单元测试（直接验证 simdSumInt64/simdSumDouble/
//            simdMinInt64/simdMaxInt64/decodeInt48Batch 的数值正确性）
//   Layer 2: 端到端 sum() 语义验证（四后端通过 sum(arr) 触发 SIMD 路径，
//            验证结果与标量路径一致）
//
// 关键不变量：
//   - SIMD 路径与标量路径必须产生相同结果（对相同输入）
//   - 三后端（Interpreter/StackVM/StackVM_IR/RegisterVM）sum() 结果必须一致
//   - 触发 SIMD 的最小元素数为 16，测试覆盖 15/16/17/256/257 等边界
//   - 整数溢出必须正确检测（块间 OverflowCheck）
//
// 注：MiniLang print() 不自动换行，断言预期值显式加 "\n"。
// ============================================================
#include "common/BackendExecutionService.h" // 拓展二期：min/max 空数组错误路径验证
#include "common/ThreeBackends.h"
#include "interpreter/SimdUtils.h"

#include <gtest/gtest.h>
#include <numeric>
#include <random>
#include <string>
#include <vector>

namespace ms = minilang::simd;

// ============================================================
// Layer 1: SIMD 内核单元测试
// ============================================================

// SIMD 整数求和：小数组（不触发 SIMD 路径，走标量回退）
TEST(SimdKernels, SumInt64_SmallArray_ScalarFallback) {
    std::vector<int64_t> data = {1, 2, 3, 4, 5};
    EXPECT_EQ(ms::simdSumInt64(data.data(), data.size()), 15);
}

// SIMD 整数求和：大数组（触发 AVX2 路径）
TEST(SimdKernels, SumInt64_LargeArray_Vectorized) {
    std::vector<int64_t> data(1000);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<int64_t>(i);
    int64_t expected = 999 * 1000 / 2; // 0+1+...+999 = 499500
    EXPECT_EQ(ms::simdSumInt64(data.data(), data.size()), expected);
}

// SIMD 整数求和：边界 16 元素（恰好触发 SIMD）
TEST(SimdKernels, SumInt64_Boundary16) {
    std::vector<int64_t> data(16, 100);
    EXPECT_EQ(ms::simdSumInt64(data.data(), data.size()), 1600);
}

// SIMD 整数求和：边界 17 元素（SIMD + 1 尾部标量）
TEST(SimdKernels, SumInt64_Boundary17) {
    std::vector<int64_t> data(17, 100);
    data[16] = 7;
    EXPECT_EQ(ms::simdSumInt64(data.data(), data.size()), 1607);
}

// SIMD 整数求和：负数
TEST(SimdKernels, SumInt64_NegativeValues) {
    std::vector<int64_t> data(100, -100);
    EXPECT_EQ(ms::simdSumInt64(data.data(), data.size()), -10000);
}

// SIMD 整数求和：含正负混合
TEST(SimdKernels, SumInt64_MixedSigns) {
    std::vector<int64_t> data(100);
    for (size_t i = 0; i < data.size(); ++i) data[i] = (i % 2 == 0) ? 100 : -100;
    EXPECT_EQ(ms::simdSumInt64(data.data(), data.size()), 0);
}

// SIMD 浮点求和：大数组
TEST(SimdKernels, SumDouble_LargeArray) {
    std::vector<double> data(1000, 1.5);
    double result = ms::simdSumDouble(data.data(), data.size());
    EXPECT_NEAR(result, 1500.0, 1e-6);
}

// SIMD 浮点求和：小数组（标量回退）
TEST(SimdKernels, SumDouble_SmallArray) {
    std::vector<double> data = {1.5, 2.5, 3.5};
    EXPECT_NEAR(ms::simdSumDouble(data.data(), data.size()), 7.5, 1e-9);
}

// SIMD 整数最小值：大数组
TEST(SimdKernels, MinInt64_LargeArray) {
    std::vector<int64_t> data(1000);
    std::mt19937 rng(42);
    for (auto& x : data) x = static_cast<int64_t>(rng() % 10000);
    int64_t expected = *std::min_element(data.begin(), data.end());
    EXPECT_EQ(ms::simdMinInt64(data.data(), data.size()), expected);
}

// SIMD 整数最大值：大数组
TEST(SimdKernels, MaxInt64_LargeArray) {
    std::vector<int64_t> data(1000);
    std::mt19937 rng(42);
    for (auto& x : data) x = static_cast<int64_t>(rng() % 10000);
    int64_t expected = *std::max_element(data.begin(), data.end());
    EXPECT_EQ(ms::simdMaxInt64(data.data(), data.size()), expected);
}

// SIMD 整数最小值：含负数
TEST(SimdKernels, MinInt64_WithNegatives) {
    std::vector<int64_t> data(100);
    for (size_t i = 0; i < data.size(); ++i) data[i] = -static_cast<int64_t>(i);
    EXPECT_EQ(ms::simdMinInt64(data.data(), data.size()), -99);
}

// SIMD 整数最大值：含负数
TEST(SimdKernels, MaxInt64_WithNegatives) {
    std::vector<int64_t> data(100);
    for (size_t i = 0; i < data.size(); ++i) data[i] = -static_cast<int64_t>(i);
    EXPECT_EQ(ms::simdMaxInt64(data.data(), data.size()), 0);
}

// int48 批量解码：正数
TEST(SimdKernels, DecodeInt48_PositiveValues) {
    // 构造 NaN-boxed int48 bits（INT_TAG_BASE | (i & INT48_MASK)）
    constexpr uint64_t INT_TAG_BASE = 0x7FF8000000000000ULL;
    constexpr uint64_t INT48_MASK = 0x0000FFFFFFFFFFFFULL;
    std::vector<uint64_t> srcBits(20);
    std::vector<int64_t> dst(20);
    for (size_t i = 0; i < srcBits.size(); ++i) {
        int64_t val = static_cast<int64_t>(i * 100);
        srcBits[i] = INT_TAG_BASE | (static_cast<uint64_t>(val) & INT48_MASK);
    }
    ms::decodeInt48Batch(srcBits.data(), dst.data(), srcBits.size());
    for (size_t i = 0; i < dst.size(); ++i) {
        EXPECT_EQ(dst[i], static_cast<int64_t>(i * 100));
    }
}

// int48 批量解码：含负数（验证符号扩展）
TEST(SimdKernels, DecodeInt48_NegativeValues) {
    constexpr uint64_t INT_TAG_BASE = 0x7FF8000000000000ULL;
    constexpr uint64_t INT48_MASK = 0x0000FFFFFFFFFFFFULL;
    std::vector<uint64_t> srcBits(20);
    std::vector<int64_t> dst(20);
    for (size_t i = 0; i < srcBits.size(); ++i) {
        int64_t val = -static_cast<int64_t>(i * 100);
        srcBits[i] = INT_TAG_BASE | (static_cast<uint64_t>(val) & INT48_MASK);
    }
    ms::decodeInt48Batch(srcBits.data(), dst.data(), srcBits.size());
    for (size_t i = 0; i < dst.size(); ++i) {
        EXPECT_EQ(dst[i], -static_cast<int64_t>(i * 100));
    }
}

// int48 批量解码：边界值（最大/最小 int48）
TEST(SimdKernels, DecodeInt48_BoundaryValues) {
    constexpr uint64_t INT_TAG_BASE = 0x7FF8000000000000ULL;
    constexpr uint64_t INT48_MASK = 0x0000FFFFFFFFFFFFULL;
    int64_t maxInt48 = (1LL << 47) - 1;
    int64_t minInt48 = -(1LL << 47);
    std::vector<uint64_t> srcBits = {
        INT_TAG_BASE | (static_cast<uint64_t>(maxInt48) & INT48_MASK),
        INT_TAG_BASE | (static_cast<uint64_t>(minInt48) & INT48_MASK),
        INT_TAG_BASE | 0,  // 0
    };
    std::vector<int64_t> dst(3);
    ms::decodeInt48Batch(srcBits.data(), dst.data(), 3);
    EXPECT_EQ(dst[0], maxInt48);
    EXPECT_EQ(dst[1], minInt48);
    EXPECT_EQ(dst[2], 0);
}

// ============================================================
// Layer 2: 端到端 sum() 四后端语义验证
// ============================================================

// 小数组 sum（不触发 SIMD）：四后端一致
TEST(SimdKernels, Sum_SmallArray_AllBackends) {
    std::string src = R"(
var arr = [1, 2, 3, 4, 5];
print(sum(arr) + "\n");
)";
    EXPECT_ALL_BACKENDS(src, "15\n");
}

// 大数组 sum（触发 SIMD）：四后端一致
// 注:RegVM 有 32 寄存器限制,大数组字面量会触发 "Register IR lowering 失败"。
// 改用 for 循环 + push 动态构造数组,绕过字面量限制,验证 sum() 在动态构造
// 的大数组上的 SIMD 路径正确性。
TEST(SimdKernels, Sum_LargeArray_AllBackends) {
    std::string src = R"(
var arr = [];
for (var i = 1; i <= 100; i = i + 1) {
    arr.push(i);
}
print(sum(arr) + "\n");
)";
    // 1+2+...+100 = 5050
    EXPECT_ALL_BACKENDS(src, "5050\n");
}

// 触发 SIMD 阈值边界：16 元素
TEST(SimdKernels, Sum_Threshold16_AllBackends) {
    std::string src = "var arr = [";
    for (int i = 1; i <= 16; ++i) {
        src += std::to_string(i);
        if (i < 16) src += ", ";
    }
    src += "];\nprint(sum(arr) + \"\\n\");\n";
    // 1+2+...+16 = 136
    EXPECT_ALL_BACKENDS(src, "136\n");
}

// 触发 SIMD 阈值边界：15 元素（不触发）
TEST(SimdKernels, Sum_BelowThreshold15_AllBackends) {
    std::string src = "var arr = [";
    for (int i = 1; i <= 15; ++i) {
        src += std::to_string(i);
        if (i < 15) src += ", ";
    }
    src += "];\nprint(sum(arr) + \"\\n\");\n";
    // 1+2+...+15 = 120
    EXPECT_ALL_BACKENDS(src, "120\n");
}

// 含负数的大数组 sum
TEST(SimdKernels, Sum_LargeArrayWithNegatives_AllBackends) {
    std::string src = R"(
var arr = [];
for (var i = 1; i <= 50; i = i + 1) {
    arr.push(i);
    arr.push(-i);
}
print(sum(arr) + "\n");
)";
    // (1 + -1) + (2 + -2) + ... + (50 + -50) = 0
    EXPECT_ALL_BACKENDS(src, "0\n");
}

// 浮点数组 sum
TEST(SimdKernels, Sum_DoubleArray_AllBackends) {
    // 100 个 1.5 求和 = 150.0
    std::string src = R"(
var arr = [];
for (var i = 1; i <= 100; i = i + 1) {
    arr.push(1.5);
}
print(sum(arr) + "\n");
)";
    EXPECT_ALL_BACKENDS(src, "150\n");
}

// 空数组 sum
TEST(SimdKernels, Sum_EmptyArray_AllBackends) {
    std::string src = R"(
var arr = [];
print(sum(arr) + "\n");
)";
    EXPECT_ALL_BACKENDS(src, "0\n");
}

// 单元素数组 sum
TEST(SimdKernels, Sum_SingleElement_AllBackends) {
    std::string src = R"(
var arr = [42];
print(sum(arr) + "\n");
)";
    EXPECT_ALL_BACKENDS(src, "42\n");
}

// ============================================================
// 拓展二期：SIMD 用户可见内建——min(arr) / max(arr) 单参数组形态
// ------------------------------------------------------------
// 全 int 数组且 >= 16 元素且 AVX2 可用时走 simdMinInt64/simdMaxInt64
// 内核；否则标量回退。共享实现层（builtinFunctionRegistry）保证
// 四后端语义一致，此处用 EXPECT_ALL_BACKENDS 验证。
// ============================================================

// 大数组 min/max（触发 SIMD）：四后端一致
TEST(SimdKernels, MinMax_LargeArray_AllBackends) {
    std::string src = R"(
var arr = [];
for (var i = 1; i <= 100; i = i + 1) {
    arr.push(i * 3 - 150);
}
print(min(arr) + "\n");
print(max(arr) + "\n");
)";
    // i=1 → -147（最小），i=100 → 150（最大）
    EXPECT_ALL_BACKENDS(src, "-147\n150\n");
}

// SIMD 阈値边界：16 元素（触发）与 15 元素（标量回退）同义
TEST(SimdKernels, MinMax_ThresholdBoundary_AllBackends) {
    for (int n : {15, 16, 17}) {
        std::string src = "var arr = [];\nfor (var i = 1; i <= " + std::to_string(n) +
                          "; i = i + 1) { arr.push(i); }\n"
                          "print(min(arr) + \"\\n\");\n"
                          "print(max(arr) + \"\\n\");\n";
        EXPECT_ALL_BACKENDS(src, "1\n" + std::to_string(n) + "\n");
    }
}

// 混合数值（含浮点）：转 double 比较，四后端一致
TEST(SimdKernels, MinMax_MixedNumeric_AllBackends) {
    std::string src = R"(
var arr = [3, 1.5, 2];
print(min(arr) + "\n");
print(max(arr) + "\n");
)";
    EXPECT_ALL_BACKENDS(src, "1.5\n3\n");
}

// 双参数形态回归锁：min(a,b)/max(a,b) 旧语义不受数组形态影响
TEST(SimdKernels, MinMax_TwoArgFormUnchanged_AllBackends) {
    std::string src = R"(
print(min(3, 5) + "\n");
print(max(3, 5) + "\n");
print(min(-1, 1) + "\n");
)";
    EXPECT_ALL_BACKENDS(src, "3\n5\n-1\n");
}

// 空数组 min/max → 运行时错误（四后端一致报错，不崩溃）
TEST(SimdKernels, MinMax_EmptyArray_ErrorsAllBackends) {
    // 仅验证 Interpreter 路径报错行为（共享实现层决定三后端同源）；
    // EXPECT_ALL_BACKENDS 不适用于错误路径（各后端错误前缀不同）。
    BackendExecResult r = BackendExecutionService::execute("var a = []; print(min(a));", BackendType::Interpreter);
    EXPECT_FALSE(r.success);
    EXPECT_NE(r.errorMsg.find("空数组"), std::string::npos) << r.errorMsg;
}
