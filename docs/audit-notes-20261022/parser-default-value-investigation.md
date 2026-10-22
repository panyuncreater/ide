# 参数默认值数组字面量两路径分叉——根因调查与最小修复设计

> 调查日期：2026-10-22 ｜ 只读调查（未运行构建/测试，纪律约束）
> 证据分级：✅ = 直接读码验证；🔶 = 基于 ✅ 代码语义的 token 级推演（逻辑闭环、未实际执行）；❓ = 未验证，已标注
> 关联：docs/code-audit-report-2026-10-21.md §3.2（原登记）与 §6.2（勘误后准确口径）

---

## 0. 一句话根因链

🔶 **两条路径共用同一个 `parseParamList`：`a: int[] = [1, 2]` 中 `int[]` 由 `parseTypeAnnotation` 正常消费、`[1, 2]` 由 `expression()`→`primary()` 作为 NODE_ARRAY_LITERAL 完整消费后，被 BUG-DEF-1 白名单 `isLiteralDefaultExpr`（不含数组字面量）拒绝并抛 ParseError——分叉完全发生在抛出后的错误恢复：顶层路径 `synchronize()` 把 `)` 和函数体 `{` 当普通 token 消耗、停在 `return` 同步点，使函数体语句泄漏为顶层语句（运行时「return 只能在函数体内使用」）；类成员路径同样的消耗序列停在 init 体的 `}`，`parseClassMembers` 循环把它误判为类闭合花括号，init 整体被静默丢弃（真实类 `}` 成顶层 stray brace）。**

即：不是两条解析路径实现不同，而是**同一个异常在不同宿主循环里的恢复落点不同**。

---

## 1. 代码定位（文件:函数:行号，均为 ✅ 读码验证）

### 1.1 默认值解析与白名单（两条路径的公共实现）

| 位置 | 内容 |
|------|------|
| `parser/Parser.cpp:931` `Parser::parseParamList` | 参数列表解析（4 个调用点共用） |
| `parser/Parser.cpp:942-970` | 局部 lambda `isLiteralDefaultExpr`：白名单 = NUMBER/STRING/BOOL/NULL/INTERPOLATED_STRING 字面量 + UOP_NEGATE 嵌套包裹 NUMBER；**无 NODE_ARRAY_LITERAL / NODE_DICT_LITERAL 分支**（default → false） |
| `parser/Parser.cpp:1035-1044` | `match(TK_ASSIGN)` → `defaultExpr = expression()` → 白名单不过则 `throw ParseError("默认参数值必须是字面量（数字/字符串/布尔/null/负数字面量）", ...)`（**抛出点在表达式已全部消费之后**，游标指向参数收尾 `)`） |
| `parser/Parser.cpp:1053-1066` | 前向引用检查 `collectVarRefs`（已递归数组/字典字面量，Parser.cpp:54-63，放行后无需改） |

四个调用点（✅）：
1. `Parser::funDecl`（Parser.cpp:726，调 parseParamList 于 :760）——顶层/嵌套 `fun`；
2. `Parser::typedFunDecl`（:839，:848）——带返回类型声明；
3. `Parser::lambdaExpr`（:881，:890）——lambda；
4. `Parser::parseClassMembers` 裸方法分支（:1230 函数体，:1287 消费方法名+`(` 后 :1293 调 parseParamList）——**类 init 无专用语法，`init` 不是关键字（lexer/Lexer.cpp 全文无 "init"，✅），走裸方法分支**。

### 1.2 `[` 的两组消费者（互不干扰，✅）

- 类型注解 `int[]` 的 `[]`：`Parser::parseTypeAnnotation`（:336，`[]` 后缀循环 :386-404，带安全回溯）。
- 默认值 `[1, 2]` 的 `[`：`expression()` → `Parser::primary()`（:2717-2721 `match(TK_LBRACKET)` → 数组字面量解析，产生 NODE_ARRAY_LITERAL）。
- 参数名/类型分支选择：`isTypeKeyword()`（:289）/`isClassTypeDeclStart()`（:296，`a` 后跟 `:` 不误判）/`isFunTypeDeclStart()`（:328）——`a: int[] = ...` 正确落入「名字在前」分支（✅）。

### 1.3 错误恢复与宿主循环（分叉的直接原因，✅）

