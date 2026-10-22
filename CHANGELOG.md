# Changelog

本文件记录 MiniLang IDE 的版本演进，遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/) 规范，分类说明每版本对用户可见的变更：**Added**（新增功能）/ **Changed**（对已有功能的修改）/ **Deprecated**（即将移除）/ **Removed**（已移除）/ **Fixed**（缺陷修复）/ **Security**（安全相关）。

条目以版本快照粒度组织，不重复 git log。历史版本归档至 [docs/changelog/archive/](docs/changelog/archive/)。版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)，以 [CMakeLists.txt](CMakeLists.txt) `project(MiniLangIDE VERSION ...)` 为基线。

## [Unreleased]

### Added

- **泛型嵌套作用域类型参数词法传播（嵌套函数/lambda/局部 enum）**：`fun f<T>(x: T) { fun inner(y: T): T { return y; } return inner(x); }` 此前四执行后端一致拒绝合法程序（类型参数上下文按函数声明覆盖而非词法作用域合并）。现：编译期 declareFunction/emitFunctionBody 以 外层上下文 ∪ 自身声明 合并（lambda 在泛型类方法体内同步继承类类型参数）；运行时词法事实源 = 闭包携带定义期类型参数快照（ClosureData::definingTypeParams，覆盖 make<T> 返回内嵌函数后再调的延迟调用形态）；局部 enum 三侧声明点并入（variant 载荷校验擦除判定）。附带修复 IR 路径 savedCurrentTypeParams 从无快照赋值的既有遗漏（嵌套函数编译后父函数类型参数上下文丢失）。非泛型程序行为零变化；回归 4 项四执行路径锁定，详见 docs/code-audit-report-2026-10-21.md §8.1（提交 1001580）
- **泛型函数内声明类——类方法继承外层函数类型参数（§3.3 同族收尾）**：编译器/IR/解释器三侧声明点并入定义期上下文，`fun f<T>` 内声明类的字段/方法参数/返回注解不再被拒；顶层类/非泛型场景零变化；回归 2 项四执行路径锁定（提交 50a49b7）
- **性能基准套件新增 object 类双基准（method_call / field_access）**：`tests/TestPerformanceRegression.cpp` 补齐实例路径覆盖——`method_call`（20000 次实例方法调用，覆盖 findMethodChunk 类/方法查找 + 帧构造 + 字段槽位预填 + this 字段读写 + 返回写回全链路）与 `field_access`（50000 次纯实例字段读/写，覆盖 unordered_map 哈希查找路径，无方法调用干扰），三执行后端 + JIT（R160 起原生支持类成员访问，实测正确执行）共 8 个新用例，category 新增 `object`。基线（release 干净树，同会话实测）：StackVM method_call 31.4ms / field_access 40.6ms；RegisterVM 36.8ms / 58.7ms；Interpreter 147.9ms / 85.7ms。

- **泛型显式类型实参语法（`Name<T>(args)` 调用/实例化，解析后擦除）**：此前 `id<int>(5)` / `Box<int>()` 被静默解析为**比较运算链**（`<int>` → `x < int` 的比较），运行时报"未定义的变量: int"——四后端一致的误导性错误（`tests/TestGenerics.cpp` 探针实测发现）。现 Parser 调用点增加探测式扫描（`Parser::trySkipExplicitTypeArgs`）：平衡 `<...>` 且其后紧跟 `(` 才按类型实参消费并擦除（延续 R163 擦除语义，AST 不携带、运行时行为零变化），否则完整回退比较解析路径。消歧规则与 C#/Java 同型——`a < b > (c)` 优先按泛型调用解释（既有程序中 `(a < b) > (c)` 的比较结果再调用属病态写法）；类型实参内容限类型形态 token（标识符/类型关键字/逗号/嵌套 `<>`/`[]`/`?`），出现字面量即回退（`x < 5 > (y)` 仍按比较解析）；词法无移位运算符（`>>` 为两个独立 `>`），`Box<Box<int>>()` 嵌套天然支持。10 个回归锁定用例见 `GenericErasureCompletion.*`。

### Changed

- **字典透明查找收尾 + JIT 只读 helper COW 误分离修复（性能收口，行为无差异）**：① StackVM/RegisterVM/Interpreter 共 9 处 dict member-set/get 改用新增的 `Value::dictFindFieldRef`/`dictGetOrInsertFieldRef`（string_view 异构查找，键为字节码常量池字段名）——此前每次 `obj.field` 读/写构造 `DictKey{std::string}`（每次访问 1 次堆分配 + 拷贝，覆盖赋值也不例外）；JIT 索引/成员 8 处 helper 同型迁移（免临时 StringData + 拥有型 DictKey 双重堆分配；仅改 helper 函数体，生成机器码零变化，`kJitCodegenVersion` 不递增）。② JIT 只读 helper（jitIndexGet/jitLen/jitAddGeneric/jitOrderedCompare/jitMemberGet/jitDefineClass）接收者改 const 声明——`bitsToValue`（fromBitsBorrowed）对堆类型 addRef 使 JIT 栈槽与局部 Value 并存（refcount ≥ 2），非 const 访问器走 ensureUnique 导致共享容器每次只读访问整容器/整串深拷贝（len()、字符串拼接/比较、`arr[i]`/`d[k]`/`obj.field` 读取在内；多处注释声称"const 重载"与代码不符，IC 版 helper 已修过，其余为同族遗漏）；jitIndexSet* 三处越界检查改 as_const（修复前越界错误路径也先整容器拷贝）。详见 docs/code-audit-report-2026-10-21.md §7.1.3/§7.1.4。

- **解析器两项 P2（§3.4/§3.5）本轮受阻未实施**：AutoClaw 平台写入守卫对 Parser.cpp 全文件扫描的凭据启发式在既有代码/注释上误判（token 型声明赋值行 175 处无法按行收敛），按守卫协议不绕过；修复方案已定位到行（trySkipExplicitTypeArgs 白名单补 `:`/`(`/`)` 配对平衡 + enum 变体分支前置探测），待守卫口径修正后一轮落盘，详见审计报告 §7.2。

- **"Inline Cache 优化"路线图项评估收口（StackVM/RegisterVM 成员字段 IC 不实施，实测数据裁决）**：排查确认内联缓存在本项目的实际落点——JIT 的 `OP_MEMBER_GET` 单态 IC 已于 R160 落地（`memberGetIC_`/`jitMemberGetWithIC`，含 COW 悬垂规避），StackVM 三级缓存（`callCache_`/per-chunk `varCache_`/`methodCache_`，PERF-14）覆盖调用/全局/方法分派；**唯一未覆盖的热路径是 StackVM/RegisterVM 的实例字段哈希查找**。立项前提实测（独立微基准，release 级优化编译）：短 SSO 键 `unordered_map::find` 仅 **5.8ns/次**，per-site IC 命中 0.8ns——每成员操作节省 ~5ns，对 field_access 基准总收益上限 **~2.5%**（低于基准方差 ±1.6% 的 2 倍），与"性能跃升"预期相距一个量级；且需引入 InstanceData 身份令牌协议（COW 克隆换新 id + 全局单调计数器，R160 在 JIT 侧已处理的同类 COW 悬垂问题）等语义复杂度。**裁决：StackVM/RegisterVM 成员字段 IC 不实施**。重启条件：① 对象模型槽位化重构（hidden-class 式，届时成员访问 O(1) 槽读，IC 无必要）；② 实测 find 退化（长字段名超 SSO / 字段表大幅增大）。

- **协程真挂起平台覆盖扩展（[ADR-007](docs/adr/ADR-007-coroutine-suspension.md) 平台覆盖项部分清偿；执行行为零语义变化——真挂起与重放的值序列逐点一致，唯一差异是前缀副作用只执行一次）**：`MINILANG_CORO_FIBER` 从仅 Windows 扩展至 **Linux**（POSIX ucontext 平台层），三执行后端在 Linux 上同步启用真挂起（ADR-002 单边启用禁令由共享门控自动保持）。实现：`interpreter/InterpreterCoroutine.cpp` 平台层改为双分支——Windows fiber API 包装原样保留，新增 ucontext 分支（每个 fiber 一个堆上控制块：`ucontext_t` + 专有 4MB 栈 + 首次入口参数；`thread_local` 登记当前 fiber 作为 `switchToFiber` 双向保存锚点；`makecontext` int 宽参数限制经控制块指针拆高低 32 位 + 首次入口 `fiberEntry` 重组；五函数接口 `convertThreadToFiber`/`createFiber`/`switchToFiber`/`deleteFiber`/`currentFiber` 两平台对称，其余挂起/恢复协议代码平台中立零改动）；ASan 构建下切换前后配对 `__sanitizer_start/finish_switch_fiber` 备用栈标注（`MINILANG_FIBER_ASAN` 门控，GCC `__SANITIZE_ADDRESS__` / clang `__has_feature` 分层判定），由 CI Linux ASan 门禁持续验证；macOS 保留重放（ucontext 例程标记 deprecated 且发布矩阵 mac job 仅编译不跑测试、无行为验证通道；bionic 无 `makecontext` 不在支持范围）。测试：`TestCoroutine.cpp` 顶部平台 `static_assert` 锁定"有平台层 ⟺ `=1`"对应关系，既有 `CoroutineSuspension.*` 语义锁与 `CoroutineTest`/`AsyncAwait` 全量用例在 Linux CI 上自动转挂起路径执行。文档同步：ADR-007（Status/Decision/实现形态表/Known Limitations #1）、`backend-consistency.md` §9 平台层扩展义务、AGENTS.md §3。验证：Windows debug 树全量 4132/4132 通过（/W4+/WX 零警告）；Linux GCC 13 实机（WSL Ubuntu 24.04）真实 TU `-fsanitize=address` 与 `-O2 -Wall -Wextra` 编译零警告 + 切换协议行为冒烟（多次挂起/恢复往返、fiber 栈对象往返完好、双 fiber 交叉切换、终结释放、ASan 运行零报告）全绿。

### Fixed

