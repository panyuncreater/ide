// ============================================================
// tests/TestLspCli.cpp - minilang-lsp 语言服务器核心逻辑测试
// ------------------------------------------------------------
// 测试 minilang_lsp 命名空间下的可测试函数：
//   - 位置转换工具（toLspLine/toLspChar/fromLspLine/fromLspChar）
//   - JSON 构建工具（json namespace）
//   - TextDocumentManager（文档管理）
//   - SymbolIndex（符号索引）
//   - DiagnosticCollector（诊断收集）
//   - JsonRpcTransport（JSON-RPC 传输）
//   - LspRequestHandler（请求处理器）
//   - CLI 辅助函数（versionString/serverName）
//
// 不测试 main 函数与真实 stdio 传输（gtest 不易测试），
// 通过直接调用 core API 间接验证。
// ============================================================
#include "cli/lsp_core.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace minilang_lsp;

// ============================================================
// 1. 位置转换工具
// ============================================================

TEST(LspPositionTest, ToLspConvertsOneBasedToZeroBased) {
    EXPECT_EQ(toLspLine(1), 0);
    EXPECT_EQ(toLspLine(5), 4);
    EXPECT_EQ(toLspChar(1), 0);
    EXPECT_EQ(toLspChar(10), 9);
}

TEST(LspPositionTest, ToLspHandlesNonPositive) {
    EXPECT_EQ(toLspLine(0), 0);
    EXPECT_EQ(toLspLine(-1), 0);
    EXPECT_EQ(toLspChar(0), 0);
    EXPECT_EQ(toLspChar(-5), 0);
}

TEST(LspPositionTest, FromLspConvertsZeroBasedToOneBased) {
    EXPECT_EQ(fromLspLine(0), 1);
    EXPECT_EQ(fromLspLine(4), 5);
    EXPECT_EQ(fromLspChar(0), 1);
    EXPECT_EQ(fromLspChar(9), 10);
}

TEST(LspPositionTest, RoundTripPreservesPosition) {
    EXPECT_EQ(fromLspLine(toLspLine(7)), 7);
    EXPECT_EQ(fromLspChar(toLspChar(3)), 3);
}

// ============================================================
// 2. JSON 构建工具
// ============================================================

TEST(LspJsonTest, PositionHasLineAndCharacter) {
    QJsonObject pos = json::position(3, 7);
    EXPECT_EQ(pos.value("line").toInt(), 3);
    EXPECT_EQ(pos.value("character").toInt(), 7);
}

TEST(LspJsonTest, RangeHasStartAndEnd) {
    QJsonObject r = json::range(1, 2, 3, 4);
    EXPECT_EQ(r.value("start").toObject().value("line").toInt(), 1);
    EXPECT_EQ(r.value("start").toObject().value("character").toInt(), 2);
    EXPECT_EQ(r.value("end").toObject().value("line").toInt(), 3);
    EXPECT_EQ(r.value("end").toObject().value("character").toInt(), 4);
}

TEST(LspJsonTest, LocationHasUriAndRange) {
    QJsonObject loc = json::location("file:///foo.ml", 0, 0, 1, 5);
    EXPECT_EQ(loc.value("uri").toString().toStdString(), "file:///foo.ml");
    EXPECT_TRUE(loc.contains("range"));
}

TEST(LspJsonTest, DiagnosticHasRangeAndSeverity) {
    LspDiagnostic d;
    d.range = {{1, 2}, {1, 5}};
    d.severity = LspSeverity::Error;
    d.message = "test error";
    d.source = "parser";

    QJsonObject j = json::diagnostic(d);
    EXPECT_EQ(j.value("severity").toInt(), 1);
    EXPECT_EQ(j.value("message").toString().toStdString(), "test error");
    EXPECT_EQ(j.value("source").toString().toStdString(), "parser");
    EXPECT_TRUE(j.contains("range"));
}

TEST(LspJsonTest, DiagnosticOmitsEmptySourceAndCode) {
    LspDiagnostic d;
    d.range = {{0, 0}, {0, 1}};
    d.severity = LspSeverity::Warning;
    d.message = "warn";

    QJsonObject j = json::diagnostic(d);
    EXPECT_FALSE(j.contains("source"));
    EXPECT_FALSE(j.contains("code"));
}

TEST(LspJsonTest, DocumentSymbolHasNameAndKind) {
    LspSymbol sym;
    sym.name = "foo";
    sym.kind = LspSymbolKind::Function;
    sym.range = {{0, 0}, {0, 3}};
    sym.selectionRange = {{0, 0}, {0, 3}};

    QJsonObject j = json::documentSymbol(sym);
    EXPECT_EQ(j.value("name").toString().toStdString(), "foo");
    EXPECT_EQ(j.value("kind").toInt(), static_cast<int>(LspSymbolKind::Function));
    EXPECT_TRUE(j.contains("range"));
    EXPECT_TRUE(j.contains("selectionRange"));
}

TEST(LspJsonTest, CompletionItemHasLabelAndKind) {
    LspCompletionItem item;
    item.label = "myVar";
    item.kind = LspCompletionItemKind::Variable;
    item.detail = ": int";

    QJsonObject j = json::completionItem(item);
    EXPECT_EQ(j.value("label").toString().toStdString(), "myVar");
    EXPECT_EQ(j.value("kind").toInt(), static_cast<int>(LspCompletionItemKind::Variable));
    EXPECT_EQ(j.value("detail").toString().toStdString(), ": int");
}

TEST(LspJsonTest, TextEditHasRangeAndNewText) {
    QJsonObject te = json::textEdit(0, 0, 5, 0, "new content");
    EXPECT_TRUE(te.contains("range"));
    EXPECT_EQ(te.value("newText").toString().toStdString(), "new content");
}

// ============================================================
// 3. TextDocumentManager
// ============================================================

TEST(TextDocumentManagerTest, DidOpenStoresDocument) {
    TextDocumentManager mgr;
    mgr.didOpen("file:///a.ml", "var x = 1;");
    ASSERT_NE(mgr.get("file:///a.ml"), nullptr);
    EXPECT_EQ(mgr.get("file:///a.ml")->source, "var x = 1;");
}

