#include "gui/CodeJourneyInfoPanel.h"
#include "gui/I18n.h"
#include "gui/TeachingTheme.h"
#include "Theme.h"   // QFluentKit（onThemeModeChanged 信号）

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTextBrowser>
#include <QPushButton>
#include <QLabel>
#include <QShowEvent>

#include "Label.h"   // QFluentKit（TitleLabel）

CodeJourneyInfoPanel::CodeJourneyInfoPanel(QWidget* parent)
    : QWidget(parent) {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(4);

    // 顶部说明
    auto* titleLabel = new TitleLabel(mlTr("🚀 代码的生命旅程"), this);
    mainLayout->addWidget(titleLabel);

    // 进度提示标签
    progressLabel_ = new QLabel(this);
    progressLabel_->setStyleSheet(
        "QLabel { background: #EEE8D5; border: 1px solid #93A1A1;"
        "  border-radius: 4px; padding: 6px 8px; font-size: 12px; }");
    progressLabel_->setWordWrap(true);
    mainLayout->addWidget(progressLabel_);

    // 主体：静态信息图（HTML 渲染）
    infoBrowser_ = new QTextBrowser(this);
    infoBrowser_->setHtml(buildJourneyHtml());
    infoBrowser_->setOpenExternalLinks(false);
    mainLayout->addWidget(infoBrowser_, 1);

    // 底部：跳转按钮（对应管线 6 个阶段）
    auto* btnBar = new QHBoxLayout;
    auto* btnEditor   = new QPushButton(mlTr("① 编辑器"), this);
    auto* btnTokens   = new QPushButton(mlTr("② Token 表"), this);
    auto* btnAst      = new QPushButton(mlTr("③ AST"), this);
    auto* btnIr       = new QPushButton(mlTr("④ IR"), this);
    auto* btnBytecode = new QPushButton(mlTr("⑤ 字节码"), this);
    auto* btnOutput   = new QPushButton(mlTr("⑥ 输出"), this);
    btnBar->addWidget(btnEditor);
    btnBar->addWidget(btnTokens);
    btnBar->addWidget(btnAst);
    btnBar->addWidget(btnIr);
    btnBar->addWidget(btnBytecode);
    btnBar->addWidget(btnOutput);
    btnBar->addStretch();
    mainLayout->addLayout(btnBar);

    connect(btnEditor,   &QPushButton::clicked, this, &CodeJourneyInfoPanel::onJumpToEditor);
    connect(btnTokens,   &QPushButton::clicked, this, &CodeJourneyInfoPanel::onJumpToTokens);
    connect(btnAst,      &QPushButton::clicked, this, &CodeJourneyInfoPanel::onJumpToAst);
    connect(btnIr,       &QPushButton::clicked, this, &CodeJourneyInfoPanel::onJumpToIr);
    connect(btnBytecode, &QPushButton::clicked, this, &CodeJourneyInfoPanel::onJumpToBytecode);
    connect(btnOutput,   &QPushButton::clicked, this, &CodeJourneyInfoPanel::onJumpToOutput);

    // 初始化进度提示
    refreshProgress();

    // 主题切换时重建 HTML（buildJourneyHtml 内部使用 TeachingTheme::surface()
    // 作为 <pre> 背景，需重新渲染以跟随新主题）。receiver=this 保证生命周期安全。
    Theme::onThemeModeChanged(this, [this](Fluent::ThemeMode) {
        if (infoBrowser_) {
            infoBrowser_->setHtml(buildJourneyHtml());
        }
    });
}

// ============================================================
// 完成判断标准 — 点进面板观看即算完成
// ============================================================

void CodeJourneyInfoPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // 首次显示即标记完成（用户点进来观看就算完成）
    if (!journeyCompleted_) {
        markCompleted();
    }
}

void CodeJourneyInfoPanel::markCompleted() {
    if (journeyCompleted_) return;
    journeyCompleted_ = true;
    refreshProgress();
    emit journeyCompleted();
}