- **P2：协程值跨线程析构 DeleteFiber 边界防御（§9.2 清偿）**——`~CoroutineFiberState` 增加 ownerThread 比对：同线程照常 DeleteFiber；非创建线程析构（IDE 可达：ReplPanel future.get() 主线程析构 worker 线程创建的挂起协程 Value）时有意弃置 fiber（不调用跨线程 DeleteFiber——Windows 未文档化行为），泄漏栈约 4MB/协程有界且仅命中 parked fiber，无 UAF，Logger 记录；ADR-007 Known Limitations #3 同步。回归 `CoroutineSuspension.CrossThreadParkedFiberDestruction`（提交 f6c0802）
- **P2：顶层 catch 遮蔽与解构绑定同族收尾（V10/V11 家族）**——① 顶层 catch removeMapping 登记 shadowedGlobalNames_（restoreCatchScope 及错误路径同步解除），catch 体内嵌套块重声明恢复缺失修复；② 解构绑定 fresh 全局名与 var 同规格登记（直译 currentLocals_ / IR topLevelBlockFreshGlobals_），块退出 DELETE_VAR 清理，visitBlock pre-pass 重构覆盖两类块级声明。回归 `ConsistencyDiff.BlockFreshDestructureScopeEndsAtBrace` / `CatchVarShadowNestedBlockRestored`（各四路锁定；提交 f9cbe4c）
- **P2：块内新声明全局泄漏（V11，四后端不一致）**——`var a = 1; { var y = 5; print(y); } print(y);` 修复前双编译路径块外仍可读 y（解释器报「未定义的变量: y」）：直译路径 visitVarDecl 顶层分支从不入 currentLocals_（blockVars 恒空无 DELETE_VAR），IR 路径 varMap_ 绑定永久残留。修复 = 直译侧 fresh 登记 currentLocals_ + 块退出 DELETE_VAR + pre-pass ancestorFresh 判定（嵌套块重声明走保存/恢复，对齐解释器环境链遮蔽语义）；IR 侧 topLevelBlockFreshGlobals_ 逐块栈 + 块退出发射 DELETE_VAR 并移除 varMap_（对齐顶层 for 循环 BUG-IR-SCOPE-1 清理先例），块退出后引用经 resolveVar 回退重绑运行期报错。回归 ConsistencyDiff 新增 BlockFreshVarScopeEndsAtBrace / BlockFreshVarNestedShadowRestored（各四路锁定）；diff_test 96/96；顺带修正 harness 错误标签分类学（VM 运行期错误 VMRT: → RT:，与解释器 RuntimeError 同阶段按消息内容比较）
- **P1：顶层嵌套块遮蔽值不恢复（V10，四后端不一致，本轮新发现即清偿）**——`var x=1; { var x=2; { var x=3; print(x); } print(x); } print(x);` 解释器 `3,2,1` vs StackVM 非 IR 直译路径 `3,3,1`（test_harness/diff_test V10 实证；harness 不在 ctest 口径，历轮全绿未拦）。根因：顶层块遮蔽协议中祖先块 removeMapping 后运行时 globalNameToSlot_ 快路径仍指向原槽位，嵌套块 pre-pass 因 lookupGlobalSlot<0 跳过保存，内层 `var x` 经名称 op 污染原槽且退出不恢复（单层块因全部读写同槽偶然正确）。修复 = Compiler::shadowedGlobalNames_ 登记活跃块移除的映射名，嵌套块对祖先遮蔽名经名称 op 保存当前有效值、退出走既有 SET_VAR 恢复（对齐 AstIRBuilder CRITICAL-3）；IR 路径此前已修故三路本就一致。回归：ConsistencyDiff 新增 runStackVM_Direct 非 IR 口径 + 遮蔽族 4 用例四路锁定；diff_test 增 V11 探针实证 VM 顶层块内新名泄漏（→ V11 条目同轮清偿）；顶层 catch 遮蔽同族待下轮（提交 6f970ad）
- **P2（产品级）：pkg writeFile 原子化（§9.2 清偿）**——同目录临时文件（`.tmp<pid>`）+ `std::filesystem::rename` 替换（Windows MOVEFILE_REPLACE_EXISTING / POSIX rename 均原子覆盖），rename 失败回退就地写；saveManifest/saveLockfile 两调用方一并受益，半截 JSON 并发读根因链源头收口（§3.12 测试隔离只治标）。回归 `PkgLockfileTest.SaveLockfileAtomicReplaceNoTempResidue`（提交 0278078）
- **协程跨线程恢复防御（§3.8 清偿）**：生成器 fiber 由首次 `.next()` 线程创建并登记 owner，非创建线程恢复在触碰任何 fiber 机制前报「协程不可跨线程恢复」——消除 Windows SwitchToFiber 跨线程未定义行为 / Linux ucontext 空指针崩溃边界；语言层不可达（spawn 为协作式延迟执行），回归用例经 std::thread 串行驱动（提交 7b4ec10）
- **P3：RegisterVM instructionSizeAt 截断防御钳制（§3.10 清偿）**——多段变长指令（REG_DEFINE_CLASS 等）在段缺失时原按 baseSize 回退，buildIpMap/relocateGlobalSlots 的线性走查会跳入指令数据中段错位解读；现把计算结果钳制到剩余字节数（合法字节码零变化，仅畸形字节码可达——缓存 FNV 校验双重防御下的保守加固；提交 1625439）
- **协程跨线程恢复防御（§3.8 清偿）**：生成器 fiber 由首次 `.next()` 线程创建并登记 owner，非创建线程恢复在触碰任何 fiber 机制前报「协程不可跨线程恢复」——消除 Windows SwitchToFiber 跨线程未定义行为 / Linux ucontext 空指针崩溃边界；语言层不可达（spawn 为协作式延迟执行），回归用例经 std::thread 串行驱动（提交 7b4ec10）
- **P3：JIT 块缓存键 enumInfos 全量入键 + kJitCodegenVersion 递增（§3.12 记录项清偿）**——同字节码指纹、不同 enum 声明不再可能命中同一缓存键；版本 1→2 为键代际确定性隔离与磁盘持久化纪律先例（机器码逐位不变；提交 38ee8fc）
- **test(pkgcli)：命令级用例 lockfile 隔离到用例临时目录（§3.12 flake 根因修复）**——共享 CWD minilang.lock 跨进程竞争（saveLockfile 非原子 × loadLockfile 半截解析）致 InstallCommandAllFromManifest 偶发 exitCode 2；8 处用例隔离，PkgCli 38/38 × 4 轮 -j 10 验证（提交 4586759）
- **P2：dict 键类型含 `:` 的注解形态擦除漏判 + 必然拒绝（§3.6 清偿）**——`fun f<K>(x: dict[dict[string:K]:int])` / `dict[fun():K:string]` 形态被首个 `:` 截断（键类型 `dict[string` / `fun()`），擦除漏判 → 运行时校验必拒合法程序（四后端一致地错）。修复：`common/TypeChecker.h` 新增与 `parseTypeAnnotation` 同构的文法贪婪切分 `findDictKVSeparator`（dict[K:V]/fun(p):r 递归 + `[]`/`?` 后缀堆叠；`fun():K` 的返回类型冒号与分隔冒号同深度，纯括号深度不可区分），擦除判定（annotationReferencesTypeParam）与运行时校验（typeMatchValue）两侧共用。回归 `GenericErasureCompletion.DictNestedDictKeyAnnotationErasure` / `DictFunKeyAnnotationErasure`（四后端）。

- **P3：字典 NaN 键可写不可查 → SameValueZero（§3.11 清偿，语义变更）**——NaN 位模式随运算来源不同散布不同 hash 桶 + equal 判否：每次 NaN 键写入成为不可查找死条目（len 只增、读永 miss）。修复：DictKeyHash 所有 NaN 规范化到 quiet_NaN 同一位模式（与既有 -0.0→+0.0 同型）+ DictKeyEqual 判 NaN 互等（对齐 JS Map/Set），单点修复对含字面量构建在内的全部路径自动一致。注：`0.0/0.0` 是显式除零错误，NaN 经 `inf - inf` 产生（浮点乘/减无检查）。回归 `ConsistencyDiff.DictNaNKeySameValueZero`（三后端 `21`）；语义条款已入 backend-consistency §2。

- **P1：解释器 fiber TCO 上下文跨挂起边界滞留（F3/W1+W2）**——fiber 体 TcoScopeGuard 滞留 fiber 栈：① 挂起窗口五成员（tcoDecl_/tcoName_/tcoIsMethod_/tcoEnabled_/tcoTryDepth_）滞留 disabled 态，调用方尾调用退化普通递归（逐层活跃挂起叠加可虚假触发 256 递归上限）；② 生成器在异于首次 `.next()` 的函数上下文终结（return 提前耗尽/错误逃逸）时守卫析构写回首次进入时的陈旧蹦床快照——终结上下文 `return a(k+1)` 被陈旧 tcoName_ 误分类为 SelfFunction 无 target 信号，被终结上下文自己的蹦床循环捕获携错误实参重跑（探针实测：期望 56 缺陷 57，a 的函数体从未执行），且与 macOS 重放路径跨平台分歧（超出 ADR-007 唯一既定差异口径）。修复 = TCO 五成员纳入挂起/恢复协议（coroutineSuspend 对称保存/恢复 + CoroutineContextGuard 退出恢复调用方真值）。回归 `CoroutineSuspension.TcoContextRestoredAcrossGeneratorTermination`（interpreter 路径；蹦床为解释器专有机制）。

- **P1：挂起窗口生成器区开放 upvalue 相对索引对跨快照访问者暴露**——yield 出捕获生成器局部的闭包并在窗口期（挂起至下次恢复）调用：open 态相对 stackSlot 指向已搬空的活栈区域（OP_GET_UPVALUE 静默读错值；OP_SET_UPVALUE 向调用方活栈槽写入，写路径可升级 P0）。修复 = 挂起时快照式关闭（`uv->value` = 快照槽值、`isClosed=true`，与语言"关闭=快照"语义自洽），窗口期闭包经关闭盒读写；恢复时先传播写回再重挂 open（生成器体与逃逸闭包双向一致，无写入时幂等）。StackVM/RegisterVM 同型实施；§9 新增挂起窗口 upvalue 语义条款。四后端回归 `CoroutineSuspension.SuspendedWindowClosureUpvalueSemantics`（`1111`）。

- **P1（四后端不一致）：VM 协程跨调用方深度恢复循环边界回拨（§9 专项审计新发现）**——真挂起恢复路径在重定基完成后把循环边界/挂起基线回拨到首次挂起时的调用方入口（`savedFrameCount = sus->baseFrames`），两次 `.next()` 调用方帧深不同即破坏：深恢复在生成器帧弹出后循环越界继续执行调用方指令（探针实测 `133true` 而非 `123true`），浅恢复循环短路、快照被消费而帧未清理，后续 `.next()` 全新重建报运行时错误；重挂起窗口切片同被回拨把调用方栈槽/帧误并入快照。修复：删除 StackVM/RegisterVM 两处回拨，循环边界/清理/挂起基线登记一律保持**本次**入口深度，快照结构移除只写基线字段（快照内不携带任何绝对基线）；RegisterVM 默认构建经 IR 内联掩盖，关内联（`setIRSSAOptimize(false)`）实测同型错值。回归 `CoroutineSuspension.ResumeAtDeeperCallSite` / `ResumeAtShallowerCallSite`（跨深度场景此前无覆盖），详见 docs/code-audit-report-2026-10-21.md §6.1（提交 251a54c）
- **P1（四后端不一致）：VM 错误逃逸后 `.next()` 语义与解释器分歧（§3.1 清偿）**——StackVM/RegisterVM 生成器体错误穿透后 vmSuspension 已消费，下次 `.next()` 走全新调用重放（实测 `1E1false1true` vs 解释器 `1E1false<runtime>`，ADR-002 违例）。修复：`CoroutineData::vmPendingErrorBox` 保留失败态——逃逸（帧深度判别）时按值捕获，入口前置检查持续同点重抛（done 保持 false、前缀副作用不重执行，对齐解释器 fiber；`=0` 重放平台不保留）；GcManager 协程分支补根集；RegisterVM 捕获基准修正（恢复后捕获计入生成器自身挂起 try 帧致内部 catch 误判逃逸，两 VM 判别均改帧深度基准）。`FailedGeneratorRethrowsSameErrorOnEveryNext` 升级四执行路径锁定（提交 7dd9557）
- **P3：JIT 块缓存同键重复 store 不清旧 LRU 节点（§3.9 清偿）**——store 前按 key 摘除旧条目（先逐块 `sharedCacheRuntime().release` 再移除 LRU/map），消除同键双节点失真与旧共享块释放缺口（防御性清偿，前置源码自标不应发生；提交 46f3a6d）
- **P0：挂起协程快照不在任何 GC 根集（三执行后端），挂起窗口增量 GC 清空快照容器元素**——`fun* gen() { var a = [1,2,3]; yield a[0]; }` 挂起后主脚本分配跨过 GC 阈值（8192）时，仅被生成器局部环境持有的数组被 sweep 清空元素，恢复后索引越界/数据损坏（探针实测；`CoroutineFiberState` 注释声称"持 GC 根"但从未接线，v1.4.0 起即存在）。修复：① `Value::gcRootPtr()` 补协程分支（对齐 BUG-REPL-AUDIT-7 闭包先例：CoroutineData 不登记 slots_ 不会被 sweep，但作为根让 markPhase 进入协程分支）；② `CoroutineData` 新增 `gcRootContributor` 类型擦除回调——Interpreter fiber 贡献 suspendedEnv/funEnv/suspendedCallStack 各帧环境链（`collectCoroutineFiberGcRoots`），StackVM 贡献栈切片 + 帧内已关闭 upvalue，RegisterVM 贡献寄存器窗口 + 已关闭 upvalue；GcManager mark 的两个 VAL_COROUTINE 分支统一接入（根集循环 switch 抽取为 `markTrackedRoot` 共用）；丢弃快照处同步置空回调（防悬垂）。回归 `CoroutineSuspension.SuspendedGeneratorLocalsSurviveIncrementalGc`（3 万次分配确保跨阈值）。

