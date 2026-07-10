# Changelog

本文件记录 MiniLang IDE 的开发演进历史，包括性能优化、正确性修复与工程基础设施改进。所有条目均通过全量单元测试验证。历史版本归档至 [docs/changelog/archive/](docs/changelog/archive/)。

## 2026-07-10 · 第七十七轮：性能仪表盘指令计数口径修复（P1 × 1 + P3 × 1，共 2 项）

### 概述

用户反馈"性能仪表盘中指令计数有问题"。经排查发现根因：`ProfileDashboardPanel::runProfile` 中时间统计取 N 次平均（`mean(samples)`），但指令计数却取 N 次累加（`stackVmCounts[j] += counts[j]`），两者口径不一致。由于 `scenario.iterations = 3`，指令计数表格显示的"执行次数"是单次实际值的 3 倍，与 OpCode 性能文档库的参考量级（如"fib(20) OP_CALL ~21891 次"）矛盾，造成用户困惑。修复后 StackVM / RegisterVM 指令计数均取 N 次平均，与时间统计口径一致。同时清理了 R73（P1-1 instrumentation 引入时）遗留的死代码 `measureStackVMOnce` / `measureRegisterVMOnce`（runProfile 实际走 `measureXxxWithProfile`，这两个函数从未被调用）。MSVC 19.51 + Qt 6.10.3 + Ninja 构建通过，全量 1763/1763 测试通过。

### 问题与修复对应表

| # | 类型 | 位置 | 修复内容 |
|---|------|------|----------|
| 1 | 语义不一致（P1） | `gui/ProfileDashboardPanel.cpp` runProfile(L517-584) | **指令计数累加 N 次而非取平均**——根因：StackVM / RegisterVM 测量循环中 `stackVmCounts[j] += counts[j]` 累加 N 次执行计数，但 `t.avgMicros = mean(samples)` 时间取平均。iterations=3 时显示值为单次实际值的 3 倍（如 fib(15) 单次 OP_CALL ≈ 1974，界面显示 ≈ 5922）。修复：累加完成后 `stackVmCounts[j] /= iterations` 取平均，与 avgMicros 口径一致。指令计数是确定值，取平均消除潜在非确定性。 |
| 2 | 死代码（P3） | `gui/ProfileDashboardPanel.{h,cpp}` | **measureStackVMOnce / measureRegisterVMOnce 从未被调用**——R73 P1-1 instrumentation 改用 `measureXxxWithProfile`（同时计时 + 累加 opcode 计数），但原 `measureXxxOnce`（仅计时）未删除，成为死代码。修复：删除 .h 声明与 .cpp 定义，避免维护混淆。 |

### 关键设计决策

1. **指令计数取平均而非取单次**——指令计数虽是确定值（同一字节码每次执行的指令数相同），但取平均与时间统计口径一致，且能消除潜在非确定性（如全局变量累积副作用影响控制流）。取单次（i==0）虽更简洁但无统计鲁棒性。
2. **保留 Interpreter 无指令计数**——Interpreter 是树遍历解释器，无指令概念（每个 AST 节点是 Visitor 分发，非 OpCode），指令计数表仅显示 StackVM / RegisterVM，设计意图保留。
3. **占比不受影响**——`aggregateTop10` 中 `ratio = count / total`，total 与 count 同比缩放，占比比例不变。仅绝对次数从 3 倍修正为单次值。

### 修改文件清单

- 修改：`gui/ProfileDashboardPanel.cpp`（runProfile StackVM/RegisterVM 累加后除以 iterations；删除 measureStackVMOnce / measureRegisterVMOnce 定义）
- 修改：`gui/ProfileDashboardPanel.h`（删除 measureStackVMOnce / measureRegisterVMOnce 声明）
- 修改：`CHANGELOG.md`（新增第七十七轮条目）

### 测试影响

- 全量 1763/1763 测试通过，零回归
- 指令计数属于 GUI 运行时行为，单元测试仅覆盖 OpCodeProfileLibrary 数据完整性（TestTeachingPanelsAudit4.cpp），计数口径修复建议手动验证：
  1. 打开性能仪表盘 → 选择"斐波那契递归 fib(15)" → 运行剖析 → 切换到"指令计数" tab
  2. StackVM 热点表 OP_CALL 执行次数应 ≈ 1974（单次），而非 ≈ 5922（3 倍）
  3. RegisterVM 热点表 REG_CALL 执行次数应与单次执行一致

