// ============================================================
// TestTeachingPanelsAudit5.cpp — 第三档教学面板数据完整性审计
// ============================================================
// 覆盖：
//   - ExceptionFlowLibrary（P2-3a ExceptionFlowPanel 教学场景库）
//   - ExceptionPhaseLibrary（P2-3a ExceptionFlowPanel 传播图解）
//   - ClosureInspectorLibrary（P2-3b ClosureInspectorPanel 教学场景库）
//   - UpvaluePhaseLibrary（P2-3b ClosureInspectorPanel 生命周期图解）
//
// 审计模式：数据完整性 + 关键字段非空 + 分类合法 + 关键 ID 齐全
// ============================================================

#include <gtest/gtest.h>
#include "gui/ExceptionFlowPanel.h"
#include "gui/ClosureInspectorPanel.h"

#include <set>
#include <string>
#include <regex>

// ============================================================
// ExceptionFlowLibrary 审计（P2-3a 教学场景库）
// ============================================================

TEST(TeachingPanelsExceptionFlow, ScenariosCount) {
    const auto& scenarios = ExceptionFlowLibrary::scenarios();
    // 至少 6 个场景
    EXPECT_GE(scenarios.size(), 6u);
}

TEST(TeachingPanelsExceptionFlow, ScenarioIdsUnique) {
    const auto& scenarios = ExceptionFlowLibrary::scenarios();
    std::set<std::string> seen;
    for (const auto& s : scenarios) {
        EXPECT_TRUE(seen.insert(s.id).second)
            << "Duplicate scenario ID: " << s.id;
    }
}

TEST(TeachingPanelsExceptionFlow, CriticalFieldsNonEmpty) {
    const auto& scenarios = ExceptionFlowLibrary::scenarios();
    for (const auto& s : scenarios) {
        EXPECT_FALSE(s.id.empty()) << "id 为空";
        EXPECT_FALSE(s.title.empty()) << "title 为空: " << s.id;
        EXPECT_FALSE(s.description.empty()) << "description 为空: " << s.id;
        EXPECT_FALSE(s.sampleCode.empty()) << "sampleCode 为空: " << s.id;
        EXPECT_FALSE(s.teachingNote.empty()) << "teachingNote 为空: " << s.id;
        EXPECT_FALSE(s.propagationPath.empty()) << "propagationPath 为空: " << s.id;
    }
}

TEST(TeachingPanelsExceptionFlow, CriticalScenarioIdsPresent) {
    const auto& scenarios = ExceptionFlowLibrary::scenarios();
    std::set<std::string> ids;
    for (const auto& s : scenarios) {
        ids.insert(s.id);
    }
    // 关键场景 ID 必须齐全
    EXPECT_TRUE(ids.count("simple-try-catch") > 0) << "缺少 simple-try-catch";
    EXPECT_TRUE(ids.count("uncaught-exception") > 0) << "缺少 uncaught-exception";
    EXPECT_TRUE(ids.count("nested-try-catch") > 0) << "缺少 nested-try-catch";
    EXPECT_TRUE(ids.count("cross-function-propagation") > 0) << "缺少 cross-function-propagation";
    EXPECT_TRUE(ids.count("rethrow") > 0) << "缺少 rethrow";
}

TEST(TeachingPanelsExceptionFlow, SampleCodeHasThrow) {
    const auto& scenarios = ExceptionFlowLibrary::scenarios();
    for (const auto& s : scenarios) {
        EXPECT_NE(s.sampleCode.find("throw"), std::string::npos)
            << "sampleCode 缺少 throw 语句: " << s.id;
    }
}

TEST(TeachingPanelsExceptionFlow, PropagationPathNonEmpty) {
    const auto& scenarios = ExceptionFlowLibrary::scenarios();
    for (const auto& s : scenarios) {
        for (const auto& step : s.propagationPath) {
            EXPECT_FALSE(step.empty()) << "propagationPath 步骤为空: " << s.id;
        }
    }
}

TEST(TeachingPanelsExceptionFlow, DescriptionMentionsExceptionSemantics) {
    const auto& scenarios = ExceptionFlowLibrary::scenarios();
    for (const auto& s : scenarios) {
        // description 或 teachingNote 应提及异常相关术语
        std::string combined = s.description + s.teachingNote;
        bool hasTerm = combined.find("异常") != std::string::npos ||
                       combined.find("catch") != std::string::npos ||
                       combined.find("throw") != std::string::npos ||
                       combined.find("传播") != std::string::npos ||
                       combined.find("栈展开") != std::string::npos ||
                       combined.find("捕获") != std::string::npos;
        EXPECT_TRUE(hasTerm) << "description/teachingNote 缺少异常语义术语: " << s.id;
    }
}

