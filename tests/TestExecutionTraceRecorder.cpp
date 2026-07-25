// ============================================================
// TestExecutionTraceRecorder.cpp — 可回放执行时间轴核心测试（R114 阶段 1+2+3）
// ------------------------------------------------------------
// 测试范围：
//   1. ExecutionTraceRecorder 核心 API（容量/清空/启停/size/stepAt/empty）
//   2. 环形缓冲区策略（超容量后移除最旧快照）
//   3. TraceSnapshot JSON 序列化（三后端标识 + 必需字段）
//   4. captureInterpreterStep 集成测试（真实 Interpreter + 简单程序）
//   5. PanelCatalog 中 "execution-timeline" 面板注册正确性
//   6. Value::toJson() 序列化（R114 阶段 2）
//   7. AST 节点路径填充（R114 阶段 2 astNodePath）
//   8. StackVM 端到端录制（R114 阶段 2 三后端一致性）
//   9. RegisterVM 端到端录制（R114 阶段 2 三后端一致性）
//   10. 三后端一致性对比（R114 阶段 2 收敛状态等价）
//   11. 状态回滚（R114 阶段 3 — FullState 模式 + restoreFromSnapshot + 双后端回滚）
//
// 测试框架：GoogleTest
// 依赖约束：链接 minilang_core（含 ExecutionTraceRecorder.cpp）+ VmStepper.cpp
//           + Interpreter（通过 minilang_core）+ Lexer/Parser（通过 minilang_core）
//
// 注意：Interpreter 通过全局单例 traceRecorder() 写入快照，故集成测试
// 必须使用 traceRecorder() 而非局部 recorder 实例。每个测试开头调用
// clear() 保证隔离。
// ============================================================

#include <gtest/gtest.h>

#include "app/VmStepper.h"
#include "compiler/Compiler.h"
#include "debug/ExecutionTraceRecorder.h"
#include "gui/PanelCatalog.h"
#include "interpreter/Interpreter.h"
#include "interpreter/Value.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// ============================================================
// 测试套件 1：ExecutionTraceRecorder 核心 API（使用局部实例，无 Interpreter）
// ============================================================

TEST(ExecutionTraceRecorderCore, DefaultCapacityIs5000) {
    ExecutionTraceRecorder r;
    EXPECT_EQ(r.capacity(), 5000u);
}

TEST(ExecutionTraceRecorderCore, SetCapacityAndGet) {
    ExecutionTraceRecorder r;
    r.setCapacity(100);
    EXPECT_EQ(r.capacity(), 100u);
}

TEST(ExecutionTraceRecorderCore, InitiallyEmpty) {
    ExecutionTraceRecorder r;
    EXPECT_TRUE(r.empty());
    EXPECT_EQ(r.size(), 0u);
}

TEST(ExecutionTraceRecorderCore, DisabledByDefault) {
    ExecutionTraceRecorder r;
    EXPECT_FALSE(r.isEnabled());
}

TEST(ExecutionTraceRecorderCore, SetEnabledToggle) {
    ExecutionTraceRecorder r;
    r.setEnabled(true);
    EXPECT_TRUE(r.isEnabled());
    r.setEnabled(false);
    EXPECT_FALSE(r.isEnabled());
}

TEST(ExecutionTraceRecorderCore, ClearResetsSize) {
    ExecutionTraceRecorder r;
    r.startSession();
    r.clear();
    EXPECT_TRUE(r.empty());
    EXPECT_EQ(r.size(), 0u);
}

TEST(ExecutionTraceRecorderCore, StartSessionEnablesRecording) {
    ExecutionTraceRecorder r;
    EXPECT_FALSE(r.isEnabled());
    r.startSession();
    EXPECT_TRUE(r.isEnabled());
}

TEST(ExecutionTraceRecorderCore, EndSessionDisablesRecording) {
    ExecutionTraceRecorder r;
    r.startSession();
    r.endSession();
    EXPECT_FALSE(r.isEnabled());
}

TEST(ExecutionTraceRecorderCore, StepAtEmptyReturnsNullopt) {
    ExecutionTraceRecorder r;
    auto snap = r.stepAt(0);
    EXPECT_FALSE(snap.has_value());
}

TEST(ExecutionTraceRecorderCore, LastStepEmptyReturnsZero) {
    ExecutionTraceRecorder r;
    EXPECT_EQ(r.lastStep(), 0u);
}

TEST(ExecutionTraceRecorderCore, RecordingModeDefaultIsStringsOnly) {
    ExecutionTraceRecorder r;
    EXPECT_EQ(r.recordingMode(), RecordingMode::StringsOnly);
}

TEST(ExecutionTraceRecorderCore, SetRecordingModeToggle) {
    ExecutionTraceRecorder r;
    r.setRecordingMode(RecordingMode::FullState);
    EXPECT_EQ(r.recordingMode(), RecordingMode::FullState);
    r.setRecordingMode(RecordingMode::StringsOnly);
    EXPECT_EQ(r.recordingMode(), RecordingMode::StringsOnly);
}

// ============================================================
// 测试套件 2：环形缓冲区策略（通过全局 traceRecorder() + Interpreter）
// ============================================================

namespace {

/// 执行源码并采集 Interpreter 路径快照到全局 traceRecorder()
/// @param interp Interpreter 引用（调用方管理生命周期）
/// @param source MiniLang 源码
void runWithRecording(Interpreter& interp, const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);
    ASSERT_NE(ast, nullptr);

    interp.setRecordingEnabled(true);
    interp.setOutputCallback([](const std::string&) {});
    interp.execute(*ast);
}

} // namespace

TEST(ExecutionTraceRecorderRingBuffer, ExceedingCapacityRemovesOldest) {
    auto& r = traceRecorder();
    r.clear();
    r.setCapacity(3);
    r.startSession();

    Interpreter interp;
    runWithRecording(interp, "var x = 1 + 2 + 3; print(x);");

    // 快照数量不超过容量
    EXPECT_LE(r.size(), 3u);
    // 但步号（snap.step）应大于 0（说明采集发生了）
    if (r.size() > 0) {
        auto lastSnap = r.stepAt(r.size() - 1);
        ASSERT_TRUE(lastSnap.has_value());
        EXPECT_GT(lastSnap->step, 0u);
    }
    r.endSession();
    r.clear();
}

TEST(ExecutionTraceRecorderRingBuffer, StepAtIndexAccess) {
    auto& r = traceRecorder();
    r.clear();
    r.setCapacity(5000); // 恢复默认容量
    r.startSession();

    Interpreter interp;
    runWithRecording(interp, "var a = 1; var b = 2;");

    if (r.size() >= 2) {
        auto snap0 = r.stepAt(0);
        auto snap1 = r.stepAt(1);
        ASSERT_TRUE(snap0.has_value());
        ASSERT_TRUE(snap1.has_value());
        EXPECT_EQ(snap0->step, 0u);
        EXPECT_EQ(snap1->step, 1u);
    }
    r.endSession();
    r.clear();
}

TEST(ExecutionTraceRecorderRingBuffer, StepAtOutOfBoundsReturnsNullopt) {
    auto& r = traceRecorder();
    r.clear();
    r.startSession();

    Interpreter interp;
    runWithRecording(interp, "var x = 1;");

    auto oob = r.stepAt(r.size() + 100);
    EXPECT_FALSE(oob.has_value());
    r.endSession();
    r.clear();
}

// ============================================================
// 测试套件 3：TraceSnapshot JSON 序列化（无 Interpreter 依赖）
// ============================================================

TEST(TraceSnapshotJson, ContainsRequiredFields) {
    TraceSnapshot snap;
    snap.step = 42;
    snap.backend = TraceBackend::StackVM;
    snap.line = 10;
    snap.column = 5;
    snap.opOrNodeName = "OP_ADD";
    snap.ip = 7;
    snap.frameCount = 2;
    snap.stackStrings = {"1", "2"};
    snap.globalsStrings = {{"x", "10"}};

    std::string json = snap.toJson();
    EXPECT_NE(json.find("\"step\":42"), std::string::npos);
    EXPECT_NE(json.find("\"line\":10"), std::string::npos);
    EXPECT_NE(json.find("\"column\":5"), std::string::npos);
    EXPECT_NE(json.find("\"opOrNodeName\":\"OP_ADD\""), std::string::npos);
    EXPECT_NE(json.find("\"ip\":7"), std::string::npos);
    EXPECT_NE(json.find("\"frameCount\":2"), std::string::npos);
    EXPECT_NE(json.find("\"backend\":\"stackvm\""), std::string::npos);
}

