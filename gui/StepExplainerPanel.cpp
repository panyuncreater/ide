// ============================================================
// StepExplainerPanel.cpp — 执行步骤讲解生成器教学面板实现
// ------------------------------------------------------------
// 实现 2 子页：指令讲解生成器 / OpCode 分类速查。
// 子页 1 面板内部独立完成 Lexer → Parser → Compiler 流程，
// 对编译产物 mainChunk 逐指令生成自然语言讲解。
// ============================================================

#include "gui/StepExplainerPanel.h"

#include "common/Diagnostic.h"
#include "compiler/Compiler.h"
#include "interpreter/Value.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QSplitter>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <sstream>

// ============================================================
// StepExplainerLibrary — 静态 OpCode 教学文档库（82 条）
// ============================================================

/// 返回 82 个 OpCode 的静态文档数据，按 10 大分类分组：
/// 常量加载 / 算术运算 / 逻辑比较 / 栈操作 / 变量访问 / 控制流 /
/// 函数调用 / 容器操作 / 成员与类 / 其他。
const std::vector<StepOpCodeDocEntry>& StepExplainerLibrary::opCodeDocs() {
    static const std::vector<StepOpCodeDocEntry> kDocs = {
        // ---- 1. 常量加载（7）----
        {"OP_CONSTANT", "常量加载", "常量池索引(2B)", "压入 1 个值",
         "从常量池读取指定索引的值并压入栈顶。该指令已废弃，编译器改用 OP_INT/OP_FLOAT/OP_STRING，保留以兼容旧字节码。",
         "var x = 42;"},
        {"OP_INT", "常量加载", "常量池索引(2B)", "压入 1 个值",
         "从常量池读取整数常量并压入栈顶。整数采用 int64_t 编码，NaN-boxing 标签位区分 int/float。",
         "var x = 42;"},
        {"OP_FLOAT", "常量加载", "常量池索引(2B)", "压入 1 个值",
         "从常量池读取浮点常量并压入栈顶。浮点采用 IEEE 754 双精度编码。",
         "var pi = 3.14;"},
        {"OP_STRING", "常量加载", "常量池索引(2B)", "压入 1 个值",
         "从常量池读取字符串常量并压入栈顶。字符串为引用计数堆对象，常量池去重避免重复分配。",
         "var s = \"hello\";"},
        {"OP_NULL", "常量加载", "无", "压入 1 个值",
         "将 null 值压入栈顶。null 在 NaN-boxing 中用专用标签表示，与 false 区分。",
         "var n = null;"},
        {"OP_TRUE", "常量加载", "无", "压入 1 个值",
         "将布尔常量 true 压入栈顶。",
         "var b = true;"},
        {"OP_FALSE", "常量加载", "无", "压入 1 个值",
         "将布尔常量 false 压入栈顶。",
         "var b = false;"},
        // ---- 2. 算术运算（6）----
        {"OP_ADD", "算术运算", "无", "弹出 2 个值，压入 1 个结果",
         "从栈顶弹出两个值，执行加法运算，将结果压入栈顶。左操作数在栈深处，右操作数在栈顶。"
         "整数加法截断向零，浮点加法遵循 IEEE 754。字符串拼接走此路径。",
         "var z = x + y;"},
        {"OP_SUBTRACT", "算术运算", "无", "弹出 2 个值，压入 1 个结果",
         "弹出栈顶两个值（右、左），执行左减右后压入结果。整数减法截断向零，三后端统一。",
         "var z = x - y;"},
        {"OP_MULTIPLY", "算术运算", "无", "弹出 2 个值，压入 1 个结果",
         "弹出栈顶两个值相乘后压入结果。整数乘法截断向零，溢出回绕为 int64_t 补码语义。",
         "var z = x * y;"},
        {"OP_DIVIDE", "算术运算", "无", "弹出 2 个值，压入 1 个结果",
         "弹出栈顶两个值相除后压入结果。整数除法截断向零（三后端统一），浮点除法遵循 IEEE 754。",
         "var z = x / y;"},
        {"OP_MODULO", "算术运算", "无", "弹出 2 个值，压入 1 个结果",
         "弹出栈顶两个值取模后压入结果。符号与被除数一致（截断向零除法的余数），三后端统一。",
         "var z = x % y;"},
        {"OP_NEGATE", "算术运算", "无", "弹出 1 个值，压入 1 个结果",
         "弹出栈顶值取负后压入。对整数取负即 0-x，对浮点翻转符号位。",
         "var z = -x;"},
        // ---- 3. 逻辑比较（9）----
        {"OP_NOT", "逻辑比较", "无", "弹出 1 个值，压入 1 个结果",
         "弹出栈顶值取逻辑非后压入布尔结果。null/false 为假，其余为真。",
         "var b = !x;"},
        {"OP_EQUAL", "逻辑比较", "无", "弹出 2 个值，压入 1 个结果",
         "弹出栈顶两个值进行相等比较后压入布尔结果。跨类型比较（int vs float）按值比较。",
         "var b = x == y;"},
        {"OP_NOT_EQUAL", "逻辑比较", "无", "弹出 2 个值，压入 1 个结果",
         "弹出栈顶两个值进行不等比较后压入布尔结果。等价于 OP_EQUAL 结果取反。",
         "var b = x != y;"},
        {"OP_LESS", "逻辑比较", "无", "弹出 2 个值，压入 1 个结果",
         "弹出栈顶两个值（右、左），若左 < 右则压入 true。仅对数字类型有效。",
         "var b = x < y;"},
        {"OP_GREATER", "逻辑比较", "无", "弹出 2 个值，压入 1 个结果",
         "弹出栈顶两个值（右、左），若左 > 右则压入 true。仅对数字类型有效。",
         "var b = x > y;"},
        {"OP_LESS_EQUAL", "逻辑比较", "无", "弹出 2 个值，压入 1 个结果",
         "弹出栈顶两个值（右、左），若左 <= 右则压入 true。仅对数字类型有效。",
         "var b = x <= y;"},
        {"OP_GREATER_EQUAL", "逻辑比较", "无", "弹出 2 个值，压入 1 个结果",
         "弹出栈顶两个值（右、左），若左 >= 右则压入 true。仅对数字类型有效。",
         "var b = x >= y;"},
        {"OP_AND", "逻辑比较", "无", "无效果（已废弃）",
         "已废弃/死代码：编译器从不生成此指令。逻辑与通过 OP_JUMP_IF_FALSE 实现短路求值，"
         "返回操作数原值（非布尔），三后端统一。",
         "var b = x and y;"},
        {"OP_OR", "逻辑比较", "无", "无效果（已废弃）",
         "已废弃/死代码：编译器从不生成此指令。逻辑或通过 OP_JUMP_IF_FALSE 实现短路求值，"
         "返回操作数原值（非布尔），三后端统一。",
         "var b = x or y;"},
        // ---- 4. 栈操作（4）----
        {"OP_POP", "栈操作", "无", "弹出 1 个值",
         "弹出栈顶值并丢弃。表达式语句副作用求值后丢弃返回值，防止栈泄漏。",
         "foo();"},
        {"OP_DUP", "栈操作", "无", "压入 1 个值（复制栈顶）",
         "复制栈顶值并压入。用于需要保留操作数副本的场景（如复合赋值的中间步骤）。",
         "x = x + 1;"},
        {"OP_DUP_N", "栈操作", "深度(1B)", "压入 1 个值",
         "复制栈中第 N 个值到栈顶（不清除原值）。用于写回时保留索引值。",
         "arr[i] = v;"},
        {"OP_SWAP", "栈操作", "无", "交换栈顶两个值",
         "交换栈顶两个值。用于 R99 match 表达式：将 [scrutinee, body_result] 变为 "
         "[body_result, scrutinee]，随后 OP_POP 弹出 scrutinee 留下 match 结果。",
         "match x { ... }"},
        // ---- 5. 变量访问（10）----
        {"OP_DEFINE_VAR", "变量访问", "名称索引(2B)", "弹出 1 个值",
         "弹出栈顶值，以常量池中名称为键首次写入全局表。与 OP_SET_GLOBAL 的已存在赋值语义区分。",
         "var x = 10;"},
        {"OP_GET_VAR", "变量访问", "名称索引(2B)", "压入 1 个值",
         "按名称读取全局变量值并压栈。名称在常量池中索引。",
         "print(x);"},
        {"OP_SET_VAR", "变量访问", "名称索引(2B)", "弹出 1 个值",
         "弹出栈顶值，以常量池中名称为键写入全局表（已存在变量的赋值）。",
         "x = 10;"},
        {"OP_DELETE_VAR", "变量访问", "名称索引(2B)", "无效果",
         "删除全局变量。用于块作用域退出时清理全局变量（如 for 循环变量）。",
         "// 块作用域退出自动触发"},
        {"OP_GET_LOCAL", "变量访问", "槽位(1B)", "压入 1 个值",
         "读取当前栈帧中局部变量槽位并压栈。槽位是编译期分配的栈相对位置，访问 O(1)。",
         "// 函数内访问局部变量"},
        {"OP_SET_LOCAL", "变量访问", "槽位(1B)", "弹出 1 个值",
         "弹出栈顶值写入当前栈帧的局部变量槽位。",
         "x = 10;"},
        {"OP_GET_GLOBAL", "变量访问", "全局槽位(2B)", "压入 1 个值",
         "读取全局槽位的值并压栈。槽位在编译期由 GlobalSlotAllocator 分配，运行时 vector 直接访问。",
         "print(x);"},
        {"OP_SET_GLOBAL", "变量访问", "全局槽位(2B)", "弹出 1 个值",
         "弹出栈顶值写入全局槽位。",
         "x = 10;"},
        {"OP_DEFINE_GLOBAL", "变量访问", "全局槽位(2B)", "弹出 1 个值",
         "弹出栈顶值，首次写入全局槽位（定义新全局变量）。",
         "var x = 10;"},
        {"OP_DELETE_GLOBAL", "变量访问", "全局槽位(2B)", "无效果",
         "删除全局槽位变量。用于块作用域退出时清理。",
         "// 块作用域退出自动触发"},
        // ---- 6. 控制流（6）----
        {"OP_JUMP", "控制流", "跳转目标(2B)", "无效果",
         "无条件跳转到指定绝对字节偏移。用于 if/else 分支跳过 false 块。",
         "if (true) { print(\"yes\"); }"},
        {"OP_JUMP_IF_FALSE", "控制流", "跳转目标(2B)", "无效果（不消费条件值）",
         "检查栈顶条件，若为 false 则跳转到指定偏移。不弹出条件值，由编译器额外生成 OP_POP。",
         "if (cond) { ... }"},
        {"OP_LOOP", "控制流", "跳转目标(2B)", "无效果",
         "回跳到循环入口（负偏移，实际编码为绝对偏移）。用于 while/for 循环回边。",
         "while (cond) { ... }"},
        {"OP_RETURN", "控制流", "无", "弹出整个调用帧，压入返回值",
         "从当前函数返回，弹出整个调用帧，将返回值压入调用者栈顶。",
         "return x;"},
        {"OP_PUSH_JUMP_TARGET", "控制流", "目标(2B)", "压入 1 个跳转目标",
         "将真实跳转目标压入 pendingJumpStack_。break/continue 在 try-finally 内时，"
         "先 push 目标再 jump 到 finally 入口，保证先执行 finally 再跳转。",
         "try { break; } finally { ... }"},
        {"OP_FINALLY_END", "控制流", "无", "弹出 1 个跳转目标",
         "从 pendingJumpStack_ 弹出跳转目标并跳转；栈空则继续执行。实现 break/continue 的 finally 续跳。",
         "try { ... } finally { ... }"},
        // ---- 7. 函数调用（6）----
        {"OP_CALL", "函数调用", "名称索引(2B) + 参数个数(1B)", "弹出 N+1 个值，压入 1 个结果",
         "调用栈顶闭包：弹出 N 个参数 + 1 个闭包值，执行后压入返回值。名称索引用于调试与错误信息。",
         "result = foo(1, 2);"},
        {"OP_CALL_EXPR", "函数调用", "参数个数(1B)", "弹出 N+1 个值，压入 1 个结果",
         "表达式调用：闭包在参数下方（编译器先 push 闭包再 push 参数）。弹出 N 个参数 + 1 个闭包执行。",
         "var r = getFn()(x);"},
        {"OP_CLOSURE", "函数调用", "名称索引(2B) + upvalue数(1B) + upvalue描述", "弹出 N 个值，压入 1 个结果",
         "创建闭包值：从栈顶弹出 N 个 upvalue 描述符（每个为 isLocal+index 编码）+ 函数名，"
         "构造 ClosureData。变长指令（4 + 2*upvalueCount 字节）。",
         "fun outer() { var x = 1; fun inner() { return x; } return inner; }"},
        {"OP_GET_UPVALUE", "函数调用", "upvalue索引(1B)", "压入 1 个值",
         "读取闭包捕获的外层变量。若 upvalue 仍开放则从栈帧读取，已关闭则从堆读取。",
         "// 闭包内访问外层变量"},
        {"OP_SET_UPVALUE", "函数调用", "upvalue索引(1B)", "弹出 1 个值",
         "弹出栈顶值写入指定 upvalue。开放则写栈帧，已关闭则写堆。",
         "// 闭包内赋值外层变量"},
        {"OP_CLOSE_UPVALUE", "函数调用", "upvalue索引(1B)", "弹出 1 个值",
         "弹出栈顶值，将仍开放的 upvalue 迁移到堆。变量逃逸出作用域时触发，保证闭包捕获的变量生命周期正确。",
         "// 闭包捕获的变量逃逸"},
        // ---- 8. 容器操作（8）----
        {"OP_BUILD_ARRAY", "容器操作", "元素个数(1B)", "弹出 N 个值，压入 1 个结果",
         "弹出栈顶 N 个元素构建 ArrayData 并压入。数组为引用计数堆对象，COW 写时拷贝。",
         "var arr = [1, 2, 3];"},
        {"OP_BUILD_DICT", "容器操作", "键值对数(1B)", "弹出 2N 个值，压入 1 个结果",
         "弹出栈顶 2N 个值（key+value 对）构建 DictData 并压入。字典键支持 int/float/string/bool。",
         "var d = {\"x\": 1};"},
        {"OP_BUILD_TUPLE", "容器操作", "元素个数(1B)", "弹出 N 个值，压入 1 个结果",
         "弹出栈顶 N 个元素构建 TupleData 并压入。元组不可变（immutable），用于多返回值与解构。",
         "var t = (1, 2, 3);"},
        {"OP_INDEX_GET", "容器操作", "无", "弹出 2 个值，压入 1 个结果",
         "弹出索引与容器，读取 container[index] 并压栈。支持数组/字典/字符串/元组。",
         "var v = arr[0];"},
        {"OP_INDEX_SET", "容器操作", "无", "弹出 3 个值",
         "弹出值、索引、容器，执行 container[index] = value。COW 写时拷贝确保独占所有权。",
         "arr[0] = 99;"},
        {"OP_INDEX_SET_VAR", "容器操作", "名称索引(2B)", "弹出 2 个值",
         "弹出值与索引，直接修改 globals_[varName][index] = value。优化路径避免重复 load/store。",
         "arr[i] = 99;"},
        {"OP_INDEX_SET_LOCAL", "容器操作", "槽位(1B)", "弹出 2 个值",
         "弹出值与索引，直接修改 stack_[bp+slot][index] = value。局部变量数组赋值的优化路径。",
         "arr[i] = 99;"},
        {"OP_LEN", "容器操作", "无", "弹出 1 个值，压入 1 个结果",
         "弹出栈顶容器（array/dict/string/tuple），压入其长度（int）。用于模式匹配的编译期元素数检查。",
         "var n = len(arr);"},
        // ---- 9. 成员与类（12）----
        {"OP_MEMBER_GET", "成员与类", "名称索引(2B)", "弹出 1 个值，压入 1 个结果",
         "弹出实例，读取字段 name 的值并压栈。通过 fieldSlotIndex 查表定位字段槽位。",
         "var v = p.x;"},
        {"OP_MEMBER_SET", "成员与类", "名称索引(2B)", "弹出 2 个值",
         "弹出值与实例，写入字段 name。实例字段槽位赋值。",
         "p.x = 10;"},
        {"OP_MEMBER_SET_VAR", "成员与类", "变量索引(2B) + 字段索引(2B)", "弹出 1 个值",
         "弹出值，直接修改 globals_[varName].fields[fieldIdx] = value。全局变量成员赋值的优化路径。",
         "p.x = 10;"},
        {"OP_MEMBER_SET_LOCAL", "成员与类", "槽位(1B) + 字段索引(2B)", "弹出 1 个值",
         "弹出值，直接修改 stack_[bp+slot].fields[fieldIdx] = value。局部变量成员赋值的优化路径。",
         "p.x = 10;"},
        {"OP_METHOD_CALL", "成员与类", "名称索引(2B) + 参数个数(1B) + 接收者索引(2B)", "弹出 N+1 个值，压入 1 个结果",
         "方法调用：通过 methodCache_ 查找方法，避免重复 ClassInfo 遍历。弹出 N 个参数 + 1 个接收者执行。",
         "p.distance();"},
        {"OP_CLASS_NEW", "成员与类", "名称索引(2B) + 参数个数(1B)", "弹出 N+1 个值，压入 1 个结果",
         "类构造：弹出 N 个参数 + 类模板，创建 InstanceData 并调用 init 方法初始化字段。",
         "var p = Point(3, 4);"},
        {"OP_INIT_FIELD", "成员与类", "字段名索引(2B)", "弹出 1 个值",
         "从栈顶 pop 值设置到实例的字段。用于类构造函数 init 中初始化字段默认值。",
         "class Point { fun init(x) { this.x = x; } }"},
        {"OP_DEFINE_CLASS", "成员与类", "名称索引(2B) + 类信息(2B)", "弹出 1 个值",
         "从栈顶 pop 模板实例，提取类信息，注册到 VM 类表。类声明编译期的最终步骤。",
         "class Point { ... }"},
        {"OP_SUPER_CALL", "成员与类", "名称索引(2B) + 参数个数(1B) + 接收者(2B+1B) + 类索引(2B)", "弹出 N+1 个值，压入 1 个结果",
         "调用父类方法：从当前实例的父类链查找方法并调用。super 语义三后端统一，"
         "确保继承链中的方法回退正确。",
         "super.foo();"},
        {"OP_SUPER_MEMBER_GET", "成员与类", "名称索引(2B)", "弹出 1 个值，压入 1 个结果",
         "从父类查找字段并读取。super 成员访问语义三后端统一。",
         "super.x;"},
        {"OP_TYPE_CHECK", "成员与类", "类型注解索引(2B)", "无效果（peek 不弹栈）",
         "peek 栈顶值，检查是否兼容类型注解，不匹配则 runtimeError。不弹栈。三后端统一强制类型注解检查。",
         "var x: int = 42;"},
        {"OP_TYPE_TEST", "成员与类", "类型注解索引(2B)", "弹出 1 个值，压入 1 个结果",
         "软类型测试：pop 栈顶值，typeMatch 检查（含实例继承链），push bool。"
         "不匹配时 fall through 而非抛错，用于模式匹配 TUPLE pattern。",
         "match x { _ if x is int => ... }"},
        // ---- 10. 其他（14）----
        {"OP_PRINT", "其他", "无", "弹出 1 个值",
         "弹出栈顶值并输出到标准输出（带换行）。Value::toString 序列化所有类型。",
         "print(x);"},
        {"OP_WRITEBACK_MEMBER_VAR", "其他", "变量索引(2B) + 字段索引(2B)", "弹出 1 个值",
         "成员写回到全局变量：从 lastMutatedReceiver_ 取基对象，写入字段。用于嵌套左值变异 a.b.c = ...",
         "a.b.c = 1;"},
        {"OP_WRITEBACK_MEMBER_LOCAL", "其他", "槽位(1B) + 字段索引(2B)", "弹出 1 个值",
         "成员写回到局部变量：从 lastMutatedReceiver_ 取基对象，写入字段。",
         "a.b.c = 1;"},
        {"OP_WRITEBACK_INDEX_VAR", "其他", "变量索引(2B)", "弹出 1 个值",
         "索引写回到全局变量：索引从栈顶 pop，从 lastMutatedReceiver_ 取基对象执行索引赋值。",
         "a[i] = 1;"},
        {"OP_WRITEBACK_INDEX_LOCAL", "其他", "槽位(1B)", "弹出 1 个值",
         "索引写回到局部变量：索引从栈顶 pop，从 lastMutatedReceiver_ 取基对象执行索引赋值。",
         "a[i] = 1;"},
        {"OP_WRITEBACK_MEMBER_UPVALUE", "其他", "upvalue索引(1B) + 字段索引(2B)", "弹出 1 个值",
         "成员写回到 upvalue：从 lastMutatedReceiver_ 取基对象，写入字段。处理 a 是 upvalue 的嵌套左值变异。",
         "a.b.c = 1;"},
        {"OP_WRITEBACK_INDEX_UPVALUE", "其他", "upvalue索引(1B)", "弹出 1 个值",
         "索引写回到 upvalue：索引从栈顶 pop，从 lastMutatedReceiver_ 取基对象执行索引赋值。",
         "a[i] = 1;"},
        {"OP_LOAD_MUTATED", "其他", "无", "压入 1 个值",
         "将 lastMutatedReceiver_ 压入栈顶（不清除），供嵌套左值写回链使用。",
         "a.b.c = 1;"},
        {"OP_TRY_BEGIN", "其他", "catch偏移(2B)", "无效果",
         "注册 try 块的 catch 处理偏移。异常抛出时沿调用栈寻找匹配的 catch 并跳转到该处。",
         "try { ... } catch (e) { ... }"},
        {"OP_TRY_END", "其他", "无", "无效果",
         "try 块正常结束，弹出已注册的 catch 处理器。try 块未抛异常时走此路径清理。",
         "try { ... } catch (e) { ... }"},
        {"OP_THROW", "其他", "无", "弹出 1 个值",
         "弹出栈顶值作为异常对象并抛出，沿调用栈寻找匹配的 catch。catch 变量作用域需正确隔离。",
         "throw \"error\";"},
        {"OP_BUILD_ENUM_VARIANT", "其他", "枚举名(2B) + variant名(2B) + 参数数(1B)", "弹出 N 个值，压入 1 个结果",
         "从栈顶依次 pop argCount 个参数，构造 EnumVariantData(enumName, variantName, fields) 并 push。"
         "用于 R99 枚举与 ADT 构造。",
         "var s = Some(42);"},
        {"OP_ENUM_VARIANT_NAME", "其他", "枚举名(2B) + variant名(2B)", "弹出 1 个值，压入 1 个结果",
         "pop 栈顶 scrutinee，检查是否为 enum variant 且 enum/variant 名匹配，匹配 push true 否则 push false。"
         "用于 R99 match 模式匹配。",
         "match c { Red => ... }"},
        {"OP_ENUM_VARIANT_FIELD", "其他", "无", "弹出 2 个值，压入 1 个结果",
         "pop 栈顶 index，pop 栈顶 scrutinee，push scrutinee.fields[index]。"
         "若 scrutinee 非 enum variant 或 index 越界则 runtimeError。用于 match 解构字段提取。",
         "match opt { Some(x) => print(x); }"},
    };
    return kDocs;
}

