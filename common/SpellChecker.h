#pragma once

// ============================================================
// SpellChecker — 智能拼写纠错工具
// ------------------------------------------------------------
// 对未定义标识符/关键字拼写错误，计算编辑距离匹配最接近的合法候选词。
// 用于错误诊断信息增强："[行:列] 错误："it"未在此作用域内声明，你是不是想写"int"？"
// ============================================================

#include <string>
#include <string_view>
#include <vector>
#include <unordered_set>
#include <algorithm>
#include <cstdint>

namespace SpellChecker {

/// 计算两个字符串的 Levenshtein 编辑距离（插入/删除/替换各计1次）
/// 使用单行滚动数组优化，空间复杂度 O(min(m,n))
inline int editDistance(std::string_view a, std::string_view b) {
    const int m = static_cast<int>(a.size());
    const int n = static_cast<int>(b.size());
    if (m == 0) return n;
    if (n == 0) return m;

    // 确保 a 是较短的串，节省空间
    if (m > n) return editDistance(b, a);

    std::vector<int> prev(n + 1);
    std::vector<int> curr(n + 1);

    for (int j = 0; j <= n; ++j) prev[j] = j;

    for (int i = 1; i <= m; ++i) {
        curr[0] = i;
        for (int j = 1; j <= n; ++j) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            curr[j] = std::min({prev[j] + 1,          // 删除
                                curr[j - 1] + 1,      // 插入
                                prev[j - 1] + cost}); // 替换
        }
        std::swap(prev, curr);
    }
    return prev[n];
}

/// 在候选词集合中查找与 misspelled 最接近的词
/// maxDistance: 最大允许编辑距离（默认2），超过则不推荐
/// 返回空字符串表示无合适建议
inline std::string findClosest(std::string_view misspelled,
                                const std::vector<std::string>& candidates,
                                int maxDistance = 2) {
    if (misspelled.empty() || candidates.empty()) return {};

    // 特殊处理：空串或单字符不纠错
    if (misspelled.size() <= 1) return {};

    std::string best;
    int bestDist = maxDistance + 1;

    for (const auto& cand : candidates) {
        // 大小写不敏感比较时，编辑距离可能更小
        // 但为简单起见，直接比较（MiniLang 关键字全小写）
        int dist = editDistance(misspelled, cand);

        // 额外规则：候选词长度差异过大时施加惩罚
        int lenDiff = static_cast<int>(cand.size()) - static_cast<int>(misspelled.size());
        if (std::abs(lenDiff) > 2) dist += std::abs(lenDiff) - 2;

        if (dist < bestDist || (dist == bestDist && cand.size() == misspelled.size())) {
            bestDist = dist;
            best = cand;
        }
    }

    // 如果最佳距离仍然太大，不推荐
    if (bestDist > maxDistance) return {};
    return best;
}

/// 构建建议消息：如果找到建议，返回 "，你是不是想写"xxx"？"，否则返回空串
inline std::string suggestSuffix(std::string_view misspelled,
                                  const std::vector<std::string>& candidates,
                                  int maxDistance = 2) {
    auto suggestion = findClosest(misspelled, candidates, maxDistance);
    if (suggestion.empty() || suggestion == misspelled) return {};
    return "，你是不是想写\"" + suggestion + "\"？";
}

} // namespace SpellChecker
