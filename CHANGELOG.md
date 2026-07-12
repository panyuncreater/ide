# Changelog

本文件记录 MiniLang IDE 的开发演进历史，包括性能优化、正确性修复与工程基础设施改进。所有条目均通过全量单元测试验证。历史版本归档至 [docs/changelog/archive/](docs/changelog/archive/)。

## 2026-07-12 · 第九十一轮：CI 覆盖率 Job 修复（工程基础设施 × 2）

### 概述

CI #36 中两个覆盖率 Job 失败。根因均为环境/命令配置问题，非代码缺陷。

### 问题 1：Windows Coverage — OpenCppCoverage.exe 未识别（CI × 1）

**位置**：`.github/workflows/ci.yml` coverage job 步骤「安装 OpenCppCoverage」

**根因**：`choco install opencppcoverage` 修改的是 Machine PATH，但当前 PowerShell 会话的 PATH 不会自动刷新。后续「运行覆盖率分析」步骤在新的 shell 中执行时仍报 `The term 'OpenCppCoverage.exe' is not recognized`。这是 GitHub Actions Windows runner 上 choco 安装工具的常见陷阱。

**修复**：安装步骤后通过 `Add-Content $env:GITHUB_PATH` 把 `OpenCppCoverage.exe` 所在目录写入 `$GITHUB_PATH`（GitHub Actions 官方推荐的跨步骤 PATH 注入机制）。用 `Get-ChildItem` 在 `C:\Program Files\OpenCppCoverage` / `C:\Program Files (x86)\OpenCppCoverage` 下递归搜索 exe，避免硬编码具体子路径；找不到则 `exit 1` 显式失败而非静默继续。

### 问题 2：Linux Coverage — ctest --preset 找不到 CMakePresets.json（CI × 1）

**位置**：`.github/workflows/ci.yml` coverage-linux job 步骤「运行测试」

**根因**：步骤设置了 `working-directory: out/build/linux-coverage`（构建目录），但命令为 `ctest --preset linux-gcc-coverage`。`--preset` 要求 `CMakePresets.json` 在当前工作目录，而该文件只存在于仓库根目录 → `Could not read presets ... File not found`。对比 build-test job 的 Linux 测试步骤用的是 `ctest -j 4`（无 `--preset`，在构建目录直接运行），coverage job 此处写法不一致。

**修复**：去掉 `--preset linux-gcc-coverage`，改为 `ctest -j 4 --output-on-failure --verbose`，与 build-test job 保持一致。`working-directory` 已是构建目录，ctest 直接读取其中已生成的 `CTestTestfile.cmake`。

### 验证

仅 CI 配置变更，不涉及 C++ 源码；本地构建与测试不受影响。需在 CI 上观察两个覆盖率 Job 转为绿色。

### 涉及文件

- `.github/workflows/ci.yml`：coverage job 安装 OpenCppCoverage 步骤改用 `GITHUB_PATH` 注入；coverage-linux job 测试步骤去掉 `--preset`

## 2026-07-11 · 第九十轮：CI clang-format 风格检查修复（工程基础设施 × 1）

### 概述

GitHub Actions CI 的 `Code Style (clang-format)` Job 失败（退出码 123），原因是 `lexer`/`parser`/`ast`/`interpreter`/`compiler`/`debug`/`formatter`/`gui`/`common`/`app` 共 10 个目录下 32 个 C/C++ 源文件不符合项目 `.clang-format`（LLVM 基础，4 空格缩进，120 列上限）规范。本轮使用与 CI 完全一致的 `clang-format==22.1.5` 对全部 174 个源文件执行 `clang-format -i` 就地格式化，使 `clang-format --dry-run --Werror` 零违规通过。

### 主要格式化差异

- **switch case 标签展开**：`case X: return "Y";` 单行 → `case X:` 换行 + `return "Y";`（LLVM 默认 `AllowShortCaseLabelsOnASingleLine: false`），涉及 `MagicCommands.cpp` 等大 switch 的 token 类型名映射
- **长布尔表达式折行对齐**：`while`/`if` 中多条件 `||`/`&&` 按运算符优先级重新折行对齐
- **指针/引用对齐**：`Type *p` → `Type* p`（`PointerAlignment: Left`）
- **include 排序**：`SortIncludes: CaseSensitive` 重新排序
- **命名空间注释**：`FixNamespaceComments: true` 自动补全 `// namespace xxx`

### 验证

