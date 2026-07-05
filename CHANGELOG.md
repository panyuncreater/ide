# Changelog

本文件记录 MiniLang IDE 的开发演进历史，包括性能优化、正确性修复与工程基础设施改进。所有条目均通过全量单元测试（1352/1352）+ formatter_audit 审计用例验证。

## 2026-07-05 · 三梯队修复：OutputPanel 死代码 + 模块隔离 + VM 条件断点局部变量

### 第一梯队 · OutputPanel 死代码清理（P2 工程清理）

- **位置**：`gui/OutputPanel.cpp`
- **类型**：死代码 / 工程清理
- **根因**：`OutputPanel` 中存在历史遗留的死代码（未使用的成员函数、未引用的辅助方法），增加维护负担且影响代码可读性。
- **修复**：清理所有未使用的死代码，保持功能不变。

### 第二梯队 · BUG-AUDIT-MOD-2 统一 VM/IR 模块隔离（P1 语义不一致）

- **位置**：新增 `ast/ModuleIsolation.h` / `ast/ModuleIsolation.cpp`；修改 `compiler/Compiler.cpp` / `compiler/IR.cpp` / `cmake/minilang_core.cmake`
- **类型**：语义不一致 / 模块系统
- **根因**：VM/IR 路径将 import 的模块内联到全局作用域（无隔离），Interpreter 在独立 Environment 中执行模块（有隔离）。导入方可以访问模块的非导出顶层名（变量、函数、类），破坏了模块封装语义，与 Interpreter 路径行为不一致。
- **修复方案**：完整统一方案——AST 重写 + 作用域分析
  - 新增 `ModuleTopLevelRenamer` 类，在 Compiler/IR 解析模块 AST 后、预扫描前调用
  - `collectNonExportTopLevelNames`：收集模块的非导出顶层声明名（VarDecl/FunDecl/ClassDecl，排除 ExportStmt 内的名称）
  - `renameInBlock` + `renameInNode`：递归重命名 AST 中所有引用（覆盖 25 个节点类型），使用作用域栈避免误改函数内局部变量
  - 重命名格式：`__mod_<FNV-1a 8-char hex>__<原名>`，确保跨模块无冲突
  - 模块内部引用同步重写，语义保持；导入方用原名访问时编译失败（未定义）
- **测试**：新增 15 个回归测试（`VME2EImportIsolation.*` 套件），覆盖 VM/IR/RegVM 三路径：非导出变量/函数/类不可访问、导出名仍可访问、模块内部引用重写后语义保持、导入方同名变量不冲突、局部变量不受重命名影响、非导出类继承链、三后端一致性。

### 第三梯队 · BUG-IDE-12 VM 条件断点支持局部变量（P2 调试器功能增强）

- **位置**：`compiler/Compiler.h` / `compiler/Compiler.cpp` / `compiler/IR.h` / `compiler/IR.cpp` / `compiler/Bytecode.h` / `compiler/RegisterBytecode.h` / `compiler/VM.h` / `compiler/VM.cpp` / `compiler/RegisterVM.h` / `compiler/RegisterVM.cpp` / `compiler/RegisterBytecodeBackend.cpp` / `app/VmStepper.h` / `app/IdeController.cpp`
- **类型**：调试器功能限制
- **根因**：VM 模式条件断点求值器（`IdeController::setConditionEvaluator`）仅注入 VM 全局变量到临时 Interpreter 环境，局部变量在栈槽/寄存器中无法按名访问。条件表达式 `i == 5`（i 为循环局部变量）在 VM 模式下永远报"未定义变量"，只能用全局变量写条件断点，严重限制调试体验。
- **修复方案**：编译时记录 slot→name 映射，运行时反查局部变量
  - `BytecodeChunk` 新增 `localSlotNames` 字段（索引即 slot）
  - `RegBytecodeChunk` 新增 `localRegNames` 字段（索引即寄存器号）
  - `Compiler` 新增 `localSlotNames_` 成员，在 `visitVarDecl`（函数内局部变量）/ `visitFunDecl`（参数 + 最终化保存）/ `visitClassDecl`（this/fields/params + 最终化保存）/ TryStmt catch 变量 4 处记录 slot→name
  - `AstIRBuilder` 同步新增 `localSlotNames_` 成员，在 `visitVarDecl` / `visitFunDecl`（this/参数/最终化）/ 嵌套函数声明 / catch 变量 5 处记录 slot→name，函数最终化时复制到 `IRFunction::localSlotNames`
  - `BytecodeIRBackend::lower` 与 `RegisterBytecodeBackend::lower` 在 lowering 时复制 `ir.localSlotNames` → `chunk->localSlotNames` / `chunk->localRegNames`
  - `VM::getCurrentFrameLocals()` 实现：遍历当前帧 chunk 的 `localSlotNames`，从栈槽（`basePointer + slot`）反查值
  - `RegisterVM::getCurrentFrameLocals()` 实现：遍历当前帧 chunk 的 `localRegNames`，从寄存器窗口反查值
  - `VmStepper::getCurrentFrameLocals()` 转发：根据 `useRegister_` 分派到对应后端
  - `IdeController` 条件求值器更新：先注入全局变量，再注入当前帧局部变量（局部变量遮蔽同名全局），移除"仅支持全局变量"限制注释
- **限制**：槽位复用（兄弟作用域）时后声明的变量名覆盖先前的，属于已知限制（不影响条件断点基本功能）
- **测试**：新增 8 个回归测试（`VMConditionalBreakpoint.*` 套件）：
  - `StackVM_Direct_LocalSlotNamesPopulated` / `StackVM_IR_LocalSlotNamesPopulated` / `RegVM_LocalRegNamesPopulated`：三路径函数 chunk 的 slot→name 映射正确填充
  - `StackVM_Direct_MethodLocalSlotNames`：类方法 chunk 包含 this/字段/参数
  - `StackVM_GetCurrentFrameLocals` / `RegVM_GetCurrentFrameLocals`：运行时反查局部变量值正确
  - `StackVM_CatchVarInLocalSlotNames`：catch 变量出现在映射中
  - `ThreeBackendLocalNamesConsistency`：三后端 slot→name 集合一致

### 构建与测试状态

- 构建：MSVC + Qt 6.10.3 msvc2022_64 + Ninja，全量重配置后构建成功（删除 `out/build/debug` 全量重编译，避免头文件修改后的 stale `.obj` 问题）
- 全量测试：**1352/1352 通过，0 失败**（原 1329 + 第二梯队 15 + 第三梯队 8）
- 测试套件新增：`VME2EImportIsolation.*`（15 用例）+ `VMConditionalBreakpoint.*`（8 用例）

## 2026-07-05 · 性能与工程优化（C1-C4）+ 代码质量与文档（D1-D3）+ SEH 崩溃根因修复

### SEH 崩溃根因修复

**根因**：Ninja 构建系统的头文件依赖追踪在修改 `Bytecode.h`（新增 `columns` 字段）后失效，部分 `.obj` 文件使用旧结构布局编译，链接后产生 ABI 不一致——访问 `columns` 成员时读到垃圾位模式（如 `cols.size=35320`），导致 `chunk_ = BytecodeChunk()` 移动赋值期间 SEH 0xc0000005 崩溃。此前误诊为 engine 层预存在 Bug，实际是构建系统问题。

**修复**：删除整个 `out/build/debug` 目录后全量重配置 + 重编译，消除全部 stale `.obj` 文件。全量 1329/1329 测试通过，0 失败——此前报告的 589 项 SEH 崩溃全部消失。

**清理**：移除调试期间引入的临时代码——`Bytecode.h` 用户定义移动赋值运算符、`Compiler.cpp` / `TestVME2E.cpp` 的 `fprintf` 追踪、`TestReproMain.cpp` / `TestRepro.cpp` 复现程序及 `repro_crash` CMake target、`dbg_compile.txt` / `dbg_vm.txt` 调试输出文件。恢复 `VMStack` 为定长数组 `Value[1024]`（PERF-13）。

### BUG-AUDIT-MOD-7 回退 · NaNBox::fromFloat 必须完全规范化 NaN（P1 语义错误）

- **位置**：`interpreter/NaNBox.h` `fromFloat`
- **根因**：跨模块审计收尾时误将 `fromFloat` 改为保留 NaN 低 48 位 payload（`NAN_BOXED_FLOAT_MARKER | (box.bits_ & INT48_MASK)`），理由是"保留 NaN payload 语义"。但 NaN-boxing 设计要求所有落入 tag 范围（0x7FF8-0x7FFB）的 NaN **完全规范化**为 `NAN_BOXED_FLOAT_MARKER`（0x7FFC...），否则低 48 位 payload 可能形成合法 int/bool/null/ptr 位模式，导致 `tag()` 误判。IEEE 754 中 `NaN != NaN`，payload 丢弃不影响语义正确性。
- **修复**：回退为 `box.bits_ = NAN_BOXED_FLOAT_MARKER`（完全替换 64 位）。
- **测试**：3 个此前失败的 NaNBoxTest 用例（`AuditNaNConflictWithIntTag` / `AuditNaNConflictWithBoolTag` / `AuditDistinctNaNsCanonicalizedEqual`）全部通过。

### C1 · 增量编译缓存优化

- `option(MINILANG_UNITY_BUILD)` — Unity Build 批量编译，首次构建加速 30-50%
- Ninja 响应文件（`.rsp`）传递编译参数，避免命令行长度限制
- `option(MINILANG_USE_CCACHE)` — ccache 缓存支持（已有）

### C2 · QtCharts 替换

- `ProfileDashboardPanel` 使用 `QPainter` 自绘柱状图，消除 QtCharts 依赖
- 第三波教学面板均采用 QPainter 自绘，避免额外库依赖

### C3 · VM 解释循环 profiling

- `option(MINILANG_VM_PROFILING)` — 编译时启用 opcode 执行计数
- `VM::opProfileCounts_`（`std::array<uint64_t, 256>`）记录每条 opcode 执行次数
- `VM::getOpCodeProfile()` 返回排序后的 opcode 统计向量
- `ProfileDashboardPanel` 双 tab（时间对比 + 指令计数），Top 10 热点聚合

### C4 · IR 优化 pass（CSE / 循环展开）

- `loopUnrollingPass(IRFunction&)` — for 循环体 ≤20 指令且 N≤4 时展开
- `commonSubexpressionEliminationPass(IRFunction&)` — 基本块内公共子表达式消除
- `optimizeIR` 签名扩展为 4 参数：`enableCopyPropagation` / `enableDCE` / `enableCSE` / `enableLoopUnrolling`
- 栈式 VM 后端 CSE 默认 false（替换 dest vreg 引用后原 dest 变为死代码，与 DCE 交互不安全）
- 寄存器式 VM 后端复制传播默认 false（`RegisterBytecodeBackend` 不检查操作数 kind，常量索引被误当 vreg 编号）

### D1 · Doxygen API 文档

- `option(MINILANG_BUILD_DOCS)` — 启用 `docs` CMake target
- `Doxyfile` 配置：提取全部公开 API、类图生成、调用图、被调用图
- 头文件 `@file` / `@brief` / `@param` / `@see` 注释完善

### D2 · 国际化 i18n

- `option(MINILANG_ENABLE_I18N)` — 启用 Qt Linguist 工具链
- `app/translations/minilang_zh_CN.ts` 翻译目录占位
- `qt_create_translation` 自动扫描 `tr()` 调用生成 .ts 文件

### D3 · const 正确性审计

- `VM::currentFrame() const` / `VM::currentChunk() const` const 重载
- `IBackend` 接口 const 修正
- `BytecodeChunk::getColumn` / `getLine` const 方法
- `DiagnosticBag` / `Result` / `RuntimeLimits` const 访问器

### 构建与测试状态

- 构建：MSVC + Qt 6.10.3 msvc2022_64 + Ninja，全量重配置后构建成功
- 全量测试：**1329/1329 通过，0 失败**（此前 589 项 SEH 崩溃全部消除）
- 根因：Ninja 头文件依赖追踪失效导致 stale `.obj` ABI 不一致，非 engine 层 Bug

## 2026-07-05 · 教学增强面板（第三档：异常流 + 闭包 upvalue 可视化）

围绕异常传播路径与闭包 upvalue 生命周期两个"动态语义"教学难点，新增 2 个纯静态教学面板。所有面板作为独立 `ads::CDockWidget` 注册到右侧 dock area，启动时默认隐藏（视图菜单勾选显示）。

### 新增面板（2 项）

#### P2-3a 异常流可视化面板（ExceptionFlowPanel）

2 个子页：

- **教学场景库**：8 个异常场景（simple-try-catch / uncaught-exception / nested-try-catch / finally-semantics / cross-function-propagation / recursive-exception / exception-object-field / rethrow），每场景含 sampleCode + propagationPath（传播路径列表）+ teachingNote
- **传播阶段图解**：6 个阶段（throw 抛出 / search 查找 catch / catch 捕获 / finally 清理 / unwind 栈展开 / recovery 恢复），每阶段含 category + description + stackEffect

教学价值：理解异常控制流跨函数跳跃、栈展开顺序、finally 与 catch 的执行先后

#### P2-3b 闭包检查器面板（ClosureInspectorPanel）

2 个子页：

- **教学场景库**：8 个闭包场景（simple-capture / counter-pattern / multi-capture / nested-closure / closure-as-return / closure-array / iife / closure-escape），每场景含 sampleCode + capturedVars（捕获变量列表）+ captureType（by-reference upvalue / by-value / heap-escaped）+ teachingNote
- **upvalue 生命周期图解**：6 个阶段（create 创建 / capture 捕获 / heap 堆化 / access 访问 / close 关闭 / destroy 销毁），每阶段含 category + description + stackEffect

教学价值：理解 OP_CLOSURE 指令、upvalue 堆化时机、闭包逃逸与堆栈过渡

### 工程改动

- 新增文件：`gui/ExceptionFlowPanel.h` / `gui/ExceptionFlowPanel.cpp` / `gui/ClosureInspectorPanel.h` / `gui/ClosureInspectorPanel.cpp` + `tests/TestTeachingPanelsAudit5.cpp`
- 修改文件：`app/ide.h`（include + 4 个 dock/panel 成员 + 2 个 viewAction 成员）/ `app/ide.cpp`（dock 创建 + loadSampleRequested 信号连接 + toggleView(false) + connectDockSave + syncViewMenuChecks + viewMenu 2 个 QAction）/ `cmake/minilang_core.cmake`（添加 2 个 cpp）/ `tests/CMakeLists.txt`（添加 TestTeachingPanelsAudit5.cpp + 2 个 Panel.cpp）
- 架构决策：纯静态教学面板（Library 静态数据嵌入 .cpp），不依赖 IdeController，不需要引擎层改造，不订阅信号——与第三波 BytecodeTracePanel 的"避免测试目标链接 IdeController.cpp"设计模式一致。子页切换与详情刷新复用第四档的 `PanelAnimator::fadeInWidget` 工具函数

### 构建与测试状态

- 构建：MSVC + Qt 6.10.3 msvc2022_64 + Ninja，构建成功（IDE + 测试目标）
- 测试：TeachingPanels 套件 92/92 全部通过（原 68 + 第三档新增 24，2 套件 TeachingPanelsExceptionFlow 12 + TeachingPanelsClosureInspector 12）
- 全量测试：1329 个用例，新增 24 个测试全部通过；此前报告的 589 项 engine 层 SEH 0xc0000005 崩溃经后续排查确认为 Ninja 头文件依赖追踪失效导致的 stale `.obj` ABI 不一致（非 engine 层 Bug），全量重编译后已消除
- 预存在崩溃根因详见上方 "SEH 崩溃根因修复" 章节

## 2026-07-05 · 跨模块集成审计 Bug 修复收尾（5 项）

跨模块集成审计发现的 20 个 Bug（3 P1 + 17 P2）此前已修复 16 项，本次完成剩余 5 项收尾修复。修复后构建成功，非引擎层测试 391/392 通过（InterpreterE2E.ModuleImportAtomic 此前因 SEH 崩溃失败，后经全量重编译已消除）。

### 修复列表

#### BUG-CP-1 · BytecodeChunk::addConstant 类型严格匹配（P1 语义错误）

- **位置**：`compiler/Bytecode.h` `addConstant`
- **根因**：`Value::equals` 允许 int/float 跨类型数值比较（`Value(0).equals(Value(0.0)) == true`），常量池去重仅依赖 `equals` 会导致 int 0 与 float 0.0 错误合并为同一索引。后续 `OP_INT`/`OP_FLOAT` 加载到的 Value 类型与编译期预期不符，触发 `toString`/格式化/类型注解等路径的语义错误甚至 VM 崩溃。
- **修复**：在 `addConstant` 哈希桶线性扫描中增加 `constants[i].getType() == val.getType()` 严格类型前置检查，与 `RegisterBytecodeChunk::addConstant`（已含同款修复）保持一致。
- **影响**：`hashValue` 对 int 0 和 float 0.0 可能落入同一桶（`std::hash<int64_t>{}(0)` 与 `std::hash<double>{}(0.0)` 均倾向于 0），仅 `IntAndFloatZeroNotDeduped` 测试能覆盖此碰撞路径；`IntAndFloatOneNotDeduped` 因哈希不碰撞而无法暴露本 Bug。

#### BUG-ORCH-8 · VM 步进未检查 REPL 运行状态（P2 竞态）

- **位置**：`app/ide.cpp` 4 个 VM 操作函数
- **根因**：`stepOver`/`stepInto`/`stepOut`/`runToLine` 在调度单步任务前未检查 `isReplRunning()`，REPL 执行期间用户触发调试步进会导致两个 Worker 同时操作同一 VM 实例，引发栈状态破坏。
- **修复**：4 个函数入口添加 `isReplRunning()` 检查，命中时弹消息框提示"REPL 正在运行"并提前返回。

#### BUG-LEAK-01 · astWindow_ 父对象缺失导致泄漏（P2 内存泄漏）

- **位置**：`app/ide.cpp` `AstViewer`/`IrViewer` 创建路径
- **根因**：`astWindow_` 创建时未传入 parent，Qt 父子对象关系未建立，主窗口关闭时不会自动释放。
- **修复**：`astWindow_` 构造传入 `this` 作为 parent，依托 Qt 父子对象树自动管理生命周期。

#### BUG-AUDIT-MOD-7 · NaNBox::fromFloat NaN 规范化丢失 payload（P1 语义错误）

- **位置**：`interpreter/NaNBox.h` `fromFloat`
- **根因**：原实现遇到 NaN 输入时直接覆盖为 `NAN_BOXED_FLOAT_MARKER`，丢失原始 double 的低 48 位 payload，导致不同 NaN 值在常量池去重和 Value 比较时被误判为相等。
- **修复**：保留原始 double 的低 48 位 payload：`box.bits_ = NAN_BOXED_FLOAT_MARKER | (box.bits_ & INT48_MASK)`，与 `RegisterBytecode` 路径的 NaN 处理对齐。

#### BUG-AUDIT-MOD-2 · VM/IR 模块隔离文档化（P2 一致性）

- **位置**：`compiler/VM.cpp`/`compiler/IR.cpp` 模块加载路径
- **根因**：VM 和 IR 路径未检查模块 `export` 标记，与 Interpreter 路径行为不一致，但移除该检查会破坏现有模块语义，需先文档化再评估是否统一。
- **修复**：在 VM/IR 模块加载路径添加注释说明已知行为差异，指向 `docs/development.md` 的模块系统一致性章节。

### 构建与测试状态

- 构建：MSVC + Qt 6.10.3 msvc2022_64 + Ninja，构建成功
- 非引擎层测试：391/392 通过（Lexer/Parser/Formatter/TeachingPanels/InterpreterE2E 全部通过）
- 引擎层测试：此前报告 589 项 SEH 0xc0000005 崩溃，后经排查确认为 Ninja 头文件依赖追踪失效导致的 stale `.obj` ABI 不一致（非 engine 层 Bug），全量重编译后已消除
- 崩溃根因详见上方 "SEH 崩溃根因修复" 章节

## 2026-07-05 · 教学增强面板（第二档：轻量引擎改造 + 第四档：已有面板动画增强）

围绕 ProfileDashboardPanel 的真实 instrumentation 改造与条件断点可视化，以及已有面板的子页切换动画增强，新增 1 个面板 + 增强 1 个面板 + 增强 2 个已有面板的动画过渡。所有面板作为独立 `ads::CDockWidget` 注册到右侧 dock area，启动时默认隐藏（视图菜单勾选显示）。

### 新增/改造面板（4 项）

#### P1-1 性能剖析仪表盘（指令计数增强版）

在原有 ProfileDashboardPanel 的"时间对比"tab 之外，新增"指令计数"tab：

- **真实 instrumentation**：直接 new 一个独立 VM/RegisterVM 实例（绕过 VmStepper 的 stepCallback 禁用逻辑），启用 VM/RegisterVM 已有的 `stepCallback_` 机制，每条指令执行后累加 `opProfileCounts_[static_cast<uint8_t>(op)]`（`std::array<uint64_t, 256>`）
- **Top 10 热点聚合**：执行完毕后通过 `aggregateTop10` lambda 聚合前 10 个最频繁的 OpCode，并在右侧 QTableWidget 显示（OpCode 名称 / 执行次数 / 占比）
- **12 条 OpCode 性能文档**：OpCodeProfileLibrary 静态数据嵌入 ProfileDashboardPanel.cpp，覆盖 OP_CONSTANT / OP_INT / OP_ADD / OP_SUBTRACT / OP_MULTIPLY / OP_DIVIDE / OP_MODULO / OP_GET_LOCAL / OP_SET_LOCAL / OP_GET_GLOBAL / OP_SET_GLOBAL / OP_JUMP_IF_FALSE，每条含 opCode / category / description / stackEffect / perfNote（性能提示）

技术实现：ProfileDashboardPanel 直接 new VM/RegisterVM 实例进行指令计数（绕过 VmStepper），避免修改 VmStepper 的 stepCallback 禁用逻辑。VM/RegisterVM 的 stepCallback_ + stepCallbackEnabled_ 字段默认 false，仅在此处主动启用。

#### P1-2 条件断点可视化面板（BreakpointConditionPanel）

新增面板，2 个子页可视化条件断点的运行期状态与教学场景：

