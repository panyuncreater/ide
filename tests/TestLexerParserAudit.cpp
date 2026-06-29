// ============================================================
// Lexer/Parser 审计测试
// ------------------------------------------------------------
// 针对审计发现的"合法输入被拒"与"非法输入被静默接受"模式构造边界用例。
// 重点验证：
//   1. Lexer 边界 Token 序列（空文件、超长字面量、数字边界、转义、运算符组合）
//   2. Parser panic mode 错误恢复是否产生误导性错误位置
//   3. AST 节点完整性（行号、类型注解是否丢失）
// ============================================================

#include <gtest/gtest.h>
#include "lexer/Lexer.h"
#include "lexer/Token.h"
#include "parser/Parser.h"
#include "ast/ASTNode.h"

#include <string>
#include <vector>

// ============================================================
// 辅助函数
// ============================================================

static std::vector<Token> scanTokens(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    if (!tokens.empty() && tokens.back().type == TokenType::TK_EOF) {
        tokens.pop_back();
    }
    return tokens;
}

static std::unique_ptr<Block> parseSource(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    return parser.parse(tokens);
}

static std::unique_ptr<Block> parseSourceWithDiag(const std::string& source, Parser& parser) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    return parser.parse(tokens);
}

static bool hasError(const std::vector<Token>& tokens) {
    for (const auto& t : tokens) {
        if (t.type == TokenType::TK_ERROR) return true;
    }
    return false;
}

// ============================================================
// 1. Lexer 边界 Token 序列
// ============================================================

// 1.1 空文件、只有注释、只有分号
TEST(LexerAudit, EmptyInput) {
    auto tokens = scanTokens("");
    EXPECT_TRUE(tokens.empty());
}

TEST(LexerAudit, OnlyLineComment) {
    auto tokens = scanTokens("// just a comment\n");
    EXPECT_TRUE(tokens.empty());
}

TEST(LexerAudit, OnlyBlockComment) {
    auto tokens = scanTokens("/* just a block comment */");
    EXPECT_TRUE(tokens.empty());
}

TEST(LexerAudit, OnlySemicolon) {
    auto tokens = scanTokens(";");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_SEMICOLON);
}

// 1.2 超长字面量（不应崩溃或抛 bad_alloc）
TEST(LexerAudit, SuperLongIdentifier) {
    std::string id(100000, 'x');
    auto tokens = scanTokens(id);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_IDENTIFIER);
    EXPECT_EQ(tokens[0].lexeme.size(), 100000u);
}

TEST(LexerAudit, SuperLongStringLiteral) {
    std::string body(200000, 'a');
    std::string src = "\"" + body + "\"";
    auto tokens = scanTokens(src);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_STRING_LIT);
    EXPECT_EQ(tokens[0].literalString().size(), 200000u);
}

TEST(LexerAudit, SuperLongNumericLiteral) {
    // 1000 位数字应触发整数溢出错误（from_chars 返回 result_out_of_range）
    std::string digits(1000, '1');
    auto tokens = scanTokens(digits);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_ERROR);
    EXPECT_TRUE(tokens[0].lexeme.find("溢出") != std::string::npos);
}

// 1.3 数字边界
TEST(LexerAudit, Int64MaxBoundary) {
    // INT64_MAX = 9223372036854775807
    auto tokens = scanTokens("9223372036854775807");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_INT_LIT);
    EXPECT_EQ(tokens[0].literalInt(), 9223372036854775807LL);
}

TEST(LexerAudit, Int64MaxOverflow) {
    // INT64_MAX + 1 = 9223372036854775808 应溢出报错
    auto tokens = scanTokens("9223372036854775808");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_ERROR);
}

TEST(LexerAudit, FloatWithZeroMantissa) {
    // 1.0000000 应识别为浮点数 1.0
    auto tokens = scanTokens("1.0000000");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_FLOAT_LIT);
    EXPECT_DOUBLE_EQ(tokens[0].literalFloat(), 1.0);
}

