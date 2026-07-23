// ============================================================
// Value.cpp — Value 的 equalsImpl / toStringImpl 实现
// ------------------------------------------------------------
// PERF-12: 从 std::variant<shared_ptr<XData>> 迁移到 NaNBox + intrusive refcount。
// 所有 std::get<I>(data_) 调用替换为 box_.asPtr<XData>() 指针转换。
// visited 集合使用 RefCounted* 原始指针作为身份标识。
// ============================================================

#include "interpreter/Value.h"
#include "common/RuntimeLimits.h"
#include "interpreter/NumericUtils.h"
#include <cmath>
#include <cstdio>
#include <unordered_set>

// ============================================================
// R114 阶段 2：Value JSON 序列化辅助
// ============================================================
namespace {

/// JSON 字符串转义（RFC 8259）。转义 `"`、`\`、控制字符（< 0x20）。
std::string escapeJsonString(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        unsigned char uc = static_cast<unsigned char>(c);
        switch (uc) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (uc < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", uc);
                out += buf;
            } else {
                out += c;
            }
            break;
        }
    }
    return out;
}

} // namespace

// ============================================================
// equalsImpl — 相等比较实现（含环检测和深度保护）
// ============================================================
//
// BUG-012 审计结论（2026-07-18）：visited 集合使用 RefCounted* 原始指针作为
// 身份标识是正确且安全的，无需改用唯一 ID。原因：
// 1. equalsImpl 是 const 成员函数，仅读取容器内容（const auto& a = aPtr->elements），
//    不调用任何非 const 方法 → 不会触发 ensureUnique() → 不会发生 COW detach。
// 2. visited 的生命周期是单次 equals() 调用（thread_local 局部集，调用前 clear），
//    期间所有 RefCounted 对象的指针稳定不变。
// 3. COW detach 只在 WRITE 操作（push/insert/字段赋值等）触发，而 equals 不写。
// 4. 唯一理论风险是另一线程在比较期间修改容器触发 COW detach——但 MiniLang 容器
//    非线程安全，并发访问本身就是 UB，与 visited 标识方式无关。
// 因此原始指针作为身份标识在 equalsImpl 上下文中是不变量，无需引入额外 ID 开销。
// toStringImpl 同理（仅读取，不触发 COW）。

bool Value::equalsImpl(const Value& other, std::unordered_set<const void*>* visited, int depth) const {
    // P1-7 fix: 深度保护，防止 [[[[...]]]] 线性嵌套导致栈溢出
    if (depth >= MAX_EQUALS_DEPTH)
        return false;

    // int 和 float 之间可以比较（M3 fix: 避免大整数精度丢失）
    if (isNumber() && other.isNumber() && getType() != other.getType()) {
        if (isInt() && other.isFloat()) {
            double d = other.floatVal();
            // double 不是整数值、或超出 int64_t 范围、或 NaN → 不可能相等
            if (std::isnan(d) || std::isinf(d))
                return false;
            double intPart;
            if (std::modf(d, &intPart) != 0.0)
                return false; // 有小数部分
            if (OverflowCheck::doubleToIntOverflow(d))
                return false;
            return intVal() == static_cast<int64_t>(d);
        }
        if (isFloat() && other.isInt()) {
            double d = floatVal();
            if (std::isnan(d) || std::isinf(d))
                return false;
            double intPart;
            if (std::modf(d, &intPart) != 0.0)
                return false;
            if (OverflowCheck::doubleToIntOverflow(d))
                return false;
            return static_cast<int64_t>(d) == other.intVal();
        }
        // P3 fix: 此行原为不可达死代码，保留以防御未来新增数值类型
        return toDouble() == other.toDouble();
    }
    if (getType() != other.getType()) {
        return false;
    }
    // R133-B fix: 按 ValueType 分组分派到 7 个独立 helper，主函数保留前置守卫
    // （深度保护 + 跨类型数值比较 + 类型一致性检查），helper 共享状态全成员（box_）。
    switch (getType()) {
    case ValueType::VAL_INT:
    case ValueType::VAL_FLOAT:
    case ValueType::VAL_BOOL:
    case ValueType::VAL_STRING:
    case ValueType::VAL_NULL:
        return equalsImplScalar(other);
    case ValueType::VAL_ARRAY:
        return equalsImplArray(other, visited, depth);
    case ValueType::VAL_DICT:
        return equalsImplDict(other, visited, depth);
    case ValueType::VAL_INSTANCE:
        return equalsImplInstance(other, visited, depth);
    case ValueType::VAL_TUPLE:
        return equalsImplTuple(other, visited, depth);
    case ValueType::VAL_ENUM_VARIANT:
        return equalsImplEnumVariant(other, visited, depth);
    case ValueType::VAL_CLOSURE:
    case ValueType::VAL_CHANNEL:
    case ValueType::VAL_MUTEX:
    case ValueType::VAL_RWLOCK:
    case ValueType::VAL_THREAD:
    case ValueType::VAL_COROUTINE: // R164 协程/生成器：引用相等（叶子对象）
        return equalsImplLeafObject(other);
    }
    return false;
}