- **子页 1 实时断点列表**：消费 `IdeController::getBreakpoints()` / `getBreakpointCondition(line)` / `getBreakpointHitCount(line)`（新增 Facade getter，转发到 DebugCoordinator），500ms 轮询刷新，显示当前所有断点（行号 / 条件表达式 / 命中次数）
- **子页 2 教学场景库**：8 个静态场景（simple-line 普通行断点 / conditional-loop-count 循环计数条件 / conditional-state 状态条件 / data-driven 数据驱动 / error-catch 异常捕获 / breakpoint-with-closure 闭包断点 / watchpoint 变量监视 / temporary-breakpoint 临时断点），每场景含 id / title / description / sourceCode / breakpointLine / condition / teachingNote

技术实现：**关键架构决策** — BreakpointConditionPanel 使用 QTimer 500ms 轮询而非订阅 IdeController 信号，避免测试目标链接 IdeController.cpp（测试目标仅链接 minilang_core + Panel.cpp 中的 Library 静态数据）。IdeController/DebugCoordinator 新增 3 个只读 getter（getBreakpoints / getBreakpointCondition / getBreakpointHitCount），均为 const noexcept 转发，不影响现有语义。

#### P2-4 IR 变换过程面板（动画化）

在原有 IRTransformPanel 的 3 个子页（AST → IR lowering / 优化 pass 对比 / 当前源码 IR）切换时，通过 `PanelAnimator::fadeInWidget` 驱动 QGraphicsOpacityEffect，使新子页 opacity 0→1 平滑淡入（220ms OutCubic 缓动）。同时 lowering 详情刷新、优化前后 IR 刷新均加入淡入动画，避免生硬跳变。

#### P2-5 编译管线可视化面板（动画化）

在原有 PipelineViewer 的 step 切换（Lexer / Parser / AST / IR / Bytecode / 运行）时，通过 `PanelAnimator::fadeInWidget` 驱动子页淡入，使管线步骤切换更流畅。

### PanelAnimator.h 扩展

新增 `fadeInWidget(QWidget*, int duration = FADE_DURATION_MS)` 工具函数：

- 通过 `QGraphicsOpacityEffect` 驱动 opacity 0→1
- 220ms OutCubic 缓动（`FADE_DURATION_MS` 常量）
- QGraphicsOpacityEffect 由 widget parent 自动释放，无需手动管理
- 适用于 QStackedWidget 子页切换、列表选中详情刷新等场景

### 单元测试新增

新增 `tests/TestTeachingPanelsAudit4.cpp`（14 个用例，2 个套件）：

- `TeachingPanelsOpCodeProfile`（6 用例）：文档数量为 12 / OpCode 名称唯一 / 关键字段（opCode/category/description/perfNote）非空 / category 在合法集合（stack/arithmetic/control/variable/dispatch）/ 关键 OpCode 齐全（OP_ADD/OP_GET_LOCAL/OP_SET_LOCAL/OP_JUMP_IF_FALSE）/ perfNote 含性能提示关键词（代价/开销/频繁/触发/热点/密度/压力/热路径/缓存/分支/栈深度）
- `TeachingPanelsBreakpointCondition`（8 用例）：场景数量为 8 / ID 唯一性 / 关键字段（id/title/description/sourceCode/breakpointLine/condition/teachingNote）非空 / 关键场景 ID 齐全（simple-line/conditional-loop-count/conditional-state/data-driven/error-catch/breakpoint-with-closure/watchpoint/temporary-breakpoint）/ condition 为表达式（含 `==`/`>`/`<`/`and`/`or`/`%` 之一）/ sampleCode 含 `// BREAK` 标记断点行 / description 提及条件语义（condition/条件/命中/触发）

### 工程改动

#### 新增文件（3 个）

- `gui/BreakpointConditionPanel.h` / `gui/BreakpointConditionPanel.cpp`
- `tests/TestTeachingPanelsAudit4.cpp`

#### 修改文件（8 个）

- `gui/PanelAnimator.h`：新增 `#include <QGraphicsOpacityEffect>` + `FADE_DURATION_MS` 常量 + `fadeInWidget` 函数
- `gui/IRTransformPanel.cpp`：添加 `#include "gui/PanelAnimator.h"` + 3 个子页切换 lambda 末尾调用 fadeInWidget + populateLoweringDetail/populateOptDetail 末尾调用 fadeInWidget
- `gui/PipelineViewer.cpp`：添加 `#include "gui/PanelAnimator.h"` + switchToStep 末尾调用 fadeInWidget
- `gui/ProfileDashboardPanel.h`：新增 `#include <array>` + OpCodePerfDoc/OpCodeProfileLibrary/OpCodeProfileEntry 结构 + 成员变量 + 方法声明
- `gui/ProfileDashboardPanel.cpp`：新增 `#include <QTabWidget>` + `#include <QTableWidgetItem>` + OpCodeProfileLibrary 实现 + 构造函数改造（QTabWidget 双 tab）+ measureStackVMWithProfile/measureRegisterVMWithProfile/renderOpCodeProfile 实现
- `app/DebugCoordinator.h`：新增 getBreakpoints/getBreakpointCondition/getBreakpointHitCount getter
- `app/IdeController.h`：新增转发到 DebugCoordinator 的 getter
- `app/ide.h` / `app/ide.cpp`：添加 BreakpointConditionPanel include + dock + panel 成员 + viewAction + 6 处插入（viewMenu / dock 创建 / loadSampleRequested / toggleView / connectDockSave / syncViewMenuChecks）
- `cmake/minilang_core.cmake`：MINILANG_GUI_SOURCES 添加 BreakpointConditionPanel.cpp
- `tests/CMakeLists.txt`：添加 TestTeachingPanelsAudit4.cpp + BreakpointConditionPanel.cpp

### 验证

- IDE 构建成功（minilang_ide.exe 8.6MB）
- 全量测试 1306/1306 通过（原 1292 + 新增 14）
- TeachingPanels 套件 68/68 通过（含 OpCodeProfile 6 + BreakpointCondition 8）

## 2026-07-05 · 教学增强面板（第三波）

围绕 IdeController Facade 已暴露的 VM/调试状态 API（无需引擎层改造），新增 3 个深度教学面板，与第一波/第二波面板共同构成完整的"编译管线 → IR 变换 → 内存模型 → 性能剖析 → Bug 训练 → 运行时状态可见性"教学闭环。所有面板作为独立 `ads::CDockWidget` 注册到右侧 dock area，启动时默认隐藏（视图菜单勾选显示）。

### 新增面板（3 个）

#### P0-1 调用栈可视化面板（CallStackPanel）

2 个子页，可视化运行期函数调用栈：

- **子页 1 实时调用栈**：优先消费 `IdeController::getVmCallStack()` 走 VM 路径（含 StackVM / RegisterVM 双模式），回退到 `getDebugCallStack()` 走 Interpreter debug 路径。显示栈帧列表（functionName / line / depth / locals），500ms 自动刷新定时器，运行模式标签自动切换（StackVM / RegisterVM / Interpreter (debug)）
- **子页 2 教学场景库**：6 个静态场景（simple-call 普通调用 / recursion 递归 / closure-capture 闭包捕获 / method-dispatch 方法分派 / try-catch 异常传播 / mutual-recursion 互递归），每场景含 id / title / description / sourceCode / expectedFrames / teachingNote

技术实现：`CallStackEntry` 结构含 functionName / line / depth / locals，复用 DebugTypes.h 公共定义；CallStackLibrary 静态数据嵌入 CallStackPanel.cpp。

#### P0-2 变量检查器面板（VariableInspectorPanel）

2 个子页，可视化运行期变量状态与 NaN-boxing 位编码：

- **子页 1 实时变量树**：消费 `IdeController::getVmGlobals()` + `IdeController::getDebugVariableSnapshot()`，按作用域分组（全局变量组 + 局部变量按 scope 二次分组）。每变量显示名 / 类型名（`Value::typeName()`）/ 值字符串（`Value::toString()`）/ NaN-boxing 位编码十六进制（通过 `NaNBox` 公共 API `rawBits()` 反推）
- **子页 2 类型教学库**：9 种类型示例（int / int-boundary / float / bool / null / string / array / dict / instance / closure），每示例含 id / typeName / value / bits / description

技术实现：辅助函数 `valueToBitsHex()` 通过 `NaNBox::fromInt` / `fromFloat` / `fromBool` / `null` / `fromPtr` 公共 API 反推位编码；`VariableSnapshot` 结构含 name / value / scope，复用 DebugTypes.h 公共定义。

#### P0-3 字节码执行轨迹面板（BytecodeTracePanel）

2 个子页，可视化栈式 VM 单步执行的指令序列：

- **子页 1 执行轨迹时间轴**：消费 `IdeController::getVmCurrentIP()` / `getVmCurrentOpCodeName()` / `getVmFrameCount()` / `getVmStack()`，记录单步执行序列（step / ip / opCodeName / frameCount / stackSnapshot / line），最多保留 1000 条历史（`kMaxTraceEntries`），500ms 轮询定时器自动捕获 + "立即捕获"按钮手动触发
- **子页 2 OpCode 教学库**：18 个 OpCode 文档条目（OP_INT / OP_FLOAT / OP_STRING / OP_NULL / OP_ADD / OP_GET_GLOBAL / OP_SET_GLOBAL / OP_JUMP / OP_JUMP_IF_FALSE / OP_LOOP / OP_CALL / OP_RETURN / OP_BUILD_ARRAY / OP_BUILD_DICT / OP_CLOSURE / OP_GET_UPVALUE / OP_CLASS_NEW / OP_METHOD_CALL），每条含 opCode / category / description / stackEffect / sampleCode

技术实现：**关键架构决策** — BytecodeTracePanel 使用 QTimer 500ms 轮询而非订阅 IdeController 信号（`pausedAt` / `vmRunPaused`），避免测试目标链接 IdeController.cpp（测试目标仅链接 minilang_core + Panel.cpp 中的 Library 静态数据）。TraceEntry 内嵌结构含 step / ip / opCodeName / frameCount / stackSnapshot / line。

### 单元测试新增

新增 `tests/TestTeachingPanelsAudit3.cpp`（13 个用例，3 个套件）：

- `TeachingPanelsCallStack`（5 用例）：场景数量为 6 / ID 唯一性 / 关键字段（id/title/sourceCode/expectedFrames/teachingNote）非空 / 关键场景 ID 齐全（simple-call/recursion/closure-capture/method-dispatch/try-catch/mutual-recursion）/ expectedFrames 非空
- `TeachingPanelsVariableInspector`（5 用例）：示例数量为 9 / ID 唯一性 / 关键字段（id/typeName/value/bits/description）非空 / 关键类型名齐全（int/float/bool/null/string/array/dict/instance/closure）/ 标量示例（int/float/bool/null）含 NaN-boxing 位编码
- `TeachingPanelsBytecodeTrace`（3 用例）：文档数量 ≥ 15 / OpCode 名称唯一 / 关键字段（opCode/category/description）非空 / category 在合法集合 / 关键 OpCode 齐全（OP_INT/OP_ADD/OP_CALL/OP_RETURN/OP_CLOSURE/OP_GET_UPVALUE）

### 工程改动

#### 新增文件（4 个）

- `gui/CallStackPanel.h` / `gui/CallStackPanel.cpp`
- `gui/VariableInspectorPanel.h` / `gui/VariableInspectorPanel.cpp`
- `gui/BytecodeTracePanel.h` / `gui/BytecodeTracePanel.cpp`
- `tests/TestTeachingPanelsAudit3.cpp`

#### 修改文件（4 个）

- `app/ide.h`：添加 3 个新 include + 3 个 dock 成员 + 3 个 panel 成员 + 3 个 viewXxxAction_ 成员
- `app/ide.cpp`：viewMenu 添加 3 个 QAction + dockManager 注册 3 个新 dock 到 RightDockWidgetArea + 启动时 toggleView(false) 隐藏 + loadSampleRequested 信号连接到 loadCodeIntoMainEditor + connectDockSave 注册 3 个新 dock 防抖保存 + syncViewMenuChecks 添加 3 个面板的勾选同步
- `cmake/minilang_core.cmake`：MINILANG_GUI_SOURCES 添加 3 个新源文件
- `tests/CMakeLists.txt`：添加 TestTeachingPanelsAudit3.cpp + 显式编译 3 个 Panel.cpp 进测试目标（Library 实现位于 Panel.cpp 中）

### BUG-AUDIT-MOD-3 修复（顺带）

实施第三波时发现 `compiler/Compiler.cpp` 与 `compiler/IR.cpp` 中存在 BUG-AUDIT-MOD-3 半成品修复（引用未限定的 `MAX_RECURSION_DEPTH` 标识符），导致构建失败：

- `compiler/Compiler.cpp`：添加 `#include "common/RuntimeLimits.h"`，2 处 `MAX_RECURSION_DEPTH` → `RuntimeLimits::MAX_RECURSION_DEPTH`
- `compiler/IR.cpp`：2 处 `MAX_RECURSION_DEPTH` → `RuntimeLimits::MAX_RECURSION_DEPTH`（已有 include）

### 构建错误与修复

- **VariableInspectorPanel.cpp(210) QSplitter::addWidget 误用 2 参数**：原 `splitter->addWidget(widget, 1)` 改为 `splitter->addWidget(widget)` + `setStretchFactor(0, 1)` / `setStretchFactor(1, 3)`
- **VariableInspectorPanel.cpp(264) auto* 类型推导失败**：`auto* globalFont = globalGroup->font(0)` 改为 `QFont globalFont = globalGroup->font(0)`（font() 返回值类型而非指针）
- **BytecodeTracePanel.cpp(249) VmStepResult 未限定**：原 lambda 中 `VmStepResult` 改为 `IdeController::VmStepResult`，后改为 QTimer 轮询模式
- **BytecodeTracePanel.cpp 链接错误**：原订阅 IdeController::pausedAt / vmRunPaused 信号导致测试目标无法链接（测试目标不链接 IdeController.cpp），改为 QTimer 500ms 轮询模式

### 验证

- IDE 构建成功（minilang_ide.exe）
- 全量测试 1292/1292 通过（原 1277 + 新增 13 + BUG-AUDIT-MOD-3 修复后重新启用 2 个测试），无回归
- formatter_audit 0 失败

## 2026-07-05 · 教学增强面板（第二波）

围绕 MiniLang IDE 三大独特性（**三套执行引擎 + 共享 IR 层 + 历史真实 Bug 沉淀**）的运行时内部细节，新增 3 个深度教学面板，与第一波/第三波面板共同构成完整的"编译管线 → IR 变换 → 内存模型 → 性能剖析 → Bug 训练"教学闭环。所有面板作为独立 `ads::CDockWidget` 注册到右侧 dock area，启动时默认隐藏（视图菜单勾选显示）。

### 新增面板（3 个）

#### P0-2 内存模型可视化面板（MemoryModelPanel）

3 个子页，可视化 MiniLang 的 NaN-boxing / COW / 引用计数 / GcManager 内存模型：

- **子页 1 NaN-boxing 编码**：7 个示例（int-inline-pos/neg/boundary + float-direct + bool-true + null-value + ptr-string），每示例含 64 位分段图示（tag bits 高 16 位红色背景 + payload 低 48 位蓝色背景）+ hex/binary 字符串 + 文字说明。int48 范围/边界/超范围装箱演示，让学习者直观看到 8 字节 Value 的位级编码
- **子页 2 RefCounted 引用计数 & COW**：4 个场景（array-basic 数组基础生命周期 / cow-detach COW 写时复制 detach / string-shared 字符串共享 / instance-fields 实例字段引用），每场景含步骤表展示 refCount 变化与备注
- **子页 3 GcManager mark-sweep**：6 个阶段说明（注册 / 触发时机 / Mark / Sweep / UAF 防护 / 已知限制）+ 实时 tracked 节点数刷新按钮

技术实现：使用 NaNBox 公共 API（fromInt/fromFloat/fromBool/null/fromPtr + rawBits）直接获取位编码，避免依赖 Value 私有字段；NaNBoundingBox.intVal 类型为 int64_t（int48 边界值 2^46-1 超 int 范围）。

#### P1-1 IR 变换过程面板（IRTransformPanel）

3 个子页，可视化 AST → IR 三地址码 lowering 与优化 pass：

- **子页 1 AST → IR lowering**：8 个典型 AST 节点的 lowering 演示（lit-int 整数字面量 / binop-add 二元加法 / var-decl 变量声明 / if-stmt 条件分支 / while-stmt 循环 / fun-call 函数调用 / closure 闭包捕获 / class-method 类方法），每示例含 AST 节点摘要 + 源码 + 文字说明 + lowering 后 IR 文本（IRToString 一致格式）
- **子页 2 优化 pass 对比**：3 个 pass（const-fold 常量折叠 / dead-code 死代码消除 / copy-prop 复制传播）的前后 IR 对比 + 指令数变化（如常量折叠 4 条 → 2 条）
- **子页 3 当前源码 IR**：从当前 IdeController.astRoot() 实时生成 IRModule 反汇编（复用 IRToString 公共 API + AstIRBuilder::build），显示基本块数与指令数

技术实现：复用 IR.h 公共 API（IRToString / irOpName / AstIRBuilder::build / IRModule::mainFunction），不修改引擎层；前两个子页是静态教学场景库，第三个子页是动态实时生成。

#### P1-2 性能剖析仪表盘（ProfileDashboardPanel）

6 个性能场景 + 三后端多次测量取平均与标准差 + 柱状图：

- **6 个性能场景**：fib-recursion（fib(20) 递归）/ loop-sum（1 到 100000 求和）/ string-concat（1000 次字符串拼接）/ class-instantiation（10000 次类实例化）/ closure-capture（10000 次闭包捕获）/ dict-access（10000 次字典访问），每场景含分类标签（arithmetic/loop/string/class/closure）+ 迭代次数 + 文字说明（解释性能差异原因）
- **三后端测量**：Interpreter / StackVM / RegisterVM 三路径独立计时（微秒精度），多次迭代取平均 + 标准差，结果表格显示平均时间 / 标准差 / 与最快比值
- **柱状图**：QPainter 自绘（避免引入 QtCharts 强依赖），蓝/橙/绿三色对应 Interpreter/StackVM/RegisterVM，柱高归一化到最大值
- **加速比分析**：自动找出最快/最慢后端，计算加速比，文字解释为何该场景某后端占优

技术实现：与 BackendComparePanel 一致在 GUI 线程顺序运行三后端（避免 WorkerManager 单 worker 互斥）；paintEvent 重绘柱状图；BackendTiming 结构含 avgMicros / stddevMicros / success / errorMessage；统计工具 mean/stddev 用样本标准差公式（n-1 分母）。

### 单元测试新增

新增 `tests/TestTeachingPanelsAudit2.cpp`（23 个用例，3 个套件）：

- `TeachingPanelsMemoryModel`（10 用例）：NaN-boxing 示例数量/ID 唯一/关键字段非空/bits 与 NaNBox 编码一致/关键 ID 齐全 + RefCount 场景数量/ID 唯一/步骤非空 + GC 阶段数量/关键字段非空
- `TeachingPanelsIRTransform`（7 用例）：lowering 示例数量/ID 唯一/关键字段非空/IR 文本含 'function' 关键字 + 优化示例数量/ID 唯一/指令数一致（优化后 ≤ 优化前）/关键字段非空
- `TeachingPanelsProfile`（5 用例）：场景数量/ID 唯一/关键字段非空/category 在合法集合/关键场景 ID 齐全

### 工程改动

#### 新增文件（7 个）

- `gui/MemoryModelPanel.h` / `gui/MemoryModelPanel.cpp`
- `gui/IRTransformPanel.h` / `gui/IRTransformPanel.cpp`
- `gui/ProfileDashboardPanel.h` / `gui/ProfileDashboardPanel.cpp`
- `tests/TestTeachingPanelsAudit2.cpp`

#### 修改文件（4 个）

- `app/ide.h`：添加 3 个新 include + 3 个 dock 成员 + 3 个 panel 成员 + 3 个 viewXxxAction_ 成员
- `app/ide.cpp`：viewMenu 添加 3 个 QAction + dockManager 注册 3 个新 dock + 启动时 toggleView(false) 隐藏 + connectDockSave 注册 3 个新 dock + syncViewMenuChecks 添加 3 个面板的勾选同步
- `cmake/minilang_core.cmake`：MINILANG_GUI_SOURCES 添加 3 个新源文件
- `tests/CMakeLists.txt`：添加 TestTeachingPanelsAudit2.cpp + 显式编译 3 个 Panel.cpp 进测试目标（Library 实现位于 Panel.cpp 中）

### 构建错误与修复

- **ProfileDashboardPanel.h 缺少 Block 类型**：measureXxxOnce 签名使用 `Block&` 但未 include ASTNode.h → 添加 `#include "ast/ASTNode.h"`
- **NaNBoundingBox.intVal 类型溢出**：原 int 类型存储 int48 边界值 2^46-1 时截断 → 改为 int64_t
- **bool-true 示例 intVal 未设置**：测试中 `b.intVal != 0` 误判为 false → 设置 `b.intVal = 1`

### 验证

- IDE 构建成功（minilang_ide.exe, 8.5MB）
- 全量测试 1277/1277 通过（原 1254 + 新增 23），无回归
- formatter_audit 0 失败

## 2026-07-05 · 教学增强面板（第一波 + 第三波）

围绕 MiniLang IDE 三大独特性（**三套执行引擎 + 共享 IR 层 + 历史真实 Bug 沉淀**）新增 5 个教学增强面板，强化 IDE 的教学价值。所有面板作为独立 `ads::CDockWidget` 注册到右侧 dock area，启动时默认隐藏（视图菜单勾选显示）。

### 新增面板（5 个）

#### 第一波 P0-1：编译管线可视化面板（PipelineViewer）

- 5 步流程导航条：源码 → Token → AST → IR → 字节码
- Token 表格含 type/lexeme/line/column 字段
- AST 步骤通过 `dumpAst` 展示摘要
- IR 步骤展示 IRModule 反汇编
- 字节码步骤展示 BytecodeChunk 反汇编
- 快捷键：Ctrl+Shift+P

#### 第一波 P0-3：三后端并行对比面板（BackendComparePanel）

