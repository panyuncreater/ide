// ============================================================
// cli/coverage_core.cpp - minilang-coverage CLI 核心逻辑实现
// ------------------------------------------------------------
// R110 行级覆盖率工具：复用 Compiler + VM stepCallback + BytecodeChunk.lines
// 行号信息收集行级覆盖率。
//
// 关键设计点：
//   (1) 可执行行集：扫描 mainChunk.lines + 所有 functionChunks[*].lines，
//       去重后得到 1-based 源码行号集合（过滤 0 表示无位置信息）。
//   (2) 已执行行集：VM 运行时 stepCallback 内读 vm.getCurrentLine()，
//       用 unordered_map<int, uint64_t> 累加每行执行次数。
//   (3) 对比两集，生成 LineCoverage 列表（覆盖全源码行号范围 1..maxLine）。
//   (4) 双后端支持：StackVM 走 Compiler::compile + VM；RegisterVM 走
//       Compiler::compileViaRegisterIR + RegisterVM。
//   (5) LCOV 格式：SF/DA/LF/LH/end_of_record，兼容 lcov/genhtml。
// ============================================================
#include "cli/coverage_core.h"

#include "common/Diagnostic.h"
#include "compiler/Bytecode.h"
#include "compiler/Compiler.h"
#include "compiler/RegisterBytecode.h"
#include "compiler/RegisterVM.h"
#include "compiler/VM.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace minilang_coverage {

// ============================================================
// 辅助函数
// ============================================================

namespace {

/// 从源码字符串拆分出每行内容（用于 --show-source）
std::vector<std::string> splitLines(const std::string& source) {
    std::vector<std::string> lines;
    std::string current;
    for (char c : source) {
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
        } else if (c != '\r') {
            current.push_back(c);
        }
    }
    lines.push_back(current); // 末尾（即使无换行）
    return lines;
}

/// 收集 BytecodeChunk 中所有非 0 行号（可执行行）
template <typename Chunk> void collectExecutableLines(const Chunk& chunk, std::unordered_set<int>& out) {
    for (int line : chunk.lines) {
        if (line > 0) {
            out.insert(line);
        }
    }
}

/// 收集 CompileResult 中所有可执行行（main + functions）
std::unordered_set<int> collectExecutableFromStack(const CompileResult& result) {
    std::unordered_set<int> lines;
    collectExecutableLines(result.mainChunk, lines);
    for (const auto& kv : result.functionChunks) {
        collectExecutableLines(kv.second, lines);
    }
    return lines;
}

/// 收集 RegisterCompileResult 中所有可执行行
std::unordered_set<int> collectExecutableFromRegister(const RegisterCompileResult& result) {
    std::unordered_set<int> lines;
    collectExecutableLines(result.mainChunk, lines);
    for (const auto& kv : result.functionChunks) {
        collectExecutableLines(kv.second, lines);
    }
    return lines;
}

/// 按 StackVM 后端分析覆盖率
FileCoverage analyzeWithStackVM(const std::string& source, const std::string& sourceName, Block& ast,
                                const CoverageOptions& opts) {
    FileCoverage fc;
    fc.sourceName = sourceName;
    fc.source = source;
    fc.backendUsed = "StackVM";

    // 1. 编译
    Compiler compiler;
    CompileResult cr = compiler.compile(ast);
    if (compiler.getDiagnostics().hasErrors()) {
        fc.ok = false;
        fc.errorPhase = "compile";
        fc.errorMessage = compiler.getDiagnostics().summary();
        return fc;
    }

    // 2. 收集可执行行集
    auto executableLines = collectExecutableFromStack(cr);

    // 3. 设置 VM stepCallback 收集行号计数
    std::unordered_map<int, uint64_t> lineCounts;
    VM vm;
    vm.setOutputCallback([](const std::string&) {}); // 静默输出
    vm.setStepCallback([&lineCounts, &vm](const VMStepInfo&) {
        int line = vm.getCurrentLine();
        if (line > 0) {
            ++lineCounts[line];
        }
    });
    vm.setStepCallbackEnabled(true);

    // 4. 全速执行
    VMResult vmres = vm.execute(cr);
    vm.setStepCallbackEnabled(false);
    if (vmres != VMResult::VM_OK || vm.hasError()) {
        fc.ok = false;
        fc.errorPhase = "run";
        fc.errorMessage = vm.hasError() ? vm.getLastError() : "VM 执行失败";
        return fc;
    }

    // 5. 计算最大行号
    int maxLine = 0;
    for (int line : executableLines) {
        if (line > maxLine)
            maxLine = line;
    }
    for (const auto& kv : lineCounts) {
        if (kv.first > maxLine)
            maxLine = kv.first;
    }
    // 也考虑源码行数（用于 showSource）
    auto srcLines = splitLines(source);
    int srcLineCount = static_cast<int>(srcLines.size());
    if (srcLineCount > maxLine)
        maxLine = srcLineCount;

    // 6. 生成 LineCoverage 列表
    fc.lines.reserve(static_cast<size_t>(maxLine));
    fc.totalExecutable = 0;
    fc.totalCovered = 0;
    for (int line = 1; line <= maxLine; ++line) {
        LineCoverage lc;
        lc.line = line;
        lc.executable = executableLines.count(line) > 0;
        auto it = lineCounts.find(line);
        if (it != lineCounts.end()) {
            lc.count = it->second;
            lc.covered = true;
        }
        if (lc.executable)
            ++fc.totalExecutable;
        if (lc.covered)
            ++fc.totalCovered;
        fc.lines.push_back(lc);
    }

    fc.ratio = fc.totalExecutable > 0
                   ? (100.0 * static_cast<double>(fc.totalCovered) / static_cast<double>(fc.totalExecutable))
                   : 0.0;
    fc.ok = true;
    return fc;
}