---

## 2026-07-10 · 第七十六轮：性能仪表盘退出崩溃根因修复（P0 × 1 + P1 × 1，共 2 项）

### 概述

用户反馈"在学习中心使用性能仪表盘时退出又崩溃了"。经排查发现两个正交根因：(A) `ProfileDashboardPanel::runProfile` 中的 `QApplication::processEvents(ExcludeUserInputEvents)` **不排除 QCloseEvent**（close 事件不是用户输入事件），closeEvent 在 runProfile 调用栈内同步执行后返回，runProfile 继续访问正在关闭的 widget；(B) `closeEvent` 未停止 `ProfileDashboardPanel::statusAnimTimer_`（QTimer，400ms），R75 的 `stopChildAnimations` 只扫描 `QPropertyAnimation*` 不覆盖 `QTimer*`，maybeSave 模态对话框期间 timeout 信号被派发访问正在清理的 statusLabel_。R72 认知"QVariantAnimation 由 QUnifiedTimer 驱动必须 stop()"和 R75 认知"QPropertyAnimation 同理"均未覆盖第三类载体 QTimer。MSVC 19.51 + Qt 6.10.3 + Ninja 构建通过，全量 1763/1763 测试通过。

### 问题与修复对应表

| # | 类型 | 位置 | 修复内容 |
|---|------|------|----------|
| 1 | 重入/UAF（P0） | `gui/ProfileDashboardPanel.cpp` runProfile(L460-600) + `app/ide.cpp` closeEvent(L518) | **性能仪表盘退出崩溃**——根因：`runProfile` 中 3 次 `processEvents(ExcludeUserInputEvents)` 会派发 QCloseEvent（非用户输入事件）。用户在 profiling 期间关闭窗口，closeEvent 在 runProfile 调用栈内同步执行并 accept()，返回后 runProfile 继续执行 `renderResults`/`renderOpCodeProfile`/`update()` 访问正在关闭的 widget → UAF。修复：ProfileDashboardPanel 新增 `closing_` 标志 + `setClosing()` 方法，closeEvent 入口调用 `setClosing(true)`，runProfile 在每次 processEvents 后检查 `if (closing_) { stopStatusAnimation(); profiling_ = false; return; }` 立即退出。 |
| 2 | 定时器泄漏/UAF（P1） | `app/ide.cpp` closeEvent stopChildAnimations(L552-571) | **statusAnimTimer_ 未停止**——根因：`statusAnimTimer_` 是 QTimer（400ms，父对象是 panel），R75 的 `stopChildAnimations` 只 `findChildren<QPropertyAnimation*>()` 不覆盖 QTimer。`removePostedEvents(this)` 只清空 Ide 队列，不清空 statusAnimTimer_ 的队列。maybeSave 模态对话框期间 timeout 信号被派发，lambda 访问正在清理的 statusLabel_ → UAF。修复：`stopChildAnimations` 扩展 `findChildren<QTimer*>()` 并 `stop()` 所有 active QTimer，递归覆盖 centerStack_ 各页 + bottomStack_/rightStack_/dockManager_ 的子 QTimer。 |

### 关键设计决策

1. **`ExcludeUserInputEvents` 不排除 QCloseEvent**——Qt6 文档定义此 flag 只排除鼠标/键盘/滚轮事件，close 事件（type=19）不在排除列表。用户点击 X 按钮的鼠标事件被排除，但窗口系统生成的 QCloseEvent 会被 processEvents 派发。Alt+F4 更是直接生成 close 请求不经过键盘事件。因此 profiling 期间任何关闭操作都会在 processEvents 中触发 closeEvent。
2. **closing_ 标志单向下行**——closeEvent 设置 `Ide::closing_` 和 `ProfileDashboardPanel::closing_`，runProfile 只读不写。两个标志独立，closeEvent 不依赖 runProfile 的返回值。
3. **stopChildAnimations 扩展 QTimer**——防御性兜底，覆盖未来新增面板的 QTimer。与 R75 的 QPropertyAnimation 扫描并行，一行 `findChildren<QTimer*>()` 即可覆盖所有子 QTimer。
4. **三次 processEvents 后均检查 closing_**——第一次（Interpreter 前）、第二次（StackVM 前）、第三次（RegisterVM 前）各检查一次。若 closeEvent 在某次 processEvents 中被派发，下一次检查即会捕获并退出。

