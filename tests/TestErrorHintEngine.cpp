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

#include <set>
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

// ============================================================
// 测试套件 3：ErrorHintEnginePatternsTable — 错误模式表（P1-F12 fix）
// ------------------------------------------------------------
// 验证 errorPatterns() 返回的模式表完整性，供手册「常见错误速查」段引用。
// 每条模式需有非空 tag/title/buggyCode/category，且 tag 唯一。
// ============================================================

TEST(ErrorHintEnginePatternsTable, PatternsAreNonEmpty) {
    const auto& patterns = ErrorHintEngine::errorPatterns();
    EXPECT_FALSE(patterns.empty()) << "错误模式表不应为空";
    for (const auto& p : patterns) {
        EXPECT_FALSE(p.tag.empty()) << "tag 不能为空";
        EXPECT_FALSE(p.title.empty()) << "title 不能为空";
        EXPECT_FALSE(p.buggyCode.empty()) << "buggyCode 不能为空";
        EXPECT_FALSE(p.category.empty()) << "category 不能为空";
    }
}

TEST(ErrorHintEnginePatternsTable, TagsAreUnique) {
    const auto& patterns = ErrorHintEngine::errorPatterns();
    std::set<std::string> tags;
    for (const auto& p : patterns) {
        auto [_, inserted] = tags.insert(p.tag);
        EXPECT_TRUE(inserted) << "tag 重复: " << p.tag;
    }
    EXPECT_EQ(tags.size(), patterns.size());
}

TEST(ErrorHintEnginePatternsTable, KnownTagsPresent) {
    const auto& patterns = ErrorHintEngine::errorPatterns();
    std::set<std::string> tags;
    for (const auto& p : patterns) tags.insert(p.tag);

    // 验证关键 tag 存在（与 enrichErrorMessage 中的模式对应）
    EXPECT_NE(tags.find("missing-semicolon"), tags.end());
    EXPECT_NE(tags.find("undefined-variable"), tags.end());
    EXPECT_NE(tags.find("division-by-zero"), tags.end());
    EXPECT_NE(tags.find("index-out-of-bounds"), tags.end());
}

TEST(ErrorHintEnginePatternsTable, CategoriesAreValid) {
    const auto& patterns = ErrorHintEngine::errorPatterns();
    const std::set<std::string> kValidCategories = {"parser", "runtime", "type"};
    for (const auto& p : patterns) {
        EXPECT_NE(kValidCategories.find(p.category), kValidCategories.end())
            << "未知 category: " << p.category << " (tag=" << p.tag << ")";
    }
}

// ============================================================
// 测试套件 4：ErrorHintEngineCodeMatching — P2 错误码优先匹配
// ------------------------------------------------------------
// P2 fix: Diagnostic 携带稳定 code 后，enrichErrorMessage 4 参重载优先按
// code 查 errorPatterns 表附加教学提示，避免消息文案变化时子串匹配失效。
// 验证：code 为空回退子串 / 已知 code 精确匹配 / 未知 code 回退子串 /
// 英文/本地化消息仍能通过 code 精确匹配（核心价值）。
// ============================================================

TEST(ErrorHintEngineCodeMatching, EmptyCodeFallsBackToSubstring) {
    // code 为空 → 走 3 参子串匹配（向后兼容）
    std::string msg = "期望 ';' (行 5, 列 20)";
    std::string enriched3 = ErrorHintEngine::enrichErrorMessage(msg, "parse", {});
    std::string enriched4 = ErrorHintEngine::enrichErrorMessage(msg, "", "parse", {});
    EXPECT_EQ(enriched3, enriched4)
        << "code 为空时 4 参版本应与 3 参版本行为一致";
    EXPECT_NE(enriched4.find("分号"), std::string::npos);
}

TEST(ErrorHintEngineCodeMatching, KnownCodeMissingSemicolon) {
    // code="missing-semicolon" 应附加分号提示，即使 msg 是英文/未知文案
    std::string msg = "syntax error near line 5";  // 子串匹配匹配不到
    std::string enriched = ErrorHintEngine::enrichErrorMessage(
        msg, "missing-semicolon", "parse", {});
    EXPECT_NE(enriched.find("分号"), std::string::npos)
        << "code=missing-semicolon 应附加分号提示";
    EXPECT_NE(enriched.find(msg), std::string::npos)
        << "应保留原始消息";
}

