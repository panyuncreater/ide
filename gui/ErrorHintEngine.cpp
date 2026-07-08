// ============================================================
// ErrorHintEngine.cpp — 错误信息友好化增强引擎实现（功能 12）
// ============================================================

#include "gui/ErrorHintEngine.h"

#include <algorithm>
#include <cstddef>
#include <vector>

// ============================================================
// Levenshtein 编辑距离（动态规划）
// ============================================================
/// 计算两字符串的 Levenshtein 编辑距离（字节级、大小写敏感）。
int ErrorHintEngine::levenshteinDistance(const std::string& a, const std::string& b) {
    const size_t m = a.size();
    const size_t n = b.size();
    if (m == 0)
        return static_cast<int>(n);
    if (n == 0)
        return static_cast<int>(m);

    std::vector<int> prev(n + 1);
    std::vector<int> curr(n + 1);
    for (size_t j = 0; j <= n; ++j)
        prev[j] = static_cast<int>(j);

    for (size_t i = 1; i <= m; ++i) {
        curr[0] = static_cast<int>(i);
        for (size_t j = 1; j <= n; ++j) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            int del = prev[j] + 1;
            int ins = curr[j - 1] + 1;
            int sub = prev[j - 1] + cost;
            int minVal = del < ins ? del : ins;
            if (sub < minVal)
                minVal = sub;
            curr[j] = minVal;
        }
        std::swap(prev, curr);
    }
    return prev[n];
}

// ============================================================
// 拼写建议
// ============================================================
/// 在候选集中给出编辑距离≤2 的拼写建议；无则返空。
std::string ErrorHintEngine::suggestSpelling(const std::string& target, const std::vector<std::string>& candidates) {
    if (target.empty())
        return "";
    if (candidates.empty())
        return "";

    // 若 target 与某个候选完全相同，则无需拼写建议（用户已使用正确名称）
    for (const auto& c : candidates) {
        if (c == target)
            return "";
    }

    std::string best;
    int bestDist = 3; // 阈值 = 2，初始为 3（不接受）
    for (const auto& c : candidates) {
        if (c.empty())
            continue;
        int d = levenshteinDistance(target, c);
        if (d == 0)
            continue; // 完全相同，已在上方提前返回 ""
        if (d < bestDist) {
            bestDist = d;
            best = c;
        }
    }
    return best; // bestDist > 2 时 best 仍为空
}

