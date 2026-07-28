// ============================================================
// cli/dap_core.h - minilang-dap 调试适配器核心逻辑（可测试）
// ------------------------------------------------------------
// 实现 Debug Adapter Protocol (DAP)，让 VS Code 等编辑器可调试
// MiniLang 程序。通过 JSON-RPC 2.0 over stdio 与编辑器通信。
//
// 协议：JSON-RPC 2.0 over stdio（Content-Length 分帧）
// 行号：1-based（与 MiniLang 内部一致，与 LSP 0-based 不同）
//
// 设计要点：
//   - DapTransport: stdio 读写 Content-Length 分帧消息（复用 LSP 模式）
//   - DebugSession: 管理 Compiler + StackVM，实现行级步进
//   - DapRequestHandler: DAP 请求/通知分发与处理
//   - 核心逻辑与 main 分离，便于 gtest 单元测试（TestDapCli.cpp）
//
// 支持的 DAP 方法：
//   请求: initialize / launch / setBreakpoints / configurationDone /
//         continue / next / stepIn / stepOut / pause /
//         stackTrace / scopes / variables / evaluate /
//         threads / terminate / disconnect / source
//   发出: stopped / continued / output / terminated / exited
//
// 架构决策：
//   - 直接使用 StackVM（stepOnce + getCallStack + getFrameLocalsAt），
//     不依赖 VmStepper/IdeController（避免 QTimer 事件循环依赖）
//   - 行级步进自实现（指令级 stepOnce → 行级 next/stepIn/stepOut）
//   - QCoreApplication 创建但不 exec()（VM 不需要事件循环）
// ============================================================
#pragma once

#include "ast/ASTNode.h"
#include "compiler/Bytecode.h"
#include "compiler/Compiler.h"
#include "compiler/VM.h"
#include "interpreter/Value.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace minilang_dap {

// ============================================================
// DAP 协议常量
// ============================================================

/// DAP 线程 ID（MiniLang 单线程，固定为 1）
constexpr int kMainThreadId = 1;

/// 变量引用常量
constexpr int kVariablesReferenceNone = 0;
constexpr int kLocalsScopeReference = 1000;
constexpr int kGlobalsScopeReference = 2000;

/// continue/step 最大指令数（防止无限循环）
constexpr int kMaxStepInstructions = 1000000;

/// L21: continue 循环中每 N 步轮询一次 stdin 是否有 pause 请求。
/// 值过小会拖慢步进（每步都调用平台 IO），过大会延迟 pause 响应。
/// 256 步在典型 MiniLang 程序中约对应 10-50μs 步进开销，轮询开销可忽略。
constexpr int kPausePollInterval = 256;

// ============================================================
// DAP 数据类型
// ============================================================

/// DAP Source
struct DapSource {
    std::string name;
    std::string path;        // 文件路径（file:// URI 或绝对路径）
    int sourceReference = 0; // 0 = 源码在磁盘上
};

/// DAP StackFrame
struct DapStackFrame {
    int id = 0;
    std::string name; // 函数名
    DapSource source;
    int line = 1;   // 1-based
    int column = 1; // 1-based
    int endLine = 0;
    int endColumn = 0;
};

/// DAP Scope
struct DapScope {
    std::string name;           // "Locals" / "Globals"
    int variablesReference = 0; // 变量引用 ID
    int namedVariables = 0;     // 命名变量数（0 = 未知）
    int indexedVariables = 0;   // 索引变量数（0 = 未知）
    bool expensive = false;     // 是否需要昂贵计算
    DapSource source;
};

/// DAP Variable
struct DapVariable {
    std::string name;
    std::string value;          // 字符串化值
    std::string type;           // 类型名
    int variablesReference = 0; // 容器变量的子变量引用
    std::string evaluateName;   // 可求值表达式名
};

/// DAP Breakpoint
struct DapBreakpoint {
    int id = 0;
    int line = 0; // 1-based
    bool verified = true;
    std::string message;
    DapSource source;
};

/// DAP StoppedEvent reason
enum class DapStoppedReason { Breakpoint, Step, Entry, Exception, Pause, Terminate };

/// 步进结果
enum class StepResult {
    Ok,              // 步进成功，暂停在新位置
    Finished,        // 执行完毕
    Error,           // 运行时错误
    MaxStepsExceeded // 超出最大指令数
};

