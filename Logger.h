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
    bool setOutputFile(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fileStream_.is_open()) fileStream_.close();
        if (path.empty()) return true;
        fileStream_.open(path, std::ios::app);
        return fileStream_.is_open();
    }

    /// 输出一条日志
    void log(LogLevel level, const std::string& message, const std::string& source = "") {
        if (level < level_.load() || level == LogLevel::NONE) return;
        std::lock_guard<std::mutex> lock(mutex_);
        std::string line = formatLine(level, message, source);
        if (consoleOutput_.load()) {
            if (level >= LogLevel::WARNING) std::cerr << line << '\n';
            else std::cout << line << '\n';
        }
        if (fileStream_.is_open()) {
            fileStream_ << line << '\n';
            // P2 fix: 仅 ERROR 级别 flush，避免高频日志每行 flush 导致性能下降
            if (level >= LogLevel::ERROR) fileStream_.flush();
        }
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

    std::mutex mutex_;
    std::atomic<LogLevel> level_{LogLevel::WARNING};
    std::atomic<bool> consoleOutput_{true};  // P1 fix: atomic 防止数据竞争
    std::ofstream fileStream_;

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