- **P1（四后端不一致）：泛型构造路径参数/默认值校验先于类型参数合并 guard**——`class Box<T> { init(v: T[]) {} }` 的 `Box([1,2])` 在 Interpreter 被拒（期望类型 T[]）而三 VM 无参数校验发射点直接通过：`setupInitEnvironment` 的 checkType 发生在 `runInitMethodBody` 合并 guard 之前（R163 擦除依赖 currentTypeParams_）。修复 = 合并 guard 前移（constructClassInstance 与 visitVarDecl 自动构造两处）。回归 `GenericErasureCompletion.GenericClassInitParamCompositeAnnotation` / `GenericClassInitDefaultTParamAnnotation`（标量 T 默认值；注：数组字面量默认值被 Parser BUG-DEF-1 字面量策略拒绝，另见审计报告新发现）。

- **P1（四后端不一致）：JIT `jitBuildEnumVariant` 载荷校验未升级递归擦除（第七处擦除点）**——OP_BUILD_ENUM_VARIANT 为 JIT 原生 helper 路径（JITCodeGen.cpp emitBuildEnumVariant，非回退 VM），载荷校验仍用 `isTypeParameter` 精确匹配：`enum Row<T> { Cell(T[]) }` + 非空数组载荷在 JIT 报"类型不匹配"中止，其余三后端（已递归擦除）构造成功。修复 = 升级为 `annotationReferencesTypeParam`（与既有六处同判定）。回归 `GenericErasureCompletion.EnumPayloadCompositeTAnnotationJIT`（runJIT 通道——expectAllGenericBackends 不含 JIT，故此前未覆盖）。

- **P1（语义分歧）：失败协程弃用重建导致 yield 计数错位**——解释器 fiber 路径错误逃逸后 reset 重建，下次 .next() 从头重跑函数体且 coroutineSuspend 无条件递增 currentYieldId：重跑首查即提前置 done、返回错误前首个 yield 值，与重放"跳过已消费 yield 后同点再抛"分歧。修复 = 失败 fiber 保留（失败态前置检查，绝不 switch 进已终结 fiber 栈——原返回点单检查改为前置 + 返回点双检查），后续 .next() 持续重抛同一错误（值/错误序列与重放逐点一致，前缀副作用只执行一次）。回归 `CoroutineSuspension.FailedGeneratorRethrowsSameErrorOnEveryNext`（解释器三段序列）+ `FailedGeneratorErrorEscapePrefix`（四后端一致前缀：错误穿透 + done=false）。

- **P2：JIT 块缓存 storeBlocksToCache 未翻转实例侧 owned 标志**——编译实例对共享 runtime 分配的块保留 owned=true，下次 execute 入口/析构经实例 runtime_.release 跨分配器释放共享块（asmjit 对外来指针按查块失败 no-op 掩盖；runtime 合一即双重释放/UAF）。修复 = store 后置实例侧 owned=false，对齐命中路径"缓存共享块实例不释放"既定不变量。

- **四后端不一致（P1）：泛型复合注解擦除遗漏**——`fun first<T>(xs: T[]): T` 在 Interpreter 被拒绝（"类型注解违反: 期望类型 T[]，实际为 array"）而 StackVM/StackVM-IR/RegisterVM 通过，违反 ADR-002（探针实测，R163 回归缺口：原擦除仅精确匹配裸类型名）。根因：`common/TypeChecker.h` 的 `isTypeParameter` 只做整串比对，`T[]`/`T?`/`dict[string:T]`/`fun(T):T` 等复合注解漏擦——Interpreter 运行时 `checkType` 落入 typeMatch 的元素递归（元素类型 "T" 未知 → false），VM 侧编译期发射点漏擦/enum 载荷校验点漏擦形态各异。修复：新增递归判定 `annotationReferencesTypeParam`（注解结构中任一基础类型引用类型参数 → 整体跳过校验，对齐 R163"类型参数运行时不校验"；注解字符串格式与 `Parser::parseTypeAnnotation` 生成格式一一对应），统一接入六处擦除点——`Interpreter::checkType`（运行时）、`Interpreter::visitEnumVariantExpr` 载荷校验、`Compiler::emitTypeCheck`（栈式发射期）、`AstIRBuilder::emitTypeCheckIR`（IR 发射期）、StackVM `OP_BUILD_ENUM_VARIANT` 载荷校验、RegisterVM 对应点。10 个四后端回归锁定（`GenericErasureCompletion.*`，含函数体内 `var: T[]`、`dict[string:T]` 参数、enum 载荷 `Cell(T[])`、`T?` 参数）。

- **docs/development.md P2-10 条目陈旧修正**：SSA 重命名阶段"LOCAL 变量循环回边 bug"早已由后续修复轮次清偿——`ssaConstructPass` 重命名阶段的 currentDef 栈（`slotStacks`：STORE_LOCAL 压栈、回边 PHI 操作数经 `stack.back()` 填充、支配树退出弹栈）即 Cytron 式重命名，且 `irOptimize_`/`irSSAOptimize_` 默认已为 `true`（`compiler/core/Compiler.h`），SSA 构造/析构包裹 GVN/LICM 按返回值守卫。原"待后续实施（修复方向：实现 currentDef 栈重命名）"的工程量评估作废；全量回归（含 TestIRSSA/TestAstIRBuilder 与默认开启 IR 优化的全量 E2E）持续通过佐证。

## [v1.4.0] - 2026-10-07

协程真挂起执行架构（[ADR-007](docs/adr/ADR-007-coroutine-suspension.md)，生成器前缀副作用只执行一次——本版唯一语义变更）与 JIT per-chunk 编译架构（[ADR-008](docs/adr/ADR-008-jit-per-chunk.md)，tiering 定向重编译 O(1) 化 + 进程内块缓存，执行行为零语义变化）两大架构级改造落地；GC 登记表 slot-index 重构与运行时/IR/GUI 多轮性能优化；新增 Windows MSVC ASan 阻断式内存错误门禁；修复 IR 路径 super 调用栈不平衡、JIT 异常分派 use-after-pop、Environment 弱指针悬挂收口等缺陷。

### Added

- **MSVC ASan 内存错误门禁（Windows CI job，[ci-msvc-asan-proposal.md](docs/ci-msvc-asan-proposal.md) 合入）**：[.github/workflows/ci.yml](.github/workflows/ci.yml) 新增 `sanitize-asan-msvc` 阻断式 job（经维护者授权对只读 CI 范围的一次性修改）——全局 `/fsanitize=address` 四处 flags（含第三方 gtest 全量插桩）+ 三测试目标（tests/app_tests/gui_smoke，`MINILANG_BUILD_PERF_TESTS=OFF`）+ 全量 ctest（`ASAN_OPTIONS=detect_leaks=0:abort_on_error=1`，-j 2 + 600s 单测超时）。补齐主力开发平台 Windows 的内存错误检测盲区（此前 ASan 门禁仅覆盖 Linux；MSVC CRT/Win64 ABI 特有路径无 sanitizer 覆盖，提案 §4 记录该流程曾捕获 JITRuntime jitThrow use-after-pop 真实缺陷）。**合入过程中捕获一个 preset 路径不可用的实测结论**：`windows-msvc-asan` preset 的 `MINILANG_SANITIZE` 经 INTERFACE 选项只插桩项目目标，树内预编译 gtest 保持未插桩，MSVC 14.51 链接期 `annotate_optional` 严格检查报 LNK2038/LNK1319（CMakeLists 属只读无法给第三方目标补选项；GCC asan 无对应 ABI 标记检查，Linux job 不受影响），故采用提案 §4 已端到端验证的全局 flags 配方；app_tests 与 gui_smoke 的 windeployqt 部署并行写同一 `tests/` 目录存在同名 DLL 拷贝竞态，三测试目标改为顺序构建。合入前本地以与 CI 逐字相同的命令完成全新 configure + 构建 + 全量 ctest 排练；其余差异适配（Qt 注入路径/perf 关闭/CRT PATH 免除）见 ci.yml job 头注释与提案状态节。

- **IR lowering 栈平衡校验器（D1 第一步）**：[compiler/ir/BytecodeIRBackend.cpp](compiler/ir/BytecodeIRBackend.cpp) 新增 debug 构建专用校验（`#ifndef NDEBUG`，Release 零开销）——每条 IROp 的 (pops, pushes) 效应表（`irStackEffect`，逐条对齐 StackVM 运行时实现）+ `lower()` 指令循环内的操作数栈深度模拟（`checkStackBalance`），替代"发现一个补一个"的人工保证模式（对应 2026-08-03 审计报告 D1 的第一步：先校验后重构）。失败分级：负深度（弹出多于可用，运行时必然错位）拒绝 lowering；LABEL 合并点深度冲突报告但不拒绝（兼容 AUDIT-R6 "break 丢弃挂起 return 值"的合法不平衡——运行时由 OP_RETURN 帧回收清理）。peek 型指令（STORE_LOCAL/JUMP_IF_FALSE/TYPE_CHECK 不消费栈顶）与 catch 标签深度协议（TRY_BEGIN 高度 + 1）已正确建模；`MINILANG_IR_STACK_DEBUG=1` 可输出逐指令栈深轨迹。回归测试 `AuditBatch1IRBalance.*` 5 用例。**校验器落地过程中顺带捕获并修复一个真实 lowering 缺陷**：顶层/嵌套普通函数中的 `super.method()` 写回链多发一个 POP（BUG-INH-4 的条件对齐 isVarRef 分支遗漏——`this` 回退到 GLOBAL 时 OP_SET_GLOBAL 已 pop），此前被"未定义的变量： this"运行时错误掩盖，见 Fixed 条目。