// ============================================================
// 变量容器（用于 variablesReference → 子变量映射）
// ============================================================

enum class VariableContainerType {
    Locals,   // 帧局部变量
    Globals,  // 全局变量
    Array,    // 数组元素
    Dict,     // 字典键值对
    Instance, // 类实例字段
    Tuple     // 元组元素
};

struct VariableContainer {
    VariableContainerType type = VariableContainerType::Locals;
    int frameIndex = 0;   // Locals: 帧索引
    Value containerValue; // Array/Dict/Instance/Tuple: 容器值
};

// ============================================================
// DebugSession — 调试会话管理
// ============================================================

class DebugSession {
public:
    DebugSession();
    ~DebugSession();

    // ---- 会话生命周期 ----

    /// 启动调试会话（编译源码 + 初始化 VM）
    /// source: MiniLang 源码
    /// filePath: 源码文件路径（用于 DAP source）
    /// returns: 错误消息（空表示成功）
    std::string launch(const std::string& source, const std::string& filePath);

    /// 是否已启动
    bool isLaunched() const { return launched_; }

    /// 是否执行完毕
    bool isFinished() const;

    /// 重置会话（清理 VM 状态和断点）
    void reset();

    // ---- 断点管理 ----

    /// 设置断点（全量替换，无条件）。保留旧 API：内部委托
    /// setSourceBreakpoints（清空条件/命中条件/命中计数）。
    void setBreakpoints(const std::unordered_set<int>& lines);

    /// DAP 二期：源断点（含条件表达式与命中条件）
    struct SourceBreakpoint {
        int line = 0;
        std::string condition;    ///< 条件表达式（空=无条件），MiniLang 表达式语法
        std::string hitCondition; ///< 命中条件："N"(==N) / "== N" / ">= N" / "> N" / "% N"(每 N 次)
    };

    /// DAP 二期：全量替换源断点（含条件/命中条件），重置命中计数。
    void setSourceBreakpoints(const std::vector<SourceBreakpoint>& bps);

    /// 获取断点集合
    const std::unordered_set<int>& getBreakpoints() const { return breakpoints_; }

    /// DAP 二期：查询断点命中计数（测试/诊断用）
    int getBreakpointHitCount(int line) const {
        auto it = bpHitCounts_.find(line);
        return it != bpHitCounts_.end() ? it->second : 0;
    }

    // ---- 步进控制 ----

    /// continue：执行直到断点/结束/错误
    StepResult doContinue();

    /// stepIn（行级）：执行到下一行（可进入函数）
    StepResult doStepIn();

    /// stepOver（行级）：执行到同一帧的下一行（跳过函数内部）
    StepResult doStepOver();

    /// stepOut（帧级）：执行直到从当前函数返回
    StepResult doStepOut();

    // ---- 状态查询 ----

    /// 获取调用栈
    std::vector<DapStackFrame> getCallStack() const;

    /// 获取帧数
    size_t getFrameCount() const;

    /// 当前行号（1-based）
    int getCurrentLine() const;

    /// 获取作用域列表（Locals + Globals）
    std::vector<DapScope> getScopes(int frameId) const;

    /// 获取变量列表（按 variablesReference）
    std::vector<DapVariable> getVariables(int variablesReference) const;

    /// 注册变量容器，返回 variablesReference
    int registerVariableContainer(const VariableContainer& container) const;

    /// 求值表达式（简单变量名查找）
    /// expr: 变量名
    /// frameId: 帧索引
    /// returns: (成功, 值字符串, 类型名, variablesReference)
    struct EvaluateResult {
        bool ok = false;
        std::string value;
        std::string type;
        int variablesReference = 0;
    };
    EvaluateResult evaluate(const std::string& expr, int frameId) const;

    /// DAP 二期：setVariable 请求——调试暂停时修改变量值。
    /// variablesReference 支持 Locals(1000+frameId) 与 Globals(2000) 作用域；
    /// 容器子元素（Array/Dict/Instance 子引用）暂不支持。
    /// valueText 解析规则：int / float / true / false / null / "带引号字符串"。
    struct SetVariableResult {
        bool ok = false;
        std::string value;          ///< 写入后的字符串化值
        std::string type;           ///< 类型名
        int variablesReference = 0; ///< 容器子引用（标量为 0）
        std::string message;        ///< 失败原因（ok=false 时）
    };
    SetVariableResult setVariable(int variablesReference, const std::string& name, const std::string& valueText);

