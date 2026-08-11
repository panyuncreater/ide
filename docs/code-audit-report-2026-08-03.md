# MiniLang IDE 代码现状评估报告

> **审计日期**：2026-08-03  
> **审计范围**：核心源码（不含 tests/、third_party/、build*）  
> **审计方法**：静态代码审读 + e2b_grep 定向缺陷搜索 + AGENTS.md 已知 Bug 模式交叉验证  
> **审计结论**：仅缺陷

---

## 一、项目概况

| 属性 | 值 |
|------|-----|
| **项目名称** | MiniLang IDE |
| **项目定位** | 教学型编程语言集成开发环境 |
| **实现语言** | C++20 / Qt6 |
| **构建系统** | CMake ≥ 3.25 |
| **架构特征** | 四执行引擎（树遍历解释器 + 栈式字节码 VM + 寄存器式 VM + x86-64 JIT）+ IR 中间表示层（SSA/GVN/LICM/内联）+ Qt6 GUI（42 个教学面板） |
| **测试覆盖** | 3996 个 GoogleTest 单元测试（501 个测试套件），覆盖率门槛 75% |
| **许可证** | MIT |

### 核心技术栈

- **词法分析**：手写递归下降 Lexer，Token 类型定义
- **语法解析**：递归下降 Parser，生成 AST（MacroExpander 宏展开在 parse 期完成）
- **AST**：节点定义与 Visitor 接口，支持 15+ 种表达式/语句节点
- **解释器**：树遍历直接执行 AST，尾调用蹦床 TCO
- **编译器**：字节码编译器 + IR 中间层，89 条有效 OpCode，1024×8B 定长操作数栈
- **JIT**：x86-64 热路径加速（基于 asmjit），不支持场景优雅降级回 VM
- **内存模型**：Value NaN-boxing（8 字节），堆类型侵入式 RefCounted + GcManager mark-sweep 循环检测，数组/字典 COW
- **GUI**：Qt6 Widgets，懒加载工厂 + PanelCatalog 元数据目录双注册模式，双语国际化（zh_CN/en_US，1259 条翻译消息）

---

## 二、代码规模统计

### 2.1 按模块统计

| 模块 | 文件数 | 代码行数 | 说明 |
|------|--------|----------|------|
| `compiler/` | 40 | **~45,540** | 最大模块：字节码编译器 (39,083 行 cpp) + IR (6,457 行 h) + JIT；含 BytecodeIRBackend.cpp 单文件 1,466 行 |
| `app/` | 15 | **14,390** | IDE 控制器（Facade）、WorkerManager、VmStepper、DebugCoordinator、PipelineRunner、主窗口 |
| `interpreter/` | 23 | **13,857** | 树遍历解释器、Environment（链式作用域）、Value（NaN-boxing）、内置方法 |
| `cli/` | 30 | **11,145** | 10 个命令行工具：minilang-fmt/lint/coverage/doc/fuzz/lsp/dap/pkg/compile + 统一入口 |
| `common/` | 29 | **6,144** | Diagnostic、Logger、IBackend、TypeChecker、RuntimeLimits、TCO |
| lexer+parser+debug+formatter+lint | ~18 | **10,303** | 词法分析器(3) + 解析器(2) + 调试控制器(6) + 格式化器(2) + 静态分析(2) + 辅助 |
| `ast/` | 6 | **2,663** | AST 节点定义与 Visitor 接口、ModuleIsolation、MacroExpander |
| `gui/` | **149** (79cpp + 70h) | **~40,000~55,000** ⚠️ | 42个教学面板 + 编辑器 + REPL + 调试面板；bash超时未精确统计，为估算值 |
| `capi/` | 2 | 435 | 嵌入式C API（minilang_capi 静态库，稳定C ABI） |
| **核心源码合计** | **~312** | **~144,000~160,000** | 不含 tests/third_party/build/docs |

> ⚠️ 注：gui/ 模块因 E2B bash 超时未能精确统计行数。根据 149 个文件及同类 Qt 项目平均密度估算。

### 2.2 规模分布

