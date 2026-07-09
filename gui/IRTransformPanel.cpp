// ============================================================
// IRTransformPanel.cpp — IR 变换过程可视化面板实现（第二波 P1-1）
// ============================================================

#include "gui/IRTransformPanel.h"
#include "app/IdeController.h"
#include "compiler/Compiler.h"
#include "compiler/IR.h"
#include "gui/PanelAnimator.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QRegularExpression>
#include <QSplitter>
#include <QVBoxLayout>
#include <sstream>
#include <utility> // std::pair for IROptReplayLibrary::replayScenarios()

#include "Label.h"      // QFluentKit（CaptionLabel）
#include "PushButton.h" // QFluentKit（PrimaryPushButton）
#include "Theme.h"      // QFluentKit（onThemeModeChanged 信号）
#include "gui/TeachingTheme.h"

// --- Clickable IR/bytecode HTML helper ---
// Converts plain text where lines may end with "; line N" into HTML
// with those lines wrapped in <a href="#LINE_N"> anchors.
namespace {
/// 将纯文本 IR 转换为可点击 HTML：识别行尾 "; line N" 注释，把该行
/// 包裹为 <a href="#LINE_N"> 锚点，便于在浏览器中点击跳转到源码对应行。
static QString irTextToClickableHtml(const QString& plainText) {
    QStringList lines = plainText.split("\n");
    QString html;
    html += "<pre style=\"font-family:Consolas,monospace;\">";
    static const QRegularExpression lineCommentRe(";\\s*line\\s+(\\d+)\\s*$");
    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines[i];
        auto m = lineCommentRe.match(line);
        if (m.hasMatch()) {
            QString lineNum = m.captured(1);
            QString escaped = line.toHtmlEscaped();
            html +=
                "<a href=\"#LINE_" + lineNum + "\" style=\"color:inherit;text-decoration:none;\">" + escaped + "</a>";
        } else {
            html += line.toHtmlEscaped();
        }
        if (i < lines.size() - 1)
            html += "\n";
    }
    html += "</pre>";
    return html;
}
} // anonymous namespace
// --- End clickable IR HTML helper ---

// ============================================================
// IRTransformLibrary — 静态教学场景库
// ============================================================
//
// 注：场景库的 IR 文本采用 IRToString 一致的人类可读格式，
// 让学习者直观看到 AST 节点如何 lowering 为三地址码 IR 指令。
// 文本为教学示意，不依赖运行时 AstIRBuilder 实际产物。

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

// ============================================================
// IRTransformPanel 实现
// ============================================================

/// 构造面板：组装顶部 4 个子页切换按钮（lowering / 优化对比 / 当前源码 IR /
/// 逐步回放）与 QStackedWidget，构建各子页并连接切换信号、主题刷新与列表填充。
IRTransformPanel::IRTransformPanel(QWidget* parent) : QWidget(parent) {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(4);

    auto* pageBar = new QHBoxLayout;
    pageLoweringBtn_ = new QPushButton(QString::fromUtf8("AST → IR lowering"), this);
    pageOptimizeBtn_ = new QPushButton(QString::fromUtf8("优化 pass 对比"), this);
    pageCurrentBtn_ = new QPushButton(QString::fromUtf8("当前源码 IR"), this);
    pageReplayBtn_ = new QPushButton(QString::fromUtf8("逐步优化回放"), this);
    pageLoweringBtn_->setCheckable(true);
    pageOptimizeBtn_->setCheckable(true);
    pageCurrentBtn_->setCheckable(true);
    pageReplayBtn_->setCheckable(true);
    pageBar->addWidget(pageLoweringBtn_);
    pageBar->addWidget(pageOptimizeBtn_);
    pageBar->addWidget(pageCurrentBtn_);
    pageBar->addWidget(pageReplayBtn_);
    pageBar->addStretch();
    mainLayout->addLayout(pageBar);

    stack_ = new QStackedWidget(this);
    mainLayout->addWidget(stack_, 1);

    auto* p1 = new QWidget(this);
    auto* p2 = new QWidget(this);
    auto* p3 = new QWidget(this);
    auto* p4 = new QWidget(this);
    buildLoweringPage(p1);
    buildOptimizePage(p2);
    buildCurrentPage(p3);
    buildReplayPage(p4);
    stack_->addWidget(p1);
    stack_->addWidget(p2);
    stack_->addWidget(p3);
    stack_->addWidget(p4);

    pageLoweringBtn_->setChecked(true);
    stack_->setCurrentIndex(0);

    connect(pageLoweringBtn_, &QPushButton::clicked, this, [this]() {
        stack_->setCurrentIndex(0);
        pageLoweringBtn_->setChecked(true);
        pageOptimizeBtn_->setChecked(false);
        pageCurrentBtn_->setChecked(false);
        pageReplayBtn_->setChecked(false);
        PanelAnimator::slideInWidget(stack_->currentWidget());
    });
    connect(pageOptimizeBtn_, &QPushButton::clicked, this, [this]() {
        stack_->setCurrentIndex(1);
        pageLoweringBtn_->setChecked(false);
        pageOptimizeBtn_->setChecked(true);
        pageCurrentBtn_->setChecked(false);
        pageReplayBtn_->setChecked(false);
        PanelAnimator::slideInWidget(stack_->currentWidget());
    });
    connect(pageCurrentBtn_, &QPushButton::clicked, this, [this]() {
        stack_->setCurrentIndex(2);
        pageLoweringBtn_->setChecked(false);
        pageOptimizeBtn_->setChecked(false);
        pageCurrentBtn_->setChecked(true);
        pageReplayBtn_->setChecked(false);
        populateCurrentIR();
        PanelAnimator::slideInWidget(stack_->currentWidget());
    });
    connect(pageReplayBtn_, &QPushButton::clicked, this, [this]() {
        stack_->setCurrentIndex(3);
        pageLoweringBtn_->setChecked(false);
        pageOptimizeBtn_->setChecked(false);
        pageCurrentBtn_->setChecked(false);
        pageReplayBtn_->setChecked(true);
        PanelAnimator::slideInWidget(stack_->currentWidget());
    });

    populateLoweringList();
    populateOptList();
    populateReplayList();

    // 主题切换时刷新当前页 HTML（populateLoweringDetail 中 <pre> 背景使用
    // TeachingTheme::surface()，需重新渲染以跟随新主题）。
    // receiver=this 保证生命周期安全，析构自动断开。
    Theme::onThemeModeChanged(this, [this](Fluent::ThemeMode) {
        if (!stack_)
            return;
        switch (stack_->currentIndex()) {
        case 0: // AST → IR lowering 页（HTML 含主题色 <pre> 背景）
            if (loweringList_)
                populateLoweringDetail(loweringList_->currentRow());
            break;
        case 1: // 优化 pass 对比页（setPlainText，无主题依赖，仍刷新以防未来扩展）
            if (optList_)
                populateOptDetail(optList_->currentRow());
            break;
        case 2: // 当前源码 IR 页（setPlainText，无主题依赖）
            populateCurrentIR();
            break;
        case 3: // 逐步优化回放页（setPlainText，无主题依赖）
            if (replayList_ && replayStepsList_)
                populateReplayStep(replayList_->currentRow(), replayStepsList_->currentRow());
            break;
        default:
            break;
        }
    });
}