// ============================================================
// 动态讲解生成
// ============================================================

namespace {

/// 格式化 DiagnosticBag 中的错误条目为 HTML（已转义），用于编译失败时显示。
QString formatDiagnosticErrors(const DiagnosticBag& bag) {
    QString result;
    for (const auto& d : bag.all()) {
        if (d.isError()) {
            result += QString::fromUtf8(d.format().c_str()).toHtmlEscaped() + QStringLiteral("<br>");
        }
    }
    if (result.isEmpty()) {
        result = QStringLiteral("（未知错误）");
    }
    return result;
}

/// 读取 ip+1 处的 16 位小端操作数（带边界检查）
uint16_t readShortOperand(const BytecodeChunk& chunk, size_t ip) {
    if (ip + 2 < chunk.code.size()) {
        return static_cast<uint16_t>(chunk.code[ip + 1]) |
               static_cast<uint16_t>(chunk.code[ip + 2] << 8);
    }
    return 0;
}

/// 读取 ip+1 处的 8 位操作数（带边界检查）
uint8_t readByteOperand(const BytecodeChunk& chunk, size_t ip) {
    if (ip + 1 < chunk.code.size()) {
        return chunk.code[ip + 1];
    }
    return 0;
}

/// 格式化常量池索引对应的值（带边界检查与 HTML 转义）
QString formatConstant(const BytecodeChunk& chunk, uint16_t idx) {
    if (idx < chunk.constants.size()) {
        QString val = QString::fromUtf8(chunk.constants[idx].toString().c_str()).toHtmlEscaped();
        return QStringLiteral("常量池索引: %1 (值: %2)").arg(idx).arg(val);
    }
    return QStringLiteral("常量池索引: %1 (越界)").arg(idx);
}

/// 按 OpCode 解析操作数字节，返回自然语言描述
QString describeOperands(const BytecodeChunk& chunk, size_t ip, OpCode op) {
    switch (op) {
    // 无操作数
    case OpCode::OP_NULL:
    case OpCode::OP_TRUE:
    case OpCode::OP_FALSE:
    case OpCode::OP_ADD:
    case OpCode::OP_SUBTRACT:
    case OpCode::OP_MULTIPLY:
    case OpCode::OP_DIVIDE:
    case OpCode::OP_MODULO:
    case OpCode::OP_NEGATE:
    case OpCode::OP_NOT:
    case OpCode::OP_EQUAL:
    case OpCode::OP_NOT_EQUAL:
    case OpCode::OP_LESS:
    case OpCode::OP_GREATER:
    case OpCode::OP_LESS_EQUAL:
    case OpCode::OP_GREATER_EQUAL:
    case OpCode::OP_AND:
    case OpCode::OP_OR:
    case OpCode::OP_PRINT:
    case OpCode::OP_POP:
    case OpCode::OP_RETURN:
    case OpCode::OP_INDEX_GET:
    case OpCode::OP_INDEX_SET:
    case OpCode::OP_DUP:
    case OpCode::OP_SWAP:
    case OpCode::OP_TRY_END:
    case OpCode::OP_THROW:
    case OpCode::OP_LOAD_MUTATED:
    case OpCode::OP_FINALLY_END:
    case OpCode::OP_ENUM_VARIANT_FIELD:
    case OpCode::OP_LEN:
        return QStringLiteral("无操作数");

    // 1 字节操作数：槽位 / 计数 / 索引
    case OpCode::OP_CALL_EXPR:
        return QStringLiteral("参数个数: %1").arg(readByteOperand(chunk, ip));
    case OpCode::OP_BUILD_ARRAY:
    case OpCode::OP_BUILD_DICT:
    case OpCode::OP_BUILD_TUPLE:
        return QStringLiteral("元素个数: %1").arg(readByteOperand(chunk, ip));
    case OpCode::OP_DUP_N:
        return QStringLiteral("深度: %1").arg(readByteOperand(chunk, ip));
    case OpCode::OP_GET_LOCAL:
    case OpCode::OP_SET_LOCAL:
        return QStringLiteral("局部槽位: %1").arg(readByteOperand(chunk, ip));
    case OpCode::OP_INDEX_SET_LOCAL:
        return QStringLiteral("局部槽位: %1").arg(readByteOperand(chunk, ip));
    case OpCode::OP_GET_UPVALUE:
    case OpCode::OP_SET_UPVALUE:
    case OpCode::OP_CLOSE_UPVALUE:
        return QStringLiteral("upvalue 索引: %1").arg(readByteOperand(chunk, ip));
    case OpCode::OP_WRITEBACK_INDEX_LOCAL:
        return QStringLiteral("局部槽位: %1").arg(readByteOperand(chunk, ip));
    case OpCode::OP_WRITEBACK_INDEX_UPVALUE:
        return QStringLiteral("upvalue 索引: %1").arg(readByteOperand(chunk, ip));

    // 2 字节：常量池索引 / 名称索引
    case OpCode::OP_CONSTANT:
    case OpCode::OP_INT:
    case OpCode::OP_FLOAT:
    case OpCode::OP_STRING:
    case OpCode::OP_DEFINE_VAR:
    case OpCode::OP_GET_VAR:
    case OpCode::OP_SET_VAR:
    case OpCode::OP_DELETE_VAR:
    case OpCode::OP_INDEX_SET_VAR:
    case OpCode::OP_MEMBER_GET:
    case OpCode::OP_MEMBER_SET:
    case OpCode::OP_SUPER_MEMBER_GET:
        return formatConstant(chunk, readShortOperand(chunk, ip));
    case OpCode::OP_INIT_FIELD:
        return QStringLiteral("字段名索引: %1").arg(readShortOperand(chunk, ip));
    case OpCode::OP_TYPE_CHECK:
    case OpCode::OP_TYPE_TEST:
        return QStringLiteral("类型注解索引: %1").arg(readShortOperand(chunk, ip));

    // 2 字节：跳转目标 / 全局槽位
    case OpCode::OP_JUMP:
    case OpCode::OP_JUMP_IF_FALSE:
    case OpCode::OP_LOOP:
        return QStringLiteral("跳转目标: %1").arg(readShortOperand(chunk, ip));
    case OpCode::OP_PUSH_JUMP_TARGET:
        return QStringLiteral("跳转目标: %1").arg(readShortOperand(chunk, ip));
    case OpCode::OP_GET_GLOBAL:
    case OpCode::OP_SET_GLOBAL:
    case OpCode::OP_DEFINE_GLOBAL:
    case OpCode::OP_DELETE_GLOBAL:
        return QStringLiteral("全局槽位: %1").arg(readShortOperand(chunk, ip));
    case OpCode::OP_TRY_BEGIN:
        return QStringLiteral("catch 偏移: %1").arg(readShortOperand(chunk, ip));

    // 名称索引(2B) + 参数个数(1B)
    case OpCode::OP_CALL:
    case OpCode::OP_CLASS_NEW: {
        uint16_t nameIdx = readShortOperand(chunk, ip);
        uint8_t argCount = (ip + 3 < chunk.code.size()) ? chunk.code[ip + 3] : 0;
        return QStringLiteral("%1, 参数个数: %2").arg(formatConstant(chunk, nameIdx)).arg(argCount);
    }

    // 变量索引(2B) + 字段索引(2B)
    case OpCode::OP_MEMBER_SET_VAR: {
        uint16_t varIdx = readShortOperand(chunk, ip);
        uint16_t fieldIdx = (ip + 4 < chunk.code.size())
                                ? static_cast<uint16_t>(chunk.code[ip + 3] | (chunk.code[ip + 4] << 8))
                                : 0;
        return QStringLiteral("变量索引: %1, 字段索引: %2").arg(varIdx).arg(fieldIdx);
    }
    // 槽位(1B) + 字段索引(2B)
    case OpCode::OP_MEMBER_SET_LOCAL:
    case OpCode::OP_WRITEBACK_MEMBER_LOCAL:
    case OpCode::OP_WRITEBACK_MEMBER_UPVALUE: {
        uint8_t slot = readByteOperand(chunk, ip);
        uint16_t fieldIdx = (ip + 3 < chunk.code.size())
                                ? static_cast<uint16_t>(chunk.code[ip + 2] | (chunk.code[ip + 3] << 8))
                                : 0;
        return QStringLiteral("槽位: %1, 字段索引: %2").arg(slot).arg(fieldIdx);
    }
    // 变量索引(2B) + 字段索引(2B)
    case OpCode::OP_WRITEBACK_MEMBER_VAR: {
        uint16_t varIdx = readShortOperand(chunk, ip);
        uint16_t fieldIdx = (ip + 4 < chunk.code.size())
                                ? static_cast<uint16_t>(chunk.code[ip + 3] | (chunk.code[ip + 4] << 8))
                                : 0;
        return QStringLiteral("变量索引: %1, 字段索引: %2").arg(varIdx).arg(fieldIdx);
    }

    // 名称索引(2B) + 参数个数(1B) + 接收者索引(2B) — OP_METHOD_CALL (7B)
    case OpCode::OP_METHOD_CALL: {
        uint16_t nameIdx = readShortOperand(chunk, ip);
        uint8_t argCount = (ip + 3 < chunk.code.size()) ? chunk.code[ip + 3] : 0;
        return QStringLiteral("%1, 参数个数: %2").arg(formatConstant(chunk, nameIdx)).arg(argCount);
    }
    // OP_SUPER_CALL (9B)
    case OpCode::OP_SUPER_CALL: {
        uint16_t nameIdx = readShortOperand(chunk, ip);
        uint8_t argCount = (ip + 3 < chunk.code.size()) ? chunk.code[ip + 3] : 0;
        return QStringLiteral("%1, 参数个数: %2").arg(formatConstant(chunk, nameIdx)).arg(argCount);
    }
    // OP_DEFINE_CLASS (5B)
    case OpCode::OP_DEFINE_CLASS: {
        uint16_t nameIdx = readShortOperand(chunk, ip);
        return formatConstant(chunk, nameIdx);
    }

    // 变长指令 OP_CLOSURE
    case OpCode::OP_CLOSURE: {
        uint16_t nameIdx = readShortOperand(chunk, ip);
        uint8_t upvalueCount = (ip + 3 < chunk.code.size()) ? chunk.code[ip + 3] : 0;
        return QStringLiteral("%1, upvalue 数: %2").arg(formatConstant(chunk, nameIdx)).arg(upvalueCount);
    }

    // R99 枚举指令
    case OpCode::OP_BUILD_ENUM_VARIANT: {
        uint16_t enumIdx = readShortOperand(chunk, ip);
        uint16_t variantIdx = (ip + 4 < chunk.code.size())
                                  ? static_cast<uint16_t>(chunk.code[ip + 3] | (chunk.code[ip + 4] << 8))
                                  : 0;
        uint8_t argCount = (ip + 5 < chunk.code.size()) ? chunk.code[ip + 5] : 0;
        return QStringLiteral("枚举名: %1, variant 名: %2, 参数个数: %3")
            .arg(formatConstant(chunk, enumIdx))
            .arg(formatConstant(chunk, variantIdx))
            .arg(argCount);
    }
    case OpCode::OP_ENUM_VARIANT_NAME: {
        uint16_t enumIdx = readShortOperand(chunk, ip);
        uint16_t variantIdx = (ip + 4 < chunk.code.size())
                                  ? static_cast<uint16_t>(chunk.code[ip + 3] | (chunk.code[ip + 4] << 8))
                                  : 0;
        return QStringLiteral("枚举名: %1, variant 名: %2")
            .arg(formatConstant(chunk, enumIdx))
            .arg(formatConstant(chunk, variantIdx));
    }
    }
    return QStringLiteral("无操作数");
}

} // anonymous namespace