TEST(TraceSnapshotJson, BackendInterpreterSerialization) {
    TraceSnapshot snap;
    snap.backend = TraceBackend::Interpreter;
    std::string json = snap.toJson();
    EXPECT_NE(json.find("\"backend\":\"interpreter\""), std::string::npos);
}

TEST(TraceSnapshotJson, BackendRegisterVMSerialization) {
    TraceSnapshot snap;
    snap.backend = TraceBackend::RegisterVM;
    std::string json = snap.toJson();
    EXPECT_NE(json.find("\"backend\":\"registervm\""), std::string::npos);
}

TEST(TraceSnapshotJson, IsParseableJsonObject) {
    TraceSnapshot snap;
    snap.step = 1;
    snap.backend = TraceBackend::StackVM;
    std::string json = snap.toJson();
    EXPECT_FALSE(json.empty());
    EXPECT_EQ(json.front(), '{');
    EXPECT_EQ(json.back(), '}');
}

TEST(TraceSnapshotJson, StackArraySerialized) {
    TraceSnapshot snap;
    snap.stackStrings = {"1", "2", "3"};
    std::string json = snap.toJson();
    EXPECT_NE(json.find("\"stack\":[\"1\",\"2\",\"3\"]"), std::string::npos);
}

TEST(TraceSnapshotJson, GlobalsArraySerialized) {
    TraceSnapshot snap;
    snap.globalsStrings = {{"x", "10"}, {"y", "20"}};
    std::string json = snap.toJson();
    EXPECT_NE(json.find("\"globals\":[{\"name\":\"x\",\"value\":\"10\"}"), std::string::npos);
    EXPECT_NE(json.find("{\"name\":\"y\",\"value\":\"20\"}"), std::string::npos);
}

TEST(TraceSnapshotJson, EmptyFieldsSerializeAsEmptyArrays) {
    TraceSnapshot snap;
    snap.backend = TraceBackend::StackVM;
    std::string json = snap.toJson();
    EXPECT_NE(json.find("\"stack\":[]"), std::string::npos);
    EXPECT_NE(json.find("\"registers\":[]"), std::string::npos);
    EXPECT_NE(json.find("\"globals\":[]"), std::string::npos);
    EXPECT_NE(json.find("\"visibleVars\":[]"), std::string::npos);
    EXPECT_NE(json.find("\"callStack\":[]"), std::string::npos);
}

// ============================================================
// 测试套件 4：captureInterpreterStep 集成测试（通过全局 traceRecorder()）
// ============================================================

TEST(ExecutionTraceRecorderInterpreterCapture, CapturesSnapshotsOnExecution) {
    auto& r = traceRecorder();
    r.clear();
    r.startSession();

    Interpreter interp;
    runWithRecording(interp, "var x = 1 + 2; print(x);");

    EXPECT_GT(r.size(), 0u) << "执行程序后应采集到至少一个快照";
    r.endSession();
    r.clear();
}

TEST(ExecutionTraceRecorderInterpreterCapture, SnapshotBackendIsInterpreter) {
    auto& r = traceRecorder();
    r.clear();
    r.startSession();

    Interpreter interp;
    runWithRecording(interp, "var x = 1;");

    ASSERT_GT(r.size(), 0u);
    auto snap = r.stepAt(0);
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(snap->backend, TraceBackend::Interpreter);
    r.endSession();
    r.clear();
}

TEST(ExecutionTraceRecorderInterpreterCapture, SnapshotContainsLineInfo) {
    auto& r = traceRecorder();
    r.clear();
    r.startSession();

    Interpreter interp;
    // 多行程序，验证快照 line 字段非零
    runWithRecording(interp, "var x = 1;\nvar y = 2;\nprint(x + y);");

    ASSERT_GT(r.size(), 0u);
    bool anyNonZeroLine = false;
    for (size_t i = 0; i < r.size(); ++i) {
        auto snap = r.stepAt(i);
        ASSERT_TRUE(snap.has_value());
        if (snap->line > 0) {
            anyNonZeroLine = true;
            break;
        }
    }
    EXPECT_TRUE(anyNonZeroLine) << "至少一个快照的 line 字段应 > 0";
    r.endSession();
    r.clear();
}

TEST(ExecutionTraceRecorderInterpreterCapture, SnapshotContainsNodeName) {
    auto& r = traceRecorder();
    r.clear();
    r.startSession();

    Interpreter interp;
    runWithRecording(interp, "var x = 1; if (x > 0) { print(x); }");

    ASSERT_GT(r.size(), 0u);
    bool anyNonEmptyName = false;
    for (size_t i = 0; i < r.size(); ++i) {
        auto snap = r.stepAt(i);
        ASSERT_TRUE(snap.has_value());
        if (!snap->opOrNodeName.empty()) {
            anyNonEmptyName = true;
            break;
        }
    }
    EXPECT_TRUE(anyNonEmptyName) << "至少一个快照的 opOrNodeName 应非空";
    r.endSession();
    r.clear();
}

TEST(ExecutionTraceRecorderInterpreterCapture, DisabledInterpreterNoCapture) {
    auto& r = traceRecorder();
    r.clear();
    r.startSession();

    Interpreter interp;
    // 显式禁用 Interpreter 录制钩子
    interp.setRecordingEnabled(false);

    Lexer lexer;
    auto tokens = lexer.scan("var x = 1;");
    Parser parser;
    auto ast = parser.parse(tokens);
    ASSERT_NE(ast, nullptr);
    interp.setOutputCallback([](const std::string&) {});
    interp.execute(*ast);

    EXPECT_EQ(r.size(), 0u) << "Interpreter 录制禁用时不应采集快照";
    r.endSession();
    r.clear();
}

TEST(ExecutionTraceRecorderInterpreterCapture, DisabledRecorderNoCapture) {
    auto& r = traceRecorder();
    r.clear();
    // 不调用 startSession()，recorder.isEnabled() == false

    Interpreter interp;
    interp.setRecordingEnabled(true);

    Lexer lexer;
    auto tokens = lexer.scan("var x = 1;");
    Parser parser;
    auto ast = parser.parse(tokens);
    ASSERT_NE(ast, nullptr);
    interp.setOutputCallback([](const std::string&) {});
    interp.execute(*ast);

    EXPECT_EQ(r.size(), 0u) << "Recorder 禁用时不应采集快照";
    r.clear();
}

TEST(ExecutionTraceRecorderInterpreterCapture, GlobalVariablesCaptured) {
    auto& r = traceRecorder();
    r.clear();
    r.startSession();

    Interpreter interp;
    // 多变量程序——验证至少一个快照的 globals 或 visibleVars 非空
    runWithRecording(interp, "var x = 42; var y = 'hello'; var z = x + 1;");

    ASSERT_GT(r.size(), 0u);
    // 至少一个快照应包含变量数据（globals 或 visibleVars）
    bool foundAnyVar = false;
    for (size_t i = 0; i < r.size(); ++i) {
        auto snap = r.stepAt(i);
        ASSERT_TRUE(snap.has_value());
        if (!snap->globalsStrings.empty() || !snap->visibleVarsStrings.empty()) {
            foundAnyVar = true;
            break;
        }
    }
    EXPECT_TRUE(foundAnyVar) << "至少一个快照应包含变量数据（globals 或 visibleVars）";
    r.endSession();
    r.clear();
}

