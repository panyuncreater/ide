// ============================================================
// BytecodeTracePanel.cpp — 字节码执行轨迹面板实现（第三波 P0-3）
// ============================================================

#include "gui/BytecodeTracePanel.h"
#include "app/IdeController.h"
#include "gui/GuidedTour.h"
#include "gui/MarkdownRenderer.h"
#include "interpreter/Value.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QSplitter>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>
#include <sstream>

#include "Label.h" // QFluentKit（CaptionLabel）

// ============================================================
// BytecodeTraceLibrary — 静态 OpCode 教学库
// ============================================================

/// 返回 OpCode 教学库（静态单例）：75 条核心字节码（常量/算术/变量/控制/
/// 调用/容器/闭包/类），每条含分类、操作数格式、栈效果、语义与样例代码，
/// 供「OpCode 教学库」子页展示并支持加载样例到主编辑器。
/// 需求 8 扩充：新增 29 条（元组/枚举/协程/finally 续跳/类型检查/尾调用/
/// 特化算术/写回族等），与 compiler/Bytecode.h 枚举注释保持语义一致。
const std::vector<OpCodeDocEntry>& BytecodeTraceLibrary::opCodeDocs() {
    static const std::vector<OpCodeDocEntry> kDocs = {
        OpCodeDocEntry{"OP_INT", "const", "nameIdx(2B)", "push 1", "📜 从常量池读取整数并压入栈顶。", "var x = 42;"},
        OpCodeDocEntry{"OP_FLOAT", "const", "nameIdx(2B)", "push 1", "📜 从常量池读取浮点数并压入栈顶。",
                       "var pi = 3.14;"},
        OpCodeDocEntry{"OP_STRING", "const", "nameIdx(2B)", "push 1", "📜 从常量池读取字符串并压入栈顶。",
                       "var s = \"hello\";"},
        OpCodeDocEntry{"OP_NULL", "const", "无", "push 1", "📜 压入 null 值。", "var n = null;"},
        OpCodeDocEntry{"OP_ADD", "arith", "无", "pop 2 / push 1",
                       "🔢 弹出栈顶两个值（右、左），相加后压入结果。注意：左操作数在栈深处，右操作数在栈顶。",
                       "var z = x + y;"},
        OpCodeDocEntry{"OP_GET_GLOBAL", "var", "slot(2B)", "push 1",
                       "📍 读取全局槽位的值并压栈。slot 在编译期由 GlobalSlotAllocator 分配。", "print(x);"},
        OpCodeDocEntry{"OP_SET_GLOBAL", "var", "slot(2B)", "pop 1", "📍 弹出栈顶值写入全局槽位。", "x = 10;"},
        OpCodeDocEntry{"OP_JUMP", "control", "offset(2B)", "no effect", "🔄 无条件跳转到 offset 指定的相对位置。",
                       "if (true) { print(\"yes\"); }"},
        OpCodeDocEntry{"OP_JUMP_IF_FALSE", "control", "offset(2B)", "pop 1", "🔄 弹出栈顶条件，若为 false 则跳转。",
                       "if (cond) { ... }"},
        OpCodeDocEntry{"OP_LOOP", "control", "offset(2B)", "no effect", "🔄 回跳到循环入口（负偏移）。",
                       "while (cond) { ... }"},
        OpCodeDocEntry{"OP_CALL", "call", "argCount(1B)", "pop N+1 / push 1",
                       "📞 调用栈顶闭包：弹出 N 个参数 + 1 个闭包值，执行后压入返回值。", "result = foo(1, 2);"},
        OpCodeDocEntry{"OP_RETURN", "call", "无", "pop frame",
                       "📞 从当前函数返回，弹出整个调用帧，将返回值压入调用者栈顶。", "return x;"},
        OpCodeDocEntry{"OP_BUILD_ARRAY", "container", "count(1B)", "pop N / push 1",
                       "📊 弹出栈顶 N 个元素构建 ArrayData 并压入。", "var arr = [1, 2, 3];"},
        OpCodeDocEntry{"OP_BUILD_DICT", "container", "pairCount(1B)", "pop 2N / push 1",
                       "📊 弹出栈顶 2N 个值（key+value 对）构建 DictData 并压入。", "var d = {\"x\": 1};"},
        OpCodeDocEntry{
            "OP_CLOSURE", "closure", "nameIdx(2B) + upvalueCount(1B)", "pop N / push 1",
            "📦 创建闭包值：从栈顶弹出 N 个 upvalue（每个为 isLocal+index 编码）+ 函数名，构造 ClosureData。",
            "fun outer() { var x = 1; fun inner() { return x; } return inner; }"},
        OpCodeDocEntry{"OP_GET_UPVALUE", "closure", "upvalueIndex(1B)", "push 1",
                       "🔗 读取闭包捕获的外层变量。若 upvalue 仍开放则从栈帧读取，已关闭则从堆读取。",
                       "// 闭包内访问外层变量"},
        OpCodeDocEntry{"OP_CLASS_NEW", "class", "nameIdx(2B) + argCount(1B)", "pop N+1 / push 1",
                       "📦 类构造：弹出 N 个参数 + 类模板，创建 InstanceData 并调用 init 方法。",
                       "var p = Point(3, 4);"},
        OpCodeDocEntry{"OP_METHOD_CALL", "class", "nameIdx(2B) + argCount(1B) + recvVarIdx(2B)", "pop N+1 / push 1",
                       "📞 方法调用：通过 methodCache_ 查找方法，避免重复 ClassInfo 遍历。", "p.distance();"},
        // ---- Arithmetic ----
        OpCodeDocEntry{"OP_SUBTRACT", "arith", "无", "pop 2 / push 1",
                       "🔢 弹出栈顶两个值（右、左），相减后压入结果（左 - 右）。", "var z = x - y;"},
        OpCodeDocEntry{"OP_MULTIPLY", "arith", "无", "pop 2 / push 1", "🔢 弹出栈顶两个值相乘后压入结果。",
                       "var z = x * y;"},
        OpCodeDocEntry{"OP_DIVIDE", "arith", "无", "pop 2 / push 1",
                       "🔢 弹出栈顶两个值相除后压入结果（整数除法截断向零，三后端统一）。", "var z = x / y;"},
        OpCodeDocEntry{"OP_MODULO", "arith", "无", "pop 2 / push 1", "🔢 弹出栈顶两个值取模后压入结果。",
                       "var z = x % y;"},
        OpCodeDocEntry{"OP_NEGATE", "arith", "无", "pop 1 / push 1", "🔢 弹出栈顶值取负后压入。", "var z = -x;"},
        // ---- Boolean ----
        OpCodeDocEntry{"OP_TRUE", "const", "无", "push 1", "🟢 压入布尔常量 true。", "var b = true;"},
        OpCodeDocEntry{"OP_FALSE", "const", "无", "push 1", "🔴 压入布尔常量 false。", "var b = false;"},
        OpCodeDocEntry{"OP_NOT", "arith", "无", "pop 1 / push 1", "🔵 弹出栈顶值取逻辑非后压入布尔结果。",
                       "var b = !x;"},
        // ---- Comparison ----
        OpCodeDocEntry{"OP_EQUAL", "arith", "无", "pop 2 / push 1", "⚖️ 弹出栈顶两个值进行相等比较后压入布尔结果。",
                       "var b = x == y;"},
        OpCodeDocEntry{"OP_NOT_EQUAL", "arith", "无", "pop 2 / push 1", "⚖️ 弹出栈顶两个值进行不等比较后压入布尔结果。",
                       "var b = x != y;"},
        OpCodeDocEntry{"OP_LESS", "arith", "无", "pop 2 / push 1", "⚖️ 弹出栈顶两个值（右、左），若左 < 右则压入 true。",
                       "var b = x < y;"},
        OpCodeDocEntry{"OP_GREATER", "arith", "无", "pop 2 / push 1",
                       "⚖️ 弹出栈顶两个值（右、左），若左 > 右则压入 true。", "var b = x > y;"},
        OpCodeDocEntry{"OP_LESS_EQUAL", "arith", "无", "pop 2 / push 1",
                       "⚖️ 弹出栈顶两个值（右、左），若左 <= 右则压入 true。", "var b = x <= y;"},
        OpCodeDocEntry{"OP_GREATER_EQUAL", "arith", "无", "pop 2 / push 1",
                       "⚖️ 弹出栈顶两个值（右、左），若左 >= 右则压入 true。", "var b = x >= y;"},
        // ---- Stack / IO ----
        OpCodeDocEntry{"OP_POP", "control", "无", "pop 1",
                       "📤 弹出栈顶值并丢弃（表达式语句副作用求值后丢弃返回值，防止栈泄漏）。", "foo();"},
        OpCodeDocEntry{"OP_PRINT", "call", "无", "pop 1", "🖨️ 弹出栈顶值并输出到标准输出（带换行）。", "print(x);"},
        // ---- Local / var ----
        OpCodeDocEntry{"OP_GET_LOCAL", "var", "slot(1B)", "push 1",
                       "📍 读取当前栈帧中局部变量槽位并压栈（编译期分配的栈相对位置）。", "// 函数内访问局部变量"},
        OpCodeDocEntry{"OP_SET_LOCAL", "var", "slot(1B)", "pop 1", "📍 弹出栈顶值写入当前栈帧的局部变量槽位。",
                       "x = 10;"},
        OpCodeDocEntry{"OP_DEFINE_GLOBAL", "var", "nameIdx(2B)", "pop 1",
                       "📍 弹出栈顶值，以 name 为键首次写入全局表（与 OP_SET_GLOBAL 的已存在赋值语义区分）。",
                       "var x = 10;"},
        // ---- Index / member ----
        OpCodeDocEntry{"OP_INDEX_GET", "container", "无", "pop 2 / push 1",
                       "🔑 弹出索引与容器，读取 container[index] 并压栈（支持数组/字典/字符串）。", "var v = arr[0];"},
        OpCodeDocEntry{"OP_INDEX_SET", "container", "无", "pop 3",
                       "🔑 弹出值、索引、容器，执行 container[index] = value（COW 写时拷贝）。", "arr[0] = 99;"},
        OpCodeDocEntry{"OP_MEMBER_GET", "container", "nameIdx(2B)", "pop 1 / push 1",
                       "🔑 弹出实例，读取字段 name 的值并压栈（通过 fieldSlotIndex 查表）。", "var v = p.x;"},
        OpCodeDocEntry{"OP_MEMBER_SET", "container", "nameIdx(2B)", "pop 2",
                       "🔑 弹出值与实例，写入字段 name（实例字段槽位赋值）。", "p.x = 10;"},
        // ---- Closure ----
        OpCodeDocEntry{"OP_SET_UPVALUE", "closure", "upvalueIndex(1B)", "pop 1",
                       "🔗 弹出栈顶值写入指定 upvalue（开放则写栈帧，已关闭则写堆）。", "// 闭包内赋值外层变量"},
        OpCodeDocEntry{"OP_CLOSE_UPVALUE", "closure", "无", "pop 1",
                       "🔗 弹出栈顶值，将仍开放的 upvalue 迁移到堆（变量逃逸出作用域时触发）。",
                       "// 闭包捕获的变量逃逸"},
        // ---- Exception ----
        OpCodeDocEntry{"OP_THROW", "control", "无", "pop 1",
                       "⚠️ 弹出栈顶值作为异常对象并抛出，沿调用栈寻找匹配的 catch。", "throw \"error\";"},
        OpCodeDocEntry{"OP_TRY_BEGIN", "control", "catchOffset(2B)", "no effect",
                       "🛡️ 注册 try 块的 catch 处理偏移，异常抛出时跳转到该处执行。", "try { ... } catch (e) { ... }"},
        // ---- Class ----
        OpCodeDocEntry{"OP_SUPER_CALL", "class", "nameIdx(2B) + argCount(1B)", "pop N+1 / push 1",
                       "📞 调用父类方法：从当前实例的父类链查找方法并调用（super 语义三后端统一）。", "super.foo();"},
        // ---- 需求 8 扩充：栈操作 ----
        OpCodeDocEntry{"OP_DUP", "control", "无", "push 1",
                       "📤 复制栈顶值并压入（栈顶出现两份相同值）。用于需要保留原值的复合赋值等场景。",
                       "arr[i] = arr[i] + 1;"},
        OpCodeDocEntry{"OP_DUP_N", "control", "depth(1B)", "push 1",
                       "📤 复制栈中第 depth 个值到栈顶（不弹出原值）。用于索引写回时保留索引值。", "m[k] = m[k] + 1;"},
        OpCodeDocEntry{"OP_SWAP", "control", "无", "swap top 2",
                       "🔀 交换栈顶两个值。match 表达式 case body 完成后将 [scrutinee, 结果] 变为 "
                       "[结果, scrutinee]，随后 OP_POP 弹出 scrutinee 留下结果。",
                       "var r = match (x) { case 1 => 100 case v => v };"},
        // ---- 需求 8 扩充：调用变体 ----
        OpCodeDocEntry{"OP_CALL_EXPR", "call", "argCount(1B)", "pop N+1 / push 1",
                       "📞 表达式调用：闭包在参数下方（编译器先 push 闭包再 push 参数）。用于调用不经变量名的"
                       "闭包表达式（数组元素/返回值/IIFE）。",
                       "fns[0](1, 2);"},
        OpCodeDocEntry{"OP_TAIL_CALL", "call", "nameIdx(2B) + argCount(1B)", "reuse frame / push 1",
                       "📞 尾调用：return g(args) 且目标为普通函数时复用当前帧（TCO，跳过后随 OP_RETURN），"
                       "不支持场景降级为 OP_CALL 语义。互递归不再爆栈。",
                       "fun even(n) { if (n == 0) return true; return odd(n - 1); }"},
        // ---- 需求 8 扩充：容器/元组/枚举 ----
        OpCodeDocEntry{"OP_BUILD_TUPLE", "container", "count(1B)", "pop N / push 1",
                       "📊 弹出栈顶 N 个元素构建不可变元组 TupleData 并压入（R98）。元组支持索引读与解构，"
                       "但禁止元素赋值。",
                       "var t = (1, \"a\", true);"},
        OpCodeDocEntry{"OP_LEN", "container", "无", "pop 1 / push 1",
                       "📊 弹出栈顶容器（array/dict/string/tuple），压入其长度（int）。用于 TUPLE 模式的"
                       "元素数检查（R134）。",
                       "var t = (10, 20); var (a, b) = t;"},
        OpCodeDocEntry{"OP_BUILD_ENUM_VARIANT", "container", "enumIdx(2B) + variantIdx(2B) + argCount(1B)",
                       "pop N / push 1",
                       "📊 弹出 N 个字段参数构造 EnumVariantData(enumName, variantName, fields) 并压入（R99 "
                       "ADT）。字段数与类型在编译期校验。",
                       "enum Color { Red(int) } var c = Color.Red(255);"},
        OpCodeDocEntry{"OP_ENUM_VARIANT_NAME", "container", "enumIdx(2B) + variantIdx(2B)", "pop 1 / push 1",
                       "📊 弹出 scrutinee，检查是否为指定 enum/variant，压入 bool。match 分支的 variant "
                       "模式匹配就靠它。",
                       "var r = match (c) { case Color.Red(v) => v default => 0 };"},
        OpCodeDocEntry{"OP_ENUM_VARIANT_FIELD", "container", "无", "pop 2 / push 1",
                       "📊 弹出 index 与 scrutinee，压入 scrutinee.fields[index]。用于 match 分支中提取 "
                       "variant 字段绑定到模式变量。",
                       "var r = match (p) { case Pair.P(a, b) => a + b };"},
        // ---- 需求 8 扩充：异常/finally 续跳 ----
        OpCodeDocEntry{"OP_TRY_END", "control", "无", "pop handler",
                       "🛡️ try 块正常结束，弹出 try 处理器（与 OP_TRY_BEGIN 配对）。此后抛出的异常不再被"
                       "该 catch 捕获。",
                       "try { safe(); } catch (e) { print(e); }"},
        OpCodeDocEntry{"OP_PUSH_JUMP_TARGET", "control", "target(2B)", "push target(pendingJumpStack)",
                       "🛡️ break/continue 在 try-finally 内时，先把真实跳转目标压入 pendingJumpStack_，"
                       "再跳到 finally 入口——实现「先执行 finally 再跳转」（三后端一致）。",
                       "while (x) { try { break; } finally { cleanup(); } }"},
        OpCodeDocEntry{"OP_FINALLY_END", "control", "无", "pop target / jump",
                       "🛡️ finally 块末尾：从 pendingJumpStack_ 弹出目标并跳转；栈空则顺序继续执行。"
                       "与 OP_PUSH_JUMP_TARGET 配对完成 break/continue 续跳。",
                       "try { work(); } finally { cleanup(); }"},
        // ---- 需求 8 扩充：类型检查 ----
        OpCodeDocEntry{"OP_TYPE_CHECK", "var", "typeIdx(2B)", "peek (no pop)",
                       "⚖️ peek 栈顶值，检查是否兼容常量池中的类型注解字符串，不匹配则 runtimeError。"
                       "不弹栈。类型注解强制三后端统一。",
                       "var x: int = 42;"},
        OpCodeDocEntry{"OP_TYPE_TEST", "control", "typeIdx(2B)", "pop 1 / push 1",
                       "⚖️ 软类型测试：弹出栈顶值做 typeMatch 检查（含实例继承链），压入 bool。与 OP_TYPE_CHECK "
                       "区别：不抛错，用于 TUPLE 模式 scrutinee 非元组时 fall through 到下一分支（R134）。",
                       "var r = match (x) { case (a, b) => a + b default => -1 };"},
        // ---- 需求 8 扩充：协程/异步 ----
        OpCodeDocEntry{"OP_YIELD", "control", "无", "pop 1 (或 push 回)",
                       "🔄 弹出 yield 值并递增 yield 计数器：命中重放目标则抛 YieldSignal 被 .next() 捕获；"
                       "未命中则 push 回栈继续执行。VM 与 Interpreter 同用重放模式，四后端语义一致（R164）。",
                       "fun* gen() { yield 1; yield 2; }"},
        OpCodeDocEntry{"OP_AWAIT", "control", "无", "pop 1 / push 1",
                       "🔄 弹出栈顶值 v：非协程则恒等 push v；协程则循环驱动 next() 直到 done，push 最终值"
                       "（同步 drain 模型，与 Interpreter 对齐）。仅 async fun 体内合法。",
                       "async fun main() { var r = await compute(); }"},
        // ---- 需求 8 扩充：类补全 ----
        OpCodeDocEntry{"OP_INIT_FIELD", "class", "fieldNameIdx(2B)", "pop 1",
                       "📦 从栈顶弹出值写入实例字段（类体字段默认值初始化）。继承时父类字段默认值也经"
                       "此指令初始化，避免字段丢失。",
                       "class P { var x = 0; }"},
        OpCodeDocEntry{"OP_DEFINE_CLASS", "class", "nameIdx(2B)", "pop 1",
                       "📦 从栈顶弹出模板实例，提取 ClassInfo（字段表/方法表/父类链）并注册到 VM。"
                       "后续 OP_CLASS_NEW 构造时查此注册表。",
                       "class Point { var x = 0; var y = 0; }"},
        OpCodeDocEntry{"OP_SUPER_MEMBER_GET", "class", "nameIdx(2B)", "pop 1 / push 1",
                       "📦 从父类开始查找成员（跳过子类同名覆盖）并压入。与 OP_MEMBER_GET 区别在查找"
                       "起点（super 语义）。",
                       "var m = super.name;"},
        // ---- 需求 8 扩充：类型特化算术（PERF）----
        OpCodeDocEntry{"OP_ADD_INT_SPEC", "arith", "无", "pop 2 / push 1",
                       "⚡ int + int 特化加法：编译期静态确定两操作数均为 int 时生成，跳过运行时类型"
                       "检查，消除热循环分支预测开销。",
                       "var i = 0; while (i < 1000) { i = i + 1; }"},
        OpCodeDocEntry{"OP_SUB_INT_SPEC", "arith", "无", "pop 2 / push 1",
                       "⚡ int - int 特化减法（无类型检查）。与 OP_ADD_INT_SPEC 同族，编译期类型推断"
                       "驱动的热路径优化。",
                       "var n = m - 1;"},
        OpCodeDocEntry{"OP_MUL_INT_SPEC", "arith", "无", "pop 2 / push 1", "⚡ int * int 特化乘法（无类型检查）。",
                       "var sq = x * x;"},
        OpCodeDocEntry{"OP_LT_INT_SPEC", "arith", "无", "pop 2 / push 1",
                       "⚡ int < int 特化比较（最常见的循环条件），压入 bool。配合 OP_JUMP_IF_FALSE "
                       "构成热循环骨架。",
                       "while (i < n) { ... }"},
        // ---- 需求 8 扩充：变量/写回族 ----
        OpCodeDocEntry{"OP_DELETE_GLOBAL", "var", "slot(2B)", "no effect",
                       "📍 删除全局槽位（块作用域退出时清理块内 var）。避免块内变量泄漏到块外可见。",
                       "{ var tmp = 1; } // 块退出后 tmp 不可见"},
        OpCodeDocEntry{"OP_INDEX_SET_LOCAL", "container", "slot(1B)", "pop 2",
                       "🔑 弹出值与索引，直接修改 stack_[bp+slot] 指向的容器（免去 push 容器 + 写回的"
                       "三步舞）。OP_INDEX_SET 的局部变量直写优化变体。",
                       "fun f() { var a = [1, 2]; a[0] = 9; }"},
        OpCodeDocEntry{"OP_MEMBER_SET_VAR", "container", "nameIdx(2B) + fieldNameIdx(2B)", "pop 1",
                       "🔑 弹出值，直接修改 globals_[varName].fields[field]（全局变量成员直写优化，"
                       "避免 COW 容器副本写丢失）。",
                       "p.x = 10;"},
        OpCodeDocEntry{"OP_LOAD_MUTATED", "container", "无", "push 1",
                       "🔑 压入 lastMutatedReceiver_（变异方法调用后的接收者副本，不清除），供嵌套左值"
                       "写回链使用。COW 容器变异后的写回机制核心。",
                       "obj.list.push(1);"},
        OpCodeDocEntry{"OP_WRITEBACK_MEMBER_VAR", "container", "varIdx(2B) + fieldIdx(2B)", "no effect",
                       "🔑 将 lastMutatedReceiver_ 写回全局变量的指定字段。嵌套访问 a.b.push(x) 变异后，"
                       "把变异后的 b 写回 a.b（COW 副本才能对外可见，写回族共 6 条覆盖 var/local/upvalue × "
                       "member/index）。",
                       "obj.items.push(42);"},
    };
    return kDocs;
}