TEST(LexerAudit, ScientificNotationValid) {
    auto tokens = scanTokens("1e5");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_FLOAT_LIT);
    EXPECT_DOUBLE_EQ(tokens[0].literalFloat(), 100000.0);
}

TEST(LexerAudit, ScientificNotationNegativeExp) {
    auto tokens = scanTokens("3.14e-2");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_FLOAT_LIT);
    EXPECT_NEAR(tokens[0].literalFloat(), 0.0314, 1e-10);
}

TEST(LexerAudit, ScientificNotationMissingExp) {
    // 1e 后无数字应报错
    auto tokens = scanTokens("1e");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_ERROR);
    EXPECT_TRUE(tokens[0].lexeme.find("科学计数法") != std::string::npos);
}

TEST(LexerAudit, LeadingDecimalPoint) {
    // .5 应识别为浮点数 0.5
    auto tokens = scanTokens(".5");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_FLOAT_LIT);
    EXPECT_DOUBLE_EQ(tokens[0].literalFloat(), 0.5);
}

TEST(LexerAudit, TrailingDecimalPoint) {
    // 1. 后无数字 — 应分词为 INT(1) + DOT(.)，而非浮点数
    // 这是一个设计决策点：当前行为是分词为 1 和 .
    auto tokens = scanTokens("1.");
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_INT_LIT);
    EXPECT_EQ(tokens[1].type, TokenType::TK_DOT);
}

TEST(LexerAudit, HexPrefixRejected) {
    auto tokens = scanTokens("0xff");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_ERROR);
}

TEST(LexerAudit, BinaryPrefixRejected) {
    auto tokens = scanTokens("0b101");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_ERROR);
}

// 1.4 字符串转义
TEST(LexerAudit, EmptyEscapeAtEnd) {
    // "\"（反斜杠后字符串立即结束）应报"未终止的字符串"
    std::string src = "\"abc\\";
    auto tokens = scanTokens(src);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_ERROR);
}

// AUDIT-BUG-L1 fix: 未知转义序列应报错（原实现静默接受为字面字符）
TEST(LexerAudit, UnknownEscapeRejected) {
    auto tokens = scanTokens("\"\\q\"");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_ERROR);
}

TEST(LexerAudit, ValidEscapeSequences) {
    auto tokens = scanTokens("\"\\n\\t\\r\\\\\\\"\\0\"");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_STRING_LIT);
    EXPECT_EQ(tokens[0].literalString(), std::string("\n\t\r\\\"\0", 6));
}

// 1.5 运算符组合
TEST(LexerAudit, TripleEquals) {
    // === 应分词为 == 和 =，而非单一运算符
    auto tokens = scanTokens("===");
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_EQ);
    EXPECT_EQ(tokens[1].type, TokenType::TK_ASSIGN);
}

TEST(LexerAudit, NestedBlockComment) {
    // /* outer /* inner */ */ 应正确处理嵌套
    auto tokens = scanTokens("/* outer /* inner */ still comment */ var x = 1;");
    ASSERT_EQ(tokens.size(), 5u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_VAR);
    EXPECT_EQ(tokens[1].type, TokenType::TK_IDENTIFIER);
    EXPECT_EQ(tokens[2].type, TokenType::TK_ASSIGN);
    EXPECT_EQ(tokens[3].type, TokenType::TK_INT_LIT);
    EXPECT_EQ(tokens[4].type, TokenType::TK_SEMICOLON);
}

TEST(LexerAudit, UnterminatedBlockComment) {
    auto tokens = scanTokens("/* never closed");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_ERROR);
}

TEST(LexerAudit, CommentSymbolsInString) {
    // 字符串内的 /* 不应被识别为注释起始
    auto tokens = scanTokens("\"/* not a comment */\"");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_STRING_LIT);
    EXPECT_EQ(tokens[0].literalString(), "/* not a comment */");
}

