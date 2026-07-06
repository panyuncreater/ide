// ============================================================
// LearningHubDialog.cpp — 学习中心对话框实现
// ------------------------------------------------------------
// 4 分组卡片导航，点击卡片项发 panelRequested 信号。
// 使用 QFluentKit PushButton + CardWidget 实现 Fluent 视觉。
// ============================================================

#include "gui/LearningHubDialog.h"
#include "gui/I18n.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QFrame>
#include <QDialogButtonBox>

#include "PushButton.h"        // QFluentKit
#include "CardWidget.h"        // QFluentKit
#include "Label.h"             // QFluentKit

LearningHubDialog::LearningHubDialog(QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(mlTr("学习中心"));
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    setMinimumSize(560, 640);
    setModal(true);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 18, 20, 16);
    root->setSpacing(10);

    // ---- 顶部标题区 ----
    auto* titleLabel = new TitleLabel(mlTr("🎓 学习中心"), this);
    root->addWidget(titleLabel);

    auto* subtitleLabel = new CaptionLabel(
        mlTr("点击下方卡片打开对应教学面板。首次使用建议从「入门导览」开始。"), this);
    subtitleLabel->setWordWrap(true);
    root->addWidget(subtitleLabel);

    // ---- 主体：滚动区域 + 4 分组 ----
    auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    auto* scrollContent = new QWidget;
    auto* contentLayout = new QVBoxLayout(scrollContent);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(14);

    using PE = PanelEntry;

    // 分组 1：入门导览
    buildGroup(contentLayout,
        mlTr("入门导览"),
        mlTr("从未接触过 MiniLang？从这里开始"),
        {
            PE{QStringLiteral("welcome"),    mlTr("欢迎向导"),     mlTr("3 步交互式导览，了解 MiniLang 核心概念与界面布局"), QStringLiteral("🚀")},
            PE{QStringLiteral("code-journey"), mlTr("代码生命旅程"), mlTr("一行代码从源码到输出的 6 阶段全景图，附跳转按钮"), QStringLiteral("🛤️")},
            PE{QStringLiteral("learning-path"), mlTr("学习路径地图"), mlTr("5 阶段 21 活动的结构化学习路线，跟踪进度"), QStringLiteral("🗺️")},
        });

    // 分组 2：编译前端
    buildGroup(contentLayout,
        mlTr("编译前端"),
        mlTr("源码 → Token → AST，理解编译器前端"),
        {
            PE{QStringLiteral("pipeline"),       mlTr("编译管线可视化"), mlTr("6 阶段管线流水线，逐步演示源码到执行结果"), QStringLiteral("🔗")},
            PE{QStringLiteral("token-puzzle"),   mlTr("Token 拼图"),    mlTr("拖拽 Token 重建源码，理解词法分析"), QStringLiteral("🧩")},
            PE{QStringLiteral("ast-toy"),        mlTr("AST 构建器"),    mlTr("交互式构建 AST，理解优先级与结合性"), QStringLiteral("🌳")},
            PE{QStringLiteral("syntax-explorer"), mlTr("语法浏览器"),    mlTr("浏览 MiniLang 全部语法结构与示例"), QStringLiteral("📖")},
        });

    // 分组 3：执行引擎
    buildGroup(contentLayout,
        mlTr("执行引擎"),
        mlTr("三后端执行：解释器 / 栈式 VM / 寄存器 VM"),
        {
            PE{QStringLiteral("backend-compare"),     mlTr("三后端对比"),    mlTr("同一代码在三条路径的输出与耗时对比"), QStringLiteral("⚖️")},
            PE{QStringLiteral("vm-sandbox"),          mlTr("VM 沙盒"),       mlTr("手动 push 字节码、单步执行，理解栈式 VM"), QStringLiteral(" sandbox️")},
            PE{QStringLiteral("memory-model"),        mlTr("内存模型"),      mlTr("NaN-boxing / COW / GC mark-sweep 实时动画"), QStringLiteral("🧠")},
            PE{QStringLiteral("ir-transform"),        mlTr("IR 优化回放"),   mlTr("const-fold / DCE / copy-prop / CSE / loop-unroll 逐步演示"), QStringLiteral("⚙️")},
            PE{QStringLiteral("bytecode-trace"),      mlTr("字节码追踪"),    mlTr("指令级执行追踪与状态快照"), QStringLiteral("📡")},
            PE{QStringLiteral("call-stack"),          mlTr("调用栈检查器"),  mlTr("帧结构、参数传递、返回地址可视化"), QStringLiteral("📚")},
            PE{QStringLiteral("variable-inspector"),  mlTr("变量检查器"),    mlTr("作用域链、闭包捕获、变量生命周期"), QStringLiteral("🔍")},
            PE{QStringLiteral("breakpoint-condition"), mlTr("条件断点"),     mlTr("断点条件求值沙箱与命中计数"), QStringLiteral("🛑")},
        });

    // 分组 4：深入实战
    buildGroup(contentLayout,
        mlTr("深入实战"),
        mlTr("异常流 / 闭包 / 性能 / Bug 狩猎"),
        {
            PE{QStringLiteral("bug-hunt"),           mlTr("Bug 狩猎"),      mlTr("7 类历史 Bug 三后端对比 + 变体挑战"), QStringLiteral("🐛")},
            PE{QStringLiteral("exception-flow"),     mlTr("异常流可视化"),  mlTr("try/catch/finally 传播路径与栈效应"), QStringLiteral("⚡")},
            PE{QStringLiteral("closure-inspector"),  mlTr("闭包检查器"),    mlTr("upvalue 生命周期：capture/heap/access/close"), QStringLiteral("🔒")},
            PE{QStringLiteral("profile-dashboard"),  mlTr("性能仪表盘"),    mlTr("三后端耗时 + opcode 热点 Top 10"), QStringLiteral("📊")},
            PE{QStringLiteral("lab-manual"),         mlTr("实验手册"),      mlTr("结构化练习题，从基础到进阶"), QStringLiteral("📘")},
        });

    contentLayout->addStretch(1);
    scrollArea->setWidget(scrollContent);
    root->addWidget(scrollArea, 1);

    // ---- 底部关闭按钮 ----
    auto* closeBtn = new PrimaryPushButton(mlTr("关闭"), this);
    closeBtn->setFixedWidth(120);
    auto* bottomBar = new QHBoxLayout;
    bottomBar->addStretch(1);
    bottomBar->addWidget(closeBtn);
    root->addLayout(bottomBar);

    connect(closeBtn, &PushButton::clicked, this, &QDialog::accept);
}