// ============================================================
// BytecodeTracePanel 实现
// ============================================================

/// 构造面板：组装顶部子页切换（执行轨迹 / OpCode 教学库）与 QStackedWidget 子页堆栈，
/// 构建两个子页，配置 2s 自动捕获定时器（安全网，即时刷新由监听器触发），
/// 并填充教学库列表。
BytecodeTracePanel::BytecodeTracePanel(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);
    outer->setSpacing(4);

    // 子页切换（UX-R fix: 统一 TeachingSubPageBar 组件；互斥选中态/主题色高亮/
    // 滑入动画由组件内置，addPage 同时创建按钮并入栈，首个自动选中）
    subPageBar_ = new TeachingSubPageBar(this);
    outer->addLayout(subPageBar_->buttonBar());

    auto* tracePage = new QWidget;
    auto* libraryPage = new QWidget;
    buildTracePage(tracePage);
    buildLibraryPage(libraryPage);
    subPageBar_->addPage(tr("① 执行轨迹"), tracePage);
    subPageBar_->addPage(tr("② OpCode 教学库"), libraryPage);
    outer->addWidget(subPageBar_->stack(), 1);

    // OPT-1: 自动捕获改为 vmStateChanged 监听器即时触发（见 setController）。
    // QTimer 降级为 2000ms 安全网，覆盖监听器未触达的边角场景。
    autoTimer_ = new QTimer(this);
    autoTimer_->setInterval(2000);
    connect(autoTimer_, &QTimer::timeout, this, &BytecodeTracePanel::onCaptureNow);

    populateDocs();
}

