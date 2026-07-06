# MiniLang IDE 项目分析与改进方案

> **文档用途**：供 AI Agent 逐项实施的功能规格说明  
> **基于版本**：AGENTS.md / README.md / 全部核心源码审读（2026-07-06）  
> **项目定位**：教学型编程语言 IDE（C++20 / Qt6）



## 二、初学者友好度审计

### 2.1 现有教学资源清单

| 资源类型 | 数量 | 内容概要 | 文件位置 |
|---------|------|---------|---------|
| 实验手册 | 8 章（lab-01~08） | 词法→解析→解释器→栈VM→寄存器VM+IR→一致性→内存模型→Bug狩猎 | `gui/LabManualContent.cpp` |
| 语法产生式 | 11 条 | EBNF 文法 + 说明 + 可运行样例 | `gui/SyntaxProductionLibrary.cpp` |
| Bug 狩猎题库 | 10 道 | 真实历史 Bug，含背景/源码/期望/Bug行为/3级提示/根因 | `gui/BugHuntLibrary.cpp` |
| 内存模型面板 | 4 子页 | NaN-boxing 7示例 / COW 4场景 / GC 6阶段 / 实时动画 | `gui/MemoryModelPanel.h` |
| 编译管线面板 | 5 步导航 | 源码→Token→AST→IR→字节码 | `gui/PipelineViewer.h` |
| 其他教学面板 | 11 个 | 三后端对比/IR变换/性能剖析/调用栈/变量检查器/字节码轨迹/异常流/闭包检查器/条件断点 | `gui/*.h, *.cpp` |

### 2.2 做得好的地方 ✅

1. **实验手册结构清晰**：每章有"目标→关键概念→实验步骤→验证断言→进阶"的标准模板
2. **样例代码可一键加载**：`loadSampleRequested` 信号让学习者不用手动敲代码
3. **Bug 狩猎有递进提示**：3 级 hint 机制（`hintLevel_` 从 0 递增），不会一次性剧透答案
4. **内存模型面板预留实时动画子页**：第 4 子页已有 `animTimer_` 500ms 自动刷新 + `refreshAnimState()` 接口
5. **三后端对比一键运行**：`BackendComparePanel::runComparison()` 顺序执行三条路径并展示差异

### 2.3 初学者 7 大痛点 ❌

#### 痛点 1：缺少「零基础入门」引导

**证据**：`LabManualContent.cpp` 第一章直接从「词法分析 — 从字符到 Token」开始，假设学习者已经知道"什么是编译器""为什么要分词法和语法分析"。没有任何 Hello World 式的预热体验。

**影响**：对从未接触过编译原理的人来说，"Token""AST""字节码"这些术语像天书。类比：教编程第一课不讲 `print("hello")`，直接讲内存对齐。

#### 痛点 2：概念密度过大

**证据**：lab-03 的样例代码是 `fib(n)` 递归函数，关键概念一口气列出 4 个：Visitor 模式、Environment 链式作用域、DebugController 断点/单步、REPL 续行判断。进阶建议直接提到"尝试 3 层嵌套闭包（参见 BUG-UV-1）"。

**影响**：一个实验同时引入递归函数 + 调用栈 + 断点调试 + 闭包 upvalue 四个概念，认知负荷过载。

#### 痛点 3：Bug 狩猎难度过高

**证据**：10 道题严重性分布：

| 题目 | 严重性 | 涉及知识 |
|------|--------|---------|
| BUG-CP-1 | P1 | 常量池去重、Value::equals() 语义 |
| BUG-CP-2 | P1 | 嵌套索引赋值栈平衡、栈溢出崩溃 |
| BUG-UV-1 | P1 | 三层嵌套闭包、upvalue 惰性解析 |
| **BUG-IR-POP-2** | **P0** | IR 栈泄漏、循环内 if 单语句体未 POP |
| BUG-DEF-1 | P2 | 默认参数表达式三后端不一致 |
| BUG-DBG-1 | P1 | 调用栈帧 line 字段更新时机 |
| BUG-REGVM-2 | P2 | RegisterVM RETURN 指令步进回调丢失 |
| BUG-REPL-1 | P2 | 路径规范化缺失导致模块缓存不命中 |
| BUG-F-04 | P2 | Formatter ExportStmt 空行决策 |
| BUG-MOD-1 | P2 | Windows 驱动器相对路径安全 |

**影响**：没有一道"入门级"题目（如"这段代码输出什么？"）。最简单的 BUG-F-04 也需要理解 Formatter 解包逻辑。P0 级别的 BUG-IR-POP-2 需要理解整个 IR 编译流程才能定位。这不是"学习"，这是"劝退"。

#### 痛点 4：EBNF 文法对零基础者有门槛

**证据**：`SyntaxProductionLibrary.cpp` 每条产生式首屏展示纯 EBNF：

```ebnf
varDecl := "var" IDENTIFIER (":" typeAnnotation)? "=" expression ";"
        | typeAnnotation IDENTIFIER "=" expression ";"
funDecl := "fun" IDENTIFIER "(" params ")" (":" typeAnnotation)? block
params := (param ("," param)*)?
param  := IDENTIFIER (":" typeAnnotation)? ("=" literal)?
```

**影响**：不懂 EBNF 的人看到 `:=` `?` `*` `|` 这些元符号会懵。虽然有 `description` 字段的文字说明，但 EBNF 作为"首屏信息"具有压迫感。

#### 痛点 5：缺乏交互式「感受」环节

**证据**：所有实验的标准流程是"在编辑器中输入以下代码 → 打开 XXX 面板 → 观察 XXX → 验证 XXX"。

**影响**：被动验证式学习，不是主动探索式学习。没有"拖拽 Token 组装成语句"、"手写一条 OP_ADD 指令看看栈怎么变"这类动手操作。

#### 痛点 6：无学习路径引导

**证据**：8 个实验是平铺的 `QListWidget` 列表，无完成度标记、无前置依赖标注、无进度持久化、面板间无导航链接。

**影响**：不知道学哪了、下一步该学什么、关闭 IDE 后丢失所有进度感。

#### 痛点 7：实时动画子页存在但联动深度不足

**证据**：`MemoryModelPanel.h` 第 4 子页已有基础设施（`animStatusLabel_`/`animOpCodeLabel_`/`heapObjectTable_`/`animTimer_`），但：
- `heapObjectTable_` 是 QTableWidget 表格，不是图形化对象关系图
- refCount 只有数字，无曲线图或动画过渡
- GC mark-sweep 只有文字说明（`GcPhaseInfo`），无颜色标记动画演示

**影响**：抽象概念难以直觉化。

---

## 三、由浅入深的学习路径改进方案

> 以下每个功能均标注了：**目标用户 / 具体设计 / 实现方式 / 涉及文件**

---

### 🟢 阶段零：第一次接触（5~10 分钟）

