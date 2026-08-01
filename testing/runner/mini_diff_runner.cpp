// ============================================================
// testing/runner/mini_diff_runner.cpp
// ------------------------------------------------------------
// 差分测试执行内核（阶段一）。运行「单个 .mini 文件 × 单个后端」，
// 在进程内完整走 Lexer → Parser →（Compiler）→ Backend 执行，
// 捕获 print 输出与错误状态，输出一行结构化 JSON。
//
// 为什么是「单后端单进程」：
//   - 崩溃隔离：某一后端的段错误/abort 不会带走其他后端的结果
//     （driver.py 为每个后端各起一个本 runner 进程）。
//   - 超时归因：编译/运行双阶段各自计时，看门狗线程可精确标注
//     HANG 发生在哪个阶段。
//
// 执行语义严格对齐 tests/common/ThreeBackends.h 与 cli/fuzz_core.cpp
// 的既有差分约定（错误标记 <parse-fail> / <compile:msg> /
// <runtime:msg> / <jit-compile:msg> / <jit-runtime:msg>），
// 确保本工具与项目自带一致性测试对「同一语义」的判定完全一致。
//
// 本文件是纯新增测试代码，不修改编译器任何实现逻辑。
//
// 退出码约定（与 driver.py 对齐）：
//   0   ok
//   10  parse_error        11  compile_error
//   12  runtime_error      13  jit_unsupported（优雅降级，非 bug）
//   14  jit_runtime_error  20  crash（未捕获 C++ 异常）
//   2   io_error / 用法错误
//   124 hang（看门狗触发，GNU timeout 约定）
// ============================================================

#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "ast/ASTNode.h"
#include "interpreter/Interpreter.h"
#include "interpreter/RuntimeExceptions.h"
#include "interpreter/Value.h"
#include "compiler/Compiler.h"
#include "compiler/VM.h"
#include "compiler/RegisterVM.h"
#ifdef MINILANG_USE_JIT
#include "compiler/JIT.h"
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

namespace {

// ---- 时间工具 ----
long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// ---- 看门狗共享状态（跨线程，原子访问）----
// phase: 0=未开始 1=compile 2=run 3=已完成
std::atomic<int> g_phase{0};
std::atomic<long long> g_deadlineMs{0}; // 0 表示未武装；>0 为绝对截止时刻（ms）
std::atomic<bool> g_done{false};

// 看门狗触发时打印的最小 JSON（不访问主线程可变数据，避免数据竞争）。
std::string g_backendName;
std::string g_filePath;

// ---- JSON 字符串转义（含控制字符 \u00XX，兼容任意输出字节）----
std::string escapeJson(const std::string& s) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20) {
                out += "\\u00";
                out += hex[(c >> 4) & 0xF];
                out += hex[c & 0xF];
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out;
}

// ---- FNV-1a 64 位哈希（无依赖，仅用于报告紧凑标识；voting 用完整字符串）----
std::string fnv1a64Hex(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return std::string(buf);
}

// ---- 单次执行结果 ----
struct RunOutcome {
    std::string status = "ok";  // ok/parse_error/compile_error/runtime_error/
                                // jit_unsupported/jit_runtime_error/crash
    bool compileOk = false;
    bool runOk = false;
    bool degraded = false;      // JIT 不支持 → 优雅降级（voting 时应跳过该后端）
    std::string output;         // 纯 print 输出
    std::string diff;           // 差分比较用规范串（output + 错误标记），对齐 ThreeBackends
    std::string error;          // 错误消息文本（stderr 语义）
    int exitCode = 0;
};

// 武装看门狗：进入某阶段前设置截止时刻。
void armWatchdog(int phase, int timeoutMs) {
    g_phase.store(phase, std::memory_order_relaxed);
    g_deadlineMs.store(nowMs() + timeoutMs, std::memory_order_relaxed);
}

// 看门狗线程体：周期检查是否超过截止时刻，超时则打印 hang JSON 并硬退出。
void watchdogLoop() {
    while (!g_done.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        long long dl = g_deadlineMs.load(std::memory_order_relaxed);
        if (dl > 0 && nowMs() > dl) {
            int phase = g_phase.load(std::memory_order_relaxed);
            const char* phaseName = (phase == 1) ? "compile" : (phase == 2) ? "run" : "unknown";
            std::printf(
                "{\"backend\":\"%s\",\"file\":\"%s\",\"status\":\"hang\",\"phase\":\"%s\","
                "\"compile_ok\":false,\"run_ok\":false,\"degraded\":false,"
                "\"exit_code\":124,\"output_len\":0,\"output_hash\":\"\",\"diff_hash\":\"\","
                "\"error\":\"watchdog timeout in %s phase\"}\n",
                escapeJson(g_backendName).c_str(), escapeJson(g_filePath).c_str(), phaseName,
                phaseName);
            std::fflush(stdout);
            std::_Exit(124);
        }
    }
}

