// ============================================================
// tests/TestExerciseGrader.cpp - 判题题库自校验测试（拓展二期·教学）
// ------------------------------------------------------------
// 验证 ExerciseGraderLibrary 题库：
//   - 每题结构完整（标题/描述/起始代码/用例非空）
//   - 每题恰含 1 个隐藏用例（拓展二期新增）且隐藏用例名不泄漏内容
//   - 全部用例（含隐藏）的参考代码在三后端上真实运行，
//     输出与 expectedOutput 精确一致——题库期望值错了判题就是冤案，
//     这里用引擎本身做单一事实源校验
// ============================================================
#include "gui/ExerciseGraderPanel.h"

#include "common/BackendExecutionService.h"

#include <gtest/gtest.h>

#include <QString>

namespace {

/// 运行一段代码并返回 trim 后输出（失败时返回错误标记，便于断言信息可读）
std::string runTrimmed(const std::string& src, BackendType backend) {
    BackendExecResult r = BackendExecutionService::execute(src, backend);
    if (!r.success) {
        return "<ERROR: " + r.errorPrefix + ": " + r.errorMsg + ">";
    }
    return QString::fromUtf8(r.output.c_str()).trimmed().toStdString();
}

} // namespace

TEST(ExerciseGraderLibraryTest, ExercisesStructureComplete) {
    const auto& exs = ExerciseGraderLibrary::exercises();
    ASSERT_EQ(exs.size(), 4u);
    for (const auto& ex : exs) {
        EXPECT_FALSE(ex.title.empty());
        EXPECT_FALSE(ex.description.empty());
        EXPECT_FALSE(ex.starterCode.empty());
        EXPECT_FALSE(ex.testCases.empty());
        for (const auto& tc : ex.testCases) {
            EXPECT_FALSE(tc.name.empty()) << ex.title;
            EXPECT_FALSE(tc.code.empty()) << ex.title;
            EXPECT_FALSE(tc.expectedOutput.empty()) << ex.title;
        }
    }
}

TEST(ExerciseGraderLibraryTest, EachExerciseHasExactlyOneHiddenCase) {
    for (const auto& ex : ExerciseGraderLibrary::exercises()) {
        int hiddenCount = 0;
        int publicCount = 0;
        for (const auto& tc : ex.testCases) {
            if (tc.hidden)
                ++hiddenCount;
            else
                ++publicCount;
        }
        EXPECT_EQ(hiddenCount, 1) << ex.title;
        EXPECT_EQ(publicCount, 3) << ex.title;
    }
}

TEST(ExerciseGraderLibraryTest, HiddenCaseNameDoesNotLeakContent) {
    // 隐藏用例名不得包含期望输出（用例名会显示在表格中）
    for (const auto& ex : ExerciseGraderLibrary::exercises()) {
        for (const auto& tc : ex.testCases) {
            if (!tc.hidden)
                continue;
            EXPECT_EQ(tc.name.find(tc.expectedOutput), std::string::npos) << ex.title;
        }
    }
}

TEST(ExerciseGraderLibraryTest, AllTestCasesVerifiedOnInterpreter) {
    for (const auto& ex : ExerciseGraderLibrary::exercises()) {
        for (const auto& tc : ex.testCases) {
            EXPECT_EQ(runTrimmed(tc.code, BackendType::Interpreter), tc.expectedOutput)
                << ex.title << " / " << tc.name;
        }
    }
}

TEST(ExerciseGraderLibraryTest, AllTestCasesVerifiedOnStackVM_IR) {
    for (const auto& ex : ExerciseGraderLibrary::exercises()) {
        for (const auto& tc : ex.testCases) {
            EXPECT_EQ(runTrimmed(tc.code, BackendType::StackVM_IR), tc.expectedOutput)
                << ex.title << " / " << tc.name;
        }
    }
}

TEST(ExerciseGraderLibraryTest, AllTestCasesVerifiedOnRegisterVM_IR) {
    for (const auto& ex : ExerciseGraderLibrary::exercises()) {
        for (const auto& tc : ex.testCases) {
            EXPECT_EQ(runTrimmed(tc.code, BackendType::RegisterVM_IR), tc.expectedOutput)
                << ex.title << " / " << tc.name;
        }
    }
}