- **审计剩余项复核与测试补强（D3/D4/D7/D8/D15 复核）**：三路并行复核 2026-08-03 审计报告剩余项的现状——D3 条件断点三个子问题（入口快照未初始化/沙箱标志跨求值/D1 求值缓存与 slot 失配）均已被既有修复覆盖或属误报（SandboxGuard 16 类状态 RAII 恢复、VM 路径每求值全新临时 Interpreter、缓存为按条件文本键控的 AST 而非 BytecodeChunk）；D4 的"4 处 FIXME + 1 处 TODO"不存在（`genericXXX` 标识符被 XXX 子串扫描误命中，审计报告已加勘误注记），NaN-boxing tag 检查与编码严格一致。落地补强：
  - **D15**：astEqual AST 等价比较器自 test_harness 提升进常规回归（[tests/TestFormatterRoundTrip.cpp](../tests/TestFormatterRoundTrip.cpp)），samples/mini 全部 .mini 样例数据驱动「parse → format → reparse → AST 等价 + 二次格式化幂等」，另含右/左嵌套括号、成员链、try/catch/finally+闭包等内联边界用例。
  - **D7**：闭包经数组/字典索引调用的 capturedVars 写回持久性回归锁（此前仅覆盖读取路径）；多闭包快照语义（对齐 R2）锁定为 "1102" 四后端一致。
  - **D8**：catch 块内 var 重声明 catch 变量的三后端语义锁（含顶层遮蔽 + DELETE_VAR 清理交互、重声明后 rethrow）。
  - **D4**：`verifyNanBoxConstants()` 接入 `JITBackend::execute` debug 调用点（原实现无任何调用点，注释声称"首次 execute 时校验"但从未接线）。
  - **D10**：gui/ 全部手工 `blockSignals(true/false)` 配对（4 文件 8 处）替换为 `QSignalBlocker` RAII；`Interpreter::stopRequested_` 协作中止标志 relaxed → release/acquire（GUI 线程写 / Worker 线程读建立同步关系）。
  - **W4 警告豁免全量退役（审计问题 5 闭环）**：CMakeLists.txt 的 gui/ 五类豁免（C4456/C4458/C4189/C4100/C4996）全部移除——C4189/C4100/C4996 复核为零警告（原豁免为陈旧保护），C4456/C4458 遮蔽 38 处集中于 AstVisualizerPanel.cpp 的 else-if 链条件声明与循环变量，逐处重命名清偿；gui/ 全部编译单元现与 core 同处 /W4 + /WX 零豁免基线。
  - **BaseTeachingPanel 面板基类（审计问题 3 第一步）**：新增 [gui/TeachingPanelBase.h](../gui/TeachingPanelBase.h)/.cpp——主题切换接线收敛为 `applyTheme()` 虚钩子（原 6 处 `Theme::onThemeModeChanged` 手工 lambda 样板），CodeJourneyInfoPanel / BreakpointConditionPanel / IRTransformPanel 三个面板先行迁移验证，其余面板按家族分批迁移（迁移路径见基类头注释）。
  - **Fuzz 语料扩充**：[tests/TestThreeEnginesFuzz.cpp](../tests/TestThreeEnginesFuzz.cpp) 新增 3 个生成器类别（每次运行 100 个随机程序，固定种子可复现）——异常处理（有条件 throw/catch 变量/finally/嵌套/重抛链，对齐已知 Bug 模式 #5）、继承与 super（super.init/方法动态派发/字段查找链，模式 #9）、字典与 COW（索引读写/副本写别名隔离，模式 #1-4）。
  - **MSVC ASan CI 门禁提案**：新增 [docs/ci-msvc-asan-proposal.md](ci-msvc-asan-proposal.md)——Windows MSVC 侧 `/fsanitize=address` 阻断式测试 job 的可直接合入定义（现有 ASan 门禁仅覆盖 Linux，主力开发平台 Windows 存在内存错误检测盲区；平台差异风险已有 v1.3.0 SysV ABI P0 先例）。CI 配置属只读范围，故以提案形式交付；job 命令已在本地 ASan 构建树验证。

### Changed

- **JIT per-chunk 编译架构落地（第六轮架构级优化，[ADR-008](docs/adr/ADR-008-jit-per-chunk.md)；执行行为零语义变化）**：debug 树全量 4132/4132 回归通过（4127 原有 + 5 新增 BlockCache），/W4+/WX 零警告。立项前代码实测勘误了积压记录的三项难度前提（详见 ADR-008 Background）：**JIT 与 VM 之间不存在帧互操作**（StackVM 零 JIT 引用；真实跨块协议是 JIT 各代码块间共享 `JitContext` 的软帧协议，lazy 模式下已跨 CodeHolder 工作多年），改造难度大幅低于 2026-10-04/05 两轮"超合理工作量"的评估结论。三段实施：
  - **阶段 1a（per-chunk 代码块）**：`compileAllChunks` 改为两遍流程——每 chunk 发射进独立 CodeHolder（1950 行循环体提取为 `emitChunkBody`），全部成功后逐块 `runtime_.add` + 入口解析；跨块 OP_CALL fast path 经 `JitContext.callImportSlots` 导入槽间接跳转（追加两字段遵循 ADR-006 append-only，零 C++ 调用保留 fast path 性能本质），非 main 块错误退出经 `ctx->errorExit` 退出桩指向 main 块 epilogue（~40 处发射辅助签名零改动）；所有权权威 `codeBlocks_`（`JitCodeBlock`），`JitContext`/`JitFrame`/helper ABI 三协议不变。
  - **阶段 1b（tiering 真单 chunk 编译）**：新增 `compileSingleChunkBlock`（无 prologue/epilogue/闭包跳板的被调块形态），`compileSingleChunkLazy`/`compileChunkSpecialized`/`Float`/`WithOsr` 两变体薄包装化——**删除 14 向量 save/restore 三份恢复块**（R157 修复注记自证的悬垂指针温床）；单 chunk 编译不触碰统计数组/chunkTiers_，IC callSiteId 追加编号（老块槽位永不失效）。
  - **阶段 2（进程内编译块缓存，共享存活设计）**：缓存键 = `fnv1a64(字节码内容) ^ kJitCodegenVersion`；未命中时深拷贝 `CompileResult` 作为编译宿主（块内嵌指针天然指向缓存持有的副本），代码块从进程级共享 `JitRuntime` 分配；同源码新 `JITBackend` 实例（GUI 每次 run 新建，4 处构造点）命中缓存直接引用共享块执行——**同源码重复运行免重编译，零重绑定零字节拷贝**；命中实例重建映射/统计（`initPerChunkState` 与全量编译共用）/导入槽/`errorExit`/闭包跳板；门控 = 纯 eager 路径（lazy/分层/自定义阈值按需重编译绕过）；LRU 容量 8；教学汇编捕获命中时复用（仅实例显式启用时呈现）。**重定位记录层归入阶段 3**（磁盘持久化才需要把代码与宿主实例解耦），进程内复用由共享存活设计覆盖。新回归 `TestJIT.BlockCache.*` 5 项（跨实例输出一致/键隔离/有状态程序命中/lazy 绕过/自定义阈值绕过）。
  - **验证期捕获并修复三个真实缺陷**：① emitCallDispatch 的被调已编译判定原置于 fast-path 帧发射**之后**，`return false` 时调用方追加 mailbox 造成帧双重压入（R157LazyConsistencyWithStackVM 段错误，经块汇编转储肉眼定位——生成序列中帧构造与 helper 调用并存即铁证）；② tiering 内层块双重所有权（`registerSpecializedOwnership` 与批量移交同块登记两次 → 析构双重 `release` → asmjit `bit_vector_index_of` 越界读 → 堆损坏，表象随布局漂移：int3/断言对话框/静默挂起，MSVC ASan 树单测复现定位）；③ INT/Float 特化路径缺失 R157 式 ctx 数组指针重同步（恢复向量后 ctx 指向已释放缓冲区，补全成功/失败四路径）。
  - **度量注记**：旧实现无同会话 A/B 基线（工作树不可干净回退），JIT 基准做"无性能悬崖"量级检查——debug 树直跑：调用密集的 `Fibonacci_JIT` 31ms vs StackVM 776ms（≈25x，该用例全程走跨块导入槽间接跳转路径）、`LargeLoop_JIT` 7ms vs 274ms（≈39x）、`Tak_JIT` 13ms、`Ackermann_JIT` 1ms，间接跳转未造成退化；缓存收益以 `blockCacheHits/blockCacheStores` 进程级计数器可观测（BlockCache 回归测试断言）。

- **GcManager 登记表 slot-index 重构（第五轮性能优化；无语义变化）**：debug 树全量 4127/4127 回归通过（4123 原有 + 4 新增），/W4+/WX 零警告；MSVC ASan 树全量回归通过 4095/4095（GC 重构专项验证通道，CI 同款配方 `-j 2 --timeout 600 -E PerfBenchmark`，零 ASan 报告）。[interpreter/GcManager.h](../interpreter/GcManager.h)/[GcManager.cpp](../interpreter/GcManager.cpp) 以单一 `slots_` 槽位表 + `freeSlots_` 空闲槽位复用取代原 `tracked_` vector + `aliveSet_` unordered_set 双结构；[RefCounted](../interpreter/RefCounted.h) 新增 `size_t gcSlot_` 槽位下标字段。核心不变量（全局 `recursive_mutex` 内）：非空槽位 ⟺ 槽位被 `gcSlot_==i` 的存活对象占用，nullptr 为已析构墓碑，锁内无悬垂指针。消除四类哈希开销——注册（`push_back` + `unordered_set::insert` 节点堆分配 → 槽位写入，freeSlots_ 命中时零堆分配；AUDIT-P2-CORRECT 的 insert 抛异常回滚问题随单结构化消失，唯一可抛点 push_back 位于状态变更前）、析构（`unordered_set::erase` → 槽位置空 + 下标回收）、sweep（逐对象 `aliveSet_.find` → 连续内存槽位判空，且按下标迭代结构性免疫级联析构期间的容器变更，旧实现依赖"级联析构不向 tracked_ push_back"的偶然性质）、Phase 3（survivors 重建 + aliveSet_ 清空重灌 → 单趟原地压缩，存活对象前移并回写 gcSlot_）。`onDestroyed` 以 `slots_[gcSlot_]==obj` 指针身份卫兵回收，覆盖 sweep/压缩后晚期析构、reset 后注销、槽位复用三类陈旧场景（ABA 免疫）。`trackedCount()` 语义由"含墓碑条目数"收紧为"精确存活数"（GUI 面板显示更准确；既有断言全部兼容）。**度量**：debug 树配对 A/B（直跑）——分配密集新基准 `ArrayAlloc_Interpreter` 863ms → 778ms（5 次重复中位，方差 ±1.6%）约 **-10%**，StackVM/RegisterVM 差值在噪声内（VM 分发主导，登记占比小，2026-07-25 审计"登记开销可忽略"的判断在纯 VM 场景成立，Interpreter 树遍历路径受益更直接）；ASan 树同会话归因 A/B 四用例新代码全面更快（ArrayAlloc_Interpreter -6.5%）。**方法论注记**：基准对比必须同会话抓取——本次实测跨会话环境漂移可达 +22%（旧代码自身次日重跑），早间抓取的 ASan 基线不可作跨日锚点，曾一度误判为改动致 +15~20% 全路径膨胀，经旧代码重跑归因排除。**全局锁纪律与全部锁点不变**——2026-07-25 审计"collectCycle 需一致状态、thread_local 批量登记不安全"的裁决继续有效，本轮只做锁内数据结构优化；分配路径 thread_local 批量登记去锁化维持"需 TSan 通道专验"的积压状态。新增回归测试 `AuditBatch3GcLeak.SlotRegistryReuseAndExactLiveCount`（槽位复用/精确计数/身份卫兵三不变量）；新增 `PerfBenchmark.ArrayAlloc_*` 三后端分配密集基准（每迭代 2 次容器登记 + 注销，4 万次登记跨 8192 阈值多次触发增量 GC，覆盖 register/onDestroyed/sweep/压缩全链路）。教学面板 GC 阶段文案（MemoryModelLibrary/GcVisualizerPanel）与 memory-model.md §3 同步 slots_ 语义。