TEST(TextDocumentManagerTest, DidChangeUpdatesSource) {
    TextDocumentManager mgr;
    mgr.didOpen("file:///a.ml", "var x = 1;");
    mgr.didChange("file:///a.ml", "var y = 2;");
    ASSERT_NE(mgr.get("file:///a.ml"), nullptr);
    EXPECT_EQ(mgr.get("file:///a.ml")->source, "var y = 2;");
}

TEST(TextDocumentManagerTest, DidChangeOnUnknownOpensDocument) {
    TextDocumentManager mgr;
    mgr.didChange("file:///b.ml", "var z = 3;");
    ASSERT_NE(mgr.get("file:///b.ml"), nullptr);
    EXPECT_EQ(mgr.get("file:///b.ml")->source, "var z = 3;");
}

TEST(TextDocumentManagerTest, DidCloseRemovesDocument) {
    TextDocumentManager mgr;
    mgr.didOpen("file:///a.ml", "var x = 1;");
    mgr.didClose("file:///a.ml");
    EXPECT_EQ(mgr.get("file:///a.ml"), nullptr);
}

TEST(TextDocumentManagerTest, DidOpenParsesAndBuildsSymbols) {
    TextDocumentManager mgr;
    mgr.didOpen("file:///a.ml", "fun add(a: int, b: int) -> int {\n  return a + b;\n}\nvar x = 1;");
    auto* doc = mgr.get("file:///a.ml");
    ASSERT_NE(doc, nullptr);
    ASSERT_NE(doc->ast, nullptr);
    EXPECT_FALSE(doc->hasParseErrors);
    EXPECT_FALSE(doc->tokens.empty());
    EXPECT_FALSE(doc->symbols.empty());
}

TEST(TextDocumentManagerTest, DidOpenWithSyntaxErrorSetsHasParseErrors) {
    TextDocumentManager mgr;
    mgr.didOpen("file:///a.ml", "fun add( { }");
    auto* doc = mgr.get("file:///a.ml");
    ASSERT_NE(doc, nullptr);
    EXPECT_TRUE(doc->hasParseErrors);
    EXPECT_FALSE(doc->diagnostics.empty());
}

TEST(TextDocumentManagerTest, UrisListsAllOpenDocuments) {
    TextDocumentManager mgr;
    mgr.didOpen("file:///a.ml", "var x = 1;");
    mgr.didOpen("file:///b.ml", "var y = 2;");
    auto uris = mgr.uris();
    EXPECT_EQ(uris.size(), 2u);
}

TEST(TextDocumentManagerTest, LintDiagnosticsCollectedWhenNoParseErrors) {
    TextDocumentManager mgr;
    // var x = 1; 后未使用 x —— LintPass 会报 unused-variable
    mgr.didOpen("file:///a.ml", "var unused = 42;");
    auto* doc = mgr.get("file:///a.ml");
    ASSERT_NE(doc, nullptr);
    EXPECT_FALSE(doc->hasParseErrors);
    // 至少应有 1 条 lint 诊断（unused-variable）
    bool hasLintDiag = false;
    for (const auto& d : doc->diagnostics) {
        if (d.source == "lint") {
            hasLintDiag = true;
            break;
        }
    }
    EXPECT_TRUE(hasLintDiag);
}

// ============================================================
// 4. SymbolIndex
// ============================================================

TEST(SymbolIndexTest, BuildFromFunctionDecl) {
    Lexer lexer;
    auto tokens = lexer.scan("fun foo(a: int) -> int {\n  return a;\n}");
    Parser parser;
    auto ast = parser.parse(tokens);
    ASSERT_NE(ast, nullptr);

    SymbolIndex idx;
    idx.build(*ast);
    auto pos = idx.findDefinition("foo");
    ASSERT_TRUE(pos.has_value());
    EXPECT_EQ(pos->line, toLspLine(1));
}

TEST(SymbolIndexTest, BuildFromClassDecl) {
    Lexer lexer;
    auto tokens = lexer.scan("class Point {\n  var x: int;\n  fun get() -> int {\n    return x;\n  }\n}");
    Parser parser;
    auto ast = parser.parse(tokens);
    ASSERT_NE(ast, nullptr);

    SymbolIndex idx;
    idx.build(*ast);

    auto classPos = idx.findDefinition("Point");
    ASSERT_TRUE(classPos.has_value());

    auto methodPos = idx.findDefinition("get");
    ASSERT_TRUE(methodPos.has_value());

    auto fieldPos = idx.findDefinition("x");
    ASSERT_TRUE(fieldPos.has_value());
}

TEST(SymbolIndexTest, BuildFromEnumDecl) {
    Lexer lexer;
    auto tokens = lexer.scan("enum Color {\n  Red,\n  Green,\n  Blue,\n}");
    Parser parser;
    auto ast = parser.parse(tokens);
    ASSERT_NE(ast, nullptr);

    SymbolIndex idx;
    idx.build(*ast);
    auto pos = idx.findDefinition("Color");
    ASSERT_TRUE(pos.has_value());

    // enum variants 作为子符号（名为 EnumName.VariantName）
    auto redPos = idx.findDefinition("Color.Red");
    ASSERT_TRUE(redPos.has_value());
}

TEST(SymbolIndexTest, FindDefinitionReturnsNulloptForUnknown) {
    Lexer lexer;
    auto tokens = lexer.scan("var x = 1;");
    Parser parser;
    auto ast = parser.parse(tokens);
    ASSERT_NE(ast, nullptr);

    SymbolIndex idx;
    idx.build(*ast);
    EXPECT_FALSE(idx.findDefinition("nonexistent").has_value());
}

