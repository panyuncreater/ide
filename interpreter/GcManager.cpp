// ============================================================
// GcManager.cpp — 循环引用垃圾收集器实现（Bug2 fix）
// ============================================================

#include "interpreter/GcManager.h"
#include "common/Logger.h"
#include "interpreter/RefCounted.h"
#include "interpreter/Value.h"

#include <atomic>

// SHUTDOWN-UAF fix: 进程退出阶段的静态析构顺序保护。
// GcManager 是 Meyer's Singleton（函数局部 static），其析构发生在 main() 返回后的
// 静态析构阶段。然而 thread_local Value（如 VM.cpp 的 nullSentinel、Value.h 的
// tlsCloned）以及文件作用域 static 容器可能在 GcManager 之后析构，触发
// ~RefCounted() → GcManager::instance().onDestroyed(this)——此时单例已销毁，
// 访问其 mutex_/slots_ 成员导致"读取访问权限冲突"（this 为已释放地址）。
//
// 修复：用 trivially-destructible 的 atomic<bool> 标记单例存活状态。
// std::atomic<bool> 无用户定义析构，不参与静态析构排序，进程退出时其存储
// 始终有效（与 int 全局变量相同语义）。
static std::atomic<bool> g_gcManagerAlive{false};

// SHUTDOWN-UAF fix: 构造时标记存活，析构时清除。
// 析构器在 main() 返回后的静态析构阶段执行（Meyer's Singleton 语义），
// 清除标志后，后续 RefCounted 析构将跳过 onDestroyed 调用。
GcManager::GcManager() {
    g_gcManagerAlive.store(true, std::memory_order_release);
}

GcManager::~GcManager() {
    g_gcManagerAlive.store(false, std::memory_order_release);
}

// Bug2 fix: RefCounted 析构函数定义在此处（GcManager 完整定义可见）
// 仅当 gcTracked_ 为 true 时通知 GcManager，非跟踪对象零开销。
RefCounted::~RefCounted() {
    // SHUTDOWN-UAF fix: 进程退出时 GcManager 单例可能已析构。
    // 此时 slots_/freeSlots_ 随进程消亡，无需（也不能）安全移除。
    // 检查 g_gcManagerAlive 避免对已销毁单例的 UAF 访问。
    if (gcTracked_ && g_gcManagerAlive.load(std::memory_order_acquire)) {
        GcManager::instance().onDestroyed(this);
    }
}

void GcManager::registerTracked(RefCounted* obj) {
    if (!obj)
        return;
    // P0-1 fix: 全程持锁，保护 slots_/freeSlots_/计数器/触发状态。
    // checkIncrementalGc 可能触发 gcTriggerCallback_→collectCycle，collectCycle 会重入
    // 同一把锁（recursive_mutex 安全），且 collectCycle 内 GcOnly delete 也会重入 onDestroyed。
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // R135 GC 模式分流：RefCountOnly 模式跳过登记，纯引用计数管理生命周期。
    // 循环引用会泄漏（已知限制，用于基线对比与教学演示）。
    if (gcMode_ == GcMode::RefCountOnly) {
        return;
    }
    // 2026-10-06 slot-index 重构：优先复用死亡槽位（freeSlots_ 命中时零堆分配、
    // 无哈希节点分配）；freeSlots_ 耗尽才扩容 slots_。
    // AUDIT-P2-CORRECT 的回滚问题随之消失：唯一可抛异常点（push_back 扩容）位于
    // 任何状态变更之前，抛出时登记表未被修改，不变量自然保持。
    size_t slot;
    if (!freeSlots_.empty()) {
        slot = freeSlots_.back();
        freeSlots_.pop_back();
        slots_[slot] = obj;
    } else {
        slots_.push_back(obj);
        slot = slots_.size() - 1;
    }
    obj->gcSlot_ = slot;
    obj->gcTracked_ = true;
    ++liveTrackedCount_;
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
    // PERF-GC: 无锁快速路径——本方法每条语句调用一次，pending==false 占绝对多数。
    // relaxed 读足够：false 直接返回无误读风险；true 时进入锁内复查（取锁的
    // acquire 语义保证看到 registerTracked 锁内写入的完整状态）。
    if (!pendingIncrementalGc_.load(std::memory_order_relaxed))
        return;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (pendingIncrementalGc_.load(std::memory_order_relaxed)) {
        pendingIncrementalGc_.store(false, std::memory_order_relaxed);
        checkIncrementalGc();
    }
}

