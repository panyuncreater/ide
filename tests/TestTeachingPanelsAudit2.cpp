// ============================================================
// 教学面板数据完整性测试（第二波）
// ------------------------------------------------------------
// 覆盖第二波新增的 3 个数据类：
//   - MemoryModelLibrary（NaN-boxing 示例 + RefCounted 场景 + GC 阶段）
//   - IRTransformLibrary（IR lowering 示例 + 优化 pass 对比）
//   - ProfileLibrary（6 个性能场景）
//
// 验证数据完整性（ID 唯一性 / 关键字段非空 / 数量符合预期 / IR 文本可识别）。
// ============================================================

#include <gtest/gtest.h>

#include "gui/MemoryModelPanel.h"
#include "gui/IRTransformPanel.h"
#include "gui/ProfileDashboardPanel.h"
#include "interpreter/NaNBox.h"

#include <set>
#include <string>

// ============================================================
// MemoryModelLibrary — 数据完整性
// ============================================================

TEST(TeachingPanelsMemoryModel, NaNBoxExamplesCount) {
    const auto& items = MemoryModelLibrary::nanBoxExamples();
    // 设计为 7 个示例：int-inline-pos/neg/boundary + float + bool + null + ptr
    EXPECT_GE(items.size(), 7u);
}

TEST(TeachingPanelsMemoryModel, NaNBoxIdsUnique) {
    const auto& items = MemoryModelLibrary::nanBoxExamples();
    std::set<std::string> ids;
    for (const auto& b : items) {
        EXPECT_FALSE(b.id.empty()) << "示例 id 不能为空";
        auto [_, inserted] = ids.insert(b.id);
        EXPECT_TRUE(inserted) << "示例 id 重复: " << b.id;
    }
    EXPECT_EQ(ids.size(), items.size());
}

TEST(TeachingPanelsMemoryModel, NaNBoxCriticalFieldsNonEmpty) {
    const auto& items = MemoryModelLibrary::nanBoxExamples();
    for (const auto& b : items) {
        EXPECT_FALSE(b.title.empty()) << b.id << ": title 为空";
        EXPECT_FALSE(b.description.empty()) << b.id << ": description 为空";
        EXPECT_FALSE(b.sourceExpr.empty()) << b.id << ": sourceExpr 为空";
    }
}

TEST(TeachingPanelsMemoryModel, NaNBoxBitsMatchEncoding) {
    // 验证 bits 字段与 NaNBox 编码一致
    const auto& items = MemoryModelLibrary::nanBoxExamples();
    for (const auto& b : items) {
        if (b.isInt) {
            NaNBox box = NaNBox::fromInt(b.intVal);
            EXPECT_EQ(box.rawBits(), b.bits)
                << b.id << ": bits 与 NaNBox::fromInt(" << b.intVal << ") 不一致";
        } else if (b.isBool) {
            NaNBox box = NaNBox::fromBool(b.intVal != 0);
            EXPECT_EQ(box.rawBits(), b.bits)
                << b.id << ": bits 与 NaNBox::fromBool 不一致";
        } else if (b.isNull) {
            NaNBox box = NaNBox::null();
            EXPECT_EQ(box.rawBits(), b.bits)
                << b.id << ": bits 与 NaNBox::null 不一致";
        }
        // float 与 pointer 因运行时不同跳过严格相等（仅校验非 0）
        if (b.isFloat || b.isPointer) {
            EXPECT_NE(b.bits, 0ULL) << b.id << ": bits 不应为 0";
        }
    }
}

TEST(TeachingPanelsMemoryModel, NaNBoxExpectedIdsPresent) {
    const auto& items = MemoryModelLibrary::nanBoxExamples();
    std::set<std::string> ids;
    for (const auto& b : items) ids.insert(b.id);
    EXPECT_NE(ids.find("int-inline-pos"), ids.end());
    EXPECT_NE(ids.find("int-inline-neg"), ids.end());
    EXPECT_NE(ids.find("float-direct"), ids.end());
    EXPECT_NE(ids.find("bool-true"), ids.end());
    EXPECT_NE(ids.find("null-value"), ids.end());
}

TEST(TeachingPanelsMemoryModel, RefCountScenariosCount) {
    const auto& items = MemoryModelLibrary::refCountScenarios();
    // 设计为 4 个场景：array-basic / cow-detach / string-shared / instance-fields
    EXPECT_GE(items.size(), 4u);
}