/// 绑定/换绑 IdeController：注册 vmStateChanged 监听器（owner=this），
/// 换绑前先反注册旧监听器，避免 controller 持有悬垂回调。
void BytecodeTracePanel::setController(IdeController* controller) {
    if (controller_ == controller)
        return;
    // AUDIT-P0 fix: 注册前若已有 controller，先反注册旧监听器避免悬垂。
    if (controller_) {
        controller_->removeVmStateChangedListener(this);
    }
    controller_ = controller;
    if (controller_) {
        // AUDIT-P0 fix: owner=this 供析构/换绑时反注册，避免悬垂 lambda UAF。
        controller_->addVmStateChangedListener(this, [this] { onVmStateChanged(); });
    }
}

// AUDIT-P0 fix: 析构时反注册监听器，避免 controller_ 持有悬垂 this 回调。
/// 析构时反注册 vmStateChanged 监听器，避免 controller 持有悬垂 this 回调。
BytecodeTracePanel::~BytecodeTracePanel() {
    if (controller_) {
        controller_->removeVmStateChangedListener(this);
    }
}

/// 构建「执行轨迹」子页：顶部状态/立即捕获/自动捕获/清空按钮 + 垂直 splitter
/// （上方轨迹表 + 下方栈快照浏览器）。轨迹表列：步/IP/OpCode/帧数/栈大小。
void BytecodeTracePanel::buildTracePage(QWidget* host) {
    auto* v = new QVBoxLayout(host);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);

    auto* bar = new QHBoxLayout;
    liveStatusLabel_ = new CaptionLabel(tr("状态：未初始化"));
    captureBtn_ = new QPushButton(tr("立即捕获"));
    autoCaptureCheck_ = new QCheckBox(tr("自动捕获（VM 暂停时）"));
    clearBtn_ = new QPushButton(tr("清空"));
    bar->addWidget(liveStatusLabel_);
    bar->addStretch();
    bar->addWidget(autoCaptureCheck_);
    bar->addWidget(captureBtn_);
    bar->addWidget(clearBtn_);
    v->addLayout(bar);

    auto* splitter = new QSplitter(Qt::Vertical);
    traceTable_ = new QTableWidget(0, 5);
    traceTable_->setHorizontalHeaderLabels({tr("步"), tr("IP"), tr("OpCode"), tr("帧数"), tr("栈大小")});
    traceTable_->verticalHeader()->setVisible(false);
    traceTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    traceTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    traceTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    traceTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    traceTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    traceTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    traceTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    traceTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    splitter->addWidget(traceTable_);

    stackDetail_ = new QTextBrowser;
    stackDetail_->setOpenExternalLinks(false);
    splitter->addWidget(stackDetail_);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    v->addWidget(splitter, 1);

    connect(captureBtn_, &QPushButton::clicked, this, &BytecodeTracePanel::onCaptureNow);
    connect(autoCaptureCheck_, &QCheckBox::toggled, this, &BytecodeTracePanel::onAutoCaptureToggled);
    connect(clearBtn_, &QPushButton::clicked, this, &BytecodeTracePanel::onClearTrace);
    connect(traceTable_, &QTableWidget::currentCellChanged, [this](int, int, int, int) { onTraceRowSelected(); });
}

