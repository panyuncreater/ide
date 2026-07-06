// ============================================================
// TestMemoryAnimAudit.cpp — MemoryModelPanel 第 4 子页"实时动画"数据完整性审计
// ============================================================
// 审计对象：
//   - MemoryAnimLibrary::gcAnimPhases()        （4 阶段：mark / sweep / reset / idle）
//   - MemoryAnimLibrary::heapObjectTypes()     （6 种堆对象类型）
//
// 审计维度：
//   1. 数量符合预期（phases >= 4，types >= 6）
//   2. 关键字段非空（phase / description / color / 类型名 / 类型说明）
//   3. 关键 ID 齐全（4 个 phase ID + 6 个类型名）
//   4. ID / 类型名不重复
//   5. color 字段格式合法（# 开头的 hex 色值）
//   6. phase 顺序合理（mark 在 sweep 之前）
//
// 注：本测试文件不加入 CMakeLists.txt（任务限制），仅作为审计参考；
//     可通过手动 g++ 编译验证 MemoryAnimLibrary 数据完整性。
// ============================================================

#include <gtest/gtest.h>
#include "gui/MemoryModelPanel.h"

#include <set>
#include <string>
#include <regex>
#include <algorithm>

// ============================================================
// MemoryAnimLibrary::gcAnimPhases() — GC 动画 4 阶段
// ============================================================

TEST(MemoryAnimLibraryPhases, CountAtLeastFour) {
    const auto& phases = MemoryAnimLibrary::gcAnimPhases();
    // 设计为 4 阶段：mark / sweep / reset / idle
    EXPECT_GE(phases.size(), 4u);
}

TEST(MemoryAnimLibraryPhases, CriticalFieldsNonEmpty) {
    const auto& phases = MemoryAnimLibrary::gcAnimPhases();
    ASSERT_GE(phases.size(), 1u);
    for (const auto& p : phases) {
        EXPECT_FALSE(p.phase.empty())        << "phase 字段为空";
        EXPECT_FALSE(p.description.empty())   << "description 字段为空: " << p.phase;
        EXPECT_FALSE(p.color.empty())         << "color 字段为空: " << p.phase;
    }
}

TEST(MemoryAnimLibraryPhases, CriticalPhaseIdsPresent) {
    const auto& phases = MemoryAnimLibrary::gcAnimPhases();
    std::set<std::string> ids;
    for (const auto& p : phases) {
        ids.insert(p.phase);
    }
    // 关键 phase ID 必须齐全（mark / sweep / reset / idle）
    EXPECT_TRUE(ids.count("mark")  > 0) << "缺少 mark 阶段";
    EXPECT_TRUE(ids.count("sweep") > 0) << "缺少 sweep 阶段";
    EXPECT_TRUE(ids.count("reset") > 0) << "缺少 reset 阶段";
    EXPECT_TRUE(ids.count("idle")  > 0) << "缺少 idle 阶段";
}

TEST(MemoryAnimLibraryPhases, PhaseIdsUnique) {
    const auto& phases = MemoryAnimLibrary::gcAnimPhases();
    std::set<std::string> seen;
    for (const auto& p : phases) {
        auto [_, inserted] = seen.insert(p.phase);
        EXPECT_TRUE(inserted) << "phase ID 重复: " << p.phase;
    }
    EXPECT_EQ(seen.size(), phases.size());
}

TEST(MemoryAnimLibraryPhases, ColorFormatValidHex) {
    const auto& phases = MemoryAnimLibrary::gcAnimPhases();
    // color 字段应为 #RRGGBB 格式（7 字符，# 开头，后跟 6 位 hex）
    std::regex re("^#[0-9A-Fa-f]{6}$");
    for (const auto& p : phases) {
        EXPECT_TRUE(std::regex_match(p.color, re))
            << "color 格式不合法（应为 #RRGGBB）: phase=" << p.phase << " color=" << p.color;
    }
}

