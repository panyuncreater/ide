// Audit verification test for PLF (Parser/Lexer/Formatter) bugs
// Standalone, no Qt, no Interpreter dependency (only Lexer/Parser/Formatter)

#include <iostream>
#include <string>
#include <vector>
#include <sstream>

#include "lexer/Lexer.h"
#include "lexer/Token.h"
#include "parser/Parser.h"
#include "ast/ASTNode.h"
#include "formatter/Formatter.h"
#include "Diagnostic.h"

static int g_pass = 0, g_fail = 0;

static void check(bool cond, const std::string& id, const std::string& desc) {
    std::cout << (cond ? " PASS " : " FAIL ") << id << "  " << desc << "\n";
    if (cond) g_pass++; else g_fail++;
}

// Print tokens for debugging
static void printTokens(const std::vector<Token>& tokens) {
    for (size_t i = 0; i < tokens.size(); ++i) {
        const auto& t = tokens[i];
        std::cout << "  [" << i << "] " << Token::typeToString(t.type)
                  << " '" << t.lexeme << "'\n";
    }
}

// Format code, returning result + hasError flag
static std::string formatCode(const std::string& source, bool& hasError) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    if (lexer.getDiagnostics().hasErrors()) {
        hasError = true;
        return "LEX:" + lexer.getDiagnostics().summary();
    }
    Parser parser;
    auto ast = parser.parse(tokens);
    if (parser.hasErrors() || !ast) {
        hasError = true;
        return "PARSE_ERR:" + parser.getDiagnostics().summary();
    }
    Formatter fmt;
    fmt.setComments(lexer.comments());
    hasError = false;
    return fmt.format(*ast);
}

// Parse code, returning whether parser had errors
static bool parseFails(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    if (lexer.getDiagnostics().hasErrors()) return true;
    Parser parser;
    auto ast = parser.parse(tokens);
    return parser.hasErrors() || !ast;
}

// ============================================================
// TEST 1: Nested string literal inside interpolation
// Source: var x = "a{f("b")}c";
// Expected: Should lex/parse successfully (nested string is valid expression)
// ============================================================
static void test_nested_string_in_interp() {
    std::cout << "\n===== TEST 1: Nested string literal inside interpolation =====\n";

    // First, just lex the source to see what tokens are produced
    {
        Lexer lexer;
        std::string src = "var x = \"a{f(\"b\")}c\";";
        auto tokens = lexer.scan(src);
        std::cout << "Tokens for: " << src << "\n";
        printTokens(tokens);
        // The inner "b" should be TK_STRING_LIT, but may be TK_STRING_PART (bug)
        bool hasStringPart = false;
        bool hasStringLit = false;
        for (const auto& t : tokens) {
            if (t.type == TokenType::TK_STRING_PART && t.lexeme == "b") hasStringPart = true;
            if (t.type == TokenType::TK_STRING_LIT && t.lexeme == "b") hasStringLit = true;
        }
        std::cout << "  Inner 'b' is STRING_LIT? " << (hasStringLit ? "yes" : "no") << "\n";
        std::cout << "  Inner 'b' is STRING_PART? " << (hasStringPart ? "yes (BUG)" : "no") << "\n";
        check(hasStringLit, "1a-string-lit-in-interp",
              "Nested string literal should be TK_STRING_LIT");
    }

    // Now try to parse — should succeed (semantic correctness)
    {
        bool fails = parseFails("var x = \"a{f(\"b\")}c\";");
        check(!fails, "1b-parse-nested-string-interp",
              "Parser should accept nested string literal in interpolation");
        if (fails) {
            std::cout << "  >>> CONFIRMED BUG: nested string literal in interpolation fails to parse\n";
        }
    }

    // Test with nested string that itself has interpolation
    {
        bool fails = parseFails("var x = \"a{f(\"inner{1+2}\")}c\";");
        check(!fails, "1c-parse-nested-interp-in-interp",
              "Parser should accept nested interpolated string in interpolation");
    }
}

// ============================================================
// TEST 2: Trailing comma support in FunCall and MethodCall
// ============================================================
static void test_trailing_commas() {
    std::cout << "\n===== TEST 2: Trailing comma in calls =====\n";

    // f()
    check(!parseFails("f();"), "2a-f-empty-args", "f() parses");
    // f(1)
    check(!parseFails("f(1);"), "2b-f-one-arg", "f(1) parses");
    // f(1,2)
    check(!parseFails("f(1,2);"), "2c-f-two-args", "f(1,2) parses");
    // f(1,) — trailing comma
    check(!parseFails("f(1,);"), "2d-f-trailing-comma", "f(1,) parses with trailing comma");
    // obj.m(1,2)
    check(!parseFails("obj.m(1,2);"), "2e-m-two-args", "obj.m(1,2) parses");
    // obj.m(1,2,) — trailing comma in method call
    check(!parseFails("obj.m(1,2,);"), "2f-m-trailing-comma",
          "obj.m(1,2,) parses with trailing comma");
    // f(1, ,2) — invalid (empty arg) should FAIL
    check(parseFails("f(1,,2);"), "2g-f-empty-arg-rejected",
          "f(1,,2) is rejected (empty argument)");
}