/// 构建「OpCode 教学库」子页：左侧 OpCode 列表 + 右侧说明浏览器 + 加载样例按钮，
/// 选中项变化时通过 onDocSelected 渲染指令语义详情。
void BytecodeTracePanel::buildLibraryPage(QWidget* host) {
    auto* v = new QVBoxLayout(host);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);

    auto* splitter = new QSplitter(Qt::Horizontal);
    docList_ = new QListWidget;
    docDetail_ = new QTextBrowser;
    docDetail_->setOpenExternalLinks(false);
    splitter->addWidget(docList_);
    splitter->addWidget(docDetail_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);
    v->addWidget(splitter, 1);

    auto* btnBar = new QHBoxLayout;
    loadCodeBtn_ = new QPushButton(tr("加载样例代码到主编辑器"));
    btnBar->addStretch();
    btnBar->addWidget(loadCodeBtn_);
    v->addLayout(btnBar);

    connect(docList_, &QListWidget::currentRowChanged, this, &BytecodeTracePanel::onDocSelected);
    connect(loadCodeBtn_, &QPushButton::clicked, this, &BytecodeTracePanel::onLoadDocCode);
}

/// 立即捕获按钮回调：委托 captureCurrentState() 抓取当前 VM 单步状态。
void BytecodeTracePanel::onCaptureNow() {
    captureCurrentState();
}

