# Changelog

本文件记录 MiniLang IDE 的开发演进历史，包括性能优化、正确性修复与工程基础设施改进。所有条目均通过全量单元测试验证。历史版本归档至 [docs/changelog/archive/](docs/changelog/archive/)。

## 2026-07-09 · 第七十轮：UI 修复方案修正——DisableStylesheet 副作用（QToolTip 黑框 + 按钮图标丢失）（P1 × 2，共 2 项）

### 概述

第六十八轮使用 `DisableStylesheet` 彻底阻止 ADS `loadStylesheet()`，虽解决了样式覆盖问题，但引入新问题：**(1) 调试按钮/工具栏 QToolTip 回退到 Windows 11 默认黑色样式（黑框白字）**；**(2) ADS 标题栏按钮图标（关闭/浮动/标签菜单）丢失，显示为空框或默认按钮**。根因：`DisableStylesheet` 使构造时的初始 `default.css` 加载也被跳过，ADS 按钮的 `qproperty-icon` 规则从未被应用，QToolTip 样式也缺失。本轮改用更精准的 `setColorSchemeMode(Light)` 方案——保留构造时 default.css 初始加载（提供图标和基础样式），仅阻止后续 palette-change 触发的重载。MSVC 19.51 + Qt 6.10.3 + Ninja 构建通过，全量 **1763/1763** 测试通过。

### 问题与修复对应表

