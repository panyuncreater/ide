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
    aliveSet_.insert(obj);
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
        const auto& captured = v.box_.asPtr<Value::ClosureData>()->capturedVars;
        for (const auto& kv : captured) {
            markValue(kv.second, marked);
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
        // 注意：清空后 obj 可能被级联释放，不能再访问 obj。
        switch (obj->type) {
        case ValueType::VAL_ARRAY: {
            static_cast<Value::ArrayData*>(obj)->elements.clear();
            break;
        }
        case ValueType::VAL_DICT: {
            static_cast<Value::DictData*>(obj)->entries.clear();
            break;
        }
        case ValueType::VAL_INSTANCE: {
            static_cast<Value::InstanceData*>(obj)->fields.clear();
            break;
        }
        default:
            break;
        }
        ++collectedCount;
    }

    // Phase 3: 清理跟踪结构（已释放的节点指针失效，新一轮 execute 重新注册）
    tracked_.clear();
    aliveSet_.clear();

    if (collectedCount > 0) {
        LOG_INFO("GcManager: 回收 " + std::to_string(collectedCount) + " 个循环引用孤岛", "GC");
    }
}

void GcManager::reset() {
    tracked_.clear();
    aliveSet_.clear();
}