/// 自动捕获复选框切换：勾选启动 2s 定时器，取消则停止（即时刷新仍由 vmStateChanged 触发）。
void BytecodeTracePanel::onAutoCaptureToggled(bool checked) {
    if (checked)
        autoTimer_->start();
    else
        autoTimer_->stop();
}

/// 面板重新可见时：若已勾选自动捕获则恢复 2s 定时器。
void BytecodeTracePanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (autoCaptureCheck_ && autoCaptureCheck_->isChecked() && autoTimer_ && !autoTimer_->isActive()) {
        autoTimer_->start();
    }
}

/// 面板隐藏时停止自动捕获定时器，避免后台空转。
void BytecodeTracePanel::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    if (autoTimer_ && autoTimer_->isActive()) {
        autoTimer_->stop();
    }
}

/// 清空轨迹：重置步计数器、清空历史向量与轨迹表/栈快照浏览器，状态回未运行。
void BytecodeTracePanel::onClearTrace() {
    traceHistory_.clear();
    stepCounter_ = 0;
    // OPT-2 fix: 清空时同步重置指纹，使下一次捕获不会被误判为"状态未变"而跳过。
    lastCaptureFingerprint_.clear();
    // R51-2 fix: 同步重置滑动标志，避免下次 capture 误触发全量重建
    dequeShifted_ = false;
    traceTable_->setRowCount(0);
    stackDetail_->clear();
    // R54-14 fix: 空状态占位提示
    stackDetail_->setHtml(
        QString::fromUtf8("<div style='color:#6E6E6E; padding:8px;'><i>（轨迹已清空，捕获后将显示栈快照）</i></div>"));
    liveStatusLabel_->setText(tr("状态：未运行（轨迹已清空）"));
}

