// ============================================================
// common/CrashHandler.cpp — 跨平台崩溃报告系统实现（R128）
// ------------------------------------------------------------
// Windows: SetUnhandledExceptionFilter + MiniDumpWriteDump
// POSIX:   sigaction + backtrace / backtrace_symbols_fd
//
// 信号安全约束（POSIX handler 内）：
//   - 仅调用 async-signal-safe 函数（write/open/close/snprintf/backtrace*）
//   - 禁用 malloc / std::string / std::ostringstream / Qt API / Logger
//   - 文件路径预构造在 install() 阶段，handler 仅拼接时间戳
// ============================================================

#include "common/CrashHandler.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

// 平台特定头文件
#ifdef _WIN32
#  include <windows.h>
#  include <dbghelp.h>
#  pragma comment(lib, "dbghelp.lib")
#else
#  include <cerrno>
#  include <csignal>
#  include <execinfo.h>
#  include <fcntl.h>
#  include <unistd.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#endif

namespace minilang {

// ============================================================
// 跨平台全局状态：测试回调（async-signal-safe 原子指针）
// ------------------------------------------------------------
// handler 是 static 函数无法访问 this，故测试回调通过全局原子
// 暴露。CrashHandler::setCrashCallback 写入此原子。
// ============================================================
namespace {
std::atomic<void(*)()> g_crashCallback{nullptr};
}

// ============================================================
// 单例
// ============================================================
CrashHandler& CrashHandler::instance() {
    static CrashHandler inst;
    return inst;
}

// ============================================================
// setCrashCallback：桥接到全局原子（handler 中可访问）
// ============================================================
void CrashHandler::setCrashCallback(void (*cb)()) {
    crashCallback_ = cb;
    g_crashCallback.store(cb, std::memory_order_relaxed);
}

// ============================================================
// 公共接口：install / uninstall
// ============================================================
bool CrashHandler::install(const std::string& dumpDir) {
    if (installed_) return true;

    // 默认 dump 目录：<temp>/minilang-crashdumps/
    if (dumpDir.empty()) {
#ifdef _WIN32
        char tempBuf[MAX_PATH];
        DWORD len = GetTempPathA(MAX_PATH, tempBuf);
        if (len == 0 || len >= MAX_PATH) {
            dumpDir_ = std::string(std::filesystem::temp_directory_path().string());
        } else {
            dumpDir_ = std::string(tempBuf, len);
        }
#else
        const char* tmp = std::getenv("TMPDIR");
        dumpDir_ = tmp ? tmp : "/tmp";
#endif
        // 统一使用正斜杠（std::filesystem 跨平台接受）
        if (!dumpDir_.empty() && dumpDir_.back() != '/' && dumpDir_.back() != '\\') {
            dumpDir_ += '/';
        }
        dumpDir_ += "minilang-crashdumps";
    } else {
        dumpDir_ = dumpDir;
    }

    // 创建目录（忽略已存在）
    std::error_code ec;
    std::filesystem::create_directories(dumpDir_, ec);

    bool ok = installPlatform(dumpDir_);
    if (ok) {
        installed_ = true;
    }
    return ok;
}

void CrashHandler::uninstall() {
    if (!installed_) return;
    uninstallPlatform();
    installed_ = false;
}

// ============================================================
// 平台特定 install/uninstall 分发
// ============================================================
bool CrashHandler::installPlatform(const std::string& dumpDir) {
#ifdef _WIN32
    return installWindows(dumpDir);
#else
    return installPosix(dumpDir);
#endif
}

void CrashHandler::uninstallPlatform() {
#ifdef _WIN32
    // Windows: 设置回 nullptr 即可恢复默认处理（WerFault.exe）
    SetUnhandledExceptionFilter(nullptr);
#else
    // POSIX: 恢复默认信号处理（SIG_DFL）
    struct sigaction sa;
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    for (int sig : {SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS}) {
        sigaction(sig, &sa, nullptr);
    }
#endif
}

// ============================================================
// 写入崩溃元信息（.crashmeta JSON 文件）
// ------------------------------------------------------------
// Windows handler 中调用（非严格 async-signal-safe）。
// POSIX handler 不调用此函数，由下次启动时构造。
// ============================================================
void CrashHandler::writeCrashMeta(const std::string& dumpDir,
                                   const std::string& dumpFile,
                                   const std::string& signalName) {
    std::ostringstream oss;
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    oss << "{\n";
    std::tm tmBuf;
    localtime_s(&tmBuf, &t);
    oss << "  \"timestamp\": \"" << std::put_time(&tmBuf, "%Y-%m-%d %H:%M:%S") << "\",\n";
    oss << "  \"signal\": \"" << signalName << "\",\n";
    oss << "  \"dumpFile\": \"" << dumpFile << "\"\n";
    oss << "}\n";

    std::string metaPath = dumpDir + "/.crashmeta";
    std::ofstream f(metaPath, std::ios::trunc);
    if (f.is_open()) {
        f << oss.str();
    }
}

// ============================================================
// 上一次崩溃报告（启动时调用）
// ============================================================
CrashReport CrashHandler::lastCrashReport() {
    CrashReport report;
    namespace fs = std::filesystem;

    fs::path dumpDir(CrashHandler::instance().dumpDirectory());
    if (dumpDir.empty()) {
        // 单例 dumpDir 为空，说明 install() 从未调用过；尝试默认路径
#ifdef _WIN32
        char tempBuf[MAX_PATH];
        DWORD len = GetTempPathA(MAX_PATH, tempBuf);
        if (len > 0 && len < MAX_PATH) {
            dumpDir = std::string(tempBuf, len);
            dumpDir /= "minilang-crashdumps";
        }
#else
        const char* tmp = std::getenv("TMPDIR");
        dumpDir = (tmp ? tmp : "/tmp");
        dumpDir /= "minilang-crashdumps";
#endif
    }

    if (!fs::exists(dumpDir)) return report;

    // 查找最新的 .dmp / .txt 文件
    fs::path latestDump;
    fs::file_time_type latestTime{};
    bool found = false;

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dumpDir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        if (ext != ".dmp" && ext != ".txt") continue;
        auto ftime = entry.last_write_time(ec);
        if (ec) continue;
        if (!found || ftime > latestTime) {
            latestTime = ftime;
            latestDump = entry.path();
            found = true;
        }
    }

