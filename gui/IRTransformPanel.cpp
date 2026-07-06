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
#include <utility>  // std::pair for IROptReplayLibrary::replayScenarios()

#include "PushButton.h"   // QFluentKit（PrimaryPushButton）
#include "Label.h"        // QFluentKit（CaptionLabel）
#include "gui/TeachingTheme.h"
#include "Theme.h"        // QFluentKit（onThemeModeChanged 信号）

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
            "🔢 整数字面量 42",
            "🔢 整数字面量直接 LOAD_CONST 加载到虚拟寄存器。"
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
            "⚙️ 二元加法 a + b",
            "⚙️ BinaryOp(ADD) lowering：左侧操作数 → vreg0，右侧 → vreg1，"
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
            "📝 变量声明 var x = expr",
            "📝 VarDecl lowering：先求值初始化表达式到 vreg，再 STORE_LOCAL 写入局部槽位。"
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
            "🔀 条件分支 if (cond) {...} else {...}",
            "🔀 IfStmt lowering：求值 cond → BRANCH_FALSE 跳到 L_else，"
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
            "🔄 循环 while (cond) {...}",
            "🔄 WhileStmt lowering：L_start 处求值 cond → BRANCH_FALSE 跳到 L_end，"
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
            "⚙️ 函数调用 f(a, b)",
            "⚙️ FunCall lowering：每个实参求值到独立 vreg，CALL 指令携带函数索引 + 参数数量。"
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
            "📦 闭包捕获 makeCounter",
            "📦 闭包 lowering：捕获的局部变量提升为 upvalue，MAKE_CLOSURE 指令携带捕获列表。"
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
            "🏛️ 类方法 Point.new()",
            "🏛️ ClassDecl lowering：方法体作为独立 IRFunction 编译，"
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
            "✨ 常量折叠 1 + 2 → 3",
            "✨ 常量折叠 pass 识别 LOAD_CONST 操作数全为常量的算术指令，"
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
            "🧹 死代码消除（未使用的赋值）",
            "🧹 DCE pass 识别结果未被使用的指令（无副作用），直接消除。"
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
            "📋 复制传播 + 跳转优化",
            "📋 复制传播 pass 识别 v_b = MOVE v_a 模式，将后续 v_b 的引用替换为 v_a。"
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
    pageReplayBtn_    = new QPushButton(QString::fromUtf8("逐步优化回放"), this);
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
        PanelAnimator::fadeInWidget(stack_->currentWidget());
    });
    connect(pageOptimizeBtn_, &QPushButton::clicked, this, [this]() {
        stack_->setCurrentIndex(1);
        pageLoweringBtn_->setChecked(false);
        pageOptimizeBtn_->setChecked(true);
        pageCurrentBtn_->setChecked(false);
        pageReplayBtn_->setChecked(false);
        PanelAnimator::fadeInWidget(stack_->currentWidget());
    });
    connect(pageCurrentBtn_, &QPushButton::clicked, this, [this]() {
        stack_->setCurrentIndex(2);
        pageLoweringBtn_->setChecked(false);
        pageOptimizeBtn_->setChecked(false);
        pageCurrentBtn_->setChecked(true);
        pageReplayBtn_->setChecked(false);
        populateCurrentIR();
        PanelAnimator::fadeInWidget(stack_->currentWidget());
    });
    connect(pageReplayBtn_, &QPushButton::clicked, this, [this]() {
        stack_->setCurrentIndex(3);
        pageLoweringBtn_->setChecked(false);
        pageOptimizeBtn_->setChecked(false);
        pageCurrentBtn_->setChecked(false);
        pageReplayBtn_->setChecked(true);
        PanelAnimator::fadeInWidget(stack_->currentWidget());
    });

    populateLoweringList();
    populateOptList();
    populateReplayList();

    // 主题切换时刷新当前页 HTML（populateLoweringDetail 中 <pre> 背景使用
    // TeachingTheme::surface()，需重新渲染以跟随新主题）。
    // receiver=this 保证生命周期安全，析构自动断开。
    Theme::onThemeModeChanged(this, [this](Fluent::ThemeMode) {
        if (!stack_) return;
        switch (stack_->currentIndex()) {
            case 0:  // AST → IR lowering 页（HTML 含主题色 <pre> 背景）
                if (loweringList_) populateLoweringDetail(loweringList_->currentRow());
                break;
            case 1:  // 优化 pass 对比页（setPlainText，无主题依赖，仍刷新以防未来扩展）
                if (optList_) populateOptDetail(optList_->currentRow());
                break;
            case 2:  // 当前源码 IR 页（setPlainText，无主题依赖）
                populateCurrentIR();
                break;
            case 3:  // 逐步优化回放页（setPlainText，无主题依赖）
                if (replayList_ && replayStepsList_)
                    populateReplayStep(replayList_->currentRow(), replayStepsList_->currentRow());
                break;
            default:
                break;
        }
    });
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
    refreshBtn_ = new PrimaryPushButton(QString::fromUtf8("重新生成 IR"), host);
    currentStatusLabel_ = new CaptionLabel(QString::fromUtf8("尚未生成"), host);
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
    os << "<pre style='background:" << TeachingTheme::surface().name().toStdString()
       << "; padding:8px; font-family:Consolas;'>" << e.irBefore << "</pre>";
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

