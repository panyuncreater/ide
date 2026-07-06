// ============================================================
// ErrorHintEngine.h — 错误信息友好化增强引擎（功能 12）
// ------------------------------------------------------------
// 提供两大核心能力：
//   1. 拼写建议（Levenshtein 编辑距离 ≤ 2）
//   2. 常见错误模式匹配（缺分号 / 括号不匹配 / 未定义变量 / 除零 / 越界 / 类型错误）
//
// 设计目标：
//   - 不依赖引擎层，纯静态工具类
//   - 中文 + 英文错误消息均可匹配
//   - 未知模式原样返回（无附加提示）
// ============================================================

#pragma once

#include <string>
#include <vector>

class ErrorHintEngine {
public:
    // ---- 拼写建议 ----

    /// 计算 Levenshtein 编辑距离（按字节比较，大小写敏感）
    static int levenshteinDistance(const std::string& a, const std::string& b);

    /// 在 candidates 中找到与 target 编辑距离最小且 ≤ 2 的候选
    /// 返回最匹配的候选；无匹配时返回空字符串
    /// 距离为 0（完全相同）的候选不返回（视为同一名称）
    static std::string suggestSpelling(const std::string& target,
                                       const std::vector<std::string>& candidates);

    // ---- 错误消息增强 ----

    /// 增强错误消息：根据 category 和模式匹配附加教学性提示
    /// scopeVars 提供当前作用域变量名列表（用于未定义变量拼写建议）
    /// 未知模式原样返回 msg
    static std::string enrichErrorMessage(const std::string& msg,
                                          const std::string& category,
                                          const std::vector<std::string>& scopeVars);
};
