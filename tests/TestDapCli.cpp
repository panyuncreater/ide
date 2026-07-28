// ============================================================
// tests/TestDapCli.cpp - minilang-dap 调试适配器核心逻辑测试
// ------------------------------------------------------------
// 测试 minilang_dap 命名空间下的可测试组件：
//   - JSON 构建工具（json namespace）
//   - DapTransport（JSON-RPC 传输，缓冲区 I/O 模式）
//   - DebugSession（launch + 步进 + 状态查询 + 变量查询）
//   - DapRequestHandler（请求分发与响应/事件生成）
//   - 端到端：完整 DAP 会话模拟
//   - CLI 辅助函数（versionString/serverName）
//
// 不测试 main 函数与真实 stdio 传输（gtest 不易测试），
// 通过 DapTransport::setInput/outputBuffer 间接验证协议帧。
// ============================================================
#include "cli/dap_core.h"
#include "interpreter/Value.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace minilang_dap;

// ============================================================
// 辅助函数：构造 DAP 消息
// ============================================================

namespace {

/// 构造一条 DAP JSON-RPC 消息字符串（含 Content-Length 分帧）
std::string makeDapMessage(const QJsonObject& msg) {
    QByteArray body = QJsonDocument(msg).toJson(QJsonDocument::Compact);
    std::string result = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    result += body.toStdString();
    return result;
}

/// 构造 DAP 请求消息
QJsonObject makeDapRequest(int seq, const std::string& command, const QJsonObject& args = {}) {
    QJsonObject req;
    req["seq"] = seq;
    req["type"] = "request";
    req["command"] = QString::fromStdString(command);
    req["arguments"] = args;
    return req;
}

/// 构造 launch 参数（内联源码模式，避免依赖磁盘文件）
QJsonObject makeLaunchArgs(const std::string& source, const std::string& path = "/test.ml") {
    QJsonObject args;
    args["program"] = QString::fromStdString(path);
    // 同时提供 source.source 内联源码（dap_core 优先读 program 文件，
    // 文件读取失败后回退到 source.source）
    QJsonObject src;
    src["path"] = QString::fromStdString(path);
    src["source"] = QString::fromStdString(source);
    args["source"] = src;
    return args;
}

} // namespace

// ============================================================
// 1. JSON 构建工具
// ============================================================

TEST(DapJsonTest, SourceHasNamePathAndReference) {
    QJsonObject s = json::source("test.ml", "/path/test.ml", 0);
    EXPECT_EQ(s.value("name").toString().toStdString(), "test.ml");
    EXPECT_EQ(s.value("path").toString().toStdString(), "/path/test.ml");
    EXPECT_EQ(s.value("sourceReference").toInt(), 0);
}

TEST(DapJsonTest, StackFrameHasIdNameSourceLineColumn) {
    DapStackFrame frame;
    frame.id = 5;
    frame.name = "main";
    frame.source.name = "test.ml";
    frame.source.path = "/test.ml";
    frame.line = 10;
    frame.column = 1;
    frame.endLine = 10;
    frame.endColumn = 20;

    QJsonObject j = json::stackFrame(frame);
    EXPECT_EQ(j.value("id").toInt(), 5);
    EXPECT_EQ(j.value("name").toString().toStdString(), "main");
    EXPECT_EQ(j.value("line").toInt(), 10);
    EXPECT_EQ(j.value("column").toInt(), 1);
    EXPECT_EQ(j.value("endLine").toInt(), 10);
    EXPECT_EQ(j.value("endColumn").toInt(), 20);
    EXPECT_TRUE(j.contains("source"));
}

TEST(DapJsonTest, ScopeHasNameAndVariablesReference) {
    DapScope s;
    s.name = "Locals";
    s.variablesReference = 1000;
    s.namedVariables = 3;
    s.expensive = false;

    QJsonObject j = json::scope(s);
    EXPECT_EQ(j.value("name").toString().toStdString(), "Locals");
    EXPECT_EQ(j.value("variablesReference").toInt(), 1000);
    EXPECT_EQ(j.value("namedVariables").toInt(), 3);
    EXPECT_EQ(j.value("expensive").toBool(), false);
}

TEST(DapJsonTest, VariableHasNameValueTypeAndReference) {
    DapVariable v;
    v.name = "x";
    v.value = "42";
    v.type = "int";
    v.variablesReference = 0;
    v.evaluateName = "x";

    QJsonObject j = json::variable(v);
    EXPECT_EQ(j.value("name").toString().toStdString(), "x");
    EXPECT_EQ(j.value("value").toString().toStdString(), "42");
    EXPECT_EQ(j.value("type").toString().toStdString(), "int");
    EXPECT_EQ(j.value("variablesReference").toInt(), 0);
    EXPECT_EQ(j.value("evaluateName").toString().toStdString(), "x");
}

TEST(DapJsonTest, VariableOmitsEmptyTypeAndEvaluateName) {
    DapVariable v;
    v.name = "y";
    v.value = "null";
    v.variablesReference = 0;

    QJsonObject j = json::variable(v);
    EXPECT_FALSE(j.contains("type"));
    EXPECT_FALSE(j.contains("evaluateName"));
}

TEST(DapJsonTest, BreakpointHasIdLineVerified) {
    DapBreakpoint bp;
    bp.id = 1;
    bp.line = 5;
    bp.verified = true;

    QJsonObject j = json::breakpoint(bp);
    EXPECT_EQ(j.value("id").toInt(), 1);
    EXPECT_EQ(j.value("line").toInt(), 5);
    EXPECT_EQ(j.value("verified").toBool(), true);
}

TEST(DapJsonTest, BreakpointWithMessageAndSource) {
    DapBreakpoint bp;
    bp.id = 2;
    bp.line = 10;
    bp.verified = false;
    bp.message = "unverified";
    bp.source.name = "test.ml";
    bp.source.path = "/test.ml";

    QJsonObject j = json::breakpoint(bp);
    EXPECT_EQ(j.value("message").toString().toStdString(), "unverified");
    EXPECT_TRUE(j.contains("source"));
}

// ============================================================
// 2. DapTransport（缓冲区 I/O 模式）
// ============================================================

TEST(DapTransportTest, ReadFromBufferParsesSingleMessage) {
    QJsonObject msg;
    msg["seq"] = 1;
    msg["type"] = "request";
    msg["command"] = "initialize";

    DapTransport transport;
    transport.setInput(makeDapMessage(msg));

    auto result = transport.read();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->value("command").toString().toStdString(), "initialize");
    EXPECT_EQ(result->value("seq").toInt(), 1);
}

TEST(DapTransportTest, ReadFromBufferParsesMultipleMessages) {
    QJsonObject msg1 = makeDapRequest(1, "initialize");
    QJsonObject msg2 = makeDapRequest(2, "threads");

    std::string input = makeDapMessage(msg1) + makeDapMessage(msg2);
    DapTransport transport;
    transport.setInput(input);

    auto r1 = transport.read();
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1->value("command").toString().toStdString(), "initialize");

    auto r2 = transport.read();
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->value("command").toString().toStdString(), "threads");

    auto r3 = transport.read();
    EXPECT_FALSE(r3.has_value());
}

TEST(DapTransportTest, ReadFromBufferReturnsNulloptOnEmptyInput) {
    DapTransport transport;
    transport.setInput("");
    EXPECT_FALSE(transport.read().has_value());
}

TEST(DapTransportTest, ReadFromBufferReturnsNulloptOnIncompleteHeader) {
    DapTransport transport;
    transport.setInput("Content-Length: 10\r\n"); // 缺少 \r\n 分隔
    EXPECT_FALSE(transport.read().has_value());
}

