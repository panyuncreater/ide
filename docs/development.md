# MiniLang 开发指南

本文档是 MiniLang IDE 开发文档的导航入口。详细的架构设计、决策记录与测试方法请参见各子文档。

## 文档导航

| 文档 | 说明 |
|------|------|
| [快速开始](getting-started.md) | 构建、运行、开发环境配置（Windows/Linux/macOS/Docker） |
| [核心架构](architecture.md) | 编译管线、NaN-boxing、三后端、内存模型、安全约束 |
| [测试指南](testing.md) | 三后端一致性验证方法论、测试分类与编写指南 |
| [设计决策 (ADR)](adr/) | 架构决策记录，详见下方索引 |
| [贡献指南](../CONTRIBUTING.md) | 环境搭建、编码规范、PR 流程 |

## 设计决策记录 (ADR)

| ADR | 主题 | 状态 |
|-----|------|------|
| [ADR-001](adr/ADR-001-nan-boxing.md) | NaN-boxing 值表示 | Accepted |
| [ADR-002](adr/ADR-002-triple-backend.md) | 三后端并存策略 | Accepted |
| [ADR-003](adr/ADR-003-ir-layer.md) | IR 中间层 | Accepted |
| [ADR-004](adr/ADR-004-cow-memory.md) | COW 内存模型 | Accepted |
| [ADR-005](adr/ADR-005-debug-consistency.md) | 调试一致性 | Accepted |

## 核心设计原则

- **PERF-12 NaN-boxing**：8 字节值编码，标量零分配，堆类型侵入式引用计数
- **PERF-13 VMStack**：定长数组 1024×8B + 栈顶指针，消除 push_back 开销
- **PERF-14 寄存器式 VM**：32 虚拟寄存器 R0-R31，59 条 RegOp，IR 直接 lowering
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


## 最近变更摘要

