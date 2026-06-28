// ============================================================
// Value.cpp — Value 的 equalsImpl / toStringImpl 实现
// ------------------------------------------------------------
// PERF-12: 从 std::variant<shared_ptr<XData>> 迁移到 NaNBox + intrusive refcount。
// 所有 std::get<I>(data_) 调用替换为 box_.asPtr<XData>() 指针转换。
// visited 集合使用 RefCounted* 原始指针作为身份标识。
// ============================================================

#include "interpreter/Value.h"
#include "interpreter/NumericUtils.h"
#include "common/RuntimeLimits.h"
#include <unordered_set>
#include <cstdio>
#include <cmath>

// ============================================================
// equalsImpl — 相等比较实现（含环检测和深度保护）
// ============================================================

bool Value::equalsImpl(const Value& other,
                       std::unordered_set<const void*>* visited,
                       int depth) const {
    // P1-7 fix: 深度保护，防止 [[[[...]]]] 线性嵌套导致栈溢出
    if (depth >= MAX_EQUALS_DEPTH) return false;

    // int 和 float 之间可以比较（M3 fix: 避免大整数精度丢失）
    if (isNumber() && other.isNumber() && getType() != other.getType()) {
        if (isInt() && other.isFloat()) {
            double d = other.floatVal();
            // double 不是整数值、或超出 int64_t 范围、或 NaN → 不可能相等
            if (std::isnan(d) || std::isinf(d)) return false;
            double intPart;
            if (std::modf(d, &intPart) != 0.0) return false;  // 有小数部分
            if (OverflowCheck::doubleToIntOverflow(d)) return false;
            return intVal() == static_cast<int64_t>(d);
        }
        if (isFloat() && other.isInt()) {
            double d = floatVal();
            if (std::isnan(d) || std::isinf(d)) return false;
            double intPart;
            if (std::modf(d, &intPart) != 0.0) return false;
            if (OverflowCheck::doubleToIntOverflow(d)) return false;
            return static_cast<int64_t>(d) == other.intVal();
        }
        // P3 fix: 此行原为不可达死代码，保留以防御未来新增数值类型
        return toDouble() == other.toDouble();
    }
    if (getType() != other.getType()) {
        return false;
    }
    switch (getType()) {
    case ValueType::VAL_INT:      return intVal() == other.intVal();
    case ValueType::VAL_FLOAT:    return floatVal() == other.floatVal();
    case ValueType::VAL_BOOL:     return boolVal() == other.boolVal();
    case ValueType::VAL_STRING:   return stringVal() == other.stringVal();
    case ValueType::VAL_NULL:     return true;
    case ValueType::VAL_ARRAY: {
        // PERF-12: 直接通过 NaNBox 指针访问 ArrayData
        auto* aPtr = box_.asPtr<ArrayData>();
        auto* bPtr = other.box_.asPtr<ArrayData>();
        const auto& a = aPtr->elements;
        const auto& b = bPtr->elements;
        if (a.size() != b.size()) return false;
        // P1-6 fix: 环检测（使用 ArrayData* 指针作为身份标识）
        if (visited) {
            if (!visited->insert(aPtr).second) return true;
            if (!visited->insert(bPtr).second) return true;
        }
        for (size_t i = 0; i < a.size(); ++i) {
            if (!a[i].equalsImpl(b[i], visited, depth + 1)) {
                if (visited) {
                    visited->erase(aPtr);
                    visited->erase(bPtr);
                }
                return false;
            }
        }
        if (visited) {
            visited->erase(aPtr);
            visited->erase(bPtr);
        }
        return true;
    }
    case ValueType::VAL_DICT: {
        auto* aPtr = box_.asPtr<DictData>();
        auto* bPtr = other.box_.asPtr<DictData>();
        const auto& a = aPtr->entries;
        const auto& b = bPtr->entries;
        if (a.size() != b.size()) return false;
        if (visited) {
            if (!visited->insert(aPtr).second) return true;
            if (!visited->insert(bPtr).second) return true;
        }
        for (const auto& kv : a) {
            auto it = b.find(kv.first);
            if (it == b.end()) {
                if (visited) {
                    visited->erase(aPtr);
                    visited->erase(bPtr);
                }
                return false;
            }
            if (!kv.second.equalsImpl(it->second, visited, depth + 1)) {
                if (visited) {
                    visited->erase(aPtr);
                    visited->erase(bPtr);
                }
                return false;
            }
        }
        if (visited) {
            visited->erase(aPtr);
            visited->erase(bPtr);
        }
        return true;
    }
    case ValueType::VAL_INSTANCE: {
        if (className() != other.className()) return false;
        auto* aPtr = box_.asPtr<InstanceData>();
        auto* bPtr = other.box_.asPtr<InstanceData>();
        const auto& a = aPtr->fields;
        const auto& b = bPtr->fields;
        if (a.size() != b.size()) return false;
        if (visited) {
            if (!visited->insert(aPtr).second) return true;
            if (!visited->insert(bPtr).second) return true;
        }
        for (const auto& kv : a) {
            auto it = b.find(kv.first);
            if (it == b.end()) {
                if (visited) {
                    visited->erase(aPtr);
                    visited->erase(bPtr);
                }
                return false;
            }
            if (!kv.second.equalsImpl(it->second, visited, depth + 1)) {
                if (visited) {
                    visited->erase(aPtr);
                    visited->erase(bPtr);
                }
                return false;
            }
        }
        if (visited) {
            visited->erase(aPtr);
            visited->erase(bPtr);
        }
        return true;
    }
    case ValueType::VAL_CLOSURE:
        // P2-14 fix: expired env 误判相等防护
        // PERF-12: 直接通过 NaNBox 指针访问 ClosureData
        // 设计决策：闭包相等性按 name + env 比较（TestValue.cpp 的
        // ClosureEqualsSameNameAndEnv/DifferentNameNotEqual/DifferentEnvNotEqual 三组
        // 测试用例明确锁定此契约）。同名同环境但函数体不同的闭包判为相等是已知限制，
        // 容器场景下闭包作为键的情况极少，权衡后保留现有语义。
        {
            auto* cd1 = box_.asPtr<ClosureData>();
            auto* cd2 = other.box_.asPtr<ClosureData>();
            bool env1Expired = cd1->env.expired();
            bool env2Expired = cd2->env.expired();
            if (env1Expired && env2Expired) {
                // 双方 env 均失效，按 ClosureData 指针身份比较
                return cd1 == cd2;
            }
            return closureName() == other.closureName()
                && closureEnv() == other.closureEnv();
        }
    }
    return false;
}