TEST(DapTransportTest, WriteProducesContentLengthFramedOutput) {
    QJsonObject msg;
    msg["type"] = "response";
    msg["seq"] = 1;
    msg["command"] = "initialize";

    DapTransport transport;
    transport.setInput(""); // 启用缓冲区模式
    transport.write(msg);

    std::string output = transport.outputBuffer();
    EXPECT_NE(output.find("Content-Length:"), std::string::npos);
    EXPECT_NE(output.find("\r\n\r\n"), std::string::npos);
    EXPECT_NE(output.find("\"type\""), std::string::npos);
    EXPECT_NE(output.find("\"command\""), std::string::npos);
}

TEST(DapTransportTest, WriteMultipleMessagesAccumulatesInBuffer) {
    DapTransport transport;
    transport.setInput("");

    QJsonObject msg1;
    msg1["seq"] = 1;
    msg1["type"] = "response";
    QJsonObject msg2;
    msg2["seq"] = 2;
    msg2["type"] = "event";

    transport.write(msg1);
    transport.write(msg2);

    std::string output = transport.outputBuffer();
    // 两条消息应该都出现在缓冲区中
    EXPECT_NE(output.find("\"seq\":1"), std::string::npos);
    EXPECT_NE(output.find("\"seq\":2"), std::string::npos);
    // 应该有两个 Content-Length 头
    size_t first = output.find("Content-Length:");
    EXPECT_NE(first, std::string::npos);
    EXPECT_NE(output.find("Content-Length:", first + 1), std::string::npos);
}

// ============================================================
// 3. DebugSession
// ============================================================

TEST(DebugSessionTest, LaunchSimpleProgramSucceeds) {
    DebugSession session;
    std::string err = session.launch("var x = 1;\nprint(x);\n", "/test.ml");
    EXPECT_TRUE(err.empty()) << "launch error: " << err;
    EXPECT_TRUE(session.isLaunched());
    EXPECT_FALSE(session.isFinished());
}

TEST(DebugSessionTest, LaunchWithSyntaxErrorReturnsErrorMessage) {
    DebugSession session;
    std::string err = session.launch("var x = ;\n", "/test.ml");
    EXPECT_FALSE(err.empty());
    EXPECT_FALSE(session.isLaunched());
}

TEST(DebugSessionTest, LaunchWithEmptySourceSucceedsAsEmptyProgram) {
    DebugSession session;
    std::string err = session.launch("", "/test.ml");
    // 空源码视为合法的空程序（仅 OP_RETURN），launch 应成功
    EXPECT_TRUE(err.empty()) << "unexpected error: " << err;
    EXPECT_TRUE(session.isLaunched());
}

TEST(DebugSessionTest, ResetClearsLaunchedState) {
    DebugSession session;
    session.launch("var x = 1;\nprint(x);\n", "/test.ml");
    EXPECT_TRUE(session.isLaunched());

    session.reset();
    EXPECT_FALSE(session.isLaunched());
    EXPECT_TRUE(session.isFinished());
    EXPECT_TRUE(session.getBreakpoints().empty());
    EXPECT_TRUE(session.getFilePath().empty());
}

TEST(DebugSessionTest, SetBreakpointsReplacesPreviousSet) {
    DebugSession session;
    session.setBreakpoints({1, 2, 3});
    EXPECT_EQ(session.getBreakpoints().size(), 3u);

    session.setBreakpoints({5});
    EXPECT_EQ(session.getBreakpoints().size(), 1u);
    EXPECT_EQ(*session.getBreakpoints().begin(), 5);
}

TEST(DebugSessionTest, DoContinueFinishesSimpleProgram) {
    DebugSession session;
    session.launch("var x = 1;\nprint(x);\n", "/test.ml");
    // 无断点，continue 应执行到结束
    StepResult result = session.doContinue();
    EXPECT_EQ(result, StepResult::Finished);
    EXPECT_TRUE(session.isFinished());
}

TEST(DebugSessionTest, DoContinueStopsAtBreakpoint) {
    DebugSession session;
    // 行 3 设置断点：var y = 2;
    session.launch("var x = 1;\nprint(x);\nvar y = 2;\nprint(y);\n", "/test.ml");
    session.setBreakpoints({3});

    StepResult result = session.doContinue();
    // 应在断点行 3 暂停（Ok）或因其他原因停止
    EXPECT_EQ(result, StepResult::Ok);
    EXPECT_FALSE(session.isFinished());
}

TEST(DebugSessionTest, DoStepInAdvancesExecution) {
    DebugSession session;
    session.launch("var x = 1;\nvar y = 2;\nprint(x + y);\n", "/test.ml");

    StepResult result = session.doStepIn();
    // stepIn 应该步进到下一行（Ok）或结束（Finished）
    EXPECT_TRUE(result == StepResult::Ok || result == StepResult::Finished);
    if (result == StepResult::Ok) {
        // 行号应该改变（或保持但帧状态改变）
        EXPECT_FALSE(session.isFinished());
    }
}

TEST(DebugSessionTest, DoStepOverAdvancesExecution) {
    DebugSession session;
    session.launch("var x = 1;\nvar y = 2;\nprint(x);\n", "/test.ml");

    StepResult result = session.doStepOver();
    EXPECT_TRUE(result == StepResult::Ok || result == StepResult::Finished);
}

TEST(DebugSessionTest, GetCallStackReturnsFrames) {
    DebugSession session;
    // 包含函数调用的程序
    session.launch("fun add(a: int, b: int) -> int {\n  return a + b;\n}\nprint(add(1, 2));\n", "/test.ml");

    // 步进到函数内部
    session.doStepIn(); // 进入 add 函数

    auto frames = session.getCallStack();
    EXPECT_FALSE(frames.empty());
    // 主帧应该在栈中
    bool hasMain = false;
    for (const auto& f : frames) {
        if (f.name == "main" || f.name.find("add") != std::string::npos) {
            hasMain = true;
            break;
        }
    }
    EXPECT_TRUE(hasMain);
}

TEST(DebugSessionTest, GetFrameCountMatchesCallStack) {
    DebugSession session;
    session.launch("var x = 1;\nprint(x);\n", "/test.ml");
    auto frames = session.getCallStack();
    EXPECT_EQ(frames.size(), session.getFrameCount());
}

TEST(DebugSessionTest, GetCurrentLineIsPositive) {
    DebugSession session;
    session.launch("var x = 1;\nprint(x);\n", "/test.ml");
    EXPECT_GT(session.getCurrentLine(), 0);
}

TEST(DebugSessionTest, GetScopesReturnsLocalsAndGlobals) {
    DebugSession session;
    session.launch("var x = 1;\nprint(x);\n", "/test.ml");

    auto scopes = session.getScopes(0);
    ASSERT_EQ(scopes.size(), 2u);
    EXPECT_EQ(scopes[0].name, "Locals");
    EXPECT_EQ(scopes[1].name, "Globals");
    // Locals scope 的 variablesReference 应为 1000 + frameId
    EXPECT_EQ(scopes[0].variablesReference, kLocalsScopeReference + 0);
    // Globals scope 的 variablesReference 应为 2000
    EXPECT_EQ(scopes[1].variablesReference, kGlobalsScopeReference);
}

TEST(DebugSessionTest, GetVariablesFromGlobalsScope) {
    DebugSession session;
    session.launch("var x = 1;\nvar y = 2;\nprint(x + y);\n", "/test.ml");
    // 步进让变量被定义
    session.doContinue();

    // 重置会话并重新启动以测试变量查询
    session.reset();
    session.launch("var x = 1;\nvar y = 2;\nprint(x + y);\n", "/test.ml");
    // 步进到 print 语句后，x 和 y 应该已定义
    session.setBreakpoints({3});
    session.doContinue();

    // 顶层 var 声明是全局变量，查询 Globals scope
    auto vars = session.getVariables(kGlobalsScopeReference);
    // 应该能查到 x 和 y（可能还有其他变量）
    bool hasX = false, hasY = false;
    for (const auto& v : vars) {
        if (v.name == "x")
            hasX = true;
        if (v.name == "y")
            hasY = true;
    }
    EXPECT_TRUE(hasX);
    EXPECT_TRUE(hasY);
}

