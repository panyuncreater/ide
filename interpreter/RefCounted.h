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