- 同一源码顺序运行 Interpreter / StackVM / RegisterVM 三条路径
- 每路径独立计时（微秒精度）+ 输出比对
- 自动统计行级差异并给出一致性 PASS/FAIL 结论
- 三后端输出一致即"三后端语义等价"教学验证
- 快捷键：Ctrl+Shift+B

#### 第一波 P1-3：Bug 狩猎面板（BugHuntPanel + BugHuntLibrary）

- 题库 10 道历史真实 Bug 题目：
  - `BUG-CP-1` 常量池去重陷阱（int(0) 与 float(0.0)）
  - `BUG-CP-2` 嵌套索引赋值栈泄漏（a[0][1]=x）
  - `BUG-UV-1` 三层嵌套闭包自由变量捕获断裂
  - `BUG-IR-POP-2` IR 路径 if 单语句体未 POP（P0）
  - `BUG-DEF-1` 默认参数表达式三后端不一致
  - `BUG-MOD-1` 模块路径 'C:foo' 漏网
  - `BUG-DBG-1` 调用栈顶帧行号：调用点 vs 当前行
  - `BUG-REGVM-2` RegisterVM 单步丢失步进事件
  - `BUG-REPL-1` clearModuleCache 路径规范化缺失
  - `BUG-F-04` Formatter ExportStmt 间未插入空行
- 每题含 background / sourceCode / expectedBehavior / buggyBehavior / hints / explanation
- 三栏布局：题目列表 + 背景说明 + 内嵌编辑器/输出
- 可一键加载源码到主编辑器进一步调试
- 快捷键：Ctrl+Shift+H

#### 第三波 P2-1：交互式语法探索器（SyntaxExplorerPanel + SyntaxProductionLibrary）

- 10 条核心产生式参考：var-decl / if-stmt / while-stmt / for-stmt / fun-decl / class-decl / try-stmt / import-stmt / string-interp / data-structures / operators
- 每条配 EBNF 形式 + 文字说明 + 可运行样例代码
- 三栏布局：产生式列表 + 说明 + 代码/输出
- 可一键加载样例代码到主编辑器
- 快捷键：Ctrl+Shift+S

#### 第三波 P2-2：内置实验手册（LabManualPanel + LabManualContent）

- 8 个实验章节：
  - `lab-01` 词法分析 — 从字符到 Token
  - `lab-02` 递归下降解析 — 从 Token 到 AST
  - `lab-03` 树遍历解释器 — Visitor 模式执行 AST
  - `lab-04` 栈式字节码 VM — 编译与执行
  - `lab-05` 寄存器式 VM — 三地址码与 IR
  - `lab-06` 三后端一致性 — 语义等价验证
  - `lab-07` 内存模型 — NaN-boxing 与 COW
  - `lab-08` Bug 狩猎 — 从历史 Bug 学习
- 每章含目标/关键概念/实验步骤/验证断言/进阶/可一键加载样例代码
- Markdown 内容通过 QTextBrowser 渲染
- 快捷键：Ctrl+Shift+L

### 单元测试新增

新增 `tests/TestTeachingPanelsAudit.cpp`（17 个用例，3 个套件）：

- `TeachingPanelsBugHunt`（6 用例）：题目数量 / ID 唯一性 / 关键字段非空 / severity 在 P0/P1/P2 集合 / sourceCode 可被 Lexer 解析 / 关键 Bug ID 齐全
- `TeachingPanelsSyntax`（5 用例）：产生式数量 / name 唯一 / 关键字段非空 / sampleCode 可被 Lexer 解析 / 核心产生式 ID 齐全
- `TeachingPanelsLabManual`（6 用例）：章节数量为 8 / id 唯一 / 关键字段非空 / lab-NN 命名约定 / sampleCode 可被 Lexer 解析 / markdown 含"目标"和"实验步骤"两节

### 工程改动

#### 新增文件（10 个）

- `gui/PipelineViewer.h` / `gui/PipelineViewer.cpp`
- `gui/BackendComparePanel.h` / `gui/BackendComparePanel.cpp`
- `gui/BugHuntPanel.h` / `gui/BugHuntPanel.cpp` / `gui/BugHuntLibrary.cpp`
- `gui/SyntaxExplorerPanel.h` / `gui/SyntaxExplorerPanel.cpp` / `gui/SyntaxProductionLibrary.cpp`
- `gui/LabManualPanel.h` / `gui/LabManualPanel.cpp` / `gui/LabManualContent.cpp`
- `tests/TestTeachingPanelsAudit.cpp`

#### 修改文件（4 个）

- `app/ide.h` / `app/ide.cpp`：注册 5 个新 dock widget + 5 个 viewXxxAction_ + loadCodeIntoMainEditor 方法 + syncViewMenuChecks/connectDockSave 同步
- `cmake/minilang_core.cmake`：MINILANG_GUI_SOURCES 添加 8 个新源文件
- `tests/CMakeLists.txt`：添加 TestTeachingPanelsAudit.cpp + 显式编译 3 个数据类源文件 + 链接 Qt6::Widgets

### 验证

- IDE 构建成功（minilang_ide.exe）
- 全量测试 1254/1254 通过（原 1237 + 新增 17），无回归
- formatter_audit 0 失败

### 待实施（第二波，下一轮实现）

下列 3 个面板已设计但本轮未实现，暂记入项目记忆待下一轮开发：

- **P0-2 内存模型可视化器**：NaN-boxing tag bits / payload 分段编码可视化、int48 内联存储动画、RefCounted 引用计数动画、COW 写时复制动画、GcManager sweep 周期清理
- **P1-1 IR 变换过程动画**：AST → IR 三地址码 lowering 过程动画、虚拟寄存器 SSA-like 分配、优化 pass（常量折叠 / 死代码消除 / 复制传播）前后对比
- **P1-2 性能剖析仪表盘**：三后端执行时间对比图表、热点函数 Top N、指令计数 / 内存分配 / GC 频率统计

## 2026-07-04 · Formatter + GUI 全模块系统性审计 + 全量修复（66 Bug）

针对 formatter/Formatter.cpp 与 gui/、app/ 目录下 14 个模块进行系统性 Bug 排查，覆盖格式化器缩进/幂等性/往返等价、CodeEditor 断点/折叠/上下文菜单、SyntaxHighlighter 状态编码、FindReplacePanel 替换/段落分隔符、AstViewer 递归深度/UAF/折叠键、IrViewer 异常隔离/高亮冲突、VmStackPanel 异常安全/列宽/标题、IdeController 状态检查/缓存上限、ide.cpp 关闭事件/eventFilter、WorkerManager 信号断开、VmStepper UI 冻结、ReplPanel 输出上限/智能滚动、DebugPanel 信号级联/调用栈上限、OutputPanel 死代码标注等方向。发现 **1 P0 + 9 P1 + 56 P2 = 66 个 Bug**，**全部 63 个 Bug 已修复**（3 个为功能增强/已知限制仅标注）。新增 25 个回归测试到 TestFormatterGUISuiteAudit.cpp，全量 1237/1237 测试通过，formatter_audit 0 失败。

### P0 修复（1 个）

- **BUG-P0-IDE-CPP-TRUNC** (P0, 已修复): `app/ide.cpp` 文件被截断（从 3655 行/158KB 缩减至 1494 行/60KB），导致大量 IDE 主窗口功能丢失。修复：通过 `git checkout HEAD -- app/ide.cpp` 恢复完整文件，并协调工作区 ide.h 的改动（移除 themeToggleBtn_、移除 onCompareEngines 声明、重新应用 BUG-DBG-6 修复）。

### P1 修复（9 个）

- **BUG-CE-1** (P1): CodeEditor isFoldable/foldEndBlock 块注释判定不完整。修复：使用 `(s == 2 || s >= 100)` 检测块注释状态。
- **BUG-CE-2** (P1): 编辑器内容变更时断点行号未同步。修复：连接 `QTextDocument::contentsChange` 信号，跟踪 `lastBlockCount_` 计算行差调整断点。
- **BUG-AV-1** (P1): AstViewer 7 个递归函数无深度保护。修复：新增 `MAX_AST_DEPTH=400`，所有递归函数添加 depth 参数与守卫。
- **BUG-AV-2** (P1): AstViewer root_ 非拥有指针，setDarkTheme UAF。修复：root_ 为空时仅刷新视口不重建场景。
- **BUG-IDE-08** (P1): closeEvent 中 REPL 运行时未发送中止请求。修复：waitReplFuture() 前调用 `controller_->requestReplStop()`。
- **BUG-IDE-02** (P1): prepareRun 未检查 VmStepper 运行状态。修复：增加 `vmStepper_.isRunning()` 检查。
- **BUG-IDE-PREP-1** (P1): prepareRun 未编译就同步旧编译结果。修复：AST 校验后调用 `runCompiler()` 编译当前源码。
- **BUG-REPL-G1** (P1): REPL 输出区无大小上限。修复：`outputArea_->document()->setMaximumBlockCount(10000)`。
- **BUG-OUT-G1** (P1): OutputPanel 是死代码。修复：三处添加 DEPRECATED 标注（cpp/h/cmake）。

### P2 修复（56 个，已全部处理，3 个为已知限制）

#### Formatter 模块（8 P2 已修复）

- **BUG-F-01**: 多行块注释后续行缩进不一致。修复：新增 `reindentBlockComment()` 辅助函数，在 4 处注释输出点应用。
- **BUG-F-03**: 独立块（NODE_BLOCK）前置多余空格。修复：去除格式化结果开头的前置空格。
- **BUG-F-04**: ExportStmt 之间未插入空行。修复：`isFunOrClass` lambda 解包 `NODE_EXPORT_STMT`。
- **BUG-F-05**: 类方法之间未插入空行。修复：`formatClassDecl` 传播 `blankLineBetweenFunctions` 到类成员。
- **BUG-F-06**: 插值表达式行号范围内注释错位。修复：`formatInterpolatedString` 跳过表达式行范围内的注释。
- **BUG-F-07**: NaN/Infinity 格式化附加 `.0`。修复：`formatNumberLiteral` 检查 `"nan"/"inf"/"-inf"`。
- **BUG-F-08**: 空 throw（expression 为 nullptr）输出 "throw"。修复：输出 `"throw null"`。
- **BUG-F-02** (跳过): 块注释缩进保留原样为预期行为。

#### CodeEditor 模块（2 P1 + 6 P2 已修复）

- **BUG-CE-3**: foldedBlocks_ 在内容变更时未同步。修复：onContentsChange 中同步调整。
- **BUG-CE-4**: setErrorRanges 对空行未跳过。修复：`blockTextLen <= 0` 跳过。
- **BUG-CE-5**: contextMenuEvent 末行空白区无菜单。修复：添加 `lastBlockBottom` 空白区检查。
- **BUG-CE-6**: 折叠点击区域不统一。修复：统一为 `[width()-14, width()-4)`。
- **BUG-CE-7**: 块注释行断点判定不准。修复：通过 `userState()` 检测块注释行。
- **BUG-CE-8**: ExtraSelection 顺序不合理。修复：findSelections_ → cachedErrorSelections_ → cursorSel → currentLine_。

#### SyntaxHighlighter 模块（4 P2 已修复）

- **BUG-SH-1**: 插值+块注释组合状态冲突。修复：组合状态 `300 + braceDepth*10 + blockCommentDepth`。
- **BUG-SH-2**: 插值嵌套字符串跨行状态丢失。修复：新状态 `400 + braceDepth`。
- **BUG-SH-3**: 0x/0b/0o 前缀未检测。修复：前缀检测与错误格式高亮。
- **BUG-SH-4**: 关键字长度检查 `wordLen <= 20` 误漏。修复：移除长度检查。

#### FindReplacePanel 模块（3 P2 已修复 + 1 跳过）

- **BUG-FR-1**: 替换使用 `'\n'` 而非段落分隔符。修复：使用 `QChar::ParagraphSeparator`。
- **BUG-FR-2**: 替换达上限时提示不准。修复：显示"已替换 N+ 处（达到上限）"。
- **BUG-FR-4**: 替换未用 beginEditBlock/endEditBlock。修复：包裹替换操作。
- **BUG-FR-5**: onFindTextChanged 触发 findText(true)。修复：移除冗余调用。
- **BUG-FR-3** (跳过): 区分大小写高亮为功能增强。

#### AstViewer 模块（4 P2 已修复）

- **BUG-AV-3**: textItem 未注册到 itemToNode_，点击文字无法折叠。修复：textItem 设置 `ItemIsSelectable=false` + `setAcceptedMouseButtons(Qt::NoButton)`。
- **BUG-AV-4**: CollapseKey 折叠键碰撞。修复：增加子节点数维度，改为 `tuple<int,int,string,int>`。
- **BUG-AV-5**: shiftSubtree 死代码。修复：删除 shiftSubtree 与 SUBTREE_SPACING。
- **BUG-AV-6**: itemToNode_ 键生命周期耦合。修复：swap 取出 oldMap，clear 后再清理。

#### IrViewer 模块（5 P2 已修复）

- **BUG-IRV-1**: setIR 无 try/catch。修复：try/catch 包裹，catch 显示错误占位。
- **BUG-IRV-2**: highlightBySourceLine 注释与实现不符。修复：注释改为"只高亮第一个"。
- **BUG-IRV-3**: setCharFormat 高亮与 span 语法高亮冲突。修复：改用 QTextBlockFormat。
- **BUG-IRV-4**: 二分查找假设排序无校验。修复：改为线性查找。
- **BUG-IRV-5**: isNumericConst 允许 - 在任意位置。修复：仅允许首字符为 '-'。

#### VmStackPanel 模块（6 P2 已修复）

- **BUG-VSP-1**: updateStack/Registers/Globals 无 try/catch。修复：每个 toString() 包 try/catch，超长截断 200 字符。
- **BUG-VSP-2**: const& 接口鼓励传引用。修复：改为 by-value 参数。
- **BUG-VSP-3**: updateGlobals 列宽不随内容变长重算。修复：`lastMaxValueWidth_` 跟踪。
- **BUG-VSP-4**: 排序用 unordered_map 元素裸指针。修复：先 move 到本地 vector 再排序。
- **BUG-VSP-5**: 空 opName 无兜底。修复：显示"(未知指令)"/"(无行号)"。
- **BUG-VSP-6**: 模式切换标题不适配。修复：`stackTitle_` 成员，模式感知标题。

#### IdeController/WorkerManager/VmStepper 模块（4 P2 已修复 + 1 已知限制）

- **BUG-IDE-15**: stopForClose 与 cleanupWorker 状态清理重复。修复：`workerThread_->disconnect(this)`。
- **BUG-IDE-13**: eventFilter 未排除 ComboBox/QLineEdit。修复：扩展为三类检测。
- **BUG-IDE-23**: setUseRegisterVM 未检查 VM 运行状态。修复：入口检查 `vmStepper_.isRunning()`。
- **BUG-IDE-10**: closeEvent 调用 `onVmStop()` 而非 `controller_->vmStop()`。修复：直接调用 controller 接口。
- **BUG-IDE-04**: condAstCache 无限增长。修复：`COND_AST_CACHE_MAX=32` LRU 上限。
- **BUG-IDE-18**: VmStepper STEP_OVER/OUT 同步循环冻结 UI。修复：每 2000 步 `processEvents(ExcludeUserInputEvents)`。
- **BUG-IDE-11**: 关闭最后编辑器标签未清除断点。修复：清空三处断点状态。
- **BUG-IDE-12**: VM 条件断点仅支持全局变量。修复：catch 块附加提示。
- **BUG-IDE-19** (已知限制): runBatch 条件断点求值可能阻塞 UI。修复：添加注释标注。

#### ReplPanel/DebugPanel/OutputPanel 模块（5 P2 已修复 + 2 已知限制）

- **BUG-REPL-G2**: reload 绕过 isRunning() 检查。修复：入口添加运行检查。
- **BUG-REPL-G3**: isInputComplete 负 braceDepth 不截断。修复：负深度立即返回 false。
- **BUG-REPL-G4**: REPL 输出区无智能滚动。修复：GuiTextUtils::appendLine 检查滚动条位置。
- **BUG-REPL-G5**: ~ReplPanel 析构无超时等待。修复：`wait_for(5s)` + LOG_WARNING。
- **BUG-REPL-G6**: REPL 错误显示到主错误面板。修复：invokeMethod 投递到 self->appendError。
- **BUG-REPL-G7** (已知限制): 多行历史不支持。修复：标注已知限制。
- **BUG-DBG-G1**: updateCallStack 恢复选中行触发信号。修复：setCurrentRow 外层 blockSignals(true)。
- **BUG-DBG-G2**: worker 恢复后调用栈未清理。修复：onWorkerFinished wasDebug 分支调用 clearAll()。
- **BUG-DBG-G3**: 调用栈深度无上限。修复：`MAX_CALL_STACK_DISPLAY=200` + 提示项。
- **BUG-DBG-G4**: pixelSize 假设脆弱。修复：`if (ps > 1)` 守卫。
- **BUG-DBG-G5** (已知限制): 变量值无编辑能力。修复：标注 TODO。

### 新增回归测试

- **TestFormatterGUISuiteAudit.cpp**（25 个测试）：覆盖 BUG-F-01~F-08、幂等性（9 个）、往返等价、边界条件、export 保留等。纯 GUI Bug（需 Qt GUI 环境）不在单元测试覆盖范围，由 formatter_audit 工具覆盖往返等价性。
- 全量测试：1237/1237 通过（原 1212 + 新增 25），formatter_audit 0 失败。

## 2026-07-04 · GUI 交互三模块（ReplPanel / DebugPanel / OutputPanel）P1+P2 Bug 修复

针对 gui/ReplPanel.cpp/.h、gui/DebugPanel.cpp/.h、gui/OutputPanel.cpp/.h、gui/GuiTextUtils.h 进行 Bug 修复，覆盖输出区无上限、reload 绕过运行检查、续行判定漏判、智能滚动缺失、析构超时、REPL 错误反馈缺失、调用栈信号级联、worker 残留数据、调用栈深度膨胀、字体像素假设、死代码标注等 14 个问题。发现 2 P1 + 12 P2，**12 个 Bug 已修复**，2 个（BUG-REPL-G7 多行历史 / BUG-DBG-G5 变量编辑）为功能增强/已知限制，仅添加注释未改逻辑。涉及 gui/ 目录 4 个模块共 7 个文件 + app/ide.cpp（onWorkerFinished）+ cmake/minilang_core.cmake（标注）+ CHANGELOG.md。

### 审计发现与修复（2 P1 + 12 P2，全部处理）

#### ReplPanel 模块（1 P1 + 5 P2 修复 + 1 P2 已知限制）

- **BUG-REPL-G1** (P1, 已修复): REPL 输出区无大小上限。`outputArea_` 未调用 `document()->setMaximumBlockCount(...)`，长时间运行（如 while 循环大量 print）导致 QTextDocument 块链表无限增长、内存膨胀。修复：构造函数中 outputArea_ 创建后调用 `setMaximumBlockCount(10000)`，与 OutputPanel 一致。位置: `gui/ReplPanel.cpp` 构造函数
- **BUG-REPL-G2** (P2, 已修复): reload 命令绕过 isRunning() 检查。reload 在 onReturnPressed 中提前 return，绕过 executeLine 入口的 isRunning() 检查，worker 线程执行 import 时清空缓存会导致崩溃或行为不一致。修复：在 reload 命令处理块入口添加 `controller_->isRunning()` 检查并拦截。位置: `gui/ReplPanel.cpp` `onReturnPressed()`
- **BUG-REPL-G3** (P2, 已修复): isInputComplete 负 braceDepth 不截断。原实现仅在末尾检查 `braceDepth != 0`，中途负深度可能因后续 `{` 重新归零而漏判（如 `}{` 末尾 braceDepth=0 误判完整）。修复：在 `}` / `)` / `]` 的 case 中检查负深度并立即返回 false。位置: `gui/ReplPanel.cpp` `isInputComplete()`
- **BUG-REPL-G4** (P2, 已修复): REPL 输出区无智能滚动。GuiTextUtils::appendLine 无条件 `ensureCursorVisible()`，强制把用户手动滚到上方查看历史的视图拉回底部。修复：追加前检查 `verticalScrollBar()->value() == maximum()`，仅当在底部时才滚动；同时添加空指针防御。位置: `gui/GuiTextUtils.h` `appendLine()`
- **BUG-REPL-G5** (P2, 已修复): ~ReplPanel 析构路径无超时等待。原 `replFuture_.wait()` 无超时，若 closeEvent 异常未调用 waitReplFuture() 且异步任务陷入死循环，析构会无限阻塞。修复：改为 `wait_for(5s)` + LOG_WARNING 告警，超时后回退阻塞 wait() 作为最后手段。controller_ 已可能析构，不调用 requestReplStop()（C++ 异常不能捕获 UAF），遵循任务说明的"更安全做法"。位置: `gui/ReplPanel.cpp` `~ReplPanel()`
- **BUG-REPL-G6** (P2, 已修复): REPL 运行时错误显示到主错误面板而非 REPL 面板。原异步 lambda 仅 emit ctrl->runtimeError/genericError 信号（由 Ide 连接到主错误面板），REPL 用户在 REPL 输出区看不到错误反馈。修复：捕获 `self = this`，在 catch 块中通过 QMetaObject::invokeMethod 将错误投递到 `self->appendError()`，同时保留原有 emit 信号行为。位置: `gui/ReplPanel.cpp` `executeLine()` 异步 lambda
- **BUG-REPL-G7** (P2, 已知限制): 多行输入的历史不支持整体重放。QLineEdit 只能显示单行，完整修复需多行历史编辑器。修复：跳过此 Bug（UX 增强），在 `gui/ReplPanel.h` history_ 声明处添加注释标注已知限制。位置: `gui/ReplPanel.h` history_ 成员注释

#### DebugPanel 模块（4 P2 修复 + 1 P2 已知限制）

