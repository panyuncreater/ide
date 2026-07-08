// ============================================================
// Lexer 单元测试
// ------------------------------------------------------------
// 覆盖 Lexer 的所有功能点：关键字、标识符、数字字面量、字符串
// 字面量、运算符、分隔符、注释、错误处理、行列号追踪。
// ============================================================

#include <gtest/gtest.h>

#include "lexer/Lexer.h"
#include "lexer/Token.h"

// ============================================================
// 辅助函数：扫描源代码并返回除 EOF 外的 Token 列表
// ============================================================
static std::vector<Token> scanTokens(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    if (!tokens.empty() && tokens.back().type == TokenType::TK_EOF) {
        tokens.pop_back();
    }
    return tokens;
}

// ============================================================
// 示例测试（保留原有 3 个）
// ============================================================

// 测试：空字符串扫描应返回单个 EOF Token
TEST(LexerTest, EmptyInputReturnsSingleEOF) {
    Lexer lexer;
    auto tokens = lexer.scan("");

    // 空输入应只产生一个 EOF Token
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_EOF);
}

// 测试：简单标识符 "abc" 应识别为 TK_IDENTIFIER
TEST(LexerTest, SimpleIdentifier) {
    Lexer lexer;
    auto tokens = lexer.scan("abc");

    // 期望：[TK_IDENTIFIER, TK_EOF]
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_IDENTIFIER);
    EXPECT_EQ(tokens[0].lexeme, "abc");
    EXPECT_EQ(tokens[1].type, TokenType::TK_EOF);
}

// 测试：整数 "123" 应识别为 TK_INT_LIT
TEST(LexerTest, IntegerLiteral) {
    Lexer lexer;
    auto tokens = lexer.scan("123");

    // 期望：[TK_INT_LIT, TK_EOF]
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_INT_LIT);
    EXPECT_EQ(tokens[0].lexeme, "123");
    EXPECT_EQ(tokens[1].type, TokenType::TK_EOF);
}

// ============================================================
// 1. 关键字测试
// ============================================================

// 测试：控制流关键字 var, fun, if, else, while, for, return
TEST(LexerTest, Keywords_ControlFlow) {
    Lexer lexer;
    auto tokens = lexer.scan("var fun if else while for return");

    ASSERT_EQ(tokens.size(), 8u);  // 7 个关键字 + EOF
    EXPECT_EQ(tokens[0].type, TokenType::TK_VAR);
    EXPECT_EQ(tokens[1].type, TokenType::TK_FUN);
    EXPECT_EQ(tokens[2].type, TokenType::TK_IF);
    EXPECT_EQ(tokens[3].type, TokenType::TK_ELSE);
    EXPECT_EQ(tokens[4].type, TokenType::TK_WHILE);
    EXPECT_EQ(tokens[5].type, TokenType::TK_FOR);
    EXPECT_EQ(tokens[6].type, TokenType::TK_RETURN);
    EXPECT_EQ(tokens[7].type, TokenType::TK_EOF);
}

// 测试：布尔与逻辑关键字 true, false, and, or, not
TEST(LexerTest, Keywords_BooleanLogic) {
    Lexer lexer;
    auto tokens = lexer.scan("true false and or not");

    ASSERT_EQ(tokens.size(), 6u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_TRUE);
    EXPECT_EQ(tokens[1].type, TokenType::TK_FALSE);
    EXPECT_EQ(tokens[2].type, TokenType::TK_AND);
    EXPECT_EQ(tokens[3].type, TokenType::TK_OR);
    EXPECT_EQ(tokens[4].type, TokenType::TK_NOT);
}

// 测试：类型关键字 int, float, bool, string
TEST(LexerTest, Keywords_TypeKeywords) {
    Lexer lexer;
    auto tokens = lexer.scan("int float bool string");

    ASSERT_EQ(tokens.size(), 5u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_INT);
    EXPECT_EQ(tokens[1].type, TokenType::TK_FLOAT);
    EXPECT_EQ(tokens[2].type, TokenType::TK_BOOL);
    EXPECT_EQ(tokens[3].type, TokenType::TK_STRING_TYPE);
}

