// ============================================================
// IRTransformPanel.cpp — IR 变换过程可视化面板实现（第二波 P1-1）
// ============================================================

#include "gui/IRTransformPanel.h"
#include "app/IdeController.h"
#include "compiler/IR.h"
#include "compiler/Compiler.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "gui/PanelAnimator.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QApplication>
#include <sstream>

// ============================================================
// IRTransformLibrary — 静态教学场景库
// ============================================================
//
// 注：场景库的 IR 文本采用 IRToString 一致的人类可读格式，
// 让学习者直观看到 AST 节点如何 lowering 为三地址码 IR 指令。
// 文本为教学示意，不依赖运行时 AstIRBuilder 实际产物。

const std::vector<IRLoweringExample>& IRTransformLibrary::loweringExamples() {
    static const std::vector<IRLoweringExample> kExamples = {
        {
            "lit-int",
            "整数字面量 42",
            "整数字面量直接 LOAD_CONST 加载到虚拟寄存器。"
            "常量 42 加入 IRFunction.constants 常量池（重复时复用索引）。",
            "NumberLiteral(42)",
            "var x = 42;",
            "function main {\n"
            "  block L0:\n"
            "    v0 = LOAD_CONST #0 (42)\n"
            "    v1 = STORE_LOCAL slot=0, v0   # x\n"
            "    end block\n"
            "}\n"
            "constants: [42]"
        },
        {
            "binop-add",
            "二元加法 a + b",
            "BinaryOp(ADD) lowering：左侧操作数 → vreg0，右侧 → vreg1，"
            "ADD 指令 → vreg2。三地址码形式 dest = OP src1, src2。",
            "BinaryOp(ADD, VarRef(a), VarRef(b))",
            "var c = a + b;",
            "function main {\n"
            "  block L0:\n"
            "    v0 = LOAD_LOCAL slot=0   # a\n"
            "    v1 = LOAD_LOCAL slot=1   # b\n"
            "    v2 = ADD v0, v1\n"
            "    v3 = STORE_LOCAL slot=2, v2   # c\n"
            "    end block\n"
            "}"
        },
        {
            "var-decl",
            "变量声明 var x = expr",
            "VarDecl lowering：先求值初始化表达式到 vreg，再 STORE_LOCAL 写入局部槽位。"
            "未初始化的 VarDecl 用 LOAD_NULL 占位。",
            "VarDecl(x, init=NumberLiteral(1))",
            "var x = 1;",
            "function main {\n"
            "  block L0:\n"
            "    v0 = LOAD_CONST #0 (1)\n"
            "    v1 = STORE_LOCAL slot=0, v0   # x\n"
            "    end block\n"
            "}"
        },
        {
            "if-stmt",
            "条件分支 if (cond) {...} else {...}",
            "IfStmt lowering：求值 cond → BRANCH_FALSE 跳到 L_else，"
            "执行 then 块后 JUMP L_end，else 块从 L_else 开始。"
            "patchJumps 解析跳转目标。",
            "IfStmt(cond=VarRef(x), then=Block, else=Block)",
            "if (x) { print 1; } else { print 2; }",
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
            "}"
        },
        {
            "while-stmt",
            "循环 while (cond) {...}",
            "WhileStmt lowering：L_start 处求值 cond → BRANCH_FALSE 跳到 L_end，"
            "执行循环体后 JUMP L_start。每个基本块终结于 BRANCH/JUMP/RETURN。",
            "WhileStmt(cond=BinaryOp(LT, VarRef(i), NumberLiteral(10)))",
            "while (i < 10) { i = i + 1; }",
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
            "}"
        },
        {
            "fun-call",
            "函数调用 f(a, b)",
            "FunCall lowering：每个实参求值到独立 vreg，CALL 指令携带函数索引 + 参数数量。"
            "返回值若被使用则存入 vreg，否则由 CALL_POP 弃用。",
            "FunCall(f, args=[VarRef(a), VarRef(b)])",
            "var r = f(a, b);",
            "function main {\n"
            "  block L0:\n"
            "    v0 = LOAD_LOCAL slot=0   # a\n"
            "    v1 = LOAD_LOCAL slot=1   # b\n"
            "    v2 = CALL func=#0, argc=2, args=[v0, v1]\n"
            "    v3 = STORE_LOCAL slot=2, v2   # r\n"
            "    end block\n"
            "}"
        },
        {
            "closure",
            "闭包捕获 makeCounter",
            "闭包 lowering：捕获的局部变量提升为 upvalue，MAKE_CLOSURE 指令携带捕获列表。"
            "ClosureData 通过 weak_ptr<Environment> 打破循环引用。",
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
            "}"
        },
        {
            "class-method",
            "类方法 Point.new()",
            "ClassDecl lowering：方法体作为独立 IRFunction 编译，"
            "slot 0 预留给隐式 this 参数。MethodCall 通过 GET_FIELD/SET_FIELD 访问实例字段。",
            "ClassDecl(Point) → methods=[new, distance]",
            "class Point {\n  var x;\n  var y;\n  fun new(px, py) { x = px; y = py; }\n}",
            "function Point::new {\n"
            "  # slot 0 = this (隐式)\n"
            "  block L0:\n"
            "    v0 = LOAD_LOCAL slot=1   # px\n"
            "    v1 = SET_FIELD this=slot0, name=#0 (x), v0\n"
            "    v2 = LOAD_LOCAL slot=2   # py\n"
            "    v3 = SET_FIELD this=slot0, name=#1 (y), v2\n"
            "    RETURN_NULL\n"
            "}"
        },
    };
    return kExamples;
}

