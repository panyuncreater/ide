#pragma once

// ============================================================
// Value.h — 运行时值结构体（PERF-12: NaN-boxing + intrusive refcount）
// ------------------------------------------------------------
// 将 Value 从 std::variant<..., shared_ptr<XData>, ...>（24 字节）
// 迁移到 NaNBox（8 字节）+ 侵入式引用计数。
//
// 核心改进：
//   - sizeof(Value) = 8 字节（原 24 字节），VM 栈缓存局部性提升 3 倍
//   - 标量拷贝（int/float/bool/null）：零原子操作，仅拷贝 8 字节
//   - 堆类型拷贝（string/array/dict/instance/closure）：一次原子递增
//   - int48 范围内的整数内联存储，超范围自动装箱（BoxedIntData）
//   - COW 语义保留：写入前检查 refCount==1，否则深拷贝
//   - 公开 API 完全向后兼容（调用方无需修改）
//
// 存储布局：
//   NaNBox box_  (8 字节)
//     ├─ 标量类型：直接编码（int48/float64/bool/null）
//     └─ 堆类型：  PTR_TAG_BASE | 48位 RefCounted* 指针
//
// 引用计数：
//   - 堆数据类型继承 RefCounted（atomic refCount + ValueType type）
//   - Value 的拷贝/移动/析构手动管理 addRef/release
//   - COW detach 通过 isUnique() 判断
//
// 已知限制：环形容器泄漏（见 RefCounted.h 的 "#9 文档化" 章节）
//   - 自引用容器（如 `a=[]; a.append(a)` 或 `d={}; d.self=d`）
//     会形成引用计数永不归零的循环，导致内存泄漏直至进程退出。
//   - 闭包场景的循环已通过 ClosureData.env = weak_ptr<Environment>
//     静态打破；用户层容器循环依赖 OS 退出回收，视为可接受。
// ============================================================

#include "interpreter/ValueTypes.h"
#include "interpreter/RefCounted.h"   // PERF-12: 侵入式引用计数基类
#include "interpreter/NaNBox.h"       // PERF-12: 8字节 NaN-boxing 编码
#include "interpreter/NumericUtils.h" // 共享溢出检查（B6 fix）
#include "interpreter/GcManager.h"    // Bug2 fix: 循环引用 GC
#include "common/RuntimeLimits.h"

#include <string>
#include <unordered_map>
#include <sstream>
#include <cstdio>
#include <charconv>   // Perf-Finding2: VAL_INT toString 用 to_chars 替代 std::to_string（避免 locale 查询）
#include <array>
#include <vector>
#include <memory>
#include <cstdint>
#include <cmath>
#include <cassert>
#include <cstdlib>     // AUDIT-NANBOX fix: std::abort — Release 构建中 assert 被剥离，类型不匹配时静默返回垃圾值（如指针间类型混淆 reinterpret_cast 错误类型），改为运行时 abort 与 NaNBox::asXxx 风格一致
#include <unordered_set>
#include <optional>  // perf1 fix: StringData 缓存 codepointCount
#include "common/Utf8Utils.h"  // perf1 fix: Value::codepointCount() 委托 Utf8::codepointCount

// ============================================================
// Value 运行时值结构体
// ============================================================

struct Value {
    // Bug2 fix: GcManager mark-sweep 需要访问 private 嵌套类型和 box_ 进行容器图遍历
    friend class GcManager;
private:
    // ---- 堆类型数据载体（继承 RefCounted， intrusive 引用计数）----
    struct StringData : RefCounted {
        std::string value;
        // perf1 fix: 缓存 UTF-8 码位数，避免 len()/substr() 在循环中 O(n²) 重复扫描。
        // mutable 允许 const stringVal() 路径的 codepointCount() 填充缓存。
        // 非 const stringVal() 返回可变引用时 reset() 使缓存失效（字符串可能被修改）。
        mutable std::optional<int64_t> cachedCodepointCount;
        StringData() : RefCounted(ValueType::VAL_STRING) {}
        explicit StringData(std::string v) : RefCounted(ValueType::VAL_STRING), value(std::move(v)) {}
    };

