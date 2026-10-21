# MiniLang IDE 代码审计报告（2026-10-21）

> 审计轮次：2026-10-21 | 方法：4 个并行只读审计子代理（协程真挂起 / JIT 块缓存 / 泛型擦除六点 / 字典透明查找与 VM 热路径）+ 主审计员逐项复核与探针实测
> 基线：v1.4.0（e937640）+ 在途批次（协程 Linux ucontext / 泛型擦除补全 + 显式类型实参，现已入库 c132756、a2aeefd）
> 本轮修复：5 项（P0×1、P1×3、P2×1），提交 d4420d8；验证 = Windows debug 全量 ctest **4156/4156 通过**（/W4+/WX 零警告，含 MINILANG_BUILD_PERF_TESTS=ON 全口径）
> 输出格式：按 AGENTS §5（位置 / 严重性 / 类型 / 复现场景 / 根因分析 / 修复建议）；**修复建议仅针对未修复项**（已修复项见 §2）

## 1. 审计范围

| 审计域 | 范围 | 结论概要 |
|---|---|---|
| 协程真挂起 | InterpreterCoroutine.cpp（Windows fiber + Linux ucontext）、CoroutineFiber.h、Interpreter.cpp callCoroutineNext/根集收集、GcManager 协程 mark 分支 | P0×1、P1×1、P2×3；VM 侧快照/重定基本轮未覆盖（见 §5） |
| JIT per-chunk 块缓存 | compiler/jit/（BlockCache、tiering 门控、IC、JitContext/JitFrame 协议）+ jitBuildEnumVariant 载荷校验 | P1×1、P2×1、P3×1、记录×2；键覆盖/导入槽重绑定/驱动逐出无现时可达缺陷 |
| 泛型擦除六点 + 显式类型实参 | TypeChecker.h、Interpreter checkType/visitEnumVariantExpr、Compiler/AstIRBuilder emitTypeCheck、StackVM/RegisterVM enum 载荷校验、Parser trySkipExplicitTypeArgs | 六点接入确认无遗漏；P1×1（构造路径时序，已修）、P2×3、P2 清理项×1；JIT 第七点移交后确认（已修） |
| 字典透明查找 + VM 热路径 | Value.h 透明查找基础设施及 19 处接入点、validateChunkInstructionBoundaries、GcManager slots_、RefCounted 内存序 | **无 P0/P1**；P2×2、P3×2、注记×1 |

复核声明：所有 P0/P1 发现已由主审计员读源码独立核实（非转述子代理结论）；语义行为类发现经 CLI/测试二进制探针实测（见 §3.1、§3.2）。

## 2. 本轮已修复（提交 d4420d8，测试 +5，全量 4156/4156 绿）

### 2.1 P0｜挂起协程快照不在任何 GC 根集（三执行后端）｜数据损坏

- **位置**：`interpreter/Interpreter.cpp` triggerIncrementalGc（约 1251–1319 行，根集枚举 0–5 项无协程快照）；`interpreter/Value.h` gcRootPtr（对 VAL_COROUTINE 返回 nullptr）；`interpreter/InterpreterCoroutine.cpp` CoroutineFiberState；`GcManager.cpp` mark 两个 VAL_COROUTINE 分支
- **类型**：GC 误回收（数据损坏）
- **复现场景**：`fun* gen() { var a = [10,20,30]; yield a[0]; yield a[1]; } var g = gen(); g.next();`（挂起，a 仅被生成器局部环境持有）→ 主脚本继续分配跨过 GC 阈值 8192（语句边界增量 GC）→ `g.next()` 时 a 的元素已被 sweep 清空，索引越界
- **根因**：挂起期间帧已移出 callStack_、环境链仅存于 `CoroutineFiberState::suspendedEnv/funEnv/suspendedCallStack`（shared_ptr 只保对象存活、不构成 mark-sweep 根）；`gcRootPtr()` 对协程返回 nullptr 使环境变量直接持有的协程整体不进根集；`CoroutineFiberState` 注释声称"env/帧持 GC 根"但从未接线——v1.4.0 文档与实现断裂。VM 侧同理（挂起窗口内 VM 栈已清空，快照 stackSuffix/寄存器窗口不可见；VM 执行期虽有 CallbackSuppressor 抑制增量 GC，但挂起窗口不受保护）
- **修复**：① `gcRootPtr()` 补协程分支（对齐 BUG-REPL-AUDIT-7 闭包先例：CoroutineData 不登记 slots_ 不会被 sweep，作为根让 mark 进入协程分支）；② `CoroutineData::gcRootContributor` 类型擦除回调——Interpreter 贡献快照环境链（collectCoroutineFiberGcRoots）、StackVM 贡献 stackSuffix + 帧内已关闭 upvalue、RegisterVM 贡献寄存器窗口 + 已关闭 upvalue；③ GcManager mark 两个 VAL_COROUTINE 分支接入（根集循环 switch 抽取为 `markTrackedRoot` 共用，消除双份 switch 漂移风险）；④ 丢弃快照处同步置空回调（防悬垂）
- **回归**：`CoroutineSuspension.SuspendedGeneratorLocalsSurviveIncrementalGc`（3 万次分配确保多次跨阈值）