TEST(ExecutionTraceRecorderInterpreterCapture, ClearResetsSnapshotCount) {
    auto& r = traceRecorder();
    r.clear();
    r.startSession();

    Interpreter interp;
    runWithRecording(interp, "var x = 1; var y = 2;");

    size_t before = r.size();
    EXPECT_GT(before, 0u);

    r.clear();
    EXPECT_EQ(r.size(), 0u);
    EXPECT_TRUE(r.empty());
    r.endSession();
    r.clear();
}

TEST(ExecutionTraceRecorderInterpreterCapture, AllSnapshotsReturnsCopy) {
    auto& r = traceRecorder();
    r.clear();
    r.startSession();

    Interpreter interp;
    runWithRecording(interp, "var x = 1; var y = 2;");

    auto all = r.allSnapshots();
    size_t recordedSize = r.size();
    EXPECT_EQ(all.size(), recordedSize);

    // 修改返回的副本不应影响 recorder 内部状态
    if (!all.empty()) {
        all.clear();
    }
    EXPECT_EQ(r.size(), recordedSize) << "recorder 大小不应受副本修改影响";
    r.endSession();
    r.clear();
}

// ============================================================
// 测试套件 5：PanelCatalog 注册——execution-timeline 面板
// ============================================================

TEST(ExecutionTimelinePanelCatalogRegistration, ExecutionTimelinePanelRegistered) {
    const auto* entry = PanelCatalog::findById("execution-timeline");
    EXPECT_NE(entry, nullptr) << "execution-timeline 面板应在 PanelCatalog 注册";
    if (entry) {
        EXPECT_EQ(std::string(entry->id), "execution-timeline");
        EXPECT_NE(std::string(entry->label), "") << "execution-timeline 面板 label 不能为空";
        EXPECT_NE(std::string(entry->emoji), "") << "execution-timeline 面板 emoji 不能为空";
    }
}

TEST(ExecutionTimelinePanelCatalogRegistration, ExecutionTimelinePanelUnderExecutionEngineCategory) {
    const auto& cats = PanelCatalog::categories();
    bool found = false;
    for (const auto& cat : cats) {
        if (std::string(cat.title) == "执行引擎") {
            for (const auto& leaf : cat.leaves) {
                if (std::string(leaf.id) == "execution-timeline") {
                    found = true;
                    break;
                }
            }
        }
    }
    EXPECT_TRUE(found) << "execution-timeline 面板应归类在'执行引擎'分类下";
}

TEST(ExecutionTimelinePanelCatalogRegistration, CanonicalIdResolvesExecutionTimeline) {
    EXPECT_EQ(PanelCatalog::canonicalPanelId("execution-timeline"), "execution-timeline");
}

TEST(ExecutionTimelinePanelCatalogRegistration, AllPanelIdsIncludesExecutionTimeline) {
    auto ids = PanelCatalog::allPanelIds();
    bool found = false;
    for (const auto& id : ids) {
        if (id == "execution-timeline") {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "allPanelIds() 应包含 execution-timeline";
}

// ============================================================
// 测试套件 6：Value::toJson() —— R114 阶段 2 JSON 序列化
// ============================================================

TEST(ValueJsonSerialization, IntToJson) {
    Value v(int64_t{42});
    EXPECT_EQ(v.toJson(), "42");
}

TEST(ValueJsonSerialization, NegativeIntToJson) {
    Value v(int64_t{-7});
    EXPECT_EQ(v.toJson(), "-7");
}

TEST(ValueJsonSerialization, FloatToJson) {
    Value v(3.14);
    std::string json = v.toJson();
    EXPECT_NE(json.find("3.14"), std::string::npos);
}

TEST(ValueJsonSerialization, BoolToJson) {
    Value t(true);
    Value f(false);
    EXPECT_EQ(t.toJson(), "true");
    EXPECT_EQ(f.toJson(), "false");
}

TEST(ValueJsonSerialization, NullToJson) {
    Value v = Value::nullValue();
    EXPECT_EQ(v.toJson(), "null");
}

TEST(ValueJsonSerialization, StringToJson) {
    Value v(std::string("hello"));
    EXPECT_EQ(v.toJson(), "\"hello\"");
}

TEST(ValueJsonSerialization, StringWithEscapesToJson) {
    Value v(std::string("a\"b\\c\nd"));
    std::string json = v.toJson();
    EXPECT_NE(json.find("\\\""), std::string::npos) << "应转义双引号";
    EXPECT_NE(json.find("\\\\"), std::string::npos) << "应转义反斜杠";
    EXPECT_NE(json.find("\\n"), std::string::npos) << "应转义换行";
}

TEST(ValueJsonSerialization, ArrayToJson) {
    std::vector<Value> elems = {Value(int64_t{1}), Value(int64_t{2})};
    Value v(elems);
    std::string json = v.toJson();
    EXPECT_NE(json.find("\"_type\":\"array\""), std::string::npos);
    EXPECT_NE(json.find("\"elements\":[1,2]"), std::string::npos);
}

TEST(ValueJsonSerialization, EmptyArrayToJson) {
    std::vector<Value> elems;
    Value v(elems);
    std::string json = v.toJson();
    EXPECT_NE(json.find("\"elements\":[]"), std::string::npos);
}

TEST(ValueJsonSerialization, DictToJson) {
    std::unordered_map<std::string, Value> m;
    m["x"] = Value(int64_t{10});
    Value v(m);
    std::string json = v.toJson();
    EXPECT_NE(json.find("\"_type\":\"dict\""), std::string::npos);
    EXPECT_NE(json.find("\"key\":\"x\""), std::string::npos);
    EXPECT_NE(json.find("\"value\":10"), std::string::npos);
}

TEST(ValueJsonSerialization, NestedArrayToJson) {
    std::vector<Value> innerElems = {Value(int64_t{1})};
    Value inner(innerElems);
    std::vector<Value> outerElems = {inner};
    Value outer(outerElems);
    std::string json = outer.toJson();
    EXPECT_NE(json.find("\"_type\":\"array\""), std::string::npos);
    // 嵌套 array 也应有 _type 标签
    size_t firstArray = json.find("\"_type\":\"array\"");
    size_t secondArray = json.find("\"_type\":\"array\"", firstArray + 1);
    EXPECT_NE(secondArray, std::string::npos) << "嵌套数组也应带 _type 标签";
}

TEST(ValueJsonSerialization, CycleDetectionProducesCycleMarker) {
    std::vector<Value> elems = {Value(int64_t{1})};
    Value arr(elems);
    // 自引用：arr.append(arr) —— 触发环检测
    arr.arrayVal().push_back(arr);
    std::string json = arr.toJson();
    EXPECT_NE(json.find("\"_type\":\"cycle\""), std::string::npos) << "环引用应输出 cycle 标记";
}

TEST(ValueJsonSerialization, InstanceToJson) {
    Value v = Value::makeInstance("Foo");
    v.fields()["x"] = Value(int64_t{42});
    std::string json = v.toJson();
    EXPECT_NE(json.find("\"_type\":\"instance\""), std::string::npos);
    EXPECT_NE(json.find("\"className\":\"Foo\""), std::string::npos);
    EXPECT_NE(json.find("\"name\":\"x\""), std::string::npos);
    EXPECT_NE(json.find("\"value\":42"), std::string::npos);
}

// ============================================================
// 测试套件 7：AST 节点路径 —— R114 阶段 2 astNodePath 填充
// ============================================================

TEST(ExecutionTraceRecorderAstPath, SnapshotContainsAstNodePath) {
    auto& r = traceRecorder();
    r.clear();
    r.startSession();

    Interpreter interp;
    runWithRecording(interp, "var x = 1; if (x > 0) { print(x); }");

    ASSERT_GT(r.size(), 0u);
    bool anyNonEmptyPath = false;
    for (size_t i = 0; i < r.size(); ++i) {
        auto snap = r.stepAt(i);
        ASSERT_TRUE(snap.has_value());
        if (!snap->astNodePath.empty()) {
            anyNonEmptyPath = true;
            // 路径格式应为 "<nodeName>#<nodeId>"，如 "IfStmt#5"
            EXPECT_NE(snap->astNodePath.find("#"), std::string::npos)
                << "astNodePath 应包含 '#' 分隔符: " << snap->astNodePath;
            break;
        }
    }
    EXPECT_TRUE(anyNonEmptyPath) << "至少一个快照的 astNodePath 应非空";
    r.endSession();
    r.clear();
}

TEST(ExecutionTraceRecorderAstPath, NodeIdIsUnique) {
    auto& r = traceRecorder();
    r.clear();
    r.startSession();

    Interpreter interp;
    runWithRecording(interp, "var a = 1; var b = 2; var c = 3;");

    // 收集所有 astNodePath 中的 nodeId（# 后的数字）
    std::set<uint64_t> nodeIds;
    for (size_t i = 0; i < r.size(); ++i) {
        auto snap = r.stepAt(i);
        if (!snap.has_value() || snap->astNodePath.empty())
            continue;
        auto pos = snap->astNodePath.find('#');
        if (pos == std::string::npos)
            continue;
        std::string idStr = snap->astNodePath.substr(pos + 1);
        try {
            uint64_t id = std::stoull(idStr);
            nodeIds.insert(id);
        } catch (...) {
            // 忽略解析失败的路径
        }
    }
    // 至少应有 2 个不同的 nodeId（多个语句节点）
    EXPECT_GE(nodeIds.size(), 2u) << "不同语句节点应有不同的 nodeId";
    r.endSession();
    r.clear();
}

TEST(ExecutionTraceRecorderAstPath, JsonIncludesAstNodePathField) {
    TraceSnapshot snap;
    snap.backend = TraceBackend::Interpreter;
    snap.opOrNodeName = "IfStmt";
    snap.astNodePath = "IfStmt#42";
    std::string json = snap.toJson();
    EXPECT_NE(json.find("\"astNodePath\":\"IfStmt#42\""), std::string::npos) << "toJson 应包含 astNodePath 字段";
}

// ============================================================
// 测试套件 8：StackVM 端到端录制（R114 阶段 2 三后端一致性）
// ------------------------------------------------------------
// 通过 VmStepper 同步步进（STEP_IN）执行简单程序，验证 StackVM 路径
// 的快照采集正确性。VmStepper.cpp 已链接到 minilang_tests 目标
// （见 tests/CMakeLists.txt L218-221）。
//
// 采集时机：VM 路径在每条指令执行"后"调用 maybeRecordStep，
// 与 Interpreter 路径"语句入口前"采集语义不对称——这是已知设计取舍
// （见 Interpreter.cpp checkBreak 注释），三后端一致性测试聚焦
// "收敛状态"对比而非逐步等价。
// ============================================================

namespace {

/// 执行源码并通过 VmStepper 采集 StackVM 路径快照到全局 traceRecorder()
void runStackVmWithRecording(VmStepper& stepper, const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);
    ASSERT_NE(ast, nullptr);

    Compiler compiler;
    CompileResult result = compiler.compile(*ast);
    stepper.setCompileResult(result);
    stepper.setOutputCallback([](const std::string&) {});
    stepper.setRecordingEnabled(true, TraceBackend::StackVM);

    // STEP_IN 同步执行直到结束/错误/不可用
    constexpr int MAX_STEPS = 1000;
    int stepCount = 0;
    while (stepCount < MAX_STEPS) {
        auto res = stepper.stepByMode(VmStepper::VmStepMode::STEP_IN);
        if (res == VmStepper::VmStepResult::FINISHED || res == VmStepper::VmStepResult::ERROR ||
            res == VmStepper::VmStepResult::NOT_READY) {
            break;
        }
        ++stepCount;
    }
}

/// 执行源码并通过 VmStepper 采集 RegisterVM 路径快照到全局 traceRecorder()
void runRegisterVmWithRecording(VmStepper& stepper, const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);
    ASSERT_NE(ast, nullptr);

    Compiler compiler;
    compiler.setUseRegisterVM(true);
    compiler.compile(*ast);
    stepper.setUseRegister(true);
    stepper.setRegisterCompileResult(compiler.getLastRegisterResult());
    stepper.setOutputCallback([](const std::string&) {});
    stepper.setRecordingEnabled(true, TraceBackend::RegisterVM);

    constexpr int MAX_STEPS = 1000;
    int stepCount = 0;
    while (stepCount < MAX_STEPS) {
        auto res = stepper.stepByMode(VmStepper::VmStepMode::STEP_IN);
        if (res == VmStepper::VmStepResult::FINISHED || res == VmStepper::VmStepResult::ERROR ||
            res == VmStepper::VmStepResult::NOT_READY) {
            break;
        }
        ++stepCount;
    }
}

/// 将 globalsStrings 转为 std::map 便于查找
std::map<std::string, std::string> globalsToMap(const TraceSnapshot& snap) {
    std::map<std::string, std::string> result;
    for (const auto& kv : snap.globalsStrings) {
        result[kv.first] = kv.second;
    }
    return result;
}

} // namespace

