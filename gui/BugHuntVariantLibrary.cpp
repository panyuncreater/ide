// ============================================================
// BugHuntVariantLibrary.cpp — 变体题库数据（独立编译单元）
// ------------------------------------------------------------
// 从 BugHuntPanel.cpp 拆分而来，避免测试目标链接 BugHuntPanel.cpp
// 时引入 IdeController.h 依赖（IdeController 依赖 app/ 下多个文件）。
// 本文件仅包含 BugHuntVariantLibrary::variants() 静态数据，无任何
// 引擎层或 GUI 层依赖。
// ============================================================

#include "gui/BugHuntPanel.h"

// ============================================================
// BugHuntVariantLibrary — 变体题库数据
// ------------------------------------------------------------
// 基于现有 BugHuntLibrary 的 7 个父题生成变体，修改参数/结构/反向挑战。
// 每个变体的 sourceCode 必须是可被 Lexer 解析的有效 MiniLang 代码。
// ============================================================

const std::vector<BugHuntVariant>& BugHuntVariantLibrary::variants() {
    static const std::vector<BugHuntVariant> kVariants = {
        // 1. variant-cp-1-param：常量池去重 int→float 跨类型
        BugHuntVariant{
            "variant-cp-1-param",
            "BUG-CP-1",
            "🔬 常量池去重变体：int 与 float 跨类型去重",
            "🔍 差异：原题使用 1/2（int）与 1.0/2.0（float）测试常量池去重；"
            "本变体将参数改为 1.5/2.5（float）与 1/2（int）混合，并增加 1.5/2.5 对比，"
            "考察跨类型常量去重在浮点除法场景下是否仍触发 int(0) 与 float(0.0) 共享索引问题。"
            "新挑战点：浮点常量 1.5 和 2.5 是否会与整型常量发生类型混淆？",
            "var a = 1.5 / 2;\nvar b = 1 / 2;\nvar c = 1.5 / 2.5;\nprint(a);\nprint(b);\nprint(c);",
            "🎯 预测三条路径下 a/b/c 的输出值是否一致",
            "✅ a=0.75（float 除法），b=0（int 截断向零），c=0.6（float 精确）",
            "💡 检查 addConstant 是否先比较 getType() 再调用 equals()——浮点常量池去重同样需要类型区分。"
        },
        // 2. variant-uv-1-closure：闭包嵌套层级提升到 4 层
        BugHuntVariant{
            "variant-uv-1-closure",
            "BUG-UV-1",
            "🔗 闭包变体：4 层嵌套自由变量捕获",
            "🔍 差异：原题为 3 层嵌套（outer→mid→inner），本变体提升到 5 层（f1→f2→f3→f4→f5），"
            "中间 f2/f3/f4 均不直接引用 x，仅 f5 返回 x。"
            "新挑战点：4 层惰性 upvalue 透传是否仍能在 resolveUpvalue 逐级回溯中正确创建中间槽位？",
            "fun f1() {\n    var x = 1;\n    fun f2() {\n        fun f3() {\n            fun f4() {\n                fun f5() {\n                    return x;\n                }\n                return f5();\n            }\n            return f4();\n        }\n        return f3();\n    }\n    return f2();\n}\nprint(f1());",
            "🎯 预测输出：是否能正确打印 1，还是运行时报错",
            "✅ 输出 1（若 computeFreeVars 前向分析正确传播）或运行时错误（若仍为惰性策略）",
            "💡 对比 IR 路径与 Compiler 路径——IR 有 computeFreeVars，Compiler 是否对齐？"
        },
        // 3. variant-ir-pop-2-expr：表达式语句含 try/catch（异常路径栈泄漏）
        BugHuntVariant{
            "variant-ir-pop-2-expr",
            "BUG-IR-POP-2",
            "📜 IR 栈泄漏变体：try/catch 异常路径表达式语句",
            "🔍 差异：原题为 while 循环内 if 单语句体泄漏；本变体将 if 语句包入 try/catch 中，"
            "考察异常处理路径下 visitStatement 的 POP 包装器是否仍正确覆盖 try 体末尾的表达式语句。"
            "新挑战点：try 块中的表达式语句抛异常后，catch 块执行完毕时栈是否平衡？",
            "var i = 0;\nwhile (i < 100) {\n    try {\n        if (i > 0) print(\"hi\");\n    } catch (e) {\n        print(\"err\");\n    }\n    i = i + 1;\n}",
            "🎯 预测循环是否能正常完成 100 次，还是中途栈溢出",
            "✅ 循环正常完成，约 99 次打印 hi（无异常路径泄漏）",
            "💡 检查 IR 路径 visitTryStmt 是否也通过 visitStatement 包装 then/else/catch 体。"
        },
        // 4. variant-def-1-default：默认参数从 1 个改为 3 个全部默认
        BugHuntVariant{
            "variant-def-1-default",
            "BUG-DEF-1",
            "🔧 默认参数变体：3 个参数全部默认",
            "🔍 差异：原题为 2 个参数（a=5, b=a+1 复杂表达式）；本变体改为 3 个参数全部字面量默认值，"
            "考察默认参数填充逻辑在参数个数 > 2 时是否正确处理部分传参（f(10) 只填 b/c）。"
            "新挑战点：三后端对 f()/f(10)/f(10,20)/f(10,20,30) 的部分默认填充是否一致？",
            "fun f(a = 1, b = 2, c = 3) {\n    return a + b + c;\n}\nprint(f());\nprint(f(10));\nprint(f(10, 20));\nprint(f(10, 20, 30));",
            "🎯 预测四次调用的输出：6 / 15 / 33 / 60",
            "✅ 输出 6, 15, 33, 60（三后端一致）",
            "💡 检查 fillDefaultArgs 在 argCount < arity 时从右向左填充默认值的顺序。"
        },
        // 5. variant-mod-1-cycle：从 2 模块循环依赖改为 3 模块循环依赖
        BugHuntVariant{
            "variant-mod-1-cycle",
            "BUG-MOD-1",
            "📦 模块变体：3 模块循环依赖",
            "🔍 差异：原题为 2 模块（a↔b）循环依赖；本变体扩展为 3 模块链式循环（a→b→c→a），"
            "考察 moduleLoadingSet_ 循环检测在 3 模块链路下是否能正确识别并报错。"
            "新挑战点：3 模块循环的检测深度——moduleLoadingStack_ 是否记录完整路径？",
            "// 模拟三模块循环依赖（需配合模块文件）\n// a.mini: import { x } from \"b.mini\";\n// b.mini: import { y } from \"c.mini\";\n// c.mini: import { z } from \"a.mini\";\nimport { x } from \"b.mini\";\nprint(x);",
            "🎯 预测：三后端是否都能检测到循环依赖并报错（而非死循环）",
            "⚠️ 运行时错误：检测到循环依赖 a→b→c→a",
            "💡 检查 moduleLoadingSet_/moduleLoadingStack_ 在 3 模块链路下的检测逻辑是否完整。"
        },
        // 6. variant-dbg-1-condition：条件断点表达式改为 arr.len() == 3
        BugHuntVariant{
            "variant-dbg-1-condition",
            "BUG-DBG-1",
            "🐞 调试变体：条件断点表达式含方法调用",
            "🔍 差异：原题条件断点为 i == 5（简单变量比较）；本变体改为 arr.len() == 3（含方法调用），"
            "考察条件断点求值器是否能正确处理 MemberCall/方法调用表达式。"
            "新挑战点：方法调用作为断点条件时，VM 条件断点求值是否走完整的方法分派路径？",
            "var arr = [1, 2, 3];\nfor (var i = 0; i < arr.len(); i = i + 1) {\n    print(arr[i]);\n}",
            "🎯 预测：条件断点 arr.len() == 3 是否在第 3 次迭代时正确命中",
            "✅ 循环正常执行 3 次，打印 1, 2, 3",
            "💡 检查 VM 条件断点求值路径是否支持 BuiltinMethod 调用（len() 是内建方法）。"
        },
        // 7. variant-regvm-2-vreg：vreg 数量从 8 个增加到 28 个（接近 32 上限）
        BugHuntVariant{
            "variant-regvm-2-vreg",
            "BUG-REGVM-2",
            "⚙️ 寄存器变体：28 个虚拟寄存器（接近 32 上限）",
            "🔍 差异：原题仅 8 个局部变量；本变体声明 28 个局部变量并求和，"
            "考察 RegisterBytecodeBackend 的 vreg 分配在接近 32 上限时是否触发溢出检查。"
            "新挑战点：28 个 vreg + 临时寄存器是否超过 MAX_REGISTERS=32 导致编译错误？",
            "fun f() {\n    var a1 = 1;\n    var a2 = 2;\n    var a3 = 3;\n    var a4 = 4;\n    var a5 = 5;\n    var a6 = 6;\n    var a7 = 7;\n    var a8 = 8;\n    var a9 = 9;\n    var a10 = 10;\n    var a11 = 11;\n    var a12 = 12;\n    var a13 = 13;\n    var a14 = 14;\n    var a15 = 15;\n    var a16 = 16;\n    var a17 = 17;\n    var a18 = 18;\n    var a19 = 19;\n    var a20 = 20;\n    var a21 = 21;\n    var a22 = 22;\n    var a23 = 23;\n    var a24 = 24;\n    var a25 = 25;\n    var a26 = 26;\n    var a27 = 27;\n    var a28 = 28;\n    return a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9 + a10 + a11 + a12 + a13 + a14 + a15 + a16 + a17 + a18 + a19 + a20 + a21 + a22 + a23 + a24 + a25 + a26 + a27 + a28;\n}\nprint(f());",
            "🎯 预测：RegisterVM 是否能编译并执行（28 vreg < 32），还是触发 vreg 溢出",
            "✅ 输出 406（1+2+...+28 = 406），三后端一致",
            "💡 检查 RegisterBytecodeBackend 的 vreg 分配上限检查——28 个局部 + 求和临时寄存器是否超 32。"
        }
    };
    return kVariants;
}