/// 按 RegisterVM 后端分析覆盖率
FileCoverage analyzeWithRegisterVM(const std::string& source, const std::string& sourceName, Block& ast,
                                   const CoverageOptions& opts) {
    FileCoverage fc;
    fc.sourceName = sourceName;
    fc.source = source;
    fc.backendUsed = "RegisterVM";

    // 1. 编译（寄存器路径）
    Compiler compiler;
    compiler.setUseRegisterVM(true);
    RegisterCompileResult cr = compiler.compileViaRegisterIR(ast);
    if (compiler.getDiagnostics().hasErrors()) {
        fc.ok = false;
        fc.errorPhase = "compile";
        fc.errorMessage = compiler.getDiagnostics().summary();
        return fc;
    }

    // 2. 收集可执行行集
    auto executableLines = collectExecutableFromRegister(cr);

    // 3. 设置 RegisterVM stepCallback 收集行号计数
    std::unordered_map<int, uint64_t> lineCounts;
    RegisterVM vm;
    vm.setOutputCallback([](const std::string&) {});
    vm.setStepCallback([&lineCounts, &vm](const RegVMStepInfo&) {
        int line = vm.getCurrentLine();
        if (line > 0) {
            ++lineCounts[line];
        }
    });
    vm.setStepCallbackEnabled(true);

    // 4. 全速执行
    VMResult vmres = vm.execute(cr);
    vm.setStepCallbackEnabled(false);
    if (vmres != VMResult::VM_OK || vm.hasError()) {
        fc.ok = false;
        fc.errorPhase = "run";
        fc.errorMessage = vm.hasError() ? vm.getLastError() : "RegisterVM 执行失败";
        return fc;
    }

    // 5. 计算最大行号
    int maxLine = 0;
    for (int line : executableLines) {
        if (line > maxLine)
            maxLine = line;
    }
    for (const auto& kv : lineCounts) {
        if (kv.first > maxLine)
            maxLine = kv.first;
    }
    auto srcLines = splitLines(source);
    int srcLineCount = static_cast<int>(srcLines.size());
    if (srcLineCount > maxLine)
        maxLine = srcLineCount;

    // 6. 生成 LineCoverage 列表
    fc.lines.reserve(static_cast<size_t>(maxLine));
    fc.totalExecutable = 0;
    fc.totalCovered = 0;
    for (int line = 1; line <= maxLine; ++line) {
        LineCoverage lc;
        lc.line = line;
        lc.executable = executableLines.count(line) > 0;
        auto it = lineCounts.find(line);
        if (it != lineCounts.end()) {
            lc.count = it->second;
            lc.covered = true;
        }
        if (lc.executable)
            ++fc.totalExecutable;
        if (lc.covered)
            ++fc.totalCovered;
        fc.lines.push_back(lc);
    }

    fc.ratio = fc.totalExecutable > 0
                   ? (100.0 * static_cast<double>(fc.totalCovered) / static_cast<double>(fc.totalExecutable))
                   : 0.0;
    fc.ok = true;
    return fc;
}

} // anonymous namespace

