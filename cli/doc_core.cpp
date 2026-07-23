// ============================================================
// cli/doc_core.cpp - minilang-doc CLI 核心逻辑实现
// ------------------------------------------------------------
// 复用 fmt_core.cpp / lint_core.cpp 的管线模式：
//   source → Lexer::scan → Parser::parse → DocGenerator::generate
// ============================================================
#include "cli/doc_core.h"

#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace minilang_doc {

// ---- 版本信息 ----
std::string versionString() {
    return "minilang-doc 1.0.0 (R162 B2, 2026-07-22)";
}

// ---- parseFormatName: 格式名 → DocFormat ----
minilang::doc::DocFormat parseFormatName(const std::string& name) {
    if (name == "markdown" || name == "md") {
        return minilang::doc::DocFormat::Markdown;
    }
    if (name == "html") {
        return minilang::doc::DocFormat::Html;
    }
    if (name == "json") {
        return minilang::doc::DocFormat::Json;
    }
    return minilang::doc::DocFormat::Markdown;
}

// ---- generateDoc: 源码 → DocResult ----
DocSourceResult generateDoc(const std::string& source, minilang::doc::DocFormat format) {
    DocSourceResult result;

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

    // 3. DocGenerator
    minilang::doc::DocGenerator gen(format);
    result.doc = gen.generate(*ast, lexer.comments());
    result.ok = true;
    return result;
}

// ---- processFile: 文件路径 → 处理结果 ----
DocProcessResult processFile(const std::string& path, minilang::doc::DocFormat format) {
    DocProcessResult result;

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
    std::string source = ss.str();

    // 2. 生成文档
    auto docResult = generateDoc(source, format);
    if (!docResult.ok) {
        result.ok = false;
        result.errorPhase = docResult.errorPhase;
        result.errorMessage = path + ": " + docResult.errorMessage;
        return result;
    }

    // 3. 格式化输出
    minilang::doc::DocGenerator gen(format);
    result.ok = true;
    result.entryCount = static_cast<int>(docResult.doc.entries.size());
    result.output = gen.formatOutput(docResult.doc);
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
        } else if (arg == "--format" || arg == "-f") {
            if (i + 1 >= argc) {
                args.parseError = true;
                args.errorMessage = "--format 需要一个参数";
                return args;
            }
            std::string fmt = argv[++i];
            if (fmt == "markdown" || fmt == "md" || fmt == "html" || fmt == "json") {
                args.format = parseFormatName(fmt);
            } else {
                args.parseError = true;
                args.errorMessage = "未知格式: " + fmt + "（可选: markdown/html/json）";
                return args;
            }
        } else if (arg == "--output" || arg == "-o") {
            if (i + 1 >= argc) {
                args.parseError = true;
                args.errorMessage = "--output 需要一个参数";
                return args;
            }
            args.outputFile = argv[++i];
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
    std::puts("minilang-doc - MiniLang API 文档生成工具\n"
              "\n"
              "用法:\n"
              "  minilang-doc [options] <file...>\n"
              "\n"
              "选项:\n"
              "  --format, -f <fmt>     输出格式: markdown | html | json（默认 markdown）\n"
              "  --output, -o <file>    输出到文件（默认 stdout）\n"
              "  -h, --help             显示帮助\n"
              "  -V, --version          显示版本\n"
              "\n"
              "文档注释约定:\n"
              "  /// 单行文档注释（连续 /// 合并为一段）\n"
              "  /** 多行文档注释 */\n"
              "  支持标签: @param @return @example @deprecated @see @since\n"
              "\n"
              "退出码:\n"
              "  0  成功（文档生成完成）\n"
              "  2  错误（参数解析失败、文件不存在、词法/语法错误等）\n"
              "\n"
              "示例:\n"
              "  minilang-doc foo.ml > api.md\n"
              "  minilang-doc --format html -o docs/api.html foo.ml bar.ml\n"
              "  minilang-doc --format json *.ml > api.json");
}

// ---- printVersion ----
void printVersion() {
    std::puts(versionString().c_str());
}

} // namespace minilang_doc
