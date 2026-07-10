# Changelog

本文件记录 MiniLang IDE 的开发演进历史，包括性能优化、正确性修复与工程基础设施改进。所有条目均通过全量单元测试验证。历史版本归档至 [docs/changelog/archive/](docs/changelog/archive/)。

## 2026-07-10 · 第八十八轮：跨机器字体一致性（UI × 1，共 1 项）

### 概述

彻底解决 IDE 界面字体在其他机器上"看不清"的问题。根因有三：(1) `main.cpp` 全局字体用 `setPixelSize(14)` + 短回退链 `{"Microsoft YaHei", "PingFang SC", "Segoe UI"}`，目标机器缺中文字体（如英文 Windows LTSC / 精简版）时中文 UI 回退到非中文字体，显示方块或难看；(2) `GuiTextUtils::monospaceFont()` 硬编码 `QFont("Consolas", n)` 无回退链，Consolas 在 Server / 精简版可能缺失；(3) 7 处面板（BackendComparePanel / IRTransformPanel / PipelineViewer）直接 `QFont("Consolas")` 绕过共享工厂。修复方案：在 `GuiTextUtils.h` 建立统一字体工厂，UI 字体与等宽字体均用 `QFont::setFamilies` 设置完整中英文回退链 + `setPointSize` 物理单位（随 DPI 自适应），等宽字体额外 `setStyleHint(QFont::TypeWriter)` 确保回退到系统等宽而非比例字体。

### 改动范围

| 层 | 文件 | 变更 |
|----|------|------|
| 字体工厂 | `gui/GuiTextUtils.h` | 新增 `uiFontFallbackChain()`（8 字体：Microsoft YaHei → PingFang SC → Noto Sans CJK SC → Source Han Sans SC → Segoe UI → SF Pro Text → Arial Unicode MS → Arial）、`monoFontFallbackChain()`（8 字体：Cascadia Code → Cascadia Mono → Consolas → JetBrains Mono → Source Code Pro → Menlo → DejaVu Sans Mono → Courier New）、`uiFont(pointSize=10)` 工厂；改进 `monospaceFont()` 用 `setFamilies` + `setStyleHint(TypeWriter)` 替代硬编码 `QFont("Consolas", n)` |
| 入口 | `app/main.cpp` | 全局字体从 `setPixelSize(14)` + 短回退链改为 `GuiTextUtils::uiFont(10)`（10pt ≈ 13.3px @ 96dpi，完整回退链 + pt 物理单位） |
| 面板 | `gui/BackendComparePanel.cpp` `gui/IRTransformPanel.cpp` `gui/PipelineViewer.cpp` | 7 处 `QFont("Consolas")` 替换为 `GuiTextUtils::monospaceFont(10)`，并添加 `#include "gui/GuiTextUtils.h"` |
| 调试面板 | `gui/DebugPanel.cpp` | `styleScopeGroupHeader` 字号缩减从 `pixelSize` 改为 `pointSize`（全局字体改用 pt 后 `pixelSize()` 返回 -1，原逻辑失效） |

### 回退链设计

| 字体类型 | 回退链 | 设计意图 |
|----------|--------|----------|
| UI 字体 | Microsoft YaHei → PingFang SC → Noto Sans CJK SC → Source Han Sans SC → Segoe UI → SF Pro Text → Arial Unicode MS → Arial | 中文优先（保证中文 UI 不回退到非中文字体）→ 跨平台中文 → 英文 UI → 通用兜底 |
| 等宽字体 | Cascadia Code → Cascadia Mono → Consolas → JetBrains Mono → Source Code Pro → Menlo → DejaVu Sans Mono → Courier New | 现代等宽 → 传统等宽 → 通用回退，`setStyleHint(TypeWriter)` 确保最终回退到系统等宽 |

### setPixelSize → setPointSize 理由

- `setPixelSize(n)` 设置逻辑像素高度，跨机器物理大小依赖 DPI 缩放设置，不同机器可能不一致
- `setPointSize(n)` 设置物理点数（1/72 英寸），Qt 按屏幕 DPI 自动计算像素，跨机器物理大小一致
- 10pt @ 96dpi = 13.3px，接近原 14px 但更标准

### QSS 中的 font-family

QSS 中的 `font-family: "Cascadia Code", "Consolas", "Courier New", monospace` 已包含 `monospace` 通用族作为最终回退，Qt 会选择系统默认等宽字体，无需修改。大部分控件继承 `QApplication::setFont()` 的全局字体（现通过 `uiFont()` 设置完整回退链）。

### 验证

MSVC 19.51 + Qt 6.11.1 + Ninja debug 增量构建通过，全量 1773/1773 测试通过，零回归。

### 修改文件清单

- `gui/GuiTextUtils.h`、`app/main.cpp`、`gui/BackendComparePanel.cpp`、`gui/IRTransformPanel.cpp`、`gui/PipelineViewer.cpp`、`gui/DebugPanel.cpp`
- `CHANGELOG.md`、`docs/development.md`

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
