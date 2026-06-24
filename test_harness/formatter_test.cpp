// MiniLang Formatter: Bidirectional Consistency & Idempotency Test
// Pipeline: Source -> Lex -> Parse -> Run = output A
//           Source -> Lex -> Parse -> Format -> Lex -> Parse -> Run = output B
//           A must == B (consistency)
//           Format(Format(Source)) must == Format(Source) (idempotency)

#include <iostream>
#include <string>
#include <vector>

#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "ast/ASTNode.h"
#include "interpreter/Interpreter.h"
#include "interpreter/Value.h"
#include "interpreter/Environment.h"
#include "formatter/Formatter.h"
#include "Diagnostic.h"

// ─── Run code, capture print output ────────────────────────────────

static std::string runCode(const std::string& source, bool& hasError) {
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
        return "PARSE_ERR";
    }
    Interpreter interp;
    std::string captured;
    interp.setOutputCallback([&captured](const std::string& s) { captured += s + "\n"; });
    try {
        interp.execute(*ast);
    } catch (const RuntimeError& e) {
        hasError = true;
        return std::string("RT:") + e.what();
    } catch (const std::exception& e) {
        hasError = true;
        return std::string("EX:") + e.what();
    }
    hasError = false;
    return captured;
}

// ─── Format code: Source → Lex → Parse → Formatter → string ───────

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
        return "PARSE_ERR";
    }
    Formatter fmt;
    fmt.setComments(lexer.comments());  // Lexer 已将注释分离到 comments()
    hasError = false;
    return fmt.format(*ast);
}

// ─── Test case ─────────────────────────────────────────────────────

struct TestResult {
    bool consistent = true;   // output A == output B
    bool idempotent = true;   // fmt1 == fmt2
    bool fmtOk = true;        // format succeeded
    std::string detail;
};

static TestResult runTest(const std::string& source) {
    TestResult r;

    // Format pass 1
    bool fmtErr1 = false;
    std::string fmt1 = formatCode(source, fmtErr1);
    if (fmtErr1) {
        r.fmtOk = false;
        r.detail = "format1 failed: " + fmt1;
        r.consistent = false;
        r.idempotent = false;
        return r;
    }

    // Format pass 2 (idempotency)
    bool fmtErr2 = false;
    std::string fmt2 = formatCode(fmt1, fmtErr2);
    if (fmtErr2) {
        r.fmtOk = false;
        r.detail = "format2 failed: " + fmt2;
        r.idempotent = false;
        return r;
    }
    r.idempotent = (fmt1 == fmt2);

    // Run original
    bool runErrA = false;
    std::string outA = runCode(source, runErrA);

    // Run formatted
    bool runErrB = false;
    std::string outB = runCode(fmt1, runErrB);

    r.consistent = (runErrA == runErrB) && (outA == outB);

    if (!r.consistent) {
        r.detail = "consistency: errA=" + std::to_string(runErrA) + " errB=" + std::to_string(runErrB);
    }
    if (!r.idempotent) {
        if (!r.detail.empty()) r.detail += "; ";
        r.detail += "idempotency: fmt1 != fmt2";
    }

    return r;
}

// ─── Build test cases ──────────────────────────────────────────────

struct TestCase {
    std::string id;
    std::string name;
    std::string source;
};