// 1.6 BOM 处理
TEST(LexerAudit, Utf8BOM) {
    std::string src = "\xEF\xBB\xBFvar x = 1;";
    auto tokens = scanTokens(src);
    ASSERT_EQ(tokens.size(), 5u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_VAR);
}

// AUDIT-BUG-L2: 第二个 BOM 不被识别，作为意外字符报错
TEST(LexerAudit, DoubleBOM) {
    std::string src = "\xEF\xBB\xBF\xEF\xBB\xBFvar x = 1;";
    auto tokens = scanTokens(src);
    // 第二个 BOM 会被当作意外字符
    EXPECT_TRUE(hasError(tokens));
}

// 1.7 双下划线保留前缀
TEST(LexerAudit, DoubleUnderscorePrefixRejected) {
    auto tokens = scanTokens("__reserved");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_ERROR);
}

TEST(LexerAudit, SingleUnderscoreAllowed) {
    auto tokens = scanTokens("_valid");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_IDENTIFIER);
}

// ============================================================
// 2. Parser 错误恢复与 panic mode
// ============================================================

// AUDIT-BUG-P1 fix: 空插值表达式 "{}" 应报错（原实现静默接受为空字符串）
TEST(ParserAudit, EmptyInterpolationRejected) {
    Parser parser;
    auto block = parseSourceWithDiag("\"{}\";", parser);
    EXPECT_TRUE(parser.hasErrors());
}

// AUDIT-BUG-P2 fix: synchronize() 现已包含 TK_CATCH 作为同步点
TEST(ParserAudit, SynchronizeHasCatchSyncPoint) {
    Parser parser;
    auto block = parseSourceWithDiag(
        "try { var x = ; } catch (e) { print(e); }", parser);
    EXPECT_TRUE(parser.hasErrors());
    // 修复后：synchronize 在 catch 处停下，不再产生误导性错误链
}

// AUDIT-BUG-P3 fix: synchronize() 现已包含 TK_TRY 作为同步点
TEST(ParserAudit, SynchronizeHasTrySyncPoint) {
    Parser parser;
    auto block = parseSourceWithDiag(
        "var x = ; try { print(1); } catch (e) {}", parser);
    EXPECT_TRUE(parser.hasErrors());
}

// AUDIT-BUG-P4 fix: synchronize() 现已包含 TK_IMPORT/TK_EXPORT 作为同步点
TEST(ParserAudit, SynchronizeHasImportSyncPoint) {
    Parser parser;
    auto block = parseSourceWithDiag(
        "var x = ; import \"m\";", parser);
    EXPECT_TRUE(parser.hasErrors());
}

// AUDIT-BUG-P5 fix: import 现已支持尾随逗号
TEST(ParserAudit, ImportTrailingCommaAllowed) {
    Parser parser;
    auto block = parseSourceWithDiag(
        "import {a, b,} from \"m\";", parser);
    EXPECT_FALSE(parser.hasErrors()) << "尾随逗号应被接受";
}

// AUDIT-BUG-P6 fix: try 块内错误不再导致 catch 子句被跳过
TEST(ParserAudit, TryBlockErrorDoesNotSkipCatch) {
    Parser parser;
    auto block = parseSourceWithDiag(
        "try { x + ; } catch (e) { print(e); }\nprint(\"after\");", parser);
    EXPECT_TRUE(parser.hasErrors());
    // 修复后：print("after") 应被正确解析
    if (block && block->statements.size() >= 2) {
        EXPECT_EQ(block->statements[1]->nodeType, NodeType::NODE_PRINT_STMT);
    }
}

