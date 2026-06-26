#pragma once

// ============================================================
// Value.h — 操作接口模块
// ------------------------------------------------------------
// 运行时值结构体 Value 的完整定义，包含数据载体（私有嵌套）
// 与所有操作接口（构造/查询/访问/变异/工具/比较）。
//
// 本文件从原 611 行的上帝头文件拆分而来：
//   - 类型定义 → ValueTypes.h（ValueType 枚举、前置声明、VMClosureData）
//   - 操作接口 → 本文件（Value 结构体）
//   - 数据载体 → ValueData.h（VMUpvalue，依赖完整 Value 类型）
//
// 包含本文件即可获得全部内容（向后兼容）。
// ============================================================

#include "interpreter/ValueTypes.h"
#include "interpreter/NumericUtils.h"  // 共享溢出检查（B6 fix）
#include "common/RuntimeLimits.h"

#include <string>
#include <unordered_map>
#include <variant>
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
    // ---- 复杂类型数据载体（通过 shared_ptr 持有，支持 COW）----
    struct StringData  { std::string value; };
    struct ArrayData   { std::vector<Value> elements; };
    struct DictData    { std::unordered_map<std::string, Value> entries; };
    struct InstanceData {
        std::string className;
        std::unordered_map<std::string, Value> fields;
    };

    struct ClosureData {
        std::string name;
        std::weak_ptr<Environment> env;  // V4 fix: weak_ptr 打破闭包→环境→闭包的循环引用
        std::vector<std::string> params;
        std::unordered_map<std::string, Value> capturedVars;
        // A3 fix: shared_ptr 所有权，避免 AST 重建后 funRegistry_/methods 持有的裸指针悬垂。
        // 自包含闭包不依赖 funRegistry_，body 共享 AST 节点所有权。
        std::shared_ptr<FunDecl> body;
        std::shared_ptr<VMClosureData> vmClosure; // VM-05/06: VM 闭包数据（定义于 ValueTypes.h）
    };

    // ---- 变体存储：同一时刻仅一个类型有效 ----
    // A1: unique_ptr → shared_ptr（支持 COW 浅拷贝 + detach-on-write）
    using Data = std::variant<
        std::monostate,                    // 0: VAL_NULL
        int64_t,                           // 1: VAL_INT
        double,                            // 2: VAL_FLOAT
        bool,                              // 3: VAL_BOOL
        std::shared_ptr<StringData>,       // 4: VAL_STRING
        std::shared_ptr<ArrayData>,        // 5: VAL_ARRAY
        std::shared_ptr<DictData>,         // 6: VAL_DICT
        std::shared_ptr<InstanceData>,     // 7: VAL_INSTANCE
        std::shared_ptr<ClosureData>       // 8: VAL_CLOSURE
    >;

    // 编译期校验：ValueType 枚举值必须与 variant Data 的 alternative 索引严格一致
    static_assert(static_cast<size_t>(ValueType::VAL_NULL)     == 0, "VAL_NULL must be variant index 0 (monostate)");
    static_assert(static_cast<size_t>(ValueType::VAL_INT)      == 1, "VAL_INT must be variant index 1 (int64_t)");
    static_assert(static_cast<size_t>(ValueType::VAL_FLOAT)    == 2, "VAL_FLOAT must be variant index 2 (double)");
    static_assert(static_cast<size_t>(ValueType::VAL_BOOL)     == 3, "VAL_BOOL must be variant index 3 (bool)");
    static_assert(static_cast<size_t>(ValueType::VAL_STRING)   == 4, "VAL_STRING must be variant index 4 (StringData)");
    static_assert(static_cast<size_t>(ValueType::VAL_ARRAY)    == 5, "VAL_ARRAY must be variant index 5 (ArrayData)");
    static_assert(static_cast<size_t>(ValueType::VAL_DICT)     == 6, "VAL_DICT must be variant index 6 (DictData)");
    static_assert(static_cast<size_t>(ValueType::VAL_INSTANCE) == 7, "VAL_INSTANCE must be variant index 7 (InstanceData)");
    static_assert(static_cast<size_t>(ValueType::VAL_CLOSURE)  == 8, "VAL_CLOSURE must be variant index 8 (ClosureData)");

    Data data_;

    // ---- A1: COW detach — 写入前确保独占所有权 ----
    // 若引用计数 > 1，深拷贝一份新数据并替换 shared_ptr
    template<size_t I>
    auto& ensureUnique() {
        auto& ptr = std::get<I>(data_);
        if (ptr && ptr.use_count() > 1) {
            ptr = std::make_shared<std::remove_reference_t<decltype(*ptr)>>(*ptr);
        }
        assert(ptr && "ensureUnique() called on null shared_ptr (moved-from Value?)");
        return *ptr;
    }

    // ---- 显式深拷贝（用于需要完全独立副本的场景）----
    // D2 fix: 递归深拷贝嵌套容器。原实现仅拷贝顶层 shared_ptr，嵌套数组/字典/实例
    // 仍共享底层数据，修改副本的嵌套元素会影响原件。现递归克隆所有层级的 Value。
    Data deepClone(const Data& src) const {
        switch (static_cast<ValueType>(src.index())) {
        case ValueType::VAL_STRING: {
            const auto& p = std::get<4>(src);
            return p ? std::make_shared<StringData>(*p) : std::shared_ptr<StringData>{};
        }
        case ValueType::VAL_ARRAY: {
            const auto& p = std::get<5>(src);
            if (!p) return std::shared_ptr<ArrayData>{};
            auto cloned = std::make_shared<ArrayData>();
            cloned->elements.reserve(p->elements.size());
            for (const auto& elem : p->elements) {
                // 递归深拷贝每个元素
                Value tmp;
                tmp.data_ = deepClone(elem.data_);
                cloned->elements.emplace_back(std::move(tmp));
            }
            return cloned;
        }
        case ValueType::VAL_DICT: {
            const auto& p = std::get<6>(src);
            if (!p) return std::shared_ptr<DictData>{};
            auto cloned = std::make_shared<DictData>();
            cloned->entries.reserve(p->entries.size());
            for (const auto& kv : p->entries) {
                Value tmp;
                tmp.data_ = deepClone(kv.second.data_);
                cloned->entries.emplace(kv.first, std::move(tmp));
            }
            return cloned;
        }
        case ValueType::VAL_INSTANCE: {
            const auto& p = std::get<7>(src);
            if (!p) return std::shared_ptr<InstanceData>{};
            auto cloned = std::make_shared<InstanceData>();
            cloned->className = p->className;
            cloned->fields.reserve(p->fields.size());
            for (const auto& kv : p->fields) {
                Value tmp;
                tmp.data_ = deepClone(kv.second.data_);
                cloned->fields.emplace(kv.first, std::move(tmp));
            }
            return cloned;
        }
        case ValueType::VAL_CLOSURE: {
            const auto& p = std::get<8>(src);
            if (!p) return std::shared_ptr<ClosureData>{};
            auto cloned = std::make_shared<ClosureData>(*p);  // 浅拷贝 env/params/body/vmClosure
            // 递归深拷贝 capturedVars（闭包捕获的变量需独立）
            cloned->capturedVars.clear();
            for (const auto& kv : p->capturedVars) {
                Value tmp;
                tmp.data_ = deepClone(kv.second.data_);
                cloned->capturedVars.emplace(kv.first, std::move(tmp));
            }
            return cloned;
        }
        default:
            if (src.index() == 0) return std::monostate{};
            if (src.index() == 1) return std::get<1>(src);
            if (src.index() == 2) return std::get<2>(src);
            return std::get<3>(src);
        }
    }

