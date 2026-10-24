# MiniLang 开发指南

本文档是 MiniLang IDE 开发文档的导航入口。详细的架构设计、决策记录与测试方法请参见各子文档。

## 文档导航

| 文档 | 说明 |
|------|------|
| [快速开始](getting-started.md) | 构建、运行、开发环境配置（Windows/Linux/macOS/Docker） |
| [核心架构](architecture.md) | 编译管线、NaN-boxing、四后端（含 JIT）、内存模型、安全约束 |
| [测试指南](testing.md) | 多后端一致性验证方法论、测试分类与编写指南 |
| [设计决策 (ADR)](adr/) | 架构决策记录，详见下方索引 |
| [贡献指南](../CONTRIBUTING.md) | 环境搭建、编码规范、PR 流程 |
| [知识维护](knowledge/ROUND-END.md) | 轮末蒸馏仪式、文档预算与门禁；项目事实不进 auto-memory |
| [规格约定 (specs)](specs/) | 内存管理 / 多后端一致性 / 测试编写 / 缺陷分类 / 已否决方案 |

## 设计决策记录 (ADR)

| ADR | 主题 | 状态 |
|-----|------|------|
| [ADR-001](adr/ADR-001-nan-boxing.md) | NaN-boxing 值表示 | Accepted |
| [ADR-002](adr/ADR-002-triple-backend.md) | 三后端并存策略 | Accepted |
| [ADR-003](adr/ADR-003-ir-layer.md) | IR 中间层 | Accepted |
| [ADR-004](adr/ADR-004-cow-memory.md) | COW 内存模型 | Accepted |
| [ADR-005](adr/ADR-005-debug-consistency.md) | 调试一致性 | Accepted |
| [ADR-006](adr/ADR-006-jit-backend.md) | JIT 后端（第四执行引擎） | Accepted |
| [ADR-007](adr/ADR-007-coroutine-suspension.md) | 协程真挂起架构 | Accepted |
| [ADR-008](adr/ADR-008-jit-per-chunk.md) | JIT per-chunk 编译与块复用 | Accepted |

## 核心设计原则

- **PERF-12 NaN-boxing**：8 字节值编码，标量零分配，堆类型侵入式引用计数
- **PERF-13 VMStack**：定长数组 1024×8B + 栈顶指针，消除 push_back 开销
- **PERF-14 寄存器式 VM**：32 虚拟寄存器 R0-R31，68 条 RegOp，IR 直接 lowering
- **PERF-15 IR 复制传播**：vreg→常量等价替换，仅寄存器后端启用
- **COW 优化**：修改前检查独占所有权，避免不必要的深拷贝
- **OPT 性能快路径（2026-10-23）**：解释器单节点 VarRef 原位变异、StackVM/JIT 接收者整体写回、RegisterVM 名称版全局缓存、JIT init 缓存均有明确的失效/回退条件——修改前必读 `docs/specs/memory-model.md` §4 与 `docs/specs/backend-consistency.md` §10，并保留对应回归锁。
- **GUI 颜色**：面板内所有六位十六进制颜色必须经 `gui/TeachingTheme.h` 的语义角色（如 `TeachingTheme::error()`），禁止新增裸 hex；core_headless 编译的 `gui/ErrorHintEngine.cpp`（无 Qt 依赖）与暗色回退分支是仅有的例外，3 位 CSS 简写暂保留。

## 安全约束摘要

| 类别 | 措施 |
|------|------|
| DoS 防护 | 源码 ≤10MB、Token ≤100 万、循环 ≤1000 万次 |
| 递归保护 | Parser/Compiler/Formatter 深度限制 512/256 |
| 线程安全 | mutex + atomic、localtime_s/localtime_r |
| 异常安全 | UI 槽 try/catch、unique_ptr 防 bad_alloc 泄漏 |
| 内存安全 | 运行时越界检查、NaN-box 类型前置校验、value-initialize |
| 生命周期 | 无裸指针、shared_ptr 共享所有权、QThread unique_ptr 管理 |

## 质量门禁工具

性能、覆盖率与 VM profiling 工具链的用法约定（工具脚本位于 `scripts/`）。

### 性能基准回归（minilang_perf_test + perf_baseline.py）

- 构建：`MINILANG_BUILD_PERF_TESTS=ON`（默认）构建 `minilang_perf_test` 目标（5 category 9 项基准，`tests/TestPerformanceRegression.cpp`）
- 基线文件：仓库根 `perf-baseline.json`（v3 schema，含 `tolerance` / `category_tolerance` 差异化容忍度）
- 对比：`python scripts/perf_baseline.py check --baseline perf-baseline.json --binary out/build/debug/tests/minilang_perf_test`（默认容忍倍数 2.0x）
- 刷新基线：`python scripts/perf_baseline.py update --baseline perf-baseline.json --binary <perf_test>`（保留旧基线 tolerance 字段，禁止用旧 schema 覆盖）
- CI 联动：`scripts/perf_to_benchmark_action.py` 将基线转化为 benchmark action

### 覆盖率阈值（coverage_threshold.py）

- 输入：Cobertura XML 报告（`linux-gcc-coverage` preset 产出）
- 检查：`python scripts/coverage_threshold.py --report <cobertura.xml> --line-threshold <0-100> --branch-threshold <0-100>`

### VM opcode profiling（vm_profile_report.py）

- 前置：`MINILANG_VM_PROFILING=ON` 构建（独立 build 目录 `out/build/vm-profile`）
- 报告：`python scripts/vm_profile_report.py --top 20`（默认样本 `samples/mini`；`--skip-build` 复用已有产物）

### 全量验证入口（verify_all.ps1）

- AGENTS.md §6 强制验证规则的脚本化执行：构建全部目标 + ctest 全量（`-j 8`）
- 用法：`powershell -ExecutionPolicy Bypass -File scripts/verify_all.ps1`（可选 `-Filter <ctest 正则>`）
- 依赖：本机 VS 2022 DevShell（`D:\vs`），详见脚本头注释



## 新增教学面板指南

MiniLang IDE 的教学面板采用**懒加载工厂 + 元数据目录**双注册模式，新增一个面板需完成以下 4 步：

**颜色约定（OPT-11）**：面板样式与 HTML 中的六位颜色值必须使用 `gui/TeachingTheme.h` 的语义角色（新增角色需保持与被替换字面量逐位同值）；禁止直接写 `#RRGGBB`。`gui/ErrorHintEngine.cpp` 属于 core_base 无 Qt 编译单元，不得 include TeachingTheme。

### 1. 创建面板源文件

在 `gui/` 目录下创建 `XxxPanel.cpp` 与 `XxxPanel.h`。面板类继承 `TeachingPanelBase`（2026-10-03 起推荐，构造函数接收 `QWidget* parent`，主题切换重建视图只需 override `applyTheme()`，基类自动接线 `Theme::onThemeModeChanged`；既有 `QWidget` 直继承面板按家族分批迁移中。注意：QWidget 只允许一个顶层布局，面板自建的 `new QVBoxLayout(this)` 会被基类 `rootLayout()` 惰性采纳（勿重复创建，详见基类头注释）），构造函数接收 `QWidget* parent`。若面板包含大量静态教学数据（如场景库、配置表），建议参照 `BugHuntLibrary.cpp` / `MemoryModelLibrary.cpp` / `IRTransformLibrary.cpp` 的拆分模式，将静态数据独立成 `XxxLibrary.cpp` 编译单元，让面板文件聚焦 UI 逻辑。

### 2. 注册到 CMake 源列表

编辑 `cmake/minilang_core.cmake` 的 `MINILANG_GUI_SOURCES` 列表，添加新 `.cpp` 文件。CMake 不使用 `file(GLOB)` 自动发现源文件（遵循 CMake 最佳实践，避免新增文件未被发现导致链接错误）。

### 3. 注册面板元数据到 PanelCatalog

编辑 `gui/PanelCatalog.cpp`，在对应分类（入门导览 / 编译前端 / 执行引擎 / 内存与优化 / 调试与观测 / 深入实战）下添加 `PanelEntry`，末位参数为难度分级（1=入门 / 2=进阶 / 3=高级，缺省 1），需按分类内难度非递减顺序插入（有 `PanelLevelsSortedWithinCategory` 测试断言）：

```cpp
PanelEntry{"xxx-panel", "我的面板", "🔧", 2},
```

`PanelCatalog` 是面板导航的**单一数据源**——`TeachingTreePanel` 构建树形导航（含难度徽标与分类 tooltip）、`Ide::onActivityRequested` 路由活动 id、帮助弹窗的「难度/分类」提示行均从此查询。无需在 `Ide` 类中维护并行的 id→显示名映射。

### 4. 注册懒加载工厂到 Ide

编辑 `app/ide.cpp` 的 `Ide::registerLazyTeachingPanels()`，在对应的 `register*Panels` helper（按波次分组：`registerCompilationPipelinePanels` / `registerMemoryIrProfilePanels` / `registerDebugInspectorPanels` / `registerLearningPathPanels`）中调用 `registrar`：

```cpp
registrar(QStringLiteral("xxx-panel"), mlTr("我的面板"), [this]() {
    xxxPanel_ = new XxxPanel(this);
    xxxPanel_->setController(controller_);
    // connect(...) 信号槽连接
    return xxxPanel_;
});
```

`registrar` 是 `registerLazyTeachingPanels` 内部的 lambda，负责将工厂函数注册到 `teachingPanelFactories_` 成员 map，并在首次访问时调用 `wrapTeachingPanel` 包装面板（添加 `TeachingPanelHeader` 标题栏 + Fluent 滚动条）后加入 `centerStack_`。