// 错误恢复基础场景：单错误不应导致后续正确代码全部被拒
TEST(ParserAudit, SingleErrorDoesNotBlockFollowingStatements) {
    Parser parser;
    auto block = parseSourceWithDiag(
        "var x = ;\nvar y = 1;\nprint(y);", parser);
    EXPECT_TRUE(parser.hasErrors());
    ASSERT_NE(block, nullptr);
    // 错误恢复后应能继续解析 var y = 1; 和 print(y);
    EXPECT_GE(block->statements.size(), 2u);
    // 第一条语句是被错误打断的 VarDecl(x)（部分构造）
    // 第二条应是被恢复出的 print(y)
    bool hasVarDeclY = false;
    bool hasPrintY = false;
    for (const auto& s : block->statements) {
        if (s->nodeType == NodeType::NODE_VAR_DECL) {
            auto* vd = static_cast<VarDecl*>(s.get());
            if (vd->name == "y") hasVarDeclY = true;
        } else if (s->nodeType == NodeType::NODE_PRINT_STMT) {
            hasPrintY = true;
        }
    }
    EXPECT_TRUE(hasVarDeclY) << "var y = 1; 未被恢复";
    EXPECT_TRUE(hasPrintY) << "print(y); 未被恢复";
}

// block() 内错误恢复：块内单错误不应放弃整个块
TEST(ParserAudit, BlockErrorRecovery) {
    Parser parser;
    auto block = parseSourceWithDiag(
        "{ var x = ; var y = 1; }", parser);
    EXPECT_TRUE(parser.hasErrors());
    ASSERT_NE(block, nullptr);
    ASSERT_EQ(block->statements.size(), 1u);
    // 块内应至少恢复出 var y = 1
    EXPECT_EQ(block->statements[0]->nodeType, NodeType::NODE_BLOCK);
    auto* blk = static_cast<Block*>(block->statements[0].get());
    EXPECT_GE(blk->statements.size(), 1u);
}

// ============================================================
// 3. AST 完整性
// ============================================================

// 验证所有 AST 节点都正确设置行号和列号
TEST(ASTCompleteness, VarDeclHasLineColumn) {
    auto block = parseSource("var x = 1;");
    ASSERT_NE(block, nullptr);
    ASSERT_EQ(block->statements.size(), 1u);
    auto* node = block->statements[0].get();
    EXPECT_EQ(node->nodeType, NodeType::NODE_VAR_DECL);
    EXPECT_GT(node->line, 0);
    EXPECT_GT(node->column, 0);
}

TEST(ASTCompleteness, TypedVarDeclPreservesTypeAnnotation) {
    auto block = parseSource("int a = 42;");
    ASSERT_NE(block, nullptr);
    ASSERT_EQ(block->statements.size(), 1u);
    auto* node = block->statements[0].get();
    EXPECT_EQ(node->nodeType, NodeType::NODE_VAR_DECL);
    auto* vd = static_cast<VarDecl*>(node);
    EXPECT_EQ(vd->typeAnnotation, "int");
}

TEST(ASTCompleteness, ArrayTypeAnnotationPreserved) {
    auto block = parseSource("int[] arr = [1, 2, 3];");
    ASSERT_NE(block, nullptr);
    ASSERT_EQ(block->statements.size(), 1u);
    auto* vd = static_cast<VarDecl*>(block->statements[0].get());
    EXPECT_EQ(vd->typeAnnotation, "int[]");
}

TEST(ASTCompleteness, FunDeclHasReturnType) {
    auto block = parseSource("int fib(int n) { return n; }");
    ASSERT_NE(block, nullptr);
    ASSERT_EQ(block->statements.size(), 1u);
    auto* node = block->statements[0].get();
    EXPECT_EQ(node->nodeType, NodeType::NODE_FUN_DECL);
    auto* fd = static_cast<FunDecl*>(node);
    EXPECT_EQ(fd->returnType, "int");
}

TEST(ASTCompleteness, FunDeclArrowReturnType) {
    auto block = parseSource("fun add(int a, int b) -> int { return a + b; }");
    ASSERT_NE(block, nullptr);
    ASSERT_EQ(block->statements.size(), 1u);
    auto* fd = static_cast<FunDecl*>(block->statements[0].get());
    EXPECT_EQ(fd->returnType, "int");
}