/// 构建「AST → IR lowering」子页：左侧产生式/节点列表 + 右侧说明浏览器，
/// 选中项变化时通过 populateLoweringDetail 渲染 HTML。
void IRTransformPanel::buildLoweringPage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* splitter = new QSplitter(Qt::Horizontal, host);
    loweringList_ = new QListWidget(host);
    loweringDetail_ = new QTextBrowser(host);
    splitter->addWidget(loweringList_);
    splitter->addWidget(loweringDetail_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);
    splitter->setSizes({200, 400});
    layout->addWidget(splitter);

    connect(loweringList_, &QListWidget::currentRowChanged, this, &IRTransformPanel::populateLoweringDetail);
}

/// 构建「优化 pass 对比」子页：左侧优化 pass 列表 + 右侧优化前/后 IR 浏览器
/// 及摘要标签，选中项变化时通过 populateOptDetail 显示指令数变化。
void IRTransformPanel::buildOptimizePage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);

    optList_ = new QListWidget(host);
    optSummary_ = new QLabel(QString::fromUtf8("请选择优化 pass"), host);

    auto* beforeWrap = new QWidget(host);
    auto* beforeLayout = new QVBoxLayout(beforeWrap);
    beforeLayout->setContentsMargins(0, 0, 0, 0);
    beforeLayout->addWidget(new QLabel(QString::fromUtf8("优化前 IR：")));
    optBefore_ = new QTextBrowser(host);
    beforeLayout->addWidget(optBefore_, 1);

    auto* afterWrap = new QWidget(host);
    auto* afterLayout = new QVBoxLayout(afterWrap);
    afterLayout->setContentsMargins(0, 0, 0, 0);
    afterLayout->addWidget(new QLabel(QString::fromUtf8("优化后 IR：")));
    optAfter_ = new QTextBrowser(host);
    afterLayout->addWidget(optAfter_, 1);

    auto* splitter = new QSplitter(Qt::Horizontal, host);
    splitter->addWidget(optList_);
    splitter->addWidget(beforeWrap);
    splitter->addWidget(afterWrap);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    splitter->setStretchFactor(2, 1);
    splitter->setSizes({150, 250, 250});

    layout->addWidget(optSummary_);
    layout->addWidget(splitter, 1);

    connect(optList_, &QListWidget::currentRowChanged, this, &IRTransformPanel::populateOptDetail);
}

/// 构建「当前源码 IR」子页：刷新按钮 + 状态标签 + 可点击 IR 浏览器。
/// 浏览器将 "; line N" 渲染为锚点，点击 emit sourceLineRequested 高亮对应源码行。
void IRTransformPanel::buildCurrentPage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* toolbar = new QHBoxLayout;
    refreshBtn_ = new PrimaryPushButton(QString::fromUtf8("重新生成 IR"), host);
    currentStatusLabel_ = new CaptionLabel(QString::fromUtf8("尚未生成"), host);
    toolbar->addWidget(refreshBtn_);
    toolbar->addStretch();
    toolbar->addWidget(currentStatusLabel_);
    layout->addLayout(toolbar);

    currentIrBrowser_ = new QTextBrowser(host);
    currentIrBrowser_->setFont(QFont("Consolas"));
    currentIrBrowser_->setOpenLinks(false);
    currentIrBrowser_->setOpenExternalLinks(false);
    layout->addWidget(currentIrBrowser_, 1);

    connect(refreshBtn_, &QPushButton::clicked, this, [this]() { populateCurrentIR(); });

    // Click-to-highlight: extract source line from anchor and emit signal
    connect(currentIrBrowser_, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
        QString fragment = url.fragment();
        if (fragment.startsWith("LINE_")) {
            bool ok = false;
            int line = fragment.mid(5).toInt(&ok);
            if (ok && line > 0) {
                emit sourceLineRequested(line);
            }
        }
    });
}

