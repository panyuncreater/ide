// ============================================================
// GcManager.cpp — 循环引用垃圾收集器实现（Bug2 fix）
// ============================================================

#include "interpreter/GcManager.h"
#include "interpreter/RefCounted.h"
#include "interpreter/Value.h"
#include "Logger.h"

// Bug2 fix: RefCounted 析构函数定义在此处（GcManager 完整定义可见）
// 仅当 gcTracked_ 为 true 时通知 GcManager，非跟踪对象零开销。
RefCounted::~RefCounted() {
    if (gcTracked_) {
        GcManager::instance().onDestroyed(this);
    }
}

void GcManager::registerTracked(RefCounted* obj) {
    if (!obj) return;
    tracked_.push_back(obj);
    // AUDIT-P2-CORRECT fix: aliveSet_.insert 可能抛 bad_alloc（rehash），
    // 此时 tracked_ 已含 obj 但 aliveSet_ 不含，破坏不变量
    // （aliveSet_ = tracked_ 中仍存活的对象集合）。
    // 后果：collectCycle Phase 2 将 obj 误判为"已销毁"跳过 sweep，
    // 循环引用容器永久泄漏。用 try/catch 回滚 push_back 维持原子性。
    try {
        aliveSet_.insert(obj);
    } catch (...) {
        tracked_.pop_back();
        throw;
    }
    obj->gcTracked_ = true;
}

void GcManager::onDestroyed(RefCounted* obj) {
    // 从 aliveSet_ 移除（tracked_ 中的悬垂指针在 collectCycle 中通过
    // aliveSet_.find 检查跳过，无需立即清理 vector）。
    // 仅用 obj 作为 key 做哈希查找/删除，不 dereference obj 内容。
    aliveSet_.erase(obj);
}

void GcManager::markValue(const Value& v, std::unordered_set<const void*>& marked) {
    // 仅指针类型的 Value 需 mark（int/float/bool/null 直接返回）
    if (!v.box_.isPointer()) return;
    const void* ptr = v.box_.asPtr<void>();
    if (!marked.insert(ptr).second) return;  // 已标记，跳过避免循环

    // 递归 mark 子元素
    switch (v.box_.asPtr<RefCounted>()->type) {
    case ValueType::VAL_ARRAY: {
        const auto& elements = v.box_.asPtr<Value::ArrayData>()->elements;
        for (const auto& elem : elements) {
            markValue(elem, marked);
        }
        break;
    }
    case ValueType::VAL_DICT: {
        const auto& entries = v.box_.asPtr<Value::DictData>()->entries;
        for (const auto& kv : entries) {
            markValue(kv.second, marked);
        }
        break;
    }
    case ValueType::VAL_INSTANCE: {
        const auto& fields = v.box_.asPtr<Value::InstanceData>()->fields;
        for (const auto& kv : fields) {
            markValue(kv.second, marked);
        }
        break;
    }
    case ValueType::VAL_CLOSURE: {
        // 闭包的 capturedVars 可能持有容器引用
        const auto* closure = v.box_.asPtr<Value::ClosureData>();
        const auto& captured = closure->capturedVars;
        for (const auto& kv : captured) {
            markValue(kv.second, marked);
        }
        // AUDIT-P2-CORRECT fix: 遍历 VM 闭包的 upvalues，标记已关闭 upvalue 的 value。
        // VMUpvalue::value 在 isClosed=true 时持有值（可能为容器引用），形成循环引用。
        // open 状态时值在栈上（GC roots），value 字段不持有有效值，markValue 安全跳过。
        // 原实现仅 mark capturedVars，遗漏 vmClosure->upvalues，可能导致仅通过
        // VM 闭包 upvalues 可达的循环容器被误判为不可达孤岛而误回收。
        if (closure->vmClosure) {
            for (const auto& upval : closure->vmClosure->upvalues) {
                if (upval && upval->isClosed) {
                    markValue(upval->value, marked);
                }
            }
        }
        // env 是 weak_ptr，不 mark（避免重新引入循环）
        break;
    }
    case ValueType::VAL_STRING:
    case ValueType::VAL_INT:
        // 叶子节点，无子引用
        break;
    default:
        break;
    }
}