| 位置 | 内容 |
|------|------|
| `Parser::synchronize`（:2799） | 同步点白名单：初始检查 `}`/EOF 直接 return、`;` 消耗后 return；主循环 `return`/`}` 与关键字白名单（var/fun/class/if/while/for/**return**/print/…）处 return；**其余 token（包括 `)` 和 `{`）逐个 advance 消耗**（:2905-2922 尾部确认 `default: break; → advance();`）。`{`、`)` 均非同步点 |
| `Parser::parse()`（:133）顶层循环 | catch ParseError → `diagnostics_.addError(...)`（:171，**诊断确实被记录**）→ synchronize（:175）；循环头对顶层 stray `}` 有专门处理：加诊断「多余的 '}' 在顶层（无匹配的 '{'）」并消耗（:159-168） |
| `Parser::parseClassMembers`（:1230） | `while (!check(TK_RBRACE) && !isAtEnd())` + try/catch（addError + synchronize）；循环退出后由 `classDecl` 的 `consume(TK_RBRACE)` 消费**它以为是类闭合的 `}`**（函数内注释自证该协作契约） |

### 1.4 默认值的存储与求值时机（✅）

- **存储**（parse 期）：AST 节点存入 `FunDecl::defaultValues`（与 params 一一对应，无默认值处 nullptr），同时算 `requiredParamCount`（前缀无默认值个数）：funDecl :825-835、typedFunDecl :858-868、lambdaExpr :917-927、类裸方法 :1308-1317。
- **求值**（调用期，四后端三种机制）：
  - Interpreter：调用点缺参时逐个 `evaluate(defaultValues[i])`（interpreter/InterpreterCalls.cpp:207、450、814、1024；Interpreter.cpp:1899 全默认参 init、3731/3832 方法）——**每次调用重新求值，数组默认值会产生新数组对象**；
  - StackVM/RegisterVM：编译期把默认值提取为常量入池——`Compiler::emitDefaultValues`（compiler/core/CompilerStmt.cpp:678，仅标量字面量 + 负数字面量，否则记 `RuntimeLimits::NO_INDEX` 哨兵）/ IR 侧 `extractConstant`（compiler/ir/AstIRBuilder.cpp:43，同样仅标量）→ `ir_->defaultConstIndices`（:2038-2051）；运行期 `VM::fillDefaultArgs`（compiler/backend-stack/VM.cpp:267-292，NO_INDEX → 返回 false → 调用方报 arity 错，如 VMCalls.cpp:451/1312「构造函数 init 期望 {r}-{a} 个参数，但传入了 {n} 个」）、RegisterVM 对应消费点 RegisterVMCalls.cpp:839/1330/1677/1814；
  - TCO 帧复用路径例外：编译期内联 `compileNode(defaultValues[i])`（CompilerStmt.cpp:963-970）——数组字面量在该路径本可编译（`visitArrayLiteral` → OP_BUILD_ARRAY，CompilerExpr.cpp:560-573）。
- **直调不编译期填充**：`visitFunCall` 对缺参调用直接发射 `OP_CALL name, argCount`（CompilerStmt.cpp:870-878，不补默认值）——缺参填充完全依赖 VM 运行时常量表。

### 1.5 运行器诊断口径差（勘误 §6.2-④ 破案，✅）

- `Parser::parse()` 恒返回 `Block`（内部 catch 所有 ParseError，:169-176），**永不返回 nullptr**。
- 测试侧辅助函数（common/ThreeBackends.h:42-150 四后端 runX，及大量测试文件内联复制品）只判 `if (!ast) return "<parse-fail>"`，**不检查 `p.hasErrors()`** → 诊断被静默，脱轨 AST 照常编译执行。
- CLI 侧（cli/compile_core.cpp:163、214；coverage_core.cpp:457；dap_core.cpp:210 等）检查 `hasErrors()` 并输出 `getDiagnostics().summary()`。
- **结论**：「当前代码会 ParseError」与「测试二进制无 ParseError」两个观察都真实——差异在运行器是否读取 DiagnosticBag，解析器行为完全一致。原勘误「原因未定位」至此定位。

### 1.6 运行时错误文本出处（✅）

