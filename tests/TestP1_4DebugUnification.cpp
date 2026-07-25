// ============================================================
// TestP1_4DebugUnification — P1-4 调试系统统一回归测试
// ------------------------------------------------------------
// 验证 VmStepper 函数断点条件求值 + Watchpoint 条件求值的 API 行为，
// 对齐 DebugController::setFunctionBreakpointCondition /
// getFunctionBreakpointCondition 的语义。
//
// 覆盖点：
//   1. setFunctionBreakpointCondition/getFunctionBreakpointCondition 读写
//   2. 条件变更时重置 hitCount（对齐 DebugController L556-568）
//   3. 函数断点不存在时自动创建（对齐 setBreakpointCondition 行为）
//   4. setFunctionBreakpoints 批量替换保留已有 condition（P1-4 新增逻辑）
//   5. removeFunctionBreakpoint 同步清理 hitCount
//   6. setFunctionBreakpoint 重复调用不丢失已有 condition
//   7. WatchpointInfo.condition 字段存储（Watchpoint 条件求值的基础）
//
// 注：checkFunctionBreakpointHit/checkWatchpointHit 的私有条件求值逻辑
// 通过 R104/R161 集成测试间接覆盖（需要完整 VM 初始化 + 步进流程）。
// 本测试聚焦 API 层存储正确性。
// ============================================================

#include <gtest/gtest.h>

#include "app/VmStepper.h"
#include "debug/DebugTypes.h"

#include <QSet>
#include <string>

// ============================================================
// 第一组：FunctionBreakpointCondition API 读写
// ============================================================

TEST(P1_4FunctionBreakpointCondition, SetAndGet) {
    VmStepper stepper;
    stepper.setFunctionBreakpoint("foo");
    EXPECT_EQ(stepper.getFunctionBreakpointCondition("foo"), "");

    stepper.setFunctionBreakpointCondition("foo", "n > 10");
    EXPECT_EQ(stepper.getFunctionBreakpointCondition("foo"), "n > 10");
}

TEST(P1_4FunctionBreakpointCondition, AutoCreateWhenNotExists) {
    // 对齐 DebugController::setFunctionBreakpointCondition L562-567：
    // 函数断点不存在时自动创建（与 setBreakpointCondition 行为一致）
    VmStepper stepper;
    EXPECT_FALSE(stepper.hasFunctionBreakpoint("bar"));

    stepper.setFunctionBreakpointCondition("bar", "x == 0");
    EXPECT_TRUE(stepper.hasFunctionBreakpoint("bar"));
    EXPECT_EQ(stepper.getFunctionBreakpointCondition("bar"), "x == 0");
}

TEST(P1_4FunctionBreakpointCondition, SetFunctionBreakpointPreservesCondition) {
    // P1-4 fix: setFunctionBreakpoint 仅插入默认（无条件）条目，
    // 若已存在则保留原有 condition（对齐 DebugController::setBreakpoint 行为）
    VmStepper stepper;
    stepper.setFunctionBreakpointCondition("foo", "n > 10");
    stepper.setFunctionBreakpoint("foo"); // 重复调用不应丢失 condition
    EXPECT_EQ(stepper.getFunctionBreakpointCondition("foo"), "n > 10");
}

// ============================================================
// 第二组：setFunctionBreakpoints 批量替换保留 condition
// ============================================================

TEST(P1_4FunctionBreakpointCondition, BatchReplacePreservesCondition) {
    // P1-4 新增逻辑：setFunctionBreakpoints 保留已有 condition
    // 避免批量替换丢失用户设置的条件（与 DebugController 未实现批量 condition 保留
    // 的语义对齐——这里更安全）
    VmStepper stepper;
    stepper.setFunctionBreakpointCondition("foo", "n > 10");
    stepper.setFunctionBreakpointCondition("bar", "x == 0");

    QSet<std::string> batch;
    batch.insert("foo");
    batch.insert("baz");
    stepper.setFunctionBreakpoints(batch);

    // foo 保留 condition
    EXPECT_TRUE(stepper.hasFunctionBreakpoint("foo"));
    EXPECT_EQ(stepper.getFunctionBreakpointCondition("foo"), "n > 10");
    // bar 被移除
    EXPECT_FALSE(stepper.hasFunctionBreakpoint("bar"));
    // baz 新增（无条件）
    EXPECT_TRUE(stepper.hasFunctionBreakpoint("baz"));
    EXPECT_EQ(stepper.getFunctionBreakpointCondition("baz"), "");
    // 集合大小正确
    EXPECT_EQ(stepper.getFunctionBreakpoints().size(), 2u);
}