    if (!found) return report;

    report.valid = true;
    report.dumpFilePath = latestDump.string();

    // 文件名格式：crash-<signal>-<timestamp>.<ext>
    // 解析 signal 名：第一个 '-' 与第二个 '-' 之间
    // 例：crash-SIGSEGV-20260720-153045.txt → signal=SIGSEGV
    std::string fname = latestDump.filename().string();
    auto firstDash = fname.find('-');
    auto secondDash = (firstDash != std::string::npos) ? fname.find('-', firstDash + 1) : std::string::npos;
    if (firstDash != std::string::npos && secondDash != std::string::npos) {
        report.signalName = fname.substr(firstDash + 1, secondDash - firstDash - 1);
    }

    // 时间戳从文件 mtime 转换
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        latestTime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    auto t = std::chrono::system_clock::to_time_t(sctp);
    std::ostringstream ts;
    std::tm tmBuf;
    localtime_s(&tmBuf, &t);
    ts << std::put_time(&tmBuf, "%Y-%m-%d %H:%M:%S");
    report.timestamp = ts.str();

    // POSIX：读取 backtrace 文件内容
#ifndef _WIN32
    std::ifstream btFile(report.dumpFilePath);
    if (btFile.is_open()) {
        std::ostringstream content;
        content << btFile.rdbuf();
        report.stackTrace = content.str();
    }
#endif

    return report;
}

CrashReport CrashHandler::consumeLastCrashReport() {
    CrashReport report = lastCrashReport();
    if (report.valid) {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (!report.dumpFilePath.empty()) {
            fs::remove(report.dumpFilePath, ec);
        }
    }
    return report;
}

int CrashHandler::cleanupOldReports(int maxAgeDays) {
    namespace fs = std::filesystem;
    fs::path dumpDir(CrashHandler::instance().dumpDirectory());
    if (dumpDir.empty() || !fs::exists(dumpDir)) return 0;

    int removed = 0;
    auto now = fs::file_time_type::clock::now();
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dumpDir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        if (ext != ".dmp" && ext != ".txt") continue;
        auto ftime = entry.last_write_time(ec);
        if (ec) continue;
        auto age = std::chrono::duration_cast<std::chrono::hours>(now - ftime);
        if (age.count() > maxAgeDays * 24) {
            if (fs::remove(entry.path(), ec)) {
                ++removed;
            }
        }
    }
    return removed;
}

