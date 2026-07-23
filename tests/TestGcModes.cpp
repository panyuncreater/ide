// ============================================================
// tests/TestGcModes.cpp
// ------------------------------------------------------------
// R135 GC 替代引用计数：三模式回归测试
//
// 测试 GcManager 的三种模式：
//   1. RefCountOnly        - 纯引用计数，跳过 GC 注册
//   2. RefCountWithCycleGc - 默认模式，refCount + GC 回收循环
//   3. GcOnly              - GC 主导，sweep 直接 delete 不可达对象
//
// 关键不变量：
//   - 模式切换不破坏现有语义（默认模式 = 历史行为）
//   - RefCountOnly 模式：trackedCount() 始终为 0
//   - RefCountWithCycleGc 模式：循环引用容器可被回收
//   - GcOnly 模式：不可达对象被直接 delete，aliveSet_ 同步清理
//   - 三模式切换需在 reset() 后进行（避免运行中状态污染）
//
// 注：测试避免触发 COW ensureUnique 与 GcOnly 的潜在冲突，
// 仅使用简单数组操作验证 GC 行为。
// ============================================================
#include "common/ThreeBackends.h"
#include "interpreter/GcManager.h"
#include "interpreter/Value.h"

#include <gtest/gtest.h>
#include <string>
#include <vector>

// ============================================================
// 模式切换基础设施测试
// ============================================================

// 默认模式应为 RefCountWithCycleGc（向后兼容）
TEST(GcModes, DefaultModeIsRefCountWithCycleGc) {
    GcManager::instance().reset();
    EXPECT_EQ(GcManager::instance().gcMode(), GcMode::RefCountWithCycleGc);
}

// 切换到 RefCountOnly 模式
TEST(GcModes, SwitchToRefCountOnly) {
    GcManager::instance().reset();
    GcManager::instance().setGcMode(GcMode::RefCountOnly);
    EXPECT_EQ(GcManager::instance().gcMode(), GcMode::RefCountOnly);
    // 恢复默认
    GcManager::instance().setGcMode(GcMode::RefCountWithCycleGc);
}

// 切换到 GcOnly 模式
TEST(GcModes, SwitchToGcOnly) {
    GcManager::instance().reset();
    GcManager::instance().setGcMode(GcMode::GcOnly);
    EXPECT_EQ(GcManager::instance().gcMode(), GcMode::GcOnly);
    // 恢复默认
    GcManager::instance().setGcMode(GcMode::RefCountWithCycleGc);
}

// ============================================================
// RefCountOnly 模式行为测试
// ============================================================

// RefCountOnly 模式：容器构造不注册 GcManager
TEST(GcModes, RefCountOnly_NoRegistration) {
    GcManager::instance().reset();
    GcManager::instance().setGcMode(GcMode::RefCountOnly);
    size_t beforeCount = GcManager::instance().trackedCount();
    {
        // 构造一个数组容器
        Value arr(std::vector<Value>{Value(1), Value(2), Value(3)});
        // RefCountOnly 模式应跳过 registerTracked
        EXPECT_EQ(GcManager::instance().trackedCount(), beforeCount);
    }
    // 恢复默认
    GcManager::instance().setGcMode(GcMode::RefCountWithCycleGc);
}

// RefCountOnly 模式：collectCycle 是 noop
TEST(GcModes, RefCountOnly_CollectCycleIsNoop) {
    GcManager::instance().reset();
    GcManager::instance().setGcMode(GcMode::RefCountOnly);
    {
        Value arr(std::vector<Value>{Value(1), Value(2)});
        // collectCycle 应直接返回（无 tracked 对象）
        std::vector<const void*> roots = {arr.gcRootPtr()};
        GcManager::instance().collectCycle(roots);
        // 没有崩溃且 tracked 仍为 0
        EXPECT_EQ(GcManager::instance().trackedCount(), 0u);
        EXPECT_EQ(GcManager::instance().lastCollectedCount(), 0u);
    }
    GcManager::instance().setGcMode(GcMode::RefCountWithCycleGc);
}

// ============================================================
// RefCountWithCycleGc 模式行为测试（默认模式，验证未破坏）
// ============================================================