void CodeJourneyInfoPanel::refreshProgress() {
    if (!progressLabel_) return;
    QString html;
    if (journeyCompleted_) {
        html = QString::fromUtf8(
            "<b>🎉 旅程完成！</b> 你已观看代码的生命旅程信息图。<br>"
            "可点击下方按钮深入探索每个阶段。");
        progressLabel_->setStyleSheet(
            "QLabel { background: #859900; color: white; border: none;"
            "  border-radius: 4px; padding: 6px 8px; font-size: 12px; font-weight: bold; }");
    } else {
        html = QString::fromUtf8(
            "<b>📖 代码的生命旅程</b> — 一张静态信息图展示编译管线。<br>"
            "点进来看即算完成，也可点击下方按钮深入探索。");
        progressLabel_->setStyleSheet(
            "QLabel { background: #EEE8D5; border: 1px solid #93A1A1;"
            "  border-radius: 4px; padding: 6px 8px; font-size: 12px; }");
    }
    progressLabel_->setText(html);
}

QString CodeJourneyInfoPanel::buildJourneyHtml() const {
    // P2 视觉一致性：
    // - 5 阶段色（绿/蓝/紫/红）走 TeachingTheme::learningStageColor()，与
    //   LearningPathPanel / WelcomeWizard Step 4 三处保持一致
    // - 橙(#FF9800)/青(#00BCD4)/棕(#795548) 是代码旅程特有语义色，保留不动
    // - 所有浅色 <pre> 背景统一改为 TeachingTheme::surface()，跟随亮/暗主题
    // - 灰色 #666/#888 保留（非阶段色，纯辅助文本色）
    const QString surfaceHex = TeachingTheme::surface().name();
    const QString stage0Hex  = TeachingTheme::learningStageColor(0).name();  // 绿
    const QString stage2Hex  = TeachingTheme::learningStageColor(2).name();  // 蓝
    const QString stage3Hex  = TeachingTheme::learningStageColor(3).name();  // 紫
    const QString stage4Hex  = TeachingTheme::learningStageColor(4).name();  // 红
    return QString(
        "<html><body style='font-family: Consolas, monospace; font-size: 13px; line-height: 1.6;'>"
        "<h2 style='color: %3;'>🚀 代码的生命旅程</h2>"
        "<p style='color: #666;'>从你写下代码到看到结果，中间发生了什么？</p>"
        "<hr>"
        "<h3 style='color: %2;'>示例代码</h3>"
        "<pre style='background: %1; padding: 8px; border-radius: 4px;'>print(1 + 2 * 3);</pre>"
        "<p>👆 这行代码最终输出 <b>7</b>。它是怎么变成 7 的？</p>"
        "<hr>"

        "<h3 style='color: #CB4B16;'>① 你写的代码（源码）</h3>"
        "<p>编辑器中的纯文本。计算机还不理解这些字符的含义。</p>"
        "<pre style='background: %1; padding: 8px; border-radius: 4px;'>print(1 + 2 * 3);</pre>"
        "<p style='color: #657B83;'>👇 词法分析</p>"
        "<hr>"

        "<h3 style='color: %4;'>② Token 表（词法分析）</h3>"
        "<p>词法分析器将源码切分为有类型的片段：</p>"
        "<pre style='background: %1; padding: 8px; border-radius: 4px;'>"
        "Token  | 类型       | lexeme<br>"
        "------|-----------|--------<br>"
        "  1   | IDENT     | print<br>"
        "  2   | LPAREN    | (<br>"
        "  3   | INT       | 1<br>"
        "  4   | PLUS      | +<br>"
        "  5   | INT       | 2<br>"
        "  6   | STAR      | *<br>"
        "  7   | INT       | 3<br>"
        "  8   | RPAREN    | )<br>"
        "  9   | SEMICOLON | ;"
        "</pre>"
        "<p>💡 注释被分离出主流（不计入 Token 表）</p>"
        "<p style='color: #657B83;'>👇 语法分析</p>"
        "<hr>"

        "<h3 style='color: %5;'>③ AST（语法分析）</h3>"
        "<p>解析器根据优先级规则构建树形结构：</p>"
        "<pre style='background: %1; padding: 8px; border-radius: 4px;'>"
        "Print<br>"
        " └── BinaryOp(+)<br>"
        "      ├── Number(1)<br>"
        "      └── BinaryOp(*)<br>"
        "           ├── Number(2)<br>"
        "           └── Number(3)"
        "</pre>"
        "<p>💡 注意 <b>*</b> 节点在 <b>+</b> 节点的下面——乘法优先级更高！</p>"
        "<p>括号改变结构：<code>(1+2)*3</code> 会变成 * 在根、+ 在左子树</p>"
        "<p style='color: #657B83;'>👇 编译</p>"
        "<hr>"

        "<h3 style='color: #2AA198;'>④ IR（中间表示）</h3>"
        "<p>AST 被转换为 IR（三地址码），便于优化：</p>"
        "<pre style='background: %1; padding: 8px; border-radius: 4px;'>"
        "function main:<br>"
        "  LOAD_CONST v1, 1      ; v1 = 1<br>"
        "  LOAD_CONST v2, 2      ; v2 = 2<br>"
        "  LOAD_CONST v3, 3      ; v3 = 3<br>"
        "  MUL v4, v2, v3        ; v4 = v2 * v3 = 6<br>"
        "  ADD v5, v1, v4        ; v5 = v1 + v4 = 7<br>"
        "  PRINT v5              ; 输出 7<br>"
        "  RETURN"
        "</pre>"
        "<p>💡 IR 优化 pass 可在此阶段进行：常量折叠、DCE、复制传播等</p>"
        "<p style='color: #657B83;'>👇 后端 lowering</p>"
        "<hr>"

        "<h3 style='color: #657B83;'>⑤ 字节码（虚拟机指令）</h3>"
        "<p>IR 被翻译为栈式 VM 字节码：</p>"
        "<pre style='background: %1; padding: 8px; border-radius: 4px;'>"
        "StackVM 字节码:<br>"
        "  OP_INT 1        ; push 1<br>"
        "  OP_INT 2        ; push 2<br>"
        "  OP_INT 3        ; push 3<br>"
        "  OP_MUL          ; pop 2,3 → push 6<br>"
        "  OP_ADD          ; pop 1,6 → push 7<br>"
        "  OP_PRINT        ; pop 7 → 输出"
        "</pre>"
        "<p>💡 三后端一致性：Interpreter / StackVM / RegisterVM 三条路径都输出 7</p>"
        "<p style='color: #657B83;'>👇 执行</p>"
        "<hr>"

        "<h3 style='color: %2;'>⑥ 输出结果</h3>"
        "<p>VM 执行字节码，得到最终结果：</p>"
        "<pre style='background: %1; padding: 8px; border-radius: 4px; font-size: 16px; font-weight: bold;'>7</pre>"
        "<p>🎉 这就是代码的生命旅程！</p>"
        "<hr>"

        "<h3 style='color: %3;'>📝 关键概念回顾</h3>"
        "<ul>"
        "<li><b>词法分析</b>：源码 → Token 序列（切分字符流）</li>"
        "<li><b>语法分析</b>：Token → AST（按优先级构建树）</li>"
        "<li><b>IR 生成</b>：AST → 三地址码（便于优化）</li>"
        "<li><b>后端 lowering</b>：IR → 字节码（VM 可执行）</li>"
        "<li><b>VM 执行</b>：字节码 → 结果（push/pop 或寄存器运算）</li>"
        "</ul>"
        "<p style='color: #657B83; font-size: 11px;'>提示：点击底部按钮跳转到对应面板，亲手探索每个阶段！</p>"
        "</body></html>"
    ).arg(surfaceHex, stage0Hex, stage2Hex, stage3Hex, stage4Hex);
}

void CodeJourneyInfoPanel::onJumpToEditor()   { emit jumpToPanelRequested("editor"); }
void CodeJourneyInfoPanel::onJumpToTokens()   { emit jumpToPanelRequested("tokens"); }
void CodeJourneyInfoPanel::onJumpToAst()      { emit jumpToPanelRequested("ast"); }
void CodeJourneyInfoPanel::onJumpToIr()       { emit jumpToPanelRequested("ir"); }
void CodeJourneyInfoPanel::onJumpToBytecode() { emit jumpToPanelRequested("bytecode"); }
void CodeJourneyInfoPanel::onJumpToOutput()   { emit jumpToPanelRequested("output"); }
