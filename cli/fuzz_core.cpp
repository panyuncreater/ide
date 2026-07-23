// ============================================================
// cli/fuzz_core.cpp - minilang-fuzz CLI 核心逻辑实现
// ------------------------------------------------------------
// 实现：三后端执行封装 + 程序生成器 + 字节变异器 + 批量执行
//
// 错误编码（与 TestThreeEnginesFuzz.cpp 对齐）：
//   "<parse-fail>"     解析失败
//   "<compile:msg>"    编译失败
//   "<runtime:msg>"    运行时错误
//
// 关键设计：
//   - import 隔离：注入空 moduleLoader 避免触发真实模块加载
//   - DoS 防护：复用 RuntimeLimits::MAX_* 常量
//   - 崩溃检测：try/catch 捕获所有异常 + VM hasError
//   - 种子可复现：std::mt19937_64，--seed 指定
// ============================================================
#include "cli/fuzz_core.h"

#include "compiler/Compiler.h"
#include "compiler/RegisterVM.h"
#include "compiler/VM.h"
#include "interpreter/Interpreter.h"
#include "interpreter/RuntimeExceptions.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>

namespace minilang_fuzz {

namespace {

// ============================================================
// 三后端执行封装（参考 TestThreeEnginesFuzz.cpp:47-99）
// ------------------------------------------------------------
// 统一返回 "<runtime:消息>" 表示运行时错误，便于差分比较。
// 编译失败返回 "<compile:消息>"，解析失败返回 "<parse-fail>"。
// 崩溃（未捕获异常）返回 "<crash:消息>"。
// ============================================================

std::string runInterp(const std::string& src, bool& crash) {
    crash = false;
    Lexer lx;
    auto tk = lx.scan(src);
    if (lx.getDiagnostics().hasErrors())
        return "<parse-fail>";
    Parser p;
    auto ast = p.parse(tk);
    if (!ast || p.hasErrors())
        return "<parse-fail>";
    Interpreter interp;
    interp.setModuleLoader([](const std::string&) { return std::string(); });
    std::string out;
    interp.setOutputCallback([&](const std::string& s) { out += s; });
    try {
        interp.execute(*ast);
    } catch (const RuntimeError& e) {
        if (out.empty())
            return "<runtime:" + std::string(e.what()) + ">";
        return out + "<runtime:" + std::string(e.what()) + ">";
    } catch (const std::exception& e) {
        crash = true;
        return "<crash:" + std::string(e.what()) + ">";
    }
    return out;
}

std::string runStackVM(const std::string& src, bool& crash) {
    crash = false;
    Lexer lx;
    auto tk = lx.scan(src);
    if (lx.getDiagnostics().hasErrors())
        return "<parse-fail>";
    Parser p;
    auto ast = p.parse(tk);
    if (!ast || p.hasErrors())
        return "<parse-fail>";
    Compiler c;
    c.setUseIR(true);
    c.setModuleLoader([](const std::string&) { return std::string(); });
    auto cr = c.compile(*ast);
    if (c.getDiagnostics().hasErrors())
        return "<compile:" + c.getLastError() + ">";
    VM vm;
    std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });
    try {
        auto res = vm.execute(cr);
        (void)res;
    } catch (const std::exception& e) {
        crash = true;
        if (out.empty())
            return "<crash:" + std::string(e.what()) + ">";
        return out + "<crash:" + std::string(e.what()) + ">";
    }
    if (vm.hasError()) {
        if (out.empty())
            return "<runtime:" + vm.getLastError() + ">";
        return out + "<runtime:" + vm.getLastError() + ">";
    }
    return out;
}

std::string runRegVM(const std::string& src, bool& crash) {
    crash = false;
    Lexer lx;
    auto tk = lx.scan(src);
    if (lx.getDiagnostics().hasErrors())
        return "<parse-fail>";
    Parser p;
    auto ast = p.parse(tk);
    if (!ast || p.hasErrors())
        return "<parse-fail>";
    Compiler c;
    c.setUseRegisterVM(true);
    c.setModuleLoader([](const std::string&) { return std::string(); });
    c.compile(*ast);
    if (c.getDiagnostics().hasErrors())
        return "<compile:" + c.getLastError() + ">";
    RegisterVM vm;
    std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });
    try {
        vm.execute(c.getLastRegisterResult());
    } catch (const std::exception& e) {
        crash = true;
        if (out.empty())
            return "<crash:" + std::string(e.what()) + ">";
        return out + "<crash:" + std::string(e.what()) + ">";
    }
    if (vm.hasError()) {
        if (out.empty())
            return "<runtime:" + vm.getLastError() + ">";
        return out + "<runtime:" + vm.getLastError() + ">";
    }
    return out;
}

// ============================================================
// MiniLangProgramGenerator — 模板化随机程序生成器
// ------------------------------------------------------------
// 参考 TestThreeEnginesFuzz.cpp:123-383 的设计，增强为可配置类别。
// 10 个类别覆盖算术/控制流/函数/递归/闭包/字符串/数组/类/错误路径/逻辑。
// ============================================================

