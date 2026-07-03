# Changelog

本文件记录 MiniLang IDE 的开发演进历史，包括性能优化、正确性修复与工程基础设施改进。所有条目均通过全量单元测试（1047/1047）+ formatter_audit 审计用例验证。

## 2026-07-02 · VM Import 功能完善

### 新增

- **VM 路径支持 import 语句**：Compiler::visitImportStmt（直接路径）与 AstIRBuilder::handleImportStmt（IR / 寄存器路径）采用编译期模块内联策略——加载模块源码、解析 AST、预扫描全局槽位、内联编译模块语句到主程序。三路径共用 GlobalSlotAllocator，模块代码内联到同一全局作用域。
- **run-once 语义**：`linkedModuleSet_`（已完成）+ `moduleLoadingSet_`（编译中）保证同一模块只编译一次，避免重复定义。
- **路径安全（SEC-1）**：`normalizeModulePath` 拒绝空路径、绝对路径、`..` 路径段，防止路径遍历攻击。
- **IDE 集成层**：`IdeController::setupCompilerModuleLoader()` 基于当前文件路径为 Compiler 注入模块加载器，对齐 WorkerManager 为 Interpreter 设置的加载器逻辑。
- **VM import 测试**：新增 30 个测试用例（VME2EImport 16 个 / VME2EImportIR 9 个 / VME2EImportReg 5 个），覆盖全部导入、命名导入、类导出、run-once、循环依赖检测、模块不存在、嵌套导入、路径遍历防护、递归函数、菱形依赖等。

### 修复

- **IR.h 不完整类型崩溃**：`moduleAsts_` 是 `vector<unique_ptr<Block>>`，Block 在 IR.h 中仅有前向声明，PCH 删除后某些翻译单元看到不完整类型导致内存布局不一致和运行时崩溃。改为在 IR.h 中直接 `#include "ast/ASTNode.h"`。
- **preScanTopLevelDecls 漏处理 FunDecl / ExportStmt**：原实现仅处理 VarDecl 和 ClassDecl，导致 IR 路径前向函数引用和 export 声明全局槽位未预分配。对齐 Compiler::preScanModuleGlobals 补全 switch 分支。
- **handleImportStmt 错误后继续生成坏 IR**：`hasError_` 设置后 `build()` 主循环和模块语句循环未短路中止。已加入 `hasError_` 检查提前退出。
- **loadAndParseModule 吞掉 Lexer/Parser 错误**：原实现仅检查 AST 是否为空，可恢复错误的 AST 被当作成功加载。改为检查 `lexer.getDiagnostics().hasErrors()` 和 `parser.hasErrors()` 并转发到编译诊断。
- **handleImportStmt 冗余补偿循环**：preScanTopLevelDecls 修复后，ExportStmt 包装的声明已在预扫描阶段处理，移除 handleImportStmt 中的重复循环。

## 2026-07-02 · 第八轮 UI 修复与细节打磨

### 工程基础设施

- **Windows 一键配置脚本**：新增 `configure.bat`，自动检测本机 Visual Studio 与 Qt6 安装位置（默认查找 `D:\qt\6.*\msvc2022_64` 与 `C:\qt\6.*\msvc2022_64`，可通过 `QTDIR` 环境变量覆盖），自动初始化 MSVC 开发环境后调用对应 CMake Preset 完成配置。新增 `windows-msvc-debug` / `windows-msvc-release` 两个共享 Preset（Ninja 生成器）。
- **CMake Preset 体系**：CMakePresets.json + CMakeUserPresets.json 双层分离——共享 Preset 在 `CMakePresets.json`（仓库内），机器特定 Qt 路径在 `CMakeUserPresets.json`（gitignore），避免硬编码路径污染仓库。

## 2026-07-01 · 第七轮 Bug 修复（AUDIT-BUG-G1~G4 + H5 系列）

针对三后端 IR 路径栈语义、Parser 类型注解识别、Formatter 缩进、默认参数 RAII 等问题，共修复 12 个 bug（2 P0 / 4 P1 / 6 P2），新增 16 个回归测试，全部 1047/1047 测试通过。

