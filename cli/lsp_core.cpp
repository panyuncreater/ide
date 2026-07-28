// ============================================================
// cli/lsp_core.cpp - minilang-lsp 语言服务器核心逻辑实现
// ------------------------------------------------------------
// 实现 JSON-RPC 传输层、文档管理、诊断生成、符号索引、请求处理器。
// 复用 Lexer/Parser/Formatter/LintPass 管线。
// ============================================================

#include "cli/lsp_core.h"

#include "common/Diagnostic.h"
#include "formatter/Formatter.h"
#include "lexer/Lexer.h"
#include "lint/LintPass.h"
#include "parser/Parser.h"

#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QTextStream>

#include <algorithm>
#include <cctype> // LSP 二期：rename 新名校验 isalpha/isalnum
#include <cstdio>
#include <cstring>
#include <sstream>
#include <unordered_map> // LSP 二期：semanticTokens 名称→类型映射

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace minilang_lsp {

// LSP 二期：semanticTokens 类型图例前向声明（定义在 handler 实现区，
// handleInitialize 构造 capabilities 时使用）
QJsonArray semanticTokenTypesLegend();

// ============================================================
// 位置转换工具（1-based MiniLang ↔ 0-based LSP）
// ============================================================
// toLspLine / toLspChar / fromLspLine / fromLspChar 在头文件中 inline 定义

// ============================================================
// JSON 构建工具
// ============================================================

namespace json {

QJsonObject position(int line, int character) {
    QJsonObject pos;
    pos["line"] = line;
    pos["character"] = character;
    return pos;
}

QJsonObject range(int startLine, int startChar, int endLine, int endChar) {
    QJsonObject r;
    r["start"] = position(startLine, startChar);
    r["end"] = position(endLine, endChar);
    return r;
}

QJsonObject location(const std::string& uri, int startLine, int startChar, int endLine, int endChar) {
    QJsonObject loc;
    loc["uri"] = QString::fromStdString(uri);
    loc["range"] = range(startLine, startChar, endLine, endChar);
    return loc;
}

QJsonObject diagnostic(const LspDiagnostic& diag) {
    QJsonObject d;
    d["range"] =
        range(diag.range.start.line, diag.range.start.character, diag.range.end.line, diag.range.end.character);
    d["severity"] = static_cast<int>(diag.severity);
    d["message"] = QString::fromStdString(diag.message);
    if (!diag.source.empty())
        d["source"] = QString::fromStdString(diag.source);
    if (!diag.code.empty())
        d["code"] = QString::fromStdString(diag.code);
    return d;
}

QJsonObject documentSymbol(const LspSymbol& sym) {
    QJsonObject s;
    s["name"] = QString::fromStdString(sym.name);
    s["kind"] = static_cast<int>(sym.kind);
    s["range"] = range(sym.range.start.line, sym.range.start.character, sym.range.end.line, sym.range.end.character);
    s["selectionRange"] = range(sym.selectionRange.start.line, sym.selectionRange.start.character,
                                sym.selectionRange.end.line, sym.selectionRange.end.character);
    if (!sym.detail.empty())
        s["detail"] = QString::fromStdString(sym.detail);
    if (!sym.children.empty()) {
        QJsonArray children;
        for (const auto& child : sym.children)
            children.append(documentSymbol(child));
        s["children"] = children;
    }
    return s;
}

QJsonObject completionItem(const LspCompletionItem& item) {
    QJsonObject c;
    c["label"] = QString::fromStdString(item.label);
    c["kind"] = static_cast<int>(item.kind);
    if (!item.detail.empty())
        c["detail"] = QString::fromStdString(item.detail);
    return c;
}

QJsonObject textEdit(int startLine, int startChar, int endLine, int endChar, const std::string& newText) {
    QJsonObject te;
    te["range"] = range(startLine, startChar, endLine, endChar);
    te["newText"] = QString::fromStdString(newText);
    return te;
}

} // namespace json

// ============================================================
// DiagnosticCollector 实现
// ============================================================

std::vector<LspDiagnostic> DiagnosticCollector::fromLexerParser(const DiagnosticBag& bag) {
    std::vector<LspDiagnostic> result;
    for (const auto& d : bag.all()) {
        if (d.level != DiagLevel::Error)
            continue; // 仅收集错误级别（Lexer/Parser 的 Info/Warning 不常见）
        LspDiagnostic ld;
        int line = d.line > 0 ? d.line : 1;
        int col = d.column > 0 ? d.column : 1;
        ld.range.start.line = toLspLine(line);
        ld.range.start.character = toLspChar(col);
        ld.range.end.line = toLspLine(line);
        ld.range.end.character = toLspChar(col) + 1; // 单字符范围
        ld.severity = LspSeverity::Error;
        ld.message = d.message;
        ld.source = d.sourceString();
        if (!d.code.empty())
            ld.code = d.code;
        result.push_back(std::move(ld));
    }
    return result;
}

std::vector<LspDiagnostic> DiagnosticCollector::fromLint(const DiagnosticBag& bag) {
    std::vector<LspDiagnostic> result;
    for (const auto& d : bag.all()) {
        LspDiagnostic ld;
        int line = d.line > 0 ? d.line : 1;
        int col = d.column > 0 ? d.column : 1;
        ld.range.start.line = toLspLine(line);
        ld.range.start.character = toLspChar(col);
        ld.range.end.line = toLspLine(line);
        ld.range.end.character = toLspChar(col) + 1;
        // 映射严重级别
        switch (d.level) {
        case DiagLevel::Error:
            ld.severity = LspSeverity::Error;
            break;
        case DiagLevel::Warning:
            ld.severity = LspSeverity::Warning;
            break;
        case DiagLevel::Info:
            ld.severity = LspSeverity::Information;
            break;
        case DiagLevel::Hint:
            ld.severity = LspSeverity::Hint;
            break;
        }
        ld.message = d.message;
        ld.source = "lint";
        if (!d.code.empty())
            ld.code = d.code;
        result.push_back(std::move(ld));
    }
    return result;
}

// ============================================================
// SymbolIndex 实现
// ============================================================

void SymbolIndex::build(Block& ast) {
    decls_.clear();
    refs_.clear();
    symbols_.clear();
    collectDecls(&ast, false);
    // 将扁平声明列表转换为符号列表（documentSymbol 用）
    for (const auto& decl : decls_) {
        // 跳过 enum variant（含 "." 的名称），它们作为 enum 的 children
        if (decl.name.find('.') != std::string::npos)
            continue;
        symbols_.push_back(toSymbol(decl));
    }
}

void SymbolIndex::collectDecls(ASTNode* node, bool inClass) {
    if (!node)
        return;

    switch (node->nodeType) {
    case NodeType::NODE_FUN_DECL: {
        auto* fun = static_cast<FunDecl*>(node);
        LspSymbolKind kind = inClass ? LspSymbolKind::Method : LspSymbolKind::Function;
        DeclInfo info;
        info.name = fun->name;
        info.kind = kind;
        info.line = fun->line;
        info.column = fun->column;
        // 构建详情：参数列表
        std::string detail = "(";
        for (size_t i = 0; i < fun->params.size(); ++i) {
            if (i > 0)
                detail += ", ";
            detail += fun->params[i];
            if (i < fun->paramTypes.size() && !fun->paramTypes[i].empty())
                detail += ": " + fun->paramTypes[i];
        }
        detail += ")";
        if (!fun->returnType.empty())
            detail += " -> " + fun->returnType;
        if (fun->isGenerator)
            detail = "fun*" + detail;
        info.detail = std::move(detail);
        decls_.push_back(info);
        // 递归遍历函数体（收集局部变量）
        if (fun->body)
            collectDecls(fun->body.get(), false);
        break;
    }
    case NodeType::NODE_CLASS_DECL: {
        auto* cls = static_cast<ClassDecl*>(node);
        DeclInfo info;
        info.name = cls->name;
        info.kind = LspSymbolKind::Class;
        info.line = cls->line;
        info.column = cls->column;
        if (!cls->superClassName.empty())
            info.detail = "extends " + cls->superClassName;
        decls_.push_back(info);
        // 遍历类成员
        for (auto& member : cls->members)
            collectDecls(member.get(), true);
        break;
    }
    case NodeType::NODE_VAR_DECL: {
        auto* var = static_cast<VarDecl*>(node);
        DeclInfo info;
        info.name = var->name;
        info.kind = inClass ? LspSymbolKind::ClassMember : LspSymbolKind::Variable;
        info.line = var->line;
        info.column = var->column;
        if (!var->typeAnnotation.empty())
            info.detail = ": " + var->typeAnnotation;
        decls_.push_back(info);
        // 递归遍历初始化表达式
        if (var->initializer)
            collectDecls(var->initializer.get(), inClass);
        break;
    }
    case NodeType::NODE_ENUM_DECL: {
        auto* en = static_cast<EnumDecl*>(node);
        DeclInfo info;
        info.name = en->name;
        info.kind = LspSymbolKind::Enum;
        info.line = en->line;
        info.column = en->column;
        if (!en->typeParams.empty()) {
            std::string tp = "<";
            for (size_t i = 0; i < en->typeParams.size(); ++i) {
                if (i > 0)
                    tp += ", ";
                tp += en->typeParams[i];
            }
            tp += ">";
            info.detail = std::move(tp);
        }
        decls_.push_back(info);
        // enum variants 作为子符号
        for (const auto& variant : en->variants) {
            DeclInfo vi;
            vi.name = en->name + "." + variant.name;
            vi.kind = LspSymbolKind::EnumMember;
            vi.line = en->line; // variant 没有独立行号，用 enum 的
            vi.column = en->column;
            decls_.push_back(vi);
        }
        break;
    }
    case NodeType::NODE_BLOCK: {
        auto* block = static_cast<Block*>(node);
        for (auto& stmt : block->statements)
            collectDecls(stmt.get(), inClass);
        break;
    }
    // ---- LSP 二期：引用收集（references/rename/semanticTokens 分类）----
    case NodeType::NODE_VAR_REF: {
        auto* ref = static_cast<VarRef*>(node);
        refs_.push_back(RefInfo{ref->name, ref->line, ref->column});
        break;
    }
    case NodeType::NODE_ASSIGNMENT: {
        // 赋值目标也是引用（写使用点）：rename 必须覆盖左侧名称
        auto* asg = static_cast<Assignment*>(node);
        refs_.push_back(RefInfo{asg->name, asg->line, asg->column});
        if (asg->value)
            collectDecls(asg->value.get(), inClass);
        break;
    }
    case NodeType::NODE_FUN_CALL: {
        // 直接名称调用 f(args)：name 非空且无 callee 时记录函数名引用
        auto* call = static_cast<FunCall*>(node);
        if (!call->name.empty() && !call->callee)
            refs_.push_back(RefInfo{call->name, call->line, call->column});
        for (auto& arg : call->arguments)
            collectDecls(arg.get(), inClass);
        if (call->callee)
            collectDecls(call->callee.get(), inClass);
        break;
    }
    default:
        // 其他节点类型：递归遍历子节点
        for (auto* child : node->children())
            collectDecls(child, inClass);
        break;
    }
}

std::optional<LspPosition> SymbolIndex::findDefinition(const std::string& name) const {
    for (const auto& decl : decls_) {
        if (decl.name == name) {
            LspPosition pos;
            pos.line = toLspLine(decl.line);
            pos.character = toLspChar(decl.column);
            return pos;
        }
    }
    return std::nullopt;
}

const SymbolIndex::DeclInfo* SymbolIndex::findDecl(const std::string& name) const {
    for (const auto& decl : decls_) {
        if (decl.name == name)
            return &decl;
    }
    return nullptr;
}

std::vector<LspPosition> SymbolIndex::findReferences(const std::string& name, bool includeDeclaration) const {
    // (line, character) 对集合去重（声明位置可能与引用重叠）
    std::vector<std::pair<int, int>> positions;
    if (includeDeclaration) {
        for (const auto& decl : decls_) {
            if (decl.name == name)
                positions.emplace_back(toLspLine(decl.line), toLspChar(decl.column));
        }
    }
    for (const auto& ref : refs_) {
        if (ref.name == name)
            positions.emplace_back(toLspLine(ref.line), toLspChar(ref.column));
    }
    std::sort(positions.begin(), positions.end());
    positions.erase(std::unique(positions.begin(), positions.end()), positions.end());
    std::vector<LspPosition> result;
    result.reserve(positions.size());
    for (const auto& [ln, ch] : positions) {
        LspPosition pos;
        pos.line = ln;
        pos.character = ch;
        result.push_back(pos);
    }
    return result;
}

std::vector<LspCompletionItem> SymbolIndex::completions() const {
    std::vector<LspCompletionItem> result;
    for (const auto& decl : decls_) {
        LspCompletionItem item;
        item.label = decl.name;
        // 简单名称（不含 "." 前缀）才作为补全项
        auto dotPos = decl.name.find('.');
        if (dotPos != std::string::npos)
            continue; // 跳过 enum variant（EnumName.VariantName 格式）
        switch (decl.kind) {
        case LspSymbolKind::Function:
            item.kind = LspCompletionItemKind::Function;
            break;
        case LspSymbolKind::Method:
            item.kind = LspCompletionItemKind::Method;
            break;
        case LspSymbolKind::Class:
            item.kind = LspCompletionItemKind::Class;
            break;
        case LspSymbolKind::Variable:
            item.kind = LspCompletionItemKind::Variable;
            break;
        case LspSymbolKind::ClassMember:
            item.kind = LspCompletionItemKind::FieldDecl;
            break;
        case LspSymbolKind::Enum:
            item.kind = LspCompletionItemKind::Enum;
            break;
        case LspSymbolKind::EnumMember:
            item.kind = LspCompletionItemKind::EnumMember;
            break;
        default:
            item.kind = LspCompletionItemKind::Variable;
            break;
        }
        item.detail = decl.detail;
        result.push_back(std::move(item));
    }
    return result;
}

LspSymbol SymbolIndex::toSymbol(const DeclInfo& info) const {
    LspSymbol sym;
    sym.name = info.name;
    sym.kind = info.kind;
    sym.range.start.line = toLspLine(info.line);
    sym.range.start.character = toLspChar(info.column);
    sym.range.end.line = toLspLine(info.line);
    sym.range.end.character = toLspChar(info.column) + static_cast<int>(info.name.length());
    sym.selectionRange = sym.range;
    sym.detail = info.detail;
    return sym;
}

// ============================================================
// TextDocumentManager 实现
// ============================================================

void TextDocumentManager::didOpen(const std::string& uri, const std::string& text) {
    DocumentState doc;
    doc.uri = uri;
    doc.source = text;
    reparse(doc);
    docs_[uri] = std::move(doc);
}

void TextDocumentManager::didChange(const std::string& uri, const std::string& text) {
    auto it = docs_.find(uri);
    if (it == docs_.end()) {
        didOpen(uri, text);
        return;
    }
    it->second.source = text;
    reparse(it->second);
}

void TextDocumentManager::didClose(const std::string& uri) {
    docs_.erase(uri);
}

DocumentState* TextDocumentManager::get(const std::string& uri) {
    auto it = docs_.find(uri);
    return it == docs_.end() ? nullptr : &it->second;
}

const DocumentState* TextDocumentManager::get(const std::string& uri) const {
    auto it = docs_.find(uri);
    return it == docs_.end() ? nullptr : &it->second;
}

std::vector<std::string> TextDocumentManager::uris() const {
    std::vector<std::string> result;
    result.reserve(docs_.size());
    for (const auto& [uri, _] : docs_)
        result.push_back(uri);
    return result;
}

void TextDocumentManager::reparse(DocumentState& doc) {
    doc.tokens.clear();
    doc.ast.reset();
    doc.diagnostics.clear();
    doc.symbols.clear();
    doc.hasParseErrors = false;

    // 1. Lexer
    Lexer lexer;
    doc.tokens = lexer.scan(doc.source);

    // 2. Parser
    Parser parser;
    doc.ast = parser.parse(doc.tokens);

    // 收集诊断
    std::vector<LspDiagnostic> diags;

    // Lexer 诊断
    auto lexDiags = DiagnosticCollector::fromLexerParser(lexer.getDiagnostics());
    diags.insert(diags.end(), lexDiags.begin(), lexDiags.end());

    // Parser 诊断
    auto parseDiags = DiagnosticCollector::fromLexerParser(parser.getDiagnostics());
    diags.insert(diags.end(), parseDiags.begin(), parseDiags.end());

    doc.hasParseErrors = parser.hasErrors();

    // 3. LintPass（仅当解析成功时）
    if (doc.ast && !doc.hasParseErrors) {
        minilang::lint::LintPass pass;
        auto lintResult = pass.analyze(*doc.ast);
        auto lintDiags = DiagnosticCollector::fromLint(lintResult.diagnostics);
        diags.insert(diags.end(), lintDiags.begin(), lintDiags.end());
    }

    doc.diagnostics = std::move(diags);

    // 4. 构建符号索引（仅当解析成功时）
    if (doc.ast && !doc.hasParseErrors) {
        SymbolIndex idx;
        idx.build(*doc.ast);
        // 将符号存入 DocumentState
        for (const auto& sym : idx.symbols())
            doc.symbols.push_back(sym);
    }
}

// ============================================================
// JsonRpcTransport 实现
// ============================================================

JsonRpcTransport::JsonRpcTransport() = default;

std::optional<QJsonObject> JsonRpcTransport::read() {
    // 如果有测试输入缓冲区，从中读取
    if (useBufferIO_) {
        return readFromBuffer();
    }

    // 从 stdin 读取 Content-Length 分帧
    std::string headers;
    std::string line;

    // 读取 header 行直到空行
    while (true) {
        int c = std::fgetc(stdin);
        if (c == EOF)
            return std::nullopt;
        headers.push_back(static_cast<char>(c));
        if (headers.size() >= 2 && headers[headers.size() - 2] == '\r' && headers[headers.size() - 1] == '\n') {
            // 检查是否是空行（\r\n\r\n）
            if (headers.size() >= 4 && headers[headers.size() - 4] == '\r' && headers[headers.size() - 3] == '\n') {
                break;
            }
        }
    }

    // 解析 Content-Length
    size_t contentLength = 0;
    {
        std::istringstream ss(headers);
        std::string headerLine;
        while (std::getline(ss, headerLine)) {
            // 去除 \r
            if (!headerLine.empty() && headerLine.back() == '\r')
                headerLine.pop_back();
            // 查找 "Content-Length:"
            const std::string key = "Content-Length:";
            if (headerLine.size() >= key.size() &&
                std::equal(key.begin(), key.end(), headerLine.begin(),
                           [](char a, char b) { return std::tolower(a) == std::tolower(b); })) {
                std::string val = headerLine.substr(key.size());
                // 去除前导空格
                size_t start = val.find_first_not_of(" \t");
                if (start != std::string::npos)
                    val = val.substr(start);
                contentLength = static_cast<size_t>(std::stoul(val));
            }
        }
    }

    if (contentLength == 0)
        return std::nullopt;

    // 读取 body
    std::string body(contentLength, '\0');
    if (std::fread(body.data(), 1, contentLength, stdin) != contentLength)
        return std::nullopt;

    // 解析 JSON
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(body), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return std::nullopt;

    return doc.object();
}

std::optional<QJsonObject> JsonRpcTransport::readFromBuffer() {
    // 查找 \r\n\r\n 分隔 header 和 body
    size_t headerEnd = inputBuffer_.find("\r\n\r\n", inputPos_);
    if (headerEnd == std::string::npos)
        return std::nullopt;

    std::string headers = inputBuffer_.substr(inputPos_, headerEnd - inputPos_);
    size_t bodyStart = headerEnd + 4;

    // 解析 Content-Length
    size_t contentLength = 0;
    {
        std::istringstream ss(headers);
        std::string headerLine;
        while (std::getline(ss, headerLine)) {
            if (!headerLine.empty() && headerLine.back() == '\r')
                headerLine.pop_back();
            const std::string key = "Content-Length:";
            if (headerLine.size() >= key.size() &&
                std::equal(key.begin(), key.end(), headerLine.begin(),
                           [](char a, char b) { return std::tolower(a) == std::tolower(b); })) {
                std::string val = headerLine.substr(key.size());
                size_t start = val.find_first_not_of(" \t");
                if (start != std::string::npos)
                    val = val.substr(start);
                contentLength = static_cast<size_t>(std::stoul(val));
            }
        }
    }

    if (contentLength == 0 || bodyStart + contentLength > inputBuffer_.size())
        return std::nullopt;

    std::string body = inputBuffer_.substr(bodyStart, contentLength);
    inputPos_ = bodyStart + contentLength;

    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(body), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return std::nullopt;

    return doc.object();
}

void JsonRpcTransport::write(const QJsonObject& message) {
    QByteArray body = QJsonDocument(message).toJson(QJsonDocument::Compact);
    std::string header = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";

    // 写入输出缓冲区（测试模式）或 stdout
    if (useBufferIO_) {
        outputBuffer_ += header;
        outputBuffer_ += body.toStdString();
    } else {
        std::fwrite(header.data(), 1, header.size(), stdout);
        std::fflush(stdout);
        std::fwrite(body.constData(), 1, body.size(), stdout);
        std::fflush(stdout);
    }
}

void JsonRpcTransport::writeNotification(const std::string& method, const QJsonObject& params) {
    QJsonObject msg;
    msg["jsonrpc"] = "2.0";
    msg["method"] = QString::fromStdString(method);
    msg["params"] = params;
    write(msg);
}

void JsonRpcTransport::writeResponse(const QJsonValue& id, const QJsonObject& result) {
    QJsonObject msg;
    msg["jsonrpc"] = "2.0";
    msg["id"] = id;
    msg["result"] = result;
    write(msg);
}

void JsonRpcTransport::writeError(const QJsonValue& id, int code, const std::string& message) {
    QJsonObject msg;
    msg["jsonrpc"] = "2.0";
    msg["id"] = id;
    QJsonObject errorObj;
    errorObj["code"] = code;
    errorObj["message"] = QString::fromStdString(message);
    msg["error"] = errorObj;
    write(msg);
}

// ============================================================
// LspRequestHandler 实现
// ============================================================

LspRequestHandler::LspRequestHandler() = default;

std::vector<QJsonObject> LspRequestHandler::handleMessage(const QJsonObject& message) {
    std::vector<QJsonObject> responses;

    QString method = message.value("method").toString();
    QJsonValue id = message.value("id");
    bool isRequest = message.contains("id") && !method.isEmpty();
    bool isNotification = !message.contains("id") && !method.isEmpty();

    if (isRequest) {
        QJsonObject params = message.value("params").toObject();
        QJsonValue result;

        if (method == "initialize") {
            result = handleInitialize(params);
        } else if (method == "shutdown") {
            shutdownRequested_ = true;
            result = QJsonValue(); // null result
        } else if (method == "textDocument/completion") {
            result = handleCompletion(params);
        } else if (method == "textDocument/hover") {
            result = handleHover(params);
        } else if (method == "textDocument/definition") {
            result = handleDefinition(params);
        } else if (method == "textDocument/documentSymbol") {
            result = handleDocumentSymbol(params);
        } else if (method == "textDocument/formatting") {
            result = handleFormatting(params);
        } else if (method == "textDocument/references") {
            result = handleReferences(params); // LSP 二期
        } else if (method == "textDocument/rename") {
            result = handleRename(params); // LSP 二期
        } else if (method == "textDocument/signatureHelp") {
            result = handleSignatureHelp(params); // LSP 二期
        } else if (method == "textDocument/semanticTokens/full") {
            result = handleSemanticTokens(params); // LSP 二期
        } else {
            // 未知方法：返回错误响应
            QJsonObject errorResponse;
            errorResponse["jsonrpc"] = "2.0";
            errorResponse["id"] = id;
            QJsonObject errorObj;
            errorObj["code"] = -32601;
            errorObj["message"] = QString::fromStdString("Method not found: " + method.toStdString());
            errorResponse["error"] = errorObj;
            responses.push_back(errorResponse);
            return responses;
        }

        // 构建响应
        QJsonObject response;
        response["jsonrpc"] = "2.0";
        response["id"] = id;
        response["result"] = result;
        responses.push_back(response);

    } else if (isNotification) {
        QJsonObject params = message.value("params").toObject();

        if (method == "initialized") {
            // 无需操作
        } else if (method == "exit") {
            shouldExit_ = true;
        } else if (method == "textDocument/didOpen") {
            auto notifs = handleDidOpen(params);
            for (auto& n : notifs)
                responses.push_back(n);
        } else if (method == "textDocument/didChange") {
            auto notifs = handleDidChange(params);
            for (auto& n : notifs)
                responses.push_back(n);
        } else if (method == "textDocument/didClose") {
            auto notifs = handleDidClose(params);
            for (auto& n : notifs)
                responses.push_back(n);
        }
        // 未知通知忽略
    }

    return responses;
}

QJsonValue LspRequestHandler::handleInitialize(const QJsonObject& /*params*/) {
    QJsonObject result;
    QJsonObject capabilities;

    // textDocumentSync: 1 = Full（每次 didChange 发送完整文本）
    capabilities["textDocumentSync"] = 1;

    // 补全提供者
    QJsonObject completionProvider;
    completionProvider["resolveProvider"] = false;
    capabilities["completionProvider"] = completionProvider;

    capabilities["hoverProvider"] = true;
    capabilities["definitionProvider"] = true;
    capabilities["documentSymbolProvider"] = true;
    capabilities["documentFormattingProvider"] = true;

    // LSP 二期：references / rename / signatureHelp / semanticTokens
    capabilities["referencesProvider"] = true;
    capabilities["renameProvider"] = true;
    QJsonObject signatureHelpProvider;
    QJsonArray triggerChars;
    triggerChars.append("(");
    triggerChars.append(",");
    signatureHelpProvider["triggerCharacters"] = triggerChars;
    capabilities["signatureHelpProvider"] = signatureHelpProvider;
    QJsonObject semanticTokensProvider;
    QJsonObject legend;
    legend["tokenTypes"] = semanticTokenTypesLegend();
    legend["tokenModifiers"] = QJsonArray();
    semanticTokensProvider["legend"] = legend;
    semanticTokensProvider["full"] = true;
    capabilities["semanticTokensProvider"] = semanticTokensProvider;

    result["capabilities"] = capabilities;

    QJsonObject serverInfo;
    serverInfo["name"] = QString::fromStdString(serverName());
    serverInfo["version"] = QString::fromStdString(versionString());
    result["serverInfo"] = serverInfo;

    return result;
}

QJsonValue LspRequestHandler::handleCompletion(const QJsonObject& params) {
    std::string uri;
    int line, character;
    if (!extractTextDocumentPosition(params, uri, line, character))
        return QJsonValue(); // null

    // 返回 CompletionList
    QJsonObject result;
    result["isIncomplete"] = false;

    QJsonArray items;

    // 1. MiniLang 关键字补全
    static const std::vector<std::pair<std::string, std::string>> keywords = {
        {"var", "变量声明"},    {"fun", "函数声明"},      {"fun*", "生成器函数声明"}, {"if", "条件语句"},
        {"else", "否则分支"},   {"while", "while 循环"},  {"for", "for 循环"},        {"return", "返回语句"},
        {"break", "跳出循环"},  {"continue", "继续循环"}, {"print", "打印输出"},      {"true", "布尔真"},
        {"false", "布尔假"},    {"null", "空值"},         {"and", "逻辑与（短路）"},  {"or", "逻辑或（短路）"},
        {"not", "逻辑非"},      {"class", "类声明"},      {"extends", "继承"},        {"super", "父类调用"},
        {"import", "导入模块"}, {"from", "从模块导入"},   {"export", "导出符号"},     {"try", "异常捕获"},
        {"catch", "异常处理"},  {"finally", "最终执行"},  {"throw", "抛出异常"},      {"enum", "枚举声明"},
        {"match", "模式匹配"},  {"case", "匹配分支"},     {"yield", "生成器产出"},    {"int", "整数类型"},
        {"float", "浮点类型"},  {"bool", "布尔类型"},     {"string", "字符串类型"},   {"dict", "字典类型"},
        {"array", "数组类型"},
    };

    for (const auto& [kw, desc] : keywords) {
        QJsonObject item;
        item["label"] = QString::fromStdString(kw);
        item["kind"] = static_cast<int>(LspCompletionItemKind::Keyword);
        item["detail"] = QString::fromStdString(desc);
        items.append(item);
    }

    // 2. 已声明符号补全
    auto* doc = docManager_.get(uri);
    if (doc && doc->ast && !doc->hasParseErrors) {
        SymbolIndex idx;
        idx.build(*doc->ast);
        for (const auto& item : idx.completions()) {
            items.append(json::completionItem(item));
        }
    }

    result["items"] = items;
    return result;
}

QJsonValue LspRequestHandler::handleHover(const QJsonObject& params) {
    std::string uri;
    int line, character;
    if (!extractTextDocumentPosition(params, uri, line, character))
        return QJsonValue(); // null

    auto* doc = docManager_.get(uri);
    if (!doc)
        return QJsonValue();

    // 查找光标下的标识符
    auto idName = getIdentifierAt(*doc, line, character);
    if (!idName) {
        // 非标识符 token：检查是否是关键字
        const Token* tok = findTokenAt(*doc, line, character);
        if (tok && !tok->lexeme.empty()) {
            auto& kwMap = Lexer::keywords();
            if (kwMap.find(tok->lexeme) != kwMap.end()) {
                QJsonObject result;
                QJsonObject contents;
                contents["kind"] = "plaintext";
                contents["value"] = QString::fromStdString(tok->lexeme + " (关键字)");
                result["contents"] = contents;
                return result;
            }
        }
        return QJsonValue();
    }

    // 查找声明信息
    if (doc->ast && !doc->hasParseErrors) {
        SymbolIndex idx;
        idx.build(*doc->ast);
        auto pos = idx.findDefinition(*idName);
        if (pos) {
            // 查找详情
            for (const auto& decl : idx.completions()) {
                if (decl.label == *idName) {
                    QJsonObject result;
                    QJsonObject contents;
                    contents["kind"] = "markdown";
                    std::string hoverText = "**" + *idName + "**";
                    if (!decl.detail.empty())
                        hoverText += "  \n`" + decl.detail + "`";
                    contents["value"] = QString::fromStdString(hoverText);
                    result["contents"] = contents;
                    return result;
                }
            }
        }
    }

    // 检查是否是关键字
    auto& kwMap = Lexer::keywords();
    if (kwMap.find(*idName) != kwMap.end()) {
        QJsonObject result;
        QJsonObject contents;
        contents["kind"] = "plaintext";
        contents["value"] = QString::fromStdString(*idName + " (关键字)");
        result["contents"] = contents;
        return result;
    }

    return QJsonValue(); // null
}

QJsonValue LspRequestHandler::handleDefinition(const QJsonObject& params) {
    std::string uri;
    int line, character;
    if (!extractTextDocumentPosition(params, uri, line, character))
        return QJsonValue(); // null

    auto* doc = docManager_.get(uri);
    if (!doc || !doc->ast || doc->hasParseErrors)
        return QJsonValue();

    auto idName = getIdentifierAt(*doc, line, character);
    if (!idName)
        return QJsonValue();

    SymbolIndex idx;
    idx.build(*doc->ast);
    auto pos = idx.findDefinition(*idName);
    if (!pos)
        return QJsonValue();

    // 返回 Location
    return json::location(uri, pos->line, pos->character, pos->line,
                          pos->character + static_cast<int>(idName->length()));
}

QJsonValue LspRequestHandler::handleDocumentSymbol(const QJsonObject& params) {
    std::string uri = params.value("textDocument").toObject().value("uri").toString().toStdString();
    auto* doc = docManager_.get(uri);
    if (!doc || !doc->ast || doc->hasParseErrors)
        return QJsonArray(); // 空数组

    SymbolIndex idx;
    idx.build(*doc->ast);

    QJsonArray symbols;
    for (const auto& sym : idx.symbols())
        symbols.append(json::documentSymbol(sym));

    return symbols;
}

QJsonValue LspRequestHandler::handleFormatting(const QJsonObject& params) {
    std::string uri = params.value("textDocument").toObject().value("uri").toString().toStdString();
    auto* doc = docManager_.get(uri);
    if (!doc || doc->source.empty())
        return QJsonArray(); // 空数组

    // 重新 Lexer → Parser → Formatter
    Lexer lexer;
    auto tokens = lexer.scan(doc->source);
    if (lexer.getDiagnostics().hasErrors())
        return QJsonArray();

    Parser parser;
    auto ast = parser.parse(tokens);
    if (parser.hasErrors() || !ast)
        return QJsonArray();

    Formatter fmt;
    fmt.setComments(lexer.comments());
    std::string formatted;
    try {
        formatted = fmt.format(*ast);
    } catch (...) {
        return QJsonArray();
    }

    if (formatted == doc->source)
        return QJsonArray(); // 无需格式化

    // 计算文档行数（用于全文档替换）
    int lineCount = 0;
    for (char c : doc->source)
        if (c == '\n')
            ++lineCount;

    // 返回一个 TextEdit 替换整个文档
    QJsonArray edits;
    edits.append(json::textEdit(0, 0, lineCount, 0, formatted));
    return edits;
}

// ============================================================
// LSP 二期：references / rename / signatureHelp / semanticTokens
// ============================================================

/// semanticTokens 类型图例（initialize 与 handleSemanticTokens 共用，
/// 索引即编码值）：0=keyword 1=function 2=class 3=variable 4=number
/// 5=string 6=enum 7=enumMember
QJsonArray semanticTokenTypesLegend() {
    QJsonArray types;
    types.append("keyword");
    types.append("function");
    types.append("class");
    types.append("variable");
    types.append("number");
    types.append("string");
    types.append("enum");
    types.append("enumMember");
    return types;
}

QJsonValue LspRequestHandler::handleReferences(const QJsonObject& params) {
    std::string uri;
    int line, character;
    if (!extractTextDocumentPosition(params, uri, line, character))
        return QJsonValue();
    auto* doc = docManager_.get(uri);
    if (!doc || !doc->ast || doc->hasParseErrors)
        return QJsonValue();
    auto idName = getIdentifierAt(*doc, line, character);
    if (!idName)
        return QJsonValue();
    // context.includeDeclaration 缺省 true（LSP 客户端通常显式传）
    bool includeDecl = true;
    if (params.contains("context"))
        includeDecl = params.value("context").toObject().value("includeDeclaration").toBool(true);

    SymbolIndex idx;
    idx.build(*doc->ast);
    auto positions = idx.findReferences(*idName, includeDecl);
    if (positions.empty())
        return QJsonValue();

    QJsonArray locations;
    int len = static_cast<int>(idName->length());
    for (const auto& pos : positions) {
        locations.append(json::location(uri, pos.line, pos.character, pos.line, pos.character + len));
    }
    return locations;
}

QJsonValue LspRequestHandler::handleRename(const QJsonObject& params) {
    std::string uri;
    int line, character;
    if (!extractTextDocumentPosition(params, uri, line, character))
        return QJsonValue();
    auto* doc = docManager_.get(uri);
    if (!doc || !doc->ast || doc->hasParseErrors)
        return QJsonValue();
    auto idName = getIdentifierAt(*doc, line, character);
    if (!idName)
        return QJsonValue();

    std::string newName = params.value("newName").toString().toStdString();
    // 新名校验：合法标识符且非关键字（避免重命名后源码不可解析）
    auto isValidIdentifier = [](const std::string& s) {
        if (s.empty())
            return false;
        if (!(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_'))
            return false;
        for (char c : s) {
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
                return false;
        }
        return true;
    };
    if (!isValidIdentifier(newName) || Lexer::keywords().count(newName) > 0)
        return QJsonValue();

    SymbolIndex idx;
    idx.build(*doc->ast);
    auto positions = idx.findReferences(*idName, /*includeDeclaration=*/true);
    if (positions.empty())
        return QJsonValue();

    QJsonArray edits;
    int oldLen = static_cast<int>(idName->length());
    for (const auto& pos : positions) {
        edits.append(json::textEdit(pos.line, pos.character, pos.line, pos.character + oldLen, newName));
    }
    QJsonObject changes;
    changes[QString::fromStdString(uri)] = edits;
    QJsonObject workspaceEdit;
    workspaceEdit["changes"] = changes;
    return workspaceEdit;
}

QJsonValue LspRequestHandler::handleSignatureHelp(const QJsonObject& params) {
    std::string uri;
    int line, character;
    if (!extractTextDocumentPosition(params, uri, line, character))
        return QJsonValue();
    auto* doc = docManager_.get(uri);
    if (!doc || !doc->ast || doc->hasParseErrors)
        return QJsonValue();

    // 从光标向前扫 tokens：找到当前未闭合的 '(' 及其前的函数名，
    // 同时统计同层逗号数作为 activeParameter。
    int targetLine = fromLspLine(line);
    int targetCol = fromLspChar(character);
    int depth = 0;
    int activeParam = 0;
    const Token* funTok = nullptr;
    for (int i = static_cast<int>(doc->tokens.size()) - 1; i >= 0; --i) {
        const Token& t = doc->tokens[i];
        // 仅考虑光标之前的 token
        if (t.line > targetLine || (t.line == targetLine && t.column >= targetCol))
            continue;
        if (t.type == TokenType::TK_RPAREN) {
            ++depth;
        } else if (t.type == TokenType::TK_LPAREN) {
            if (depth == 0) {
                if (i > 0 && doc->tokens[static_cast<size_t>(i) - 1].type == TokenType::TK_IDENTIFIER)
                    funTok = &doc->tokens[static_cast<size_t>(i) - 1];
                break;
            }
            --depth;
        } else if (t.type == TokenType::TK_COMMA && depth == 0) {
            ++activeParam;
        }
    }
    if (!funTok)
        return QJsonValue();

    SymbolIndex idx;
    idx.build(*doc->ast);
    const auto* decl = idx.findDecl(funTok->lexeme);
    if (!decl || (decl->kind != LspSymbolKind::Function && decl->kind != LspSymbolKind::Method))
        return QJsonValue();

    // 签名 label = 名称 + detail（如 "add(a: int, b: int) -> int"）；
    // 参数列表从 detail 括号内按顶层逗号拆分
    QJsonObject signature;
    signature["label"] = QString::fromStdString(decl->name + decl->detail);
    QJsonArray paramInfos;
    auto lp = decl->detail.find('(');
    auto rp = decl->detail.rfind(')');
    if (lp != std::string::npos && rp != std::string::npos && rp > lp + 1) {
        std::string inner = decl->detail.substr(lp + 1, rp - lp - 1);
        size_t start = 0;
        while (start <= inner.size()) {
            size_t comma = inner.find(',', start);
            std::string piece =
                (comma == std::string::npos) ? inner.substr(start) : inner.substr(start, comma - start);
            // trim
            size_t b = piece.find_first_not_of(' ');
            size_t e = piece.find_last_not_of(' ');
            if (b != std::string::npos) {
                QJsonObject pi;
                pi["label"] = QString::fromStdString(piece.substr(b, e - b + 1));
                paramInfos.append(pi);
            }
            if (comma == std::string::npos)
                break;
            start = comma + 1;
        }
    }
    signature["parameters"] = paramInfos;

    QJsonObject result;
    QJsonArray signatures;
    signatures.append(signature);
    result["signatures"] = signatures;
    result["activeSignature"] = 0;
    result["activeParameter"] = activeParam;
    return result;
}

QJsonValue LspRequestHandler::handleSemanticTokens(const QJsonObject& params) {
    std::string uri = params.value("textDocument").toObject().value("uri").toString().toStdString();
    auto* doc = docManager_.get(uri);
    if (!doc)
        return QJsonValue();

    // 标识符分类表：从 SymbolIndex 声明构建 name→类型索引（解析失败时
    // 仅关键字/字面量着色，标识符默认 variable）
    std::unordered_map<std::string, int> nameToType;
    if (doc->ast && !doc->hasParseErrors) {
        SymbolIndex idx;
        idx.build(*doc->ast);
        for (const auto& decl : idx.decls()) {
            int type = 3; // variable
            switch (decl.kind) {
            case LspSymbolKind::Function:
            case LspSymbolKind::Method:
                type = 1;
                break;
            case LspSymbolKind::Class:
                type = 2;
                break;
            case LspSymbolKind::Enum:
                type = 6;
                break;
            case LspSymbolKind::EnumMember:
                type = 7;
                break;
            default:
                type = 3;
                break;
            }
            // enum variant 名含 "."：取最后一段作为标识符分类键
            auto dotPos = decl.name.rfind('.');
            std::string key = (dotPos == std::string::npos) ? decl.name : decl.name.substr(dotPos + 1);
            nameToType.emplace(key, type);
        }
    }

    const auto& kwMap = Lexer::keywords();
    // LSP semanticTokens 差分编码：每 token 5 元组
    // (deltaLine, deltaStartChar, length, tokenType, tokenModifiers)
    QJsonArray data;
    int prevLine = 0;
    int prevChar = 0;
    for (const auto& tok : doc->tokens) {
        if (tok.lexeme.empty())
            continue;
        int type = -1;
        if (kwMap.find(tok.lexeme) != kwMap.end()) {
            type = 0; // keyword
        } else if (tok.type == TokenType::TK_IDENTIFIER) {
            auto it = nameToType.find(tok.lexeme);
            type = (it != nameToType.end()) ? it->second : 3;
        } else if (tok.type == TokenType::TK_INT_LIT || tok.type == TokenType::TK_FLOAT_LIT) {
            type = 4; // number
        } else if (tok.type == TokenType::TK_STRING_LIT || tok.type == TokenType::TK_STRING_PART) {
            type = 5; // string
        }
        if (type < 0)
            continue;
        int tokLine = toLspLine(tok.line);
        int tokChar = toLspChar(tok.column);
        int deltaLine = tokLine - prevLine;
        int deltaChar = (deltaLine == 0) ? tokChar - prevChar : tokChar;
        if (deltaLine < 0 || deltaChar < 0)
            continue; // 乱序 token（插值字符串片段等）保守跳过
        data.append(deltaLine);
        data.append(deltaChar);
        data.append(static_cast<int>(tok.lexeme.size()));
        data.append(type);
        data.append(0);
        prevLine = tokLine;
        prevChar = tokChar;
    }

    QJsonObject result;
    result["data"] = data;
    return result;
}

std::vector<QJsonObject> LspRequestHandler::handleDidOpen(const QJsonObject& params) {
    std::vector<QJsonObject> responses;

    std::string uri = params.value("textDocument").toObject().value("uri").toString().toStdString();
    std::string text = params.value("textDocument").toObject().value("text").toString().toStdString();

    docManager_.didOpen(uri, text);

    // 发送 publishDiagnostics
    auto* doc = docManager_.get(uri);
    if (doc) {
        responses.push_back(makePublishDiagnostics(uri, doc->diagnostics));
    }

    return responses;
}

std::vector<QJsonObject> LspRequestHandler::handleDidChange(const QJsonObject& params) {
    std::vector<QJsonObject> responses;

    std::string uri = params.value("textDocument").toObject().value("uri").toString().toStdString();

    // 全量替换模式：contentChanges[0].text
    QJsonArray changes = params.value("contentChanges").toArray();
    if (changes.isEmpty())
        return responses;

    std::string text = changes.at(0).toObject().value("text").toString().toStdString();
    docManager_.didChange(uri, text);

    // 发送 publishDiagnostics
    auto* doc = docManager_.get(uri);
    if (doc) {
        responses.push_back(makePublishDiagnostics(uri, doc->diagnostics));
    }

    return responses;
}

std::vector<QJsonObject> LspRequestHandler::handleDidClose(const QJsonObject& params) {
    std::string uri = params.value("textDocument").toObject().value("uri").toString().toStdString();

    // 发送空诊断清除标记
    auto responses = std::vector<QJsonObject>();
    responses.push_back(makePublishDiagnostics(uri, {}));

    docManager_.didClose(uri);
    return responses;
}

QJsonObject LspRequestHandler::makePublishDiagnostics(const std::string& uri, const std::vector<LspDiagnostic>& diags) {
    QJsonObject params;
    params["uri"] = QString::fromStdString(uri);

    QJsonArray diagArray;
    for (const auto& d : diags)
        diagArray.append(json::diagnostic(d));
    params["diagnostics"] = diagArray;

    QJsonObject notification;
    notification["jsonrpc"] = "2.0";
    notification["method"] = "textDocument/publishDiagnostics";
    notification["params"] = params;
    return notification;
}

bool LspRequestHandler::extractTextDocumentPosition(const QJsonObject& params, std::string& uri, int& line,
                                                    int& character) {
    QJsonObject textDoc = params.value("textDocument").toObject();
    uri = textDoc.value("uri").toString().toStdString();
    if (uri.empty())
        return false;

    QJsonObject pos = params.value("position").toObject();
    line = pos.value("line").toInt(0);
    character = pos.value("character").toInt(0);
    return true;
}

const Token* LspRequestHandler::findTokenAt(const DocumentState& doc, int line, int character) const {
    // LSP line/character 是 0-based，Token line/column 是 1-based
    int targetLine = fromLspLine(line);
    int targetCol = fromLspChar(character);

    for (const auto& token : doc.tokens) {
        if (token.line != targetLine)
            continue;
        // token 范围 [column, column + lexeme.length())
        int tokenStart = token.column;
        int tokenEnd = token.column + static_cast<int>(token.lexeme.size());
        if (targetCol >= tokenStart && targetCol < tokenEnd)
            return &token;
    }
    return nullptr;
}

std::optional<std::string> LspRequestHandler::getIdentifierAt(const DocumentState& doc, int line, int character) const {
    const Token* token = findTokenAt(doc, line, character);
    if (!token || token->type != TokenType::TK_IDENTIFIER)
        return std::nullopt;
    return token->lexeme;
}

// ============================================================
// CLI 辅助函数
// ============================================================

void printHelp() {
    std::fputs("minilang-lsp - MiniLang Language Server Protocol 实现\n"
               "\n"
               "用法:\n"
               "  minilang-lsp [--stdio] [--help] [--version]\n"
               "\n"
               "选项:\n"
               "  --stdio     使用 stdio 作为传输层（默认，LSP 标准模式）\n"
               "  --help      显示帮助信息\n"
               "  --version   显示版本信息\n"
               "\n"
               "LSP 服务器通过 JSON-RPC 2.0 over stdio 与编辑器通信。\n"
               "在 VS Code 中通过安装 MiniLang 扩展自动启动，或手动配置\n"
               "language server 命令为 minilang-lsp --stdio。\n"
               "\n"
               "支持的功能:\n"
               "  - textDocument/didOpen, didChange, didClose\n"
               "  - textDocument/publishDiagnostics（词法/语法/lint 诊断）\n"
               "  - textDocument/completion（关键字 + 已声明符号补全）\n"
               "  - textDocument/hover（符号信息悬停）\n"
               "  - textDocument/definition（跳转到声明）\n"
               "  - textDocument/documentSymbol（文档符号列表）\n"
               "  - textDocument/formatting（代码格式化）\n",
               stdout);
}

void printVersion() {
    std::fprintf(stdout, "minilang-lsp %s\n", versionString().c_str());
}

std::string versionString() {
    return "1.0.0";
}

std::string serverName() {
    return "minilang-lsp";
}

// ============================================================
// LSP 服务器主循环（供 lsp_entry.cpp 统一入口调用）
// ============================================================

int runLspServer() {
    JsonRpcTransport transport;
    LspRequestHandler handler;

    while (!handler.shouldExit()) {
        auto message = transport.read();
        if (!message) {
            // stdin EOF（客户端断开连接）
            return handler.isShutdownRequested() ? 0 : 1;
        }

        std::vector<QJsonObject> responses = handler.handleMessage(*message);
        for (const auto& resp : responses) {
            transport.write(resp);
        }
    }

    return handler.isShutdownRequested() ? 0 : 1;
}

} // namespace minilang_lsp