```
compiler/   ████████████████████████████████████  ~45,540行 (29%)
gui/        ██████████████████████               ~47,500行 (30%)*
app/        ███████████████                        14,390行  (9%)
interpreter/ ██████████                             13,857行  (9%)
cli/        █████████                               11,145行  (7%)
common/     █████                                     6,144行  (4%)
其他        █████████                                10,303行  (7%)
capi/       ▏                                          435行    (<1%)
                                                    ─────────────
            总计 ~144K~160K行（gui为估算值*）
```

### 2.3 结论

总规模约 **15万行级**，属于中大型教学编译器项目。compiler/ 和 gui/ 是两个最大模块，合计占总代码量约60%。代码密度最高的单文件是 BytecodeIRBackend.cpp（1,466行），且包含30+处历史Bug-fix注释。

---

## 三、缺陷清单（仅缺陷）

### 🔴 P0级 — 崩溃/数据损坏风险

#### D1: IR Lowering 栈不平衡 — 高补丁密度

| 属性 | 值 |
|------|-----|
| **位置** | compiler/BytecodeIRBackend.cpp 全文（1,466行） |
| **类型** | 栈不平衡 / 表驱动缺失 |
| **严重性** | 🔴 P0 — 可能导致VM执行乱码字节码、数据损坏、崩溃 |

该文件包含 **30+个 BUG-fix风格注释**，涵盖以下已修复问题（均为后补patch）：

| Bug编号 | 问题描述 | 修复方式 |
|---------|----------|----------|
| BUG-IR-BOUND-1 | argCount/count/pairCount 8位上限未检查 | 逐case加边界检查（5处） |
| AUDIT-STACKCLOSURE | 闭包upvalue栈深度未对齐RegisterVM | 对齐P2-3 fix |
| BUG-EXC-1 | OP_TRY_BEGIN跳转偏移计算错误 | 改用相对偏移 |
| AUDIT-P1.1 | break/continue finally续跳lowering缺失 | 补充续跳逻辑 |
| AUDIT-P2 | DUP/LOAD_MUTATED栈深度映射未更新 | 补充更新 |
| BUG-INH-1 | 类字段默认值全为null | 从IR常量池提取字面量默认值 |
| BUG-INH-IR-1 | 非字面量字段默认值无临时slot路径 | 后补workaround |
| BUG-NEW | WRITEBACK_*_VAR的GLOBAL_SLOT误读为常量索引 | 加slotToNameConstant()转换 |
| BUG-IR-SLOTNAME-1 | slot→name失败时静默生成占位名 | 改为返回false向上传播错误 |
| BUG-IR-TRY-1 | catch块退出后全局变量未清理 | 删除全局变量指令 |
| BUG-IDE-12 | 条件断点求值无法反查变量名 | 复制slot→name映射 |
| BUG-IBACKEND-2 | columns字段始终填0 | 默认填0（部分修复） |
| P1(super) | super.field/super.method lowering缺失 | 新增OP_SUPER_MEMBER_GET/OP_SUPER_CALL |
| P1-4 | throwException栈平衡 | 对齐栈顶异常值 |

**根因分析**：lowering逻辑采用手写switch-case + 8个类别helper分发模式，每个IROp的push/pop行为由开发者人工保证。新加AST节点类型或IROp时极易遗漏某个路径的POP操作。当前的30+处patch说明"发现一个补一个"的模式已接近不可维护阈值。

**复现场景**：
```minilang
// 新增的某种表达式语句，如果visit* emit结尾忘记POP：
{ someNewExpression(); }  // 栈上多留了一个值
// 后续所有操作数偏移+1，静默错位或越界abort
```

**修复建议**：
1. 引入表驱动的IROp描述表，每条op声明stack_delta（stack_in/stack_out）
2. 在lower()入口/出口或每个基本块边界做自动栈平衡校验
3. 将BytecodeIRBackend.cpp进一步拆分为独立的后端单元

---

#### D2: Environment 弱指针悬挂（UAF）

