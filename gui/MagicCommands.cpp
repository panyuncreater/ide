// ============================================================
// MagicCommands.cpp — REPL %magic 命令系统实现（功能 11）
// ------------------------------------------------------------
// 命令分发：检测 % 前缀 → 解析命令名+参数 → 路由到对应 handler
//
// ROUND-69 修复：
//   - P1-1: magic 命令在 isInputComplete 之前拦截（ReplPanel.cpp），
//           避免 "%ast fun f() {" 因 { 未闭合而错误进入续行模式
//   - P1-2/3: 支持参数——带参数时对参数代码做 Lexer/Parser/Compiler 分析，
//             不带参数时沿用"最近一次执行结果"行为
//   - P2-4: handleTokens idx 列对齐修复（自增前 idx 判断空格数）
//   - P2-5: %reset 真正重置 REPL 环境（调用 resetReplEnvironment）
//
// 链接约束：仅调用 IdeController 的内联方法（转发到 minilang_core 符号），
// 以及直接使用 Lexer/Parser/Compiler（均在 minilang_core 中），
// 测试目标仅链接 minilang_core 即可满足链接依赖，无需 app/*.cpp。
// ============================================================

#include "gui/MagicCommands.h"
#include "gui/I18n.h"

#include "app/IdeController.h"

#include "ast/ASTNode.h"
#include "compiler/Bytecode.h"
#include "compiler/Compiler.h"
#include "compiler/IR.h"
#include "interpreter/Value.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>

// ============================================================
// 命令元数据（静态数据，不依赖 IdeController）
// ============================================================

const std::vector<MagicCommand>& MagicCommands::commands() {
    static const std::vector<MagicCommand> kCommands = {
        {"help", "%help", "列出所有 magic 命令及简短描述"},
        {"disassemble", "%disassemble [expr]", "字节码反汇编（带参数则编译参数代码）"},
        {"ir", "%ir [expr]", "显示 IR 中间表示（带参数则编译参数代码）"},
        {"compare", "%compare", "提示打开 BackendComparePanel（三后端对比）"},
        {"profile", "%profile", "提示打开 ProfileDashboardPanel（性能剖析）"},
        {"memory", "%memory", "显示当前堆对象统计（全局变量/操作数栈类型分布）"},
        {"ast", "%ast [expr]", "显示 AST（带参数则解析参数代码）"},
        {"tokens", "%tokens [expr]", "显示 Token 表（带参数则词法分析参数代码）"},
        {"reset", "%reset", "重置 REPL 环境（清除所有变量/函数/类/模块缓存）"},
        {"version", "%version", "显示 MiniLang 版本信息"},
    };
    return kCommands;
}

// ============================================================
// 辅助函数
// ============================================================