TEST(DebugSessionTest, GetSingleGlobalVariable) {
    DebugSession session;
    session.launch("var g = 42;\nprint(g);\n", "/test.ml");
    session.setBreakpoints({2});
    session.doContinue();

    auto vars = session.getVariables(kGlobalsScopeReference);
    bool hasG = false;
    for (const auto& v : vars) {
        if (v.name == "g")
            hasG = true;
    }
    EXPECT_TRUE(hasG);
}

TEST(DebugSessionTest, GetVariablesFromInvalidReferenceReturnsEmpty) {
    DebugSession session;
    session.launch("var x = 1;\n", "/test.ml");
    auto vars = session.getVariables(99999); // 无效引用
    EXPECT_TRUE(vars.empty());
}

TEST(DebugSessionTest, EvaluateLocalVariable) {
    DebugSession session;
    session.launch("var x = 42;\nprint(x);\n", "/test.ml");
    session.setBreakpoints({2});
    session.doContinue();

    auto result = session.evaluate("x", 0);
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.value, "42");
    EXPECT_EQ(result.type, "int");
}

TEST(DebugSessionTest, EvaluateGlobalVariable) {
    DebugSession session;
    session.launch("var g = 100;\nprint(g);\n", "/test.ml");
    session.setBreakpoints({2});
    session.doContinue();

    auto result = session.evaluate("g", 0);
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.value, "100");
}

TEST(DebugSessionTest, EvaluateUnknownVariableReturnsFalse) {
    DebugSession session;
    session.launch("var x = 1;\nprint(x);\n", "/test.ml");
    session.setBreakpoints({2});
    session.doContinue();

    auto result = session.evaluate("nonexistent", 0);
    EXPECT_FALSE(result.ok);
}

TEST(DebugSessionTest, OutputCallbackReceivesPrintOutput) {
    DebugSession session;
    std::string captured;
    session.setOutputCallback([&captured](const std::string& s) { captured += s; });
    session.launch("print(\"hello\");\n", "/test.ml");
    session.doContinue();

    // 输出应该包含 "hello"
    EXPECT_NE(captured.find("hello"), std::string::npos);
}

TEST(DebugSessionTest, GetOutputAccumulatesPrintOutput) {
    DebugSession session;
    session.launch("print(\"line1\");\nprint(\"line2\");\n", "/test.ml");
    session.doContinue();

    std::string output = session.getOutput();
    EXPECT_NE(output.find("line1"), std::string::npos);
    EXPECT_NE(output.find("line2"), std::string::npos);
}

TEST(DebugSessionTest, ArrayVariableHasVariablesReference) {
    DebugSession session;
    session.launch("var arr = [1, 2, 3];\nprint(arr);\n", "/test.ml");
    session.setBreakpoints({2});
    session.doContinue();

    // 顶层 var 声明是全局变量，查询 Globals scope
    auto vars = session.getVariables(kGlobalsScopeReference);
    bool foundArr = false;
    for (const auto& v : vars) {
        if (v.name == "arr") {
            foundArr = true;
            EXPECT_NE(v.variablesReference, kVariablesReferenceNone);
            // 展开数组元素
            auto elements = session.getVariables(v.variablesReference);
            EXPECT_EQ(elements.size(), 3u);
            break;
        }
    }
    EXPECT_TRUE(foundArr);
}

TEST(DebugSessionTest, DictVariableHasVariablesReference) {
    DebugSession session;
    session.launch("var d = {\"a\": 1, \"b\": 2};\nprint(d);\n", "/test.ml");
    session.setBreakpoints({2});
    session.doContinue();

    // 顶层 var 声明是全局变量，查询 Globals scope
    auto vars = session.getVariables(kGlobalsScopeReference);
    bool foundDict = false;
    for (const auto& v : vars) {
        if (v.name == "d") {
            foundDict = true;
            EXPECT_NE(v.variablesReference, kVariablesReferenceNone);
            auto entries = session.getVariables(v.variablesReference);
            EXPECT_EQ(entries.size(), 2u);
            break;
        }
    }
    EXPECT_TRUE(foundDict);
}

TEST(DebugSessionTest, RegisterVariableContainerReturnsIncrementalRef) {
    DebugSession session;
    session.launch("var x = 1;\n", "/test.ml");

    VariableContainer c1;
    c1.type = VariableContainerType::Array;
    c1.containerValue = Value(std::vector<Value>{Value(1), Value(2)});
    int ref1 = session.registerVariableContainer(c1);

    VariableContainer c2;
    c2.type = VariableContainerType::Array;
    c2.containerValue = Value(std::vector<Value>{Value(3)});
    int ref2 = session.registerVariableContainer(c2);

    EXPECT_GT(ref2, ref1);
    EXPECT_GE(ref1, 3000);

    // 通过引用查询容器变量
    auto vars1 = session.getVariables(ref1);
    EXPECT_EQ(vars1.size(), 2u);
    auto vars2 = session.getVariables(ref2);
    EXPECT_EQ(vars2.size(), 1u);
}

// ============================================================
// 4. DapRequestHandler
// ============================================================

TEST(DapRequestHandlerTest, InitializeReturnsCapabilities) {
    DapRequestHandler handler;
    auto req = makeDapRequest(1, "initialize");
    auto responses = handler.handleMessage(req);

    ASSERT_EQ(responses.size(), 1u);
    EXPECT_EQ(responses[0].value("type").toString().toStdString(), "response");
    EXPECT_EQ(responses[0].value("command").toString().toStdString(), "initialize");
    EXPECT_EQ(responses[0].value("request_seq").toInt(), 1);
    EXPECT_TRUE(responses[0].value("success").toBool());

    QJsonObject body = responses[0].value("body").toObject();
    EXPECT_TRUE(body.contains("capabilities"));
    EXPECT_TRUE(body.value("linesStartAt1").toBool());
    EXPECT_TRUE(body.value("columnsStartAt1").toBool());

    QJsonObject caps = body.value("capabilities").toObject();
    EXPECT_TRUE(caps.value("supportsConfigurationDoneRequest").toBool());
    EXPECT_TRUE(caps.value("supportsEvaluateForHovers").toBool());
    EXPECT_TRUE(caps.value("supportsTerminateRequest").toBool());
}

TEST(DapRequestHandlerTest, LaunchWithInlineSourceSucceeds) {
    DapRequestHandler handler;
    QJsonObject args = makeLaunchArgs("var x = 1;\nprint(x);\n", "/test.ml");
    auto req = makeDapRequest(1, "launch", args);
    auto responses = handler.handleMessage(req);

    ASSERT_EQ(responses.size(), 1u);
    EXPECT_TRUE(responses[0].value("success").toBool());
    EXPECT_TRUE(handler.session().isLaunched());
}

TEST(DapRequestHandlerTest, LaunchWithSyntaxErrorFails) {
    DapRequestHandler handler;
    QJsonObject args = makeLaunchArgs("var x = ;\n", "/test.ml");
    auto req = makeDapRequest(1, "launch", args);
    auto responses = handler.handleMessage(req);

    ASSERT_EQ(responses.size(), 1u);
    EXPECT_FALSE(responses[0].value("success").toBool());
    EXPECT_FALSE(handler.session().isLaunched());
    // 错误消息应该非空
    QString msg = responses[0].value("message").toString();
    EXPECT_FALSE(msg.isEmpty());
}

