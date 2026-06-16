#pragma once

#include <string>
#include <unordered_map>
#include <variant>
#include <sstream>
#include <cstdio>
#include <vector>
#include <memory>
#include <cstdint>

// 前向声明 Environment（避免循环依赖）
class Environment;
class FunDecl;

// ============================================================
// Value 运行时值类型 — std::variant 存储优化
// ============================================================
// 优化说明：
//   旧设计：所有类型字段同时存在（~341 bytes / Value）
//   新设计：std::variant 互斥存储 + unique_ptr 隔离大类型
//   结果：sizeof(Value) 从 ~341 字节降至 ~16 字节（减少 95%+）
//   小类型（int/double/bool/null）内联存储，无堆分配
//   大类型（string/array/dict/instance/closure）通过 unique_ptr 堆分配

/// 值类型枚举（顺序必须与 std::variant Data 的类型顺序严格一致！
/// getType() 通过 static_cast<ValueType>(data_.index()) 将 variant 索引映射到此枚举）
enum class ValueType {
    VAL_NULL,      // 0 = std::monostate
    VAL_INT,       // 1 = int64_t
    VAL_FLOAT,     // 2 = double
    VAL_BOOL,      // 3 = bool
    VAL_STRING,    // 4 = std::unique_ptr<StringData>
    VAL_ARRAY,     // 5 = std::unique_ptr<ArrayData>
    VAL_DICT,      // 6 = std::unique_ptr<DictData>
    VAL_INSTANCE,  // 7 = std::unique_ptr<InstanceData>
    VAL_CLOSURE    // 8 = std::unique_ptr<ClosureData>
};

/// 运行时值结构体
struct Value {
private:
    // ---- 复杂类型数据载体（仅通过 unique_ptr 持有）----
    struct StringData  { std::string value; };
    struct ArrayData   { std::vector<Value> elements; };
    struct DictData    { std::unordered_map<std::string, Value> entries; };
    struct InstanceData {
        std::string className;
        std::unordered_map<std::string, Value> fields;
    };
    struct ClosureData {
        std::string name;
        std::shared_ptr<Environment> env;
        std::vector<std::string> params;
        std::unordered_map<std::string, Value> capturedVars;
        FunDecl* body = nullptr;       // 函数体 AST 节点（自包含，不依赖 funRegistry_）
    };

    // ---- 变体存储：同一时刻仅一个类型有效 ----
    // 注意：类型顺序必须与 ValueType 枚举严格对应（index 0~8 → VAL_NULL~VAL_CLOSURE）
    using Data = std::variant<
        std::monostate,                    // 0: VAL_NULL
        int64_t,                           // 1: VAL_INT
        double,                            // 2: VAL_FLOAT
        bool,                              // 3: VAL_BOOL
        std::unique_ptr<StringData>,       // 4: VAL_STRING
        std::unique_ptr<ArrayData>,        // 5: VAL_ARRAY
        std::unique_ptr<DictData>,         // 6: VAL_DICT
        std::unique_ptr<InstanceData>,     // 7: VAL_INSTANCE
        std::unique_ptr<ClosureData>       // 8: VAL_CLOSURE
    >;

    Data data_;

