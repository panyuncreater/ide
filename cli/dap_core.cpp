// ============================================================
// cli/dap_core.cpp - minilang-dap 调试适配器核心逻辑实现
// ------------------------------------------------------------
// 实现 Debug Adapter Protocol (DAP) 的核心逻辑：
//   - DapTransport: JSON-RPC 2.0 over stdio（Content-Length 分帧）
//   - DebugSession: 管理 Lexer/Parser/Compiler/StackVM，行级步进
//   - DapRequestHandler: DAP 请求/通知分发与处理
// 复用 LSP 的 JsonRpcTransport 传输模式，核心逻辑与 main 分离便于测试。
// ============================================================

#include "cli/dap_core.h"

// DAP 二期：条件断点求值使用临时 Interpreter 沙箱（与 IdeController 的
// VM 条件断点求值同模式），仅在断点行首次到达时执行，开销可接受。
// 命中条件/改值文本解析复用 debug/DebugTypes.h 共享实现（三路径一致）。
#include "debug/DebugTypes.h"
#include "interpreter/Interpreter.h"

#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QTextStream>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>

// W4 模式下 /wd4996 被释放，本文件使用 std::fopen（C 标准接口），
// 局部禁用 C4996 弃用警告，避免阻塞 W4/WERROR 构建。
#ifdef _MSC_VER
#pragma warning(disable : 4996)
#endif

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace minilang_dap {

namespace {

/// 从文件路径提取文件名（最后一个路径分隔符后的部分）
std::string extractFilename(const std::string& path) {
    if (path.empty())
        return "source";
    size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos)
        return path;
    return path.substr(pos + 1);
}

/// 读取文件全部内容（二进制模式）。失败返回空字符串。
std::string readFileContent(const std::string& path) {
    if (path.empty())
        return "";
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
        return "";
    std::string content;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size > 0) {
        content.resize(static_cast<size_t>(size));
        // glibc 对 fread 标记 warn_unused_result：读取不足时截断到实际字节数
        size_t readBytes = std::fread(&content[0], 1, static_cast<size_t>(size), f);
        if (readBytes < static_cast<size_t>(size))
            content.resize(readBytes);
    }
    std::fclose(f);
    return content;
}

/// 从 DiagnosticBag 中提取第一条错误消息（用于 launch 失败时返回）
std::string firstErrorMessage(const DiagnosticBag& bag) {
    for (const auto& d : bag.all()) {
        if (d.isError()) {
            return d.format();
        }
    }
    return "unknown error";
}

/// DAP stopped reason 枚举转字符串
const char* stoppedReasonString(DapStoppedReason reason) {
    switch (reason) {
    case DapStoppedReason::Breakpoint:
        return "breakpoint";
    case DapStoppedReason::Step:
        return "step";
    case DapStoppedReason::Entry:
        return "entry";
    case DapStoppedReason::Exception:
        return "exception";
    case DapStoppedReason::Pause:
        return "pause";
    case DapStoppedReason::Terminate:
        return "terminate";
    }
    return "step";
}

} // anonymous namespace

// ============================================================
// JSON 构建工具（namespace json）
// ============================================================

namespace json {

QJsonObject source(const std::string& name, const std::string& path, int sourceReference) {
    QJsonObject obj;
    obj["name"] = QString::fromStdString(name);
    obj["path"] = QString::fromStdString(path);
    obj["sourceReference"] = sourceReference;
    return obj;
}

QJsonObject stackFrame(const DapStackFrame& frame) {
    QJsonObject obj;
    obj["id"] = frame.id;
    obj["name"] = QString::fromStdString(frame.name);
    obj["source"] = source(frame.source.name, frame.source.path, frame.source.sourceReference);
    obj["line"] = frame.line;
    obj["column"] = frame.column;
    obj["endLine"] = frame.endLine;
    obj["endColumn"] = frame.endColumn;
    return obj;
}

QJsonObject scope(const DapScope& s) {
    QJsonObject obj;
    obj["name"] = QString::fromStdString(s.name);
    obj["variablesReference"] = s.variablesReference;
    obj["namedVariables"] = s.namedVariables;
    obj["indexedVariables"] = s.indexedVariables;
    obj["expensive"] = s.expensive;
    if (!s.source.path.empty()) {
        obj["source"] = source(s.source.name, s.source.path, s.source.sourceReference);
    }
    return obj;
}

QJsonObject variable(const DapVariable& v) {
    QJsonObject obj;
    obj["name"] = QString::fromStdString(v.name);
    obj["value"] = QString::fromStdString(v.value);
    if (!v.type.empty()) {
        obj["type"] = QString::fromStdString(v.type);
    }
    obj["variablesReference"] = v.variablesReference;
    if (!v.evaluateName.empty()) {
        obj["evaluateName"] = QString::fromStdString(v.evaluateName);
    }
    return obj;
}

QJsonObject breakpoint(const DapBreakpoint& bp) {
    QJsonObject obj;
    obj["id"] = bp.id;
    obj["verified"] = bp.verified;
    obj["line"] = bp.line;
    if (!bp.message.empty()) {
        obj["message"] = QString::fromStdString(bp.message);
    }
    if (!bp.source.path.empty()) {
        obj["source"] = source(bp.source.name, bp.source.path, bp.source.sourceReference);
    }
    return obj;
}

} // namespace json

// ============================================================
// DebugSession 实现
// ============================================================

DebugSession::DebugSession() = default;

DebugSession::~DebugSession() = default;

std::string DebugSession::launch(const std::string& source, const std::string& filePath) {
    // 保存源码与文件路径
    source_ = source;
    filePath_ = filePath;

    // 重置 VM 状态（清理上一轮残留）
    vm_.resetState();
    launched_ = false;
    vmInitialized_ = false;
    outputBuffer_.clear();
    variableContainers_.clear();
    nextVariableReference_ = 3000;
    breakpoints_.clear();

    // 词法分析
    tokens_ = lexer_.scan(source);
    if (lexer_.getDiagnostics().hasErrors()) {
        return firstErrorMessage(lexer_.getDiagnostics());
    }

    // 语法分析（Parser 可能抛 ParseError，用 try-catch 包裹）
    std::unique_ptr<Block> parsed;
    try {
        parsed = parser_.parse(tokens_);
    } catch (const std::exception& e) {
        return std::string("Parse error: ") + e.what();
    }
    if (!parsed || parser_.hasErrors()) {
        return firstErrorMessage(parser_.getDiagnostics());
    }
    ast_ = std::shared_ptr<Block>(std::move(parsed));

    // 编译（Compiler 可能抛 runtime_error，用 try-catch 包裹）
    try {
        compileResult_ = compiler_.compile(*ast_);
    } catch (const std::exception& e) {
        return std::string("Compile error: ") + e.what();
    }
    if (compiler_.getDiagnostics().hasErrors()) {
        return firstErrorMessage(compiler_.getDiagnostics());
    }

    // 设置 VM 输出回调：累积到 outputBuffer_ 并转发给外部回调
    vm_.setOutputCallback([this](const std::string& s) {
        outputBuffer_ += s;
        if (outputCallback_) {
            outputCallback_(s);
        }
    });

    // 初始化 VM 执行环境（单步模式前置，不运行）
    vm_.initExecution(compileResult_);

    launched_ = true;
    vmInitialized_ = true;
    return "";
}