- 本地 `clang-format --dry-run --Werror` 对 174 个文件零违规（退出码 0）
- MSVC 19.51 + Qt 6.10.3 + Ninja 全量构建通过（78/78 目标）
- 全量 1773/1773 测试通过，零回归

### 涉及文件

共 32 个源文件被格式化（app ×5、compiler ×8、debug ×1、gui ×16、interpreter ×1、其余目录无违规）。CI 配置无需修改。

## 2026-07-11 · 第八十九轮：Bug 狩猎面板关闭崩溃根因修复（P0 × 1 + P1 × 1）

### 概述

用户反馈在使用 Bug 狩猎模式时关闭程序抛出异常（读取访问权限冲突）。经深入排查发现根因是 `showTeachingPanel` 中自动触发 GuidedTour 的 `QTimer::singleShot(0, this, ...)` 在 closeEvent 中未被取消，导致关闭过程中创建"孤儿" GuidedTour 访问正在析构的 widget → UAF。此问题影响全部 5 个带自动引导的面板（bytecode-trace / call-stack / variable-inspector / breakpoint-condition / bug-hunt），BugHuntPanel 因其复杂 widget 树最易复现。

### 问题 1：closeEvent 未停止 Ide 直接子 QTimer（P0 × 1）

**位置**：`app/ide.cpp` closeEvent + `showTeachingPanel` line 1655

**根因**：`showTeachingPanel` 在首次访问 5 个自动引导面板时通过 `QTimer::singleShot(0, this, lambda)` 延迟启动引导。该 singleShot 创建的临时 QTimer 是 Ide 的直接子对象。closeEvent 的 `stopChildAnimations` 只扫描 `centerStack_`/`bottomStack_`/`rightStack_`/`dockManager_` 的子对象，**不覆盖 Ide 直接子 QTimer**；`QCoreApplication::removePostedEvents(this)` 只清除 posted events 队列，**不取消定时器事件**。当 `maybeSave()` 模态对话框或 `processEvents` 的事件循环派发该定时器时，`onPanelGuidedTourRequested` 被调用（无 `closing_` 守卫），在关闭过程中创建 GuidedTour，其 `showStep` 修改 widget 样式表、创建 `bubble_` 子 widget、访问正在清理的目标 widget → UAF。

**崩溃路径**：
1. 用户打开 Bug 狩猎面板 → `showTeachingPanel("bug-hunt")` 投递 `singleShot(0, this, ...)`
2. 用户立即关闭窗口 → closeEvent 开始，`closing_ = true`
3. `stopChildAnimations` 不扫描 Ide 直接子 QTimer → 定时器存活
4. `activePanelTours_` 清理（此时集合为空，tour 尚未创建）
5. `removePostedEvents(this)` → 不取消定时器事件
6. `maybeSave()` 模态对话框事件循环 → **定时器触发** → `onPanelGuidedTourRequested` 创建孤儿 tour
7. tour 的 `showStep` 访问正在清理的 BugHuntPanel 子 widget → UAF

**修复**（三层防御）：
1. `onPanelGuidedTourRequested` 入口添加 `if (closing_) return;` 守卫（P0 根因修复）
2. closeEvent 中新增 `findChildren<QTimer*>()` + `findChildren<QPropertyAnimation*>()` 对 `this` 递归停止所有活跃定时器/动画（P0 纵深防御，覆盖 Ide 全部直接子 singleShot 定时器，包括 restoreState/dock resize 延迟调用）
3. `GuidedTour::showStep` 中将裸 `QWidget*` 改为 `QPointer<QWidget>` 捕获（P1 纵深防御，防止目标 widget 析构后 singleShot lambda 访问悬垂指针）

### 测试

全量 1773/1773 测试通过，零回归。MSVC 19.51 + Qt 6.10.3 + Ninja 构建通过。

### 涉及文件

- `app/ide.cpp`：closeEvent 新增 Ide 直接子 QTimer/QPropertyAnimation 捕获逻辑；`onPanelGuidedTourRequested` 添加 `closing_` 守卫
- `gui/GuidedTour.cpp`：`showStep` 中 `QWidget* targetWidget` → `QPointer<QWidget> targetWidget`

## 2026-07-10 · 第八十八轮：交付前 UI 跨机器一致性彻底修复（UI × 53，共 3 类）

### 概述

交付前对整个 IDE 进行系统性 UI 跨机器一致性审查与修复，覆盖字体回退链、颜色对比度、构建配置三大类。共修复 53 处问题，确保在任何 Windows 机器（中文/英文/精简版/Server）上 UI 文字清晰可读、字体正确渲染。

