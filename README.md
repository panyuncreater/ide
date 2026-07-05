# MiniLang IDE

![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![Qt6](https://img.shields.io/badge/Qt-6-green)
![Build](https://img.shields.io/badge/build-passing-brightgreen)
![Tests](https://img.shields.io/badge/tests-1352-brightgreen)
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

- **代码编辑器**：语法高亮、行号、断点标记、错误下划线、当前执行行高亮、代码折叠（基于块结构）、查找替换（带迭代限制防 UI 阻塞）
- **词法/语法分析可视化**：Token 表格（10000 行显示上限）、AST 树形图（自动布局，可缩放/平移）
- **字节码反汇编**：主 chunk 与函数 chunk 分段显示，单步高亮当前指令，同时兼容栈式 VM（OpCode）和寄存器式 VM（RegOp）
- **IR 中间表示层**：可选 AST -> IR -> Bytecode 三段式编译，52 个 IROp 指令，支持 SSA-like 虚拟寄存器、多函数 lowering、闭包 upvalue 捕获、写回指令、全局槽位分配、优化 pass（常量折叠 / 死代码消除 / 复制传播）、IR 可视化面板与 IR 调试器集成。IR 路径退出后自动恢复配置，不污染后续编译路径
- **三执行引擎**：
  - 树遍历解释器（支持 REPL 续行输入、条件断点沙箱求值）
  - 栈式字节码 VM（支持单步、栈/全局变量监视、条件断点）
  - 寄存器式 VM（32 虚拟寄存器 R0-R31，50+ RegOp，IR 三地址码直接 lowering）
- **调试器**：断点（含条件断点，条件求值沙箱隔离程序状态）、单步进入/跳过/跳出、变量监视、调用栈、首行断点 pre-execution 检查。双后端调试路径暂停语义文档化：Interpreter pre-execution 暂停，VM post-execution 检查（IP 指向下条指令，变量快照反映断点行 pre-execution 状态）
- **类型检查器**：编译期类型注解检查，三后端运行时统一强制，类型违反报告警告
- **代码格式化器**：可配置缩进/花括号风格/运算符空格，保留注释。幂等性 + 往返不变量验证（AST 结构比较），括号保留遵循运算符优先级（右嵌套同优先级加括号，左嵌套冗余括号丢弃）
- **REPL 面板**：交互式求值，支持多行续行（未闭合 `{ ( [` / 未闭合字符串 / 未闭合块注释 / `try` 缺失 `catch` 自动续行，正确处理字符串插值嵌套上下文），续行中按空行可中止；表达式语句自动求值并打印结果；异步执行不阻塞 UI；`help`/`clear` 特殊命令

### 教学增强面板（第一波 + 第三波）

围绕"三套执行引擎 + 共享 IR 层 + 历史真实 Bug 沉淀"三大独特性设计，强化 IDE 的教学价值：

- **编译管线可视化面板**（Ctrl+Shift+P）：5 步流程导航条（源码 → Token → AST → IR → 字节码），逐步展示编译管线每一阶段的中间产物。Token 表格含 type/lexeme/line/column 字段；AST 步骤展示 dumpAst 摘要；IR 步骤展示 IRModule 反汇编；字节码步骤展示 BytecodeChunk 反汇编
- **三后端并行对比面板**（Ctrl+Shift+B）：同一源码顺序运行 Interpreter / StackVM / RegisterVM 三条路径，每路径独立计时（微秒精度）+ 输出比对，自动统计行级差异并给出一致性 PASS/FAIL 结论。三后端输出一致即"三后端语义等价"教学验证
- **Bug 狩猎面板**（Ctrl+Shift+H）：基于项目历史真实 Bug 训练调试能力。题库 10 道（BUG-CP-1/CP-2/UV-1/IR-POP-2/DEF-1/MOD-1/DBG-1/REGVM-2/REPL-1/F-04），每题含背景/源码/期望行为/Bug 行为/递进提示/根因分析。三栏布局：题目列表 + 背景说明 + 内嵌编辑器/输出。可一键加载源码到主编辑器进一步调试
- **交互式语法探索器**（Ctrl+Shift+S）：10 条核心产生式参考（var-decl/if-stmt/while-stmt/for-stmt/fun-decl/class-decl/try-stmt/import-stmt/string-interp/data-structures/operators），每条配 EBNF 形式 + 文字说明 + 可运行样例代码。三栏布局：产生式列表 + 说明 + 代码/输出
- **内置实验手册**（Ctrl+Shift+L）：8 个实验章节（lab-01~lab-08：词法分析 / 递归下降 / 树遍历解释器 / 栈式 VM / 寄存器式 VM / 三后端一致性 / 内存模型 / Bug 狩猎），每章含目标/关键概念/实验步骤/验证断言/进阶/可一键加载样例代码。Markdown 内容通过 QTextBrowser 渲染

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
| `tests/` | GoogleTest 单元测试（1329 个） |
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
- 新增代码需通过全量测试（1329/1329）+ formatter_audit 审计
- 提交前运行 `./scripts/run_tests.bat` 确认无回归
- 详细工程约定参见 [docs/development.md](./docs/development.md)

## 测试

项目包含 **1329 个 GoogleTest 单元测试**（104 个测试套件），覆盖所有核心模块：

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