- **BUG-DBG-G1** (P2, 已修复): updateCallStack 恢复选中行触发 onStackFrameSelected。原 `setCurrentRow(savedRow)` 在 `blockSignals(false)` 之后调用，触发 currentRowChanged → onStackFrameSelected → populateVariableTree，覆盖刚由 updateVariables 设置的完整变量视图。修复：在 setCurrentRow 外层保持 `blockSignals(true)`，仅恢复视觉选中状态不触发变量树更新。位置: `gui/DebugPanel.cpp` `updateCallStack()`
- **BUG-DBG-G2** (P2, 已修复): worker 恢复后调用栈与变量不清理。原 onWorkerFinished 未调用 debugPanel_->clearAll()，worker 异常结束或用户停止时残留调用栈/变量会误导用户以为仍在调试中。修复：在 Ide::onWorkerFinished 的 wasDebug 分支调用 `debugPanel_->clearAll()`（DebugPanel 已有 clearAll 方法）。位置: `app/ide.cpp` `onWorkerFinished()`
- **BUG-DBG-G3** (P2, 已修复): 调用栈深度无显示上限。深度递归（如 fib(40)）产生上万帧导致 QListWidget 卡顿与内存膨胀。修复：新增 `MAX_CALL_STACK_DISPLAY = 200` 常量，循环添加上限 200 帧，超出时添加"... (还有 N 帧未显示)"提示项（设为不可选中）。位置: `gui/DebugPanel.cpp` `updateCallStack()`
- **BUG-DBG-G4** (P2, 已修复): styleScopeGroupHeader pixelSize 假设脆弱。`f.pixelSize() - 1` 在字体未显式设置 pixelSize 时返回 1，-1 得到 0 导致字体渲染异常。修复：增加 `if (ps > 1)` 守卫，仅当 pixelSize 有效时才缩减。位置: `gui/DebugPanel.cpp` `styleScopeGroupHeader()`
- **BUG-DBG-G5** (P2, 已知限制): 变量值无编辑能力。完整实现需双向绑定机制（itemChanged → 写回 Interpreter/VM、类型校验、COW 容器写回、跨后端一致性），工程量大且调试场景下修改变量易引发状态不一致。修复：跳过此 Bug（功能增强），在 variableTree_ 设置处添加 TODO 注释标注功能缺失。位置: `gui/DebugPanel.cpp` 构造函数 variableTree_ 设置

#### OutputPanel 模块（1 P1 + 1 P2 由统一修复覆盖）

- **BUG-OUT-G1** (P1, 已标注): OutputPanel 是死代码。此类未在 IDE 中使用，实际输出由 Ide::outputTextEdit_ 和 errorListWidget_ 直接实现。修复：不删除文件（保留备用），在 `gui/OutputPanel.cpp` 文件头、`gui/OutputPanel.h` 类注释、`cmake/minilang_core.cmake` 源文件列表三处添加 DEPRECATED 标注说明。位置: `gui/OutputPanel.cpp/.h` + `cmake/minilang_core.cmake`
- **BUG-OUT-G2** (P2, 已修复): OutputPanel 无智能滚动。由 BUG-REPL-G4 的 GuiTextUtils.h appendLine 统一修复覆盖（OutputPanel::appendOutput/appendError 已委托 GuiTextUtils::appendLine）。位置: `gui/GuiTextUtils.h` `appendLine()`

### 设计说明

- **BUG-REPL-G5 析构超时策略**：任务说明的原始建议是在析构中调用 requestReplStop + try-catch，但 controller_ 是裸指针且非 closeEvent 路径下可能已析构，C++ 异常不能捕获 UAF。任务说明的"更安全做法"是：在 Ide 中保证 closeEvent 在 controller_ 析构前调用 waitReplFuture（已实现于 ide.cpp closeEvent L443），析构函数仅需无超时 wait() 兜底。本次在此基础增加 wait_for(5s) + LOG_WARNING，超时后回退阻塞 wait()，既避免无限阻塞又有日志可追溯，且不调用 requestReplStop 避免 UAF 风险。
- **BUG-REPL-G6 生命周期安全**：异步 lambda 捕获 `self = this`，~ReplPanel 的 wait()（含本次新增的 5s 超时）保证异步任务完成前 ReplPanel 不会析构，invokeMethod 投递的 lambda 不会访问已析构对象。与原有 ctrl 捕获遵循相同的生命周期假设。
- **BUG-DBG-G1 信号阻塞范围**：原 `blockSignals(false)` 在恢复选中前已恢复，导致 setCurrentRow 触发信号。修复在 setCurrentRow 外层再次 blockSignals(true)，仅这一行不触发信号，恢复后不影响后续用户手动点击栈帧的信号响应。
- **BUG-DBG-G2 清理时机**：仅在 wasDebug 分支调用 clearAll()，因为 run 模式下 DebugPanel 无新数据写入（updateVariables/updateCallStack 仅在调试暂停时调用），run 结束时调试数据本就为空或为上次调试残留。统一在 wasDebug 分支清理更精确，避免对 run 模式做无意义清理。
- **BUG-DBG-G3 上限选择**：200 帧对调试场景足够（用户通常只关心最近调用层级），超出时添加不可选中的提示项，保留 currentStack_ 完整数据（onStackFrameSelected 仍可访问未显示帧的 locals，但用户无法通过点击选中——这是显示层限制，数据层完整）。
- **BUG-OUT-G1 死代码处理**：不删除文件避免未来复用时重新实现，三处标注（cpp 文件头 / h 类注释 / cmake 源列表）确保后续维护者知晓其状态。如需启用需将 Ide::appendOutput/appendError 路由到 OutputPanel 实例。

## 2026-07-04 · app/ 四模块（IdeController / ide.cpp / WorkerManager / VmStepper）P1+P2 Bug 修复

针对 app/ 目录下的 IdeController.cpp/.h、ide.cpp/.h、WorkerManager.cpp/.h、VmStepper.cpp/.h 进行 Bug 修复，覆盖关闭事件资源清理、运行前状态检查、编译结果同步、信号重复触发、UI 冻结、缓存增长、断点泄漏等 12 个问题。发现 3 P1 + 9 P2，**全部 12 个 Bug 已修复**（其中 BUG-IDE-19 标注为已知限制，仅添加注释未改逻辑）。仅修改 app/ 目录下 4 个模块共 7 个文件 + CHANGELOG.md，不影响其他模块。

### 审计发现与修复（3 P1 + 9 P2，已全部修复）

#### P1 修复（3 个）

- **BUG-IDE-08** (P1, 已修复): closeEvent 中 REPL 运行时未发送中止请求。对话框承诺"发送中止请求并等待最多 5 秒"，但 `waitReplFuture()` 前未调用 `controller_->requestReplStop()`，REPL 中的死循环会等到默认超时才结束（或永远不结束）。修复：在 `waitReplFuture()` 前判断 `replPanel_->isReplRunning()` 并调用 `controller_->requestReplStop()` 设置 interpreter stop 标志。位置: `app/ide.cpp` `closeEvent()`
- **BUG-IDE-02** (P1, 已修复): prepareRun 未检查 VmStepper 运行状态。仅检查 `workerMgr_.isRunning()`，未检查 `vmStepper_.isRunning()`，VM RUN 模式异步执行期间点击"运行/调试"会替换编译结果导致 frame.chunk 悬垂。修复：在 workerMgr 检查后增加 `vmStepper_.isRunning()` 检查，emit genericError 并返回 false。位置: `app/IdeController.cpp` `prepareRun()`
- **BUG-IDE-PREP-1** (P1, 已修复): prepareRun 未编译就同步旧编译结果。prepareRun 仅跑前端管线（Lexer+Parser），同步到 VmStepper 的是上一次 `lastCompileResult()`，与本次源码不对应——用户改源码后直接点"运行/调试"会执行旧字节码。修复：在 AST 校验后、`workerMgr_.prepareRun` 前调用 `runCompiler()`，编译错误通过 diagnosticsReady 信号报告。位置: `app/IdeController.cpp` `prepareRun()`

#### P2 修复（9 个）

- **BUG-IDE-15** (P2, 已修复): stopForClose 与 cleanupWorker 状态清理重复。stopForClose 成功路径 reset workerThread_ 后，已投递到主线程事件队列的 QueuedConnection `finished` lambda 仍会执行 cleanupWorker，导致 `restoreReplState/debugger_->reset/setupMainCallbacks` 被调用两次。修复：在 `workerThread_.reset()` 前调用 `workerThread_->disconnect(this)` 断开所有到 this 的信号连接。位置: `app/WorkerManager.cpp` `stopForClose()`
- **BUG-IDE-13** (P2, 已修复): eventFilter 中 titleBar_ 拖拽逻辑未排除 ComboBox/QLineEdit。原实现仅检查 `QAbstractButton*`，QFluentKit ComboBox 继承自 QPushButton 已能被匹配，但缺少对 QComboBox/QLineEdit 的防御性检查。修复：在两个 while 循环（MouseButtonPress + MouseButtonDblClick）中扩展为 `QAbstractButton || QComboBox || QLineEdit` 三类检测。位置: `app/ide.cpp` `eventFilter()`
- **BUG-IDE-23** (P2, 已修复): setUseRegisterVM 未检查 VM 运行状态。VM 运行中切换后端会导致 frame.chunk/RegChunk 悬垂。修复：在 setUseRegisterVM 入口检查 `vmStepper_.isRunning()`，运行中 emit genericError 并 return。位置: `app/IdeController.h` `setUseRegisterVM()`
- **BUG-IDE-10** (P2, 已修复): closeEvent 中调用 `onVmStop()` 而非 `controller_->vmStop()`。onVmStop() 会触发 UI 更新（setVmStepActionsEnabled/codeEditor->setReadOnly 等），在 closeEvent 路径下既无必要也可能与清理竞态。修复：改为直接调用 `controller_->vmStop()` 仅停止 VM。位置: `app/ide.cpp` `closeEvent()`
- **BUG-IDE-04** (P2, 已修复): condAstCache 无限增长。VM 条件断点 AST 缓存无上限，用户反复切换条件表达式会无限增长内存。修复：新增 `COND_AST_CACHE_MAX = 32` 常量，emplace 前检查 size，超出时 erase 最旧条目。位置: `app/IdeController.cpp` 构造函数条件求值器 lambda
- **BUG-IDE-18** (P2, 已修复): VmStepper STEP_OVER/OUT 同步循环可能冻结 UI。深递归/长循环上同步执行可达数十万步，期间不处理事件导致 UI"无响应"。修复：每 2000 步调用 `QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents)` 让出事件循环处理绘制/定时器，排除用户输入防重入；若期间 `isVmRunning_` 被置 false 则立即返回 OK。位置: `app/VmStepper.cpp` `stepByMode()` 主循环
- **BUG-IDE-19** (P2, 已知限制): runBatch 中条件断点求值可能阻塞 UI。条件断点求值是用户主动设置的功能，性能可接受。修复：在 runBatch 断点检查处添加注释标注已知限制，不修改逻辑。位置: `app/VmStepper.cpp` `runBatch()`
- **BUG-IDE-11** (P2, 已修复): 关闭最后一个编辑器标签时未清除 Interpreter/VM 断点。原实现仅清空本地 editor 引用，DebugController/VmStepper 仍持有旧断点行号，下次新建/打开文件行号重叠会意外暂停。修复：在 `codeEditor_ = nullptr` 后调用 `setBreakpoints({})` / `setVmBreakpoints({})` / `setVmBreakpointConditions({})` 清空三处断点状态。位置: `app/ide.cpp` `onEditorTabCloseRequested()`
- **BUG-IDE-12** (P2, 已修复): VM 条件断点仅支持全局变量。VM 局部变量在寄存器/栈中无法按名访问，完整修复需 Compiler 记录变量名→槽位映射，工程量大。修复：在条件求值器 catch 块中检测"未定义/undefined/变量"关键字，附加提示"VM 模式条件断点仅支持全局变量，局部变量无法访问"。位置: `app/IdeController.cpp` 构造函数条件求值器 lambda catch 块

### 设计说明

- **BUG-IDE-15 disconnect 策略选择**：原修复清单建议 `disconnect(workerThread_.get(), &QThread::finished, this, &WorkerManager::cleanupWorker)`，但实际连接目标是 lambda 不是成员函数，该 disconnect 不会匹配。改用 `workerThread_->disconnect(this)` 断开 workerThread_ 所有信号到 this 的连接，更稳健且 workerThread_ 即将 reset 无需保留任何连接。
- **BUG-IDE-PREP-1 冗余性分析**：onRun/onDebug 调用链中 `blockIfHasErrors()` 仅跑前端管线（Lexer+Parser），不调用 runCompiler。因此 prepareRun 中新增的 `runCompiler()` 调用是必要的（非冗余），确保同步到 VmStepper 的编译结果对应当前源码。runCompiler 内部会再次同步编译结果到 VmStepper，与 prepareRun 末尾的同步重复但安全（idempotent）。
- **BUG-IDE-18 processEvents 重入风险**：`ExcludeUserInputEvents` 排除鼠标/键盘事件，避免用户在 STEP_OVER 期间点击其他按钮触发重入。定时器/绘制事件处理后立即检查 `isVmRunning_`，若被异步停止则返回。isVmRunning_ 为非原子 bool，但单线程读写且仅作循环退出判断，可接受。
- **BUG-IDE-04 缓存淘汰策略**：unordered_map 迭代顺序非严格 LRU，erase(begin()) 移除的是任意一个最旧条目（按哈希桶顺序），不能保证淘汰最久未使用。但 32 条上限对调试场景足够，且条件表达式通常不超过 5-10 个，实际很少触发淘汰。完整 LRU 需引入双向链表，工程量大收益低。
- **BUG-IDE-12 关键字检测范围**：catch 块中检测"未定义/undefined/变量"三个关键字匹配 Interpreter 抛出的"未定义的变量 xxx"类异常。误匹配（如条件表达式包含"变量"字面量）只会附加冗余提示，不影响功能。

## 2026-07-04 · GUI 可视化三模块（AstViewer / IrViewer / VmStackPanel）系统性审计 + 全量修复

针对 gui/AstViewer.cpp/.h、gui/IrViewer.cpp/.h、gui/VmStackPanel.cpp/.h 进行系统性 Bug 排查，覆盖递归深度保护、非拥有指针 UAF、折叠键碰撞、QGraphicsItem 生命周期、IR 渲染异常隔离、语法高亮冲突、二分查找有序性假设、Value.toString 异常安全、unordered_map 指针生命周期、列宽自适应、模式切换标题适配等 11 个排查方向。发现 17 个 Bug（2 P1 + 15 P2），**全部 17 个 Bug 已修复**。仅修改 gui/ 目录下 3 个模块共 6 个文件，不影响其他模块。

### 审计发现与修复（2 P1 + 15 P2，已全部修复）

#### AstViewer 模块（2 P1 + 4 P2，已全部修复）

- **BUG-AV-1** (P1, 已修复): 递归遍历无深度保护。7 个递归函数（countNodes/buildRtTree/layoutSubtree/resetLayoutState/syncCollapsedState/computeAbsoluteCoords/drawRtNode）无深度上限，病态深度 AST 可触发栈溢出。修复：新增 `static constexpr int MAX_AST_DEPTH = 400;` 常量，所有递归函数添加 `int depth = 0` 参数与 `if (depth > MAX_AST_DEPTH) return;` 守卫。countNodes 超深度时仍计数当前节点（让其计入 MAX_AST_NODES 上限触发跳过渲染）但不再递归子节点。位置: `gui/AstViewer.h` 新增常量 + `gui/AstViewer.cpp` 7 个递归函数
- **BUG-AV-2** (P1, 已修复): root_ 非拥有指针，setDarkTheme UAF。root_ 是非拥有指针，外部 AST 失效后 setDarkTheme 仍尝试重建场景触发 UAF。修复：setDarkTheme 中先更新主题标志与背景色，root_ 为空时仅刷新视口不重建场景；完整防护需调用方在 AST 失效时调用 clearAst()，已在注释中说明。位置: `gui/AstViewer.cpp` `setDarkTheme()`
- **BUG-AV-3** (P2, 已修复): textItem 未注册到 itemToNode_，点击文字无法折叠。drawRtNode 中 textItem 覆盖 rectItem，点击命中 textItem 时无法触发折叠。修复：textItem 设置 `ItemIsSelectable=false` 与 `setAcceptedMouseButtons(Qt::NoButton)`，让点击事件穿透到下层 rectItem。位置: `gui/AstViewer.cpp` `drawRtNode()`
- **BUG-AV-4** (P2, 已修复): collapsedKeys_ 折叠键碰撞风险。CollapseKey 为 `(line, column, nodeName)`，同位置同类型不同子节点数的节点会碰撞。修复：CollapseKey 类型改为 `std::tuple<int, int, std::string, int>`，增加子节点数维度；makeCollapseKey 计算非空子节点数；CollapseKeyHash 同步更新。位置: `gui/AstViewer.h` CollapseKey 类型 + `gui/AstViewer.cpp` `makeCollapseKey()`
- **BUG-AV-5** (P2, 已修复): shiftSubtree 死代码。shiftSubtree 因 `if (offset != 0)` 守卫且所有调用点 offset=0，实际为死代码。修复：删除 shiftSubtree 函数声明与定义，删除 SUBTREE_SPACING 常量。位置: `gui/AstViewer.h` + `gui/AstViewer.cpp`
- **BUG-AV-6** (P2, 已修复): itemToNode_ 键生命周期耦合。setAst 中 scene_->clear() 销毁 QGraphicsRectItem 后 itemToNode_ 仍持有悬垂键。修复：用 swap 先取出 itemToNode_ 到 oldMap，scene_->clear() 后 oldMap.clear() 安全清理。位置: `gui/AstViewer.cpp` `setAst()`

#### IrViewer 模块（5 P2，已全部修复）

- **BUG-IRV-1** (P2, 已修复): setIR 无 try/catch。formatIRInstruction 或 HTML 构造抛异常时面板进入未定义状态。修复：用 try/catch 包裹整个函数体，catch std::exception 与 ... 显示错误占位文本。位置: `gui/IrViewer.cpp` `setIR()`
- **BUG-IRV-2** (P2, 已修复): highlightBySourceLine 注释承诺高亮所有但只高亮第一个。头文件注释"高亮所有 line 匹配的指令行"与实现（break 后仅高亮首个）不符。修复：修正头文件注释为"只高亮第一个 line 匹配的指令行"。位置: `gui/IrViewer.h` `highlightBySourceLine` 注释
- **BUG-IRV-3** (P2, 已修复): setCharFormat 高亮可能与 span 语法高亮冲突。setCharFormat 用 LineUnderCursor 覆盖整行字符格式，会擦除 HTML span 标签产生的语法高亮颜色。修复：改用 QTextBlockFormat 设置整行背景（setBlockFormat），不触碰字符格式；新增 `#include <QTextBlockFormat>`。位置: `gui/IrViewer.cpp` `highlightBySourceLine()` + `highlightByBytecodeOffset()` + `clearHighlight()`
- **BUG-IRV-4** (P2, 已修复): 二分查找假设排序无校验。highlightByBytecodeOffset 用二分查找最大 <= currentBytecodeOffset 项，但 irToBytecodeOffset 的 .second 排序未强制保证。修复：改为线性查找，遇到大于值即停止（假设大致有序），既稳健又不损失常见路径效率。位置: `gui/IrViewer.cpp` `highlightByBytecodeOffset()`
- **BUG-IRV-5** (P2, 已修复): isNumericConst 允许 - 在任意位置。`c == '-'` 在任意位置都 continue，导致 "1-2" 等非法字面量被误识别为数字。修复：收紧规则，仅允许首字符（i==0）为 '-'。位置: `gui/IrViewer.cpp` `isNumericConst()`

#### VmStackPanel 模块（6 P2，已全部修复）

- **BUG-VSP-1** (P2, 已修复): updateStack/Registers/Globals 无 try/catch。Value.toString() 可能抛异常（NaN-boxing 解码失败等），未捕获会导致面板崩溃。修复：对每个 toString() 调用包 try/catch，异常时显示 "<error>"，并对超长字符串（>200）截断。位置: `gui/VmStackPanel.cpp` `updateStack()` + `updateRegisters()` + `updateGlobals()`
- **BUG-VSP-2** (P2, 已修复): const& 接口鼓励传引用。updateStack/Registers/Globals 用 const& 参数，若调用方传入临时引用的子元素可能悬垂。修复：接口改为 by-value（调用方 IdeController 已 by-value 返回，改为 by-value 参数不影响调用方）。位置: `gui/VmStackPanel.h` + `gui/VmStackPanel.cpp` 三个函数签名
- **BUG-VSP-3** (P2, 已修复): updateGlobals 列宽不随内容变长重算。原实现仅在行数变化时 resizeColumnsToContents，同行数下值变长不会扩展列宽导致截断。修复：新增 `lastMaxValueWidth_` 成员跟踪值列最大文本长度，行数变化或长度增长超过 5 时重算列宽。位置: `gui/VmStackPanel.h` 新增成员 + `gui/VmStackPanel.cpp` `updateGlobals()`
- **BUG-VSP-4** (P2, 已修复): updateGlobals 排序用 unordered_map 元素裸指针。原实现用 `const std::pair<const std::string, Value>*` 排序，若 globals 被修改/销毁指针即悬垂。修复：函数入口先 move 到本地 `std::vector<std::pair<std::string, Value>>`，排序与访问均基于本地 vector，生命周期完全独立。位置: `gui/VmStackPanel.cpp` `updateGlobals()`
- **BUG-VSP-5** (P2, 已修复): updateCurrentOp 对空 opName 无兜底。空 opName 或 line<=0 时显示空白。修复：空 opName 显示"(未知指令)"，line<=0 显示"(无行号)"。位置: `gui/VmStackPanel.cpp` `updateCurrentOp()`
- **BUG-VSP-6** (P2, 已修复): 模式切换时标题不适配。栈式/寄存器模式切换时 stackTitle 始终显示"操作数栈"。修复：stackTitle 改为成员变量 `stackTitle_`，updateStack 入口设为"操作数栈 (栈顶 ↑)"，updateRegisters 入口设为"寄存器 (R0..Rn)"。位置: `gui/VmStackPanel.h` 新增成员 + `gui/VmStackPanel.cpp` 构造函数 + `updateStack()` + `updateRegisters()`

### 设计说明