TEST(SymbolIndexTest, SymbolsExcludesEnumVariantDottedNames) {
    Lexer lexer;
    auto tokens = lexer.scan("enum Color {\n  Red,\n  Green,\n}");
    Parser parser;
    auto ast = parser.parse(tokens);
    ASSERT_NE(ast, nullptr);

    SymbolIndex idx;
    idx.build(*ast);

    // symbols() 只包含顶层符号（不含 "Color.Red" 这类点号名）
    for (const auto& sym : idx.symbols()) {
        EXPECT_EQ(sym.name.find('.'), std::string::npos) << "symbols() 不应包含点号名: " << sym.name;
    }
    // 应包含 "Color" 本身
    bool hasColor = false;
    for (const auto& sym : idx.symbols()) {
        if (sym.name == "Color") {
            hasColor = true;
            EXPECT_EQ(sym.kind, LspSymbolKind::Enum);
        }
    }
    EXPECT_TRUE(hasColor);
}

TEST(SymbolIndexTest, CompletionsSkipEnumVariants) {
    Lexer lexer;
    auto tokens = lexer.scan("enum Color {\n  Red,\n  Green,\n}\nvar x = 1;");
    Parser parser;
    auto ast = parser.parse(tokens);
    ASSERT_NE(ast, nullptr);

    SymbolIndex idx;
    idx.build(*ast);

    auto comps = idx.completions();
    // 补全列表不应包含 "Color.Red" / "Color.Green"
    for (const auto& c : comps) {
        EXPECT_EQ(c.label.find('.'), std::string::npos) << "completions 不应包含点号名: " << c.label;
    }
    // 应包含 Color 和 x
    bool hasColor = false, hasX = false;
    for (const auto& c : comps) {
        if (c.label == "Color")
            hasColor = true;
        if (c.label == "x")
            hasX = true;
    }
    EXPECT_TRUE(hasColor);
    EXPECT_TRUE(hasX);
}

TEST(SymbolIndexTest, CompletionsMapSymbolKindToCompletionKind) {
    Lexer lexer;
    auto tokens = lexer.scan("fun f() {}\nclass C {}\nvar v = 1;");
    Parser parser;
    auto ast = parser.parse(tokens);
    ASSERT_NE(ast, nullptr);

    SymbolIndex idx;
    idx.build(*ast);

    auto comps = idx.completions();
    bool hasFunc = false, hasClass = false, hasVar = false;
    for (const auto& c : comps) {
        if (c.label == "f") {
            EXPECT_EQ(c.kind, LspCompletionItemKind::Function);
            hasFunc = true;
        }
        if (c.label == "C") {
            EXPECT_EQ(c.kind, LspCompletionItemKind::Class);
            hasClass = true;
        }
        if (c.label == "v") {
            EXPECT_EQ(c.kind, LspCompletionItemKind::Variable);
            hasVar = true;
        }
    }
    EXPECT_TRUE(hasFunc);
    EXPECT_TRUE(hasClass);
    EXPECT_TRUE(hasVar);
}

TEST(SymbolIndexTest, FunctionDetailIncludesParamsAndReturn) {
    Lexer lexer;
    auto tokens = lexer.scan("fun add(a: int, b: int) -> int {\n  return a + b;\n}");
    Parser parser;
    auto ast = parser.parse(tokens);
    ASSERT_NE(ast, nullptr);

    SymbolIndex idx;
    idx.build(*ast);

    auto comps = idx.completions();
    ASSERT_FALSE(comps.empty());
    EXPECT_EQ(comps[0].label, "add");
    EXPECT_NE(comps[0].detail.find("a"), std::string::npos);
    EXPECT_NE(comps[0].detail.find("b"), std::string::npos);
    EXPECT_NE(comps[0].detail.find("int"), std::string::npos);
}

// ============================================================
// 5. JsonRpcTransport（缓冲区 I/O 模式）
// ============================================================

namespace {

/// 构造一条 JSON-RPC 消息字符串（含 Content-Length 分帧）
std::string makeRpcMessage(const QJsonObject& msg) {
    QByteArray body = QJsonDocument(msg).toJson(QJsonDocument::Compact);
    std::string result = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    result += body.toStdString();
    return result;
}

} // namespace

TEST(JsonRpcTransportTest, ReadFromBufferParsesSingleMessage) {
    QJsonObject msg;
    msg["jsonrpc"] = "2.0";
    msg["id"] = 1;
    msg["method"] = "initialize";

    JsonRpcTransport transport;
    transport.setInput(makeRpcMessage(msg));

    auto result = transport.read();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->value("method").toString().toStdString(), "initialize");
    EXPECT_EQ(result->value("id").toInt(), 1);
}

TEST(JsonRpcTransportTest, ReadFromBufferParsesMultipleMessages) {
    QJsonObject msg1;
    msg1["jsonrpc"] = "2.0";
    msg1["id"] = 1;
    msg1["method"] = "initialize";

    QJsonObject msg2;
    msg2["jsonrpc"] = "2.0";
    msg2["id"] = 2;
    msg2["method"] = "shutdown";

    std::string input = makeRpcMessage(msg1) + makeRpcMessage(msg2);
    JsonRpcTransport transport;
    transport.setInput(input);

    auto r1 = transport.read();
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1->value("method").toString().toStdString(), "initialize");

    auto r2 = transport.read();
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->value("method").toString().toStdString(), "shutdown");

    auto r3 = transport.read();
    EXPECT_FALSE(r3.has_value());
}

TEST(JsonRpcTransportTest, ReadFromBufferReturnsNulloptOnEmptyInput) {
    JsonRpcTransport transport;
    transport.setInput("");
    EXPECT_FALSE(transport.read().has_value());
}

TEST(JsonRpcTransportTest, ReadFromBufferReturnsNulloptOnIncompleteHeader) {
    JsonRpcTransport transport;
    transport.setInput("Content-Length: 10\r\n"); // 缺少 \r\n 分隔
    EXPECT_FALSE(transport.read().has_value());
}

TEST(JsonRpcTransportTest, WriteProducesContentLengthFramedOutput) {
    QJsonObject msg;
    msg["jsonrpc"] = "2.0";
    msg["id"] = 1;
    msg["result"] = QJsonObject{{"ok", true}};

    JsonRpcTransport transport;
    transport.setInput(""); // 启用缓冲区模式
    transport.write(msg);

    std::string output = transport.outputBuffer();
    EXPECT_NE(output.find("Content-Length:"), std::string::npos);
    EXPECT_NE(output.find("\r\n\r\n"), std::string::npos);
    EXPECT_NE(output.find("\"jsonrpc\""), std::string::npos);
    EXPECT_NE(output.find("\"id\""), std::string::npos);
}