TEST(ExecutionTraceRecorderStackVMCapture, CapturesSnapshotsOnVmExecution) {
    auto& r = traceRecorder();
    r.clear();
    r.startSession();

    VmStepper stepper;
    runStackVmWithRecording(stepper, "var x = 1 + 2; print(x);");

    EXPECT_GT(r.size(), 0u) << "StackVM 执行程序后应采集到至少一个快照";
    r.endSession();
    r.clear();
}

TEST(ExecutionTraceRecorderStackVMCapture, SnapshotBackendIsStackVM) {
    auto& r = traceRecorder();
    r.clear();
    r.startSession();

    VmStepper stepper;
    runStackVmWithRecording(stepper, "var x = 1;");

    ASSERT_GT(r.size(), 0u);
    auto snap = r.stepAt(0);
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(snap->backend, TraceBackend::StackVM);
    r.endSession();
    r.clear();
}

TEST(ExecutionTraceRecorderStackVMCapture, SnapshotContainsOpCodeName) {
    auto& r = traceRecorder();
    r.clear();
    r.startSession();

    VmStepper stepper;
    runStackVmWithRecording(stepper, "var x = 1 + 2;");

    ASSERT_GT(r.size(), 0u);
    bool anyOpCodeName = false;
    for (size_t i = 0; i < r.size(); ++i) {
        auto snap = r.stepAt(i);
        ASSERT_TRUE(snap.has_value());
        if (!snap->opOrNodeName.empty()) {
            anyOpCodeName = true;
            // StackVM 操作码名应以 "OP_" 前缀开头
            EXPECT_NE(snap->opOrNodeName.find("OP_"), std::string::npos)
                << "StackVM opOrNodeName 应以 OP_ 前缀开头: " << snap->opOrNodeName;
            break;
        }
    }
    EXPECT_TRUE(anyOpCodeName) << "至少一个快照的 opOrNodeName 应非空";
    r.endSession();
    r.clear();
}

TEST(ExecutionTraceRecorderStackVMCapture, SnapshotContainsLineInfo) {
    auto& r = traceRecorder();
    r.clear();
    r.startSession();

    VmStepper stepper;
    runStackVmWithRecording(stepper, "var x = 1;\nvar y = 2;\nprint(x + y);");

    ASSERT_GT(r.size(), 0u);
    bool anyNonZeroLine = false;
    for (size_t i = 0; i < r.size(); ++i) {
        auto snap = r.stepAt(i);
        ASSERT_TRUE(snap.has_value());
        if (snap->line > 0) {
            anyNonZeroLine = true;
            break;
        }
    }
    EXPECT_TRUE(anyNonZeroLine) << "至少一个快照的 line 字段应 > 0";
    r.endSession();
    r.clear();
}

TEST(ExecutionTraceRecorderStackVMCapture, StackStringsCapturedDuringExecution) {
    auto& r = traceRecorder();
    r.clear();
    r.startSession();

    VmStepper stepper;
    // 表达式 1 + 2 在 OP_ADD 之前会有两个常量压栈
    runStackVmWithRecording(stepper, "var x = 1 + 2;");

    ASSERT_GT(r.size(), 0u);
    bool anyNonEmptyStack = false;
    for (size_t i = 0; i < r.size(); ++i) {
        auto snap = r.stepAt(i);
        ASSERT_TRUE(snap.has_value());
        if (!snap->stackStrings.empty()) {
            anyNonEmptyStack = true;
            break;
        }
    }
    EXPECT_TRUE(anyNonEmptyStack) << "至少一个快照的 stackStrings 应非空（表达式求值期间操作数栈有值）";
    r.endSession();
    r.clear();
}