// ============================================================
// ExceptionPhaseLibrary 审计（P2-3a 传播图解）
// ============================================================

TEST(TeachingPanelsExceptionFlow, PhasesCount) {
    const auto& phases = ExceptionPhaseLibrary::phases();
    // 至少 4 个阶段
    EXPECT_GE(phases.size(), 4u);
}

TEST(TeachingPanelsExceptionFlow, PhaseNamesUnique) {
    const auto& phases = ExceptionPhaseLibrary::phases();
    std::set<std::string> seen;
    for (const auto& p : phases) {
        EXPECT_TRUE(seen.insert(p.phase).second)
            << "Duplicate phase: " << p.phase;
    }
}

TEST(TeachingPanelsExceptionFlow, PhaseCategoriesValid) {
    const auto& phases = ExceptionPhaseLibrary::phases();
    std::set<std::string> validCats = {
        "throw", "search", "catch", "finally", "unwind", "recovery"
    };
    for (const auto& p : phases) {
        EXPECT_TRUE(validCats.count(p.category) > 0)
            << "Invalid category '" << p.category << "' for phase " << p.phase;
    }
}

TEST(TeachingPanelsExceptionFlow, CriticalPhasesPresent) {
    const auto& phases = ExceptionPhaseLibrary::phases();
    std::set<std::string> names;
    for (const auto& p : phases) {
        names.insert(p.phase);
    }
    // 关键阶段必须齐全
    EXPECT_TRUE(names.count("throw") > 0) << "缺少 throw 阶段";
    EXPECT_TRUE(names.count("catch") > 0) << "缺少 catch 阶段";
    EXPECT_TRUE(names.count("unwind") > 0) << "缺少 unwind 阶段";
}

TEST(TeachingPanelsExceptionFlow, PhaseFieldsNonEmpty) {
    const auto& phases = ExceptionPhaseLibrary::phases();
    for (const auto& p : phases) {
        EXPECT_FALSE(p.phase.empty()) << "phase 为空";
        EXPECT_FALSE(p.category.empty()) << "category 为空: " << p.phase;
        EXPECT_FALSE(p.description.empty()) << "description 为空: " << p.phase;
        EXPECT_FALSE(p.stackEffect.empty()) << "stackEffect 为空: " << p.phase;
    }
}

// ============================================================
// ClosureInspectorLibrary 审计（P2-3b 教学场景库）
// ============================================================

TEST(TeachingPanelsClosureInspector, ScenariosCount) {
    const auto& scenarios = ClosureInspectorLibrary::scenarios();
    // ROUND-60: 扩充至 12 个场景（原 8 + 新增 4：as-param/recursive/close-upvalue/mutual-recursion）
    EXPECT_GE(scenarios.size(), 8u);
}

TEST(TeachingPanelsClosureInspector, ScenarioIdsUnique) {
    const auto& scenarios = ClosureInspectorLibrary::scenarios();
    std::set<std::string> seen;
    for (const auto& s : scenarios) {
        EXPECT_TRUE(seen.insert(s.id).second)
            << "Duplicate scenario ID: " << s.id;
    }
}

TEST(TeachingPanelsClosureInspector, CriticalFieldsNonEmpty) {
    const auto& scenarios = ClosureInspectorLibrary::scenarios();
    for (const auto& s : scenarios) {
        EXPECT_FALSE(s.id.empty()) << "id 为空";
        EXPECT_FALSE(s.title.empty()) << "title 为空: " << s.id;
        EXPECT_FALSE(s.description.empty()) << "description 为空: " << s.id;
        EXPECT_FALSE(s.sampleCode.empty()) << "sampleCode 为空: " << s.id;
        EXPECT_FALSE(s.teachingNote.empty()) << "teachingNote 为空: " << s.id;
        EXPECT_FALSE(s.capturedVars.empty()) << "capturedVars 为空: " << s.id;
        EXPECT_FALSE(s.captureType.empty()) << "captureType 为空: " << s.id;
    }
}