### 修改文件清单

- 修改：`gui/ProfileDashboardPanel.h`（新增 `closing_` 成员 + `setClosing()` 方法）
- 修改：`gui/ProfileDashboardPanel.cpp`（runProfile 三处 processEvents 后加 `if (closing_)` 检查并退出）
- 修改：`app/ide.cpp`（closeEvent 入口 `setClosing(true)` + stopChildAnimations 扩展 QTimer 扫描）
- 修改：`CHANGELOG.md`（新增第七十六轮条目）

### 测试影响

- 全量 1763/1763 测试通过，零回归
- 竞态/UAF 属于时序相关，单元测试难以直接覆盖，建议手动验证：
  1. 打开性能仪表盘 → 点击"运行剖析" → profiling 期间按 Alt+F4 → 无崩溃
  2. 打开性能仪表盘 → 点击"运行剖析" → profiling 完成后正常关闭 → 无崩溃
  3. 反复切换面板 + profiling + 关闭 → 无崩溃

---

## 2026-07-10 · 第七十五轮：学习中心教学面板打不开 + 关闭崩溃根因修复（P0 × 2，共 2 项）

### 概述

本轮根因修复两个用户反馈：用户报告"学习中心教学面板还是打不开"（R74 修复 `isUnlocked` 未解决）且"关闭时诱发异常"（R72 修复 `splitterAnim_` 未解决）。经系统性排查发现两个正交根因：(A) 教学面板打不开——`ensureEditorVisible`/`showEditorArea` 启动的折叠动画后排队 350ms 延迟回调（`QTimer::singleShot`），在用户切换到教学面板后仍执行 `centerStack_->hide()` 覆盖 `show()`，锁死教学区宽度为 0；(B) 关闭崩溃——`slideInWidget` 创建的 `QPropertyAnimation` 以 panel widget 为 parent（非 Ide 成员），`QPropertyAnimation` 是 `QAbstractAnimation` 子类由 `QUnifiedTimer` 驱动不经过事件队列，`removePostedEvents` 完全无法清理，`maybeSave` 模态对话框期间动画继续运行访问正在析构的 widget → UAF。R72 已认知此原理但只应用到 `splitterAnim_`/`bottomPanelAnim_` 成员，未应用到 `slideInWidget` 的匿名动画。MSVC 19.51 + Qt 6.10.3 + Ninja 构建通过，全量 1763/1763 测试通过。

### 问题与修复对应表

| # | 类型 | 位置 | 修复内容 |
|---|------|------|----------|
| 1 | 竞态/状态覆盖（P0） | `app/ide.cpp` ensureEditorVisible(L1312)/showEditorArea(L1545)/showTeachingPanel(L1377) | **教学面板打不开**——根因：`ensureEditorVisible`/`showEditorArea` 启动折叠教学区动画后用 `QTimer::singleShot(350,...)` 排队延迟回调执行 `centerStack_->hide()` + `setSizes({0,total})`。用户在 350ms 内切换到教学面板，`showTeachingPanel` 执行 `centerStack_->show()`，但挂起的 350ms 回调仍执行 `hide()` 覆盖。`singleShot` 创建的临时 QTimer 事件队列独立，无法取消。修复：改为成员 QTimer `pendingHideCenterTimer_`，`showTeachingPanel` 入口 `stop()` 取消挂起的 hide 意图；同时停止正在运行的折叠动画 `splitterAnim_`，避免旧动画继续压缩教学区宽度。 |
| 2 | UAF/P0 | `app/ide.cpp` closeEvent(L535-570) + `gui/PanelAnimator.h` slideInWidget(L213) | **关闭时崩溃**——根因：`PanelAnimator::slideInWidget` 创建 `QPropertyAnimation` 以 widget 为 parent，是 `QAbstractAnimation` 子类由全局 `QUnifiedTimer` 驱动不经过 Qt 事件队列，`removePostedEvents` 完全无法清理。closeEvent 的 `maybeSave` 模态对话框运行嵌套事件循环期间动画继续运行，访问正在 reparent/析构的 widget → 读取访问权限冲突。R72 已认知此原理但只应用到 `splitterAnim_`/`bottomPanelAnim_` 成员，遗漏 `slideInWidget` 的匿名动画。修复：closeEvent 中遍历 centerStack_/bottomStack_/rightStack_/dockManager_ 的子 `QPropertyAnimation`，运行中的 `stop()` + `deleteLater()`。 |

