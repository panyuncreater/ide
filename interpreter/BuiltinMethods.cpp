// ============================================================
// BuiltinMethods.cpp - 内置方法实现
// ============================================================
// 从 Interpreter::visitMethodCall 中提取的数组/字典/字符串内置方法处理。
// 错误通过 throw RuntimeError(msg, line, col) 抛出，与 Interpreter::runtimeError 语义一致。
//
// 共享纯函数层（executeSharedLen / executeSharedArrayContains / executeSharedDictHas）
// 不抛异常，供 Interpreter 和 VM 共用。下方的 handle*Method 在调用共享函数后，
// 自行将 SharedBuiltinResult.isError 转换为 RuntimeError 抛出。

#include "interpreter/BuiltinMethods.h"
#include "interpreter/Interpreter.h"  // RuntimeError 定义
#include <cctype>
#include <cstdint>
#include <string>

// ============================================================
// 共享纯函数实现（供 Interpreter 和 VM 共用）
// ============================================================

SharedBuiltinResult executeSharedLen(const Value& obj,
                                     const Value* args, size_t argCount,
                                     int line, int column) {
    SharedBuiltinResult r;
    if (argCount != 0) {
        r.isError = true;
        r.errorMessage = "len 期望 0 个参数，但传入了 " + std::to_string(argCount) + " 个";
        r.errorLine = line;
        r.errorColumn = column;
        return r;
    }
    if (obj.isArray()) {
        r.result = Value(static_cast<int64_t>(obj.arrayVal().size()));
    } else if (obj.isDict()) {
        r.result = Value(static_cast<int64_t>(obj.dictVal().size()));
    } else if (obj.isString()) {
        // M6 fix: 按 UTF-8 码位计数而非字节数
        const std::string& s = obj.stringVal();
        size_t count = 0;
        for (size_t i = 0; i < s.size(); ) {
            unsigned char c = static_cast<unsigned char>(s[i]);
            i += (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2 :
                 ((c & 0xF0) == 0xE0) ? 3 : ((c & 0xF8) == 0xF0) ? 4 : 1;
            count++;
        }
        r.result = Value(static_cast<int64_t>(count));
    } else {
        r.isError = true;
        r.errorMessage = "len 不支持类型 " + obj.typeName();
        r.errorLine = line;
        r.errorColumn = column;
    }
    return r;
}

SharedBuiltinResult executeSharedArrayContains(const Value& arr,
                                               const Value* args, size_t argCount,
                                               int line, int column) {
    SharedBuiltinResult r;
    if (argCount != 1) {
        r.isError = true;
        r.errorMessage = "contains 期望 1 个参数";
        r.errorLine = line;
        r.errorColumn = column;
        return r;
    }
    bool found = false;
    for (const auto& elem : arr.arrayVal()) {
        if (elem.equals(args[0])) { found = true; break; }
    }
    r.result = Value(found);
    return r;
}

SharedBuiltinResult executeSharedDictHas(const Value& dict,
                                         const std::string& method,
                                         const Value* args, size_t argCount,
                                         int line, int column) {
    SharedBuiltinResult r;
    if (argCount != 1) {
        r.isError = true;
        r.errorMessage = method + " 期望 1 个参数(键)";
        r.errorLine = line;
        r.errorColumn = column;
        return r;
    }
    r.result = Value(dict.dictVal().find(args[0].toString()) != dict.dictVal().end());
    return r;
}

// ============================================================
// 顶层内置函数共享层实现
// ============================================================

bool isBuiltinFunction(const std::string& name) {
    return name == "len" || name == "type" || name == "str" ||
           name == "int" || name == "abs" || name == "min" ||
           name == "max" || name == "range" || name == "sum";
}

SharedBuiltinResult executeSharedBuiltinFunction(
    const std::string& funcName,
    const Value* args, size_t argCount,
    int line, int column) {

    SharedBuiltinResult r;

    // ---- len(x): 长度 ----
    if (funcName == "len") {
        if (argCount != 1) {
            r.isError = true;
            r.errorMessage = "len 期望 1 个参数，但传入了 " + std::to_string(argCount) + " 个";
            r.errorLine = line; r.errorColumn = column;
            return r;
        }
        // 复用已有的 executeSharedLen（它期望 obj + 0 个额外参数）
        return executeSharedLen(args[0], nullptr, 0, line, column);
    }

    // ---- type(x): 类型名 ----
    if (funcName == "type") {
        if (argCount != 1) {
            r.isError = true;
            r.errorMessage = "type 期望 1 个参数，但传入了 " + std::to_string(argCount) + " 个";
            r.errorLine = line; r.errorColumn = column;
            return r;
        }
        r.result = Value(args[0].typeName());
        return r;
    }

    // ---- str(x): 转字符串 ----
    if (funcName == "str") {
        if (argCount != 1) {
            r.isError = true;
            r.errorMessage = "str 期望 1 个参数，但传入了 " + std::to_string(argCount) + " 个";
            r.errorLine = line; r.errorColumn = column;
            return r;
        }
        r.result = Value(args[0].toString());
        return r;
    }

    // ---- int(x): 转整数 ----
    if (funcName == "int") {
        if (argCount != 1) {
            r.isError = true;
            r.errorMessage = "int 期望 1 个参数，但传入了 " + std::to_string(argCount) + " 个";
            r.errorLine = line; r.errorColumn = column;
            return r;
        }
        const Value& v = args[0];
        if (v.isInt()) {
            r.result = v;
        } else if (v.isFloat()) {
            // 截断小数部分（向零取整，与 C++ static_cast 一致）
            r.result = Value(static_cast<int64_t>(v.floatVal()));
        } else if (v.isBool()) {
            r.result = Value(static_cast<int64_t>(v.boolVal() ? 1 : 0));
        } else if (v.isString()) {
            // 尝试解析字符串为整数
            const std::string& s = v.stringVal();
            try {
                size_t pos = 0;
                long long parsed = std::stoll(s, &pos);
                // 允许尾部空白，但不允许其他字符
                while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) pos++;
                if (pos != s.size()) {
                    r.isError = true;
                    r.errorMessage = "int 无法将字符串 \"" + s + "\" 转换为整数";
                    r.errorLine = line; r.errorColumn = column;
                    return r;
                }
                r.result = Value(static_cast<int64_t>(parsed));
            } catch (...) {
                r.isError = true;
                r.errorMessage = "int 无法将字符串 \"" + s + "\" 转换为整数";
                r.errorLine = line; r.errorColumn = column;
                return r;
            }
        } else {
            r.isError = true;
            r.errorMessage = "int 不支持类型 " + v.typeName();
            r.errorLine = line; r.errorColumn = column;
        }
        return r;
    }

    // ---- abs(x): 绝对值 ----
    if (funcName == "abs") {
        if (argCount != 1) {
            r.isError = true;
            r.errorMessage = "abs 期望 1 个参数，但传入了 " + std::to_string(argCount) + " 个";
            r.errorLine = line; r.errorColumn = column;
            return r;
        }
        const Value& v = args[0];
        if (v.isInt()) {
            int64_t iv = v.intVal();
            r.result = Value(iv < 0 ? -iv : iv);
        } else if (v.isFloat()) {
            double dv = v.floatVal();
            r.result = Value(dv < 0 ? -dv : dv);
        } else {
            r.isError = true;
            r.errorMessage = "abs 期望数值参数，实际为 " + v.typeName();
            r.errorLine = line; r.errorColumn = column;
        }
        return r;
    }

    // ---- min(a, b): 最小值 ----
    if (funcName == "min") {
        if (argCount != 2) {
            r.isError = true;
            r.errorMessage = "min 期望 2 个参数，但传入了 " + std::to_string(argCount) + " 个";
            r.errorLine = line; r.errorColumn = column;
            return r;
        }
        const Value& a = args[0];
        const Value& b = args[1];
        if (!a.isNumber() || !b.isNumber()) {
            r.isError = true;
            r.errorMessage = "min 期望数值参数";
            r.errorLine = line; r.errorColumn = column;
            return r;
        }
        // 保持类型语义：两个 int 返回 int，否则返回 float
        if (a.isInt() && b.isInt()) {
            r.result = (a.intVal() <= b.intVal()) ? a : b;
        } else {
            double da = a.toDouble(), db = b.toDouble();
            r.result = Value(da <= db ? da : db);
        }
        return r;
    }

    // ---- max(a, b): 最大值 ----
    if (funcName == "max") {
        if (argCount != 2) {
            r.isError = true;
            r.errorMessage = "max 期望 2 个参数，但传入了 " + std::to_string(argCount) + " 个";
            r.errorLine = line; r.errorColumn = column;
            return r;
        }
        const Value& a = args[0];
        const Value& b = args[1];
        if (!a.isNumber() || !b.isNumber()) {
            r.isError = true;
            r.errorMessage = "max 期望数值参数";
            r.errorLine = line; r.errorColumn = column;
            return r;
        }
        if (a.isInt() && b.isInt()) {
            r.result = (a.intVal() >= b.intVal()) ? a : b;
        } else {
            double da = a.toDouble(), db = b.toDouble();
            r.result = Value(da >= db ? da : db);
        }
        return r;
    }

    // ---- range(n): 生成 [0, 1, ..., n-1] 数组 ----
    if (funcName == "range") {
        if (argCount != 1) {
            r.isError = true;
            r.errorMessage = "range 期望 1 个参数，但传入了 " + std::to_string(argCount) + " 个";
            r.errorLine = line; r.errorColumn = column;
            return r;
        }
        const Value& v = args[0];
        if (!v.isInt()) {
            r.isError = true;
            r.errorMessage = "range 期望整数参数，实际为 " + v.typeName();
            r.errorLine = line; r.errorColumn = column;
            return r;
        }
        int64_t n = v.intVal();
        if (n < 0) {
            r.isError = true;
            r.errorMessage = "range 参数不能为负数: " + std::to_string(n);
            r.errorLine = line; r.errorColumn = column;
            return r;
        }
        // DoS 防护：限制 range 上限（与 MAX_LOOP_ITERATIONS 对齐）
        static constexpr int64_t MAX_RANGE = 10000000;
        if (n > MAX_RANGE) {
            r.isError = true;
            r.errorMessage = "range 参数超过上限 " + std::to_string(MAX_RANGE);
            r.errorLine = line; r.errorColumn = column;
            return r;
        }
        std::vector<Value> elements;
        elements.reserve(static_cast<size_t>(n));
        for (int64_t i = 0; i < n; ++i) {
            elements.emplace_back(Value(i));
        }
        r.result = Value(std::move(elements));
        return r;
    }

    // ---- sum(arr): 数组元素求和 ----
    if (funcName == "sum") {
        if (argCount != 1) {
            r.isError = true;
            r.errorMessage = "sum 期望 1 个参数，但传入了 " + std::to_string(argCount) + " 个";
            r.errorLine = line; r.errorColumn = column;
            return r;
        }
        const Value& v = args[0];
        if (!v.isArray()) {
            r.isError = true;
            r.errorMessage = "sum 期望数组参数，实际为 " + v.typeName();
            r.errorLine = line; r.errorColumn = column;
            return r;
        }
        const auto& arr = v.arrayVal();
        // 判断是否全为 int（结果保持 int 类型）
        bool allInt = true;
        for (const auto& elem : arr) {
            if (!elem.isInt()) { allInt = false; break; }
        }
        if (allInt) {
            int64_t total = 0;
            for (const auto& elem : arr) {
                total += elem.intVal();
            }
            r.result = Value(total);
        } else {
            double total = 0.0;
            for (const auto& elem : arr) {
                if (elem.isNumber()) {
                    total += elem.toDouble();
                } else {
                    r.isError = true;
                    r.errorMessage = "sum 数组元素包含非数值类型: " + elem.typeName();
                    r.errorLine = line; r.errorColumn = column;
                    return r;
                }
            }
            r.result = Value(total);
        }
        return r;
    }

    // 未知内置函数名
    r.isError = true;
    r.errorMessage = "未知的内置函数: " + funcName;
    r.errorLine = line; r.errorColumn = column;
    return r;
}

