// ============================================================
// cli/fuzz_core.h - minilang-fuzz CLI 核心逻辑（可测试）
// ------------------------------------------------------------
// R164 工具链拓展：模糊测试器，对 Lexer/Parser/Compiler/VM 做：
//   (1) 生成模式：基于种子的模板化随机程序生成 + 三后端差分
//   (2) 变异模式：对种子语料做字节级变异（发现 Lexer/Parser 边界 bug）
//   (3) 文件模式：对给定文件做三后端差分
//
// 设计要点：
//   - fuzzSource: 源码字符串 → 单后端执行结果
//   - fuzzThreeAgree: 源码 → 三后端差分（Interpreter/StackVM/RegisterVM）
//   - runFuzzBatch: 批量生成 + 执行 + 汇总（发现崩溃/分歧时记录用例）
//   - processFile: 文件输入模式
//   - parseArgs: 命令行参数解析（--seed/--iterations/--backend/--format/...）
//
// 退出码约定：
//   0  成功（无崩溃、无分歧）
//   1  发现崩溃或三后端分歧（已记录用例）
//   2  错误（参数解析失败、文件不存在等）
// ============================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace minilang_fuzz {

/// 模糊测试后端选择
enum class FuzzBackend {
    Interpreter, ///< 树遍历解释器
    StackVM,     ///< 栈式字节码 VM（IR 路径）
    RegisterVM,  ///< 寄存器式 VM
    All          ///< 三后端差分（默认）
};

/// 模糊测试模式
enum class FuzzMode {
    Generate, ///< 生成模式：基于种子的模板化随机程序生成
    Mutate,   ///< 变异模式：对种子语料做字节级变异
    File      ///< 文件模式：对给定文件做差分
};

/// 输出格式
enum class OutputFormat {
    Text, ///< 文本格式（默认，人类可读）
    Json  ///< JSON 格式（机器可读）
};

/// 生成类别（位掩码，可组合）
enum class FuzzCategory : uint32_t {
    Arithmetic = 1u << 0,  ///< 算术（int/float + - * / %）
    ControlFlow = 1u << 1, ///< if/else + while/for
    Function = 1u << 2,    ///< 函数定义/调用/参数
    Recursion = 1u << 3,   ///< 递归（fib/factorial/sum）
    Closure = 1u << 4,     ///< 闭包捕获
    String = 1u << 5,      ///< 字符串拼接 + 插值
    Array = 1u << 6,       ///< 数组 push/index/length
    Class = 1u << 7,       ///< 类字段/方法/init
    ErrorPath = 1u << 8,   ///< 运行时错误（除零/越界/未定义）
    Logic = 1u << 9,       ///< 逻辑短路 + 比较
    All = 0xFFFFFFFFu      ///< 全部类别（默认）
};

/// 模糊测试选项
struct FuzzOptions {
    uint64_t seed = 0;                                                ///< 随机种子（0 = 时间派生）
    int iterations = 100;                                             ///< 生成/变异迭代次数
    FuzzBackend backend = FuzzBackend::All;                           ///< 后端选择
    FuzzMode mode = FuzzMode::Generate;                               ///< 模式
    uint32_t categoryMask = static_cast<uint32_t>(FuzzCategory::All); ///< 生成类别位掩码
    int maxLength = 8192;                                             ///< 最大源码长度（字节）
    bool quiet = false;                                               ///< 仅输出崩溃/分歧
    bool dumpCrashes = true;                                          ///< 发现崩溃时保存用例到 crash-*.ml 文件
};

/// 命令行参数
struct CliArgs {
    std::vector<std::string> files;           ///< 输入文件列表（文件模式）
    FuzzOptions options;                      ///< 模糊测试选项
    OutputFormat format = OutputFormat::Text; ///< 输出格式
    bool showHelp = false;                    ///< 显示帮助
    bool showVersion = false;                 ///< 显示版本
    bool parseError = false;                  ///< 参数解析失败
    std::string errorMessage;                 ///< 解析错误信息
};