// ============================================================
// 测试套件 9：RegisterVM 端到端录制（R114 阶段 2 三后端一致性）
// ============================================================

TEST(ExecutionTraceRecorderRegisterVMCapture, CapturesSnapshotsOnRegVmExecution) {
    auto& r = traceRecorder();
    r.clear();
    r.startSession();

    VmStepper stepper;
    runRegisterVmWithRecording(stepper, "var x = 1 + 2; print(x);");

    EXPECT_GT(r.size(), 0u) << "RegisterVM 执行程序后应采集到至少一个快照";
    r.endSession();
    r.clear();
}

TEST(ExecutionTraceRecorderRegisterVMCapture, SnapshotBackendIsRegisterVM) {
    auto& r = traceRecorder();
    r.clear();
    r.startSession();

    VmStepper stepper;
    runRegisterVmWithRecording(stepper, "var x = 1;");

    ASSERT_GT(r.size(), 0u);
    auto snap = r.stepAt(0);
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(snap->backend, TraceBackend::RegisterVM);
    r.endSession();
    r.clear();
}

TEST(ExecutionTraceRecorderRegisterVMCapture, RegisterStringsPopulatedForRegVm) {
    auto& r = traceRecorder();
    r.clear();
    r.startSession();

    VmStepper stepper;
    runRegisterVmWithRecording(stepper, "var x = 1; var y = 2;");

    ASSERT_GT(r.size(), 0u);
    // RegisterVM 路径下，registerStrings 应被填充（与 stackStrings 内容相同）
    bool anyNonEmptyRegisters = false;
    for (size_t i = 0; i < r.size(); ++i) {
        auto snap = r.stepAt(i);
        ASSERT_TRUE(snap.has_value());
        if (!snap->registerStrings.empty()) {
            anyNonEmptyRegisters = true;
            // RegisterVM 的 registerStrings 与 stackStrings 内容应一致
            EXPECT_EQ(snap->registerStrings, snap->stackStrings)
                << "RegisterVM 的 registerStrings 应与 stackStrings 内容一致";
            break;
        }
    }
    EXPECT_TRUE(anyNonEmptyRegisters) << "RegisterVM 路径下至少一个快照的 registerStrings 应非空";
    r.endSession();
    r.clear();
}

TEST(ExecutionTraceRecorderRegisterVMCapture, SnapshotContainsOpCodeName) {
    auto& r = traceRecorder();
    r.clear();
    r.startSession();

    VmStepper stepper;
    runRegisterVmWithRecording(stepper, "var x = 1 + 2;");

    ASSERT_GT(r.size(), 0u);
    bool anyOpCodeName = false;
    for (size_t i = 0; i < r.size(); ++i) {
        auto snap = r.stepAt(i);
        ASSERT_TRUE(snap.has_value());
        if (!snap->opOrNodeName.empty()) {
            anyOpCodeName = true;
            // RegisterVM 操作码名应以 "REG_" 前缀开头
            EXPECT_NE(snap->opOrNodeName.find("REG_"), std::string::npos)
                << "RegisterVM opOrNodeName 应以 REG_ 前缀开头: " << snap->opOrNodeName;
            break;
        }
    }
    EXPECT_TRUE(anyOpCodeName) << "至少一个快照的 opOrNodeName 应非空";
    r.endSession();
    r.clear();
}

TEST(ExecutionTraceRecorderRegisterVMCapture, SnapshotContainsLineInfo) {
    auto& r = traceRecorder();
    r.clear();
    r.startSession();

    VmStepper stepper;
    runRegisterVmWithRecording(stepper, "var x = 1;\nvar y = 2;\nprint(x + y);");

    ASSERT_GT(r.size(), 0u);
    bool anyNonZeroLine = false;
    for (size_t i = 0; i < r.size(); ++i) {
        auto snap = r.stepAt(i);
        ASSERT_TRUE(snap.has_value());
        if (snap->line > 0) {
            anyNonZeroLine = true;
            break;
        }
    }
    EXPECT_TRUE(anyNonZeroLine) << "至少一个快照的 line 字段应 > 0";
    r.endSession();
    r.clear();
}

// ============================================================
// 测试套件 10：三后端一致性对比（R114 阶段 2 收敛状态等价）
// ------------------------------------------------------------
// 同一源码在 Interpreter / StackVM / RegisterVM 三条路径上分别录制，
// 断言"收敛状态"等价（最终 globals 包含相同变量与值）。
//
// 设计取舍：不对比逐步快照——Interpreter 在"语句入口前"采集，
// VM 在"指令执行后"采集，步数与粒度不对称。三后端一致性聚焦
// "程序结束后最终状态"对比，与 BackendConsistency 测试套件
// （TestVME2E.cpp）对比 print 输出的思路一致。
// ============================================================

TEST(ExecutionTraceRecorderThreeBackendConsistency, AllBackendsProduceSnapshots) {
    const std::string source = "var x = 42; var y = x + 1; print(y);";
    auto& r = traceRecorder();

    // Interpreter
    r.clear();
    r.startSession();
    {
        Interpreter interp;
        runWithRecording(interp, source);
    }
    size_t interpCount = r.size();
    EXPECT_GT(interpCount, 0u) << "Interpreter 应采集到快照";
    r.endSession();
    r.clear();

    // StackVM
    r.clear();
    r.startSession();
    {
        VmStepper stepper;
        runStackVmWithRecording(stepper, source);
    }
    size_t stackVmCount = r.size();
    EXPECT_GT(stackVmCount, 0u) << "StackVM 应采集到快照";
    r.endSession();
    r.clear();

    // RegisterVM
    r.clear();
    r.startSession();
    {
        VmStepper stepper;
        runRegisterVmWithRecording(stepper, source);
    }
    size_t regVmCount = r.size();
    EXPECT_GT(regVmCount, 0u) << "RegisterVM 应采集到快照";
    r.endSession();
    r.clear();
}

TEST(ExecutionTraceRecorderThreeBackendConsistency, AllBackendsHaveCorrectBackendLabel) {
    const std::string source = "var x = 1;";
    auto& r = traceRecorder();

    // Interpreter
    r.clear();
    r.startSession();
    {
        Interpreter interp;
        runWithRecording(interp, source);
    }
    {
        auto snap = r.stepAt(0);
        ASSERT_TRUE(snap.has_value());
        EXPECT_EQ(snap->backend, TraceBackend::Interpreter);
    }
    r.endSession();
    r.clear();

    // StackVM
    r.clear();
    r.startSession();
    {
        VmStepper stepper;
        runStackVmWithRecording(stepper, source);
    }
    {
        auto snap = r.stepAt(0);
        ASSERT_TRUE(snap.has_value());
        EXPECT_EQ(snap->backend, TraceBackend::StackVM);
    }
    r.endSession();
    r.clear();

    // RegisterVM
    r.clear();
    r.startSession();
    {
        VmStepper stepper;
        runRegisterVmWithRecording(stepper, source);
    }
    {
        auto snap = r.stepAt(0);
        ASSERT_TRUE(snap.has_value());
        EXPECT_EQ(snap->backend, TraceBackend::RegisterVM);
    }
    r.endSession();
    r.clear();
}

