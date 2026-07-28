// ============================================================
// MemoryInspectionAPI 单元测试（ARCH-10）
// ------------------------------------------------------------
// 验证内存检视只读快照 API 的契约正确性：
//   1. NaN-box 编码/解码往返一致性
//   2. canEncodeInt 边界判定（int48 范围）
//   3. inspectValue 对各 Value 类型的快照字段
//   4. inspectHeap 对非指针类型的行为
//   5. getGcStats 默认快照与模式切换
//   6. resetGc 重置行为
//   7. gcPhaseToString / gcModeToString 字符串转换
//   8. setGcAllocationThreshold + setGcTriggerCallback 触发验证
// ============================================================

#include <gtest/gtest.h>
#include "common/MemoryInspectionAPI.h"
#include "interpreter/Value.h"
#include "interpreter/Interpreter.h"

#include <atomic>
#include <cmath>
#include <string>
#include <vector>

// ============================================================
// 1. NaN-box 编码/解码往返一致性
// ============================================================

// encodeInt → decodeInt 往返：编码后解码应还原原始整数值
TEST(MemoryInspectionAPITest, EncodeDecodeInt_RoundTrip) {
    int64_t values[] = {0, 1, -1, 42, -42, 1000000, -1000000,
                        (1LL << 47) - 1, -(1LL << 47)};
    for (int64_t v : values) {
        ASSERT_TRUE(MemoryInspectionAPI::canEncodeInt(v)) << "value=" << v;
        uint64_t bits = MemoryInspectionAPI::encodeInt(v);
        int64_t decoded = MemoryInspectionAPI::decodeInt(bits);
        EXPECT_EQ(decoded, v) << "round-trip failed for value=" << v;
    }
}

// encodeFloat → decodeFloat 往返：编码后解码应还原原始浮点值
TEST(MemoryInspectionAPITest, EncodeDecodeFloat_RoundTrip) {
    double values[] = {0.0, 1.0, -1.0, 3.14159, -2.71828, 1e100, -1e-100};
    for (double v : values) {
        uint64_t bits = MemoryInspectionAPI::encodeFloat(v);
        double decoded = MemoryInspectionAPI::decodeFloat(bits);
        EXPECT_DOUBLE_EQ(decoded, v) << "round-trip failed for value=" << v;
    }
}

// encodeBool → inspectBits：tagName 应为 "BOOL"
TEST(MemoryInspectionAPITest, EncodeBool_InspectBitsTagName) {
    uint64_t bitsTrue = MemoryInspectionAPI::encodeBool(true);
    NaNBoxSnapshot snapTrue = MemoryInspectionAPI::inspectBits(bitsTrue);
    EXPECT_EQ(snapTrue.tagName, "BOOL");

    uint64_t bitsFalse = MemoryInspectionAPI::encodeBool(false);
    NaNBoxSnapshot snapFalse = MemoryInspectionAPI::inspectBits(bitsFalse);
    EXPECT_EQ(snapFalse.tagName, "BOOL");
}

// encodeNull → inspectBits：tagName 应为 "NUL"
TEST(MemoryInspectionAPITest, EncodeNull_InspectBitsTagName) {
    uint64_t bits = MemoryInspectionAPI::encodeNull();
    NaNBoxSnapshot snap = MemoryInspectionAPI::inspectBits(bits);
    EXPECT_EQ(snap.tagName, "NUL");
}

// encodeInt → inspectBits：tagName 应为 "INT"
TEST(MemoryInspectionAPITest, EncodeInt_InspectBitsTagName) {
    uint64_t bits = MemoryInspectionAPI::encodeInt(42);
    NaNBoxSnapshot snap = MemoryInspectionAPI::inspectBits(bits);
    EXPECT_EQ(snap.tagName, "INT");
}