// 测试：类相关关键字 class, extends, super, dict, array, null
TEST(LexerTest, Keywords_ClassRelated) {
    Lexer lexer;
    auto tokens = lexer.scan("class extends super dict array null");

    ASSERT_EQ(tokens.size(), 7u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_CLASS);
    EXPECT_EQ(tokens[1].type, TokenType::TK_EXTENDS);
    EXPECT_EQ(tokens[2].type, TokenType::TK_SUPER);
    EXPECT_EQ(tokens[3].type, TokenType::TK_DICT);
    EXPECT_EQ(tokens[4].type, TokenType::TK_ARRAY);
    EXPECT_EQ(tokens[5].type, TokenType::TK_NULL);
}

// 测试：print 关键字，以及 function/func 作为 fun 的别名
TEST(LexerTest, Keywords_PrintAndFunctionAliases) {
    Lexer lexer;
    auto tokens = lexer.scan("print function func");

    ASSERT_EQ(tokens.size(), 4u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_PRINT);
    // function 和 func 都是 fun 的别名，统一映射为 TK_FUN
    EXPECT_EQ(tokens[1].type, TokenType::TK_FUN);
    EXPECT_EQ(tokens[1].lexeme, "function");
    EXPECT_EQ(tokens[2].type, TokenType::TK_FUN);
    EXPECT_EQ(tokens[2].lexeme, "func");
}

// 测试：true/false/null 关键字应携带字面量值
TEST(LexerTest, Keywords_LiteralValues) {
    Lexer lexer;
    auto tokens = lexer.scan("true false null");

    ASSERT_EQ(tokens.size(), 4u);
    // true → 布尔值 true
    EXPECT_EQ(tokens[0].type, TokenType::TK_TRUE);
    EXPECT_TRUE(tokens[0].literalIsBool());
    EXPECT_TRUE(tokens[0].literalBool());
    // false → 布尔值 false
    EXPECT_EQ(tokens[1].type, TokenType::TK_FALSE);
    EXPECT_TRUE(tokens[1].literalIsBool());
    EXPECT_FALSE(tokens[1].literalBool());
    // null → 空值
    EXPECT_EQ(tokens[2].type, TokenType::TK_NULL);
    EXPECT_TRUE(tokens[2].literalIsNull());
}

// ============================================================
// 2. 标识符测试
// ============================================================

// 测试：带下划线的标识符
TEST(LexerTest, Identifier_WithUnderscore) {
    auto tokens = scanTokens("my_var");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_IDENTIFIER);
    EXPECT_EQ(tokens[0].lexeme, "my_var");
}

// 测试：带数字的标识符（数字不在首位）
TEST(LexerTest, Identifier_WithDigits) {
    auto tokens = scanTokens("var123");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_IDENTIFIER);
    EXPECT_EQ(tokens[0].lexeme, "var123");
}

// 测试：标识符区分大小写
TEST(LexerTest, Identifier_CaseSensitive) {
    auto tokens = scanTokens("abc ABC Abc aBc");
    ASSERT_EQ(tokens.size(), 4u);
    EXPECT_EQ(tokens[0].lexeme, "abc");
    EXPECT_EQ(tokens[1].lexeme, "ABC");
    EXPECT_EQ(tokens[2].lexeme, "Abc");
    EXPECT_EQ(tokens[3].lexeme, "aBc");
    // 全部应为 TK_IDENTIFIER，不应误识别为关键字
    for (const auto& t : tokens) {
        EXPECT_EQ(t.type, TokenType::TK_IDENTIFIER);
    }
}

// 测试：单个下划线作为标识符
TEST(LexerTest, Identifier_SingleUnderscore) {
    auto tokens = scanTokens("_");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_IDENTIFIER);
    EXPECT_EQ(tokens[0].lexeme, "_");
}

// 测试：下划线开头的标识符（非双下划线）
TEST(LexerTest, Identifier_LeadingUnderscore) {
    auto tokens = scanTokens("_foo _bar123");
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_IDENTIFIER);
    EXPECT_EQ(tokens[0].lexeme, "_foo");
    EXPECT_EQ(tokens[1].type, TokenType::TK_IDENTIFIER);
    EXPECT_EQ(tokens[1].lexeme, "_bar123");
}

