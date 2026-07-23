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
    static std::string suggestSpelling(const std::string& target, const std::vector<std::string>& candidates);

    // ---- 错误消息增强 ----

    /// 增强错误消息：根据 category 和模式匹配附加教学性提示
    /// scopeVars 提供当前作用域变量名列表（用于未定义变量拼写建议）
    /// 未知模式原样返回 msg
    static std::string enrichErrorMessage(const std::string& msg, const std::string& category,
                                          const std::vector<std::string>& scopeVars);

    /// P2 fix (错误码优先匹配): 带 stable diagnostic code 的增强重载。
    /// 当 code 非空时优先按 code 查 errorPatterns 表附加对应教学提示，
    /// 避免「诊断消息文案变化（如英文/本地化）时子串匹配失效」的可维护性风险。
    /// code 为空或表里查不到时，回退到现有的中/英子串匹配兜底逻辑（向后兼容）。
    /// @param msg 原始错误消息
    /// @param code 稳定诊断码（如 "missing-semicolon"），与 errorPatterns() 表 tag 对应
    /// @param category 错误类别（"parse" / "runtime" / "type" 等）
    /// @param scopeVars 作用域变量名列表（用于未定义变量拼写建议）
    static std::string enrichErrorMessage(const std::string& msg, const std::string& code, const std::string& category,
                                          const std::vector<std::string>& scopeVars);

    // ---- P1-F12 fix: 错误模式表（带 tag），供手册「常见错误」表引用 ----

    /// 错误模式描述：tag / 标题 / 触发该错误的示例代码 / 所属类别 / 教学说明
    struct ErrorPattern {
        std::string tag;       // 稳定标识，如 "missing-semicolon"
        std::string title;     // 中文标题，如 "缺少分号"
        std::string buggyCode; // 触发该错误的最小 MiniLang 代码
        std::string category;  // 所属类别：parser / runtime / type
        // R117 新增：教学说明 markdown，用于错误列表 tooltip 与「为什么」浮层
        // 包含：根因 / 常见触发场景 / 修复模板 / 相关节点
        std::string teachingMarkdown;
    };

    /// 返回所有错误模式（带 tag）。手册「常见错误」表按 tag 引用此表，
    /// 配合 LabManualPanel 的 `buggy:tag` 链接实现「触发示例」按钮——
    /// 点击后自动加载 buggyCode 到编辑器并运行，让学员看到真实报错 +
    /// ErrorHintEngine 增强提示。引擎扩模式时手册自动同步，消除双份真相。
    static const std::vector<ErrorPattern>& errorPatterns();

    // ---- R117: 错误消息教学模式 API ----

    /// 按 tag 查找 ErrorPattern（用于 tooltip / 教学浮层渲染）
    /// @return 找到返回指针，未找到返回 nullptr
    static const ErrorPattern* findPattern(const std::string& tag);

    /// 渲染为 tooltip 富文本（HTML）：tag 标题 + 一句话根因 + 完整教学说明
    /// 用于错误列表 item 的 setToolTip（鼠标悬停显示教学说明）
    /// @param tag 错误模式 tag（如 "missing-semicolon"）
    /// @return HTML 富文本；tag 未找到返回空字符串（调用方回退到原始消息）
    static std::string renderTooltipHtml(const std::string& tag);

    /// 渲染为教学 markdown（用于 Flyout / Dialog 显示完整教学说明）
    /// @param tag 错误模式 tag
    /// @return markdown 文本；tag 未找到返回空字符串
    static std::string renderTeachingMarkdown(const std::string& tag);
};
