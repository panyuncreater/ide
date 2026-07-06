// ============================================================
// TeachingPanelHeader.cpp — 教学面板统一标题栏组件实现
// ------------------------------------------------------------
// 内置 15 个教学面板的帮助文案（panelId → {purpose, order, concepts}）
// ============================================================

#include "gui/TeachingPanelHeader.h"
#include "gui/I18n.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QDialog>
#include <QVBoxLayout>
#include <QTextBrowser>

#include "PushButton.h"   // QFluentKit
#include "Label.h"        // QFluentKit

namespace {

struct HelpDoc {
    QString purpose;       // 面板用途
    QString recommendedOrder;  // 推荐使用顺序
    QString relatedConcepts;   // 关联概念
};

/// 15 个教学面板的帮助文案表
const QHash<QString, HelpDoc>& helpDocs() {
    static const QHash<QString, HelpDoc> docs = {
        {
            QStringLiteral("code-journey"),
            {
                mlTr("展示一行 MiniLang 代码从源码到输出结果的完整 6 阶段生命旅程：源码 → Token → AST → IR → 字节码 → 输出。每个阶段附带示例与跳转按钮，可亲手探索对应面板。"),
                mlTr("1. 先浏览本面板了解全流程\n2. 点击底部按钮跳转到各阶段面板\n3. 在对应面板中动手实验"),
                mlTr("词法分析 / 语法分析 / IR / 字节码 / 虚拟机")
            }
        },
        {
            QStringLiteral("learning-path"),
            {
                mlTr("结构化学习路线图，5 个阶段 21 个活动，跟踪你的学习进度。首次使用建议从「阶段零：首次接触」开始，按推荐顺序逐步完成。"),
                mlTr("1. 阶段零：首次接触（欢迎向导、代码旅程）\n2. 阶段一：编译前端（Token、AST）\n3. 阶段二：执行引擎（VM、IR）\n4. 阶段三：深入理解（闭包、异常）\n5. 阶段四：实战训练（Bug 狩猎、实验）"),
                mlTr("学习路径 / 进度跟踪 / 活动推荐")
            }
        },
        {
            QStringLiteral("pipeline"),
            {
                mlTr("编译管线可视化：6 阶段流水线，逐步演示源码到执行结果。每一步可查看该阶段的输入/输出数据结构与中间产物。"),
                mlTr("1. 先看「代码生命旅程」了解全流程\n2. 在本面板逐步推进管线\n3. 遇到感兴趣的阶段，跳转到对应专题面板"),
                mlTr("Lexer / Parser / AST / IR / Bytecode / VM")
            }
        },
        {
            QStringLiteral("token-puzzle"),
            {
                mlTr("Token 拼图游戏：拖拽 Token 重建源码，理解词法分析器如何将源码切分为有类型的片段。完成关卡后可解锁下一关。"),
                mlTr("1. 先看「编译管线可视化」了解 Token 阶段\n2. 在本面板完成拼图关卡\n3. 尝试「语法浏览器」了解 Token 类型全集"),
                mlTr("Token / 词法分析 / lexeme / token 类型")
            }
        },
        {
            QStringLiteral("ast-toy"),
            {
                mlTr("AST 交互式构建器：通过拖拽节点构建抽象语法树，理解运算符优先级与结合性如何影响树形结构。"),
                mlTr("1. 先完成「Token 拼图」理解词法\n2. 在本面板构建 AST\n3. 对比「编译管线可视化」的 AST 阶段"),
                mlTr("AST / 优先级 / 结合性 / 语法树")
            }
        },
        {
            QStringLiteral("syntax-explorer"),
            {
                mlTr("语法浏览器：浏览 MiniLang 全部语法结构与示例代码。点击语法节点查看对应的 AST 形态与使用场景。"),
                mlTr("1. 遇到不熟悉的语法时查阅本面板\n2. 结合「AST 构建器」实验对应语法"),
                mlTr("语法规则 / 语法节点 / AST")
            }
        },
        {
            QStringLiteral("backend-compare"),
            {
                mlTr("三后端对比：同一份 MiniLang 代码在 Interpreter / StackVM / RegisterVM 三条执行路径上的输出与耗时对比。理解三后端语义一致性。"),
                mlTr("1. 先了解三后端架构（见「代码生命旅程」）\n2. 在本面板输入代码对比输出\n3. 若结果不一致，可能是 Bug，可去「Bug 狩猎」"),
                mlTr("Interpreter / StackVM / RegisterVM / 三后端一致性")
            }
        },
        {
            QStringLiteral("vm-sandbox"),
            {
                mlTr("VM 栈沙盒：手动 push 字节码指令、单步执行，直观理解栈式虚拟机的工作原理。观察操作数栈的 push/pop 过程。"),
                mlTr("1. 先看「编译管线可视化」的字节码阶段\n2. 在本面板手动操作栈\n3. 结合「字节码追踪」看真实代码的执行轨迹"),
                mlTr("栈式 VM / OpCode / 操作数栈 / push/pop")
            }
        },
        {
            QStringLiteral("memory-model"),
            {
                mlTr("内存模型可视化：NaN-boxing 编码、Copy-On-Write 容器、GC mark-sweep 实时动画。理解值如何在内存中表示与回收。"),
                mlTr("1. 先了解 Value 类型（见「变量检查器」）\n2. 在本面板观察 GC 动画\n3. 结合「闭包检查器」看堆对象生命周期"),
                mlTr("NaN-boxing / COW / GC / mark-sweep / RefCounted")
            }
        },
        {
            QStringLiteral("ir-transform"),
            {
                mlTr("IR 优化逐步回放：const-fold / DCE / copy-prop / CSE / loop-unroll 五种优化 pass 的逐步演进与决策解释。理解编译器如何优化代码。"),
                mlTr("1. 先看「编译管线可视化」的 IR 阶段\n2. 在本面板逐步推进优化\n3. 结合「字节码追踪」看优化后的字节码"),
                mlTr("IR / 常量折叠 / 死代码消除 / 复制传播 / 公共子表达式 / 循环展开")
            }
        },
        {
            QStringLiteral("bytecode-trace"),
            {
                mlTr("字节码追踪：指令级执行追踪与状态快照。每条字节码指令执行后记录操作数栈、寄存器、IP 状态，便于回溯分析。"),
                mlTr("1. 先在「VM 栈沙盒」理解基本指令\n2. 在本面板追踪真实代码\n3. 结合「调用栈检查器」看函数调用"),
                mlTr("字节码 / 指令追踪 / 操作数栈 / IP")
            }
        },
        {
            QStringLiteral("call-stack"),
            {
                mlTr("调用栈检查器：帧结构、参数传递、返回地址可视化。理解函数调用时栈帧的创建与销毁过程。"),
                mlTr("1. 先了解函数调用机制\n2. 在本面板观察调用栈变化\n3. 结合「变量检查器」看局部变量"),
                mlTr("调用栈 / 栈帧 / 返回地址 / 参数传递")
            }
        },
        {
            QStringLiteral("variable-inspector"),
            {
                mlTr("变量检查器：作用域链、闭包捕获、变量生命周期可视化。理解变量在不同作用域中的可见性与生命周期。"),
                mlTr("1. 先了解作用域规则\n2. 在本面板观察变量绑定\n3. 结合「闭包检查器」看捕获机制"),
                mlTr("作用域 / 闭包捕获 / 变量生命周期 / Environment 链")
            }
        },
        {
            QStringLiteral("breakpoint-condition"),
            {
                mlTr("条件断点可视化：断点条件求值沙箱与命中计数。理解条件断点的工作原理与求值时机。"),
                mlTr("1. 先了解基本断点机制\n2. 在本面板实验条件表达式\n3. 在编辑器中设置真实条件断点"),
                mlTr("条件断点 / 求值沙箱 / 命中计数")
            }
        },
        {
            QStringLiteral("bug-hunt"),
            {
                mlTr("Bug 狩猎：7 类历史 Bug 的三后端对比 + 变体挑战题。理解常见 Bug 模式，锻炼调试能力。"),
                mlTr("1. 完成基础学习后进入\n2. 逐个挑战 Bug 题\n3. 开启「变体模式」尝试进阶变体"),
                mlTr("Bug 模式 / 三后端对比 / 变体挑战")
            }
        },
        {
            QStringLiteral("exception-flow"),
            {
                mlTr("异常流可视化：try/catch/finally 传播路径与栈效应。理解异常抛出后如何在调用栈中传播与捕获。"),
                mlTr("1. 先了解 try/catch 语法\n2. 在本面板观察传播路径\n3. 结合「调用栈检查器」看 unwind 过程"),
                mlTr("try/catch/finally / 异常传播 / 栈 unwind")
            }
        },
        {
            QStringLiteral("closure-inspector"),
            {
                mlTr("闭包检查器：upvalue 生命周期可视化（capture/heap/access/close/destroy）。理解闭包如何捕获与持有外部变量。"),
                mlTr("1. 先了解闭包概念\n2. 在本面板观察 upvalue 状态\n3. 结合「内存模型」看堆对象"),
                mlTr("闭包 / upvalue / 捕获 / 堆逃逸")
            }
        },
        {
            QStringLiteral("profile-dashboard"),
            {
                mlTr("性能仪表盘：三后端耗时对比 + opcode 执行计数 Top 10 热点。理解代码性能特征与优化方向。"),
                mlTr("1. 完成基础学习后进入\n2. 对比三后端性能\n3. 分析 opcode 热点找优化点"),
                mlTr("性能剖析 / opcode 计数 / 热点 / 三后端耗时")
            }
        },
        {
            QStringLiteral("lab-manual"),
            {
                mlTr("实验手册：结构化练习题，从基础到进阶。每个实验包含目标、步骤、验证标准，适合系统化学习。"),
                mlTr("1. 按「学习路径地图」推荐顺序进入\n2. 逐个完成实验\n3. 遇到困难查阅对应教学面板"),
                mlTr("实验 / 练习题 / 验证标准")
            }
        },
    };
    return docs;
}

} // namespace