// ============================================================
// R133-B fix: equalsImpl 分组 helper 实现
// ------------------------------------------------------------
// 按 ValueType 分组：标量无环检测（直接值比较）；ARRAY/DICT/INSTANCE/TUPLE/ENUM_VARIANT
// 是 cyclic 容器，需 visited 集合防环；CLOSURE/SYNC 是叶子对象（无递归子元素）。
// 所有 helper 共享 box_ 成员，签名 (const Value&, visited, depth) 与原 case body 一致。
// ============================================================

bool Value::equalsImplScalar(const Value& other) const {
    // INT/FLOAT/BOOL/STRING/NULL：直接值比较，无环检测需求
    // 跨 int/float 类型已在主 dispatcher 前置守卫中处理，此处类型必然相同
    switch (getType()) {
    case ValueType::VAL_INT:
        return intVal() == other.intVal();
    case ValueType::VAL_FLOAT:
        return floatVal() == other.floatVal();
    case ValueType::VAL_BOOL:
        return boolVal() == other.boolVal();
    case ValueType::VAL_STRING:
        return stringVal() == other.stringVal();
    case ValueType::VAL_NULL:
        return true;
    default:
        return false;
    }
}

bool Value::equalsImplArray(const Value& other, std::unordered_set<const void*>* visited, int depth) const {
    // PERF-12: 直接通过 NaNBox 指针访问 ArrayData
    auto* aPtr = box_.asPtr<ArrayData>();
    auto* bPtr = other.box_.asPtr<ArrayData>();
    const auto& a = aPtr->elements;
    const auto& b = bPtr->elements;
    if (a.size() != b.size())
        return false;
    // P1-6 fix: 环检测（使用 ArrayData* 指针作为身份标识）
    if (visited) {
        if (!visited->insert(aPtr).second)
            return true;
        if (!visited->insert(bPtr).second)
            return true;
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

bool Value::equalsImplDict(const Value& other, std::unordered_set<const void*>* visited, int depth) const {
    auto* aPtr = box_.asPtr<DictData>();
    auto* bPtr = other.box_.asPtr<DictData>();
    const auto& a = aPtr->entries;
    const auto& b = bPtr->entries;
    if (a.size() != b.size())
        return false;
    if (visited) {
        if (!visited->insert(aPtr).second)
            return true;
        if (!visited->insert(bPtr).second)
            return true;
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

bool Value::equalsImplInstance(const Value& other, std::unordered_set<const void*>* visited, int depth) const {
    if (className() != other.className())
        return false;
    auto* aPtr = box_.asPtr<InstanceData>();
    auto* bPtr = other.box_.asPtr<InstanceData>();
    const auto& a = aPtr->fields;
    const auto& b = bPtr->fields;
    if (a.size() != b.size())
        return false;
    if (visited) {
        if (!visited->insert(aPtr).second)
            return true;
        if (!visited->insert(bPtr).second)
            return true;
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

bool Value::equalsImplTuple(const Value& other, std::unordered_set<const void*>* visited, int depth) const {
    // R98 元组与解构：按元素逐位比较（与数组语义一致，但 immutable）。
    auto* aPtr = box_.asPtr<TupleData>();
    auto* bPtr = other.box_.asPtr<TupleData>();
    const auto& a = aPtr->elements;
    const auto& b = bPtr->elements;
    if (a.size() != b.size())
        return false;
    if (visited) {
        if (!visited->insert(aPtr).second)
            return true;
        if (!visited->insert(bPtr).second)
            return true;
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

bool Value::equalsImplEnumVariant(const Value& other, std::unordered_set<const void*>* visited, int depth) const {
    // R99 枚举与 ADT：同 enum + 同 variant + 同 fields 判等。
    // 与 tuple 类似的逐元素比较，但额外校验 enumName/variantName。
    // 同 enum 的不同 variant（Color.Red vs Color.Green）必不相等；
    // 不同 enum 同名 variant（Color.None vs Option.None）必不相等。
    auto* aPtr = box_.asPtr<EnumVariantData>();
    auto* bPtr = other.box_.asPtr<EnumVariantData>();
    if (aPtr->enumName != bPtr->enumName || aPtr->variantName != bPtr->variantName)
        return false;
    const auto& a = aPtr->fields;
    const auto& b = bPtr->fields;
    if (a.size() != b.size())
        return false;
    if (visited) {
        if (!visited->insert(aPtr).second)
            return true;
        if (!visited->insert(bPtr).second)
            return true;
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

bool Value::equalsImplLeafObject(const Value& other) const {
    // CLOSURE 与同步对象（CHANNEL/MUTEX/RWLOCK/THREAD）都是叶子对象，无递归子元素，
    // 无需环检测。CLOSURE 按 functionId 比较，SYNC 对象按底层 Inner 指针比较。
    switch (getType()) {
    case ValueType::VAL_CLOSURE:
        // L3 fix（2026-07-19）: 闭包相等性改为按 functionId 比较。
        // 原契约按 name + env 比较，导致"同名同环境但函数体不同的闭包误判相等"（已知限制 L3）。
        // 引入全局单调递增的 functionId（makeClosure 时分配，COW 拷贝保留），
        // 同一闭包值的拷贝判等，独立 makeClosure 创建的闭包不判等。
        // P2-14 fix: expired env 防护已不再需要——functionId 是值语义不受 env 生命周期影响。
        // PERF-12: 直接通过 NaNBox 指针访问 ClosureData。
        {
            auto* cd1 = box_.asPtr<ClosureData>();
            auto* cd2 = other.box_.asPtr<ClosureData>();
            return cd1->functionId == cd2->functionId;
        }
    // R136 同步对象相等性：同一底层 Inner（指针相等）即为相等
    // 语义：两个 channel Value 相等当且仅当它们指向同一通道（共享同一 Inner）
    case ValueType::VAL_CHANNEL:
        return box_.asPtr<ChannelData>()->inner == other.box_.asPtr<ChannelData>()->inner;
    case ValueType::VAL_MUTEX:
        return box_.asPtr<MutexData>()->inner == other.box_.asPtr<MutexData>()->inner;
    case ValueType::VAL_RWLOCK:
        return box_.asPtr<RwLockData>()->inner == other.box_.asPtr<RwLockData>()->inner;
    case ValueType::VAL_THREAD:
        return box_.asPtr<ThreadData>()->inner == other.box_.asPtr<ThreadData>()->inner;
    // R164 协程/生成器：引用相等（同一 CoroutineData 指针即为相等）
    case ValueType::VAL_COROUTINE:
        return box_.asPtr<CoroutineData>() == other.box_.asPtr<CoroutineData>();
    default:
        return false;
    }
}

// ============================================================
// toStringImpl — 字符串化实现（含环检测和深度保护）
// ============================================================

std::string Value::toStringImpl(std::unordered_set<const void*>& visited, int depth) const {
    // B5 fix: 深度保护
    if (depth >= MAX_TOSTRING_DEPTH)
        return "[...too deep]";
    // R133-C fix: 按 ValueType 分组分派到 7 个独立 helper，主函数保留深度保护守卫。
    // CLOSURE/SYNC 在顶层 Value::toString() 已走快速路径返回字符串字面量；
    // 但当容器元素递归调用 toStringImpl 时仍会进入本函数，故需 toStringImplLeafObject
    // 处理这些叶子类型，否则会落到 default: return "null" 导致 [closure()] 输出 [null]。
    // helper 共享状态全成员（box_），标量 helper 无环检测无需 visited/depth 参数。
    switch (getType()) {
    case ValueType::VAL_INT:
    case ValueType::VAL_FLOAT:
    case ValueType::VAL_BOOL:
    case ValueType::VAL_STRING:
    case ValueType::VAL_NULL:
        return toStringImplScalar();
    case ValueType::VAL_ARRAY:
        return toStringImplArray(visited, depth);
    case ValueType::VAL_DICT:
        return toStringImplDict(visited, depth);
    case ValueType::VAL_INSTANCE:
        return toStringImplInstance(visited, depth);
    case ValueType::VAL_TUPLE:
        return toStringImplTuple(visited, depth);
    case ValueType::VAL_ENUM_VARIANT:
        return toStringImplEnumVariant(visited, depth);
    case ValueType::VAL_CLOSURE:
    case ValueType::VAL_CHANNEL:
    case ValueType::VAL_MUTEX:
    case ValueType::VAL_RWLOCK:
    case ValueType::VAL_THREAD:
    case ValueType::VAL_COROUTINE: // R164 协程/生成器：叶子对象
        return toStringImplLeafObject();
    // 未知类型兜底（防御未来新增类型未在拆分 helper 中处理）
    default:
        return "null";
    }
}

// ============================================================
// R133-C fix: toStringImpl 分组 helper 实现
// ------------------------------------------------------------
// 按 ValueType 分组：标量无环检测（直接格式化）；ARRAY/DICT/INSTANCE/TUPLE/ENUM_VARIANT
// 是 cyclic 容器，需 visited 集合防环。helper 共享 box_ 成员，标量 helper 无参，
// 容器 helper 签名 (visited, depth) 与原 case body 一致。
// ============================================================

std::string Value::toStringImplScalar() const {
    // INT/FLOAT/BOOL/STRING/NULL：直接格式化，无环检测需求
    switch (getType()) {
    case ValueType::VAL_INT: {
        // Perf-Finding2: 用 std::to_chars 替代 std::to_string（避免 locale 查询）
        char buf[32];
        auto res = std::to_chars(buf, buf + sizeof(buf), intVal());
        return std::string(buf, res.ptr);
    }
    case ValueType::VAL_FLOAT: {
        // AUDIT-BUG-C2 fix: 用 std::to_chars 替代 snprintf——locale-independent，
        // 与 Value.h toString() 路径保持一致。此函数用于容器 toString 时嵌套 float 输出。
        char buf[64];
        auto res = std::to_chars(buf, buf + sizeof(buf), box_.asFloat(), std::chars_format::general, 17);
        if (res.ec != std::errc{})
            return "nan";
        return std::string(buf, res.ptr);
    }
    case ValueType::VAL_BOOL:
        return box_.asBool() ? "true" : "false";
    case ValueType::VAL_STRING:
        return stringVal();
    case ValueType::VAL_NULL:
        return "null";
    default:
        return "null";
    }
}

std::string Value::toStringImplArray(std::unordered_set<const void*>& visited, int depth) const {
    // PERF-12: 直接通过 NaNBox 指针访问 ArrayData
    auto* ptr = box_.asPtr<ArrayData>();
    if (!visited.insert(ptr).second)
        return "[cycle]";
    const auto& arr = ptr->elements;
    std::string result;
    result.reserve(arr.size() * 8 + 2);
    result += "[";
    for (size_t i = 0; i < arr.size(); ++i) {
        if (i > 0)
            result += ", ";
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

std::string Value::toStringImplDict(std::unordered_set<const void*>& visited, int depth) const {
    auto* ptr = box_.asPtr<DictData>();
    if (!visited.insert(ptr).second)
        return "[cycle]";
    const auto& dict = ptr->entries;
    std::string result;
    result.reserve(dict.size() * 16 + 2);
    result += "{";
    bool first = true;
    for (const auto& kv : dict) {
        if (!first)
            result += ", ";
        first = false;
        // L4 fix: 键可能是 string/int/bool/float，string 键加引号，其他类型直接打印
        if (std::holds_alternative<std::string>(kv.first)) {
            result += "\"";
            result += std::get<std::string>(kv.first);
            result += "\": ";
        } else {
            result += dictKeyToString(kv.first);
            result += ": ";
        }
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

std::string Value::toStringImplInstance(std::unordered_set<const void*>& visited, int depth) const {
    auto* ptr = box_.asPtr<InstanceData>();
    if (!visited.insert(ptr).second)
        return "[cycle]";
    const auto& cn = ptr->className;
    const auto& flds = ptr->fields;
    std::string result;
    result.reserve(cn.size() + flds.size() * 16 + 2);
    result += cn;
    result += "{";
    bool first = true;
    for (const auto& kv : flds) {
        if (!first)
            result += ", ";
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

std::string Value::toStringImplTuple(std::unordered_set<const void*>& visited, int depth) const {
    // R98 元组与解构：元组字符串化为 (e1, e2, ...)
    auto* ptr = box_.asPtr<TupleData>();
    if (!visited.insert(ptr).second)
        return "[cycle]";
    const auto& tup = ptr->elements;
    std::string result;
    result.reserve(tup.size() * 8 + 2);
    result += "(";
    for (size_t i = 0; i < tup.size(); ++i) {
        if (i > 0)
            result += ", ";
        if (tup[i].isString()) {
            result += "\"";
            result += tup[i].stringVal();
            result += "\"";
        } else {
            result += tup[i].toStringImpl(visited, depth + 1);
        }
    }
    // 单元素元组需保留尾逗号以区别于分组表达式：(1,) vs (1)
    if (tup.size() == 1)
        result += ",";
    result += ")";
    visited.erase(ptr);
    return result;
}

std::string Value::toStringImplEnumVariant(std::unordered_set<const void*>& visited, int depth) const {
    // R99 枚举与 ADT：字符串化为 EnumName.VariantName 或 EnumName.VariantName(arg1, arg2, ...)
    // 对齐 Rust Debug 格式（Color.Red → "Color.Red"，Option.Some(42) → "Option.Some(42)"）。
    // 字符串 fields 加引号（与 array/dict/tuple 一致的 repr 风格）。
    auto* ptr = box_.asPtr<EnumVariantData>();
    if (!visited.insert(ptr).second)
        return "[cycle]";
    std::string result;
    result.reserve(ptr->enumName.size() + ptr->variantName.size() + ptr->fields.size() * 8 + 4);
    result += ptr->enumName;
    result += ".";
    result += ptr->variantName;
    if (!ptr->fields.empty()) {
        result += "(";
        for (size_t i = 0; i < ptr->fields.size(); ++i) {
            if (i > 0)
                result += ", ";
            if (ptr->fields[i].isString()) {
                result += "\"";
                result += ptr->fields[i].stringVal();
                result += "\"";
            } else {
                result += ptr->fields[i].toStringImpl(visited, depth + 1);
            }
        }
        // 单字段 variant 保留尾逗号以对齐单元素元组语义（避免 Option.Some(42) 与
        // Option.Some((42,)) 歧义——但实际两者 Value 类型不同，无需尾逗号区分；
        // 此处不加尾逗号，对齐 Rust Debug 格式 Option.Some(42) 而非 Option.Some(42,)）
        result += ")";
    }
    visited.erase(ptr);
    return result;
}

std::string Value::toStringImplLeafObject() const {
    // CLOSURE 与同步对象（CHANNEL/MUTEX/RWLOCK/THREAD）都是叶子对象，无递归子元素，
    // 无需环检测与 visited 集合。CLOSURE 走 closureName() 格式化，SYNC 对象用固定字面量。
    // 注意：Value.h toString() 顶层快速路径已处理这些类型，但当容器元素递归调用
    // toStringImpl 时（如 [closure()] / {k: thread()}）仍会进入本函数，故提取为独立 helper
    // 以恢复原 dispatcher 中这些 case 的字符串字面量（R133-C fix）。
    switch (getType()) {
    case ValueType::VAL_CLOSURE:
        return "<fun:" + closureName() + ">";
    case ValueType::VAL_CHANNEL:
        return "<channel>";
    case ValueType::VAL_MUTEX:
        return "<mutex>";
    case ValueType::VAL_RWLOCK:
        return "<rwlock>";
    case ValueType::VAL_THREAD:
        return "<thread>";
    // R164 协程/生成器
    case ValueType::VAL_COROUTINE:
        return "<coroutine>";
    default:
        return "null";
    }
}

// ============================================================
// toJsonImpl — JSON 序列化实现（R114 阶段 2）
// ------------------------------------------------------------
// 格式约定：
//   - 标量（int/float/bool/null）输出 JSON 原生类型
//   - 字符串输出 JSON 转义字符串
//   - 容器输出带 "_type" 标签的对象，便于回放器重建：
//       array   → {"_type":"array","elements":[...]}
//       dict    → {"_type":"dict","entries":[{"key":K,"value":V},...]}
//       instance→ {"_type":"instance","className":"Foo","fields":[{"name":N,"value":V}]}
//       tuple   → {"_type":"tuple","elements":[...]}
//       enum    → {"_type":"enum","enumName":"E","variantName":"V","fields":[...]}
//       closure → {"_type":"closure","name":"foo"}
//   - 环引用 → {"_type":"cycle"}
//   - 深度超限 → {"_type":"too_deep"}
// ============================================================

std::string Value::toJsonImpl(std::unordered_set<const void*>& visited, int depth) const {
    if (depth >= MAX_TOSTRING_DEPTH)
        return "{\"_type\":\"too_deep\"}";
    switch (getType()) {
    case ValueType::VAL_INT: {
        char buf[32];
        auto res = std::to_chars(buf, buf + sizeof(buf), intVal());
        return std::string(buf, res.ptr);
    }
    case ValueType::VAL_FLOAT: {
        char buf[64];
        auto res = std::to_chars(buf, buf + sizeof(buf), box_.asFloat(), std::chars_format::general, 17);
        if (res.ec != std::errc{})
            return "null";
        return std::string(buf, res.ptr);
    }
    case ValueType::VAL_BOOL:
        return box_.asBool() ? "true" : "false";
    case ValueType::VAL_NULL:
        return "null";
    case ValueType::VAL_STRING:
        return "\"" + escapeJsonString(stringVal()) + "\"";
    case ValueType::VAL_CLOSURE:
        return "{\"_type\":\"closure\",\"name\":\"" + escapeJsonString(closureName()) + "\"}";
    case ValueType::VAL_ARRAY: {
        auto* ptr = box_.asPtr<ArrayData>();
        if (!visited.insert(ptr).second)
            return "{\"_type\":\"cycle\"}";
        const auto& arr = ptr->elements;
        std::string result = "{\"_type\":\"array\",\"elements\":[";
        for (size_t i = 0; i < arr.size(); ++i) {
            if (i > 0)
                result += ",";
            result += arr[i].toJsonImpl(visited, depth + 1);
        }
        result += "]}";
        visited.erase(ptr);
        return result;
    }
    case ValueType::VAL_DICT: {
        auto* ptr = box_.asPtr<DictData>();
        if (!visited.insert(ptr).second)
            return "{\"_type\":\"cycle\"}";
        const auto& dict = ptr->entries;
        std::string result = "{\"_type\":\"dict\",\"entries\":[";
        bool first = true;
        for (const auto& kv : dict) {
            if (!first)
                result += ",";
            first = false;
            result += "{\"key\":";
            // 键可能是 string/int/bool/float，统一用 dictKeyToValue 递归序列化
            result += dictKeyToValue(kv.first).toJsonImpl(visited, depth + 1);
            result += ",\"value\":";
            result += kv.second.toJsonImpl(visited, depth + 1);
            result += "}";
        }
        result += "]}";
        visited.erase(ptr);
        return result;
    }
    case ValueType::VAL_INSTANCE: {
        auto* ptr = box_.asPtr<InstanceData>();
        if (!visited.insert(ptr).second)
            return "{\"_type\":\"cycle\"}";
        std::string result = "{\"_type\":\"instance\",\"className\":\"";
        result += escapeJsonString(ptr->className);
        result += "\",\"fields\":[";
        bool first = true;
        for (const auto& kv : ptr->fields) {
            if (!first)
                result += ",";
            first = false;
            result += "{\"name\":\"";
            result += escapeJsonString(kv.first);
            result += "\",\"value\":";
            result += kv.second.toJsonImpl(visited, depth + 1);
            result += "}";
        }
        result += "]}";
        visited.erase(ptr);
        return result;
    }
    case ValueType::VAL_TUPLE: {
        auto* ptr = box_.asPtr<TupleData>();
        if (!visited.insert(ptr).second)
            return "{\"_type\":\"cycle\"}";
        const auto& tup = ptr->elements;
        std::string result = "{\"_type\":\"tuple\",\"elements\":[";
        for (size_t i = 0; i < tup.size(); ++i) {
            if (i > 0)
                result += ",";
            result += tup[i].toJsonImpl(visited, depth + 1);
        }
        result += "]}";
        visited.erase(ptr);
        return result;
    }
    case ValueType::VAL_ENUM_VARIANT: {
        auto* ptr = box_.asPtr<EnumVariantData>();
        if (!visited.insert(ptr).second)
            return "{\"_type\":\"cycle\"}";
        std::string result = "{\"_type\":\"enum\",\"enumName\":\"";
        result += escapeJsonString(ptr->enumName);
        result += "\",\"variantName\":\"";
        result += escapeJsonString(ptr->variantName);
        result += "\",\"fields\":[";
        for (size_t i = 0; i < ptr->fields.size(); ++i) {
            if (i > 0)
                result += ",";
            result += ptr->fields[i].toJsonImpl(visited, depth + 1);
        }
        result += "]}";
        visited.erase(ptr);
        return result;
    }
    // R136 同步对象 JSON 序列化（叶子节点，无环检测需求）
    case ValueType::VAL_CHANNEL:
        return "{\"_type\":\"channel\"}";
    case ValueType::VAL_MUTEX:
        return "{\"_type\":\"mutex\"}";
    case ValueType::VAL_RWLOCK:
        return "{\"_type\":\"rwlock\"}";
    case ValueType::VAL_THREAD:
        return "{\"_type\":\"thread\"}";
    // R164 协程/生成器 JSON 序列化（叶子节点）
    case ValueType::VAL_COROUTINE:
        return "{\"_type\":\"coroutine\"}";
    }
    return "null";
}