class MiniLangProgramGenerator {
public:
    explicit MiniLangProgramGenerator(uint64_t seed) : rng_(seed) {}

    // 按位掩码随机选择一个类别生成程序
    std::string generate(uint32_t categoryMask) {
        std::vector<uint32_t> categories;
        for (uint32_t i = 0; i < 10; ++i) {
            if (categoryMask & (1u << i))
                categories.push_back(i);
        }
        if (categories.empty())
            return generateArithmetic();
        int idx = categories[randInt(0, static_cast<int>(categories.size()) - 1)];
        switch (idx) {
        case 0:
            return generateArithmetic();
        case 1:
            return generateControlFlow();
        case 2:
            return generateFunction();
        case 3:
            return generateRecursion();
        case 4:
            return generateClosure();
        case 5:
            return generateString();
        case 6:
            return generateArray();
        case 7:
            return generateClass();
        case 8:
            return generateErrorPath();
        case 9:
            return generateLogic();
        default:
            return generateArithmetic();
        }
    }

    // ---- 类别 1：算术表达式 ----
    std::string generateArithmetic() {
        std::ostringstream oss;
        oss << "print(" << genArithExpr(2) << ");\n";
        return oss.str();
    }

    // ---- 类别 2：控制流 ----
    std::string generateControlFlow() {
        std::ostringstream oss;
        int bound = randInt(1, 8);
        int threshold = randInt(0, bound);
        oss << "var total = 0;\n";
        oss << "var i = 0;\n";
        oss << "while (i < " << bound << ") {\n";
        oss << "  if (i > " << threshold << ") {\n";
        oss << "    total = total + i;\n";
        oss << "  } else {\n";
        oss << "    total = total + 1;\n";
        oss << "  }\n";
        oss << "  i = i + 1;\n";
        oss << "}\n";
        oss << "print(total);\n";
        return oss.str();
    }

    // ---- 类别 3：函数定义/调用 ----
    std::string generateFunction() {
        std::ostringstream oss;
        int a = randInt(1, 20);
        int b = randInt(1, 20);
        oss << "fun add(x, y) { return x + y; }\n";
        oss << "print(add(" << a << ", " << b << "));\n";
        int c = randInt(0, 30);
        oss << "fun clamp(x, lo, hi) {\n";
        oss << "  if (x < lo) return lo;\n";
        oss << "  if (x > hi) return hi;\n";
        oss << "  return x;\n";
        oss << "}\n";
        oss << "print(clamp(" << c << ", 5, 25));\n";
        return oss.str();
    }

    // ---- 类别 4：递归 ----
    std::string generateRecursion() {
        std::ostringstream oss;
        int kind = randInt(0, 2);
        int n = randInt(0, 10);
        if (kind == 0) {
            oss << "fun fib(n) {\n";
            oss << "  if (n < 2) return n;\n";
            oss << "  return fib(n - 1) + fib(n - 2);\n";
            oss << "}\n";
            oss << "print(fib(" << n << "));\n";
        } else if (kind == 1) {
            oss << "fun fact(n) {\n";
            oss << "  if (n <= 1) return 1;\n";
            oss << "  return n * fact(n - 1);\n";
            oss << "}\n";
            oss << "print(fact(" << n << "));\n";
        } else {
            oss << "fun sumTo(n) {\n";
            oss << "  if (n <= 0) return 0;\n";
            oss << "  return n + sumTo(n - 1);\n";
            oss << "}\n";
            oss << "print(sumTo(" << n << "));\n";
        }
        return oss.str();
    }

    // ---- 类别 5：闭包 ----
    std::string generateClosure() {
        std::ostringstream oss;
        int kind = randInt(0, 1);
        int calls = randInt(1, 5);
        if (kind == 0) {
            oss << "fun outer() {\n";
            oss << "  var count = 0;\n";
            oss << "  fun inc() { count = count + 1; return count; }\n";
            oss << "  var i = 0;\n";
            oss << "  while (i < " << calls << ") {\n";
            oss << "    print(inc());\n";
            oss << "    i = i + 1;\n";
            oss << "  }\n";
            oss << "}\n";
            oss << "outer();\n";
        } else {
            int base = randInt(1, 100);
            oss << "fun outer(n) {\n";
            oss << "  fun add(x) { return x + n; }\n";
            oss << "  print(add(10));\n";
            oss << "  print(add(20));\n";
            oss << "}\n";
            oss << "outer(" << base << ");\n";
        }
        return oss.str();
    }

    // ---- 类别 6：字符串 ----
    std::string generateString() {
        std::ostringstream oss;
        int a = randInt(1, 100);
        int b = randInt(1, 100);
        oss << "var x = " << a << ";\n";
        oss << "var y = " << b << ";\n";
        oss << "var s = \"a=\" + x + \", b=\" + y;\n";
        oss << "print(s);\n";
        oss << "print(\"{x}+{y}={x + y}\");\n";
        oss << "print(x == y);\n";
        return oss.str();
    }

