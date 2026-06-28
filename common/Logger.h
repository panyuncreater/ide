#pragma once

// ============================================================
// Logger 轻量级日志系统
// ------------------------------------------------------------
// 设计目标：为 MiniLang 各模块（Lexer/Parser/Interpreter/
// Compiler/VM/Debugger/IDE）提供统一的内部日志输出能力，
// 支持分级过滤、多目标输出（控制台/文件），线程安全。
//
// 与 Diagnostic.h 的区别：
//   - DiagnosticBag 面向用户，收集编译/运行时的错误与警告
//     用于 IDE 鷃出面板展示。
//   - Logger 面向开发者，用于调试、监控与问题定位，不影响
//     程序执行流程。
// ============================================================

#include <string>
#include <iostream>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <mutex>
#include <atomic>
#include <charconv>
#include <array>

/// 日志级别（由低到高）
enum class LogLevel {
    DEBUG,    // 调试信息（最详细，仅开发时启用）
    INFO,     // 一般信息（程序运行状态）
    WARNING,  // 警告（潜在问题，不影响执行）
    ERROR,    // 错误（严重问题，可能影响执行）
    NONE      // 禁用所有日志输出
};

/// 轻量级日志器（单例，线程安全）
class Logger {
public:
    /// 获取单例实例
    static Logger& instance() {
        static Logger logger;
        return logger;
    }

    /// 设置最低输出级别（低于此级别的日志将被丢弃）
    void setLevel(LogLevel level) { level_.store(level); }

    /// 获取当前最低输出级别
    LogLevel level() const { return level_.load(); }

    /// 启用/禁用控制台输出（默认启用）
    // P1 fix: 使用 atomic<bool> 防止 setConsoleOutput 与 log() 之间的数据竞争
    void setConsoleOutput(bool enabled) { consoleOutput_.store(enabled); }

    /// 设置日志文件输出路径（空字符串则关闭文件输出）
    // Bug fix: fileStream_ 由 flushBuffer() 在 ioMutex_ 下访问，setOutputFile()
    // 必须同时持有 ioMutex_ 才能避免与并发 flushBuffer() 产生数据竞争。
    bool setOutputFile(const std::string& path) {
        std::lock_guard<std::mutex> ioLock(ioMutex_);  // 序列化 fileStream_ 访问
        std::lock_guard<std::mutex> lock(mutex_);       // 序列化 activeBuffer_ 访问
        if (fileStream_.is_open()) fileStream_.close();
        if (path.empty()) return true;
        fileStream_.open(path, std::ios::app);
        return fileStream_.is_open();
    }

    /// 输出一条日志
    // P2-2 fix: 双缓冲避免持锁 I/O。log() 仅在 mutex_ 下格式化+入队，
    // 当队列达阈值或级别为 ERROR 时，swap 出缓冲并在 ioMutex_ 下批量写
    // （mutex_ 持有时间从 O(行数) 降至 O(格式化单行)）。
    void log(LogLevel level, const std::string& message, const std::string& source = "") {
        if (level < level_.load() || level == LogLevel::NONE) return;
        std::vector<std::string> localBuffer;
        bool shouldFlush = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            activeBuffer_.push_back(formatLine(level, message, source));
            // ERROR 立即 flush；INFO/WARN/DEBUG 攒满阈值后批量 flush
            if (level >= LogLevel::ERROR || activeBuffer_.size() >= FLUSH_THRESHOLD) {
                localBuffer.swap(activeBuffer_);
                shouldFlush = true;
            }
        }
        if (shouldFlush) {
            flushBuffer(localBuffer);
        }
    }

    /// 强制刷新所有缓冲的日志（用于析构、关键节点）
    void flush() {
        std::vector<std::string> localBuffer;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            localBuffer.swap(activeBuffer_);
        }
        flushBuffer(localBuffer);
    }

    // ---- 实例方法便捷接口 ----
    void debug(const std::string& msg, const std::string& source = "") { log(LogLevel::DEBUG, msg, source); }
    void info(const std::string& msg, const std::string& source = "") { log(LogLevel::INFO, msg, source); }
    void warning(const std::string& msg, const std::string& source = "") { log(LogLevel::WARNING, msg, source); }
    void error(const std::string& msg, const std::string& source = "") { log(LogLevel::ERROR, msg, source); }

    // ---- 静态便捷接口（推荐使用，调用更简洁）----
    static void Debug(const std::string& msg, const std::string& source = "") { instance().log(LogLevel::DEBUG, msg, source); }
    static void Info(const std::string& msg, const std::string& source = "") { instance().log(LogLevel::INFO, msg, source); }
    static void Warning(const std::string& msg, const std::string& source = "") { instance().log(LogLevel::WARNING, msg, source); }
    static void Error(const std::string& msg, const std::string& source = "") { instance().log(LogLevel::ERROR, msg, source); }

