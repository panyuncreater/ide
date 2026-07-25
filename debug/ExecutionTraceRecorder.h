#pragma once

// ============================================================
// ExecutionTraceRecorder — 可回放执行时间轴核心抽象（R114 阶段 1+2+3）
// ------------------------------------------------------------
// 三后端统一的执行轨迹记录器。订阅 VmStepper 的步进事件与
// Interpreter 的 checkBreak 钩子，每步采集 TraceSnapshot 写入
// 环形缓冲区。GUI 时间轴 widget 通过 stepAt(i) 随机访问任意步。
//
// 三阶段能力：
//   阶段 1：单步快照采集（栈/locals/globals/callStack 字符串化）
//   阶段 2：三后端统一 + AST 节点路径 + Value JSON 序列化
//   阶段 3：状态深拷贝（Environment/VM 帧/Value 副本）支持回滚
//
// 线程模型：
//   - VM 路径在主线程调用（VmStepper 的 QTimer + UI 槽）
//   - Interpreter 路径在 worker 线程调用（checkBreak 内）
//   - 内部 mutex 保护 deque，跨线程 push 主线程 read
//
// 容量策略：
//   - 默认上限 5000 步（教学场景足够，避免内存膨胀）
//   - 超限后采用 ring buffer 策略：pop_front + push_back
//   - 用户可调整容量（如长循环场景可调至 20000）
// ============================================================

#include <QSet>
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "debug/DebugTypes.h" // CallStackEntry
#include "interpreter/Value.h"

class Interpreter;
class VM;
class RegisterVM;
class VmStepper;

/// 后端类型标识（用于快照溯源与三后端对比）
enum class TraceBackend : uint8_t {
    Interpreter, // AST 节点级步进
    StackVM,     // ~60 OpCode 字节码步进
    RegisterVM,  // ~50 RegOp 寄存器指令步进
};

/// 单步快照——三后端统一格式
struct TraceSnapshot {
    size_t step = 0;          // 步号（从 0 开始）
    TraceBackend backend;     // 来源后端
    int line = 0;             // 源码行号
    int column = 0;           // 源码列号
    std::string opOrNodeName; // VM 模式：OP_ADD；Interpreter 模式：IfStmt
    std::string astNodePath;  // AST 节点路径（阶段 2 填充）
    size_t ip = 0;            // VM 模式 IP；Interpreter 模式 0
    size_t frameCount = 0;    // 帧深度

    // ---- 字符串化快照（阶段 1）----
    std::vector<std::string> stackStrings;                               // 操作数栈顶→底
    std::vector<std::string> registerStrings;                            // 寄存器窗口（仅 RegisterVM）
    std::vector<CallStackEntry> callStack;                               // 含每帧 locals
    std::vector<std::pair<std::string, std::string>> globalsStrings;     // 全局变量
    std::vector<std::pair<std::string, std::string>> visibleVarsStrings; // 当前可见变量

    // ---- 结构化快照（阶段 3：状态回滚）----
    // 仅在 recordingMode_ == FullState 时填充，否则为空
    std::vector<Value> stackValues;                               // 操作数栈值深拷贝
    std::vector<Value> registerValues;                            // 寄存器值深拷贝
    std::vector<std::pair<std::string, Value>> globalsValues;     // 全局变量值
    std::vector<std::pair<std::string, Value>> visibleVarsValues; // 可见变量值
    // L18: Interpreter 后端完整状态快照（类型擦除，避免头文件循环依赖）。
    // 实际类型为 std::shared_ptr<Interpreter::StateSnapshot>。
    // 仅 Interpreter 后端 + FullState 模式填充；VM 后端为空。
    // 回滚时 VmStepper/DebugController 通过 Interpreter::restoreFromSnapshot 消费。
    std::shared_ptr<void> interpreterState;
    // 注：Environment 链与 VM 帧的深拷贝由 PlaybackController 在回滚时按需重建，
    // 不在每个快照中存储（避免每步深拷贝整个环境链导致内存爆炸）
    // L18 更新：Interpreter 后端例外——interpreterState 持有完整环境链快照，
    // 因为 Interpreter 无 IP/帧概念，必须依赖完整状态快照回滚（而非重建）

