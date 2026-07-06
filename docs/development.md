# MiniLang 开发指南

本文档是 MiniLang IDE 开发文档的导航入口。详细的架构设计、决策记录与测试方法请参见各子文档。

## 文档导航

| 文档 | 说明 |
|------|------|
| [核心架构](architecture.md) | 编译管线、NaN-boxing、三后端、内存模型、安全约束 |
| [测试指南](testing-guide.md) | 三后端一致性验证方法论、测试分类与编写指南 |
| [设计决策 (ADR)](design-decisions/) | 架构决策记录，详见下方索引 |
| [贡献指南](../CONTRIBUTING.md) | 环境搭建、编码规范、PR 流程 |

## 设计决策记录 (ADR)

| ADR | 主题 | 状态 |
|-----|------|------|
| [ADR-001](design-decisions/ADR-001-nan-boxing.md) | NaN-boxing 值表示 | Accepted |
| [ADR-002](design-decisions/ADR-002-triple-backend.md) | 三后端并存策略 | Accepted |
| [ADR-003](design-decisions/ADR-003-ir-layer.md) | IR 中间层 | Accepted |
| [ADR-004](design-decisions/ADR-004-cow-memory.md) | COW 内存模型 | Accepted |
| [ADR-005](design-decisions/ADR-005-debug-consistency.md) | 调试一致性 | Accepted |

## 核心设计原则

- **PERF-12 NaN-boxing**：8 字节值编码，标量零分配，堆类型侵入式引用计数
- **PERF-13 VMStack**：定长数组 1024×8B + 栈顶指针，消除 push_back 开销
- **PERF-14 寄存器式 VM**：32 虚拟寄存器 R0-R31，50+ RegOp，IR 直接 lowering
- **PERF-15 IR 复制传播**：vreg→常量等价替换，仅寄存器后端启用
- **COW 优化**：修改前检查独占所有权，避免不必要的深拷贝

## 安全约束摘要

| 类别 | 措施 |
|------|------|
| DoS 防护 | 源码 ≤10MB、Token ≤100 万、循环 ≤1000 万次 |
| 递归保护 | Parser/Compiler/Formatter 深度限制 512/256 |
| 线程安全 | mutex + atomic、localtime_s/localtime_r |
| 异常安全 | UI 槽 try/catch、unique_ptr 防 bad_alloc 泄漏 |
| 内存安全 | 运行时越界检查、NaN-box 类型前置校验、value-initialize |
| 生命周期 | 无裸指针、shared_ptr 共享所有权、QThread unique_ptr 管理 |

## 开发日志

以下为按时间倒序排列的开发变更记录摘要，完整内容已拆分至各子文档。

### 2026-07-06 变更摘要

