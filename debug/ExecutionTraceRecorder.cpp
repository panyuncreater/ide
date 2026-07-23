// ============================================================
// ExecutionTraceRecorder.cpp — 可回放执行时间轴核心抽象实现（R114 阶段 1+2+3）
// ============================================================

#include "debug/ExecutionTraceRecorder.h"

#include "app/VmStepper.h" // VmStepper inline accessors
#include "interpreter/CallFrame.h"
#include "interpreter/Environment.h"
#include "interpreter/Interpreter.h"
#include "interpreter/Value.h"

#include <algorithm>
#include <sstream>

// ============================================================
// TraceSnapshot::toJson — 阶段 2 持久化序列化
// ============================================================
std::string TraceSnapshot::toJson() const {
    std::ostringstream os;
    os << "{";
    os << "\"step\":" << step << ",";
    os << "\"backend\":\"";
    switch (backend) {
    case TraceBackend::Interpreter:
        os << "interpreter";
        break;
    case TraceBackend::StackVM:
        os << "stackvm";
        break;
    case TraceBackend::RegisterVM:
        os << "registervm";
        break;
    }
    os << "\",";
    os << "\"line\":" << line << ",";
    os << "\"column\":" << column << ",";
    os << "\"opOrNodeName\":\"" << opOrNodeName << "\",";
    os << "\"astNodePath\":\"" << astNodePath << "\",";
    os << "\"ip\":" << ip << ",";
    os << "\"frameCount\":" << frameCount << ",";

    // 栈快照（字符串数组）
    os << "\"stack\":[";
    for (size_t i = 0; i < stackStrings.size(); ++i) {
        if (i > 0)
            os << ",";
        os << "\"" << stackStrings[i] << "\"";
    }
    os << "],";

    // 寄存器快照
    os << "\"registers\":[";
    for (size_t i = 0; i < registerStrings.size(); ++i) {
        if (i > 0)
            os << ",";
        os << "\"" << registerStrings[i] << "\"";
    }
    os << "],";

    // 全局变量
    os << "\"globals\":[";
    for (size_t i = 0; i < globalsStrings.size(); ++i) {
        if (i > 0)
            os << ",";
        os << "{\"name\":\"" << globalsStrings[i].first << "\",\"value\":\"" << globalsStrings[i].second << "\"}";
    }
    os << "],";

    // 可见变量
    os << "\"visibleVars\":[";
    for (size_t i = 0; i < visibleVarsStrings.size(); ++i) {
        if (i > 0)
            os << ",";
        os << "{\"name\":\"" << visibleVarsStrings[i].first << "\",\"value\":\"" << visibleVarsStrings[i].second
           << "\"}";
    }
    os << "],";

    // 调用栈
    os << "\"callStack\":[";
    for (size_t i = 0; i < callStack.size(); ++i) {
        if (i > 0)
            os << ",";
        os << "{\"function\":\"" << callStack[i].functionName << "\",";
        os << "\"line\":" << callStack[i].line << ",";
        os << "\"depth\":" << callStack[i].depth << ",";
        os << "\"locals\":[";
        for (size_t j = 0; j < callStack[i].locals.size(); ++j) {
            if (j > 0)
                os << ",";
            os << "{\"name\":\"" << callStack[i].locals[j].first << "\",\"value\":\""
               << callStack[i].locals[j].second.toString() << "\"}";
        }
        os << "]}";
    }
    os << "]";

    os << "}";
    return os.str();
}

// ============================================================
// ExecutionTraceRecorder — 实现
// ============================================================

void ExecutionTraceRecorder::pushSnapshot(TraceSnapshot&& snap) {
    if (!enabled_.load(std::memory_order_relaxed))
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    snap.step = stepCounter_++;
    if (snap.step >= maxSteps_) {
        // ring buffer 策略：移除最旧的快照
        if (!snapshots_.empty())
            snapshots_.erase(snapshots_.begin());
        // stepCounter_ 保持单调递增；UI 通过 stepAt(idx) 索引访问，
        // 步号字段（snap.step）会超过 maxSteps_，但 vector 索引始终从 0 开始
    }
    snapshots_.push_back(std::move(snap));
}

namespace {

/// Value → 字符串（带环检测保护，toString 内部已有）
std::string valueToStringSafe(const Value& v) {
    try {
        return v.toString();
    } catch (...) {
        return "<error>";
    }
}

} // namespace