// 测试：双下划线前缀的标识符应报错（编译器内部保留）
TEST(LexerTest, Identifier_DoubleUnderscorePrefixIsError) {
    Lexer lexer;
    auto tokens = lexer.scan("__foo");

    // 应产生一个 TK_ERROR Token
    ASSERT_EQ(tokens.size(), 2u);  // ERROR + EOF
    EXPECT_EQ(tokens[0].type, TokenType::TK_ERROR);
    // 错误消息应包含保留前缀提示
    EXPECT_NE(tokens[0].lexeme.find("__"), std::string::npos);
    // 诊断器应报告错误
    EXPECT_TRUE(lexer.getDiagnostics().hasErrors());
}

// ============================================================
// 3. 数字字面量测试
// ============================================================

// 测试：整数字面量的值
TEST(LexerTest, Number_IntegerValue) {
    auto tokens = scanTokens("42");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_INT_LIT);
    EXPECT_EQ(tokens[0].lexeme, "42");
    ASSERT_TRUE(tokens[0].literalIsInt());
    EXPECT_EQ(tokens[0].literalInt(), 42);
}

// 测试：浮点数字面量
TEST(LexerTest, Number_Float) {
    auto tokens = scanTokens("3.14");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_FLOAT_LIT);
    EXPECT_EQ(tokens[0].lexeme, "3.14");
}

// 测试：浮点数字面量的值
TEST(LexerTest, Number_FloatValue) {
    auto tokens = scanTokens("3.14");
    ASSERT_EQ(tokens.size(), 1u);
    ASSERT_TRUE(tokens[0].literalIsFloat());
    EXPECT_DOUBLE_EQ(tokens[0].literalFloat(), 3.14);
}

// 测试：前导点浮点数 .123
TEST(LexerTest, Number_LeadingDotFloat) {
    auto tokens = scanTokens(".123");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_FLOAT_LIT);
    EXPECT_EQ(tokens[0].lexeme, ".123");
    ASSERT_TRUE(tokens[0].literalIsFloat());
    EXPECT_DOUBLE_EQ(tokens[0].literalFloat(), 0.123);
}

// 测试：科学计数法 1e5
TEST(LexerTest, Number_ScientificNotation) {
    auto tokens = scanTokens("1e5");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_FLOAT_LIT);
    EXPECT_EQ(tokens[0].lexeme, "1e5");
    ASSERT_TRUE(tokens[0].literalIsFloat());
    EXPECT_DOUBLE_EQ(tokens[0].literalFloat(), 1e5);
}

// 测试：科学计数法带负指数 3.14e-2
TEST(LexerTest, Number_ScientificNotationNegativeExponent) {
    auto tokens = scanTokens("3.14e-2");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_FLOAT_LIT);
    EXPECT_EQ(tokens[0].lexeme, "3.14e-2");
    ASSERT_TRUE(tokens[0].literalIsFloat());
    EXPECT_DOUBLE_EQ(tokens[0].literalFloat(), 3.14e-2);
}

// 测试：科学计数法带正指数和大写 E 2E+10
TEST(LexerTest, Number_ScientificNotationPositiveExponent) {
    auto tokens = scanTokens("2E+10");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_FLOAT_LIT);
    EXPECT_EQ(tokens[0].lexeme, "2E+10");
    ASSERT_TRUE(tokens[0].literalIsFloat());
    EXPECT_DOUBLE_EQ(tokens[0].literalFloat(), 2e10);
}

// 测试：整数后跟点和标识符（123.foo 应分为三个 Token）
TEST(LexerTest, Number_IntegerFollowedByDotAndIdentifier) {
    auto tokens = scanTokens("123.foo");
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_INT_LIT);
    EXPECT_EQ(tokens[0].lexeme, "123");
    EXPECT_EQ(tokens[1].type, TokenType::TK_DOT);
    EXPECT_EQ(tokens[2].type, TokenType::TK_IDENTIFIER);
    EXPECT_EQ(tokens[2].lexeme, "foo");
}

// 测试：数字后跟字母的边界情况（123abc 分为 INT_LIT 和 IDENTIFIER）
TEST(LexerTest, Number_FollowedByLetters) {
    auto tokens = scanTokens("123abc");
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_INT_LIT);
    EXPECT_EQ(tokens[0].lexeme, "123");
    EXPECT_EQ(tokens[1].type, TokenType::TK_IDENTIFIER);
    EXPECT_EQ(tokens[1].lexeme, "abc");
}

