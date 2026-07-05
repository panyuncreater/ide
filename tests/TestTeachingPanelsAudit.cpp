// ============================================================
// 教学面板数据完整性测试
// ------------------------------------------------------------
// 覆盖第一波（P0-1/P0-3/P1-3）和第三波（P2-1/P2-2）新增面板的
// 数据类：
//   - BugHuntLibrary（10 道历史 Bug 题目）
//   - SyntaxProductionLibrary（10 条语法产生式）
//   - LabManualContent（8 个实验章节）
//
// 注：GUI 交互行为（按钮点击/信号连接/dock 切换）需要 Qt GUI
//     环境，不在单元测试覆盖范围。本测试只覆盖数据完整性
//     （ID 唯一性 / 关键字段非空 / 数量符合预期 / 内容可被 Lexer
//     解析为有效 Token）。
// ============================================================

#include <gtest/gtest.h>

#include "gui/BugHuntPanel.h"
#include "gui/SyntaxExplorerPanel.h"
#include "gui/LabManualPanel.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <set>
#include <string>

// ============================================================
// BugHuntLibrary — 数据完整性
// ============================================================

TEST(TeachingPanelsBugHunt, HasExpectedItemCount) {
    const auto& items = BugHuntLibrary::items();
    // 题库设计为 10 道题（BUG-CP-1/CP-2/UV-1/IR-POP-2/DEF-1/
    // MOD-1/DBG-1/REGVM-2/REPL-1/F-04）
    EXPECT_GE(items.size(), 10u);
}

TEST(TeachingPanelsBugHunt, IdsAreUnique) {
    const auto& items = BugHuntLibrary::items();
    std::set<std::string> ids;
    for (const auto& it : items) {
        EXPECT_FALSE(it.id.empty()) << "题目 id 不能为空";
        auto [_, inserted] = ids.insert(it.id);
        EXPECT_TRUE(inserted) << "题目 id 重复: " << it.id;
    }
    EXPECT_EQ(ids.size(), items.size());
}

TEST(TeachingPanelsBugHunt, CriticalFieldsNonEmpty) {
    const auto& items = BugHuntLibrary::items();
    for (const auto& it : items) {
        EXPECT_FALSE(it.title.empty()) << it.id << ": title 为空";
        EXPECT_FALSE(it.severity.empty()) << it.id << ": severity 为空";
        EXPECT_FALSE(it.category.empty()) << it.id << ": category 为空";
        EXPECT_FALSE(it.background.empty()) << it.id << ": background 为空";
        EXPECT_FALSE(it.sourceCode.empty()) << it.id << ": sourceCode 为空";
        EXPECT_FALSE(it.expectedBehavior.empty()) << it.id << ": expectedBehavior 为空";
        EXPECT_FALSE(it.buggyBehavior.empty()) << it.id << ": buggyBehavior 为空";
        EXPECT_FALSE(it.explanation.empty()) << it.id << ": explanation 为空";
        EXPECT_FALSE(it.hints.empty()) << it.id << ": hints 不能为空";
    }
}

TEST(TeachingPanelsBugHunt, SeverityIsRecognized) {
    const auto& items = BugHuntLibrary::items();
    std::set<std::string> validSeverities = {"P0", "P1", "P2"};
    for (const auto& it : items) {
        EXPECT_NE(validSeverities.find(it.severity), validSeverities.end())
            << it.id << ": severity 不在 P0/P1/P2 集合中: " << it.severity;
    }
}

TEST(TeachingPanelsBugHunt, SourceCodeIsLexable) {
    // 每道题的 sourceCode 必须能被 Lexer 解析为非空 token 流
    // （保证题目代码本身不是空字符或不可解析的乱码）
    const auto& items = BugHuntLibrary::items();
    for (const auto& it : items) {
        Lexer lexer;
        auto tokens = lexer.scan(it.sourceCode);
        // 至少应包含 EOF；非空源码应有多个 token
        EXPECT_FALSE(tokens.empty()) << it.id << ": sourceCode 无法 lex";
        // 主流 token 数（不含 EOF）应 > 0
        size_t mainStreamCount = 0;
        for (const auto& tk : tokens) {
            if (tk.type != TokenType::TK_EOF) ++mainStreamCount;
        }
        EXPECT_GT(mainStreamCount, 0u) << it.id << ": sourceCode 无有效 token";
    }
}

TEST(TeachingPanelsBugHunt, ExpectedBugIdsPresent) {
    // 验证关键历史 Bug 题目都已被收录
    const auto& items = BugHuntLibrary::items();
    std::set<std::string> ids;
    for (const auto& it : items) ids.insert(it.id);
    EXPECT_NE(ids.find("BUG-CP-1"), ids.end());      // 常量池去重
    EXPECT_NE(ids.find("BUG-CP-2"), ids.end());      // 嵌套索引栈泄漏
    EXPECT_NE(ids.find("BUG-UV-1"), ids.end());      // 三层闭包
    EXPECT_NE(ids.find("BUG-IR-POP-2"), ids.end());  // IR if 未 POP
}

// ============================================================
// SyntaxProductionLibrary — 数据完整性
// ============================================================

TEST(TeachingPanelsSyntax, HasExpectedItemCount) {
    const auto& items = SyntaxProductionLibrary::items();
    // 设计为 10 条产生式：var-decl / if-stmt / while-stmt / for-stmt /
    // fun-decl / class-decl / try-stmt / import-stmt / string-interp /
    // data-structures / operators
    EXPECT_GE(items.size(), 10u);
}