TEST(ErrorHintEngineCodeMatching, KnownCodeUnbalancedParen) {
    std::string msg = "parse failure";
    std::string enriched = ErrorHintEngine::enrichErrorMessage(
        msg, "unbalanced-paren", "parse", {});
    EXPECT_NE(enriched.find("括号"), std::string::npos);
}

TEST(ErrorHintEngineCodeMatching, KnownCodeDivisionByZero) {
    std::string msg = "arithmetic error";  // 子串匹配不到
    std::string enriched = ErrorHintEngine::enrichErrorMessage(
        msg, "division-by-zero", "runtime", {});
    EXPECT_NE(enriched.find("除数"), std::string::npos);
}

TEST(ErrorHintEngineCodeMatching, KnownCodeRecursionDepth) {
    std::string msg = "stack overflow";  // 子串能匹配，验证 code 优先仍工作
    std::string enriched = ErrorHintEngine::enrichErrorMessage(
        msg, "recursion-depth", "runtime", {});
    EXPECT_NE(enriched.find("无限递归"), std::string::npos);
}

TEST(ErrorHintEngineCodeMatching, UnknownCodeFallsBackToSubstring) {
    // 未知 code 应回退到子串匹配
    std::string msg = "期望 ';' (行 5, 列 20)";
    std::string enriched = ErrorHintEngine::enrichErrorMessage(
        msg, "some-unknown-code", "parse", {});
    EXPECT_NE(enriched.find("分号"), std::string::npos)
        << "未知 code 应回退子串匹配";
}

TEST(ErrorHintEngineCodeMatching, UndefinedVariableCodeFallsBackForSpelling) {
    // undefined-variable 需要从 msg 提取变量名做拼写建议，回退到 3 参版本
    std::vector<std::string> scopeVars = {"count", "counter"};
    std::string msg = "未定义的变量: coount";
    std::string enriched = ErrorHintEngine::enrichErrorMessage(
        msg, "undefined-variable", "runtime", scopeVars);
    EXPECT_NE(enriched.find("count"), std::string::npos)
        << "undefined-variable code 应回退子串匹配并返回拼写建议";
}

TEST(ErrorHintEngineCodeMatching, LocalizedMsgStillMatchesViaCode) {
    // 核心价值验证：即使 msg 是英文/未来本地化的文案（子串匹配失效），
    // code 仍能精确匹配并附加中文教学提示
    std::string msg = "Expected semicolon but found 'var'";  // 子串匹配可能失效
    std::string enriched = ErrorHintEngine::enrichErrorMessage(
        msg, "missing-semicolon", "parse", {});
    EXPECT_NE(enriched.find("分号"), std::string::npos)
        << "英文消息通过 code 仍应附加中文教学提示";
}

// ============================================================
// 测试套件 5：ErrorHintEngineTeachingMode — R117 错误消息教学模式
// ------------------------------------------------------------
// R117: 新增 teachingMarkdown 字段 + findPattern / renderTooltipHtml /
// renderTeachingMarkdown 三个 API。验证：
//   1. 每个 ErrorPattern 都有非空 teachingMarkdown
//   2. teachingMarkdown 包含根因/触发场景/修复模板关键章节
//   3. findPattern 命中已知 tag / 未命中返回 nullptr
//   4. renderTooltipHtml 返回 HTML 富文本（含标题 + 类别 + 教学）
//   5. renderTeachingMarkdown 返回原始 markdown 文本
//   6. 未命中 tag 时 renderTooltipHtml / renderTeachingMarkdown 返回空字符串
// ============================================================

TEST(ErrorHintEngineTeachingMode, AllPatternsHaveTeachingMarkdown) {
    // R117: 每个 ErrorPattern 必须有非空 teachingMarkdown 字段
    const auto& patterns = ErrorHintEngine::errorPatterns();
    EXPECT_FALSE(patterns.empty());
    for (const auto& p : patterns) {
        EXPECT_FALSE(p.teachingMarkdown.empty())
            << "teachingMarkdown 不能为空 (tag=" << p.tag << ")";
        // 教学说明应包含根因章节
        EXPECT_NE(p.teachingMarkdown.find("根因"), std::string::npos)
            << "teachingMarkdown 应包含「根因」章节 (tag=" << p.tag << ")";
        // 教学说明应包含修复模板章节
        EXPECT_NE(p.teachingMarkdown.find("修复模板"), std::string::npos)
            << "teachingMarkdown 应包含「修复模板」章节 (tag=" << p.tag << ")";
    }
}