### 2.2 P1｜泛型构造路径参数/默认值校验先于类型参数合并 guard｜四后端不一致

- **位置**：`interpreter/InterpreterCalls.cpp` setupInitEnvironment（checkType 于参数/默认值绑定循环）+ runInitMethodBody（mergedTypeParams + CallFrameGuard 在其后才建立）；同型 `interpreter/Interpreter.cpp` visitVarDecl 自动构造路径（默认值 checkType 先于 guard）
- **类型**：语义不一致（违反 ADR-002）
- **复现场景**：`class Box<T> { init(v: T[]) { } } var b = Box([1, 2]);` → 修复前 Interpreter 报"类型注解违反: 期望类型 T[]"，StackVM/RegisterVM/JIT 通过（VM 无参数校验发射点）；且与 init 体内同注解被擦除自相矛盾
- **根因**：checkType 的擦除依赖 `currentTypeParams_`，构造路径的参数校验发生在合并 guard 建立之前（此刻为调用方上下文，顶层为空）
- **修复**：合并 guard 前移至两处参数循环之前（CallFrameGuard{manageCtx=false} 仅管理类型参数语境）；对齐 invokeMethod/callNamedFunction 的"先合并后校验"顺序
- **回归**：`GenericErasureCompletion.GenericClassInitParamCompositeAnnotation`（四后端）、`GenericClassInitDefaultTParamAnnotation`（标量 T 默认值，经探针确认语义后改形）

### 2.3 P1｜JIT `jitBuildEnumVariant` 载荷校验未升级递归擦除（第七处擦除点）｜四后端不一致

- **位置**：`compiler/jit/JITRuntime.cpp:1013`（jitBuildEnumVariant 内）
- **类型**：语义不一致
- **复现场景**：`enum Row<T> { Cell(T[]) } var r = Row.Cell([1, 2]); print(r);` → 修复前 JIT 报"类型不匹配"中止，Interpreter/StackVM/RegisterVM（含 IR 路径）构造成功。触发条件 = 泛型 enum 变体 + 复合注解载荷 + 非空数组（空数组 vacuously 通过；裸 T 不触发）
- **根因**：OP_BUILD_ENUM_VARIANT 是 JIT **原生 helper 路径**（JITCodeGen.cpp:1214 → emitBuildEnumVariant，无回退 VM 通道），R163 六点修复清单不含 JIT；`expectAllGenericBackends` 不含 JIT 通道，回归网未覆盖
- **修复**：升级为 `annotationReferencesTypeParam`（与六处同判定）；补 JIT 通道回归 `EnumPayloadCompositeTAnnotationJIT`
- **纪律**：泛型注解擦除点现为**七处**（原六处 + JIT）——AGENTS §3 #11 已按七处口径维护

### 2.4 P1｜失败协程弃用重建导致 yield 计数错位｜语义分歧（fiber vs 重放）