public:
    // ---- 构造 / 拷贝 / 移动 ----

    Value() : data_(std::monostate{}) {}

    // 整型构造（int64_t 主构造 + int 便捷构造）
    explicit Value(int64_t v) : data_(v) {}
    explicit Value(int v) : data_(static_cast<int64_t>(v)) {}

    // 浮点构造
    explicit Value(double v) : data_(v) {}

    // 布尔构造（explicit 防止 int→bool 隐式转换）
    explicit Value(bool v) : data_(v) {}

    // 字符串构造
    explicit Value(const std::string& v)
        : data_(std::make_shared<StringData>(StringData{v})) {}
    explicit Value(std::string&& v)
        : data_(std::make_shared<StringData>(StringData{std::move(v)})) {}
    explicit Value(const char* v)
        : data_(std::make_shared<StringData>(StringData{std::string(v)})) {}

    // 数组构造
    explicit Value(const std::vector<Value>& v)
        : data_(std::make_shared<ArrayData>(ArrayData{v})) {}
    explicit Value(std::vector<Value>&& v)
        : data_(std::make_shared<ArrayData>(ArrayData{std::move(v)})) {}

    // 字典构造
    explicit Value(const std::unordered_map<std::string, Value>& v)
        : data_(std::make_shared<DictData>(DictData{v})) {}
    explicit Value(std::unordered_map<std::string, Value>&& v)
        : data_(std::make_shared<DictData>(DictData{std::move(v)})) {}

    // A1: 拷贝构造 — 浅拷贝（shared_ptr 引用计数递增，O(1)）
    Value(const Value& other) = default;

    // A1: 拷贝赋值 — 浅拷贝
    Value& operator=(const Value& other) = default;

    // 移动（默认即可，shared_ptr 自动转移所有权）
    Value(Value&&) noexcept = default;
    Value& operator=(Value&&) noexcept = default;

    ~Value() = default;

    /// A1: 显式深拷贝（需要完全独立副本时使用）
    Value clone() const {
        Value result;
        result.data_ = deepClone(data_);
        return result;
    }

    // ---- 静态工厂方法 ----

    static Value nullValue() { return Value(); }

    static Value makeInstance(const std::string& clsName) {
        Value v;
        v.data_ = std::make_shared<InstanceData>(InstanceData{clsName, {}});
        return v;
    }

    static Value makeClosure(const std::string& name,
                             std::shared_ptr<Environment> env,
                             const std::vector<std::string>& params,
                             std::shared_ptr<FunDecl> body = nullptr) {
        Value v;
        v.data_ = std::make_shared<ClosureData>(ClosureData{name, env, params, {}, std::move(body)});
        return v;
    }

    // ---- 类型查询 ----

    ValueType getType() const {
        return static_cast<ValueType>(data_.index());
    }

    // 向后兼容：允许读取 .type（如 switch(val.type)）
    // 注意：不可写（val.type = X 需改为构造函数/工厂方法）
    ValueType type() const { return getType(); }

    bool isInt()      const { return std::holds_alternative<int64_t>(data_); }
    bool isFloat()    const { return std::holds_alternative<double>(data_); }
    bool isBool()     const { return std::holds_alternative<bool>(data_); }
    bool isString()   const { return std::holds_alternative<std::shared_ptr<StringData>>(data_); }
    bool isNull()     const { return std::holds_alternative<std::monostate>(data_); }
    bool isArray()    const { return std::holds_alternative<std::shared_ptr<ArrayData>>(data_); }
    bool isDict()     const { return std::holds_alternative<std::shared_ptr<DictData>>(data_); }
    bool isInstance() const { return std::holds_alternative<std::shared_ptr<InstanceData>>(data_); }
    bool isClosure()  const { return std::holds_alternative<std::shared_ptr<ClosureData>>(data_); }
    bool isNumber()   const { auto i = data_.index(); return i == 1 || i == 2; }

    // ---- 访问器 ----
    // const 访问器：直接读取，不触发 COW detach
    // 非 const 访问器：调用 ensureUnique() 确保独占后再返回引用

    // -- intVal --
    int64_t& intVal()             { return std::get<1>(data_); }
    const int64_t& intVal() const { return std::get<1>(data_); }

    // -- floatVal --
    double& floatVal()             { return std::get<2>(data_); }
    const double& floatVal() const { return std::get<2>(data_); }

    // -- boolVal --
    bool& boolVal()             { return std::get<3>(data_); }
    const bool& boolVal() const { return std::get<3>(data_); }

    // -- stringVal --
    std::string& stringVal() {
        return ensureUnique<4>().value;
    }
    const std::string& stringVal() const {
        auto& ptr = std::get<4>(data_);
        assert(ptr && "stringVal() called on null StringData");
        return ptr->value;
    }

    // -- arrayVal --
    std::vector<Value>& arrayVal() {
        return ensureUnique<5>().elements;
    }
    const std::vector<Value>& arrayVal() const {
        auto& ptr = std::get<5>(data_);
        assert(ptr && "arrayVal() called on null ArrayData");
        return ptr->elements;
    }

    // -- dictVal --
    std::unordered_map<std::string, Value>& dictVal() {
        return ensureUnique<6>().entries;
    }
    const std::unordered_map<std::string, Value>& dictVal() const {
        auto& ptr = std::get<6>(data_);
        assert(ptr && "dictVal() called on null DictData");
        return ptr->entries;
    }

    // -- className（实例专用）--
    std::string& className() {
        return ensureUnique<7>().className;
    }
    const std::string& className() const {
        auto& ptr = std::get<7>(data_);
        assert(ptr && "className() called on null InstanceData");
        return ptr->className;
    }

    // -- fields（实例字段）--
    std::unordered_map<std::string, Value>& fields() {
        return ensureUnique<7>().fields;
    }
    const std::unordered_map<std::string, Value>& fields() const {
        auto& ptr = std::get<7>(data_);
        assert(ptr && "fields() called on null InstanceData");
        return ptr->fields;
    }

    // -- 闭包字段 --
    std::string& closureName() {
        return ensureUnique<8>().name;
    }
    const std::string& closureName() const {
        auto& ptr = std::get<8>(data_);
        assert(ptr && "closureName() called on null ClosureData");
        return ptr->name;
    }

    std::shared_ptr<Environment> closureEnv() const {
        auto& ptr = std::get<8>(data_);
        assert(ptr && "closureEnv() called on null ClosureData");
        return ptr->env.lock();
    }

    std::vector<std::string>& closureParams() {
        return ensureUnique<8>().params;
    }
    const std::vector<std::string>& closureParams() const {
        auto& ptr = std::get<8>(data_);
        assert(ptr && "closureParams() called on null ClosureData");
        return ptr->params;
    }

    // A3 fix: closureBody() 返回裸指针用于只读访问（Value 存活期间 body 有效）。
    // 移除非 const 写入版本——body 仅通过 makeClosure 设置，无需可变访问器。
    FunDecl* closureBody() const {
        auto& ptr = std::get<8>(data_);
        assert(ptr && "closureBody() called on null ClosureData");
        return ptr->body.get();
    }
    /// 获取 body 的 shared_ptr 副本（用于缓存等需延长生命周期的场景）
    std::shared_ptr<FunDecl> closureBodyShared() const {
        auto& ptr = std::get<8>(data_);
        assert(ptr && "closureBodyShared() called on null ClosureData");
        return ptr->body;
    }

    std::unordered_map<std::string, Value>& capturedVars() {
        return ensureUnique<8>().capturedVars;
    }
    const std::unordered_map<std::string, Value>& capturedVars() const {
        auto& ptr = std::get<8>(data_);
        assert(ptr && "capturedVars() called on null ClosureData");
        return ptr->capturedVars;
    }

    // VM-05/06: VM 闭包数据访问器
    std::shared_ptr<VMClosureData>& vmClosure() {
        return ensureUnique<8>().vmClosure;
    }
    const std::shared_ptr<VMClosureData>& vmClosure() const {
        auto& ptr = std::get<8>(data_);
        assert(ptr && "vmClosure() called on null ClosureData");
        return ptr->vmClosure;
    }

    // ============================================================
    // A2: 原地变异辅助方法 — 跳过 COW detach 当 refcount==1
    // ============================================================

    /// 若独占拥有数组数据（refcount==1），返回可修改指针；否则返回 nullptr
    std::vector<Value>* tryGetMutableArray() {
        if (!isArray()) return nullptr;
        auto& ptr = std::get<5>(data_);
        if (!ptr || ptr.use_count() != 1) return nullptr;
        return &ptr->elements;
    }

    /// 若独占拥有字典数据（refcount==1），返回可修改指针；否则返回 nullptr
    std::unordered_map<std::string, Value>* tryGetMutableDict() {
        if (!isDict()) return nullptr;
        auto& ptr = std::get<6>(data_);
        if (!ptr || ptr.use_count() != 1) return nullptr;
        return &ptr->entries;
    }

    /// 若独占拥有实例数据（refcount==1），返回可修改字段指针；否则返回 nullptr
    std::unordered_map<std::string, Value>* tryGetMutableFields() {
        if (!isInstance()) return nullptr;
        auto& ptr = std::get<7>(data_);
        if (!ptr || ptr.use_count() != 1) return nullptr;
        return &ptr->fields;
    }

    /// 检查是否独占拥有数据（标量类型始终返回 true）
    bool isUniquelyOwned() const {
        switch (getType()) {
        case ValueType::VAL_STRING:   return !std::get<4>(data_) || std::get<4>(data_).use_count() == 1;
        case ValueType::VAL_ARRAY:    return !std::get<5>(data_) || std::get<5>(data_).use_count() == 1;
        case ValueType::VAL_DICT:     return !std::get<6>(data_) || std::get<6>(data_).use_count() == 1;
        case ValueType::VAL_INSTANCE: return !std::get<7>(data_) || std::get<7>(data_).use_count() == 1;
        case ValueType::VAL_CLOSURE:  return !std::get<8>(data_) || std::get<8>(data_).use_count() == 1;
        default: return true;
        }
    }

    // ============================================================
    // 工具方法
    // ============================================================

    /// 转换为 double（用于数值运算）
    double toDouble() const {
        if (isInt()) return static_cast<double>(intVal());
        if (isFloat()) return floatVal();
        return 0.0;
    }

    /// 获取类型名称字符串
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

    // B5 fix: toString 递归深度上限，防止极端嵌套结构导致栈溢出
    // S1 fix: 统一引用 RuntimeLimits
    static constexpr int MAX_TOSTRING_DEPTH = RuntimeLimits::MAX_TOSTRING_DEPTH;
    // P1-6/7 fix: equals 递归深度上限，与 toString 对齐
    static constexpr int MAX_EQUALS_DEPTH = RuntimeLimits::MAX_EQUALS_DEPTH;

    /// 相等比较
    bool equals(const Value& other) const {
        // P1-6/7 fix: 容器类型需要环检测和深度保护
        // 标量类型（int/float/bool/string/null）走快速路径，避免每次分配 unordered_set
        bool thisScalar = isInt() || isFloat() || isBool() || isString() || isNull();
        bool otherScalar = other.isInt() || other.isFloat() || other.isBool() || other.isString() || other.isNull();
        if (thisScalar || otherScalar) {
            return equalsImpl(other, nullptr, 0);
        }
        std::unordered_set<const void*> visited;
        return equalsImpl(other, &visited, 0);
    }