- interpreter/Interpreter.cpp:240：顶层 `run()` catch `ReturnException` → `runtimeError("return 只能在函数体内使用", 0, 0)`。与实测症状①文本逐字吻合。

---

## 2. 两条路径的逐 token 推演（🔶，基于 §1 已验证语义）

### 2.1 顶层 `fun f(a: int[] = [1, 2]) { return 1; } print(f());`

token 流：`fun f ( a : int [ ] = [ 1 , 2 ] ) { return 1 ; } print ( f ( ) ) ;`

1. `funDecl`（:726）→ `parseParamList`：`a` 走「名字在前」分支，`parseTypeAnnotation` 消费 `int` `[]`；`match(TK_ASSIGN)` 成功；`expression()` 完整消费 `[1, 2]` 得 NODE_ARRAY_LITERAL（游标已指向 `)`）。
2. :1040 白名单拒绝 → **throw ParseError**（诊断将记录，但见 §2.1-4）。
3. 异常穿透 funDecl → `parse()` 顶层循环 catch（:171 addError）→ `synchronize()`（:2799）：
   - peek `)`：非同步点 → advance（消耗 `)`）；
   - peek `{`（函数体）：非同步点 → advance（**消耗函数体开括号**）；
   - peek `return`：TK_RETURN 在同步白名单 → **return，不消耗**。
4. 顶层循环继续 → `declaration()` 把 `return 1;` 解析为**顶层 ReturnStmt**（函数体语句泄漏到顶层）；随后 stray `}` 触发第二条诊断「多余的 '}' 在顶层」并消耗；`print(f());` 正常解析。
5. 执行：顶层 ReturnStmt → Interpreter.cpp:240 → **「return 只能在函数体内使用」**，程序终止（`f` 根本未定义，print 未及执行）。

→ 与实测症状①（解析脱轨、函数体错误归属、无可见 ParseError、运行时报错）逐项吻合。诊断实际**已记录**（一条「默认参数值必须是字面量」+ 一条「多余的 '}'」），只是测试类运行器不显示（§1.5）。

### 2.2 类 `class C { init(a: int[] = [1, 2]) {} }`（后接 `var c = C(); print(c.x);`）

token 流：`class C { init ( a : int [ ] = [ 1 , 2 ] ) { } } var c = C ( ) ; print ( c . x ) ;`

1. `parseClassMembers`（:1230）：`init` 是 TK_IDENTIFIER → 裸方法分支 → `parseParamList` → 与 §2.1 相同地 **throw**（游标指向 `)`）。
2. 成员循环 catch（addError）→ `synchronize()`：
   - 消耗 `)`（第一个 advance）；peek `{`（init 体开括号）非同步点 → advance；
   - peek `}`（**init 体的闭合**）→ P1-2 规则「`}` 不消耗直接 return」。
3. 成员循环条件 `!check(TK_RBRACE)` 看到 init 体的 `}` → **误判类体结束，提前退出**。
4. `classDecl` 的 `consume(TK_RBRACE)` 消费掉 **init 的 `}`**（当作类闭合）→ **ClassDecl C 成员列表为空，init 静默丢失**，无任何成员被记录。
5. 真实的类闭合 `}` 泄漏到顶层 → 顶层循环 stray-brace 分支：第二条诊断 + 消耗；后续 `var c = C();` 正常解析。
6. 执行：`C()` 时 initMethod 为 null（成员里没有 init）→ 构造流程跳过 init（InterpreterCalls.cpp:659 的 `initMethod &&` 短路）→ `this.x` 从未赋值 → `c.x` 为 **null**。

→ 与实测症状②（「解析通过」= 未中断执行、默认值静默不绑定、字段 null）吻合；且解释了为何该症状**完全无脱轨迹象**——类路径的恢复落点恰好消费齐了花括号配对，只是配对错了对象。

### 2.3 标量默认值全路径正常的原因（✅）

`a: int = 1` → `expression()` 得 NODE_NUMBER_LITERAL → 白名单通过 → `defaultValues` 正常填充 → 四后端既有机制（§1.4）工作。症状③与实现一致。

---

## 3. 根因链完整表述（任务问题逐项回答）