/// 当面板当前停在「当前源码 IR」子页（index==2）时，重新生成并刷新 IR 显示。
void IRTransformPanel::reloadCurrentIR() {
    if (stack_->currentIndex() == 2) {
        populateCurrentIR();
    }
}

/// 用 lowering 场景库标题填充左侧列表并默认选中首项。
void IRTransformPanel::populateLoweringList() {
    loweringList_->clear();
    const auto& items = IRTransformLibrary::loweringExamples();
    for (const auto& e : items) {
        loweringList_->addItem(QString::fromUtf8(e.title.c_str()));
    }
    if (!items.empty())
        loweringList_->setCurrentRow(0);
}

/// 根据选中索引渲染 lowering 详情：标题 / AST 节点 / 源码 / 说明 +
/// 原始 IR（<pre> 背景用 TeachingTheme::surface() 以跟随主题）。
// R51-8 fix: 所有库文本字段经 toHtmlEscaped 转义，防止 < > & 等字符破坏 HTML 结构
void IRTransformPanel::populateLoweringDetail(int index) {
    const auto& items = IRTransformLibrary::loweringExamples();
    if (index < 0 || index >= (int)items.size())
        return;
    const auto& e = items[index];

    // R51-8 fix: 转义库文本中的 HTML 特殊字符
    auto esc = [](const std::string& s) -> std::string {
        return QString::fromUtf8(s.c_str()).toHtmlEscaped().toStdString();
    };

    std::ostringstream os;
    os << "<h3>" << esc(e.title) << "</h3>";
    os << "<p><b>AST 节点：</b> <code>" << esc(e.astSummary) << "</code></p>";
    os << "<p><b>源码：</b> <code>" << esc(e.sourceCode) << "</code></p>";
    os << "<hr>";
    os << "<p>" << esc(e.description) << "</p>";
    os << "<h4>Lowering 后的 IR：</h4>";
    os << "<pre style='background:" << TeachingTheme::surface().name().toStdString()
       << "; padding:8px; font-family:Consolas;'>" << esc(e.irBefore) << "</pre>";
    loweringDetail_->setHtml(QString::fromUtf8(os.str().c_str()));
    // 注：移除 fadeInWidget —— opacity 卡 0 导致切换后详情区空白
}

/// 用优化场景库的「pass 名 + 标题」填充左侧列表并默认选中首项。
void IRTransformPanel::populateOptList() {
    optList_->clear();
    const auto& items = IRTransformLibrary::optimizationExamples();
    for (const auto& e : items) {
        QString text = QString::fromUtf8("[%1] %2")
                           .arg(QString::fromUtf8(e.passName.c_str()))
                           .arg(QString::fromUtf8(e.title.c_str()));
        optList_->addItem(text);
    }
    if (!items.empty())
        optList_->setCurrentRow(0);
}

/// 根据选中索引渲染优化对比：摘要标签（标题 + 指令数变化）+ 优化前/后 IR 文本。
void IRTransformPanel::populateOptDetail(int index) {
    const auto& items = IRTransformLibrary::optimizationExamples();
    if (index < 0 || index >= (int)items.size())
        return;
    const auto& e = items[index];

    optSummary_->setText(QString::fromUtf8("%1 — %2 → %3 条指令（减少 %4 条）")
                             .arg(QString::fromUtf8(e.title.c_str()))
                             .arg(e.instrBefore)
                             .arg(e.instrAfter)
                             .arg(e.instrBefore - e.instrAfter));

    optBefore_->setPlainText(QString::fromUtf8(e.irBefore.c_str()));
    optAfter_->setPlainText(QString::fromUtf8(e.irAfter.c_str()));
    // 注：移除 fadeInWidget —— opacity 卡 0 导致切换后详情区空白
}

/// 从 controller 取 AST，用 AstIRBuilder 生成 IRModule，经 IRToString 转文本后
/// 渲染为可点击 HTML，并显示基本块/指令数。异常时在状态栏报告。
void IRTransformPanel::populateCurrentIR() {
    if (!controller_) {
        currentStatusLabel_->setText(QString::fromUtf8("未绑定控制器"));
        currentIrBrowser_->setPlainText(QString::fromUtf8("（未绑定控制器）"));
        return;
    }
    Block* ast = controller_->astRoot();
    if (!ast) {
        currentStatusLabel_->setText(QString::fromUtf8("请先在主编辑器中输入并编译代码"));
        currentIrBrowser_->setPlainText(QString::fromUtf8("（无 AST）"));
        return;
    }

    try {
        AstIRBuilder builder;
        builder.build(*ast);
        IRModule* mod = builder.getModule();
        if (!mod || !mod->mainFunction) {
            currentStatusLabel_->setText(QString::fromUtf8("IR 生成失败"));
            return;
        }
        std::string irText = IRToString(*mod->mainFunction);
        currentIrBrowser_->setHtml(irTextToClickableHtml(QString::fromUtf8(irText.c_str())));
        int instrCount = 0;
        for (const auto& blk : mod->mainFunction->blocks) {
            instrCount += (int)blk.instructions.size();
        }
        currentStatusLabel_->setText(QString::fromUtf8("已生成 IR：%1 基本块 / %2 条指令")
                                         .arg(mod->mainFunction->blocks.size())
                                         .arg(instrCount));
    } catch (const std::exception& e) {
        currentStatusLabel_->setText(QString::fromUtf8("IR 生成异常"));
        currentIrBrowser_->setPlainText(QString::fromUtf8(e.what()));
    }
}

