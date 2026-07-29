// ============================================================
// cli/lsp_core.h - minilang-lsp 语言服务器核心逻辑（可测试）
// ------------------------------------------------------------
// 将 Lexer/Parser/Formatter/LintPass 封装为 Language Server Protocol
// 实现，支持任意编辑器（VS Code/Vim/Emacs）接入。
//
// 协议：JSON-RPC 2.0 over stdio（Content-Length 分帧）
// 位置：0-based（MiniLang 内部 1-based，转换在内部完成）
//
// 设计要点：
//   - JsonRpcTransport: stdio 读写 Content-Length 分帧消息
//   - TextDocumentManager: 文档状态管理（源码+AST+诊断+符号）
//   - SymbolIndex: AST 遍历收集声明（用于 definition/completion/documentSymbol）
//   - LspRequestHandler: 请求/通知分发与处理
//   - 核心逻辑与 main 分离，便于 gtest 单元测试（TestLspCli.cpp）
//
// 支持的 LSP 方法：
//   请求: initialize / shutdown / completion / hover / definition /
//         documentSymbol / formatting
//   通知: initialized / exit / textDocument/didOpen / didChange / didClose
//   发出: textDocument/publishDiagnostics
// ============================================================
#pragma once

#include "ast/ASTNode.h"
#include "common/Diagnostic.h"
#include "lexer/Token.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace minilang_lsp {

// ============================================================
// LSP 位置类型（0-based，与 LSP 协议一致）
// ============================================================

struct LspPosition {
    int line = 0;      // 0-based 行号
    int character = 0; // 0-based 列号（字符偏移）
};

struct LspRange {
    LspPosition start;
    LspPosition end;
};

struct LspLocation {
    std::string uri;
    LspRange range;
};

// ============================================================
// LSP 诊断
// ============================================================

enum class LspSeverity { Error = 1, Warning = 2, Information = 3, Hint = 4 };

struct LspDiagnostic {
    LspRange range;
    LspSeverity severity = LspSeverity::Error;
    std::string message;
    std::string source; // "minilang" / "lexer" / "parser" / "lint"
    std::string code;   // 诊断码（可选）
};

// ============================================================
// LSP 符号（documentSymbol）
// ============================================================

// 与 LSP SymbolKind 枚举值对齐
enum class LspSymbolKind {
    File = 1,
    Module = 2,
    Namespace = 3,
    Class = 5,
    Method = 6,
    ClassMember = 8, // LSP SymbolKind 8 = Field；命名避开 Windows/Qt 宏冲突
    Function = 12,
    Variable = 13,
    Constant = 14,
    Enum = 10,
    EnumMember = 22
};

struct LspSymbol {
    std::string name;
    LspSymbolKind kind = LspSymbolKind::Variable;
    LspRange range;
    LspRange selectionRange; // 名称所在范围
    std::string detail;      // 可选详情（如类型注解）
    std::vector<LspSymbol> children;
};

// ============================================================
// LSP 补全项
// ============================================================

// 与 LSP CompletionItemKind 枚举值对齐
enum class LspCompletionItemKind {
    Text = 1,
    Method = 2,
    Function = 3,
    Constructor = 4,
    FieldDecl = 5,
    Variable = 6,
    Class = 7,
    Keyword = 14,
    Enum = 13,
    EnumMember = 20,
    Constant = 21,
    Snippet = 15
};

struct LspCompletionItem {
    std::string label;
    LspCompletionItemKind kind = LspCompletionItemKind::Text;
    std::string detail; // 可选详情
};

// ============================================================
// 文档状态
// ============================================================

struct DocumentState {
    std::string uri;
    std::string source;
    std::vector<Token> tokens;
    std::shared_ptr<Block> ast; // 可能为 nullptr（解析失败）
    std::vector<LspDiagnostic> diagnostics;
    std::vector<LspSymbol> symbols;
    bool hasParseErrors = false;
};

// ============================================================
// 文档管理器
// ============================================================

class TextDocumentManager {
public:
    /// 处理 textDocument/didOpen
    void didOpen(const std::string& uri, const std::string& text);

    /// 处理 textDocument/didChange（全量替换模式）
    void didChange(const std::string& uri, const std::string& text);

    /// 处理 textDocument/didClose
    void didClose(const std::string& uri);

    /// 获取文档状态（不存在返回 nullptr）
    DocumentState* get(const std::string& uri);
    const DocumentState* get(const std::string& uri) const;

    /// 获取所有已打开文档的 URI
    std::vector<std::string> uris() const;

private:
    std::unordered_map<std::string, DocumentState> docs_;

