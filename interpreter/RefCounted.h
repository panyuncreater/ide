#pragma once

// ============================================================
// RefCounted.h — 侵入式引用计数基类（PERF-12 基础设施）
// ------------------------------------------------------------
// 为 Value 的堆类型数据提供引用计数，替代 shared_ptr。
// 配合 NaNBox 使用：Value 仅持有 8 字节 NaNBox，指针类型
// 存储原始 RefCounted* 指针，由 Value 的拷贝/移动/析构
// 手动管理引用计数。
//
// 设计要点：
//   - refCount 为 mutable atomic，支持 const 对象的 addRef/release
//   - type 为 const ValueType，构造时设定后不可变
//   - 拷贝构造重置 refCount=1（克隆出新对象，不继承原引用计数）
//   - 拷贝赋值删除（避免意外复制引用计数）
//   - 虚析构支持派生类正确释放
//
// 已知限制：环形容器泄漏（#9 文档化）
// ---------------------------------------------
// 侵入式引用计数 *不* 进行循环收集。当用户构造自引用容器时，
// 引用计数永远不会降到 0，导致内存泄漏直至进程退出。例如：
//
//   var a = [];
//   a.append(a);            // a → a 形成自环，refCount ≥ 2 永不归零
//
//   var d = {};
//   d.self = d;             // 字典自引用，同上
//
//   var x = [];
//   var y = [x];
//   x.append(y);            // x ⇄ y 双向循环
//
// 设计权衡：
//   - 循环收集（如 Python 的 gc 模块、JavaScript 的 mark-sweep）
//     需要独立追踪容器图并周期性扫描，开销和复杂度显著高于纯引用计数。
//   - 闭包场景已通过 weak_ptr<Environment> 显式打破 ClosureData →
//     Environment → ClosureData 的循环（见 Value.h 的 ClosureData），
//     因为该循环是语言语义固定模式，可静态消除。
//   - 用户层容器自引用是动态行为，无法静态消除；MiniLang 作为教学/
//     嵌入式语言，进程退出时由 OS 回收全部内存，可接受一次性泄漏。
//
// 缓解措施（语言层）：
//   - 解释器/VM 在每次程序执行结束后 resetState()，丢弃所有根引用，
//     对于 *非循环* 的容器图足以释放；循环容器则依赖 OS 退出回收。
//   - 长期运行宿主（如 IDE 持续执行用户代码）可通过限制单次执行的
//     内存上限或定期重启 VM 实例来控制累积泄漏。
//
// 缓解措施（未来扩展）：
//   - 若需支持长生命周期 + 用户可构造循环，可引入分代/增量 mark-sweep
//     作为 refcount 的补充：周期性扫描 ArrayData/DictData/InstanceData
//     节点，标记从根集可达的对象，释放不可达的循环孤岛。
//   - 实现位置：新增 GcManager 单例，持有所有 RefCounted 容器节点的
//     弱引用列表，VM 主循环每 N 条指令触发一次扫描。
// ============================================================

#include <atomic>
#include "interpreter/ValueTypes.h"  // ValueType 枚举

struct RefCounted {
    mutable std::atomic<int> refCount{1};
    const ValueType type;

    explicit RefCounted(ValueType t) : type(t) {}

    // 拷贝构造：新对象 refCount=1，不继承原对象的引用计数
    RefCounted(const RefCounted& other) : refCount(1), type(other.type) {}
    RefCounted(RefCounted&& other) noexcept : refCount(1), type(other.type) {}

    // 禁止拷贝/移动赋值：引用计数和 type 不可通过赋值改变
    RefCounted& operator=(const RefCounted&) = delete;
    RefCounted& operator=(RefCounted&&) = delete;

    virtual ~RefCounted() = default;

    /// 增加引用计数（relaxed 内存序， sufficient for refcounting）
    void addRef() const {
        refCount.fetch_add(1, std::memory_order_relaxed);
    }

    /// 减少引用计数，若降为 0 则自删除
    void release() const {
        if (refCount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete this;
        }
    }

    /// 检查是否独占引用（用于 COW detach 判断）
    bool isUnique() const {
        return refCount.load(std::memory_order_relaxed) == 1;
    }

    /// 获取当前引用计数（调试/诊断用）
    int useCount() const {
        return refCount.load(std::memory_order_relaxed);
    }
};