void DebugSession::setBreakpoints(const std::unordered_set<int>& lines) {
    // 旧 API 兼容：无条件全量替换，委托结构化 API 保证元数据一致清理
    std::vector<SourceBreakpoint> bps;
    bps.reserve(lines.size());
    for (int line : lines) {
        SourceBreakpoint bp;
        bp.line = line;
        bps.push_back(bp);
    }
    setSourceBreakpoints(bps);
}

void DebugSession::setSourceBreakpoints(const std::vector<SourceBreakpoint>& bps) {
    breakpoints_.clear();
    bpConditions_.clear();
    bpHitConditions_.clear();
    // 命中计数重置：与 DebugController::setBreakpointCondition 对齐——
    // 断点集合/条件变更后旧计数无意义（hitCondition 基于计数判定）。
    bpHitCounts_.clear();
    for (const auto& bp : bps) {
        if (bp.line <= 0)
            continue;
        breakpoints_.insert(bp.line);
        if (!bp.condition.empty())
            bpConditions_[bp.line] = bp.condition;
        if (!bp.hitCondition.empty())
            bpHitConditions_[bp.line] = bp.hitCondition;
    }
}

StepResult DebugSession::doContinue() {
    // DAP 二期 fix：逐行跟踪（prevLine）替代固定起始行（startLine）门控。
    // 原实现仅比较起始行：离开起始行后，断点行的每条指令都会调用
    // hitBreakpoint()。无状态时碰巧无害（首次即 return），但引入命中计数/
    // 条件求值后会重复递增计数、重复求值条件。prevLine 跟踪保证
    // 每次"到达新行"仅检查一次（与 DebugController crossedLine 语义对齐）。
    int prevLine = vm_.getCurrentLine();
    int count = 0;
    while (count < kMaxStepInstructions) {
        // L21: 每 kPausePollInterval 步轮询一次 stdin 是否有 pause 请求。
        // stdinPollCallback_ 由 dap.cpp 注入（非 Windows 用 poll/select，Windows 用
        // PeekNamedPipe）。测试场景不设置回调，此分支被跳过（保持旧行为）。
        // pauseRequested_ 也可能由 handlePause 在 continue 启动后通过另一消息设置
        // （仅多线程场景，当前同步模型下 pause 必然在 continue 之前到达）。
        if ((count & (kPausePollInterval - 1)) == 0 && count > 0) {
            if (pauseRequested_.load(std::memory_order_relaxed)) {
                return StepResult::Ok; // 暂停，回到断点位置
            }
            if (stdinPollCallback_ && stdinPollCallback_()) {
                // stdin 有数据待读，可能是 pause 请求。
                // 不在此处解析消息（会破坏 DapTransport 的分帧状态），仅设置标志
                // 让 doContinue 退出，控制权交回 main 循环读取并处理 pause。
                pauseRequested_ = true;
                return StepResult::Ok;
            }
        }
        StepResult r = stepOnceWithCheck();
        if (r != StepResult::Ok) {
            return r;
        }
        // 同一行的剩余指令不检查断点（逐行跟踪，见函数头注释）
        int curLine = vm_.getCurrentLine();
        if (curLine == prevLine) {
            ++count;
            continue;
        }
        prevLine = curLine;
        if (hitBreakpoint()) {
            return StepResult::Ok;
        }
        ++count;
    }
    return StepResult::MaxStepsExceeded;
}

StepResult DebugSession::doStepIn() {
    // 记录当前行号（用于日志/调试，实际步进逻辑由 stepUntilLineChange 处理）
    (void)vm_.getCurrentLine();
    return stepUntilLineChange(true);
}

StepResult DebugSession::doStepOver() {
    // 记录当前帧深度（实际步进逻辑由 stepUntilLineChange 处理）
    (void)vm_.getFrameCount();
    return stepUntilLineChange(false);
}

StepResult DebugSession::doStepOut() {
    size_t initialFrameCount = vm_.getFrameCount();
    return stepUntilFrameDepthDecrease(initialFrameCount);
}

StepResult DebugSession::stepOnceWithCheck() {
    VMResult result = vm_.stepOnce();
    if (result == VMResult::VM_RUNTIME_ERROR || result == VMResult::VM_STACK_OVERFLOW) {
        return StepResult::Error;
    }
    if (vm_.isFinished()) {
        return StepResult::Finished;
    }
    return StepResult::Ok;
}

StepResult DebugSession::stepUntilLineChange(bool allowFrameDepthIncrease) {
    int startLine = vm_.getCurrentLine();
    size_t startFrameCount = vm_.getFrameCount();

    int count = 0;
    while (count < kMaxStepInstructions) {
        StepResult r = stepOnceWithCheck();
        if (r != StepResult::Ok) {
            return r;
        }

        int newLine = vm_.getCurrentLine();
        size_t newFrameCount = vm_.getFrameCount();

        // 从函数返回（帧深度减少）→ 暂停
        if (newFrameCount < startFrameCount) {
            return StepResult::Ok;
        }
        // 不允许帧深度增加时，在函数内部继续执行（不暂停）
        if (!allowFrameDepthIncrease && newFrameCount > startFrameCount) {
            ++count;
            continue;
        }
        // 行号变化且帧深度未增加 → 暂停
        if (newLine != startLine && newFrameCount <= startFrameCount) {
            return StepResult::Ok;
        }
        ++count;
    }
    return StepResult::MaxStepsExceeded;
}

StepResult DebugSession::stepUntilFrameDepthDecrease(size_t initialFrameCount) {
    int count = 0;
    while (count < kMaxStepInstructions) {
        StepResult r = stepOnceWithCheck();
        if (r != StepResult::Ok) {
            return r;
        }
        if (vm_.getFrameCount() < initialFrameCount) {
            return StepResult::Ok;
        }
        ++count;
    }
    return StepResult::MaxStepsExceeded;
}