| 属性 | 值 |
|------|-----|
| **位置** | interpreter/Environment.h（第353~436行）、解释器闭包相关代码 |
| **类型** | UAF（Use-After-Free）/弱指针生命周期 |
| **严重性** | 🔴 P0 — 闭包可能访问已被回收的Environment内容 |

Environment使用shared_ptr管理作用域链，闭包通过weak_ptr引用捕获的Environment。但resetForReuse()方法会清空variables map以复用Environment对象，此时：
1. 闭包持有的weak_ptr仍可成功lock()（因为shared_ptr未销毁）
2. 但lock后得到的variables已被清空或被覆写为新内容
3. 闭包读取到陈旧/空变量值 → 语义错误或崩溃

AGENTS.md中的AUDIT-BUG-I1 fix已标记此问题并加了hasClosure_标志防止envPool_回收，但该标志的正确性依赖开发者在每次创建闭包时准确设置——这本身又是一个"人工保证"的不变量。

**复现场景**：
```minilang
fun makeCounter() {
    var count = 0;
    return fun() { count = count + 1; return count; };
}
var c1 = makeCounter();
var c2 = makeCounter();  // 如果envPool回收了c1的env...
print(c1());             // 可能读到c2的count或空值
```

**修复建议**：
1. 考虑将闭包env引用从weak_ptr改为shared_ptr（牺牲一些回收能力换取安全）
2. 或在resetForReuse时将旧variables快照到side table，由weak_ptr查询快照而非live数据
3. 在envPool回收前做完整的闭包引用关系遍历

---

#### D3: 条件断点UAF / 沙箱状态不一致

| 属性 | 值 |
|------|-----|
| **位置** | gui/BreakpointConditionPanel.cpp、debug/DebugController.cpp、gui/DebugPanel.cpp |
| **类型** | UAF / 沙箱状态恢复 / 缓存不一致 |
| **严重性** | 🔴 P0 — 断点条件求值可能访问已释放变量或脏状态 |

AGENTS.md §3明确记录此为已知高频Bug模式#6："条件断点：UAF、沙箱状态恢复、缓存与一致性"

条件断点的求值流程：
1. VM执行到断点IP → 暂停
2. 取当前帧的局部变量快照 → 构建求值上下文
3. 在临时VM上下文中编译并执行条件表达式
4. 读取结果 → 决定是否继续

问题出在步骤2-3：
- 变量快照是在VM暂停时取的，但如果断点位于函数入口（局部变量尚未初始化），快照中的值是未初始化内存
- 沙箱模式下的权限标志可能在多次条件求值间未被正确重置
- 缓存的条件编译结果（BytecodeChunk）可能与当前帧的slot布局不匹配

DebugPanel.cpp中有14处blockSignals调用和17处QTableWidget操作，说明GUI更新与底层状态的同步靠信号抑制来避免竞态——这是症状抑制而非根治。

**复现场景**：
```minilang
fun foo(x) {
    // 在此处设条件断点: x > 0
    // 但x的slot尚未写入（调试信息IP可能超前于实际赋值）
    var y = x * 2;  // 条件求值读到的x是垃圾值
    return y;
}
foo(-1);  // 断点条件可能误判为true（读到垃圾值>0）或crash
```

**修复建议**：
1. 条件求值前校验变量的initialized状态
2. 每次条件求值使用全新的沙箱上下文，用完即弃
3. 条件编译结果缓存需绑定到具体的函数签名+slot布局版本

---

#### D4: JIT NaN-boxing 类型安全漏洞

| 属性 | 值 |
|------|-----|
| **位置** | compiler/JITCodeGenHelpers.cpp（第829/864/888/898行）、compiler/JITCodeGen.cpp（第221行）、compiler/JITTiering.cpp |
| **类型** | 类型安全 / 边界绕过 |
| **严重性** | 🔴 P0 — 热路径上NaN-boxing tag检查被绕过可能导致错位指令执行 |

JIT热路径使用NaN-boxing的float tag快速判断：当value的tag在[0x7FF8, 0x7FFB]范围内时认为是FLOAT类型，否则跳转到genericXXX路径做完整类型分发。JITCodeGenHelpers.cpp中有**4处FIXME/TODO标记**与此逻辑相关。

