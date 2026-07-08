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
// 触发时机：
//   - Interpreter::execute() 在 resetState 之后、runStatements 之前
//     调用 collectCycle(空根集)。此时上一轮残留的循环容器 refCount>0
//     仍 aliveSet_，本轮新建容器尚未注册，安全。
//
// 性能权衡：
//   - mark-sweep 的开销是 O(节点数 + 边数)，仅在 execute 起点触发
//   - RefCounted 析构增加一个 bool 检查（非跟踪对象零开销）
//   - tracked_ 用 vector，aliveSet_ 用 unordered_set，注册/注销均 O(1)
// ============================================================

#include "interpreter/ValueTypes.h" // ValueType
#include <unordered_set>
#include <vector>

struct RefCounted;
struct Value;

class GcManager {
public:
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

private:
    GcManager() = default;
    ~GcManager() = default;
    GcManager(const GcManager&) = delete;
    GcManager& operator=(const GcManager&) = delete;

    // 从单个 Value 出发递归 mark 所有可达容器
    void markValue(const Value& v, std::unordered_set<const void*>& marked);

    // tracked 节点列表（弱引用，对象由 refCount 管理生命周期）
    // collectCycle 期间可能有 nullptr 墓碑（对象已析构但还未 compact）
    std::vector<RefCounted*> tracked_;

    // 当前存活的 tracked 对象集合（析构时移除）
    // 用于在 collectCycle 迭代 tracked_ 时区分存活对象与悬垂指针
    std::unordered_set<RefCounted*> aliveSet_;
};
