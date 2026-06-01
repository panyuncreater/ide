#pragma once

#include <string>
#include <unordered_map>
#include <variant>
#include <sstream>
#include <vector>
#include <memory>

// 前向声明 Environment（避免循环依赖）
class Environment;

// ============================================================
// Value 运行时值类型
// ============================================================

/// 值类型枚举
enum class ValueType {
    VAL_INT,
    VAL_FLOAT,
    VAL_BOOL,
    VAL_STRING,
    VAL_NULL,
    VAL_ARRAY,       // 数组
    VAL_DICT,        // 字典
    VAL_INSTANCE,    // 类实例
    VAL_CLOSURE      // 闭包
};

/// 运行时值结构体
struct Value {
    ValueType type = ValueType::VAL_NULL;
    int intVal = 0;
    double floatVal = 0.0;
    bool boolVal = false;
    std::string stringVal;
    std::vector<Value> arrayVal;                                    // 数组
    std::unordered_map<std::string, Value> dictVal;                 // 字典
    std::string className;                                          // 实例的类名
    std::unordered_map<std::string, Value> fields;                  // 实例字段

    // 闭包字段
    std::string closureName;
    std::shared_ptr<Environment> closureEnv;
    std::vector<std::string> closureParams;

    Value() = default;

    // 整型构造
    explicit Value(int v) : type(ValueType::VAL_INT), intVal(v) {}

    // 浮点构造
    explicit Value(double v) : type(ValueType::VAL_FLOAT), floatVal(v) {}

    // 布尔构造
    explicit Value(bool v) : type(ValueType::VAL_BOOL), boolVal(v) {}

    // 字符串构造
    explicit Value(const std::string& v) : type(ValueType::VAL_STRING), stringVal(v) {}

    // 数组构造
    explicit Value(const std::vector<Value>& v) : type(ValueType::VAL_ARRAY), arrayVal(v) {}

    // 字典构造
    explicit Value(const std::unordered_map<std::string, Value>& v) : type(ValueType::VAL_DICT), dictVal(v) {}

    // 空值静态工厂
    static Value nullValue() {
        Value v;
        v.type = ValueType::VAL_NULL;
        return v;
    }

    /// 创建类实例
    static Value makeInstance(const std::string& clsName) {
        Value v;
        v.type = ValueType::VAL_INSTANCE;
        v.className = clsName;
        return v;
    }

    /// 创建闭包值
    static Value makeClosure(const std::string& name,
                             std::shared_ptr<Environment> env,
                             const std::vector<std::string>& params) {
        Value v;
        v.type = ValueType::VAL_CLOSURE;
        v.closureName = name;
        v.closureEnv = env;
        v.closureParams = params;
        return v;
    }

    bool isInt() const { return type == ValueType::VAL_INT; }
    bool isFloat() const { return type == ValueType::VAL_FLOAT; }
    bool isBool() const { return type == ValueType::VAL_BOOL; }
    bool isString() const { return type == ValueType::VAL_STRING; }
    bool isNull() const { return type == ValueType::VAL_NULL; }
    bool isArray() const { return type == ValueType::VAL_ARRAY; }
    bool isDict() const { return type == ValueType::VAL_DICT; }
    bool isInstance() const { return type == ValueType::VAL_INSTANCE; }
    bool isClosure() const { return type == ValueType::VAL_CLOSURE; }
    bool isNumber() const { return type == ValueType::VAL_INT || type == ValueType::VAL_FLOAT; }

    /// 转换为 double（用于数值运算）
    double toDouble() const {
        if (type == ValueType::VAL_INT) return static_cast<double>(intVal);
        if (type == ValueType::VAL_FLOAT) return floatVal;
        return 0.0;
    }

    /// 获取类型名称字符串
    std::string typeName() const {
        switch (type) {
        case ValueType::VAL_INT: return "int";
        case ValueType::VAL_FLOAT: return "float";
        case ValueType::VAL_BOOL: return "bool";
        case ValueType::VAL_STRING: return "string";
        case ValueType::VAL_NULL: return "null";
        case ValueType::VAL_ARRAY: return "array";
        case ValueType::VAL_DICT: return "dict";
        case ValueType::VAL_INSTANCE: return className.empty() ? "instance" : className;
        case ValueType::VAL_CLOSURE: return "closure";
        }
        return "unknown";
    }

