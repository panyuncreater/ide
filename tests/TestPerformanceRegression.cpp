// ============================================================
// CI 性能基准回归检测
// ------------------------------------------------------------
// 定义一组 MiniLang 源代码基准测试，在三个执行后端
// （Interpreter / StackVM / RegisterVM）上测量执行时间。
//
// 基准分类（R117 引入）：
//   - compute       纯算术与循环（large_loop）
//   - recursion     自递归（fibonacci）+ 互递归（tak）+ 深递归（ackermann）
//   - memory        堆分配与字符串操作（string_concat）
//   - container     数组索引与交换（bubble_sort）
//   - closure       闭包捕获与 upvalue 访问（closure_counter）
//
// 输出格式：JSON（stdout），供 CI 流水线采集与趋势分析。
// 设计原则：仅报告，不失败 —— 回归检测由外部 CI 脚本对比历史数据。
//
// Phase 3 Task 3.5: CI performance benchmark regression detection
// R117: 拓展为基准套件（4 个新基准 + category 分类字段）
// ============================================================

#include <gtest/gtest.h>

#include "compiler/Compiler.h"
#include "compiler/RegisterVM.h"
#include "compiler/VM.h"
#include "interpreter/Interpreter.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"
#ifdef MINILANG_USE_JIT
#include "compiler/JIT.h"
#endif

#include <chrono>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// ============================================================
// 基准测试源码定义
// ============================================================

namespace {

// Fibonacci 递归：测试自递归函数调用开销
// 预期输出：46368（fib(24)）
const std::string kFibonacciSource = R"(
fun fib(n) {
    if (n <= 1) { return n; }
    return fib(n - 1) + fib(n - 2);
}
print(fib(24));
)";

// 大循环累加：测试循环与算术运算吞吐
// 预期输出：4999950000（0+1+...+99999）
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
// 预期输出：5000（字符串长度）
const std::string kStringConcatSource = R"(
var s = "";
var i = 0;
while (i < 5000) {
    s = s + "a";
    i = i + 1;
}
print(len(s));
)";

// R117 新增基准 1：Tak 互递归（Knuth's tak function）
// 测试互递归调用路径（区别于 fib 的自递归）
// 预期输出：12
const std::string kTakSource = R"(
fun tak(x, y, z) {
    if (y < x) {
        return tak(tak(x - 1, y, z), tak(y - 1, z, x), tak(z - 1, x, y));
    }
    return z;
}
print(tak(12, 6, 0));
)";

// R117 新增基准 2：Ackermann 深递归
// 测试调用栈压力与深度递归
// 预期输出：13（ack(2, 8) = 13）
const std::string kAckermannSource = R"(
fun ack(m, n) {
    if (m == 0) { return n + 1; }
    if (n == 0) { return ack(m - 1, 1); }
    return ack(m - 1, ack(m, n - 1));
}
print(ack(2, 8));
)";

// R117 新增基准 3：冒泡排序
// 测试数组索引访问、比较、交换、容器操作
// 50 个元素（5 组 × 10 个）排序
// 预期输出：0（最小元素）
const std::string kBubbleSortSource = R"(
fun bubble_sort(arr) {
    var n = len(arr);
    var i = 0;
    while (i < n) {
        var j = 0;
        while (j < n - i - 1) {
            if (arr[j] > arr[j + 1]) {
                var t = arr[j];
                arr[j] = arr[j + 1];
                arr[j + 1] = t;
            }
            j = j + 1;
        }
        i = i + 1;
    }
    return arr;
}
var arr = [5, 2, 8, 1, 9, 3, 7, 4, 6, 0,
           5, 2, 8, 1, 9, 3, 7, 4, 6, 0,
           5, 2, 8, 1, 9, 3, 7, 4, 6, 0,
           5, 2, 8, 1, 9, 3, 7, 4, 6, 0,
           5, 2, 8, 1, 9, 3, 7, 4, 6, 0];
var sorted = bubble_sort(arr);
print(sorted[0]);
)";

// R117 新增基准 4：闭包计数器
// 测试闭包捕获与 upvalue 访问性能
// 10000 次调用闭包，sum = 1+2+...+10000
// 预期输出：50005000
const std::string kClosureCounterSource = R"(
fun make_counter() {
    var count = 0;
    fun inc() {
        count = count + 1;
        return count;
    }
    return inc;
}
var c = make_counter();
var i = 0;
var sum = 0;
while (i < 10000) {
    sum = sum + c();
    i = i + 1;
}
print(sum);
)";

// ============================================================
// 基准规格表（R117 引入，用于集中管理 name / category / source）
// ============================================================

struct BenchmarkSpec {
    const char* name;
    const char* category;
    const std::string& source;
};

