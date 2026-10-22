# 解释器与运行时性能审计（interpreter/ · Value · GC · Environment）

- 日期：2026-10-22 ｜ 模式：**只读审计**（未修改任何产品代码，未运行任何构建/测试/GUI 命令）
- 范围：`interpreter/` 全目录（Interpreter.cpp、InterpreterCalls.cpp、InterpreterClasses.cpp、InterpreterCoroutine.cpp、InterpreterModules.cpp、BuiltinMethods.cpp）、`interpreter/Value.h/.cpp`、`GcManager.h/.cpp`、`Environment.h`、`common/SmallMap.h`、`ast/ASTNode.cpp`（StringLiteral）
- 方法：逐函数代码审读 + 热路径分配/拷贝追踪 + 调用链推演。**所有收益估计均为定性推理，未经基准实测，一律标注「待证（未实测）」**。
- 纪律：不重复报告近期已修复项（字典透明查找 dictFindFieldRef/dictGetOrInsertFieldRef、NaN 键 SameValueZero、协程快照 gcRootContributor、isTypeParameter 双事实源）。
- 修复建议均以「不改变 Interpreter 与 StackVM/RegisterVM/JIT 可观测语义」为约束（AGENTS.md 四后端一致性）。

## 发现总览

| # | 位置（热路径） | 类型 | 预期收益 | 风险 | 代价 |
|---|---|---|---|---|---|
| 1 | visitMethodCall / writeBack / collectAndEvaluateChain | COW 伪共享 → 容器变异全量克隆 | 高 | 零 | 小改 |
| 2 | visitVarDecl/visitAssignment/调用路径 debugger() | 每语句/每调用 mutex+引用计数 | 高 | 零 | 小改 |
| 3 | StringLiteral::getValue | 字面量每次求值堆分配 | 高 | 零 | 小改 |
| 4 | callClosureValue/invokeMethod/invokeClosureSync | 函数 env 无池化 | 中 | 低 | 需设计 |
| 5 | GcManager markValue/collectCycle | mark 阶段 unordered_set 哈希 | 中 | 零 | 小-中 |
| 6 | collectAndEvaluateChain | 每次链求值 3 次 vector 分配 | 中 | 零 | 小-中 |
| 7 | GcManager registerTracked/onDestroyed | 每容器分配 2 遍 recursive_mutex | 中 | 低 | 中 |
| 8 | BuiltinMethods 注册表 find | 内建每次调用哈希查表 | 中-低 | 零 | 小 |
| 9 | invokeMethod 帧名拼接 | 每方法调用字符串拼接 | 低 | 零 | 小 |
| 10 | evaluateArguments/mergedTypeParams | 调用参数临时 vector | 低 | 零 | 需设计 |

---

## 发现 1（高）：COW 伪共享——vals 拷贝使每次容器元素写/可变方法调用都全量克隆

**位置**
- `Interpreter.cpp : collectAndEvaluateChain : ~1443-1470`：`info.vals[n-1] = *baseVal`（Value 拷贝，与 env 槽位共享同一堆对象 → rc≥2）
- `Interpreter.cpp : visitMethodCall : ~3528-3571`：`obj = std::move(info.vals[0])` 后调 `BuiltinMethods`
- `BuiltinMethods.cpp : ~1499`（ARR_PUSH 等可变方法）：`obj.arrayVal().push_back(args[0])` 走非 const accessor
- `Interpreter.cpp : writeBack : ~1599-1653`：`modifiedObj.arrayVal()[idx] = val`（visitIndexAssign ~3513 路径）、成员赋值 `modifiedObj.fields()[fieldName] = val`
- 克隆点：`Value.h : ensureUnique : ~580-604`（rc>1 → `make_unique<T>(*ptr)` 全量深拷贝 + `GcManager::registerTracked` 递归锁）

**类型与预期收益**：热路径分配 churn（最高价值项）。收益：**高**——「循环 push 建数组」从 O(N²) 降为 O(N)（每元素一次全容器深拷贝 + N 次 addRef + GC 登记锁 → 每元素摊还 O(1)）；`a[i]=v`、`d[k]=v`、`obj.f=v`、`arr.push/insert/pop`、`dict.set`、字符串拼接方法同理。**待证（未实测）**，定性依据：每次变异的深拷贝成本与容器尺寸成正比，是解释器中数量级最大的单点分配源。