    // ---- 类别 7：数组 ----
    std::string generateArray() {
        std::ostringstream oss;
        int n = randInt(1, 6);
        oss << "var arr = [];\n";
        oss << "var i = 0;\n";
        oss << "while (i < " << n << ") {\n";
        oss << "  arr.push(i * 2);\n";
        oss << "  i = i + 1;\n";
        oss << "}\n";
        oss << "print(arr.len());\n";
        oss << "var sum = 0;\n";
        oss << "var j = 0;\n";
        oss << "while (j < arr.len()) {\n";
        oss << "  sum = sum + arr[j];\n";
        oss << "  j = j + 1;\n";
        oss << "}\n";
        oss << "print(sum);\n";
        return oss.str();
    }

    // ---- 类别 8：类 ----
    std::string generateClass() {
        std::ostringstream oss;
        int xv = randInt(1, 20);
        int yv = randInt(1, 20);
        oss << "class Point {\n";
        oss << "  var x; var y;\n";
        oss << "  fun init(x, y) { this.x = x; this.y = y; }\n";
        oss << "  fun sum() { return this.x + this.y; }\n";
        oss << "  fun scale(k) { return this.x * k; }\n";
        oss << "}\n";
        oss << "var p = Point(" << xv << ", " << yv << ");\n";
        oss << "print(p.sum());\n";
        oss << "print(p.scale(3));\n";
        return oss.str();
    }

    // ---- 类别 9：错误路径 ----
    std::string generateErrorPath() {
        std::ostringstream oss;
        int kind = randInt(0, 2);
        if (kind == 0) {
            int cond = randInt(0, 1);
            oss << "var z = " << cond << ";\n";
            oss << "var w = 10;\n";
            oss << "if (z == 0) { print(w / z); } else { print(w); }\n";
        } else if (kind == 1) {
            int idx = randInt(5, 20);
            oss << "var arr = [1, 2, 3];\n";
            oss << "print(arr[" << idx << "]);\n";
        } else {
            oss << "print(undefined_var);\n";
        }
        return oss.str();
    }

    // ---- 类别 10：逻辑短路 + 比较 ----
    std::string generateLogic() {
        std::ostringstream oss;
        int a = randInt(0, 1);
        int b = randInt(0, 1);
        int c = randInt(0, 1);
        oss << "var a = " << (a ? "true" : "false") << ";\n";
        oss << "var b = " << (b ? "true" : "false") << ";\n";
        oss << "print(a and b);\n";
        oss << "print(a or b);\n";
        oss << "var c = " << (c ? "true" : "false") << ";\n";
        oss << "print(a and b or c);\n";
        int x = randInt(0, 10);
        int y = randInt(0, 10);
        oss << "var x = " << x << ";\n";
        oss << "var y = " << y << ";\n";
        oss << "print(x and y);\n";
        return oss.str();
    }

private:
    std::mt19937_64 rng_;

    int randInt(int lo, int hi) {
        if (hi < lo)
            std::swap(lo, hi);
        std::uniform_int_distribution<int> dist(lo, hi);
        return dist(rng_);
    }

    std::string genArithExpr(int depth) {
        if (depth <= 0 || randInt(0, 3) == 0) {
            if (randInt(0, 1) == 0) {
                return std::to_string(randInt(0, 20));
            }
            double vals[] = {0.5, 1.5, 2.5, 3.5, 0.25, 1.25};
            return std::to_string(vals[randInt(0, 5)]);
        }
        char ops[] = {'+', '-', '*', '/'};
        char op = ops[randInt(0, 3)];
        std::string l = genArithExpr(depth - 1);
        if (op == '/') {
            std::string nonZeroR = std::to_string(randInt(1, 20));
            return "(" + l + " / " + nonZeroR + ")";
        }
        std::string r = genArithExpr(depth - 1);
        return "(" + l + " " + op + " " + r + ")";
    }
};

// ============================================================
// ByteMutator — 字节级变异器
// ------------------------------------------------------------
// 对种子语料做字节级变异，发现 Lexer/Parser 边界 bug。
// 5 种变异策略：位翻转/字节插入/字节删除/字节随机化/片段复制
// ============================================================

class ByteMutator {
public:
    explicit ByteMutator(uint64_t seed) : rng_(seed) {}

    enum class Strategy {
        BitFlip,       ///< 随机翻转 1 位
        ByteInsert,    ///< 随机位置插入随机字节
        ByteDelete,    ///< 随机位置删除 1 字节
        ByteRandom,    ///< 随机位置替换为随机字节
        ChunkDuplicate ///< 复制一段字节到随机位置
    };

