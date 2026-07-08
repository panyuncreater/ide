/**
 * @file PipelineViewer.cpp
 * @brief 编译流水线可视化面板（功能：编译过程分步展示）
 *
 * 职责：将一段 MiniLang 源码依次经过「源码 → 词法 → 语法(AST) → IR → 字节码」
 * 五个阶段，用分步页面逐项展示中间产物，帮助学员建立「代码如何被翻译」的整体认知。
 * 通过左侧步骤条切换阶段，并支持编辑器光标联动高亮对应源码行。
 */
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
#include "gui/I18n.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTextBrowser>
#include <QLabel>
#include <QFrame>
#include <QRegularExpression>
#include <QHeaderView>
#include <QApplication>
#include <QClipboard>
#include <QMenu>
#include <QShortcut>
#include <QPropertyAnimation>
#include <QEasingCurve>
#include <QAbstractAnimation>
#include <sstream>
#include <cstring>

#include "Label.h"   // QFluentKit（CaptionLabel）


// --- Clickable IR/bytecode HTML helpers ---
namespace {

/// Convert IR text (lines ending with "; line N") to clickable HTML
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
            html += "<a href=\"#LINE_" + lineNum + "\" style=\"color:inherit;text-decoration:none;\">"
                 + escaped + "</a>";
        } else {
            html += line.toHtmlEscaped();
        }
        if (i < lines.size() - 1) html += "\n";
    }
    html += "</pre>";
    return html;
}

/// Convert bytecode disassembly (format: "<offset> L<line> <OP> ...") to clickable HTML
static QString bytecodeTextToClickableHtml(const QString& plainText) {
    QStringList lines = plainText.split("\n");
    QString html;
    html += "<pre style=\"font-family:Consolas,monospace;\">";
    // Match lines starting with digits, space, L followed by digits
    static const QRegularExpression bcLineRe("^(\\d+)\\s+L(\\d+)\\s");
    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines[i];
        auto m = bcLineRe.match(line);
        if (m.hasMatch()) {
            QString lineNum = m.captured(2);
            QString escaped = line.toHtmlEscaped();
            html += "<a href=\"#LINE_" + lineNum + "\" style=\"color:inherit;text-decoration:none;\">"
                 + escaped + "</a>";
        } else {
            html += line.toHtmlEscaped();
        }
        if (i < lines.size() - 1) html += "\n";
    }
    html += "</pre>";
    return html;
}

} // anonymous namespace
// --- End clickable HTML helpers ---

// ============================================================
// 5 阶段主题色 / 图标 / 标题 / 描述
// ------------------------------------------------------------
// 配色取自 Solarized 亮色色板：
//   源码  → 石墨灰 #586E75（base01，中性厚重）
//   Token → 海蓝   #268BD2（blue，清澈）
//   AST   → 森林绿 #859900（green，生机）
//   IR    → 紫色   #6C71C4（violet，抽象）
//   字节码→ 橙色   #CB4B16（orange，落定）
// 与 TeachingTheme::learningStageColor 共用色板但映射不同
// （learningStageColor 用于学习路径 5 阶段，此处用于管线 5 阶段）。
// ============================================================
QColor PipelineViewer::stageColor(int step) {
    static const QColor kColors[] = {
        QColor("#586E75"),  // 源码：石墨灰
        QColor("#268BD2"),  // Token：海蓝
        QColor("#859900"),  // AST：森林绿
        QColor("#6C71C4"),  // IR：紫色
        QColor("#CB4B16"),  // 字节码：橙色
    };
    if (step < 0 || step >= 5) return QColor("#93A1A1");
    return kColors[step];
}