// 默认模式：容器构造注册 GcManager
TEST(GcModes, RefCountWithCycleGc_RegistersTracked) {
    GcManager::instance().reset();
    GcManager::instance().setGcMode(GcMode::RefCountWithCycleGc);
    size_t beforeCount = GcManager::instance().trackedCount();
    {
        Value arr(std::vector<Value>{Value(1), Value(2), Value(3)});
        // 默认模式应注册到 GcManager
        EXPECT_GT(GcManager::instance().trackedCount(), beforeCount);
    }
    // 离开作用域后 refCount 降至 0，对象析构，从 aliveSet_ 移除
    // 但 tracked_ 中的悬垂指针在下次 collectCycle 时才清理
}

// 默认模式：循环引用容器可被回收
TEST(GcModes, RefCountWithCycleGc_CycleCollection) {
    GcManager::instance().reset();
    GcManager::instance().setGcMode(GcMode::RefCountWithCycleGc);
    {
        // 构造循环引用：a = [a]
        Value a(std::vector<Value>{});
        a.arrayVal().push_back(a); // a 引用自身，形成循环
        // 此时 a 的 refCount = 2（变量 a + 数组元素 a）
    }
    // 离开作用域后，变量 a 释放（refCount--），但数组元素仍引用自身
    // refCount = 1，无法自然释放，需要 GC 回收
    // 调用 collectCycle(空 roots) 应回收这个循环孤岛
    size_t beforeTracked = GcManager::instance().trackedCount();
    std::vector<const void*> emptyRoots;
    GcManager::instance().collectCycle(emptyRoots);
    // 应该回收至少 1 个循环孤岛
    EXPECT_GT(GcManager::instance().lastCollectedCount(), 0u);
    // tracked 数应减少
    EXPECT_LT(GcManager::instance().trackedCount(), beforeTracked);
}

// ============================================================
// GcOnly 模式行为测试
// ============================================================

// GcOnly 模式：容器构造仍注册 GcManager（sweep 需要遍历）
TEST(GcModes, GcOnly_RegistersTracked) {
    GcManager::instance().reset();
    GcManager::instance().setGcMode(GcMode::GcOnly);
    size_t beforeCount = GcManager::instance().trackedCount();
    {
        Value arr(std::vector<Value>{Value(1), Value(2), Value(3)});
        // GcOnly 模式也需要注册（sweep 需要遍历 tracked_）
        EXPECT_GT(GcManager::instance().trackedCount(), beforeCount);
    }
    GcManager::instance().setGcMode(GcMode::RefCountWithCycleGc);
}

// GcOnly 模式：不可达对象被直接 delete
TEST(GcModes, GcOnly_DirectDeleteUnreachable) {
    GcManager::instance().reset();
    GcManager::instance().setGcMode(GcMode::GcOnly);
    {
        // 构造循环引用：a = [a]
        Value a(std::vector<Value>{});
        a.arrayVal().push_back(a);
    }
    // 离开作用域后，循环孤岛 refCount=1，等待 GC 回收
    size_t beforeTracked = GcManager::instance().trackedCount();
    ASSERT_GT(beforeTracked, 0u); // 应有未回收的循环孤岛
    std::vector<const void*> emptyRoots;
    GcManager::instance().collectCycle(emptyRoots);
    // GcOnly 模式应直接 delete 不可达对象
    EXPECT_GT(GcManager::instance().lastCollectedCount(), 0u);
    // tracked 应被清理（delete 触发 onDestroyed → aliveSet_.erase，
    // Phase 3 重建 tracked_ 时过滤掉悬垂指针）
    EXPECT_LT(GcManager::instance().trackedCount(), beforeTracked);
    GcManager::instance().setGcMode(GcMode::RefCountWithCycleGc);
}

