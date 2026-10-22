# perf-audit-backends — run aedb3fcf participant 1

**笔记文件未能写成**：`write audit-notes-20261022/perf-compiler-backends.md` 重试仍被 `tool_participant_budget_exhausted` 拒绝（Run 工具预算耗尽），故无法 `deliver_file`；按纪律第 7 条，将发现直接写入回复（详细六要素版未能落盘；所有收益均为「待证（未实测）」）。

**执行期 7 条（A1–A7）**
- **A1 高**：`VMCalls.cpp:executeReturn`（~L72–230）方法返回字段写回逐字段哈希赋值；RegisterVM 已有 directReplace 整体赋值未同步，建议对接收者整体赋值（需 COW 别名论证+差分回归）。
- **A2 中高**：`executeInstanceMethodCall`（~L1030–1105）+RegisterVM 对称处，帧构造做 fieldCount 次字符串 find；轻量改单次遍历 fields()+fieldSlotIndex O(1) 查槽，根治改 fieldOrder 对齐 vector（高代价）。
- **A3 中**：`RegisterVMExec.cpp:executeVars` 名称版全局变量无 varCache（StackVM 已有），每次全量哈希 find；建议 RegBytecodeChunk 移植平坦数组+版本失效（遗漏失效点即悬垂指针）。
- **A4 中**：`setupFunctionCallFrame`（~L905–925）每调用拷贝 functionName（常超 SSO）+逐 shared_ptr 拷 upvalues；改 const string* / shared 向量，零行为变化。
- **A5 中**：`currentFrame`（VM.cpp ~L759–771）每指令 2–3 次冗余获取且含 throw 冷路径；fast path 移 VM.h 内联，RegisterVM 对称。
- **A6 低中**：OP_ADD 通用路径 tryOperatorOverload+numericOp 双重调用（VM.cpp ~L2043/L934）；前置 int-int 快速路径。
- **A7 低**：extraSlots 逐个 push null；加 VMStack::pushNulls(n)。

**编译期 5 条（B1–B6）**
- **B1 中**：`IROptPasses.cpp` CSE（~L696–797）以 string 键值编号，每条纯指令重建 to_string 串，3 轮×；改结构化键 `{op,a,b}` 零分配；GVN 同型。
- **B2 中**：`optimizeIR`（~L1099–1116）固定至多 3 轮全 pass 重扫，未见零修改即 break（待证）；全 false 即 break，vregDefs 跨轮复用。
- **B3 中**：`Bytecode.h` L222–223 行/列元数据每字节 8B，编译期拷贝与缓存序列化翻倍、执行期仅错误路径使用；轻改 4B/字节或 per-instruction 行表（行号映射需全面回归）。
- **B4 低中**：`BytecodeCache.cpp`（~L1691–1754）memo 命中与写入各整份 CompileResult 深拷贝；改 `shared_ptr<const CompileResult>` 共享。
- **B5 低**：常量折叠每轮重建 vregDefs；附非 SSA 模式无支配性校验的正确性疑点（待证，需差分验证）。
- **B6 中**：int 特化仅 ADD/SUB/MUL/LT（CompilerExpr.cpp L117–158），建议补 GT/GTE/LTE_SPEC+扩大 isExprKnownInt；字节码变化→chunk 指纹变化→JIT 块缓存键自然失效，**无需递增 kJitCodegenVersion**。

**顺序**：B2→B1→A1→A3/A6/A7→A2 轻量→B3(a)→A4/A5→B6。已排除良好项（VMStack、varCache、方法缓存、透明查找等）与未覆盖范围（interpreter/、jit/ 细节、perf-baseline 对比）见上文检查点 C/E 节。待预算恢复后可按检查点 Next Steps 补写文件。