#### 功能 1：Welcome 向导 / 首次启动导览

**目标**：让从没听过"编译原理"的人在 5 分钟内感受到"原来我写的代码是这样变成程序的"。

**具体设计**：

启动时弹出 Welcome 对话框：

```
┌─────────────────────────────────────────────────────┐
│  👋 欢迎使用 MiniLang IDE！                          │
│                                                     │
│  让我们用 3 分钟看看你的代码是如何运行的：             │
│                                                     │
│  [▶ 开始探索]                                       │
│                                                     │
│  ── 或选择你的起点 ──                              │
│  ○ 我完全新手，从头开始                             │
│  ○ 我懂一点编程，想了解编译原理                     │
│  ○ 我学过编译原理，想看高级功能                     │
└─────────────────────────────────────────────────────┘
```

点击「开始探索」进入 **3 步交互式导览**：

| 步骤 | 展示内容 | 交互 |
|------|---------|------|
| Step 1 | 左侧编辑器预填 `print("Hello!");`，右侧自动弹出 Token 表（3 个 token 高亮动画） | 点击任意 token，编辑器中对应字符高亮 |
| Step 2 | 切到 AST 视图，展示一棵极简的 3 节点树（Print → StringLiteral → "Hello!"） | 点击 AST 节点，Token 表和源码同步高亮 |
| Step 3 | 切到字节码视图，展示 2 条指令（OP_STRING "Hello!" / OP_PRINT），点击「运行」看到输出 | 点击「单步」按钮，当前指令高亮 + 输出区逐步出现结果 |

**现有基础**：
> ⚠️ 项目已有 `Ide::initWelcomePage()` 实现的启动页（`app/ide.cpp`），包含最近文件列表、新建/打开按钮和拖放支持。
> 本功能应**在现有 `welcomePage_` 基础上增强**，而非从零新建。具体做法：将现有 welcome page 改造为 `QStackedWidget`，
> 第一页为现有功能（最近文件/新建/打开），第二页为新增的 3 步交互导览。通过 `QSettings` 的 `welcome_completed` 标记决定首次启动显示哪一页。

**实现方式**：
- 重构现有 `welcomePage_`（`app/ide.cpp` 中的 `initWelcomePage()`）为 `QStackedWidget` 或新建 `WelcomeWizard` 类（继承 `QDialog`）
- 复用现有 `IdeController` API（compile / getTokenTable / getAst / disassemble）
- 使用 `QPropertyAnimation` 做高亮和过渡动画
- 不需要修改引擎层任何代码

**涉及文件**：
- 修改：`app/ide.cpp`（重构 `initWelcomePage()`）、`app/ide.h`（`welcomePage_` 类型调整）
- 新建：`gui/WelcomeWizard.h`, `gui/WelcomeWizard.cpp`（3 步交互导览组件）
- 修改：`app/IdeController.h`（确认 API 已满足需求，可能无需改动）

**验收标准**：
- [ ] 从零打开 IDE 能在 3 分钟内完成导览且不感到困惑
- [ ] 导览结束后能正确跳转到实验手册 lab-01
- [ ] 选择"我学过编译原理"可跳过导览直接进入主界面

---

#### 功能 2：「代码的生命旅程」一键动画

**目标**：用一个精心设计的 30 秒动画，建立直觉认知。

**具体设计**：在工具栏增加一个 ▶️ 「播放旅程」按钮（火箭图标 🚀），点击后按时间轴编排动画：

```
时间轴 0s:   编辑器中的 print(1 + 2 * 3);  全部高亮
时间轴 2s:   源码上方浮现箭头 ↓ "① 你写的代码"
时间轴 4s:   箭头指向 Token 表区域，token 逐个弹出动画
              print → ( → 1 → + → 2 → * → 3 → ) → ;
时间轴 8s:   浮现文字 "② 词法分析：拆成一个个 Token"
时间轴 10s:  箭头指向 AST 区域，树从根向下生长动画
              Print └── BinaryOp(+)
                       ├── Number(1)
                       └── BinaryOp(*)
                           ├── Number(2)
                           └── Number(3)
时间轴 14s:  浮现文字 "③ 语法分析：理解代码的结构（乘法优先！）"
              * 节点闪烁强调位置
时间轴 18s:  箭头指向字节码区域，指令逐行打出
              OP_INT 1, OP_INT 2, OP_INT 3, OP_MUL, OP_ADD, OP_PRINT
时间轴 22s: 浮现文字 "④ 编译：翻译成虚拟机能懂的指令"
时间轴 25s:  VM 栈动画（侧边小窗口）：push 1 → push 2 → push 3 → mul→6 → add→7
时间轴 28s: 输出区出现 "7"，浮动文字 "⑤ 执行：得到结果！"
时间轴 30s: 结束，显示 "想亲自试试？去实验手册开始吧！" + 按钮
```

**实现方式**：
- 使用 `QSequentialAnimationGroup` + `QPauseAnimation` 编排时间轴
- 各面板提供 `highlightForJourney(duration)` / `animateTreeGrowth()` 等临时高亮方法
- 动画结束后清理所有高亮状态
- 工具栏按钮使用 `QToolBar::addAction()` 添加

**涉及文件**：
- 新建：`gui/CodeJourneyAnimator.h`, `gui/CodeJourneyAnimator.cpp`
- 修改：`gui/MainWindow.cpp`（添加工具栏按钮）
- 修改：`gui/PipelineViewer.cpp`（添加 journey 高亮接口）
- 修改：`gui/AstViewer.cpp`（添加树生长动画接口）
- 修改：`gui/BytecodeTracePanel.cpp`（添加指令打出动画接口）

**验收标准**：
- [ ] 动画总时长 ≤ 35 秒
- [ ] 动画过程中可随时点击「跳过」终止
- [ ] 动画结束后所有面板恢复干净状态（无残留高亮）
- [ ] 强调"乘法优先"时 * 节点的视觉位置清晰可辨

> ⚠️ **维护成本警告**：30 秒动画与语言特性紧耦合——每次新增语法、调整字节码或修改 AST 结构都需要同步更新动画。
> 建议**降级为 P2**，或改用**静态信息图**（一张 SVG/HTML 图展示编译管线）替代动画，成本低且不易过时。
> 如果保留动画，建议做成可配置的（设置中可关闭），避免每次引擎变更都触发维护。

---

### 🟡 阶段一：感知期（1~2 小时）

#### 功能 3：交互式 Token 拼图游戏

**目标**：通过游戏化方式理解"词法分析就是切分字符流"。

**具体设计**：新建 `TokenPuzzlePanel`：