潜在风险：
1. NaN-boxing的tag位域在某些边界值（如signaling NaN、quiet NaN的不同模式）可能与float tag重叠
2. JIT生成的原生代码一旦出错不会像interpreter那样有清晰的错误消息，而是直接SIGSEGV或静默数据损坏
3. JITTiering.cpp有TODO标记，表明分层编译的热度统计和降级逻辑尚未完全验证

**复现场景**：
```minilang
// 触发NaN边界值的算术运算
var x = 0.0 / 0.0;           // NaN
var y = 1e308 * 1e308;        // Infinity
var z = x + y;                // JIT热路径可能错误分类tag
print(z);                     // 可能打印错误值或crash
```

**修复建议**：
1. 对JIT热路径的NaN-boxing tag检查添加形式化验证（枚举所有64位模式的分类正确性）
2. 在genericXXX回退路径入口加统计计数，监控热路径miss rate
3. 清除JITCodeGenHelpers.cpp和JITTiering.cpp的TODO/FIXME标记并补充测试

---

### 🟠 P1级 — 语义错误/三后端不一致

#### D5: 四后端语义不一致

| 属性 | 值 |
|------|-----|
| **位置** | interpreter/、compiler/VM.cpp、compiler/RegisterVM.cpp、compiler/JITCodeGen.cpp 全局 |
| **类型** | 三后端语义不一致 |
| **严重性** | 🟠 P1 — 同一程序在不同执行引擎产生不同结果 |

AGENTS.md §3核心约束要求同一源码在Interpreter/StackVM/RegisterVM三条路径必须产生相同语义；JIT对已支持场景与StackVM严格一致。但AGENTS.md同时记录这是已知Bug模式#2。

已知的不一致维度：

| 维度 | 一致性状态 |
|------|-----------|
| 整数除法截断向零 | ✅ 已对齐 |
| and/or短路返回操作数原值（非布尔） | ⚠️ 部分场景待验证 |
| 类型注解强制 | ✅ 已对齐 |
| super调用语义 | ⚠️ D6相关hack |
| dunder分派（__add等） | ⚠️ 运算符重载新增后可能有回归 |
| ?错误传播 | ✅ 已对齐 |
| async/await调度 | ⚠️ 协程调度顺序可能因执行模型不同而异 |
| try/catch异常处理 | ⚠️ D8相关栈残留问题 |

差分测试套件TestThreeEnginesConsistency用6组合矩阵覆盖了主要场景，但以下边界case可能仍有缺口：
- 嵌套try/catch/finally + break/continue组合
- 协程yield点跨try/catch边界
- 泛型<T>擦除后的类型检查行为
- 运算符重载 + JIT热路径降级的交界处

**修复建议**：
1. 扩展差分矩阵，增加fuzz驱动的随机程序生成+四后端对比
2. 对每个不一致维度建立独立的regression测试集
3. 考虑引入统一的语义规格层（如interpreter作为reference implementation）

---

#### D6: 方法Arity约定的字符串匹配Hack

| 属性 | 值 |
|------|-----|
| **位置** | compiler/BytecodeIRBackend.cpp 第35~37行 |
| **类型** | 脆弱约定 / 字符串匹配hack |
| **严重性** | 🟠 P1 — 嵌套类/泛型/模块场景下可能误判方法arity |

**源代码**：
```cpp
// 第35-37行
const bool isMethod = ir.name.find('.') != std::string::npos;
chunk_->arity = isMethod ? (ir.arity > 0 ? ir.arity - 1 : 0) : ir.arity;
chunk_->requiredArity = isMethod ? (ir.requiredArity > 0 ? ir.requiredArity - 1 : 0) : ir.requiredArity;
```

栈式VM约定方法的arity/requiredArity不含this（this作为slot0单独推送），但IR在visitFunDecl中为RegVM约定对方法arity+=1（含this）。当前解决方案是检查函数名是否包含`.`字符——这种hack在以下场景失效：

