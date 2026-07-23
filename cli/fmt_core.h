// ============================================================
// cli/fmt_core.h - minilang-fmt CLI 核心逻辑（可测试）
// ------------------------------------------------------------
// R109 代码格式化 CLI 工具：将 Formatter 抽出为独立 minilang-fmt 命令。
// 核心逻辑与 main 分离，便于 gtest 单元测试（TestFmtCli.cpp）。
//
// 设计要点：
//   - formatSource: 源码字符串 → 格式化后字符串（复用 Lexer → Parser → Formatter 管线）
//   - processFile:  文件路径 → 读/写/检查（三种模式）
//   - parseArgs:    命令行参数解析
//   - printHelp/printVersion: 帮助与版本信息
//
// 退出码约定：
//   0  成功（--check 模式下：所有文件已格式化）
//   1  --check 模式下：至少一个文件需要格式化
//   2  错误（参数解析失败、文件不存在、词法/语法错误等）
// ============================================================
#pragma once

#include "formatter/Formatter.h"
#include <string>
#include <vector>

namespace minilang_fmt {

/// 格式化源码的结果
struct FormatResult {
    bool ok = false;          ///< 是否成功
    std::string output;       ///< 格式化后源码（ok=true 时有效）
    std::string errorMessage; ///< 错误信息（ok=false 时有效）
    std::string errorPhase;   ///< 错误阶段（"lex"/"parse"/"format"）
};

/// 文件处理模式
enum class ProcessMode {
    Check, ///< 检查模式：仅报告是否需要格式化，不修改文件
    Write, ///< 写入模式：原地格式化文件（默认）
    DryRun ///< 试运行：输出格式化结果到 stdout 但不修改文件
};

/// 文件处理结果
struct ProcessResult {
    bool ok = false;          ///< 是否成功（文件存在、词法/语法正确）
    bool needsFormat = false; ///< 是否需要格式化（Check 模式有效）
    std::string output;       ///< DryRun 模式输出格式化后源码；其他模式为空
    std::string errorMessage; ///< 错误信息
    std::string errorPhase;   ///< 错误阶段
};

/// 命令行参数
struct CliArgs {
    std::vector<std::string> files;        ///< 待格式化文件列表
    ProcessMode mode = ProcessMode::Write; ///< 默认写入模式
    FormatOptions options;                 ///< 格式化选项（默认 K&R + 4 空格）
    bool showHelp = false;                 ///< 显示帮助
    bool showVersion = false;              ///< 显示版本
    bool parseError = false;               ///< 参数解析失败
    std::string errorMessage;              ///< 解析错误信息
};

/// 格式化源码字符串
/// 完整管线：source → Lexer::scan → Parser::parse → Formatter::format
/// 失败时返回 ok=false + errorPhase + errorMessage
FormatResult formatSource(const std::string& source, const FormatOptions& opts);

/// 处理单个文件
/// 根据模式执行：Check（比较是否需要格式化）/ Write（写回）/ DryRun（输出到 stdout）
ProcessResult processFile(const std::string& path, ProcessMode mode, const FormatOptions& opts);

/// 解析命令行参数
/// 支持选项：--check / --write / --dry-run / --indent N / --style kr|allman|compact|tabbed
///           / --tabs / --help / --version
CliArgs parseArgs(int argc, char* argv[]);

/// 打印帮助信息到 stdout
void printHelp();

/// 打印版本信息到 stdout
void printVersion();

/// 返回 minilang-fmt 版本字符串
std::string versionString();

} // namespace minilang_fmt