// ============================================================
// 数组内置方法
// ============================================================

BuiltinMethodResult BuiltinMethods::handleArrayMethod(
    const std::string& method, Value& obj,
    const std::vector<Value>& args, int line, int col)
{
    if (method == "push") {
        if (args.size() != 1)
            throw RuntimeError("push 期望 1 个参数", line, col);
        obj.arrayVal().push_back(args[0]);
        return BuiltinMethodResult(Value::nullValue(), /*objectModified=*/true);
    }

    if (method == "pop") {
        if (!args.empty())
            throw RuntimeError("pop 期望 0 个参数，但传入了 " + std::to_string(args.size()) + " 个", line, col);
        if (obj.arrayVal().empty())
            throw RuntimeError("对空数组调用 pop", line, col);
        Value last = obj.arrayVal().back();
        obj.arrayVal().pop_back();
        return BuiltinMethodResult(std::move(last), /*objectModified=*/true);
    }

    if (method == "len") {
        // 委托共享纯函数（供 VM 复用同一份逻辑）
        auto sr = executeSharedLen(obj, args.empty() ? nullptr : args.data(), args.size(), line, col);
        if (sr.isError) throw RuntimeError(sr.errorMessage, sr.errorLine, sr.errorColumn);
        return BuiltinMethodResult(std::move(sr.result));
    }

    if (method == "remove") {
        if (args.size() != 1)
            throw RuntimeError("remove 期望 1 个参数(索引)", line, col);
        if (!args[0].isInt())
            throw RuntimeError("remove 参数必须是整数索引", line, col);
        int64_t idx = args[0].intVal();
        if (idx < 0 || static_cast<size_t>(idx) >= obj.arrayVal().size())
            throw RuntimeError("数组索引越界: " + std::to_string(idx) + ", 有效范围 [0, " + std::to_string(obj.arrayVal().size()) + ")", line, col);
        obj.arrayVal().erase(obj.arrayVal().begin() + static_cast<size_t>(idx));
        return BuiltinMethodResult(Value::nullValue(), /*objectModified=*/true);
    }

    if (method == "contains") {
        // 委托共享纯函数（供 VM 复用同一份逻辑）
        auto sr = executeSharedArrayContains(obj, args.empty() ? nullptr : args.data(), args.size(), line, col);
        if (sr.isError) throw RuntimeError(sr.errorMessage, sr.errorLine, sr.errorColumn);
        return BuiltinMethodResult(std::move(sr.result));
    }

    if (method == "join") {
        if (args.size() > 1)
            throw RuntimeError("join 期望 0 或 1 个参数，但传入了 " + std::to_string(args.size()) + " 个", line, col);
        std::string sep = args.empty() ? "" : args[0].toString();
        std::string result;
        // P-01 fix: 只读方法使用 const 访问，避免触发 COW 深拷贝
        const auto& arr = static_cast<const Value&>(obj).arrayVal();
        // P24 fix + P2-12 fix: 预估结果字符串大小，避免反复 realloc
        // 同时对 reserve 设上限（64MB），防止恶意输入触发 OOM
        constexpr size_t MAX_JOIN_RESERVE = 64 * 1024 * 1024;  // 64MB
        size_t estimated = arr.size() * 16 + (arr.size() > 0 ? (arr.size() - 1) * sep.size() : 0);
        if (estimated > MAX_JOIN_RESERVE) estimated = MAX_JOIN_RESERVE;
        result.reserve(estimated);
        for (size_t i = 0; i < arr.size(); ++i) {
            if (i > 0) result += sep;
            result += arr[i].toString();
        }
        return BuiltinMethodResult(Value(std::move(result)));
    }

    throw RuntimeError("数组没有方法 " + method, line, col);
}

