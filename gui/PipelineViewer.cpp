#include "gui/PipelineViewer.h"
#include "app/IdeController.h"
#include "lexer/Lexer.h"
#include "lexer/Token.h"
#include "parser/Parser.h"
#include "compiler/Compiler.h"
#include "compiler/Bytecode.h"
#include "compiler/IR.h"
#include "ast/ASTNode.h"
#include "gui/PanelAnimator.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTextBrowser>
#include <QLabel>
#include <QHeaderView>
#include <sstream>
#include <cstring>

PipelineViewer::PipelineViewer(QWidget* parent)
    : QWidget(parent) {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(4);

    // 步骤导航条
    auto* stepBar = new QWidget(this);
    auto* stepLayout = new QHBoxLayout(stepBar);
    stepLayout->setContentsMargins(0, 0, 0, 0);
    stepLayout->setSpacing(4);

    auto makeStepBtn = [this](const QString& text, int step) {
        auto* btn = new QPushButton(text, this);
        btn->setCheckable(true);
        btn->setMinimumWidth(80);
        connect(btn, &QPushButton::clicked, this, [this, step]() { switchToStep(step); });
        return btn;
    };

    stepSourceBtn_   = makeStepBtn(QString::fromUtf8("1. 源码"), 0);
    stepTokenBtn_    = makeStepBtn(QString::fromUtf8("2. Token"), 1);
    stepAstBtn_      = makeStepBtn(QString::fromUtf8("3. AST"), 2);
    stepIrBtn_       = makeStepBtn(QString::fromUtf8("4. IR"), 3);
    stepBytecodeBtn_ = makeStepBtn(QString::fromUtf8("5. 字节码"), 4);

    stepLayout->addWidget(stepSourceBtn_);
    stepLayout->addWidget(new QLabel(QString::fromUtf8("→"), this));
    stepLayout->addWidget(stepTokenBtn_);
    stepLayout->addWidget(new QLabel(QString::fromUtf8("→"), this));
    stepLayout->addWidget(stepAstBtn_);
    stepLayout->addWidget(new QLabel(QString::fromUtf8("→"), this));
    stepLayout->addWidget(stepIrBtn_);
    stepLayout->addWidget(new QLabel(QString::fromUtf8("→"), this));
    stepLayout->addWidget(stepBytecodeBtn_);
    stepLayout->addStretch();

    mainLayout->addWidget(stepBar);

    // 主体：QStackedWidget
    stack_ = new QStackedWidget(this);
    sourceBrowser_ = new QTextBrowser(this);
    sourceBrowser_->setFont(QFont("Consolas"));
    tokenTable_ = new QTableWidget(this);
    tokenTable_->setColumnCount(5);
    tokenTable_->setHorizontalHeaderLabels({
        QString::fromUtf8("#"),
        QString::fromUtf8("类型"),
        QString::fromUtf8("字面文本"),
        QString::fromUtf8("行"),
        QString::fromUtf8("列")
    });
    tokenTable_->horizontalHeader()->setStretchLastSection(true);
    tokenTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);

    astSummary_ = new QTextBrowser(this);
    astSummary_->setFont(QFont("Consolas"));

    irBrowser_ = new QTextBrowser(this);
    irBrowser_->setFont(QFont("Consolas"));

    bytecodeBrowser_ = new QTextBrowser(this);
    bytecodeBrowser_->setFont(QFont("Consolas"));

    stack_->addWidget(sourceBrowser_);
    stack_->addWidget(tokenTable_);
    stack_->addWidget(astSummary_);
    stack_->addWidget(irBrowser_);
    stack_->addWidget(bytecodeBrowser_);
    mainLayout->addWidget(stack_, 1);

    // 底部状态条
    statusLabel_ = new QLabel(QString::fromUtf8("步骤: 源码 | 光标: 行 1, 列 1"), this);
    mainLayout->addWidget(statusLabel_);

    switchToStep(0);
}

void PipelineViewer::switchToStep(int step) {
    if (step < 0 || step >= 5) return;
    currentStep_ = step;
    stack_->setCurrentIndex(step);

    stepSourceBtn_->setChecked(step == 0);
    stepTokenBtn_->setChecked(step == 1);
    stepAstBtn_->setChecked(step == 2);
    stepIrBtn_->setChecked(step == 3);
    stepBytecodeBtn_->setChecked(step == 4);

    reloadCurrentStep();
    // 第四档 P2-5：管线步骤切换淡入动画
    PanelAnimator::fadeInWidget(stack_->currentWidget());
}

void PipelineViewer::reloadCurrentStep() {
    switch (currentStep_) {
        case 0: populateSource(); break;
        case 1: populateTokens(); break;
        case 2: populateAstSummary(); break;
        case 3: populateIR(); break;
        case 4: populateBytecode(); break;
    }
    QStringList stepNames = {
        QString::fromUtf8("源码"), QString::fromUtf8("Token"),
        QString::fromUtf8("AST"), QString::fromUtf8("IR"),
        QString::fromUtf8("字节码")
    };
    statusLabel_->setText(QString::fromUtf8("步骤: %1 | 光标: 行 %2, 列 %3")
        .arg(stepNames.value(currentStep_))
        .arg(cursorLine_).arg(cursorColumn_));
}

