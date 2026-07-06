# MiniLang IDE

![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![Qt6](https://img.shields.io/badge/Qt-6-green)
![Build](https://img.shields.io/badge/build-passing-brightgreen)
![Tests](https://img.shields.io/badge/tests-1693-brightgreen)
![License](https://img.shields.io/badge/license-MIT-blue)

一个用 C++20 / Qt6 构建的轻量级教学型编程语言集成开发环境，包含自研词法分析器、递归下降解析器、栈式字节码虚拟机、树遍历解释器、调试器、代码格式化器、IR 中间表示层与完整 GUI。

本项目旨在通过从零实现一门完整编程语言（词法分析 -> 解析 -> 解释/编译 -> 虚拟机）来教授编译原理与运行时设计的核心概念。所有实现均可在 IDE 中可视化追踪，包括 Token 流、AST 树形图、字节码反汇编、IR 可视化与调试器单步执行。适合对编译原理、语言实现或运行时系统感兴趣的开发者与学习者。

与同类教学项目（如 Crafting Interpreters 的 Lox）不同，MiniLang 同时实现了**三套执行引擎**（树遍历解释器、栈式字节码 VM、寄存器式 VM）并共享一套 IR 中间表示层，配合完整的 Qt GUI 可视化面板，让学习者可以直观对比不同执行模型的差异与取舍。

## 界面预览

> 截图待补充。计划包含：
>
> - IDE 主界面（编辑器 + AST 视图 + 调试面板）
> - REPL 交互面板
> - IR 可视化面板
>
> 如需添加截图，请将图片放入 docs/screenshots/ 目录并在此处引用。

## 目录

- [界面预览](#界面预览)
- [功能特性](#功能特性)
- [架构](#架构)
- [构建与运行](#构建与运行)
- [快速开始](#快速开始)
- [开发环境](#开发环境)
- [测试](#测试)
- [语言示例](#语言示例)
- [示例程序](#示例程序)
- [贡献](#贡献)
- [许可证](#许可证)

## 功能特性

### 语言能力

- **基础类型**：`int` / `float` / `bool` / `string` / `null`，支持类型注解（`int a = 5;`）。`int` 支持 int48 范围内联存储，超范围自动装箱；`string` 按 UTF-8 码位语义处理（length/substr/indexOf 均按码位计数）
- **变量与赋值**：`var x = 10;` 动态声明，也支持类型注解。支持类型注解，三后端统一强制。null 兼容所有类型注解，float 注解接受 int 值（宽化），int 注解拒绝 float 值
- **控制流**：`if/else`、`while`、`for (init; cond; upd)`，支持嵌套块作用域。循环支持 `break`/`continue`，`continue` 跳过当前迭代，`break` 退出循环
- **函数**：`fun f(x) { ... }`，支持闭包与 upvalue 捕获（3+ 层）。函数无提升——`f(); fun f() {}` 会报"未定义的函数"。支持默认参数（`fun f(a=1, b=2)`），参数从左到右求值，不允许前向引用
- **类与继承**：`class A extends B { ... }`，支持 `super.method()` 调用与字段同步回实例。类继承父类名存字符串运行时查找，支持跨行定义（REPL 中先 `class A {}` 再 `class B extends A {}`）。类方法自动预留 slot 0 给 `this`，字段按继承链展平存储。三后端 super 调用语义一致
- **数据结构**：数组 `[1, 2, 3]`、字典 `{"key": val}`，支持索引读写。字典键强制为 string，键不存在时返回 null。数组/字典使用 Copy-On-Write（COW），写前检查独占所有权
- **运算符**：算术 `+ - * / %`、比较 `== != < > <= >=`、逻辑 `and or not`。整数除法截断向零（三后端统一）。`and/or` 短路求值——左操作数决定结果时跳过右操作数求值，返回操作数原值（三后端统一，非布尔）。`+` 支持字符串拼接（任一操作数为字符串即触发）
- **内置方法**：`push` / `pop` / `len` / `substr` / `replace` / `join` / `has` / `contains` / `get` / `set` 等。`len`/`contains` 跨类型复用（数组/字典/字符串共用）。`substr` 负索引/超长索引返回空串。`replace` 全局字面量替换（非正则）
- **字符串插值**：`"hello {name}, count={x+1}"` 支持表达式嵌入。插值支持嵌套字符串、字典、数组，嵌套深度限制 64 层防栈溢出。空插值 `{}` 报语法错误
- **异常处理**：`try { ... } catch (e) { ... } throw expr`。`throw` 可抛任意值。`catch` 参数独占 slot（防止覆盖外层变量）。`try` 缺失 `catch` 会导致 REPL 续行提示
- **模块系统**：`import {a, b} from "module";`、`import * as m from "module";`、`export var x = 1;`、`export fun f() {}`。import/export 限制在顶层作用域。模块路径非空校验，路径遍历防护（拒绝 ".." 父目录引用和绝对路径）。VM 路径通过编译期模块内联支持 import

### IDE 能力

- **代码编辑器**：语法高亮、行号、断点标记、错误下划线、当前执行行高亮、代码折叠（基于块结构）、查找替换（带迭代限制防 UI 阻塞）、Tab/Shift+Tab 多行缩进、回车自动缩进、括号匹配高亮、Ctrl+G 跳转行、Ctrl+/ 注释切换、Ctrl+D 选中相同词（单光标简化版）、Ctrl+Shift+K 删除当前行
- **词法/语法分析可视化**：Token 表格（10000 行显示上限）、AST 树形图（自动布局，可缩放/平移）
- **字节码反汇编**：主 chunk 与函数 chunk 分段显示，单步高亮当前指令，同时兼容栈式 VM（OpCode）和寄存器式 VM（RegOp）
- **IR 中间表示层**：可选 AST -> IR -> Bytecode 三段式编译，52 个 IROp 指令，支持 SSA-like 虚拟寄存器、多函数 lowering、闭包 upvalue 捕获、写回指令、全局槽位分配、优化 pass（常量折叠 / 死代码消除 / 复制传播）、IR 可视化面板与 IR 调试器集成。IR 路径退出后自动恢复配置，不污染后续编译路径
- **三执行引擎**：
  - 树遍历解释器（支持 REPL 续行输入、条件断点沙箱求值）
  - 栈式字节码 VM（支持单步、栈/全局变量监视、条件断点）
  - 寄存器式 VM（32 虚拟寄存器 R0-R31，50+ RegOp，IR 三地址码直接 lowering）
- **调试器**：断点（含条件断点，条件求值沙箱隔离程序状态）、单步进入/跳过/跳出、变量监视、调用栈（M5: 选中栈帧自动跳转到对应源码行）、首行断点 pre-execution 检查。双后端调试路径暂停语义文档化：Interpreter pre-execution 暂停，VM post-execution 检查（IP 指向下条指令，变量快照反映断点行 pre-execution 状态）
- **类型检查器**：编译期类型注解检查，三后端运行时统一强制，类型违反报告警告
- **代码格式化器**：可配置缩进/花括号风格/运算符空格，保留注释。幂等性 + 往返不变量验证（AST 结构比较），括号保留遵循运算符优先级（右嵌套同优先级加括号，左嵌套冗余括号丢弃）
- **REPL 面板**：交互式求值，支持多行续行（未闭合 `{ ( [` / 未闭合字符串 / 未闭合块注释 / `try` 缺失 `catch` 自动续行，正确处理字符串插值嵌套上下文），续行中按空行可中止；表达式语句自动求值并打印结果；异步执行不阻塞 UI；`help`/`clear` 特殊命令；`%magic` 命令系统（功能 11）：10 个 magic 命令（`%help` / `%version` / `%disassemble` / `%ir` / `%ast` / `%tokens` / `%memory` / `%profile` / `%compare` / `%reset`），在 REPL 输入 `%` 前缀即可快速调用各面板的数据获取逻辑，复用 IdeController Facade API
- **工作区 UX**：状态栏最右侧实时显示当前执行引擎（`⚙ 树遍历解释器`/`⚙ 栈式 VM`/`⚙ 寄存器式 VM`，随引擎切换同步）；状态栏选中范围显示（`选中 N 行 M 字符`，仅在有选中时显示）；文件树顶部搜索过滤框（按文件名小写包含递归过滤，目录按可见子项决定显隐）；文件树右键菜单扩展 3 项（复制路径 / 复制相对路径 / 在文件资源管理器中显示）；错误列表类型过滤栏（错误/警告/信息/提示 4 个 toggle 按钮，按 DiagLevel 显隐过滤，新增错误项自动遵循当前过滤状态）

### 教学增强面板（第一波 + 第三波）

围绕"三套执行引擎 + 共享 IR 层 + 历史真实 Bug 沉淀"三大独特性设计，强化 IDE 的教学价值：

- **编译管线可视化面板**（Ctrl+Shift+P）：5 步流程导航条（源码 → Token → AST → IR → 字节码），逐步展示编译管线每一阶段的中间产物。Token 表格含 type/lexeme/line/column 字段，支持右键菜单复制单元格/整行 + Ctrl+C 快捷复制；AST 步骤展示 dumpAst 摘要；IR 步骤展示 IRModule 反汇编；字节码步骤展示 BytecodeChunk 反汇编
- **三后端并行对比面板**（Ctrl+Shift+B）：同一源码顺序运行 Interpreter / StackVM / RegisterVM 三条路径，每路径独立计时（微秒精度）+ 输出比对，自动统计行级差异并给出一致性 PASS/FAIL 结论。三后端输出一致即"三后端语义等价"教学验证
- **Bug 狩猎面板**（Ctrl+Shift+H）：基于项目历史真实 Bug 训练调试能力。题库 15 道，按难度分三级——🟢 **入门级** 5 道阅读理解型（BUG-READ-01~05：预测输出 / 块作用域 / Formatter 展开 / 循环内 vs 循环外 var / COW refCount），🟡 **进阶级** 4 道（BUG-DEF-1/REGVM-2/REPL-1/F-04），🔴 **专家级** 6 道（BUG-IR-POP-2/CP-1/UV-1/DBG-1/CP-2/MOD-1）。每题含背景/源码/期望行为/Bug 行为/递进提示/根因分析。难度筛选栏 4 互斥按钮（全部 / 入门 / 进阶 / 专家），默认显示入门级。题目列表上方搜索框按文本小写包含过滤。三栏布局：题目列表 + 背景说明 + 内嵌编辑器/输出。可一键加载源码到主编辑器进一步调试
- **交互式语法探索器**（Ctrl+Shift+S）：10 条核心产生式参考（var-decl/if-stmt/while-stmt/for-stmt/fun-decl/class-decl/try-stmt/import-stmt/string-interp/data-structures/operators），每条配 EBNF 形式 + 文字说明 + 可运行样例代码。三栏布局：产生式列表 + 说明 + 代码/输出
- **内置实验手册**（Ctrl+Shift+L）：8 个实验章节（lab-01~lab-08：词法分析 / 递归下降 / 树遍历解释器 / 栈式 VM / 寄存器式 VM / 三后端一致性 / 内存模型 / Bug 狩猎），每章含目标/关键概念/实验步骤/验证断言/进阶/可一键加载样例代码，并配备 📋 学习清单（任务列表）/ 💡 小贴士（引用块）/ ⚠️ 常见错误（表格）/ 🤔 思考题四类增强区块。Markdown 内容通过 MarkdownRenderer 渲染（支持任务列表/引用块/表格/围栏代码块）

### 教学增强面板（第二波）

围绕"内存模型 + IR 变换 + 性能剖析"三大运行时内部细节进一步深化教学价值：

- **内存模型可视化面板**：3 个子页 — (1) NaN-boxing 编码：8 字节 Value 的 64 位分段图示（tag/payload），含 int48 范围/边界/超范围装箱演示，7 个示例（int-inline-pos/neg/boundary + float + bool + null + ptr）；(2) RefCounted 引用计数 & COW：4 个场景（数组基础生命周期 / COW 写时复制 detach / 字符串共享 / 实例字段引用），每场景含步骤表展示 refCount 变化；(3) GcManager mark-sweep：6 个阶段说明（注册/触发时机/Mark/Sweep/UAF 防护/已知限制）+ 实时 tracked 节点数
- **IR 变换过程面板**：3 个子页 — (1) AST → IR lowering：8 个典型 AST 节点的 lowering 演示（整数字面量 / 二元加法 / 变量声明 / if / while / 函数调用 / 闭包捕获 / 类方法），每个含 AST 摘要 + 源码 + lowering 后 IR 文本；(2) 优化 pass 对比：3 个 pass（常量折叠 / 死代码消除 / 复制传播）的前后 IR 对比 + 指令数变化；(3) 当前源码 IR：从当前 AST 实时生成 IRModule 反汇编（复用 IRToString 公共 API）
- **性能剖析仪表盘**：6 个性能场景（fib 递归 / 循环求和 / 字符串拼接 / 类实例化 / 闭包捕获 / 字典访问）+ 三后端多次测量取平均与标准差 + 柱状图（QPainter 自绘，避免 QtCharts 强依赖）+ 加速比分析。每场景含分类标签与迭代次数配置

> 第二波 3 个面板同样作为独立 ads::CDockWidget 注册到右侧 dock area，启动时默认隐藏。性能剖析面板顺序运行三后端（与 BackendComparePanel 一致），柱状图通过 paintEvent 自绘（蓝/橙/绿三色对应 Interpreter/StackVM/RegisterVM）。

### 教学增强面板（第三波）

围绕 IdeController Facade 已暴露的 VM/调试状态 API（`getVmCallStack` / `getVmGlobals` / `getDebugVariableSnapshot` / `getVmCurrentIP` / `getVmCurrentOpCodeName` / `getVmStack` 等），无需引擎层改造即可消费运行期状态，强化"运行时状态可见性"教学维度：

- **调用栈可视化面板**（CallStackPanel）：2 个子页 — (1) 实时调用栈：优先消费 `getVmCallStack()` 走 VM 路径，回退到 `getDebugCallStack()` 走 Interpreter debug 路径，显示栈帧列表（函数名 / 行号 / 深度 / 本地变量），500ms 自动刷新；(2) 教学场景库：6 个场景（simple-call 普通调用 / recursion 递归 / closure-capture 闭包捕获 / method-dispatch 方法分派 / try-catch 异常传播 / mutual-recursion 互递归），每场景含源码 + 期望栈帧序列 + 教学注释。教学价值：理解函数调用栈、闭包 upvalue、递归与异常传播
- **变量检查器面板**（VariableInspectorPanel）：2 个子页 — (1) 实时变量树：消费 `getVmGlobals()` + `getDebugVariableSnapshot()`，按作用域分组（全局变量组 + 局部变量按 scope 二次分组），每变量显示名 / 类型名（`Value::typeName()`）/ 值字符串 / NaN-boxing 位编码十六进制；(2) 类型教学库：9 种类型示例（int / int-boundary / float / bool / null / string / array / dict / instance / closure），每示例含值 + 位编码 + 文字说明。教学价值：理解作用域、类型注解、NaN-boxing 位编码
- **字节码执行轨迹面板**（BytecodeTracePanel）：2 个子页 — (1) 执行轨迹时间轴：消费 `getVmCurrentIP()` / `getVmCurrentOpCodeName()` / `getVmFrameCount()` / `getVmStack()`，记录单步执行序列（step / IP / OpCode / frameCount / 栈快照），最多保留 1000 条历史，500ms 轮询自动捕获；(2) OpCode 教学库：18 个 OpCode 文档条目（OP_INT / OP_FLOAT / OP_STRING / OP_NULL / OP_ADD / OP_GET_GLOBAL / OP_SET_GLOBAL / OP_JUMP / OP_JUMP_IF_FALSE / OP_LOOP / OP_CALL / OP_RETURN / OP_BUILD_ARRAY / OP_BUILD_DICT / OP_CLOSURE / OP_GET_UPVALUE / OP_CLASS_NEW / OP_METHOD_CALL），每条含分类 + 文字说明 + 栈效应 + 示例代码。教学价值：理解栈式 VM 的 push/pop 平衡、寄存器分配、IP 前进机制

> 第三波 3 个面板同样作为独立 ads::CDockWidget 注册到右侧 dock area，启动时默认隐藏。关键架构决策：BytecodeTracePanel 使用 QTimer 500ms 轮询而非订阅 IdeController 信号，避免测试目标链接 IdeController.cpp（测试目标仅链接 minilang_core + Panel.cpp 中的 Library 静态数据）；CallStackPanel/VariableInspectorPanel 优先走 VM 路径（`isVmInitialized()`），回退到 Interpreter debug 路径（`isRunning() && isDebugRun()`）。

### 教学增强面板（第二档：轻量引擎改造）

围绕 ProfileDashboardPanel 的真实 instrumentation 改造与条件断点可视化，强化"性能剖析真实性"与"调试条件断点"教学维度：

- **性能剖析仪表盘（指令计数增强版）**：在原有"时间对比"tab 之外，新增"指令计数"tab。直接 new 一个独立 VM/RegisterVM 实例（绕过 VmStepper 的 stepCallback 禁用逻辑），启用 VM/RegisterVM 已有的 `stepCallback_` 机制，每条指令执行后累加 `opProfileCounts_[static_cast<uint8_t>(op)]`。执行完毕后聚合 Top 10 热点 OpCode，并在右侧 QTableWidget 显示。同时附带 12 条 OpCode 性能文档（含分类 / 栈效应 / perfNote 性能提示），帮助学习者识别热路径并理解 OpCode 代价差异
- **条件断点可视化面板**（BreakpointConditionPanel）：2 个子页 — (1) 实时断点列表：消费 `IdeController::getBreakpoints()` / `getBreakpointCondition(line)` / `getBreakpointHitCount(line)`（新增 Facade getter，转发到 DebugCoordinator），500ms 轮询刷新，显示当前所有断点（行号 / 条件表达式 / 命中次数）；(2) 教学场景库：8 个场景（simple-line 普通行断点 / conditional-loop-count 循环计数条件 / conditional-state 状态条件 / data-driven 数据驱动 / error-catch 异常捕获 / breakpoint-with-closure 闭包断点 / watchpoint 变量监视 / temporary-breakpoint 临时断点），每场景含源码 + 期望断点行 + 教学注释。教学价值：理解条件断点表达式语义、命中次数累计、断点生命周期

> 第二档 2 个面板同样作为独立 ads::CDockWidget 注册到右侧 dock area，启动时默认隐藏。关键架构决策：ProfileDashboardPanel 直接 new VM/RegisterVM 实例进行指令计数（绕过 VmStepper），避免修改 VmStepper 的 stepCallback 禁用逻辑；BreakpointConditionPanel 使用 QTimer 500ms 轮询而非订阅 IdeController 信号，避免测试目标链接 IdeController.cpp（测试目标仅链接 minilang_core + Panel.cpp 中的 Library 静态数据）。IdeController/DebugCoordinator 新增 3 个只读 getter（getBreakpoints / getBreakpointCondition / getBreakpointHitCount），均为 const noexcept 转发，不影响现有语义。

### 教学增强面板（第四档：已有面板动画增强）

围绕 IRTransformPanel 与 PipelineViewer 的子页切换动画，强化"过渡平滑性"教学体验：

- **IR 变换过程面板（动画化）**：在原有 3 个子页（AST → IR lowering / 优化 pass 对比 / 当前源码 IR）切换时，通过 `PanelAnimator::fadeInWidget` 驱动 QGraphicsOpacityEffect，使新子页 opacity 0→1 平滑淡入（220ms OutCubic 缓动）。同时 lowering 详情刷新、优化前后 IR 刷新均加入淡入动画，避免生硬跳变
- **编译管线可视化面板（动画化）**：在原有 step 切换（Lexer / Parser / AST / IR / Bytecode / 运行）时，通过 `PanelAnimator::fadeInWidget` 驱动子页淡入，使管线步骤切换更流畅

> 第四档 2 个面板仅扩展 `gui/PanelAnimator.h` 新增 `fadeInWidget` 工具函数（QGraphicsOpacityEffect 驱动 opacity 0→1，220ms OutCubic），并在 IRTransformPanel.cpp / PipelineViewer.cpp 的子页切换 lambda 末尾调用该函数。无引擎层改造，无新增源文件，无新增测试目标。

### 教学增强面板（第三档：异常流 + 闭包 upvalue 可视化）

围绕异常传播路径与闭包 upvalue 生命周期两个"动态语义"教学难点，强化"控制流跨函数跳跃"与"堆化变量捕获"教学维度：

- **异常流可视化面板**（ExceptionFlowPanel）：2 个子页 — (1) 教学场景库：8 个异常场景（simple-try-catch 简单 try/catch / uncaught-exception 未捕获异常终止 / nested-try-catch 嵌套 try/catch / finally-semantics finally 块语义 / cross-function-propagation 跨函数传播 / recursive-exception 递归异常 / exception-object-field 异常对象字段访问 / rethrow 重抛），每场景含 sampleCode + propagationPath（传播路径列表，例：`main() → try-block → throw → catch-block → main()`）+ teachingNote 教学注释；(2) 传播阶段图解：6 个阶段（throw 抛出 / search 查找 catch / catch 捕获 / finally 清理 / unwind 栈展开 / recovery 恢复），每阶段含 category + description + stackEffect（栈/堆效应）。教学价值：理解异常控制流跨函数跳跃、栈展开顺序、finally 与 catch 的执行先后
- **闭包检查器面板**（ClosureInspectorPanel）：2 个子页 — (1) 教学场景库：8 个闭包场景（simple-capture 简单捕获 / counter-pattern 计数器模式 / multi-capture 多变量捕获 / nested-closure 嵌套闭包 / closure-as-return 闭包作为返回值 / closure-array 闭包数组 / iife 立即调用 / closure-escape 闭包逃逸），每场景含 sampleCode + capturedVars 捕获变量列表 + captureType 捕获类型（by-reference upvalue / by-value / heap-escaped）+ teachingNote 教学注释；(2) upvalue 生命周期图解：6 个阶段（create 创建 / capture 捕获 / heap 堆化 / access 访问 / close 关闭 / destroy 销毁），每阶段含 category + description + stackEffect（栈/堆效应）。教学价值：理解 OP_CLOSURE 指令、upvalue 堆化时机、闭包逃逸与堆栈过渡

> 第三档 2 个面板均为纯静态教学面板（Library 静态数据嵌入 .cpp），不依赖 IdeController，不需要引擎层改造，不订阅信号——与第三波 BytecodeTracePanel 的"避免测试目标链接 IdeController.cpp"设计模式一致，确保测试目标仅链接 minilang_core + Panel.cpp 静态数据。子页切换与详情刷新复用第四档的 `PanelAnimator::fadeInWidget` 工具函数。

### 教学增强面板（第五档：高价值改进方向 Top 3）

围绕 IR 优化过程、内存模型动态、Bug 修复交互三个高价值教学维度，对已有三面板各新增一子页：

- **IR 变换过程面板（第四子页：IR 优化逐步回放 + 决策解释）**：5 个回放场景（replay-const-fold 常量折叠 / replay-dce 死代码消除 / replay-copy-prop 复制传播 / replay-cse 公共子表达式消除 / replay-loop-unroll 循环展开），每场景含 3 步 `IROptStepRecord`，每步记录 passName / passRound / irSnapshot / instrCount / modifiedCount + decisions（指令级别修改原因列表，例如"折叠 `1 + 2 → 3`"、"删除不可达 OP_RETURN"、"vreg1 → vreg2 等价替换"）。教学价值：理解 IR 优化的逐 pass 演进、修改/删除决策依据、收敛条件
- **内存模型面板（第四子页：实时动画 + VM 单步联动）**：4 个 GC 阶段动画（mark 标记可达 / sweep 回收孤岛 / reset 复位标志 / idle 空闲，4 色 #3079C0/#C03030/#709030/#909090 区分）+ 6 种堆对象类型（ArrayData / DictData / InstanceData / StringData / ClosureData / BoundMethodData）+ VM 单步联动（500ms QTimer 轮询 `IdeController::isVmInitialized` / `getVmStack` / `getVmGlobals`，实时展示堆对象图、refCount 变化曲线、GC mark-sweep 阶段切换）。教学价值：理解 GC mark-sweep 算法、循环引用回收、refCount 与可达性的关系
- **Bug 狩猎面板（交互式修复模式 + 三后端对比 + 变体挑战）**：新增"三后端对比"按钮（同时运行 Interpreter / StackVM / RegisterVM 三条路径，对比输出/异常/耗时）+ 7 个变体挑战题（基于 7 个父题 CP-1/UV-1/IR-POP-2/DEF-1/MOD-1/DBG-1/REGVM-2 各生成一个变体，含 sourceCode / challengeGoal / hint）+ 变体模式切换（变体列表默认隐藏，点击"变体模式"按钮切换显示）。教学价值：理解三后端语义一致性、变体题目设计思路、Bug 修复验证流程

> 第五档 3 个面板新增子页均复用已有 Panel.cpp 的 Library 静态数据模式。关键架构决策：BugHuntVariantLibrary::variants() 拆分到独立 `gui/BugHuntVariantLibrary.cpp` 编译单元（与 BugHuntLibrary.cpp 模式一致），避免测试目标链接 IdeController.h 依赖。MemoryModelPanel 第四子页新增 `Value::isPointer()` / `asPointer()` 公共访问器（line 452-463），便于面板读取堆对象指针元数据。IRTransformPanel 第四子页静态 5 场景 × 3 步共 15 个 IROptStepRecord，irSnapshot 字段含 `function` 关键字验证 IR 文本格式正确性。

### 教学增强面板（功能 6：学习路径地图）

作为中央导航枢纽，将所有教学面板与实验组织为 5 阶段渐进式学习路径，配合进度持久化与智能推荐：

- **学习路径地图面板**（LearningPathPanel）：顶部总进度条 + 垂直滚动 5 阶段卡片列表（阶段零 首次接触 / 阶段一 编译前端 / 阶段二 执行引擎 / 阶段三 深入理解 / 阶段四 实战训练），每阶段含阶段标题 + 阶段进度条 + 活动项列表。共 21 个学习活动（welcome 导览 / code-journey 动画 / token-puzzle 拼图 / ast-toy 玩具 / vm-sandbox 沙盒 / lab-01~08 八个实验 / syntax-explorer / op-priority-challenge / backend-compare / ir-transform / profile-dashboard / bug-hunt-beginner/intermediate/expert 三档 Bug 狩猎 / freeform-project 自由项目）
- **解锁机制**：基于 `prerequisites` 字段，前置活动全部完成后才能解锁后续活动。阶段零所有活动无前置（首次启动即可解锁），lab-N 通常前置为 lab-(N-1)（递进学习），bug-hunt 三档必须按顺序通关
- **推荐算法**：当前推荐 = 未完成 + 已解锁 + stage 最小，同 stage 内优先 attemptCount 最少 → estimatedMinutes 最少 → id 字典序
- **进度持久化**：JSON 文件存储（QStandardPaths::AppDataLocation + minilang_progress.json），记录 completed / attemptCount / lastAccessTime / currentStage。加载失败/格式不匹配时回退到空进度，不崩溃
- **信号路由**：点击活动项发射 `activityRequested(activityId)` 信号，主窗口连接后路由到对应面板
- **键盘导航**（M9）：面板设 StrongFocus 后，Up/Down 在已解锁活动行间循环高亮（蓝色边框 + 自动滚动可见），Enter 触发当前行点击，无需鼠标操作即可浏览学习路径

> 关键架构决策：LearnerProgress.cpp 与 LearningPathData.cpp 拆分为独立编译单元（仅依赖 Qt6::Core，不依赖 IdeController / Qt6::Widgets），测试目标可安全链接。LearningPathPanel.cpp 依赖 Qt Widgets + PanelAnimator + QScrollArea，不加入测试目标。活动行使用 QPushButton flat 模式实现可点击（避免 installEventFilter）。

### 教学增强面板（功能 4：AST 节点搭建玩具）

通过点击/组装 AST 节点让学习者理解「AST 是代码的结构化表示」，6 道题由浅入深：

- **AST 搭建玩具面板**（AstBuilderToyPanel）：顶部 QComboBox 题目选择 + 进度 QLabel；左侧 7 个工具箱按钮（Number / Add / Sub / Mul / Div / Print / VarDecl）；右侧 QTreeWidget 显示已搭建树；底部 5 个操作按钮（检查 / 查看标准答案 / 下一题 / 删除选中节点 / 清空）+ 反馈 QLabel + 题目说明 QLabel
- **6 道题设计**：(1) `42` — 最简叶子节点；(2) `1 + 2` — 二元运算根+两叶；(3) `1 + 2 * 3` — 优先级：* 在 + 下面；(4) `(1 + 2) * 3` — 括号改变结构：+ 在 * 下面；(5) `print(x)` — 函数调用；(6) `var x = 1 + 2;` — 完整语句 VarDecl+initializer
- **核心教学时刻**：题目 3 vs 题目 4——同样的数字 1/2/3 与运算符 +/*，仅括号不同就导致完全不同的树结构。这个"顿悟瞬间"是整个玩具的核心价值
- **交互逻辑**：点击工具箱节点添加到 QTreeWidget（树空时为根，否则为选中项子节点）；Number/VarDecl 弹 QInputDialog 询问值/名称；Delete 键或按钮删除选中节点；检查答案递归比对玩家树与标准答案的拓扑结构（label 严格相等 + children 顺序）；查看标准答案将 answer 树渲染到画布
- **信号**：`activityCompleted(levelId)` 在检查通过时发射，levelId 格式 `ast-toy-level-N`，供 LearningPathPanel 标记活动完成

> 关键架构决策：AstToyLevels.cpp 拆分为独立编译单元（仅依赖 Qt6::Core / 标准库，不依赖 Qt6::Widgets / IdeController），测试目标可安全链接。AstBuilderToyPanel.cpp 依赖 Qt6::Widgets（QTreeWidget / QComboBox / QInputDialog），不加入测试目标。使用 QTreeWidget 而非 QGraphicsScene，实现简单、测试容易。所有用户可见文本用 mlTr() 包裹（i18n）。

### 教学增强面板（功能 5：VM 栈沙盒）

通过亲手操作理解"栈式 VM 就是 push 和 pop"，5 关卡由浅入深：

- **VM 栈沙盒面板**（VmStackSandboxPanel）：顶部关卡选择 QComboBox + 目标/教学点 QLabel；中部三栏——左侧可用指令按钮区（点击执行该指令）/ 中间操作数栈可视化 QListWidget（栈顶在顶部，深色背景）/ 右侧已执行指令序列 QListWidget；底部输出区 QTextEdit + [↩ 撤销] [🔄 重置] [✓ 检查] 按钮 + 反馈 QLabel
- **5 关卡设计**：(1) 计算 `1 + 2` → 输出 "3"，最基本的 push-pop；(2) 计算 `1 + 2 * 3` → 输出 "7"，运算顺序由指令决定（先 MUL 后 ADD）；(3) 计算 `(1 + 2) * 3` → 输出 "9"，与关卡 2 相同可用指令但不同执行顺序（先 ADD 后 MUL）；(4) 打印 `"hello"` → 输出 "hello"，字符串也是值；(5) 自由模式，全部指令可用（PUSH_INT/PUSH_STRING/ADD/SUB/MUL/DIV/MOD/NEG/PRINT/HALT）
- **核心教学时刻**：关卡 2 vs 关卡 3——同样的可用指令集（1/2/3 + ADD + MUL + PRINT），不同的执行顺序得到不同结果（7 vs 9）。这正体现了栈式 VM 的"指令即语义"
- **栈状态机模拟**（不连接真实 VM）：`std::vector<std::string>` 模拟操作数栈，PUSH 压栈 / 二元运算 pop 两个 int 运算后压回 / PRINT pop 栈顶加到输出。栈不足 2 / 除数为 0 / 非整数操作数均有明确错误反馈。撤销通过快照实现（每步前保存栈与输出状态）
- **信号**：`activityCompleted(levelId)` 在检查通过时发射，levelId 格式 `level-N`，供 LearningPathPanel 标记活动完成

> 关键架构决策：SandboxLevels.cpp 拆分为独立编译单元（仅依赖 Qt6::Core / 标准库，不依赖 Qt6::Widgets / IdeController），测试目标可安全链接。VmStackSandboxPanel.cpp 依赖 Qt6::Widgets（QComboBox / QListWidget / QTextEdit），不加入测试目标。栈状态机用 std::vector<std::string> 而非真实 Value，纯前端游戏不依赖引擎层，整数除法截断向零与 MiniLang 三后端一致。所有用户可见文本用 mlTr() 包裹（i18n）。

### 教学增强面板（功能 3：交互式 Token 拼图游戏）

通过游戏化方式让学习者理解「词法分析就是切分字符流」，5 关卡由浅入深：

- **Token 拼图面板**（TokenPuzzlePanel）：顶部 QComboBox 关卡选择 + 得分 QLabel（⭐ 总分 / 满分 15）+ 关卡进度 QLabel；中部上为目标语句展示（只读，Consolas 等宽字体 + 灰底卡片）+ 教学点说明（斜体灰字）；中部中为打乱的 token 按钮（QGridLayout 排列，点击添加到答案区并禁用原按钮）；中部下为玩家答案区（QListWidget 横向排列，点击移除并恢复对应按钮）；底部 4 个操作按钮（✓ 检查答案 / 💡 提示 / ⏭ 跳过 / 🔄 重置）+ 反馈 QLabel
- **5 关卡设计**（由浅入深）：(1) `var x = 42;` — 基本 5 个 token（⭐）；(2) `print("hello");` — 字符串是一个 token 而非 7 个（⭐）；(3) `x > 0 and y < 10;` — 运算符优先级不影响 token 切分（⭐⭐）；(4) `// 这是注释\nprint(1);` — 注释被分离出主流（⭐⭐）；(5) `a[0] = b["key"] + 1;` — 复杂表达式 12 个 token（⭐⭐⭐）
- **星级评分**：0 次提示 = ⭐⭐⭐，1 次提示 = ⭐⭐☆，2+ 次提示 = ⭐☆☆，跳过 = ☆☆☆。检查答案错误时高亮错误位置的 token（红底），反馈"第 N 个 token 不对：你填了「X」，应该是「Y」"
- **解锁机制**：初始仅第 1 关解锁（QStandardItemModel 逐项 enable/disable），完成或跳过当前关后自动解锁下一关
- **信号**：`activityCompleted(levelId)` 在检查通过或跳过时发射，levelId 格式 `token-puzzle-N`，供 LearningPathPanel 标记活动完成

> 关键架构决策：TokenPuzzleData.cpp 拆分为独立编译单元（仅依赖标准库 std::string / std::vector，不依赖 Qt Widgets / IdeController），测试目标可安全链接。TokenPuzzlePanel.cpp 依赖 Qt6::Widgets（QComboBox / QListWidget / QPushButton / QStandardItemModel），不加入测试目标。纯前端游戏，不调用 Lexer/Parser/Interpreter。shuffledTokens 为 tokens 的固定乱序（硬编码非运行时随机），保证可重现。所有用户可见文本用 mlTr() 包裹（i18n）。

### 教学增强面板（功能 1：Welcome 向导 / 首次启动导览）

让从没听过"编译原理"的人在 3 分钟内感受到"原来我写的代码是这样变成程序的"，首次启动模态弹出独立向导对话框：

- **Welcome 向导**（WelcomeWizard，继承 QDialog）：QStackedWidget 3 步交互式导览 + 顶部"步骤 N / 3"进度指示 + 底部 [← 上一步] [下一步 →]/[完成 ✓] 导航
- **Step 1：欢迎页 + 角色选择** — 标题"👋 欢迎使用 MiniLang IDE！"+ 副标题；3 个角色 QRadioButton（QButtonGroup 互斥）："我完全新手" / "我懂一点编程" → 走完整 3 步导览；"我学过编译原理" → 跳过导览直接关闭；按钮 [▶ 开始探索] [跳过]
- **Step 2：Token 概念（词法分析）** — 左侧只读 QTextEdit 预填 `print("Hello!");`（Consolas 等宽字体 + 深色主题）；右侧 QTableWidget 3 行 Token（print / "Hello!" / ;，含类型列 IDENTIFIER/STRING/SEMICOLON 与说明列）；点击 Token 表行通过 `QTextEdit::setExtraSelections` 高亮编辑器中对应字符区间（蓝底白字）；文字说明"① 你写的代码被切成了 3 个 Token —— 这就是「词法分析」"
- **Step 3：字节码与运行** — 左侧 QTreeWidget 简化 AST 树（Print → StringLiteral "Hello!"）；右侧 QListWidget 字节码（OP_STRING "Hello!" / OP_PRINT）；底部 [▶ 运行] 按钮 + 输出区 QTextEdit（深色终端风格）；点击 [▶ 运行] 通过 QTimer 延迟模拟执行（600ms × 3 步：高亮 OP_STRING → 高亮 OP_PRINT → 输出 "Hello!"），可重复点击"再次运行"；文字说明"②③④ 代码 → AST → 字节码 → 执行，得到结果！"
- **进度持久化**：QSettings `welcome_completed` (bool) 标记，首次启动（false 或不存在）显示，完成/跳过后置 true，下次启动不再弹出
- **"再次显示欢迎向导"菜单项**：帮助菜单新增入口，点击后重置标记并重新弹出向导
- **QFluentKit 主题化**：按钮使用 QFluentKit 组件（主操作 [▶ 开始探索] / [开始学习 ✓] → `PrimaryPushButton` 主题色自动驱动；次操作 [跳过] / [← 上一步] / [下一步 →] → `PushButton`；[▶ 运行] 保留 QPushButton 但用 `TeachingTheme::success()` 绿色着色）；标题/副标题用 `TitleLabel` / `CaptionLabel`；所有硬编码颜色（#0078d4 / #5a5a5a / #666 / #8a8a8a / #3060c0 / #107d58 等）替换为 `gui/TeachingTheme.h` 主题色板（primary / textSecondary / textHint / surface / warning / success），亮/暗主题自适应。代码编辑器深色主题（#1e1e1e）、运行输出终端色、Step 4 五阶段语义色卡片保留不变

> 关键架构决策：WelcomeWizard 作为独立 QDialog 模态弹出，在 `Ide::Ide()` 构造函数末尾（所有 UI 初始化完成后）显示，避免改动现有 `welcomePage_` 结构（最近文件/新建/打开/拖放功能保持不变）。向导内"运行"是 QTimer 纯前端模拟，不调用 IdeController / 真实引擎层。WelcomeWizard.cpp 依赖 Qt6::Widgets（QDialog / QStackedWidget / QTableWidget / QTreeWidget）+ QFluentKit（PushButton / Label），仅在 minilang_ide 主程序链接，不加入测试目标（无单元测试要求）。所有用户可见文本用 mlTr() 包裹（i18n）。

### 教学增强面板（功能 2 降级：代码生命旅程静态信息图）

原方案是 30 秒动画展示代码从源码到输出的全过程，但维护成本高（与语言特性紧耦合，每次语法变更都需更新）。降级为静态信息图：用一张 HTML 风格的图展示编译管线，配合"逐步查看"按钮可跳转到对应面板。成本低且不易过时。

- **代码旅程信息面板**（CodeJourneyInfoPanel）：顶部标题"🚀 代码的生命旅程"；主体 QTextBrowser 渲染静态 HTML 信息图，展示 `print(1 + 2 * 3);` 这行代码的完整生命旅程：① 源码 → ② Token 表（9 个 token）→ ③ AST（树形结构，强调 * 在 + 的右子树）→ ④ IR（三地址码 v1=1, v2=2, v3=3, v4=v2*v3=6, v5=v1+v4=7）→ ⑤ 字节码（OP_INT/OP_MUL/OP_ADD/OP_PRINT）→ ⑥ 输出（7）；底部 6 个跳转按钮（① 编辑器 / ② Token 表 / ③ AST / ④ IR / ⑤ 字节码 / ⑥ 输出）
- **跳转信号**：`jumpToPanelRequested(panelId)` 在点击跳转按钮时发射，panelId 取值 "editor"/"tokens"/"ast"/"ir"/"bytecode"/"output"，供 Ide 主窗口分发到对应面板
- **关键教学点**：① 词法分析（切分字符流）→ ② 语法分析（按优先级构建树）→ ③ IR 生成（三地址码便于优化）→ ④ 后端 lowering（IR → 字节码）→ ⑤ VM 执行（push/pop 或寄存器运算）；强调"括号改变 AST 结构"（`(1+2)*3` 与 `1+2*3` 的 AST 不同）和"三后端一致性"（Interpreter / StackVM / RegisterVM 三条路径都输出 7）

> 关键架构决策：CodeJourneyInfoPanel.cpp 依赖 Qt6::Widgets（QTextBrowser / QPushButton / QLabel），不依赖 IdeController / 引擎层，仅在 minilang_ide 主程序链接，不加入测试目标（纯静态信息图无单元测试要求）。原方案的 30 秒动画与语言特性紧耦合（每次语法/字节码/IR 变更都需同步更新动画脚本），维护成本过高；静态信息图 + 跳转按钮的方案让学习者直接跳转到对应面板亲手探索，反而比被动观看动画更有效。所有用户可见文本用 mlTr() 包裹（i18n）。

### 第四档教学面板 UI 接线（功能 1-6 IDE 集成）

将 5 个教学面板（CodeJourneyInfoPanel / AstBuilderToyPanel / TokenPuzzlePanel / VmStackSandboxPanel / LearningPathPanel）正式接入 IDE 主窗口的 dock 系统、视图菜单与跨面板信号路由网络。用户现在可以从「视图」菜单打开这些面板，并通过 LearningPathPanel 的活动项点击跨面板跳转。

- **8 触点完整注册**（每个面板）：ide.h include + ide.h 成员变量 + view QAction 创建 + dock 创建/添加到 dockManager + 信号连接 + 启动隐藏 + connectDockSave 防抖 + syncViewMenuChecks 勾选同步
- **视图菜单新增 5 项**：学习路径地图 / Token 拼图游戏 / AST 搭建玩具 / VM 栈沙盒 / 代码生命旅程
- **跨面板信号路由**：
  - `CodeJourneyInfoPanel::jumpToPanelRequested(panelId)` → `Ide::onJumpToPanel`：6 目标路由（editor→主编辑器 / tokens→rightPivot token tab / ast→AST 独立窗口 / ir→rightPivot ir tab / bytecode→rightPivot bytecode tab / output→bottomPivot output tab）
  - `LearningPathPanel::activityRequested(activityId)` → `Ide::onActivityRequested`：21 活动 ID 路由（welcome→WelcomeWizard / journey→codeJourney / token-puzzle / ast-toy / vm-sandbox / lab-XX→labManual / syntax-explorer / op-priority-challenge→ast-toy / backend-compare / ir-transform / profile-dashboard / bug-hunt-XX→bugHunt / freeform-project→editor）
  - 3 游戏 `activityCompleted` → `LearningPathPanel::markActivityCompleted`：含 ID 映射（"token-puzzle-N"→"token-puzzle" / "ast-toy-level-N"→"ast-toy" / "level-N"→"vm-sandbox"）

> 关键架构决策：5 个面板均为纯静态面板（无 setController 调用），与 ExceptionFlowPanel/ClosureInspectorPanel 模式一致——不依赖 IdeController / 引擎层，仅作为静态教学内容的展示与交互。ID 映射逻辑放在 Ide slot 中而非面板内部，保持面板的独立性与可测试性。showDock lambda 复用（`auto showDock = [this](ads::CDockWidget* dock) {...}`）让 12 个活动路由分支共用同一个显隐逻辑。LearningPathPanel 显示时自动 refresh（view action toggled=true 时调用 `learningPathPanel_->refresh()`），确保进度从持久化存储重新加载。

### 教学导航增强（学习中心 + 标题栏 + 主题色板 + 首次展开）

针对「视图菜单 24 项平铺」「教学面板无说明」「窗口样式不统一」三大新手体验问题，实施完整的教学导航增强方案：

- **学习中心对话框**（LearningHubDialog）：ActivityBar 新增「学习」项（EDUCATION 图标），点击弹出 4 分组卡片导航对话框——入门导览(3) / 编译前端(4) / 执行引擎(8) / 深入实战(5)。点击卡片项发 panelRequested 信号并关闭对话框。视图菜单从 24 项平铺重构为 4 子菜单 + 学习中心入口（Ctrl+Shift+L）。使用 QFluentKit TitleLabel/CaptionLabel/SimpleCardWidget/PrimaryPushButton
- **教学面板统一标题栏**（TeachingPanelHeader）：19 个教学面板顶部统一包装为 `[标题] [这是什么？] [学习路径]` 三段式。「这是什么？」弹出 480x360 帮助对话框，含面板用途/推荐使用顺序/关联概念三段；「学习路径」按钮发 learningPathRequested 信号由 Ide slot 路由到 LearningPathPanel。标题栏含浅蓝→白色渐变背景 + 底部分隔线，帮助按钮 hover 态圆角淡蓝高亮；面板容器卡片化（4px 圆角 + 1px 边框 + 白底，与 #f3f3f3 面板背景形成层次对比）
- **主题色板工具**（TeachingTheme.h）：集中教学面板色板（primary/primaryHover/primaryPressed/textPrimary/textSecondary/textHint/surface/surfaceHover/border/accent/success/warning/error），全部跟随 QFluentKit Theme::isDark() + Theme::themeColor() 自适应亮/暗主题。提供 primaryButtonStyle()/secondaryButtonStyle() 便捷样式表
- **WelcomeWizard Step 4 学习路径推荐**：新增第 4 步，5 阶段彩色卡片（绿/黄/蓝/紫/红）+「开始学习 ✓」按钮，点击发 learningPathRequested 信号并 accept。3 处 WelcomeWizard 创建点（首次启动 / helpMenu / onActivityRequested "welcome"）均连接信号
- **WelcomeWizard Fluent 化**：主操作按钮 → PrimaryPushButton（主题色自动驱动）；次操作按钮 → PushButton；标题/副标题 → TitleLabel/CaptionLabel；硬编码颜色 → TeachingTheme 主题色板。代码编辑器深色主题与 Step 4 五阶段语义色保留
- **首次用户 LearningPathPanel 默认展开**：首次用户（welcome_completed=false）完成向导后，无论跳过还是完成，都自动展开 LearningPathPanel 作为学习起点；老用户保持上次布局
- **3 个第三波面板子页淡入动画**：CallStackPanel / VariableInspectorPanel / BytecodeTracePanel 子页切换新增 PanelAnimator::fadeInWidget 调用，与第二档已动画化的 8 个面板风格统一

> 关键架构决策：TeachingTheme 用 inline 函数而非静态常量，保证每次调用动态读取 Theme::isDark()/themeColor()，主题切换时无需手动刷新。首次展开放在 wizard exec 之后（用户已了解概念再展开面板，避免布局抢占注意力）。3 处 WelcomeWizard 创建点信号连接一致，保证任何入口触发向导都能正确展开 LearningPathPanel。LearningHubDialog/TeachingPanelHeader 依赖 QFluentKit（PushButton/CardWidget/Label），不依赖 IdeController / 引擎层，仅在 minilang_ide 主程序链接，不加入测试目标。

### 教学面板 QFluentKit 控件统一迁移

将 19 个教学面板的原生 QWidget 控件批量迁移到 QFluentKit 控件，统一视觉风格与滚动条样式，遵循「最小侵入」原则：

- **主操作按钮 → PrimaryPushButton**：8 类面板的主操作按钮（运行/检查/刷新/加载）替换为 QFluentKit `PrimaryPushButton`（主题色填充），覆盖 BugHunt/LearningPath/IRTransform/TokenPuzzle/AstBuilderToy/VmStackSandbox/MemoryModel/BackendCompare/ProfileDashboard/LabManual/SyntaxExplorer。`PrimaryPushButton` 继承自 `QPushButton`（经 `PushButton` 中间层），原 `&QPushButton::clicked` 信号连接与 `findChildren<QPushButton*>()` 兼容性保持不变
- **标题/状态 QLabel → Fluent Label**：19 个面板的标题/状态 QLabel 按语义替换为 `TitleLabel`（顶部大标题，仅 CodeJourneyInfoPanel）/ `CaptionLabel`（状态/副标题，10 个面板的 statusLabel_）/ `StrongBodyLabel`（强调数据标签，6 个面板的 scoreLabel_/progressLabel_ 等），区分主次信息层级
- **滚动条统一 Fluent 化**：`wrapTeachingPanel()` 包装器新增 `applyFluentScrollBars` lambda，遍历所有 `QAbstractScrollArea*` 子类（QListWidget/QTableWidget/QTreeWidget/QTextBrowser/QScrollArea），替换原生 `QScrollBar` 为 QFluentKit `ScrollBar`，通过 `metaObject()->className()` 检查避免重复替换
- **硬编码颜色热点清理**：IrViewer 的 `#f8f8f8`、CodeJourneyInfoPanel 的 `#f5f5f5`、VmStackSandboxPanel 的标签背景，以及 BreakpointConditionPanel/IRTransformPanel 中 `<pre>` 代码块背景，全部替换为 `TeachingTheme::surface()` 等主题色板，跟随亮/暗主题自适应
- **关键约束遵循**：不动 CodeEditor.cpp 与主题切换逻辑；保留全部信号连接、QTimer、QStackedWidget 子页切换；工具箱/导航按钮保留原生 QPushButton 避免视觉过载

> 关键架构决策：PrimaryPushButton 仅用于主操作按钮以提供视觉强调，工具箱/导航按钮保留原生 QPushButton；滚动条替换用 className 检查防御性防重复；`<pre>` 背景用 TeachingTheme::surface().name() 跟随主题，无需在 HTML 中重复定义两套配色。全量 1668/1668 测试通过。

### 主题系统完整性增强（DebugPanel 主题化 + 调用栈帧跳转源码 + 教学面板 HTML 重建）

针对主题系统三处不完整实现，独立但同主题的 3 项 P1 高优先级任务在同一批次内完成，保证主题切换语义一致性：

- **M5 调用栈帧跳转源码**（gui/DebugPanel + app/ide.cpp）：DebugPanel 新增 `gotoLineRequested(int line)` 信号，`onStackFrameSelected` 末尾根据 `currentStack_[index].line` 发射信号（直接复用已验证的 CallStackEntry.line 字段，无需 UserRole 冗余存储），app/ide.cpp 连接到 `codeEditor_->gotoLine(line)`。调试时点击调用栈帧，主编辑器自动跳转到对应源码行，与 VS Code/CLion 调试器行为一致
- **DebugPanel 主题化接入**（gui/DebugPanel）：7 处硬编码颜色（`#616161` / `#ffffff` / `#e5e5e5` / `#cfe4f5`）全部替换为 TeachingTheme 主题色板（textSecondary / surface / border / primary().lighter(160)）。新增 `applyThemeStyles()` 私有方法集中管理所有 setStyleSheet 调用，构造函数末尾添加 `Theme::onThemeModeChanged` 监听，主题切换时重新应用样式
- **主题切换后教学面板 HTML 重建**（3 面板）：CodeJourneyInfoPanel / IRTransformPanel / BreakpointConditionPanel 构造函数末尾添加 `Theme::onThemeModeChanged` 监听，主题切换时重建/刷新 HTML 内容（buildJourneyHtml / populateLoweringDetail / populateScenarioDetail 内部使用 `TeachingTheme::surface()` 作为 `<pre>` 背景）

> 关键架构决策：直接复用 CallStackEntry.line 而非 UserRole 冗余存储行号（避免数据双源不一致）；applyThemeStyles 集中管理样式避免构造函数与主题切换回调代码重复；styleScopeGroupHeader 在每次 populateVariableTree 时重新设置画刷，主题切换后下次刷新自动跟随新主题无需手动处理；IRTransformPanel 按当前页签 `stack_->currentIndex()` 分发到对应 populate 函数，确保所有子页内容都刷新。全量 1668/1668 测试通过。

### P2 视觉一致性收尾（8 项独立任务批量处理）

针对主题系统在 8 处遗留的视觉一致性问题进行批量收尾，所有改动严格遵循「不动 CodeEditor.cpp / DebugPanel.cpp / 3 教学面板 HTML 重建」约束，全量 1668/1668 测试通过：

- **T1 5 阶段色集中管理**（gui/LearningPathPanel.cpp + gui/WelcomeWizard.cpp）：原本 LearningPathPanel::stageColor() 内嵌 switch 硬编码、WelcomeWizard Step 4 内嵌 QStringList 5 色数组，两处定义可能漂移。TeachingTheme.h 新增 `learningStageColor(int stage)` inline 函数（绿 #4CAF50 / 黄 #FFC107 / 蓝 #2196F3 / 紫 #9C27B0 / 红 #F44336），两处调用方均改为 `TeachingTheme::learningStageColor(stage).name()`，三处面板阶段色（含 CodeJourneyInfoPanel）自此统一走单一数据源
- **T2 输出面板语义色**（app/ide.cpp）：错误列表 4 个 errFilter 按钮（errFilterErrorBtn_ / errFilterWarnBtn_ / errFilterInfoBtn_ / errFilterHintBtn_）原本硬编码 QColor(...)，改为 `TeachingTheme::error()` / `warning()` / `info()`（新增 `#0078D4`）/ `hint()`（新增 `#8C8C8C`）语义色。`kTs/kInfo/kSuccess/kWarn/kError/kBody` 与诊断图标色受 RichTextItemDelegate HTML 拼接的 `const char*` 限制保留，加注释标注语义关系
- **T3 applyFluentStyle 14 色变量集中管理**（app/ide.cpp）：原本 applyFluentStyle 内 14 处 `isDark ? "#xxx" : "#yyy"` 三元硬编码，TeachingTheme.h 新增 14 个 `ide*()` inline 函数（ideBgMain/ideBgPanel/ideBgSidebar/ideFgPrimary/ideFgSecondary/ideBorder/ideAccent/ideHoverBg/ideSelectedBg/ideTitleBg/ideStatusBg/ideEditorBg/ideLineNumBg/ideLineNumFg），全部调用 `Theme::isDark()` 自适应，14 处样式表变量统一走 TeachingTheme
- **T4 文件树 Fluent 图标**（app/ide.cpp）：文件树原本用 QStyle::SP_DirIcon / SP_FileIcon 系统图标，视觉与 Fluent Design 不一致。改为 `Fluent::icon(Fluent::IconType::FOLDER)` / `DOCUMENT` / `CODE`（按文件扩展名 .mini/.txt 选 CODE，其他选 DOCUMENT），与 QFluentKit 风格统一
- **T5 Welcome 欢迎页 Fluent 化**（app/ide.cpp）：welcomePage_ 的标题 QLabel 与副标题 QLabel 改为 `TitleLabel` / `CaptionLabel`，与 WelcomeWizard Step 1 风格一致。按钮保留 QPushButton（暗色主题 Claude DS terra-cotta #D97757 品牌色，原代码注释明确说明 QFluentKit PrimaryPushButton 自绘不读 QSS）
- **T6 QGroupBox 全局样式表统一外观**（app/ide.cpp applyFluentStyle 末尾）：原本 QGroupBox 使用系统默认样式（无圆角、无统一边距、标题色不跟随主题），与 Fluent Design 风格不一致。新增全局 QSS 样式表（1px border + 6px radius + 12px margin-top + 8px padding-top + 标题左偏移 8px + 标题色跟随 TeachingTheme::textPrimary()），所有 QGroupBox 自动级联 Fluent 外观。决策：不替换为 SimpleCardWidget（高风险，会改变 19 个面板的父类层级与布局），改用全局样式表是最小侵入方案
- **T7 CodeJourneyInfoPanel Material 色清理**（gui/CodeJourneyInfoPanel.cpp buildJourneyHtml）：12 处 Material 色清理——6 处 5 阶段色（绿/蓝/紫/红 + 标题绿/输出绿）改为 `TeachingTheme::learningStageColor(0/2/3/4).name()`，6 处浅色 `<pre>` 背景（#f5f5f5 等）改为 `TeachingTheme::surface().name()` 跟随亮/暗主题。保留特有色 #FF9800（橙，源码阶段）/ #00BCD4（青，IR 阶段）/ #795548（棕，字节码阶段），这些是代码旅程特有语义色
- **T8 帮助对话框硬编码色替换**（app/ide.cpp showHelpDialog）：原本 7 处 `isDark() ? "#xxx" : "#yyy"` 三元硬编码（标题色/正文色/边框色/背景色/链接色等），全部改为 TeachingTheme 函数（textPrimary / textSecondary / border / surface / primary 等），与 applyFluentStyle 主题色板一致

> 关键架构决策：(1) TeachingTheme.h 仅新增 inline 函数（learningStageColor / info / hint + 14 个 ide*），不修改已有函数签名，保证向后兼容；(2) const char* HTML 拼接受限于 RichTextItemDelegate 接口，无法直接换 QColor::name()，保留并加注释标注语义关系是务实选择；(3) QGroupBox 全局样式表是比 SimpleCardWidget 替换更低风险的方案（避免改变父类层级与布局）；(4) Welcome 按钮保留 QPushButton 维持暗色主题 Claude DS terra-cotta 品牌色（原代码注释明确说明 QFluentKit 自绘不读 QSS）。全量 1668/1668 测试通过。

### 教学面板 Markdown 渲染接入（7 面板散文字段统一）

将 7 个教学面板的散文式说明字段统一通过 `gui/MarkdownRenderer.h` 的 `markdownToHtmlFragment` 渲染为 HTML 片段，替代各面板手写的 `"<p>" + raw_string + "</p>"` 模板，支持标题/粗体/斜体/行内代码/围栏代码块/列表/分割线等 markdown 子集：

- **CallStackPanel**（showScenario）：`s.description` 与 `s.teachingNote` 改用 markdown fragment
- **VariableInspectorPanel**（showExample）：`e.teachingNote` 与 `e.heapLayout` 改用 markdown fragment；保留 sourceExpr 的 `<pre>` 包裹
- **BytecodeTracePanel**（showDoc）：`d.semantics` 改用 markdown fragment；保留 operandFormat / stackEffect 单行字段
- **ClosureInspectorPanel**（populateScenarioDetail + populatePhaseDetail）：`s.description` / `s.teachingNote` / `p.description` 改用 markdown fragment；同时修复先前未调用 `toHtmlEscaped()` 的 HTML 注入隐患
- **ExceptionFlowPanel**（populateScenarioDetail + populatePhaseDetail）：同前，3 处字段改 markdown fragment + HTML 注入修复
- **MemoryModelPanel**（populateNanBoxDetail + populateGcPhases）：`b.description` 与 GcPhaseInfo 的 `p.description` 改用 markdown fragment
- **SyntaxExplorerPanel**（showCurrentItem）：`p.description` 与 `p.naturalLanguage` 改用 markdown fragment；naturalLanguage 移除 `<pre>` 包裹改由 markdown 处理多段格式；保留 ebnf 的 `<pre>`（字面文法表示）

> 关键架构决策：(1) 使用 `markdownToHtmlFragment` 而非 `markdownToHtml`——fragment 版本不含 `<html><body>` 包裹，便于嵌入各面板已有 HTML 模板（QString::arg 或 std::ostringstream 拼接）；(2) std::string 重载直接传递 Library 字段，避免调用点反复 `QString::fromUtf8`；(3) 移除手动 `<p>` 包裹避免与 markdown fragment 自带 `<p>...</p>` 嵌套产生无效 HTML；(4) 保留 id / title / ebnf / operandFormat / stackEffect / sampleCode 等单行字面字段不变，避免 markdown 误解析字面字符（如 EBNF 中的 `*` 会被识别为强调）；(5) ClosureInspector / ExceptionFlow 先前直接 `oss << s.description` 未转义，markdown 渲染器内部处理转义一并修复该潜在注入隐患。minilang_ide 构建成功，无编译错误。

## 架构

```
┌──────────────────────────────────────────────────────────┐
│  GUI 层 (app/ide.cpp, gui/*)                             │
│  QMainWindow + 各面板，仅处理事件与 UI 更新               │
├──────────────────────────────────────────────────────────┤
│  业务层 (app/IdeController)                              │
│  编译管线编排、Worker 线程管理、调试状态、VM 单步          │
├──────────────────────────────────────────────────────────┤
│  Worker 层 (app/InterpreterWorker)                       │
│  独立 QThread 执行解释器，信号回传结果                    │
├──────────────────────────────────────────────────────────┤
│  引擎层 (minilang_core 静态库)                           │
│                                                          │
│  Lexer -> Parser -> AST                                  │
│       |            |                                     │
│       |            +-> Formatter (代码格式化)             │
│       |            |                                     │
│       |            +-> Interpreter (引擎1: 树遍历)        │
│       |            |                                     │
│       |            +-> Compiler -> BytecodeChunk          │
│       |            |       |                             │
│       |            |       +-> VM (引擎2: 栈式)           │
│       |            |                                     │
│       |            +-> AstIRBuilder -> IRModule           │
│       |                    |         |                   │
│       |                    |         +-> BytecodeIRBackend│
│       |                    |         |    -> VM (引擎2)   │
│       |                    |                             │
│       |                    +-> RegisterBytecodeBackend    │
│       |                             |                   │
│       |                             +-> RegisterVM       │
│       |                                (引擎3: 寄存器式)  │
│       |                                                  │
│       +-> DebugController (断点/单步/条件求值)            │
└──────────────────────────────────────────────────────────┘
```

### 三引擎对比

| 特性 | 树遍历解释器 | 栈式 VM | 寄存器式 VM |
|------|------------|---------|------------|
| 执行方式 | 递归遍历 AST | 取指-解码-执行循环 | 取指-解码-执行循环 |
| 中间表示 | 无（直接执行 AST） | BytecodeChunk（~60 OpCode） | RegBytecodeChunk（~50 RegOp） |
| 数据传递 | 函数返回值 | 操作数栈（push/pop） | 32 虚拟寄存器（R0-R31） |
| IR 优化 | 不适用 | 可选（常量折叠/死代码消除） | 可选（+ 复制传播） |
| 调试支持 | 完整（DebugController） | 有限（VmStepper 单步） | 有限（VmStepper 单步） |
| 适用场景 | 教学调试、REPL | 通用执行、字节码可视化 | 性能对比、IR 研究 |

### 目录结构

| 目录 | 说明 |
|------|------|
| `lexer/` | 词法分析器，Token 类型定义 |
| `parser/` | 递归下降解析器，生成 AST |
| `ast/` | AST 节点定义与 Visitor 接口 |
| `interpreter/` | 树遍历解释器、Environment、Value、内置方法 |
| `compiler/` | 字节码编译器、IR 中间表示层、VM、BytecodeChunk |
| `debug/` | DebugController（断点/单步/条件求值/变量快照） |
| `formatter/` | 代码格式化器（Visitor 模式） |
| `gui/` | Qt6 GUI 组件（编辑器/AST 视图/调试面板等） |
| `app/` | IdeController、InterpreterWorker、main、Ide 主窗口 |
| `common/` | Diagnostic、Logger、IBackend、TypeChecker |
| `tests/` | GoogleTest 单元测试（1693 个） |
| `test_harness/` | 独立测试工具（AST/格式化器/调试一致性审计） |
| `samples/mini/` | MiniLang 示例程序（模块系统演示） |
| `docs/` | 开发指南与文档 |

## 构建与运行

### 环境要求

- **CMake** >= 3.20
- **C++20** 编译器（MSVC 19.51+ / GCC 13+ / Clang 16+）
- **Qt6** >= 6.0（Core / Gui / Widgets）
- **GoogleTest**（随 `third_party/` 提供，CMake 自动拉取）

### Windows 一键构建（推荐）

项目提供 `configure.bat` 自动检测本机 Visual Studio 与 Qt6 安装位置，调用对应的 CMake Preset 完成配置：

```powershell
# 1. 配置（首次）—— 自动初始化 MSVC 环境 + 检测 Qt6
configure.bat              # Debug（默认）
configure.bat release      # Release

# 2. 编译
cmake --build out/build/Qt-Debug
# 或：cmake --build out/build/Qt-Release

# 3. 运行 IDE（Qt DLL 已由 windeployqt 自动部署到同目录，可直接双击运行）
./out/build/Qt-Debug/minilang_ide.exe

# 4. 运行单元测试
./out/build/Qt-Debug/tests/minilang_tests.exe
# 或：cd out/build/Qt-Debug && ctest -R LexerTest.* --verbose  # 筛选单个用例
```

> **Qt 路径**：`configure.bat` 默认查找 `D:\qt\6.*\msvc2022_64` 与 `C:\qt\6.*\msvc2022_64`。如安装在别处，先设置环境变量：
> `set QTDIR=<你的Qt路径>/msvc2022_64` 再运行 `configure.bat`。

### 手动配置（替代方式）

若不使用 `configure.bat`，可直接调用 CMake（需自行初始化 MSVC 开发环境并设置 Qt 路径）：

```powershell
# 方式 A：使用 CMake Preset（需 CMakeUserPresets.json 指定 QTDIR）
cmake --preset Qt-Debug
cmake --build out/build/Qt-Debug

# 方式 B：手动指定生成器与 Qt 路径
cmake -S . -B build -G "NMake Makefiles" `
  -DCMAKE_PREFIX_PATH="<Qt路径>\msvc2022_64"
cmake --build build
```

### Linux / macOS

依赖：CMake >= 3.20、C++20 编译器（GCC 13+ / Clang 16+）、Qt6（Core/Gui/Widgets）

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH="<Qt安装路径>"
cmake --build build
./build/minilang_ide
```

### Qt 运行时依赖部署

构建完成后，CMake 会自动调用 `windeployqt` 将 Qt6 DLL（`Qt6Core[d].dll` / `Qt6Gui[d].dll` / `Qt6Widgets[d].dll`）和 `platforms\qwindows[d].dll` 插件复制到可执行文件同目录。因此：

- **构建产物目录下的 `.exe` 可直接双击运行**，无需手动配置 PATH。
- 若手动拷贝 `.exe` 到其他目录，需重新运行 `windeployqt <exe路径>` 部署依赖，或确保目标机器已安装 Qt6 且其 `bin` 目录在 PATH 中。

### 快捷脚本

```powershell
./scripts/build_ide.bat   # 构建 IDE
./scripts/run_tests.bat   # 构建并运行测试
```

> 注意：`scripts/*.bat` 中包含机器特定的硬编码路径（如 `D:\qt\6.10.3\...`），在其他机器上需按本机环境修改。**首次构建建议使用 `configure.bat`**，它会自动探测路径。

### CMake 选项

| 选项 | 默认 | 说明 |
|------|------|------|
| `MINILANG_UNITY_BUILD` | OFF | C1：Unity Build 批量编译，首次构建加速 30-50% |
| `MINILANG_VM_PROFILING` | OFF | C3：VM 解释循环 opcode 执行计数 profiling |
| `MINILANG_BUILD_DOCS` | OFF | D1：Doxygen API 文档生成（target `docs`） |
| `MINILANG_ENABLE_I18N` | OFF | D2：Qt Linguist 国际化 i18n 工具链 |
| `MINILANG_ENABLE_LTO` | ON | Release 构建启用链接时优化（LTO） |
| `MINILANG_USE_CCACHE` | OFF | 启用 ccache 缓存（如可用） |

```powershell
# 启用 VM profiling + Doxygen 文档
cmake --preset windows-msvc-debug -DMINILANG_VM_PROFILING=ON -DMINILANG_BUILD_DOCS=ON
cmake --build out/build/debug --target minilang_tests docs
```

> **构建问题排查**：若遇到大规模 SEH 0xc0000005 崩溃（尤其是修改头文件 struct 布局后），优先删除整个 `out/build/debug` 目录后全量重配置 + 重编译——Ninja 的头文件依赖追踪在某些 struct 布局修改场景下会失效，导致 stale `.obj` 文件 ABI 不一致。

## 快速开始

构建完成后，运行 IDE：

```powershell
# Windows
./out/build/Qt-Debug/minilang_ide.exe
```

```bash
# Linux / macOS
./build/minilang_ide
```

在编辑器中输入以下代码，点击运行：

```text
print("Hello, MiniLang!");

fun fib(n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

for (var i = 0; i < 10; i = i + 1) {
    print(fib(i));
}
```

更多语法请参考"语言示例"章节。

## 开发环境

### 推荐 IDE

- **CLion**：直接打开 `CMakeLists.txt`，CMake Preset 自动识别，调试体验最佳
- **Qt Creator**：打开 `CMakeLists.txt`，Qt 集成度高，UI 设计方便
- **VS Code**：安装 CMake Tools + C/C++ 扩展，轻量灵活

### CMake Preset 说明

项目采用双层 Preset 体系：

- `CMakePresets.json`（仓库内）：共享配置，定义 `windows-msvc-debug` / `windows-msvc-release` 等通用 Preset
- `CMakeUserPresets.json`（gitignore）：机器特定配置，存放 Qt 安装路径等本地信息

首次克隆后运行 `configure.bat` 会自动生成 `CMakeUserPresets.json`。

### 调试技巧

- **解释器调试**：在 IDE 中点击"调试"按钮，支持断点、单步、变量监视、调用栈
- **VM 调试**：打开"VM 栈面板"查看字节码执行过程，支持单步执行
- **IR 调试**：启用 IR 路径后打开"IR 可视化面板"，查看中间表示与优化效果
- **单元测试调试**：在 CLion/Qt Creator 中直接运行 GoogleTest 用例，或使用 `ctest -R <pattern> --verbose`

### 代码规范

- 编码风格遵循项目现有约定（参见 `AGENTS.md`）
- 新增代码需通过全量测试（1693/1693）+ formatter_audit 审计用例验证
- 提交前运行 `./scripts/run_tests.bat` 确认无回归
- 详细工程约定参见 [docs/development.md](./docs/development.md)

## 测试

项目包含 **1693 个 GoogleTest 单元测试**（194 个测试套件），覆盖所有核心模块：

### 前端模块

| 测试套件 | 覆盖模块 |
|----------|----------|
| `LexerTest` | lexer/ |
| `ParserTest` | parser/ |
| `LexerAudit` / `ParserAudit` | 审计套件 |

### 解释器

| 测试套件 | 覆盖模块 |
|----------|----------|
| `InterpreterE2E` | interpreter/ (端到端) |
| `ArrayBuiltin*Test` / `DictBuiltin*Test` / `StringBuiltin*Test` | interpreter/BuiltinMethods（按类型分组） |
| `BuiltinErrorHandlingTest` / `BuiltinObjectModifiedTest` | 内建方法错误路径 |
| `Value*Test`（13 个套件） | interpreter/Value（构造/COW/深拷贝/相等/数组/字典/实例/闭包等） |

### 编译器与虚拟机

| 测试套件 | 覆盖模块 |
|----------|----------|
| `CompilerBasicTest` / `CompilerVariableTest` / `CompilerControlFlowTest` / `CompilerFunctionTest` / `CompilerClassTest` | compiler/Compiler |
| `VME2E` / `RegVME2E` / `VMConsistency` | compiler/VM / RegisterVM (端到端 + 一致性) |
| `CompilerConstantPoolTest` / `CompilerGlobalSlotTest` | compiler/ 常量池 / 全局槽位 |
| `CompilerAuditCP1` / `CompilerAuditPRES1` / `CompilerAuditTRY1` / `CompilerAuditMOD1` | compiler/ 审计套件（常量池去重 / 预扫描 / try-catch / 模块路径） |
| `CompilerAudit2CP2` / `CompilerAudit2CP3` / `CompilerAudit2UV1` / `CompilerAudit2IRPOP1` / `CompilerAudit2INHIR1` / `CompilerAudit2TRYLEAK1` / `CompilerAudit2IRPRES1` / `CompilerAudit2DEF1` / `CompilerAudit2INHREG1` / `CompilerAudit2DEAD1` / `CompilerAudit2InstrSize` | compiler/ + IR + RegisterVM 第二轮深度审计（嵌套左值 / 闭包 / IR POP / 字段默认值 / try-catch 恢复 / 预扫描 / 默认参数 / 字段去重 / 死代码） |

### IR 中间层

| 测试套件 | 覆盖模块 |
|----------|----------|
| `IRFunctionTest` / `IRBuilderTest` / `BytecodeIRBackendTest` / `IRE2E` / `IROptimizeTest` / `IRToStringTest` | compiler/IR（中间表示层） |

### 一致性与类型

| 测试套件 | 覆盖模块 |
|----------|----------|
| `ConsistencyDiff` | 三后端一致性差分测试（190 用例） |
| `CompilerTypeCheckTest` | 类型检查器（含 float->int 拒绝断言强化） |
| `BackendConsistency` | 三后端类型注解一致性 |

### 审计与回归

| 测试套件 | 覆盖模块 |
|----------|----------|
| `ASTCompleteness` / `IntegrationAudit` | 审计套件 |
| `CompilerAudit` / `CompilerAudit2` / `IRAudit` | Compiler/IR 模块审计回归（含 P0/P1/P2 修复） |
| `ThreeEngAudit*`（13 个套件） | 三执行引擎审计回归（Interpreter + StackVM + RegisterVM，含 INTP-1/2/3 + VM-01~05 + REGVM-1/2/3/4 共 12 个 P2 修复） |
| `DebugAuditMainFrame` / `DebugAuditStopReset` / `DebugAuditModuleCache` / `DebugAuditSandbox` / `DebugAuditCallStack` / `DebugAuditRegression`（6 个套件） | DebugController + REPL 审计回归（调用栈 main 帧 / stopRequested 重置 / 模块缓存路径规范化 / 沙箱状态隔离 / 闭包调用栈名 共 15 个 P1/P2 修复） |
| `FormatterAuditF01` / `FormatterAuditF03` / `FormatterAuditF04` / `FormatterAuditF05` / `FormatterAuditF06` / `FormatterAuditF07` / `FormatterAuditF08` / `FormatterAuditIdempotency` / `FormatterAuditRoundTrip` / `FormatterAuditEdge` / `FormatterAuditExport`（11 个套件，25 用例） | Formatter + GUI 全模块审计回归（1 P0 + 9 P1 + 56 P2 = 66 Bug，63 个已修复 + 3 个已知限制。覆盖格式化器缩进/幂等性/往返等价、export 空行、类方法空行、NaN 格式化、空 throw 等） |
| `NaNBoxTest` / `IRShadowFix` / `NestedLvalueProbe` / `MethodCallProbe` | 回归与探针测试 |
| `TeachingPanelsBugHunt` / `TeachingPanelsSyntax` / `TeachingPanelsLabManual`（3 个套件，17 用例） | 教学面板数据完整性审计：BugHuntLibrary（10 道历史 Bug 题目 ID 唯一性 / 关键字段非空 / severity 在 P0/P1/P2 集合 / sourceCode 可被 Lexer 解析）、SyntaxProductionLibrary（10 条产生式 name 唯一 / 关键字段非空 / sampleCode 可被 Lexer 解析 / 核心产生式 ID 齐全）、LabManualContent（8 章节 id 唯一 / lab-NN 命名约定 / markdown 含"目标"和"实验步骤"两节 / sampleCode 可被 Lexer 解析） |
| `TeachingPanelsMemoryModel` / `TeachingPanelsIRTransform` / `TeachingPanelsProfile`（3 个套件，23 用例） | 第二波教学面板数据完整性审计：MemoryModelLibrary（7 个 NaN-boxing 示例 ID 唯一 / bits 与 NaNBox 编码一致 / 4 个 RefCount 场景步骤非空 / 6 个 GC 阶段说明非空）、IRTransformLibrary（8 个 lowering 示例 + 3 个优化 pass，IR 文本含 'function' 关键字，优化后指令数 ≤ 优化前）、ProfileLibrary（6 个性能场景 ID 唯一 / category 在合法集合 / iterations ≥ 1） |
| `TeachingPanelsCallStack` / `TeachingPanelsVariableInspector` / `TeachingPanelsBytecodeTrace`（3 个套件，13 用例） | 第三波教学面板数据完整性审计：CallStackLibrary（6 个调用栈场景 ID 唯一 / 关键字段非空 / 关键场景 ID 齐全 / expectedFrames 非空）、VariableInspectorLibrary（9 种类型示例 ID 唯一 / 关键字段非空 / 关键类型名齐全 / 标量示例含 NaN-boxing 位编码）、BytecodeTraceLibrary（18 个 OpCode 文档 ID 唯一 / 关键字段非空 / category 合法 / 关键 OpCode 齐全） |
| `TeachingPanelsOpCodeProfile` / `TeachingPanelsBreakpointCondition`（2 个套件，14 用例） | 第二档教学面板数据完整性审计：OpCodeProfileLibrary（12 个 OpCode 性能文档 ID 唯一 / 关键字段非空 / category 合法 / 关键 OpCode 齐全 / perfNote 含性能提示关键词）、BreakpointConditionLibrary（8 个条件断点场景 ID 唯一 / 关键字段非空 / 关键场景 ID 齐全 / condition 为表达式 / sampleCode 含断点行 / description 提及条件语义） |
| `TeachingPanelsExceptionFlow` / `TeachingPanelsClosureInspector`（2 个套件，24 用例） | 第三档教学面板数据完整性审计：ExceptionFlowLibrary（8 个异常场景 ID 唯一 / 关键字段非空 / 关键场景 ID 齐全 / sampleCode 含 throw / propagationPath 非空 / description 提及异常语义，6 个传播阶段 phase/category/stackEffect 合法）、ClosureInspectorLibrary（8 个闭包场景 ID 唯一 / 关键字段非空 / 关键场景 ID 齐全 / sampleCode 含闭包 / captureType 含 upvalue / description 提及闭包语义，6 个 upvalue 阶段 phase/category/stackEffect 合法） |
| `IROptReplayAudit` / `MemoryAnimLibraryPhases` / `MemoryAnimLibraryTypes` / `MemoryAnimLibraryConsistency` / `BugHuntVariantAudit`（5 个套件，37 用例） | 第五档 Top 3 改进方向数据完整性审计：IROptReplayLibrary（5 个 IR 优化回放场景 ID 唯一 / 关键场景 ID 齐全 / 每场景 3 步骤 / passName/passRound/irSnapshot/instrCount/decisions/modifiedCount 字段非空，irSnapshot 含 'function' 关键字）、MemoryAnimLibrary（4 个 GC 阶段 mark/sweep/reset/idle 顺序与 color #RRGGBB 格式 / 6 种堆对象类型 ArrayData/DictData/InstanceData/StringData/ClosureData/BoundMethodData 字段非空 / phase 与 type 一致性）、BugHuntVariantLibrary（7 个变体 ID 唯一 / 关键变体 ID 齐全 / parentId 必须存在于 BugHuntLibrary / sourceCode 可被 Lexer 解析 / description 含"差异"+"挑战点"关键词） |
| `LearningPathDataAudit` / `LearningPathProgressAudit` / `LearningPathPrereqAudit`（3 个套件，15 用例） | 功能 6 学习路径地图数据完整性审计：LearningPathData（活动数 ≥ 18 / ID 唯一 / 关键 ID 齐全 / stage 0-4 / prerequisites 引用已存在 ID）、LearnerProgressStore（初始空进度 / markCompleted 解锁联动 / stageProgress 计算 / overallProgress 计算 / nextRecommended 返回未完成+已解锁 / reset 清空）、Prerequisites（DFS 环检测 / 阶段零无前置 / lab-02 前置含 lab-01 / bug-hunt-expert 前置含 intermediate） |
| `ErrorHintEngineSpelling` / `ErrorHintEnginePatterns`（2 个套件，12 用例） | 功能 12 错误信息友好化引擎审计：Levenshtein 编辑距离计算 / 阈值边界 / 空候选集 / 多候选择优 / 大小写敏感 / 数字符号 / 缺分号模式 / 未知模式透传 / 未定义变量拼写建议 / 未定义函数无提升提示 / 除零提示 / 越界提示 / 类型错误提示 |
| `SandboxLibraryAudit`（1 个套件，18 用例） | 功能 5 VM 栈沙盒关卡数据完整性审计：SandboxLibrary（5 关卡 / level 1-5 连续 / goal/availableOps/teachingPoint/hint 非空 / difficulty 1-3 / 关卡 1 expectedSequence 含 PUSH_INT+ADD+PRINT / 关卡 2 与 3 availableOps 相同但 expectedSequence 不同 / 关卡 4 含 PUSH_STRING / 关卡 1-3 expectedOutput 为 "3"/"7"/"9" / 关卡 1-4 expectedSequence 执行后确实得到 expectedOutput 自洽性验证） |
| `MagicCommandsLibraryAudit` / `MagicCommandsHandlerAudit`（2 个套件，13 用例） | 功能 11 REPL %magic 命令系统审计：命令数 ≥ 10 / 命令名唯一 / 必需命令齐全（help/disassemble/ir/compare/profile/memory/ast/tokens/reset/version）/ description 非空 / syntax 以 % 开头 / %version 输出含 "MiniLang" / %help 输出含所有命令名 / %disassemble+controller=null 友好错误 / %ir+controller=null 友好错误 / %unknown 未知命令提示 / 空输入不处理 / 非 % 开头不处理 |
| `TeachingPanelsE2E.*`（9 个套件，55 用例） | 教学面板端到端交互测试：8 个面板（ExceptionFlow / ClosureInspector / MemoryModel / IRTransform / CallStack / VariableInspector / BytecodeTrace / BreakpointCondition）实例化 + 子页切换（QStackedWidget 索引验证）+ 列表选择 → 详情刷新联动 + 列表项数与 Library 静态数据一致 + QTimer 在 nullptr controller 下不触发（防御性 `stopAllTimers` + 无活动定时器断言）+ 跨面板所有 8 个面板均含 QStackedWidget。验证 `controller_=nullptr` 构造路径安全（refreshLive/refreshAnimState 早返回） |

### GUI 编辑器审计

| 审计范围 | 修复内容 |
|----------|----------|
| `gui/CodeEditor` + `gui/SyntaxHighlighter` + `gui/FindReplacePanel` | GUI 编辑器三模块系统性审计（2 P1 + 14 P2，共 16 个 Bug，15 个已修复 + 1 个跳过）。覆盖块注释状态编码不匹配、文档修改后断点/折叠偏移补偿、行号区域事件处理、查找替换面板交互、语法高亮状态机（插值内嵌套块注释/字符串跨行、数字字面量 0x/0b/0o 前缀、关键字长度上限）等。仅修改 gui/ 目录下 3 个模块共 5 个文件，不影响其他模块。详见 CHANGELOG 2026-07-04 GUI 编辑器三模块审计条目 |

运行测试：

```powershell
# 方式 1：直接运行测试二进制
./out/build/Qt-Debug/tests/minilang_tests.exe

# 方式 2：通过 CTest（已用 gtest_discover_tests 注册为独立用例，可筛选）
cd out/build/Qt-Debug
ctest                                  # 运行全部
ctest -R LexerTest.* --verbose         # 筛选指定套件
ctest -R "VME2E\..*" --output-on-failure
```

测试覆盖要点：
- **正常路径**：所有语言特性的 happy path 覆盖
- **错误路径**：除零、类型违反、数组越界、空指针访问、未定义变量/函数等
- **三后端一致性**：同一代码在 Interpreter / StackVM / RegisterVM 行为一致。整数除法截断向零、`and/or` 返回操作数原值、整数溢出检测均已三后端统一。已知差异：非方法上下文 super 错误消息文本不同（错误类型相同均为 RuntimeError，三引擎均不被 try/catch 捕获，行为一致）
- **边界条件**：接近 MAX_RECURSION_DEPTH、MAX_FRAMES 的极限场景
- **类型检查**：类型注解强制、null 兼容性、int/float 宽化规则、float->int 拒绝（断言强化：同时验证警告消息内容含期望类型名）
- **格式化器质量**：幂等性 + 表达式级 AST 往返等价性（19 用例）+ 语句级 AST 往返等价性（16 用例，覆盖 if/for/while/class/try 等结构）
- **test_harness**：独立 cl.exe 编译，含 formatter_audit（100+ 审计用例）、stress_test（100 个压力测试）、debug_test（调试不变量审计）

## 语言示例

### 基础语法

```text
// 变量与类型注解
var name = "MiniLang";
int count = 0;
float pi = 3.14159;

// 函数与闭包
fun makeCounter() {
    var n = 0;
    fun inc() {
        n = n + 1;
        return n;
    }
    return inc;
}

var counter = makeCounter();
print(counter());  // 1
print(counter());  // 2

// 类与继承
class Animal {
    fun init(name) {
        this.name = name;
    }
    fun speak() {
        print(this.name + " makes a sound");
    }
}

class Dog extends Animal {
    fun speak() {
        super.speak();
        print(this.name + " barks");
    }
}

var dog = Dog("Rex");
dog.speak();

// 数据结构
var arr = [1, 2, 3];
arr.push(4);
print(arr.len());  // 4

var cfg = {"key": "value", "count": 42};
print(cfg["key"]);  // value
```

### 进阶特性

```text
// 字符串插值
var name = "World";
var items = [1, 2, 3];
print("Hello, {name}! You have {items.len()} items.");

// 异常处理
fun divide(a, b) {
    if (b == 0) throw "Division by zero";
    return a / b;
}

try {
    var result = divide(10, 0);
    print(result);
} catch (e) {
    print("Error: " + e);  // Error: Division by zero
}

// 模块导入（需配合 samples/mini/ 中的模块文件）
// import { greet } from "string_utils.mini";
// print(greet("MiniLang"));  // Hello, MiniLang!

// 类型注解与检查
int add(int a, int b) {
    return a + b;
}

// print(add(1, 2));     // OK: 3
// print(add(1.5, 2));   // 警告: float 值不能赋给 int 参数
```

## 示例程序

`samples/mini/` 目录包含一组演示模块系统的示例程序：

| 文件 | 说明 |
|------|------|
| `main.mini` | 主程序，演示跨文件 import 与多模块协作 |
| `math_utils.mini` | 数学工具模块（add/multiply/factorial/fibonacci） |
| `string_utils.mini` | 字符串工具模块（greet/repeat/isEmpty/padLeft） |
| `shapes.mini` | 图形类模块（Shape/Circle/Rectangle 继承体系） |

运行方式：在 IDE 中打开 `samples/mini/main.mini` 并点击运行。

## 贡献

本项目为教学项目，欢迎提交 Issue 报告问题或建议。如需贡献代码，请先开 Issue 讨论方案。

详细的工程约定与开发规范参见 [docs/development.md](./docs/development.md)。

## 许可证

本项目为教学用途，采用 MIT 许可证。详见 [LICENSE](./LICENSE)。