### 问题 1：字体回退链不完整（P0 × 3 + P1 × 44，共 47 处）

根因：R75 虽建立了 `GuiTextUtils.h` 字体工厂，但仅接入部分文件，仍有 47 处 QSS/HTML/QFont 硬编码短回退链或零回退。

| # | 严重性 | 位置 | 修复 |
|---|--------|------|------|
| P0.1-3 | P0 | `WelcomeWizard.cpp:181` `TokenPuzzlePanel.cpp:144` `VmStackSandboxPanel.cpp:1074` | `QFont("Consolas")` 单族硬编码 → `GuiTextUtils::monospaceFont(N)` 完整 8 族回退链 |
| P1.1-2 | P1 | `BreakpointConditionPanel.cpp:369,389` `IRTransformPanel.cpp:565` | QSS `font-family:Consolas;` 零回退 → 完整 8 族 + monospace 通用族 |
| P1.3-44 | P1 | 17 个文件共 42 处 QSS/HTML `font-family` 短链（2-3 族）→ 完整 8 族回退链 | 见下表 |

**QSS 字符串工厂**：在 `GuiTextUtils.h` 新增 `monoFontFamilyQss()` / `uiFontFamilyQss()` 返回拼接好的完整回退链字符串，作为 QSS/HTML font-family 的单一真相源。

**等宽完整回退链**（8 族 + monospace）：
`Cascadia Code → Cascadia Mono → Consolas → JetBrains Mono → Source Code Pro → Menlo → DejaVu Sans Mono → Courier New → monospace`

**UI 完整回退链**（8 族 + sans-serif）：
`Microsoft YaHei → PingFang SC → Noto Sans CJK SC → Source Han Sans SC → Segoe UI → SF Pro Text → Arial Unicode MS → Arial → sans-serif`

**涉及文件**：`app/styles.qss`(9处) `app/ide.cpp`(4处) `gui/CodeEditor.cpp`(2处) `gui/WelcomeWizard.cpp`(4处) `gui/VmStackSandboxPanel.cpp`(9处) `gui/CodeJourneyInfoPanel.cpp`(1处) `gui/CallStackPanel.cpp`(1处) `gui/VariableInspectorPanel.cpp`(1处) `gui/IrViewer.cpp`(1处) `gui/IRTransformPanel.cpp`(2处) `gui/PipelineViewer.cpp`(3处) `gui/MarkdownRenderer.cpp`(4处) `gui/BreakpointConditionPanel.cpp`(2处) `gui/TeachingPanelHeader.cpp`(2处)

### 问题 2：颜色对比度不足（P0 × 4 + P1 × 6，共 10 处）

根因：R74 背景色从 Solarized 米黄改为白/浅灰时，遗漏了部分浅色文本，导致对比度不足甚至完全不可见。

| # | 严重性 | 位置 | 旧色 | 新色 | 旧对比度 | 新对比度 |
|---|--------|------|------|------|---------|---------|
| P0.4 | P0 | `AstBuilderToyPanel.cpp:110` locked 芯片 | `#AAA` on `#EDEDED` | `#6E6E6E` | 1.98:1 | 5.07:1 |
| P0.5 | P0 | `VmStackSandboxPanel.cpp:80` locked 芯片 | `#AAA` on `#EDEDED` | `#6E6E6E` | 1.98:1 | 5.07:1 |
| P0.6 | P0 | `TokenPuzzlePanel.cpp:218` locked 芯片（二轮发现） | `#AAA` on `#EDEDED` | `#6E6E6E` | 1.98:1 | 5.07:1 |
| P0.7 | P0 | `MarkdownRenderer.cpp:374` 代码块语言标签 | `#999` on 白底 | `#6E6E6E` | 2.85:1 | 5.07:1 |
| P1.1 | P1 | `BytecodeTracePanel.cpp:326` 占位文本 | `#888` | `#6E6E6E` | 3.55:1 | 5.07:1 |
| P1.2 | P1 | `IrViewer.cpp:262` 占位文本 | `#888` | `#6E6E6E` | 3.55:1 | 5.07:1 |
| P1.3 | P1 | `LabManualPanel.cpp:300` 占位文本 | `#888` | `#6E6E6E` | 3.55:1 | 5.07:1 |
| P1.4 | P1 | `LearningPathPanel.cpp:394,423,474` 未解锁标题/时间/按钮（二轮发现） | `#999`/`#888` | `#6E6E6E` | 2.85~3.55:1 | 5.07:1 |
| P1.5 | P1 | `ExceptionFlowPanel.cpp:371,518,569` 箭头符号/说明文字（二轮发现） | `#7F8C8D` | `#6E6E6E` | 3.28:1 | 5.07:1 |
| P1.6 | P1 | `CodeJourneyInfoPanel.cpp:128` 副标题 | `#666` | 保留（4.76:1 合格） | — | — |