**根因**：`collectAndEvaluateChain` 将链根变量的 Value **按值**拷入 `info.vals`，导致随后任何变异前的 `ensureUnique` 因 rc=2 必然触发克隆——即使程序中根本不存在别名共享。COW 本应只在「真实共享」时付出克隆代价，此实现把它变成「每次变异无条件克隆」。变异结果再经 `writeBackChain` → `currentEnv_->set(...)` 写回（写回值又是克隆后的新对象）。

**具体建议**：增加单节点快路径：当链根为直接 `VarRef`（`idxs.empty()`，n==1，最常见形态）时，跳过 vals 拷贝与 writeBackChain——在 RHS/索引/参数求值**完成后**，直接经 `currentEnv_->findMutableSlot(name)`（或 get 返回内部 Value*）对 env 槽位内的 Value 原位变异（`arrayVal()` 非 const accessor 此时 rc==1，不触发克隆）。多节点链（`a[i][j]`、`obj.arr.push`）可先只做 n==1，或对中间层逐级应用同一原则。注意：
- 快路径须在求值后取槽位指针（RHS 求值可能触发 GC/用户代码改链）；
- boundInstance 字段回退解析的槽位指向 InstanceData::fields 内 Value，同样适用（rc==1 时原位变异）；
- 真别名场景（`b = a` 后经 b 变异，或闭包/快照持引用）rc>1，ensureUnique 仍克隆，语义完全保留。

**行为风险**：零行为变化。可观测语义（值、别名隔离、GC 可见性）与现实现一致：非别名路径现克隆后写回等价于原位变异；别名路径不受影响。COW 对外承诺（写时复制隔离）不变。

**实施代价**：小改（writeBack + visitMethodCall 两处快路径判断；findMutableSlot 可由现有 Environment::get/getPtr 变体承担）。

---

## 发现 2（高）：每次赋值/声明/函数调用无条件 `debugger()`——mutex 锁 + shared_ptr 引用计数

**位置**
- `Interpreter.h : debugger() : ~693`：`std::lock_guard + return debugger_;`（shared_ptr 拷贝 = 1 对原子 inc/dec）
- 无条件调用点：`Interpreter.cpp : visitVarDecl : ~1816`、`visitAssignment : ~2011`、`visitIndexAssign : ~3519`、`visitThrowStmt : ~2573`；`InterpreterCalls.cpp : callClosureValue : ~158`、`callNamedFunction : ~916`；`InterpreterClasses.cpp : constructClassInstance : ~157`

**类型与预期收益**：每语句/每函数调用的同步原语开销。收益：**高**（相对其改动量）——常规 Run（无调试器）路径上，每个赋值/声明/调用节点固定付出 1 次 mutex lock/unlock + 2 次原子引用计数操作；解释器每秒执行数十万语句级节点时累积显著。**待证（未实测）**，定性依据：这些调用位于 AST 访问者最内层，且结果仅用于 `hasWatchpoints()` / `checkFunctionBreakpoint()` 判断。

**根因**：`checkBreak`（`Interpreter.cpp:929`）已建立「debugMode_ 原子短路」先例，但 debugger() 直接取用路径未跟进；`debugger_` 为 shared_ptr 成员，attach 状态在运行期几乎不变，却每次取用都走锁。

**具体建议**：Interpreter 增加 `std::atomic<bool> debuggerAttached_{false}`，`setDebugger`（attach/detach）时置位/清零；上述各点改为 `if (debuggerAttached_.load(std::memory_order_relaxed))` 才取 debugger()。或进一步：把 watchpoint 判断本身收敛进 checkBreak 同款短路。

**行为风险**：零行为变化。flag 只反映 `debugger_ != nullptr` 这一既有事实；attach 后行为逐字节一致（含调试面板/观察点/函数断点）。

**实施代价**：小改（一个原子成员 + ~7 处调用点包裹）。

---

## 发现 3（高）：字符串字面量每次求值都堆分配 StringData

**位置**
- `ast/ASTNode.cpp : StringLiteral::getValue : ~25`：`return Value(value);` → `Value(const std::string&)`（`Value.h:638`）→ `new StringData`
- 触发点：`Interpreter.cpp : visitStringLiteral : ~1777`（`lastValue_ = node.getValue()`）