/// 单次模糊测试结果
struct FuzzResult {
    bool ok = false;            ///< 是否成功执行（无崩溃）
    std::string source;         ///< 输入源码
    std::string interpOutput;   ///< Interpreter 输出
    std::string stackvmOutput;  ///< StackVM 输出
    std::string regvmOutput;    ///< RegisterVM 输出
    bool crashDetected = false; ///< 是否检测到崩溃（未捕获异常/abort）
    bool disagreement = false;  ///< 三后端输出是否不一致
    std::string errorMessage;   ///< 错误信息
    std::string errorPhase;     ///< 错误阶段："lex"/"parse"/"compile"/"runtime"
    int64_t durationMs = 0;     ///< 执行耗时（毫秒）
};

/// 批量模糊测试汇总
struct FuzzSummary {
    int totalRuns = 0;                         ///< 总执行次数
    int crashes = 0;                           ///< 崩溃次数
    int disagreements = 0;                     ///< 三后端分歧次数
    int agreements = 0;                        ///< 三后端一致次数
    int parseFailures = 0;                     ///< 解析失败次数
    int compileFailures = 0;                   ///< 编译失败次数
    int runtimeErrors = 0;                     ///< 运行时错误次数（三后端一致）
    uint64_t seed = 0;                         ///< 实际使用的种子
    std::vector<FuzzResult> crashCases;        ///< 发现的崩溃用例
    std::vector<FuzzResult> disagreementCases; ///< 发现的分歧用例
    int64_t totalDurationMs = 0;               ///< 总耗时（毫秒）
};

/// 对单段源码执行单后端模糊测试
/// 完整管线：source → Lexer → Parser → Compiler → Backend execute
/// 崩溃（未捕获异常）时 crashDetected=true
FuzzResult fuzzSource(const std::string& source, FuzzBackend backend);

/// 对单段源码做三后端差分模糊测试
/// 三后端输出必须完全一致（运行时错误消息也要一致）
/// disagreement=true 表示三后端输出不一致
FuzzResult fuzzThreeAgree(const std::string& source);

/// 批量生成 + 执行模糊测试
/// 按 opts.mode 选择生成/变异模式，opts.iterations 次迭代
FuzzSummary runFuzzBatch(const FuzzOptions& opts);

/// 文件输入模式：读取文件 → 三后端差分
/// 适合对已知崩溃用例做回归验证
FuzzSummary processFile(const std::string& path, const FuzzOptions& opts);

/// 解析命令行参数
/// 支持选项：--seed <N> / --iterations <N> / --backend <name>
///           / --mode <generate|mutate|file> / --category <name>
///           / --max-length <N> / --format text|json / --quiet
///           / --no-dump / --help / --version
CliArgs parseArgs(int argc, char* argv[]);

/// 打印帮助信息到 stdout
void printHelp();

/// 打印版本信息到 stdout
void printVersion();

/// 返回 minilang-fuzz 版本字符串
std::string versionString();

/// 将后端名解析为 FuzzBackend 枚举
/// 无效名返回 FuzzBackend::All
FuzzBackend parseBackendName(const std::string& name);

/// 将模式名解析为 FuzzMode 枚举
/// 无效名返回 FuzzMode::Generate
FuzzMode parseModeName(const std::string& name);

/// 将类别名解析为 FuzzCategory 位掩码
/// 无效名返回 0
uint32_t parseCategoryName(const std::string& name);

/// 格式化 FuzzSummary 为文本输出
std::string formatSummaryText(const FuzzSummary& summary);

/// 格式化 FuzzSummary 为 JSON 输出
std::string formatSummaryJson(const FuzzSummary& summary);

/// 保存崩溃用例到文件（crash-<index>.ml）
/// 返回保存的文件路径
std::string saveCrashCase(const FuzzResult& result, int index);

} // namespace minilang_fuzz