TEST(ErrorHintEngineTeachingMode, FindPatternHitsKnownTag) {
    // findPattern 应能查到所有 errorPatterns() 中的 tag
    const auto& patterns = ErrorHintEngine::errorPatterns();
    for (const auto& p : patterns) {
        const auto* found = ErrorHintEngine::findPattern(p.tag);
        ASSERT_NE(found, nullptr) << "findPattern 应命中已知 tag: " << p.tag;
        EXPECT_EQ(found->tag, p.tag);
        EXPECT_EQ(found->title, p.title);
        EXPECT_EQ(found->teachingMarkdown, p.teachingMarkdown);
    }
}

TEST(ErrorHintEngineTeachingMode, FindPatternMissesUnknownTag) {
    // findPattern 对未知 tag 应返回 nullptr
    EXPECT_EQ(ErrorHintEngine::findPattern("nonexistent-tag"), nullptr);
    EXPECT_EQ(ErrorHintEngine::findPattern(""), nullptr);
}

TEST(ErrorHintEngineTeachingMode, RenderTooltipHtmlReturnsHtmlForKnownTag) {
    // renderTooltipHtml 应返回非空 HTML 富文本
    std::string html = ErrorHintEngine::renderTooltipHtml("missing-semicolon");
    EXPECT_FALSE(html.empty());
    // HTML 应包含 <div> 标签
    EXPECT_NE(html.find("<div"), std::string::npos);
    // HTML 应包含标题（"缺少分号"）
    EXPECT_NE(html.find("缺少分号"), std::string::npos);
    // HTML 应包含教学说明的「根因」关键词
    EXPECT_NE(html.find("根因"), std::string::npos);
}

TEST(ErrorHintEngineTeachingMode, RenderTooltipHtmlEmptyForUnknownTag) {
    // 未命中 tag 时返回空字符串（调用方回退到原始消息）
    EXPECT_TRUE(ErrorHintEngine::renderTooltipHtml("nonexistent-tag").empty());
    EXPECT_TRUE(ErrorHintEngine::renderTooltipHtml("").empty());
}

TEST(ErrorHintEngineTeachingMode, RenderTeachingMarkdownReturnsRawMarkdown) {
    // renderTeachingMarkdown 应返回原始 markdown（不含 HTML 包装）
    std::string md = ErrorHintEngine::renderTeachingMarkdown("division-by-zero");
    EXPECT_FALSE(md.empty());
    // markdown 应以 "## " 标题开头
    EXPECT_EQ(md.substr(0, 3), "## ");
    // markdown 不应包含 HTML 标签（与 renderTooltipHtml 区分）
    EXPECT_EQ(md.find("<div"), std::string::npos);
    EXPECT_EQ(md.find("<br>"), std::string::npos);
}

TEST(ErrorHintEngineTeachingMode, RenderTeachingMarkdownEmptyForUnknownTag) {
    EXPECT_TRUE(ErrorHintEngine::renderTeachingMarkdown("nonexistent-tag").empty());
    EXPECT_TRUE(ErrorHintEngine::renderTeachingMarkdown("").empty());
}

TEST(ErrorHintEngineTeachingMode, TooltipHtmlEscapesSpecialChars) {
    // renderTooltipHtml 应转义 markdown 中的 < > & 避免 HTML 注入
    // 选取一个含 < 或 > 的教学说明（recursion-depth 的 `if (n = 0)` 中有 = 但无 < >，
    // 改用 type-mismatch，其教学说明含 `var x: int = "string"`，无 < >；
    // 实际所有教学说明中 < > 出现在 `if (i >= 0 && i < len(arr))` 的 index-out-of-bounds）
    std::string html = ErrorHintEngine::renderTooltipHtml("index-out-of-bounds");
    EXPECT_FALSE(html.empty());
    // 教学说明原文含 `i < len(arr)`，转义后应变为 `i &lt; len(arr)`
    EXPECT_NE(html.find("&lt;"), std::string::npos)
        << "应将 < 转义为 &lt;";
    // 不应出现裸 < （除了 HTML 标签本身的 <）
    // 简单验证：去掉所有 HTML 标签后不应有 < 字符
    std::string stripped = html;
    auto pos = std::string::npos;
    while ((pos = stripped.find('<')) != std::string::npos) {
        auto end = stripped.find('>', pos);
        if (end == std::string::npos) break;
        stripped.erase(pos, end - pos + 1);
    }
    EXPECT_EQ(stripped.find('<'), std::string::npos)
        << "转义后正文不应有裸 < 字符";
}
