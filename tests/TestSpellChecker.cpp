// ============================================================
// SpellChecker 单元测试
// ------------------------------------------------------------
// 验证智能拼写纠错工具的三个核心函数：
//   1. editDistance    — Levenshtein 编辑距离计算
//   2. findClosest     — 候选词最近匹配查找
//   3. suggestSuffix   — 建议消息构建
//
// 覆盖场景：
//   - 相同/空串/单字符/对称性/完全不同串
//   - 精确匹配/距离内匹配/超距无匹配/空候选/长度惩罚/平局偏好
//   - 中文建议格式/无建议/建议等于原词
// ============================================================

#include <gtest/gtest.h>
#include "common/SpellChecker.h"

#include <string>
#include <vector>

// ============================================================
// 1. editDistance — 编辑距离计算
// ============================================================

// 相同字符串距离为 0
TEST(SpellCheckerTest, EditDistance_IdenticalStrings) {
    EXPECT_EQ(SpellChecker::editDistance("hello", "hello"), 0);
    EXPECT_EQ(SpellChecker::editDistance("", ""), 0);
    EXPECT_EQ(SpellChecker::editDistance("a", "a"), 0);
    EXPECT_EQ(SpellChecker::editDistance("MiniLang", "MiniLang"), 0);
}

// 单次插入（目标比源多一个字符）
TEST(SpellCheckerTest, EditDistance_SingleInsertion) {
    EXPECT_EQ(SpellChecker::editDistance("abc", "abcd"), 1);
    EXPECT_EQ(SpellChecker::editDistance("abc", "xabc"), 1);
    EXPECT_EQ(SpellChecker::editDistance("abc", "abxc"), 1);
}

// 单次删除（目标比源少一个字符）
TEST(SpellCheckerTest, EditDistance_SingleDeletion) {
    EXPECT_EQ(SpellChecker::editDistance("abcd", "abc"), 1);
    EXPECT_EQ(SpellChecker::editDistance("xabc", "abc"), 1);
    EXPECT_EQ(SpellChecker::editDistance("abxc", "abc"), 1);
}

// 单次替换（等长串中一个字符不同）
TEST(SpellCheckerTest, EditDistance_SingleSubstitution) {
    EXPECT_EQ(SpellChecker::editDistance("abc", "axc"), 1);
    EXPECT_EQ(SpellChecker::editDistance("int", "ibt"), 1);
    EXPECT_EQ(SpellChecker::editDistance("cat", "bat"), 1);
}

// 空串与非空串：距离等于非空串长度
TEST(SpellCheckerTest, EditDistance_EmptyStrings) {
    EXPECT_EQ(SpellChecker::editDistance("", "abc"), 3);
    EXPECT_EQ(SpellChecker::editDistance("abc", ""), 3);
    EXPECT_EQ(SpellChecker::editDistance("", "a"), 1);
    EXPECT_EQ(SpellChecker::editDistance("a", ""), 1);
}

// 对称性：editDistance(a, b) == editDistance(b, a)
TEST(SpellCheckerTest, EditDistance_SymmetricProperty) {
    EXPECT_EQ(SpellChecker::editDistance("kitten", "sitting"),
              SpellChecker::editDistance("sitting", "kitten"));
    EXPECT_EQ(SpellChecker::editDistance("abc", "xyz"),
              SpellChecker::editDistance("xyz", "abc"));
    EXPECT_EQ(SpellChecker::editDistance("", "hello"),
              SpellChecker::editDistance("hello", ""));
    EXPECT_EQ(SpellChecker::editDistance("int", "float"),
              SpellChecker::editDistance("float", "int"));
}

// 完全不同的字符串：距离等于较长串长度
TEST(SpellCheckerTest, EditDistance_CompletelyDifferent) {
    EXPECT_EQ(SpellChecker::editDistance("abc", "xyz"), 3);
    EXPECT_EQ(SpellChecker::editDistance("ab", "xyz"), 3);
    EXPECT_EQ(SpellChecker::editDistance("a", "b"), 1);
}

// 经典案例：kitten → sitting = 3（替换 k→s, 替换 e→i, 插入 g）
TEST(SpellCheckerTest, EditDistance_ClassicExample) {
    EXPECT_EQ(SpellChecker::editDistance("kitten", "sitting"), 3);
}

