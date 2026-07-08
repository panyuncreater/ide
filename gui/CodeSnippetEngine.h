// ============================================================
// CodeSnippetEngine.h — 代码模板 / Snippets 系统
// ------------------------------------------------------------
// 功能 13（MINILANG_IDE_IMPROVEMENT_PLAN.md）：为初学者提供"脚手架"，
// 降低从零写代码的门槛。在编辑器中输入关键字后按 Tab 键展开模板，
// 展开后第一个占位符高亮，Tab 键在占位符之间跳转。
//
// 设计要点：
//   - 占位符格式 ${N:default}（VS Code 风格，N 从 1 开始）
//   - 相同 N 的占位符在模板中可出现多次（如 for 模板的 ${1:i}）
//   - 本引擎仅负责"匹配触发词"和"展开模板"，不处理编辑器交互
//   - 编辑器层（CodeEditor）负责 Tab 导航、占位符选择、范围追踪
//   - 独立编译单元：不依赖 CodeEditor / IdeController，可被测试目标链接
// ============================================================

#pragma once

#include <string>
#include <utility>
#include <vector>

#include <QString>

// ============================================================
// CodeSnippet — 单个代码模板定义
// ============================================================
struct CodeSnippet {
    std::string trigger;      // 触发关键字（如 "fun"）
    std::string templateText; // 模板文本（含 ${1:placeholder} 格式占位符）
    std::string description;  // 描述（用于 Ctrl+T 模板列表对话框）
};

// ============================================================
// CodeSnippetEngine — 代码模板引擎（静态工具类）
// ============================================================
class CodeSnippetEngine {
public:
    /// 获取内置模板列表（首次调用时初始化，后续调用零开销）
    static const std::vector<CodeSnippet>& snippets();

    /// 检测光标前的触发词；返回匹配的 snippet 或 nullptr
    /// textBeforeCursor: 从文档开头到光标位置的全部文本
    /// 触发词必须满足：
    ///   1) 是连续的非空白字符
    ///   2) 前方是空白或位于行首/文档开头
    static const CodeSnippet* matchTrigger(const QString& textBeforeCursor);

    /// 展开结果：包含展开后的文本与占位符位置列表
    struct Expansion {
        QString text;                                       // 展开后的文本（占位符已替换为默认值）
        std::vector<std::pair<int, int>> placeholderRanges; // [start, end) 字符偏移
        int primaryCursorPos = -1;                          // 展开后光标应定位的位置：第一个占位符起点，-1 表示末尾
    };

    /// 展开模板：解析 ${N:default} 占位符，替换为 default 文本，记录每个占位符的范围
    /// 相同 N 的占位符会生成多个独立的 range（编辑器层负责同步编辑）
    static Expansion expand(const CodeSnippet& snip);
};