- **`app/ide.h` 头文件自包含化（面板头前置声明）+ TeachingPanelBase 迁移评估（未实施，记录结论）**：全量 4123/4123 回归通过，均为无行为变化的工程卫生改动。
  - **ide.h 瘦身**：[app/ide.h](app/ide.h) 原透传 45 个 gui 面板头 + ADS/QFluentKit 头，但其自身声明对这些类型仅以指针/引用形式使用——现只保留按值使用所需的包含（`QMainWindow`/`QMap`/`QSet`/`QList`/`QStringList`/`QByteArray`/`QPoint` + Diagnostic.h/IdeController.h），其余全部前置声明（Qt 控件、`ads::CDockManager/CDockWidget`、QFluentKit ComboBox/Pivot/RoundMenu、52 个 gui 面板/组件类），面板头与 FluentGlobal.h 下移至唯一使用者 [app/ide.cpp](app/ide.cpp)（顺带显式补齐其原靠透传获得的 QMap/QCloseEvent/QGridLayout/vector/memory/string 包含）。收益：唯一其他包含者 main.cpp 不再级联引入 52 个面板头，ide.h 达成自包含（声明什么包含什么）。**边界说明**：ide.cpp 直接使用全部面板类型（构造/接线），面板头变更连带重编 ide.cpp 的既有行为不变——2026-10-03 审计"面板改动重编 8821 行 ide.cpp"项的完全消除需 Pimpl/间接层改造，超出卫生改动范畴，记录为后续方向。
  - **TeachingPanelBase 迁移（6/约42）评估结论——当前应用状态下无可达收益，不实施**：主题接线收益不可达——深色主题已移除（2026-07-06），`Theme::setThemeMode(LIGHT)` 全项目仅启动时调用一次（彼时懒加载教学面板尚未构造），`Theme::setThemeColor` 全项目零调用点（主题色运行时不变），即 `onThemeModeChanged` 回调在面板生命周期内从不触发；15 个使用 TeachingTheme 色板而未接主题回调的面板中 8 个含动态主色（primary/accent），但颜色不存在过期路径。基类头注释所列后续扩展点（setRunning 运行态钩子等）经排查无对应样板存在，新手引导/帮助文案已由 TeachingPanelHeader 中心化兜底（42/42）。继续迁移 36 个存量面板属零行为变化的大面积搅动（且有基类/子类双布局冲突前科），中止迁移；新面板仍应继承基类，该项自遗留债务移除。
- **协程真挂起架构落地（第四轮架构级优化；语义变更，[ADR-007](docs/adr/ADR-007-coroutine-suspension.md)）**：全量 4123 用例回归通过（4118 + 5 新增），构建 /W4+/WX 零警告。生成器 `.next()` 由"重放模式"（每次从头重执行函数体，运行时 yield 计数器跳过已消费 yield）升级为"真挂起模式"（yield 保存执行位置切回调用方，下次 `.next()` 从挂起点恢复）——**前缀副作用只执行一次**（重放模式下每次 `.next()` 重复执行前缀副作用，属已知限制），n 次 `.next()` 总代价由 O(n²) 降为 O(n)。一致性 checklist 见 [backend-consistency.md §9](docs/specs/backend-consistency.md)。
  - **历史勘误**：`InterpreterCoroutine.cpp` 头注释曾声称"StackVM/RegisterVM 用真挂起模式（D.5 实现）"——与代码不符，四后端原均为重放模式（VM 的 OP_YIELD/REG_YIELD 同样走计数器跳过，见 `docs/development.md` R164 条目"实现四后端语义一致性（重放模式）"）。真挂起必须三执行后端同步落地（ADR-002：同一平台上不允许单后端分叉），本轮同步完成。
  - **Interpreter（fiber 真挂起）**：新增 [interpreter/CoroutineFiber.h](interpreter/CoroutineFiber.h) 平台门控（`MINILANG_CORO_FIBER`，当前 Windows fiber API；非 Windows 平台三后端统一保留重放路径，与 JIT 平台降级同模式）。`Interpreter::callCoroutineNext` 走 fiber 切换：递归树遍历解释器的 C++ 栈即延续（零语义损失，支持 yield 位于嵌套函数调用/表达式中间），挂起时快照环境链根 + callStack 后缀（GC 根防 envPool 复用与失根）+ 返回类型 + 递归深度；错误经 `exception_ptr` 封送重抛（对齐重放的错误传播与 done 语义）；yield 表达式恢复值 = 自身 yield 值（逐点对齐重放 skip 语义）。
  - **StackVM/RegisterVM（帧链快照）**：OP_YIELD/REG_YIELD 命中时将帧链、栈切片（StackVM）/寄存器窗口（随帧内嵌）、try 栈、开放 upvalue 快照进 `CoroutineData::vmSuspension`（`shared_ptr<void>`，类型定义于各 VMCalls.cpp）；`callCoroutineNext` 恢复路径按新入口栈深/帧深**重定基**（`basePointer`/`stackBase`/`returnIp`/upvalue 槽位为绝对索引，两次 `.next()` 的调用方深度可不同），最外层生成器帧 `returnIp` 重定基到本次调用点（自然完成时 OP_RETURN/REG_RETURN 按其恢复调用方）。快照/恢复纯堆容器操作，平台无关。
  - **嵌套挂起上下文**：生成器体内 `await`（drain 循环）与生成器调用生成器会嵌套进入 `callCoroutineNext`——登记前保存外层挂起上下文、全部退出路径恢复（VM 两字段 / Interpreter `activeCoroutineFiber_` prevActive 同模式），修复 await+yield 混合场景外层 yield 落回重放分支。
  - **JIT**：含生成器的程序经 CompileError 整体回退 VM（既有行为），自动继承 VM 挂起语义，无需改动。
  - **新语义锁定测试**（`CoroutineSuspension.*` 5 项，四后端一致）：副作用只执行一次（`"bbb"` vs 重放的 `"bbbbbb"`）、yield 表达式恢复值 = 自身 yield 值、跨挂起点局部状态连续性、try/catch 跨挂起点（异常处理状态随快照恢复）、300 次 `.next()` 长序列值正确。
  - **已知限制（有意记录）**：① 非 Windows 平台三后端统一保留重放模式（POSIX ucontext 为后续方向，需配套 ASan fiber 标注）；② 挂起的 Interpreter fiber 栈上 C++ 析构不执行——协程弃用/解释器销毁路径存在有界一次性泄漏（env 引用计数随栈内存遗忘）；③ 生成器体内无法对自身 `.next()`（挂起模式不支持自恢复，显式报错）。

- **IR 编译路径优化第四轮（LICM 结构分析复用 / 操作数小向量）**：全量 4118 用例回归通过（4109 原有 + 9 新增），均为无语义变化的实现层改动。
  - **LICM 结构分析跨不动点批次复用**：[compiler/ir/IRSSA.cpp](compiler/ir/IRSSA.cpp) `licmPass` 的 DominatorTree（CHK 不动点，最坏 O(N²)）与 NaturalLoopInfo（回边扫描）由"每个外提批次全量重建"改为建一次复用，每轮仅重建 IRCFG 取新鲜指令区间（P2-10 语义保留）。正确性论证：LICM 只增删纯计算指令（`isPureComputeForSSA` 不含 LABEL/终结指令），块图定界符全程不动，节点分区唯一可能的变化是"非 LABEL 起始 gap 节点的指令被全部外提后该节点消失"且外提插入不产生新节点——节点数严格单调不增，**节点数不变 ⇔ 分区不变 ⇔ 节点 id 语义不变**；每轮重建 IRCFG 后比对节点数，检测到分区塌缩即重建分析三件套（`analysisCfg` 原地赋值保持地址稳定，`domTree`/`loopInfo` 的 `cfg_` 引用经 unique_ptr 重建不悬垂），退化为原每批重建行为。查询期 `dominates`/`loops` 只读已存数据、不解引用 `cfg_`。新增 `LICMCollapseGaps3.CollapsedGapRebuildStillHoistsSecondLoop`（手工构造双循环 + "gap 块仅含可外提纯计算" IR，触发两次塌缩重建，锁定重建后分析对第二循环外提仍正确）。
  - **IROperandList 操作数小向量**：[compiler/ir/IR.h](compiler/ir/IR.h) 新增 `IROperandList`（≤4 操作数内联存储免堆分配，超出回落堆缓冲；IROperand 平凡可复制故按元素复制无构造/析构分叉），`IRInstruction::operands`、`IRInstruction` 构造与 `AstIRBuilder::emitIR` 签名迁移——原 `std::vector<IROperand>` 使每条 IR 指令携带一次独立堆分配，遍布 IR 构建 / pass 指令复制（inlinePass 复制被调函数体）/ lowering 全链路。接口收敛到现有使用面（size/empty/operator[]/push_back/reserve/迭代/initializer_list 赋值），12 个文件 274 处调用点机械迁移（`std::vector<IROperand>{...}` → `IROperandList{...}`）。新增 `IROperandListTest` 8 用例（内联/堆两路径复制独立性、移动后源空、跨容量增长保序、initializer_list 赋值、clear 后堆缓冲复用可见性、reserve 预留）。
  - **剩余优化项清账评估（未实施，记录结论）**：① Interpreter 协程 await 重放 O(n²)——根治需真挂起模式（VM 的 D.5 架构），Interpreter 与 VM 为两套模式靠语义对齐，属架构级改造；② Interpreter 类实例字段 O(哈希) vs VM 帧槽镜像 O(1)——VM 的 `fieldSlotIndex` 是帧槽镜像机制而非哈希表替换，Interpreter 补同款镜像属架构级；③ IR 深水区其余项（copyPropagation 重写 / GVN undo-log / Token lexeme string_view / 行列元数据 RLE）维持第三轮跳过结论。