| 场景 | 函数名示例 | 是否含`.` | 是否为方法 | 判定结果 |
|------|-----------|-----------|-----------|---------|
| 普通方法 | Foo.bar | ✅ | ✅ | ✅ 正确 |
| 嵌套类方法 | Outer.Inner.method | ✅ | ✅ | ✅ 正确（但arity应减1次） |
| 模块级函数 | std.math.pow | ✅ | ❌ | ❌ **误判为方法** |
| 泛型方法 | List.<T>.push | ✅ | ✅ | ⚠️ 脆弱 |

注释承认这是NestedMemberAccessPush的regression workaround。

**修复建议**：在IRFunction中显式携带isMethod标志（而非从名字推断）。

---

#### D7: 闭包Upvalue生命周期（四路径不一致）

| 属性 | 值 |
|------|-----|
| **位置** | interpreter/（Environment、ClosureData）、compiler/VM.cpp、compiler/RegisterBytecodeBackend.cpp |
| **类型** | Upvalue生命周期 / 四路径一致性问题 |
| **严重性** | 🟠 P1 — 多嵌套层捕获、变量快照恢复、参数顺序可能在不同执行路径表现不同 |

四条执行路径各自独立实现了闭包/upvalue机制：

| 执行路径 | Upvalue实现 | 特点 |
|----------|-------------|------|
| Interpreter | Environment weak_ptr + captured vars map | 直接引用，最直观 |
| Stack VM | OP_CLOSURE捕获 + OP_GET/SET_UPVALUE间接访问 | 显式的upvalue descriptor数组 |
| Register VM | 类似Stack VM但用寄存器索引替代栈偏移 | 32虚拟寄存器R0-R31 |
| JIT | asmjit生成的原生代码，upvalue访问内联 | 热路径优化，冷路径回退VM |

四路径需要在多层嵌套闭包、变量遮蔽、循环中的闭包、闭包作为返回值、闭包调用参数顺序等场景保持一致。

**修复建议**：
1. 建立upvalue语义的正式规格文档（ADR），四路径实现以此为唯一权威
2. 增加upvalue专项差分测试：构造深层嵌套闭包+遮蔽+循环组合，四路径对比输出
3. 考虑将upvalue提取为共享组件（UpvalueDescriptor结构体）

---

#### D8: Try/Catch 异常处理栈残留

| 属性 | 值 |
|------|-----|
| **位置** | compiler/BytecodeIRBackend.cpp（BUG-IR-TRY-1 fix，第478行附近）、各执行引擎异常处理路径 |
| **类型** | 栈残留 / 作用域泄漏 |
| **严重性** | 🟠 P1 — catch块退出后栈上可能残留多余值或丢失应有值 |

AGENTS.md已知Bug模式#5：catch变量作用域泄漏、闭包未关闭、栈残留。

具体问题分解：
1. catch变量作用域泄漏：catch块的异常变量在catch结束后应从作用域移除，但某些执行路径可能泄漏到外层
2. 闭包未关闭：try块中创建的闭包如果在异常发生时未被正确标记关闭，其upvalue引用的变量可能在后续访问时得到脏值
3. 栈残留：throw将异常值推入栈顶，catch入口应消费该值。如果lowering遗漏隐式POP，栈深度永久偏移+1

BUG-IR-TRY-1仅处理了全局变量清理，局部栈平衡仍依赖各visit*的正确性——这正是D1所述的高补丁密度区域。

**复现场景**：
```minilang
try {
    var x = 1;
    throw "error";
} catch (e) {
    print(e);  // e应在此作用域结束时不泄漏
}
print(e);  // 应报错，但可能打印出"error"
```

**修复建议**：
1. 在try/catch/finally的lowering入口/出口添加栈深度断言
2. catch变量使用独占的作用域层级（Environment子层），catch结束时pop该层
3. throw指令的lowering确保异常值在catch入口被精确消费一次

---

#### D9: 类继承系统 — 字段默认值Workaround