// encodeFloat → inspectBits：tagName 应为 "FLOAT"
TEST(MemoryInspectionAPITest, EncodeFloat_InspectBitsTagName) {
    uint64_t bits = MemoryInspectionAPI::encodeFloat(3.14);
    NaNBoxSnapshot snap = MemoryInspectionAPI::inspectBits(bits);
    EXPECT_EQ(snap.tagName, "FLOAT");
}

// ============================================================
// 2. canEncodeInt 边界判定（int48 范围：|v| <= 2^47-1 或 v == -(2^47)）
// ============================================================

TEST(MemoryInspectionAPITest, CanEncodeInt_Boundary) {
    // 常见小值：可编码
    EXPECT_TRUE(MemoryInspectionAPI::canEncodeInt(0));
    EXPECT_TRUE(MemoryInspectionAPI::canEncodeInt(1));
    EXPECT_TRUE(MemoryInspectionAPI::canEncodeInt(-1));

    // int48 最大值：2^47 - 1（可编码）
    EXPECT_TRUE(MemoryInspectionAPI::canEncodeInt((1LL << 47) - 1));

    // 2^47：超出 int48 正数范围（不可编码）
    EXPECT_FALSE(MemoryInspectionAPI::canEncodeInt(1LL << 47));

    // int48 最小值：-(2^47)（可编码，补码对称性）
    EXPECT_TRUE(MemoryInspectionAPI::canEncodeInt(-(1LL << 47)));

    // -(2^47) - 1：超出 int48 负数范围（不可编码）
    EXPECT_FALSE(MemoryInspectionAPI::canEncodeInt(-(1LL << 47) - 1));
}

// ============================================================
// 3. inspectValue 对各 Value 类型的快照字段验证
// ============================================================

// Value(int64_t) → inspectValue：tagName="INT"，bitsHex/bitsBinary 格式正确
TEST(MemoryInspectionAPITest, InspectValue_Int) {
    Value v(int64_t(42));
    NaNBoxSnapshot snap = MemoryInspectionAPI::inspectValue(v);

    // tagName 应为 "INT"
    EXPECT_EQ(snap.tagName, "INT");

    // bitsHex 格式："0x" + 16 位十六进制字符
    ASSERT_GE(snap.bitsHex.size(), 18u);
    EXPECT_EQ(snap.bitsHex.substr(0, 2), "0x");
    EXPECT_EQ(snap.bitsHex.size(), 18u); // "0x" + 16 hex chars

    // bitsBinary 长度应为 64（64 位二进制字符串）
    EXPECT_EQ(snap.bitsBinary.size(), 64u);

    // bits 字段应与 encodeInt(42) 一致
    EXPECT_EQ(snap.bits, MemoryInspectionAPI::encodeInt(42));
}

// Value(double) → inspectValue：tagName="FLOAT"
TEST(MemoryInspectionAPITest, InspectValue_Float) {
    Value v(3.14);
    NaNBoxSnapshot snap = MemoryInspectionAPI::inspectValue(v);

    EXPECT_EQ(snap.tagName, "FLOAT");
    EXPECT_EQ(snap.bitsHex.substr(0, 2), "0x");
    EXPECT_EQ(snap.bitsHex.size(), 18u);
    EXPECT_EQ(snap.bitsBinary.size(), 64u);
}

// Value(bool) → inspectValue：tagName="BOOL"
TEST(MemoryInspectionAPITest, InspectValue_Bool) {
    Value vTrue(true);
    NaNBoxSnapshot snapTrue = MemoryInspectionAPI::inspectValue(vTrue);
    EXPECT_EQ(snapTrue.tagName, "BOOL");

    Value vFalse(false);
    NaNBoxSnapshot snapFalse = MemoryInspectionAPI::inspectValue(vFalse);
    EXPECT_EQ(snapFalse.tagName, "BOOL");
}

