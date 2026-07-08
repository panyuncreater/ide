# 2026-07-07 · 第十八轮审计批次 8：Parser 错误恢复 + 三后端类型注解一致性

> 本文档从 CHANGELOG.md 归档（CHANGELOG 仅保留最近 3 个版本）。

### 概述

承接第十七轮，本轮系统性审计 Parser 错误恢复（panic mode）与三后端类型注解一致性，修复 2 P1 + 3 P2 共 5 项 Bug。核心是 BUG-PARSER-SYNC-1/2/3/4 一组修复（synchronize 吞掉同步关键字 + block 块边界遗漏）和 BUG-TYPE-1 三后端函数返回类型注解检查对齐。新增 10 个回归测试，全量 1720/1721 测试通过（1 perf 占位符 Not Run）。

### 修改内容

1. **parser/Parser.cpp — synchronize() 吞掉同步关键字修复（BUG-PARSER-SYNC-1，P1）**
   - 原实现第 1539 行 `advance()` 无条件执行，当错误恰好发生在同步关键字位置（如前一条语句缺分号，下一个 token 是 `var`/`fun`/`class`/...）时，该关键字被当作"错误 token"吞掉，导致下一条声明整体从 AST 中丢失（用户只看到"期望 ';'"，但下一条声明消失）
   - 修复：在 `advance()` 前添加同步关键字预检查 switch，若 peek() 是同步关键字（var/fun/class/if/while/for/return/print/break/continue/else/try/catch/finally/throw/import/export/int/float/bool/string/dict/array）直接 return，不消耗

2. **parser/Parser.cpp — block() 块边界遗漏修复（BUG-PARSER-SYNC-2/4，P1/P2）**
   - 原实现 `block()` 仅识别 `TK_CATCH` 作为块边界，遗漏 `TK_FINALLY`（try-finally 结构）和 `TK_ELSE`（if-else 结构）
   - 修复：添加 `if (check(TK_FINALLY)) break;` 和 `if (check(TK_ELSE)) break;`
   - 同时修复 block() 结尾的 `consume(TK_RBRACE)` 逻辑：当因 catch/finally/else 中断时，peek() 不是 `}`，原 consume 会抛错导致整个语句丢失。改为三分支处理：`}` 正常消耗 / catch/finally/else/EOF 跳过让调用方处理 / 其他记录诊断但不抛错
   - **必须与 SYNC-1 一组修复**：仅修复 SYNC-1 而不修复 SYNC-2/SYNC-4 会导致 block() 在 finally/else 处无限循环（synchronize 不再吞关键字但 block 不中断 → declaration 不识别关键字 → 抛错 → synchronize 又回到关键字 → 死循环）

3. **parser/Parser.cpp — synchronize() switch 补充 TK_FINALLY（BUG-PARSER-SYNC-3，P2）**
   - 原实现 while 循环内 switch 已含 `TK_CATCH` 但缺 `TK_FINALLY`
   - 修复：在两个 switch（advance 前预检查 + while 循环内同步点）均补充 `case TK_FINALLY`

4. **parser/Parser.cpp — consume() 错误消息含实际 token（BUG-PARSER-MSG-1，P2）**
   - 原实现 `consume()` 仅输出调用方传入的 message（如"期望 ';'"），用户不知道下一个 token 是什么
   - 修复：拼接实际 token lexeme，改为"期望 ';' 但得到 'var'"，与 primary() 的"意外的 Token: 'xxx'"风格一致
   - `consumeIdentifierOrType()` 同步修复

