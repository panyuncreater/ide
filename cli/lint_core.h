// ============================================================
// cli/lint_core.h - minilang-lint CLI 核心逻辑（可测试）
// ------------------------------------------------------------
// R162 工具链拓展：将 LintPass 抽出为独立 minilang-lint 命令。
// 核心逻辑与 main 分离，便于 gtest 单元测试（TestLintCli.cpp）。
//
// 设计要点：
//   - lintSource: 源码字符串 → LintResult（复用 Lexer → Parser → LintPass 管线）
//   - processFile: 文件路径 → LintProcessResult（读文件 + lint + 格式化输出）
//   - parseArgs: 命令行参数解析（--quiet/--rule/--max-complexity/--format/--help/--version）
//   - printHelp/printVersion: 帮助与版本信息
//
// 退出码约定：
//   0  成功（无警告、无错误）
//   1  有 lint 警告（源码可解析但存在静态问题）
//   2  错误（参数解析失败、文件不存在、词法/语法错误等）
// ============================================================
#pragma once

#include "lint/LintPass.h"

#include <string>
#include <vector>

namespace minilang_lint {

// LintOptions/LintRule/LintResult 定义在 minilang::lint 命名空间中，
// 通过 using 引入到 minilang_lint 命名空间以便简洁引用。
using namespace minilang::lint;

/// 输出格式
enum class OutputFormat {
    Text, ///< 文本格式（默认，人类可读）
    Json  ///< JSON 格式（机器可读，便于 IDE 集成）
};

/// 命令行参数
struct CliArgs {
    std::vector<std::string> files;           ///< 待 lint 文件列表
    LintOptions options;                      ///< Lint 选项（规则开关 + 阈值）
    OutputFormat format = OutputFormat::Text; ///< 输出格式（默认 Text）
    bool quiet = false;                       ///< 仅输出错误，不输出警告
    bool showHelp = false;                    ///< 显示帮助
    bool showVersion = false;                 ///< 显示版本
    bool parseError = false;                  ///< 参数解析失败
    std::string errorMessage;                 ///< 解析错误信息
};

/// 文件处理结果
struct LintProcessResult {
    bool ok = false;          ///< 是否成功（文件存在、词法/语法正确）
    bool hasWarnings = false; ///< 是否有 lint 警告
    int warningCount = 0;     ///< 警告数
    int errorCount = 0;       ///< 错误数（lint 通常 0，仅词法/语法错误计为错误）
    std::string output;       ///< 格式化后的输出（Text 或 JSON）
    std::string errorMessage; ///< 错误信息（ok=false 时有效）
    std::string errorPhase;   ///< 错误阶段（"io"/"lex"/"parse"）
};

/// lint 源码字符串的结果
struct LintSourceResult {
    bool ok = false;          ///< 是否成功（词法/语法正确，lint 完成）
    LintResult lint;          ///< lint 分析结果（ok=true 时有效）
    std::string errorMessage; ///< 错误信息（ok=false 时有效）
    std::string errorPhase;   ///< 错误阶段（"lex"/"parse"）
};

/// lint 源码字符串
/// 完整管线：source → Lexer::scan → Parser::parse → LintPass::analyze
/// 失败时返回 ok=false + errorPhase + errorMessage
LintSourceResult lintSource(const std::string& source, const LintOptions& opts);

/// 处理单个文件
/// 读取文件 → lint → 按 format 格式化输出
LintProcessResult processFile(const std::string& path, const LintOptions& opts, OutputFormat format, bool quiet);

/// 解析命令行参数
/// 支持选项：--quiet / --rule <name> / --max-complexity <N>
///           / --format text|json / --help / --version
CliArgs parseArgs(int argc, char* argv[]);

/// 打印帮助信息到 stdout
void printHelp();

/// 打印版本信息到 stdout
void printVersion();

/// 返回 minilang-lint 版本字符串
std::string versionString();

/// 将规则名（PascalCase）解析为 LintRule 枚举
/// 无效名返回 LintRule::Count
LintRule parseRuleName(const std::string& name);

/// 格式化单条诊断为文本行（如 "foo.ml:3:5: 警告: 未使用的变量 'x' [lint-unused-variable]"）
std::string formatDiagnosticText(const std::string& filePath, const Diagnostic& diag);

/// 格式化 LintResult 为 JSON 输出
std::string formatResultJson(const std::string& filePath, const LintResult& result);

} // namespace minilang_lint
