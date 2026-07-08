// diff_test.cpp — MiniLang 差分一致性测试（解释器 vs 编译器+VM）
// 覆盖模块: 解释器与 Compiler+VM 两条后端对同一批约 80 个程序的行为一致性。
// 验证目标: 同一源码送两条后端，比较「错误状态 / 打印输出 / 全局变量终值 / 错误消息内容」四方面，
//           要求逐字节一致；任一后端行为分歧都记为 DIFFER，用于捕获后端实现偏差。
// MiniLang Differential Test: Interpreter vs Compiler+VM
// Runs 80 programs through both paths, compares output + global variable state

#include <iostream>
#include <string>
#include <vector>
#include <functional>
#include <sstream>
#include <memory>
#include <unordered_map>

#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "ast/ASTNode.h"
#include "interpreter/Interpreter.h"
#include "interpreter/Value.h"
#include "compiler/Compiler.h"
#include "compiler/VM.h"
#include "Diagnostic.h"

// ─── Execution result ───────────────────────────────────────────────────

struct ExecResult {
    std::string output;
    std::string error;
    bool hasError = false;
    std::unordered_map<std::string, std::string> globals; // name → toString()
};

// ─── Run through Interpreter ────────────────────────────────────────────

static ExecResult runInterpreter(const std::string& source) {
    ExecResult res;
    Lexer lexer;
    auto tokens = lexer.scan(source);
    if (lexer.getDiagnostics().hasErrors()) {
        res.hasError = true;
        res.error = "LEX:" + lexer.getDiagnostics().summary();
        return res;
    }
    Parser parser;
    auto ast = parser.parse(tokens);
    if (parser.hasErrors() || !ast) {
        res.hasError = true;
        std::string e;
        for (auto& pe : parser.getDiagnostics().all()) { if (!e.empty()) e += "; "; e += pe.message; }
        res.error = "PARSE:" + (e.empty() ? "unknown" : e);
        return res;
    }
    Interpreter interp;
    std::string captured;
    interp.setOutputCallback([&captured](const std::string& s) { captured += s + "\n"; });
    try {
        interp.execute(*ast);
        // Collect globals
        auto* env = interp.currentEnvironment();
        if (env) {
            for (auto& [name, val] : env->localVariables()) {
                res.globals[name] = val.toString();
            }
        }
        res.output = captured;
    } catch (const RuntimeError& e) {
        res.hasError = true;
        res.error = std::string("RT:") + e.what();
    } catch (const std::exception& e) {
        res.hasError = true;
        res.error = std::string("EX:") + e.what();
    }
    return res;
}

// ─── Run through Compiler + VM ──────────────────────────────────────────

static ExecResult runVM(const std::string& source) {
    ExecResult res;
    Lexer lexer;
    auto tokens = lexer.scan(source);
    if (lexer.getDiagnostics().hasErrors()) {
        res.hasError = true;
        res.error = "LEX:" + lexer.getDiagnostics().summary();
        return res;
    }
    Parser parser;
    auto ast = parser.parse(tokens);
    if (parser.hasErrors() || !ast) {
        res.hasError = true;
        std::string e;
        for (auto& pe : parser.getDiagnostics().all()) { if (!e.empty()) e += "; "; e += pe.message; }
        res.error = "PARSE:" + (e.empty() ? "unknown" : e);
        return res;
    }
    Compiler compiler;
    auto compileResult = compiler.compile(*ast);
    if (compiler.getDiagnostics().hasErrors()) {
        res.hasError = true;
        res.error = "COMPILE:" + compiler.getLastError();
        return res;
    }
    VM vm;
    std::string captured;
    vm.setOutputCallback([&captured](const std::string& s) { captured += s + "\n"; });
    VMResult vmRes = vm.execute(compileResult);
    if (vmRes == VMResult::VM_RUNTIME_ERROR) {
        res.hasError = true;
        res.error = std::string("VMRT:") + vm.getLastError();
    } else {
        // Collect globals
        for (auto& [name, val] : vm.getGlobalsRef()) {
            res.globals[name] = val.toString();
        }
        res.output = captured;
    }
    return res;
}

// ─── Test case definition ───────────────────────────────────────────────

