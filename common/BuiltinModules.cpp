#include "common/BuiltinModules.h"

// ============================================================
// BuiltinModuleRegistry — 内建标准库模块注册表实现
// ------------------------------------------------------------
// P2-11 子项 4: 三后端零侵入的 `import "std/xxx"` 支持。
// moduleLoader lambda 拦截 "std/" 前缀，调用 getSource() 返回源码。
// ============================================================

namespace {

/// std/math 模块源码：数学工具函数
/// 注：MiniLang 使用 // 单行注释与 /* */ 块注释，不支持 # 注释。
const std::string kStdMath = R"(
// std/math — 数学工具函数模块
// P2-11: 内建标准库，纯 MiniLang 实现

export fun sqrt(x) {
    if (x < 0) {
        return 0.0 / 0.0;  // NaN
    }
    if (x == 0) {
        return 0.0;
    }
    var guess = x;
    var i = 0;
    while (i < 20) {
        guess = (guess + x / guess) / 2.0;
        i = i + 1;
    }
    return guess;
}

export fun pow(base, exp) {
    if (exp == 0) {
        return 1;
    }
    var result = 1;
    var n = exp;
    var b = base;
    while (n > 0) {
        if (n % 2 == 1) {
            result = result * b;
        }
        b = b * b;
        n = n / 2;
    }
    return result;
}

export fun factorial(n) {
    if (n < 0) {
        return 0;
    }
    var result = 1;
    var i = 2;
    while (i <= n) {
        result = result * i;
        i = i + 1;
    }
    return result;
}

export fun fibonacci(n) {
    if (n <= 0) {
        return 0;
    }
    if (n == 1) {
        return 1;
    }
    var a = 0;
    var b = 1;
    var i = 2;
    while (i <= n) {
        var temp = a + b;
        a = b;
        b = temp;
        i = i + 1;
    }
    return b;
}

export fun gcd(a, b) {
    a = abs(a);
    b = abs(b);
    while (b != 0) {
        var temp = b;
        b = a % b;
        a = temp;
    }
    return a;
}

export fun max_of(arr) {
    if (len(arr) == 0) {
        return 0;
    }
    var result = arr[0];
    var i = 1;
    while (i < len(arr)) {
        if (arr[i] > result) {
            result = arr[i];
        }
        i = i + 1;
    }
    return result;
}

export fun min_of(arr) {
    if (len(arr) == 0) {
        return 0;
    }
    var result = arr[0];
    var i = 1;
    while (i < len(arr)) {
        if (arr[i] < result) {
            result = arr[i];
        }
        i = i + 1;
    }
    return result;
}
)";

/// std/string 模块源码：字符串工具函数
const std::string kStdString = R"(
// std/string — 字符串工具函数模块
// P2-11: 内建标准库，纯 MiniLang 实现

export fun contains(haystack, needle) {
    var i = 0;
    var hlen = len(haystack);
    var nlen = len(needle);
    if (nlen == 0) {
        return true;
    }
    while (i <= hlen - nlen) {
        var j = 0;
        var matched = true;
        while (j < nlen) {
            if (haystack[i + j] != needle[j]) {
                matched = false;
                break;
            }
            j = j + 1;
        }
        if (matched) {
            return true;
        }
        i = i + 1;
    }
    return false;
}

export fun starts_with(s, prefix) {
    if (len(prefix) > len(s)) {
        return false;
    }
    var i = 0;
    while (i < len(prefix)) {
        if (s[i] != prefix[i]) {
            return false;
        }
        i = i + 1;
    }
    return true;
}

export fun ends_with(s, suffix) {
    var slen = len(s);
    var suflen = len(suffix);
    if (suflen > slen) {
        return false;
    }
    var i = 0;
    while (i < suflen) {
        if (s[slen - suflen + i] != suffix[i]) {
            return false;
        }
        i = i + 1;
    }
    return true;
}

export fun reverse(s) {
    var result = "";
    var i = len(s) - 1;
    while (i >= 0) {
        result = result + str(s[i]);
        i = i - 1;
    }
    return result;
}

export fun repeat(s, n) {
    var result = "";
    var i = 0;
    while (i < n) {
        result = result + s;
        i = i + 1;
    }
    return result;
}

export fun count_char(s, ch) {
    var count = 0;
    var i = 0;
    while (i < len(s)) {
        if (s[i] == ch) {
            count = count + 1;
        }
        i = i + 1;
    }
    return count;
}
)";

/// std/list 模块源码：列表/数组工具函数
/// 注：MiniLang 的 `+` 运算符不支持数组拼接（仅支持数值算术与字符串拼接），
/// 故数组构建统一使用 push 方法（arr.push(x) 就地追加，返回 null）。
const std::string kStdList = R"(
// std/list — 列表工具函数模块
// P2-11: 内建标准库，纯 MiniLang 实现

