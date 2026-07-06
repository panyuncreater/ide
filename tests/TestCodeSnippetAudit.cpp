// ============================================================
// TestCodeSnippetAudit.cpp — 代码模板 / Snippets 系统测试
// ------------------------------------------------------------
// 功能 13（MINILANG_IDE_IMPROVEMENT_PLAN.md）：覆盖 CodeSnippetEngine
// 的模板库数据完整性与引擎行为正确性。
//
// 两个测试套件：
//   1. CodeSnippetLibraryAudit.*（5 用例）—— 模板库数据完整性
//   2. CodeSnippetEngineAudit.*（5 用例）—— 引擎行为正确性
//
// 注：CodeSnippetEngine.cpp 为独立编译单元（仅依赖 Qt6::Core 的 QString），
//     不依赖 CodeEditor / IdeController / Qt6::Widgets，已在
//     tests/CMakeLists.txt 中显式加入测试目标。
// ============================================================

#include <gtest/gtest.h>

#include "gui/CodeSnippetEngine.h"

#include <cctype>
#include <set>
#include <string>

// ============================================================
// 辅助函数：从模板文本中提取所有 ${N:default} 的编号 N
// ------------------------------------------------------------
// 返回有序集合（std::set 自动去重排序），用于检查编号连续性。
// ============================================================
static std::set<int> extractPlaceholderNumbers(const std::string& tpl) {
    std::set<int> numbers;
    size_t pos = 0;
    while ((pos = tpl.find("${", pos)) != std::string::npos) {
        size_t close = tpl.find('}', pos);
        if (close == std::string::npos) break;
        const std::string inner = tpl.substr(pos + 2, close - pos - 2);
        const size_t colon = inner.find(':');
        const std::string numStr =
            (colon != std::string::npos) ? inner.substr(0, colon) : inner;
        if (!numStr.empty()) {
            bool allDigits = true;
            for (char c : numStr) {
                if (!std::isdigit(static_cast<unsigned char>(c))) {
                    allDigits = false;
                    break;
                }
            }
            if (allDigits) {
                numbers.insert(std::stoi(numStr));
            }
        }
        pos = close + 1;
    }
    return numbers;
}

// ============================================================
// 套件 1：CodeSnippetLibraryAudit —— 模板库数据完整性
// ============================================================

// 用例 1：模板数量 >= 7（fun/class/for/if/while/try/print）
TEST(CodeSnippetLibraryAudit, HasAtLeastSevenSnippets) {
    const auto& all = CodeSnippetEngine::snippets();
    EXPECT_GE(all.size(), 7u)
        << "代码模板数量应 >= 7，实际: " << all.size();
}

// 用例 2：触发词唯一（避免同一关键字对应多个模板导致展开歧义）
TEST(CodeSnippetLibraryAudit, TriggersAreUnique) {
    const auto& all = CodeSnippetEngine::snippets();
    std::set<std::string> triggers;
    for (const auto& snip : all) {
        EXPECT_FALSE(snip.trigger.empty()) << "触发词不能为空";
        auto [it, inserted] = triggers.insert(snip.trigger);
        EXPECT_TRUE(inserted) << "触发词重复: " << snip.trigger;
    }
    EXPECT_EQ(triggers.size(), all.size())
        << "触发词集合大小应等于模板数量";
}

// 用例 3：7 个必需触发词齐全
TEST(CodeSnippetLibraryAudit, CriticalTriggersPresent) {
    const auto& all = CodeSnippetEngine::snippets();
    std::set<std::string> triggers;
    for (const auto& snip : all) {
        triggers.insert(snip.trigger);
    }
    // 功能 13 规范要求 7 个基础模板
    const std::set<std::string> required = {
        "fun", "class", "for", "if", "while", "try", "print"
    };
    for (const auto& t : required) {
        EXPECT_NE(triggers.find(t), triggers.end())
            << "缺少必需触发词: " << t;
    }
}

// 用例 4：每个模板至少含一个 ${N:...} 占位符
TEST(CodeSnippetLibraryAudit, EveryTemplateHasAtLeastOnePlaceholder) {
    const auto& all = CodeSnippetEngine::snippets();
    for (const auto& snip : all) {
        EXPECT_NE(snip.templateText.find("${"), std::string::npos)
            << "模板 [" << snip.trigger << "] 缺少占位符起始符 ${";
        EXPECT_NE(snip.templateText.find(':'), std::string::npos)
            << "模板 [" << snip.trigger << "] 缺少占位符分隔符 :";
        EXPECT_NE(snip.templateText.find('}'), std::string::npos)
            << "模板 [" << snip.trigger << "] 缺少占位符结束符 }";
    }
}

