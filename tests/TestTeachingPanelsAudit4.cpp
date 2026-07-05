// ============================================================
// TestTeachingPanelsAudit4.cpp — 第二档/第四档教学面板数据完整性审计
// ============================================================
// 覆盖：
//   - OpCodeProfileLibrary（P1-1 ProfileDashboardPanel 真实 instrumentation 配套）
//   - BreakpointConditionLibrary（P1-2 BreakpointConditionPanel 教学场景库）
//
// 审计模式：数据完整性 + 关键字段非空 + 分类合法 + 关键 ID 齐全
// ============================================================

#include <gtest/gtest.h>
#include "gui/ProfileDashboardPanel.h"
#include "gui/BreakpointConditionPanel.h"

#include <set>
#include <string>

// ============================================================
// OpCodeProfileLibrary 审计（P1-1 配套）
// ============================================================

TEST(TeachingPanelsOpCodeProfile, DocsCount) {
    const auto& docs = OpCodeProfileLibrary::docs();
    // 至少 10 条 OpCode 性能文档
    EXPECT_GE(docs.size(), 10u);
}

TEST(TeachingPanelsOpCodeProfile, OpCodeNamesUnique) {
    const auto& docs = OpCodeProfileLibrary::docs();
    std::set<std::string> seen;
    for (const auto& d : docs) {
        EXPECT_TRUE(seen.insert(d.opCode).second)
            << "Duplicate OpCode: " << d.opCode;
    }
}

TEST(TeachingPanelsOpCodeProfile, CriticalFieldsNonEmpty) {
    const auto& docs = OpCodeProfileLibrary::docs();
    for (const auto& d : docs) {
        EXPECT_FALSE(d.opCode.empty()) << "opCode 为空";
        EXPECT_FALSE(d.category.empty()) << "category 为空: " << d.opCode;
        EXPECT_FALSE(d.perfNote.empty()) << "perfNote 为空: " << d.opCode;
        EXPECT_FALSE(d.exampleCode.empty()) << "exampleCode 为空: " << d.opCode;
    }
}

TEST(TeachingPanelsOpCodeProfile, CategoriesValid) {
    const auto& docs = OpCodeProfileLibrary::docs();
    std::set<std::string> validCats = {
        "constant", "arith", "compare", "var", "call", "control", "container"
    };
    for (const auto& d : docs) {
        EXPECT_TRUE(validCats.count(d.category) > 0)
            << "Invalid category '" << d.category << "' for OpCode " << d.opCode;
    }
}

TEST(TeachingPanelsOpCodeProfile, CriticalOpCodesPresent) {
    const auto& docs = OpCodeProfileLibrary::docs();
    std::set<std::string> ids;
    for (const auto& d : docs) ids.insert(d.opCode);
    // 关键 OpCode 必须有性能文档
    EXPECT_TRUE(ids.count("OP_ADD") > 0) << "缺少 OP_ADD 文档";
    EXPECT_TRUE(ids.count("OP_CALL") > 0) << "缺少 OP_CALL 文档";
    EXPECT_TRUE(ids.count("OP_RETURN") > 0) << "缺少 OP_RETURN 文档";
    EXPECT_TRUE(ids.count("OP_GET_LOCAL") > 0) << "缺少 OP_GET_LOCAL 文档";
    EXPECT_TRUE(ids.count("OP_CLOSURE") > 0) << "缺少 OP_CLOSURE 文档";
    EXPECT_TRUE(ids.count("OP_METHOD_CALL") > 0) << "缺少 OP_METHOD_CALL 文档";
}

TEST(TeachingPanelsOpCodeProfile, PerfNoteContainsPerformanceHint) {
    const auto& docs = OpCodeProfileLibrary::docs();
    for (const auto& d : docs) {
        // 性能文档应包含至少一个性能相关关键词
        // 覆盖：热点 / 开销 / 快 / 慢 / 性能 / 分配 / 频繁 / 触发 / 代价 / 密度 / 压力
        bool hasKeyword = d.perfNote.find("热点") != std::string::npos ||
                          d.perfNote.find("开销") != std::string::npos ||
                          d.perfNote.find("快") != std::string::npos ||
                          d.perfNote.find("慢") != std::string::npos ||
                          d.perfNote.find("性能") != std::string::npos ||
                          d.perfNote.find("分配") != std::string::npos ||
                          d.perfNote.find("频繁") != std::string::npos ||
                          d.perfNote.find("触发") != std::string::npos ||
                          d.perfNote.find("代价") != std::string::npos ||
                          d.perfNote.find("密度") != std::string::npos ||
                          d.perfNote.find("压力") != std::string::npos;
        EXPECT_TRUE(hasKeyword) << "perfNote 缺少性能关键词: " << d.opCode;
    }
}