// ============================================================
// IROptReplayLibrary — 逐步优化回放场景库
// ============================================================
//
// 注：每个场景包含 3 个 IROptStepRecord，逐步回放一个优化 pass 的执行轨迹。
// irSnapshot 采用与 IRToString 一致的人类可读格式（含 "function" 关键字），
// decisions 字段含具体指令级别的修改/删除原因，便于学习者理解 pass 内部行为。

/// 返回逐步优化回放场景库（静态单例）：每个场景含 3 个 IROptStepRecord，
/// 逐步回放一个优化 pass（常量折叠 / DCE / 复制传播 / CSE / 循环展开）的执行轨迹，
/// 含每轮 IR 快照与指令级修改/删除决策，供回放页展示。
const std::vector<std::pair<std::string, std::vector<IROptStepRecord>>>& IROptReplayLibrary::replayScenarios() {
    static const std::vector<std::pair<std::string, std::vector<IROptStepRecord>>> kScenarios = {
        // ---- 1. 常量折叠回放（3 步骤）----
        {"replay-const-fold",
         {{"✨ 常量折叠",
           "Round 1",
           "function main {\n"
           "  block L0:\n"
           "    v0 = LOAD_CONST #0 (1)\n"
           "    v1 = LOAD_CONST #1 (2)\n"
           "    v2 = ADD v0, v1              # 1 + 2 → 3 (本次折叠)\n"
           "    v3 = LOAD_CONST #2 (3)\n"
           "    v4 = LOAD_CONST #3 (4)\n"
           "    v5 = MUL v3, v4              # 待下轮折叠\n"
           "    v6 = STORE_LOCAL slot=0, v5   # x = (1+2) * (3*4)\n"
           "    end block\n"
           "}\n",
           {"v2 = ADD v0, v1 → LOAD_CONST #2 (3) — 折叠原因：v0=LOAD_CONST #0 (1), v1=LOAD_CONST #1 (2) 均为常量加载",
            "保留 v3 = LOAD_CONST #3 (4), v4 = LOAD_CONST #4 (4) — MUL "
            "操作数均为常量但下一轮再折叠（保守策略，避免一次折叠破坏数据流分析）",
            "v0/v1 暂不删除 — DCE pass 会在后续步骤中清理无引用的纯计算指令"},
           8,
           1},
          {"✨ 常量折叠",
           "Round 2",
           "function main {\n"
           "  block L0:\n"
           "    v0 = LOAD_CONST #0 (1)\n"
           "    v1 = LOAD_CONST #1 (2)\n"
           "    v2 = LOAD_CONST #2 (3)        # 已折叠\n"
           "    v3 = LOAD_CONST #3 (3)\n"
           "    v4 = LOAD_CONST #4 (4)\n"
           "    v5 = MUL v3, v4 → LOAD_CONST #5 (12)  # 3 * 4 → 12 (本次折叠)\n"
           "    v6 = STORE_LOCAL slot=0, v5   # x = 12\n"
           "    end block\n"
           "}\n",
           {"v5 = MUL v3, v4 → LOAD_CONST #5 (12) — 折叠原因：v3=LOAD_CONST #3 (3), v4=LOAD_CONST #4 (4) 均为常量加载",
            "新增常量 #5 (12) 加入常量池（重复时复用索引）", "v3/v4 暂不删除 — 等待 DCE pass 清理"},
           8,
           1},
          {"✨ 常量折叠",
           "Round 3 (收敛)",
           "function main {\n"
           "  block L0:\n"
           "    v2 = LOAD_CONST #2 (3)        # 已折叠\n"
           "    v5 = LOAD_CONST #5 (12)       # 已折叠\n"
           "    v6 = STORE_LOCAL slot=0, v5   # x = 12\n"
           "    end block\n"
           "}\n",
           {"本 round 未发生折叠 — IR 已收敛（无更多可折叠的算术指令）",
            "下一阶段：DCE pass 将清理 v0/v1/v3/v4 等无引用的 LOAD_CONST"},
           3,
           0}}},
        // ---- 2. 死代码消除回放（3 步骤）----
        {"replay-dce",
         {{"🧹 DCE",
           "Round 1",
           "function main {\n"
           "  block L0:\n"
           "    v0 = LOAD_CONST #0 (1)       # 待分析\n"
           "    v1 = LOAD_CONST #1 (2)       # 待分析\n"
           "    v2 = LOAD_CONST #2 (\"hello\")\n"
           "    v3 = STORE_LOCAL slot=0, v0   # x = 1 (从未使用)\n"
           "    CALL_PRINT v2                 # 副作用：打印\n"
           "    end block\n"
           "}\n",
           {"扫描所有指令的操作数 → 收集被引用的 vreg 集合 used = {v0 (by STORE_LOCAL), v2 (by CALL_PRINT)}",
            "标记 LOAD_CONST #1 (2) → v1：dest v1 未出现在任何指令的操作数中，且无副作用 → 候选删除",
            "保守保留 v0/v2/v3/CALL_PRINT — STORE_LOCAL 写入内存（可能被闭包/upvalue 捕获），CALL_PRINT 有 I/O 副作用"},
           5,
           1},
          {"🧹 DCE",
           "Round 2",
           "function main {\n"
           "  block L0:\n"
           "    v0 = LOAD_CONST #0 (1)       # 重新分析\n"
           "    v2 = LOAD_CONST #2 (\"hello\")\n"
           "    v3 = STORE_LOCAL slot=0, v0   # x = 1 (从未使用)\n"
           "    CALL_PRINT v2                 # 副作用：打印\n"
           "    end block\n"
           "}\n",
           {"删除 v1 = LOAD_CONST #1 (2) — 原因：dest v1 从未被后续指令引用，纯计算无副作用",
            "重新扫描：used = {v0 (by STORE_LOCAL), v2 (by CALL_PRINT)}",
            "发现 v3 = STORE_LOCAL slot=0, v0 — STORE_LOCAL 写入局部变量槽位，可能被后续代码或闭包捕获，保守保留"},
           4,
           1},
          {"🧹 DCE",
           "Round 3 (报告)",
           "function main {\n"
           "  block L0:\n"
           "    v0 = LOAD_CONST #0 (1)       # 保留\n"
           "    v2 = LOAD_CONST #2 (\"hello\")\n"
           "    v3 = STORE_LOCAL slot=0, v0   # 保留\n"
           "    CALL_PRINT v2                 # 保留\n"
           "    end block\n"
           "}\n",
           {"报告：本 round 共删除 1 条指令（v1 = LOAD_CONST #1 (2)）", "本轮 DCE 收敛 — 未发现新的可删除指令",
            "保留指令原因：v0 被 STORE_LOCAL 引用、v2 被 CALL_PRINT 引用、STORE_LOCAL 有写副作用、CALL_PRINT "
            "有打印副作用"},
           4,
           0}}},
        // ---- 3. 复制传播回放（3 步骤）----
        {"replay-copy-prop",
         {{"📋 复制传播",
           "Round 1",
           "function main {\n"
           "  block L0:\n"
           "    v0 = LOAD_LOCAL slot=0   # a\n"
           "    v1 = MOVE v0              # b = a (复制模式识别)\n"
           "    v2 = LOAD_CONST #0 (1)\n"
           "    v3 = ADD v1, v2           # b + 1 (待替换)\n"
           "    v4 = STORE_LOCAL slot=1, v3   # c = b + 1\n"
           "    end block\n"
           "}\n",
           {"扫描指令识别 MOVE 模式 → 建立 copy 链：v1 = MOVE v0 (即 v1 等价于 v0)",
            "记录 v1 的等价源为 v0 — 后续指令引用 v1 处可替换为 v0", "暂不修改 — 等待下一 round 执行替换"},
           5,
           1},
          {"📋 复制传播",
           "Round 2",
           "function main {\n"
           "  block L0:\n"
           "    v0 = LOAD_LOCAL slot=0   # a\n"
           "    v1 = MOVE v0              # b = a (待删除)\n"
           "    v2 = LOAD_CONST #0 (1)\n"
           "    v3 = ADD v0, v2           # a + 1 (v1 → v0 已替换)\n"
           "    v4 = STORE_LOCAL slot=1, v3   # c = a + 1\n"
           "    end block\n"
           "}\n",
           {"替换 v3 = ADD v1, v2 中操作数 v1 → v0 — 原因：v1 = MOVE v0 等价复制关系",
            "标记 v1 = MOVE v0 为待删除 — 替换后 v1 不再被任何指令引用",
            "本 round 未跨 STORE_LOCAL slot=0 传播 — STORE_LOCAL 可能修改 a 的值，传播链在此终止"},
           5,
           1},
          {"📋 复制传播",
           "Round 3 (DCE 二次清理)",
           "function main {\n"
           "  block L0:\n"
           "    v0 = LOAD_LOCAL slot=0   # a\n"
           "    v2 = LOAD_CONST #0 (1)\n"
           "    v3 = ADD v0, v2           # a + 1\n"
           "    v4 = STORE_LOCAL slot=1, v3   # c = a + 1\n"
           "    end block\n"
           "}\n",
           {"DCE 二次清理：删除 v1 = MOVE v0 — 原因：替换后 v1 不再被任何指令引用，纯复制指令无副作用",
            "指令数从 5 减少到 4 — 复制传播 + DCE 联合优化效果", "本 round 收敛 — 未发现新的 MOVE 模式"},
           4,
           1}}},
        // ---- 4. 公共子表达式消除回放（3 步骤）----
        {"replay-cse",
         {{"🔍 CSE",
           "Round 1",
           "function main {\n"
           "  block L0:\n"
           "    v0 = LOAD_LOCAL slot=0   # a\n"
           "    v1 = LOAD_LOCAL slot=1   # b\n"
           "    v2 = ADD v0, v1           # a + b (表达式首次出现)\n"
           "    v3 = STORE_LOCAL slot=2, v2   # x = a + b\n"
           "    v4 = LOAD_LOCAL slot=0   # a\n"
           "    v5 = LOAD_LOCAL slot=1   # b\n"
           "    v6 = ADD v4, v5           # a + b (重复表达式)\n"
           "    v7 = STORE_LOCAL slot=3, v6   # y = a + b\n"
           "    end block\n"
           "}\n",
           {"建立值编号表 (value numbering table) — hash((ADD, VIRTUAL, 0, VIRTUAL, 1)) → v2",
            "扫描 v6 = ADD v4, v5 — 发现 hash((ADD, VIRTUAL, 0, VIRTUAL, 1)) 与 v2 相同",
            "暂不修改 — 等待下一 round 执行引用替换"},
           8,
           1},
          {"🔍 CSE",
           "Round 2",
           "function main {\n"
           "  block L0:\n"
           "    v0 = LOAD_LOCAL slot=0   # a\n"
           "    v1 = LOAD_LOCAL slot=1   # b\n"
           "    v2 = ADD v0, v1           # a + b (canonical)\n"
           "    v3 = STORE_LOCAL slot=2, v2   # x = a + b\n"
           "    v4 = LOAD_LOCAL slot=0   # a\n"
           "    v5 = LOAD_LOCAL slot=1   # b\n"
           "    v6 = ADD v4, v5           # 待删除 (替换 v6 → v2)\n"
           "    v7 = STORE_LOCAL slot=3, v2   # y = a + b (v6 → v2 已替换)\n"
           "    end block\n"
           "}\n",
           {"替换 STORE_LOCAL slot=3, v6 中操作数 v6 → v2 — 原因：v6 = ADD(v4, v5) 与 v2 = ADD(v0, v1) 是相同表达式",
            "保留 v6 = ADD v4, v5 原指令 — 安全策略：算术指令可能触发除零/溢出副作用，不删除指令只替换引用",
            "值编号表更新 — (ADD, VIRTUAL, 4, VIRTUAL, 5) → v2 (canonical 引用)"},
           8,
           1},
          {"🔍 CSE",
           "Round 3 (标记待删除)",
           "function main {\n"
           "  block L0:\n"
           "    v0 = LOAD_LOCAL slot=0   # a\n"
           "    v1 = LOAD_LOCAL slot=1   # b\n"
           "    v2 = ADD v0, v1           # canonical\n"
           "    v3 = STORE_LOCAL slot=2, v2   # x = a + b\n"
           "    v7 = STORE_LOCAL slot=3, v2   # y = a + b\n"
           "    end block\n"
           "}\n",
           {"标记 v4 = LOAD_LOCAL slot=0、v5 = LOAD_LOCAL slot=1、v6 = ADD v4, v5 为待删除 — 引用已被替换为 v2/v0/v1",
            "DCE 二次清理：删除 v4/v5/v6 — 原因：dest 不再被任何指令引用，纯计算无副作用",
            "指令数从 8 减少到 5 — CSE + DCE 联合优化效果"},
           5,
           3}}},
        // ---- 5. 循环展开回放（3 步骤）----
        {"replay-loop-unroll",
         {{"🔄 循环展开",
           "Round 1",
           "function main {\n"
           "  block L0:                    # start (已标记为待展开)\n"
           "    v0 = LOAD_LOCAL slot=0   # i\n"
           "    v1 = LOAD_CONST #0 (3)     # N=3 (常量边界)\n"
           "    v2 = LT v0, v1             # i < 3\n"
           "    BRANCH_FALSE v2, L2        # 跳出循环\n"
           "  block L1:                    # body\n"
           "    v3 = LOAD_LOCAL slot=1   # acc\n"
           "    v4 = LOAD_CONST #1 (1)\n"
           "    v5 = ADD v3, v4            # acc + 1\n"
           "    v6 = STORE_LOCAL slot=1, v5   # acc = acc + 1\n"
           "    v7 = LOAD_LOCAL slot=0   # i\n"
           "    v8 = LOAD_CONST #1 (1)\n"
           "    v9 = ADD v7, v8            # i + 1\n"
           "    v10 = STORE_LOCAL slot=0, v9   # i = i + 1\n"
           "    JUMP L0\n"
           "  block L2:                    # end\n"
           "    end block\n"
           "}\n",
           {"识别模式：LABEL L0; cond_load; LOAD_CONST N; LT; JUMP_IF_FALSE L2; <body>; counter_load; LOAD_CONST 1; "
            "ADD; STORE_LOCAL; JUMP L0",
            "N=3 是常量且 1 ≤ N ≤ kMaxUnrollCount(4) → 可展开",
            "body 指令数 = 8 ≤ kMaxUnrollBodySize(20) → 满足安全门；body 不含 break/continue/return/throw → "
            "安全展开；标记循环结构（L0/L1/L2 共 13 条指令）为待展开"},
           13,
           1},
          {"🔄 循环展开",
           "Round 2",
           "function main {\n"
           "  block L0:                    # 展开第 1 次\n"
           "    v3 = LOAD_LOCAL slot=1   # acc\n"
           "    v4 = LOAD_CONST #1 (1)\n"
           "    v5 = ADD v3, v4            # acc + 1\n"
           "    v6 = STORE_LOCAL slot=1, v5   # acc = acc + 1\n"
           "    v7 = LOAD_LOCAL slot=0   # i\n"
           "    v8 = LOAD_CONST #1 (1)\n"
           "    v9 = ADD v7, v8            # i + 1\n"
           "    v10 = STORE_LOCAL slot=0, v9   # i = i + 1\n"
           "  block L1:                    # 展开第 2 次\n"
           "    v11 = LOAD_LOCAL slot=1   # acc\n"
           "    v12 = LOAD_CONST #1 (1)\n"
           "    v13 = ADD v11, v12         # acc + 1\n"
           "    v14 = STORE_LOCAL slot=1, v13   # acc = acc + 1\n"
           "    v15 = LOAD_LOCAL slot=0   # i\n"
           "    v16 = LOAD_CONST #1 (1)\n"
           "    v17 = ADD v15, v16         # i + 1\n"
           "    v18 = STORE_LOCAL slot=0, v17   # i = i + 1\n"
           "  block L2:                    # 展开第 3 次\n"
           "    v19 = LOAD_LOCAL slot=1   # acc\n"
           "    v20 = LOAD_CONST #1 (1)\n"
           "    v21 = ADD v19, v20         # acc + 1\n"
           "    v22 = STORE_LOCAL slot=1, v21   # acc = acc + 1\n"
           "    v23 = LOAD_LOCAL slot=0   # i\n"
           "    v24 = LOAD_CONST #1 (1)\n"
           "    v25 = ADD v23, v24         # i + 1\n"
           "    v26 = STORE_LOCAL slot=0, v25   # i = i + 1\n"
           "    end block\n"
           "}\n",
           {"展开 N=3 次循环体 → 生成 3 份顺序 body（L0/L1/L2）",
            "vreg 重命名：第 2/3 份 body 使用新分配的 v11~v26（避免与 v3~v10 冲突）",
            "保留所有 LOAD/STORE 指令 — 循环展开后变量生命周期分析需重新进行"},
           21,
           8},
          {"🔄 循环展开",
           "Round 3 (删除原循环结构)",
           "function main {\n"
           "  block L0:                    # 展开第 1 次\n"
           "    v3 = LOAD_LOCAL slot=1   # acc\n"
           "    v4 = LOAD_CONST #1 (1)\n"
           "    v5 = ADD v3, v4            # acc + 1\n"
           "    v6 = STORE_LOCAL slot=1, v5   # acc = acc + 1\n"
           "    v7 = LOAD_LOCAL slot=0   # i\n"
           "    v8 = LOAD_CONST #1 (1)\n"
           "    v9 = ADD v7, v8            # i + 1\n"
           "    v10 = STORE_LOCAL slot=0, v9   # i = i + 1\n"
           "  block L1:                    # 展开第 2 次\n"
           "    v11 = LOAD_LOCAL slot=1   # acc\n"
           "    v12 = LOAD_CONST #1 (1)\n"
           "    v13 = ADD v11, v12         # acc + 1\n"
           "    v14 = STORE_LOCAL slot=1, v13   # acc = acc + 1\n"
           "    v15 = LOAD_LOCAL slot=0   # i\n"
           "    v16 = LOAD_CONST #1 (1)\n"
           "    v17 = ADD v15, v16         # i + 1\n"
           "    v18 = STORE_LOCAL slot=0, v17   # i = i + 1\n"
           "  block L2:                    # 展开第 3 次\n"
           "    v19 = LOAD_LOCAL slot=1   # acc\n"
           "    v20 = LOAD_CONST #1 (1)\n"
           "    v21 = ADD v19, v20         # acc + 1\n"
           "    v22 = STORE_LOCAL slot=1, v21   # acc = acc + 1\n"
           "    v23 = LOAD_LOCAL slot=0   # i\n"
           "    v24 = LOAD_CONST #1 (1)\n"
           "    v25 = ADD v23, v24         # i + 1\n"
           "    v26 = STORE_LOCAL slot=0, v25   # i = i + 1\n"
           "    end block\n"
           "}\n",
           {"删除原循环结构 — v0 = LOAD_LOCAL slot=0 (i)、v1 = LOAD_CONST #0 (3)、v2 = LT v0, v1、BRANCH_FALSE v2, "
            "L2、JUMP L0 共 5 条控制流指令",
            "删除循环结束块 L2 的占位 — 原 block L2 已被展开后的第 3 次 body 替代",
            "指令数变化：展开前 13 条 → 展开后 21 条（+8 条 = 3×7 - 13，循环开销 5 条被删除，body 复制 3 份）"},
           21,
           5}}},
    };
    return kScenarios;
}