bool DebugSession::hitBreakpoint() const {
    int line = vm_.getCurrentLine();
    if (breakpoints_.count(line) == 0)
        return false;
    // DAP 二期：条件断点——条件不满足则不命中（不递增 hitCount，
    // 对齐 DebugController::shouldPauseAtBreakpoint 的条件分支语义）
    auto condIt = bpConditions_.find(line);
    if (condIt != bpConditions_.end() && !condIt->second.empty()) {
        if (!evalBreakpointCondition(condIt->second))
            return false;
    }
    // 条件满足（或无条件）才计一次命中，再用命中条件决定是否暂停
    int count = ++bpHitCounts_[line];
    auto hcIt = bpHitConditions_.find(line);
    if (hcIt != bpHitConditions_.end() && !hcIt->second.empty()) {
        return hitConditionSatisfied(hcIt->second, count);
    }
    return true;
}

bool DebugSession::evalBreakpointCondition(const std::string& cond) const {
    try {
        // 与 IdeController::setConditionEvaluator 同模式：
        // (1) 自动补分号（用户条件表达式通常不带分号，Parser 要求分号）
        // (2) 临时 Interpreter + setGlobalEnvironment 注入变量
        // (3) evaluateCondition 沙箱求值单语句（不用 execute：
        //     execute 会 resetState 清空预注入的环境）
        std::string condExpr = cond;
        if (!condExpr.empty() && condExpr.back() != ';') {
            condExpr += ';';
        }
        Lexer lx;
        auto tokens = lx.scan(condExpr);
        if (lx.getDiagnostics().hasErrors())
            return false;
        Parser ps;
        auto parsed = ps.parse(tokens);
        if (!parsed || parsed->statements.empty() || ps.getDiagnostics().hasErrors())
            return false;
        Interpreter tempInterp;
        auto env = std::make_shared<Environment>();
        // 注入顺序：先全局后栈顶帧 locals（locals 遮蔽同名全局，与 VM 作用域一致）
        for (const auto& kv : vm_.getGlobals())
            env->define(kv.first, kv.second);
        size_t fc = vm_.getFrameCount();
        if (fc > 0) {
            for (const auto& kv : vm_.getFrameLocalsAt(fc - 1))
                env->define(kv.first, kv.second);
        }
        tempInterp.setGlobalEnvironment(env);
        Value result = tempInterp.evaluateCondition(parsed->statements[0].get());
        return result.isTruthy();
    } catch (...) {
        // 求值异常（未定义变量/类型错误等）视为条件不满足
        return false;
    }
}

bool DebugSession::hitConditionSatisfied(const std::string& expr, int count) {
    // 转发 debug/DebugTypes.h 共享实现（DebugController/VmStepper/DAP 三路径一致）
    return evalHitCondition(expr, count);
}

bool DebugSession::parseValueText(const std::string& text, Value& out) {
    // 转发 debug/DebugTypes.h 共享实现
    return parseDebugValueText(text, out);
}

DebugSession::SetVariableResult DebugSession::setVariable(int variablesReference, const std::string& name,
                                                          const std::string& valueText) {
    SetVariableResult r;
    Value newVal;
    if (!parseValueText(valueText, newVal)) {
        r.message = "无法解析新值（支持 int / float / true / false / null / \"字符串\"）";
        return r;
    }
    bool written = false;
    if (variablesReference == kGlobalsScopeReference) {
        written = vm_.setGlobalValue(name, newVal);
    } else if (variablesReference >= kLocalsScopeReference && variablesReference < kGlobalsScopeReference) {
        // DAP frameId (0=栈顶) → VM frameIndex (0=栈底)，与 evaluate 同换算
        int frameId = variablesReference - kLocalsScopeReference;
        size_t frameCount = vm_.getFrameCount();
        if (frameCount > 0 && frameId >= 0 && static_cast<size_t>(frameId) < frameCount) {
            size_t vmFrameIndex = frameCount - 1 - static_cast<size_t>(frameId);
            written = vm_.setFrameLocalAt(vmFrameIndex, name, newVal);
        }
        // main 帧的"局部"实为全局槽位：帧内未命中时回退全局写入
        if (!written)
            written = vm_.setGlobalValue(name, newVal);
    } else {
        r.message = "容器子元素编辑暂不支持（仅支持 Locals/Globals 作用域变量）";
        return r;
    }
    if (!written) {
        r.message = "变量不存在: " + name;
        return r;
    }
    r.ok = true;
    r.value = valueToString(newVal);
    r.type = valueTypeName(newVal);
    r.variablesReference = getVariablesReference(newVal);
    return r;
}

std::vector<DapStackFrame> DebugSession::getCallStack() const {
    std::vector<DapStackFrame> frames;
    if (!launched_) {
        return frames;
    }

    auto vmStack = vm_.getCallStack();
    std::string filename = extractFilename(filePath_);

    // DAP fixup: DAP 规范要求 stackTrace 响应的 frames[0] = 栈顶（当前帧），
    // 但 VM 内部 vmStack[0] = 栈底（main 帧）。反转遍历顺序使 DAP frameId=0 指向当前帧。
    frames.reserve(vmStack.size());
    for (size_t dapId = 0; dapId < vmStack.size(); ++dapId) {
        size_t vmIdx = vmStack.size() - 1 - dapId; // 反转：DAP id=0 → VM 栈顶
        DapStackFrame frame;
        frame.id = static_cast<int>(dapId);
        frame.name = vmStack[vmIdx].functionName.empty() ? "main" : vmStack[vmIdx].functionName;
        frame.source.name = filename;
        frame.source.path = filePath_;
        frame.source.sourceReference = 0;
        frame.line = vmStack[vmIdx].line > 0 ? vmStack[vmIdx].line : 1;
        frame.column = 1;
        frames.push_back(std::move(frame));
    }
    return frames;
}

size_t DebugSession::getFrameCount() const {
    return vm_.getFrameCount();
}

int DebugSession::getCurrentLine() const {
    return vm_.getCurrentLine();
}

bool DebugSession::isFinished() const {
    if (!launched_) {
        return true;
    }
    return vm_.isFinished();
}

void DebugSession::reset() {
    vm_.resetState();
    breakpoints_.clear();
    launched_ = false;
    vmInitialized_ = false;
    outputBuffer_.clear();
    variableContainers_.clear();
    nextVariableReference_ = 3000;
    source_.clear();
    filePath_.clear();
}

std::vector<DapScope> DebugSession::getScopes(int frameId) const {
    std::vector<DapScope> scopes;

    // Locals scope
    DapScope locals;
    locals.name = "Locals";
    locals.variablesReference = kLocalsScopeReference + frameId;
    locals.namedVariables = 0;
    locals.indexedVariables = 0;
    locals.expensive = false;
    scopes.push_back(locals);

    // Globals scope
    DapScope globals;
    globals.name = "Globals";
    globals.variablesReference = kGlobalsScopeReference;
    globals.namedVariables = 0;
    globals.indexedVariables = 0;
    globals.expensive = false;
    scopes.push_back(globals);

    return scopes;
}