    struct ArrayData : RefCounted {
        std::vector<Value> elements;
        ArrayData() : RefCounted(ValueType::VAL_ARRAY) {
            // Bug2 fix: 注册到 GcManager 跟踪循环引用
            GcManager::instance().registerTracked(this);
        }
        explicit ArrayData(std::vector<Value> v) : RefCounted(ValueType::VAL_ARRAY), elements(std::move(v)) {
            GcManager::instance().registerTracked(this);
        }
    };

    struct DictData : RefCounted {
        std::unordered_map<std::string, Value> entries;
        DictData() : RefCounted(ValueType::VAL_DICT) {
            GcManager::instance().registerTracked(this);
        }
    };

    struct InstanceData : RefCounted {
        std::string className;
        std::unordered_map<std::string, Value> fields;
        InstanceData() : RefCounted(ValueType::VAL_INSTANCE) {
            GcManager::instance().registerTracked(this);
        }
        explicit InstanceData(const std::string& cn) : RefCounted(ValueType::VAL_INSTANCE), className(cn) {
            GcManager::instance().registerTracked(this);
        }
    };

    struct ClosureData : RefCounted {
        std::string name;
        std::weak_ptr<Environment> env;  // V4 fix: weak_ptr 打破闭包→环境→闭包的循环引用
        std::vector<std::string> params;
        std::unordered_map<std::string, Value> capturedVars;
        // A3 fix: shared_ptr 所有权，避免 AST 重建后 funRegistry_/methods 持有的裸指针悬垂。
        std::shared_ptr<FunDecl> body;
        std::shared_ptr<VMClosureData> vmClosure; // VM-05/06: VM 闭包数据

        ClosureData() : RefCounted(ValueType::VAL_CLOSURE) {}
        ClosureData(const std::string& n, std::shared_ptr<Environment> e,
                    const std::vector<std::string>& p, std::shared_ptr<FunDecl> b)
            : RefCounted(ValueType::VAL_CLOSURE), name(n), env(e), params(p), body(std::move(b)) {}
    };

    // PERF-12: 超出 int48 范围的 int64 装箱到堆上
    struct BoxedIntData : RefCounted {
        int64_t value;
        explicit BoxedIntData(int64_t v) : RefCounted(ValueType::VAL_INT), value(v) {}
    };

    // ---- 唯一存储成员：8 字节 NaNBox ----
    NaNBox box_;

    // ---- COW detach — 写入前确保独占所有权 ----
    template<typename T>
    T* ensureUnique() {
        T* ptr = box_.asPtr<T>();
        if (!ptr->isUnique()) {
            // 引用计数 > 1：深拷贝一份新数据
            // P0 fix: 用 unique_ptr 包裹 cloned，防止 new T(*ptr) 抛 bad_alloc 时
            // 部分已克隆的子树泄漏（如 ArrayData/DictData/InstanceData 的 cloneImpl
            // 递归克隆嵌套 Value，中途抛异常会泄漏已分配的子对象）。
            // 异常安全：unique_ptr 析构时 delete cloned（释放已分配对象）；
            //           ptr->release() 未执行，旧引用计数不变，仍由调用方持有。
            auto cloned = std::make_unique<T>(*ptr);  // 拷贝构造（RefCounted 拷贝 ctor 重置 refCount=1）
            ptr->release();                          // 释放旧引用
            T* raw = cloned.release();
            box_ = NaNBox::fromPtr(static_cast<const void*>(raw));
            return raw;
        }
        return ptr;
    }

    // ---- 从堆指针构造（内部工厂，接管所有权）----
    static Value fromHeapPtr(RefCounted* ptr) {
        Value v;
        v.box_ = NaNBox::fromPtr(static_cast<const void*>(ptr));
        return v;
    }

public:
    // ---- 构造 / 拷贝 / 移动 ----

    Value() : box_(NaNBox::null()) {}

