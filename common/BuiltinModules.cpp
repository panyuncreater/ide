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

/// 内建模块注册表：路径→源码
const std::unordered_map<std::string, std::string>& moduleRegistry() {
    static const std::unordered_map<std::string, std::string> registry = {
        {"std/math", kStdMath},
        {"std/string", kStdString},
        {"std/list", kStdList},
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
