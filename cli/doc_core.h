// ============================================================
// cli/doc_core.h - minilang-doc CLI 核心逻辑（可测试）
// ------------------------------------------------------------
// R162 B2 工具链拓展：将 DocGenerator 抽出为独立 minilang-doc 命令。
// 核心逻辑与 main 分离，便于 gtest 单元测试（TestDocCli.cpp）。
//
// 设计要点：
//   - generateDoc: 源码字符串 → DocResult（复用 Lexer → Parser → DocGenerator 管线）
//   - processFile: 文件路径 → DocProcessResult（读文件 + 生成文档 + 输出）
//   - parseArgs: 命令行参数解析（--format/--output/--help/--version）
//   - printHelp/printVersion: 帮助与版本信息
//
// 退出码约定：
//   0  成功（文档生成完成）
//   2  错误（参数解析失败、文件不存在、词法/语法错误等）
// ============================================================
#pragma once

#include "doc/DocGenerator.h"

#include <string>
#include <vector>

namespace minilang_doc {

/// 命令行参数
struct CliArgs {
    std::vector<std::string> files;                                       ///< 输入源码文件列表
    minilang::doc::DocFormat format = minilang::doc::DocFormat::Markdown; ///< 输出格式（默认 Markdown）
    std::string outputFile;                                               ///< 输出文件路径（空 = stdout）
    bool showHelp = false;                                                ///< 显示帮助
    bool showVersion = false;                                             ///< 显示版本
    bool parseError = false;                                              ///< 参数解析失败
    std::string errorMessage;                                             ///< 解析错误信息
};

/// 生成文档的结果
struct DocSourceResult {
    bool ok = false;              ///< 是否成功（词法/语法正确，文档生成完成）
    minilang::doc::DocResult doc; ///< 文档生成结果（ok=true 时有效）
    std::string errorMessage;     ///< 错误信息（ok=false 时有效）
    std::string errorPhase;       ///< 错误阶段（"lex"/"parse"）
};

/// 文件处理结果
struct DocProcessResult {
    bool ok = false;          ///< 是否成功
    std::string output;       ///< 生成的文档内容
    int entryCount = 0;       ///< 条目数量
    std::string errorMessage; ///< 错误信息（ok=false 时有效）
    std::string errorPhase;   ///< 错误阶段（"io"/"lex"/"parse"）
};

/// 生成文档
/// 完整管线：source → Lexer::scan → Parser::parse → DocGenerator::generate
DocSourceResult generateDoc(const std::string& source, minilang::doc::DocFormat format);

/// 处理单个文件
/// 读取文件 → 生成文档 → 按 format 输出
DocProcessResult processFile(const std::string& path, minilang::doc::DocFormat format);

/// 解析命令行参数
/// 支持选项：--format markdown|html|json / --output <file> / --help / --version
CliArgs parseArgs(int argc, char* argv[]);

/// 打印帮助信息到 stdout
void printHelp();

/// 打印版本信息到 stdout
void printVersion();

/// 返回 minilang-doc 版本字符串
std::string versionString();

/// 将格式名解析为 DocFormat 枚举
/// 无效名返回 DocFormat::Markdown（默认）
minilang::doc::DocFormat parseFormatName(const std::string& name);

} // namespace minilang_doc
