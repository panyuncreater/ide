// ============================================================
// CI 性能基准回归检测
// ------------------------------------------------------------
// 定义一组 MiniLang 源代码基准测试（fibonacci、大循环、字符串拼接），
// 在三个执行后端（Interpreter / StackVM / RegisterVM）上测量执行时间。
//
// 输出格式：JSON（stdout），供 CI 流水线采集与趋势分析。
// 设计原则：仅报告，不失败 —— 回归检测由外部 CI 脚本对比历史数据。
//
// Phase 3 Task 3.5: CI performance benchmark regression detection
// ============================================================

#include <gtest/gtest.h>

#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "interpreter/Interpreter.h"
#include "compiler/Compiler.h"
#include "compiler/VM.h"
#include "compiler/RegisterVM.h"

#include <chrono>
#include <iostream>
#include <string>
#include <vector>
#include <sstream>

// ============================================================
// 基准测试源码定义
// ============================================================

namespace {

// Fibonacci 递归：测试函数调用与递归开销
const std::string kFibonacciSource = R"(
fun fib(n) {
    if (n <= 1) { return n; }
    return fib(n - 1) + fib(n - 2);
}
print(fib(24));
)";

// 大循环累加：测试循环与算术运算吞吐
const std::string kLargeLoopSource = R"(
var sum = 0;
var i = 0;
while (i < 100000) {
    sum = sum + i;
    i = i + 1;
}
print(sum);
)";

// 字符串拼接：测试堆分配与字符串操作性能
const std::string kStringConcatSource = R"(
var s = "";
var i = 0;
while (i < 5000) {
    s = s + "a";
    i = i + 1;
}
print(len(s));
)";

// ============================================================
// 后端执行辅助函数
// ============================================================

struct BenchmarkResult {
    std::string backend;
    std::string benchmark;
    double elapsed_ms;
    std::string output;
};

// Interpreter 后端执行（返回 print 输出 + 耗时毫秒）
static BenchmarkResult runInterpreterBenchmark(const std::string& source,
                                                const std::string& name) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);

    BenchmarkResult result;
    result.backend = "Interpreter";
    result.benchmark = name;

    if (!ast) {
        result.elapsed_ms = -1.0;
        result.output = "<parse-error>";
        return result;
    }

    Interpreter interp;
    std::string captured;
    interp.setOutputCallback([&](const std::string& s) { captured += s; });

    auto start = std::chrono::high_resolution_clock::now();
    try {
        interp.execute(*ast);
    } catch (...) {
        captured += "<runtime-error>";
    }
    auto end = std::chrono::high_resolution_clock::now();

    result.elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
    result.output = captured;
    return result;
}

// StackVM 后端执行
static BenchmarkResult runStackVMBenchmark(const std::string& source,
                                            const std::string& name) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);

    BenchmarkResult result;
    result.backend = "StackVM";
    result.benchmark = name;

    if (!ast) {
        result.elapsed_ms = -1.0;
        result.output = "<parse-error>";
        return result;
    }

    Compiler compiler;
    CompileResult cr = compiler.compile(*ast);

    VM vm;
    std::string captured;
    vm.setOutputCallback([&](const std::string& s) { captured += s; });

    auto start = std::chrono::high_resolution_clock::now();
    vm.execute(cr);
    auto end = std::chrono::high_resolution_clock::now();

    if (vm.hasError()) {
        captured += "<runtime:" + vm.getLastError() + ">";
    }

    result.elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
    result.output = captured;
    return result;
}

// RegisterVM 后端执行
static BenchmarkResult runRegisterVMBenchmark(const std::string& source,
                                               const std::string& name) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);

    BenchmarkResult result;
    result.backend = "RegisterVM";
    result.benchmark = name;

    if (!ast) {
        result.elapsed_ms = -1.0;
        result.output = "<parse-error>";
        return result;
    }

    Compiler compiler;
    compiler.setUseRegisterVM(true);
    compiler.compile(*ast);

    RegisterVM vm;
    std::string captured;
    vm.setOutputCallback([&](const std::string& s) { captured += s; });

    auto start = std::chrono::high_resolution_clock::now();
    vm.execute(compiler.getLastRegisterResult());
    auto end = std::chrono::high_resolution_clock::now();

    if (vm.hasError()) {
        captured += "<runtime:" + vm.getLastError() + ">";
    }

    result.elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
    result.output = captured;
    return result;
}

