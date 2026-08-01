// ============================================================
// TestWatchPanel.cpp — Watch 表达式面板测试（R117 调试器拓展）
// ------------------------------------------------------------
// 测试范围：
//   1. WatchExpressionLibrary 静态场景库完整性（6 个场景 + 字段非空 + ID 唯一）
//   2. PanelCatalog 中 "watch-expressions" 面板注册正确性
//   3. 场景库内容覆盖关键求值模式（标量/算术/索引/字段/方法/闭包）
//
// 测试框架：GoogleTest
// 依赖约束：仅链接 WatchExpressionLibrary.cpp + PanelCatalog.cpp，
// 不依赖 IdeController.cpp（evaluateWatchExpression 为非 inline 方法，
// 测试目标不链接 IdeController.cpp，故仅测数据完整性，不测求值行为）
// ============================================================

#include <gtest/gtest.h>

#include "gui/PanelCatalog.h"
#include "gui/WatchPanel.h"

#include <set>
#include <string>

// ============================================================
// 测试套件 1：WatchExpressionLibrary — 场景库完整性（6 用例）
// ============================================================

TEST(WatchExpressionLibrary, ScenariosNonEmpty) {
    const auto& scenarios = WatchExpressionLibrary::scenarios();
    EXPECT_FALSE(scenarios.empty()) << "Watch 场景库不应为空";
    EXPECT_GE(scenarios.size(), 6u) << "应至少有 6 个 watch 场景";
}

TEST(WatchExpressionLibrary, ScenariosFieldsNonEmpty) {
    const auto& scenarios = WatchExpressionLibrary::scenarios();
    for (const auto& s : scenarios) {
        EXPECT_FALSE(s.id.empty()) << "场景 id 不能为空";
        EXPECT_FALSE(s.title.empty()) << "场景 title 不能为空";
        EXPECT_FALSE(s.expression.empty()) << "场景 expression 不能为空";
        EXPECT_FALSE(s.description.empty()) << "场景 description 不能为空";
        EXPECT_FALSE(s.sampleCode.empty()) << "场景 sampleCode 不能为空";
        EXPECT_FALSE(s.expectedType.empty()) << "场景 expectedType 不能为空";
    }
}

TEST(WatchExpressionLibrary, ScenarioIdsAreUnique) {
    const auto& scenarios = WatchExpressionLibrary::scenarios();
    std::set<std::string> ids;
    for (const auto& s : scenarios) {
        auto [_, inserted] = ids.insert(s.id);
        EXPECT_TRUE(inserted) << "场景 id 重复: " << s.id;
    }
    EXPECT_EQ(ids.size(), scenarios.size());
}

TEST(WatchExpressionLibrary, CoversKeyEvaluationPatterns) {
    // 验证场景库覆盖关键求值模式：标量/算术/索引/字段/方法/闭包
    const auto& scenarios = WatchExpressionLibrary::scenarios();
    std::set<std::string> ids;
    for (const auto& s : scenarios) {
        ids.insert(s.id);
    }

    EXPECT_NE(ids.find("simple-var"), ids.end()) << "缺少简单变量场景";
    EXPECT_NE(ids.find("arithmetic-expr"), ids.end()) << "缺少算术表达式场景";
    EXPECT_NE(ids.find("array-index"), ids.end()) << "缺少数组索引访问场景";
    EXPECT_NE(ids.find("instance-field"), ids.end()) << "缺少实例字段访问场景";
    EXPECT_NE(ids.find("method-call"), ids.end()) << "缺少方法调用场景";
    EXPECT_NE(ids.find("closure-capture"), ids.end()) << "缺少闭包捕获场景";
}

TEST(WatchExpressionLibrary, ScenarioExpressionsAreDistinct) {
    // 验证每个场景的表达式互不相同（避免重复演示同一表达式）
    const auto& scenarios = WatchExpressionLibrary::scenarios();
    std::set<std::string> exprs;
    for (const auto& s : scenarios) {
        auto [_, inserted] = exprs.insert(s.expression);
        EXPECT_TRUE(inserted) << "场景表达式重复: " << s.expression;
    }
}

TEST(WatchExpressionLibrary, ScenarioSampleCodeContainsBreakpointHint) {
    // 验证每个场景的示例代码包含断点提示注释（教学场景应明确指出断点位置）
    const auto& scenarios = WatchExpressionLibrary::scenarios();
    for (const auto& s : scenarios) {
        EXPECT_NE(s.sampleCode.find("断点"), std::string::npos) << "场景 " << s.id << " 的示例代码应包含断点提示";
    }
}