// 多字节字符（UTF-8 按字节计算距离）
// 注意：editDistance 操作的是 string_view（字节序列），中文 UTF-8 每字符 3 字节
TEST(SpellCheckerTest, EditDistance_MultiByteCharacters) {
    // 两个相同的中文字符串距离为 0
    EXPECT_EQ(SpellChecker::editDistance("\xe4\xbd\xa0\xe5\xa5\xbd", "\xe4\xbd\xa0\xe5\xa5\xbd"), 0);
    // 空串与中文字符串：距离 = 字节数
    EXPECT_EQ(SpellChecker::editDistance("", "\xe4\xbd\xa0"), 3); // "你" = 3 字节
}

// ============================================================
// 2. findClosest — 最近候选词查找
// ============================================================

// 精确匹配（misspelled 本身就在候选中）：编辑距离为 0，返回该词本身
// 注意：findClosest 不排除精确匹配，由 suggestSuffix 层过滤
TEST(SpellCheckerTest, FindClosest_ExactMatch) {
    std::vector<std::string> candidates = {"int", "float", "bool"};
    // "int" 在候选中，距离 0，直接返回
    EXPECT_EQ(SpellChecker::findClosest("int", candidates), "int");
}

// 距离内的近似匹配
TEST(SpellCheckerTest, FindClosest_CloseMatchWithinDistance) {
    std::vector<std::string> candidates = {"int", "float", "bool", "string"};
    // "inr" 与 "int" 距离 1（替换 r→t）
    EXPECT_EQ(SpellChecker::findClosest("inr", candidates), "int");
    // "flaot" 与 "float" 距离 2（转置相当于 2 次替换）
    EXPECT_EQ(SpellChecker::findClosest("flaot", candidates), "float");
    // "boul" 与 "bool" 距离 1（替换 u→o）
    EXPECT_EQ(SpellChecker::findClosest("boul", candidates), "bool");
}

// 超出 maxDistance 的候选不推荐
TEST(SpellCheckerTest, FindClosest_NoMatchBeyondMaxDistance) {
    std::vector<std::string> candidates = {"int", "float", "bool"};
    // "zzzzzzz" 与所有候选距离远超 2
    EXPECT_EQ(SpellChecker::findClosest("zzzzzzz", candidates), "");
    // 自定义 maxDistance=0，距离 1 的也不推荐
    EXPECT_EQ(SpellChecker::findClosest("inr", candidates, 0), "");
}

// 空候选列表返回空
TEST(SpellCheckerTest, FindClosest_EmptyCandidates) {
    std::vector<std::string> empty;
    EXPECT_EQ(SpellChecker::findClosest("hello", empty), "");
}

// 空 misspelled 返回空
TEST(SpellCheckerTest, FindClosest_EmptyMisspelled) {
    std::vector<std::string> candidates = {"int", "float"};
    EXPECT_EQ(SpellChecker::findClosest("", candidates), "");
}

// 单字符 misspelled 不纠错（特殊规则：size <= 1 直接返回空）
TEST(SpellCheckerTest, FindClosest_SingleCharReturnsEmpty) {
    std::vector<std::string> candidates = {"int", "if", "is"};
    EXPECT_EQ(SpellChecker::findClosest("i", candidates), "");
    EXPECT_EQ(SpellChecker::findClosest("x", candidates), "");
}

// 长度惩罚：候选词与 misspelled 长度差异过大时施加额外惩罚
TEST(SpellCheckerTest, FindClosest_LengthPenalty) {
    std::vector<std::string> candidates = {"function", "if"};
    // "it" 与 "if" 距离 1（替换 t→f），长度差 0，无惩罚
    // "it" 与 "function" 距离 7（1 替换 + 6 插入），长度差 6 > 2，惩罚 +4 → 总距离 11
    // 应选 "if"
    EXPECT_EQ(SpellChecker::findClosest("it", candidates), "if");
}

