<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE TS>
<TS version="2.1" language="en_US">
<!--
  MiniLang IDE translation file (English, US).
  Generated from zh_CN.ts by scripts/generate_en_us_translation.py.
-->
<context>
    <name>MiniLang</name>
    <message>
        <source>
(无函数 chunk)
        </source>
        <translation type="finished">
(No function chunk)
        </translation>
    </message>
    <message>
        <source>
[Interpreter 异常] 
        </source>
        <translation type="finished">
[Interpreter Exception] 
        </translation>
    </message>
    <message>
        <source>
[Interpreter 路径运行完成]
        </source>
        <translation type="finished">
[Interpreter path Run complete]
        </translation>
    </message>
    <message>
        <source>
[StackVM 异常] 
        </source>
        <translation type="finished">
[StackVM Exception] 
        </translation>
    </message>
    <message>
        <source>
[StackVM 路径: 
        </source>
        <translation type="finished">
[StackVM path: 
        </translation>
    </message>
    <message>
        <source>
    # then 块

        </source>
        <translation type="finished">
 # then block

        </translation>
    </message>
    <message>
        <source>
    BRANCH_FALSE v0, L1       # 跳到 else

        </source>
        <translation type="finished">
 BRANCH_FALSE v0, L1 # jump to else

        </translation>
    </message>
    <message>
        <source>
    BRANCH_FALSE v2, L2        # 跳出循环

        </source>
        <translation type="finished">
 BRANCH_FALSE v2, L2 # jump out of loop

        </translation>
    </message>
    <message>
        <source>
    v0 = LOAD_CONST #2 (3)        # 折叠后的常量

        </source>
        <translation type="finished">
 v0 = LOAD_CONST #2 (3) # folded constant

        </translation>
    </message>
    <message>
        <source>
    v1 = STORE_LOCAL slot=0, v0   # x = 1 (从未使用)

        </source>
        <translation type="finished">
 v1 = STORE_LOCAL slot=0, v0 # x = 1 (never used)

        </translation>
    </message>
    <message>
        <source>
    v2 = ADD v0, v1           # a + 1 (替换 v1→v0，消除 MOVE)

        </source>
        <translation type="finished">
 v2 = ADD v0, v1 # a + 1 (replace v1→v0, eliminate MOVE)

        </translation>
    </message>
    <message>
        <source>
  # slot 0 = this (隐式)

        </source>
        <translation type="finished">
 # slot 0 = this (implicit)

        </translation>
    </message>
    <message>
        <source>
  # upvalue: c (来自 makeCounter 的局部)

        </source>
        <translation type="finished">
 # upvalue: c (from makeCounter's local)

        </translation>
    </message>
    <message>
        <source>
  - &#x27;clear&#x27; 仅清空输出区与续行缓冲,不重置已定义变量/函数/类

        </source>
        <translation type="finished">
 - &#x27;clear&#x27; only clears the output area and continuation buffer; it does not reset defined variables/functions/classes

        </translation>
    </message>
    <message>
        <source>
  - &#x27;reload &quot;mod&quot;&#x27; 清除模块缓存,下次 import 重新加载源码

        </source>
        <translation type="finished">
 - &#x27;reload &quot;mod&quot;&#x27; clears the module cache; the next import reloads the source

        </translation>
    </message>
    <message>
        <source>
  - &#x27;reload all&#x27; 清除所有模块缓存

        </source>
        <translation type="finished">
 - &#x27;reload all&#x27; clears all module caches

        </translation>
    </message>
    <message>
        <source>
  - 表达式语句(如 &#x27;1 + 2;&#x27;)自动求值并打印结果

        </source>
        <translation type="finished">
 - Expression statements (e.g., &#x27;1 + 2;&#x27;) are automatically evaluated and the result is printed

        </translation>
    </message>
    <message>
        <source>
  - 重置全部状态需重启 IDE

        </source>
        <translation type="finished">
 - To reset all state, restart the IDE

        </translation>
    </message>
    <message>
        <source>
  [1, 2, 3]            数组字面量

        </source>
        <translation type="finished">
 [1, 2, 3] array literal

        </translation>
    </message>
    <message>
        <source>
  class Name { ... }   类声明

        </source>
        <translation type="finished">
 class Name {... } class declaration

        </translation>
    </message>
    <message>
        <source>
  for (init; cond; upd) { ... }  for循环

        </source>
        <translation type="finished">
 for (init; cond; upd) {... } for loop

        </translation>
    </message>
    <message>
        <source>
  fun f(x) { ... }     函数声明

        </source>
        <translation type="finished">
 fun f(x) {... } function declaration

        </translation>
    </message>
    <message>
        <source>
  if (cond) { ... }    条件语句

        </source>
        <translation type="finished">
 if (cond) {... } conditional statement

        </translation>
    </message>
    <message>
        <source>
  import &quot;mod&quot; { f }; 模块导入

        </source>
        <translation type="finished">
 import &quot;mod&quot; { f }; module import

        </translation>
    </message>
    <message>
        <source>
  int a = 5;           类型注解变量

        </source>
        <translation type="finished">
 int a = 5; type-annotated variable

        </translation>
    </message>
    <message>
        <source>
  null                 空值

        </source>
        <translation type="finished">
 null null value

        </translation>
    </message>
    <message>
        <source>
  print(expr);         输出

        </source>
        <translation type="finished">
 print(expr); output

        </translation>
    </message>
    <message>
        <source>
  var x = 10;          变量声明

        </source>
        <translation type="finished">
 var x = 10; variable declaration

        </translation>
    </message>
    <message>
        <source>
  while (cond) { ... } 循环语句

        </source>
        <translation type="finished">
 while (cond) {... } loop statement

        </translation>
    </message>
    <message>
        <source>
  {&quot;key&quot;: val}        字典字面量

        </source>
        <translation type="finished">
 {&quot;key&quot;: val} dictionary literal

        </translation>
    </message>
    <message>
        <source>
  多行输入: 未闭合的 { ( [ 或未闭合字符串会自动续行

        </source>
        <translation type="finished">
 Multi-line input: unclosed { ([ or unclosed strings auto-continue

        </translation>
    </message>
    <message>
        <source>
  续行中按回车(空行)可中止续行

        </source>
        <translation type="finished">
 Press Enter on an empty line while continuing to abort continuation

        </translation>
    </message>
    <message>
        <source> (行 %1</source>
        <translation type="finished"> (Line %1</translation>
    </message>
    <message>
        <source> [待运行]</source>
        <translation type="finished"> [Pending]</translation>
    </message>
    <message>
        <source> [无 AST]</source>
        <translation type="finished"> [None AST]</translation>
    </message>
    <message>
        <source> 字节)</source>
        <translation type="finished"> byte)</translation>
    </message>
    <message>
        <source> 字节)，超过上限 (</source>
        <translation type="finished"> byte),exceed limit (</translation>
    </message>
    <message>
        <source> 次迭代&lt;br&gt;&lt;hr&gt;</source>
        <translation type="finished"> iterations&lt;br&gt;&lt;hr&gt;</translation>
    </message>
    <message>
        <source> 次迭代平均&lt;/p&gt;</source>
        <translation type="finished"> iterations average&lt;/p&gt;</translation>
    </message>
    <message>
        <source> 缓存，下次 import 将重新加载]</source>
        <translation type="finished"> cache; the next import will reload]</translation>
    </message>
    <message>
        <source>#</source>
        <translation type="finished">#</translation>
    </message>
    <message>
        <source>
# 实验 1：词法分析 — 从字符到 Token


        </source>
        <translation type="finished">
# Experiment 1: Lexical analysis — from characters to tokens


        </translation>
    </message>
    <message>
        <source>
# 实验 2：递归下降解析 — 从 Token 到 AST


        </source>
        <translation type="finished">
# Experiment 2: Recursive-descent parsing — from tokens to AST


        </translation>
    </message>
    <message>
        <source>
# 实验 3：树遍历解释器 — Visitor 模式执行 AST


        </source>
        <translation type="finished">
# Experiment 3: Tree-walking interpreter — executing AST with the Visitor pattern


        </translation>
    </message>
    <message>
        <source>
# 实验 4：栈式字节码 VM — 编译与执行


        </source>
        <translation type="finished">
# Experiment 4: Stack bytecode VM — compilation and execution


        </translation>
    </message>
    <message>
        <source>
# 实验 5：寄存器式 VM — 三地址码与 IR


        </source>
        <translation type="finished">
# Experiment 5: Register-based VM — three-address code and IR


        </translation>
    </message>
    <message>
        <source>
# 实验 6：三后端一致性 — 语义等价验证


        </source>
        <translation type="finished">
# Experiment 6: Three-backend consistency — semantic equivalence verification


        </translation>
    </message>
    <message>
        <source>
# 实验 7：内存模型 — NaN-boxing 与 COW


        </source>
        <translation type="finished">
# Experiment 7: Memory model — NaN-boxing and COW


        </translation>
    </message>
    <message>
        <source>
# 实验 8：Bug 狩猎 — 从历史 Bug 学习


        </source>
        <translation type="finished">
# Experiment 8: Bug hunting — learning from historical bugs


        </translation>
    </message>
    <message>
        <source>
## 关键概念

        </source>
        <translation type="finished">
## Key concepts

        </translation>
    </message>
    <message>
        <source>
## 实验步骤

        </source>
        <translation type="finished">
## Experiment steps

        </translation>
    </message>
    <message>
        <source>
## 目标

        </source>
        <translation type="finished">
## Goals

        </translation>
    </message>
    <message>
        <source>
## 进阶

        </source>
        <translation type="finished">
## Advanced

        </translation>
    </message>
    <message>
        <source>
## 验证断言

        </source>
        <translation type="finished">
## Verification assertions

        </translation>
    </message>
    <message>
        <source>%1 [%2] %3 ms</source>
        <translation type="finished">%1 [%2] %3 ms</translation>
    </message>
    <message>
        <source>%1 — %2 → %3 条指令（减少 %4 条）</source>
        <translation type="finished">%1 — %2 → %3 instructions(reduce %4 item)</translation>
    </message>
    <message>
        <source>%1x 慢</source>
        <translation type="finished">%1x slow</translation>
    </message>
    <message>
        <source>(无字节码)</source>
        <translation type="finished">(No bytecode)</translation>
    </message>
    <message>
        <source>(无激活寄存器)</source>
        <translation type="finished">(No active registers)</translation>
    </message>
    <message>
        <source>(无行号)</source>
        <translation type="finished">(No line number)</translation>
    </message>
    <message>
        <source>(未知指令)</source>
        <translation type="finished">(Unknown instruction)</translation>
    </message>
    <message>
        <source>(栈为空)</source>
        <translation type="finished">(Stack is empty)</translation>
    </message>
    <message>
        <source>+ 支持字符串拼接（任一操作数为字符串即触发）。</source>
        <translation type="finished">+ support string concatenation(any operand is String i.e. trigger)。</translation>
    </message>
    <message>
        <source>, 列 %1</source>
        <translation type="finished">, Column %1</translation>
    </message>
    <message>
        <source>
- **32 虚拟寄存器**：R0-R31，零堆分配（std::array&lt;Value, 32&gt;）

        </source>
        <translation type="finished">
- **32 virtual register**:R0-R31,zero heap allocation(std::array&lt;Value, 32&gt;)

        </translation>
    </message>
    <message>
        <source>
- **AST 节点**：33 个 NodeType，ASTNode 基类含 line/column/nodeType + accept(Visitor&amp;)


        </source>
        <translation type="finished">
- **AST Node**:33 NodeType,ASTNode base class contain line/column/nodeType + accept(Visitor&amp;)


        </translation>
    </message>
    <message>
        <source>
- **BytecodeChunk**：code（字节流）+ constants（常量池）+ lines（行号映射）

        </source>
        <translation type="finished">
- **BytecodeChunk**:code(byte stream)+ constants(constant pool)+ lines(line number mapping)

        </translation>
    </message>
    <message>
        <source>
- **COW**：写前检查独占所有权，refCount &gt; 1 时复制

        </source>
        <translation type="finished">
- **COW**:write before check exclusive ownership,refCount &gt; 1 when copy

        </translation>
    </message>
    <message>
        <source>
- **ConsistencyDiff 测试**：190 个差分测试用例验证三后端等价

        </source>
        <translation type="finished">
- **ConsistencyDiff test**:190 differential test case verify three-backend equivalence

        </translation>
    </message>
    <message>
        <source>
- **DebugController**：断点 / 单步 / 条件求值 / 沙箱隔离

        </source>
        <translation type="finished">
- **DebugController**:Breakpoint / Step / condition evaluation / sandbox isolation

        </translation>
    </message>
    <message>
        <source>
- **DoS 防护**：MAX_SOURCE_SIZE / MAX_TOKEN_COUNT / MAX_INTERP_DEPTH


        </source>
        <translation type="finished">
- **DoS protection**:MAX_SOURCE_SIZE / MAX_TOKEN_COUNT / MAX_INTERP_DEPTH


        </translation>
    </message>
    <message>
        <source>
- **Environment**：链式作用域，boundInstance_ 缓存优化

        </source>
        <translation type="finished">
- **Environment**:chained scope,boundInstance_ cache optimization

        </translation>
    </message>
    <message>
        <source>
- **GcManager sweep**：周期性清理 refCount==0 的对象防 UAF


        </source>
        <translation type="finished">
- **GcManager sweep**:periodic clean refCount==0 object guard UAF


        </translation>
    </message>
    <message>
        <source>
- **IROp**：52 个 IR 指令（LOAD_CONST/ADD/CALL 等）

        </source>
        <translation type="finished">
- **IROp**:52 IR instruction(LOAD_CONST/ADD/CALL etc.)

        </translation>
    </message>
    <message>
        <source>
- **NaN-boxing**：8 字节 Value，tag bits / payload 分段编码

        </source>
        <translation type="finished">
- **NaN-boxing**:8 byte Value,tag bits / payload segmented encoding

        </translation>
    </message>
    <message>
        <source>
- **OpCode**：~60 个栈式操作码（OP_CONSTANT/OP_ADD/OP_CALL 等）

        </source>
        <translation type="finished">
- **OpCode**:~60 stack-based opcode(OP_CONSTANT/OP_ADD/OP_CALL etc.)

        </translation>
    </message>
    <message>
        <source>
- **RAII guard 构造顺序**：必须在状态修改前构造以捕获正确的 saved 状态


        </source>
        <translation type="finished">
- **RAII guard construct order**:must in state modification before construct by capture correct saved Status


        </translation>
    </message>
    <message>
        <source>
- **REPL**：续行判断（未闭合 {/(/[/字符串/块注释/try 缺 catch）


        </source>
        <translation type="finished">
- **REPL**:continuation detection(unclosed {/(/[/String/block comment/try missing catch)


        </translation>
    </message>
    <message>
        <source>
- **RefCounted**：侵入式引用计数基类，数组/字典/字符串/InstanceData 都继承

        </source>
        <translation type="finished">
- **RefCounted**:intrusive Reference counting base class,Array/Dictionary/String/InstanceData all Inheritance

        </translation>
    </message>
    <message>
        <source>
- **RegOp**：~50 个三地址码操作码

        </source>
        <translation type="finished">
- **RegOp**:~50 three-address code opcode

        </translation>
    </message>
    <message>
        <source>
- **Token 类型**：关键字 / 标识符 / 字面量 / 运算符 / 分隔符

        </source>
        <translation type="finished">
- **Token Type**:keyword / identifier / literal / operator / delimiter

        </translation>
    </message>
    <message>
        <source>
- **Visitor 模式**：每个 AST 节点类型对应一个 visit 方法

        </source>
        <translation type="finished">
- **Visitor mode**:each AST node type corresponds to one visit Method

        </translation>
    </message>
    <message>
        <source>
- **int48 内联存储**：int 在 int48 范围内联存储，超范围自动装箱

        </source>
        <translation type="finished">
- **int48 inlined storage**:int in int48 within range lined storage,out of range autoboxing

        </translation>
    </message>
    <message>
        <source>
- **三后端一致性**：整数除法截断向零 / and-or 短路返回原值 / 类型注解强制 / super 调用语义

        </source>
        <translation type="finished">
- **three-backend consistency**:integer division truncates toward zero / and-or short-circuit return original value / type annotation force / super call semantics

        </translation>
    </message>
    <message>
        <source>
- **三后端对称性审计**：修改某后端时同步检查其他两个后端

        </source>
        <translation type="finished">
- **three-backend symmetry audit**:Modify some backend when check synchronously other two backend

        </translation>
    </message>
    <message>
        <source>
- **优先级链**：assignment → or_ → and_ → equality → comparison → term → factor → unary → call → primary

        </source>
        <translation type="finished">
- **precedence chain**:assignment → or_ → and_ → equality → comparison → term → factor → unary → call → primary

        </translation>
    </message>
    <message>
        <source>
- **优化 pass**：常量折叠 / 死代码消除 / 复制传播


        </source>
        <translation type="finished">
- **optimization pass**:constant folding / dead-code elimination / copy propagation


        </translation>
    </message>
    <message>
        <source>
- **全局槽位**：编译期分配，运行时通过 slot 索引访问


        </source>
        <translation type="finished">
- **global slot**:compile time allocate,runtime via slot index access


        </translation>
    </message>
    <message>
        <source>
- **列号计算**：按 UTF-8 码位计数（columnAt）

        </source>
        <translation type="finished">
- **column number computation**:by UTF-8 code point count(columnAt)

        </translation>
    </message>
    <message>
        <source>
- **已知差异**：非方法上下文 super 错误消息文本不同（错误类型相同，行为一致）

        </source>
        <translation type="finished">
- **known differences**:non-method on below text super error message text different(error type same,behavior consistent)

        </translation>
    </message>
    <message>
        <source>
- **常量池去重**：类型严格匹配 + 数值相等（BUG-CP-1 修复后）

        </source>
        <translation type="finished">
- **constant pool deduplication**:type strict match + numeric equality(BUG-CP-1 After the fix)

        </translation>
    </message>
    <message>
        <source>
- **操作数栈**：定长 Value[1024]，push/pop 模型

        </source>
        <translation type="finished">
- **operand stack**:fixed-length Value[1024],push/pop model

        </translation>
    </message>
    <message>
        <source>
- **注释分离**：MiniLang 将 TK_LINE_COMMENT/TK_BLOCK_COMMENT 从主流分离到 lexer.comments()，供 Formatter 保留注释

        </source>
        <translation type="finished">
- **comment separation**:MiniLang TK_LINE_COMMENT/TK_BLOCK_COMMENT from mainstream separation to lexer.comments(),for Formatter preserve comments

        </translation>
    </message>
    <message>
        <source>
- **深度防护**：MAX_PARSE_DEPTH / MAX_BLOCK_DEPTH 通过 DepthGuard RAII 守卫防止 C++ 栈溢出

        </source>
        <translation type="finished">
- **depth protection**:MAX_PARSE_DEPTH / MAX_BLOCK_DEPTH via DepthGuard RAII guard prevents C++ stack overflow

        </translation>
    </message>
    <message>
        <source>
- **虚拟寄存器**：SSA-like，nextVReg 分配

        </source>
        <translation type="finished">
- **virtual register**:SSA-like,nextVReg allocate

        </translation>
    </message>
    <message>
        <source>
- **错误恢复**：synchronize() 在出错后跳到下一个同步点（语句边界）继续解析

        </source>
        <translation type="finished">
- **error recovery**:synchronize() in after error jump to Next sync point(Statement boundary)continue parsing

        </translation>
    </message>
    <message>
        <source>
- **错误消息归一化**：差分时归一化错误类型而非文本


        </source>
        <translation type="finished">
- **error message normalization**:when differential normalization error type rather than text


        </translation>
    </message>
    <message>
        <source>
- **错误路径栈平衡**：每个 error return 点必须 popN 清理栈

        </source>
        <translation type="finished">
- **error path stack balance**:each error return point must popN clean Stack

        </translation>
    </message>
    <message>
        <source>
- **高频 Bug 模式**：栈不平衡 / UAF / 泄漏 / 语义不一致 / 竞态 / 错误传播 / 边界条件

        </source>
        <translation type="finished">
- **high-frequency Bug mode**:stack imbalance / UAF / leak / semantic inconsistency / race / error propagation / boundary condition

        </translation>
    </message>
    <message>
        <source>
- + 的右子节点是 BinaryOp(*)


        </source>
        <translation type="finished">
- + right child node is BinaryOp(*)


        </translation>
    </message>
    <message>
        <source>
- AST 根节点是 Block

        </source>
        <translation type="finished">
- AST root node is Block

        </translation>
    </message>
    <message>
        <source>
- IR 中应看到 vreg 分配与三地址码结构

        </source>
        <translation type="finished">
- IR in should see vreg allocate and three-address code structure

        </translation>
    </message>
    <message>
        <source>
- Token 表中应有 5 个主流 token：var / x / = / 42 / ;

        </source>
        <translation type="finished">
- Token table in should has 5 mainstream token:var / x / = / 42 /;

        </translation>
    </message>
    <message>
        <source>
- VarDecl 的 initializer 是 BinaryOp(+)

        </source>
        <translation type="finished">
- VarDecl initializer is BinaryOp(+)

        </translation>
    </message>
    <message>
        <source>
- a 与 b 共享同一 ArrayData 直到第一次写

        </source>
        <translation type="finished">
- a and b share the same ArrayData directly to first times write

        </translation>
    </message>
    <message>
        <source>
- and/or 短路返回操作数原值（非布尔）


        </source>
        <translation type="finished">
- and/or short-circuit return operand original value(non-boolean)


        </translation>
    </message>
    <message>
        <source>
- print(x); 对应 4 个 token：print / ( / x / ) / ;

        </source>
        <translation type="finished">
- print(x); corresponds to 4 token:print / (/ x /) /;

        </translation>
    </message>
    <message>
        <source>
- 三后端都输出：3 / 3.5 / 0 / 0

        </source>
        <translation type="finished">
- three backends all Output:3 / 3.5 / 0 / 0

        </translation>
    </message>
    <message>
        <source>
- 任何修改 b 的操作触发 COW，a 不受影响

        </source>
        <translation type="finished">
- any modify b operation trigger COW,a unaffected

        </translation>
    </message>
    <message>
        <source>
- 优化启用后 IR 指令数减少


        </source>
        <translation type="finished">
- after optimization is enabled IR instruction count reduce


        </translation>
    </message>
    <message>
        <source>
- 修改为 `var x = (1 + 2) * 3;` 观察括号如何改变 AST 结构

        </source>
        <translation type="finished">
- modify to `var x = (1 + 2) * 3;` observe parentheses e.g. how change AST structure

        </translation>
    </message>
    <message>
        <source>- 修改源码观察哪些情况下三后端可能不一致（如默认参数表达式）</source>
        <translation type="finished">- modify source observe which cases below three backends may inconsistent(e.g. default parameter expression)</translation>
    </message>
    <message>
        <source>- 启用 IR 路径观察字节码差异</source>
        <translation type="finished">- enable IR path observe Bytecode difference</translation>
    </message>
    <message>
        <source>
- 启用复制传播观察 v0/v1 是否被消除

        </source>
        <translation type="finished">
- enable copy propagation observe v0/v1 whether eliminate

        </translation>
    </message>
    <message>
        <source>- 在 REPL 中输入多行代码观察续行判断</source>
        <translation type="finished">- in REPL in input multi Line code observe continuation detection</translation>
    </message>
    <message>
        <source>
- 字符串拼接（+）任一操作数为字符串即触发


        </source>
        <translation type="finished">
- string concatenation(+)any operand is String i.e. trigger


        </translation>
    </message>
    <message>
        <source>
- 完成 10 道题后尝试在主编辑器中重现某个 Bug

        </source>
        <translation type="finished">
- complete 10 problems after try in main editor in reproduce some Bug

        </translation>
    </message>
    <message>
        <source>
- 寄存器 VM 执行后 y == 9

        </source>
        <translation type="finished">
- Register VM after execution y == 9

        </translation>
    </message>
    <message>
        <source>
- 尝试 3 层嵌套闭包（参见 BUG-UV-1）

        </source>
        <translation type="finished">
- try 3 layers nested Closure(see BUG-UV-1)

        </translation>
    </message>
    <message>
        <source>
- 尝试嵌套左值 `a[0][1] = x;` 观察栈平衡（参见 BUG-CP-2）

        </source>
        <translation type="finished">
- try nested left Value `a[0][1] = x;` observe stack balance(see BUG-CP-2)

        </translation>
    </message>
    <message>
        <source>
- 尝试嵌套数组 `[[1,2],[3,4]]` 观察深拷贝 vs 浅拷贝

        </source>
        <translation type="finished">
- try nested Array `[[1,2],[3,4]]` observe deep-copy vs shallow-copy

        </translation>
    </message>
    <message>
        <source>- 尝试死代码（如 `var unused = 5;`）观察 DCE 效果</source>
        <translation type="finished">- try dead code(e.g. `var unused = 5;`)observe DCE effect</translation>
    </message>
    <message>
        <source>
- 尝试触发已知差异（非方法上下文 super）观察错误消息差异

        </source>
        <translation type="finished">
- try trigger known differences(non-method on below text super)observe Error message difference

        </translation>
    </message>
    <message>
        <source>- 尝试输入多字节 UTF-8 字符（如 &quot;é&quot;）观察列号计数</source>
        <translation type="finished">- try input multi-byte UTF-8 character(e.g. &quot;é&quot;)observe column number counting</translation>
    </message>
    <message>
        <source>
- 尝试输入非法字符（如 $）观察错误 token

        </source>
        <translation type="finished">
- try input invalid character(e.g. $)observe Error token

        </translation>
    </message>
    <message>
        <source>
- 常量池去重：相同字面量共享索引

        </source>
        <translation type="finished">
- constant pool deduplication:same literal share the index

        </translation>
    </message>
    <message>
        <source>
- 整数除法 7/2 三后端一致为 3

        </source>
        <translation type="finished">
- integer division 7/2 three backends consistent is 3

        </translation>
    </message>
    <message>
        <source>
- 条件断点求值不污染程序状态（沙箱保存/恢复所有可变状态）


        </source>
        <translation type="finished">
- conditional breakpoint evaluate not pollute program Status(sandbox Save/restore all mutable state)


        </translation>
    </message>
    <message>
        <source>
- 栈式 VM 执行后 x == 3


        </source>
        <translation type="finished">
- stack-based VM after execution x == 3


        </translation>
    </message>
    <message>
        <source>
- 注释 token 不计入主流


        </source>
        <translation type="finished">
- comment token not count into mainstream


        </translation>
    </message>
    <message>
        <source>- 测试字符串索引 UTF-8 多字节字符（参见 BUG-INTP-3）</source>
        <translation type="finished">- test string index UTF-8 multi-byte character(see BUG-INTP-3)</translation>
    </message>
    <message>
        <source>
- 浮点除法 7.0/2 三后端一致为 3.5

        </source>
        <translation type="finished">
- float division 7.0/2 three backends consistent is 3.5

        </translation>
    </message>
    <message>
        <source>
- 理解三后端对称性约束：修改指令布局时必须同步更新 instructionSizeAt

        </source>
        <translation type="finished">
- understand three-backend symmetry constraint:Modify instruction layout when must update synchronously instructionSizeAt

        </translation>
    </message>
    <message>
        <source>
- 理解条件断点沙箱必须保存/恢复所有可变状态


        </source>
        <translation type="finished">
- understand conditional breakpoint sandbox must save/restore all mutable state


        </translation>
    </message>
    <message>
        <source>
- 第一个子节点是 VarDecl

        </source>
        <translation type="finished">
- first child node is VarDecl

        </translation>
    </message>
    <message>
        <source>
- 编译后字节码长度合理（约 6-8 字节）

        </source>
        <translation type="finished">
- after compilation the bytecode length is reasonable(about 6-8 byte)

        </translation>
    </message>
    <message>
        <source>
- 能区分 Value::equals()（数值相等）与类型严格相等

        </source>
        <translation type="finished">
- can distinguish Value::equals()(numeric equality)and strict type equality

        </translation>
    </message>
    <message>
        <source>
- 能识别&quot;push 在检查之前&quot;反模式

        </source>
        <translation type="finished">
- can recognize&quot;push in check before&quot;anti-pattern

        </translation>
    </message>
    <message>
        <source>
- 调用栈顶帧行号显示当前执行行（BUG-DBG-1 修复后）

        </source>
        <translation type="finished">
- top of call stack frame line number show the currently executing line(BUG-DBG-1 After the fix)

        </translation>
    </message>
    <message>
        <source>- 输入语法错误（如 `var = ;`）观察错误恢复</source>
        <translation type="finished">- enter a syntax error(e.g. `var =;`)observe error recovery</translation>
    </message>
    <message>
        <source>- 阅读 project_memory.md 中的&quot;Lessons Learned&quot;加深理解</source>
        <translation type="finished">- read project_memory.md in &quot;Lessons Learned&quot;deepen understanding</translation>
    </message>
    <message>
        <source>
--- AST 树形结构（最大深度 12）---

        </source>
        <translation type="finished">
--- AST tree structure (max depth 12)---

        </translation>
    </message>
    <message>
        <source>--- VM 执行结束 ---</source>
        <translation type="finished">--- VM execution finished ---</translation>
    </message>
    <message>
        <source>--- 执行引擎切换: %1 ---</source>
        <translation type="finished">--- execution engine switched: %1 ---</translation>
    </message>
    <message>
        <source>--- 程序执行结束 ---</source>
        <translation type="finished">--- program execution finished ---</translation>
    </message>
    <message>
        <source>--- 调试终止 ---</source>
        <translation type="finished">--- debug terminated ---</translation>
    </message>
    <message>
        <source>... (还有 %1 帧未显示)</source>
        <translation type="finished">... (still has %1 frame not shown)</translation>
    </message>
    <message>
        <source>
// 假设 utils.mini 中有 export fun greet(name) { ... }
import { greet } from &quot;utils.mini&quot;;
print(greet(&quot;MiniLang&quot;));
        </source>
        <translation type="finished">
// assume utils.mini in has export fun greet(name) {... }
import { greet } from &quot;utils.mini&quot;;
print(greet(&quot;MiniLang&quot;));
        </translation>
    </message>
    <message>
        <source>
// 启用 VM 单步调试后执行：
fun f() { return 1; }
print(f());
        </source>
        <translation type="finished">
// enable VM single-step debugging after execute:
fun f() { return 1; }
print(f());
        </translation>
    </message>
    <message>
        <source>
// 在 REPL 中：
import { x } from &quot;./utils.mini&quot;;
// 然后修改 utils.mini 后尝试 reload：
// reload 命令内部调用 clearModuleCache(&quot;./utils.mini&quot;) 但实际缓存 key 是 &quot;utils.mini&quot;
        </source>
        <translation type="finished">
// in REPL in:
import { x } from &quot;./utils.mini&quot;;
// then modify utils.mini after try reload:
// reload command internal call clearModuleCache(&quot;./utils.mini&quot;) but actual cache key is &quot;utils.mini&quot;
        </translation>
    </message>
    <message>
        <source>
// 触发路径遍历尝试（应被拒绝）
import { x } from &quot;C:etc/passwd&quot;;
        </source>
        <translation type="finished">
// trigger path traversal try(should be rejected)
import { x } from &quot;C:etc/passwd&quot;;
        </translation>
    </message>
    <message>
        <source>// 闭包内访问外层变量</source>
        <translation type="finished">// Closure in access outer variables</translation>
    </message>
    <message>
        <source>
1. 在编辑器中输入以下代码：

        </source>
        <translation type="finished">
1. in editor in enter the following code:

        </translation>
    </message>
    <message>
        <source>
1. 在编辑器中输入：

        </source>
        <translation type="finished">
1. in editor in input:

        </translation>
    </message>
    <message>
        <source>
1. 打开&quot;Bug 狩猎&quot;面板

        </source>
        <translation type="finished">
1. open the "Bug Hunt" panel

        </translation>
    </message>
    <message>
        <source>1. 注册（registerTracked）</source>
        <translation type="finished">1. register (registerTracked)</translation>
    </message>
    <message>
        <source>1. 源码</source>
        <translation type="finished">1. Source</translation>
    </message>
    <message>
        <source>2. Token</source>
        <translation type="finished">2. Token</translation>
    </message>
    <message>
        <source>
2. 切换到 RegisterVM 模式（视图菜单）

        </source>
        <translation type="finished">
2. switch to RegisterVM mode(View menu)

        </translation>
    </message>
    <message>
        <source>
2. 打开&quot;三后端并行对比面板&quot;

        </source>
        <translation type="finished">
2. Open&quot;three backends parallel comparison Panel&quot;

        </translation>
    </message>
    <message>
        <source>
2. 打开&quot;编译管线可视化面板&quot;，切到 AST 步骤

        </source>
        <translation type="finished">
2. Open&quot;Compilation pipeline visualization Panel&quot;,switch to AST step

        </translation>
    </message>
    <message>
        <source>
2. 打开&quot;编译管线可视化面板&quot;，切到 Token 步骤

        </source>
        <translation type="finished">
2. Open&quot;Compilation pipeline visualization Panel&quot;,switch to Token step

        </translation>
    </message>
    <message>
        <source>
2. 打开&quot;编译管线可视化面板&quot;，切到字节码步骤

        </source>
        <translation type="finished">
2. Open&quot;Compilation pipeline visualization Panel&quot;,switch to bytecode step

        </translation>
    </message>
    <message>
        <source>
2. 点击&quot;调试&quot;按钮（F6），设置断点在 fib 函数体首行

        </source>
        <translation type="finished">
2. click "Debug" button(F6),set breakpoint at fib function body first line

        </translation>
    </message>
    <message>
        <source>2. 触发时机</source>
        <translation type="finished">2. trigger timing</translation>
    </message>
    <message>
        <source>
2. 运行观察输出

        </source>
        <translation type="finished">
2. run and observe the output

        </translation>
    </message>
    <message>
        <source>
2. 选择 BUG-CP-1（常量池去重陷阱）

        </source>
        <translation type="finished">
2. select BUG-CP-1(constant pool deduplication pitfall)

        </translation>
    </message>
    <message>
        <source>3 层嵌套时中间函数不直接引用外层变量则 currentUpvalueNames_ 为空，内层函数无法透传捕获。</source>
        <translation type="finished">3 layers nested when middle function not directly reference outer variable then currentUpvalueNames_ is empty,inner Function cannot pass-through capture。</translation>
    </message>
    <message>
        <source>3. AST</source>
        <translation type="finished">3. AST</translation>
    </message>
    <message>
        <source>3. Mark 阶段</source>
        <translation type="finished">3. Mark phase</translation>
    </message>
    <message>
        <source>
3. 修改为 `var b = a; b[0] = 99;` 验证 COW 写时复制

        </source>
        <translation type="finished">
3. modify to `var b = a; b[0] = 99;` verify COW write when copy

        </translation>
    </message>
    <message>
        <source>
3. 单步进入 / 跳过 / 跳出，观察调用栈

        </source>
        <translation type="finished">
3. step into / skip / step out,observe Call stack

        </translation>
    </message>
    <message>
        <source>
3. 打开&quot;编译管线可视化面板&quot;，切到 IR 步骤

        </source>
        <translation type="finished">
3. Open&quot;Compilation pipeline visualization Panel&quot;,switch to IR Step

        </translation>
    </message>
    <message>
        <source>
3. 点击&quot;运行对比&quot;按钮

        </source>
        <translation type="finished">
3. click&quot;Run comparison&quot;button

        </translation>
    </message>
    <message>
        <source>
3. 观察 AST 树形结构：BinaryOp(+) 左 VarDecl(=x), 右 BinaryOp(*) 左 2 右 3

        </source>
        <translation type="finished">
3. observe AST tree structure:BinaryOp(+) left VarDecl(=x), right BinaryOp(*) left 2 right 3

        </translation>
    </message>
    <message>
        <source>
3. 观察 Token 表：每行包含 type/lexeme/line/column 字段

        </source>
        <translation type="finished">
3. observe Token table:each line contains type/lexeme/line/column Field

        </translation>
    </message>
    <message>
        <source>
3. 观察反汇编：

        </source>
        <translation type="finished">
3. observe disassembly:

        </translation>
    </message>
    <message>
        <source>
3. 阅读背景：int(0) 与 float(0.0) 共享常量索引的后果

        </source>
        <translation type="finished">
3. read background:int(0) and float(0.0) share the constant index after result

        </translation>
    </message>
    <message>
        <source>4. IR</source>
        <translation type="finished">4. IR</translation>
    </message>
    <message>
        <source>4. Sweep 阶段</source>
        <translation type="finished">4. Sweep phase</translation>
    </message>
    <message>
        <source>
4. 切换到 VM 模式，单步执行观察栈变化


        </source>
        <translation type="finished">
4. switch to VM mode,single-step execution observe Stack change


        </translation>
    </message>
    <message>
        <source>
4. 在内嵌编辑器中预测输出

        </source>
        <translation type="finished">
4. in inline editor in predict output

        </translation>
    </message>
    <message>
        <source>
4. 测试大数组传递观察引用计数（暂无可视化，需通过性能感知）


        </source>
        <translation type="finished">
4. test large array pass observe Reference counting(temporarily none visualization,needs via performance awareness)


        </translation>
    </message>
    <message>
        <source>
4. 观察 IR 指令：

        </source>
        <translation type="finished">
4. observe IR instruction:

        </translation>
    </message>
    <message>
        <source>
4. 观察三列输出是否一致


        </source>
        <translation type="finished">
4. observe three columns Output whether consistent


        </translation>
    </message>
    <message>
        <source>
4. 设置条件断点 `n == 5`，验证只在 n=5 时暂停


        </source>
        <translation type="finished">
4. set conditional breakpoint `n == 5`,verify only at n=5 pauses when


        </translation>
    </message>
    <message>
        <source>
4. 验证：乘法比加法优先级高，* 节点在 + 节点的右侧子树


        </source>
        <translation type="finished">
4. verify: multiplication has higher precedence than addition,* Node in + Node right subtree


        </translation>
    </message>
    <message>
        <source>
4. 验证：注释 token 不在主流中（在 lexer.comments() 中）


        </source>
        <translation type="finished">
4. verify: comment token not in in the mainstream(in lexer.comments() in)


        </translation>
    </message>
    <message>
        <source>5. UAF 防护</source>
        <translation type="finished">5. UAF protection</translation>
    </message>
    <message>
        <source>
5. 启用优化观察 v2 是否被常量折叠


        </source>
        <translation type="finished">
5. enable optimization observe v2 whether constant folding


        </translation>
    </message>
    <message>
        <source>5. 字节码</source>
        <translation type="finished">5. Bytecode</translation>
    </message>
    <message>
        <source>
5. 点击&quot;运行验证&quot;查看实际输出

        </source>
        <translation type="finished">
5. click "Run verification" view actual output

        </translation>
    </message>
    <message>
        <source>6. 已知限制</source>
        <translation type="finished">6. known limitations</translation>
    </message>
    <message>
        <source>
6. 逐步查看提示，最后查看答案

        </source>
        <translation type="finished">
6. step by step view hint,finally view answer

        </translation>
    </message>
    <message>
        <source>
7. 重复 5-10 道题，覆盖不同 Bug 模式


        </source>
        <translation type="finished">
7. repeat 5-10 problems,cover different Bug mode


        </translation>
    </message>
    <message>
        <source>
;
            break;
    }
    return os.str();
}

std::string bitsToBinary(uint64_t bits) {
    std::string s(64, &#x27;0&#x27;);
    for (int i = 0; i &lt; 64; ++i) {
        if (bits &amp; (1ULL &lt;&lt; (63 - i))) s[i] = &#x27;1&#x27;;
    }
    return s;
}

}  // namespace

// ============================================================
// VariableInspectorPanel 实现
// ============================================================

VariableInspectorPanel::VariableInspectorPanel(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer-&gt;setContentsMargins(4, 4, 4, 4);
    outer-&gt;setSpacing(4);

    auto* pageBar = new QHBoxLayout;
    pageLiveBtn_    = new QPushButton(tr(
        </source>
        <translation type="finished">
;
 break;
 }
 return os.str();
}

std::string bitsToBinary(uint64_t bits) {
 std::string s(64, &#x27;0&#x27;);
 for (int i = 0; i &lt; 64; ++i) {
 if (bits &amp; (1ULL &lt;&lt; (63 - i))) s[i] = &#x27;1&#x27;;
 }
 return s;
}

} // namespace

// ============================================================
// VariableInspectorPanel implement
// ============================================================

VariableInspectorPanel::VariableInspectorPanel(QWidget* parent): QWidget(parent) {
 auto* outer = new QVBoxLayout(this);
 outer-&gt;setContentsMargins(4, 4, 4, 4);
 outer-&gt;setSpacing(4);

 auto* pageBar = new QHBoxLayout;
 pageLiveBtn_ = new QPushButton(tr(
        </translation>
    </message>
    <message>
        <source>&lt;div style=&#x27;color:#6e6e6e;padding:8px;&#x27;&gt;(未启用 IR 编译 — 在视图菜单勾选编译分析面板后查看)&lt;/div&gt;</source>
        <translation type="finished">&lt;div style=&#x27;color:#6e6e6e;padding:8px;&#x27;&gt;(not yet enable IR Compile — in check in view menu Compilation analysis panel after View)&lt;/div&gt;</translation>
    </message>
    <message>
        <source>&lt;div style=&#x27;color:#6e6e6e;padding:8px;&#x27;&gt;(空 IR — 无基本块)&lt;/div&gt;</source>
        <translation type="finished">&lt;div style=&#x27;color:#6e6e6e;padding:8px;&#x27;&gt;(empty IR — None basic block)&lt;/div&gt;</translation>
    </message>
    <message>
        <source>&lt;div style=&#x27;color:#808080;padding:2px 0;&#x27;&gt;... (IR 超过 %1 行，已截断显示)&lt;/div&gt;</source>
        <translation type="finished">&lt;div style=&#x27;color:#808080;padding:2px 0;&#x27;&gt;... (IR exceed %1 Line, truncated display)&lt;/div&gt;</translation>
    </message>
    <message>
        <source>&lt;div style=&#x27;color:red;padding:8px;&#x27;&gt;(IR 渲染失败: %1)&lt;/div&gt;</source>
        <translation type="finished">&lt;div style=&#x27;color:red;padding:8px;&#x27;&gt;(IR render failed: %1)&lt;/div&gt;</translation>
    </message>
    <message>
        <source>&lt;div style=&#x27;color:red;padding:8px;&#x27;&gt;(IR 渲染失败: 未知错误)&lt;/div&gt;</source>
        <translation type="finished">&lt;div style=&#x27;color:red;padding:8px;&#x27;&gt;(IR render failed: unknown error)&lt;/div&gt;</translation>
    </message>
    <message>
        <source>&lt;h2&gt;GcManager — 循环引用垃圾收集器&lt;/h2&gt;</source>
        <translation type="finished">&lt;h2&gt;GcManager — reference cycle garbage collector&lt;/h2&gt;</translation>
    </message>
    <message>
        <source>&lt;h3&gt;NaN-boxing 位&lt;/h3&gt;</source>
        <translation type="finished">&lt;h3&gt;NaN-boxing bits&lt;/h3&gt;</translation>
    </message>
    <message>
        <source>&lt;h3&gt;传播路径&lt;/h3&gt;&lt;ol&gt;</source>
        <translation type="finished">&lt;h3&gt;propagation path&lt;/h3&gt;&lt;ol&gt;</translation>
    </message>
    <message>
        <source>&lt;h3&gt;值表示&lt;/h3&gt;</source>
        <translation type="finished">&lt;h3&gt;value representation&lt;/h3&gt;</translation>
    </message>
    <message>
        <source>&lt;h3&gt;变量: %1&lt;/h3&gt;</source>
        <translation type="finished">&lt;h3&gt;Variable: %1&lt;/h3&gt;</translation>
    </message>
    <message>
        <source>&lt;h3&gt;堆布局&lt;/h3&gt;</source>
        <translation type="finished">&lt;h3&gt;Heap layout&lt;/h3&gt;</translation>
    </message>
    <message>
        <source>&lt;h3&gt;捕获变量&lt;/h3&gt;&lt;ul&gt;</source>
        <translation type="finished">&lt;h3&gt;captured variable&lt;/h3&gt;&lt;ul&gt;</translation>
    </message>
    <message>
        <source>&lt;h3&gt;操作数格式&lt;/h3&gt;</source>
        <translation type="finished">&lt;h3&gt;operand format&lt;/h3&gt;</translation>
    </message>
    <message>
        <source>&lt;h3&gt;教学注解&lt;/h3&gt;</source>
        <translation type="finished">&lt;h3&gt;teaching annotation&lt;/h3&gt;</translation>
    </message>
    <message>
        <source>&lt;h3&gt;教学注释&lt;/h3&gt;&lt;p&gt;</source>
        <translation type="finished">&lt;h3&gt;teaching comment&lt;/h3&gt;&lt;p&gt;</translation>
    </message>
    <message>
        <source>&lt;h3&gt;期望栈帧序列&lt;/h3&gt;</source>
        <translation type="finished">&lt;h3&gt;expected stack frame sequence&lt;/h3&gt;</translation>
    </message>
    <message>
        <source>&lt;h3&gt;栈帧: %1&lt;/h3&gt;</source>
        <translation type="finished">&lt;h3&gt;stack frame: %1&lt;/h3&gt;</translation>
    </message>
    <message>
        <source>&lt;h3&gt;栈效果&lt;/h3&gt;</source>
        <translation type="finished">&lt;h3&gt;stack effect&lt;/h3&gt;</translation>
    </message>
    <message>
        <source>&lt;h3&gt;样例代码&lt;/h3&gt;</source>
        <translation type="finished">&lt;h3&gt;sample code&lt;/h3&gt;</translation>
    </message>
    <message>
        <source>&lt;h3&gt;源码&lt;/h3&gt;</source>
        <translation type="finished">&lt;h3&gt;Source&lt;/h3&gt;</translation>
    </message>
    <message>
        <source>&lt;h3&gt;示例代码&lt;/h3&gt;&lt;pre&gt;</source>
        <translation type="finished">&lt;h3&gt;example code&lt;/h3&gt;&lt;pre&gt;</translation>
    </message>
    <message>
        <source>&lt;h3&gt;语义&lt;/h3&gt;</source>
        <translation type="finished">&lt;h3&gt;semantics&lt;/h3&gt;</translation>
    </message>
    <message>
        <source>&lt;h4&gt;Lowering 后的 IR：&lt;/h4&gt;</source>
        <translation type="finished">&lt;h4&gt;Lowering after IR:&lt;/h4&gt;</translation>
    </message>
    <message>
        <source>&lt;h4&gt;示例代码：&lt;/h4&gt;</source>
        <translation type="finished">&lt;h4&gt;example code:&lt;/h4&gt;</translation>
    </message>
    <message>
        <source>&lt;hr&gt;&lt;p&gt;&lt;a href=&quot;#load&quot;&gt;载入到编辑器&lt;/a&gt;&lt;/p&gt;</source>
        <translation type="finished">&lt;hr&gt;&lt;p&gt;&lt;a href=&quot;#load&quot;&gt;load into editor&lt;/a&gt;&lt;/p&gt;</translation>
    </message>
    <message>
        <source>&lt;i&gt;未启用 IR 编译。请在视图菜单启用 IR 模式或触发一次 IR 编译。&lt;/i&gt;</source>
        <translation type="finished">&lt;i&gt;not yet enable IR Compile。please in View menu enable IR mode or trigger once IR Compile。&lt;/i&gt;</translation>
    </message>
    <message>
        <source>&lt;p style=&#x27;color:#cc0000;&#x27;&gt;&lt;b&gt;所有后端均失败&lt;/b&gt;&lt;/p&gt;</source>
        <translation type="finished">&lt;p style=&#x27;color:#cc0000;&#x27;&gt;&lt;b&gt;all backends all fail&lt;/b&gt;&lt;/p&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;AST 节点：&lt;/b&gt; &lt;code&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;AST Node:&lt;/b&gt; &lt;code&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;Bug 行为:&lt;/b&gt;&lt;/p&gt;&lt;p&gt;%6&lt;/p&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;Bug behavior:&lt;/b&gt;&lt;/p&gt;&lt;p&gt;%6&lt;/p&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;IP:&lt;/b&gt; %3 | &lt;b&gt;行:&lt;/b&gt; %4 | &lt;b&gt;帧数:&lt;/b&gt; %5&lt;/p&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;IP:&lt;/b&gt; %3 | &lt;b&gt;line:&lt;/b&gt; %4 | &lt;b&gt;frame count:&lt;/b&gt; %5&lt;/p&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;NaN-boxing 位:&lt;/b&gt; &lt;code&gt;%5&lt;/code&gt;&lt;/p&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;NaN-boxing bits:&lt;/b&gt; &lt;code&gt;%5&lt;/code&gt;&lt;/p&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;值 (toString):&lt;/b&gt; %4&lt;/p&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;Value (toString):&lt;/b&gt; %4&lt;/p&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;值:&lt;/b&gt; %3&lt;/p&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;Value:&lt;/b&gt; %3&lt;/p&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;分类:&lt;/b&gt; </source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;classification:&lt;/b&gt; </translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;分类:&lt;/b&gt; %2&lt;/p&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;classification:&lt;/b&gt; %2&lt;/p&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;加速比：&lt;/b&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;speedup ratio:&lt;/b&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;原始位（binary）：&lt;/b&gt; &lt;code style=&#x27;font-size:10pt;&#x27;&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;raw bits(binary):&lt;/b&gt; &lt;code style=&#x27;font-size:10pt;&#x27;&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;原始位（hex）：&lt;/b&gt; &lt;code&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;raw bits(hex):&lt;/b&gt; &lt;code&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;当前行:&lt;/b&gt; %3&lt;/p&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;current line:&lt;/b&gt; %3&lt;/p&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;捕获类型:&lt;/b&gt; </source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;capture Type:&lt;/b&gt; </translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;最快后端：&lt;/b&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;Fastest backend:&lt;/b&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;最慢后端：&lt;/b&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;most slow backend:&lt;/b&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;期望行为:&lt;/b&gt;&lt;/p&gt;&lt;p&gt;%5&lt;/p&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;expected behavior:&lt;/b&gt;&lt;/p&gt;&lt;p&gt;%5&lt;/p&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;本地变量数:&lt;/b&gt; %4&lt;/p&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;local variable count:&lt;/b&gt; %4&lt;/p&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;条件表达式：&lt;/b&gt; &lt;code&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;conditional expression:&lt;/b&gt; &lt;code&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;栈/堆效应:&lt;/b&gt; </source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;stack/heap effect:&lt;/b&gt; </translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;栈效应:&lt;/b&gt; </source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;stack effect:&lt;/b&gt; </translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;源码：&lt;/b&gt; &lt;code&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;Source:&lt;/b&gt; &lt;code&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;示例：&lt;/b&gt; &lt;code&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;Example:&lt;/b&gt; &lt;code&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;类别:&lt;/b&gt; %3&lt;/p&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;category:&lt;/b&gt; %3&lt;/p&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;类型:&lt;/b&gt; %2&lt;/p&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;Type:&lt;/b&gt; %2&lt;/p&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;类型:&lt;/b&gt; %3&lt;/p&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;Type:&lt;/b&gt; %3&lt;/p&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;类型名:&lt;/b&gt; &lt;code&gt;%3&lt;/code&gt;&lt;/p&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;type name:&lt;/b&gt; &lt;code&gt;%3&lt;/code&gt;&lt;/p&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;背景:&lt;/b&gt;&lt;/p&gt;&lt;p&gt;%4&lt;/p&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;background:&lt;/b&gt;&lt;/p&gt;&lt;p&gt;%4&lt;/p&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;表达式：&lt;/b&gt; &lt;code&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;Expression:&lt;/b&gt; &lt;code&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;解码值：&lt;/b&gt; RefCounted* 指针（48 位虚拟地址）&lt;/p&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;solve code Value:&lt;/b&gt; RefCounted* pointer(48 bits virtual address)&lt;/p&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;解码值：&lt;/b&gt; bool = </source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;solve code Value:&lt;/b&gt; bool = </translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;解码值：&lt;/b&gt; float = </source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;solve code Value:&lt;/b&gt; float = </translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;解码值：&lt;/b&gt; int = </source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;solve code Value:&lt;/b&gt; int = </translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;解码值：&lt;/b&gt; null&lt;/p&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;solve code Value:&lt;/b&gt; null&lt;/p&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;触发行为：&lt;/b&gt; </source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;trigger behavior:&lt;/b&gt; </translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;说明:&lt;/b&gt; </source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;description:&lt;/b&gt; </translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;说明:&lt;/b&gt;&lt;/p&gt;&lt;p&gt;%3&lt;/p&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;description:&lt;/b&gt;&lt;/p&gt;&lt;p&gt;%3&lt;/p&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;调用深度:&lt;/b&gt; %2&lt;/p&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;call Depth:&lt;/b&gt; %2&lt;/p&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;small&gt;注：标准差反映多次运行的稳定性。</source>
        <translation type="finished">&lt;p&gt;&lt;small&gt;note:standard deviation reflects multiple times Run stability。</translation>
    </message>
    <message>
        <source>&lt;p&gt;侵入式引用计数无法回收循环引用（如 &lt;code&gt;a=[]; a.append(a)&lt;/code&gt;）。</source>
        <translation type="finished">&lt;p&gt;intrusive Reference counting cannot Collection reference cycle(e.g. &lt;code&gt;a=[]; a.append(a)&lt;/code&gt;)。</translation>
    </message>
    <message>
        <source>&lt;tr&gt;&lt;th&gt;深度&lt;/th&gt;&lt;th&gt;帧名&lt;/th&gt;&lt;/tr&gt;</source>
        <translation type="finished">&lt;tr&gt;&lt;th&gt;Depth&lt;/th&gt;&lt;th&gt;frame name&lt;/th&gt;&lt;/tr&gt;</translation>
    </message>
    <message>
        <source>
=== 提示 %1 ===
%2
        </source>
        <translation type="finished">
=== Hint %1 ===
%2
        </translation>
    </message>
    <message>
        <source>
=== 答案 ===
%1
        </source>
        <translation type="finished">
=== Answer ===
%1
        </translation>
    </message>
    <message>
        <source>&gt;&gt;&gt; 输入 MiniLang 代码...</source>
        <translation type="finished">&gt;&gt;&gt; input MiniLang code...</translation>
    </message>
    <message>
        <source>ADD 指令 → vreg2。三地址码形式 dest = OP src1, src2。</source>
        <translation type="finished">ADD instruction → vreg2。three-address code form dest = OP src1, src2。</translation>
    </message>
    <message>
        <source>AST</source>
        <translation type="finished">AST</translation>
    </message>
    <message>
        <source>AST → IR lowering</source>
        <translation type="finished">AST → IR lowering</translation>
    </message>
    <message>
        <source>AST 树形图</source>
        <translation type="finished">AST tree diagram</translation>
    </message>
    <message>
        <source>AST 根节点: </source>
        <translation type="finished">AST root node: </translation>
    </message>
    <message>
        <source>
AST 节点数 %1 超过上限 %2，已跳过渲染以避免 UI 卡顿。

        </source>
        <translation type="finished">
AST node count %1 exceeds the limit %2; rendering skipped to avoid UI lag.

        </translation>
    </message>
    <message>
        <source>ArrayData 继承 RefCounted，构造时 refCount=1。</source>
        <translation type="finished">ArrayData inherits RefCounted; refCount=1 on construction.</translation>
    </message>
    <message>
        <source>BinaryOp(ADD) lowering：左侧操作数 → vreg0，右侧 → vreg1，</source>
        <translation type="finished">BinaryOp(ADD) lowering:left side operand → vreg0,right side → vreg1,</translation>
    </message>
    <message>
        <source>Bug 狩猎</source>
        <translation type="finished">Bug Hunt</translation>
    </message>
    <message>
        <source>Bug 触发时：Interpreter 返回 6，VM/RegisterVM 运行时错误。</source>
        <translation type="finished">When the bug triggers: Interpreter returns 6, VM/RegisterVM raise runtime errors.</translation>
    </message>
    <message>
        <source>Bug 触发时：RETURN 指令不触发步进回调，单步跳过。</source>
        <translation type="finished">When the bug triggers: RETURN instruction does not fire the step callback, single-step is skipped.</translation>
    </message>
    <message>
        <source>Bug 触发时：a 与 b 中可能有一个值错乱。</source>
        <translation type="finished">When the bug triggers: one of a or b may have a corrupted value.</translation>
    </message>
    <message>
        <source>Bug 触发时：reload 命令无效，仍使用旧模块。</source>
        <translation type="finished">When the bug triggers: the reload command is ineffective and the old module is still used.</translation>
    </message>
    <message>
        <source>Bug 触发时：两条 export 紧挨，与未导出形式不一致。</source>
        <translation type="finished">When the bug triggers: two export statements are adjacent, inconsistent with the non-exported form.</translation>
    </message>
    <message>
        <source>Bug 触发时：可能加载到非预期文件。</source>
        <translation type="finished">When the bug triggers: an unexpected file may be loaded.</translation>
    </message>
    <message>
        <source>Bug 触发时：循环约 512 次后栈溢出崩溃。</source>
        <translation type="finished">When the bug triggers: stack overflow crash after about 512 iterations.</translation>
    </message>
    <message>
        <source>Bug 触发时：栈溢出崩溃（约 1024 次泄漏后）。</source>
        <translation type="finished">When the bug triggers: stack overflow crash (after about 1024 leaks).</translation>
    </message>
    <message>
        <source>Bug 触发时：调用栈顶显示调用点行号。</source>
        <translation type="finished">When the bug triggers: the top of the call stack shows the call-site line number.</translation>
    </message>
    <message>
        <source>Bug 触发时：运行时错误（无法找到 upvalue）。</source>
        <translation type="finished">When the bug triggers: runtime error (cannot find upvalue).</translation>
    </message>
    <message>
        <source>BytecodeChunk::addConstant 与 RegBytecodeChunk::addConstant 常量池去重时仅依赖 Value::equals()，</source>
        <translation type="finished">BytecodeChunk::addConstant and RegBytecodeChunk::addConstant rely solely on Value::equals() when deduplicating the constant pool, </translation>
    </message>
    <message>
        <source>C 风格 for 循环。init/condition/update 三段式，每段可空。</source>
        <translation type="finished">C-style for loop. Three-part init/condition/update, each of which may be empty.</translation>
    </message>
    <message>
        <source>COW detach 在写前检查 refCount==1，否则深拷贝自身。</source>
        <translation type="finished">COW detach checks refCount==1 before writing; otherwise it deep-copies itself.</translation>
    </message>
    <message>
        <source>COW detach：原 refCount-- (→1)，深拷贝出新对象 refCount=1，写入新对象</source>
        <translation type="finished">COW detach: the original refCount-- (→1), a new object is deep-copied with refCount=1, and the new object is written</translation>
    </message>
    <message>
        <source>COW 写时复制 detach</source>
        <translation type="finished">COW copy-on-write detach</translation>
    </message>
    <message>
        <source>CallFrame::line 在 callNamedFunction 等处初始化为调用点行号后从不更新。</source>
        <translation type="finished">CallFrame::line is initialized to the call-site line number in callNamedFunction and never updated afterwards.</translation>
    </message>
    <message>
        <source>ClassDecl lowering：方法体作为独立 IRFunction 编译，</source>
        <translation type="finished">ClassDecl lowering:method body as independent IRFunction Compile,</translation>
    </message>
    <message>
        <source>ClosureData 通过 weak_ptr&lt;Environment&gt; 打破循环引用。</source>
        <translation type="finished">ClosureData breaks reference cycles via weak_ptr&lt;Environment&gt;.</translation>
    </message>
    <message>
        <source>Compiler 路径缺少前向自由变量分析，resolveUpvalue 是惰性的（仅 visitVarRef 触发）。</source>
        <translation type="finished">The Compiler path lacks forward free-variable analysis; resolveUpvalue is lazy (only triggered by visitVarRef).</translation>
    </message>
    <message>
        <source>DCE pass 识别结果未被使用的指令（无副作用），直接消除。</source>
        <translation type="finished">The DCE pass identifies instructions whose results are unused (no side effects) and eliminates them directly.</translation>
    </message>
    <message>
        <source>Formatter / 往返等价</source>
        <translation type="finished">Formatter / round-trip equivalence</translation>
    </message>
    <message>
        <source>Formatter：ExportStmt 间未插入空行</source>
        <translation type="finished">Formatter: no blank line inserted between ExportStmts</translation>
    </message>
    <message>
        <source>FunCall lowering：每个实参求值到独立 vreg，CALL 指令携带函数索引 + 参数数量。</source>
        <translation type="finished">FunCall lowering:each actual argument evaluate to independent vreg,CALL instruction carries function index + argument count。</translation>
    </message>
    <message>
        <source>GC 在此场景频繁触发，sweep 开销可能拉低三后端共同基线。</source>
        <translation type="finished">GC fires frequently in this scenario; sweep overhead may drag down the three-backend common baseline.</translation>
    </message>
    <message>
        <source>GC 通过 mark-sweep 可回收（mark 时 a 已 marked，sweep 不会误清），</source>
        <translation type="finished">Reclaimable by GC via mark-sweep (a is already marked during mark, so sweep will not clear it by mistake), </translation>
    </message>
    <message>
        <source>GcManager mark-sweep</source>
        <translation type="finished">GcManager mark-sweep</translation>
    </message>
    <message>
        <source>GcManager 实现轻量级 mark-sweep，周期性扫描所有容器节点，</source>
        <translation type="finished">GcManager implements a lightweight mark-sweep that periodically scans all container nodes, </translation>
    </message>
    <message>
        <source>IIFE 每次调用创建新栈帧，参数 captured 绑定当前 i 的值。</source>
        <translation type="finished">Each IIFE call creates a new stack frame; the captured parameter binds the current value of i.</translation>
    </message>
    <message>
        <source>IIFE 用于创建新作用域，避免变量泄漏或闭包共享问题。</source>
        <translation type="finished">IIFE is used to create a new scope, avoiding variable leakage or closure-sharing issues.</translation>
    </message>
    <message>
        <source>IP: %1  |  %2  |  行: %3</source>
        <translation type="finished">IP: %1 | %2 | line: %3</translation>
    </message>
    <message>
        <source>IR</source>
        <translation type="finished">IR</translation>
    </message>
    <message>
        <source>IR 变换</source>
        <translation type="finished">IR transforms</translation>
    </message>
    <message>
        <source>IR 生成失败</source>
        <translation type="finished">IR generation failed</translation>
    </message>
    <message>
        <source>IR 生成异常</source>
        <translation type="finished">IR generation exception</translation>
    </message>
    <message>
        <source>IR 编译发生未知异常</source>
        <translation type="finished">Unknown exception during IR compilation</translation>
    </message>
    <message>
        <source>IR 编译异常: %1</source>
        <translation type="finished">IR compilation exception: %1</translation>
    </message>
    <message>
        <source>IR 路径 / 栈平衡</source>
        <translation type="finished">IR path / stack balance</translation>
    </message>
    <message>
        <source>IR 路径 if 单语句体未 POP</source>
        <translation type="finished">IR path: if single-statement body not POPed</translation>
    </message>
    <message>
        <source>IR 路径 visitIfStmt then/else 分支直接调用 visitNode，未通过 visitStatement 统一包装器处理 POP。</source>
        <translation type="finished">The IR path's visitIfStmt then/else branches call visitNode directly, bypassing the visitStatement wrapper that handles POP.</translation>
    </message>
    <message>
        <source>IfStmt lowering：求值 cond → BRANCH_FALSE 跳到 L_else，</source>
        <translation type="finished">IfStmt lowering:evaluate cond → BRANCH_FALSE jump to L_else,</translation>
    </message>
    <message>
        <source>InstanceData release → refCount=1（ArrayData 仍 refCount=1）</source>
        <translation type="finished">InstanceData release → refCount=1 (ArrayData still refCount=1)</translation>
    </message>
    <message>
        <source>InstanceData → 遍历 fields。递归标记直到所有可达节点 marked=true。</source>
        <translation type="finished">InstanceData → traverse fields. Recursively mark until all reachable nodes have marked=true.</translation>
    </message>
    <message>
        <source>Interpreter</source>
        <translation type="finished">Interpreter</translation>
    </message>
    <message>
        <source>Interpreter 调试中</source>
        <translation type="finished">Interpreter debugging</translation>
    </message>
    <message>
        <source>Interpreter 路径支持任意表达式默认值（b=a+1 求值为 6），VM/RegisterVM 路径仅支持字面量，</source>
        <translation type="finished">The Interpreter path supports arbitrary expression default values (b=a+1 evaluates to 6), while the VM/RegisterVM paths only support literals, </translation>
    </message>
    <message>
        <source>Interpreter::execute() 在 resetState 之后、runStatements 之前调用 collectCycle(空根集)。</source>
        <translation type="finished">Interpreter::execute() in resetState after,runStatements before call collectCycle(empty root set)。</translation>
    </message>
    <message>
        <source>Lexer 通过 INTERP_START/INTERP_END/STRING_PART 三种 token 类型识别。</source>
        <translation type="finished">The lexer recognizes interpolation via three token types: INTERP_START/INTERP_END/STRING_PART.</translation>
    </message>
    <message>
        <source>MiniLang IDE</source>
        <translation type="finished">MiniLang IDE</translation>
    </message>
    <message>
        <source>MiniLang IDE - 启动错误</source>
        <translation type="finished">MiniLang IDE - startup error</translation>
    </message>
    <message>
        <source>MiniLang IDE 快捷键</source>
        <translation type="finished">MiniLang IDE shortcuts</translation>
    </message>
    <message>
        <source>
MiniLang REPL 输出
输入表达式或语句后按回车执行
        </source>
        <translation type="finished">
MiniLang REPL output
Enter an expression or statement and press Enter to execute
        </translation>
    </message>
    <message>
        <source>MiniLang 中 catch 不区分异常类型，因此第一个 catch 块总是匹配。</source>
        <translation type="finished">In MiniLang, catch does not distinguish exception types, so the first catch block always matches.</translation>
    </message>
    <message>
        <source>MiniLang 中 null 是一等值，可与任何变量比较。</source>
        <translation type="finished">In MiniLang, null is a first-class value and can be compared with any variable.</translation>
    </message>
    <message>
        <source>MiniLang 中 throw 是语句而非表达式，不返回值。</source>
        <translation type="finished">In MiniLang, throw is a statement rather than an expression and returns no value.</translation>
    </message>
    <message>
        <source>MiniLang 异常通过 InstanceData 表示，字段访问通过 OP_GET_FIELD 完成，</source>
        <translation type="finished">MiniLang exceptions are represented by InstanceData; field access is done via OP_GET_FIELD, </translation>
    </message>
    <message>
        <source>
MiniLang 支持的语法:

        </source>
        <translation type="finished">
MiniLang supported syntax:

        </translation>
    </message>
    <message>
        <source>MiniLang 无 finally 关键字，但 catch 块可用于资源清理。</source>
        <translation type="finished">MiniLang has no finally keyword, but the catch block can be used for resource cleanup.</translation>
    </message>
    <message>
        <source>MiniLang 的 GC 会在闭包不可达时释放 upvalue。</source>
        <translation type="finished">MiniLang's GC releases upvalues when a closure becomes unreachable.</translation>
    </message>
    <message>
        <source>MiniLang 的 catch 不区分异常类型，因此内层 catch 总是捕获异常。</source>
        <translation type="finished">MiniLang's catch does not distinguish exception types, so the inner catch always captures the exception.</translation>
    </message>
    <message>
        <source>MiniLang 的异常对象是字符串值，catch 匹配不区分类型。</source>
        <translation type="finished">MiniLang's exception object is a string value; catch matching does not distinguish types.</translation>
    </message>
    <message>
        <source>NaN-boxing 编码</source>
        <translation type="finished">NaN-boxing encoding</translation>
    </message>
    <message>
        <source>OP_CLOSURE 指令编码两个 upvalue 条目，每个指定变量位置。</source>
        <translation type="finished">The OP_CLOSURE instruction encodes two upvalue entries, each specifying a variable location.</translation>
    </message>
    <message>
        <source>OP_WRITEBACK_INDEX_VAR/LOCAL 改为整体替换语义（不 pop 索引）后，visitIndexAssign 仍向</source>
        <translation type="finished">After OP_WRITEBACK_INDEX_VAR/LOCAL changed to whole-replacement semantics (not popping the index), visitIndexAssign still </translation>
    </message>
    <message>
        <source>OpCode</source>
        <translation type="finished">OpCode</translation>
    </message>
    <message>
        <source>OpCode 性能文档：</source>
        <translation type="finished">OpCode performance documentation:</translation>
    </message>
    <message>
        <source>Parser 层限制 import/export 不能在无花括号单语句体中使用。</source>
        <translation type="finished">The Parser layer restricts import/export from being used in braceless single-statement bodies.</translation>
    </message>
    <message>
        <source>Point 类构造 + 字段赋值循环。InstanceData 分配 + GcManager::registerTracked 是主要开销，</source>
        <translation type="finished">Point class construction + field assignment loop. InstanceData allocation + GcManager::registerTracked are the main costs, </translation>
    </message>
    <message>
        <source>Point.new() 构造 + p.distance() 方法调用。</source>
        <translation type="finished">Point.new() construct + p.distance() method call。</translation>
    </message>
    <message>
        <source>QThreadDeleter: Worker 未在 5 秒内退出，回退 terminate()（避免 delete running QThread UB）</source>
        <translation type="finished">QThreadDeleter: Worker not yet in 5 seconds in Exit,fall back terminate()(avoid delete running QThread UB)</translation>
    </message>
    <message>
        <source>REG_RETURN/REG_RETURN_NULL/REG_THROW 通过直接 return 跳过函数末尾统一 stepCallback_ 调用，</source>
        <translation type="finished">REG_RETURN/REG_RETURN_NULL/REG_THROW skip the unified stepCallback_ call at the end of the function via a direct return, </translation>
    </message>
    <message>
        <source>REPL</source>
        <translation type="finished">REPL</translation>
    </message>
    <message>
        <source>REPL / 模块系统</source>
        <translation type="finished">REPL / module system</translation>
    </message>
    <message>
        <source>REPL 仍在执行</source>
        <translation type="finished">REPL is still executing</translation>
    </message>
    <message>
        <source>REPL 异步任务未在 5 秒内响应中止请求，强制 _Exit 退出进程</source>
        <translation type="finished">The REPL async task did not respond to the abort request within 5 seconds; force _Exit to quit the process</translation>
    </message>
    <message>
        <source>REPL 执行异常: %1</source>
        <translation type="finished">REPL execution exception: %1</translation>
    </message>
    <message>
        <source>REPL 执行未知异常</source>
        <translation type="finished">Unknown REPL execution exception</translation>
    </message>
    <message>
        <source>
REPL 有异步任务正在执行。
关闭窗口将发送中止请求并等待最多 5 秒。

是否继续关闭？
        </source>
        <translation type="finished">
An REPL async task is still running.
Closing the window will send an abort request and wait up to 5 seconds.

Continue closing?
        </translation>
    </message>
    <message>
        <source>REPL 析构等待异步任务超时（5s），强制 _Exit 退出进程</source>
        <translation type="finished">REPL destructor timed out waiting for the async task (5s); force _Exit to quit the process</translation>
    </message>
    <message>
        <source>REPL 正在执行，请先停止 REPL 再使用 VM 单步</source>
        <translation type="finished">REPL is executing; please stop it before using VM step-into</translation>
    </message>
    <message>
        <source>REPL 正在执行，请先停止 REPL 再使用 VM 跨出</source>
        <translation type="finished">REPL is executing; please stop it before using VM step-out</translation>
    </message>
    <message>
        <source>REPL 正在执行，请先停止 REPL 再使用 VM 跨过</source>
        <translation type="finished">REPL is executing; please stop it before using VM step-over</translation>
    </message>
    <message>
        <source>REPL 正在执行，请先停止 REPL 再使用 VM 运行</source>
        <translation type="finished">REPL is executing; please stop it before using VM run</translation>
    </message>
    <message>
        <source>REPL 正在执行，请等待其完成后再调试</source>
        <translation type="finished">REPL is executing; please wait for it to finish before debugging</translation>
    </message>
    <message>
        <source>REPL 正在执行，请等待其完成后再运行</source>
        <translation type="finished">REPL is executing; please wait for it to finish before running</translation>
    </message>
    <message>
        <source>
REPL 行为说明:

        </source>
        <translation type="finished">
REPL behavior notes:

        </translation>
    </message>
    <message>
        <source>RegisterVM</source>
        <translation type="finished">RegisterVM</translation>
    </message>
    <message>
        <source>RegisterVM / 调试器</source>
        <translation type="finished">RegisterVM / debugger</translation>
    </message>
    <message>
        <source>RegisterVM 单步：丢失步进事件</source>
        <translation type="finished">RegisterVM single-step: lost step event</translation>
    </message>
    <message>
        <source>RegisterVM 热点 OpCode Top 10：</source>
        <translation type="finished">RegisterVM hot OpCode Top 10:</translation>
    </message>
    <message>
        <source>RegisterVM 调试中</source>
        <translation type="finished">RegisterVM debugging</translation>
    </message>
    <message>
        <source>RegisterVM 通常略快于 StackVM（寄存器消除 push/pop 内存往返），但差距小于 Interpreter vs VM。</source>
        <translation type="finished">RegisterVM is usually slightly faster than StackVM (registers eliminate push/pop memory round-trips), but the gap is smaller than Interpreter vs VM.</translation>
    </message>
    <message>
        <source>RegisterVM 通过虚拟寄存器直接访问，无需 OP_GET_LOCAL 指令（指令密度显著降低）。</source>
        <translation type="finished">RegisterVM accesses virtual registers directly, with no need for OP_GET_LOCAL instructions (instruction density drops significantly).</translation>
    </message>
    <message>
        <source>RegisterVM 通过虚拟寄存器避免 push/pop 内存往返，进一步降低开销。</source>
        <translation type="finished">RegisterVM avoids push/pop memory round-trips through virtual registers, further reducing overhead.</translation>
    </message>
    <message>
        <source>RegisterVM 通过虚拟寄存器避免每次运算 push/pop，进一步降低内存带宽占用。</source>
        <translation type="finished">RegisterVM avoids push/pop on every operation through virtual registers, further reducing memory bandwidth usage.</translation>
    </message>
    <message>
        <source>StackVM</source>
        <translation type="finished">StackVM</translation>
    </message>
    <message>
        <source>StackVM 热点 OpCode Top 10：</source>
        <translation type="finished">StackVM hot OpCode Top 10:</translation>
    </message>
    <message>
        <source>StackVM 调试中</source>
        <translation type="finished">StackVM debugging</translation>
    </message>
    <message>
        <source>Token</source>
        <translation type="finished">Token</translation>
    </message>
    <message>
        <source>UTF-8</source>
        <translation type="finished">UTF-8</translation>
    </message>
    <message>
        <source>Upvalue 对象持有指向栈槽的指针（is_open=true）。</source>
        <translation type="finished">The Upvalue object holds a pointer to a stack slot (is_open=true).</translation>
    </message>
    <message>
        <source>VM RUN 模式正在执行，请先停止 VM 再启动新运行</source>
        <translation type="finished">VM RUN mode is executing; please stop the VM before starting a new run</translation>
    </message>
    <message>
        <source>VM RUN 模式正在执行，请先停止 VM 再重新编译</source>
        <translation type="finished">VM RUN mode is executing; please stop the VM before recompiling</translation>
    </message>
    <message>
        <source>VM 创建 Closure 对象，分配 upvalue 数组。</source>
        <translation type="finished">The VM creates a Closure object and allocates the upvalue array.</translation>
    </message>
    <message>
        <source>VM 单步 (Ctrl+Shift+N)</source>
        <translation type="finished">VM step (Ctrl+Shift+N)</translation>
    </message>
    <message>
        <source>VM 单步发生未知异常</source>
        <translation type="finished">Unknown exception during VM single-step</translation>
    </message>
    <message>
        <source>VM 单步异常: %1</source>
        <translation type="finished">VM single-step exception: %1</translation>
    </message>
    <message>
        <source>VM 条件断点求值异常: </source>
        <translation type="finished">VM conditional-breakpoint evaluation exception: </translation>
    </message>
    <message>
        <source>VM 调用 closeUpvaluesFrom(slot) 关闭所有指向该栈槽及以上的 upvalue。</source>
        <translation type="finished">VM calls closeUpvaluesFrom(slot) to close all upvalues pointing to that stack slot and above.</translation>
    </message>
    <message>
        <source>VM 调用 closeUpvaluesFrom(slot)，将所有指向 slot 及以上的 open upvalue 堆化。</source>
        <translation type="finished">VM calls closeUpvaluesFrom(slot), heapifying all open upvalues pointing to slot and above.</translation>
    </message>
    <message>
        <source>VM 跨出 (Ctrl+Shift+U)</source>
        <translation type="finished">VM step-out (Ctrl+Shift+U)</translation>
    </message>
    <message>
        <source>VM 跨出发生未知异常</source>
        <translation type="finished">Unknown exception during VM step-out</translation>
    </message>
    <message>
        <source>VM 跨出异常: %1</source>
        <translation type="finished">VM Step out Exception: %1</translation>
    </message>
    <message>
        <source>VM 跨过 (Ctrl+Shift+O)</source>
        <translation type="finished">VM Step over (Ctrl+Shift+O)</translation>
    </message>
    <message>
        <source>VM 跨过发生未知异常</source>
        <translation type="finished">VM Step over unknown exception occurs</translation>
    </message>
    <message>
        <source>VM 跨过异常: %1</source>
        <translation type="finished">VM Step over Exception: %1</translation>
    </message>
    <message>
        <source>VM 路径通过编译期模块内联支持 import。</source>
        <translation type="finished">VM path via compile time module inline support import。</translation>
    </message>
    <message>
        <source>VM 运行 (Ctrl+Shift+R)</source>
        <translation type="finished">VM Run (Ctrl+Shift+R)</translation>
    </message>
    <message>
        <source>VM 运行发生未知异常</source>
        <translation type="finished">VM Run unknown exception occurs</translation>
    </message>
    <message>
        <source>VM 运行异常: %1</source>
        <translation type="finished">VM Run Exception: %1</translation>
    </message>
    <message>
        <source>VM停止</source>
        <translation type="finished">VM Stop</translation>
    </message>
    <message>
        <source>VM单步</source>
        <translation type="finished">VM Step</translation>
    </message>
    <message>
        <source>VM跨出</source>
        <translation type="finished">VM Step out</translation>
    </message>
    <message>
        <source>VM跨过</source>
        <translation type="finished">VM Step over</translation>
    </message>
    <message>
        <source>VM运行</source>
        <translation type="finished">VM Run</translation>
    </message>
    <message>
        <source>Value 拷贝：addRef → refCount=2（共享所有权，未深拷贝）</source>
        <translation type="finished">Value copy:addRef → refCount=2(share ownership,not yet deep-copy)</translation>
    </message>
    <message>
        <source>Value 析构：release → refCount=0，delete this</source>
        <translation type="finished">Value destruction:release → refCount=0,delete this</translation>
    </message>
    <message>
        <source>Value 析构：release → refCount=1</source>
        <translation type="finished">Value destruction:release → refCount=1</translation>
    </message>
    <message>
        <source>VarDecl lowering：先求值初始化表达式到 vreg，再 STORE_LOCAL 写入局部槽位。</source>
        <translation type="finished">VarDecl lowering:first evaluate initialize Expression to vreg,then STORE_LOCAL write Local slot。</translation>
    </message>
    <message>
        <source>WhileStmt lowering：L_start 处求值 cond → BRANCH_FALSE 跳到 L_end，</source>
        <translation type="finished">WhileStmt lowering:L_start place evaluate cond → BRANCH_FALSE jump to L_end,</translation>
    </message>
    <message>
        <source>Worker terminate 后 2 秒仍未退出，强制 _Exit</source>
        <translation type="finished">Worker terminate after 2 seconds still not Exit,force _Exit</translation>
    </message>
    <message>
        <source>Worker 未在 5 秒内响应取消请求，回退 terminate() + join（确保 close 路径安全）</source>
        <translation type="finished">Worker not yet in 5 seconds in respond cancel request,fall back terminate() + join(ensure close path safety)</translation>
    </message>
    <message>
        <source>[%1] %2</source>
        <translation type="finished">[%1] %2</translation>
    </message>
    <message>
        <source>[%1] %2 — %3</source>
        <translation type="finished">[%1] %2 — %3</translation>
    </message>
    <message>
        <source>
[Parser 错误]

        </source>
        <translation type="finished">
[Parser Error]

        </translation>
    </message>
    <message>
        <source>[已清除所有模块缓存，下次 import 将重新加载源码]</source>
        <translation type="finished">[ clear all module cache,below times import reload source]</translation>
    </message>
    <message>
        <source>[已清除模块 </source>
        <translation type="finished">[ clear module </translation>
    </message>
    <message>
        <source>[异常] </source>
        <translation type="finished">[Exception] </translation>
    </message>
    <message>
        <source>[提示] Token 数量 %1 超过显示上限 %2，仅显示前 %2 条</source>
        <translation type="finished">[Hint] Token quantity %1 exceed show limit %2,only show before %2 item</translation>
    </message>
    <message>
        <source>[格式化] </source>
        <translation type="finished">[Format] </translation>
    </message>
    <message>
        <source>[格式化] %1: %2</source>
        <translation type="finished">[Format] %1: %2</translation>
    </message>
    <message>
        <source>[格式化] 断点已清除（行号变化，断点不再有效）</source>
        <translation type="finished">[Format] breakpoint cleared(line number change,Breakpoint no longer valid)</translation>
    </message>
    <message>
        <source>[格式化] 格式化异常: %1</source>
        <translation type="finished">[Format] Format Exception: %1</translation>
    </message>
    <message>
        <source>[续行已中止]</source>
        <translation type="finished">[continuation abort]</translation>
    </message>
    <message>
        <source>[运行完成]</source>
        <translation type="finished">[Run complete]</translation>
    </message>
    <message>
        <source>\u95EE\u9898</source>
        <translation type="finished">\u95EE\u9898</translation>
    </message>
    <message>
        <source>
```
var a = [1, 2, 3];
var b = a;          // 共享所有权
b.push(4);          // COW 触发，b 拥有新副本
print(a.len());     // 3
print(b.len());     // 4
```

        </source>
        <translation type="finished">
```
var a = [1, 2, 3];
var b = a; // share ownership
b.push(4); // COW trigger,b own new copy
print(a.len()); // 3
print(b.len()); // 4
```

        </translation>
    </message>
    <message>
        <source>
```
var x = 42; // 行内注释
print(x);
```

        </source>
        <translation type="finished">
```
var x = 42; // inline comment
print(x);
```

        </translation>
    </message>
    <message>
        <source>acc 闭包持有 total 的 upvalue 指针，total 在堆上存活。</source>
        <translation type="finished">acc Closure holds total upvalue pointer,total in on the heap alive。</translation>
    </message>
    <message>
        <source>add 闭包捕获两个 upvalue：base 和 delta。</source>
        <translation type="finished">add Closure capture two upvalue:base and delta。</translation>
    </message>
    <message>
        <source>and/or 短路求值——左操作数决定结果时跳过右操作数求值，返回操作数原值（非布尔）。</source>
        <translation type="finished">and/or short-circuit evaluation——left operand determines the result when skip right operand evaluate,return operand original value(non-boolean)。</translation>
    </message>
    <message>
        <source>array 类型</source>
        <translation type="finished">array Type</translation>
    </message>
    <message>
        <source>b 离开作用域时 release，refCount 降为 1；a 离开时 release，refCount=0 触发析构。</source>
        <translation type="finished">b leave scope when release,refCount decrease is 1;a when leaving release,refCount=0 trigger destruction。</translation>
    </message>
    <message>
        <source>bool 类型</source>
        <translation type="finished">bool Type</translation>
    </message>
    <message>
        <source>bool 类型用 BOOL_TAG_BASE = 0x7FF9 编码，低 48 位存储 0/1。</source>
        <translation type="finished">bool Type use BOOL_TAG_BASE = 0x7FF9 compile code,low 48 bits store 0/1。</translation>
    </message>
    <message>
        <source>c1 和 c2 是两个独立的计数器，各自捕获自己的 count。</source>
        <translation type="finished">c1 and c2 is two independent counter,each capture own count。</translation>
    </message>
    <message>
        <source>catch 变量 e 绑定字典后，可通过 e.code / e.message 访问字段。</source>
        <translation type="finished">catch Variable e bind Dictionary after,can via e.code / e.message access fields。</translation>
    </message>
    <message>
        <source>catch 变量 e 绑定异常对象（字符串值），执行完 catch 块后继续正常流程。</source>
        <translation type="finished">catch Variable e bind the exception object(String Value),after execution catch block after continue normal flow。</translation>
    </message>
    <message>
        <source>catch 变量绑定异常对象后，可通过字段访问或方法调用获取详情。</source>
        <translation type="finished">catch Variable bind the exception object after,can via field access or method call acquire details。</translation>
    </message>
    <message>
        <source>catch 块中可以再次 throw（重新抛出），传播到外层。</source>
        <translation type="finished">catch block in can again throw(re-throw),propagate to outer。</translation>
    </message>
    <message>
        <source>catch 块中可以再次 throw，将异常（或新异常）传播到外层。</source>
        <translation type="finished">catch block in can again throw, Exception(or new Exception)propagate to outer。</translation>
    </message>
    <message>
        <source>catch 块内 throw 会跳过清理代码——为正确处理，catch 块用 OP_TRY_BEGIN 包装，</source>
        <translation type="finished">catch block in throw will skip cleanup code——is correct handle,catch block use OP_TRY_BEGIN wrap,</translation>
    </message>
    <message>
        <source>catch 块执行完毕后，控制流回到 try/catch 之后的代码。</source>
        <translation type="finished">catch block after execution completes,control flow return to code after try/catch。</translation>
    </message>
    <message>
        <source>catch 块执行完毕后，程序恢复正常控制流。</source>
        <translation type="finished">catch block after execution completes,program restore normal control flow。</translation>
    </message>
    <message>
        <source>catch 块捕获并处理。异常对象通过 catch 变量名（如 e）访问。</source>
        <translation type="finished">catch block capture and handle。exception object via catch variable name(e.g. e)access。</translation>
    </message>
    <message>
        <source>catch 块捕获异常，catch 变量绑定异常对象。</source>
        <translation type="finished">catch block capture exception,catch Variable bind the exception object。</translation>
    </message>
    <message>
        <source>clearModuleCache 路径规范化缺失</source>
        <translation type="finished">clearModuleCache missing path normalization</translation>
    </message>
    <message>
        <source>closure 类型</source>
        <translation type="finished">closure Type</translation>
    </message>
    <message>
        <source>dict 类型</source>
        <translation type="finished">dict Type</translation>
    </message>
    <message>
        <source>double 和 triple 各自捕获不同的 factor（2 和 3）。</source>
        <translation type="finished">double and triple each capture different factor(2 and 3)。</translation>
    </message>
    <message>
        <source>fib(20) 场景下 OP_CALL 触发 ~21891 次，是 RegisterVM 相对 StackVM 优势最明显的指令。</source>
        <translation type="finished">fib(20) in the scenario OP_CALL trigger ~21891 times,is RegisterVM relative StackVM the most obvious advantage instruction。</translation>
    </message>
    <message>
        <source>fib(5) 递归展开，每层调用产生一个新栈帧，最大深度等于入参。</source>
        <translation type="finished">fib(5) recursion expand,each layer call produce one new stack frame,maximum depth equal to input parameter。</translation>
    </message>
    <message>
        <source>finally 块无论是否发生异常都会执行。MiniLang 不支持 finally 关键字，</source>
        <translation type="finished">finally block None theory whether exception occurs all will execute。MiniLang does not support finally keyword,</translation>
    </message>
    <message>
        <source>finally 块无论是否发生异常都会执行。常用于资源清理</source>
        <translation type="finished">finally block None theory whether exception occurs all will execute。commonly used for resource cleanup</translation>
    </message>
    <message>
        <source>finally 语义（始终执行）</source>
        <translation type="finished">finally semantics(always execute)</translation>
    </message>
    <message>
        <source>float 3.14 直接存储（IEEE 754 double）</source>
        <translation type="finished">float 3.14 stored directly (IEEE 754 double)</translation>
    </message>
    <message>
        <source>float 类型</source>
        <translation type="finished">float Type</translation>
    </message>
    <message>
        <source>for 循环</source>
        <translation type="finished">for loop</translation>
    </message>
    <message>
        <source>
fun inner() {
    print(&quot;now at inner line 2&quot;);  // 实际暂停行
}
fun outer() {
    inner();  // 调用点行号
}
outer();
        </source>
        <translation type="finished">
fun inner() {
 print(&quot;now at inner line 2&quot;); // actual paused line
}
fun outer() {
 inner(); // call-site line number
}
outer();
        </translation>
    </message>
    <message>
        <source>
fun makeAccumulator() {
    var total = 0;
    fun add(x) {
        total = total + x;
        return total;
    }
    return add;
}

var acc = makeAccumulator();
// makeAccumulator 已返回，total 仍存活
print acc(10);  // 10
print acc(20);  // 30
print acc(5);   // 35
        </source>
        <translation type="finished">
fun makeAccumulator() {
 var total = 0;
 fun add(x) {
 total = total + x;
 return total;
 }
 return add;
}

var acc = makeAccumulator();
// makeAccumulator has returned,total still alive
print acc(10); // 10
print acc(20); // 30
print acc(5); // 35
        </translation>
    </message>
    <message>
        <source>
fun makeCounter() {
    var count = 0;
    fun next() {
        count = count + 1;
        return count;
    }
    return next;
}

var c1 = makeCounter();
var c2 = makeCounter();
print c1();  // 1
print c1();  // 2
print c2();  // 1（独立计数器）
        </source>
        <translation type="finished">
fun makeCounter() {
 var count = 0;
 fun next() {
 count = count + 1;
 return count;
 }
 return next;
}

var c1 = makeCounter();
var c2 = makeCounter();
print c1(); // 1
print c1(); // 2
print c2(); // 1(independent counter)
        </translation>
    </message>
    <message>
        <source>i = 0, 100, 200, 300... 时暂停，共触发 10 次（假设循环 1000 次）</source>
        <translation type="finished">i = 0, 100, 200, 300... pauses when,total trigger 10 times(assume loop 1000 times)</translation>
    </message>
    <message>
        <source>if 语句</source>
        <translation type="finished">if statement</translation>
    </message>
    <message>
        <source>increment 闭包捕获外层变量 x。makeCounter 返回后，</source>
        <translation type="finished">increment Closure capture outer variable x。makeCounter after returning,</translation>
    </message>
    <message>
        <source>inner 调用时通过 upvalue 链访问堆上的 a 和 b。</source>
        <translation type="finished">inner call via upvalue chain access on the heap a and b。</translation>
    </message>
    <message>
        <source>inner 闭包捕获两个 upvalue：a 来自 outer，b 来自 middle。</source>
        <translation type="finished">inner Closure capture two upvalue:a from outer,b from middle。</translation>
    </message>
    <message>
        <source>input() 超时（主线程 30 秒未响应）</source>
        <translation type="finished">input() timeout(main thread 30 seconds not responding)</translation>
    </message>
    <message>
        <source>input() 超时（主线程 30 秒未响应），抛出 RuntimeError</source>
        <translation type="finished">input() timeout(main thread 30 seconds not responding),throw RuntimeError</translation>
    </message>
    <message>
        <source>instance 类型</source>
        <translation type="finished">instance Type</translation>
    </message>
    <message>
        <source>int -1 内联（int48 负值）</source>
        <translation type="finished">int -1 inlined (int48 negative value)</translation>
    </message>
    <message>
        <source>int 42 内联（int48 范围内）</source>
        <translation type="finished">int 42 inline(int48 within range)</translation>
    </message>
    <message>
        <source>int 类型</source>
        <translation type="finished">int Type</translation>
    </message>
    <message>
        <source>int 边界值</source>
        <translation type="finished">int boundary Value</translation>
    </message>
    <message>
        <source>int(0) 与 float(0.0) 因数值相等被误判为同一常量共享索引。</source>
        <translation type="finished">int(0) and float(0.0) because numeric equality misjudge is same constant shares index。</translation>
    </message>
    <message>
        <source>int48 最大值 2^46-1 = 70368744177663（约 7×10^13）。超出此范围会触发装箱为 BoxedIntData*。</source>
        <translation type="finished">int48 maximum value 2^46-1 = 70368744177663(about 7×10^13)。exceeds this range will trigger boxing is BoxedIntData*。</translation>
    </message>
    <message>
        <source>int48 最大正值（2^46 - 1）</source>
        <translation type="finished">int48 maximum positive(2^46 - 1)</translation>
    </message>
    <message>
        <source>int48 范围上限：|v| &lt; 2^47。</source>
        <translation type="finished">int48 range limit:|v| &lt; 2^47。</translation>
    </message>
    <message>
        <source>isEven 与 isOdd 互相调用直到 n=0/1，栈呈交替增长。</source>
        <translation type="finished">isEven and isOdd mutual call directly to n=0/1,Stack alternates growth。</translation>
    </message>
    <message>
        <source>isFunOrClass lambda 未解包 ExportStmt 内部声明，导致 export var x 与 export fun f 之间无空行，</source>
        <translation type="finished">isFunOrClass lambda not unwrapped ExportStmt internal declaration,causes export var x and export fun f between None blank line,</translation>
    </message>
    <message>
        <source>makeAccumulator 返回后，total 的栈帧弹出，total 堆化。</source>
        <translation type="finished">makeAccumulator after returning,total stack frame pop,total heapify。</translation>
    </message>
    <message>
        <source>makeCounter 每次调用创建新的栈帧，新的 count 变量。</source>
        <translation type="finished">makeCounter each time call create new stack frame,new count Variable。</translation>
    </message>
    <message>
        <source>makeCounter 返回闭包，闭包帧捕获外层 count 变量。</source>
        <translation type="finished">makeCounter returns a closure,closure frame capture outer count Variable。</translation>
    </message>
    <message>
        <source>makeCounter 闭包捕获循环。ClosureData 通过 weak_ptr&lt;Environment&gt; 打破循环，</source>
        <translation type="finished">makeCounter Closure capture loop。ClosureData via weak_ptr&lt;Environment&gt; break loop,</translation>
    </message>
    <message>
        <source>makeMultiplier 返回一个匿名闭包，捕获 factor。</source>
        <translation type="finished">makeMultiplier return an anonymous Closure,capture factor。</translation>
    </message>
    <message>
        <source>makeMultiplier 返回后，factor 的栈帧被弹出，factor 堆化。</source>
        <translation type="finished">makeMultiplier after returning,factor Stack frame is popped,factor heapify。</translation>
    </message>
    <message>
        <source>markValue 检查 Value 类型：ArrayData → 遍历 elements；DictData → 遍历 entries；</source>
        <translation type="finished">markValue check Value Type:ArrayData → traverse elements;DictData → traverse entries;</translation>
    </message>
    <message>
        <source>middle 闭包本身也捕获了 a。inner 的 upvalue a 指向 middle 的 upvalue a，</source>
        <translation type="finished">middle Closure itself also capture a。inner upvalue a points to middle upvalue a,</translation>
    </message>
    <message>
        <source>normalizeModulePath 仅检测 &#x27;C:/&#x27; 形式未拒绝 &#x27;C:foo&#x27;（Windows 驱动器相对路径），</source>
        <translation type="finished">normalizeModulePath only detect &#x27;C:/&#x27; form not reject &#x27;C:foo&#x27;(Windows drive relative path),</translation>
    </message>
    <message>
        <source>null 值编码为固定常量 NULL_BITS = 0x7FFA000000000000，</source>
        <translation type="finished">null Value compile code is fixed constant NULL_BITS = 0x7FFA000000000000,</translation>
    </message>
    <message>
        <source>null 兼容所有类型，float 注解接受 int 值（宽化），int 注解拒绝 float 值。</source>
        <translation type="finished">null compatible all Type,float annotation accepts int Value(widening),int annotation reject float Value。</translation>
    </message>
    <message>
        <source>null 检查条件断点。常用于排查变量未初始化导致的运行时错误。</source>
        <translation type="finished">null check the conditional breakpoint。commonly used for investigate Variable uninitialized causes runtime error。</translation>
    </message>
    <message>
        <source>null 检查条件（x == null）</source>
        <translation type="finished">null check condition(x == null)</translation>
    </message>
    <message>
        <source>null 类型</source>
        <translation type="finished">null Type</translation>
    </message>
    <message>
        <source>p.x 字段持有 ArrayData（refCount=1）</source>
        <translation type="finished">p.x Field holds ArrayData(refCount=1)</translation>
    </message>
    <message>
        <source>patchJumps 解析跳转目标。</source>
        <translation type="finished">patchJumps parse jump goal。</translation>
    </message>
    <message>
        <source>payload 全为 0。</source>
        <translation type="finished">payload all are 0。</translation>
    </message>
    <message>
        <source>refCount</source>
        <translation type="finished">refCount</translation>
    </message>
    <message>
        <source>slot 0 预留给隐式 this 参数。MethodCall 通过 GET_FIELD/SET_FIELD 访问实例字段。</source>
        <translation type="finished">slot 0 reserve give implicit this argument。MethodCall via GET_FIELD/SET_FIELD access instance fields。</translation>
    </message>
    <message>
        <source>string &quot;hello&quot; 装箱（PTR_TAG_BASE + 48 位指针）</source>
        <translation type="finished">string &quot;hello&quot; boxing (PTR_TAG_BASE + 48-bit pointer)</translation>
    </message>
    <message>
        <source>string 类型</source>
        <translation type="finished">string Type</translation>
    </message>
    <message>
        <source>throw 语句创建异常对象（可以是任意值：字符串、数字、字典、实例），</source>
        <translation type="finished">throw Statement create exception object(can is any value:String,number,Dictionary,instance),</translation>
    </message>
    <message>
        <source>total 在 acc 不可达时才会被 GC 回收。</source>
        <translation type="finished">total is only reclaimed by GC when acc becomes unreachable.</translation>
    </message>
    <message>
        <source>tracked 节点数：%1</source>
        <translation type="finished">tracked node count: %1</translation>
    </message>
    <message>
        <source>tracked 节点数：—</source>
        <translation type="finished">tracked node count: —</translation>
    </message>
    <message>
        <source>tracked_ 列表中的 RefCounted* 可能在 collectCycle 期间被析构（如 sweep 清空子元素后 refCount→0）。</source>
        <translation type="finished">RefCounted* in the tracked_ list may be destructed during collectCycle (e.g., after sweep clears child elements, refCount→0).</translation>
    </message>
    <message>
        <source>true 编码为 0x7FF9...0001，false 为 0x7FF9...0000。</source>
        <translation type="finished">true is encoded as 0x7FF9...0001, false as 0x7FF9...0000.</translation>
    </message>
    <message>
        <source>try 块内 throw 后栈迅速回退到 catch 帧。</source>
        <translation type="finished">try block in throw after Stack quickly fall back to catch frame。</translation>
    </message>
    <message>
        <source>try/catch 之后的代码会正常执行（除非再次抛出异常）。</source>
        <translation type="finished">code after try/catch will execute normally(unless again throw exception)。</translation>
    </message>
    <message>
        <source>try/catch 之后的代码继续执行。</source>
        <translation type="finished">code after try/catch continue execution。</translation>
    </message>
    <message>
        <source>try/catch 异常处理</source>
        <translation type="finished">try/catch exception handling</translation>
    </message>
    <message>
        <source>upvalue 位置可以是&quot;栈槽&quot;（直接外层变量）或&quot;上层 upvalue&quot;（嵌套闭包）。</source>
        <translation type="finished">upvalue location can is&quot;stack slot&quot;(directly outer variable)or&quot;on layer upvalue&quot;(nested closure)。</translation>
    </message>
    <message>
        <source>upvalue 生命周期</source>
        <translation type="finished">upvalue lifetime</translation>
    </message>
    <message>
        <source>validate() 的局部变量（如 x）在栈展开时被销毁。</source>
        <translation type="finished">validate() local variable(e.g. x)in stack unwinding when destroy。</translation>
    </message>
    <message>
        <source>var b = a 共享所有权（refCount=2），不进行深拷贝（COW 语义）。</source>
        <translation type="finished">var b = a shares ownership (refCount=2) without deep-copying (COW semantics).</translation>
    </message>
    <message>
        <source>
var fns = [];
var i = 0;
while (i &lt; 3) {
    fns.push(fun() {
        return i;
    });
    i = i + 1;
}
print fns[0]();  // 3（不是 0！）
print fns[1]();  // 3
print fns[2]();  // 3
        </source>
        <translation type="finished">
var fns = [];
var i = 0;
while (i &lt; 3) {
 fns.push(fun() {
 return i;
 });
 i = i + 1;
}
print fns[0](); // 3(is not 0!)
print fns[1](); // 3
print fns[2](); // 3
        </translation>
    </message>
    <message>
        <source>var s2 = s1 共享所有权，s2 += &#x27;x&#x27; 实际是构造新 StringData 赋值给 s2。</source>
        <translation type="finished">var s2 = s1 shares ownership; s2 += &#x27;x&#x27; actually constructs a new StringData and assigns it to s2.</translation>
    </message>
    <message>
        <source>
var x = 42; // 行内注释
print(x);
        </source>
        <translation type="finished">
var x = 42; // inline comment
print(x);
        </translation>
    </message>
    <message>
        <source>
var x = 5;
print(x &gt; 0 and &quot;positive&quot; or &quot;non-positive&quot;);
print(7 / 2);    // 3
print(7.0 / 2);  // 3.5
print(0 or &quot;default&quot;);  // 0（保留原值）
        </source>
        <translation type="finished">
var x = 5;
print(x &gt; 0 and &quot;positive&quot; or &quot;non-positive&quot;);
print(7 / 2); // 3
print(7.0 / 2); // 3.5
print(0 or &quot;default&quot;); // 0(preserve original value)
        </translation>
    </message>
    <message>
        <source>var 声明</source>
        <translation type="finished">var declaration</translation>
    </message>
    <message>
        <source>visitImportStmt 将 \\ 转 / 并去除 ./ 前缀后存入 moduleCache_，clearModuleCache 直接用原始 path 查 erase 无法命中。</source>
        <translation type="finished">visitImportStmt \\ convert / and strip./ prefix after store into moduleCache_,clearModuleCache directly use original path lookup erase cannot hit。</translation>
    </message>
    <message>
        <source>while 循环</source>
        <translation type="finished">while loop</translation>
    </message>
    <message>
        <source>x 的栈帧被弹出，但 x 作为 upvalue 被堆化（迁移到堆）。</source>
        <translation type="finished">x Stack frame is popped,but x as upvalue heapify(migrate to heap)。</translation>
    </message>
    <message>
        <source>x86-64 用户态虚拟地址空间为 48 位，可直接编码。</source>
        <translation type="finished">The x86-64 user-space virtual address space is 48 bits and can be encoded directly.</translation>
    </message>
    <message>
        <source>×</source>
        <translation type="finished">×</translation>
    </message>
    <message>
        <source>—</source>
        <translation type="finished">—</translation>
    </message>
    <message>
        <source>→</source>
        <translation type="finished">→</translation>
    </message>
    <message>
        <source>● 未保存</source>
        <translation type="finished">● unsaved</translation>
    </message>
    <message>
        <source>三后端在闭包捕获上开销相近，RegisterVM 因寄存器分配稍占优势。</source>
        <translation type="finished">The three backends have similar overhead for closure capture; RegisterVM has a slight edge due to register allocation.</translation>
    </message>
    <message>
        <source>三后端对比</source>
        <translation type="finished">three-backend comparison</translation>
    </message>
    <message>
        <source>三后端差异 %1 行 / 共 %2 行 — 一致性 FAIL</source>
        <translation type="finished">Three-backend differences %1 lines / %2 lines total — consistency FAIL</translation>
    </message>
    <message>
        <source>三后端差距较小（解释器仍稍慢，因为 MethodCall 的 Visitor 分发）。</source>
        <translation type="finished">The three-backend gap is small (the interpreter is still slightly slower due to MethodCall Visitor dispatch).</translation>
    </message>
    <message>
        <source>三后端平均时间对比 (μs)</source>
        <translation type="finished">Three-backend average time comparison (μs)</translation>
    </message>
    <message>
        <source>三后端平均时间（微秒）：</source>
        <translation type="finished">Three-backend average time (microseconds):</translation>
    </message>
    <message>
        <source>三后端性能相近（瓶颈在 hash 而非指令分发），Interpreter 略慢。</source>
        <translation type="finished">The three backends perform similarly (the bottleneck is hashing rather than instruction dispatch); Interpreter is slightly slower.</translation>
    </message>
    <message>
        <source>三后端输出一致（%1 行）— 一致性 PASS</source>
        <translation type="finished">Three-backend output is consistent (%1 lines) — consistency PASS</translation>
    </message>
    <message>
        <source>三层嵌套闭包：自由变量捕获断裂</source>
        <translation type="finished">Three-layer nested closure: free-variable capture broken</translation>
    </message>
    <message>
        <source>上一个</source>
        <translation type="finished">Previous</translation>
    </message>
    <message>
        <source>上一次执行尚未完成，请稍候...</source>
        <translation type="finished">The previous execution has not finished, please wait...</translation>
    </message>
    <message>
        <source>下一个</source>
        <translation type="finished">Next</translation>
    </message>
    <message>
        <source>下一提示</source>
        <translation type="finished">Next hint</translation>
    </message>
    <message>
        <source>与 OP_GET_LOCAL 配对出现，是 StackVM 指令密度的典型代表。</source>
        <translation type="finished">Paired with OP_GET_LOCAL, this is a typical representative of StackVM instruction density.</translation>
    </message>
    <message>
        <source>与未导出形式（var x 紧跟 fun f）的格式化结果不一致。</source>
        <translation type="finished">Inconsistent with the formatting result of the non-exported form (var x followed by fun f).</translation>
    </message>
    <message>
        <source>两个闭包的 upvalue 指向不同的堆地址，互不干扰。</source>
        <translation type="finished">The two closures' upvalues point to different heap addresses and do not interfere with each other.</translation>
    </message>
    <message>
        <source>中间IR</source>
        <translation type="finished">Intermediate IR</translation>
    </message>
    <message>
        <source>中间层又捕获外层变量。upvalue 可以指向另一层闭包的 upvalue</source>
        <translation type="finished">The middle layer also captures outer variables. An upvalue can point to another closure's upvalue</translation>
    </message>
    <message>
        <source>二元加法 a + b</source>
        <translation type="finished">Binary addition a + b</translation>
    </message>
    <message>
        <source>从 roots（globals / VM 栈 / 调用帧中的 Value）出发，递归 mark 所有可达容器节点。</source>
        <translation type="finished">Starting from roots (globals / VM stack / Values in call frames), recursively mark all reachable container nodes.</translation>
    </message>
    <message>
        <source>从常量池读取字符串并压入栈顶。</source>
        <translation type="finished">Read a string from the constant pool and push it onto the stack top.</translation>
    </message>
    <message>
        <source>从常量池读取整数并压入栈顶。</source>
        <translation type="finished">Read an integer from the constant pool and push it onto the stack top.</translation>
    </message>
    <message>
        <source>从常量池读取浮点数并压入栈顶。</source>
        <translation type="finished">Read a float from the constant pool and push it onto the stack top.</translation>
    </message>
    <message>
        <source>从当前函数返回，弹出整个调用帧，将返回值压入调用者栈顶。</source>
        <translation type="finished">from current function return,pop entire call frame, push return value caller stack top。</translation>
    </message>
    <message>
        <source>代码：</source>
        <translation type="finished">code:</translation>
    </message>
    <message>
        <source>优化 pass 对比</source>
        <translation type="finished">optimization pass comparison</translation>
    </message>
    <message>
        <source>优化前 IR：</source>
        <translation type="finished">optimization before IR:</translation>
    </message>
    <message>
        <source>优化后 IR：</source>
        <translation type="finished">optimization after IR:</translation>
    </message>
    <message>
        <source>会被规范化为 NAN_BOXED_FLOAT_MARKER（罕见）。</source>
        <translation type="finished">will normalization is NAN_BOXED_FLOAT_MARKER(rare)。</translation>
    </message>
    <message>
        <source>传播图解</source>
        <translation type="finished">propagation diagram</translation>
    </message>
    <message>
        <source>但仅当 a 的根引用被丢弃时才会被回收。若 a 仍在 globals 中，GC 不会触碰。</source>
        <translation type="finished">but only when a root reference discard when only then will Collection。if a still in globals in,GC will not touch。</translation>
    </message>
    <message>
        <source>但可通过 catch + 显式清理模拟类似语义。</source>
        <translation type="finished">but can via catch + explicitly clean simulate similar semantics。</translation>
    </message>
    <message>
        <source>但可通过 catch 块中的显式清理代码模拟。</source>
        <translation type="finished">but can via catch block in explicitly clean code simulation。</translation>
    </message>
    <message>
        <source>位模式（64 位，高 16 位为 tag）：</source>
        <translation type="finished">Bit pattern (64 bits, high 16 bits are the tag):</translation>
    </message>
    <message>
        <source>使闭包捕获不同的变量值。</source>
        <translation type="finished">Closure capture different Variable Value。</translation>
    </message>
    <message>
        <source>保存(&amp;S)</source>
        <translation type="finished">Save (&amp;S)</translation>
    </message>
    <message>
        <source>保存文件</source>
        <translation type="finished">Save file</translation>
    </message>
    <message>
        <source>修复后：ExportStmt 之间插入空行（与未导出形式一致）。</source>
        <translation type="finished">After the fix:ExportStmt between insert blank line(and not yet Export form consistent)。</translation>
    </message>
    <message>
        <source>修复后：Parser 层拒绝复杂表达式默认值，仅允许字面量 + 负数字面量。</source>
        <translation type="finished">After the fix:Parser layer reject complex expression default value,only allows literal + negative literal。</translation>
    </message>
    <message>
        <source>修复后：a=3（int 截断向零），b=3.5（float 精确）。</source>
        <translation type="finished">After the fix:a=3(int truncates toward zero),b=3.5(float precise)。</translation>
    </message>
    <message>
        <source>修复后：reload 命令正确清缓存，下次 import 重新加载源码。</source>
        <translation type="finished">After the fix:reload command correct clear cache,below times import reload source。</translation>
    </message>
    <message>
        <source>修复后：单步调试时每个 RETURN 指令触发步进回调。</source>
        <translation type="finished">After the fix:single-step debugging when each RETURN instruction trigger the step callback。</translation>
    </message>
    <message>
        <source>修复后：循环正常执行，arr[0][1] == 599。</source>
        <translation type="finished">After the fix:the loop executes normally,arr[0][1] == 599。</translation>
    </message>
    <message>
        <source>修复后：循环正常执行，约 499 次打印 hi。</source>
        <translation type="finished">After the fix:the loop executes normally,about 499 times print hi。</translation>
    </message>
    <message>
        <source>修复后：拒绝所有 &#x27;X:&#x27; 开头形式。</source>
        <translation type="finished">After the fix:reject all &#x27;X:&#x27; opening form。</translation>
    </message>
    <message>
        <source>修复后：调用栈顶帧行号显示当前执行行号。</source>
        <translation type="finished">After the fix:top of call stack frame line number show the currently executing line number。</translation>
    </message>
    <message>
        <source>修复后：输出 1。</source>
        <translation type="finished">After the fix:Output 1。</translation>
    </message>
    <message>
        <source>修复点：compiler/Bytecode.h addConstant() + compiler/RegisterBytecode.cpp RegBytecodeChunk::addConstant()。</source>
        <translation type="finished">Fix point:compiler/Bytecode.h addConstant() + compiler/RegisterBytecode.cpp RegBytecodeChunk::addConstant()。</translation>
    </message>
    <message>
        <source>修复点：compiler/Compiler.cpp normalizeModulePath() + InterpreterModules.cpp + IR.cpp 三处同步。</source>
        <translation type="finished">Fix point:compiler/Compiler.cpp normalizeModulePath() + InterpreterModules.cpp + IR.cpp three places sync。</translation>
    </message>
    <message>
        <source>修复点：compiler/Compiler.cpp resolveUpvalue() + visitFunDecl()。</source>
        <translation type="finished">Fix point:compiler/Compiler.cpp resolveUpvalue() + visitFunDecl()。</translation>
    </message>
    <message>
        <source>修复点：compiler/IR.cpp visitIfStmt() + visitWhileStmt() + visitForStmt()。</source>
        <translation type="finished">Fix point:compiler/IR.cpp visitIfStmt() + visitWhileStmt() + visitForStmt()。</translation>
    </message>
    <message>
        <source>修复点：compiler/RegisterVM.cpp executeCalls() + executeMisc()。</source>
        <translation type="finished">Fix point:compiler/RegisterVM.cpp executeCalls() + executeMisc()。</translation>
    </message>
    <message>
        <source>修复点：formatter/Formatter.cpp isFunOrClass lambda。</source>
        <translation type="finished">Fix point:formatter/Formatter.cpp isFunOrClass lambda。</translation>
    </message>
    <message>
        <source>修复点：interpreter/Interpreter.cpp checkBreak()。</source>
        <translation type="finished">Fix point:interpreter/Interpreter.cpp checkBreak()。</translation>
    </message>
    <message>
        <source>修复点：interpreter/Interpreter.h clearModuleCache()。</source>
        <translation type="finished">Fix point:interpreter/Interpreter.h clearModuleCache()。</translation>
    </message>
    <message>
        <source>修复点：parser/Parser.cpp isLiteralDefaultExpr + 三后端同步。</source>
        <translation type="finished">Fix point:parser/Parser.cpp isLiteralDefaultExpr + three backends sync。</translation>
    </message>
    <message>
        <source>修改条件: &quot;%1&quot;</source>
        <translation type="finished">Modify condition: &quot;%1&quot;</translation>
    </message>
    <message>
        <source>值</source>
        <translation type="finished">Value</translation>
    </message>
    <message>
        <source>停止</source>
        <translation type="finished">Stop</translation>
    </message>
    <message>
        <source>停止 VM</source>
        <translation type="finished">Stop VM</translation>
    </message>
    <message>
        <source>停止运行</source>
        <translation type="finished">Stop running</translation>
    </message>
    <message>
        <source>停止运行 (Shift+F5)</source>
        <translation type="finished">Stop running (Shift+F5)</translation>
    </message>
    <message>
        <source>先比较 getType() 再调用 equals()，否则 int(0) 与 float(0.0) 会被误判为同一常量。</source>
        <translation type="finished">first compare getType() then call equals(),whether then int(0) and float(0.0) will misjudge is same constant。</translation>
    </message>
    <message>
        <source>全字匹配</source>
        <translation type="finished">Whole word match</translation>
    </message>
    <message>
        <source>全局</source>
        <translation type="finished">Global</translation>
    </message>
    <message>
        <source>全局作用域</source>
        <translation type="finished">Global scope</translation>
    </message>
    <message>
        <source>全局变量</source>
        <translation type="finished">Global variable</translation>
    </message>
    <message>
        <source>全部替换</source>
        <translation type="finished">Replace all</translation>
    </message>
    <message>
        <source>共 %1 个断点 | %2</source>
        <translation type="finished">%1 breakpoints total | %2</translation>
    </message>
    <message>
        <source>共享所有权，refCount=2</source>
        <translation type="finished">Shared ownership, refCount=2</translation>
    </message>
    <message>
        <source>关闭</source>
        <translation type="finished">Close</translation>
    </message>
    <message>
        <source>关闭 (Esc)</source>
        <translation type="finished">Close (Esc)</translation>
    </message>
    <message>
        <source>关闭全部</source>
        <translation type="finished">Close all</translation>
    </message>
    <message>
        <source>关闭其他</source>
        <translation type="finished">Close others</translation>
    </message>
    <message>
        <source>关闭查找面板</source>
        <translation type="finished">Close find panel</translation>
    </message>
    <message>
        <source>关闭的 upvalue 将值从栈复制到堆，Upvalue 对象改为指向堆地址（is_open=false）。</source>
        <translation type="finished">A closed upvalue copies its value from the stack to the heap, and the Upvalue object points to the heap address (is_open=false).</translation>
    </message>
    <message>
        <source>关闭面板</source>
        <translation type="finished">Close panel</translation>
    </message>
    <message>
        <source>内存模型</source>
        <translation type="finished">Memory model</translation>
    </message>
    <message>
        <source>内层 catch 捕获异常后打印日志，然后重新 throw。</source>
        <translation type="finished">The inner catch captures the exception, prints a log, then re-throws.</translation>
    </message>
    <message>
        <source>内层 try/catch 结束。控制流回到外层 try 块继续执行。</source>
        <translation type="finished">The inner try/catch ends. Control flow returns to the outer try block and continues.</translation>
    </message>
    <message>
        <source>内部错误：前端管线返回成功但 AST 为空</source>
        <translation type="finished">Internal error: the front-end pipeline returned success but the AST is empty</translation>
    </message>
    <message>
        <source>内部闭包捕获 captured（而非 i），因此每个闭包引用不同的 captured。</source>
        <translation type="finished">The inner closure captures captured (rather than i), so each closure references a different captured.</translation>
    </message>
    <message>
        <source>再深拷贝出新的 ArrayData（refCount=1），最后写入新对象。</source>
        <translation type="finished">Then deep-copy a new ArrayData (refCount=1) and finally write to the new object.</translation>
    </message>
    <message>
        <source>写入局部变量槽位。在循环体内频繁触发（如 i = i + 1）。</source>
        <translation type="finished">Write to local variable slot. Frequently triggered in loop bodies (e.g., i = i + 1).</translation>
    </message>
    <message>
        <source>函数: %1 @ 行 %2 (深度: %3)</source>
        <translation type="finished">Function: %1 @ Line %2 (Depth: %3)</translation>
    </message>
    <message>
        <source>函数声明</source>
        <translation type="finished">function declaration</translation>
    </message>
    <message>
        <source>函数声明。支持默认参数（仅字面量 + 负数字面量，复杂表达式被 Parser 拒绝）。</source>
        <translation type="finished">function declaration。support default parameter(only literal + negative literal,complex expression Parser reject)。</translation>
    </message>
    <message>
        <source>函数无提升——f(); fun f() {} 会报&quot;未定义的函数&quot;。</source>
        <translation type="finished">functions are not hoisted——f(); fun f() {} reports&quot;undefined Function&quot;。</translation>
    </message>
    <message>
        <source>函数调用 f(a, b)</source>
        <translation type="finished">function call f(a, b)</translation>
    </message>
    <message>
        <source>函数调用。开销最大——涉及帧栈分配、参数传递、返回地址保存。</source>
        <translation type="finished">Function call. The most expensive — involves frame-stack allocation, argument passing, and return-address saving.</translation>
    </message>
    <message>
        <source>函数返回。与 OP_CALL 配对，开销同样较大（帧栈回收、返回值传递）。</source>
        <translation type="finished">Function return. Paired with OP_CALL, also relatively expensive (frame-stack reclamation, return-value passing).</translation>
    </message>
    <message>
        <source>分支预测失败的代价高于指令本身，但 MiniLang VM 无分支预测（解释执行）。</source>
        <translation type="finished">The cost of a branch-prediction miss is higher than the instruction itself, but the MiniLang VM has no branch prediction (interpreted execution).</translation>
    </message>
    <message>
        <source>切换执行引擎</source>
        <translation type="finished">switch execution engine</translation>
    </message>
    <message>
        <source>列</source>
        <translation type="finished">Column</translation>
    </message>
    <message>
        <source>列 %1</source>
        <translation type="finished">Column %1</translation>
    </message>
    <message>
        <source>列 1</source>
        <translation type="finished">Column 1</translation>
    </message>
    <message>
        <source>列号</source>
        <translation type="finished">column number</translation>
    </message>
    <message>
        <source>创建闭包值：从栈顶弹出 N 个 upvalue（每个为 isLocal+index 编码）+ 函数名，构造 ClosureData。</source>
        <translation type="finished">create Closure Value:from stack top pop N upvalue(each is isLocal+index compile code)+ Function name,construct ClosureData。</translation>
    </message>
    <message>
        <source>删除</source>
        <translation type="finished">Delete</translation>
    </message>
    <message>
        <source>删除失败</source>
        <translation type="finished">Delete fail</translation>
    </message>
    <message>
        <source>利用 and 短路特性。当 x 为 truthy 时才会求值 y &gt; 0。</source>
        <translation type="finished">utilize and short-circuit characteristic。when x is truthy when only then will evaluate y &gt; 0。</translation>
    </message>
    <message>
        <source>刷新统计</source>
        <translation type="finished">Refresh statistics</translation>
    </message>
    <message>
        <source>剖析完成</source>
        <translation type="finished">profile complete</translation>
    </message>
    <message>
        <source>加入 tracked_ 列表与 aliveSet_。StringData/ClosureData 不注册（无循环引用风险）。</source>
        <translation type="finished">add to tracked_ Column table and aliveSet_。StringData/ClosureData not register (None reference cycle risk)。</translation>
    </message>
    <message>
        <source>加载到主编辑器</source>
        <translation type="finished">load to main editor</translation>
    </message>
    <message>
        <source>加载章节示例到主编辑器</source>
        <translation type="finished">load chapter Example to main editor</translation>
    </message>
    <message>
        <source>动作</source>
        <translation type="finished">action</translation>
    </message>
    <message>
        <source>动态声明变量，或使用类型注解强制类型。三后端统一强制类型注解，</source>
        <translation type="finished">dynamically declare Variable,or use type annotation force type。three backends unified force type annotation,</translation>
    </message>
    <message>
        <source>匹配 %1 处</source>
        <translation type="finished">match %1 place</translation>
    </message>
    <message>
        <source>匹配 %1+ 处</source>
        <translation type="finished">match %1+ place</translation>
    </message>
    <message>
        <source>区分大小写</source>
        <translation type="finished">case-sensitive</translation>
    </message>
    <message>
        <source>单步跳出</source>
        <translation type="finished">step out</translation>
    </message>
    <message>
        <source>单步跳出 (Shift+F11)</source>
        <translation type="finished">step out (Shift+F11)</translation>
    </message>
    <message>
        <source>单步跳出发生未知异常</source>
        <translation type="finished">step out unknown exception occurs</translation>
    </message>
    <message>
        <source>单步跳出失败: %1</source>
        <translation type="finished">step out fail: %1</translation>
    </message>
    <message>
        <source>单步跳过</source>
        <translation type="finished">step over</translation>
    </message>
    <message>
        <source>单步跳过 (F10)</source>
        <translation type="finished">step over (F10)</translation>
    </message>
    <message>
        <source>单步跳过发生未知异常</source>
        <translation type="finished">step over unknown exception occurs</translation>
    </message>
    <message>
        <source>单步跳过失败: %1</source>
        <translation type="finished">step over fail: %1</translation>
    </message>
    <message>
        <source>单步进入</source>
        <translation type="finished">step into</translation>
    </message>
    <message>
        <source>单步进入 (F11)</source>
        <translation type="finished">step into (F11)</translation>
    </message>
    <message>
        <source>单步进入发生未知异常</source>
        <translation type="finished">step into unknown exception occurs</translation>
    </message>
    <message>
        <source>单步进入失败: %1</source>
        <translation type="finished">step into fail: %1</translation>
    </message>
    <message>
        <source>占比</source>
        <translation type="finished">proportion</translation>
    </message>
    <message>
        <source>即使外层函数返回后，闭包仍可访问 x。</source>
        <translation type="finished">even if outer function return after,Closure can still be accessed x。</translation>
    </message>
    <message>
        <source>压入 null 值。</source>
        <translation type="finished">push null Value。</translation>
    </message>
    <message>
        <source>反斜杠在更早处已统一转为正斜杠使原反斜杠分支成为死代码。</source>
        <translation type="finished">backslash in earlier place unified convert to forward slash original backslash branch becomes is dead code。</translation>
    </message>
    <message>
        <source>取模触发条件（i % 100 == 0）</source>
        <translation type="finished">modulo trigger condition(i % 100 == 0)</translation>
    </message>
    <message>
        <source>变量名</source>
        <translation type="finished">variable name</translation>
    </message>
    <message>
        <source>变量声明 var x = expr</source>
        <translation type="finished">variable declaration var x = expr</translation>
    </message>
    <message>
        <source>变量快照回调发生未知异常</source>
        <translation type="finished">variable snapshot callback unknown exception occurs</translation>
    </message>
    <message>
        <source>变量快照回调异常: </source>
        <translation type="finished">variable snapshot callback Exception: </translation>
    </message>
    <message>
        <source>变量检查器</source>
        <translation type="finished">Variable inspector</translation>
    </message>
    <message>
        <source>变量监视</source>
        <translation type="finished">variable watch</translation>
    </message>
    <message>
        <source>另存为(&amp;A)...</source>
        <translation type="finished">save as(&amp;A)...</translation>
    </message>
    <message>
        <source>名称</source>
        <translation type="finished">name</translation>
    </message>
    <message>
        <source>名称不得包含路径分隔符或父目录引用</source>
        <translation type="finished">name not get contains path separator or parent directory reference</translation>
    </message>
    <message>
        <source>后端</source>
        <translation type="finished">backend</translation>
    </message>
    <message>
        <source>启动发生未知异常</source>
        <translation type="finished">Start unknown exception occurs</translation>
    </message>
    <message>
        <source>启动失败: %1</source>
        <translation type="finished">Start fail: %1</translation>
    </message>
    <message>
        <source>启动程序运行</source>
        <translation type="finished">start program run</translation>
    </message>
    <message>
        <source>启动调试发生未知异常</source>
        <translation type="finished">start debug unknown exception occurs</translation>
    </message>
    <message>
        <source>启动调试失败: %1</source>
        <translation type="finished">start debug fail: %1</translation>
    </message>
    <message>
        <source>启动调试运行</source>
        <translation type="finished">start debug Run</translation>
    </message>
    <message>
        <source>命中次数</source>
        <translation type="finished">hit count</translation>
    </message>
    <message>
        <source>回跳到循环入口（负偏移）。</source>
        <translation type="finished">jump back to loop entry(negative offset)。</translation>
    </message>
    <message>
        <source>因此所有闭包引用的 upvalue 指向同一地址。</source>
        <translation type="finished">therefore all Closure reference upvalue points to same address。</translation>
    </message>
    <message>
        <source>因此所有闭包调用都返回 3。这是闭包捕获循环变量的经典陷阱。</source>
        <translation type="finished">therefore all Closure call all return 3。this is Closure capture loop variable classic pitfall。</translation>
    </message>
    <message>
        <source>在 catch 块中设置条件断点，检查异常对象字段。</source>
        <translation type="finished">in catch block in set conditional breakpoint,check exception object Field。</translation>
    </message>
    <message>
        <source>在代码行号左侧点击可设置/取消断点，右键点击断点可设置条件。</source>
        <translation type="finished">in code line number left side click can set/cancel breakpoint,right-click Breakpoint can set condition。</translation>
    </message>
    <message>
        <source>在大数组构造场景下是热点，且 GC 压力大。</source>
        <translation type="finished">in big Array construct in the scenario is hot spot,and GC high pressure。</translation>
    </message>
    <message>
        <source>在密集全局变量引用场景下可能成为热点。</source>
        <translation type="finished">in dense Global variable reference in the scenario may become hot spot。</translation>
    </message>
    <message>
        <source>在密集闭包调用场景下与 OP_CLOSURE 配对出现。</source>
        <translation type="finished">in dense closures call in the scenario and OP_CLOSURE pair appear。</translation>
    </message>
    <message>
        <source>在循环中使用 IIFE 可为每次迭代创建独立作用域，</source>
        <translation type="finished">in in the loop use IIFE can is every iterations create independent scope,</translation>
    </message>
    <message>
        <source>在循环中创建闭包时，所有闭包共享同一个循环变量</source>
        <translation type="finished">in in the loop create Closure when,all Closure share the same loop variable</translation>
    </message>
    <message>
        <source>在方法内部设置条件断点，访问 this 的字段。</source>
        <translation type="finished">in inside method set conditional breakpoint,access this Field。</translation>
    </message>
    <message>
        <source>在类实例方法密集调用场景下是热点。</source>
        <translation type="finished">in Class instance Method dense calls in the scenario is hot spot。</translation>
    </message>
    <message>
        <source>在编译期求值替换为单一 LOAD_CONST。</source>
        <translation type="finished">in compile time evaluate Replace is single LOAD_CONST。</translation>
    </message>
    <message>
        <source>在闭包对象中按索引存储。MiniLang 的 OP_CLOSURE 指令</source>
        <translation type="finished">in Closure object in store by index。MiniLang OP_CLOSURE instruction</translation>
    </message>
    <message>
        <source>在闭包循环场景下热点明显，开销高于普通函数调用。</source>
        <translation type="finished">in Closure loop scenario below hot spot obviously,overhead high at normal function call。</translation>
    </message>
    <message>
        <source>场景说明：</source>
        <translation type="finished">scenario Note:</translation>
    </message>
    <message>
        <source>堆值也被回收。MiniLang 使用引用计数 + 周期回收。</source>
        <translation type="finished">Heap Value also Collection。MiniLang use Reference counting + cycle collection。</translation>
    </message>
    <message>
        <source>堆分配：COW 容器。`var b = a` 共享所有权 refCount=2，`b.push(4)` 触发 detach 深拷贝。</source>
        <translation type="finished">heap allocation:COW container。`var b = a` share ownership refCount=2,`b.push(4)` trigger detach deep-copy。</translation>
    </message>
    <message>
        <source>堆分配：Value 存 StringData* 指针，对象继承 RefCounted 维护引用计数。COW：写时检查 refCount==1，否则深拷贝。</source>
        <translation type="finished">heap allocation:Value exist StringData* pointer,object Inheritance RefCounted maintain reference count。COW:write when check refCount==1,whether then deep-copy。</translation>
    </message>
    <message>
        <source>堆分配：基于哈希表的 COW 容器。键需为 string 类型。</source>
        <translation type="finished">heap allocation:based on hash table COW container。key needs is string Type。</translation>
    </message>
    <message>
        <source>堆分配：实例字段表通过 ClassInfo::flattenedFieldOrder 描述。方法查找经 methodCache_ 加速。</source>
        <translation type="finished">heap allocation:instance field table via ClassInfo::flattenedFieldOrder describe。method lookup through methodCache_ accelerate。</translation>
    </message>
    <message>
        <source>堆分配：闭包捕获外层 Environment（弱引用链 parent）。env 链打破循环依赖。</source>
        <translation type="finished">heap allocation:Closure capture outer Environment(weak reference chain parent)。env chain breaks cycle depend on。</translation>
    </message>
    <message>
        <source>堆化阶段：当外层函数返回时（OP_RETURN），栈帧被弹出。</source>
        <translation type="finished">heapify phase:when outer function return when(OP_RETURN),Stack frame is popped。</translation>
    </message>
    <message>
        <source>备注</source>
        <translation type="finished">remark</translation>
    </message>
    <message>
        <source>复制传播 + 跳转优化</source>
        <translation type="finished">copy propagation + jump optimization</translation>
    </message>
    <message>
        <source>复制传播 pass 识别 v_b = MOVE v_a 模式，将后续 v_b 的引用替换为 v_a。</source>
        <translation type="finished">copy propagation pass recognize v_b = MOVE v_a mode, after continue v_b reference Replace is v_a。</translation>
    </message>
    <message>
        <source>复合布尔条件（a &gt; 0 &amp;&amp; b &lt; 100）</source>
        <translation type="finished">compound boolean condition(a &gt; 0 &amp;&amp; b &lt; 100)</translation>
    </message>
    <message>
        <source>复合布尔表达式条件断点。MiniLang 的 and/or 短路求值（返回操作数原值而非布尔），</source>
        <translation type="finished">compound boolean Expression conditional breakpoint。MiniLang and/or short-circuit evaluation(return operand original value rather than boolean),</translation>
    </message>
    <message>
        <source>复杂表达式记录 0xFFFF 哨兵运行时报错。违反三后端一致性。</source>
        <translation type="finished">complex expression records 0xFFFF sentinel runtime report an error。violate three-backend consistency。</translation>
    </message>
    <message>
        <source>外层</source>
        <translation type="finished">outer</translation>
    </message>
    <message>
        <source>外层 catch 不会被触发（除非内层 catch 中再次 throw）。</source>
        <translation type="finished">outer catch will not trigger(unless inner catch in again throw)。</translation>
    </message>
    <message>
        <source>多个闭包可以共享同一个 upvalue，也可以各自独立。</source>
        <translation type="finished">multiple Closure can share the same upvalue,also can each independent。</translation>
    </message>
    <message>
        <source>多变量捕获</source>
        <translation type="finished">multi Variable capture</translation>
    </message>
    <message>
        <source>多层嵌套的闭包。最内层闭包捕获中间层变量，</source>
        <translation type="finished">multi-layer nested Closure。innermost Closure capture middle layer Variable,</translation>
    </message>
    <message>
        <source>失败</source>
        <translation type="finished">fail</translation>
    </message>
    <message>
        <source>子节点数: </source>
        <translation type="finished">child node count: </translation>
    </message>
    <message>
        <source>字典访问循环（10000 次）</source>
        <translation type="finished">Dictionary access loop (10000 times)</translation>
    </message>
    <message>
        <source>字典键值读写循环。DictData 使用 unordered_map，每次访问涉及哈希计算，</source>
        <translation type="finished">Dictionary key-value read/write loop。DictData use unordered_map,each time access involves hash computation,</translation>
    </message>
    <message>
        <source>字符串 &quot;hello&quot; 长度超过 ASCII 内联阈值，装箱为 StringData* 堆对象。</source>
        <translation type="finished">String &quot;hello&quot; length exceeds ASCII inline threshold,boxing is StringData* Heap object。</translation>
    </message>
    <message>
        <source>字符串 + 拼接。每次拼接会构造新 StringData（不可变语义），三后端性能相近（瓶颈在堆分配而非指令分发）。</source>
        <translation type="finished">String + concatenation。each time concatenation will construct a new StringData(immutable semantics),three backends perform similarly(bottleneck in heap allocation rather than instruction dispatch)。</translation>
    </message>
    <message>
        <source>字符串 StringData 同样使用 RefCounted，但字符串不可变，无需 COW detach。</source>
        <translation type="finished">String StringData same use RefCounted,but String immutable,no need COW detach。</translation>
    </message>
    <message>
        <source>字符串共享（无 COW）</source>
        <translation type="finished">string sharing(None COW)</translation>
    </message>
    <message>
        <source>字符串拼接循环（1000 次）</source>
        <translation type="finished">string concatenation loop(1000 times)</translation>
    </message>
    <message>
        <source>字符串插值</source>
        <translation type="finished">string interpolation</translation>
    </message>
    <message>
        <source>字符串插值。支持表达式嵌入。空插值 {} 报语法错误。</source>
        <translation type="finished">string interpolation。support Expression embed。empty interpolation {} report syntax error。</translation>
    </message>
    <message>
        <source>字符串相等条件（s == &quot;target&quot;）</source>
        <translation type="finished">string equality condition(s == &quot;target&quot;)</translation>
    </message>
    <message>
        <source>字符串相等比较。MiniLang 字符串相等基于值比较（非引用），</source>
        <translation type="finished">string equality compare。MiniLang string equality based on value comparison(non- reference),</translation>
    </message>
    <message>
        <source>字节码</source>
        <translation type="finished">Bytecode</translation>
    </message>
    <message>
        <source>字节码编译异常: %1</source>
        <translation type="finished">Bytecode Compile Exception: %1</translation>
    </message>
    <message>
        <source>字节码轨迹</source>
        <translation type="finished">Bytecode trace</translation>
    </message>
    <message>
        <source>字面文本</source>
        <translation type="finished">literal text</translation>
    </message>
    <message>
        <source>字面量</source>
        <translation type="finished">literal</translation>
    </message>
    <message>
        <source>存活节点重置 marked=false，为下一轮收集做准备。</source>
        <translation type="finished">alive node Reset marked=false,is below prepare for next collection round。</translation>
    </message>
    <message>
        <source>实例 InstanceData 持有字段表，字段值是 Value（可能引用其它堆对象）。</source>
        <translation type="finished">instance InstanceData holds field table,field value is Value(may reference other Heap object)。</translation>
    </message>
    <message>
        <source>实例字段引用</source>
        <translation type="finished">instance field reference</translation>
    </message>
    <message>
        <source>实例的 refCount 与字段值 refCount 相互独立。</source>
        <translation type="finished">instance's refCount and field value refCount mutually independent。</translation>
    </message>
    <message>
        <source>实时断点</source>
        <translation type="finished">real-time breakpoint</translation>
    </message>
    <message>
        <source>实现私有状态（外部无法直接访问 count，只能通过闭包操作）。</source>
        <translation type="finished">implement private state(external cannot directly access count,only via Closure operation)。</translation>
    </message>
    <message>
        <source>实验 1：词法分析 — 从字符到 Token</source>
        <translation type="finished">experiment 1:lexical analysis — from character to Token</translation>
    </message>
    <message>
        <source>实验 2：递归下降解析 — 从 Token 到 AST</source>
        <translation type="finished">experiment 2:recursive descent parse — from Token to AST</translation>
    </message>
    <message>
        <source>实验 3：树遍历解释器 — Visitor 模式执行 AST</source>
        <translation type="finished">experiment 3:tree-traversal interpreter — Visitor mode execute AST</translation>
    </message>
    <message>
        <source>实验 4：栈式字节码 VM — 编译与执行</source>
        <translation type="finished">experiment 4:stack-based Bytecode VM — Compile and execute</translation>
    </message>
    <message>
        <source>实验 5：寄存器式 VM — 三地址码与 IR</source>
        <translation type="finished">experiment 5:register-based VM — three-address code and IR</translation>
    </message>
    <message>
        <source>实验 6：三后端一致性 — 语义等价验证</source>
        <translation type="finished">experiment 6:three-backend consistency — semantic equivalence verification</translation>
    </message>
    <message>
        <source>实验 7：内存模型 — NaN-boxing 与 COW</source>
        <translation type="finished">experiment 7:Memory model — NaN-boxing and COW</translation>
    </message>
    <message>
        <source>实验 8：Bug 狩猎 — 从历史 Bug 学习</source>
        <translation type="finished">experiment 8:Bug Hunt — learn from historical bugs</translation>
    </message>
    <message>
        <source>实验手册</source>
        <translation type="finished">Lab manual</translation>
    </message>
    <message>
        <source>寄存器 (R0..Rn)</source>
        <translation type="finished">Register (R0..Rn)</translation>
    </message>
    <message>
        <source>寄存器式 VM</source>
        <translation type="finished">register-based VM</translation>
    </message>
    <message>
        <source>尚未生成</source>
        <translation type="finished">Not generated yet</translation>
    </message>
    <message>
        <source>尚未运行</source>
        <translation type="finished">Not run yet</translation>
    </message>
    <message>
        <source>局部</source>
        <translation type="finished">Local</translation>
    </message>
    <message>
        <source>局部变量被销毁（调用析构函数）。栈展开从 throw 点开始，</source>
        <translation type="finished">local variable destroy(call destructor)。stack unwinding from throw point start,</translation>
    </message>
    <message>
        <source>嵌套 try/catch（内层捕获）</source>
        <translation type="finished">Nested try/catch (inner capture)</translation>
    </message>
    <message>
        <source>嵌套左值 / 栈平衡</source>
        <translation type="finished">Nested lvalue / stack balance</translation>
    </message>
    <message>
        <source>嵌套的 try/catch 结构。内层 catch 优先匹配异常。</source>
        <translation type="finished">A nested try/catch structure. The inner catch matches the exception first.</translation>
    </message>
    <message>
        <source>嵌套索引赋值栈泄漏：a[0][1]=x</source>
        <translation type="finished">Nested index-assignment stack leak: a[0][1]=x</translation>
    </message>
    <message>
        <source>嵌套闭包（多层捕获）</source>
        <translation type="finished">Nested closure (multi-layer capture)</translation>
    </message>
    <message>
        <source>已存在</source>
        <translation type="finished">Already exists</translation>
    </message>
    <message>
        <source>已无更多提示</source>
        <translation type="finished">No more hints</translation>
    </message>
    <message>
        <source>已显示提示 %1/%2</source>
        <translation type="finished">Hint %1/%2 shown</translation>
    </message>
    <message>
        <source>已替换 %1 处</source>
        <translation type="finished">Replaced %1 occurrence(s)</translation>
    </message>
    <message>
        <source>已替换 %1+ 处（达到上限，仍有剩余匹配）</source>
        <translation type="finished">Replaced %1+ occurrences (limit reached, more matches remain)</translation>
    </message>
    <message>
        <source>已有运行在进行，请先停止当前运行</source>
        <translation type="finished">A run is in progress; please stop it first</translation>
    </message>
    <message>
        <source>已生成 IR：%1 基本块 / %2 条指令</source>
        <translation type="finished">IR generated: %1 basic blocks / %2 instructions</translation>
    </message>
    <message>
        <source>已请求加载到主编辑器</source>
        <translation type="finished">Requested to load into the main editor</translation>
    </message>
    <message>
        <source>已请求加载示例（请切到主编辑器查看）</source>
        <translation type="finished">Requested to load the sample (switch to the main editor to view it)</translation>
    </message>
    <message>
        <source>布尔短路条件（x &amp;&amp; y &gt; 0）</source>
        <translation type="finished">Boolean short-circuit condition (x &amp;&amp; y &gt; 0)</translation>
    </message>
    <message>
        <source>帧被弹出（栈展开）。main() 也没有 try/catch，继续弹出。</source>
        <translation type="finished">frame is popped(stack unwinding)。main() also no try/catch,Continue pop。</translation>
    </message>
    <message>
        <source>帮助</source>
        <translation type="finished">Help</translation>
    </message>
    <message>
        <source>常用于块作用域结束时确保变量被堆化（如 for 循环变量）。</source>
        <translation type="finished">Commonly used to ensure variables are heapified when a block scope ends (e.g., for-loop variables).</translation>
    </message>
    <message>
        <source>常用于：内层 catch 记录日志后重新抛出，或转换异常类型。</source>
        <translation type="finished">Commonly used: the inner catch logs and re-throws, or converts the exception type.</translation>
    </message>
    <message>
        <source>常用字典或实例作为异常对象，携带结构化错误信息。</source>
        <translation type="finished">A dictionary or instance is commonly used as the exception object, carrying structured error information.</translation>
    </message>
    <message>
        <source>常量 42 加入 IRFunction.constants 常量池（重复时复用索引）。</source>
        <translation type="finished">The constant 42 is added to the IRFunction.constants pool (the index is reused when duplicated).</translation>
    </message>
    <message>
        <source>常量折叠 1 + 2 → 3</source>
        <translation type="finished">Constant folding 1 + 2 → 3</translation>
    </message>
    <message>
        <source>常量折叠 pass 识别 LOAD_CONST 操作数全为常量的算术指令，</source>
        <translation type="finished">The constant-folding pass identifies arithmetic instructions whose LOAD_CONST operands are all constants, </translation>
    </message>
    <message>
        <source>常量池去重 / 三后端一致性</source>
        <translation type="finished">Constant-pool deduplication / three-backend consistency</translation>
    </message>
    <message>
        <source>常量池去重陷阱：int(0) 与 float(0.0)</source>
        <translation type="finished">Constant-pool deduplication trap: int(0) vs float(0.0)</translation>
    </message>
    <message>
        <source>平均 (μs)</source>
        <translation type="finished">Average (μs)</translation>
    </message>
    <message>
        <source>平均时间 (μs)</source>
        <translation type="finished">Average time (μs)</translation>
    </message>
    <message>
        <source>并立即中断当前控制流。异常对象被保存，用于后续 catch 变量绑定。</source>
        <translation type="finished">and immediately in break current control flow。exception object Save,use at after continue catch Variable bind。</translation>
    </message>
    <message>
        <source>异常从 risky() 抛出，沿调用栈向上搜索。risky() 没有 try/catch，</source>
        <translation type="finished">The exception is thrown from risky() and searches up the call stack. risky() has no try/catch, </translation>
    </message>
    <message>
        <source>异常从 try 块抛出后，立即跳转到对应 catch 块。</source>
        <translation type="finished">After the exception is thrown from the try block, it immediately jumps to the corresponding catch block.</translation>
    </message>
    <message>
        <source>异常从 validate() 抛出，validate() 没有 try/catch，帧被弹出（栈展开）。</source>
        <translation type="finished">The exception is thrown from validate(), which has no try/catch; the frame is popped (stack unwinding).</translation>
    </message>
    <message>
        <source>异常从内层 try 块抛出，首先匹配内层 catch。内层 catch 捕获后执行，</source>
        <translation type="finished">The exception is thrown from the inner try block and first matches the inner catch. After the inner catch captures and runs, </translation>
    </message>
    <message>
        <source>异常从最深层 countdown(0) 抛出，沿递归链向上传播。</source>
        <translation type="finished">The exception is thrown from the deepest countdown(0) and propagates up the recursion chain.</translation>
    </message>
    <message>
        <source>异常从被调用函数抛出，沿调用栈跨帧传播到调用者的 catch 块。</source>
        <translation type="finished">The exception is thrown from the callee and propagates across frames along the call stack to the caller's catch block.</translation>
    </message>
    <message>
        <source>异常传播器沿调用栈向上搜索匹配的 catch 块。</source>
        <translation type="finished">The exception propagator searches up the call stack for a matching catch block.</translation>
    </message>
    <message>
        <source>异常传播：throw 时 VM 沿调用栈向上查找 try 块，沿途弹出栈帧（risky 帧被销毁），最终落到 catch 帧。注意 risky 的局部变量在异常后不可访问。</source>
        <translation type="finished">Exception propagation: on throw, the VM searches up the call stack for a try block, popping frames along the way (the risky frame is destroyed) and finally landing on the catch frame. Note that risky's local variables are inaccessible after the exception.</translation>
    </message>
    <message>
        <source>异常处理 try/catch</source>
        <translation type="finished">exception handling try/catch</translation>
    </message>
    <message>
        <source>异常处理。throw 可抛任意值。catch 参数独占 slot（防止覆盖外层变量）。</source>
        <translation type="finished">Exception handling. throw can throw any value. The catch parameter occupies an exclusive slot (to avoid overwriting outer variables).</translation>
    </message>
    <message>
        <source>异常对象可以是任意值（字符串、数字、字典、实例）。</source>
        <translation type="finished">The exception object can be any value (string, number, dictionary, instance).</translation>
    </message>
    <message>
        <source>异常对象字段访问</source>
        <translation type="finished">exception object field access</translation>
    </message>
    <message>
        <source>异常对象是字典值，包含 code 和 message 字段。</source>
        <translation type="finished">The exception object is a dictionary value containing code and message fields.</translation>
    </message>
    <message>
        <source>异常对象检查（e.message == &quot;expected&quot;)</source>
        <translation type="finished">exception object check(e.message == &quot;expected&quot;)</translation>
    </message>
    <message>
        <source>异常沿调用栈传播到外层 catch 块被捕获。</source>
        <translation type="finished">The exception propagates along the call stack and is captured by the outer catch block.</translation>
    </message>
    <message>
        <source>异常沿调用栈向上传播，若没有找到任何匹配的 catch 块，</source>
        <translation type="finished">Exception along the call stack upward Propagation,if no find to any match catch block,</translation>
    </message>
    <message>
        <source>异常流</source>
        <translation type="finished">exception flow</translation>
    </message>
    <message>
        <source>异常路径复制 cleanup 字节码后 OP_THROW rethrow。</source>
        <translation type="finished">Exception path copy cleanup Bytecode after OP_THROW rethrow。</translation>
    </message>
    <message>
        <source>引用计数 &amp; COW</source>
        <translation type="finished">Reference counting &amp; COW</translation>
    </message>
    <message>
        <source>引用计数变化步骤：</source>
        <translation type="finished">Reference counting change Step:</translation>
    </message>
    <message>
        <source>弹出栈顶 2N 个值（key+value 对）构建 DictData 并压入。</source>
        <translation type="finished">pop stack top 2N Value(key+value pair) build DictData and push。</translation>
    </message>
    <message>
        <source>弹出栈顶 N 个元素构建 ArrayData 并压入。</source>
        <translation type="finished">pop stack top N elements build ArrayData and push。</translation>
    </message>
    <message>
        <source>弹出栈顶两个值（右、左），相加后压入结果。注意：左操作数在栈深处，右操作数在栈顶。</source>
        <translation type="finished">pop Stack top two Value(right,left),add together after push result。note:left operand in Stack deep,right operand in stack top。</translation>
    </message>
    <message>
        <source>弹出栈顶值写入全局槽位。</source>
        <translation type="finished">pop stack top Value write global slot。</translation>
    </message>
    <message>
        <source>弹出栈顶条件，若为 false 则跳转。</source>
        <translation type="finished">pop Stack top condition,if is false then jump。</translation>
    </message>
    <message>
        <source>
强制终止会跳过正常析构，未保存的代码将丢失。


        </source>
        <translation type="finished">
force-terminate will skip normal destruction,unsaved code lost。


        </translation>
    </message>
    <message>
        <source>当 a &gt; 0 且 b &lt; 100 同时成立时暂停</source>
        <translation type="finished">when a &gt; 0 and b &lt; 100 while holds pauses when</translation>
    </message>
    <message>
        <source>当 catch 块捕获异常且异常 message 字段等于 &quot;expected&quot; 时暂停</source>
        <translation type="finished">when catch block capture exception and Exception message Field equal to &quot;expected&quot; pauses when</translation>
    </message>
    <message>
        <source>当 outer 返回时，a 堆化；当 middle 返回时，b 堆化。</source>
        <translation type="finished">when outer return when,a heapify;when middle return when,b heapify。</translation>
    </message>
    <message>
        <source>当 x truthy 且 y &gt; 0 时暂停</source>
        <translation type="finished">when x truthy and y &gt; 0 pauses when</translation>
    </message>
    <message>
        <source>当 x 为 null 时暂停（如未初始化或显式赋值为 null）</source>
        <translation type="finished">when x is null pauses when(e.g. uninitialized or explicitly assign is null)</translation>
    </message>
    <message>
        <source>当前产生式：%1</source>
        <translation type="finished">current production:%1</translation>
    </message>
    <message>
        <source>当前函数局部作用域</source>
        <translation type="finished">current Function Local scope</translation>
    </message>
    <message>
        <source>当前源码 IR</source>
        <translation type="finished">current Source IR</translation>
    </message>
    <message>
        <source>当前章节：%1</source>
        <translation type="finished">current chapter:%1</translation>
    </message>
    <message>
        <source>当前题目：%1</source>
        <translation type="finished">current problem:%1</translation>
    </message>
    <message>
        <source>当变量 s 等于 &quot;target&quot; 时暂停</source>
        <translation type="finished">when Variable s equal to &quot;target&quot; pauses when</translation>
    </message>
    <message>
        <source>当方法被调用且 this.value &gt; 100 时暂停</source>
        <translation type="finished">when Method called and this.value &gt; 100 pauses when</translation>
    </message>
    <message>
        <source>循环 while (cond) {...}</source>
        <translation type="finished">loop while (cond) {...}</translation>
    </message>
    <message>
        <source>循环内 `if (cond) foo();` 每次泄漏 1 个栈值，多次循环触发栈溢出。</source>
        <translation type="finished">in the loop `if (cond) foo();` each time leak 1 Stack Value,multi times loop trigger stack overflow。</translation>
    </message>
    <message>
        <source>循环执行到第 50 次迭代时暂停，其余迭代继续执行（不暂停）</source>
        <translation type="finished">loop execute to ordinal 50 iterations pauses when,the rest iteration continue execution(not Pause)</translation>
    </message>
    <message>
        <source>循环求和（1 到 100000）</source>
        <translation type="finished">loop sum(1 to 100000)</translation>
    </message>
    <message>
        <source>性能分析：</source>
        <translation type="finished">performance analysis:</translation>
    </message>
    <message>
        <source>性能剖析</source>
        <translation type="finished">performance profiling</translation>
    </message>
    <message>
        <source>所有新建的 ArrayData / DictData / InstanceData 在构造函数中调用 GcManager::instance().registerTracked(this)，</source>
        <translation type="finished">all New ArrayData / DictData / InstanceData in construct Function in call GcManager::instance().registerTracked(this),</translation>
    </message>
    <message>
        <source>所有闭包共享同一个 i 变量。循环结束后 i = 3，</source>
        <translation type="finished">all Closure share the same i Variable。loop end after i = 3,</translation>
    </message>
    <message>
        <source>打开文件</source>
        <translation type="finished">Open File</translation>
    </message>
    <message>
        <source>打开文件(&amp;O)...</source>
        <translation type="finished">Open File(&amp;O)...</translation>
    </message>
    <message>
        <source>打开文件夹</source>
        <translation type="finished">Open File clip</translation>
    </message>
    <message>
        <source>打开文件夹(&amp;D)...</source>
        <translation type="finished">Open File clip(&amp;D)...</translation>
    </message>
    <message>
        <source>执行 then 块后 JUMP L_end，else 块从 L_else 开始。</source>
        <translation type="finished">execute then block after JUMP L_end,else block from L_else start。</translation>
    </message>
    <message>
        <source>执行循环体后 JUMP L_start。每个基本块终结于 BRANCH/JUMP/RETURN。</source>
        <translation type="finished">execute loop body after JUMP L_start。each basic block finalize at BRANCH/JUMP/RETURN。</translation>
    </message>
    <message>
        <source>执行期间不再触发（性能权衡：mark-sweep 开销 O(节点数+边数)，仅起点触发）。</source>
        <translation type="finished">execute during no longer trigger(performance tradeoff:mark-sweep overhead O(node count+edge count),only starting point trigger)。</translation>
    </message>
    <message>
        <source>执行次数</source>
        <translation type="finished">execute times count</translation>
    </message>
    <message>
        <source>指令计数</source>
        <translation type="finished">instruction count</translation>
    </message>
    <message>
        <source>指令读写 upvalue。若 upvalue 仍 open，直接访问栈槽；若已 closed，访问堆地址。</source>
        <translation type="finished">instruction read/write upvalue。if upvalue still open,directly access stack slot;if closed,access heap address。</translation>
    </message>
    <message>
        <source>捕获的变量称为 upvalue（上层作用域的值）。</source>
        <translation type="finished">capture Variable called upvalue(on layer scope Value)。</translation>
    </message>
    <message>
        <source>捕获阶段：VM 遍历 upvalue 描述符，为每个 upvalue 查找或创建 Upvalue 对象。</source>
        <translation type="finished">capture phase:VM traverse upvalue descriptor,is each upvalue Find or create Upvalue object。</translation>
    </message>
    <message>
        <source>控制流回到 main() 的 try 块，异常被 catch 块捕获。</source>
        <translation type="finished">control flow return to main() try block,Exception catch block capture。</translation>
    </message>
    <message>
        <source>提示1：resolveUpvalue 沿外层帧查找——但中间函数若没显式引用 x，则不会创建 upvalue 槽位。</source>
        <translation type="finished">Hint 1:resolveUpvalue along outer frame Find——but middle function if no explicitly reference x,then will not create upvalue slot。</translation>
    </message>
    <message>
        <source>提示1：在 visitIndexAssign 中检查 OP_WRITEBACK_INDEX_* 后栈的 push/pop 数量是否匹配。</source>
        <translation type="finished">Hint 1:in visitIndexAssign in check OP_WRITEBACK_INDEX_* after Stack push/pop quantity whether match。</translation>
    </message>
    <message>
        <source>提示1：对比 visitImportStmt 存入 moduleCache_ 的 key 与 clearModuleCache 接收的 path。</source>
        <translation type="finished">Hint 1:comparison visitImportStmt store into moduleCache_ key and clearModuleCache receive path。</translation>
    </message>
    <message>
        <source>提示1：检查 BytecodeChunk::addConstant 的去重条件——只比较 Value::equals() 是否足够？</source>
        <translation type="finished">Hint 1:check BytecodeChunk::addConstant deduplication condition——only compare Value::equals() whether sufficient?</translation>
    </message>
    <message>
        <source>提示1：检查 Formatter 中 isFunOrClass lambda 的判定——是否检查 NODE_EXPORT_STMT 内部？</source>
        <translation type="finished">Hint 1:check Formatter in isFunOrClass lambda determine——whether check NODE_EXPORT_STMT internal?</translation>
    </message>
    <message>
        <source>提示1：检查 IR 路径 visitIfStmt 的 then/else 分支调用——是 visitNode 还是 visitStatement？</source>
        <translation type="finished">Hint 1:check IR path visitIfStmt then/else branch call——is visitNode still is visitStatement?</translation>
    </message>
    <message>
        <source>提示1：检查 Parser::isLiteralDefaultExpr 哪些节点被允许？</source>
        <translation type="finished">Hint 1:check Parser::isLiteralDefaultExpr which Node allow?</translation>
    </message>
    <message>
        <source>提示1：检查 callNamedFunction 中 CallFrame.line 初始化值。</source>
        <translation type="finished">Hint 1:check callNamedFunction in CallFrame.line initialize Value。</translation>
    </message>
    <message>
        <source>提示1：检查 executeCalls 中 REG_RETURN 的处理路径——是 return 直接返回，还是 goto 到统一调用点？</source>
        <translation type="finished">Hint 1:check executeCalls in REG_RETURN processing path——is return directly return,still is goto to unified call site?</translation>
    </message>
    <message>
        <source>提示1：检查 normalizeModulePath 的 &#x27;X:&#x27; 检测分支——只检测 &#x27;X:/&#x27; 还是所有 &#x27;X:&#x27; 开头？</source>
        <translation type="finished">Hint 1:check normalizeModulePath &#x27;X:&#x27; detect branch——only detect &#x27;X:/&#x27; still is all &#x27;X:&#x27; beginning?</translation>
    </message>
    <message>
        <source>提示2：BinaryOp/FunCall 等表达式不应作为默认值。</source>
        <translation type="finished">Hint 2:BinaryOp/FunCall etc. Expression not should as default value。</translation>
    </message>
    <message>
        <source>提示2：IR 路径有 computeFreeVars 前向分析修复了此问题，Compiler 路径没有。</source>
        <translation type="finished">Hint 2:IR path has computeFreeVars before to analyze and fix this Problem,Compiler path does not has。</translation>
    </message>
    <message>
        <source>提示2：StackVM 通过 notifyStep(ip, op) 在所有指令后统一调用，RegisterVM 是否对齐？</source>
        <translation type="finished">Hint 2:StackVM via notifyStep(ip, op) in all instruction after unified call,RegisterVM whether align?</translation>
    </message>
    <message>
        <source>提示2：checkBreak 是否在某处更新 line？</source>
        <translation type="finished">Hint 2:checkBreak whether in somewhere update line?</translation>
    </message>
    <message>
        <source>提示2：int(0) 与 float(0.0) 的 getType() 不同，但 equals() 返回 true。</source>
        <translation type="finished">Hint 2:int(0) and float(0.0) getType() different,but equals() return true。</translation>
    </message>
    <message>
        <source>提示2：visitImportStmt 是否做了路径规范化（\\→/、strip ./）？</source>
        <translation type="finished">Hint 2:visitImportStmt whether do path normalization(\\→/,strip./)?</translation>
    </message>
    <message>
        <source>提示2：visitStatement 是统一包装器，会对表达式语句 emit POP。</source>
        <translation type="finished">Hint 2:visitStatement is unified wrapper,will, for the expression statement emit POP。</translation>
    </message>
    <message>
        <source>提示2：反斜杠在何处被转为正斜杠？若已转，原反斜杠分支是死代码。</source>
        <translation type="finished">Hint 2:backslash in where convert to forward slash?if convert,original backslash branch is dead code。</translation>
    </message>
    <message>
        <source>提示2：对比 visitMethodCall 的同步修复——它已经不推索引，visitIndexAssign 是否漏改？</source>
        <translation type="finished">Hint 2:comparison visitMethodCall sync fix——it already not push index,visitIndexAssign whether missed modification?</translation>
    </message>
    <message>
        <source>提示2：导出形式应与未导出形式格式化结果一致。</source>
        <translation type="finished">Hint 2:Export form should and not yet Export form formatting result consistent。</translation>
    </message>
    <message>
        <source>提示3：UnaryOp(NEGATE) 套 NumberLiteral 应该被允许（负数字面量）。</source>
        <translation type="finished">Hint 3:UnaryOp(NEGATE) wrap NumberLiteral should allow(negative literal)。</translation>
    </message>
    <message>
        <source>提示3：clearModuleCache 需同步做相同规范化。</source>
        <translation type="finished">Hint 3:clearModuleCache needs synchronously do same normalization。</translation>
    </message>
    <message>
        <source>提示3：修复方案：在 visitFunDecl 加入类似 IR 的 computeFreeVars 前向分析。</source>
        <translation type="finished">Hint 3:fix solution:in visitFunDecl add to similar IR computeFreeVars before to analyze。</translation>
    </message>
    <message>
        <source>提示3：修复需在每次暂停时更新 callStack_.back().line 为当前节点行号。</source>
        <translation type="finished">Hint 3:fix needs in each time when paused update callStack_.back().line is current Node line number。</translation>
    </message>
    <message>
        <source>提示3：修复需拒绝所有 path.size() &gt;= 2 &amp;&amp; path[1] == &#x27;:&#x27; 形式。</source>
        <translation type="finished">Hint 3:fix needs reject all path.size() &gt;= 2 &amp;&amp; path[1] == &#x27;:&#x27; form。</translation>
    </message>
    <message>
        <source>提示3：修复需要先比较 getType()，再比较 equals()。</source>
        <translation type="finished">Hint 3:fix needs first compare getType(),then compare equals()。</translation>
    </message>
    <message>
        <source>提示3：修复：isFunOrClass 解包 NODE_EXPORT_STMT 检查内部 declaration 节点类型。</source>
        <translation type="finished">Hint 3:fix:isFunOrClass unwrap NODE_EXPORT_STMT check internal declaration node type。</translation>
    </message>
    <message>
        <source>提示3：修复：捕获结果，若 VM_OK 则调用 stepCallback_。</source>
        <translation type="finished">Hint 3:fix:capture result,if VM_OK then call stepCallback_。</translation>
    </message>
    <message>
        <source>提示3：同样的 Bug 也存在于 visitWhileStmt 和 visitForStmt 的单语句体路径。</source>
        <translation type="finished">Hint 3:same Bug also exist in at visitWhileStmt and visitForStmt single statement body path。</translation>
    </message>
    <message>
        <source>提示3：在 IR 路径不触发是因为 BytecodeIRBackend 用 vreg 物化栈值，不依赖物理栈平衡。</source>
        <translation type="finished">Hint 3:in IR path does not trigger is because BytecodeIRBackend use vreg materialize Stack Value,not depend on physical stack balance。</translation>
    </message>
    <message>
        <source>插值支持嵌套字符串、字典、数组，嵌套深度限制 64 层防栈溢出。</source>
        <translation type="finished">interpolation supports nested string,Dictionary,Array,nested depth restrict 64 layer prevent stack overflow。</translation>
    </message>
    <message>
        <source>操作数栈 (栈顶 ↑)</source>
        <translation type="finished">operand stack (stack top ↑)</translation>
    </message>
    <message>
        <source>支持 break/continue。循环变量在顶层时退出后清理（DELETE_VAR）。</source>
        <translation type="finished">support break/continue。loop variable in top level when Exit after clean(DELETE_VAR)。</translation>
    </message>
    <message>
        <source>支持 super.method() 调用，三后端 super 调用语义一致。</source>
        <translation type="finished">support super.method() call,three backends super call semantic consistency。</translation>
    </message>
    <message>
        <source>支持索引读写（a[0] / d[&quot;key&quot;]）和嵌套索引赋值（a[0][1]=x）。</source>
        <translation type="finished">support index read/write(a[0] / d[&quot;key&quot;])and nested index assign(a[0][1]=x)。</translation>
    </message>
    <message>
        <source>支持闭包与 upvalue 捕获（3+ 层）。</source>
        <translation type="finished">support Closure and upvalue capture(3+ layer)。</translation>
    </message>
    <message>
        <source>教学场景库</source>
        <translation type="finished">teaching scenario library</translation>
    </message>
    <message>
        <source>数组/字典使用 Copy-On-Write（COW），写前检查独占所有权。</source>
        <translation type="finished">Array/Dictionary use Copy-On-Write(COW),write before check exclusive ownership。</translation>
    </message>
    <message>
        <source>数组与字典</source>
        <translation type="finished">Array and Dictionary</translation>
    </message>
    <message>
        <source>数组与字典字面量。字典键强制为 string，键不存在时返回 null。</source>
        <translation type="finished">Array and dictionary literal。Dictionary key force is string,key does not exist when return null。</translation>
    </message>
    <message>
        <source>数组基础生命周期</source>
        <translation type="finished">Array basis lifetime</translation>
    </message>
    <message>
        <source>整数 -1 以补码形式存储到低 48 位（全 1），</source>
        <translation type="finished">integer -1 stored in two's complement to low 48 bits(all 1),</translation>
    </message>
    <message>
        <source>整数 42 在 int48 范围（|v| &lt; 2^47）内，直接内联到 NaN-box 的低 48 位。</source>
        <translation type="finished">The integer 42 is within the int48 range (|v| &lt; 2^47) and is inlined directly into the low 48 bits of the NaN-box.</translation>
    </message>
    <message>
        <source>整数减法。性能特征与 OP_ADD 一致，但热度通常较低（循环场景下递增多于递减）。</source>
        <translation type="finished">Integer subtraction. Performance characteristics match OP_ADD, but it is usually less hot (in loops, increments outnumber decrements).</translation>
    </message>
    <message>
        <source>整数加法。在循环场景下是绝对热点——单条 OP_ADD 比解释器 Visitor dispatch 快 5-10 倍。</source>
        <translation type="finished">Integer addition. An absolute hot spot in loop scenarios — a single OP_ADD is 5-10x faster than the interpreter's Visitor dispatch.</translation>
    </message>
    <message>
        <source>整数字面量 42</source>
        <translation type="finished">Integer literal 42</translation>
    </message>
    <message>
        <source>整数字面量直接 LOAD_CONST 加载到虚拟寄存器。</source>
        <translation type="finished">An integer literal is loaded directly into a virtual register via LOAD_CONST.</translation>
    </message>
    <message>
        <source>文件</source>
        <translation type="finished">File</translation>
    </message>
    <message>
        <source>文件名 (将以 .mini 扩展名创建):</source>
        <translation type="finished">File name (will be created with the.mini extension):</translation>
    </message>
    <message>
        <source>文件名不得包含路径分隔符或父目录引用</source>
        <translation type="finished">The file name must not contain path separators or parent-directory references</translation>
    </message>
    <message>
        <source>文件夹名:</source>
        <translation type="finished">Folder name:</translation>
    </message>
    <message>
        <source>文件已修改，是否保存？</source>
        <translation type="finished">The file has been modified. Save it?</translation>
    </message>
    <message>
        <source>文件已存在：</source>
        <translation type="finished">File already exists:</translation>
    </message>
    <message>
        <source>文件过大 (</source>
        <translation type="finished">File too large (</translation>
    </message>
    <message>
        <source>斐波那契递归（fib(20)）</source>
        <translation type="finished">fibonacci recursion(fib(20))</translation>
    </message>
    <message>
        <source>新名称:</source>
        <translation type="finished">New name:</translation>
    </message>
    <message>
        <source>新建(&amp;N)</source>
        <translation type="finished">New (&amp;N)</translation>
    </message>
    <message>
        <source>新建文件</source>
        <translation type="finished">New file</translation>
    </message>
    <message>
        <source>新建文件夹</source>
        <translation type="finished">New folder</translation>
    </message>
    <message>
        <source>方法调用。比 OP_CALL 更昂贵——涉及方法查找（method resolution）。</source>
        <translation type="finished">Method call. More expensive than OP_CALL — involves method lookup (method resolution).</translation>
    </message>
    <message>
        <source>方法调用条件（this.value &gt; 100）</source>
        <translation type="finished">Method-call condition (this.value &gt; 100)</translation>
    </message>
    <message>
        <source>方法调用栈：distance 帧的 this 隐式绑定到 p 实例，可通过 this 访问字段 x/y。方法查找经过 ClassInfo::methodCache_ 缓存加速。</source>
        <translation type="finished">Method-call stack: in the distance frame, this is implicitly bound to the p instance, and fields x/y can be accessed via this. Method lookup is accelerated by the ClassInfo::methodCache_ cache.</translation>
    </message>
    <message>
        <source>方法调用：通过 methodCache_ 查找方法，避免重复 ClassInfo 遍历。</source>
        <translation type="finished">Method call: looks up the method via methodCache_ to avoid repeated ClassInfo traversal.</translation>
    </message>
    <message>
        <source>无</source>
        <translation type="finished">None</translation>
    </message>
    <message>
        <source>无断点</source>
        <translation type="finished">No breakpoints</translation>
    </message>
    <message>
        <source>无条件跳转到 offset 指定的相对位置。</source>
        <translation type="finished">Unconditional jump to the relative position specified by offset.</translation>
    </message>
    <message>
        <source>无法保存文件: </source>
        <translation type="finished">Cannot save file: </translation>
    </message>
    <message>
        <source>无法创建文件夹（可能已存在）</source>
        <translation type="finished">Cannot create the folder (it may already exist)</translation>
    </message>
    <message>
        <source>无法创建文件：</source>
        <translation type="finished">Cannot create file:</translation>
    </message>
    <message>
        <source>无法启动异步执行: %1</source>
        <translation type="finished">Cannot start async execution: %1</translation>
    </message>
    <message>
        <source>无法打开文件: </source>
        <translation type="finished">Cannot open file: </translation>
    </message>
    <message>
        <source>无选中章节</source>
        <translation type="finished">No chapter selected</translation>
    </message>
    <message>
        <source>无需堆分配，Value 拷贝仅 8 字节赋值。</source>
        <translation type="finished">No heap allocation needed; copying a Value is just an 8-byte assignment.</translation>
    </message>
    <message>
        <source>时间对比</source>
        <translation type="finished">Time comparison</translation>
    </message>
    <message>
        <source>是否现在保存？</source>
        <translation type="finished">Save now?</translation>
    </message>
    <message>
        <source>显式关闭：OP_CLOSE_UPVALUE 指令显式关闭 upvalue（用于变量离开作用域时）。</source>
        <translation type="finished">Explicit close: the OP_CLOSE_UPVALUE instruction explicitly closes an upvalue (used when a variable leaves scope).</translation>
    </message>
    <message>
        <source>普通断点</source>
        <translation type="finished">Normal breakpoint</translation>
    </message>
    <message>
        <source>暂无最近打开的工作区</source>
        <translation type="finished">No recently opened workspaces</translation>
    </message>
    <message>
        <source>替换</source>
        <translation type="finished">Replace</translation>
    </message>
    <message>
        <source>替换为...</source>
        <translation type="finished">Replace with...</translation>
    </message>
    <message>
        <source>最基础的异常处理形式。try 块中 throw 抛出异常对象，</source>
        <translation type="finished">The most basic form of exception handling. throw in the try block raises an exception object, </translation>
    </message>
    <message>
        <source>最基础的条件断点形式。当循环变量 i 等于 50 时暂停。</source>
        <translation type="finished">The most basic form of conditional breakpoint. Pauses when the loop variable i equals 50.</translation>
    </message>
    <message>
        <source>最基础的调用栈形态：main 调用 foo，foo 返回后栈帧弹出。</source>
        <translation type="finished">The most basic call-stack shape: main calls foo, and the frame pops after foo returns.</translation>
    </message>
    <message>
        <source>最基础的闭包形式。内部函数捕获外层变量 x，</source>
        <translation type="finished">The most basic closure form. The inner function captures the outer variable x, </translation>
    </message>
    <message>
        <source>最大化</source>
        <translation type="finished">Maximize</translation>
    </message>
    <message>
        <source>最小化</source>
        <translation type="finished">Minimize</translation>
    </message>
    <message>
        <source>最快</source>
        <translation type="finished">Fastest</translation>
    </message>
    <message>
        <source>最终传播到 main() 的 catch 块被捕获。</source>
        <translation type="finished">Finally it propagates to main()'s catch block and is captured.</translation>
    </message>
    <message>
        <source>最终传播到顶层导致程序终止。MiniLang 中未捕获异常会打印</source>
        <translation type="finished">Finally it propagates to the top level and terminates the program. In MiniLang, an uncaught exception prints an</translation>
    </message>
    <message>
        <source>最终传播到顶层，程序终止，print 语句不会执行。</source>
        <translation type="finished">Finally it propagates to the top level, the program terminates, and the print statement does not execute.</translation>
    </message>
    <message>
        <source>最近打开</source>
        <translation type="finished">Recently opened</translation>
    </message>
    <message>
        <source>未初始化的 VarDecl 用 LOAD_NULL 占位。</source>
        <translation type="finished">An uninitialized VarDecl uses LOAD_NULL as a placeholder.</translation>
    </message>
    <message>
        <source>未命名-%1</source>
        <translation type="finished">unnamed-%1</translation>
    </message>
    <message>
        <source>未找到</source>
        <translation type="finished">not yet find to</translation>
    </message>
    <message>
        <source>未捕获异常（传播到顶层）</source>
        <translation type="finished">uncaptured exception(propagate to top level)</translation>
    </message>
    <message>
        <source>未知的启动异常</source>
        <translation type="finished">unknown Start Exception</translation>
    </message>
    <message>
        <source>未绑定控制器</source>
        <translation type="finished">unbound controller</translation>
    </message>
    <message>
        <source>未运行</source>
        <translation type="finished">not yet Run</translation>
    </message>
    <message>
        <source>未预期的未知异常（无法获取类型信息）</source>
        <translation type="finished">unexpected unknown exception(cannot acquire type information)</translation>
    </message>
    <message>
        <source>未预期的错误 [%1]: %2</source>
        <translation type="finished">unexpected Error [%1]: %2</translation>
    </message>
    <message>
        <source>本例 refCount=2 时调用 b.push(4)，触发 detach：先 release 原 refCount（→1），</source>
        <translation type="finished">in this example refCount=2 called when b.push(4),trigger detach:first release original refCount(→1),</translation>
    </message>
    <message>
        <source>本例 v = 2^46 - 1 仍在范围内，仍内联存储。</source>
        <translation type="finished">in this example v = 2^46 - 1 still in within range,still inlined storage。</translation>
    </message>
    <message>
        <source>本例 x = 1 后从未读取 x，整条赋值链可消除。</source>
        <translation type="finished">in this example x = 1 after never read x,entire assign chain can eliminate。</translation>
    </message>
    <message>
        <source>本图示演示 PTR_TAG_BASE + 指针位模式，实际指针值会随运行而变化。</source>
        <translation type="finished">this diagram Demo PTR_TAG_BASE + pointer bit pattern,actual pointer value changes with execution。</translation>
    </message>
    <message>
        <source>条件为假时跳过循环体。</source>
        <translation type="finished">condition is false when skip loop body。</translation>
    </message>
    <message>
        <source>条件分支 if (cond) {...} else {...}</source>
        <translation type="finished">conditional branch if (cond) {...} else {...}</translation>
    </message>
    <message>
        <source>条件分支。then/else 分支可以是 block（{}包裹）或单语句（无花括号）。</source>
        <translation type="finished">conditional branch。then/else branch can is block({}wrap)or single statement(None braces)。</translation>
    </message>
    <message>
        <source>条件在断点行命中时求值一次，结果为 truthy（非 0 / 非 null / 非 false）时暂停。</source>
        <translation type="finished">condition in Breakpoint Line hit when evaluate once,result is truthy(non- 0 / non- null / non- false)pauses when。</translation>
    </message>
    <message>
        <source>条件循环。支持 break/continue。break 退出循环，continue 跳过当前迭代。</source>
        <translation type="finished">conditional loop。support break/continue。break exit loop,continue skip current iteration。</translation>
    </message>
    <message>
        <source>条件断点</source>
        <translation type="finished">conditional breakpoint</translation>
    </message>
    <message>
        <source>条件断点在沙箱中求值时使用 Interpreter 的 EQ 实现。</source>
        <translation type="finished">conditional breakpoint in sandbox in evaluate when use Interpreter EQ implement。</translation>
    </message>
    <message>
        <source>条件断点求值发生未知异常（条件: </source>
        <translation type="finished">conditional breakpoint evaluate unknown exception occurs (condition: </translation>
    </message>
    <message>
        <source>条件断点求值异常: </source>
        <translation type="finished">conditional breakpoint evaluate Exception: </translation>
    </message>
    <message>
        <source>条件断点求值时与正常运行时路径一致。</source>
        <translation type="finished">conditional breakpoint evaluate when and normal runtime path consistent。</translation>
    </message>
    <message>
        <source>条件断点求值时若 x 为 falsy，整个表达式直接返回 x（短路），不计算 y。</source>
        <translation type="finished">conditional breakpoint evaluate when if x is falsy,entire Expression directly return x(short-circuit),not compute y。</translation>
    </message>
    <message>
        <source>条件断点求值时遵循相同语义：a &gt; 0 为 falsy 时不会求值 b &lt; 100。</source>
        <translation type="finished">conditional breakpoint evaluate when follow same semantics:a &gt; 0 is falsy when will not evaluate b &lt; 100。</translation>
    </message>
    <message>
        <source>条件求值时通过 boundInstance_ 缓存访问实例字段，与正常运行时语义一致。</source>
        <translation type="finished">condition evaluation via boundInstance_ cache access instance fields,and normal runtime semantic consistency。</translation>
    </message>
    <message>
        <source>条件表达式</source>
        <translation type="finished">conditional expression</translation>
    </message>
    <message>
        <source>条件跳转。在 while/if 场景下每次迭代触发。</source>
        <translation type="finished">conditional jump。in while/if in the scenario every iterations trigger。</translation>
    </message>
    <message>
        <source>构造数组。涉及堆分配 + GcManager::registerTracked。</source>
        <translation type="finished">construct Array。involves heap allocation + GcManager::registerTracked。</translation>
    </message>
    <message>
        <source>构造新 ArrayData，refCount=1</source>
        <translation type="finished">construct a new ArrayData,refCount=1</translation>
    </message>
    <message>
        <source>构造新 InstanceData，refCount=1</source>
        <translation type="finished">construct a new InstanceData,refCount=1</translation>
    </message>
    <message>
        <source>构造新 StringData（&#x27;hello!&#x27;），s2 旧值 release（→1）</source>
        <translation type="finished">construct a new StringData(&#x27;hello!&#x27;),s2 old Value release(→1)</translation>
    </message>
    <message>
        <source>构造新 StringData，refCount=1</source>
        <translation type="finished">construct a new StringData,refCount=1</translation>
    </message>
    <message>
        <source>构造闭包。涉及 ClosureData 分配 + upvalue 捕获。</source>
        <translation type="finished">construct Closure。involves ClosureData allocate + upvalue capture。</translation>
    </message>
    <message>
        <source>析构时 Worker 未在 5 秒内停止，回退到 terminate()（进程退出阶段）</source>
        <translation type="finished">destruction when Worker not yet in 5 seconds in Stop,fall back to terminate()(process exit phase)</translation>
    </message>
    <message>
        <source>析构钩子 onDestroyed 同步从 aliveSet_ 移除本指针，保证 aliveSet_ 与实际存活状态一致。</source>
        <translation type="finished">destruction hook onDestroyed synchronously from aliveSet_ remove this pointer,guarantee aliveSet_ and actually alive Status consistent。</translation>
    </message>
    <message>
        <source>查找</source>
        <translation type="finished">Find</translation>
    </message>
    <message>
        <source>查找...</source>
        <translation type="finished">Find...</translation>
    </message>
    <message>
        <source>查看 AST 树形图 (Ctrl+Shift+A)</source>
        <translation type="finished">View AST tree diagram (Ctrl+Shift+A)</translation>
    </message>
    <message>
        <source>查看 Token / IR / 字节码</source>
        <translation type="finished">View Token / IR / Bytecode</translation>
    </message>
    <message>
        <source>查看答案</source>
        <translation type="finished">view answer</translation>
    </message>
    <message>
        <source>标准差 (μs)</source>
        <translation type="finished">standard deviation (μs)</translation>
    </message>
    <message>
        <source>标记从根集可达的对象，释放不可达的循环孤岛。&lt;/p&gt;</source>
        <translation type="finished">mark from root set reachable object,release unreachable cycle island。&lt;/p&gt;</translation>
    </message>
    <message>
        <source>标量内联：IEEE 754 double 直接存入 NaN-box 的 64 位。注意 tag bits 与 NaN 模式不冲突。</source>
        <translation type="finished">scalar inline:IEEE 754 double directly store into NaN-box 64 bits。note tag bits and NaN mode not conflict。</translation>
    </message>
    <message>
        <source>标量内联：bool 编码为 int48（0/1），tag bits 与 int 不同（INT_TAG_BASE=0x7FF8 vs BOOL_TAG_BASE=0x7FF9）。</source>
        <translation type="finished">scalar inline:bool compile code is int48(0/1),tag bits and int different(INT_TAG_BASE=0x7FF8 vs BOOL_TAG_BASE=0x7FF9)。</translation>
    </message>
    <message>
        <source>标量内联：int48 直接存入 NaN-box 的低 48 位，无需堆分配。范围 |v| &lt; 2^47。</source>
        <translation type="finished">scalar inline:int48 directly store into NaN-box low 48 bits,no need heap allocation。range |v| &lt; 2^47。</translation>
    </message>
    <message>
        <source>标量内联：唯一编码 NULL_BITS=0x7FFA&lt;&lt;48，payload 全 0。</source>
        <translation type="finished">scalar inline:unique encoding NULL_BITS=0x7FFA&lt;&lt;48,payload all 0。</translation>
    </message>
    <message>
        <source>栈/堆：Closure 引用计数归零，upvalue 数组释放，堆值（若无引用）被回收</source>
        <translation type="finished">stack/heap:Closure Reference counting zero,upvalue Array release,Heap Value(if no references) Collection</translation>
    </message>
    <message>
        <source>栈展开是异常传播的核心机制。未匹配的帧被弹出，</source>
        <translation type="finished">stack unwinding is exception propagation core mechanism。not yet match frame is popped,</translation>
    </message>
    <message>
        <source>栈展开期间不可中断（除非再次抛出异常）。</source>
        <translation type="finished">stack unwinding during non-interruptible(unless again throw exception)。</translation>
    </message>
    <message>
        <source>栈帧</source>
        <translation type="finished">stack frame</translation>
    </message>
    <message>
        <source>栈式 VM</source>
        <translation type="finished">stack-based VM</translation>
    </message>
    <message>
        <source>栈推入外层索引。循环内每次迭代泄漏 2 个栈值，约 512 次循环触发栈溢出。仅影响非 IR 路径。</source>
        <translation type="finished">Stack push outer index。in the loop every iterations leak 2 Stack Value,about 512 times loop trigger stack overflow。only affect non-IR path。</translation>
    </message>
    <message>
        <source>栈：OP_CLOSE_UPVALUE 执行后，upvalue 从栈指向堆</source>
        <translation type="finished">Stack:OP_CLOSE_UPVALUE after execution,upvalue from Stack points to Heap</translation>
    </message>
    <message>
        <source>栈：OP_GET_UPVALUE 推入 upvalue 值，OP_SET_UPVALUE 弹出值写入 upvalue</source>
        <translation type="finished">Stack:OP_GET_UPVALUE push upvalue Value,OP_SET_UPVALUE pop Value write upvalue</translation>
    </message>
    <message>
        <source>栈：Upvalue 指针指向栈槽，openUpvalues_ 插入条目</source>
        <translation type="finished">Stack:Upvalue pointer points to stack slot,openUpvalues_ insert entry</translation>
    </message>
    <message>
        <source>栈：finally/catch 块执行完毕后，栈状态恢复到 try 之前</source>
        <translation type="finished">Stack:finally/catch block after execution completes,Stack state restoration to try before</translation>
    </message>
    <message>
        <source>栈：异常对象弹出，绑定到 catch 变量，恢复正常执行</source>
        <translation type="finished">Stack:exception object pop,bind to catch Variable,restore execute normally</translation>
    </message>
    <message>
        <source>栈：弹出 OP_CLOSURE 的操作数，推入 Closure 对象</source>
        <translation type="finished">Stack:pop OP_CLOSURE operand,push Closure object</translation>
    </message>
    <message>
        <source>栈：弹出当前操作数，标记异常状态</source>
        <translation type="finished">Stack:pop current operand,mark Exception Status</translation>
    </message>
    <message>
        <source>栈：恢复正常执行状态，清除异常标志</source>
        <translation type="finished">Stack:restore execute normally Status,clear exception flag</translation>
    </message>
    <message>
        <source>栈：栈槽弹出，Upvalue.value 从栈复制到堆，openUpvalues_ 移除条目</source>
        <translation type="finished">Stack:stack slot pop,Upvalue.value from Stack copy to the heap,openUpvalues_ remove entry</translation>
    </message>
    <message>
        <source>栈：逐帧弹出，局部变量销毁，帧计数器递减</source>
        <translation type="finished">Stack:frame by frame pop,local variable destroy,frame counter decrement</translation>
    </message>
    <message>
        <source>栈：逐帧搜索，未匹配的帧被标记为待展开</source>
        <translation type="finished">Stack:frame by frame search,not yet match frame mark as pending expansion</translation>
    </message>
    <message>
        <source>树遍历解释器</source>
        <translation type="finished">tree-traversal interpreter</translation>
    </message>
    <message>
        <source>样例代码：</source>
        <translation type="finished">Sample code:</translation>
    </message>
    <message>
        <source>根因：Formatter 的空行决策必须考虑 ExportStmt 包装的内部声明节点类型。</source>
        <translation type="finished">Root cause:Formatter blank line decision must consider ExportStmt wrap internal declaration node type。</translation>
    </message>
    <message>
        <source>根因：IR 路径语句上下文必须用 visitStatement 统一包装器处理 POP，不能直接调用 visitNode。</source>
        <translation type="finished">Root cause:IR path Statement on below text must use visitStatement unified wrapper processing POP,not directly call visitNode。</translation>
    </message>
    <message>
        <source>根因：RegisterVM stepCallback_ 在&#x27;直接 return&#x27;路径被跳过，调试器单步模式丢失步进事件。</source>
        <translation type="finished">Root cause:RegisterVM stepCallback_ in&#x27;directly return&#x27;path skip,The debugger single-step mode loses step events.</translation>
    </message>
    <message>
        <source>根因：Value::equals() 是数值相等性而非类型严格相等。常量池去重需要类型区分的场景必须</source>
        <translation type="finished">Root cause:Value::equals() is numeric equality rather than strict type equality。constant pool deduplication needs type distinction scenario must</translation>
    </message>
    <message>
        <source>根因：resolveUpvalue 惰性策略对 3+ 层嵌套闭包失效，中间函数需要预先声明捕获哪些外层变量。</source>
        <translation type="finished">Root cause:resolveUpvalue lazy strategy for 3+ layers nested closure invalid,middle function needs pre-declare capture which outer variable。</translation>
    </message>
    <message>
        <source>根因：三后端必须对默认参数求值能力一致。修复：Parser 层拒绝复杂表达式，仅允许字面量 + 负数字面量。</source>
        <translation type="finished">Root cause:three backends must for default parameter evaluate consistent capability。fix:Parser layer reject complex expression,only allows literal + negative literal。</translation>
    </message>
    <message>
        <source>根因：修改整体替换语义指令时必须同步审计 visitIndexAssign、visitMemberAssign、visitMethodCall 三个 emit 点，</source>
        <translation type="finished">Root cause:Modify whole-replacement semantics instruction when must synchronously audit visitIndexAssign,visitMemberAssign,visitMethodCall three emit point,</translation>
    </message>
    <message>
        <source>根因：调用栈帧的 line 字段需要随执行进度更新，不能只在调用时初始化。</source>
        <translation type="finished">Root cause:Call stack frame line Field needs update with execution progress,not only at initialized at call time。</translation>
    </message>
    <message>
        <source>根因：路径安全校验需拒绝所有 &#x27;X:&#x27; 开头形式，不能仅检测 &#x27;X:/&#x27;。</source>
        <translation type="finished">Root cause:path safety validation needs reject all &#x27;X:&#x27; opening form,not only detect &#x27;X:/&#x27;。</translation>
    </message>
    <message>
        <source>根因：路径规范化必须在所有访问 moduleCache_ 的入口一致。</source>
        <translation type="finished">Root cause:path normalization must in all access moduleCache_ entry consistent。</translation>
    </message>
    <message>
        <source>格式化</source>
        <translation type="finished">Format</translation>
    </message>
    <message>
        <source>格式化代码</source>
        <translation type="finished">Format code</translation>
    </message>
    <message>
        <source>格式化代码 (Ctrl+Shift+F)</source>
        <translation type="finished">Format code (Ctrl+Shift+F)</translation>
    </message>
    <message>
        <source>模块导入</source>
        <translation type="finished">Module import</translation>
    </message>
    <message>
        <source>模块系统。import/export 限制在顶层作用域（Parser 在 statement() 入口显式拒绝）。</source>
        <translation type="finished">Module system. import/export is restricted to the top-level scope (the Parser explicitly rejects it at the statement() entry).</translation>
    </message>
    <message>
        <source>模块路径安全</source>
        <translation type="finished">Module path safety</translation>
    </message>
    <message>
        <source>模块路径非空校验，路径遍历防护（拒绝 .. 父目录引用和绝对路径）。</source>
        <translation type="finished">Module path non-empty validation and path-traversal protection (rejecting.. parent references and absolute paths).</translation>
    </message>
    <message>
        <source>模块路径：&#x27;C:foo&#x27; 漏网</source>
        <translation type="finished">Module path: &#x27;C:foo&#x27; slips through</translation>
    </message>
    <message>
        <source>此时上一轮残留的循环容器 refCount&gt;0 仍 aliveSet_，本轮新建容器尚未注册，安全。</source>
        <translation type="finished">At this point, leftover cyclic containers from the previous round with refCount&gt;0 are still in aliveSet_, while newly created containers this round are not yet registered, so it is safe.</translation>
    </message>
    <message>
        <source>步入</source>
        <translation type="finished">Step into</translation>
    </message>
    <message>
        <source>步骤: %1 | 光标: 行 %2, 列 %3</source>
        <translation type="finished">Step: %1 | Cursor: Line %2, Column %3</translation>
    </message>
    <message>
        <source>步骤: 源码 | 光标: 行 1, 列 1</source>
        <translation type="finished">Step: Source | Cursor: Line 1, Column 1</translation>
    </message>
    <message>
        <source>死代码消除（未使用的赋值）</source>
        <translation type="finished">Dead-code elimination (unused assignment)</translation>
    </message>
    <message>
        <source>每一层 countdown() 都没有 try/catch，帧依次被弹出。</source>
        <translation type="finished">every one layer countdown() all no try/catch,frames are popped in turn。</translation>
    </message>
    <message>
        <source>每一层递归帧都会被检查是否有 catch 块。</source>
        <translation type="finished">every one layer recursion frame all will check whether has catch block。</translation>
    </message>
    <message>
        <source>每一帧检查是否存在 try/catch 结构，若存在则跳转到 catch 块。</source>
        <translation type="finished">every one frame check whether exist in try/catch structure,if exist in then jump to catch block。</translation>
    </message>
    <message>
        <source>每个帧在栈展开时被弹出，局部变量被销毁。</source>
        <translation type="finished">each frame in stack unwinding when pop,local variable destroy。</translation>
    </message>
    <message>
        <source>每次调用 acc(x) 时，闭包通过 upvalue 修改堆上的 total。</source>
        <translation type="finished">Each time acc(x) is called, the closure modifies the heap-allocated total via its upvalue.</translation>
    </message>
    <message>
        <source>每次调用 counter() 时，闭包通过 upvalue 访问并修改堆上的 x。</source>
        <translation type="finished">Each time counter() is called, the closure accesses and modifies the heap-allocated x via its upvalue.</translation>
    </message>
    <message>
        <source>每隔 100 次迭代暂停一次。常用于在大循环中观察周期性状态，</source>
        <translation type="finished">Pause once every 100 iterations. Commonly used to observe periodic state in large loops, </translation>
    </message>
    <message>
        <source>比值</source>
        <translation type="finished">Ratio</translation>
    </message>
    <message>
        <source>注册为 O(1) 操作（vector::push_back + unordered_set::insert）。</source>
        <translation type="finished">Registered as an O(1) operation (vector::push_back + unordered_set::insert).</translation>
    </message>
    <message>
        <source>浮点数直接以 IEEE 754 double 的 64 位原始位存储，</source>
        <translation type="finished">Floats are stored directly as the raw 64 bits of an IEEE 754 double, </translation>
    </message>
    <message>
        <source>消除 MOVE 后常触发 DCE 二次优化，进一步减少指令。</source>
        <translation type="finished">After MOVE elimination, a secondary DCE optimization is often triggered, further reducing instructions.</translation>
    </message>
    <message>
        <source>深度</source>
        <translation type="finished">Depth</translation>
    </message>
    <message>
        <source>深拷贝后修改不影响 a，a 仍是 [1,2,3]</source>
        <translation type="finished">After deep-copy, modification does not affect a; a is still [1,2,3]</translation>
    </message>
    <message>
        <source>清空</source>
        <translation type="finished">Clear</translation>
    </message>
    <message>
        <source>清空其子元素打破循环 → refCount 自然降为 0 → 节点析构 → onDestroyed 从 aliveSet_ 移除。</source>
        <translation type="finished">Clearing its child elements breaks the cycle → refCount naturally drops to 0 → the node is destructed → onDestroyed removes it from aliveSet_.</translation>
    </message>
    <message>
        <source>清空输出面板</source>
        <translation type="finished">Clear Output panel</translation>
    </message>
    <message>
        <source>源码</source>
        <translation type="finished">Source</translation>
    </message>
    <message>
        <source>演示 b = a 之后修改 b 触发 COW detach。</source>
        <translation type="finished">Demonstrates that after b = a, modifying b triggers a COW detach.</translation>
    </message>
    <message>
        <source>演示 var a = [1,2,3] 的构造、拷贝与释放。</source>
        <translation type="finished">Demonstrates the construction, copying, and release of var a = [1,2,3].</translation>
    </message>
    <message>
        <source>状态</source>
        <translation type="finished">Status</translation>
    </message>
    <message>
        <source>环形容器泄漏：a.append(a) 形成自环，refCount ≥ 2 永不归零。</source>
        <translation type="finished">Circular container leak: a.append(a) forms a self-loop, refCount ≥ 2 never returns to zero.</translation>
    </message>
    <message>
        <source>现代化 MiniLang 编程语言开发环境</source>
        <translation type="finished">A modern MiniLang programming language development environment</translation>
    </message>
    <message>
        <source>
理解 IR 中间表示与寄存器式 VM：AST → IR → RegBytecode → RegisterVM。


        </source>
        <translation type="finished">
Understand the IR intermediate representation and the register-based VM: AST → IR → RegBytecode → RegisterVM.


        </translation>
    </message>
    <message>
        <source>
理解 MiniLang 的内存模型：Value 8 字节 NaN-boxing，堆类型侵入式 RefCounted，数组/字典 Copy-On-Write。


        </source>
        <translation type="finished">
Understand MiniLang's memory model: 8-byte NaN-boxed Value, intrusive RefCounted heap types, and Copy-On-Write arrays/dictionaries.


        </translation>
    </message>
    <message>
        <source>
理解 Parser 的工作方式：手写递归下降，按优先级链解析表达式。


        </source>
        <translation type="finished">
Understand how the Parser works: hand-written recursive descent, parsing expressions by precedence chain.


        </translation>
    </message>
    <message>
        <source>
理解三后端一致性约束：同一 MiniLang 源码在 Interpreter / StackVM / RegisterVM 三条路径上必须产生相同语义结果。


        </source>
        <translation type="finished">
Understand the three-backend consistency constraint: the same MiniLang source must produce identical semantic results on the three paths Interpreter / StackVM / RegisterVM.


        </translation>
    </message>
    <message>
        <source>
理解字节码编译与栈式 VM 执行：AST → BytecodeChunk → VM。


        </source>
        <translation type="finished">
Understand bytecode compilation and stack-VM execution: AST → BytecodeChunk → VM.


        </translation>
    </message>
    <message>
        <source>
理解解释器的执行模型：通过 Visitor 模式递归遍历 AST 节点求值。


        </source>
        <translation type="finished">
Understand the interpreter's execution model: recursively traversing AST nodes for evaluation via the Visitor pattern.


        </translation>
    </message>
    <message>
        <source>
理解词法分析器的职责：将源代码字符串切分为有意义的 Token 序列，并分离注释。


        </source>
        <translation type="finished">
Understand the lexer's responsibility: splitting the source string into a meaningful token sequence and separating comments.


        </translation>
    </message>
    <message>
        <source>用法: reload &quot;模块路径&quot; 或 reload all</source>
        <translation type="finished">Usage: reload &quot;module path&quot; or reload all</translation>
    </message>
    <message>
        <source>相互递归 isEven/isOdd</source>
        <translation type="finished">mutual recursion isEven/isOdd</translation>
    </message>
    <message>
        <source>相互递归：栈帧交替出现 isEven / isOdd，每帧 n 递减。注意 isOdd 在 isEven 之后定义但能被调用（前向引用通过 preScanModuleGlobals 修复）。</source>
        <translation type="finished">Mutual recursion: stack frames alternate between isEven / isOdd, with n decreasing each frame. Note that isOdd is defined after isEven but can still be called (forward references are fixed via preScanModuleGlobals).</translation>
    </message>
    <message>
        <source>确定</source>
        <translation type="finished">OK</translation>
    </message>
    <message>
        <source>确定删除 %1 ？</source>
        <translation type="finished">Confirm delete %1?</translation>
    </message>
    <message>
        <source>确认删除</source>
        <translation type="finished">Confirm delete</translation>
    </message>
    <message>
        <source>移除条件</source>
        <translation type="finished">Remove condition</translation>
    </message>
    <message>
        <source>程序无响应，即将强制终止</source>
        <translation type="finished">Program is not responding; about to force-terminate</translation>
    </message>
    <message>
        <source>程序正在运行，请先停止后再使用 REPL</source>
        <translation type="finished">A program is running; please stop it before using the REPL</translation>
    </message>
    <message>
        <source>程序正在运行，请先停止后再使用 reload</source>
        <translation type="finished">A program is running; please stop it before using reload</translation>
    </message>
    <message>
        <source>程序运行中，REPL 已暂停</source>
        <translation type="finished">Program is running; REPL is paused</translation>
    </message>
    <message>
        <source>立即调用函数表达式（IIFE）</source>
        <translation type="finished">Immediately Invoked Function Expression (IIFE)</translation>
    </message>
    <message>
        <source>等待执行...</source>
        <translation type="finished">Waiting to execute...</translation>
    </message>
    <message>
        <source>答案已显示</source>
        <translation type="finished">Answer shown</translation>
    </message>
    <message>
        <source>简单 try/catch（单层捕获）</source>
        <translation type="finished">Simple try/catch (single-level capture)</translation>
    </message>
    <message>
        <source>简单函数调用</source>
        <translation type="finished">Simple function call</translation>
    </message>
    <message>
        <source>简单相等条件（i == 50）</source>
        <translation type="finished">Simple equality condition (i == 50)</translation>
    </message>
    <message>
        <source>简单闭包（捕获单个变量）</source>
        <translation type="finished">Simple closure (captures a single variable)</translation>
    </message>
    <message>
        <source>类与继承</source>
        <translation type="finished">Classes and inheritance</translation>
    </message>
    <message>
        <source>类型</source>
        <translation type="finished">Type</translation>
    </message>
    <message>
        <source>类声明。extends 后父类名为字符串运行时查找。</source>
        <translation type="finished">Class declaration. The parent class name after extends is looked up at runtime as a string.</translation>
    </message>
    <message>
        <source>类定义不创建实例，仅注册到 classRegistry_</source>
        <translation type="finished">A class definition does not create an instance; it is only registered in classRegistry_</translation>
    </message>
    <message>
        <source>类实例化循环（10000 次）</source>
        <translation type="finished">Class instantiation loop (10000 times)</translation>
    </message>
    <message>
        <source>类方法 Point.new()</source>
        <translation type="finished">Class method Point.new()</translation>
    </message>
    <message>
        <source>类方法分派</source>
        <translation type="finished">Class method dispatch</translation>
    </message>
    <message>
        <source>类方法自动预留 slot 0 给 this，字段按继承链展平存储。</source>
        <translation type="finished">Class methods automatically reserve slot 0 for this; fields are stored flattened along the inheritance chain.</translation>
    </message>
    <message>
        <source>类构造：弹出 N 个参数 + 类模板，创建 InstanceData 并调用 init 方法。</source>
        <translation type="finished">Class construction: pops N arguments + the class template, creates an InstanceData, and calls the init method.</translation>
    </message>
    <message>
        <source>纯算术循环。栈式 VM 与 RegisterVM 在热路径上优势最明显——单条 OP_ADD 比访问者模式 dispatch 快 5-10 倍。</source>
        <translation type="finished">Pure arithmetic loop. The stack VM and RegisterVM show the most obvious advantage on hot paths — a single OP_ADD is 5-10x faster than visitor-mode dispatch.</translation>
    </message>
    <message>
        <source>经典计数器模式。闭包通过 upvalue 修改外层变量 count，</source>
        <translation type="finished">The classic counter pattern. The closure modifies the outer variable count via its upvalue, </translation>
    </message>
    <message>
        <source>继续</source>
        <translation type="finished">Continue</translation>
    </message>
    <message>
        <source>继续执行发生未知异常</source>
        <translation type="finished">Unknown exception occurred while continuing execution</translation>
    </message>
    <message>
        <source>继续执行失败: %1</source>
        <translation type="finished">Continue execution failed: %1</translation>
    </message>
    <message>
        <source>继续运行到下一个断点 (F9)</source>
        <translation type="finished">Continue to the next breakpoint (F9)</translation>
    </message>
    <message>
        <source>编码了 upvalue 数量与每个 upvalue 的位置（栈槽或上层 upvalue）。</source>
        <translation type="finished">It encodes the number of upvalues and each upvalue's location (stack slot or upper-layer upvalue).</translation>
    </message>
    <message>
        <source>编译分析</source>
        <translation type="finished">Compilation analysis</translation>
    </message>
    <message>
        <source>编译分析面板</source>
        <translation type="finished">Compilation analysis panel</translation>
    </message>
    <message>
        <source>编译管线</source>
        <translation type="finished">Compilation pipeline</translation>
    </message>
    <message>
        <source>编译管线可视化</source>
        <translation type="finished">Compilation pipeline visualization</translation>
    </message>
    <message>
        <source>编辑</source>
        <translation type="finished">Edit</translation>
    </message>
    <message>
        <source>而非直接指向 outer 的栈槽。这形成 upvalue 链。</source>
        <translation type="finished">rather than directly points to outer stack slot。this forming an upvalue chain。</translation>
    </message>
    <message>
        <source>
节点: %1
位置: %2:%3
子节点: %4
        </source>
        <translation type="finished">
Node: %1
location: %2:%3
child node: %4
        </translation>
    </message>
    <message>
        <source>节点行号: </source>
        <translation type="finished">Node line number: </translation>
    </message>
    <message>
        <source>若 try 块中抛出异常，catch 块执行清理逻辑。</source>
        <translation type="finished">if try block in throw exception,catch block perform cleanup logic。</translation>
    </message>
    <message>
        <source>若 upvalue 指向栈槽，VM 在 openUpvalues_ 中查找或创建条目。</source>
        <translation type="finished">if upvalue points to stack slot,VM in openUpvalues_ in Find or create entry。</translation>
    </message>
    <message>
        <source>若 v 超出范围（|v| ≥ 2^47）则触发装箱为 BoxedIntData* 堆对象。</source>
        <translation type="finished">if v exceeds range(|v| ≥ 2^47)then trigger boxing is BoxedIntData* Heap object。</translation>
    </message>
    <message>
        <source>若内层不匹配（或再次抛出），异常继续传播到外层 catch。</source>
        <translation type="finished">if inner not match(or again throw),Exception Continue propagate to outer catch。</translation>
    </message>
    <message>
        <source>若常量池将 0 与 0.0 共享，可能让某一后端走错算术分支。</source>
        <translation type="finished">if constant pool 0 and 0.0 share,may some one after end goes wrong arithmetic branch。</translation>
    </message>
    <message>
        <source>若异常未被捕获，传播到顶层导致程序终止，设置错误标志。</source>
        <translation type="finished">if Exception not yet capture,propagate to top level causing program termination,Settings error flag。</translation>
    </message>
    <message>
        <source>若所有层都没有 catch，异常传播到顶层。</source>
        <translation type="finished">if all layer all no catch,Exception propagate to top level。</translation>
    </message>
    <message>
        <source>若无异常，catch 块不执行（需在 try 块末尾也放清理代码）。</source>
        <translation type="finished">if None Exception,catch block not executed(needs in try block end also put cleanup code)。</translation>
    </message>
    <message>
        <source>若标准差 &gt; 10% 平均值，可能是 GC 周期或系统调度影响。&lt;/small&gt;&lt;/p&gt;</source>
        <translation type="finished">if standard deviation &gt; 10% average Value,may be GC cycle or system scheduling affect。&lt;/small&gt;&lt;/p&gt;</translation>
    </message>
    <message>
        <source>若需要每个闭包捕获不同的值，需在每次迭代中创建新作用域</source>
        <translation type="finished">if needs each Closure capture different Value,needs in every iterations in create a new scope</translation>
    </message>
    <message>
        <source>菜单</source>
        <translation type="finished">menu</translation>
    </message>
    <message>
        <source>行</source>
        <translation type="finished">Line</translation>
    </message>
    <message>
        <source>行 %1</source>
        <translation type="finished">Line %1</translation>
    </message>
    <message>
        <source>行 %1 的条件表达式（为空则变为无条件断点）: mlTr(</source>
        <translation type="finished">Line %1 conditional expression(is empty then become None conditional breakpoint): mlTr(</translation>
    </message>
    <message>
        <source>行 1</source>
        <translation type="finished">Line 1</translation>
    </message>
    <message>
        <source>行号</source>
        <translation type="finished">line number</translation>
    </message>
    <message>
        <source>视图</source>
        <translation type="finished">view</translation>
    </message>
    <message>
        <source>解析失败：%1</source>
        <translation type="finished">parse fail:%1</translation>
    </message>
    <message>
        <source>解析异常</source>
        <translation type="finished">parse Exception</translation>
    </message>
    <message>
        <source>解码时通过符号位扩展恢复 int64 值。</source>
        <translation type="finished">solve code via symbol bits extend restore int64 Value。</translation>
    </message>
    <message>
        <source>解释器未初始化</source>
        <translation type="finished">Interpreter uninitialized</translation>
    </message>
    <message>
        <source>计数器模式（通过闭包修改外层变量）</source>
        <translation type="finished">Counter pattern (modifying an outer variable via a closure)</translation>
    </message>
    <message>
        <source>设置断点条件</source>
        <translation type="finished">Set breakpoint condition</translation>
    </message>
    <message>
        <source>设置条件... (行 %1)</source>
        <translation type="finished">Set condition... (line %1)</translation>
    </message>
    <message>
        <source>访问阶段：闭包调用时（OP_CALL），VM 通过 OP_GET_UPVALUE / OP_SET_UPVALUE </source>
        <translation type="finished">access phase:Closure call when(OP_CALL),VM via OP_GET_UPVALUE / OP_SET_UPVALUE </translation>
    </message>
    <message>
        <source>词法Token</source>
        <translation type="finished">Lexical token</translation>
    </message>
    <message>
        <source>词法分析异常</source>
        <translation type="finished">Lexical analysis exception</translation>
    </message>
    <message>
        <source>词法异常</source>
        <translation type="finished">Lexical exception</translation>
    </message>
    <message>
        <source>词法错误 (行 %1, 列 %2): %3</source>
        <translation type="finished">Lexical error (line %1, column %2): %3</translation>
    </message>
    <message>
        <source>词法错误: %1</source>
        <translation type="finished">Lexical error: %1</translation>
    </message>
    <message>
        <source>词素</source>
        <translation type="finished">Lexeme</translation>
    </message>
    <message>
        <source>语法探索器</source>
        <translation type="finished">Syntax explorer</translation>
    </message>
    <message>
        <source>语法示例</source>
        <translation type="finished">Syntax examples</translation>
    </message>
    <message>
        <source>语法错误 (行 %1, 列 %2): %3</source>
        <translation type="finished">Syntax error (line %1, column %2): %3</translation>
    </message>
    <message>
        <source>说明：</source>
        <translation type="finished">Note:</translation>
    </message>
    <message>
        <source>请先在主编辑器中输入并编译代码</source>
        <translation type="finished">Please enter and compile code in the main editor first</translation>
    </message>
    <message>
        <source>请先打开或新建一个文件再进行编译分析。</source>
        <translation type="finished">Please open or create a file before running compilation analysis.</translation>
    </message>
    <message>
        <source>请先选择产生式</source>
        <translation type="finished">Please select a production first</translation>
    </message>
    <message>
        <source>请先选择场景</source>
        <translation type="finished">Please select a scenario first</translation>
    </message>
    <message>
        <source>请先选择章节</source>
        <translation type="finished">Please select a chapter first</translation>
    </message>
    <message>
        <source>请先选择题目</source>
        <translation type="finished">Please select a problem first</translation>
    </message>
    <message>
        <source>请考虑简化代码或使用字节码视图查看。</source>
        <translation type="finished">Please consider simplifying the code or viewing it via the bytecode view.</translation>
    </message>
    <message>
        <source>请选择产生式</source>
        <translation type="finished">Please select a production</translation>
    </message>
    <message>
        <source>请选择优化 pass</source>
        <translation type="finished">Please select an optimization pass</translation>
    </message>
    <message>
        <source>请选择场景</source>
        <translation type="finished">Please select a scenario</translation>
    </message>
    <message>
        <source>请选择章节</source>
        <translation type="finished">Please select a chapter</translation>
    </message>
    <message>
        <source>请选择题目</source>
        <translation type="finished">Please select a problem</translation>
    </message>
    <message>
        <source>读取全局变量。比 OP_GET_LOCAL 稍慢（需查 hash 表）。</source>
        <translation type="finished">Reads a global variable. Slightly slower than OP_GET_LOCAL (requires a hash-table lookup).</translation>
    </message>
    <message>
        <source>读取全局槽位的值并压栈。slot 在编译期由 GlobalSlotAllocator 分配。</source>
        <translation type="finished">Reads the value of a global slot and pushes it. The slot is allocated at compile time by GlobalSlotAllocator.</translation>
    </message>
    <message>
        <source>读取局部变量槽位。在 StackVM 中是热点——每次变量引用都触发一次栈读取。</source>
        <translation type="finished">Reads a local-variable slot. A hot spot in the StackVM — every variable reference triggers a stack read.</translation>
    </message>
    <message>
        <source>读取闭包捕获变量。比 OP_GET_LOCAL 稍慢（需通过 upvalue 链间接访问）。</source>
        <translation type="finished">Reads a closure-captured variable. Slightly slower than OP_GET_LOCAL (indirect access via the upvalue chain).</translation>
    </message>
    <message>
        <source>读取闭包捕获的外层变量。若 upvalue 仍开放则从栈帧读取，已关闭则从堆读取。</source>
        <translation type="finished">Reads an outer variable captured by the closure. If the upvalue is still open, it reads from the stack frame; if closed, from the heap.</translation>
    </message>
    <message>
        <source>调用栈</source>
        <translation type="finished">Call stack</translation>
    </message>
    <message>
        <source>调用栈回调发生未知异常</source>
        <translation type="finished">Unknown exception occurred in the call-stack callback</translation>
    </message>
    <message>
        <source>调用栈回调异常: </source>
        <translation type="finished">Call-stack callback exception: </translation>
    </message>
    <message>
        <source>调用栈顶帧行号：调用点 vs 当前行</source>
        <translation type="finished">Top call-stack frame line number: call site vs current line</translation>
    </message>
    <message>
        <source>调用栈顶闭包：弹出 N 个参数 + 1 个闭包值，执行后压入返回值。</source>
        <translation type="finished">Top call-stack closure: pops N arguments + 1 closure value, and pushes the return value after execution.</translation>
    </message>
    <message>
        <source>调试</source>
        <translation type="finished">Debug</translation>
    </message>
    <message>
        <source>调试中</source>
        <translation type="finished">Debugging</translation>
    </message>
    <message>
        <source>调试信息更新发生未知异常</source>
        <translation type="finished">Unknown exception occurred while updating debug info</translation>
    </message>
    <message>
        <source>调试信息更新失败: %1</source>
        <translation type="finished">Debug information update fail: %1</translation>
    </message>
    <message>
        <source>调试器 / 调用栈显示</source>
        <translation type="finished">Debugger / call-stack display</translation>
    </message>
    <message>
        <source>调试器单步模式丢失步进事件。</source>
        <translation type="finished">The debugger single-step mode loses step events.</translation>
    </message>
    <message>
        <source>调试器暂停时，调用栈顶显示的是调用点行号，而非当前执行行号，导致用户困惑。</source>
        <translation type="finished">When the debugger pauses, the top of the call stack shows the call-site line number instead of the currently executing line, confusing the user.</translation>
    </message>
    <message>
        <source>调试程序</source>
        <translation type="finished">Debug program</translation>
    </message>
    <message>
        <source>调试面板</source>
        <translation type="finished">Debug panel</translation>
    </message>
    <message>
        <source>资源清理（如关闭文件、释放锁）应放在 catch 块中确保执行。</source>
        <translation type="finished">Resource cleanup (e.g., closing files, releasing locks) should be placed in a catch block to ensure execution.</translation>
    </message>
    <message>
        <source>资源管理器</source>
        <translation type="finished">Resource manager</translation>
    </message>
    <message>
        <source>跨出</source>
        <translation type="finished">Step out</translation>
    </message>
    <message>
        <source>跨函数异常传播</source>
        <translation type="finished">Cross-function exception propagation</translation>
    </message>
    <message>
        <source>跨过</source>
        <translation type="finished">Step over</translation>
    </message>
    <message>
        <source>输入 &#x27;help&#x27; 查看帮助，输入 &#x27;clear&#x27; 清空输出。</source>
        <translation type="finished">Type &#x27;help&#x27; for help, type &#x27;clear&#x27; to clear the output.</translation>
    </message>
    <message>
        <source>输入 MiniLang 表达式或语句，按回车执行。</source>
        <translation type="finished">Enter a MiniLang expression or statement and press Enter to execute.</translation>
    </message>
    <message>
        <source>输出</source>
        <translation type="finished">Output</translation>
    </message>
    <message>
        <source>输出面板</source>
        <translation type="finished">Output panel</translation>
    </message>
    <message>
        <source>运算符与短路求值</source>
        <translation type="finished">Operators and short-circuit evaluation</translation>
    </message>
    <message>
        <source>运算符优先级链。整数除法截断向零（三后端统一）。</source>
        <translation type="finished">Operator precedence chain. Integer division truncates toward zero (unified across the three backends).</translation>
    </message>
    <message>
        <source>运行</source>
        <translation type="finished">Run</translation>
    </message>
    <message>
        <source>运行三后端对比</source>
        <translation type="finished">Run three-backend comparison</translation>
    </message>
    <message>
        <source>运行中</source>
        <translation type="finished">Running</translation>
    </message>
    <message>
        <source>运行中...</source>
        <translation type="finished">Running...</translation>
    </message>
    <message>
        <source>运行剖析</source>
        <translation type="finished">Run profiling</translation>
    </message>
    <message>
        <source>运行样例</source>
        <translation type="finished">Run sample</translation>
    </message>
    <message>
        <source>运行程序</source>
        <translation type="finished">Run program</translation>
    </message>
    <message>
        <source>运行程序 (F5)</source>
        <translation type="finished">Run program (F5)</translation>
    </message>
    <message>
        <source>运行结束</source>
        <translation type="finished">Run finished</translation>
    </message>
    <message>
        <source>运行结果：</source>
        <translation type="finished">Run result:</translation>
    </message>
    <message>
        <source>运行验证</source>
        <translation type="finished">Run verification</translation>
    </message>
    <message>
        <source>返回值若被使用则存入 vreg，否则由 CALL_POP 弃用。</source>
        <translation type="finished">If the return value is used, it is stored in a vreg; otherwise it is discarded by CALL_POP.</translation>
    </message>
    <message>
        <source>返回的闭包携带捕获的 upvalue，即使定义作用域已销毁。</source>
        <translation type="finished">The returned closure carries the captured upvalues even after the defining scope is destroyed.</translation>
    </message>
    <message>
        <source>这实现了闭包对外层变量的透明访问。</source>
        <translation type="finished">This implements transparent access to outer variables from a closure.</translation>
    </message>
    <message>
        <source>这展示了异常传播的栈展开机制——每帧的局部变量都被销毁。</source>
        <translation type="finished">This demonstrates the stack-unwinding mechanism of exception propagation — every frame's local variables are destroyed.</translation>
    </message>
    <message>
        <source>这展示了异常对象可以是结构化数据，而非仅字符串。</source>
        <translation type="finished">This demonstrates that the exception object can be structured data, not just a string.</translation>
    </message>
    <message>
        <source>这展示了异常的转换与传播链。</source>
        <translation type="finished">This demonstrates the conversion and propagation chain of exceptions.</translation>
    </message>
    <message>
        <source>这展示了通过 IIFE 创建独立 upvalue 的模式。</source>
        <translation type="finished">This demonstrates the pattern of creating independent upvalues via IIFE.</translation>
    </message>
    <message>
        <source>这展示了闭包共享 upvalue 的语义。</source>
        <translation type="finished">This demonstrates the semantics of closures sharing an upvalue.</translation>
    </message>
    <message>
        <source>这展示了闭包实现私有状态的机制——每个闭包实例独立维护状态。</source>
        <translation type="finished">This demonstrates the mechanism by which closures implement private state — each closure instance maintains its own state independently.</translation>
    </message>
    <message>
        <source>这展示了闭包的&quot;状态保持&quot;能力——变量在闭包之间共享。</source>
        <translation type="finished">This demonstrates the closure's "state-retention" ability — variables are shared among closures.</translation>
    </message>
    <message>
        <source>这展示了闭包的多 upvalue 机制。</source>
        <translation type="finished">This demonstrates the closure's multi-upvalue mechanism.</translation>
    </message>
    <message>
        <source>这展示了闭包逃逸——闭包超出定义作用域后仍可访问捕获的变量。</source>
        <translation type="finished">This demonstrates closure escape — the closure can still access captured variables after leaving its defining scope.</translation>
    </message>
    <message>
        <source>这展示了闭包逃逸后 upvalue 的堆化与 GC 机制。</source>
        <translation type="finished">This demonstrates the heapification and GC mechanism of upvalues after a closure escapes.</translation>
    </message>
    <message>
        <source>这是异常处理最常见的实际使用模式。</source>
        <translation type="finished">This is the most common real-world usage pattern of exception handling.</translation>
    </message>
    <message>
        <source>这是闭包&quot;逃逸&quot;的最常见形式——闭包超出定义作用域存活。</source>
        <translation type="finished">This is the most common form of closure "escape" — the closure outlives its defining scope.</translation>
    </message>
    <message>
        <source>这是闭包实现封装的核心模式。</source>
        <translation type="finished">This is the core pattern for encapsulation via closures.</translation>
    </message>
    <message>
        <source>这破坏了三后端语义一致性——整数除法 7/2 应为 3，浮点除法 7.0/2 应为 3.5，</source>
        <translation type="finished">This breaks three-backend semantic consistency — integer division 7/2 should be 3, float division 7.0/2 should be 3.5, </translation>
    </message>
    <message>
        <source>这解决了闭包数组中共享循环变量的问题。</source>
        <translation type="finished">This solves the problem of sharing a loop variable among closures in an array.</translation>
    </message>
    <message>
        <source>迭代 tracked_ 列表，对 aliveSet_ 仍在但 marked 未标记的节点（即不可达的循环孤岛）执行：</source>
        <translation type="finished">Iterate the tracked_ list and, for nodes still in aliveSet_ but unmarked (i.e., unreachable cycle islands), execute:</translation>
    </message>
    <message>
        <source>适用于 int/float 加减乘除、布尔逻辑、字符串拼接等。</source>
        <translation type="finished">Applies to int/float arithmetic, boolean logic, string concatenation, etc.</translation>
    </message>
    <message>
        <source>逃逸的闭包通过 upvalue 访问已堆化的变量。</source>
        <translation type="finished">The escaped closure accesses heapified variables via upvalues.</translation>
    </message>
    <message>
        <source>逐帧向上直到找到 catch 块或传播到顶层。</source>
        <translation type="finished">Goes up frame by frame until a catch block is found or it propagates to the top level.</translation>
    </message>
    <message>
        <source>递归中的异常传播</source>
        <translation type="finished">Exception propagation in recursion</translation>
    </message>
    <message>
        <source>递归型 fib(20)。栈式 VM 与 RegisterVM 在密集函数调用场景下都显著快于 Interpreter（解释器每个 AST 节点都需要虚函数分发）。</source>
        <translation type="finished">Recursive fib(20). Both the stack VM and RegisterVM are significantly faster than the Interpreter in dense call scenarios (the interpreter requires virtual dispatch for every AST node).</translation>
    </message>
    <message>
        <source>递归调用中抛出异常，异常沿递归链向上传播。</source>
        <translation type="finished">An exception thrown during a recursive call propagates up the recursion chain.</translation>
    </message>
    <message>
        <source>递归调用栈：每一帧持有独立的 n 值，递归返回时帧逐层弹出。栈深度等于递归深度，过深会触发 MAX_RECURSION_DEPTH=256 保护。</source>
        <translation type="finished">Recursive call stack: each frame holds an independent n value; frames pop layer by layer on return. The stack depth equals the recursion depth; going too deep triggers the MAX_RECURSION_DEPTH=256 safeguard.</translation>
    </message>
    <message>
        <source>递归调用（fib）</source>
        <translation type="finished">Recursive call (fib)</translation>
    </message>
    <message>
        <source>通过 aliveSet_ 区分存活对象与已释放的悬垂指针，避免迭代时访问已释放内存。</source>
        <translation type="finished">aliveSet_ distinguishes live objects from freed dangling pointers, avoiding access to freed memory during iteration.</translation>
    </message>
    <message>
        <source>
通过项目自身沉淀的真实 Bug 训练调试能力：阅读代码、预测行为、定位根因。


        </source>
        <translation type="finished">
Train debugging skills through real bugs accumulated by the project itself: read code, predict behavior, and locate root causes.


        </translation>
    </message>
    <message>
        <source>避免每次迭代都暂停导致调试效率低下。</source>
        <translation type="finished">Avoid pausing on every iteration, which would make debugging inefficient.</translation>
    </message>
    <message>
        <source>避免遗漏推/不推索引导致栈泄漏。修复：删除 L1953-1960 推索引代码对齐 visitMethodCall。</source>
        <translation type="finished">Avoid stack leaks caused by missing push/no-push of the index. Fix: delete the index-push code at L1953-1960 to align with visitMethodCall.</translation>
    </message>
    <message>
        <source>重命名</source>
        <translation type="finished">Rename</translation>
    </message>
    <message>
        <source>重命名失败</source>
        <translation type="finished">Rename failed</translation>
    </message>
    <message>
        <source>重新 throw 后，内层 catch 块剩余代码不执行。</source>
        <translation type="finished">re-throw after,inner catch block remaining code not executed。</translation>
    </message>
    <message>
        <source>重新抛出后，当前 catch 块剩余代码不执行，异常沿调用栈继续传播。</source>
        <translation type="finished">re-throw after,current catch block remaining code not executed,Exception along the call stack continue propagating。</translation>
    </message>
    <message>
        <source>重新抛出（catch 中再次 throw）</source>
        <translation type="finished">re-throw(catch in again throw)</translation>
    </message>
    <message>
        <source>重新生成 IR</source>
        <translation type="finished">Regenerate IR</translation>
    </message>
    <message>
        <source>销毁阶段：当闭包不可达时（无引用指向 Closure 对象），GC 回收闭包。</source>
        <translation type="finished">destroy phase:when Closure unreachable when(no references points to Closure object),GC Collection Closure。</translation>
    </message>
    <message>
        <source>错误</source>
        <translation type="finished">Error</translation>
    </message>
    <message>
        <source>错误信息并设置 VM/Interpreter 的错误标志。</source>
        <translation type="finished">error message and Settings VM/Interpreter error flag。</translation>
    </message>
    <message>
        <source>闭包 / upvalue 捕获</source>
        <translation type="finished">Closure / upvalue capture</translation>
    </message>
    <message>
        <source>闭包 lowering：捕获的局部变量提升为 upvalue，MAKE_CLOSURE 指令携带捕获列表。</source>
        <translation type="finished">Closure lowering: captured local variables are promoted to upvalues, and the MAKE_CLOSURE instruction carries the capture list.</translation>
    </message>
    <message>
        <source>闭包 upvalue 捕获</source>
        <translation type="finished">Closure upvalue capture</translation>
    </message>
    <message>
        <source>闭包作为函数返回值是函数式编程的核心模式。</source>
        <translation type="finished">Returning a closure as a function value is a core pattern of functional programming.</translation>
    </message>
    <message>
        <source>闭包作为返回值</source>
        <translation type="finished">Closure as return value</translation>
    </message>
    <message>
        <source>闭包作用域</source>
        <translation type="finished">Closure scope</translation>
    </message>
    <message>
        <source>闭包可以捕获多个外层变量。每个捕获的变量成为独立的 upvalue，</source>
        <translation type="finished">A closure can capture multiple outer variables. Each captured variable becomes an independent upvalue, </translation>
    </message>
    <message>
        <source>闭包定义时（OP_CLOSURE 指令执行），VM 读取 upvalue 数量与每个 upvalue 的位置。</source>
        <translation type="finished">When a closure is defined (OP_CLOSURE executes), the VM reads the upvalue count and each upvalue's location.</translation>
    </message>
    <message>
        <source>闭包对象在堆上包含两个 upvalue 指针，指向堆化的 base 和 delta。</source>
        <translation type="finished">The closure object on the heap contains two upvalue pointers pointing to the heapified base and delta.</translation>
    </message>
    <message>
        <source>闭包捕获 makeCounter</source>
        <translation type="finished">Closure captures makeCounter</translation>
    </message>
    <message>
        <source>闭包捕获循环（10000 次）</source>
        <translation type="finished">Closure capture loop (10000 times)</translation>
    </message>
    <message>
        <source>闭包数组</source>
        <translation type="finished">Closure array</translation>
    </message>
    <message>
        <source>闭包检查器</source>
        <translation type="finished">Closure inspector</translation>
    </message>
    <message>
        <source>闭包的 upvalue 数组被释放，若 upvalue 引用的堆值无其他引用，</source>
        <translation type="finished">The closure's upvalue array is freed; if the heap value referenced by an upvalue has no other references, </translation>
    </message>
    <message>
        <source>闭包调用栈：闭包帧不持有 count 的本地副本，而是通过 upvalue 引用外层 makeCounter 帧的 count。makeCounter 返回后帧已弹出，闭包通过 Upvalue 对象保持对 count 的引用（堆分配）。</source>
        <translation type="finished">Closure call stack: the closure frame does not hold a local copy of count; instead it references count in the outer makeCounter frame via an upvalue. After makeCounter returns, its frame is popped, and the closure keeps its reference to count (heap-allocated) via the Upvalue object.</translation>
    </message>
    <message>
        <source>闭包逃逸指闭包超出定义作用域后仍被使用。</source>
        <translation type="finished">Closure escape means the closure is still used after leaving its defining scope.</translation>
    </message>
    <message>
        <source>闭包逃逸（超出定义作用域）</source>
        <translation type="finished">Closure escape (outlives defining scope)</translation>
    </message>
    <message>
        <source>问题</source>
        <translation type="finished">Problem</translation>
    </message>
    <message>
        <source>非法名称</source>
        <translation type="finished">Invalid name</translation>
    </message>
    <message>
        <source>非法文件名</source>
        <translation type="finished">Invalid file name</translation>
    </message>
    <message>
        <source>面板</source>
        <translation type="finished">Panel</translation>
    </message>
    <message>
        <source>顶层调用 foo()，栈只有 2 帧：main + foo。</source>
        <translation type="finished">top level call foo(),Stack only 2 frame:main + foo。</translation>
    </message>
    <message>
        <source>验证完成 — 对比期望/Bug 行为</source>
        <translation type="finished">Verification complete — compare expected/bug behavior</translation>
    </message>
    <message>
        <source>高 16 位为 INT_TAG_BASE = 0x7FF8，标记类型为 VAL_INT。</source>
        <translation type="finished">The high 16 bits are INT_TAG_BASE = 0x7FF8, tagging the type as VAL_INT.</translation>
    </message>
    <message>
        <source>高 16 位为 PTR_TAG_BASE = 0x7FFB，低 48 位为 StringData 对象的虚拟地址。</source>
        <translation type="finished">The high 16 bits are PTR_TAG_BASE = 0x7FFB; the low 48 bits are the virtual address of the StringData object.</translation>
    </message>
    <message>
        <source>高 16 位是 exponent 字段。若 float 的位模式恰好落入 NaN-boxed tag 范围，</source>
        <translation type="finished">The high 16 bits are the exponent field. If a float's bit pattern happens to fall in the NaN-boxed tag range, </translation>
    </message>
    <message>
        <source>默认参数 / 三后端一致性</source>
        <translation type="finished">Default parameters / three-backend consistency</translation>
    </message>
    <message>
        <source>默认参数表达式：三后端不一致</source>
        <translation type="finished">Default-parameter expression: three-backend inconsistency</translation>
    </message>
    <message>
        <source>（MiniLang 中 var 声明的变量在同一作用域内共享），</source>
        <translation type="finished">(In MiniLang, variables declared with var are shared within the same scope), </translation>
    </message>
    <message>
        <source>（a 离开作用域）</source>
        <translation type="finished">(a leaves scope)</translation>
    </message>
    <message>
        <source>（b 离开作用域）</source>
        <translation type="finished">(b leaves scope)</translation>
    </message>
    <message>
        <source>（q 离开作用域）</source>
        <translation type="finished">(q leaves scope)</translation>
    </message>
    <message>
        <source>（堆指针，tag bits=0x7FFB）</source>
        <translation type="finished">(heap pointer, tag bits=0x7FFB)</translation>
    </message>
    <message>
        <source>（如使用 IIFE 或块作用域）。</source>
        <translation type="finished">(e.g., using IIFE or block scope).</translation>
    </message>
    <message>
        <source>（如关闭文件、释放锁）。MiniLang 的 try/catch 不支持 finally 关键字，</source>
        <translation type="finished">(e.g., closing files, releasing locks). MiniLang's try/catch does not support the finally keyword, </translation>
    </message>
    <message>
        <source>（尚未编译，请在主编辑器输入代码并触发编译分析）</source>
        <translation type="finished">(Not compiled yet; enter code in the main editor and trigger compilation analysis)</translation>
    </message>
    <message>
        <source>（尚未解析，请先在主编辑器中输入代码）</source>
        <translation type="finished">(Not parsed yet; please enter code in the main editor first)</translation>
    </message>
    <message>
        <source>（新对象中 b[3]=4）</source>
        <translation type="finished">(in the new object b[3]=4)</translation>
    </message>
    <message>
        <source>（无 AST）</source>
        <translation type="finished">(No AST)</translation>
    </message>
    <message>
        <source>（无条件）</source>
        <translation type="finished">(unconditional)</translation>
    </message>
    <message>
        <source>（未绑定控制器）</source>
        <translation type="finished">(no controller bound)</translation>
    </message>
    <message>
        <source>（条件: </source>
        <translation type="finished">(condition: </translation>
    </message>
    <message>
        <source>（空代码）</source>
        <translation type="finished">(Empty code)</translation>
    </message>
    <message>
        <source>（而非直接指向栈槽），形成 upvalue 链。</source>
        <translation type="finished">(rather than directly pointing to a stack slot), forming an upvalue chain.</translation>
    </message>
    <message>
        <source>），视为条件不满足</source>
        <translation type="finished">)), the condition is considered not satisfied</translation>
    </message>
    <message>
        <source>🔴 VM 命中断点: 第 %1 行</source>
        <translation type="finished">🔴 VM hit Breakpoint: ordinal %1 Line</translation>
    </message>
</context>
</TS>