### 问题 3：CMakePresets toolset 错误（P3 × 1）

| # | 严重性 | 位置 | 修复 |
|---|--------|------|------|
| P3.1 | P3 | `CMakePresets.json:73` | `windows-msvc-vs` preset 的 `toolset: "v145"` → `"v143"`（VS2022 工具集为 v143，v145 不存在） |

### 审查未发现问题的项（通过）

- **资源完整性**：`app/ide.qrc` 37 个资源文件全部存在（15 对 QFluent SVG + 7 品牌 Logo）
- **构建脚本**：`scripts/_common.bat` 三策略 Qt 检测（QTDIR/PATH/常见路径）+ vswhere，跨机器兼容性优秀
- **windeployqt 部署**：release 目录 Qt6Core/Gui/Widgets/Svg/Xml.dll + QFluent.dll + platforms/qwindows.dll + imageformats 插件全部就位
- **高 DPI**：`main.cpp` 已设 `HighDpiScaleFactorRoundingPolicy::PassThrough`，Qt6 默认启用高 DPI 缩放
- **.gitignore**：覆盖全面，无遗漏

### 验证

MSVC 19.51 + Qt 6.11.1 + Ninja debug 构建通过，全量 **1773/1773 测试通过**，零回归。

### 修改文件清单（共 19 个源文件 + 3 文档）

- **字体工厂**：`gui/GuiTextUtils.h`（新增 `monoFontFamilyQss()`/`uiFontFamilyQss()`）
- **P0 字体硬编码**：`gui/WelcomeWizard.cpp` `gui/TokenPuzzlePanel.cpp` `gui/VmStackSandboxPanel.cpp`
- **QSS 回退链**：`app/styles.qss` `app/ide.cpp` `gui/CodeEditor.cpp` `gui/WelcomeWizard.cpp` `gui/VmStackSandboxPanel.cpp` `gui/CodeJourneyInfoPanel.cpp` `gui/CallStackPanel.cpp` `gui/VariableInspectorPanel.cpp` `gui/IrViewer.cpp` `gui/IRTransformPanel.cpp` `gui/PipelineViewer.cpp` `gui/MarkdownRenderer.cpp` `gui/BreakpointConditionPanel.cpp` `gui/TeachingPanelHeader.cpp`
- **对比度**：`gui/AstBuilderToyPanel.cpp` `gui/VmStackSandboxPanel.cpp` `gui/MarkdownRenderer.cpp` `gui/BytecodeTracePanel.cpp` `gui/IrViewer.cpp` `gui/LabManualPanel.cpp`
- **构建配置**：`CMakePresets.json`
- **文档**：`CHANGELOG.md` `docs/development.md`

## 2026-07-10 · 第八十七轮：背景色回退中性白（UI × 1，共 1 项）

### 概述

将 IDE 全部背景色从 Solarized 米黄配色（`#FDF6E3` base3 / `#EEE8D5` base2）回退为中性白/浅灰（`#FFFFFF` / `#F5F5F5`），解决跨机器渲染不一致问题。Solarized 米黄色在不同显示器/ICC 配置下色差明显，交付前统一回退为通用白底，保证演示与答辩环境视觉一致。语义强调色（blue `#268BD2` / green `#859900` / yellow `#B58900` / red `#DC322F`）保留原 Solarized 配色，确保在白底下可读性与语义辨识度。

### 改动范围