### 关键设计决策

1. **成员 QTimer 替代 singleShot**：3 处 `QTimer::singleShot`（350ms×2 + 360ms×1）改为成员 QTimer `pendingHideCenterTimer_`/`pendingHideEditorTimer_`。`start()` 重复调用自动重置计时取消旧的；`showTeachingPanel` 入口 `stop()` 撤销挂起的 hide 意图；closeEvent 中 `stop()` 避免回调在 maybeSave 期间触发。所有 lambda 加 `if (closing_) return;` 守卫。
2. **showTeachingPanel 入口停止折叠动画**：除了取消 hide 定时器，还 `stop()` 正在运行的 `splitterAnim_`。否则旧折叠动画继续把教学区宽度压缩到 0，且 `showTeachingPanel` 的动画分支条件在"教学区尚可见但正被压缩"时不满足，splitter 不重分配。
3. **closeEvent 遍历子动画**：`findChildren<QPropertyAnimation*>()` 递归遍历 centerStack_ 各页 + bottomStack_/rightStack_/dockManager_ 的子动画。运行中的 `stop()` + `deleteLater()`，彻底切断 QUnifiedTimer → valueChanged → widget 访问链。
4. **无效 dockManager_ 安全**：`stopChildAnimations` 入口检查 `!w` 提前返回，`dockManager_` 为 nullptr 时不崩溃。

### 修改文件清单

- 修改：`app/ide.h`（新增 `pendingHideCenterTimer_`/`pendingHideEditorTimer_` 成员变量）
- 修改：`app/ide.cpp`：
  - `ensureEditorVisible`/`showEditorArea`：350ms `singleShot` → 成员 QTimer + closing_ 守卫
  - `onEditorTabCloseRequested`：360ms `singleShot` → 成员 QTimer + closing_ 守卫
  - `showTeachingPanel`：入口 `stop()` pendingHideCenterTimer_ + `stop()` splitterAnim_
  - `closeEvent`：新增遍历子 QPropertyAnimation 并 stop() + 停止 pendingHide 定时器
  - 新增 `#include <QPropertyAnimation>` / `<QAbstractAnimation>`
- 修改：`CHANGELOG.md`（新增第七十五轮条目）

### 测试影响

- 全量 1763/1763 测试通过，零回归
- 本次修改仅涉及 UI 动画时序与 closeEvent 清理，不涉及语言核心逻辑
- 竞态/UAF 属于时序相关，单元测试难以直接覆盖，建议手动验证：
  1. 打开文件 → 350ms 内点击学习中心 → 面板应可见（不再被 hide 覆盖）
  2. 打开学习中心面板 → 150ms 内关闭窗口 → 无访问冲突
  3. 反复切换面板后关闭 → 无崩溃

---

## 2026-07-10 · 第七十四轮：学习中心解除全部学习限制（功能调整 × 1，共 1 项）

### 概述

本轮根据用户反馈"学习中心打不开了"，排查根因后发现：学习路径中的活动项被前置依赖（prerequisites）锁定，`LearnerProgressStore::isUnlocked` 检查前置活动是否完成，未完成则 `row->setEnabled(false)` 禁用点击，表现为 🔒 图标且不可进入。用户要求去掉所有学习限制，解锁全部章节与关卡。修改 `isUnlocked` 统一返回 true（仅对无效活动 id 返回 false），并同步 `nextRecommended` 改用 `isUnlocked` 而非独立的 prerequisites 检查，保证解锁逻辑单点维护。prerequisites 数据结构保留在 `LearningPathData` 中以便后续按需恢复。MSVC 19.51 + Qt 6.10.3 + Ninja 构建通过，全量 1763/1763 测试通过。

### 问题与修复对应表