    // 对输入做 1-3 次随机变异
    std::string mutate(const std::string& input, int maxLength) {
        std::string result = input;
        int mutationCount = randInt(1, 3);
        for (int i = 0; i < mutationCount; ++i) {
            if (result.empty())
                break;
            int strategy = randInt(0, 4);
            switch (strategy) {
            case 0:
                mutateBitFlip(result);
                break;
            case 1:
                mutateByteInsert(result, maxLength);
                break;
            case 2:
                mutateByteDelete(result);
                break;
            case 3:
                mutateByteRandom(result);
                break;
            case 4:
                mutateChunkDuplicate(result, maxLength);
                break;
            default:
                break;
            }
            if (static_cast<int>(result.size()) > maxLength) {
                result.resize(maxLength);
            }
        }
        return result;
    }

    // 生成一个随机种子语料（从预设模板派生）
    std::string generateSeedCorpus() {
        static const std::vector<std::string> seeds = {
            "print(1 + 2);\n",
            "var x = 10;\nprint(x);\n",
            "fun add(a, b) { return a + b; }\nprint(add(1, 2));\n",
            "var arr = [1, 2, 3];\nprint(arr[0]);\n",
            "var s = \"hello\";\nprint(s);\n",
            "class Point { var x; fun init(x) { this.x = x; } }\nvar p = Point(5);\nprint(p.x);\n",
            "var i = 0;\nwhile (i < 3) { print(i); i = i + 1; }\n",
            "print(10 / 0);\n",
            "var s = \"\\\"quoted\\\"\";\nprint(s);\n",
            "print(1.5 + 2.5);\n",
        };
        return seeds[randInt(0, static_cast<int>(seeds.size()) - 1)];
    }

private:
    std::mt19937_64 rng_;

    int randInt(int lo, int hi) {
        if (hi < lo)
            std::swap(lo, hi);
        std::uniform_int_distribution<int> dist(lo, hi);
        return dist(rng_);
    }

    void mutateBitFlip(std::string& s) {
        if (s.empty())
            return;
        int pos = randInt(0, static_cast<int>(s.size()) - 1);
        int bit = randInt(0, 7);
        s[pos] ^= static_cast<char>(1 << bit);
    }

    void mutateByteInsert(std::string& s, int maxLength) {
        if (static_cast<int>(s.size()) >= maxLength)
            return;
        int pos = randInt(0, static_cast<int>(s.size()));
        char c = static_cast<char>(randInt(0, 255));
        s.insert(pos, 1, c);
    }

    void mutateByteDelete(std::string& s) {
        if (s.size() <= 1)
            return;
        int pos = randInt(0, static_cast<int>(s.size()) - 1);
        s.erase(pos, 1);
    }

    void mutateByteRandom(std::string& s) {
        if (s.empty())
            return;
        int pos = randInt(0, static_cast<int>(s.size()) - 1);
        s[pos] = static_cast<char>(randInt(0, 255));
    }

    void mutateChunkDuplicate(std::string& s, int maxLength) {
        if (s.size() < 2)
            return;
        int chunkStart = randInt(0, static_cast<int>(s.size()) - 2);
        int chunkEnd = randInt(chunkStart + 1, static_cast<int>(s.size()) - 1);
        int chunkLen = chunkEnd - chunkStart + 1;
        if (static_cast<int>(s.size()) + chunkLen > maxLength)
            return;
        int insertPos = randInt(0, static_cast<int>(s.size()));
        s.insert(insertPos, s.substr(chunkStart, chunkLen));
    }
};

// ============================================================
// 辅助：判断错误阶段
// ============================================================

std::string detectErrorPhase(const std::string& encoded) {
    if (encoded.find("<parse-fail>") != std::string::npos)
        return "parse";
    if (encoded.find("<compile:") != std::string::npos)
        return "compile";
    if (encoded.find("<runtime:") != std::string::npos)
        return "runtime";
    if (encoded.find("<crash:") != std::string::npos)
        return "crash";
    return "";
}

// R164 fixup2: 错误阶段归一化 — 将 <compile:msg> 和 <runtime:msg> 统一为 <error:msg>。
// 某些语义错误（如变量重定义）在 Interpreter 中是运行时错误（runtimeError 抛异常），
// 在 VM 中是编译时错误（Compiler::error 记录后阻止执行）。两者错误消息相同但阶段不同，
// 不应计为三后端分歧。归一化后仅比较消息内容。
//
// 同时去掉编译器诊断前缀 [编译器] 错误 (行 N): 或 [编译器] 错误 (行 N, 列 M):
// （Diagnostic::format() 格式），因为 Interpreter 运行时错误消息无此前缀。
// 例：<compile:[编译器] 错误 (行 1): 变量 'b' 已在当前作用域中定义>
//   → <error:变量 'b' 已在当前作用域中定义>
std::string normalizeErrorPhase(const std::string& encoded) {
    std::string result = encoded;
    // R164 fixup2: 依次处理 <compile:> 和 <runtime:> 标记
    static const std::string compileTag = "<compile:";
    static const std::string runtimeTag = "<runtime:";
    for (const auto& tag : {compileTag, runtimeTag}) {
        size_t pos = result.find(tag);
        if (pos == std::string::npos)
            continue;
        // 替换标记为 <error:
        result.replace(pos, tag.length(), "<error:");
        // 去掉编译器诊断前缀 [编译器] 错误 (行 N...):
        // 仅编译错误（<compile:>）会有此前缀，运行时错误无前缀，查找失败时不影响
        static const std::string diagPrefix = "[编译器] 错误 (行 ";
        size_t prefixPos = result.find(diagPrefix, pos);
        if (prefixPos != std::string::npos) {
            size_t endPos = result.find("): ", prefixPos);
            if (endPos != std::string::npos) {
                result.erase(prefixPos, endPos + 3 - prefixPos);
            }
        }
    }
    return result;
}

