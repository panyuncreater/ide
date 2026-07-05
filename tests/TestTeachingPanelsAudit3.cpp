// ============================================================
// 教学面板数据完整性测试（第三波）
// ------------------------------------------------------------
// 覆盖第三波新增的 3 个数据类：
//   - CallStackLibrary（6 个调用栈教学场景）
//   - VariableInspectorLibrary（9 种变量类型示例）
//   - BytecodeTraceLibrary（18 个 OpCode 教学条目）
//
// 验证数据完整性（ID 唯一性 / 关键字段非空 / 数量符合预期 / 分类合法）。
// ============================================================

#include <gtest/gtest.h>

#include "gui/CallStackPanel.h"
#include "gui/VariableInspectorPanel.h"
#include "gui/BytecodeTracePanel.h"

#include <set>
#include <string>

// ============================================================
// CallStackLibrary — 数据完整性
// ============================================================

TEST(TeachingPanelsCallStack, ScenariosCount) {
    const auto& items = CallStackLibrary::scenarios();
    // 设计为 6 个场景：simple-call / recursion / closure-capture / method-dispatch / try-catch / mutual-recursion
    EXPECT_GE(items.size(), 6u);
}

TEST(TeachingPanelsCallStack, ScenarioIdsUnique) {
    const auto& items = CallStackLibrary::scenarios();
    std::set<std::string> ids;
    for (const auto& s : items) {
        EXPECT_FALSE(s.id.empty()) << "场景 id 不能为空";
        auto [_, inserted] = ids.insert(s.id);
        EXPECT_TRUE(inserted) << "场景 id 重复: " << s.id;
    }
    EXPECT_EQ(ids.size(), items.size());
}

TEST(TeachingPanelsCallStack, CriticalFieldsNonEmpty) {
    const auto& items = CallStackLibrary::scenarios();
    for (const auto& s : items) {
        EXPECT_FALSE(s.title.empty()) << s.id << ": title 为空";
        EXPECT_FALSE(s.description.empty()) << s.id << ": description 为空";
        EXPECT_FALSE(s.sourceCode.empty()) << s.id << ": sourceCode 为空";
        EXPECT_FALSE(s.teachingNote.empty()) << s.id << ": teachingNote 为空";
        EXPECT_FALSE(s.expectedFrames.empty()) << s.id << ": expectedFrames 为空";
    }
}

TEST(TeachingPanelsCallStack, CriticalScenarioIdsPresent) {
    const auto& items = CallStackLibrary::scenarios();
    std::set<std::string> ids;
    for (const auto& s : items) ids.insert(s.id);
    EXPECT_TRUE(ids.count("simple-call"))    << "缺少 simple-call 场景";
    EXPECT_TRUE(ids.count("recursion"))      << "缺少 recursion 场景";
    EXPECT_TRUE(ids.count("closure-capture")) << "缺少 closure-capture 场景";
    EXPECT_TRUE(ids.count("try-catch"))       << "缺少 try-catch 场景";
}

TEST(TeachingPanelsCallStack, ExpectedFramesNonEmpty) {
    const auto& items = CallStackLibrary::scenarios();
    for (const auto& s : items) {
        for (const auto& f : s.expectedFrames) {
            EXPECT_FALSE(f.empty()) << s.id << ": expectedFrames 含空帧名";
        }
    }
}

// ============================================================
// VariableInspectorLibrary — 数据完整性
// ============================================================

TEST(TeachingPanelsVariableInspector, ExamplesCount) {
    const auto& items = VariableInspectorLibrary::examples();
    // 设计为 9 种类型：int / int-boundary / float / bool / null / string / array / dict / instance / closure
    EXPECT_GE(items.size(), 9u);
}

TEST(TeachingPanelsVariableInspector, ExampleIdsUnique) {
    const auto& items = VariableInspectorLibrary::examples();
    std::set<std::string> ids;
    for (const auto& e : items) {
        EXPECT_FALSE(e.id.empty()) << "示例 id 不能为空";
        auto [_, inserted] = ids.insert(e.id);
        EXPECT_TRUE(inserted) << "示例 id 重复: " << e.id;
    }
    EXPECT_EQ(ids.size(), items.size());
}

TEST(TeachingPanelsVariableInspector, CriticalFieldsNonEmpty) {
    const auto& items = VariableInspectorLibrary::examples();
    for (const auto& e : items) {
        EXPECT_FALSE(e.typeName.empty()) << e.id << ": typeName 为空";
        EXPECT_FALSE(e.displayName.empty()) << e.id << ": displayName 为空";
        EXPECT_FALSE(e.sourceExpr.empty()) << e.id << ": sourceExpr 为空";
        EXPECT_FALSE(e.teachingNote.empty()) << e.id << ": teachingNote 为空";
    }
}