// 注意：保持顺序与下面 TEST() 一致，便于 ZZZ_PrintResults 汇总
const std::vector<BenchmarkSpec> kBenchmarkSpecs = {
    {"fibonacci", "recursion", kFibonacciSource},          {"large_loop", "compute", kLargeLoopSource},
    {"string_concat", "memory", kStringConcatSource},      {"tak", "recursion", kTakSource},
    {"ackermann", "recursion", kAckermannSource},          {"bubble_sort", "container", kBubbleSortSource},
    {"closure_counter", "closure", kClosureCounterSource},
};

// ============================================================
// 后端执行辅助函数
// ============================================================

struct BenchmarkResult {
    std::string backend;
    std::string benchmark;
    std::string category; // R117 新增：compute/recursion/memory/container/closure
    double elapsed_ms;
    std::string output;
};

// 根据 benchmark 名查表得到 category
static std::string lookupCategory(const std::string& name) {
    for (const auto& spec : kBenchmarkSpecs) {
        if (name == spec.name) {
            return spec.category;
        }
    }
    return "uncategorized";
}

// Interpreter 后端执行（返回 print 输出 + 耗时毫秒）
static BenchmarkResult runInterpreterBenchmark(const std::string& source, const std::string& name) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);

    BenchmarkResult result;
    result.backend = "Interpreter";
    result.benchmark = name;
    result.category = lookupCategory(name);

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
static BenchmarkResult runStackVMBenchmark(const std::string& source, const std::string& name) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);

    BenchmarkResult result;
    result.backend = "StackVM";
    result.benchmark = name;
    result.category = lookupCategory(name);

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
static BenchmarkResult runRegisterVMBenchmark(const std::string& source, const std::string& name) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);

    BenchmarkResult result;
    result.backend = "RegisterVM";
    result.benchmark = name;
    result.category = lookupCategory(name);

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

// JIT 后端执行（R150 新增）
// 当 MINILANG_USE_JIT=OFF 时此函数不存在，对应的 TEST 用例也通过 #ifdef 跳过。
#ifdef MINILANG_USE_JIT
static BenchmarkResult runJITBenchmark(const std::string& source, const std::string& name) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);

    BenchmarkResult result;
    result.backend = "JIT";
    result.benchmark = name;
    result.category = lookupCategory(name);

    if (!ast) {
        result.elapsed_ms = -1.0;
        result.output = "<parse-error>";
        return result;
    }

    Compiler compiler;
    CompileResult cr = compiler.compile(*ast);

    JITBackend jit;
    std::string captured;
    jit.setOutputCallback([&](const std::string& s) { captured += s; });

    auto start = std::chrono::high_resolution_clock::now();
    JitResult jres = jit.execute(cr);
    auto end = std::chrono::high_resolution_clock::now();

    // JIT 编译/运行错误也记录输出（与 StackVM/RegisterVM 一致），不使测试失败
    if (jres == JitResult::CompileError) {
        captured += "<jit-compile:" + jit.getLastError() + ">";
    } else if (jres == JitResult::RuntimeError) {
        captured += "<jit-runtime:" + jit.getLastError() + ">";
    }

    result.elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
    result.output = captured;
    return result;
}
#endif

// ============================================================
// JSON 输出辅助
// ============================================================

// 转义 JSON 字符串中的特殊字符
static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += c;
            break;
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
        std::cout << "      \"category\": \"" << jsonEscape(r.category) << "\",\n";
        std::cout << "      \"elapsed_ms\": " << r.elapsed_ms << ",\n";
        std::cout << "      \"output\": \"" << jsonEscape(r.output) << "\"\n";
        std::cout << "    }";
        if (i + 1 < results.size())
            std::cout << ",";
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
// 最终由 ZZZ_PrintResults 汇总输出 JSON。
// 不做 pass/fail 断言 —— 仅测量并记录。
// ============================================================

// 全局结果收集（由 Runner 统一输出）
static std::vector<BenchmarkResult> g_allResults;

// --- Fibonacci 基准（recursion）---
TEST(PerfBenchmark, Fibonacci_Interpreter) {
    g_allResults.push_back(runInterpreterBenchmark(kFibonacciSource, "fibonacci"));
}
TEST(PerfBenchmark, Fibonacci_StackVM) {
    g_allResults.push_back(runStackVMBenchmark(kFibonacciSource, "fibonacci"));
}
TEST(PerfBenchmark, Fibonacci_RegisterVM) {
    g_allResults.push_back(runRegisterVMBenchmark(kFibonacciSource, "fibonacci"));
}

// --- 大循环基准（compute）---
TEST(PerfBenchmark, LargeLoop_Interpreter) {
    g_allResults.push_back(runInterpreterBenchmark(kLargeLoopSource, "large_loop"));
}
TEST(PerfBenchmark, LargeLoop_StackVM) {
    g_allResults.push_back(runStackVMBenchmark(kLargeLoopSource, "large_loop"));
}
TEST(PerfBenchmark, LargeLoop_RegisterVM) {
    g_allResults.push_back(runRegisterVMBenchmark(kLargeLoopSource, "large_loop"));
}