/// 为 chunk 中 ip 处的指令生成动态 HTML 讲解：
/// 指令名 / 操作数值 / 栈效果 / 语义讲解 / 源码行号。
QString StepExplainerLibrary::generateStepExplanation(const BytecodeChunk& chunk, size_t ip, OpCode op) {
    const char* name = opCodeName(op);
    QString operandDesc = describeOperands(chunk, ip, op);
    int line = chunk.getLine(ip);

    // 查找静态文档条目获取栈效果与语义
    QString stackEffect = QStringLiteral("（未知）");
    QString semantics = QStringLiteral("（暂无文档）");
    QString category = QStringLiteral("其他");
    QString nameStr = QString::fromUtf8(name).toHtmlEscaped();
    for (const auto& doc : opCodeDocs()) {
        if (doc.name == name) {
            stackEffect = QString::fromUtf8(doc.stackEffect.c_str()).toHtmlEscaped();
            semantics = QString::fromUtf8(doc.semantics.c_str()).toHtmlEscaped();
            category = QString::fromUtf8(doc.category.c_str()).toHtmlEscaped();
            break;
        }
    }

    QString html = QStringLiteral(
        "<h3>%1</h3>"
        "<p><b>分类:</b> %2 | <b>源码行:</b> %3 | <b>IP:</b> %4</p>"
        "<h4>操作数值</h4>"
        "<p><code>%5</code></p>"
        "<h4>栈效果</h4>"
        "<p><code>%6</code></p>"
        "<h4>语义讲解</h4>"
        "<p>%7</p>")
        .arg(nameStr)
        .arg(category)
        .arg(line)
        .arg(ip)
        .arg(operandDesc)
        .arg(stackEffect)
        .arg(semantics);
    return html;
}