**类型与预期收益**：热路径分配 churn。收益：**高**——循环体内的字符串字面量（`while ... print("tick")`、`s = s + ","`、`a.push("x")`）每次迭代 1-2 次 `new StringData` + 字符串深拷贝，字符串越长拷贝越贵。**待证（未实测）**，定性依据：字面量求值是仅次于数值与变量的最高频节点之一。

**根因**：`StringLiteral` 节点持有 `std::string value`，`getValue()` 每次从它构造新 Value（新堆对象）。AST 节点不可变且生命周期覆盖整个运行，天然适合缓存。

**具体建议**：`StringLiteral` 增加缓存字段（`mutable Value cached_; mutable bool cached_{false};`——需保持 getValue() const），首次求值构造后缓存，后续返回缓存 Value 的拷贝（一次原子 addRef）。COW 保证安全：任何对返回值的写操作都会因 rc>1 克隆，不会污染缓存。附带收益：相同字面量的 equals 比较可走「同 StringData 指针快速返回」路径（若 Value::equals 已有指针短路则直接受益）。

**行为风险**：零行为变化。字符串无对象身份语义（`==` 比较内容），共享同一 StringData 不可观测（COW 变异隔离）。

**实施代价**：小改（单文件单类；注意 Interpreter 与 Compiler/IR/JIT 共用 AST，getValue() 语义不变，各路径同样受益）。

---

## 发现 4（中）：函数/方法/回调 Environment 每次 make_shared，无池化

**位置**
- `InterpreterCalls.cpp : callClosureValue : ~180`、`callNamedFunction : ~940`：每次调用 `std::make_shared<Environment>(...)`
- `Interpreter.cpp : invokeMethod : ~3845`（方法调用，且 `bindInstance` → `Environment.h : ensureHeapStorage` **强制晋升 unordered_map**）、`invokeClosureSync : ~453`（map/filter/forEach 每元素一次）

**类型与预期收益**：每调用容器分配 churn。收益：**中**——每次函数/方法调用 1 次 Environment 堆分配（~700B 量级，含 SmallMap 内联区 + typeAnnotations_ SmallMap + capturedVarNames_ unordered_set 等成员构造），方法调用额外 1 次 unordered_map 桶数组分配；map/filter 循环中每元素触发。已有先例：`visitBlock`（Interpreter.cpp:2804）/`visitForStmt`（2091）用 `envPool_` 复用。**待证（未实测）**。

**根因**：调用路径未接入 envPool_ 复用机制；且 `SmallMap::clear()`（`common/SmallMap.h:329`）为 `heap_.reset()`——销毁堆存储降级内联，池化复用时方法 env 的堆桶会被反复销毁重建，抵消收益。

**具体建议**：调用返回时若 env `use_count()==1` 且无闭包捕获引用（无 hasClosureEnvRef/快照持有）则归还池；`Environment`/`SmallMap` 增加「保留堆存储」的 clear 变体（`keepHeapClear()`：逐元素清空但保留桶数组）。需审计协程挂起快照与 upvalue 对 env 的持有路径（持有者会令 use_count>1，天然不归还，安全）。

**行为风险**：低。沿用 visitBlock 池化同一不变量；风险点仅在快照/upvalue 生命周期，需按现有 envPool_ 的守卫条件同构处理。

**实施代价**：需设计（SmallMap 新变体 + 四个调用点 + 生命周期审计）。

---

## 发现 5（中）：GC mark 阶段以 unordered_set 做去重，每对象哈希查找

**位置**
- `GcManager.cpp : markValue : ~165-280`：worklist 迭代式标记，每对象 `marked.insert(obj)`、每次子节点遍历 `marked.find(...)`
- `GcManager.cpp : collectCycle : ~340`（Phase 2/3）：每对象两遍 `marked.find`

**类型与预期收益**：GC mark 成本。收益：**中**——每轮 GC 对 N 个存活/扫描对象做约 2-3N 次 unordered_set 哈希（每对象哈希指针 + 桶定位）；GC 阈值 8192、含大对象图时每轮数千次哈希。**待证（未实测）**，定性依据：哈希常数（取指针值、乘混编、桶探测）远高于一次成员位测试。

