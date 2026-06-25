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
#include "interpreter/NumericUtils.h"  // BUG9 fix: 溢出检查
#include "common/RuntimeLimits.h"      // S1 fix: MAX_RANGE 统一定义
#include <cctype>
#include <cstdint>
#include <string>
#include <charconv>

// ============================================================
// 共享纯函数实现（供 Interpreter 和 VM 共用）
// ============================================================

Result<Value> executeSharedLen(const Value& obj,
                                     const Value* args, size_t argCount,
                                     int line, int column) {
    if (argCount != 0) {
        return Result<Value>::err("len 期望 0 个参数，但传入了 " + std::to_string(argCount) + " 个", line, column);
    }
    if (obj.isArray()) {
        return Result<Value>::ok(Value(static_cast<int64_t>(obj.arrayVal().size())));
    } else if (obj.isDict()) {
        return Result<Value>::ok(Value(static_cast<int64_t>(obj.dictVal().size())));
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
        return Result<Value>::ok(Value(static_cast<int64_t>(count)));
    } else {
        return Result<Value>::err("len 不支持类型 " + obj.typeName(), line, column);
    }
}

Result<Value> executeSharedArrayContains(const Value& arr,
                                               const Value* args, size_t argCount,
                                               int line, int column) {
    if (argCount != 1) {
        return Result<Value>::err("contains 期望 1 个参数", line, column);
    }
    bool found = false;
    for (const auto& elem : arr.arrayVal()) {
        if (elem.equals(args[0])) { found = true; break; }
    }
    return Result<Value>::ok(Value(found));
}

Result<Value> executeSharedDictHas(const Value& dict,
                                         const std::string& method,
                                         const Value* args, size_t argCount,
                                         int line, int column) {
    if (argCount != 1) {
        return Result<Value>::err(method + " 期望 1 个参数(键)", line, column);
    }
    return Result<Value>::ok(Value(dict.dictVal().find(args[0].toString()) != dict.dictVal().end()));
}

// ============================================================
// 字符串方法共享层实现（供 Interpreter 和 VM 共用）
// ============================================================

Result<Value> executeSharedStrStartsWith(const Value& str,
                                                const Value* args, size_t argCount,
                                                int line, int column) {
    if (argCount != 1) {
        return Result<Value>::err("startsWith 期望 1 个参数，但传入了 " + std::to_string(argCount) + " 个", line, column);
    }
    const std::string& prefix = args[0].toString();
    const std::string& s = str.stringVal();
    return Result<Value>::ok(Value(s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0));
}

Result<Value> executeSharedStrEndsWith(const Value& str,
                                              const Value* args, size_t argCount,
                                              int line, int column) {
    if (argCount != 1) {
        return Result<Value>::err("endsWith 期望 1 个参数，但传入了 " + std::to_string(argCount) + " 个", line, column);
    }
    const std::string& suffix = args[0].toString();
    const std::string& s = str.stringVal();
    return Result<Value>::ok(Value(s.size() >= suffix.size() &&
                     s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0));
}