void ExecutionTraceRecorder::captureVmStep(VmStepper& stepper, TraceBackend backend) {
    if (!enabled_.load(std::memory_order_relaxed))
        return;

    TraceSnapshot snap;
    snap.backend = backend;
    snap.line = stepper.getCurrentLine();
    snap.ip = stepper.getCurrentIP();
    snap.opOrNodeName = stepper.getCurrentOpCodeName();
    snap.frameCount = stepper.getFrameCount();

    // 操作数栈 / 寄存器窗口
    auto stack = stepper.getStack();
    snap.stackStrings.reserve(stack.size());
    for (const auto& v : stack) {
        snap.stackStrings.push_back(valueToStringSafe(v));
    }
    if (backend == TraceBackend::RegisterVM) {
        snap.registerStrings = snap.stackStrings; // RegisterVM 的 getStack 返回寄存器窗口
    }

    // 调用栈（含每帧 locals）
    snap.callStack = stepper.getCallStack();

    // 全局变量
    auto globals = stepper.getGlobals();
    snap.globalsStrings.reserve(globals.size());
    for (const auto& kv : globals) {
        snap.globalsStrings.emplace_back(kv.first, valueToStringSafe(kv.second));
    }

    // 当前帧局部变量作为"可见变量"
    auto currentLocals = stepper.getCurrentFrameLocals();
    snap.visibleVarsStrings.reserve(currentLocals.size());
    for (const auto& kv : currentLocals) {
        snap.visibleVarsStrings.emplace_back(kv.first, valueToStringSafe(kv.second));
    }

    // 阶段 3：结构化深拷贝（FullState 模式）
    if (recordingMode_ == RecordingMode::FullState) {
        snap.stackValues = std::move(stack);
        if (backend == TraceBackend::RegisterVM) {
            snap.registerValues = snap.stackValues; // 寄存器窗口与栈相同
        }
        for (const auto& kv : globals) {
            snap.globalsValues.emplace_back(kv.first, kv.second);
        }
        for (const auto& kv : currentLocals) {
            snap.visibleVarsValues.emplace_back(kv.first, kv.second);
        }
    }

    pushSnapshot(std::move(snap));
}

void ExecutionTraceRecorder::captureInterpreterStep(Interpreter& interp, int nodeLine, int nodeColumn,
                                                    const std::string& nodeName, const std::string& astNodePath) {
    if (!enabled_.load(std::memory_order_relaxed))
        return;

    TraceSnapshot snap;
    snap.backend = TraceBackend::Interpreter;
    snap.line = nodeLine;
    snap.column = nodeColumn;
    snap.opOrNodeName = nodeName;
    snap.astNodePath = astNodePath;
    snap.ip = 0; // Interpreter 模式无 IP 概念

    // 调用栈快照（跨线程值拷贝）——先取调用栈以确定 frameCount
    auto frames = interp.getCallStackSnapshot();
    snap.frameCount = frames.size();

    // 跨线程安全：currentEnvironmentShared 返回 shared_ptr 副本
    auto env = interp.currentEnvironmentShared();
    if (env) {
        auto vars = env->allVariablesMap();
        snap.visibleVarsStrings.reserve(vars.size());
        for (const auto& kv : vars) {
            snap.visibleVarsStrings.emplace_back(kv.first, valueToStringSafe(kv.second));
        }
        if (recordingMode_ == RecordingMode::FullState) {
            for (const auto& kv : vars) {
                snap.visibleVarsValues.emplace_back(kv.first, kv.second);
            }
        }
    }
    snap.callStack.reserve(frames.size());
    for (const auto& f : frames) {
        CallStackEntry entry;
        entry.functionName = f.functionName;
        entry.line = f.line;
        entry.depth = f.depth;
        if (f.env) {
            auto frameVars = f.env->allVariablesMap();
            for (const auto& kv : frameVars) {
                entry.locals.emplace_back(kv.first, kv.second);
            }
        }
        snap.callStack.push_back(std::move(entry));
    }

    // 全局环境
    auto globalEnv = interp.getGlobalEnvironment();
    if (globalEnv) {
        auto globals = globalEnv->allVariablesMap();
        snap.globalsStrings.reserve(globals.size());
        for (const auto& kv : globals) {
            snap.globalsStrings.emplace_back(kv.first, valueToStringSafe(kv.second));
        }
        if (recordingMode_ == RecordingMode::FullState) {
            for (const auto& kv : globals) {
                snap.globalsValues.emplace_back(kv.first, kv.second);
            }
        }
    }

    pushSnapshot(std::move(snap));
}

// ============================================================
// 全局单例
// ============================================================
ExecutionTraceRecorder& traceRecorder() {
    static ExecutionTraceRecorder instance;
    return instance;
}
