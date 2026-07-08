// ============================================================
// MagicCommands.cpp — REPL %magic 命令系统实现（功能 11）
// ------------------------------------------------------------
// 命令分发：检测 % 前缀 → 解析命令名 → 路由到对应 handler
//
// 链接约束：仅调用 IdeController 的内联方法（lastTokens / astRoot /
// lastIR / lastCompileResult / vmReset / getVmGlobals / getVmStack /
// isVmInitialized），这些方法转发到 VmStepper / PipelineRunner 的内联
// 方法，最终解析到 minilang_core 符号。测试目标仅链接 minilang_core
// 即可满足链接依赖，无需 app/*.cpp。
// ============================================================

#include "gui/MagicCommands.h"
#include "gui/I18n.h" // mlTr() i18n 宏

// IdeController.h 包含 PipelineRunner.h / VmStepper.h 等头文件，
// 此处仅调用其内联方法（转发到 minilang_core 符号），不依赖 app/*.cpp 实现。
#include "app/IdeController.h"

#include "ast/ASTNode.h"
#include "compiler/Bytecode.h" // opCodeName / BytecodeChunk
#include "compiler/IR.h"       // formatIRInstruction / irOpName
#include "interpreter/Value.h" // Value::getType() / ValueType
#include "lexer/Token.h"

#include <sstream>
#include <string>
#include <unordered_map>

// ============================================================
// 命令元数据（静态数据，不依赖 IdeController）
// ============================================================

/// 返回全部已注册 magic 命令的元数据列表（静态数据）。
const std::vector<MagicCommand>& MagicCommands::commands() {
    static const std::vector<MagicCommand> kCommands = {
        {"help", "%help", "列出所有 magic 命令及简短描述"},
        {"disassemble", "%disassemble", "显示最近执行的字节码（同 BytecodeTracePanel）"},
        {"ir", "%ir", "显示当前输入/最近执行的 IR（同 IrViewer）"},
        {"compare", "%compare", "运行三后端对比（同 BackendComparePanel）"},
        {"profile", "%profile", "显示指令计数热点（同 ProfileDashboardPanel）"},
        {"memory", "%memory", "显示当前堆对象统计（同 MemoryModelPanel 第4子页）"},
        {"ast", "%ast", "显示最近执行的 AST（同 AstViewer）"},
        {"tokens", "%tokens", "显示最近的 Token 表（同 PipelineViewer Step 1）"},
        {"reset", "%reset", "重置 VM 状态（清除所有变量/函数）"},
        {"version", "%version", "显示 MiniLang 版本信息"},
    };
    return kCommands;
}

// ============================================================
// 辅助函数
// ============================================================