void GcManager::onDestroyed(RefCounted* obj) {
    // P0-1 fix: 加锁保护槽位回收。任何线程上的 RefCounted 析构都会触及此路径，
    // 与 registerTracked/collectCycle 并发时无锁写 slots_/freeSlots_ 会破坏登记表
    // 内部结构。collectCycle 持锁期间 delete 触发本方法时通过 recursive_mutex 安全重入。
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // 指针身份卫兵：仅当槽位仍被本对象占用时才回收。覆盖三类陈旧场景
    // （等价于旧 aliveSet_.erase 对已注销指针的 no-op 语义，且天然 ABA 免疫）：
    //   1. 本轮 sweep/Phase 3 已回收本对象的槽位（sweep 清空子元素后 refCount
    //      归零晚于 Phase 3 压缩）；
    //   2. reset() 已清空登记表（gcSlot_ 越界）；
    //   3. 槽位已被新对象复用（slots_[gcSlot_] 指向他人，身份不符）。
    if (obj->gcSlot_ < slots_.size() && slots_[obj->gcSlot_] == obj) {
        slots_[obj->gcSlot_] = nullptr;
        freeSlots_.push_back(obj->gcSlot_);
        --liveTrackedCount_;
    }
}

void GcManager::markValue(const Value& v, std::unordered_set<const void*>& marked,
                          std::vector<const Value*>& worklist) {
    // AUDIT-P1-ROUND53 fix: 原 markValue 对 ARRAY/DICT/INSTANCE/CLOSURE 四种堆类型
    // 纯递归遍历子元素。MiniLang 的 MAX_LOOP_ITERATIONS=10000000 允许 while 循环
    // 构建极深嵌套的线性容器链（如 100000 层 [[[[...]]]]）。这条链不是循环引用
    // （每层 refCount=1，全部从根可达），但 collectCycle Phase 1 mark 阶段仍需
    // 从 roots 遍历全部可达对象。markValue 递归深度等于嵌套深度，100000 层远超
    // Windows 默认 1MB 栈容量（每帧约 100-200 字节，~5000-10000 层即溢出）。
    // 改为显式 worklist 迭代式：栈深度恒定，消除栈溢出风险，且 worklist 连续
    // 内存访问对 cache 友好（同时解决 PERF-53-4 递归调用开销）。
    // PERF-GC: worklist 由调用方持有（整个 collectCycle 仅 1 次 vector 分配），
    // 原实现每个根元素各建一个 worklist，在 stop-the-world 暂停内产生 R 次分配。
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
    // 快速退出：无存活被跟踪对象时无事可做（登记表可能仍保有墓碑槽位容量）。
    if (liveTrackedCount_ == 0)
        return;

    // R113 C 项：Phase 1: Mark - 从 roots 出发标记所有可达的容器节点
    currentPhase_ = GcPhase::Marking;
    std::unordered_set<const void*> marked;
    marked.reserve(liveTrackedCount_ * 2);
    // PERF-GC: 整个 mark 阶段共享一个 worklist（1 次分配），传引用给所有 markValue 调用。
    std::vector<const Value*> markWorklist;
    for (const void* rootPtr : roots) {
        if (marked.insert(rootPtr).second) {
            // 根据 type 遍历根指针的子元素
            const RefCounted* rc = static_cast<const RefCounted*>(rootPtr);
            switch (rc->type) {
            case ValueType::VAL_ARRAY: {
                const auto& elements = static_cast<const Value::ArrayData*>(rootPtr)->elements;
                for (const auto& elem : elements) {
                    markValue(elem, marked, markWorklist);
                }
                break;
            }
            case ValueType::VAL_DICT: {
                const auto& entries = static_cast<const Value::DictData*>(rootPtr)->entries;
                for (const auto& kv : entries) {
                    markValue(kv.second, marked, markWorklist);
                }
                break;
            }
            case ValueType::VAL_INSTANCE: {
                const auto& fields = static_cast<const Value::InstanceData*>(rootPtr)->fields;
                for (const auto& kv : fields) {
                    markValue(kv.second, marked, markWorklist);
                }
                break;
            }
            case ValueType::VAL_CLOSURE: {
                const auto& captured = static_cast<const Value::ClosureData*>(rootPtr)->capturedVars;
                for (const auto& kv : captured) {
                    markValue(kv.second, marked, markWorklist);
                }
                break;
            }
            case ValueType::VAL_TUPLE: {
                // R98 元组与解构：元组作为根时，标记其元素
                const auto& elements = static_cast<const Value::TupleData*>(rootPtr)->elements;
                for (const auto& elem : elements) {
                    markValue(elem, marked, markWorklist);
                }
                break;
            }
            case ValueType::VAL_ENUM_VARIANT: {
                // R99 枚举与 ADT：enum variant 作为根时，标记其 fields
                const auto& fields = static_cast<const Value::EnumVariantData*>(rootPtr)->fields;
                for (const auto& f : fields) {
                    markValue(f, marked, markWorklist);
                }
                break;
            }
            case ValueType::VAL_COROUTINE: {
                // AUDIT-R3 P1-6 fix: 协程作为根时，标记其持有的参数/yield 值/闭包值
                const auto* cd = static_cast<const Value::CoroutineData*>(rootPtr);
                for (const auto& a : cd->args)
                    markValue(a, marked, markWorklist);
                for (const auto& v : cd->currentValueBox)
                    markValue(v, marked, markWorklist);
                for (const auto& v : cd->vmClosureBox)
                    markValue(v, marked, markWorklist);
                break;
            }
            default:
                break;
            }
        }
    }
    // R113 C 项：Phase 1 结束，记录可达节点数
    lastMarkedCount_ = marked.size();

    // Phase 2: Sweep - 遍历 slots_ 登记表，对槽位被占用（非 null 墓碑）但 marked
    // 中不存在的对象（不可达的循环孤岛）执行回收。
    //
    // 2026-10-06 slot-index 重构：原实现逐对象 aliveSet_.find（哈希查找）区分
    // 存活与已析构；现登记表本身即真相源——非空槽位在锁内必为存活对象，
    // nullptr 墓碑直接跳过。迭代安全性与旧设计等价并更强：
    //   - 清空孤岛子元素触发的级联析构（~RefCounted→onDestroyed）只会把
    //     其他槽位置空 + push freeSlots_，从不改变 slots_.size()，
    //     因此按下标迭代 slots_ 不受迭代器/扩容失效影响（旧实现依赖
    //     "级联析构不向 tracked_ push_back"这一偶然性质保证 range-for 安全）。
    //
    // R135 GC 模式分流：
    //   RefCountWithCycleGc (默认): 清空子元素打破循环，让 refCount 降至 0 自然释放
    //   GcOnly: 收集到 toDelete 列表，迭代结束后统一 delete
    //
    // 安全性说明：
    //   - 非 null 槽位即存活（全局锁内不变量），无需哈希查找即可安全 dereference。
    //   - RefCountWithCycleGc 清空 obj 的子元素会触发级联析构，其他槽位随之置空，
    //     故后续迭代到那些槽位时 nullptr → 安全跳过。
    //   - GcOnly 模式不在迭代中 delete（避免级联析构破坏迭代不变量），
    //     而是收集到 toDelete，迭代结束后统一 delete。delete 触发 ~RefCounted →
    //     onDestroyed → 槽位回收，安全。
    // R113 C 项：Phase 2 开始
    currentPhase_ = GcPhase::Sweeping;
    size_t collectedCount = 0;
    std::vector<RefCounted*> toDelete; // R135 GcOnly 模式：延迟 delete 列表
    for (size_t i = 0; i < slots_.size(); ++i) {
        RefCounted* obj = slots_[i];
        if (!obj)
            continue; // 已析构墓碑（旧 aliveSet_.find 失败的等价路径）
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
        // onDestroyed 会回收 obj 的槽位（置空），后续通过槽位检查跳过 toDelete.push_back
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
            // 级联析构已 delete obj 并回收其槽位（置空），跳过 toDelete.push_back 避免 double-free。
            // 若 obj 还被循环外引用（refCount > 0），槽位仍被 obj 占用，需 push 到 toDelete
            // 在迭代结束后显式 delete（GcOnly 语义：GC 主导生命周期，强制回收不可达对象）。
            if (slots_[i] == obj) {
                toDelete.push_back(obj);
            }
        }
        ++collectedCount;
    }

    // R135 GcOnly 模式：迭代结束后统一 delete 不可达对象。
    // toDelete 仅包含 std::move 清空子元素后仍存活的对象（refCount > 0，
    // 即被循环外引用持有）。仅被循环引用持有的对象已在 std::move tmp 析构时
    // 通过 RefCounted release 机制自然 delete，不进入 toDelete。
    // delete 触发 ~RefCounted → onDestroyed → 槽位回收（置空 + 进 freeSlots_）。
    // Phase 3 压缩登记表时会收缩掉死亡槽位。
    if (gcMode_ == GcMode::GcOnly && !toDelete.empty()) {
        for (RefCounted* obj : toDelete) {
            // 安全性：obj 在 push 时确认槽位仍被其占用（未被级联析构释放）。
            // 且所有 toDelete 条目的子元素已在 sweep 中 move 清空，delete 不会级联
            // 析构其他条目（与旧设计相同的 R135 保证），无悬挂风险。
            // GcOnly 语义：GC 主导生命周期，强制回收不可达对象，无视 refCount > 0。
            // 调用方需保证无其他强引用（GcOnly 不与 COW 共享引用共存）。
            delete obj;
        }
    }
    // R113 C 项：Phase 2 结束，记录回收孤岛数
    lastCollectedCount_ = collectedCount;

    // Phase 3: 压缩登记表（原地）。
    // BUG-INTR-AUDIT-1 fix（语义保留）：原实现无条件 tracked_.clear()，导致上一轮
    // marked 为可达而存活的循环引用容器（如 a.push(a)）从跟踪结构中移除，且不会在
    // 下一轮 execute 中重新 registerTracked（registerTracked 仅在容器构造时调用）。
    // 当这些容器后来变为不可达时，sweep 阶段不会检查它们，无法打破循环，导致永久泄漏。
    // 修复语义：仅保留本轮被标记为可达的存活容器，清除已析构墓碑与已回收的孤岛。
    // 2026-10-06 slot-index 重构：以单趟原地压缩取代旧「survivors 重建 tracked_ +
    // 清空重灌 aliveSet_」的双结构哈希风暴——存活对象前移并回写 gcSlot_，死亡槽位
    // 置空并回收进 freeSlots_（仅回收本轮仍被占用的槽位；早已是墓碑的槽位下标
    // 已在 freeSlots_ 中，不重复入栈，避免复用冲突）。
    // R113 C 项：Phase 3 开始
    currentPhase_ = GcPhase::Finalizing;
    size_t w = 0;
    for (size_t i = 0; i < slots_.size(); ++i) {
        RefCounted* obj = slots_[i];
        if (!obj)
            continue; // 墓碑槽位：压缩时直接收缩掉
        // 仅保留本轮被标记为可达的容器（下一轮可能变为不可达，需要再次检查）
        if (marked.find(obj) != marked.end()) {
            if (w != i) {
                slots_[w] = obj;
                obj->gcSlot_ = w; // 回写新下标（对象存活且持锁，安全）
            }
            ++w;
        }
    }
    // 尾部死亡槽位：置空 + 回收（跳过已是墓碑的槽位，其下标已在 freeSlots_ 中）
    for (size_t i = w; i < slots_.size(); ++i) {
        if (slots_[i]) {
            slots_[i] = nullptr;
            freeSlots_.push_back(i);
        }
    }
    liveTrackedCount_ = w;
    // 容量策略：slots_.size() 保持历史峰值不缩容，死亡槽位经 freeSlots_ 复用，
    // 与旧 tracked_ 的高水位行为一致（旧 Phase 3 由 survivors 重置低水位，但
    // 下次增长重新扩容；新设计以 O(1) 复用取代重复扩容）。

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
    slots_.clear();
    freeSlots_.clear();
    liveTrackedCount_ = 0;
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
    // 导致测试隔离时虽重置 slots_ 登记表/统计，但悬垂的回调仍指向已析构的 Interpreter。
    // 后续后端执行触发 checkIncrementalGc 时调用悬垂回调 → UAF。
    // 配合 Interpreter 析构函数的清除（根因修复）双重保护。
    gcTriggerCallback_ = nullptr;
    gcTriggerOwner_ = nullptr; // AUDIT-R3 P2-9 fix: owner 令牌同步清空
}