// ============================================================
// 字典内置方法
// ============================================================

BuiltinMethodResult BuiltinMethods::handleDictMethod(
    const std::string& method, Value& obj,
    const std::vector<Value>& args, int line, int col)
{
    if (method == "len") {
        // 委托共享纯函数（供 VM 复用同一份逻辑）
        auto sr = executeSharedLen(obj, args.empty() ? nullptr : args.data(), args.size(), line, col);
        if (sr.isError) throw RuntimeError(sr.errorMessage, sr.errorLine, sr.errorColumn);
        return BuiltinMethodResult(std::move(sr.result));
    }

    if (method == "keys") {
        if (!args.empty())
            throw RuntimeError("keys 期望 0 个参数，但传入了 " + std::to_string(args.size()) + " 个", line, col);
        // P-01 fix: 只读方法使用 const 访问，避免触发 COW 深拷贝
        const auto& dict = static_cast<const Value&>(obj).dictVal();
        std::vector<Value> keys;
        keys.reserve(dict.size());
        for (const auto& kv : dict) {
            keys.push_back(Value(kv.first));
        }
        return BuiltinMethodResult(Value(std::move(keys)));
    }

    if (method == "values") {
        if (!args.empty())
            throw RuntimeError("values 期望 0 个参数，但传入了 " + std::to_string(args.size()) + " 个", line, col);
        // P-01 fix: 只读方法使用 const 访问，避免触发 COW 深拷贝
        const auto& dict = static_cast<const Value&>(obj).dictVal();
        std::vector<Value> vals;
        vals.reserve(dict.size());
        for (const auto& kv : dict) {
            vals.push_back(kv.second);
        }
        return BuiltinMethodResult(Value(std::move(vals)));
    }

    if (method == "has" || method == "contains") {
        // 委托共享纯函数（供 VM 复用同一份逻辑）
        auto sr = executeSharedDictHas(obj, method, args.empty() ? nullptr : args.data(), args.size(), line, col);
        if (sr.isError) throw RuntimeError(sr.errorMessage, sr.errorLine, sr.errorColumn);
        return BuiltinMethodResult(std::move(sr.result));
    }

    if (method == "get") {
        if (args.empty() || args.size() > 2)
            throw RuntimeError("get 期望 1-2 个参数(键[, 默认值])", line, col);
        std::string key = args[0].toString();
        // P-01 fix: 只读方法使用 const 访问，避免触发 COW 深拷贝
        const auto& dict = static_cast<const Value&>(obj).dictVal();
        auto it = dict.find(key);
        if (it != dict.end()) {
            return BuiltinMethodResult(it->second);
        }
        return BuiltinMethodResult((args.size() == 2) ? args[1] : Value::nullValue());
    }

    if (method == "remove") {
        if (args.size() != 1)
            throw RuntimeError("remove 期望 1 个参数(键)", line, col);
        obj.dictVal().erase(args[0].toString());
        return BuiltinMethodResult(Value::nullValue(), /*objectModified=*/true);
    }

    throw RuntimeError("字典没有方法 " + method, line, col);
}