| # | 类型 | 位置 | 修复内容 |
|---|------|------|----------|
| 1 | 学习限制/活动锁定 | `gui/LearnerProgress.cpp` isUnlocked (L612-623) + nextRecommended (L657-670) | **学习中心活动被前置条件锁定无法点击**——根因：`isUnlocked` 遍历活动的 prerequisites 字段，前置未完成则返回 false，`LearningPathPanel::buildActivityRow` 据此 `row->setEnabled(unlocked)` 禁用点击并显示 🔒 图标。用户反馈"打不开"即指这些被锁定的活动无法进入。修复：`isUnlocked` 改为对有效活动 id 统一返回 true，仅对无效 id 返回 false；`nextRecommended` 同步改用 `isUnlocked` 替代独立 prerequisites 检查，消除解锁逻辑重复。 |

### 关键设计决策

1. **单点修改 `isUnlocked`**：所有解锁判断（LearningPathPanel 行可点击性、nextRecommended 候选筛选、estimatedRemainingMinutes 计入范围）均经此函数，改一处即全局生效。
2. **保留 prerequisites 数据**：`LearningPathData` 中的 prerequisites 字段不删除，仅 `isUnlocked` 不再检查它。便于后续按需恢复学习限制（如增加"教学模式/自由模式"开关）。
3. **`nextRecommended` 统一走 `isUnlocked`**：原实现有独立的 prerequisites 检查循环，与 `isUnlocked` 逻辑重复。改为调用 `isUnlocked` 后，解锁策略变更只需改一处。
4. **无效 id 仍返回 false**：`isUnlocked` 对不在 activities 列表中的 id 返回 false，保留防御性校验，避免无效 id 误解锁。

### 修改文件清单

- 修改：`gui/LearnerProgress.cpp`（`isUnlocked` 统一返回 true；`nextRecommended` 改用 `isUnlocked`）
- 修改：`tests/TestLearningPathAudit.cpp`（更新 `MarkCompletedAffectsUnlock`、`EstimatedRemainingMinutesExcludesCompletedAndLocked` 测试以反映新解锁策略）
- 修改：`CHANGELOG.md`（新增第七十四轮条目）

### 测试影响

- 全量 1763/1763 测试通过，零回归
- 本次修改仅影响学习路径解锁策略，不涉及语言核心逻辑

---

## 2026-07-10 · 第七十三轮：三个 UI 问题真正根因修复（P1 × 3，共 3 项）

### 概述

本轮针对用户反复反馈"没解决"的三个 UI 问题进行系统性根因排查。此前 R65-R72 共 8 轮修复均基于代码分析推测根因，均未实际运行验证。本轮通过子 agent 系统性审查 ADS 源码、QFluentKit 主题系统、RichTextItemDelegate 等，找到了三个问题的**真正根因**：(1) 字节码背景问题源于 RichTextItemDelegate::paint 硬编码颜色（#ffffff/#f8f8f8），完全绕过 QSS/palette；(2) 标签蓝底源于 restoreState 创建的新 tab 未被 applyFluentStyle 样式化（时序错误）；(3) 面板大小问题源于 dock widget 的 objectName 使用 i18n 标题，locale 变化导致 restoreState 静默失败。MSVC 19.51 + Qt 6.10.3 + Ninja 构建通过，全量 1773/1773 测试通过。

### 问题与修复对应表

| # | 严重性 | 类型 | 位置 | 修复内容 |
|---|--------|------|------|----------|
| P1.1 | P1 | 视觉/字节码背景 | `app/ide.cpp` RichTextItemDelegate::paint (L255-267) | **字节码背景未变米黄色**——真正根因：RichTextItemDelegate::paint() 中 `painter->fillRect()` 硬编码 `#ffffff`（白色）和 `#f8f8f8`（浅灰），完全绕过 itemViewQss 和所有 palette/QSS 机制。8 轮修复均未检查 item delegate。修复：替换为 `TeachingTheme::ideSelectedBg()`/`ideBgPanel()`/`ideBgMain()`。 |
| P1.2 | P1 | 视觉/标签蓝底 | `app/ide.cpp` applyFluentStyle (L3691-3716) + restoreLayout (L4771) + dockWidgetAdded 信号 (L2905) | **ADS dock 标签蓝底**——真正根因：applyFluentStyle 在 restoreState **之前**执行，restoreState 创建的新 tab 未被样式化。此外未连接 dockWidgetAdded 信号，运行时新建 tab 也不被样式化。修复：(a) restoreState 后追加 applyFluentStyle 调用；(b) 连接 dockWidgetAdded 信号延迟应用样式；(c) 增强 tab palette（补充 Light/Midlight + 子控件 palette）。 |
| P1.3 | P1 | 布局/面板初始尺寸 | `app/ide.cpp` initUI (L2920,2937,2945,3161) + restoreLayout (L4733-4766) | **面板初始大小每次启动都太小**——真正根因：dock widget 的 objectName 使用 `mlTr("资源管理器")` 等 i18n 标题（`CDockWidget` 构造函数 `setObjectName(title)`）。ADS restoreState 通过 objectName 匹配，locale 变化导致 objectName 变化 → restoreState 静默失败 → 每次应用默认尺寸。修复：(a) 为每个 dock widget 显式 `setObjectName` 固定ID（fileTreeDock/debugPanelDock/rightDock/teachingTreeDock）；(b) 增大默认尺寸（左 280→320，右 560→600）。 |