static std::vector<TestCase> buildTests() {
    std::vector<TestCase> tests;
    auto T = [&](const char* id, const char* name, const char* src) {
        tests.push_back({id, name, src});
    };

    // ═══════ AR: Arithmetic expressions (8) ═══════
    T("AR1", "integer", "print(42);");
    T("AR2", "float", "print(3.14);");
    T("AR3", "add-sub", "var x = 10 + 5 - 3; print(x);");
    T("AR4", "mul-div", "var x = 20 * 3 / 4; print(x);");
    T("AR5", "precedence", "print(2 + 3 * 4);");
    T("AR6", "modulo", "print(17 % 5);");
    T("AR7", "unary neg", "var x = -5; print(x);");
    T("AR8", "complex arith", "var a = 1 + 2 * 3 - 4 / 2; print(a);");

    // ═══════ UN: Unary and comparison (6) ═══════
    T("UN1", "unary not", "print(!true); print(!false);");
    T("UN2", "comparison chain", "print(1 < 2); print(3 >= 3); print(4 == 4); print(5 != 6);");
    T("UN3", "logical and", "print(true and false); print(true and true);");
    T("UN4", "logical or", "print(false or true); print(false or false);");
    T("UN5", "string literal", "print(\"hello world\");");
    T("UN6", "bool and null", "print(true); print(false); print(null);");

    // ═══════ VR: Variables (6) ═══════
    T("VR1", "var decl", "var x = 42; print(x);");
    T("VR2", "reassign", "var x = 1; x = 2; print(x);");
    T("VR3", "multi var", "var a = 1; var b = 2; var c = a + b; print(c);");
    T("VR4", "string concat", "var s = \"hello\" + \" \" + \"world\"; print(s);");
    T("VR5", "shadow", "var x = 1; { var x = 99; print(x); } print(x);");
    T("VR6", "compound expr", "var x = (1 + 2) * (3 + 4); print(x);");

    // ═══════ BR: Branches (8) ═══════
    T("BR1", "if true", "if (true) { print(\"yes\"); }");
    T("BR2", "if-else", "if (false) { print(\"a\"); } else { print(\"b\"); }");
    T("BR3", "else-if", "var x = 85; if (x >= 90) { print(\"A\"); } else if (x >= 80) { print(\"B\"); } else { print(\"C\"); }");
    T("BR4", "nested if", "if (true) { if (false) { print(1); } else { print(2); } }");
    T("BR5", "dangling else", "if (false) { if (true) { print(1); } } else { print(2); }");
    T("BR6", "ternary-like", "var x = 5; if (x > 3) { print(\"big\"); } else { print(\"small\"); }");
    T("BR7", "empty if", "if (false) { } print(\"after\");");
    T("BR8", "nested if-else", "var a = 1; var b = 2; if (a == 1) { if (b == 2) { print(\"yes\"); } else { print(\"no\"); } }");

    // ═══════ LP: Loops (6) ═══════
    T("LP1", "while basic", "var i = 0; while (i < 5) { print(i); i = i + 1; }");
    T("LP2", "while sum", "var s = 0; var i = 1; while (i <= 10) { s = s + i; i = i + 1; } print(s);");
    T("LP3", "for basic", "for (var i = 0; i < 5; i = i + 1) { print(i); }");
    T("LP4", "for sum", "var s = 0; for (var i = 1; i <= 100; i = i + 1) { s = s + i; } print(s);");
    T("LP5", "nested for", "for (var i = 0; i < 3; i = i + 1) { for (var j = 0; j < 3; j = j + 1) { print(i * 10 + j); } }");
    T("LP6", "while countdown", "var n = 5; while (n > 0) { print(n); n = n - 1; } print(\"done\");");

    // ═══════ FN: Functions (8) ═══════
    T("FN1", "basic fun", "fun add(a, b) { return a + b; } print(add(3, 4));");
    T("FN2", "no return", "fun f() { var x = 1; } print(f());");
    T("FN3", "recursive", "fun fact(n) { if (n <= 1) { return 1; } return n * fact(n - 1); } print(fact(6));");
    T("FN4", "multi-param", "fun f(a, b, c) { return a + b + c; } print(f(1, 2, 3));");
    T("FN5", "nested call", "fun a(x) { return x + 1; } fun b(x) { return a(x) * 2; } print(b(5));");
    T("FN6", "void fun", "var s = 0; fun add(v) { s = s + v; } add(10); add(20); add(30); print(s);");
    T("FN7", "typed params", "fun add(int a, int b) { return a + b; } print(add(7, 8));");
    T("FN8", "mutual recursion", "fun isEven(n) { if (n == 0) { return true; } return isOdd(n - 1); } fun isOdd(n) { if (n == 0) { return false; } return isEven(n - 1); } print(isEven(6));");

    // ═══════ AY: Arrays (6) ═══════
    T("AY1", "array literal", "var a = [1, 2, 3]; print(a[0]); print(a[1]); print(a[2]);");
    T("AY2", "array len", "var a = [10, 20, 30]; print(a.len());");
    T("AY3", "array push", "var a = [1]; a.push(2); a.push(3); print(a.len());");
    T("AY4", "array iter", "var a = [1, 2, 3]; var i = 0; while (i < a.len()) { print(a[i]); i = i + 1; }");
    T("AY5", "array concat", "var a = [1, 2]; var b = [3, 4]; var c = a + b; print(c.len());");
    T("AY6", "nested array", "var a = [[1, 2], [3, 4]]; print(a[0][1]); print(a[1][0]);");

    // ═══════ DC: Dicts (4) ═══════
    T("DC1", "dict literal", "var d = {\"a\": 1, \"b\": 2}; print(d[\"a\"]); print(d[\"b\"]);");
    T("DC2", "dict methods", "var d = {\"x\": 10, \"y\": 20}; print(d.len()); print(d.has(\"x\"));");
    T("DC3", "dict set", "var d = {}; d[\"k\"] = 99; print(d[\"k\"]);");
    T("DC4", "nested dict", "var d = {\"inner\": {\"v\": 42}}; print(d[\"inner\"][\"v\"]);");

    // ═══════ CL: Classes (4) ═══════
    T("CL1", "basic class",
      "class Animal { init(n) { this.name = n; } speak() { return this.name; } }\n"
      "var a = Animal(\"cat\"); print(a.speak());");
    T("CL2", "inheritance",
      "class A { init() { this.x = 1; } get() { return this.x; } }\n"
      "class B : A { init() { super.init(); this.y = 2; } }\n"
      "var b = B(); print(b.get()); print(b.y);");
    T("CL3", "class field",
      "class P { init(x, y) { this.x = x; this.y = y; } sum() { return this.x + this.y; } }\n"
      "var p = P(3, 4); print(p.sum());");
    T("CL4", "class method call",
      "class C { init() { this.v = 0; } inc() { this.v = this.v + 1; } get() { return this.v; } }\n"
      "var c = C(); c.inc(); c.inc(); c.inc(); print(c.get());");

    // ═══════ CM: Complex expressions (4) ═══════
    T("CM1", "nested parens", "print(((1 + 2) * (3 + 4)) - 5);");
    T("CM2", "array method chain", "var a = [1, 2, 3, 4, 5]; a.push(6); print(a.len()); print(a.contains(3));");
    T("CM3", "string methods", "var s = \"hello\"; print(s.len()); print(s.upper());");
    T("CM4", "mixed expr", "var a = [10, 20, 30]; var s = 0; var i = 0; while (i < a.len()) { s = s + a[i]; i = i + 1; } print(s);");

    // ═══════ BD: Boundary tests (10) ═══════
    T("BD1", "10-level nesting",
      "{ { { { { { { { { { print(42); } } } } } } } } } }");
    T("BD2", "whitespace mess",
      "var   x  =  1  +  2  ;  print ( x ) ;");
    T("BD3", "long expression",
      "print(1 + 2 + 3 + 4 + 5 + 6 + 7 + 8 + 9 + 10 + 11 + 12 + 13 + 14 + 15);");
    T("BD4", "empty program", "");
    T("BD5", "single print", "print(1);");
    T("BD6", "multi print",
      "print(1); print(2); print(3); print(4); print(5);");
    T("BD7", "deep function chain",
      "fun a(x){return x+1;} fun b(x){return a(x)+1;} fun c(x){return b(x)+1;} "
      "fun d(x){return c(x)+1;} fun e(x){return d(x)+1;} print(e(0));");
    T("BD8", "large array",
      "var a = [1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20]; print(a.len());");
    T("BD9", "deep class hierarchy with super.init",
      "class A { init() { this.a = 1; } }\n"
      "class B : A { init() { super.init(); this.b = 2; } }\n"
      "class C : B { init() { super.init(); this.c = 3; } }\n"
      "var c = C(); print(c.a); print(c.b); print(c.c);");
    T("BD10", "messy class",
      "class   Point  {  init (  x , y  ) {  this.x = x ;  this.y = y ;  }  "
      "dist ( ) {  return this.x * this.x + this.y * this.y ;  }  }  "
      "var  p = Point ( 3 , 4 ) ; print ( p.dist ( ) ) ;");

    return tests;
}