    // 整型构造：int48 范围内内联，超范围装箱
    explicit Value(int64_t v) {
        if (NaNBox::canEncodeInt(v)) {
            box_ = NaNBox::fromInt(v);
        } else {
            // PERF-12: 超出 int48 范围，堆分配 BoxedIntData
            box_ = NaNBox::fromPtr(static_cast<const void*>(new BoxedIntData(v)));
        }
    }
    explicit Value(int v) : Value(static_cast<int64_t>(v)) {}

    // 浮点构造
    explicit Value(double v) : box_(NaNBox::fromFloat(v)) {}

    // 布尔构造
    explicit Value(bool v) : box_(NaNBox::fromBool(v)) {}

    // 字符串构造
    explicit Value(const std::string& v)
        : box_(NaNBox::fromPtr(static_cast<const void*>(new StringData(v)))) {}
    explicit Value(std::string&& v)
        : box_(NaNBox::fromPtr(static_cast<const void*>(new StringData(std::move(v))))) {}
    explicit Value(const char* v)
        : box_(NaNBox::fromPtr(static_cast<const void*>(new StringData(std::string(v))))) {}

    // 数组构造
    explicit Value(const std::vector<Value>& v)
        : box_(NaNBox::fromPtr(static_cast<const void*>(new ArrayData(v)))) {}
    explicit Value(std::vector<Value>&& v)
        : box_(NaNBox::fromPtr(static_cast<const void*>(new ArrayData(std::move(v))))) {}

    // 字典构造
    explicit Value(const std::unordered_map<std::string, Value>& v) {
        auto* d = new DictData();
        d->entries = v;
        box_ = NaNBox::fromPtr(static_cast<const void*>(d));
    }
    explicit Value(std::unordered_map<std::string, Value>&& v) {
        auto* d = new DictData();
        d->entries = std::move(v);
        box_ = NaNBox::fromPtr(static_cast<const void*>(d));
    }

    // PERF-12: 手动引用计数管理
    Value(const Value& other) : box_(other.box_) {
        if (box_.isPointer()) {
            box_.asPtr<RefCounted>()->addRef();
        }
    }

    Value& operator=(const Value& other) {
        if (this != &other) {
            // 先固化 other 的位并 addRef，再 release 旧值——避免"自子对象赋值"
            // （如 a = a.arrayVal()[i]）时 release 旧容器导致 other 悬垂的 UAF。
            NaNBox saved = other.box_;
            if (saved.isPointer()) {
                saved.asPtr<RefCounted>()->addRef();
            }
            if (box_.isPointer()) {
                box_.asPtr<RefCounted>()->release();
            }
            box_ = saved;
        }
        return *this;
    }

    Value(Value&& other) noexcept : box_(other.box_) {
        other.box_ = NaNBox::null();
    }

    Value& operator=(Value&& other) noexcept {
        if (this != &other) {
            // 先把 other 抽空，再 release 旧值——避免 other 别名到 *this 拥有的
            // 容器子元素时，release 触发容器析构使 other 悬垂。
            NaNBox saved = other.box_;
            other.box_ = NaNBox::null();
            if (box_.isPointer()) {
                box_.asPtr<RefCounted>()->release();
            }
            box_ = saved;
        }
        return *this;
    }

    ~Value() {
        if (box_.isPointer()) {
            box_.asPtr<RefCounted>()->release();
        }
    }

    /// 显式深拷贝（需要完全独立副本时使用）
    /// 注意：环形容器结构（如 a.append(a)）会被检测到并打断（back-edge
    /// 指向浅拷贝引用），避免无限递归栈溢出。线性深嵌套超过
    /// MAX_CLONE_DEPTH 也会终止并返回浅拷贝。
    Value clone() const {
        if (!box_.isPointer()) {
            // 标量：直接拷贝 NaNBox
            Value result;
            result.box_ = box_;
            return result;
        }
        thread_local std::unordered_map<const void*, Value> tlsCloned;
        tlsCloned.clear();
        return cloneImpl(0, tlsCloned);
    }

