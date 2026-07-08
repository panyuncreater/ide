// formatter_test.cpp — MiniLang 格式化器「双向一致性 + 幂等性」测试
// 覆盖模块: formatter/Formatter 在各类语法结构上的输出稳定性。
// 验证目标1（一致性）: 源码经「解析→格式化→重解析→执行」得到的输出 B，
//                      必须等于源码直接「解析→执行」得到的输出 A。
// 验证目标2（幂等性）: 对格式化结果再格式化一次，应与第一次结果完全相同。
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

    // ═══════ AR: 算术表达式（8 例）— 验证整数/浮点/加减乘除/取模/一元负号等算术结构格式化后语义不变 ═══════
    // 验证: 整数字面量格式化后输出不变
    T("AR1", "integer", "print(42);");
    // 验证: 浮点字面量格式化后输出不变
    T("AR2", "float", "print(3.14);");
    // 验证: 加减混合表达式格式化后求值一致
    T("AR3", "add-sub", "var x = 10 + 5 - 3; print(x);");
    // 验证: 乘除混合表达式格式化后求值一致
    T("AR4", "mul-div", "var x = 20 * 3 / 4; print(x);");
    // 验证: 运算符优先级在格式化后保持不变
    T("AR5", "precedence", "print(2 + 3 * 4);");
    // 验证: 取模运算格式化后输出不变
    T("AR6", "modulo", "print(17 % 5);");
    // 验证: 一元负号格式化后语义不变
    T("AR7", "unary neg", "var x = -5; print(x);");
    // 验证: 复杂算术表达式格式化后结果一致
    T("AR8", "complex arith", "var a = 1 + 2 * 3 - 4 / 2; print(a);");

    // ═══════ UN: 一元/比较/逻辑/字面量（6 例）— 验证 ! / 比较链 / and / or / 字符串与布尔空值格式化后输出一致 ═══════
    // 验证: 逻辑非 ! 格式化后输出不变
    T("UN1", "unary not", "print(!true); print(!false);");
    // 验证: 比较链格式化后输出不变
    T("UN2", "comparison chain", "print(1 < 2); print(3 >= 3); print(4 == 4); print(5 != 6);");
    // 验证: 逻辑 and 格式化后输出不变
    T("UN3", "logical and", "print(true and false); print(true and true);");
    // 验证: 逻辑 or 格式化后输出不变
    T("UN4", "logical or", "print(false or true); print(false or false);");
    // 验证: 字符串字面量格式化后输出不变
    T("UN5", "string literal", "print(\"hello world\");");
    // 验证: 布尔与 null 字面量格式化后输出不变
    T("UN6", "bool and null", "print(true); print(false); print(null);");

    // ═══════ VR: 变量与赋值（6 例）— 验证变量声明、重赋值、字符串拼接、块内遮蔽、复合表达式的格式化等价性 ═══════
    // 验证: 变量声明与打印格式化后不变
    T("VR1", "var decl", "var x = 42; print(x);");
    // 验证: 变量重赋值格式化后语义一致
    T("VR2", "reassign", "var x = 1; x = 2; print(x);");
    // 验证: 多变量声明与相加格式化后不变
    T("VR3", "multi var", "var a = 1; var b = 2; var c = a + b; print(c);");
    // 验证: 字符串拼接格式化后输出不变
    T("VR4", "string concat", "var s = \"hello\" + \" \" + \"world\"; print(s);");
    // 验证: 块内变量遮蔽格式化后语义不变
    T("VR5", "shadow", "var x = 1; { var x = 99; print(x); } print(x);");
    // 验证: 复合括号表达式格式化后求值一致
    T("VR6", "compound expr", "var x = (1 + 2) * (3 + 4); print(x);");

    // ═══════ BR: 分支语句（8 例）— 验证 if / if-else / else-if / 嵌套 if / 悬空 else / 空 if 等分支结构格式化后一致性 ═══════
    // 验证: 单 if 分支格式化后输出不变
    T("BR1", "if true", "if (true) { print(\"yes\"); }");
    // 验证: if-else 分支格式化后输出不变
    T("BR2", "if-else", "if (false) { print(\"a\"); } else { print(\"b\"); }");
    // 验证: else-if 多分支格式化后输出不变
    T("BR3", "else-if", "var x = 85; if (x >= 90) { print(\"A\"); } else if (x >= 80) { print(\"B\"); } else { print(\"C\"); }");
    // 验证: 嵌套 if 格式化后语义一致
    T("BR4", "nested if", "if (true) { if (false) { print(1); } else { print(2); } }");
    // 验证: 悬空 else 绑定格式化后不变
    T("BR5", "dangling else", "if (false) { if (true) { print(1); } } else { print(2); }");
    // 验证: 条件分支格式化后输出不变
    T("BR6", "ternary-like", "var x = 5; if (x > 3) { print(\"big\"); } else { print(\"small\"); }");
    // 验证: 空 if 体格式化后不破坏后续语句
    T("BR7", "empty if", "if (false) { } print(\"after\");");
    // 验证: 嵌套 if-else 格式化后语义一致
    T("BR8", "nested if-else", "var a = 1; var b = 2; if (a == 1) { if (b == 2) { print(\"yes\"); } else { print(\"no\"); } }");

    // ═══════ LP: 循环语句（6 例）— 验证 while / for（含求和、嵌套、倒计时）等循环结构格式化后输出不变 ═══════
    // 验证: while 基础循环格式化后输出不变
    T("LP1", "while basic", "var i = 0; while (i < 5) { print(i); i = i + 1; }");
    // 验证: while 求和格式化后结果一致
    T("LP2", "while sum", "var s = 0; var i = 1; while (i <= 10) { s = s + i; i = i + 1; } print(s);");
    // 验证: for 基础循环格式化后输出不变
    T("LP3", "for basic", "for (var i = 0; i < 5; i = i + 1) { print(i); }");
    // 验证: for 求和格式化后结果一致（1..100=5050）
    T("LP4", "for sum", "var s = 0; for (var i = 1; i <= 100; i = i + 1) { s = s + i; } print(s);");
    // 验证: 嵌套 for 格式化后输出不变
    T("LP5", "nested for", "for (var i = 0; i < 3; i = i + 1) { for (var j = 0; j < 3; j = j + 1) { print(i * 10 + j); } }");
    // 验证: while 倒计时格式化后输出不变
    T("LP6", "while countdown", "var n = 5; while (n > 0) { print(n); n = n - 1; } print(\"done\");");

    // ═══════ FN: 函数定义（8 例）— 验证基本函数、无返回、递归、多参数、嵌套调用、void 函数、类型注解、互递归的格式化等价 ═══════
    // 验证: 基本函数定义与调用格式化后不变
    T("FN1", "basic fun", "fun add(a, b) { return a + b; } print(add(3, 4));");
    // 验证: 无返回值函数（返回 null）格式化后输出不变
    T("FN2", "no return", "fun f() { var x = 1; } print(f());");
    // 验证: 递归函数格式化后结果一致（fact(6)=720）
    T("FN3", "recursive", "fun fact(n) { if (n <= 1) { return 1; } return n * fact(n - 1); } print(fact(6));");
    // 验证: 多参数函数格式化后不变
    T("FN4", "multi-param", "fun f(a, b, c) { return a + b + c; } print(f(1, 2, 3));");
    // 验证: 嵌套函数调用格式化后结果一致
    T("FN5", "nested call", "fun a(x) { return x + 1; } fun b(x) { return a(x) * 2; } print(b(5));");
    // 验证: 副作用 void 函数累加格式化后不变
    T("FN6", "void fun", "var s = 0; fun add(v) { s = s + v; } add(10); add(20); add(30); print(s);");
    // 验证: 带类型注解参数函数格式化后不变
    T("FN7", "typed params", "fun add(int a, int b) { return a + b; } print(add(7, 8));");
    // 验证: 互递归函数格式化后结果一致
    T("FN8", "mutual recursion", "fun isEven(n) { if (n == 0) { return true; } return isOdd(n - 1); } fun isOdd(n) { if (n == 0) { return false; } return isEven(n - 1); } print(isEven(6));");

    // ═══════ AY: 数组（6 例）— 验证数组字面量、len/push、遍历、拼接、嵌套数组等结构格式化后语义不变 ═══════
    // 验证: 数组字面量索引访问格式化后不变
    T("AY1", "array literal", "var a = [1, 2, 3]; print(a[0]); print(a[1]); print(a[2]);");
    // 验证: 数组 len 方法格式化后不变
    T("AY2", "array len", "var a = [10, 20, 30]; print(a.len());");
    // 验证: 数组 push 扩容格式化后不变
    T("AY3", "array push", "var a = [1]; a.push(2); a.push(3); print(a.len());");
    // 验证: 数组遍历格式化后输出不变
    T("AY4", "array iter", "var a = [1, 2, 3]; var i = 0; while (i < a.len()) { print(a[i]); i = i + 1; }");
    // 验证: 数组拼接 + 格式化后不变
    T("AY5", "array concat", "var a = [1, 2]; var b = [3, 4]; var c = a + b; print(c.len());");
    // 验证: 嵌套数组索引格式化后不变
    T("AY6", "nested array", "var a = [[1, 2], [3, 4]]; print(a[0][1]); print(a[1][0]);");

    // ═══════ DC: 字典（4 例）— 验证字典字面量、len/has 方法、下标赋值、嵌套字典等格式化后一致性 ═══════
    // 验证: 字典字面量访问格式化后不变
    T("DC1", "dict literal", "var d = {\"a\": 1, \"b\": 2}; print(d[\"a\"]); print(d[\"b\"]);");
    // 验证: 字典 len/has 方法格式化后不变
    T("DC2", "dict methods", "var d = {\"x\": 10, \"y\": 20}; print(d.len()); print(d.has(\"x\"));");
    // 验证: 字典下标赋值格式化后不变
    T("DC3", "dict set", "var d = {}; d[\"k\"] = 99; print(d[\"k\"]);");
    // 验证: 嵌套字典访问格式化后不变
    T("DC4", "nested dict", "var d = {\"inner\": {\"v\": 42}}; print(d[\"inner\"][\"v\"]);");

    // ═══════ CL: 类与继承（4 例）— 验证类定义、继承+super、字段、成员方法调用等面向对象结构格式化后等价 ═══════
    // 验证: 基础类成员方法调用格式化后不变
    T("CL1", "basic class",
      "class Animal { init(n) { this.name = n; } speak() { return this.name; } }\n"
      "var a = Animal(\"cat\"); print(a.speak());");
    // 验证: 继承 + super.init 格式化后不变
    T("CL2", "inheritance",
      "class A { init() { this.x = 1; } get() { return this.x; } }\n"
      "class B : A { init() { super.init(); this.y = 2; } }\n"
      "var b = B(); print(b.get()); print(b.y);");
    // 验证: 类字段与成员方法求和格式化后不变
    T("CL3", "class field",
      "class P { init(x, y) { this.x = x; this.y = y; } sum() { return this.x + this.y; } }\n"
      "var p = P(3, 4); print(p.sum());");
    // 验证: 类方法多次调用副作用格式化后不变
    T("CL4", "class method call",
      "class C { init() { this.v = 0; } inc() { this.v = this.v + 1; } get() { return this.v; } }\n"
      "var c = C(); c.inc(); c.inc(); c.inc(); print(c.get());");

    // ═══════ CM: 复杂表达式（4 例）— 验证嵌套括号、数组方法链、字符串方法、循环累加等混合表达式格式化后一致 ═══════
    // 验证: 嵌套括号表达式格式化后不变
    T("CM1", "nested parens", "print(((1 + 2) * (3 + 4)) - 5);");
    // 验证: 数组方法链（push/len/contains）格式化后不变
    T("CM2", "array method chain", "var a = [1, 2, 3, 4, 5]; a.push(6); print(a.len()); print(a.contains(3));");
    // 验证: 字符串 len/upper 方法格式化后不变
    T("CM3", "string methods", "var s = \"hello\"; print(s.len()); print(s.upper());");
    // 验证: 数组循环累加混合表达式格式化后不变
    T("CM4", "mixed expr", "var a = [10, 20, 30]; var s = 0; var i = 0; while (i < a.len()) { s = s + a[i]; i = i + 1; } print(s);");

    // ═══════ BD: 边界/压力测试（10 例）— 验证深层嵌套、混乱空白、长表达式、空程序、深层函数链、超大数组、多层继承、格式混乱的类等极端输入的格式化健壮性 ═══════
    // 验证: 十层嵌套块格式化后仍能正确输出
    T("BD1", "10-level nesting",
      "{ { { { { { { { { { print(42); } } } } } } } } } }");
    // 验证: 混乱空白经格式化后语义不变
    T("BD2", "whitespace mess",
      "var   x  =  1  +  2  ;  print ( x ) ;");
    // 验证: 超长加法表达式格式化后结果一致（1..15=120）
    T("BD3", "long expression",
      "print(1 + 2 + 3 + 4 + 5 + 6 + 7 + 8 + 9 + 10 + 11 + 12 + 13 + 14 + 15);");
    // 验证: 空程序格式化后不产生错误
    T("BD4", "empty program", "");
    // 验证: 单条 print 格式化后不变
    T("BD5", "single print", "print(1);");
    // 验证: 多条 print 格式化后输出不变
    T("BD6", "multi print",
      "print(1); print(2); print(3); print(4); print(5);");
    // 验证: 深层函数链格式化后结果一致（e(0)=5）
    T("BD7", "deep function chain",
      "fun a(x){return x+1;} fun b(x){return a(x)+1;} fun c(x){return b(x)+1;} "
      "fun d(x){return c(x)+1;} fun e(x){return d(x)+1;} print(e(0));");
    // 验证: 大数组字面量格式化后 len 不变（20）
    T("BD8", "large array",
      "var a = [1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20]; print(a.len());");
    // 验证: 多层继承 + super.init 格式化后不变
    T("BD9", "deep class hierarchy with super.init",
      "class A { init() { this.a = 1; } }\n"
      "class B : A { init() { super.init(); this.b = 2; } }\n"
      "class C : B { init() { super.init(); this.c = 3; } }\n"
      "var c = C(); print(c.a); print(c.b); print(c.c);");
    // 验证: 格式混乱的类定义经格式化后语义不变（3*3+4*4=25）
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
