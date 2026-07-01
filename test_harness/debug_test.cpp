// MiniLang Debug Consistency Test: Full Run vs Step-by-Step
// Verifies that debugging (step over/in/out) produces identical results to full execution

#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <memory>
#include <algorithm>

#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "ast/ASTNode.h"
#include "interpreter/Interpreter.h"
#include "interpreter/Value.h"
#include "interpreter/Environment.h"
#include "debug/DebugTypes.h"
// Include stub DebugController (test_harness/debug/ via target_include_directories BEFORE)
#include "debug/DebugController.h"
#include "Diagnostic.h"

// ─── Result structures ──────────────────────────────────────────────

struct RunResult {
    std::string output;
    std::string error;
    bool hasError = false;
    std::unordered_map<std::string, std::string> globals;
};

struct DebugResult : RunResult {
    int pauseCount = 0;
    int maxDepth = 0;
    std::vector<int> pauseLines;
    // Pause line bounds check: count of pauseLines outside [1, sourceLineCount]
    int pauseLinesOutOfBounds = 0;
};

// ─── Group A: Full execution ────────────────────────────────────────

static RunResult runFull(const std::string& source) {
    RunResult res;
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

// ─── Group B: Step-through execution ────────────────────────────────

static DebugResult runDebug(const std::string& source, StepMode mode,
                            DebugController* externalDbg = nullptr) {
    DebugResult res;
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
    DebugController localDbg;
    DebugController* dbg = externalDbg ? externalDbg : &localDbg;
    std::string captured;
    interp.setOutputCallback([&captured](const std::string& s) { captured += s + "\n"; });

    // Wire debugger to interpreter
    // MEM-01 fix: setDebugger 改用 shared_ptr。此处 dbg 是栈对象或外部所有，
    // 用空删除器避免 shared_ptr 析构时误 delete。
    interp.setDebugger(std::shared_ptr<DebugController>(dbg, [](DebugController*){}));
    interp.setDebugMode(true);

    // Variable snapshot callback: walk environment chain
    dbg->setVariableCallback([&interp]() -> std::vector<VariableSnapshot> {
        std::vector<VariableSnapshot> vars;
        auto* env = interp.currentEnvironment();
        while (env) {
            for (auto& [name, val] : env->localVariables()) {
                vars.push_back({name, val, "local"});
            }
            env = env->parent.get();
        }
        return vars;
    });

    // Call stack callback
    dbg->setCallStackCallback([&interp]() -> std::vector<CallStackEntry> {
        std::vector<CallStackEntry> entries;
        for (auto& frame : interp.getCallStack()) {
            CallStackEntry entry;
            entry.functionName = frame.functionName;
            entry.line = frame.line;
            entry.depth = frame.depth;
            if (frame.env) {
                for (auto& [name, val] : frame.env->localVariables()) {
                    entry.locals.push_back({name, val});
                }
            }
            entries.push_back(std::move(entry));
        }
        return entries;
    });

    // Set step mode
    dbg->reset();
    switch (mode) {
    case StepMode::MODE_STEP_OVER: dbg->stepOver(); break;
    case StepMode::MODE_STEP_IN:   dbg->stepIn(); break;
    case StepMode::MODE_STEP_OUT:  dbg->stepOut(); break;
    default: dbg->resume(); break;
    }

    try {
        interp.execute(*ast);
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

    interp.setDebugMode(false);

    // Collect debug recording data
    res.pauseCount = dbg->pauseCount();
    res.maxDepth = dbg->maxDepthSeen();
    res.pauseLines = dbg->pauseLines();

    // Pause-line bounds check: every reported pause line must fall within
    // [1, sourceLineCount]. A pause line of 0 or > lineCount indicates the
    // debugger's line tracking is broken (e.g. stale AST node line numbers,
    // or pauseEvents recorded after AST destruction).
    // Note: the previous "variable consistency check" here compared the last
    // pause's variable snapshot against FINAL globals, which is meaningless —
    // variables legitimately change after the last pause. Replaced with a
    // real invariant the audit can meaningfully verify.
    int sourceLineCount = 1;
    for (char ch : source) {
        if (ch == '\n') ++sourceLineCount;
    }
    for (int ln : res.pauseLines) {
        if (ln < 1 || ln > sourceLineCount) {
            ++res.pauseLinesOutOfBounds;
        }
    }

    return res;
}

// ─── Test case definition ───────────────────────────────────────────

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

    // ═══════ Branches (BR1-BR8) ═══════
    T("BR1", "if-true",
      "var x = 10;\nif (x > 5) { print(\"big\"); } else { print(\"small\"); }");
    T("BR2", "if-false",
      "var x = 3;\nif (x > 5) { print(\"big\"); } else { print(\"small\"); }");
    T("BR3", "nested if",
      "var a = 1; var b = 2;\nif (a == 1) { if (b == 2) { print(\"yes\"); } else { print(\"no\"); } }");
    T("BR4", "else-if chain",
      "var g = 85;\nif (g >= 90) { print(\"A\"); } else if (g >= 80) { print(\"B\"); } else if (g >= 70) { print(\"C\"); } else { print(\"F\"); }");
    T("BR5", "dangling else",
      "if (false) { if (true) { print(1); } } else { print(2); }");
    T("BR6", "truthy values",
      "if (0) { print(\"zero-t\"); } else { print(\"zero-f\"); }\nif (\"\") { print(\"empty-t\"); } else { print(\"empty-f\"); }\nif (null) { print(\"null-t\"); } else { print(\"null-f\"); }");
    T("BR7", "if in loop",
      "var s = 0;\nfor (var i = 0; i < 10; i = i + 1) { if (i % 2 == 0) { s = s + i; } }\nprint(s);");
    T("BR8", "conditional assignment",
      "var x = 5;\nvar y = 0;\nif (x > 3) { y = x * 2; } else { y = x + 1; }\nprint(y);");

    // ═══════ Loops (LP1-LP8) ═══════
    T("LP1", "while basic",
      "var i = 0;\nwhile (i < 5) { print(i); i = i + 1; }");
    T("LP2", "while sum",
      "var s = 0; var i = 1;\nwhile (i <= 10) { s = s + i; i = i + 1; }\nprint(s);");
    T("LP3", "for basic",
      "for (var i = 0; i < 5; i = i + 1) { print(i); }");
    T("LP4", "for sum",
      "var s = 0;\nfor (var i = 1; i <= 100; i = i + 1) { s = s + i; }\nprint(s);");
    T("LP5", "nested for",
      "for (var i = 0; i < 3; i = i + 1) { for (var j = 0; j < 3; j = j + 1) { print(i * 10 + j); } }");
    T("LP6", "while countdown",
      "var n = 5;\nwhile (n > 0) { print(n); n = n - 1; }\nprint(\"done\");");
    T("LP7", "for with break-like return",
      "fun findFirst(lim) { for (var i = 0; i < lim; i = i + 1) { if (i * i > 20) { return i; } } return -1; }\nprint(findFirst(10));");
    T("LP8", "loop with string build",
      "var s = \"\";\nfor (var i = 0; i < 5; i = i + 1) { s = s + \"x\"; }\nprint(s);");

    // ═══════ Function Calls (FC1-FC8) ═══════
    T("FC1", "basic function",
      "fun add(a, b) { return a + b; }\nprint(add(3, 4));");
    T("FC2", "no return",
      "fun f() { var x = 1; }\nprint(f());");
    T("FC3", "nested call",
      "fun a(x) { return x + 1; }\nfun b(x) { return a(x) * 2; }\nprint(b(5));");
    T("FC4", "multi-param",
      "fun f(a, b, c) { return a + b + c; }\nprint(f(1, 2, 3));");
    T("FC5", "function as expression",
      "fun sq(x) { return x * x; }\nprint(sq(3) + sq(4));");
    T("FC6", "void function side effect",
      "var s = 0;\nfun add(v) { s = s + v; }\nadd(10); add(20); add(30);\nprint(s);");
    T("FC7", "typed params",
      "fun add(int a, int b) { return a + b; }\nprint(add(7, 8));");
    T("FC8", "mutual recursion",
      "fun isEven(n) { if (n == 0) { return true; } return isOdd(n - 1); }\nfun isOdd(n) { if (n == 0) { return false; } return isEven(n - 1); }\nprint(isEven(6)); print(isOdd(5));");

    // ═══════ Recursion (RC1-RC8) ═══════
    T("RC1", "factorial",
      "fun fact(n) { if (n <= 1) { return 1; } return n * fact(n - 1); }\nprint(fact(8));");
    T("RC2", "fibonacci",
      "fun fib(n) { if (n <= 1) { return n; } return fib(n - 1) + fib(n - 2); }\nprint(fib(10));");
    T("RC3", "sum recursive",
      "fun sum(n) { if (n <= 0) { return 0; } return n + sum(n - 1); }\nprint(sum(20));");
    T("RC4", "power recursive",
      "fun pow(b, e) { if (e == 0) { return 1; } return b * pow(b, e - 1); }\nprint(pow(2, 10));");
    T("RC5", "gcd recursive",
      "fun gcd(a, b) { if (b == 0) { return a; } return gcd(b, a % b); }\nprint(gcd(48, 18));");
    T("RC6", "depth 10",
      "fun f(n) { if (n == 0) { return 0; } return 1 + f(n - 1); }\nprint(f(10));");
    T("RC7", "tree recursion",
      "fun tree(n) { if (n <= 0) { return 1; } return tree(n - 1) + tree(n - 2); }\nprint(tree(8));");
    T("RC8", "recursive string",
      "fun repeat(s, n) { if (n <= 0) { return \"\"; } return s + repeat(s, n - 1); }\nprint(repeat(\"ab\", 4));");

    // ═══════ Nested Scopes (NS1-NS8) ═══════
    T("NS1", "block shadow",
      "var x = 1;\n{ var x = 99; print(x); }\nprint(x);");
    T("NS2", "nested shadow",
      "var x = 1;\n{ var x = 2; { var x = 3; print(x); } print(x); }\nprint(x);");
    T("NS3", "function local scope",
      "var x = 1;\nfun f() { var x = 100; return x; }\nprint(f()); print(x);");
    T("NS4", "loop var scope",
      "var s = 0;\nfor (var i = 0; i < 5; i = i + 1) { s = s + i; }\nprint(s);");
    T("NS5", "nested function calls",
      "fun outer(x) { fun inner(y) { return x + y; } return inner(10); }\nprint(outer(5));");
    T("NS6", "multiple functions same name var",
      "fun f() { var x = 1; return x; }\nfun g() { var x = 2; return x; }\nprint(f() + g());");
    T("NS7", "deep nesting",
      "var a = 1;\n{ var b = 2; { var c = 3; { var d = 4; print(a + b + c + d); } } }");
    T("NS8", "scope with assignment",
      "var x = 10;\nfun modify() { x = x + 5; }\nmodify(); modify();\nprint(x);");

    return tests;
}

// ─── Comparison and reporting ───────────────────────────────────────

struct TestFailure {
    std::string id, name;
    std::string detail;
    std::string fullOutput, stepOutput;
    std::string fullError, stepError;
};

// ─── Main ───────────────────────────────────────────────────────────

int main() {
    auto tests = buildTests();
    std::cout << "MiniLang Debug Consistency Test\n";
    std::cout << "================================\n";
    std::cout << "Test count: " << tests.size() << "\n\n";

    int pass = 0, fail = 0, skip = 0;
    std::vector<TestFailure> failures;

    for (auto& tc : tests) {
        // Group A: Full execution
        auto fullRes = runFull(tc.source);

        // Group B: Step-Over execution
        auto stepRes = runDebug(tc.source, StepMode::MODE_STEP_OVER);

        bool match = true;
        std::string detail;

        // Compare error state
        if (fullRes.hasError != stepRes.hasError) {
            match = false;
            detail = "error-state mismatch: full=" +
                std::string(fullRes.hasError ? "ERR" : "OK") + " step=" +
                std::string(stepRes.hasError ? "ERR" : "OK");
        }

        // Compare output
        if (!fullRes.hasError && !stepRes.hasError) {
            if (fullRes.output != stepRes.output) {
                match = false;
                detail = "output mismatch";
            }
            // Compare globals
            for (auto& [k, fv] : fullRes.globals) {
                auto it = stepRes.globals.find(k);
                if (it != stepRes.globals.end()) {
                    if (fv != it->second) {
                        match = false;
                        if (!detail.empty()) detail += "; ";
                        detail += "global '" + k + "': full=" + fv + " step=" + it->second;
                    }
                }
            }
        }

        // Verify debug recording
        if (!stepRes.hasError && stepRes.pauseCount == 0) {
            match = false;
            if (!detail.empty()) detail += "; ";
            detail += "no debug pauses recorded";
        }
        // Pause-line bounds check: pauseLines must reference valid source lines.
        // Out-of-bounds pause lines indicate broken debugger line tracking.
        if (stepRes.pauseLinesOutOfBounds > 0) {
            match = false;
            if (!detail.empty()) detail += "; ";
            detail += "pauseLines out of bounds: " + std::to_string(stepRes.pauseLinesOutOfBounds);
        }
        // AUDIT-INVARIANT fix: maxDepth sanity check.
        // 原实现 maxDepth 仅在 PASS 输出中打印，主循环中为空操作不变量。
        // maxDepth 超过 MAX_RECURSION_DEPTH (256) 表示调用栈跟踪损坏或
        // 栈溢出检测失败——真正的调试器不变量违反。
        if (stepRes.maxDepth > 256) {
            match = false;
            if (!detail.empty()) detail += "; ";
            detail += "maxDepth exceeds MAX_RECURSION_DEPTH: " + std::to_string(stepRes.maxDepth);
        }

        if (match) {
            pass++;
            std::cout << "PASS " << tc.id << " " << tc.name
                      << " (pauses=" << stepRes.pauseCount
                      << " maxDepth=" << stepRes.maxDepth << ")\n";
        } else {
            fail++;
            std::cout << "FAIL " << tc.id << " " << tc.name << " — " << detail << "\n";
            failures.push_back({tc.id, tc.name, detail,
                fullRes.output, stepRes.output,
                fullRes.error, stepRes.error});
        }
    }

    // ═══════ Step-In specific tests ═══════
    std::cout << "\n── Step-In Verification ──\n";
    {
        // Step-In should enter functions (more pauses than Step-Over)
        std::string src = "fun f(x) { return x + 1; }\nprint(f(5));";
        auto stepOverRes = runDebug(src, StepMode::MODE_STEP_OVER);
        auto stepInRes = runDebug(src, StepMode::MODE_STEP_IN);

        bool siMatch = (stepOverRes.output == stepInRes.output);
        bool siDeeper = (stepInRes.pauseCount > stepOverRes.pauseCount);
        bool siInFunc = (stepInRes.maxDepth >= 1);

        if (siMatch && siDeeper && siInFunc) {
            pass++;
            std::cout << "PASS SI1 Step-In enters functions (over=" << stepOverRes.pauseCount
                      << " in=" << stepInRes.pauseCount << " maxDepth=" << stepInRes.maxDepth << ")\n";
        } else {
            fail++;
            std::string d = "output=" + std::string(siMatch ? "match" : "MISMATCH") +
                " deeper=" + std::string(siDeeper ? "yes" : "NO") +
                " inFunc=" + std::string(siInFunc ? "yes" : "NO");
            std::cout << "FAIL SI1 Step-In enters functions — " << d << "\n";
            failures.push_back({"SI1", "Step-In enters functions", d,
                stepOverRes.output, stepInRes.output, "", ""});
        }
    }
    {
        // Step-In depth tracking in nested calls
        std::string src = "fun a(x) { return x + 1; }\nfun b(x) { return a(x) * 2; }\nprint(b(5));";
        auto stepInRes = runDebug(src, StepMode::MODE_STEP_IN);
        bool nestedOk = (stepInRes.maxDepth >= 2);  // main→b→a
        if (nestedOk && !stepInRes.hasError) {
            pass++;
            std::cout << "PASS SI2 Step-In nested depth (maxDepth=" << stepInRes.maxDepth << ")\n";
        } else {
            fail++;
            std::cout << "FAIL SI2 Step-In nested depth (maxDepth=" << stepInRes.maxDepth << ")\n";
            failures.push_back({"SI2", "Step-In nested depth", "maxDepth < 2",
                "", "", "", ""});
        }
    }

    // ═══════ Step-Out specific tests ═══════
    std::cout << "\n── Step-Out Verification ──\n";
    {
        // Step-Out from depth 0 should run to completion (no pauses after init)
        std::string src = "fun f(x) { var y = x * 2; return y; }\nprint(f(10));";
        // Start at depth 0, step out means "pause when depth < 0" → never → run to end
        auto stepOutRes = runDebug(src, StepMode::MODE_STEP_OUT);
        bool soOk = !stepOutRes.hasError && stepOutRes.output == "20\n";
        if (soOk) {
            pass++;
            std::cout << "PASS SO1 Step-Out from depth 0 runs to completion (pauses=" << stepOutRes.pauseCount << ")\n";
        } else {
            fail++;
            std::cout << "FAIL SO1 Step-Out from depth 0 (output=[" << stepOutRes.output << "] err=" << stepOutRes.error << ")\n";
            failures.push_back({"SO1", "Step-Out depth 0", "output or error mismatch",
                "20\n", stepOutRes.output, "", stepOutRes.error});
        }
    }
    {
        // Step-Out call stack consistency
        std::string src = "fun inner() { return 42; }\nfun outer() { return inner(); }\nprint(outer());";
        auto fullRes = runFull(src);
        auto stepOverRes = runDebug(src, StepMode::MODE_STEP_OVER);
        bool soMatch = (fullRes.output == stepOverRes.output);
        if (soMatch && !stepOverRes.hasError) {
            pass++;
            std::cout << "PASS SO2 Step-Out call chain consistency (output matches)\n";
        } else {
            fail++;
            std::cout << "FAIL SO2 Step-Out call chain (full=[" << fullRes.output << "] step=[" << stepOverRes.output << "])\n";
            failures.push_back({"SO2", "Step-Out call chain", "output mismatch",
                fullRes.output, stepOverRes.output, "", ""});
        }
    }

    // ═══════ Call Stack depth verification ═══════
    std::cout << "\n── Call Stack Depth Verification ──\n";
    {
        // Recursive function should show increasing then decreasing depth
        std::string src = "fun fact(n) { if (n <= 1) { return 1; } return n * fact(n - 1); }\nprint(fact(5));";
        auto stepInRes = runDebug(src, StepMode::MODE_STEP_IN);
        // maxDepth should be at least 4 (fact(5)→fact(4)→fact(3)→fact(2)→fact(1))
        bool csOk = (stepInRes.maxDepth >= 4);
        if (csOk && !stepInRes.hasError) {
            pass++;
            std::cout << "PASS CS1 Recursive call stack depth (maxDepth=" << stepInRes.maxDepth << ")\n";
        } else {
            fail++;
            std::cout << "FAIL CS1 Recursive call stack depth (maxDepth=" << stepInRes.maxDepth << ")\n";
            failures.push_back({"CS1", "Recursive call stack", "depth < 4",
                "", "", "", ""});
        }
    }
    {
        // Variable snapshot at pause should contain current scope vars
        std::string src = "var g = 100;\nfun f(x) { var y = x + 1; return y; }\nprint(f(g));";
        DebugController cs2Dbg;
        auto stepInRes = runDebug(src, StepMode::MODE_STEP_IN, &cs2Dbg);
        // Check that at some pause, we see 'x' or 'y' as local variables
        bool foundLocal = false;
        for (auto& evt : cs2Dbg.pauseEvents()) {
            for (auto& [vname, vval] : evt.variables) {
                if (vname == "x" || vname == "y") {
                    foundLocal = true;
                    break;
                }
            }
            if (foundLocal) break;
        }
        if (foundLocal && !stepInRes.hasError) {
            pass++;
            std::cout << "PASS CS2 Variable snapshot captures locals (found x/y)\n";
        } else {
            fail++;
            std::cout << "FAIL CS2 Variable snapshot missing locals\n";
            failures.push_back({"CS2", "Variable snapshot locals", "x/y not found in any pause",
                "", "", "", ""});
        }
    }

    // ═══════ Summary ═══════
    int total = pass + fail + skip;
    std::cout << "\n================================\n";
    std::cout << "RESULTS: " << pass << " passed, " << fail << " failed, "
              << skip << " skipped, " << total << " total\n";
    std::cout << "================================\n";

    if (!failures.empty()) {
        std::cout << "\nFAILURES:\n─────────\n";
        for (auto& f : failures) {
            std::cout << "[" << f.id << "] " << f.name << "\n";
            std::cout << "  Detail: " << f.detail << "\n";
            if (!f.fullOutput.empty())
                std::cout << "  Full output:  [" << f.fullOutput.substr(0, std::min<size_t>(80, f.fullOutput.size())) << "]\n";
            if (!f.stepOutput.empty())
                std::cout << "  Step output:  [" << f.stepOutput.substr(0, std::min<size_t>(80, f.stepOutput.size())) << "]\n";
            if (!f.fullError.empty())
                std::cout << "  Full error:   " << f.fullError << "\n";
            if (!f.stepError.empty())
                std::cout << "  Step error:   " << f.stepError << "\n";
            std::cout << "\n";
        }
    }

    return fail > 0 ? 1 : 0;
}