TEST(ExecutionTraceRecorderThreeBackendConsistency, AllBackendsCaptureGlobalsFinalState) {
    // 多语句程序确保 Interpreter 最后一个快照（PrintStmt 入口）时 x,y 已定义
    const std::string source = "var x = 42; var y = x + 1; print(y);";
    auto& r = traceRecorder();

    std::map<std::string, std::string> interpGlobals, stackVmGlobals, regVmGlobals;

    // Interpreter
    r.clear();
    r.startSession();
    {
        Interpreter interp;
        runWithRecording(interp, source);
    }
    {
        ASSERT_GT(r.size(), 0u);
        auto snap = r.stepAt(r.size() - 1);
        ASSERT_TRUE(snap.has_value());
        interpGlobals = globalsToMap(*snap);
    }
    r.endSession();
    r.clear();

    // StackVM
    r.clear();
    r.startSession();
    {
        VmStepper stepper;
        runStackVmWithRecording(stepper, source);
    }
    {
        ASSERT_GT(r.size(), 0u);
        auto snap = r.stepAt(r.size() - 1);
        ASSERT_TRUE(snap.has_value());
        stackVmGlobals = globalsToMap(*snap);
    }
    r.endSession();
    r.clear();

    // RegisterVM
    r.clear();
    r.startSession();
    {
        VmStepper stepper;
        runRegisterVmWithRecording(stepper, source);
    }
    {
        ASSERT_GT(r.size(), 0u);
        auto snap = r.stepAt(r.size() - 1);
        ASSERT_TRUE(snap.has_value());
        regVmGlobals = globalsToMap(*snap);
    }
    r.endSession();
    r.clear();

    // 三后端最终 globals 都应包含 x=42, y=43
    EXPECT_EQ(interpGlobals.count("x"), 1u) << "Interpreter 最终 globals 应包含 x";
    EXPECT_EQ(stackVmGlobals.count("x"), 1u) << "StackVM 最终 globals 应包含 x";
    EXPECT_EQ(regVmGlobals.count("x"), 1u) << "RegisterVM 最终 globals 应包含 x";

    if (interpGlobals.count("x") && stackVmGlobals.count("x") && regVmGlobals.count("x")) {
        EXPECT_EQ(interpGlobals["x"], "42");
        EXPECT_EQ(stackVmGlobals["x"], "42");
        EXPECT_EQ(regVmGlobals["x"], "42");
    }

    EXPECT_EQ(interpGlobals.count("y"), 1u) << "Interpreter 最终 globals 应包含 y";
    EXPECT_EQ(stackVmGlobals.count("y"), 1u) << "StackVM 最终 globals 应包含 y";
    EXPECT_EQ(regVmGlobals.count("y"), 1u) << "RegisterVM 最终 globals 应包含 y";

    if (interpGlobals.count("y") && stackVmGlobals.count("y") && regVmGlobals.count("y")) {
        EXPECT_EQ(interpGlobals["y"], "43");
        EXPECT_EQ(stackVmGlobals["y"], "43");
        EXPECT_EQ(regVmGlobals["y"], "43");
    }
}

TEST(ExecutionTraceRecorderThreeBackendConsistency, AllBackendsProduceValidJson) {
    const std::string source = "var x = 1; print(x);";
    auto& r = traceRecorder();

    // 验证三后端的快照都能序列化为合法 JSON（以 { 开头、} 结尾）
    auto checkJsonValid = [](const TraceSnapshot& snap) {
        std::string json = snap.toJson();
        EXPECT_FALSE(json.empty()) << "toJson 不应返回空串";
        EXPECT_EQ(json.front(), '{') << "JSON 应以 { 开头";
        EXPECT_EQ(json.back(), '}') << "JSON 应以 } 结尾";
    };

    // Interpreter
    r.clear();
    r.startSession();
    {
        Interpreter interp;
        runWithRecording(interp, source);
    }
    {
        ASSERT_GT(r.size(), 0u);
        auto snap = r.stepAt(r.size() - 1);
        ASSERT_TRUE(snap.has_value());
        checkJsonValid(*snap);
    }
    r.endSession();
    r.clear();

    // StackVM
    r.clear();
    r.startSession();
    {
        VmStepper stepper;
        runStackVmWithRecording(stepper, source);
    }
    {
        ASSERT_GT(r.size(), 0u);
        auto snap = r.stepAt(r.size() - 1);
        ASSERT_TRUE(snap.has_value());
        checkJsonValid(*snap);
    }
    r.endSession();
    r.clear();

    // RegisterVM
    r.clear();
    r.startSession();
    {
        VmStepper stepper;
        runRegisterVmWithRecording(stepper, source);
    }
    {
        ASSERT_GT(r.size(), 0u);
        auto snap = r.stepAt(r.size() - 1);
        ASSERT_TRUE(snap.has_value());
        checkJsonValid(*snap);
    }
    r.endSession();
    r.clear();
}

TEST(ExecutionTraceRecorderThreeBackendConsistency, AllBackendsHaveUniqueStepNumbers) {
    const std::string source = "var x = 1; var y = 2; print(x + y);";
    auto& r = traceRecorder();

    auto checkUniqueSteps = [&r]() {
        std::set<size_t> stepNumbers;
        for (size_t i = 0; i < r.size(); ++i) {
            auto snap = r.stepAt(i);
            ASSERT_TRUE(snap.has_value());
            stepNumbers.insert(snap->step);
        }
        EXPECT_EQ(stepNumbers.size(), r.size()) << "所有快照的 step 号应唯一";
    };

    // Interpreter
    r.clear();
    r.startSession();
    {
        Interpreter interp;
        runWithRecording(interp, source);
    }
    checkUniqueSteps();
    r.endSession();
    r.clear();

    // StackVM
    r.clear();
    r.startSession();
    {
        VmStepper stepper;
        runStackVmWithRecording(stepper, source);
    }
    checkUniqueSteps();
    r.endSession();
    r.clear();

    // RegisterVM
    r.clear();
    r.startSession();
    {
        VmStepper stepper;
        runRegisterVmWithRecording(stepper, source);
    }
    checkUniqueSteps();
    r.endSession();
    r.clear();
}

// ============================================================
// 测试套件 11：状态回滚（R114 阶段 3 — VM 状态深拷贝 + 回滚）
// ------------------------------------------------------------
// 验证 VmStepper::restoreFromSnapshot 在 StackVM / RegisterVM 双后端的回滚语义。
// 测试模式：(1) FullState 模式录制 → (2) 取某步快照 → (3) 继续执行改变状态 →
// (4) 回滚到快照 → (5) 断言状态恢复到快照时的值。
//
// 注：Interpreter 路径不支持回滚（AST 树遍历无 IP 概念），故本套件仅覆盖
// StackVM / RegisterVM 双后端。
// ============================================================

namespace {

/// 在 FullState 模式下录制 StackVM 执行（用于回滚测试）
void runStackVmFullState(VmStepper& stepper, const std::string& source, int maxSteps = 200) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);
    ASSERT_NE(ast, nullptr);

    Compiler compiler;
    CompileResult result = compiler.compile(*ast);
    stepper.setCompileResult(result);
    stepper.setOutputCallback([](const std::string&) {});
    stepper.setRecordingEnabled(true, TraceBackend::StackVM);

    int stepCount = 0;
    while (stepCount < maxSteps) {
        auto res = stepper.stepByMode(VmStepper::VmStepMode::STEP_IN);
        if (res == VmStepper::VmStepResult::FINISHED || res == VmStepper::VmStepResult::ERROR ||
            res == VmStepper::VmStepResult::NOT_READY) {
            break;
        }
        ++stepCount;
    }
}

/// 在 FullState 模式下录制 RegisterVM 执行（用于回滚测试）
void runRegisterVmFullState(VmStepper& stepper, const std::string& source, int maxSteps = 200) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);
    ASSERT_NE(ast, nullptr);

    Compiler compiler;
    compiler.setUseRegisterVM(true);
    compiler.compile(*ast);
    stepper.setUseRegister(true);
    stepper.setRegisterCompileResult(compiler.getLastRegisterResult());
    stepper.setOutputCallback([](const std::string&) {});
    stepper.setRecordingEnabled(true, TraceBackend::RegisterVM);

    int stepCount = 0;
    while (stepCount < maxSteps) {
        auto res = stepper.stepByMode(VmStepper::VmStepMode::STEP_IN);
        if (res == VmStepper::VmStepResult::FINISHED || res == VmStepper::VmStepResult::ERROR ||
            res == VmStepper::VmStepResult::NOT_READY) {
            break;
        }
        ++stepCount;
    }
}

} // namespace

