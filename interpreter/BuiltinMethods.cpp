// ============================================================
// BuiltinMethods.cpp - 内置方法实现
// ============================================================
// 从 Interpreter::visitMethodCall 中提取的数组/字典/字符串内置方法处理。
// 错误通过 throw RuntimeError(msg, line, col) 抛出，与 Interpreter::runtimeError 语义一致。
//
// 共享纯函数层（executeSharedLen / executeSharedArrayContains / executeSharedDictHas）
// 不抛异常，供 Interpreter 和 VM 共用。下方的 handle*Method 在调用共享函数后，
// 自行将 Result<Value>::is_err() 转换为 RuntimeError 抛出（通过 to_runtime_error()）。

#include "interpreter/BuiltinMethods.h"
#include "common/RuntimeLimits.h"     // S1 fix: MAX_RANGE 统一定义
#include "common/Utf8Utils.h"         // P0-4 fix: UTF-8 码位工具
#include "interpreter/ErrorFormat.h"  // P3 fix: runtimeErrorFmt 替代 std::to_string 拼接
#include "interpreter/NumericUtils.h" // BUG9 fix: 溢出检查
#include <cctype>
#include <charconv>
#include <cstdint>
#include <string>
#include <unordered_set>

// ============================================================
// #20 fix: 内建方法名枚举分发（从 VM::classifyBuiltinMethod 提取为共享自由函数）
// ============================================================
/// 将方法名分类为 BuiltinMethod 枚举（供 VM/Interpreter 预先分派）。
/// 优化：先按名字长度 switch 过滤，再在固定长度组内做少量字符串比较，
/// 避免对长名字链表式逐个比较。未知/不支持的名字返回 UNKNOWN。
/// 注意：此分类不区分接收者类型（数组/字典/字符串同名方法），
/// 类型归属在 handle*Method 的注册表中最终决定。
BuiltinMethod classifyBuiltinMethod(const std::string& name) {
    // 按长度快速筛选，减少不必要的字符串比较
    switch (name.size()) {
    case 3:
        if (name == "pop")
            return BuiltinMethod::ARR_POP;
        if (name == "len")
            return BuiltinMethod::ARR_LEN; // 数组/字典/字符串共用
        if (name == "has")
            return BuiltinMethod::DICT_HAS;
        if (name == "get")
            return BuiltinMethod::DICT_GET;
        if (name == "set")
            return BuiltinMethod::DICT_SET;
        break;
    case 4:
        if (name == "push")
            return BuiltinMethod::ARR_PUSH;
        if (name == "keys")
            return BuiltinMethod::DICT_KEYS;
        if (name == "trim")
            return BuiltinMethod::STR_TRIM;
        if (name == "join")
            return BuiltinMethod::ARR_JOIN;
        break;
    case 5:
        if (name == "upper")
            return BuiltinMethod::STR_UPPER;
        if (name == "lower")
            return BuiltinMethod::STR_LOWER;
        if (name == "split")
            return BuiltinMethod::STR_SPLIT;
        break;
    case 6:
        if (name == "values")
            return BuiltinMethod::DICT_VALUES;
        if (name == "remove")
            return BuiltinMethod::ARR_REMOVE; // 数组/字典共用
        if (name == "substr")
            return BuiltinMethod::STR_SUBSTR;
        break;
    case 7:
        if (name == "replace")
            return BuiltinMethod::STR_REPLACE;
        if (name == "indexOf")
            return BuiltinMethod::STR_INDEX_OF;
        break;
    case 8:
        if (name == "contains")
            return BuiltinMethod::ARR_CONTAINS; // 数组/字典共用
        if (name == "endsWith")
            return BuiltinMethod::STR_ENDS_WITH;
        break;
    case 9:
        break;
    case 10:
        if (name == "startsWith")
            return BuiltinMethod::STR_STARTS_WITH;
        break;
    }
    return BuiltinMethod::UNKNOWN;
}

// ============================================================
// P1-5 fix: 参数数量检查辅助函数
// ============================================================
// 消除 BuiltinMethods.cpp 中 20 处重复的 argCount 校验样板。
// 匹配时返回 ok(null)，不匹配时返回 err。
// 调用方: if (auto r = checkExact("len", argCount, 0, line, column); r.is_err()) return r;

namespace {

Result<Value> checkExact(const char* name, size_t argCount, size_t expected, int line, int column) {
    if (argCount != expected) {
        return Result<Value>::err(ErrorFormat::format("%s 期望 %zu 个参数，但传入了 %zu 个", name, expected, argCount),
                                  line, column);
    }
    return Result<Value>::ok(Value::nullValue());
}

Result<Value> checkRange(const char* name, size_t argCount, size_t minExpected, size_t maxExpected, int line,
                         int column) {
    if (argCount < minExpected || argCount > maxExpected) {
        return Result<Value>::err(
            ErrorFormat::format("%s 期望 %zu-%zu 个参数，但传入了 %zu 个", name, minExpected, maxExpected, argCount),
            line, column);
    }
    return Result<Value>::ok(Value::nullValue());
}

} // anonymous namespace