    // ---- 输出回调 ----

    /// 设置输出回调（print 输出）
    void setOutputCallback(std::function<void(const std::string&)> callback);

    /// 获取累积的输出
    std::string getOutput() const { return outputBuffer_; }
    void clearOutput() { outputBuffer_.clear(); }

    // ---- 错误信息 ----

    bool hasError() const;
    std::string getLastError() const;
    int getLastErrorLine() const;

    // ---- 源码文件路径 ----

    const std::string& getFilePath() const { return filePath_; }

    /// 获取 VM（供测试用）
    const VM& getVM() const { return vm_; }

    // ---- L21: 同步暂停（pause 请求）支持 ----

    /// 设置 stdin 非阻塞轮询回调。dap.cpp 在非 Windows 平台用 `poll`/`select`，
    /// Windows 平台用 `_kbhit` 或 PeekNamedPipe 检测 stdin 是否有数据可读。
    /// 回调返回 true 表示 stdin 有数据待读取（调用方应中断 continue 循环）。
    /// @note 测试场景不设置回调，doContinue 不会主动检查 pause（保持旧行为）。
    void setStdinPollCallback(std::function<bool()> cb) { stdinPollCallback_ = std::move(cb); }

    /// 请求暂停（由 handlePause 调用）。doContinue 循环将检查此标志并提前退出。
    void requestPause() { pauseRequested_ = true; }
    bool isPauseRequested() const { return pauseRequested_; }
    void clearPauseRequest() { pauseRequested_ = false; }

private:
    Lexer lexer_;
    Parser parser_;
    Compiler compiler_;
    VM vm_;

    CompileResult compileResult_;
    std::shared_ptr<Block> ast_;
    std::vector<Token> tokens_;

    std::unordered_set<int> breakpoints_;
    // DAP 二期：条件断点/命中条件/命中计数（行号 → 元数据）。
    // bpHitCounts_ 为 mutable：hitBreakpoint() 保持 const 签名（命中计数是
    // 诊断状态而非会话逻辑状态，与 variableContainers_ 的 mutable 理由一致）。
    std::unordered_map<int, std::string> bpConditions_;
    std::unordered_map<int, std::string> bpHitConditions_;
    mutable std::unordered_map<int, int> bpHitCounts_;
    std::string filePath_;
    std::string source_;

    bool launched_ = false;
    bool vmInitialized_ = false;

    // L21: 同步暂停支持
    std::atomic<bool> pauseRequested_{false};      // pause 请求标志（handlePause 设置）
    std::function<bool()> stdinPollCallback_;      // stdin 非阻塞轮询回调（dap.cpp 注入）

    std::function<void(const std::string&)> outputCallback_;
    std::string outputBuffer_;

    /// 变量容器映射（variablesReference → container）
    mutable std::unordered_map<int, VariableContainer> variableContainers_;
    mutable int nextVariableReference_ = 3000;

    /// 执行单条指令
    StepResult stepOnceWithCheck();

    /// 行级步进核心逻辑
    StepResult stepUntilLineChange(bool allowFrameDepthIncrease);
    StepResult stepUntilFrameDepthDecrease(size_t initialFrameCount);

    /// 检查当前行是否命中断点（DAP 二期：含条件求值与命中条件判定）
    bool hitBreakpoint() const;

    /// DAP 二期：在临时 Interpreter 沙箱中求值条件表达式。
    /// 注入当前栈顶帧 locals + 全局变量后执行 "var __dap_cond = (cond);"，
    /// 读取结果 truthiness（与 IdeController 的 VM 条件断点求值同模式）。
    /// 解析/求值失败视为条件不满足（不暂停，对齐 DebugEvaluator 语义）。
    bool evalBreakpointCondition(const std::string& cond) const;

    /// DAP 二期：命中条件判定。支持 "N"/"== N"/">= N"/"> N"/"% N"；
    /// 解析失败时返回 true（视为无命中条件，不拦截）。
    static bool hitConditionSatisfied(const std::string& expr, int count);

    /// DAP 二期：setVariable 新值文本解析（int/float/bool/null/"string"）
    static bool parseValueText(const std::string& text, Value& out);

    /// 将 Value 转换为 DAP Variable
    DapVariable toVariable(const std::string& name, const Value& val) const;