// 测试：0x 前缀不支持，应报错
TEST(LexerTest, Number_HexPrefixError) {
    Lexer lexer;
    auto tokens = lexer.scan("0x1F");

    ASSERT_EQ(tokens.size(), 2u);  // ERROR + EOF
    EXPECT_EQ(tokens[0].type, TokenType::TK_ERROR);
    EXPECT_NE(tokens[0].lexeme.find("0x1F"), std::string::npos);
    EXPECT_TRUE(lexer.getDiagnostics().hasErrors());
}

// 测试：0b 前缀不支持，应报错
TEST(LexerTest, Number_BinaryPrefixError) {
    Lexer lexer;
    auto tokens = lexer.scan("0b101");

    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_ERROR);
    EXPECT_NE(tokens[0].lexeme.find("0b101"), std::string::npos);
}

// 测试：科学计数法格式错误（e 后无数字）
TEST(LexerTest, Number_ScientificFormatError) {
    Lexer lexer;
    auto tokens = lexer.scan("1e");

    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_ERROR);
    EXPECT_NE(tokens[0].lexeme.find("科学计数法"), std::string::npos);
    EXPECT_TRUE(lexer.getDiagnostics().hasErrors());
}

// 测试：科学计数法 123.e5（整数后直接跟 .e 形式）
TEST(LexerTest, Number_ScientificWithDecimalPoint) {
    auto tokens = scanTokens("123.e5");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_FLOAT_LIT);
    EXPECT_EQ(tokens[0].lexeme, "123.e5");
    ASSERT_TRUE(tokens[0].literalIsFloat());
    EXPECT_DOUBLE_EQ(tokens[0].literalFloat(), 123e5);
}

// ============================================================
// 4. 字符串字面量测试
// ============================================================

// 测试：简单字符串
TEST(LexerTest, String_Simple) {
    auto tokens = scanTokens("\"hello\"");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_STRING_LIT);
    ASSERT_TRUE(tokens[0].literalIsString());
    EXPECT_EQ(tokens[0].literalString(), "hello");
}

// 测试：空字符串
TEST(LexerTest, String_Empty) {
    auto tokens = scanTokens("\"\"");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_STRING_LIT);
    ASSERT_TRUE(tokens[0].literalIsString());
    EXPECT_EQ(tokens[0].literalString(), "");
}

// 测试：lexeme 包含引号
TEST(LexerTest, String_LexemeIncludesQuotes) {
    auto tokens = scanTokens("\"hello\"");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].lexeme, "\"hello\"");
}

// 测试：转义字符 \n, \t, \r, \\, \"
TEST(LexerTest, String_EscapeSequences) {
    // 测试 \n 换行
    {
        auto tokens = scanTokens("\"a\\nb\"");
        ASSERT_EQ(tokens.size(), 1u);
        ASSERT_TRUE(tokens[0].literalIsString());
        EXPECT_EQ(tokens[0].literalString(), "a\nb");
    }
    // 测试 \t 制表符
    {
        auto tokens = scanTokens("\"a\\tb\"");
        ASSERT_EQ(tokens.size(), 1u);
        EXPECT_EQ(tokens[0].literalString(), "a\tb");
    }
    // 测试 \\ 反斜杠
    {
        auto tokens = scanTokens("\"a\\\\b\"");
        ASSERT_EQ(tokens.size(), 1u);
        EXPECT_EQ(tokens[0].literalString(), "a\\b");
    }
    // 测试 \" 双引号
    {
        auto tokens = scanTokens("\"a\\\"b\"");
        ASSERT_EQ(tokens.size(), 1u);
        EXPECT_EQ(tokens[0].literalString(), "a\"b");
    }
    // 测试 \r 回车
    {
        auto tokens = scanTokens("\"a\\rb\"");
        ASSERT_EQ(tokens.size(), 1u);
        EXPECT_EQ(tokens[0].literalString(), "a\rb");
    }
}