std::vector<DapVariable> DebugSession::getVariables(int variablesReference) const {
    std::vector<DapVariable> vars;

    // Locals scope: [1000, 2000)
    if (variablesReference >= kLocalsScopeReference && variablesReference < kGlobalsScopeReference) {
        int frameId = variablesReference - kLocalsScopeReference;
        // DAP fixup: DAP frameId (0=栈顶当前帧) → VM frameIndex (0=栈底 main 帧)
        size_t frameCount = vm_.getFrameCount();
        if (frameCount == 0 || frameId < 0 || static_cast<size_t>(frameId) >= frameCount) {
            return vars; // 越界
        }
        size_t vmFrameIndex = frameCount - 1 - static_cast<size_t>(frameId);
        auto locals = vm_.getFrameLocalsAt(vmFrameIndex);
        vars.reserve(locals.size());
        for (const auto& kv : locals) {
            vars.push_back(toVariable(kv.first, kv.second));
        }
        return vars;
    }

    // Globals scope: [2000, 3000)
    if (variablesReference >= kGlobalsScopeReference && variablesReference < 3000) {
        auto globals = vm_.getGlobals();
        vars.reserve(globals.size());
        for (const auto& kv : globals) {
            vars.push_back(toVariable(kv.first, kv.second));
        }
        return vars;
    }

    // 动态注册的容器变量: [3000, ∞)
    auto it = variableContainers_.find(variablesReference);
    if (it == variableContainers_.end()) {
        return vars;
    }

    const VariableContainer& container = it->second;
    const Value& cv = container.containerValue;

    switch (container.type) {
    case VariableContainerType::Array: {
        if (cv.isArray()) {
            const auto& arr = std::as_const(cv).arrayVal();
            vars.reserve(arr.size());
            for (size_t i = 0; i < arr.size(); ++i) {
                vars.push_back(toVariable("[" + std::to_string(i) + "]", arr[i]));
            }
        }
        break;
    }
    case VariableContainerType::Dict: {
        if (cv.isDict()) {
            const auto& dict = std::as_const(cv).dictVal();
            vars.reserve(dict.size());
            for (const auto& kv : dict) {
                vars.push_back(toVariable(Value::dictKeyToString(kv.first), kv.second));
            }
        }
        break;
    }
    case VariableContainerType::Instance: {
        if (cv.isInstance()) {
            const auto& fields = std::as_const(cv).fields();
            vars.reserve(fields.size());
            for (const auto& kv : fields) {
                vars.push_back(toVariable(kv.first, kv.second));
            }
        }
        break;
    }
    case VariableContainerType::Tuple: {
        if (cv.isTuple()) {
            const auto& tup = std::as_const(cv).tupleVal();
            vars.reserve(tup.size());
            for (size_t i = 0; i < tup.size(); ++i) {
                vars.push_back(toVariable("." + std::to_string(i), tup[i]));
            }
        }
        break;
    }
    case VariableContainerType::Locals:
    case VariableContainerType::Globals:
        // Locals/Globals 不通过容器展开（由 scope reference 直接处理）
        break;
    }

    return vars;
}

int DebugSession::registerVariableContainer(const VariableContainer& container) const {
    int ref = nextVariableReference_++;
    variableContainers_[ref] = container;
    return ref;
}

DebugSession::EvaluateResult DebugSession::evaluate(const std::string& expr, int frameId) const {
    EvaluateResult result;

    // DAP fixup: DAP frameId (0=栈顶当前帧) → VM frameIndex (0=栈底 main 帧)
    size_t frameCount = vm_.getFrameCount();
    size_t vmFrameIndex = (frameCount == 0 || frameId < 0 || static_cast<size_t>(frameId) >= frameCount)
                              ? 0
                              : frameCount - 1 - static_cast<size_t>(frameId);

    // 先在局部变量中查找
    auto locals = vm_.getFrameLocalsAt(vmFrameIndex);
    auto localIt = locals.find(expr);
    if (localIt != locals.end()) {
        result.ok = true;
        result.value = valueToString(localIt->second);
        result.type = valueTypeName(localIt->second);
        result.variablesReference = getVariablesReference(localIt->second);
        return result;
    }

    // 再在全局变量中查找
    auto globals = vm_.getGlobals();
    auto globalIt = globals.find(expr);
    if (globalIt != globals.end()) {
        result.ok = true;
        result.value = valueToString(globalIt->second);
        result.type = valueTypeName(globalIt->second);
        result.variablesReference = getVariablesReference(globalIt->second);
        return result;
    }

    return result;
}

void DebugSession::setOutputCallback(std::function<void(const std::string&)> callback) {
    outputCallback_ = std::move(callback);
}

bool DebugSession::hasError() const {
    return vm_.hasError();
}

std::string DebugSession::getLastError() const {
    return vm_.getLastError();
}

int DebugSession::getLastErrorLine() const {
    return vm_.getLastErrorLine();
}

DapVariable DebugSession::toVariable(const std::string& name, const Value& val) const {
    DapVariable var;
    var.name = name;
    var.value = valueToString(val);
    var.type = valueTypeName(val);
    var.variablesReference = getVariablesReference(val);
    var.evaluateName = name;
    return var;
}

int DebugSession::getVariablesReference(const Value& val) const {
    // 仅容器类型需要子变量引用
    if (!val.isArray() && !val.isDict() && !val.isInstance() && !val.isTuple()) {
        return kVariablesReferenceNone;
    }

    VariableContainer container;
    if (val.isArray()) {
        container.type = VariableContainerType::Array;
    } else if (val.isDict()) {
        container.type = VariableContainerType::Dict;
    } else if (val.isInstance()) {
        container.type = VariableContainerType::Instance;
    } else {
        container.type = VariableContainerType::Tuple;
    }
    container.containerValue = val; // 拷贝 Value（NaN-box 8 字节，堆类型原子引用计数递增）

    return registerVariableContainer(container);
}

std::string DebugSession::valueToString(const Value& val) {
    std::string s = val.toString();
    // 截断过长的字符串表示，避免 DAP 消息过大
    constexpr size_t kMaxValueLen = 256;
    if (s.size() > kMaxValueLen) {
        s.resize(kMaxValueLen);
        s += "...";
    }
    return s;
}

std::string DebugSession::valueTypeName(const Value& val) {
    return val.typeName();
}

// ============================================================
// DapTransport 实现（复用 LSP JsonRpcTransport 模式）
// ============================================================

DapTransport::DapTransport() = default;