/// 返回指定编译阶段在步骤条上显示的图标字符。
QString PipelineViewer::stageIcon(int step) {
    static const QString kIcons[] = {
        QString::fromUtf8("\xF0\x9F\x93\x84"),  // 📄 源码
        QString::fromUtf8("\xF0\x9F\x94\xA2"),  // 🔢 Token
        QString::fromUtf8("\xF0\x9F\x8C\xB3"),  // 🌳 AST
        QString::fromUtf8("\xE2\x9A\x99"),       // ⚙ IR
        QString::fromUtf8("\xF0\x9F\x93\xA6"),  // 📦 字节码
    };
    if (step < 0 || step >= 5) return QString();
    return kIcons[step];
}

/// 返回指定编译阶段的标题文案（如「词法分析」）。
QString PipelineViewer::stageTitle(int step) {
    static const QString kTitles[] = {
        QString::fromUtf8("源码 Source"),
        QString::fromUtf8("Token 词法"),
        QString::fromUtf8("AST 语法树"),
        QString::fromUtf8("IR 中间表示"),
        QString::fromUtf8("字节码 Bytecode"),
    };
    if (step < 0 || step >= 5) return QString();
    return kTitles[step];
}

/// 返回指定编译阶段的功能说明文字，用于页面副标题。
QString PipelineViewer::stageDesc(int step) {
    static const QString kDescs[] = {
        QString::fromUtf8("用户输入的 MiniLang 源代码文本"),
        QString::fromUtf8("词法分析器输出的 Token 序列"),
        QString::fromUtf8("语法分析器构建的抽象语法树"),
        QString::fromUtf8("AST 转换的三地址码中间表示"),
        QString::fromUtf8("VM 可执行的栈式 / 寄存器式指令"),
    };
    if (step < 0 || step >= 5) return QString();
    return kDescs[step];
}