// Value() (null) → inspectValue：tagName="NUL"
TEST(MemoryInspectionAPITest, InspectValue_Null) {
    Value v;
    NaNBoxSnapshot snap = MemoryInspectionAPI::inspectValue(v);
    EXPECT_EQ(snap.tagName, "NUL");
    EXPECT_EQ(snap.bitsHex.substr(0, 2), "0x");
    EXPECT_EQ(snap.bitsBinary.size(), 64u);
}

// bitsBinary 仅包含 '0' 和 '1' 字符
TEST(MemoryInspectionAPITest, InspectValue_BitsBinaryOnlyContainsBinaryDigits) {
    Value v(int64_t(12345));
    NaNBoxSnapshot snap = MemoryInspectionAPI::inspectValue(v);
    for (char c : snap.bitsBinary) {
        EXPECT_TRUE(c == '0' || c == '1') << "unexpected char in bitsBinary: " << c;
    }
}

// ============================================================
// 4. inspectHeap 对非指针类型的行为
// ============================================================

// 标量类型（int/float/bool/null）不是堆指针
TEST(MemoryInspectionAPITest, InspectHeap_NonPointerReturnsFalse) {
    // int
    Value vInt(int64_t(99));
    HeapObjectSnapshot snapInt = MemoryInspectionAPI::inspectHeap(vInt);
    EXPECT_FALSE(snapInt.isPointer);

    // float
    Value vFloat(2.718);
    HeapObjectSnapshot snapFloat = MemoryInspectionAPI::inspectHeap(vFloat);
    EXPECT_FALSE(snapFloat.isPointer);

    // bool
    Value vBool(true);
    HeapObjectSnapshot snapBool = MemoryInspectionAPI::inspectHeap(vBool);
    EXPECT_FALSE(snapBool.isPointer);

    // null
    Value vNull;
    HeapObjectSnapshot snapNull = MemoryInspectionAPI::inspectHeap(vNull);
    EXPECT_FALSE(snapNull.isPointer);
}

// ============================================================
// 5. getGcStats 默认快照与模式切换
// ============================================================

// 默认 GC 统计：mode=RefCountWithCycleGc，phase=Idle
TEST(MemoryInspectionAPITest, GetGcStats_DefaultSnapshot) {
    MemoryInspectionAPI::resetGc();
    GcStatsSnapshot stats = MemoryInspectionAPI::getGcStats();

    EXPECT_EQ(stats.mode, GcMode::RefCountWithCycleGc);
    EXPECT_EQ(stats.phase, GcPhase::Idle);
}

// setGcMode + getGcStats 往返：切换模式后统计应反映新模式
TEST(MemoryInspectionAPITest, SetGcMode_GetGcStats_RoundTrip) {
    MemoryInspectionAPI::resetGc();

    // 切换到 RefCountOnly
    MemoryInspectionAPI::setGcMode(GcMode::RefCountOnly);
    GcStatsSnapshot stats1 = MemoryInspectionAPI::getGcStats();
    EXPECT_EQ(stats1.mode, GcMode::RefCountOnly);

    // 切换到 GcOnly
    MemoryInspectionAPI::setGcMode(GcMode::GcOnly);
    GcStatsSnapshot stats2 = MemoryInspectionAPI::getGcStats();
    EXPECT_EQ(stats2.mode, GcMode::GcOnly);

    // 恢复默认
    MemoryInspectionAPI::setGcMode(GcMode::RefCountWithCycleGc);
    GcStatsSnapshot stats3 = MemoryInspectionAPI::getGcStats();
    EXPECT_EQ(stats3.mode, GcMode::RefCountWithCycleGc);
}

// ============================================================
// 6. resetGc 重置行为
// ============================================================

// resetGc 后 trackedCount 应为 0
TEST(MemoryInspectionAPITest, ResetGc_TrackedCountZero) {
    // 先分配一些容器使 trackedCount > 0
    {
        Value arr(std::vector<Value>{Value(int64_t(1)), Value(int64_t(2)), Value(int64_t(3))});
    }
    // 重置 GC
    MemoryInspectionAPI::resetGc();
    GcStatsSnapshot stats = MemoryInspectionAPI::getGcStats();
    EXPECT_EQ(stats.trackedCount, 0u);
}