// P0-1 fix: 以下访问器全部加锁，消除 UI 线程读取与 worker 线程写入的数据竞争。
// 2026-10-06 slot-index 重构：原返回 tracked_.size()（含已析构未压缩的墓碑条目），
// 现返回 liveTrackedCount_（精确存活数）——GUI 面板/测试读数更准确，且 O(1)。
size_t GcManager::trackedCount() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return liveTrackedCount_;
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

// PERF-GC: 统计字段已迁移到 atomic（GcManager.h），只读访问无锁化（relaxed 读）；
// 写入仍在锁内（registerTracked/collectCycle/reset），锁的 release 语义 + atomic
// 保证 GUI 线程读到的是完整写入后的值。trackedCount() 仍读 vector，保持加锁。
size_t GcManager::allocationsSinceLastGc() const {
    return allocationsSinceLastGc_.load(std::memory_order_relaxed);
}

void GcManager::setGcAllocationThreshold(size_t threshold) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    gcAllocationThreshold_.store(threshold, std::memory_order_relaxed);
}

size_t GcManager::gcAllocationThreshold() const {
    return gcAllocationThreshold_.load(std::memory_order_relaxed);
}

GcPhase GcManager::currentPhase() const {
    return currentPhase_.load(std::memory_order_relaxed);
}

size_t GcManager::lastMarkedCount() const {
    return lastMarkedCount_.load(std::memory_order_relaxed);
}

size_t GcManager::lastCollectedCount() const {
    return lastCollectedCount_.load(std::memory_order_relaxed);
}

size_t GcManager::totalGcCount() const {
    return totalGcCount_.load(std::memory_order_relaxed);
}

GcMode GcManager::gcMode() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return gcMode_;
}

void GcManager::setGcMode(GcMode mode) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    gcMode_ = mode;
}