TEST(P1_4FunctionBreakpointCondition, BatchReplaceClearsRemovedHitCount) {
    // P1-4: setFunctionBreakpoints 清理被移除断点的 hitCount
    VmStepper stepper;
    stepper.setFunctionBreakpoint("foo");
    stepper.setFunctionBreakpoint("bar");
    // 通过 getFunctionBreakpointHitCount 验证初始为 0
    EXPECT_EQ(stepper.getFunctionBreakpointHitCount("foo"), 0);
    EXPECT_EQ(stepper.getFunctionBreakpointHitCount("bar"), 0);

    QSet<std::string> batch;
    batch.insert("foo");
    stepper.setFunctionBreakpoints(batch);

    EXPECT_TRUE(stepper.hasFunctionBreakpoint("foo"));
    EXPECT_FALSE(stepper.hasFunctionBreakpoint("bar"));
    // bar 被移除后 hitCount 查询返回 0（不存在）
    EXPECT_EQ(stepper.getFunctionBreakpointHitCount("bar"), 0);
}

// ============================================================
// 第三组：removeFunctionBreakpoint 清理 hitCount
// ============================================================

TEST(P1_4FunctionBreakpointCondition, RemoveClearsHitCount) {
    // P1-4: removeFunctionBreakpoint 同步清理 hitCount
    VmStepper stepper;
    stepper.setFunctionBreakpoint("foo");
    EXPECT_TRUE(stepper.hasFunctionBreakpoint("foo"));

    stepper.removeFunctionBreakpoint("foo");
    EXPECT_FALSE(stepper.hasFunctionBreakpoint("foo"));
    EXPECT_EQ(stepper.getFunctionBreakpointCondition("foo"), "");
    EXPECT_EQ(stepper.getFunctionBreakpointHitCount("foo"), 0);
}

TEST(P1_4FunctionBreakpointCondition, RemoveNonExistentIsNoOp) {
    VmStepper stepper;
    // 移除不存在的断点不应崩溃
    stepper.removeFunctionBreakpoint("nonexistent");
    EXPECT_FALSE(stepper.hasFunctionBreakpoint("nonexistent"));
}

// ============================================================
// 第四组：条件变更重置 hitCount（对齐 DebugController L561）
// ============================================================

TEST(P1_4FunctionBreakpointCondition, ConditionChangeResetsHitCount) {
    // P1-4: setFunctionBreakpointCondition 重置对应函数的 hitCount
    // 对齐 DebugController::setFunctionBreakpointCondition L561:
    //   it->hitCount = 0;
    VmStepper stepper;
    stepper.setFunctionBreakpointCondition("foo", "n > 10");
    // hitCount 初始为 0（无需手动设置）
    EXPECT_EQ(stepper.getFunctionBreakpointHitCount("foo"), 0);

    // 再次设置条件应重置 hitCount（即便为 0 也应清理 vmFunctionBreakpointHitCounts_）
    stepper.setFunctionBreakpointCondition("foo", "n > 20");
    EXPECT_EQ(stepper.getFunctionBreakpointCondition("foo"), "n > 20");
    EXPECT_EQ(stepper.getFunctionBreakpointHitCount("foo"), 0);
}

// ============================================================
// 第五组：getFunctionBreakpoints 返回所有断点名（含条件断点）
// ============================================================

TEST(P1_4FunctionBreakpointCondition, GetFunctionBreakpointsIncludesConditional) {
    VmStepper stepper;
    stepper.setFunctionBreakpoint("plain");
    stepper.setFunctionBreakpointCondition("conditional", "x > 0");

    QSet<std::string> bps = stepper.getFunctionBreakpoints();
    EXPECT_EQ(bps.size(), 2u);
    EXPECT_TRUE(bps.contains("plain"));
    EXPECT_TRUE(bps.contains("conditional"));
}

// ============================================================
// 第六组：Watchpoint 条件存储（条件求值的基础）
// ============================================================

TEST(P1_4WatchpointCondition, ConditionFieldStored) {
    // P1-4: WatchpointInfo.condition 字段已存在但 v1 未使用。
    // P1-4 修复后 checkWatchpointHit 会读取此字段调用 vmConditionEvaluator_。
    // 此测试验证 WatchpointInfo 能正确存储条件。
    WatchpointInfo wp;
    wp.varName = "counter";
    wp.condition = "counter > 5";
    EXPECT_TRUE(wp.isConditional());
    EXPECT_EQ(wp.condition, "counter > 5");
}