// ============================================================
// 公开 API 实现
// ============================================================

std::string versionString() {
    return "minilang-coverage 1.0.0 (R110, 2026-07-19)";
}

FileCoverage analyzeSource(const std::string& source, const std::string& sourceName, const CoverageOptions& opts) {
    FileCoverage fail; // 错误返回模板

    // 1. Lexer
    Lexer lexer;
    auto tokens = lexer.scan(source);
    if (lexer.getDiagnostics().hasErrors()) {
        fail.sourceName = sourceName;
        fail.source = source;
        fail.ok = false;
        fail.errorPhase = "lex";
        fail.errorMessage = lexer.getDiagnostics().summary();
        return fail;
    }

    // 2. Parser
    Parser parser;
    auto ast = parser.parse(tokens);
    if (parser.hasErrors() || !ast) {
        fail.sourceName = sourceName;
        fail.source = source;
        fail.ok = false;
        fail.errorPhase = "parse";
        fail.errorMessage = parser.getDiagnostics().summary();
        return fail;
    }

    // 3. 根据后端选择分析路径
    if (opts.backend == Backend::StackVM) {
        return analyzeWithStackVM(source, sourceName, *ast, opts);
    } else if (opts.backend == Backend::RegisterVM) {
        return analyzeWithRegisterVM(source, sourceName, *ast, opts);
    } else {
        // Both: 先跑 StackVM 再跑 RegisterVM；返回 StackVM 的报告，
        // 但将 RegisterVM 的覆盖率信息追加到 errorMessage 字段（仅当 ok 时）。
        // 简化设计：Both 模式下返回 StackVM 报告，并在 sourceName 标注。
        // 完整的并列展示由 formatReportText 处理（多文件场景）。
        auto stackFC = analyzeWithStackVM(source, sourceName, *ast, opts);
        auto regFC = analyzeWithRegisterVM(source, sourceName + " [RegisterVM]", *ast, opts);
        // 在 Both 模式下，把 regFC 嵌入 stackFC 的 sourceName 末尾备注
        // 简化做法：返回 stackFC，调用方需分别跑两次（StackVM + RegisterVM）
        // 这里为保持 API 简洁，Both 模式退化为 StackVM + 在 sourceName 标注
        if (stackFC.ok && regFC.ok) {
            stackFC.sourceName = sourceName +
                                 " (StackVM vs RegisterVM: " + std::to_string(static_cast<int>(stackFC.ratio)) +
                                 "% / " + std::to_string(static_cast<int>(regFC.ratio)) + "%)";
        }
        return stackFC;
    }
}

FileCoverage analyzeFile(const std::string& path, const CoverageOptions& opts) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        FileCoverage fc;
        fc.sourceName = path;
        fc.ok = false;
        fc.errorPhase = "io";
        fc.errorMessage = "无法打开文件: " + path;
        return fc;
    }
    std::stringstream ss;
    ss << ifs.rdbuf();
    std::string source = ss.str();

    // 用文件名（不含路径）作为 sourceName，便于 LCOV 输出
    std::string sourceName = path;
    try {
        sourceName = std::filesystem::path(path).filename().string();
    } catch (...) {
        // 保持原 path
    }

    return analyzeSource(source, sourceName, opts);
}

CoverageReport analyzeFiles(const std::vector<std::string>& paths, const CoverageOptions& opts) {
    CoverageReport report;
    report.ok = true;
    report.totalExecutable = 0;
    report.totalCovered = 0;

    // Both 模式：分别跑 StackVM + RegisterVM，每文件产生两条条目
    bool bothMode = (opts.backend == Backend::Both);

    for (const auto& path : paths) {
        if (bothMode) {
            CoverageOptions stackOpts = opts;
            stackOpts.backend = Backend::StackVM;
            CoverageOptions regOpts = opts;
            regOpts.backend = Backend::RegisterVM;
            FileCoverage stackFC = analyzeFile(path, stackOpts);
            FileCoverage regFC = analyzeFile(path, regOpts);
            // 在 sourceName 标注后端以便区分
            if (stackFC.ok) {
                stackFC.sourceName += " [StackVM]";
            }
            if (regFC.ok) {
                regFC.sourceName += " [RegisterVM]";
            }
            if (!stackFC.ok || !regFC.ok) {
                report.ok = false;
            } else {
                report.totalExecutable += stackFC.totalExecutable;
                report.totalCovered += stackFC.totalCovered;
                // RegisterVM 行单独累加（Both 模式下汇总是 StackVM）
                // 为避免重复计算，Both 模式的汇总仅基于 StackVM（与主后端对齐）
            }
            report.files.push_back(std::move(stackFC));
            report.files.push_back(std::move(regFC));
        } else {
            FileCoverage fc = analyzeFile(path, opts);
            if (!fc.ok) {
                report.ok = false;
            } else {
                report.totalExecutable += fc.totalExecutable;
                report.totalCovered += fc.totalCovered;
            }
            report.files.push_back(std::move(fc));
        }
    }

    report.ratio =
        report.totalExecutable > 0
            ? (100.0 * static_cast<double>(report.totalCovered) / static_cast<double>(report.totalExecutable))
            : 0.0;
    return report;
}

