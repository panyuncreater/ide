# MiniLang 内存管理约定（Spec）

> 本文档定义 MiniLang 运行时内存管理的**强制约定**：新增堆类型时必须做什么、COW 何时生效、GC 如何交互、审计时查什么。
> 原理性描述见 `docs/adr/ADR-001-nan-boxing.md`、`docs/adr/ADR-004-cow-memory.md`；代码事实源：`interpreter/Value.h`、`interpreter/RefCounted.h`、`interpreter/GcManager.h`。

## 1. 值表示

- 所有运行值以 `Value` 表示，NaN-boxing 8 字节（`interpreter/Value.h`，见 ADR-001）。
- **约定**：新增运行时类型时，先在 `Value` 的类型标签枚举中登记并实现编解码路径，禁止用裸指针/裸标记绕过 `Value`。
- 整数/浮点/布尔/字符串/闭包等基础类型均有既定编码，新类型必须复用既有标签或显式申请新标签。

## 2. 堆类型生命周期（RefCounted）

堆类型通过侵入式 `RefCounted` 基类管理引用计数（`interpreter/RefCounted.h`）。

**内存序约定（2026-10-04 起）**：`release()` 使用 shared_ptr 同款惯用法——`fetch_sub(1, memory_order_release)` + 归零时 `atomic_thread_fence(memory_order_acquire)` 后 `delete this`（非归零递减省去 acquire 半序，堆值密集代码每次少一次强同步）。**勿"修复"回 `acq_rel`**：归零路径的 acquire fence 已覆盖跨线程析构可见性语义。
**新增堆类型强制 checklist**：
1. 继承 `RefCounted`，实现 `release`/减引用语义，禁止手工 `delete` 裸 `Value` 持有者。
2. 检查是否需要被 `GcManager` 追踪（见 §3）；`Value.h` 中 `friend class GcManager` 表明 GC 可访问内部结构，新类型如参与循环引用检测须配合 GC 遍历接口。
3. 析构链中不得访问已释放的兄弟节点（顺序依赖必须注释说明）。
4. 所有权转移点（函数边界、容器插入、闭包捕获）必须显式标注 `shared_ptr`/侵入式计数归属。

## 3. GcManager（mark-sweep 循环检测）

- `interpreter/GcManager.h`：mark-sweep 回收 RefCounted 无法处理的循环引用；`Interpreter` 构造时注册增量 GC 触发回调（`interpreter/Interpreter.cpp`：`registerTracked` 累计到阈值触发）。
- **约定**：
  - `Interpreter` 析构必须清除 GcManager 单例中持有 `this` 的回调，避免悬垂指针（`Interpreter.cpp` R157 fix 即此模式，新加回调必须成对清理）。
  - 被追踪对象注册/注销必须与构造/析构配对；任何「先于析构注销」的路径缺失都会导致 UAF。
  - GC 触发点是增量式的（阈值驱动），**不得假设 GC 只在特定时机运行**——持引用跨 GC 边界的代码必须自行保住引用计数。
  - **根集收集零拷贝（2026-10-04 起）**：`Interpreter::triggerIncrementalGc` 与 execute 入口兜底 GC 只读遍历 `Environment::localVariables()`（`collectEnvLocalGcRoots`）取 `gcRootPtr()`，并按 `visitedEnvs` 对重叠环境链去重——**禁止改回 `snapshotLocalVariables()` 深拷贝模式**（每次触发 O(可见变量数) 次堆分配 + 2N 原子操作）。
  - **统计字段为 `std::atomic`（2026-10-04 起）**：`allocationsSinceLastGc_`/`currentPhase_`/`lastMarkedCount_`/`lastCollectedCount_`/`totalGcCount_`/`gcAllocationThreshold_`/`pendingIncrementalGc_` 均为 atomic，写入点在锁内、只读访问（GUI 轮询）无锁；`checkPendingGc` 走无锁快速路径（relaxed 读 + 慢路径锁内复查）。新增统计字段沿用此模式。
  - **登记表为 slots_ 槽位表（2026-10-06 起）**：GcManager 以单一 `slots_` 槽位表取代原 `tracked_` vector + `aliveSet_` unordered_set 双结构——不变量（全局 `recursive_mutex` 内）：非空槽位 ⟺ 槽位被 `gcSlot_==i` 的存活对象占用，nullptr 为已析构墓碑，锁内无悬垂指针；死亡槽位经 `freeSlots_` 复用（LIFO），`trackedCount()` 为 O(1) 精确存活数。对象侧 `RefCounted::gcSlot_` 仅在锁内读写（registerTracked 回写 / Phase 3 压缩回写 / onDestroyed 读取）。**约定**：新增跟踪类型照常「构造函数 `registerTracked` + 基类析构钩子注销」配对，禁止绕过 `onDestroyed` 手工操作槽位（onDestroyed 以 `slots_[gcSlot_]==obj` 指针身份卫兵回收，手工绕过会破坏身份不变量导致槽位复用冲突）；全局锁纪律（分配/析构/collectCycle 全程持锁）不变——分配路径的 thread_local 批量登记去锁化已评估为需 TSan 通道专验，未实施（见优化积压记录）。
  - mark 阶段整个 `collectCycle` 共享一个 worklist（按引用传入 `markValue`），不得在每个根元素内新建。

