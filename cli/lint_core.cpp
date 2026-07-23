// ============================================================
// cli/lint_core.cpp - minilang-lint CLI 核心逻辑实现
// ------------------------------------------------------------
// 复用 fmt_core.cpp 的管线模式：
//   source → Lexer::scan → Parser::parse → LintPass::analyze
// 输出按 format（text/json）格式化。
// ============================================================
#include "cli/lint_core.h"

#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace minilang_lint {

// ---- 版本信息 ----
std::string versionString() {
    return "minilang-lint 1.0.0 (R162, 2026-07-22)";
}

// ---- parseRuleName: PascalCase → LintRule ----
LintRule parseRuleName(const std::string& name) {
    if (name == "UnusedVariable")
        return LintRule::UnusedVariable;
    if (name == "UnusedFunction")
        return LintRule::UnusedFunction;
    if (name == "UnusedParameter")
        return LintRule::UnusedParameter;
    if (name == "AssignmentInCondition")
        return LintRule::AssignmentInCondition;
    if (name == "DeadCodeAfterReturn")
        return LintRule::DeadCodeAfterReturn;
    if (name == "EmptyBlock")
        return LintRule::EmptyBlock;
    if (name == "CyclomaticComplexity")
        return LintRule::CyclomaticComplexity;
    if (name == "NamingConvention")
        return LintRule::NamingConvention;
    return LintRule::Count;
}

// ---- lintSource: 源码 → LintResult ----
LintSourceResult lintSource(const std::string& source, const LintOptions& opts) {
    LintSourceResult result;

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

    // 3. LintPass
    minilang::lint::LintPass pass(opts);
    result.lint = pass.analyze(*ast);
    result.ok = true;
    return result;
}

// ---- formatDiagnosticText: 单条诊断 → 文本行 ----
std::string formatDiagnosticText(const std::string& filePath, const Diagnostic& diag) {
    std::ostringstream oss;
    oss << filePath << ":" << diag.line << ":" << diag.column << ": ";
    switch (diag.level) {
    case DiagLevel::Error:
        oss << "错误";
        break;
    case DiagLevel::Warning:
        oss << "警告";
        break;
    case DiagLevel::Info:
        oss << "信息";
        break;
    case DiagLevel::Hint:
        oss << "建议";
        break;
    }
    oss << ": " << diag.message;
    if (!diag.code.empty()) {
        oss << " [" << diag.code << "]";
    }
    return oss.str();
}

// ---- formatResultJson: LintResult → JSON ----
std::string formatResultJson(const std::string& filePath, const LintResult& result) {
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"file\": \"" << filePath << "\",\n";
    oss << "  \"ok\": " << (result.ok ? "true" : "false") << ",\n";
    oss << "  \"errors\": " << result.errorCount << ",\n";
    oss << "  \"warnings\": " << result.warningCount << ",\n";
    oss << "  \"diagnostics\": [";
    const auto& diags = result.diagnostics.all();
    for (size_t i = 0; i < diags.size(); ++i) {
        const auto& d = diags[i];
        if (i > 0)
            oss << ",";
        oss << "\n    {";
        oss << "\"line\": " << d.line << ", ";
        oss << "\"column\": " << d.column << ", ";
        oss << "\"level\": \"";
        switch (d.level) {
        case DiagLevel::Error:
            oss << "error";
            break;
        case DiagLevel::Warning:
            oss << "warning";
            break;
        case DiagLevel::Info:
            oss << "info";
            break;
        case DiagLevel::Hint:
            oss << "hint";
            break;
        }
        oss << "\", ";
        oss << "\"code\": \"" << d.code << "\", ";
        oss << "\"message\": \"" << d.message << "\"";
        oss << "}";
    }
    if (!diags.empty())
        oss << "\n  ";
    oss << "]\n}\n";
    return oss.str();
}