    // clone 递归深度上限（对齐 equals/toString 的 MAX_*_DEPTH）
    static constexpr int MAX_CLONE_DEPTH = RuntimeLimits::MAX_CLONE_DEPTH;

private:
    /// 递归深拷贝实现。
    /// cloned 映射：旧 RefCounted* → 已克隆的新 Value，用于打断环形 back-edge
    /// 和避免 DAG 重复克隆。遇到已克隆节点直接返回新引用（addRef）。
    Value cloneImpl(int depth, std::unordered_map<const void*, Value>& cloned) const {
        if (!box_.isPointer()) {
            Value result;
            result.box_ = box_;
            return result;
        }
        auto* rc = box_.asPtr<RefCounted>();
        // 深度保护：超限时返回浅拷贝（保留原引用），避免栈溢出
        if (depth >= MAX_CLONE_DEPTH) {
            return *this;  // 拷贝构造 + addRef
        }
        // 环检测：遇到已克隆节点直接复用（addRef 后返回）
        auto it = cloned.find(rc);
        if (it != cloned.end()) {
            return it->second;  // 拷贝构造 + addRef
        }

        switch (rc->type) {
        case ValueType::VAL_INT: {
            auto* p = static_cast<BoxedIntData*>(rc);
            return fromHeapPtr(new BoxedIntData(p->value));
        }
        case ValueType::VAL_STRING: {
            auto* p = static_cast<StringData*>(rc);
            return fromHeapPtr(new StringData(p->value));
        }
        case ValueType::VAL_ARRAY: {
            auto* p = static_cast<ArrayData*>(rc);
            auto* newPtr = new ArrayData();
            Value result = fromHeapPtr(newPtr);
            // 先登记到 cloned，再递归克隆元素——这样环 back-edge
            // 在递归遇到此节点时能从 cloned 取到新引用。
            cloned.emplace(rc, result);
            newPtr->elements.reserve(p->elements.size());
            for (const auto& elem : p->elements) {
                newPtr->elements.push_back(elem.cloneImpl(depth + 1, cloned));
            }
            return result;
        }
        case ValueType::VAL_DICT: {
            auto* p = static_cast<DictData*>(rc);
            auto* newPtr = new DictData();
            Value result = fromHeapPtr(newPtr);
            cloned.emplace(rc, result);
            newPtr->entries.reserve(p->entries.size());
            for (const auto& kv : p->entries) {
                newPtr->entries.emplace(kv.first, kv.second.cloneImpl(depth + 1, cloned));
            }
            return result;
        }
        case ValueType::VAL_INSTANCE: {
            auto* p = static_cast<InstanceData*>(rc);
            auto* newPtr = new InstanceData(p->className);
            Value result = fromHeapPtr(newPtr);
            cloned.emplace(rc, result);
            newPtr->fields.reserve(p->fields.size());
            for (const auto& kv : p->fields) {
                newPtr->fields.emplace(kv.first, kv.second.cloneImpl(depth + 1, cloned));
            }
            return result;
        }
        case ValueType::VAL_CLOSURE: {
            auto* p = static_cast<ClosureData*>(rc);
            auto* newPtr = new ClosureData(*p);  // 浅拷贝 env/params/body/vmClosure
            Value result = fromHeapPtr(newPtr);
            cloned.emplace(rc, result);
            newPtr->capturedVars.clear();
            for (const auto& kv : p->capturedVars) {
                newPtr->capturedVars.emplace(kv.first, kv.second.cloneImpl(depth + 1, cloned));
            }
            return result;
        }
        default:
            return Value();  // null
        }
    }

public:

    // ---- 静态工厂方法 ----

    static Value nullValue() { return Value(); }

    static Value makeInstance(const std::string& clsName) {
        return fromHeapPtr(new InstanceData(clsName));
    }

    static Value makeClosure(const std::string& name,
                             std::shared_ptr<Environment> env,
                             const std::vector<std::string>& params,
                             std::shared_ptr<FunDecl> body = nullptr) {
        return fromHeapPtr(new ClosureData(name, env, params, std::move(body)));
    }

    // ---- 类型查询 ----