// ============================================================
// 共享纯函数实现（供 Interpreter 和 VM 共用）
// ============================================================

Result<Value> executeSharedLen(const Value& obj, const Value* args, size_t argCount, int line, int column) {
    if (auto r = checkExact("len", argCount, 0, line, column); r.is_err())
        return r;
    if (obj.isArray()) {
        return Result<Value>::ok(Value(static_cast<int64_t>(obj.arrayVal().size())));
    } else if (obj.isDict()) {
        return Result<Value>::ok(Value(static_cast<int64_t>(obj.dictVal().size())));
    } else if (obj.isString()) {
        // M6 fix: 按 UTF-8 码位计数而非字节数
        // perf1 fix: 使用 Value::codepointCount() 带缓存，避免循环中 O(n²) 重复扫描
        return Result<Value>::ok(Value(obj.codepointCount()));
    } else {
        return Result<Value>::err("len 不支持类型 " + obj.typeName(), line, column);
    }
}

Result<Value> executeSharedArrayContains(const Value& arr, const Value* args, size_t argCount, int line, int column) {
    if (argCount != 1) {
        return Result<Value>::err("contains 期望 1 个参数", line, column);
    }
    bool found = false;
    for (const auto& elem : arr.arrayVal()) {
        if (elem.equals(args[0])) {
            found = true;
            break;
        }
    }
    return Result<Value>::ok(Value(found));
}

Result<Value> executeSharedDictHas(const Value& dict, const std::string& method, const Value* args, size_t argCount,
                                   int line, int column) {
    if (argCount != 1) {
        return Result<Value>::err(method + " 期望 1 个参数(键)", line, column);
    }
    // Perf-Finding: 缓存 find 迭代器，避免 dictVal() 二次调用与同键二次 hash 查找
    const auto& entries = dict.dictVal();
    auto it = entries.find(args[0].toString());
    return Result<Value>::ok(Value(it != entries.end()));
}

// ============================================================
// 字符串方法共享层实现（供 Interpreter 和 VM 共用）
// ============================================================

Result<Value> executeSharedStrStartsWith(const Value& str, const Value* args, size_t argCount, int line, int column) {
    if (auto r = checkExact("startsWith", argCount, 1, line, column); r.is_err())
        return r;
    const std::string& prefix = args[0].toString();
    const std::string& s = str.stringVal();
    return Result<Value>::ok(Value(s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0));
}

Result<Value> executeSharedStrEndsWith(const Value& str, const Value* args, size_t argCount, int line, int column) {
    if (auto r = checkExact("endsWith", argCount, 1, line, column); r.is_err())
        return r;
    const std::string& suffix = args[0].toString();
    const std::string& s = str.stringVal();
    return Result<Value>::ok(
        Value(s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0));
}

Result<Value> executeSharedStrSubstr(const Value& str, const Value* args, size_t argCount, int line, int column) {
    if (argCount < 1 || argCount > 2) {
        return Result<Value>::err(
            ErrorFormat::format("substr 期望 1-2 个参数(起始[, 长度])，但传入了 %zu 个", argCount), line, column);
    }
    if (!args[0].isInt()) {
        return Result<Value>::err("substr 起始位置必须是整数", line, column);
    }
    int64_t start = args[0].intVal();
    const std::string& s = str.stringVal();

    // BUG 1.1 fix: 将码位索引转换为字节索引，与 len()/indexOf() 的码位语义一致
    // P0-4 fix: 使用 Utf8 工具函数替代重复的内联 lambda

    int64_t totalCp = str.codepointCount(); // perf1 fix: 带缓存的码位计数
    // AUDIT-BUG-F11 fix: 负 start 报错（与 len<0 一致），而非静默返回空串。
    // 原 start<0 静默返回 ""，len<0 报错，错误处理不对称。用户无法区分"空串"与"负索引错误"。
    if (start < 0) {
        return Result<Value>::err("substr 起始位置不能为负数", line, column);
    }
    if (start > totalCp) {
        return Result<Value>::ok(Value(std::string("")));
    }

    size_t byteStart = Utf8::codepointToByteIndex(s, start);

    if (argCount == 2) {
        if (!args[1].isInt()) {
            return Result<Value>::err("substr 长度必须是整数", line, column);
        }
        int64_t len = args[1].intVal();
        if (len < 0) {
            return Result<Value>::err("substr 长度不能为负数", line, column);
        }
        // #27 fix: 原 codepointToByteIndex(s, start + len) 从头重新扫描到 start+len，
        // 重复扫描了前 start 个码位。改为从 byteStart 起推进 len 个码位，单次扫描。
        size_t byteEnd = byteStart;
        int64_t remaining = len;
        while (byteEnd < s.size() && remaining > 0) {
            byteEnd += Utf8::byteLength(static_cast<unsigned char>(s[byteEnd]));
            --remaining;
        }
        if (byteEnd > s.size())
            byteEnd = s.size();
        return Result<Value>::ok(Value(s.substr(byteStart, byteEnd - byteStart)));
    } else {
        return Result<Value>::ok(Value(s.substr(byteStart)));
    }
}

