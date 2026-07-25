// ============================================================
// cli/compile_core.h - minilang-compile CLI 核心逻辑（可测试）
// ------------------------------------------------------------
// P2-11 预编译模块 CLI 工具：将 MiniLang 源码编译为 .minic 预编译模块文件。
// 核心逻辑与 main 分离，便于 gtest 单元测试。
//
// 用法：
//   minilang-compile --module <source.mini> [-o <output.minic>]
//
// 退出码：
//   0  成功
//   2  错误（参数解析失败、文件不存在、编译错误、序列化失败等）
// ============================================================
#pragma once

#include <string>
#include <vector>

namespace minilang_compile {

/// 命令行参数
struct CliArgs {
    std::string sourceFile;       ///< 源码文件路径
    std::string outputFile;       ///< 输出 .minic 路径（空 = 自动推导：source 去扩展名 + .minic）
    bool compileAsModule = false; ///< --module：编译为预编译模块
    bool useRegisterVM = false;   ///< --register：生成 RegisterVM 路径的 MLRC 格式 .minic
    bool showHelp = false;        ///< 显示帮助
    bool showVersion = false;     ///< 显示版本
    bool parseError = false;      ///< 参数解析失败
    std::string errorMessage;     ///< 解析错误信息
};

/// 模块编译结果
struct ModuleResult {
    bool ok = false;          ///< 是否成功
    std::string outputFile;   ///< 实际输出的 .minic 路径
    std::string errorMessage; ///< 错误信息
    int functionCount = 0;    ///< 编译生成的函数 chunk 数量
    int exportCount = 0;      ///< 导出名称数量
};

/// 解析命令行参数
/// 支持选项：--module / -o <output> / --help / --version
CliArgs parseArgs(int argc, char* argv[]);

/// 将源码文件编译为预编译模块 (.minic)
/// 完整管线：readFile → Compiler::compileModule → BytecodeCache::storeToFile
ModuleResult compileModuleToFile(const std::string& sourcePath, const std::string& outputPath);

/// L11: 将源码文件编译为 RegisterVM 路径的预编译模块 (.minic, MLRC 格式)
/// 完整管线：readFile → Compiler::compileModuleViaRegisterIR → BytecodeCache::storeRegisterToFile
ModuleResult compileModuleToFileRegister(const std::string& sourcePath, const std::string& outputPath);

/// 从源码路径推导默认 .minic 输出路径（去扩展名 + .minic）
std::string deriveOutputPath(const std::string& sourcePath);

/// 打印帮助信息到 stdout
void printHelp();

/// 打印版本信息到 stdout
void printVersion();

/// 返回 minilang-compile 版本字符串
std::string versionString();

} // namespace minilang_compile