TEST(TeachingPanelsMemoryModel, RefCountScenariosIdsUnique) {
    const auto& items = MemoryModelLibrary::refCountScenarios();
    std::set<std::string> ids;
    for (const auto& s : items) {
        EXPECT_FALSE(s.id.empty()) << "场景 id 为空";
        auto [_, inserted] = ids.insert(s.id);
        EXPECT_TRUE(inserted) << "场景 id 重复: " << s.id;
    }
    EXPECT_EQ(ids.size(), items.size());
}

TEST(TeachingPanelsMemoryModel, RefCountScenariosNonEmptySteps) {
    const auto& items = MemoryModelLibrary::refCountScenarios();
    for (const auto& s : items) {
        EXPECT_FALSE(s.title.empty()) << s.id << ": title 为空";
        EXPECT_FALSE(s.description.empty()) << s.id << ": description 为空";
        EXPECT_FALSE(s.steps.empty()) << s.id << ": steps 不能为空";
        for (const auto& st : s.steps) {
            EXPECT_FALSE(st.action.empty()) << s.id << ": step.action 为空";
            EXPECT_GE(st.refCount, 0) << s.id << ": step.refCount 不应为负";
            EXPECT_FALSE(st.note.empty()) << s.id << ": step.note 为空";
        }
    }
}

TEST(TeachingPanelsMemoryModel, GcPhasesCount) {
    const auto& items = MemoryModelLibrary::gcPhases();
    // 设计为 6 个阶段：注册 / 触发时机 / Mark / Sweep / UAF 防护 / 已知限制
    EXPECT_GE(items.size(), 6u);
}

TEST(TeachingPanelsMemoryModel, GcPhasesCriticalFieldsNonEmpty) {
    const auto& items = MemoryModelLibrary::gcPhases();
    for (const auto& p : items) {
        EXPECT_FALSE(p.title.empty()) << "GC phase title 为空";
        EXPECT_FALSE(p.description.empty()) << "GC phase description 为空";
    }
}

// ============================================================
// IRTransformLibrary — 数据完整性
// ============================================================

TEST(TeachingPanelsIRTransform, LoweringExamplesCount) {
    const auto& items = IRTransformLibrary::loweringExamples();
    // 设计为 8 个：lit-int / binop-add / var-decl / if-stmt / while-stmt /
    // fun-call / closure / class-method
    EXPECT_GE(items.size(), 8u);
}

TEST(TeachingPanelsIRTransform, LoweringIdsUnique) {
    const auto& items = IRTransformLibrary::loweringExamples();
    std::set<std::string> ids;
    for (const auto& e : items) {
        EXPECT_FALSE(e.id.empty()) << "示例 id 为空";
        auto [_, inserted] = ids.insert(e.id);
        EXPECT_TRUE(inserted) << "示例 id 重复: " << e.id;
    }
    EXPECT_EQ(ids.size(), items.size());
}

TEST(TeachingPanelsIRTransform, LoweringCriticalFieldsNonEmpty) {
    const auto& items = IRTransformLibrary::loweringExamples();
    for (const auto& e : items) {
        EXPECT_FALSE(e.title.empty()) << e.id << ": title 为空";
        EXPECT_FALSE(e.description.empty()) << e.id << ": description 为空";
        EXPECT_FALSE(e.astSummary.empty()) << e.id << ": astSummary 为空";
        EXPECT_FALSE(e.sourceCode.empty()) << e.id << ": sourceCode 为空";
        EXPECT_FALSE(e.irBefore.empty()) << e.id << ": irBefore 为空";
    }
}

TEST(TeachingPanelsIRTransform, LoweringIRContainsFunctionKeyword) {
    // 所有 IR 文本应包含 "function" 关键字（IRToString 格式）
    const auto& items = IRTransformLibrary::loweringExamples();
    for (const auto& e : items) {
        EXPECT_NE(e.irBefore.find("function"), std::string::npos)
            << e.id << ": IR 文本缺少 'function' 关键字";
    }
}

TEST(TeachingPanelsIRTransform, OptimizationExamplesCount) {
    const auto& items = IRTransformLibrary::optimizationExamples();
    // 设计为 3 个：const-fold / dead-code / copy-prop
    EXPECT_GE(items.size(), 3u);
}

