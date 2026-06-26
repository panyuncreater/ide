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
// ============================================================

#include "interpreter/ValueTypes.h"
#include "interpreter/RefCounted.h"   // PERF-12: 侵入式引用计数基类
#include "interpreter/NaNBox.h"       // PERF-12: 8字节 NaN-boxing 编码
#include "interpreter/NumericUtils.h" // 共享溢出检查（B6 fix）
#include "common/RuntimeLimits.h"

#include <string>
#include <unordered_map>
#include <sstream>
#include <cstdio>
#include <vector>
#include <memory>
#include <cstdint>
#include <cmath>
#include <cassert>
#include <unordered_set>

// ============================================================
// Value 运行时值结构体
// ============================================================

struct Value {
private:
    // ---- 堆类型数据载体（继承 RefCounted， intrusive 引用计数）----
    struct StringData : RefCounted {
        std::string value;
        StringData() : RefCounted(ValueType::VAL_STRING) {}
        explicit StringData(std::string v) : RefCounted(ValueType::VAL_STRING), value(std::move(v)) {}
    };

    struct ArrayData : RefCounted {
        std::vector<Value> elements;
        ArrayData() : RefCounted(ValueType::VAL_ARRAY) {}
        explicit ArrayData(std::vector<Value> v) : RefCounted(ValueType::VAL_ARRAY), elements(std::move(v)) {}
    };

    struct DictData : RefCounted {
        std::unordered_map<std::string, Value> entries;
        DictData() : RefCounted(ValueType::VAL_DICT) {}
    };