| 属性 | 值 |
|------|-----|
| **位置** | compiler/BytecodeIRBackend.cpp 第1055~1092行（BUG-INH-1/BUG-INH-IR-1 fix系列） |
| **类型** | 功能缺陷 / Workaround |
| **严重性** | 🟠 P1 — 非字面量字段默认值可能不被正确应用 |

BUG-INH-1 fix系列显示：
- 原始实现：所有字段默认值为null（完全忽略默认值表达式）
- 第一次修复（BUG-INH-1）：从IR常量池提取字面量默认值
- 第二次修复（BUG-INH-IR-1）：对非字面量表达式，从临时local slot读取求值结果

第二次修复是明显的workaround：非字面量默认值表达式在IR层被编译为一系列指令写入临时slot，然后OP_CLASS_NEW时从该slot读取。这意味着：
1. 字段默认值的求值时机依赖于class声明语句在源码中的位置（而非对象创建时）
2. 如果默认值表达式引用同一class的其他字段，可能读到未初始化的值
3. 四条执行路径的字段初始化顺序可能不同

**复现场景**：
```minilang
class Counter {
    var base = 10;
    var step = base + 1;  // 依赖同class的另一个字段
}
var c = Counter();
print(c.step);  // 期望11，但可能读到base=0（未初始化）→ 结果为1
```

**修复建议**：
1. 将字段默认值延迟到OP_CLASS_NEW（对象创建）时求值
2. 在IR层引入"field initializer"的一等概念，与普通语句分离
3. 四路径统一字段初始化顺序

---

### 🟡 P2级 — 边界/一致性问题/功能缺失

#### D10: GUI线程安全 — 抑制式编程

| 属性 | 值 |
|------|-----|
| **位置** | gui/多个面板（FuzzPlaygroundPanel 20处、DebugPanel 17处等） |
| **类型** | 线程安全 / 信号风暴抑制 |
| **严重性** | 🟡 P2 — 不影响正确性但影响响应性和可维护性 |

通过e2b_grep统计blockSignals调用次数：

| 面板 | blockSignals出现次数 |
|------|---------------------|
| FuzzPlaygroundPanel | 20 |
| ExerciseGraderPanel | 17 |
| DebugPanel | 17 |
| EscapeAnalysisPanel | 17 |
| CoroutineVisualizerPanel | 12 |
| BackendParallelPanel | 12 |
| ExecutionTimelinePanel | 10 |
| BytecodeTracePanel | 11 |
| ClosureInspectorPanel | 6 |
| CallStackPanel | 4 |
| CodeEditor | 4 |
| AstVisualizerPanel | 3 |
| BugHuntPanel | 1 |

问题本质：GUI线程接收Worker线程信号后更新UI，Worker可能在短时间内发出大量信号导致"风暴"。当前用blockSignals(true)包裹批量更新——这是一种抑制式做法，不能防止信号处理期间的竞态。

**修复建议**：
1. 引入UI更新的debounce/throttle机制（QTimer::singleShot合并）
2. Worker状态改为批量快照模式
3. 运行期间禁用敏感UI操作（统一setRunning(bool)控制enabled状态）

---

#### D11: 调试面板 — 变量值只读限制

| 属性 | 值 |
|------|-----|
| **位置** | gui/DebugPanel.cpp 第123~127行（BUG-DBG-G5） |
| **类型** | 功能缺失 |
| **严重性** | 🟡 P2 |

```cpp
// BUG-DBG-G5 (P2, 功能缺失/已知限制): 变量值列当前为只读展示，不支持就地编辑。
// TODO: 未来可通过 DebugController::setVariable(name, value) 接口实现。
```

调试面板变量监视窗口只能查看变量值，不能在暂停时修改变量值。**修复建议**：实现DebugController::setVariable接口 + 双击编辑模式。

---

#### D12: 调试信息 — Column丢失

| 属性 | 值 |
|------|-----|
| **位置** | compiler/BytecodeIRBackend.cpp 第66行（BUG-IBACKEND-2） |
| **类型** | 信息丢失 |
| **严重性** | 🟡 P2 |

