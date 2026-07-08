# 2026-07-07 · 第十九轮：7 项教学面板修复与优化（已归档）

> 本文件从 CHANGELOG.md 归档。第二十二轮新增时按「只保留最近 3 个版本」规则将第十九轮移入此处。

### 概述

本轮完成用户指定的 7 项教学面板修复与优化任务，覆盖学习面板布局、三栏布局、新手引导、主题配色、条件断点场景加载、性能仪表盘运行剖析、教学字号调节。修复过程中发现并解决 4 个预存编译错误（GlossaryPanel.cpp 字符串拼接 / SandboxLevels.cpp 缺 include / TokenPuzzleData.cpp 未转义引号 / AstToyLibrary::findByLevel 未实现），全量 1730/1730 测试通过。

### 修改内容

1. **任务2 三栏布局（app/ide.h + app/ide.cpp）**
   - 新增 `QSplitter* centerSplitter_` 成员，包含 [centerStack_ | editorTabWidget_] 水平布局
   - `showTeachingPanel()` 适配三栏：显示 centerStack_，按 editorTabWidget_ 标签数决定显示/隐藏
   - `showEditorArea()` / `ensureEditorVisible()` / `onEditorTabCloseRequested()` 适配三栏切换
   - F3/Shift+F3 快捷键 + `syncViewMenuChecks()` 兼容 `centerInEditorMode_`

2. **任务3 新手引导补全（gui/TeachingPanelHeader.cpp）**
   - 补全 glossary（术语表）面板帮助文案，覆盖 20 个教学面板

3. **任务4 VM 沙盒配色统一（gui/VmStackSandboxPanel.cpp）**
   - 替换 stackList_ / outputEdit_ 两处硬编码 `#101820` 黑色为 `TeachingTheme::surface()` / `surfaceHover()`

4. **任务5 条件断点场景加载（gui/BreakpointConditionPanel.cpp/.h）**
   - 修复 8 个场景的 MiniLang 语法错误（print 语句括号 / 类构造 ClassName() / len() 方法）
   - 新增 `loadSampleBtn_` 按钮 + `loadSampleRequested` 信号 + `currentSampleCode_` 成员
   - `setController` 从 inline 改为 .cpp 实现（注册 vmStateChanged 监听器）

5. **任务6 性能仪表盘运行剖析（gui/ProfileDashboardPanel.cpp）**
   - 修复 6 个场景的 MiniLang 语法错误（fib/loop-sum/string-concat/class-instantiation/closure-capture/dict-access）

6. **任务7 教学字号调节（app/ide.h + app/ide.cpp）**
   - 新增 `applyTeachingFontSize()` 方法：递归遍历 centerStack_ 所有页面，统一设置 QTextBrowser/QListWidget/QTextEdit 字号
   - `applyCodeFontSizeToAllEditors()` 末尾调用 `applyTeachingFontSize()`
   - 构造函数中 `applyFluentStyle()` 后初始化教学字号

7. **预存编译错误修复**
   - **gui/GlossaryPanel.cpp**：修复 line 343/648 两处 `"```\n"\n\n"` 字符串拼接错误（多了 `\n"`）
   - **gui/SandboxLevels.cpp**：补充 `#include "gui/SandboxLevels.h"`（原文件缺少头文件包含）
   - **gui/TokenPuzzleData.cpp**：修复多处 C++ 字符串字面量中未转义的 ASCII 双引号（用作中文引号），统一替换为「」
   - **gui/BreakpointConditionPanel.cpp**：修复 `PushButton` → `QPushButton`（原代码误用不存在的类型名）
   - **gui/AstToyLevels.cpp**：补充 `AstToyLibrary::findByLevel()` 实现（头文件声明但 .cpp 未实现，导致链接器错误）

### 关键决策

1. **任务1 学习按钮侧边栏化**：确认第十四轮已实现 TeachingTreePanel 替代 LearningHubDialog 弹窗，无需重复实施
2. **三栏布局用 QSplitter 而非 QDockWidget**：centerSplitter_ 简单可靠，各面板独立关闭由 widget->hide() 实现
3. **教学字号跟随代码字号**：`applyTeachingFontSize()` 以 `codeFontSize_ + 2` 为基准（范围 [9,34]），与代码编辑器字号联动
4. **BreakpointConditionPanel 测试失败根因**：添加新成员 `currentSampleCode_` / `loadSampleBtn_` 后，ninja 未自动重编译 TestTeachingPanelsE2E.cpp.obj，导致类布局不匹配（旧 obj 分配的 BreakpointConditionPanel 尺寸不足，构造函数写入新成员越界 → STATUS_HEAP_CORRUPTION）。手动删除过期 .obj 后全量通过

### 修改文件清单

- 修改：`app/ide.h`（+QSplitter 成员 +applyTeachingFontSize 声明）
- 修改：`app/ide.cpp`（三栏布局 + 教学字号 + 适配方法）
- 修改：`gui/TeachingPanelHeader.cpp`（glossary 帮助文案）
- 修改：`gui/VmStackSandboxPanel.cpp`（配色统一）
- 修改：`gui/BreakpointConditionPanel.cpp/.h`（场景修复 + 加载按钮 + setController）
- 修改：`gui/ProfileDashboardPanel.cpp`（场景修复）
- 修改：`gui/GlossaryPanel.cpp`（字符串拼接修复）
- 修改：`gui/SandboxLevels.cpp`（补充 include）
- 修改：`gui/TokenPuzzleData.cpp`（引号转义修复）
- 修改：`gui/AstToyLevels.cpp`（findByLevel 实现）
- 文档同步：`README.md`（测试徽章 1720→1730）+ `CHANGELOG.md` + `docs/development.md` + `project_memory.md`