// 测试：更多转义字符 \', \0, \b, \f
TEST(LexerTest, String_AdditionalEscapeSequences) {
    // \' 单引号
    {
        auto tokens = scanTokens("\"a\\'b\"");
        ASSERT_EQ(tokens.size(), 1u);
        EXPECT_EQ(tokens[0].literalString(), "a'b");
    }
    // \0 空字符
    {
        auto tokens = scanTokens("\"a\\0b\"");
        ASSERT_EQ(tokens.size(), 1u);
        std::string expected = "a";
        expected += '\0';
        expected += "b";
        EXPECT_EQ(tokens[0].literalString(), expected);
    }
    // \b 退格
    {
        auto tokens = scanTokens("\"a\\bb\"");
        ASSERT_EQ(tokens.size(), 1u);
        EXPECT_EQ(tokens[0].literalString(), "a\bb");
    }
    // \f 换页
    {
        auto tokens = scanTokens("\"a\\fb\"");
        ASSERT_EQ(tokens.size(), 1u);
        EXPECT_EQ(tokens[0].literalString(), "a\fb");
    }
}

// AUDIT-P2 fix: \xNN 十六进制字节转义 + \uXXXX Unicode 码点转义
TEST(LexerTest, String_HexAndUnicodeEscapes) {
    // \x41 = 'A'
    {
        auto tokens = scanTokens("\"\\x41\"");
        ASSERT_EQ(tokens.size(), 1u);
        EXPECT_EQ(tokens[0].literalString(), "A");
    }
    // \x4A = 'J'
    {
        auto tokens = scanTokens("\"\\x4A\"");
        ASSERT_EQ(tokens.size(), 1u);
        EXPECT_EQ(tokens[0].literalString(), "J");
    }
    // \u0041 = 'A' (U+0041)
    {
        auto tokens = scanTokens("\"\\u0041\"");
        ASSERT_EQ(tokens.size(), 1u);
        EXPECT_EQ(tokens[0].literalString(), "A");
    }
    // \u4E2D = '中' (U+4E2D, UTF-8: E4 B8 AD)
    {
        auto tokens = scanTokens("\"\\u4E2D\"");
        ASSERT_EQ(tokens.size(), 1u);
        EXPECT_EQ(tokens[0].literalString(), "\xE4\xB8\xAD");
    }
    // \u00E9 = 'é' (U+00E9, UTF-8: C3 A9)
    {
        auto tokens = scanTokens("\"\\u00E9\"");
        ASSERT_EQ(tokens.size(), 1u);
        EXPECT_EQ(tokens[0].literalString(), "\xC3\xA9");
    }
    // 无效的 \x：第二位非十六进制
    {
        auto tokens = scanTokens("\"\\xG\"");
        ASSERT_EQ(tokens.size(), 1u);
        EXPECT_EQ(tokens[0].type, TokenType::TK_ERROR);
    }
    // 无效的 \u：第三位非十六进制
    {
        auto tokens = scanTokens("\"\\u00G1\"");
        ASSERT_EQ(tokens.size(), 1u);
        EXPECT_EQ(tokens[0].type, TokenType::TK_ERROR);
    }
}

// AUDIT-BUG-L1 fix: 未知转义字符应报错（原实现静默保留为 \x）
// AUDIT-P2 fix: \x 和 \u 现已支持为合法转义，改用 \q 作为未知转义测试用例
TEST(LexerTest, String_UnknownEscapeRejected) {
    auto tokens = scanTokens("\"a\\qb\"");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_ERROR);
    EXPECT_NE(tokens[0].lexeme.find("未知转义"), std::string::npos);
}

// 测试：未闭合字符串应产生错误
TEST(LexerTest, String_UnterminatedProducesError) {
    Lexer lexer;
    auto tokens = lexer.scan("\"hello");

    ASSERT_EQ(tokens.size(), 2u);  // ERROR + EOF
    EXPECT_EQ(tokens[0].type, TokenType::TK_ERROR);
    EXPECT_NE(tokens[0].lexeme.find("未终止"), std::string::npos);
    EXPECT_TRUE(lexer.getDiagnostics().hasErrors());
}

// 测试：反斜杠结尾的未闭合字符串
TEST(LexerTest, String_UnterminatedWithBackslashAtEnd) {
    Lexer lexer;
    auto tokens = lexer.scan("\"abc\\");

    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_ERROR);
    EXPECT_TRUE(lexer.getDiagnostics().hasErrors());
}

// ============================================================
// 5. 运算符与分隔符测试
// ============================================================