// ============================================================
// toStringImpl — 字符串化实现（含环检测和深度保护）
// ============================================================

std::string Value::toStringImpl(std::unordered_set<const void*>& visited, int depth) const {
    // B5 fix: 深度保护
    if (depth >= MAX_TOSTRING_DEPTH) return "[...too deep]";
    switch (getType()) {
    case ValueType::VAL_INT: {
        // Perf-Finding2: 用 std::to_chars 替代 std::to_string（避免 locale 查询）
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
    case ValueType::VAL_ARRAY: {
        // PERF-12: 直接通过 NaNBox 指针访问 ArrayData
        auto* ptr = box_.asPtr<ArrayData>();
        if (!visited.insert(ptr).second) return "[cycle]";
        const auto& arr = ptr->elements;
        std::string result;
        result.reserve(arr.size() * 8 + 2);
        result += "[";
        for (size_t i = 0; i < arr.size(); ++i) {
            if (i > 0) result += ", ";
            if (arr[i].isString()) {
                result += "\"";
                result += arr[i].stringVal();
                result += "\"";
            } else {
                result += arr[i].toStringImpl(visited, depth + 1);
            }
        }
        result += "]";
        visited.erase(ptr);
        return result;
    }
    case ValueType::VAL_DICT: {
        auto* ptr = box_.asPtr<DictData>();
        if (!visited.insert(ptr).second) return "[cycle]";
        const auto& dict = ptr->entries;
        std::string result;
        result.reserve(dict.size() * 16 + 2);
        result += "{";
        bool first = true;
        for (const auto& kv : dict) {
            if (!first) result += ", ";
            first = false;
            result += "\"";
            result += kv.first;
            result += "\": ";
            if (kv.second.isString()) {
                result += "\"";
                result += kv.second.stringVal();
                result += "\"";
            } else {
                result += kv.second.toStringImpl(visited, depth + 1);
            }
        }
        result += "}";
        visited.erase(ptr);
        return result;
    }
    case ValueType::VAL_INSTANCE: {
        auto* ptr = box_.asPtr<InstanceData>();
        if (!visited.insert(ptr).second) return "[cycle]";
        const auto& cn = ptr->className;
        const auto& flds = ptr->fields;
        std::string result;
        result.reserve(cn.size() + flds.size() * 16 + 2);
        result += cn;
        result += "{";
        bool first = true;
        for (const auto& kv : flds) {
            if (!first) result += ", ";
            first = false;
            result += kv.first;
            result += ": ";
            if (kv.second.isString()) {
                result += "\"";
                result += kv.second.stringVal();
                result += "\"";
            } else {
                result += kv.second.toStringImpl(visited, depth + 1);
            }
        }
        result += "}";
        visited.erase(ptr);
        return result;
    }
    case ValueType::VAL_CLOSURE:
        return "<fun:" + closureName() + ">";
    }
    return "null";
}
