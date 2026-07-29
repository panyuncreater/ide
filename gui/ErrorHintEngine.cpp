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
std::string ErrorHintEngine::enrichErrorMessage(const std::string& msg, const std::string& /*category*/,
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
    // R52-4 fix: 移除死代码 "Undefined variable"——含该子串的消息已在模式 3 返回
    if (msg.find("未定义的函数") != std::string::npos || msg.find("Undefined function") != std::string::npos) {
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
    if ((msg.find("Expected") != std::string::npos && msg.find("argument") != std::string::npos) ||
        (msg.find("参数") != std::string::npos && msg.find("个") != std::string::npos) ||
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
        {"missing-semicolon", "缺少分号", "var x = 42\nprint(x)", "parser",
         "## 缺少分号\n\n"
         "**根因**：MiniLang 沿用 C/Java 风格语法，每条语句必须以分号 `;` 结尾。\n\n"
         "**常见触发场景**：\n"
         "- 行末忘记写分号（最常见）\n"
         "- 复制粘贴代码时丢失分号\n"
         "- 块语句 `}` 后忘记分号（如 `var f = fun() { ... };`）\n\n"
         "**修复模板**：在每条语句末尾添加 `;`。若不确定，可在编辑器中开启自动分号补全。\n\n"
         "**相关概念**：语句 vs 表达式、空语句、自动分号插入（ASI，JavaScript 中的不同设计）。"},

        {"unbalanced-paren", "括号未闭合（')')", "print(1 + 2;\n", "parser",
         "## 括号未闭合\n\n"
         "**根因**：每个左括号 `(` 必须有一个对应的右括号 `)`。Parser 在到达行末/文件末尾时"
         "若仍有未闭合的括号，会报此错误。\n\n"
         "**常见触发场景**：\n"
         "- 函数调用 `print(1 + 2` 漏写右括号\n"
         "- 嵌套表达式 `((a + b) * c` 漏写右括号\n"
         "- if 条件 `if (x > 0` 漏写右括号\n\n"
         "**修复模板**：从错误位置向前查找最近的 `(`，在合适位置补上 `)`。"
         "IDE 的括号匹配高亮（`(paren)`）可帮助定位。\n\n"
         "**相关概念**：Parser 状态机、错误恢复（error recovery）、括号配对算法。"},

        {"unbalanced-brace", "花括号未闭合（'}')", "fun foo() {\n  print(1);\n", "parser",
         "## 花括号未闭合\n\n"
         "**根因**：每个左花括号 `{` 必须有一个对应的右花括号 `}`。常用于函数体、"
         "类体、if/while/for 块。\n\n"
         "**常见触发场景**：\n"
         "- 函数定义 `fun foo() { ... ` 漏写右花括号\n"
         "- 嵌套块语句\n"
         "- 复制粘贴时丢失\n\n"
         "**修复模板**：从错误位置向前查找最近的 `{`，在块末尾补上 `}`。"
         "IDE 的缩进指示线可帮助识别块边界。\n\n"
         "**相关概念**：作用域、块语句、词法作用域 vs 动态作用域。"},

        {"undefined-variable", "未定义的变量", "print(undefinedVar);\n", "runtime",
         "## 未定义的变量\n\n"
         "**根因**：使用变量前必须先用 `var` 声明。MiniLang 不会自动创建全局变量，"
         "未声明的标识符在运行时抛出错误。\n\n"
         "**常见触发场景**：\n"
         "- 拼写错误（如 `lenght` vs `length`）\n"
         "- 在作用域外访问局部变量\n"
         "- 忘记声明直接赋值\n\n"
         "**修复模板**：\n"
         "1. 检查变量名拼写\n"
         "2. 确认 `var` 声明在使用之前\n"
         "3. 利用错误消息中的拼写建议（基于 Levenshtein 编辑距离）\n\n"
         "**相关概念**：变量提升（hoisting，MiniLang 中不存在）、词法作用域、闭包捕获。"},

        {"undefined-function", "未定义的函数（函数无提升）",
         "callBeforeDefine();\nfun callBeforeDefine() { print(1); }\n", "runtime",
         "## 未定义的函数（函数无提升）\n\n"
         "**根因**：MiniLang 的函数**不会提升**（no hoisting），与 JavaScript 不同。"
         "函数声明 `fun name() {}` 必须在调用之前执行。\n\n"
         "**常见触发场景**：\n"
         "- 在函数声明前调用：`f(); fun f() {}`\n"
         "- 模块导入顺序错误\n"
         "- 互递归函数未使用前置声明\n\n"
         "**修复模板**：将函数声明移到调用之前，或用 `var f = fun() {};` 形式提前绑定。\n\n"
         "**相关概念**：函数提升（hoisting）、词法作用域、模块系统、前向引用。"},

        {"division-by-zero", "除零错误", "var x = 10 / 0;\nprint(x);\n", "runtime",
         "## 除零错误\n\n"
         "**根因**：整数除法中除数为 0 在数学上未定义，运行时检测到除数为 0 抛出错误。\n\n"
         "**常见触发场景**：\n"
         "- 字面量 `10 / 0`\n"
         "- 运行时计算结果为 0：`10 / (a - a)`\n"
         "- 用户输入未校验\n\n"
         "**修复模板**：在除法前检查除数是否为 0：`if (d != 0) { x = n / d; }`。\n\n"
         "**相关概念**：整数除法截断（向零取整）、浮点除法（IEEE 754 的 inf/nan）、防御式编程。"},

        {"index-out-of-bounds", "数组索引越界", "var arr = [1, 2];\nprint(arr[5]);\n", "runtime",
         "## 数组索引越界\n\n"
         "**根因**：数组索引必须在 `[0, len(arr)-1]` 范围内。负索引或大于等于长度的索引"
         "在运行时抛出错误。\n\n"
         "**常见触发场景**：\n"
         "- 索引等于长度：`arr[len(arr)]`（应到 `len(arr)-1` 为止）\n"
         "- 循环边界错误：`while (i <= len(arr))` 应为 `<`\n"
         "- 空数组访问 `arr[0]`\n\n"
         "**修复模板**：访问前检查 `if (i >= 0 && i < len(arr))`，或用 for-each 循环。\n\n"
         "**相关概念**：0-based vs 1-based 索引、半开区间 `[0, n)`、缓冲区溢出（C/C++ 中的安全风险）。"},

        {"type-mismatch", "类型不匹配（int 注解拒绝 string）", "var x: int = \"string\";\nprint(x);\n", "type",
         "## 类型不匹配\n\n"
         "**根因**：MiniLang 支持类型注解（`var x: int = ...`）。当注解类型与实际值类型"
         "不一致时，类型检查器拒绝赋值。\n\n"
         "**常见触发场景**：\n"
         "- `var x: int = \"string\"`（int 注解拒绝 string）\n"
         "- 函数参数类型不符：`fun f(n: int) {}; f(\"hello\")`\n"
         "- 返回值类型不符\n\n"
         "**修复模板**：\n"
         "1. 改变注解类型以匹配实际值\n"
         "2. 转换值类型（如 `toInt(\"123\")`）\n"
         "3. 移除注解让类型推断处理\n\n"
         "**相关概念**：静态类型 vs 动态类型、类型推断、类型强制的安全风险。"},

        {"unexpected-token", "无法识别的语法结构", "= 5;\n", "parser",
         "## 无法识别的语法结构\n\n"
         "**根因**：Parser 在当前位置期望某种 token（如标识符、关键字），但实际读到的不匹配"
         "任何产生式规则。\n\n"
         "**常见触发场景**：\n"
         "- `= 5;`（缺少左值，如 `var x = 5;`）\n"
         "- `if { }`（缺少条件表达式，应为 `if (cond) { }`）\n"
         "- `fun () {}`（缺少函数名）\n\n"
         "**修复模板**：根据错误位置回溯，补全缺失的部分。错误消息通常会提示期望的 token。\n\n"
         "**相关概念**：LL/LR 解析、错误恢复、First/Follow 集。"},

        {"arity-mismatch", "函数参数个数不匹配", "fun add(a, b) { return a + b; }\nprint(add(1));\n", "runtime",
         "## 函数参数个数不匹配\n\n"
         "**根因**：函数调用时传入的实参数量必须与声明的形参数量一致（除非使用默认参数）。\n\n"
         "**常见触发场景**：\n"
         "- 调用时少传参数：`add(1)` 但声明 `fun add(a, b)`\n"
         "- 调用时多传参数：`add(1, 2, 3)` 但声明 `fun add(a, b)`\n"
         "- 默认参数与可选参数混淆\n\n"
         "**修复模板**：\n"
         "1. 检查函数声明的形参列表\n"
         "2. 调整调用处实参数量\n"
         "3. 或为形参添加默认值：`fun add(a, b = 0)`\n\n"
         "**相关概念**：默认参数、可变参数（varargs）、命名参数、currying。"},

        {"recursion-depth", "递归深度超限（无限递归）", "fun f() { f(); }\nf();\n", "runtime",
         "## 递归深度超限\n\n"
         "**根因**：函数递归调用深度超过 `MAX_RECURSION_DEPTH`（默认 1000），通常意味着"
         "缺少基础情况（base case）导致无限递归。\n\n"
         "**常见触发场景**：\n"
         "- 忘记写 base case：`fun f() { f(); }`\n"
         "- base case 条件错误：`if (n = 0)` 应为 `if (n == 0)`\n"
         "- 递归步进方向错误：`fib(n)` 调用 `fib(n+1)` 而非 `fib(n-1)`\n\n"
         "**修复模板**：\n"
         "1. 确认 base case 存在且条件正确\n"
         "2. 确认递归步进向 base case 收敛\n"
         "3. 考虑改用迭代（while/for 循环）避免栈溢出\n"
         "4. 必要时启用 TCO（尾调用优化，R112 实现）\n\n"
         "**相关概念**：调用栈、栈溢出、尾递归、记忆化（memoization）。"},

        {"null-access", "null 值上访问属性/方法", "var x = null;\nprint(x.field);\n", "runtime",
         "## null 值访问\n\n"
         "**根因**：`null` 表示「无值」，访问其属性或调用其方法在语义上无意义，运行时抛出错误。\n\n"
         "**常见触发场景**：\n"
         "- 字典查找返回 null 后直接访问：`d.get(\"missing\").field`\n"
         "- 函数返回 null 未检查\n"
         "- 未初始化的变量（MiniLang 中 `var x;` 默认为 null）\n\n"
         "**修复模板**：\n"
         "1. 访问前显式检查：`if (x != null) { x.field }`\n"
         "2. 使用空合并运算符（若语言支持）\n"
         "3. 提供默认值：`var v = x == null ? defaultValue : x.field`\n\n"
         "**相关概念**：null vs undefined、空对象模式（Null Object Pattern）、Option/Maybe 类型。"},
    };
    return kPatterns;
}

// ============================================================
// R117: 错误消息教学模式 API 实现
// ============================================================

const ErrorHintEngine::ErrorPattern* ErrorHintEngine::findPattern(const std::string& tag) {
    for (const auto& p : errorPatterns()) {
        if (p.tag == tag) {
            return &p;
        }
    }
    return nullptr;
}

std::string ErrorHintEngine::renderTooltipHtml(const std::string& tag) {
    const ErrorPattern* p = findPattern(tag);
    if (!p) {
        return "";
    }
    // 简易 HTML 转义（避免 markdown 中的 < > & 影响渲染）
    auto escape = [](const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            switch (c) {
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '&':
                out += "&amp;";
                break;
            default:
                out += c;
                break;
            }
        }
        return out;
    };
    // tooltip 用紧凑格式：标题 + 类别 + 教学说明（保留 markdown 原文，Qt tooltip 支持 HTML）
    std::string html = "<div style='max-width:400px;'>";
    html += "<b style='color:#268BD2;'>" + escape(p->title) + "</b>";
    html += " <span style='color:#8C8C8C;font-size:11px;'>[" + escape(p->category) + "]</span>";
    html += "<hr style='border:0;border-top:1px solid #E0E0E0;margin:4px 0;'/>";
    // 将 markdown 中的换行转为 <br>，保留缩进
    std::string body = escape(p->teachingMarkdown);
    for (size_t i = 0; i < body.size(); ++i) {
        if (body[i] == '\n') {
            body.replace(i, 1, "<br>");
            i += 3;
        }
    }
    html += "<div style='font-size:12px;line-height:1.5;'>" + body + "</div>";
    html += "</div>";
    return html;
}

std::string ErrorHintEngine::renderTeachingMarkdown(const std::string& tag) {
    const ErrorPattern* p = findPattern(tag);
    if (!p) {
        return "";
    }
    return p->teachingMarkdown;
}