TEST(WatchExpressionLibrary, ExpectedTypesAreValid) {
    // 验证 expectedType 字段使用有效的类型名
    const auto& scenarios = WatchExpressionLibrary::scenarios();
    const std::set<std::string> kValidTypes = {"int",  "float",    "bool",    "null",     "string",      "array",
                                               "dict", "instance", "closure", "function", "bound-method"};
    for (const auto& s : scenarios) {
        EXPECT_NE(kValidTypes.find(s.expectedType), kValidTypes.end())
            << "场景 " << s.id << " 的 expectedType 不合法: " << s.expectedType;
    }
}

// ============================================================
// 测试套件 2：PanelCatalog 注册 — watch-expressions 面板（3 用例）
// ============================================================

TEST(WatchPanelCatalogRegistration, WatchExpressionsPanelRegistered) {
    // 验证 watch-expressions 已在 PanelCatalog 注册
    const auto* entry = PanelCatalog::findById("watch-expressions");
    EXPECT_NE(entry, nullptr) << "watch-expressions 面板应在 PanelCatalog 注册";
    if (entry) {
        EXPECT_EQ(std::string(entry->id), "watch-expressions");
        EXPECT_NE(std::string(entry->label), "") << "watch-expressions 面板 label 不能为空";
        EXPECT_NE(std::string(entry->emoji), "") << "watch-expressions 面板 emoji 不能为空";
    }
}

TEST(WatchPanelCatalogRegistration, WatchExpressionsPanelUnderDebugObserveCategory) {
    // 验证 watch-expressions 面板归类在"调试与观测"分类下
    // （UX-R fix: 原"执行引擎"拆分后调试类面板迁入"调试与观测"）
    const auto& cats = PanelCatalog::categories();
    bool found = false;
    for (const auto& cat : cats) {
        if (std::string(cat.title) == "调试与观测") {
            for (const auto& leaf : cat.leaves) {
                if (std::string(leaf.id) == "watch-expressions") {
                    found = true;
                    break;
                }
            }
        }
    }
    EXPECT_TRUE(found) << "watch-expressions 面板应归类在'调试与观测'分类下";
}

TEST(WatchPanelCatalogRegistration, CanonicalIdResolvesWatchExpressions) {
    // 验证 canonicalPanelId 对 "watch-expressions" 返回原值（已注册面板 id 原样返回）
    EXPECT_EQ(PanelCatalog::canonicalPanelId("watch-expressions"), "watch-expressions");
}

// ============================================================
// 测试套件 3：场景库内容深度验证（3 用例）
// ============================================================

TEST(WatchExpressionLibraryContent, InstanceFieldScenarioUsesThisKeyword) {
    // instance-field 场景应使用 this 关键字访问字段
    const auto& scenarios = WatchExpressionLibrary::scenarios();
    for (const auto& s : scenarios) {
        if (s.id == "instance-field") {
            EXPECT_NE(s.expression.find("this"), std::string::npos) << "instance-field 场景的表达式应包含 this 关键字";
            return;
        }
    }
    FAIL() << "未找到 instance-field 场景";
}

TEST(WatchExpressionLibraryContent, MethodCallScenarioUsesMethodInvocation) {
    // method-call 场景应使用方法调用语法（点号 + 括号）
    const auto& scenarios = WatchExpressionLibrary::scenarios();
    for (const auto& s : scenarios) {
        if (s.id == "method-call") {
            EXPECT_NE(s.expression.find("."), std::string::npos) << "method-call 场景的表达式应使用点号访问";
            EXPECT_NE(s.expression.find("("), std::string::npos) << "method-call 场景的表达式应使用括号调用";
            return;
        }
    }
    FAIL() << "未找到 method-call 场景";
}

TEST(WatchExpressionLibraryContent, ArrayIndexScenarioUsesIndexOperator) {
    // array-index 场景应使用索引访问操作符 []
    const auto& scenarios = WatchExpressionLibrary::scenarios();
    for (const auto& s : scenarios) {
        if (s.id == "array-index") {
            EXPECT_NE(s.expression.find("["), std::string::npos) << "array-index 场景的表达式应使用 [ 操作符";
            EXPECT_NE(s.expression.find("]"), std::string::npos) << "array-index 场景的表达式应使用 ] 操作符";
            return;
        }
    }
    FAIL() << "未找到 array-index 场景";
}
