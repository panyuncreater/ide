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
int ErrorHintEngine::levenshteinDistance(const std::string& a, const std::string& b) {
    const size_t m = a.size();
    const size_t n = b.size();
    if (m == 0) return static_cast<int>(n);
    if (n == 0) return static_cast<int>(m);

    std::vector<int> prev(n + 1);
    std::vector<int> curr(n + 1);
    for (size_t j = 0; j <= n; ++j) prev[j] = static_cast<int>(j);

    for (size_t i = 1; i <= m; ++i) {
        curr[0] = static_cast<int>(i);
        for (size_t j = 1; j <= n; ++j) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            int del = prev[j] + 1;
            int ins = curr[j - 1] + 1;
            int sub = prev[j - 1] + cost;
            int minVal = del < ins ? del : ins;
            if (sub < minVal) minVal = sub;
            curr[j] = minVal;
        }
        std::swap(prev, curr);
    }
    return prev[n];
}

// ============================================================
// 拼写建议
// ============================================================
std::string ErrorHintEngine::suggestSpelling(const std::string& target,
                                              const std::vector<std::string>& candidates) {
    if (target.empty()) return "";
    if (candidates.empty()) return "";

    // 若 target 与某个候选完全相同，则无需拼写建议（用户已使用正确名称）
    for (const auto& c : candidates) {
        if (c == target) return "";
    }

    std::string best;
    int bestDist = 3;  // 阈值 = 2，初始为 3（不接受）
    for (const auto& c : candidates) {
        if (c.empty()) continue;
        int d = levenshteinDistance(target, c);
        if (d == 0) continue;  // 完全相同，已在上方提前返回 ""
        if (d < bestDist) {
            bestDist = d;
            best = c;
        }
    }
    return best;  // bestDist > 2 时 best 仍为空
}

// ============================================================
// 错误消息增强
// ============================================================
std::string ErrorHintEngine::enrichErrorMessage(const std::string& msg,
                                                 const std::string& category,
                                                 const std::vector<std::string>& scopeVars) {
    // 未知模式原样返回
    std::string enriched = msg;

    // 模式 1：缺少分号（中文 / 英文）
    if (msg.find("期望 ';'") != std::string::npos ||
        msg.find("Expected ';'") != std::string::npos) {
        enriched += " 提示：MiniLang 每条语句需要以分号结尾。";
        return enriched;
    }

    // 模式 2：括号不匹配（中文 / 英文）
    if (msg.find("期望 ')'") != std::string::npos ||
        msg.find("Expected ')'") != std::string::npos) {
        enriched += " 提示：括号未闭合，请检查 '(' 是否有对应的 ')'。";
        return enriched;
    }
    if (msg.find("期望 '}'") != std::string::npos ||
        msg.find("Expected '}'") != std::string::npos) {
        enriched += " 提示：花括号未闭合，请检查 '{' 是否有对应的 '}'。";
        return enriched;
    }
    if (msg.find("期望 '('") != std::string::npos ||
        msg.find("期望 '{'") != std::string::npos) {
        enriched += " 提示：括号不匹配，请检查语法。";
        return enriched;
    }

    // 模式 3：未定义变量 + 拼写建议
    if (msg.find("未定义的变量") != std::string::npos ||
        msg.find("Undefined variable") != std::string::npos) {
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
    if (msg.find("未定义的函数") != std::string::npos ||
        msg.find("Undefined function") != std::string::npos ||
        msg.find("Undefined variable") != std::string::npos) {
        // 函数无提升提示
        if (msg.find("未定义的函数") != std::string::npos ||
            msg.find("Undefined function") != std::string::npos) {
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
    if (msg.find("除零错误") != std::string::npos ||
        msg.find("Division by zero") != std::string::npos) {
        enriched += " 提示：除数不能为 0，请在除法前检查除数。";
        return enriched;
    }

    // 模式 6：数组索引越界
    if (msg.find("数组索引越界") != std::string::npos ||
        msg.find("Index out of bounds") != std::string::npos) {
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

    // 未知模式：原样返回
    return msg;
}