TEST(TeachingPanelsClosureInspector, CriticalScenarioIdsPresent) {
    const auto& scenarios = ClosureInspectorLibrary::scenarios();
    std::set<std::string> ids;
    for (const auto& s : scenarios) {
        ids.insert(s.id);
    }
    // 关键场景 ID 必须齐全
    EXPECT_TRUE(ids.count("simple-capture") > 0) << "缺少 simple-capture";
    EXPECT_TRUE(ids.count("counter-pattern") > 0) << "缺少 counter-pattern";
    EXPECT_TRUE(ids.count("nested-closure") > 0) << "缺少 nested-closure";
    EXPECT_TRUE(ids.count("closure-as-return") > 0) << "缺少 closure-as-return";
    EXPECT_TRUE(ids.count("closure-escape") > 0) << "缺少 closure-escape";
}

TEST(TeachingPanelsClosureInspector, SampleCodeHasClosure) {
    const auto& scenarios = ClosureInspectorLibrary::scenarios();
    for (const auto& s : scenarios) {
        // sampleCode 应包含 fun 关键字（定义闭包）
        EXPECT_NE(s.sampleCode.find("fun"), std::string::npos)
            << "sampleCode 缺少 fun 关键字: " << s.id;
    }
}

TEST(TeachingPanelsClosureInspector, CaptureTypeIsUpvalue) {
    const auto& scenarios = ClosureInspectorLibrary::scenarios();
    for (const auto& s : scenarios) {
        // captureType 应包含 upvalue（按引用捕获）
        EXPECT_NE(s.captureType.find("upvalue"), std::string::npos)
            << "captureType 缺少 upvalue: " << s.id;
    }
}

TEST(TeachingPanelsClosureInspector, DescriptionMentionsClosureSemantics) {
    const auto& scenarios = ClosureInspectorLibrary::scenarios();
    for (const auto& s : scenarios) {
        // description 或 teachingNote 应提及闭包相关术语
        std::string combined = s.description + s.teachingNote;
        bool hasTerm = combined.find("闭包") != std::string::npos ||
                       combined.find("upvalue") != std::string::npos ||
                       combined.find("捕获") != std::string::npos ||
                       combined.find("堆化") != std::string::npos ||
                       combined.find("逃逸") != std::string::npos ||
                       combined.find("作用域") != std::string::npos;
        EXPECT_TRUE(hasTerm) << "description/teachingNote 缺少闭包语义术语: " << s.id;
    }
}

// ============================================================
// UpvaluePhaseLibrary 审计（P2-3b 生命周期图解）
// ============================================================

TEST(TeachingPanelsClosureInspector, PhasesCount) {
    const auto& phases = UpvaluePhaseLibrary::phases();
    // 至少 4 个阶段
    EXPECT_GE(phases.size(), 4u);
}

TEST(TeachingPanelsClosureInspector, PhaseNamesUnique) {
    const auto& phases = UpvaluePhaseLibrary::phases();
    std::set<std::string> seen;
    for (const auto& p : phases) {
        EXPECT_TRUE(seen.insert(p.phase).second)
            << "Duplicate phase: " << p.phase;
    }
}

TEST(TeachingPanelsClosureInspector, PhaseCategoriesValid) {
    const auto& phases = UpvaluePhaseLibrary::phases();
    std::set<std::string> validCats = {
        "create", "capture", "heap", "access", "close", "destroy"
    };
    for (const auto& p : phases) {
        EXPECT_TRUE(validCats.count(p.category) > 0)
            << "Invalid category '" << p.category << "' for phase " << p.phase;
    }
}

TEST(TeachingPanelsClosureInspector, CriticalPhasesPresent) {
    const auto& phases = UpvaluePhaseLibrary::phases();
    std::set<std::string> names;
    for (const auto& p : phases) {
        names.insert(p.phase);
    }
    // 关键阶段必须齐全
    EXPECT_TRUE(names.count("create") > 0) << "缺少 create 阶段";
    EXPECT_TRUE(names.count("capture") > 0) << "缺少 capture 阶段";
    EXPECT_TRUE(names.count("heap") > 0) << "缺少 heap 阶段";
}

TEST(TeachingPanelsClosureInspector, PhaseFieldsNonEmpty) {
    const auto& phases = UpvaluePhaseLibrary::phases();
    for (const auto& p : phases) {
        EXPECT_FALSE(p.phase.empty()) << "phase 为空";
        EXPECT_FALSE(p.category.empty()) << "category 为空: " << p.phase;
        EXPECT_FALSE(p.description.empty()) << "description 为空: " << p.phase;
        EXPECT_FALSE(p.stackEffect.empty()) << "stackEffect 为空: " << p.phase;
    }
}