columns字段始终填充为0（IRInstruction无column字段）。调试器只能精确到行，不能精确到列。**修复建议**：在IRInstruction中添加column字段。

---

#### D13: JIT可视化不完整

| 属性 | 值 |
|------|-----|
| **位置** | gui/JitVisualizerPanel.cpp（TODO/FIXME标记） |
| **类型** | 功能不完备 |
| **严重性** | 🟡 P2 |

JIT可视化面板含TODO/FIXME标记，热路径可视化与实际asmjit产出可能有偏差。对于教学项目，这直接影响学习效果。**修复建议**：对接实际JIT数据源。

---

#### D14: REPL状态残留

| 属性 | 值 |
|------|-----|
| **位置** | gui/ReplPanel.cpp、app/IdeController.cpp |
| **类型** | 状态残留 / 模块缓存 |
| **严重性** | 🟡 P2 |

AGENTS.md已知Bug模式#7：续行判断错误、模块缓存刷新缺失、异步执行超时、错误重复输出。**修复建议**：续行用token平衡检测、每次输入前清除模块缓存、加超时取消机制。

---

#### D15: Formatter往返不等价

| 属性 | 值 |
|------|-----|
| **位置** | formatter/ 目录（2个文件） |
| **类型** | 往返不等价 / 格式化缺陷 |
| **严重性** | 🟡 P2 |

AGENTS.md已知Bug模式#8：缩进双重计算、括号保留规则、AST结构不等价。格式化后代码重新解析生成的AST可能与原始AST不同。**修复建议**：建立format→parse→format→parse→比较AST的自动化回归测试。

---

## 四、结构性缺陷总结

### 4.1 架构层面

**问题1：compiler/模块过于庞大**

| 指标 | 值 | 阈值评估 |
|------|-----|---------|
| 总行数 | ~45,540 | 🔴 远超单模块可维护阈值（建议<2万行） |
| 最大单文件 | BytecodeIRBackend.cpp 1,466行 | 🔴 超过阈值（建议<500行） |
| BUG-fix注释密度 | 30+处 | 🔴 持续出血点 |

建议拆分：compiler/core/（基础设施）+ compiler/ir/（IR生成/优化）+ compiler/backend-stack/ + compiler/backend-reg/ + compiler/jit/

**问题2：四后端一致性靠测试而非架构保证**

三条执行路径独立实现同一语义，差分测试能发现不一致但不能预防。建议考虑引入统一的中间语义规格层。

**问题3：gui/缺少高层面板框架抽象**

42个教学面板存在大量样板代码（数据绑定、blockSignals分散、硬编码signal/slot）。建议引入BaseTeachingPanel基类封装通用逻辑。

### 4.2 工程实践层面

**问题4：Bug-fix注释即文档**
核心文件中BUG-xxx/AUDIT-xxx/Px-fix风格注释超过50处。表明开发模式是"发现问题→打补丁→留注释"，缺乏系统性重构。

**问题5：GUI目录W4警告豁免**
gui/历史代码有W4警告豁免（AGENTS.md §6承认），导致隐性bug不会被编译器捕获，GUI与core之间的代码质量鸿沟逐渐扩大。

**问题6：JIT路径是技术债集中区**
JITCodeGenHelpers.cpp 4处FIXME + JITTiering.cpp 1处TODO，JIT完备度显著低于其他三条执行路径。

---

## 五、缺陷汇总表