TEST(DapRequestHandlerTest, SetBreakpointsReturnsVerifiedBreakpoints) {
    DapRequestHandler handler;
    // 先 launch（避免空 session 设置断点）
    QJsonObject launchArgs = makeLaunchArgs("var x = 1;\nprint(x);\nvar y = 2;\n", "/test.ml");
    handler.handleMessage(makeDapRequest(1, "launch", launchArgs));

    // 设置断点
    QJsonObject bpArgs;
    bpArgs["source"] = QJsonObject{{"path", "/test.ml"}};
    QJsonArray bpArray;
    bpArray.append(QJsonObject{{"line", 2}});
    bpArray.append(QJsonObject{{"line", 3}});
    bpArgs["breakpoints"] = bpArray;

    auto responses = handler.handleMessage(makeDapRequest(2, "setBreakpoints", bpArgs));
    ASSERT_EQ(responses.size(), 1u);
    EXPECT_TRUE(responses[0].value("success").toBool());

    QJsonObject body = responses[0].value("body").toObject();
    QJsonArray bps = body.value("breakpoints").toArray();
    ASSERT_EQ(bps.size(), 2);
    EXPECT_TRUE(bps[0].toObject().value("verified").toBool());
    EXPECT_EQ(bps[0].toObject().value("line").toInt(), 2);
    EXPECT_EQ(bps[1].toObject().value("line").toInt(), 3);
}

TEST(DapRequestHandlerTest, ConfigurationDoneAfterLaunchSendsStoppedEntry) {
    DapRequestHandler handler;
    QJsonObject launchArgs = makeLaunchArgs("var x = 1;\nprint(x);\n", "/test.ml");
    handler.handleMessage(makeDapRequest(1, "launch", launchArgs));

    auto responses = handler.handleMessage(makeDapRequest(2, "configurationDone"));
    // 应该有 1 个 response + 1 个 stopped(entry) 事件
    ASSERT_EQ(responses.size(), 2u);
    EXPECT_EQ(responses[0].value("type").toString().toStdString(), "response");
    EXPECT_EQ(responses[1].value("type").toString().toStdString(), "event");
    EXPECT_EQ(responses[1].value("event").toString().toStdString(), "stopped");

    QJsonObject body = responses[1].value("body").toObject();
    EXPECT_EQ(body.value("reason").toString().toStdString(), "entry");
    EXPECT_EQ(body.value("threadId").toInt(), kMainThreadId);
}

TEST(DapRequestHandlerTest, ContinueRunsToCompletionSendsTerminated) {
    DapRequestHandler handler;
    QJsonObject launchArgs = makeLaunchArgs("var x = 1;\nprint(x);\n", "/test.ml");
    handler.handleMessage(makeDapRequest(1, "launch", launchArgs));
    handler.handleMessage(makeDapRequest(2, "configurationDone"));

    auto responses = handler.handleMessage(makeDapRequest(3, "continue"));
    // 应该有 1 个 response + output 事件（如果有输出）+ terminated 事件
    EXPECT_GE(responses.size(), 2u);
    EXPECT_EQ(responses[0].value("type").toString().toStdString(), "response");
    EXPECT_EQ(responses[0].value("command").toString().toStdString(), "continue");

    // 最后一个事件应该是 terminated
    QJsonObject lastEvent = responses.back();
    EXPECT_EQ(lastEvent.value("type").toString().toStdString(), "event");
    EXPECT_EQ(lastEvent.value("event").toString().toStdString(), "terminated");
}

TEST(DapRequestHandlerTest, ContinueWithBreakpointSendsStopped) {
    DapRequestHandler handler;
    QJsonObject launchArgs = makeLaunchArgs("var x = 1;\nprint(x);\nvar y = 2;\nprint(y);\n", "/test.ml");
    handler.handleMessage(makeDapRequest(1, "launch", launchArgs));

    // 设置断点在行 3
    QJsonObject bpArgs;
    QJsonArray bpArray;
    bpArray.append(QJsonObject{{"line", 3}});
    bpArgs["breakpoints"] = bpArray;
    handler.handleMessage(makeDapRequest(2, "setBreakpoints", bpArgs));
    handler.handleMessage(makeDapRequest(3, "configurationDone"));

    auto responses = handler.handleMessage(makeDapRequest(4, "continue"));
    EXPECT_GE(responses.size(), 2u);
    // 最后一个事件应该是 stopped（断点命中）
    QJsonObject lastEvent = responses.back();
    EXPECT_EQ(lastEvent.value("type").toString().toStdString(), "event");
    EXPECT_EQ(lastEvent.value("event").toString().toStdString(), "stopped");
}

TEST(DapRequestHandlerTest, NextStepSendsStoppedStep) {
    DapRequestHandler handler;
    QJsonObject launchArgs = makeLaunchArgs("var x = 1;\nvar y = 2;\nprint(x);\n", "/test.ml");
    handler.handleMessage(makeDapRequest(1, "launch", launchArgs));
    handler.handleMessage(makeDapRequest(2, "configurationDone"));

    auto responses = handler.handleMessage(makeDapRequest(3, "next"));
    EXPECT_GE(responses.size(), 2u);
    EXPECT_EQ(responses[0].value("command").toString().toStdString(), "next");

    QJsonObject lastEvent = responses.back();
    EXPECT_EQ(lastEvent.value("event").toString().toStdString(), "stopped");
    EXPECT_EQ(lastEvent.value("body").toObject().value("reason").toString().toStdString(), "step");
}

TEST(DapRequestHandlerTest, StepInSendsStoppedStep) {
    DapRequestHandler handler;
    QJsonObject launchArgs = makeLaunchArgs("fun f() -> int {\n  return 1;\n}\nprint(f());\n", "/test.ml");
    handler.handleMessage(makeDapRequest(1, "launch", launchArgs));
    handler.handleMessage(makeDapRequest(2, "configurationDone"));

    auto responses = handler.handleMessage(makeDapRequest(3, "stepIn"));
    EXPECT_GE(responses.size(), 2u);
    EXPECT_EQ(responses[0].value("command").toString().toStdString(), "stepIn");
}

TEST(DapRequestHandlerTest, ThreadsReturnsMainThread) {
    DapRequestHandler handler;
    auto responses = handler.handleMessage(makeDapRequest(1, "threads"));
    ASSERT_EQ(responses.size(), 1u);
    EXPECT_TRUE(responses[0].value("success").toBool());

    QJsonObject body = responses[0].value("body").toObject();
    QJsonArray threads = body.value("threads").toArray();
    ASSERT_EQ(threads.size(), 1);
    EXPECT_EQ(threads[0].toObject().value("id").toInt(), kMainThreadId);
    EXPECT_EQ(threads[0].toObject().value("name").toString().toStdString(), "main");
}

TEST(DapRequestHandlerTest, StackTraceReturnsFrames) {
    DapRequestHandler handler;
    QJsonObject launchArgs = makeLaunchArgs("var x = 1;\nprint(x);\n", "/test.ml");
    handler.handleMessage(makeDapRequest(1, "launch", launchArgs));
    handler.handleMessage(makeDapRequest(2, "configurationDone"));

    QJsonObject stArgs;
    stArgs["threadId"] = kMainThreadId;
    auto responses = handler.handleMessage(makeDapRequest(3, "stackTrace", stArgs));
    ASSERT_EQ(responses.size(), 1u);
    EXPECT_TRUE(responses[0].value("success").toBool());

    QJsonObject body = responses[0].value("body").toObject();
    QJsonArray frames = body.value("stackFrames").toArray();
    ASSERT_GE(frames.size(), 1);
    EXPECT_EQ(body.value("totalFrames").toInt(), frames.size());

    QJsonObject firstFrame = frames[0].toObject();
    EXPECT_GT(firstFrame.value("line").toInt(), 0);
    EXPECT_FALSE(firstFrame.value("name").toString().isEmpty());
}

