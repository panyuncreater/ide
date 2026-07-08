# 2026-07-07 · 第十七轮审计批次 7：寄存器分配 P0 修复 + 模块状态泄漏 + Formatter 往返等价性

> 本文档从 CHANGELOG.md 归档（CHANGELOG 仅保留最近 3 个版本）。

### 概述

承接第十六轮架构优化，本轮系统性排查「寄存器溢出 + 模块系统状态隔离 + Formatter 往返等价性 + 条件断点求值」四大方向，共发现并修复 1 个 P0 + 1 个 P1 + 4 个 P2 共 6 项潜在 Bug。核心修复：(1) **P0-REGALLOC 寄存器溢出**——`RegisterBytecodeBackend::vregToReg()` 使用 `vreg N → reg localCount+N` 直接映射，vreg 单调递增不复用，稍长程序（72 条指令的 while+for 控制流）即溢出 32 寄存器上限导致编译期失败，5 个 ThreeEnginesFuzz 测试 + 2 个回归测试失败；改为基于"全局最后使用点"的线性扫描寄存器分配（collectVRegLastUse + releaseDeadVRegs + freeRegs_ 复用），并正确处理 MEMBER_SET/INDEX_SET 的 obj vreg 隐式使用（通过 lastMutatedReceiverReg_ 延伸到下一个 WRITEBACK_* 指令）。(2) **P1-D Formatter 单语句体强制花括号**——`formatBlock` 对单语句体强制包裹 `{}`，与源码"裸语句体"不等价导致往返失真。(3) **P2-A Interpreter moduleMtimes_ 跨会话泄漏**——模块修改时间缓存跨会话未清理，导致同会话内重新加载模块时跳过重新编译。(4) **P2-B Compiler 模块状态清理不完整**——`compileViaIR`/`compileViaRegisterIR` 失败路径未清理 `moduleLoadingStack_` 和 `moduleExports_`。(5) **P2-E 条件断点求值器未设置 boundInstance_**——条件断点求值时 Environment 未设置 `boundInstance_`，类方法内 `this` 求值失败。新增 1 个回归测试（33 元素数组真正触发溢出），更新 1 个测试预期（寄存器复用后原程序不再溢出），全量 1710/1711 测试通过（1 个 perf 占位符 Not Run）。

### 修改内容

1. **compiler/RegisterBytecodeBackend.h + .cpp — 线性扫描寄存器分配（P0-REGALLOC）**
   - 原实现：`vregToReg(vreg)` 直接映射 `vreg N → reg localCount+N`，vreg 单调递增不复用。IR 的 `allocVReg()` 也是单调递增（SSA-like），每个 vreg 只定义一次，但原实现未利用"vreg 生命周期从首次定义到最后使用"的特性，导致稍长程序（72 条指令）即溢出 32 寄存器上限
   - 修复：实现基于"全局最后使用点"的线性扫描寄存器分配
     - 新增成员：`vregToReg_`（活跃 vreg → 物理寄存器）/ `vregLastUse_`（vreg → 全局最后使用 instrIndex）/ `lastUseToVregs_`（instrIndex → 在此处最后使用的 vreg 列表）/ `freeRegs_`（空闲物理寄存器栈）
     - `collectVRegLastUse(ir)`：预扫描所有指令构建 vreg→最后使用映射。**关键处理 MEMBER_SET/INDEX_SET 隐式使用**——这两条指令的 obj vreg（操作数[0]）会被记录到 `lastMutatedReceiverReg_`，后续 WRITEBACK_* 指令通过 `reg(lastMutatedReceiverReg_)` 读取变异后容器。若按显式操作数释放 obj vreg，会在 WRITEBACK_* 之前被过早释放，寄存器被复用覆盖，导致运行时"<runtime:该类型不支持成员访问>"
     - `releaseDeadVRegs(instrIndex)`：每条指令 lower 完成后，释放最后使用点 == instrIndex 的 vreg 寄存器到 freeRegs_
     - `vregToReg(vreg)` 改为优先复用 freeRegs_ 中的空闲寄存器，无空闲时才分配新寄存器
   - 复现：`var i=0; var s=0; while(i<10){ s=s+i; i=i+1; } for(var j=0;j<10;j=j+1){ s=s+j; } print(s);` 在 RegisterVM 编译失败（`<compile:[编译器] 错误: Register IR lowering 失败>`）

2. **formatter/Formatter.cpp — 单语句体保留原样（P1-D）**
   - 原实现：`formatBlock` 对单语句体（如 `if (x) doSomething();`）强制包裹 `{}`，改为 `if (x) { doSomething(); }`，与源码不等价导致 formatter 往返失真
   - 修复：保留单语句体的原样格式（不强制包裹花括号），仅在用户已写花括号时保留
   - 设计原则：Formatter 的不变量是"format(parse(src)) == format(parse(format(parse(src))))"，即二次格式化幂等。强制包裹花括号破坏了首次格式化的等价性