/// 构造编译流水线面板：初始化 UI 骨架并默认进入第一阶段。
PipelineViewer::PipelineViewer(QWidget* parent)
    : QWidget(parent) {
    // 整面板背景：Solarized base3（与 IDE 主背景一致）
    setObjectName("pipelineRoot");
    setStyleSheet(
        "QWidget#pipelineRoot { background: #FDF6E3; }"
    );

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(6);

    // === 步骤导航条：圆角容器 + 主题色按钮 + chevron 箭头 ===
    auto* stepBar = new QFrame(this);
    stepBar->setObjectName("pipelineStepBar");
    stepBar->setStyleSheet(QString(
        "QFrame#pipelineStepBar {"
        "  background: #EEE8D5;"             // Solarized base2
        "  border: 1px solid #93A1A1;"       // Solarized base1
        "  border-radius: 8px;"
        "}"
    ));
    auto* stepLayout = new QHBoxLayout(stepBar);
    stepLayout->setContentsMargins(8, 4, 8, 4);
    stepLayout->setSpacing(4);

    // 步骤按钮构造：emoji 图标 + objectName + 阶段主题色 QSS
    // 选中态：阶段色填充 + 白字；未选中态：浅色背景 + 阶段色边框
    // 按钮文本自动前置 stageIcon(step) emoji（📄/🔢/🌳/⚙/📦）
    auto makeStepBtn = [this](const QString& text, int step) {
        auto* btn = new QPushButton(stageIcon(step) + " " + text, this);
        btn->setObjectName(QString("stepBtn%1").arg(step));
        btn->setCheckable(true);
        btn->setMinimumWidth(96);
        btn->setCursor(Qt::PointingHandCursor);
        const QString colorHex = stageColor(step).name();
        btn->setStyleSheet(QString(
            "QPushButton {"
            "  background: #FDF6E3;"          // base3 未选中背景
            "  color: %1;"
            "  border: 1.5px solid %1;"
            "  border-radius: 4px;"
            "  padding: 6px 12px;"
            "  font-weight: 600;"
            "  font-size: 12px;"
            "}"
            "QPushButton:checked {"
            "  background: %1;"               // 选中态填充主题色
            "  color: white;"
            "}"
            "QPushButton:hover:!checked {"
            "  background: #EEE8D5;"          // base2 hover
            "}"
            "QPushButton:pressed {"
            "  background: %1; color: white;"
            "}"
        ).arg(colorHex));
        connect(btn, &QPushButton::clicked, this, [this, step]() { switchToStep(step); });
        return btn;
    };

    // 阶段间箭头：chevron › 带目标阶段主题色（暗示流向）
    auto makeArrow = [this](int destStep) {
        auto* arrow = new QLabel(QString::fromUtf8("\xE2\x80\xBA"), this);  // › U+203A
        arrow->setAlignment(Qt::AlignCenter);
        arrow->setFixedWidth(14);
        arrow->setStyleSheet(QString(
            "QLabel {"
            "  color: %1;"
            "  font-size: 22px;"
            "  font-weight: bold;"
            "  background: transparent;"
            "  border: none;"
            "}"
        ).arg(stageColor(destStep).name()));
        return arrow;
    };

    stepSourceBtn_   = makeStepBtn(QString::fromUtf8("1. 源码"),       0);
    stepTokenBtn_    = makeStepBtn(QString::fromUtf8("2. Token"),      1);
    stepAstBtn_      = makeStepBtn(QString::fromUtf8("3. AST"),        2);
    stepIrBtn_       = makeStepBtn(QString::fromUtf8("4. IR"),         3);
    stepBytecodeBtn_ = makeStepBtn(QString::fromUtf8("5. 字节码"),     4);

    stepLayout->addWidget(stepSourceBtn_);
    stepLayout->addWidget(makeArrow(1));
    stepLayout->addWidget(stepTokenBtn_);
    stepLayout->addWidget(makeArrow(2));
    stepLayout->addWidget(stepAstBtn_);
    stepLayout->addWidget(makeArrow(3));
    stepLayout->addWidget(stepIrBtn_);
    stepLayout->addWidget(makeArrow(4));
    stepLayout->addWidget(stepBytecodeBtn_);
    stepLayout->addStretch();

    mainLayout->addWidget(stepBar);

    // === 主体：QStackedWidget（外层圆角容器 + 每页 header + content） ===
    stack_ = new QStackedWidget(this);
    stack_->setObjectName("pipelineStack");
    stack_->setStyleSheet(QString(
        "QStackedWidget#pipelineStack {"
        "  background: #FDF6E3;"
        "  border: 1px solid #93A1A1;"
        "  border-radius: 6px;"
        "}"
    ));

    // === 内容控件创建（保持原有逻辑） ===
    sourceBrowser_ = new QTextBrowser(this);
    sourceBrowser_->setFont(QFont("Consolas"));
    sourceBrowser_->setStyleSheet(QString(
        "QTextBrowser {"
        "  background: #FDF6E3;"
        "  border: 1px solid #93A1A1;"
        "  border-radius: 4px;"
        "  padding: 4px;"
        "}"
    ));

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
    tokenTable_->setSelectionBehavior(QAbstractItemView::SelectItems);
    tokenTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    tokenTable_->setAlternatingRowColors(true);
    tokenTable_->setStyleSheet(QString(
        "QTableWidget {"
        "  background: #FDF6E3;"
        "  alternate-background-color: #EEE8D5;"
        "  border: 1px solid #93A1A1;"
        "  border-radius: 4px;"
        "  gridline-color: #93A1A1;"
        "  selection-background-color: #268BD2;"
        "  selection-color: white;"
        "}"
        "QHeaderView::section {"
        "  background: #EEE8D5;"
        "  color: #002B36;"
        "  border: none;"
        "  border-bottom: 1px solid #93A1A1;"
        "  padding: 4px 8px;"
        "  font-weight: bold;"
        "}"
    ));

    // M10: 右键菜单 — 复制单元格 / 复制整行
    tokenTable_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tokenTable_, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        auto* item = tokenTable_->itemAt(pos);
        if (!item) return;
        QMenu menu(tokenTable_);
        auto* copyAct = menu.addAction(mlTr("复制单元格"));
        auto* copyRowAct = menu.addAction(mlTr("复制整行"));
        QAction* selected = menu.exec(tokenTable_->viewport()->mapToGlobal(pos));
        if (selected == copyAct) {
            QApplication::clipboard()->setText(item->text());
        } else if (selected == copyRowAct) {
            int row = item->row();
            QStringList cells;
            for (int col = 0; col < tokenTable_->columnCount(); ++col) {
                auto* cellItem = tokenTable_->item(row, col);
                cells << (cellItem ? cellItem->text() : QString());
            }
            QApplication::clipboard()->setText(cells.join("\t"));
        }
    });

    // M10: Ctrl+C 复制当前单元格
    auto* copyShortcut = new QShortcut(QKeySequence::Copy, tokenTable_);
    connect(copyShortcut, &QShortcut::activated, this, [this]() {
        auto* item = tokenTable_->currentItem();
        if (item) {
            QApplication::clipboard()->setText(item->text());
        }
    });

    astSummary_ = new QTextBrowser(this);
    astSummary_->setFont(QFont("Consolas"));
    astSummary_->setStyleSheet(QString(
        "QTextBrowser {"
        "  background: #FDF6E3;"
        "  border: 1px solid #93A1A1;"
        "  border-radius: 4px;"
        "  padding: 4px;"
        "}"
    ));

    irBrowser_ = new QTextBrowser(this);
    irBrowser_->setFont(QFont("Consolas"));
    irBrowser_->setOpenLinks(false);
    irBrowser_->setOpenExternalLinks(false);
    irBrowser_->setStyleSheet(QString(
        "QTextBrowser {"
        "  background: #FDF6E3;"
        "  border: 1px solid #93A1A1;"
        "  border-radius: 4px;"
        "  padding: 4px;"
        "}"
    ));

    bytecodeBrowser_ = new QTextBrowser(this);
    bytecodeBrowser_->setFont(QFont("Consolas"));
    bytecodeBrowser_->setOpenLinks(false);
    bytecodeBrowser_->setOpenExternalLinks(false);
    bytecodeBrowser_->setStyleSheet(QString(
        "QTextBrowser {"
        "  background: #FDF6E3;"
        "  border: 1px solid #93A1A1;"
        "  border-radius: 4px;"
        "  padding: 4px;"
        "}"
    ));

    // === 包装每个内容控件到「header + content」容器 ===
    // header 条：左侧 4px 主题色竖线 + 标题 + 描述
    // content 区：原有控件，stretch=1 占满剩余空间
    auto wrapPage = [this](QWidget* content, int step) -> QWidget* {
        auto* page = new QWidget(this);
        page->setObjectName(QString("stagePage%1").arg(step));
        page->setStyleSheet(QString(
            "QWidget#stagePage%1 { background: transparent; }"
        ).arg(step));
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(8, 8, 8, 8);
        layout->setSpacing(6);

        // header 标签：富文本展示「图标 标题   描述」
        auto* header = new QLabel(page);
        header->setTextFormat(Qt::RichText);
        const QColor color = stageColor(step);
        const QString html = QString(
            "<div style='font-family: \"Segoe UI\", \"Microsoft YaHei\", sans-serif;'>"
            "<span style='font-size: 14px; font-weight: 600; color: %1;'>%2 %3</span>"
            "&nbsp;&nbsp;&nbsp;"
            "<span style='font-size: 11px; color: #657B83;'>%4</span>"
            "</div>"
        ).arg(color.name())
         .arg(stageIcon(step))
         .arg(stageTitle(step))
         .arg(stageDesc(step));
        header->setText(html);
        // header QSS：左 4px 主题色竖线 + 浅色背景 + 上下竖直 padding
        header->setStyleSheet(QString(
            "QLabel {"
            "  background: #EEE8D5;"             // base2 背景
            "  border-left: 4px solid %1;"       // 主题色左竖线
            "  border-top: 1px solid #93A1A1;"
            "  border-right: 1px solid #93A1A1;"
            "  border-bottom: 1px solid #93A1A1;"
            "  border-top-left-radius: 4px;"
            "  border-bottom-left-radius: 4px;"
            "  padding: 8px 12px;"
            "}"
        ).arg(color.name()));
        layout->addWidget(header);
        layout->addWidget(content, 1);  // auto-reparent 到 page
        return page;
    };

    stack_->addWidget(wrapPage(sourceBrowser_, 0));
    stack_->addWidget(wrapPage(tokenTable_, 1));
    stack_->addWidget(wrapPage(astSummary_, 2));
    stack_->addWidget(wrapPage(irBrowser_, 3));
    stack_->addWidget(wrapPage(bytecodeBrowser_, 4));
    mainLayout->addWidget(stack_, 1);

    // Click-to-highlight: IR browser anchor clicked
    connect(irBrowser_, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
        QString fragment = url.fragment();
        if (fragment.startsWith("LINE_")) {
            bool ok = false;
            int line = fragment.mid(5).toInt(&ok);
            if (ok && line > 0) {
                emit sourceLineRequested(line);
            }
        }
    });

    // Click-to-highlight: Bytecode browser anchor clicked
    connect(bytecodeBrowser_, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
        QString fragment = url.fragment();
        if (fragment.startsWith("LINE_")) {
            bool ok = false;
            int line = fragment.mid(5).toInt(&ok);
            if (ok && line > 0) {
                emit sourceLineRequested(line);
            }
        }
    });

    // === 底部状态条：emoji 图标 + 主题色左竖线 ===
    statusLabel_ = new CaptionLabel(QString::fromUtf8("\xF0\x9F\x93\x8C 步骤: 源码 | 光标: 行 1, 列 1"), this);  // 📌
    mainLayout->addWidget(statusLabel_);

    switchToStep(0);
}