TEST(DapRequestHandlerTest, ScopesReturnsLocalsAndGlobals) {
    DapRequestHandler handler;
    QJsonObject launchArgs = makeLaunchArgs("var x = 1;\nprint(x);\n", "/test.ml");
    handler.handleMessage(makeDapRequest(1, "launch", launchArgs));
    handler.handleMessage(makeDapRequest(2, "configurationDone"));

    QJsonObject scopeArgs;
    scopeArgs["frameId"] = 0;
    auto responses = handler.handleMessage(makeDapRequest(3, "scopes", scopeArgs));
    ASSERT_EQ(responses.size(), 1u);

    QJsonObject body = responses[0].value("body").toObject();
    QJsonArray scopes = body.value("scopes").toArray();
    ASSERT_EQ(scopes.size(), 2);
    EXPECT_EQ(scopes[0].toObject().value("name").toString().toStdString(), "Locals");
    EXPECT_EQ(scopes[1].toObject().value("name").toString().toStdString(), "Globals");
}

TEST(DapRequestHandlerTest, VariablesReturnsGlobalVariables) {
    DapRequestHandler handler;
    // 顶层 var 声明在 MiniLang 中是全局变量，查询 Globals scope 验证
    QJsonObject launchArgs = makeLaunchArgs("var x = 42;\nvar y = 7;\nprint(x + y);\n", "/test.ml");
    handler.handleMessage(makeDapRequest(1, "launch", launchArgs));
    handler.handleMessage(makeDapRequest(2, "configurationDone"));

    // 在第 3 行设置断点，continue 到断点时 x 和 y 已定义
    QJsonObject bpArgs;
    QJsonArray bpArray;
    bpArray.append(QJsonObject{{"line", 3}});
    bpArgs["breakpoints"] = bpArray;
    handler.handleMessage(makeDapRequest(3, "setBreakpoints", bpArgs));
    handler.handleMessage(makeDapRequest(4, "continue"));

    // 查询 Globals scope（reference=2000）
    QJsonObject varArgs;
    varArgs["variablesReference"] = kGlobalsScopeReference;
    auto responses = handler.handleMessage(makeDapRequest(5, "variables", varArgs));
    ASSERT_EQ(responses.size(), 1u);

    QJsonObject body = responses[0].value("body").toObject();
    QJsonArray vars = body.value("variables").toArray();
    bool hasX = false;
    bool hasY = false;
    for (const auto& v : vars) {
        std::string name = v.toObject().value("name").toString().toStdString();
        if (name == "x") {
            hasX = true;
            EXPECT_EQ(v.toObject().value("value").toString().toStdString(), "42");
        }
        if (name == "y") {
            hasY = true;
            EXPECT_EQ(v.toObject().value("value").toString().toStdString(), "7");
        }
    }
    EXPECT_TRUE(hasX);
    EXPECT_TRUE(hasY);
}

TEST(DapRequestHandlerTest, EvaluateReturnsVariableValue) {
    DapRequestHandler handler;
    QJsonObject launchArgs = makeLaunchArgs("var x = 99;\nprint(x);\n", "/test.ml");
    handler.handleMessage(makeDapRequest(1, "launch", launchArgs));

    QJsonObject bpArgs;
    QJsonArray bpArray;
    bpArray.append(QJsonObject{{"line", 2}});
    bpArgs["breakpoints"] = bpArray;
    handler.handleMessage(makeDapRequest(2, "setBreakpoints", bpArgs));
    handler.handleMessage(makeDapRequest(3, "configurationDone"));
    handler.handleMessage(makeDapRequest(4, "continue"));

    QJsonObject evalArgs;
    evalArgs["expression"] = "x";
    evalArgs["frameId"] = 0;
    auto responses = handler.handleMessage(makeDapRequest(5, "evaluate", evalArgs));
    ASSERT_EQ(responses.size(), 1u);

    QJsonObject body = responses[0].value("body").toObject();
    EXPECT_EQ(body.value("result").toString().toStdString(), "99");
    EXPECT_EQ(body.value("type").toString().toStdString(), "int");
}

TEST(DapRequestHandlerTest, EvaluateUnknownVariableReturnsErrorMessage) {
    DapRequestHandler handler;
    QJsonObject launchArgs = makeLaunchArgs("var x = 1;\nprint(x);\n", "/test.ml");
    handler.handleMessage(makeDapRequest(1, "launch", launchArgs));
    handler.handleMessage(makeDapRequest(2, "configurationDone"));

    QJsonObject evalArgs;
    evalArgs["expression"] = "nonexistent";
    evalArgs["frameId"] = 0;
    auto responses = handler.handleMessage(makeDapRequest(3, "evaluate", evalArgs));
    ASSERT_EQ(responses.size(), 1u);

    QJsonObject body = responses[0].value("body").toObject();
    QString result = body.value("result").toString();
    EXPECT_TRUE(result.contains("无法求值"));
}

TEST(DapRequestHandlerTest, TerminateResetsSessionAndShouldExit) {
    DapRequestHandler handler;
    QJsonObject launchArgs = makeLaunchArgs("var x = 1;\n", "/test.ml");
    handler.handleMessage(makeDapRequest(1, "launch", launchArgs));
    EXPECT_TRUE(handler.session().isLaunched());

    auto responses = handler.handleMessage(makeDapRequest(2, "terminate"));
    // 应该有 1 个 response + 1 个 terminated 事件
    ASSERT_EQ(responses.size(), 2u);
    EXPECT_EQ(responses[0].value("type").toString().toStdString(), "response");
    EXPECT_EQ(responses[1].value("type").toString().toStdString(), "event");
    EXPECT_EQ(responses[1].value("event").toString().toStdString(), "terminated");
    EXPECT_TRUE(handler.shouldExit());
    EXPECT_FALSE(handler.session().isLaunched());
}

TEST(DapRequestHandlerTest, DisconnectSetsShouldExit) {
    DapRequestHandler handler;
    auto responses = handler.handleMessage(makeDapRequest(1, "disconnect"));
    ASSERT_EQ(responses.size(), 1u);
    EXPECT_TRUE(responses[0].value("success").toBool());
    EXPECT_TRUE(handler.shouldExit());
}

TEST(DapRequestHandlerTest, PauseReturnsSuccessResponse) {
    // L21: pause 请求现在返回成功响应（原为错误）。
    // 未 launch 时仅返回响应（无 stopped 事件，因 session 未启动）。
    DapRequestHandler handler;
    auto responses = handler.handleMessage(makeDapRequest(1, "pause"));
    ASSERT_EQ(responses.size(), 1u);
    EXPECT_TRUE(responses[0].value("success").toBool());
    // pause 不应导致 shouldExit
    EXPECT_FALSE(handler.shouldExit());
    // pauseRequested_ 标志应被设置
    EXPECT_TRUE(handler.session().isPauseRequested());
}

TEST(DapRequestHandlerTest, PauseBeforeContinueSetsFlagAndStopsImmediately) {
    // L21: pause 在 continue 之前到达（continue 未在执行）。
    // 已 launch 但未 finished 时，应发送 stopped(Pause) 事件。
    DapRequestHandler handler;
    // launch 一个简单程序
    handler.session().launch("var x = 1;", "test.mini");
    // 清除 launch 产生的输出，便于后续检查
    handler.session().clearOutput();

    auto responses = handler.handleMessage(makeDapRequest(1, "pause"));
    // 应有 2 个响应：pause 成功响应 + stopped(Pause) 事件
    ASSERT_EQ(responses.size(), 2u);
    EXPECT_TRUE(responses[0].value("success").toBool());
    EXPECT_EQ(responses[0].value("command").toString().toStdString(), "pause");
    // 第二个是 stopped 事件
    EXPECT_EQ(responses[1].value("event").toString().toStdString(), "stopped");
    EXPECT_EQ(responses[1].value("body").toObject().value("reason").toString().toStdString(), "pause");
    // pauseRequested_ 应被清除（已通过 stopped 事件消费）
    EXPECT_FALSE(handler.session().isPauseRequested());
}