> ⚠️ **样例代码必须可运行**：面板内置的教学样例（`XxxLibrary` 中的 `sampleCode` / `exampleCode`，以及引导中提示用户粘贴运行的代码）会被用户「加载样例 → 运行」，必须是合法可解析的 MiniLang。注意 `print` 是语句关键字且强制要求括号（见 `parser/Parser.cpp::printStmt`）——必须写 `print(x);`，无括号的 `print x;` 会 parse 失败。新增样例后建议用 CLI `minilang coverage xxx.mini` 实测解析+执行无错。

### 5. （推荐）注册「这是什么？」帮助文案

编辑 `gui/TeachingPanelHeader.cpp` 的 `helpDocs()` 静态 `QHash`，为新面板添加 `panelId → {purpose, recommendedOrder, relatedConcepts}` 条目：

```cpp
{QStringLiteral("xxx-panel"),
 {mlTr("面板用途（1-2 句）"), mlTr("1. 推荐使用顺序"), mlTr("关联概念 / 术语")}},
```

未登记时，`TeachingPanelHeader` 的「这是什么？」按钮会弹出「暂无此面板的帮助文档」兜底提示。应保证 `helpDocs()` 对 `PanelCatalog` 全部面板 100% 覆盖。

### 6. （可选）提供新手引导 GuidedTour

若面板需要逐控件高亮引导，在面板类实现 `GuidedTour* createGuidedTour(QWidget* host)`，并在 `app/ide.cpp::onPanelGuidedTourRequested` 的 if-else 路由中补对应分支。**两处必须同步**：`createGuidedTour` 实现 / `onPanelGuidedTourRequested` 路由——否则专属引导不会生效（会退化为通用兜底引导）。首访自动引导默认对全部面板生效（`guidedTour/shown_<panelId>` QSettings 键去重），纯浏览型面板可加入同文件 `kNoAutoTourPanels` 排除集合。

> 💡 新面板未提供专属 GuidedTour 属正常情况：`onPanelGuidedTourRequested` 会自动走 `createGenericPanelTour` 通用兜底引导——从 `TeachingPanelHeader::helpDocFor()` 的帮助文案派生 3 步气泡（面板用途/推荐顺序/关联概念），因此**步骤 5 的帮助文案是引导兜底的前提**，务必登记。

### 面板注册机制架构

| 组件 | 文件 | 职责 |
|------|------|------|
| `PanelCatalog` | `gui/PanelCatalog.cpp/.h` | 面板元数据单一数据源（id / label / emoji / level 难度分级 / category description），供导航树与路由查询；6 大分类按学习曲线递进排序 |
| `teachingPanelFactories_` | `app/ide.h` | `QMap<QString, std::function<void()>>` 懒加载工厂注册表，首次访问时构造面板 |
| `registerLazyTeachingPanels` | `app/ide.cpp` | 工厂注册入口，按波次拆分到 4 个 `register*Panels` helper |
| `wrapTeachingPanel` | `app/ide.cpp` | 面板包装器：添加 `TeachingPanelHeader` 标题栏 + Fluent 滚动条 + 卡片样式 |
| `helpDocs()` | `gui/TeachingPanelHeader.cpp` | 「这是什么？」帮助文案表（panelId → 用途/顺序/关联概念），应对全部面板覆盖 |
| `onPanelGuidedTourRequested` | `app/ide.cpp` | 新手引导路由：panelId → 对应面板 `createGuidedTour`；含 `closing_` 防护与 `tour==nullptr` 兜底 |
| `kAutoTourPanels` | `app/ide.cpp` | 首次访问自动弹出引导的面板集合（`guidedTour/shown_<id>` QSettings 去重） |
| `onActivityRequested` | `app/ide.cpp` | 活动 id 路由：`PanelCatalog::canonicalPanelId` 解析别名 → `showTeachingPanel` |

### 已弃用的 PanelRegistry

历史上曾设计 `gui/PanelRegistry.h/.cpp`（单例工厂模式）作为面板注册的通用基础设施，但因与 `Ide` 的 `centerStack_` / dock 管理紧密耦合的模型不兼容，从未被实际采用，已于 2026-07-25 作为死代码移除。当前 `teachingPanelFactories_` 成员 map 即为实际的面板注册表。


## 变更历史

本文件不再维护变更摘要。用户可见变更见 [CHANGELOG](../CHANGELOG.md)；审计轮次与决策复盘见 [audit/](audit/README.md)。开发约定只保留稳定规则，历史叙事不进入本文件。

## 已知限制与后续决策

- 开放缺陷、阻塞与优化候选：见 [BACKLOG](BACKLOG.md)（唯一活跃清单）。
- 评估后不实施的方案与重启条件：见 [rejected-decisions](specs/rejected-decisions.md)。
- 已修复限制、历史勘误与事故复盘：见 [CHANGELOG](../CHANGELOG.md)，本文件不再重复。
- 当前仍需注意的测试限制见 [testing.md](testing.md)「已知限制」。

## 核心公开 API 文档

本章节索引 MiniLang 核心公开 API 的 Doxygen 注释位置，便于开发者快速定位。详细参数说明见各头文件中的 `@brief` / `@param` / `@return` / `@note` / `@warning` 标注。

### 错误处理层（common/）

| 头文件 | 核心 API | 说明 |
|--------|----------|------|
| `common/ErrorFormat.h` | `ErrorFormat::format(fmt, args...)` | printf 风格错误消息格式化，栈缓冲零堆分配 |
| `common/ErrorFormat.h` | `ErrorFormat::intToString(value)` | 整数转字符串，`std::to_chars` 实现 |
| `common/ErrorFormat.h` | `ErrorFormat::formatWithLocation(msg, line, col)` | 附加 `(行 N, 列 N)` 定位信息 |
| `common/ErrorFormat.h` | `ErrorFormat::formatWithLine(msg, line)` | 附加 `(行 N)` 行号信息 |
| `common/ErrorMessages.h` | `ErrorMessages::kArrayIndexMustBeInt` 等 | 三后端共享错误消息常量（保证文本一致性） |
| `common/ErrorMessages.h` | `ErrorMessages::kRecursionDepthExceededFmt` | 递归深度超限消息格式串（R97 #11 fix） |
| `common/ErrorMessages.h` | `ErrorMessages::kSuperOutsideMethod` | super 在非方法上下文使用时的错误消息（R97 #11 fix） |

### 运行时配置层（common/）

| 头文件 | 核心 API | 说明 |
|--------|----------|------|
| `common/RuntimeLimits.h` | `RuntimeLimits::MAX_RECURSION_DEPTH` 等 | 编译期 constexpr 限制常量（17 项） |
| `common/RuntimeLimits.h` | `RuntimeLimits::RuntimeConfig::instance()` | 运行时可配置 DoS 防护单例（atomic 存储，L7 fix） |
| `common/RuntimeLimits.h` | `RuntimeConfig::maxInstructions() / setMaxInstructions(v)` | VM 指令预算读写 |
| `common/RuntimeLimits.h` | `RuntimeConfig::maxLoopIterations() / setMaxLoopIterations(v)` | 循环迭代上限读写 |
| `common/RuntimeLimits.h` | `RuntimeConfig::maxParseErrors() / setMaxParseErrors(v)` | 解析错误数量上限读写 |
| `common/RuntimeLimits.h` | `RuntimeConfig::resetToDefaults()` | 重置为编译期默认值（测试 tearDown 调用） |

### 解释器层（interpreter/）

| 头文件 | 核心 API | 说明 |
|--------|----------|------|
| `interpreter/Interpreter.h` | `Interpreter::execute(Block&)` | 执行 AST（树遍历解释器） |
| `interpreter/Interpreter.h` | `Interpreter::executeRepl(Block&)` | REPL 模式执行（保留环境） |
| `interpreter/Interpreter.h` | `Interpreter::evaluateCondition(ASTNode*)` | 安全求值条件断点表达式（沙箱） |
| `interpreter/Interpreter.h` | `Interpreter::lookupClassSafely(name, msg, line, col)` | 安全查找类，找不到时 runtimeError（R97 #10 fix） |
| `interpreter/Interpreter.h` | `Interpreter::currentEnvironmentShared()` | 获取当前环境 shared_ptr（跨线程安全） |
| `interpreter/Value.h` | `Value::isAsciiString()` | O(1) 读取 StringData 持久缓存的 isAscii 标志（R97 #3 fix） |
| `interpreter/Value.h` | `Value::DictKey` / `DictMap` | 字典键 variant 类型与容器 typedef（R97 #2 / L4 fix） |
| `interpreter/Value.h` | `Value::sharesStorageWith(other)` | 判断两个 Value 是否指向同一堆对象（OPT-01 原位变异身份校验） |
| `interpreter/Environment.h` | `Environment::getMutableVariableSlot(name)` | 沿作用域链取变量可写槽位（OPT-01：RHS 求值后重取，避免悬垂/错对象） |

### 编译器层（compiler/）

| 头文件 | 核心 API | 说明 |
|--------|----------|------|
| `compiler/core/Compiler.h` | `Compiler::compile(Block&)` | 编译 AST 到 BytecodeChunk |
| `compiler/core/Compiler.h` | `Compiler::CompileContextGuard` | RAII 守卫，自动保存/恢复编译上下文（R97 #9 fix） |
| `compiler/backend-stack/VM.h` | `VM::execute(BytecodeChunk&)` | 执行栈式字节码 |
| `compiler/backend-reg/RegisterVM.h` | `RegisterVM::execute(RegBytecodeChunk&)` | 执行寄存器式字节码 |
| `compiler/core/Bytecode.h` | `OpCode` 枚举 | 栈式 VM 操作码（93 项，含 int 比较特化） |
| `compiler/backend-reg/RegisterBytecode.h` | `RegOp` 枚举 | 寄存器式 VM 操作码（69 项） |
| `compiler/core/Bytecode.h` | `BytecodeLineColumnMap` / `getLine` / `getColumn` | 行列元数据 4B/字节打包（OPT-10） |

