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

// 构造全部测试用例：10 组（A–J）共 100 个复合用例，外加 3 个诊断用例（DIA1–DIA3，聚焦 this 成员持久化）。
// 每个用例组合 ≥3 种语法特性，features 字段记录所覆盖的特性组合，便于失败时定位是哪一族特性出问题。
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

    // 验证: 递归构造数组（build 在每层 push 当前值），期望输出升序 [1,2,3,4,5]
    T("T001", "递归构建数组",
      R"(fun build(n) { if (n == 0) { return []; } var a = build(n - 1); a.push(n); return a; } var r = build(5); print(r);)",
      "[1, 2, 3, 4, 5]", "recursion+array+branching");

    // 验证: 迭代阶乘数组在每个循环步后 push 累积乘积，期望 [1,2,6,24,120]
    T("T002", "迭代阶乘数组",
      R"(fun fact(n) { var a = []; var r = 1; for (var i = 1; i <= n; i = i + 1) { r = r * i; a.push(r); } return a; } print(fact(5));)",
      "[1, 2, 6, 24, 120]", "loop+array+arithmetic+function");

    // 验证: 双层循环展平嵌套数组，索引访问 a[i][j] 应抽出所有元素
    T("T003", "数组展平(嵌套数组)",
      R"(var a = [[1, 2], [3, 4], [5]]; var r = []; for (var i = 0; i < a.len(); i = i + 1) { for (var j = 0; j < a[i].len(); j = j + 1) { r.push(a[i][j]); } } print(r);)",
      "[1, 2, 3, 4, 5]", "nested_loop+array+indexing");

    // 验证: 递归过滤偶数，因从末尾回退收集，期望 [6,4,2]
    T("T004", "递归过滤偶数",
      R"(fun filterEven(a, i) { if (i >= a.len()) { return []; } var r = filterEven(a, i + 1); if (a[i] % 2 == 0) { r.push(a[i]); } return r; } print(filterEven([1,2,3,4,5,6], 0));)",
      "[6, 4, 2]", "recursion+array+branching+modulo");

    // 验证: 递归数组求和，期望元素和 100
    T("T005", "递归数组求和",
      R"(fun sum(a, i) { if (i >= a.len()) { return 0; } return a[i] + sum(a, i + 1); } print(sum([10, 20, 30, 40], 0));)",
      "100", "recursion+array+arithmetic");

    // 验证: 迭代 Fibonacci（滚动变量），fib(10) 期望 55
    T("T006", "Fibonacci 迭代",
      R"(fun fib(n) { if (n <= 1) { return n; } var a = 0; var b = 1; for (var i = 2; i <= n; i = i + 1) { var t = b; b = a + b; a = t; } return b; } print(fib(10));)",
      "55", "recursion+loop+arithmetic+function");

    // 验证: 递归反转数组（反复 pop 到新数组），期望 [5,4,3,2,1]
    T("T007", "递归反转数组",
      R"(fun rev(a) { if (a.len() == 0) { return []; } var r = []; while (a.len() > 0) { r.push(a.pop()); } return r; } print(rev([1, 2, 3, 4, 5]));)",
      "[5, 4, 3, 2, 1]", "recursion+array+loop+method_call");

    // 验证: 递归 GCD 计算 48 与 36 的最大公约数，期望两次打印 12
    T("T008", "递归 GCD 双打印",
      R"(fun gcd(a, b) { if (b == 0) { return a; } return gcd(b, a % b); } var g = gcd(48, 36); print(g); print(g);)",
      "12\n12", "recursion+modulo+function+variable");

    // 验证: 递归统计偶数个数，[1..8] 中偶数共 4 个
    T("T009", "递归计数偶数",
      R"(fun countEven(a, i) { if (i >= a.len()) { return 0; } if (a[i] % 2 == 0) { return 1 + countEven(a, i + 1); } return countEven(a, i + 1); } print(countEven([1,2,3,4,5,6,7,8], 0));)",
      "4", "recursion+array+branching+modulo");

    // 验证: 递归找最大值，[3,1,7,2,9,4] 最大值为 9
    T("T010", "递归找最大值",
      R"(fun maxOf(a, i, m) { if (i >= a.len()) { return m; } if (a[i] > m) { return maxOf(a, i + 1, a[i]); } return maxOf(a, i + 1, m); } print(maxOf([3, 1, 7, 2, 9, 4], 0, 0));)",
      "9", "recursion+array+branching+comparison");

    // ═══════════════════════════════════════════════════════════
    // B. 嵌套循环 + 函数 + 字符串 (T011-T020)
    // 本组目标: 验证嵌套循环、字符串拼接/分割/方法调用与函数组合的语义正确性（含 FizzBuzz、质数筛选、单词反转等经典算法）
    // ═══════════════════════════════════════════════════════════

    // 验证: 循环调用字符串拼接函数，逐步累积 "word"+序号，期望 " word1 word2 word3"
    T("T011", "循环字符串拼接",
      R"(fun concat(a, b) { return a + " " + b; } var r = ""; for (var i = 1; i <= 3; i = i + 1) { r = concat(r, "word" + ("" + i)); } print(r);)",
      " word1 word2 word3", "function+string+loop+concat");

    // 验证: 字符串相等比较函数，相同/不同输入分别返回 "same"/"diff"
    T("T012", "字符串比较与函数",
      R"(fun check(a, b) { if (a == b) { return "same"; } return "diff"; } print(check("hello", "hello")); print(check("abc", "xyz"));)",
      "same\ndiff", "function+string+comparison+branching");

    // 验证: FizzBuzz 嵌套循环，按 3/5 倍数替换为 Fizz/Buzz，期望标准序列
    T("T013", "FizzBuzz 嵌套循环",
      R"(for (var i = 1; i <= 15; i = i + 1) { var s = ""; if (i % 3 == 0) { s = s + "Fizz"; } if (i % 5 == 0) { s = s + "Buzz"; } if (s == "") { s = "" + i; } print(s); })",
      "1\n2\nFizz\n4\nBuzz\nFizz\n7\n8\nFizz\nBuzz\n11\nFizz\n13\n14\nFizzBuzz",
      "nested_loop+modulo+string+branching");

    // 验证: 用字典统计数组各元素出现频率，期望 [1,2,3] 对应计数 1/2/3
    T("T014", "数组元素频率统计",
      R"(var arr = [1, 2, 2, 3, 3, 3]; var freq = {}; for (var i = 0; i < arr.len(); i = i + 1) { var k = "" + arr[i]; if (freq[k] == null) { freq[k] = 0; } freq[k] = freq[k] + 1; } print(freq["1"]); print(freq["2"]); print(freq["3"]);)",
      "1\n2\n3", "loop+array+dict+branching+null_check");

    // 验证: 倒序遍历单词列表并拼接，句子单词反转为 "foo world hello"
    T("T015", "句子单词反转",
      R"(fun revWords(s) { var words = s.split(" "); var r = ""; for (var i = words.len() - 1; i >= 0; i = i - 1) { if (r != "") { r = r + " "; } r = r + words[i]; } return r; } print(revWords("hello world foo"));)",
      "foo world hello", "function+string+loop+array+split");

    // 验证: 双层循环打印 w×h 的星号网格，期望两行各三个 '*'
    T("T016", "字符网格打印",
      R"(fun grid(w, h) { for (var i = 0; i < h; i = i + 1) { var r = ""; for (var j = 0; j < w; j = j + 1) { r = r + "*"; } print(r); } } grid(3, 2);)",
      "***\n***", "nested_loop+string+function+concat");

    // 验证: 嵌套循环试除法求质数，[2..19] 内质数应为 [2,3,5,7,11,13,17,19]
    T("T017", "嵌套循环求质数",
      R"(fun isPrime(n) { if (n < 2) { return false; } for (var i = 2; i * i <= n; i = i + 1) { if (n % i == 0) { return false; } } return true; } var p = []; for (var n = 2; n < 20; n = n + 1) { if (isPrime(n)) { p.push(n); } } print(p);)",
      "[2, 3, 5, 7, 11, 13, 17, 19]", "nested_loop+function+modulo+array+branching");

    // 验证: 遍历单词列表取最长长度，期望 5（"quick"）
    T("T018", "最长单词",
      R"(fun longest(s) { var words = s.split(" "); var m = 0; for (var i = 0; i < words.len(); i = i + 1) { if (words[i].len() > m) { m = words[i].len(); } } return m; } print(longest("the quick brown fox"));)",
      "5", "function+string+loop+comparison+method_call");

    // 验证: 循环对数组中每个字符串调用 upper()，批量大写，期望 ["HELLO","WORLD"]
    T("T019", "数组字符串批量大写",
      R"(var words = ["hello", "world"]; var result = []; for (var i = 0; i < words.len(); i = i + 1) { result.push(words[i].upper()); } print(result);)",
      "[\"HELLO\", \"WORLD\"]", "loop+array+string+method_call");

    // 验证: 字符串替换 + split 链，期望拆出 ["a","b","c"] 且长度为 3
    T("T020", "字符串替换链",
      R"(var s = "aXbXc"; s = s.replace("X", "-"); var words = s.split("-"); print(words); print(words.len());)",
      "[\"a\", \"b\", \"c\"]\n3", "string+method_call+array+split+replace");

    // ═══════════════════════════════════════════════════════════
    // C. 作用域嵌套 + 函数返回 + 布尔逻辑 (T021-T030)
    // 本组目标: 验证嵌套作用域、函数返回值、布尔短路求值（and/or 真值保留）与逻辑副作用的交互
    // ═══════════════════════════════════════════════════════════

    // 验证: 嵌套块作用域与布尔逻辑，内层 else 输出 2、外层 or 分支输出 3
    T("T021", "嵌套作用域与逻辑",
      R"(var x = true; { if (x) { var y = false; if (y) { print(1); } else { print(2); } } if (x or false) { print(3); } })",
      "2\n3", "scope+branching+boolean+logical_or");

    // 验证: and/or 短路时右侧赋值不执行，x 保持原值 0
    T("T022", "短路求值与副作用",
      R"(var x = 0; var a = false and (x = 1); print(x); var b = true or (x = 2); print(x);)",
      "0\n0", "short_circuit+boolean+assignment+side_effect");

    // 验证: 函数内部 var 为局部作用域，不污染外部同名变量
    T("T023", "函数作用域隔离",
      R"(var x = 1; fun f() { var x = 100; print(x); } f(); print(x);)",
      "100\n1", "function+scope+variable+isolation");

    // 验证: 函数多分支 return 正常返回字符串结果
    T("T024", "函数返回与条件逻辑",
      R"(fun check(n) { if (n < 0) { return "negative"; } if (n == 0) { return "zero"; } return "positive"; } print(check(-5)); print(check(3));)",
      "negative\npositive", "function+return+branching+string");

    // 验证: 递归与布尔短路结合，边界分支返回 true/false
    T("T025", "递归与逻辑短路",
      R"(fun check(n) { if (n <= 0) { return true; } if (n > 3) { return false; } return check(n - 1); } print(check(5)); print(check(2));)",
      "false\ntrue", "recursion+boolean+branching+function");

    // 验证: and/or 做真值保留，返回最后求值操作数（数值/字符串）
    T("T026", "逻辑运算与真值保留",
      R"(var x = 5 and 10; print(x); var y = 0 or 42; print(y); var z = "" or "default"; print(z);)",
      "10\n42\ndefault", "logical_and+logical_or+truthy+short_circuit");

    // 验证: 三层 {} 嵌套作用域的变量访问链按层级可见
    T("T027", "三层嵌套作用域",
      R"(var a = 1; { var b = 2; { var c = 3; print(a + b + c); } print(a + b); } print(a);)",
      "6\n3\n1", "nested_scope+variable+arithmetic+access_chain");

    // 验证: or/and 返回真实操作数，而非单纯布尔 0/1
    T("T028", "布尔与 or 返回值",
      R"(var x = false or 42; print(x); var y = true and "hello"; print(y);)",
      "42\nhello", "boolean_or+short_circuit+truthy+string");

    // 验证: 函数封装复合布尔比较，返回布尔结果
    T("T029", "复合条件与函数",
      R"(fun between(x, lo, hi) { return x >= lo and x <= hi; } print(between(5, 1, 10)); print(between(15, 1, 10));)",
      "true\nfalse", "function+boolean+comparison+logical_and");

    // 验证: 嵌套函数调用按从内到外顺序求值（double->inc->square）
    T("T030", "嵌套函数调用链",
      R"(fun double(x) { return x * 2; } fun inc(x) { return x + 1; } fun square(x) { return x * x; } print(square(inc(double(3))));)",
      "49", "function_chain+arithmetic+nested_call");

    // ═══════════════════════════════════════════════════════════
    // D. 类继承 + 成员方法 + 循环 + 变量遮蔽 (T031-T040)
    // 本组目标: 验证类继承、super 链、方法重写、成员遮蔽（局部 vs this）与循环内方法调用的组合
    // ═══════════════════════════════════════════════════════════

    // 验证: 子类重写父类方法，super 调用父类 init 后 speak 返回 "woof"
    T("T031", "类继承与方法重写",
      R"(class Animal { init(n) { this.name = n; } speak() { return "sound"; } } class Dog : Animal { init(n) { super.init(n); } speak() { return "woof"; } } var d = Dog("Rex"); print(d.name); print(d.speak());)",
      "Rex\nwoof", "class+inheritance+method_override+super");

    // 验证: 多层继承下 super 链逐层初始化，成员 x/y/z 均可见
    T("T032", "多层继承与属性链",
      R"(class A { init() { this.x = 1; } } class B : A { init() { super.init(); this.y = 2; } } class C : B { init() { super.init(); this.z = 3; } } var c = C(); print(c.x); print(c.y); print(c.z);)",
      "1\n2\n3", "multi_inheritance+super+member_access+chain");

    // 验证: 类方法在 for 循环中累加成员属性（1..10 求和=55）
    T("T033", "成员方法与循环求和",
      R"(class Sum { init() { this.total = 0; } add(n) { this.total = this.total + n; } } var s = Sum(); for (var i = 1; i <= 10; i = i + 1) { s.add(i); } print(s.total);)",
      "55", "class+method+loop+arithmetic+member_assign");

    // 验证: 子类重写 area 方法覆盖基类默认实现（3*5*5=75）
    T("T034", "形状类继承与面积",
      R"(class Shape { area() { return 0; } } class Circle : Shape { init(r) { this.r = r; } area() { return 3 * this.r * this.r; } } var c = Circle(5); print(c.area());)",
      "75", "class+inheritance+method+arithmetic+this");

    // 验证: 循环中多态方法调用，子类 sides 返回 3 共三组
    T("T035", "循环中的多态方法调用",
      R"(class Shape { sides() { return 0; } } class Tri : Shape { sides() { return 3; } } var shapes = [Tri(), Tri(), Tri()]; var t = 0; for (var i = 0; i < shapes.len(); i = i + 1) { t = t + shapes[i].sides(); } print(t);)",
      "9", "class+polymorphism+loop+array+method_call");

    // 验证: 类 init 内用 for 循环向成员变量数组 push 元素
    T("T036", "类初始化与数组填充",
      R"(class Buf { init(n) { this.data = []; for (var i = 0; i < n; i = i + 1) { this.data.push(i); } } } var b = Buf(5); print(b.data.len()); print(b.data);)",
      "5\n[0, 1, 2, 3, 4]", "class+init+loop+array+method_call");

    // 验证: 方法内局部变量 x 遮蔽 this.x，分别输出 20 与 10
    T("T037", "成员遮蔽：局部 vs this",
      R"(class Obj { init() { this.x = 10; } test() { var x = 20; print(x); print(this.x); } } var o = Obj(); o.test();)",
      "20\n10", "class+scope+shadowing+this+member");

    // 验证: 方法内循环累加成员属性（1..100 求和=5050）
    T("T038", "方法内循环与累加",
      R"(class Calc { init() { this.sum = 0; } addRange(n) { for (var i = 1; i <= n; i = i + 1) { this.sum = this.sum + i; } } } var c = Calc(); c.addRange(100); print(c.sum);)",
      "5050", "class+method+loop+arithmetic+member_assign");

    // 验证: 继承链中 super.greet 逐级拼接返回 "ABC"
    T("T039", "继承链中 super 方法调用",
      R"(class A { greet() { return "A"; } } class B : A { greet() { return super.greet() + "B"; } } class C : B { greet() { return super.greet() + "C"; } } print(C().greet());)",
      "ABC", "inheritance+super+method+string_concat+chain");

    // 验证: 类方法内循环+条件过滤数组，保留大于阈值的元素
    T("T040", "类方法与数组过滤",
      R"(class Filter { init(thresh) { this.thresh = thresh; } run(arr) { var r = []; for (var i = 0; i < arr.len(); i = i + 1) { if (arr[i] > this.thresh) { r.push(arr[i]); } } return r; } } var f = Filter(3); print(f.run([1, 5, 2, 8, 3, 7]));)",
      "[5, 8, 7]", "class+method+loop+array+branching+member_access");

    // ═══════════════════════════════════════════════════════════
    // E. 变量遮蔽跨作用域 (T041-T050)
    // 本组目标: 验证变量遮蔽在嵌套块、函数参数、循环变量、嵌套函数、类成员等不同层级的解析优先级
    // ═══════════════════════════════════════════════════════════

    // 验证: 块内 var x 遮蔽外层，块外恢复原值 10
    T("T041", "嵌套块变量遮蔽",
      R"(var x = 10; { var x = 20; print(x); } print(x);)",
      "20\n10", "scope+shadowing+variable+block");

    // 验证: 函数参数 x 遮蔽全局 x，参数作用域内输出 2
    T("T042", "函数参数遮蔽全局",
      R"(var x = 1; fun f(x) { print(x); var x2 = 3; print(x2); } f(2); print(x);)",
      "2\n3\n1", "function+parameter+shadowing+scope");

    // 验证: for 循环变量 i 遮蔽外层 99，循环结束后恢复 99
    T("T043", "循环变量遮蔽外层",
      R"(var i = 99; for (var i = 0; i < 3; i = i + 1) { print(i); } print(i);)",
      "0\n1\n2\n99", "for_loop+scope+shadowing+variable");

    // 验证: 嵌套循环中外层 x 被循环变量遮蔽，内层 x2 基于遮蔽值计算
    T("T044", "嵌套循环多层遮蔽",
      R"(var x = 1; for (var x = 0; x < 2; x = x + 1) { var x2 = x * 10; print(x2); } print(x);)",
      "0\n10\n1", "nested_loop+scope+shadowing+arithmetic");

    // 验证: 递归调用中每层参数 n 独立，逆序打印 0..3
    T("T045", "递归中的参数遮蔽",
      R"(fun f(n) { if (n == 0) { print(n); return; } f(n - 1); print(n); } f(3);)",
      "0\n1\n2\n3", "recursion+scope+shadowing+function");

    // 验证: 三层嵌套函数作用域，inner 输出 3、outer 局部 2、全局 1
    T("T046", "嵌套函数作用域",
      R"(var x = 1; fun outer() { var x = 2; fun inner() { var x = 3; print(x); } inner(); print(x); } outer(); print(x);)",
      "3\n2\n1", "nested_function+scope+shadowing+three_levels");

    // 验证: 方法参数 x 遮蔽成员 this.x，分别输出 99 与 10
    T("T047", "方法参数遮蔽成员",
      R"(class C { init() { this.x = 10; } test(x) { print(x); print(this.x); } } var c = C(); c.test(99);)",
      "99\n10", "class+method+parameter+shadowing+member");

    // 验证: 块内重声明函数 f 遮蔽外层，块内调用返回 2
    T("T048", "块内函数声明遮蔽",
      R"(fun f() { return 1; } { fun f() { return 2; } print(f()); } print(f());)",
      "2\n1", "function+scope+shadowing+block+redeclaration");

    // 验证: 循环内嵌套块变量 k 遮蔽计算累加到 sum（0+10+21=33... 实际 0+1+12=33）
    T("T049", "循环内块遮蔽",
      R"(var sum = 0; for (var i = 0; i < 3; i = i + 1) { var j = i * 10; { var k = j + i; sum = sum + k; } } print(sum);)",
      "33", "loop+block+scope+variable+arithmetic");

    // 验证: 子类 init 中 super.init 后重写 this.x，最终 x=2
    T("T050", "类继承成员遮蔽",
      R"(class A { init() { this.x = 1; } } class B : A { init() { super.init(); this.x = 2; } } var b = B(); print(b.x);)",
      "2", "class+inheritance+shadowing+member+super");

    // ═══════════════════════════════════════════════════════════
    // F. 表达式优先级与结合性 (T051-T060)
    // 本组目标: 验证混合算术、嵌套括号、一元/二元混合、比较链、除法/减法左结合性等优先级规则
    // ═══════════════════════════════════════════════════════════

    // 验证: 乘法/除法优先级高于加减（1 + 6 - 2 = 5）
    T("T051", "混合加减乘",
      R"(print(1 + 2 * 3 - 4 / 2);)",
      "5", "precedence+mul_over_add+division");

    // 验证: 括号强制优先，按分组求值 (3*7-11=10)
    T("T052", "嵌套括号优先级",
      R"(print((1 + 2) * (3 + 4) - (5 + 6));)",
      "10", "parentheses+precedence+arithmetic");

    // 验证: 一元负号优先级与括号分组（-(3+4)*2 = -14）
    T("T053", "一元运算符与二元混合",
      R"(print(-3 + 4 * -2); print(-(3 + 4) * 2);)",
      "-11\n-14", "unary+binary+precedence+negation");

    // 验证: 比较链与 and/or 组合，整体返回布尔
    T("T054", "比较链与逻辑",
      R"(print(1 < 2 and 2 < 3 and 3 < 4); print(1 > 2 or 2 > 1);)",
      "true\ntrue", "comparison+logical_and+logical_or+chain");

    // 验证: 减法左结合 (100-30-20-10 = 40)
    T("T055", "减法的左结合性",
      R"(print(100 - 30 - 20 - 10);)",
      "40", "left_associativity+subtraction+chain");

    // 验证: 除法左结合 (96/8/4/3 = 1)
    T("T056", "除法的左结合性",
      R"(print(96 / 8 / 4 / 3);)",
      "1", "left_associativity+division+chain");

    // 验证: 取模优先级高于加法，括号改变分组
    T("T057", "取模与加法混合",
      R"(print(17 % 5 + 3); print(17 % (5 + 3));)",
      "5\n1", "modulo+precedence+parentheses+arithmetic");

    // 验证: 相等/不等与逻辑组合，整体返回布尔
    T("T058", "相等性与逻辑组合",
      R"(print(1 == 1 and 2 != 3); print(false or 1 == 1 and true);)",
      "true\ntrue", "equality+inequality+logical+precedence");

    // 验证: 复杂嵌套表达式分组求值（5*3/3 = 5）
    T("T059", "复杂嵌套表达式",
      R"(var x = (2 + 3) * (4 - 1) / (6 % 4 + 1); print(x);)",
      "5", "complex_expression+parentheses+modulo+division");

    // 验证: 算术结果参与比较链，整体返回布尔 true
    T("T060", "算术与比较混合",
      R"(print(1 + 2 < 3 + 4 and 5 * 2 > 8);)",
      "true", "arithmetic+comparison+logical+mixed");

    // ═══════════════════════════════════════════════════════════
    // G. 函数返回值与栈帧 (T061-T070)
    // 本组目标: 验证循环/条件多路返回、嵌套函数返回链、递归返回与累加、函数返回数组/字典等栈帧生命周期
    // ═══════════════════════════════════════════════════════════

    // 验证: 循环内命中即 return 索引，否则返回 -1
    T("T061", "循环内提前返回",
      R"(fun findFirst(arr, target) { for (var i = 0; i < arr.len(); i = i + 1) { if (arr[i] == target) { return i; } } return -1; } print(findFirst([10, 20, 30], 20));)",
      "1", "function+return+loop+array+early_return");

    // 验证: 条件多路 return，分别返回 1/-1/0
    T("T062", "条件多路返回",
      R"(fun sign(x) { if (x > 0) { return 1; } if (x < 0) { return -1; } return 0; } print(sign(5)); print(sign(-3)); print(sign(0));)",
      "1\n-1\n0", "function+return+branching+multi_path");

    // 验证: 嵌套函数返回链 inner()+1 = 43
    T("T063", "嵌套函数与返回链",
      R"(fun outer() { fun inner() { return 42; } return inner() + 1; } print(outer());)",
      "43", "nested_function+return+chain+arithmetic");

    // 验证: 递归返回累加 0..50 = 1275
    T("T064", "递归返回与累加",
      R"(fun sumTo(n) { if (n == 0) { return 0; } return n + sumTo(n - 1); } print(sumTo(50));)",
      "1275", "recursion+return+arithmetic+stack_frame");

    // 验证: 函数返回布尔短路结果（50 在区间内、-1/200 不在）
    T("T065", "布尔逻辑与返回短路",
      R"(fun check(x) { return x > 0 and x < 100; } print(check(50)); print(check(-1)); print(check(200));)",
      "true\nfalse\nfalse", "function+return+boolean+short_circuit");

    // 验证: 递归斐波那契 fib(6)=8
    T("T066", "递归 Fibonacci",
      R"(fun fib(n) { if (n <= 1) { return n; } return fib(n - 1) + fib(n - 2); } print(fib(6));)",
      "8", "recursion+return+arithmetic+fibonacci");

    // 验证: 函数返回填充后的数组
    T("T067", "函数返回数组",
      R"(fun make(n) { var a = []; for (var i = 0; i < n; i = i + 1) { a.push(i * i); } return a; } print(make(5));)",
      "[0, 1, 4, 9, 16]", "function+return+array+loop+arithmetic");

    // 验证: 函数返回字典，外部按 key 取字段
    T("T068", "返回字典",
      R"(fun profile(name, age) { var d = {}; d["name"] = name; d["age"] = age; return d; } var p = profile("Alice", 30); print(p["name"]); print(p["age"]);)",
      "Alice\n30", "function+return+dict+string+index_access");

    // 验证: 递归搜索命中目标返回索引 2
    T("T069", "递归搜索与返回索引",
      R"(fun find(arr, target, i) { if (i >= arr.len()) { return -1; } if (arr[i] == target) { return i; } return find(arr, target, i + 1); } print(find([1,2,3,4,5], 3, 0));)",
      "2", "recursion+return+array+search");

    // 验证: 四层函数链式返回累加 (1+1+1+1=4)
    T("T070", "链式返回值",
      R"(fun a() { return 1; } fun b() { return a() + 1; } fun c() { return b() + 1; } fun d() { return c() + 1; } print(d());)",
      "4", "function_chain+return+stack_frame+arithmetic");

    // ═══════════════════════════════════════════════════════════
    // H. 数组 + 字典 + 嵌套数据结构 (T071-T080)
    // 本组目标: 验证嵌套数组/字典、字典含数组、对象数组、字典合并/求和、去重与字符频率等复杂数据结构的读写
    // ═══════════════════════════════════════════════════════════

    // 验证: 嵌套数组二维索引访问并相加 (2 + 7 = 9)
    T("T071", "嵌套数组操作",
      R"(var a = [[1, 2], [3, 4]]; var b = [[5, 6], [7, 8]]; print(a[0][1] + b[1][0]);)",
      "9", "nested_array+index_access+arithmetic");

    // 验证: 字典值为数组，支持索引访问与 push 扩容
    T("T072", "字典包含数组",
      R"(var d = {}; d["nums"] = [10, 20, 30]; print(d["nums"][1]); d["nums"].push(40); print(d["nums"].len());)",
      "20\n4", "dict+array+nested_access+method_call");

    // 验证: 类对象数组，按索引访问成员变量
    T("T073", "对象数组",
      R"(class Pt { init(x, y) { this.x = x; this.y = y; } } var pts = [Pt(1, 2), Pt(3, 4)]; print(pts[0].x + pts[1].x); print(pts[1].y);)",
      "4\n4", "class+array+member_access+arithmetic");

    // 验证: 函数合并两个字典，返回含新键的字典
    T("T074", "字典合并",
      R"(fun merge(a, b) { var keys = b.keys(); for (var i = 0; i < keys.len(); i = i + 1) { a[keys[i]] = b[keys[i]]; } return a; } var d1 = {}; d1["a"] = 1; var d2 = {}; d2["b"] = 2; d2["c"] = 3; var result = merge(d1, d2); print(result["b"]);)",
      "2", "function+dict+loop+method_call+merge");

    // 验证: 用字符串键动态构造平方表字典
    T("T075", "平方表字典",
      R"(var sq = {}; for (var i = 1; i <= 5; i = i + 1) { sq["" + i] = i * i; } print(sq["3"]); print(sq["5"]);)",
      "9\n25", "loop+dict+string_key+arithmetic+dynamic_key");

    // 验证: 循环按正负将元素分区到两个数组
    T("T076", "数组分组(正/负)",
      R"(var pos = []; var neg = []; var arr = [3, -1, 4, -5, 2, -3]; for (var i = 0; i < arr.len(); i = i + 1) { if (arr[i] >= 0) { pos.push(arr[i]); } else { neg.push(arr[i]); } } print(pos.len()); print(neg.len());)",
      "3\n3", "array+loop+branching+method_call+partition");

    // 验证: 多层字典嵌套访问链读取配置
    T("T077", "嵌套字典访问链",
      R"(var cfg = {}; cfg["db"] = {}; cfg["db"]["host"] = "localhost"; cfg["db"]["port"] = 5432; print(cfg["db"]["host"]); print(cfg["db"]["port"]);)",
      "localhost\n5432", "nested_dict+string+number+index_chain");

    // 验证: 用字典去重，保留首次出现的元素
    T("T078", "数组去重",
      R"(fun dedup(arr) { var seen = {}; var r = []; for (var i = 0; i < arr.len(); i = i + 1) { var k = "" + arr[i]; if (seen[k] == null) { seen[k] = true; r.push(arr[i]); } } return r; } print(dedup([1,2,2,3,3,3,4]));)",
      "[1, 2, 3, 4]", "function+array+dict+loop+null_check+dedup");

    // 验证: 遍历字典 keys 累加各值（10+20+30=60）
    T("T079", "字典值求和",
      R"(var d = {}; d["a"] = 10; d["b"] = 20; d["c"] = 30; var keys = d.keys(); var sum = 0; for (var i = 0; i < keys.len(); i = i + 1) { sum = sum + d[keys[i]]; } print(sum);)",
      "60", "dict+loop+method_call+arithmetic+keys");

    // 验证: 数组转字符频率字典，统计不同字符数
    T("T080", "数组到字典映射",
      R"(var s = ["h","e","l","l","o"]; var freq = {}; for (var i = 0; i < s.len(); i = i + 1) { var c = s[i]; if (freq[c] == null) { freq[c] = 0; } freq[c] = freq[c] + 1; } var keys = freq.keys(); print(keys.len());)",
      "4", "array+dict+loop+null_check+method_call");

    // ═══════════════════════════════════════════════════════════
    // I. 复杂控制流 + 数组 + 函数 (T081-T090)
    // 本组目标: 验证数组分区、游程编码、矩阵乘法、幂运算、Tribonacci、数组旋转、进制转换等算法级组合
    // ═══════════════════════════════════════════════════════════

    // 验证: 按基准值将数组分为 [小于, 其余] 两部分
    T("T081", "数组分区",
      R"(fun partition(arr, pivot) { var lo = []; var hi = []; for (var i = 0; i < arr.len(); i = i + 1) { if (arr[i] < pivot) { lo.push(arr[i]); } else { hi.push(arr[i]); } } return [lo, hi]; } var r = partition([3,1,4,1,5,9,2], 4); print(r[0]); print(r[1]);)",
      "[3, 1, 1, 2]\n[4, 5, 9]", "function+array+loop+branching+partition");

    // 验证: 循环累计求和并输出滑动平均值
    T("T082", "累计平均值",
      R"(fun avg(arr) { var sum = 0; for (var i = 0; i < arr.len(); i = i + 1) { sum = sum + arr[i]; print(sum / (i + 1)); } } avg([10, 20, 30]);)",
      "10\n15\n20", "function+array+loop+arithmetic+division");

    // 验证: 双 while 实现游程长度编码（a3b2c1）
    T("T083", "数组游程编码",
      R"(fun rle(arr) { var r = ""; var i = 0; while (i < arr.len()) { var c = arr[i]; var cnt = 1; while (i + cnt < arr.len() and arr[i + cnt] == c) { cnt = cnt + 1; } r = r + c + ("" + cnt); i = i + cnt; } return r; } print(rle(["a","a","a","b","b","c"]));)",
      "a3b2c1", "function+array+while_loop+concat+nested_loop");

    // 验证: 三重循环实现 2x2 矩阵乘法
    T("T084", "矩阵乘法",
      R"(fun matmul(a, b) { var r = []; for (var i = 0; i < a.len(); i = i + 1) { var row = []; for (var j = 0; j < b[0].len(); j = j + 1) { var s = 0; for (var k = 0; k < a[0].len(); k = k + 1) { s = s + a[i][k] * b[k][j]; } row.push(s); } r.push(row); } return r; } print(matmul([[1,2],[3,4]], [[5,6],[7,8]]));)",
      "[[19, 22], [43, 50]]", "nested_loop+array+function+arithmetic+matrix");

    // 验证: 递归快速幂 power(2,10)=1024
    T("T085", "幂运算函数",
      R"(fun power(base, exp) { if (exp == 0) { return 1; } if (exp % 2 == 0) { var half = power(base, exp / 2); return half * half; } return base * power(base, exp - 1); } print(power(2, 10));)",
      "1024", "recursion+function+branching+modulo+arithmetic");

    // 验证: 循环按模筛选保留整除元素
    T("T086", "模运算筛选",
      R"(fun modFilter(arr, m) { var r = []; for (var i = 0; i < arr.len(); i = i + 1) { if (arr[i] % m == 0) { r.push(arr[i]); } } return r; } print(modFilter([1,2,3,4,5,6,7,8,9,10], 3));)",
      "[3, 6, 9]", "function+array+loop+modulo+branching");

    // 验证: 循环递推生成 Tribonacci 序列
    T("T087", "Tribonacci 序列",
      R"(fun tribo(n) { var a = [0, 0, 1]; for (var i = 3; i < n; i = i + 1) { a.push(a[i-1] + a[i-2] + a[i-3]); } return a; } print(tribo(8));)",
      "[0, 0, 1, 1, 2, 4, 7, 13]", "function+array+loop+arithmetic+sequence");

    // 验证: 循环将末尾元素移到头部实现数组旋转
    T("T088", "数组旋转",
      R"(fun rotate(arr, k) { for (var i = 0; i < k; i = i + 1) { var last = arr.pop(); var r = [last]; for (var j = 0; j < arr.len(); j = j + 1) { r.push(arr[j]); } arr = r; } return arr; } print(rotate([1,2,3,4,5], 2));)",
      "[4, 5, 1, 2, 3]", "function+array+loop+method_call+rotation");

    // 验证: 函数+字典去重，返回首次出现顺序
    T("T089", "数组去重(函数+字典)",
      R"(fun unique(arr) { var seen = {}; var r = []; for (var i = 0; i < arr.len(); i = i + 1) { var k = "" + arr[i]; if (seen[k] == null) { seen[k] = true; r.push(arr[i]); } } return r; } print(unique([5,3,5,1,3,5,1]));)",
      "[5, 3, 1]", "function+array+dict+loop+null_check+dedup");

    // 验证: 循环取模除 2 逆序拼接得到二进制串（13=1101）
    T("T090", "十进制转二进制",
      R"(fun toBin(n) { if (n == 0) { return "0"; } var r = ""; var num = n; while (num > 0) { r = ("" + (num % 2)) + r; num = num / 2; } return r; } print(toBin(13));)",
      "1101", "function+string+loop+modulo+division+concat");

    // ═══════════════════════════════════════════════════════════
    // J. 多特性综合压轴 (T091-T100)
    // 本组目标: 综合压轴——递归/类/状态机/解释器式求值器等最复杂组合，重点排查跨特性交互下的整体语义
    // ═══════════════════════════════════════════════════════════

    // 验证: 递归构造逆序区间 [5,4,3,2,1] 并求和 15
    T("T091", "递归生成区间",
      R"(fun range(s, e) { if (s > e) { return []; } var r = range(s + 1, e); r.push(s); return r; } var a = range(1, 5); print(a); var sum = 0; for (var i = 0; i < a.len(); i = i + 1) { sum = sum + a[i]; } print(sum);)",
      "[5, 4, 3, 2, 1]\n15", "recursion+array+loop+arithmetic");

    // 验证: Collatz 迭代步数（6 -> 3->10->5->16->8->4->2->1 共 8 步）
    T("T092", "Collatz 猜想计数",
      R"(fun collatz(n) { var count = 0; while (n != 1) { if (n % 2 == 0) { n = n / 2; } else { n = 3 * n + 1; } count = count + 1; } return count; } print(collatz(6));)",
      "8", "function+loop+branching+modulo+arithmetic");

    // 验证: 循环取各位数字求和（9876 -> 9+8+7+6=30）
    T("T093", "数字各位之和",
      R"(fun digitSum(n) { var sum = 0; while (n > 0) { sum = sum + n % 10; n = n / 10; } return sum; } print(digitSum(9876));)",
      "30", "function+loop+modulo+arithmetic+division");

    // 验证: 递归汉诺塔 4 盘共 2^4-1=15 步
    T("T094", "汉诺塔",
      R"(var moves = 0; fun hanoi(n, src, to, aux) { if (n == 1) { moves = moves + 1; return; } hanoi(n - 1, src, aux, to); moves = moves + 1; hanoi(n - 1, aux, to, src); } hanoi(4, "A", "C", "B"); print(moves);)",
      "15", "recursion+function+branching+variable+global_state");

    // 验证: 用字典标记 b 的元素求 a 与 b 的交集
    T("T095", "数组交集",
      R"(fun intersect(a, b) { var inB = {}; for (var i = 0; i < b.len(); i = i + 1) { inB["" + b[i]] = true; } var r = []; for (var i = 0; i < a.len(); i = i + 1) { if (inB["" + a[i]] == true) { r.push(a[i]); } } return r; } print(intersect([1,2,3,4,5], [3,4,5,6,7]));)",
      "[3, 4, 5]", "function+array+dict+loop+intersection");

    // 验证: 队列类 FIFO，pop 顺序 1、2，最终 size=1
    T("T096", "队列类",
      R"(class Queue { init() { this.data = []; } push(v) { this.data.push(v); } pop() { var first = this.data[0]; this.data.remove(0); return first; } size() { return this.data.len(); } } var q = Queue(); q.push(1); q.push(2); q.push(3); print(q.pop()); print(q.pop()); print(q.size());)",
      "1\n2\n1", "class+method+array+queue+FIFO");

    // 验证: 链表类头插法，toArray 返回正序 [1,2,3]
    T("T097", "链表类",
      R"(class Node { init(v) { this.val = v; this.next = null; } } class LList { init() { this.head = null; } push(v) { var n = Node(v); n.next = this.head; this.head = n; } toArray() { var r = []; var cur = this.head; while (cur != null) { r.push(cur.val); cur = cur.next; } return r; } } var l = LList(); l.push(3); l.push(2); l.push(1); print(l.toArray());)",
      "[1, 2, 3]", "class+linked_list+loop+null_check+method_chain");

    // 验证: 递归表达式求值器解析 3*2+4 = 10
    T("T098", "递归表达式求值器",
      R"(var tokens = [3, "*", 2, "+", 4]; var pos = [0]; fun advance() { pos[0] = pos[0] + 1; } fun current() { return tokens[pos[0]]; } fun evalExpr() { var result = 0; while (pos[0] < tokens.len()) { var tok = current(); if (tok == "+") { advance(); var right = evalTerm(); result = result + right; } else if (tok == "*") { advance(); var right = evalTerm(); result = result * right; } else { advance(); result = tok; } } return result; } fun evalTerm() { var tok = current(); advance(); return tok; } print(evalExpr());)",
      "10", "recursion+function+array+loop+branching+evaluator+global_state");

    // 验证: 多态形状描述与面积计算（矩形 3x4=12，圆 3*25=75）
    T("T099", "多态形状描述",
      R"(class Shape { describe() { return "shape"; } } class Rect : Shape { init(w, h) { this.w = w; this.h = h; } describe() { return "rect " + ("" + this.w) + "x" + ("" + this.h); } area() { return this.w * this.h; } } class Cir : Shape { init(r) { this.r = r; } describe() { return "circle r=" + ("" + this.r); } area() { return 3 * this.r * this.r; } } var shapes = [Rect(3, 4), Cir(5)]; for (var i = 0; i < shapes.len(); i = i + 1) { print(shapes[i].describe()); print(shapes[i].area()); })",
      "rect 3x4\n12\ncircle r=5\n75", "class+polymorphism+inheritance+string+loop+array");

    // 验证: 三态状态机按输入在各状态间转移并统计停留次数
    T("T100", "状态机(循环+分支+数组+字典)",
      R"(fun run(input) { var state = 0; var counts = {}; counts["0"] = 0; counts["1"] = 0; counts["2"] = 0; for (var i = 0; i < input.len(); i = i + 1) { var v = input[i]; if (state == 0) { if (v > 0) { state = 1; } else if (v < 0) { state = 2; } } else if (state == 1) { if (v < 0) { state = 2; } else if (v == 0) { state = 0; } } else { if (v > 0) { state = 1; } else if (v == 0) { state = 0; } } counts["" + state] = counts["" + state] + 1; } print(counts["0"]); print(counts["1"]); print(counts["2"]); } run([1, -1, 0, 2, -3, 0, 4]);)",
      "2\n3\n2", "class+state_machine+loop+branching+dict+array");

    // DIAGNOSTIC: this.member persistence（诊断用，验证成员在 init/方法/累加中的持久化）
    // 验证: init 中赋值的 this.x 在构造后可见
    T("DIA1", "simple this.member assign",
      R"(class C { init() { this.x = 42; } } var c = C(); print(c.x);)",
      "42", "diag");

    // 验证: 方法内通过 this.x 赋值并持久化
    T("DIA2", "this.member in method",
      R"(class C { init() { this.x = 0; } set(v) { this.x = v; } } var c = C(); c.set(42); print(c.x);)",
      "42", "diag");

    // 验证: 方法内 this.x = this.x + n 自增持久化
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

    // 逐个运行：错误（词法/语法/运行时）与输出不匹配都计为失败，并收集期望/实际以便排查
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