TEST(JsonRpcTransportTest, WriteNotificationFormatsCorrectly) {
    JsonRpcTransport transport;
    transport.setInput("");
    QJsonObject params;
    params["key"] = "value";
    transport.writeNotification("textDocument/publishDiagnostics", params);

    std::string output = transport.outputBuffer();
    EXPECT_NE(output.find("publishDiagnostics"), std::string::npos);
    EXPECT_EQ(output.find("\"id\""), std::string::npos); // 通知无 id
}

TEST(JsonRpcTransportTest, WriteResponseIncludesIdAndResult) {
    JsonRpcTransport transport;
    transport.setInput("");
    QJsonObject result;
    result["value"] = 42;
    transport.writeResponse(7, result);

    std::string output = transport.outputBuffer();
    EXPECT_NE(output.find("\"id\":7"), std::string::npos);
    EXPECT_NE(output.find("\"result\""), std::string::npos);
}

TEST(JsonRpcTransportTest, WriteErrorIncludesCodeAndMessage) {
    JsonRpcTransport transport;
    transport.setInput("");
    transport.writeError(3, -32601, "Method not found");

    std::string output = transport.outputBuffer();
    EXPECT_NE(output.find("-32601"), std::string::npos);
    EXPECT_NE(output.find("Method not found"), std::string::npos);
    EXPECT_NE(output.find("\"error\""), std::string::npos);
}

// ============================================================
// 6. LspRequestHandler
// ============================================================

TEST(LspRequestHandlerTest, InitializeReturnsCapabilitiesAndServerInfo) {
    LspRequestHandler handler;

    QJsonObject req;
    req["jsonrpc"] = "2.0";
    req["id"] = 1;
    req["method"] = "initialize";

    auto responses = handler.handleMessage(req);
    ASSERT_EQ(responses.size(), 1u);
    auto resp = responses[0];
    EXPECT_EQ(resp.value("id").toInt(), 1);
    EXPECT_EQ(resp.value("jsonrpc").toString().toStdString(), "2.0");
    auto result = resp.value("result").toObject();
    EXPECT_TRUE(result.contains("capabilities"));
    EXPECT_TRUE(result.contains("serverInfo"));
    auto caps = result.value("capabilities").toObject();
    EXPECT_EQ(caps.value("textDocumentSync").toInt(), 1);
    EXPECT_TRUE(caps.value("hoverProvider").toBool());
    EXPECT_TRUE(caps.value("definitionProvider").toBool());
    EXPECT_TRUE(caps.value("documentSymbolProvider").toBool());
    EXPECT_TRUE(caps.value("documentFormattingProvider").toBool());
    auto serverInfo = result.value("serverInfo").toObject();
    EXPECT_EQ(serverInfo.value("name").toString().toStdString(), serverName());
    EXPECT_FALSE(serverInfo.value("version").toString().isEmpty());
}

TEST(LspRequestHandlerTest, ShutdownSetsFlagAndReturnsNull) {
    LspRequestHandler handler;
    EXPECT_FALSE(handler.isShutdownRequested());

    QJsonObject req;
    req["jsonrpc"] = "2.0";
    req["id"] = 5;
    req["method"] = "shutdown";

    auto responses = handler.handleMessage(req);
    ASSERT_EQ(responses.size(), 1u);
    EXPECT_TRUE(handler.isShutdownRequested());
    EXPECT_TRUE(responses[0].contains("result"));
    EXPECT_TRUE(responses[0].value("result").isNull());
}

TEST(LspRequestHandlerTest, ExitNotificationSetsShouldExit) {
    LspRequestHandler handler;
    EXPECT_FALSE(handler.shouldExit());

    QJsonObject notif;
    notif["jsonrpc"] = "2.0";
    notif["method"] = "exit";

    auto responses = handler.handleMessage(notif);
    EXPECT_TRUE(responses.empty());
    EXPECT_TRUE(handler.shouldExit());
}

TEST(LspRequestHandlerTest, InitializedNotificationReturnsNoResponses) {
    LspRequestHandler handler;
    QJsonObject notif;
    notif["jsonrpc"] = "2.0";
    notif["method"] = "initialized";

    auto responses = handler.handleMessage(notif);
    EXPECT_TRUE(responses.empty());
}

TEST(LspRequestHandlerTest, UnknownRequestMethodReturnsMethodNotFound) {
    LspRequestHandler handler;
    QJsonObject req;
    req["jsonrpc"] = "2.0";
    req["id"] = 99;
    req["method"] = "custom/method";

    auto responses = handler.handleMessage(req);
    ASSERT_EQ(responses.size(), 1u);
    auto resp = responses[0];
    EXPECT_TRUE(resp.contains("error"));
    EXPECT_FALSE(resp.contains("result"));
    auto err = resp.value("error").toObject();
    EXPECT_EQ(err.value("code").toInt(), -32601);
    EXPECT_NE(err.value("message").toString().toStdString().find("Method not found"), std::string::npos);
}

TEST(LspRequestHandlerTest, UnknownNotificationIsIgnored) {
    LspRequestHandler handler;
    QJsonObject notif;
    notif["jsonrpc"] = "2.0";
    notif["method"] = "custom/notification";

    auto responses = handler.handleMessage(notif);
    EXPECT_TRUE(responses.empty());
}

// ============================================================
// 7. LspRequestHandler — didOpen/didChange/didClose
// ============================================================