TeachingPanelHeader::TeachingPanelHeader(const QString& panelId,
                                         const QString& title,
                                         QWidget* parent)
    : QWidget(parent), panelId_(panelId), title_(title) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(8);

    titleLabel_ = new StrongBodyLabel(title, this);
    titleLabel_->setPixelFontSize(14);
    layout->addWidget(titleLabel_, 1);

    helpBtn_ = new PushButton(mlTr("这是什么？"), this);
    helpBtn_->setFixedHeight(28);
    connect(helpBtn_, &PushButton::clicked, this, [this]() { showHelpDialog(); });
    layout->addWidget(helpBtn_);

    learningPathBtn_ = new PushButton(mlTr("学习路径"), this);
    learningPathBtn_->setFixedHeight(28);
    connect(learningPathBtn_, &PushButton::clicked, this, [this]() {
        emit learningPathRequested();
    });
    layout->addWidget(learningPathBtn_);
}

void TeachingPanelHeader::setTitle(const QString& title) {
    title_ = title;
    if (titleLabel_) titleLabel_->setText(title);
}

void TeachingPanelHeader::setShowLearningPathButton(bool show) {
    if (learningPathBtn_) learningPathBtn_->setVisible(show);
}

void TeachingPanelHeader::showHelpDialog() {
    const auto& docs = helpDocs();
    auto it = docs.find(panelId_);
    if (it == docs.end()) {
        // 未知 panelId，显示通用提示
        auto* dlg = new QDialog(this);
        dlg->setWindowTitle(mlTr("帮助"));
        auto* layout = new QVBoxLayout(dlg);
        auto* label = new BodyLabel(mlTr("暂无此面板的帮助文档。"), dlg);
        label->setWordWrap(true);
        layout->addWidget(label);
        dlg->setMinimumSize(400, 200);
        dlg->exec();
        dlg->deleteLater();
        return;
    }

    const HelpDoc& doc = it.value();

    auto* dlg = new QDialog(this);
    dlg->setWindowTitle(mlTr("帮助 · ") + title_);
    dlg->setWindowFlags(dlg->windowFlags() & ~Qt::WindowContextHelpButtonHint);
    dlg->setMinimumSize(480, 360);

    auto* layout = new QVBoxLayout(dlg);
    layout->setContentsMargins(20, 18, 20, 16);
    layout->setSpacing(10);

    auto* titleLabel = new TitleLabel(title_, dlg);
    layout->addWidget(titleLabel);

    auto* browser = new QTextBrowser(dlg);
    browser->setOpenExternalLinks(false);
    browser->setHtml(
        QStringLiteral("<html><body style='font-family: Microsoft YaHei, sans-serif; font-size: 13px; line-height: 1.7;'>"
        "<h3 style='color: #0078d4; margin-bottom: 4px;'>📋 面板用途</h3>"
        "<p style='margin: 0 0 12px 0;'>%1</p>"
        "<h3 style='color: #0078d4; margin-bottom: 4px;'>🔢 推荐使用顺序</h3>"
        "<p style='margin: 0 0 12px 0; white-space: pre-wrap;'>%2</p>"
        "<h3 style='color: #0078d4; margin-bottom: 4px;'>💡 关联概念</h3>"
        "<p style='margin: 0; color: #666;'>%3</p>"
        "</body></html>")
        .arg(doc.purpose.toHtmlEscaped())
        .arg(doc.recommendedOrder.toHtmlEscaped())
        .arg(doc.relatedConcepts.toHtmlEscaped()));
    layout->addWidget(browser, 1);

    auto* closeBtn = new PrimaryPushButton(mlTr("知道了"), dlg);
    closeBtn->setFixedWidth(120);
    auto* bottomBar = new QHBoxLayout;
    bottomBar->addStretch(1);
    bottomBar->addWidget(closeBtn);
    layout->addLayout(bottomBar);
    connect(closeBtn, &PushButton::clicked, dlg, &QDialog::accept);

    dlg->exec();
    dlg->deleteLater();
}