```
┌──────────────────────────────────────────────────┐
│  🔢 第 1 关 / 共 5 关          得分: ⭐⭐⭐☆☆     │
├──────────────────────────────────────────────────┤
│                                                  │
│  目标语句:  var x = 42;                          │
│                                                  │
│  下面是被打乱的 Token，点击按正确顺序排列：         │
│                                                  │
│  [ = ]  [ 42 ]  [ var ]  [ ; ]  [ x ]           │
│                                                  │
│  你的答案:  ___ ___ ___ ___ ___                  │
│                                                  │
│  [✓ 检查答案]  [💡 提示]  [⏭ 跳过]               │
│                                                  │
│  💡 提示: 以关键字 var 开头，以分号 ; 结尾        │
└──────────────────────────────────────────────────┘
```

**关卡设计**（由浅入深）：

| 关卡 | 目标语句 | 教学点 | 难度 |
|------|---------|--------|------|
| 1 | `var x = 42;` | 基本 5 个 token | ⭐ |
| 2 | `print("hello");` | 字符串是一个 token（不是 7 个） | ⭐ |
| 3 | `x > 0 and y < 10;` | 运算符优先级不影响 token 切分 | ⭐⭐ |
| 4 | `// 这是注释\nprint(1);` | 注释被分离出主流 | ⭐⭐ |
| 5 | `a[0] = b["key"] + 1;` | 复杂表达式（自选挑战） | ⭐⭐⭐ |

**实现方式**：
- 纯静态关卡数据（`TokenPuzzleLevel` 结构体数组）
- Qt 拖拽或点击排序逻辑（`QListWidget` + 自定义 item widget）
- 答案校验逻辑：比较用户序列与标准 Token 序列
- 得分系统：提示使用次数越少星级越高
- 不依赖引擎层（纯前端游戏）

**涉及文件**：
- 新建：`gui/TokenPuzzlePanel.h`, `gui/TokenPuzzlePanel.cpp`
- 新建：`gui/TokenPuzzleData.h`（关卡数据定义）
- 修改：`gui/ActivityBar.cpp`（添加入口按钮）

**验收标准**：
- [ ] 5 关全部可在 15 分钟内完成
- [ ] 每关失败后有明确反馈（哪些位置错了）
- [ ] 完成后在学习路径地图中标记为已完成

---

#### 功能 4：AST 节点搭建玩具

**目标**：通过拖拽组装理解"AST 是代码的结构化表示"。

**具体设计**：新建 `AstBuilderToyPanel`：

```
┌──────────────────────────────────────────────────┐
│  🌳 AST 搭建玩具                                 │
├──────────────────────────────────────────────────┤
│                                                  │
│  目标: 搭建 1 + 2 * 3 的 AST                    │
│                                                  │
│  左侧: 节点工具箱        右侧: 画布               │
│  ┌──────────┐           ┌──────────────────┐     │
│  │ 📄 Print  │           │                  │     │
│  │ 🔢 Number │           │    (拖到这里)     │     │
│  │ ➕ Add    │           │                  │     │
│  │ ➖ Sub    │           │                  │     │
│  │ ✖️ Mul    │           │                  │     │
│  │ ➗ Div    │           │                  │     │
│  │ 📦 VarDecl│           │                  │     │
│  └──────────┘           └──────────────────┘     │
│                                                  │
│  正确答案预览 (可折叠):                            │
│      BinaryOp(+)                                 │
│      ├── Number(1)                               │
│  └─── BinaryOp(*)                                │
│       ├── Number(2)                              │
│       └── Number(3)                              │
│                                                  │
│  [✓ 检查] [👁 查看标准答案] [➡ 下一题]            │
└──────────────────────────────────────────────────┘
```

**题目设计**：

| 题目 | 表达式 | 教学点 | 难度 |
|------|--------|--------|------|
| 1 | `42` | 最简：只有一个叶子节点 | ⭐ |
| 2 | `1 + 2` | 二元运算：根 + 两个叶子 | ⭐ |
| 3 | `1 + 2 * 3` | **优先级**：* 在 + 下面 | ⭐⭐ |
| 4 | `(1 + 2) * 3` | **括号改变结构**：+ 在 * 下面 | ⭐⭐ |
| 5 | `print(x)` | 函数调用树 | ⭐⭐ |
| 6 | `var x = 1 + 2;` | 完整语句（VarDecl 包含 initializer） | ⭐⭐⭐ |

**核心教学时刻**：第 3 题 vs 第 4 题——同样的数字和运算符，仅括号不同就导致完全不同的树结构。这个"顿悟瞬间"是整个玩具的核心价值。

**实现方式**：
- 使用 `QGraphicsScene` + `QGraphicsItem`（复用 `AstViewer` 的 QGraphicsScene 经验）
- 节点可拖拽、可连线、自动布局（简单层次布局算法即可）
- 答案校验：比对树的拓扑结构（不考虑绝对坐标）
- "查看标准答案"功能：自动将正确树渲染到画布上

**涉及文件**：
- 新建：`gui/AstBuilderToyPanel.h`, `gui/AstBuilderToyPanel.cpp`
- 新建：`gui/AstToyNodes.h`（节点图形项定义）
- 新建：`gui/AstToyLevels.h`（题目数据定义）

**验收标准**：
- [ ] 拖拽操作流畅（无明显卡顿）
- [ ] 第 3/4 题的树结构差异视觉上清晰可辨
- [ ] 自动布局不会让节点重叠

---

#### 功能 5：VM 栈沙盒

**目标**：通过亲手操作理解"栈式 VM 就是 push 和 pop"。

**具体设计**：新建 `VmStackSandboxPanel`：

```
┌──────────────────────────────────────────────────┐
│  📚 VM 栈沙盒                                    │
├──────────────────────────────────────────────────┤
│                                                  │
│  目标: 执行 1 + 2 * 3 的字节码，得到正确结果      │
│                                                  │
│  可用指令 (点击执行):                             │
│  [OP_INT 1]  [OP_INT 2]  [OP_INT 3]             │
│  [OP_ADD]    [OP_MUL]    [OP_PRINT]              │
│  [↩ 撤销]    [🔄 重置]                            │
│                                                  │
│  操作数栈 (可视化):                                │
│  ┌───────┐                                      │
│  │       │ ← 栈顶 (top)                          │
│  │       │                                       │
│  │       │                                       │
│  └───────┘                                      │
│                                                  │
│  已执行指令序列:                                  │
│  (空)                                            │
│                                                  │
│  输出: ___                                       │
│  💡 提示: 先压入操作数，再执行运算！              │
└──────────────────────────────────────────────────┘
```

**交互流程**（以正确顺序为例）：

1. 点击 `[OP_INT 1]` → 栈中出现 `1`（带下落动画）
2. 点击 `[OP_INT 2]` → 栈中变为 `1, 2`
3. 点击 `[OP_INT 3]` → 栈中变为 `1, 2, 3`
4. 点击 `[OP_MUL]` → 栈顶两个弹出（动画），计算 `2*3=6`，结果 `6` 压回
5. 栈中变为 `1, 6`
6. 点击 `[OP_ADD]` → 弹出 `1, 6`，计算 `7`，压回
7. 点击 `[OP_PRINT]` → 输出 `7` ✅

