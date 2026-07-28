// ============================================================
// GcManager.cpp — 循环引用垃圾收集器实现（Bug2 fix）
// ============================================================

#include "interpreter/GcManager.h"
#include "common/Logger.h"
#include "interpreter/RefCounted.h"
#include "interpreter/Value.h"

// Bug2 fix: RefCounted 析构函数定义在此处（GcManager 完整定义可见）
// 仅当 gcTracked_ 为 true 时通知 GcManager，非跟踪对象零开销。
RefCounted::~RefCounted() {
    if (gcTracked_) {
        GcManager::instance().onDestroyed(this);
    }
}

void GcManager::registerTracked(RefCounted* obj) {
    if (!obj)
        return;
    // P0-1 fix: 全程持锁，保护 tracked_/aliveSet_/计数器/checkIncrementalGc 读写的状态。
    // checkIncrementalGc 可能触发 gcTriggerCallback_→collectCycle，collectCycle 会重入
    // 同一把锁（recursive_mutex 安全），且 collectCycle 内 GcOnly delete 也会重入 onDestroyed。
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // R135 GC 模式分流：RefCountOnly 模式跳过注册，纯引用计数管理生命周期。
    // 循环引用会泄漏（已知限制，用于基线对比与教学演示）。
    if (gcMode_ == GcMode::RefCountOnly) {
        return;
    }
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
    // BUG-003 fix: 增量分配计数 + 阈值触发，避免长时间运行函数中循环引用累积
    // 导致的内存峰值。
    // P0 fix: 不再直接调用 checkIncrementalGc()——容器构造函数体内调用
    // registerTracked 时新容器尚未加入 roots，若此时触发 GC 会被误回收。
    // 改为设置 pendingIncrementalGc_ 标志，由 Interpreter 在语句边界
    // （构造完成后）调用 checkPendingGc() 安全触发。
    ++allocationsSinceLastGc_;
    if (!gcInProgress_ && allocationsSinceLastGc_ >= gcAllocationThreshold_ && gcTriggerCallback_) {
        pendingIncrementalGc_ = true;
    }
}

void GcManager::checkIncrementalGc() {
    // BUG-003 fix: 增量 GC 触发逻辑
    //   - 阈值未达：直接返回（O(1) 检查）
    //   - 回调未注册：直接返回（无 Interpreter 场景，如单元测试直接构造 Value）
    //   - GC 进行中：直接返回（防止回调内 collectCycle → 析构 → registerTracked → 递归）
    //   - 触发：调用回调，回调内 Interpreter 收集 roots 并调用 collectCycle
    if (gcInProgress_)
        return;
    if (allocationsSinceLastGc_ < gcAllocationThreshold_)
        return;
    if (!gcTriggerCallback_)
        return;
    gcInProgress_ = true;
    try {
        gcTriggerCallback_();
    } catch (...) {
        // 回调内不应抛异常（collectCycle 不抛）；防御性捕获避免异常逃逸到构造函数
        gcInProgress_ = false;
        throw;
    }
    gcInProgress_ = false;
}

void GcManager::checkPendingGc() {
    // P0 fix: 供 Interpreter 在语句边界（容器构造完成后）调用。
    // 若 registerTracked 期间累积了待触发的增量 GC 请求，此处安全执行。
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (pendingIncrementalGc_) {
        pendingIncrementalGc_ = false;
        checkIncrementalGc();
    }
}

void GcManager::onDestroyed(RefCounted* obj) {
    // P0-1 fix: 加锁保护 aliveSet_.erase。任何线程上的 RefCounted 析构都会触及此路径，
    // 与 registerTracked/collectCycle 并发时 aliveSet_ 无锁修改会破坏哈希表内部结构。
    // collectCycle 持锁期间 delete 触发本方法时通过 recursive_mutex 安全重入。
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // 从 aliveSet_ 移除（tracked_ 中的悬垂指针在 collectCycle 中通过
    // aliveSet_.find 检查跳过，无需立即清理 vector）。
    // 仅用 obj 作为 key 做哈希查找/删除，不 dereference obj 内容。
    aliveSet_.erase(obj);
}