std::optional<QJsonObject> DapTransport::read() {
    if (useBufferIO_) {
        return readFromBuffer();
    }

    // 从 stdin 读取 Content-Length 分帧
    std::string headers;
    while (true) {
        int c = std::fgetc(stdin);
        if (c == EOF) {
            return std::nullopt;
        }
        headers.push_back(static_cast<char>(c));
        // 检测空行（\r\n\r\n）
        if (headers.size() >= 4 && headers[headers.size() - 4] == '\r' && headers[headers.size() - 3] == '\n' &&
            headers[headers.size() - 2] == '\r' && headers[headers.size() - 1] == '\n') {
            break;
        }
    }

    // 解析 Content-Length
    size_t contentLength = 0;
    {
        std::istringstream ss(headers);
        std::string headerLine;
        while (std::getline(ss, headerLine)) {
            if (!headerLine.empty() && headerLine.back() == '\r') {
                headerLine.pop_back();
            }
            const std::string key = "Content-Length:";
            if (headerLine.size() >= key.size() &&
                std::equal(key.begin(), key.end(), headerLine.begin(),
                           [](char a, char b) { return std::tolower(a) == std::tolower(b); })) {
                std::string val = headerLine.substr(key.size());
                size_t start = val.find_first_not_of(" \t");
                if (start != std::string::npos) {
                    val = val.substr(start);
                }
                contentLength = static_cast<size_t>(std::stoul(val));
            }
        }
    }

    if (contentLength == 0) {
        return std::nullopt;
    }

    // 读取 body
    std::string body(contentLength, '\0');
    if (std::fread(body.data(), 1, contentLength, stdin) != contentLength) {
        return std::nullopt;
    }

    // 解析 JSON
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(body), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return std::nullopt;
    }
    return doc.object();
}

std::optional<QJsonObject> DapTransport::readFromBuffer() {
    // 查找 \r\n\r\n 分隔 header 和 body
    size_t headerEnd = inputBuffer_.find("\r\n\r\n", inputPos_);
    if (headerEnd == std::string::npos) {
        return std::nullopt;
    }

    std::string headers = inputBuffer_.substr(inputPos_, headerEnd - inputPos_);
    size_t bodyStart = headerEnd + 4;

    // 解析 Content-Length
    size_t contentLength = 0;
    {
        std::istringstream ss(headers);
        std::string headerLine;
        while (std::getline(ss, headerLine)) {
            if (!headerLine.empty() && headerLine.back() == '\r') {
                headerLine.pop_back();
            }
            const std::string key = "Content-Length:";
            if (headerLine.size() >= key.size() &&
                std::equal(key.begin(), key.end(), headerLine.begin(),
                           [](char a, char b) { return std::tolower(a) == std::tolower(b); })) {
                std::string val = headerLine.substr(key.size());
                size_t start = val.find_first_not_of(" \t");
                if (start != std::string::npos) {
                    val = val.substr(start);
                }
                contentLength = static_cast<size_t>(std::stoul(val));
            }
        }
    }

    if (contentLength == 0 || bodyStart + contentLength > inputBuffer_.size()) {
        return std::nullopt;
    }

    std::string body = inputBuffer_.substr(bodyStart, contentLength);
    inputPos_ = bodyStart + contentLength;

    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(body), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return std::nullopt;
    }
    return doc.object();
}