/// 抓取一次 VM 单步快照（轨迹单步/断点的数据来源）：
/// 从未初始化的 VM 直接返回占位；否则从 controller 取当前 IP、OpCode 名、
/// 栈帧数、源码行号，并通过 getVmStack() 取得操作数栈快照（每个 Value 经
/// toString 序列化，堆类型用 try/catch 包裹防异常）。快照追加到 traceHistory_
/// （超 kMaxTraceEntries 则丢弃最旧），刷新表格并自动选中最新行。
/// 该轨迹即字节码单步/断点的可视化数据源，每行对应一条指令执行后的完整状态。
void BytecodeTracePanel::captureCurrentState() {
    if (!controller_) {
        liveStatusLabel_->setText(tr("状态：未绑定 controller"));
        return;
    }
    if (!controller_->isVmInitialized()) {
        liveStatusLabel_->setText(tr("状态：VM 未初始化（启动 VM 单步以捕获轨迹）"));
        return;
    }
    // AUDIT-P2 fix: 对齐 CallStackPanel/VariableInspectorPanel/BreakpointConditionPanel/
    // MemoryModelPanel 的 isVmRunning() 守卫。VM RUN 批之间捕获轨迹会混入用户
    // 未主动请求的中间状态数据，且未来若 VmStepper 改为多线程会升级为数据竞争。
    if (controller_->isVmRunning()) {
        liveStatusLabel_->setText(tr("状态：VM 运行中（暂停后可捕获）"));
        return;
    }

    TraceEntry e;
    // OPT-2 fix: step 延迟到指纹检查通过后再赋值，避免状态未变时 stepCounter_
    // 误增导致后续捕获步号跳号（autoTimer 2s 安全网可能在 VM 状态未变时触发）。
    e.ip = controller_->getVmCurrentIP();
    e.opCodeName = controller_->getVmCurrentOpCodeName();
    e.frameCount = static_cast<int>(controller_->getVmFrameCount());
    e.line = controller_->getVmCurrentLine();

    auto stack = controller_->getVmStack();
    e.stackSnapshot.reserve(stack.size());
    // AUDIT-P2 fix: Value::toString 对堆类型（数组/字典/实例）走 toStringImpl，
    // 极端情况（循环引用、深度嵌套、bad_alloc）可能抛异常。对齐 CallStackPanel/
    // VariableInspectorPanel 的 try/catch 防护，避免异常传播到 QTimer 槽。
    for (const auto& v : stack) {
        std::string s;
        try {
            s = v.toString();
        } catch (...) {
            s = "<error>";
        }
        e.stackSnapshot.push_back(std::move(s));
    }

    // OPT-2 fix: autoTimer 2s 安全网无指纹对比会盲目累积重复轨迹（累积式 push_back）。
    // 在 e 各字段已填充后计算指纹，与上次比对——状态未变则跳过本次捕获。
    // 指纹不含 stackSnapshot 内容（避免长栈 O(n) 字符串拼接），仅用大小即可
    // 区分绝大多数"状态未变"场景；如栈大小相同但内容变化，会多捕获一条，
    // 不影响正确性，仅极小性能开销。
    std::string fingerprint = std::to_string(e.ip) + "|" + e.opCodeName + "|" + std::to_string(e.frameCount) + "|" +
                              std::to_string(e.stackSnapshot.size());
    if (fingerprint == lastCaptureFingerprint_) {
        // 状态未变，跳过本次捕获（不更新 stepCounter_，不追加历史，不刷新表格）
        return;
    }
    lastCaptureFingerprint_ = std::move(fingerprint);

    // 指纹检查通过，确认本次捕获有效，递增步号
    e.step = ++stepCounter_;

    liveStatusLabel_->setText(tr("状态：已捕获 step=%1 | IP=%2 | OpCode=%3 | 帧数=%4")
                                  .arg(e.step)
                                  .arg(e.ip)
                                  .arg(QString::fromUtf8(e.opCodeName.c_str()))
                                  .arg(e.frameCount));

    if (static_cast<int>(traceHistory_.size()) >= kMaxTraceEntries) {
        // AUDIT-P1 fix: deque pop_front O(1)（原 vector erase(begin()) O(n) 移位）
        traceHistory_.pop_front();
        // R51-2 fix: 标记 deque 已滑动，refreshTraceTable 需全量重建
        dequeShifted_ = true;
    }
    traceHistory_.push_back(std::move(e));
    refreshTraceTable();

    // 自动选中最新条目
    int lastRow = traceTable_->rowCount() - 1;
    if (lastRow >= 0) {
        traceTable_->selectRow(lastRow);
        onTraceRowSelected();
    }
}