3. **interpreter/Interpreter.cpp — moduleMtimes_ 跨会话清理（P2-A）**
   - 原实现：`moduleMtimes_` 缓存模块文件修改时间，跨会话未清理。同一 Interpreter 实例多次执行（如 REPL 中多次运行同一程序）时，若模块文件未修改则跳过重新编译，但前次执行的模块导出可能已被 GC/作用域销毁，导致后续执行找不到导出
   - 修复：在 `execute()` 入口清理 `moduleMtimes_`（仅保留当前会话的修改时间记录），确保每次执行重新加载模块

4. **compiler/Compiler.cpp — compileViaIR/compileViaRegisterIR 状态清理（P2-B）**
   - 原实现：`compileViaIR` 和 `compileViaRegisterIR` 失败路径（lowering 失败）直接 return 空 result，未清理 `moduleLoadingStack_` 和 `moduleExports_`。后续编译受残留状态污染，可能误判循环依赖或导出缺失
   - 修复：在 return 前调用 `moduleLoadingStack_.clear()` 和 `moduleExports_.clear()`

5. **app/IdeController.cpp — 条件断点求值器设置 boundInstance_（P2-E）**
   - 原实现：条件断点求值时创建新 Environment 但未设置 `boundInstance_`，类方法内 `this` 求值失败（`<runtime:未定义的标识符 'this'>`）
   - 修复：求值前从调试上下文获取当前类实例，设置 `boundInstance_`

6. **tests/TestThreeEnginesConsistency.cpp — 测试预期更新 + 新增溢出测试**
   - `AuditRegFrame_Exceeds32RegistersSafetyCheck`：原程序（20 局部变量 + 左结合加法）寄存器复用后不再溢出，改为三后端一致返回 "210"
   - 新增 `AuditRegFrame_RealOverflowWith33ElementArray`：33 元素数组字面量 `[1,2,...,33]` 真正触发 32 寄存器溢出，RegVM 须编译期报错

### 关键设计决策

- **线性扫描而非图着色**：MiniLang IR 的 vreg 生命周期简单（从首次定义到最后使用，无复杂控制流交叉），线性扫描（linear scan）足够且实现简单。图着色分配器实现复杂度高，收益不匹配
- **MEMBER_SET/INDEX_SET 隐式使用延长到 WRITEBACK_*"：`lastMutatedReceiverReg_` 机制是 RegisterVM 的特有设计——MEMBER_SET/INDEX_SET 修改容器后，结果通过 objReg 隐式传递给后续 WRITEBACK_* 指令。这个数据流不在 IR 操作数中体现，必须在 collectVRegLastUse 中特殊处理：跳过 MEMBER_SET/INDEX_SET 的 obj vreg，将其最后使用点延长到下一个 WRITEBACK_* 指令
- **Formatter 幂等性优先于"风格统一"**：Formatter 的核心不变量是往返幂等（format(parse(format(parse(src)))) == format(parse(src)))。强制包裹花括号虽然提升风格一致性，但破坏了幂等性。保留原样让 formatter 成为"纯格式化"工具而非"风格重构"工具
- **moduleMtimes_ 清理时机选在 execute() 入口**：而非 `reset()` 方法。原因：`reset()` 可能在会话内被多次调用（如 REPL 重置），清理 moduleMtimes_ 会导致同会话内模块重复加载。`execute()` 入口标志着新一次完整执行，此时清理最合适

### 修改文件清单

- 修改：`compiler/RegisterBytecodeBackend.h`（+5 成员变量 + 2 方法声明）
- 修改：`compiler/RegisterBytecodeBackend.cpp`（resetState 清理新成员 + vregToReg 复用逻辑 + collectVRegLastUse + releaseDeadVRegs + lower 调用点）
- 修改：`formatter/Formatter.cpp`（formatBlock 单语句体保留原样）
- 修改：`interpreter/Interpreter.cpp`（execute 入口清理 moduleMtimes_）
- 修改：`compiler/Compiler.cpp`（compileViaIR/compileViaRegisterIR 失败路径清理模块状态）
- 修改：`app/IdeController.cpp`（条件断点求值器设置 boundInstance_）
- 修改：`tests/TestThreeEnginesConsistency.cpp`（更新 1 测试预期 + 新增 1 溢出测试）
- 文档同步：`README.md`（测试徽章 1709→1710）+ `CHANGELOG.md`（本章节）+ `docs/development.md`（2026-07-07 摘要表头插入新行）+ `project_memory.md`（追加本轮关键决策与教训）

### 验证结果

- MSVC 19.51 + Qt 6.10.3 + Ninja 构建：`minilang_ide` target exit code 0；`minilang_tests` target exit code 0
- 全量测试：`ctest --test-dir out/build/debug` 1710/1711 通过（1 个 perf 占位符 Not Run，预先存在）
- 5 个 ThreeEnginesFuzz 失败测试全部修复（ControlFlowFuzz/ClosureFuzz/ArrayFuzz/ClassFuzz/DeterministicBaseline）