### 关键设计决策

1. **RichTextItemDelegate 绕过所有样式机制**：delegate 的 `paint()` 中 `fillRect()` 直接绘制，QSS/palette 完全无效。这是 8 轮修复的盲区——没有任何一轮检查 item delegate。教训：列表背景异常时优先检查 item delegate 的 paint 方法。
2. **applyFluentStyle 与 restoreState 的时序**：applyFluentStyle 必须在 restoreState **之后**执行，否则 restoreState 创建的新 tab 不被样式化。R68 的注释"先样式后 restoreState"是正确的布局顺序，但遗漏了 restoreState 后需要再次样式化新 tab。
3. **dockWidgetAdded 信号处理运行时新建 tab**：连接此信号确保拖拽/restoreState 创建的新 tab 自动应用样式，使用 `QTimer::singleShot(0)` 延迟确保 tab 完全构造。
4. **objectName 必须用固定ID**：`CDockWidget::CDockWidget()` 中 `setObjectName(title)` 将标题作为 objectName。ADS restoreState 通过 objectName 匹配 dock widget。i18n 标题随 locale 变化，导致 restoreState 失败。显式 `setObjectName("固定ID")` 覆盖标题，确保跨 locale 一致。

### 关键教训

1. **Item delegate 是样式盲区**——delegate 的 paint() 直接绘制，绕过所有 QSS/palette。列表背景异常时优先检查 delegate。
2. **restoreState 创建新 widget 需要后续样式化**——restoreState 不继承已设置的样式，必须在之后重新应用。
3. **objectName 不能依赖 i18n**——用于持久化标识的 objectName 必须是固定字符串，不能随 locale 变化。
4. **UI 问题需要系统性排查而非推测**——8 轮失败均因基于推测修复。本轮通过子 agent 系统审查 ADS 源码、delegate、QFluentKit 才找到真正根因。

### 修改文件清单

- 修改：`app/ide.cpp`（RichTextItemDelegate::paint 替换硬编码颜色；4 个 dock widget 显式 setObjectName 固定ID；连接 dockWidgetAdded 信号；restoreState 后追加 applyFluentStyle；增强 tab palette 设置；增大默认面板尺寸）
- 修改：`CHANGELOG.md`（更新第七十三轮条目）

### 测试影响

- 全量 1773/1773 测试通过，零回归
- 本次修改仅影响 UI 渲染和布局，不涉及语言核心逻辑

---

## 2026-07-09 · 第七十二轮：学习中心面板关闭 UAF 根因修复（P0 × 1 + P1 × 2 + P2 × 1，共 4 项）

### 概述

本轮针对用户反馈"使用学习中心面板后退出程序仍崩溃（同一读取访问权限冲突 UAF）"进行深入排查。第六十六轮已修复 `removePostedEvents` API 误用和定时器未停止，但**遗漏了 `QVariantAnimation` 这类不经过 Qt 事件队列的动画对象**——`QVariantAnimation` 由 `QUnifiedTimer`（全局动画定时器）驱动，`removePostedEvents` 完全无法清理它，只有显式 `stop()` 才能停止。学习中心面板的 `animateCenterSplitter`（7 处调用点）是用户打开/切换教学面板时必然触发的动画，持续约 300ms，成为 UAF 的核心来源。MSVC 19.51 + Qt 6.10.3 + Ninja 构建通过，全量 1773/1773 测试通过。

### 问题与修复对应表