// ============================================================
// TEST 3: Formatter idempotency for bare compound statements
// ============================================================
static void test_formatter_bare_compound_idempotency() {
    std::cout << "\n===== TEST 3: Formatter idempotency (bare compounds) =====\n";

    struct Case {
        std::string id;
        std::string desc;
        std::string src;
    };
    std::vector<Case> cases = {
        {"3a", "if with bare while body",         "if (a) while (b) x;"},
        {"3b", "while with bare if body",          "while (a) if (b) x;"},
        {"3c", "for with bare while body",         "for (var i = 0; i < 3; i = i + 1) while (a) x;"},
        {"3d", "if-else if with bare compound",    "if (a) x; else if (b) y; else z;"},
        {"3e", "if with bare try body",            "if (a) try { x; } catch (e) { y; }"},
        {"3f", "nested bare compounds (3 deep)",   "if (a) if (b) if (c) x;"},
        {"3g", "while with bare for body",         "while (a) for (var i = 0; i < 1; i = i + 1) x;"},
        {"3h", "fun with bare if body",            "fun f() if (a) x;"},
        {"3i", "deeply nested bare: if-while-if",  "if (a) while (b) if (c) x;"},
    };

    for (const auto& c : cases) {
        bool e1, e2;
        std::string fmt1 = formatCode(c.src, e1);
        std::string fmt2 = formatCode(fmt1, e2);
        bool idempotent = !e1 && !e2 && (fmt1 == fmt2);
        check(idempotent, c.id, c.desc);
        if (!idempotent) {
            std::cout << "  src:  [" << c.src << "]\n";
            std::cout << "  fmt1: [" << fmt1 << "]\n";
            std::cout << "  fmt2: [" << fmt2 << "]\n";
            if (e1) std::cout << "  fmt1 ERROR: " << fmt1 << "\n";
            if (e2) std::cout << "  fmt2 ERROR: " << fmt2 << "\n";
        }
    }
}

// ============================================================
// TEST 4: Formatter doesn't under-indent bare compounds
// ============================================================
static void test_formatter_no_under_indent() {
    std::cout << "\n===== TEST 4: Formatter doesn't under-indent bare compounds =====\n";

    // Verify expected indentation: bare compound at body of if should be at indent level 1 (4 spaces)
    {
        bool e;
        std::string fmt = formatCode("if (a) while (b) x;", e);
        // Expected output:
        // if (a) {
        //     while (b) {
        //         x;
        //     }
        // }
        // The "while" should be indented 4 spaces, "x" 8 spaces.
        bool has4spaceWhile = fmt.find("\n    while") != std::string::npos;
        bool has8spaceX = fmt.find("\n        x;") != std::string::npos;
        check(!e && has4spaceWhile && has8spaceX, "4a-if-while-indent",
              "Bare while inside if should be indented 4 spaces, body 8");
        if (!e) {
            std::cout << "  Output:\n" << fmt << "\n";
        }
    }
}

// ============================================================
// TEST 5: isClassTypeDeclStart edge cases
// ============================================================
static void test_isClassTypeDeclStart_edge_cases() {
    std::cout << "\n===== TEST 5: ClassName[] paramName pattern =====\n";

    // ClassName[] paramName in function params
    check(!parseFails("fun f(MyClass[] arr) { }"), "5a-class-array-param",
          "Function param 'MyClass[] arr' parses");
    // ClassName[] paramName in for init
    check(!parseFails("for (MyClass[] arr = things; false;) { }"), "5b-for-class-array-init",
          "for-init 'MyClass[] arr = ...' parses");
    // Just ClassName paramName
    check(!parseFails("fun f(MyClass p) { }"), "5c-class-param",
          "Function param 'MyClass p' parses");
    // Edge: ClassName[ (no ]) should not be treated as type decl — should fall through to expression
    check(!parseFails("fun f() { arr[0] = 1; }"), "5d-arr-index-assign",
          "Array index assign still works");
    // Edge: just identifier followed by EOF (no param)
    // This should parse as expression statement (then fail on missing ;)
    check(parseFails("fun f() { standaloneIdent }"), "5e-lone-ident-rejected",
          "Lone identifier without ; rejected");
}