- **第七十七轮（2026-07-10）**：性能仪表盘指令计数口径修复（P1 × 1 + P3 × 1）。根因：`ProfileDashboardPanel::runProfile` 中 StackVM/RegisterVM 测量循环 `stackVmCounts[j] += counts[j]` 累加 N 次执行计数，但同一循环 `t.avgMicros = mean(samples)` 时间取平均，两者口径不一致。`scenario.iterations = 3` 导致指令计数表格"执行次数"是单次实际值的 3 倍（fib(15) OP_CALL 显示 ≈5922，实际单次 ≈1974）。修复：累加后 `stackVmCounts[j] /= iterations` 取平均。附带清理死代码 `measureStackVMOnce`/`measureRegisterVMOnce`（R73 改用 `measureXxxWithProfile` 后遗留）。全量 1763/1763 测试通过。
- **第七十六轮（2026-07-10）**：性能仪表盘退出崩溃根因修复（P0 × 1 + P1 × 1）。两个正交根因：(A) `ProfileDashboardPanel::runProfile` 中 3 次 `processEvents(ExcludeUserInputEvents)` 不排除 QCloseEvent（close 事件不是用户输入事件），closeEvent 在 runProfile 调用栈内同步执行后返回，runProfile 继续访问正在关闭的 widget → UAF。修复：ProfileDashboardPanel 新增 `closing_` 标志 + `setClosing()` 方法，closeEvent 入口调用 `setClosing(true)`，runProfile 在每次 processEvents 后检查并退出。(B) `closeEvent` 未停止 `statusAnimTimer_`（QTimer，400ms），R75 的 `stopChildAnimations` 只扫描 `QPropertyAnimation*` 不覆盖 `QTimer*`，maybeSave 模态对话框期间 timeout 信号被派发访问正在清理的 statusLabel_ → UAF。修复：`stopChildAnimations` 扩展 `findChildren<QTimer*>()` 并 stop() 所有 active QTimer。全量 1763/1763 测试通过。
- **第七十五轮（2026-07-10）**：学习中心教学面板打不开 + 关闭崩溃根因修复（P0 × 2）。两个正交根因：(A) 教学面板打不开——`ensureEditorVisible`/`showEditorArea` 启动折叠动画后用 `QTimer::singleShot(350,...)` 排队延迟回调执行 `centerStack_->hide()`，用户在 350ms 内切换到教学面板后该回调仍执行 hide() 覆盖 show()，`singleShot` 创建的临时 QTimer 事件队列独立无法取消。修复：3 处 `singleShot` 改为成员 QTimer（`pendingHideCenterTimer_`/`pendingHideEditorTimer_`），`showTeachingPanel` 入口 `stop()` 取消挂起的 hide 意图 + `stop()` 正在运行的折叠动画 `splitterAnim_`。(B) 关闭崩溃——`PanelAnimator::slideInWidget` 创建的 `QPropertyAnimation` 以 widget 为 parent，是 `QAbstractAnimation` 子类由 `QUnifiedTimer` 驱动不经过事件队列，`removePostedEvents` 无法清理，`maybeSave` 模态对话框期间动画继续运行访问正在析构的 widget → UAF。R72 已认知此原理但只应用到 `splitterAnim_`/`bottomPanelAnim_` 成员，遗漏 `slideInWidget` 的匿名动画。修复：closeEvent 中遍历 centerStack_/bottomStack_/rightStack_/dockManager_ 的子 `QPropertyAnimation`，运行中的 `stop()` + `deleteLater()`。全量 1763/1763 测试通过。
- **第七十四轮（2026-07-10）**：学习中心解除全部学习限制（功能调整 × 1）。用户反馈"学习中心打不开了"，根因：`LearnerProgressStore::isUnlocked` 检查活动的 prerequisites 字段，前置未完成则返回 false，`LearningPathPanel::buildActivityRow` 据此 `row->setEnabled(false)` 禁用点击并显示 🔒 图标。修复：`isUnlocked` 改为对有效活动 id 统一返回 true（仅无效 id 返回 false），所有章节与关卡无前置条件可自由访问；`nextRecommended` 同步改用 `isUnlocked` 替代独立 prerequisites 检查，消除解锁逻辑重复。prerequisites 数据保留以便后续按需恢复。全量 1763/1763 测试通过。
- **第七十二轮（2026-07-09）**：学习中心面板关闭 UAF 根因修复（P0 × 1 + P1 × 2 + P2 × 1）。核心发现：`QVariantAnimation` 由 `QUnifiedTimer` 驱动，不经过 Qt 事件队列，`removePostedEvents` 完全无法清理它。P0：`splitterAnim_`（centerSplitter 尺寸动画，7 处调用点）未在 closeEvent 中停止，maybeSave 模态对话框期间动画继续运行访问正在清理的 `centerSplitter_` → UAF。P1：底部面板展开/收起动画改用 `bottomPanelAnim_` 成员变量存储并在 closeEvent 停止；5 个教学面板首次访问自动触发的 GuidedTour 新增 `activePanelTours_` 跟踪，closeEvent 中 `disconnect` + `removePostedEvents(tour)` + `delete`。P2：新增 `clearVmStateChangedListeners()` 清空 7 个面板的纯 C++ 观察者回调（不受 Qt disconnect 影响）。全量 1773/1773 测试通过。
- **第七十轮（2026-07-09）**：UI 修复方案修正——DisableStylesheet 副作用（P1 × 2）。第六十八轮的 DisableStylesheet 虽阻止了 loadStylesheet 覆盖，但导致 ADS 按钮图标丢失（qproperty-icon 规则未执行）和 QToolTip 黑框（回退 Windows 11 原生黑色样式）。改用 `setColorSchemeMode(Light)`：保留构造时 default.css 初始加载（提供图标+基础样式），仅阻止后续 palette-change 重载。adsQss 补全 qproperty-icon 规则，QPalette 设置 ToolTipBase/ToolTipText，qApp 级别追加 QToolTip QSS。全量 1763/1763 测试通过。
- **第六十九轮（2026-07-09）**：REPL %magic 命令系统 5 项 Bug 修复（P1 × 3 + P2 × 2）。P1：(1) magic 命令在 isInputComplete 之前拦截，解决 `%ast fun f() {` 因未闭合括号被误判续行的问题；(2) 分析类命令（%ast/%tokens/%disassemble/%ir）支持参数代码，使用 ScopedAnalysis 临时 Lexer/Parser/Compiler 分析参数，不依赖 IdeController；(3) %reset 真正重置 REPL 环境（新增 Interpreter::resetReplEnvironment() 重建 globalEnv_、清空所有缓存）。P2：handleTokens idx 列对齐 off-by-one 修复；help 提示过时文本更新。全量 1763/1763 测试通过。
- **第六十八轮（2026-07-09）**：UI 三大顽疾真正根因修复——ADS loadStylesheet 覆盖（P1 × 3）。**注：本轮的 DisableStylesheet 方案在第七十轮被 setColorSchemeMode(Light) 替代**，因 DisableStylesheet 过于激进导致按钮图标丢失和 QToolTip 黑框回归。根因分析（setPalette→ApplicationPaletteChange→loadStylesheet 覆盖）正确，但修复方案 DisableStylesheet 有副作用，已修正。全量 1758/1758 测试通过。
- **第六十七轮（2026-07-09）**：UI 三大顽疾修复尝试（P1 × 2 + P2 × 1）——**本轮修复实际无效，根因分析错误**。归咎于 QFluentKit registerWidget 覆盖 itemViewQss，但真正根因是 ADS loadStylesheet（见第六十八轮）。移除了 5 个控件的 registerWidget 调用 + 添加 ADS tab QLabel 子选择器规则 + QTimer::singleShot(0) restoreState。全量 1758/1758 测试通过。

完整开发日志请参阅 [CHANGELOG.md](../CHANGELOG.md) 及 [归档目录](changelog/archive/)。