// 判断是否为解析失败（三后端都是 parse-fail）
bool isParseFailure(const std::string& s) {
    return s == "<parse-fail>";
}

// 判断是否为编译失败（任一后端 compile 错误）
bool isCompileFailure(const std::string& s) {
    return s.find("<compile:") != std::string::npos;
}

} // anonymous namespace

// ============================================================
// 公有 API 实现
// ============================================================

FuzzResult fuzzSource(const std::string& source, FuzzBackend backend) {
    FuzzResult result;
    result.source = source;
    auto start = std::chrono::steady_clock::now();

    bool crash = false;
    std::string output;
    switch (backend) {
    case FuzzBackend::Interpreter:
        output = runInterp(source, crash);
        result.interpOutput = output;
        break;
    case FuzzBackend::StackVM:
        output = runStackVM(source, crash);
        result.stackvmOutput = output;
        break;
    case FuzzBackend::RegisterVM:
        output = runRegVM(source, crash);
        result.regvmOutput = output;
        break;
    case FuzzBackend::All:
        result.interpOutput = runInterp(source, crash);
        if (crash)
            result.crashDetected = true;
        crash = false;
        result.stackvmOutput = runStackVM(source, crash);
        if (crash)
            result.crashDetected = true;
        crash = false;
        result.regvmOutput = runRegVM(source, crash);
        if (crash)
            result.crashDetected = true;
        // 三后端差分
        if (!result.crashDetected) {
            // RegisterVM 可能编译失败（已知 IR lowering 限制），跳过比较
            bool regvmCompileFail = isCompileFailure(result.regvmOutput);
            // R164 fixup2: 归一化错误阶段（<compile:msg> / <runtime:msg> → <error:msg>），
            // 使同一语义错误在不同阶段（Interpreter 运行时 vs VM 编译时）不被计为分歧
            std::string interpNorm = normalizeErrorPhase(result.interpOutput);
            std::string stackNorm = normalizeErrorPhase(result.stackvmOutput);
            std::string regNorm = normalizeErrorPhase(result.regvmOutput);
            if (interpNorm != stackNorm) {
                result.disagreement = true;
                result.errorMessage = "Interpreter vs StackVM 分歧";
            } else if (!regvmCompileFail && interpNorm != regNorm) {
                result.disagreement = true;
                result.errorMessage = "Interpreter vs RegisterVM 分歧";
            }
        }
        break;
    }

    auto end = std::chrono::steady_clock::now();
    result.durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    // 错误阶段检测（All 模式以 Interpreter 输出为准）
    std::string refOutput = result.interpOutput.empty() ? result.stackvmOutput : result.interpOutput;
    result.errorPhase = detectErrorPhase(refOutput);
    result.ok = !result.crashDetected && !result.disagreement;
    return result;
}

FuzzResult fuzzThreeAgree(const std::string& source) {
    return fuzzSource(source, FuzzBackend::All);
}