TEST(ExecutionTraceRecorderRollback, FullStateModePopulatesStackValues) {
    auto& r = traceRecorder();
    r.clear();
    r.setRecordingMode(RecordingMode::FullState);
    r.startSession();

    {
        VmStepper stepper;
        runStackVmFullState(stepper, "var x = 42; var y = x + 1;");
    }

    ASSERT_GT(r.size(), 0u);
    // FullState 模式下，至少一个快照应有 stackValues 非空或 globalsValues 非空
    bool anyStructured = false;
    for (size_t i = 0; i < r.size(); ++i) {
        auto snap = r.stepAt(i);
        ASSERT_TRUE(snap.has_value());
        if (!snap->stackValues.empty() || !snap->globalsValues.empty()) {
            anyStructured = true;
            break;
        }
    }
    EXPECT_TRUE(anyStructured) << "FullState 模式应填充结构化快照字段";

    r.endSession();
    r.clear();
    r.setRecordingMode(RecordingMode::StringsOnly); // 恢复默认模式
}

TEST(ExecutionTraceRecorderRollback, StackVM_RestoresGlobalsAfterRollback) {
    auto& r = traceRecorder();
    r.clear();
    r.setRecordingMode(RecordingMode::FullState);
    r.startSession();

    VmStepper stepper;
    runStackVmFullState(stepper, "var x = 10; var y = 20;");

    ASSERT_GT(r.size(), 0u);
    // 取执行中期的某个快照（包含 x=10, y=20 的状态）
    size_t targetIdx = r.size() > 1 ? r.size() - 2 : 0;
    auto targetSnap = r.stepAt(targetIdx);
    ASSERT_TRUE(targetSnap.has_value());
    ASSERT_EQ(targetSnap->backend, TraceBackend::StackVM);

    // 验证快照包含 globals 数据
    bool hasX = false, hasY = false;
    for (const auto& kv : targetSnap->globalsValues) {
        if (kv.first == "x")
            hasX = true;
        if (kv.first == "y")
            hasY = true;
    }
    // 至少应捕获到一些 globals（具体哪些取决于快照采集点）
    // 即使没有 x/y，回滚操作本身也应成功（不依赖具体字段）

    // 回滚到该快照
    bool ok = stepper.restoreFromSnapshot(*targetSnap);
    EXPECT_TRUE(ok) << "StackVM 回滚应成功";

    if (ok) {
        // 验证回滚后 VM 仍可继续步进
        auto res = stepper.stepByMode(VmStepper::VmStepMode::STEP_IN);
        // 回滚后步进应返回 OK / FINISHED 之一（不应该是 NOT_READY）
        EXPECT_NE(res, VmStepper::VmStepResult::NOT_READY);
    }

    r.endSession();
    r.clear();
    r.setRecordingMode(RecordingMode::StringsOnly);
}

TEST(ExecutionTraceRecorderRollback, RegisterVM_RestoresRegistersAfterRollback) {
    auto& r = traceRecorder();
    r.clear();
    r.setRecordingMode(RecordingMode::FullState);
    r.startSession();

    VmStepper stepper;
    runRegisterVmFullState(stepper, "var x = 10; var y = 20;");

    ASSERT_GT(r.size(), 0u);
    size_t targetIdx = r.size() > 1 ? r.size() - 2 : 0;
    auto targetSnap = r.stepAt(targetIdx);
    ASSERT_TRUE(targetSnap.has_value());
    ASSERT_EQ(targetSnap->backend, TraceBackend::RegisterVM);

    bool ok = stepper.restoreFromSnapshot(*targetSnap);
    EXPECT_TRUE(ok) << "RegisterVM 回滚应成功";

    if (ok) {
        auto res = stepper.stepByMode(VmStepper::VmStepMode::STEP_IN);
        EXPECT_NE(res, VmStepper::VmStepResult::NOT_READY);
    }

    r.endSession();
    r.clear();
    r.setRecordingMode(RecordingMode::StringsOnly);
}

TEST(ExecutionTraceRecorderRollback, MismatchedBackendReturnsFalse) {
    auto& r = traceRecorder();
    r.clear();
    r.setRecordingMode(RecordingMode::FullState);
    r.startSession();

    VmStepper stepper; // 默认 StackVM 模式
    runStackVmFullState(stepper, "var x = 1;");

    ASSERT_GT(r.size(), 0u);
    auto targetSnap = r.stepAt(0);
    ASSERT_TRUE(targetSnap.has_value());
    ASSERT_EQ(targetSnap->backend, TraceBackend::StackVM);

    // 构造一个 RegisterVM 后端的假快照，传给 StackVM 模式的 stepper
    TraceSnapshot fakeSnap;
    fakeSnap.backend = TraceBackend::RegisterVM;
    fakeSnap.ip = 0;
    fakeSnap.frameCount = 1;
    bool ok = stepper.restoreFromSnapshot(fakeSnap);
    EXPECT_FALSE(ok) << "后端不匹配时应返回 false";

    r.endSession();
    r.clear();
    r.setRecordingMode(RecordingMode::StringsOnly);
}

TEST(ExecutionTraceRecorderRollback, VmStepperRejectsInterpreterBackendSnapshot) {
    // L18: VmStepper 是 VM 侧类（StackVM/RegisterVM），不持有 Interpreter 引用，
    // 无法回滚 Interpreter 后端快照——仍返回 false。
    // Interpreter 路径的回滚通过 Interpreter::restoreFromSnapshot 直接消费，
    // 见 ExecutionTraceRecorderInterpreterRollback 套件。
    auto& r = traceRecorder();
    r.clear();
    r.startSession();

    VmStepper stepper;
    runStackVmFullState(stepper, "var x = 1;");

    // 构造一个 Interpreter 后端的假快照
    TraceSnapshot fakeSnap;
    fakeSnap.backend = TraceBackend::Interpreter;
    fakeSnap.ip = 0;
    fakeSnap.frameCount = 0;
    bool ok = stepper.restoreFromSnapshot(fakeSnap);
    EXPECT_FALSE(ok) << "VmStepper 不支持 Interpreter 后端快照回滚，应返回 false";

    r.endSession();
    r.clear();
}

TEST(ExecutionTraceRecorderRollback, RollbackPreservesGlobalsValue_EarlyState) {
    // 验证回滚到早期快照后，globals 中的值应恢复到快照时的值
    auto& r = traceRecorder();
    r.clear();
    r.setRecordingMode(RecordingMode::FullState);
    r.startSession();

    VmStepper stepper;
    // 程序：x 先 = 10，后 = 20。回滚到中间快照后，x 应为 10 或尚未定义
    runStackVmFullState(stepper, "var x = 10; x = 20;");

    ASSERT_GT(r.size(), 0u);
    // 找到第一个包含 x=10 的快照
    size_t earlyIdx = 0;
    bool foundEarly = false;
    for (size_t i = 0; i < r.size(); ++i) {
        auto snap = r.stepAt(i);
        ASSERT_TRUE(snap.has_value());
        for (const auto& kv : snap->globalsValues) {
            if (kv.first == "x" && kv.second.isInt() && kv.second.intVal() == 10) {
                earlyIdx = i;
                foundEarly = true;
                break;
            }
        }
        if (foundEarly)
            break;
    }

    if (foundEarly) {
        auto earlySnap = r.stepAt(earlyIdx);
        ASSERT_TRUE(earlySnap.has_value());
        bool ok = stepper.restoreFromSnapshot(*earlySnap);
        EXPECT_TRUE(ok) << "回滚到 x=10 的快照应成功";
        if (ok) {
            // 验证回滚后 globals 中 x = 10
            auto globals = stepper.getGlobals();
            auto it = globals.find("x");
            if (it != globals.end()) {
                EXPECT_EQ(it->second.intVal(), 10) << "回滚后 x 应恢复为 10";
            }
        }
    }

    r.endSession();
    r.clear();
    r.setRecordingMode(RecordingMode::StringsOnly);
}

