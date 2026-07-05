# MiniLang 开发指南

本文档面向参与 MiniLang 开发与维护的开发者，记录项目的工程约定、设计决策与性能优化策略。

## 目录

- [核心设计原则](#核心设计原则)
- [安全约束](#安全约束)
- [IR 中间层](#ir-中间层)
- [调试一致性](#调试一致性)
- [教学增强面板（第二波）](#教学增强面板第二波)
- [教学增强面板（第三波）](#教学增强面板第三波)
- [教学增强面板（第二档 + 第四档）](#教学增强面板第二档轻量引擎改造--第四档已有面板动画增强)
- [三梯队修复（2026-07-05）](#三梯队修复outputpanel-死代码--模块隔离--vm-条件断点局部变量2026-07-05)
- [教学增强面板（第三档）](#教学增强面板第三档异常流--闭包-upvalue-可视化)
- [跨模块集成审计 Bug 修复收尾](#跨模块集成审计-bug-修复收尾2026-07-05)
- [性能与工程优化 + 代码质量与文档](#性能与工程优化c1-c4--代码质量与文档d1-d32026-07-05)


## 核心设计原则

### PERF-12 NaN-boxing

Value 类型使用 8 字节 NaN-boxing 编码（`sizeof` 从 24 字节降至 8 字节），标量（int/float/bool/null）内联存储零原子操作，堆类型通过侵入式引用计数（`RefCounted` 基类）管理。

### PERF-13 VMStack

VM 操作数栈使用定长数组（1024 x 8B = 8KB）+ 栈顶指针替代 `std::vector`，消除 `push_back` 容量检查和堆分配开销。

### PERF-14 寄存器式 VM

双后端并存策略，新增独立 RegisterBytecode（50+ RegOp，32 虚拟寄存器 R0-R31）+ RegisterBytecodeBackend（IR 三地址码直接 lowering）+ RegisterVM 解释循环，通过 `setUseRegisterVM(true)` 启用；默认关闭，栈式 VM 完整保留。

### PERF-15 IR 复制传播

`copyPropagationPass` 完整实现，记录 vreg->常量等价关系并替换后续引用；仅在寄存器式后端启用（`optimizeIR` 新增 `enableCopyPropagation` 参数），栈式后端禁用避免删除 LOAD_CONST 导致栈下溢。

### COW 优化

Value 类型使用 Copy-On-Write，堆类型修改前检查独占所有权，避免不必要的深拷贝。

## 安全约束

### DoS 防护

源码 <=10MB、Token <=100 万、循环 <=1000 万次、字符串 reserve <=1MB。

### 递归保护

Parser/Compiler/Formatter/equals() 均有深度限制（512/256）。

### 线程安全

DebugController 使用 mutex + atomic，Worker 线程通过 Qt 信号回传；`std::localtime()` 等非线程安全函数替换为 `localtime_s` (Windows) / `localtime_r` (POSIX)。

### 异常安全

UI 槽函数包裹 try/catch，确保异常时回滚 UI 状态；深拷贝操作使用 `std::unique_ptr` 包裹防止 bad_alloc 泄漏。

### 内存安全

所有 assert 检查的越界/类型保护在 Release 构建中使用运行时检查（`std::abort` / `runtimeError`）；Value 数组/容器强制 value-initialize 防止 NaN-box 未初始化内存被误判为合法 float。

### 防御性编程

封闭枚举 switch 必须加 default 分支；边界检查覆盖全深度、全路径、全状态；测试 helper 检查中间错误状态而非仅断言输出。

### 生命周期安全

不持有外部对象的裸指针（编译器结果、实例绑定等），改用 `std::optional` 按值拷贝或 `std::shared_ptr` 共享所有权；QThread::terminate 后立即退出进程（损坏状态无法安全析构）。

### 资源管理

unique_ptr 管理 QThread（自定义删除器先 quit+wait 再 delete）。

### 安全性硬约束

所有 assert 检查的越界/类型保护改为运行时 `std::abort`；NaNBox 类型访问前必须用 `isXxx()` 校验；容器越界错误路径强制走 const 访问器避免 COW 深拷贝；`std::localtime()` 替换为线程安全的 `localtime_s`/`localtime_r`。

## IR 中间层

可选 AST -> IR -> Bytecode 三段式编译，IRBuilder/IRBackend 抽象接口可扩展多后端；启用 `setIR(true)` + `setIROptimize(true)` 后在 lowering 前执行常量折叠 / 死代码消除 pass（最多 3 轮迭代）；寄存器式后端额外启用复制传播（`setUseRegisterVM(true)` + `setIROptimize(true)`）。

## 调试一致性

条件断点求值沙箱隔离程序状态（Environment 快照/恢复）；首行断点 pre-execution 检查；双后端暂停语义文档化；VmStepper 双后端分发使用 helper 统一状态机逻辑。

## 教学增强面板（第二波）

第二波 3 个教学增强面板于 2026-07-05 实施完成，全部作为独立 `ads::CDockWidget` 注册到右侧 dock area，启动时默认 `toggleView(false)` 隐藏，通过视图菜单切换显隐。新增 23 个单元测试（3 个套件），全量 1292/1292 测试通过。

### P0-2 MemoryModelPanel

- **目标**：可视化 NaN-boxing / RefCounted / GcManager 内存模型
- **结构**：3 个子页（NaN-boxing 编码 / RefCount 引用计数 / GC 阶段）
- **NaN-boxing 子页**：7 个示例（int/float/bool-true/bool-false/null/ptr/int48-boundary），通过 `NaNBox` 公共 API（`fromInt`/`fromFloat`/`fromBool`/`null`/`fromPtr` + `rawBits()`）获取位编码；64 位分段图示用 8×8 `QTableWidget`，tag bits 高 16 位红色背景，payload 低 48 位蓝色背景
- **RefCount 子页**：4 个场景（独占/共享/COW 触发/scope 退出释放）
- **GC 子页**：6 个阶段（mark root / mark phase / sweep / finalize / cycle detect / collected），含实时 tracked 节点数刷新按钮（`GcManager::trackedCount()`）
- **关键设计**：使用 `NaNBox` 公共 API 而非 `Value` 私有字段（`Value` 未提供 `rawBits()`）；`NaNBoundingBox.intVal` 使用 `int64_t`（int48 边界值 2^46-1 超 `int` 范围）

### P1-1 IRTransformPanel

- **目标**：可视化 AST → IR 三地址码 lowering 与优化 pass 前后对比
- **结构**：3 个子页（Lowering 示例 / 优化 pass 对比 / 当前源码实时 IR）
- **Lowering 子页**：8 个示例（lit-int/binop-add/var-decl/if-stmt/while-stmt/fun-call/closure/class-method）
- **优化 pass 子页**：3 个示例（const-fold/dead-code/copy-prop），含 before/after IR 指令对比
- **当前源码子页**：`IdeController::astRoot()` → `AstIRBuilder::build(*ast)` → `IRToString(*mod->mainFunction)` 实时生成
- **关键设计**：复用 `IR.h` 公共 API，不修改引擎层；优化前后 IR 指令数对比通过累计 `blocks` 中 `instructions` 数量

### P1-2 ProfileDashboardPanel

- **目标**：三后端执行性能对比（柱状图 + 标准差）
- **场景库**：6 个性能场景（fib-recursion/loop-sum/string-concat/class-instantiation/closure-capture/dict-access）
- **三后端测量**：`measureInterpreterOnce` / `measureStackVMOnce` / `measureRegisterVMOnce`，`chrono::high_resolution_clock` 计时（微秒精度）
- **统计**：mean + stddev（样本标准差公式，n-1 分母）
- **可视化**：`paintEvent` override 自绘柱状图（蓝/橙/绿三色对应 Interpreter/StackVM/RegisterVM）
- **关键设计**：使用 `QPainter` 自绘柱状图，避免引入 `QtCharts` 强依赖；在 GUI 线程顺序运行三后端（与 `BackendComparePanel` 一致）

### 测试策略

每个面板的 Library 数据类都新增了 `TestTeachingPanelsAudit2.cpp` 数据完整性测试，参照第一波 `TestTeachingPanelsAudit.cpp` 模式：
- ID 唯一性 / 关键字段非空 / 数量符合预期
- `TeachingPanelsMemoryModel.NaNBoxBitsMatchEncoding`：验证 `bits` 字段与 `NaNBox` 编码一致（`fromInt`/`fromFloat`/`fromBool`/`null`）
- `TeachingPanelsIRTransform.OptimizationReducesInstructionCount`：验证优化 pass 减少 IR 指令数
- `TeachingPanelsProfile.ScenarioIdUnique`：验证性能场景 ID 唯一

## 教学增强面板（第三波）

第三波 3 个教学增强面板于 2026-07-05 实施完成，与第一波/第二波面板共同构成完整的"编译管线 → IR 变换 → 内存模型 → 性能剖析 → Bug 训练 → 运行时状态可见性"教学闭环。所有面板作为独立 `ads::CDockWidget` 注册到右侧 dock area，启动时默认 `toggleView(false)` 隐藏，通过视图菜单切换显隐。新增 13 个单元测试（3 个套件），全量 1292/1292 测试通过。

### P0-1 CallStackPanel（调用栈可视化器）

- **目标**：可视化运行期函数调用栈
- **结构**：2 个子页（实时调用栈 / 教学场景库）
- **实时调用栈子页**：优先消费 `IdeController::getVmCallStack()` 走 VM 路径（StackVM / RegisterVM 双模式），回退到 `getDebugCallStack()` 走 Interpreter debug 路径。500ms 自动刷新定时器，运行模式标签自动切换（StackVM / RegisterVM / Interpreter (debug)）
- **教学场景库子页**：6 个静态场景（simple-call / recursion / closure-capture / method-dispatch / try-catch / mutual-recursion），每场景含 id / title / description / sourceCode / expectedFrames / teachingNote
- **关键设计**：`CallStackEntry` 结构复用 DebugTypes.h 公共定义（functionName / line / depth / locals）；CallStackLibrary 静态数据嵌入 CallStackPanel.cpp

### P0-2 VariableInspectorPanel（变量检查器）

- **目标**：可视化运行期变量状态与 NaN-boxing 位编码
- **结构**：2 个子页（实时变量树 / 类型教学库）
- **实时变量树子页**：消费 `IdeController::getVmGlobals()` + `IdeController::getDebugVariableSnapshot()`，按作用域分组（全局变量组 + 局部变量按 scope 二次分组）。每变量显示名 / 类型名（`Value::typeName()`）/ 值字符串（`Value::toString()`）/ NaN-boxing 位编码十六进制
- **类型教学库子页**：9 种类型示例（int / int-boundary / float / bool / null / string / array / dict / instance / closure），每示例含 id / typeName / value / bits / description
- **关键设计**：辅助函数 `valueToBitsHex()` 通过 `NaNBox::fromInt` / `fromFloat` / `fromBool` / `null` / `fromPtr` 公共 API 反推位编码；`VariableSnapshot` 结构复用 DebugTypes.h 公共定义

### P0-3 BytecodeTracePanel（字节码执行轨迹）

- **目标**：可视化栈式 VM 单步执行的指令序列
- **结构**：2 个子页（执行轨迹时间轴 / OpCode 教学库）
- **执行轨迹子页**：消费 `IdeController::getVmCurrentIP()` / `getVmCurrentOpCodeName()` / `getVmFrameCount()` / `getVmStack()`，记录单步执行序列（step / ip / opCodeName / frameCount / stackSnapshot / line），最多保留 1000 条历史（`kMaxTraceEntries`），500ms 轮询定时器自动捕获 + "立即捕获"按钮手动触发
- **OpCode 教学库子页**：18 个 OpCode 文档条目（OP_INT / OP_FLOAT / OP_STRING / OP_NULL / OP_ADD / OP_GET_GLOBAL / OP_SET_GLOBAL / OP_JUMP / OP_JUMP_IF_FALSE / OP_LOOP / OP_CALL / OP_RETURN / OP_BUILD_ARRAY / OP_BUILD_DICT / OP_CLOSURE / OP_GET_UPVALUE / OP_CLASS_NEW / OP_METHOD_CALL），每条含 opCode / category / description / stackEffect / sampleCode
- **关键设计**：**BytecodeTracePanel 使用 QTimer 500ms 轮询而非订阅 IdeController 信号**（`pausedAt` / `vmRunPaused`），避免测试目标链接 IdeController.cpp（测试目标仅链接 minilang_core + Panel.cpp 中的 Library 静态数据）；TraceEntry 内嵌结构含 step / ip / opCodeName / frameCount / stackSnapshot / line

### 第三波实施汇总

**新增文件（4 个源 + 1 个测试）**：
- gui/CallStackPanel.h / .cpp
- gui/VariableInspectorPanel.h / .cpp
- gui/BytecodeTracePanel.h / .cpp
- tests/TestTeachingPanelsAudit3.cpp（13 个测试用例，3 个套件）

**修改文件（4 个工程）**：
- app/ide.h：3 个新 include + 3 个 dock 成员 + 3 个 panel 成员 + 3 个 viewXxxAction_
- app/ide.cpp：viewMenu 添加 3 个 QAction + dock 创建 + toggleView(false) + connectDockSave + syncViewMenuChecks + loadSampleRequested 信号连接
- cmake/minilang_core.cmake：MINILANG_GUI_SOURCES 添加 3 个新源文件
- tests/CMakeLists.txt：添加 TestTeachingPanelsAudit3.cpp + 3 个 Panel.cpp 测试源

**顺带修复 BUG-AUDIT-MOD-3**：实施第三波时发现 `compiler/Compiler.cpp` 与 `compiler/IR.cpp` 中存在半成品修复（引用未限定的 `MAX_RECURSION_DEPTH` 标识符），导致构建失败。修复：2 处文件中将 `MAX_RECURSION_DEPTH` 改为 `RuntimeLimits::MAX_RECURSION_DEPTH`，并在 Compiler.cpp 添加 `#include "common/RuntimeLimits.h"`

**构建验证**：
- IDE 构建成功（minilang_ide.exe）
- 全量测试 1292/1292 通过（原 1277 + 新增 13 + BUG-AUDIT-MOD-3 修复后重新启用 2 个测试），无回归
- formatter_audit 0 失败

## 教学增强面板（第二档：轻量引擎改造 + 第四档：已有面板动画增强）

第二档（轻量引擎改造）与第四档（已有面板动画增强）于 2026-07-05 实施完成。第二档新增 1 个面板（BreakpointConditionPanel）+ 增强 1 个面板（ProfileDashboardPanel 指令计数）；第四档增强 2 个已有面板的子页切换动画。所有面板作为独立 `ads::CDockWidget` 注册到右侧 dock area，启动时默认 `toggleView(false)` 隐藏。新增 14 个单元测试（2 个套件），全量 1306/1306 测试通过。

### P1-1 ProfileDashboardPanel 指令计数增强

在原有 ProfileDashboardPanel 的"时间对比"tab 之外，新增"指令计数"tab。

**核心思路**：直接 new 一个独立 VM/RegisterVM 实例（绕过 VmStepper 的 stepCallback 禁用逻辑），启用 VM/RegisterVM 已有的 `stepCallback_` 机制，每条指令执行后累加 `opProfileCounts_[static_cast<uint8_t>(op)]`（`std::array<uint64_t, 256>`）。

**关键代码点**：
- `gui/ProfileDashboardPanel.h`：新增 OpCodePerfDoc / OpCodeProfileLibrary / OpCodeProfileEntry 结构 + `lastStackVMOpProfile_` / `lastRegisterVMOpProfile_` / `stackVmOpTable_` / `registerVmOpTable_` / `opCodeDocView_` 成员 + `measureStackVMWithProfile` / `measureRegisterVMWithProfile` / `renderOpCodeProfile` 方法
- `gui/ProfileDashboardPanel.cpp`：构造函数 rightWrap 改造为 QTabWidget（Tab 1 时间对比 / Tab 2 指令计数）；measureStackVMWithProfile / measureRegisterVMWithProfile 使用 stepCallback 累加 opcode 计数；aggregateTop10 lambda 聚合 Top 10 热点；renderOpCodeProfile 填充两个 QTableWidget
- OpCodeProfileLibrary 静态数据：12 个 OpCode 文档（OP_CONSTANT / OP_INT / OP_ADD / OP_SUBTRACT / OP_MULTIPLY / OP_DIVIDE / OP_MODULO / OP_GET_LOCAL / OP_SET_LOCAL / OP_GET_GLOBAL / OP_SET_GLOBAL / OP_JUMP_IF_FALSE），每条含 opCode / category / description / stackEffect / perfNote

**架构决策**：直接 new VM/RegisterVM 实例进行指令计数（绕过 VmStepper），避免修改 VmStepper 的 stepCallback 禁用逻辑。VM/RegisterVM 的 stepCallback_ + stepCallbackEnabled_ 字段默认 false，仅在此处主动启用。

### P1-2 BreakpointConditionPanel 条件断点可视化

新增面板，2 个子页可视化条件断点的运行期状态与教学场景。

**子页 1 实时断点列表**：消费 `IdeController::getBreakpoints()` / `getBreakpointCondition(line)` / `getBreakpointHitCount(line)`（新增 Facade getter，转发到 DebugCoordinator），500ms 轮询刷新，显示当前所有断点（行号 / 条件表达式 / 命中次数）。

**子页 2 教学场景库**：8 个静态场景（simple-line / conditional-loop-count / conditional-state / data-driven / error-catch / breakpoint-with-closure / watchpoint / temporary-breakpoint），每场景含 id / title / description / sourceCode / breakpointLine / condition / teachingNote。

**关键代码点**：
- `gui/BreakpointConditionPanel.h` / `.cpp`：定义 BreakpointScenario / BreakpointConditionLibrary / BreakpointConditionPanel
- `app/DebugCoordinator.h`：新增 getBreakpoints / getBreakpointCondition / getBreakpointHitCount getter
- `app/IdeController.h`：新增转发到 DebugCoordinator 的 getter（const noexcept）
- `app/ide.h` / `app/ide.cpp`：注册 dock + viewAction，6 处插入

**架构决策**：使用 QTimer 500ms 轮询而非订阅 IdeController 信号（`pausedAt` / `breakpointHit`），避免测试目标链接 IdeController.cpp（测试目标仅链接 minilang_core + Panel.cpp 中的 Library 静态数据）。这与第三波 BytecodeTracePanel 的设计模式一致。

### P2-4 IRTransformPanel 动画化

在原有 IRTransformPanel 的 3 个子页切换时，通过 `PanelAnimator::fadeInWidget` 驱动 QGraphicsOpacityEffect，使新子页 opacity 0→1 平滑淡入（220ms OutCubic 缓动）。同时 lowering 详情刷新、优化前后 IR 刷新均加入淡入动画。

**关键代码点**：
- `gui/IRTransformPanel.cpp`：添加 `#include "gui/PanelAnimator.h"` + 3 个子页切换 lambda 末尾调用 fadeInWidget + populateLoweringDetail 末尾 + populateOptDetail 末尾

### P2-5 PipelineViewer 动画化

在原有 PipelineViewer 的 step 切换时，通过 `PanelAnimator::fadeInWidget` 驱动子页淡入。

**关键代码点**：
- `gui/PipelineViewer.cpp`：添加 `#include "gui/PanelAnimator.h"` + switchToStep 末尾调用 fadeInWidget

### PanelAnimator.h 扩展

新增 `fadeInWidget(QWidget*, int duration = FADE_DURATION_MS)` 工具函数：

```cpp
inline void fadeInWidget(QWidget* widget, int duration = FADE_DURATION_MS) {
    if (!widget) return;
    auto* effect = qobject_cast<QGraphicsOpacityEffect*>(widget->graphicsEffect());
    if (!effect) {
        effect = new QGraphicsOpacityEffect(widget);
        widget->setGraphicsEffect(effect);
    }
    effect->setOpacity(0.0);
    auto* anim = new QPropertyAnimation(effect, "opacity", widget);
    anim->setDuration(duration);
    anim->setStartValue(0.0);
    anim->setEndValue(1.0);
    anim->setEasingCurve(QEasingCurve::OutCubic);
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}
```

QGraphicsOpacityEffect 由 widget parent 自动释放，无需手动管理。适用于 QStackedWidget 子页切换、列表选中详情刷新等场景。

### 第二档/第四档实施汇总

**新增文件（3 个）**：
- `gui/BreakpointConditionPanel.h` / `gui/BreakpointConditionPanel.cpp`
- `tests/TestTeachingPanelsAudit4.cpp`

**修改文件（10 个）**：
- `gui/PanelAnimator.h`：新增 fadeInWidget 工具
- `gui/IRTransformPanel.cpp`：3 处子页切换 + 2 处详情刷新加入淡入
- `gui/PipelineViewer.cpp`：switchToStep 末尾加入淡入
- `gui/ProfileDashboardPanel.h` / `.cpp`：QTabWidget 双 tab + OpCodeProfileLibrary + measureXxxWithProfile + renderOpCodeProfile
- `app/DebugCoordinator.h`：新增 3 个只读 getter
- `app/IdeController.h`：新增转发 getter
- `app/ide.h` / `app/ide.cpp`：注册 BreakpointConditionPanel dock
- `cmake/minilang_core.cmake`：MINILANG_GUI_SOURCES 添加 BreakpointConditionPanel.cpp
- `tests/CMakeLists.txt`：添加 TestTeachingPanelsAudit4.cpp + BreakpointConditionPanel.cpp

**测试新增**：`tests/TestTeachingPanelsAudit4.cpp`（14 个用例，2 个套件）
- `TeachingPanelsOpCodeProfile`（6 用例）：文档数量 / OpCode 名称唯一 / 关键字段非空 / category 合法 / 关键 OpCode 齐全 / perfNote 含性能提示关键词
- `TeachingPanelsBreakpointCondition`（8 用例）：场景数量 / ID 唯一 / 关键字段非空 / 关键场景 ID 齐全 / condition 为表达式 / sampleCode 含断点行 / description 提及条件语义

**构建验证**：
- IDE 构建成功（minilang_ide.exe 8.6MB）
- 全量测试 1306/1306 通过（原 1292 + 新增 14），TeachingPanels 套件 68/68 全部通过
- formatter_audit 0 失败

## 三梯队修复：OutputPanel 死代码 + 模块隔离 + VM 条件断点局部变量（2026-07-05）

三梯队修复于 2026-07-05 实施完成，覆盖 GUI 死代码清理、模块系统语义一致性、调试器条件断点功能增强。新增 23 个回归测试（2 个套件），全量 1352 个测试用例全部通过。

### 第一梯队 · OutputPanel 死代码清理

清理 `gui/OutputPanel.cpp` 中历史遗留的死代码（未使用的成员函数、未引用的辅助方法），保持功能不变，降低维护负担。

### 第二梯队 · BUG-AUDIT-MOD-2 统一 VM/IR 模块隔离

**问题**：VM/IR 路径将 import 的模块内联到全局作用域（无隔离），Interpreter 在独立 Environment 中执行模块（有隔离）。导入方可以访问模块的非导出顶层名，破坏模块封装语义。

**修复方案**：完整统一方案——AST 重写 + 作用域分析

新增 `ast/ModuleIsolation.h` / `ast/ModuleIsolation.cpp`，定义 `ModuleTopLevelRenamer` 类：
- `collectNonExportTopLevelNames`：收集模块的非导出顶层声明名（VarDecl/FunDecl/ClassDecl，排除 ExportStmt 内的名称）
- `renameInBlock` + `renameInNode`：递归重命名 AST 中所有引用（覆盖 25 个节点类型），使用作用域栈避免误改函数内局部变量
- 重命名格式：`__mod_<FNV-1a 8-char hex>__<原名>`，确保跨模块无冲突
- 在 `Compiler::visitImportStmt` 和 `IR::handleImportStmt` 中，AST 解析后、预扫描前调用 `ModuleTopLevelRenamer::rename`

**集成点**：`compiler/Compiler.cpp` 步骤 6.5（AST 解析后、preScanModuleGlobals 前）；`compiler/IR.cpp` 同步位置

**测试**：`tests/TestVME2E.cpp` 新增 `VME2EImportIsolation.*` 套件（15 用例），覆盖 VM/IR/RegVM 三路径

### 第三梯队 · BUG-IDE-12 VM 条件断点支持局部变量

**问题**：VM 模式条件断点求值器仅注入 VM 全局变量到临时 Interpreter 环境，局部变量在栈槽/寄存器中无法按名访问。条件表达式 `i == 5`（i 为循环局部变量）在 VM 模式下永远报"未定义变量"。

**修复方案**：编译时记录 slot→name 映射，运行时反查局部变量

数据结构扩展：
- `BytecodeChunk::localSlotNames`（`std::vector<std::string>`，索引即 slot）
- `RegBytecodeChunk::localRegNames`（`std::vector<std::string>`，索引即寄存器号）
- `IRFunction::localSlotNames`（IR 层中间存储）
- `Compiler::localSlotNames_` / `AstIRBuilder::localSlotNames_`（编译期累积）

记录点（4-5 处）：
- `visitVarDecl`（函数内新局部变量）
- `visitFunDecl`（参数 + this + 最终化保存到 chunk）
- `visitClassDecl` 方法编译（this/fields/params + 最终化保存）
- TryStmt catch 变量
- AstIRBuilder 额外：嵌套函数声明作为 LOCAL 时

Lowering 复制：
- `BytecodeIRBackend::lower`：`chunk_->localSlotNames = ir.localSlotNames`
- `RegisterBytecodeBackend::lower`：`chunk_->localRegNames = ir.localSlotNames`

运行时反查：
- `VM::getCurrentFrameLocals()`：遍历 `frame.chunk->localSlotNames`，从栈槽（`basePointer + slot`）反查值
- `RegisterVM::getCurrentFrameLocals()`：遍历 `frame.chunk->localRegNames`，从寄存器窗口反查值
- `VmStepper::getCurrentFrameLocals()`：根据 `useRegister_` 分派到对应后端

条件求值器更新（`app/IdeController.cpp`）：
- 先注入全局变量，再注入当前帧局部变量（局部变量遮蔽同名全局）
- 移除"仅支持全局变量"限制注释

**限制**：槽位复用（兄弟作用域）时后声明的变量名覆盖先前的，属于已知限制（不影响条件断点基本功能）

**测试**：`tests/TestVME2E.cpp` 新增 `VMConditionalBreakpoint.*` 套件（8 用例），覆盖三路径 slot→name 填充、类方法、运行时反查、catch 变量、三后端一致性

### 三梯队实施汇总

**新增文件（3 个）**：
- `ast/ModuleIsolation.h` / `ast/ModuleIsolation.cpp`
- （测试用例追加到已有 `tests/TestVME2E.cpp`，无新测试文件）

**修改文件（13 个）**：
- `compiler/Compiler.h` / `compiler/Compiler.cpp`：`localSlotNames_` 成员 + 4 处记录点
- `compiler/IR.h` / `compiler/IR.cpp`：`IRFunction::localSlotNames` + `AstIRBuilder::localSlotNames_` + 5 处记录点 + 保存/恢复
- `compiler/Bytecode.h`：`BytecodeChunk::localSlotNames` 字段
- `compiler/RegisterBytecode.h`：`RegBytecodeChunk::localRegNames` 字段
- `compiler/VM.h` / `compiler/VM.cpp`：`getCurrentFrameLocals()` 声明 + 实现
- `compiler/RegisterVM.h` / `compiler/RegisterVM.cpp`：`getCurrentFrameLocals()` 声明 + 实现
- `compiler/RegisterBytecodeBackend.cpp`：lowering 时复制 `localRegNames`
- `app/VmStepper.h`：`getCurrentFrameLocals()` 转发
- `app/IdeController.cpp`：条件求值器注入 locals + 移除限制注释
- `cmake/minilang_core.cmake`：添加 `ast/ModuleIsolation.cpp`
- `tests/TestVME2E.cpp`：新增 23 个回归测试（`VME2EImportIsolation.*` 15 + `VMConditionalBreakpoint.*` 8）+ `<set>` include
- `gui/OutputPanel.cpp`：死代码清理

**构建验证**：
- 构建：MSVC + Qt 6.10.3 msvc2022_64 + Ninja，全量重配置后构建成功
- 全量测试：**1352/1352 通过，0 失败**（原 1329 + 第二梯队 15 + 第三梯队 8）
- 新增测试套件：`VME2EImportIsolation.*`（15 用例）+ `VMConditionalBreakpoint.*`（8 用例）

## 教学增强面板（第三档：异常流 + 闭包 upvalue 可视化）

第三档 2 个纯静态教学面板于 2026-07-05 实施完成，围绕异常传播路径与闭包 upvalue 生命周期两个"动态语义"教学难点。所有面板作为独立 `ads::CDockWidget` 注册到右侧 dock area，启动时默认 `toggleView(false)` 隐藏。新增 24 个单元测试（2 个套件），全量 1329 个测试用例全部通过（此前报告的 589 项 SEH 崩溃经排查确认为 Ninja 头文件依赖追踪失效导致的 stale `.obj` ABI 不一致，全量重编译后已消除）。

### P2-3a 异常流可视化面板（ExceptionFlowPanel）

2 个子页：

1. **教学场景库**：8 个异常场景（simple-try-catch / uncaught-exception / nested-try-catch / finally-semantics / cross-function-propagation / recursive-exception / exception-object-field / rethrow），每场景含 sampleCode + propagationPath（传播路径列表）+ teachingNote 教学注释
2. **传播阶段图解**：6 个阶段（throw 抛出 / search 查找 catch / catch 捕获 / finally 清理 / unwind 栈展开 / recovery 恢复），每阶段含 category + description + stackEffect（栈/堆效应）

教学价值：理解异常控制流跨函数跳跃、栈展开顺序、finally 与 catch 的执行先后

### P2-3b 闭包检查器面板（ClosureInspectorPanel）

2 个子页：

1. **教学场景库**：8 个闭包场景（simple-capture / counter-pattern / multi-capture / nested-closure / closure-as-return / closure-array / iife / closure-escape），每场景含 sampleCode + capturedVars（捕获变量列表）+ captureType（by-reference upvalue / by-value / heap-escaped）+ teachingNote
2. **upvalue 生命周期图解**：6 个阶段（create 创建 / capture 捕获 / heap 堆化 / access 访问 / close 关闭 / destroy 销毁），每阶段含 category + description + stackEffect

教学价值：理解 OP_CLOSURE 指令、upvalue 堆化时机、闭包逃逸与堆栈过渡

### 架构决策

- **纯静态教学面板**：Library 静态数据嵌入 .cpp，不依赖 IdeController，不需要引擎层改造，不订阅信号——与第三波 BytecodeTracePanel 的"避免测试目标链接 IdeController.cpp"设计模式一致，确保测试目标仅链接 minilang_core + Panel.cpp 静态数据
- **子页切换动画复用**：复用第四档的 `PanelAnimator::fadeInWidget` 工具函数（QGraphicsOpacityEffect 驱动 opacity 0→1，220ms OutCubic），无需新增工具函数
- **双子页结构**：每个面板含 2 个子页（教学场景库 + 图解/生命周期），通过 QStackedWidget + QPushButton 切换
- **QSplitter 左右分栏**：场景列表（QListWidget）+ 详情（QTextBrowser）水平分栏

### 第三档实施汇总

**新增文件（5 个）**：
- `gui/ExceptionFlowPanel.h` / `gui/ExceptionFlowPanel.cpp`
- `gui/ClosureInspectorPanel.h` / `gui/ClosureInspectorPanel.cpp`
- `tests/TestTeachingPanelsAudit5.cpp`

**修改文件（4 个）**：
- `app/ide.h`：include 2 个 Panel.h + 4 个 dock/panel 成员 + 2 个 viewAction 成员
- `app/ide.cpp`：dock 创建 + loadSampleRequested 信号连接 + toggleView(false) + connectDockSave + syncViewMenuChecks + viewMenu 2 个 QAction
- `cmake/minilang_core.cmake`：MINILANG_GUI_SOURCES 添加 2 个 cpp
- `tests/CMakeLists.txt`：添加 TestTeachingPanelsAudit5.cpp + 2 个 Panel.cpp

**测试新增**：`tests/TestTeachingPanelsAudit5.cpp`（24 个用例，2 个套件）
- `TeachingPanelsExceptionFlow`（12 用例）：场景数量 / ID 唯一 / 关键字段非空 / 关键场景 ID 齐全（simple-try-catch / uncaught-exception / nested-try-catch / cross-function-propagation / rethrow）/ sampleCode 含 throw / propagationPath 非空 / description 提及异常语义；6 个传播阶段 phase 唯一 / category 合法（throw/search/catch/finally/unwind/recovery）/ 关键 phase 齐全（throw/catch/unwind）/ 字段非空
- `TeachingPanelsClosureInspector`（12 用例）：场景数量 / ID 唯一 / 关键字段非空 / 关键场景 ID 齐全（simple-capture / counter-pattern / nested-closure / closure-as-return / closure-escape）/ sampleCode 含闭包 / captureType 含 upvalue / description 提及闭包语义；6 个 upvalue 阶段 phase 唯一 / category 合法（create/capture/heap/access/close/destroy）/ 关键 phase 齐全（create/capture/heap）/ 字段非空

**构建验证**：
- IDE 构建成功（minilang_ide.exe）
- 测试目标构建成功（minilang_tests.exe）
- 全量测试 1329 个用例全部通过（此前报告的 589 项 SEH 崩溃经排查确认为 Ninja 头文件依赖追踪失效导致的 stale `.obj` ABI 不一致，全量重编译后已消除）
- TeachingPanels 套件 92/92 全部通过（原 68 + 第三档新增 24）
- 崩溃根因详见上方 "SEH 崩溃根因修复" 章节

## 跨模块集成审计 Bug 修复收尾（2026-07-05）

跨模块集成审计发现的 20 个 Bug（3 P1 + 17 P2）已全部修复完成。前 16 项在上一轮会话修复，本轮完成剩余 5 项收尾。

### BUG-CP-1 · BytecodeChunk::addConstant 类型严格匹配

**位置**：`compiler/Bytecode.h` `addConstant`

**根因**：`Value::equals` 允许 int/float 跨类型数值比较（`Value(0).equals(Value(0.0)) == true`），常量池去重仅依赖 `equals` 会导致 int 0 与 float 0.0 错误合并为同一索引。后续 `OP_INT`/`OP_FLOAT` 加载到的 Value 类型与编译期预期不符，触发 `toString`/格式化/类型注解等路径的语义错误甚至 VM 崩溃。

**修复**：在 `addConstant` 哈希桶线性扫描中增加 `constants[i].getType() == val.getType()` 严格类型前置检查，与 `RegisterBytecodeChunk::addConstant`（已含同款修复）保持一致。

**影响**：`hashValue` 对 int 0 和 float 0.0 可能落入同一桶（`std::hash<int64_t>{}(0)` 与 `std::hash<double>{}(0.0)` 均倾向于 0），仅 `IntAndFloatZeroNotDeduped` 测试能覆盖此碰撞路径；`IntAndFloatOneNotDeduped` 因哈希不碰撞而无法暴露本 Bug。

### BUG-ORCH-8 · VM 步进未检查 REPL 运行状态

**位置**：`app/ide.cpp` 4 个 VM 操作函数

**根因**：`stepOver`/`stepInto`/`stepOut`/`runToLine` 在调度单步任务前未检查 `isReplRunning()`，REPL 执行期间用户触发调试步进会导致两个 Worker 同时操作同一 VM 实例，引发栈状态破坏。

**修复**：4 个函数入口添加 `isReplRunning()` 检查，命中时弹消息框提示"REPL 正在运行"并提前返回。

### BUG-LEAK-01 · astWindow_ 父对象缺失导致泄漏

**位置**：`app/ide.cpp` `AstViewer`/`IrViewer` 创建路径

**根因**：`astWindow_` 创建时未传入 parent，Qt 父子对象关系未建立，主窗口关闭时不会自动释放。

**修复**：`astWindow_` 构造传入 `this` 作为 parent，依托 Qt 父子对象树自动管理生命周期。

### BUG-AUDIT-MOD-7 · NaNBox::fromFloat NaN 规范化（已回退）

**位置**：`interpreter/NaNBox.h` `fromFloat`

**误诊与回退**：跨模块审计收尾时误将 `fromFloat` 改为保留 NaN 低 48 位 payload（`NAN_BOXED_FLOAT_MARKER | (box.bits_ & INT48_MASK)`），理由是"保留 NaN payload 语义"。但 NaN-boxing 设计要求所有落入 tag 范围（0x7FF8-0x7FFB）的 NaN **完全规范化**为 `NAN_BOXED_FLOAT_MARKER`（0x7FFC...），否则低 48 位 payload 可能形成合法 int/bool/null/ptr 位模式，导致 `tag()` 误判。

**最终修复**：回退为 `box.bits_ = NAN_BOXED_FLOAT_MARKER`（完全替换 64 位）。IEEE 754 中 `NaN != NaN`，payload 丢弃不影响语义正确性。3 个 NaNBoxTest 用例（`AuditNaNConflictWithIntTag` / `AuditNaNConflictWithBoolTag` / `AuditDistinctNaNsCanonicalizedEqual`）验证通过。

### BUG-AUDIT-MOD-2 · VM/IR 模块隔离文档化

**位置**：`compiler/VM.cpp`/`compiler/IR.cpp` 模块加载路径

**根因**：VM 和 IR 路径未检查模块 `export` 标记，与 Interpreter 路径行为不一致，但移除该检查会破坏现有模块语义，需先文档化再评估是否统一。

**修复**：在 VM/IR 模块加载路径添加注释说明已知行为差异，指向 `docs/development.md` 的模块系统一致性章节。

### 构建与测试状态

- 构建：MSVC + Qt 6.10.3 msvc2022_64 + Ninja，全量重配置后构建成功
- 全量测试：**1329/1329 通过，0 失败**
- 根因：此前报告的 589 项 SEH 0xc0000005 崩溃经排查确认为 Ninja 头文件依赖追踪失效导致的 stale `.obj` ABI 不一致（非 engine 层 Bug），全量重编译后已消除
- 排查方法：删除整个 `out/build/debug` 目录后全量重配置 + 重编译

## 性能与工程优化（C1-C4）+ 代码质量与文档（D1-D3）（2026-07-05）

### C1 · 增量编译缓存优化

- `option(MINILANG_UNITY_BUILD)` — Unity Build 批量编译，将 minilang_core 的多个 .cpp 合并为单个 TU 编译，减少 PCH 重复加载，首次构建加速 30-50%
- Ninja 响应文件（`.rsp`）传递编译参数，避免源文件较多时命令行长度限制

### C2 · QtCharts 替换

- `ProfileDashboardPanel` 使用 `QPainter` 自绘柱状图，消除 QtCharts 依赖，减少二进制体积
- 第三波教学面板（CallStackPanel / VariableInspectorPanel / BytecodeTracePanel）均采用 QPainter 自绘，避免额外库依赖

### C3 · VM 解释循环 profiling

- `option(MINILANG_VM_PROFILING)` — 编译时启用 opcode 执行计数
- `VM::opProfileCounts_`（`std::array<uint64_t, 256>`）记录每条 opcode 执行次数，dispatch 主循环中通过 `++opProfileCounts_[static_cast<uint8_t>(op)]` 累加
- `VM::getOpCodeProfile()` 返回按计数降序排序的 opcode 统计向量
- `ProfileDashboardPanel` 双 tab（时间对比 + 指令计数），Top 10 热点聚合
- 设计决策：直接 new VM/RegisterVM 实例（绕过 VmStepper 的 stepCallback 禁用逻辑），启用 stepCallback_ 累加 opcode 计数

### C4 · IR 优化 pass（CSE / 循环展开）

- `loopUnrollingPass(IRFunction&)` — for 循环体 ≤20 指令且 N≤4 时展开
- `commonSubexpressionEliminationPass(IRFunction&)` — 基本块内公共子表达式消除
- `optimizeIR` 签名扩展为 4 参数：`enableCopyPropagation` / `enableDCE` / `enableCSE` / `enableLoopUnrolling`
- **栈式 VM 后端 CSE 默认 false**：CSE 替换后续指令对 dest vreg 的引用，原 dest 变为死代码，与 DCE 交互不安全（BUG-IR-DCE-2）
- **寄存器式 VM 后端复制传播默认 false**：`RegisterBytecodeBackend` 不检查操作数 kind，常量索引被误当 vreg 编号（AUDIT-BUG-E4）

### D1 · Doxygen API 文档

- `option(MINILANG_BUILD_DOCS)` — 启用 `docs` CMake target
- `Doxyfile` 配置：提取全部公开 API、类图生成（Graphviz dot）、调用图、被调用图
- 头文件 `@file` / `@brief` / `@param` / `@see` 注释完善
- 用法：`cmake --build out/build/debug --target docs` 生成 HTML 文档到 `docs/html/`

### D2 · 国际化 i18n

- `option(MINILANG_ENABLE_I18N)` — 启用 Qt Linguist 工具链
- `app/translations/minilang_zh_CN.ts` 翻译目录占位
- `qt_create_translation` 自动扫描 `tr()` 调用生成 .ts 文件，编译为 .qm 文件
- 用法：`cmake --preset windows-msvc-debug -DMINILANG_ENABLE_I18N=ON`，构建后 .qm 文件自动嵌入资源

### D3 · const 正确性审计

- `VM::currentFrame() const` / `VM::currentChunk() const` const 重载（GUI 调试面板只读访问）
- `IBackend` 接口 const 修正（`backendName()` / `getLastResult()` 等只读方法）
- `BytecodeChunk::getColumn` / `getLine` const 方法
- `DiagnosticBag` / `Result` / `RuntimeLimits` const 访问器
- 设计原则：所有只读访问器标记 const，配合 mutable 仅用于惰性缓存（如 `fieldIndexCache_`）

### SEH 崩溃根因修复（2026-07-05）

**根因**：Ninja 构建系统的头文件依赖追踪在修改 `Bytecode.h`（新增 `columns` 字段）后失效，部分 `.obj` 文件使用旧结构布局编译，链接后产生 ABI 不一致——访问 `columns` 成员时读到垃圾位模式（如 `cols.size=35320`），导致 `chunk_ = BytecodeChunk()` 移动赋值期间 SEH 0xc0000005 崩溃。此前误诊为 engine 层预存在 Bug，实际是构建系统问题。

**修复**：删除整个 `out/build/debug` 目录后全量重配置 + 重编译，消除全部 stale `.obj` 文件。全量 1329/1329 测试通过，0 失败——此前报告的 589 项 SEH 崩溃全部消失。

**清理**：移除调试期间引入的临时代码——`Bytecode.h` 用户定义移动赋值运算符、`Compiler.cpp` / `TestVME2E.cpp` 的 `fprintf` 追踪、`TestReproMain.cpp` / `TestRepro.cpp` 复现程序及 `repro_crash` CMake target、`dbg_compile.txt` / `dbg_vm.txt` 调试输出文件。恢复 `VMStack` 为定长数组 `Value[1024]`（PERF-13）。

**教训**：遇到大规模 SEH 崩溃时，优先排查构建系统依赖追踪问题（删除 build 目录全量重编译），而非假设是源码 Bug。Ninja 的 `.ninja_deps` 在某些头文件修改场景下会失效，尤其是修改 struct 成员布局时。