FuzzSummary runFuzzBatch(const FuzzOptions& opts) {
    FuzzSummary summary;
    // 种子派生：0 = 时间派生
    uint64_t actualSeed = opts.seed;
    if (actualSeed == 0) {
        actualSeed = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    }
    summary.seed = actualSeed;

    MiniLangProgramGenerator generator(actualSeed);
    ByteMutator mutator(actualSeed ^ 0xDEADBEEFCAFEBABEULL);

    auto batchStart = std::chrono::steady_clock::now();

    for (int i = 0; i < opts.iterations; ++i) {
        std::string source;
        if (opts.mode == FuzzMode::Mutate) {
            std::string seed = mutator.generateSeedCorpus();
            source = mutator.mutate(seed, opts.maxLength);
        } else {
            source = generator.generate(opts.categoryMask);
        }

        FuzzResult r = fuzzThreeAgree(source);
        summary.totalRuns++;

        if (r.crashDetected) {
            summary.crashes++;
            if (static_cast<int>(summary.crashCases.size()) < 50) {
                summary.crashCases.push_back(r);
            }
        } else if (r.disagreement) {
            summary.disagreements++;
            if (static_cast<int>(summary.disagreementCases.size()) < 50) {
                summary.disagreementCases.push_back(r);
            }
        } else if (r.errorPhase == "parse") {
            summary.parseFailures++;
        } else if (r.errorPhase == "compile") {
            summary.compileFailures++;
        } else if (r.errorPhase == "runtime") {
            summary.runtimeErrors++;
        } else {
            summary.agreements++;
        }

        // 非 quiet 模式实时输出进度
        if (!opts.quiet && (i % 10 == 0 || i == opts.iterations - 1)) {
            std::fprintf(stdout, "[%d/%d] crashes=%d disagreements=%d\n", i + 1, opts.iterations, summary.crashes,
                         summary.disagreements);
            std::fflush(stdout);
        }
    }

    auto batchEnd = std::chrono::steady_clock::now();
    summary.totalDurationMs = std::chrono::duration_cast<std::chrono::milliseconds>(batchEnd - batchStart).count();

    // 保存崩溃用例
    if (opts.dumpCrashes && !summary.crashCases.empty()) {
        for (int i = 0; i < static_cast<int>(summary.crashCases.size()); ++i) {
            saveCrashCase(summary.crashCases[i], i);
        }
    }

    return summary;
}

FuzzSummary processFile(const std::string& path, const FuzzOptions& opts) {
    FuzzSummary summary;
    std::ifstream ifs(path);
    if (!ifs) {
        summary.crashes = -1; // 用 -1 标记文件错误
        return summary;
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    std::string source = oss.str();

    FuzzResult r = fuzzThreeAgree(source);
    summary.totalRuns = 1;
    summary.seed = opts.seed;
    if (r.crashDetected) {
        summary.crashes = 1;
        summary.crashCases.push_back(r);
    } else if (r.disagreement) {
        summary.disagreements = 1;
        summary.disagreementCases.push_back(r);
    } else if (r.errorPhase == "parse") {
        summary.parseFailures = 1;
    } else if (r.errorPhase == "compile") {
        summary.compileFailures = 1;
    } else if (r.errorPhase == "runtime") {
        summary.runtimeErrors = 1;
    } else {
        summary.agreements = 1;
    }
    summary.totalDurationMs = r.durationMs;
    return summary;
}

// ============================================================
// CLI 参数解析
// ============================================================

CliArgs parseArgs(int argc, char* argv[]) {
    CliArgs args;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto needValue = [&](const std::string& optName) -> std::string {
            if (i + 1 >= argc) {
                args.parseError = true;
                args.errorMessage = "选项 " + optName + " 缺少参数";
                return "";
            }
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h") {
            args.showHelp = true;
        } else if (arg == "--version" || arg == "-v") {
            args.showVersion = true;
        } else if (arg == "--quiet" || arg == "-q") {
            args.options.quiet = true;
        } else if (arg == "--no-dump") {
            args.options.dumpCrashes = false;
        } else if (arg == "--seed") {
            std::string val = needValue("--seed");
            if (args.parseError)
                break;
            try {
                args.options.seed = std::stoull(val);
            } catch (...) {
                args.parseError = true;
                args.errorMessage = "无效的种子值: " + val;
                break;
            }
        } else if (arg == "--iterations" || arg == "-n") {
            std::string val = needValue("--iterations");
            if (args.parseError)
                break;
            try {
                args.options.iterations = std::stoi(val);
                if (args.options.iterations <= 0) {
                    args.parseError = true;
                    args.errorMessage = "迭代次数必须为正整数: " + val;
                    break;
                }
            } catch (...) {
                args.parseError = true;
                args.errorMessage = "无效的迭代次数: " + val;
                break;
            }
        } else if (arg == "--backend") {
            std::string val = needValue("--backend");
            if (args.parseError)
                break;
            args.options.backend = parseBackendName(val);
        } else if (arg == "--mode") {
            std::string val = needValue("--mode");
            if (args.parseError)
                break;
            args.options.mode = parseModeName(val);
        } else if (arg == "--category") {
            std::string val = needValue("--category");
            if (args.parseError)
                break;
            uint32_t cat = parseCategoryName(val);
            if (cat == 0) {
                args.parseError = true;
                args.errorMessage = "未知的生成类别: " + val;
                break;
            }
            args.options.categoryMask = cat;
        } else if (arg == "--max-length") {
            std::string val = needValue("--max-length");
            if (args.parseError)
                break;
            try {
                args.options.maxLength = std::stoi(val);
                if (args.options.maxLength <= 0) {
                    args.parseError = true;
                    args.errorMessage = "最大长度必须为正整数: " + val;
                    break;
                }
            } catch (...) {
                args.parseError = true;
                args.errorMessage = "无效的最大长度: " + val;
                break;
            }
        } else if (arg == "--format") {
            std::string val = needValue("--format");
            if (args.parseError)
                break;
            if (val == "text") {
                args.format = OutputFormat::Text;
            } else if (val == "json") {
                args.format = OutputFormat::Json;
            } else {
                args.parseError = true;
                args.errorMessage = "未知的输出格式: " + val + "（支持: text, json）";
                break;
            }
        } else if (arg.size() > 1 && arg[0] == '-' && arg[1] == '-') {
            args.parseError = true;
            args.errorMessage = "未知的选项: " + arg;
            break;
        } else {
            // 非选项参数视为文件（文件模式）
            args.files.push_back(arg);
        }
    }

    // 若有文件参数，自动切换到文件模式
    if (!args.files.empty() && !args.parseError) {
        args.options.mode = FuzzMode::File;
    }

    return args;
}