// ─── Main ──────────────────────────────────────────────────────────

int main() {
    std::cout << "MiniLang Formatter Consistency & Idempotency Test" << std::endl;
    std::cout << "==================================================" << std::endl;
    auto tests = buildTests();
    std::cout << "Test count: " << tests.size() << std::endl << std::endl;

    int pass = 0, fail = 0, skip = 0;
    struct FailInfo {
        std::string id, name, detail;
    };
    std::vector<FailInfo> failures;

    for (auto& tc : tests) {
        std::cout << "  running " << tc.id << "..." << std::flush;
        auto r = runTest(tc.source);

        if (!r.fmtOk) {
            fail++;
            std::cout << " FAIL " << tc.id << " " << tc.name << " -- " << r.detail << std::endl;
            failures.push_back({tc.id, tc.name, r.detail});
            continue;
        }

        if (r.consistent && r.idempotent) {
            pass++;
            std::cout << " PASS " << tc.id << " " << tc.name << std::endl;
        } else {
            fail++;
            std::string d;
            if (!r.consistent) d += "CONSISTENCY_FAIL";
            if (!r.idempotent) {
                if (!d.empty()) d += " + ";
                d += "IDEMPOTENCY_FAIL";
            }
            if (!r.detail.empty()) d += ": " + r.detail;
            std::cout << " FAIL " << tc.id << " " << tc.name << " -- " << d << std::endl;
            failures.push_back({tc.id, tc.name, d});
        }
    }

    // ═══════ Summary ═══════
    int total = pass + fail + skip;
    std::cout << std::endl << "==================================================" << std::endl;
    std::cout << "RESULTS: " << pass << " passed, " << fail << " failed, "
              << skip << " skipped, " << total << " total" << std::endl;
    std::cout << "==================================================" << std::endl;

    if (!failures.empty()) {
        std::cout << std::endl << "FAILURES:" << std::endl;
        for (auto& f : failures) {
            std::cout << "  [" << f.id << "] " << f.name << ": " << f.detail << std::endl;
        }
    }

    return fail > 0 ? 1 : 0;
}