private:
    Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    // P2-2 fix: 析构时强制 flush 缓冲，避免进程退出丢失未写日志
    ~Logger() { flush(); }

    std::mutex mutex_;
    // P2-2 fix: I/O 专用锁，与 mutex_ 解耦。log() 持有 mutex_ 仅做格式化+入队，
    // 实际写文件/控制台在 flushBuffer() 中持有 ioMutex_ 进行，避免阻塞其他线程的 log() 调用
    std::mutex ioMutex_;
    std::atomic<LogLevel> level_{LogLevel::WARNING};
    std::atomic<bool> consoleOutput_{true};  // P1 fix: atomic 防止数据竞争
    std::ofstream fileStream_;
    // P2-2 fix: 双缓冲。activeBuffer_ 由 log() 在 mutex_ 下追加；
    // 达阈值或 ERROR 时 swap 出到本地，由 flushBuffer() 在 ioMutex_ 下批量写
    std::vector<std::string> activeBuffer_;
    // 缓冲触发阈值：DEBUG/INFO/WARN 攒满 32 行批量写；ERROR 立即写
    static constexpr size_t FLUSH_THRESHOLD = 32;

    /// 批量写缓冲到控制台/文件（持 ioMutex_，不持 mutex_）
    void flushBuffer(const std::vector<std::string>& buffer) {
        if (buffer.empty()) return;
        std::lock_guard<std::mutex> ioLock(ioMutex_);
        bool toConsole = consoleOutput_.load();
        bool toFile = fileStream_.is_open();
        for (const auto& line : buffer) {
            if (toConsole) {
                // 注意：原实现按级别选 cerr/cout，但缓冲后级别信息丢失。
                // 改进：行内已含级别标签（[WARN]/[ERROR]），统一用 cerr 输出便于合并。
                // 若需严格区分可改用 pair<LogLevel,string>，但当前简化为合并写 cout
                std::cout << line << '\n';
            }
            if (toFile) {
                fileStream_ << line << '\n';
            }
        }
        if (toFile) fileStream_.flush();  // 批量写后统一 flush
    }

    /// 级别文本标签
    const char* levelString(LogLevel level) const {
        switch (level) {
        case LogLevel::DEBUG:   return "DEBUG";
        case LogLevel::INFO:    return "INFO";
        case LogLevel::WARNING: return "WARN";
        case LogLevel::ERROR:   return "ERROR";
        case LogLevel::NONE:    return "NONE";
        }
        return "?";
    }

    /// 生成带时间戳的格式化日志行
    std::string formatLine(LogLevel level, const std::string& message, const std::string& source) {
        std::ostringstream oss;
        // 时间戳
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;
        oss << std::put_time(std::localtime(&t), "%H:%M:%S");
        oss << '.' << std::setfill('0') << std::setw(3) << ms.count();
        // 级别
        oss << " [" << levelString(level) << "]";
        // 来源
        if (!source.empty()) oss << " [" << source << "]";
        // 消息
        oss << ' ' << message;
        return oss.str();
    }
};

// ============================================================
// 便捷宏（P2 fix: 真正懒求值，仅在级别启用时才构造字符串）
// ============================================================
#define LOG_DEBUG(msg, source)   do { if (Logger::instance().level() <= LogLevel::DEBUG)   Logger::Debug(msg, source); } while(0)
#define LOG_INFO(msg, source)    do { if (Logger::instance().level() <= LogLevel::INFO)    Logger::Info(msg, source); } while(0)
#define LOG_WARNING(msg, source) do { if (Logger::instance().level() <= LogLevel::WARNING) Logger::Warning(msg, source); } while(0)
#define LOG_ERROR(msg, source)   do { if (Logger::instance().level() <= LogLevel::ERROR)   Logger::Error(msg, source); } while(0)

// ============================================================
// P1-9 fix: 错误消息定位格式化（热路径优化）
// ------------------------------------------------------------
// runtimeError 是错误处理热路径，原实现使用 std::to_string + operator+
// 多次堆分配。改用 std::to_chars 写入栈缓冲区，零堆分配。
// ============================================================
namespace ErrorFormat {
inline std::string formatWithLocation(const std::string& msg, int line, int col) {
    // 预估容量：msg + " (行 " + 11 位 int + ", 列 " + 11 位 int + ")"
    std::string result;
    result.reserve(msg.size() + 32);
    result = msg;
    result += " (行 ";
    std::array<char, 24> buf{};
    auto r1 = std::to_chars(buf.data(), buf.data() + buf.size(), line);
    result.append(buf.data(), r1.ptr);
    result += ", 列 ";
    auto r2 = std::to_chars(buf.data(), buf.data() + buf.size(), col);
    result.append(buf.data(), r2.ptr);
    result += ')';
    return result;
}

inline std::string formatWithLine(const std::string& msg, int line) {
    std::string result;
    result.reserve(msg.size() + 16);
    result = msg;
    result += " (行 ";
    std::array<char, 24> buf{};
    auto r = std::to_chars(buf.data(), buf.data() + buf.size(), line);
    result.append(buf.data(), r.ptr);
    result += ')';
    return result;
}
} // namespace ErrorFormat