TEST(TeachingPanelsSyntax, NamesAreUnique) {
    const auto& items = SyntaxProductionLibrary::items();
    std::set<std::string> names;
    for (const auto& p : items) {
        EXPECT_FALSE(p.name.empty()) << "产生式 name 为空";
        auto [_, inserted] = names.insert(p.name);
        EXPECT_TRUE(inserted) << "产生式 name 重复: " << p.name;
    }
    EXPECT_EQ(names.size(), items.size());
}

TEST(TeachingPanelsSyntax, CriticalFieldsNonEmpty) {
    const auto& items = SyntaxProductionLibrary::items();
    for (const auto& p : items) {
        EXPECT_FALSE(p.title.empty()) << p.name << ": title 为空";
        EXPECT_FALSE(p.ebnf.empty()) << p.name << ": ebnf 为空";
        EXPECT_FALSE(p.description.empty()) << p.name << ": description 为空";
        EXPECT_FALSE(p.sampleCode.empty()) << p.name << ": sampleCode 为空";
    }
}

TEST(TeachingPanelsSyntax, SampleCodeIsLexable) {
    const auto& items = SyntaxProductionLibrary::items();
    for (const auto& p : items) {
        Lexer lexer;
        auto tokens = lexer.scan(p.sampleCode);
        EXPECT_FALSE(tokens.empty()) << p.name << ": sampleCode 无法 lex";
        size_t mainStreamCount = 0;
        for (const auto& tk : tokens) {
            if (tk.type != TokenType::TK_EOF) ++mainStreamCount;
        }
        EXPECT_GT(mainStreamCount, 0u) << p.name << ": sampleCode 无有效 token";
    }
}

TEST(TeachingPanelsSyntax, ExpectedProductionsPresent) {
    const auto& items = SyntaxProductionLibrary::items();
    std::set<std::string> names;
    for (const auto& p : items) names.insert(p.name);
    EXPECT_NE(names.find("var-decl"), names.end());
    EXPECT_NE(names.find("if-stmt"), names.end());
    EXPECT_NE(names.find("while-stmt"), names.end());
    EXPECT_NE(names.find("for-stmt"), names.end());
    EXPECT_NE(names.find("fun-decl"), names.end());
    EXPECT_NE(names.find("class-decl"), names.end());
    EXPECT_NE(names.find("try-stmt"), names.end());
    EXPECT_NE(names.find("import-stmt"), names.end());
}

// ============================================================
// LabManualContent — 数据完整性
// ============================================================

TEST(TeachingPanelsLabManual, HasExpectedChapterCount) {
    const auto& chs = LabManualContent::chapters();
    // 设计为 8 个实验章节
    EXPECT_EQ(chs.size(), 8u);
}

TEST(TeachingPanelsLabManual, ChapterIdsAreUnique) {
    const auto& chs = LabManualContent::chapters();
    std::set<std::string> ids;
    for (const auto& ch : chs) {
        EXPECT_FALSE(ch.id.empty()) << "章节 id 为空";
        auto [_, inserted] = ids.insert(ch.id);
        EXPECT_TRUE(inserted) << "章节 id 重复: " << ch.id;
    }
    EXPECT_EQ(ids.size(), chs.size());
}

TEST(TeachingPanelsLabManual, CriticalFieldsNonEmpty) {
    const auto& chs = LabManualContent::chapters();
    for (const auto& ch : chs) {
        EXPECT_FALSE(ch.title.empty()) << ch.id << ": title 为空";
        EXPECT_FALSE(ch.markdown.empty()) << ch.id << ": markdown 为空";
        EXPECT_FALSE(ch.sampleCode.empty()) << ch.id << ": sampleCode 为空";
    }
}

TEST(TeachingPanelsLabManual, ChapterIdsFollowNamingConvention) {
    // 章节应遵循 lab-NN 命名约定
    const auto& chs = LabManualContent::chapters();
    for (const auto& ch : chs) {
        EXPECT_EQ(ch.id.substr(0, 4), "lab-")
            << ch.id << ": id 不以 'lab-' 开头";
    }
}

TEST(TeachingPanelsLabManual, SampleCodeIsLexable) {
    const auto& chs = LabManualContent::chapters();
    for (const auto& ch : chs) {
        Lexer lexer;
        auto tokens = lexer.scan(ch.sampleCode);
        EXPECT_FALSE(tokens.empty()) << ch.id << ": sampleCode 无法 lex";
        size_t mainStreamCount = 0;
        for (const auto& tk : tokens) {
            if (tk.type != TokenType::TK_EOF) ++mainStreamCount;
        }
        EXPECT_GT(mainStreamCount, 0u) << ch.id << ": sampleCode 无有效 token";
    }
}

TEST(TeachingPanelsLabManual, MarkdownHasExpectedSections) {
    // 每章 markdown 应至少包含 "目标" 与 "实验步骤" 两节
    const auto& chs = LabManualContent::chapters();
    for (const auto& ch : chs) {
        EXPECT_NE(ch.markdown.find("目标"), std::string::npos)
            << ch.id << ": markdown 缺少 '目标' 节";
        EXPECT_NE(ch.markdown.find("实验步骤"), std::string::npos)
            << ch.id << ": markdown 缺少 '实验步骤' 节";
    }
}