// ============================================================
// StepExplainerPanel 实现
// ============================================================

/// 构造面板：组装顶部子页切换（指令讲解生成器 / OpCode 分类速查）+ QStackedWidget，
/// 构建两个子页，预填示例代码，填充 OpCode 速查列表。
StepExplainerPanel::StepExplainerPanel(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);
    outer->setSpacing(4);

    // 子页切换按钮栏
    auto* pageBar = new QHBoxLayout;
    pageExplainBtn_ = new QPushButton(tr("指令讲解生成器"));
    pageOpCodeBtn_ = new QPushButton(tr("OpCode 分类速查"));
    pageExplainBtn_->setCheckable(true);
    pageOpCodeBtn_->setCheckable(true);
    pageExplainBtn_->setChecked(true);
    pageBar->addWidget(pageExplainBtn_);
    pageBar->addWidget(pageOpCodeBtn_);
    pageBar->addStretch();
    outer->addLayout(pageBar);

    stack_ = new QStackedWidget;
    auto* explainPage = new QWidget;
    auto* opCodePage = new QWidget;
    buildExplainPage(explainPage);
    buildOpCodePage(opCodePage);
    stack_->addWidget(explainPage);
    stack_->addWidget(opCodePage);
    outer->addWidget(stack_, 1);

    connect(pageExplainBtn_, &QPushButton::clicked, [this]() {
        stack_->setCurrentIndex(0);
        pageOpCodeBtn_->setChecked(false);
    });
    connect(pageOpCodeBtn_, &QPushButton::clicked, [this]() {
        stack_->setCurrentIndex(1);
        pageExplainBtn_->setChecked(false);
    });

    // 预填示例代码
    sourceEdit_->setText(QString::fromUtf8(
        "var x = 42;\nvar y = 10;\nvar z = x + y;\nif (z > 40) {\n    print(z);\n}\n"));

    populateOpCodes();

    // 占位提示
    stepDetail_->setHtml(QString::fromUtf8(
        "<div style='color:#6E6E6E; padding:8px;'><i>（点击「编译并生成讲解」后，选中上方表格的某行查看该指令的详细讲解）</i></div>"));
}