    /// 重新解析文档（Lexer → Parser → 收集诊断 + 符号）
    void reparse(DocumentState& doc);
};

// ============================================================
// 符号索引
// ============================================================

class SymbolIndex {
public:
    /// 从 AST 构建符号表
    void build(Block& ast);

    /// 查找名称对应的声明位置（用于 definition）
    /// name: 要查找的标识符名称
    /// 返回声明位置（1-based MiniLang 位置），未找到返回 nullopt
    std::optional<LspPosition> findDefinition(const std::string& name) const;

    /// LSP 二期：查找名称的全部引用位置（VarRef/FunCall/Assignment 目标）。
    /// includeDeclaration=true 时同时包含声明位置。
    /// 返回 LSP 0-based 位置列表（按行/列排序去重）。
    std::vector<LspPosition> findReferences(const std::string& name, bool includeDeclaration) const;

    /// 获取所有顶层符号（用于 documentSymbol）
    const std::vector<LspSymbol>& symbols() const { return symbols_; }

    /// 获取补全列表（已声明的符号）
    std::vector<LspCompletionItem> completions() const;

    /// LSP 二期：声明信息（公开供 semanticTokens/signatureHelp 分类查询）
    struct DeclInfo {
        std::string name;
        LspSymbolKind kind;
        int line;   // 1-based
        int column; // 1-based
        std::string detail;
    };
    const std::vector<DeclInfo>& decls() const { return decls_; }

    /// LSP 二期：按名查声明（未找到返回 nullptr）
    const DeclInfo* findDecl(const std::string& name) const;

private:
    /// LSP 二期：引用位置（读/写使用点，不含声明）
    struct RefInfo {
        std::string name;
        int line;   // 1-based
        int column; // 1-based
    };

    std::vector<DeclInfo> decls_;
    std::vector<RefInfo> refs_;
    std::vector<LspSymbol> symbols_;

    /// 递归遍历 AST 收集声明（LSP 二期：同时收集引用）
    void collectDecls(ASTNode* node, bool inClass);

    /// 将 DeclInfo 转换为 LspSymbol（含 children）
    LspSymbol toSymbol(const DeclInfo& info) const;
};

// ============================================================
// 诊断收集器
// ============================================================

class DiagnosticCollector {
public:
    /// 从 Lexer + Parser 诊断生成 LSP 诊断
    static std::vector<LspDiagnostic> fromLexerParser(const DiagnosticBag& bag);

    /// 从 LintResult 诊断生成 LSP 诊断
    static std::vector<LspDiagnostic> fromLint(const DiagnosticBag& bag);
};

// ============================================================
// JSON-RPC 消息构建工具
// ============================================================

namespace json {

/// 构建 LSP Position JSON
QJsonObject position(int line, int character);

/// 构建 LSP Range JSON
QJsonObject range(int startLine, int startChar, int endLine, int endChar);

/// 构建 LSP Location JSON
QJsonObject location(const std::string& uri, int startLine, int startChar, int endLine, int endChar);

/// 构建 LSP Diagnostic JSON
QJsonObject diagnostic(const LspDiagnostic& diag);

/// 构建 LSP SymbolInformation / DocumentSymbol JSON
QJsonObject documentSymbol(const LspSymbol& sym);

/// 构建 LSP CompletionItem JSON
QJsonObject completionItem(const LspCompletionItem& item);

/// 构建 LSP TextEdit JSON
QJsonObject textEdit(int startLine, int startChar, int endLine, int endChar, const std::string& newText);

} // namespace json

// ============================================================
// JSON-RPC 传输层
// ============================================================

class JsonRpcTransport {
public:
    JsonRpcTransport();

    /// 从 stdin 读取一条 JSON-RPC 消息（阻塞）
    /// 返回 nullopt 表示 EOF 或读取错误
    std::optional<QJsonObject> read();

    /// 向 stdout 写入一条 JSON-RPC 消息
    void write(const QJsonObject& message);

    /// 向 stdout 写入一条 JSON-RPC 通知（无 id）
    void writeNotification(const std::string& method, const QJsonObject& params);

    /// 向 stdout 写入一条 JSON-RPC 响应
    void writeResponse(const QJsonValue& id, const QJsonObject& result);

    /// 向 stdout 写入一条 JSON-RPC 错误响应
    void writeError(const QJsonValue& id, int code, const std::string& message);