// 用例 5：占位符编号从 1 开始连续（1,2,3,... 无跳号）
TEST(CodeSnippetLibraryAudit, PlaceholderNumbersAreContiguous) {
    const auto& all = CodeSnippetEngine::snippets();
    for (const auto& snip : all) {
        const auto numbers = extractPlaceholderNumbers(snip.templateText);
        EXPECT_FALSE(numbers.empty())
            << "模板 [" << snip.trigger << "] 无有效占位符编号";
        int expected = 1;
        for (int n : numbers) {
            EXPECT_EQ(n, expected)
                << "模板 [" << snip.trigger << "] 占位符编号不连续："
                << "期望 " << expected << " 实际 " << n;
            ++expected;
        }
    }
}

// ============================================================
// 套件 2：CodeSnippetEngineAudit —— 引擎行为正确性
// ============================================================

// 用例 6：matchTrigger 命中已知触发词
TEST(CodeSnippetEngineAudit, MatchTriggerHitsKnownTrigger) {
    // 光标前仅有触发词（位于文档开头）
    const CodeSnippet* snip = CodeSnippetEngine::matchTrigger(QString("fun"));
    ASSERT_NE(snip, nullptr);
    EXPECT_EQ(snip->trigger, "fun");

    // 前方有其他代码 + 换行 + 触发词
    snip = CodeSnippetEngine::matchTrigger(QString("var x = 1;\nclass"));
    ASSERT_NE(snip, nullptr);
    EXPECT_EQ(snip->trigger, "class");

    // 前方有其他代码 + 空格 + 触发词
    snip = CodeSnippetEngine::matchTrigger(QString("var x = 1; if"));
    ASSERT_NE(snip, nullptr);
    EXPECT_EQ(snip->trigger, "if");

    // 触发词后无尾随空白
    snip = CodeSnippetEngine::matchTrigger(QString("while"));
    ASSERT_NE(snip, nullptr);
    EXPECT_EQ(snip->trigger, "while");
}

// 用例 7：matchTrigger 对空/未知/前缀冲突词返回 nullptr
TEST(CodeSnippetEngineAudit, MatchTriggerReturnsNullForEmptyOrUnknown) {
    // 空字符串
    EXPECT_EQ(CodeSnippetEngine::matchTrigger(QString()), nullptr);
    EXPECT_EQ(CodeSnippetEngine::matchTrigger(QString("")), nullptr);

    // 全是空白
    EXPECT_EQ(CodeSnippetEngine::matchTrigger(QString("   ")), nullptr);
    EXPECT_EQ(CodeSnippetEngine::matchTrigger(QString("\n\t  ")), nullptr);

    // 未知触发词
    EXPECT_EQ(CodeSnippetEngine::matchTrigger(QString("xyz")), nullptr);
    EXPECT_EQ(CodeSnippetEngine::matchTrigger(QString("foobar")), nullptr);

    // 触发词是其他词的前缀（如 "func" 不应匹配 "fun"）
    EXPECT_EQ(CodeSnippetEngine::matchTrigger(QString("func")), nullptr);

    // 触发词前方无空白粘连（如 "myfun" 不应匹配 "fun"）
    EXPECT_EQ(CodeSnippetEngine::matchTrigger(QString("myfun")), nullptr);

    // 前方有代码但末尾是未知词
    EXPECT_EQ(CodeSnippetEngine::matchTrigger(QString("var x = 1; unknownword")), nullptr);
}