// 测试：单字符运算符 + - * / %
TEST(LexerTest, Operator_SingleChar) {
    auto tokens = scanTokens("+ - * / %");
    ASSERT_EQ(tokens.size(), 5u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_PLUS);
    EXPECT_EQ(tokens[1].type, TokenType::TK_MINUS);
    EXPECT_EQ(tokens[2].type, TokenType::TK_STAR);
    EXPECT_EQ(tokens[3].type, TokenType::TK_SLASH);
    EXPECT_EQ(tokens[4].type, TokenType::TK_PERCENT);
}

// 测试：双字符运算符 == != <= >=
TEST(LexerTest, Operator_DualChar) {
    auto tokens = scanTokens("== != <= >=");
    ASSERT_EQ(tokens.size(), 4u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_EQ);
    EXPECT_EQ(tokens[1].type, TokenType::TK_NEQ);
    EXPECT_EQ(tokens[2].type, TokenType::TK_LEQ);
    EXPECT_EQ(tokens[3].type, TokenType::TK_GEQ);
}

// 测试：单字符比较运算符 < >
TEST(LexerTest, Operator_Comparison) {
    auto tokens = scanTokens("< >");
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_LT);
    EXPECT_EQ(tokens[1].type, TokenType::TK_GT);
}

// 测试：赋值运算符 =
TEST(LexerTest, Operator_Assign) {
    auto tokens = scanTokens("=");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_ASSIGN);
}

// 测试：! 运算符（逻辑非，映射为 TK_NOT）
TEST(LexerTest, Operator_Not) {
    auto tokens = scanTokens("!");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_NOT);
}

// 测试：所有分隔符 ( ) { } ; , [ ] : .
TEST(LexerTest, Delimiters_All) {
    auto tokens = scanTokens("( ) { } ; , [ ] : .");
    ASSERT_EQ(tokens.size(), 10u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_LPAREN);
    EXPECT_EQ(tokens[1].type, TokenType::TK_RPAREN);
    EXPECT_EQ(tokens[2].type, TokenType::TK_LBRACE);
    EXPECT_EQ(tokens[3].type, TokenType::TK_RBRACE);
    EXPECT_EQ(tokens[4].type, TokenType::TK_SEMICOLON);
    EXPECT_EQ(tokens[5].type, TokenType::TK_COMMA);
    EXPECT_EQ(tokens[6].type, TokenType::TK_LBRACKET);
    EXPECT_EQ(tokens[7].type, TokenType::TK_RBRACKET);
    EXPECT_EQ(tokens[8].type, TokenType::TK_COLON);
    EXPECT_EQ(tokens[9].type, TokenType::TK_DOT);
}

// ============================================================
// 6. 注释测试
// ============================================================

// 测试：单行注释
TEST(LexerTest, Comment_SingleLine) {
    Lexer lexer;
    auto tokens = lexer.scan("// this is a comment");

    // 注释不进入主 Token 流，只有 EOF
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_EOF);
    // 注释应被收集到 comments_ 列表
    const auto& comments = lexer.comments();
    ASSERT_EQ(comments.size(), 1u);
    EXPECT_EQ(comments[0].type, TokenType::TK_LINE_COMMENT);
    // 注释文本包含 // 前缀
    EXPECT_EQ(comments[0].lexeme, "// this is a comment");
}

// 测试：注释不影响后续 Token
TEST(LexerTest, Comment_DoesNotAffectFollowingTokens) {
    auto tokens = scanTokens("// comment\nabc 123");
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_IDENTIFIER);
    EXPECT_EQ(tokens[0].lexeme, "abc");
    EXPECT_EQ(tokens[1].type, TokenType::TK_INT_LIT);
    EXPECT_EQ(tokens[1].lexeme, "123");
}

// 测试：注释在输入末尾（无换行）
TEST(LexerTest, Comment_AtEndOfInput) {
    Lexer lexer;
    auto tokens = lexer.scan("abc // trailing comment");

    ASSERT_EQ(tokens.size(), 2u);  // IDENTIFIER + EOF
    EXPECT_EQ(tokens[0].type, TokenType::TK_IDENTIFIER);
    ASSERT_EQ(lexer.comments().size(), 1u);
    EXPECT_EQ(lexer.comments()[0].lexeme, "// trailing comment");
}