    // 用于测试：直接设置输入/输出缓冲区
    void setInput(const std::string& input) {
        inputBuffer_ = input;
        inputPos_ = 0;
        useBufferIO_ = true;
    }
    std::string outputBuffer() const { return outputBuffer_; }

private:
    std::string inputBuffer_;
    size_t inputPos_ = 0;
    std::string outputBuffer_;
    bool useBufferIO_ = false;

    /// 从 inputBuffer_ 读取一条消息（解析 Content-Length 分帧）
    std::optional<QJsonObject> readFromBuffer();
};

// ============================================================
// LSP 请求处理器
// ============================================================

class LspRequestHandler {
public:
    LspRequestHandler();

    /// 处理一条 JSON-RPC 消息
    /// 返回需要发送的响应（请求返回响应，通知可能返回需要发送的通知列表）
    /// 如果不需要发送任何消息，返回空 vector
    std::vector<QJsonObject> handleMessage(const QJsonObject& message);

    /// 是否已收到 shutdown 请求
    bool isShutdownRequested() const { return shutdownRequested_; }

    /// 是否应该退出（收到 exit 通知）
    bool shouldExit() const { return shouldExit_; }

private:
    TextDocumentManager docManager_;
    SymbolIndex symbolIndex_;
    bool shutdownRequested_ = false;
    bool shouldExit_ = false;
    bool initialized_ = false;

    // 请求处理（有 id，需返回 result）
    // 返回 QJsonValue 而非 QJsonObject，因为 formatting/documentSymbol 返回数组
    QJsonValue handleInitialize(const QJsonObject& params);
    QJsonValue handleCompletion(const QJsonObject& params);
    QJsonValue handleHover(const QJsonObject& params);
    QJsonValue handleDefinition(const QJsonObject& params);
    QJsonValue handleDocumentSymbol(const QJsonObject& params);
    QJsonValue handleFormatting(const QJsonObject& params);
    // LSP 二期：references / rename / signatureHelp / semanticTokens
    QJsonValue handleReferences(const QJsonObject& params);
    QJsonValue handleRename(const QJsonObject& params);
    QJsonValue handleSignatureHelp(const QJsonObject& params);
    QJsonValue handleSemanticTokens(const QJsonObject& params);
    QJsonValue handleCodeAction(const QJsonObject& params);   // LSP 二期：codeAction
    QJsonValue handleInlayHint(const QJsonObject& params);    // LSP 二期：inlayHint

    // 通知处理（无 id，可能返回需要发送的通知）
    std::vector<QJsonObject> handleInitialized();
    std::vector<QJsonObject> handleExit();
    std::vector<QJsonObject> handleDidOpen(const QJsonObject& params);
    std::vector<QJsonObject> handleDidChange(const QJsonObject& params);
    std::vector<QJsonObject> handleDidClose(const QJsonObject& params);

    /// 为指定文档生成 publishDiagnostics 通知
    QJsonObject makePublishDiagnostics(const std::string& uri, const std::vector<LspDiagnostic>& diags);

    /// 从消息中提取 textDocument.uri 和 position
    bool extractTextDocumentPosition(const QJsonObject& params, std::string& uri, int& line, int& character);

    /// 查找光标位置处的 token
    const Token* findTokenAt(const DocumentState& doc, int line, int character) const;

    /// 获取光标下的标识符名称
    std::optional<std::string> getIdentifierAt(const DocumentState& doc, int line, int character) const;
};

// ============================================================
// 位置转换工具
// ============================================================

/// MiniLang 1-based line → LSP 0-based line
inline int toLspLine(int minilangLine) {
    return minilangLine > 0 ? minilangLine - 1 : 0;
}

/// MiniLang 1-based column → LSP 0-based character
inline int toLspChar(int minilangColumn) {
    return minilangColumn > 0 ? minilangColumn - 1 : 0;
}

/// LSP 0-based line → MiniLang 1-based line
inline int fromLspLine(int lspLine) {
    return lspLine + 1;
}

/// LSP 0-based character → MiniLang 1-based column
inline int fromLspChar(int lspChar) {
    return lspChar + 1;
}

// ============================================================
// CLI 辅助函数
// ============================================================

/// 打印帮助信息到 stdout
void printHelp();

/// 打印版本信息到 stdout
void printVersion();

/// 返回 minilang-lsp 版本字符串
std::string versionString();

/// 返回 LSP 服务器名称
std::string serverName();

/// 运行 LSP 服务器主循环（供 lsp_entry.cpp 统一入口调用）
/// 返回退出码：0=正常退出，1=异常退出
int runLspServer();

} // namespace minilang_lsp