TEST(DapRequestHandlerTest, PauseRequestedFlagStopsContinueImmediately) {
    // L21: 预先设置 pauseRequested_ 标志，doContinue 第一轮检查即退出。
    DapRequestHandler handler;
    // launch 一个会无限循环的程序
    handler.session().launch("var i = 0; while (i < 1000000) { i = i + 1; }", "test.mini");
    handler.session().clearOutput();

    // 预设 pause 标志（模拟 pause 在 continue 之前到达）
    handler.session().requestPause();

    // 发送 continue 请求——doContinue 应在第一轮检查即退出
    auto responses = handler.handleMessage(makeDapRequest(1, "continue"));
    // 应有 2 个响应：continue 成功响应 + stopped(Pause) 事件
    ASSERT_EQ(responses.size(), 2u);
    EXPECT_TRUE(responses[0].value("success").toBool());
    EXPECT_EQ(responses[1].value("event").toString().toStdString(), "stopped");
    EXPECT_EQ(responses[1].value("body").toObject().value("reason").toString().toStdString(), "pause");
    // pauseRequested_ 应被清除
    EXPECT_FALSE(handler.session().isPauseRequested());
}

TEST(DapRequestHandlerTest, StdinPollCallbackTriggersPauseDuringContinue) {
    // L21: stdin 轮询回调检测到数据时，doContinue 应退出并发送 stopped(Pause)。
    DapRequestHandler handler;
    // launch 一个会执行很多步的程序
    handler.session().launch("var i = 0; while (i < 1000000) { i = i + 1; } print(i);", "test.mini");
    handler.session().clearOutput();

    // 注入轮询回调：第 3 次调用返回 true（模拟 stdin 有 pause 消息）
    int pollCount = 0;
    handler.session().setStdinPollCallback([&pollCount]() {
        ++pollCount;
        return pollCount >= 3; // 第 3 次轮询时返回 true
    });

    auto responses = handler.handleMessage(makeDapRequest(1, "continue"));
    // 应有 2 个响应：continue 成功响应 + stopped(Pause) 事件
    ASSERT_GE(responses.size(), 2u);
    EXPECT_TRUE(responses[0].value("success").toBool());
    // 找到 stopped 事件
    bool foundStop = false;
    for (const auto& resp : responses) {
        if (resp.value("event").toString().toStdString() == "stopped") {
            foundStop = true;
            EXPECT_EQ(resp.value("body").toObject().value("reason").toString().toStdString(), "pause");
            break;
        }
    }
    EXPECT_TRUE(foundStop);
    // 轮询回调应被调用至少 3 次
    EXPECT_GE(pollCount, 3);
    // pauseRequested_ 应被清除
    EXPECT_FALSE(handler.session().isPauseRequested());
}

TEST(DapRequestHandlerTest, UnknownCommandReturnsErrorResponse) {
    DapRequestHandler handler;
    auto responses = handler.handleMessage(makeDapRequest(1, "nonexistentCommand"));
    ASSERT_EQ(responses.size(), 1u);
    EXPECT_FALSE(responses[0].value("success").toBool());
    EXPECT_TRUE(responses[0].value("message").toString().contains("unknown command"));
}

TEST(DapRequestHandlerTest, NonRequestMessageReturnsNoResponses) {
    DapRequestHandler handler;
    QJsonObject msg;
    msg["type"] = "event"; // 非 request
    msg["event"] = "stopped";
    auto responses = handler.handleMessage(msg);
    EXPECT_TRUE(responses.empty());
}

TEST(DapRequestHandlerTest, SourceRequestReturnsErrorResponse) {
    DapRequestHandler handler;
    auto responses = handler.handleMessage(makeDapRequest(1, "source"));
    ASSERT_EQ(responses.size(), 1u);
    EXPECT_FALSE(responses[0].value("success").toBool());
}

TEST(DapRequestHandlerTest, StackTraceWithLevelsLimitsFrames) {
    DapRequestHandler handler;
    QJsonObject launchArgs = makeLaunchArgs("var x = 1;\nprint(x);\n", "/test.ml");
    handler.handleMessage(makeDapRequest(1, "launch", launchArgs));
    handler.handleMessage(makeDapRequest(2, "configurationDone"));

    QJsonObject stArgs;
    stArgs["threadId"] = kMainThreadId;
    stArgs["startFrame"] = 0;
    stArgs["levels"] = 1; // 只请求 1 帧
    auto responses = handler.handleMessage(makeDapRequest(3, "stackTrace", stArgs));
    ASSERT_EQ(responses.size(), 1u);

    QJsonObject body = responses[0].value("body").toObject();
    QJsonArray frames = body.value("stackFrames").toArray();
    EXPECT_LE(frames.size(), 1);
}

TEST(DapRequestHandlerTest, ContinueEmitsOutputEventForPrint) {
    DapRequestHandler handler;
    QJsonObject launchArgs = makeLaunchArgs("print(\"test_output\");\n", "/test.ml");
    handler.handleMessage(makeDapRequest(1, "launch", launchArgs));
    handler.handleMessage(makeDapRequest(2, "configurationDone"));

    auto responses = handler.handleMessage(makeDapRequest(3, "continue"));
    // 应该至少有 response + output + terminated
    EXPECT_GE(responses.size(), 2u);

    // 查找 output 事件
    bool foundOutput = false;
    for (const auto& r : responses) {
        if (r.value("type").toString() == "event" && r.value("event").toString() == "output") {
            foundOutput = true;
            QString output = r.value("body").toObject().value("output").toString();
            EXPECT_TRUE(output.contains("test_output"));
            break;
        }
    }
    EXPECT_TRUE(foundOutput);
}

// ============================================================
// 5. 端到端：完整 DAP 会话模拟
// ============================================================