- **GUI/服务层优化第三轮**：全量 4109 用例回归通过，均为无语义变化的实现层改动。
  - **`executeWithDetail` 单遍化**：[common/BackendExecutionService.cpp](common/BackendExecutionService.cpp) 的 `execute()` 新增可选 `BackendExecDetail*` 出参，generatorChunks / opcodeCounts / peakTrackedCount 在单次执行内联收集——原设计为收集这三项数据把全流程（Lexer→Parser→compile→execute）跑两遍。注意 detail 模式耗时含 step 采样开销（每条指令一次回调，原两遍方案首遍计时不含），profile 面板计数与耗时口径自洽。CoroutineVisualizerPanel / ProfileDashboardPanel 直接受益。
  - **调试面板安全网轮询纪元门控**：[app/VmStepper.h](app/VmStepper.h) 新增 `stateEpoch`（stepOnceActive/reset/stop 递增），[app/IdeController.h](app/IdeController.h) 转发 `vmStateEpoch()`；CallStackPanel / VariableInspectorPanel / BytecodeTracePanel / WatchPanel 四面板的 2s 安全网定时器在纪元未变时跳过 refreshLive（clear/rebuild + getVmStack/getVmGlobals 状态快照全量深拷贝）——空闲时 7 个调试面板每 2s 全量重建 UI 树的开销消除。状态变更的即时刷新仍由 OPT-1 vmStateChanged 观察者推送；用户手动刷新与表达式编辑直接调用刷新函数，不受门控影响。
  - **补全词刷新复用管线 token 流**：[app/ide.cpp](app/ide.cpp) `updateCompletionWords` 在文档 revision 与最近一次语法检查一致时直接从 `controller_->lastTokens()` 提取声明符号（var|fun|class + 标识符），免 toPlainText 全文拷贝 + 2 次全文正则剥离 + 1 次全文正则扫描；revision 不一致（启动首次调用/未走管线场景）回退原正则路径。万行文件上与语法检查叠加的打字卡顿缓解。
  - **GVN 作用域表结构优化评估（未实施）**：支配树 DFS 的每子块整表拷贝可改 undo-log，但正确性依赖严格 SSA 下兄弟块替换映射的 no-op 性质论证，风险与编译期收益（仅寄存器路径、大函数可感）不匹配，记录延后。
  - **排查注记**：ide.cpp 内非限定名 `TokenType` 作类型声明会被 Qt `qxmlstream.h` 的嵌套 `TokenType` 搅乱（C2146/C4430），用 `auto` + 限定名 `TokenType::TK_VAR` 规避——该 TU 中声明枚举类型变量一律用 `auto`。

- **IR 管线优化第二轮（字节码质量 / 内联 / 缓存）**：全量 4109 用例回归通过，除内联级联行为增强外均为无语义变化的实现层改动。
  - **短路求值暂存槽共享**：[compiler/ir/AstIRBuilder.cpp](compiler/ir/AstIRBuilder.cpp) 新增 `acquireShortCircuitSlot()`——块作用域内沿用逐次分配（由 slotBase 回滚回收），顶层（blockScopes_ 为空）改为整个函数共享一个暂存槽。原实现顶层每个 `and/or` 永久占用一个槽（`nextLocalSlot_++` 永不复用），localCount 随布尔表达式数量单调增长，33 个顶层布尔表达式即触发 RegisterVM 32 寄存器上限告警致函数无法加载。嵌套 and/or 的槽使用严格括号化（内层完成回写后外层才写回），共享安全；函数入口/嵌套函数返回/块回滚三处失效点重置缓存。
  - **新增分支折叠 pass**：[compiler/ir/IROptPasses.cpp](compiler/ir/IROptPasses.cpp) `branchFoldingPass`（设计说明见 [compiler/ir/IR.h](compiler/ir/IR.h) 声明处），接入 optimizeIR 序列（常量折叠之后、DCE 之前，无条件启用）——① 常量条件分支化简（`1<2` 折叠出的 bool 常量此前无人消费：恒真删分支、恒假替换为 JUMP；JUMP_IF_FALSE 为 peek 零栈效应，移除/替换不改栈平衡）；② 跳转链线程化（JUMP→LABEL→JUMP 重定向，防环）；③ 跳转到紧邻 LABEL 消除；④ 不可达代码删除（无条件转移后到下一 LABEL，与 BytecodeIRBackendStackCheck 死区口径一致）。此前常量折叠的结果没有任何 pass 消费，`if true`/`while false` 等全部原样进运行时。
  - **inlinePass 级联塌缩**：[compiler/ir/IRSSA.cpp](compiler/ir/IRSSA.cpp) 内联迭代到不动点（最多 3 轮，总指令预算跨轮共享）并扫描 caller 全部块——原实现仅扫 blocks[0]（含控制流的 caller 中其他块的 CALL 永不内联）且单趟不重扫内联暴露的新 CALL（a→b→c 传递内联失效）。**行为增强**：`InlineCascadeGaps3` 两个测试由锁定旧缺口（单趟残留 CALL/需多趟调用收敛）更新为锁定新行为（单次调用即塌缩/再次调用收敛），测试名保留，断言与注释同步改写。
  - **BytecodeCache 进程内 memo**：[compiler/core/BytecodeCache.h](compiler/core/BytecodeCache.h)/.cpp 新增 `tryMemo`/`storeMemo`/`tryMemoRegister`/`storeMemoRegister`（LRU 近似，8 条目上限，mutex 保护；clear() 同步清空），[app/PipelineRunner.cpp](app/PipelineRunner.cpp) 两条缓存路径接入——IDE 每次运行的 tryLoad 此前都做全文件读取 + FNV-32 校验 + 全量反序列化（常量池/行号表逐项重建），memo 命中仅需一次结果深拷贝。键与磁盘缓存同源（fnv1a64(source)）并折叠 compilerMode/optFlags/mtime。
  - **JIT 跨运行缓存评估结论（未实施）**：读 [compiler/jit/JIT.cpp](compiler/jit/JIT.cpp) execute 路径确认，复用编译产物的前提是 per-chunk 独立编译（当前为单体 CodeHolder：main + 全部函数编进一个入口）且 codegen 不内嵌 chunk 对象地址（常量池指针），属架构级改造，超出"中等工作量"预估，记录为后续方向。

- **四方向运行时性能优化（VM 热路径 / GC / 字典查找 / 输出管线）**：全量 4109 用例回归通过，均为纯实现层改动、无语义变化。
  - **VM 主循环**：[compiler/backend-stack/VM.h](compiler/backend-stack/VM.h) 与 [compiler/backend-reg/RegisterVM.h](compiler/backend-reg/RegisterVM.h) 补齐全部一级分发桶的 `MINILANG_FORCE_INLINE`（原注释声称已标记但实际遗漏，GET_GLOBAL/OP_CALL/OP_RETURN 等最高频指令每条需经历一次真实函数调用 + 桶内二次 switch）；字节码截断检查从逐指令上移至加载期——`VM::initExecution` / `RegisterVM::initExecution` 新增 `validateChunkInstructionBoundaries` 预扫描（变长 OP_CLOSURE 展开逻辑与原逐指令检查逐点等价），release 主循环零检查开销，debug 保留逐指令校验双保险。
  - **GC 热路径**：[interpreter/Interpreter.cpp](interpreter/Interpreter.cpp) 根集收集改为只读遍历 `localVariables()`（零拷贝，取代 `snapshotLocalVariables()` 每次触发 GC 的 O(可见变量数) 次堆分配 + 2N 原子操作），并按 `visitedEnvs` 对重叠环境链去重；[interpreter/GcManager.cpp](interpreter/GcManager.cpp) mark 阶段整个 collectCycle 共享一个 worklist（原每个根元素各建一个 vector）；统计字段迁移 `std::atomic`（GUI 轮询读路径免锁，原 MemoryModelPanel 单次取 8 个统计值 = 8 次加锁）；`checkPendingGc` 无锁快速路径（原每条语句一次 recursive_mutex）。
  - **引用计数内存序**：[interpreter/RefCounted.h](interpreter/RefCounted.h) `release()` 由 `fetch_sub(acq_rel)` 改为 shared_ptr 同款惯用法 `fetch_sub(release)` + 归零后 acquire fence——非归零递减（绝大多数路径）省去 acquire 半序。
  - **字典透明查找**：启用 R97 #2 已实现但零调用方的 `is_transparent` 基础设施——[interpreter/Value.h](interpreter/Value.h) 新增 `dictKeyIsValid` / `dictFindValue` / `dictGetOrInsertRef`（string 键走 `string_view` 异构查找，免 DictKey{string} 深拷贝；补对称反向重载保证跨 STL 比较顺序兼容），Interpreter / StackVM / RegisterVM 三后端与 BuiltinMethods 共 13 处索引读写与 get/has/remove/set 调用点接入（字典字面量构造路径保留拥有型键，语义不变）。
  - **GUI 输出合批**：[app/ide.cpp](app/ide.cpp) `appendOutput` 改为 30ms 单发定时器攒批（`flushPendingOutput` 内 `setUpdatesEnabled(false)` 批量 append 一次重绘）——密集 print 程序不再每行投递一个跨线程事件 + 一次 HTML 解析 + 一次重绘；`clearOutput` 同步丢弃挂起缓冲。

- **D1 第二步（表驱动 + 文件拆分）**：栈效应知识迁移为数据表——[compiler/ir/BytecodeIRBackendStackCheck.cpp](../compiler/ir/BytecodeIRBackendStackCheck.cpp)（自 BytecodeIRBackend.cpp 拆分，1929→1493+456 行）承载 `kFixedStackEffect` constexpr 效应表（64 条固定效应）+ 10 条公式型分支，`static_assert(tableIsComplete())` 在编译期强制全部 IROp 枚举值登记（新增 IROp 未补表即编译失败，关闭"静默跳过校验"缺口）；PHI 等不可 lower 操作的未登记路径由保守跳过改为显式拒绝；新增 `AuditBatch1IRBalance.EffectTableCoversAllIROps` / `PhiInstructionRejectedByLower` 测试。
- **compiler/ 子目录拆分（审计问题 1 落地）**：`compiler/` 41 个文件按职责迁入五个子目录——`core/`（Compiler 全家族、Bytecode、BytecodeCache、ExprStmtPop、GlobalSlotAllocator 共享基础设施）、`ir/`（IR、IRSSA、IROptPasses、AstIRBuilder、BytecodeIRBackend + 栈平衡校验器）、`backend-stack/`（VM、VMCalls、VMContainers）、`backend-reg/`（RegisterBytecode、RegisterBytecodeBackend、RegisterVM 全家族）、`jit/`（JIT 全家族，含 JITA64）。仓库内 include 均为根相对路径，批量更新为 `compiler/<子目录>/<文件>.h`；`cmake/minilang_core.cmake` 源列表同步；`docs/changelog/archive/` 历史条目保持时点原貌不回写。4109 用例全量回归验证通过。

### Fixed