// --- 字符串拼接基准（memory）---
TEST(PerfBenchmark, StringConcat_Interpreter) {
    g_allResults.push_back(runInterpreterBenchmark(kStringConcatSource, "string_concat"));
}
TEST(PerfBenchmark, StringConcat_StackVM) {
    g_allResults.push_back(runStackVMBenchmark(kStringConcatSource, "string_concat"));
}
TEST(PerfBenchmark, StringConcat_RegisterVM) {
    g_allResults.push_back(runRegisterVMBenchmark(kStringConcatSource, "string_concat"));
}

// --- R117 新增：Tak 互递归基准（recursion）---
TEST(PerfBenchmark, Tak_Interpreter) {
    g_allResults.push_back(runInterpreterBenchmark(kTakSource, "tak"));
}
TEST(PerfBenchmark, Tak_StackVM) {
    g_allResults.push_back(runStackVMBenchmark(kTakSource, "tak"));
}
TEST(PerfBenchmark, Tak_RegisterVM) {
    g_allResults.push_back(runRegisterVMBenchmark(kTakSource, "tak"));
}

// --- R117 新增：Ackermann 深递归基准（recursion）---
TEST(PerfBenchmark, Ackermann_Interpreter) {
    g_allResults.push_back(runInterpreterBenchmark(kAckermannSource, "ackermann"));
}
TEST(PerfBenchmark, Ackermann_StackVM) {
    g_allResults.push_back(runStackVMBenchmark(kAckermannSource, "ackermann"));
}
TEST(PerfBenchmark, Ackermann_RegisterVM) {
    g_allResults.push_back(runRegisterVMBenchmark(kAckermannSource, "ackermann"));
}

// --- R117 新增：冒泡排序基准（container）---
TEST(PerfBenchmark, BubbleSort_Interpreter) {
    g_allResults.push_back(runInterpreterBenchmark(kBubbleSortSource, "bubble_sort"));
}
TEST(PerfBenchmark, BubbleSort_StackVM) {
    g_allResults.push_back(runStackVMBenchmark(kBubbleSortSource, "bubble_sort"));
}
TEST(PerfBenchmark, BubbleSort_RegisterVM) {
    g_allResults.push_back(runRegisterVMBenchmark(kBubbleSortSource, "bubble_sort"));
}

// --- R117 新增：闭包计数器基准（closure）---
TEST(PerfBenchmark, ClosureCounter_Interpreter) {
    g_allResults.push_back(runInterpreterBenchmark(kClosureCounterSource, "closure_counter"));
}
TEST(PerfBenchmark, ClosureCounter_StackVM) {
    g_allResults.push_back(runStackVMBenchmark(kClosureCounterSource, "closure_counter"));
}
TEST(PerfBenchmark, ClosureCounter_RegisterVM) {
    g_allResults.push_back(runRegisterVMBenchmark(kClosureCounterSource, "closure_counter"));
}

// --- R150 新增：JIT 后端基准（条件编译，MINILANG_USE_JIT=ON 时启用）---
// 设计原则与三后端一致：仅报告，不判定回归。
// 注意：closure_counter 触发 JIT 不支持的 upvalue 捕获，会记录 jit-compile 错误
// 输出（<jit-compile:...>），这是预期行为，用于跟踪 JIT 后端覆盖范围。
#ifdef MINILANG_USE_JIT
TEST(PerfBenchmark, Fibonacci_JIT) {
    g_allResults.push_back(runJITBenchmark(kFibonacciSource, "fibonacci"));
}
TEST(PerfBenchmark, LargeLoop_JIT) {
    g_allResults.push_back(runJITBenchmark(kLargeLoopSource, "large_loop"));
}
TEST(PerfBenchmark, StringConcat_JIT) {
    g_allResults.push_back(runJITBenchmark(kStringConcatSource, "string_concat"));
}
TEST(PerfBenchmark, Tak_JIT) {
    g_allResults.push_back(runJITBenchmark(kTakSource, "tak"));
}
TEST(PerfBenchmark, Ackermann_JIT) {
    g_allResults.push_back(runJITBenchmark(kAckermannSource, "ackermann"));
}
TEST(PerfBenchmark, BubbleSort_JIT) {
    g_allResults.push_back(runJITBenchmark(kBubbleSortSource, "bubble_sort"));
}
TEST(PerfBenchmark, ClosureCounter_JIT) {
    g_allResults.push_back(runJITBenchmark(kClosureCounterSource, "closure_counter"));
}
#endif

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