// ============================================================
// 后端执行封装（镜像 tests/common/ThreeBackends.h 的规范语义）
// ============================================================

RunOutcome runInterp(const std::string& src, int compileTimeoutMs, int runTimeoutMs) {
    RunOutcome r;
    armWatchdog(1, compileTimeoutMs);
    Lexer lx;
    auto tk = lx.scan(src);
    if (lx.getDiagnostics().hasErrors()) {
        r.status = "parse_error"; r.diff = "<parse-fail>"; r.error = lx.getDiagnostics().summary();
        r.exitCode = 10; return r;
    }
    Parser p;
    auto ast = p.parse(tk);
    if (!ast || p.hasErrors()) {
        r.status = "parse_error"; r.diff = "<parse-fail>"; r.error = "parse failed";
        r.exitCode = 10; return r;
    }
    r.compileOk = true;
    Interpreter interp;
    // import 隔离：注入空 moduleLoader，避免触发真实文件模块加载（对齐 fuzz_core）
    interp.setModuleLoader([](const std::string&) { return std::string(); });
    std::string out;
    interp.setOutputCallback([&](const std::string& s) { out += s; });
    armWatchdog(2, runTimeoutMs);
    try {
        interp.execute(*ast);
    } catch (const RuntimeError& e) {
        r.status = "runtime_error"; r.output = out; r.error = e.what();
        r.diff = out + "<runtime:" + e.what() + ">"; r.exitCode = 12; return r;
    } catch (const std::exception& e) {
        r.status = "crash"; r.output = out; r.error = e.what();
        r.diff = out + "<crash:" + e.what() + ">"; r.exitCode = 20; return r;
    } catch (...) {
        r.status = "crash"; r.output = out; r.error = "unknown exception";
        r.diff = out + "<crash:unknown>"; r.exitCode = 20; return r;
    }
    r.runOk = true; r.output = out; r.diff = out; return r;
}

// StackVM：useIR=false 为非 IR 路径，true 为 IR 路径（AstIRBuilder → BytecodeIRBackend）。
RunOutcome runStackVM(const std::string& src, bool useIR, int compileTimeoutMs, int runTimeoutMs) {
    RunOutcome r;
    armWatchdog(1, compileTimeoutMs);
    Lexer lx;
    auto tk = lx.scan(src);
    if (lx.getDiagnostics().hasErrors()) {
        r.status = "parse_error"; r.diff = "<parse-fail>"; r.error = lx.getDiagnostics().summary();
        r.exitCode = 10; return r;
    }
    Parser p;
    auto ast = p.parse(tk);
    if (!ast || p.hasErrors()) {
        r.status = "parse_error"; r.diff = "<parse-fail>"; r.error = "parse failed";
        r.exitCode = 10; return r;
    }
    Compiler c;
    if (useIR) c.setUseIR(true);
    c.setModuleLoader([](const std::string&) { return std::string(); });
    auto cr = c.compile(*ast);
    if (c.getDiagnostics().hasErrors()) {
        r.status = "compile_error"; r.error = c.getLastError();
        r.diff = "<compile:" + c.getLastError() + ">"; r.exitCode = 11; return r;
    }
    r.compileOk = true;
    VM vm;
    std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });
    armWatchdog(2, runTimeoutMs);
    try {
        vm.execute(cr);
    } catch (const std::exception& e) {
        r.status = "crash"; r.output = out; r.error = e.what();
        r.diff = out + "<crash:" + e.what() + ">"; r.exitCode = 20; return r;
    } catch (...) {
        r.status = "crash"; r.output = out; r.error = "unknown exception";
        r.diff = out + "<crash:unknown>"; r.exitCode = 20; return r;
    }
    if (vm.hasError()) {
        r.status = "runtime_error"; r.output = out; r.error = vm.getLastError();
        r.diff = out + "<runtime:" + vm.getLastError() + ">"; r.exitCode = 12; return r;
    }
    r.runOk = true; r.output = out; r.diff = out; return r;
}