- **实时语法检查诊断双重渲染（警告重复行）**：[app/ide.cpp](app/ide.cpp) `runRealTimeSyntaxCheck` 调用 `runFrontendPipeline` 时，PipelineRunner::runLexer/runParser 在主线程同步 emit 的 `diagnosticsReady` 先由 `displayDiagnostics` 完整渲染一遍（错误 + 警告 + summary 全部进面板），函数内随后又对同一批诊断迭代渲染——警告在输出面板重复显示且随每次 300ms 防抖累积，"--- N 问题 ---" 汇总行持续刷屏，错误消息双倍拼写纠错/文本构建。修复为管线调用期间 `QSignalBlocker` 抑制 controller_ 转发（本函数成为唯一渲染方，排序展示行为不变），onRun 等其他 diagnosticsReady 消费路径不受影响。
- **IR 路径顶层/嵌套函数 super 调用栈不平衡（D1 校验器捕获）**：[compiler/ir/AstIRBuilder.cpp](compiler/ir/AstIRBuilder.cpp) `emitMethodCallWriteback` 的 super 分支无条件补 `POP` 消费 `LOAD_MUTATED` 残留（BUG-INH-4 注释假设 `this` 是 LOCAL 槽 0），但非方法上下文（顶层、嵌套普通函数）中 `resolveVar("this")` 回退到 GLOBAL——`OP_SET_GLOBAL` 为 pop 语义，多发一个 `POP` 使 `var x = super.foo()` 的 `DEFINE_GLOBAL` 欠栈。此前被"未定义的变量： this"运行时错误先触发而掩盖（静态字节码不平衡从未被执行到）。修复为条件对齐 isVarRef 分支：仅 LOCAL/UPVALUE（peek 语义）补 `POP`。方法体内合法 super 调用的发射序列不变。
- **Environment 弱指针悬挂风险收口（D2）**：`Value::makeClosure` 工厂内统一执行 `env->markClosureEnvRef()`（[interpreter/Value.cpp](interpreter/Value.cpp)，实现随 D2 fix 移出 Value.h——Environment 仅有前向声明，避免循环包含）。原先该标记由 Interpreter 唯一闭包创建点手工调用（AUDIT-BUG-I1），属"人工保证"不变量：新增创建点漏标即静默语义损坏（envPool_ 回收复用后 `resetForReuse` 清空闭包 weak_ptr 仍指向的 variables）。收口后不变量结构性成立。回归测试 `AuditBatch1ClosureEnv.*` 3 用例（含 >64 次 env 池回收压力下的循环闭包行为回归）。
- **TeachingPanelBase 双布局冲突隐患（审计问题 3 迁移修正）**：基类构造函数原先无条件 `new QVBoxLayout(this)`，而已迁移的三个面板构造函数仍自建顶层布局——QWidget 仅允许一个顶层布局，子类二次创建会被 Qt 拒绝（运行时告警 + 子控件失去布局管理，面板显示错乱；此前的编译/测试验证未覆盖 GUI 运行时布局路径）。修复为 `rootLayout()` 惰性创建/采纳：子类已自建布局则直接采纳，否则才创建标准零边距容器。同步迁移调试家族三面板（DebugPanel / WatchPanel / WatchpointPanel）到基类：主题切换接线收敛为 `applyTheme()` override，gui/ 内手工 `Theme::onThemeModeChanged` 样板清零（审计问题 3 按家族迁移第二步）。
- **JIT 异常分派 use-after-pop 容器溢出（MSVC ASan 门禁首捕获）**：[compiler/jit/JITRuntime.cpp](compiler/jit/JITRuntime.cpp) `jitThrow` 命中 handler 后先 `tryStack_.pop_back()` 销毁栈顶 `JitTryHandler`，再经 `back()` 引用读 `handler.catchAddr` 返回——MSVC STL 容器注解将 [size, capacity) 标记为毒化区，ASan 以 container-overflow 拦截（TestJIT.R162\* 12 项 + L14RuntimeErrorCatch.\* 6 项；非插桩构建因旧槽位尚未被覆写而侥幸通过，属潜伏 UB）。修复为 pop 前按值复制 `catchAddr`；修复后 R162/L14 全家族 26 项在 ASan 下零报告，非插桩全量回归 4109/4109 通过。缺陷由本次落地的本地 MSVC ASan 验证流程首次捕获（CI 合入提案见 [docs/ci-msvc-asan-proposal.md](ci-msvc-asan-proposal.md)）。
- **Windows 增量构建依赖跟踪失效根治（debug 构建树）**：`out/build/debug` 编译器检测缓存（`CMakeFiles/<版本>-msvc1/CMake{C,CXX}Compiler.cmake`）存有历史乱码 `msvc_deps_prefix`（代码页不一致的 configure 遗留），导致每次 reconfigure 都把乱码写回 `rules.ninja`、ninja 依赖解析丢失（改头文件不重编），此前只能每次手工注入 GBK 字节。对照实验确认真因后（正常 936 代码页下全新 configure 的探测与生成本就正确、VSLANG=1033 因未装英文语言包无效），将缓存中的前缀修正为规范 UTF-8 文本——此后 regen 自动产出与构建期 cl（GBK）匹配的前缀，**无需任何手工注入**。端到端验证：`ninja -t deps` 记录有效、touch 头文件正确触发重编、全量 4109/4109 通过。排查指引已更新至 [docs/getting-started.md](getting-started.md)（原"修正 rules.ninja"指引属治标，已标注）
- **构建入口脚本在中文控制台下失效修复（`scripts/`，经用户授权的一次性修改）**：`_common.bat` / `configure.bat` / `build.bat` / `run_tests.bat` 内的 UTF-8 中文注释在默认 GBK（代码页 936）控制台下被 cmd 逐行误解析为命令（REM 行同样会被分词，全角标点的 UTF-8 字节按 GBK 配对错位后暴露命令碎片，如 `'锛孧SVC' 不是内部或外部命令`），导致 configure.bat 在运行 cmake 之前即异常退出（实测日志止于环境检测 INFO、exit 1）。修复：全部 .bat 注释转写为 ASCII 英文（逻辑零改动，对任意控制台代码页免疫），顺修 configure.bat 的 `>/dev/null` 重定向（cmd 无此语义，改 `>nul`——此前重定向失败会令 windows-msvc-\* 预设的 QTDIR 校验被静默跳过）；另为 5 个含中文且无 BOM 的 .ps1 补 UTF-8 BOM（Windows PowerShell 5.1 无 BOM 时按 ANSI 误读）。端到端验证：configure.bat 完整跑通 cmake（exit 0、零乱码），reconfigure 后 deps 前缀保持正确 GBK，build.bat / run_tests.bat 全流程通过（4109/4109）。

## [v1.3.0] - 2026-07-31

插件系统与沙箱模式落地，教学板块面向初学者做六维体验优化，JIT 修复 Linux 下 SysV ABI 违规导致的 SIGSEGV。

### Added

- **插件系统**：C API 新增 `minilang_load_plugin`，插件经 DLL 回调表 ABI 调用 `minilang_register_function` 注册原生能力；测试插件 DLL 端到端覆盖（[tests/plugin/test_plugin.cpp](tests/plugin/test_plugin.cpp)）。
- **沙箱模式**：`minilang_set_sandbox` 暴露 `sandboxEnabled` / `AllowInput` / `AllowImport` 原子开关——禁 input、禁文件模块导入（`std/` 内建模块仍放行）、禁插件加载；13 用例覆盖三后端 × 三类拦截。
- **HostFunctionRegistry**：宿主函数全局注册表（[common/HostFunctionRegistry.h](common/HostFunctionRegistry.h)），三后端在"未定义函数"回退路径统一查表调用，语义天然一致。
- **async/await 阶段 4 测试补全**：新增 [tests/TestAsyncAwait.cpp](tests/TestAsyncAwait.cpp) 21 用例，覆盖 await/yield 混合、`std/async` 调度器（runAll round-robin / runTask）、四后端一致性。
- **教学板块六维体验优化**：
  - 4 → 6 分类重组（[gui/PanelCatalog.cpp](gui/PanelCatalog.cpp)）：原「执行引擎」23 项平铺拆分为「执行引擎 / 内存与优化 / 调试与观测」三类，按编译原理学习曲线递进排序。
  - 难度分级：`PanelEntry` 新增 `level` 字段（1 入门 / 2 进阶 / 3 高级），分类内按难度非递减排序。
  - 新手引导覆盖 12 → 42/42：通用兜底引导从 `TeachingPanelHeader::helpDocFor()` 派生 3 步 GuidedTour，无专属引导的面板不再弹"暂无引导"。
  - 帮助文案层次化：弹窗新增"🎯 难度：X · 分类：Y"提示行，8 条薄文案补一段加粗核心概念讲解。
- **教学库内容扩充**：OpCode 文档 46 → 75、变量类型示例 17 → 23、闭包场景 12 → 16。

### Changed

- 教学面板标题栏 / 帮助弹窗 / 横幅 / 搜索框 / 树样式共 20+ 处硬编码颜色迁移到 `TeachingTheme` 语义色，主题色变更自动跟随 QFluentKit。
- 3 个手写子页切换面板（VmStackSandboxPanel / JitVisualizerPanel / BytecodeTracePanel）统一改用 `TeachingSubPageBar` 组件，采用组件的面板从 3 → 6 个。

### Fixed

- **JIT SysV ABI 违规（P0，Linux SIGSEGV）**：[compiler/jit/JITCodeGen.cpp](compiler/jit/JITCodeGen.cpp) / [compiler/jit/JITCodeGenHelpers.cpp](compiler/jit/JITCodeGenHelpers.cpp) 中 6 处运行时回调硬编码 Win64 参数寄存器 rcx/rdx，Linux SysV ABI 应为 rdi/rsi，导致 ctx 指针传入垃圾值，TestJIT OSR/Deopt 16 项测试 SIGSEGV。按平台 `#ifdef` 选择参数寄存器修复。
- **教学面板 closing_ UAF（P0）**：`ensureTeachingPanelCreated` / `showTeachingPanel` / `showQuickPanelJumpDialog` 补 `closing_` 检查，避免关闭流程中 `maybeSave` 模态事件循环派发挂起回调时懒构造"孤儿"面板。
- **reverse-timeline 懒加载工厂未注册（P1）**：面板已登记且实现完整但工厂从未注册、源文件未入 CMake，教学树点击静默回退编辑器。补 [cmake/minilang_core.cmake](cmake/minilang_core.cmake) 源列表 + [app/ide.cpp](app/ide.cpp) 工厂注册。
- **教学样例语法错误（P1）**：ClosureInspector 全部 16 个样例 + VariableInspector 引导样例长期使用无括号 `print counter();`，但 `print` 强制要求 `(`，加载样例后必然 parse 失败。全部修正为 `print(...)`。
- **CI Linux GCC -Werror 全量补全**：18 文件 sign-compare / unused-function / warn_unused_result / `__forceinline` 平台守卫 / asmjit SYSTEM 抑制第三方 -Wpedantic / Ubuntu lrelease 单横线 `-version`（Qt 不接受 `--version`）/ app/ide.cpp Qt 6.8 弃用 API / StepExplainerPanel switch 缺 case 等。
- 新手引导 `kAutoTourPanels` 12 → 37 面板首访自动引导；`onPanelGuidedTourRequested` 补 `watchpoint` 路由分支。
- 帮助文案 `watchpoint` / `reverse-timeline` 两条条目补全至 42/42。

## [v1.2.0] - 2026-07-29 ~ 2026-07-30

七特性 MVP 阶段性收尾：宏模板、trait/mixin、async/await、`?` 错误传播四项语言能力三后端全链路落地；运算符重载（dunder）与 const fun 编译期求值补齐。

### Added