TEST(MemoryAnimLibraryPhases, MarkBeforeSweep) {
    const auto& phases = MemoryAnimLibrary::gcAnimPhases();
    // mark 阶段必须在 sweep 之前（mark-sweep 语义）
    int markIdx  = -1;
    int sweepIdx = -1;
    for (size_t i = 0; i < phases.size(); ++i) {
        if (phases[i].phase == "mark")  markIdx  = static_cast<int>(i);
        if (phases[i].phase == "sweep") sweepIdx = static_cast<int>(i);
    }
    ASSERT_GE(markIdx, 0)  << "未找到 mark 阶段";
    ASSERT_GE(sweepIdx, 0) << "未找到 sweep 阶段";
    EXPECT_LT(markIdx, sweepIdx) << "mark 阶段应在 sweep 阶段之前";
}

// ============================================================
// MemoryAnimLibrary::heapObjectTypes() — 6 种堆对象类型
// ============================================================

TEST(MemoryAnimLibraryTypes, CountAtLeastSix) {
    const auto& types = MemoryAnimLibrary::heapObjectTypes();
    // 设计为 6 种：ArrayData / DictData / InstanceData / StringData / ClosureData / BoundMethodData
    EXPECT_GE(types.size(), 6u);
}

TEST(MemoryAnimLibraryTypes, CriticalFieldsNonEmpty) {
    const auto& types = MemoryAnimLibrary::heapObjectTypes();
    ASSERT_GE(types.size(), 1u);
    for (const auto& kv : types) {
        EXPECT_FALSE(kv.first.empty())  << "类型名为空";
        EXPECT_FALSE(kv.second.empty()) << "类型说明为空: " << kv.first;
    }
}

TEST(MemoryAnimLibraryTypes, CriticalTypeNamesPresent) {
    const auto& types = MemoryAnimLibrary::heapObjectTypes();
    std::set<std::string> names;
    for (const auto& kv : types) {
        names.insert(kv.first);
    }
    // 关键类型名必须齐全
    EXPECT_TRUE(names.count("ArrayData")       > 0) << "缺少 ArrayData";
    EXPECT_TRUE(names.count("DictData")        > 0) << "缺少 DictData";
    EXPECT_TRUE(names.count("InstanceData")    > 0) << "缺少 InstanceData";
    EXPECT_TRUE(names.count("StringData")      > 0) << "缺少 StringData";
    EXPECT_TRUE(names.count("ClosureData")     > 0) << "缺少 ClosureData";
    EXPECT_TRUE(names.count("BoundMethodData") > 0) << "缺少 BoundMethodData";
}

TEST(MemoryAnimLibraryTypes, TypeNamesUnique) {
    const auto& types = MemoryAnimLibrary::heapObjectTypes();
    std::set<std::string> seen;
    for (const auto& kv : types) {
        auto [_, inserted] = seen.insert(kv.first);
        EXPECT_TRUE(inserted) << "类型名重复: " << kv.first;
    }
    EXPECT_EQ(seen.size(), types.size());
}

TEST(MemoryAnimLibraryTypes, DescriptionsMentionContainer) {
    const auto& types = MemoryAnimLibrary::heapObjectTypes();
    // 每种类型说明应包含"容器"或"数据"或"实例"或"闭包"或"方法"或"字符串"
    // 至少能反映该类型的语义角色
    for (const auto& kv : types) {
        bool meaningful = (kv.second.find("容器") != std::string::npos ||
                          kv.second.find("数据") != std::string::npos ||
                          kv.second.find("实例") != std::string::npos ||
                          kv.second.find("闭包") != std::string::npos ||
                          kv.second.find("方法") != std::string::npos ||
                          kv.second.find("字符串") != std::string::npos);
        EXPECT_TRUE(meaningful) << "类型说明缺少语义关键词: " << kv.first;
    }
}

// ============================================================
// 交叉验证：phases 与 types 数量与一致性
// ============================================================

TEST(MemoryAnimLibraryConsistency, PhasesAndTypesCoherent) {
    const auto& phases = MemoryAnimLibrary::gcAnimPhases();
    const auto& types  = MemoryAnimLibrary::heapObjectTypes();
    // 两者均应非空，且数量符合设计预期（phases=4, types=6）
    EXPECT_FALSE(phases.empty()) << "gcAnimPhases 为空";
    EXPECT_FALSE(types.empty())  << "heapObjectTypes 为空";
    EXPECT_EQ(phases.size(), 4u) << "GC 动画阶段数应为 4";
    EXPECT_EQ(types.size(), 6u)  << "堆对象类型数应为 6";
}
