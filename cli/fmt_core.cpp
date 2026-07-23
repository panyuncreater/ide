// ============================================================
// cli/fmt_core.cpp - minilang-fmt CLI 核心逻辑实现
// ------------------------------------------------------------
// 复用 test_harness/formatter_audit.cpp 的 formatCode 模式：
//   source → Lexer::scan → Parser::parse → Formatter::format
// 注释必须从 lexer.comments() 单独传入 formatter.setComments()。
// ============================================================
#include "cli/fmt_core.h"

#include "common/Diagnostic.h"
#include "formatter/Formatter.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace minilang_fmt {

// ---- 版本信息 ----
std::string versionString() {
    return "minilang-fmt 1.0.0 (R109, 2026-07-19)";
}

// ---- formatSource: 源码 → 格式化后字符串 ----
FormatResult formatSource(const std::string& source, const FormatOptions& opts) {
    FormatResult result;

    // 1. Lexer
    Lexer lexer;
    auto tokens = lexer.scan(source);
    if (lexer.getDiagnostics().hasErrors()) {
        result.ok = false;
        result.errorPhase = "lex";
        result.errorMessage = lexer.getDiagnostics().summary();
        return result;
    }

    // 2. Parser
    Parser parser;
    auto ast = parser.parse(tokens);
    if (parser.hasErrors() || !ast) {
        result.ok = false;
        result.errorPhase = "parse";
        result.errorMessage = parser.getDiagnostics().summary();
        return result;
    }

    // 3. Formatter
    Formatter fmt;
    fmt.setComments(lexer.comments()); // F1 fix: 注释保留必需
    fmt.setOptions(opts);
    try {
        result.output = fmt.format(*ast);
        result.ok = true;
    } catch (const std::exception& e) {
        result.ok = false;
        result.errorPhase = "format";
        result.errorMessage = e.what();
    }
    return result;
}

// ---- processFile: 文件路径 → 处理结果 ----
ProcessResult processFile(const std::string& path, ProcessMode mode, const FormatOptions& opts) {
    ProcessResult result;

    // 1. 读取文件
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        result.ok = false;
        result.errorPhase = "io";
        result.errorMessage = "无法打开文件: " + path;
        return result;
    }
    std::stringstream ss;
    ss << ifs.rdbuf();
    std::string original = ss.str();

    // 2. 格式化
    auto fmtResult = formatSource(original, opts);
    if (!fmtResult.ok) {
        result.ok = false;
        result.errorPhase = fmtResult.errorPhase;
        result.errorMessage = path + ": " + fmtResult.errorMessage;
        return result;
    }

    // 3. 根据模式处理
    result.ok = true;
    result.needsFormat = (original != fmtResult.output);

    switch (mode) {
    case ProcessMode::Check:
        // 仅报告是否需要格式化，不修改文件
        break;
    case ProcessMode::Write:
        // 写回文件（仅当需要格式化时）
        if (result.needsFormat) {
            std::ofstream ofs(path, std::ios::trunc);
            if (!ofs.is_open()) {
                result.ok = false;
                result.errorPhase = "io";
                result.errorMessage = "无法写入文件: " + path;
                return result;
            }
            ofs << fmtResult.output;
        }
        break;
    case ProcessMode::DryRun:
        // 输出格式化结果到 stdout（调用方负责输出）
        result.output = fmtResult.output;
        break;
    }
    return result;
}

// ---- parseArgs: 命令行参数解析 ----
CliArgs parseArgs(int argc, char* argv[]) {
    CliArgs args;
    std::vector<std::string> positional;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            args.showHelp = true;
        } else if (arg == "--version" || arg == "-V") {
            args.showVersion = true;
        } else if (arg == "--check") {
            args.mode = ProcessMode::Check;
        } else if (arg == "--write") {
            args.mode = ProcessMode::Write;
        } else if (arg == "--dry-run") {
            args.mode = ProcessMode::DryRun;
        } else if (arg == "--tabs") {
            args.options.useTabs = true;
        } else if (arg == "--indent") {
            if (i + 1 >= argc) {
                args.parseError = true;
                args.errorMessage = "--indent 需要一个参数";
                return args;
            }
            std::string val = argv[++i];
            try {
                int n = std::stoi(val);
                if (n <= 0 || n > 16) {
                    args.parseError = true;
                    args.errorMessage = "--indent 必须在 1..16 范围内";
                    return args;
                }
                args.options.indentSize = n;
            } catch (const std::exception&) {
                args.parseError = true;
                args.errorMessage = "--indent 无效: " + val;
                return args;
            }
        } else if (arg == "--style") {
            if (i + 1 >= argc) {
                args.parseError = true;
                args.errorMessage = "--style 需要一个参数";
                return args;
            }
            std::string style = argv[++i];
            if (style == "kr") {
                args.options = FormatOptions(); // 默认 K&R
            } else if (style == "allman") {
                args.options = FormatOptions::allman();
            } else if (style == "compact") {
                args.options = FormatOptions::compact();
            } else if (style == "tabbed") {
                args.options = FormatOptions::tabbed();
            } else {
                args.parseError = true;
                args.errorMessage = "未知样式: " + style + "（可选: kr/allman/compact/tabbed）";
                return args;
            }
        } else if (arg.rfind("--", 0) == 0) {
            args.parseError = true;
            args.errorMessage = "未知选项: " + arg;
            return args;
        } else {
            positional.push_back(arg);
        }
    }

    args.files = std::move(positional);
    return args;
}

// ---- printHelp ----
void printHelp() {
    std::puts("minilang-fmt - MiniLang 代码格式化工具\n"
              "\n"
              "用法:\n"
              "  minilang-fmt [options] <file...>\n"
              "\n"
              "选项:\n"
              "  --check               检查模式：仅报告是否需要格式化（不修改文件）\n"
              "                        退出码 0=已格式化, 1=需要格式化\n"
              "  --write               写入模式：原地格式化文件（默认）\n"
              "  --dry-run             试运行：输出格式化结果到 stdout\n"
              "  --indent <N>          缩进空格数（默认 4，范围 1..16）\n"
              "  --style <preset>      预设样式：kr | allman | compact | tabbed\n"
              "  --tabs                使用 tab 缩进（覆盖 --indent）\n"
              "  -h, --help            显示帮助\n"
              "  -V, --version         显示版本\n"
              "\n"
              "退出码:\n"
              "  0  成功（--check 模式下：所有文件已格式化）\n"
              "  1  --check 模式下：至少一个文件需要格式化\n"
              "  2  错误（参数解析失败、文件不存在、词法/语法错误等）\n"
              "\n"
              "示例:\n"
              "  minilang-fmt --write foo.ml bar.ml\n"
              "  minilang-fmt --check *.ml\n"
              "  minilang-fmt --dry-run --style allman foo.ml\n"
              "  minilang-fmt --indent 2 --tabs foo.ml");
}

// ---- printVersion ----
void printVersion() {
    std::puts(versionString().c_str());
}

} // namespace minilang_fmt
