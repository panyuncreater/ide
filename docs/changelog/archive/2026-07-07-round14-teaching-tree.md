# MiniLang 变更日志归档

本文件包含 MiniLang IDE 第十四轮教学面板布局重构的完整变更记录（2026-07-07）。

---

## 2026-07-07 · 第十四轮教学面板布局重构（树形导航 + centerStack_ 切换 + 全局字号）

### 概述

按用户需求将所有 19 个教学面板从右侧独立 dock 迁移到中央 `centerStack_` 子页，新建 `TeachingTreePanel` 树形导航组件替代 `LearningHubDialog` 弹窗模式。点击 ActivityBar 「学习」图标 → 左侧显示可折叠树形导航（4 大分类 + 顶部「代码编辑器」入口）→ 点击叶子节点切换中央区域到对应教学面板，点击「代码编辑器」切回编辑器+输出面板。同时修复代码区字号调节仅作用于当前编辑器的问题：引入全局 `codeFontSize_` 成员 + `applyCodeFontSizeToAllEditors()` 方法，字号调节（Ctrl+=/-/0）应用于所有已打开编辑器标签页，新创建标签页自动继承全局字号。MSVC 19.51 + Qt 6.10.3 + Ninja 构建通过，全量 1693/1693 测试通过。

### 修改内容

1. **新建 gui/TeachingTreePanel.h / .cpp（教学面板树形导航组件）**
   - QTreeWidget 单列树，顶部「📝 代码编辑器」特殊项（加粗）+ 分隔符 + 4 大分类
   - 4 大分类（与 LearningHubDialog / viewMenu 4 子菜单一致）：🌱 入门导览 / 🔤 编译前端 / ⚙️ 执行引擎 / 🏗️ 深入实战
   - 22 个叶子节点（含 welcome / code-journey / learning-path / glossary / pipeline / token-puzzle / ast-toy / syntax-explorer / backend-compare / vm-sandbox / memory-model / ir-transform / bytecode-trace / call-stack / variable-inspector / breakpoint-condition / bug-hunt / exception-flow / closure-inspector / profile-dashboard / lab-manual）
   - 默认仅展开第一个分类（入门导览），其余折叠——隐式实现「实验手册默认不打开」（实验手册在「深入实战」分类下）
   - `panelRequested(panelId)` 信号：点击叶子节点或「代码编辑器」时发射
   - `setCurrentPanel(panelId)` 槽：跨面板跳转时同步树高亮

2. **app/ide.h — 新增成员与方法声明**
   - `#include "gui/TeachingTreePanel.h"` + `#include <QMap>`
   - 新增成员：`teachingTreeDock_` / `teachingTreePanel_` / `panelToStackIndex_`（QMap<QString,int>）/ `centerInEditorMode_`（bool）/ `codeFontSize_`（int，默认 11）
   - 新增方法声明：`showTeachingPanel(panelId)` / `showEditorArea()` / `onTeachingPanelRequested(panelId)` / `applyCodeFontSizeToAllEditors()`

3. **app/ide.cpp — initUI 教学面板迁移**
   - 原 19 个教学面板从 `dockManager_->createDockWidget + addDockWidget(RightDockWidgetArea)` 改为通过 `registerTeachingPanel` lambda 添加到 `centerStack_` 并建立 `panelToStackIndex_` 映射
   - `registerTeachingPanel(panelId, title, panel)` → `wrapTeachingPanel` 包装 + `centerStack_->addWidget` + 索引映射
   - `teachingTreeDock_` 创建并与 `fileTreeDock_` 同区域 tab；`toggleView(false)` 默认隐藏
   - `connectDockSave` 与 `syncViewMenuChecks` 简化：移除 19 个教学面板 dock 的调用，仅保留 `teachingTreeDock_`

4. **app/ide.cpp — 视图菜单 19 个教学面板 action 重构**
   - 提取 `makeTeachingToggle(action, panelId, label, shortcut)` lambda 替代 19 个重复的 `connect(toggled, ...)` 块
   - toggled(true) → `showTeachingPanel(panelId)`；toggled(false) → 仅当当前正显示该面板时切回编辑器
   - `syncViewMenuChecks` 新增 `syncTeachingAction` lambda，按 `centerStack_->currentIndex()` 同步 19 个 action 的勾选状态
   - `viewLearningHubAction_` 从 `triggered → showLearningHub()` 改为 `toggled → teachingTreeDock_ toggleView`（替代原 LearningHubDialog 弹窗）