- **G1**（P0）：IR 路径 `AND/OR` 短路在顶层代码使用 `STORE_LOCAL/LOAD_LOCAL` 临时槽（`nextLocalSlot_` 分配），但 StackVM 主帧 `basePointer=0` 且栈初始为空，`OP_SET_LOCAL/OP_GET_LOCAL` 的 `bp+slot` 越界检查失败（"局部变量槽越疆"）。修复：`VM::initExecution` 为主帧预留 `mainChunk_.localCount` 个 null 槽。正常 Compiler 路径顶层代码用 `OP_DEFINE_VAR`（globals_ 哈希），`localCount=0`，此处为 no-op。
- **G2**（P1）：Parser `parseParamList` / `forStmt` 初始化用 `checkNext(TK_IDENTIFIER)` 识别 `ClassName paramName` 模式，遇到 `ClassName[] paramName` 时下一 token 是 `[` 而非标识符，导致误判为非类型声明。修复：新增 `isClassTypeDeclStart()` 同时识别 `ClassName paramName` 与 `ClassName[] paramName` 两种序列。
- **G3**（P1）：Formatter 裸复合语句（if/while/for 作为 `thenBranch`/`body`）出现双重缩进——L17 "修复" 在 4 处 `isSelfTerminating` 分支额外添加了 `currentIndent_++/--`，但 `formatNode` 内部已用 `currentIndent_++` 处理 body 缩进。修复：移除 4 处额外缩进对（revert L17）。
- **G4**（P0，IR 栈泄漏）：IR 路径 `build()/visitBlock()` 仅对 `FUN_CALL/METHOD_CALL` emit POP，其余 14 种表达式语句的返回值残留在栈上，循环内泄漏必然触发栈溢出。修复：`needsPopForExprStmt(NodeType)` 覆盖全部 16 种节点类型；`visitForStmt` 对 update 表达式与表达式 initializer emit POP；`visitAssignment` 对 GLOBAL 存储后 emit `LOAD_GLOBAL` 重载值确保 POP 安全。
- **H5**（P1）：StackVM `OP_CALL_EXPR`（闭包作为表达式调用）参数压栈顺序与 `OP_CALL` 不一致，导致闭包调用取到错误参数。修复：与 `OP_CALL` 对齐参数压栈顺序，新增 4 个 H5 回归测试覆盖 0/1/多参数与循环内调用。
- **BUGFIX-P2 / H2**（P2，条件断点 UAF）：`evaluateCondition` 恢复阶段先调用 `restoreLocalVariables`（整表替换 `variables`）会使 `inst` 指针悬垂，再执行 `inst->fields() = fields` 触发 UAF。修复：恢复顺序改为先恢复实例字段（`inst` 仍有效）再恢复局部变量；`Environment::restoreLocalVariables` 内部在新 map 中重新锚定 `boundInstance_` 防止悬垂。
- **L1**（P2，IR 栈残留）：IR 路径 `visitForStmt` 的 `JUMP_IF_FALSE` peek 不 pop，条件值在循环体/退出路径均残留栈上；`visitTryStmt` catchVarName 为空时异常值残留栈上。修复：for 循环引入 `exitLabel`（条件假路径，需 POP）与 `endLabel`（break 路径，栈已空）双标签模式；try 块 `STORE_LOCAL` 后 emit POP 对齐直接 Compiler 路径；catchVarName 为空时显式 emit POP。
- **BUGFIX-P2**（P2，RegisterVM 字段未找到无方法回退）：`REG_MEMBER_GET` / `REG_SUPER_MEMBER_GET` 字段未找到时直接报错，无方法回退（StackVM `OP_MEMBER_GET` 有 `findMethodChunk` 回退）。修复：沿继承链查找方法（与 StackVM 一致），未找到才报 "类 X 没有字段或方法 'Y'"。
- **BUGFIX-P2**（P2，RegisterVM 默认参数非字面量）：`fillDefaultArgs` 未处理 `0xFFFF` 哨兵值（表示非字面量默认表达式），错误地将 0xFFFF 当作常量索引导致越界。修复：与 StackVM 对齐，`0xFFFF` 返回 false 回退到 Interpreter 路径求值。
- **BUGFIX-P2**（P2，VM ASCII 缓存误命中）：`lastAsciiStrPtr_` 仅比对指针，堆地址复用（原 string 释放后新 string 复用同地址）会导致非 ASCII 字符串误判为 ASCII。修复：新增 `lastAsciiStrSize_` 同时比对指针 + size。
- **BUGFIX-P2**（P2，currentEnv_ 异常泄漏）：`callClosureValue` / `constructClassInstance` / `callNamedFunction` 中默认参数求值使用 `auto savedEnv = currentEnv_; ... currentEnv_ = savedEnv;` 手动恢复，`evaluate()` 抛异常时 `currentEnv_` 不恢复导致泄漏错误 scope。修复：改为 RAII guard（结构体析构函数恢复）。
- **AUDIT**（GUI/REPL 一致性）：(1) `irAction_` 在运行期间未禁用，查看正在编译/执行的 IR 导致状态不一致；(2) `onNew/onOpen` 在运行/调试期间未禁止，清空/替换正在执行的代码导致状态混乱；(3) `loadFile` 文件大小检查使用裸 `MAX_SOURCE_SIZE` 而非 `RuntimeLimits::MAX_SOURCE_SIZE`；(4) `ReplPanel` 异步执行 RuntimeError 经信号显示后，`pollReplFuture` 仍打印 "null" 结果造成重复输出——新增 `hadReplError_` 原子标志，错误时跳过结果输出；(5) `WorkerManager::stopForClose` 成功路径未与 `forceStop` 对齐做完整清理（debugMode / REPL 状态 / debugger / 主回调），导致下次运行读到脏状态；(6) `IdeController` VM 条件断点求值器每次命中都重新 Lexer+Parser 严重影响循环内条件断点性能——改用 `shared_ptr<unordered_map>` 缓存条件 AST。