// ---- processFile: 文件路径 → 处理结果 ----
LintProcessResult processFile(const std::string& path, const LintOptions& opts, OutputFormat format, bool quiet) {
    LintProcessResult result;

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

    // 2. lint
    auto lintResult = lintSource(source, opts);
    if (!lintResult.ok) {
        result.ok = false;
        result.errorPhase = lintResult.errorPhase;
        result.errorMessage = path + ": " + lintResult.errorMessage;
        return result;
    }

    // 3. 收集结果
    result.ok = true;
    result.warningCount = lintResult.lint.warningCount;
    result.errorCount = lintResult.lint.errorCount;
    result.hasWarnings = lintResult.lint.warningCount > 0;

    // 4. 格式化输出
    std::ostringstream out;
    if (format == OutputFormat::Json) {
        out << formatResultJson(path, lintResult.lint);
    } else {
        // Text 格式
        const auto& diags = lintResult.lint.diagnostics.all();
        for (const auto& d : diags) {
            // --quiet 模式：仅输出错误，跳过警告/信息/建议
            if (quiet && !d.isError())
                continue;
            out << formatDiagnosticText(path, d) << "\n";
        }
        // 摘要行
        if (!quiet || result.errorCount > 0) {
            out << path << ": " << lintResult.lint.diagnostics.summary() << "\n";
        }
    }
    result.output = out.str();
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
        } else if (arg == "--quiet" || arg == "-q") {
            args.quiet = true;
        } else if (arg == "--format") {
            if (i + 1 >= argc) {
                args.parseError = true;
                args.errorMessage = "--format 需要一个参数";
                return args;
            }
            std::string fmt = argv[++i];
            if (fmt == "text") {
                args.format = OutputFormat::Text;
            } else if (fmt == "json") {
                args.format = OutputFormat::Json;
            } else {
                args.parseError = true;
                args.errorMessage = "未知格式: " + fmt + "（可选: text/json）";
                return args;
            }
        } else if (arg == "--rule") {
            if (i + 1 >= argc) {
                args.parseError = true;
                args.errorMessage = "--rule 需要一个参数";
                return args;
            }
            std::string ruleName = argv[++i];
            auto rule = parseRuleName(ruleName);
            if (rule == LintRule::Count) {
                args.parseError = true;
                args.errorMessage = "未知规则: " + ruleName +
                                    "（可选: UnusedVariable/UnusedFunction/UnusedParameter/"
                                    "AssignmentInCondition/DeadCodeAfterReturn/EmptyBlock/"
                                    "CyclomaticComplexity/NamingConvention）";
                return args;
            }
            args.options.disabledRules.insert(rule);
        } else if (arg == "--max-complexity") {
            if (i + 1 >= argc) {
                args.parseError = true;
                args.errorMessage = "--max-complexity 需要一个参数";
                return args;
            }
            std::string val = argv[++i];
            try {
                int n = std::stoi(val);
                if (n <= 0 || n > 100) {
                    args.parseError = true;
                    args.errorMessage = "--max-complexity 必须在 1..100 范围内";
                    return args;
                }
                args.options.maxCyclomaticComplexity = n;
            } catch (const std::exception&) {
                args.parseError = true;
                args.errorMessage = "--max-complexity 无效: " + val;
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
    std::puts("minilang-lint - MiniLang 静态分析工具\n"
              "\n"
              "用法:\n"
              "  minilang-lint [options] <file...>\n"
              "\n"
              "选项:\n"
              "  --quiet, -q              仅输出错误，不输出警告\n"
              "  --rule <name>            禁用指定规则（可重复使用）\n"
              "                           可选: UnusedVariable, UnusedFunction, UnusedParameter,\n"
              "                                 AssignmentInCondition, DeadCodeAfterReturn,\n"
              "                                 EmptyBlock, CyclomaticComplexity, NamingConvention\n"
              "  --max-complexity <N>     圈复杂度阈值（默认 10，范围 1..100）\n"
              "  --format <fmt>           输出格式: text | json（默认 text）\n"
              "  -h, --help               显示帮助\n"
              "  -V, --version            显示版本\n"
              "\n"
              "退出码:\n"
              "  0  成功（无警告、无错误）\n"
              "  1  有 lint 警告（源码可解析但存在静态问题）\n"
              "  2  错误（参数解析失败、文件不存在、词法/语法错误等）\n"
              "\n"
              "示例:\n"
              "  minilang-lint foo.ml bar.ml\n"
              "  minilang-lint --quiet *.ml\n"
              "  minilang-lint --rule UnusedVariable --rule EmptyBlock foo.ml\n"
              "  minilang-lint --max-complexity 5 foo.ml\n"
              "  minilang-lint --format json foo.ml > lint-report.json");
}

// ---- printVersion ----
void printVersion() {
    std::puts(versionString().c_str());
}

} // namespace minilang_lint