// ============================================================
// 文本格式化
// ============================================================

std::string formatFileText(const FileCoverage& fc, const CoverageOptions& opts) {
    std::ostringstream os;
    if (!fc.ok) {
        os << "[" << fc.sourceName << "] 错误（" << fc.errorPhase << "）: " << fc.errorMessage << "\n";
        return os.str();
    }

    os << "=== " << fc.sourceName << " ===\n";
    os << "后端: " << fc.backendUsed << "\n";
    os << "覆盖率: " << fc.totalCovered << "/" << fc.totalExecutable << " (" << std::fixed << std::setprecision(2)
       << fc.ratio << "%)\n";

    if (opts.showSource) {
        auto srcLines = splitLines(fc.source);
        os << "\n行号  状态  次数      源码\n";
        os << "----  ----  --------  -----------------------------------------\n";
        for (const auto& lc : fc.lines) {
            if (!lc.executable && lc.line > static_cast<int>(srcLines.size())) {
                continue; // 跳过超出源码范围的行
            }
            const std::string& src = lc.line <= static_cast<int>(srcLines.size()) ? srcLines[lc.line - 1] : "";
            std::string status;
            if (!lc.executable) {
                status = "    "; // 非可执行
            } else if (lc.covered) {
                status = " OK ";
            } else {
                status = "MISS";
            }
            char countBuf[32];
            if (lc.executable && lc.covered) {
                std::snprintf(countBuf, sizeof(countBuf), "%-8llu", static_cast<unsigned long long>(lc.count));
            } else if (lc.executable) {
                std::snprintf(countBuf, sizeof(countBuf), "%-8s", "0");
            } else {
                std::snprintf(countBuf, sizeof(countBuf), "%-8s", "-");
            }
            // 仅在 showUncovered 或 covered 时打印，避免输出过长
            if (!opts.showUncovered && lc.executable && !lc.covered) {
                continue;
            }
            os << std::setw(4) << lc.line << "  " << status << "  " << countBuf << "  " << src << "\n";
        }
    } else {
        // 仅显示摘要 + 未覆盖行清单
        if (opts.showUncovered) {
            std::vector<int> uncovered;
            for (const auto& lc : fc.lines) {
                if (lc.executable && !lc.covered) {
                    uncovered.push_back(lc.line);
                }
            }
            if (!uncovered.empty()) {
                os << "\n未覆盖行 (" << uncovered.size() << "): ";
                for (size_t i = 0; i < uncovered.size(); ++i) {
                    if (i > 0)
                        os << ", ";
                    os << uncovered[i];
                    if (opts.showCounts) {
                        // 已是未覆盖，count 必为 0，无需追加
                    }
                }
                os << "\n";
            } else {
                os << "\n所有可执行行均已覆盖\n";
            }
        }
    }
    return os.str();
}

std::string formatReportText(const CoverageReport& report, const CoverageOptions& opts) {
    std::ostringstream os;
    for (const auto& fc : report.files) {
        os << formatFileText(fc, opts);
        os << "\n";
    }
    if (report.files.size() > 1) {
        os << "=== 汇总 ===\n";
        os << "文件数: " << report.files.size() << "\n";
        os << "总覆盖率: " << report.totalCovered << "/" << report.totalExecutable << " (" << std::fixed
           << std::setprecision(2) << report.ratio << "%)\n";
    }
    return os.str();
}

// ============================================================
// LCOV 格式化
// ============================================================