### IR 中间层（compiler/）

| 头文件 | 核心 API | 说明 |
|--------|----------|------|
| `compiler/ir/IR.h` | `IROp` 枚举 | IR 指令枚举（三地址码） |
| `compiler/ir/IR.h` | `AstIRBuilder::build(Block&)` | AST → IR 构建 |
| `compiler/ir/IR.h` | `BytecodeIRBackend::lower(IRModule&)` | IR → 栈式字节码 lowering |
| `compiler/backend-reg/RegisterBytecodeBackend.h` | `RegisterBytecodeBackend::lower(IRModule&)` | IR → 寄存器式字节码 lowering |

### 后端抽象层（common/）

| 头文件 | 核心 API | 说明 |
|--------|----------|------|
| `common/IBackend.h` | `IBackend` 接口 | 后端抽象接口（ARCH-09 fix） |
| `common/IBackend.h` | `IBackend::backendName()` | 后端名称 |
| `common/IBackend.h` | `IBackend::setOutputCallback(cb)` | 设置输出回调 |
| `common/IBackend.h` | `IBackend::getDiagnostics()` | 获取诊断信息 |

## 未来拓展方向

本章节记录 MiniLang IDE 后续可继续推进的拓展方向，按"语言能力 / 执行引擎 / 调试器 / 工具链 / 教学能力 / 平台与生态 / 基础设施"七维度分类。每项标注工程量评估（S/M/L）与依赖关系，便于后续按优先级与资源安排立项。

### 一、语言能力拓展

| 拓展项 | 说明 | 工程量 | 依赖 |
|--------|------|--------|------|
| **泛型 / 模板** ✅ 已完成（R163，2026-07-23） | 支持 `fun identity<T>(x: T): T` 风格泛型函数与 `class Box<T>` 泛型类。类型参数运行时擦除（四后端统一跳过类型校验）。**实现摘要**：AST FunDecl/ClassDecl/EnumDecl 新增 `typeParams` 字段；Parser 解析 `<T, E, ...>` 类型参数列表；Interpreter CallFrameGuard 扩展管理 `currentTypeParams_`（方法调用合并类泛型+方法泛型）；Compiler/IR emitTypeCheck/emitTypeCheckIR 编译期擦除；Formatter/DocGenerator 输出类型参数；common/TypeChecker.h 共享 `isTypeParameter()` 工具；26 个测试覆盖泛型函数/类/继承/兼容性/Formatter/DocGenerator；2844/2844 全量回归通过。 | L | 类型系统重构 |
| **模式匹配** ✅ 已完成（R99/R102/R103，2026-07-19；R134 扩展 6 种 pattern + guard + 嵌套） | 支持 `match expr { case Pattern => body }` 语法，Pattern 包含字面量、变量绑定、构造器嵌套。教学价值：展示代数数据类型（ADT）与编译期穷尽性检查。**实现摘要**：R134 扩展 WILDCARD/LITERAL/VARIABLE/VARIANT/TUPLE/OR 六种 pattern + guard 表达式 + 嵌套 pattern，新增 OP_LEN/OP_TYPE_TEST 三后端实现，23 个 EXPECT_ALL_BACKENDS 测试。 | M | AST 新增 MatchExpr / Pattern 节点 |
| **协程 / async-await** ✅ 已完成（生成器/协程 R164，2026-07-23；async/await 七特性 MVP） | 引入 `async fun`/`await` 关键字，VM 新增 `OP_YIELD`/`OP_AWAIT` 指令，调度器协作用户级协程。教学价值：展示状态机变换与无栈协程实现。**已完成**：生成器/协程（`yield`/`.next()`/`.done()`）四后端一致（R164）；async/await MVP 复用协程重放模式，`async fun` 调用返回 Task/协程，`await` 同步 drain 至完成取最终值，新增 OP_AWAIT/REG_AWAIT/IROp::AWAIT 四后端一致，`std/async` 协作式调度器（runAll/runTask），11 个四后端测试。 | L | VM 新增指令 + 调度器 |
| **宏系统** ✅ 已完成（七特性 MVP + 语句宏升级） | 表达式模板宏 `macro name(p1,p2) { <expr> }` 与语句/块体宏 `macro name(p) { stmt; stmt; }` + `name!(args)` 调用，parse 期展开（AST 克隆 + 参数替换）。教学价值：展示宏展开 vs 函数调用的求值策略差异（实参多次代入）。**实现摘要**：新增 TK_MACRO/NODE_MACRO_DECL/NODE_MACRO_CALL + ast/MacroExpander 展开器（支持表达式与基础语句节点克隆 + 左值参数代入）；parse 期展开为普通 AST，四后端直接求值 expanded 子树（零指令集改动，语句宏补 null 占位保栈平衡）；递归宏/arity/未定义宏报 ParseError；Formatter 往返等价；23 个四后端测试（含 swap/if/while/return 语句宏）。 | L | Lexer/Parser 前置宏展开阶段 |
| **Trait / Mixin** ✅ 已完成（七特性 MVP） | `trait T { fun m() {...} }` + `class C with T1, T2`（可与 extends 并存），方法组合而非单继承。教学价值：展示组合优于继承与 diamond 问题解决。**实现摘要**：新增 TK_TRAIT/TK_WITH/NODE_TRAIT_DECL + ClassDecl.traits/ownMemberCount；parse 期将 trait 方法（shared_ptr 共享 FunDecl）合入混入类 members，三后端按普通类方法编译（零指令集改动）；类自身方法优先、diamond 冲突报 ParseError；14 个四后端测试。 | M | 类系统重构 |
| **枚举与 ADT** ✅ 已完成（R99/R102/R103，2026-07-19） | `enum Color { Red, Green, Blue }` 简单枚举 + `enum Option<T> { Some(T), None }` 带参枚举。教学价值：展示代数数据类型与 switch 穷尽性检查。**实现摘要**：AST 新增 EnumDecl 节点 + variant 名与 arity 三后端校验（`enumRegistry_`），配合 R134 match 表达式实现 ADT 穷尽性；泛型 enum 语法 `enum Shape<T> { Circle(T) }` 支持 payload 类型擦除。 | M | AST 新增 EnumDecl 节点 |
| **元组与解构** ✅ 已完成（R100，2026-07-19） | `var (a, b) = (1, "x")` 元组字面量与解构绑定。教学价值：展示多返回值与模式匹配的语法基础。**实现摘要**：28 个文件覆盖 AST/Visitor/ValueTypes/Value/Parser/Interpreter/栈式 VM/寄存器式 VM/IR 层/TypeChecker/Formatter/测试；新增 `OP_BUILD_TUPLE` / `REG_BUILD_TUPLE` / `IROp::BUILD_TUPLE`；元组 immutable + Python repr 风格字符串化；解构通过 DUP + OP_INT i + OP_INDEX_GET 实现；21 个测试覆盖字面量/解构/嵌套/函数返回值；1809/1809 测试通过。详见 CHANGELOG.md 第一百轮条目。 | S | AST 新增 TupleLiteral / DestructurePattern |
| **错误处理增强** ✅ 已完成（七特性 MVP） | `Result<T, E>` 类型（std/result 模块 + 泛型 enum）与 `?` 错误传播操作符，与现有 try/catch 互补。教学价值：展示异常与错误值两种范式对比。**实现摘要**：`?` 脱糖为共享内建 __qmark_unwrap（三后端自动一致），同时支持 std/result dict 协议与泛型 enum variant（Ok/Err/Some/None 按 variantName 分派）；Err/None 抑制为可被 try/catch 捕获的异常；9 个四后端测试。 | M | 类型系统 + Result 标准库类型 |

### 二、执行引擎拓展

| 拓展项 | 说明 | 工程量 | 依赖 |
|--------|------|--------|------|
| **JIT 编译** ✅ 已完成并持续演进 | 第四套执行引擎：asmjit 动态编译字节码到本机码，教学价值为 JIT/AOT/解释执行三角对比。实现阶段、性能数据、分层编译、OSR/反优化与限制见 [ADR-006](adr/ADR-006-jit-backend.md)、[ADR-008](adr/ADR-008-jit-per-chunk.md) 与 [CHANGELOG](../CHANGELOG.md)。 | L | 三后端抽象层稳定 |
| **GC 替代引用计数** | 现状：`GcManager` 已提供 mark-sweep 循环引用检测（与 RefCounted + COW 并存，GcVisualizerPanel 可视化）。本项指“完整替代引用计数的分代 GC 作为可选内存模型”，仍未实施。教学价值：展示不同内存回收策略的延迟分布与吞吐量权衡。 | L | Value 类型扩展 |
| ~~**线程与并发原语**~~ ✅ 已完成（spawn/channel/Mutex/RwLock 四后端一致；含 channel.recvTimeout（2026-07-28 E3）与 mutex 自死锁检测，由 ConcurrencyGaps2/RwLockGaps3/MutexDeadlockGaps3 等套件覆盖） | ~~支持 `spawn` 创建线程 + `channel` 通信 + `Mutex`/`RwLock` 同步原语。~~教学价值：展示 CSP（Communicating Sequential Processes）vs 共享内存两种并发模型。 | L | ✅ 已落地 |
| ~~**Inline Cache 优化**~~ ✅ 已完成（R160，2026-07-22） | ~~为 method dispatch / global lookup 添加 inline cache（IC），热路径命中缓存避免 hash 查找。教学价值：展示 V8/PyPy 优化技术基础。~~ **实现摘要**：为 `OP_MEMBER_GET` 属性访问添加单态 inline cache——编译期统计所有 chunk 的 `OP_MEMBER_GET` 总数预分配 `memberGetIC_` 数组 + 按顺序分配全局唯一 `callSiteId`；运行时 `jitMemberGetWithIC` 辅助函数对 Instance 类型走 IC 路径（命中直接读字段 Value 指针跳过 `unordered_map::find`，未命中走慢速路径并更新 cache）；`MemberGetInlineCacheEntry` 缓存 `(InstanceData 原始地址, 字段 Value 指针)`；关键 COW 陷阱规避——必须用 `std::as_const(obj).fields()` 调用 const 重载避免 `ensureUnique` 深拷贝导致 cache 指针悬垂；`getInlineCacheStats()`/`resetInlineCacheStats()` 公有 API 供教学统计；3 个 R160 专项测试（语义正确性 + hit/miss 统计 + 多态退化）。 | M | 现有 dispatch 路径 |
| ~~**Tail Call Optimization**~~ ✅ 已完成（R112 + R113） | 编译期识别尾递归并转换为循环，避免栈溢出。教学价值：展示 TCO 的实现条件与 Scheme/Rust 不同的 TCO 策略。 | S | Compiler 跳转指令改造 |
| **SIMD 向量化** | 数组运算自动向量化（如 `arr1 + arr2` 元素级加法用 AVX2）。教学价值：展示数据并行与自动向量化的边界。 | M | 数组内存布局连续化 |
| **第四后端：WebAssembly** | 将 BytecodeChunk 编译为 WASM 字节码，在浏览器中运行。教学价值：展示跨平台字节码设计与 WASM 二进制格式。 | L | WASM 二进制编码层 |