**错误反馈示例**：
- 用户先点 `OP_ADD` → 提示："❌ OP_ADD 需要栈顶有两个值！当前栈只有 1 个值。"
- 用户最后没点 `OP_PRINT` → 提示："💡 你已经算出了结果，记得用 OP_PRINT 输出它！"

**实现方式**：
- 内置小型栈状态机模拟（`std::vector<Value>` + 操作历史列表）
- `QListWidget` 显示栈内容 + `QPropertyAnimation` 做弹出动效
- 不连接真实 VM（纯前端模拟，降低复杂度）
- 预设多组题目（类似 Token 拼图的关卡制）

**沙盒关卡设计**：

| 关卡 | 目标 | 可用指令 | 教学点 |
|------|------|---------|--------|
| 1 | 计算 `1 + 2` | INT, ADD, PRINT | 最基本的 push-pop |
| 2 | 计算 `1 + 2 * 3` | INT, ADD, MUL, PRINT | 运算顺序由指令决定 |
| 3 | 计算 `(1 + 2) * 3` | 同上 | 同样的数不同的指令顺序 |
| 4 | 打印 `"hello"` | STRING, PRINT | 字符串也是值 |
| 5 | 自由模式 | 全部指令 | 自己探索 |

**涉及文件**：
- 新建：`gui/VmStackSandboxPanel.h`, `gui/VmStackSandboxPanel.cpp`
- 新建：`gui/SandboxLevels.h`（关卡数据和校验逻辑）

**验收标准**：
- [ ] 栈的 push/pop 动画直观易懂（≤ 1 秒感知延迟）
- [ ] 错误反馈信息准确指出问题原因
- [ ] 撤销功能可回退任意步骤

---

### 🔵 阶段二：理解期（5~10 小时）

#### 功能 6：学习路径地图（Learning Path Map）

**目标**：解决"不知道学哪了、下一步学什么"的问题——中央导航枢纽。

**具体设计**：新建 `LearningPathPanel`：

```
┌──────────────────────────────────────────────────────┐
│  🗺️ 学习路径                        总进度: ████░░ 40% │
├──────────────────────────────────────────────────────┤
│                                                      │
│  ┌─ 🟢 阶段零：首次接触 ────────────── ✅ 100%        │
│  │  ☑ Welcome 导览                                   │
│  │  ☑ 代码生命旅程动画                               │
│  │  ☑ Token 拼图 (5/5 关卡)                           │
│  │  ☑ AST 搭建玩具 (6/6 题)                           │
│  │  ☑ VM 栈沙盒                                     │
│  └──────────────────────────────────────────────────┤
│                                                      │
│  ├─ 🟡 阶段一：编译前端 ──────────── 🔄 60%           │
│  │  ☑ 实验 1: 词法分析                                │
│  │  ☐ 实验 2: 递归下降解析  ← 当前推荐               │
│  │  ☐ 语法探索器: if/while/for                       │
│  │  ☐ 挑战: 自己写一个运算符优先级链                   │
│  └──────────────────────────────────────────────────┤
│                                                      │
│  ├─ 🔵 阶段二：执行引擎 ──────────── 🔒 未解锁         │
│  │  🔒 实验 3: 树遍历解释器  (需完成阶段一 80%)        │
│  │  🔒 实验 4: 栈式字节码 VM                          │
│  │  🔒 实验 5: 寄存器式 VM + IR                       │
│  │  🔒 三后端对比面板                                 │
│  └──────────────────────────────────────────────────┤
│                                                      │
│  ├─ 🟣 阶段三：深入理解 ──────────── 🔒 未解锁         │
│  │  🔒 实验 6: 三后端一致性                            │
│  │  🔒 实验 7: 内存模型                               │
│  │  🔒 IR 变换过程面板                                │
│  │  🔒 性能剖析仪表盘                                 │
│  └──────────────────────────────────────────────────┤
│                                                      │
│  └─ 🔴 阶段四：实战训练 ──────────── 🔒 未解锁         │
│     🔒 Bug 狩猎: 入门级 (3题)                         │
│     🔒 Bug 狩猎: 进阶级 (4题)                         │
│     🔒 Bug 狩猎: 专家级 (3题)                         │
│     🔒 自由项目: 实现一个新特性                        │
└──────────────────────────────────────────────────────┘
```

**关键设计决策**：

| 决策点 | 选择 | 理由 |
|--------|------|------|
| 解锁机制 | 基于关键节点解锁（如完成 lab-02 后解锁 lab-03） | 与 `prerequisites` 字段一致，避免百分比阈值的模糊性 |
| 当前推荐算法 | 基于前置依赖 + 难度平滑 + 最近活动时间 | 动态推荐最合适的下一步 |
| 进度持久化 | JSON 文件 `~/.minilang_progress.json` | 重启不丢失，可手动备份 |
| 活动多样性 | 实验/拼图/玩具/挑战/自由项目混合 | 避免单调疲劳 |
| 视觉风格 | 进度条 + 图标 + 颜色编码（绿/黄/蓝/紫/红） | 一眼看清全局状态 |

**数据结构设计**：

```cpp
struct LearningActivity {
    std::string id;            // 唯一标识
    std::string title;         // 显示名称
    std::string description;   // 简短描述
    int         stage;         // 所属阶段 (0-4)
    std::vector<std::string> prerequisites; // 前置活动 ID 列表
    int         estimatedMinutes; // 预计耗时
    ActivityType type;         // LAB / PUZZLE / TOY / SANDBOX / CHALLENGE / FREEFORM
};

struct LearnerProgress {
    std::map<std::string, bool> completed;     // 已完成的活动
    std::map<std::string, int>  attemptCount;  // 尝试次数
    std::map<std::string, int64_t> lastAccessTime; // 最后访问时间
    int currentStage = 0;
};
```

**实现方式**：
- `LearningPathData` 静态数据定义完整的依赖图
- `QSettings` 或 JSON 文件持久化进度
- 各面板完成后发送信号给 `LearningPathPanel` 更新状态
- `LearningPathPanel` 提供 `getNextRecommended()` 方法

**涉及文件**：
- 新建：`gui/LearningPathPanel.h`, `gui/LearningPathPanel.cpp`
- 新建：`gui/LearningPathData.h`（活动和依赖数据定义）
- 新建：`gui/LearnerProgress.h`（进度持久化逻辑）
- 修改：所有教学面板（完成后 emit `activityCompleted(id)` 信号）

