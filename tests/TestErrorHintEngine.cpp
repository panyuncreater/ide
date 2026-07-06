// ============================================================
// TestErrorHintEngine.cpp — 错误信息友好化增强引擎测试
// ------------------------------------------------------------
// 覆盖 ErrorHintEngine 的两大核心能力：
//   1. ErrorHintEngineSpelling.* — 拼写建议（Levenshtein 编辑距离）
//   2. ErrorHintEnginePatterns.* — 常见错误模式匹配
//
// 测试框架：GoogleTest（项目已集成，doctest 不可用）
// 依赖约束：仅链接 minilang_core + ErrorHintEngine.cpp，不依赖 IdeController.h
// ============================================================

#include <gtest/gtest.h>

#include "gui/ErrorHintEngine.h"

#include <string>
#include <vector>

// ============================================================
// 测试套件 1：ErrorHintEngineSpelling — 拼写建议（6 用例）
// ============================================================

TEST(ErrorHintEngineSpelling, LevenshteinDistanceBasicCalculation) {
    // 基础编辑距离计算验证
    EXPECT_EQ(ErrorHintEngine::levenshteinDistance("", ""), 0);
    EXPECT_EQ(ErrorHintEngine::levenshteinDistance("abc", "abc"), 0);
    EXPECT_EQ(ErrorHintEngine::levenshteinDistance("abc", "abd"), 1);   // 替换
    EXPECT_EQ(ErrorHintEngine::levenshteinDistance("abc", "ab"), 1);    // 删除
    EXPECT_EQ(ErrorHintEngine::levenshteinDistance("ab", "abc"), 1);    // 插入
    EXPECT_EQ(ErrorHintEngine::levenshteinDistance("kitten", "sitting"), 3);  // 经典案例
    EXPECT_EQ(ErrorHintEngine::levenshteinDistance("count", "coount"), 1);    // 多一个字母
    EXPECT_EQ(ErrorHintEngine::levenshteinDistance("count", "counter"), 2);   // 多两个字母
}

TEST(ErrorHintEngineSpelling, ThresholdBoundaryCases) {
    // 阈值 = 2：距离 0/1/2 应被接受，距离 3+ 不应被接受
    std::vector<std::string> candidates = {"cat", "cats", "cart", "category"};

    // 距离 0（完全相同）— 不应返回建议（因为名字相同）
    EXPECT_EQ(ErrorHintEngine::suggestSpelling("cat", candidates), "");

    // 距离 1 — 应返回 "cats" 或 "cart"（距离更近的优先）
    std::string r1 = ErrorHintEngine::suggestSpelling("catt", candidates);
    EXPECT_FALSE(r1.empty());
    EXPECT_TRUE(r1 == "cat" || r1 == "cats");

    // 距离 2 — 应返回
    std::string r2 = ErrorHintEngine::suggestSpelling("ca", {"cat"});
    EXPECT_EQ(r2, "cat");

    // 距离 3 — 超出阈值，应返回空
    EXPECT_EQ(ErrorHintEngine::suggestSpelling("xyz", candidates), "");
    EXPECT_EQ(ErrorHintEngine::suggestSpelling("xyzxyz", {"abc"}), "");
}

TEST(ErrorHintEngineSpelling, EmptyCandidateSet) {
    // 空候选集应返回空字符串
    EXPECT_EQ(ErrorHintEngine::suggestSpelling("foo", {}), "");
    EXPECT_EQ(ErrorHintEngine::suggestSpelling("", {"foo", "bar"}), "");
    EXPECT_EQ(ErrorHintEngine::suggestSpelling("", {}), "");

    // 候选集中包含空字符串应被跳过
    EXPECT_EQ(ErrorHintEngine::suggestSpelling("foo", {"", ""}), "");
}

TEST(ErrorHintEngineSpelling, MultipleCandidatesPickBest) {
    // 多候选时应选编辑距离最小的
    std::vector<std::string> candidates = {"counter", "count", "amount", "account"};
    // "coount" → "count"(距离1) vs "counter"(距离2) vs "account"(距离2) vs "amount"(距离3)
    EXPECT_EQ(ErrorHintEngine::suggestSpelling("coount", candidates), "count");

    // "counte" → "counter"(距离1) vs "count"(距离1) — 两者距离相同，应返回其中一个
    std::string r = ErrorHintEngine::suggestSpelling("counte", candidates);
    EXPECT_TRUE(r == "counter" || r == "count");
    EXPECT_FALSE(r.empty());
}

