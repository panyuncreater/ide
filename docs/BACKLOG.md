# BACKLOG（唯一活跃工作清单）

> 规则：本文件是**唯一**开放项清单；审计报告只引用 ID，不再维护“剩余未修”列表。
> 状态：`open`（待做）/ `blocked`（外部阻塞）/ `verifying`（待证实）/ `candidate`（优化候选，未立项）。
> 关闭时删除本行并在 CHANGELOG 记录；已否决方案见 `docs/specs/rejected-decisions.md`。
> 轮末检查：超过 5 轮未更新的 `open/verifying` 行必须重新定级或关闭。

## 1. 缺陷与阻塞

| ID | 严重性 | 状态 | 主题 | 证据 | 下一步 |
|----|--------|------|------|------|--------|
| B-001 | P2 | blocked | `trySkipExplicitTypeArgs` 白名单缺 `:`/`(`/`)`，复合类型实参回退为比较解析 | `docs/code-audit-report-2026-10-21.md` §3.4、§7.2、§10.2 | Parser.cpp 写入守卫误判解除后：白名单补配对平衡（`(` 仅允许前缀 `TK_FUN`）+ 四后端回归 |
| B-002 | P2 | blocked | enum 变体构造携带显式类型实参时 AST 降级为 MemberAccess+FunCall | 同上 §3.5 | 在 `match(TK_LPAREN)` 前前置 `trySkipExplicitTypeArgs()` + 四后端回归 |
| B-003 | P2 | blocked | 参数默认值为数组/字典字面量时解析脱轨或静默不绑定 | 同上 §3.2、§6.2、§10.2；`docs/audit-notes-20261022/parser-default-value-investigation.md` | 守卫解除后按调查方案改 `parseParamList` 白名单 + 常量提取 + T1-T6 用例 |
| B-004 | P1 | open | REPL 跨行生成器恢复必然失败：每行独立 `std::async` 线程与协程 ownerThread 防御冲突 | `docs/audit-notes-20261022/robustness-gui-cli-lifetime.md` F2 | REPL 会话绑定单一执行线程；与宿主侧 `toString` 线程改造合并设计 |
| B-005 | P1 | open | capi 插件卸载后宿主函数悬垂 | 同上 F7 | 插件句柄生命周期协议 + 卸载时注销 HostFunctionRegistry + 回归 |
| B-006 | P2 | verifying | JIT methodCallIC 以克隆临时串 `c_str()` 地址为键，可能假命中/错误分发 | `docs/audit-notes-20261022/perf-jit-frontend.md` #2 | 改 `InstanceData*` 键；构造多态调用点差分用例证实/证伪 |
| B-007 | P3 | open | `diff_test` 未注册进 ctest，V10/V11 曾因此逃过全量回归 | `docs/code-audit-report-2026-10-21.md` §10.5 | 将 diff_test 纳入 ctest，或把探针迁入 `ConsistencyDiff`（四路口径） |
| B-008 | P2 | open | finally 同帧“abrupt 替换 abrupt”残留同帧续跳条目；递归同 try 站点重入时异常暂存全局可能被覆盖 | `docs/testing.md` 已知限制 #5（AUDIT-R7 残余） | 反模式叠加边角，低优先级；补四路径用例后再定级 |

## 2. 优化候选（均待实测，未立项）

| ID | 严重性 | 状态 | 主题 | 证据 | 下一步 |
|----|--------|------|------|------|--------|
| OPT-01 | P2 | candidate | 解释器链求值按值拷贝导致 COW 伪共享：容器每次变异全量克隆（循环 push O(N²)） | `docs/audit-notes-20261022/perf-interpreter.md` #1 | 单节点 VarRef 原位变异快路径（求值后取槽位指针），零行为变化 + 基准 A/B |
| OPT-02 | P2 | candidate | 每语句/每调用无条件 `debugger()`（mutex + shared_ptr 原子计数） | 同上 #2 | `debuggerAttached_` 原子短路，~7 处调用点包裹 |
| OPT-03 | P2 | candidate | 字符串字面量每次求值堆分配 `StringData` | 同上 #3 | `StringLiteral` 缓存 `mutable Value`（COW 保证隔离） |
| OPT-04 | P2 | candidate | JIT `jitMethodCall` 非 const `className()` 触发整实例 COW 克隆 | `docs/audit-notes-20261022/perf-jit-frontend.md` #1 | 接收者 const 化（同族 `jitMemberGetWithIC` 已修） |
| OPT-05 | P2 | candidate | StackVM 方法返回字段逐字段哈希写回；RegisterVM 已有 directReplace | `docs/audit-notes-20261022/perf-compiler-backends.md` A1 | 接收者整体赋值 + COW 别名论证 + 四后端差分 |
| OPT-06 | P2 | candidate | 帧构造对 fieldCount 次字符串 find | 同上 A2 | 单次遍历 + `fieldSlotIndex` 查槽 |
| OPT-07 | P2 | candidate | RegisterVM 名称版全局变量无 varCache（StackVM 已有） | 同上 A3 | 平坦数组 + 版本失效；逐一核对失效点防悬垂 |
| OPT-08 | P2 | candidate | IR CSE/GVN 以 string 键值编号，每指令 `to_string` | 同上 B1 | 结构化键 `{op,a,b}` 零分配 |
| OPT-09 | P2 | candidate | `optimizeIR` 固定 3 轮全 pass 重扫，无零修改提前退出 | 同上 B2 | 全 pass 无修改即 break；跨轮复用 vregDefs |
| OPT-10 | P2 | candidate | 字节码行列元数据每字节 8B，执行期仅错误路径使用 | 同上 B3 | 4B/字节或 per-instruction 行表；错误消息全量回归 |
| OPT-11 | P2 | candidate | gui/ 47 文件 350+ 处硬编码颜色 | 2026-07-23 排查（全仓 grep 可复核） | 按高/中/低分批迁移 `TeachingTheme`，每文件视觉回归 |
| OPT-12 | P3 | candidate | formatter 逐节点临时串 + 父级拼接，深表达式 O(深度×长度) 拷贝 | `docs/audit-notes-20261022/perf-jit-frontend.md` formatter | 单缓冲线性 append（34 个 visit*），往返等价回归 |
| OPT-13 | P3 | candidate | JIT `jitMethodReturn` this/receiver 两侧整实例克隆回写 | 同上 #3 | 所有权设计立项后再动 |
| OPT-14 | P3 | candidate | parser 每标识符 `StringIntern::intern`（mutex+hash）无实效，VarRef 仍按值拷贝 name | 同上 parser | 去 intern（小改）或 name 改 interned 指针（立项） |
| OPT-15 | P3 | candidate | JIT `jitGetVar/SetVar` 临时 `std::string` 查表 | 同上 #4 | 透明哈希或 slotPtr IC |
| OPT-16 | P3 | candidate | JIT `jitClassNew` 每次构造拼接类名/`.init` 查找 | 同上 #5 | `JitClassInfo` 缓存 init 入口 |
| OPT-17 | P3 | candidate | 调用帧 functionName 拷贝、`currentFrame` 冗余获取、OP_ADD 双重分派、extraSlots 逐个 push | 同上 A4-A7 | 分组小改，逐项基准 |
| OPT-18 | P3 | candidate | BytecodeCache memo 深拷贝、常量折叠重建 vregDefs、int 特化仅 4 指令 | 同上 B4-B6 | 分组小改；B6 字节码变化使块缓存键自然失效 |