/// 切换到指定阶段索引并刷新对应的分步页面内容。
void PipelineViewer::switchToStep(int step) {
    if (step < 0 || step >= 5) return;
    // AUDIT-P2 fix: 同一步骤重复点击不重新触发动画（原实现 page->pos() 依赖
    // 前一个动画的中间值，导致 finalPos 错误，页面卡在偏移位置）。
    if (step == currentStep_) {
        reloadCurrentStep();
        return;
    }
    const int oldStep = currentStep_;
    currentStep_ = step;
    stack_->setCurrentIndex(step);

    stepSourceBtn_->setChecked(step == 0);
    stepTokenBtn_->setChecked(step == 1);
    stepAstBtn_->setChecked(step == 2);
    stepIrBtn_->setChecked(step == 3);
    stepBytecodeBtn_->setChecked(step == 4);

    reloadCurrentStep();

    // === 切换动画：仅水平滑动 ===
    // 注：移除 fadeInWidget —— QGraphicsOpacityEffect 对含子 widget 的页面
    // 会卡 opacity=0 导致切换后空白，仅保留水平滑动动画即可。
    // 水平滑动：前进方向（step > oldStep）从右侧滑入，后退方向从左侧滑入
    // QStackedLayout 在 setCurrentIndex 后已固定子控件 geometry，
    // 之后调用 move() 不会被布局覆盖直到下一次几何变化
    QWidget* page = stack_->currentWidget();
    // AUDIT-P2 fix: 停止 page 上正在运行的 pos 动画，避免多个 QPropertyAnimation
    // 叠加修改 pos 属性导致页面最终位置错误。
    const auto oldAnims = page->findChildren<QPropertyAnimation*>();
    for (auto* a : oldAnims) {
        if (a->targetObject() == page && a->propertyName() == "pos") {
            a->stop();
            a->deleteLater();
        }
    }
    const int offset = 24;
    const int dx = (step >= oldStep) ? offset : -offset;
    const QPoint finalPos = page->pos();
    page->move(finalPos.x() + dx, finalPos.y());
    auto* slideAnim = new QPropertyAnimation(page, "pos", page);
    slideAnim->setDuration(220);
    slideAnim->setStartValue(page->pos());
    slideAnim->setEndValue(finalPos);
    slideAnim->setEasingCurve(QEasingCurve::OutCubic);
    slideAnim->start(QAbstractAnimation::DeleteWhenStopped);
}