Result<Value> executeSharedStrIndexOf(const Value& str, const Value* args, size_t argCount, int line, int column) {
    if (auto r = checkExact("indexOf", argCount, 1, line, column); r.is_err())
        return r;
    // M2 fix: 返回 UTF-8 字符位置而非字节位置
    const std::string& s = str.stringVal();
    const std::string& needle = args[0].toString();
    size_t bytePos = s.find(needle);
    if (bytePos == std::string::npos) {
        return Result<Value>::ok(Value(static_cast<int64_t>(-1)));
    } else {
        // P0-4 fix: 使用 Utf8 工具函数替代内联码位计算
        return Result<Value>::ok(Value(Utf8::byteToCodepointIndex(s, bytePos)));
    }
}

// S1 fix: 共享 replace 实现 — 单遍构建 O(N)，消除 VM 侧 O(N²) Bug
Result<Value> executeSharedStrReplace(const Value& str, const Value* args, size_t argCount, int line, int column) {
    if (argCount != 2) {
        return Result<Value>::err(ErrorFormat::format("replace 期望 2 个参数(旧串, 新串)，但传入了 %zu 个", argCount),
                                  line, column);
    }
    const std::string& src = str.stringVal();
    std::string from = args[0].toString();
    std::string to = args[1].toString();
    if (from.empty()) {
        return Result<Value>::ok(Value(std::string(src)));
    }

    // 预扫统计匹配数以估算容量
    size_t matchCount = 0;
    size_t scanPos = 0;
    while ((scanPos = src.find(from, scanPos)) != std::string::npos) {
        ++matchCount;
        scanPos += from.size();
    }

    // 单遍构建结果字符串
    std::string result;
    size_t estimated = src.size();
    if (to.size() > from.size()) {
        estimated += matchCount * (to.size() - from.size());
    }
    result.reserve(estimated);

    size_t lastEnd = 0;
    size_t pos = 0;
    while ((pos = src.find(from, pos)) != std::string::npos) {
        result.append(src, lastEnd, pos - lastEnd);
        result.append(to);
        pos += from.size();
        lastEnd = pos;
    }
    result.append(src, lastEnd, std::string::npos);
    return Result<Value>::ok(Value(std::move(result)));
}

// S3 fix: 字符串/字典/数组非变异方法共享层实现
// 消除 Interpreter 与 VM 间的双重实现，统一参数校验和内存预分配策略