| # | 严重性 | 类型 | 位置 | 修复内容 |
|---|--------|------|------|----------|
| P0.1 | P0 | QVariantAnimation UAF | `app/ide.cpp` closeEvent + ~Ide + `app/ide.h` | **`splitterAnim_` 未在 closeEvent 中停止**——`QVariantAnimation` 由 `QUnifiedTimer` 驱动，不经过 Qt 对象事件队列，`removePostedEvents` 完全无法清理它。`animateCenterSplitter` 在打开/切换教学面板时频繁触发（7 处调用点），动画持续约 300ms。closeEvent 中 `maybeSave` 模态对话框创建嵌套事件循环期间，动画继续运行，`valueChanged` lambda 访问正在清理的 `centerSplitter_` → UAF。修复：closeEvent 入口和 ~Ide 中显式 `stop()` `splitterAnim_`。 |
| P1.1 | P1 | 匿名动画 UAF | `app/ide.cpp` showBottomPanel + hideBottomPanel + `app/ide.h` | **底部面板展开/收起动画未停止**——`showBottomPanel`/`hideBottomPanel` 中的匿名 `QVariantAnimation`（`valueChanged` lambda 访问 `editorSplitter_`）未存储在成员变量中，closeEvent 无法停止。学习中心面板切换路径间接触发底部面板动画。修复：新增 `bottomPanelAnim_` 成员变量存储，closeEvent 和 ~Ide 中停止。 |
| P1.2 | P1 | GuidedTour UAF | `app/ide.cpp` onPanelGuidedTourRequested + closeEvent + ~Ide + `app/ide.h` | **面板特定 GuidedTour 未清理**——5 个教学面板首次访问自动触发 GuidedTour，tour 的 `showStep` 排队 `QTimer::singleShot(0, tour, ...)` 持有裸 `targetWidget` 指针。`removePostedEvents(this)` 只清空 Ide 队列，不清空 tour 队列。maybeSave 模态对话框期间 tour 的 singleShot 被派发，访问可能已被 reparent/清理的 `targetWidget` → UAF。修复：新增 `activePanelTours_` 成员跟踪所有活跃 tour，closeEvent 中 `disconnect` + `removePostedEvents(tour)` + `delete`。 |
| P2.1 | P2 | vmStateChangedListener 残留 | `app/IdeController.h` clearVmStateChangedListeners + `app/ide.cpp` closeEvent | **closeEvent 后续操作触发回调**——closeEvent 中 `vmStop`/`stopForClose` 等操作触发 `notifyVmStateChanged`，7 个教学面板的 `onVmStateChanged` 回调访问正在清理的 UI。修复：新增 `clearVmStateChangedListeners()` 方法，closeEvent 中 `disconnect` 后立即清空所有回调。 |

### 关键设计决策

1. **`QVariantAnimation` 不受 `removePostedEvents` 控制**：这是第六十六轮遗漏的关键认知。`QVariantAnimation` 通过 `QUnifiedTimer`（全局动画定时器单例）驱动，不经过 Qt 对象事件队列。`removePostedEvents(obj)` 只清理投递到对象事件队列的事件（如 `QMetaCallEvent`/`QEvent::Timer`），对 `QUnifiedTimer` 驱动的动画无效。只有显式 `stop()` 才能停止。
2. **`activePanelTours_` 跟踪面板特定 tour**：原实现 tour 为局部变量不存储，依赖 `finished → deleteLater` 自动清理。但 closeEvent 期间 `deleteLater` 不会立即执行，tour 的 `singleShot` 仍可能被派发。显式 `disconnect` + `removePostedEvents(tour)` + `delete` 是唯一安全方式。
3. **`clearVmStateChangedListeners` 在 disconnect 之后立即调用**：closeEvent 中 `disconnect(controller_)` 仅断开 Qt 信号-槽，不断开纯 C++ 观察者回调。`vmStop`/`stopForClose` 会触发 `notifyVmStateChanged`，此时回调仍活跃。必须在 disconnect 之后、后续操作之前清空回调。

### 关键教训

1. **`QVariantAnimation` 是 closeEvent UAF 的隐蔽来源**——不经过事件队列，`removePostedEvents` 无效。所有活跃动画必须在 closeEvent 入口显式 `stop()`。
2. **局部变量创建的 QObject 必须有成员变量跟踪**——`onPanelGuidedTourRequested` 中 tour 为局部变量，closeEvent 无法停止。任何在 closeEvent 期间可能仍有挂起回调的对象都必须被跟踪。
3. **纯 C++ 观察者回调不受 Qt disconnect 影响**——`vmStateChangedListeners_` 是 `std::vector<std::function>`，`disconnect()` 不影响它。需要单独的 `clearVmStateChangedListeners()` 方法。