void GcManager::markValue(const Value& v, std::unordered_set<const void*>& marked) {
    // AUDIT-P1-ROUND53 fix: 原 markValue 对 ARRAY/DICT/INSTANCE/CLOSURE 四种堆类型
    // 纯递归遍历子元素。MiniLang 的 MAX_LOOP_ITERATIONS=10000000 允许 while 循环
    // 构建极深嵌套的线性容器链（如 100000 层 [[[[...]]]]）。这条链不是循环引用
    // （每层 refCount=1，全部从根可达），但 collectCycle Phase 1 mark 阶段仍需
    // 从 roots 遍历全部可达对象。markValue 递归深度等于嵌套深度，100000 层远超
    // Windows 默认 1MB 栈容量（每帧约 100-200 字节，~5000-10000 层即溢出）。
    // 改为显式 worklist 迭代式：栈深度恒定，消除栈溢出风险，且 worklist 连续
    // 内存访问对 cache 友好（同时解决 PERF-53-4 递归调用开销）。
    std::vector<const Value*> worklist;
    worklist.push_back(&v);

    while (!worklist.empty()) {
        const Value* cur = worklist.back();
        worklist.pop_back();

        // 仅指针类型的 Value 需 mark（int/float/bool/null 直接跳过）
        if (!cur->box_.isPointer())
            continue;
        const void* ptr = cur->box_.asPtr<void>();
        if (!marked.insert(ptr).second)
            continue; // 已标记，跳过避免循环

        // 将子元素压入 worklist 而非递归
        switch (cur->box_.asPtr<RefCounted>()->type) {
        case ValueType::VAL_ARRAY: {
            const auto& elements = cur->box_.asPtr<Value::ArrayData>()->elements;
            for (const auto& elem : elements) {
                worklist.push_back(&elem);
            }
            break;
        }
        case ValueType::VAL_DICT: {
            const auto& entries = cur->box_.asPtr<Value::DictData>()->entries;
            for (const auto& kv : entries) {
                worklist.push_back(&kv.second);
            }
            break;
        }
        case ValueType::VAL_INSTANCE: {
            const auto& fields = cur->box_.asPtr<Value::InstanceData>()->fields;
            for (const auto& kv : fields) {
                worklist.push_back(&kv.second);
            }
            break;
        }
        case ValueType::VAL_CLOSURE: {
            // 闭包的 capturedVars 可能持有容器引用
            const auto* closure = cur->box_.asPtr<Value::ClosureData>();
            const auto& captured = closure->capturedVars;
            for (const auto& kv : captured) {
                worklist.push_back(&kv.second);
            }
            // AUDIT-P2-CORRECT fix: 遍历 VM 闭包的 upvalues，标记已关闭 upvalue 的 value。
            // VMUpvalue::value 在 isClosed=true 时持有值（可能为容器引用），形成循环引用。
            // open 状态时值在栈上（GC roots），value 字段不持有有效值，markValue 安全跳过。
            // 原实现仅 mark capturedVars，遗漏 vmClosure->upvalues，可能导致仅通过
            // VM 闭包 upvalues 可达的循环容器被误判为不可达孤岛而误回收。
            if (closure->vmClosure) {
                for (const auto& upval : closure->vmClosure->upvalues) {
                    if (upval && upval->isClosed) {
                        worklist.push_back(&upval->value);
                    }
                }
            }
            // env 是 weak_ptr，不 mark（避免重新引入循环）
            break;
        }
        case ValueType::VAL_TUPLE: {
            // R98 元组与解构：元组元素可能持有可变容器形成间接环，需 mark 元素。
            const auto& elements = cur->box_.asPtr<Value::TupleData>()->elements;
            for (const auto& elem : elements) {
                worklist.push_back(&elem);
            }
            break;
        }
        case ValueType::VAL_ENUM_VARIANT: {
            // R99 枚举与 ADT：enum variant 的 fields 可能持有可变容器形成间接环，需 mark。
            const auto& fields = cur->box_.asPtr<Value::EnumVariantData>()->fields;
            for (const auto& f : fields) {
                worklist.push_back(&f);
            }
            break;
        }
        case ValueType::VAL_COROUTINE: {
            // AUDIT-R3 P1-6 fix: 协程持有 args（调用参数）/currentValueBox（最近
            // yield 值）/vmClosureBox（闭包值），均可能引用被跟踪容器。原实现
            // 未遍历——仅通过挂起协程可达的循环容器在 mark 阶段不可达，
            // sweep 会清空其元素，协程恢复后拿到被掉空的容器。
            const auto* cd = cur->box_.asPtr<Value::CoroutineData>();
            for (const auto& a : cd->args)
                worklist.push_back(&a);
            for (const auto& cv : cd->currentValueBox)
                worklist.push_back(&cv);
            for (const auto& cb : cd->vmClosureBox)
                worklist.push_back(&cb);
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
}

void GcManager::collectCycle(const std::vector<const void*>& roots) {
    // P0-1 fix: 全程持锁，互斥其他线程的 registerTracked/onDestroyed/reset/统计读取。
    // GcOnly 模式 delete 对象触发 ~RefCounted→onDestroyed 通过 recursive_mutex 重入安全。
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // R135 GC 模式分流：RefCountOnly 模式无 tracked 对象，直接返回。
    if (gcMode_ == GcMode::RefCountOnly) {
        return;
    }
    if (tracked_.empty())
        return;

    // R113 C 项：Phase 1: Mark - 从 roots 出发标记所有可达的容器节点
    currentPhase_ = GcPhase::Marking;
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
            case ValueType::VAL_TUPLE: {
                // R98 元组与解构：元组作为根时，标记其元素
                const auto& elements = static_cast<const Value::TupleData*>(rootPtr)->elements;
                for (const auto& elem : elements) {
                    markValue(elem, marked);
                }
                break;
            }
            case ValueType::VAL_ENUM_VARIANT: {
                // R99 枚举与 ADT：enum variant 作为根时，标记其 fields
                const auto& fields = static_cast<const Value::EnumVariantData*>(rootPtr)->fields;
                for (const auto& f : fields) {
                    markValue(f, marked);
                }
                break;
            }
            case ValueType::VAL_COROUTINE: {
                // AUDIT-R3 P1-6 fix: 协程作为根时，标记其持有的参数/yield 值/闭包值
                const auto* cd = static_cast<const Value::CoroutineData*>(rootPtr);
                for (const auto& a : cd->args)
                    markValue(a, marked);
                for (const auto& v : cd->currentValueBox)
                    markValue(v, marked);
                for (const auto& v : cd->vmClosureBox)
                    markValue(v, marked);
                break;
            }
            default:
                break;
            }
        }
    }
    // R113 C 项：Phase 1 结束，记录可达节点数
    lastMarkedCount_ = marked.size();

    // Phase 2: Sweep - 遍历 tracked 列表，对 aliveSet_ 中存在但 marked 中不存在的
    // 节点（不可达的循环孤岛）执行回收。
    //
    // R135 GC 模式分流：
    //   RefCountWithCycleGc (默认): 清空子元素打破循环，让 refCount 降至 0 自然释放
    //   GcOnly: 收集到 toDelete 列表，迭代结束后统一 delete
    //
    // 安全性说明：
    //   - aliveSet_.find(obj) 用 obj 作为 key 哈希查找，不 dereference obj 内容，
    //     即使 obj 已被释放，也是安全的（key 比较只比较指针值）。
    //   - RefCountWithCycleGc 清空 obj 的子元素会触发级联析构，其他 tracked_ 条目
    //     的析构会调用 onDestroyed 从 aliveSet_ 移除，故后续遍历到那些条目时
    //     aliveSet_.find 返回 not found → 安全跳过。
    //   - GcOnly 模式不在迭代中 delete（避免级联析构修改 aliveSet_ 破坏迭代不变量），
    //     而是收集到 toDelete，迭代结束后统一 delete。delete 触发 ~RefCounted →
    //     onDestroyed → aliveSet_.erase，安全。
    // R113 C 项：Phase 2 开始
    currentPhase_ = GcPhase::Sweeping;
    size_t collectedCount = 0;
    std::vector<RefCounted*> toDelete; // R135 GcOnly 模式：延迟 delete 列表
    for (RefCounted* obj : tracked_) {
        if (!obj)
            continue;
        // 跳过已释放的悬垂指针（不在 aliveSet_ 中）
        if (aliveSet_.find(obj) == aliveSet_.end())
            continue;
        // 跳过从根集可达的对象
        if (marked.find(obj) != marked.end())
            continue;

        // 不可达的循环孤岛处理：先清空子元素打破循环。
        // R135 GcOnly 修复：原实现 GcOnly 分支直接 push 到 toDelete 跳过清空，
        // 导致后续 delete obj 触发 ~ArrayData → ~vector<Value> → 释放元素（即 obj 自身）
        // → release() → refCount 降为 0 → 再次 delete obj，构成 use-after-free（SEH 0xc0000005）。
        // 修复：GcOnly 与 RefCountWithCycleGc 共享相同的 std::move 清空策略打破循环。
        // 区别仅在收尾：RefCountWithCycleGc 依赖 refCount 自然释放（不清 toDelete），
        // GcOnly 收集存活对象到 toDelete 在迭代结束后显式 delete（处理 refCount>1 的强制回收语义）。
        // AUDIT-BUG-I3 fix: 先 move 出子元素到局部变量再清空，防止自引用容器
        // （如 a.append(a)）在 clear() 期间级联析构重入 vector 析构器导致 use-after-free。
        // move 后 obj->elements/entries/fields 为空，级联析构 obj 时其析构器看到空容器，安全。
        // R135 GcOnly 路径：std::move 析构 tmp 时若触发级联 delete obj（仅当 refCount 来自循环引用），
        // onDestroyed 会从 aliveSet_ 移除 obj，后续通过 aliveSet_.find 检查跳过 toDelete.push_back
        // 避免 double-free。
        switch (obj->type) {
        case ValueType::VAL_ARRAY: {
            auto tmp = std::move(static_cast<Value::ArrayData*>(obj)->elements);
            (void)tmp; // tmp 析构时若级联释放 obj，obj->elements 已空
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
        case ValueType::VAL_TUPLE: {
            // R98 元组与解构：清空元组元素打破循环（元组元素可能持有容器形成间接环）
            auto tmp = std::move(static_cast<Value::TupleData*>(obj)->elements);
            (void)tmp;
            break;
        }
        case ValueType::VAL_ENUM_VARIANT: {
            // R99 枚举与 ADT：清空 fields 打破循环（fields 可能持有容器形成间接环）
            auto tmp = std::move(static_cast<Value::EnumVariantData*>(obj)->fields);
            (void)tmp;
            break;
        }
        default:
            break;
        }

        if (gcMode_ == GcMode::GcOnly) {
            // R135 GcOnly 模式：std::move 清空子元素后，若 obj 仅被循环引用持有，
            // 级联析构已 delete obj 并从 aliveSet_ 移除，跳过 toDelete.push_back 避免 double-free。
            // 若 obj 还被循环外引用（refCount > 0），aliveSet_ 仍含 obj，需 push 到 toDelete
            // 在迭代结束后显式 delete（GcOnly 语义：GC 主导生命周期，强制回收不可达对象）。
            if (aliveSet_.find(obj) != aliveSet_.end()) {
                toDelete.push_back(obj);
            }
        }
        ++collectedCount;
    }

    // R135 GcOnly 模式：迭代结束后统一 delete 不可达对象。
    // toDelete 仅包含 std::move 清空子元素后仍存活的对象（refCount > 0，
    // 即被循环外引用持有）。仅被循环引用持有的对象已在 std::move tmp 析构时
    // 通过 RefCounted release 机制自然 delete，不进入 toDelete。
    // delete 触发 ~RefCounted → onDestroyed → aliveSet_.erase + tracked_ 条目悬垂。
    // Phase 3 重建 tracked_ 时会过滤掉悬垂指针（aliveSet_ 中不存在）。
    if (gcMode_ == GcMode::GcOnly && !toDelete.empty()) {
        for (RefCounted* obj : toDelete) {
            // 安全性：obj 在 push 时确认仍在 aliveSet_ 中（未被级联析构释放）。
            // GcOnly 语义：GC 主导生命周期，强制回收不可达对象，无视 refCount > 0。
            // 调用方需保证无其他强引用（GcOnly 不与 COW 共享引用共存）。
            delete obj;
        }
    }
    // R113 C 项：Phase 2 结束，记录回收孤岛数
    lastCollectedCount_ = collectedCount;

    // Phase 3: 重建跟踪结构。
    // BUG-INTR-AUDIT-1 fix: 原实现无条件 tracked_.clear()，导致上一轮 marked 为可达
    // 而存活的循环引用容器（如 a.push(a)）从 tracked_ 中移除，且不会在下一轮 execute
    // 中重新 registerTracked（registerTracked 仅在容器构造时调用）。当这些容器后来
    // 变为不可达时，sweep 阶段不会检查它们（不在 tracked_ 中），无法打破循环，导致
    // 永久泄漏。修复：保留仍存活（在 aliveSet_ 中）且被标记为可达（在 marked 中）的
    // 容器条目，仅清除已释放的悬垂指针和已被回收的孤岛。
    // R113 C 项：Phase 3 开始
    currentPhase_ = GcPhase::Finalizing;
    std::vector<RefCounted*> survivors;
    survivors.reserve(tracked_.size());
    for (RefCounted* obj : tracked_) {
        if (!obj)
            continue;
        // 仅保留仍存活且本轮被标记为可达的容器（下一轮可能变为不可达，需要再次检查）
        if (aliveSet_.find(obj) != aliveSet_.end() && marked.find(obj) != marked.end()) {
            survivors.push_back(obj);
        }
    }
    tracked_ = std::move(survivors);
    // AUDIT-P1-CORRECT fix: 用 survivors 重建 aliveSet_，维持不变量
    // （aliveSet_ = tracked_ 中仍存活的对象集合）。原实现仅 clear 未重建，
    // 导致下一轮 collectCycle 的 Phase 2 将 survivors 误判为“已销毁”而跳过 sweep，
    // survivors 变为不可达循环孤岛时无法被回收 → 永久内存泄漏。
    aliveSet_.clear();
    // PERF: 预分配桶数，避免重建时频繁 rehash（tracked_ 通常 1000+ 元素）。
    aliveSet_.reserve(tracked_.size());
    for (RefCounted* obj : tracked_) {
        aliveSet_.insert(obj);
    }

    if (collectedCount > 0) {
        LOG_INFO("GcManager: 回收 " + std::to_string(collectedCount) + " 个循环引用孤岛", "GC");
    }
    // BUG-003 fix: 重置分配计数，下一次增量触发需累计到阈值
    allocationsSinceLastGc_ = 0;
    // R113 C 项：collectCycle 结束，回归 Idle 并累计 GC 次数
    ++totalGcCount_;
    currentPhase_ = GcPhase::Idle;
}

void GcManager::reset() {
    // P0-1 fix: 加锁保护，与 worker 线程的 registerTracked/onDestroyed/collectCycle 互斥。
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    tracked_.clear();
    aliveSet_.clear();
    // BUG-003 fix: 同步重置分配计数，避免 reset 后立即触发误增量 GC
    allocationsSinceLastGc_ = 0;
    gcInProgress_ = false;
    pendingIncrementalGc_ = false; // P0 fix: 同步清除延迟触发标志
    // R133 fix: 同步重置 GC 统计计数器。reset() 语义为"完全重置"，
    // 但原实现遗漏 totalGcCount_/lastMarkedCount_/lastCollectedCount_，
    // 导致测试隔离失败（前序测试累计的统计值污染后续测试断言）。
    // 例如 GcModes.AllModes_GcStatsAvailable 期望 reset 后 totalGcCount()==0，
    // GcModes.RefCountOnly_CollectCycleIsNoop 期望 lastCollectedCount()==0。
    totalGcCount_ = 0;
    lastMarkedCount_ = 0;
    lastCollectedCount_ = 0;
    currentPhase_ = GcPhase::Idle;
    // R157 fix: 清除 gcTriggerCallback_，防御性修复。
    // 根因：reset() 语义为"完全重置"，原实现遗漏 gcTriggerCallback_，
    // 导致测试隔离时虽重置 tracked_/统计，但悬垂的回调仍指向已析构的 Interpreter。
    // 后续后端执行触发 checkIncrementalGc 时调用悬垂回调 → UAF。
    // 配合 Interpreter 析构函数的清除（根因修复）双重保护。
    gcTriggerCallback_ = nullptr;
    gcTriggerOwner_ = nullptr; // AUDIT-R3 P2-9 fix: owner 令牌同步清空
}

// P0-1 fix: 以下访问器全部加锁，消除 UI 线程读取与 worker 线程写入的数据竞争。
size_t GcManager::trackedCount() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return tracked_.size();
}

