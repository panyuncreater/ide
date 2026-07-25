// ============================================================
// IRTransformLibrary.cpp — IR 变换教学场景库实现（P3-20 拆分）
// ------------------------------------------------------------
// 从 gui/IRTransformPanel.cpp 拆分而来，与 BugHuntLibrary.cpp /
// MemoryModelLibrary.cpp 拆分模式一致：将纯静态教学数据独立成库，
// 让面板文件聚焦于 UI 逻辑。
//
// 本文件提供两类静态场景数据：
//   - loweringExamples()：8 个 AST → IR lowering 教学场景
//   - optimizationExamples()：3 个优化 pass 前后对比场景
// 场景库的 IR 文本采用与 IRToString 一致的人类可读格式，
// 文本为教学示意，不依赖运行时 AstIRBuilder 实际产物。
// ============================================================

#include "gui/IRTransformPanel.h" // IRLoweringExample / IROptimizationExample 结构定义

#include <vector>

// ============================================================
// IRTransformLibrary — 静态教学场景库
// ============================================================

/// 返回 AST → IR lowering 教学场景库（静态单例）。
/// 每个场景演示一种 AST 节点（字面量 / 二元运算 / 变量声明 / 分支 / 循环 /
/// 函数调用 / 闭包 / 类方法）如何 lowering 为三地址码 IR 指令，供 lowering 页展示。
const std::vector<IRLoweringExample>& IRTransformLibrary::loweringExamples() {
    static const std::vector<IRLoweringExample> kExamples = {
        {"lit-int", "🔢 整数字面量 42",
         "🔢 整数字面量直接 LOAD_CONST 加载到虚拟寄存器。<br>"
         "<b>类比：</b>虚拟寄存器（v0、v1…）就像一排带编号的储物柜，编译器把值暂存进去，后面随时按编号取用。<br>"
         "<b>常量池：</b>常量 42 会被登记进 IRFunction.constants 这张「常量清单」。如果后面又出现 "
         "42，不会重复登记，而是复用同一个索引 #0——就像图书馆里同一本书只编一个索书号。<br>"
         "<b>输入输出：</b>源码「var x = 42;」会生成「v0 = LOAD_CONST #0 (42)」，再「STORE_LOCAL」写回变量 x。运行后 x "
         "的值就是 42。",
         "NumberLiteral(42)", "var x = 42;",
         "function main {\n"
         "  block L0:\n"
         "    v0 = LOAD_CONST #0 (42)\n"
         "    v1 = STORE_LOCAL slot=0, v0   # x\n"
         "    end block\n"
         "}\n"
         "constants: [42]"},
        {"binop-add", "⚙️ 二元加法 a + b",
         "⚙️ BinaryOp(ADD) lowering：左侧操作数先算到 vreg0，右侧算到 vreg1，再用一条 ADD 指令把结果写入 vreg2。<br>"
         "<b>三地址码：</b>形式固定为「dest = OP src1, "
         "src2」——每条指令最多涉及三个「地址」（一个结果加两个来源），这正是「三地址码」名字的由来。<br>"
         "<b>类比：</b>就像厨房做菜，先把食材 a、b 分别备好放在两个碗里（v0、v1），最后倒进炒锅（v2）翻炒出成品。<br>"
         "<b>输入输出：</b>源码「var c = a + b;」生成「v2 = ADD v0, v1」再存回 c。若 a=3、b=4，则 c=7。",
         "BinaryOp(ADD, VarRef(a), VarRef(b))", "var c = a + b;",
         "function main {\n"
         "  block L0:\n"
         "    v0 = LOAD_LOCAL slot=0   # a\n"
         "    v1 = LOAD_LOCAL slot=1   # b\n"
         "    v2 = ADD v0, v1\n"
         "    v3 = STORE_LOCAL slot=2, v2   # c\n"
         "    end block\n"
         "}"},
        {"var-decl", "📝 变量声明 var x = expr",
         "📝 VarDecl lowering：先求初始化表达式的值到 vreg，再用 STORE_LOCAL 写进该变量的「局部槽位」（slot）。<br>"
         "<b>槽位：</b>每个局部变量在函数里占一个编号格子（slot 0、slot "
         "1…），好比一排带名字的信箱。变量名在编译期就绑定到固定槽位，运行期不再查表。<br>"
         "<b>未初始化：</b>如果写成「var x;」没有初值，编译器会用 LOAD_NULL "
         "给这个格子先放一个「空值」占位，保证任何读取都有定义。<br>"
         "<b>输入输出：</b>「var x = 1;」生成「v0 = LOAD_CONST #0 (1)」再「v1 = STORE_LOCAL slot=0, v0」。运行后 x "
         "的值为 1。",
         "VarDecl(x, init=NumberLiteral(1))", "var x = 1;",
         "function main {\n"
         "  block L0:\n"
         "    v0 = LOAD_CONST #0 (1)\n"
         "    v1 = STORE_LOCAL slot=0, v0   # x\n"
         "    end block\n"
         "}"},
        {"if-stmt", "🔀 条件分支 if (cond) {...} else {...}",
         "🔀 IfStmt lowering：先求条件 cond 到 vreg，再生成 BRANCH_FALSE——若条件为假就跳到 else "
         "块（L1），为真则顺序往下执行 then 块。<br>"
         "<b>基本块与跳转：</b>代码被切成多个「基本块」（L0/L1/L2），每块末尾要么是跳转、要么是条件分支。then "
         "块执行完用 JUMP 跳过 else，直接到 L2 收尾。<br>"
         "<b>回填跳转：</"
         "b>编译器先把跳转目标写成占位符，等所有基本块编号确定后再回填正确地址，这一步叫「patchJumps（回填跳转）」。<"
         "br>"
         "<b>输入输出：</b>「if (x) { print 1; } else { print 2; }」当 x 为真时打印 1，为假时打印 "
         "2；两条分支不会同时执行。",
         "IfStmt(cond=VarRef(x), then=Block, else=Block)", "if (x) { print 1; } else { print 2; }",
         "function main {\n"
         "  block L0:\n"
         "    v0 = LOAD_LOCAL slot=0   # x\n"
         "    BRANCH_FALSE v0, L1       # 跳到 else\n"
         "    # then 块\n"
         "    v1 = LOAD_CONST #0 (1)\n"
         "    CALL_PRINT v1\n"
         "    JUMP L2\n"
         "  block L1:                    # else\n"
         "    v2 = LOAD_CONST #1 (2)\n"
         "    CALL_PRINT v2\n"
         "  block L2:                    # end\n"
         "    end block\n"
         "}"},
        {"while-stmt", "🔄 循环 while (cond) {...}",
         "🔄 WhileStmt lowering：在循环头 L0 求值条件，为假就用 BRANCH_FALSE 跳出到 L2；为真则进入循环体 "
         "L1，执行完末尾 JUMP 回到 L0 重新判断。<br>"
         "<b>闭环结构：</"
         "b>"
         "循环的本质就是「判断→执行→跳回判断」的闭环。每绕一圈，条件都会被重新求值，一旦不满足就退出，因此永远不会在条"
         "件为假时卡死。<br>"
         "<b>基本块终结规则：</b>每个基本块必须以 BRANCH / JUMP / RETURN "
         "之一收尾，这样控制流才清晰、可被优化器分析。<br>"
         "<b>输入输出：</b>「while (i 小于 10) { i = i + 1; }」从 i 的初值开始，每轮 i 加 1，直到 i 不再小于 10 "
         "时停止（若初值已 ≥10 则一次都不执行）。",
         "WhileStmt(cond=BinaryOp(LT, VarRef(i), NumberLiteral(10)))", "while (i < 10) { i = i + 1; }",
         "function main {\n"
         "  block L0:                    # start\n"
         "    v0 = LOAD_LOCAL slot=0   # i\n"
         "    v1 = LOAD_CONST #0 (10)\n"
         "    v2 = LT v0, v1\n"
         "    BRANCH_FALSE v2, L2        # 跳出循环\n"
         "  block L1:                    # body\n"
         "    v3 = LOAD_LOCAL slot=0\n"
         "    v4 = LOAD_CONST #1 (1)\n"
         "    v5 = ADD v3, v4\n"
         "    v6 = STORE_LOCAL slot=0, v5   # i = i + 1\n"
         "    JUMP L0\n"
         "  block L2:                    # end\n"
         "    end block\n"
         "}"},
        {"fun-call", "⚙️ 函数调用 f(a, b)",
         "⚙️ FunCall lowering：先把每个实参从左到右求值到独立 vreg，再生成 CALL 指令，携带「函数索引 "
         "func=#0」和「参数个数 argc」。<br>"
         "<b>类比：</"
         "b>"
         "就像打电话——先按号码找到对方（函数索引），把要说的话依次准备好（参数），拨通后对方返回的结果（返回值）若你需"
         "要就记在 vreg 里，不需要就挂掉丢弃。<br>"
         "<b>返回值处理：</b>若调用结果被赋值（如「var r = f(a,b);」），返回值存入 vreg 再写回 "
         "r；若只是「f(a,b);」作为语句调用，则由 CALL_POP 把返回值弹出栈，避免堆积。<br>"
         "<b>输入输出：</b>「var r = f(a, b);」最终 r 得到 f 的返回值。",
         "FunCall(f, args=[VarRef(a), VarRef(b)])", "var r = f(a, b);",
         "function main {\n"
         "  block L0:\n"
         "    v0 = LOAD_LOCAL slot=0   # a\n"
         "    v1 = LOAD_LOCAL slot=1   # b\n"
         "    v2 = CALL func=#0, argc=2, args=[v0, v1]\n"
         "    v3 = STORE_LOCAL slot=2, v2   # r\n"
         "    end block\n"
         "}"},
        {"closure", "📦 闭包捕获 makeCounter",
         "📦 闭包 lowering：函数「记住」了定义时所在环境的局部变量，这些被捕获的变量提升为 "
         "upvalue（上层值），MAKE_CLOSURE 指令会带上捕获列表。<br>"
         "<b>类比：</"
         "b>"
         "闭包像一封「带着老家钥匙的信」——即使离开了定义时的环境，它仍握着访问那些局部变量的钥匙（upvalue），随时能回去"
         "读写。<br>"
         "<b>防内存泄漏：</b>闭包引用外层环境、外层环境又可能引用闭包，容易形成循环引用。ClosureData 用 "
         "weak_ptr<Environment> 弱引用打破这个环，让垃圾回收能正确回收。<br>"
         "<b>输入输出：</b>「makeCounter」返回的 counter 函数每次调用都让内部计数 c 加 1 并返回，多次调用得到 1、2、3… "
         "这样递增的序列。",
         "FunDecl(makeCounter) → body returns FunDecl(counter)",
         "fun makeCounter() {\n  var c = 0;\n  fun counter() { c = c + 1; return c; }\n  return counter;\n}",
         "function main {\n"
         "  block L0:\n"
         "    v0 = MAKE_CLOSURE func=#0, captures=[]\n"
         "    v1 = STORE_LOCAL slot=0, v0   # makeCounter\n"
         "    end block\n"
         "}\n"
         "function counter {\n"
         "  # upvalue: c (来自 makeCounter 的局部)\n"
         "  block L0:\n"
         "    v0 = LOAD_UPVALUE idx=0   # c\n"
         "    v1 = LOAD_CONST #0 (1)\n"
         "    v2 = ADD v0, v1\n"
         "    v3 = STORE_UPVALUE idx=0, v2   # c = c + 1\n"
         "    v4 = LOAD_UPVALUE idx=0\n"
         "    RETURN v4\n"
         "}"},
        {"class-method", "🏛️ 类方法 Point() 构造",
         "🏛️ ClassDecl lowering：类的每个方法（如 init、distance）都作为独立的 IRFunction 单独编译，互不干扰。<br>"
         "<b>隐式 this：</b>方法被调用时，对象自身会作为第 0 号槽位（slot 0）传入，名字叫 "
         "this。方法体内访问「x」「y」其实都是在操作 this 指向的实例字段。<br>"
         "<b>字段访问：</b>MethodCall 通过 SET_FIELD / GET_FIELD "
         "读写实例字段，指令里标注「this=slot0」和字段名索引（如 #0 代表 x）。<br>"
         "<b>输入输出：</b>「Point(px, py)」执行后，会把传入的 px、py 写进 this 的 x、y "
         "字段；后续通过「点对象.x」即可取回这两个值。",
         "ClassDecl(Point) → methods=[init, distance]",
         "class Point {\n  var x;\n  var y;\n  fun init(px, py) { x = px; y = py; }\n}",
         "function Point::init {\n"
         "  # slot 0 = this (隐式)\n"
         "  block L0:\n"
         "    v0 = LOAD_LOCAL slot=1   # px\n"
         "    v1 = SET_FIELD this=slot0, name=#0 (x), v0\n"
         "    v2 = LOAD_LOCAL slot=2   # py\n"
         "    v3 = SET_FIELD this=slot0, name=#1 (y), v2\n"
         "    RETURN_NULL\n"
         "}"},
    };
    return kExamples;
}

