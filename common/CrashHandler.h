/**
 * @file common/CrashHandler.h
 * @brief 跨平台崩溃报告系统（R128）。
 *
 * 设计目标：
 *   - 捕获原生崩溃（segfault / access violation / abort / 浮点异常等）
 *   - 写入崩溃 minidump（Windows）或 backtrace 文本（Linux/macOS）到磁盘
 *   - 与 Logger 互补：Logger 仅捕获同步 throw 异常，CrashHandler 捕获
 *     信号级与硬件级异常
 *
 * 跨平台策略：
 *   - Windows：SetUnhandledExceptionFilter + MiniDumpWriteDump（dbghelp.lib）
 *     输出 .dmp 二进制 minidump
 *   - Linux/macOS：sigaction 注册 SIGSEGV/SIGABRT/SIGFPE/SIGILL/SIGBUS
 *     handler，使用 backtrace(3) + backtrace_symbols(3) 输出文本栈回溯
 *
 * 信号安全约束：
 *   - POSIX 信号 handler 中只能调用异步信号安全函数（man 7 signal-safety）
 *   - 禁用：malloc / printf / std::string / std::ostringstream / Qt API
 *   - 允许：write / open / close / snprintf / backtrace / backtrace_symbols_fd
 *
 * 与 GUI 的协作：
 *   - 崩溃 handler 写入文件后立即终止进程（不能再调用 Qt API）
 *   - 下次启动时由 main.cpp 调用 CrashHandler::lastCrashReport() 检查
 *   - 若存在崩溃报告，弹出 CrashReportDialog 让用户查看/导出/上报
 *
 * @see CrashReportDialog
 */
#pragma once

#include <ctime>
#include <string>

namespace minilang {

/// 崩溃报告元信息（由 lastCrashReport() 返回）
struct CrashReport {
    bool        valid = false;          ///< 是否存在有效崩溃报告
    std::string timestamp;              ///< 崩溃时间（YYYY-MM-DD HH:MM:SS）
    std::string signalName;             ///< 信号/异常名（SIGSEGV / EXCEPTION_ACCESS_VIOLATION 等）
    std::string dumpFilePath;           ///< minidump 或 backtrace 文件路径
    std::string logFilePath;            ///< 崩溃时的应用日志路径（如有）
    std::string stackTrace;             ///< 文本栈回溯（仅 POSIX；Windows 留空，需工具解析 .dmp）
};

/// 跨平台崩溃处理器
class CrashHandler {
public:
    /// 获取单例
    static CrashHandler& instance();

    /// 安装崩溃处理器（在 main 入口最早调用，确保 QApplication 之前）
    /// @param dumpDir 崩溃报告输出目录（默认为 <temp>/minilang-crashdumps/）
    /// @return 是否安装成功
    bool install(const std::string& dumpDir = "");

    /// 卸载崩溃处理器（一般不调用，进程退出时自动失效）
    void uninstall();

    /// 获取上一次崩溃报告（启动时调用，若 valid=true 则弹对话框）
    /// @note 不会自动删除报告文件，调用 consumeLastCrashReport() 才会删除
    static CrashReport lastCrashReport();

    /// 获取并删除上次崩溃报告（用户已处理后调用）
    /// @return 删除前的崩溃报告
    static CrashReport consumeLastCrashReport();

    /// 删除所有过期的崩溃报告（默认保留 7 天内的）
    /// @param maxAgeDays 最大保留天数
    /// @return 删除的文件数
    static int cleanupOldReports(int maxAgeDays = 7);

    /// 写入崩溃元信息文件（.crashmeta）— handler 内部调用
    /// @note Windows handler 可使用 std::ofstream；POSIX 由下次启动构造
    /// @note public 以便匿名命名空间中的 handler 函数访问
    static void writeCrashMeta(const std::string& dumpDir,
                                const std::string& dumpFile,
                                const std::string& signalName);

    /// 设置崩溃回调（用于测试注入；handler 中调用，必须异步信号安全）
    /// @note 实际产品中不应使用，仅单元测试用
    void setCrashCallback(void (*cb)());

    /// 获取当前 dump 目录
    const std::string& dumpDirectory() const { return dumpDir_; }

private:
    CrashHandler() = default;
    ~CrashHandler() = default;
    CrashHandler(const CrashHandler&) = delete;
    CrashHandler& operator=(const CrashHandler&) = delete;

    std::string dumpDir_;
    bool        installed_ = false;
    void        (*crashCallback_)() = nullptr;

    // 平台特定实现（在 .cpp 中按平台 #ifdef 分发）
    bool installPlatform(const std::string& dumpDir);
    void uninstallPlatform();

#ifdef _WIN32
    bool installWindows(const std::string& dumpDir);
#else
    bool installPosix(const std::string& dumpDir);
#endif
};

} // namespace minilang