TEST(ErrorHintEngineSpelling, CaseSensitivity) {
    // 编辑距离按字节比较，大小写敏感
    // "Cat" vs "cat" → 距离 1（C→c 替换）
    EXPECT_EQ(ErrorHintEngine::levenshteinDistance("Cat", "cat"), 1);
    EXPECT_EQ(ErrorHintEngine::levenshteinDistance("CAT", "cat"), 3);

    // 拼写建议大小写敏感：不同大小写仍可匹配（距离 ≤ 2）
    EXPECT_EQ(ErrorHintEngine::suggestSpelling("Cat", {"cat"}), "cat");      // 距离 1
    EXPECT_EQ(ErrorHintEngine::suggestSpelling("Count", {"count"}), "count"); // 距离 1
    // 距离 3（全大写 vs 全小写）— 超出阈值
    EXPECT_EQ(ErrorHintEngine::suggestSpelling("CAT", {"cat"}), "");
}

TEST(ErrorHintEngineSpelling, NumbersAndSymbolsHandling) {
    // 数字与符号混合的名称也应正确计算距离
    EXPECT_EQ(ErrorHintEngine::levenshteinDistance("var1", "var2"), 1);      // 数字替换
    EXPECT_EQ(ErrorHintEngine::levenshteinDistance("arr[0]", "arr[1]"), 1);  // 索引数字替换
    EXPECT_EQ(ErrorHintEngine::levenshteinDistance("x_1", "x_2"), 1);
    EXPECT_EQ(ErrorHintEngine::levenshteinDistance("x_1", "x_12"), 1);       // 多一位数字

    // 拼写建议支持数字名称
    EXPECT_EQ(ErrorHintEngine::suggestSpelling("var1", {"var2", "var3"}), "var2");
    EXPECT_EQ(ErrorHintEngine::suggestSpelling("var1", {"var2"}), "var2");

    // 纯数字名称
    EXPECT_EQ(ErrorHintEngine::levenshteinDistance("123", "1234"), 1);
    EXPECT_EQ(ErrorHintEngine::suggestSpelling("123", {"1234", "12345"}), "1234");
}

// ============================================================
// 测试套件 2：ErrorHintEnginePatterns — 错误模式匹配（6 用例）
// ============================================================

TEST(ErrorHintEnginePatterns, MissingSemicolonPattern) {
    // 缺少分号 — 中文模式（实际代码库使用）
    std::string enriched1 = ErrorHintEngine::enrichErrorMessage(
        "期望 ';' (行 5, 列 20)", "parse", {});
    EXPECT_NE(enriched1.find("分号"), std::string::npos)
        << "缺少分号错误应包含分号提示";
    EXPECT_NE(enriched1.find("期望 ';'"), std::string::npos)
        << "应保留原始错误消息";

    // 缺少分号 — 英文模式（任务规格）
    std::string enriched2 = ErrorHintEngine::enrichErrorMessage(
        "Expected ';' at line 5", "parse", {});
    EXPECT_NE(enriched2.find("分号"), std::string::npos)
        << "英文缺分号错误也应匹配并返回中文提示";

    // 括号不匹配
    std::string enriched3 = ErrorHintEngine::enrichErrorMessage(
        "期望 ')' (行 2, 列 15)", "parse", {});
    EXPECT_NE(enriched3.find("括号"), std::string::npos);

    // 花括号不匹配
    std::string enriched4 = ErrorHintEngine::enrichErrorMessage(
        "期望 '}' (行 10, 列 5)", "parse", {});
    EXPECT_NE(enriched4.find("花括号"), std::string::npos);
}

TEST(ErrorHintEnginePatterns, UnknownPatternPassthrough) {
    // 未知错误模式应原样返回（无附加提示）
    std::string original = "某些未知错误信息 xyz123";
    std::string enriched = ErrorHintEngine::enrichErrorMessage(original, "runtime", {});
    EXPECT_EQ(enriched, original)
        << "未知错误模式应原样返回，不附加任何提示";

    // 另一个未知模式
    std::string original2 = "Internal compiler error: phase 42";
    std::string enriched2 = ErrorHintEngine::enrichErrorMessage(original2, "compile", {});
    EXPECT_EQ(enriched2, original2);
}

