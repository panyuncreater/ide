// ============================================================
// CodeSnippetEngine.cpp — 代码模板 / Snippets 系统实现
// ------------------------------------------------------------
// 独立编译单元：仅依赖 Qt6::Core（QString），不依赖 CodeEditor /
// IdeController / Qt6::Widgets，可被 minilang_tests 测试目标直接链接。
// ============================================================

#include "gui/CodeSnippetEngine.h"

// ============================================================
// 内置模板库
// ------------------------------------------------------------
// 7 个基础模板覆盖 MiniLang 最常见的语法结构。
// 占位符格式 ${N:default}：
//   - N 从 1 开始，相同 N 的占位符在编辑器中应同步编辑
//   - default 是占位符的默认文本（用户可编辑替换）
//   - for 模板中 ${1:i} 出现 4 次，编辑任一处时其余处应同步更新
// ============================================================
const std::vector<CodeSnippet>& CodeSnippetEngine::snippets() {
    static const std::vector<CodeSnippet> kSnippets = {
        CodeSnippet{
            "fun",
            "fun ${1:函数名}(${2:参数列表}) : ${3:返回类型} {\n    ${4:// 函数体}\n}",
            "函数声明模板"
        },
        CodeSnippet{
            "class",
            "class ${1:类名} {\n    var ${2:属性} = ${3:默认值};\n    fun ${4:方法名}() {\n        ${5:// 方法体}\n    }\n}",
            "类声明模板"
        },
        CodeSnippet{
            "for",
            "for (var ${1:i} = 0; ${1:i} < ${2:10}; ${1:i} = ${1:i} + 1) {\n    ${3:// 循环体}\n}",
            "for 循环模板（含同步占位符）"
        },
        CodeSnippet{
            "if",
            "if (${1:条件}) {\n    ${2:// 条件为真}\n} else {\n    ${3:// 否则}\n}",
            "if-else 条件分支模板"
        },
        CodeSnippet{
            "while",
            "while (${1:条件}) {\n    ${2:// 循环体}\n}",
            "while 循环模板"
        },
        CodeSnippet{
            "try",
            "try {\n    ${1:// 可能出错}\n} catch (${2:e}) {\n    ${3:// 错误处理}\n}",
            "try-catch 异常处理模板"
        },
        CodeSnippet{
            "print",
            "print(${1:表达式});",
            "print 输出语句模板"
        },
        CodeSnippet{
            "var",
            "var ${1:name}: ${2:int} = ${3:0};",
            "变量声明（可选类型注解）"
        },
        CodeSnippet{
            "import",
            "import { ${1:name} } from \"${2:module}\";",
            "模块导入"
        },
        CodeSnippet{
            "export",
            "export ${1:fun} ${2:name}(${3:参数}) {\n    ${4:// 导出内容}\n}",
            "模块导出"
        },
        CodeSnippet{
            "return",
            "return ${1:value};",
            "返回语句"
        },
        CodeSnippet{
            "array",
            "var ${1:arr} = [${2:1, 2, 3}];\n${1:arr}.push(${3:4});\nprint(${1:arr}.len());",
            "数组操作"
        },
        CodeSnippet{
            "throw",
            "throw \"${1:错误信息}\";",
            "抛出异常"
        },
    };
    return kSnippets;
}