- **位置**：`interpreter/InterpreterCoroutine.cpp` callCoroutineNext failed 分支 + coroutineSuspend（currentYieldId 无条件自增）
- **类型**：语义不一致 / 错误传播
- **复现场景**（探针实测）：`fun boom() { throw "boom"; } fun* g() { yield 1; boom(); yield 2; }` 依次 `.next()`：修复前 fiber 路径第三次 next() 从头重跑函数体 → 返回首个 yield 值且提前置 done；重放路径同点再抛
- **根因**：错误逃逸后 reset 重建 + coroutineSuspend 无条件递增，与重放"跳过已消费 yield 后同点再抛"分歧
- **修复**：失败 fiber 保留，`fs->failed` **前置检查**（绝不 switch 进已终结 fiber 栈——原实现仅在 switch 返回后检查，保留 fiber 后前置检查缺失即重入死栈 UB，探针实测 0xC0000005 崩溃后修正）+ 返回点双检查；后续 .next() 持续重抛同一错误（值/错误序列与重放逐点一致，前缀副作用只执行一次——ADR-007 既定差异口径）
- **回归**：`CoroutineSuspension.FailedGeneratorRethrowsSameErrorOnEveryNext`（解释器三段序列）+ `FailedGeneratorErrorEscapePrefix`（四后端一致前缀）
- **测试设计注记**：throw 必须位于非最终 yield 之后（经辅助函数逃逸）——生成器最终 yield 之后的尾部语句不执行是既有设计，直接写在末尾是死代码（探针 coverage 实测行 MISS）

### 2.5 P2｜JIT 块缓存 storeBlocksToCache 未翻转实例侧 owned 标志｜所有权协议违反

- **位置**：`compiler/jit/JIT.cpp` storeBlocksToCache（约 810–852 行）
- **类型**：UAF 温床（现时被 asmjit 容错掩盖）
- **复现场景**：eager 可缓存编译 → 块入缓存 → 同实例再次 execute() 入口/析构 → 实例 `runtime_.release` 释放共享 runtime 的块——跨 JitAllocator 释放（asmjit 按查块失败 no-op；runtime 合一即双重释放/UAF）
- **根因**：注释与命中路径不变量均声明"缓存共享块实例不释放（owned=false）"，但 miss→store 路径漏翻转
- **修复**：store 逐块拷贝后置 `blk.owned = false`

## 3. 未修复发现（按严重性排序；均为本轮新登记，修复建议含在条目内）

### 3.1 P1｜VM 错误逃逸后 `.next()` 语义与解释器分歧（四后端不一致，预存）

- **位置**：`compiler/backend-stack/VMCalls.cpp` callCoroutineNext 异常穿透路径（约 2181–2189 行"不置 done"）与其后的新生成器调用分支；RegisterVMCalls.cpp 对应点
- **类型**：语义不一致 / 错误传播
- **复现场景**（探针实测，同脚本四后端差分）：`fun boom() { throw "b"; } fun* g() { yield 1; boom(); yield 2; }` 错误穿透后：Interpreter（fiber 修复后）第三次 `.next()` **同点重抛**（done 保持 false）；StackVM 第三次 `.next()` **返回末 yield 值 1 且置 done=true**（实测序列 `1E1false1true` vs 解释器 `1E1false<runtime:未捕获的异常: b>`）
- **根因**：VM 错误穿透路径本身不置 done，但异常后 vmSuspension 已消费、下次 .next() 走全新调用重放——实测未按预期再次抛出而是返回末值置 done，推断异常后 `currentCoroutineTargetYieldId_/currentYieldExecutionCount_` 状态使重放门控短路（确切机制需 VM 侧协程审计补齐，见 §5）
- **修复建议**：VM 侧协程审计（本轮未覆盖）确认后统一口径——推荐对齐解释器"重抛直至耗尽"（改 VM 新生成分支的错误重放门控），或全后端统一"错误即终结"（需同步改解释器 fiber + 重放 + ADR-007 语义表）；属语义变更，需 ADR 立项
- **置信**：行为差异为探针实测（确定）；根因定位为中

### 3.2 P2｜数组字面量默认参数值静默不绑定（预存）