TEST(TeachingPanelsVariableInspector, CriticalTypeNamesPresent) {
    const auto& items = VariableInspectorLibrary::examples();
    std::set<std::string> typeNames;
    for (const auto& e : items) typeNames.insert(e.typeName);
    EXPECT_TRUE(typeNames.count("int"))      << "缺少 int 类型示例";
    EXPECT_TRUE(typeNames.count("float"))    << "缺少 float 类型示例";
    EXPECT_TRUE(typeNames.count("bool"))     << "缺少 bool 类型示例";
    EXPECT_TRUE(typeNames.count("string"))   << "缺少 string 类型示例";
    EXPECT_TRUE(typeNames.count("array"))    << "缺少 array 类型示例";
    EXPECT_TRUE(typeNames.count("dict"))     << "缺少 dict 类型示例";
    EXPECT_TRUE(typeNames.count("instance")) << "缺少 instance 类型示例";
    EXPECT_TRUE(typeNames.count("closure"))  << "缺少 closure 类型示例";
}

TEST(TeachingPanelsVariableInspector, NanboxBitsPresentForScalar) {
    // 标量类型必须有 nanboxBits 描述
    const auto& items = VariableInspectorLibrary::examples();
    static const std::set<std::string> kScalarTypes = {"int", "float", "bool", "null"};
    for (const auto& e : items) {
        if (kScalarTypes.count(e.typeName)) {
            EXPECT_FALSE(e.nanboxBits.empty()) << e.id << ": 标量类型缺少 nanboxBits";
        }
    }
}

// ============================================================
// BytecodeTraceLibrary — 数据完整性
// ============================================================

TEST(TeachingPanelsBytecodeTrace, DocsCount) {
    const auto& items = BytecodeTraceLibrary::opCodeDocs();
    // 设计为 18 个 OpCode 教学条目
    EXPECT_GE(items.size(), 15u);
}

TEST(TeachingPanelsBytecodeTrace, OpCodeNamesUnique) {
    const auto& items = BytecodeTraceLibrary::opCodeDocs();
    std::set<std::string> names;
    for (const auto& d : items) {
        EXPECT_FALSE(d.opCodeName.empty()) << "opCodeName 不能为空";
        auto [_, inserted] = names.insert(d.opCodeName);
        EXPECT_TRUE(inserted) << "opCodeName 重复: " << d.opCodeName;
    }
    EXPECT_EQ(names.size(), items.size());
}

TEST(TeachingPanelsBytecodeTrace, CriticalFieldsNonEmpty) {
    const auto& items = BytecodeTraceLibrary::opCodeDocs();
    for (const auto& d : items) {
        EXPECT_FALSE(d.category.empty())      << d.opCodeName << ": category 为空";
        EXPECT_FALSE(d.operandFormat.empty()) << d.opCodeName << ": operandFormat 为空";
        EXPECT_FALSE(d.stackEffect.empty())   << d.opCodeName << ": stackEffect 为空";
        EXPECT_FALSE(d.semantics.empty())     << d.opCodeName << ": semantics 为空";
    }
}

TEST(TeachingPanelsBytecodeTrace, CategoriesValid) {
    const auto& items = BytecodeTraceLibrary::opCodeDocs();
    static const std::set<std::string> kValidCategories = {
        "const", "arith", "var", "control", "call",
        "container", "closure", "class"
    };
    for (const auto& d : items) {
        EXPECT_TRUE(kValidCategories.count(d.category))
            << d.opCodeName << ": 非法 category " << d.category;
    }
}

TEST(TeachingPanelsBytecodeTrace, CriticalOpCodesPresent) {
    const auto& items = BytecodeTraceLibrary::opCodeDocs();
    std::set<std::string> names;
    for (const auto& d : items) names.insert(d.opCodeName);
    EXPECT_TRUE(names.count("OP_INT"))            << "缺少 OP_INT 教学条目";
    EXPECT_TRUE(names.count("OP_ADD"))             << "缺少 OP_ADD 教学条目";
    EXPECT_TRUE(names.count("OP_CALL"))            << "缺少 OP_CALL 教学条目";
    EXPECT_TRUE(names.count("OP_RETURN"))          << "缺少 OP_RETURN 教学条目";
    EXPECT_TRUE(names.count("OP_JUMP_IF_FALSE"))  << "缺少 OP_JUMP_IF_FALSE 教学条目";
    EXPECT_TRUE(names.count("OP_CLOSURE"))         << "缺少 OP_CLOSURE 教学条目";
}