struct TestCase {
    std::string id;
    std::string name;
    std::string source;
};

// 构造约 80 个用例：覆盖变量/算术/比较逻辑/分支/循环/函数/字符串/数组/混合等族。
// 这些用例会分别送解释器与编译器+VM 两条后端执行，用于做差分一致性校验。
static std::vector<TestCase> buildTests() {
    std::vector<TestCase> tests;
    auto T = [&](const char* id, const char* name, const char* src) {
        tests.push_back({id, name, src});
    };

    // ═══════════ Variables & Assignments (V1-V10) ═══════════
    // 变量与赋值族：验证变量声明、重赋值、复合表达式、块内与嵌套遮蔽在两后端输出与全局态一致。
    // 验证: 整型变量声明与打印在两后端一致
    T("V1","int var","var x = 42; print(x);");
    // 验证: 浮点变量在两后端一致
    T("V2","float var","var f = 3.14; print(f);");
    // 验证: 字符串变量在两后端一致
    T("V3","string var","var s = \"hello\"; print(s);");
    // 验证: 布尔变量在两后端一致
    T("V4","bool var","var b = true; print(b);");
    // 验证: null 变量在两后端一致
    T("V5","null var","var n = null; print(n);");
    // 验证: 变量重赋值在两后端一致
    T("V6","reassign","var x = 1; x = 2; print(x);");
    // 验证: 多变量相加在两后端一致
    T("V7","multi assign","var a = 1; var b = 2; var c = a + b; print(c);");
    // 验证: 复合赋值表达式在两后端一致
    T("V8","assign expr","var x = 10; x = x * 2 + 1; print(x);");
    // 验证: 块内变量遮蔽在两后端一致
    T("V9","block shadow","var x = 1; { var x = 99; print(x); } print(x);");
    // 验证: 双层嵌套遮蔽在两后端一致
    T("V10","nested shadow","var x=1; { var x=2; { var x=3; print(x); } print(x); } print(x);");

    // ═══════════ Arithmetic (A1-A15) ═══════════
    // 算术族：验证整数/浮点除法、取模、一元正负、括号分组、字符串拼接等在两后端数值完全一致。
    // 验证: 整数加法在两后端一致
    T("A1","add","print(3 + 4);");
    // 验证: 整数减法在两后端一致
    T("A2","sub","print(10 - 7);");
    // 验证: 整数乘法在两后端一致
    T("A3","mul","print(6 * 7);");
    // 验证: 整数除法在两后端一致（3）
    T("A4","div int","print(15 / 4);");
    // 验证: 浮点除法在两后端一致
    T("A5","div float","print(15.0 / 4);");
    // 验证: 取模运算在两后端一致
    T("A6","mod","print(17 % 5);");
    // 验证: 混合算术优先级在两后端一致
    T("A7","mixed arith","print(2 + 3 * 4 - 1);");
    // 验证: 括号分组在两后端一致
    T("A8","paren","print((2 + 3) * (4 - 1));");
    // 验证: 一元负号在两后端一致
    T("A9","negate","var x = 5; print(-x);");
    // 验证: 一元正号在两后端一致
    T("A10","unary plus","print(+(3));");
    // 验证: 浮点加法在两后端一致
    T("A11","float add","print(1.5 + 2.5);");
    // 验证: 整数+浮点隐式转换在两后端一致
    T("A12","int+float","print(1 + 2.5);");
    // 验证: 字符串拼接在两后端一致
    T("A13","str concat","print(\"ab\" + \"cd\");");
    // 验证: 字符串+整数拼接在两后端一致
    T("A14","str+int","print(\"n=\" + 42);");
    // 验证: 复杂括号算术在两后端一致（(80)/(4)*2=40）
    T("A15","complex arith","var r = (100 - 20) / (3 + 1) * 2; print(r);");

    // ═══════════ Comparisons & Logic (C1-C10) ═══════════
    // 比较与逻辑族：验证 ==/!=/</>/<=/>= 与 and/or/not 的布尔结果在两后端一致。
    // 验证: 相等比较为真在两后端一致
    T("C1","eq true","print(1 == 1);");
    // 验证: 相等比较为假在两后端一致
    T("C2","eq false","print(1 == 2);");
    // 验证: 不等比较在两后端一致
    T("C3","neq","print(1 != 2);");
    // 验证: 小于比较在两后端一致
    T("C4","lt","print(3 < 5);");
    // 验证: 大于比较在两后端一致
    T("C5","gt","print(5 > 3);");
    // 验证: 小于等于比较在两后端一致
    T("C6","lte","print(5 <= 5);");
    // 验证: 大于等于比较在两后端一致
    T("C7","gte","print(5 >= 6);");
    // 验证: 逻辑 and 在两后端一致
    T("C8","and","print(true and false);");
    // 验证: 逻辑 or 在两后端一致
    T("C9","or","print(false or true);");
    // 验证: 逻辑 not 在两后端一致
    T("C10","not","print(not true);");

    // ═══════════ Branches (B1-B10) ═══════════
    // 分支族：验证 if/else/else-if、悬空 else、带表达式分支，以及 0/空串/null 的真值判定在两后端一致。
    // 验证: if true 分支在两后端一致
    T("B1","if true","if (true) { print(1); }");
    // 验证: if false 跳过分支在两后端一致
    T("B2","if false","if (false) { print(1); } print(0);");
    // 验证: if-else 分支在两后端一致
    T("B3","if else","if (1 > 2) { print(1); } else { print(2); }");
    // 验证: else-if 多分支在两后端一致
    T("B4","else if","var x=2; if(x==1){print(1);}else if(x==2){print(2);}else{print(3);}");
    // 验证: 嵌套 if 在两后端一致
    T("B5","nested if","if(true){if(false){print(1);}else{print(2);}}");
    // 验证: 悬空 else 绑定在两后端一致
    T("B6","dangling else","if(false){if(true){print(1);}}else{print(2);}");
    // 验证: 带表达式分支在两后端一致
    T("B7","if with expr","var x=10; if(x>5){print(x+1);}else{print(x-1);}");
    // 验证: 0 为假值分支在两后端一致
    T("B8","truthy 0","if(0){print(\"t\");}else{print(\"f\");}");
    // 验证: 空串为假值分支在两后端一致
    T("B9","truthy str","if(\"\"){print(\"t\");}else{print(\"f\");}");
    // 验证: null 为假值分支在两后端一致
    T("B10","truthy null","if(null){print(\"t\");}else{print(\"f\");}");

    // ═══════════ Loops (L1-L10) ═══════════
    // 循环族：验证 while/for（含提前返回、缺更新段、循环内 if、无限循环退出）在两后端行为一致。
    // 验证: while 基础循环在两后端一致
    T("L1","while basic","var i=0; while(i<5){print(i); i=i+1;}");
    // 验证: while 求和在两后端一致（55）
    T("L2","while sum","var s=0; var i=1; while(i<=10){s=s+i; i=i+1;} print(s);");
    // 验证: while false 不进入循环在两后端一致
    T("L3","while false","var x=42; while(false){x=0;} print(x);");
    // 验证: for 基础循环在两后端一致
    T("L4","for basic","for(var i=0;i<5;i=i+1){print(i);}");
    // 验证: for 求和在两后端一致（5050）
    T("L5","for sum","var s=0; for(var i=1;i<=10;i=i+1){s=s+i;} print(s);");
    // 验证: 嵌套 for 在两后端一致
    T("L6","for nested","for(var i=0;i<3;i=i+1){for(var j=0;j<2;j=j+1){print(i*10+j);}}");
    // 验证: while 倒计时在两后端一致
    T("L7","while countdown","var n=5; while(n>0){print(n); n=n-1;}");
    // 验证: 缺更新段的 for 循环在两后端一致
    T("L8","for no update","var c=0; for(var i=0;i<3;){i=i+1; c=c+10;} print(c);");
    // 验证: 循环内 if 在两后端一致
    T("L9","loop with if","for(var i=0;i<10;i=i+1){if(i%2==0){print(i);}}");
    // 验证: while 无限循环提前返回在两后端一致
    T("L10","while early exit","fun f(){var i=0;while(true){if(i>=3){return i;}i=i+1;}} print(f());");

    // ═══════════ Functions (F1-F15) ═══════════
    // 函数族：验证递归/闭包/嵌套调用/互递归/void 函数/类型参数/返回类型/变量遮蔽在两后端一致。
    // 验证: 基本函数调用在两后端一致
    T("F1","basic fun","fun add(a,b){return a+b;} print(add(3,4));");
    // 验证: 无参函数在两后端一致
    T("F2","no args","fun pi(){return 3;} print(pi());");
    // 验证: 无返回值函数（null）在两后端一致
    T("F3","no return","fun f(){var x=1;} print(f());");
    // 验证: 递归阶乘在两后端一致（fact(6)=720）
    T("F4","factorial","fun fact(n){if(n<=1){return 1;}return n*fact(n-1);} print(fact(6));");
    // 验证: 递归斐波那契在两后端一致（fib(10)=55）
    T("F5","fibonacci","fun fib(n){if(n<=1){return n;}return fib(n-1)+fib(n-2);} print(fib(10));");
    // 验证: 闭包捕获外层变量在两后端一致
    T("F6","closure","fun make(){var x=10;fun inner(){return x;}return inner;} var f=make(); print(f());");
    // 验证: 嵌套函数调用在两后端一致
    T("F7","nested call","fun a(x){return x+1;} fun b(x){return a(x)*2;} print(b(5));");
    // 验证: 多参数函数在两后端一致
    T("F8","multi param","fun f(a,b,c){return a+b+c;} print(f(1,2,3));");
    // 验证: 函数作为表达式组合在两后端一致
    T("F9","fun as expr","fun sq(x){return x*x;} print(sq(3)+sq(4));");
    // 验证: void 函数全局副作用在两后端一致
    T("F10","void fun","var s=0; fun add(v){s=s+v;} add(10); add(20); print(s);");
    // 验证: 深递归在两后端一致（f(30)=0）
    T("F11","recursion depth","fun f(n){if(n==0){return 0;}return f(n-1);} print(f(30));");
    // 验证: 带类型参数函数在两后端一致
    T("F12","typed params","fun add(int a,int b){return a+b;} print(add(7,8));");
    // 验证: 互递归在两后端一致
    T("F13","mutual rec","fun isEven(n){if(n==0){return true;}return isOdd(n-1);} fun isOdd(n){if(n==0){return false;}return isEven(n-1);} print(isEven(4)); print(isOdd(3));");
    // 验证: 函数内变量遮蔽在两后端一致
    T("F14","fun shadow","var x=1; fun f(){var x=100; return x;} print(f()); print(x);");
    // 验证: 循环内 return 提前退出在两后端一致
    T("F15","return in loop","fun first(lim){for(var i=0;;i=i+1){if(i>=lim){return i;}}} print(first(7));");

    // ═══════════ Strings (S1-S5) ═══════════
    // 字符串族：验证 len/下标/index/contains/substr/replace 等字符串方法在两后端结果一致。
    // 验证: 字符串 len 在两后端一致
    T("S1","str len","var s=\"hello\"; print(s.len());");
    // 验证: 字符串下标访问在两后端一致
    T("S2","str index","var s=\"abcde\"; print(s[2]);");
    // 验证: 字符串 contains 在两后端一致
    T("S3","str contains","var s=\"hello world\"; print(s.contains(\"world\"));");
    // 验证: 字符串 substr 在两后端一致
    T("S4","str substring","var s=\"abcdef\"; print(s.substr(2,3));");
    // 验证: 字符串 replace 在两后端一致
    T("S5","str replace","var s=\"aabbcc\"; print(s.replace(\"bb\",\"XX\"));");

    // ═══════════ Arrays (R1-R10) ═══════════
    // 数组族：验证字面量、len、push、下标赋值、遍历、pop、嵌套数组、空数组、字符串数组在两后端一致。
    // 验证: 数组字面量索引在两后端一致
    T("R1","array lit","var a=[1,2,3]; print(a[0]); print(a[1]); print(a[2]);");
    // 验证: 数组 len 在两后端一致
    T("R2","array len","print([10,20,30].len());");
    // 验证: 数组 push 扩容在两后端一致
    T("R3","array push","var a=[1]; a.push(2); a.push(3); print(a.len());");
    // 验证: 数组下标赋值在两后端一致
    T("R4","array set","var a=[1,2,3]; a[1]=99; print(a[1]);");
    // 验证: 数组遍历在两后端一致
    T("R5","array iter","var a=[10,20,30]; for(var i=0;i<a.len();i=i+1){print(a[i]);}");
    // 验证: 嵌套数组索引在两后端一致
    T("R6","array nested","var a=[[1,2],[3,4]]; print(a[0][1]); print(a[1][0]);");
    // 验证: 空数组与动态扩容在两后端一致
    T("R7","array empty","var a=[]; print(a.len()); a.push(1); print(a.len());");
    // 验证: 数组 pop 在两后端一致
    T("R8","array pop","var a=[1,2,3]; var v=a.pop(); print(v); print(a.len());");
    // 验证: 字符串数组拼接在两后端一致
    T("R9","array of str","var a=[\"a\",\"b\",\"c\"]; print(a[0]+a[1]+a[2]);");
    // 验证: 循环构建数组在两后端一致
    T("R10","array build","var a=[]; for(var i=0;i<5;i=i+1){a.push(i*i);} print(a[4]);");

    // ═══════════ Mixed / Complex (M1-M10) ═══════════
    // 混合/复杂族：验证组合算法（GCD、质数判定、Collatz、幂运算、全局副作用、成绩分级等）在两后端一致。
    // 验证: 变量+算术+打印混合在两后端一致
    T("M1","var+arith+print","var x=10; var y=3; print(x/y); print(x%y);");
    // 验证: 循环+函数求和在两后端一致（sum(100)=5050）
    T("M2","loop+fun","fun sum(n){var s=0; for(var i=1;i<=n;i=i+1){s=s+i;} return s;} print(sum(100));");
    // 验证: 循环中字符串累加在两后端一致
    T("M3","str in loop","var s=\"\"; for(var i=0;i<3;i=i+1){s=s+\"x\";} print(s);");
    // 验证: GCD 算法在两后端一致（gcd(48,18)=6）
    T("M4","gcd","fun gcd(a,b){while(b!=0){var t=b; b=a%b; a=t;} return a;} print(gcd(48,18));");
    // 验证: 质数判定在两后端一致
    T("M5","prime check","fun isPrime(n){if(n<2){return false;} for(var i=2;i*i<=n;i=i+1){if(n%i==0){return false;}} return true;} print(isPrime(17)); print(isPrime(15));");
    // 验证: Collatz 步数在两后端一致（collatz(27)=111）
    T("M6","collatz","fun collatz(n){var c=0; while(n!=1){if(n%2==0){n=n/2;}else{n=n*3+1;} c=c+1;} return c;} print(collatz(27));");
    // 验证: 幂运算在两后端一致（pow(2,10)=1024）
    T("M7","power","fun pow(b,e){var r=1; for(var i=0;i<e;i=i+1){r=r*b;} return r;} print(pow(2,10));");
    // 验证: 数组参数交换副作用在两后端一致
    T("M8","swap via array","fun swap(a){var t=a[0]; a[0]=a[1]; a[1]=t;} var p=[10,20]; swap(p); print(p[0]); print(p[1]);");
    // 验证: 嵌套 if-else 分级在两后端一致
    T("M9","nested if-else chain","var g=85; if(g>=90){print(\"A\");}else if(g>=80){print(\"B\");}else if(g>=70){print(\"C\");}else{print(\"F\");}");
    // 验证: 全局计数器副作用在两后端一致
    T("M10","global side effect","var cnt=0; fun inc(){cnt=cnt+1;} inc(); inc(); inc(); print(cnt);");

    return tests;
}