void PipelineViewer::onCursorPositionChanged(int line, int column) {
    cursorLine_ = line;
    cursorColumn_ = column;
    if (statusLabel_) {
        QStringList stepNames = {
            QString::fromUtf8("源码"), QString::fromUtf8("Token"),
            QString::fromUtf8("AST"), QString::fromUtf8("IR"),
            QString::fromUtf8("字节码")
        };
        statusLabel_->setText(QString::fromUtf8("步骤: %1 | 光标: 行 %2, 列 %3")
            .arg(stepNames.value(currentStep_))
            .arg(line).arg(column));
    }
}

void PipelineViewer::populateSource() {
    if (!controller_) {
        sourceBrowser_->setPlainText(QString::fromUtf8("（未绑定控制器）"));
        return;
    }
    // 显示当前 token 流推导出的源码（从 controller->lastTokens 反推）
    // 简化方案：直接显示 token 字面拼接
    const auto& tokens = controller_->lastTokens();
    if (tokens.empty()) {
        sourceBrowser_->setPlainText(QString::fromUtf8("（尚未编译，请在主编辑器输入代码并触发编译分析）"));
        return;
    }
    std::ostringstream os;
    int lastLine = 1;
    for (const auto& tk : tokens) {
        while (lastLine < tk.line) {
            os << "\n";
            lastLine++;
        }
        if (tk.column > 1 && (os.tellp() == 0 || os.str().back() != '\n')) {
            os << " ";
        }
        os << tk.lexeme;
    }
    sourceBrowser_->setPlainText(QString::fromUtf8(os.str().c_str()));
}

void PipelineViewer::populateTokens() {
    if (!controller_) {
        tokenTable_->setRowCount(0);
        return;
    }
    const auto& tokens = controller_->lastTokens();
    tokenTable_->setRowCount((int)tokens.size());
    for (int i = 0; i < (int)tokens.size(); ++i) {
        const auto& tk = tokens[i];
        tokenTable_->setItem(i, 0, new QTableWidgetItem(QString::number(i)));
        tokenTable_->setItem(i, 1, new QTableWidgetItem(QString::fromStdString(Token::typeToString(tk.type))));
        tokenTable_->setItem(i, 2, new QTableWidgetItem(QString::fromUtf8(tk.lexeme.c_str())));
        tokenTable_->setItem(i, 3, new QTableWidgetItem(QString::number(tk.line)));
        tokenTable_->setItem(i, 4, new QTableWidgetItem(QString::number(tk.column)));
    }
}

void PipelineViewer::dumpAst(std::ostringstream& os, ASTNode* node, int depth, int maxDepth) {
    if (!node || depth > maxDepth) return;
    for (int i = 0; i < depth; ++i) os << "  ";
    os << node->nodeName().c_str();
    if (node->line > 0) os << "  [line " << node->line << "]";
    os << "\n";
    auto children = node->children();
    for (auto* child : children) {
        dumpAst(os, child, depth + 1, maxDepth);
    }
}

void PipelineViewer::populateAstSummary() {
    if (!controller_) {
        astSummary_->setPlainText(QString::fromUtf8("（未绑定控制器）"));
        return;
    }
    Block* ast = controller_->astRoot();
    if (!ast) {
        astSummary_->setPlainText(QString::fromUtf8("（尚未解析，请先在主编辑器中输入代码）"));
        return;
    }
    std::ostringstream os;
    os << "AST 根节点: " << ast->nodeName().c_str() << "\n";
    os << "节点行号: " << ast->line << "\n";
    os << "子节点数: " << ast->children().size() << "\n\n";
    os << "--- AST 树形结构（最大深度 12）---\n";
    dumpAst(os, ast, 0, 12);
    astSummary_->setPlainText(QString::fromUtf8(os.str().c_str()));
}

void PipelineViewer::populateIR() {
    if (!controller_) {
        irBrowser_->setPlainText(QString::fromUtf8("（未绑定控制器）"));
        return;
    }
    const IRFunction* ir = controller_->lastIR();
    if (!ir) {
        irBrowser_->setHtml(QString::fromUtf8(
            "<i>未启用 IR 编译。请在视图菜单启用 IR 模式或触发一次 IR 编译。</i>"));
        return;
    }
    // 简化输出：用 IRToString
    std::string s = IRToString(*ir);
    irBrowser_->setPlainText(QString::fromUtf8(s.c_str()));
}

void PipelineViewer::populateBytecode() {
    if (!controller_) {
        bytecodeBrowser_->setPlainText(QString::fromUtf8("（未绑定控制器）"));
        return;
    }
    const CompileResult& result = controller_->lastCompileResult();
    std::ostringstream os;
    os << "=== main chunk ===\n";
    os << result.mainChunk.disassemble();
    for (const auto& [name, chunk] : result.functionChunks) {
        os << "\n=== function: " << name << " ===\n";
        os << chunk.disassemble();
    }
    if (result.functionChunks.empty()) {
        os << "\n(无函数 chunk)";
    }
    bytecodeBrowser_->setPlainText(QString::fromUtf8(os.str().c_str()));
}
