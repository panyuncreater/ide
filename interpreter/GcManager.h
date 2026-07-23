#pragma once

// ============================================================
// GcManager.h — 循环引用垃圾收集器（Bug2 fix）
// ------------------------------------------------------------
// 侵入式引用计数无法回收循环引用（#9 已知限制）。本管理器实现
// 轻量级 mark-sweep，周期性扫描所有容器节点，标记从根集可达的
// 对象，释放不可达的循环孤岛。
//
// 工作流程：
//   1. registerTracked(obj): 所有新建的 ArrayData/DictData/InstanceData
//      通过此方法注册到 tracked 列表，并加入 aliveSet_。
//   2. RefCounted 析构时调用 onDestroyed(this)，从 aliveSet_ 移除。
//      这样 collectCycle 迭代 tracked_ 时可通过 aliveSet_ 区分
//      存活对象与已释放的悬垂指针，避免 UAF。
//   3. collectCycle(roots): 从 roots 出发 mark 所有可达容器节点，
//      然后 sweep tracked 列表中 aliveSet_ 仍在但 marked 未标记的
//      节点（即不可达的循环孤岛）——清空其子元素打破循环。
//
// 触发时机（BUG-003 fix: 双触发机制）：
//   - 兜底触发：Interpreter::execute() 在 resetState 之后、runStatements 之前
//     调用 collectCycle(空根集)。此时上一轮残留的循环容器 refCount>0
//     仍 aliveSet_，本轮新建容器尚未注册，安全。
//   - 增量触发：registerTracked 累计 allocationsSinceLastGc_ 达到
//     GC_ALLOCATION_THRESHOLD 时，调用 gcTriggerCallback_（由 Interpreter
//     注册），Interpreter 收集当前 callStack/globalEnv 中的根集后调用
//     collectCycle。这解决了长时间运行函数内累积循环引用的内存峰值问题。
//
// 性能权衡：
//   - mark-sweep 的开销是 O(节点数 + 边数)，仅在 execute 起点或分配阈值触发
//   - RefCounted 析构增加一个 bool 检查（非跟踪对象零开销）
//   - tracked_ 用 vector，aliveSet_ 用 unordered_set，注册/注销均 O(1)
// ============================================================

#include "interpreter/ValueTypes.h" // ValueType
#include <functional>
#include <unordered_set>
#include <vector>

struct RefCounted;
struct Value;

// R113 C 项：GC 阶段状态枚举——用于 MemoryModelPanel 实时展示当前 mark-sweep 进度。
// collectCycle 是同步阻塞操作，currentPhase_ 在阶段切换时短暂变更，
// collectCycle 结束后回归 Idle。UI 在 animTimer_ 周期内能观察到上次 GC 经历的阶段。
enum class GcPhase : uint8_t {
    Idle,       // 未在 GC 中（或上次 GC 已结束）
    Marking,    // Phase 1: 从 roots 出发 mark 所有可达容器节点
    Sweeping,   // Phase 2: 清扫 marked 中不存在但 aliveSet_ 中存在的循环孤岛
    Finalizing, // Phase 3: 重建 tracked_ / aliveSet_，重置 marked 标志
};

// R135 GC 模式枚举——三种内存管理策略可切换，用于教学对比与性能基准。
// 模式语义：
//   RefCountOnly        - 纯引用计数，不注册 GcManager，循环引用会泄漏（基线对比用）
//   RefCountWithCycleGc - 引用计数主导 + GC 仅回收循环孤岛（默认，项目历史模式）
//   GcOnly              - GC 主导：sweep 阶段直接 delete 不可达对象（实验性）
// 切换时机：仅在 Interpreter/VM 完全重置后切换（避免运行中切换导致状态不一致）
enum class GcMode : uint8_t {
    RefCountOnly = 0,
    RefCountWithCycleGc = 1,
    GcOnly = 2,
};

class GcManager {
public:
    // BUG-003 fix: 默认分配阈值。8192 个容器节点触发一次增量 GC，
    // 平衡内存峰值与 GC 开销（每次 mark-sweep O(N+E)，N=tracked 节点数）。
    // 测试场景可通过 setGcAllocationThreshold 调小验证触发逻辑。
    static constexpr size_t GC_ALLOCATION_THRESHOLD = 8192;

    static GcManager& instance() {
        static GcManager gc;
        return gc;
    }

    // 注册容器节点到 tracked 列表 + aliveSet_
    // 仅注册 ArrayData/DictData/InstanceData（可能形成循环的类型）
    // StringData/ClosureData 不注册（StringData 无子引用，ClosureData 已用 weak_ptr 打破循环）
    void registerTracked(RefCounted* obj);

    // RefCounted 析构钩子：从 aliveSet_ 移除本指针
    // 安全性：单线程 collectCycle 期间不会并发；this 在析构函数内仍有效
    void onDestroyed(RefCounted* obj);