// ============================================================
// 错误消息增强
// ============================================================
/// 为错误消息附加教学性提示（按类别/模式/诊断码匹配）。
std::string ErrorHintEngine::enrichErrorMessage(const std::string& msg, const std::string& category,
                                                const std::vector<std::string>& scopeVars) {
    // 未知模式原样返回
    std::string enriched = msg;

    // 模式 1：缺少分号（中文 / 英文）
    if (msg.find("期望 ';'") != std::string::npos || msg.find("Expected ';'") != std::string::npos) {
        enriched += " 提示：MiniLang 每条语句需要以分号结尾。";
        return enriched;
    }

    // 模式 2：括号不匹配（中文 / 英文）
    if (msg.find("期望 ')'") != std::string::npos || msg.find("Expected ')'") != std::string::npos) {
        enriched += " 提示：括号未闭合，请检查 '(' 是否有对应的 ')'。";
        return enriched;
    }
    if (msg.find("期望 '}'") != std::string::npos || msg.find("Expected '}'") != std::string::npos) {
        enriched += " 提示：花括号未闭合，请检查 '{' 是否有对应的 '}'。";
        return enriched;
    }
    if (msg.find("期望 '('") != std::string::npos || msg.find("期望 '{'") != std::string::npos) {
        enriched += " 提示：括号不匹配，请检查语法。";
        return enriched;
    }

    // 模式 3：未定义变量 + 拼写建议
    if (msg.find("未定义的变量") != std::string::npos || msg.find("Undefined variable") != std::string::npos) {
        // 提取变量名：尝试从消息中找到冒号后或引号内的部分
        std::string varName;
        size_t colonPos = msg.find(':');
        if (colonPos != std::string::npos) {
            varName = msg.substr(colonPos + 1);
            // 去除前后空格
            while (!varName.empty() && (varName.front() == ' ' || varName.front() == '\''))
                varName.erase(0, 1);
            while (!varName.empty() && (varName.back() == ' ' || varName.back() == '\''))
                varName.pop_back();
        } else {
            size_t q1 = msg.find('\'');
            if (q1 != std::string::npos) {
                size_t q2 = msg.find('\'', q1 + 1);
                if (q2 != std::string::npos) {
                    varName = msg.substr(q1 + 1, q2 - q1 - 1);
                }
            }
        }
        if (!varName.empty() && !scopeVars.empty()) {
            std::string suggestion = suggestSpelling(varName, scopeVars);
            if (!suggestion.empty()) {
                enriched += " 提示：你是否想用 '" + suggestion + "'?";
                return enriched;
            }
        }
        return enriched;
    }

    // 模式 4：未定义函数 + 函数无提升提示
    if (msg.find("未定义的函数") != std::string::npos || msg.find("Undefined function") != std::string::npos ||
        msg.find("Undefined variable") != std::string::npos) {
        // 函数无提升提示
        if (msg.find("未定义的函数") != std::string::npos || msg.find("Undefined function") != std::string::npos) {
            enriched += " 提示：函数无提升——必须先声明后使用。";
            // 提取函数名做拼写建议
            std::string funcName;
            size_t colonPos = msg.find(':');
            if (colonPos != std::string::npos) {
                funcName = msg.substr(colonPos + 1);
                while (!funcName.empty() && (funcName.front() == ' ' || funcName.front() == '\''))
                    funcName.erase(0, 1);
                while (!funcName.empty() && (funcName.back() == ' ' || funcName.back() == '\''))
                    funcName.pop_back();
            } else {
                size_t q1 = msg.find('\'');
                if (q1 != std::string::npos) {
                    size_t q2 = msg.find('\'', q1 + 1);
                    if (q2 != std::string::npos) {
                        funcName = msg.substr(q1 + 1, q2 - q1 - 1);
                    }
                }
            }
            if (!funcName.empty() && !scopeVars.empty()) {
                std::string suggestion = suggestSpelling(funcName, scopeVars);
                if (!suggestion.empty()) {
                    enriched += " 你是否想用 '" + suggestion + "'?";
                }
            }
            return enriched;
        }
    }

    // 模式 5：除零错误
    if (msg.find("除零错误") != std::string::npos || msg.find("Division by zero") != std::string::npos) {
        enriched += " 提示：除数不能为 0，请在除法前检查除数。";
        return enriched;
    }

    // 模式 6：数组索引越界
    if (msg.find("数组索引越界") != std::string::npos || msg.find("Index out of bounds") != std::string::npos) {
        enriched += " 提示：索引超出数组长度范围，请用 len() 检查数组长度。";
        return enriched;
    }

    // 模式 7：类型错误
    if (msg.find("Type error") != std::string::npos) {
        if (msg.find("expected int, got string") != std::string::npos) {
            enriched += " 提示：类型不匹配，不能用 string 做 int 运算。可用 toInt() 转换。";
            return enriched;
        }
        if (msg.find("expected int, got float") != std::string::npos) {
            enriched += " 提示：类型不匹配，int 注解拒绝 float 值。";
            return enriched;
        }
        enriched += " 提示：类型不匹配。";
        return enriched;
    }

    // N3 fix: Unexpected token (Parser 遇到无法识别的 token)
    if (msg.find("Unexpected token") != std::string::npos || msg.find("意外的 token") != std::string::npos ||
        msg.find("unexpected") != std::string::npos) {
        return msg + " 提示：Parser 遇到了无法识别的语法结构。请检查这行代码是否完整——"
                     "例如 `var = 5;` 缺少变量名，`if { }` 缺少条件。";
    }

    // N3 fix: 参数个数不匹配
    if (msg.find("Expected") != std::string::npos && msg.find("argument") != std::string::npos ||
        msg.find("参数") != std::string::npos && msg.find("个") != std::string::npos ||
        msg.find("arity") != std::string::npos) {
        return msg + " 提示：函数调用时参数个数必须与声明一致。"
                     "检查 fun 声明的参数列表和调用时传入的参数数量。";
    }

    // N3 fix: 递归深度超限 / 栈溢出
    if (msg.find("Maximum recursion depth") != std::string::npos || msg.find("recursion") != std::string::npos ||
        msg.find("stack overflow") != std::string::npos || msg.find("递归") != std::string::npos) {
        return msg + " 提示：可能是无限递归——检查函数是否在没有基础情况下调用自身。"
                     "例如 fib(n) 必须检查 n < 2 的边界条件。";
    }

    // N3 fix: null 属性/方法访问
    if (msg.find("null") != std::string::npos &&
        (msg.find("property") != std::string::npos || msg.find("method") != std::string::npos ||
         msg.find("access") != std::string::npos || msg.find("属性") != std::string::npos)) {
        return msg + " 提示：不能在 null 值上访问属性或调用方法。"
                     "请在访问前用 `if (x != null)` 检查。";
    }

    // 未知模式：原样返回
    return msg;
}

