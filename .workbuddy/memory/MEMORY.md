# MiniLang 项目记忆

## 项目概况
- **路径**: `C:\Users\v\Desktop\ide\ide`
- **技术栈**: Qt 6.10.3 + MSVC 2022 + VS2026, C++
- **类型**: 迷你编程语言解释器 + IDE
- **构建**: MSVC (vcxproj), 语法检查用 MinGW g++ + `-I"D:/qt/6.10.3/mingw_64/include"`

## 架构
- **Lexer**: 词法分析
- **Parser**: 递归下降，25+ AST节点类型（类继承体系）
- **Interpreter**: Tree-walking 解释器，访问者模式
- **Compiler**: AST → 字节码编译器（多帧 CompileResult 架构）
- **VM**: 栈式虚拟机，VMCallFrame，函数独立 chunk
- **GUI/IDE**: 代码编辑器、语法高亮、AST可视化、调试器、REPL、字节码查看、VM栈状态面板

## 关键文件
| 文件 | 职责 |
|------|------|
| compiler/VM.h/cpp | 虚拟机，VMStepInfo回调，getCurrentLine/ChunkName |
| compiler/Compiler.cpp | 字节码编译，多帧，类编译 |
| compiler/Bytecode.h | 字节码定义与反汇编 |
| compiler/Lexer.h/cpp | 词法分析 |
| compiler/Parser.h/cpp | 语法分析 |
| compiler/AST.h | AST节点定义 |
| compiler/Interpreter.h/cpp | Tree-walking解释器 |
| gui/VmStackPanel.h/cpp | VM栈状态面板（深色主题） |
| ide.h/cpp | 主窗口，VM Run/Step/Stop，字节码高亮 |
| ide.vcxproj + .filters | 项目文件 |

## 已完成功能
- 2.2 基础功能 30/30 ✅
- 2.3 扩展功能 14/15 ✅（仅缺VM栈状态可视化已补）
- 字节码编译+VM执行、类/OOP支持、闭包、数组/字典

## 三轮修复历史（关键Bug）
1. **Round 1**: VM栈可视化（VmStackPanel）、OP_CALL反汇编4字节修复
2. **Round 2**: 多帧重构后5个Bug（行号取错chunk、类未注册globals、init不执行、OP_CLOSURE栈泄漏、OP_SET_VAR栈语义）
3. **Round 3**: 新OpCode后4个Bug（OP_INDEX_SET/MEMBER_SET未回写、init方法不调用、VmStackPanel缺opNames、类声明未注册globals）

## OpCode一致性四维检查法
每个OpCode指令长度必须在4处一致：
1. **Compiler编码**（Compiler.cpp emitByte/emitShort）
2. **VM执行**（VM.cpp run() 中 readByte/readShort 次数）
3. **Bytecode反汇编**（Bytecode.h disassembleInstruction 偏移步进）
4. **ide高亮**（ide.cpp highlightBytecodeLine 偏移步进）

## 关键设计决策
- OP_SET_VAR：赋值是语句不推栈（无push）
- OP_CLOSURE：声明后需OP_POP防栈泄漏（OP_CALL按名查找不从栈取）
- OP_CLASS_NEW：无条件调用init方法（不论argCount）
- compileClassDecl：用OP_DEFINE_VAR注册到globals（非OP_POP）
- 字节码查看：QListWidget + chunkRowMap_支持多chunk定位
- VM步进回调：VMStepInfo struct + setStepCallback/Enabled
- **VM自持mainChunk_副本**：initExecution拷贝result.mainChunk到VM成员，避免悬空指针
- **hasError_标志**：runtimeError设置hasError_=true，execute/stepOnce检测后立即终止
- **peek边界检查**：越界时返回static nullSentinel引用并报运行时错误
- **numericOp枚举分发**：用int枚举替代字符串比较（OP_ADD_INT等）
- **OpCode统一名称映射**：Bytecode.h中的opCodeName()函数，VmStackPanel等直接引用
- **ipToInstrIndex预计算映射**：BytecodeChunk::buildIpMap()在编译后调用，highlightBytecodeLine O(1)查找
- **instructionSize()**：BytecodeChunk静态方法，返回每个OpCode的字节长度
- **热路径const引用**：VM executeOneInstruction中所有string变量改为const std::string&

## 2026-06-09 综合代码审查
- 工程保障团队审查完成：5位审查员（3×代码审查+2×测试），去重后44项发现
- 🔴严重8项：peek/pop栈操作安全、常量池越界、OP_METHOD_CALL字段顺序、OP_CLOSURE未捕获环境、BUILD_ARRAY溢出、ClassInfo裸指针、push溢出不终止
- 🟠高12项：Value胖结构体(245B)、VM/Interpreter语义不一致、闭包语义缺失、比较运算无类型检查等
- 功能完成度：必做6/6✅ 选做4/5✅+1⚠️（VM嵌套赋值不支持）
- 报告已落盘：deliverables/engineering-assurance/code-review-minilang-2026-06-09.md

## 2026-06-09 综合代码审查
- 工程保障团队5位审查员（3×Cody+2×Tessa），去重后43项发现
- 🔴严重10项：peek/pop栈安全、常量池越界、OP_METHOD_CALL字段顺序、OP_CLOSURE未捕获环境、BUILD_ARRAY溢出、ClassInfo裸指针、push溢出不终止、局部变量slot乱序、QEventLoop重入
- 🟠高11项：Value胖结构体(245B)、VM/Interpreter语义不一致、闭包缺失、比较无类型检查等
- 功能完成度：必做6/6✅ 选做4/5✅+1⚠️
- 报告：deliverables/engineering-assurance/code-review-minilang-2026-06-09.md

## 待扩展功能（计划中）
- 类型注解、REPL增强、代码格式化、字节码VM优化
- **优先修复**：10项🔴严重问题（见审查报告行动清单）
- **架构改进**：Value重构为std::variant、VM闭包环境捕获、VM/Interpreter语义统一
- **优先修复**：8项🔴严重问题（见审查报告行动清单）
- **架构改进**：Value重构为std::variant、VM闭包环境捕获、VM/Interpreter语义统一