TEST(P1_4WatchpointCondition, EmptyConditionNotConditional) {
    WatchpointInfo wp;
    wp.varName = "counter";
    EXPECT_FALSE(wp.isConditional());
    EXPECT_EQ(wp.condition, "");
}

TEST(P1_4WatchpointCondition, SetWatchpointPreservesCondition) {
    // VmStepper::setWatchpoint 应原样存储 WatchpointInfo（含 condition）
    VmStepper stepper;
    WatchpointInfo wp;
    wp.varName = "counter";
    wp.condition = "counter > 5";
    stepper.setWatchpoint(wp);

    const auto& wps = stepper.getWatchpoints();
    ASSERT_EQ(wps.size(), 1u);
    EXPECT_EQ(wps[0].varName, "counter");
    EXPECT_EQ(wps[0].condition, "counter > 5");
    EXPECT_TRUE(wps[0].isConditional());
}

TEST(P1_4WatchpointCondition, FieldWatchpointWithCondition) {
    VmStepper stepper;
    WatchpointInfo wp;
    wp.kind = WatchpointTargetKind::Field;
    wp.varName = "obj";
    wp.fieldName = "value";
    wp.condition = "obj.value > 100";
    stepper.setWatchpoint(wp);

    const auto& wps = stepper.getWatchpoints();
    ASSERT_EQ(wps.size(), 1u);
    EXPECT_EQ(wps[0].kind, WatchpointTargetKind::Field);
    EXPECT_EQ(wps[0].varName, "obj");
    EXPECT_EQ(wps[0].fieldName, "value");
    EXPECT_EQ(wps[0].condition, "obj.value > 100");
}

// ============================================================
// 第七组：reset() 行为（保留断点配置，清理运行时状态）
// ============================================================

TEST(P1_4ResetBehavior, ResetPreservesBreakpointConfig) {
    // VmStepper::reset() 应保留断点配置（与 DebugController::reset 一致），
    // 仅清理运行时状态（hitCount）
    VmStepper stepper;
    stepper.setFunctionBreakpoint("foo");
    stepper.setFunctionBreakpointCondition("foo", "n > 10");

    stepper.reset();

    // 断点配置保留
    EXPECT_TRUE(stepper.hasFunctionBreakpoint("foo"));
    EXPECT_EQ(stepper.getFunctionBreakpointCondition("foo"), "n > 10");
    // hitCount 清理（初始为 0，验证不崩溃）
    EXPECT_EQ(stepper.getFunctionBreakpointHitCount("foo"), 0);
}

TEST(P1_4ResetBehavior, ResetPreservesWatchpointConfig) {
    VmStepper stepper;
    WatchpointInfo wp;
    wp.varName = "counter";
    wp.condition = "counter > 5";
    stepper.setWatchpoint(wp);

    stepper.reset();

    // Watchpoint 配置保留（VmStepper::reset 未清理 vmWatchpoints_）
    EXPECT_EQ(stepper.getWatchpoints().size(), 1u);
    EXPECT_EQ(stepper.getWatchpoints()[0].varName, "counter");
    EXPECT_EQ(stepper.getWatchpoints()[0].condition, "counter > 5");
}

// ============================================================
// 第八组：空字符串边界条件
// ============================================================

TEST(P1_4EdgeCases, EmptyFunctionNameRejected) {
    // setFunctionBreakpoint/setFunctionBreakpointCondition 应拒绝空函数名
    VmStepper stepper;
    stepper.setFunctionBreakpoint("");
    EXPECT_FALSE(stepper.hasFunctionBreakpoint(""));

    stepper.setFunctionBreakpointCondition("", "x > 0");
    EXPECT_FALSE(stepper.hasFunctionBreakpoint(""));
    EXPECT_EQ(stepper.getFunctionBreakpointCondition(""), "");
}

TEST(P1_4EdgeCases, EmptyConditionEquivalentToNoCondition) {
    // 设置空条件等价于无条件断点
    VmStepper stepper;
    stepper.setFunctionBreakpoint("foo");
    stepper.setFunctionBreakpointCondition("foo", "");
    EXPECT_EQ(stepper.getFunctionBreakpointCondition("foo"), "");
}
