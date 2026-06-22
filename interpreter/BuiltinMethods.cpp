// ============================================================
// BuiltinMethods.cpp - 内置方法实现
// ============================================================
// 从 Interpreter::visitMethodCall 中提取的数组/字典/字符串内置方法处理。
// 错误通过 throw RuntimeError(msg, line, col) 抛出，与 Interpreter::runtimeError 语义一致。

#include "interpreter/BuiltinMethods.h"
#include "interpreter/Interpreter.h"  // RuntimeError 定义
#include <cctype>
#include <cstdint>
#include <string>

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
        if (!args.empty())
            throw RuntimeError("len 期望 0 个参数，但传入了 " + std::to_string(args.size()) + " 个", line, col);
        return BuiltinMethodResult(Value(static_cast<int64_t>(obj.arrayVal().size())));
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
        if (args.size() != 1)
            throw RuntimeError("contains 期望 1 个参数", line, col);
        for (const auto& elem : obj.arrayVal()) {
            if (elem.equals(args[0])) return BuiltinMethodResult(Value(true));
        }
        return BuiltinMethodResult(Value(false));
    }

    if (method == "join") {
        if (args.size() > 1)
            throw RuntimeError("join 期望 0 或 1 个参数，但传入了 " + std::to_string(args.size()) + " 个", line, col);
        std::string sep = args.empty() ? "" : args[0].toString();
        std::string result;
        const auto& arr = obj.arrayVal();
        // P24 fix: 预估结果字符串大小，避免反复 realloc
        result.reserve(arr.size() * 16 + (arr.size() > 0 ? (arr.size() - 1) * sep.size() : 0));
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
        if (!args.empty())
            throw RuntimeError("len 期望 0 个参数，但传入了 " + std::to_string(args.size()) + " 个", line, col);
        return BuiltinMethodResult(Value(static_cast<int64_t>(obj.dictVal().size())));
    }

    if (method == "keys") {
        if (!args.empty())
            throw RuntimeError("keys 期望 0 个参数，但传入了 " + std::to_string(args.size()) + " 个", line, col);
        std::vector<Value> keys;
        keys.reserve(obj.dictVal().size());
        for (const auto& kv : obj.dictVal()) {
            keys.push_back(Value(kv.first));
        }
        return BuiltinMethodResult(Value(std::move(keys)));
    }

    if (method == "values") {
        if (!args.empty())
            throw RuntimeError("values 期望 0 个参数，但传入了 " + std::to_string(args.size()) + " 个", line, col);
        std::vector<Value> vals;
        vals.reserve(obj.dictVal().size());
        for (const auto& kv : obj.dictVal()) {
            vals.push_back(kv.second);
        }
        return BuiltinMethodResult(Value(std::move(vals)));
    }

    if (method == "has" || method == "contains") {
        if (args.size() != 1)
            throw RuntimeError(method + " 期望 1 个参数(键)", line, col);
        return BuiltinMethodResult(Value(obj.dictVal().find(args[0].toString()) != obj.dictVal().end()));
    }

    if (method == "get") {
        if (args.empty() || args.size() > 2)
            throw RuntimeError("get 期望 1-2 个参数(键[, 默认值])", line, col);
        std::string key = args[0].toString();
        auto it = obj.dictVal().find(key);
        if (it != obj.dictVal().end()) {
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
        if (!args.empty())
            throw RuntimeError("len 期望 0 个参数，但传入了 " + std::to_string(args.size()) + " 个", line, col);
        // M6 fix: 按 UTF-8 码位计数而非字节数
        const std::string& s = obj.stringVal();
        size_t count = 0;
        for (size_t i = 0; i < s.size(); ) {
            unsigned char c = static_cast<unsigned char>(s[i]);
            i += (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2 :
                 ((c & 0xF0) == 0xE0) ? 3 : ((c & 0xF8) == 0xF0) ? 4 : 1;
            count++;
        }
        return BuiltinMethodResult(Value(static_cast<int64_t>(count)));
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
        if (args.size() < 2) {
            throw RuntimeError("replace() 需要 2 个参数", line, col);
        }
        std::string from = args[0].toString();
        std::string to = args[1].toString();
        std::string result = obj.stringVal();
        if (from.empty()) return BuiltinMethodResult(Value(std::move(result)));
        size_t pos = 0;
        while ((pos = result.find(from, pos)) != std::string::npos) {
            result.replace(pos, from.length(), to);
            pos += to.length();
        }
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

    throw RuntimeError("字符串没有方法 " + method, line, col);
}