TEST(DapEndToEndTest, FullSessionInitializeLaunchSetBpConfigDoneContinueTerminate) {
    DapTransport transport;
    DapRequestHandler handler;

    // 1. initialize
    transport.setInput(makeDapMessage(makeDapRequest(1, "initialize")));
    auto msg = transport.read();
    ASSERT_TRUE(msg.has_value());
    auto responses = handler.handleMessage(*msg);
    ASSERT_EQ(responses.size(), 1u);
    transport.write(responses[0]);
    EXPECT_FALSE(transport.outputBuffer().empty());
    transport.outputBuffer().clear();
    EXPECT_FALSE(handler.shouldExit());

    // 2. launch
    QJsonObject launchArgs = makeLaunchArgs("var x = 1;\nprint(x);\nvar y = 2;\nprint(y);\n", "/test.ml");
    transport.setInput(makeDapMessage(makeDapRequest(2, "launch", launchArgs)));
    msg = transport.read();
    ASSERT_TRUE(msg.has_value());
    responses = handler.handleMessage(*msg);
    ASSERT_EQ(responses.size(), 1u);
    EXPECT_TRUE(responses[0].value("success").toBool());
    EXPECT_TRUE(handler.session().isLaunched());

    // 3. setBreakpoints
    QJsonObject bpArgs;
    bpArgs["source"] = QJsonObject{{"path", "/test.ml"}};
    QJsonArray bpArray;
    bpArray.append(QJsonObject{{"line", 3}});
    bpArgs["breakpoints"] = bpArray;
    transport.setInput(makeDapMessage(makeDapRequest(3, "setBreakpoints", bpArgs)));
    msg = transport.read();
    ASSERT_TRUE(msg.has_value());
    responses = handler.handleMessage(*msg);
    ASSERT_EQ(responses.size(), 1u);
    EXPECT_TRUE(responses[0].value("success").toBool());

    // 4. configurationDone → 应触发 stopped(entry)
    transport.setInput(makeDapMessage(makeDapRequest(4, "configurationDone")));
    msg = transport.read();
    ASSERT_TRUE(msg.has_value());
    responses = handler.handleMessage(*msg);
    ASSERT_EQ(responses.size(), 2u);
    EXPECT_EQ(responses[1].value("event").toString().toStdString(), "stopped");

    // 5. continue → 应在断点行 3 暂停
    transport.setInput(makeDapMessage(makeDapRequest(5, "continue")));
    msg = transport.read();
    ASSERT_TRUE(msg.has_value());
    responses = handler.handleMessage(*msg);
    EXPECT_GE(responses.size(), 2u);
    QJsonObject lastEvent = responses.back();
    EXPECT_EQ(lastEvent.value("event").toString().toStdString(), "stopped");

    // 6. stackTrace
    QJsonObject stArgs;
    stArgs["threadId"] = kMainThreadId;
    transport.setInput(makeDapMessage(makeDapRequest(6, "stackTrace", stArgs)));
    msg = transport.read();
    ASSERT_TRUE(msg.has_value());
    responses = handler.handleMessage(*msg);
    ASSERT_EQ(responses.size(), 1u);
    QJsonArray frames = responses[0].value("body").toObject().value("stackFrames").toArray();
    EXPECT_GE(frames.size(), 1);

    // 7. scopes
    QJsonObject scopeArgs;
    scopeArgs["frameId"] = 0;
    transport.setInput(makeDapMessage(makeDapRequest(7, "scopes", scopeArgs)));
    msg = transport.read();
    ASSERT_TRUE(msg.has_value());
    responses = handler.handleMessage(*msg);
    ASSERT_EQ(responses.size(), 1u);
    QJsonArray scopes = responses[0].value("body").toObject().value("scopes").toArray();
    EXPECT_EQ(scopes.size(), 2);

    // 8. variables（Locals）
    QJsonObject varArgs;
    varArgs["variablesReference"] = kLocalsScopeReference + 0;
    transport.setInput(makeDapMessage(makeDapRequest(8, "variables", varArgs)));
    msg = transport.read();
    ASSERT_TRUE(msg.has_value());
    responses = handler.handleMessage(*msg);
    ASSERT_EQ(responses.size(), 1u);

    // 9. continue → 应执行到结束（terminated）
    transport.setInput(makeDapMessage(makeDapRequest(9, "continue")));
    msg = transport.read();
    ASSERT_TRUE(msg.has_value());
    responses = handler.handleMessage(*msg);
    EXPECT_GE(responses.size(), 2u);
    EXPECT_EQ(responses.back().value("event").toString().toStdString(), "terminated");

    // 10. disconnect
    transport.setInput(makeDapMessage(makeDapRequest(10, "disconnect")));
    msg = transport.read();
    ASSERT_TRUE(msg.has_value());
    responses = handler.handleMessage(*msg);
    ASSERT_EQ(responses.size(), 1u);
    EXPECT_TRUE(handler.shouldExit());
}

TEST(DapEndToEndTest, StepByStepExecution) {
    DapRequestHandler handler;
    QJsonObject launchArgs = makeLaunchArgs("var x = 1;\nvar y = 2;\nvar z = x + y;\nprint(z);\n", "/test.ml");
    handler.handleMessage(makeDapRequest(1, "launch", launchArgs));
    handler.handleMessage(makeDapRequest(2, "configurationDone"));

    // 连续 next 步进直到结束
    int stepCount = 0;
    int maxSteps = 20; // 防止无限循环
    while (stepCount < maxSteps) {
        auto responses = handler.handleMessage(makeDapRequest(100 + stepCount, "next"));
        ++stepCount;

        QJsonObject lastEvent = responses.back();
        if (lastEvent.value("event").toString().toStdString() == "terminated") {
            break;
        }
        EXPECT_EQ(lastEvent.value("event").toString().toStdString(), "stopped");
    }
    EXPECT_LT(stepCount, maxSteps); // 应该在 maxSteps 内结束
    EXPECT_TRUE(handler.session().isFinished());
}

// ============================================================
// 6. CLI 辅助函数
// ============================================================

TEST(DapCliTest, VersionStringIsNonEmpty) {
    EXPECT_FALSE(versionString().empty());
}

TEST(DapCliTest, ServerNameIsMinilangDap) {
    EXPECT_EQ(serverName(), "minilang-dap");
}

TEST(DapCliTest, VersionStringContainsR165) {
    EXPECT_NE(versionString().find("R165"), std::string::npos);
}

// ============================================================
// 7. DAP 二期：条件断点 / 命中条件 / setVariable / attach
// ============================================================

namespace {
/// DAP 二期测试共用源码：while 循环 5 轮
/// 行 4 = 循环体首行（s = s + i;），每轮到达一次
const char* kLoopSource = "var i = 0;\n"
                          "var s = 0;\n"
                          "while (i < 5) {\n"
                          "  s = s + i;\n"
                          "  i = i + 1;\n"
                          "}\n"
                          "print(s);\n";
} // namespace

TEST(DapPhase2Test, ConditionalBreakpointStopsWhenConditionTrue) {
    DebugSession session;
    session.launch(kLoopSource, "/test.ml");
    std::vector<DebugSession::SourceBreakpoint> bps;
    DebugSession::SourceBreakpoint bp;
    bp.line = 4;
    bp.condition = "i == 3";
    bps.push_back(bp);
    session.setSourceBreakpoints(bps);

    StepResult r = session.doContinue();
    ASSERT_EQ(r, StepResult::Ok);
    // 条件 i == 3：前 3 轮（i=0,1,2）不停，停下时 i 应为 3
    auto ev = session.evaluate("i", 0);
    ASSERT_TRUE(ev.ok);
    EXPECT_EQ(ev.value, "3");
}

TEST(DapPhase2Test, ConditionalBreakpointNeverTrueRunsToEnd) {
    DebugSession session;
    session.launch(kLoopSource, "/test.ml");
    std::vector<DebugSession::SourceBreakpoint> bps;
    DebugSession::SourceBreakpoint bp;
    bp.line = 4;
    bp.condition = "i > 100";
    bps.push_back(bp);
    session.setSourceBreakpoints(bps);

    StepResult r = session.doContinue();
    EXPECT_EQ(r, StepResult::Finished);
    // 条件永假：不计入命中计数（对齐 DebugController 条件分支语义）
    EXPECT_EQ(session.getBreakpointHitCount(4), 0);
}

TEST(DapPhase2Test, InvalidConditionTreatedAsNotHit) {
    DebugSession session;
    session.launch(kLoopSource, "/test.ml");
    std::vector<DebugSession::SourceBreakpoint> bps;
    DebugSession::SourceBreakpoint bp;
    bp.line = 4;
    bp.condition = "i ==="; // 语法错误：视为不命中，不崩溃
    bps.push_back(bp);
    session.setSourceBreakpoints(bps);

    StepResult r = session.doContinue();
    EXPECT_EQ(r, StepResult::Finished);
}

TEST(DapPhase2Test, HitConditionGreaterEqualSkipsEarlyHits) {
    DebugSession session;
    session.launch(kLoopSource, "/test.ml");
    std::vector<DebugSession::SourceBreakpoint> bps;
    DebugSession::SourceBreakpoint bp;
    bp.line = 4;
    bp.hitCondition = ">= 3";
    bps.push_back(bp);
    session.setSourceBreakpoints(bps);

    StepResult r = session.doContinue();
    ASSERT_EQ(r, StepResult::Ok);
    // 第 3 次到达循环体才停：i 应为 2
    auto ev = session.evaluate("i", 0);
    ASSERT_TRUE(ev.ok);
    EXPECT_EQ(ev.value, "2");
    EXPECT_EQ(session.getBreakpointHitCount(4), 3);
}