1. **`[` 在参数列表上下文被谁消费**：两组 `[]` 消费者不同。`int[]` 的 `[]` 由 `parseTypeAnnotation` 的后缀循环消费（:386-404）；`= [1, 2]` 的 `[` 由 `expression()`→`primary()`（:2717）作为数组字面量**连同元素和 `]` 一起完整消费**。消费本身无缺陷。
2. **默认值表达式何时求值/存储**：parse 期只存 AST（`FunDecl::defaultValues` + `requiredParamCount`，§1.4）；求值在调用期——Interpreter 逐调用 `evaluate`（每次新对象）；Stack/RegisterVM 经编译期常量表 + 运行期 `fillDefaultArgs`（共享常量对象，靠 COW 保证写安全）；TCO 帧复用为编译期内联。**数组字面量在 Interpreter/TCO 通路本可求值/编译，卡点只在 Parser 白名单与两 VM 的常量提取器不含 NODE_ARRAY_LITERAL。**
3. **类路径为何静默丢弃**：同一个 ParseError 抛出后，`synchronize()` 消耗 `)` 与 init 体 `{`、停在 init 体 `}`（不消耗）；`parseClassMembers` 的循环条件把该 `}` 当类尾 → 循环提前退出 → `consume(TK_RBRACE)` 消费错误的 `}` → init 不入成员表。「静默」= 诊断已记入 DiagnosticBag 但测试运行器不显示（§1.5），执行照常进行，字段保持 null。
4. **白名单机制与「解析脱轨」的关系（任务要求核实）**：**白名单拒绝本身不是脱轨原因**——对真正非法的程序抛 ParseError 是 BUG-DEF-1 的设计内行为；脱轨是 `synchronize()` 缺乏「参数列表/函数体边界」上下文（`{`、`)` 不是同步点）造成的**恢复位置错误**。二者是「拒绝策略」与「恢复策略」两个正交缺陷的组合：数组字面量被白名单拒绝（缺陷一：策略过窄），拒绝后的恢复吞掉函数体边界（缺陷二：恢复脱轨），类成员循环再放大为静默丢成员（缺陷二的类路径表现）。

---

## 4. 最小修复设计

### 改动点 1（核心）：白名单接受常量数组字面量 —— parser/Parser.cpp

- 位置：`Parser::parseParamList` 内 lambda `isLiteralDefaultExpr`（:942-970）。
- 改法：把 lambda 提为可递归辅助函数（文件内 static 自由函数或 Parser 静态成员），新增分支：

```cpp
case NodeType::NODE_ARRAY_LITERAL: {
    const auto* arr = static_cast<const ArrayLiteral*>(node);
    // 上限交给编译器 visitArrayLiteral 的既有 255 检查报更准确的错；
    // 此处只做「纯字面量」判定
    for (const auto& e : arr->elements)
        if (!isLiteralDefaultExpr(e.get())) return false;
    return true;
}
// 可选同型（建议同轮）：NODE_DICT_LITERAL 键/值递归，封闭同族问题
```

- 为何最小：四个调用点（funDecl/typedFunDecl/lambdaExpr/类裸方法）共用 parseParamList，**单点修复同时治愈两条路径**；不改文法、不改 AST、不改 token 流、既有 case 零改动。

### 改动点 2：StackVM 常量表支持数组常量 —— compiler/core/CompilerStmt.cpp

- 位置：`Compiler::emitDefaultValues`（:678-716）。
- 改法：新增 NODE_ARRAY_LITERAL 分支——递归为元素构建 `Value`（标量/嵌套数组），构造常量数组 Value 写入 chunk 常量池并记录索引；提取失败仍 NO_INDEX（保留现有哨兵协议）。实现期需确认 Value 的数组构造入口（NaN-boxing + COW ArrayData，ADR-001/004）。

### 改动点 3：IR 常量提取支持数组 —— compiler/ir/AstIRBuilder.cpp

- 位置：`extractConstant`（:43-70）。
- 改法：新增 NODE_ARRAY_LITERAL 递归提取（嵌套数组→嵌套 Value 数组）。`:2038` 的 `defaultConstIndices` 填充与 RegisterVM 全部消费点（RegisterVMCalls.cpp:839/1330/1677/1814）**无需改动**，自动生效。

### 改动点 4（可选加固，建议独立提交）：错误恢复结构化 —— parser/Parser.cpp