    struct InstanceData : RefCounted {
        std::string className;
        std::unordered_map<std::string, Value> fields;
        InstanceData() : RefCounted(ValueType::VAL_INSTANCE) {}
        explicit InstanceData(const std::string& cn) : RefCounted(ValueType::VAL_INSTANCE), className(cn) {}
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
            T* cloned = new T(*ptr);  // 拷贝构造（RefCounted 拷贝 ctor 重置 refCount=1）
            ptr->release();           // 释放旧引用
            box_ = NaNBox::fromPtr(static_cast<const void*>(cloned));
            return cloned;
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
            if (box_.isPointer()) {
                box_.asPtr<RefCounted>()->release();
            }
            box_ = other.box_;
            if (box_.isPointer()) {
                box_.asPtr<RefCounted>()->addRef();
            }
        }
        return *this;
    }

    Value(Value&& other) noexcept : box_(other.box_) {
        other.box_ = NaNBox::null();
    }

    Value& operator=(Value&& other) noexcept {
        if (this != &other) {
            if (box_.isPointer()) {
                box_.asPtr<RefCounted>()->release();
            }
            box_ = other.box_;
            other.box_ = NaNBox::null();
        }
        return *this;
    }

    ~Value() {
        if (box_.isPointer()) {
            box_.asPtr<RefCounted>()->release();
        }
    }

    /// 显式深拷贝（需要完全独立副本时使用）
    Value clone() const {
        if (!box_.isPointer()) {
            // 标量：直接拷贝 NaNBox
            Value result;
            result.box_ = box_;
            return result;
        }
        // 堆类型：递归深拷贝
        auto* rc = box_.asPtr<RefCounted>();
        switch (rc->type) {
        case ValueType::VAL_INT: {
            // 装箱的 int64
            auto* p = static_cast<BoxedIntData*>(rc);
            return fromHeapPtr(new BoxedIntData(p->value));
        }
        case ValueType::VAL_STRING: {
            auto* p = static_cast<StringData*>(rc);
            return fromHeapPtr(new StringData(p->value));
        }
        case ValueType::VAL_ARRAY: {
            auto* p = static_cast<ArrayData*>(rc);
            auto* cloned = new ArrayData();
            cloned->elements.reserve(p->elements.size());
            for (const auto& elem : p->elements) {
                cloned->elements.push_back(elem.clone());
            }
            return fromHeapPtr(cloned);
        }
        case ValueType::VAL_DICT: {
            auto* p = static_cast<DictData*>(rc);
            auto* cloned = new DictData();
            cloned->entries.reserve(p->entries.size());
            for (const auto& kv : p->entries) {
                cloned->entries.emplace(kv.first, kv.second.clone());
            }
            return fromHeapPtr(cloned);
        }
        case ValueType::VAL_INSTANCE: {
            auto* p = static_cast<InstanceData*>(rc);
            auto* cloned = new InstanceData(p->className);
            cloned->fields.reserve(p->fields.size());
            for (const auto& kv : p->fields) {
                cloned->fields.emplace(kv.first, kv.second.clone());
            }
            return fromHeapPtr(cloned);
        }
        case ValueType::VAL_CLOSURE: {
            auto* p = static_cast<ClosureData*>(rc);
            auto* cloned = new ClosureData(*p);  // 浅拷贝 env/params/body/vmClosure
            cloned->capturedVars.clear();
            for (const auto& kv : p->capturedVars) {
                cloned->capturedVars.emplace(kv.first, kv.second.clone());
            }
            return fromHeapPtr(cloned);
        }
        default:
            return Value();  // null
        }
    }

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
        assert(isInt() && "intVal() called on non-int Value");
        if (box_.isInt()) return box_.asInt();  // 内联 int48
        return box_.asPtr<BoxedIntData>()->value;  // 装箱 int64
    }

    // -- floatVal --
    double floatVal() const {
        assert(isFloat() && "floatVal() called on non-float Value");
        return box_.asFloat();
    }

    // -- boolVal --
    bool boolVal() const {
        assert(isBool() && "boolVal() called on non-bool Value");
        return box_.asBool();
    }

    // -- stringVal --
    std::string& stringVal() {
        assert(isString() && "stringVal() called on non-string Value");
        return ensureUnique<StringData>()->value;
    }
    const std::string& stringVal() const {
        assert(isString() && "stringVal() called on non-string Value");
        return box_.asPtr<StringData>()->value;
    }

    // -- arrayVal --
    std::vector<Value>& arrayVal() {
        assert(isArray() && "arrayVal() called on non-array Value");
        return ensureUnique<ArrayData>()->elements;
    }
    const std::vector<Value>& arrayVal() const {
        assert(isArray() && "arrayVal() called on non-array Value");
        return box_.asPtr<ArrayData>()->elements;
    }

    // -- dictVal --
    std::unordered_map<std::string, Value>& dictVal() {
        assert(isDict() && "dictVal() called on non-dict Value");
        return ensureUnique<DictData>()->entries;
    }
    const std::unordered_map<std::string, Value>& dictVal() const {
        assert(isDict() && "dictVal() called on non-dict Value");
        return box_.asPtr<DictData>()->entries;
    }

    // -- className（实例专用）--
    std::string& className() {
        assert(isInstance() && "className() called on non-instance Value");
        return ensureUnique<InstanceData>()->className;
    }
    const std::string& className() const {
        assert(isInstance() && "className() called on non-instance Value");
        return box_.asPtr<InstanceData>()->className;
    }

    // -- fields（实例字段）--
    std::unordered_map<std::string, Value>& fields() {
        assert(isInstance() && "fields() called on non-instance Value");
        return ensureUnique<InstanceData>()->fields;
    }
    const std::unordered_map<std::string, Value>& fields() const {
        assert(isInstance() && "fields() called on non-instance Value");
        return box_.asPtr<InstanceData>()->fields;
    }

    // -- 闭包字段 --
    std::string& closureName() {
        assert(isClosure() && "closureName() called on non-closure Value");
        return ensureUnique<ClosureData>()->name;
    }
    const std::string& closureName() const {
        assert(isClosure() && "closureName() called on non-closure Value");
        return box_.asPtr<ClosureData>()->name;
    }

    std::shared_ptr<Environment> closureEnv() const {
        assert(isClosure() && "closureEnv() called on non-closure Value");
        return box_.asPtr<ClosureData>()->env.lock();
    }

    std::vector<std::string>& closureParams() {
        assert(isClosure() && "closureParams() called on non-closure Value");
        return ensureUnique<ClosureData>()->params;
    }
    const std::vector<std::string>& closureParams() const {
        assert(isClosure() && "closureParams() called on non-closure Value");
        return box_.asPtr<ClosureData>()->params;
    }

    FunDecl* closureBody() const {
        assert(isClosure() && "closureBody() called on non-closure Value");
        return box_.asPtr<ClosureData>()->body.get();
    }
    std::shared_ptr<FunDecl> closureBodyShared() const {
        assert(isClosure() && "closureBodyShared() called on non-closure Value");
        return box_.asPtr<ClosureData>()->body;
    }

    std::unordered_map<std::string, Value>& capturedVars() {
        assert(isClosure() && "capturedVars() called on non-closure Value");
        return ensureUnique<ClosureData>()->capturedVars;
    }
    const std::unordered_map<std::string, Value>& capturedVars() const {
        assert(isClosure() && "capturedVars() called on non-closure Value");
        return box_.asPtr<ClosureData>()->capturedVars;
    }

    // VM-05/06: VM 闭包数据访问器
    std::shared_ptr<VMClosureData>& vmClosure() {
        assert(isClosure() && "vmClosure() called on non-closure Value");
        return ensureUnique<ClosureData>()->vmClosure;
    }
    const std::shared_ptr<VMClosureData>& vmClosure() const {
        assert(isClosure() && "vmClosure() called on non-closure Value");
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
        case ValueType::VAL_INT:
            return std::to_string(intVal());
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