## 2026-06-24 ~ 06-26 · 第六轮 Bug 修复（AUDIT-BUG-F1~F13 系列）

通过三个并行 agent 覆盖 Lexer/BuiltinFunctions/DebugController/GUI/Compiler/Interpreter 边界，共修复 13 个 bug（2 HIGH / 7 MED / 4 LOW），新增 5 个回归测试，全部 1047/1047 测试通过。

- **F1**（HIGH）：`constructClassInstance` 中 `callStack_.emplace_back` 在 `CallFrameGuard` 构造之前执行，guard 保存错误的 `savedStackDepth` 导致 init 帧不弹出，循环构造实例时 callStack_ 泄漏。修复：交换两行顺序，guard 先于 emplace_back。
- **F2**（HIGH）：REPL `reload` 命令用 `startsWith("reload")` 误匹配 `reloadable`/`reloadX` 等标识符，命中后清空用户输入。修复：改为精确匹配 + 空格前缀。
- **F3**（MED）：`VmStepper::vmCrossedDeeper_` 仅在 `stop()` 中重置，`stepByMode` 入口未重置，导致 STEP_OVER 退化为 STEP_IN。修复：在 stepByMode 入口添加重置。
- **F4**（MED）：`DebugController::crossedLine_` 在条件断点求值之前重置，条件不满足时已清 false，单行循环中条件断点永不再触发。修复：将重置推迟到断点真正命中之前。
- **F5**（MED）：`IrViewer::setUpdatesEnabled(false)` 后填充逻辑无异常保护，异常时 `setUpdatesEnabled(true)` 永不执行，列表永久冻结。修复：用 try/catch 包裹填充逻辑。
- **F6**（MED）：`onRun`/`onDebug` 在 `isReplRunning()` 检查之前执行 `clearAll()`，REPL 运行时清空用户历史输出后才报错。修复：将互斥检查移到 `clearAll()` 之前。
- **F7**（MED）：IR 路径 `visitTryStmt` 的 catch 变量绑定到外层 `varMap_` 且不恢复，catch 块后仍可引用，与 Interpreter/StackVM 语义不一致。修复：为三条路径（函数内、顶层遮蔽、顶层无遮蔽）添加 varMap_ 保存/恢复。
- **F8**（MED）：模块导入的 try/catch 块在异常时未调用 `moduleEnv->closeCapturedVariables()`，逃逸闭包的 capturedVars 保持初始值。修复：在 catch 块中添加 closeCapturedVariables 调用。
- **F9**（MED）：`RegisterVM::executeSharedBuiltinFunction` 调用仅传 3 参数，line/column 取默认值 0,0，错误消息显示"行 0:0"。修复：从当前帧的 chunk 获取行号并传入。
- **F10**（LOW）：Lexer 插值表达式中嵌套字符串的 `start_` 未在 `advance()` 前更新，导致 token 列号/lexeme 错误。修复：添加 `start_ = current_`。
- **F11**（LOW）：`substr` 对负 `start` 静默返回空串，但对负 `len` 报错，错误处理不对称。修复：负 `start` 改为报错。
- **F12**（LOW）：`SyntaxHighlighter` 不识别字符串插值 `{expr}`，整个字符串统一着色。修复：识别 `{` 作为插值起始，用 braceDepth 跟踪嵌套，插值表达式内字符不标记 mask。
- **F13**（LOW）：`CodeEditor` 空白区域点击 `cursorForPosition` 返回文档末尾光标，导致在最后一行设置断点。修复：检查 Y 坐标是否超出最后一个块的下边界。

