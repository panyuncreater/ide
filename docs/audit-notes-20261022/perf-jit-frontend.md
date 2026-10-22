# perf-audit-jit-frontend — run aedb3fcf participant 2

工具预算耗尽，笔记文件未能写出（read/write 均被预算拦截）——详细版以本回复为准，抱歉未能落盘。以下为审计总结（只读，未改任何源码，未运行构建）：

**JIT（均待证·未实测）**
1.【高】`JITRuntime.cpp jitMethodCall`（约2033/2059/2077行）：`receiver` 经 `bitsToValue`（addRef，已验证）后调非 const `className()` → `ensureUnique` → **每次方法调用整实例 COW 克隆**。同族 `jitMemberGetWithIC` 已修 as_const，此为遗漏。小改、风险极低。
2.【疑似P1】methodCallIC 以克隆临时串 `c_str()` 地址为键：克隆尺寸恒定、即释即分，**不同类名高度可能同键 → 多态调用点假命中、错误方法分发**（推理，未实测）。建议改 InstanceData* 键。
3.【中】`jitMethodReturn`（约1460行起）：this 与 receiver 两侧整实例克隆回写，每次返回 2 克隆 + O(fields) 拷贝，需所有权设计立项。
4.【中低】`jitGetVar/SetVar`（约2520行起）：const char* → 临时 std::string 查 map，建议透明哈希或 slotPtr IC。
5.【低】`jitClassNew`（约900行）：每次构造类名临时串 + ".init" 拼接查找，建议 JitClassInfo 缓存 init 入口。块缓存键/命中/tiering 本身开销可控，不建议动。

**lexer（记录项1：Token lexeme string_view）**
Token ≈88B（lexeme 32B + variant 40B，已验证）；addToken 每 token 构造新串（>15字符堆分配）。改 string_view 约 -18% 体积、免每 token 拷贝，收益中偏低；但 `Formatter::setComments` 按值拷贝 Token 出 scan 作用域（已验证）等四层生命周期波及，**需立项非小改**。

**记录项2：行列元数据 RLE**——line/col 已内联于 Token（8B），无独立数组可压，columnAt 已 O(L) 缓存；**建议关闭该项（不适用）**。

**parser**：`Parser.cpp:2707` 每标识符 `StringIntern::intern`（mutex+hash）而 VarRef 仍按值拷贝 name（已验证）——intern 无实效，建议去 intern（小改）或 name 改 interned 指针（立项）；AST 逐节点 make_unique，arena 化需立项。Token 流借用/引用消费无问题。

**formatter**：lastFormatResult_ 逐节点临时串+父级拼接架构（已验证），深表达式近似 O(深度×长度) 拷贝，建议单缓冲线性 append，34 个 visit* 全改需立项+往返回归；`escapeStringContent` 的 substr 临时串小改。**lint/LintPass 未覆盖**（预算尽，推断有同族拼接风险，下轮补）。