void LearningHubDialog::buildGroup(QVBoxLayout* parentLayout,
                                   const QString& groupTitle,
                                   const QString& groupSubtitle,
                                   const std::vector<PanelEntry>& entries) {
    auto* groupCard = new SimpleCardWidget(this);
    groupCard->setBorderRadius(8);
    auto* groupLayout = new QVBoxLayout(groupCard);
    groupLayout->setContentsMargins(16, 12, 16, 12);
    groupLayout->setSpacing(6);

    auto* titleLabel = new StrongBodyLabel(groupTitle, groupCard);
    titleLabel->setPixelFontSize(15);
    groupLayout->addWidget(titleLabel);

    auto* subtitleLabel = new CaptionLabel(groupSubtitle, groupCard);
    subtitleLabel->setWordWrap(true);
    groupLayout->addWidget(subtitleLabel);

    groupLayout->addSpacing(4);

    for (const auto& entry : entries) {
        auto* itemCard = new SimpleCardWidget(groupCard);
        itemCard->setClickEnabled(true);
        itemCard->setBorderRadius(6);
        auto* itemLayout = new QHBoxLayout(itemCard);
        itemLayout->setContentsMargins(12, 8, 12, 8);
        itemLayout->setSpacing(10);

        // 图标提示（emoji 字符）
        auto* iconLabel = new BodyLabel(entry.iconHint, itemCard);
        iconLabel->setPixelFontSize(18);
        iconLabel->setFixedWidth(28);
        itemLayout->addWidget(iconLabel);

        // 标题 + 描述
        auto* textLayout = new QVBoxLayout;
        textLayout->setSpacing(2);
        auto* nameLbl = new BodyLabel(entry.name, itemCard);
        nameLbl->setPixelFontSize(13);
        auto* descLbl = new CaptionLabel(entry.description, itemCard);
        descLbl->setWordWrap(true);
        textLayout->addWidget(nameLbl);
        textLayout->addWidget(descLbl);
        itemLayout->addLayout(textLayout, 1);

        groupLayout->addWidget(itemCard);

        // 点击发信号
        const QString panelId = entry.id;
        connect(itemCard, &SimpleCardWidget::clicked, this, [this, panelId]() {
            // 特殊处理：欢迎向导
            if (panelId == QStringLiteral("welcome")) {
                emit welcomeWizardRequested();
            } else {
                emit panelRequested(panelId);
            }
            accept();  // 关闭对话框
        });
    }

    parentLayout->addWidget(groupCard);
}