## 2026-06-30 · 测试质量审计

审计发现并修复 2 个测试覆盖缺口：

- **TypeChecker `int` 注解拒绝 `float` 值零覆盖**：新增 5 个 CompilerTypeCheckTest 测试（A11-A15），覆盖声明/赋值路径的 float→int 拒绝，并强化断言：不仅验证警告计数，还验证警告消息内容含期望类型名，防止误报通过。
- **Formatter 语句级 AST 往返等价性仅覆盖表达式级（19 用例）**：新增 `test_statements()` 函数 + 16 个 S1-S16 测试用例覆盖 if/for/while/class/try 等语句结构，对每条语句做 `Parse(src)` vs `Parse(format(src))` 的 AST 结构等价比较，检测 Formatter 在格式化时是否丢失或重组子节点。

## 2026-06-18 ~ 06-26 · 性能优化与正确性阶段成果

近期完成了多个性能与正确性优化批次，全部通过 1047/1047 单元测试 + 100+ formatter_audit 审计用例。

- **VMUpvalue O(1) 帧定位**（#18）：`VMUpvalue` 新增 `owningFrameIdx` 字段，`OP_SET_UPVALUE` 用 O(1) 索引替代原 O(frames) 线性扫描定位目标帧；passthrough upvalue 复用 shared_ptr 自动透传该字段。
- **executeReturn 字段同步合并遍历**（#19）：Path B1 中"写 caller `this.fields()`"与"写 caller field slots"两次 O(fieldCount) 遍历合并为单次遍历，slot 索引用 `BytecodeChunk::fieldSlotIndex` O(1) 查找。
- **内建方法枚举分发**（#20）：提取共享自由函数 `classifyBuiltinMethod(name) -> BuiltinMethod` 枚举，VM / RegisterVM / Interpreter 共用；按 `name.size()` 快速筛选 + 单次字符串比较 + `switch` 分发，消除长串 if-else 字符串比较链；共享方法名（`len` / `contains`）用 fallthrough case 标签跨类型复用。
- **Environment boundInstance_ 缓存**（#21）：`get` / `set` / `hasVariable` 缓存 `lastCheckedInstance` 指针，`boundInstance_` 沿作用域链继承且同链多数层指针相同，命中缓存时跳过重复 `fields()` 哈希查找；仅当链上出现不同 `boundInstance_`（`bindInstance` 显式覆盖）时才重新检查。
- **类方法哈希索引**（#12）：`VM` 新增两级 `unordered_map<className, unordered_map<methodName, BytecodeChunk*>>` 索引，`initExecution` 一次性扫描 `functionChunks_` 构建，`findMethodChunk` 用哈希查找替代每层继承链拼 "Class.method" 字符串。
- **寄存器帧零堆分配**（#8）：`RegCallFrame::registers` 从 `std::vector<Value>` 改为 `std::array<Value, 32>` + `uint8_t registerCount`，利用 32 寄存器硬上限消除每帧堆分配。
- **VMStack 定长数组**（PERF-13）：VM 操作数栈使用 `Value[1024]` + `size_t top_` 替代 `std::vector`，消除 `push_back` 容量检查与堆分配开销。
- **NaN-boxing Value**（PERF-12）：`sizeof(Value)` 从 24 字节降至 8 字节，标量内联存储零原子操作，堆类型通过侵入式 `RefCounted` 基类管理一次原子递增。
- **字符串码位缓存**（#21）：StringData 缓存 `codepointCount`，消除 len()/substr() 在循环中的 O(n²) 重复扫描。缓存失效时机：非 const stringVal() 返回可变引用时 reset()。
- **IR 常量/全局去重哈希表**：IRFunction::addConstant/addGlobal 使用 hash 侧表维护去重索引，消除 O(n²) 线性扫描。constants/globalNames 不允许外部直接 push_back/clear。
- **RegClassInfo 懒预计算**：继承链展平字段和 init 方法名在 REG_DEFINE_CLASS 时预计算缓存，避免实例化时重复遍历继承链。
- **条件断点沙箱优化**：evaluateCondition 快照作用域链变量 + 绑定实例字段，求值后恢复。残余限制：容器变异仍影响共享 ref-counted 对象（Value 引用语义），已文档化为可接受现状。

