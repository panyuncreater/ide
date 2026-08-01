// ============================================================
// testing/fuzz_parser.cpp — MiniLang 前端鲁棒性 Fuzzing target（阶段四）
// ------------------------------------------------------------
// 被测属性（front-end robustness invariant）：
//   任意字节流 → 完整前端（Lexer::scan → Parser::parse，含 parse 期宏展开）
//   → 不得崩溃（无段错误 / abort / UB / 未处理 C++ 异常 / 死循环）。
//
// 前端契约是「全函数」（total）：Lexer::scan 与 Parser::parse 对一切非法输入
// 都通过内部 DiagnosticBag 记录 + 错误恢复（synchronize）处理，ParseError 在
// parse() 内部被捕获，绝不向外传播。因此本 target **不用 try/catch 包裹**——
// 任何逸出的异常 / abort / UB 都是真实的鲁棒性缺陷，必须暴露给 fuzzer/sanitizer。
//
// 链接闭包（无 Qt / 无 windows.h / 无 asmjit，纯前端 6 个源文件）：
//   lexer/Lexer.cpp  parser/Parser.cpp  ast/ASTNode.cpp  ast/MacroExpander.cpp
//   interpreter/Value.cpp  interpreter/GcManager.cpp
//
// 两种构建模式（同一份 target 复用）：
//   (A) libFuzzer（Clang，WSL/Linux 或支持 -fsanitize=fuzzer 的 clang-cl）：
//       libFuzzer 提供 main()，LLVMFuzzerTestOneInput 为覆盖率引导入口。
//       构建见 testing/fuzz/build_fuzz.sh，运行见 testing/fuzz/run_fuzz.sh。
//   (B) Standalone 回放（任意编译器，含 MSVC）：定义宏 MINILANG_FUZZER_STANDALONE
//       后编译，提供 main() 逐个回放 corpus/crash 文件，进程存活到底即通过。
//       用于 CTest 回归（pipeline.fuzz.replay），在无 Clang 的平台上守护该 target。
//
// 本文件为纯新增测试/工具代码，不修改编译器任何实现逻辑。
// ============================================================

#include "ast/ASTNode.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

// 有界遍历上限：防止对超大 AST 的遍历本身成为热点/伪 OOM。
// 前端已有 DoS 护栏（Lexer::MAX_SOURCE_SIZE / Parser::MAX_PARSE_DEPTH），
// 此上限只是遍历侧的额外保险，取足够大的值以覆盖正常程序规模。
constexpr size_t kMaxNodesVisited = 200000;

// 有界 DFS 遍历整棵 AST：调用每个节点的 children() 与 nodeName()。
// 目的：
//   1. 让优化器无法把「构建后立即析构」的 AST 整体消除（保证前端真的跑完）；
//   2. nodeName() 对字面量节点会构造 Value（NumberLiteral::nodeName →
//      getValue().toString()），顺带覆盖 Value/RefCounted/StringData 路径，
//      使 ASan/UBSan 能观察到 AST + Value 的构造与析构全链路。
void walkAst(ASTNode* root) {
    if (!root)
        return;
    std::vector<ASTNode*> stack;
    stack.push_back(root);
    size_t visited = 0;
    while (!stack.empty() && visited < kMaxNodesVisited) {
        ASTNode* node = stack.back();
        stack.pop_back();
        if (!node)
            continue;
        ++visited;
        // nodeName() 触发部分节点的 Value 构造（覆盖标量/字符串封装路径）。
        volatile size_t nameLen = node->nodeName().size();
        (void)nameLen;
        for (ASTNode* child : node->children()) {
            if (child && visited + stack.size() < kMaxNodesVisited)
                stack.push_back(child);
        }
    }
}

// 前端核心：把原始字节当作 MiniLang 源码，走完 scan + parse + 有界 AST 遍历。
// 保留内嵌 NUL：用 std::string(ptr, len) 构造，完整暴露 Lexer 的字节处理路径。
void runFrontEnd(const uint8_t* data, size_t size) {
    std::string source(reinterpret_cast<const char*>(data), size);

    // ---- 阶段 1：词法分析 ----
    Lexer lexer;
    std::vector<Token> tokens = lexer.scan(source);
    (void)lexer.getDiagnostics().hasErrors();
    (void)lexer.comments().size();

    // ---- 阶段 2：语法分析（含 parse 期宏展开）----
    Parser parser;
    std::unique_ptr<Block> ast = parser.parse(tokens);
    (void)parser.hasErrors();

    // ---- 阶段 3：有界遍历 AST，随后作用域结束触发整棵树析构 ----
    if (ast) {
        volatile size_t topLevel = ast->statements.size();
        (void)topLevel;
        walkAst(ast.get());
    }
    // ast 在此处析构：递归释放全部子节点 + 内嵌 Value，覆盖析构链路。
}

} // namespace