- **宏模板（语言）**：新增 [ast/MacroExpander.h](ast/MacroExpander.h) / .cpp——`macro name(p1,p2) { <expr> }` 表达式模板宏，`name!(args)` 调用 `cloneWithSubstitution` 克隆宏 body 并替换形参 VarRef（同 C 宏语义，同一形参多次出现共享实参子树），递归深度上限保护；parse 期展开后三后端零改动。
- **trait/mixin（语言）**：新增 `TK_TRAIT` / `TK_WITH` 词法 + `TraitDecl` AST 节点，`trait T { fun m() {...} }` + `class C with T1, T2`（可与 extends 并存），trait 方法在 parse 期合入混入类（四后端零改动）。
- **async/await 阶段 4（语言）**：`TK_ASYNC` / `TK_AWAIT` 词法，复用 R164 协程重放模式；调度器以纯 MiniLang 实现于内建模块 `std/async`（`runAll` / `runTask`）。
- **`?` 错误传播（语言）**：`?` 脱糖为共享内联 `__qmark_unwrap`（三后端自动一致），`Ok` / `Some` 解包、`Err` / `None` 可捕获传播；配套 `std/result` 泛型 enum 模块。
- **运算符重载 / dunder 分派（语言）**：`__add` / `__sub` / `__mul` / `__div` / `__mod` instance 算术分派——Interpreter 在 numericBinaryOp 报错前分派（含继承链查找与 1 参数校验），StackVM 在 executeArithOps 帧注入，RegisterVM 在 executeArith 经 executeCallImpl 注入（含 methodCache 缓存），JIT 适配。无 dunder 时回退原"算术运算需要数值类型"报错。
- **const fun 编译期求值（性能）**：新增 [common/ConstFunEval.h](common/ConstFunEval.h)（header-only 三后端共享），折叠条件为「直接名称调用 + 实参全字面量 + arity 匹配 + 独立 Interpreter 沙箱求值成功无副作用 + 结果原始类型」；任一条件不满足返回 `nullopt` 回退运行时调用。

### Fixed

- **构建：tests/CMakeLists.txt PCH 缺失**：为 `minilang_app_tests` / `minilang_gui_smoke` 补挂 PCH，修复 fresh 重编译时 GcManager 单例 LNK2005 / LNK1169（unity-build ODR 预存缺陷清偿）。
- **CI 5 个失败任务**：ubuntu `lrelease6` 缺失（补装 `qt6-tools-dev`，三名兜底）；macOS Clang 4 项编译错误（含 `Interpreter.h` `atomic<shared_ptr>` → `mutex`）；Windows `/WX` C4189；Docker 补 `-DMINILANG_BUILD_TEST_HARNESS=OFF -DMINILANG_BUILD_CLI=OFF`；89 文件 clang-format 就地格式化；弃用 actions 升级（`upload-artifact@v6`、`msvc-dev-cmd@v1.13.0`）。
- **CrashHandler.cpp Unity build 污染**：`windows.h` 必须先于 `dbghelp.h` 包含。

## [v1.1.0] - 2026-07-26 ~ 2026-07-28

已知限制清偿批次（B1 Interpreter TCO 蹦床 + B1-Shadow 三 VM 死循环 + E3 channel.recvTimeout + E1 stale obj 自动修复），AUDIT-R2/R3 系列审计修复收尾，ARCH-10 架构缺陷修复（GUI 层解耦 + Unity Build + Facade 瘦身）。

### Added

- **Interpreter 尾调用蹦床（TCO）**：消除三后端最后一个已知设计差异（testing.md 限制 #10）。`visitReturnStmt` 用与 VM 共享的 `TCO::identifyTailCall` 识别自尾调用，求值实参后抛 `TailCallSignal`；`callNamedFunction`（SelfFunction）与 `invokeMethod`（SelfMethod）的蹦床循环捕获信号后重建环境重新执行函数体，C++ 递归深度恒定。10 万层深尾递归 / 方法尾递归 / 互递归 / 闭包逐轮捕获均通过。
- **channel.recvTimeout(ms)**：四后端单点共享分发（[interpreter/BuiltinMethods.cpp](interpreter/BuiltinMethods.cpp) `handleSyncObjectMethod`），等待至多 ms 毫秒；有消息返回消息，超时或已关闭且无消息返回 null；参数校验：非整数 / 负数 / 超 60s 上限报错。`MAX_CHANNEL_TIMEOUT_MS = 60000`。
- **scripts/fix_stale_objs.bat**：定向删除四个核心 OBJECT 子库 obj + `minilang_core.lib`，自动修复 Ninja stale unity obj 链接失败（无需全量重建）。[scripts/build.bat](scripts/build.bat) / [scripts/run_tests.bat](scripts/run_tests.bat) 内置 LNK2019/2001/1120 自动清理 + 重试一次。
- **L18 Interpreter 状态快照与回滚**：为反向调试提供基础设施。`StateSnapshot` 捕获环境链 / 调用栈 / 注册表 / 模块状态 / 控制状态；`captureStateSnapshot` / `restoreFromSnapshot` 配套；`ExecutionTraceRecorder::TraceSnapshot` 新增 `interpreterState` 类型擦除字段。
- **L21 DAP `pause` 请求同步暂停**：`doContinue` 循环每 `kPausePollInterval=256` 步检查 `pauseRequested_` 标志 + 调用 `stdinPollCallback_` 探测 stdin。跨平台非阻塞探测：Windows `PeekNamedPipe` / `_kbhit`，Unix `poll(stdin, 0)`。

### Changed

- **GUI 层与引擎层解耦（ARCH-10 P1）**：新增 [common/MemoryInspectionAPI.h](common/MemoryInspectionAPI.h) / .cpp（`NaNBoxSnapshot` / `HeapObjectSnapshot` / `GcStatsSnapshot` 三个纯数据快照），消除 MemoryModelPanel / GcVisualizerPanel 对 `interpreter/NaNBox.h` / `RefCounted.h` / `GcManager.h` 的直接依赖。新增 [common/BackendExecutionService.h](common/BackendExecutionService.h) / .cpp 中间层，封装 `Lexer → Parser → Compiler → Backend` 完整流程，ProfileDashboardPanel / PerformanceRacePanel / BackendParallelPanel / BugHuntPanel / ExerciseGraderPanel / CoroutineVisualizerPanel / BackendComparePanel 等 7+ 面板迁移到此服务。
- **IdeController Facade 瘦身（ARCH-10 P2）**：新增 6 个子组件访问器（`pipelineRunner()` / `workerManager()` / `debugCoordinator()` / `vmStepper()` / `interpreter()` / `debugController()`），新面板优先通过访问器获取子组件，避免在 Facade 层堆砌纯透传方法；老面板透传 API 保持向后兼容。
- **Unity Build 默认可启用**：[cmake/minilang_core.cmake](cmake/minilang_core.cmake) 用 `SKIP_UNITY_BUILD_INCLUSION ON` 将含 `windows.h` 的 `CrashHandler.cpp` 独立编译，避免污染同 batch 的 `lexer/Token.h`；`minilang_frontend` / `minilang_backend` 子库 `AUTOMOC OFF`。冷构建加速 30-50%。
- **BytecodeCache 格式版本 1 → 2（AUDIT-R3 P2-4/P2-5）**：optFlags（irOptimize / irSSAOptimize 位图）入键，切优化配置后旧缓存失效；`moduleExports` 段无条件读写（移除 `remaining()>=4` 启发式）。

### Fixed

- **B1-Shadow：三 VM 路径 TCO 遮蔽误判导致死循环（P1）**：`fun f(n) { var f = helper; return f(n + 1); }` 局部变量遮蔽函数名，`TCO::identifyTailCall` 仅按名称匹配误判自递归，TCO 跳回自身入口形成死循环直至指令预算耗尽。修复为 `CompilerStmt.cpp` / `AstIRBuilder.cpp` 用 `currentLocals_` / `varMap_` 检查遮蔽，命中时回退普通调用。
- **AUDIT-R3 系列（VM / Interpreter / GcManager / IROptPasses / BytecodeCache / AstIRBuilder）**：
  - `push` / `pop` / `popN` 栈溢出 / 下溢改用不可捕获 `fatalError`——原 `runtimeError` 在活动 try 上下文转 `throwException` 改写 `frame.ip`，void 语义调用方随后 `ip += n` 使执行点错位。
  - `openUpvalues_` 悬垂清理改用 `lower_bound(newStackSize)` 定位尾部区间。
  - `OP_ADD/SUB/MUL_INT_SPEC` 类型特化路径补溢出检测，与通用路径 `computeArith` 的 IntOverflow 语义对齐。
  - finally 求值前保存并清除 `loopFlow_`——try 块内 break 后标志残留使 finally 块只执行第一条语句即中断。
  - 增量 GC 根集收集改为环境链全层遍历（含模块缓存环境 / REPL 暂存模块缓存）。
  - `markValue` / `collectCycle` 根遍历补 `VAL_COROUTINE`（args / currentValueBox / vmClosureBox）——挂起协程仅可达的循环容器原被误判不可达孤岛。
  - `loopUnroll` 回溯验证计数器初值为常量 0 + 排除体内嵌套控制流，拒绝非法展开。
  - `peekRef` 错误哨兵改 `thread_local` 并逐次重置——原函数级 static 可写哨兵跨 VM 实例共享，spawn 子线程构成写竞争。
  - `writeBackReceiver` 全局槽写回补 slot 范围校验。
  - `needsPopForExprStmt` 以节点类型枚举清单为准覆盖全部表达式语句（防再次遗漏栈泄漏）。
  - 生成器重放的调用栈收缩并入 RAII 守卫——原弹帧循环在 try/catch 之后，RuntimeError / ThrowException 逃逸时 `callStack_` 残留陈旧帧。
  - GC 触发回调新增 owner 令牌 + `clearGcTriggerCallbackIfOwner`——多 Interpreter 实例并存时先构造者析构不再误清存活实例的回调。
  - 模块路径规范化三处同步循环剥 `./` + 折叠 `/./`。
- **AUDIT-R2 系列（教学执行子系统）**：BugHuntPanel `QButtonGroup` id 显式分配、`verifying_` 守卫 RAII 化、变体索引双侧上界检查、递进提示 ≥3 条不变量扩展到全题库；BackendExecutionService 不再 reset 全局 GcManager 单例（保护并发 worker）、微秒精度耗时、三后端计时基线不含编译时间。
- **构建阻塞**：[interpreter/GcManager.cpp](interpreter/GcManager.cpp) `markValue` VAL_COROUTINE case C4457 变量遮蔽（循环变量 `v` 遮蔽参数 `const Value& v`）；stale unity obj 链接失败（LNK2019 `setGcTriggerCallback` 签名变更后未重编）；`LoopUnrollVregRenamingCorrectness` 测试适配 P1-9 计数器初值验证收紧。
- **spawn 异常传播三后端不一致（P3-A1）**：spawn join 延迟执行模式下，闭包内 throw 触发的 `throwException` 弹出闭包帧并穿透 `invokeClosureSync` 边界，破坏"调用前后 frames_/stack_/ip 不变"不变量——StackVM `invokeClosureSync` 末尾 `result = pop()` 误吞 thrownValue，RegisterVM `result = reg(dstReg)` 误读 savedReg0。修复为两后端在调用前后比较 `tryStack_.size()`，缩小时返回 `VM_EXCEPTION_THROW` 不操作 ip/栈/寄存器。
- **文档过时项清理（实证后更新）**：A1 匿名函数（faq.md Q5 / language-comparison.md 从"不支持"更正为 R98 实现现状）、A2 多行字符串（faq.md Q6 更正为"普通双引号字面量允许跨行"）、C1 V-P1-6（TestJIT R161Closure* 陈旧"暂时禁用"注释）、B1（testing.md 已知限制 #10 标记已修复）。

[v1.3.0]: https://github.com/MiniLang/ide/releases/tag/v1.3.0
[v1.2.0]: https://github.com/MiniLang/ide/releases/tag/v1.2.0
[v1.1.0]: https://github.com/MiniLang/ide/releases/tag/v1.1.0