    ValueType getType() const {
        switch (box_.tag()) {
        case NaNBox::Tag::FLOAT:   return ValueType::VAL_FLOAT;
        case NaNBox::Tag::INT:     return ValueType::VAL_INT;
        case NaNBox::Tag::BOOL:    return ValueType::VAL_BOOL;
        case NaNBox::Tag::NUL:     return ValueType::VAL_NULL;
        case NaNBox::Tag::POINTER: return box_.asPtr<RefCounted>()->type;
        }
        return ValueType::VAL_NULL;
    }

    // 向后兼容：允许读取 .type
    ValueType type() const { return getType(); }

    bool isInt()      const {
        if (box_.isInt()) return true;  // 内联 int48
        if (box_.isPointer()) {
            return box_.asPtr<RefCounted>()->type == ValueType::VAL_INT;  // 装箱 int64
        }
        return false;
    }
    bool isFloat()    const { return box_.isFloat(); }
    bool isBool()     const { return box_.isBool(); }
    bool isNull()     const { return box_.isNull(); }
    bool isString()   const {
        return box_.isPointer() && box_.asPtr<RefCounted>()->type == ValueType::VAL_STRING;
    }
    bool isArray()    const {
        return box_.isPointer() && box_.asPtr<RefCounted>()->type == ValueType::VAL_ARRAY;
    }
    bool isDict()     const {
        return box_.isPointer() && box_.asPtr<RefCounted>()->type == ValueType::VAL_DICT;
    }
    bool isInstance() const {
        return box_.isPointer() && box_.asPtr<RefCounted>()->type == ValueType::VAL_INSTANCE;
    }
    bool isClosure()  const {
        return box_.isPointer() && box_.asPtr<RefCounted>()->type == ValueType::VAL_CLOSURE;
    }
    bool isNumber()   const { return isInt() || isFloat(); }

    // ---- 访问器 ----
    // 标量访问器返回 by value（NaNBox 内联存储，非地址able lvalue）
    // 堆类型访问器返回引用（需 dereference 指针）
    // 非 const 堆类型访问器调用 ensureUnique() 确保 COW 独占

    // -- intVal --
    int64_t intVal() const {
        // AUDIT-NANBOX fix: assert 在 Release 被剥离，改为运行时 abort（与 NaNBox::asXxx 一致）
        if (!isInt()) { std::abort(); }
        if (box_.isInt()) return box_.asInt();  // 内联 int48
        return box_.asPtr<BoxedIntData>()->value;  // 装箱 int64
    }

    // -- floatVal --
    double floatVal() const {
        if (!isFloat()) { std::abort(); }
        return box_.asFloat();
    }

    // -- boolVal --
    bool boolVal() const {
        if (!isBool()) { std::abort(); }
        return box_.asBool();
    }

    // -- stringVal --
    std::string& stringVal() {
        if (!isString()) { std::abort(); }
        auto* sd = ensureUnique<StringData>();
        sd->cachedCodepointCount.reset();  // perf1 fix: 字符串可能被修改，使码位缓存失效
        return sd->value;
    }
    const std::string& stringVal() const {
        if (!isString()) { std::abort(); }
        return box_.asPtr<StringData>()->value;
    }
    // perf1 fix: 返回字符串 UTF-8 码位数（带缓存，const 路径首次计算后复用）
    // O(n) 首次 → O(1) 后续，消除 len()/substr() 循环中的 O(n²) 重复扫描。
    int64_t codepointCount() const {
        if (!isString()) { std::abort(); }
        StringData* sd = box_.asPtr<StringData>();
        if (!sd->cachedCodepointCount.has_value()) {
            sd->cachedCodepointCount = Utf8::codepointCount(sd->value);
        }
        return *sd->cachedCodepointCount;
    }

    // -- arrayVal --
    std::vector<Value>& arrayVal() {
        if (!isArray()) { std::abort(); }
        return ensureUnique<ArrayData>()->elements;
    }
    const std::vector<Value>& arrayVal() const {
        if (!isArray()) { std::abort(); }
        return box_.asPtr<ArrayData>()->elements;
    }

    // -- dictVal --
    std::unordered_map<std::string, Value>& dictVal() {
        if (!isDict()) { std::abort(); }
        return ensureUnique<DictData>()->entries;
    }
    const std::unordered_map<std::string, Value>& dictVal() const {
        if (!isDict()) { std::abort(); }
        return box_.asPtr<DictData>()->entries;
    }