- **位置**：`parser/Parser.cpp` parseParamList 默认值处理（BUG-DEF-1 isLiteralDefaultExpr 白名单）+ 三后端默认值绑定
- **类型**：错误传播 / 三后端一致性边界
- **复现场景**（探针实测）：`fun f(a: int[] = [1, 2]) { return a; } print(f());` → **运行时错误"未定义的变量: a"**（默认值未被绑定）；类 init 同型（`init(v: int[] = [1, 2])` → 字段 null，无任何诊断）
- **根因**：BUG-DEF-1 策略为"默认值仅字面量，Parser 层拒绝复杂表达式"，但数组字面量未触发 ParseError 而是静默通过解析、绑定阶段丢弃（白名单与解析行为不一致）；CLI 侧多行形式另报 2 个 parse 错误，单行与多行行为不一致
- **修复建议**：parseParamList 对 ArrayLiteral/DictLiteral 默认值显式 ParseError（策略内自洽），或扩展 BUG-DEF-1 支持数组/字典字面量默认值（VM 侧 0xFFFF 哨兵机制已有先例——InterpolatedString）；同时核查 CLI 与测试管线解析行为不一致
- **置信**：行为实测确定；机制定位中

### 3.3 P2｜泛型嵌套函数/lambda/局部 enum 不继承外层类型参数（预存，四后端一致拒绝合法程序）

- **位置**：`compiler/core/CompilerStmt.cpp:608`（currentTypeParams_ 覆盖式赋值）、`compiler/ir/AstIRBuilder.cpp:2086`、`interpreter/InterpreterCalls.cpp` CallFrameGuard 三处（245/459/1035）
- **类型**：边界条件（非一致性缺陷——四后端同错）
- **复现场景**：`fun f<T>(x: T) { fun inner(y) { var z: T[] = [y]; return z; } return inner(x); } print(f(1));` → 四后端一致报"类型注解 T[] 不匹配"
- **根因**：类型参数上下文按函数声明覆盖而非词法作用域传播；类方法路径已正确合并（CompilerClass.cpp:162-164 等）
- **修复建议**：进入子函数时 `currentTypeParams_ = 外层 ∪ node.typeParams`（编译期与运行时 CallFrameGuard 同步）；局部 enum 引用外层 T 需并入 EnumInfo.typeParams。属能力缺口而非回归，建议立项
- **置信**：高（三处赋值点直接代码证据；未运行验证）

### 3.4 P2｜trySkipExplicitTypeArgs 白名单缺 `:`/`(`/`)`（预存）

- **位置**：`parser/Parser.cpp:424-470`（白名单分支 447-451 行）
- **类型**：边界条件
- **复现场景**：`fun f<T>(x: T) { return x; } print(f<dict[string:int]>({ "a": 1 }));` → 复合类型实参回退为比较解析，报误导性错误"未定义的变量: dict"
- **根因**：探测白名单未覆盖 parseTypeAnnotation 可生成的 dict[..]/fun(..) 形态所需的 `:`/`(` token
- **修复建议**：白名单加入 TK_COLON/TK_LPAREN/TK_RPAREN 并配对平衡计数（裸加会误吃 `x < (a+b) > (c)`），或直接复用 parseTypeAnnotation 的平衡消耗；回退零残留机制本身已验证无问题
- **置信**：高（逐 token 核对）

### 3.5 P2｜enum 变体构造携带显式类型实参时 AST 降级（预存）

- **位置**：`parser/Parser.cpp` call()（约 2418–2447 行 R99 分支）
- **类型**：边界条件
- **复现场景**：`enum Box<T> { some(T) } print(Box.some<int>(5));` → 全后端运行时错误（MemberAccess+FunCall 降级 AST）；`Box.some(5)` 正常
- **根因**：enum 变体分支要求 VariantName 后紧跟 `(`，`<int>(` 探测在后续循环才命中，AST 已降级
- **修复建议**：enum 变体分支在 match(TK_LPAREN) 前先尝试 trySkipExplicitTypeArgs()
- **置信**：中高（解析路径直接证据；运行时报错文案未实测）

### 3.6 P2｜dict 键类型含 `:` 的注解形态擦除漏判 + 必然拒绝（预存，低触达）

- **位置**：`common/TypeChecker.h:201-208`（annotationReferencesTypeParam dict 分支取首个 `:`）与 87-106（typeMatchValue 同法）
- **类型**：边界条件
- **复现场景**：`dict[fun():K:string]`/`dict[dict[string:K]:int]` 形态——键类型被首个冒号截断，擦除漏判 + typeMatchValue 必拒
- **修复建议**：与 parseTypeAnnotation 同构的括号感知切分（fun `(` 深度、dict `[` 深度），键/值两侧共用
- **置信**：高（两处切分逐行核对一致地错；触达面极窄——闭包/dict 作键本超 DictKey 运行时表示）