export fun map(arr, fn) {
    var result = [];
    var i = 0;
    while (i < len(arr)) {
        result.push(fn(arr[i]));
        i = i + 1;
    }
    return result;
}

export fun filter(arr, fn) {
    var result = [];
    var i = 0;
    while (i < len(arr)) {
        if (fn(arr[i])) {
            result.push(arr[i]);
        }
        i = i + 1;
    }
    return result;
}

export fun reduce(arr, fn, init) {
    var result = init;
    var i = 0;
    while (i < len(arr)) {
        result = fn(result, arr[i]);
        i = i + 1;
    }
    return result;
}

export fun for_each(arr, fn) {
    var i = 0;
    while (i < len(arr)) {
        fn(arr[i]);
        i = i + 1;
    }
}

export fun find(arr, fn) {
    var i = 0;
    while (i < len(arr)) {
        if (fn(arr[i])) {
            return arr[i];
        }
        i = i + 1;
    }
    return null;
}

export fun find_index(arr, fn) {
    var i = 0;
    while (i < len(arr)) {
        if (fn(arr[i])) {
            return i;
        }
        i = i + 1;
    }
    return -1;
}

export fun contains(arr, item) {
    var i = 0;
    while (i < len(arr)) {
        if (arr[i] == item) {
            return true;
        }
        i = i + 1;
    }
    return false;
}

export fun sort(arr) {
    // 简单冒泡排序（原地修改 arr 的副本 result）
    var result = arr;
    var n = len(result);
    var i = 0;
    while (i < n) {
        var j = 0;
        while (j < n - i - 1) {
            if (result[j] > result[j + 1]) {
                var temp = result[j];
                result[j] = result[j + 1];
                result[j + 1] = temp;
            }
            j = j + 1;
        }
        i = i + 1;
    }
    return result;
}

export fun sum(arr) {
    var result = 0;
    var i = 0;
    while (i < len(arr)) {
        result = result + arr[i];
        i = i + 1;
    }
    return result;
}

export fun flatten(arr) {
    var result = [];
    var i = 0;
    while (i < len(arr)) {
        var item = arr[i];
        var j = 0;
        while (j < len(item)) {
            result.push(item[j]);
            j = j + 1;
        }
        i = i + 1;
    }
    return result;
}
)";

/// std/result — Result/Option 错误处理模块（拓展二期·语言）
/// 用 dict 封装 tag+负载（避开 enum 不支持自引用/泛型参数的限制），
/// 提供 Ok/Err/Some/None 构造子 + is_ok/is_err/unwrap/unwrap_or/map 组合子。
/// 纯 MiniLang 实现，三后端零侵入（同 std/math|string|list）。
/// ? 传播：由 Parser 将 expr? 脱糖为 match（后续 lang-result 二阶段）；
/// 本模块先提供运行时基础设施，手动 is_err+unwrap 即可实现传播语义。
const std::string kStdResult = R"(
// std/result — Result/Option 错误处理模块
// P拓展二期: 内建标准库，纯 MiniLang 实现（用 dict 封装 tag）

// ---- Result: Ok(value) / Err(error) ----
export fun Ok(value) {
    return {"tag": "ok", "value": value};
}

export fun Err(error) {
    return {"tag": "err", "error": error};
}

export fun is_ok(r) {
    return r["tag"] == "ok";
}

export fun is_err(r) {
    return r["tag"] == "err";
}

// unwrap: Ok 返回值；Err 抛出（由调用方 try/catch 捕获）
export fun unwrap(r) {
    if (r["tag"] == "ok") {
        return r["value"];
    }
    throw "called unwrap on Err: " + str(r["error"]);
}

// unwrap_or: Ok 返回值；Err 返回默认值（不抛出）
export fun unwrap_or(r, fallback) {
    if (r["tag"] == "ok") {
        return r["value"];
    }
    return fallback;
}

// unwrap_err: Err 返回错误；Ok 抛出
export fun unwrap_err(r) {
    if (r["tag"] == "err") {
        return r["error"];
    }
    throw "called unwrap_err on Ok";
}

// map: Ok(v) -> Ok(fn(v))；Err 原样传递
export fun map_ok(r, fn) {
    if (r["tag"] == "ok") {
        return {"tag": "ok", "value": fn(r["value"])};
    }
    return r;
}

// ---- Option: Some(value) / None() ----
export fun Some(value) {
    return {"tag": "some", "value": value};
}

export fun None() {
    return {"tag": "none"};
}

export fun is_some(o) {
    return o["tag"] == "some";
}