/// 返回优化 pass 教学场景库（静态单例）：常量折叠 / 死代码消除 / 复制传播。
/// 每个场景含优化前 IR、优化后 IR 与指令数变化，供优化对比页并排展示。
const std::vector<IROptimizationExample>& IRTransformLibrary::optimizationExamples() {
    static const std::vector<IROptimizationExample> kExamples = {
        {"const-fold", "✨ 常量折叠 1 + 2 → 3",
         "✨ 常量折叠（Constant Folding）pass：扫描 "
         "IR，凡是操作数全部是常量的算术指令，就在「编译期」直接算出结果，替换成一条 LOAD_CONST。<br>"
         "<b>类比：</b>就像你在草稿纸上先把「1+2」算成 3 再抄到正式试卷上——机器运行时就省去了这次加法，直接拿到答案 "
         "3。<br>"
         "<b>适用范围：</b>int / float 的加减乘除、布尔逻辑（如 true 与 false "
         "的与运算）、字符串拼接（「a」+「b」）等都能折叠。<br>"
         "<b>效果：</b>本例「x = 1 + 2」从 4 条指令压缩到 2 条（ADD 被折叠消失），运行更快、体积更小。",
         "Constant Folding",
         "function main {\n"
         "  block L0:\n"
         "    v0 = LOAD_CONST #0 (1)\n"
         "    v1 = LOAD_CONST #1 (2)\n"
         "    v2 = ADD v0, v1\n"
         "    v3 = STORE_LOCAL slot=0, v2   # x = 1 + 2\n"
         "    end block\n"
         "}",
         "function main {\n"
         "  block L0:\n"
         "    v0 = LOAD_CONST #2 (3)        # 折叠后的常量\n"
         "    v1 = STORE_LOCAL slot=0, v0   # x = 3\n"
         "    end block\n"
         "}",
         4, 2},
        {"dead-code", "🧹 死代码消除（未使用的赋值）",
         "🧹 死代码消除（Dead Code Elimination, "
         "DCE）：找出「结果再也没有被使用、且本身没有副作用」的指令，直接删除。<br>"
         "<b>副作用：</b>像打印、写入文件这种会改变外部状态的操作叫「有副作用」，不能随便删；而单纯的「x = "
         "1」若后面从没读 x，就是无用的死代码。<br>"
         "<b>类比：</"
         "b>如同写了一行笔记却从没翻看过，这行笔记对结果毫无贡献，删掉它不影响最终答案，还能让页面更干净。<br>"
         "<b>效果：</b>本例「x = 1」从未被读取，整条赋值链被消除，指令数从 4 降到 2，只保留真正会打印的「hello」。",
         "Dead Code Elimination",
         "function main {\n"
         "  block L0:\n"
         "    v0 = LOAD_CONST #0 (1)\n"
         "    v1 = STORE_LOCAL slot=0, v0   # x = 1 (从未使用)\n"
         "    v2 = LOAD_CONST #1 (\"hello\")\n"
         "    CALL_PRINT v2\n"
         "    end block\n"
         "}",
         "function main {\n"
         "  block L0:\n"
         "    v0 = LOAD_CONST #0 (\"hello\")\n"
         "    CALL_PRINT v0\n"
         "    end block\n"
         "}",
         4, 2},
        {"copy-prop", "📋 复制传播 + 跳转优化",
         "📋 复制传播（Copy Propagation）+ DCE：先识别「v_b = MOVE v_a」这类「把 a 原样抄给 b」的指令，把后面所有用到 "
         "v_b 的地方都改成直接用 v_a，于是 MOVE 本身成了多余。<br>"
         "<b>连锁反应：</b>消掉 MOVE 后，原本只为 MOVE 准备的中间值常常变成死代码，于是 DCE "
         "会「二次出手」再删一轮，指令进一步减少。<br>"
         "<b>类比：</b>好比同事把文件从 A 夹复制到 B 夹，但之后大家都直接看 A 夹的原件——那 B "
         "夹那份拷贝就是多余的，删掉它反而清爽。<br>"
         "<b>效果：</b>本例「b = a」被消除后，后续「b + 1」直接变成「a + 1」，指令数从 5 降到 4，且变量关系更直观。",
         "Copy Propagation + DCE",
         "function main {\n"
         "  block L0:\n"
         "    v0 = LOAD_LOCAL slot=0   # a\n"
         "    v1 = MOVE v0              # b = a\n"
         "    v2 = LOAD_CONST #0 (1)\n"
         "    v3 = ADD v1, v2           # b + 1\n"
         "    v4 = STORE_LOCAL slot=1, v3   # c = b + 1\n"
         "    end block\n"
         "}",
         "function main {\n"
         "  block L0:\n"
         "    v0 = LOAD_LOCAL slot=0   # a\n"
         "    v1 = LOAD_CONST #0 (1)\n"
         "    v2 = ADD v0, v1           # a + 1 (替换 v1→v0，消除 MOVE)\n"
         "    v3 = STORE_LOCAL slot=1, v2   # c = a + 1\n"
         "    end block\n"
         "}",
         5, 4},
    };
    return kExamples;
}