**根因**：用集合容器模拟「已标记」状态，而 RefCounted 对象本身有稳定身份，标记位完全可以内联。

**具体建议**：`RefCounted` 增加侵入式 `gcMarked_` 位（或复用 gcSlot_ 高位做一个 bitmap），mark 置位、find 变位测试、sweep 后统一清零；未登记 GC 的类型（String/Closure/Coroutine——mark 需覆盖 Closure/Coroutine 防环）同样适用该位，防环终止判断照常工作。现有 worklist 迭代式结构不动。

**行为风险**：零行为变化（纯内部数据结构替换，标记集合语义等价）。

**实施代价**：小-中（RefCounted 加字段 + GcManager 三处替换 + sweep 清零点）。

---

## 发现 6（中）：每次链求值固定 3 次 vector 堆分配

**位置**
- `Interpreter.cpp : collectAndEvaluateChain : ~1443-1455`：`chain.reserve(8)`、`vals.reserve(8)`、`idxs.reserve(8)` 三个 vector，即使最常见单节点链（`a.push(x)`、`a[i]=v`）也全额分配
- `InterpreterCalls.cpp`（collectAndEvaluateChain 的调用侧重载）：`vals[n-1] = *baseVal` 等拷贝伴随分配

**类型与预期收益**：每方法调用/复合赋值/索引访问的固定分配 churn。收益：**中**——与发现 1、2 同在最高频路径；每节点 3 次 malloc/free。**待证（未实测）**。

**根因**：ChainInfo 一律走堆 vector，未利用「绝大多数链 ≤ 2 节点」的分布事实。

**具体建议**：与发现 1 的 n==1 快路径合并解决（快路径完全不构造 vals/idxs）；对一般情况可给 ChainInfo 加固定容量内联缓冲（`SmallVector` 风格，8 节点内零堆分配，超出回退堆）。

**行为风险**：零行为变化。

**实施代价**：小-中（快路径小改；内联缓冲需引入/复用小缓冲容器）。

---

## 发现 7（中）：容器对象分配/析构每次取全局 recursive_mutex（各一遍）

**位置**
- `GcManager.cpp : registerTracked : ~49-70`：每次数组/字典/实例/元组/枚举变体创建 1 次 `recursive_mutex` lock/unlock
- `GcManager.cpp : onDestroyed : ~131`：每次销毁再 1 次；COW 克隆（ensureUnique 内 registerTracked）额外加 1 次

**类型与预期收益**：每容器生命周期的同步开销。收益：**中**——与发现 1 联动：修复伪共享前，每次变异克隆都付 1 次锁；修复后仍是每容器创建/销毁各 1 次。**待证（未实测）**，定性依据：recursive_mutex 加解锁比 std::mutex 更贵，而解释器主循环是单线程。

**根因**：锁同时保护登记表与统计；recursive 语义仅为「collectCycle 持锁期间 onDestroyed 重入」这一条路径服务。

**具体建议**：改 `std::mutex` + 显式重入守卫（collectCycle 调 onDestroyed 时传「已持锁」标记），或 thread_local 深度计数；GC 统计已是 atomic 无锁（2026-10-04 收口），不受影响。与发现 1 合并后锁频次大幅下降，此项可排后。

**行为风险**：低（需完整枚举重入路径：collectCycle → ~析构 → onDestroyed；GUI 侧统计读取走 atomic 不入锁）。

**实施代价**：中。

---

## 发现 8（中-低）：内建函数/方法每次调用 unordered_map 哈希查表

**位置**
- `BuiltinMethods.cpp : ~960`：`builtinFunctionRegistry.find(funcName)`（len/str/abs/push…每次 FunCall）
- `BuiltinMethods.cpp : ~1534 / ~1592 / ~1613`：方法注册表 `find(method)`（每次 MethodCall）

**类型与预期收益**：内建调度哈希成本。收益：**中-低**——字符串哈希每次重算（节点名 std::string 现算），但 `classifyBuiltinMethod`（BuiltinMethods.cpp:31-95，枚举分发）已存在而 Interpreter 主路径未使用，说明表查找本可消除。**待证（未实测）**。

**根因**：注册表是编译期常量（`const`，运行期不变），但查找结果未缓存在 AST 节点，也未转枚举 switch。