const std::vector<IROptimizationExample>& IRTransformLibrary::optimizationExamples() {
    static const std::vector<IROptimizationExample> kExamples = {
        {
            "const-fold",
            "常量折叠 1 + 2 → 3",
            "常量折叠 pass 识别 LOAD_CONST 操作数全为常量的算术指令，"
            "在编译期求值替换为单一 LOAD_CONST。"
            "适用于 int/float 加减乘除、布尔逻辑、字符串拼接等。",
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
            4, 2
        },
        {
            "dead-code",
            "死代码消除（未使用的赋值）",
            "DCE pass 识别结果未被使用的指令（无副作用），直接消除。"
            "本例 x = 1 后从未读取 x，整条赋值链可消除。",
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
            4, 2
        },
        {
            "copy-prop",
            "复制传播 + 跳转优化",
            "复制传播 pass 识别 v_b = MOVE v_a 模式，将后续 v_b 的引用替换为 v_a。"
            "消除 MOVE 后常触发 DCE 二次优化，进一步减少指令。",
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
            5, 4
        },
    };
    return kExamples;
}

// ============================================================
// IRTransformPanel 实现
// ============================================================

IRTransformPanel::IRTransformPanel(QWidget* parent)
    : QWidget(parent) {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(4);

    auto* pageBar = new QHBoxLayout;
    pageLoweringBtn_ = new QPushButton(QString::fromUtf8("AST → IR lowering"), this);
    pageOptimizeBtn_ = new QPushButton(QString::fromUtf8("优化 pass 对比"), this);
    pageCurrentBtn_   = new QPushButton(QString::fromUtf8("当前源码 IR"), this);
    pageLoweringBtn_->setCheckable(true);
    pageOptimizeBtn_->setCheckable(true);
    pageCurrentBtn_->setCheckable(true);
    pageBar->addWidget(pageLoweringBtn_);
    pageBar->addWidget(pageOptimizeBtn_);
    pageBar->addWidget(pageCurrentBtn_);
    pageBar->addStretch();
    mainLayout->addLayout(pageBar);

    stack_ = new QStackedWidget(this);
    mainLayout->addWidget(stack_, 1);

    auto* p1 = new QWidget(this);
    auto* p2 = new QWidget(this);
    auto* p3 = new QWidget(this);
    buildLoweringPage(p1);
    buildOptimizePage(p2);
    buildCurrentPage(p3);
    stack_->addWidget(p1);
    stack_->addWidget(p2);
    stack_->addWidget(p3);

    pageLoweringBtn_->setChecked(true);
    stack_->setCurrentIndex(0);

    connect(pageLoweringBtn_, &QPushButton::clicked, this, [this]() {
        stack_->setCurrentIndex(0);
        pageLoweringBtn_->setChecked(true);
        pageOptimizeBtn_->setChecked(false);
        pageCurrentBtn_->setChecked(false);
        PanelAnimator::fadeInWidget(stack_->currentWidget());
    });
    connect(pageOptimizeBtn_, &QPushButton::clicked, this, [this]() {
        stack_->setCurrentIndex(1);
        pageLoweringBtn_->setChecked(false);
        pageOptimizeBtn_->setChecked(true);
        pageCurrentBtn_->setChecked(false);
        PanelAnimator::fadeInWidget(stack_->currentWidget());
    });
    connect(pageCurrentBtn_, &QPushButton::clicked, this, [this]() {
        stack_->setCurrentIndex(2);
        pageLoweringBtn_->setChecked(false);
        pageOptimizeBtn_->setChecked(false);
        pageCurrentBtn_->setChecked(true);
        populateCurrentIR();
        PanelAnimator::fadeInWidget(stack_->currentWidget());
    });

    populateLoweringList();
    populateOptList();
}

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

    connect(loweringList_, &QListWidget::currentRowChanged,
            this, &IRTransformPanel::populateLoweringDetail);
}

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

    connect(optList_, &QListWidget::currentRowChanged,
            this, &IRTransformPanel::populateOptDetail);
}