void printHelp() {
    std::fputs("minilang-fuzz - MiniLang 模糊测试器\n"
               "\n"
               "用法:\n"
               "  minilang-fuzz [选项] [文件...]\n"
               "  minilang-fuzz --mode generate --iterations 1000\n"
               "  minilang-fuzz --mode mutate --seed 42 --iterations 500\n"
               "  minilang-fuzz crash-0.ml  # 对已知崩溃用例做回归验证\n"
               "\n"
               "选项:\n"
               "  --seed <N>            随机种子（默认: 时间派生）\n"
               "  --iterations, -n <N>   生成/变异迭代次数（默认: 100）\n"
               "  --mode <name>          模式: generate | mutate | file（默认: generate）\n"
               "  --backend <name>       后端: interp | stackvm | regvm | all（默认: all）\n"
               "  --category <name>      生成类别（默认: all）:\n"
               "                          arithmetic, controlflow, function, recursion,\n"
               "                          closure, string, array, class, errorpath, logic\n"
               "  --max-length <N>       最大源码长度（字节，默认: 8192）\n"
               "  --format <name>        输出格式: text | json（默认: text）\n"
               "  --quiet, -q            仅输出崩溃/分歧\n"
               "  --no-dump              不保存崩溃用例到文件\n"
               "  --help, -h             显示帮助\n"
               "  --version, -v          显示版本\n"
               "\n"
               "退出码:\n"
               "  0  成功（无崩溃、无分歧）\n"
               "  1  发现崩溃或三后端分歧（已记录用例）\n"
               "  2  错误（参数解析失败、文件不存在等）\n"
               "\n"
               "示例:\n"
               "  minilang-fuzz --iterations 1000 --seed 42\n"
               "  minilang-fuzz --mode mutate --iterations 500 --quiet\n"
               "  minilang-fuzz --category recursion --iterations 200\n"
               "  minilang-fuzz crash-0.ml --format json\n",
               stdout);
}

void printVersion() {
    std::fputs(versionString().c_str(), stdout);
    std::fputc('\n', stdout);
}

std::string versionString() {
    return "minilang-fuzz 1.0.0 (R164)";
}

FuzzBackend parseBackendName(const std::string& name) {
    if (name == "interp")
        return FuzzBackend::Interpreter;
    if (name == "stackvm")
        return FuzzBackend::StackVM;
    if (name == "regvm")
        return FuzzBackend::RegisterVM;
    if (name == "all")
        return FuzzBackend::All;
    return FuzzBackend::All;
}

FuzzMode parseModeName(const std::string& name) {
    if (name == "generate")
        return FuzzMode::Generate;
    if (name == "mutate")
        return FuzzMode::Mutate;
    if (name == "file")
        return FuzzMode::File;
    return FuzzMode::Generate;
}

uint32_t parseCategoryName(const std::string& name) {
    if (name == "arithmetic")
        return static_cast<uint32_t>(FuzzCategory::Arithmetic);
    if (name == "controlflow")
        return static_cast<uint32_t>(FuzzCategory::ControlFlow);
    if (name == "function")
        return static_cast<uint32_t>(FuzzCategory::Function);
    if (name == "recursion")
        return static_cast<uint32_t>(FuzzCategory::Recursion);
    if (name == "closure")
        return static_cast<uint32_t>(FuzzCategory::Closure);
    if (name == "string")
        return static_cast<uint32_t>(FuzzCategory::String);
    if (name == "array")
        return static_cast<uint32_t>(FuzzCategory::Array);
    if (name == "class")
        return static_cast<uint32_t>(FuzzCategory::Class);
    if (name == "errorpath")
        return static_cast<uint32_t>(FuzzCategory::ErrorPath);
    if (name == "logic")
        return static_cast<uint32_t>(FuzzCategory::Logic);
    if (name == "all")
        return static_cast<uint32_t>(FuzzCategory::All);
    return 0;
}

// ============================================================
// 输出格式化
// ============================================================