Result<Value> executeSharedStrUpper(const Value& str, const Value* args, size_t argCount, int line, int column) {
    if (auto r = checkExact("upper", argCount, 0, line, column); r.is_err())
        return r;
    const std::string& s = str.stringVal();
    std::string result;
    result.reserve(s.size());
    for (char c : s) {
        result += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return Result<Value>::ok(Value(std::move(result)));
}

Result<Value> executeSharedStrLower(const Value& str, const Value* args, size_t argCount, int line, int column) {
    if (auto r = checkExact("lower", argCount, 0, line, column); r.is_err())
        return r;
    const std::string& s = str.stringVal();
    std::string result;
    result.reserve(s.size());
    for (char c : s) {
        result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return Result<Value>::ok(Value(std::move(result)));
}

Result<Value> executeSharedStrSplit(const Value& str, const Value* args, size_t argCount, int line, int column) {
    if (auto r = checkRange("split", argCount, 0, 1, line, column); r.is_err())
        return r;
    const std::string& s = str.stringVal();
    std::string sep = (argCount == 1) ? args[0].toString() : " ";
    if (sep.empty()) {
        return Result<Value>::err("split 的分隔符不能为空字符串", line, column);
    }

    // 预扫描统计分隔符数量以预估容量
    size_t estCount = 1;
    for (size_t pos = s.find(sep); pos != std::string::npos; pos = s.find(sep, pos + sep.size())) {
        ++estCount;
    }

    std::vector<Value> parts;
    parts.reserve(estCount);
    size_t start = 0;
    size_t pos = s.find(sep);
    while (pos != std::string::npos) {
        parts.emplace_back(Value(s.substr(start, pos - start)));
        start = pos + sep.size();
        pos = s.find(sep, start);
    }
    parts.emplace_back(Value(s.substr(start)));
    return Result<Value>::ok(Value(std::move(parts)));
}

Result<Value> executeSharedStrTrim(const Value& str, const Value* args, size_t argCount, int line, int column) {
    if (auto r = checkExact("trim", argCount, 0, line, column); r.is_err())
        return r;
    const std::string& s = str.stringVal();
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return Result<Value>::ok(Value(std::string("")));
    }
    size_t end = s.find_last_not_of(" \t\r\n");
    return Result<Value>::ok(Value(s.substr(start, end - start + 1)));
}

// ARCH-15 fix: str.contains 共享实现（消除 Interpreter/VM 内联重复）
Result<Value> executeSharedStrContains(const Value& str, const Value* args, size_t argCount, int line, int column) {
    if (argCount != 1) {
        return Result<Value>::err("contains 期望 1 个参数", line, column);
    }
    return Result<Value>::ok(Value(str.stringVal().find(args[0].toString()) != std::string::npos));
}

Result<Value> executeSharedDictKeys(const Value& dict, const Value* args, size_t argCount, int line, int column) {
    if (auto r = checkExact("keys", argCount, 0, line, column); r.is_err())
        return r;
    const auto& entries = dict.dictVal();
    std::vector<Value> keys;
    keys.reserve(entries.size());
    for (const auto& kv : entries) {
        keys.emplace_back(Value(kv.first));
    }
    return Result<Value>::ok(Value(std::move(keys)));
}

Result<Value> executeSharedDictValues(const Value& dict, const Value* args, size_t argCount, int line, int column) {
    if (auto r = checkExact("values", argCount, 0, line, column); r.is_err())
        return r;
    const auto& entries = dict.dictVal();
    std::vector<Value> vals;
    vals.reserve(entries.size());
    for (const auto& kv : entries) {
        vals.push_back(kv.second);
    }
    return Result<Value>::ok(Value(std::move(vals)));
}

Result<Value> executeSharedDictGet(const Value& dict, const Value* args, size_t argCount, int line, int column) {
    if (argCount < 1 || argCount > 2) {
        return Result<Value>::err(ErrorFormat::format("get 期望 1-2 个参数(键[, 默认值])，但传入了 %zu 个", argCount),
                                  line, column);
    }
    const auto& entries = dict.dictVal();
    std::string key = args[0].toString();
    auto it = entries.find(key);
    if (it != entries.end()) {
        return Result<Value>::ok(it->second);
    } else if (argCount == 2) {
        return Result<Value>::ok(args[1]);
    } else {
        return Result<Value>::ok(Value::nullValue());
    }
}

Result<Value> executeSharedArrayJoin(const Value& arr, const Value* args, size_t argCount, int line, int column) {
    if (auto r = checkRange("join", argCount, 0, 1, line, column); r.is_err())
        return r;
    std::string sep = (argCount == 1) ? args[0].toString() : "";
    const auto& elements = arr.arrayVal();

    // 容量预估（每个元素平均 16 字节 + 分隔符），上限 64MB 防 OOM
    size_t estimated = elements.size() * (16 + sep.size());
    constexpr size_t MAX_JOIN_RESERVE = 64 * 1024 * 1024;
    if (estimated > MAX_JOIN_RESERVE)
        estimated = MAX_JOIN_RESERVE;

    std::string result;
    result.reserve(estimated);
    for (size_t i = 0; i < elements.size(); ++i) {
        if (i > 0)
            result += sep;
        result += elements[i].toString();
    }
    return Result<Value>::ok(Value(std::move(result)));
}

// ============================================================
// 顶层内置函数共享层实现
// ============================================================

/// 判断 name 是否为顶层内置函数（len/type/str/int/abs/min/max/range/sum）。
/// 用静态 unordered_set 做 O(1) 查找，供调用方在调用 executeSharedBuiltinFunction
/// 前快速识别，避免将用户自定义函数误判为内置。
bool isBuiltinFunction(const std::string& name) {
    // C7 fix: 用 unordered_set 实现 O(1) 查找，替代每次调用 9 次字符串比较
    static const std::unordered_set<std::string> builtinNames = {"len", "type", "str",   "int", "abs",
                                                                 "min", "max",  "range", "sum"};
    return builtinNames.count(name) > 0;
}

// ============================================================
// P0-2 fix: 顶层内置函数注册表模式（替代 9 分支 if-else）
// ============================================================
// 每个内置函数提取为独立函数，统一签名 SharedBuiltinFn，
// 通过 static unordered_map 实现 O(1) 分派。
// 新增内置函数只需在 registry 中添加一行。

namespace {

/// 顶层内置函数处理函数类型：接收 args 数组，返回 Result<Value>
using SharedBuiltinFn = Result<Value> (*)(const Value*, size_t, int, int);

// ---- len(x): 长度（委托 executeSharedLen）----
/// len(x): 返回 x 的长度。x 可为数组/字典/字符串（字符串按 UTF-8 码位数计）。
/// 恰好 1 个参数，多/少均返回 err；实际计算委托 executeSharedLen。
Result<Value> executeBuiltinLen(const Value* args, size_t argCount, int line, int column) {
    if (auto r = checkExact("len", argCount, 1, line, column); r.is_err())
        return r;
    return executeSharedLen(args[0], nullptr, 0, line, column);
}

// ---- type(x): 类型名 ----
/// type(x): 返回 x 的运行时类型名字符串（如 "int"/"array"/"closure"）。
/// 恰好 1 个参数；结果来自 Value::typeName()。
Result<Value> executeBuiltinType(const Value* args, size_t argCount, int line, int column) {
    if (auto r = checkExact("type", argCount, 1, line, column); r.is_err())
        return r;
    return Result<Value>::ok(Value(args[0].typeName()));
}

// ---- str(x): 转字符串 ----
/// str(x): 将 x 转换为字符串表示（与 Value::toString() 一致）。
/// 恰好 1 个参数；用于数值/布尔/容器等转字符串场景。
Result<Value> executeBuiltinStr(const Value* args, size_t argCount, int line, int column) {
    if (auto r = checkExact("str", argCount, 1, line, column); r.is_err())
        return r;
    return Result<Value>::ok(Value(args[0].toString()));
}

// ---- int(x): 转整数 ----
/// int(x): 将 x 转换为整数（int64）。
///   - int 原样返回；float 向零截断（溢出检查）；bool 转 0/1；
///   - 字符串先用 from_chars 解析（避免 locale 依赖），要求整串可解析（前后空白允许、中间不允许），否则 err；
///   - 其他类型不支持，返回 err。溢出与格式错误均以 err 返回而非 UB。
Result<Value> executeBuiltinInt(const Value* args, size_t argCount, int line, int column) {
    if (auto r = checkExact("int", argCount, 1, line, column); r.is_err())
        return r;
    const Value& v = args[0];
    if (v.isInt()) {
        return Result<Value>::ok(v);
    } else if (v.isFloat()) {
        // BUG 9.3 fix: 浮点转整数溢出检查
        double dv = v.floatVal();
        if (OverflowCheck::doubleToIntOverflow(dv)) {
            return Result<Value>::err(ErrorFormat::format("int 转换溢出: %g 超出 int64_t 范围", dv), line, column);
        }
        // 截断小数部分（向零取整，与 C++ static_cast 一致）
        return Result<Value>::ok(Value(static_cast<int64_t>(dv)));
    } else if (v.isBool()) {
        return Result<Value>::ok(Value(static_cast<int64_t>(v.boolVal() ? 1 : 0)));
    } else if (v.isString()) {
        // L-P2 fix: 使用 from_chars 替代 stoll，避免 locale 依赖
        const std::string& s = v.stringVal();
        size_t startIdx = 0;
        while (startIdx < s.size() && std::isspace(static_cast<unsigned char>(s[startIdx])))
            startIdx++;
        if (startIdx >= s.size()) {
            return Result<Value>::err("int 无法将空字符串转换为整数", line, column);
        }
        long long parsed = 0;
        auto [ptr, ec] = std::from_chars(s.data() + startIdx, s.data() + s.size(), parsed);
        if (ec != std::errc()) {
            return Result<Value>::err("int 无法将字符串 \"" + s + "\" 转换为整数", line, column);
        }
        size_t consumed = static_cast<size_t>(ptr - s.data());
        while (consumed < s.size() && std::isspace(static_cast<unsigned char>(s[consumed])))
            consumed++;
        if (consumed != s.size()) {
            return Result<Value>::err("int 无法将字符串 \"" + s + "\" 转换为整数", line, column);
        }
        return Result<Value>::ok(Value(static_cast<int64_t>(parsed)));
    } else {
        return Result<Value>::err("int 不支持类型 " + v.typeName(), line, column);
    }
}

// ---- abs(x): 绝对值 ----
/// abs(x): 返回数值 x 的绝对值。
///   - int: 注意 INT64_MIN 取负会溢出（C++ UB），用 negateOverflow 预检，溢出返回 err；
///   - float: 直接取负；其他类型返回 err。
Result<Value> executeBuiltinAbs(const Value* args, size_t argCount, int line, int column) {
    if (auto r = checkExact("abs", argCount, 1, line, column); r.is_err())
        return r;
    const Value& v = args[0];
    if (v.isInt()) {
        int64_t iv = v.intVal();
        // BUG 9.1 fix: abs(INT64_MIN) 会导致整数溢出（C++ UB）
        if (OverflowCheck::negateOverflow(iv)) {
            return Result<Value>::err("abs 溢出: INT64_MIN 的绝对值无法表示", line, column);
        }
        return Result<Value>::ok(Value(iv < 0 ? -iv : iv));
    } else if (v.isFloat()) {
        double dv = v.floatVal();
        return Result<Value>::ok(Value(dv < 0 ? -dv : dv));
    } else {
        return Result<Value>::err("abs 期望数值参数，实际为 " + v.typeName(), line, column);
    }
}

// ---- min(a, b): 最小值 ----
/// min(a, b): 返回两个数值中的较小者。a、b 必须同为数值类型，否则 err。
/// 同为 int 时做整数比较（无精度损失）；否则都转 double 后比较，返回 double。
Result<Value> executeBuiltinMin(const Value* args, size_t argCount, int line, int column) {
    if (auto r = checkExact("min", argCount, 2, line, column); r.is_err())
        return r;
    const Value& a = args[0];
    const Value& b = args[1];
    if (!a.isNumber() || !b.isNumber()) {
        return Result<Value>::err("min 期望数值参数", line, column);
    }
    if (a.isInt() && b.isInt()) {
        return Result<Value>::ok((a.intVal() <= b.intVal()) ? a : b);
    } else {
        double da = a.toDouble(), db = b.toDouble();
        return Result<Value>::ok(Value(da <= db ? da : db));
    }
}

// ---- max(a, b): 最大值 ----
/// max(a, b): 返回两个数值中的较大者。a、b 必须同为数值类型，否则 err。
/// 同为 int 时做整数比较；否则转 double 后比较，返回 double。
Result<Value> executeBuiltinMax(const Value* args, size_t argCount, int line, int column) {
    if (auto r = checkExact("max", argCount, 2, line, column); r.is_err())
        return r;
    const Value& a = args[0];
    const Value& b = args[1];
    if (!a.isNumber() || !b.isNumber()) {
        return Result<Value>::err("max 期望数值参数", line, column);
    }
    if (a.isInt() && b.isInt()) {
        return Result<Value>::ok((a.intVal() >= b.intVal()) ? a : b);
    } else {
        double da = a.toDouble(), db = b.toDouble();
        return Result<Value>::ok(Value(da >= db ? da : db));
    }
}

// ---- range(n): 生成 [0, 1, ..., n-1] 数组 ----
/// range(n): 生成包含 [0, 1, ..., n-1] 的数组。
/// 参数 n 必须为非负整数（负数 err），且不超过 RuntimeLimits::MAX_RANGE 上限（防 OOM），
/// 否则返回 err。结果预分配容量后逐元素填充。
Result<Value> executeBuiltinRange(const Value* args, size_t argCount, int line, int column) {
    if (auto r = checkExact("range", argCount, 1, line, column); r.is_err())
        return r;
    const Value& v = args[0];
    if (!v.isInt()) {
        return Result<Value>::err("range 期望整数参数，实际为 " + v.typeName(), line, column);
    }
    int64_t n = v.intVal();
    if (n < 0) {
        return Result<Value>::err(ErrorFormat::format("range 参数不能为负数: %lld", static_cast<long long>(n)), line,
                                  column);
    }
    if (n > RuntimeLimits::MAX_RANGE) {
        return Result<Value>::err(
            ErrorFormat::format("range 参数超过上限 %lld", static_cast<long long>(RuntimeLimits::MAX_RANGE)), line,
            column);
    }
    std::vector<Value> elements;
    elements.reserve(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
        elements.emplace_back(Value(i));
    }
    return Result<Value>::ok(Value(std::move(elements)));
}

// ---- sum(arr): 数组元素求和 ----
/// sum(arr): 对数组元素求和。
/// 先判断数组是否全为 int：全 int 时做整数累加（addOverflow 溢出检查，溢出 err）；
/// 否则转 double 累加，遇到非数值元素返回 err。返回 int 或 double。
Result<Value> executeBuiltinSum(const Value* args, size_t argCount, int line, int column) {
    if (auto r = checkExact("sum", argCount, 1, line, column); r.is_err())
        return r;
    const Value& v = args[0];
    if (!v.isArray()) {
        return Result<Value>::err("sum 期望数组参数，实际为 " + v.typeName(), line, column);
    }
    const auto& arr = v.arrayVal();
    bool allInt = true;
    for (const auto& elem : arr) {
        if (!elem.isInt()) {
            allInt = false;
            break;
        }
    }
    if (allInt) {
        int64_t total = 0;
        for (const auto& elem : arr) {
            if (OverflowCheck::addOverflow(total, elem.intVal())) {
                return Result<Value>::err("sum 整数累加溢出", line, column);
            }
            total += elem.intVal();
        }
        return Result<Value>::ok(Value(total));
    } else {
        double total = 0.0;
        for (const auto& elem : arr) {
            if (elem.isNumber()) {
                total += elem.toDouble();
            } else {
                return Result<Value>::err("sum 数组元素包含非数值类型: " + elem.typeName(), line, column);
            }
        }
        return Result<Value>::ok(Value(total));
    }
}

/// 顶层内置函数注册表（延迟初始化，thread-safe since C++11）
const std::unordered_map<std::string, SharedBuiltinFn>& builtinFunctionRegistry() {
    static const std::unordered_map<std::string, SharedBuiltinFn> registry = {
        {"len", executeBuiltinLen}, {"type", executeBuiltinType},   {"str", executeBuiltinStr},
        {"int", executeBuiltinInt}, {"abs", executeBuiltinAbs},     {"min", executeBuiltinMin},
        {"max", executeBuiltinMax}, {"range", executeBuiltinRange}, {"sum", executeBuiltinSum},
    };
    return registry;
}

} // anonymous namespace

/// 顶层内置函数统一入口（供 Interpreter 和 VM 共用）。
/// 通过 builtinFunctionRegistry() 的 O(1) 查找将 funcName 分派到对应处理函数；
/// 未找到时返回 err（未知函数）。每个处理函数内部已完成参数个数校验与类型检查，
/// 失败以 Result<Value>::err 表示，由调用方决定是否转为异常。
Result<Value> executeSharedBuiltinFunction(const std::string& funcName, const Value* args, size_t argCount, int line,
                                           int column) {

    const auto& registry = builtinFunctionRegistry();
    auto it = registry.find(funcName);
    if (it == registry.end()) {
        return Result<Value>::err("未知的内置函数: " + funcName, line, column);
    }
    return it->second(args, argCount, line, column);
}

// ============================================================
// E3 fix: input() 共享实现（供 Interpreter 和 VM 共用）
// ============================================================
// 不走 executeSharedBuiltinFunction 注册表（依赖 callback 跨线程交互）。
// 调用方（Interpreter/VM）注入 inputCallback；callback 抛出的异常会被捕获并附行号。
// WorkerManager.buildInputCallback 超时路径抛 std::runtime_error 表示超时，
// 此处 catch 后转 Result::err，使程序感知中断（替代原静默返回空串）。
Result<Value> executeSharedInput(const std::function<std::string(const std::string&)>& inputCallback, const Value* args,
                                 size_t argCount, int line, int column) {
    if (auto r = checkRange("input", argCount, 0, 1, line, column); r.is_err())
        return r;
    std::string prompt;
    if (argCount == 1) {
        prompt = args[0].toString();
    }
    if (!inputCallback) {
        // 无回调时返回空串（允许非交互式运行不崩溃，保留原行为）
        return Result<Value>::ok(Value(std::string()));
    }
    try {
        std::string userInput = inputCallback(prompt);
        return Result<Value>::ok(Value(std::move(userInput)));
    } catch (const std::exception& e) {
        // E3 fix: callback 抛出异常（如超时）转 Result::err，
        // 让程序能感知中断而非拿到空串继续执行
        return Result<Value>::err(e.what(), line, column);
    }
}

// ============================================================
// S6 fix: 内置方法注册表模式
// ============================================================
// 替代 if-else 字符串比较链，使用 static unordered_map 注册方法名→处理函数。
// 优势：O(1) 平均查找，新增方法只需注册一行，消除长 if-else 链。
//
// 方法分两类：
//   1. 非变异方法：委托共享纯函数（executeSharedXxx），返回 Result<Value>
//   2. 变异方法（push/pop/remove）：直接修改 obj，返回 BuiltinMethodResult
//
// 注册表使用 function 指针，首次调用时初始化（thread-safe since C++11）

namespace {

/// 非变异方法处理函数类型：接收 const Value& + args，返回 Result<Value>
using SharedMethodFn = Result<Value> (*)(const Value&, const Value*, size_t, int, int);

/// 非变异方法注册表（延迟初始化）
const std::unordered_map<std::string, SharedMethodFn>& arraySharedMethods() {
    static const std::unordered_map<std::string, SharedMethodFn> registry = {
        {"len", executeSharedLen},
        {"contains", executeSharedArrayContains},
        {"join", executeSharedArrayJoin},
    };
    return registry;
}

const std::unordered_map<std::string, SharedMethodFn>& dictSharedMethods() {
    static const std::unordered_map<std::string, SharedMethodFn> registry = {
        {"len", executeSharedLen},
        {"keys", executeSharedDictKeys},
        {"values", executeSharedDictValues},
        {"get", executeSharedDictGet},
    };
    return registry;
}

const std::unordered_map<std::string, SharedMethodFn>& stringSharedMethods() {
    static const std::unordered_map<std::string, SharedMethodFn> registry = {
        {"len", executeSharedLen},
        {"upper", executeSharedStrUpper},
        {"lower", executeSharedStrLower},
        {"split", executeSharedStrSplit},
        {"replace", executeSharedStrReplace},
        {"trim", executeSharedStrTrim},
        {"contains", executeSharedStrContains}, // ARCH-15 fix: 补齐 contains 家族
        {"startsWith", executeSharedStrStartsWith},
        {"endsWith", executeSharedStrEndsWith},
        {"substr", executeSharedStrSubstr},
        {"indexOf", executeSharedStrIndexOf},
    };
    return registry;
}

/// 调用共享方法并转换为 BuiltinMethodResult（统一错误处理路径）
BuiltinMethodResult dispatchShared(SharedMethodFn fn, const Value& obj, const Value* args, size_t argCount, int line,
                                   int col) {
    auto sr = fn(obj, args, argCount, line, col);
    if (sr.is_err())
        throw to_runtime_error(sr); // A1 fix: 自由函数模板
    return BuiltinMethodResult(std::move(sr.value()));
}

} // anonymous namespace

// ============================================================
// 数组内置方法
// ============================================================

/// 分发数组内置方法。
/// 设计：变异方法（push/pop/remove）直接就地修改 obj，返回 objectModified=true
/// 以便调用方（Interpreter/VM）知道原对象已变更、无需再取返回值；非变异方法
/// （len/contains/join）通过 arraySharedMethods() 注册表委托共享纯函数实现，
/// 统一经 dispatchShared 将 Result<Value> 转为 BuiltinMethodResult 或抛 RuntimeError。
/// 未知方法名抛出 RuntimeError。方法名由 classifyBuiltinMethod 预先分类。
BuiltinMethodResult BuiltinMethods::handleArrayMethod(const std::string& method, Value& obj,
                                                      const std::vector<Value>& args, int line, int col) {
    // S6 fix: 变异方法直接处理
    if (method == "push") {
        if (args.size() != 1)
            throw RuntimeError("push 期望 1 个参数", line, col);
        obj.arrayVal().push_back(args[0]);
        return BuiltinMethodResult(Value::nullValue(), /*objectModified=*/true);
    }

    if (method == "pop") {
        if (!args.empty())
            throw RuntimeError(ErrorFormat::format("pop 期望 0 个参数，但传入了 %zu 个", args.size()), line, col);
        // AUDIT-BUG-C5 fix: 空数组检查用 const 访问器避免 COW detach。
        // 非 const arrayVal() 在 refCount>1 时会 ensureUnique 深拷贝整个数组，
        // 此处仅为检查 empty() 却触发不必要的克隆。
        if (std::as_const(obj).arrayVal().empty())
            throw RuntimeError("对空数组调用 pop", line, col);
        Value last = std::as_const(obj).arrayVal().back();
        obj.arrayVal().pop_back();
        return BuiltinMethodResult(std::move(last), /*objectModified=*/true);
    }

    if (method == "remove") {
        if (args.size() != 1)
            throw RuntimeError("remove 期望 1 个参数(索引)", line, col);
        if (!args[0].isInt())
            throw RuntimeError("remove 参数必须是整数索引", line, col);
        int64_t idx = args[0].intVal();
        if (idx < 0 || static_cast<size_t>(idx) >= std::as_const(obj).arrayVal().size())
            throw RuntimeError(ErrorFormat::format("数组索引越界: %lld, 有效范围 [0, %zu)", static_cast<long long>(idx),
                                                   std::as_const(obj).arrayVal().size()),
                               line, col);
        obj.arrayVal().erase(obj.arrayVal().begin() + static_cast<size_t>(idx));
        return BuiltinMethodResult(Value::nullValue(), /*objectModified=*/true);
    }

    // S6 fix: 非变异方法通过注册表分发
    auto& registry = arraySharedMethods();
    auto it = registry.find(method);
    if (it != registry.end()) {
        return dispatchShared(it->second, obj, args.empty() ? nullptr : args.data(), args.size(), line, col);
    }

    throw RuntimeError("数组没有方法 " + method, line, col);
}

// ============================================================
// 字典内置方法
// ============================================================

/// 分发字典内置方法。
/// 变异方法（remove/set）直接就地修改 obj 的 entries；has/contains 因共享层
/// executeSharedDictHas 需要 method 名用于错误消息而单独特判；其余（len/keys/values/get）
/// 委托 dictSharedMethods() 共享实现。未知方法名抛出 RuntimeError。
BuiltinMethodResult BuiltinMethods::handleDictMethod(const std::string& method, Value& obj,
                                                     const std::vector<Value>& args, int line, int col) {
    // S6 fix: 变异方法直接处理
    if (method == "remove") {
        if (args.size() != 1)
            throw RuntimeError("remove 期望 1 个参数(键)", line, col);
        obj.dictVal().erase(args[0].toString());
        return BuiltinMethodResult(Value::nullValue(), /*objectModified=*/true);
    }
    if (method == "set") {
        if (args.size() != 2)
            throw RuntimeError("set 期望 2 个参数(键, 值)", line, col);
        obj.dictVal()[args[0].toString()] = args[1];
        return BuiltinMethodResult(Value::nullValue(), /*objectModified=*/true);
    }

    // S6 fix: has/contains 特殊处理（共享函数需要 method 名用于错误消息）
    if (method == "has" || method == "contains") {
        auto sr = executeSharedDictHas(obj, method, args.empty() ? nullptr : args.data(), args.size(), line, col);
        if (sr.is_err())
            throw to_runtime_error(sr); // A1 fix: 自由函数模板
        return BuiltinMethodResult(std::move(sr.value()));
    }

    // S6 fix: 非变异方法通过注册表分发
    auto& registry = dictSharedMethods();
    auto it = registry.find(method);
    if (it != registry.end()) {
        return dispatchShared(it->second, obj, args.empty() ? nullptr : args.data(), args.size(), line, col);
    }

    throw RuntimeError("字典没有方法 " + method, line, col);
}

// ============================================================
// 字符串内置方法
// ============================================================

/// 分发字符串内置方法。
/// 字符串方法均为非变异（返回新字符串，不改原串），因此 obj 以 const 引用传入。
/// 全部通过 stringSharedMethods() 注册表委托共享纯函数实现；contains 已在注册表中，
/// 无需内联特例。未知方法名抛出 RuntimeError。
BuiltinMethodResult BuiltinMethods::handleStringMethod(const std::string& method, const Value& obj,
                                                       const std::vector<Value>& args, int line, int col) {
    // ARCH-15 fix: contains 已注册到 stringSharedMethods()，无需内联特例
    // S6 fix: 非变异方法通过注册表分发
    auto& registry = stringSharedMethods();
    auto it = registry.find(method);
    if (it != registry.end()) {
        return dispatchShared(it->second, obj, args.empty() ? nullptr : args.data(), args.size(), line, col);
    }

    throw RuntimeError("字符串没有方法 " + method, line, col);
}