### 三、调试器拓展

| 拓展项 | 说明 | 工程量 | 依赖 |
|--------|------|--------|------|
| ~~**数据断点 / Watchpoint**~~ ✅ R161（2026-07-22） | ~~监视变量/字段被修改时暂停（类似 GDB `watch` 命令）。VM 在 OP_SET_VAR/OP_SET_FIELD 检查地址。~~ **实现摘要**：新增 `WatchpointInfo`/`WriteTarget` 共享类型（`debug/DebugTypes.h`）+ `VM::peekWriteTarget`/`RegisterVM::peekWriteTarget` pre-execution peek 解码 18 种 SET 类 OpCode（OP_SET_VAR/OP_DEFINE_VAR/OP_SET_LOCAL/OP_SET_GLOBAL/OP_SET_UPVALUE/OP_MEMBER_SET/OP_INIT_FIELD/OP_INDEX_SET 等 + RegOp 对应指令）+ `VmStepper::checkWatchpointHit` 命中检测（Variable/Field 两种 kind + hitCount + vmLastPausedLine_ 设置）+ 5 个调用点条件扩展 `\|\| hasWatchpoints()` + `IdeController` watchpoint facade API + `WatchpointPanel` 双子页（实时列表 + 6 教学场景库）+ `evaluateDebuggerRepl` 调试器 REPL 阶段 1（只读，复用 evaluateWatchExpression 沙箱模式但用 executeRepl 不做沙箱恢复）+ ReplPanel 暂停态分支 + 22 个测试用例（6 结构体 + 8 VM peekWriteTarget + 6 RegisterVM peekWriteTarget + 2 跨后端一致性）。**仅 VM 路径支持**，Interpreter 路径留待后续。详见 CHANGELOG.md R161 条目。 | M | 变量地址追踪 |
| ~~**回溯调试（Reverse Debugging）**~~ ✅ 已完成（2026-07 反向调试时间轴） | ~~记录每步执行状态快照，支持 `reverse-step`/`reverse-continue`。教学价值：展示 record/replay 调试技术（如 rr、UndoDB）。~~ **实现摘要**：基础设施 L18（`Interpreter::StateSnapshot` + `captureStateSnapshot`/`restoreFromSnapshot` + `ExecutionTraceRecorder` FullState 模式）+ `gui/ReverseDebugTimelinePanel`（id=`reverse-timeline`，已注册 PanelCatalog「执行引擎」分类），消费 TraceSnapshot 列表渲染水平时间轴，点击任意历史步调用 `Interpreter::restoreFromSnapshot` 回滚状态。 | L | 状态快照序列化 |
| ~~**调用栈任意帧切换**~~ ✅ R121（2026-07-19） | ~~暂停后可"上移"到调用栈任意帧查看局部变量（类似 GDB `frame N`）。需要保留每帧环境快照。~~ **实现摘要**：新增 `IdeController::setSelectedFrame/getSelectedFrame/getSelectedFrameLocals/clearSelectedFrame` API（`selectedFrame_` = -1=未选/默认栈顶；>= 0=帧索引）+ CallStackPanel 帧选择 UI（`inRefresh_` 守卫避免循环刷新 + 重建后恢复选中）+ VariableInspectorPanel/WatchPanel 联动消费所选帧 locals + `evaluateWatchExpression` 注入所选帧上下文 + 三后端统一 dispatch（VM 模式 `vmStepper_.getFrameLocalsAt` / Interpreter 调试暂停 `debugCoord_.getFrameLocalsAt`）+ 8 个测试用例。详见 CHANGELOG.md 第一百二十四轮条目。 | M | Environment 快照机制 |
| **多线程调试视图** | 同时显示多个协程/线程的栈帧与暂停状态。依赖项（协程 R164 + 并发原语）均已落地，可立项。 | L | ✅ 依赖已满足（协程/线程支持已完成） |
| ~~**断点日志（Logpoint）**~~ ✅ R104 | 断点不暂停，仅打印表达式值到输出面板。教学价值：展示非侵入式日志调试。 | S | 现有条件断点扩展 |
| ~~**异常断点**~~ ✅ R104 | 在 try/catch 抛出异常时自动暂停（类似 GDB `catch throw`）。 | S | 异常抛出钩子 |
| ~~**函数断点**~~ ✅ R104 | 按函数名设置断点（不依赖行号），适合无法看到源码的标准库函数。 | S | 函数符号表 |
| ~~**观察表达式面板**~~ ✅ R117（2026-07-19） | ~~持久化的表达式监视面板，每次暂停自动求值并显示变化。~~ **实现摘要**：新增 `IdeController::evaluateWatchExpression()` API（复用条件断点沙箱机制但返回完整 `Value`）+ `WatchPanel` 双子页（实时观察 + 教学场景库 6 个场景）+ PanelCatalog 注册 + VmStateChanged 监听器自动刷新 + Library 抽取模式（`WatchExpressionLibrary.cpp` 独立编译单元）+ GuidedTour 5 步引导 + QSettings 持久化 + 13 个测试用例。详见 CHANGELOG.md 第一百二十轮条目。 | S | 现有变量监视扩展 |
| ~~**调试器 REPL**~~ ✅ R161 阶段 1 + 阶段 2（2026-07-22） | ~~暂停时在 REPL 中执行任意表达式（修改变量、调用函数），与现有教学 REPL 共享运行时。~~ **实现摘要（阶段 1，只读）**：新增 `IdeController::evaluateDebuggerRepl(source, output)` API（复用 `evaluateWatchExpression` 的临时 Interpreter 沙箱模式但用 `executeRepl` 替代 `evaluateCondition` 不做沙箱恢复，允许 var/print/赋值语句在沙箱内执行）+ ReplPanel 暂停态分支（`isDebugPaused() \|\| (isVmInitialized() && !isVmRunning())` 时调用 evaluateDebuggerRepl）+ 捕获 print 输出到 output 参数显示。**实现摘要（阶段 2，函数调用）**：`Interpreter::injectRegistriesFrom(src)` 复制主 Interpreter 的 `funRegistry_/classRegistry_/enumRegistry_`（shared_ptr 共享所有权）+ `hasRegistries()` 内联查询避免空注入开销 + `evaluateDebuggerRepl` 在 `executeRepl` 前判定并注入注册表，使调试器 REPL 中可调用用户定义函数（通过 env 闭包值）、实例化类（`classRegistry_` 直接支撑 `constructClassInstance`）、访问枚举 variant（`enumRegistry_` 支撑 `visitEnumVariantExpr`）+ 主 Interpreter 通过 `retainReplAst` 保留 AST 所有权使 shared_ptr 持续有效。**关键限制**：(a) `executeRepl` 清除 `funRegistry_` 但保留 `classRegistry_`/`enumRegistry_`（类/枚举跨行保留），故函数调用依赖 env 闭包值（携带 `shared_ptr<FunDecl> body`）；(b) Parser `knownEnums_` 跨实例不共享，`Color.RED` 消歧依赖 Parser 实例私有集合，调试器 REPL 源码需自带 enum 定义才能生成 `EnumVariantExpr`；(c) 非泛型 enum payload 做类型校验，传 int payload 需用泛型 enum 语法 `enum Shape<T> { Circle(T) }` 规避。**27 个回归测试**：8 个场景分组（HasRegistries/Class/Inheritance/Enum/Function/Closure/Boundary/Combined）覆盖注册表注入语义、类实例化、继承链 super 查找、枚举 variant 访问、函数递归/默认参数/互递归、闭包捕获变量、多次注入/输出捕获边界、类方法调用注入函数 + REPL 中 match 枚举 variant。详见 CHANGELOG.md R161 条目。 | M | 沙箱求值器扩展 |

### 四、工具链拓展

