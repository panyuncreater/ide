// ============================================================
// cli/compile_core.cpp - minilang-compile CLI 核心逻辑实现
// ------------------------------------------------------------
// P2-11 预编译模块 CLI 工具：将 MiniLang 源码编译为 .minic 预编译模块文件。
// 完整管线：readFile → Lexer::scan → Parser::parse → Compiler::compileModule
//           → BytecodeCache::storeToFile
// ============================================================
#include "cli/compile_core.h"

#include "compiler/BytecodeCache.h"
#include "compiler/Compiler.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace minilang_compile {

namespace fs = std::filesystem;

// ---- 版本信息 ----

std::string versionString() {
    return "minilang-compile 1.0.0 (P2-11 预编译模块)";
}

void printVersion() {
    std::printf("%s\n", versionString().c_str());
}

void printHelp() {
    std::printf("用法: minilang-compile --module <source.mini> [--register] [-o <output.minic>]\n"
                "\n"
                "将 MiniLang 源码文件编译为预编译模块文件 (.minic)。\n"
                "预编译模块可被其他 MiniLang 程序通过 import 快速加载，跳过词法分析和解析阶段。\n"
                "\n"
                "选项:\n"
                "  --module         编译为预编译模块模式（必须指定）\n"
                "  --register       生成 RegisterVM 路径的 MLRC 格式 .minic（默认生成 StackVM 的 MLC 格式）\n"
                "  -o <file>        指定输出 .minic 文件路径（默认：源码路径去扩展名 + .minic）\n"
                "  --help, -h       显示帮助信息\n"
                "  --version, -v    显示版本信息\n"
                "\n"
                "示例:\n"
                "  minilang-compile --module mymod.mini\n"
                "  minilang-compile --module mymod.mini --register\n"
                "  minilang-compile --module mymod.mini -o build/mymod.minic\n"
                "\n"
                "退出码:\n"
                "  0  成功\n"
                "  2  错误（参数解析失败、文件不存在、编译错误、序列化失败等）\n");
}

// ---- 参数解析 ----

CliArgs parseArgs(int argc, char* argv[]) {
    CliArgs args;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            args.showHelp = true;
            return args;
        }
        if (arg == "--version" || arg == "-v") {
            args.showVersion = true;
            return args;
        }
        if (arg == "--module") {
            args.compileAsModule = true;
            continue;
        }
        if (arg == "--register") {
            args.useRegisterVM = true;
            continue;
        }
        if (arg == "-o") {
            if (i + 1 >= argc) {
                args.parseError = true;
                args.errorMessage = "-o 需要指定输出文件路径";
                return args;
            }
            args.outputFile = argv[++i];
            continue;
        }

        // 非选项参数 = 源码文件路径
        if (args.sourceFile.empty()) {
            args.sourceFile = arg;
        } else {
            args.parseError = true;
            args.errorMessage = "多余的参数: " + arg;
            return args;
        }
    }

    // --module 是当前唯一支持的模式
    if (!args.compileAsModule) {
        args.parseError = true;
        args.errorMessage = "必须指定 --module 模式";
        return args;
    }

    if (args.sourceFile.empty()) {
        args.parseError = true;
        args.errorMessage = "未指定源码文件";
        return args;
    }

    return args;
}

// ---- 路径推导 ----

std::string deriveOutputPath(const std::string& sourcePath) {
    fs::path p(sourcePath);
    fs::path output = p.parent_path() / (p.stem().string() + ".minic");
    return output.string();
}

// ---- 读取文件 ----

static bool readFile(const std::string& path, std::string& content) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs)
        return false;
    std::ostringstream oss;
    oss << ifs.rdbuf();
    content = oss.str();
    return true;
}

// ---- 模块编译核心 ----

ModuleResult compileModuleToFile(const std::string& sourcePath, const std::string& outputPath) {
    ModuleResult result;

    // 1. 读取源码文件
    std::string source;
    if (!readFile(sourcePath, source)) {
        result.errorMessage = "无法读取源码文件: " + sourcePath;
        return result;
    }

    // 2. 推导模块名（使用文件名去扩展名）
    fs::path p(sourcePath);
    std::string moduleName = p.stem().string();

    // 3. 编译为模块
    Compiler compiler;
    CompileResult compileResult;
    try {
        compileResult = compiler.compileModule(source, moduleName);
    } catch (const std::exception& e) {
        result.errorMessage = std::string("编译异常: ") + e.what();
        return result;
    }

    if (compiler.getDiagnostics().hasErrors()) {
        result.errorMessage = "编译错误: " + compiler.getLastError();
        return result;
    }

    // 4. 序列化到 .minic 文件（L12: 传入 sourcePath 嵌入源码 mtime+hash，
    //    供 tryLoadFromFile 校验源码是否被修改，自动失效过时 .minic）
    std::string actualOutput = outputPath.empty() ? deriveOutputPath(sourcePath) : outputPath;

    BytecodeCache cache;
    if (!cache.storeToFile(actualOutput, compileResult, moduleName, sourcePath)) {
        result.errorMessage = "序列化失败: " + actualOutput;
        return result;
    }

    // 5. 填充结果
    result.ok = true;
    result.outputFile = actualOutput;
    result.functionCount = static_cast<int>(compileResult.functionChunks.size());
    result.exportCount = static_cast<int>(compileResult.moduleExports.size());
    return result;
}

// ============================================================
// L11 预编译模块（RegisterVM）：生成 MLRC 格式 .minic
// ============================================================
ModuleResult compileModuleToFileRegister(const std::string& sourcePath, const std::string& outputPath) {
    ModuleResult result;

    // 1. 读取源码文件
    std::string source;
    if (!readFile(sourcePath, source)) {
        result.errorMessage = "无法读取源码文件: " + sourcePath;
        return result;
    }

    // 2. 推导模块名（使用文件名去扩展名）
    fs::path p(sourcePath);
    std::string moduleName = p.stem().string();

    // 3. 编译为 RegisterVM 模块
    Compiler compiler;
    compiler.setUseRegisterVM(true);
    RegisterCompileResult compileResult;
    try {
        compileResult = compiler.compileModuleViaRegisterIR(source, moduleName);
    } catch (const std::exception& e) {
        result.errorMessage = std::string("编译异常: ") + e.what();
        return result;
    }

    if (compiler.getDiagnostics().hasErrors()) {
        result.errorMessage = "编译错误: " + compiler.getLastError();
        return result;
    }

    // 4. 序列化到 .minic 文件（MLRC 格式，嵌入源码 mtime+hash 供加载校验）
    std::string actualOutput = outputPath.empty() ? deriveOutputPath(sourcePath) : outputPath;

    BytecodeCache cache;
    if (!cache.storeRegisterToFile(actualOutput, compileResult, moduleName, sourcePath)) {
        result.errorMessage = "序列化失败: " + actualOutput;
        return result;
    }

    // 5. 填充结果
    result.ok = true;
    result.outputFile = actualOutput;
    result.functionCount = static_cast<int>(compileResult.functionChunks.size());
    result.exportCount = static_cast<int>(compileResult.moduleExports.size());
    return result;
}

} // namespace minilang_compile