// ============================================================
// IROptReplayLibrary — 逐步优化回放场景库
// ============================================================
//
// 注：每个场景包含 3 个 IROptStepRecord，逐步回放一个优化 pass 的执行轨迹。
// irSnapshot 采用与 IRToString 一致的人类可读格式（含 "function" 关键字），
// decisions 字段含具体指令级别的修改/删除原因，便于学习者理解 pass 内部行为。

const std::vector<std::pair<std::string, std::vector<IROptStepRecord>>>&
IROptReplayLibrary::replayScenarios() {
    static const std::vector<std::pair<std::string, std::vector<IROptStepRecord>>> kScenarios = {
        // ---- 1. 常量折叠回放（3 步骤）----
        {
            "replay-const-fold",
            {
                {
                    "✨ 常量折叠", "Round 1",
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
                    {
                        "v2 = ADD v0, v1 → LOAD_CONST #2 (3) — 折叠原因：v0=LOAD_CONST #0 (1), v1=LOAD_CONST #1 (2) 均为常量加载",
                        "保留 v3 = LOAD_CONST #3 (4), v4 = LOAD_CONST #4 (4) — MUL 操作数均为常量但下一轮再折叠（保守策略，避免一次折叠破坏数据流分析）",
                        "v0/v1 暂不删除 — DCE pass 会在后续步骤中清理无引用的纯计算指令"
                    },
                    8, 1
                },
                {
                    "✨ 常量折叠", "Round 2",
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
                    {
                        "v5 = MUL v3, v4 → LOAD_CONST #5 (12) — 折叠原因：v3=LOAD_CONST #3 (3), v4=LOAD_CONST #4 (4) 均为常量加载",
                        "新增常量 #5 (12) 加入常量池（重复时复用索引）",
                        "v3/v4 暂不删除 — 等待 DCE pass 清理"
                    },
                    8, 1
                },
                {
                    "✨ 常量折叠", "Round 3 (收敛)",
                    "function main {\n"
                    "  block L0:\n"
                    "    v2 = LOAD_CONST #2 (3)        # 已折叠\n"
                    "    v5 = LOAD_CONST #5 (12)       # 已折叠\n"
                    "    v6 = STORE_LOCAL slot=0, v5   # x = 12\n"
                    "    end block\n"
                    "}\n",
                    {
                        "本 round 未发生折叠 — IR 已收敛（无更多可折叠的算术指令）",
                        "下一阶段：DCE pass 将清理 v0/v1/v3/v4 等无引用的 LOAD_CONST"
                    },
                    3, 0
                }
            }
        },
        // ---- 2. 死代码消除回放（3 步骤）----
        {
            "replay-dce",
            {
                {
                    "🧹 DCE", "Round 1",
                    "function main {\n"
                    "  block L0:\n"
                    "    v0 = LOAD_CONST #0 (1)       # 待分析\n"
                    "    v1 = LOAD_CONST #1 (2)       # 待分析\n"
                    "    v2 = LOAD_CONST #2 (\"hello\")\n"
                    "    v3 = STORE_LOCAL slot=0, v0   # x = 1 (从未使用)\n"
                    "    CALL_PRINT v2                 # 副作用：打印\n"
                    "    end block\n"
                    "}\n",
                    {
                        "扫描所有指令的操作数 → 收集被引用的 vreg 集合 used = {v0 (by STORE_LOCAL), v2 (by CALL_PRINT)}",
                        "标记 LOAD_CONST #1 (2) → v1：dest v1 未出现在任何指令的操作数中，且无副作用 → 候选删除",
                        "保守保留 v0/v2/v3/CALL_PRINT — STORE_LOCAL 写入内存（可能被闭包/upvalue 捕获），CALL_PRINT 有 I/O 副作用"
                    },
                    5, 1
                },
                {
                    "🧹 DCE", "Round 2",
                    "function main {\n"
                    "  block L0:\n"
                    "    v0 = LOAD_CONST #0 (1)       # 重新分析\n"
                    "    v2 = LOAD_CONST #2 (\"hello\")\n"
                    "    v3 = STORE_LOCAL slot=0, v0   # x = 1 (从未使用)\n"
                    "    CALL_PRINT v2                 # 副作用：打印\n"
                    "    end block\n"
                    "}\n",
                    {
                        "删除 v1 = LOAD_CONST #1 (2) — 原因：dest v1 从未被后续指令引用，纯计算无副作用",
                        "重新扫描：used = {v0 (by STORE_LOCAL), v2 (by CALL_PRINT)}",
                        "发现 v3 = STORE_LOCAL slot=0, v0 — STORE_LOCAL 写入局部变量槽位，可能被后续代码或闭包捕获，保守保留"
                    },
                    4, 1
                },
                {
                    "🧹 DCE", "Round 3 (报告)",
                    "function main {\n"
                    "  block L0:\n"
                    "    v0 = LOAD_CONST #0 (1)       # 保留\n"
                    "    v2 = LOAD_CONST #2 (\"hello\")\n"
                    "    v3 = STORE_LOCAL slot=0, v0   # 保留\n"
                    "    CALL_PRINT v2                 # 保留\n"
                    "    end block\n"
                    "}\n",
                    {
                        "报告：本 round 共删除 1 条指令（v1 = LOAD_CONST #1 (2)）",
                        "本轮 DCE 收敛 — 未发现新的可删除指令",
                        "保留指令原因：v0 被 STORE_LOCAL 引用、v2 被 CALL_PRINT 引用、STORE_LOCAL 有写副作用、CALL_PRINT 有打印副作用"
                    },
                    4, 0
                }
            }
        },
        // ---- 3. 复制传播回放（3 步骤）----
        {
            "replay-copy-prop",
            {
                {
                    "📋 复制传播", "Round 1",
                    "function main {\n"
                    "  block L0:\n"
                    "    v0 = LOAD_LOCAL slot=0   # a\n"
                    "    v1 = MOVE v0              # b = a (复制模式识别)\n"
                    "    v2 = LOAD_CONST #0 (1)\n"
                    "    v3 = ADD v1, v2           # b + 1 (待替换)\n"
                    "    v4 = STORE_LOCAL slot=1, v3   # c = b + 1\n"
                    "    end block\n"
                    "}\n",
                    {
                        "扫描指令识别 MOVE 模式 → 建立 copy 链：v1 = MOVE v0 (即 v1 等价于 v0)",
                        "记录 v1 的等价源为 v0 — 后续指令引用 v1 处可替换为 v0",
                        "暂不修改 — 等待下一 round 执行替换"
                    },
                    5, 1
                },
                {
                    "📋 复制传播", "Round 2",
                    "function main {\n"
                    "  block L0:\n"
                    "    v0 = LOAD_LOCAL slot=0   # a\n"
                    "    v1 = MOVE v0              # b = a (待删除)\n"
                    "    v2 = LOAD_CONST #0 (1)\n"
                    "    v3 = ADD v0, v2           # a + 1 (v1 → v0 已替换)\n"
                    "    v4 = STORE_LOCAL slot=1, v3   # c = a + 1\n"
                    "    end block\n"
                    "}\n",
                    {
                        "替换 v3 = ADD v1, v2 中操作数 v1 → v0 — 原因：v1 = MOVE v0 等价复制关系",
                        "标记 v1 = MOVE v0 为待删除 — 替换后 v1 不再被任何指令引用",
                        "本 round 未跨 STORE_LOCAL slot=0 传播 — STORE_LOCAL 可能修改 a 的值，传播链在此终止"
                    },
                    5, 1
                },
                {
                    "📋 复制传播", "Round 3 (DCE 二次清理)",
                    "function main {\n"
                    "  block L0:\n"
                    "    v0 = LOAD_LOCAL slot=0   # a\n"
                    "    v2 = LOAD_CONST #0 (1)\n"
                    "    v3 = ADD v0, v2           # a + 1\n"
                    "    v4 = STORE_LOCAL slot=1, v3   # c = a + 1\n"
                    "    end block\n"
                    "}\n",
                    {
                        "DCE 二次清理：删除 v1 = MOVE v0 — 原因：替换后 v1 不再被任何指令引用，纯复制指令无副作用",
                        "指令数从 5 减少到 4 — 复制传播 + DCE 联合优化效果",
                        "本 round 收敛 — 未发现新的 MOVE 模式"
                    },
                    4, 1
                }
            }
        },
        // ---- 4. 公共子表达式消除回放（3 步骤）----
        {
            "replay-cse",
            {
                {
                    "🔍 CSE", "Round 1",
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
                    {
                        "建立值编号表 (value numbering table) — hash((ADD, VIRTUAL, 0, VIRTUAL, 1)) → v2",
                        "扫描 v6 = ADD v4, v5 — 发现 hash((ADD, VIRTUAL, 0, VIRTUAL, 1)) 与 v2 相同",
                        "暂不修改 — 等待下一 round 执行引用替换"
                    },
                    8, 1
                },
                {
                    "🔍 CSE", "Round 2",
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
                    {
                        "替换 STORE_LOCAL slot=3, v6 中操作数 v6 → v2 — 原因：v6 = ADD(v4, v5) 与 v2 = ADD(v0, v1) 是相同表达式",
                        "保留 v6 = ADD v4, v5 原指令 — 安全策略：算术指令可能触发除零/溢出副作用，不删除指令只替换引用",
                        "值编号表更新 — (ADD, VIRTUAL, 4, VIRTUAL, 5) → v2 (canonical 引用)"
                    },
                    8, 1
                },
                {
                    "🔍 CSE", "Round 3 (标记待删除)",
                    "function main {\n"
                    "  block L0:\n"
                    "    v0 = LOAD_LOCAL slot=0   # a\n"
                    "    v1 = LOAD_LOCAL slot=1   # b\n"
                    "    v2 = ADD v0, v1           # canonical\n"
                    "    v3 = STORE_LOCAL slot=2, v2   # x = a + b\n"
                    "    v7 = STORE_LOCAL slot=3, v2   # y = a + b\n"
                    "    end block\n"
                    "}\n",
                    {
                        "标记 v4 = LOAD_LOCAL slot=0、v5 = LOAD_LOCAL slot=1、v6 = ADD v4, v5 为待删除 — 引用已被替换为 v2/v0/v1",
                        "DCE 二次清理：删除 v4/v5/v6 — 原因：dest 不再被任何指令引用，纯计算无副作用",
                        "指令数从 8 减少到 5 — CSE + DCE 联合优化效果"
                    },
                    5, 3
                }
            }
        },
        // ---- 5. 循环展开回放（3 步骤）----
        {
            "replay-loop-unroll",
            {
                {
                    "🔄 循环展开", "Round 1",
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
                    {
                        "识别模式：LABEL L0; cond_load; LOAD_CONST N; LT; JUMP_IF_FALSE L2; <body>; counter_load; LOAD_CONST 1; ADD; STORE_LOCAL; JUMP L0",
                        "N=3 是常量且 1 ≤ N ≤ kMaxUnrollCount(4) → 可展开",
                        "body 指令数 = 8 ≤ kMaxUnrollBodySize(20) → 满足安全门；body 不含 break/continue/return/throw → 安全展开；标记循环结构（L0/L1/L2 共 13 条指令）为待展开"
                    },
                    13, 1
                },
                {
                    "🔄 循环展开", "Round 2",
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
                    {
                        "展开 N=3 次循环体 → 生成 3 份顺序 body（L0/L1/L2）",
                        "vreg 重命名：第 2/3 份 body 使用新分配的 v11~v26（避免与 v3~v10 冲突）",
                        "保留所有 LOAD/STORE 指令 — 循环展开后变量生命周期分析需重新进行"
                    },
                    21, 8
                },
                {
                    "🔄 循环展开", "Round 3 (删除原循环结构)",
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
                    {
                        "删除原循环结构 — v0 = LOAD_LOCAL slot=0 (i)、v1 = LOAD_CONST #0 (3)、v2 = LT v0, v1、BRANCH_FALSE v2, L2、JUMP L0 共 5 条控制流指令",
                        "删除循环结束块 L2 的占位 — 原 block L2 已被展开后的第 3 次 body 替代",
                        "指令数变化：展开前 13 条 → 展开后 21 条（+8 条 = 3×7 - 13，循环开销 5 条被删除，body 复制 3 份）"
                    },
                    21, 5
                }
            }
        },
    };
    return kScenarios;
}

