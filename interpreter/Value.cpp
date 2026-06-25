// ============================================================
// Value.cpp — Value 的 equalsImpl / toStringImpl 实现
// ------------------------------------------------------------
// S6 fix: 从 Value.h 提取，减小头文件体积（726→约 450 行）。
// 这两个方法是 Value 中最大的实现块（约 240 行），
// 提取到 .cpp 后 Value.h 仅保留接口声明。
// ============================================================

#include "interpreter/Value.h"
#include "interpreter/NumericUtils.h"
#include "common/RuntimeLimits.h"
#include <unordered_set>
#include <sstream>
#include <cstdio>
#include <cmath>
#include <cassert>

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
        const auto& a = arrayVal();
        const auto& b = other.arrayVal();
        if (a.size() != b.size()) return false;
        // P1-6 fix: 环检测（与 toStringImpl 一致）
        if (visited) {
            const void* ptrA = reinterpret_cast<const void*>(a.data());
            const void* ptrB = reinterpret_cast<const void*>(b.data());
            // 使用 data() 指针作为身份标识；若已访问过则视为相等（避免无限递归）
            if (!visited->insert(ptrA).second) return true;
            if (!visited->insert(ptrB).second) return true;
        }
        for (size_t i = 0; i < a.size(); ++i) {
            if (!a[i].equalsImpl(b[i], visited, depth + 1)) {
                if (visited) {
                    visited->erase(reinterpret_cast<const void*>(a.data()));
                    visited->erase(reinterpret_cast<const void*>(b.data()));
                }
                return false;
            }
        }
        if (visited) {
            visited->erase(reinterpret_cast<const void*>(a.data()));
            visited->erase(reinterpret_cast<const void*>(b.data()));
        }
        return true;
    }
    case ValueType::VAL_DICT: {
        const auto& a = dictVal();
        const auto& b = other.dictVal();
        if (a.size() != b.size()) return false;
        // P1-6 fix: 环检测
        if (visited) {
            const void* ptrA = reinterpret_cast<const void*>(&a);
            const void* ptrB = reinterpret_cast<const void*>(&b);
            if (!visited->insert(ptrA).second) return true;
            if (!visited->insert(ptrB).second) return true;
        }
        for (const auto& kv : a) {
            auto it = b.find(kv.first);
            if (it == b.end()) {
                if (visited) {
                    visited->erase(reinterpret_cast<const void*>(&a));
                    visited->erase(reinterpret_cast<const void*>(&b));
                }
                return false;
            }
            if (!kv.second.equalsImpl(it->second, visited, depth + 1)) {
                if (visited) {
                    visited->erase(reinterpret_cast<const void*>(&a));
                    visited->erase(reinterpret_cast<const void*>(&b));
                }
                return false;
            }
        }
        if (visited) {
            visited->erase(reinterpret_cast<const void*>(&a));
            visited->erase(reinterpret_cast<const void*>(&b));
        }
        return true;
    }
    case ValueType::VAL_INSTANCE: {
        if (className() != other.className()) return false;
        const auto& a = fields();
        const auto& b = other.fields();
        if (a.size() != b.size()) return false;
        // P1-6 fix: 环检测
        if (visited) {
            const void* ptrA = reinterpret_cast<const void*>(&a);
            const void* ptrB = reinterpret_cast<const void*>(&b);
            if (!visited->insert(ptrA).second) return true;
            if (!visited->insert(ptrB).second) return true;
        }
        for (const auto& kv : a) {
            auto it = b.find(kv.first);
            if (it == b.end()) {
                if (visited) {
                    visited->erase(reinterpret_cast<const void*>(&a));
                    visited->erase(reinterpret_cast<const void*>(&b));
                }
                return false;
            }
            if (!kv.second.equalsImpl(it->second, visited, depth + 1)) {
                if (visited) {
                    visited->erase(reinterpret_cast<const void*>(&a));
                    visited->erase(reinterpret_cast<const void*>(&b));
                }
                return false;
            }
        }
        if (visited) {
            visited->erase(reinterpret_cast<const void*>(&a));
            visited->erase(reinterpret_cast<const void*>(&b));
        }
        return true;
    }
    case ValueType::VAL_CLOSURE:
        // P2-14 fix: expired env 误判相等防护
        // 双方 env 均失效时按 ClosureData 身份比较，避免 nullptr==nullptr 误判
        {
            const auto& cd1 = std::get<8>(data_);
            const auto& cd2 = std::get<8>(other.data_);
            bool env1Expired = cd1->env.expired();
            bool env2Expired = cd2->env.expired();
            if (env1Expired && env2Expired) {
                // 双方 env 均失效，按 ClosureData 指针身份比较
                return cd1.get() == cd2.get();
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
    // B5 fix: 深度保护，防止 [[[[...]]]] 线性嵌套导致栈溢出
    // visited 仅防真环（insert/erase 路径检测），depth 防线性深度
    if (depth >= MAX_TOSTRING_DEPTH) return "[...too deep]";
    switch (getType()) {
    case ValueType::VAL_INT:
        return std::to_string(intVal());
    case ValueType::VAL_FLOAT: {
        char buf[64];
        int len = snprintf(buf, sizeof(buf), "%.17g", floatVal());  // FMT-R1 fix: 17位有效数字保证 double 往返无损
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
        const auto& ptr = std::get<5>(data_);
        assert(ptr && "toString on null ArrayData");
        if (!visited.insert(ptr.get()).second) return "[cycle]";
        const auto& arr = ptr->elements;
        std::ostringstream oss;
        oss << "[";
        for (size_t i = 0; i < arr.size(); ++i) {
            if (i > 0) oss << ", ";
            if (arr[i].isString()) {
                oss << "\"" << arr[i].stringVal() << "\"";
            } else {
                oss << arr[i].toStringImpl(visited, depth + 1);
            }
        }
        oss << "]";
        visited.erase(ptr.get());
        return oss.str();
    }
    case ValueType::VAL_DICT: {
        const auto& ptr = std::get<6>(data_);
        assert(ptr && "toString on null DictData");
        if (!visited.insert(ptr.get()).second) return "[cycle]";
        const auto& dict = ptr->entries;
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
                oss << kv.second.toStringImpl(visited, depth + 1);
            }
        }
        oss << "}";
        visited.erase(ptr.get());
        return oss.str();
    }
    case ValueType::VAL_INSTANCE: {
        const auto& ptr = std::get<7>(data_);
        assert(ptr && "toString on null InstanceData");
        if (!visited.insert(ptr.get()).second) return "[cycle]";
        const auto& cn = ptr->className;
        const auto& flds = ptr->fields;
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
                oss << kv.second.toStringImpl(visited, depth + 1);
            }
        }
        oss << "}";
        visited.erase(ptr.get());
        return oss.str();
    }
    case ValueType::VAL_CLOSURE:
        return "<fun:" + closureName() + ">";
    }
    return "null";
}