### 3.7 P2｜协程 TcoScopeGuard 快照滞留 fiber 栈（置信：中低，待证）

- **位置**：`interpreter/InterpreterCoroutine.cpp` coroutineFiberBody（TcoScopeGuard 局部构造）
- **类型**：状态泄漏
- **根因（待证）**：守卫滞留 fiber C++ 栈至生成器终结；期间 Interpreter TCO 上下文停留于生成器设置的 disabled 态，若蹦床循环执行中调用 .next() 恢复方剩余迭代读到被篡改上下文；coroutineSuspend/callCoroutineNext 未保存/恢复 TCO 上下文
- **修复建议**：核对 TcoScopeGuard 成员与蹦床消费点后，将 TCO 上下文纳入 coroutineSuspend 快照集合（与 suspendedReturnType 同模式）
- **置信**：中低（未逐行核对消费点）

### 3.8 P2｜协程跨线程恢复缺防御（置信：中）

- **位置**：`interpreter/InterpreterCoroutine.cpp` Linux switchToFiber（t_currentFiber 为 null → swapcontext 空指针解引用）+ Windows SwitchToFiber 非 fiber 线程
- **类型**：崩溃边界（前置条件：同一 Interpreter 被两线程驱动——常规 GUI 架构单 worker 不可达）
- **修复建议**：.next() 入口校验 fs->resumer 线程一致性（thread_local 登记），不一致显式报错"协程不可跨线程恢复"
- **置信**：机制确定；可达性低

### 3.9 P3｜JIT 块缓存同键重复 store 不清旧 LRU 节点

- **位置**：`compiler/jit/JIT.cpp:849-850`
- **类型**：资源泄漏 + LRU 结构破坏（前置：元数据损坏回退或跨线程双 store——源码自标"不应发生"）
- **修复建议**：store 前按 key 查旧条目并先移除 LRU 节点
- **置信**：逻辑确定/可达性低

### 3.10 P3｜RegisterBytecode instructionSizeAt(REG_DEFINE_CLASS) 多段截断回退 baseSize=7 可漏报

- **位置**：`compiler/backend-reg/RegisterBytecode.cpp` instructionSizeAt
- **类型**：边界条件（仅畸形字节码可达；新旧共用同函数、等价性不破坏）
- **修复建议**：多段截断场景逐段校验或保守拒绝加载

### 3.11 P3｜NaN 键可写不可查

- **位置**：`interpreter/Value.h` dictKeyFromValue/DictKeyEqual
- **类型**：语义边界（三后端一致；与 JS/Python 不同；每次 set 新增条目、get/has 永 miss）
- **修复建议**：dictKeyIsValid 前置拦截 NaN 键显式报错，或文档化

### 3.12 记录项（无行为缺陷）

- JIT computeBlockCacheKey 的 enumInfos 仅 name 入键（variants/paramTypes/typeParams 不入）——未发现可达的陈旧机器码路径（同字节码 ⇒ 同 enum 声明）；建议随下次 key 结构变更补全
- JIT LRU 驱逐立即释放共享块，被驱逐后存活命中实例持悬垂 entry——安全仅靠单线程假设 + execute 入口清空（JIT.h 已显式豁免）；"共享存活"仅覆盖数据侧
- `Interpreter::isTypeParameter` 成员函数已无调用方（与 common/TypeChecker.h 自由函数构成双事实源）——建议删除或改转调，防复活精确匹配漏擦
- `interpreter/Value.h:302` 注释"rehash 后引用失效"与标准不符（rehash 只失效迭代器）——过度保守，可修订
- StackVM/RegisterVM 约 10 处 member-set/get 与 remove 非 string 分支未透明化——仅多余深拷贝，行为无差异
- PkgCliCommandTest.InstallCommandAllFromManifest 在全量 `-j 10` 负载下出现过一次失败（exitCode 2），单跑与 PkgCli 38 用例并行 4 轮均通过——环境性 flake，建议后续给该夹具增加重试或定位负载敏感点

## 4. 审计覆盖声明

