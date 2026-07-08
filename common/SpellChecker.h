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

    // 确保 a 是较短的串，节省空间（滚动数组只需长度+1 的一维向量）
    if (m > n) return editDistance(b, a);

    std::vector<int> prev(n + 1);
    std::vector<int> curr(n + 1);

    // 边界初始化：prev[j] 表示将空串 a 变为 b[0..j] 需 j 次插入
    for (int j = 0; j <= n; ++j) prev[j] = j;

    for (int i = 1; i <= m; ++i) {
        // curr[0] 表示将 a[0..i] 变为空串需 i 次删除
        curr[0] = i;
        for (int j = 1; j <= n; ++j) {
            // cost：本字符相同则无需替换（0），否则计一次替换（1）
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            // 经典递推：取 删除(prev[j]+1) / 插入(curr[j-1]+1) / 替换(prev[j-1]+cost)
            // 三者最小值。prev 为上一行结果，curr[j-1] 为当前行已算的左邻。
            curr[j] = std::min({prev[j] + 1,          // 删除
                                curr[j - 1] + 1,      // 插入
                                prev[j - 1] + cost}); // 替换
        }
        // 滚动窗口：交换后 curr 成为下一轮的 prev，旧 prev 复用为下一轮 curr，
        // 仅需两行向量即可完成整张 DP 表，空间 O(n)。
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

        // 额外规则：候选词长度差异过大时施加惩罚。
        // 原因：纯编辑距离对"长度悬殊但少数字符不同"的词不敏感
        // （如 it→function 只有 1 次替换+6 次插入=7，但实际显然不是拼写纠错目标）。
        // 当 |长度差|>2 时按超出部分累加惩罚，压低长候选的优先级，
        // 使建议更贴近用户的真实意图（短词纠短词）。
        int lenDiff = static_cast<int>(cand.size()) - static_cast<int>(misspelled.size());
        if (std::abs(lenDiff) > 2) dist += std::abs(lenDiff) - 2;

        // 距离更优直接采纳；距离相等时偏向长度相同的候选（更可能是同一词的不同写法）
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