void GcManager::collectCycle(const std::vector<const void*>& roots) {
    if (tracked_.empty()) return;

    // Phase 1: Mark - 从 roots 出发标记所有可达的容器节点
    std::unordered_set<const void*> marked;
    marked.reserve(tracked_.size() * 2);
    for (const void* rootPtr : roots) {
        if (marked.insert(rootPtr).second) {
            // 根据 type 遍历根指针的子元素
            const RefCounted* rc = static_cast<const RefCounted*>(rootPtr);
            switch (rc->type) {
            case ValueType::VAL_ARRAY: {
                const auto& elements = static_cast<const Value::ArrayData*>(rootPtr)->elements;
                for (const auto& elem : elements) {
                    markValue(elem, marked);
                }
                break;
            }
            case ValueType::VAL_DICT: {
                const auto& entries = static_cast<const Value::DictData*>(rootPtr)->entries;
                for (const auto& kv : entries) {
                    markValue(kv.second, marked);
                }
                break;
            }
            case ValueType::VAL_INSTANCE: {
                const auto& fields = static_cast<const Value::InstanceData*>(rootPtr)->fields;
                for (const auto& kv : fields) {
                    markValue(kv.second, marked);
                }
                break;
            }
            case ValueType::VAL_CLOSURE: {
                const auto& captured = static_cast<const Value::ClosureData*>(rootPtr)->capturedVars;
                for (const auto& kv : captured) {
                    markValue(kv.second, marked);
                }
                break;
            }
            default:
                break;
            }
        }
    }

    // Phase 2: Sweep - 遍历 tracked 列表，对 aliveSet_ 中存在但 marked 中不存在的
    // 节点（不可达的循环孤岛）清空子元素打破循环。
    //
    // 安全性说明：
    //   - aliveSet_.find(obj) 用 obj 作为 key 哈希查找，不 dereference obj 内容，
    //     即使 obj 已被释放，也是安全的（key 比较只比较指针值）。
    //   - 清空 obj 的子元素会触发级联析构，其他 tracked_ 条目的析构会调用
    //     onDestroyed 从 aliveSet_ 移除，故后续遍历到那些条目时
    //     aliveSet_.find 返回 not found → 安全跳过。
    size_t collectedCount = 0;
    for (RefCounted* obj : tracked_) {
        if (!obj) continue;
        // 跳过已释放的悬垂指针（不在 aliveSet_ 中）
        if (aliveSet_.find(obj) == aliveSet_.end()) continue;
        // 跳过从根集可达的对象
        if (marked.find(obj) != marked.end()) continue;

        // 不可达的循环孤岛：清空子元素打破循环，让 refCount 降至 0 自然释放。
        // AUDIT-BUG-I3 fix: 先 move 出子元素到局部变量再清空，防止自引用容器
        // （如 a.append(a)）在 clear() 期间级联析构重入 vector 析构器导致 use-after-free。
        // move 后 obj->elements/entries/fields 为空，级联析构 obj 时其析构器看到空容器，安全。
        switch (obj->type) {
        case ValueType::VAL_ARRAY: {
            auto tmp = std::move(static_cast<Value::ArrayData*>(obj)->elements);
            (void)tmp;  // tmp 析构时若级联释放 obj，obj->elements 已空
            break;
        }
        case ValueType::VAL_DICT: {
            auto tmp = std::move(static_cast<Value::DictData*>(obj)->entries);
            (void)tmp;
            break;
        }
        case ValueType::VAL_INSTANCE: {
            auto tmp = std::move(static_cast<Value::InstanceData*>(obj)->fields);
            (void)tmp;
            break;
        }
        default:
            break;
        }
        ++collectedCount;
    }

    // Phase 3: 重建跟踪结构。
    // BUG-INTR-AUDIT-1 fix: 原实现无条件 tracked_.clear()，导致上一轮 marked 为可达
    // 而存活的循环引用容器（如 a.push(a)）从 tracked_ 中移除，且不会在下一轮 execute
    // 中重新 registerTracked（registerTracked 仅在容器构造时调用）。当这些容器后来
    // 变为不可达时，sweep 阶段不会检查它们（不在 tracked_ 中），无法打破循环，导致
    // 永久泄漏。修复：保留仍存活（在 aliveSet_ 中）且被标记为可达（在 marked 中）的
    // 容器条目，仅清除已释放的悬垂指针和已被回收的孤岛。
    std::vector<RefCounted*> survivors;
    survivors.reserve(tracked_.size());
    for (RefCounted* obj : tracked_) {
        if (!obj) continue;
        // 仅保留仍存活且本轮被标记为可达的容器（下一轮可能变为不可达，需要再次检查）
        if (aliveSet_.find(obj) != aliveSet_.end() &&
            marked.find(obj) != marked.end()) {
            survivors.push_back(obj);
        }
    }
    tracked_ = std::move(survivors);
    // AUDIT-P1-CORRECT fix: 用 survivors 重建 aliveSet_，维持不变量
    // （aliveSet_ = tracked_ 中仍存活的对象集合）。原实现仅 clear 未重建，
    // 导致下一轮 collectCycle 的 Phase 2 将 survivors 误判为"已销毁"而跳过 sweep，
    // survivors 变为不可达循环孤岛时无法被回收 → 永久内存泄漏。
    aliveSet_.clear();
    for (RefCounted* obj : tracked_) {
        aliveSet_.insert(obj);
    }

    if (collectedCount > 0) {
        LOG_INFO("GcManager: 回收 " + std::to_string(collectedCount) + " 个循环引用孤岛", "GC");
    }
}

void GcManager::reset() {
    tracked_.clear();
    aliveSet_.clear();
}