// GcOnly 模式：可达对象不被 delete
TEST(GcModes, GcOnly_PreservesReachable) {
    GcManager::instance().reset();
    GcManager::instance().setGcMode(GcMode::GcOnly);
    {
        // 构造一个可达的数组
        Value arr(std::vector<Value>{Value(1), Value(2), Value(3)});
        const void* rootPtr = arr.gcRootPtr();
        ASSERT_NE(rootPtr, nullptr);
        // collectCycle 应保留 arr（作为 root 可达）
        std::vector<const void*> roots = {rootPtr};
        size_t beforeCount = GcManager::instance().trackedCount();
        GcManager::instance().collectCycle(roots);
        // 不应回收任何对象（arr 可达）
        EXPECT_EQ(GcManager::instance().lastCollectedCount(), 0u);
        // tracked 数应保持不变（arr 仍存活且可达）
        EXPECT_EQ(GcManager::instance().trackedCount(), beforeCount);
    }
    GcManager::instance().setGcMode(GcMode::RefCountWithCycleGc);
}

// ============================================================
// 三模式语义一致性测试
// ============================================================

// 三模式下简单数组操作结果一致（端到端验证）
TEST(GcModes, AllModes_ProduceSameResult_SimpleArray) {
    std::string src = R"(
var arr = [1, 2, 3, 4, 5];
var total = 0;
for (var i = 0; i < arr.len(); i = i + 1) {
    total = total + arr[i];
}
print(total + "\n");
)";
    // 默认模式
    GcManager::instance().reset();
    GcManager::instance().setGcMode(GcMode::RefCountWithCycleGc);
    std::string defaultResult = minilang_test::runInterpreter(src);

    // RefCountOnly 模式
    GcManager::instance().reset();
    GcManager::instance().setGcMode(GcMode::RefCountOnly);
    std::string refCountOnlyResult = minilang_test::runInterpreter(src);

    // GcOnly 模式
    GcManager::instance().reset();
    GcManager::instance().setGcMode(GcMode::GcOnly);
    std::string gcOnlyResult = minilang_test::runInterpreter(src);

    // 恢复默认
    GcManager::instance().reset();
    GcManager::instance().setGcMode(GcMode::RefCountWithCycleGc);

    EXPECT_EQ(defaultResult, "15\n");
    EXPECT_EQ(refCountOnlyResult, defaultResult);
    EXPECT_EQ(gcOnlyResult, defaultResult);
}

// 三模式下类实例操作结果一致
TEST(GcModes, AllModes_ProduceSameResult_ClassInstance) {
    std::string src = R"(
class Box {
    var value = 0;
    fun init(v) { this.value = v; }
    fun get() { return this.value; }
}
var b = Box(42);
print(b.get() + "\n");
)";
    GcManager::instance().reset();
    GcManager::instance().setGcMode(GcMode::RefCountWithCycleGc);
    std::string defaultResult = minilang_test::runInterpreter(src);

    GcManager::instance().reset();
    GcManager::instance().setGcMode(GcMode::RefCountOnly);
    std::string refCountOnlyResult = minilang_test::runInterpreter(src);

    GcManager::instance().reset();
    GcManager::instance().setGcMode(GcMode::GcOnly);
    std::string gcOnlyResult = minilang_test::runInterpreter(src);

    GcManager::instance().reset();
    GcManager::instance().setGcMode(GcMode::RefCountWithCycleGc);

    EXPECT_EQ(defaultResult, "42\n");
    EXPECT_EQ(refCountOnlyResult, defaultResult);
    EXPECT_EQ(gcOnlyResult, defaultResult);
}

// GC 统计 API 在三模式下都可用
TEST(GcModes, AllModes_GcStatsAvailable) {
    GcManager::instance().reset();
    GcManager::instance().setGcMode(GcMode::RefCountWithCycleGc);
    EXPECT_EQ(GcManager::instance().currentPhase(), GcPhase::Idle);
    EXPECT_EQ(GcManager::instance().totalGcCount(), 0u);

    GcManager::instance().setGcMode(GcMode::RefCountOnly);
    EXPECT_EQ(GcManager::instance().currentPhase(), GcPhase::Idle);

    GcManager::instance().setGcMode(GcMode::GcOnly);
    EXPECT_EQ(GcManager::instance().currentPhase(), GcPhase::Idle);

    GcManager::instance().setGcMode(GcMode::RefCountWithCycleGc);
}