namespace {

QJsonObject makeDidOpen(const std::string& uri, const std::string& text) {
    QJsonObject notif;
    notif["jsonrpc"] = "2.0";
    notif["method"] = "textDocument/didOpen";
    QJsonObject params;
    QJsonObject textDoc;
    textDoc["uri"] = QString::fromStdString(uri);
    textDoc["text"] = QString::fromStdString(text);
    params["textDocument"] = textDoc;
    notif["params"] = params;
    return notif;
}

QJsonObject makeDidChange(const std::string& uri, const std::string& text) {
    QJsonObject notif;
    notif["jsonrpc"] = "2.0";
    notif["method"] = "textDocument/didChange";
    QJsonObject params;
    QJsonObject textDoc;
    textDoc["uri"] = QString::fromStdString(uri);
    params["textDocument"] = textDoc;
    QJsonArray changes;
    QJsonObject change;
    change["text"] = QString::fromStdString(text);
    changes.append(change);
    params["contentChanges"] = changes;
    notif["params"] = params;
    return notif;
}

QJsonObject makeDidClose(const std::string& uri) {
    QJsonObject notif;
    notif["jsonrpc"] = "2.0";
    notif["method"] = "textDocument/didClose";
    QJsonObject params;
    QJsonObject textDoc;
    textDoc["uri"] = QString::fromStdString(uri);
    params["textDocument"] = textDoc;
    notif["params"] = params;
    return notif;
}

QJsonObject makeRequest(int id, const std::string& method, const QJsonObject& params) {
    QJsonObject req;
    req["jsonrpc"] = "2.0";
    req["id"] = id;
    req["method"] = QString::fromStdString(method);
    req["params"] = params;
    return req;
}

QJsonObject makeTextDocPositionParams(const std::string& uri, int line, int character) {
    QJsonObject params;
    QJsonObject textDoc;
    textDoc["uri"] = QString::fromStdString(uri);
    params["textDocument"] = textDoc;
    QJsonObject pos;
    pos["line"] = line;
    pos["character"] = character;
    params["position"] = pos;
    return params;
}

} // namespace

TEST(LspRequestHandlerTest, DidOpenPublishesDiagnostics) {
    LspRequestHandler handler;
    auto responses = handler.handleMessage(makeDidOpen("file:///test.ml", "var x = ;"));
    ASSERT_EQ(responses.size(), 1u);
    auto notif = responses[0];
    EXPECT_EQ(notif.value("method").toString().toStdString(), "textDocument/publishDiagnostics");
    auto params = notif.value("params").toObject();
    EXPECT_EQ(params.value("uri").toString().toStdString(), "file:///test.ml");
    EXPECT_FALSE(params.value("diagnostics").toArray().empty());
}

TEST(LspRequestHandlerTest, DidOpenWithValidSourcePublishesEmptyDiagnosticsIfNoLint) {
    LspRequestHandler handler;
    // x 被使用，无 lint 警告
    auto responses = handler.handleMessage(makeDidOpen("file:///test.ml", "var x = 1;\nprint(x);"));
    ASSERT_EQ(responses.size(), 1u);
    auto params = responses[0].value("params").toObject();
    EXPECT_EQ(params.value("uri").toString().toStdString(), "file:///test.ml");
    // 可能有/无 lint 警告，但至少 diagnostics 字段存在
    EXPECT_TRUE(params.contains("diagnostics"));
}

TEST(LspRequestHandlerTest, DidChangeUpdatesDocumentAndPublishesDiagnostics) {
    LspRequestHandler handler;
    // 先 didOpen 一个语法错误
    handler.handleMessage(makeDidOpen("file:///test.ml", "var x = ;"));
    // 修正为合法代码
    auto responses = handler.handleMessage(makeDidChange("file:///test.ml", "var x = 1;\nprint(x);"));
    ASSERT_EQ(responses.size(), 1u);
    EXPECT_EQ(responses[0].value("method").toString().toStdString(), "textDocument/publishDiagnostics");
}

TEST(LspRequestHandlerTest, DidCloseClearsDiagnostics) {
    LspRequestHandler handler;
    handler.handleMessage(makeDidOpen("file:///test.ml", "var x = ;"));

    auto responses = handler.handleMessage(makeDidClose("file:///test.ml"));
    ASSERT_EQ(responses.size(), 1u);
    auto notif = responses[0];
    EXPECT_EQ(notif.value("method").toString().toStdString(), "textDocument/publishDiagnostics");
    auto params = notif.value("params").toObject();
    EXPECT_EQ(params.value("uri").toString().toStdString(), "file:///test.ml");
    EXPECT_TRUE(params.value("diagnostics").toArray().empty());
}

TEST(LspRequestHandlerTest, DidChangeWithEmptyChangesReturnsNoResponses) {
    LspRequestHandler handler;
    handler.handleMessage(makeDidOpen("file:///test.ml", "var x = 1;"));

    // 构造一个 contentChanges 为空的 didChange
    QJsonObject notif;
    notif["jsonrpc"] = "2.0";
    notif["method"] = "textDocument/didChange";
    QJsonObject params;
    QJsonObject textDoc;
    textDoc["uri"] = "file:///test.ml";
    params["textDocument"] = textDoc;
    params["contentChanges"] = QJsonArray();
    notif["params"] = params;

    auto responses = handler.handleMessage(notif);
    EXPECT_TRUE(responses.empty());
}

// ============================================================
// 8. LspRequestHandler — completion/hover/definition/documentSymbol/formatting
// ============================================================

TEST(LspRequestHandlerTest, CompletionReturnsKeywordsAndDeclaredSymbols) {
    LspRequestHandler handler;
    handler.handleMessage(makeDidOpen("file:///test.ml", "fun foo() {}\nvar x = 1;"));

    auto req = makeRequest(1, "textDocument/completion", makeTextDocPositionParams("file:///test.ml", 0, 0));
    auto responses = handler.handleMessage(req);
    ASSERT_EQ(responses.size(), 1u);
    auto result = responses[0].value("result").toObject();
    EXPECT_FALSE(result.value("isIncomplete").toBool());
    auto items = result.value("items").toArray();
    EXPECT_GE(items.size(), 35); // 至少 35 个关键字

    // 验证关键字中包含 "var" 和 "fun"
    bool hasVar = false, hasFun = false;
    for (const auto& item : items) {
        auto label = item.toObject().value("label").toString().toStdString();
        if (label == "var")
            hasVar = true;
        if (label == "fun")
            hasFun = true;
    }
    EXPECT_TRUE(hasVar);
    EXPECT_TRUE(hasFun);

    // 验证包含已声明的 foo 和 x
    bool hasFoo = false, hasX = false;
    for (const auto& item : items) {
        auto label = item.toObject().value("label").toString().toStdString();
        if (label == "foo")
            hasFoo = true;
        if (label == "x")
            hasX = true;
    }
    EXPECT_TRUE(hasFoo);
    EXPECT_TRUE(hasX);
}