| 类别 | 内容 | 详情 |
|------|------|------|
| 编辑器核心功能增强 | CodeEditor 4 项基础功能补齐 + 工作区 UX 增强：H1 Tab/Shift+Tab 多行缩进（每行行首插入/移除 4 空格，beginEditBlock 单次 undo）+ 单行 Tab 插入 4 空格 + 回车自动复制上一行缩进（{ 结尾加一级）；H2 括号匹配高亮（光标停在 ([{ 或 )]} 时半透明黄色高亮对端，bracketSelections_ 成员由 highlightCurrentLine 合并避免覆盖查找/错误/执行行高亮，cursorPositionChanged 触发）；H3 Ctrl+G 跳转行（editMenu 菜单项 + QInputDialog，1~blockCount 范围校验）；H4 Ctrl+/ 注释切换（选中范围每行行首切换 //，智能检测全注释则批量移除）；状态栏新增 statusEngineLabel_ 显示 ⚙ 引擎名（engineCombo_ 信号同步）；文件树加 fileTreeFilterEdit_ 搜索框（递归过滤，叶子按文件名匹配目录按可见子项显隐）+ 右键菜单 +3 项（复制路径/相对路径/在资源管理器中显示）；错误列表加 4 checkable 过滤按钮（错误/警告/信息/提示，复用 Qt::UserRole+3 级别 setHidden），1668/1668 测试通过 | [CHANGELOG](../CHANGELOG.md) |
| 教学面板 QFluentKit 控件统一迁移 | 19 个教学面板原生 QWidget 控件批量迁移到 QFluentKit：8 个面板主操作按钮（运行/检查/刷新/加载）→ PrimaryPushButton（BugHunt/LearningPath/IRTransform/TokenPuzzle/AstBuilderToy/VmStackSandbox/MemoryModel/BackendCompare/ProfileDashboard/LabManual/SyntaxExplorer）；19 个面板标题/状态 QLabel → TitleLabel/CaptionLabel/StrongBodyLabel（区分顶部标题/状态副标题/强调数据）；wrapTeachingPanel() 新增 applyFluentScrollBars lambda 替换所有 QAbstractScrollArea 子类的滚动条为 QFluentKit ScrollBar（className 检查防重复）；3 个硬编码颜色热点（IrViewer #f8f8f8 / CodeJourneyInfoPanel #f5f5f5 / VmStackSandboxPanel 标签背景）替换为 TeachingTheme::surface() 等主题色板；2 个 `<pre>` 代码块背景（BreakpointConditionPanel/IRTransformPanel）同样替换；保留全部信号连接（PrimaryPushButton 继承 QPushButton）、不动 CodeEditor.cpp 与主题切换逻辑、工具箱/导航按钮保留原生 QPushButton 避免视觉过载，1668/1668 测试通过 | [CHANGELOG](../CHANGELOG.md) |
| 主题切换系统真实化 | 亮/暗主题切换完整实现：标题栏 TransparentToolButton 主题切换按钮（CONSTRACT 图标，亮色月亮/暗色太阳）+ Theme::onThemeModeChanged 信号响应（重算 QSS + 更新图标 + 通知所有 CodeEditor setDarkTheme）+ QSettings 持久化（theme_mode 字段，默认 light）+ styles.qss 资源注册清理（applyFluentStyle 集中管理）+ VmStackPanel 样式迁移到 applyFluentStyle 主题自适应 + 全编辑器标签主题同步（原仅更新当前活跃编辑器）；修复 3 个预存构建阻断（ide.h 5 个缺失成员声明 + QClipboard include + tests QFluent include 路径/链接），1668/1668 测试通过 | [CHANGELOG](../CHANGELOG.md) |
| IDE UX 增强 | 状态栏引擎显示 + 文件树搜索与右键菜单扩展 + 错误列表类型过滤：状态栏新增第 5 permanent QLabel 显示 `⚙ <引擎名>`，监听 engineCombo_ currentIndexChanged 同步（controller_ 检查前更新保证无 controller_ 也生效）；文件树上方加 QLineEdit 过滤框（placeholder「搜索文件...」+ 清除按钮），textChanged 递归 setHidden 过滤（叶子按文件名匹配、目录按可见子项决定显隐）；文件树右键菜单 +3 项（复制路径/复制相对路径/在资源管理器中显示，Windows 用 explorer /select,）；错误列表上方加 4 checkable QToolButton（错误/警告/信息/提示，checked 填充语义色），applyErrorFilter 复用 Qt::UserRole+3 级别 setHidden，appendError 末尾调用保证新增项遵循过滤；全部改动集中在 app/ide.h/.cpp，1668/1668 测试通过 | [CHANGELOG](../CHANGELOG.md) |
| 教学导航增强 | 学习中心对话框 + 教学面板统一标题栏 + TeachingTheme 主题色板 + WelcomeWizard Step 4 + 首次展开：ActivityBar「学习」入口 → LearningHubDialog 4 分组卡片导航（入门导览/编译前端/执行引擎/深入实战）；TeachingPanelHeader 19 面板「这是什么？」帮助文案 + 「学习路径」跳转；gui/TeachingTheme.h 集中色板（primary/textSecondary/surface/border/accent 等，跟随 Theme::isDark() + themeColor() 自适应）；WelcomeWizard 新增 Step 4 学习路径推荐（5 阶段彩色卡片 + 「开始学习 ✓」按钮）；首次用户向导完成后自动展开 LearningPathPanel；3 处 WelcomeWizard 创建点信号连接一致；CallStack/VariableInspector/BytecodeTrace 3 面板新增 fadeInWidget 子页动画；视图菜单 24 项平铺重构为 4 子菜单 + 学习中心入口（Ctrl+Shift+L），1668/1668 测试通过 | [CHANGELOG](../CHANGELOG.md) |
| WelcomeWizard 主题化 | 欢迎向导 QFluentKit 主题化迁移 + IDE 构建修复：按钮迁移到 PrimaryPushButton/PushButton（Step1 开始探索/Step4 开始学习→PrimaryPushButton；跳过/prev/next→PushButton；运行按钮保留 QPushButton 用 TeachingTheme::success() 着色）；标签迁移到 TitleLabel/CaptionLabel；硬编码颜色（#0078d4/#5a5a5a/#666/#8a8a8a/#3060c0/#107d58 等）替换为 TeachingTheme 主题色板（primary/textSecondary/textHint/surface/warning/success），暗色主题自适应；修复两个预存 IDE 构建阻断（QFluentKit 组件头文件 include 路径缺失 + TeachingPanelHeader.h titleLabel_ 类型 QLabel*→StrongBodyLabel* latent bug）；无行为变更，4 步流程/信号/tokenSpans_ 保持不变 | [CHANGELOG](../CHANGELOG.md) |
| 第四档 UI 接线 | 5 个教学面板 IDE 集成（CodeJourneyInfoPanel / AstBuilderToyPanel / TokenPuzzlePanel / VmStackSandboxPanel / LearningPathPanel）：8 触点完整注册（ide.h include + 成员变量 + view QAction + dock 创建 + 信号连接 + 启动隐藏 + connectDockSave + syncViewMenuChecks）；跨面板信号路由（onJumpToPanel 6 目标 + onActivityRequested 21 活动 ID + 3 游戏 activityCompleted→markActivityCompleted 含 ID 映射）；纯静态面板模式（无 setController），1668/1668 测试通过 | [CHANGELOG](../CHANGELOG.md) |
| 功能 2 降级 | 代码生命旅程静态信息图（CodeJourneyInfoPanel）：原方案 30 秒动画与语言特性紧耦合维护成本高，降级为静态 HTML 信息图 + 6 个跳转按钮；展示 `print(1 + 2 * 3);` 完整生命旅程（源码→Token→AST→IR→字节码→输出），强调"括号改变 AST 结构"与"三后端一致性"；QTextBrowser 渲染 + jumpToPanelRequested 信号；不依赖 IdeController / 引擎层，无测试要求（纯静态信息图），minilang_ide 构建验证通过 | [CHANGELOG](../CHANGELOG.md) |
| 教学面板 E2E | 教学面板端到端交互测试（TestTeachingPanelsE2E）：8 个面板（ExceptionFlow / ClosureInspector / MemoryModel / IRTransform / CallStack / VariableInspector / BytecodeTrace / BreakpointCondition）实例化 + 子页切换 + 列表 → 详情联动 + Library 一致性 + QTimer 在 nullptr controller 下不触发；QApplication 懒初始化（Meyers 单例）+ 防御性 stopAllTimers + nullptr controller 安全路径验证；55 新增测试用例，9 套件 | [CHANGELOG](../CHANGELOG.md) |
| 功能 9 | Bug 狩猎分级题库重构（BugHuntDifficulty）：BugHuntItem 新增 difficulty 字段（BEGINNER/INTERMEDIATE/EXPERT），原 10 道重分级（4 进阶 + 6 专家）+ 新增 5 道入门级阅读理解型（BUG-READ-01~05：整数除法 / 块作用域 / Formatter 展开 / 循环 var / COW refCount）；BugHuntPanel 新增 4 互斥难度筛选按钮（默认入门级），Qt::UserRole 记录实际索引；16 新增测试用例，1 套件 | [CHANGELOG](../CHANGELOG.md) |
| 功能 1 | Welcome 向导 / 首次启动导览（WelcomeWizard）：QDialog + QStackedWidget 3 步交互式导览（角色选择 / Token 高亮 / 字节码模拟运行），首次启动 QSettings 持久化标记 + "再次显示欢迎向导"菜单项；纯前端模拟（QTimer 延迟输出 "Hello!"），不依赖 IdeController / 引擎层；无测试要求（依赖 Qt Widgets），minilang_ide 构建验证通过 | [CHANGELOG](../CHANGELOG.md) |
| 功能 11 | REPL %magic 命令系统（MagicCommands）：10 个命令（help/version/disassemble/ir/ast/tokens/memory/profile/compare/reset），ReplPanel::executeLine 开头检测 % 前缀路由到 MagicCommands::handle；MagicCommands.cpp 加入 MINILANG_CORE_SOURCES（仅依赖 IdeController 内联方法 + minilang_core 符号），13 新增测试用例，2 套件 | [CHANGELOG](../CHANGELOG.md) |
| 功能 5 | VM 栈沙盒（VmStackSandboxPanel）：5 关卡由浅入深（1+2 / 1+2*3 / (1+2)*3 / "hello" / 自由模式），内置 std::vector<std::string> 栈状态机模拟（不依赖真实 VM），核心教学时刻关卡 2 vs 关卡 3 同指令不同顺序验证"运算顺序由指令决定"；SandboxLevels.cpp 独立编译单元，18 新增测试用例，1 套件 | [CHANGELOG](../CHANGELOG.md) |
| 功能 4 | AST 节点搭建玩具（AstBuilderToyPanel）：6 道题由浅入深（42 / 1+2 / 1+2*3 / (1+2)*3 / print(x) / var x=1+2;），核心教学时刻题 3 vs 题 4 验证"括号改变 AST 结构"；QTreeWidget + 工具箱 + 严格拓扑比对；AstToyLevels.cpp 独立编译单元，17 新增测试用例，1 套件 | [CHANGELOG](../CHANGELOG.md) |
| 功能 3 | 交互式 Token 拼图游戏（TokenPuzzlePanel）：5 关卡（var x=42; / print("hello"); / x>0 and y<10; / 注释 / a[0]=b["key"]+1;），难度 1-3，星级评分（0 提示=⭐⭐⭐ / 1=⭐⭐☆ / 2+=⭐☆☆ / 跳过=☆☆☆），关卡解锁机制；TokenPuzzleData.cpp 独立编译单元（仅依赖 std::string/std::vector），QStandardItemModel 控制关卡启用态；12 新增测试用例，1 套件 | [CHANGELOG](../CHANGELOG.md) |
| 审计批次 5 | Lexer 边界（插值循环上限 + 列号一致性）+ Parser 错误恢复（类型注解预检 + classDecl try/catch + 错误数量上限 + 深度检查时机）+ Formatter 往返等价性（插值注释游标 + MemberAccess 括号 + NaN/Infinity + nullptr 防御，2 P1 + 13 P2 Bug，27 新增测试用例，14 套件） | [CHANGELOG](../CHANGELOG.md) |
| 性能基准 + 热点优化 | Release 模式基准建立（3 基准 × 3 后端）+ RegisterVM `reg()` 内联 + `callCache_` 内联缓存 | [CHANGELOG](../CHANGELOG.md) |
| 功能 6 + 功能 12 | 学习路径地图（5 阶段 21 活动 + JSON 进度持久化 + 智能推荐）+ 错误信息友好化增强（Levenshtein 拼写建议 + 7 种错误模式匹配 + ReplPanel presentation 层集成，27 新增测试用例，5 套件） | [CHANGELOG](../CHANGELOG.md) |
| 功能 13 | 代码模板 / Snippets 系统（7 模板 + Tab 展开 + 占位符导航 + Ctrl+T 列表，10 新增测试用例，2 套件） | [CHANGELOG](../CHANGELOG.md) |
| Top 3 高价值改进 | IR 优化回放 + 内存模型动画 + Bug 狩猎交互式修复（37 新增测试用例，5 套件） | [CHANGELOG](../CHANGELOG.md) |
| 审计批次 4 | 异常处理闭包 + cleanup wrap 残留 + RegisterVM 类定义检查 + VM 闭包崩溃（1 P0 + 4 P1 + 4 P2 Bug） | [CHANGELOG](../CHANGELOG.md) |
| REPL 修复 | BUG-REPL-AUDIT-9：REPL 首次 import 未设置模块加载器导致必败（P1） | [CHANGELOG](../CHANGELOG.md) |
| 调试器修复 | BUG-DBG-AUDIT-2：VM 模式断点 hitCount 永远为 0（P1） | [CHANGELOG](../CHANGELOG.md) |

### 2026-07-05 变更摘要

| 类别 | 内容 | 详情 |
|------|------|------|
| 审计批次 3 | finally 语法下游同步 + GC 漏洞 + 断点偏移 + 类型检查覆盖（1 P1 + 5 P2 Bug） | [CHANGELOG](../CHANGELOG.md) |
| 审计批次 2 | 三后端继承一致性 + REPL 防御性 + IR 优化统一 + GUI 线程安全（16 P2 Bug） | [CHANGELOG](../CHANGELOG.md) |
| 审计批次 1 | finally 语法 + 循环继承检测 + 闭包根标记 + super 运行时错误 + IR 优化防御 | [CHANGELOG](../CHANGELOG.md) |
| 教学面板（第二波） | MemoryModelPanel / IRTransformPanel / ProfileDashboardPanel | [架构文档](architecture.md#教学面板架构) |
| 教学面板（第三波） | CallStackPanel / VariableInspectorPanel / BytecodeTracePanel | [架构文档](architecture.md#教学面板架构) |
| 教学面板（第二档+第四档） | BreakpointConditionPanel / ProfileDashboard 指令计数 / 动画增强 | — |
| 教学面板（第三档） | ExceptionFlowPanel / ClosureInspectorPanel | — |
| 模块隔离 | ModuleTopLevelRenamer AST 重写，三路径统一 | [ADR-002](design-decisions/ADR-002-triple-backend.md) |
| VM 条件断点 | slot→name 映射，局部变量注入条件求值器 | [ADR-005](design-decisions/ADR-005-debug-consistency.md) |
| 跨模块审计修复 | 20 个 Bug（3 P1 + 17 P2） | — |
| 性能优化 | Unity Build / QtCharts 替换 / VM profiling / IR CSE+循环展开 | [ADR-003](design-decisions/ADR-003-ir-layer.md) |
| 工程优化 | Doxygen 文档 / i18n / const 正确性审计 | [贡献指南](../CONTRIBUTING.md) |

### 测试状态

- 全量测试：**1627 个测试注册**（191 测试套件，全部通过）
- 功能 11 REPL magic 命令套件：13/13 全部通过（MagicCommandsLibraryAudit/MagicCommandsHandlerAudit）
- 功能 3 Token 拼图套件：12/12 全部通过（TokenPuzzleLibraryAudit）
- 功能 4 AST 搭建玩具套件：17/17 全部通过（AstToyLibraryAudit）
- 功能 5 VM 栈沙盒套件：18/18 全部通过（SandboxLibraryAudit）
- 审计批次 5 套件：27/27 全部通过（AuditBatch5LexerInterpColumn/LexerCommentColumn/LexerInterpLoop/ParserTypeAnn/ParserClassRecovery/ParserErrorLimit/ParserDepthCheck/FmtInterpComment/FmtMemberCall/FmtClosingBraceComment/FmtUnaryUnknown/FmtNaN/FmtInterpNullptr/Consistency）
- 功能 6 学习路径套件：15/15 全部通过（LearningPathDataAudit/LearningPathProgressAudit/LearningPathPrereqAudit）
- 功能 12 错误提示引擎套件：12/12 全部通过（ErrorHintEngineSpelling/ErrorHintEnginePatterns）
- 功能 13 代码模板套件：10/10 全部通过（CodeSnippetLibraryAudit/CodeSnippetEngineAudit）
- 教学面板套件：92/92 全部通过
- 审计批次 1 套件：21/21 全部通过（AuditBatch1Finally/Inherit/CloseUpvalue/GcRoot/Super/OptIR）
- 审计批次 2 套件：16/16 全部通过（AuditBatch2Inherit/AuditBatch2IRopt）
- 审计批次 3 套件：16/16 全部通过（AuditBatch3FormatterFinally/ModuleIsolationFinally/TypeCheckerCoverage/GcLeak）
- 审计批次 4 套件：21/21 全部通过（AuditBatch4CatchUpvalue/CleanupTryDepth/RegVMClassCheck/FormatterAudit/VmClosureCrash/Consistency）
- Top 3 改进方向套件：37/37 全部通过（IROptReplayAudit/MemoryAnimLibraryPhases/MemoryAnimLibraryTypes/MemoryAnimLibraryConsistency/BugHuntVariantAudit）
- 构建：MSVC + Qt 6.10.3 msvc2022_64 + Ninja

## 项目结构

```
ide/
├── app/              # 应用层（IdeController, DebugCoordinator, main.cpp）
├── ast/              # AST 节点定义 + ModuleIsolation
├── compiler/         # 编译器（Compiler, Bytecode, IR, VM, RegisterVM）
├── gui/              # GUI 面板与组件（教学面板、编辑器、调试器）
├── interpreter/      # 树遍历解释器 + NaN-boxing Value
├── lexer/            # 词法分析器
├── parser/           # 语法分析器
├── samples/mini/     # MiniLang 示例程序（15 个 .mini 文件）
├── tests/            # 单元测试与审计测试（1614 用例）
├── cmake/            # CMake 模块
├── scripts/          # 构建脚本（build_ide.bat, run_tests.bat）
└── CMakeLists.txt    # 顶层构建配置
```