- 位置：`Parser::synchronize`（:2799）或 `parseParamList` 的调用方。
- 保守方案（推荐）：`parseParamList` 内白名单拒绝时**不抛异常**——`diagnostics_.addError` 记录后，跳过默认值 token 至深度 0 的 `,`/`)`，该参数按「无默认值」继续（后续调用得到干净的 arity 错误）。影响面仅参数列表路径。
- 激进方案（不推荐本轮）：synchronize 增加 `TK_LBRACE`/`TK_RPAREN` 为结构边界——影响全部错误恢复场景，需全量回归，且 ① 中 `f` 仍不定义、`{ return 1; }` 仍被当顶层块语句，收益有限。

### 为何最小 & 对既有合法程序零影响

- **结构性零影响论证**：现状下任何含数组（/字典）字面量默认值的程序都必然被 ：1040 抛 ParseError 判非法——即「含数组默认值的合法程序」集合为空；放行后该集合从空变为非空，**不改变任何一个现行合法程序的行为**。标量默认值路径（白名单既有 case、emitDefaultValues/extractConstant 既有分支、fillDefaultArgs、requiredParamCount 逻辑）一行未动。
- 四后端闭包：Interpreter 逐调用求值天然支持；Stack/RegisterVM 补常量提取后走既有 fillDefaultArgs 协议；JIT 复用 chunk 常量/未支持场景降级（需专项回归，见风险 2）。

### 风险清单

1. **共享 vs 每调用新数组**：Interpreter 每调用 evaluate 产生新数组；VM 共享常量池数组。COW（ADR-004）保证首次写分离 → 可观察语义一致（T5 锁定）。实现时须确认常量池 Value 引用计数入池基数，防「常量数组被就地写穿」——若发现 COW 判定不含常量池引用，需在入池时构建不可变标记或复制。
2. **JIT**（AGENTS §3 #11 教训）：默认值填充在 VM `fillDefaultArgs`（OP_CALL/OP_CALL_EXPR 侧），JIT per-chunk 跨块调用经 import slot 回 VM 入口，理论自动生效；但 JIT 不计入 `expectAllGenericBackends`，须 runJIT 专项回归。现有 chunk 的 codegen 序列不变 → `kJitCodegenVersion` **不递增**（新程序 = 新字节码指纹，缓存键自然区分）；若实现改为 JIT 内联填充则必须递增。
3. **元素数 >255**：白名单不拦截（否则报「必须是字面量」误导），交给 `visitArrayLiteral` 既有上限检查报准确错误。
4. **改动点 4** 若选激进方案，需全量 ctest 回归错误恢复用例。
5. 字典字面量默认值为同族问题，本轮范围外（建议同轮处理或单独登记）。

### 替代方案

- **方案 B（语言维持字面量限制）**：只做改动点 4 保守变体。数组默认值仍非法，但从「脱轨/静默丢成员」变为「1 条清晰 ParseError + 调用时 arity 错」。改动最小（1 文件），不满足「让探针程序按预期工作」的修复预期——仅消除伤害。适合作为改动点 1-3 受阻时的降级交付。
- **方案 C（调用点编译期填充）**：把缺参填充从「VM 运行时常量表」改为「编译器在 visitFunCall 内联编译默认值」，可绕开常量提取器改动；但变更四后端调用协议与 arity 语义（chunk.requiredArity 语义、闭包/方法/TCO 多路径），影响面远大于改动点 2/3，且改变既有 chunk 布局——不推荐。

---

## 5. 回归测试清单（四后端 = Interpreter / StackVM / RegisterVM / JIT）

> 放置建议：tests/ 新增 TestParserDefaultValues.cpp 或并入既有参数默认值用例组；每例四后端断言（runInterpreter/runStackVM/runStackVM_IR/runRegVM/runJIT，JIT 专项）。

### T1 顶层 fun 数组默认值（修复目标 ①）
```mini
fun f(a: int[] = [1, 2]) { return a; }
print(f());
print(f([9]));
```
- 修复前（现状）：四后端一致 → 运行时错误「return 只能在函数体内使用」（脱轨；❓VM 路径对顶层 return 的具体报错形态未逐路径实测，根因相同）。
- **修复后四后期望：`[1, 2]` 换行 `[9]`。**

