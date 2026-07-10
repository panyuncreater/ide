
---

**P1 — 封面**

标题：MiniLang：让编译原理课「活」起来的三后端教学语言

底部：答辩人：___________ 日期：___________

---

**P2 — MiniLang 是什么**

标题：一张全景图

视觉：只放一张架构图（建议后续插入 SVG）

讲解要点："这就是 MiniLang 的全貌。一段源码经过词法分析、语法分析生成 AST，然后有三条执行路径——树遍历解释器直接跑，编译器先把 AST 翻译成中间表示 IR 再做优化，最后降低成两种字节码分别由栈式 VM 和寄存器式 VM 执行。还有 Formatter 做代码格式化。"

底部过渡语："那为什么要做三条路径？我们一个一个来看。"

---

**P3 — 后端一：树遍历**

标题：最直觉的执行方式

大字数据：6,535 行

正文卡片（3 张）：

卡片 1 — 工作原理：
"拿到 AST 就直接走。每个节点 visit 一下就出结果。这是最自然的实现方式。"

卡片 2 — NaN-boxing 值表示：
"sizeof(Value) = 8 字节。int48 内联存储 + COW 侵入式引用计数。在 8 字节里同时编码类型标签和值。"

卡片 3 — 完整语言支持：
"REPL / 模块系统 (import/export) / OOP (类/继承/super) / 异常处理 (try/catch/finally) / 闭包"

底部深色条（抛出问题）：
"它跑起来没问题——但每个节点都是一次虚函数调用，递归深了开销就上来了。有没有办法把遍历的开销从运行时挪到编译时？"


---

**P4 — 后端二：编译成字节码**

标题：把思考前置

顶部数据流图（横向箭头链）：
AST → AstIRBuilder → IR（5 个优化 Pass）→ BytecodeIRBackend → Stack Bytecode

5 个优化 Pass 标签条（横向排列）：
常量折叠 | 拷贝传播 | 死代码消除 | CSE | 循环展开

正文区域（2×2 卡片网格）：

卡片 1 — IR 中间层：
"SSA 风格的中间表示。优化在 IR 上做，而不是在 AST 上——因为 AST 结构太松散，几乎不可能做死代码消除或公共子表达式消除。"

卡片 2 — 75 条 OpCode：
"算术 / 比较 / 控制流 / 闭包 / 类 / 异常 / 容器。覆盖完整的操作码集。"

卡片 3 — 全局槽位优化：
"Compiler 预扫描 globalSlots_，用数组索引替代哈希查找。4 条专用操作码（GET/SET/DEFINE/DELETE_GLOBAL）。"

卡片 4 — Lua 式 upvalue 闭包：
"open/closed upvalue 方案。multimap 管理，O(log n + k) 关闭。3 条操作码（GET/SET/CLOSE_UPVALUE）。"

底部深色条（抛出问题）：
"栈式 VM 每条指令都要 push 和 pop。1 + 2 * 3 需要 7 条指令。能不能少做几次栈操作？"
---

**P5 — 后端三：寄存器 VM**

标题：更激进的答案

核心对比区域——同一段代码的两种字节码：

左侧卡片「Stack Bytecode」深色背景：
```
INT_CONST  0, 1      // push 1
INT_CONST  0, 2      // push 2
INT_CONST  0, 3      // push 3
MULTIPLY              // pop 2,3 → push 6
ADD                   // pop 1,6 → push 7
PRINT                 // pop 7 → print
POP                   // 清理
```
标注：7 条指令，6 次栈操作

右侧卡片「Register Bytecode」深色背景：
```
LOAD R0, 1
LOAD R1, 2
LOAD R2, 3
MUL  R3, R1, R2     // R3 = 2*3 = 6
ADD  R4, R0, R3     // R4 = 1+6 = 7
PRINT R4
```
标注：6 条指令，0 次栈操作

底部设计要点：
"线性扫描寄存器分配器在 IR 降低阶段完成。与 Stack VM 共享同一 IR 层（AstIRBuilder），仅后端不同"