TEST(ErrorHintEnginePatterns, UndefinedVariableWithScopeSuggestion) {
    // 未定义变量 + 作用域中有相似变量 → 应包含拼写建议
    std::vector<std::string> scopeVars = {"count", "counter", "amount", "total"};
    std::string enriched = ErrorHintEngine::enrichErrorMessage(
        "未定义的变量: coount", "runtime", scopeVars);

    // 应保留原始消息
    EXPECT_NE(enriched.find("未定义的变量: coount"), std::string::npos);
    // 应包含拼写建议
    EXPECT_NE(enriched.find("count"), std::string::npos)
        << "应建议 'count' 作为相似变量名";
    // 应有 "提示" 前缀
    EXPECT_NE(enriched.find("提示"), std::string::npos);

    // 英文模式
    std::string enrichedEn = ErrorHintEngine::enrichErrorMessage(
        "Undefined variable 'coount' at line 3", "runtime", scopeVars);
    EXPECT_NE(enrichedEn.find("count"), std::string::npos)
        << "英文未定义变量错误也应返回拼写建议";
}

TEST(ErrorHintEnginePatterns, UndefinedFunctionWithoutScope) {
    // 未定义函数 + 无作用域变量 → 应有"函数无提升"提示但无拼写建议
    std::string enriched = ErrorHintEngine::enrichErrorMessage(
        "未定义的函数: mian", "runtime", {});

    // 应包含"函数无提升"提示
    EXPECT_NE(enriched.find("函数无提升"), std::string::npos)
        << "未定义函数应提示'函数无提升——必须先声明后使用'";
    // 应保留原始消息
    EXPECT_NE(enriched.find("未定义的函数: mian"), std::string::npos);

    // 有作用域变量时也应有拼写建议
    std::vector<std::string> scopeVars = {"main", "init", "run"};
    std::string enriched2 = ErrorHintEngine::enrichErrorMessage(
        "未定义的函数: mian", "runtime", scopeVars);
    EXPECT_NE(enriched2.find("main"), std::string::npos)
        << "应建议 'main' 作为相似函数名";
    EXPECT_NE(enriched2.find("函数无提升"), std::string::npos);
}

TEST(ErrorHintEnginePatterns, DivisionByZero) {
    // 除零错误 — 中文模式
    std::string enriched1 = ErrorHintEngine::enrichErrorMessage(
        "除零错误", "runtime", {});
    EXPECT_NE(enriched1.find("除零错误"), std::string::npos)
        << "应保留原始错误消息";
    EXPECT_NE(enriched1.find("除数"), std::string::npos)
        << "应包含除数相关提示";

    // 除零错误 — 英文模式
    std::string enriched2 = ErrorHintEngine::enrichErrorMessage(
        "Division by zero at line 8", "runtime", {});
    EXPECT_NE(enriched2.find("除数"), std::string::npos)
        << "英文除零错误也应返回中文提示";
}

TEST(ErrorHintEnginePatterns, IndexOutOfBounds) {
    // 数组索引越界 — 中文模式
    std::string enriched1 = ErrorHintEngine::enrichErrorMessage(
        "数组索引越界: 10, 有效范围 [0, 5)", "runtime", {});
    EXPECT_NE(enriched1.find("数组索引越界"), std::string::npos)
        << "应保留原始错误消息";
    EXPECT_NE(enriched1.find("数组长度"), std::string::npos)
        << "应包含数组长度相关提示";

    // 数组索引越界 — 英文模式
    std::string enriched2 = ErrorHintEngine::enrichErrorMessage(
        "Index out of bounds: 10 (size=5)", "runtime", {});
    EXPECT_NE(enriched2.find("数组长度"), std::string::npos)
        << "英文越界错误也应返回中文提示";

    // 类型错误模式
    std::string enriched3 = ErrorHintEngine::enrichErrorMessage(
        "Type error: expected int, got string", "compile", {});
    EXPECT_NE(enriched3.find("类型不匹配"), std::string::npos)
        << "类型错误应包含类型不匹配提示";
    EXPECT_NE(enriched3.find("toInt"), std::string::npos)
        << "string→int 错误应提示用 toInt() 转换";

    // int 注解拒绝 float
    std::string enriched4 = ErrorHintEngine::enrichErrorMessage(
        "Type error: expected int, got float", "compile", {});
    EXPECT_NE(enriched4.find("float"), std::string::npos)
        << "int 拒绝 float 错误应提及 float 注解";
}