/// 构建「指令讲解生成器」子页：顶部源码输入 + 编译按钮，中间垂直 splitter
/// （上方字节码步骤表 + 下方讲解详情），底部加载样例按钮。
void StepExplainerPanel::buildExplainPage(QWidget* host) {
    auto* v = new QVBoxLayout(host);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);

    // 顶部：源码输入 + 编译按钮
    auto* topBar = new QHBoxLayout;
    topBar->addWidget(new QLabel(tr("源码：")));
    sourceEdit_ = new QLineEdit;
    sourceEdit_->setPlaceholderText(tr("输入 MiniLang 源码后点击编译"));
    compileBtn_ = new QPushButton(tr("编译并生成讲解"));
    topBar->addWidget(sourceEdit_, 1);
    topBar->addWidget(compileBtn_);
    v->addLayout(topBar);

    // 中间：垂直 splitter（步骤表 + 讲解详情）
    auto* splitter = new QSplitter(Qt::Vertical);
    stepTable_ = new QTableWidget(0, 4);
    stepTable_->setHorizontalHeaderLabels({tr("步骤"), tr("IP"), tr("OpCode"), tr("讲解摘要")});
    stepTable_->verticalHeader()->setVisible(false);
    stepTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    stepTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    stepTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    stepTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    stepTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    stepTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    stepTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    splitter->addWidget(stepTable_);

    stepDetail_ = new QTextBrowser;
    stepDetail_->setOpenExternalLinks(false);
    splitter->addWidget(stepDetail_);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    v->addWidget(splitter, 1);

    // 底部：加载样例按钮
    auto* btnBar = new QHBoxLayout;
    btnBar->addStretch();
    loadSampleBtn_ = new QPushButton(tr("加载样例代码到主编辑器"));
    btnBar->addWidget(loadSampleBtn_);
    v->addLayout(btnBar);

    connect(compileBtn_, &QPushButton::clicked, this, &StepExplainerPanel::onCompile);
    connect(stepTable_, &QTableWidget::currentCellChanged,
            [this](int, int, int, int) { onStepSelected(); });
    connect(loadSampleBtn_, &QPushButton::clicked, this, &StepExplainerPanel::onLoadSample);
}