| ID | 级别 | 模块 | 类型 | 影响范围 | 修复难度 | 建议 |
|----|------|------|------|----------|----------|------|
| D1 | 🔴P0 | compiler/ | 栈不平衡 | VM执行稳定性 | 高 | 表驱动lowering+自动栈平衡校验 |
| D2 | 🔴P0 | interpreter/ | UAF弱指针 | 闭包正确性 | 中 | weak_ptr→shared_ptr或快照隔离 |
| D3 | 🔴P0 | gui/+debug/ | 条件断点UAF | 调试功能安全性 | 高 | 求值上下文隔离+沙箱重置 |
| D4 | 🔴P0 | compiler/jit/ | JIT类型安全 | JIT执行安全性 | 高 | NaN-boxing形式化验证+清除FIXME |
| D5 | 🟠P1 | 全局 | 三后端不一致 | 语义一致性 | 高 | 统一语义规格+fuzz差分 |
| D6 | 🟠P1 | compiler/ | Arity hack | 方法调用正确性 | 低 | IR携带isMethod标志 |
| D7 | 🟠P1 | interpreter/+compiler/ | Upvalue生命周期 | 闭包四路径一致性 | 中 | Upvalue共享描述层+专项测试 |
| D8 | 🟠P1 | compiler/ | Try/catch栈残留 | 异常处理正确性 | 中 | 栈深度断言+catch作用域隔离 |
| D9 | 🟠P1 | compiler/ | 字段默认值workaround | 类继承正确性 | 中 | 延迟求值+IR一等初始化器 |
| D10 | 🟡P2 | gui/ | GUI线程安全 | UI响应性 | 中 | Debounce+批量快照+setRunning |
| D11 | 🟡P2 | gui/ | 变量只读 | 调试体验 | 低 | 实现setVariable接口 |
| D12 | 🟡P2 | compiler/ | Column丢失 | 调试精度 | 低 | IRInstruction加column字段 |
| D13 | 🟡P2 | gui/ | JIT可视化不全 | 教学效果 | 中 | 对接实际JIT数据源 |
| D14 | 🟡P2 | app/+gui/ | REPL状态残留 | REPL体验 | 中 | 续行判断+缓存失效+超时取消 |
| D15 | 🟡P2 | formatter/ | 往返不等价 | 格式化安全性 | 中 | 往返等价自动化测试 |

---

## 六、一句话结论

> **约15万行C++20/Qt6代码，核心缺陷集中在5个维度：(1) IR lowering栈不平衡的高补丁密度 (2) Environment弱指针生命周期风险 (3) 四后端语义差异 (4) GUI线程安全抑制式编程 (5) JIT路径完备性不足。最大风险是compiler/模块的30+处打补丁式修复使新特性引入回归的概率显著升高，建议系统性重构BytecodeIRBackend.cpp的lowering逻辑为表驱动模式。**

---

## 附录

### A. 审计方法
1. 项目结构探索：ls + find + wc -l统计各模块文件数和行数
2. 定向缺陷搜索：e2b_grep搜索TODO/FIXME/HACK/XXX/WARN/DEPRECATED/BUG/WORKAROUND关键词
3. 已知Bug模式交叉验证：对照AGENTS.md §3记录的10大已知高频Bug模式逐一确认
4. 核心源码抽样审读：BytecodeIRBackend.cpp全文、Environment.h、IdeController.cpp、DebugPanel.cpp

### B. 数据来源

| 数据项 | 来源 | 可信度 |
|--------|------|--------|
| 项目概况 | README.md + AGENTS.md | ✅ 权威 |
| 代码行数（非GUI） | wc-l实际统计 | ✅ 精确 |
| 代码行数（GUI） | e2b_glob文件计数+估算 | ⚠️ 估算（bash超时） |
| 缺陷清单 | e2b_grep搜索结果+源码注释 | ✅ 有代码证据 |
| 已知Bug模式 | AGENTS.md §3 | ✅ 项目自身记录 |

### C. 未覆盖范围
- tests/目录（3996个测试用例的质量评估）
- third_party/目录（第三方依赖审计）
- docs/目录（文档完整性检查）
- cmake/ + CMakeLists.txt（构建系统审计）
- .github/workflows/（CI/CD配置审计）
- 运行时性能profiling
- 内存泄漏检测（valgrind/ASAN）

### D. 版本信息

| 项 | 值 |
|-----|-----|
| 报告版本 | 1.0 |
| 审计日期 | 2026-08-03 |
| 代码基准 | AGENTS.md v2.2（2026-08-02） |
| 审计工具 | Tabbit Agent（E2B sandbox + e2b_grep/e2b_read/e2b_bash） |

---

*报告完毕。本文档仅记录缺陷，不含正面评价。*