    // -- className（实例专用）--
    std::string& className() {
        if (!isInstance()) { std::abort(); }
        return ensureUnique<InstanceData>()->className;
    }
    const std::string& className() const {
        if (!isInstance()) { std::abort(); }
        return box_.asPtr<InstanceData>()->className;
    }

    // -- fields（实例字段）--
    std::unordered_map<std::string, Value>& fields() {
        if (!isInstance()) { std::abort(); }
        return ensureUnique<InstanceData>()->fields;
    }
    const std::unordered_map<std::string, Value>& fields() const {
        if (!isInstance()) { std::abort(); }
        return box_.asPtr<InstanceData>()->fields;
    }

    // -- 闭包字段 --
    std::string& closureName() {
        if (!isClosure()) { std::abort(); }
        return ensureUnique<ClosureData>()->name;
    }
    const std::string& closureName() const {
        if (!isClosure()) { std::abort(); }
        return box_.asPtr<ClosureData>()->name;
    }

    std::shared_ptr<Environment> closureEnv() const {
        if (!isClosure()) { std::abort(); }
        return box_.asPtr<ClosureData>()->env.lock();
    }

    std::vector<std::string>& closureParams() {
        if (!isClosure()) { std::abort(); }
        return ensureUnique<ClosureData>()->params;
    }
    const std::vector<std::string>& closureParams() const {
        if (!isClosure()) { std::abort(); }
        return box_.asPtr<ClosureData>()->params;
    }

    FunDecl* closureBody() const {
        if (!isClosure()) { std::abort(); }
        return box_.asPtr<ClosureData>()->body.get();
    }
    std::shared_ptr<FunDecl> closureBodyShared() const {
        if (!isClosure()) { std::abort(); }
        return box_.asPtr<ClosureData>()->body;
    }

    std::unordered_map<std::string, Value>& capturedVars() {
        if (!isClosure()) { std::abort(); }
        return ensureUnique<ClosureData>()->capturedVars;
    }
    const std::unordered_map<std::string, Value>& capturedVars() const {
        if (!isClosure()) { std::abort(); }
        return box_.asPtr<ClosureData>()->capturedVars;
    }

    // VM-05/06: VM 闭包数据访问器
    std::shared_ptr<VMClosureData>& vmClosure() {
        if (!isClosure()) { std::abort(); }
        return ensureUnique<ClosureData>()->vmClosure;
    }
    const std::shared_ptr<VMClosureData>& vmClosure() const {
        if (!isClosure()) { std::abort(); }
        return box_.asPtr<ClosureData>()->vmClosure;
    }

    // ============================================================
    // 原地变异辅助方法 — 跳过 COW detach 当 refCount==1
    // ============================================================

    std::string* tryGetMutableString() {
        if (!isString()) return nullptr;
        auto* ptr = box_.asPtr<StringData>();
        return ptr->isUnique() ? &ptr->value : nullptr;
    }

    std::vector<Value>* tryGetMutableArray() {
        if (!isArray()) return nullptr;
        auto* ptr = box_.asPtr<ArrayData>();
        return ptr->isUnique() ? &ptr->elements : nullptr;
    }

    std::unordered_map<std::string, Value>* tryGetMutableDict() {
        if (!isDict()) return nullptr;
        auto* ptr = box_.asPtr<DictData>();
        return ptr->isUnique() ? &ptr->entries : nullptr;
    }

    std::unordered_map<std::string, Value>* tryGetMutableFields() {
        if (!isInstance()) return nullptr;
        auto* ptr = box_.asPtr<InstanceData>();
        return ptr->isUnique() ? &ptr->fields : nullptr;
    }

    bool isUniquelyOwned() const {
        if (!box_.isPointer()) return true;  // 标量始终独占
        return box_.asPtr<RefCounted>()->isUnique();
    }

    // ============================================================
    // 工具方法
    // ============================================================

    double toDouble() const {
        if (isInt()) return static_cast<double>(intVal());
        if (isFloat()) return box_.asFloat();
        return 0.0;
    }

