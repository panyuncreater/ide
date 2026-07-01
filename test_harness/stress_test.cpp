// stress_test.cpp — 专项6：多特性组合语义压力测试
// 100 个复合测试用例，每个同时组合 ≥3 种语法特性
// 重点排查：跨作用域变量访问、隐式类型转换、表达式优先级、返回值生命周期

#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "interpreter/Interpreter.h"
#include "ast/ASTNode.h"
#include <iostream>
#include <string>
#include <vector>

// ─── 测试基础设施 ─────────────────────────────────────────────

struct TestCase {
    std::string id;
    std::string name;
    std::string source;
    std::string expected;
    std::string features; // 标记组合的特性
};

static std::string runMiniLang(const std::string& source, std::string& error) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    if (lexer.getDiagnostics().hasErrors()) {
        error = "lex: " + lexer.getDiagnostics().all()[0].message;
        return "";
    }
    Parser parser;
    auto ast = parser.parse(tokens);
    if (parser.hasErrors()) {
        error = "parse: " + parser.getDiagnostics().all()[0].message;
        return "";
    }

    Interpreter interp;
    std::vector<std::string> lines;
    interp.setOutputCallback([&](const std::string& text) {
        lines.push_back(text);
    });

    try {
        interp.execute(*ast);
    } catch (const std::exception& e) {
        error = "runtime: " + std::string(e.what());
        return "";
    }

    std::string result;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i > 0) result += "\n";
        result += lines[i];
    }
    return result;
}

// ─── 100 个测试用例 ───────────────────────────────────────────