**验收标准**：
- [ ] 首次打开显示阶段零已解锁，其余锁定
- [ ] 完成阶段零的关键活动（Welcome 导览 + 至少 2 个游戏化面板）后自动解锁阶段一
- [ ] 关闭 IDE 重启后进度保持不变
- [ ] "当前推荐"标签始终指向最适合的下一个活动

---

#### 功能 7：实验手册分层重写 —— 增加「迷你实验」

**目标**：在每个正式实验之前加一个 **5 分钟能完成的迷你实验**作为缓冲，降低认知门槛。

**改造模板**（以 lab-03 为例）：

```markdown
## 实验 3：树遍历解释器

### 🏃 迷你实验（5 分钟）
**目标**：感受"解释器就是遍历 AST 并执行"

**步骤**：
1. 在编辑器中输入: `print(42);`
2. 点击运行 → 看到 `42`
3. 打开 AST 面板 → 看到 `Print(StringLiteral("42"))`
4. 改成 `print(3 + 4);` → 看 AST 变成 `Print(BinaryOp(+, 3, 4))`
5. 运行 → 看到 `7`

💡 这就是解释器的全部工作：遍历 AST，遇到 Print 就打印，
   遇到 BinaryOp 就计算！

✅ **迷你实验验证**：你看到了 7 吗？

---
### 📖 正式实验（原内容不变）
## 目标
理解解释器的执行模型：Visitor 模式递归遍历 AST 节点求值。
...（原有内容：fib + 断点调试 + 闭包 upvalue）
```

**全部 8 个实验的迷你版设计**：

| 实验 | 迷你实验内容 | 时间 | 核心感受 |
|------|-------------|------|---------|
| lab-01 词法 | 输入 `var x = 1;` 看 Token 表，数几个 token | 3 min | "源码被切成了有类型的片段" |
| lab-02 解析 | 输入 `1+2*3` vs `(1+2)*3` 对比两棵 AST | 5 min | "括号改变树的结构" |
| lab-03 解释器 | 输入 `print(42)` 然后 `print(3+4)` 理解遍历求值 | 5 min | "解释器 = 递归遍历 + 执行" |
| lab-04 栈VM | 输入 `1+2` 看字节码 3 条指令，手动单步 | 5 min | "每条指令操作栈顶" |
| lab-05 寄存器VM | 同上代码切换 RegisterVM，对比两种字节码格式 | 5 min | "不同后端的指令集不同" |
| lab-06 一致性 | 用 `print(7/2)` 三后端对比，看三个 OK | 3 min | "三条路径结果一致" |
| lab-07 内存 | 执行 COW 样例代码，观察 a/b 输出不同 | 5 min | "写时复制真正复制了" |
| lab-08 Bug狩猎 | 只读一道 P2 级别简单题（BUG-F-04 Formatter） | 5 min | "Bug 隐藏在细节中" |

**实现方式**：
- 仅修改 `LabManualContent.cpp` 中的 markdown 内容
- 每个 LabChapter 开头插入迷你实验段落（用 `---` 分隔符区分）
- 不需要改代码结构或 GUI 组件

**涉及文件**：
- 修改：`gui/LabManualContent.cpp`（仅内容追加）

**验收标准**：
- [ ] 每个迷你实验都能在 5 分钟内完成
- [ ] 迷你实验与正式实验之间有清晰的分隔线
- [ ] 迷你实验本身包含验证断言（类似正式实验的验证部分）

---

#### 功能 8：语法探索器增加「自然语言模式」

**目标**：降低 EBNF 门槛，让不懂形式语言的人也能理解文法。

**具体设计**：在 `SyntaxExplorerPanel` 增加视图切换按钮：

```
[EBNF 视图] | [自然语言视图] | [混合视图]
```

三种视图的内容对照（以 `if-stmt` 为例）：

**EBNF 视图**（原有，不变）：
```ebnf
ifStmt := "if" "(" expr ")" block ("else" block)?
```

**自然语言视图**（新增）：
```
if 语句的写法：

  必须以 if 关键字开头
  接着是括号包裹的条件表达式: if (条件)
  然后是代码块（用 { } 包围的一或多条语句）
  可以选择性地跟一个 else 和另一个代码块

  示例:
    ✓ if (x > 0) { print(x); }
    ✓ if (x > 0) { print(x); } else { print("non-positive"); }
    ✗ if x > 0 { print(x); }        ← 缺少括号!
    ✗ if (x > 0) print(x);          ← 缺少花括号! (MiniLang 要求)

  注意事项:
    • 条件必须用圆括号 ( ) 包裹
    • 代码块必须用花括号 { } 包裹
    • else 部分是可选的
```

**混合视图**（新增）：
```ebnf
ifStmt := "if" "(" expr ")" block ("else" block)?

═══════════════════════════════════════
人话翻译:

  if (条件) {
      // 条件为真时执行的代码
  } [else {
      // 否则时执行的代码（可选）
  }]
═══════════════════════════════════════
```

**实现方式**：
- 为每条产生式新增 `naturalLanguageDescription` 字段
- `SyntaxExplorerPanel` 内部增加 `QStackedWidget` 切换三种视图
- 自然语言内容硬编码在 `SyntaxProductionLibrary.cpp` 中（静态数据）

**涉及文件**：
- 修改：`gui/SyntaxProductionLibrary.cpp`（为每条产生式增加自然语言描述）
- 修改：`gui/SyntaxExplorerPanel.h/cpp`（增加视图切换 UI）

**验收标准**：
- [ ] 11 条产生式都有完整的自然语言描述
- [ ] 三种视图之间切换流畅（无闪烁）
- [ ] 默认显示"混合视图"（兼顾两者）

---

### 🟣 阶段三：实战期（10+ 小时）

#### 功能 9：Bug 狩猎分级题库重构

**目标**：将现有的 10 道题重新分级，补充入门级题目。

**新的分级体系**：

##### 🟢 入门级（阅读理解型，3~5 道）

面向刚完成阶段二的学习者，重点是**培养 Debug 直觉**，不需要定位代码。

| 题目 ID | 类型 | 描述 | 示例 |
|---------|------|------|------|
| BUG-READ-01 | 预测输出 | 给出代码，问输出是什么 | `print(7/2)` 在三后端分别输出什么？ |
| BUG-READ-02 | 定位异常行 | 给出代码+报错信息，问哪一行出错 | `Undefined variable 'x'` 出现在哪个作用域？ |
| BUG-READ-03 | 格式化预测 | 给出代码，问 Formatter 输出 | `BUG-F-04` 改为选择题形式 |
| BUG-READ-04 | 性能直觉 | 给出两段代码，问哪个更快 | 循环内 vs 循环外声明变量 |
| BUG-READ-05 | 内存推理 | 给出代码，问 refCount 变化过程 | `a = [1,2,3]; b = a;` 后各对象 refCount? |

##### 🟡 进阶级（当前 P2 级别，4 道）

需要阅读代码、理解逻辑、定位问题区域：