/// 将 traceHistory_ 渲染为轨迹表。AUDIT-P1 fix: 改为增量更新——
/// 仅追加新行 + 移除溢出行，避免每次 capture 都 setRowCount(0) 全量重建 N×5 个 item。
/// R51-2 fix: deque 容量满后 pop_front+push_back 保持 size 不变但索引偏移，
/// 增量更新无法感知，需检测 dequeShifted_ 标志后全量重建。
void BytecodeTracePanel::refreshTraceTable() {
    int histSize = static_cast<int>(traceHistory_.size());

    // R51-2 fix: deque 滑动后索引整体偏移，必须全量重建
    if (dequeShifted_) {
        traceTable_->setRowCount(0);
        dequeShifted_ = false;
    }

    int tableRows = traceTable_->rowCount();

    // 表行多于历史（清空场景或溢出移除）→ 移除多余行
    while (tableRows > histSize) {
        traceTable_->removeRow(tableRows - 1);
        --tableRows;
    }

    // 追加新行（仅新增部分）
    for (int i = tableRows; i < histSize; ++i) {
        traceTable_->insertRow(i);
    }

    // 填充新行的单元格数据（仅新行，旧行数据不变）
    for (int i = tableRows; i < histSize; ++i) {
        const auto& e = traceHistory_[i];
        auto* c0 = new QTableWidgetItem(QString::number(e.step));
        auto* c1 = new QTableWidgetItem(QString::number(e.ip));
        auto* c2 = new QTableWidgetItem(QString::fromUtf8(e.opCodeName.c_str()));
        auto* c3 = new QTableWidgetItem(QString::number(e.frameCount));
        auto* c4 = new QTableWidgetItem(QString::number(e.stackSnapshot.size()));
        c0->setTextAlignment(Qt::AlignCenter);
        c1->setTextAlignment(Qt::AlignCenter);
        c3->setTextAlignment(Qt::AlignCenter);
        c4->setTextAlignment(Qt::AlignCenter);
        traceTable_->setItem(i, 0, c0);
        traceTable_->setItem(i, 1, c1);
        traceTable_->setItem(i, 2, c2);
        traceTable_->setItem(i, 3, c3);
        traceTable_->setItem(i, 4, c4);
    }
    traceTable_->scrollToBottom();
}