// 长度惩罚使超长候选被排除
TEST(SpellCheckerTest, FindClosest_LengthPenaltyExcludesLongCandidate) {
    std::vector<std::string> candidates = {"abcdefghij"};
    // "ab" 与 "abcdefghij"：编辑距离 8（插入 8 字符），长度差 8 > 2，惩罚 +6 → 总 14
    // 远超 maxDistance=2，不推荐
    EXPECT_EQ(SpellChecker::findClosest("ab", candidates), "");
}

// 平局时偏好长度相同的候选
TEST(SpellCheckerTest, FindClosest_TieBreakingPrefersSameLength) {
    // 构造两个候选与 misspelled 编辑距离相同，但长度不同
    // "abc" vs "ab"（距离 1：删除 c）和 "axc"（距离 1：替换 b→x）
    // 两者距离都是 1，但 "axc" 长度与 "abc" 相同，应优先选择
    std::vector<std::string> candidates = {"ab", "axc"};
    EXPECT_EQ(SpellChecker::findClosest("abc", candidates), "axc");
}

// 自定义 maxDistance 参数
TEST(SpellCheckerTest, FindClosest_CustomMaxDistance) {
    std::vector<std::string> candidates = {"abcdef"};
    // "abcxyz" 与 "abcdef" 距离 3（替换 x→d, y→e, z→f）
    // maxDistance=2 时不推荐
    EXPECT_EQ(SpellChecker::findClosest("abcxyz", candidates, 2), "");
    // maxDistance=3 时推荐
    EXPECT_EQ(SpellChecker::findClosest("abcxyz", candidates, 3), "abcdef");
}

// ============================================================
// 3. suggestSuffix — 建议消息构建
// ============================================================

// 有建议时返回格式化中文字符串："，你是不是想写\"xxx\"？"
TEST(SpellCheckerTest, SuggestSuffix_FormattedChineseSuggestion) {
    std::vector<std::string> candidates = {"int", "float", "bool"};
    // "inr" → 建议 "int"
    std::string suffix = SpellChecker::suggestSuffix("inr", candidates);
    // 期望格式："，你是不是想写\"int\"？"
    std::string expected = "\xef\xbc\x8c\xe4\xbd\xa0\xe6\x98\xaf\xe4\xb8\x8d\xe6\x98\xaf"
                           "\xe6\x83\xb3\xe5\x86\x99\"int\"\xef\xbc\x9f";
    EXPECT_EQ(suffix, expected);
}

// 无建议时返回空串
TEST(SpellCheckerTest, SuggestSuffix_EmptyWhenNoSuggestion) {
    std::vector<std::string> candidates = {"int", "float", "bool"};
    // "zzzzzzz" 无近似候选
    EXPECT_EQ(SpellChecker::suggestSuffix("zzzzzzz", candidates), "");
    // 空候选
    std::vector<std::string> empty;
    EXPECT_EQ(SpellChecker::suggestSuffix("hello", empty), "");
}

// 建议等于 misspelled 本身时返回空串（精确匹配不需要纠错）
TEST(SpellCheckerTest, SuggestSuffix_EmptyWhenSuggestionEqualsMisspelled) {
    std::vector<std::string> candidates = {"int", "float", "bool"};
    // "int" 精确匹配候选 "int"，findClosest 返回 "int"，
    // 但 suggestion == misspelled，suggestSuffix 返回空
    EXPECT_EQ(SpellChecker::suggestSuffix("int", candidates), "");
}

// 建议消息格式验证：以中文逗号开头，以中文问号结尾，包含建议词
TEST(SpellCheckerTest, SuggestSuffix_FormatStructure) {
    std::vector<std::string> candidates = {"while", "for", "if"};
    std::string suffix = SpellChecker::suggestSuffix("whilr", candidates);
    ASSERT_FALSE(suffix.empty());
    // 应以 "，" 开头（UTF-8: \xef\xbc\x8c）
    EXPECT_EQ(suffix.substr(0, 3), "\xef\xbc\x8c");
    // 应以 "？" 结尾（UTF-8: \xef\xbc\x9f）
    EXPECT_EQ(suffix.substr(suffix.size() - 3), "\xef\xbc\x9f");
    // 应包含建议词 "while"
    EXPECT_NE(suffix.find("while"), std::string::npos);
}