// ============================================================
// 字符串内置方法
// ============================================================

BuiltinMethodResult BuiltinMethods::handleStringMethod(
    const std::string& method, const Value& obj,
    const std::vector<Value>& args, int line, int col)
{
    if (method == "len") {
        // 委托共享纯函数（供 VM 复用同一份逻辑）
        auto sr = executeSharedLen(obj, args.empty() ? nullptr : args.data(), args.size(), line, col);
        if (sr.isError) throw RuntimeError(sr.errorMessage, sr.errorLine, sr.errorColumn);
        return BuiltinMethodResult(std::move(sr.result));
    }

    if (method == "upper") {
        if (!args.empty())
            throw RuntimeError("upper 期望 0 个参数，但传入了 " + std::to_string(args.size()) + " 个", line, col);
        std::string s = obj.stringVal();
        for (auto& c : s) c = std::toupper(static_cast<unsigned char>(c));
        return BuiltinMethodResult(Value(std::move(s)));
    }

    if (method == "lower") {
        if (!args.empty())
            throw RuntimeError("lower 期望 0 个参数，但传入了 " + std::to_string(args.size()) + " 个", line, col);
        std::string s = obj.stringVal();
        for (auto& c : s) c = std::tolower(static_cast<unsigned char>(c));
        return BuiltinMethodResult(Value(std::move(s)));
    }

    if (method == "split") {
        // str.split(sep) — 按 sep 分割返回数组
        std::string sep = args.empty() ? " " : args[0].toString();
        if (sep.empty()) {
            // 空分隔符下 std::string::find("") 总返回 start，会导致死循环
            throw RuntimeError("split 的分隔符不能为空字符串", line, col);
        }
        std::vector<Value> parts;
        size_t start = 0, pos;
        const std::string& str = obj.stringVal();
        while ((pos = str.find(sep, start)) != std::string::npos) {
            parts.push_back(Value(str.substr(start, pos - start)));
            start = pos + sep.size();
        }
        parts.push_back(Value(str.substr(start)));
        return BuiltinMethodResult(Value(std::move(parts)));
    }

    if (method == "replace") {
        // str.replace(from, to) — 将所有 from 替换为 to
        // P2-11 fix: 改为单遍构建新字符串，复杂度从 O(N²) 降至 O(N)
        if (args.size() != 2) {
            throw RuntimeError("replace 期望 2 个参数", line, col);
        }
        std::string from = args[0].toString();
        std::string to = args[1].toString();
        const std::string& src = obj.stringVal();
        if (from.empty()) return BuiltinMethodResult(Value(std::string(src)));

        // 预扫统计匹配数以估算容量
        size_t matchCount = 0;
        size_t scanPos = 0;
        while ((scanPos = src.find(from, scanPos)) != std::string::npos) {
            ++matchCount;
            scanPos += from.size();
        }

        // 单遍构建结果字符串
        std::string result;
        // 容量估算：原长度 + 匹配数 × 长度差（仅当 to 比 from 长时增加）
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
        return BuiltinMethodResult(Value(std::move(result)));
    }

    if (method == "trim") {
        if (!args.empty())
            throw RuntimeError("trim 期望 0 个参数，但传入了 " + std::to_string(args.size()) + " 个", line, col);
        std::string s = obj.stringVal();
        size_t l = s.find_first_not_of(" \t\r\n");
        size_t r = s.find_last_not_of(" \t\r\n");
        if (l == std::string::npos) return BuiltinMethodResult(Value(std::string("")));
        return BuiltinMethodResult(Value(s.substr(l, r - l + 1)));
    }

    if (method == "contains") {
        if (args.size() != 1)
            throw RuntimeError("contains 期望 1 个参数", line, col);
        return BuiltinMethodResult(Value(obj.stringVal().find(args[0].toString()) != std::string::npos));
    }
    throw RuntimeError("字符串没有方法 " + method, line, col);
}
