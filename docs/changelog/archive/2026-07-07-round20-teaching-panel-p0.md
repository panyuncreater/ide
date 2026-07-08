# 2026-07-07 · 第二十轮：教学模块 P0 三项改进（归档）

> 本文件归档自 CHANGELOG.md，按「只保留最近 3 个版本」规则移入 archive。
> 原文位置：CHANGELOG.md 顶部第二十轮章节。

## 概述

本轮依据【MiniLang教学模块分析与改进建议.md】完成 P0 优先级三项改进，覆盖教学闭环打通、游戏进度持久化、错误信息友好化三条主线。修复后全量 1730/1730 测试通过，minilang_ide 与 minilang_tests 双目标构建成功。

## 修改内容

1. **P0-1 打通「看教学 + 写代码 + 看运行结果」同屏（F5/F13）— app/ide.cpp + gui/LabManualPanel.{h,cpp}**
   - `showTeachingPanel()` 引入白名单策略：lab-manual / bug-hunt / syntax-explorer / backend-compare / ir-transform / profile-dashboard 等需底部 dock 的面板显示时不再隐藏 bottomDock_，仅 rightDock_ 隐藏
   - LabManualPanel 新增 `runSampleRequested` 信号 + `runBtn_`「▶ 运行示例」按钮 + `onRunSample()` 槽
   - app/ide.cpp 连接 `runSampleRequested` → `loadCodeIntoMainEditor(code)` + `onRun()`，实现「看教学样例 → 一键运行 → 底部输出」闭环

2. **P0-2 细化游戏进度粒度并持久化星级（F7）— gui/LearnerProgress.{h,cpp} + 三个游戏面板 + app/ide.cpp**
   - `LearnerProgress` struct 新增 `std::map<std::string, int> levelStars` 字段（key="token-puzzle-N"/"ast-toy-level-N"/"level-N"，value=-1 未完成 / 0 跳过 / 1-3 星级）
   - `LearnerProgressStore` 新增 3 个 API：`markLevelStars(levelId, stars)`（保留历史最佳）、`getLevelStars(levelId)`、`areAllLevelsCompleted(levelIds)`
   - `load()` 读取 levelStars JSON 对象，`save()` 写入，`reset()` 同步清空
   - **TokenPuzzlePanel**：构造时从 store 加载已记录星级，按已完成关卡解锁下一关 UI；`onCheckAnswer()`/`onSkipLevel()` 写入 store 并保存
   - **AstBuilderToyPanel**：构造时从 store 加载已完成关卡列表；`markCurrentCompleted()` 写入 store（AST 玩具无星级评分，统一记 3 星）
   - **VmStackSandboxPanel**：`onCheck()` 通关时写入 store（VM 沙盒无星级评分，统一记 3 星）
   - **app/ide.cpp** 三个 connect 简化：移除原 `levelId.section('-', 0, 1)` 的复杂映射逻辑，统一改为只转发粗粒度活动 ID 到 LearningPathPanel（细粒度星级由面板自身直接持久化到 LearnerProgressStore）

3. **P0-3 主编译/运行路径接入 ErrorHintEngine（F10/F11）— app/IdeController.{h,cpp} + app/ide.cpp + gui/ReplPanel.cpp**
   - **IdeController** 新增 `getReplScopeVariableNames()` 公共接口：返回当前 Interpreter 作用域链上所有可见变量名（去重，子作用域优先），供 GUI ErrorHintEngine 拼写建议使用
   - **app/ide.cpp** 三处 appendError 调用点接入 ErrorHintEngine：
     - `runtimeError` 信号处理器：收集 scopeVars → `enrichErrorMessage(msg, "runtime", scopeVars)` → diag.format()
     - `displayDiagnostics()` 循环内：对 Error 级诊断调用 `enrichErrorMessage(msg, "parse", {})`（parse 阶段无 scopeVars）
     - VM ERROR case：调用 `enrichErrorMessage(msg, "runtime", {})`（VM 路径无法直接访问 Interpreter 作用域）
   - **gui/ReplPanel.cpp** 异步 worker 线程的 RuntimeError/std::exception 捕获点：在 worker 线程上下文（此时主线程在 pollReplFuture 阻塞等待）调用 `ctrl->getReplScopeVariableNames()` 收集 scopeVars，再 `enrichErrorMessage` 增强消息后投递到 REPL 输出区。原有 emit runtimeError/genericError 信号保留原始 msg，由主线程 handler 二次增强（避免双重增强）

## 关键决策

1. **白名单而非黑名单**：P0-1 的 dock 可见性策略用白名单（kKeepBottomDockPanels），新增需底部 dock 的教学面板需显式加入白名单。比黑名单更安全，避免新增面板时遗忘导致默认被隐藏
2. **markLevelStars 保留历史最佳**：相同关卡的更高星级才覆盖，避免用户重玩低星覆盖历史高星记录
3. **AST 玩具/VM 沙盒统一记 3 星**：这两个面板无星级评分机制（只有结构/序列比对），用 3 星表示"完美通过"，与 TokenPuzzle 的 1-3 星语义区分
4. **ReplPanel 双路径增强避免双重增强**：worker 线程捕获点增强后投递到 REPL 输出区；emit 信号携带原始 msg，由主线程 handler 二次增强后显示到主错误面板。两条路径各自增强一次，避免重复附加教学提示
5. **parse 阶段 scopeVars 为空**：MiniLang 的未定义变量/函数错误是运行时错误（解释器求值阶段），parse 阶段仅做语法检查。故 displayDiagnostics 的 parse 错误 scopeVars 为空，拼写建议在 runtime 错误路径生效

## 修改文件清单

- 修改：`app/IdeController.h`（新增 getReplScopeVariableNames 声明）
- 修改：`app/IdeController.cpp`（新增 Interpreter.h + unordered_set include + getReplScopeVariableNames 实现）
- 修改：`app/ide.cpp`（顶部新增 ErrorHintEngine.h include；runtimeError/displayDiagnostics/VM ERROR 三处接入；P0-1 showTeachingPanel 白名单；P0-1 LabManualPanel runSampleRequested connect；P0-2 三游戏面板 connect 简化）
- 修改：`gui/LabManualPanel.h`（新增 runSampleRequested 信号 + runBtn_ 成员 + onRunSample 槽）
- 修改：`gui/LabManualPanel.cpp`（新增 runBtn_ 创建 + onRunSample 实现）
- 修改：`gui/LearnerProgress.h`（LearnerProgress 新增 levelStars 字段 + 3 个 API 声明）
- 修改：`gui/LearnerProgress.cpp`（load/save/reset 同步 levelStars + 3 个 API 实现）
- 修改：`gui/TokenPuzzlePanel.cpp`（构造加载持久化星级 + onCheckAnswer/onSkipLevel 写入 store + include LearnerProgress.h）
- 修改：`gui/AstBuilderToyPanel.cpp`（构造加载已完成关卡 + markCurrentCompleted 写入 store + include LearnerProgress.h）
- 修改：`gui/VmStackSandboxPanel.cpp`（onCheck 通关写入 store + include LearnerProgress.h）
- 修改：`gui/ReplPanel.cpp`（worker 线程 RuntimeError/std::exception 捕获点收集 scopeVars + ErrorHintEngine 增强）
- 文档同步：`README.md` + `CHANGELOG.md` + `docs/development.md` + `project_memory.md`