// ─── Main: differential comparison ──────────────────────────────────────

int main() {
    auto tests = buildTests();
    std::cout << "MiniLang Differential Test: Interpreter vs Compiler+VM\n";
    std::cout << "======================================================\n";
    std::cout << "Test count: " << tests.size() << "\n\n";

    int pass = 0, fail = 0, skip = 0;
    struct Failure {
        std::string id, name;
        std::string interpOut, interpErr;
        std::string vmOut, vmErr;
        std::string diffDetail;
    };
    std::vector<Failure> failures;

    // 对每个用例同时跑两条后端，依次比较：错误状态、打印输出、全局变量 final 值。
    // 全局变量仅比较两者共有的键，验证语义在两条后端间逐字节一致。
    for (auto& tc : tests) {
        auto ir = runInterpreter(tc.source);
        auto vr = runVM(tc.source);

        bool match = true;
        std::string detail;

        // Compare error state
        if (ir.hasError != vr.hasError) {
            match = false;
            detail = "error-state mismatch: interp=" + std::string(ir.hasError?"ERR":"OK")
                   + " vm=" + std::string(vr.hasError?"ERR":"OK");
        }
        // Compare output (if both ran OK)
        if (!ir.hasError && !vr.hasError) {
            if (ir.output != vr.output) {
                match = false;
                detail = "output mismatch";
            }
            // Compare globals (only keys present in both)
            for (auto& [k, iv] : ir.globals) {
                auto it = vr.globals.find(k);
                if (it != vr.globals.end()) {
                    if (iv != it->second) {
                        match = false;
                        if (!detail.empty()) detail += "; ";
                        detail += "global '" + k + "': interp=" + iv + " vm=" + it->second;
                    }
                }
            }
        }
        // Error-path consistency: when both backends error, verify the error
        // category AND message content are consistent.
        // AUDIT-ERRPATH fix: 原实现仅比较错误类别前缀（LEX/PARSE/RT/EX），导致
        // 两条语义完全不同的 RT 错误（如 "未定义的变量: x" vs "类型错误"）被判为一致。
        // 修复：类别不同 → 一定不一致；类别相同（RT）→ 进一步比较错误消息内容。
        // 三后端已统一错误消息（BUG-1 fix），RT 错误消息应完全一致。
        if (ir.hasError && vr.hasError) {
            auto categoryOf = [](const std::string& err) -> std::string {
                if (err.rfind("LEX:", 0) == 0) return "LEX";
                if (err.rfind("PARSE:", 0) == 0) return "PARSE";
                if (err.rfind("RT:", 0) == 0) return "RT";
                if (err.rfind("EX:", 0) == 0) return "EX";
                return "OTHER";
            };
            std::string irCat = categoryOf(ir.error);
            std::string vrCat = categoryOf(vr.error);
            if (irCat != vrCat) {
                match = false;
                detail = "error-category mismatch: interp=" + irCat
                       + " (" + ir.error + ") vm=" + vrCat
                       + " (" + vr.error + ")";
            } else if (irCat == "RT" && ir.error != vr.error) {
                // AUDIT-ERRPATH: RT 错误消息应完全一致（三后端已统一）。
                // 消息不一致表示同一运行时错误在后端间有不同描述——真正的语义分歧。
                match = false;
                detail = "error-message mismatch: interp='" + ir.error
                       + "' vm='" + vr.error + "'";
            }
        }

        if (match) {
            pass++;
            std::cout << "PASS " << tc.id << " " << tc.name << "\n";
        } else {
            fail++;
            std::cout << "FAIL " << tc.id << " " << tc.name << "\n";
            failures.push_back({tc.id, tc.name,
                ir.output, ir.error,
                vr.output, vr.error,
                detail});
        }
    }

    // Summary
    std::cout << "\n======================================================\n";
    std::cout << "RESULTS: " << pass << " match, " << fail << " differ, "
              << skip << " skip, " << (pass+fail+skip) << " total\n";
    std::cout << "======================================================\n";

    if (!failures.empty()) {
        std::cout << "\nDIFFERENCES:\n";
        std::cout << "────────────\n";
        for (auto& f : failures) {
            std::cout << "[" << f.id << "] " << f.name << "\n";
            std::cout << "  Detail:  " << f.diffDetail << "\n";
            if (!f.interpOut.empty())
                std::cout << "  Interp output: [" << f.interpOut.substr(0, f.interpOut.size()-1) << "]\n";
            if (!f.interpErr.empty())
                std::cout << "  Interp error:  " << f.interpErr << "\n";
            if (!f.vmOut.empty())
                std::cout << "  VM output:     [" << f.vmOut.substr(0, f.vmOut.size()-1) << "]\n";
            if (!f.vmErr.empty())
                std::cout << "  VM error:      " << f.vmErr << "\n";
            std::cout << "\n";
        }
    }

    return fail > 0 ? 1 : 0;
}
