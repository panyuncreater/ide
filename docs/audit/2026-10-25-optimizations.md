# 2026-10-25 优化候选清偿轮（OPT-19~22 + IR 常量池缺陷）

> 范围：BACKLOG §2 OPT-19/20/21/22 全部清偿 + 本轮发现即修的 IR 常量池复制缺陷 ｜ 方法：逐项实现 + 交错 A/B 基准 + 归因实验 ｜ 基线：`fee553c`（同日首轮性能轮提交）+ 工作树
> 开放项：见 `docs/BACKLOG.md`（B-009/B-012 维持；§2 已清空）

## 已实施

### OPT-19：JIT OP_JUMP_IF_FALSE/OP_NOT 真值判定内联

- 实现：新增 `JITBackend::emitTruthyToRax`（`JITCodeGenHelpers.cpp`）——tag 三路比较（INT 0x7FF8 / BOOL 0x7FF9 → payload≠0；NULL 0x7FFA → 恒假），FLOAT/PTR（空串判定）回退 `jitTruthy` helper；`OP_NOT`/`OP_JUMP_IF_FALSE` 改调（peek 不弹出，rax 返回 0/1）。生成码变更，`kJitCodegenVersion` 5→6。
- 回归：`BacklogRegression.OPT19TruthyInline*` 四项（标量条件 / NOT 全类型 / helper 回退类型 / while 热路径），`EXPECT_FIVE_BACKENDS` 锁定与 `Value::isTruthy` 一致；TestJIT 全套 429/429。
- **性能归因（重要）**：实测 JIT fibonacci 13.65→4.11 ms（−69.9%）、large_loop 7.03→0.78（−88.8%）、tak 5.71→1.78（−68.8%）（同机最小值，7 轮交错，输出逐项核对一致）。但归因实验证明收益**不是省 helper 调用本身**：①JIF 单独回退 helper 仍 4.5 ms；②emitTruthyToRax 内部整体 helper 化仍 4.4 ms；③JIF 恢复原发射序列回 15.5 ms；④原序列前插 16B nop 不变。结论：收益来自**发射序列变化导致的机器码布局/微架构效应**（后续热路径 OP_CALL 递归直连的布局状态），数值不可作为稳定承诺，换编译器/机器可能漂移；内联本身的理论收益（每条件省一次调用）<10%。

### OPT-20：JIT lazy/特化编译 funcTable 代缓存

- 实现：`tieringFuncTable_` 成员缓存（`JIT.h`），`compileSingleChunkBlock` 同源复用（换源重建、execute 入口失效），消除每次 lazy/特化/OSR 编译全量重扫 functionChunks 的 O(F²)。
- 回归：`BacklogRegression.OPT20LazyFuncTableReuseMultipleChunks`（闭包/互递归多 chunk lazy 编译语义不变）。仅编译期，无机器码变化，`kJitCodegenVersion` 不递增。

### OPT-21：IR 路径语句位赋值跳过结果重载

- 实现：`visitStatement`（含 build/模块/块/for init/update/match case body 全部语句上下文，统一经 `visitStatement`）对 NODE_ASSIGNMENT 置抑制标志；`visitAssignment` 对 GLOBAL/MEMBER 路径 STORE 消费值后跳过重载，配对 POP 一并跳过。表达式位（嵌套赋值）不受影响。七个重复 POP 站点收敛到单一实现。
- 回归：`OPT21StmtAssignmentValueSemantics`、`OPT21StmtAssignInBlocksAndLoops`（五后端）、`OPT21NullReceiverMemberSetStillErrors`（MEMBER_SET 自身报错不吞）。RegisterVM 实测 large_loop −12.4%、string_concat −7.6%（同机最小值 5 轮交错）。

### OPT-22：LSP 符号索引文档级缓存

- 实现：`SymbolIndex` 前移并入 `DocumentState::symbolIndex`，`reparse` 构建一次；hover/definition/references/completion/documentSymbol/signatureHelp/semanticTokens/rename 八请求点全部复用（原每请求全 AST 重建）。
- 回归：`Opt22SymbolIndexCacheTest` 三项（open/change 重建、解析失败空索引、多请求共享索引）；LSP 全套 45/45。

## 已修复（本轮发现）

- **IR 常量池复制 -0.0/0.0 位模式失配（P2）**：IR 侧 `scalarKey` 按位模式区分 −0.0/0.0，StackVM IR lowering 曾经 `chunk_->addConstant` 去重复制（IEEE equals 判等把两者合并），池长度失配致其后所有 LOAD_CONST 索引越界（运行时"常量池索引越界"）。触发条件：`-0.0` 常量之后引用新常量。修复：原序 push_back 复制（与 RegisterBytecodeBackend 对齐）。回归 `BacklogRegression.IRConstPoolCopyKeepsNegativeZeroDistinct`（五后端）。本缺陷在 OPT-19 差分测试（`!-0.0`）中暴露。

## 方法学教训（暂不升级 spec，复发再议）

- **JIT 基准对机器码布局极端敏感**：同义发射序列变化可致数倍漂移（fib 3.3×），单批次先后对比会产生假数据（本轮首批测量即被误导）；必须同轮交错 + 输出核对 + 最小值统计。已记入 `docs/BACKLOG.md` §2 尾注。
- 测量期间系统节流/后台扫描可致全后端 +80% 假回退，长套件单轮数据不可信。

## 验证

- 五目标构建 `/W4+/WX` 零警告；全量 ctest 4257/4257（新增 12 个回归）；`knowledge_lint.py` 0 error / 0 warning。
- 基准为 debug 构建同机最小值，基线（`fee553c` 重建）与工作树二进制同轮交错执行；JIT 关键项 7 轮 / RegisterVM 5 轮，输出逐项一致。