/// 构建「OpCode 分类速查」子页：水平 splitter（左侧分类 OpCode 列表 + 右侧语义详情）。
void StepExplainerPanel::buildOpCodePage(QWidget* host) {
    auto* v = new QVBoxLayout(host);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);

    auto* splitter = new QSplitter(Qt::Horizontal);
    opCodeList_ = new QListWidget;
    opCodeDetail_ = new QTextBrowser;
    opCodeDetail_->setOpenExternalLinks(false);
    splitter->addWidget(opCodeList_);
    splitter->addWidget(opCodeDetail_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);
    v->addWidget(splitter, 1);

    connect(opCodeList_, &QListWidget::currentRowChanged, this, &StepExplainerPanel::onOpCodeSelected);
}

/// 编译源码：面板内独立完成 Lexer → Parser → Compiler，遍历 mainChunk 逐指令
/// 填充步骤表（步骤/IP/OpCode/讲解摘要）。编译失败时在详情区显示错误。
void StepExplainerPanel::onCompile() {
    steps_.clear();
    stepTable_->setRowCount(0);
    stepDetail_->clear();

    std::string source = sourceEdit_->text().toStdString();
    if (source.empty()) {
        stepDetail_->setHtml(QString::fromUtf8(
            "<div style='color:#C0392B; padding:8px;'><b>请输入源码</b></div>"));
        return;
    }

    // Lexer → Parser → Compiler
    Lexer lex;
    std::vector<Token> tokens;
    try {
        tokens = lex.scan(source);
    } catch (const std::exception& e) {
        stepDetail_->setHtml(QString::fromUtf8(
            "<div style='color:#C0392B; padding:8px;'><b>词法错误:</b> %1</div>")
            .arg(QString::fromUtf8(e.what()).toHtmlEscaped()));
        return;
    }
    if (lex.getDiagnostics().hasErrors()) {
        stepDetail_->setHtml(QString::fromUtf8(
            "<div style='color:#C0392B; padding:8px;'><b>词法错误:</b> %1</div>")
            .arg(formatDiagnosticErrors(lex.getDiagnostics())));
        return;
    }

    Parser parser;
    std::unique_ptr<Block> ast;
    try {
        ast = parser.parse(tokens);
    } catch (const std::exception& e) {
        stepDetail_->setHtml(QString::fromUtf8(
            "<div style='color:#C0392B; padding:8px;'><b>语法错误:</b> %1</div>")
            .arg(QString::fromUtf8(e.what()).toHtmlEscaped()));
        return;
    }
    if (parser.hasErrors()) {
        stepDetail_->setHtml(QString::fromUtf8(
            "<div style='color:#C0392B; padding:8px;'><b>语法错误:</b> %1</div>")
            .arg(formatDiagnosticErrors(parser.getDiagnostics())));
        return;
    }

    Compiler compiler;
    CompileResult result;
    try {
        result = compiler.compile(*ast);
    } catch (const std::exception& e) {
        stepDetail_->setHtml(QString::fromUtf8(
            "<div style='color:#C0392B; padding:8px;'><b>编译错误:</b> %1</div>")
            .arg(QString::fromUtf8(e.what()).toHtmlEscaped()));
        return;
    }
    if (compiler.getDiagnostics().hasErrors()) {
        stepDetail_->setHtml(QString::fromUtf8(
            "<div style='color:#C0392B; padding:8px;'><b>编译错误:</b> %1</div>")
            .arg(formatDiagnosticErrors(compiler.getDiagnostics())));
        return;
    }

    // 遍历 mainChunk 逐指令填充步骤表
    lastChunk_ = result.mainChunk;
    const auto& code = lastChunk_.code;
    size_t ip = 0;
    int step = 0;
    while (ip < code.size()) {
        OpCode op = static_cast<OpCode>(code[ip]);
        StepInfo info;
        info.step = step;
        info.ip = ip;
        info.op = op;
        info.summary = describeOperands(lastChunk_, ip, op);
        steps_.push_back(info);

        int row = stepTable_->rowCount();
        stepTable_->insertRow(row);
        auto* c0 = new QTableWidgetItem(QString::number(step));
        auto* c1 = new QTableWidgetItem(QString::number(ip));
        auto* c2 = new QTableWidgetItem(QString::fromUtf8(opCodeName(op)));
        auto* c3 = new QTableWidgetItem(info.summary);
        c0->setTextAlignment(Qt::AlignCenter);
        c1->setTextAlignment(Qt::AlignCenter);
        c2->setTextAlignment(Qt::AlignCenter);
        stepTable_->setItem(row, 0, c0);
        stepTable_->setItem(row, 1, c1);
        stepTable_->setItem(row, 2, c2);
        stepTable_->setItem(row, 3, c3);

        ip += lastChunk_.instructionSizeAt(ip);
        ++step;
    }

    if (!steps_.empty()) {
        stepTable_->selectRow(0);
    } else {
        stepDetail_->setHtml(QString::fromUtf8(
            "<div style='color:#6E6E6E; padding:8px;'><i>（编译产物为空）</i></div>"));
    }
}