- **VM 侧协程快照/重定基未审**：compiler/backend-stack/VMCalls.cpp OP_YIELD 快照、RegisterVMCalls.cpp REG_YIELD 的重定基算术、try 栈恢复、upvalue 槽位重捕获——子代理时间边界内未完成，**不构成"无问题"结论**。本轮修复已触及两处挂起创建点（根集贡献回调）与四处快照丢弃点（置空回调），该部分经全量回归验证；重定基算术本身仍建议下轮按 backend-consistency §9 checklist 专项审计，并优先验证 §3.1 的 VM 门控短路机制
- 字典透明查找/VM 热路径/GC slots_/RefCounted 内存序：本轮无 P0/P1（详见 §3.10-3.12 记录项）
- JIT tiering/OSR/IC 内部实现、ARM64 后端、汇编编码：本轮明确排除

## 5. 验证与统计

- 本轮修复 5 项，新增回归 6 项（5 新测试 + 1 拆分），全量 ctest **4156/4156 通过**（-j 10，--timeout 600，MINILANG_BUILD_PERF_TESTS=ON 全口径），/W4+/WX 零警告
- 构建注记：`minilang_perf_test` 目标受 `MINILANG_BUILD_PERF_TESTS` 门控且 option 定义于 `add_subdirectory(tests)` 之后——全新 configure 必须显式 `-DMINILANG_BUILD_PERF_TESTS=ON`（已记入 AGENTS §6）
- 审计方法论注记：探针实测纠正了两处子代理"确定级"结论的复现脚本（最终 yield 后语句为死代码；数组默认值触发 BUG-DEF-1 既有约束）——**复现场景必须实跑验证**，逻辑推演级复现需标注置信度

---

# 2026-10-22 轮次（第二日，VM 侧专项 + 协作事件记录）

## 6. 本轮执行内容（按上轮推荐：① VM 侧协程快照/重定基专项审计 ② §3.1 语义对齐）

### 6.1 已修复（本轮）

- **P1｜§3.1 清偿｜VM 错误逃逸后 `.next()` 语义对齐解释器**：机制定位（并行会话与主审独立收敛）= 错误穿透后 `vmSuspension` 已消费、`done=false`、cd 无失败记录，下次 `.next()` 全新调用在 fiber 模式每遇必挂 → 重挂首 yield → `currentYieldId` 伪递增至 yieldCount → 提前置 done（赋值点 = fiber 最终-yield 分支）并返回首 yield 值。修复 = `CoroutineData::vmPendingErrorBox` 失败保留盒 + 入口前置 throwException 同点重抛；逃逸判别用**帧深度**而非 tryStack 计数（后者在内部 try 跨挂起恢复场景误触发——TryCatchAcrossSuspension 实测回归后改判，两次实测教训已记入 §9 条款）。`=0` 重放平台静态核对本已与解释器重放一致（零改动）。回归升级四路径锁定（`1boomboomfalse`）。
- **P1｜F1（上轮 3.1 待证项 W2 升级清偿）｜TCO 上下文跨挂起边界滞留**：子代理核实报告证实原文四项断言全部成立，并给出主攻击路径（终结上下文尾调用被陈旧 tcoName_ 误分类为 SelfFunction → 函数体携错误实参重跑，探针期望 56 缺陷 57）。修复 = TCO 五成员纳入挂起/恢复协议（coroutineSuspend 对称保存/恢复 + CoroutineContextGuard 退出恢复调用方真值）；探针实测修复后 56。回归 `TcoContextRestoredAcrossGeneratorTermination`。
- **P1｜F1（上轮 3.1 待证项）｜挂起窗口开放 upvalue 相对索引暴露**（StackVM 审计新发现，主审复核采纳）：yield 出捕获生成器局部的闭包并在窗口期调用——open 态相对 stackSlot 指向已搬空的活栈区域，静默读错值/向调用方活栈写入（写路径可升级 P0）。修复 = 快照式关闭（挂起时 `uv->value`=槽值 + isClosed，恢复时传播写回再重挂 open，与语言"关闭=快照"语义自洽，无写入时幂等）。两 VM 同型。回归 `SuspendedWindowClosureUpvalueSemantics`（`1111`）。
- **P1｜跨调用方深度恢复循环边界回拨**（并行会话独立发现并修复，提交 251a54c）：恢复路径重定基后把循环边界回拨到首次挂起入口，两次 `.next()` 调用方帧深不同即越界执行调用方代码/短路消耗快照；RegisterVM 被 IR 内联掩盖（关内联实测同型）。主审已审阅 diff：修复正确（保持本次入口深度、快照移除绝对基线），回归 `ResumeAtDeeper/ShallowerCallSite`。
- **P3｜D3 清偿**（并行会话提交 46f3a6d）：JIT 块缓存同键重复 store 先摘除旧 LRU 条目。主审已审阅 diff：正确。