// ============================================================
// JSON 输出辅助
// ============================================================

// 转义 JSON 字符串中的特殊字符
static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

static void emitJsonResults(const std::vector<BenchmarkResult>& results) {
    std::cout << "{\n";
    std::cout << "  \"benchmark_results\": [\n";
    for (size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        std::cout << "    {\n";
        std::cout << "      \"backend\": \"" << jsonEscape(r.backend) << "\",\n";
        std::cout << "      \"benchmark\": \"" << jsonEscape(r.benchmark) << "\",\n";
        std::cout << "      \"elapsed_ms\": " << r.elapsed_ms << ",\n";
        std::cout << "      \"output\": \"" << jsonEscape(r.output) << "\"\n";
        std::cout << "    }";
        if (i + 1 < results.size()) std::cout << ",";
        std::cout << "\n";
    }
    std::cout << "  ]\n";
    std::cout << "}\n";
}

} // anonymous namespace

// ============================================================
// GoogleTest 基准测试用例
// ------------------------------------------------------------
// 每个 TEST() 在三个后端上执行对应基准，收集结果。
// 最终由 PerfRegressionRunner 汇总输出 JSON。
// 不做 pass/fail 断言 —— 仅测量并记录。
// ============================================================

// 全局结果收集（由 Runner 统一输出）
static std::vector<BenchmarkResult> g_allResults;

// --- Fibonacci 基准 ---
TEST(PerfBenchmark, Fibonacci_Interpreter) {
    auto r = runInterpreterBenchmark(kFibonacciSource, "fibonacci");
    g_allResults.push_back(r);
}

TEST(PerfBenchmark, Fibonacci_StackVM) {
    auto r = runStackVMBenchmark(kFibonacciSource, "fibonacci");
    g_allResults.push_back(r);
}

TEST(PerfBenchmark, Fibonacci_RegisterVM) {
    auto r = runRegisterVMBenchmark(kFibonacciSource, "fibonacci");
    g_allResults.push_back(r);
}

// --- 大循环基准 ---
TEST(PerfBenchmark, LargeLoop_Interpreter) {
    auto r = runInterpreterBenchmark(kLargeLoopSource, "large_loop");
    g_allResults.push_back(r);
}

TEST(PerfBenchmark, LargeLoop_StackVM) {
    auto r = runStackVMBenchmark(kLargeLoopSource, "large_loop");
    g_allResults.push_back(r);
}

TEST(PerfBenchmark, LargeLoop_RegisterVM) {
    auto r = runRegisterVMBenchmark(kLargeLoopSource, "large_loop");
    g_allResults.push_back(r);
}

// --- 字符串拼接基准 ---
TEST(PerfBenchmark, StringConcat_Interpreter) {
    auto r = runInterpreterBenchmark(kStringConcatSource, "string_concat");
    g_allResults.push_back(r);
}

TEST(PerfBenchmark, StringConcat_StackVM) {
    auto r = runStackVMBenchmark(kStringConcatSource, "string_concat");
    g_allResults.push_back(r);
}

TEST(PerfBenchmark, StringConcat_RegisterVM) {
    auto r = runRegisterVMBenchmark(kStringConcatSource, "string_concat");
    g_allResults.push_back(r);
}

// ============================================================
// 汇总输出（注册为最后一个测试，确保 JSON 在所有基准完成后输出）
// ============================================================
TEST(PerfBenchmark, ZZZ_PrintResults) {
    // 命名为 ZZZ_ 以确保在字母序排列中最后执行
    emitJsonResults(g_allResults);
    // 清空全局结果，避免跨运行累积
    g_allResults.clear();
    // 始终通过 —— 仅报告，不判定回归
    SUCCEED();
}

// ============================================================
// 独立 main() —— 作为 minilang_perf_test 可执行目标入口
// ============================================================
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