private:
    /// P1-6/7 fix: equals 实现细节（S6 fix: 实现移至 Value.cpp）
    /// - visited: 非空时用于环检测（仅对 VAL_ARRAY/VAL_DICT/VAL_INSTANCE）
    /// - depth: 递归深度，防止线性嵌套栈溢出
    bool equalsImpl(const Value& other,
                    std::unordered_set<const void*>* visited,
                    int depth) const;

public:
    std::string toString() const {
        // P-03 fix: 标量类型走快速路径，避免每次分配 unordered_set
        switch (getType()) {
        case ValueType::VAL_INT:
            return std::to_string(intVal());
        case ValueType::VAL_FLOAT: {
            char buf[64];
            int len = snprintf(buf, sizeof(buf), "%.17g", floatVal());
            if (len < 0) return "nan";
            return std::string(buf, len);
        }
        case ValueType::VAL_BOOL:
            return boolVal() ? "true" : "false";
        case ValueType::VAL_STRING:
            return stringVal();
        case ValueType::VAL_NULL:
            return "null";
        case ValueType::VAL_CLOSURE:
            return "<fun:" + closureName() + ">";
        default:
            break;
        }
        // 容器类型（数组/字典/实例）需要环检测
        std::unordered_set<const void*> visited;
        return toStringImpl(visited, 0);
    }

private:
    /// B5 fix: toString 实现细节（S6 fix: 实现移至 Value.cpp）
    std::string toStringImpl(std::unordered_set<const void*>& visited, int depth) const;

public:

    /// 判断真值
    bool isTruthy() const {
        switch (getType()) {
        case ValueType::VAL_INT:      return intVal() != 0;
        case ValueType::VAL_FLOAT:    return floatVal() != 0.0;
        case ValueType::VAL_BOOL:     return boolVal();
        case ValueType::VAL_STRING:   return !stringVal().empty();
        case ValueType::VAL_NULL:     return false;
        case ValueType::VAL_ARRAY:    return true;
        case ValueType::VAL_DICT:     return true;
        case ValueType::VAL_INSTANCE: return true;
        case ValueType::VAL_CLOSURE:  return true;
        }
        return false;
    }

    // equals() 已上移至 MAX_EQUALS_DEPTH 常量附近，支持环检测和深度保护
};

// 包含数据载体模块（VMUpvalue 等，依赖完整 Value 类型）
// 放在 Value 定义之后，确保 ValueData.h 中的 VMUpvalue 能使用完整 Value 类型
#include "interpreter/ValueData.h"