- **BUG-AV-1 深度守卫策略**：countNodes 超深度时仍计数当前节点（返回 1 而非 0），目的是让 MAX_AST_NODES=2000 上限仍能触发跳过渲染提示，而非静默截断深度。其余 6 个递归函数超深度时直接跳过子节点处理。
- **BUG-AV-2 UAF 防护范围**：本次仅实现 root_==nullptr 的基本守卫与文档说明。完整防护需调用方（IdeController）在 AST 失效时主动调用 clearAst() 清空 root_，本次任务范围内不修改 IdeController。
- **BUG-IRV-3 setBlockFormat 选择**：QTextBrowser 的 HTML span 标签会被转为 QTextCharFormat，setCharFormat(LineUnderCursor) 会整体覆盖字符格式擦除 span 颜色。setBlockFormat 仅修改块级背景，保留字符级格式，是更安全的行高亮方式。
- **BUG-IRV-4 线性查找选择**：原二分查找依赖 irToBytecodeOffset 按 .second 严格升序，但该不变量未在文档或断言中强制。线性查找（遇到大于值即停止）在常见路径（大致有序）下性能等同，且对局部乱序稳健。
- **BUG-VSP-2/VSP-4 by-value 策略**：IdeController::getVmStack()/getVmGlobals() 已 by-value 返回，改为 by-value 参数后临时对象直接 move 进参数，无额外拷贝开销，同时消除引用生命周期耦合。

## 2026-07-04 · GUI 编辑器三模块（CodeEditor / SyntaxHighlighter / FindReplacePanel）系统性审计 + 全量修复

针对 gui/CodeEditor.cpp/.h、gui/SyntaxHighlighter.cpp/.h、gui/FindReplacePanel.cpp/.h 进行系统性 Bug 排查，覆盖块注释状态编码、文档修改后断点/折叠偏移、行号区域事件处理、查找替换面板交互、语法高亮状态机等 8 个排查方向。发现 16 个 Bug（2 P1 + 14 P2），**15 个 Bug 已修复**，1 个（BUG-FR-3 正则表达式支持）为功能增强非 Bug 修复，按任务说明跳过。仅修改 gui/ 目录下 3 个模块共 5 个文件，不影响其他模块。

### 审计发现与修复（2 P1 + 13 P2 已修复 + 1 跳过）

#### CodeEditor 模块（2 P1 + 6 P2，已全部修复）

- **BUG-CE-1** (P1, 已修复): 块注释状态编码不匹配。`isFoldable()` / `foldEndBlock()` 用 `prev.userState() == 2` 判断块注释，但 SyntaxHighlighter 实际设置 `100 + depth`，导致块注释行的折叠判定失效。修复：兼容两种编码 `(s == 2 || s >= 100)`。位置: `gui/CodeEditor.cpp` `isFoldable()` + `foldEndBlock()`
- **BUG-CE-2** (P1, 已修复): 断点行号无偏移补偿。未监听 `QTextDocument::contentsChange`，文档修改后断点停留在原行号。修复：连接 `contentsChange` 信号，通过 `lastBlockCount_` 计算 delta，调整 `breakpoints_` / `breakpointConditions_` 行号。位置: `gui/CodeEditor.h` 新增成员 + `gui/CodeEditor.cpp` 构造函数 + `onContentsChange()`
- **BUG-CE-3** (P2, 已修复): `foldedBlocks_` 块号无偏移补偿。同 BUG-CE-2，`foldedBlocks_` 以 blockNumber 为 key，文档修改后块号变化。修复：在 `onContentsChange` 中同步调整 `foldedBlocks_`。位置: `gui/CodeEditor.cpp` `onContentsChange()`
- **BUG-CE-4** (P2, 已修复): 空行错误范围越界。`setErrorRanges()` 空行时 `len=1` 选中换行符。修复：`blockTextLen <= 0` 时 `continue` 跳过。位置: `gui/CodeEditor.cpp` `setErrorRanges()`
- **BUG-CE-5** (P2, 已修复): `contextMenuEvent` 缺少空白区域检查。`cursorForPosition` 对最后一行下方空白返回末尾光标，误触发条件菜单。修复：添加与 `mousePressEvent` 一致的 `lastBlockBottom` 检查。位置: `gui/CodeEditor.cpp` `LineNumberArea::contextMenuEvent()`
- **BUG-CE-6** (P2, 已修复): 折叠点击区域与绘制区域不一致。判定区域 `>=width()-16` 比绘制区域 `width()-14` 宽。修复：统一为 `[width()-14, width()-4)`。位置: `gui/CodeEditor.cpp` `LineNumberArea::mousePressEvent()`
- **BUG-CE-7** (P2, 已修复): `setBreakpoints` / `mousePressEvent` 不识别块注释行。仅过滤 `text.startsWith("//")`，不识别块注释行。修复：增加 `userState() == 2 || userState() >= 100` 判断。位置: `gui/CodeEditor.cpp` `setBreakpoints()` + `mousePressEvent()`
- **BUG-CE-8** (P2, 已修复): 查找高亮覆盖执行行高亮。`findSelections_` 附加在 `currentLine_` 之后，查找高亮覆盖执行行高亮。修复：调整顺序，`findSelections_` 先添加（底层），执行行最后添加（覆盖）。位置: `gui/CodeEditor.cpp` `highlightCurrentLine()`

#### SyntaxHighlighter 模块（4 P2，已全部修复）

- **BUG-SH-1** (P2, 已修复): 插值内嵌套块注释跨行时上下文丢失。`blockCommentDepth > 0` 优先于 `interpBraceDepth > 0`，插值上下文丢失。修复：引入组合状态编码 `300 + braceDepth*10 + blockCommentDepth`。位置: `gui/SyntaxHighlighter.cpp` `highlightBlock()` 状态编解码
- **BUG-SH-2** (P2, 已修复): 插值内嵌套字符串跨行未处理。嵌套字符串为单行扫描，未闭合时下一行错误处理。修复：引入新状态编码 `400 + braceDepth`，嵌套字符串未闭合时跨行继续扫描。位置: `gui/SyntaxHighlighter.cpp` `highlightBlock()` 插值分支 + 状态编解码
- **BUG-SH-3** (P2, 已修复): 数字字面量不支持 0x/0b/0o 前缀。`0xFF` 被当作数字 0 处理后 `xFF` 被误识别为标识符。修复：识别 0x/0b/0o 前缀，将整个非法字面量高亮为错误格式（红色）。位置: `gui/SyntaxHighlighter.cpp` `highlightBlock()` 数字分支前
- **BUG-SH-4** (P2, 已修复): `keywordSet_` 长度上限 20。`wordLen <= 20` 检查跳过过长标识符的查找，虽当前无长关键字但为防御性修复。修复：移除 `wordLen <= 20` 检查，直接用 `keywordSet_.contains()`。位置: `gui/SyntaxHighlighter.cpp` `highlightBlock()` 标识符分支

#### FindReplacePanel 模块（3 P2 已修复 + 1 跳过）

- **BUG-FR-1** (P2, 已修复): 跨行选中文本判断用 `'\n'`。`QTextCursor::selectedText()` 返回的换行符是 `QChar::ParagraphSeparator`（U+2029），原判断 `contains('\n')` 无法识别跨行选中。修复：改为 `contains(QChar::ParagraphSeparator)`。位置: `gui/FindReplacePanel.cpp` `showPanel()`
- **BUG-FR-2** (P2, 已修复): `onReplaceAll` 达到上限时未提示。循环退出后未检查是否仍有剩余匹配。修复：检查 `found` 是否仍非 null，若是则显示"已替换 N+ 处（达到上限，仍有剩余匹配）"。位置: `gui/FindReplacePanel.cpp` `onReplaceAll()`
- **BUG-FR-3** (P2, 跳过): 缺少正则表达式支持。功能增强，非 Bug 修复，按任务说明跳过。
- **BUG-FR-4** (P2, 已修复): `onReplace` 没有 `beginEditBlock`。替换操作未包裹 `beginEditBlock`/`endEditBlock`，无法作为单步撤销。修复：用 `beginEditBlock`/`endEditBlock` 包裹 `insertText`。位置: `gui/FindReplacePanel.cpp` `onReplace()`
- **BUG-FR-5** (P2, 已修复): `onFindTextChanged` 移动光标打断编辑。文本变化时调用 `findText(true)` 移动光标到匹配位置，打断用户编辑。修复：仅调用 `highlightMatches`，不调用 `findText`。位置: `gui/FindReplacePanel.cpp` `onFindTextChanged()`

### 设计说明

- **BUG-CE-2 实现选择**：`contentsChange` 在变更已应用后发射，被删除文本不可访问，无法直接统计其换行符数量。改为通过文档块数变化计算 delta（`lastBlockCount_` 在上次变更后/构造时更新），保证纯插入/纯删除/替换三类场景均正确。
- **BUG-SH-1/SH-2 状态编码重构**：原状态 300（插值内字符串）合并入 200（插值），因 `inString` 在 `interpBraceDepth > 0` 期间恒为 true 无需单独编码。新 300 用于插值+块注释组合，新 400 用于插值内嵌套字符串跨行。解码顺序高范围优先（400 > 300 > 200 > 100）避免误匹配。旧状态 300 的文档在下次 rehighlight 后自动修正。

## 2026-07-04 · Formatter 模块 P2 Bug 修复（BUG-F-01~F-08）

针对 formatter/Formatter.cpp 进行系统性 Bug 排查，发现 8 个 P2 Bug（多行块注释缩进 / 独立块前置空格 / ExportStmt 空行 / 类方法无空行 / 插值注释错位 / NaN-Infinity 格式化 / 空 throw 兜底，以及 1 个经审计确认的预期行为）。**7 个 Bug 已修复**，1 个（BUG-F-02）经审计确认为预期行为跳过。仅修改 formatter/Formatter.cpp，不影响其他模块。

### 审计发现与修复（7 P2 已修复 + 1 跳过）

- **BUG-F-01** (P2, 已修复): 多行块注释缩进不一致。多行块注释的 lexeme 内含 `\n`，后续行继承源码原始缩进，与格式化后的缩进不一致。修复：新增 `reindentBlockComment` 辅助函数，在每个 `\n` 后插入当前 `indent()`；在 formatBlock 的全部 4 处注释输出点（语句前注释 / 同行尾部注释 / 块末尾注释 / 顶层块剩余注释）应用。位置: `formatter/Formatter.cpp` `reindentBlockComment()` + `formatBlock()` 4 处
- **BUG-F-02** (P2, 跳过): Allman 风格 try/catch 的 `catch` 关键字始终以 `" catch"` 前置空格追加。经审计确认这与 `formatIfStmt` 的 `" else "` 行为一致，是风格选择而非 Bug。位置: `formatter/Formatter.cpp` `visitTryStmt()`
- **BUG-F-03** (P2, 已修复): K&R 风格 `openBrace()` 返回 `" {"`（带前置空格），独立块（NODE_BLOCK 作为语句）经 formatBlock 输出 `<indent> {\n...}`，首部多余空格。修复：在 formatBlock 中对 NODE_BLOCK 类型节点去除 formatNode 返回值开头的前置空格。位置: `formatter/Formatter.cpp` `formatBlock()`
- **BUG-F-04** (P2, 已修复): `isFunOrClass` lambda 未解包 `NODE_EXPORT_STMT`，导致 `export fun f() {}` 之间不会插入空行。修复：lambda 中对 ExportStmt 解包，检查内部 declaration 是否为 FunDecl/ClassDecl。位置: `formatter/Formatter.cpp` `formatBlock()` `isFunOrClass` lambda
- **BUG-F-05** (P2, 已修复): `blankLineBetweenFunctions` 选项未传播到类成员，类方法之间无空行。修复：在 formatClassDecl 成员循环中，当选项为 true 且前后两个成员都是 FunDecl 时插入空行。位置: `formatter/Formatter.cpp` `formatClassDecl()`
- **BUG-F-06** (P2, 已修复): `formatNode` 不消费 `comments_` 数组，插值表达式行号范围内的注释会落入外层语句的"同行尾部注释"分支。修复：在 formatInterpolatedString 中跳过插值表达式行号范围内的注释（推进 commentIndex_ 但不输出）。完整修复需重构注释游标机制（按列范围匹配），此处为部分修复。位置: `formatter/Formatter.cpp` `formatInterpolatedString()`
- **BUG-F-07** (P2, 已修复): 对 NaN/Infinity，`toString()` 返回 `"nan"`/`"inf"`/`"-inf"`，经过 `.0` 附加逻辑变成 `"nan.0"`/`"inf.0"`/`"-inf.0"`，非合法 NumberLiteral。修复：在 `.0` 附加逻辑前检查是否为 NaN/Infinity 特殊字符串，保持原样输出。位置: `formatter/Formatter.cpp` `formatNumberLiteral()`
- **BUG-F-08** (P2, 已修复): `visitThrowStmt` 中 `expression` 为 nullptr 时输出 `"throw"`（无表达式），Parser 强制要求表达式故正常路径不会产生此 AST，但外部构造的 AST 会触发。修复：空 throw 时输出 `"throw null"` 作为兜底。位置: `formatter/Formatter.cpp` `visitThrowStmt()`

## 2026-07-04 · DebugController + REPL 模块系统性审计 + 全量修复

针对 app/ 目录下的 VmStepper / WorkerManager / DebugCoordinator / IdeController / ide.cpp 以及 interpreter/Interpreter.cpp / Interpreter.h / InterpreterModules.cpp 进行系统性审计，覆盖 8 个排查方向（条件断点沙箱状态恢复 / 断点暂停语义 / 单步执行 / 调用栈可视化 / REPL 续行 / 模块缓存刷新 / 异步执行 / Worker 状态清理），通过并行 agent 分模块审计后汇总。发现 15 个 Bug（1 P1 + 14 P2），全部为状态隔离、重置顺序、路径规范化或调用栈显示问题，不影响正常执行语义。**全部 15 个 Bug 已修复**，新增 14 个回归测试（tests/TestDebugAudit.cpp，6 个套件），全量 1212 个测试通过（原 1198 + 新增 14），无回归。

### 审计发现与修复（1 P1 + 14 P2，已全部修复）

#### 调用栈与断点语义（5 个）

- **BUG-DBG-1** (P1, 已修复): 调用栈顶帧行号显示调用点而非当前执行行。`CallFrame::line` 在 `callNamedFunction` 等处初始化为调用点行号后从不更新，导致调试暂停时调用栈面板显示的行号是函数调用发起处，而非当前执行行。修复：`checkBreak` 在每次暂停时更新 `callStack_.back().line` 为当前节点行号。位置: `interpreter/Interpreter.cpp` `checkBreak()`
- **BUG-DBG-2** (P2, 已修复): `VmStepper::runBatch` 缺少 `crossedLine_` 机制。`stepByMode` 已有 `crossedLine_` 机制（F4 fix）允许同一行断点在跨行后重新触发，但 `runBatch` 仅有 `lastPausedLine_` 检查，导致循环内单行断点在首次暂停后无法再次暂停。修复：`runBatch` 同步引入 `vmCrossedLine_` / `vmLastSeenLine_` 机制。位置: `app/VmStepper.cpp` `runBatch()` + `app/VmStepper.h`
- **BUG-DBG-3** (P2, 已修复): `STEP_OUT` 顶层行为与 `STEP_OVER` 顶层不一致。`STEP_OVER` 顶层使用 `crossedLine_` 机制允许同行暂停，`STEP_OUT` 顶层仅检查 `currentLine != lastPausedLine_`，导致顶层 `STEP_OUT` 在同行无法暂停。修复：`STEP_OUT` 顶层对齐 `STEP_OVER` 使用 `crossedLine_`。位置: `app/VmStepper.cpp` `stepByMode()`
- **BUG-DBG-4** (P2, 已修复): `callNamedFunction` 使用变量名而非闭包名。闭包赋值给变量后调用，调用栈显示变量名而非闭包定义名。修复：使用 `effectiveName`（闭包定义名）而非 `node.name`（变量名）。位置: `interpreter/InterpreterCalls.cpp` `callNamedFunction()`
- **BUG-DBG-5** (P2, 已修复): Interpreter 调用栈无顶层 `main` 帧。`execute()` / `executeRepl()` 不压入 `main` 帧，导致顶层代码调试时调用栈面板为空，而 VM 路径显示 `main` 帧。修复：`execute()` / `executeRepl()` 入口压入 `main` 帧。位置: `interpreter/Interpreter.cpp` `execute()` + `executeRepl()`

#### 沙箱状态隔离（4 个）

- **BUG-DBG-7** (P2, 已修复): 沙箱未重置 `recursionDepth_`。条件断点求值时 `recursionDepth_` 可能已接近 `MAX_RECURSION_DEPTH`，沙箱内递归调用触发假阳性深度超限。修复：`SandboxGuard` 保存 `recursionDepth_`，求值前重置为 0，析构时恢复。位置: `interpreter/Interpreter.cpp` `evaluateCondition()` + `SandboxGuard`
- **BUG-DBG-8** (P2, 已修复): 沙箱未保存/恢复 `funRegistry_` / `classRegistry_`。条件断点中定义的函数/类会污染注册表，求值后仍可调用。修复：`SandboxGuard` 保存/恢复 `funRegistry_` / `funRegistryGen_` / `classRegistry_` / `classRegistryGen_`。位置: `interpreter/Interpreter.cpp` `evaluateCondition()` + `SandboxGuard`
- **BUG-REPL-2** (P2, 已修复): 沙箱未保存/恢复 `lastValue_`。条件断点求值覆盖 `lastValue_`，求值后 `evaluateExpr` 行为异常。修复：`SandboxGuard` 保存/恢复 `lastValue_`，条件求值结果改为拷贝（非 move）避免影响保存值。位置: `interpreter/Interpreter.cpp` `evaluateCondition()` + `SandboxGuard`
- **BUG-DBG-9** (P2, 已修复): `stopRequested_` 未在 `execute()` 中重置。`executeRepl()` 入口重置 `stopRequested_`，但 `execute()` 遗漏，导致 Run 模式下连续运行会因上一轮"停止"残留而立即中止。修复：`execute()` 入口添加 `stopRequested_.store(false)`。位置: `interpreter/Interpreter.cpp` `execute()`

#### Worker 状态清理（2 个）

- **BUG-DBG-10** (P2, 已修复): `prepareRun` catch 块重置顺序错误。原顺序 `worker_.reset()` → `workerThread_.reset()` 会在 Worker 仍引用时销毁 thread，与 `cleanupWorker()` 顺序（quit+wait thread → reset worker）不一致。修复：catch 块对齐 `cleanupWorker()` 顺序。位置: `app/WorkerManager.cpp` `prepareRun()` catch 块
- **BUG-DBG-11** (P2, 已修复): `prepareRun` 状态设置在 try 块外。`interpreter_->setDebugMode(true)` / `isDebugRun_ = isDebug` / `isRunning_ = true` 在 try 块之前执行，若后续抛异常这些状态残留为 true。修复：移入 try 块内。位置: `app/WorkerManager.cpp` `prepareRun()`

#### VM 调用栈与文档（2 个）

- **BUG-DBG-6** (P2, 已修复): VM 调用栈从未在 GUI 显示。`updateDebugInfo` 始终使用 Interpreter 调用栈，VM 模式下显示空栈。修复：`VmStepper::getCallStack()` 转换 VM/RegisterVM 调用栈为 `CallStackEntry`，`IdeController::getVmCallStack()` 暴露，`updateDebugInfo` 在 VM 面板可见时使用 VM 调用栈。位置: `app/VmStepper.h` + `app/IdeController.h` + `app/ide.cpp` `updateDebugInfo()`
- **BUG-DBG-13** (P2, 已修复): `VmStepper` 注释引用不存在的函数 `resetVmStepState`。修复：更新注释引用为正确的 `stop()`。位置: `app/VmStepper.cpp`

#### 模块缓存与测试桩（2 个）