TEST(DapPhase2Test, HitConditionModuloStopsEveryN) {
    DebugSession session;
    session.launch(kLoopSource, "/test.ml");
    std::vector<DebugSession::SourceBreakpoint> bps;
    DebugSession::SourceBreakpoint bp;
    bp.line = 4;
    bp.hitCondition = "% 2";
    bps.push_back(bp);
    session.setSourceBreakpoints(bps);

    // 第一次 continue：第 2 次命中停（i=1）
    ASSERT_EQ(session.doContinue(), StepResult::Ok);
    auto ev = session.evaluate("i", 0);
    ASSERT_TRUE(ev.ok);
    EXPECT_EQ(ev.value, "1");
    // 第二次 continue：第 4 次命中停（i=3）
    ASSERT_EQ(session.doContinue(), StepResult::Ok);
    ev = session.evaluate("i", 0);
    ASSERT_TRUE(ev.ok);
    EXPECT_EQ(ev.value, "3");
}

TEST(DapPhase2Test, HitCountIncrementsOncePerLineArrival) {
    // 回归锁：doContinue 逐行跟踪后，同一断点行的多条指令不重复计数，
    // 单行循环每轮重新命中（对齐 DebugController crossedLine 语义）
    DebugSession session;
    session.launch(kLoopSource, "/test.ml");
    session.setBreakpoints({4});

    int stops = 0;
    while (stops < 10) {
        StepResult r = session.doContinue();
        if (r != StepResult::Ok)
            break;
        ++stops;
    }
    EXPECT_EQ(stops, 5);                             // 循环 5 轮，每轮停一次
    EXPECT_EQ(session.getBreakpointHitCount(4), 5);  // 计数与停次一致（无重复递增）
}

TEST(DapPhase2Test, SetVariableGlobalChangesExecution) {
    DebugSession session;
    session.launch("var x = 1;\nvar y = 0;\ny = x + 1;\nprint(y);\n", "/test.ml");
    session.setBreakpoints({3});
    ASSERT_EQ(session.doContinue(), StepResult::Ok);

    // 暂停在行 3（y = x + 1 执行前）：改 x = 41
    auto r = session.setVariable(kGlobalsScopeReference, "x", "41");
    ASSERT_TRUE(r.ok) << r.message;
    EXPECT_EQ(r.value, "41");

    // 继续执行：y = 41 + 1 = 42
    StepResult cont = session.doContinue();
    EXPECT_EQ(cont, StepResult::Finished);
    EXPECT_NE(session.getOutput().find("42"), std::string::npos) << session.getOutput();
}

TEST(DapPhase2Test, SetVariableRejectsUnknownName) {
    DebugSession session;
    session.launch("var x = 1;\nprint(x);\n", "/test.ml");
    session.setBreakpoints({2});
    ASSERT_EQ(session.doContinue(), StepResult::Ok);

    auto r = session.setVariable(kGlobalsScopeReference, "nonexistent", "1");
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.message.empty());
}

TEST(DapPhase2Test, SetVariableParsesTypedLiterals) {
    DebugSession session;
    session.launch("var a = 0;\nvar b = false;\nvar c = \"old\";\nprint(a);\n", "/test.ml");
    session.setBreakpoints({4});
    ASSERT_EQ(session.doContinue(), StepResult::Ok);

    EXPECT_TRUE(session.setVariable(kGlobalsScopeReference, "a", "3.5").ok);
    EXPECT_TRUE(session.setVariable(kGlobalsScopeReference, "b", "true").ok);
    auto rc = session.setVariable(kGlobalsScopeReference, "c", "\"new\"");
    ASSERT_TRUE(rc.ok);
    EXPECT_EQ(rc.type, "string");
    // 无法解析的文本拒绝（不支持任意表达式）
    EXPECT_FALSE(session.setVariable(kGlobalsScopeReference, "a", "1 + 2").ok);
}

TEST(DapPhase2Test, InitializeAdvertisesPhase2Capabilities) {
    DapRequestHandler handler;
    auto responses = handler.handleMessage(makeDapRequest(1, "initialize"));
    ASSERT_EQ(responses.size(), 1u);
    QJsonObject caps = responses[0].value("body").toObject().value("capabilities").toObject();
    EXPECT_TRUE(caps.value("supportsConditionalBreakpoints").toBool());
    EXPECT_TRUE(caps.value("supportsHitConditionalBreakpoints").toBool());
    EXPECT_TRUE(caps.value("supportsSetVariable").toBool());
}

TEST(DapPhase2Test, AttachBehavesLikeLaunch) {
    DapRequestHandler handler;
    QJsonObject attachArgs = makeLaunchArgs("var x = 1;\nprint(x);\n", "/test.ml");
    auto responses = handler.handleMessage(makeDapRequest(1, "attach", attachArgs));
    ASSERT_EQ(responses.size(), 1u);
    EXPECT_TRUE(responses[0].value("success").toBool());
    EXPECT_TRUE(handler.session().isLaunched());
}

TEST(DapPhase2Test, SetBreakpointsRequestParsesConditionFields) {
    DapRequestHandler handler;
    QJsonObject launchArgs = makeLaunchArgs(kLoopSource, "/test.ml");
    handler.handleMessage(makeDapRequest(1, "launch", launchArgs));

    QJsonObject bpArgs;
    QJsonArray bpArray;
    bpArray.append(QJsonObject{{"line", 4}, {"condition", "i == 2"}});
    bpArgs["breakpoints"] = bpArray;
    auto responses = handler.handleMessage(makeDapRequest(2, "setBreakpoints", bpArgs));
    ASSERT_EQ(responses.size(), 1u);
    EXPECT_TRUE(responses[0].value("success").toBool());
    handler.handleMessage(makeDapRequest(3, "configurationDone"));

    auto contResponses = handler.handleMessage(makeDapRequest(4, "continue"));
    QJsonObject lastEvent = contResponses.back();
    ASSERT_EQ(lastEvent.value("event").toString().toStdString(), "stopped");
    // 条件 i == 2 命中时验证变量值
    auto ev = handler.session().evaluate("i", 0);
    ASSERT_TRUE(ev.ok);
    EXPECT_EQ(ev.value, "2");
}

TEST(DapPhase2Test, SetVariableRequestViaHandler) {
    DapRequestHandler handler;
    QJsonObject launchArgs = makeLaunchArgs("var x = 1;\nvar y = 0;\ny = x + 1;\nprint(y);\n", "/test.ml");
    handler.handleMessage(makeDapRequest(1, "launch", launchArgs));
    QJsonObject bpArgs;
    QJsonArray bpArray;
    bpArray.append(QJsonObject{{"line", 3}});
    bpArgs["breakpoints"] = bpArray;
    handler.handleMessage(makeDapRequest(2, "setBreakpoints", bpArgs));
    handler.handleMessage(makeDapRequest(3, "configurationDone"));
    handler.handleMessage(makeDapRequest(4, "continue"));

    QJsonObject svArgs;
    svArgs["variablesReference"] = kGlobalsScopeReference;
    svArgs["name"] = "x";
    svArgs["value"] = "41";
    auto responses = handler.handleMessage(makeDapRequest(5, "setVariable", svArgs));
    ASSERT_EQ(responses.size(), 1u);
    EXPECT_TRUE(responses[0].value("success").toBool());
    EXPECT_EQ(responses[0].value("body").toObject().value("value").toString().toStdString(), "41");

    // 失败路径：未知变量 → success=false
    svArgs["name"] = "nope";
    responses = handler.handleMessage(makeDapRequest(6, "setVariable", svArgs));
    ASSERT_EQ(responses.size(), 1u);
    EXPECT_FALSE(responses[0].value("success").toBool());
}