// ============================================================
// 第 4 子页：逐步优化回放
// ============================================================
//
// 布局：左侧场景列表 + 中间步骤列表 + 右上 IR 快照 + 右下决策列表
// 使用 QSplitter 嵌套布局：水平 splitter（场景 | 步骤 | 右侧垂直 splitter）

/// 构建「逐步优化回放」子页：左侧场景列表 + 中间 pass 步骤列表 +
/// 右上 IR 快照浏览器 + 右下决策解释列表（嵌套水平/垂直 splitter）。
/// 连接场景/步骤切换与锚点点击（跳转源码行）。
void IRTransformPanel::buildReplayPage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);

    replayStatusLabel_ = new QLabel(QString::fromUtf8("请选择场景"), host);

    // 左侧：场景列表
    auto* scenarioWrap = new QWidget(host);
    auto* scenarioLayout = new QVBoxLayout(scenarioWrap);
    scenarioLayout->setContentsMargins(0, 0, 0, 0);
    scenarioLayout->addWidget(new QLabel(QString::fromUtf8("场景列表："), scenarioWrap));
    replayList_ = new QListWidget(scenarioWrap);
    scenarioLayout->addWidget(replayList_, 1);

    // 中间：步骤列表
    auto* stepsWrap = new QWidget(host);
    auto* stepsLayout = new QVBoxLayout(stepsWrap);
    stepsLayout->setContentsMargins(0, 0, 0, 0);
    stepsLayout->addWidget(new QLabel(QString::fromUtf8("Pass 步骤："), stepsWrap));
    replayStepsList_ = new QListWidget(stepsWrap);
    stepsLayout->addWidget(replayStepsList_, 1);

    // 右上：IR 快照
    auto* irWrap = new QWidget(host);
    auto* irLayout = new QVBoxLayout(irWrap);
    irLayout->setContentsMargins(0, 0, 0, 0);
    irLayout->addWidget(new QLabel(QString::fromUtf8("IR 快照："), irWrap));
    replayIrBrowser_ = new QTextBrowser(irWrap);
    replayIrBrowser_->setFont(QFont("Consolas"));
    replayIrBrowser_->setOpenLinks(false);
    replayIrBrowser_->setOpenExternalLinks(false);
    irLayout->addWidget(replayIrBrowser_, 1);

    // 右下：决策列表
    auto* decisionsWrap = new QWidget(host);
    auto* decisionsLayout = new QVBoxLayout(decisionsWrap);
    decisionsLayout->setContentsMargins(0, 0, 0, 0);
    decisionsLayout->addWidget(new QLabel(QString::fromUtf8("决策解释："), decisionsWrap));
    replayDecisionsList_ = new QListWidget(decisionsWrap);
    decisionsLayout->addWidget(replayDecisionsList_, 1);

    // 右侧垂直 splitter（IR 上 | 决策 下）
    auto* rightSplitter = new QSplitter(Qt::Vertical, host);
    rightSplitter->addWidget(irWrap);
    rightSplitter->addWidget(decisionsWrap);
    rightSplitter->setStretchFactor(0, 1);
    rightSplitter->setStretchFactor(1, 1);
    rightSplitter->setSizes({250, 150});

    // 主水平 splitter（场景 | 步骤 | 右侧 splitter）
    auto* splitter = new QSplitter(Qt::Horizontal, host);
    splitter->addWidget(scenarioWrap);
    splitter->addWidget(stepsWrap);
    splitter->addWidget(rightSplitter);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    splitter->setStretchFactor(2, 2);
    splitter->setSizes({150, 150, 350});

    layout->addWidget(replayStatusLabel_);
    layout->addWidget(splitter, 1);

    // 场景列表切换 → 刷新步骤列表
    connect(replayList_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0)
            return;
        const auto& scenarios = IROptReplayLibrary::replayScenarios();
        if (row >= (int)scenarios.size())
            return;
        replayStepsList_->clear();
        const auto& steps = scenarios[row].second;
        for (const auto& s : steps) {
            QString text = QString::fromUtf8("[%1] %2 (修改 %3 条)")
                               .arg(QString::fromUtf8(s.passName.c_str()))
                               .arg(QString::fromUtf8(s.passRound.c_str()))
                               .arg(s.modifiedCount);
            replayStepsList_->addItem(text);
        }
        if (!steps.empty()) {
            replayStepsList_->setCurrentRow(0);
        }
        replayStatusLabel_->setText(QString::fromUtf8("场景：%1 — %2 个步骤")
                                        .arg(QString::fromUtf8(scenarios[row].first.c_str()))
                                        .arg(steps.size()));
        // 注：移除 fadeInWidget —— QListWidget 刷新无需动画，
        // QGraphicsOpacityEffect 会导致连续切换时 opacity 卡 0 内容空白。
    });

    // Click-to-highlight from replay IR browser
    connect(replayIrBrowser_, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
        QString fragment = url.fragment();
        if (fragment.startsWith("LINE_")) {
            bool ok = false;
            int line = fragment.mid(5).toInt(&ok);
            if (ok && line > 0) {
                emit sourceLineRequested(line);
            }
        }
    });

    // 步骤列表切换 → 刷新 IR 快照 + 决策列表
    connect(replayStepsList_, &QListWidget::currentRowChanged, this, [this](int stepRow) {
        int scenRow = replayList_->currentRow();
        if (scenRow < 0 || stepRow < 0)
            return;
        const auto& scenarios = IROptReplayLibrary::replayScenarios();
        if (scenRow >= (int)scenarios.size())
            return;
        const auto& steps = scenarios[scenRow].second;
        if (stepRow >= (int)steps.size())
            return;
        populateReplayStep(scenRow, stepRow);
    });
}

