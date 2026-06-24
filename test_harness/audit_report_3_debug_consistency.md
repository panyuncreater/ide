## 专项3：完整运行与单步调试一致性校验报告

### 审计目标

验证 MiniLang 调试器（DebugController + Interpreter）在完整运行与单步调试两种模式下的语义一致性，确保调试面板呈现的变量值、调用栈与解释器内部状态完全同步。

### 测试设计

共设计 46 个测试用例，分为 6 组：

**主测试组（40 例，A/B 对照）：** 每个用例分别以"完整运行"和"Step Over 逐步执行"两种方式运行，比较最终输出和变量终值是否一致。

- BR1–BR8（分支）：if/else、嵌套 if、else-if 链、dangling else、truthy 值、循环内条件、条件赋值
- LP1–LP8（循环）：while 基础/求和、for 基础/求和、嵌套 for、倒计时、函数内 for+return、字符串累积
- FC1–FC8（函数调用）：基础函数、无返回值、嵌套调用、多参数、函数作表达式、void 副作用、类型参数、互递归
- RC1–RC8（递归）：阶乘、斐波那契、递归求和、幂运算、GCD、深度 10、树形递归、字符串递归拼接
- NS1–NS8（嵌套作用域）：块级遮蔽、嵌套遮蔽、函数局部作用域、循环变量作用域、嵌套函数调用、同名变量不同函数、深层嵌套、作用域内赋值

**专项验证组（6 例）：**

- SI1：Step-In 进入函数 — 验证 Step-In 比 Step-Over 产生更多暂停点，且 maxDepth >= 1
- SI2：Step-In 嵌套深度 — 验证 `a()→b()` 调用链中 maxDepth >= 2
- SO1：Step-Out 从深度 0 — 验证直接运行到结束，输出正确
- SO2：Step-Out 调用链一致性 — 验证 `outer()→inner()` 链的输出与完整运行一致
- CS1：递归调用栈深度 — 验证 `fact(5)` 的 Step-In 过程中 maxDepth >= 4
- CS2：变量快照捕获局部变量 — 验证暂停时变量回调能捕获函数内部的 x、y 局部变量

### 测试结果

```
RESULTS: 46 passed, 0 failed, 0 skipped, 46 total
```

所有 40 个主测试用例的完整运行与 Step Over 调试输出完全一致，变量终值匹配。6 个专项验证用例全部通过。

### 发现并修复的 Bug

**BUG-DBG-1：Step-In 模式在递归函数中丢失暂停（严重）**

- 位置：`debug/DebugController.cpp:76-81`（`checkBreak()` 的 `MODE_STEP_IN` 分支）
- 根因：Step-In 暂停条件仅判断 `node->line != lastPausedLine_`。递归函数每次递归重入同一源码行时，`lastPausedLine_` 已等于当前行号，导致不再暂停。用户在调试 `fact(5)` 时只能看到第一次进入，后续 4 次递归调用全部跳过。
- 修复：新增 `lastPausedDepth_` 成员变量，暂停条件改为 `line != lastPausedLine_ || currentDepth_ != lastPausedDepth_`，使同一行在不同调用深度时仍触发暂停。
- 修改文件：`debug/DebugController.h`（新增成员）、`debug/DebugController.cpp`（checkBreak 条件 + pause 赋值 + reset 清理共 3 处）
- 验证：修复后 CS1 测试 maxDepth 从 1 升至 5，符合 `fact(5)` 的 5 层递归预期。

### 结论

调试器与解释器的状态同步机制在修复 BUG-DBG-1 后工作正常。完整运行与 Step Over 单步执行在所有测试场景下输出一致，Step-In/Step-Out 的调用栈深度追踪正确，变量快照回调能正确反映解释器的环境链状态。