static std::vector<TestCase> buildTests() {
    std::vector<TestCase> tests;
    auto T = [&](const std::string& id, const std::string& name,
                 const std::string& src, const std::string& expected,
                 const std::string& features) {
        tests.push_back({id, name, src, expected, features});
    };

    // ═══════════════════════════════════════════════════════════
    // A. 递归 + 数组 + 分支 (T001-T010)
    // ═══════════════════════════════════════════════════════════

    T("T001", "递归构建数组",
      R"(fun build(n) { if (n == 0) { return []; } var a = build(n - 1); a.push(n); return a; } var r = build(5); print(r);)",
      "[1, 2, 3, 4, 5]", "recursion+array+branching");

    T("T002", "迭代阶乘数组",
      R"(fun fact(n) { var a = []; var r = 1; for (var i = 1; i <= n; i = i + 1) { r = r * i; a.push(r); } return a; } print(fact(5));)",
      "[1, 2, 6, 24, 120]", "loop+array+arithmetic+function");

    T("T003", "数组展平(嵌套数组)",
      R"(var a = [[1, 2], [3, 4], [5]]; var r = []; for (var i = 0; i < a.len(); i = i + 1) { for (var j = 0; j < a[i].len(); j = j + 1) { r.push(a[i][j]); } } print(r);)",
      "[1, 2, 3, 4, 5]", "nested_loop+array+indexing");

    T("T004", "递归过滤偶数",
      R"(fun filterEven(a, i) { if (i >= a.len()) { return []; } var r = filterEven(a, i + 1); if (a[i] % 2 == 0) { r.push(a[i]); } return r; } print(filterEven([1,2,3,4,5,6], 0));)",
      "[6, 4, 2]", "recursion+array+branching+modulo");

    T("T005", "递归数组求和",
      R"(fun sum(a, i) { if (i >= a.len()) { return 0; } return a[i] + sum(a, i + 1); } print(sum([10, 20, 30, 40], 0));)",
      "100", "recursion+array+arithmetic");

    T("T006", "Fibonacci 迭代",
      R"(fun fib(n) { if (n <= 1) { return n; } var a = 0; var b = 1; for (var i = 2; i <= n; i = i + 1) { var t = b; b = a + b; a = t; } return b; } print(fib(10));)",
      "55", "recursion+loop+arithmetic+function");

    T("T007", "递归反转数组",
      R"(fun rev(a) { if (a.len() == 0) { return []; } var r = []; while (a.len() > 0) { r.push(a.pop()); } return r; } print(rev([1, 2, 3, 4, 5]));)",
      "[5, 4, 3, 2, 1]", "recursion+array+loop+method_call");

    T("T008", "递归 GCD 双打印",
      R"(fun gcd(a, b) { if (b == 0) { return a; } return gcd(b, a % b); } var g = gcd(48, 36); print(g); print(g);)",
      "12\n12", "recursion+modulo+function+variable");

    T("T009", "递归计数偶数",
      R"(fun countEven(a, i) { if (i >= a.len()) { return 0; } if (a[i] % 2 == 0) { return 1 + countEven(a, i + 1); } return countEven(a, i + 1); } print(countEven([1,2,3,4,5,6,7,8], 0));)",
      "4", "recursion+array+branching+modulo");

    T("T010", "递归找最大值",
      R"(fun maxOf(a, i, m) { if (i >= a.len()) { return m; } if (a[i] > m) { return maxOf(a, i + 1, a[i]); } return maxOf(a, i + 1, m); } print(maxOf([3, 1, 7, 2, 9, 4], 0, 0));)",
      "9", "recursion+array+branching+comparison");

    // ═══════════════════════════════════════════════════════════
    // B. 嵌套循环 + 函数 + 字符串 (T011-T020)
    // ═══════════════════════════════════════════════════════════

    T("T011", "循环字符串拼接",
      R"(fun concat(a, b) { return a + " " + b; } var r = ""; for (var i = 1; i <= 3; i = i + 1) { r = concat(r, "word" + ("" + i)); } print(r);)",
      " word1 word2 word3", "function+string+loop+concat");

    T("T012", "字符串比较与函数",
      R"(fun check(a, b) { if (a == b) { return "same"; } return "diff"; } print(check("hello", "hello")); print(check("abc", "xyz"));)",
      "same\ndiff", "function+string+comparison+branching");

    T("T013", "FizzBuzz 嵌套循环",
      R"(for (var i = 1; i <= 15; i = i + 1) { var s = ""; if (i % 3 == 0) { s = s + "Fizz"; } if (i % 5 == 0) { s = s + "Buzz"; } if (s == "") { s = "" + i; } print(s); })",
      "1\n2\nFizz\n4\nBuzz\nFizz\n7\n8\nFizz\nBuzz\n11\nFizz\n13\n14\nFizzBuzz",
      "nested_loop+modulo+string+branching");

    T("T014", "数组元素频率统计",
      R"(var arr = [1, 2, 2, 3, 3, 3]; var freq = {}; for (var i = 0; i < arr.len(); i = i + 1) { var k = "" + arr[i]; if (freq[k] == null) { freq[k] = 0; } freq[k] = freq[k] + 1; } print(freq["1"]); print(freq["2"]); print(freq["3"]);)",
      "1\n2\n3", "loop+array+dict+branching+null_check");

    T("T015", "句子单词反转",
      R"(fun revWords(s) { var words = s.split(" "); var r = ""; for (var i = words.len() - 1; i >= 0; i = i - 1) { if (r != "") { r = r + " "; } r = r + words[i]; } return r; } print(revWords("hello world foo"));)",
      "foo world hello", "function+string+loop+array+split");

    T("T016", "字符网格打印",
      R"(fun grid(w, h) { for (var i = 0; i < h; i = i + 1) { var r = ""; for (var j = 0; j < w; j = j + 1) { r = r + "*"; } print(r); } } grid(3, 2);)",
      "***\n***", "nested_loop+string+function+concat");

    T("T017", "嵌套循环求质数",
      R"(fun isPrime(n) { if (n < 2) { return false; } for (var i = 2; i * i <= n; i = i + 1) { if (n % i == 0) { return false; } } return true; } var p = []; for (var n = 2; n < 20; n = n + 1) { if (isPrime(n)) { p.push(n); } } print(p);)",
      "[2, 3, 5, 7, 11, 13, 17, 19]", "nested_loop+function+modulo+array+branching");

    T("T018", "最长单词",
      R"(fun longest(s) { var words = s.split(" "); var m = 0; for (var i = 0; i < words.len(); i = i + 1) { if (words[i].len() > m) { m = words[i].len(); } } return m; } print(longest("the quick brown fox"));)",
      "5", "function+string+loop+comparison+method_call");

    T("T019", "数组字符串批量大写",
      R"(var words = ["hello", "world"]; var result = []; for (var i = 0; i < words.len(); i = i + 1) { result.push(words[i].upper()); } print(result);)",
      "[\"HELLO\", \"WORLD\"]", "loop+array+string+method_call");

    T("T020", "字符串替换链",
      R"(var s = "aXbXc"; s = s.replace("X", "-"); var words = s.split("-"); print(words); print(words.len());)",
      "[\"a\", \"b\", \"c\"]\n3", "string+method_call+array+split+replace");

    // ═══════════════════════════════════════════════════════════
    // C. 作用域嵌套 + 函数返回 + 布尔逻辑 (T021-T030)
    // ═══════════════════════════════════════════════════════════

    T("T021", "嵌套作用域与逻辑",
      R"(var x = true; { if (x) { var y = false; if (y) { print(1); } else { print(2); } } if (x or false) { print(3); } })",
      "2\n3", "scope+branching+boolean+logical_or");

    T("T022", "短路求值与副作用",
      R"(var x = 0; var a = false and (x = 1); print(x); var b = true or (x = 2); print(x);)",
      "0\n0", "short_circuit+boolean+assignment+side_effect");

    T("T023", "函数作用域隔离",
      R"(var x = 1; fun f() { var x = 100; print(x); } f(); print(x);)",
      "100\n1", "function+scope+variable+isolation");

    T("T024", "函数返回与条件逻辑",
      R"(fun check(n) { if (n < 0) { return "negative"; } if (n == 0) { return "zero"; } return "positive"; } print(check(-5)); print(check(3));)",
      "negative\npositive", "function+return+branching+string");

    T("T025", "递归与逻辑短路",
      R"(fun check(n) { if (n <= 0) { return true; } if (n > 3) { return false; } return check(n - 1); } print(check(5)); print(check(2));)",
      "false\ntrue", "recursion+boolean+branching+function");

    T("T026", "逻辑运算与真值保留",
      R"(var x = 5 and 10; print(x); var y = 0 or 42; print(y); var z = "" or "default"; print(z);)",
      "10\n42\ndefault", "logical_and+logical_or+truthy+short_circuit");

    T("T027", "三层嵌套作用域",
      R"(var a = 1; { var b = 2; { var c = 3; print(a + b + c); } print(a + b); } print(a);)",
      "6\n3\n1", "nested_scope+variable+arithmetic+access_chain");

    T("T028", "布尔与 or 返回值",
      R"(var x = false or 42; print(x); var y = true and "hello"; print(y);)",
      "42\nhello", "boolean_or+short_circuit+truthy+string");

    T("T029", "复合条件与函数",
      R"(fun between(x, lo, hi) { return x >= lo and x <= hi; } print(between(5, 1, 10)); print(between(15, 1, 10));)",
      "true\nfalse", "function+boolean+comparison+logical_and");

    T("T030", "嵌套函数调用链",
      R"(fun double(x) { return x * 2; } fun inc(x) { return x + 1; } fun square(x) { return x * x; } print(square(inc(double(3))));)",
      "49", "function_chain+arithmetic+nested_call");

    // ═══════════════════════════════════════════════════════════
    // D. 类继承 + 成员方法 + 循环 + 变量遮蔽 (T031-T040)
    // ═══════════════════════════════════════════════════════════

    T("T031", "类继承与方法重写",
      R"(class Animal { init(n) { this.name = n; } speak() { return "sound"; } } class Dog : Animal { init(n) { super.init(n); } speak() { return "woof"; } } var d = Dog("Rex"); print(d.name); print(d.speak());)",
      "Rex\nwoof", "class+inheritance+method_override+super");

    T("T032", "多层继承与属性链",
      R"(class A { init() { this.x = 1; } } class B : A { init() { super.init(); this.y = 2; } } class C : B { init() { super.init(); this.z = 3; } } var c = C(); print(c.x); print(c.y); print(c.z);)",
      "1\n2\n3", "multi_inheritance+super+member_access+chain");

    T("T033", "成员方法与循环求和",
      R"(class Sum { init() { this.total = 0; } add(n) { this.total = this.total + n; } } var s = Sum(); for (var i = 1; i <= 10; i = i + 1) { s.add(i); } print(s.total);)",
      "55", "class+method+loop+arithmetic+member_assign");

    T("T034", "形状类继承与面积",
      R"(class Shape { area() { return 0; } } class Circle : Shape { init(r) { this.r = r; } area() { return 3 * this.r * this.r; } } var c = Circle(5); print(c.area());)",
      "75", "class+inheritance+method+arithmetic+this");

    T("T035", "循环中的多态方法调用",
      R"(class Shape { sides() { return 0; } } class Tri : Shape { sides() { return 3; } } var shapes = [Tri(), Tri(), Tri()]; var t = 0; for (var i = 0; i < shapes.len(); i = i + 1) { t = t + shapes[i].sides(); } print(t);)",
      "9", "class+polymorphism+loop+array+method_call");

    T("T036", "类初始化与数组填充",
      R"(class Buf { init(n) { this.data = []; for (var i = 0; i < n; i = i + 1) { this.data.push(i); } } } var b = Buf(5); print(b.data.len()); print(b.data);)",
      "5\n[0, 1, 2, 3, 4]", "class+init+loop+array+method_call");

    T("T037", "成员遮蔽：局部 vs this",
      R"(class Obj { init() { this.x = 10; } test() { var x = 20; print(x); print(this.x); } } var o = Obj(); o.test();)",
      "20\n10", "class+scope+shadowing+this+member");

    T("T038", "方法内循环与累加",
      R"(class Calc { init() { this.sum = 0; } addRange(n) { for (var i = 1; i <= n; i = i + 1) { this.sum = this.sum + i; } } } var c = Calc(); c.addRange(100); print(c.sum);)",
      "5050", "class+method+loop+arithmetic+member_assign");

    T("T039", "继承链中 super 方法调用",
      R"(class A { greet() { return "A"; } } class B : A { greet() { return super.greet() + "B"; } } class C : B { greet() { return super.greet() + "C"; } } print(C().greet());)",
      "ABC", "inheritance+super+method+string_concat+chain");

    T("T040", "类方法与数组过滤",
      R"(class Filter { init(thresh) { this.thresh = thresh; } run(arr) { var r = []; for (var i = 0; i < arr.len(); i = i + 1) { if (arr[i] > this.thresh) { r.push(arr[i]); } } return r; } } var f = Filter(3); print(f.run([1, 5, 2, 8, 3, 7]));)",
      "[5, 8, 7]", "class+method+loop+array+branching+member_access");

    // ═══════════════════════════════════════════════════════════
    // E. 变量遮蔽跨作用域 (T041-T050)
    // ═══════════════════════════════════════════════════════════

    T("T041", "嵌套块变量遮蔽",
      R"(var x = 10; { var x = 20; print(x); } print(x);)",
      "20\n10", "scope+shadowing+variable+block");

    T("T042", "函数参数遮蔽全局",
      R"(var x = 1; fun f(x) { print(x); var x2 = 3; print(x2); } f(2); print(x);)",
      "2\n3\n1", "function+parameter+shadowing+scope");

    T("T043", "循环变量遮蔽外层",
      R"(var i = 99; for (var i = 0; i < 3; i = i + 1) { print(i); } print(i);)",
      "0\n1\n2\n99", "for_loop+scope+shadowing+variable");

    T("T044", "嵌套循环多层遮蔽",
      R"(var x = 1; for (var x = 0; x < 2; x = x + 1) { var x2 = x * 10; print(x2); } print(x);)",
      "0\n10\n1", "nested_loop+scope+shadowing+arithmetic");

    T("T045", "递归中的参数遮蔽",
      R"(fun f(n) { if (n == 0) { print(n); return; } f(n - 1); print(n); } f(3);)",
      "0\n1\n2\n3", "recursion+scope+shadowing+function");

    T("T046", "嵌套函数作用域",
      R"(var x = 1; fun outer() { var x = 2; fun inner() { var x = 3; print(x); } inner(); print(x); } outer(); print(x);)",
      "3\n2\n1", "nested_function+scope+shadowing+three_levels");

    T("T047", "方法参数遮蔽成员",
      R"(class C { init() { this.x = 10; } test(x) { print(x); print(this.x); } } var c = C(); c.test(99);)",
      "99\n10", "class+method+parameter+shadowing+member");

    T("T048", "块内函数声明遮蔽",
      R"(fun f() { return 1; } { fun f() { return 2; } print(f()); } print(f());)",
      "2\n1", "function+scope+shadowing+block+redeclaration");

    T("T049", "循环内块遮蔽",
      R"(var sum = 0; for (var i = 0; i < 3; i = i + 1) { var j = i * 10; { var k = j + i; sum = sum + k; } } print(sum);)",
      "33", "loop+block+scope+variable+arithmetic");

    T("T050", "类继承成员遮蔽",
      R"(class A { init() { this.x = 1; } } class B : A { init() { super.init(); this.x = 2; } } var b = B(); print(b.x);)",
      "2", "class+inheritance+shadowing+member+super");

    // ═══════════════════════════════════════════════════════════
    // F. 表达式优先级与结合性 (T051-T060)
    // ═══════════════════════════════════════════════════════════

    T("T051", "混合加减乘",
      R"(print(1 + 2 * 3 - 4 / 2);)",
      "5", "precedence+mul_over_add+division");

    T("T052", "嵌套括号优先级",
      R"(print((1 + 2) * (3 + 4) - (5 + 6));)",
      "10", "parentheses+precedence+arithmetic");

    T("T053", "一元运算符与二元混合",
      R"(print(-3 + 4 * -2); print(-(3 + 4) * 2);)",
      "-11\n-14", "unary+binary+precedence+negation");

    T("T054", "比较链与逻辑",
      R"(print(1 < 2 and 2 < 3 and 3 < 4); print(1 > 2 or 2 > 1);)",
      "true\ntrue", "comparison+logical_and+logical_or+chain");

    T("T055", "减法的左结合性",
      R"(print(100 - 30 - 20 - 10);)",
      "40", "left_associativity+subtraction+chain");

    T("T056", "除法的左结合性",
      R"(print(96 / 8 / 4 / 3);)",
      "1", "left_associativity+division+chain");

    T("T057", "取模与加法混合",
      R"(print(17 % 5 + 3); print(17 % (5 + 3));)",
      "5\n1", "modulo+precedence+parentheses+arithmetic");

    T("T058", "相等性与逻辑组合",
      R"(print(1 == 1 and 2 != 3); print(false or 1 == 1 and true);)",
      "true\ntrue", "equality+inequality+logical+precedence");

    T("T059", "复杂嵌套表达式",
      R"(var x = (2 + 3) * (4 - 1) / (6 % 4 + 1); print(x);)",
      "5", "complex_expression+parentheses+modulo+division");

    T("T060", "算术与比较混合",
      R"(print(1 + 2 < 3 + 4 and 5 * 2 > 8);)",
      "true", "arithmetic+comparison+logical+mixed");

    // ═══════════════════════════════════════════════════════════
    // G. 函数返回值与栈帧 (T061-T070)
    // ═══════════════════════════════════════════════════════════

    T("T061", "循环内提前返回",
      R"(fun findFirst(arr, target) { for (var i = 0; i < arr.len(); i = i + 1) { if (arr[i] == target) { return i; } } return -1; } print(findFirst([10, 20, 30], 20));)",
      "1", "function+return+loop+array+early_return");

    T("T062", "条件多路返回",
      R"(fun sign(x) { if (x > 0) { return 1; } if (x < 0) { return -1; } return 0; } print(sign(5)); print(sign(-3)); print(sign(0));)",
      "1\n-1\n0", "function+return+branching+multi_path");

    T("T063", "嵌套函数与返回链",
      R"(fun outer() { fun inner() { return 42; } return inner() + 1; } print(outer());)",
      "43", "nested_function+return+chain+arithmetic");

    T("T064", "递归返回与累加",
      R"(fun sumTo(n) { if (n == 0) { return 0; } return n + sumTo(n - 1); } print(sumTo(50));)",
      "1275", "recursion+return+arithmetic+stack_frame");

    T("T065", "布尔逻辑与返回短路",
      R"(fun check(x) { return x > 0 and x < 100; } print(check(50)); print(check(-1)); print(check(200));)",
      "true\nfalse\nfalse", "function+return+boolean+short_circuit");

    T("T066", "递归 Fibonacci",
      R"(fun fib(n) { if (n <= 1) { return n; } return fib(n - 1) + fib(n - 2); } print(fib(6));)",
      "8", "recursion+return+arithmetic+fibonacci");

    T("T067", "函数返回数组",
      R"(fun make(n) { var a = []; for (var i = 0; i < n; i = i + 1) { a.push(i * i); } return a; } print(make(5));)",
      "[0, 1, 4, 9, 16]", "function+return+array+loop+arithmetic");

    T("T068", "返回字典",
      R"(fun profile(name, age) { var d = {}; d["name"] = name; d["age"] = age; return d; } var p = profile("Alice", 30); print(p["name"]); print(p["age"]);)",
      "Alice\n30", "function+return+dict+string+index_access");

    T("T069", "递归搜索与返回索引",
      R"(fun find(arr, target, i) { if (i >= arr.len()) { return -1; } if (arr[i] == target) { return i; } return find(arr, target, i + 1); } print(find([1,2,3,4,5], 3, 0));)",
      "2", "recursion+return+array+search");

    T("T070", "链式返回值",
      R"(fun a() { return 1; } fun b() { return a() + 1; } fun c() { return b() + 1; } fun d() { return c() + 1; } print(d());)",
      "4", "function_chain+return+stack_frame+arithmetic");

    // ═══════════════════════════════════════════════════════════
    // H. 数组 + 字典 + 嵌套数据结构 (T071-T080)
    // ═══════════════════════════════════════════════════════════

    T("T071", "嵌套数组操作",
      R"(var a = [[1, 2], [3, 4]]; var b = [[5, 6], [7, 8]]; print(a[0][1] + b[1][0]);)",
      "9", "nested_array+index_access+arithmetic");

    T("T072", "字典包含数组",
      R"(var d = {}; d["nums"] = [10, 20, 30]; print(d["nums"][1]); d["nums"].push(40); print(d["nums"].len());)",
      "20\n4", "dict+array+nested_access+method_call");

    T("T073", "对象数组",
      R"(class Pt { init(x, y) { this.x = x; this.y = y; } } var pts = [Pt(1, 2), Pt(3, 4)]; print(pts[0].x + pts[1].x); print(pts[1].y);)",
      "4\n4", "class+array+member_access+arithmetic");

    T("T074", "字典合并",
      R"(fun merge(a, b) { var keys = b.keys(); for (var i = 0; i < keys.len(); i = i + 1) { a[keys[i]] = b[keys[i]]; } return a; } var d1 = {}; d1["a"] = 1; var d2 = {}; d2["b"] = 2; d2["c"] = 3; var result = merge(d1, d2); print(result["b"]);)",
      "2", "function+dict+loop+method_call+merge");

    T("T075", "平方表字典",
      R"(var sq = {}; for (var i = 1; i <= 5; i = i + 1) { sq["" + i] = i * i; } print(sq["3"]); print(sq["5"]);)",
      "9\n25", "loop+dict+string_key+arithmetic+dynamic_key");

    T("T076", "数组分组(正/负)",
      R"(var pos = []; var neg = []; var arr = [3, -1, 4, -5, 2, -3]; for (var i = 0; i < arr.len(); i = i + 1) { if (arr[i] >= 0) { pos.push(arr[i]); } else { neg.push(arr[i]); } } print(pos.len()); print(neg.len());)",
      "3\n3", "array+loop+branching+method_call+partition");

    T("T077", "嵌套字典访问链",
      R"(var cfg = {}; cfg["db"] = {}; cfg["db"]["host"] = "localhost"; cfg["db"]["port"] = 5432; print(cfg["db"]["host"]); print(cfg["db"]["port"]);)",
      "localhost\n5432", "nested_dict+string+number+index_chain");

    T("T078", "数组去重",
      R"(fun dedup(arr) { var seen = {}; var r = []; for (var i = 0; i < arr.len(); i = i + 1) { var k = "" + arr[i]; if (seen[k] == null) { seen[k] = true; r.push(arr[i]); } } return r; } print(dedup([1,2,2,3,3,3,4]));)",
      "[1, 2, 3, 4]", "function+array+dict+loop+null_check+dedup");

    T("T079", "字典值求和",
      R"(var d = {}; d["a"] = 10; d["b"] = 20; d["c"] = 30; var keys = d.keys(); var sum = 0; for (var i = 0; i < keys.len(); i = i + 1) { sum = sum + d[keys[i]]; } print(sum);)",
      "60", "dict+loop+method_call+arithmetic+keys");

    T("T080", "数组到字典映射",
      R"(var s = ["h","e","l","l","o"]; var freq = {}; for (var i = 0; i < s.len(); i = i + 1) { var c = s[i]; if (freq[c] == null) { freq[c] = 0; } freq[c] = freq[c] + 1; } var keys = freq.keys(); print(keys.len());)",
      "4", "array+dict+loop+null_check+method_call");

    // ═══════════════════════════════════════════════════════════
    // I. 复杂控制流 + 数组 + 函数 (T081-T090)
    // ═══════════════════════════════════════════════════════════

    T("T081", "数组分区",
      R"(fun partition(arr, pivot) { var lo = []; var hi = []; for (var i = 0; i < arr.len(); i = i + 1) { if (arr[i] < pivot) { lo.push(arr[i]); } else { hi.push(arr[i]); } } return [lo, hi]; } var r = partition([3,1,4,1,5,9,2], 4); print(r[0]); print(r[1]);)",
      "[3, 1, 1, 2]\n[4, 5, 9]", "function+array+loop+branching+partition");

    T("T082", "累计平均值",
      R"(fun avg(arr) { var sum = 0; for (var i = 0; i < arr.len(); i = i + 1) { sum = sum + arr[i]; print(sum / (i + 1)); } } avg([10, 20, 30]);)",
      "10\n15\n20", "function+array+loop+arithmetic+division");

    T("T083", "数组游程编码",
      R"(fun rle(arr) { var r = ""; var i = 0; while (i < arr.len()) { var c = arr[i]; var cnt = 1; while (i + cnt < arr.len() and arr[i + cnt] == c) { cnt = cnt + 1; } r = r + c + ("" + cnt); i = i + cnt; } return r; } print(rle(["a","a","a","b","b","c"]));)",
      "a3b2c1", "function+array+while_loop+concat+nested_loop");

    T("T084", "矩阵乘法",
      R"(fun matmul(a, b) { var r = []; for (var i = 0; i < a.len(); i = i + 1) { var row = []; for (var j = 0; j < b[0].len(); j = j + 1) { var s = 0; for (var k = 0; k < a[0].len(); k = k + 1) { s = s + a[i][k] * b[k][j]; } row.push(s); } r.push(row); } return r; } print(matmul([[1,2],[3,4]], [[5,6],[7,8]]));)",
      "[[19, 22], [43, 50]]", "nested_loop+array+function+arithmetic+matrix");

    T("T085", "幂运算函数",
      R"(fun power(base, exp) { if (exp == 0) { return 1; } if (exp % 2 == 0) { var half = power(base, exp / 2); return half * half; } return base * power(base, exp - 1); } print(power(2, 10));)",
      "1024", "recursion+function+branching+modulo+arithmetic");

    T("T086", "模运算筛选",
      R"(fun modFilter(arr, m) { var r = []; for (var i = 0; i < arr.len(); i = i + 1) { if (arr[i] % m == 0) { r.push(arr[i]); } } return r; } print(modFilter([1,2,3,4,5,6,7,8,9,10], 3));)",
      "[3, 6, 9]", "function+array+loop+modulo+branching");

    T("T087", "Tribonacci 序列",
      R"(fun tribo(n) { var a = [0, 0, 1]; for (var i = 3; i < n; i = i + 1) { a.push(a[i-1] + a[i-2] + a[i-3]); } return a; } print(tribo(8));)",
      "[0, 0, 1, 1, 2, 4, 7, 13]", "function+array+loop+arithmetic+sequence");

    T("T088", "数组旋转",
      R"(fun rotate(arr, k) { for (var i = 0; i < k; i = i + 1) { var last = arr.pop(); var r = [last]; for (var j = 0; j < arr.len(); j = j + 1) { r.push(arr[j]); } arr = r; } return arr; } print(rotate([1,2,3,4,5], 2));)",
      "[4, 5, 1, 2, 3]", "function+array+loop+method_call+rotation");

    T("T089", "数组去重(函数+字典)",
      R"(fun unique(arr) { var seen = {}; var r = []; for (var i = 0; i < arr.len(); i = i + 1) { var k = "" + arr[i]; if (seen[k] == null) { seen[k] = true; r.push(arr[i]); } } return r; } print(unique([5,3,5,1,3,5,1]));)",
      "[5, 3, 1]", "function+array+dict+loop+null_check+dedup");

    T("T090", "十进制转二进制",
      R"(fun toBin(n) { if (n == 0) { return "0"; } var r = ""; var num = n; while (num > 0) { r = ("" + (num % 2)) + r; num = num / 2; } return r; } print(toBin(13));)",
      "1101", "function+string+loop+modulo+division+concat");

    // ═══════════════════════════════════════════════════════════
    // J. 多特性综合压轴 (T091-T100)
    // ═══════════════════════════════════════════════════════════

    T("T091", "递归生成区间",
      R"(fun range(s, e) { if (s > e) { return []; } var r = range(s + 1, e); r.push(s); return r; } var a = range(1, 5); print(a); var sum = 0; for (var i = 0; i < a.len(); i = i + 1) { sum = sum + a[i]; } print(sum);)",
      "[5, 4, 3, 2, 1]\n15", "recursion+array+loop+arithmetic");

    T("T092", "Collatz 猜想计数",
      R"(fun collatz(n) { var count = 0; while (n != 1) { if (n % 2 == 0) { n = n / 2; } else { n = 3 * n + 1; } count = count + 1; } return count; } print(collatz(6));)",
      "8", "function+loop+branching+modulo+arithmetic");

    T("T093", "数字各位之和",
      R"(fun digitSum(n) { var sum = 0; while (n > 0) { sum = sum + n % 10; n = n / 10; } return sum; } print(digitSum(9876));)",
      "30", "function+loop+modulo+arithmetic+division");

    T("T094", "汉诺塔",
      R"(var moves = 0; fun hanoi(n, from, to, aux) { if (n == 1) { moves = moves + 1; return; } hanoi(n - 1, from, aux, to); moves = moves + 1; hanoi(n - 1, aux, to, from); } hanoi(4, "A", "C", "B"); print(moves);)",
      "15", "recursion+function+branching+variable+global_state");

    T("T095", "数组交集",
      R"(fun intersect(a, b) { var inB = {}; for (var i = 0; i < b.len(); i = i + 1) { inB["" + b[i]] = true; } var r = []; for (var i = 0; i < a.len(); i = i + 1) { if (inB["" + a[i]] == true) { r.push(a[i]); } } return r; } print(intersect([1,2,3,4,5], [3,4,5,6,7]));)",
      "[3, 4, 5]", "function+array+dict+loop+intersection");

    T("T096", "队列类",
      R"(class Queue { init() { this.data = []; } push(v) { this.data.push(v); } pop() { var first = this.data[0]; this.data.remove(0); return first; } size() { return this.data.len(); } } var q = Queue(); q.push(1); q.push(2); q.push(3); print(q.pop()); print(q.pop()); print(q.size());)",
      "1\n2\n1", "class+method+array+queue+FIFO");

    T("T097", "链表类",
      R"(class Node { init(v) { this.val = v; this.next = null; } } class LList { init() { this.head = null; } push(v) { var n = Node(v); n.next = this.head; this.head = n; } toArray() { var r = []; var cur = this.head; while (cur != null) { r.push(cur.val); cur = cur.next; } return r; } } var l = LList(); l.push(3); l.push(2); l.push(1); print(l.toArray());)",
      "[1, 2, 3]", "class+linked_list+loop+null_check+method_chain");

    T("T098", "递归表达式求值器",
      R"(var tokens = [3, "*", 2, "+", 4]; var pos = [0]; fun advance() { pos[0] = pos[0] + 1; } fun current() { return tokens[pos[0]]; } fun evalExpr() { var result = 0; while (pos[0] < tokens.len()) { var tok = current(); if (tok == "+") { advance(); var right = evalTerm(); result = result + right; } else if (tok == "*") { advance(); var right = evalTerm(); result = result * right; } else { advance(); result = tok; } } return result; } fun evalTerm() { var tok = current(); advance(); return tok; } print(evalExpr());)",
      "10", "recursion+function+array+loop+branching+evaluator+global_state");

    T("T099", "多态形状描述",
      R"(class Shape { describe() { return "shape"; } } class Rect : Shape { init(w, h) { this.w = w; this.h = h; } describe() { return "rect " + ("" + this.w) + "x" + ("" + this.h); } area() { return this.w * this.h; } } class Cir : Shape { init(r) { this.r = r; } describe() { return "circle r=" + ("" + this.r); } area() { return 3 * this.r * this.r; } } var shapes = [Rect(3, 4), Cir(5)]; for (var i = 0; i < shapes.len(); i = i + 1) { print(shapes[i].describe()); print(shapes[i].area()); })",
      "rect 3x4\n12\ncircle r=5\n75", "class+polymorphism+inheritance+string+loop+array");

    T("T100", "状态机(循环+分支+数组+字典)",
      R"(fun run(input) { var state = 0; var counts = {}; counts["0"] = 0; counts["1"] = 0; counts["2"] = 0; for (var i = 0; i < input.len(); i = i + 1) { var v = input[i]; if (state == 0) { if (v > 0) { state = 1; } else if (v < 0) { state = 2; } } else if (state == 1) { if (v < 0) { state = 2; } else if (v == 0) { state = 0; } } else { if (v > 0) { state = 1; } else if (v == 0) { state = 0; } } counts["" + state] = counts["" + state] + 1; } print(counts["0"]); print(counts["1"]); print(counts["2"]); } run([1, -1, 0, 2, -3, 0, 4]);)",
      "2\n3\n2", "class+state_machine+loop+branching+dict+array");

    // DIAGNOSTIC: this.member persistence
    T("DIA1", "simple this.member assign",
      R"(class C { init() { this.x = 42; } } var c = C(); print(c.x);)",
      "42", "diag");

    T("DIA2", "this.member in method",
      R"(class C { init() { this.x = 0; } set(v) { this.x = v; } } var c = C(); c.set(42); print(c.x);)",
      "42", "diag");

    T("DIA3", "this.member = this.member + n",
      R"(class C { init() { this.x = 0; } add(n) { this.x = this.x + n; } } var c = C(); c.add(5); print(c.x);)",
      "5", "diag");

    return tests;
}