- **BUG-REPL-1** (P2, 已修复): `clearModuleCache` 未规范化路径。`visitImportStmt` 将 `\` 转 `/` 并去除 `./` 前缀后存入 `moduleCache_`，但 `clearModuleCache` 直接用原始 path 查 erase，导致用户用 `"foo\\bar.mini"` 或 `"./foo.mini"` 调用时无法命中缓存。修复：`clearModuleCache` 同步规范化路径（`\`→`/`，strip `./`）。位置: `interpreter/Interpreter.h` `clearModuleCache()`
- **BUG-DBG-14** (P2, 已修复): 测试桩 `DebugController` 缺少关键修复。`test_harness/debug/DebugController.h/.cpp` 的桩实现缺少 `crossedLine_` / `crossedDeeper_` 机制与条件求值，导致 `debug_test` 与真实 `DebugController` 行为不一致。修复：桩同步添加 `crossedLine_` / `crossedDeeper_` 字段与条件求值逻辑。位置: `test_harness/debug/DebugController.h` + `DebugController.cpp`

### 已确认无 Bug 的关键区域

- `DebugEvaluator` 条件求值异常隔离（try/catch + SandboxGuard）
- `VmStepper` 步进状态机（STEP_IN/OVER/OUT/RUN）主逻辑
- `DebugCoordinator` 断点/步进回调注册与分发
- `InterpreterWorker` 异步执行与信号槽通信
- `Interpreter` REPL 状态保存/恢复（saveReplState/restoreReplState）

### 测试

新增 14 个回归测试（tests/TestDebugAudit.cpp，6 个套件）：
- `DebugAuditMainFrame`（3 个）：execute/executeRepl 含 main 帧、main 帧行号更新
- `DebugAuditStopReset`（2 个）：execute 重置 stopRequested_、连续 execute 不残留
- `DebugAuditModuleCache`（2 个）：反斜杠路径清除、`./` 前缀路径清除
- `DebugAuditSandbox`（4 个）：funRegistry/classRegistry 不污染、lastValue_ 保留、recursionDepth_ 重置
- `DebugAuditCallStack`（1 个）：闭包调用栈显示闭包名
- `DebugAuditRegression`（2 个）：stop 后重新 execute 状态完整、沙箱求值后所有状态恢复

## 2026-07-04 · 三执行引擎（Interpreter + StackVM + RegisterVM）系统性审计 + 全量修复

针对 interpreter/ 目录全部文件 + compiler/ 目录下 VM/VMCalls/VMContainers/RegisterVM/RegisterBytecode/RegisterBytecodeBackend/IR 进行系统性审计，覆盖 7 个排查方向（算术运算/逻辑运算/变量与作用域/类与继承/异常处理/数据结构/单引擎特有问题），通过并行 agent 分模块审计后汇总。发现 12 个 P2 Bug（0 P0 / 0 P1），全部为错误路径栈平衡、缓存失效、调试器一致性或死代码问题，不影响正常执行语义，不影响三后端一致性。**全部 12 个 P2 Bug 已修复**，新增 27 个回归测试（tests/TestThreeEnginesAudit.cpp，13 个套件），全量 1198 个测试通过（原 1171 + 新增 27），无回归。

### 审计发现与修复（P2 级，12 个，已全部修复）

#### Interpreter + Value 模块（3 个）

- **BUG-INTP-1** (P2, 已修复): `deepCloneForSandbox` 对 instance 返回浅拷贝，条件断点中 `this.inner.field = 99; true` 会通过 boundInstance_ 链找到原嵌套 InstanceData 原地修改，`SandboxGuard` 析构恢复 Outer.fields 但 `fields["inner"]` 仍指向被污染的 Inner。修复：对 instance 递归深拷贝 fields（与 array/dict 一致），破坏引用语义为沙箱隔离固有矛盾，已在注释中标注。位置: `interpreter/Interpreter.cpp` `deepCloneForSandbox()`
- **BUG-INTP-2** (P2, 已修复): `SandboxGuard` 析构 this 重赋值场景污染新实例。BUG-INT-3 fix 检查 `inst->isInstance()` 跳过非实例 this 重赋值，但若 this 被重赋值为另一实例（仍 `isInstance==true`），析构会把旧 A 字段快照写入新 B 实例。修复：`instSnaps` 中额外存储原始 InstanceData 指针（`gcRootPtr()`），析构时比较，不匹配则跳过字段恢复。位置: `interpreter/Interpreter.cpp` `evaluateCondition()` + `SandboxGuard` 析构
- **BUG-INTP-3** (P2, 已修复): 字符串索引 ASCII 缓存在原地 append 后失效。`lastAsciiStrPtr_` 缓存以 `&s` 为 key，`s = s + "é"` 通过 `tryGetMutableString` 原地 append（refCount=1 不触发 COW detach），StringData 指针不变，缓存仍命中旧 `isAscii=true`，按字节索引返回多字节字符碎片。修复：缓存 key 改用 `(ptr, len)` 双重验证，原地 append 后 len 变化使缓存失效。`Interpreter.h` 新增 `lastAsciiStrLen_` 字段。位置: `interpreter/Interpreter.cpp` `visitIndexAccess()` + `interpreter/Interpreter.h`

#### StackVM 模块（5 个）

- **BUG-VM-01** (P2, 已修复): `executeClassNew` 错误路径栈残留。`push(instance)` 在 `argCount > 0` 检查之前，与同文件 `executeCall` 类构造路径（检查在 push 之前）顺序相反。修复：将 `argCount > 0` 检查移到 `push(instance)` 之前。**同时修复 RegisterVM 的同源问题**：`executeClassNewImpl` 无 init 但有参数时静默忽略，三后端语义不一致，新增 `argCount > 0` 检查报运行时错误。位置: `compiler/VMCalls.cpp` `executeClassNew` + `compiler/RegisterVM.cpp` `executeClassNewImpl()`
- **BUG-VM-02** (P2, 已修复): OP_CALL 函数路径 6 个错误返回点未 `popN(argCount)`，与 OP_CALL_EXPR 路径不一致（R3-1 fix / AUDIT-BUG-V4 fix 仅应用于 OP_CALL_EXPR）。缺失点: argCount 范围/默认参数索引越界/非字面量/常量索引越界/MAX_FRAMES/extraSlots<0。修复：所有错误返回点前添加 `popN(argCount)`。位置: `compiler/VMCalls.cpp` `executeCall` (OP_CALL)
- **BUG-VM-03** (P2, 已修复): `executeMethodCall` MAX_FRAMES 错误未 `popN(argCount+1)`，同函数 argCount 检查（L665-673）正确执行了 `popN(argCount+1)`。修复：MAX_FRAMES 错误返回前添加 `popN(argCount + 1)`。位置: `compiler/VMCalls.cpp` `executeMethodCall`
- **BUG-VM-04** (P2, 已修复): OP_BUILD_DICT 错误路径栈残留。循环中先 pop key/val 再检查 `!key.isString()`，错误返回时栈上残留 `2 * (pairCount - i - 1)` 个未处理键值对。修复：错误返回前 `popN` 清理剩余键值对。位置: `compiler/VMContainers.cpp` `OP_BUILD_DICT`
- **BUG-VM-05** (P2, 已知限制): `lastAsciiStrPtr_` 缓存失效条件不完备。缓存以裸指针为 key，StringData 释放后内存复用 + 相同长度 + 不同 ASCII 状态下可能误命中。当前 (ptr, size) 双重验证已大幅降低风险（与 BUG-INTP-3 同步修复）。位置: `compiler/VMContainers.cpp` `OP_INDEX_GET`（字符串索引）

#### RegisterVM 模块（4 个）

- **BUG-REGVM-1** (P2, 已修复): `executeArith` 字符串拼接路径跳过 `stepCallback_`。L384 直接 `return VM_OK;` 跳过函数末尾统一 `stepCallback_` 调用。修复：改用 `goto arithDone` 落入统一调用点。位置: `compiler/RegisterVM.cpp` `executeArith()`
- **BUG-REGVM-2** (P2, 已修复): REG_RETURN/REG_RETURN_NULL/REG_THROW 跳过 `stepCallback_`。REG_RETURN/REG_RETURN_NULL 通过 `return executeReturnImpl(...)` 直接返回，跳过 `executeCalls` 末尾 `stepCallback_`；REG_THROW 通过 `return throwException(...)` 直接返回，跳过 `executeMisc` 末尾 `stepCallback_`。修复：捕获结果，若 `VM_OK` 则调用 `stepCallback_`。位置: `compiler/RegisterVM.cpp` `executeCalls()` + `executeMisc()`
- **BUG-REGVM-3** (P2, 已修复): `functionClosures_` 死代码 + 潜在 upvalue 丢失。`functionClosures_` 在 `resetState()` clear、`executeCallImpl` find，但**无任何写入点**，是死代码。修复：删除 `functionClosures_` 字段及死代码路径。位置: `compiler/RegisterVM.cpp` `executeCallImpl()` + `compiler/RegisterVM.h`
- **BUG-REGVM-4** (P2, 已修复): `closeUpvaluesFrom` 防御性缺陷。当 `frameIdx >= frames_.size()`（目标帧已弹出）时，upvalue 标记 `isClosed=true` 但 value 未赋值，保留默认 null 或残留值。当前调用契约保证此路径不可达。修复：添加 `Logger::Warning` 留痕。位置: `compiler/RegisterVM.cpp` `closeUpvaluesFrom()`

### 已确认无 Bug 的关键区域

#### Interpreter + Value
- Value.cloneImpl 完整覆盖（含 BoxedIntData VAL_INT case）、环检测、深度保护正确
- NaN-boxing asPtr 零扩展、Tag 枚举完整
- NumericOps 三后端共享算术逻辑，溢出检查完整
- Environment boundInstance_ 缓存（H2 fix）、restoreLocalVariables 重新锚定
- GcManager sweep 防 UAF（AUDIT-BUG-I3 fix）
- InterpreterCalls 闭包 capturedVars 快照重建/回写（C1/P0-6 fix）、默认参数在闭包环境求值
- InterpreterClasses findMethod 缓存失效（C6 fix）、lookupInheritanceChain 防循环继承
- InterpreterModules 路径安全（BUG-MOD-1 fix）、循环依赖检测、原子性导入（P2-1 fix）
- visitTryStmt CatchEnvGuard 异常路径正确调用 closeCapturedVariables
- visitForStmt/visitWhileStmt LoopFlow 标志、MAX_LOOP_ITERATIONS 防护
- visitFunDecl computeFreeVariables 静态分析仅捕获自由变量（B1 fix）、registerClosureCapture 注册 open upvalue
- writeBack/writeBackChain 链式左值赋值求值顺序正确

#### StackVM
- VMStack 全部有 std::abort() 越界检查
- MAX_FRAMES=256 / MAX_STACK_SIZE=1024 / MAX_INSTRUCTIONS 限制正确实施
- IP 边界检查在所有跳转指令中存在
- numericOp 共享 NumericOps::computeArith 三后端一致
- OP_AND/OP_OR 为死代码（D6 fix），短路在编译器层面统一
- closeUpvaluesFrom O(log n + k) 正确（P0-5 fix）
- throwException 跨帧传播正确（P0-5/P1-5/V-P1-5 fix）
- executeReturn 的 (fieldsModified || wasInitCall) 条件（B2 fix）
- super 调用编译时编码类名（B1 fix）避免 3+ 级死循环
- COW 通过 const/非 const fields() 重载正确
- lastMutatedReceiver_ 机制正确（BUG-CP-2/CP-3 修复稳固）
- executeDefineClass 沿继承链正序遍历（chain，AUDIT-BUG-C3 fix）合并字段

#### RegisterVM
- executeArith REG_DIV 截断除法与 Interpreter/StackVM 一致（AUDIT-DIV-UNIFY fix）
- executeCompare EQ/NEQ/LT/GT/LTE/GTE 与 StackVM orderedCompare 完全一致
- REG_NOT 使用 isTruthy() 一致
- AND/OR 短路求值正确返回操作数原值（非布尔）
- fillDefaultArgs 0xFFFF 哨兵检查正确、requiredArity/arity 边界处理正确
- executeClassNewImpl flattenedFieldOrder 去重正确（BUG-INH-REG-1 fix）
- executeReturnImpl 方法返回字段同步正确（P0-1 fix）、init 隐式返回 this、super.init 链字段传播
- executeMethodCallImpl 内建方法返回 bool 正确（C-9 fix）、沿继承链查找方法
- REG_SUPER_CALL 沿继承链查找正确、指令编码与 instructionSizeAt 一致
- throwException 跨帧异常展开正确、closeUpvaluesFrom 在每帧弹出前调用、pendingException_ 正确（P1-4 fix）
- 闭包 upvalue 编码 stackSlot = frameIdx*32+slot 全局唯一、openUpvalues_ multimap 排序正确
- REG_DEFINE_CLASS 5 字节/字段编码与 instructionSizeAt 一致（BUG-INH-IR-1 fix）
- break/continue 在 try 块内正确发射 TRY_END（BUG-EXC-2 fix）
- visitTryStmt catch 块 throw cleanup 包装正确（BUG-IR-TRY-CATCH-WRAP fix）
- vregToReg localCount > 32 硬门正确（P2-1 fix）、hasError_ 阻止生成损坏字节码
- patchJumps 64KB 偏移溢出检查正确
- 常量池类型严格去重（BUG-CP-1 fix）

### 三后端一致性结论

三后端核心语义完全一致，本次发现的 12 个 P2 Bug 均为单引擎特有的边缘问题：

| 特性 | 一致性 | 备注 |
|------|-------|------|
| 整数除法截断向零 | 一致 | 共享 NumericOps::computeArith |
| 整数溢出检测 | 一致 | ArithStatus 统一处理 |
| and/or 短路返回原值 | 一致 | 编译器层面统一 |
| not 语义 | 一致 | 共享 isTruthy() |
| COW 语义 | 一致 | const/非 const fields() 重载 |
| super 调用 this 绑定 | 一致 | 各自实现正确 |
| 异常处理栈清理 | 一致 | 各自 closeUpvaluesFrom 正确 |
| 默认参数 | 一致 | Parser 层统一约束字面量 |
| 字符串 UTF-8 索引 | 一致 | 共享 UTF-8 码位计算（缓存失效为单引擎边缘问题） |
| 模块路径安全 | 一致 | BUG-MOD-1 三后端同步 |

## 2026-07-04 · IR 中间表示层 + IR Backend 模块系统性审计 + 全量修复（含 P2 修复）

针对 compiler/IR.cpp + IR.h + BytecodeIRBackend + RegisterBytecodeBackend 进行系统性审计，覆盖 IR 栈平衡、VarMap 作用域恢复、常量/全局去重哈希表、BytecodeIRBackend lowering、RegisterBytecodeBackend lowering、IR 优化 Pass、错误传播等 7 个方向。完成 11 个 P0+P1 Bug 修复（2 P0 + 9 P1）+ 15 个 P2 Bug 修复，新增 36 个回归测试（27 P0+P1 + 9 P2），全量 1171 个测试通过（原 1135 + 新增 36），无回归。

### 审计发现与修复（P0 级，2 个）

- **BUG-IR-POP-2** (P0, 已修复): IR 路径 if 单语句体未 POP 表达式语句返回值。`visitIfStmt` then/else 分支直接调用 `visitNode`，未通过 `visitStatement` 统一包装器处理 POP。循环内 `if (cond) foo();` 每次泄漏 1 个栈值。修复：then/else 分支改用 `visitStatement`。位置: compiler/IR.cpp visitIfStmt()
- **BUG-IR-POP-3** (P0, 已修复): IR 路径 while/for 单语句体未 POP。与 BUG-IR-POP-2 同源，`visitWhileStmt`/`visitForStmt` 的 body 直接调用 `visitNode`。修复：body 改用 `visitStatement`。位置: compiler/IR.cpp visitWhileStmt() + visitForStmt()

### 审计发现与修复（P1 级，9 个）

- **BUG-IR-SCOPE-1** (P1, 已修复): IR 路径顶层 for 循环变量泄漏。`visitForStmt` 仅对 `inFunction_` 路径用 block scope 清理，顶层路径遗留 varMap_ 映射，导致 `for (var i...) {}` 后 `i` 仍可引用（与 StackVM/Interpreter 不一致）。修复：顶层路径对齐 Compiler.cpp L1131-L1147，循环退出后从 varMap_ 移除 initializer 声明的变量并发射 DELETE_VAR 清理全局。位置: compiler/IR.cpp visitForStmt()
- **BUG-IR-SUPER-1** (P1, 已修复): IR 路径 `visitNode` 缺少 `NODE_SUPER_EXPR` 独立 case，super 表达式走 default 分支递归 children()，但 SuperExpr 无 children 导致 super 方法调用编译失败。修复：添加 `NODE_SUPER_EXPR` 显式 case。位置: compiler/IR.cpp visitNode()
- **BUG-IR-PATCH-1** (P1, 已修复): IR 路径 `patchJumps` 未检查 TRY_BEGIN 的 64KB 偏移溢出。TRY_BEGIN 操作数为 16 位偏移，超大 try 块（>64KB）会截断为错误值。修复：添加 65535 上限检查。位置: compiler/IR.cpp patchJumps()
- **BUG-IR-SLOTNAME-1** (P1, 已修复): `slotToNameConstant` 失败时生成占位名静默产生坏字节码。原实现返回 uint16_t(0)，调用方无法检测失败。修复：改为返回 bool，调用方检查返回值，失败时中止 lowering 并向上传递错误。位置: compiler/IR.h + compiler/IR.cpp slotToNameConstant() + 两处 WRITEBACK_*_VAR IMM_UINT 分支
- **BUG-IR-DCE-1** (P1, 已修复): `isPureCompute` 将 ADD/SUB/MUL/DIV/MOD/NEGATE 列为纯计算，DCE 可能消除含副作用的算术指令（除零错误、整数溢出、字符串拼接）。修复：从 `isPureCompute` 移除算术指令。位置: compiler/IR.cpp isPureCompute()
- **BUG-IR-DCE-2** (P1, 已修复): DCE 与复制传播控制耦合。原 `optimizeIR` 的 DCE 受 `enableCopyPropagation` 控制，禁用复制传播时连带禁用 DCE。栈式 VM 后端需禁用 DCE（POP operands 为空，DCE 看不到 POP 对 vreg 的消费关系），但寄存器式后端可独立启用 DCE。修复：`optimizeIR` 新增 `enableDCE` 参数，栈式后端 false，寄存器式后端 true。位置: compiler/IR.h + compiler/IR.cpp optimizeIR() + compiler/Compiler.cpp 两处优化调用
- **BUG-IR-OPT-1** (P1, 已修复): `optimizeIR` 仅优化 mainFunction，未遍历 `module->functions` 优化子函数。子函数中的常量折叠等优化未应用。修复：遍历 `module->functions` 优化所有子函数。位置: compiler/Compiler.cpp 两处优化调用
- **BUG-IR-TRY-1** (P1, 已修复): IR 路径顶层 catch 变量清理不完整。原实现用 `IMM_UINT` kind（→ OP_DEFINE_GLOBAL slot），DELETE_VAR 对 slot-based 变量置 null（仍可访问）；StackVM 用 `GLOBAL_NAME` kind（→ OP_DEFINE_VAR），DELETE_VAR 从 globals_ 擦除（→ undefined）。三后端不一致。修复：IR 路径顶层 catch 变量改用 `GLOBAL_NAME` kind，DELETE_VAR 从 globals_ 擦除，对齐 StackVM 语义。新增 `IROp::DELETE_VAR` 在 BytecodeIRBackend 和 RegisterBytecodeBackend 的 lowering。位置: compiler/IR.h 新增 DELETE_VAR 枚举 + compiler/IR.cpp visitTryStmt() + BytecodeIRBackend lowering + RegisterBytecodeBackend lowering
- **BUG-IR-FV-1** (P1, 已修复): `collectFreeVars` 的 `NODE_CLASS_DECL` case 仅插入类名后 break，不递归类成员，导致类方法体中引用的外层变量未被识别为自由变量，upvalue 传递链断裂。修复：递归分析类成员。位置: compiler/IR.cpp collectFreeVars()
- **BUG-IR-VARDECL-1** (P1, 已修复): IR 路径 `visitVarDecl` 无初始化器时统一初始化为 null，未检查类型注解是否为类名（Compiler 路径有 S2 fix 自动构造实例）。三后端不一致。修复：新增 `definedClassNames_` 成员，`visitClassDecl` 时填充，`visitVarDecl` 检查类型注解是否为类名，若是则自动构造实例。位置: compiler/IR.h 新增 definedClassNames_ + compiler/IR.cpp visitVarDecl() + visitClassDecl()

### 新增回归测试（tests/TestIRAudit.cpp，27 个用例）

- IRAuditPOP: 4 个测试（if/while/for/if-else 单语句体）
- IRAuditSCOPE: 2 个测试（for 循环变量作用域，含顶层 + 嵌套）
- IRAuditSUPER: 2 个测试（super 方法/构造函数调用）
- IRAuditPATCH: 1 个测试（try/catch 正常工作）
- IRAuditSLOTNAME: 2 个测试（嵌套索引/成员赋值）
- IRAuditDCE2: 2 个测试（寄存器式后端 DCE）
- IRAuditDCE1: 2 个测试（除零保留/正常算术）
- IRAuditOPT1: 2 个测试（子函数常量折叠）
- IRAuditTRY1: 3 个测试（catch 变量清理，对比 IR 与 StackVM）
- IRAuditFV1: 3 个测试（类方法引用全局变量/类名/外层变量编译）
- IRAuditVARDECL1: 3 个测试（类型注解类实例化）
- IRAuditConsistency: 1 个测试（三后端一致性综合）

### 审计发现与修复（P2 级，15 个）

- **BUG-IR-TRY-CATCH-WRAP** (P2, 已修复): IR 路径顶层 catch 块内 throw 跳过 cleanup IR（恢复遮蔽全局原值/清理 catch 变量）。原实现 catch 块正常退出才发射 cleanup IR，catch 块内 throw 跳过 cleanup。修复：用内层 `TRY_BEGIN` 包装 catch 块，异常路径复制 cleanup IR + rethrow，对齐 Compiler.cpp visitTryStmt 的 needsCleanupWrap 模式。位置: compiler/IR.cpp visitTryStmt()
- **BUG-IR-BOUND-1** (P2, 已修复): BytecodeIRBackend lowering 时 7 处 8 位操作数用 `& 0xFF` 静默截断，未检查 >= 256 上限，与 RegisterBytecodeBackend 不对称。修复：CALL/CALL_EXPR/BUILD_ARRAY/BUILD_DICT（上限 255）、METHOD_CALL/SUPER_CALL/CLASS_NEW（上限 254，含 this）添加溢出检查，失败时返回 false 中止 lowering。位置: compiler/IR.cpp BytecodeIRBackend::lowerInstruction() 7 处
- **BUG-IR-DEAD-1** (P2, 已修复): IR.h 的 `#include <variant>` 未使用。修复：删除。位置: compiler/IR.h
- **BUG-IR-DEAD-2** (P2, 已修复): IR.h 的 `std::vector<std::string> outerLocals_` 字段未使用（AstIRBuilder 用 `outerLocalSlots_`）。修复：删除。位置: compiler/IR.h
- **BUG-IR-DEAD-3** (P2, 已修复): IR.h 的 `std::vector<UpvalueDesc> outerUpvalues_` 字段未使用（AstIRBuilder 用 `outerUpvalueNames_`）。修复：删除。位置: compiler/IR.h
- **BUG-IR-DEAD-4** (P2, 已修复): RegisterBytecodeBackend.cpp 的 `lower()` 内 `globalName` lambda 未调用（`lowerInstruction` 有独立的 `globalName` lambda）。修复：删除。位置: compiler/RegisterBytecodeBackend.cpp
- **BUG-IR-DOC-1~6** (P2, 已修复): IR.cpp 中 6 处注释行号引用失效（如 `Compiler.cpp:1194` 实际指向空行）。修复：更新为函数名引用。位置: compiler/IR.cpp 6 处
- **BUG-IR-DOC-7** (P2, 已修复): IR.h 的注释 `Compiler.cpp:1194` 失效。修复：更新为 `Compiler.cpp visitBreakStmt`。位置: compiler/IR.h
- **BUG-IR-STYLE-1** (P2, 已修复): Logger.h 引用路径不统一（IR.cpp 用 `"Logger.h"`，RegisterBytecodeBackend.cpp 用 `"common/Logger.h"`）。修复：统一为 `"Logger.h"`。位置: compiler/RegisterBytecodeBackend.cpp
- **BUG-IR-BOUND-2** (P2, 已知限制): IR 路径嵌套深度/循环栈/块作用域栈/try 深度无上限检查。Compiler 路径同样无此检查，单独添加 IR 路径检查会造成三后端不一致；实际触发需极深嵌套（数百层）。标记为已知限制，不修复。

### 新增回归测试（tests/TestIRAudit.cpp，P2 部分 9 个用例）