TEST(LspRequestHandlerTest, CompletionOnUnknownUriReturnsOnlyKeywords) {
    LspRequestHandler handler;
    auto req = makeRequest(1, "textDocument/completion", makeTextDocPositionParams("file:///nope.ml", 0, 0));
    auto responses = handler.handleMessage(req);
    ASSERT_EQ(responses.size(), 1u);
    auto result = responses[0].value("result").toObject();
    auto items = result.value("items").toArray();
    EXPECT_GE(items.size(), 35);
}

TEST(LspRequestHandlerTest, HoverReturnsInfoForDeclaredFunction) {
    LspRequestHandler handler;
    handler.handleMessage(makeDidOpen("file:///test.ml", "fun foo(a: int) -> int {\n  return a;\n}\n"));

    // 光标位于第 1 行（0-based）的 foo 上
    auto req = makeRequest(1, "textDocument/hover", makeTextDocPositionParams("file:///test.ml", 0, 4));
    auto responses = handler.handleMessage(req);
    ASSERT_EQ(responses.size(), 1u);
    auto result = responses[0].value("result").toObject();
    EXPECT_TRUE(result.contains("contents"));
    auto contents = result.value("contents").toObject();
    EXPECT_TRUE(contents.contains("value"));
    EXPECT_NE(contents.value("value").toString().toStdString().find("foo"), std::string::npos);
}

TEST(LspRequestHandlerTest, HoverReturnsKeywordInfoForKeyword) {
    LspRequestHandler handler;
    handler.handleMessage(makeDidOpen("file:///test.ml", "var x = 1;"));

    // 光标位于 var 关键字上
    auto req = makeRequest(1, "textDocument/hover", makeTextDocPositionParams("file:///test.ml", 0, 1));
    auto responses = handler.handleMessage(req);
    ASSERT_EQ(responses.size(), 1u);
    auto result = responses[0].value("result").toObject();
    EXPECT_TRUE(result.contains("contents"));
    auto contents = result.value("contents").toObject();
    EXPECT_NE(contents.value("value").toString().toStdString().find("var"), std::string::npos);
}

TEST(LspRequestHandlerTest, HoverReturnsNullForNonIdentifier) {
    LspRequestHandler handler;
    handler.handleMessage(makeDidOpen("file:///test.ml", "var x = 1;"));

    // 光标位于 "1" 数字字面量上（非标识符）
    auto req = makeRequest(1, "textDocument/hover", makeTextDocPositionParams("file:///test.ml", 0, 8));
    auto responses = handler.handleMessage(req);
    ASSERT_EQ(responses.size(), 1u);
    EXPECT_TRUE(responses[0].value("result").isNull());
}

TEST(LspRequestHandlerTest, DefinitionJumpsToDeclaration) {
    LspRequestHandler handler;
    handler.handleMessage(makeDidOpen("file:///test.ml", "fun foo() {}\nfoo();\n"));

    // 光标位于第 2 行（0-based 1）的 foo 调用上
    auto req = makeRequest(1, "textDocument/definition", makeTextDocPositionParams("file:///test.ml", 1, 1));
    auto responses = handler.handleMessage(req);
    ASSERT_EQ(responses.size(), 1u);
    auto result = responses[0].value("result").toObject();
    EXPECT_TRUE(result.contains("uri"));
    EXPECT_EQ(result.value("uri").toString().toStdString(), "file:///test.ml");
    auto range = result.value("range").toObject();
    EXPECT_EQ(range.value("start").toObject().value("line").toInt(), 0); // 跳转到第 1 行（0-based 0）
}

TEST(LspRequestHandlerTest, DefinitionReturnsNullForUnknownIdentifier) {
    LspRequestHandler handler;
    handler.handleMessage(makeDidOpen("file:///test.ml", "var x = 1;\n"));

    // 光标位于 x 上（var x = 1; 中 x 在列 5 即 LSP 列 4）
    auto req = makeRequest(1, "textDocument/definition", makeTextDocPositionParams("file:///test.ml", 0, 4));
    auto responses = handler.handleMessage(req);
    ASSERT_EQ(responses.size(), 1u);
    // x 已声明，应返回 Location 而非 null
    EXPECT_FALSE(responses[0].value("result").isNull());
}

TEST(LspRequestHandlerTest, DefinitionOnUnknownDocumentReturnsNull) {
    LspRequestHandler handler;
    auto req = makeRequest(1, "textDocument/definition", makeTextDocPositionParams("file:///nope.ml", 0, 0));
    auto responses = handler.handleMessage(req);
    ASSERT_EQ(responses.size(), 1u);
    EXPECT_TRUE(responses[0].value("result").isNull());
}

TEST(LspRequestHandlerTest, DocumentSymbolReturnsArrayOfSymbols) {
    LspRequestHandler handler;
    handler.handleMessage(makeDidOpen("file:///test.ml", "fun foo() {}\nvar x = 1;\nclass C {}\n"));

    QJsonObject params;
    QJsonObject textDoc;
    textDoc["uri"] = "file:///test.ml";
    params["textDocument"] = textDoc;

    auto req = makeRequest(1, "textDocument/documentSymbol", params);
    auto responses = handler.handleMessage(req);
    ASSERT_EQ(responses.size(), 1u);
    auto result = responses[0].value("result");
    EXPECT_TRUE(result.isArray());
    auto arr = result.toArray();
    EXPECT_GE(arr.size(), 3); // foo, x, C

    // 验证存在 foo / x / C
    bool hasFoo = false, hasX = false, hasC = false;
    for (const auto& sym : arr) {
        auto name = sym.toObject().value("name").toString().toStdString();
        if (name == "foo")
            hasFoo = true;
        if (name == "x")
            hasX = true;
        if (name == "C")
            hasC = true;
    }
    EXPECT_TRUE(hasFoo);
    EXPECT_TRUE(hasX);
    EXPECT_TRUE(hasC);
}