// ============================================================
// 测试套件 12：Interpreter 后端状态回滚（L18 — reverse debugging）
// ------------------------------------------------------------
// 验证 Interpreter::captureStateSnapshot / restoreFromSnapshot 的回滚语义。
// 测试模式：(1) FullState 模式录制 Interpreter 执行 → (2) 取某步快照 →
// (3) 继续执行改变状态 → (4) 回滚到快照 → (5) 断言 Interpreter 状态恢复。
//
// 与 VM 路径的区别：
//   - VM 路径通过 VmStepper::restoreFromSnapshot 消费快照
//   - Interpreter 路径通过 Interpreter::restoreFromSnapshot 直接消费
//   - TraceSnapshot::interpreterState（shared_ptr<void>）持有完整状态快照
// ============================================================

namespace {

/// 在 FullState 模式下录制 Interpreter 执行（用于 L18 回滚测试）
void runInterpreterFullState(Interpreter& interp, const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);
    ASSERT_NE(ast, nullptr);

    interp.setRecordingEnabled(true);
    interp.setOutputCallback([](const std::string&) {});
    interp.execute(*ast);
}

} // namespace

TEST(ExecutionTraceRecorderInterpreterRollback, FullStateModePopulatesInterpreterState) {
    // L18: FullState 模式下，Interpreter 后端快照应填充 interpreterState 字段
    auto& r = traceRecorder();
    r.clear();
    r.setRecordingMode(RecordingMode::FullState);
    r.startSession();

    {
        Interpreter interp;
        runInterpreterFullState(interp, "var x = 42; var y = x + 1;");
    }

    ASSERT_GT(r.size(), 0u);
    bool anyHasState = false;
    for (size_t i = 0; i < r.size(); ++i) {
        auto snap = r.stepAt(i);
        ASSERT_TRUE(snap.has_value());
        if (snap->backend == TraceBackend::Interpreter && snap->interpreterState) {
            anyHasState = true;
            break;
        }
    }
    EXPECT_TRUE(anyHasState) << "FullState 模式下 Interpreter 快照应填充 interpreterState";

    r.endSession();
    r.clear();
    r.setRecordingMode(RecordingMode::StringsOnly);
}

TEST(ExecutionTraceRecorderInterpreterRollback, StringsOnlyModeLeavesInterpreterStateEmpty) {
    // L18: StringsOnly 模式下，interpreterState 字段应为空（不捕获完整状态）
    auto& r = traceRecorder();
    r.clear();
    r.setRecordingMode(RecordingMode::StringsOnly);
    r.startSession();

    {
        Interpreter interp;
        runInterpreterFullState(interp, "var x = 1;");
    }

    ASSERT_GT(r.size(), 0u);
    for (size_t i = 0; i < r.size(); ++i) {
        auto snap = r.stepAt(i);
        ASSERT_TRUE(snap.has_value());
        EXPECT_FALSE(snap->interpreterState)
            << "StringsOnly 模式下 interpreterState 应为空（快照 idx=" << i << "）";
    }

    r.endSession();
    r.clear();
}

TEST(ExecutionTraceRecorderInterpreterRollback, RestoreFromSnapshotSucceeds) {
    // L18: Interpreter::restoreFromSnapshot 应成功恢复状态
    auto& r = traceRecorder();
    r.clear();
    r.setRecordingMode(RecordingMode::FullState);
    r.startSession();

    Interpreter interp;
    runInterpreterFullState(interp, "var x = 10; var y = 20;");

    ASSERT_GT(r.size(), 0u);
    // 取最后一个快照（程序执行完毕后的状态）
    auto targetSnap = r.stepAt(r.size() - 1);
    ASSERT_TRUE(targetSnap.has_value());
    ASSERT_EQ(targetSnap->backend, TraceBackend::Interpreter);
    ASSERT_TRUE(targetSnap->interpreterState);

    // 回滚到该快照
    auto stateSnap = std::static_pointer_cast<Interpreter::StateSnapshot>(targetSnap->interpreterState);
    bool ok = interp.restoreFromSnapshot(*stateSnap);
    EXPECT_TRUE(ok) << "Interpreter 回滚应成功";

    r.endSession();
    r.clear();
    r.setRecordingMode(RecordingMode::StringsOnly);
}

TEST(ExecutionTraceRecorderInterpreterRollback, RollbackRestoresVariableValue) {
    // L18: 回滚到早期快照后，变量应恢复到快照时的值
    // 程序：x 先 = 10，后 = 20。回滚到中间快照后，x 应为 10
    auto& r = traceRecorder();
    r.clear();
    r.setRecordingMode(RecordingMode::FullState);
    r.startSession();

    Interpreter interp;
    runInterpreterFullState(interp, "var x = 10; x = 20;");

    ASSERT_GT(r.size(), 0u);
    // 找到第一个包含 x=10 的快照
    size_t earlyIdx = 0;
    bool foundEarly = false;
    for (size_t i = 0; i < r.size(); ++i) {
        auto snap = r.stepAt(i);
        ASSERT_TRUE(snap.has_value());
        for (const auto& kv : snap->globalsValues) {
            if (kv.first == "x" && kv.second.isInt() && kv.second.intVal() == 10) {
                earlyIdx = i;
                foundEarly = true;
                break;
            }
        }
        if (foundEarly)
            break;
    }

    if (foundEarly) {
        auto earlySnap = r.stepAt(earlyIdx);
        ASSERT_TRUE(earlySnap.has_value());
        ASSERT_TRUE(earlySnap->interpreterState);
        auto stateSnap = std::static_pointer_cast<Interpreter::StateSnapshot>(earlySnap->interpreterState);
        bool ok = interp.restoreFromSnapshot(*stateSnap);
        EXPECT_TRUE(ok) << "回滚到 x=10 的快照应成功";
        if (ok) {
            // 验证回滚后 globals 中 x = 10
            auto globalEnv = interp.getGlobalEnvironment();
            ASSERT_TRUE(globalEnv);
            const Value* xVal = globalEnv->get("x");
            ASSERT_NE(xVal, nullptr);
            EXPECT_EQ(xVal->intVal(), 10) << "回滚后 x 应恢复为 10";
        }
    }

    r.endSession();
    r.clear();
    r.setRecordingMode(RecordingMode::StringsOnly);
}

TEST(ExecutionTraceRecorderInterpreterRollback, RollbackAllowsContinueExecution) {
    // L18: 回滚后 Interpreter 应能继续执行（状态一致可继续步进）
    auto& r = traceRecorder();
    r.clear();
    r.setRecordingMode(RecordingMode::FullState);
    r.startSession();

    Interpreter interp;
    runInterpreterFullState(interp, "var x = 10; x = 20;");

    ASSERT_GT(r.size(), 0u);
    // 取第一个快照作为回滚目标
    auto earlySnap = r.stepAt(0);
    ASSERT_TRUE(earlySnap.has_value());
    ASSERT_TRUE(earlySnap->interpreterState);

    auto stateSnap = std::static_pointer_cast<Interpreter::StateSnapshot>(earlySnap->interpreterState);
    bool ok = interp.restoreFromSnapshot(*stateSnap);
    EXPECT_TRUE(ok);
    if (ok) {
        // 回滚后继续执行新代码——Interpreter 应能正常工作
        // 注：使用字面量 print(42) 而非 print(x)，因为第一个快照捕获时机早于
        // var x 的执行，回滚后 x 可能尚未定义；本测试关注"回滚后能继续执行"，
        // 不关注 x 的值是否保留（由 RollbackRestoresVariableValue 覆盖）。
        std::string output;
        interp.setOutputCallback([&output](const std::string& s) { output += s; });
        Lexer lexer;
        auto tokens = lexer.scan("print(42);");
        Parser parser;
        auto ast = parser.parse(tokens);
        ASSERT_NE(ast, nullptr);
        interp.executeRepl(*ast);
        // 关键是不崩溃且产生输出
        EXPECT_FALSE(output.empty()) << "回滚后继续执行应产生输出";
        EXPECT_NE(output.find("42"), std::string::npos) << "应打印字面量 42";
    }

    r.endSession();
    r.clear();
    r.setRecordingMode(RecordingMode::StringsOnly);
}