void DapTransport::write(const QJsonObject& message) {
    QByteArray body = QJsonDocument(message).toJson(QJsonDocument::Compact);
    std::string header = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";

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

// ============================================================
// DapRequestHandler 实现
// ============================================================

DapRequestHandler::DapRequestHandler() = default;

std::vector<QJsonObject> DapRequestHandler::handleMessage(const QJsonObject& message) {
    std::vector<QJsonObject> responses;

    // DAP 消息必须有 type 字段
    QString type = message.value("type").toString();
    if (type != "request") {
        return responses;
    }

    int seq = message.value("seq").toInt();
    QString command = message.value("command").toString();
    QJsonObject args = message.value("arguments").toObject();
    std::string cmd = command.toStdString();

    // 分发到对应 handler
    if (cmd == "initialize") {
        QJsonValue body = handleInitialize(args);
        responses.push_back(makeResponse(seq, cmd, true, body));
    } else if (cmd == "launch") {
        QJsonValue body = handleLaunch(args);
        // 通过 session_.isLaunched() 判断启动是否成功；
        // 失败时 handleLaunch 在 body 中放入 "message" 字段供提取
        if (session_.isLaunched()) {
            QJsonObject emptyBody;
            responses.push_back(makeResponse(seq, cmd, true, emptyBody));
        } else {
            std::string errMsg = body.toObject().value("message").toString().toStdString();
            if (errMsg.empty()) {
                errMsg = "launch failed";
            }
            QJsonObject emptyBody;
            responses.push_back(makeResponse(seq, cmd, false, emptyBody, errMsg));
        }
    } else if (cmd == "attach") {
        // DAP 二期：attach 请求。MiniLang 调试器为进程内模型，无外部进程可
        // 附加——attach 语义等价 launch（客户端 attach 配置提供 program/source）。
        QJsonValue body = handleLaunch(args);
        if (session_.isLaunched()) {
            QJsonObject emptyBody;
            responses.push_back(makeResponse(seq, cmd, true, emptyBody));
        } else {
            std::string errMsg = body.toObject().value("message").toString().toStdString();
            if (errMsg.empty()) {
                errMsg = "attach failed";
            }
            QJsonObject emptyBody;
            responses.push_back(makeResponse(seq, cmd, false, emptyBody, errMsg));
        }
    } else if (cmd == "setBreakpoints") {
        QJsonValue body = handleSetBreakpoints(args);
        responses.push_back(makeResponse(seq, cmd, true, body));
    } else if (cmd == "configurationDone") {
        QJsonValue body = handleConfigurationDone(args);
        responses.push_back(makeResponse(seq, cmd, true, body));
        // 如果已 launch 且 configurationDone 刚完成，发送 stopped(entry) 事件
        if (session_.isLaunched() && !session_.isFinished()) {
            responses.push_back(makeStoppedEvent(DapStoppedReason::Entry, session_.getCurrentLine()));
        }
    } else if (cmd == "continue") {
        // 先发送 response，再执行步进并发送事件
        QJsonObject body;
        body["allThreadsContinued"] = false;
        responses.push_back(makeResponse(seq, cmd, true, body));
        auto events = doStepAndSendEvents(&DebugSession::doContinue);
        responses.insert(responses.end(), events.begin(), events.end());
    } else if (cmd == "next") {
        QJsonObject body;
        body["allThreadsContinued"] = false;
        responses.push_back(makeResponse(seq, cmd, true, body));
        auto events = doStepAndSendEvents(&DebugSession::doStepOver);
        responses.insert(responses.end(), events.begin(), events.end());
    } else if (cmd == "stepIn") {
        QJsonObject body;
        body["allThreadsContinued"] = false;
        responses.push_back(makeResponse(seq, cmd, true, body));
        auto events = doStepAndSendEvents(&DebugSession::doStepIn);
        responses.insert(responses.end(), events.begin(), events.end());
    } else if (cmd == "stepOut") {
        QJsonObject body;
        body["allThreadsContinued"] = false;
        responses.push_back(makeResponse(seq, cmd, true, body));
        auto events = doStepAndSendEvents(&DebugSession::doStepOut);
        responses.insert(responses.end(), events.begin(), events.end());
    } else if (cmd == "pause") {
        // L21: 返回成功响应 + stopped 事件（reason=pause）。
        // 两种场景：
        // (a) continue 正在执行：stdin 轮询回调检测到数据 → doContinue 退出 →
        //     doStepAndSendEvents 发送 stopped(Pause)。此处 handlePause 仅设置标志
        //     作为后备（若 continue 已退出则标志由下次 continue 检查）。
        // (b) continue 未在执行（pause 在 continue 之前/之后到达）：直接发送
        //     stopped(Pause) 事件，客户端据此刷新调用栈。
        QJsonValue body = handlePause(args);
        responses.push_back(makeResponse(seq, cmd, true, body));
        if (session_.isLaunched() && !session_.isFinished()) {
            responses.push_back(makeStoppedEvent(DapStoppedReason::Pause, session_.getCurrentLine()));
            session_.clearPauseRequest();
        }
    } else if (cmd == "stackTrace") {
        QJsonValue body = handleStackTrace(args);
        responses.push_back(makeResponse(seq, cmd, true, body));
    } else if (cmd == "scopes") {
        QJsonValue body = handleScopes(args);
        responses.push_back(makeResponse(seq, cmd, true, body));
    } else if (cmd == "variables") {
        QJsonValue body = handleVariables(args);
        responses.push_back(makeResponse(seq, cmd, true, body));
    } else if (cmd == "evaluate") {
        QJsonValue body = handleEvaluate(args);
        responses.push_back(makeResponse(seq, cmd, true, body));
    } else if (cmd == "setVariable") {
        // DAP 二期：调试暂停时修改变量值
        QJsonValue body = handleSetVariable(args);
        QJsonObject b = body.toObject();
        bool ok = b.value("success").toBool();
        std::string msg = b.value("message").toString().toStdString();
        b.remove("success");
        b.remove("message");
        responses.push_back(makeResponse(seq, cmd, ok, b, ok ? "" : msg));
    } else if (cmd == "threads") {
        QJsonValue body = handleThreads(args);
        responses.push_back(makeResponse(seq, cmd, true, body));
    } else if (cmd == "terminate") {
        QJsonValue body = handleTerminate(args);
        responses.push_back(makeResponse(seq, cmd, true, body));
        responses.push_back(makeTerminatedEvent());
    } else if (cmd == "disconnect") {
        QJsonValue body = handleDisconnect(args);
        responses.push_back(makeResponse(seq, cmd, true, body));
        shouldExit_ = true;
    } else if (cmd == "source") {
        QJsonValue body = handleSource(args);
        responses.push_back(makeResponse(seq, cmd, false, body, "source request not supported"));
    } else {
        // 未知 command：返回错误 response
        QJsonObject emptyBody;
        responses.push_back(makeResponse(seq, cmd, false, emptyBody, "unknown command: " + cmd));
    }

    return responses;
}

QJsonObject DapRequestHandler::makeResponse(int requestSeq, const std::string& command, bool success,
                                            const QJsonValue& body, const std::string& message) {
    QJsonObject resp;
    resp["seq"] = nextSeq_++;
    resp["type"] = "response";
    resp["request_seq"] = requestSeq;
    resp["command"] = QString::fromStdString(command);
    resp["success"] = success;
    if (!message.empty()) {
        resp["message"] = QString::fromStdString(message);
    }
    resp["body"] = body;
    return resp;
}

QJsonObject DapRequestHandler::makeEvent(const std::string& event, const QJsonValue& body) {
    QJsonObject evt;
    evt["seq"] = nextSeq_++;
    evt["type"] = "event";
    evt["event"] = QString::fromStdString(event);
    evt["body"] = body;
    return evt;
}

QJsonObject DapRequestHandler::makeStoppedEvent(DapStoppedReason reason, int line, const std::string& exceptionText) {
    QJsonObject body;
    body["reason"] = QString::fromStdString(stoppedReasonString(reason));
    body["threadId"] = kMainThreadId;
    body["allThreadsStopped"] = true;
    if (line > 0) {
        // 部分客户端接受 line 字段，但标准 DAP 不要求，保留以便调试
    }
    if (!exceptionText.empty()) {
        body["text"] = QString::fromStdString(exceptionText);
    }
    return makeEvent("stopped", body);
}

QJsonObject DapRequestHandler::makeTerminatedEvent() {
    QJsonObject body;
    return makeEvent("terminated", body);
}

QJsonObject DapRequestHandler::makeOutputEvent(const std::string& output, const std::string& category) {
    QJsonObject body;
    body["category"] = QString::fromStdString(category);
    body["output"] = QString::fromStdString(output);
    return makeEvent("output", body);
}

std::vector<QJsonObject> DapRequestHandler::doStepAndSendEvents(StepResult (DebugSession::*stepFn)()) {
    std::vector<QJsonObject> events;

    // 记录步进前的输出长度，用于计算增量输出
    size_t outputBefore = session_.getOutput().size();

    // 执行步进
    StepResult result = (session_.*stepFn)();

    // 发送增量输出事件
    std::string fullOutput = session_.getOutput();
    if (fullOutput.size() > outputBefore) {
        std::string newOutput = fullOutput.substr(outputBefore);
        events.push_back(makeOutputEvent(newOutput, "stdout"));
    }

    // 根据步进结果生成事件
    switch (result) {
    case StepResult::Finished:
        events.push_back(makeTerminatedEvent());
        break;
    case StepResult::Error: {
        std::string errText = session_.hasError() ? session_.getLastError() : "runtime error";
        int errLine = session_.getLastErrorLine();
        if (errLine <= 0) {
            errLine = session_.getCurrentLine();
        }
        events.push_back(makeStoppedEvent(DapStoppedReason::Exception, errLine, errText));
        break;
    }
    case StepResult::Ok:
        // 步进暂停：continue 命中断点为 breakpoint，其他为 step
        // L21: 若 pauseRequested_ 为 true，说明是 pause 请求触发的暂停，用 Pause 原因。
        if (session_.isPauseRequested()) {
            events.push_back(makeStoppedEvent(DapStoppedReason::Pause, session_.getCurrentLine()));
            session_.clearPauseRequest();
        } else {
            // 简化：统一用 step（客户端会刷新调用栈）
            events.push_back(makeStoppedEvent(DapStoppedReason::Step, session_.getCurrentLine()));
        }
        break;
    case StepResult::MaxStepsExceeded:
        events.push_back(makeStoppedEvent(DapStoppedReason::Step, session_.getCurrentLine()));
        break;
    }

    return events;
}

QJsonValue DapRequestHandler::handleInitialize(const QJsonObject& /*args*/) {
    QJsonObject capabilities;
    capabilities["supportsConfigurationDoneRequest"] = true;
    capabilities["supportsFunctionBreakpoints"] = false;
    // DAP 二期：条件断点/命中条件/setVariable 已实现
    capabilities["supportsConditionalBreakpoints"] = true;
    capabilities["supportsHitConditionalBreakpoints"] = true;
    capabilities["supportsEvaluateForHovers"] = true;
    capabilities["supportsStepBack"] = false;
    capabilities["supportsSetVariable"] = true;
    capabilities["supportsTerminateRequest"] = true;
    capabilities["supportsDisassembleRequest"] = false;
    capabilities["supportsCancelRequest"] = false;
    capabilities["supportsBreakpointLocationsRequest"] = false;

    QJsonObject body;
    body["capabilities"] = capabilities;

    // 附加 linesStartAt1：MiniLang 行号 1-based，与 DAP 默认一致
    body["linesStartAt1"] = true;
    body["columnsStartAt1"] = true;

    return body;
}

QJsonValue DapRequestHandler::handleLaunch(const QJsonObject& args) {
    std::string filePath;
    std::string source;

    // 优先从 program 字段提取文件路径
    if (args.contains("program")) {
        filePath = args.value("program").toString().toStdString();
    }

    // 若无 program，尝试从 source.path 提取
    if (filePath.empty() && args.contains("source")) {
        QJsonValue srcVal = args.value("source");
        if (srcVal.isObject()) {
            filePath = srcVal.toObject().value("path").toString().toStdString();
        } else if (srcVal.isString()) {
            // source 直接为源码字符串（测试用）
            source = srcVal.toString().toStdString();
        }
    }

    // 若有文件路径且尚无源码，尝试读取文件内容
    // 文件读取失败时不直接报错，继续尝试 source.source 内联源码回退
    bool fileReadFailed = false;
    if (!filePath.empty() && source.empty()) {
        source = readFileContent(filePath);
        if (source.empty()) {
            fileReadFailed = true;
        }
    }

    // 若仍无源码，尝试从 args["source"]["source"] 获取内联源码
    if (source.empty() && args.contains("source")) {
        QJsonValue srcVal = args.value("source");
        if (srcVal.isObject()) {
            QJsonValue innerSrc = srcVal.toObject().value("source");
            if (innerSrc.isString()) {
                source = innerSrc.toString().toStdString();
            }
        }
    }

    if (source.empty()) {
        QJsonObject errBody;
        if (fileReadFailed) {
            errBody["message"] = QString::fromStdString("无法读取文件: " + filePath);
        } else {
            errBody["message"] = "launch 缺少 program 或 source 参数";
        }
        return errBody;
    }

    // 启动调试会话
    std::string err = session_.launch(source, filePath);
    if (!err.empty()) {
        QJsonObject errBody;
        errBody["message"] = QString::fromStdString(err);
        return errBody;
    }

    initialized_ = true;
    // 成功时返回空 body（错误信息通过 session_.isLaunched() 在 handleMessage 中判断）
    return QJsonObject();
}

QJsonValue DapRequestHandler::handleSetVariable(const QJsonObject& args) {
    // DAP 二期：setVariable 请求。body 中的 success/message 为内部传递字段，
    // 分发层提取后移除（DAP 响应的 success 在外层 response 对象）。
    int ref = args.value("variablesReference").toInt();
    std::string name = args.value("name").toString().toStdString();
    std::string valueText = args.value("value").toString().toStdString();

    auto result = session_.setVariable(ref, name, valueText);

    QJsonObject body;
    body["success"] = result.ok;
    if (result.ok) {
        body["value"] = QString::fromStdString(result.value);
        body["type"] = QString::fromStdString(result.type);
        body["variablesReference"] = result.variablesReference;
    } else {
        body["message"] = QString::fromStdString(result.message);
    }
    return body;
}

QJsonValue DapRequestHandler::handleSetBreakpoints(const QJsonObject& args) {
    QJsonArray breakpointsArray;
    std::vector<DebugSession::SourceBreakpoint> bps;

    // DAP 二期：从 args["breakpoints"] 提取 line + condition + hitCondition
    if (args.contains("breakpoints")) {
        QJsonValue bpVal = args.value("breakpoints");
        if (bpVal.isArray()) {
            breakpointsArray = bpVal.toArray();
            for (const QJsonValue& v : breakpointsArray) {
                if (v.isObject()) {
                    QJsonObject bpObj = v.toObject();
                    DebugSession::SourceBreakpoint sbp;
                    sbp.line = bpObj.value("line").toInt();
                    sbp.condition = bpObj.value("condition").toString().toStdString();
                    sbp.hitCondition = bpObj.value("hitCondition").toString().toStdString();
                    if (sbp.line > 0) {
                        bps.push_back(std::move(sbp));
                    }
                }
            }
        }
    }

    // 设置断点（全量替换，含条件元数据）
    session_.setSourceBreakpoints(bps);

    // 构建断点响应
    QJsonArray responseBreakpoints;
    int bpId = 0;
    for (const auto& sbp : bps) {
        DapBreakpoint bp;
        bp.id = bpId++;
        bp.line = sbp.line;
        bp.verified = true;

        // 附加 source 信息
        if (args.contains("source")) {
            QJsonValue srcVal = args.value("source");
            if (srcVal.isObject()) {
                bp.source.path = srcVal.toObject().value("path").toString().toStdString();
                bp.source.name = extractFilename(bp.source.path);
            }
        }

        responseBreakpoints.append(json::breakpoint(bp));
    }

    QJsonObject body;
    body["breakpoints"] = responseBreakpoints;
    return body;
}

QJsonValue DapRequestHandler::handleConfigurationDone(const QJsonObject& /*args*/) {
    configurationDone_ = true;
    return QJsonObject();
}

QJsonValue DapRequestHandler::handleContinue(const QJsonObject& /*args*/) {
    // 实际步进在 handleMessage 中通过 doStepAndSendEvents 执行
    // 此处仅返回 response body
    QJsonObject body;
    body["allThreadsContinued"] = false;
    return body;
}

QJsonValue DapRequestHandler::handleNext(const QJsonObject& /*args*/) {
    QJsonObject body;
    body["allThreadsContinued"] = false;
    return body;
}

QJsonValue DapRequestHandler::handleStepIn(const QJsonObject& /*args*/) {
    QJsonObject body;
    body["allThreadsContinued"] = false;
    return body;
}

QJsonValue DapRequestHandler::handleStepOut(const QJsonObject& /*args*/) {
    QJsonObject body;
    body["allThreadsContinued"] = false;
    return body;
}

QJsonValue DapRequestHandler::handleStackTrace(const QJsonObject& args) {
    // threadId 参数忽略（MiniLang 单线程）
    (void)args.value("threadId").toInt();

    // 可选的 levels 参数（限制返回帧数）
    int maxLevels = args.value("levels").toInt(0); // 0 = 全部
    int startFrame = args.value("startFrame").toInt(0);

    auto frames = session_.getCallStack();

    QJsonArray frameArray;
    int count = 0;
    for (int i = startFrame; i < static_cast<int>(frames.size()); ++i) {
        if (maxLevels > 0 && count >= maxLevels) {
            break;
        }
        frameArray.append(json::stackFrame(frames[static_cast<size_t>(i)]));
        ++count;
    }

    QJsonObject body;
    body["stackFrames"] = frameArray;
    body["totalFrames"] = static_cast<int>(frames.size());
    return body;
}

QJsonValue DapRequestHandler::handleScopes(const QJsonObject& args) {
    int frameId = args.value("frameId").toInt(0);
    auto scopes = session_.getScopes(frameId);

    QJsonArray scopeArray;
    for (const auto& s : scopes) {
        scopeArray.append(json::scope(s));
    }

    QJsonObject body;
    body["scopes"] = scopeArray;
    return body;
}

QJsonValue DapRequestHandler::handleVariables(const QJsonObject& args) {
    int variablesReference = args.value("variablesReference").toInt(0);
    auto vars = session_.getVariables(variablesReference);

    QJsonArray varArray;
    for (const auto& v : vars) {
        varArray.append(json::variable(v));
    }

    QJsonObject body;
    body["variables"] = varArray;
    return body;
}

QJsonValue DapRequestHandler::handleEvaluate(const QJsonObject& args) {
    std::string expr = args.value("expression").toString().toStdString();
    int frameId = args.value("frameId").toInt(0);

    auto result = session_.evaluate(expr, frameId);

    QJsonObject body;
    if (result.ok) {
        body["result"] = QString::fromStdString(result.value);
        if (!result.type.empty()) {
            body["type"] = QString::fromStdString(result.type);
        }
        body["variablesReference"] = result.variablesReference;
    } else {
        body["result"] = QString::fromStdString("无法求值: " + expr);
        body["variablesReference"] = 0;
    }
    return body;
}

QJsonValue DapRequestHandler::handleThreads(const QJsonObject& /*args*/) {
    QJsonArray threads;
    QJsonObject thread;
    thread["id"] = kMainThreadId;
    thread["name"] = "main";
    threads.append(thread);

    QJsonObject body;
    body["threads"] = threads;
    return body;
}

QJsonValue DapRequestHandler::handleTerminate(const QJsonObject& /*args*/) {
    session_.reset();
    shouldExit_ = true;
    return QJsonObject();
}

QJsonValue DapRequestHandler::handleDisconnect(const QJsonObject& /*args*/) {
    session_.reset();
    shouldExit_ = true;
    return QJsonObject();
}

QJsonValue DapRequestHandler::handleSource(const QJsonObject& /*args*/) {
    // 不支持 source 请求（源码在磁盘上，sourceReference=0）
    return QJsonObject();
}

QJsonValue DapRequestHandler::handlePause(const QJsonObject& /*args*/) {
    // L21: 同步暂停支持。
    // (a) 若 continue 尚未启动（pause 在 continue 之前到达），设置 pauseRequested_
    //     标志，下一次 doContinue 启动时首轮检查即退出。
    // (b) 若 continue 正在执行（不可能在当前同步模型中发生——handleMessage 同步
    //     处理，doContinue 占用控制权直到结束），pauseRequested_ 由 stdin 轮询
    //     回调间接设置（dap.cpp 检测到 stdin 数据 → 回调返回 true → doContinue 退出
    //     → main 循环读取 pause 消息 → 调用 handlePause → 设置标志）。
    // 成功响应后 DAP 客户端期望收到 stopped 事件（reason=pause），
    // 该事件由 doStepAndSendEvents 在 doContinue 返回 Ok 后发送。
    session_.requestPause();
    return QJsonObject();
}

bool DapRequestHandler::extractSourceAndLine(const QJsonObject& args, std::string& path, int& line) const {
    bool found = false;
    if (args.contains("source")) {
        QJsonValue srcVal = args.value("source");
        if (srcVal.isObject()) {
            path = srcVal.toObject().value("path").toString().toStdString();
            if (!path.empty()) {
                found = true;
            }
        }
    }
    if (args.contains("line")) {
        line = args.value("line").toInt();
    }
    return found;
}

// ============================================================
// CLI 辅助函数
// ============================================================

void printHelp() {
    std::fputs("minilang-dap - MiniLang Debug Adapter Protocol 实现\n"
               "\n"
               "用法:\n"
               "  minilang-dap [--stdio] [--help] [--version]\n"
               "\n"
               "选项:\n"
               "  --stdio     使用 stdio 作为传输层（默认，DAP 标准模式）\n"
               "  --help      显示帮助信息\n"
               "  --version   显示版本信息\n"
               "\n"
               "DAP 调试适配器通过 JSON-RPC 2.0 over stdio 与编辑器通信。\n"
               "在 VS Code 中通过 launch.json 配置 type=minilang 启动，\n"
               "或手动配置 debugger 命令为 minilang-dap --stdio。\n"
               "\n"
               "支持的 DAP 功能:\n"
               "  - launch / setBreakpoints / configurationDone\n"
               "  - continue / next / stepIn / stepOut\n"
               "  - stackTrace / scopes / variables\n"
               "  - evaluate（变量名求值，支持 hover）\n"
               "  - threads / terminate / disconnect\n",
               stdout);
}

void printVersion() {
    std::fprintf(stdout, "minilang-dap %s\n", versionString().c_str());
}

std::string versionString() {
    return "1.0.0 (R165)";
}

std::string serverName() {
    return "minilang-dap";
}

// ============================================================
// DAP 服务器主循环（供 dap_entry.cpp 统一入口调用）
// ============================================================

int runDapServer() {
    DapTransport transport;
    DapRequestHandler handler;

    while (!handler.shouldExit()) {
        auto message = transport.read();
        if (!message) {
            // EOF 或读取错误
            return 0;
        }

        // 处理消息前清除 pause 标志
        handler.session().clearPauseRequest();

        std::vector<QJsonObject> responses = handler.handleMessage(*message);
        for (const auto& resp : responses) {
            transport.write(resp);
        }
    }

    return 0;
}

} // namespace minilang_dap