// ============================================================
// matchTrigger — 检测光标前的触发词
// ------------------------------------------------------------
// 算法：
//   1) 从 textBeforeCursor 末尾向前扫描，跳过尾随空白（如有）
//   2) 继续向前扫描连续的非空白字符，得到候选触发词 [start, end)
//   3) 检查触发词前方是否为空白或位于文档开头（start == 0 或
//      textBeforeCursor[start-1] 是空白）
//      —— 由于扫描在空白处停止，若 start > 0 则
//      textBeforeCursor[start-1] 必为空白
//   4) 将候选触发词与 snippet.trigger 精确匹配，返回首个匹配项
//
// 不命中场景：
//   - textBeforeCursor 为空 → 返回 nullptr
//   - 光标前全是空白 → 候选触发词为空，返回 nullptr
//   - 触发词未在 snippet 库中 → 返回 nullptr
// ============================================================
const CodeSnippet* CodeSnippetEngine::matchTrigger(const QString& textBeforeCursor) {
    if (textBeforeCursor.isEmpty()) return nullptr;

    const int total = textBeforeCursor.size();
    int end = total;

    // 跳过尾随空白（光标可能位于空白之后）
    while (end > 0 && textBeforeCursor[end - 1].isSpace()) {
        --end;
    }
    if (end == 0) return nullptr;  // 全是空白

    // 向前扫描连续的非空白字符
    int start = end;
    while (start > 0 && !textBeforeCursor[start - 1].isSpace()) {
        --start;
    }
    // 此时若 start > 0，则 textBeforeCursor[start-1] 必为空白（循环退出条件）
    // 若 start == 0，则触发词位于文档开头，同样满足"前方是空白或行首"

    const QString candidate = textBeforeCursor.mid(start, end - start);
    if (candidate.isEmpty()) return nullptr;

    const auto& all = snippets();
    for (const auto& snip : all) {
        if (candidate.toStdString() == snip.trigger) {
            return &snip;
        }
    }
    return nullptr;
}

// ============================================================
// expand — 展开模板
// ------------------------------------------------------------
// 算法：
//   1) 逐字符扫描 templateText，识别 ${N:default} 模式
//   2) 模式识别：'$' '{' [digits] ':' [default] '}'
//   3) 将 default 文本写入输出，记录 [start, end) 范围
//   4) 相同 N 的多次出现生成多个独立 range
//   5) primaryCursorPos = 第一个占位符的 start（用于编辑器初始定位）
//      若无占位符则为 -1（编辑器将光标置于展开文本末尾）
//
// 边界处理：
//   - "${" 后无 '}'：按字面量输出 "$" 和后续字符
//   - "${N}" 无冒号：default 为空，range 长度为 0
//   - "${:default}" 无 N：仍然识别为占位符（N 默认为 0）
//   - 嵌套 "${...${...}...}"：当前实现不支持嵌套，第一个 '}' 即结束
// ============================================================
CodeSnippetEngine::Expansion CodeSnippetEngine::expand(const CodeSnippet& snip) {
    Expansion result;
    const QString tpl = QString::fromStdString(snip.templateText);
    QString& output = result.text;

    int i = 0;
    const int n = tpl.size();
    while (i < n) {
        // 检测 "${" 起始
        if (i + 1 < n && tpl[i] == QChar('$') && tpl[i + 1] == QChar('{')) {
            // 查找匹配的 '}'
            int j = i + 2;
            while (j < n && tpl[j] != QChar('}')) {
                ++j;
            }
            if (j < n) {
                // 找到 "${...}"，解析内容
                const QString inner = tpl.mid(i + 2, j - i - 2);
                const int colonPos = inner.indexOf(QChar(':'));
                QString defaultText;
                if (colonPos >= 0) {
                    defaultText = inner.mid(colonPos + 1);
                } else {
                    // "${N}" 形式，无默认值
                    defaultText.clear();
                }
                const int rangeStart = output.size();
                output += defaultText;
                const int rangeEnd = output.size();
                result.placeholderRanges.push_back({rangeStart, rangeEnd});
                i = j + 1;
                continue;
            }
            // 未找到 '}'，按字面量输出 '$' 并继续
        }
        output += tpl[i];
        ++i;
    }

    // 设置 primaryCursorPos：第一个占位符的起点，或 -1 表示末尾
    if (!result.placeholderRanges.empty()) {
        result.primaryCursorPos = result.placeholderRanges.front().first;
    } else {
        result.primaryCursorPos = -1;
    }

    return result;
}