// ============================================================
// BreakpointConditionLibrary 审计（P1-2）
// ============================================================

TEST(TeachingPanelsBreakpointCondition, ScenariosCount) {
    const auto& scenarios = BreakpointConditionLibrary::scenarios();
    // 至少 6 个场景
    EXPECT_GE(scenarios.size(), 6u);
}

TEST(TeachingPanelsBreakpointCondition, ScenarioIdsUnique) {
    const auto& scenarios = BreakpointConditionLibrary::scenarios();
    std::set<std::string> seen;
    for (const auto& s : scenarios) {
        EXPECT_TRUE(seen.insert(s.id).second)
            << "Duplicate scenario ID: " << s.id;
    }
}

TEST(TeachingPanelsBreakpointCondition, CriticalFieldsNonEmpty) {
    const auto& scenarios = BreakpointConditionLibrary::scenarios();
    for (const auto& s : scenarios) {
        EXPECT_FALSE(s.id.empty()) << "id 为空";
        EXPECT_FALSE(s.title.empty()) << "title 为空: " << s.id;
        EXPECT_FALSE(s.condition.empty()) << "condition 为空: " << s.id;
        EXPECT_FALSE(s.description.empty()) << "description 为空: " << s.id;
        EXPECT_FALSE(s.expectedBehavior.empty()) << "expectedBehavior 为空: " << s.id;
        EXPECT_FALSE(s.sampleCode.empty()) << "sampleCode 为空: " << s.id;
    }
}

TEST(TeachingPanelsBreakpointCondition, CriticalScenarioIdsPresent) {
    const auto& scenarios = BreakpointConditionLibrary::scenarios();
    std::set<std::string> ids;
    for (const auto& s : scenarios) ids.insert(s.id);
    // 关键场景必须存在
    EXPECT_TRUE(ids.count("simple-equality") > 0) << "缺少 simple-equality 场景";
    EXPECT_TRUE(ids.count("null-check") > 0) << "缺少 null-check 场景";
    EXPECT_TRUE(ids.count("compound-condition") > 0) << "缺少 compound-condition 场景";
    EXPECT_TRUE(ids.count("method-call-condition") > 0) << "缺少 method-call-condition 场景";
}

TEST(TeachingPanelsBreakpointCondition, ConditionsAreExpressions) {
    const auto& scenarios = BreakpointConditionLibrary::scenarios();
    for (const auto& s : scenarios) {
        // 条件表达式应包含至少一个比较运算符或逻辑运算符
        bool hasOperator = s.condition.find("==") != std::string::npos ||
                           s.condition.find("!=") != std::string::npos ||
                           s.condition.find("<") != std::string::npos ||
                           s.condition.find(">") != std::string::npos ||
                           s.condition.find("&&") != std::string::npos ||
                           s.condition.find("||") != std::string::npos;
        EXPECT_TRUE(hasOperator) << "condition 缺少比较/逻辑运算符: " << s.id
                                  << " (condition=" << s.condition << ")";
    }
}

TEST(TeachingPanelsBreakpointCondition, SampleCodeHasBreakpointLine) {
    const auto& scenarios = BreakpointConditionLibrary::scenarios();
    for (const auto& s : scenarios) {
        // 示例代码应至少包含一个 while 循环或 if 分支（断点通常设在循环/分支内）
        bool hasBreakpointTarget = s.sampleCode.find("while") != std::string::npos ||
                                     s.sampleCode.find("if") != std::string::npos ||
                                     s.sampleCode.find("catch") != std::string::npos;
        EXPECT_TRUE(hasBreakpointTarget) << "sampleCode 缺少可设断点的语句: " << s.id;
    }
}

TEST(TeachingPanelsBreakpointCondition, DescriptionMentionsConditionSemantics) {
    const auto& scenarios = BreakpointConditionLibrary::scenarios();
    for (const auto& s : scenarios) {
        // description 应至少提到以下语义之一：求值 / truthy / 短路 / 沙箱 / 暂停 / 条件
        bool hasSemantics = s.description.find("求值") != std::string::npos ||
                            s.description.find("truthy") != std::string::npos ||
                            s.description.find("短路") != std::string::npos ||
                            s.description.find("沙箱") != std::string::npos ||
                            s.description.find("暂停") != std::string::npos ||
                            s.description.find("条件") != std::string::npos ||
                            s.description.find("求值") != std::string::npos;
        EXPECT_TRUE(hasSemantics) << "description 缺少条件断点语义说明: " << s.id;
    }
}