// ============================================================
// P2 fix (错误码优先匹配): 带 stable diagnostic code 的增强重载
// ------------------------------------------------------------
// 设计目标：让 ErrorHintEngine 不再完全依赖易变的本地化消息子串。
// 引擎模块逐步迁移到带 code 的 Diagnostic 后，调用方传入 code，
// 本方法优先按 code 查 errorPatterns 表附加教学提示，避免消息文案
// 变化（如英文/本地化）时子串匹配失效。
//
// 兼容性：code 为空 / 表里查不到 tag / tag 需要从 msg 提取变量名
// (undefined-variable / undefined-function / type-mismatch) 时，
// 回退到现有的 3 参子串匹配版本（已实现变量名提取 + 拼写建议）。
// ============================================================
std::string ErrorHintEngine::enrichErrorMessage(const std::string& msg, const std::string& code,
                                                const std::string& category,
                                                const std::vector<std::string>& scopeVars) {
    // code 为空：直接回退到子串匹配
    if (code.empty()) {
        return enrichErrorMessage(msg, category, scopeVars);
    }

    // 简单 tag → 固定教学提示映射（与现有子串匹配的提示字符串保持一致）
    // 这些 tag 不需要从 msg 提取变量名，可直接附加返回。
    static const std::vector<std::pair<std::string, std::string>> kCodeToHint = {
        {"missing-semicolon", " 提示：MiniLang 每条语句需要以分号结尾。"},
        {"unbalanced-paren", " 提示：括号未闭合，请检查 '(' 是否有对应的 ')'。"},
        {"unbalanced-brace", " 提示：花括号未闭合，请检查 '{' 是否有对应的 '}'。"},
        {"division-by-zero", " 提示：除数不能为 0，请在除法前检查除数。"},
        {"index-out-of-bounds", " 提示：索引超出数组长度范围，请用 len() 检查数组长度。"},
        {"unexpected-token", " 提示：Parser 遇到了无法识别的语法结构。请检查这行代码是否完整——"
                             "例如 `var = 5;` 缺少变量名，`if { }` 缺少条件。"},
        {"arity-mismatch", " 提示：函数调用时参数个数必须与声明一致。"
                           "检查 fun 声明的参数列表和调用时传入的参数数量。"},
        {"recursion-depth", " 提示：可能是无限递归——检查函数是否在没有基础情况下调用自身。"
                            "例如 fib(n) 必须检查 n < 2 的边界条件。"},
        {"null-access", " 提示：不能在 null 值上访问属性或调用方法。"
                        "请在访问前用 `if (x != null)` 检查。"},
    };
    for (const auto& kv : kCodeToHint) {
        if (kv.first == code) {
            return msg + kv.second;
        }
    }

    // 以下 tag 需要从 msg 提取变量名做拼写建议，逻辑较复杂，
    // 已在 3 参子串匹配版本中实现，直接回退以复用逻辑。
    // 涉及：undefined-variable / undefined-function / type-mismatch
    // （以及未来需要 msg 提取的新 tag）
    return enrichErrorMessage(msg, category, scopeVars);
}

// ============================================================
// P1-F12 fix: 错误模式表（带 tag + 触发示例代码）
// ------------------------------------------------------------
// 与 enrichErrorMessage 中的模式一一对应。手册「常见错误」表按 tag 引用，
// 配合 LabManualPanel 的 `buggy:tag` 链接实现「触发示例」按钮。
// 引擎扩模式时只需在此表追加条目，手册渲染时自动可见，消除双份真相。
// ============================================================
const std::vector<ErrorHintEngine::ErrorPattern>& ErrorHintEngine::errorPatterns() {
    static const std::vector<ErrorPattern> kPatterns = {
        {"missing-semicolon", "缺少分号", "var x = 42\nprint(x)", "parser"},
        {"unbalanced-paren", "括号未闭合（')')", "print(1 + 2;\n", "parser"},
        {"unbalanced-brace", "花括号未闭合（'}')", "fun foo() {\n  print(1);\n", "parser"},
        {"undefined-variable", "未定义的变量", "print(undefinedVar);\n", "runtime"},
        {"undefined-function", "未定义的函数（函数无提升）",
         "callBeforeDefine();\nfun callBeforeDefine() { print(1); }\n", "runtime"},
        {"division-by-zero", "除零错误", "var x = 10 / 0;\nprint(x);\n", "runtime"},
        {"index-out-of-bounds", "数组索引越界", "var arr = [1, 2];\nprint(arr[5]);\n", "runtime"},
        {"type-mismatch", "类型不匹配（int 注解拒绝 string）", "var x: int = \"string\";\nprint(x);\n", "type"},
        {"unexpected-token", "无法识别的语法结构", "= 5;\n", "parser"},
        {"arity-mismatch", "函数参数个数不匹配", "fun add(a, b) { return a + b; }\nprint(add(1));\n", "runtime"},
        {"recursion-depth", "递归深度超限（无限递归）", "fun f() { f(); }\nf();\n", "runtime"},
        {"null-access", "null 值上访问属性/方法", "var x = null;\nprint(x.field);\n", "runtime"},
    };
    return kPatterns;
}