// ─── Main ────────────────────────────────────────────────────

int main() {
    auto tests = buildTests();
    std::cout << "MiniLang Multi-Feature Stress Test (zhuanXiang 6)\n";
    std::cout << "==================================================\n";
    std::cout << "Test count: " << tests.size() << "\n\n";

    int pass = 0, fail = 0;
    struct FailInfo { std::string id, name, detail, expected, actual; };
    std::vector<FailInfo> failures;

    for (auto& tc : tests) {
        std::string error;
        std::string actual = runMiniLang(tc.source, error);

        if (!error.empty()) {
            fail++;
            std::string detail = "ERROR: " + error;
            std::cout << "FAIL " << tc.id << " " << tc.name << " -- " << detail << "\n";
            failures.push_back({tc.id, tc.name, detail, tc.expected, actual});
        } else if (actual == tc.expected) {
            pass++;
            std::cout << "PASS " << tc.id << " " << tc.name << "\n";
        } else {
            fail++;
            std::string detail = "output mismatch";
            std::cout << "FAIL " << tc.id << " " << tc.name << " -- " << detail << "\n";
            failures.push_back({tc.id, tc.name, detail, tc.expected, actual});
        }
    }

    int total = pass + fail;
    std::cout << "\n==================================================\n";
    std::cout << "RESULTS: " << pass << " passed, " << fail << " failed, "
              << total << " total\n";
    std::cout << "==================================================\n";

    if (!failures.empty()) {
        std::cout << "\nFAILURES:\n";
        for (auto& f : failures) {
            std::cout << "  [" << f.id << "] " << f.name << ": " << f.detail << "\n";
            std::cout << "    expected: " << f.expected << "\n";
            std::cout << "    actual:   " << f.actual << "\n";
        }
    }

    return fail > 0 ? 1 : 0;
}