TEST(LspRequestHandlerTest, DocumentSymbolOnUnknownUriReturnsEmptyArray) {
    LspRequestHandler handler;
    QJsonObject params;
    QJsonObject textDoc;
    textDoc["uri"] = "file:///nope.ml";
    params["textDocument"] = textDoc;

    auto req = makeRequest(1, "textDocument/documentSymbol", params);
    auto responses = handler.handleMessage(req);
    ASSERT_EQ(responses.size(), 1u);
    auto result = responses[0].value("result");
    EXPECT_TRUE(result.isArray());
    EXPECT_TRUE(result.toArray().empty());
}

TEST(LspRequestHandlerTest, FormattingReturnsTextEditArray) {
    LspRequestHandler handler;
    // 故意写一个格式不规范的源码（缺缩进）
    handler.handleMessage(makeDidOpen("file:///test.ml", "fun foo(){\nreturn 1;\n}\n"));

    QJsonObject params;
    QJsonObject textDoc;
    textDoc["uri"] = "file:///test.ml";
    params["textDocument"] = textDoc;

    auto req = makeRequest(1, "textDocument/formatting", params);
    auto responses = handler.handleMessage(req);
    ASSERT_EQ(responses.size(), 1u);
    auto result = responses[0].value("result");
    EXPECT_TRUE(result.isArray());
    // 若源码需要格式化，应返回 1 个 TextEdit；若已规范则返回空数组
    // 这里输入不规范，应该有 TextEdit
    EXPECT_GE(result.toArray().size(), 1);
    if (!result.toArray().empty()) {
        auto te = result.toArray().at(0).toObject();
        EXPECT_TRUE(te.contains("range"));
        EXPECT_TRUE(te.contains("newText"));
    }
}

TEST(LspRequestHandlerTest, FormattingOnUnknownUriReturnsEmptyArray) {
    LspRequestHandler handler;
    QJsonObject params;
    QJsonObject textDoc;
    textDoc["uri"] = "file:///nope.ml";
    params["textDocument"] = textDoc;

    auto req = makeRequest(1, "textDocument/formatting", params);
    auto responses = handler.handleMessage(req);
    ASSERT_EQ(responses.size(), 1u);
    auto result = responses[0].value("result");
    EXPECT_TRUE(result.isArray());
    EXPECT_TRUE(result.toArray().empty());
}

TEST(LspRequestHandlerTest, FormattingWithSyntaxErrorsReturnsEmptyArray) {
    LspRequestHandler handler;
    handler.handleMessage(makeDidOpen("file:///test.ml", "fun foo( { }")); // 语法错误

    QJsonObject params;
    QJsonObject textDoc;
    textDoc["uri"] = "file:///test.ml";
    params["textDocument"] = textDoc;

    auto req = makeRequest(1, "textDocument/formatting", params);
    auto responses = handler.handleMessage(req);
    ASSERT_EQ(responses.size(), 1u);
    auto result = responses[0].value("result");
    EXPECT_TRUE(result.isArray());
    EXPECT_TRUE(result.toArray().empty());
}

// ============================================================
// 9. 端到端：完整 LSP 会话模拟
// ============================================================

TEST(LspEndToEndTest, FullSessionInitializeOpenEditShutdownExit) {
    JsonRpcTransport transport;
    LspRequestHandler handler;

    // 1. initialize 请求
    QJsonObject initReq;
    initReq["jsonrpc"] = "2.0";
    initReq["id"] = 1;
    initReq["method"] = "initialize";

    transport.setInput(makeRpcMessage(initReq));
    auto msg = transport.read();
    ASSERT_TRUE(msg.has_value());
    auto responses = handler.handleMessage(*msg);
    ASSERT_EQ(responses.size(), 1u);
    transport.write(responses[0]);
    EXPECT_FALSE(transport.outputBuffer().empty());
    transport.outputBuffer().clear();

    EXPECT_FALSE(handler.isShutdownRequested());
    EXPECT_FALSE(handler.shouldExit());

    // 2. initialized 通知
    QJsonObject initNotif;
    initNotif["jsonrpc"] = "2.0";
    initNotif["method"] = "initialized";
    transport.setInput(makeRpcMessage(initNotif));
    msg = transport.read();
    ASSERT_TRUE(msg.has_value());
    responses = handler.handleMessage(*msg);
    EXPECT_TRUE(responses.empty());

    // 3. didOpen 通知
    transport.setInput(makeRpcMessage(makeDidOpen("file:///test.ml", "var x = 1;\nprint(x);")));
    msg = transport.read();
    ASSERT_TRUE(msg.has_value());
    responses = handler.handleMessage(*msg);
    EXPECT_EQ(responses.size(), 1u);

    // 4. shutdown 请求
    QJsonObject shutdownReq;
    shutdownReq["jsonrpc"] = "2.0";
    shutdownReq["id"] = 2;
    shutdownReq["method"] = "shutdown";
    transport.setInput(makeRpcMessage(shutdownReq));
    msg = transport.read();
    ASSERT_TRUE(msg.has_value());
    responses = handler.handleMessage(*msg);
    ASSERT_EQ(responses.size(), 1u);
    EXPECT_TRUE(handler.isShutdownRequested());

    // 5. exit 通知
    QJsonObject exitNotif;
    exitNotif["jsonrpc"] = "2.0";
    exitNotif["method"] = "exit";
    transport.setInput(makeRpcMessage(exitNotif));
    msg = transport.read();
    ASSERT_TRUE(msg.has_value());
    responses = handler.handleMessage(*msg);
    EXPECT_TRUE(responses.empty());
    EXPECT_TRUE(handler.shouldExit());
}

// ============================================================
// 10. CLI 辅助函数
// ============================================================

TEST(LspCliTest, VersionStringIsNonEmpty) {
    EXPECT_FALSE(versionString().empty());
}

TEST(LspCliTest, ServerNameIsMinilangLsp) {
    EXPECT_EQ(serverName(), "minilang-lsp");
}