namespace {

struct ParsedCommand {
    std::string name;
    std::string arg;
};

ParsedCommand parseCommand(const std::string& input) {
    ParsedCommand result;
    size_t i = 0;
    while (i < input.size() && (input[i] == ' ' || input[i] == '\t' || input[i] == '\r' || input[i] == '\n')) {
        ++i;
    }
    if (i >= input.size() || input[i] != '%')
        return result;
    ++i;
    size_t nameStart = i;
    while (i < input.size() && input[i] != ' ' && input[i] != '\t' && input[i] != '\r' && input[i] != '\n') {
        ++i;
    }
    result.name = input.substr(nameStart, i - nameStart);
    while (i < input.size() && (input[i] == ' ' || input[i] == '\t')) {
        ++i;
    }
    if (i < input.size()) {
        result.arg = input.substr(i);
        while (!result.arg.empty() &&
               (result.arg.back() == ' ' || result.arg.back() == '\t' ||
                result.arg.back() == '\r' || result.arg.back() == '\n')) {
            result.arg.pop_back();
        }
    }
    return result;
}

struct ScopedAnalysis {
    bool ok = false;
    std::string errorMsg;
    std::vector<Token> tokens;
    std::unique_ptr<Block> ast;
    CompileResult compileResult;
    std::unique_ptr<IRFunction> irFunc;
};

ScopedAnalysis analyzeArg(const std::string& source) {
    ScopedAnalysis sa;
    Lexer lexer;
    try {
        sa.tokens = lexer.scan(source);
    } catch (const std::exception& e) {
        sa.errorMsg = std::string("词法错误: ") + e.what();
        return sa;
    }
    for (const auto& tok : sa.tokens) {
        if (tok.type == TokenType::TK_ERROR) {
            std::ostringstream os;
            os << "词法错误 (行 " << tok.line << ", 列 " << tok.column << "): " << tok.lexeme;
            sa.errorMsg = os.str();
            return sa;
        }
    }
    Parser parser;
    try {
        sa.ast = parser.parse(sa.tokens);
    } catch (const ParseError& e) {
        std::ostringstream os;
        os << "语法错误 (行 " << e.line << ", 列 " << e.column << "): " << e.what();
        sa.errorMsg = os.str();
        return sa;
    }
    if (!sa.ast) {
        sa.errorMsg = "语法错误: 解析返回空 AST";
        return sa;
    }
    sa.ok = true;
    return sa;
}

ScopedAnalysis analyzeArgWithCompile(const std::string& source, bool withIR) {
    ScopedAnalysis sa = analyzeArg(source);
    if (!sa.ok)
        return sa;
    Compiler compiler;
    if (withIR) {
        compiler.setUseIR(true);
    }
    try {
        sa.compileResult = compiler.compile(*sa.ast);
    } catch (const std::exception& e) {
        sa.errorMsg = std::string("编译错误: ") + e.what();
        sa.ok = false;
        return sa;
    }
    if (withIR) {
        const IRFunction* ir = compiler.getLastIR();
        if (ir) {
            sa.irFunc = std::make_unique<IRFunction>(*ir);
        }
    }
    return sa;
}

const char* tokenTypeName(TokenType type) {
    switch (type) {
    case TokenType::TK_VAR: return "VAR";
    case TokenType::TK_FUN: return "FUN";
    case TokenType::TK_IF: return "IF";
    case TokenType::TK_ELSE: return "ELSE";
    case TokenType::TK_WHILE: return "WHILE";
    case TokenType::TK_FOR: return "FOR";
    case TokenType::TK_RETURN: return "RETURN";
    case TokenType::TK_TRUE: return "TRUE";
    case TokenType::TK_FALSE: return "FALSE";
    case TokenType::TK_AND: return "AND";
    case TokenType::TK_OR: return "OR";
    case TokenType::TK_NOT: return "NOT";
    case TokenType::TK_PRINT: return "PRINT";
    case TokenType::TK_BREAK: return "BREAK";
    case TokenType::TK_CONTINUE: return "CONTINUE";
    case TokenType::TK_TRY: return "TRY";
    case TokenType::TK_CATCH: return "CATCH";
    case TokenType::TK_THROW: return "THROW";
    case TokenType::TK_FINALLY: return "FINALLY";
    case TokenType::TK_IMPORT: return "IMPORT";
    case TokenType::TK_FROM: return "FROM";
    case TokenType::TK_EXPORT: return "EXPORT";
    case TokenType::TK_INT: return "INT";
    case TokenType::TK_FLOAT: return "FLOAT";
    case TokenType::TK_BOOL: return "BOOL";
    case TokenType::TK_STRING_TYPE: return "STRING_TYPE";
    case TokenType::TK_CLASS: return "CLASS";
    case TokenType::TK_EXTENDS: return "EXTENDS";
    case TokenType::TK_SUPER: return "SUPER";
    case TokenType::TK_NULL: return "NULL";
    case TokenType::TK_IDENTIFIER: return "IDENTIFIER";
    case TokenType::TK_INT_LIT: return "INT_LIT";
    case TokenType::TK_FLOAT_LIT: return "FLOAT_LIT";
    case TokenType::TK_STRING_LIT: return "STRING_LIT";
    case TokenType::TK_PLUS: return "PLUS";
    case TokenType::TK_MINUS: return "MINUS";
    case TokenType::TK_STAR: return "STAR";
    case TokenType::TK_SLASH: return "SLASH";
    case TokenType::TK_PERCENT: return "PERCENT";
    case TokenType::TK_EQ: return "EQ";
    case TokenType::TK_NEQ: return "NEQ";
    case TokenType::TK_LT: return "LT";
    case TokenType::TK_GT: return "GT";
    case TokenType::TK_LEQ: return "LEQ";
    case TokenType::TK_GEQ: return "GEQ";
    case TokenType::TK_ASSIGN: return "ASSIGN";
    case TokenType::TK_LPAREN: return "LPAREN";
    case TokenType::TK_RPAREN: return "RPAREN";
    case TokenType::TK_LBRACE: return "LBRACE";
    case TokenType::TK_RBRACE: return "RBRACE";
    case TokenType::TK_SEMICOLON: return "SEMICOLON";
    case TokenType::TK_COMMA: return "COMMA";
    case TokenType::TK_LBRACKET: return "LBRACKET";
    case TokenType::TK_RBRACKET: return "RBRACKET";
    case TokenType::TK_COLON: return "COLON";
    case TokenType::TK_DOT: return "DOT";
    case TokenType::TK_EOF: return "EOF";
    case TokenType::TK_ERROR: return "ERROR";
    case TokenType::TK_LINE_COMMENT: return "LINE_COMMENT";
    case TokenType::TK_BLOCK_COMMENT: return "BLOCK_COMMENT";
    case TokenType::TK_INTERP_START: return "INTERP_START";
    case TokenType::TK_INTERP_END: return "INTERP_END";
    case TokenType::TK_STRING_PART: return "STRING_PART";
    default: return "UNKNOWN";
    }
}

const char* valueTypeName(ValueType type) {
    switch (type) {
    case ValueType::VAL_NULL: return "null";
    case ValueType::VAL_INT: return "int";
    case ValueType::VAL_FLOAT: return "float";
    case ValueType::VAL_BOOL: return "bool";
    case ValueType::VAL_STRING: return "string";
    case ValueType::VAL_ARRAY: return "array";
    case ValueType::VAL_DICT: return "dict";
    case ValueType::VAL_INSTANCE: return "instance";
    case ValueType::VAL_CLOSURE: return "closure";
    default: return "unknown";
    }
}

void dumpAstNode(std::ostringstream& os, ASTNode* node, int depth, int maxDepth) {
    if (!node || depth > maxDepth)
        return;
    for (int i = 0; i < depth; ++i)
        os << "  ";
    os << node->nodeName();
    if (node->line > 0) {
        os << "  [line " << node->line << "]";
    }
    os << "\n";
    auto children = node->children();
    for (auto* child : children) {
        dumpAstNode(os, child, depth + 1, maxDepth);
    }
}

// ---- handler 实现 ----

std::string handleHelp() {
    std::ostringstream os;
    os << mlTr("MiniLang REPL Magic 命令列表:").toStdString() << "\n";
    os << std::string(60, '-') << "\n";
    for (const auto& cmd : MagicCommands::commands()) {
        os << cmd.syntax;
        int padLen = 22 - static_cast<int>(cmd.syntax.size());
        if (padLen < 1)
            padLen = 1;
        os << std::string(padLen, ' ') << cmd.description << "\n";
    }
    os << std::string(60, '-') << "\n";
    os << mlTr("带 [expr] 参数的命令会在临时上下文中分析参数代码，").toStdString() << "\n";
    os << mlTr("不带参数则显示最近一次 Run/REPL 执行的结果。").toStdString();
    return os.str();
}

std::string handleVersion() {
    std::ostringstream os;
    os << "MiniLang IDE v1.0\n";
    os << "MiniLang language v1.0\n";
    os << "Three backends: Interpreter / StackVM / RegisterVM";
    return os.str();
}

std::string formatChunk(const BytecodeChunk& chunk, const std::string& title) {
    std::ostringstream os;
    os << title << "\n";
    os << "Chunk: " << chunk.name << "  ";
    os << "code.size=" << chunk.code.size() << "  ";
    os << "constants=" << chunk.constants.size() << "\n";
    os << std::string(40, '-') << "\n";
    size_t offset = 0;
    int shown = 0;
    const int maxShow = 200;
    while (offset < chunk.code.size() && shown < maxShow) {
        os << chunk.disassembleInstruction(offset);
        os << "\n";
        ++shown;
    }
    if (offset < chunk.code.size()) {
        os << "... (" << (chunk.code.size() - offset) << " more bytes)";
    }
    return os.str();
}

std::string handleDisassemble(IdeController* controller, const std::string& arg) {
    if (!arg.empty()) {
        ScopedAnalysis sa = analyzeArgWithCompile(arg, false);
        if (!sa.ok)
            return sa.errorMsg;
        std::ostringstream os;
        os << mlTr("=== 字节码反汇编（参数代码） ===").toStdString() << "\n";
        os << formatChunk(sa.compileResult.mainChunk, "");
        for (const auto& [name, ch] : sa.compileResult.functionChunks) {
            os << "\n" << formatChunk(ch, "");
        }
        return os.str();
    }
    if (!controller) {
        return mlTr("[Error] IdeController 未设置，无法获取数据。请提供表达式参数，如 %disassemble 1+2;").toStdString();
    }
    const auto& result = controller->lastCompileResult();
    const auto& chunk = result.mainChunk;
    if (chunk.code.empty()) {
        return mlTr("[Info] 暂无数据。用法: %disassemble <expr> 或先执行一段代码再运行 %disassemble").toStdString();
    }
    std::ostringstream os;
    os << mlTr("=== 字节码反汇编（最近编译结果） ===").toStdString() << "\n";
    os << formatChunk(chunk, "");
    return os.str();
}

std::string formatIR(const IRFunction& ir, const std::string& title) {
    std::ostringstream os;
    os << title << "\n";
    os << "IRFunction \"" << ir.name << "\"  ";
    os << "blocks=" << ir.blocks.size() << "  ";
    os << "constants=" << ir.constants.size() << "  ";
    os << "globals=" << ir.globalNames.size() << "  ";
    os << "vregs=" << ir.nextVReg << "\n";
    os << std::string(40, '-') << "\n";
    int shown = 0;
    const int maxShow = 200;
    for (size_t bi = 0; bi < ir.blocks.size() && shown < maxShow; ++bi) {
        const auto& block = ir.blocks[bi];
        os << "BB" << bi << " (label=" << block.labelIndex << "):\n";
        for (const auto& instr : block.instructions) {
            if (shown >= maxShow)
                break;
            os << "  " << formatIRInstruction(instr) << "\n";
            ++shown;
        }
    }
    return os.str();
}

std::string handleIr(IdeController* controller, const std::string& arg) {
    if (!arg.empty()) {
        ScopedAnalysis sa = analyzeArgWithCompile(arg, true);
        if (!sa.ok)
            return sa.errorMsg;
        if (!sa.irFunc) {
            return mlTr("[Error] IR 构建失败（编译器未返回 IRFunction）").toStdString();
        }
        std::ostringstream os;
        os << mlTr("=== IR 中间表示（参数代码） ===").toStdString() << "\n";
        os << formatIR(*sa.irFunc, "");
        return os.str();
    }
    if (!controller) {
        return mlTr("[Error] IdeController 未设置，无法获取数据。请提供表达式参数，如 %ir 1+2;").toStdString();
    }
    const IRFunction* ir = controller->lastIR();
    if (!ir) {
        return mlTr("[Info] 暂无数据。用法: %ir <expr> 或启用 IR 路径执行代码后再运行 %ir").toStdString();
    }
    std::ostringstream os;
    os << mlTr("=== IR 中间表示（最近 IR 编译结果） ===").toStdString() << "\n";
    os << formatIR(*ir, "");
    return os.str();
}

std::string formatAst(Block* ast, const std::string& title) {
    std::ostringstream os;
    os << title << "\n";
    os << "Root: " << ast->nodeName() << "  ";
    os << "line=" << ast->line << "  ";
    os << "children=" << ast->children().size() << "\n";
    os << std::string(40, '-') << "\n";
    dumpAstNode(os, ast, 0, 12);
    return os.str();
}

std::string handleAst(IdeController* controller, const std::string& arg) {
    if (!arg.empty()) {
        ScopedAnalysis sa = analyzeArg(arg);
        if (!sa.ok)
            return sa.errorMsg;
        std::ostringstream os;
        os << mlTr("=== AST 抽象语法树（参数代码） ===").toStdString() << "\n";
        os << formatAst(sa.ast.get(), "");
        return os.str();
    }
    if (!controller) {
        return mlTr("[Error] IdeController 未设置，无法获取数据。请提供表达式参数，如 %ast 1+2;").toStdString();
    }
    Block* ast = controller->astRoot();
    if (!ast) {
        return mlTr("[Info] 暂无数据。用法: %ast <expr> 或先执行一段代码再运行 %ast").toStdString();
    }
    std::ostringstream os;
    os << mlTr("=== AST 抽象语法树（最近解析结果） ===").toStdString() << "\n";
    os << formatAst(ast, "");
    return os.str();
}

std::string formatTokens(const std::vector<Token>& tokens, const std::string& title) {
    std::ostringstream os;
    os << title << "\n";
    os << std::string(60, '-') << "\n";
    os << "Idx  Type              Lexeme              Line:Col\n";
    os << std::string(60, '-') << "\n";
    for (int idx = 0; idx < static_cast<int>(tokens.size()); ++idx) {
        const auto& tok = tokens[idx];
        os << idx;
        if (idx < 10)
            os << "    ";
        else if (idx < 100)
            os << "   ";
        else
            os << "  ";
        const char* tname = tokenTypeName(tok.type);
        os << tname;
        int typeLen = static_cast<int>(std::string(tname).size());
        int typePad = (typeLen < 18) ? (18 - typeLen) : 1;
        os << std::string(typePad, ' ');
        std::string lex = tok.lexeme;
        if (lex.size() > 20)
            lex = lex.substr(0, 17) + "...";
        os << lex;
        int lexPad = (lex.size() < 20) ? static_cast<int>(20 - lex.size()) : 1;
        os << std::string(lexPad, ' ');
        os << tok.line << ":" << tok.column << "\n";
    }
    return os.str();
}

std::string handleTokens(IdeController* controller, const std::string& arg) {
    if (!arg.empty()) {
        ScopedAnalysis sa;
        Lexer lexer;
        try {
            sa.tokens = lexer.scan(arg);
        } catch (const std::exception& e) {
            return std::string("词法错误: ") + e.what();
        }
        std::ostringstream os;
        os << mlTr("=== Token 表（参数代码） ===").toStdString() << "\n";
        os << formatTokens(sa.tokens, "");
        return os.str();
    }
    if (!controller) {
        return mlTr("[Error] IdeController 未设置，无法获取数据。请提供表达式参数，如 %tokens 1+2;").toStdString();
    }
    const auto& tokens = controller->lastTokens();
    if (tokens.empty()) {
        return mlTr("[Info] 暂无数据。用法: %tokens <expr> 或先执行一段代码再运行 %tokens").toStdString();
    }
    std::ostringstream os;
    os << mlTr("=== Token 表（最近词法分析结果） ===").toStdString() << "\n";
    os << formatTokens(tokens, "");
    return os.str();
}

std::string handleMemory(IdeController* controller) {
    if (!controller) {
        return mlTr("[Error] IdeController 未设置，无法获取数据").toStdString();
    }
    auto globals = controller->getVmGlobals();
    auto stack = controller->getVmStack();
    if (globals.empty() && stack.empty()) {
        return mlTr("[Info] 暂无数据，请先运行一段代码").toStdString();
    }
    std::unordered_map<std::string, int> counts;
    auto countValue = [&counts](const Value& v) { counts[valueTypeName(v.getType())]++; };
    for (const auto& [key, val] : globals) {
        (void)key;
        countValue(val);
    }
    for (const auto& v : stack) {
        countValue(v);
    }
    std::ostringstream os;
    os << mlTr("=== 堆对象统计 ===").toStdString() << "\n";
    os << "Globals: " << globals.size() << "  Stack: " << stack.size() << "\n";
    os << std::string(40, '-') << "\n";
    os << "Type         Count\n";
    for (const auto& [name, cnt] : counts) {
        os << name;
        int pad = (name.size() < 12) ? static_cast<int>(12 - name.size()) : 1;
        os << std::string(pad, ' ') << cnt << "\n";
    }
    return os.str();
}

std::string handleProfile(IdeController* controller) {
    (void)controller;
    return mlTr("[Info] 指令计数热点需运行性能剖析，请打开 ProfileDashboardPanel 面板查看").toStdString();
}

std::string handleCompare(IdeController* controller) {
    (void)controller;
    return mlTr("[Info] 三后端对比需运行 BackendComparePanel 面板，请在右侧面板中点击运行").toStdString();
}

std::string handleReset(IdeController* controller) {
    if (!controller) {
        return mlTr("[Error] IdeController 未设置，无法重置 REPL 环境").toStdString();
    }
    controller->resetReplEnvironment();
    return mlTr("[OK] REPL 环境已重置（所有变量/函数/类/模块缓存已清除）").toStdString();
}

} // anonymous namespace

// ============================================================
// 主分发入口
// ============================================================

std::string MagicCommands::handle(const std::string& input, IdeController* controller) {
    if (input.empty())
        return "";

    ParsedCommand pc = parseCommand(input);
    if (pc.name.empty())
        return "";

    if (pc.name == "help")
        return handleHelp();
    if (pc.name == "version")
        return handleVersion();
    if (pc.name == "disassemble")
        return handleDisassemble(controller, pc.arg);
    if (pc.name == "ir")
        return handleIr(controller, pc.arg);
    if (pc.name == "ast")
        return handleAst(controller, pc.arg);
    if (pc.name == "tokens")
        return handleTokens(controller, pc.arg);
    if (pc.name == "memory")
        return handleMemory(controller);
    if (pc.name == "profile")
        return handleProfile(controller);
    if (pc.name == "compare")
        return handleCompare(controller);
    if (pc.name == "reset")
        return handleReset(controller);

    std::string result = mlTr("Unknown magic command:").toStdString();
    result += " " + pc.name;
    result += "\n" + mlTr("输入 %help 查看可用命令列表").toStdString();
    return result;
}