TEST(TeachingPanelsIRTransform, OptimizationIdsUnique) {
    const auto& items = IRTransformLibrary::optimizationExamples();
    std::set<std::string> ids;
    for (const auto& e : items) {
        EXPECT_FALSE(e.id.empty()) << "优化示例 id 为空";
        auto [_, inserted] = ids.insert(e.id);
        EXPECT_TRUE(inserted) << "优化示例 id 重复: " << e.id;
    }
    EXPECT_EQ(ids.size(), items.size());
}

TEST(TeachingPanelsIRTransform, OptimizationInstrCountConsistent) {
    // 优化后指令数应 <= 优化前指令数
    const auto& items = IRTransformLibrary::optimizationExamples();
    for (const auto& e : items) {
        EXPECT_GT(e.instrBefore, 0) << e.id << ": instrBefore 应 > 0";
        EXPECT_GT(e.instrAfter, 0) << e.id << ": instrAfter 应 > 0";
        EXPECT_LE(e.instrAfter, e.instrBefore)
            << e.id << ": 优化后指令数 (" << e.instrAfter
            << ") 应 <= 优化前 (" << e.instrBefore << ")";
    }
}

TEST(TeachingPanelsIRTransform, OptimizationCriticalFieldsNonEmpty) {
    const auto& items = IRTransformLibrary::optimizationExamples();
    for (const auto& e : items) {
        EXPECT_FALSE(e.title.empty()) << e.id << ": title 为空";
        EXPECT_FALSE(e.passName.empty()) << e.id << ": passName 为空";
        EXPECT_FALSE(e.description.empty()) << e.id << ": description 为空";
        EXPECT_FALSE(e.irBefore.empty()) << e.id << ": irBefore 为空";
        EXPECT_FALSE(e.irAfter.empty()) << e.id << ": irAfter 为空";
    }
}

// ============================================================
// ProfileLibrary — 数据完整性
// ============================================================

TEST(TeachingPanelsProfile, ScenariosCount) {
    const auto& items = ProfileLibrary::scenarios();
    // ROUND-60 fix: closure-capture 已删除（闭包捕获循环），现 5 个场景：
    // fib-recursion / loop-sum / string-concat / class-instantiation / dict-access
    EXPECT_GE(items.size(), 5u);
}

TEST(TeachingPanelsProfile, ScenarioIdsUnique) {
    const auto& items = ProfileLibrary::scenarios();
    std::set<std::string> ids;
    for (const auto& s : items) {
        EXPECT_FALSE(s.id.empty()) << "场景 id 为空";
        auto [_, inserted] = ids.insert(s.id);
        EXPECT_TRUE(inserted) << "场景 id 重复: " << s.id;
    }
    EXPECT_EQ(ids.size(), items.size());
}

TEST(TeachingPanelsProfile, ScenarioCriticalFieldsNonEmpty) {
    const auto& items = ProfileLibrary::scenarios();
    for (const auto& s : items) {
        EXPECT_FALSE(s.title.empty()) << s.id << ": title 为空";
        EXPECT_FALSE(s.description.empty()) << s.id << ": description 为空";
        EXPECT_FALSE(s.sourceCode.empty()) << s.id << ": sourceCode 为空";
        EXPECT_FALSE(s.category.empty()) << s.id << ": category 为空";
        EXPECT_GE(s.iterations, 1) << s.id << ": iterations 应 >= 1";
    }
}

TEST(TeachingPanelsProfile, ScenarioCategoriesRecognized) {
    const auto& items = ProfileLibrary::scenarios();
    std::set<std::string> validCats = {
        "arithmetic", "loop", "string", "class", "closure"
    };
    for (const auto& s : items) {
        EXPECT_NE(validCats.find(s.category), validCats.end())
            << s.id << ": category 不在合法集合中: " << s.category;
    }
}

TEST(TeachingPanelsProfile, ExpectedScenariosPresent) {
    const auto& items = ProfileLibrary::scenarios();
    std::set<std::string> ids;
    for (const auto& s : items) ids.insert(s.id);
    EXPECT_NE(ids.find("fib-recursion"), ids.end());
    EXPECT_NE(ids.find("loop-sum"), ids.end());
    EXPECT_NE(ids.find("string-concat"), ids.end());
    EXPECT_NE(ids.find("class-instantiation"), ids.end());
}