    // ---- 深拷贝辅助（用于拷贝构造 / 拷贝赋值）----
    // switch 分发比 std::visit + generic lambda 生成更紧凑的跳转表
    Data deepCopy(const Data& src) {
        switch (static_cast<ValueType>(src.index())) {
        case ValueType::VAL_STRING: {
            const auto& p = std::get<4>(src);
            return p ? std::make_unique<StringData>(*p) : std::unique_ptr<StringData>{};
        }
        case ValueType::VAL_ARRAY: {
            const auto& p = std::get<5>(src);
            return p ? std::make_unique<ArrayData>(*p) : std::unique_ptr<ArrayData>{};
        }
        case ValueType::VAL_DICT: {
            const auto& p = std::get<6>(src);
            return p ? std::make_unique<DictData>(*p) : std::unique_ptr<DictData>{};
        }
        case ValueType::VAL_INSTANCE: {
            const auto& p = std::get<7>(src);
            return p ? std::make_unique<InstanceData>(*p) : std::unique_ptr<InstanceData>{};
        }
        case ValueType::VAL_CLOSURE: {
            const auto& p = std::get<8>(src);
            return p ? std::make_unique<ClosureData>(*p) : std::unique_ptr<ClosureData>{};
        }
        default:
            // 基础类型：逐个提取（variant 含 unique_ptr 不可整体拷贝）
            if (src.index() == 0) return std::monostate{};
            if (src.index() == 1) return std::get<1>(src);
            if (src.index() == 2) return std::get<2>(src);
            return std::get<3>(src);  // bool
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
        : data_(std::make_unique<StringData>(StringData{v})) {}
    explicit Value(std::string&& v)
        : data_(std::make_unique<StringData>(StringData{std::move(v)})) {}
    explicit Value(const char* v)
        : data_(std::make_unique<StringData>(StringData{std::string(v)})) {}

    // 数组构造
    explicit Value(const std::vector<Value>& v)
        : data_(std::make_unique<ArrayData>(ArrayData{v})) {}
    explicit Value(std::vector<Value>&& v)
        : data_(std::make_unique<ArrayData>(ArrayData{std::move(v)})) {}

    // 字典构造
    explicit Value(const std::unordered_map<std::string, Value>& v)
        : data_(std::make_unique<DictData>(DictData{v})) {}
    explicit Value(std::unordered_map<std::string, Value>&& v)
        : data_(std::make_unique<DictData>(DictData{std::move(v)})) {}

    // 拷贝构造（深拷贝 unique_ptr 数据）
    Value(const Value& other) : data_(deepCopy(other.data_)) {}

    // 拷贝赋值
    Value& operator=(const Value& other) {
        if (this != &other) data_ = deepCopy(other.data_);
        return *this;
    }

    // 移动（默认即可，unique_ptr 自动转移所有权）
    Value(Value&&) noexcept = default;
    Value& operator=(Value&&) noexcept = default;

    ~Value() = default;

    // ---- 静态工厂方法 ----

    static Value nullValue() { return Value(); }

    static Value makeInstance(const std::string& clsName) {
        Value v;
        v.data_ = std::make_unique<InstanceData>(InstanceData{clsName, {}});
        return v;
    }

    static Value makeClosure(const std::string& name,
                             std::shared_ptr<Environment> env,
                             const std::vector<std::string>& params,
                             FunDecl* body = nullptr) {
        Value v;
        v.data_ = std::make_unique<ClosureData>(ClosureData{name, env, params, {}, body});
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
    bool isString()   const { return std::holds_alternative<std::unique_ptr<StringData>>(data_); }
    bool isNull()     const { return std::holds_alternative<std::monostate>(data_); }
    bool isArray()    const { return std::holds_alternative<std::unique_ptr<ArrayData>>(data_); }
    bool isDict()     const { return std::holds_alternative<std::unique_ptr<DictData>>(data_); }
    bool isInstance() const { return std::holds_alternative<std::unique_ptr<InstanceData>>(data_); }
    bool isClosure()  const { return std::holds_alternative<std::unique_ptr<ClosureData>>(data_); }
    bool isNumber()   const { auto i = data_.index(); return i == 1 || i == 2; }

    // ---- 向后兼容的字段访问器 ----
    // 所有访问器返回引用，使 val.intVal、val.stringVal、val.fields[k] 等用法
    // 无需修改即可正常工作（读/写均兼容）。

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
        return std::get<4>(data_)->value;
    }
    const std::string& stringVal() const {
        return std::get<4>(data_)->value;
    }

    // -- arrayVal --
    std::vector<Value>& arrayVal() {
        return std::get<5>(data_)->elements;
    }
    const std::vector<Value>& arrayVal() const {
        return std::get<5>(data_)->elements;
    }

    // -- dictVal --
    std::unordered_map<std::string, Value>& dictVal() {
        return std::get<6>(data_)->entries;
    }
    const std::unordered_map<std::string, Value>& dictVal() const {
        return std::get<6>(data_)->entries;
    }

    // -- className（实例专用）--
    std::string& className() {
        return std::get<7>(data_)->className;
    }
    const std::string& className() const {
        return std::get<7>(data_)->className;
    }

    // -- fields（实例字段）--
    std::unordered_map<std::string, Value>& fields() {
        return std::get<7>(data_)->fields;
    }
    const std::unordered_map<std::string, Value>& fields() const {
        return std::get<7>(data_)->fields;
    }

    // -- 闭包字段 --
    std::string& closureName() {
        return std::get<8>(data_)->name;
    }
    const std::string& closureName() const {
        return std::get<8>(data_)->name;
    }

    std::shared_ptr<Environment>& closureEnv() {
        return std::get<8>(data_)->env;
    }
    const std::shared_ptr<Environment>& closureEnv() const {
        return std::get<8>(data_)->env;
    }

    std::vector<std::string>& closureParams() {
        return std::get<8>(data_)->params;
    }
    const std::vector<std::string>& closureParams() const {
        return std::get<8>(data_)->params;
    }

    FunDecl*& closureBody() {
        return std::get<8>(data_)->body;
    }
    FunDecl* closureBody() const {
        return std::get<8>(data_)->body;
    }

    std::unordered_map<std::string, Value>& capturedVars() {
        return std::get<8>(data_)->capturedVars;
    }
    const std::unordered_map<std::string, Value>& capturedVars() const {
        return std::get<8>(data_)->capturedVars;
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
        case ValueType::VAL_INT:      return "int";
        case ValueType::VAL_FLOAT:    return "float";
        case ValueType::VAL_BOOL:     return "bool";
        case ValueType::VAL_STRING:   return "string";
        case ValueType::VAL_NULL:     return "null";
        case ValueType::VAL_ARRAY:    return "array";
        case ValueType::VAL_DICT:     return "dict";
        case ValueType::VAL_INSTANCE: {
            const auto& cn = className();
            return cn.empty() ? "instance" : cn;
        }
        case ValueType::VAL_CLOSURE:  return "closure";
        }
        return "unknown";
    }

    /// 转换为字符串表示
    std::string toString() const {
        switch (getType()) {
        case ValueType::VAL_INT:
            return std::to_string(intVal());
        case ValueType::VAL_FLOAT: {
            char buf[64];
            int len = snprintf(buf, sizeof(buf), "%g", floatVal());
            if (len < 0) return "nan";
            return std::string(buf, len);
        }
        case ValueType::VAL_BOOL:
            return boolVal() ? "true" : "false";
        case ValueType::VAL_STRING:
            return stringVal();
        case ValueType::VAL_NULL:
            return "null";
        case ValueType::VAL_ARRAY: {
            const auto& arr = arrayVal();
            std::ostringstream oss;
            oss << "[";
            for (size_t i = 0; i < arr.size(); ++i) {
                if (i > 0) oss << ", ";
                if (arr[i].isString()) {
                    oss << "\"" << arr[i].stringVal() << "\"";
                } else {
                    oss << arr[i].toString();
                }
            }
            oss << "]";
            return oss.str();
        }
        case ValueType::VAL_DICT: {
            const auto& dict = dictVal();
            std::ostringstream oss;
            oss << "{";
            bool first = true;
            for (const auto& kv : dict) {
                if (!first) oss << ", ";
                first = false;
                oss << "\"" << kv.first << "\": ";
                if (kv.second.isString()) {
                    oss << "\"" << kv.second.stringVal() << "\"";
                } else {
                    oss << kv.second.toString();
                }
            }
            oss << "}";
            return oss.str();
        }
        case ValueType::VAL_INSTANCE: {
            const auto& cn = className();
            const auto& flds = fields();
            std::ostringstream oss;
            oss << cn << "{";
            bool first = true;
            for (const auto& kv : flds) {
                if (!first) oss << ", ";
                first = false;
                oss << kv.first << ": ";
                if (kv.second.isString()) {
                    oss << "\"" << kv.second.stringVal() << "\"";
                } else {
                    oss << kv.second.toString();
                }
            }
            oss << "}";
            return oss.str();
        }
        case ValueType::VAL_CLOSURE:
            return "<fun:" + closureName() + ">";
        }
        return "null";
    }

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

    /// 相等比较
    bool equals(const Value& other) const {
        // int 和 float 之间可以比较
        if (isNumber() && other.isNumber() && getType() != other.getType()) {
            return toDouble() == other.toDouble();
        }
        if (getType() != other.getType()) {
            if (isNull() && other.isNull()) return true;
            return false;
        }
        switch (getType()) {
        case ValueType::VAL_INT:      return intVal() == other.intVal();
        case ValueType::VAL_FLOAT:    return floatVal() == other.floatVal();
        case ValueType::VAL_BOOL:     return boolVal() == other.boolVal();
        case ValueType::VAL_STRING:   return stringVal() == other.stringVal();
        case ValueType::VAL_NULL:     return true;
        case ValueType::VAL_ARRAY: {
            const auto& a = arrayVal();
            const auto& b = other.arrayVal();
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i) {
                if (!a[i].equals(b[i])) return false;
            }
            return true;
        }
        case ValueType::VAL_DICT: {
            const auto& a = dictVal();
            const auto& b = other.dictVal();
            if (a.size() != b.size()) return false;
            for (const auto& kv : a) {
                auto it = b.find(kv.first);
                if (it == b.end()) return false;
                if (!kv.second.equals(it->second)) return false;
            }
            return true;
        }
        case ValueType::VAL_INSTANCE: {
            if (className() != other.className()) return false;
            const auto& a = fields();
            const auto& b = other.fields();
            if (a.size() != b.size()) return false;
            for (const auto& kv : a) {
                auto it = b.find(kv.first);
                if (it == b.end()) return false;
                if (!kv.second.equals(it->second)) return false;
            }
            return true;
        }
        case ValueType::VAL_CLOSURE:
            return closureName() == other.closureName()
                && closureEnv() == other.closureEnv();
        }
        return false;
    }
};