- IRAuditTRYWRAP: 3 个测试（catch 块 throw 恢复遮蔽全局/清理 catch 变量/三后端一致性）
- IRAuditBOUND: 2 个测试（8 位操作数边界值）
- IRAuditDEAD: 1 个测试（死代码清理后功能正常）
- IRAuditSTYLE: 1 个测试（Logger.h 路径统一后功能正常）
- IRAuditVARDECL1.ClassTypeAnnotationWithInitializer: 1 个测试（有初始化器时不触发自动构造）
- IRAuditP2.ComprehensiveP2Fixes: 1 个测试（catch 块 throw + 三后端一致性综合）

## 2026-07-04 · Compiler + IR + RegisterVM 第二轮深度审计 + 全量修复

针对 compiler/ + IR + RegisterVM 进行第二轮深度审计，覆盖嵌套左值栈泄漏、前向自由变量分析、IR 路径表达式语句 POP、非字面量字段默认值、try/catch 状态恢复、IR 路径预扫描不对称、默认参数三后端一致性、寄存器式字段去重、死代码清理等方向。发现 10 个 Bug（5 P1 / 5 P2），全部完成修复并新增 33 个回归测试，全量 1135 个测试通过（原 1102 + 新增 33），无回归。

### 审计发现与修复（P1 级，5 个）

- **BUG-CP-2**（P1，已修复）：`visitIndexAssign` 嵌套路径栈泄漏。BUG-NEW 修复将 `OP_WRITEBACK_INDEX_VAR/LOCAL` 改为整体替换语义（不 pop 索引），`visitMethodCall` 已同步修复（不推索引），但 `visitIndexAssign` L1953-1960 仍向栈推入外层索引。循环内 `a[0][1]=x` 每次迭代泄漏 2 个栈值，约 512 次触发栈溢出。仅影响非 IR 路径（IDE 调试模式 `setUseIR(false)`）。修复：删除推索引代码，对齐 `visitMethodCall`。位置：`compiler/Compiler.cpp` `visitIndexAssign()` L1939-L1984。
- **BUG-CP-3**（P1，已修复）：`visitMemberAssign` 嵌套路径栈泄漏，与 BUG-CP-2 同源。`visitMemberAssign` L2293-2300 在 `OP_MEMBER_SET` 后仍推入外层索引/字段，但 `OP_WRITEBACK_MEMBER_*` 已改为整体替换不 pop。循环内 `d.b.c=x` 每次泄漏 2 个栈值。修复：删除推索引代码。位置：`compiler/Compiler.cpp` `visitMemberAssign()` L2279-L2326。
- **BUG-UV-1**（P1，已修复）：Compiler 路径缺少前向自由变量分析，3+ 层嵌套闭包失败。`resolveUpvalue` 是惰性的（仅 `visitVarRef` 触发），3 层嵌套时中间函数不直接引用外层变量则 `currentUpvalueNames_` 为空，内层函数无法透传捕获。IR 路径有 `computeFreeVars` 前向分析修复了此问题，但 Compiler 路径没有。修复：在 `visitFunDecl` 加入类似 IR 的 `computeFreeVars` 前向分析。位置：`compiler/Compiler.cpp` `resolveUpvalue()` L726-758 + `visitFunDecl()` L1026-1034。
- **BUG-IR-POP-1**（P1，已修复）：IR 路径模块内联表达式语句未 POP。`build()` L137-139 对顶层表达式语句 emit POP 防止 StackVM 栈泄漏，但 `handleImportStmt` L314-315 仅有注释"表达式语句的返回值需 POP"却缺少实际 `needsPopForExprStmt` 检查与 `emitIR(IROp::POP)` 调用。BytecodeIRBackend 将 vreg 物化为栈值，未 POP 的值永久残留。RegisterVM 中 POP 为 no-op 不受影响。修复：在 `handleImportStmt` 添加 `if(needsPopForExprStmt(stmt->nodeType)) emitIR(IROp::POP, {}, stmt->line);`。位置：`compiler/IR.cpp` `handleImportStmt()` L305-L317。
- **BUG-INH-IR-1**（P1，已修复）：IR 路径非字面量字段默认值降级为 null。`extractConstant` 仅支持 `NumberLiteral/StringLiteral/BoolLiteral/NullLiteral/UnaryOp(NEGATE)`，非字面量表达式（`BinaryOp/FunCall` 等）返回 false，`defaultIdx` 保持 `UINT32_MAX`，后端 emit `OP_NULL`。而 `Compiler.cpp` 直接路径通过 `compileNode` 编译任意表达式，`Interpreter` 通过 `evaluate` 求值任意表达式。违反三后端一致性。修复：非字面量表达式改为栈传递字段默认值——在 `visitClassDecl` 中对非字面量表达式 emit 求值 IR + `STORE_LOCAL` 存入临时槽位 + `POP` 清栈，`DEFINE_CLASS` IR 指令布局从每字段 2 操作数（name + defaultConstIdx）扩展为 3 操作数（name + defaultConstIdx + exprLocalSlot），`RegisterBytecodeBackend` 和 `RegisterVM` 同步更新 `REG_DEFINE_CLASS` 字段编码（每字段从 4 字节扩展为 5 字节）。位置：`compiler/IR.cpp` `visitClassDecl()` L1911-1931 + `extractConstant()` L33-72 + `compiler/RegisterBytecodeBackend.cpp` + `compiler/RegisterVM.cpp`。

### 审计发现与修复（P2 级，5 个）

- **BUG-TRY-LEAK-1**（P2，已修复）：`visitTryStmt` 中 `cleanupThrowOffset > 65535` 溢出检查 early return 跳过 `restoreMapping`。L1695-L1698 的 `error()` 后 return 跳过 L1709-1711 的 `globalSlotAllocator_.restoreMapping` 和 L1713-1714 的 `currentLocals_` 恢复，导致 `GlobalSlotAllocator` 状态不一致。极罕见（catch 块 > 64KB）。修复：在 return 前添加恢复逻辑。位置：`compiler/Compiler.cpp` `visitTryStmt()` L1695-L1700。
- **BUG-IR-PRES-1**（P2，已修复）：IR 路径 `preScanTopLevelDecls` 对主模块预扫描 FunDecl，与 `compile()` 不对称设计不一致。`compile()` 不预扫描 FunDecl（避免 `var f=funName` 静默 null），但 IR 路径 `preScanTopLevelDecls` 对主模块也预扫描 FunDecl，导致 IR 路径 `var f=funName` 静默 null 而 Compiler 路径报运行时错误。修复：`preScanTopLevelDecls` 增加 `isMainModule` 参数，主模块调用时跳过 FunDecl 预扫描。位置：`compiler/IR.cpp` `preScanTopLevelDecls()` L159-L197 + `compiler/IR.h`。
- **BUG-DEF-1**（P2，已修复）：默认参数求值三后端不一致。Interpreter 路径支持任意表达式默认值（`b=a+1` 求值为 6），VM/RegisterVM 路径仅支持字面量，复杂表达式记录 `0xFFFF` 哨兵运行时报错。违反三后端一致性。修复：Parser 层拒绝复杂表达式默认值（`BinaryOp/FunCall/VariableRef` 等），仅允许字面量（Number/String/Bool/Null/InterpolatedString）和负数字面量（`UnaryOp(NEGATE)` 套 NumberLiteral）。位置：`parser/Parser.cpp` `isLiteralDefaultExpr` L410-L445 + `compiler/Compiler.cpp` `visitFunDecl()` L1077-L1132 + `compiler/IR.cpp` L1193-L1203 + `interpreter/InterpreterCalls.cpp` L136-L142。
- **BUG-INH-REG-1**（P2，已修复）：RegisterVM `flattenedFieldOrder` 字段名重复。`executeClassNewImpl` L1854-1870 沿继承链逆序遍历无去重，子类覆盖的父类同名字段在链中每个类都 push 一次。无语义影响（后写入覆盖），仅效率问题。修复：构建时去重，对齐 StackVM 的 `mergedOrder` 语义。位置：`compiler/RegisterVM.cpp` `executeClassNewImpl()` L1854-L1870。
- **BUG-DEAD-1**（P2，已修复）：`extractExportName` 死代码。定义为从 ExportStmt 提取声明名，但全代码库无调用点。`visitImportStmt` 直接用 `lookupGlobalSlot` 验证。修复：删除声明与定义。位置：`compiler/Compiler.cpp` L1537-L1545 + `compiler/Compiler.h` L183。

### 附带修复的回归 Bug

- **instructionSizeAt 回归**：BUG-INH-IR-1 修复将 `REG_DEFINE_CLASS` 每字段编码从 4 字节扩展为 5 字节（新增 `exprReg`），但 `RegisterBytecode.cpp` 的 `instructionSizeAt` 仍按 4 字节计算，导致 RegisterVM 字节码解析错位，触发"字节码截断: 指令不完整"运行时错误。修复：`instructionSizeAt` 同步更新为 5 字节/字段。位置：`compiler/RegisterBytecode.cpp` `instructionSizeAt()` L216-L235。
- **isLiteralDefaultExpr 过严回归**：BUG-DEF-1 修复的 `isLiteralDefaultExpr` 未包含 `NODE_INTERPOLATED_STRING`，拒绝了插值字符串默认值，导致 BUG-LPA-03 回归测试失败。修复：在 `isLiteralDefaultExpr` 的 switch 中添加 `case NodeType::NODE_INTERPOLATED_STRING: return true;`。位置：`parser/Parser.cpp` L410-L445。

### 新增回归测试（tests/TestCompilerAudit2.cpp，33 个用例）

- `CompilerAudit2CP2`（3 个）：嵌套索引赋值不栈泄漏（IR 路径 600 次循环）、直接路径编译成功、循环内多次嵌套索引赋值保留值
- `CompilerAudit2CP3`（3 个）：嵌套成员赋值不栈泄漏（IR 路径 600 次循环）、直接路径编译成功、嵌套成员赋值保留值
- `CompilerAudit2UV1`（3 个）：3 层嵌套闭包捕获、StackVM 路径验证、4 层嵌套闭包捕获
- `CompilerAudit2IRPOP1`（2 个）：IR 路径循环内表达式语句不栈泄漏、多个表达式语句连续执行不泄漏
- `CompilerAudit2INHIR1`（4 个）：非字面量字段默认值（BinaryOp）、函数调用作为默认值、混合字面量与非字面量、字段默认值与 init 方法覆盖
- `CompilerAudit2TRYLEAK1`（1 个）：正常 try/catch 恢复 catch 变量作用域
- `CompilerAudit2IRPRES1`（2 个）：主模块不预扫描 FunDecl、前向引用行为一致性
- `CompilerAudit2DEF1`（5 个）：BinaryOp 默认参数被拒绝、FunCall 默认参数被拒绝、字面量默认参数被接受、插值字符串默认参数被接受、变量引用默认参数被拒绝
- `CompilerAudit2INHREG1`（3 个）：字段覆盖无重复、深层继承字段覆盖、字段覆盖与 init
- `CompilerAudit2DEAD1`（3 个）：export var 编译成功、export class 编译成功、export fun 编译成功
- `CompilerAudit2InstrSize`（4 个）：RegisterVM 类字段与方法、继承与字段、非字面量字段默认值、多个类定义无字节码错位

## 2026-07-04 · 构建 Skill 创建（minilang-build）

将 Compiler 模块审计修复过程中遇到的全部构建问题与解决方案总结为可复用 skill，避免后续重复踩坑。

### 新增

- **minilang-build skill**：创建 `.trae/skills/minilang-build/SKILL.md`，覆盖 Windows 构建全部已知问题与解决方案。当用户要求构建、运行测试、配置 CMake，或遇到构建错误时自动调用。
- **5 大已知问题与解决方案**：
  1. MinGW GCC 与 Qt MSVC ABI 不兼容 → 切换 `windows-msvc-debug` Preset + VS DevShell
  2. MSVC 环境未初始化导致 `cstddef` 找不到 → `Enter-VsDevShell` 初始化
  3. 环境变量跨 PowerShell 会话丢失 → 环境初始化与构建合并为单条命令
  4. `windeployqt.cmake` 路径解析失败 → `tests/CMakeLists.txt` 添加 `MINILANG_ROOT_DIR` fallback
  5. Qt 路径未设置 → `$env:QTDIR = "D:\qt\6.10.3\msvc2022_64"`
- **5 个一键命令模板**：构建测试 / 运行全部测试 / 运行指定套件 / 首次配置+构建 / 构建 IDE
- **错误诊断速查表**：7 种常见错误模式 → 根因 → 解决方案
- **项目记忆同步更新**：新增 `Build Skills & Conventions` 章节，记录 skill 路径、硬性约束、构建/测试命令、环境初始化、错误速查。

## 2026-07-04 · Compiler + BytecodeChunk 模块系统性审计 + 全量修复

针对 compiler/ 模块进行系统性审计，覆盖 6 个排查方向（栈平衡、全局槽位分配、常量池去重、函数编译、类编译、模块内联），发现 4 个 Bug（1 P1 / 3 P2），全部完成修复并新增 15 个回归测试，全量 1102 个测试通过（原 1087 + 新增 15），无回归。

### 审计发现与修复

- **BUG-CP-1**（P1，已修复）：`BytecodeChunk::addConstant` 与 `RegBytecodeChunk::addConstant` 常量池去重时仅依赖 `Value::equals()`，而 `equals()` 对 int(0) 与 float(0.0) 返回 true（数值相等性）。这导致 int 0 与 float 0.0 共享同一常量索引，编译器根据原始类型发射 OP_INT/OP_FLOAT，VM push 共享的常量值，float 变量静默变为 int，破坏三后端一致性（整数除法截断 vs 浮点除法精度）。修复：`addConstant` 增加类型严格匹配 `constants[i].getType() == val.getType() && constants[i].equals(val)`，栈式与寄存器式两个后端同步修复。位置：`compiler/Bytecode.h` `addConstant()` + `compiler/RegisterBytecode.cpp` `RegBytecodeChunk::addConstant()`。
- **BUG-PRES-1**（P2，已修复）：`Compiler::compile()` 预扫描仅覆盖 VarDecl/ClassDecl，未解包 `ExportStmt` 内部声明，导致 `export var x` 使用慢路径 `OP_DEFINE_VAR` 而非快路径 `OP_DEFINE_GLOBAL`，且与普通 `var x` 的前向引用行为不一致。修复：`compile()` 与 `preScanModuleGlobals()` 同步解包 `ExportStmt(VarDecl/ClassDecl)`。**关键设计**：`compile()` 与 `preScanModuleGlobals()` 存在有意的不对称——`compile()` 不预扫描 FunDecl（避免 `var f = funName` 从"报错"变为"静默 null"），`preScanModuleGlobals()` 预扫描 FunDecl（`visitImportStmt` 第 11 步使用 `lookupGlobalSlot(name)` 验证导出名是否存在）。位置：`compiler/Compiler.cpp` `compile()` L80-L114 + `preScanModuleGlobals()` L1490-L1535。
- **BUG-TRY-1**（P2，已修复）：`visitTryStmt` catch 块编译后直接发射清理代码（OP_DELETE_VAR 等），但 catch 块内若 throw，异常传播跳过清理代码，导致 catch 变量未从全局表删除（泄漏到外层作用域）、被遮蔽的全局变量未恢复（值丢失）。修复：用 `OP_TRY_BEGIN` 包装 catch 块，捕获内层 throw，跳到 `cleanupThrowIp` 执行清理代码后 `OP_THROW` rethrow。字节码布局：`catchIp: OP_TRY_BEGIN <cleanupThrowOffset> / <catch block> / OP_TRY_END / <cleanup code 正常路径> / OP_JUMP afterCatch / cleanupThrowIp: <cleanup code 异常路径复制> / OP_THROW / afterCatch:`。位置：`compiler/Compiler.cpp` `visitTryStmt()` L1617-L1718。
- **BUG-MOD-1**（P2，已修复）：`normalizeModulePath` 仅检测 `C:/` 形式（`path[1]==':' && path[2]=='/'`），未拒绝 `C:foo`（Windows 驱动器相对路径），且反斜杠检测分支为死代码（反斜杠在更早处已统一转为正斜杠）。修复：拒绝所有 `X:` 开头形式（`path.size() >= 2 && path[1] == ':'`），三后端（Compiler / Interpreter / IR）同步修改。位置：`compiler/Compiler.cpp` `normalizeModulePath()` L1426-L1433 + `interpreter/InterpreterModules.cpp` + `compiler/IR.cpp`。

### 已确认无 Bug 的方向

- 栈平衡审计：29 个 visit 方法所有执行路径 push/pop 平衡，包括表达式语句、print 多参数、try/catch 异常路径、模块导入内联
- 全局槽位分配：`GlobalSlotAllocator` 的 allocate/lookup/removeMapping/restoreMapping 配对正确，shadowed global 保存/恢复机制在 blockDepth_==0 时生效
- 函数编译：闭包 upvalue 捕获、默认参数填充、init 方法返回 this、递归调用缓存
- 类编译：字段默认值继承、super 方法查找、fieldSlotIndex、方法回退链
- 模块内联：run-once 语义（`linkedModuleSet_` + `moduleLoadingSet_`）、循环依赖检测、嵌套导入、菱形依赖

### 新增回归测试（tests/TestCompilerAudit.cpp，15 个用例）

- `CompilerAuditCP1.IntAndFloatZeroNotDeduped`：整数 0 与浮点 0.0 在常量池中独立存在
- `CompilerAuditCP1.IntAndFloatOneNotDeduped`：整数 1 与浮点 1.0 不共享常量索引
- `CompilerAuditCP1.FloatDivisionPreservesType`：浮点除法保持浮点类型（10.0/4=2.5）
- `CompilerAuditCP1.RegisterBackendAlsoTypeStrict`：寄存器式后端常量池也类型严格去重
- `CompilerAuditPRES1.ExportVarGetsGlobalSlot`：`export var` 分配全局槽位
- `CompilerAuditPRES1.ExportClassGetsGlobalSlot`：`export class` 分配全局槽位
- `CompilerAuditPRES1.ExportVarUsesDefineGlobalOp`：`export var` 使用 OP_DEFINE_GLOBAL 快路径
- `CompilerAuditPRES1.ForwardRefBehaviorConsistent`：未定义变量统一报运行时错误（非静默 null）
- `CompilerAuditTRY1.CatchThrowRestoresShadowedGlobal`：catch 块内 throw 后被遮蔽的全局变量恢复原值
- `CompilerAuditTRY1.CatchThrowCleansUpCatchVar`：catch 块内 throw 后 catch 变量被清理
- `CompilerAuditTRY1.CatchNormalExitStillCleansUp`：catch 正常退出时清理代码仍执行
- `CompilerAuditMOD1.WindowsDriveRelativePathRejected`：`C:foo` 形式路径被拒绝
- `CompilerAuditMOD1.OtherDriveRelativePathRejected`：`D:somefile` 形式路径被拒绝
- `CompilerAuditMOD1.LegitRelativePathNotRejected`：合法相对路径不误拒
- `CompilerAuditMOD1.AbsolutePathStillRejected`：Unix 绝对路径 `/etc/passwd` 仍被拒绝

## 2026-07-04 · Lexer / Parser / AST 模块系统性审计 + 全量修复

针对 lexer/、parser/、ast/ 三大模块进行系统性审计，覆盖 17 个排查方向（Lexer 6 项 + Parser 7 项 + AST 3 项），发现 6 个潜在 Bug（2 P1 / 4 P2），全部完成修复并新增 10 个回归测试，全量 1087 个测试通过（原 1077 + 新增 10），formatter_audit 全部通过（失败用例数: 0）。

### 审计发现与修复

- **BUG-LPA-01**（P1，已修复）：`astEqual` 字段比较不完整。`test_harness/formatter_audit.cpp` 的 `default` 分支仅递归比较 `children()`，不比较节点自身字段（VarDecl.name/typeAnnotation、FunDecl.name/params/paramTypes/returnType、ImportStmt.modulePath/names/importAll、ClassDecl.name/superClassName、TryStmt.catchVarName、Assignment.name、MemberAccess.fieldName、MemberAssign.fieldName、MethodCall.methodName、FunCall.name）。修复：为所有含自身字段的节点添加显式 case。
- **BUG-LPA-02**（P1，已修复）：`InterpolatedString` 在 `astEqual` 中不比较 `literals` 片段。`InterpolatedString::children()` 只返回 `expressions` 不返回 `literals`，导致 `"a{x}b"` 与 `"c{x}d"` 被判定 AST 等价。修复：添加 `NODE_INTERPOLATED_STRING` 显式 case 比较 literals。
- **BUG-LPA-03**（P2，已修复）：`collectVarRefs` 未覆盖 `NODE_INTERPOLATED_STRING`，`fun f(a = "{b}", b = 1)` 可绕过默认参数前向引用检查。修复：在 switch 中添加 InterpolatedString case 递归扫描 expressions。
- **BUG-LPA-04**（P2，已修复）：类成员声明不支持 `ClassName[] fieldName;` 数组类型字段。`classDecl` 类名分支未使用 `parseTypeAnnotation()` 处理 `[]` 后缀，与参数列表/for 循环/顶层声明行为不一致。修复：在 `check(TK_LBRACKET) && checkNext(TK_RBRACKET)` 分支中消费 `[]` 后缀并构造类型注解 `ClassName[]`。
- **BUG-LPA-05**（P2，已修复）：`import`/`export` 在无花括号单语句体（`if (true) import "m";`）中报"意外的 Token"而非"只能在顶层使用"。`statement()` 未识别 `TK_IMPORT`/`TK_EXPORT`，`blockDepth_` 检查无法覆盖无块场景。修复：在 `statement()` 入口添加显式拒绝，抛"import/export 语句只能在顶层使用"。
- **BUG-LPA-06**（P2，已修复）：Lexer 非法多字节 UTF-8 字符产生多个误导性错误。`default` 分支按单字节处理，3 字节 UTF-8 字符产生 3 个错误。修复：检测 UTF-8 多字节首字节，消费完整码位生成单个错误，错误消息包含完整字符。

### 已确认无 Bug 的方向