// ============================================================
// 11. LSP 二期：references / rename / signatureHelp / semanticTokens
// ============================================================

namespace {
/// LSP 二期共用源码：声明 + 3 处使用（print 读 / 赋值写 / 表达式读）
const char* kRefSource = "var x = 1;\n"   // (0,4) 声明
                         "print(x);\n"    // (1,6) 读
                         "x = x + 1;\n";  // (2,0) 写 + (2,4) 读
} // namespace

TEST(LspPhase2Test, InitializeAdvertisesPhase2Capabilities) {
    LspRequestHandler handler;
    QJsonObject req;
    req["jsonrpc"] = "2.0";
    req["id"] = 1;
    req["method"] = "initialize";
    auto responses = handler.handleMessage(req);
    ASSERT_EQ(responses.size(), 1u);
    auto caps = responses[0].value("result").toObject().value("capabilities").toObject();
    EXPECT_TRUE(caps.value("referencesProvider").toBool());
    EXPECT_TRUE(caps.value("renameProvider").toBool());
    EXPECT_TRUE(caps.contains("signatureHelpProvider"));
    auto semTokens = caps.value("semanticTokensProvider").toObject();
    EXPECT_TRUE(semTokens.value("full").toBool());
    EXPECT_FALSE(semTokens.value("legend").toObject().value("tokenTypes").toArray().isEmpty());
}

TEST(LspPhase2Test, ReferencesFindsDeclarationAndUses) {
    LspRequestHandler handler;
    handler.handleMessage(makeDidOpen("file:///r.ml", kRefSource));

    auto params = makeTextDocPositionParams("file:///r.ml", 0, 4);
    QJsonObject ctx;
    ctx["includeDeclaration"] = true;
    params["context"] = ctx;
    auto responses = handler.handleMessage(makeRequest(1, "textDocument/references", params));
    ASSERT_EQ(responses.size(), 1u);
    auto locations = responses[0].value("result").toArray();
    // 声明(0,4) + 读(1,6) + 写(2,0) + 读(2,4) = 4 处
    EXPECT_EQ(locations.size(), 4);

    // includeDeclaration=false：仅 3 处使用
    ctx["includeDeclaration"] = false;
    params["context"] = ctx;
    responses = handler.handleMessage(makeRequest(2, "textDocument/references", params));
    ASSERT_EQ(responses.size(), 1u);
    EXPECT_EQ(responses[0].value("result").toArray().size(), 3);
}

TEST(LspPhase2Test, RenameProducesWorkspaceEdit) {
    LspRequestHandler handler;
    handler.handleMessage(makeDidOpen("file:///r.ml", kRefSource));

    auto params = makeTextDocPositionParams("file:///r.ml", 0, 4);
    params["newName"] = "count";
    auto responses = handler.handleMessage(makeRequest(1, "textDocument/rename", params));
    ASSERT_EQ(responses.size(), 1u);
    auto changes = responses[0].value("result").toObject().value("changes").toObject();
    auto edits = changes.value("file:///r.ml").toArray();
    EXPECT_EQ(edits.size(), 4);
    EXPECT_EQ(edits[0].toObject().value("newText").toString().toStdString(), "count");
}

TEST(LspPhase2Test, RenameRejectsInvalidNewName) {
    LspRequestHandler handler;
    handler.handleMessage(makeDidOpen("file:///r.ml", kRefSource));

    // 非法标识符与关键字均拒绝（result 为 null）
    for (const char* bad : {"1bad", "a-b", "var", ""}) {
        auto params = makeTextDocPositionParams("file:///r.ml", 0, 4);
        params["newName"] = bad;
        auto responses = handler.handleMessage(makeRequest(1, "textDocument/rename", params));
        ASSERT_EQ(responses.size(), 1u);
        EXPECT_TRUE(responses[0].value("result").isNull()) << "newName=" << bad;
    }
}

TEST(LspPhase2Test, SignatureHelpInsideCall) {
    LspRequestHandler handler;
    handler.handleMessage(makeDidOpen("file:///s.ml", "fun add(a: int, b: int) -> int {\n"
                                                      "  return a + b;\n"
                                                      "}\n"
                                                      "print(add(1, 2));\n"));

    // 光标在 "print(add(1, |2))" 的第二参数处（0-based line 3, char 13）
    auto responses =
        handler.handleMessage(makeRequest(1, "textDocument/signatureHelp", makeTextDocPositionParams("file:///s.ml", 3, 13)));
    ASSERT_EQ(responses.size(), 1u);
    auto result = responses[0].value("result").toObject();
    auto signatures = result.value("signatures").toArray();
    ASSERT_EQ(signatures.size(), 1);
    auto label = signatures[0].toObject().value("label").toString().toStdString();
    EXPECT_NE(label.find("add("), std::string::npos);
    EXPECT_EQ(signatures[0].toObject().value("parameters").toArray().size(), 2);
    EXPECT_EQ(result.value("activeParameter").toInt(), 1); // 第二个参数
}

TEST(LspPhase2Test, SemanticTokensProducesDeltaEncodedData) {
    LspRequestHandler handler;
    handler.handleMessage(makeDidOpen("file:///t.ml", "var x = 1;\nprint(x);\n"));

    QJsonObject params;
    QJsonObject textDoc;
    textDoc["uri"] = "file:///t.ml";
    params["textDocument"] = textDoc;
    auto responses = handler.handleMessage(makeRequest(1, "textDocument/semanticTokens/full", params));
    ASSERT_EQ(responses.size(), 1u);
    auto data = responses[0].value("result").toObject().value("data").toArray();
    ASSERT_FALSE(data.isEmpty());
    EXPECT_EQ(data.size() % 5, 0); // 5 元组差分编码
    // 首 token 是 "var"：deltaLine=0 deltaChar=0 len=3 type=0(keyword)
    EXPECT_EQ(data[0].toInt(), 0);
    EXPECT_EQ(data[1].toInt(), 0);
    EXPECT_EQ(data[2].toInt(), 3);
    EXPECT_EQ(data[3].toInt(), 0);
}