5. **app/ide.cpp — 路由更新**
   - `onActivityChangedById("learn")`：从 `showLearningHub()` 改为显示 `teachingTreeDock_`（隐藏文件树/调试面板）
   - `onActivityRequested`：19 个 `showDock(xxxDock_)` 改为 `showTeachingPanel(panelId)`；`ensureEditorVisible()` 改为 `showEditorArea()`
   - `onJumpToPanel`：editor/tokens/ast/ir/bytecode/output 路由前置 `showEditorArea()`（先切回编辑器模式再打开对应面板）
   - `wrapTeachingPanel` 的 learningPathRequested 信号、3 处 WelcomeWizard learningPathRequested、GuidedTour finished → 全部改为 `showTeachingPanel("learning-path")`

6. **app/ide.cpp — 4 个新方法实现**
   - `showTeachingPanel(panelId)`：查 `panelToStackIndex_` 切换 `centerStack_`；pipeline/learning-path 刷新特例；隐藏 `bottomDock_`/`rightDock_`（教学面板独立展示）；同步教学树高亮；确保 `teachingTreeDock_` 可见
   - `showEditorArea()`：切 `centerStack_` 到 `editorTabWidget_`（或 `welcomePage_`）；同步教学树高亮到「代码编辑器」项；不强制恢复 dock（尊重用户布局选择）
   - `onTeachingPanelRequested(panelId)`：教学树点击路由（"editor" → showEditorArea，其余 → showTeachingPanel）
   - `applyCodeFontSizeToAllEditors()`：遍历 `editorTabs_`，按 `codeFontSize_ - cur` delta 调用 `changeFontSize`

7. **app/ide.cpp — 字号调节全局化**
   - `codeFontSize_` 成员变量（默认 11pt）替代直接调用 `codeEditor_->changeFontSize`
   - 字号菜单 Ctrl+=/-/0 → 更新 `codeFontSize_`（qBound 8~32）+ `applyCodeFontSizeToAllEditors()`
   - `createNewEditorTab` 新编辑器：`if (codeFontSize_ != 11) data.editor->changeFontSize(codeFontSize_ - 11)` 应用全局字号

8. **cmake/minilang_core.cmake — 注册 TeachingTreePanel.cpp**

### 关键设计决策

- **教学面板迁移到 centerStack_ 而非保留 dock**：教学面板占据主区域（原代码编辑区位置），与代码编辑器并列切换，符合用户「右侧代码区变成相应章节」需求。`panelToStackIndex_` 映射 panelId → 索引，O(1) 切换
- **实验手册默认不打开通过分类折叠隐式实现**：实验手册在「深入实战」分类下，默认折叠，无需额外标志位
- **教学面板模式隐藏 bottomDock_/rightDock_**：教学面板独立展示时不需要输出/错误/REPL/编译分析面板（这些是代码编辑器配套），`showTeachingPanel` 主动隐藏它们；`showEditorArea` 不强制恢复，尊重用户上次布局
- **字号全局化用 delta 而非绝对值**：`applyCodeFontSizeToAllEditors` 按 `codeFontSize_ - cur` delta 调用 `changeFontSize`，复用 CodeEditor 公开 API，无需暴露 `setFont` + `updateLineNumberAreaWidth`
- **makeTeachingToggle lambda 替代 19 个重复 connect 块**：19 个 view action 的 toggled 处理逻辑完全一致（on → showTeachingPanel，off → showEditorArea），提取为 lambda 避免代码重复
- **syncTeachingAction lambda 同步勾选**：`syncViewMenuChecks` 用 `centerStack_->currentIndex() == panelToStackIndex_[panelId]` 判断哪个教学面板正在显示，仅勾选对应的 view action

### 修改文件清单

- 新增：`gui/TeachingTreePanel.h`（51 行）+ `gui/TeachingTreePanel.cpp`（177 行）
- 修改：`app/ide.h`（+8 行：TeachingTreePanel include + 6 个成员 + 4 个方法声明）
- 修改：`app/ide.cpp`（+~150 行：4 个新方法实现 + makeTeachingToggle lambda + syncTeachingAction lambda + 字号全局化 + 路由更新；-~250 行：移除 19 个 view action 重复 connect 块 + 19 个 showDock 调用 + 19 个 dock 创建块）
- 修改：`cmake/minilang_core.cmake`（+5 行：注册 TeachingTreePanel.cpp）

### 验证结果

- MSVC 19.51 + Qt 6.10.3 + Ninja 构建：`minilang_ide` target exit code 0；`minilang_tests` target exit code 0
- 全量测试：`ctest --test-dir out/build/debug` 1693/1693 通过（225.42s，0 失败）
- 无新增依赖：仅新增 2 个文件 + 修改 3 个文件，无 CMakeLists.txt 改动（cmake/minilang_core.cmake 已注册）