| 拓展项 | 说明 | 工程量 | 依赖 |
|--------|------|--------|------|
| ~~**LSP 服务器**~~ ✅ R164（2026-07-23） | ~~将 Lexer/Parser/语义分析封装为 Language Server Protocol 实现，支持任意编辑器（VS Code/Vim/Emacs）接入。~~ **实现摘要**：新增 `cli/lsp_core.h` + `.cpp` + `cli/lsp.cpp` 独立 `minilang-lsp` CLI 工具（JSON-RPC 2.0 over stdio，Content-Length 分帧）+ `tests/TestLspCli.cpp` 39 个测试（5 套件）。支持 11 个 LSP 方法：initialize/shutdown/exit + didOpen/didChange/didClose + publishDiagnostics + completion（35 关键字 + 已声明符号）+ hover（符号详情 + 关键字提示）+ definition（基于 SymbolIndex）+ documentSymbol（Function/Class/Variable/Enum/EnumMember）+ formatting（复用 Formatter）。关键设计：(a) QJsonDocument 处理 JSON（通过 minilang_core PUBLIC 链接 Qt6::Core 传递依赖）；(b) 位置转换 4 个内联函数（MiniLang 1-based ↔ LSP 0-based）；(c) SymbolIndex 用 ASTNode::nodeType switch + children() 递归遍历收集声明；(d) `useBufferIO_` 标志位支持单元测试（缓冲区 I/O 替代 stdin/stdout）；(e) Windows `_setmode(_O_BINARY)` 避免 \r\n 转换破坏分帧。编译期修复 4 个问题：DiagnosticBag 前向声明命名空间、`Field` 宏冲突（`#define Field FieldDecl`）、std::string 赋值 QJsonValueRef、handleHover 关键字分支不可达。这是第六个独立 CLI 工具。详见 CHANGELOG.md R164 LSP 条目。 | L | 抽象 IDE 逻辑为 Headless 服务 |
| ~~**包管理器**~~ ✅ R165（2026-07-23） | ~~`minilang install <package>` + `minilang.pkg` 清单文件，支持模块仓库索引。~~ **实现摘要**：新增 `cli/pkg_core.h` + `.cpp` + `cli/pkg.cpp` 独立 `minilang-pkg` CLI 工具（第 8 个 CLI 工具）+ `tests/TestPkgCli.cpp` 74 个测试（9 套件）。支持 4 个命令：init（创建清单）/install（从 registry 或 file: 安装）/list（列出已安装）/add（添加依赖并安装）。关键设计：(a) 清单 `minilang.pkg`（JSON），空字段省略；(b) 本地 registry 模式（复制包到 packages 目录）；(c) 双版本查找（版本子目录 → pkg.json version 回退）；(d) `resolveModule` 供 moduleLoader 注入（零侵入集成）；(e) `file:` 协议安装。详见 CHANGELOG.md R165 包管理器 CLI 条目。 | M | 模块系统稳定 |
| ~~**语言服务器 DAP**~~ ✅ R165（2026-07-23） | ~~实现 Debug Adapter Protocol，让 VS Code 等编辑器可调试 MiniLang。~~ **实现摘要**：新增 `cli/dap_core.h` + `.cpp` + `cli/dap.cpp` 独立 `minilang-dap` CLI 工具（JSON-RPC 2.0 over stdio，Content-Length 分帧）+ `tests/TestDapCli.cpp` 41 个测试（6 套件）。支持 16 个 DAP 请求（initialize/launch/setBreakpoints/configurationDone/continue/next/stepIn/stepOut/pause/stackTrace/scopes/variables/evaluate/threads/terminate/disconnect/source）+ 5 个事件（stopped/continued/output/terminated/exited）。关键设计：(a) 直接复用 StackVM 的 `stepOnce` + `getCallStack` + `getFrameLocalsAt`，不依赖 VmStepper/IdeController 的 QTimer 事件循环；(b) 行级步进自实现（指令级 stepOnce → 行级 next/stepIn/stepOut）；(c) 核心逻辑与 main 分离（dap_core 可独立编译为 gtest 测试目标）；(d) 内联源码回退（`source.source` 字段，便于单元测试无需磁盘文件）；(e) 顶层 `var` 是全局变量（`kGlobalsScopeReference=2000`），函数内 `var` 是局部变量（`kLocalsScopeReference + frameId`）。修复 2 个 P1 Bug：`doContinue` 断点重复命中（记录 startLine 在行号改变前不检查断点）+ `handleLaunch` 内联源码回退缺失（文件读取失败后回退到 `source.source`）。这是第 7 个独立 CLI 工具。详见 CHANGELOG.md R165 DAP 条目。 | M | 调试器抽象 |
| **代码格式化 CLI** ✅ 已完成（R109/R110，2026-07-19） | 将 Formatter 抽出为独立 `minilang-fmt` 命令，CI 集成。**实现摘要**：`cli/fmt_core.h/.cpp` + `cli/fmt.cpp` 独立可执行目标，支持 `--check`/`--write`/`--dry-run`，复用 Formatter 往返等价能力，`.pre-commit-config.yaml` 集成。 | S | Formatter 已稳定 |
| ~~**Lint 工具**~~ ✅ R162 B1（2026-07-22） | ~~静态分析器：未使用变量、可疑比较、未处理错误、圈复杂度等。~~ **实现摘要**：新增 `lint/LintPass.h` + `.cpp` 静态分析器（继承 `DefaultVisitor` 遍历 AST，8 项检查规则：UnusedVariable/UnusedFunction/UnusedParameter/AssignmentInCondition/DeadCodeAfterReturn/EmptyBlock/CyclomaticComplexity/NamingConvention）+ `cli/lint_core.h` + `.cpp` + `cli/lint.cpp` 独立 `minilang-lint` CLI 工具（完整管线 source → Lexer → Parser → LintPass::analyze，支持 --quiet/--rule/--max-complexity/--format text\|json 选项，退出码 0=成功/1=有警告/2=错误）+ `tests/TestLintCli.cpp` 50 个测试（4 套件：LintCliSource 18 + LintCliParseArgs 20 + LintCliProcessFile 6 + LintCliHelpers 5）。关键设计：(a) DefaultVisitor 继承模式只需覆盖 14 个需要的 visit 方法，未覆盖的自动走 defaultVisit → traverseChildren 默认遍历；(b) 作用域栈 `vector<unordered_map<string, VarInfo>>` 追踪变量使用，函数表 `unordered_map<string, FunInfo>` 追踪函数调用，exitScope/analyze 结束时检查未使用；(c) 圈复杂度 if/while/for +1 + BIN_AND/BIN_OR +1，通过 savedComplexity 保存/恢复支持嵌套函数；(d) 死代码检测用 unreachableInCurrentBlock_ 标志，return/break/continue 后设 true，新作用域保存重置；(e) 命名规范检查跳过全大写常量（如 MAX_SIZE）。这是继 `minilang-fmt`（R109）/`minilang-coverage`（R110）之后的第三个独立 CLI 工具。详见 CHANGELOG.md R162 B1 条目。 | M | 语义分析层 |
| ~~**文档生成器**~~ ✅ R162 B2（2026-07-22） | ~~从注释生成 API 文档（类似 rustdoc / javadoc）。~~ **实现摘要**：新增 `doc/DocGenerator.h` + `.cpp` 文档生成器（继承 `DefaultVisitor` 遍历 AST，DocCommentExtractor 从 Lexer 注释流提取 `///` 和 `/** */` 文档注释，支持 @param/@return/@example/@deprecated/@see/@since 标签）+ `cli/doc_core.h` + `.cpp` + `cli/doc.cpp` 独立 `minilang-doc` CLI 工具（完整管线 source → Lexer → Parser → DocGenerator::generate，支持 --format markdown\|html\|json / --output 选项）+ `tests/TestDocCli.cpp` 46 个测试（4 套件：DocCliSource 21 + DocCliProcessFile 4 + DocCliParseArgs 16 + DocCliHelpers 2）。关键设计：(a) 文档注释按 endLine 建立映射，findDocForDecl 用 declLine-1 查找（紧邻关联）；(b) `///` 去 3 字符前缀，`//` 去 2 字符前缀；(c) defaultVisit 手动遍历 node.children() 调用 accept（ASTNode 无 traverseChildren 工具）；(d) Markdown 按种类分组带 emoji 图标，HTML 带 CSS 样式，JSON 结构化便于 IDE 集成。这是第四个独立 CLI 工具，工具链覆盖"格式化/覆盖率/静态分析/文档生成"四大方向。详见 CHANGELOG.md R162 B2 条目。 | M | 文档工具 |
| ~~**Coverage 工具**~~ ✅ R111（2026-07-19） | ~~基于现有 VM 指令计数扩展为行级覆盖率报告。~~ **实现摘要**：新增 `cli/coverage_core.h` + `.cpp` + `cli/coverage.cpp` 独立 `minilang-coverage` CLI 工具，基于 VM 指令计数扩展为行级覆盖率报告。这是第二个独立 CLI 工具（继 `minilang-fmt` R109 之后）。 | S | VM profiling 已有 |
| ~~**模糊测试集成**~~ ✅ R164（2026-07-23） | ~~模糊测试 Lexer/Parser/VM，自动发现崩溃与未处理边界。教学价值：展示现代软件质量保障方法。~~ **实现摘要**：新增 `cli/fuzz_core.h` + `.cpp` + `cli/fuzz.cpp` 独立 `minilang-fuzz` CLI 工具，支持 `--mode generate`（模板化生成器，10 类别）和 `--mode mutate`（字节级变异，10 种子预设）两种模式，对三后端（Interpreter/StackVM/RegisterVM）进行差分测试，自动发现语义分歧。R164 fixup/fixup2 通过 mutate 模式发现并修复 17+ 个三后端分歧。 | M | libFuzzer 集成（实际采用自实现差分测试替代 libFuzzer） |
| ~~**性能基准套件**~~ ✅ R117（2026-07-19） | ~~标准化 benchmark（fib/tak/ackermann/sort），跨版本性能回归追踪。~~ **实现摘要**：`tests/TestPerformanceRegression.cpp` 拓展为完整基准套件，5 个 category（compute/recursion/memory/container/closure）9 项基准（large_loop/fibonacci/tak/ackermann/string_concat/bubble_sort/closure_counter 等），三后端（Interpreter/StackVM/RegisterVM）测量，JSON stdout 输出供 CI 采集。配合 R116 的 `scripts/perf_baseline.py` + `perf-baseline.json` + ci.yml `perf-benchmark` job 实现回归检测。 | S | 现有 ProfileDashboard 扩展 |