| 题目 ID | 原题 | 调整 |
|---------|------|------|
| BUG-DEF-1 | 默认参数不一致 | 保持不变 |
| BUG-REGVM-2 | RegisterVM RETURN 回调丢失 | 保持不变 |
| BUG-REPL-1 | 路径规范化缺失 | 保持不变 |
| BUG-F-04 | Formatter 空行决策 | 从入门级移上来，增加代码定位要求 |

##### 🔴 专家级（当前 P0/P1 级别，3 道）

需要深入理解引擎架构、跨模块追踪：

| 题目 ID | 原题 | 调整 |
|---------|------|------|
| BUG-IR-POP-2 | IR 栈泄漏 | 保持不变 |
| BUG-CP-1 | 常量池去重 | 保持不变 |
| BUG-UV-1 | 闭包 upvalue | 保持不变 |

**实现方式**：
- 修改 `BugHuntLibrary.cpp` 中的数据结构，增加 `difficulty` 字段（BEGINNER / INTERMEDIATE / EXPERT）
- 修改 `BugHuntPanel.h/cpp`，增加难度筛选 Tab 或下拉框
- 新增 5 道入门级题目的数据（`description`/`sourceCode`/`expectedBehavior`/`bugBehavior`/`hints`/`rootCause`）

**涉及文件**：
- 修改：`gui/BugHuntLibrary.cpp`（增加入门级题目 + difficulty 字段）
- 修改：`gui/BugHuntPanel.h/cpp`（增加难度筛选 UI）

**验收标准**：
- [ ] 入门级题目平均完成时间 < 10 分钟
- [ ] 难度筛选 UI 清晰（三个 Tab 或下拉框）
- [ ] 学习路径地图中 Bug 狩猎分为三个子项

---

#### 功能 10：内存模型实时动画增强

**目标**：将已有的第 4 子页基础设施升级为真正的可视化动画。

**现状**（`MemoryModelPanel.h` 已有）：
- `heapObjectTable_`: QTableWidget（表格形式显示堆对象）
- `animTimer_`: 500ms 自动刷新定时器
- `refreshAnimState()`: 刷新状态方法
- `animOpCodeLabel_`/`animStackDepthLabel_`/`animFrameCountLabel_`/`animGcTrackedLabel_`: 数值标签

**增强方案**：

##### 10a. 堆对象关系图（替代/补充表格）

```
┌─────────────────────────────────────────────┐
│  堆对象关系图                                │
│                                             │
│  ┌──────────┐     ┌──────────┐             │
│  │ Array #1 │←────│ Array #2 │             │
│  │ [1,2,3]  │ copy│ [1,2,3] │             │
│  │ ref=2    │────→│ ref=1    │             │
│  └──────────┘     └──────────┘             │
│       ↑                ↑                   │
│       │ a               │ b                 │
│                                             │
│  💡 a 和 b 共享同一个 ArrayData (COW 前)    │
│  🔴 执行 b[0] = 99 后触发 COW detach       │
└─────────────────────────────────────────────┘
```

- 使用 `QGraphicsScene` 绘制对象框 + 引用连线
- COW detach 时播放"分裂"动画（一个对象变成两个）
- 引用计数变化时数字带颜色脉冲（增加=绿色，减少=红色）

##### 10b. refCount 曲线图

- 随 VM 单步执行绘制折线图（横轴=时间/步数，纵轴=refCount）
- 支持选择跟踪特定对象的曲线
- 使用 `QPainter` 自绘（轻量，不需引入第三方图表库）

##### 10c. GC mark-sweep 颜色动画

| 阶段 | 颜色 | 动画效果 |
|------|------|---------|
| Idle | 白色 | 所有对象正常显示 |
| Mark | 灰色渐变 | 从 root 开始，被引用的对象逐个变灰 |
| Sweep | 红色闪烁 | 不可达对象变红然后缩小消失 |
| Reset | 白色 | 剩余对象恢复白色 |

**前置可行性调研（必须在实施前完成）**：
> ⚠️ `IdeController` 当前暴露了 `getVmStack()`、`getVmGlobals()` 等 API，但**没有暴露堆对象引用关系图**。
> NaN-boxing 的 Value 是 8 字节内联的，要提取堆对象引用关系需要遍历整个 Value 空间并检查 `RefCounted*` 指针，
> 这涉及引擎层内部数据结构（`NaNBox.h`、`RefCounted.h`）。
> **调研任务**：确认是否能在不破坏 NaN-boxing 封装的前提下，通过 `IdeController` 暴露一个 `getHeapObjectGraph()` API。
> 如果不可行，功能 10a（堆对象关系图）需要降级为“预设脚本演示”（硬编码几个典型场景的动画，不连接真实 VM 状态）。

**实现方式**：
- 新建 `HeapObjectGraphWidget`（继承 `QWidget`，使用 `QPainter` 自绘或 `QGraphicsScene`）
- 新建 `RefCountChartWidget`（继承 `QWidget`，`QPainter` 绘制折线图）
- 修改 `MemoryModelPanel.cpp` 的 `buildAnimPage()` 将表格替换/补充为图形组件
- 通过 `app/IdeController.h` API 获取 VM 运行时堆对象信息（依赖前置调研结果）

**涉及文件**：
- 新建：`gui/HeapObjectGraphWidget.h`, `gui/HeapObjectGraphWidget.cpp`
- 新建：`gui/RefCountChartWidget.h`, `gui/RefCountChartWidget.cpp`
- 修改：`gui/MemoryModelPanel.cpp`（替换/补充第 4 子页内容）
- 可能修改：`app/IdeController.h`（如需新增堆对象查询 API）

**验收标准**：
- [ ] COW detach 时"分裂"动画在 1 秒内完成且清晰可见
- [ ] GC mark-sweep 四个阶段的颜色变化符合上述规范
- [ ] 同时运行多个动画时不出现画面撕裂

---

#### 功能 11：REPL `%magic` 命令

**目标**：在 REPL 交互场景中快速调用已有面板能力，增强探索体验。

**命令设计**：

```minilang
> %help                     -- 列出所有 magic 命令
> %disassemble              -- 显示最近执行的字节码（同 BytecodeTracePanel）
> %ir                       -- 显示当前输入/最近执行的 IR（同 IrViewer）
> %compare                  -- 运行三后端对比（同 BackendComparePanel）
> %profile                  -- 显示指令计数热点（同 ProfileDashboardPanel）
> %memory                   -- 显示当前堆对象统计（同 MemoryModelPanel 第4子页）
> %ast                      -- 显示最近执行的 AST（同 AstViewer）
> %tokens                   -- 显示最近的 Token 表（同 PipelineViewer Step 1）
> %reset                    -- 重置 VM 状态（清除所有变量/函数）
> %version                  -- 显示 MiniLang 版本信息
```