    /// 转换为字符串表示
    std::string toString() const {
        switch (type) {
        case ValueType::VAL_INT: {
            return std::to_string(intVal);
        }
        case ValueType::VAL_FLOAT: {
            // 去掉多余的零
            std::ostringstream oss;
            oss << floatVal;
            return oss.str();
        }
        case ValueType::VAL_BOOL:
            return boolVal ? "true" : "false";
        case ValueType::VAL_STRING:
            return stringVal;
        case ValueType::VAL_NULL:
            return "null";
        case ValueType::VAL_ARRAY: {
            // 输出 [1, 2, 3]
            std::ostringstream oss;
            oss << "[";
            for (size_t i = 0; i < arrayVal.size(); ++i) {
                if (i > 0) oss << ", ";
                // 字符串元素加引号
                if (arrayVal[i].isString()) {
                    oss << "\"" << arrayVal[i].stringVal << "\"";
                } else {
                    oss << arrayVal[i].toString();
                }
            }
            oss << "]";
            return oss.str();
        }
        case ValueType::VAL_DICT: {
            // 输出 {"a": 1}
            std::ostringstream oss;
            oss << "{";
            bool first = true;
            for (const auto& kv : dictVal) {
                if (!first) oss << ", ";
                first = false;
                oss << "\"" << kv.first << "\": ";
                if (kv.second.isString()) {
                    oss << "\"" << kv.second.stringVal << "\"";
                } else {
                    oss << kv.second.toString();
                }
            }
            oss << "}";
            return oss.str();
        }
        case ValueType::VAL_INSTANCE: {
            // 输出 ClassName{field1: val1, field2: val2}
            std::ostringstream oss;
            oss << className << "{";
            bool first = true;
            for (const auto& kv : fields) {
                if (!first) oss << ", ";
                first = false;
                oss << kv.first << ": ";
                if (kv.second.isString()) {
                    oss << "\"" << kv.second.stringVal << "\"";
                } else {
                    oss << kv.second.toString();
                }
            }
            oss << "}";
            return oss.str();
        }
        case ValueType::VAL_CLOSURE:
            return "<fun:" + closureName + ">";
        }
        return "null";
    }

    /// 判断真值
    bool isTruthy() const {
        switch (type) {
        case ValueType::VAL_INT:    return intVal != 0;
        case ValueType::VAL_FLOAT:  return floatVal != 0.0;
        case ValueType::VAL_BOOL:   return boolVal;
        case ValueType::VAL_STRING: return !stringVal.empty();
        case ValueType::VAL_NULL:   return false;
        case ValueType::VAL_ARRAY:  return true;
        case ValueType::VAL_DICT:   return true;
        case ValueType::VAL_INSTANCE: return true;
        case ValueType::VAL_CLOSURE: return true;
        }
        return false;
    }

    /// 相等比较
    bool equals(const Value& other) const {
        if (type != other.type) {
            // int 和 float 之间可以比较
            if (isNumber() && other.isNumber()) {
                return toDouble() == other.toDouble();
            }
            // null 与 null 比较
            if (isNull() && other.isNull()) return true;
            return false;
        }
        switch (type) {
        case ValueType::VAL_INT:    return intVal == other.intVal;
        case ValueType::VAL_FLOAT:  return floatVal == other.floatVal;
        case ValueType::VAL_BOOL:   return boolVal == other.boolVal;
        case ValueType::VAL_STRING: return stringVal == other.stringVal;
        case ValueType::VAL_NULL:   return true;
        case ValueType::VAL_ARRAY: {
            // 数组比较元素
            if (arrayVal.size() != other.arrayVal.size()) return false;
            for (size_t i = 0; i < arrayVal.size(); ++i) {
                if (!arrayVal[i].equals(other.arrayVal[i])) return false;
            }
            return true;
        }
        case ValueType::VAL_DICT: {
            // 字典比较 key-value
            if (dictVal.size() != other.dictVal.size()) return false;
            for (const auto& kv : dictVal) {
                auto it = other.dictVal.find(kv.first);
                if (it == other.dictVal.end()) return false;
                if (!kv.second.equals(it->second)) return false;
            }
            return true;
        }
        case ValueType::VAL_INSTANCE: {
            // 实例比较：同类名 + 同字段
            if (className != other.className) return false;
            if (fields.size() != other.fields.size()) return false;
            for (const auto& kv : fields) {
                auto it = other.fields.find(kv.first);
                if (it == other.fields.end()) return false;
                if (!kv.second.equals(it->second)) return false;
            }
            return true;
        }
        case ValueType::VAL_CLOSURE: {
            // 闭包比较：同名 + 同环境指针
            if (closureName != other.closureName) return false;
            if (closureEnv != other.closureEnv) return false;
            return true;
        }
        }
        return false;
    }
};