/// 在不切换阶段的前提下，重新加载当前阶段页面（数据变更后调用）。
void PipelineViewer::reloadCurrentStep() {
    switch (currentStep_) {
        case 0: populateSource(); break;
        case 1: populateTokens(); break;
        case 2: populateAstSummary(); break;
        case 3: populateIR(); break;
        case 4: populateBytecode(); break;
    }
    updateStatusBar();
}

/// 编辑器光标移动回调：记录行列号并刷新状态栏显示。
void PipelineViewer::onCursorPositionChanged(int line, int column) {
    cursorLine_ = line;
    cursorColumn_ = column;
    if (statusLabel_) {
        updateStatusBar();
    }
}

// 刷新底部状态条：emoji 图标 + 当前阶段主题色左竖线
// 每次切换阶段或光标移动时调用，颜色跟随当前阶段
/// 刷新底部状态栏：展示当前阶段、光标行列等概览信息。
void PipelineViewer::updateStatusBar() {
    if (!statusLabel_) return;
    static const QStringList kStepNames = {
        QString::fromUtf8("源码"), QString::fromUtf8("Token"),
        QString::fromUtf8("AST"), QString::fromUtf8("IR"),
        QString::fromUtf8("字节码")
    };
    const QColor color = stageColor(currentStep_);
    statusLabel_->setStyleSheet(QString(
        "QLabel {"
        "  background: #EEE8D5;"                 // base2 背景
        "  border: 1px solid #93A1A1;"
        "  border-left: 4px solid %1;"           // 当前阶段主题色左竖线
        "  border-radius: 4px;"
        "  padding: 4px 10px;"
        "  color: #002B36;"
        "}"
    ).arg(color.name()));
    statusLabel_->setText(QString::fromUtf8("\xF0\x9F\x93\x8C 步骤: %1 | 光标: 行 %2, 列 %3")  // 📌
        .arg(kStepNames.value(currentStep_))
        .arg(cursorLine_).arg(cursorColumn_));
}