    std::string typeName() const {
        switch (getType()) {
        case ValueType::VAL_INT:      return TypeName::INT;
        case ValueType::VAL_FLOAT:    return TypeName::FLOAT;
        case ValueType::VAL_BOOL:     return TypeName::BOOL;
        case ValueType::VAL_STRING:   return TypeName::STRING;
        case ValueType::VAL_NULL:     return TypeName::NULL_T;
        case ValueType::VAL_ARRAY:    return TypeName::ARRAY;
        case ValueType::VAL_DICT:     return TypeName::DICT;
        case ValueType::VAL_INSTANCE: {
            const auto& cn = className();
            return cn.empty() ? TypeName::INSTANCE : cn;
        }
        case ValueType::VAL_CLOSURE:  return TypeName::CLOSURE;
        }
        return "unknown";
    }

    // B5 fix: toString 递归深度上限
    static constexpr int MAX_TOSTRING_DEPTH = RuntimeLimits::MAX_TOSTRING_DEPTH;
    // P1-6/7 fix: equals 递归深度上限
    static constexpr int MAX_EQUALS_DEPTH = RuntimeLimits::MAX_EQUALS_DEPTH;

    /// 相等比较
    bool equals(const Value& other) const {
        bool thisScalar = isInt() || isFloat() || isBool() || isString() || isNull();
        bool otherScalar = other.isInt() || other.isFloat() || other.isBool() || other.isString() || other.isNull();
        if (thisScalar || otherScalar) {
            return equalsImpl(other, nullptr, 0);
        }
        thread_local std::unordered_set<const void*> tlsVisited;
        tlsVisited.clear();
        return equalsImpl(other, &tlsVisited, 0);
    }

private:
    bool equalsImpl(const Value& other,
                    std::unordered_set<const void*>* visited,
                    int depth) const;

public:
    std::string toString() const {
        switch (getType()) {
        case ValueType::VAL_INT: {
            // Perf-Finding2: std::to_string 在 MSVC 上触发 locale 查询 + 堆分配，
            // 用 std::to_chars 写入栈缓冲消除 locale 开销（输出字节与 to_string 完全一致）
            char buf[32];
            auto res = std::to_chars(buf, buf + sizeof(buf), intVal());
            return std::string(buf, res.ptr);
        }
        case ValueType::VAL_FLOAT: {
            char buf[64];
            int len = snprintf(buf, sizeof(buf), "%.17g", box_.asFloat());
            if (len < 0) return "nan";
            return std::string(buf, len);
        }
        case ValueType::VAL_BOOL:
            return box_.asBool() ? "true" : "false";
        case ValueType::VAL_STRING:
            return stringVal();
        case ValueType::VAL_NULL:
            return "null";
        case ValueType::VAL_CLOSURE:
            return "<fun:" + closureName() + ">";
        default:
            break;
        }
        thread_local std::unordered_set<const void*> tlsVisited;
        tlsVisited.clear();
        return toStringImpl(tlsVisited, 0);
    }

private:
    std::string toStringImpl(std::unordered_set<const void*>& visited, int depth) const;

public:

    /// 判断真值
    bool isTruthy() const {
        switch (getType()) {
        case ValueType::VAL_INT:      return intVal() != 0;
        case ValueType::VAL_FLOAT:    return box_.asFloat() != 0.0;
        case ValueType::VAL_BOOL:     return box_.asBool();
        case ValueType::VAL_STRING:   return !stringVal().empty();
        case ValueType::VAL_NULL:     return false;
        case ValueType::VAL_ARRAY:    return true;
        case ValueType::VAL_DICT:     return true;
        case ValueType::VAL_INSTANCE: return true;
        case ValueType::VAL_CLOSURE:  return true;
        }
        return false;
    }
};

// 编译期断言：Value 必须是 8 字节（NaN-boxing）
static_assert(sizeof(Value) == 8, "Value must be 8 bytes (NaN-boxed)");

// 包含数据载体模块（VMUpvalue 等，依赖完整 Value 类型）
#include "interpreter/ValueData.h"