**实现方式**：
- 在 REPL 输入处理逻辑中检测 `%` 前缀
- 每个 magic 命令是一个独立的 handler 函数
- 复用各面板的数据获取逻辑（通过 `IdeController`）
- 输出格式化为美化文本（使用 ANSI 颜色码或纯文本表格）

**现有基础**：
> `gui/ReplPanel.cpp` 的 `executeLine()` 已实现 `help`、`clear`、`reload "mod"`、`reload all` 等特殊命令的分发逻辑。
> 新增 `%magic` 命令应在同一分发点扩展，检测 `%` 前缀后路由到对应的 handler 函数。

**涉及文件**：
- 修改：`gui/ReplPanel.cpp`（在 `executeLine()` 中扩展 `%` 前缀命令分发）
- 新建：`gui/MagicCommands.h`, `gui/MagicCommands.cpp`（各 magic 命令的 handler 实现）
- 修改：`app/IdeController.h`（确认各面板数据获取 API 已满足需求）

**验收标准**：
- [ ] `%help` 列出所有命令及简短描述
- [ ] `%disassemble` 输出与 BytecodeTracePanel 信息一致
- [ ] 非 `%` 开头的普通输入不受影响

---

#### 功能 12：错误信息友好化增强

**目标**：将编译器/运行时错误信息从“能看懂”提升到“能引导初学者自救”。

**具体设计**：

| 错误类型 | 当前输出 | 增强后输出 |
|---------|---------|----------|
| 未定义变量 | `Undefined variable 'x' at line 3` | `Undefined variable 'x' at line 3, column 12. Did you mean 'y'?` |
| 缺少分号 | `Expected ';' at line 5` | `Expected ';' at line 5, column 20. MiniLang 每条语句需要以分号结尾。` |
| 类型不匹配 | `Type error: expected int, got string` | `Type error at line 8: 不能将 string 用于 int 的运算。提示：用 toInt() 转换。` |
| 括号不匹配 | `Expected ')' at line 2` | `Expected ')' at line 2, column 15. 你打开了 '(' 但忘记关闭。` |

**增强策略**：
- **拼写建议**：对 `Undefined variable` 错误，用编辑距离算法在当前作用域的变量名中查找相似名称（阈值 ≤ 2）
- **常见错误模式匹配**：维护一个 `ErrorPattern → Hint` 映射表
- **错误分类链接**：每类错误关联到对应的实验手册章节

**涉及文件**：
- 新建：`gui/ErrorHintEngine.h`, `gui/ErrorHintEngine.cpp`
- 修改：`interpreter/Interpreter.cpp`、`compiler/Compiler.cpp`（错误报告点增加 hint hook）
- 修改：`gui/ReplPanel.cpp`（展示增强后的错误信息）

**验收标准**：
- [ ] 未定义变量错误有拼写建议（当存在相似变量名时）
- [ ] 常见错误类型有教学性提示
- [ ] 错误提示不影响正常编译/运行性能

---

#### 功能 13：代码模板 / Snippets 系统

**目标**：为初学者提供“脚手架”，降低从零写代码的门槛。

**具体设计**：在编辑器中支持关键字触发自动展开代码模板：

| 触发关键字 | 展开模板 |
|-----------|--------|
| `fun` | `fun 函数名(参数列表) : 返回类型 {\n    // 函数体\n}` |
| `class` | `class 类名 {\n    var 属性 = 默认值;\n    fun 方法名() {\n    }\n}` |
| `for` | `for (var i = 0; i < 10; i = i + 1) {\n    // 循环体\n}` |
| `if` | `if (条件) {\n    // 条件为真\n} else {\n    // 否则\n}` |
| `while` | `while (条件) {\n    // 循环体\n}` |
| `try` | `try {\n    // 可能出错\n} catch (e) {\n    // 错误处理\n}` |
| `print` | `print(表达式);` |

**交互方式**：
- 输入关键字后按 `Tab` 键展开模板
- 展开后第一个占位符高亮，`Tab` 键在占位符之间跳转
- 右键菜单或 `Ctrl+T` 打开模板列表

**涉及文件**：
- 新建：`gui/CodeSnippetEngine.h`, `gui/CodeSnippetEngine.cpp`
- 修改：`gui/CodeEditor.cpp`（Tab 键检测 + snippet 集成）

**验收标准**：
- [x] 7 个基础模板可正常展开
- [x] Tab 键在占位符之间正确跳转
- [x] 不影响正常的 Tab 缩进功能

---

## 四、预期效果对比

| 维度 | 当前 | 改进后 | 提升 |
|------|------|--------|------|
| **首次启动留存率** | 低（新手直接懵） | 高（3分钟建立直觉） | **+++** |
| **lab-03 完成率** | 中（概念过载放弃） | 高（迷你实验缓冲） | **++** |
| **Bug 狩猎参与度** | 低（太难） | 中（分级后可逐步挑战） | **++** |
| **平均学习时长** | ~3小时（碎片化） | ~15小时（有路径引导） | **+++++** |
| **核心概念掌握度** | ~40%（被动验证） | ~75%（主动构建+游戏化） | **+++** |
| **EBNF 理解门槛** | 高（需前置知识） | 低（自然语言翻译） | **++** |
| **学习路径清晰度** | 无（平铺列表） | 高（4阶段地图+进度） | **+++++** |
| **内存模型直觉** | 中（静态图解） | 高（实时动画联动） | **++** |
| **错误信息友好度** | 低（纯技术描述） | 高（教学性提示+拼写建议） | **+++** |
| **代码编写门槛** | 高（从零开始） | 低（模板脚手架） | **++** |

---

## 五、实施优先级与工作量估算

### Top 5 优先级排序

| 优先级 | 功能 | 投入产出比 | 工作量估计 | 理由 |
|--------|------|-----------|-----------|------|
| **P0** | Welcome 向导（含现有 welcomePage_ 重构） | ⭐⭐⭐⭐⭐ | 4-5 天 | 解决"第一眼劝退"问题，需整合已有启动页 |
| **P0.5** | ActivityBar 注册制重构（前置任务） | ⭐⭐⭐⭐ | 1-2 天 | 所有新面板入口的基础设施，必须先完成 |
| **P1** | 学习路径地图 | ⭐⭐⭐⭐⭐ | 3-4 天 | 解决"不知道学哪了"的核心痛点，串联所有其他功能 |
| **P2** | 实验手册分层（迷你实验） | ⭐⭐⭐⭐ | 1 天 | 只需改 Markdown 内容，无需新代码，性价比极高 |
| **P2.5** | 错误信息友好化增强 | ⭐⭐⭐⭐ | 2-3 天 | 直接影响初学者日常体验，ROI 极高 |
| **P3** | Token 拼图 + AST 玩具 + VM 栈沙盒 | ⭐⭐⭐⭐ | 7-9 天 | 游戏化大幅提升参与度，AST 玩具的 QGraphicsScene 拖拽较复杂 |
| **P3.5** | 代码模板 / Snippets | ⭐⭐⭐⭐ | 2-3 天 | 直接降低编写门槛，与游戏化面板互补 |
| **P4** | Bug 狩猎分级 + 内存动画增强 | ⭐⭐⭐ | 5-7 天 | 深化已有资源，内存动画依赖前置可行性调研 |
| **P5** | 代码生命旅程动画（降级） | ⭐⭐ | 2-3 天 | 维护成本高，建议用静态信息图替代 |