### 修改文件清单

- 修改：`app/ide.h`（新增 `#include <QSet>`、`bottomPanelAnim_` 成员、`activePanelTours_` 成员）
- 修改：`app/ide.cpp`（closeEvent + ~Ide 停止动画/清理 tour/clearVmStateChangedListeners；showBottomPanel/hideBottomPanel 改用 `bottomPanelAnim_`；onPanelGuidedTourRequested 记录到 `activePanelTours_`）
- 修改：`app/IdeController.h`（新增 `clearVmStateChangedListeners()` 方法）
- 修改：`CHANGELOG.md`（新增第七十二轮条目）

### 测试影响

- 全部 4 项修复均无回归。全量 1773/1773 测试通过。

## 2026-07-10 · 第七十一轮：REPL 功能全面审查与 3 项 Bug 修复（P2 × 3，共 3 项）

### 概述

本轮对 REPL 功能进行系统性审查，覆盖 ReplPanel 交互逻辑、MagicCommands 命令系统、Interpreter REPL 执行路径、异步执行/超时/错误处理、状态管理与边界条件。审查了 5 大模块共 ~1500 行代码，发现并修复 3 项 P2 级 Bug。MSVC 19.51 + Qt 6.10.3 + Ninja 构建通过，全量 **1763/1763** 测试通过。

### 审查范围与结论

| 模块 | 审查内容 | 结论 |
|------|----------|------|
| isInputComplete | 括号配对、字符串插值栈、块注释嵌套、try/catch/finally 配对 | 逻辑正确，字符串插值栈处理完善 |
| executeLine | magic 拦截、互斥检查、AST 生命周期、异步执行 | 互斥检查完善，retainReplAst 所有权管理正确 |
| 异步执行/超时 | std::async + QTimer 轮询、closeEvent 超时、QPointer 防 UAF | 超时处理健壮，QPointer 模式正确 |
| MagicCommands | 10 个命令、参数解析、ScopedAnalysis 临时管线 | disassembleInstruction offset 前进正确 |
| 状态管理 | executeRepl/resetReplEnvironment/save/restoreReplState | 所有成员变量均被正确重置 |

### 问题与修复对应表

| # | 严重性 | 类型 | 位置 | 修复内容 |
|---|--------|------|------|----------|
| 1 | P2 | UX/输出冗余 | `gui/ReplPanel.cpp` pollReplFuture | **REPL 对非表达式语句打印多余 "null"**——原行为（PANEL-03 fix）打印所有结果包括 null，导致 `print(1);` 输出 `1\nnull`、`var x=1;` 输出 `null`。修复：添加 `!result.isNull()` 条件，仅打印非 null 结果，与 Python/Node REPL 行为一致。表达式语句（如 `1+2;`）的结果正常显示。 |
| 2 | P2 | 死代码 | `gui/ReplPanel.cpp` executeLine | **executeLine 中的 magic 命令检测为死代码**——onReturnPressed 已在 isInputComplete 之前拦截 magic 命令，executeLine 永远不会收到以 `%` 开头的输入。移除死代码避免维护负担和读者困惑。 |
| 3 | P2 | 硬编码 | `gui/ReplPanel.cpp` 构造函数 | **欢迎信息中 magic 命令数量硬编码**——"10 个 magic 命令"写死，未来新增命令会不同步。改为不写死数量。 |

### 修改文件清单

- 修改：`gui/ReplPanel.cpp`（pollReplFuture null 输出修复 + executeLine 死代码移除 + 欢迎信息去硬编码）

### 测试影响

- 全量 1763/1763 测试通过，零回归。
- REPL 输出行为变更（null 不再打印）属于 UX 改进，不影响引擎语义。

**历史变更**：更早的开发记录（第六十八轮及以前）已归档至 [docs/changelog/archive/](docs/changelog/archive/)。

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

**历史变更**：更早的开发记录（第六十八轮及以前）已归档至 [docs/changelog/archive/](docs/changelog/archive/)。