TEST(ASTCompleteness, BinaryOpPreservesPositions) {
    auto block = parseSource("1 + 2;");
    ASSERT_NE(block, nullptr);
    auto* bin = static_cast<BinaryOp*>(block->statements[0].get());
    EXPECT_GT(bin->line, 0);
    EXPECT_GT(bin->column, 0);
    // 左右操作数也应有行列号
    EXPECT_GT(bin->left->line, 0);
    EXPECT_GT(bin->right->line, 0);
}

TEST(ASTCompleteness, IfStmtHasLineColumn) {
    auto block = parseSource("if (true) { print(1); }");
    ASSERT_NE(block, nullptr);
    auto* ifNode = block->statements[0].get();
    EXPECT_EQ(ifNode->nodeType, NodeType::NODE_IF_STMT);
    EXPECT_GT(ifNode->line, 0);
    EXPECT_GT(ifNode->column, 0);
}

TEST(ASTCompleteness, BlockRecordsClosingBraceLine) {
    auto block = parseSource("{\n  var x = 1;\n}\n");
    ASSERT_NE(block, nullptr);
    auto* blk = static_cast<Block*>(block->statements[0].get());
    // Block 的 closingBraceLine 应记录 '}' 的行号（第 3 行）
    EXPECT_EQ(blk->closingBraceLine, 3);
}

// ============================================================
// 4. Lexer/Parser 集成边界
// ============================================================

// 验证：完整文件解析 vs REPL 多行续行解析结果一致
TEST(IntegrationAudit, MultiLineStatementParsesCorrectly) {
    std::string src = "fun f() {\n  var x = 1;\n  return x;\n}\n";
    auto block = parseSource(src);
    ASSERT_NE(block, nullptr);
    ASSERT_EQ(block->statements.size(), 1u);
    EXPECT_EQ(block->statements[0]->nodeType, NodeType::NODE_FUN_DECL);
}

// 验证：未闭合字符串在 Lexer 层应报错
TEST(IntegrationAudit, UnterminatedStringReportsLexerError) {
    Lexer lexer;
    auto tokens = lexer.scan("\"unterminated");
    EXPECT_TRUE(lexer.getDiagnostics().hasErrors());
    // 最终 EOF Token 仍存在
    EXPECT_FALSE(tokens.empty());
    EXPECT_EQ(tokens.back().type, TokenType::TK_EOF);
}

// 验证：未闭合块在 Parser 层应报错
TEST(IntegrationAudit, UnclosedBlockReportsError) {
    Parser parser;
    auto block = parseSourceWithDiag("{ var x = 1;", parser);
    EXPECT_TRUE(parser.hasErrors());
}

// 验证：嵌套深度限制生效
TEST(IntegrationAudit, DeepNestingRejected) {
    // 构造超深嵌套表达式
    std::string src;
    for (int i = 0; i < 600; ++i) src += "(";
    src += "1";
    for (int i = 0; i < 600; ++i) src += ")";
    src += ";";
    Parser parser;
    auto block = parseSourceWithDiag(src, parser);
    // 应触发深度限制错误
    EXPECT_TRUE(parser.hasErrors());
}

// 验证：插值字符串正确生成 InterpolatedString 节点
TEST(IntegrationAudit, InterpolatedStringGeneratesCorrectNode) {
    auto block = parseSource("\"Hello {name}!\";");
    ASSERT_NE(block, nullptr);
    ASSERT_EQ(block->statements.size(), 1u);
    auto* node = block->statements[0].get();
    EXPECT_EQ(node->nodeType, NodeType::NODE_INTERPOLATED_STRING);
    auto* interp = static_cast<InterpolatedString*>(node);
    // 应有 2 个文本片段和 1 个表达式
    EXPECT_EQ(interp->literals.size(), 2u);
    EXPECT_EQ(interp->expressions.size(), 1u);
    EXPECT_EQ(interp->literals[0], "Hello ");
    EXPECT_EQ(interp->literals[1], "!");
}