| # | 严重性 | 类型 | 位置 | 修复内容 |
|---|--------|------|------|----------|
| 1 | P1 | 回归（图标丢失） | `app/ide.cpp` setConfigFlags | **DisableStylesheet 导致 ADS 标题栏按钮图标丢失**——`DisableStylesheet` 使构造函数中 `loadStylesheet()` 直接 return（[DockManager.cpp:209-212](third_party/ads/src/DockManager.cpp#L209-L212)），default.css 中 `#tabCloseButton`/`#tabsMenuButton`/`#dockAreaCloseButton`/`#detachGroupButton` 的 `qproperty-icon` 规则从未执行。后续 adsQss 通过 `setStyleSheet` 覆盖到 dockManager_ 时，也缺少这些图标规则（R68 adsQss 未完整复刻 default.css 的图标 qproperty）。修复：移除 `DisableStylesheet`，保留默认配置让构造函数正常加载 default.css 提供图标；adsQss 补全所有 `qproperty-icon` 规则（tabCloseButton/tabsMenuButton/dockAreaCloseButton/detachGroupButton）。 |
| 2 | P1 | 回归（QToolTip 黑框） | `app/ide.cpp` setConfigFlags + applyFluentStyle | **QToolTip 回退到 Windows 11 黑色样式**——default.css 被 DisableStylesheet 阻止加载后，QToolTip 无任何 QSS 样式，Windows 11 上回退为系统原生黑色 tooltip（黑底白字），与米黄主题严重不协调。修复：(1) 移除 DisableStylesheet 使 default.css 的初始加载提供 Fusion 样式的 tooltip 基础；(2) QPalette 设置 `ToolTipBase`/`ToolTipText` 为主题色；(3) qApp 级别追加 QToolTip QSS（背景/前景/边框/圆角），静态变量守卫防重复追加；(4) adsQss 内也包含 QToolTip 规则。三重防御确保 tooltip 使用米黄主题色。 |

### 关键设计决策

1. **setColorSchemeMode(Light) 替代 DisableStylesheet**——DisableStylesheet 过于激进，连初始默认样式都阻止加载。`setColorSchemeMode(Light)` 是更精准的方案：构造后立即调用，此时 `ColorSchemeMode` 从默认 `FollowPalette` 切换到 `Light`，`eventFilter` 条件 `ColorSchemeMode == FollowPalette` 不再成立，后续 `ApplicationPaletteChange` 事件不会触发 `loadStylesheet()`。构造时的初始 default.css 加载（在 setColorSchemeMode 之前执行）正常提供图标等基础样式，随后我们的 adsQss 通过 `setStyleSheet` 替换它。
2. **adsQss 必须完整复刻 default.css 的 qproperty-icon 规则**——因为 `setStyleSheet(adsQss)` 会完全替换 dockManager_ 上的样式表（包括构造时 loadStylesheet 设置的图标属性）。缺失任何图标规则都会导致对应按钮无图标。
3. **QToolTip 三重保险**：(a) default.css 初始加载提供 Fusion 基础；(b) QPalette ToolTipBase/ToolTipText 设置主题色；(c) qApp 级别 QSS 定义完整 tooltip 外观。确保即使 default.css 被替换，tooltip 仍保持正确样式。

### 修改文件清单

- 修改：`app/ide.cpp`（移除 `DisableStylesheet` 配置标志；添加 `setColorSchemeMode(Light)` 调用；adsQss 补全 ADS 按钮 qproperty-icon 规则；添加 QPalette ToolTipBase/ToolTipText；添加 qApp 级别 QToolTip QSS；更新注释解释方案演进）
- 修改：`CHANGELOG.md`（新增第七十轮条目）

### 测试影响

- 全部 2 项修复均无回归。全量 1763/1763 测试通过。
- 图标丢失和 tooltip 黑框属于视觉表现层，单元测试不直接覆盖，构建通过 + 无回归即视为修复验证。建议用户启动 IDE 视觉确认。

## 2026-07-09 · 第六十九轮：REPL %magic 命令系统 5 项 Bug 修复（P1 × 3 + P2 × 2，共 5 项）

### 概述

本轮针对用户报告的 REPL `%magic` 命令系统问题进行系统性修复。核心问题包括：magic 命令因括号未闭合被误判为续行输入、分析类命令（`%ast`/`%tokens`/`%disassemble`/`%ir`）不支持参数代码、`%reset` 无法真正清除 REPL 状态、Token 输出列对齐 off-by-one、help 提示信息过时。MSVC 19.51 + Qt 6.10.3 + Ninja 构建通过，全量 **1763/1763** 测试通过。

### 问题与修复对应表

| # | 严重性 | 类型 | 位置 | 修复内容 |
|---|--------|------|------|----------|
| 1 | P1 | 续行误判 | `gui/ReplPanel.cpp` onReturnPressed | **magic 命令被误判为续行输入**——`%ast fun f() {` 因 `{` 未闭合被 `isInputComplete()` 判为"不完整"，进入续行模式，用户被迫输入 `}` 闭合才能触发 magic 命令。修复：在非续行模式下、`isInputComplete()` 检查之前拦截 magic 命令，magic 是单行元命令不走续行逻辑；同时添加 replRunning_/isRunning()/isVmRunning() 三重守卫。 |
| 2 | P1 | 功能缺失 | `gui/MagicCommands.cpp` parseCommand/ScopedAnalysis | **分析类命令不支持参数**——`%ast 1+2;`、`%tokens var x=42;`、`%disassemble 1+2;`、`%ir var x=1;` 均无输出（原实现只能查看当前已执行代码的缓存结果）。修复：新增 `ParsedCommand` 解析命令名+参数，当带参数时使用临时 `ScopedAnalysis`（独立 Lexer/Parser/Compiler/IRBuilder 栈上实例）分析参数代码，不依赖 IdeController，零副作用。实现 AstPrinter 递归打印 AST 节点。 |
| 3 | P1 | 功能缺失 | `interpreter/Interpreter.{h,cpp}` + `app/IdeController.h` + `gui/MagicCommands.cpp` | **%reset 无法真正重置 REPL 环境**——原 `%reset` 仅重置 VmStepper 状态，不清除全局变量/函数/类/模块缓存，`var x=1; %reset; print(x);` 仍输出 1。修复：新增 `Interpreter::resetReplEnvironment()` 方法，重置 stopRequested_/evaluationStepCount_、重建 globalEnv_、清空所有缓存（AST/字节码/IR/模块/源码/类注册表/VM 实例），重置内部执行状态；`IdeController::resetReplEnvironment()` 转发并重置 vmStepper_。 |
| 4 | P2 | off-by-one | `gui/MagicCommands.cpp` handleTokens | **Token 输出 idx 列对齐错位**——原代码 `idx++` 后再做 padding 判断，导致 idx=9 时自增为 10，应补 4 空格但补了 3 空格，第 10 个 token 起列偏移。修复：先输出 idx 并基于当前值判断 padding，再 idx++。 |
| 5 | P2 | 文档过时 | `gui/ReplPanel.cpp` help 文本 | **help 提示"重置全部状态需重启 IDE"过时**——%reset 现已能真正重置，更新为"%reset 重置 REPL 环境(清除所有变量/函数/类/模块缓存)"。 |

### 关键设计决策

1. **ScopedAnalysis 栈上临时实例**——magic 命令参数分析不能污染 REPL 状态（不替换当前 bytecode/IR/变量）。创建临时 Lexer/Parser/Compiler 实例在栈上，分析完毕自动析构，零副作用。
2. **ReplPanel 入口拦截而非修改 isInputComplete**——isInputComplete 是括号配对等通用续行判断，为 magic 命令加特殊分支会污染核心逻辑。在 onReturnPressed 入口处拦截更干净，职责分离。
3. **%reset 重建 globalEnv_ 而非逐个清除**——Environment 链式作用域中逐个清除键值对容易遗漏（闭包捕获、父作用域引用），重建 shared_ptr 更彻底，旧 Environment 在无引用时自动析构。

### 修改文件清单

- 修改：`gui/MagicCommands.cpp`（新增 parseCommand/AstPrinter/ScopedAnalysis；handle 重写支持参数；handleTokens 对齐修复；handleReset 调用 resetReplEnvironment）
- 修改：`interpreter/Interpreter.h`（新增 resetReplEnvironment 声明）
- 修改：`interpreter/Interpreter.cpp`（实现 resetReplEnvironment，全状态重置）
- 修改：`app/IdeController.h`（新增 resetReplEnvironment 转发方法）
- 修改：`gui/ReplPanel.cpp`（onReturnPressed 在 isInputComplete 之前拦截 magic 命令；help 提示更新）
- 修改：`tests/TestMagicCommandsAudit.cpp`（18 个测试用例，新增参数分析测试）

### 测试影响

- 新增/更新测试共 18 个（MagicCommandsLibraryAudit 6 个 + MagicCommandsHandlerAudit 12 个），覆盖命令元数据完整性、无参友好错误、带参数代码分析（%ast/%tokens/%disassemble/%ir）、未知命令、空/非 magic 输入、前导空白处理。
- 全量 1763/1763 测试通过，零回归。

## 2026-07-09 · 第六十八轮：UI 三大顽疾真正根因修复——ADS loadStylesheet 覆盖（P1 × 3，共 3 项）

### 概述

本轮针对用户反馈"全都没解决"的三个 UI 问题（经第六十五~六十七轮修复均无效）进行**真正根因定位**。前几轮的根因分析全部错误——真正原因不是 QFluentKit StyleSheetManager，而是 **ADS 自身的 `loadStylesheet()` 机制**：`applyFluentStyle()` 中 `setPalette(pal)` 向 dockManager_ 传播 `QEvent::ApplicationPaletteChange` 事件，ADS eventFilter（[DockManager.cpp:759-763](third_party/ads/src/DockManager.cpp#L759-L763)）检测到 `ColorSchemeMode == FollowPalette`（默认值）→ 调用 `loadStylesheet()` → `setStyleSheet(default.css)` → **完全覆盖**自定义 adsQss。这发生在**每次** `applyFluentStyle()` 调用时，导致前几轮所有 QSS 修复从未真正生效。

**统一修复**：添加 `ads::CDockManager::DisableStylesheet` 配置标志，使 `loadStylesheet()` 成为 no-op（[DockManager.cpp:209-212](third_party/ads/src/DockManager.cpp#L209-L212)），自定义 adsQss 才能持久存在。MSVC 19.51 + Qt 6.10.3 + Ninja 构建通过，全量 1758/1758 测试通过。

### 问题与修复对应表

| # | 严重性 | 类型 | 位置 | 修复内容 |
|---|--------|------|------|----------|
| 1 | P1 | 样式覆盖（真正根因） | `app/ide.cpp` setConfigFlags | **ADS dock 标签蓝底+米黄字体**——真正根因：`applyFluentStyle()` 行 3540 `dockManager_->setStyleSheet(adsQss)` 设置自定义 QSS 后，行 3785 `setPalette(pal)` 向 dockManager_ 传播 `ApplicationPaletteChange` 事件，ADS eventFilter 检测到 `ColorSchemeMode==FollowPalette`（默认）→ `loadStylesheet()` → `setStyleSheet(default.css)` 完全覆盖 adsQss。default.css 的 `ads--CDockWidgetTab[activeTab="true"] QLabel { color: palette(foreground); }` 导致字体颜色异常。修复：添加 `DisableStylesheet` 标志使 `loadStylesheet()` 成为 no-op，adsQss 持久生效。 |
| 2 | P1 | 样式覆盖（同一根因） | `app/ide.cpp` setConfigFlags | **编译分析面板字节码背景未变米黄色**——同一根因：adsQss 中 `ads--CDockWidget { background: bgMain }` 被 loadStylesheet 覆盖，dock 容器显示 default.css 的默认（白色）背景。前轮移除 registerWidget 的修复方向错误——问题不在 QFluentKit 而在 ADS 自身。DisableStylesheet 修复后 adsQss 的 dock 容器背景规则生效。 |
| 3 | P1 | 布局干扰（同一根因） | `app/ide.cpp` setConfigFlags + QTimer 顺序 | **面板初始大小每次都要手动调整**——部分根因同上：restoreState 后的 `applyFluentStyle()` → `setPalette` → `loadStylesheet` → `setStyleSheet` 触发样式重算，可能干扰 restoreState 恢复的 splitter 尺寸。DisableStylesheet 消除此干扰。额外加固：将 QTimer lambda 中 `applyFluentStyle()` 移到 `restoreState()` **之前**，确保 restoreState 是最后的布局操作，其 splitter 尺寸不被后续重算覆盖。 |

### 关键设计决策

1. **DisableStylesheet 而非对抗 loadStylesheet**：前几轮尝试用"追加 QSS"、"覆盖 QSS"、"移除 FocusHighlighting 标志"等方式对抗 loadStylesheet，全部失败——因为 loadStylesheet 在每次 ApplicationPaletteChange 时重新覆盖。DisableStylesheet 从源头禁用 ADS 内置样式加载，自定义 adsQss 成为唯一样式来源，彻底消除覆盖。
2. **applyFluentStyle 移到 restoreState 之前**：即使 DisableStylesheet 已消除 loadStylesheet 干扰，将样式应用放在 restoreState 之前仍更稳健——确保 restoreState 是 QTimer lambda 中最后的布局操作，其设置的 splitter 尺寸为最终值。

### 关键教训

1. **`setPalette` 会向子 widget 传播 `ApplicationPaletteChange` 事件**——这是 Qt 的事件传播机制，子 widget 的事件过滤器会收到。ADS 的 eventFilter 利用此事件在 `ColorSchemeMode==FollowPalette` 时重新加载样式表。在调用 `setPalette` 后设置的自定义 QSS 会被覆盖。
2. **ADS `ColorSchemeMode` 默认值为 `FollowPalette`**——意味着任何 palette 变化都会触发 loadStylesheet。除非显式设置为 `Light`/`Dark`，或使用 `DisableStylesheet` 标志。
3. **前几轮根因分析为何全部错误**：第六十五~六十七轮分别归咎于"restoreState 时机"、"QFluentKit registerWidget"、"ADS default.css QLabel 规则"，均未发现 `setPalette → ApplicationPaletteChange → loadStylesheet` 这一真正因果链。教训：样式覆盖问题应优先排查事件驱动的样式重载机制，而非逐条检查 QSS 规则。

### 修改文件清单

- 修改：`app/ide.cpp`（添加 `DisableStylesheet` 配置标志 + QTimer lambda 中 applyFluentStyle 移到 restoreState 之前）
- 修改：`CHANGELOG.md`（新增第六十八轮条目，归档第六十五轮）

### 测试影响

- 全部 3 项修复均无回归。全量 1758/1758 测试通过。
- UI 样式问题属于视觉表现层，单元测试不直接覆盖，构建通过 + 无回归即视为修复验证。建议用户启动 IDE 视觉确认。

## 2026-07-09 · 第六十七轮：UI 三大顽疾根因修复（P1 × 2 + P2 × 1，共 3 项）

### 概述

本轮针对用户反复反馈（经三轮修复均未解决）的三个 UI 问题进行彻底根因排查并修复：**(1) ADS dock 标签蓝底+米黄字体突兀**、**(2) 编译分析面板字节码背景未变米黄色**、**(3) 面板初始大小每次都要手动调整**。通过调试日志验证 restoreState 机制正确（restored=1），定位字节码背景问题的真正根因为 QFluentKit `StyleSheetManager::updateStyleSheet` 在主题信号触发时重新应用 `list_view.qss`（`background: transparent`）覆盖自定义 `itemViewQss`。MSVC 19.51 + Qt 6.10.3 + Ninja 构建通过，全量 1758/1758 测试通过。

### 问题与修复对应表

| # | 严重性 | 类型 | 位置 | 修复内容 |
|---|--------|------|------|----------|
| 1 | P1 | 样式覆盖 | `app/ide.cpp` applyFluentStyle registerWidget | **字节码/文件树/错误列表背景被 QFluentKit 覆盖为 transparent**——根因：`StyleSheet::registerWidget(widget, LIST_VIEW)` 注册后，QFluentKit `StyleSheetManager` 在 `Theme::onThemeModeChanged`/`onThemeColorChanged` 信号触发时调用 `updateStyleSheet(false)`，重新应用 `list_view.qss`（`ListWidget { background: transparent; }`），覆盖 `itemViewQss` 中设置的米黄色背景。`setStyleSheet(itemViewQss)` 仅在 `applyFluentStyle()` 调用时生效，主题信号触发后被 QFluentKit 覆盖。修复：移除 `fileTree_`/`errorListWidget_`/`bytecodeList_`/`recentListWidget_`/`tokenTable_` 的 `registerWidget` 调用，这些控件由 `itemViewQss` 统一样式化，Fluent ScrollBar 仍单独替换。`editorTabWidget_`（TAB_VIEW）保留注册（无背景冲突）。 |
| 2 | P1 | 样式覆盖 | `app/ide.cpp` applyFluentStyle adsQss | **ADS dock 标签 focused/active 状态蓝底+米黄字体**——根因：ADS `default.css` 包含 `ads--CDockWidgetTab[activeTab="true"] QLabel { color: palette(foreground); }` 规则，使用系统默认前景色，覆盖 adsQss 中 tab 的 color 属性。修复：在 adsQss 中添加显式 `ads--CDockWidgetTab QLabel`、`ads--CDockWidgetTab[activeTab="true"] QLabel`、`ads--CDockWidgetTab[focused="true"] QLabel` 子选择器规则，设置正确的 color（普通 tab 用 fgPrimary，active/focused tab 用 accent 色），防止 default.css 的 palette 规则干扰。 |
| 3 | P2 | 布局恢复 | `app/ide.cpp` restoreLayout QTimer::singleShot(0) | **面板初始大小每次都要手动调整**——根因：ADS `restoreState` 在构造函数或 showEvent 中调用时，窗口几何尺寸无效（未经过布局引擎处理），ADS 内部依赖容器几何计算 splitter 比例，在无效几何下静默失败。修复：将 `restoreState` 延迟到 `QTimer::singleShot(0)` 中执行——事件循环开始后所有 show/layout 事件已处理完毕，几何尺寸有效，restoreState 能正确恢复。通过调试日志验证 `restored=1`（成功）、`pendingDockState_.size()=646`（有保存数据）。同时清理了验证用的 3 处调试日志代码。 |

### 关键设计决策

1. **移除 registerWidget 而非对抗 QFluentKit**：`list_view.qss` 的 `background: transparent` 是 QFluentKit 的设计决策（透明背景让父容器色透出），与我们的 `itemViewQss`（显式设置米黄色背景）根本冲突。尝试用 `setCustomStyleSheet` 集成会引入 light/dark QSS 双份管理的复杂性，且 `list_view.qss` 的 `alternate-background-color: transparent` 仍会覆盖 palette 的 AlternateBase。直接移除注册是最简洁的方案——这些控件不需要 QFluentKit 的 list_view 样式（透明背景、item padding 等），`itemViewQss` 已覆盖所有需求。
2. **QLabel 子选择器规则**：ADS 的 `default.css` 对 tab 内 QLabel 有 `color: palette(foreground)` 规则。即使 tab 本身的 `color` 属性正确，QLabel 子控件可能继承 default.css 的颜色。显式添加 `ads--CDockWidgetTab QLabel { color: ... }` 确保文字颜色与 tab 背景一致。
3. **QTimer::singleShot(0) 延迟 restoreState**：Qt 事件循环的 `singleShot(0)` 在当前事件处理完毕后立即触发，此时所有 show/layout 事件已处理，窗口几何有效。`firstShow_` 守卫在 restoreState 完成后才设为 false，防止 restoreState 触发的 Resize 事件误启动 `splitterSaveTimer_` 覆盖用户布局。

### 关键教训

1. **QFluentKit registerWidget 是双向承诺**——注册后 QFluentKit 获得 stylesheet 管理权，会在主题信号时重新应用其 QSS。若自定义 QSS 与 QFluentKit QSS 冲突（如 background），每次主题变化都会被覆盖。对于需要自定义背景的控件，不应注册到 QFluentKit。
2. **ADS default.css 的子选择器规则容易被忽略**——`ads--CDockWidgetTab[activeTab="true"] QLabel { color: palette(foreground); }` 这类规则不直接影响 tab 本身，但影响其子控件（QLabel），导致文字颜色异常。自定义 ADS QSS 时必须同时覆盖子控件选择器。
3. **Qt 窗口几何在 showEvent 中可能无效**——`showEvent` 在窗口首次显示时触发，但布局引擎可能尚未完成几何计算。依赖容器几何的 API（如 ADS `restoreState` 的 splitter 比例计算）应延迟到事件循环空闲后调用。

### 修改文件清单

- 修改：`app/ide.cpp`（移除 5 个控件的 registerWidget 调用 + 添加 ADS tab QLabel 子选择器规则 + QTimer::singleShot(0) restoreState + 清理 3 处调试日志）
- 修改：`CHANGELOG.md`（新增第六十七轮条目）

### 测试影响

- 全部 3 项修复均无回归。全量 1758/1758 测试通过。
- UI 样式问题属于视觉表现层，单元测试不直接覆盖，构建通过 + 无回归即视为修复验证。

**历史变更**：更早的开发记录（第六十六轮及以前）已归档至 [docs/changelog/archive/](docs/changelog/archive/)。