/// 用回放场景库的场景名填充左侧列表并默认选中首项。
void IRTransformPanel::populateReplayList() {
    if (!replayList_)
        return;
    replayList_->clear();
    const auto& scenarios = IROptReplayLibrary::replayScenarios();
    for (const auto& s : scenarios) {
        replayList_->addItem(QString::fromUtf8(s.first.c_str()));
    }
    if (!scenarios.empty()) {
        replayList_->setCurrentRow(0);
    }
}

/// 根据场景与步骤索引渲染回放详情：IR 快照（可点击）+ 决策解释列表 +
/// 状态标签（场景 / pass 名 / 轮次 / 指令数 / 修改数）。
void IRTransformPanel::populateReplayStep(int scenarioIdx, int stepIdx) {
    const auto& scenarios = IROptReplayLibrary::replayScenarios();
    if (scenarioIdx < 0 || scenarioIdx >= (int)scenarios.size())
        return;
    const auto& steps = scenarios[scenarioIdx].second;
    if (stepIdx < 0 || stepIdx >= (int)steps.size())
        return;
    const auto& s = steps[stepIdx];

    // IR 快照
    replayIrBrowser_->setHtml(irTextToClickableHtml(QString::fromUtf8(s.irSnapshot.c_str())));

    // 决策列表
    replayDecisionsList_->clear();
    for (const auto& d : s.decisions) {
        replayDecisionsList_->addItem(QString::fromUtf8(d.c_str()));
    }

    // 状态标签
    replayStatusLabel_->setText(QString::fromUtf8("场景：%1 — %2 / %3 — %4 条指令（修改 %5 条）")
                                    .arg(QString::fromUtf8(scenarios[scenarioIdx].first.c_str()))
                                    .arg(QString::fromUtf8(s.passName.c_str()))
                                    .arg(QString::fromUtf8(s.passRound.c_str()))
                                    .arg(s.instrCount)
                                    .arg(s.modifiedCount));

    // 注：移除 fadeInWidget —— QTextBrowser/QListWidget 内容刷新无需动画，
    // QGraphicsOpacityEffect 会导致连续切换步骤时 opacity 卡 0 内容空白。
}