// ============================================================
// Windows 实现
// ============================================================
#ifdef _WIN32

namespace {

// 全局 dump 目录（handler 中无法访问 this，需用全局变量）
std::string g_winDumpDir;

// 上一次异常过滤器（用于链式调用）
LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;

// 异常码 → 可读名称
const char* exceptionName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:         return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_BREAKPOINT:               return "EXCEPTION_BREAKPOINT";
    case EXCEPTION_DATATYPE_MISALIGNMENT:    return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DENORMAL_OPERAND:     return "EXCEPTION_FLT_DENORMAL_OPERAND";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INEXACT_RESULT:       return "EXCEPTION_FLT_INEXACT_RESULT";
    case EXCEPTION_FLT_INVALID_OPERATION:    return "EXCEPTION_FLT_INVALID_OPERATION";
    case EXCEPTION_FLT_OVERFLOW:             return "EXCEPTION_FLT_OVERFLOW";
    case EXCEPTION_FLT_STACK_CHECK:          return "EXCEPTION_FLT_STACK_CHECK";
    case EXCEPTION_FLT_UNDERFLOW:            return "EXCEPTION_FLT_UNDERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION:      return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:            return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW:             return "EXCEPTION_INT_OVERFLOW";
    case EXCEPTION_INVALID_DISPOSITION:      return "EXCEPTION_INVALID_DISPOSITION";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
    case EXCEPTION_PRIV_INSTRUCTION:         return "EXCEPTION_PRIV_INSTRUCTION";
    case EXCEPTION_SINGLE_STEP:              return "EXCEPTION_SINGLE_STEP";
    case EXCEPTION_STACK_OVERFLOW:           return "EXCEPTION_STACK_OVERFLOW";
    default:                                 return "UNKNOWN_EXCEPTION";
    }
}

LONG WINAPI minilangExceptionFilter(EXCEPTION_POINTERS* ep) {
    // 测试回调（不进行真实崩溃写入）
    if (auto cb = g_crashCallback.load(std::memory_order_relaxed)) {
        cb();
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // 构造 dump 文件名：crash-<SIGNAL>-<YYYYMMDD-HHMMSS>.dmp
    const char* excName = exceptionName(ep->ExceptionRecord->ExceptionCode);
    SYSTEMTIME st;
    GetLocalTime(&st);
    char fname[256];
    snprintf(fname, sizeof(fname), "crash-%s-%04d%02d%02d-%02d%02d%02d.dmp",
             excName,
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    std::string dumpPath = g_winDumpDir + "\\" + fname;

    // 写 minidump
    HANDLE hFile = CreateFileA(dumpPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
                          MiniDumpNormal, &mei, nullptr, nullptr);
        CloseHandle(hFile);
    }

    // 写元信息文件
    CrashHandler::writeCrashMeta(g_winDumpDir, dumpPath, excName);

    // 链式调用上一个过滤器（如 WerFault）
    if (g_prevFilter) {
        return g_prevFilter(ep);
    }
    return EXCEPTION_EXECUTE_HANDLER; // 终止进程
}

} // anonymous namespace

bool CrashHandler::installWindows(const std::string& dumpDir) {
    g_winDumpDir = dumpDir;
    g_prevFilter = SetUnhandledExceptionFilter(minilangExceptionFilter);
    // 同时注册 pure virtual call handler 与 invalid parameter handler
    // 这些是 CRT 级别的扩展，覆盖更多崩溃路径
    _set_purecall_handler([]() {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    });
    _set_invalid_parameter_handler([](const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    });
    return true;
}

#endif // _WIN32

// ============================================================
// POSIX 实现（Linux / macOS）
// ============================================================
#ifndef _WIN32

namespace {

// 全局 dump 目录（handler 中无法访问 this）
std::string g_posixDumpDir;

// 原始 sigaction 备份
struct sigaction g_oldSegv;
struct sigaction g_oldAbort;
struct sigaction g_oldFpe;
struct sigaction g_oldIll;
struct sigaction g_oldBus;

// 信号 → 可读名称
const char* signalName(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGABRT: return "SIGABRT";
    case SIGFPE:  return "SIGFPE";
    case SIGILL:  return "SIGILL";
    case SIGBUS:  return "SIGBUS";
    default:      return "UNKNOWN_SIGNAL";
    }
}