/// 选中步骤表某行时，生成该指令的详细讲解并显示在下方详情区。
void StepExplainerPanel::onStepSelected() {
    int row = stepTable_->currentRow();
    if (row < 0 || row >= static_cast<int>(steps_.size())) {
        return;
    }
    const auto& info = steps_[row];
    QString html = StepExplainerLibrary::generateStepExplanation(lastChunk_, info.ip, info.op);
    stepDetail_->setHtml(html);
}

/// 用 OpCode 文档名填充左侧列表并默认选中首项。
void StepExplainerPanel::populateOpCodes() {
    opCodeList_->clear();
    for (const auto& doc : StepExplainerLibrary::opCodeDocs()) {
        opCodeList_->addItem(QString::fromUtf8(doc.name.c_str()));
    }
    if (opCodeList_->count() > 0) {
        opCodeList_->setCurrentRow(0);
    }
}

/// OpCode 列表选中项变化时委托 showOpCode 渲染该 OpCode 的文档详情。
void StepExplainerPanel::onOpCodeSelected(int index) {
    showOpCode(index);
}

/// 渲染 OpCode 文档：标题、分类、操作数格式、栈效果、语义与样例代码（HTML 转义）。
void StepExplainerPanel::showOpCode(int index) {
    currentOpCodeIdx_ = index;
    if (index < 0 || index >= static_cast<int>(StepExplainerLibrary::opCodeDocs().size())) {
        opCodeDetail_->clear();
        return;
    }
    const auto& d = StepExplainerLibrary::opCodeDocs()[index];
    QString html = QString("<h2>%1</h2>"
                           "<p><b>分类:</b> %2</p>"
                           "<h3>操作数格式</h3>"
                           "<p><code>%3</code></p>"
                           "<h3>栈效果</h3>"
                           "<p><code>%4</code></p>"
                           "<h3>语义讲解</h3>"
                           "<p>%5</p>"
                           "<h3>样例代码</h3>"
                           "<pre>%6</pre>")
                       .arg(QString::fromUtf8(d.name.c_str()).toHtmlEscaped())
                       .arg(QString::fromUtf8(d.category.c_str()).toHtmlEscaped())
                       .arg(QString::fromUtf8(d.operandFormat.c_str()).toHtmlEscaped())
                       .arg(QString::fromUtf8(d.stackEffect.c_str()).toHtmlEscaped())
                       .arg(QString::fromUtf8(d.semantics.c_str()).toHtmlEscaped())
                       .arg(QString::fromUtf8(d.exampleCode.c_str()).toHtmlEscaped());
    opCodeDetail_->setHtml(html);
}

/// 将当前源码输入框的内容 emit loadSampleRequested，加载到主编辑器运行观察。
void StepExplainerPanel::onLoadSample() {
    emit loadSampleRequested(sourceEdit_->text());
}