/// 选中某条轨迹行时渲染其详情：倒序展示操作数栈（栈顶在前），并显示
/// IP/行号/帧数；同时 emit sourceLineRequested 高亮编辑器对应源码行。
void BytecodeTracePanel::onTraceRowSelected() {
    int row = traceTable_->currentRow();
    if (row < 0 || row >= static_cast<int>(traceHistory_.size())) {
        stackDetail_->clear();
        return;
    }
    const auto& e = traceHistory_[row];

    QString stackHtml;
    if (e.stackSnapshot.empty()) {
        stackHtml = tr("<p><i>（操作数栈为空）</i></p>");
    } else {
        stackHtml = tr("<h4>操作数栈（栈顶 → 栈底）</h4><ol>");
        // 倒序显示：栈顶在前
        for (auto it = e.stackSnapshot.rbegin(); it != e.stackSnapshot.rend(); ++it) {
            stackHtml += QString("<li><code>%1</code></li>").arg(QString::fromUtf8(it->c_str()).toHtmlEscaped());
        }
        stackHtml += QStringLiteral("</ol>");
    }

    QString html = QString("<h3>Step %1: %2</h3>"
                           "<p><b>IP:</b> %3 | <b>行:</b> %4 | <b>帧数:</b> %5</p>"
                           "%6")
                       .arg(e.step)
                       .arg(QString::fromUtf8(e.opCodeName.c_str()).toHtmlEscaped()) // R54-15 fix: HTML 转义
                       .arg(e.ip)
                       .arg(e.line)
                       .arg(e.frameCount)
                       .arg(stackHtml);
    stackDetail_->setHtml(html);

    // Emit source line for editor highlighting
    if (e.line > 0) {
        emit sourceLineRequested(e.line);
    }
}

/// 用 OpCode 教学库名称填充左侧列表并默认选中首项。
void BytecodeTracePanel::populateDocs() {
    docList_->clear();
    for (const auto& d : BytecodeTraceLibrary::opCodeDocs()) {
        docList_->addItem(QString::fromUtf8(d.opCodeName.c_str()));
    }
    if (docList_->count() > 0) {
        docList_->setCurrentRow(0);
    }
}

/// 教学库列表选中项变化时委托 showDoc 渲染该 OpCode 的指令详情。
void BytecodeTracePanel::onDocSelected(int index) {
    showDoc(index);
}

/// 根据索引渲染 OpCode 文档：标题、分类、操作数格式、栈效果、Markdown 语义
/// 与样例代码（HTML 转义），写入右侧说明浏览器。
void BytecodeTracePanel::showDoc(int index) {
    currentDocIdx_ = index;
    if (index < 0 || index >= static_cast<int>(BytecodeTraceLibrary::opCodeDocs().size())) {
        docDetail_->clear();
        return;
    }
    const auto& d = BytecodeTraceLibrary::opCodeDocs()[index];
    // R54-16 fix: 统一 HTML 转义所有字段，对齐 R52 批量转义修复
    QString html = QString("<h2>%1</h2>"
                           "<p><b>分类:</b> %2</p>"
                           "<h3>操作数格式</h3>"
                           "<p><code>%3</code></p>"
                           "<h3>栈效果</h3>"
                           "<p><code>%4</code></p>"
                           "<h3>语义</h3>"
                           "%5"
                           "<h3>样例代码</h3>"
                           "<pre>%6</pre>")
                       .arg(QString::fromUtf8(d.opCodeName.c_str()).toHtmlEscaped())
                       .arg(QString::fromUtf8(d.category.c_str()).toHtmlEscaped())
                       .arg(QString::fromUtf8(d.operandFormat.c_str()).toHtmlEscaped())
                       .arg(QString::fromUtf8(d.stackEffect.c_str()).toHtmlEscaped())
                       .arg(MarkdownRenderer::markdownToHtmlFragment(d.semantics))
                       .arg(QString::fromUtf8(d.exampleCode.c_str()).toHtmlEscaped());
    docDetail_->setHtml(html);
}

/// 将当前选中 OpCode 的样例代码 emit loadSampleRequested，加载到主编辑器运行观察。
void BytecodeTracePanel::onLoadDocCode() {
    if (currentDocIdx_ < 0 || currentDocIdx_ >= static_cast<int>(BytecodeTraceLibrary::opCodeDocs().size())) {
        return;
    }
    const auto& d = BytecodeTraceLibrary::opCodeDocs()[currentDocIdx_];
    emit loadSampleRequested(QString::fromUtf8(d.exampleCode.c_str()));
}

// ============================================================
// createGuidedTour — 新手引导（5 步）
// ============================================================

/// 构建 5 步新手引导：高亮执行轨迹页、自动捕获、轨迹表、OpCode 教学库与加载样例按钮。
GuidedTour* BytecodeTracePanel::createGuidedTour(QWidget* host) {
    auto* tour = new GuidedTour(host, host);
    // UX-R fix: 子页按钮改从 TeachingSubPageBar 取（组件按钮为统一主题色样式）
    QWidget* traceBtn = subPageBar_ ? static_cast<QWidget*>(subPageBar_->buttonAt(0)) : this;
    tour->addStep(traceBtn, QString::fromUtf8("执行轨迹"),
                  QString::fromUtf8("这里显示每条字节码指令执行后的栈状态快照。"));
    tour->addStep(autoCaptureCheck_, QString::fromUtf8("自动捕获"),
                  QString::fromUtf8("勾选后，VM 暂停时会自动捕获一条轨迹，无需手动点击。"));
    tour->addStep(traceTable_, QString::fromUtf8("轨迹表"),
                  QString::fromUtf8("每行一条指令记录，包含 IP / OpCode / 栈快照。点击某行可定位到对应源码行。"));
    QWidget* libraryBtn = subPageBar_ ? static_cast<QWidget*>(subPageBar_->buttonAt(1)) : this;
    tour->addStep(libraryBtn, QString::fromUtf8("OpCode 教学库"),
                  QString::fromUtf8("切换到 OpCode 参考库，查看每条指令的语义说明与样例代码。"));
    tour->addStep(loadCodeBtn_, QString::fromUtf8("加载样例"),
                  QString::fromUtf8("点击可将当前 OpCode 的示例代码加载到主编辑器，方便直接运行观察。"));
    return tour;
}