export fun is_none(o) {
    return o["tag"] == "none";
}

// option 取值：Some 返回值；None 返回默认值
export fun option_or(o, fallback) {
    if (o["tag"] == "some") {
        return o["value"];
    }
    return fallback;
}
)";

/// std/bigint — 字符串十进制大数模块（拓展二期·语言：int 溢出 BigInt 提升演示）
/// MiniLang int 为 NaN-boxed 48 位（算术溢出报错，如 sum 的 addOverflow）。
/// 本模块演示“溢出后提升到任意精度”的另一条路：非负十进制字符串大数
/// 加法/乘法/比较，纯 MiniLang 实现，三后端零侵入。教学定位：
/// 真正的 Value 层 BigInt 提升需改 NaN-boxing 内存模型（后续工作）。
const std::string kStdBigint = R"(
// std/bigint — 字符串十进制大数模块（非负整数）
// 拓展二期: int 溢出 BigInt 提升演示，纯 MiniLang 实现

// 去除前导零（保留至少一位）
export fun bstrip(s) {
    var i = 0;
    while (i < len(s) - 1 and s[i] == "0") {
        i = i + 1;
    }
    var r = "";
    var j = i;
    while (j < len(s)) {
        r = r + s[j];
        j = j + 1;
    }
    return r;
}

// int → 大数字符串
export fun bfrom(n) {
    return str(n);
}

// 大数加法：逐位加 + 进位（经典笔算法）
export fun badd(a, b) {
    var i = len(a) - 1;
    var j = len(b) - 1;
    var carry = 0;
    var result = "";
    while (i >= 0 or j >= 0 or carry > 0) {
        var da = 0;
        if (i >= 0) {
            da = int(a[i]);
        }
        var db = 0;
        if (j >= 0) {
            db = int(b[j]);
        }
        var s = da + db + carry;
        carry = s / 10;
        result = str(s % 10) + result;
        i = i - 1;
        j = j - 1;
    }
    return bstrip(result);
}

// 大数乘法：逐位分解 + 重复加法（O(n*m*9)，教学演示优先可读性）
export fun bmul(a, b) {
    var result = "0";
    var shift = "";
    var j = len(b) - 1;
    while (j >= 0) {
        var db = int(b[j]);
        var partial = "0";
        var k = 0;
        while (k < db) {
            partial = badd(partial, a);
            k = k + 1;
        }
        if (partial != "0") {
            result = badd(result, partial + shift);
        }
        shift = shift + "0";
        j = j - 1;
    }
    return bstrip(result);
}

// 大数比较：a<b 返回 -1，a==b 返回 0，a>b 返回 1
export fun bcmp(a, b) {
    var sa = bstrip(a);
    var sb = bstrip(b);
    if (len(sa) < len(sb)) {
        return -1;
    }
    if (len(sa) > len(sb)) {
        return 1;
    }
    var i = 0;
    while (i < len(sa)) {
        var da = int(sa[i]);
        var db = int(sb[i]);
        if (da < db) {
            return -1;
        }
        if (da > db) {
            return 1;
        }
        i = i + 1;
    }
    return 0;
}

// 大数幂：base^exp（exp 为普通 int）——溢出演示的典型入口（2^100 等）
export fun bpow(base, exp) {
    var result = "1";
    var k = 0;
    while (k < exp) {
        result = bmul(result, base);
        k = k + 1;
    }
    return result;
}
)";

/// 内建模块注册表：路径→源码
const std::unordered_map<std::string, std::string>& moduleRegistry() {
    static const std::unordered_map<std::string, std::string> registry = {
        {"std/math", kStdMath},     {"std/string", kStdString}, {"std/list", kStdList},
        {"std/result", kStdResult}, {"std/bigint", kStdBigint},
    };
    return registry;
}

/// 内建模块路径列表（懒初始化）
const std::vector<std::string>& moduleList() {
    static const std::vector<std::string> list = []() {
        std::vector<std::string> result;
        for (const auto& [path, _] : moduleRegistry()) {
            result.push_back(path);
        }
        return result;
    }();
    return list;
}

} // namespace

// ============================================================
// 公共 API 实现
// ============================================================

bool BuiltinModuleRegistry::isBuiltinModule(const std::string& modulePath) {
    return moduleRegistry().count(modulePath) > 0;
}

const std::string& BuiltinModuleRegistry::getSource(const std::string& modulePath) {
    static const std::string empty;
    const auto& reg = moduleRegistry();
    auto it = reg.find(modulePath);
    if (it != reg.end()) {
        return it->second;
    }
    return empty;
}

const std::vector<std::string>& BuiltinModuleRegistry::listModules() {
    return moduleList();
}