5. **三后端函数返回类型注解检查对齐（BUG-TYPE-1，P1）**
   - **compiler/Compiler.h**：CompileContext 结构体添加 `currentFunctionReturnType` 字段；Compiler 类添加 `currentFunctionReturnType_` 成员
   - **compiler/Compiler.cpp**：saveCompileContext/restoreCompileContext 处理新字段；visitFunDecl 设置 `currentFunctionReturnType_ = node.returnType`；visitReturnStmt 在 OP_RETURN 前发射 `OP_TYPE_CHECK`（若有返回类型注解）
   - **compiler/IR.h**：AstIRBuilder 添加 `currentFunctionReturnType_` 成员
   - **compiler/IR.cpp**：visitFunDecl 保存/设置/恢复 currentFunctionReturnType_（异常路径 + 正常路径两处恢复）；visitReturnStmt 在 RETURN 前发射 `TYPE_CHECK` IR
   - 原实现仅 Interpreter 在 visitReturnStmt 中检查返回类型注解（通过 `currentFunctionReturnType_` + `CallFrameGuard`），StackVM/RegisterVM 静默通过，导致 `fun foo(): int { return "str"; }` 仅 Interpreter 报错

### 关键决策

1. **SYNC-1/2/3/4 必须一组修复**：synchronize() 不再吞同步关键字后，block() 必须识别 finally/else 作为块边界中断，否则会无限循环（declaration 不识别关键字 → 抛错 → synchronize 回到关键字 → 死循环）。同时 block() 结尾的 consume(TK_RBRACE) 必须改为三分支处理，否则因关键字中断时 consume 抛错导致整个语句丢失
2. **block() 结尾三分支处理**：`}` 正常消耗 / catch/finally/else/EOF 跳过让调用方处理 / 其他记录诊断但不抛错。原 consume(TK_RBRACE) 在缺 `}` 时抛错会传播到 tryStmt/ifStmt 导致整个语句丢失，改为记录诊断后继续，保留已解析的语句
3. **类型注解 Bug #2（参数类型检查）暂不实施**：需维护函数名→paramTypes 注册表，且需处理闭包、方法调用、模块导入、前向引用等复杂场景，改动大风险高。Bug #1 已覆盖返回类型检查核心场景，参数类型注解在教学场景主要作文档用途，差分测试已能捕获回归。记录为已知 gap
4. **类型注解错误消息不一致（Bug #3）暂不修**：Interpreter 输出含上下文（"返回值 期望类型 int"），StackVM/RegisterVM 输出固定前缀（"类型注解违反: 期望类型 int"）。需扩展 OP_TYPE_CHECK 指令编码增加上下文字符串常量索引，改动中等但收益低（仅消息文本差异）。测试用 `find("<runtime:")` 验证"都报错"而非比较消息文本

### 未完成部分（已知 gap）

- **BUG-TYPE-2（P1）函数参数类型注解检查**：Interpreter 在 callNamedFunction/闭包调用/方法调用 3 处检查参数类型，StackVM/RegisterVM 不检查。需维护函数名→paramTypes 注册表 + 处理闭包/方法/模块/前向引用场景。暂不实施
- **BUG-TYPE-3（P2）类型注解错误消息三后端不一致**：Interpreter 含上下文，StackVM/RegisterVM 固定前缀。需扩展指令编码。暂不实施

### 修改文件清单

- 修改：`parser/Parser.cpp`（synchronize advance 前预检查 + block 添加 FINALLY/ELSE 中断 + block 结尾三分支处理 + consume/consumeIdentifierOrType 消息拼接 + while 循环 switch 补充 FINALLY）
- 修改：`compiler/Compiler.h`（CompileContext + currentFunctionReturnType_ 成员）
- 修改：`compiler/Compiler.cpp`（save/restore 新字段 + visitFunDecl 设置 + visitReturnStmt 发射 OP_TYPE_CHECK）
- 修改：`compiler/IR.h`（currentFunctionReturnType_ 成员）
- 修改：`compiler/IR.cpp`（visitFunDecl 保存/设置/恢复 + visitReturnStmt 发射 TYPE_CHECK IR）
- 修改：`tests/TestParser.cpp`（+6 测试：SYNC-1×3 + SYNC-2 + SYNC-4 + MSG-1）
- 修改：`tests/TestThreeEnginesConsistency.cpp`（+4 测试：TYPE-1 返回类型注解三后端一致）
- 文档同步：`README.md`（测试徽章 1710→1720）+ `CHANGELOG.md`（本章节）+ `docs/development.md`（2026-07-07 摘要表头插入新行）+ `project_memory.md`（追加本轮关键决策与教训）
