// ============================================================
// cli/lint_core.cpp - minilang-lint CLI 核心逻辑实现
// ------------------------------------------------------------
// 复用 fmt_core.cpp 的管线模式：
//   source → Lexer::scan → Parser::parse → LintPass::analyze
// 输出按 format（text/json）格式化。
// ============================================================
#include "cli/lint_core.h"

#include "ast/ASTNode.h"          // 拓展二期：--fix 结构化修复遍历 AST
#include "formatter/Formatter.h" // 拓展二期：修复后 Formatter 重输出
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <cstdio>
#include <fstream>
#include <set> // 拓展二期：诊断位置集合
#include <sstream>

namespace minilang_lint {

// P1 fix: JSON 字符串转义，防止特殊字符（引号/反斜杠/换行）破坏 JSON 结构（注入防护）
static std::string escapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c;
        }
    }
    return out;
}


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
    if (name == "NonExhaustiveMatch")
        return LintRule::NonExhaustiveMatch;
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

// ============================================================
// 拓展二期：--fix 自动修复（删除 AST 节点 + Formatter 重输出）
// ============================================================
namespace {

/// 副作用保守判定：子树含调用/写操作则不可安全删除
bool hasSideEffects(const ASTNode* node) {
    if (!node)
        return false;
    switch (node->nodeType) {
    case NodeType::NODE_FUN_CALL:
    case NodeType::NODE_METHOD_CALL:
    case NodeType::NODE_ASSIGNMENT:
    case NodeType::NODE_INDEX_ASSIGN:
    case NodeType::NODE_MEMBER_ASSIGN:
        return true;
    default:
        break;
    }
    for (const ASTNode* child : node->children()) {
        if (hasSideEffects(child))
            return true;
    }
    return false;
}

using PosSet = std::set<std::pair<int, int>>;

void fixNodeBlocks(ASTNode* node, const PosSet& unusedVars, const PosSet& deadCode, const PosSet& unusedFuns,
                   int& removed, int& skipped);

/// 对单个 Block 应用修复：按诊断位置删除语句，其余语句递归处理嵌套块
void fixBlock(Block& block, const PosSet& unusedVars, const PosSet& deadCode, const PosSet& unusedFuns, int& removed,
              int& skipped) {
    auto& stmts = block.statements;
    for (auto it = stmts.begin(); it != stmts.end();) {
        ASTNode* s = it->get();
        bool erase = false;
        if (s) {
            auto key = std::make_pair(s->line, s->column);
            if (s->nodeType == NodeType::NODE_VAR_DECL && unusedVars.count(key)) {
                auto* vd = static_cast<VarDecl*>(s);
                if (vd->initializer && hasSideEffects(vd->initializer.get())) {
                    ++skipped; // 初始化器有副作用：保守保留
                } else {
                    erase = true;
                }
            } else if (s->nodeType == NodeType::NODE_FUN_DECL && unusedFuns.count(key)) {
                erase = true;
            } else if (deadCode.count(key)) {
                erase = true; // 不可达语句：整节点删除，副作用无关（永不执行）
            }
        }
        if (erase) {
            it = stmts.erase(it);
            ++removed;
        } else {
            fixNodeBlocks(s, unusedVars, deadCode, unusedFuns, removed, skipped);
            ++it;
        }
    }
}

/// 通用递归：找到所有嵌套 Block（函数体/if/while/try 等）并应用修复
void fixNodeBlocks(ASTNode* node, const PosSet& unusedVars, const PosSet& deadCode, const PosSet& unusedFuns,
                   int& removed, int& skipped) {
    if (!node)
        return;
    if (node->nodeType == NodeType::NODE_BLOCK) {
        fixBlock(*static_cast<Block*>(node), unusedVars, deadCode, unusedFuns, removed, skipped);
        return;
    }
    for (ASTNode* child : node->children()) {
        fixNodeBlocks(child, unusedVars, deadCode, unusedFuns, removed, skipped);
    }
}

} // namespace