// 异步信号安全的字符串长度
size_t safeStrlen(const char* s) {
    size_t n = 0;
    while (s[n]) ++n;
    return n;
}

// 异步信号安全的 write 包装（处理 EINTR）
void safeWrite(int fd, const char* buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        buf += n;
        len -= static_cast<size_t>(n);
    }
}

void safeWriteStr(int fd, const char* s) {
    safeWrite(fd, s, safeStrlen(s));
}

// 崩溃 handler（async-signal-safe）
void crashHandler(int sig, siginfo_t* info, void* ucontext) {
    (void)info;
    (void)ucontext;

    // 测试回调（用于单元测试，避免真的崩溃）
    if (auto cb = g_crashCallback.load(std::memory_order_relaxed)) {
        cb();
        return;
    }

    // 构造文件名：crash-<SIGNAL>-<YYYYMMDD-HHMMSS>.txt
    time_t now = time(nullptr);
    struct tm tmBuf;
    localtime_r(&now, &tmBuf);
    char fname[256];
    snprintf(fname, sizeof(fname), "crash-%s-%04d%02d%02d-%02d%02d%02d.txt",
             signalName(sig),
             tmBuf.tm_year + 1900, tmBuf.tm_mon + 1, tmBuf.tm_mday,
             tmBuf.tm_hour, tmBuf.tm_min, tmBuf.tm_sec);

    // 拼接完整路径（不调用 malloc，使用栈缓冲）
    char fullPath[1024];
    snprintf(fullPath, sizeof(fullPath), "%s/%s",
             g_posixDumpDir.c_str(), fname);

    // 打开文件（O_CREAT | O_WRONLY | O_TRUNC）
    int fd = open(fullPath, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        // 无法写文件，回退到 stderr
        fd = STDERR_FILENO;
    }

    // 写入头部
    safeWriteStr(fd, "=== MiniLang IDE Crash Report ===\n");
    safeWriteStr(fd, "Signal: ");
    safeWriteStr(fd, signalName(sig));
    safeWriteStr(fd, "\n");

    char timeBuf[64];
    snprintf(timeBuf, sizeof(timeBuf),
             "Timestamp: %04d-%02d-%02d %02d:%02d:%02d\n",
             tmBuf.tm_year + 1900, tmBuf.tm_mon + 1, tmBuf.tm_mday,
             tmBuf.tm_hour, tmBuf.tm_min, tmBuf.tm_sec);
    safeWriteStr(fd, timeBuf);

    safeWriteStr(fd, "PID: ");
    char pidBuf[32];
    snprintf(pidBuf, sizeof(pidBuf), "%d\n", static_cast<int>(getpid()));
    safeWriteStr(fd, pidBuf);

    safeWriteStr(fd, "\n=== Stack Trace ===\n");

    // backtrace + backtrace_symbols_fd（async-signal-safe）
    void* frames[128];
    int n = backtrace(frames, 128);
    if (n > 0) {
        backtrace_symbols_fd(frames, n, fd);
    } else {
        safeWriteStr(fd, "(backtrace returned no frames)\n");
    }

    safeWriteStr(fd, "\n=== End of Report ===\n");

    if (fd != STDERR_FILENO) {
        close(fd);
    }

    // 恢复默认 handler 并重新发送信号，让系统生成 core dump
    struct sigaction sa;
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(sig, &sa, nullptr);
    raise(sig);
}

bool registerHandler(int sig, struct sigaction* old) {
    struct sigaction sa;
    sa.sa_sigaction = crashHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    return sigaction(sig, &sa, old) == 0;
}

} // anonymous namespace

bool CrashHandler::installPosix(const std::string& dumpDir) {
    g_posixDumpDir = dumpDir;
    bool ok = true;
    ok &= registerHandler(SIGSEGV, &g_oldSegv);
    ok &= registerHandler(SIGABRT, &g_oldAbort);
    ok &= registerHandler(SIGFPE,  &g_oldFpe);
    ok &= registerHandler(SIGILL,  &g_oldIll);
    ok &= registerHandler(SIGBUS,  &g_oldBus);
    return ok;
}

#endif // POSIX

} // namespace minilang