// 用例 8：expand 返回正确文本（占位符已替换为默认值，无残留 ${）
TEST(CodeSnippetEngineAudit, ExpandReturnsCorrectText) {
    const CodeSnippet* snip = CodeSnippetEngine::matchTrigger(QString("fun"));
    ASSERT_NE(snip, nullptr);

    const auto expansion = CodeSnippetEngine::expand(*snip);
    // 展开后应保留模板的语法结构
    EXPECT_TRUE(expansion.text.contains(QString("fun ")));
    EXPECT_TRUE(expansion.text.contains(QString("(")));
    EXPECT_TRUE(expansion.text.contains(QString(")")));
    EXPECT_TRUE(expansion.text.contains(QString("{")));
    EXPECT_TRUE(expansion.text.contains(QString("}")));
    // 占位符已被替换为默认值，不应再包含 ${ 或 }
    EXPECT_FALSE(expansion.text.contains(QString("${")))
        << "展开后仍含未解析占位符: " << expansion.text.toStdString();
    // 应包含默认值文本（fun 模板含 "函数名"/"参数列表"/"返回类型"/"函数体"）
    EXPECT_TRUE(expansion.text.contains(QString("函数名")))
        << "展开后缺少默认值'函数名': " << expansion.text.toStdString();
}

// 用例 9：expand 占位符范围正确（位置与文本对应）
TEST(CodeSnippetEngineAudit, ExpandPlaceholderRangesCorrect) {
    const CodeSnippet* snip = CodeSnippetEngine::matchTrigger(QString("print"));
    ASSERT_NE(snip, nullptr);

    // print 模板: "print(${1:表达式});"
    // 展开后: "print(表达式);"
    //         0123456789...
    // "print(" = 6 字符，"表达式" = 3 字符（UTF-16 每个 CJK 字符占 1 个 QChar）
    const auto expansion = CodeSnippetEngine::expand(*snip);
    ASSERT_EQ(expansion.placeholderRanges.size(), 1u)
        << "print 模板应有 1 个占位符";

    const auto& range = expansion.placeholderRanges[0];
    EXPECT_EQ(range.first, 6) << "占位符起点应在 'print(' 之后（位置 6）";
    EXPECT_EQ(range.second, 9) << "占位符终点应在 '表达式' 之后（位置 9）";

    // 验证 range 对应的文本确实是默认值
    const QString placeholderText =
        expansion.text.mid(range.first, range.second - range.first);
    EXPECT_EQ(placeholderText.toStdString(), "表达式");

    // primaryCursorPos 应指向第一个占位符起点
    EXPECT_EQ(expansion.primaryCursorPos, range.first);
}

// 用例 10：多占位符编号一致（for 模板的 ${1:i} 出现 4 次）
TEST(CodeSnippetEngineAudit, ExpandMultiplePlaceholdersWithSameNumber) {
    // for 模板:
    //   "for (var ${1:i} = 0; ${1:i} < ${2:10}; ${1:i} = ${1:i} + 1) {\n    ${3:// 循环体}\n}"
    // ${1:i} 出现 4 次（同步占位符），${2:10} 1 次，${3:// 循环体} 1 次
    const CodeSnippet* snip = CodeSnippetEngine::matchTrigger(QString("for"));
    ASSERT_NE(snip, nullptr);

    const auto expansion = CodeSnippetEngine::expand(*snip);
    // 总占位符数: 4 + 1 + 1 = 6
    EXPECT_EQ(expansion.placeholderRanges.size(), 6u)
        << "for 模板应有 6 个占位符（${1:i}×4 + ${2:10}×1 + ${3:// 循环体}×1）";

    // 统计各默认值出现的次数
    int count_i = 0, count_10 = 0, count_body = 0;
    for (const auto& [start, end] : expansion.placeholderRanges) {
        const QString text = expansion.text.mid(start, end - start);
        if (text == QString("i")) {
            ++count_i;
        } else if (text == QString("10")) {
            ++count_10;
        } else if (text == QString("// 循环体")) {
            ++count_body;
        }
    }
    EXPECT_EQ(count_i, 4) << "${1:i} 应出现 4 次（同步占位符）";
    EXPECT_EQ(count_10, 1) << "${2:10} 应出现 1 次";
    EXPECT_EQ(count_body, 1) << "${3:// 循环体} 应出现 1 次";

    // primaryCursorPos 应指向第一个占位符起点（第一个 ${1:i}）
    EXPECT_EQ(expansion.primaryCursorPos, expansion.placeholderRanges.front().first);

    // 所有占位符范围应在 [0, text.length()) 内
    for (const auto& [start, end] : expansion.placeholderRanges) {
        EXPECT_GE(start, 0);
        EXPECT_LE(end, expansion.text.length());
        EXPECT_LE(start, end);
    }
}