### 五、教学能力拓展

| 拓展项 | 说明 | 工程量 | 依赖 |
|--------|------|--------|------|
| ~~**可回放执行时间轴**~~ ✅ R114 阶段 1+2（2026-07-19/20） | ~~记录每步执行的 AST 节点路径 + 变量快照，支持时间轴拖拽回放（类似 Python Tutor）。教学价值：可视化执行流。~~ **阶段 1 实现摘要**：新增 `debug/ExecutionTraceRecorder.h` + `.cpp`（~280 行）三后端统一执行轨迹记录器（mutex 保护的环形缓冲区，容量 5000 步）+ `gui/ExecutionTimelinePanel.h` + `.cpp`（~430 行）教学面板（QSlider 随机访问回放 + 5 表 + LRU JSON 缓存 + VmStateChanged 监听 + 2s 安全网 QTimer）+ Interpreter checkBreak 钩子集成（atomic<bool> + mutex 跨线程安全）+ VmStepper maybeRecordStep 集成（绕过 stepCallback 禁用机制）+ IdeController 转发方法 + 35 个测试用例。阶段 1 实现"字符串化快照"语义（最简语义），覆盖 Interpreter/StackVM/RegisterVM 三条执行路径。**阶段 2 实现摘要**（2026-07-20）：新增 `Value::toJson()` 结构化 JSON 序列化层（全类型支持，带 `_type` 标签的容器对象格式，环检测 + 深度保护）+ `ASTNode::nodeId` 全局唯一标识符（atomic 计数器，构造时分配）+ `Interpreter::checkBreak` 填充 `astNodePath` 为 `"<nodeName>#<nodeId>"` 格式 + 16 个新增测试用例（13 个 ValueJsonSerialization + 3 个 ExecutionTraceRecorderAstPath）。阶段 3（状态深拷贝 + 状态回滚）后续推进。 | L | 执行轨迹序列化 |
| ~~**三后端并行可视化**~~ ✅ 已完成（2026-07-22） | ~~同时展示三后端执行同一代码的栈/寄存器状态，对比指令数与执行步骤。~~ **实现摘要**：新增 `gui/BackendParallelPanel.h` + `.cpp` 教学面板（ID `backend-parallel`，归入"执行引擎"分类），面板内部独立完成 Lexer → Parser → 三后端执行流程（参考 `TestNestedLvalueProbe.cpp` 的 `runInterp`/`runStackVM_IR`/`runRegVM_IR` 辅助模式，不依赖 IdeController）。单页设计：顶部 QLineEdit 源码输入 + 运行按钮 + 3 个内置样例切换（基本算术/容器操作/递归调用）；中间 QSplitter(Vertical) 上方 5 列 QTableWidget（后端/输出/指令数/状态/耗时ms）+ 下方 QSplitter(Horizontal) 三个 QTextBrowser 详细执行轨迹（编译阶段/执行阶段/统计信息 HTML）；底部加载样例到主编辑器按钮 + 一致性提示标签（✅ 三后端输出一致 / ❌ 不一致）。三后端在主线程串行执行（避免引擎层非线程安全问题），Interpreter 指令数显示 N/A（无字节码），StackVM/RegisterVM 显示 mainChunk.code.size()。 | M | 现有三后端面板整合 |
| ~~**内存布局可视化**~~ ✅ 已完成（2026-07-22） | ~~可视化 NaN-boxing 的位模式、COW 写时复制过程、闭包 upvalue 捕获链。教学价值：消除"魔法"感。~~ **实现摘要**：新增 `gui/MemoryLayoutPanel.h` + `.cpp` 教学面板（ID `memory-layout`，归入"执行引擎"分类），纯教学/模拟面板。2 子页设计：① 内存布局原理（NaN-boxing 位模式表 + COW 过程表 + upvalue 状态表）② 交互式内存布局模拟器（4 预设场景：整数 42 / 浮点 3.14 / 数组 COW / 闭包捕获，值类型分析表 + 8 字节位模式 HTML 可视化，颜色区分 tag/payload）。 | M | Value 内部状态暴露 |
| ~~**教学课程系统**~~ ✅ 已完成（2026-07-22） | ~~扩展 LearningPathPanel，支持自定义课程（YAML 描述章节/关卡/任务/答案校验）。~~ **实现摘要**：新增 `gui/CourseSystemPanel.h` + `.cpp` 教学面板（第 34 个教学面板，纯教学/不运行执行引擎）。与 LearningPathPanel（固定 5 阶段 21 活动进度跟踪）互补，本面板为可自定义课程内容管理。2 子页设计：① 课程库（QComboBox 选择 3 个预设课程 + QTextBrowser HTML 课程大纲 + 加载课程代码到编辑器）② 课程编辑器（QTextEdit JSON 编辑 + 解析预览 + 加载到课程库）。3 个预设课程（MiniLang 入门 / 进阶语法 / 高级特性，各 3 章 6 课），每课含 title/code/explanation/expectedOutput 四字段。**改用 JSON（QJsonDocument）替代 YAML** 避免引入第三方依赖（偏离原计划）。expectedOutput 仅展示不校验。 | M | 现有学习中心扩展 |
| ~~**交互式练习评分**~~ ✅ 已完成（2026-07-22） | ~~学生提交代码，自动运行测试用例集 + 风格检查 + 提示反馈。~~ **实现摘要**：新增 `gui/ExerciseGraderPanel.h` + `.cpp` 教学面板（ID `exercise-grader`，归入"深入实战"分类），复用 BackendParallelPanel 三后端执行模式。单页三区布局：顶部题目选择（4 预设：两数之和 / 阶乘计算 / 数组求和 / 字符串反转）+ 题目描述 + 代码编辑器；中间测试用例表（5 列：用例名/输入/期望输出/实际输出/状态）+ 三后端执行详情；底部评分表（4 项：测试通过率 60 分 + 代码风格 20 分 + 编译错误 10 分 + 运行时错误 10 分）+ 反馈报告。评分校验用 Interpreter 后端，执行详情展示三后端结果。风格检查：Tab 缩进 / 连续空行 / 行尾空格 / 长行 / 赋值运算符空格。 | M | 测试用例格式标准化 |
| ~~**AST 可视化编辑器**~~ ✅ 已完成（2026-07-22） | ~~拖拽方式构建 AST 节点，导出为 MiniLang 代码。教学价值：双向理解语法树与代码。~~ **实现摘要**：新增 `gui/AstVisualizerPanel.h` + `.cpp` 教学面板（ID `ast-editor`，归入"编译前端"分类）。与现有 `ast-toy`（关卡式 AST 构建练习）互补，本面板侧重"源码 → AST"的可视化对应关系与节点参考查询。面板内部独立完成 Lexer → Parser → AST 流程（不依赖 IdeController），2 子页设计：子页 1（源码 → AST 可视化）含 QTreeWidget 树形展示 + QTextBrowser 节点 HTML 详情，子页 2（AST 节点参考）含 31 条节点类型文档。 | M | AST 节点序列化 |
| ~~**三后端性能竞赛面板**~~ ✅ 已完成（2026-07-22） | ~~输入代码，三后端并行执行，柱状图展示耗时/指令数/内存，支持动画回放。~~ **实现摘要**：新增 `gui/PerformanceRacePanel.h` + `.cpp` 教学面板（ID `performance-race`，归入"执行引擎"分类），复用 BackendParallelPanel 三后端执行模式。单页设计：顶部源码输入 + QSpinBox 运行次数（1-10 默认 5）+ 竞赛按钮 + 3 样例按钮；中间 QSplitter(Vertical)：5 列性能对比表 / HTML/CSS 柱状图 / 5 节性能分析报告（最快后端 / 加速比 / 性能差异原因 / 指令数对比 / 各次运行明细）。多次运行取平均（avg/min/max），柱状图用 HTML/CSS 嵌套 table + bgcolor 渲染（Interpreter 蓝 / StackVM 绿 / RegisterVM 橙）。 | S | ProfileDashboard 扩展 |
| ~~**循环展开可视化**~~ ✅ 已完成（2026-07-22） | ~~JIT 经典优化原理 + 交互式展开模拟器。~~ **实现摘要**：新增 `gui/LoopUnrollingPanel.h` + `.cpp` 教学面板（ID `loop-unrolling`，归入"执行引擎"分类），纯教学/模拟面板（不运行执行引擎）。2 子页设计：① 循环展开理论（6 列指令数对比表 + MiniLang 状态对照）② 交互式模拟器（循环次数 + 展开因子 K + 4 预设 + 伪代码预览 + 加速比计算）。模拟算法：iterations=ceil(N/K), controlInstrs=ceil(N/K)*2, speedup=N*7/(ceil(N/K)*2+N*5)。 | S | JIT 配套 |
| ~~**逃逸分析可视化**~~ ✅ 已完成（2026-07-22） | ~~逃逸三态原理 + 交互式逃逸分析模拟器。~~ **实现摘要**：新增 `gui/EscapeAnalysisPanel.h` + `.cpp` 教学面板（ID `escape-analysis`，归入"执行引擎"分类），纯教学/模拟面板。2 子页设计：① 逃逸三态理论（No/Arg/Global Escape + 栈上分配对照）② 交互式模拟器（4 预设场景：不逃逸 / 全局逃逸 / 参数逃逸 / 返回值逃逸，对象分析表 + 优化建议）。简单规则匹配：对象赋值给全局变量→Global Escape，作为返回值→Global Escape，作为参数→Arg Escape，仅函数内使用→No Escape。 | S | JIT/GC 配套 |
| ~~**寄存器分配可视化**~~ ✅ 已完成（2026-07-22） | ~~分配算法原理 + 线性扫描模拟器。~~ **实现摘要**：新增 `gui/RegisterAllocatorPanel.h` + `.cpp` 教学面板（ID `register-allocator`，归入"执行引擎"分类），纯教学/模拟面板。2 子页设计：① 分配算法理论（图着色 / 线性扫描 / SSA-based / MiniLang 4 路对比 + 32 虚拟寄存器模型）② 交互式模拟器（4 预设场景：3 变量 / 8 变量 / 40 变量部分溢出 / 循环变量复用，变量-寄存器分配表 + 32 行寄存器使用表）。核心分析逻辑：extractLiveRanges() + computeActivePeak() + runLinearScan()（简化版线性扫描）。 | S | RegisterVM 配套 |
| ~~**错误消息教学模式**~~ ✅ R117（2026-07-19） | ~~在错误消息旁显示"为什么"链接，弹出对该错误的详细解释与修复示例。~~ **实现摘要**：`gui/ErrorHintEngine.cpp` 新增 R117 错误消息教学模式 API（`errorPatterns()` 返回错误模式表，含 tag/title/buggyCode/category 字段）+ `enrichErrorMessage(msg, code, phase, scopeVars)` 4 参重载优先按稳定 code 精确匹配附加教学提示，避免消息文案变化时子串匹配失效。配合既有 `suggestSpelling`（Levenshtein 编辑距离 ≤ 2）+ 子串模式匹配（缺分号/括号不匹配/未定义变量/除零/越界/类型错误），形成"错误消息 + 拼写建议 + 教学解释"三层增强。 | S | ErrorMessages 索引扩展 |
| ~~**执行步骤讲解生成器**~~ ✅ 已完成（2026-07-22） | ~~自动为当前执行的指令生成自然语言讲解（"现在执行 OP_ADD，从栈顶弹出两个 int 相加并 push 结果"）。~~ **实现摘要**：新增 `gui/StepExplainerPanel.h` + `.cpp` 教学面板（ID `step-explainer`，归入"执行引擎"分类）。面板内部独立完成 Lexer → Parser → Compiler → Bytecode 流程，对每条 OpCode 生成自然语言讲解。 | M | 指令文档表 |