**具体建议**：AST FunCall/MethodCall 节点首次解析后缓存命中结果（枚举/指针，静态表不变故安全）；或调度改为 classify 枚举 + switch。注意与发现 3 同理：AST 为四路径共用，各通道均受益（JIT 走自身 helper，不受影响）。

**行为风险**：零行为变化（同名常驻、表不可变）。

**实施代价**：小改。

---

## 发现 9（低）：方法调用帧名每次拼接字符串

**位置**
- `Interpreter.cpp : invokeMethod : ~3883`：`callStack_.emplace_back(obj.className() + "." + node.methodName, ...)`——每次方法调用一次 string 拼接（多数超 SSO 长度 → 堆分配）
- `InterpreterCalls.cpp : callClosureValue/callNamedFunction`：`effectiveName` 为 `closureName()` 值拷贝；`CallFrame.h` 帧内存 `std::string functionName`

**类型与预期收益**：热路径字符串 churn。收益：**低**——单次拼接便宜，但位于每次方法调用；仅用于调试/错误显示。**待证（未实测）**。

**根因**：CallFrame 按值存拼接结果；项目已有 `StringIntern` 驻留池未用于帧名。

**具体建议**：帧名驻留（intern 后存指针/句柄），或方法帧延迟拼接（仅在取帧名显示时构造）。

**行为风险**：零行为变化（显示内容一致）。

**实施代价**：小改（若改 CallFrame 存储形态则需设计）。

---

## 发现 10（低）：调用参数与泛型上下文的临时 vector 拷贝

**位置**
- `InterpreterCalls.cpp : evaluateArguments : ~1208`：每次调用返回 `std::vector<Value>`（新建+析构）
- `Interpreter.cpp : invokeMethod : ~3777`：`mergedTypeParams`（外层∪自身）每次调用完整拷贝构建，非泛型调用也在路径上

**类型与预期收益**：每调用容器 churn。收益：**低**——无参/少参调用多数走 SSO/空 vector（libstdc++/MSVC 空 vector 不分配），泛型合并仅泛型上下文付出。**待证（未实测）**。

**根因**：按值返回的 vector 依赖 RVO/move，但每次调用仍经历构造/析构与容量管理。

**具体建议**：调用点复用调用方栈上缓冲（out-param 或 SmallVector<Value, 8>）；mergedTypeParams 在非泛型时走零拷贝空上下文常量。

**行为风险**：零行为变化。

**实施代价**：需设计（签名调整面较宽）。

---

## 已审读且确认状态良好（不作为发现）

- `numericBinaryOp`（Value 原地 append、PERF-06 已优化）、`visitBinaryOp` move 语义、`compareNumericOrString` 模板化。
- `checkBreak` 快路径（stopRequested_ acquire + recordingEnabled_ relaxed + debugMode_ 短路）、`checkPendingGc` 每语句 atomic 快路径。
- GC：slot-index 登记表（2026-10-06 重构）、markValue worklist 迭代式防栈溢出、统计 atomic 无锁化、阈值触发模型本身合理。
- `StringIntern` 驻留池、`to_chars` 数字格式化（Perf-Finding2）、`InterpolatedString`/`visitPrintStmt` reserve（PERF-11）。
- `evaluate` 主分发、`visitIndexAccess` 字典透明查找（近期已修复项，按纪律不重复报告）。
- 协程（InterpreterCoroutine.cpp，Windows fiber 真挂起）：suspendedCallStack move 语义、fiber 切换路径无显著 churn；快照 GC 根集回调为近期已修项不报。

## 复核与限制说明

- 发现 1 的调用链（vals 拷贝 → ensureUnique rc>1 → 全量克隆）经三轮复核（collectAndEvaluateChain 两个重载、writeBack 两个分支、BuiltinMethods push/dict.set/成员赋值）。
- 全部收益为静态推演，**未运行任何基准**（任务纪律：禁止构建/测试）；建议修复前用 `minilang_perf_test` 现有基准建立基线，修复后同口径对比。
- 未覆盖（时间盒外，建议后续）：`InterpreterModules.cpp` 导入缓存路径、`Value::equals` 深比较的逐字符路径细节、`checkType` 完整注解递归成本、JIT 与 VM 通道的同类 churn（超出本次 interpreter 范围）。
