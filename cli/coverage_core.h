// ============================================================
// cli/coverage_core.h - minilang-coverage CLI 核心逻辑（可测试）
// ------------------------------------------------------------
// R110 行级覆盖率工具：基于现有 VM 指令行号信息（BytecodeChunk::lines）
// 与 stepCallback 机制扩展为行级覆盖率报告。
//
// 工作流程：
//   source → Lexer::scan → Parser::parse → Compiler::compile → CompileResult
//   → (1) 扫描 mainChunk + functionChunks.lines 收集"可执行行集"
//   → (2) 创建 VM 设置 stepCallback，callback 内读 vm.getCurrentLine()
//        累加到 lineCounts[]
//   → (3) VM::execute 全速运行
//   → (4) 对比可执行行集 vs 已执行行集，生成覆盖率报告
//
// 设计要点：
//   - 核心逻辑与 main 分离便于 gtest 单元测试（仿 minilang-fmt 模式）
//   - 支持 StackVM（默认）与 RegisterVM 两种后端
//   - 支持 text（默认）与 lcov 两种输出格式（lcov 兼容 genhtml/CI 集成）
//   - 可执行行集从 BytecodeChunk::lines 去重得到（与源码行号 1-based 对齐）
//   - 行号 0 表示无位置信息（过滤）
//
// 退出码约定：
//   0  成功（覆盖率 >= 阈值，或未指定阈值）
//   1  覆盖率低于 --fail-under 指定的阈值
//   2  错误（参数解析失败、文件不存在、词法/语法/编译错误等）
// ============================================================
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace minilang_coverage {

/// 覆盖率后端选择
enum class Backend {
    StackVM,    ///< 栈式 VM（默认，复用 BytecodeChunk.lines）
    RegisterVM, ///< 寄存器式 VM（复用 RegBytecodeChunk.lines）
    Both        ///< 同时跑两后端，分别报告（用于交叉验证）
};

/// 输出格式
enum class OutputFormat {
    Text, ///< 人类可读文本（默认）
    Lcov  ///< LCOV .info 格式（CI 集成，lcov/genhtml 兼容）
};

/// 覆盖率选项
struct CoverageOptions {
    Backend backend = Backend::StackVM;
    OutputFormat format = OutputFormat::Text;
    bool showUncovered = true; ///< 高亮未覆盖行
    bool showCounts = false;   ///< 显示每行执行次数（默认仅 covered/uncovered）
    bool showSource = false;   ///< 在 text 输出中显示源码行内容
    bool branch = false;       ///< 拓展二期：分支覆盖率（JUMP_IF_FALSE 双向统计）
    double failUnder = -1.0;   ///< 覆盖率阈值（0-100），低于则返回退出码 1；-1 表示不检查
};

/// 单行覆盖率信息
struct LineCoverage {
    int line = 0;            ///< 1-based 源码行号
    bool executable = false; ///< 是否为可执行行（出现在 chunk.lines 中）
    bool covered = false;    ///< 是否被执行（lineCounts[line] > 0）
    uint64_t count = 0;      ///< 执行次数（covered=true 时 >0）
};

/// 拓展二期：单个分支点覆盖信息（每个 JUMP_IF_FALSE 两个 outcome）
struct BranchCoverage {
    int line = 0;               ///< 1-based 分支所在行（条件表达式行）
    int id = 0;                 ///< 同行多分支时的序号（按 chunk/ip 顺序）
    uint64_t trueCount = 0;     ///< 条件为真（fall-through）执行次数
    uint64_t falseCount = 0;    ///< 条件为假（跳转）执行次数
};

/// 单文件覆盖率报告
struct FileCoverage {
    std::string sourceName;          ///< 文件名或 "<stdin>"
    std::string source;              ///< 源码内容（用于 showSource）
    std::vector<LineCoverage> lines; ///< 按行号排序（1-based 索引：lines[line-1]）
    int totalExecutable = 0;         ///< 可执行行总数
    int totalCovered = 0;            ///< 已覆盖行数
    double ratio = 0.0;              ///< 覆盖率百分比（0-100）
    bool ok = false;                 ///< 是否成功生成报告
    std::string errorMessage;        ///< 错误信息（ok=false 时有效）
    std::string errorPhase;          ///< 错误阶段："lex"/"parse"/"compile"/"run"/"io"
    std::string backendUsed;         ///< "StackVM" / "RegisterVM"
    // 拓展二期：分支覆盖（opts.branch=true 时填充）
    std::vector<BranchCoverage> branches; ///< 按 (line, id) 排序
    int totalBranchOutcomes = 0;          ///< 分支 outcome 总数（每分支点 2）
    int coveredBranchOutcomes = 0;        ///< 至少执行一次的 outcome 数
};

/// 多文件聚合报告
struct CoverageReport {
    std::vector<FileCoverage> files;
    int totalExecutable = 0;
    int totalCovered = 0;
    double ratio = 0.0;
    bool ok = true; ///< 任一文件失败时设为 false
};

/// 分析单个源码字符串的覆盖率
/// 完整管线：source → Lexer → Parser → Compiler → VM stepCallback 收集
/// @param source 源码字符串
/// @param sourceName 源码名（文件名或 "<stdin>"，用于报告标识）
/// @param opts 覆盖率选项
FileCoverage analyzeSource(const std::string& source, const std::string& sourceName, const CoverageOptions& opts);

/// 分析单个文件的覆盖率
/// 内部读取文件 → analyzeSource
FileCoverage analyzeFile(const std::string& path, const CoverageOptions& opts);

/// 分析多个文件，返回聚合报告
CoverageReport analyzeFiles(const std::vector<std::string>& paths, const CoverageOptions& opts);

/// 将单文件覆盖率格式化为文本
std::string formatFileText(const FileCoverage& fc, const CoverageOptions& opts);

/// 将多文件报告格式化为文本
std::string formatReportText(const CoverageReport& report, const CoverageOptions& opts);

/// 将单文件覆盖率格式化为 LCOV .info 片段
/// LCOV 格式参考：https://ltp.sourceforge.net/coverage/lcov/geninfo.1.php
///   SF:<source file path>
///   DA:<line number>,<execution count>
///   LF:<total executable lines>
///   LH:<covered lines>
///   end_of_record
std::string formatFileLcov(const FileCoverage& fc);

/// 将多文件报告格式化为 LCOV .info 完整内容（多个 record 串联）
std::string formatReportLcov(const CoverageReport& report);

/// 命令行参数
struct CliArgs {
    std::vector<std::string> files;
    CoverageOptions options;
    bool showHelp = false;
    bool showVersion = false;
    bool parseError = false;
    std::string errorMessage;
};

/// 解析命令行参数
/// 支持选项：
///   --backend stack|register|both   选择后端（默认 stack）
///   --format text|lcov              输出格式（默认 text）
///   --show-counts                   显示每行执行次数
///   --show-source                   显示源码行内容
///   --no-uncovered                  不高亮未覆盖行（默认高亮）
///   --fail-under <percent>          覆盖率阈值（0-100），低于则返回退出码 1
///   -h, --help                      显示帮助
///   -V, --version                   显示版本
CliArgs parseArgs(int argc, char* argv[]);

/// 打印帮助信息到 stdout
void printHelp();

/// 打印版本信息到 stdout
void printVersion();

/// 返回 minilang-coverage 版本字符串
std::string versionString();

} // namespace minilang_coverage