    /// 获取变量的子变量引用（如果是容器）
    int getVariablesReference(const Value& val) const;

    /// Value 转字符串
    static std::string valueToString(const Value& val);
    static std::string valueTypeName(const Value& val);
};

// ============================================================
// JSON-RPC 传输层（复用 LSP 模式）
// ============================================================

class DapTransport {
public:
    DapTransport();

    /// 从 stdin 读取一条 JSON-RPC 消息（阻塞）
    std::optional<QJsonObject> read();

    /// 向 stdout 写入一条 JSON-RPC 消息
    void write(const QJsonObject& message);

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

    std::optional<QJsonObject> readFromBuffer();
};

// ============================================================
// DAP 请求处理器
// ============================================================

class DapRequestHandler {
public:
    DapRequestHandler();

    /// 处理一条 JSON-RPC 消息
    /// 返回需要发送的响应/事件列表
    std::vector<QJsonObject> handleMessage(const QJsonObject& message);

    /// 是否应该退出（收到 disconnect/terminate）
    bool shouldExit() const { return shouldExit_; }

    // ---- 测试辅助 ----

    /// 获取内部 DebugSession（供测试直接操作）
    DebugSession& session() { return session_; }
    const DebugSession& session() const { return session_; }

private:
    DebugSession session_;
    bool shouldExit_ = false;
    bool initialized_ = false;
    bool configurationDone_ = false;
    int nextSeq_ = 1; // 事件/响应序列号

    // ---- 响应构建 ----

    QJsonObject makeResponse(int requestSeq, const std::string& command, bool success, const QJsonValue& body,
                             const std::string& message = "");
    QJsonObject makeEvent(const std::string& event, const QJsonValue& body);

    // ---- 请求处理 ----

    QJsonValue handleInitialize(const QJsonObject& args);
    QJsonValue handleLaunch(const QJsonObject& args);
    QJsonValue handleSetBreakpoints(const QJsonObject& args);
    QJsonValue handleConfigurationDone(const QJsonObject& args);
    QJsonValue handleContinue(const QJsonObject& args);
    QJsonValue handleNext(const QJsonObject& args);
    QJsonValue handleStepIn(const QJsonObject& args);
    QJsonValue handleStepOut(const QJsonObject& args);
    QJsonValue handleStackTrace(const QJsonObject& args);
    QJsonValue handleScopes(const QJsonObject& args);
    QJsonValue handleVariables(const QJsonObject& args);
    QJsonValue handleEvaluate(const QJsonObject& args);
    QJsonValue handleSetVariable(const QJsonObject& args); // DAP 二期
    QJsonValue handleThreads(const QJsonObject& args);
    QJsonValue handleTerminate(const QJsonObject& args);
    QJsonValue handleDisconnect(const QJsonObject& args);
    QJsonValue handleSource(const QJsonObject& args);
    QJsonValue handlePause(const QJsonObject& args);

    /// 执行步进并发送 stopped 事件
    std::vector<QJsonObject> doStepAndSendEvents(StepResult (DebugSession::*stepFn)());

    /// 发送 stopped 事件
    QJsonObject makeStoppedEvent(DapStoppedReason reason, int line, const std::string& exceptionText = "");

    /// 发送 terminated 事件
    QJsonObject makeTerminatedEvent();

    /// 发送 output 事件
    QJsonObject makeOutputEvent(const std::string& output, const std::string& category = "stdout");

    /// 从消息中提取 textDocument.uri 和 position
    bool extractSourceAndLine(const QJsonObject& args, std::string& path, int& line) const;
};

// ============================================================
// JSON 构建工具
// ============================================================

namespace json {

QJsonObject source(const std::string& name, const std::string& path, int sourceReference = 0);
QJsonObject stackFrame(const DapStackFrame& frame);
QJsonObject scope(const DapScope& s);
QJsonObject variable(const DapVariable& v);
QJsonObject breakpoint(const DapBreakpoint& bp);

} // namespace json

// ============================================================
// CLI 辅助函数
// ============================================================

void printHelp();
void printVersion();
std::string versionString();
std::string serverName();

/// 运行 DAP 服务器主循环（供 dap_entry.cpp 统一入口调用）
/// 返回退出码：0=正常退出
int runDapServer();

} // namespace minilang_dap