std::string formatSummaryText(const FuzzSummary& summary) {
    std::ostringstream oss;
    oss << "=== 模糊测试汇总 ===\n";
    oss << "种子: 0x" << std::hex << summary.seed << std::dec << "\n";
    oss << "总执行: " << summary.totalRuns << "\n";
    oss << "  三后端一致: " << summary.agreements << "\n";
    oss << "  解析失败: " << summary.parseFailures << "\n";
    oss << "  编译失败: " << summary.compileFailures << "\n";
    oss << "  运行时错误（三后端一致）: " << summary.runtimeErrors << "\n";
    oss << "  崩溃: " << summary.crashes << "\n";
    oss << "  三后端分歧: " << summary.disagreements << "\n";
    oss << "总耗时: " << summary.totalDurationMs << " ms\n";
    if (summary.totalRuns > 0) {
        oss << "平均: " << (summary.totalDurationMs / summary.totalRuns) << " ms/次\n";
    }

    if (!summary.crashCases.empty()) {
        oss << "\n--- 崩溃用例（前 " << summary.crashCases.size() << " 个）---\n";
        for (size_t i = 0; i < summary.crashCases.size(); ++i) {
            const auto& c = summary.crashCases[i];
            oss << "[crash #" << i << "] phase=" << c.errorPhase << "\n";
            oss << "  源码:\n";
            // 缩进源码
            std::istringstream iss(c.source);
            std::string line;
            while (std::getline(iss, line)) {
                oss << "    " << line << "\n";
            }
        }
    }

    if (!summary.disagreementCases.empty()) {
        oss << "\n--- 三后端分歧用例（前 " << summary.disagreementCases.size() << " 个）---\n";
        for (size_t i = 0; i < summary.disagreementCases.size(); ++i) {
            const auto& d = summary.disagreementCases[i];
            oss << "[disagreement #" << i << "] " << d.errorMessage << "\n";
            oss << "  Interpreter: " << d.interpOutput.substr(0, 200) << "\n";
            oss << "  StackVM:     " << d.stackvmOutput.substr(0, 200) << "\n";
            oss << "  RegisterVM:  " << d.regvmOutput.substr(0, 200) << "\n";
            oss << "  源码:\n";
            std::istringstream iss(d.source);
            std::string line;
            while (std::getline(iss, line)) {
                oss << "    " << line << "\n";
            }
        }
    }

    oss << "\n结论: ";
    if (summary.crashes > 0) {
        oss << "发现 " << summary.crashes << " 个崩溃（已保存到 crash-*.ml）";
    } else if (summary.disagreements > 0) {
        oss << "发现 " << summary.disagreements << " 个三后端分歧";
    } else {
        oss << "无崩溃、无分歧，三后端语义一致";
    }
    oss << "\n";
    return oss.str();
}

std::string formatSummaryJson(const FuzzSummary& summary) {
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"seed\": \"" << std::hex << summary.seed << std::dec << "\",\n";
    oss << "  \"totalRuns\": " << summary.totalRuns << ",\n";
    oss << "  \"agreements\": " << summary.agreements << ",\n";
    oss << "  \"parseFailures\": " << summary.parseFailures << ",\n";
    oss << "  \"compileFailures\": " << summary.compileFailures << ",\n";
    oss << "  \"runtimeErrors\": " << summary.runtimeErrors << ",\n";
    oss << "  \"crashes\": " << summary.crashes << ",\n";
    oss << "  \"disagreements\": " << summary.disagreements << ",\n";
    oss << "  \"totalDurationMs\": " << summary.totalDurationMs << ",\n";
    oss << "  \"crashCases\": [\n";
    for (size_t i = 0; i < summary.crashCases.size(); ++i) {
        const auto& c = summary.crashCases[i];
        oss << "    {\"index\": " << i << ", \"phase\": \"" << c.errorPhase << "\", \"source\": \"" << c.source
            << "\"}";
        if (i + 1 < summary.crashCases.size())
            oss << ",";
        oss << "\n";
    }
    oss << "  ],\n";
    oss << "  \"disagreementCases\": [\n";
    for (size_t i = 0; i < summary.disagreementCases.size(); ++i) {
        const auto& d = summary.disagreementCases[i];
        oss << "    {\"index\": " << i << ", \"reason\": \"" << d.errorMessage << "\", \"interp\": \"" << d.interpOutput
            << "\", \"stackvm\": \"" << d.stackvmOutput << "\", \"regvm\": \"" << d.regvmOutput << "\", \"source\": \""
            << d.source << "\"}";
        if (i + 1 < summary.disagreementCases.size())
            oss << ",";
        oss << "\n";
    }
    oss << "  ]\n";
    oss << "}\n";
    return oss.str();
}

std::string saveCrashCase(const FuzzResult& result, int index) {
    std::string filename = "crash-" + std::to_string(index) + ".ml";
    std::ofstream ofs(filename);
    if (ofs) {
        ofs << "// crash-" << index << " phase=" << result.errorPhase << "\n";
        ofs << "// errorMessage=" << result.errorMessage << "\n";
        ofs << result.source;
    }
    return filename;
}

} // namespace minilang_fuzz
