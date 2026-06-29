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
        for (auto& pe : parser.getErrors()) { if (!e.empty()) e += "; "; e += pe.what(); }
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
        for (auto& pe : parser.getErrors()) { if (!e.empty()) e += "; "; e += pe.what(); }
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

static std::vector<TestCase> buildTests() {
    std::vector<TestCase> tests;
    auto T = [&](const char* id, const char* name, const char* src) {
        tests.push_back({id, name, src});
    };

    // ═══════════ Variables & Assignments (V1-V10) ═══════════
    T("V1","int var","var x = 42; print(x);");
    T("V2","float var","var f = 3.14; print(f);");
    T("V3","string var","var s = \"hello\"; print(s);");
    T("V4","bool var","var b = true; print(b);");
    T("V5","null var","var n = null; print(n);");
    T("V6","reassign","var x = 1; x = 2; print(x);");
    T("V7","multi assign","var a = 1; var b = 2; var c = a + b; print(c);");
    T("V8","assign expr","var x = 10; x = x * 2 + 1; print(x);");
    T("V9","block shadow","var x = 1; { var x = 99; print(x); } print(x);");
    T("V10","nested shadow","var x=1; { var x=2; { var x=3; print(x); } print(x); } print(x);");

    // ═══════════ Arithmetic (A1-A15) ═══════════
    T("A1","add","print(3 + 4);");
    T("A2","sub","print(10 - 7);");
    T("A3","mul","print(6 * 7);");
    T("A4","div int","print(15 / 4);");
    T("A5","div float","print(15.0 / 4);");
    T("A6","mod","print(17 % 5);");
    T("A7","mixed arith","print(2 + 3 * 4 - 1);");
    T("A8","paren","print((2 + 3) * (4 - 1));");
    T("A9","negate","var x = 5; print(-x);");
    T("A10","unary plus","print(+(3));");
    T("A11","float add","print(1.5 + 2.5);");
    T("A12","int+float","print(1 + 2.5);");
    T("A13","str concat","print(\"ab\" + \"cd\");");
    T("A14","str+int","print(\"n=\" + 42);");
    T("A15","complex arith","var r = (100 - 20) / (3 + 1) * 2; print(r);");

    // ═══════════ Comparisons & Logic (C1-C10) ═══════════
    T("C1","eq true","print(1 == 1);");
    T("C2","eq false","print(1 == 2);");
    T("C3","neq","print(1 != 2);");
    T("C4","lt","print(3 < 5);");
    T("C5","gt","print(5 > 3);");
    T("C6","lte","print(5 <= 5);");
    T("C7","gte","print(5 >= 6);");
    T("C8","and","print(true and false);");
    T("C9","or","print(false or true);");
    T("C10","not","print(not true);");

    // ═══════════ Branches (B1-B10) ═══════════
    T("B1","if true","if (true) { print(1); }");
    T("B2","if false","if (false) { print(1); } print(0);");
    T("B3","if else","if (1 > 2) { print(1); } else { print(2); }");
    T("B4","else if","var x=2; if(x==1){print(1);}else if(x==2){print(2);}else{print(3);}");
    T("B5","nested if","if(true){if(false){print(1);}else{print(2);}}");
    T("B6","dangling else","if(false){if(true){print(1);}}else{print(2);}");
    T("B7","if with expr","var x=10; if(x>5){print(x+1);}else{print(x-1);}");
    T("B8","truthy 0","if(0){print(\"t\");}else{print(\"f\");}");
    T("B9","truthy str","if(\"\"){print(\"t\");}else{print(\"f\");}");
    T("B10","truthy null","if(null){print(\"t\");}else{print(\"f\");}");

    // ═══════════ Loops (L1-L10) ═══════════
    T("L1","while basic","var i=0; while(i<5){print(i); i=i+1;}");
    T("L2","while sum","var s=0; var i=1; while(i<=10){s=s+i; i=i+1;} print(s);");
    T("L3","while false","var x=42; while(false){x=0;} print(x);");
    T("L4","for basic","for(var i=0;i<5;i=i+1){print(i);}");
    T("L5","for sum","var s=0; for(var i=1;i<=10;i=i+1){s=s+i;} print(s);");
    T("L6","for nested","for(var i=0;i<3;i=i+1){for(var j=0;j<2;j=j+1){print(i*10+j);}}");
    T("L7","while countdown","var n=5; while(n>0){print(n); n=n-1;}");
    T("L8","for no update","var c=0; for(var i=0;i<3;){i=i+1; c=c+10;} print(c);");
    T("L9","loop with if","for(var i=0;i<10;i=i+1){if(i%2==0){print(i);}}");
    T("L10","while early exit","fun f(){var i=0;while(true){if(i>=3){return i;}i=i+1;}} print(f());");

    // ═══════════ Functions (F1-F15) ═══════════
    T("F1","basic fun","fun add(a,b){return a+b;} print(add(3,4));");
    T("F2","no args","fun pi(){return 3;} print(pi());");
    T("F3","no return","fun f(){var x=1;} print(f());");
    T("F4","factorial","fun fact(n){if(n<=1){return 1;}return n*fact(n-1);} print(fact(6));");
    T("F5","fibonacci","fun fib(n){if(n<=1){return n;}return fib(n-1)+fib(n-2);} print(fib(10));");
    T("F6","closure","fun make(){var x=10;fun inner(){return x;}return inner;} var f=make(); print(f());");
    T("F7","nested call","fun a(x){return x+1;} fun b(x){return a(x)*2;} print(b(5));");
    T("F8","multi param","fun f(a,b,c){return a+b+c;} print(f(1,2,3));");
    T("F9","fun as expr","fun sq(x){return x*x;} print(sq(3)+sq(4));");
    T("F10","void fun","var s=0; fun add(v){s=s+v;} add(10); add(20); print(s);");
    T("F11","recursion depth","fun f(n){if(n==0){return 0;}return f(n-1);} print(f(30));");
    T("F12","typed params","fun add(int a,int b){return a+b;} print(add(7,8));");
    T("F13","mutual rec","fun isEven(n){if(n==0){return true;}return isOdd(n-1);} fun isOdd(n){if(n==0){return false;}return isEven(n-1);} print(isEven(4)); print(isOdd(3));");
    T("F14","fun shadow","var x=1; fun f(){var x=100; return x;} print(f()); print(x);");
    T("F15","return in loop","fun first(lim){for(var i=0;;i=i+1){if(i>=lim){return i;}}} print(first(7));");

    // ═══════════ Strings (S1-S5) ═══════════
    T("S1","str len","var s=\"hello\"; print(s.len());");
    T("S2","str index","var s=\"abcde\"; print(s[2]);");
    T("S3","str contains","var s=\"hello world\"; print(s.contains(\"world\"));");
    T("S4","str substring","var s=\"abcdef\"; print(s.substr(2,3));");
    T("S5","str replace","var s=\"aabbcc\"; print(s.replace(\"bb\",\"XX\"));");

    // ═══════════ Arrays (R1-R10) ═══════════
    T("R1","array lit","var a=[1,2,3]; print(a[0]); print(a[1]); print(a[2]);");
    T("R2","array len","print([10,20,30].len());");
    T("R3","array push","var a=[1]; a.push(2); a.push(3); print(a.len());");
    T("R4","array set","var a=[1,2,3]; a[1]=99; print(a[1]);");
    T("R5","array iter","var a=[10,20,30]; for(var i=0;i<a.len();i=i+1){print(a[i]);}");
    T("R6","array nested","var a=[[1,2],[3,4]]; print(a[0][1]); print(a[1][0]);");
    T("R7","array empty","var a=[]; print(a.len()); a.push(1); print(a.len());");
    T("R8","array pop","var a=[1,2,3]; var v=a.pop(); print(v); print(a.len());");
    T("R9","array of str","var a=[\"a\",\"b\",\"c\"]; print(a[0]+a[1]+a[2]);");
    T("R10","array build","var a=[]; for(var i=0;i<5;i=i+1){a.push(i*i);} print(a[4]);");

    // ═══════════ Mixed / Complex (M1-M10) ═══════════
    T("M1","var+arith+print","var x=10; var y=3; print(x/y); print(x%y);");
    T("M2","loop+fun","fun sum(n){var s=0; for(var i=1;i<=n;i=i+1){s=s+i;} return s;} print(sum(100));");
    T("M3","str in loop","var s=\"\"; for(var i=0;i<3;i=i+1){s=s+\"x\";} print(s);");
    T("M4","gcd","fun gcd(a,b){while(b!=0){var t=b; b=a%b; a=t;} return a;} print(gcd(48,18));");
    T("M5","prime check","fun isPrime(n){if(n<2){return false;} for(var i=2;i*i<=n;i=i+1){if(n%i==0){return false;}} return true;} print(isPrime(17)); print(isPrime(15));");
    T("M6","collatz","fun collatz(n){var c=0; while(n!=1){if(n%2==0){n=n/2;}else{n=n*3+1;} c=c+1;} return c;} print(collatz(27));");
    T("M7","power","fun pow(b,e){var r=1; for(var i=0;i<e;i=i+1){r=r*b;} return r;} print(pow(2,10));");
    T("M8","swap via array","fun swap(a){var t=a[0]; a[0]=a[1]; a[1]=t;} var p=[10,20]; swap(p); print(p[0]); print(p[1]);");
    T("M9","nested if-else chain","var g=85; if(g>=90){print(\"A\");}else if(g>=80){print(\"B\");}else if(g>=70){print(\"C\");}else{print(\"F\");}");
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
        // category is consistent. The previous logic skipped all comparison
        // when both hadError, letting two completely different error messages
        // (e.g. "未定义的变量: x" vs "类型错误") pass as consistent.
        // We compare the error category prefix (LEX:/PARSE:/RT:/EX:) which
        // identifies which pipeline stage failed; a stage mismatch indicates
        // one backend accepts code the other rejects — a real divergence.
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