底部过渡语：
"现在同一个程序，三种跑法，三种性能。"

---

**P6 — 三后端同台**

标题：让差异看得见

顶部强调：
"输入一段代码，三个后端同时跑，输出必须一模一样。"

插入教学板块中三后端对比的图片：



---

**P7 — 调试：两套系统，一个目标**

标题：为什么需要两套调试器？

正文分两个卡片（左右并列）：

左侧卡片「DebugController — AST 节点级」（深色顶部条）：
- 面向 Interpreter 后端
- 在 AST 节点级别暂停，精确到表达式
- 断点 + 条件表达式求值（DebugEvaluator）
- Step In / Step Over / Step Out
- 跨线程架构（worker 线程 + condition_variable）
- "学生用它理解程序的语义——变量是什么值，调用栈长什么样"

右侧卡片「VmStepper — 字节码级」（深色顶部条）：
- 面向 Stack VM + Register VM
- 在字节码级别暂停，能看到栈帧和寄存器状态
- 同样的断点 + Step 能力，独立实现
- 主线程内联执行
- "学生用它理解底层执行——指令是怎么一步步走的，栈是怎么变化的"

---

**P8 — 教学系统总览**

展示学习中心截图

底部过渡语：
"21 个教学面板，按 4 大类组织：入门导览 / 编译前端 / 执行引擎 / 深入实战。"

---

**P9 — 基础教学工具**

标题：从词法到执行，每个环节都有交互工具
先展示「AST 搭建玩具」截图，然后动画切换「VM 栈沙盒」截图

AST部分
正文："自己动手搭建 AST，直观理解运算符优先级和结合性如何影响树的结构。"

VM沙盒部分
正文："手动执行每条指令，观察操作数栈的变化——比看 PPT 上的栈帧图有效一百倍。"

---

**P10 — 闭包检查器**

标题：让最抽象的概念变得可见

叙事引入：
"闭包是编译原理课最抽象的概念之一——变量到底存在栈上还是堆上？函数返回后变量去哪了？"

先插入学习中心闭包演示截图，

正文：
"每个场景都可以单步执行，观察变量在栈和堆之间的迁移。从最简单的 var capture 到相互递归闭包，难度递进。"

---

**P11 — Bug 狩猎场**

标题：调试能力的终极考场

叙事引入：
"不只是写代码——而是给你一段有 bug 的代码，让你找出来。"

插入实际截图


---

**P12 — 工程质量**

标题：这不是原型

顶部定调大字：
"109K 行代码 · 1700+单元测试 · 跨平台 CI"

左侧区域「测试金字塔」：引入Google Test框架

从底到顶（金字塔形，5 层）：
底层：单元测试 47 文件 / 27,319 行
（TestLexer / TestParser / TestValue  / TestBuiltinMethods  / TestCompiler / TestIR  / TestInterpreterE2E / TestVME2E ）

第二层：三后端一致性测试 + Fuzz

第三层：审计测试 AuditBatch 1-5

第四层：教学面板测试 5 审计文件 + E2E 

顶层：test_harness 9 个独立可执行文件

右侧区域「CI 流水线」：

6 个 Job 卡片（纵向排列）：
build-test：跨平台 Win/Ubuntu/macOS + ctest -j4 + CPack
docker-build：Docker 多阶段构建验证
coverage：Windows OpenCppCoverage → Codecov
coverage-linux：Linux gcovr → Codecov
test-harness：独立测试工具构建
static-analysis：cppcheck + clang-format

底部横条：
"Windows (MSVC) | Ubuntu (GCC) | macOS (Clang) · Qt 6.8.3 LTS · CPack: ZIP / TGZ / DragNDrop"

---

**P13 — ai协作经验**
1.项目记忆管理，规则制定
2.任务拆分细致，分模块完成，提升编码速度
3.高质量promote，需求描述准确
4.善用skill，对于反复出现的问题，让ai总结为skill，相似问题直接调用
---

**P14 — 收尾**

谢谢老师
---