### 六、平台与生态拓展

| 拓展项 | 说明 | 工程量 | 依赖 |
|--------|------|--------|------|
| **WebAssembly 移植** | 将 IDE 移植为 Web 应用（Qt for WebAssembly），浏览器中运行。 | L | Qt WASM 构建 |
| ~~**嵌入式 API**~~ ✅ 已完成（拓展二期·平台） | ~~提供 C API（`minilang_eval/source/debug`），让 MiniLang 作为嵌入式脚本语言用于其他 C++ 项目。教学价值：展示 Lua/Python 嵌入式设计。~~ **实现摘要**：`capi/minilang_capi.h/.cpp` 稳定 C ABI（create/eval/get_output/get_error/set_backend/destroy）+ `minilang_capi` STATIC 库（`cli/CMakeLists.txt` L192），嵌入方链接 `minilang_capi + minilang_core` 即可；测试目标编译 `capi/minilang_capi.cpp`（`tests/CMakeLists.txt` L214）。 | M | API 边界稳定化 |
| **移动端探索** | Qt for Android/iOS 移植，触控交互优化。 | L | 平台适配 |
| ~~**多语言 UI**~~ ✅ R117（2026-07-19） | ~~在现有 zh_CN 基础上扩展 en_US/ja_JP 等，验证 i18n 基础设施。~~ **实现摘要**：`app/translations/` 目录建立 Qt Linguist 流程，`minilang_zh_CN.ts`（269KB 主翻译）+ `minilang_en_US.ts`（英文翻译）双语言文件，CMake 集成 `qt_add_translation`。`mlTr()` 函数包装 `QObject::tr()` 支持运行时语言切换，UI 文本统一通过 `mlTr("...")` 调用便于提取与翻译。 | S | Qt Linguist 流程 |
| **插件系统** ✅ 已完成（七特性 MVP） | 宿主函数注册 + 动态库（.dll/.so）插件加载，扩展 MiniLang 可调用的原生能力。教学价值：展示嵌入式语言的宿主扩展机制。**实现摘要**：`common/HostFunctionRegistry`（中立 HostValue 标量类型 + name→fn 全局表），三后端经 isBuiltinFunction/executeSharedBuiltinFunction 共享层查表调用（零指令集改动）；C API 新增 minilang_register_function/minilang_load_plugin（LoadLibraryW/dlopen + 注册回调表 ABI 避免插件链接依赖）；注册生命周期归属注册 context——`minilang_destroy` 先注销并释放宿主函数再卸载插件库，避免悬垂代码页；沙箱下拒绝加载；测试见 `tests/TestPlugin.cpp`（含真实 DLL 加载调用）。注：GUI Qt 面板插件化仍待做。 | M | 插件接口抽象 |
| **沙箱安全模式** ✅ 已完成（七特性 MVP） | 能力级限制：禁 input 读取、禁导入文件模块（std/ 内建模块仍放行）、禁插件加载，支持运行不受信任代码。教学价值：展示语言运行时安全模型。**实现摘要**：RuntimeConfig 单例新增 sandboxEnabled/AllowInput/AllowImport 原子开关（与 DoS 限制正交）；input 在共享层 executeSharedInput 单点拦截，import 在三后端路径仅放行 std/；C API minilang_set_sandbox（eval 期间经线程局部 `SandboxScope` 应用，异常安全且跨 context 隔离）；测试见 `tests/TestSandboxMode.cpp`。 | M | 内置函数分层 |

### 七、基础设施拓展

| 拓展项 | 说明 | 工程量 | 依赖 |
|--------|------|--------|------|
| ~~**CI 性能基准门槛**~~ | ~~CI 中自动运行 benchmark，与基线对比超阈值则失败。~~ ✅ 已完成（R116，2026-07-19） | S | 性能基准套件 |
| ~~**覆盖率门槛**~~ | ~~CI 中检查测试覆盖率，低于阈值则失败。~~ ✅ 已完成（R116，2026-07-19） | S | OpenCppCoverage 已集成 |
| ~~**静态分析集成**~~ | ~~clang-tidy / cppcheck / PVS-Studio 集成到 CI。~~ ✅ 已完成（R116，2026-07-19，cppcheck + clang-tidy） | S | .clang-tidy 配置 |
| ~~**提交前钩子**~~ | ~~git pre-commit hook 自动运行 clang-format 检查。~~ ✅ 已完成（R116，2026-07-19） | S | 钩子脚本 |
| ~~**Doxygen 文档 CI 部署**~~ | ~~CI 自动构建 Doxygen 并部署到 GitHub Pages。~~ ✅ 已完成（R116，2026-07-19） | S | Doxyfile 已配置 |
| ~~**Release 自动化**~~ | ~~GitHub Actions 自动构建 Windows/Linux/macOS 安装包并发布 Release。~~ **实现摘要**：`.github/workflows/release.yml` 推送 `v*.*.*` tag 自动触发跨平台矩阵构建（Win MSVC / Linux GCC / macOS Clang）→ cpack 打包 → 重命名为 `MiniLangIDE-<version>-<platform>.<ext>` → 生成 SHA256SUMS → softprops/action-gh-release 创建 GitHub Release 上传产物。支持 `workflow_dispatch` 手动触发指定版本号 + 标记预发布。 | M | 跨平台构建矩阵 |
| ~~**崩溃报告系统**~~ | ~~集成 Breakpad/Crashpad，自动收集用户崩溃报告。~~ **实现摘要**：采用轻量级跨平台方案（无外部依赖，匹配教学项目定位）。`common/CrashHandler.h/cpp`：Windows 用 `SetUnhandledExceptionFilter` + `MiniDumpWriteDump`（dbghelp）+ pure virtual/invalid parameter handler；POSIX 用 `sigaction` 注册 SIGSEGV/SIGABRT/SIGFPE/SIGILL/SIGBUS，handler 内仅用 async-signal-safe 函数（`write/open/backtrace_symbols_fd`）写文本栈回溯。`gui/CrashReportDialog.h/cpp`：模态对话框显示信号/时间戳/dump 路径/栈回溯，提供"打开目录/复制信息/删除报告/忽略"按钮，QSettings 持久化"忽略此报告"偏好。`app/main.cpp` 在 QApplication 前安装 handler，启动时检查上次崩溃。10 个单元测试覆盖 install/consume/cleanup/callback 路径（不触发真实崩溃，避免 CI 危险）。 | M | Breakpad 集成（实际采用轻量自实现） |
| ~~**Benchmark 持续追踪**~~ | ~~使用 bencher / continuous-benchmark 在 CI 中追踪性能趋势。~~ **实现摘要**：`.github/workflows/benchmark-tracking.yml` 触发 push/schedule/手动 → 运行 `minilang_perf_test` → `scripts/perf_to_benchmark_action.py` 把 perf JSON 转换为 benchmark-action 兼容格式（每行 `{"name":"<Backend>::<bench>","unit":"ms","value":...,"extra":{...}}`）→ benchmark-action/github-action-benchmark 持久化数据到 `gh-pages/dev/bench/data.js` → 自动生成趋势图页面。PR 仅测量不更新数据（防污染），push/schedule 才写入。 | M | 基准套件稳定 |