### T2 类 init 数组默认值（修复目标 ②）
```mini
class C {
    var x: int[];
    init(a: int[] = [1, 2]) { this.x = a; }
}
var c = C();
print(c.x);
```
- 修复前：四后端一致 → `null`（init 被静默丢弃）。
- **修复后四后期望：`[1, 2]`。**（VM 侧走 VMCalls.cpp:451/1312 init 填充路径）

### T3 lambda 数组默认值
```mini
var g = fun(a: int[] = [7]) { return a; };
print(g());
```
- 修复前：同一 ParseError 家族，顶层脱轨（`return a;` 泄漏为顶层语句）→ 运行时「return 只能在函数体内使用」。
- **修复后四后期望：`[7]`。**

### T4 混合标量/嵌套数组/多参（标量回归保护 + 白名单递归）
```mini
fun h(a: int = 3, b: int[] = [1, [2, 3]], c: string = "x") { return [a, b, c]; }
print(h());
```
- **修复后四后期望：`[3, [1, [2, 3]], x]`。**（纯标量默认值行为必须与修复前逐字节一致）

### T5 默认数组写隔离 / COW 一致性（跨后端语义关键）
```mini
fun m(a: int[] = [1, 2]) { return a; }
var x = m();
x.push(99);
print(x);
print(m());
```
- **修复后四后期望：`[1, 2, 99]` 换行 `[1, 2]`。**（锁死「VM 共享常量数组被写穿则第二行变 [1,2,99]」的风险；Interpreter 每调用新数组天然通过）

### T6 非法默认值仍拒绝且不脱轨（BUG-DEF-1 × synchronize 交互回归）
```mini
fun k(a: int[] = b) { return 1; }
print("after");
```
- 现状：ParseError（默认参数值必须是字面量）+ 脱轨（`{ return 1; }` 泄漏顶层 + stray `}` 双重诊断）。
- 仅做改动点 1-3 时：仍 1 条 ParseError + 脱轨（T6 程序本就非法，允许保留现状，登记即可）。
- **做改动点 4 保守方案后：恰好 1 条「默认参数值必须是字面量」诊断，k 以无默认值形态声明，`print("after")` 正常执行输出 `after`（随后调用 `k()` 报 arity 错——另断言）。**

---

## 6. §3.2 遗留小项核实：白名单拒绝后 synchronize 残留 `}` 双重诊断噪音

- **行为（✅ 推演闭环）**：白名单拒绝抛 ParseError → `synchronize()`（Parser.cpp:2799）消耗 `)` 与函数/方法体 `{`、停在体 `}` → 外层宿主循环（顶层 `parse()` :159-168 或 `parseClassMembers` :1230）把该 `}` 当自己的结构闭合处理：顶层 = stray-brace 诊断 + 消耗（真实闭合 `}` 随后成为第二条 stray 诊断或使后续成员泄漏顶层）；类成员 = 提前退出 + `consume(TK_RBRACE)` 消费错误 `}`（成员静默丢失）。**「双重诊断」= 「默认参数值必须是字面量」+「多余的 '}' 在顶层」**。
- **切入点**：`Parser::synchronize()`（Parser.cpp:2799）的同步点集合（不含 `TK_LBRACE`/`TK_RPAREN`）与 `parseParamList` 抛出点游标位置（Parser.cpp:1040，抛出时游标已停在 `)`——修复具备天然落点）。修法方向（不需本轮设计）：参数列表上下文的结构化恢复（parseParamList 内降级或调用方归位到 `)`），或 synchronize 增补结构边界 token（后者需全量回归验证不破坏既有恢复用例）。

---

## 7. 未验证项与边界说明（❓）

1. 修复前 VM/JIT 路径对脱轨 AST（顶层 return）的具体报错形态未实测（构建/测试被纪律禁止）；根因与四后端 AST 输入一致性不受影响。
2. `Value` 数组常量的构造 API 细节（NaN-boxing/COW 入池引用计数基数）需实现期确认（改动点 2 风险 1）。
3. JIT 对「缺参调用 + fillDefaultArgs」路径的实际行为（import slot 回 VM 填充）为架构推断，须 runJIT 专项回归实证。
4. 字典字面量默认值（NODE_DICT_LITERAL）同族缺陷未单独推演，判定与数组同构（白名单与两个常量提取器均不含该 case）。