// 测试：多个注释都被收集
TEST(LexerTest, Comment_MultipleCollected) {
    Lexer lexer;
    auto tokens = lexer.scan("// c1\nabc // c2");

    ASSERT_EQ(tokens.size(), 2u);  // IDENTIFIER + EOF
    EXPECT_EQ(lexer.comments().size(), 2u);
    EXPECT_EQ(lexer.comments()[0].lexeme, "// c1");
    EXPECT_EQ(lexer.comments()[1].lexeme, "// c2");
}

// ============================================================
// 7. 错误处理测试
// ============================================================

// 测试：非法字符 @ # $ 应产生错误
TEST(LexerTest, Error_IllegalCharacters) {
    Lexer lexer;
    auto tokens = lexer.scan("@ # $");

    // 3 个错误 Token + EOF
    ASSERT_EQ(tokens.size(), 4u);
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(tokens[i].type, TokenType::TK_ERROR);
    }
    EXPECT_EQ(tokens[3].type, TokenType::TK_EOF);
    EXPECT_TRUE(lexer.getDiagnostics().hasErrors());
    EXPECT_EQ(lexer.getDiagnostics().errorCount(), 3);
}

// 测试：& 和 && 应产生错误（提示使用 and 关键字）
TEST(LexerTest, Error_Ampersand) {
    // 单个 &
    {
        Lexer lexer;
        auto tokens = lexer.scan("&");
        ASSERT_EQ(tokens.size(), 2u);
        EXPECT_EQ(tokens[0].type, TokenType::TK_ERROR);
        EXPECT_NE(tokens[0].lexeme.find("and"), std::string::npos);
        EXPECT_TRUE(lexer.getDiagnostics().hasErrors());
    }
    // 双 &&
    {
        Lexer lexer;
        auto tokens = lexer.scan("&&");
        ASSERT_EQ(tokens.size(), 2u);
        EXPECT_EQ(tokens[0].type, TokenType::TK_ERROR);
        EXPECT_NE(tokens[0].lexeme.find("and"), std::string::npos);
        EXPECT_TRUE(lexer.getDiagnostics().hasErrors());
    }
}

// 测试：| 和 || 应产生错误（提示使用 or 关键字）
TEST(LexerTest, Error_Pipe) {
    // 单个 |
    {
        Lexer lexer;
        auto tokens = lexer.scan("|");
        ASSERT_EQ(tokens.size(), 2u);
        EXPECT_EQ(tokens[0].type, TokenType::TK_ERROR);
        EXPECT_NE(tokens[0].lexeme.find("or"), std::string::npos);
        EXPECT_TRUE(lexer.getDiagnostics().hasErrors());
    }
    // 双 ||
    {
        Lexer lexer;
        auto tokens = lexer.scan("||");
        ASSERT_EQ(tokens.size(), 2u);
        EXPECT_EQ(tokens[0].type, TokenType::TK_ERROR);
        EXPECT_NE(tokens[0].lexeme.find("or"), std::string::npos);
        EXPECT_TRUE(lexer.getDiagnostics().hasErrors());
    }
}

// 测试：错误 Token 仍保留在 Token 流中
TEST(LexerTest, Error_TokenStillInStream) {
    Lexer lexer;
    auto tokens = lexer.scan("@ abc");

    // ERROR + IDENTIFIER + EOF
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_ERROR);
    EXPECT_EQ(tokens[1].type, TokenType::TK_IDENTIFIER);
    EXPECT_EQ(tokens[1].lexeme, "abc");
    EXPECT_EQ(tokens[2].type, TokenType::TK_EOF);
}

// 测试：诊断信息包含正确的行号和列号
TEST(LexerTest, Error_DiagnosticHasCorrectLocation) {
    Lexer lexer;
    lexer.scan("  @");

    const auto& diags = lexer.getDiagnostics().all();
    ASSERT_EQ(diags.size(), 1u);
    EXPECT_EQ(diags[0].line, 1);
    EXPECT_EQ(diags[0].column, 3);  // 第 3 列
    EXPECT_EQ(diags[0].source, DiagSource::Lexer);
    EXPECT_TRUE(diags[0].isError());
}

// 测试：一次扫描中多个错误都被收集
TEST(LexerTest, Error_MultipleErrorsInOneScan) {
    Lexer lexer;
    auto tokens = lexer.scan("@ # $");

    // 3 个非法字符 → 3 个错误
    EXPECT_EQ(lexer.getDiagnostics().errorCount(), 3);
    EXPECT_FALSE(lexer.getDiagnostics().empty());
}