/// 填充「源码」页：原样展示当前编辑区源代码。
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
    // AUDIT-P2 fix: 原实现每次循环调用 os.str().back() 获取最后字符，os.str()
    // 返回 std::string 值拷贝（O(n)），n 个 Token 总复杂度 O(n²)。改用 lastChar
    // 变量跟踪最后写入字符，降为 O(n)。
    char lastChar = '\0';
    for (const auto& tk : tokens) {
        while (lastLine < tk.line) {
            os << "\n";
            lastChar = '\n';
            lastLine++;
        }
        if (tk.column > 1 && lastChar != '\0' && lastChar != '\n') {
            os << " ";
            lastChar = ' ';
        }
        os << tk.lexeme;
        if (!tk.lexeme.empty()) lastChar = tk.lexeme.back();
    }
    sourceBrowser_->setPlainText(QString::fromUtf8(os.str().c_str()));
}

/// 填充「词法」页：展示词法分析得到的 token 序列。
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

/// 递归将 AST 节点序列化为带缩进的文本，供 AST 摘要页使用。
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

/// 填充「语法树」页：展示从源码解析出的 AST 结构摘要。
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

/// 填充「中间表示」页：展示编译器生成的中间表示(IR)。
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
    irBrowser_->setHtml(irTextToClickableHtml(QString::fromUtf8(s.c_str())));
}

/// 填充「字节码」页：展示最终生成的字节码指令序列。
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
    bytecodeBrowser_->setHtml(bytecodeTextToClickableHtml(QString::fromUtf8(os.str().c_str())));
}