    // 执行一次 mark-sweep 收集
    // roots: 当前存活的根集（globals/stack/frames 中的容器 Value）
    // 从 roots 出发 mark 所有可达容器，sweep 未标记的循环孤岛
    void collectCycle(const std::vector<const void*>& roots);

    // 清空 tracked 列表 + aliveSet_（VM/Interpreter 完全重置时调用）
    // 注意：不释放节点（节点由 refCount 管理），仅清空跟踪结构
    void reset();

    // 调试：当前 tracked 节点数
    size_t trackedCount() const { return tracked_.size(); }

    // BUG-003 fix: 设置增量 GC 触发回调（由 Interpreter 在构造时注册）
    // 回调内 Interpreter 收集当前根集（globalEnv_、callStack_ 各帧 env 等）
    // 并调用 collectCycle。回调可为空（无 Interpreter 时跳过增量触发）。
    void setGcTriggerCallback(std::function<void()> cb) { gcTriggerCallback_ = std::move(cb); }

    // BUG-003 fix: 上次 GC 后累计分配的容器数（用于诊断/测试）
    size_t allocationsSinceLastGc() const { return allocationsSinceLastGc_; }

    // BUG-003 fix: 配置增量触发阈值（测试场景可调小以验证触发行为）
    void setGcAllocationThreshold(size_t threshold) { gcAllocationThreshold_ = threshold; }

    // R113 C 项：GC 进度只读统计——供 MemoryModelPanel 实时展示上次 GC 经历的阶段
    // 与结果。collectCycle 同步阻塞，currentPhase_ 在阶段切换时短暂变更，结束后
    // 回归 Idle；UI 在 animTimer_ 周期内观察到的值通常是 Idle（除非 GC 恰好在该
    // 周期内执行）。lastMarkedCount/lastCollectedCount 反映上次 GC 的结果。
    GcPhase currentPhase() const { return currentPhase_; }
    size_t lastMarkedCount() const { return lastMarkedCount_; }
    size_t lastCollectedCount() const { return lastCollectedCount_; }
    size_t totalGcCount() const { return totalGcCount_; }

    // R135 GC 模式切换 API——运行前切换内存管理策略。
    // 默认 RefCountWithCycleGc（项目历史行为）。
    // RefCountOnly: 跳过 registerTracked，循环引用会泄漏（基线对比）
    // GcOnly: sweep 阶段直接 delete 不可达对象（实验性，存在 COW/VM root 限制）
    // 切换时机：仅在 Interpreter/VM 完全重置后（reset() 后）切换，避免运行中
    // 切换导致 tracked_/aliveSet_ 状态不一致。
    GcMode gcMode() const { return gcMode_; }
    void setGcMode(GcMode mode) { gcMode_ = mode; }

private:
    GcManager() = default;
    ~GcManager() = default;
    GcManager(const GcManager&) = delete;
    GcManager& operator=(const GcManager&) = delete;

    // 从单个 Value 出发递归 mark 所有可达容器
    void markValue(const Value& v, std::unordered_set<const void*>& marked);

    // BUG-003 fix: 检查分配阈值，达到则调用 gcTriggerCallback_ 进行增量回收
    void checkIncrementalGc();

    // tracked 节点列表（弱引用，对象由 refCount 管理生命周期）
    // collectCycle 期间可能有 nullptr 墓碑（对象已析构但还未 compact）
    std::vector<RefCounted*> tracked_;

    // 当前存活的 tracked 对象集合（析构时移除）
    // 用于在 collectCycle 迭代 tracked_ 时区分存活对象与悬垂指针
    std::unordered_set<RefCounted*> aliveSet_;

    // BUG-003 fix: 增量触发相关状态
    size_t allocationsSinceLastGc_ = 0;
    size_t gcAllocationThreshold_ = GC_ALLOCATION_THRESHOLD;
    std::function<void()> gcTriggerCallback_;
    // 进行中标志：防止回调内 collectCycle → 析构 → registerTracked → 再触发 GC 的递归
    bool gcInProgress_ = false;

    // R113 C 项：GC 进度统计（仅 collectCycle 内部写入，外部只读访问）
    GcPhase currentPhase_ = GcPhase::Idle;
    size_t lastMarkedCount_ = 0;    // 上次 GC 标记的可达节点数
    size_t lastCollectedCount_ = 0; // 上次 GC 回收的孤岛数
    size_t totalGcCount_ = 0;       // 累计 GC 次数

    // R135 GC 模式字段——控制 registerTracked / collectCycle 的行为分流。
    // 默认 RefCountWithCycleGc 保持向后兼容。
    GcMode gcMode_ = GcMode::RefCountWithCycleGc;
};