FixResult applyFixes(const std::string& source, const LintOptions& opts) {
    FixResult result;

    // 1. Lexer + Parser（与 lintSource 同管线，额外保留注释供 Formatter）
    Lexer lexer;
    auto tokens = lexer.scan(source);
    if (lexer.getDiagnostics().hasErrors()) {
        result.errorPhase = "lex";
        result.errorMessage = lexer.getDiagnostics().summary();
        return result;
    }
    Parser parser;
    auto ast = parser.parse(tokens);
    if (parser.hasErrors() || !ast) {
        result.errorPhase = "parse";
        result.errorMessage = parser.getDiagnostics().summary();
        return result;
    }

    // 2. Lint 分析，按诊断码收集可修复位置集合
    //    注：lint-unused-parameter 与函数声明同位置，必须排除
    //    （否则会把含未用参数的函数当未用变量误删）。
    minilang::lint::LintPass pass(opts);
    auto lintResult = pass.analyze(*ast);
    PosSet unusedVars;
    PosSet deadCode;
    PosSet unusedFuns;
    for (const auto& d : lintResult.diagnostics.all()) {
        auto key = std::make_pair(d.line, d.column);
        if (d.code == "lint-unused-variable") {
            unusedVars.insert(key);
        } else if (d.code == "lint-dead-code") {
            deadCode.insert(key);
        } else if (d.code == "lint-unused-function") {
            unusedFuns.insert(key);
        }
    }

    // 3. 结构化修复（从顶层 Block 递归）
    fixBlock(*ast, unusedVars, deadCode, unusedFuns, result.removedCount, result.skippedCount);

    // 4. Formatter 重输出（保留注释）
    Formatter fmt;
    fmt.setComments(lexer.comments());
    try {
        result.fixedSource = fmt.format(*ast);
    } catch (const std::exception& e) {
        result.errorPhase = "format";
        result.errorMessage = e.what();
        return result;
    }
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
    oss << "  \"file\": \"" << escapeJson(filePath) << "\",\n";
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
        oss << "\"code\": \"" << escapeJson(d.code) << "\", ";
        oss << "\"message\": \"" << escapeJson(d.message) << "\"";
        oss << "}";
    }
    if (!diags.empty())
        oss << "\n  ";
    oss << "]\n}\n";
    return oss.str();
}

// ---- processFile: 文件路径 → 处理结果 ----
// ---- processFileFix: 拓展二期 --fix 模式文件处理 ----
LintProcessResult processFileFix(const std::string& path, const LintOptions& opts, bool dryRun) {
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
    ifs.close();

    // 2. 应用修复
    FixResult fix = applyFixes(source, opts);
    if (!fix.ok) {
        result.ok = false;
        result.errorPhase = fix.errorPhase;
        result.errorMessage = path + ": " + fix.errorMessage;
        return result;
    }

    std::ostringstream out;
    if (dryRun) {
        // dry-run：修复后源码输出到 stdout，不回写
        out << fix.fixedSource;
        out << "// minilang-lint --fix-dry-run: 将删除 " << fix.removedCount << " 处，跳过 " << fix.skippedCount
            << " 处（副作用保守）\n";
    } else if (fix.removedCount > 0) {
        // 仅当确有删除时回写（避免纯格式化扰动无关文件）
        std::ofstream ofs(path, std::ios::trunc);
        if (!ofs.is_open()) {
            result.ok = false;
            result.errorPhase = "io";
            result.errorMessage = "无法写入文件: " + path;
            return result;
        }
        ofs << fix.fixedSource;
        out << path << ": 已修复 " << fix.removedCount << " 处，跳过 " << fix.skippedCount << " 处（副作用保守）\n";
    } else {
        out << path << ": 无可自动修复项（跳过 " << fix.skippedCount << " 处）\n";
    }

    result.ok = true;
    result.output = out.str();
    return result;
}

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
        } else if (arg == "--fix") {
            args.fix = true; // 拓展二期：结构化自动修复（回写文件）
        } else if (arg == "--fix-dry-run") {
            args.fixDryRun = true; // 拓展二期：仅预览不回写
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
              "  --fix                    自动修复可修复问题并回写文件\n"
              "                           （未用变量/未用函数/不可达代码，副作用保守）\n"
              "  --fix-dry-run            预览修复后源码，不回写文件\n"
              "  --rule <name>            禁用指定规则（可重复使用）\n"
              "                           可选: UnusedVariable, UnusedFunction, UnusedParameter,\n"
              "                                 AssignmentInCondition, DeadCodeAfterReturn,\n"
              "                                 EmptyBlock, CyclomaticComplexity, NamingConvention,\n"
              "                                 NonExhaustiveMatch\n"
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