RunOutcome runRegVM(const std::string& src, int compileTimeoutMs, int runTimeoutMs) {
    RunOutcome r;
    armWatchdog(1, compileTimeoutMs);
    Lexer lx;
    auto tk = lx.scan(src);
    if (lx.getDiagnostics().hasErrors()) {
        r.status = "parse_error"; r.diff = "<parse-fail>"; r.error = lx.getDiagnostics().summary();
        r.exitCode = 10; return r;
    }
    Parser p;
    auto ast = p.parse(tk);
    if (!ast || p.hasErrors()) {
        r.status = "parse_error"; r.diff = "<parse-fail>"; r.error = "parse failed";
        r.exitCode = 10; return r;
    }
    Compiler c;
    c.setUseRegisterVM(true);
    c.setModuleLoader([](const std::string&) { return std::string(); });
    c.compile(*ast);
    if (c.getDiagnostics().hasErrors()) {
        r.status = "compile_error"; r.error = c.getLastError();
        r.diff = "<compile:" + c.getLastError() + ">"; r.exitCode = 11; return r;
    }
    r.compileOk = true;
    RegisterVM vm;
    std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });
    armWatchdog(2, runTimeoutMs);
    try {
        vm.execute(c.getLastRegisterResult());
    } catch (const std::exception& e) {
        r.status = "crash"; r.output = out; r.error = e.what();
        r.diff = out + "<crash:" + e.what() + ">"; r.exitCode = 20; return r;
    } catch (...) {
        r.status = "crash"; r.output = out; r.error = "unknown exception";
        r.diff = out + "<crash:unknown>"; r.exitCode = 20; return r;
    }
    if (vm.hasError()) {
        r.status = "runtime_error"; r.output = out; r.error = vm.getLastError();
        r.diff = out + "<runtime:" + vm.getLastError() + ">"; r.exitCode = 12; return r;
    }
    r.runOk = true; r.output = out; r.diff = out; return r;
}

#ifdef MINILANG_USE_JIT
RunOutcome runJIT(const std::string& src, int compileTimeoutMs, int runTimeoutMs) {
    RunOutcome r;
    armWatchdog(1, compileTimeoutMs);
    Lexer lx;
    auto tk = lx.scan(src);
    if (lx.getDiagnostics().hasErrors()) {
        r.status = "parse_error"; r.diff = "<parse-fail>"; r.error = lx.getDiagnostics().summary();
        r.exitCode = 10; return r;
    }
    Parser p;
    auto ast = p.parse(tk);
    if (!ast || p.hasErrors()) {
        r.status = "parse_error"; r.diff = "<parse-fail>"; r.error = "parse failed";
        r.exitCode = 10; return r;
    }
    Compiler c; // JIT 消费非 IR BytecodeChunk（对齐 ThreeBackends::runJIT）
    c.setModuleLoader([](const std::string&) { return std::string(); });
    auto cr = c.compile(*ast);
    if (c.getDiagnostics().hasErrors()) {
        r.status = "compile_error"; r.error = c.getLastError();
        r.diff = "<compile:" + c.getLastError() + ">"; r.exitCode = 11; return r;
    }
    r.compileOk = true;
    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    armWatchdog(2, runTimeoutMs);
    JitResult result;
    try {
        result = jit.execute(cr);
    } catch (const std::exception& e) {
        r.status = "crash"; r.output = out; r.error = e.what();
        r.diff = out + "<crash:" + e.what() + ">"; r.exitCode = 20; return r;
    } catch (...) {
        r.status = "crash"; r.output = out; r.error = "unknown exception";
        r.diff = out + "<crash:unknown>"; r.exitCode = 20; return r;
    }
    if (result == JitResult::CompileError) {
        // JIT 不支持该特性 → 优雅降级：标记 degraded，voting 时跳过 JIT。
        r.status = "jit_unsupported"; r.degraded = true; r.output = out; r.error = jit.getLastError();
        r.diff = out + "<jit-compile:" + jit.getLastError() + ">"; r.exitCode = 13; return r;
    }
    if (result == JitResult::RuntimeError) {
        r.status = "jit_runtime_error"; r.output = out; r.error = jit.getLastError();
        r.diff = out + "<jit-runtime:" + jit.getLastError() + ">"; r.exitCode = 14; return r;
    }
    r.runOk = true; r.output = out; r.diff = out; return r;
}
#endif

void printResultJson(const RunOutcome& r) {
    std::printf(
        "{\"backend\":\"%s\",\"file\":\"%s\",\"status\":\"%s\",\"phase\":\"done\","
        "\"compile_ok\":%s,\"run_ok\":%s,\"degraded\":%s,\"exit_code\":%d,"
        "\"output_len\":%zu,\"output_hash\":\"%s\",\"diff_hash\":\"%s\","
        "\"output\":\"%s\",\"diff\":\"%s\",\"error\":\"%s\"}\n",
        escapeJson(g_backendName).c_str(), escapeJson(g_filePath).c_str(), r.status.c_str(),
        r.compileOk ? "true" : "false", r.runOk ? "true" : "false", r.degraded ? "true" : "false",
        r.exitCode, r.output.size(), fnv1a64Hex(r.output).c_str(), fnv1a64Hex(r.diff).c_str(),
        escapeJson(r.output).c_str(), escapeJson(r.diff).c_str(), escapeJson(r.error).c_str());
    std::fflush(stdout);
}