### 6.2 报告勘误（§3.2 数组字面量默认值）

原记录"解析成功但默认值静默不绑定，无诊断"**不准确**。追加探针实测（2026-10-22）：① 顶层 `fun f(a: int[] = [1, 2])` —— **解析脱轨**（函数体错误归属，运行时报"return 只能在函数体内使用"，无 ParseError）；② 类 init 同型——解析通过但默认值静默不绑定（字段 null）；③ 标量默认值（int/T）全路径正常；④ 标量子代理报告的"当前代码会 ParseError"结论与实测不符（其 CLI 探针与测试二进制行为不一致，原因未定位）。真实机制 = 参数默认值表达式的数组字面量解析/求值链路在函数声明与类方法两条路径上行为分叉且均不正确——属解析器参数默认值重构范畴，维持 **P2 登记、本轮不修**（需专项轮次），BUG-DEF-1 策略与实现的偏差一并纳入该轮。

### 6.3 协作事件（如实记录）

本轮执行期间发现**另一并行会话在同一工作区提交了三个修复**（7dd9557 = 本轮 §3.1 对齐的主审实现被代为入库；251a54c = 重定基回拨修复；46f3a6d = JIT D3），作者身份相同（zwork@local），内容均源自本报告的推荐项且质量经主审逐项审阅采纳（含 54 行回归测试、探针实证、对主审探针的识别避让）。期间发生一次并发构建锁冲突（LNK1168，重试解决）。**风险与建议**：双会话并行变更同一仓库存在编辑互踩与验证口径漂移风险——建议后续将仓库变更操作收敛到单一会话，或建立明确的任务分工边界；本报告与 CHANGELOG 已合并双方条目。

### 6.4 剩余未修（更新后）

- P2：参数默认值数组字面量解析脱轨/静默不绑定（§6.2，需解析器专项轮）
- P2：泛型嵌套函数/lambda/局部 enum 不继承外层类型参数（需设计）
- P2：trySkipExplicitTypeArgs 白名单缺 `:`/`(`（需配对平衡设计）
- P2：enum 变体构造带类型实参 AST 降级；P2：dict 键类型含 `:` 长尾；P2：协程跨线程恢复缺防御
- P3：NaN 键可写不可查；RegisterVM tryStack 跨挂起计数不可靠（本轮改用帧深度判别后仍建议下轮核对账目）
- RegisterVM 协程 §9 checklist 专项审计仍建议补齐（本轮由并行会话覆盖了重定基与回拨，IC/tiering 外的其余项未逐条核）

## 6. 2026-10-22 轮次（夜间调度任务追加：VM 侧协程快照/重定基专项审计 + 推荐项清偿）

> 本轮即上节 §4 遗留的"VM 侧协程快照/重定基专项审计"，按 backend-consistency §9 checklist 逐项核对（单边启用禁令/快照字段相对化/returnIp 重定基/嵌套挂起上下文/GC 根义务/OP_RETURN try 处理器随帧回收——均确认无恙），**新发现并修复一个 P1**。同夜另有并行会话按同一推荐清单作业（其 §3.7 TCO 五成员挂起协议在途），双方提交边界经 git 逐文件独立核验、未互串。

### 6.1 已修复（本轮 3 项）