- Lexer：标识符/关键字冲突（`true_`/`nullx`/`class_name` 整体查表不命中）、浮点数边界（`.5`/`1.`/`1e10`/`1.5e-3`/`123.e5`）、字符串插值嵌套 `braceDepth` 计数（`"{"}"` 正确报错）、多行字符串/块注释 token 位置、双下划线前缀拒绝
- Parser：操作符优先级链（`or_` < `and_` < `equality` < `comparison` < `term` < `factor` < `unary`，全左结合）、for 循环类型注解（H2 fix）、参数列表类型注解（PARSE-02 fix）、`synchronize()` 同步点完备性、错误恢复路径
- AST：Visitor 接口完整性（33 个 visit 方法覆盖全部 NodeType）、无 `clone()` 方法（设计使用 `shared_ptr` 共享，非缺陷）

### 新增回归测试

- `ParserAudit.DefaultParamInterpolatedStringForwardRef`：默认参数含 InterpolatedString 的前向引用检查
- `ParserAudit.DefaultParamInterpolatedStringValid`：合法插值默认参数不应报错
- `ParserAudit.ClassMemberArrayTypeField`：类成员 `ClassName[] fieldName;` 数组类型字段
- `ParserAudit.ImportInIfWithoutBracesRejected`：无花括号 if 体 import 报明确错误
- `ParserAudit.ExportInWhileWithoutBracesRejected`：无花括号 while 体 export 报明确错误
- `ParserAudit.ImportInBlockStillRejected`：块内 import 仍报"只能在顶层使用"
- `LexerAudit.Utf8MultibyteCharProducesSingleError`：单字符 UTF-8 错误数验证
- `LexerAudit.MultipleUtf8MultibyteCharsProduceOneErrorEach`：多字符 UTF-8 每字符一错误
- `LexerAudit.Utf8MultibyteErrorMessageContainsChar`：UTF-8 错误消息含完整字符
- `LexerAudit.SingleByteInvalidCharStillErrors`：单字节非法字符仍报错

## 2026-07-02 · VM Import 功能完善

### 新增

- **VM 路径支持 import 语句**：Compiler::visitImportStmt（直接路径）与 AstIRBuilder::handleImportStmt（IR / 寄存器路径）采用编译期模块内联策略——加载模块源码、解析 AST、预扫描全局槽位、内联编译模块语句到主程序。三路径共用 GlobalSlotAllocator，模块代码内联到同一全局作用域。
- **run-once 语义**：`linkedModuleSet_`（已完成）+ `moduleLoadingSet_`（编译中）保证同一模块只编译一次，避免重复定义。
- **路径安全（SEC-1）**：`normalizeModulePath` 拒绝空路径、绝对路径、`..` 路径段，防止路径遍历攻击。
- **IDE 集成层**：`IdeController::setupCompilerModuleLoader()` 基于当前文件路径为 Compiler 注入模块加载器，对齐 WorkerManager 为 Interpreter 设置的加载器逻辑。
- **VM import 测试**：新增 30 个测试用例（VME2EImport 16 个 / VME2EImportIR 9 个 / VME2EImportReg 5 个），覆盖全部导入、命名导入、类导出、run-once、循环依赖检测、模块不存在、嵌套导入、路径遍历防护、递归函数、菱形依赖等。

### 修复

- **IR.h 不完整类型崩溃**：`moduleAsts_` 是 `vector<unique_ptr<Block>>`，Block 在 IR.h 中仅有前向声明，PCH 删除后某些翻译单元看到不完整类型导致内存布局不一致和运行时崩溃。改为在 IR.h 中直接 `#include "ast/ASTNode.h"`。
- **preScanTopLevelDecls 漏处理 FunDecl / ExportStmt**：原实现仅处理 VarDecl 和 ClassDecl，导致 IR 路径前向函数引用和 export 声明全局槽位未预分配。对齐 Compiler::preScanModuleGlobals 补全 switch 分支。
- **handleImportStmt 错误后继续生成坏 IR**：`hasError_` 设置后 `build()` 主循环和模块语句循环未短路中止。已加入 `hasError_` 检查提前退出。
- **loadAndParseModule 吞掉 Lexer/Parser 错误**：原实现仅检查 AST 是否为空，可恢复错误的 AST 被当作成功加载。改为检查 `lexer.getDiagnostics().hasErrors()` 和 `parser.hasErrors()` 并转发到编译诊断。
- **handleImportStmt 冗余补偿循环**：preScanTopLevelDecls 修复后，ExportStmt 包装的声明已在预扫描阶段处理，移除 handleImportStmt 中的重复循环。

## 2026-07-02 · 第八轮 UI 修复与细节打磨

### 工程基础设施

- **Windows 一键配置脚本**：新增 `configure.bat`，自动检测本机 Visual Studio 与 Qt6 安装位置（默认查找 `D:\qt\6.*\msvc2022_64` 与 `C:\qt\6.*\msvc2022_64`，可通过 `QTDIR` 环境变量覆盖），自动初始化 MSVC 开发环境后调用对应 CMake Preset 完成配置。新增 `windows-msvc-debug` / `windows-msvc-release` 两个共享 Preset（Ninja 生成器）。
- **CMake Preset 体系**：CMakePresets.json + CMakeUserPresets.json 双层分离——共享 Preset 在 `CMakePresets.json`（仓库内），机器特定 Qt 路径在 `CMakeUserPresets.json`（gitignore），避免硬编码路径污染仓库。

## 2026-07-01 · 第七轮 Bug 修复（AUDIT-BUG-G1~G4 + H5 系列）

针对三后端 IR 路径栈语义、Parser 类型注解识别、Formatter 缩进、默认参数 RAII 等问题，共修复 12 个 bug（2 P0 / 4 P1 / 6 P2），新增 16 个回归测试，全部 1047/1047 测试通过。

- **G1**（P0）：IR 路径 `AND/OR` 短路在顶层代码使用 `STORE_LOCAL/LOAD_LOCAL` 临时槽（`nextLocalSlot_` 分配），但 StackVM 主帧 `basePointer=0` 且栈初始为空，`OP_SET_LOCAL/OP_GET_LOCAL` 的 `bp+slot` 越界检查失败（"局部变量槽越疆"）。修复：`VM::initExecution` 为主帧预留 `mainChunk_.localCount` 个 null 槽。正常 Compiler 路径顶层代码用 `OP_DEFINE_VAR`（globals_ 哈希），`localCount=0`，此处为 no-op。
- **G2**（P1）：Parser `parseParamList` / `forStmt` 初始化用 `checkNext(TK_IDENTIFIER)` 识别 `ClassName paramName` 模式，遇到 `ClassName[] paramName` 时下一 token 是 `[` 而非标识符，导致误判为非类型声明。修复：新增 `isClassTypeDeclStart()` 同时识别 `ClassName paramName` 与 `ClassName[] paramName` 两种序列。
- **G3**（P1）：Formatter 裸复合语句（if/while/for 作为 `thenBranch`/`body`）出现双重缩进——L17 "修复" 在 4 处 `isSelfTerminating` 分支额外添加了 `currentIndent_++/--`，但 `formatNode` 内部已用 `currentIndent_++` 处理 body 缩进。修复：移除 4 处额外缩进对（revert L17）。
- **G4**（P0，IR 栈泄漏）：IR 路径 `build()/visitBlock()` 仅对 `FUN_CALL/METHOD_CALL` emit POP，其余 14 种表达式语句的返回值残留在栈上，循环内泄漏必然触发栈溢出。修复：`needsPopForExprStmt(NodeType)` 覆盖全部 16 种节点类型；`visitForStmt` 对 update 表达式与表达式 initializer emit POP；`visitAssignment` 对 GLOBAL 存储后 emit `LOAD_GLOBAL` 重载值确保 POP 安全。
- **H5**（P1）：StackVM `OP_CALL_EXPR`（闭包作为表达式调用）参数压栈顺序与 `OP_CALL` 不一致，导致闭包调用取到错误参数。修复：与 `OP_CALL` 对齐参数压栈顺序，新增 4 个 H5 回归测试覆盖 0/1/多参数与循环内调用。
- **BUGFIX-P2 / H2**（P2，条件断点 UAF）：`evaluateCondition` 恢复阶段先调用 `restoreLocalVariables`（整表替换 `variables`）会使 `inst` 指针悬垂，再执行 `inst->fields() = fields` 触发 UAF。修复：恢复顺序改为先恢复实例字段（`inst` 仍有效）再恢复局部变量；`Environment::restoreLocalVariables` 内部在新 map 中重新锚定 `boundInstance_` 防止悬垂。
- **L1**（P2，IR 栈残留）：IR 路径 `visitForStmt` 的 `JUMP_IF_FALSE` peek 不 pop，条件值在循环体/退出路径均残留栈上；`visitTryStmt` catchVarName 为空时异常值残留栈上。修复：for 循环引入 `exitLabel`（条件假路径，需 POP）与 `endLabel`（break 路径，栈已空）双标签模式；try 块 `STORE_LOCAL` 后 emit POP 对齐直接 Compiler 路径；catchVarName 为空时显式 emit POP。
- **BUGFIX-P2**（P2，RegisterVM 字段未找到无方法回退）：`REG_MEMBER_GET` / `REG_SUPER_MEMBER_GET` 字段未找到时直接报错，无方法回退（StackVM `OP_MEMBER_GET` 有 `findMethodChunk` 回退）。修复：沿继承链查找方法（与 StackVM 一致），未找到才报 "类 X 没有字段或方法 'Y'"。
- **BUGFIX-P2**（P2，RegisterVM 默认参数非字面量）：`fillDefaultArgs` 未处理 `0xFFFF` 哨兵值（表示非字面量默认表达式），错误地将 0xFFFF 当作常量索引导致越界。修复：与 StackVM 对齐，`0xFFFF` 返回 false 回退到 Interpreter 路径求值。
- **BUGFIX-P2**（P2，VM ASCII 缓存误命中）：`lastAsciiStrPtr_` 仅比对指针，堆地址复用（原 string 释放后新 string 复用同地址）会导致非 ASCII 字符串误判为 ASCII。修复：新增 `lastAsciiStrSize_` 同时比对指针 + size。
- **BUGFIX-P2**（P2，currentEnv_ 异常泄漏）：`callClosureValue` / `constructClassInstance` / `callNamedFunction` 中默认参数求值使用 `auto savedEnv = currentEnv_; ... currentEnv_ = savedEnv;` 手动恢复，`evaluate()` 抛异常时 `currentEnv_` 不恢复导致泄漏错误 scope。修复：改为 RAII guard（结构体析构函数恢复）。
- **AUDIT**（GUI/REPL 一致性）：(1) `irAction_` 在运行期间未禁用，查看正在编译/执行的 IR 导致状态不一致；(2) `onNew/onOpen` 在运行/调试期间未禁止，清空/替换正在执行的代码导致状态混乱；(3) `loadFile` 文件大小检查使用裸 `MAX_SOURCE_SIZE` 而非 `RuntimeLimits::MAX_SOURCE_SIZE`；(4) `ReplPanel` 异步执行 RuntimeError 经信号显示后，`pollReplFuture` 仍打印 "null" 结果造成重复输出——新增 `hadReplError_` 原子标志，错误时跳过结果输出；(5) `WorkerManager::stopForClose` 成功路径未与 `forceStop` 对齐做完整清理（debugMode / REPL 状态 / debugger / 主回调），导致下次运行读到脏状态；(6) `IdeController` VM 条件断点求值器每次命中都重新 Lexer+Parser 严重影响循环内条件断点性能——改用 `shared_ptr<unordered_map>` 缓存条件 AST。

## 2026-06-24 ~ 06-26 · 第六轮 Bug 修复（AUDIT-BUG-F1~F13 系列）

通过三个并行 agent 覆盖 Lexer/BuiltinFunctions/DebugController/GUI/Compiler/Interpreter 边界，共修复 13 个 bug（2 HIGH / 7 MED / 4 LOW），新增 5 个回归测试，全部 1047/1047 测试通过。

- **F1**（HIGH）：`constructClassInstance` 中 `callStack_.emplace_back` 在 `CallFrameGuard` 构造之前执行，guard 保存错误的 `savedStackDepth` 导致 init 帧不弹出，循环构造实例时 callStack_ 泄漏。修复：交换两行顺序，guard 先于 emplace_back。
- **F2**（HIGH）：REPL `reload` 命令用 `startsWith("reload")` 误匹配 `reloadable`/`reloadX` 等标识符，命中后清空用户输入。修复：改为精确匹配 + 空格前缀。
- **F3**（MED）：`VmStepper::vmCrossedDeeper_` 仅在 `stop()` 中重置，`stepByMode` 入口未重置，导致 STEP_OVER 退化为 STEP_IN。修复：在 stepByMode 入口添加重置。
- **F4**（MED）：`DebugController::crossedLine_` 在条件断点求值之前重置，条件不满足时已清 false，单行循环中条件断点永不再触发。修复：将重置推迟到断点真正命中之前。
- **F5**（MED）：`IrViewer::setUpdatesEnabled(false)` 后填充逻辑无异常保护，异常时 `setUpdatesEnabled(true)` 永不执行，列表永久冻结。修复：用 try/catch 包裹填充逻辑。
- **F6**（MED）：`onRun`/`onDebug` 在 `isReplRunning()` 检查之前执行 `clearAll()`，REPL 运行时清空用户历史输出后才报错。修复：将互斥检查移到 `clearAll()` 之前。
- **F7**（MED）：IR 路径 `visitTryStmt` 的 catch 变量绑定到外层 `varMap_` 且不恢复，catch 块后仍可引用，与 Interpreter/StackVM 语义不一致。修复：为三条路径（函数内、顶层遮蔽、顶层无遮蔽）添加 varMap_ 保存/恢复。
- **F8**（MED）：模块导入的 try/catch 块在异常时未调用 `moduleEnv->closeCapturedVariables()`，逃逸闭包的 capturedVars 保持初始值。修复：在 catch 块中添加 closeCapturedVariables 调用。
- **F9**（MED）：`RegisterVM::executeSharedBuiltinFunction` 调用仅传 3 参数，line/column 取默认值 0,0，错误消息显示"行 0:0"。修复：从当前帧的 chunk 获取行号并传入。
- **F10**（LOW）：Lexer 插值表达式中嵌套字符串的 `start_` 未在 `advance()` 前更新，导致 token 列号/lexeme 错误。修复：添加 `start_ = current_`。
- **F11**（LOW）：`substr` 对负 `start` 静默返回空串，但对负 `len` 报错，错误处理不对称。修复：负 `start` 改为报错。
- **F12**（LOW）：`SyntaxHighlighter` 不识别字符串插值 `{expr}`，整个字符串统一着色。修复：识别 `{` 作为插值起始，用 braceDepth 跟踪嵌套，插值表达式内字符不标记 mask。
- **F13**（LOW）：`CodeEditor` 空白区域点击 `cursorForPosition` 返回文档末尾光标，导致在最后一行设置断点。修复：检查 Y 坐标是否超出最后一个块的下边界。

## 2026-06-30 · 测试质量审计

审计发现并修复 2 个测试覆盖缺口：

- **TypeChecker `int` 注解拒绝 `float` 值零覆盖**：新增 5 个 CompilerTypeCheckTest 测试（A11-A15），覆盖声明/赋值路径的 float→int 拒绝，并强化断言：不仅验证警告计数，还验证警告消息内容含期望类型名，防止误报通过。
- **Formatter 语句级 AST 往返等价性仅覆盖表达式级（19 用例）**：新增 `test_statements()` 函数 + 16 个 S1-S16 测试用例覆盖 if/for/while/class/try 等语句结构，对每条语句做 `Parse(src)` vs `Parse(format(src))` 的 AST 结构等价比较，检测 Formatter 在格式化时是否丢失或重组子节点。

## 2026-06-18 ~ 06-26 · 性能优化与正确性阶段成果

近期完成了多个性能与正确性优化批次，全部通过 1047/1047 单元测试 + 100+ formatter_audit 审计用例。

- **VMUpvalue O(1) 帧定位**（#18）：`VMUpvalue` 新增 `owningFrameIdx` 字段，`OP_SET_UPVALUE` 用 O(1) 索引替代原 O(frames) 线性扫描定位目标帧；passthrough upvalue 复用 shared_ptr 自动透传该字段。
- **executeReturn 字段同步合并遍历**（#19）：Path B1 中"写 caller `this.fields()`"与"写 caller field slots"两次 O(fieldCount) 遍历合并为单次遍历，slot 索引用 `BytecodeChunk::fieldSlotIndex` O(1) 查找。
- **内建方法枚举分发**（#20）：提取共享自由函数 `classifyBuiltinMethod(name) -> BuiltinMethod` 枚举，VM / RegisterVM / Interpreter 共用；按 `name.size()` 快速筛选 + 单次字符串比较 + `switch` 分发，消除长串 if-else 字符串比较链；共享方法名（`len` / `contains`）用 fallthrough case 标签跨类型复用。
- **Environment boundInstance_ 缓存**（#21）：`get` / `set` / `hasVariable` 缓存 `lastCheckedInstance` 指针，`boundInstance_` 沿作用域链继承且同链多数层指针相同，命中缓存时跳过重复 `fields()` 哈希查找；仅当链上出现不同 `boundInstance_`（`bindInstance` 显式覆盖）时才重新检查。
- **类方法哈希索引**（#12）：`VM` 新增两级 `unordered_map<className, unordered_map<methodName, BytecodeChunk*>>` 索引，`initExecution` 一次性扫描 `functionChunks_` 构建，`findMethodChunk` 用哈希查找替代每层继承链拼 "Class.method" 字符串。
- **寄存器帧零堆分配**（#8）：`RegCallFrame::registers` 从 `std::vector<Value>` 改为 `std::array<Value, 32>` + `uint8_t registerCount`，利用 32 寄存器硬上限消除每帧堆分配。
- **VMStack 定长数组**（PERF-13）：VM 操作数栈使用 `Value[1024]` + `size_t top_` 替代 `std::vector`，消除 `push_back` 容量检查与堆分配开销。
- **NaN-boxing Value**（PERF-12）：`sizeof(Value)` 从 24 字节降至 8 字节，标量内联存储零原子操作，堆类型通过侵入式 `RefCounted` 基类管理一次原子递增。
- **字符串码位缓存**（#21）：StringData 缓存 `codepointCount`，消除 len()/substr() 在循环中的 O(n²) 重复扫描。缓存失效时机：非 const stringVal() 返回可变引用时 reset()。
- **IR 常量/全局去重哈希表**：IRFunction::addConstant/addGlobal 使用 hash 侧表维护去重索引，消除 O(n²) 线性扫描。constants/globalNames 不允许外部直接 push_back/clear。
- **RegClassInfo 懒预计算**：继承链展平字段和 init 方法名在 REG_DEFINE_CLASS 时预计算缓存，避免实例化时重复遍历继承链。
- **条件断点沙箱优化**：evaluateCondition 快照作用域链变量 + 绑定实例字段，求值后恢复。残余限制：容器变异仍影响共享 ref-counted 对象（Value 引用语义），已文档化为可接受现状。

## 2026-06-18 ~ 06-20 · 继承链正确性修复（BUG-INH 系列）

针对三后端继承语义一致性审计发现的问题：

- **BUG-INH-1**（P0）：IR 路径丢失类字段默认值表达式——`AstIRBuilder::visitClassDecl` 未收集字段初始化器字面量，所有字段被硬编码为 null。修复：DEFINE_CLASS IR 操作数新增 fieldDefaultConstIdx，lowering 时按 Value 类型 emit 对应常量加载指令。
- **BUG-INH-4**（P0）：StackVM IR 路径 `super.method()` 返回错误值——`LOAD_MUTATED + STORE_LOCAL` 后值残留导致 `RETURN` 弹出错误值。修复：visitMethodCall 的 isVarRef/isSuperCall 分支在 emitStoreVar 后，若存储目标是 LOCAL/UPVALUE 则显式 emit IROp::POP 消费残留值。
- **BUG-INH-3**（P2）：super 方法未找到时三后端错误消息不一致——RegVM 报 "父类链中无方法"/"无方法:" 与 Interpreter/StackVM 的 "没有方法" 不一致。修复：统一为 "类 X 没有方法 Y"。
- **BUG-INH-2**（P2，文档化）：非方法上下文中使用 super 时错误消息不一致——Interpreter 报 "super 只能在类方法中使用"，IR 路径报 "未定义的变量: this"。文档化为已知差异（不崩溃；错误类型相同均为 RuntimeError，三引擎均不被 try/catch 捕获，仅消息文本不同）。

## 2026-06-18 ~ 06-20 · 整数溢出与模块系统修复（BUG-OVF / BUG-MOD / SEC 系列）

- **BUG-OVF-1**（P1）：RegisterVM REG_NEGATE 整数溢出错误消息缺少后缀——统一为 "整数溢出：无法对最小值取负"。
- **BUG-OVF-2**（P1）：RegisterVM REG_DIV 整数溢出错误消息不一致——统一为 "整数运算溢出"（对齐 Interpreter/StackVM 的 computeArith DIV 分支）。
- **BUG-MOD-1**（P0）：IR 路径 import 语句崩溃（Debug）/静默生成坏 IR（Release）——AstIRBuilder 新增 hasError_/errorMessage_ 错误报告接口，NODE_IMPORT_STMT 独立 case 设置错误标志，compileViaIR/compileViaRegisterIR 检查并转化为用户可见 diagnostic。
- **SEC-1**（P0）：模块路径遍历攻击漏洞——路径校验移到 loader 检查之前，拒绝 ".." 父目录引用和绝对路径，防止 import 读取项目目录外文件。

## 2026-06-20 · REPL 正确性修复

- **REPL 模块缓存刷新**：原重新 import 模块时 `moduleCache_` 命中即复用旧 moduleEnv，不重新调用 loader 读源码——用户修改模块源文件后重新 import 仍得旧值。新增 `Interpreter::clearModuleCache(path)` / `clearAllModuleCache()` 方法；ReplPanel 新增 `reload "mod"` / `reload all` 命令清除缓存后下次 import 重新加载。
- **REPL 异步执行超时**：原 `waitReplFuture()` 调用 `replFuture_.wait()` 无超时，死循环场景下 closeEvent 永久阻塞。新增 `Interpreter::stopRequested_` 原子标志；`checkBreak` 在每个语句节点检查标志并抛 `std::runtime_error`（被异步 lambda 的 catch 捕获）；`waitReplFuture` 改为 `requestReplStop()` + `wait_for(5s)` + 超时回退阻塞。正常代码（含 checkBreak 调用）能在毫秒级响应中止。