### 推荐实施路线图

```
Week 1:  P0.5 (ActivityBar重构) + P0 (Welcome向导) + P2 (迷你实验)  → 基础设施 + 解决“入门劝退”
Week 2:  P1 (学习路径地图) + P2.5 (错误信息增强)                     → 解决“迷失方向” + 日常体验
Week 3:  P3 (Token拼图 + AST玩具) + P3.5 (代码模板)                  → 增加“趣味性” + 降低编写门槛
Week 4:  P3续 (VM栈沙箱) + P4 (Bug狩猎分级)                          → 进入“深度学习”支持
Week 5+:  P4续 (内存动画，依赖前置调研) + 功能11 (REPL magic) + P5 (生命旅程动画/信息图) → 锦上添花

**总工作量估算**：22-28 天（原估 15-19 天偏乐观，已根据实际复杂度上调）
```

### 每个功能的 AI Agent 实施注意事项

1. **Welcome 向导**：注意 Qt 窗口层级管理（向导必须在 MainWindow 之上模态显示）；动画速度要考虑不同机器性能（提供设置选项）。**注意**：项目已有 `Ide::initWelcomePage()` 启动页，需在其基础上增强而非从零新建
2. **学习路径地图**：JSON 进度文件要做好兼容性处理（版本升级时旧格式不能崩溃）
3. **Token 拼图 / AST 玩具**：拖拽操作在不同平台（Windows/macOS/Linux）行为可能不同，需充分测试
4. **内存模型动画**：如果 `IdeController` 缺少堆对象查询 API，需要先最小化扩展该接口（遵循 Facade 模式）。**前置调研**：确认 NaN-boxing Value 的堆对象引用提取可行性
5. **所有新面板**：遵循现有架构约定（Facade 模式消费 API、QTimer 轮询避免耦合、不修改引擎层）
6. **★ ActivityBar 注册制重构（前置任务）**：当前 `ActivityBar` 的 `onActivityChanged(int index)` 是硬编码的 index 分发（index 0 = `fileTreeDock_`，index 1 = `debugPanelDock_`）。新增 5+ 个面板后 if-else 链会失控。**建议**：在添加任何新面板之前，先将 `ActivityBar` 重构为注册制——`registerPanel(id, icon, widget)` + 信号驱动的切换逻辑。这是所有新面板入口的基础设施
7. **★ i18n 要求**：所有新增教学文本（迷你实验、自然语言描述、Bug 狩猎入门题、magic 命令输出）必须用 `mlTr()` 包裹。项目已有 ~372 处未包裹的待清理文本，新增内容不应再增加这个债务
8. **★ REPL 命令分发**：`ReplPanel.cpp` 的 `executeLine()` 已有特殊命令处理逻辑，`%magic` 命令应在同一点扩展

---

## 六、附录：关键源码参考索引

### 核心引擎层

| 文件 | 内容 | 改进方案关联 |
|------|------|-------------|
| `compiler/IR.h` | 52 个 IROp 定义 + IRContext | 功能 2（旅程动画）、功能 8（IR 变换） |
| `compiler/VM.h` | 栈式 VM (~60 OpCode) | 功能 5（栈沙盒）、功能 11（REPL magic） |
| `compiler/RegisterVM.h` | 寄存器式 VM (32 regs, ~50 RegOp) | 功能 5（栈沙盒对比）、功能 9（Bug狩猎） |
| `interpreter/Value.h` | NaN-boxing Value 类型 | 功能 10（内存动画） |
| `interpreter/NaNBox.h` | NaN-boxing 编码实现 | 功能 10（内存动画） |

### GUI 面板层

| 文件 | 内容 | 改进方案关联 |
|------|------|-------------|
| `gui/LabManualContent.cpp` | 8 个实验手册全文 | **功能 7（迷你实验）— 直接修改此文件** |
| `gui/SyntaxProductionLibrary.cpp` | 10 条语法产生式 | **功能 8（自然语言模式）— 直接修改此文件** |
| `gui/BugHuntLibrary.cpp` | 10 道 Bug 狩猎题 | **功能 9（分级重构）— 直接修改此文件** |
| `gui/BugHuntPanel.h/cpp` | Bug 狩猎面板 UI | 功能 9（难度筛选 UI） |
| `gui/MemoryModelPanel.h/cpp` | 内存模型 4 子页 | **功能 10（动画增强）— 主要修改目标** |
| `gui/PipelineViewer.h/cpp` | 编译管线 5 步导航 | 功能 1（Welcome 导览）、功能 2（旅程动画） |
| `gui/BackendComparePanel.h/cpp` | 三后端对比面板 | 功能 1（Welcome 导览）、功能 11（REPL magic） |
| `gui/AstViewer.h/cpp` | AST 树可视化 | 功能 2（旅程动画）、功能 4（AST 玩具参考） |
| `gui/BytecodeTracePanel.h/cpp` | 字节码执行轨迹 | 功能 2（旅程动画）、功能 5（栈沙盒参考） |
| `gui/ReplPanel.h/cpp` | REPL 交互面板 | **功能 11（%magic 命令）— 主要修改目标** |
| `gui/ActivityBar.h/cpp` | 左侧活动栏 | 所有新面板的入口注册（★ 需重构为注册制） |
| `gui/CodeEditor.h/cpp` | 代码编辑器 | 功能 13（代码模板）— Tab 键触发集成 |
| `app/IdeController.h/cpp` | Facade 控制器 | 可能需要最小扩展（堆对象查询 API） |

### 项目配置

| 文件 | 内容 | 备注 |
|------|------|------|
| `AGENTS.md` | AI Agent 开发指南 | **必读** — 包含架构约束、Bug 模式、开发规范 |
| `README.md` | 项目说明文档 | 548 行，含完整特性列表和截图计划 |
| `CMakeLists.txt` | 构建配置 | 新文件需在此注册 |

---

> **文档版本**：v1.1（审校修订版）  
> **生成日期**：2026-07-06  
> **适用范围**：AI Agent 逐项实施的功能规格说明  
> **使用建议**：按 P0.5 → P0 → P1 → P2 → P2.5 → P3 → P3.5 → P4 → P5 顺序逐项实施，每完成一项更新本文档对应部分的"验收标准"勾选状态