void IRTransformPanel::buildCurrentPage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* toolbar = new QHBoxLayout;
    refreshBtn_ = new QPushButton(QString::fromUtf8("重新生成 IR"), host);
    currentStatusLabel_ = new QLabel(QString::fromUtf8("尚未生成"), host);
    toolbar->addWidget(refreshBtn_);
    toolbar->addStretch();
    toolbar->addWidget(currentStatusLabel_);
    layout->addLayout(toolbar);

    currentIrBrowser_ = new QTextBrowser(host);
    currentIrBrowser_->setFont(QFont("Consolas"));
    layout->addWidget(currentIrBrowser_, 1);

    connect(refreshBtn_, &QPushButton::clicked, this, [this]() {
        populateCurrentIR();
    });
}

void IRTransformPanel::reloadCurrentIR() {
    if (stack_->currentIndex() == 2) {
        populateCurrentIR();
    }
}

void IRTransformPanel::populateLoweringList() {
    loweringList_->clear();
    const auto& items = IRTransformLibrary::loweringExamples();
    for (const auto& e : items) {
        loweringList_->addItem(QString::fromUtf8(e.title.c_str()));
    }
    if (!items.empty()) loweringList_->setCurrentRow(0);
}

void IRTransformPanel::populateLoweringDetail(int index) {
    const auto& items = IRTransformLibrary::loweringExamples();
    if (index < 0 || index >= (int)items.size()) return;
    const auto& e = items[index];

    std::ostringstream os;
    os << "<h3>" << e.title << "</h3>";
    os << "<p><b>AST 节点：</b> <code>" << e.astSummary << "</code></p>";
    os << "<p><b>源码：</b> <code>" << e.sourceCode << "</code></p>";
    os << "<hr>";
    os << "<p>" << e.description << "</p>";
    os << "<h4>Lowering 后的 IR：</h4>";
    os << "<pre style='background:#f5f5f5; padding:8px; font-family:Consolas;'>" << e.irBefore << "</pre>";
    loweringDetail_->setHtml(QString::fromUtf8(os.str().c_str()));
    PanelAnimator::fadeInWidget(loweringDetail_);
}

void IRTransformPanel::populateOptList() {
    optList_->clear();
    const auto& items = IRTransformLibrary::optimizationExamples();
    for (const auto& e : items) {
        QString text = QString::fromUtf8("[%1] %2")
            .arg(QString::fromUtf8(e.passName.c_str()))
            .arg(QString::fromUtf8(e.title.c_str()));
        optList_->addItem(text);
    }
    if (!items.empty()) optList_->setCurrentRow(0);
}

void IRTransformPanel::populateOptDetail(int index) {
    const auto& items = IRTransformLibrary::optimizationExamples();
    if (index < 0 || index >= (int)items.size()) return;
    const auto& e = items[index];

    optSummary_->setText(QString::fromUtf8("%1 — %2 → %3 条指令（减少 %4 条）")
        .arg(QString::fromUtf8(e.title.c_str()))
        .arg(e.instrBefore)
        .arg(e.instrAfter)
        .arg(e.instrBefore - e.instrAfter));

    optBefore_->setPlainText(QString::fromUtf8(e.irBefore.c_str()));
    optAfter_->setPlainText(QString::fromUtf8(e.irAfter.c_str()));
    PanelAnimator::fadeInWidget(optBefore_);
    PanelAnimator::fadeInWidget(optAfter_);
}

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
        currentIrBrowser_->setPlainText(QString::fromUtf8(irText.c_str()));
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