namespace {

/// 提取 magic 命令名：从 input 中找到第一个 % 后的 token
/// input 可能含前导空白。返回空字符串表示非 magic 命令。
std::string extractCommandName(const std::string& input) {
    size_t i = 0;
    while (i < input.size() && (input[i] == ' ' || input[i] == '\t' || input[i] == '\r' || input[i] == '\n')) {
        ++i;
    }
    if (i >= input.size() || input[i] != '%')
        return "";
    ++i; // 跳过 %
    size_t start = i;
    while (i < input.size() && input[i] != ' ' && input[i] != '\t' && input[i] != '\r' && input[i] != '\n') {
        ++i;
    }
    return input.substr(start, i - start);
}

/// Token 类型枚举 → 可读名称（用于 %tokens 输出）
const char* tokenTypeName(TokenType type) {
    switch (type) {
    case TokenType::TK_VAR:
        return "VAR";
    case TokenType::TK_FUN:
        return "FUN";
    case TokenType::TK_IF:
        return "IF";
    case TokenType::TK_ELSE:
        return "ELSE";
    case TokenType::TK_WHILE:
        return "WHILE";
    case TokenType::TK_FOR:
        return "FOR";
    case TokenType::TK_RETURN:
        return "RETURN";
    case TokenType::TK_TRUE:
        return "TRUE";
    case TokenType::TK_FALSE:
        return "FALSE";
    case TokenType::TK_AND:
        return "AND";
    case TokenType::TK_OR:
        return "OR";
    case TokenType::TK_NOT:
        return "NOT";
    case TokenType::TK_PRINT:
        return "PRINT";
    case TokenType::TK_BREAK:
        return "BREAK";
    case TokenType::TK_CONTINUE:
        return "CONTINUE";
    case TokenType::TK_TRY:
        return "TRY";
    case TokenType::TK_CATCH:
        return "CATCH";
    case TokenType::TK_THROW:
        return "THROW";
    case TokenType::TK_FINALLY:
        return "FINALLY";
    case TokenType::TK_IMPORT:
        return "IMPORT";
    case TokenType::TK_FROM:
        return "FROM";
    case TokenType::TK_EXPORT:
        return "EXPORT";
    case TokenType::TK_INT:
        return "INT";
    case TokenType::TK_FLOAT:
        return "FLOAT";
    case TokenType::TK_BOOL:
        return "BOOL";
    case TokenType::TK_STRING_TYPE:
        return "STRING_TYPE";
    case TokenType::TK_CLASS:
        return "CLASS";
    case TokenType::TK_EXTENDS:
        return "EXTENDS";
    case TokenType::TK_SUPER:
        return "SUPER";
    case TokenType::TK_NULL:
        return "NULL";
    case TokenType::TK_IDENTIFIER:
        return "IDENTIFIER";
    case TokenType::TK_INT_LIT:
        return "INT_LIT";
    case TokenType::TK_FLOAT_LIT:
        return "FLOAT_LIT";
    case TokenType::TK_STRING_LIT:
        return "STRING_LIT";
    case TokenType::TK_PLUS:
        return "PLUS";
    case TokenType::TK_MINUS:
        return "MINUS";
    case TokenType::TK_STAR:
        return "STAR";
    case TokenType::TK_SLASH:
        return "SLASH";
    case TokenType::TK_PERCENT:
        return "PERCENT";
    case TokenType::TK_EQ:
        return "EQ";
    case TokenType::TK_NEQ:
        return "NEQ";
    case TokenType::TK_LT:
        return "LT";
    case TokenType::TK_GT:
        return "GT";
    case TokenType::TK_LEQ:
        return "LEQ";
    case TokenType::TK_GEQ:
        return "GEQ";
    case TokenType::TK_ASSIGN:
        return "ASSIGN";
    case TokenType::TK_LPAREN:
        return "LPAREN";
    case TokenType::TK_RPAREN:
        return "RPAREN";
    case TokenType::TK_LBRACE:
        return "LBRACE";
    case TokenType::TK_RBRACE:
        return "RBRACE";
    case TokenType::TK_SEMICOLON:
        return "SEMICOLON";
    case TokenType::TK_COMMA:
        return "COMMA";
    case TokenType::TK_LBRACKET:
        return "LBRACKET";
    case TokenType::TK_RBRACKET:
        return "RBRACKET";
    case TokenType::TK_COLON:
        return "COLON";
    case TokenType::TK_DOT:
        return "DOT";
    case TokenType::TK_EOF:
        return "EOF";
    case TokenType::TK_ERROR:
        return "ERROR";
    case TokenType::TK_LINE_COMMENT:
        return "LINE_COMMENT";
    case TokenType::TK_BLOCK_COMMENT:
        return "BLOCK_COMMENT";
    case TokenType::TK_INTERP_START:
        return "INTERP_START";
    case TokenType::TK_INTERP_END:
        return "INTERP_END";
    case TokenType::TK_STRING_PART:
        return "STRING_PART";
    default:
        return "UNKNOWN";
    }
}

/// ValueType 枚举 → 可读名称
const char* valueTypeName(ValueType type) {
    switch (type) {
    case ValueType::VAL_NULL:
        return "null";
    case ValueType::VAL_INT:
        return "int";
    case ValueType::VAL_FLOAT:
        return "float";
    case ValueType::VAL_BOOL:
        return "bool";
    case ValueType::VAL_STRING:
        return "string";
    case ValueType::VAL_ARRAY:
        return "array";
    case ValueType::VAL_DICT:
        return "dict";
    case ValueType::VAL_INSTANCE:
        return "instance";
    case ValueType::VAL_CLOSURE:
        return "closure";
    default:
        return "unknown";
    }
}

/// 递归转储 AST 为缩进文本（参考 PipelineViewer::dumpAst）
void dumpAstNode(std::ostringstream& os, ASTNode* node, int depth, int maxDepth) {
    if (!node || depth > maxDepth)
        return;
    for (int i = 0; i < depth; ++i)
        os << "  ";
    os << node->nodeName();
    // 显示行号信息辅助定位
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
    os << std::string(50, '-') << "\n";
    for (const auto& cmd : MagicCommands::commands()) {
        os << cmd.syntax;
        // 对齐
        int padLen = 14 - static_cast<int>(cmd.syntax.size());
        if (padLen < 1)
            padLen = 1;
        os << std::string(padLen, ' ') << cmd.description << "\n";
    }
    os << std::string(50, '-') << "\n";
    os << mlTr("用法: 在 REPL 输入 %命令名，如 %help / %version / %ast").toStdString();
    return os.str();
}

std::string handleVersion() {
    std::ostringstream os;
    os << "MiniLang IDE v1.0\n";
    os << "MiniLang language v1.0\n";
    os << "Three backends: Interpreter / StackVM / RegisterVM";
    return os.str();
}

std::string handleDisassemble(IdeController* controller) {
    if (!controller) {
        return mlTr("[Error] IdeController 未设置，无法获取数据").toStdString();
    }
    const auto& result = controller->lastCompileResult();
    const auto& chunk = result.mainChunk;
    if (chunk.code.empty()) {
        return mlTr("[Info] 暂无数据，请先运行一段代码").toStdString();
    }
    std::ostringstream os;
    os << mlTr("=== 字节码反汇编 ===").toStdString() << "\n";
    os << "Chunk: " << chunk.name << "  ";
    os << "code.size=" << chunk.code.size() << "  ";
    os << "constants=" << chunk.constants.size() << "\n";
    os << std::string(40, '-') << "\n";

    // 简化反汇编：遍历字节码，显示 offset / opcode 名称 / 行号
    size_t offset = 0;
    int shown = 0;
    const int maxShow = 200; // 显示上限，避免超长输出
    while (offset < chunk.code.size() && shown < maxShow) {
        uint8_t byte = chunk.code[offset];
        OpCode op = static_cast<OpCode>(byte);
        int line = (offset < chunk.lines.size()) ? chunk.lines[offset] : 0;
        os << offset << ": " << opCodeName(op);
        if (line > 0)
            os << "  (line " << line << ")";
        os << "\n";
        ++offset;
        ++shown;
    }
    if (offset < chunk.code.size()) {
        os << "... (" << (chunk.code.size() - offset) << " more bytes)";
    }
    return os.str();
}

std::string handleIr(IdeController* controller) {
    if (!controller) {
        return mlTr("[Error] IdeController 未设置，无法获取数据").toStdString();
    }
    const IRFunction* ir = controller->lastIR();
    if (!ir) {
        return mlTr("[Info] 暂无数据，请先运行一段代码（需启用 IR 编译路径）").toStdString();
    }
    std::ostringstream os;
    os << mlTr("=== IR 中间表示 ===").toStdString() << "\n";
    os << "IRFunction \"" << ir->name << "\"  ";
    os << "blocks=" << ir->blocks.size() << "  ";
    os << "constants=" << ir->constants.size() << "  ";
    os << "globals=" << ir->globalNames.size() << "  ";
    os << "vregs=" << ir->nextVReg << "\n";
    os << std::string(40, '-') << "\n";

    int shown = 0;
    const int maxShow = 200;
    for (size_t bi = 0; bi < ir->blocks.size() && shown < maxShow; ++bi) {
        const auto& block = ir->blocks[bi];
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

std::string handleAst(IdeController* controller) {
    if (!controller) {
        return mlTr("[Error] IdeController 未设置，无法获取数据").toStdString();
    }
    Block* ast = controller->astRoot();
    if (!ast) {
        return mlTr("[Info] 暂无数据，请先运行一段代码").toStdString();
    }
    std::ostringstream os;
    os << mlTr("=== AST 抽象语法树 ===").toStdString() << "\n";
    os << "Root: " << ast->nodeName() << "  ";
    os << "line=" << ast->line << "  ";
    os << "children=" << ast->children().size() << "\n";
    os << std::string(40, '-') << "\n";
    dumpAstNode(os, ast, 0, 12);
    return os.str();
}

std::string handleTokens(IdeController* controller) {
    if (!controller) {
        return mlTr("[Error] IdeController 未设置，无法获取数据").toStdString();
    }
    const auto& tokens = controller->lastTokens();
    if (tokens.empty()) {
        return mlTr("[Info] 暂无数据，请先运行一段代码").toStdString();
    }
    std::ostringstream os;
    os << mlTr("=== Token 表 ===").toStdString() << "\n";
    os << std::string(60, '-') << "\n";
    os << "Idx  Type              Lexeme              Line:Col\n";
    os << std::string(60, '-') << "\n";
    int idx = 0;
    for (const auto& tok : tokens) {
        os << idx++;
        // 对齐
        os << (idx < 10 ? "    " : (idx < 100 ? "   " : "  "));
        os << tokenTypeName(tok.type);
        // type 字段对齐到 18 字符
        int typeLen = static_cast<int>(std::string(tokenTypeName(tok.type)).size());
        int typePad = (typeLen < 18) ? (18 - typeLen) : 1;
        os << std::string(typePad, ' ');
        // lexeme（截断过长内容）
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

std::string handleMemory(IdeController* controller) {
    if (!controller) {
        return mlTr("[Error] IdeController 未设置，无法获取数据").toStdString();
    }
    // 统计 VM 全局变量与操作数栈中的 Value 类型分布
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
    if (!controller) {
        return mlTr("[Error] IdeController 未设置，无法获取数据").toStdString();
    }
    // ProfileDashboardPanel 通过独立 VM 实例进行指令计数，无直接 IdeController API。
    // 返回提示信息引导用户打开对应面板。
    (void)controller;
    return mlTr("[Info] 指令计数热点需运行性能剖析，请打开 ProfileDashboardPanel 面板查看").toStdString();
}

std::string handleCompare(IdeController* controller) {
    if (!controller) {
        return mlTr("[Error] IdeController 未设置，无法获取数据").toStdString();
    }
    // BackendComparePanel 顺序运行三后端，无直接 IdeController API。
    // 返回提示信息引导用户打开对应面板。
    (void)controller;
    return mlTr("[Info] 三后端对比需运行 BackendComparePanel 面板，请在右侧面板中点击运行").toStdString();
}

std::string handleReset(IdeController* controller) {
    if (!controller) {
        return mlTr("[Error] IdeController 未设置，无法获取数据").toStdString();
    }
    controller->vmReset();
    return mlTr("[OK] VM 状态已重置（字节码执行状态清除，全局变量/函数保留需重启 IDE）").toStdString();
}

} // anonymous namespace

// ============================================================
// 主分发入口
// ============================================================

/// 处理一行 %magic 输入：解析命令并调用对应能力，返回输出文本。
std::string MagicCommands::handle(const std::string& input, IdeController* controller) {
    // 空/非 % 开头输入：不处理
    if (input.empty())
        return "";

    std::string cmd = extractCommandName(input);
    if (cmd.empty())
        return ""; // 非 magic 命令

    if (cmd == "help")
        return handleHelp();
    if (cmd == "version")
        return handleVersion();
    if (cmd == "disassemble")
        return handleDisassemble(controller);
    if (cmd == "ir")
        return handleIr(controller);
    if (cmd == "ast")
        return handleAst(controller);
    if (cmd == "tokens")
        return handleTokens(controller);
    if (cmd == "memory")
        return handleMemory(controller);
    if (cmd == "profile")
        return handleProfile(controller);
    if (cmd == "compare")
        return handleCompare(controller);
    if (cmd == "reset")
        return handleReset(controller);

    // 未知命令
    std::string result = mlTr("Unknown magic command:").toStdString();
    result += " " + cmd;
    return result;
}
