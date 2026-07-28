#include "gui/BugHuntPanel.h"

#include <sstream>

// ============================================================
// BugHuntLibrary — 题库数据
// ------------------------------------------------------------
// 题目源自项目历史真实 Bug（见 project_memory.md / CHANGELOG.md）。
// 每道题对应一个已修复的真实问题，学习者通过分析代码预测/验证 Bug 行为。
//
// 功能 9（Bug 狩猎分级题库重构）新增 difficulty 字段：
//   BEGINNER      — 入门级（阅读理解型，5 道 BUG-READ-01..05）
//   INTERMEDIATE  — 进阶级（4 道：BUG-DEF-1 / BUG-REGVM-2 / BUG-REPL-1 / BUG-F-04）
//   EXPERT        — 专家级（6 道，按题库顺序：BUG-CP-1 / BUG-CP-2 / BUG-UV-1 /
//                   BUG-IR-POP-2 / BUG-MOD-1 / BUG-DBG-1）
// ============================================================

/// 返回 Bug 狩猎题库的全部条目（静态单例），按难度分组：
/// BEGINNER（5 道）/ INTERMEDIATE（4 道）/ EXPERT（6 道）。
/// 每道题对应一个已修复的真实历史 Bug，含预测/验证用源码、提示与解析，
/// 供 BugHuntPanel 渲染与交互。
const std::vector<BugHuntItem>& BugHuntLibrary::items() {
    static const std::vector<BugHuntItem> kItems = {
        // ====================================================
        // 🟢 入门级（BEGINNER）— 阅读理解型，培养 Debug 直觉
        // ====================================================
        BugHuntItem{"BUG-READ-01",
                    "🌱 预测输出：整数除法截断向零",
                    "P2",
                    "整数除法 / 三后端一致性",
                    "🔍 下面代码在三后端分别输出什么？\n"
                    "MiniLang 整数除法截断向零（三后端统一）。",
                    "print(7 / 2);",
                    "✅ 三后端都输出 3（整数除法截断向零）",
                    "🎯 无 Bug，重点是理解整数除法语义",
                    {"💡 提示1: 7/2 数学上是 3.5", "💡 提示2: MiniLang 整数除法截断向零", "💡 提示3: 三后端统一为 3"},
                    "📖 这道题要你记住的：MiniLang 的整数除法是「截断向零」的，而且三条执行路径都得这么算。"
                    "7/2 = 3.5 截断向零 = 3，Interpreter / StackVM / RegisterVM 三条路径均输出 3。\n"
                    "注意：7.0/2 才会得到 float 3.5（类型由操作数决定）。",
                    BugHuntDifficulty::BEGINNER},
        BugHuntItem{
            "BUG-READ-02",
            "🌱 定位异常行：块作用域变量不可见",
            "P2",
            "块作用域 / 变量可见性",
            "🔍 下面代码运行时输出 'Undefined variable b'，问 b 是在哪个作用域未定义？",
            "fun outer() {\n    var a = 1;\n    if (a > 0) {\n        var b = 2;\n    }\n    print(b);\n}\nouter();",
            "✅ b 在 if 块作用域内定义，块外不可访问",
            "⚠️ 运行时报 'Undefined variable b'",
            {"💡 提示1: b 在哪个块内定义？", "💡 提示2: 块作用域退出后变量生命周期结束",
             "💡 提示3: MiniLang 块作用域规则"},
            "📖 记住这条铁律：var 写在 if 块里，就只在那块里活着；块一关，外面谁也找不着它。"
            "MiniLang 中 var 声明的变量生命周期限于其所在的块作用域（{ } 包裹区域）。\n"
            "if 块退出后 b 即被销毁，print(b) 在外层作用域找不到 b，故报未定义。",
            BugHuntDifficulty::BEGINNER},
        BugHuntItem{"BUG-READ-03",
                    "🌱 格式化预测：字典字面量展开规则",
                    "P2",
                    "Formatter / 格式化规则",
                    "🔍 下面代码经 Formatter 格式化后输出是什么？（选择题）\n"
                    "A. 一行\nB. 多行展开\nC. 保持原样",
                    "var x={\"a\":1,\"b\":2};",
                    "✅ Formatter 输出：\nvar x = {\"a\": 1, \"b\": 2};",
                    "🎯 无 Bug，理解 Formatter 字典格式化规则",
                    {"💡 提示1: Formatter 保持单行格式", "💡 提示2: 字典键必须用双引号字符串字面量",
                     "💡 提示3: Formatter 不自动加引号也不展开多行"},
                    "📖 这道题点出一个事实：Formatter 对字典字面量保持单行格式，"
                    "键值对之间加空格，但不自动展开多行也不为裸标识符加引号。\n"
                    "MiniLang 要求字典键必须是字符串字面量（用双引号包裹），"
                    "违反此规则的代码会在运行时报错。\n"
                    "答案选 A（一行）。",
                    BugHuntDifficulty::BEGINNER},
        BugHuntItem{
            "BUG-READ-04",
            "🌱 性能直觉：循环内 vs 循环外 var 声明",
            "P2",
            "性能 / 变量分配",
            "🔍 下面两段代码哪个更快？为什么？",
            "// 代码 A\nfor (var i = 0; i < 100; i = i + 1) {\n    var x = i * 2;\n    print(x);\n}\n\n// 代码 B\nvar "
            "x = 0;\nfor (var i = 0; i < 100; i = i + 1) {\n    x = i * 2;\n    print(x);\n}",
            "✅ 代码 B 更快（变量在循环外声明，避免每次迭代的 var 开销）",
            "🎯 无 Bug，理解性能差异",
            {"💡 提示1: var 声明有开销吗？", "💡 提示2: 循环内 var vs 循环外 var", "💡 提示3: 考虑变量分配的开销"},
            "📖 这道题比的是性能直觉：var 写在循环里，每轮都重新分配 slot；挪到外面只分配一次，更快。"
            "代码 A 每次迭代都执行 var x 声明（分配新 slot），代码 B 复用外层 slot 仅做赋值。\n"
            "在 StackVM/RegisterVM 路径下，代码 B 的指令数更少，执行更快。",
            BugHuntDifficulty::BEGINNER},
        BugHuntItem{"BUG-READ-05",
                    "🌱 内存推理：COW（写时复制）refCount 变化",
                    "P2",
                    "COW / 内存模型",
                    "🔍 执行下面代码后，a 和 b 各自引用的 ArrayData 的 refCount 是多少？",
                    "var a = [1, 2, 3];\nvar b = a;\nb.push(4);",
                    "✅ 执行 b.push(4) 后，a 的 refCount=1（原 ArrayData），b 的 refCount=1（COW 后新 ArrayData）",
                    "🎯 无 Bug，理解 COW（Copy-On-Write）语义",
                    {"💡 提示1: a=b 共享同一个 ArrayData", "💡 提示2: b.push 触发 COW detach",
                     "💡 提示3: COW 后 refCount 各为 1"},
                    "📖 这道题讲 COW：对 b 调 push，触发 detach，b 会拷贝出一份新的 ArrayData，原来的那份不动。"
                    "var b = a 时共享同一 ArrayData（refCount=2）；b.push 写操作触发 COW，\n"
                    "b 拷贝出新 ArrayData，原 ArrayData refCount 降为 1，新 ArrayData refCount=1。\n"
                    "此时 a 仍指向原数组 [1,2,3]，b 指向新数组 [1,2,3,4]，互不影响。",
                    BugHuntDifficulty::BEGINNER},
        // ====================================================
        // 🟡 进阶级（INTERMEDIATE）— 需阅读代码、定位问题区域
        // ====================================================
        BugHuntItem{"BUG-DEF-1",
                    "🔧 默认参数表达式：三后端不一致",
                    "P2",
                    "默认参数 / 三后端一致性",
                    "🔍 Interpreter 路径支持任意表达式默认值（b=a+1 求值为 6），VM/RegisterVM 路径仅支持字面量，"
                    "复杂表达式记录 0xFFFF 哨兵运行时报错。违反三后端一致性。",
                    "fun f(a = 5, b = a + 1) {\n    return b;\n}\nprint(f());",
                    "✅ 修复后：Parser 层拒绝复杂表达式默认值，仅允许字面量 + 负数字面量。",
                    "⚠️ Bug 触发时：Interpreter 返回 6，VM/RegisterVM 运行时错误。",
                    {"💡 提示1：检查 Parser::isLiteralDefaultExpr 哪些节点被允许？",
                     "💡 提示2：BinaryOp/FunCall 等表达式不应作为默认值。",
                     "💡 提示3：UnaryOp(NEGATE) 套 NumberLiteral 应该被允许（负数字面量）。"},
                    "📖 根因其实很朴素：三个后端对「默认参数能算多复杂」必须步调一致。修法是 Parser "
                    "把关——复杂表达式直接拒，只放字面量（含负数）。"
                    "修复点：parser/Parser.cpp isLiteralDefaultExpr + 三后端同步。",
                    BugHuntDifficulty::INTERMEDIATE},
        BugHuntItem{
            "BUG-REGVM-2",
            "🔧 RegisterVM 单步：丢失步进事件",
            "P2",
            "RegisterVM / 调试器",
            "🔍 REG_RETURN/REG_RETURN_NULL/REG_THROW 通过直接 return 跳过函数末尾统一 stepCallback_ 调用，"
            "调试器单步模式丢失步进事件。",
            "// 启用 VM 单步调试后执行：\nfun f() { return 1; }\nprint(f());",
            "✅ 修复后：单步调试时每个 RETURN 指令触发步进回调。",
            "⚠️ Bug 触发时：RETURN 指令不触发步进回调，单步跳过。",
            {"💡 提示1：检查 executeCalls 中 REG_RETURN 的处理路径——是 return 直接返回，还是 goto 到统一调用点？",
             "💡 提示2：StackVM 通过 notifyStep(ip, op) 在所有指令后统一调用，RegisterVM 是否对齐？",
             "💡 提示3：修复：捕获结果，若 VM_OK 则调用 stepCallback_。"},
            "📖 根因：RegisterVM 的 stepCallback_ 在「直接 return」那条捷径上被跳过了，于是调试器单步模式丢了步进事件。"
            "修复点：compiler/RegisterVM.cpp executeCalls() + executeMisc()。",
            BugHuntDifficulty::INTERMEDIATE},
        BugHuntItem{"BUG-REPL-1",
                    "💻 clearModuleCache 路径规范化缺失",
                    "P2",
                    "REPL / 模块系统",
                    "🔍 visitImportStmt 将 \\ 转 / 并去除 ./ 前缀后存入 moduleCache_，clearModuleCache 直接用原始 path "
                    "查 erase 无法命中。",
                    "// 在 REPL 中：\nimport { x } from \"./utils.mini\";\n// 然后修改 utils.mini 后尝试 reload：\n// "
                    "reload 命令内部调用 clearModuleCache(\"./utils.mini\") 但实际缓存 key 是 \"utils.mini\"",
                    "✅ 修复后：reload 命令正确清缓存，下次 import 重新加载源码。",
                    "⚠️ Bug 触发时：reload 命令无效，仍使用旧模块。",
                    {"💡 提示1：对比 visitImportStmt 存入 moduleCache_ 的 key 与 clearModuleCache 接收的 path。",
                     "💡 提示2：visitImportStmt 是否做了路径规范化（\\→/、strip ./）？",
                     "💡 提示3：clearModuleCache 需同步做相同规范化。"},
                    "📖 根因：路径规范化这件事，必须在每一个碰 moduleCache_ 的入口都做一致，否则就串味了。"
                    "修复点：interpreter/Interpreter.h clearModuleCache()。",
                    BugHuntDifficulty::INTERMEDIATE},
        BugHuntItem{"BUG-F-04",
                    "🔧 Formatter：ExportStmt 间未插入空行",
                    "P2",
                    "Formatter / 往返等价",
                    "🔍 isFunOrClass lambda 未解包 ExportStmt 内部声明，导致 export var x 与 export fun f 之间无空行，"
                    "与未导出形式（var x 紧跟 fun f）的格式化结果不一致。",
                    "export var x = 1;\nexport fun f() { return 1; }",
                    "✅ 修复后：ExportStmt 之间插入空行（与未导出形式一致）。",
                    "⚠️ Bug 触发时：两条 export 紧挨，与未导出形式不一致。",
                    {"💡 提示1：检查 Formatter 中 isFunOrClass lambda 的判定——是否检查 NODE_EXPORT_STMT 内部？",
                     "💡 提示2：导出形式应与未导出形式格式化结果一致。",
                     "💡 提示3：修复：isFunOrClass 解包 NODE_EXPORT_STMT 检查内部 declaration 节点类型。"},
                    "📖 根因：Formatter 决定空行时，得把 ExportStmt 里包着的那层声明节点类型也算进去。"
                    "修复点：formatter/Formatter.cpp isFunOrClass lambda。",
                    BugHuntDifficulty::INTERMEDIATE},
        // ====================================================
        // 🔴 专家级（EXPERT）— 需深入理解引擎架构、跨模块追踪
        // ====================================================
        BugHuntItem{
            "BUG-CP-1",
            "🚀 常量池去重陷阱：int(0) 与 float(0.0)",
            "P1",
            "常量池去重 / 三后端一致性",
            "🔍 BytecodeChunk::addConstant 与 RegBytecodeChunk::addConstant 常量池去重时仅依赖 Value::equals()，"
            "int(0) 与 float(0.0) 因数值相等被误判为同一常量共享索引。"
            "这破坏了三后端语义一致性——整数除法 7/2 应为 3，浮点除法 7.0/2 应为 3.5，"
            "若常量池将 0 与 0.0 共享，可能让某一后端走错算术分支。",
            "var a = 7 / 2;\nvar b = 7.0 / 2;\nprint(a);\nprint(b);",
            "✅ 修复后：a=3（int 截断向零），b=3.5（float 精确）。",
            "⚠️ Bug 触发时：a 与 b 中可能有一个值错乱。",
            {"💡 提示1：检查 BytecodeChunk::addConstant 的去重条件——只比较 Value::equals() 是否足够？",
             "💡 提示2：int(0) 与 float(0.0) 的 getType() 不同，但 equals() 返回 true。",
             "💡 提示3：修复需要先比较 getType()，再比较 equals()。"},
            "📖 根因：Value::equals() 只比数值、不管类型。可常量池去重该按类型区分时，它就没辙了。"
            "先比较 getType() 再调用 equals()，否则 int(0) 与 float(0.0) 会被误判为同一常量。"
            "修复点：compiler/Bytecode.h addConstant() + compiler/RegisterBytecode.cpp "
            "RegBytecodeChunk::addConstant()。",
            BugHuntDifficulty::EXPERT},
        BugHuntItem{"BUG-CP-2",
                    "🚀 嵌套索引赋值栈泄漏：a[0][1]=x",
                    "P1",
                    "嵌套左值 / 栈平衡",
                    "🔍 OP_WRITEBACK_INDEX_VAR/LOCAL 改为整体替换语义（不 pop 索引）后，visitIndexAssign 仍向"
                    "栈推入外层索引。循环内每次迭代泄漏 2 个栈值，约 512 次循环触发栈溢出。仅影响非 IR 路径。",
                    "var arr = [[0,0],[0,0],[0,0]];\nfor (var i = 0; i < 600; i = i + 1) {\n    arr[0][1] = "
                    "i;\n}\nprint(arr[0][1]);",
                    "✅ 修复后：循环正常执行，arr[0][1] == 599。",
                    "⚠️ Bug 触发时：循环约 512 次后栈溢出崩溃。",
                    {"💡 提示1：在 visitIndexAssign 中检查 OP_WRITEBACK_INDEX_* 后栈的 push/pop 数量是否匹配。",
                     "💡 提示2：对比 visitMethodCall 的同步修复——它已经不推索引，visitIndexAssign 是否漏改？",
                     "💡 提示3：在 IR 路径不触发是因为 BytecodeIRBackend 用 vreg 物化栈值，不依赖物理栈平衡。"},
                    "📖 根因：你一动「整体替换」语义的指令，就得同时去查 "
                    "visitIndexAssign、visitMemberAssign、visitMethodCall 这三个 emit 点，一个都不能漏。"
                    "避免遗漏推/不推索引导致栈泄漏。修复：删除 L1953-1960 推索引代码对齐 visitMethodCall。",
                    BugHuntDifficulty::EXPERT},
        BugHuntItem{"BUG-UV-1",
                    "🚀 三层嵌套闭包：自由变量捕获断裂",
                    "P1",
                    "闭包 / upvalue 捕获",
                    "🔍 Compiler 路径缺少前向自由变量分析，resolveUpvalue 是惰性的（仅 visitVarRef 触发）。"
                    "3 层嵌套时中间函数不直接引用外层变量则 currentUpvalueNames_ 为空，内层函数无法透传捕获。",
                    "fun outer() {\n    var x = 1;\n    fun mid() {\n        fun inner() {\n            return x;\n    "
                    "    }\n        return inner();\n    }\n    return mid();\n}\nprint(outer());",
                    "✅ 修复后：输出 1。",
                    "⚠️ Bug 触发时：运行时错误（无法找到 upvalue）。",
                    {"💡 提示1：resolveUpvalue 沿外层帧查找——但中间函数若没显式引用 x，则不会创建 upvalue 槽位。",
                     "💡 提示2：IR 路径有 computeFreeVars 前向分析修复了此问题，Compiler 路径没有。",
                     "💡 提示3：修复方案：在 visitFunDecl 加入类似 IR 的 computeFreeVars 前向分析。"},
                    "📖 根因：resolveUpvalue 的惰性策略，遇到 3 "
                    "层以上嵌套闭包就失灵；中间那层函数得先声明自己要抓哪些外层变量。"
                    "修复点：compiler/Compiler.cpp resolveUpvalue() + visitFunDecl()。",
                    BugHuntDifficulty::EXPERT},
        BugHuntItem{
            "BUG-IR-POP-2",
            "🚀 IR 路径 if 单语句体未 POP",
            "P0",
            "IR 路径 / 栈平衡",
            "🔍 IR 路径 visitIfStmt then/else 分支直接调用 visitNode，未通过 visitStatement 统一包装器处理 POP。"
            "循环内 `if (cond) foo();` 每次泄漏 1 个栈值，多次循环触发栈溢出。",
            "var i = 0;\nwhile (i < 500) {\n    if (i > 0) print(\"hi\");\n    i = i + 1;\n}",
            "✅ 修复后：循环正常执行，约 499 次打印 hi。",
            "⚠️ Bug 触发时：栈溢出崩溃（约 1024 次泄漏后）。",
            {"💡 提示1：检查 IR 路径 visitIfStmt 的 then/else 分支调用——是 visitNode 还是 visitStatement？",
             "💡 提示2：visitStatement 是统一包装器，会对表达式语句 emit POP。",
             "💡 提示3：同样的 Bug 也存在于 visitWhileStmt 和 visitForStmt 的单语句体路径。"},
            "📖 根因：IR 路径里，语句上下文得用 visitStatement 这个统一包装器来发 POP，不能图省事直接调 visitNode。"
            "修复点：compiler/IR.cpp visitIfStmt() + visitWhileStmt() + visitForStmt()。",
            BugHuntDifficulty::EXPERT},
        BugHuntItem{"BUG-MOD-1",
                    "🚀 模块路径：'C:foo' 漏网",
                    "P2",
                    "模块路径安全",
                    "🔍 normalizeModulePath 仅检测 'C:/' 形式未拒绝 'C:foo'（Windows 驱动器相对路径），"
                    "反斜杠在更早处已统一转为正斜杠使原反斜杠分支成为死代码。",
                    "// 触发路径遍历尝试（应被拒绝）\nimport { x } from \"C:etc/passwd\";",
                    "✅ 修复后：拒绝所有 'X:' 开头形式。",
                    "⚠️ Bug 触发时：可能加载到非预期文件。",
                    {"💡 提示1：检查 normalizeModulePath 的 'X:' 检测分支——只检测 'X:/' 还是所有 'X:' 开头？",
                     "💡 提示2：反斜杠在何处被转为正斜杠？若已转，原反斜杠分支是死代码。",
                     "💡 提示3：修复需拒绝所有 path.size() >= 2 && path[1] == ':' 形式。"},
                    "📖 根因：路径安全校验不能只盯着 'X:/'，凡是以 'X:' 开头的形式都得挡回去。"
                    "修复点：compiler/Compiler.cpp normalizeModulePath() + InterpreterModules.cpp + IR.cpp 三处同步。",
                    BugHuntDifficulty::EXPERT},
        BugHuntItem{"BUG-DBG-1",
                    "🚀 调用栈顶帧行号：调用点 vs 当前行",
                    "P1",
                    "调试器 / 调用栈显示",
                    "🔍 CallFrame::line 在 callNamedFunction 等处初始化为调用点行号后从不更新。"
                    "调试器暂停时，调用栈顶显示的是调用点行号，而非当前执行行号，导致用户困惑。",
                    "fun inner() {\n    print(\"now at inner line 2\");  // 实际暂停行\n}\nfun outer() {\n    inner(); "
                    " // 调用点行号\n}\nouter();",
                    "✅ 修复后：调用栈顶帧行号显示当前执行行号。",
                    "⚠️ Bug 触发时：调用栈顶显示调用点行号。",
                    {"💡 提示1：检查 callNamedFunction 中 CallFrame.line 初始化值。",
                     "💡 提示2：checkBreak 是否在某处更新 line？",
                     "💡 提示3：修复需在每次暂停时更新 callStack_.back().line 为当前节点行号。"},
                    "📖 根因：调用栈帧的 line 字段得跟着执行进度走，不能只在调用那一刻设一次就完事。"
                    "修复点：interpreter/Interpreter.cpp checkBreak()。",
                    BugHuntDifficulty::EXPERT}};
    return kItems;
}