std::string formatFileLcov(const FileCoverage& fc) {
    std::ostringstream os;
    if (!fc.ok) {
        // LCOV 无标准错误格式，输出注释行
        os << "# ERROR (" << fc.errorPhase << "): " << fc.sourceName << ": " << fc.errorMessage << "\n";
        return os.str();
    }
    os << "SF:" << fc.sourceName << "\n";
    int lf = 0;
    int lh = 0;
    for (const auto& lc : fc.lines) {
        if (!lc.executable)
            continue;
        ++lf;
        if (lc.covered)
            ++lh;
        os << "DA:" << lc.line << "," << lc.count << "\n";
    }
    os << "LF:" << lf << "\n";
    os << "LH:" << lh << "\n";
    os << "end_of_record\n";
    return os.str();
}

std::string formatReportLcov(const CoverageReport& report) {
    std::ostringstream os;
    os << "TN:minilang-coverage\n"; // 测试名（LCOV 规范要求）
    for (const auto& fc : report.files) {
        os << formatFileLcov(fc);
    }
    return os.str();
}

// ============================================================
// CLI 参数解析
// ============================================================

CliArgs parseArgs(int argc, char* argv[]) {
    CliArgs args;
    std::vector<std::string> positional;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            args.showHelp = true;
        } else if (arg == "--version" || arg == "-V") {
            args.showVersion = true;
        } else if (arg == "--backend") {
            if (i + 1 >= argc) {
                args.parseError = true;
                args.errorMessage = "--backend 需要一个参数";
                return args;
            }
            std::string b = argv[++i];
            if (b == "stack") {
                args.options.backend = Backend::StackVM;
            } else if (b == "register") {
                args.options.backend = Backend::RegisterVM;
            } else if (b == "both") {
                args.options.backend = Backend::Both;
            } else {
                args.parseError = true;
                args.errorMessage = "未知后端: " + b + "（可选: stack/register/both）";
                return args;
            }
        } else if (arg == "--format") {
            if (i + 1 >= argc) {
                args.parseError = true;
                args.errorMessage = "--format 需要一个参数";
                return args;
            }
            std::string f = argv[++i];
            if (f == "text") {
                args.options.format = OutputFormat::Text;
            } else if (f == "lcov") {
                args.options.format = OutputFormat::Lcov;
            } else {
                args.parseError = true;
                args.errorMessage = "未知格式: " + f + "（可选: text/lcov）";
                return args;
            }
        } else if (arg == "--show-counts") {
            args.options.showCounts = true;
        } else if (arg == "--show-source") {
            args.options.showSource = true;
        } else if (arg == "--no-uncovered") {
            args.options.showUncovered = false;
        } else if (arg == "--fail-under") {
            if (i + 1 >= argc) {
                args.parseError = true;
                args.errorMessage = "--fail-under 需要一个参数";
                return args;
            }
            std::string v = argv[++i];
            try {
                double d = std::stod(v);
                if (d < 0 || d > 100) {
                    args.parseError = true;
                    args.errorMessage = "--fail-under 必须在 0..100 范围内";
                    return args;
                }
                args.options.failUnder = d;
            } catch (const std::exception&) {
                args.parseError = true;
                args.errorMessage = "--fail-under 无效: " + v;
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

void printHelp() {
    std::puts("minilang-coverage - MiniLang 行级覆盖率工具\n"
              "\n"
              "用法:\n"
              "  minilang-coverage [options] <file...>\n"
              "\n"
              "选项:\n"
              "  --backend <type>      选择后端：stack | register | both（默认 stack）\n"
              "  --format <format>     输出格式：text | lcov（默认 text）\n"
              "  --show-counts         显示每行执行次数\n"
              "  --show-source         显示源码行内容\n"
              "  --no-uncovered        不显示未覆盖行清单（默认显示）\n"
              "  --fail-under <pct>    覆盖率阈值（0-100），低于则返回退出码 1\n"
              "  -h, --help            显示帮助\n"
              "  -V, --version         显示版本\n"
              "\n"
              "退出码:\n"
              "  0  成功（覆盖率 >= 阈值，或未指定阈值）\n"
              "  1  覆盖率低于 --fail-under 指定的阈值\n"
              "  2  错误（参数解析失败、文件不存在、词法/语法错误等）\n"
              "\n"
              "示例:\n"
              "  minilang-coverage foo.ml\n"
              "  minilang-coverage --show-source foo.ml\n"
              "  minilang-coverage --format lcov --fail-under 80 *.ml > coverage.info\n"
              "  minilang-coverage --backend register --show-counts foo.ml\n"
              "  minilang-coverage --backend both foo.ml");
}

void printVersion() {
    std::puts(versionString().c_str());
}

} // namespace minilang_coverage