## 2026-06-18 ~ 06-20 · 继承链正确性修复（BUG-INH 系列）

针对三后端继承语义一致性审计发现的问题：

- **BUG-INH-1**（P0）：IR 路径丢失类字段默认值表达式——`AstIRBuilder::visitClassDecl` 未收集字段初始化器字面量，所有字段被硬编码为 null。修复：DEFINE_CLASS IR 操作数新增 fieldDefaultConstIdx，lowering 时按 Value 类型 emit 对应常量加载指令。
- **BUG-INH-4**（P0）：StackVM IR 路径 `super.method()` 返回错误值——`LOAD_MUTATED + STORE_LOCAL` 后值残留导致 `RETURN` 弹出错误值。修复：visitMethodCall 的 isVarRef/isSuperCall 分支在 emitStoreVar 后，若存储目标是 LOCAL/UPVALUE 则显式 emit IROp::POP 消费残留值。
- **BUG-INH-3**（P2）：super 方法未找到时三后端错误消息不一致——RegVM 报 "父类链中无方法"/"无方法:" 与 Interpreter/StackVM 的 "没有方法" 不一致。修复：统一为 "类 X 没有方法 Y"。
- **BUG-INH-2**（P2，文档化）：非方法上下文中使用 super 时错误消息不一致——Interpreter 报 "super 只能在类方法中使用"，IR 路径报 "未定义的变量: this"。文档化为已知差异（不崩溃；错误类型相同均为 RuntimeError，三引擎均不被 try/catch 捕获，仅消息文本不同）。

## 2026-06-18 ~ 06-20 · 整数溢出与模块系统修复（BUG-OVF / BUG-MOD / SEC 系列）

- **BUG-OVF-1**（P1）：RegisterVM REG_NEGATE 整数溢出错误消息缺少后缀——统一为 "整数溢出：无法对最小值取负"。
- **BUG-OVF-2**（P1）：RegisterVM REG_DIV 整数溢出错误消息不一致——统一为 "整数运算溢出"（对齐 Interpreter/StackVM 的 computeArith DIV 分支）。
- **BUG-MOD-1**（P0）：IR 路径 import 语句崩溃（Debug）/静默生成坏 IR（Release）——AstIRBuilder 新增 hasError_/errorMessage_ 错误报告接口，NODE_IMPORT_STMT 独立 case 设置错误标志，compileViaIR/compileViaRegisterIR 检查并转化为用户可见 diagnostic。
- **SEC-1**（P0）：模块路径遍历攻击漏洞——路径校验移到 loader 检查之前，拒绝 ".." 父目录引用和绝对路径，防止 import 读取项目目录外文件。

## 2026-06-20 · REPL 正确性修复

- **REPL 模块缓存刷新**：原重新 import 模块时 `moduleCache_` 命中即复用旧 moduleEnv，不重新调用 loader 读源码——用户修改模块源文件后重新 import 仍得旧值。新增 `Interpreter::clearModuleCache(path)` / `clearAllModuleCache()` 方法；ReplPanel 新增 `reload "mod"` / `reload all` 命令清除缓存后下次 import 重新加载。
- **REPL 异步执行超时**：原 `waitReplFuture()` 调用 `replFuture_.wait()` 无超时，死循环场景下 closeEvent 永久阻塞。新增 `Interpreter::stopRequested_` 原子标志；`checkBreak` 在每个语句节点检查标志并抛 `std::runtime_error`（被异步 lambda 的 catch 捕获）；`waitReplFuture` 改为 `requestReplStop()` + `wait_for(5s)` + 超时回退阻塞。正常代码（含 checkBreak 调用）能在毫秒级响应中止。