void GcManager::setGcTriggerCallback(std::function<void()> cb, const void* owner) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    gcTriggerCallback_ = std::move(cb);
    // AUDIT-R3 P2-9 fix: 记录所有者令牌（回调为空时同步清空 owner）
    gcTriggerOwner_ = gcTriggerCallback_ ? owner : nullptr;
}

void GcManager::clearGcTriggerCallbackIfOwner(const void* owner) {
    // AUDIT-R3 P2-9 fix: 仅当回调仍属于 owner 时才清除——若已被后构造的
    // 实例覆盖，先构造者析构不得清掉存活实例的回调。
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (gcTriggerOwner_ == owner) {
        gcTriggerCallback_ = nullptr;
        gcTriggerOwner_ = nullptr;
    }
}

// P2-9 fix: CallbackSuppressor 实现。
// 构造时原子保存 gcTriggerCallback_ 并置空，析构时恢复。
// 嵌套场景：每个实例独立持有 saved_ 副本，析构按栈序恢复，
// 最内层析构恢复最外层 suppressor 置空前的回调（即 Interpreter 的回调）。
// AUDIT-R3 P2-9 fix: owner 令牌随回调同步保存/恢复，保持归属一致。
GcManager::CallbackSuppressor::CallbackSuppressor() {
    auto& gc = GcManager::instance();
    std::lock_guard<std::recursive_mutex> lock(gc.mutex_);
    saved_ = std::move(gc.gcTriggerCallback_);
    savedOwner_ = gc.gcTriggerOwner_;
    gc.gcTriggerCallback_ = nullptr;
    gc.gcTriggerOwner_ = nullptr;
}

GcManager::CallbackSuppressor::~CallbackSuppressor() {
    auto& gc = GcManager::instance();
    std::lock_guard<std::recursive_mutex> lock(gc.mutex_);
    gc.gcTriggerCallback_ = std::move(saved_);
    gc.gcTriggerOwner_ = savedOwner_;
}

size_t GcManager::allocationsSinceLastGc() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return allocationsSinceLastGc_;
}

void GcManager::setGcAllocationThreshold(size_t threshold) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    gcAllocationThreshold_ = threshold;
}

size_t GcManager::gcAllocationThreshold() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return gcAllocationThreshold_;
}

GcPhase GcManager::currentPhase() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return currentPhase_;
}

size_t GcManager::lastMarkedCount() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return lastMarkedCount_;
}

size_t GcManager::lastCollectedCount() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return lastCollectedCount_;
}

size_t GcManager::totalGcCount() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return totalGcCount_;
}

GcMode GcManager::gcMode() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return gcMode_;
}

void GcManager::setGcMode(GcMode mode) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    gcMode_ = mode;
}