// ============================================================
// 第 4 子页：逐步优化回放
// ============================================================
//
// 布局：左侧场景列表 + 中间步骤列表 + 右上 IR 快照 + 右下决策列表
// 使用 QSplitter 嵌套布局：水平 splitter（场景 | 步骤 | 右侧垂直 splitter）

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
        if (row < 0) return;
        const auto& scenarios = IROptReplayLibrary::replayScenarios();
        if (row >= (int)scenarios.size()) return;
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
        PanelAnimator::fadeInWidget(replayStepsList_);
    });

    // 步骤列表切换 → 刷新 IR 快照 + 决策列表
    connect(replayStepsList_, &QListWidget::currentRowChanged, this, [this](int stepRow) {
        int scenRow = replayList_->currentRow();
        if (scenRow < 0 || stepRow < 0) return;
        const auto& scenarios = IROptReplayLibrary::replayScenarios();
        if (scenRow >= (int)scenarios.size()) return;
        const auto& steps = scenarios[scenRow].second;
        if (stepRow >= (int)steps.size()) return;
        populateReplayStep(scenRow, stepRow);
    });
}

void IRTransformPanel::populateReplayList() {
    if (!replayList_) return;
    replayList_->clear();
    const auto& scenarios = IROptReplayLibrary::replayScenarios();
    for (const auto& s : scenarios) {
        replayList_->addItem(QString::fromUtf8(s.first.c_str()));
    }
    if (!scenarios.empty()) {
        replayList_->setCurrentRow(0);
    }
}

void IRTransformPanel::populateReplayStep(int scenarioIdx, int stepIdx) {
    const auto& scenarios = IROptReplayLibrary::replayScenarios();
    if (scenarioIdx < 0 || scenarioIdx >= (int)scenarios.size()) return;
    const auto& steps = scenarios[scenarioIdx].second;
    if (stepIdx < 0 || stepIdx >= (int)steps.size()) return;
    const auto& s = steps[stepIdx];

    // IR 快照
    replayIrBrowser_->setPlainText(QString::fromUtf8(s.irSnapshot.c_str()));

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

    PanelAnimator::fadeInWidget(replayIrBrowser_);
    PanelAnimator::fadeInWidget(replayDecisionsList_);
}