## 4. Copy-On-Write（COW）

数组/字典写前检查独占所有权（见 ADR-004）。

- **约定**：修改容器元素前必须走「独占检查 → 需要时深拷贝」路径；直接写共享缓冲是数据竞争/别名错误。
- 新增可变容器 API 时，写路径必须复用既有 COW 检查，禁止绕过。
- 审计点：所有 `visit*`/内建方法的写路径逐一核对 COW 检查存在。

## 5. 栈与帧约束

- VM 操作数栈：`VMStack` 定长 1024 元素 + 栈顶指针，越界显式 abort（**禁止**改为静默回绕/增长）。
- 寄存器帧：`std::array<Value, 32>` 零堆分配，32 虚拟寄存器 R0-R31（`docs/architecture.md` L63-64）。
- **约定**：新增执行路径不得引入无界/变长栈分配；递归深度有 RuntimeLimits 兜底（`common/RuntimeLimits.h`）。

## 6. UAF 高危点清单（审计必查）

| 高危点 | 风险形态 | 检查方式 |
|--------|---------|---------|
| 闭包 upvalue | 闭包逃逸定义作用域后捕获变量被释放 | `compiler/backend-stack/VM.cpp` OP_CLOSURE/upvalue 描述符生命周期；`compiler/backend-reg/RegisterVM.cpp` 对应 RegOp |
| 条件断点 | 断点对象持引用失效（slot→name 映射） | `tests/TestVME2E.cpp` VMConditionalBreakpoint 套件；断点销毁路径 |
| 回调持引用 | `this` 泄漏进单例/回调（GC 回调、输出回调） | 构造注册 vs 析构清理配对审计 |
| Environment/boundInstance_ | 链式作用域中缓存实例释放 | `interpreter/Environment.h` 释放顺序；缓存失效路径 |
| envPool_ 回收 × 闭包 env 弱引用 | `resetForReuse` 清空 weak_ptr 仍指向该 env 的闭包的可见变量（陈旧读/语义损坏） | 不变量已收口到 `Value::makeClosure`（D2 fix，2026-10-03）：工厂内统一 `markClosureEnvRef()`，回收守卫（use_count==1 + 无捕获 + 无闭包 env 引用）不再依赖各创建点手工标记；回归测试 `AuditBatch1ClosureEnv.*` |
| JIT 上下文 | `JitContext` 邮箱持有跨线程引用 | `compiler/JIT*.cpp` 上下文生命周期与 VM 停靠同步 |

## 7. 内存审计清单（配合 AGENTS.md §5 策略 4）

1. 每个 RAII guard / shared_ptr / raw pointer 的获取与释放是否配对（含异常路径）。
2. 新增 `RefCounted` 子类是否走完 §2 checklist。
3. 新增回调是否在析构中成对清理（§3 约定）。
4. COW 写路径是否全部经过独占检查（§4）。
5. 栈/帧分配是否符合 §5 约束（无界分配禁止）。
6. 修改头文件 struct 布局后，必须全量重编译验证（SEH 0xc0000005 风险，`docs/getting-started.md` 构建问题排查）。
