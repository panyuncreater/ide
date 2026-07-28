<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE TS>
<TS version="2.1" language="zh_CN">
<!--
  MiniLang IDE 翻译文件（简体中文，源语言）。

  由 i18n 脚本自动生成。源码中所有 mlTr() 包裹的字符串均在此列出。
  由于源语言已是中文，翻译文本与原文相同。
  翻译人员请确认每条 <translation> 的内容是否正确。
-->
<context>
    <name>MiniLang</name>
    <message>
        <source>
(无函数 chunk)</source>
        <translation type="finished">
(无函数 chunk)</translation>
    </message>
    <message>
        <source>
[Interpreter 异常] </source>
        <translation type="finished">
[Interpreter 异常] </translation>
    </message>
    <message>
        <source>
[Interpreter 路径运行完成]</source>
        <translation type="finished">
[Interpreter 路径运行完成]</translation>
    </message>
    <message>
        <source>
[StackVM 异常] </source>
        <translation type="finished">
[StackVM 异常] </translation>
    </message>
    <message>
        <source>
[StackVM 路径: </source>
        <translation type="finished">
[StackVM 路径: </translation>
    </message>
    <message>
        <source>    # then 块
</source>
        <translation type="finished">    # then 块
</translation>
    </message>
    <message>
        <source>    BRANCH_FALSE v0, L1       # 跳到 else
</source>
        <translation type="finished">    BRANCH_FALSE v0, L1       # 跳到 else
</translation>
    </message>
    <message>
        <source>    BRANCH_FALSE v2, L2        # 跳出循环
</source>
        <translation type="finished">    BRANCH_FALSE v2, L2        # 跳出循环
</translation>
    </message>
    <message>
        <source>    v0 = LOAD_CONST #2 (3)        # 折叠后的常量
</source>
        <translation type="finished">    v0 = LOAD_CONST #2 (3)        # 折叠后的常量
</translation>
    </message>
    <message>
        <source>    v1 = STORE_LOCAL slot=0, v0   # x = 1 (从未使用)
</source>
        <translation type="finished">    v1 = STORE_LOCAL slot=0, v0   # x = 1 (从未使用)
</translation>
    </message>
    <message>
        <source>    v2 = ADD v0, v1           # a + 1 (替换 v1→v0，消除 MOVE)
</source>
        <translation type="finished">    v2 = ADD v0, v1           # a + 1 (替换 v1→v0，消除 MOVE)
</translation>
    </message>
    <message>
        <source>  # slot 0 = this (隐式)
</source>
        <translation type="finished">  # slot 0 = this (隐式)
</translation>
    </message>
    <message>
        <source>  # upvalue: c (来自 makeCounter 的局部)
</source>
        <translation type="finished">  # upvalue: c (来自 makeCounter 的局部)
</translation>
    </message>
    <message>
        <source>  - &#x27;clear&#x27; 仅清空输出区与续行缓冲,不重置已定义变量/函数/类
</source>
        <translation type="finished">  - &#x27;clear&#x27; 仅清空输出区与续行缓冲,不重置已定义变量/函数/类
</translation>
    </message>
    <message>
        <source>  - &#x27;reload &quot;mod&quot;&#x27; 清除模块缓存,下次 import 重新加载源码
</source>
        <translation type="finished">  - &#x27;reload &quot;mod&quot;&#x27; 清除模块缓存,下次 import 重新加载源码
</translation>
    </message>
    <message>
        <source>  - &#x27;reload all&#x27; 清除所有模块缓存
</source>
        <translation type="finished">  - &#x27;reload all&#x27; 清除所有模块缓存
</translation>
    </message>
    <message>
        <source>  - 表达式语句(如 &#x27;1 + 2;&#x27;)自动求值并打印结果
</source>
        <translation type="finished">  - 表达式语句(如 &#x27;1 + 2;&#x27;)自动求值并打印结果
</translation>
    </message>
    <message>
        <source>  - 重置全部状态需重启 IDE
</source>
        <translation type="finished">  - 重置全部状态需重启 IDE
</translation>
    </message>
    <message>
        <source>  [1, 2, 3]            数组字面量
</source>
        <translation type="finished">  [1, 2, 3]            数组字面量
</translation>
    </message>
    <message>
        <source>  class Name { ... }   类声明
</source>
        <translation type="finished">  class Name { ... }   类声明
</translation>
    </message>
    <message>
        <source>  for (init; cond; upd) { ... }  for循环
</source>
        <translation type="finished">  for (init; cond; upd) { ... }  for循环
</translation>
    </message>
    <message>
        <source>  fun f(x) { ... }     函数声明
</source>
        <translation type="finished">  fun f(x) { ... }     函数声明
</translation>
    </message>
    <message>
        <source>  if (cond) { ... }    条件语句
</source>
        <translation type="finished">  if (cond) { ... }    条件语句
</translation>
    </message>
    <message>
        <source>  import &quot;mod&quot; { f }; 模块导入
</source>
        <translation type="finished">  import &quot;mod&quot; { f }; 模块导入
</translation>
    </message>
    <message>
        <source>  int a = 5;           类型注解变量
</source>
        <translation type="finished">  int a = 5;           类型注解变量
</translation>
    </message>
    <message>
        <source>  null                 空值
</source>
        <translation type="finished">  null                 空值
</translation>
    </message>
    <message>
        <source>  print(expr);         输出
</source>
        <translation type="finished">  print(expr);         输出
</translation>
    </message>
    <message>
        <source>  var x = 10;          变量声明
</source>
        <translation type="finished">  var x = 10;          变量声明
</translation>
    </message>
    <message>
        <source>  while (cond) { ... } 循环语句
</source>
        <translation type="finished">  while (cond) { ... } 循环语句
</translation>
    </message>
    <message>
        <source>  {&quot;key&quot;: val}        字典字面量
</source>
        <translation type="finished">  {&quot;key&quot;: val}        字典字面量
</translation>
    </message>
    <message>
        <source>  多行输入: 未闭合的 { ( [ 或未闭合字符串会自动续行
</source>
        <translation type="finished">  多行输入: 未闭合的 { ( [ 或未闭合字符串会自动续行
</translation>
    </message>
    <message>
        <source>  续行中按回车(空行)可中止续行
</source>
        <translation type="finished">  续行中按回车(空行)可中止续行
</translation>
    </message>
    <message>
        <source> (行 %1</source>
        <translation type="finished"> (行 %1</translation>
    </message>
    <message>
        <source> [待运行]</source>
        <translation type="finished"> [待运行]</translation>
    </message>
    <message>
        <source> [无 AST]</source>
        <translation type="finished"> [无 AST]</translation>
    </message>
    <message>
        <source> 字节)</source>
        <translation type="finished"> 字节)</translation>
    </message>
    <message>
        <source> 字节)，超过上限 (</source>
        <translation type="finished"> 字节)，超过上限 (</translation>
    </message>
    <message>
        <source> 次迭代&lt;br&gt;&lt;hr&gt;</source>
        <translation type="finished"> 次迭代&lt;br&gt;&lt;hr&gt;</translation>
    </message>
    <message>
        <source> 次迭代平均&lt;/p&gt;</source>
        <translation type="finished"> 次迭代平均&lt;/p&gt;</translation>
    </message>
    <message>
        <source> 缓存，下次 import 将重新加载]</source>
        <translation type="finished"> 缓存，下次 import 将重新加载]</translation>
    </message>
    <message>
        <source>#</source>
        <translation type="finished">#</translation>
    </message>
    <message>
        <source># 实验 1：词法分析 — 从字符到 Token

</source>
        <translation type="finished"># 实验 1：词法分析 — 从字符到 Token

</translation>
    </message>
    <message>
        <source># 实验 2：递归下降解析 — 从 Token 到 AST

</source>
        <translation type="finished"># 实验 2：递归下降解析 — 从 Token 到 AST

</translation>
    </message>
    <message>
        <source># 实验 3：树遍历解释器 — Visitor 模式执行 AST

</source>
        <translation type="finished"># 实验 3：树遍历解释器 — Visitor 模式执行 AST

</translation>
    </message>
    <message>
        <source># 实验 4：栈式字节码 VM — 编译与执行

</source>
        <translation type="finished"># 实验 4：栈式字节码 VM — 编译与执行

</translation>
    </message>
    <message>
        <source># 实验 5：寄存器式 VM — 三地址码与 IR

</source>
        <translation type="finished"># 实验 5：寄存器式 VM — 三地址码与 IR

</translation>
    </message>
    <message>
        <source># 实验 6：三后端一致性 — 语义等价验证

</source>
        <translation type="finished"># 实验 6：三后端一致性 — 语义等价验证

</translation>
    </message>
    <message>
        <source># 实验 7：内存模型 — NaN-boxing 与 COW

</source>
        <translation type="finished"># 实验 7：内存模型 — NaN-boxing 与 COW

</translation>
    </message>
    <message>
        <source># 实验 8：Bug 狩猎 — 从历史 Bug 学习

</source>
        <translation type="finished"># 实验 8：Bug 狩猎 — 从历史 Bug 学习

</translation>
    </message>
    <message>
        <source>## 关键概念
</source>
        <translation type="finished">## 关键概念
</translation>
    </message>
    <message>
        <source>## 实验步骤
</source>
        <translation type="finished">## 实验步骤
</translation>
    </message>
    <message>
        <source>## 目标
</source>
        <translation type="finished">## 目标
</translation>
    </message>
    <message>
        <source>## 进阶
</source>
        <translation type="finished">## 进阶
</translation>
    </message>
    <message>
        <source>## 验证断言
</source>
        <translation type="finished">## 验证断言
</translation>
    </message>
    <message>
        <source>%1 [%2] %3 ms</source>
        <translation type="finished">%1 [%2] %3 ms</translation>
    </message>
    <message>
        <source>%1 — %2 → %3 条指令（减少 %4 条）</source>
        <translation type="finished">%1 — %2 → %3 条指令（减少 %4 条）</translation>
    </message>
    <message>
        <source>%1x 慢</source>
        <translation type="finished">%1x 慢</translation>
    </message>
    <message>
        <source>(无字节码)</source>
        <translation type="finished">(无字节码)</translation>
    </message>
    <message>
        <source>(无激活寄存器)</source>
        <translation type="finished">(无激活寄存器)</translation>
    </message>
    <message>
        <source>(无行号)</source>
        <translation type="finished">(无行号)</translation>
    </message>
    <message>
        <source>(未知指令)</source>
        <translation type="finished">(未知指令)</translation>
    </message>
    <message>
        <source>(栈为空)</source>
        <translation type="finished">(栈为空)</translation>
    </message>
    <message>
        <source>+ 支持字符串拼接（任一操作数为字符串即触发）。</source>
        <translation type="finished">+ 支持字符串拼接（任一操作数为字符串即触发）。</translation>
    </message>
    <message>
        <source>, 列 %1</source>
        <translation type="finished">, 列 %1</translation>
    </message>
    <message>
        <source>- **32 虚拟寄存器**：R0-R31，零堆分配（std::array&lt;Value, 32&gt;）
</source>
        <translation type="finished">- **32 虚拟寄存器**：R0-R31，零堆分配（std::array&lt;Value, 32&gt;）
</translation>
    </message>
    <message>
        <source>- **AST 节点**：33 个 NodeType，ASTNode 基类含 line/column/nodeType + accept(Visitor&amp;)

</source>
        <translation type="finished">- **AST 节点**：33 个 NodeType，ASTNode 基类含 line/column/nodeType + accept(Visitor&amp;)

</translation>
    </message>
    <message>
        <source>- **BytecodeChunk**：code（字节流）+ constants（常量池）+ lines（行号映射）
</source>
        <translation type="finished">- **BytecodeChunk**：code（字节流）+ constants（常量池）+ lines（行号映射）
</translation>
    </message>
    <message>
        <source>- **COW**：写前检查独占所有权，refCount &gt; 1 时复制
</source>
        <translation type="finished">- **COW**：写前检查独占所有权，refCount &gt; 1 时复制
</translation>
    </message>
    <message>
        <source>- **ConsistencyDiff 测试**：190 个差分测试用例验证三后端等价
</source>
        <translation type="finished">- **ConsistencyDiff 测试**：190 个差分测试用例验证三后端等价
</translation>
    </message>
    <message>
        <source>- **DebugController**：断点 / 单步 / 条件求值 / 沙箱隔离
</source>
        <translation type="finished">- **DebugController**：断点 / 单步 / 条件求值 / 沙箱隔离
</translation>
    </message>
    <message>
        <source>- **DoS 防护**：MAX_SOURCE_SIZE / MAX_TOKEN_COUNT / MAX_INTERP_DEPTH

</source>
        <translation type="finished">- **DoS 防护**：MAX_SOURCE_SIZE / MAX_TOKEN_COUNT / MAX_INTERP_DEPTH

</translation>
    </message>
    <message>
        <source>- **Environment**：链式作用域，boundInstance_ 缓存优化
</source>
        <translation type="finished">- **Environment**：链式作用域，boundInstance_ 缓存优化
</translation>
    </message>
    <message>
        <source>- **GcManager sweep**：周期性清理 refCount==0 的对象防 UAF

</source>
        <translation type="finished">- **GcManager sweep**：周期性清理 refCount==0 的对象防 UAF

</translation>
    </message>
    <message>
        <source>- **IROp**：52 个 IR 指令（LOAD_CONST/ADD/CALL 等）
</source>
        <translation type="finished">- **IROp**：52 个 IR 指令（LOAD_CONST/ADD/CALL 等）
</translation>
    </message>
    <message>
        <source>- **NaN-boxing**：8 字节 Value，tag bits / payload 分段编码
</source>
        <translation type="finished">- **NaN-boxing**：8 字节 Value，tag bits / payload 分段编码
</translation>
    </message>
    <message>
        <source>- **OpCode**：~60 个栈式操作码（OP_CONSTANT/OP_ADD/OP_CALL 等）
</source>
        <translation type="finished">- **OpCode**：~60 个栈式操作码（OP_CONSTANT/OP_ADD/OP_CALL 等）
</translation>
    </message>
    <message>
        <source>- **RAII guard 构造顺序**：必须在状态修改前构造以捕获正确的 saved 状态

</source>
        <translation type="finished">- **RAII guard 构造顺序**：必须在状态修改前构造以捕获正确的 saved 状态

</translation>
    </message>
    <message>
        <source>- **REPL**：续行判断（未闭合 {/(/[/字符串/块注释/try 缺 catch）

</source>
        <translation type="finished">- **REPL**：续行判断（未闭合 {/(/[/字符串/块注释/try 缺 catch）

</translation>
    </message>
    <message>
        <source>- **RefCounted**：侵入式引用计数基类，数组/字典/字符串/InstanceData 都继承
</source>
        <translation type="finished">- **RefCounted**：侵入式引用计数基类，数组/字典/字符串/InstanceData 都继承
</translation>
    </message>
    <message>
        <source>- **RegOp**：~50 个三地址码操作码
</source>
        <translation type="finished">- **RegOp**：~50 个三地址码操作码
</translation>
    </message>
    <message>
        <source>- **Token 类型**：关键字 / 标识符 / 字面量 / 运算符 / 分隔符
</source>
        <translation type="finished">- **Token 类型**：关键字 / 标识符 / 字面量 / 运算符 / 分隔符
</translation>
    </message>
    <message>
        <source>- **Visitor 模式**：每个 AST 节点类型对应一个 visit 方法
</source>
        <translation type="finished">- **Visitor 模式**：每个 AST 节点类型对应一个 visit 方法
</translation>
    </message>
    <message>
        <source>- **int48 内联存储**：int 在 int48 范围内联存储，超范围自动装箱
</source>
        <translation type="finished">- **int48 内联存储**：int 在 int48 范围内联存储，超范围自动装箱
</translation>
    </message>
    <message>
        <source>- **三后端一致性**：整数除法截断向零 / and-or 短路返回原值 / 类型注解强制 / super 调用语义
</source>
        <translation type="finished">- **三后端一致性**：整数除法截断向零 / and-or 短路返回原值 / 类型注解强制 / super 调用语义
</translation>
    </message>
    <message>
        <source>- **三后端对称性审计**：修改某后端时同步检查其他两个后端
</source>
        <translation type="finished">- **三后端对称性审计**：修改某后端时同步检查其他两个后端
</translation>
    </message>
    <message>
        <source>- **优先级链**：assignment → or_ → and_ → equality → comparison → term → factor → unary → call → primary
</source>
        <translation type="finished">- **优先级链**：assignment → or_ → and_ → equality → comparison → term → factor → unary → call → primary
</translation>
    </message>
    <message>
        <source>- **优化 pass**：常量折叠 / 死代码消除 / 复制传播

</source>
        <translation type="finished">- **优化 pass**：常量折叠 / 死代码消除 / 复制传播

</translation>
    </message>
    <message>
        <source>- **全局槽位**：编译期分配，运行时通过 slot 索引访问

</source>
        <translation type="finished">- **全局槽位**：编译期分配，运行时通过 slot 索引访问

</translation>
    </message>
    <message>
        <source>- **列号计算**：按 UTF-8 码位计数（columnAt）
</source>
        <translation type="finished">- **列号计算**：按 UTF-8 码位计数（columnAt）
</translation>
    </message>
    <message>
        <source>- **已知差异**：非方法上下文 super 错误消息文本不同（错误类型相同，行为一致）
</source>
        <translation type="finished">- **已知差异**：非方法上下文 super 错误消息文本不同（错误类型相同，行为一致）
</translation>
    </message>
    <message>
        <source>- **常量池去重**：类型严格匹配 + 数值相等（BUG-CP-1 修复后）
</source>
        <translation type="finished">- **常量池去重**：类型严格匹配 + 数值相等（BUG-CP-1 修复后）
</translation>
    </message>
    <message>
        <source>- **操作数栈**：定长 Value[1024]，push/pop 模型
</source>
        <translation type="finished">- **操作数栈**：定长 Value[1024]，push/pop 模型
</translation>
    </message>
    <message>
        <source>- **注释分离**：MiniLang 将 TK_LINE_COMMENT/TK_BLOCK_COMMENT 从主流分离到 lexer.comments()，供 Formatter 保留注释
</source>
        <translation type="finished">- **注释分离**：MiniLang 将 TK_LINE_COMMENT/TK_BLOCK_COMMENT 从主流分离到 lexer.comments()，供 Formatter 保留注释
</translation>
    </message>
    <message>
        <source>- **深度防护**：MAX_PARSE_DEPTH / MAX_BLOCK_DEPTH 通过 DepthGuard RAII 守卫防止 C++ 栈溢出
</source>
        <translation type="finished">- **深度防护**：MAX_PARSE_DEPTH / MAX_BLOCK_DEPTH 通过 DepthGuard RAII 守卫防止 C++ 栈溢出
</translation>
    </message>
    <message>
        <source>- **虚拟寄存器**：SSA-like，nextVReg 分配
</source>
        <translation type="finished">- **虚拟寄存器**：SSA-like，nextVReg 分配
</translation>
    </message>
    <message>
        <source>- **错误恢复**：synchronize() 在出错后跳到下一个同步点（语句边界）继续解析
</source>
        <translation type="finished">- **错误恢复**：synchronize() 在出错后跳到下一个同步点（语句边界）继续解析
</translation>
    </message>
    <message>
        <source>- **错误消息归一化**：差分时归一化错误类型而非文本

</source>
        <translation type="finished">- **错误消息归一化**：差分时归一化错误类型而非文本

</translation>
    </message>
    <message>
        <source>- **错误路径栈平衡**：每个 error return 点必须 popN 清理栈
</source>
        <translation type="finished">- **错误路径栈平衡**：每个 error return 点必须 popN 清理栈
</translation>
    </message>
    <message>
        <source>- **高频 Bug 模式**：栈不平衡 / UAF / 泄漏 / 语义不一致 / 竞态 / 错误传播 / 边界条件
</source>
        <translation type="finished">- **高频 Bug 模式**：栈不平衡 / UAF / 泄漏 / 语义不一致 / 竞态 / 错误传播 / 边界条件
</translation>
    </message>
    <message>
        <source>- + 的右子节点是 BinaryOp(*)

</source>
        <translation type="finished">- + 的右子节点是 BinaryOp(*)

</translation>
    </message>
    <message>
        <source>- AST 根节点是 Block
</source>
        <translation type="finished">- AST 根节点是 Block
</translation>
    </message>
    <message>
        <source>- IR 中应看到 vreg 分配与三地址码结构
</source>
        <translation type="finished">- IR 中应看到 vreg 分配与三地址码结构
</translation>
    </message>
    <message>
        <source>- Token 表中应有 5 个主流 token：var / x / = / 42 / ;
</source>
        <translation type="finished">- Token 表中应有 5 个主流 token：var / x / = / 42 / ;
</translation>
    </message>
    <message>
        <source>- VarDecl 的 initializer 是 BinaryOp(+)
</source>
        <translation type="finished">- VarDecl 的 initializer 是 BinaryOp(+)
</translation>
    </message>
    <message>
        <source>- a 与 b 共享同一 ArrayData 直到第一次写
</source>
        <translation type="finished">- a 与 b 共享同一 ArrayData 直到第一次写
</translation>
    </message>
    <message>
        <source>- and/or 短路返回操作数原值（非布尔）

</source>
        <translation type="finished">- and/or 短路返回操作数原值（非布尔）

</translation>
    </message>
    <message>
        <source>- print(x); 对应 4 个 token：print / ( / x / ) / ;
</source>
        <translation type="finished">- print(x); 对应 4 个 token：print / ( / x / ) / ;
</translation>
    </message>
    <message>
        <source>- 三后端都输出：3 / 3.5 / 0 / 0
</source>
        <translation type="finished">- 三后端都输出：3 / 3.5 / 0 / 0
</translation>
    </message>
    <message>
        <source>- 任何修改 b 的操作触发 COW，a 不受影响
</source>
        <translation type="finished">- 任何修改 b 的操作触发 COW，a 不受影响
</translation>
    </message>
    <message>
        <source>- 优化启用后 IR 指令数减少

</source>
        <translation type="finished">- 优化启用后 IR 指令数减少

</translation>
    </message>
    <message>
        <source>- 修改为 `var x = (1 + 2) * 3;` 观察括号如何改变 AST 结构
</source>
        <translation type="finished">- 修改为 `var x = (1 + 2) * 3;` 观察括号如何改变 AST 结构
</translation>
    </message>
    <message>
        <source>- 修改源码观察哪些情况下三后端可能不一致（如默认参数表达式）</source>
        <translation type="finished">- 修改源码观察哪些情况下三后端可能不一致（如默认参数表达式）</translation>
    </message>
    <message>
        <source>- 启用 IR 路径观察字节码差异</source>
        <translation type="finished">- 启用 IR 路径观察字节码差异</translation>
    </message>
    <message>
        <source>- 启用复制传播观察 v0/v1 是否被消除
</source>
        <translation type="finished">- 启用复制传播观察 v0/v1 是否被消除
</translation>
    </message>
    <message>
        <source>- 在 REPL 中输入多行代码观察续行判断</source>
        <translation type="finished">- 在 REPL 中输入多行代码观察续行判断</translation>
    </message>
    <message>
        <source>- 字符串拼接（+）任一操作数为字符串即触发

</source>
        <translation type="finished">- 字符串拼接（+）任一操作数为字符串即触发

</translation>
    </message>
    <message>
        <source>- 完成 10 道题后尝试在主编辑器中重现某个 Bug
</source>
        <translation type="finished">- 完成 10 道题后尝试在主编辑器中重现某个 Bug
</translation>
    </message>
    <message>
        <source>- 寄存器 VM 执行后 y == 9
</source>
        <translation type="finished">- 寄存器 VM 执行后 y == 9
</translation>
    </message>
    <message>
        <source>- 尝试 3 层嵌套闭包（参见 BUG-UV-1）
</source>
        <translation type="finished">- 尝试 3 层嵌套闭包（参见 BUG-UV-1）
</translation>
    </message>
    <message>
        <source>- 尝试嵌套左值 `a[0][1] = x;` 观察栈平衡（参见 BUG-CP-2）
</source>
        <translation type="finished">- 尝试嵌套左值 `a[0][1] = x;` 观察栈平衡（参见 BUG-CP-2）
</translation>
    </message>
    <message>
        <source>- 尝试嵌套数组 `[[1,2],[3,4]]` 观察深拷贝 vs 浅拷贝
</source>
        <translation type="finished">- 尝试嵌套数组 `[[1,2],[3,4]]` 观察深拷贝 vs 浅拷贝
</translation>
    </message>
    <message>
        <source>- 尝试死代码（如 `var unused = 5;`）观察 DCE 效果</source>
        <translation type="finished">- 尝试死代码（如 `var unused = 5;`）观察 DCE 效果</translation>
    </message>
    <message>
        <source>- 尝试触发已知差异（非方法上下文 super）观察错误消息差异
</source>
        <translation type="finished">- 尝试触发已知差异（非方法上下文 super）观察错误消息差异
</translation>
    </message>
    <message>
        <source>- 尝试输入多字节 UTF-8 字符（如 &quot;é&quot;）观察列号计数</source>
        <translation type="finished">- 尝试输入多字节 UTF-8 字符（如 &quot;é&quot;）观察列号计数</translation>
    </message>
    <message>
        <source>- 尝试输入非法字符（如 $）观察错误 token
</source>
        <translation type="finished">- 尝试输入非法字符（如 $）观察错误 token
</translation>
    </message>
    <message>
        <source>- 常量池去重：相同字面量共享索引
</source>
        <translation type="finished">- 常量池去重：相同字面量共享索引
</translation>
    </message>
    <message>
        <source>- 整数除法 7/2 三后端一致为 3
</source>
        <translation type="finished">- 整数除法 7/2 三后端一致为 3
</translation>
    </message>
    <message>
        <source>- 条件断点求值不污染程序状态（沙箱保存/恢复所有可变状态）

</source>
        <translation type="finished">- 条件断点求值不污染程序状态（沙箱保存/恢复所有可变状态）

</translation>
    </message>
    <message>
        <source>- 栈式 VM 执行后 x == 3

</source>
        <translation type="finished">- 栈式 VM 执行后 x == 3

</translation>
    </message>
    <message>
        <source>- 注释 token 不计入主流

</source>
        <translation type="finished">- 注释 token 不计入主流

</translation>
    </message>
    <message>
        <source>- 测试字符串索引 UTF-8 多字节字符（参见 BUG-INTP-3）</source>
        <translation type="finished">- 测试字符串索引 UTF-8 多字节字符（参见 BUG-INTP-3）</translation>
    </message>
    <message>
        <source>- 浮点除法 7.0/2 三后端一致为 3.5
</source>
        <translation type="finished">- 浮点除法 7.0/2 三后端一致为 3.5
</translation>
    </message>
    <message>
        <source>- 理解三后端对称性约束：修改指令布局时必须同步更新 instructionSizeAt
</source>
        <translation type="finished">- 理解三后端对称性约束：修改指令布局时必须同步更新 instructionSizeAt
</translation>
    </message>
    <message>
        <source>- 理解条件断点沙箱必须保存/恢复所有可变状态

</source>
        <translation type="finished">- 理解条件断点沙箱必须保存/恢复所有可变状态

</translation>
    </message>
    <message>
        <source>- 第一个子节点是 VarDecl
</source>
        <translation type="finished">- 第一个子节点是 VarDecl
</translation>
    </message>
    <message>
        <source>- 编译后字节码长度合理（约 6-8 字节）
</source>
        <translation type="finished">- 编译后字节码长度合理（约 6-8 字节）
</translation>
    </message>
    <message>
        <source>- 能区分 Value::equals()（数值相等）与类型严格相等
</source>
        <translation type="finished">- 能区分 Value::equals()（数值相等）与类型严格相等
</translation>
    </message>
    <message>
        <source>- 能识别&quot;push 在检查之前&quot;反模式
</source>
        <translation type="finished">- 能识别&quot;push 在检查之前&quot;反模式
</translation>
    </message>
    <message>
        <source>- 调用栈顶帧行号显示当前执行行（BUG-DBG-1 修复后）
</source>
        <translation type="finished">- 调用栈顶帧行号显示当前执行行（BUG-DBG-1 修复后）
</translation>
    </message>
    <message>
        <source>- 输入语法错误（如 `var = ;`）观察错误恢复</source>
        <translation type="finished">- 输入语法错误（如 `var = ;`）观察错误恢复</translation>
    </message>
    <message>
        <source>- 阅读 project_memory.md 中的&quot;Lessons Learned&quot;加深理解</source>
        <translation type="finished">- 阅读 project_memory.md 中的&quot;Lessons Learned&quot;加深理解</translation>
    </message>
    <message>
        <source>--- AST 树形结构（最大深度 12）---
</source>
        <translation type="finished">--- AST 树形结构（最大深度 12）---
</translation>
    </message>
    <message>
        <source>--- VM 执行结束 ---</source>
        <translation type="finished">--- VM 执行结束 ---</translation>
    </message>
    <message>
        <source>--- 执行引擎切换: %1 ---</source>
        <translation type="finished">--- 执行引擎切换: %1 ---</translation>
    </message>
    <message>
        <source>--- 程序执行结束 ---</source>
        <translation type="finished">--- 程序执行结束 ---</translation>
    </message>
    <message>
        <source>--- 调试终止 ---</source>
        <translation type="finished">--- 调试终止 ---</translation>
    </message>
    <message>
        <source>... (还有 %1 帧未显示)</source>
        <translation type="finished">... (还有 %1 帧未显示)</translation>
    </message>
    <message>
        <source>// 假设 utils.mini 中有 export fun greet(name) { ... }
import { greet } from &quot;utils.mini&quot;;
print(greet(&quot;MiniLang&quot;));</source>
        <translation type="finished">// 假设 utils.mini 中有 export fun greet(name) { ... }
import { greet } from &quot;utils.mini&quot;;
print(greet(&quot;MiniLang&quot;));</translation>
    </message>
    <message>
        <source>// 启用 VM 单步调试后执行：
fun f() { return 1; }
print(f());</source>
        <translation type="finished">// 启用 VM 单步调试后执行：
fun f() { return 1; }
print(f());</translation>
    </message>
    <message>
        <source>// 在 REPL 中：
import { x } from &quot;./utils.mini&quot;;
// 然后修改 utils.mini 后尝试 reload：
// reload 命令内部调用 clearModuleCache(&quot;./utils.mini&quot;) 但实际缓存 key 是 &quot;utils.mini&quot;</source>
        <translation type="finished">// 在 REPL 中：
import { x } from &quot;./utils.mini&quot;;
// 然后修改 utils.mini 后尝试 reload：
// reload 命令内部调用 clearModuleCache(&quot;./utils.mini&quot;) 但实际缓存 key 是 &quot;utils.mini&quot;</translation>
    </message>
    <message>
        <source>// 触发路径遍历尝试（应被拒绝）
import { x } from &quot;C:etc/passwd&quot;;</source>
        <translation type="finished">// 触发路径遍历尝试（应被拒绝）
import { x } from &quot;C:etc/passwd&quot;;</translation>
    </message>
    <message>
        <source>// 闭包内访问外层变量</source>
        <translation type="finished">// 闭包内访问外层变量</translation>
    </message>
    <message>
        <source>1. 在编辑器中输入以下代码：
</source>
        <translation type="finished">1. 在编辑器中输入以下代码：
</translation>
    </message>
    <message>
        <source>1. 在编辑器中输入：
</source>
        <translation type="finished">1. 在编辑器中输入：
</translation>
    </message>
    <message>
        <source>1. 打开&quot;Bug 狩猎&quot;面板
</source>
        <translation type="finished">1. 打开&quot;Bug 狩猎&quot;面板
</translation>
    </message>
    <message>
        <source>1. 注册（registerTracked）</source>
        <translation type="finished">1. 注册（registerTracked）</translation>
    </message>
    <message>
        <source>1. 源码</source>
        <translation type="finished">1. 源码</translation>
    </message>
    <message>
        <source>2. Token</source>
        <translation type="finished">2. Token</translation>
    </message>
    <message>
        <source>2. 切换到 RegisterVM 模式（视图菜单）
</source>
        <translation type="finished">2. 切换到 RegisterVM 模式（视图菜单）
</translation>
    </message>
    <message>
        <source>2. 打开&quot;三后端并行对比面板&quot;
</source>
        <translation type="finished">2. 打开&quot;三后端并行对比面板&quot;
</translation>
    </message>
    <message>
        <source>2. 打开&quot;编译管线可视化面板&quot;，切到 AST 步骤
</source>
        <translation type="finished">2. 打开&quot;编译管线可视化面板&quot;，切到 AST 步骤
</translation>
    </message>
    <message>
        <source>2. 打开&quot;编译管线可视化面板&quot;，切到 Token 步骤
</source>
        <translation type="finished">2. 打开&quot;编译管线可视化面板&quot;，切到 Token 步骤
</translation>
    </message>
    <message>
        <source>2. 打开&quot;编译管线可视化面板&quot;，切到字节码步骤
</source>
        <translation type="finished">2. 打开&quot;编译管线可视化面板&quot;，切到字节码步骤
</translation>
    </message>
    <message>
        <source>2. 点击&quot;调试&quot;按钮（F6），设置断点在 fib 函数体首行
</source>
        <translation type="finished">2. 点击&quot;调试&quot;按钮（F6），设置断点在 fib 函数体首行
</translation>
    </message>
    <message>
        <source>2. 触发时机</source>
        <translation type="finished">2. 触发时机</translation>
    </message>
    <message>
        <source>2. 运行观察输出
</source>
        <translation type="finished">2. 运行观察输出
</translation>
    </message>
    <message>
        <source>2. 选择 BUG-CP-1（常量池去重陷阱）
</source>
        <translation type="finished">2. 选择 BUG-CP-1（常量池去重陷阱）
</translation>
    </message>
    <message>
        <source>3 层嵌套时中间函数不直接引用外层变量则 currentUpvalueNames_ 为空，内层函数无法透传捕获。</source>
        <translation type="finished">3 层嵌套时中间函数不直接引用外层变量则 currentUpvalueNames_ 为空，内层函数无法透传捕获。</translation>
    </message>
    <message>
        <source>3. AST</source>
        <translation type="finished">3. AST</translation>
    </message>
    <message>
        <source>3. Mark 阶段</source>
        <translation type="finished">3. Mark 阶段</translation>
    </message>
    <message>
        <source>3. 修改为 `var b = a; b[0] = 99;` 验证 COW 写时复制
</source>
        <translation type="finished">3. 修改为 `var b = a; b[0] = 99;` 验证 COW 写时复制
</translation>
    </message>
    <message>
        <source>3. 单步进入 / 跳过 / 跳出，观察调用栈
</source>
        <translation type="finished">3. 单步进入 / 跳过 / 跳出，观察调用栈
</translation>
    </message>
    <message>
        <source>3. 打开&quot;编译管线可视化面板&quot;，切到 IR 步骤
</source>
        <translation type="finished">3. 打开&quot;编译管线可视化面板&quot;，切到 IR 步骤
</translation>
    </message>
    <message>
        <source>3. 点击&quot;运行对比&quot;按钮
</source>
        <translation type="finished">3. 点击&quot;运行对比&quot;按钮
</translation>
    </message>
    <message>
        <source>3. 观察 AST 树形结构：BinaryOp(+) 左 VarDecl(=x), 右 BinaryOp(*) 左 2 右 3
</source>
        <translation type="finished">3. 观察 AST 树形结构：BinaryOp(+) 左 VarDecl(=x), 右 BinaryOp(*) 左 2 右 3
</translation>
    </message>
    <message>
        <source>3. 观察 Token 表：每行包含 type/lexeme/line/column 字段
</source>
        <translation type="finished">3. 观察 Token 表：每行包含 type/lexeme/line/column 字段
</translation>
    </message>
    <message>
        <source>3. 观察反汇编：
</source>
        <translation type="finished">3. 观察反汇编：
</translation>
    </message>
    <message>
        <source>3. 阅读背景：int(0) 与 float(0.0) 共享常量索引的后果
</source>
        <translation type="finished">3. 阅读背景：int(0) 与 float(0.0) 共享常量索引的后果
</translation>
    </message>
    <message>
        <source>4. IR</source>
        <translation type="finished">4. IR</translation>
    </message>
    <message>
        <source>4. Sweep 阶段</source>
        <translation type="finished">4. Sweep 阶段</translation>
    </message>
    <message>
        <source>4. 切换到 VM 模式，单步执行观察栈变化

</source>
        <translation type="finished">4. 切换到 VM 模式，单步执行观察栈变化

</translation>
    </message>
    <message>
        <source>4. 在内嵌编辑器中预测输出
</source>
        <translation type="finished">4. 在内嵌编辑器中预测输出
</translation>
    </message>
    <message>
        <source>4. 测试大数组传递观察引用计数（暂无可视化，需通过性能感知）

</source>
        <translation type="finished">4. 测试大数组传递观察引用计数（暂无可视化，需通过性能感知）

</translation>
    </message>
    <message>
        <source>4. 观察 IR 指令：
</source>
        <translation type="finished">4. 观察 IR 指令：
</translation>
    </message>
    <message>
        <source>4. 观察三列输出是否一致

</source>
        <translation type="finished">4. 观察三列输出是否一致

</translation>
    </message>
    <message>
        <source>4. 设置条件断点 `n == 5`，验证只在 n=5 时暂停

</source>
        <translation type="finished">4. 设置条件断点 `n == 5`，验证只在 n=5 时暂停

</translation>
    </message>
    <message>
        <source>4. 验证：乘法比加法优先级高，* 节点在 + 节点的右侧子树

</source>
        <translation type="finished">4. 验证：乘法比加法优先级高，* 节点在 + 节点的右侧子树

</translation>
    </message>
    <message>
        <source>4. 验证：注释 token 不在主流中（在 lexer.comments() 中）

</source>
        <translation type="finished">4. 验证：注释 token 不在主流中（在 lexer.comments() 中）

</translation>
    </message>
    <message>
        <source>5. UAF 防护</source>
        <translation type="finished">5. UAF 防护</translation>
    </message>
    <message>
        <source>5. 启用优化观察 v2 是否被常量折叠

</source>
        <translation type="finished">5. 启用优化观察 v2 是否被常量折叠

</translation>
    </message>
    <message>
        <source>5. 字节码</source>
        <translation type="finished">5. 字节码</translation>
    </message>
    <message>
        <source>5. 点击&quot;运行验证&quot;查看实际输出
</source>
        <translation type="finished">5. 点击&quot;运行验证&quot;查看实际输出
</translation>
    </message>
    <message>
        <source>6. 已知限制</source>
        <translation type="finished">6. 已知限制</translation>
    </message>
    <message>
        <source>6. 逐步查看提示，最后查看答案
</source>
        <translation type="finished">6. 逐步查看提示，最后查看答案
</translation>
    </message>
    <message>
        <source>7. 重复 5-10 道题，覆盖不同 Bug 模式

</source>
        <translation type="finished">7. 重复 5-10 道题，覆盖不同 Bug 模式

</translation>
    </message>
    <message>
        <source>;
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
    pageLiveBtn_    = new QPushButton(tr(</source>
        <translation type="finished">;
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
    pageLiveBtn_    = new QPushButton(tr(</translation>
    </message>
    <message>
        <source>&lt;div style=&#x27;color:#6e6e6e;padding:8px;&#x27;&gt;(未启用 IR 编译 — 在视图菜单勾选编译分析面板后查看)&lt;/div&gt;</source>
        <translation type="finished">&lt;div style=&#x27;color:#6e6e6e;padding:8px;&#x27;&gt;(未启用 IR 编译 — 在视图菜单勾选编译分析面板后查看)&lt;/div&gt;</translation>
    </message>
    <message>
        <source>&lt;div style=&#x27;color:#6e6e6e;padding:8px;&#x27;&gt;(空 IR — 无基本块)&lt;/div&gt;</source>
        <translation type="finished">&lt;div style=&#x27;color:#6e6e6e;padding:8px;&#x27;&gt;(空 IR — 无基本块)&lt;/div&gt;</translation>
    </message>
    <message>
        <source>&lt;div style=&#x27;color:#808080;padding:2px 0;&#x27;&gt;... (IR 超过 %1 行，已截断显示)&lt;/div&gt;</source>
        <translation type="finished">&lt;div style=&#x27;color:#808080;padding:2px 0;&#x27;&gt;... (IR 超过 %1 行，已截断显示)&lt;/div&gt;</translation>
    </message>
    <message>
        <source>&lt;div style=&#x27;color:red;padding:8px;&#x27;&gt;(IR 渲染失败: %1)&lt;/div&gt;</source>
        <translation type="finished">&lt;div style=&#x27;color:red;padding:8px;&#x27;&gt;(IR 渲染失败: %1)&lt;/div&gt;</translation>
    </message>
    <message>
        <source>&lt;div style=&#x27;color:red;padding:8px;&#x27;&gt;(IR 渲染失败: 未知错误)&lt;/div&gt;</source>
        <translation type="finished">&lt;div style=&#x27;color:red;padding:8px;&#x27;&gt;(IR 渲染失败: 未知错误)&lt;/div&gt;</translation>
    </message>
    <message>
        <source>&lt;h2&gt;GcManager — 循环引用垃圾收集器&lt;/h2&gt;</source>
        <translation type="finished">&lt;h2&gt;GcManager — 循环引用垃圾收集器&lt;/h2&gt;</translation>
    </message>
    <message>
        <source>&lt;h3&gt;NaN-boxing 位&lt;/h3&gt;</source>
        <translation type="finished">&lt;h3&gt;NaN-boxing 位&lt;/h3&gt;</translation>
    </message>
    <message>
        <source>&lt;h3&gt;传播路径&lt;/h3&gt;&lt;ol&gt;</source>
        <translation type="finished">&lt;h3&gt;传播路径&lt;/h3&gt;&lt;ol&gt;</translation>
    </message>
    <message>
        <source>&lt;h3&gt;值表示&lt;/h3&gt;</source>
        <translation type="finished">&lt;h3&gt;值表示&lt;/h3&gt;</translation>
    </message>
    <message>
        <source>&lt;h3&gt;变量: %1&lt;/h3&gt;</source>
        <translation type="finished">&lt;h3&gt;变量: %1&lt;/h3&gt;</translation>
    </message>
    <message>
        <source>&lt;h3&gt;堆布局&lt;/h3&gt;</source>
        <translation type="finished">&lt;h3&gt;堆布局&lt;/h3&gt;</translation>
    </message>
    <message>
        <source>&lt;h3&gt;捕获变量&lt;/h3&gt;&lt;ul&gt;</source>
        <translation type="finished">&lt;h3&gt;捕获变量&lt;/h3&gt;&lt;ul&gt;</translation>
    </message>
    <message>
        <source>&lt;h3&gt;操作数格式&lt;/h3&gt;</source>
        <translation type="finished">&lt;h3&gt;操作数格式&lt;/h3&gt;</translation>
    </message>
    <message>
        <source>&lt;h3&gt;教学注解&lt;/h3&gt;</source>
        <translation type="finished">&lt;h3&gt;教学注解&lt;/h3&gt;</translation>
    </message>
    <message>
        <source>&lt;h3&gt;教学注释&lt;/h3&gt;&lt;p&gt;</source>
        <translation type="finished">&lt;h3&gt;教学注释&lt;/h3&gt;&lt;p&gt;</translation>
    </message>
    <message>
        <source>&lt;h3&gt;期望栈帧序列&lt;/h3&gt;</source>
        <translation type="finished">&lt;h3&gt;期望栈帧序列&lt;/h3&gt;</translation>
    </message>
    <message>
        <source>&lt;h3&gt;栈帧: %1&lt;/h3&gt;</source>
        <translation type="finished">&lt;h3&gt;栈帧: %1&lt;/h3&gt;</translation>
    </message>
    <message>
        <source>&lt;h3&gt;栈效果&lt;/h3&gt;</source>
        <translation type="finished">&lt;h3&gt;栈效果&lt;/h3&gt;</translation>
    </message>
    <message>
        <source>&lt;h3&gt;样例代码&lt;/h3&gt;</source>
        <translation type="finished">&lt;h3&gt;样例代码&lt;/h3&gt;</translation>
    </message>
    <message>
        <source>&lt;h3&gt;源码&lt;/h3&gt;</source>
        <translation type="finished">&lt;h3&gt;源码&lt;/h3&gt;</translation>
    </message>
    <message>
        <source>&lt;h3&gt;示例代码&lt;/h3&gt;&lt;pre&gt;</source>
        <translation type="finished">&lt;h3&gt;示例代码&lt;/h3&gt;&lt;pre&gt;</translation>
    </message>
    <message>
        <source>&lt;h3&gt;语义&lt;/h3&gt;</source>
        <translation type="finished">&lt;h3&gt;语义&lt;/h3&gt;</translation>
    </message>
    <message>
        <source>&lt;h4&gt;Lowering 后的 IR：&lt;/h4&gt;</source>
        <translation type="finished">&lt;h4&gt;Lowering 后的 IR：&lt;/h4&gt;</translation>
    </message>
    <message>
        <source>&lt;h4&gt;示例代码：&lt;/h4&gt;</source>
        <translation type="finished">&lt;h4&gt;示例代码：&lt;/h4&gt;</translation>
    </message>
    <message>
        <source>&lt;hr&gt;&lt;p&gt;&lt;a href=&quot;#load&quot;&gt;载入到编辑器&lt;/a&gt;&lt;/p&gt;</source>
        <translation type="finished">&lt;hr&gt;&lt;p&gt;&lt;a href=&quot;#load&quot;&gt;载入到编辑器&lt;/a&gt;&lt;/p&gt;</translation>
    </message>
    <message>
        <source>&lt;i&gt;未启用 IR 编译。请在视图菜单启用 IR 模式或触发一次 IR 编译。&lt;/i&gt;</source>
        <translation type="finished">&lt;i&gt;未启用 IR 编译。请在视图菜单启用 IR 模式或触发一次 IR 编译。&lt;/i&gt;</translation>
    </message>
    <message>
        <source>&lt;p style=&#x27;color:#cc0000;&#x27;&gt;&lt;b&gt;所有后端均失败&lt;/b&gt;&lt;/p&gt;</source>
        <translation type="finished">&lt;p style=&#x27;color:#cc0000;&#x27;&gt;&lt;b&gt;所有后端均失败&lt;/b&gt;&lt;/p&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;AST 节点：&lt;/b&gt; &lt;code&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;AST 节点：&lt;/b&gt; &lt;code&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;Bug 行为:&lt;/b&gt;&lt;/p&gt;&lt;p&gt;%6&lt;/p&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;Bug 行为:&lt;/b&gt;&lt;/p&gt;&lt;p&gt;%6&lt;/p&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;IP:&lt;/b&gt; %3 | &lt;b&gt;行:&lt;/b&gt; %4 | &lt;b&gt;帧数:&lt;/b&gt; %5&lt;/p&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;IP:&lt;/b&gt; %3 | &lt;b&gt;行:&lt;/b&gt; %4 | &lt;b&gt;帧数:&lt;/b&gt; %5&lt;/p&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;NaN-boxing 位:&lt;/b&gt; &lt;code&gt;%5&lt;/code&gt;&lt;/p&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;NaN-boxing 位:&lt;/b&gt; &lt;code&gt;%5&lt;/code&gt;&lt;/p&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;值 (toString):&lt;/b&gt; %4&lt;/p&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;值 (toString):&lt;/b&gt; %4&lt;/p&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;值:&lt;/b&gt; %3&lt;/p&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;值:&lt;/b&gt; %3&lt;/p&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;分类:&lt;/b&gt; </source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;分类:&lt;/b&gt; </translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;分类:&lt;/b&gt; %2&lt;/p&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;分类:&lt;/b&gt; %2&lt;/p&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;加速比：&lt;/b&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;加速比：&lt;/b&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;原始位（binary）：&lt;/b&gt; &lt;code style=&#x27;font-size:10pt;&#x27;&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;原始位（binary）：&lt;/b&gt; &lt;code style=&#x27;font-size:10pt;&#x27;&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;原始位（hex）：&lt;/b&gt; &lt;code&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;原始位（hex）：&lt;/b&gt; &lt;code&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;当前行:&lt;/b&gt; %3&lt;/p&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;当前行:&lt;/b&gt; %3&lt;/p&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;捕获类型:&lt;/b&gt; </source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;捕获类型:&lt;/b&gt; </translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;最快后端：&lt;/b&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;最快后端：&lt;/b&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;最慢后端：&lt;/b&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;最慢后端：&lt;/b&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;期望行为:&lt;/b&gt;&lt;/p&gt;&lt;p&gt;%5&lt;/p&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;期望行为:&lt;/b&gt;&lt;/p&gt;&lt;p&gt;%5&lt;/p&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;本地变量数:&lt;/b&gt; %4&lt;/p&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;本地变量数:&lt;/b&gt; %4&lt;/p&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;条件表达式：&lt;/b&gt; &lt;code&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;条件表达式：&lt;/b&gt; &lt;code&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;栈/堆效应:&lt;/b&gt; </source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;栈/堆效应:&lt;/b&gt; </translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;栈效应:&lt;/b&gt; </source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;栈效应:&lt;/b&gt; </translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;源码：&lt;/b&gt; &lt;code&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;源码：&lt;/b&gt; &lt;code&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;示例：&lt;/b&gt; &lt;code&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;示例：&lt;/b&gt; &lt;code&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;类别:&lt;/b&gt; %3&lt;/p&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;类别:&lt;/b&gt; %3&lt;/p&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;类型:&lt;/b&gt; %2&lt;/p&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;类型:&lt;/b&gt; %2&lt;/p&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;类型:&lt;/b&gt; %3&lt;/p&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;类型:&lt;/b&gt; %3&lt;/p&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;类型名:&lt;/b&gt; &lt;code&gt;%3&lt;/code&gt;&lt;/p&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;类型名:&lt;/b&gt; &lt;code&gt;%3&lt;/code&gt;&lt;/p&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;背景:&lt;/b&gt;&lt;/p&gt;&lt;p&gt;%4&lt;/p&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;背景:&lt;/b&gt;&lt;/p&gt;&lt;p&gt;%4&lt;/p&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;表达式：&lt;/b&gt; &lt;code&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;表达式：&lt;/b&gt; &lt;code&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;解码值：&lt;/b&gt; RefCounted* 指针（48 位虚拟地址）&lt;/p&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;解码值：&lt;/b&gt; RefCounted* 指针（48 位虚拟地址）&lt;/p&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;解码值：&lt;/b&gt; bool = </source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;解码值：&lt;/b&gt; bool = </translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;解码值：&lt;/b&gt; float = </source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;解码值：&lt;/b&gt; float = </translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;解码值：&lt;/b&gt; int = </source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;解码值：&lt;/b&gt; int = </translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;解码值：&lt;/b&gt; null&lt;/p&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;解码值：&lt;/b&gt; null&lt;/p&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;触发行为：&lt;/b&gt; </source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;触发行为：&lt;/b&gt; </translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;说明:&lt;/b&gt; </source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;说明:&lt;/b&gt; </translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;说明:&lt;/b&gt;&lt;/p&gt;&lt;p&gt;%3&lt;/p&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;说明:&lt;/b&gt;&lt;/p&gt;&lt;p&gt;%3&lt;/p&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;b&gt;调用深度:&lt;/b&gt; %2&lt;/p&gt;</source>
        <translation type="finished">&lt;p&gt;&lt;b&gt;调用深度:&lt;/b&gt; %2&lt;/p&gt;</translation>
    </message>
    <message>
        <source>&lt;p&gt;&lt;small&gt;注：标准差反映多次运行的稳定性。</source>
        <translation type="finished">&lt;p&gt;&lt;small&gt;注：标准差反映多次运行的稳定性。</translation>
    </message>
    <message>
        <source>&lt;p&gt;侵入式引用计数无法回收循环引用（如 &lt;code&gt;a=[]; a.append(a)&lt;/code&gt;）。</source>
        <translation type="finished">&lt;p&gt;侵入式引用计数无法回收循环引用（如 &lt;code&gt;a=[]; a.append(a)&lt;/code&gt;）。</translation>
    </message>
    <message>
        <source>&lt;tr&gt;&lt;th&gt;深度&lt;/th&gt;&lt;th&gt;帧名&lt;/th&gt;&lt;/tr&gt;</source>
        <translation type="finished">&lt;tr&gt;&lt;th&gt;深度&lt;/th&gt;&lt;th&gt;帧名&lt;/th&gt;&lt;/tr&gt;</translation>
    </message>
    <message>
        <source>=== 提示 %1 ===
%2</source>
        <translation type="finished">=== 提示 %1 ===
%2</translation>
    </message>
    <message>
        <source>=== 答案 ===
%1</source>
        <translation type="finished">=== 答案 ===
%1</translation>
    </message>
    <message>
        <source>&gt;&gt;&gt; 输入 MiniLang 代码...</source>
        <translation type="finished">&gt;&gt;&gt; 输入 MiniLang 代码...</translation>
    </message>
    <message>
        <source>ADD 指令 → vreg2。三地址码形式 dest = OP src1, src2。</source>
        <translation type="finished">ADD 指令 → vreg2。三地址码形式 dest = OP src1, src2。</translation>
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
        <translation type="finished">AST 树形图</translation>
    </message>
    <message>
        <source>AST 根节点: </source>
        <translation type="finished">AST 根节点: </translation>
    </message>
    <message>
        <source>AST 节点数 %1 超过上限 %2，已跳过渲染以避免 UI 卡顿。
</source>
        <translation type="finished">AST 节点数 %1 超过上限 %2，已跳过渲染以避免 UI 卡顿。
</translation>
    </message>
    <message>
        <source>ArrayData 继承 RefCounted，构造时 refCount=1。</source>
        <translation type="finished">ArrayData 继承 RefCounted，构造时 refCount=1。</translation>
    </message>
    <message>
        <source>BinaryOp(ADD) lowering：左侧操作数 → vreg0，右侧 → vreg1，</source>
        <translation type="finished">BinaryOp(ADD) lowering：左侧操作数 → vreg0，右侧 → vreg1，</translation>
    </message>
    <message>
        <source>Bug 狩猎</source>
        <translation type="finished">Bug 狩猎</translation>
    </message>
    <message>
        <source>Bug 触发时：Interpreter 返回 6，VM/RegisterVM 运行时错误。</source>
        <translation type="finished">Bug 触发时：Interpreter 返回 6，VM/RegisterVM 运行时错误。</translation>
    </message>
    <message>
        <source>Bug 触发时：RETURN 指令不触发步进回调，单步跳过。</source>
        <translation type="finished">Bug 触发时：RETURN 指令不触发步进回调，单步跳过。</translation>
    </message>
    <message>
        <source>Bug 触发时：a 与 b 中可能有一个值错乱。</source>
        <translation type="finished">Bug 触发时：a 与 b 中可能有一个值错乱。</translation>
    </message>
    <message>
        <source>Bug 触发时：reload 命令无效，仍使用旧模块。</source>
        <translation type="finished">Bug 触发时：reload 命令无效，仍使用旧模块。</translation>
    </message>
    <message>
        <source>Bug 触发时：两条 export 紧挨，与未导出形式不一致。</source>
        <translation type="finished">Bug 触发时：两条 export 紧挨，与未导出形式不一致。</translation>
    </message>
    <message>
        <source>Bug 触发时：可能加载到非预期文件。</source>
        <translation type="finished">Bug 触发时：可能加载到非预期文件。</translation>
    </message>
    <message>
        <source>Bug 触发时：循环约 512 次后栈溢出崩溃。</source>
        <translation type="finished">Bug 触发时：循环约 512 次后栈溢出崩溃。</translation>
    </message>
    <message>
        <source>Bug 触发时：栈溢出崩溃（约 1024 次泄漏后）。</source>
        <translation type="finished">Bug 触发时：栈溢出崩溃（约 1024 次泄漏后）。</translation>
    </message>
    <message>
        <source>Bug 触发时：调用栈顶显示调用点行号。</source>
        <translation type="finished">Bug 触发时：调用栈顶显示调用点行号。</translation>
    </message>
    <message>
        <source>Bug 触发时：运行时错误（无法找到 upvalue）。</source>
        <translation type="finished">Bug 触发时：运行时错误（无法找到 upvalue）。</translation>
    </message>
    <message>
        <source>BytecodeChunk::addConstant 与 RegBytecodeChunk::addConstant 常量池去重时仅依赖 Value::equals()，</source>
        <translation type="finished">BytecodeChunk::addConstant 与 RegBytecodeChunk::addConstant 常量池去重时仅依赖 Value::equals()，</translation>
    </message>
    <message>
        <source>C 风格 for 循环。init/condition/update 三段式，每段可空。</source>
        <translation type="finished">C 风格 for 循环。init/condition/update 三段式，每段可空。</translation>
    </message>
    <message>
        <source>COW detach 在写前检查 refCount==1，否则深拷贝自身。</source>
        <translation type="finished">COW detach 在写前检查 refCount==1，否则深拷贝自身。</translation>
    </message>
    <message>
        <source>COW detach：原 refCount-- (→1)，深拷贝出新对象 refCount=1，写入新对象</source>
        <translation type="finished">COW detach：原 refCount-- (→1)，深拷贝出新对象 refCount=1，写入新对象</translation>
    </message>
    <message>
        <source>COW 写时复制 detach</source>
        <translation type="finished">COW 写时复制 detach</translation>
    </message>
    <message>
        <source>CallFrame::line 在 callNamedFunction 等处初始化为调用点行号后从不更新。</source>
        <translation type="finished">CallFrame::line 在 callNamedFunction 等处初始化为调用点行号后从不更新。</translation>
    </message>
    <message>
        <source>ClassDecl lowering：方法体作为独立 IRFunction 编译，</source>
        <translation type="finished">ClassDecl lowering：方法体作为独立 IRFunction 编译，</translation>
    </message>
    <message>
        <source>ClosureData 通过 weak_ptr&lt;Environment&gt; 打破循环引用。</source>
        <translation type="finished">ClosureData 通过 weak_ptr&lt;Environment&gt; 打破循环引用。</translation>
    </message>
    <message>
        <source>Compiler 路径缺少前向自由变量分析，resolveUpvalue 是惰性的（仅 visitVarRef 触发）。</source>
        <translation type="finished">Compiler 路径缺少前向自由变量分析，resolveUpvalue 是惰性的（仅 visitVarRef 触发）。</translation>
    </message>
    <message>
        <source>DCE pass 识别结果未被使用的指令（无副作用），直接消除。</source>
        <translation type="finished">DCE pass 识别结果未被使用的指令（无副作用），直接消除。</translation>
    </message>
    <message>
        <source>Formatter / 往返等价</source>
        <translation type="finished">Formatter / 往返等价</translation>
    </message>
    <message>
        <source>Formatter：ExportStmt 间未插入空行</source>
        <translation type="finished">Formatter：ExportStmt 间未插入空行</translation>
    </message>
    <message>
        <source>FunCall lowering：每个实参求值到独立 vreg，CALL 指令携带函数索引 + 参数数量。</source>
        <translation type="finished">FunCall lowering：每个实参求值到独立 vreg，CALL 指令携带函数索引 + 参数数量。</translation>
    </message>
    <message>
        <source>GC 在此场景频繁触发，sweep 开销可能拉低三后端共同基线。</source>
        <translation type="finished">GC 在此场景频繁触发，sweep 开销可能拉低三后端共同基线。</translation>
    </message>
    <message>
        <source>GC 通过 mark-sweep 可回收（mark 时 a 已 marked，sweep 不会误清），</source>
        <translation type="finished">GC 通过 mark-sweep 可回收（mark 时 a 已 marked，sweep 不会误清），</translation>
    </message>
    <message>
        <source>GcManager mark-sweep</source>
        <translation type="finished">GcManager mark-sweep</translation>
    </message>
    <message>
        <source>GcManager 实现轻量级 mark-sweep，周期性扫描所有容器节点，</source>
        <translation type="finished">GcManager 实现轻量级 mark-sweep，周期性扫描所有容器节点，</translation>
    </message>
    <message>
        <source>IIFE 每次调用创建新栈帧，参数 captured 绑定当前 i 的值。</source>
        <translation type="finished">IIFE 每次调用创建新栈帧，参数 captured 绑定当前 i 的值。</translation>
    </message>
    <message>
        <source>IIFE 用于创建新作用域，避免变量泄漏或闭包共享问题。</source>
        <translation type="finished">IIFE 用于创建新作用域，避免变量泄漏或闭包共享问题。</translation>
    </message>
    <message>
        <source>IP: %1  |  %2  |  行: %3</source>
        <translation type="finished">IP: %1  |  %2  |  行: %3</translation>
    </message>
    <message>
        <source>IR</source>
        <translation type="finished">IR</translation>
    </message>
    <message>
        <source>IR 变换</source>
        <translation type="finished">IR 变换</translation>
    </message>
    <message>
        <source>IR 生成失败</source>
        <translation type="finished">IR 生成失败</translation>
    </message>
    <message>
        <source>IR 生成异常</source>
        <translation type="finished">IR 生成异常</translation>
    </message>
    <message>
        <source>IR 编译发生未知异常</source>
        <translation type="finished">IR 编译发生未知异常</translation>
    </message>
    <message>
        <source>IR 编译异常: %1</source>
        <translation type="finished">IR 编译异常: %1</translation>
    </message>
    <message>
        <source>IR 路径 / 栈平衡</source>
        <translation type="finished">IR 路径 / 栈平衡</translation>
    </message>
    <message>
        <source>IR 路径 if 单语句体未 POP</source>
        <translation type="finished">IR 路径 if 单语句体未 POP</translation>
    </message>
    <message>
        <source>IR 路径 visitIfStmt then/else 分支直接调用 visitNode，未通过 visitStatement 统一包装器处理 POP。</source>
        <translation type="finished">IR 路径 visitIfStmt then/else 分支直接调用 visitNode，未通过 visitStatement 统一包装器处理 POP。</translation>
    </message>
    <message>
        <source>IfStmt lowering：求值 cond → BRANCH_FALSE 跳到 L_else，</source>
        <translation type="finished">IfStmt lowering：求值 cond → BRANCH_FALSE 跳到 L_else，</translation>
    </message>
    <message>
        <source>InstanceData release → refCount=1（ArrayData 仍 refCount=1）</source>
        <translation type="finished">InstanceData release → refCount=1（ArrayData 仍 refCount=1）</translation>
    </message>
    <message>
        <source>InstanceData → 遍历 fields。递归标记直到所有可达节点 marked=true。</source>
        <translation type="finished">InstanceData → 遍历 fields。递归标记直到所有可达节点 marked=true。</translation>
    </message>
    <message>
        <source>Interpreter</source>
        <translation type="finished">Interpreter</translation>
    </message>
    <message>
        <source>Interpreter 调试中</source>
        <translation type="finished">Interpreter 调试中</translation>
    </message>
    <message>
        <source>Interpreter 路径支持任意表达式默认值（b=a+1 求值为 6），VM/RegisterVM 路径仅支持字面量，</source>
        <translation type="finished">Interpreter 路径支持任意表达式默认值（b=a+1 求值为 6），VM/RegisterVM 路径仅支持字面量，</translation>
    </message>
    <message>
        <source>Interpreter::execute() 在 resetState 之后、runStatements 之前调用 collectCycle(空根集)。</source>
        <translation type="finished">Interpreter::execute() 在 resetState 之后、runStatements 之前调用 collectCycle(空根集)。</translation>
    </message>
    <message>
        <source>Lexer 通过 INTERP_START/INTERP_END/STRING_PART 三种 token 类型识别。</source>
        <translation type="finished">Lexer 通过 INTERP_START/INTERP_END/STRING_PART 三种 token 类型识别。</translation>
    </message>
    <message>
        <source>MiniLang IDE</source>
        <translation type="finished">MiniLang IDE</translation>
    </message>
    <message>
        <source>MiniLang IDE - 启动错误</source>
        <translation type="finished">MiniLang IDE - 启动错误</translation>
    </message>
    <message>
        <source>MiniLang IDE 快捷键</source>
        <translation type="finished">MiniLang IDE 快捷键</translation>
    </message>
    <message>
        <source>MiniLang REPL 输出
输入表达式或语句后按回车执行</source>
        <translation type="finished">MiniLang REPL 输出
输入表达式或语句后按回车执行</translation>
    </message>
    <message>
        <source>MiniLang 中 catch 不区分异常类型，因此第一个 catch 块总是匹配。</source>
        <translation type="finished">MiniLang 中 catch 不区分异常类型，因此第一个 catch 块总是匹配。</translation>
    </message>
    <message>
        <source>MiniLang 中 null 是一等值，可与任何变量比较。</source>
        <translation type="finished">MiniLang 中 null 是一等值，可与任何变量比较。</translation>
    </message>
    <message>
        <source>MiniLang 中 throw 是语句而非表达式，不返回值。</source>
        <translation type="finished">MiniLang 中 throw 是语句而非表达式，不返回值。</translation>
    </message>
    <message>
        <source>MiniLang 异常通过 InstanceData 表示，字段访问通过 OP_GET_FIELD 完成，</source>
        <translation type="finished">MiniLang 异常通过 InstanceData 表示，字段访问通过 OP_GET_FIELD 完成，</translation>
    </message>
    <message>
        <source>MiniLang 支持的语法:
</source>
        <translation type="finished">MiniLang 支持的语法:
</translation>
    </message>
    <message>
        <source>MiniLang 无 finally 关键字，但 catch 块可用于资源清理。</source>
        <translation type="finished">MiniLang 无 finally 关键字，但 catch 块可用于资源清理。</translation>
    </message>
    <message>
        <source>MiniLang 的 GC 会在闭包不可达时释放 upvalue。</source>
        <translation type="finished">MiniLang 的 GC 会在闭包不可达时释放 upvalue。</translation>
    </message>
    <message>
        <source>MiniLang 的 catch 不区分异常类型，因此内层 catch 总是捕获异常。</source>
        <translation type="finished">MiniLang 的 catch 不区分异常类型，因此内层 catch 总是捕获异常。</translation>
    </message>
    <message>
        <source>MiniLang 的异常对象是字符串值，catch 匹配不区分类型。</source>
        <translation type="finished">MiniLang 的异常对象是字符串值，catch 匹配不区分类型。</translation>
    </message>
    <message>
        <source>NaN-boxing 编码</source>
        <translation type="finished">NaN-boxing 编码</translation>
    </message>
    <message>
        <source>OP_CLOSURE 指令编码两个 upvalue 条目，每个指定变量位置。</source>
        <translation type="finished">OP_CLOSURE 指令编码两个 upvalue 条目，每个指定变量位置。</translation>
    </message>
    <message>
        <source>OP_WRITEBACK_INDEX_VAR/LOCAL 改为整体替换语义（不 pop 索引）后，visitIndexAssign 仍向</source>
        <translation type="finished">OP_WRITEBACK_INDEX_VAR/LOCAL 改为整体替换语义（不 pop 索引）后，visitIndexAssign 仍向</translation>
    </message>
    <message>
        <source>OpCode</source>
        <translation type="finished">OpCode</translation>
    </message>
    <message>
        <source>OpCode 性能文档：</source>
        <translation type="finished">OpCode 性能文档：</translation>
    </message>
    <message>
        <source>Parser 层限制 import/export 不能在无花括号单语句体中使用。</source>
        <translation type="finished">Parser 层限制 import/export 不能在无花括号单语句体中使用。</translation>
    </message>
    <message>
        <source>Point 类构造 + 字段赋值循环。InstanceData 分配 + GcManager::registerTracked 是主要开销，</source>
        <translation type="finished">Point 类构造 + 字段赋值循环。InstanceData 分配 + GcManager::registerTracked 是主要开销，</translation>
    </message>
    <message>
        <source>Point.new() 构造 + p.distance() 方法调用。</source>
        <translation type="finished">Point.new() 构造 + p.distance() 方法调用。</translation>
    </message>
    <message>
        <source>QThreadDeleter: Worker 未在 5 秒内退出，回退 terminate()（避免 delete running QThread UB）</source>
        <translation type="finished">QThreadDeleter: Worker 未在 5 秒内退出，回退 terminate()（避免 delete running QThread UB）</translation>
    </message>
    <message>
        <source>REG_RETURN/REG_RETURN_NULL/REG_THROW 通过直接 return 跳过函数末尾统一 stepCallback_ 调用，</source>
        <translation type="finished">REG_RETURN/REG_RETURN_NULL/REG_THROW 通过直接 return 跳过函数末尾统一 stepCallback_ 调用，</translation>
    </message>
    <message>
        <source>REPL</source>
        <translation type="finished">REPL</translation>
    </message>
    <message>
        <source>REPL / 模块系统</source>
        <translation type="finished">REPL / 模块系统</translation>
    </message>
    <message>
        <source>REPL 仍在执行</source>
        <translation type="finished">REPL 仍在执行</translation>
    </message>
    <message>
        <source>REPL 异步任务未在 5 秒内响应中止请求，强制 _Exit 退出进程</source>
        <translation type="finished">REPL 异步任务未在 5 秒内响应中止请求，强制 _Exit 退出进程</translation>
    </message>
    <message>
        <source>REPL 执行异常: %1</source>
        <translation type="finished">REPL 执行异常: %1</translation>
    </message>
    <message>
        <source>REPL 执行未知异常</source>
        <translation type="finished">REPL 执行未知异常</translation>
    </message>
    <message>
        <source>REPL 有异步任务正在执行。
关闭窗口将发送中止请求并等待最多 5 秒。

是否继续关闭？</source>
        <translation type="finished">REPL 有异步任务正在执行。
关闭窗口将发送中止请求并等待最多 5 秒。

是否继续关闭？</translation>
    </message>
    <message>
        <source>REPL 析构等待异步任务超时（5s），强制 _Exit 退出进程</source>
        <translation type="finished">REPL 析构等待异步任务超时（5s），强制 _Exit 退出进程</translation>
    </message>
    <message>
        <source>REPL 正在执行，请先停止 REPL 再使用 VM 单步</source>
        <translation type="finished">REPL 正在执行，请先停止 REPL 再使用 VM 单步</translation>
    </message>
    <message>
        <source>REPL 正在执行，请先停止 REPL 再使用 VM 跨出</source>
        <translation type="finished">REPL 正在执行，请先停止 REPL 再使用 VM 跨出</translation>
    </message>
    <message>
        <source>REPL 正在执行，请先停止 REPL 再使用 VM 跨过</source>
        <translation type="finished">REPL 正在执行，请先停止 REPL 再使用 VM 跨过</translation>
    </message>
    <message>
        <source>REPL 正在执行，请先停止 REPL 再使用 VM 运行</source>
        <translation type="finished">REPL 正在执行，请先停止 REPL 再使用 VM 运行</translation>
    </message>
    <message>
        <source>REPL 正在执行，请等待其完成后再调试</source>
        <translation type="finished">REPL 正在执行，请等待其完成后再调试</translation>
    </message>
    <message>
        <source>REPL 正在执行，请等待其完成后再运行</source>
        <translation type="finished">REPL 正在执行，请等待其完成后再运行</translation>
    </message>
    <message>
        <source>REPL 行为说明:
</source>
        <translation type="finished">REPL 行为说明:
</translation>
    </message>
    <message>
        <source>RegisterVM</source>
        <translation type="finished">RegisterVM</translation>
    </message>
    <message>
        <source>RegisterVM / 调试器</source>
        <translation type="finished">RegisterVM / 调试器</translation>
    </message>
    <message>
        <source>RegisterVM 单步：丢失步进事件</source>
        <translation type="finished">RegisterVM 单步：丢失步进事件</translation>
    </message>
    <message>
        <source>RegisterVM 热点 OpCode Top 10：</source>
        <translation type="finished">RegisterVM 热点 OpCode Top 10：</translation>
    </message>
    <message>
        <source>RegisterVM 调试中</source>
        <translation type="finished">RegisterVM 调试中</translation>
    </message>
    <message>
        <source>RegisterVM 通常略快于 StackVM（寄存器消除 push/pop 内存往返），但差距小于 Interpreter vs VM。</source>
        <translation type="finished">RegisterVM 通常略快于 StackVM（寄存器消除 push/pop 内存往返），但差距小于 Interpreter vs VM。</translation>
    </message>
    <message>
        <source>RegisterVM 通过虚拟寄存器直接访问，无需 OP_GET_LOCAL 指令（指令密度显著降低）。</source>
        <translation type="finished">RegisterVM 通过虚拟寄存器直接访问，无需 OP_GET_LOCAL 指令（指令密度显著降低）。</translation>
    </message>
    <message>
        <source>RegisterVM 通过虚拟寄存器避免 push/pop 内存往返，进一步降低开销。</source>
        <translation type="finished">RegisterVM 通过虚拟寄存器避免 push/pop 内存往返，进一步降低开销。</translation>
    </message>
    <message>
        <source>RegisterVM 通过虚拟寄存器避免每次运算 push/pop，进一步降低内存带宽占用。</source>
        <translation type="finished">RegisterVM 通过虚拟寄存器避免每次运算 push/pop，进一步降低内存带宽占用。</translation>
    </message>
    <message>
        <source>StackVM</source>
        <translation type="finished">StackVM</translation>
    </message>
    <message>
        <source>StackVM 热点 OpCode Top 10：</source>
        <translation type="finished">StackVM 热点 OpCode Top 10：</translation>
    </message>
    <message>
        <source>StackVM 调试中</source>
        <translation type="finished">StackVM 调试中</translation>
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
        <translation type="finished">Upvalue 对象持有指向栈槽的指针（is_open=true）。</translation>
    </message>
    <message>
        <source>VM RUN 模式正在执行，请先停止 VM 再启动新运行</source>
        <translation type="finished">VM RUN 模式正在执行，请先停止 VM 再启动新运行</translation>
    </message>
    <message>
        <source>VM RUN 模式正在执行，请先停止 VM 再重新编译</source>
        <translation type="finished">VM RUN 模式正在执行，请先停止 VM 再重新编译</translation>
    </message>
    <message>
        <source>VM 创建 Closure 对象，分配 upvalue 数组。</source>
        <translation type="finished">VM 创建 Closure 对象，分配 upvalue 数组。</translation>
    </message>
    <message>
        <source>VM 单步 (Ctrl+Shift+N)</source>
        <translation type="finished">VM 单步 (Ctrl+Shift+N)</translation>
    </message>
    <message>
        <source>VM 单步发生未知异常</source>
        <translation type="finished">VM 单步发生未知异常</translation>
    </message>
    <message>
        <source>VM 单步异常: %1</source>
        <translation type="finished">VM 单步异常: %1</translation>
    </message>
    <message>
        <source>VM 条件断点求值异常: </source>
        <translation type="finished">VM 条件断点求值异常: </translation>
    </message>
    <message>
        <source>VM 调用 closeUpvaluesFrom(slot) 关闭所有指向该栈槽及以上的 upvalue。</source>
        <translation type="finished">VM 调用 closeUpvaluesFrom(slot) 关闭所有指向该栈槽及以上的 upvalue。</translation>
    </message>
    <message>
        <source>VM 调用 closeUpvaluesFrom(slot)，将所有指向 slot 及以上的 open upvalue 堆化。</source>
        <translation type="finished">VM 调用 closeUpvaluesFrom(slot)，将所有指向 slot 及以上的 open upvalue 堆化。</translation>
    </message>
    <message>
        <source>VM 跨出 (Ctrl+Shift+U)</source>
        <translation type="finished">VM 跨出 (Ctrl+Shift+U)</translation>
    </message>
    <message>
        <source>VM 跨出发生未知异常</source>
        <translation type="finished">VM 跨出发生未知异常</translation>
    </message>
    <message>
        <source>VM 跨出异常: %1</source>
        <translation type="finished">VM 跨出异常: %1</translation>
    </message>
    <message>
        <source>VM 跨过 (Ctrl+Shift+O)</source>
        <translation type="finished">VM 跨过 (Ctrl+Shift+O)</translation>
    </message>
    <message>
        <source>VM 跨过发生未知异常</source>
        <translation type="finished">VM 跨过发生未知异常</translation>
    </message>
    <message>
        <source>VM 跨过异常: %1</source>
        <translation type="finished">VM 跨过异常: %1</translation>
    </message>
    <message>
        <source>VM 路径通过编译期模块内联支持 import。</source>
        <translation type="finished">VM 路径通过编译期模块内联支持 import。</translation>
    </message>
    <message>
        <source>VM 运行 (Ctrl+Shift+R)</source>
        <translation type="finished">VM 运行 (Ctrl+Shift+R)</translation>
    </message>
    <message>
        <source>VM 运行发生未知异常</source>
        <translation type="finished">VM 运行发生未知异常</translation>
    </message>
    <message>
        <source>VM 运行异常: %1</source>
        <translation type="finished">VM 运行异常: %1</translation>
    </message>
    <message>
        <source>VM停止</source>
        <translation type="finished">VM停止</translation>
    </message>
    <message>
        <source>VM单步</source>
        <translation type="finished">VM单步</translation>
    </message>
    <message>
        <source>VM跨出</source>
        <translation type="finished">VM跨出</translation>
    </message>
    <message>
        <source>VM跨过</source>
        <translation type="finished">VM跨过</translation>
    </message>
    <message>
        <source>VM运行</source>
        <translation type="finished">VM运行</translation>
    </message>
    <message>
        <source>Value 拷贝：addRef → refCount=2（共享所有权，未深拷贝）</source>
        <translation type="finished">Value 拷贝：addRef → refCount=2（共享所有权，未深拷贝）</translation>
    </message>
    <message>
        <source>Value 析构：release → refCount=0，delete this</source>
        <translation type="finished">Value 析构：release → refCount=0，delete this</translation>
    </message>
    <message>
        <source>Value 析构：release → refCount=1</source>
        <translation type="finished">Value 析构：release → refCount=1</translation>
    </message>
    <message>
        <source>VarDecl lowering：先求值初始化表达式到 vreg，再 STORE_LOCAL 写入局部槽位。</source>
        <translation type="finished">VarDecl lowering：先求值初始化表达式到 vreg，再 STORE_LOCAL 写入局部槽位。</translation>
    </message>
    <message>
        <source>WhileStmt lowering：L_start 处求值 cond → BRANCH_FALSE 跳到 L_end，</source>
        <translation type="finished">WhileStmt lowering：L_start 处求值 cond → BRANCH_FALSE 跳到 L_end，</translation>
    </message>
    <message>
        <source>Worker terminate 后 2 秒仍未退出，强制 _Exit</source>
        <translation type="finished">Worker terminate 后 2 秒仍未退出，强制 _Exit</translation>
    </message>
    <message>
        <source>Worker 未在 5 秒内响应取消请求，回退 terminate() + join（确保 close 路径安全）</source>
        <translation type="finished">Worker 未在 5 秒内响应取消请求，回退 terminate() + join（确保 close 路径安全）</translation>
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
        <source>[Parser 错误]
</source>
        <translation type="finished">[Parser 错误]
</translation>
    </message>
    <message>
        <source>[已清除所有模块缓存，下次 import 将重新加载源码]</source>
        <translation type="finished">[已清除所有模块缓存，下次 import 将重新加载源码]</translation>
    </message>
    <message>
        <source>[已清除模块 </source>
        <translation type="finished">[已清除模块 </translation>
    </message>
    <message>
        <source>[异常] </source>
        <translation type="finished">[异常] </translation>
    </message>
    <message>
        <source>[提示] Token 数量 %1 超过显示上限 %2，仅显示前 %2 条</source>
        <translation type="finished">[提示] Token 数量 %1 超过显示上限 %2，仅显示前 %2 条</translation>
    </message>
    <message>
        <source>[格式化] </source>
        <translation type="finished">[格式化] </translation>
    </message>
    <message>
        <source>[格式化] %1: %2</source>
        <translation type="finished">[格式化] %1: %2</translation>
    </message>
    <message>
        <source>[格式化] 断点已清除（行号变化，断点不再有效）</source>
        <translation type="finished">[格式化] 断点已清除（行号变化，断点不再有效）</translation>
    </message>
    <message>
        <source>[格式化] 格式化异常: %1</source>
        <translation type="finished">[格式化] 格式化异常: %1</translation>
    </message>
    <message>
        <source>[续行已中止]</source>
        <translation type="finished">[续行已中止]</translation>
    </message>
    <message>
        <source>[运行完成]</source>
        <translation type="finished">[运行完成]</translation>
    </message>
    <message>
        <source>\u95EE\u9898</source>
        <translation type="finished">\u95EE\u9898</translation>
    </message>
    <message>
        <source>```
var a = [1, 2, 3];
var b = a;          // 共享所有权
b.push(4);          // COW 触发，b 拥有新副本
print(a.len());     // 3
print(b.len());     // 4
```
</source>
        <translation type="finished">```
var a = [1, 2, 3];
var b = a;          // 共享所有权
b.push(4);          // COW 触发，b 拥有新副本
print(a.len());     // 3
print(b.len());     // 4
```
</translation>
    </message>
    <message>
        <source>```
var x = 42; // 行内注释
print(x);
```
</source>
        <translation type="finished">```
var x = 42; // 行内注释
print(x);
```
</translation>
    </message>
    <message>
        <source>acc 闭包持有 total 的 upvalue 指针，total 在堆上存活。</source>
        <translation type="finished">acc 闭包持有 total 的 upvalue 指针，total 在堆上存活。</translation>
    </message>
    <message>
        <source>add 闭包捕获两个 upvalue：base 和 delta。</source>
        <translation type="finished">add 闭包捕获两个 upvalue：base 和 delta。</translation>
    </message>
    <message>
        <source>and/or 短路求值——左操作数决定结果时跳过右操作数求值，返回操作数原值（非布尔）。</source>
        <translation type="finished">and/or 短路求值——左操作数决定结果时跳过右操作数求值，返回操作数原值（非布尔）。</translation>
    </message>
    <message>
        <source>array 类型</source>
        <translation type="finished">array 类型</translation>
    </message>
    <message>
        <source>b 离开作用域时 release，refCount 降为 1；a 离开时 release，refCount=0 触发析构。</source>
        <translation type="finished">b 离开作用域时 release，refCount 降为 1；a 离开时 release，refCount=0 触发析构。</translation>
    </message>
    <message>
        <source>bool 类型</source>
        <translation type="finished">bool 类型</translation>
    </message>
    <message>
        <source>bool 类型用 BOOL_TAG_BASE = 0x7FF9 编码，低 48 位存储 0/1。</source>
        <translation type="finished">bool 类型用 BOOL_TAG_BASE = 0x7FF9 编码，低 48 位存储 0/1。</translation>
    </message>
    <message>
        <source>c1 和 c2 是两个独立的计数器，各自捕获自己的 count。</source>
        <translation type="finished">c1 和 c2 是两个独立的计数器，各自捕获自己的 count。</translation>
    </message>
    <message>
        <source>catch 变量 e 绑定字典后，可通过 e.code / e.message 访问字段。</source>
        <translation type="finished">catch 变量 e 绑定字典后，可通过 e.code / e.message 访问字段。</translation>
    </message>
    <message>
        <source>catch 变量 e 绑定异常对象（字符串值），执行完 catch 块后继续正常流程。</source>
        <translation type="finished">catch 变量 e 绑定异常对象（字符串值），执行完 catch 块后继续正常流程。</translation>
    </message>
    <message>
        <source>catch 变量绑定异常对象后，可通过字段访问或方法调用获取详情。</source>
        <translation type="finished">catch 变量绑定异常对象后，可通过字段访问或方法调用获取详情。</translation>
    </message>
    <message>
        <source>catch 块中可以再次 throw（重新抛出），传播到外层。</source>
        <translation type="finished">catch 块中可以再次 throw（重新抛出），传播到外层。</translation>
    </message>
    <message>
        <source>catch 块中可以再次 throw，将异常（或新异常）传播到外层。</source>
        <translation type="finished">catch 块中可以再次 throw，将异常（或新异常）传播到外层。</translation>
    </message>
    <message>
        <source>catch 块内 throw 会跳过清理代码——为正确处理，catch 块用 OP_TRY_BEGIN 包装，</source>
        <translation type="finished">catch 块内 throw 会跳过清理代码——为正确处理，catch 块用 OP_TRY_BEGIN 包装，</translation>
    </message>
    <message>
        <source>catch 块执行完毕后，控制流回到 try/catch 之后的代码。</source>
        <translation type="finished">catch 块执行完毕后，控制流回到 try/catch 之后的代码。</translation>
    </message>
    <message>
        <source>catch 块执行完毕后，程序恢复正常控制流。</source>
        <translation type="finished">catch 块执行完毕后，程序恢复正常控制流。</translation>
    </message>
    <message>
        <source>catch 块捕获并处理。异常对象通过 catch 变量名（如 e）访问。</source>
        <translation type="finished">catch 块捕获并处理。异常对象通过 catch 变量名（如 e）访问。</translation>
    </message>
    <message>
        <source>catch 块捕获异常，catch 变量绑定异常对象。</source>
        <translation type="finished">catch 块捕获异常，catch 变量绑定异常对象。</translation>
    </message>
    <message>
        <source>clearModuleCache 路径规范化缺失</source>
        <translation type="finished">clearModuleCache 路径规范化缺失</translation>
    </message>
    <message>
        <source>closure 类型</source>
        <translation type="finished">closure 类型</translation>
    </message>
    <message>
        <source>dict 类型</source>
        <translation type="finished">dict 类型</translation>
    </message>
    <message>
        <source>double 和 triple 各自捕获不同的 factor（2 和 3）。</source>
        <translation type="finished">double 和 triple 各自捕获不同的 factor（2 和 3）。</translation>
    </message>
    <message>
        <source>fib(20) 场景下 OP_CALL 触发 ~21891 次，是 RegisterVM 相对 StackVM 优势最明显的指令。</source>
        <translation type="finished">fib(20) 场景下 OP_CALL 触发 ~21891 次，是 RegisterVM 相对 StackVM 优势最明显的指令。</translation>
    </message>
    <message>
        <source>fib(5) 递归展开，每层调用产生一个新栈帧，最大深度等于入参。</source>
        <translation type="finished">fib(5) 递归展开，每层调用产生一个新栈帧，最大深度等于入参。</translation>
    </message>
    <message>
        <source>finally 块无论是否发生异常都会执行。MiniLang 不支持 finally 关键字，</source>
        <translation type="finished">finally 块无论是否发生异常都会执行。MiniLang 不支持 finally 关键字，</translation>
    </message>
    <message>
        <source>finally 块无论是否发生异常都会执行。常用于资源清理</source>
        <translation type="finished">finally 块无论是否发生异常都会执行。常用于资源清理</translation>
    </message>
    <message>
        <source>finally 语义（始终执行）</source>
        <translation type="finished">finally 语义（始终执行）</translation>
    </message>
    <message>
        <source>float 3.14 直接存储（IEEE 754 double）</source>
        <translation type="finished">float 3.14 直接存储（IEEE 754 double）</translation>
    </message>
    <message>
        <source>float 类型</source>
        <translation type="finished">float 类型</translation>
    </message>
    <message>
        <source>for 循环</source>
        <translation type="finished">for 循环</translation>
    </message>
    <message>
        <source>fun inner() {
    print(&quot;now at inner line 2&quot;);  // 实际暂停行
}
fun outer() {
    inner();  // 调用点行号
}
outer();</source>
        <translation type="finished">fun inner() {
    print(&quot;now at inner line 2&quot;);  // 实际暂停行
}
fun outer() {
    inner();  // 调用点行号
}
outer();</translation>
    </message>
    <message>
        <source>fun makeAccumulator() {
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
print acc(5);   // 35</source>
        <translation type="finished">fun makeAccumulator() {
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
print acc(5);   // 35</translation>
    </message>
    <message>
        <source>fun makeCounter() {
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
print c2();  // 1（独立计数器）</source>
        <translation type="finished">fun makeCounter() {
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
print c2();  // 1（独立计数器）</translation>
    </message>
    <message>
        <source>i = 0, 100, 200, 300... 时暂停，共触发 10 次（假设循环 1000 次）</source>
        <translation type="finished">i = 0, 100, 200, 300... 时暂停，共触发 10 次（假设循环 1000 次）</translation>
    </message>
    <message>
        <source>if 语句</source>
        <translation type="finished">if 语句</translation>
    </message>
    <message>
        <source>increment 闭包捕获外层变量 x。makeCounter 返回后，</source>
        <translation type="finished">increment 闭包捕获外层变量 x。makeCounter 返回后，</translation>
    </message>
    <message>
        <source>inner 调用时通过 upvalue 链访问堆上的 a 和 b。</source>
        <translation type="finished">inner 调用时通过 upvalue 链访问堆上的 a 和 b。</translation>
    </message>
    <message>
        <source>inner 闭包捕获两个 upvalue：a 来自 outer，b 来自 middle。</source>
        <translation type="finished">inner 闭包捕获两个 upvalue：a 来自 outer，b 来自 middle。</translation>
    </message>
    <message>
        <source>input() 超时（主线程 30 秒未响应）</source>
        <translation type="finished">input() 超时（主线程 30 秒未响应）</translation>
    </message>
    <message>
        <source>input() 超时（主线程 30 秒未响应），抛出 RuntimeError</source>
        <translation type="finished">input() 超时（主线程 30 秒未响应），抛出 RuntimeError</translation>
    </message>
    <message>
        <source>instance 类型</source>
        <translation type="finished">instance 类型</translation>
    </message>
    <message>
        <source>int -1 内联（int48 负值）</source>
        <translation type="finished">int -1 内联（int48 负值）</translation>
    </message>
    <message>
        <source>int 42 内联（int48 范围内）</source>
        <translation type="finished">int 42 内联（int48 范围内）</translation>
    </message>
    <message>
        <source>int 类型</source>
        <translation type="finished">int 类型</translation>
    </message>
    <message>
        <source>int 边界值</source>
        <translation type="finished">int 边界值</translation>
    </message>
    <message>
        <source>int(0) 与 float(0.0) 因数值相等被误判为同一常量共享索引。</source>
        <translation type="finished">int(0) 与 float(0.0) 因数值相等被误判为同一常量共享索引。</translation>
    </message>
    <message>
        <source>int48 最大值 2^46-1 = 70368744177663（约 7×10^13）。超出此范围会触发装箱为 BoxedIntData*。</source>
        <translation type="finished">int48 最大值 2^46-1 = 70368744177663（约 7×10^13）。超出此范围会触发装箱为 BoxedIntData*。</translation>
    </message>
    <message>
        <source>int48 最大正值（2^46 - 1）</source>
        <translation type="finished">int48 最大正值（2^46 - 1）</translation>
    </message>
    <message>
        <source>int48 范围上限：|v| &lt; 2^47。</source>
        <translation type="finished">int48 范围上限：|v| &lt; 2^47。</translation>
    </message>
    <message>
        <source>isEven 与 isOdd 互相调用直到 n=0/1，栈呈交替增长。</source>
        <translation type="finished">isEven 与 isOdd 互相调用直到 n=0/1，栈呈交替增长。</translation>
    </message>
    <message>
        <source>isFunOrClass lambda 未解包 ExportStmt 内部声明，导致 export var x 与 export fun f 之间无空行，</source>
        <translation type="finished">isFunOrClass lambda 未解包 ExportStmt 内部声明，导致 export var x 与 export fun f 之间无空行，</translation>
    </message>
    <message>
        <source>makeAccumulator 返回后，total 的栈帧弹出，total 堆化。</source>
        <translation type="finished">makeAccumulator 返回后，total 的栈帧弹出，total 堆化。</translation>
    </message>
    <message>
        <source>makeCounter 每次调用创建新的栈帧，新的 count 变量。</source>
        <translation type="finished">makeCounter 每次调用创建新的栈帧，新的 count 变量。</translation>
    </message>
    <message>
        <source>makeCounter 返回闭包，闭包帧捕获外层 count 变量。</source>
        <translation type="finished">makeCounter 返回闭包，闭包帧捕获外层 count 变量。</translation>
    </message>
    <message>
        <source>makeCounter 闭包捕获循环。ClosureData 通过 weak_ptr&lt;Environment&gt; 打破循环，</source>
        <translation type="finished">makeCounter 闭包捕获循环。ClosureData 通过 weak_ptr&lt;Environment&gt; 打破循环，</translation>
    </message>
    <message>
        <source>makeMultiplier 返回一个匿名闭包，捕获 factor。</source>
        <translation type="finished">makeMultiplier 返回一个匿名闭包，捕获 factor。</translation>
    </message>
    <message>
        <source>makeMultiplier 返回后，factor 的栈帧被弹出，factor 堆化。</source>
        <translation type="finished">makeMultiplier 返回后，factor 的栈帧被弹出，factor 堆化。</translation>
    </message>
    <message>
        <source>markValue 检查 Value 类型：ArrayData → 遍历 elements；DictData → 遍历 entries；</source>
        <translation type="finished">markValue 检查 Value 类型：ArrayData → 遍历 elements；DictData → 遍历 entries；</translation>
    </message>
    <message>
        <source>middle 闭包本身也捕获了 a。inner 的 upvalue a 指向 middle 的 upvalue a，</source>
        <translation type="finished">middle 闭包本身也捕获了 a。inner 的 upvalue a 指向 middle 的 upvalue a，</translation>
    </message>
    <message>
        <source>normalizeModulePath 仅检测 &#x27;C:/&#x27; 形式未拒绝 &#x27;C:foo&#x27;（Windows 驱动器相对路径），</source>
        <translation type="finished">normalizeModulePath 仅检测 &#x27;C:/&#x27; 形式未拒绝 &#x27;C:foo&#x27;（Windows 驱动器相对路径），</translation>
    </message>
    <message>
        <source>null 值编码为固定常量 NULL_BITS = 0x7FFA000000000000，</source>
        <translation type="finished">null 值编码为固定常量 NULL_BITS = 0x7FFA000000000000，</translation>
    </message>
    <message>
        <source>null 兼容所有类型，float 注解接受 int 值（宽化），int 注解拒绝 float 值。</source>
        <translation type="finished">null 兼容所有类型，float 注解接受 int 值（宽化），int 注解拒绝 float 值。</translation>
    </message>
    <message>
        <source>null 检查条件断点。常用于排查变量未初始化导致的运行时错误。</source>
        <translation type="finished">null 检查条件断点。常用于排查变量未初始化导致的运行时错误。</translation>
    </message>
    <message>
        <source>null 检查条件（x == null）</source>
        <translation type="finished">null 检查条件（x == null）</translation>
    </message>
    <message>
        <source>null 类型</source>
        <translation type="finished">null 类型</translation>
    </message>
    <message>
        <source>p.x 字段持有 ArrayData（refCount=1）</source>
        <translation type="finished">p.x 字段持有 ArrayData（refCount=1）</translation>
    </message>
    <message>
        <source>patchJumps 解析跳转目标。</source>
        <translation type="finished">patchJumps 解析跳转目标。</translation>
    </message>
    <message>
        <source>payload 全为 0。</source>
        <translation type="finished">payload 全为 0。</translation>
    </message>
    <message>
        <source>refCount</source>
        <translation type="finished">refCount</translation>
    </message>
    <message>
        <source>slot 0 预留给隐式 this 参数。MethodCall 通过 GET_FIELD/SET_FIELD 访问实例字段。</source>
        <translation type="finished">slot 0 预留给隐式 this 参数。MethodCall 通过 GET_FIELD/SET_FIELD 访问实例字段。</translation>
    </message>
    <message>
        <source>string &quot;hello&quot; 装箱（PTR_TAG_BASE + 48 位指针）</source>
        <translation type="finished">string &quot;hello&quot; 装箱（PTR_TAG_BASE + 48 位指针）</translation>
    </message>
    <message>
        <source>string 类型</source>
        <translation type="finished">string 类型</translation>
    </message>
    <message>
        <source>throw 语句创建异常对象（可以是任意值：字符串、数字、字典、实例），</source>
        <translation type="finished">throw 语句创建异常对象（可以是任意值：字符串、数字、字典、实例），</translation>
    </message>
    <message>
        <source>total 在 acc 不可达时才会被 GC 回收。</source>
        <translation type="finished">total 在 acc 不可达时才会被 GC 回收。</translation>
    </message>
    <message>
        <source>tracked 节点数：%1</source>
        <translation type="finished">tracked 节点数：%1</translation>
    </message>
    <message>
        <source>tracked 节点数：—</source>
        <translation type="finished">tracked 节点数：—</translation>
    </message>
    <message>
        <source>tracked_ 列表中的 RefCounted* 可能在 collectCycle 期间被析构（如 sweep 清空子元素后 refCount→0）。</source>
        <translation type="finished">tracked_ 列表中的 RefCounted* 可能在 collectCycle 期间被析构（如 sweep 清空子元素后 refCount→0）。</translation>
    </message>
    <message>
        <source>true 编码为 0x7FF9...0001，false 为 0x7FF9...0000。</source>
        <translation type="finished">true 编码为 0x7FF9...0001，false 为 0x7FF9...0000。</translation>
    </message>
    <message>
        <source>try 块内 throw 后栈迅速回退到 catch 帧。</source>
        <translation type="finished">try 块内 throw 后栈迅速回退到 catch 帧。</translation>
    </message>
    <message>
        <source>try/catch 之后的代码会正常执行（除非再次抛出异常）。</source>
        <translation type="finished">try/catch 之后的代码会正常执行（除非再次抛出异常）。</translation>
    </message>
    <message>
        <source>try/catch 之后的代码继续执行。</source>
        <translation type="finished">try/catch 之后的代码继续执行。</translation>
    </message>
    <message>
        <source>try/catch 异常处理</source>
        <translation type="finished">try/catch 异常处理</translation>
    </message>
    <message>
        <source>upvalue 位置可以是&quot;栈槽&quot;（直接外层变量）或&quot;上层 upvalue&quot;（嵌套闭包）。</source>
        <translation type="finished">upvalue 位置可以是&quot;栈槽&quot;（直接外层变量）或&quot;上层 upvalue&quot;（嵌套闭包）。</translation>
    </message>
    <message>
        <source>upvalue 生命周期</source>
        <translation type="finished">upvalue 生命周期</translation>
    </message>
    <message>
        <source>validate() 的局部变量（如 x）在栈展开时被销毁。</source>
        <translation type="finished">validate() 的局部变量（如 x）在栈展开时被销毁。</translation>
    </message>
    <message>
        <source>var b = a 共享所有权（refCount=2），不进行深拷贝（COW 语义）。</source>
        <translation type="finished">var b = a 共享所有权（refCount=2），不进行深拷贝（COW 语义）。</translation>
    </message>
    <message>
        <source>var fns = [];
var i = 0;
while (i &lt; 3) {
    fns.push(fun() {
        return i;
    });
    i = i + 1;
}
print fns[0]();  // 3（不是 0！）
print fns[1]();  // 3
print fns[2]();  // 3</source>
        <translation type="finished">var fns = [];
var i = 0;
while (i &lt; 3) {
    fns.push(fun() {
        return i;
    });
    i = i + 1;
}
print fns[0]();  // 3（不是 0！）
print fns[1]();  // 3
print fns[2]();  // 3</translation>
    </message>
    <message>
        <source>var s2 = s1 共享所有权，s2 += &#x27;x&#x27; 实际是构造新 StringData 赋值给 s2。</source>
        <translation type="finished">var s2 = s1 共享所有权，s2 += &#x27;x&#x27; 实际是构造新 StringData 赋值给 s2。</translation>
    </message>
    <message>
        <source>var x = 42; // 行内注释
print(x);</source>
        <translation type="finished">var x = 42; // 行内注释
print(x);</translation>
    </message>
    <message>
        <source>var x = 5;
print(x &gt; 0 and &quot;positive&quot; or &quot;non-positive&quot;);
print(7 / 2);    // 3
print(7.0 / 2);  // 3.5
print(0 or &quot;default&quot;);  // 0（保留原值）</source>
        <translation type="finished">var x = 5;
print(x &gt; 0 and &quot;positive&quot; or &quot;non-positive&quot;);
print(7 / 2);    // 3
print(7.0 / 2);  // 3.5
print(0 or &quot;default&quot;);  // 0（保留原值）</translation>
    </message>
    <message>
        <source>var 声明</source>
        <translation type="finished">var 声明</translation>
    </message>
    <message>
        <source>visitImportStmt 将 \\ 转 / 并去除 ./ 前缀后存入 moduleCache_，clearModuleCache 直接用原始 path 查 erase 无法命中。</source>
        <translation type="finished">visitImportStmt 将 \\ 转 / 并去除 ./ 前缀后存入 moduleCache_，clearModuleCache 直接用原始 path 查 erase 无法命中。</translation>
    </message>
    <message>
        <source>while 循环</source>
        <translation type="finished">while 循环</translation>
    </message>
    <message>
        <source>x 的栈帧被弹出，但 x 作为 upvalue 被堆化（迁移到堆）。</source>
        <translation type="finished">x 的栈帧被弹出，但 x 作为 upvalue 被堆化（迁移到堆）。</translation>
    </message>
    <message>
        <source>x86-64 用户态虚拟地址空间为 48 位，可直接编码。</source>
        <translation type="finished">x86-64 用户态虚拟地址空间为 48 位，可直接编码。</translation>
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
        <translation type="finished">● 未保存</translation>
    </message>
    <message>
        <source>三后端在闭包捕获上开销相近，RegisterVM 因寄存器分配稍占优势。</source>
        <translation type="finished">三后端在闭包捕获上开销相近，RegisterVM 因寄存器分配稍占优势。</translation>
    </message>
    <message>
        <source>三后端对比</source>
        <translation type="finished">三后端对比</translation>
    </message>
    <message>
        <source>三后端差异 %1 行 / 共 %2 行 — 一致性 FAIL</source>
        <translation type="finished">三后端差异 %1 行 / 共 %2 行 — 一致性 FAIL</translation>
    </message>
    <message>
        <source>三后端差距较小（解释器仍稍慢，因为 MethodCall 的 Visitor 分发）。</source>
        <translation type="finished">三后端差距较小（解释器仍稍慢，因为 MethodCall 的 Visitor 分发）。</translation>
    </message>
    <message>
        <source>三后端平均时间对比 (μs)</source>
        <translation type="finished">三后端平均时间对比 (μs)</translation>
    </message>
    <message>
        <source>三后端平均时间（微秒）：</source>
        <translation type="finished">三后端平均时间（微秒）：</translation>
    </message>
    <message>
        <source>三后端性能相近（瓶颈在 hash 而非指令分发），Interpreter 略慢。</source>
        <translation type="finished">三后端性能相近（瓶颈在 hash 而非指令分发），Interpreter 略慢。</translation>
    </message>
    <message>
        <source>三后端输出一致（%1 行）— 一致性 PASS</source>
        <translation type="finished">三后端输出一致（%1 行）— 一致性 PASS</translation>
    </message>
    <message>
        <source>三层嵌套闭包：自由变量捕获断裂</source>
        <translation type="finished">三层嵌套闭包：自由变量捕获断裂</translation>
    </message>
    <message>
        <source>上一个</source>
        <translation type="finished">上一个</translation>
    </message>
    <message>
        <source>上一次执行尚未完成，请稍候...</source>
        <translation type="finished">上一次执行尚未完成，请稍候...</translation>
    </message>
    <message>
        <source>下一个</source>
        <translation type="finished">下一个</translation>
    </message>
    <message>
        <source>下一提示</source>
        <translation type="finished">下一提示</translation>
    </message>
    <message>
        <source>与 OP_GET_LOCAL 配对出现，是 StackVM 指令密度的典型代表。</source>
        <translation type="finished">与 OP_GET_LOCAL 配对出现，是 StackVM 指令密度的典型代表。</translation>
    </message>
    <message>
        <source>与未导出形式（var x 紧跟 fun f）的格式化结果不一致。</source>
        <translation type="finished">与未导出形式（var x 紧跟 fun f）的格式化结果不一致。</translation>
    </message>
    <message>
        <source>两个闭包的 upvalue 指向不同的堆地址，互不干扰。</source>
        <translation type="finished">两个闭包的 upvalue 指向不同的堆地址，互不干扰。</translation>
    </message>
    <message>
        <source>中间IR</source>
        <translation type="finished">中间IR</translation>
    </message>
    <message>
        <source>中间层又捕获外层变量。upvalue 可以指向另一层闭包的 upvalue</source>
        <translation type="finished">中间层又捕获外层变量。upvalue 可以指向另一层闭包的 upvalue</translation>
    </message>
    <message>
        <source>二元加法 a + b</source>
        <translation type="finished">二元加法 a + b</translation>
    </message>
    <message>
        <source>从 roots（globals / VM 栈 / 调用帧中的 Value）出发，递归 mark 所有可达容器节点。</source>
        <translation type="finished">从 roots（globals / VM 栈 / 调用帧中的 Value）出发，递归 mark 所有可达容器节点。</translation>
    </message>
    <message>
        <source>从常量池读取字符串并压入栈顶。</source>
        <translation type="finished">从常量池读取字符串并压入栈顶。</translation>
    </message>
    <message>
        <source>从常量池读取整数并压入栈顶。</source>
        <translation type="finished">从常量池读取整数并压入栈顶。</translation>
    </message>
    <message>
        <source>从常量池读取浮点数并压入栈顶。</source>
        <translation type="finished">从常量池读取浮点数并压入栈顶。</translation>
    </message>
    <message>
        <source>从当前函数返回，弹出整个调用帧，将返回值压入调用者栈顶。</source>
        <translation type="finished">从当前函数返回，弹出整个调用帧，将返回值压入调用者栈顶。</translation>
    </message>
    <message>
        <source>代码：</source>
        <translation type="finished">代码：</translation>
    </message>
    <message>
        <source>优化 pass 对比</source>
        <translation type="finished">优化 pass 对比</translation>
    </message>
    <message>
        <source>优化前 IR：</source>
        <translation type="finished">优化前 IR：</translation>
    </message>
    <message>
        <source>优化后 IR：</source>
        <translation type="finished">优化后 IR：</translation>
    </message>
    <message>
        <source>会被规范化为 NAN_BOXED_FLOAT_MARKER（罕见）。</source>
        <translation type="finished">会被规范化为 NAN_BOXED_FLOAT_MARKER（罕见）。</translation>
    </message>
    <message>
        <source>传播图解</source>
        <translation type="finished">传播图解</translation>
    </message>
    <message>
        <source>但仅当 a 的根引用被丢弃时才会被回收。若 a 仍在 globals 中，GC 不会触碰。</source>
        <translation type="finished">但仅当 a 的根引用被丢弃时才会被回收。若 a 仍在 globals 中，GC 不会触碰。</translation>
    </message>
    <message>
        <source>但可通过 catch + 显式清理模拟类似语义。</source>
        <translation type="finished">但可通过 catch + 显式清理模拟类似语义。</translation>
    </message>
    <message>
        <source>但可通过 catch 块中的显式清理代码模拟。</source>
        <translation type="finished">但可通过 catch 块中的显式清理代码模拟。</translation>
    </message>
    <message>
        <source>位模式（64 位，高 16 位为 tag）：</source>
        <translation type="finished">位模式（64 位，高 16 位为 tag）：</translation>
    </message>
    <message>
        <source>使闭包捕获不同的变量值。</source>
        <translation type="finished">使闭包捕获不同的变量值。</translation>
    </message>
    <message>
        <source>保存(&amp;S)</source>
        <translation type="finished">保存(&amp;S)</translation>
    </message>
    <message>
        <source>保存文件</source>
        <translation type="finished">保存文件</translation>
    </message>
    <message>
        <source>修复后：ExportStmt 之间插入空行（与未导出形式一致）。</source>
        <translation type="finished">修复后：ExportStmt 之间插入空行（与未导出形式一致）。</translation>
    </message>
    <message>
        <source>修复后：Parser 层拒绝复杂表达式默认值，仅允许字面量 + 负数字面量。</source>
        <translation type="finished">修复后：Parser 层拒绝复杂表达式默认值，仅允许字面量 + 负数字面量。</translation>
    </message>
    <message>
        <source>修复后：a=3（int 截断向零），b=3.5（float 精确）。</source>
        <translation type="finished">修复后：a=3（int 截断向零），b=3.5（float 精确）。</translation>
    </message>
    <message>
        <source>修复后：reload 命令正确清缓存，下次 import 重新加载源码。</source>
        <translation type="finished">修复后：reload 命令正确清缓存，下次 import 重新加载源码。</translation>
    </message>
    <message>
        <source>修复后：单步调试时每个 RETURN 指令触发步进回调。</source>
        <translation type="finished">修复后：单步调试时每个 RETURN 指令触发步进回调。</translation>
    </message>
    <message>
        <source>修复后：循环正常执行，arr[0][1] == 599。</source>
        <translation type="finished">修复后：循环正常执行，arr[0][1] == 599。</translation>
    </message>
    <message>
        <source>修复后：循环正常执行，约 499 次打印 hi。</source>
        <translation type="finished">修复后：循环正常执行，约 499 次打印 hi。</translation>
    </message>
    <message>
        <source>修复后：拒绝所有 &#x27;X:&#x27; 开头形式。</source>
        <translation type="finished">修复后：拒绝所有 &#x27;X:&#x27; 开头形式。</translation>
    </message>
    <message>
        <source>修复后：调用栈顶帧行号显示当前执行行号。</source>
        <translation type="finished">修复后：调用栈顶帧行号显示当前执行行号。</translation>
    </message>
    <message>
        <source>修复后：输出 1。</source>
        <translation type="finished">修复后：输出 1。</translation>
    </message>
    <message>
        <source>修复点：compiler/Bytecode.h addConstant() + compiler/RegisterBytecode.cpp RegBytecodeChunk::addConstant()。</source>
        <translation type="finished">修复点：compiler/Bytecode.h addConstant() + compiler/RegisterBytecode.cpp RegBytecodeChunk::addConstant()。</translation>
    </message>
    <message>
        <source>修复点：compiler/Compiler.cpp normalizeModulePath() + InterpreterModules.cpp + IR.cpp 三处同步。</source>
        <translation type="finished">修复点：compiler/Compiler.cpp normalizeModulePath() + InterpreterModules.cpp + IR.cpp 三处同步。</translation>
    </message>
    <message>
        <source>修复点：compiler/Compiler.cpp resolveUpvalue() + visitFunDecl()。</source>
        <translation type="finished">修复点：compiler/Compiler.cpp resolveUpvalue() + visitFunDecl()。</translation>
    </message>
    <message>
        <source>修复点：compiler/IR.cpp visitIfStmt() + visitWhileStmt() + visitForStmt()。</source>
        <translation type="finished">修复点：compiler/IR.cpp visitIfStmt() + visitWhileStmt() + visitForStmt()。</translation>
    </message>
    <message>
        <source>修复点：compiler/RegisterVM.cpp executeCalls() + executeMisc()。</source>
        <translation type="finished">修复点：compiler/RegisterVM.cpp executeCalls() + executeMisc()。</translation>
    </message>
    <message>
        <source>修复点：formatter/Formatter.cpp isFunOrClass lambda。</source>
        <translation type="finished">修复点：formatter/Formatter.cpp isFunOrClass lambda。</translation>
    </message>
    <message>
        <source>修复点：interpreter/Interpreter.cpp checkBreak()。</source>
        <translation type="finished">修复点：interpreter/Interpreter.cpp checkBreak()。</translation>
    </message>
    <message>
        <source>修复点：interpreter/Interpreter.h clearModuleCache()。</source>
        <translation type="finished">修复点：interpreter/Interpreter.h clearModuleCache()。</translation>
    </message>
    <message>
        <source>修复点：parser/Parser.cpp isLiteralDefaultExpr + 三后端同步。</source>
        <translation type="finished">修复点：parser/Parser.cpp isLiteralDefaultExpr + 三后端同步。</translation>
    </message>
    <message>
        <source>修改条件: &quot;%1&quot;</source>
        <translation type="finished">修改条件: &quot;%1&quot;</translation>
    </message>
    <message>
        <source>值</source>
        <translation type="finished">值</translation>
    </message>
    <message>
        <source>停止</source>
        <translation type="finished">停止</translation>
    </message>
    <message>
        <source>停止 VM</source>
        <translation type="finished">停止 VM</translation>
    </message>
    <message>
        <source>停止运行</source>
        <translation type="finished">停止运行</translation>
    </message>
    <message>
        <source>停止运行 (Shift+F5)</source>
        <translation type="finished">停止运行 (Shift+F5)</translation>
    </message>
    <message>
        <source>先比较 getType() 再调用 equals()，否则 int(0) 与 float(0.0) 会被误判为同一常量。</source>
        <translation type="finished">先比较 getType() 再调用 equals()，否则 int(0) 与 float(0.0) 会被误判为同一常量。</translation>
    </message>
    <message>
        <source>全字匹配</source>
        <translation type="finished">全字匹配</translation>
    </message>
    <message>
        <source>全局</source>
        <translation type="finished">全局</translation>
    </message>
    <message>
        <source>全局作用域</source>
        <translation type="finished">全局作用域</translation>
    </message>
    <message>
        <source>全局变量</source>
        <translation type="finished">全局变量</translation>
    </message>
    <message>
        <source>全部替换</source>
        <translation type="finished">全部替换</translation>
    </message>
    <message>
        <source>共 %1 个断点 | %2</source>
        <translation type="finished">共 %1 个断点 | %2</translation>
    </message>
    <message>
        <source>共享所有权，refCount=2</source>
        <translation type="finished">共享所有权，refCount=2</translation>
    </message>
    <message>
        <source>关闭</source>
        <translation type="finished">关闭</translation>
    </message>
    <message>
        <source>关闭 (Esc)</source>
        <translation type="finished">关闭 (Esc)</translation>
    </message>
    <message>
        <source>关闭全部</source>
        <translation type="finished">关闭全部</translation>
    </message>
    <message>
        <source>关闭其他</source>
        <translation type="finished">关闭其他</translation>
    </message>
    <message>
        <source>关闭查找面板</source>
        <translation type="finished">关闭查找面板</translation>
    </message>
    <message>
        <source>关闭的 upvalue 将值从栈复制到堆，Upvalue 对象改为指向堆地址（is_open=false）。</source>
        <translation type="finished">关闭的 upvalue 将值从栈复制到堆，Upvalue 对象改为指向堆地址（is_open=false）。</translation>
    </message>
    <message>
        <source>关闭面板</source>
        <translation type="finished">关闭面板</translation>
    </message>
    <message>
        <source>内存模型</source>
        <translation type="finished">内存模型</translation>
    </message>
    <message>
        <source>内层 catch 捕获异常后打印日志，然后重新 throw。</source>
        <translation type="finished">内层 catch 捕获异常后打印日志，然后重新 throw。</translation>
    </message>
    <message>
        <source>内层 try/catch 结束。控制流回到外层 try 块继续执行。</source>
        <translation type="finished">内层 try/catch 结束。控制流回到外层 try 块继续执行。</translation>
    </message>
    <message>
        <source>内部错误：前端管线返回成功但 AST 为空</source>
        <translation type="finished">内部错误：前端管线返回成功但 AST 为空</translation>
    </message>
    <message>
        <source>内部闭包捕获 captured（而非 i），因此每个闭包引用不同的 captured。</source>
        <translation type="finished">内部闭包捕获 captured（而非 i），因此每个闭包引用不同的 captured。</translation>
    </message>
    <message>
        <source>再深拷贝出新的 ArrayData（refCount=1），最后写入新对象。</source>
        <translation type="finished">再深拷贝出新的 ArrayData（refCount=1），最后写入新对象。</translation>
    </message>
    <message>
        <source>写入局部变量槽位。在循环体内频繁触发（如 i = i + 1）。</source>
        <translation type="finished">写入局部变量槽位。在循环体内频繁触发（如 i = i + 1）。</translation>
    </message>
    <message>
        <source>函数: %1 @ 行 %2 (深度: %3)</source>
        <translation type="finished">函数: %1 @ 行 %2 (深度: %3)</translation>
    </message>
    <message>
        <source>函数声明</source>
        <translation type="finished">函数声明</translation>
    </message>
    <message>
        <source>函数声明。支持默认参数（仅字面量 + 负数字面量，复杂表达式被 Parser 拒绝）。</source>
        <translation type="finished">函数声明。支持默认参数（仅字面量 + 负数字面量，复杂表达式被 Parser 拒绝）。</translation>
    </message>
    <message>
        <source>函数无提升——f(); fun f() {} 会报&quot;未定义的函数&quot;。</source>
        <translation type="finished">函数无提升——f(); fun f() {} 会报&quot;未定义的函数&quot;。</translation>
    </message>
    <message>
        <source>函数调用 f(a, b)</source>
        <translation type="finished">函数调用 f(a, b)</translation>
    </message>
    <message>
        <source>函数调用。开销最大——涉及帧栈分配、参数传递、返回地址保存。</source>
        <translation type="finished">函数调用。开销最大——涉及帧栈分配、参数传递、返回地址保存。</translation>
    </message>
    <message>
        <source>函数返回。与 OP_CALL 配对，开销同样较大（帧栈回收、返回值传递）。</source>
        <translation type="finished">函数返回。与 OP_CALL 配对，开销同样较大（帧栈回收、返回值传递）。</translation>
    </message>
    <message>
        <source>分支预测失败的代价高于指令本身，但 MiniLang VM 无分支预测（解释执行）。</source>
        <translation type="finished">分支预测失败的代价高于指令本身，但 MiniLang VM 无分支预测（解释执行）。</translation>
    </message>
    <message>
        <source>切换执行引擎</source>
        <translation type="finished">切换执行引擎</translation>
    </message>
    <message>
        <source>列</source>
        <translation type="finished">列</translation>
    </message>
    <message>
        <source>列 %1</source>
        <translation type="finished">列 %1</translation>
    </message>
    <message>
        <source>列 1</source>
        <translation type="finished">列 1</translation>
    </message>
    <message>
        <source>列号</source>
        <translation type="finished">列号</translation>
    </message>
    <message>
        <source>创建闭包值：从栈顶弹出 N 个 upvalue（每个为 isLocal+index 编码）+ 函数名，构造 ClosureData。</source>
        <translation type="finished">创建闭包值：从栈顶弹出 N 个 upvalue（每个为 isLocal+index 编码）+ 函数名，构造 ClosureData。</translation>
    </message>
    <message>
        <source>删除</source>
        <translation type="finished">删除</translation>
    </message>
    <message>
        <source>删除失败</source>
        <translation type="finished">删除失败</translation>
    </message>
    <message>
        <source>利用 and 短路特性。当 x 为 truthy 时才会求值 y &gt; 0。</source>
        <translation type="finished">利用 and 短路特性。当 x 为 truthy 时才会求值 y &gt; 0。</translation>
    </message>
    <message>
        <source>刷新统计</source>
        <translation type="finished">刷新统计</translation>
    </message>
    <message>
        <source>剖析完成</source>
        <translation type="finished">剖析完成</translation>
    </message>
    <message>
        <source>加入 tracked_ 列表与 aliveSet_。StringData/ClosureData 不注册（无循环引用风险）。</source>
        <translation type="finished">加入 tracked_ 列表与 aliveSet_。StringData/ClosureData 不注册（无循环引用风险）。</translation>
    </message>
    <message>
        <source>加载到主编辑器</source>
        <translation type="finished">加载到主编辑器</translation>
    </message>
    <message>
        <source>加载章节示例到主编辑器</source>
        <translation type="finished">加载章节示例到主编辑器</translation>
    </message>
    <message>
        <source>动作</source>
        <translation type="finished">动作</translation>
    </message>
    <message>
        <source>动态声明变量，或使用类型注解强制类型。三后端统一强制类型注解，</source>
        <translation type="finished">动态声明变量，或使用类型注解强制类型。三后端统一强制类型注解，</translation>
    </message>
    <message>
        <source>匹配 %1 处</source>
        <translation type="finished">匹配 %1 处</translation>
    </message>
    <message>
        <source>匹配 %1+ 处</source>
        <translation type="finished">匹配 %1+ 处</translation>
    </message>
    <message>
        <source>区分大小写</source>
        <translation type="finished">区分大小写</translation>
    </message>
    <message>
        <source>单步跳出</source>
        <translation type="finished">单步跳出</translation>
    </message>
    <message>
        <source>单步跳出 (Shift+F11)</source>
        <translation type="finished">单步跳出 (Shift+F11)</translation>
    </message>
    <message>
        <source>单步跳出发生未知异常</source>
        <translation type="finished">单步跳出发生未知异常</translation>
    </message>
    <message>
        <source>单步跳出失败: %1</source>
        <translation type="finished">单步跳出失败: %1</translation>
    </message>
    <message>
        <source>单步跳过</source>
        <translation type="finished">单步跳过</translation>
    </message>
    <message>
        <source>单步跳过 (F10)</source>
        <translation type="finished">单步跳过 (F10)</translation>
    </message>
    <message>
        <source>单步跳过发生未知异常</source>
        <translation type="finished">单步跳过发生未知异常</translation>
    </message>
    <message>
        <source>单步跳过失败: %1</source>
        <translation type="finished">单步跳过失败: %1</translation>
    </message>
    <message>
        <source>单步进入</source>
        <translation type="finished">单步进入</translation>
    </message>
    <message>
        <source>单步进入 (F11)</source>
        <translation type="finished">单步进入 (F11)</translation>
    </message>
    <message>
        <source>单步进入发生未知异常</source>
        <translation type="finished">单步进入发生未知异常</translation>
    </message>
    <message>
        <source>单步进入失败: %1</source>
        <translation type="finished">单步进入失败: %1</translation>
    </message>
    <message>
        <source>占比</source>
        <translation type="finished">占比</translation>
    </message>
    <message>
        <source>即使外层函数返回后，闭包仍可访问 x。</source>
        <translation type="finished">即使外层函数返回后，闭包仍可访问 x。</translation>
    </message>
    <message>
        <source>压入 null 值。</source>
        <translation type="finished">压入 null 值。</translation>
    </message>
    <message>
        <source>反斜杠在更早处已统一转为正斜杠使原反斜杠分支成为死代码。</source>
        <translation type="finished">反斜杠在更早处已统一转为正斜杠使原反斜杠分支成为死代码。</translation>
    </message>
    <message>
        <source>取模触发条件（i % 100 == 0）</source>
        <translation type="finished">取模触发条件（i % 100 == 0）</translation>
    </message>
    <message>
        <source>变量名</source>
        <translation type="finished">变量名</translation>
    </message>
    <message>
        <source>变量声明 var x = expr</source>
        <translation type="finished">变量声明 var x = expr</translation>
    </message>
    <message>
        <source>变量快照回调发生未知异常</source>
        <translation type="finished">变量快照回调发生未知异常</translation>
    </message>
    <message>
        <source>变量快照回调异常: </source>
        <translation type="finished">变量快照回调异常: </translation>
    </message>
    <message>
        <source>变量检查器</source>
        <translation type="finished">变量检查器</translation>
    </message>
    <message>
        <source>变量监视</source>
        <translation type="finished">变量监视</translation>
    </message>
    <message>
        <source>另存为(&amp;A)...</source>
        <translation type="finished">另存为(&amp;A)...</translation>
    </message>
    <message>
        <source>名称</source>
        <translation type="finished">名称</translation>
    </message>
    <message>
        <source>名称不得包含路径分隔符或父目录引用</source>
        <translation type="finished">名称不得包含路径分隔符或父目录引用</translation>
    </message>
    <message>
        <source>后端</source>
        <translation type="finished">后端</translation>
    </message>
    <message>
        <source>启动发生未知异常</source>
        <translation type="finished">启动发生未知异常</translation>
    </message>
    <message>
        <source>启动失败: %1</source>
        <translation type="finished">启动失败: %1</translation>
    </message>
    <message>
        <source>启动程序运行</source>
        <translation type="finished">启动程序运行</translation>
    </message>
    <message>
        <source>启动调试发生未知异常</source>
        <translation type="finished">启动调试发生未知异常</translation>
    </message>
    <message>
        <source>启动调试失败: %1</source>
        <translation type="finished">启动调试失败: %1</translation>
    </message>
    <message>
        <source>启动调试运行</source>
        <translation type="finished">启动调试运行</translation>
    </message>
    <message>
        <source>命中次数</source>
        <translation type="finished">命中次数</translation>
    </message>
    <message>
        <source>回跳到循环入口（负偏移）。</source>
        <translation type="finished">回跳到循环入口（负偏移）。</translation>
    </message>
    <message>
        <source>因此所有闭包引用的 upvalue 指向同一地址。</source>
        <translation type="finished">因此所有闭包引用的 upvalue 指向同一地址。</translation>
    </message>
    <message>
        <source>因此所有闭包调用都返回 3。这是闭包捕获循环变量的经典陷阱。</source>
        <translation type="finished">因此所有闭包调用都返回 3。这是闭包捕获循环变量的经典陷阱。</translation>
    </message>
    <message>
        <source>在 catch 块中设置条件断点，检查异常对象字段。</source>
        <translation type="finished">在 catch 块中设置条件断点，检查异常对象字段。</translation>
    </message>
    <message>
        <source>在代码行号左侧点击可设置/取消断点，右键点击断点可设置条件。</source>
        <translation type="finished">在代码行号左侧点击可设置/取消断点，右键点击断点可设置条件。</translation>
    </message>
    <message>
        <source>在大数组构造场景下是热点，且 GC 压力大。</source>
        <translation type="finished">在大数组构造场景下是热点，且 GC 压力大。</translation>
    </message>
    <message>
        <source>在密集全局变量引用场景下可能成为热点。</source>
        <translation type="finished">在密集全局变量引用场景下可能成为热点。</translation>
    </message>
    <message>
        <source>在密集闭包调用场景下与 OP_CLOSURE 配对出现。</source>
        <translation type="finished">在密集闭包调用场景下与 OP_CLOSURE 配对出现。</translation>
    </message>
    <message>
        <source>在循环中使用 IIFE 可为每次迭代创建独立作用域，</source>
        <translation type="finished">在循环中使用 IIFE 可为每次迭代创建独立作用域，</translation>
    </message>
    <message>
        <source>在循环中创建闭包时，所有闭包共享同一个循环变量</source>
        <translation type="finished">在循环中创建闭包时，所有闭包共享同一个循环变量</translation>
    </message>
    <message>
        <source>在方法内部设置条件断点，访问 this 的字段。</source>
        <translation type="finished">在方法内部设置条件断点，访问 this 的字段。</translation>
    </message>
    <message>
        <source>在类实例方法密集调用场景下是热点。</source>
        <translation type="finished">在类实例方法密集调用场景下是热点。</translation>
    </message>
    <message>
        <source>在编译期求值替换为单一 LOAD_CONST。</source>
        <translation type="finished">在编译期求值替换为单一 LOAD_CONST。</translation>
    </message>
    <message>
        <source>在闭包对象中按索引存储。MiniLang 的 OP_CLOSURE 指令</source>
        <translation type="finished">在闭包对象中按索引存储。MiniLang 的 OP_CLOSURE 指令</translation>
    </message>
    <message>
        <source>在闭包循环场景下热点明显，开销高于普通函数调用。</source>
        <translation type="finished">在闭包循环场景下热点明显，开销高于普通函数调用。</translation>
    </message>
    <message>
        <source>场景说明：</source>
        <translation type="finished">场景说明：</translation>
    </message>
    <message>
        <source>堆值也被回收。MiniLang 使用引用计数 + 周期回收。</source>
        <translation type="finished">堆值也被回收。MiniLang 使用引用计数 + 周期回收。</translation>
    </message>
    <message>
        <source>堆分配：COW 容器。`var b = a` 共享所有权 refCount=2，`b.push(4)` 触发 detach 深拷贝。</source>
        <translation type="finished">堆分配：COW 容器。`var b = a` 共享所有权 refCount=2，`b.push(4)` 触发 detach 深拷贝。</translation>
    </message>
    <message>
        <source>堆分配：Value 存 StringData* 指针，对象继承 RefCounted 维护引用计数。COW：写时检查 refCount==1，否则深拷贝。</source>
        <translation type="finished">堆分配：Value 存 StringData* 指针，对象继承 RefCounted 维护引用计数。COW：写时检查 refCount==1，否则深拷贝。</translation>
    </message>
    <message>
        <source>堆分配：基于哈希表的 COW 容器。键需为 string 类型。</source>
        <translation type="finished">堆分配：基于哈希表的 COW 容器。键需为 string 类型。</translation>
    </message>
    <message>
        <source>堆分配：实例字段表通过 ClassInfo::flattenedFieldOrder 描述。方法查找经 methodCache_ 加速。</source>
        <translation type="finished">堆分配：实例字段表通过 ClassInfo::flattenedFieldOrder 描述。方法查找经 methodCache_ 加速。</translation>
    </message>
    <message>
        <source>堆分配：闭包捕获外层 Environment（弱引用链 parent）。env 链打破循环依赖。</source>
        <translation type="finished">堆分配：闭包捕获外层 Environment（弱引用链 parent）。env 链打破循环依赖。</translation>
    </message>
    <message>
        <source>堆化阶段：当外层函数返回时（OP_RETURN），栈帧被弹出。</source>
        <translation type="finished">堆化阶段：当外层函数返回时（OP_RETURN），栈帧被弹出。</translation>
    </message>
    <message>
        <source>备注</source>
        <translation type="finished">备注</translation>
    </message>
    <message>
        <source>复制传播 + 跳转优化</source>
        <translation type="finished">复制传播 + 跳转优化</translation>
    </message>
    <message>
        <source>复制传播 pass 识别 v_b = MOVE v_a 模式，将后续 v_b 的引用替换为 v_a。</source>
        <translation type="finished">复制传播 pass 识别 v_b = MOVE v_a 模式，将后续 v_b 的引用替换为 v_a。</translation>
    </message>
    <message>
        <source>复合布尔条件（a &gt; 0 &amp;&amp; b &lt; 100）</source>
        <translation type="finished">复合布尔条件（a &gt; 0 &amp;&amp; b &lt; 100）</translation>
    </message>
    <message>
        <source>复合布尔表达式条件断点。MiniLang 的 and/or 短路求值（返回操作数原值而非布尔），</source>
        <translation type="finished">复合布尔表达式条件断点。MiniLang 的 and/or 短路求值（返回操作数原值而非布尔），</translation>
    </message>
    <message>
        <source>复杂表达式记录 0xFFFF 哨兵运行时报错。违反三后端一致性。</source>
        <translation type="finished">复杂表达式记录 0xFFFF 哨兵运行时报错。违反三后端一致性。</translation>
    </message>
    <message>
        <source>外层</source>
        <translation type="finished">外层</translation>
    </message>
    <message>
        <source>外层 catch 不会被触发（除非内层 catch 中再次 throw）。</source>
        <translation type="finished">外层 catch 不会被触发（除非内层 catch 中再次 throw）。</translation>
    </message>
    <message>
        <source>多个闭包可以共享同一个 upvalue，也可以各自独立。</source>
        <translation type="finished">多个闭包可以共享同一个 upvalue，也可以各自独立。</translation>
    </message>
    <message>
        <source>多变量捕获</source>
        <translation type="finished">多变量捕获</translation>
    </message>
    <message>
        <source>多层嵌套的闭包。最内层闭包捕获中间层变量，</source>
        <translation type="finished">多层嵌套的闭包。最内层闭包捕获中间层变量，</translation>
    </message>
    <message>
        <source>失败</source>
        <translation type="finished">失败</translation>
    </message>
    <message>
        <source>子节点数: </source>
        <translation type="finished">子节点数: </translation>
    </message>
    <message>
        <source>字典访问循环（10000 次）</source>
        <translation type="finished">字典访问循环（10000 次）</translation>
    </message>
    <message>
        <source>字典键值读写循环。DictData 使用 unordered_map，每次访问涉及哈希计算，</source>
        <translation type="finished">字典键值读写循环。DictData 使用 unordered_map，每次访问涉及哈希计算，</translation>
    </message>
    <message>
        <source>字符串 &quot;hello&quot; 长度超过 ASCII 内联阈值，装箱为 StringData* 堆对象。</source>
        <translation type="finished">字符串 &quot;hello&quot; 长度超过 ASCII 内联阈值，装箱为 StringData* 堆对象。</translation>
    </message>
    <message>
        <source>字符串 + 拼接。每次拼接会构造新 StringData（不可变语义），三后端性能相近（瓶颈在堆分配而非指令分发）。</source>
        <translation type="finished">字符串 + 拼接。每次拼接会构造新 StringData（不可变语义），三后端性能相近（瓶颈在堆分配而非指令分发）。</translation>
    </message>
    <message>
        <source>字符串 StringData 同样使用 RefCounted，但字符串不可变，无需 COW detach。</source>
        <translation type="finished">字符串 StringData 同样使用 RefCounted，但字符串不可变，无需 COW detach。</translation>
    </message>
    <message>
        <source>字符串共享（无 COW）</source>
        <translation type="finished">字符串共享（无 COW）</translation>
    </message>
    <message>
        <source>字符串拼接循环（1000 次）</source>
        <translation type="finished">字符串拼接循环（1000 次）</translation>
    </message>
    <message>
        <source>字符串插值</source>
        <translation type="finished">字符串插值</translation>
    </message>
    <message>
        <source>字符串插值。支持表达式嵌入。空插值 {} 报语法错误。</source>
        <translation type="finished">字符串插值。支持表达式嵌入。空插值 {} 报语法错误。</translation>
    </message>
    <message>
        <source>字符串相等条件（s == &quot;target&quot;）</source>
        <translation type="finished">字符串相等条件（s == &quot;target&quot;）</translation>
    </message>
    <message>
        <source>字符串相等比较。MiniLang 字符串相等基于值比较（非引用），</source>
        <translation type="finished">字符串相等比较。MiniLang 字符串相等基于值比较（非引用），</translation>
    </message>
    <message>
        <source>字节码</source>
        <translation type="finished">字节码</translation>
    </message>
    <message>
        <source>字节码编译异常: %1</source>
        <translation type="finished">字节码编译异常: %1</translation>
    </message>
    <message>
        <source>字节码轨迹</source>
        <translation type="finished">字节码轨迹</translation>
    </message>
    <message>
        <source>字面文本</source>
        <translation type="finished">字面文本</translation>
    </message>
    <message>
        <source>字面量</source>
        <translation type="finished">字面量</translation>
    </message>
    <message>
        <source>存活节点重置 marked=false，为下一轮收集做准备。</source>
        <translation type="finished">存活节点重置 marked=false，为下一轮收集做准备。</translation>
    </message>
    <message>
        <source>实例 InstanceData 持有字段表，字段值是 Value（可能引用其它堆对象）。</source>
        <translation type="finished">实例 InstanceData 持有字段表，字段值是 Value（可能引用其它堆对象）。</translation>
    </message>
    <message>
        <source>实例字段引用</source>
        <translation type="finished">实例字段引用</translation>
    </message>
    <message>
        <source>实例的 refCount 与字段值 refCount 相互独立。</source>
        <translation type="finished">实例的 refCount 与字段值 refCount 相互独立。</translation>
    </message>
    <message>
        <source>实时断点</source>
        <translation type="finished">实时断点</translation>
    </message>
    <message>
        <source>实现私有状态（外部无法直接访问 count，只能通过闭包操作）。</source>
        <translation type="finished">实现私有状态（外部无法直接访问 count，只能通过闭包操作）。</translation>
    </message>
    <message>
        <source>实验 1：词法分析 — 从字符到 Token</source>
        <translation type="finished">实验 1：词法分析 — 从字符到 Token</translation>
    </message>
    <message>
        <source>实验 2：递归下降解析 — 从 Token 到 AST</source>
        <translation type="finished">实验 2：递归下降解析 — 从 Token 到 AST</translation>
    </message>
    <message>
        <source>实验 3：树遍历解释器 — Visitor 模式执行 AST</source>
        <translation type="finished">实验 3：树遍历解释器 — Visitor 模式执行 AST</translation>
    </message>
    <message>
        <source>实验 4：栈式字节码 VM — 编译与执行</source>
        <translation type="finished">实验 4：栈式字节码 VM — 编译与执行</translation>
    </message>
    <message>
        <source>实验 5：寄存器式 VM — 三地址码与 IR</source>
        <translation type="finished">实验 5：寄存器式 VM — 三地址码与 IR</translation>
    </message>
    <message>
        <source>实验 6：三后端一致性 — 语义等价验证</source>
        <translation type="finished">实验 6：三后端一致性 — 语义等价验证</translation>
    </message>
    <message>
        <source>实验 7：内存模型 — NaN-boxing 与 COW</source>
        <translation type="finished">实验 7：内存模型 — NaN-boxing 与 COW</translation>
    </message>
    <message>
        <source>实验 8：Bug 狩猎 — 从历史 Bug 学习</source>
        <translation type="finished">实验 8：Bug 狩猎 — 从历史 Bug 学习</translation>
    </message>
    <message>
        <source>实验手册</source>
        <translation type="finished">实验手册</translation>
    </message>
    <message>
        <source>寄存器 (R0..Rn)</source>
        <translation type="finished">寄存器 (R0..Rn)</translation>
    </message>
    <message>
        <source>寄存器式 VM</source>
        <translation type="finished">寄存器式 VM</translation>
    </message>
    <message>
        <source>尚未生成</source>
        <translation type="finished">尚未生成</translation>
    </message>
    <message>
        <source>尚未运行</source>
        <translation type="finished">尚未运行</translation>
    </message>
    <message>
        <source>局部</source>
        <translation type="finished">局部</translation>
    </message>
    <message>
        <source>局部变量被销毁（调用析构函数）。栈展开从 throw 点开始，</source>
        <translation type="finished">局部变量被销毁（调用析构函数）。栈展开从 throw 点开始，</translation>
    </message>
    <message>
        <source>嵌套 try/catch（内层捕获）</source>
        <translation type="finished">嵌套 try/catch（内层捕获）</translation>
    </message>
    <message>
        <source>嵌套左值 / 栈平衡</source>
        <translation type="finished">嵌套左值 / 栈平衡</translation>
    </message>
    <message>
        <source>嵌套的 try/catch 结构。内层 catch 优先匹配异常。</source>
        <translation type="finished">嵌套的 try/catch 结构。内层 catch 优先匹配异常。</translation>
    </message>
    <message>
        <source>嵌套索引赋值栈泄漏：a[0][1]=x</source>
        <translation type="finished">嵌套索引赋值栈泄漏：a[0][1]=x</translation>
    </message>
    <message>
        <source>嵌套闭包（多层捕获）</source>
        <translation type="finished">嵌套闭包（多层捕获）</translation>
    </message>
    <message>
        <source>已存在</source>
        <translation type="finished">已存在</translation>
    </message>
    <message>
        <source>已无更多提示</source>
        <translation type="finished">已无更多提示</translation>
    </message>
    <message>
        <source>已显示提示 %1/%2</source>
        <translation type="finished">已显示提示 %1/%2</translation>
    </message>
    <message>
        <source>已替换 %1 处</source>
        <translation type="finished">已替换 %1 处</translation>
    </message>
    <message>
        <source>已替换 %1+ 处（达到上限，仍有剩余匹配）</source>
        <translation type="finished">已替换 %1+ 处（达到上限，仍有剩余匹配）</translation>
    </message>
    <message>
        <source>已有运行在进行，请先停止当前运行</source>
        <translation type="finished">已有运行在进行，请先停止当前运行</translation>
    </message>
    <message>
        <source>已生成 IR：%1 基本块 / %2 条指令</source>
        <translation type="finished">已生成 IR：%1 基本块 / %2 条指令</translation>
    </message>
    <message>
        <source>已请求加载到主编辑器</source>
        <translation type="finished">已请求加载到主编辑器</translation>
    </message>
    <message>
        <source>已请求加载示例（请切到主编辑器查看）</source>
        <translation type="finished">已请求加载示例（请切到主编辑器查看）</translation>
    </message>
    <message>
        <source>布尔短路条件（x &amp;&amp; y &gt; 0）</source>
        <translation type="finished">布尔短路条件（x &amp;&amp; y &gt; 0）</translation>
    </message>
    <message>
        <source>帧被弹出（栈展开）。main() 也没有 try/catch，继续弹出。</source>
        <translation type="finished">帧被弹出（栈展开）。main() 也没有 try/catch，继续弹出。</translation>
    </message>
    <message>
        <source>帮助</source>
        <translation type="finished">帮助</translation>
    </message>
    <message>
        <source>常用于块作用域结束时确保变量被堆化（如 for 循环变量）。</source>
        <translation type="finished">常用于块作用域结束时确保变量被堆化（如 for 循环变量）。</translation>
    </message>
    <message>
        <source>常用于：内层 catch 记录日志后重新抛出，或转换异常类型。</source>
        <translation type="finished">常用于：内层 catch 记录日志后重新抛出，或转换异常类型。</translation>
    </message>
    <message>
        <source>常用字典或实例作为异常对象，携带结构化错误信息。</source>
        <translation type="finished">常用字典或实例作为异常对象，携带结构化错误信息。</translation>
    </message>
    <message>
        <source>常量 42 加入 IRFunction.constants 常量池（重复时复用索引）。</source>
        <translation type="finished">常量 42 加入 IRFunction.constants 常量池（重复时复用索引）。</translation>
    </message>
    <message>
        <source>常量折叠 1 + 2 → 3</source>
        <translation type="finished">常量折叠 1 + 2 → 3</translation>
    </message>
    <message>
        <source>常量折叠 pass 识别 LOAD_CONST 操作数全为常量的算术指令，</source>
        <translation type="finished">常量折叠 pass 识别 LOAD_CONST 操作数全为常量的算术指令，</translation>
    </message>
    <message>
        <source>常量池去重 / 三后端一致性</source>
        <translation type="finished">常量池去重 / 三后端一致性</translation>
    </message>
    <message>
        <source>常量池去重陷阱：int(0) 与 float(0.0)</source>
        <translation type="finished">常量池去重陷阱：int(0) 与 float(0.0)</translation>
    </message>
    <message>
        <source>平均 (μs)</source>
        <translation type="finished">平均 (μs)</translation>
    </message>
    <message>
        <source>平均时间 (μs)</source>
        <translation type="finished">平均时间 (μs)</translation>
    </message>
    <message>
        <source>并立即中断当前控制流。异常对象被保存，用于后续 catch 变量绑定。</source>
        <translation type="finished">并立即中断当前控制流。异常对象被保存，用于后续 catch 变量绑定。</translation>
    </message>
    <message>
        <source>异常从 risky() 抛出，沿调用栈向上搜索。risky() 没有 try/catch，</source>
        <translation type="finished">异常从 risky() 抛出，沿调用栈向上搜索。risky() 没有 try/catch，</translation>
    </message>
    <message>
        <source>异常从 try 块抛出后，立即跳转到对应 catch 块。</source>
        <translation type="finished">异常从 try 块抛出后，立即跳转到对应 catch 块。</translation>
    </message>
    <message>
        <source>异常从 validate() 抛出，validate() 没有 try/catch，帧被弹出（栈展开）。</source>
        <translation type="finished">异常从 validate() 抛出，validate() 没有 try/catch，帧被弹出（栈展开）。</translation>
    </message>
    <message>
        <source>异常从内层 try 块抛出，首先匹配内层 catch。内层 catch 捕获后执行，</source>
        <translation type="finished">异常从内层 try 块抛出，首先匹配内层 catch。内层 catch 捕获后执行，</translation>
    </message>
    <message>
        <source>异常从最深层 countdown(0) 抛出，沿递归链向上传播。</source>
        <translation type="finished">异常从最深层 countdown(0) 抛出，沿递归链向上传播。</translation>
    </message>
    <message>
        <source>异常从被调用函数抛出，沿调用栈跨帧传播到调用者的 catch 块。</source>
        <translation type="finished">异常从被调用函数抛出，沿调用栈跨帧传播到调用者的 catch 块。</translation>
    </message>
    <message>
        <source>异常传播器沿调用栈向上搜索匹配的 catch 块。</source>
        <translation type="finished">异常传播器沿调用栈向上搜索匹配的 catch 块。</translation>
    </message>
    <message>
        <source>异常传播：throw 时 VM 沿调用栈向上查找 try 块，沿途弹出栈帧（risky 帧被销毁），最终落到 catch 帧。注意 risky 的局部变量在异常后不可访问。</source>
        <translation type="finished">异常传播：throw 时 VM 沿调用栈向上查找 try 块，沿途弹出栈帧（risky 帧被销毁），最终落到 catch 帧。注意 risky 的局部变量在异常后不可访问。</translation>
    </message>
    <message>
        <source>异常处理 try/catch</source>
        <translation type="finished">异常处理 try/catch</translation>
    </message>
    <message>
        <source>异常处理。throw 可抛任意值。catch 参数独占 slot（防止覆盖外层变量）。</source>
        <translation type="finished">异常处理。throw 可抛任意值。catch 参数独占 slot（防止覆盖外层变量）。</translation>
    </message>
    <message>
        <source>异常对象可以是任意值（字符串、数字、字典、实例）。</source>
        <translation type="finished">异常对象可以是任意值（字符串、数字、字典、实例）。</translation>
    </message>
    <message>
        <source>异常对象字段访问</source>
        <translation type="finished">异常对象字段访问</translation>
    </message>
    <message>
        <source>异常对象是字典值，包含 code 和 message 字段。</source>
        <translation type="finished">异常对象是字典值，包含 code 和 message 字段。</translation>
    </message>
    <message>
        <source>异常对象检查（e.message == &quot;expected&quot;)</source>
        <translation type="finished">异常对象检查（e.message == &quot;expected&quot;)</translation>
    </message>
    <message>
        <source>异常沿调用栈传播到外层 catch 块被捕获。</source>
        <translation type="finished">异常沿调用栈传播到外层 catch 块被捕获。</translation>
    </message>
    <message>
        <source>异常沿调用栈向上传播，若没有找到任何匹配的 catch 块，</source>
        <translation type="finished">异常沿调用栈向上传播，若没有找到任何匹配的 catch 块，</translation>
    </message>
    <message>
        <source>异常流</source>
        <translation type="finished">异常流</translation>
    </message>
    <message>
        <source>异常路径复制 cleanup 字节码后 OP_THROW rethrow。</source>
        <translation type="finished">异常路径复制 cleanup 字节码后 OP_THROW rethrow。</translation>
    </message>
    <message>
        <source>引用计数 &amp; COW</source>
        <translation type="finished">引用计数 &amp; COW</translation>
    </message>
    <message>
        <source>引用计数变化步骤：</source>
        <translation type="finished">引用计数变化步骤：</translation>
    </message>
    <message>
        <source>弹出栈顶 2N 个值（key+value 对）构建 DictData 并压入。</source>
        <translation type="finished">弹出栈顶 2N 个值（key+value 对）构建 DictData 并压入。</translation>
    </message>
    <message>
        <source>弹出栈顶 N 个元素构建 ArrayData 并压入。</source>
        <translation type="finished">弹出栈顶 N 个元素构建 ArrayData 并压入。</translation>
    </message>
    <message>
        <source>弹出栈顶两个值（右、左），相加后压入结果。注意：左操作数在栈深处，右操作数在栈顶。</source>
        <translation type="finished">弹出栈顶两个值（右、左），相加后压入结果。注意：左操作数在栈深处，右操作数在栈顶。</translation>
    </message>
    <message>
        <source>弹出栈顶值写入全局槽位。</source>
        <translation type="finished">弹出栈顶值写入全局槽位。</translation>
    </message>
    <message>
        <source>弹出栈顶条件，若为 false 则跳转。</source>
        <translation type="finished">弹出栈顶条件，若为 false 则跳转。</translation>
    </message>
    <message>
        <source>强制终止会跳过正常析构，未保存的代码将丢失。

</source>
        <translation type="finished">强制终止会跳过正常析构，未保存的代码将丢失。

</translation>
    </message>
    <message>
        <source>当 a &gt; 0 且 b &lt; 100 同时成立时暂停</source>
        <translation type="finished">当 a &gt; 0 且 b &lt; 100 同时成立时暂停</translation>
    </message>
    <message>
        <source>当 catch 块捕获异常且异常 message 字段等于 &quot;expected&quot; 时暂停</source>
        <translation type="finished">当 catch 块捕获异常且异常 message 字段等于 &quot;expected&quot; 时暂停</translation>
    </message>
    <message>
        <source>当 outer 返回时，a 堆化；当 middle 返回时，b 堆化。</source>
        <translation type="finished">当 outer 返回时，a 堆化；当 middle 返回时，b 堆化。</translation>
    </message>
    <message>
        <source>当 x truthy 且 y &gt; 0 时暂停</source>
        <translation type="finished">当 x truthy 且 y &gt; 0 时暂停</translation>
    </message>
    <message>
        <source>当 x 为 null 时暂停（如未初始化或显式赋值为 null）</source>
        <translation type="finished">当 x 为 null 时暂停（如未初始化或显式赋值为 null）</translation>
    </message>
    <message>
        <source>当前产生式：%1</source>
        <translation type="finished">当前产生式：%1</translation>
    </message>
    <message>
        <source>当前函数局部作用域</source>
        <translation type="finished">当前函数局部作用域</translation>
    </message>
    <message>
        <source>当前源码 IR</source>
        <translation type="finished">当前源码 IR</translation>
    </message>
    <message>
        <source>当前章节：%1</source>
        <translation type="finished">当前章节：%1</translation>
    </message>
    <message>
        <source>当前题目：%1</source>
        <translation type="finished">当前题目：%1</translation>
    </message>
    <message>
        <source>当变量 s 等于 &quot;target&quot; 时暂停</source>
        <translation type="finished">当变量 s 等于 &quot;target&quot; 时暂停</translation>
    </message>
    <message>
        <source>当方法被调用且 this.value &gt; 100 时暂停</source>
        <translation type="finished">当方法被调用且 this.value &gt; 100 时暂停</translation>
    </message>
    <message>
        <source>循环 while (cond) {...}</source>
        <translation type="finished">循环 while (cond) {...}</translation>
    </message>
    <message>
        <source>循环内 `if (cond) foo();` 每次泄漏 1 个栈值，多次循环触发栈溢出。</source>
        <translation type="finished">循环内 `if (cond) foo();` 每次泄漏 1 个栈值，多次循环触发栈溢出。</translation>
    </message>
    <message>
        <source>循环执行到第 50 次迭代时暂停，其余迭代继续执行（不暂停）</source>
        <translation type="finished">循环执行到第 50 次迭代时暂停，其余迭代继续执行（不暂停）</translation>
    </message>
    <message>
        <source>循环求和（1 到 100000）</source>
        <translation type="finished">循环求和（1 到 100000）</translation>
    </message>
    <message>
        <source>性能分析：</source>
        <translation type="finished">性能分析：</translation>
    </message>
    <message>
        <source>性能剖析</source>
        <translation type="finished">性能剖析</translation>
    </message>
    <message>
        <source>所有新建的 ArrayData / DictData / InstanceData 在构造函数中调用 GcManager::instance().registerTracked(this)，</source>
        <translation type="finished">所有新建的 ArrayData / DictData / InstanceData 在构造函数中调用 GcManager::instance().registerTracked(this)，</translation>
    </message>
    <message>
        <source>所有闭包共享同一个 i 变量。循环结束后 i = 3，</source>
        <translation type="finished">所有闭包共享同一个 i 变量。循环结束后 i = 3，</translation>
    </message>
    <message>
        <source>打开文件</source>
        <translation type="finished">打开文件</translation>
    </message>
    <message>
        <source>打开文件(&amp;O)...</source>
        <translation type="finished">打开文件(&amp;O)...</translation>
    </message>
    <message>
        <source>打开文件夹</source>
        <translation type="finished">打开文件夹</translation>
    </message>
    <message>
        <source>打开文件夹(&amp;D)...</source>
        <translation type="finished">打开文件夹(&amp;D)...</translation>
    </message>
    <message>
        <source>执行 then 块后 JUMP L_end，else 块从 L_else 开始。</source>
        <translation type="finished">执行 then 块后 JUMP L_end，else 块从 L_else 开始。</translation>
    </message>
    <message>
        <source>执行循环体后 JUMP L_start。每个基本块终结于 BRANCH/JUMP/RETURN。</source>
        <translation type="finished">执行循环体后 JUMP L_start。每个基本块终结于 BRANCH/JUMP/RETURN。</translation>
    </message>
    <message>
        <source>执行期间不再触发（性能权衡：mark-sweep 开销 O(节点数+边数)，仅起点触发）。</source>
        <translation type="finished">执行期间不再触发（性能权衡：mark-sweep 开销 O(节点数+边数)，仅起点触发）。</translation>
    </message>
    <message>
        <source>执行次数</source>
        <translation type="finished">执行次数</translation>
    </message>
    <message>
        <source>指令计数</source>
        <translation type="finished">指令计数</translation>
    </message>
    <message>
        <source>指令读写 upvalue。若 upvalue 仍 open，直接访问栈槽；若已 closed，访问堆地址。</source>
        <translation type="finished">指令读写 upvalue。若 upvalue 仍 open，直接访问栈槽；若已 closed，访问堆地址。</translation>
    </message>
    <message>
        <source>捕获的变量称为 upvalue（上层作用域的值）。</source>
        <translation type="finished">捕获的变量称为 upvalue（上层作用域的值）。</translation>
    </message>
    <message>
        <source>捕获阶段：VM 遍历 upvalue 描述符，为每个 upvalue 查找或创建 Upvalue 对象。</source>
        <translation type="finished">捕获阶段：VM 遍历 upvalue 描述符，为每个 upvalue 查找或创建 Upvalue 对象。</translation>
    </message>
    <message>
        <source>控制流回到 main() 的 try 块，异常被 catch 块捕获。</source>
        <translation type="finished">控制流回到 main() 的 try 块，异常被 catch 块捕获。</translation>
    </message>
    <message>
        <source>提示1：resolveUpvalue 沿外层帧查找——但中间函数若没显式引用 x，则不会创建 upvalue 槽位。</source>
        <translation type="finished">提示1：resolveUpvalue 沿外层帧查找——但中间函数若没显式引用 x，则不会创建 upvalue 槽位。</translation>
    </message>
    <message>
        <source>提示1：在 visitIndexAssign 中检查 OP_WRITEBACK_INDEX_* 后栈的 push/pop 数量是否匹配。</source>
        <translation type="finished">提示1：在 visitIndexAssign 中检查 OP_WRITEBACK_INDEX_* 后栈的 push/pop 数量是否匹配。</translation>
    </message>
    <message>
        <source>提示1：对比 visitImportStmt 存入 moduleCache_ 的 key 与 clearModuleCache 接收的 path。</source>
        <translation type="finished">提示1：对比 visitImportStmt 存入 moduleCache_ 的 key 与 clearModuleCache 接收的 path。</translation>
    </message>
    <message>
        <source>提示1：检查 BytecodeChunk::addConstant 的去重条件——只比较 Value::equals() 是否足够？</source>
        <translation type="finished">提示1：检查 BytecodeChunk::addConstant 的去重条件——只比较 Value::equals() 是否足够？</translation>
    </message>
    <message>
        <source>提示1：检查 Formatter 中 isFunOrClass lambda 的判定——是否检查 NODE_EXPORT_STMT 内部？</source>
        <translation type="finished">提示1：检查 Formatter 中 isFunOrClass lambda 的判定——是否检查 NODE_EXPORT_STMT 内部？</translation>
    </message>
    <message>
        <source>提示1：检查 IR 路径 visitIfStmt 的 then/else 分支调用——是 visitNode 还是 visitStatement？</source>
        <translation type="finished">提示1：检查 IR 路径 visitIfStmt 的 then/else 分支调用——是 visitNode 还是 visitStatement？</translation>
    </message>
    <message>
        <source>提示1：检查 Parser::isLiteralDefaultExpr 哪些节点被允许？</source>
        <translation type="finished">提示1：检查 Parser::isLiteralDefaultExpr 哪些节点被允许？</translation>
    </message>
    <message>
        <source>提示1：检查 callNamedFunction 中 CallFrame.line 初始化值。</source>
        <translation type="finished">提示1：检查 callNamedFunction 中 CallFrame.line 初始化值。</translation>
    </message>
    <message>
        <source>提示1：检查 executeCalls 中 REG_RETURN 的处理路径——是 return 直接返回，还是 goto 到统一调用点？</source>
        <translation type="finished">提示1：检查 executeCalls 中 REG_RETURN 的处理路径——是 return 直接返回，还是 goto 到统一调用点？</translation>
    </message>
    <message>
        <source>提示1：检查 normalizeModulePath 的 &#x27;X:&#x27; 检测分支——只检测 &#x27;X:/&#x27; 还是所有 &#x27;X:&#x27; 开头？</source>
        <translation type="finished">提示1：检查 normalizeModulePath 的 &#x27;X:&#x27; 检测分支——只检测 &#x27;X:/&#x27; 还是所有 &#x27;X:&#x27; 开头？</translation>
    </message>
    <message>
        <source>提示2：BinaryOp/FunCall 等表达式不应作为默认值。</source>
        <translation type="finished">提示2：BinaryOp/FunCall 等表达式不应作为默认值。</translation>
    </message>
    <message>
        <source>提示2：IR 路径有 computeFreeVars 前向分析修复了此问题，Compiler 路径没有。</source>
        <translation type="finished">提示2：IR 路径有 computeFreeVars 前向分析修复了此问题，Compiler 路径没有。</translation>
    </message>
    <message>
        <source>提示2：StackVM 通过 notifyStep(ip, op) 在所有指令后统一调用，RegisterVM 是否对齐？</source>
        <translation type="finished">提示2：StackVM 通过 notifyStep(ip, op) 在所有指令后统一调用，RegisterVM 是否对齐？</translation>
    </message>
    <message>
        <source>提示2：checkBreak 是否在某处更新 line？</source>
        <translation type="finished">提示2：checkBreak 是否在某处更新 line？</translation>
    </message>
    <message>
        <source>提示2：int(0) 与 float(0.0) 的 getType() 不同，但 equals() 返回 true。</source>
        <translation type="finished">提示2：int(0) 与 float(0.0) 的 getType() 不同，但 equals() 返回 true。</translation>
    </message>
    <message>
        <source>提示2：visitImportStmt 是否做了路径规范化（\\→/、strip ./）？</source>
        <translation type="finished">提示2：visitImportStmt 是否做了路径规范化（\\→/、strip ./）？</translation>
    </message>
    <message>
        <source>提示2：visitStatement 是统一包装器，会对表达式语句 emit POP。</source>
        <translation type="finished">提示2：visitStatement 是统一包装器，会对表达式语句 emit POP。</translation>
    </message>
    <message>
        <source>提示2：反斜杠在何处被转为正斜杠？若已转，原反斜杠分支是死代码。</source>
        <translation type="finished">提示2：反斜杠在何处被转为正斜杠？若已转，原反斜杠分支是死代码。</translation>
    </message>
    <message>
        <source>提示2：对比 visitMethodCall 的同步修复——它已经不推索引，visitIndexAssign 是否漏改？</source>
        <translation type="finished">提示2：对比 visitMethodCall 的同步修复——它已经不推索引，visitIndexAssign 是否漏改？</translation>
    </message>
    <message>
        <source>提示2：导出形式应与未导出形式格式化结果一致。</source>
        <translation type="finished">提示2：导出形式应与未导出形式格式化结果一致。</translation>
    </message>
    <message>
        <source>提示3：UnaryOp(NEGATE) 套 NumberLiteral 应该被允许（负数字面量）。</source>
        <translation type="finished">提示3：UnaryOp(NEGATE) 套 NumberLiteral 应该被允许（负数字面量）。</translation>
    </message>
    <message>
        <source>提示3：clearModuleCache 需同步做相同规范化。</source>
        <translation type="finished">提示3：clearModuleCache 需同步做相同规范化。</translation>
    </message>
    <message>
        <source>提示3：修复方案：在 visitFunDecl 加入类似 IR 的 computeFreeVars 前向分析。</source>
        <translation type="finished">提示3：修复方案：在 visitFunDecl 加入类似 IR 的 computeFreeVars 前向分析。</translation>
    </message>
    <message>
        <source>提示3：修复需在每次暂停时更新 callStack_.back().line 为当前节点行号。</source>
        <translation type="finished">提示3：修复需在每次暂停时更新 callStack_.back().line 为当前节点行号。</translation>
    </message>
    <message>
        <source>提示3：修复需拒绝所有 path.size() &gt;= 2 &amp;&amp; path[1] == &#x27;:&#x27; 形式。</source>
        <translation type="finished">提示3：修复需拒绝所有 path.size() &gt;= 2 &amp;&amp; path[1] == &#x27;:&#x27; 形式。</translation>
    </message>
    <message>
        <source>提示3：修复需要先比较 getType()，再比较 equals()。</source>
        <translation type="finished">提示3：修复需要先比较 getType()，再比较 equals()。</translation>
    </message>
    <message>
        <source>提示3：修复：isFunOrClass 解包 NODE_EXPORT_STMT 检查内部 declaration 节点类型。</source>
        <translation type="finished">提示3：修复：isFunOrClass 解包 NODE_EXPORT_STMT 检查内部 declaration 节点类型。</translation>
    </message>
    <message>
        <source>提示3：修复：捕获结果，若 VM_OK 则调用 stepCallback_。</source>
        <translation type="finished">提示3：修复：捕获结果，若 VM_OK 则调用 stepCallback_。</translation>
    </message>
    <message>
        <source>提示3：同样的 Bug 也存在于 visitWhileStmt 和 visitForStmt 的单语句体路径。</source>
        <translation type="finished">提示3：同样的 Bug 也存在于 visitWhileStmt 和 visitForStmt 的单语句体路径。</translation>
    </message>
    <message>
        <source>提示3：在 IR 路径不触发是因为 BytecodeIRBackend 用 vreg 物化栈值，不依赖物理栈平衡。</source>
        <translation type="finished">提示3：在 IR 路径不触发是因为 BytecodeIRBackend 用 vreg 物化栈值，不依赖物理栈平衡。</translation>
    </message>
    <message>
        <source>插值支持嵌套字符串、字典、数组，嵌套深度限制 64 层防栈溢出。</source>
        <translation type="finished">插值支持嵌套字符串、字典、数组，嵌套深度限制 64 层防栈溢出。</translation>
    </message>
    <message>
        <source>操作数栈 (栈顶 ↑)</source>
        <translation type="finished">操作数栈 (栈顶 ↑)</translation>
    </message>
    <message>
        <source>支持 break/continue。循环变量在顶层时退出后清理（DELETE_VAR）。</source>
        <translation type="finished">支持 break/continue。循环变量在顶层时退出后清理（DELETE_VAR）。</translation>
    </message>
    <message>
        <source>支持 super.method() 调用，三后端 super 调用语义一致。</source>
        <translation type="finished">支持 super.method() 调用，三后端 super 调用语义一致。</translation>
    </message>
    <message>
        <source>支持索引读写（a[0] / d[&quot;key&quot;]）和嵌套索引赋值（a[0][1]=x）。</source>
        <translation type="finished">支持索引读写（a[0] / d[&quot;key&quot;]）和嵌套索引赋值（a[0][1]=x）。</translation>
    </message>
    <message>
        <source>支持闭包与 upvalue 捕获（3+ 层）。</source>
        <translation type="finished">支持闭包与 upvalue 捕获（3+ 层）。</translation>
    </message>
    <message>
        <source>教学场景库</source>
        <translation type="finished">教学场景库</translation>
    </message>
    <message>
        <source>数组/字典使用 Copy-On-Write（COW），写前检查独占所有权。</source>
        <translation type="finished">数组/字典使用 Copy-On-Write（COW），写前检查独占所有权。</translation>
    </message>
    <message>
        <source>数组与字典</source>
        <translation type="finished">数组与字典</translation>
    </message>
    <message>
        <source>数组与字典字面量。字典键强制为 string，键不存在时返回 null。</source>
        <translation type="finished">数组与字典字面量。字典键强制为 string，键不存在时返回 null。</translation>
    </message>
    <message>
        <source>数组基础生命周期</source>
        <translation type="finished">数组基础生命周期</translation>
    </message>
    <message>
        <source>整数 -1 以补码形式存储到低 48 位（全 1），</source>
        <translation type="finished">整数 -1 以补码形式存储到低 48 位（全 1），</translation>
    </message>
    <message>
        <source>整数 42 在 int48 范围（|v| &lt; 2^47）内，直接内联到 NaN-box 的低 48 位。</source>
        <translation type="finished">整数 42 在 int48 范围（|v| &lt; 2^47）内，直接内联到 NaN-box 的低 48 位。</translation>
    </message>
    <message>
        <source>整数减法。性能特征与 OP_ADD 一致，但热度通常较低（循环场景下递增多于递减）。</source>
        <translation type="finished">整数减法。性能特征与 OP_ADD 一致，但热度通常较低（循环场景下递增多于递减）。</translation>
    </message>
    <message>
        <source>整数加法。在循环场景下是绝对热点——单条 OP_ADD 比解释器 Visitor dispatch 快 5-10 倍。</source>
        <translation type="finished">整数加法。在循环场景下是绝对热点——单条 OP_ADD 比解释器 Visitor dispatch 快 5-10 倍。</translation>
    </message>
    <message>
        <source>整数字面量 42</source>
        <translation type="finished">整数字面量 42</translation>
    </message>
    <message>
        <source>整数字面量直接 LOAD_CONST 加载到虚拟寄存器。</source>
        <translation type="finished">整数字面量直接 LOAD_CONST 加载到虚拟寄存器。</translation>
    </message>
    <message>
        <source>文件</source>
        <translation type="finished">文件</translation>
    </message>
    <message>
        <source>文件名 (将以 .mini 扩展名创建):</source>
        <translation type="finished">文件名 (将以 .mini 扩展名创建):</translation>
    </message>
    <message>
        <source>文件名不得包含路径分隔符或父目录引用</source>
        <translation type="finished">文件名不得包含路径分隔符或父目录引用</translation>
    </message>
    <message>
        <source>文件夹名:</source>
        <translation type="finished">文件夹名:</translation>
    </message>
    <message>
        <source>文件已修改，是否保存？</source>
        <translation type="finished">文件已修改，是否保存？</translation>
    </message>
    <message>
        <source>文件已存在：</source>
        <translation type="finished">文件已存在：</translation>
    </message>
    <message>
        <source>文件过大 (</source>
        <translation type="finished">文件过大 (</translation>
    </message>
    <message>
        <source>斐波那契递归（fib(20)）</source>
        <translation type="finished">斐波那契递归（fib(20)）</translation>
    </message>
    <message>
        <source>新名称:</source>
        <translation type="finished">新名称:</translation>
    </message>
    <message>
        <source>新建(&amp;N)</source>
        <translation type="finished">新建(&amp;N)</translation>
    </message>
    <message>
        <source>新建文件</source>
        <translation type="finished">新建文件</translation>
    </message>
    <message>
        <source>新建文件夹</source>
        <translation type="finished">新建文件夹</translation>
    </message>
    <message>
        <source>方法调用。比 OP_CALL 更昂贵——涉及方法查找（method resolution）。</source>
        <translation type="finished">方法调用。比 OP_CALL 更昂贵——涉及方法查找（method resolution）。</translation>
    </message>
    <message>
        <source>方法调用条件（this.value &gt; 100）</source>
        <translation type="finished">方法调用条件（this.value &gt; 100）</translation>
    </message>
    <message>
        <source>方法调用栈：distance 帧的 this 隐式绑定到 p 实例，可通过 this 访问字段 x/y。方法查找经过 ClassInfo::methodCache_ 缓存加速。</source>
        <translation type="finished">方法调用栈：distance 帧的 this 隐式绑定到 p 实例，可通过 this 访问字段 x/y。方法查找经过 ClassInfo::methodCache_ 缓存加速。</translation>
    </message>
    <message>
        <source>方法调用：通过 methodCache_ 查找方法，避免重复 ClassInfo 遍历。</source>
        <translation type="finished">方法调用：通过 methodCache_ 查找方法，避免重复 ClassInfo 遍历。</translation>
    </message>
    <message>
        <source>无</source>
        <translation type="finished">无</translation>
    </message>
    <message>
        <source>无断点</source>
        <translation type="finished">无断点</translation>
    </message>
    <message>
        <source>无条件跳转到 offset 指定的相对位置。</source>
        <translation type="finished">无条件跳转到 offset 指定的相对位置。</translation>
    </message>
    <message>
        <source>无法保存文件: </source>
        <translation type="finished">无法保存文件: </translation>
    </message>
    <message>
        <source>无法创建文件夹（可能已存在）</source>
        <translation type="finished">无法创建文件夹（可能已存在）</translation>
    </message>
    <message>
        <source>无法创建文件：</source>
        <translation type="finished">无法创建文件：</translation>
    </message>
    <message>
        <source>无法启动异步执行: %1</source>
        <translation type="finished">无法启动异步执行: %1</translation>
    </message>
    <message>
        <source>无法打开文件: </source>
        <translation type="finished">无法打开文件: </translation>
    </message>
    <message>
        <source>无选中章节</source>
        <translation type="finished">无选中章节</translation>
    </message>
    <message>
        <source>无需堆分配，Value 拷贝仅 8 字节赋值。</source>
        <translation type="finished">无需堆分配，Value 拷贝仅 8 字节赋值。</translation>
    </message>
    <message>
        <source>时间对比</source>
        <translation type="finished">时间对比</translation>
    </message>
    <message>
        <source>是否现在保存？</source>
        <translation type="finished">是否现在保存？</translation>
    </message>
    <message>
        <source>显式关闭：OP_CLOSE_UPVALUE 指令显式关闭 upvalue（用于变量离开作用域时）。</source>
        <translation type="finished">显式关闭：OP_CLOSE_UPVALUE 指令显式关闭 upvalue（用于变量离开作用域时）。</translation>
    </message>
    <message>
        <source>普通断点</source>
        <translation type="finished">普通断点</translation>
    </message>
    <message>
        <source>暂无最近打开的工作区</source>
        <translation type="finished">暂无最近打开的工作区</translation>
    </message>
    <message>
        <source>替换</source>
        <translation type="finished">替换</translation>
    </message>
    <message>
        <source>替换为...</source>
        <translation type="finished">替换为...</translation>
    </message>
    <message>
        <source>最基础的异常处理形式。try 块中 throw 抛出异常对象，</source>
        <translation type="finished">最基础的异常处理形式。try 块中 throw 抛出异常对象，</translation>
    </message>
    <message>
        <source>最基础的条件断点形式。当循环变量 i 等于 50 时暂停。</source>
        <translation type="finished">最基础的条件断点形式。当循环变量 i 等于 50 时暂停。</translation>
    </message>
    <message>
        <source>最基础的调用栈形态：main 调用 foo，foo 返回后栈帧弹出。</source>
        <translation type="finished">最基础的调用栈形态：main 调用 foo，foo 返回后栈帧弹出。</translation>
    </message>
    <message>
        <source>最基础的闭包形式。内部函数捕获外层变量 x，</source>
        <translation type="finished">最基础的闭包形式。内部函数捕获外层变量 x，</translation>
    </message>
    <message>
        <source>最大化</source>
        <translation type="finished">最大化</translation>
    </message>
    <message>
        <source>最小化</source>
        <translation type="finished">最小化</translation>
    </message>
    <message>
        <source>最快</source>
        <translation type="finished">最快</translation>
    </message>
    <message>
        <source>最终传播到 main() 的 catch 块被捕获。</source>
        <translation type="finished">最终传播到 main() 的 catch 块被捕获。</translation>
    </message>
    <message>
        <source>最终传播到顶层导致程序终止。MiniLang 中未捕获异常会打印</source>
        <translation type="finished">最终传播到顶层导致程序终止。MiniLang 中未捕获异常会打印</translation>
    </message>
    <message>
        <source>最终传播到顶层，程序终止，print 语句不会执行。</source>
        <translation type="finished">最终传播到顶层，程序终止，print 语句不会执行。</translation>
    </message>
    <message>
        <source>最近打开</source>
        <translation type="finished">最近打开</translation>
    </message>
    <message>
        <source>未初始化的 VarDecl 用 LOAD_NULL 占位。</source>
        <translation type="finished">未初始化的 VarDecl 用 LOAD_NULL 占位。</translation>
    </message>
    <message>
        <source>未命名-%1</source>
        <translation type="finished">未命名-%1</translation>
    </message>
    <message>
        <source>未找到</source>
        <translation type="finished">未找到</translation>
    </message>
    <message>
        <source>未捕获异常（传播到顶层）</source>
        <translation type="finished">未捕获异常（传播到顶层）</translation>
    </message>
    <message>
        <source>未知的启动异常</source>
        <translation type="finished">未知的启动异常</translation>
    </message>
    <message>
        <source>未绑定控制器</source>
        <translation type="finished">未绑定控制器</translation>
    </message>
    <message>
        <source>未运行</source>
        <translation type="finished">未运行</translation>
    </message>
    <message>
        <source>未预期的未知异常（无法获取类型信息）</source>
        <translation type="finished">未预期的未知异常（无法获取类型信息）</translation>
    </message>
    <message>
        <source>未预期的错误 [%1]: %2</source>
        <translation type="finished">未预期的错误 [%1]: %2</translation>
    </message>
    <message>
        <source>本例 refCount=2 时调用 b.push(4)，触发 detach：先 release 原 refCount（→1），</source>
        <translation type="finished">本例 refCount=2 时调用 b.push(4)，触发 detach：先 release 原 refCount（→1），</translation>
    </message>
    <message>
        <source>本例 v = 2^46 - 1 仍在范围内，仍内联存储。</source>
        <translation type="finished">本例 v = 2^46 - 1 仍在范围内，仍内联存储。</translation>
    </message>
    <message>
        <source>本例 x = 1 后从未读取 x，整条赋值链可消除。</source>
        <translation type="finished">本例 x = 1 后从未读取 x，整条赋值链可消除。</translation>
    </message>
    <message>
        <source>本图示演示 PTR_TAG_BASE + 指针位模式，实际指针值会随运行而变化。</source>
        <translation type="finished">本图示演示 PTR_TAG_BASE + 指针位模式，实际指针值会随运行而变化。</translation>
    </message>
    <message>
        <source>条件为假时跳过循环体。</source>
        <translation type="finished">条件为假时跳过循环体。</translation>
    </message>
    <message>
        <source>条件分支 if (cond) {...} else {...}</source>
        <translation type="finished">条件分支 if (cond) {...} else {...}</translation>
    </message>
    <message>
        <source>条件分支。then/else 分支可以是 block（{}包裹）或单语句（无花括号）。</source>
        <translation type="finished">条件分支。then/else 分支可以是 block（{}包裹）或单语句（无花括号）。</translation>
    </message>
    <message>
        <source>条件在断点行命中时求值一次，结果为 truthy（非 0 / 非 null / 非 false）时暂停。</source>
        <translation type="finished">条件在断点行命中时求值一次，结果为 truthy（非 0 / 非 null / 非 false）时暂停。</translation>
    </message>
    <message>
        <source>条件循环。支持 break/continue。break 退出循环，continue 跳过当前迭代。</source>
        <translation type="finished">条件循环。支持 break/continue。break 退出循环，continue 跳过当前迭代。</translation>
    </message>
    <message>
        <source>条件断点</source>
        <translation type="finished">条件断点</translation>
    </message>
    <message>
        <source>条件断点在沙箱中求值时使用 Interpreter 的 EQ 实现。</source>
        <translation type="finished">条件断点在沙箱中求值时使用 Interpreter 的 EQ 实现。</translation>
    </message>
    <message>
        <source>条件断点求值发生未知异常（条件: </source>
        <translation type="finished">条件断点求值发生未知异常（条件: </translation>
    </message>
    <message>
        <source>条件断点求值异常: </source>
        <translation type="finished">条件断点求值异常: </translation>
    </message>
    <message>
        <source>条件断点求值时与正常运行时路径一致。</source>
        <translation type="finished">条件断点求值时与正常运行时路径一致。</translation>
    </message>
    <message>
        <source>条件断点求值时若 x 为 falsy，整个表达式直接返回 x（短路），不计算 y。</source>
        <translation type="finished">条件断点求值时若 x 为 falsy，整个表达式直接返回 x（短路），不计算 y。</translation>
    </message>
    <message>
        <source>条件断点求值时遵循相同语义：a &gt; 0 为 falsy 时不会求值 b &lt; 100。</source>
        <translation type="finished">条件断点求值时遵循相同语义：a &gt; 0 为 falsy 时不会求值 b &lt; 100。</translation>
    </message>
    <message>
        <source>条件求值时通过 boundInstance_ 缓存访问实例字段，与正常运行时语义一致。</source>
        <translation type="finished">条件求值时通过 boundInstance_ 缓存访问实例字段，与正常运行时语义一致。</translation>
    </message>
    <message>
        <source>条件表达式</source>
        <translation type="finished">条件表达式</translation>
    </message>
    <message>
        <source>条件跳转。在 while/if 场景下每次迭代触发。</source>
        <translation type="finished">条件跳转。在 while/if 场景下每次迭代触发。</translation>
    </message>
    <message>
        <source>构造数组。涉及堆分配 + GcManager::registerTracked。</source>
        <translation type="finished">构造数组。涉及堆分配 + GcManager::registerTracked。</translation>
    </message>
    <message>
        <source>构造新 ArrayData，refCount=1</source>
        <translation type="finished">构造新 ArrayData，refCount=1</translation>
    </message>
    <message>
        <source>构造新 InstanceData，refCount=1</source>
        <translation type="finished">构造新 InstanceData，refCount=1</translation>
    </message>
    <message>
        <source>构造新 StringData（&#x27;hello!&#x27;），s2 旧值 release（→1）</source>
        <translation type="finished">构造新 StringData（&#x27;hello!&#x27;），s2 旧值 release（→1）</translation>
    </message>
    <message>
        <source>构造新 StringData，refCount=1</source>
        <translation type="finished">构造新 StringData，refCount=1</translation>
    </message>
    <message>
        <source>构造闭包。涉及 ClosureData 分配 + upvalue 捕获。</source>
        <translation type="finished">构造闭包。涉及 ClosureData 分配 + upvalue 捕获。</translation>
    </message>
    <message>
        <source>析构时 Worker 未在 5 秒内停止，回退到 terminate()（进程退出阶段）</source>
        <translation type="finished">析构时 Worker 未在 5 秒内停止，回退到 terminate()（进程退出阶段）</translation>
    </message>
    <message>
        <source>析构钩子 onDestroyed 同步从 aliveSet_ 移除本指针，保证 aliveSet_ 与实际存活状态一致。</source>
        <translation type="finished">析构钩子 onDestroyed 同步从 aliveSet_ 移除本指针，保证 aliveSet_ 与实际存活状态一致。</translation>
    </message>
    <message>
        <source>查找</source>
        <translation type="finished">查找</translation>
    </message>
    <message>
        <source>查找...</source>
        <translation type="finished">查找...</translation>
    </message>
    <message>
        <source>查看 AST 树形图 (Ctrl+Shift+A)</source>
        <translation type="finished">查看 AST 树形图 (Ctrl+Shift+A)</translation>
    </message>
    <message>
        <source>查看 Token / IR / 字节码</source>
        <translation type="finished">查看 Token / IR / 字节码</translation>
    </message>
    <message>
        <source>查看答案</source>
        <translation type="finished">查看答案</translation>
    </message>
    <message>
        <source>标准差 (μs)</source>
        <translation type="finished">标准差 (μs)</translation>
    </message>
    <message>
        <source>标记从根集可达的对象，释放不可达的循环孤岛。&lt;/p&gt;</source>
        <translation type="finished">标记从根集可达的对象，释放不可达的循环孤岛。&lt;/p&gt;</translation>
    </message>
    <message>
        <source>标量内联：IEEE 754 double 直接存入 NaN-box 的 64 位。注意 tag bits 与 NaN 模式不冲突。</source>
        <translation type="finished">标量内联：IEEE 754 double 直接存入 NaN-box 的 64 位。注意 tag bits 与 NaN 模式不冲突。</translation>
    </message>
    <message>
        <source>标量内联：bool 编码为 int48（0/1），tag bits 与 int 不同（INT_TAG_BASE=0x7FF8 vs BOOL_TAG_BASE=0x7FF9）。</source>
        <translation type="finished">标量内联：bool 编码为 int48（0/1），tag bits 与 int 不同（INT_TAG_BASE=0x7FF8 vs BOOL_TAG_BASE=0x7FF9）。</translation>
    </message>
    <message>
        <source>标量内联：int48 直接存入 NaN-box 的低 48 位，无需堆分配。范围 |v| &lt; 2^47。</source>
        <translation type="finished">标量内联：int48 直接存入 NaN-box 的低 48 位，无需堆分配。范围 |v| &lt; 2^47。</translation>
    </message>
    <message>
        <source>标量内联：唯一编码 NULL_BITS=0x7FFA&lt;&lt;48，payload 全 0。</source>
        <translation type="finished">标量内联：唯一编码 NULL_BITS=0x7FFA&lt;&lt;48，payload 全 0。</translation>
    </message>
    <message>
        <source>栈/堆：Closure 引用计数归零，upvalue 数组释放，堆值（若无引用）被回收</source>
        <translation type="finished">栈/堆：Closure 引用计数归零，upvalue 数组释放，堆值（若无引用）被回收</translation>
    </message>
    <message>
        <source>栈展开是异常传播的核心机制。未匹配的帧被弹出，</source>
        <translation type="finished">栈展开是异常传播的核心机制。未匹配的帧被弹出，</translation>
    </message>
    <message>
        <source>栈展开期间不可中断（除非再次抛出异常）。</source>
        <translation type="finished">栈展开期间不可中断（除非再次抛出异常）。</translation>
    </message>
    <message>
        <source>栈帧</source>
        <translation type="finished">栈帧</translation>
    </message>
    <message>
        <source>栈式 VM</source>
        <translation type="finished">栈式 VM</translation>
    </message>
    <message>
        <source>栈推入外层索引。循环内每次迭代泄漏 2 个栈值，约 512 次循环触发栈溢出。仅影响非 IR 路径。</source>
        <translation type="finished">栈推入外层索引。循环内每次迭代泄漏 2 个栈值，约 512 次循环触发栈溢出。仅影响非 IR 路径。</translation>
    </message>
    <message>
        <source>栈：OP_CLOSE_UPVALUE 执行后，upvalue 从栈指向堆</source>
        <translation type="finished">栈：OP_CLOSE_UPVALUE 执行后，upvalue 从栈指向堆</translation>
    </message>
    <message>
        <source>栈：OP_GET_UPVALUE 推入 upvalue 值，OP_SET_UPVALUE 弹出值写入 upvalue</source>
        <translation type="finished">栈：OP_GET_UPVALUE 推入 upvalue 值，OP_SET_UPVALUE 弹出值写入 upvalue</translation>
    </message>
    <message>
        <source>栈：Upvalue 指针指向栈槽，openUpvalues_ 插入条目</source>
        <translation type="finished">栈：Upvalue 指针指向栈槽，openUpvalues_ 插入条目</translation>
    </message>
    <message>
        <source>栈：finally/catch 块执行完毕后，栈状态恢复到 try 之前</source>
        <translation type="finished">栈：finally/catch 块执行完毕后，栈状态恢复到 try 之前</translation>
    </message>
    <message>
        <source>栈：异常对象弹出，绑定到 catch 变量，恢复正常执行</source>
        <translation type="finished">栈：异常对象弹出，绑定到 catch 变量，恢复正常执行</translation>
    </message>
    <message>
        <source>栈：弹出 OP_CLOSURE 的操作数，推入 Closure 对象</source>
        <translation type="finished">栈：弹出 OP_CLOSURE 的操作数，推入 Closure 对象</translation>
    </message>
    <message>
        <source>栈：弹出当前操作数，标记异常状态</source>
        <translation type="finished">栈：弹出当前操作数，标记异常状态</translation>
    </message>
    <message>
        <source>栈：恢复正常执行状态，清除异常标志</source>
        <translation type="finished">栈：恢复正常执行状态，清除异常标志</translation>
    </message>
    <message>
        <source>栈：栈槽弹出，Upvalue.value 从栈复制到堆，openUpvalues_ 移除条目</source>
        <translation type="finished">栈：栈槽弹出，Upvalue.value 从栈复制到堆，openUpvalues_ 移除条目</translation>
    </message>
    <message>
        <source>栈：逐帧弹出，局部变量销毁，帧计数器递减</source>
        <translation type="finished">栈：逐帧弹出，局部变量销毁，帧计数器递减</translation>
    </message>
    <message>
        <source>栈：逐帧搜索，未匹配的帧被标记为待展开</source>
        <translation type="finished">栈：逐帧搜索，未匹配的帧被标记为待展开</translation>
    </message>
    <message>
        <source>树遍历解释器</source>
        <translation type="finished">树遍历解释器</translation>
    </message>
    <message>
        <source>样例代码：</source>
        <translation type="finished">样例代码：</translation>
    </message>
    <message>
        <source>根因：Formatter 的空行决策必须考虑 ExportStmt 包装的内部声明节点类型。</source>
        <translation type="finished">根因：Formatter 的空行决策必须考虑 ExportStmt 包装的内部声明节点类型。</translation>
    </message>
    <message>
        <source>根因：IR 路径语句上下文必须用 visitStatement 统一包装器处理 POP，不能直接调用 visitNode。</source>
        <translation type="finished">根因：IR 路径语句上下文必须用 visitStatement 统一包装器处理 POP，不能直接调用 visitNode。</translation>
    </message>
    <message>
        <source>根因：RegisterVM stepCallback_ 在&#x27;直接 return&#x27;路径被跳过，调试器单步模式丢失步进事件。</source>
        <translation type="finished">根因：RegisterVM stepCallback_ 在&#x27;直接 return&#x27;路径被跳过，调试器单步模式丢失步进事件。</translation>
    </message>
    <message>
        <source>根因：Value::equals() 是数值相等性而非类型严格相等。常量池去重需要类型区分的场景必须</source>
        <translation type="finished">根因：Value::equals() 是数值相等性而非类型严格相等。常量池去重需要类型区分的场景必须</translation>
    </message>
    <message>
        <source>根因：resolveUpvalue 惰性策略对 3+ 层嵌套闭包失效，中间函数需要预先声明捕获哪些外层变量。</source>
        <translation type="finished">根因：resolveUpvalue 惰性策略对 3+ 层嵌套闭包失效，中间函数需要预先声明捕获哪些外层变量。</translation>
    </message>
    <message>
        <source>根因：三后端必须对默认参数求值能力一致。修复：Parser 层拒绝复杂表达式，仅允许字面量 + 负数字面量。</source>
        <translation type="finished">根因：三后端必须对默认参数求值能力一致。修复：Parser 层拒绝复杂表达式，仅允许字面量 + 负数字面量。</translation>
    </message>
    <message>
        <source>根因：修改整体替换语义指令时必须同步审计 visitIndexAssign、visitMemberAssign、visitMethodCall 三个 emit 点，</source>
        <translation type="finished">根因：修改整体替换语义指令时必须同步审计 visitIndexAssign、visitMemberAssign、visitMethodCall 三个 emit 点，</translation>
    </message>
    <message>
        <source>根因：调用栈帧的 line 字段需要随执行进度更新，不能只在调用时初始化。</source>
        <translation type="finished">根因：调用栈帧的 line 字段需要随执行进度更新，不能只在调用时初始化。</translation>
    </message>
    <message>
        <source>根因：路径安全校验需拒绝所有 &#x27;X:&#x27; 开头形式，不能仅检测 &#x27;X:/&#x27;。</source>
        <translation type="finished">根因：路径安全校验需拒绝所有 &#x27;X:&#x27; 开头形式，不能仅检测 &#x27;X:/&#x27;。</translation>
    </message>
    <message>
        <source>根因：路径规范化必须在所有访问 moduleCache_ 的入口一致。</source>
        <translation type="finished">根因：路径规范化必须在所有访问 moduleCache_ 的入口一致。</translation>
    </message>
    <message>
        <source>格式化</source>
        <translation type="finished">格式化</translation>
    </message>
    <message>
        <source>格式化代码</source>
        <translation type="finished">格式化代码</translation>
    </message>
    <message>
        <source>格式化代码 (Ctrl+Shift+F)</source>
        <translation type="finished">格式化代码 (Ctrl+Shift+F)</translation>
    </message>
    <message>
        <source>模块导入</source>
        <translation type="finished">模块导入</translation>
    </message>
    <message>
        <source>模块系统。import/export 限制在顶层作用域（Parser 在 statement() 入口显式拒绝）。</source>
        <translation type="finished">模块系统。import/export 限制在顶层作用域（Parser 在 statement() 入口显式拒绝）。</translation>
    </message>
    <message>
        <source>模块路径安全</source>
        <translation type="finished">模块路径安全</translation>
    </message>
    <message>
        <source>模块路径非空校验，路径遍历防护（拒绝 .. 父目录引用和绝对路径）。</source>
        <translation type="finished">模块路径非空校验，路径遍历防护（拒绝 .. 父目录引用和绝对路径）。</translation>
    </message>
    <message>
        <source>模块路径：&#x27;C:foo&#x27; 漏网</source>
        <translation type="finished">模块路径：&#x27;C:foo&#x27; 漏网</translation>
    </message>
    <message>
        <source>此时上一轮残留的循环容器 refCount&gt;0 仍 aliveSet_，本轮新建容器尚未注册，安全。</source>
        <translation type="finished">此时上一轮残留的循环容器 refCount&gt;0 仍 aliveSet_，本轮新建容器尚未注册，安全。</translation>
    </message>
    <message>
        <source>步入</source>
        <translation type="finished">步入</translation>
    </message>
    <message>
        <source>步骤: %1 | 光标: 行 %2, 列 %3</source>
        <translation type="finished">步骤: %1 | 光标: 行 %2, 列 %3</translation>
    </message>
    <message>
        <source>步骤: 源码 | 光标: 行 1, 列 1</source>
        <translation type="finished">步骤: 源码 | 光标: 行 1, 列 1</translation>
    </message>
    <message>
        <source>死代码消除（未使用的赋值）</source>
        <translation type="finished">死代码消除（未使用的赋值）</translation>
    </message>
    <message>
        <source>每一层 countdown() 都没有 try/catch，帧依次被弹出。</source>
        <translation type="finished">每一层 countdown() 都没有 try/catch，帧依次被弹出。</translation>
    </message>
    <message>
        <source>每一层递归帧都会被检查是否有 catch 块。</source>
        <translation type="finished">每一层递归帧都会被检查是否有 catch 块。</translation>
    </message>
    <message>
        <source>每一帧检查是否存在 try/catch 结构，若存在则跳转到 catch 块。</source>
        <translation type="finished">每一帧检查是否存在 try/catch 结构，若存在则跳转到 catch 块。</translation>
    </message>
    <message>
        <source>每个帧在栈展开时被弹出，局部变量被销毁。</source>
        <translation type="finished">每个帧在栈展开时被弹出，局部变量被销毁。</translation>
    </message>
    <message>
        <source>每次调用 acc(x) 时，闭包通过 upvalue 修改堆上的 total。</source>
        <translation type="finished">每次调用 acc(x) 时，闭包通过 upvalue 修改堆上的 total。</translation>
    </message>
    <message>
        <source>每次调用 counter() 时，闭包通过 upvalue 访问并修改堆上的 x。</source>
        <translation type="finished">每次调用 counter() 时，闭包通过 upvalue 访问并修改堆上的 x。</translation>
    </message>
    <message>
        <source>每隔 100 次迭代暂停一次。常用于在大循环中观察周期性状态，</source>
        <translation type="finished">每隔 100 次迭代暂停一次。常用于在大循环中观察周期性状态，</translation>
    </message>
    <message>
        <source>比值</source>
        <translation type="finished">比值</translation>
    </message>
    <message>
        <source>注册为 O(1) 操作（vector::push_back + unordered_set::insert）。</source>
        <translation type="finished">注册为 O(1) 操作（vector::push_back + unordered_set::insert）。</translation>
    </message>
    <message>
        <source>浮点数直接以 IEEE 754 double 的 64 位原始位存储，</source>
        <translation type="finished">浮点数直接以 IEEE 754 double 的 64 位原始位存储，</translation>
    </message>
    <message>
        <source>消除 MOVE 后常触发 DCE 二次优化，进一步减少指令。</source>
        <translation type="finished">消除 MOVE 后常触发 DCE 二次优化，进一步减少指令。</translation>
    </message>
    <message>
        <source>深度</source>
        <translation type="finished">深度</translation>
    </message>
    <message>
        <source>深拷贝后修改不影响 a，a 仍是 [1,2,3]</source>
        <translation type="finished">深拷贝后修改不影响 a，a 仍是 [1,2,3]</translation>
    </message>
    <message>
        <source>清空</source>
        <translation type="finished">清空</translation>
    </message>
    <message>
        <source>清空其子元素打破循环 → refCount 自然降为 0 → 节点析构 → onDestroyed 从 aliveSet_ 移除。</source>
        <translation type="finished">清空其子元素打破循环 → refCount 自然降为 0 → 节点析构 → onDestroyed 从 aliveSet_ 移除。</translation>
    </message>
    <message>
        <source>清空输出面板</source>
        <translation type="finished">清空输出面板</translation>
    </message>
    <message>
        <source>源码</source>
        <translation type="finished">源码</translation>
    </message>
    <message>
        <source>演示 b = a 之后修改 b 触发 COW detach。</source>
        <translation type="finished">演示 b = a 之后修改 b 触发 COW detach。</translation>
    </message>
    <message>
        <source>演示 var a = [1,2,3] 的构造、拷贝与释放。</source>
        <translation type="finished">演示 var a = [1,2,3] 的构造、拷贝与释放。</translation>
    </message>
    <message>
        <source>状态</source>
        <translation type="finished">状态</translation>
    </message>
    <message>
        <source>环形容器泄漏：a.append(a) 形成自环，refCount ≥ 2 永不归零。</source>
        <translation type="finished">环形容器泄漏：a.append(a) 形成自环，refCount ≥ 2 永不归零。</translation>
    </message>
    <message>
        <source>现代化 MiniLang 编程语言开发环境</source>
        <translation type="finished">现代化 MiniLang 编程语言开发环境</translation>
    </message>
    <message>
        <source>理解 IR 中间表示与寄存器式 VM：AST → IR → RegBytecode → RegisterVM。

</source>
        <translation type="finished">理解 IR 中间表示与寄存器式 VM：AST → IR → RegBytecode → RegisterVM。

</translation>
    </message>
    <message>
        <source>理解 MiniLang 的内存模型：Value 8 字节 NaN-boxing，堆类型侵入式 RefCounted，数组/字典 Copy-On-Write。

</source>
        <translation type="finished">理解 MiniLang 的内存模型：Value 8 字节 NaN-boxing，堆类型侵入式 RefCounted，数组/字典 Copy-On-Write。

</translation>
    </message>
    <message>
        <source>理解 Parser 的工作方式：手写递归下降，按优先级链解析表达式。

</source>
        <translation type="finished">理解 Parser 的工作方式：手写递归下降，按优先级链解析表达式。

</translation>
    </message>
    <message>
        <source>理解三后端一致性约束：同一 MiniLang 源码在 Interpreter / StackVM / RegisterVM 三条路径上必须产生相同语义结果。

</source>
        <translation type="finished">理解三后端一致性约束：同一 MiniLang 源码在 Interpreter / StackVM / RegisterVM 三条路径上必须产生相同语义结果。

</translation>
    </message>
    <message>
        <source>理解字节码编译与栈式 VM 执行：AST → BytecodeChunk → VM。

</source>
        <translation type="finished">理解字节码编译与栈式 VM 执行：AST → BytecodeChunk → VM。

</translation>
    </message>
    <message>
        <source>理解解释器的执行模型：通过 Visitor 模式递归遍历 AST 节点求值。

</source>
        <translation type="finished">理解解释器的执行模型：通过 Visitor 模式递归遍历 AST 节点求值。

</translation>
    </message>
    <message>
        <source>理解词法分析器的职责：将源代码字符串切分为有意义的 Token 序列，并分离注释。

</source>
        <translation type="finished">理解词法分析器的职责：将源代码字符串切分为有意义的 Token 序列，并分离注释。

</translation>
    </message>
    <message>
        <source>用法: reload &quot;模块路径&quot; 或 reload all</source>
        <translation type="finished">用法: reload &quot;模块路径&quot; 或 reload all</translation>
    </message>
    <message>
        <source>相互递归 isEven/isOdd</source>
        <translation type="finished">相互递归 isEven/isOdd</translation>
    </message>
    <message>
        <source>相互递归：栈帧交替出现 isEven / isOdd，每帧 n 递减。注意 isOdd 在 isEven 之后定义但能被调用（前向引用通过 preScanModuleGlobals 修复）。</source>
        <translation type="finished">相互递归：栈帧交替出现 isEven / isOdd，每帧 n 递减。注意 isOdd 在 isEven 之后定义但能被调用（前向引用通过 preScanModuleGlobals 修复）。</translation>
    </message>
    <message>
        <source>确定</source>
        <translation type="finished">确定</translation>
    </message>
    <message>
        <source>确定删除 %1 ？</source>
        <translation type="finished">确定删除 %1 ？</translation>
    </message>
    <message>
        <source>确认删除</source>
        <translation type="finished">确认删除</translation>
    </message>
    <message>
        <source>移除条件</source>
        <translation type="finished">移除条件</translation>
    </message>
    <message>
        <source>程序无响应，即将强制终止</source>
        <translation type="finished">程序无响应，即将强制终止</translation>
    </message>
    <message>
        <source>程序正在运行，请先停止后再使用 REPL</source>
        <translation type="finished">程序正在运行，请先停止后再使用 REPL</translation>
    </message>
    <message>
        <source>程序正在运行，请先停止后再使用 reload</source>
        <translation type="finished">程序正在运行，请先停止后再使用 reload</translation>
    </message>
    <message>
        <source>程序运行中，REPL 已暂停</source>
        <translation type="finished">程序运行中，REPL 已暂停</translation>
    </message>
    <message>
        <source>立即调用函数表达式（IIFE）</source>
        <translation type="finished">立即调用函数表达式（IIFE）</translation>
    </message>
    <message>
        <source>等待执行...</source>
        <translation type="finished">等待执行...</translation>
    </message>
    <message>
        <source>答案已显示</source>
        <translation type="finished">答案已显示</translation>
    </message>
    <message>
        <source>简单 try/catch（单层捕获）</source>
        <translation type="finished">简单 try/catch（单层捕获）</translation>
    </message>
    <message>
        <source>简单函数调用</source>
        <translation type="finished">简单函数调用</translation>
    </message>
    <message>
        <source>简单相等条件（i == 50）</source>
        <translation type="finished">简单相等条件（i == 50）</translation>
    </message>
    <message>
        <source>简单闭包（捕获单个变量）</source>
        <translation type="finished">简单闭包（捕获单个变量）</translation>
    </message>
    <message>
        <source>类与继承</source>
        <translation type="finished">类与继承</translation>
    </message>
    <message>
        <source>类型</source>
        <translation type="finished">类型</translation>
    </message>
    <message>
        <source>类声明。extends 后父类名为字符串运行时查找。</source>
        <translation type="finished">类声明。extends 后父类名为字符串运行时查找。</translation>
    </message>
    <message>
        <source>类定义不创建实例，仅注册到 classRegistry_</source>
        <translation type="finished">类定义不创建实例，仅注册到 classRegistry_</translation>
    </message>
    <message>
        <source>类实例化循环（10000 次）</source>
        <translation type="finished">类实例化循环（10000 次）</translation>
    </message>
    <message>
        <source>类方法 Point.new()</source>
        <translation type="finished">类方法 Point.new()</translation>
    </message>
    <message>
        <source>类方法分派</source>
        <translation type="finished">类方法分派</translation>
    </message>
    <message>
        <source>类方法自动预留 slot 0 给 this，字段按继承链展平存储。</source>
        <translation type="finished">类方法自动预留 slot 0 给 this，字段按继承链展平存储。</translation>
    </message>
    <message>
        <source>类构造：弹出 N 个参数 + 类模板，创建 InstanceData 并调用 init 方法。</source>
        <translation type="finished">类构造：弹出 N 个参数 + 类模板，创建 InstanceData 并调用 init 方法。</translation>
    </message>
    <message>
        <source>纯算术循环。栈式 VM 与 RegisterVM 在热路径上优势最明显——单条 OP_ADD 比访问者模式 dispatch 快 5-10 倍。</source>
        <translation type="finished">纯算术循环。栈式 VM 与 RegisterVM 在热路径上优势最明显——单条 OP_ADD 比访问者模式 dispatch 快 5-10 倍。</translation>
    </message>
    <message>
        <source>经典计数器模式。闭包通过 upvalue 修改外层变量 count，</source>
        <translation type="finished">经典计数器模式。闭包通过 upvalue 修改外层变量 count，</translation>
    </message>
    <message>
        <source>继续</source>
        <translation type="finished">继续</translation>
    </message>
    <message>
        <source>继续执行发生未知异常</source>
        <translation type="finished">继续执行发生未知异常</translation>
    </message>
    <message>
        <source>继续执行失败: %1</source>
        <translation type="finished">继续执行失败: %1</translation>
    </message>
    <message>
        <source>继续运行到下一个断点 (F9)</source>
        <translation type="finished">继续运行到下一个断点 (F9)</translation>
    </message>
    <message>
        <source>编码了 upvalue 数量与每个 upvalue 的位置（栈槽或上层 upvalue）。</source>
        <translation type="finished">编码了 upvalue 数量与每个 upvalue 的位置（栈槽或上层 upvalue）。</translation>
    </message>
    <message>
        <source>编译分析</source>
        <translation type="finished">编译分析</translation>
    </message>
    <message>
        <source>编译分析面板</source>
        <translation type="finished">编译分析面板</translation>
    </message>
    <message>
        <source>编译管线</source>
        <translation type="finished">编译管线</translation>
    </message>
    <message>
        <source>编译管线可视化</source>
        <translation type="finished">编译管线可视化</translation>
    </message>
    <message>
        <source>编辑</source>
        <translation type="finished">编辑</translation>
    </message>
    <message>
        <source>而非直接指向 outer 的栈槽。这形成 upvalue 链。</source>
        <translation type="finished">而非直接指向 outer 的栈槽。这形成 upvalue 链。</translation>
    </message>
    <message>
        <source>节点: %1
位置: %2:%3
子节点: %4</source>
        <translation type="finished">节点: %1
位置: %2:%3
子节点: %4</translation>
    </message>
    <message>
        <source>节点行号: </source>
        <translation type="finished">节点行号: </translation>
    </message>
    <message>
        <source>若 try 块中抛出异常，catch 块执行清理逻辑。</source>
        <translation type="finished">若 try 块中抛出异常，catch 块执行清理逻辑。</translation>
    </message>
    <message>
        <source>若 upvalue 指向栈槽，VM 在 openUpvalues_ 中查找或创建条目。</source>
        <translation type="finished">若 upvalue 指向栈槽，VM 在 openUpvalues_ 中查找或创建条目。</translation>
    </message>
    <message>
        <source>若 v 超出范围（|v| ≥ 2^47）则触发装箱为 BoxedIntData* 堆对象。</source>
        <translation type="finished">若 v 超出范围（|v| ≥ 2^47）则触发装箱为 BoxedIntData* 堆对象。</translation>
    </message>
    <message>
        <source>若内层不匹配（或再次抛出），异常继续传播到外层 catch。</source>
        <translation type="finished">若内层不匹配（或再次抛出），异常继续传播到外层 catch。</translation>
    </message>
    <message>
        <source>若常量池将 0 与 0.0 共享，可能让某一后端走错算术分支。</source>
        <translation type="finished">若常量池将 0 与 0.0 共享，可能让某一后端走错算术分支。</translation>
    </message>
    <message>
        <source>若异常未被捕获，传播到顶层导致程序终止，设置错误标志。</source>
        <translation type="finished">若异常未被捕获，传播到顶层导致程序终止，设置错误标志。</translation>
    </message>
    <message>
        <source>若所有层都没有 catch，异常传播到顶层。</source>
        <translation type="finished">若所有层都没有 catch，异常传播到顶层。</translation>
    </message>
    <message>
        <source>若无异常，catch 块不执行（需在 try 块末尾也放清理代码）。</source>
        <translation type="finished">若无异常，catch 块不执行（需在 try 块末尾也放清理代码）。</translation>
    </message>
    <message>
        <source>若标准差 &gt; 10% 平均值，可能是 GC 周期或系统调度影响。&lt;/small&gt;&lt;/p&gt;</source>
        <translation type="finished">若标准差 &gt; 10% 平均值，可能是 GC 周期或系统调度影响。&lt;/small&gt;&lt;/p&gt;</translation>
    </message>
    <message>
        <source>若需要每个闭包捕获不同的值，需在每次迭代中创建新作用域</source>
        <translation type="finished">若需要每个闭包捕获不同的值，需在每次迭代中创建新作用域</translation>
    </message>
    <message>
        <source>菜单</source>
        <translation type="finished">菜单</translation>
    </message>
    <message>
        <source>行</source>
        <translation type="finished">行</translation>
    </message>
    <message>
        <source>行 %1</source>
        <translation type="finished">行 %1</translation>
    </message>
    <message>
        <source>行 %1 的条件表达式（为空则变为无条件断点）: mlTr(</source>
        <translation type="finished">行 %1 的条件表达式（为空则变为无条件断点）: mlTr(</translation>
    </message>
    <message>
        <source>行 1</source>
        <translation type="finished">行 1</translation>
    </message>
    <message>
        <source>行号</source>
        <translation type="finished">行号</translation>
    </message>
    <message>
        <source>视图</source>
        <translation type="finished">视图</translation>
    </message>
    <message>
        <source>解析失败：%1</source>
        <translation type="finished">解析失败：%1</translation>
    </message>
    <message>
        <source>解析异常</source>
        <translation type="finished">解析异常</translation>
    </message>
    <message>
        <source>解码时通过符号位扩展恢复 int64 值。</source>
        <translation type="finished">解码时通过符号位扩展恢复 int64 值。</translation>
    </message>
    <message>
        <source>解释器未初始化</source>
        <translation type="finished">解释器未初始化</translation>
    </message>
    <message>
        <source>计数器模式（通过闭包修改外层变量）</source>
        <translation type="finished">计数器模式（通过闭包修改外层变量）</translation>
    </message>
    <message>
        <source>设置断点条件</source>
        <translation type="finished">设置断点条件</translation>
    </message>
    <message>
        <source>设置条件... (行 %1)</source>
        <translation type="finished">设置条件... (行 %1)</translation>
    </message>
    <message>
        <source>访问阶段：闭包调用时（OP_CALL），VM 通过 OP_GET_UPVALUE / OP_SET_UPVALUE </source>
        <translation type="finished">访问阶段：闭包调用时（OP_CALL），VM 通过 OP_GET_UPVALUE / OP_SET_UPVALUE </translation>
    </message>
    <message>
        <source>词法Token</source>
        <translation type="finished">词法Token</translation>
    </message>
    <message>
        <source>词法分析异常</source>
        <translation type="finished">词法分析异常</translation>
    </message>
    <message>
        <source>词法异常</source>
        <translation type="finished">词法异常</translation>
    </message>
    <message>
        <source>词法错误 (行 %1, 列 %2): %3</source>
        <translation type="finished">词法错误 (行 %1, 列 %2): %3</translation>
    </message>
    <message>
        <source>词法错误: %1</source>
        <translation type="finished">词法错误: %1</translation>
    </message>
    <message>
        <source>词素</source>
        <translation type="finished">词素</translation>
    </message>
    <message>
        <source>语法探索器</source>
        <translation type="finished">语法探索器</translation>
    </message>
    <message>
        <source>语法示例</source>
        <translation type="finished">语法示例</translation>
    </message>
    <message>
        <source>语法错误 (行 %1, 列 %2): %3</source>
        <translation type="finished">语法错误 (行 %1, 列 %2): %3</translation>
    </message>
    <message>
        <source>说明：</source>
        <translation type="finished">说明：</translation>
    </message>
    <message>
        <source>请先在主编辑器中输入并编译代码</source>
        <translation type="finished">请先在主编辑器中输入并编译代码</translation>
    </message>
    <message>
        <source>请先打开或新建一个文件再进行编译分析。</source>
        <translation type="finished">请先打开或新建一个文件再进行编译分析。</translation>
    </message>
    <message>
        <source>请先选择产生式</source>
        <translation type="finished">请先选择产生式</translation>
    </message>
    <message>
        <source>请先选择场景</source>
        <translation type="finished">请先选择场景</translation>
    </message>
    <message>
        <source>请先选择章节</source>
        <translation type="finished">请先选择章节</translation>
    </message>
    <message>
        <source>请先选择题目</source>
        <translation type="finished">请先选择题目</translation>
    </message>
    <message>
        <source>请考虑简化代码或使用字节码视图查看。</source>
        <translation type="finished">请考虑简化代码或使用字节码视图查看。</translation>
    </message>
    <message>
        <source>请选择产生式</source>
        <translation type="finished">请选择产生式</translation>
    </message>
    <message>
        <source>请选择优化 pass</source>
        <translation type="finished">请选择优化 pass</translation>
    </message>
    <message>
        <source>请选择场景</source>
        <translation type="finished">请选择场景</translation>
    </message>
    <message>
        <source>请选择章节</source>
        <translation type="finished">请选择章节</translation>
    </message>
    <message>
        <source>请选择题目</source>
        <translation type="finished">请选择题目</translation>
    </message>
    <message>
        <source>读取全局变量。比 OP_GET_LOCAL 稍慢（需查 hash 表）。</source>
        <translation type="finished">读取全局变量。比 OP_GET_LOCAL 稍慢（需查 hash 表）。</translation>
    </message>
    <message>
        <source>读取全局槽位的值并压栈。slot 在编译期由 GlobalSlotAllocator 分配。</source>
        <translation type="finished">读取全局槽位的值并压栈。slot 在编译期由 GlobalSlotAllocator 分配。</translation>
    </message>
    <message>
        <source>读取局部变量槽位。在 StackVM 中是热点——每次变量引用都触发一次栈读取。</source>
        <translation type="finished">读取局部变量槽位。在 StackVM 中是热点——每次变量引用都触发一次栈读取。</translation>
    </message>
    <message>
        <source>读取闭包捕获变量。比 OP_GET_LOCAL 稍慢（需通过 upvalue 链间接访问）。</source>
        <translation type="finished">读取闭包捕获变量。比 OP_GET_LOCAL 稍慢（需通过 upvalue 链间接访问）。</translation>
    </message>
    <message>
        <source>读取闭包捕获的外层变量。若 upvalue 仍开放则从栈帧读取，已关闭则从堆读取。</source>
        <translation type="finished">读取闭包捕获的外层变量。若 upvalue 仍开放则从栈帧读取，已关闭则从堆读取。</translation>
    </message>
    <message>
        <source>调用栈</source>
        <translation type="finished">调用栈</translation>
    </message>
    <message>
        <source>调用栈回调发生未知异常</source>
        <translation type="finished">调用栈回调发生未知异常</translation>
    </message>
    <message>
        <source>调用栈回调异常: </source>
        <translation type="finished">调用栈回调异常: </translation>
    </message>
    <message>
        <source>调用栈顶帧行号：调用点 vs 当前行</source>
        <translation type="finished">调用栈顶帧行号：调用点 vs 当前行</translation>
    </message>
    <message>
        <source>调用栈顶闭包：弹出 N 个参数 + 1 个闭包值，执行后压入返回值。</source>
        <translation type="finished">调用栈顶闭包：弹出 N 个参数 + 1 个闭包值，执行后压入返回值。</translation>
    </message>
    <message>
        <source>调试</source>
        <translation type="finished">调试</translation>
    </message>
    <message>
        <source>调试中</source>
        <translation type="finished">调试中</translation>
    </message>
    <message>
        <source>调试信息更新发生未知异常</source>
        <translation type="finished">调试信息更新发生未知异常</translation>
    </message>
    <message>
        <source>调试信息更新失败: %1</source>
        <translation type="finished">调试信息更新失败: %1</translation>
    </message>
    <message>
        <source>调试器 / 调用栈显示</source>
        <translation type="finished">调试器 / 调用栈显示</translation>
    </message>
    <message>
        <source>调试器单步模式丢失步进事件。</source>
        <translation type="finished">调试器单步模式丢失步进事件。</translation>
    </message>
    <message>
        <source>调试器暂停时，调用栈顶显示的是调用点行号，而非当前执行行号，导致用户困惑。</source>
        <translation type="finished">调试器暂停时，调用栈顶显示的是调用点行号，而非当前执行行号，导致用户困惑。</translation>
    </message>
    <message>
        <source>调试程序</source>
        <translation type="finished">调试程序</translation>
    </message>
    <message>
        <source>调试面板</source>
        <translation type="finished">调试面板</translation>
    </message>
    <message>
        <source>资源清理（如关闭文件、释放锁）应放在 catch 块中确保执行。</source>
        <translation type="finished">资源清理（如关闭文件、释放锁）应放在 catch 块中确保执行。</translation>
    </message>
    <message>
        <source>资源管理器</source>
        <translation type="finished">资源管理器</translation>
    </message>
    <message>
        <source>跨出</source>
        <translation type="finished">跨出</translation>
    </message>
    <message>
        <source>跨函数异常传播</source>
        <translation type="finished">跨函数异常传播</translation>
    </message>
    <message>
        <source>跨过</source>
        <translation type="finished">跨过</translation>
    </message>
    <message>
        <source>输入 &#x27;help&#x27; 查看帮助，输入 &#x27;clear&#x27; 清空输出。</source>
        <translation type="finished">输入 &#x27;help&#x27; 查看帮助，输入 &#x27;clear&#x27; 清空输出。</translation>
    </message>
    <message>
        <source>输入 MiniLang 表达式或语句，按回车执行。</source>
        <translation type="finished">输入 MiniLang 表达式或语句，按回车执行。</translation>
    </message>
    <message>
        <source>输出</source>
        <translation type="finished">输出</translation>
    </message>
    <message>
        <source>输出面板</source>
        <translation type="finished">输出面板</translation>
    </message>
    <message>
        <source>运算符与短路求值</source>
        <translation type="finished">运算符与短路求值</translation>
    </message>
    <message>
        <source>运算符优先级链。整数除法截断向零（三后端统一）。</source>
        <translation type="finished">运算符优先级链。整数除法截断向零（三后端统一）。</translation>
    </message>
    <message>
        <source>运行</source>
        <translation type="finished">运行</translation>
    </message>
    <message>
        <source>运行三后端对比</source>
        <translation type="finished">运行三后端对比</translation>
    </message>
    <message>
        <source>运行中</source>
        <translation type="finished">运行中</translation>
    </message>
    <message>
        <source>运行中...</source>
        <translation type="finished">运行中...</translation>
    </message>
    <message>
        <source>运行剖析</source>
        <translation type="finished">运行剖析</translation>
    </message>
    <message>
        <source>运行样例</source>
        <translation type="finished">运行样例</translation>
    </message>
    <message>
        <source>运行程序</source>
        <translation type="finished">运行程序</translation>
    </message>
    <message>
        <source>运行程序 (F5)</source>
        <translation type="finished">运行程序 (F5)</translation>
    </message>
    <message>
        <source>运行结束</source>
        <translation type="finished">运行结束</translation>
    </message>
    <message>
        <source>运行结果：</source>
        <translation type="finished">运行结果：</translation>
    </message>
    <message>
        <source>运行验证</source>
        <translation type="finished">运行验证</translation>
    </message>
    <message>
        <source>返回值若被使用则存入 vreg，否则由 CALL_POP 弃用。</source>
        <translation type="finished">返回值若被使用则存入 vreg，否则由 CALL_POP 弃用。</translation>
    </message>
    <message>
        <source>返回的闭包携带捕获的 upvalue，即使定义作用域已销毁。</source>
        <translation type="finished">返回的闭包携带捕获的 upvalue，即使定义作用域已销毁。</translation>
    </message>
    <message>
        <source>这实现了闭包对外层变量的透明访问。</source>
        <translation type="finished">这实现了闭包对外层变量的透明访问。</translation>
    </message>
    <message>
        <source>这展示了异常传播的栈展开机制——每帧的局部变量都被销毁。</source>
        <translation type="finished">这展示了异常传播的栈展开机制——每帧的局部变量都被销毁。</translation>
    </message>
    <message>
        <source>这展示了异常对象可以是结构化数据，而非仅字符串。</source>
        <translation type="finished">这展示了异常对象可以是结构化数据，而非仅字符串。</translation>
    </message>
    <message>
        <source>这展示了异常的转换与传播链。</source>
        <translation type="finished">这展示了异常的转换与传播链。</translation>
    </message>
    <message>
        <source>这展示了通过 IIFE 创建独立 upvalue 的模式。</source>
        <translation type="finished">这展示了通过 IIFE 创建独立 upvalue 的模式。</translation>
    </message>
    <message>
        <source>这展示了闭包共享 upvalue 的语义。</source>
        <translation type="finished">这展示了闭包共享 upvalue 的语义。</translation>
    </message>
    <message>
        <source>这展示了闭包实现私有状态的机制——每个闭包实例独立维护状态。</source>
        <translation type="finished">这展示了闭包实现私有状态的机制——每个闭包实例独立维护状态。</translation>
    </message>
    <message>
        <source>这展示了闭包的&quot;状态保持&quot;能力——变量在闭包之间共享。</source>
        <translation type="finished">这展示了闭包的&quot;状态保持&quot;能力——变量在闭包之间共享。</translation>
    </message>
    <message>
        <source>这展示了闭包的多 upvalue 机制。</source>
        <translation type="finished">这展示了闭包的多 upvalue 机制。</translation>
    </message>
    <message>
        <source>这展示了闭包逃逸——闭包超出定义作用域后仍可访问捕获的变量。</source>
        <translation type="finished">这展示了闭包逃逸——闭包超出定义作用域后仍可访问捕获的变量。</translation>
    </message>
    <message>
        <source>这展示了闭包逃逸后 upvalue 的堆化与 GC 机制。</source>
        <translation type="finished">这展示了闭包逃逸后 upvalue 的堆化与 GC 机制。</translation>
    </message>
    <message>
        <source>这是异常处理最常见的实际使用模式。</source>
        <translation type="finished">这是异常处理最常见的实际使用模式。</translation>
    </message>
    <message>
        <source>这是闭包&quot;逃逸&quot;的最常见形式——闭包超出定义作用域存活。</source>
        <translation type="finished">这是闭包&quot;逃逸&quot;的最常见形式——闭包超出定义作用域存活。</translation>
    </message>
    <message>
        <source>这是闭包实现封装的核心模式。</source>
        <translation type="finished">这是闭包实现封装的核心模式。</translation>
    </message>
    <message>
        <source>这破坏了三后端语义一致性——整数除法 7/2 应为 3，浮点除法 7.0/2 应为 3.5，</source>
        <translation type="finished">这破坏了三后端语义一致性——整数除法 7/2 应为 3，浮点除法 7.0/2 应为 3.5，</translation>
    </message>
    <message>
        <source>这解决了闭包数组中共享循环变量的问题。</source>
        <translation type="finished">这解决了闭包数组中共享循环变量的问题。</translation>
    </message>
    <message>
        <source>迭代 tracked_ 列表，对 aliveSet_ 仍在但 marked 未标记的节点（即不可达的循环孤岛）执行：</source>
        <translation type="finished">迭代 tracked_ 列表，对 aliveSet_ 仍在但 marked 未标记的节点（即不可达的循环孤岛）执行：</translation>
    </message>
    <message>
        <source>适用于 int/float 加减乘除、布尔逻辑、字符串拼接等。</source>
        <translation type="finished">适用于 int/float 加减乘除、布尔逻辑、字符串拼接等。</translation>
    </message>
    <message>
        <source>逃逸的闭包通过 upvalue 访问已堆化的变量。</source>
        <translation type="finished">逃逸的闭包通过 upvalue 访问已堆化的变量。</translation>
    </message>
    <message>
        <source>逐帧向上直到找到 catch 块或传播到顶层。</source>
        <translation type="finished">逐帧向上直到找到 catch 块或传播到顶层。</translation>
    </message>
    <message>
        <source>递归中的异常传播</source>
        <translation type="finished">递归中的异常传播</translation>
    </message>
    <message>
        <source>递归型 fib(20)。栈式 VM 与 RegisterVM 在密集函数调用场景下都显著快于 Interpreter（解释器每个 AST 节点都需要虚函数分发）。</source>
        <translation type="finished">递归型 fib(20)。栈式 VM 与 RegisterVM 在密集函数调用场景下都显著快于 Interpreter（解释器每个 AST 节点都需要虚函数分发）。</translation>
    </message>
    <message>
        <source>递归调用中抛出异常，异常沿递归链向上传播。</source>
        <translation type="finished">递归调用中抛出异常，异常沿递归链向上传播。</translation>
    </message>
    <message>
        <source>递归调用栈：每一帧持有独立的 n 值，递归返回时帧逐层弹出。栈深度等于递归深度，过深会触发 MAX_RECURSION_DEPTH=256 保护。</source>
        <translation type="finished">递归调用栈：每一帧持有独立的 n 值，递归返回时帧逐层弹出。栈深度等于递归深度，过深会触发 MAX_RECURSION_DEPTH=256 保护。</translation>
    </message>
    <message>
        <source>递归调用（fib）</source>
        <translation type="finished">递归调用（fib）</translation>
    </message>
    <message>
        <source>通过 aliveSet_ 区分存活对象与已释放的悬垂指针，避免迭代时访问已释放内存。</source>
        <translation type="finished">通过 aliveSet_ 区分存活对象与已释放的悬垂指针，避免迭代时访问已释放内存。</translation>
    </message>
    <message>
        <source>通过项目自身沉淀的真实 Bug 训练调试能力：阅读代码、预测行为、定位根因。

</source>
        <translation type="finished">通过项目自身沉淀的真实 Bug 训练调试能力：阅读代码、预测行为、定位根因。

</translation>
    </message>
    <message>
        <source>避免每次迭代都暂停导致调试效率低下。</source>
        <translation type="finished">避免每次迭代都暂停导致调试效率低下。</translation>
    </message>
    <message>
        <source>避免遗漏推/不推索引导致栈泄漏。修复：删除 L1953-1960 推索引代码对齐 visitMethodCall。</source>
        <translation type="finished">避免遗漏推/不推索引导致栈泄漏。修复：删除 L1953-1960 推索引代码对齐 visitMethodCall。</translation>
    </message>
    <message>
        <source>重命名</source>
        <translation type="finished">重命名</translation>
    </message>
    <message>
        <source>重命名失败</source>
        <translation type="finished">重命名失败</translation>
    </message>
    <message>
        <source>重新 throw 后，内层 catch 块剩余代码不执行。</source>
        <translation type="finished">重新 throw 后，内层 catch 块剩余代码不执行。</translation>
    </message>
    <message>
        <source>重新抛出后，当前 catch 块剩余代码不执行，异常沿调用栈继续传播。</source>
        <translation type="finished">重新抛出后，当前 catch 块剩余代码不执行，异常沿调用栈继续传播。</translation>
    </message>
    <message>
        <source>重新抛出（catch 中再次 throw）</source>
        <translation type="finished">重新抛出（catch 中再次 throw）</translation>
    </message>
    <message>
        <source>重新生成 IR</source>
        <translation type="finished">重新生成 IR</translation>
    </message>
    <message>
        <source>销毁阶段：当闭包不可达时（无引用指向 Closure 对象），GC 回收闭包。</source>
        <translation type="finished">销毁阶段：当闭包不可达时（无引用指向 Closure 对象），GC 回收闭包。</translation>
    </message>
    <message>
        <source>错误</source>
        <translation type="finished">错误</translation>
    </message>
    <message>
        <source>错误信息并设置 VM/Interpreter 的错误标志。</source>
        <translation type="finished">错误信息并设置 VM/Interpreter 的错误标志。</translation>
    </message>
    <message>
        <source>闭包 / upvalue 捕获</source>
        <translation type="finished">闭包 / upvalue 捕获</translation>
    </message>
    <message>
        <source>闭包 lowering：捕获的局部变量提升为 upvalue，MAKE_CLOSURE 指令携带捕获列表。</source>
        <translation type="finished">闭包 lowering：捕获的局部变量提升为 upvalue，MAKE_CLOSURE 指令携带捕获列表。</translation>
    </message>
    <message>
        <source>闭包 upvalue 捕获</source>
        <translation type="finished">闭包 upvalue 捕获</translation>
    </message>
    <message>
        <source>闭包作为函数返回值是函数式编程的核心模式。</source>
        <translation type="finished">闭包作为函数返回值是函数式编程的核心模式。</translation>
    </message>
    <message>
        <source>闭包作为返回值</source>
        <translation type="finished">闭包作为返回值</translation>
    </message>
    <message>
        <source>闭包作用域</source>
        <translation type="finished">闭包作用域</translation>
    </message>
    <message>
        <source>闭包可以捕获多个外层变量。每个捕获的变量成为独立的 upvalue，</source>
        <translation type="finished">闭包可以捕获多个外层变量。每个捕获的变量成为独立的 upvalue，</translation>
    </message>
    <message>
        <source>闭包定义时（OP_CLOSURE 指令执行），VM 读取 upvalue 数量与每个 upvalue 的位置。</source>
        <translation type="finished">闭包定义时（OP_CLOSURE 指令执行），VM 读取 upvalue 数量与每个 upvalue 的位置。</translation>
    </message>
    <message>
        <source>闭包对象在堆上包含两个 upvalue 指针，指向堆化的 base 和 delta。</source>
        <translation type="finished">闭包对象在堆上包含两个 upvalue 指针，指向堆化的 base 和 delta。</translation>
    </message>
    <message>
        <source>闭包捕获 makeCounter</source>
        <translation type="finished">闭包捕获 makeCounter</translation>
    </message>
    <message>
        <source>闭包捕获循环（10000 次）</source>
        <translation type="finished">闭包捕获循环（10000 次）</translation>
    </message>
    <message>
        <source>闭包数组</source>
        <translation type="finished">闭包数组</translation>
    </message>
    <message>
        <source>闭包检查器</source>
        <translation type="finished">闭包检查器</translation>
    </message>
    <message>
        <source>闭包的 upvalue 数组被释放，若 upvalue 引用的堆值无其他引用，</source>
        <translation type="finished">闭包的 upvalue 数组被释放，若 upvalue 引用的堆值无其他引用，</translation>
    </message>
    <message>
        <source>闭包调用栈：闭包帧不持有 count 的本地副本，而是通过 upvalue 引用外层 makeCounter 帧的 count。makeCounter 返回后帧已弹出，闭包通过 Upvalue 对象保持对 count 的引用（堆分配）。</source>
        <translation type="finished">闭包调用栈：闭包帧不持有 count 的本地副本，而是通过 upvalue 引用外层 makeCounter 帧的 count。makeCounter 返回后帧已弹出，闭包通过 Upvalue 对象保持对 count 的引用（堆分配）。</translation>
    </message>
    <message>
        <source>闭包逃逸指闭包超出定义作用域后仍被使用。</source>
        <translation type="finished">闭包逃逸指闭包超出定义作用域后仍被使用。</translation>
    </message>
    <message>
        <source>闭包逃逸（超出定义作用域）</source>
        <translation type="finished">闭包逃逸（超出定义作用域）</translation>
    </message>
    <message>
        <source>问题</source>
        <translation type="finished">问题</translation>
    </message>
    <message>
        <source>非法名称</source>
        <translation type="finished">非法名称</translation>
    </message>
    <message>
        <source>非法文件名</source>
        <translation type="finished">非法文件名</translation>
    </message>
    <message>
        <source>面板</source>
        <translation type="finished">面板</translation>
    </message>
    <message>
        <source>顶层调用 foo()，栈只有 2 帧：main + foo。</source>
        <translation type="finished">顶层调用 foo()，栈只有 2 帧：main + foo。</translation>
    </message>
    <message>
        <source>验证完成 — 对比期望/Bug 行为</source>
        <translation type="finished">验证完成 — 对比期望/Bug 行为</translation>
    </message>
    <message>
        <source>高 16 位为 INT_TAG_BASE = 0x7FF8，标记类型为 VAL_INT。</source>
        <translation type="finished">高 16 位为 INT_TAG_BASE = 0x7FF8，标记类型为 VAL_INT。</translation>
    </message>
    <message>
        <source>高 16 位为 PTR_TAG_BASE = 0x7FFB，低 48 位为 StringData 对象的虚拟地址。</source>
        <translation type="finished">高 16 位为 PTR_TAG_BASE = 0x7FFB，低 48 位为 StringData 对象的虚拟地址。</translation>
    </message>
    <message>
        <source>高 16 位是 exponent 字段。若 float 的位模式恰好落入 NaN-boxed tag 范围，</source>
        <translation type="finished">高 16 位是 exponent 字段。若 float 的位模式恰好落入 NaN-boxed tag 范围，</translation>
    </message>
    <message>
        <source>默认参数 / 三后端一致性</source>
        <translation type="finished">默认参数 / 三后端一致性</translation>
    </message>
    <message>
        <source>默认参数表达式：三后端不一致</source>
        <translation type="finished">默认参数表达式：三后端不一致</translation>
    </message>
    <message>
        <source>（MiniLang 中 var 声明的变量在同一作用域内共享），</source>
        <translation type="finished">（MiniLang 中 var 声明的变量在同一作用域内共享），</translation>
    </message>
    <message>
        <source>（a 离开作用域）</source>
        <translation type="finished">（a 离开作用域）</translation>
    </message>
    <message>
        <source>（b 离开作用域）</source>
        <translation type="finished">（b 离开作用域）</translation>
    </message>
    <message>
        <source>（q 离开作用域）</source>
        <translation type="finished">（q 离开作用域）</translation>
    </message>
    <message>
        <source>（堆指针，tag bits=0x7FFB）</source>
        <translation type="finished">（堆指针，tag bits=0x7FFB）</translation>
    </message>
    <message>
        <source>（如使用 IIFE 或块作用域）。</source>
        <translation type="finished">（如使用 IIFE 或块作用域）。</translation>
    </message>
    <message>
        <source>（如关闭文件、释放锁）。MiniLang 的 try/catch 不支持 finally 关键字，</source>
        <translation type="finished">（如关闭文件、释放锁）。MiniLang 的 try/catch 不支持 finally 关键字，</translation>
    </message>
    <message>
        <source>（尚未编译，请在主编辑器输入代码并触发编译分析）</source>
        <translation type="finished">（尚未编译，请在主编辑器输入代码并触发编译分析）</translation>
    </message>
    <message>
        <source>（尚未解析，请先在主编辑器中输入代码）</source>
        <translation type="finished">（尚未解析，请先在主编辑器中输入代码）</translation>
    </message>
    <message>
        <source>（新对象中 b[3]=4）</source>
        <translation type="finished">（新对象中 b[3]=4）</translation>
    </message>
    <message>
        <source>（无 AST）</source>
        <translation type="finished">（无 AST）</translation>
    </message>
    <message>
        <source>（无条件）</source>
        <translation type="finished">（无条件）</translation>
    </message>
    <message>
        <source>（未绑定控制器）</source>
        <translation type="finished">（未绑定控制器）</translation>
    </message>
    <message>
        <source>（条件: </source>
        <translation type="finished">（条件: </translation>
    </message>
    <message>
        <source>（空代码）</source>
        <translation type="finished">（空代码）</translation>
    </message>
    <message>
        <source>（而非直接指向栈槽），形成 upvalue 链。</source>
        <translation type="finished">（而非直接指向栈槽），形成 upvalue 链。</translation>
    </message>
    <message>
        <source>），视为条件不满足</source>
        <translation type="finished">），视为条件不满足</translation>
    </message>
    <message>
        <source>🔴 VM 命中断点: 第 %1 行</source>
        <translation type="finished">🔴 VM 命中断点: 第 %1 行</translation>
    </message>
    <message>
        <source>⑥ 字节码↔汇编对照</source>
        <translation>⑥ 字节码↔汇编对照</translation>
    </message>
    <message>
        <source>&lt;b&gt;字节码↔汇编对照&lt;/b&gt;：编辑源码后点击「编译并对照」，左栏展示栈式字节码反汇编，右栏展示 JIT 真实发射的 x86-64 汇编（含机器码字节，asmjit StringLogger 捕获）。</source>
        <translation>&lt;b&gt;字节码↔汇编对照&lt;/b&gt;：编辑源码后点击「编译并对照」，左栏展示栈式字节码反汇编，右栏展示 JIT 真实发射的 x86-64 汇编（含机器码字节，asmjit StringLogger 捕获）。</translation>
    </message>
    <message>
        <source>编译并对照</source>
        <translation>编译并对照</translation>
    </message>
    <message>
        <source>字节码反汇编：</source>
        <translation>字节码反汇编：</translation>
    </message>
    <message>
        <source>JIT 发射的 x86-64 汇编：</source>
        <translation>JIT 发射的 x86-64 汇编：</translation>
    </message>
    <message>
        <source>编译中...</source>
        <translation>编译中...</translation>
    </message>
    <message>
        <source>❌ 失败：%1</source>
        <translation>❌ 失败：%1</translation>
    </message>
    <message>
        <source>✅ 成功（输出：%1）</source>
        <translation>✅ 成功（输出：%1）</translation>
    </message>
</context>
</TS>