void printUsage() {
    std::fprintf(stderr,
        "mini_diff_runner - MiniLang 单后端差分执行内核\n"
        "\n"
        "用法: mini_diff_runner --file <path> --backend <name> [options]\n"
        "\n"
        "后端 (--backend):\n"
        "  interp       树遍历解释器\n"
        "  stackvm      栈式 VM（非 IR 路径）\n"
        "  stackvm-ir   栈式 VM（IR 路径）\n"
        "  regvm        寄存器式 VM（IR 路径）\n"
        "  jit          x86-64 JIT（不支持场景优雅降级）\n"
        "\n"
        "选项:\n"
        "  --compile-timeout-ms N   编译阶段超时（默认 10000）\n"
        "  --run-timeout-ms N       运行阶段超时（默认 10000）\n"
        "  --help                   显示帮助\n");
}

} // namespace

int main(int argc, char* argv[]) {
    std::string file;
    std::string backend;
    int compileTimeoutMs = 10000;
    int runTimeoutMs = 10000;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "错误: %s 需要参数\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--file") file = next("--file");
        else if (a == "--backend") backend = next("--backend");
        else if (a == "--compile-timeout-ms") compileTimeoutMs = std::atoi(next("--compile-timeout-ms"));
        else if (a == "--run-timeout-ms") runTimeoutMs = std::atoi(next("--run-timeout-ms"));
        else if (a == "--help" || a == "-h") { printUsage(); return 0; }
        else { std::fprintf(stderr, "错误: 未知参数 %s\n", a.c_str()); printUsage(); return 2; }
    }

    if (file.empty() || backend.empty()) {
        std::fprintf(stderr, "错误: --file 与 --backend 均为必填\n");
        printUsage();
        return 2;
    }

    // 读取源文件（二进制读入，保留原始字节）
    std::ifstream ifs(file, std::ios::binary);
    if (!ifs) {
        std::printf(
            "{\"backend\":\"%s\",\"file\":\"%s\",\"status\":\"io_error\",\"phase\":\"init\","
            "\"compile_ok\":false,\"run_ok\":false,\"degraded\":false,\"exit_code\":2,"
            "\"output_len\":0,\"output_hash\":\"\",\"diff_hash\":\"\",\"error\":\"cannot open file\"}\n",
            escapeJson(backend).c_str(), escapeJson(file).c_str());
        std::fflush(stdout);
        return 2;
    }
    std::stringstream ss;
    ss << ifs.rdbuf();
    std::string src = ss.str();

    g_backendName = backend;
    g_filePath = file;

    // 启动看门狗线程（在任何后端执行前）
    std::thread watchdog(watchdogLoop);

    RunOutcome outcome;
    if (backend == "interp") {
        outcome = runInterp(src, compileTimeoutMs, runTimeoutMs);
    } else if (backend == "stackvm") {
        outcome = runStackVM(src, /*useIR=*/false, compileTimeoutMs, runTimeoutMs);
    } else if (backend == "stackvm-ir") {
        outcome = runStackVM(src, /*useIR=*/true, compileTimeoutMs, runTimeoutMs);
    } else if (backend == "regvm") {
        outcome = runRegVM(src, compileTimeoutMs, runTimeoutMs);
    } else if (backend == "jit") {
#ifdef MINILANG_USE_JIT
        outcome = runJIT(src, compileTimeoutMs, runTimeoutMs);
#else
        outcome.status = "jit_unsupported";
        outcome.degraded = true;
        outcome.error = "built without MINILANG_USE_JIT";
        outcome.diff = "<jit-compile:disabled>";
        outcome.exitCode = 13;
#endif
    } else {
        g_done.store(true, std::memory_order_relaxed);
        if (watchdog.joinable()) watchdog.join();
        std::fprintf(stderr, "错误: 未知后端 %s\n", backend.c_str());
        printUsage();
        return 2;
    }

    // 停表：解除看门狗武装并回收线程
    g_deadlineMs.store(0, std::memory_order_relaxed);
    g_phase.store(3, std::memory_order_relaxed);
    g_done.store(true, std::memory_order_relaxed);
    if (watchdog.joinable()) watchdog.join();

    printResultJson(outcome);
    return outcome.exitCode;
}