    /// JSON 序列化（阶段 2）——用于持久化/跨语言交换
    std::string toJson() const;
};

/// 录制模式
enum class RecordingMode : uint8_t {
    StringsOnly, // 阶段 1：仅字符串化快照（最简，安全）
    FullState,   // 阶段 3：含 Value 深拷贝（支持状态回滚）
};

/// 可回放执行时间轴记录器
class ExecutionTraceRecorder {
public:
    ExecutionTraceRecorder() = default;
    ~ExecutionTraceRecorder() = default;

    // ---- 配置 ----
    void setCapacity(size_t maxSteps) { maxSteps_ = maxSteps; }
    size_t capacity() const { return maxSteps_; }
    void setRecordingMode(RecordingMode mode) { recordingMode_ = mode; }
    RecordingMode recordingMode() const { return recordingMode_; }

    /// 启用/禁用录制。禁用时所有 capture* 方法变为空操作。
    void setEnabled(bool enabled) { enabled_ = enabled; }
    bool isEnabled() const { return enabled_; }

    // ---- 三后端统一的快照采集接口 ----
    // 这些方法由 VmStepper / Interpreter 在步进点调用，传入当前状态。

    /// VM 路径采集（StackVM / RegisterVM 通用）
    /// @param stepper VmStepper 引用，用于读取 getStack/getCallStack/getCurrentFrameLocals 等
    /// @param backend 当前后端类型
    void captureVmStep(VmStepper& stepper, TraceBackend backend);

    /// Interpreter 路径采集
    /// @param interp Interpreter 引用，用于读取 currentEnvironment/getCallStackSnapshot
    /// @param nodeLine 当前 AST 节点行号
    /// @param nodeColumn 列号
    /// @param nodeName 节点类型名（如 "IfStmt" / "BinaryOp"）
    /// @param astNodePath AST 节点路径（阶段 2，如 "IfStmt#42"）
    void captureInterpreterStep(Interpreter& interp, int nodeLine, int nodeColumn, const std::string& nodeName,
                                const std::string& astNodePath = "");

    // ---- 快照访问 ----
    /// 快照数量
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshots_.size();
    }

    /// 是否为空
    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshots_.empty();
    }

    /// 获取第 idx 步快照（const 副本，线程安全）
    /// @param idx 步号索引，必须 < size()
    std::optional<TraceSnapshot> stepAt(size_t idx) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (idx >= snapshots_.size())
            return std::nullopt;
        return snapshots_[idx];
    }

    /// 获取所有快照（const 副本，线程安全）
    std::vector<TraceSnapshot> allSnapshots() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshots_;
    }

    /// 获取最后一步步号（用于 UI 显示"当前步 / 总步数"）
    size_t lastStep() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshots_.empty() ? 0 : snapshots_.back().step;
    }

    // ---- 控制 ----
    /// 清空所有快照（重置录制会话）
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshots_.clear();
        stepCounter_ = 0;
    }

    /// 开始新录制会话（清空 + 重置步号）
    void startSession() {
        clear();
        enabled_ = true;
    }

    /// 结束录制会话（禁用录制但保留快照）
    void endSession() { enabled_ = false; }

private:
    mutable std::mutex mutex_;
    std::vector<TraceSnapshot> snapshots_;
    size_t stepCounter_ = 0;
    size_t maxSteps_ = 5000;
    RecordingMode recordingMode_ = RecordingMode::StringsOnly;
    std::atomic<bool> enabled_{false};

    /// 内部推送快照（容量管理 + 步号分配）
    void pushSnapshot(TraceSnapshot&& snap);
};

/// 全局单例（IDE 内单一录制器，三后端共享）
ExecutionTraceRecorder& traceRecorder();