// ============================================================
// libFuzzer 入口（模式 A）
// ------------------------------------------------------------
// 返回值：libFuzzer 约定非 0 保留，恒返回 0。
// ============================================================
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    runFrontEnd(data, size);
    return 0;
}

// ============================================================
// Standalone 回放驱动（模式 B，仅在定义 MINILANG_FUZZER_STANDALONE 时编译）
// ------------------------------------------------------------
// 用法：
//   fuzz_parser_replay [路径...]
//     路径可为文件或目录；目录递归收集全部常规文件逐一回放。
//     无参数时运行一组内置边界字节样例作冒烟。
//   任一输入使前端崩溃 → 进程崩溃 → 调用方（CTest）判失败。
//   全部存活跑完 → 退出码 0。
// ============================================================
#ifdef MINILANG_FUZZER_STANDALONE

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace fs = std::filesystem;

// 读取整个文件为字节串（二进制，保留任意字节含 NUL）。
bool readFileBytes(const fs::path& path, std::string& out) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs)
        return false;
    std::string data((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    out.swap(data);
    return true;
}

// 回放单个文件；返回是否成功读取（读取失败不视为崩溃，仅告警）。
bool replayOneFile(const fs::path& path, size_t& runCount) {
    std::string bytes;
    if (!readFileBytes(path, bytes)) {
        std::fprintf(stderr, "[fuzz-replay] 警告：无法读取 %s\n", path.string().c_str());
        return false;
    }
    runFrontEnd(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    ++runCount;
    return true;
}

// 内置边界字节样例：无 corpus 时的冒烟集，覆盖 Lexer/Parser 典型脆弱点。
void runBuiltinSmoke(size_t& runCount) {
    static const std::string kSamples[] = {
        std::string(),                                              // 空输入
        std::string("\0\0\0\0", 4),                                 // 纯 NUL 字节
        std::string("\xff\xfe\x80\x81 invalid utf8"),               // 非法 UTF-8 首字节
        std::string("\"unterminated string"),                       // 未闭合字符串
        std::string("\"interp {1 + {2 + {3"),                       // 未闭合嵌套插值
        std::string("/* unterminated block comment"),               // 未闭合块注释
        std::string("var x = 999999999999999999999999999999999;"),  // 超大整数字面量
        std::string("1.7976931348623159e309 + .5e-"),               // 浮点上溢 + 残缺指数
        std::string("((((((((((((((((((((((((((((((1))))))))))))"),  // 深度嵌套括号
        std::string("fun f() { return f(); } f!();"),               // 宏调用形态 + 递归
        std::string("class A extends A with T { fun m() {} }"),     // 病态类/trait 头
        std::string("\\u{110000}\\xZZ \\ "),                        // 非法转义（越界码点/十六进制）
        std::string("match x { case _ => "),                        // 残缺 match
        std::string("import \"../../../etc/passwd\";"),             // 路径穿越（parse 期校验）
    };
    for (const std::string& s : kSamples) {
        runFrontEnd(reinterpret_cast<const uint8_t*>(s.data()), s.size());
        ++runCount;
    }
}

} // namespace

int main(int argc, char** argv) {
    size_t runCount = 0;
    size_t fileCount = 0;

    if (argc <= 1) {
        std::fprintf(stdout, "[fuzz-replay] 无输入路径，运行内置边界样例冒烟...\n");
        runBuiltinSmoke(runCount);
        std::fprintf(stdout, "[fuzz-replay] 冒烟完成：%zu 个样例全部存活。\n", runCount);
        return 0;
    }

    for (int i = 1; i < argc; ++i) {
        fs::path arg(argv[i]);
        std::error_code ec;
        if (fs::is_directory(arg, ec)) {
            // 递归收集目录下全部常规文件（含无扩展名/任意扩展名）。
            for (fs::recursive_directory_iterator it(arg, ec), end; it != end; it.increment(ec)) {
                if (ec) {
                    std::fprintf(stderr, "[fuzz-replay] 目录遍历错误：%s\n", ec.message().c_str());
                    break;
                }
                if (it->is_regular_file(ec)) {
                    if (replayOneFile(it->path(), runCount))
                        ++fileCount;
                }
            }
        } else if (fs::is_regular_file(arg, ec)) {
            if (replayOneFile(arg, runCount))
                ++fileCount;
        } else {
            std::fprintf(stderr, "[fuzz-replay] 跳过（非文件/目录）：%s\n", arg.string().c_str());
        }
    }

    // 附带跑一遍内置冒烟，保证即使 corpus 为空也有最小覆盖。
    runBuiltinSmoke(runCount);

    std::fprintf(stdout, "[fuzz-replay] 全部存活：回放 %zu 个语料文件 + 内置样例，共 %zu 次前端执行，无崩溃。\n",
                 fileCount, runCount);
    return 0;
}

#endif // MINILANG_FUZZER_STANDALONE