### 优先级建议

按教学价值 / 工程量 / 现有基础设施复用度三维度评估，建议优先级：

**短期可推进（S 工程量，高复用现有设施）**：
1. ~~元组与解构（语言能力，教学价值高）~~ ✅ 已完成（R100，2026-07-19）
2. ~~Logpoint / 异常断点 / 函数断点（调试器，复用现有断点机制）~~ ✅ 已完成（R104，2026-07-19）
3. ~~代码格式化 CLI（工具链，Formatter 已稳定）~~ ✅ 已完成（R109/R110，2026-07-19）
4. ~~Coverage 工具（工具链，复用 VM profiling）~~ ✅ 已完成（R111，2026-07-19）
5. ~~三后端性能竞赛面板（教学，复用 ProfileDashboard）~~ ✅ 已完成（R112，2026-07-19）
6. ~~TCO（执行引擎，编译期改造）~~ ✅ 已完成（R112 + R113 收尾，2026-07-19）
7. ~~枚举与 ADT（语言能力，M 工程量但教学价值高，可作为下一项语言能力拓展）~~ ✅ 已完成（R99/R102/R103，2026-07-19，含 match 表达式 + variant 校验三后端对齐）
8. ~~多语言 UI（平台与生态，Qt Linguist 流程）~~ ✅ 已完成（R117，2026-07-19，`app/translations/` 双语言文件 + `mlTr()` 包装）
9. ~~性能基准套件（工具链，标准化 benchmark）~~ ✅ 已完成（R117，2026-07-19，`TestPerformanceRegression.cpp` 5 category 9 项基准 + JSON 输出）
10. ~~错误消息教学模式（教学，错误消息 + 教学解释）~~ ✅ 已完成（R117，2026-07-19，`ErrorHintEngine` 新增 `errorPatterns()` + 4 参 `enrichErrorMessage` 按 code 精确匹配）
11. ~~观察表达式面板（调试器，持久化表达式监视）~~ ✅ 已完成（R117，2026-07-19，`evaluateWatchExpression` API + `WatchPanel` 双子页 + VmStateChanged 监听 + Library 抽取模式）

**中期推进（M 工程量，显著能力增量）**：
1. ~~模式匹配 + 枚举与 ADT（语言能力核心拓展）~~ ✅ 已完成（R99/R102/R103，2026-07-19，含 match 表达式 + enum + ADT variant + 三后端校验对齐；~~后续可扩展为更复杂的 pattern matching：嵌套 pattern / guard 表达式 / OR pattern~~ → ✅ 已完成 R134，2026-07-20，扩展 6 种 pattern（WILDCARD/LITERAL/VARIABLE/VARIANT/TUPLE/OR）+ guard 表达式 + 嵌套 pattern，新增 OP_LEN/OP_TYPE_TEST 三后端实现，修复 IR 路径 failLabels 累积 POP 栈下溢 P1 bug，23 个 EXPECT_ALL_BACKENDS 测试）
2. LSP 服务器（工具链，让 MiniLang 接入主流编辑器生态）
3. ~~Inline Cache 优化（执行引擎，性能跃升）~~ ✅ 实质已完成：JIT `OP_MEMBER_GET` 单态 IC（R160，2026-07-22，见上方拓展表）+ StackVM 三级缓存（`callCache_`/`varCache_`/`methodCache_`，PERF-14）。残余的 StackVM/RegisterVM 成员字段 IC 经 2026-10-07 实测评估**不实施**（短 SSO 键 find 仅 5.8ns，收益上限 ~2.5%，见 CHANGELOG Unreleased 裁决条目）。
4. ~~可回放执行时间轴（教学，差异化竞争力）~~ ✅ R114 阶段 1+2+3 已完成（2026-07-19/20，阶段 1：三后端统一执行轨迹录制 + QSlider 随机访问回放面板 + 35 个测试用例；阶段 2：Value JSON 序列化层 + AST 节点路径记录 + 16 个新增测试用例 + 收尾轮补齐 StackVM/RegisterVM 端到端录制测试与三后端一致性对比测试 15 个用例；阶段 3：状态回滚 `restoreFromSnapshot` 双后端实现 + VmStepper 统一门面 + FullState 模式结构化 Value 采集 + 6 个回滚测试用例）
5. ~~内存布局可视化（教学，展示 NaN-boxing 设计精妙）~~ ✅ 已完成（R113，2026-07-19，4 项打包：D NaN-box 交互式编辑器 + B 闭包 upvalue 生命周期 + C GC 实时进度 + A RegisterVM 寄存器帧；MemoryModelPanel 从 4 子页扩为 5 子页）
6. ~~教学课程系统（教学，可扩展性）~~ ✅ 已完成（2026-07-22，第 34 个教学面板 `CourseSystemPanel`，2 子页课程库 + JSON 课程编辑器，3 预设课程，改用 JSON 替代 YAML 避免新依赖）

**长期推进（L 工程量，架构级演进）**：
1. ~~JIT 编译（执行引擎，第四后端）~~ ✅ 阶段 4b dict/tuple/OP_INDEX_SET_VAR 已完成（R147，2026-07-21，dict/tuple/全局容器索引赋值支持：新增 3 种 OpCode（OP_BUILD_DICT/OP_BUILD_TUPLE/OP_INDEX_SET_VAR），JIT 现支持 40 种 OpCode；编译期 nameIdx→slot 解析——JITBackend 新增 `globalNameToSlot_` 成员（`unordered_map<string,int>`），`execute()` 入口遍历 `CompileResult.globalSlotNames` 构建，`OP_INDEX_SET_VAR` 处理时 O(1) 查询避免运行时 hash 查找开销，slot 偏移内联到 `lea rdx, [rdx + slot*8]` 指令；扩展 jitIndexGet 支持 dict（键 string/int/bool/float，不存在键返回 null）+ tuple（immutable，越界报"元组索引越界"）；扩展 jitIndexSetLocal 支持 dict 索引赋值；新增 3 个 extern "C" 辅助函数：jitIndexSetGlobal（全局变量数组/字典索引赋值，slotPtr 编译期计算）/jitBuildDict（构造字典 + DictMap::emplace）/jitBuildTuple（构造元组 + Value::makeTuple）；24 个 R147 专项测试，全量 2537/2537 通过零回归；阶段 1 PoC R138 / 阶段 2a R139 / 阶段 2a+ R140 / 阶段 2b R141 / 阶段 3a R142 / 阶段 3b R143（浮点+字符串，"INT 快速路径 + C++ 辅助慢速路径"模式，修复 asmjit 64 位立即数截断 P0 bug）/ 阶段 3b+ R144 INT 标签检查优化（shr+cmp 替代 movabs+and+cmp，消除 56 条 movabs 指令）/ 阶段 3b++ R145 系统性 Bug 排查与修复（审查 34 个 OpCode，修复 3 个 P1 语义 Bug：C++ 辅助函数补全 INT 路径 + INT 原生路径 INT48 溢出检测"先运算不 pop，溢出则走 C++ 辅助"模式 + OP_DIVIDE/MODULO INT64_MIN/-1 检查；性能 40-75x 保持）/ 阶段 4a R146 数组类型支持（OP_BUILD_ARRAY/INDEX_GET/INDEX_SET_LOCAL + JitContext.stackTop 字段 + COW 语义对齐）；后续阶段 4c 类支持（OP_DEFINE_CLASS/METHOD_CALL/MEMBER_GET/SET 等复杂机制）/ 阶段 5 热点检测+性能基准量化）
2. ~~协程 / async-await（语言能力 + 引擎，现代语言必备）~~ ✅ 已完成：生成器/协程 R164（2026-07-23）+ async/await 七特性 MVP + 真挂起架构 [ADR-007](adr/ADR-007-coroutine-suspension.md)（2026-10-05 Windows，2026-10-07 平台覆盖扩展至 Linux ucontext）
3. WebAssembly 移植（平台，扩大受众）
4. ~~回溯调试（调试器，差异化能力）~~ ✅ 已完成（2026-07 反向调试时间轴，见上方拓展表"回溯调试（Reverse Debugging）"条目——L18 基础设施 + R114 时间轴/回滚 + ReverseDebugTimelinePanel；2026-10-07 列表勘误勾销）
5. 泛型 / 模板（语言能力，类型系统重构）——R163 已落地泛型函数/类/enum + 擦除；2026-10-07 补齐复合注解擦除四后端一致 + 显式类型实参语法（解析后擦除）。**残余**：泛型运行时强检（按声明类型参数校验成员写入/载荷，对齐 dict[K:V] 强检）——语义变更，需 ADR 立项。

### 拓展原则

1. **三后端一致性优先**：任何语言能力拓展必须同时落地 Interpreter / StackVM / RegisterVM 三条路径，并新增三后端等价测试。这是 MiniLang 的核心不变量（参见 AGENTS.md）。
2. **教学价值优先**：每个拓展需明确教学价值（"展示什么概念"），避免堆砌功能而失去教学聚焦。
3. **API 稳定性**：拓展不应破坏现有公开 API（见上文"核心公开 API 文档"章节），必要时通过版本化接口演进。
4. **测试先行**：每个拓展需配套测试用例（参考 [docs/testing.md](testing.md) 三后端等价测试方法论），覆盖率不低于现有水平。
5. **文档同步**：拓展落地后需同步更新 README / CHANGELOG / development.md / ADR（架构决策记录）。
6. **构建验证**：每个拓展需通过 `clang-format==22.1.5` 零违规 + MSVC+Qt 构建 + 全量测试零回归（参见 CI 配置）。