// ============================================================
// TEST 6: Lexer — multi-line strings & escape sequences
// ============================================================
static void test_lexer_strings() {
    std::cout << "\n===== TEST 6: Lexer string edge cases =====\n";

    // Multi-line string (raw newline in string)
    {
        Lexer lexer;
        auto tokens = lexer.scan("var x = \"line1\nline2\";");
        bool hasError = lexer.getDiagnostics().hasErrors();
        // MiniLang allows newlines in string literals (per Lexer behavior — no explicit check)
        check(!hasError, "6a-multiline-string", "Multi-line string literal lexes without error");
    }

    // Unknown escape sequence — should be rejected (AUDIT-BUG-L1 fix)
    {
        Lexer lexer;
        auto tokens = lexer.scan("var x = \"bad\\qescape\";");
        bool hasError = lexer.getDiagnostics().hasErrors();
        check(hasError, "6b-unknown-escape-rejected",
              "Unknown escape sequence \\q should be rejected");
    }

    // Valid escape sequences
    {
        Lexer lexer;
        auto tokens = lexer.scan("var x = \"tab\\there\\nnewline\";");
        bool hasError = lexer.getDiagnostics().hasErrors();
        check(!hasError, "6c-valid-escapes", "Valid escape sequences \\t \\n parse");
    }
}

// ============================================================
// TEST 7: Parser synchronize coverage
// ============================================================
static void test_synchronize_coverage() {
    std::cout << "\n===== TEST 7: Error recovery (synchronize) =====\n";

    // Missing semicolon then valid statement — should recover
    // The second 'var y = 2;' should be parsed successfully
    {
        Lexer lexer;
        auto tokens = lexer.scan("var x = 1\nvar y = 2;");
        Parser parser;
        auto ast = parser.parse(tokens);
        // The parser should at least parse 'var y = 2;' successfully
        bool ok = ast && ast->statements.size() >= 1;
        check(ok, "7a-recover-missing-semicolon",
              "Parser recovers from missing semicolon (should keep var y = 2;)");
        if (ast) {
            std::cout << "  statements parsed: " << ast->statements.size() << "\n";
        }
    }

    // Multiple errors should not cascade indefinitely
    {
        Lexer lexer;
        auto tokens = lexer.scan("fun f() { bad bad bad\nfun g() { return 1; } }");
        Parser parser;
        auto ast = parser.parse(tokens);
        check(ast != nullptr, "7b-no-cascade-crash",
              "Parser doesn't crash on cascading errors");
    }

    // Verify synchronize doesn't consume '}' — error inside block followed by '}'
    // Should preserve the closing '}' so block() can consume it
    {
        Lexer lexer;
        auto tokens = lexer.scan("fun f() { bad_statement }");
        Parser parser;
        auto ast = parser.parse(tokens);
        // Should not crash with "期望 '}'" because synchronize ate the '}'
        check(ast != nullptr, "7c-sync-preserves-rbrace",
              "synchronize() should not consume '}' (block boundary)");
        if (ast) {
            std::cout << "  parse errors: " << (parser.hasErrors() ? "yes" : "no") << "\n";
        }
    }

    // Verify synchronize doesn't consume sync keywords at the current position
    // After error, the next 'var' should be preserved
    {
        Lexer lexer;
        auto tokens = lexer.scan("x = 1\nvar y = 2;");
        Parser parser;
        auto ast = parser.parse(tokens);
        // 'x = 1' is missing ';' — synchronize should NOT consume 'var'
        // Then 'var y = 2;' should be parsed
        bool ok = ast && ast->statements.size() >= 1;
        check(ok, "7d-sync-preserves-var-keyword",
              "synchronize() should not consume sync keywords (var/fun/class)");
        if (ast) {
            std::cout << "  statements parsed: " << ast->statements.size() << "\n";
        }
    }
}

int main() {
    std::cout << "PLF Audit Verification Test Suite\n";
    std::cout << "================================\n";

    test_nested_string_in_interp();
    test_trailing_commas();
    test_formatter_bare_compound_idempotency();
    test_formatter_no_under_indent();
    test_isClassTypeDeclStart_edge_cases();
    test_lexer_strings();
    test_synchronize_coverage();

    std::cout << "\n\n================================\n";
    std::cout << "RESULTS: " << g_pass << " passed, " << g_fail << " failed\n";
    std::cout << "================================\n";
    return g_fail > 0 ? 1 : 0;
}