Result<Value> executeSharedStrSubstr(const Value& str,
                                            const Value* args, size_t argCount,
                                            int line, int column) {
    if (argCount < 1 || argCount > 2) {
        return Result<Value>::err("substr 期望 1-2 个参数(起始[, 长度])，但传入了 " + std::to_string(argCount) + " 个", line, column);
    }
    if (!args[0].isInt()) {
        return Result<Value>::err("substr 起始位置必须是整数", line, column);
    }
    int64_t start = args[0].intVal();
    const std::string& s = str.stringVal();

    // BUG 1.1 fix: 将码位索引转换为字节索引，与 len()/indexOf() 的码位语义一致
    auto codepointToByte = [](const std::string& str, int64_t cpIdx) -> size_t {
        size_t bytePos = 0;
        int64_t cp = 0;
        while (bytePos < str.size() && cp < cpIdx) {
            unsigned char c = static_cast<unsigned char>(str[bytePos]);
            bytePos += (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2 :
                       ((c & 0xF0) == 0xE0) ? 3 : ((c & 0xF8) == 0xF0) ? 4 : 1;
            cp++;
        }
        return bytePos;
    };

    // 计算字符串的码位总数
    auto codepointCount = [](const std::string& str) -> int64_t {
        int64_t count = 0;
        for (size_t i = 0; i < str.size(); ) {
            unsigned char c = static_cast<unsigned char>(str[i]);
            i += (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2 :
                 ((c & 0xF0) == 0xE0) ? 3 : ((c & 0xF8) == 0xF0) ? 4 : 1;
            count++;
        }
        return count;
    };

    int64_t totalCp = codepointCount(s);
    if (start < 0 || start > totalCp) {
        return Result<Value>::ok(Value(std::string("")));
    }

    size_t byteStart = codepointToByte(s, start);

    if (argCount == 2) {
        if (!args[1].isInt()) {
            return Result<Value>::err("substr 长度必须是整数", line, column);
        }
        int64_t len = args[1].intVal();
        if (len < 0) {
            return Result<Value>::err("substr 长度不能为负数", line, column);
        }
        // 截取 len 个码位
        size_t byteEnd = codepointToByte(s, start + len);
        if (byteEnd > s.size()) byteEnd = s.size();
        return Result<Value>::ok(Value(s.substr(byteStart, byteEnd - byteStart)));
    } else {
        return Result<Value>::ok(Value(s.substr(byteStart)));
    }
}

Result<Value> executeSharedStrIndexOf(const Value& str,
                                             const Value* args, size_t argCount,
                                             int line, int column) {
    if (argCount != 1) {
        return Result<Value>::err("indexOf 期望 1 个参数，但传入了 " + std::to_string(argCount) + " 个", line, column);
    }
    // M2 fix: 返回 UTF-8 字符位置而非字节位置
    const std::string& s = str.stringVal();
    const std::string& needle = args[0].toString();
    size_t bytePos = s.find(needle);
    if (bytePos == std::string::npos) {
        return Result<Value>::ok(Value(static_cast<int64_t>(-1)));
    } else {
        int64_t charIdx = 0;
        for (size_t b = 0; b < bytePos; ) {
            unsigned char c = static_cast<unsigned char>(s[b]);
            b += (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2 :
                 ((c & 0xF0) == 0xE0) ? 3 : ((c & 0xF8) == 0xF0) ? 4 : 1;
            charIdx++;
        }
        return Result<Value>::ok(Value(charIdx));
    }
}

// S1 fix: 共享 replace 实现 — 单遍构建 O(N)，消除 VM 侧 O(N²) Bug
Result<Value> executeSharedStrReplace(const Value& str,
                                            const Value* args, size_t argCount,
                                            int line, int column) {
    if (argCount != 2) {
        return Result<Value>::err("replace 期望 2 个参数(旧串, 新串)，但传入了 " + std::to_string(argCount) + " 个", line, column);
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

Result<Value> executeSharedStrUpper(const Value& str,
                                          const Value* args, size_t argCount,
                                          int line, int column) {
    if (argCount != 0) {
        return Result<Value>::err("upper 期望 0 个参数，但传入了 " + std::to_string(argCount) + " 个", line, column);
    }
    const std::string& s = str.stringVal();
    std::string result;
    result.reserve(s.size());
    for (char c : s) {
        result += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return Result<Value>::ok(Value(std::move(result)));
}

Result<Value> executeSharedStrLower(const Value& str,
                                          const Value* args, size_t argCount,
                                          int line, int column) {
    if (argCount != 0) {
        return Result<Value>::err("lower 期望 0 个参数，但传入了 " + std::to_string(argCount) + " 个", line, column);
    }
    const std::string& s = str.stringVal();
    std::string result;
    result.reserve(s.size());
    for (char c : s) {
        result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return Result<Value>::ok(Value(std::move(result)));
}

Result<Value> executeSharedStrSplit(const Value& str,
                                          const Value* args, size_t argCount,
                                          int line, int column) {
    if (argCount > 1) {
        return Result<Value>::err("split 期望 0-1 个参数，但传入了 " + std::to_string(argCount) + " 个", line, column);
    }
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

Result<Value> executeSharedStrTrim(const Value& str,
                                         const Value* args, size_t argCount,
                                         int line, int column) {
    if (argCount != 0) {
        return Result<Value>::err("trim 期望 0 个参数，但传入了 " + std::to_string(argCount) + " 个", line, column);
    }
    const std::string& s = str.stringVal();
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return Result<Value>::ok(Value(std::string("")));
    }
    size_t end = s.find_last_not_of(" \t\r\n");
    return Result<Value>::ok(Value(s.substr(start, end - start + 1)));
}

Result<Value> executeSharedDictKeys(const Value& dict,
                                          const Value* args, size_t argCount,
                                          int line, int column) {
    if (argCount != 0) {
        return Result<Value>::err("keys 期望 0 个参数，但传入了 " + std::to_string(argCount) + " 个", line, column);
    }
    const auto& entries = dict.dictVal();
    std::vector<Value> keys;
    keys.reserve(entries.size());
    for (const auto& kv : entries) {
        keys.emplace_back(Value(kv.first));
    }
    return Result<Value>::ok(Value(std::move(keys)));
}

Result<Value> executeSharedDictValues(const Value& dict,
                                            const Value* args, size_t argCount,
                                            int line, int column) {
    if (argCount != 0) {
        return Result<Value>::err("values 期望 0 个参数，但传入了 " + std::to_string(argCount) + " 个", line, column);
    }
    const auto& entries = dict.dictVal();
    std::vector<Value> vals;
    vals.reserve(entries.size());
    for (const auto& kv : entries) {
        vals.push_back(kv.second);
    }
    return Result<Value>::ok(Value(std::move(vals)));
}

Result<Value> executeSharedDictGet(const Value& dict,
                                         const Value* args, size_t argCount,
                                         int line, int column) {
    if (argCount < 1 || argCount > 2) {
        return Result<Value>::err("get 期望 1-2 个参数(键[, 默认值])，但传入了 " + std::to_string(argCount) + " 个", line, column);
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

Result<Value> executeSharedArrayJoin(const Value& arr,
                                           const Value* args, size_t argCount,
                                           int line, int column) {
    if (argCount > 1) {
        return Result<Value>::err("join 期望 0-1 个参数，但传入了 " + std::to_string(argCount) + " 个", line, column);
    }
    std::string sep = (argCount == 1) ? args[0].toString() : "";
    const auto& elements = arr.arrayVal();

    // 容量预估（每个元素平均 16 字节 + 分隔符），上限 64MB 防 OOM
    size_t estimated = elements.size() * (16 + sep.size());
    constexpr size_t MAX_JOIN_RESERVE = 64 * 1024 * 1024;
    if (estimated > MAX_JOIN_RESERVE) estimated = MAX_JOIN_RESERVE;

    std::string result;
    result.reserve(estimated);
    for (size_t i = 0; i < elements.size(); ++i) {
        if (i > 0) result += sep;
        result += elements[i].toString();
    }
    return Result<Value>::ok(Value(std::move(result)));
}

// ============================================================
// 顶层内置函数共享层实现
// ============================================================

bool isBuiltinFunction(const std::string& name) {
    return name == "len" || name == "type" || name == "str" ||
           name == "int" || name == "abs" || name == "min" ||
           name == "max" || name == "range" || name == "sum";
}

Result<Value> executeSharedBuiltinFunction(
    const std::string& funcName,
    const Value* args, size_t argCount,
    int line, int column) {

    // ---- len(x): 长度 ----
    if (funcName == "len") {
        if (argCount != 1) {
            return Result<Value>::err("len 期望 1 个参数，但传入了 " + std::to_string(argCount) + " 个", line, column);
        }
        // 复用已有的 executeSharedLen（它期望 obj + 0 个额外参数）
        return executeSharedLen(args[0], nullptr, 0, line, column);
    }

    // ---- type(x): 类型名 ----
    if (funcName == "type") {
        if (argCount != 1) {
            return Result<Value>::err("type 期望 1 个参数，但传入了 " + std::to_string(argCount) + " 个", line, column);
        }
        return Result<Value>::ok(Value(args[0].typeName()));
    }

    // ---- str(x): 转字符串 ----
    if (funcName == "str") {
        if (argCount != 1) {
            return Result<Value>::err("str 期望 1 个参数，但传入了 " + std::to_string(argCount) + " 个", line, column);
        }
        return Result<Value>::ok(Value(args[0].toString()));
    }

    // ---- int(x): 转整数 ----
    if (funcName == "int") {
        if (argCount != 1) {
            return Result<Value>::err("int 期望 1 个参数，但传入了 " + std::to_string(argCount) + " 个", line, column);
        }
        const Value& v = args[0];
        if (v.isInt()) {
            return Result<Value>::ok(v);
        } else if (v.isFloat()) {
            // BUG 9.3 fix: 浮点转整数溢出检查
            double dv = v.floatVal();
            if (OverflowCheck::doubleToIntOverflow(dv)) {
                return Result<Value>::err("int 转换溢出: " + std::to_string(dv) + " 超出 int64_t 范围", line, column);
            }
            // 截断小数部分（向零取整，与 C++ static_cast 一致）
            return Result<Value>::ok(Value(static_cast<int64_t>(dv)));
        } else if (v.isBool()) {
            return Result<Value>::ok(Value(static_cast<int64_t>(v.boolVal() ? 1 : 0)));
        } else if (v.isString()) {
            // 尝试解析字符串为整数
            // L-P2 fix: 使用 from_chars 替代 stoll，避免 locale 依赖
            const std::string& s = v.stringVal();
            // 跳过前导空白
            size_t startIdx = 0;
            while (startIdx < s.size() && std::isspace(static_cast<unsigned char>(s[startIdx]))) startIdx++;
            if (startIdx >= s.size()) {
                return Result<Value>::err("int 无法将空字符串转换为整数", line, column);
            }
            long long parsed = 0;
            auto [ptr, ec] = std::from_chars(s.data() + startIdx, s.data() + s.size(), parsed);
            if (ec != std::errc()) {
                return Result<Value>::err("int 无法将字符串 \"" + s + "\" 转换为整数", line, column);
            }
            // 允许尾部空白，但不允许其他字符
            size_t consumed = static_cast<size_t>(ptr - s.data());
            while (consumed < s.size() && std::isspace(static_cast<unsigned char>(s[consumed]))) consumed++;
            if (consumed != s.size()) {
                return Result<Value>::err("int 无法将字符串 \"" + s + "\" 转换为整数", line, column);
            }
            return Result<Value>::ok(Value(static_cast<int64_t>(parsed)));
        } else {
            return Result<Value>::err("int 不支持类型 " + v.typeName(), line, column);
        }
    }

    // ---- abs(x): 绝对值 ----
    if (funcName == "abs") {
        if (argCount != 1) {
            return Result<Value>::err("abs 期望 1 个参数，但传入了 " + std::to_string(argCount) + " 个", line, column);
        }
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
    if (funcName == "min") {
        if (argCount != 2) {
            return Result<Value>::err("min 期望 2 个参数，但传入了 " + std::to_string(argCount) + " 个", line, column);
        }
        const Value& a = args[0];
        const Value& b = args[1];
        if (!a.isNumber() || !b.isNumber()) {
            return Result<Value>::err("min 期望数值参数", line, column);
        }
        // 保持类型语义：两个 int 返回 int，否则返回 float
        if (a.isInt() && b.isInt()) {
            return Result<Value>::ok((a.intVal() <= b.intVal()) ? a : b);
        } else {
            double da = a.toDouble(), db = b.toDouble();
            return Result<Value>::ok(Value(da <= db ? da : db));
        }
    }

    // ---- max(a, b): 最大值 ----
    if (funcName == "max") {
        if (argCount != 2) {
            return Result<Value>::err("max 期望 2 个参数，但传入了 " + std::to_string(argCount) + " 个", line, column);
        }
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
    if (funcName == "range") {
        if (argCount != 1) {
            return Result<Value>::err("range 期望 1 个参数，但传入了 " + std::to_string(argCount) + " 个", line, column);
        }
        const Value& v = args[0];
        if (!v.isInt()) {
            return Result<Value>::err("range 期望整数参数，实际为 " + v.typeName(), line, column);
        }
        int64_t n = v.intVal();
        if (n < 0) {
            return Result<Value>::err("range 参数不能为负数: " + std::to_string(n), line, column);
        }
        // DoS 防护：限制 range 上限（S1 fix: 统一引用 RuntimeLimits::MAX_RANGE）
        if (n > RuntimeLimits::MAX_RANGE) {
            return Result<Value>::err("range 参数超过上限 " + std::to_string(RuntimeLimits::MAX_RANGE), line, column);
        }
        std::vector<Value> elements;
        elements.reserve(static_cast<size_t>(n));
        for (int64_t i = 0; i < n; ++i) {
            elements.emplace_back(Value(i));
        }
        return Result<Value>::ok(Value(std::move(elements)));
    }

    // ---- sum(arr): 数组元素求和 ----
    if (funcName == "sum") {
        if (argCount != 1) {
            return Result<Value>::err("sum 期望 1 个参数，但传入了 " + std::to_string(argCount) + " 个", line, column);
        }
        const Value& v = args[0];
        if (!v.isArray()) {
            return Result<Value>::err("sum 期望数组参数，实际为 " + v.typeName(), line, column);
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
                // BUG 9.2 fix: 累加溢出检查
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

    // 未知内置函数名
    return Result<Value>::err("未知的内置函数: " + funcName, line, column);
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
using SharedMethodFn = Result<Value>(*)(const Value&, const Value*, size_t, int, int);

/// 非变异方法注册表（延迟初始化）
const std::unordered_map<std::string, SharedMethodFn>& arraySharedMethods() {
    static const std::unordered_map<std::string, SharedMethodFn> registry = {
        {"len",      executeSharedLen},
        {"contains", executeSharedArrayContains},
        {"join",     executeSharedArrayJoin},
    };
    return registry;
}

const std::unordered_map<std::string, SharedMethodFn>& dictSharedMethods() {
    static const std::unordered_map<std::string, SharedMethodFn> registry = {
        {"len",    executeSharedLen},
        {"keys",   executeSharedDictKeys},
        {"values", executeSharedDictValues},
        {"get",    executeSharedDictGet},
    };
    return registry;
}

const std::unordered_map<std::string, SharedMethodFn>& stringSharedMethods() {
    static const std::unordered_map<std::string, SharedMethodFn> registry = {
        {"len",        executeSharedLen},
        {"upper",      executeSharedStrUpper},
        {"lower",      executeSharedStrLower},
        {"split",      executeSharedStrSplit},
        {"replace",    executeSharedStrReplace},
        {"trim",       executeSharedStrTrim},
        {"startsWith", executeSharedStrStartsWith},
        {"endsWith",   executeSharedStrEndsWith},
        {"substr",     executeSharedStrSubstr},
        {"indexOf",    executeSharedStrIndexOf},
    };
    return registry;
}

/// 调用共享方法并转换为 BuiltinMethodResult（统一错误处理路径）
BuiltinMethodResult dispatchShared(SharedMethodFn fn, const Value& obj,
                                    const Value* args, size_t argCount,
                                    int line, int col) {
    auto sr = fn(obj, args, argCount, line, col);
    if (sr.is_err()) throw sr.to_runtime_error();
    return BuiltinMethodResult(std::move(sr.value()));
}

} // anonymous namespace

// ============================================================
// 数组内置方法
// ============================================================

BuiltinMethodResult BuiltinMethods::handleArrayMethod(
    const std::string& method, Value& obj,
    const std::vector<Value>& args, int line, int col)
{
    // S6 fix: 变异方法直接处理
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

BuiltinMethodResult BuiltinMethods::handleDictMethod(
    const std::string& method, Value& obj,
    const std::vector<Value>& args, int line, int col)
{
    // S6 fix: 变异方法直接处理
    if (method == "remove") {
        if (args.size() != 1)
            throw RuntimeError("remove 期望 1 个参数(键)", line, col);
        obj.dictVal().erase(args[0].toString());
        return BuiltinMethodResult(Value::nullValue(), /*objectModified=*/true);
    }

    // S6 fix: has/contains 特殊处理（共享函数需要 method 名用于错误消息）
    if (method == "has" || method == "contains") {
        auto sr = executeSharedDictHas(obj, method, args.empty() ? nullptr : args.data(), args.size(), line, col);
        if (sr.is_err()) throw sr.to_runtime_error();
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

BuiltinMethodResult BuiltinMethods::handleStringMethod(
    const std::string& method, const Value& obj,
    const std::vector<Value>& args, int line, int col)
{
    // S6 fix: contains 特殊处理（内联实现，不走共享层）
    if (method == "contains") {
        if (args.size() != 1)
            throw RuntimeError("contains 期望 1 个参数", line, col);
        return BuiltinMethodResult(Value(obj.stringVal().find(args[0].toString()) != std::string::npos));
    }

    // S6 fix: 非变异方法通过注册表分发
    auto& registry = stringSharedMethods();
    auto it = registry.find(method);
    if (it != registry.end()) {
        return dispatchShared(it->second, obj, args.empty() ? nullptr : args.data(), args.size(), line, col);
    }

    throw RuntimeError("字符串没有方法 " + method, line, col);
}