// ============================================================
// 8. 行号与列号测试
// ============================================================

// 测试：单行 Token 的列号
TEST(LexerTest, Position_SingleLineColumn) {
    auto tokens = scanTokens("abc");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].line, 1);
    EXPECT_EQ(tokens[0].column, 1);
}

// 测试：空格后的 Token 列号正确
TEST(LexerTest, Position_ColumnAfterWhitespace) {
    auto tokens = scanTokens("  abc");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].line, 1);
    EXPECT_EQ(tokens[0].column, 3);  // 第 3 列
}

// 测试：多行输入的行号递增
TEST(LexerTest, Position_MultiLineLineNumbers) {
    auto tokens = scanTokens("abc\ndef\nghi");
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].line, 1);
    EXPECT_EQ(tokens[0].lexeme, "abc");
    EXPECT_EQ(tokens[1].line, 2);
    EXPECT_EQ(tokens[1].lexeme, "def");
    EXPECT_EQ(tokens[2].line, 3);
    EXPECT_EQ(tokens[2].lexeme, "ghi");
}

// 测试：换行后列号重置为 1
TEST(LexerTest, Position_ColumnResetsAfterNewline) {
    auto tokens = scanTokens("abc\ndef");
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[1].line, 2);
    EXPECT_EQ(tokens[1].column, 1);
}

// 测试：CRLF (\r\n) 行尾被正确处理
TEST(LexerTest, Position_CRLFLineEnding) {
    auto tokens = scanTokens("abc\r\ndef");
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].line, 1);
    EXPECT_EQ(tokens[1].line, 2);
    EXPECT_EQ(tokens[1].column, 1);
}

// 测试：UTF-8 BOM 被跳过，不影响 Token 位置
TEST(LexerTest, Position_BOMSkipped) {
    std::string source;
    source += static_cast<char>(0xEF);
    source += static_cast<char>(0xBB);
    source += static_cast<char>(0xBF);
    source += "abc";

    auto tokens = scanTokens(source);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_IDENTIFIER);
    EXPECT_EQ(tokens[0].lexeme, "abc");
    EXPECT_EQ(tokens[0].line, 1);
    EXPECT_EQ(tokens[0].column, 1);  // BOM 跳过后列号从 1 开始
}

// 测试：多行字符串使用起始位置（而非结束位置）
TEST(LexerTest, Position_StringUsesStartPosition) {
    auto tokens = scanTokens("\"hello\nworld\"");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::TK_STRING_LIT);
    // 字符串起始在第 1 行第 1 列
    EXPECT_EQ(tokens[0].line, 1);
    EXPECT_EQ(tokens[0].column, 1);
}

// 测试：EOF Token 的行号正确
TEST(LexerTest, Position_EOFLineNumber) {
    Lexer lexer;
    auto tokens = lexer.scan("abc\ndef");

    ASSERT_EQ(tokens.size(), 3u);  // IDENTIFIER + IDENTIFIER + EOF
    EXPECT_EQ(tokens[2].type, TokenType::TK_EOF);
    EXPECT_EQ(tokens[2].line, 2);  // EOF 在第 2 行末尾
}

// 测试：混合 Token 序列的位置正确性
TEST(LexerTest, Position_MixedTokenSequence) {
    auto tokens = scanTokens("var x = 123;");
    ASSERT_EQ(tokens.size(), 5u);
    // var: 第 1 行第 1 列
    EXPECT_EQ(tokens[0].line, 1);
    EXPECT_EQ(tokens[0].column, 1);
    // x: 第 1 行第 5 列
    EXPECT_EQ(tokens[1].line, 1);
    EXPECT_EQ(tokens[1].column, 5);
    // =: 第 1 行第 7 列
    EXPECT_EQ(tokens[2].line, 1);
    EXPECT_EQ(tokens[2].column, 7);
    // 123: 第 1 行第 9 列
    EXPECT_EQ(tokens[3].line, 1);
    EXPECT_EQ(tokens[3].column, 9);
    // ;: 第 1 行第 12 列
    EXPECT_EQ(tokens[4].line, 1);
    EXPECT_EQ(tokens[4].column, 12);
}