// ============================================================
// 7. gcPhaseToString / gcModeToString 字符串转换
// ============================================================

TEST(MemoryInspectionAPITest, GcPhaseToString_AllPhases) {
    EXPECT_EQ(gcPhaseToString(GcPhase::Idle), "Idle");
    EXPECT_EQ(gcPhaseToString(GcPhase::Marking), "Marking");
    EXPECT_EQ(gcPhaseToString(GcPhase::Sweeping), "Sweeping");
    EXPECT_EQ(gcPhaseToString(GcPhase::Finalizing), "Finalizing");
}

TEST(MemoryInspectionAPITest, GcModeToString_AllModes) {
    EXPECT_EQ(gcModeToString(GcMode::RefCountOnly), "RefCountOnly");
    EXPECT_EQ(gcModeToString(GcMode::RefCountWithCycleGc), "RefCountWithCycleGc");
    EXPECT_EQ(gcModeToString(GcMode::GcOnly), "GcOnly");
}

// ============================================================
// 8. setGcAllocationThreshold + setGcTriggerCallback 触发验证
// ============================================================

// 验证 setGcAllocationThreshold + setGcTriggerCallback API 可调用且不崩溃。
// 注意：实际触发时机取决于 Interpreter 内部分配路径（GcManager::notifyAllocation），
// 此处仅验证 API 契约（设置/清除不崩溃），不做触发断言。
// 不在回调注册期间执行 Interpreter 代码——覆盖 Interpreter 内部注册的回调
// 会导致 GC 路径死锁（Interpreter 期望回调内部执行特定逻辑）。
TEST(MemoryInspectionAPITest, AllocationThreshold_APISetAndClear) {
    MemoryInspectionAPI::resetGc();
    MemoryInspectionAPI::setGcMode(GcMode::RefCountWithCycleGc);

    // 注册回调 + 设置阈值——不应崩溃
    bool callbackFired = false;
    MemoryInspectionAPI::setGcTriggerCallback([&callbackFired]() {
        callbackFired = true;
    });
    MemoryInspectionAPI::setGcAllocationThreshold(1);

    // 验证 getGcStats 反映阈值设置
    auto stats = MemoryInspectionAPI::getGcStats();
    EXPECT_EQ(stats.allocationThreshold, 1u);

    // 清理：恢复默认阈值和回调——不应崩溃
    MemoryInspectionAPI::setGcAllocationThreshold(0);
    MemoryInspectionAPI::setGcTriggerCallback(nullptr);
    MemoryInspectionAPI::resetGc();
    MemoryInspectionAPI::setGcMode(GcMode::RefCountWithCycleGc);
}

// 阈值设为极大值时，少量分配不应触发回调
TEST(MemoryInspectionAPITest, AllocationThreshold_HighThreshold_NoTrigger) {
    MemoryInspectionAPI::resetGc();
    MemoryInspectionAPI::setGcMode(GcMode::RefCountWithCycleGc);

    std::atomic<bool> callbackFired{false};
    MemoryInspectionAPI::setGcTriggerCallback([&callbackFired]() {
        callbackFired.store(true);
    });

    // 设置极大阈值：少量分配不应触发
    MemoryInspectionAPI::setGcAllocationThreshold(999999);

    {
        Value arr(std::vector<Value>{Value(int64_t(1)), Value(int64_t(2))});
    }

    // 不应触发
    EXPECT_FALSE(callbackFired.load());

    // 清理
    MemoryInspectionAPI::setGcAllocationThreshold(0);
    MemoryInspectionAPI::setGcTriggerCallback(nullptr);
    MemoryInspectionAPI::resetGc();
    MemoryInspectionAPI::setGcMode(GcMode::RefCountWithCycleGc);
}