- **P1｜§3.1 VM 错误逃逸后 `.next()` 语义对齐解释器（提交 7dd9557）**：`CoroutineData` 新增 `vmPendingErrorBox`——StackVM/RegisterVM 生成器体内未捕获异常穿透到脚本层处理器（帧深度判别）时按值捕获，入口前置检查持续同点重抛（done 保持 false、前缀副作用不重执行），对齐解释器 fiber 失败保留语义；`=0` 重放平台不保留（重执行+重抛一致）。`FailedGeneratorRethrowsSameErrorOnEveryNext` 升级四执行路径锁定（`1boomboomfalse`）。GcManager 协程分支补 `vmPendingErrorBox` 根集。修复过程中捕获回归由探针实测定位：RegisterVM 恢复后捕获基准计入生成器自身挂起 try 帧，生成器内 catch 弹出自己 try 帧被误判逃逸（TryCatchInsideGenerator / TryCatchAcrossSuspension 回归），两 VM 判别均已改为帧深度基准（含并行会话在 7dd9557 收录的判别器修正）
- **P1｜VM 协程跨调用方深度恢复循环边界回拨（§9 专项审计新发现，提交 251a54c）**：真挂起恢复路径在完成相对→绝对重定基后，旧实现把循环边界/挂起基线回拨到**首次挂起时**的调用方入口（`savedFrameCount = sus->baseFrames` / `savedStackSize = sus->baseStack`）——两次 `.next()` 调用方帧深不同即破坏：深恢复在生成器帧弹出后循环越界继续执行调用方指令（探针实测 yield 序列 `133true` 而非 `123true`）；浅恢复循环短路、快照被消费而帧未清理，后续 `.next()` 全新重建报"yield 只能在生成器函数体内出现"；重挂起窗口 `coroSuspendBase*` 同被回拨，切片把调用方栈槽/帧误并入快照。修复：删除两处回拨（StackVM/RegisterVM），循环边界、VMYieldSignal 清理、needCleanup 栈收缩、`coroSuspendBase*` 登记一律保持**本次**入口深度；快照结构移除只写字段 `baseStack`/`baseFrames`，快照内不携带任何绝对基线。**关键验证注记**：RegisterVM 默认构建经 IR inlinePass 掩盖（小函数内联后无跨深度路径），`setIRSSAOptimize(false)` 关内联实测与 StackVM 同型错值——修复对两 VM 同步生效。回归：`CoroutineSuspension.ResumeAtDeeperCallSite` / `ResumeAtShallowerCallSite`（跨深度场景此前无任何测试覆盖）
- **P3｜§3.9 JIT 块缓存同键重复 store（提交 46f3a6d）**：store 前按 key 查旧条目，先逐块 `sharedCacheRuntime().release` 再摘除 LRU 节点与 map 条目——旧实现的同键双节点使命中遍历/淘汰计数失真且旧共享块永不释放（防御性清偿，前置源码自标不应发生）

### 6.2 §3.2 勘误（无需代码修复）

- 四后端探针复测：`fun f(a: int[] = [1, 2]) ...` / 无注解变体 / 类 init 变体 / dict 字面量默认值——当前 HEAD 全部被 BUG-DEF-1 白名单**显式 ParseError 拒绝**（"默认参数值必须是字面量…"），产品主路径（GUI/CLI 展示诊断、拒绝运行）**不存在"静默不绑定"**。原报告"运行时错误『未定义的变量: a』"实为探针 harness 缺陷：宽松 harness 忽略 parse 诊断继续执行错误恢复后的残缺 AST（fun 声明保留但默认值未绑定）——与 §5 方法论注记"复现场景必须实跑验证"同源，本轮实测还原了该误判链路
- 遗留小项（登记不动）：① 白名单拒绝后 synchronize 残留 `}` 再报一条"多余的 '}'} 在顶层"（双重诊断噪音，GUI 诊断面板可见）；② 宽松 harness 下残缺 AST 各后端恢复形态不一致（Interpreter/RegisterVM 跑出运行时错、StackVM 报编译错）——两者仅在忽略诊断的 harness 可达，修复收益低、synchronize 全局共用改动风险高。"扩展 BUG-DEF-1 支持数组/字典字面量默认值"属能力增强（非缺陷），如立项另行走特性流程

### 6.3 验证与统计（本轮）

- 三次提交（7dd9557 / 251a54c / 46f3a6d）各自独立通过全量 ctest（-j 10，MINILANG_BUILD_PERF_TESTS=ON 全口径，/W4+/WX 零警告）；末次口径 4159/4159（含本轮新增 2 项回归；并行会话故意失败的临时探针 ProbeTmp2 已由其自行撤除）
- 测试计数：4156 → 4159（+2 协程跨深度回归 +1 并行会话轮次新增）
- 本轮未触及：§3.7（并行会话在途实施）、§3.3-§3.6/§3.8/§3.10-§3.12（按约束清单第 6 条维持登记）