| 层 | 文件 | 变更 |
|----|------|------|
| 主题色板 | `gui/TeachingTheme.h` | 14 色 IDE 变量 + 文本色 + surface/border 全部从 Solarized 回退中性：`ideBgMain` `#FDF6E3→#FFFFFF`、`ideBgPanel/Sidebar` `#EEE8D5→#F5F5F5`、`ideBorder` `#93A1A1→#E0E0E0`、`ideFgPrimary` `#002B36→#1E1E1E`、`ideFgSecondary` `#657B83→#8C8C8C`、`ideSelectedBg` `#EEE8D5→#CCE4F7`（浅蓝选中）、`ideEditorBg→#FFFFFF`、`ideLineNumBg→#F5F5F5` |
| 主窗口 | `app/ide.cpp` | 输出面板 Base 色 → 白、标题栏渐变 `#FDF6E3→#EEE8D5` 改为 `#FFFFFF→#F5F5F5`、VM 面板浅色路径全部回退中性、字节码/Token 高亮文本色（默认标识符/注释/符号）对齐中性灰、ADS QSS Highlight=面板色注释更新 |
| 编辑器 | `gui/CodeEditor.cpp` | 浅色模式 Base → 纯白、行号区 → 浅灰、光标行高亮 → 浅灰、执行行保留柔黄 `#FFF4D4`、补全弹窗/断点条件对话框色板回退中性 |
| 教学面板 | `gui/PipelineViewer.cpp` `IrViewer.cpp` `AstViewer.cpp` `TeachingTreePanel.cpp` `AstBuilderToyPanel.cpp` `BugHuntPanel.cpp` `CodeJourneyInfoPanel.cpp` `LabManualPanel.cpp` `BreakpointConditionPanel.cpp` `CallStackPanel.cpp` `TeachingPanelHeader.cpp` `TokenPuzzlePanel.cpp` `VariableInspectorPanel.cpp` `VmStackSandboxPanel.cpp` `MarkdownRenderer.cpp` | 全部硬编码 Solarized 背景米黄/边框/文本色替换为中性白/浅灰/深灰；阶段配色与语义强调色保留 |
| 样式参考 | `app/styles.qss` | 头部注释更新为中性白主题（本文件为设计参考，运行时不加载） |

### 保留未改

- `gui/SyntaxHighlighter.cpp` 语法 token 前景色（keyword/string/number 等 Solarized 配色）：属于语法高亮方案，在白底下可读性良好，保持不变
- `CodeEditor.cpp` 深色主题分支的 `0xfd,0xf6,0xe3` HighlightedText：深色主题已禁用（强制 LIGHT），不影响运行
- 语义强调色（blue/green/yellow/red/purple/cyan）：保留 Solarized 配色

### 验证

MSVC 19.51 + Qt 6.10.3 + Ninja release 增量构建通过，全量 1763/1763 测试通过，零回归。

### 修改文件清单

- `gui/TeachingTheme.h`、`app/ide.cpp`、`gui/CodeEditor.cpp`、`app/styles.qss`
- `gui/` 下 15 个面板文件（见上表）
- `CHANGELOG.md`、`docs/development.md`

## 2026-07-10 · 第八十六轮：条件断点表达式自动补充分号（P0 × 1，共 1 项）

### 概述

修复条件断点完全不触发的问题。用户在断点条件对话框中输入 `i%2==1`（不带分号），但 `Parser::parse()` 内部的 `expressionStatement()` 调用 `consume(TK_SEMICOLON)` 强制要求分号，导致解析失败（`getDiagnostics().hasErrors()` 为 true），条件求值器返回 false，断点**永远不触发**。此 Bug 影响 Interpreter 和 VM/RegisterVM 全部三条路径。

### 问题与修复对应表

| # | 严重性 | 类型 | 位置 | 修复内容 |
|---|--------|------|------|----------|
| P0.1 | P0 | 语义错误（功能完全失效） | `app/DebugCoordinator.cpp` `setConditionEvaluator` lambda / `app/IdeController.cpp` `setConditionEvaluator` lambda | **条件断点永不触发**。根因：`Parser::expressionStatement()` (L1246-1250) 调用 `consume(TK_SEMICOLON)`，用户输入的条件表达式如 `i%2==1` 不含分号 → 解析失败 → `getDiagnostics().hasErrors()` 为 true → 条件求值器返回 false → 断点永远不暂停。修复：在两处条件求值器 lambda 中，解析前检查条件字符串末尾是否为 `;`，不是则自动追加。VM 路径同时修复 LRU 缓存 key 使用补充分号后的 `condExpr`，避免 `i%2==1` 和 `i%2==1;` 产生不同缓存条目。 |

### 修改文件清单

- `app/DebugCoordinator.cpp`：Interpreter 路径条件求值器 lambda 中自动补充分号
- `app/IdeController.cpp`：VM 路径条件求值器 lambda 中自动补充分号，LRU 缓存 key 同步使用 `condExpr`
- `CHANGELOG.md`、`docs/development.md`

**历史变更**：更早的开发记录（第六十九轮至第八十五轮）已归档至 [2026-07-10-rounds-69-85.md](docs/changelog/archive/2026-07-10-rounds-69-85.md)，第六十八轮及以前见 [docs/changelog/archive/](docs/changelog/archive/)。
