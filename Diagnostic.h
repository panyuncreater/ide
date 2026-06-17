#pragma once

#include <string>
#include <vector>
#include <algorithm>

// ============================================================
// Diagnostic 统一诊断体系
// ============================================================
// 设计目标：为 Lexer、Parser、Compiler、Interpreter、VM 提供
// 统一的错误/警告/信息报告格式，便于 IDE 集中展示和处理。

/// 诊断严重级别
enum class DiagLevel {
    Error,       // 致命错误，阻止执行
    Warning,     // 警告，不阻止执行
    Info,        // 提示信息
    Hint         // 改进建议
};

/// 诊断来源模块
enum class DiagSource {
    Lexer,
    Parser,
    Compiler,
    Interpreter,
    VM,
    Formatter,
    IDE
};

/// 单条诊断信息
struct Diagnostic {
    DiagLevel level;
    std::string message;
    int line;
    int column;
    DiagSource source;

    Diagnostic(DiagLevel lv, const std::string& msg, int ln, int col, DiagSource src)
        : level(lv), message(msg), line(ln), column(col), source(src) {}

    /// 是否为错误级别
    bool isError() const { return level == DiagLevel::Error; }

    /// 是否为警告级别
    bool isWarning() const { return level == DiagLevel::Warning; }

    /// 获取严重级别的文本标签
    std::string levelString() const {
        switch (level) {
        case DiagLevel::Error:   return "错误";
        case DiagLevel::Warning: return "警告";
        case DiagLevel::Info:    return "信息";
        case DiagLevel::Hint:    return "建议";
        }
        return "未知";
    }

    /// 获取来源模块的文本标签
    std::string sourceString() const {
        switch (source) {
        case DiagSource::Lexer:       return "词法分析";
        case DiagSource::Parser:      return "语法分析";
        case DiagSource::Compiler:    return "编译器";
        case DiagSource::Interpreter: return "解释器";
        case DiagSource::VM:          return "虚拟机";
        case DiagSource::Formatter:   return "格式化器";
        case DiagSource::IDE:         return "IDE";
        }
        return "未知";
    }

    /// 格式化为 IDE 输出面板显示文本
    /// 格式：[来源] 级别 (行 X, 列 Y): 消息
    std::string format() const {
        std::string result = "[" + sourceString() + "] " + levelString();
        if (line > 0) {
            result += " (行 " + std::to_string(line);
            if (column > 0) {
                result += ", 列 " + std::to_string(column);
            }
            result += ")";
        }
        result += ": " + message;
        return result;
    }
};

/// 诊断收集器：聚合来自多个模块的诊断信息
class DiagnosticBag {
public:
    /// 添加一条诊断
    void add(const Diagnostic& diag) {
        diagnostics_.push_back(diag);
    }

    /// 便捷方法：添加错误
    void addError(const std::string& msg, int line, int col, DiagSource src) {
        diagnostics_.emplace_back(DiagLevel::Error, msg, line, col, src);
    }

    /// 便捷方法：添加警告
    void addWarning(const std::string& msg, int line, int col, DiagSource src) {
        diagnostics_.emplace_back(DiagLevel::Warning, msg, line, col, src);
    }

    /// 便捷方法：添加信息
    void addInfo(const std::string& msg, int line, int col, DiagSource src) {
        diagnostics_.emplace_back(DiagLevel::Info, msg, line, col, src);
    }

    /// 是否有错误
    bool hasErrors() const {
        return std::any_of(diagnostics_.begin(), diagnostics_.end(),
                          [](const Diagnostic& d) { return d.isError(); });
    }

    /// 是否有警告
    bool hasWarnings() const {
        return std::any_of(diagnostics_.begin(), diagnostics_.end(),
                          [](const Diagnostic& d) { return d.isWarning(); });
    }

    /// 错误数量
    int errorCount() const {
        return static_cast<int>(std::count_if(diagnostics_.begin(), diagnostics_.end(),
                                              [](const Diagnostic& d) { return d.isError(); }));
    }

    /// 警告数量
    int warningCount() const {
        return static_cast<int>(std::count_if(diagnostics_.begin(), diagnostics_.end(),
                                              [](const Diagnostic& d) { return d.isWarning(); }));
    }

    /// 获取所有诊断
    const std::vector<Diagnostic>& all() const { return diagnostics_; }

    /// 获取错误行号集合（用于编辑器标记）
    std::vector<int> errorLines() const {
        std::vector<int> lines;
        for (const auto& d : diagnostics_) {
            if (d.isError() && d.line > 0) {
                lines.push_back(d.line);
            }
        }
        std::sort(lines.begin(), lines.end());
        lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
        return lines;
    }

    /// 清空所有诊断
    void clear() { diagnostics_.clear(); }

    /// 诊断数量
    size_t size() const { return diagnostics_.size(); }

    /// 是否为空
    bool empty() const { return diagnostics_.empty(); }

    /// 按行号排序（同文件内的诊断按位置排列）
    void sortByLocation() {
        std::stable_sort(diagnostics_.begin(), diagnostics_.end(),
            [](const Diagnostic& a, const Diagnostic& b) {
                if (a.line != b.line) return a.line < b.line;
                return a.column < b.column;
            });
    }

    /// 获取摘要文本（如 "3 个错误, 1 个警告"）
    std::string summary() const {
        int errs = errorCount();
        int warns = warningCount();
        std::string result;
        if (errs > 0) result += std::to_string(errs) + " 个错误";
        if (warns > 0) {
            if (!result.empty()) result += ", ";
            result += std::to_string(warns) + " 个警告";
        }
        if (result.empty()) result = "无错误";
        return result;
    }

private:
    std::vector<Diagnostic> diagnostics_;
};
