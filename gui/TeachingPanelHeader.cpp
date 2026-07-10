// ============================================================
// TeachingPanelHeader.cpp — 教学面板统一标题栏组件实现
// ------------------------------------------------------------
// 内置教学面板的帮助文案（panelId → {purpose, order, concepts}）
// P2-C fix: 文案中的活动数 / 阶段数 / Bug 档位数从数据源派生，
//           不再硬编码魔法数字，避免活动集变化时帮助文案静默说谎。
// ============================================================

#include "gui/TeachingPanelHeader.h"
#include "gui/I18n.h"
#include "gui/LearningPathData.h" // P2-C fix: 派生活动数 / 阶段数

#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QTextBrowser>
#include <QTextDocument> // adjustHelpDialogSize 用到 document()->size()/setTextWidth()
#include <QVBoxLayout>

#include "Label.h"      // QFluentKit
#include "PushButton.h" // QFluentKit

namespace {

struct HelpDoc {
    QString purpose;          // 面板用途
    QString recommendedOrder; // 推荐使用顺序
    QString relatedConcepts;  // 关联概念
};

/// P2-C fix: 从 LearningPathData 派生「N 个阶段 M 个活动」文案。
/// 调用点不缓存，因为数据源是首次调用后 const static，零开销。
QString learningPathSummaryText() {
    const int stages = LearningPathData::stageCount();
    const int acts = static_cast<int>(LearningPathData::activities().size());
    return QStringLiteral("一张结构化的学习路线图，分 %1 个阶段、共 %2 个活动，一路帮你记着学到哪了。"
                          "第一次用，建议从「阶段零：首次接触」起步，照推荐顺序慢慢刷就行。")
        .arg(stages)
        .arg(acts);
}

/// 教学面板的帮助文案表（P2-C fix: 文案从数据源派生，不再硬编码计数）
const QHash<QString, HelpDoc>& helpDocs() {
    static const QHash<QString, HelpDoc> docs = {
        {QStringLiteral("code-journey"),
         {mlTr(
              "一行 MiniLang 代码从出生到结果，要走过 6 个阶段：源码 → Token → AST → IR → 字节码 → "
              "输出。这个面板就是把这趟旅程摊开给你看——每个阶段都带示例和跳转按钮，点一下就能去对应的专题面板动手玩。"),
          mlTr("1. 先在本面板把全流程溜一遍\n2. 看中哪个阶段，点底部按钮跳过去\n3. 在对应面板里真正动手试"),
          mlTr("词法分析 / 语法分析 / IR / 字节码 / 虚拟机")}},
        {QStringLiteral("learning-path"),
         {// P2-C fix: 活动数 / 阶段数从 LearningPathData 派生，避免硬编码 21
          learningPathSummaryText(),
          mlTr("1. 阶段零：首次接触（欢迎向导、代码旅程）\n2. 阶段一：编译前端（Token、AST）\n3. "
               "阶段二：执行引擎（VM、IR）\n4. 阶段三：深入理解（闭包、异常）\n5. 阶段四：实战训练（Bug 狩猎、实验）"),
          mlTr("学习路径 / 进度跟踪 / 活动推荐")}},
        {QStringLiteral("pipeline"),
         {mlTr("编译管线可视化：把源码变成运行结果的全过程拆成 6 "
               "个阶段，一步步演示给你看。每推进一步，都能看到这一步吃进去什么、吐出来什么（输入/"
               "输出数据结构和中间产物）。"),
          mlTr("1. 先在本面板看一遍完整流程\n2. 手动逐步推进管线\n3. 卡在哪个阶段，就跳去那个专题面板深挖"),
          mlTr("Lexer / Parser / AST / IR / Bytecode / VM")}},
        {QStringLiteral("token-puzzle"),
         {mlTr("Token 拼图：拖着一个个 Token "
               "把源码拼回去，直观感受词法分析器是怎么把字符流切成「有类型的零件」的。关卡通关后自动解锁下一关——像闯关"
               "，不像上课。"),
          mlTr("1. 先看「编译管线可视化」搞懂 Token 是啥\n2. 在本面板把拼图关卡刷了\n3. 顺便去「语法浏览器」看看 Token "
               "类型全家福"),
          mlTr("Token / 词法分析 / lexeme / token 类型")}},
        {QStringLiteral("ast-toy"),
         {mlTr("AST "
               "构建器：拖节点搭出一棵抽象语法树，亲眼看看运算符优先级和结合性是怎么决定树长什么样的。树长歪了，结果就"
               "歪了——这面板让你手动把树掰正。"),
          mlTr(
              "1. 先把「Token 拼图」做了，理解词法\n2. 在本面板动手搭 AST\n3. 回头对照「编译管线可视化」里的 AST 阶段"),
          mlTr("AST / 优先级 / 结合性 / 语法树")}},
        {QStringLiteral("syntax-explorer"),
         {mlTr("语法浏览器：把 MiniLang 的所有语法结构和示例代码摊在一张表里。点一个语法节点，就能看到它对应的 AST "
               "长相和典型用法——相当于一本会动的语法说明书。"),
          mlTr("1. 碰到不认识的语法，先来这翻翻\n2. 顺手去「AST 构建器」把对应语法试一遍"),
          mlTr("语法规则 / 语法节点 / AST")}},
        {QStringLiteral("backend-compare"),
         {mlTr("三后端对比：同一份代码，让 Interpreter / StackVM / RegisterVM "
               "三条路径各跑一遍，把输出和耗时摆在一起比。哪一后端偷偷耍了花招，一眼就看出来——这正是 MiniLang "
               "的「金标准」一致性检查。"),
          mlTr("1. 先搞懂三后端架构（看「代码生命旅程」）\n2. 在本面板输入代码对比三份输出\n3. 万一结果对不上，八成是 "
               "Bug，去「Bug 狩猎」练手"),
          mlTr("Interpreter / StackVM / RegisterVM / 三后端一致性")}},
        {QStringLiteral("vm-sandbox"),
         {mlTr("VM 栈沙盒：自己动手 push "
               "字节码、单步执行，看着操作数栈一会儿压进去、一会儿弹出来。栈式虚拟机就这么点事儿——把指令一条条喂进去，"
               "看栈怎么变。"),
          mlTr("1. 先看「编译管线可视化」的字节码阶段\n2. 在本面板手动摆弄栈\n3. "
               "配合「字节码追踪」看真实代码跑起来后的轨迹"),
          mlTr("栈式 VM / OpCode / 操作数栈 / push/pop")}},
        {QStringLiteral("memory-model"),
         {mlTr("内存模型可视化：把 NaN-boxing 编码、写时复制（COW）容器、GC 的 mark-sweep "
               "全做成了实时动画。值到底在内存里怎么放、怎么共享、怎么回收，看动画比看文档直观十倍。"),
          mlTr("1. 先搞懂 Value 类型（看「变量检查器」）\n2. 在本面板盯着 GC 动画看\n3. "
               "配合「闭包检查器」看堆里对象的寿命"),
          mlTr("NaN-boxing / COW / GC / mark-sweep / RefCounted")}},
        {QStringLiteral("ir-transform"),
         {mlTr("IR 优化回放：把 const-fold / DCE / copy-prop / CSE / loop-unroll 这五种优化 pass "
               "一步步演给你看，每步都解释「为什么这么改」。编译器不是魔法，是一步步把代码变瘦变快。"),
          mlTr("1. 先看「编译管线可视化」的 IR 阶段\n2. 在本面板逐步推进优化\n3. "
               "配合「字节码追踪」看优化后的字节码长啥样"),
          mlTr("IR / 常量折叠 / 死代码消除 / 复制传播 / 公共子表达式 / 循环展开")}},
        {QStringLiteral("bytecode-trace"),
         {mlTr("字节码追踪：指令级执行回放，每条字节码跑完都记一下操作数栈、寄存器、IP "
               "的状态。想搞清楚「这条指令到底改了什么」，就靠它来回溯。"),
          mlTr("1. 先在「VM 栈沙盒」搞懂基本指令\n2. 在本面板追踪真实代码\n3. 配合「调用栈检查器」看函数调用怎么发生"),
          mlTr("字节码 / 指令追踪 / 操作数栈 / IP")}},
        {QStringLiteral("call-stack"),
         {mlTr("调用栈检查器：把函数调用时的栈帧结构、参数怎么传、返回地址在哪，全画了出来。函数一层套一层时，谁调用了"
               "谁、返回去哪儿，一目了然。"),
          mlTr("1. 先搞懂函数调用机制\n2. 在本面板看调用栈怎么长怎么消\n3. 配合「变量检查器」看局部变量"),
          mlTr("调用栈 / 栈帧 / 返回地址 / 参数传递")}},
        {QStringLiteral("variable-inspector"),
         {mlTr("变量检查器：把作用域链、闭包捕获、变量生命周期画出来。一个变量在哪个作用域看得见、活多久，这个面板替你"
               "盯着。"),
          mlTr("1. 先搞懂作用域规则\n2. 在本面板看变量怎么绑定\n3. 配合「闭包检查器」看捕获是怎么发生的"),
          mlTr("作用域 / 闭包捕获 / 变量生命周期 / Environment 链")}},
        {QStringLiteral("breakpoint-condition"),
         {mlTr("条件断点可视化：把断点条件的求值沙箱和命中次数摆出来。条件断点到底什么时候算、什么时候命中，不是玄学——"
               "这个面板把它摊开给你看。"),
          mlTr("1. 先搞懂基本断点怎么用\n2. 在本面板试条件表达式\n3. 去编辑器里设个真实的条件断点验证"),
          mlTr("条件断点 / 求值沙箱 / 命中计数")}},
        {QStringLiteral("bug-hunt"),
         {// P2-C fix: 删除硬编码「7 类」描述（与 BugHuntLibrary 实际档位数无关）
          mlTr("Bug 狩猎：三档分级挑战（初阶 / 中阶 / 高阶），每一档都配三后端对比 + 变体题。读代码找 Bug "
               "的直觉，是刷出来的——这面板就是你的训练场。"),
          mlTr("1. 基础学完再进来\n2. 一道道 Bug 题啃过去\n3. 开「变体模式」挑战进阶变体"),
          mlTr("Bug 模式 / 三后端对比 / 变体挑战")}},
        {QStringLiteral("exception-flow"),
         {mlTr("异常流可视化：把 try/catch/finally "
               "的传播路径和栈效应画出来。异常抛出后怎么沿着调用栈往上爬、最后在哪被抓住，看一眼就明白。"),
          mlTr("1. 先搞懂 try/catch 语法\n2. 在本面板看传播路径\n3. 配合「调用栈检查器」看 unwind 过程"),
          mlTr("try/catch/finally / 异常传播 / 栈 unwind")}},
        {QStringLiteral("closure-inspector"),
         {mlTr("闭包检查器：把 upvalue 的一生（capture / heap / access / close / "
               "destroy）画出来。闭包到底是怎么抓住外面那个变量、又怎么一直抱着不放的，这个面板说清楚了。"),
          mlTr("1. 先搞懂闭包是什么\n2. 在本面板看 upvalue 的状态变化\n3. 配合「内存模型」看堆里的对象"),
          mlTr("闭包 / upvalue / 捕获 / 堆逃逸")}},
        {QStringLiteral("profile-dashboard"),
         {mlTr("性能仪表盘：把三后端的耗时摆一起比，再列个 opcode 执行次数 Top 10 "
               "热点。你的代码慢在哪、该从哪优化，看这张图就有方向了。"),
          mlTr("1. 基础学完再进来\n2. 对比三后端谁快谁慢\n3. 盯着 opcode 热点找优化点"),
          mlTr("性能剖析 / opcode 计数 / 热点 / 三后端耗时")}},
        {QStringLiteral("lab-manual"),
         {mlTr("实验手册：一套从基础到进阶的结构化练习，每个实验都给了目标、步骤和验证标准。想系统学一遍，跟着它走就行"
               "。"),
          mlTr("1. 按「学习路径地图」推荐的顺序进\n2. 一个实验一个实验做过去\n3. 卡住了就去对应的教学面板查"),
          mlTr("实验 / 练习题 / 验证标准")}},
        {QStringLiteral("glossary"),
         {mlTr("术语表：把 MiniLang IDE 的核心术语（词法 / 语法 / IR / 字节码 / VM / "
               "内存模型……）按类归在一起，每条都配简明释义和跳转按钮。碰到不认识的概念，随时来翻。"),
          mlTr("1. 读面板遇到陌生词，打开本面板查\n2. 点术语条目跳去相关教学面板\n3. "
               "配合「学习路径地图」把术语系统过一遍"),
          mlTr("术语 / 释义 / 概念索引 / 跨面板跳转")}},
    };
    return docs;
}

} // namespace

TeachingPanelHeader::TeachingPanelHeader(const QString& panelId, const QString& title, QWidget* parent)
    : QWidget(parent), panelId_(panelId), title_(title) {
    // WA_StyledBackground：确保 QSS background 在普通 QWidget 上生效
    setObjectName("teachingPanelHeader");
    setAttribute(Qt::WA_StyledBackground, true);

    auto* layout = new QHBoxLayout(this);
    // 整体 padding 8px 12px（垂直 8 / 水平 12）
    layout->setContentsMargins(12, 8, 12, 8);
    layout->setSpacing(8);

    titleLabel_ = new StrongBodyLabel(title, this);
    titleLabel_->setPixelFontSize(14);
    titleLabel_->setObjectName("teachingPanelTitle");
    // 标题加粗（StrongBodyLabel 已是强字重，显式 setBold 保证一致）
    QFont titleFont = titleLabel_->font();
    titleFont.setBold(true);
    titleLabel_->setFont(titleFont);
    layout->addWidget(titleLabel_, 1);

    helpBtn_ = new PushButton(mlTr("这是什么？"), this);
    helpBtn_->setFixedHeight(28);
    helpBtn_->setObjectName("teachingHeaderBtn");
    connect(helpBtn_, &PushButton::clicked, this, [this]() { showHelpDialog(); });
    layout->addWidget(helpBtn_);

    // 「新手引导」按钮：用主题色 #268BD2 强调，区别于普通帮助按钮
    tourBtn_ = new PushButton(mlTr("新手引导"), this);
    tourBtn_->setFixedHeight(28);
    tourBtn_->setObjectName("teachingTourBtn");
    connect(tourBtn_, &PushButton::clicked, this, [this]() { emit guidedTourRequested(panelId_); });
    layout->addWidget(tourBtn_);

    learningPathBtn_ = new PushButton(mlTr("学习路径"), this);
    learningPathBtn_->setFixedHeight(28);
    learningPathBtn_->setObjectName("teachingHeaderBtn");
    connect(learningPathBtn_, &PushButton::clicked, this, [this]() { emit learningPathRequested(); });
    layout->addWidget(learningPathBtn_);

    // 「返回编辑器」按钮：让用户从教学面板快速切回代码编辑区
    backBtn_ = new PushButton(mlTr("← 返回编辑器"), this);
    backBtn_->setFixedHeight(28);
    backBtn_->setObjectName("teachingBackBtn");
    connect(backBtn_, &PushButton::clicked, this, [this]() { emit returnToEditorRequested(); });
    layout->addWidget(backBtn_);

    // header 样式：浅蓝→白色渐变背景 + 底部分隔线 + 标题加粗 + 按钮 hover 圆角淡蓝
    setStyleSheet(QStringLiteral("#teachingPanelHeader {"
                                 "  background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
                                 "    stop:0 #eaf3fc, stop:1 #ffffff);"
                                 "  border-bottom: 1px solid #e5e5e5;"
                                 "}"
                                 "#teachingPanelTitle {"
                                 "  font-size: 14px;"
                                 "  font-weight: 600;"
                                 "  color: #1e1e1e;"
                                 "}"
                                 "#teachingHeaderBtn:hover {"
                                 "  background: #eaf3fc;"
                                 "  border-radius: 4px;"
                                 "}"
                                 // 「新手引导」按钮用主题色 #268BD2 强调，提示新手可点击获取引导
                                 "#teachingTourBtn {"
                                 "  background: #268BD2;"
                                 "  color: white;"
                                 "  border: 1px solid #1E6FA3;"
                                 "  border-radius: 4px;"
                                 "  padding: 2px 10px;"
                                 "  font-weight: 600;"
                                 "}"
                                 "#teachingTourBtn:hover {"
                                 "  background: #1E6FA3;"
                                 "}"
                                 "#teachingTourBtn:pressed {"
                                 "  background: #1A6090;"
                                 "}"
                                 // 「返回编辑器」按钮用浅色边框强调，方便用户从教学面板切回编辑器
                                 "#teachingBackBtn {"
                                 "  background: #FFFFFF;"
                                 "  color: #1E1E1E;"
                                 "  border: 1px solid #E0E0E0;"
                                 "  border-radius: 4px;"
                                 "  padding: 2px 10px;"
                                 "  font-weight: 600;"
                                 "}"
                                 "#teachingBackBtn:hover {"
                                 "  background: #F5F5F5;"
                                 "  border-color: #268BD2;"
                                 "  color: #268BD2;"
                                 "}"
                                 "#teachingBackBtn:pressed {"
                                 "  background: #F5F5F5;"
                                 "}"));
}

void TeachingPanelHeader::setTitle(const QString& title) {
    title_ = title;
    if (titleLabel_)
        titleLabel_->setText(title);
}

void TeachingPanelHeader::setShowLearningPathButton(bool show) {
    if (learningPathBtn_)
        learningPathBtn_->setVisible(show);
}

void TeachingPanelHeader::showHelpDialog() {
    // emoji 用 UTF-8 字节序列构造，避免 MSVC 源码编码不一致问题
    const QString kBookEmoji = QString::fromUtf8("\xF0\x9F\x93\x96"); // 📖
    const QString kTipEmoji = QString::fromUtf8("\xF0\x9F\x92\xA1");  // 💡

    // HTML 末尾的提示行：Esc 关闭 + 拖拽角落调整大小
    const QString kTipLine =
        QStringLiteral("<hr style='border: none; border-top: 1px dashed #E0E0E0; margin: 14px 0 8px 0;'/>"
                       "<p style='margin: 0; font-size: 12px; color: #8C8C8C;'>%1 提示：按 Esc 关闭，"
                       "拖拽角落可调整窗口大小</p>")
            .arg(kTipEmoji);

    // 中性白 QSS：背景 #FFFFFF，文字 #1E1E1E，padding 12px（R74: 回退 Solarized 米黄）
    // 「知道了」按钮保持 PrimaryButton 自带 Fluent 主色 #268BD2 样式，不覆盖
    const QString kDialogQss = QStringLiteral("QDialog#helpDialog {"
                                              "  background: #FFFFFF;"
                                              "}"
                                              "QTextBrowser#helpBrowser {"
                                              "  background: #FFFFFF;"
                                              "  color: #1E1E1E;"
                                              "  border: 1px solid #E0E0E0;"
                                              "  border-radius: 6px;"
                                              "  padding: 12px;"
                                              "}");

    const auto& docs = helpDocs();
    auto it = docs.find(panelId_);
    if (it == docs.end()) {
        // 兜底情况：未知 panelId，改用 QTextBrowser 替代 BodyLabel，确保长文本可滚动
        auto* dlg = new QDialog(this);
        dlg->setObjectName("helpDialog");
        // 窗口标题加 📖 emoji
        dlg->setWindowTitle(kBookEmoji + QStringLiteral(" ") + mlTr("帮助"));
        dlg->setWindowFlags(dlg->windowFlags() & ~Qt::WindowContextHelpButtonHint);
        dlg->setMinimumSize(500, 300);
        dlg->setMaximumSize(800, 600);
        dlg->resize(540, 340);

        auto* layout = new QVBoxLayout(dlg);
        layout->setContentsMargins(20, 18, 20, 16);
        layout->setSpacing(10);

        auto* browser = new QTextBrowser(dlg);
        browser->setObjectName("helpBrowser");
        browser->setOpenExternalLinks(false);
        browser->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        browser->setHtml(
            QStringLiteral(
                "<html><body style='font-family: Microsoft YaHei, sans-serif; font-size: 13px; line-height: 1.7;'>"
                "<p style='margin: 0;'>%1</p>"
                "%2"
                "</body></html>")
                .arg(mlTr("暂无此面板的帮助文档。").toHtmlEscaped())
                .arg(kTipLine));
        layout->addWidget(browser, 1);

        auto* closeBtn = new PrimaryPushButton(mlTr("知道了"), dlg);
        closeBtn->setFixedWidth(120);
        auto* bottomBar = new QHBoxLayout;
        bottomBar->addStretch(1);
        bottomBar->addWidget(closeBtn);
        layout->addLayout(bottomBar);
        connect(closeBtn, &PushButton::clicked, dlg, &QDialog::accept);

        dlg->setStyleSheet(kDialogQss);
        adjustHelpDialogSize(dlg, browser);
        // QDialog 默认支持 Esc 关闭（触发 reject）
        dlg->exec();
        dlg->deleteLater();
        return;
    }

    const HelpDoc& doc = it.value();

    auto* dlg = new QDialog(this);
    dlg->setObjectName("helpDialog");
    // 窗口标题格式：「📖 帮助 · <面板标题>」
    dlg->setWindowTitle(kBookEmoji + QStringLiteral(" ") + mlTr("帮助 · ") + title_);
    dlg->setWindowFlags(dlg->windowFlags() & ~Qt::WindowContextHelpButtonHint);
    // 统一调大窗口尺寸：最小 640×480，最大 900×700（限制最大避免占满屏幕）
    dlg->setMinimumSize(640, 480);
    dlg->setMaximumSize(900, 700);
    dlg->resize(680, 520); // 合理的初始大小

    auto* layout = new QVBoxLayout(dlg);
    layout->setContentsMargins(20, 18, 20, 16);
    layout->setSpacing(10);

    auto* titleLabel = new TitleLabel(title_, dlg);
    layout->addWidget(titleLabel);

    auto* browser = new QTextBrowser(dlg);
    browser->setObjectName("helpBrowser");
    browser->setOpenExternalLinks(false);
    browser->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    browser->setHtml(
        QStringLiteral(
            "<html><body style='font-family: Microsoft YaHei, sans-serif; font-size: 13px; line-height: 1.7;'>"
            "<h3 style='color: #268BD2; margin-bottom: 4px;'>📋 面板用途</h3>"
            "<p style='margin: 0 0 12px 0;'>%1</p>"
            "<h3 style='color: #268BD2; margin-bottom: 4px;'>🔢 推荐使用顺序</h3>"
            "<p style='margin: 0 0 12px 0; white-space: pre-wrap;'>%2</p>"
            "<h3 style='color: #268BD2; margin-bottom: 4px;'>💡 关联概念</h3>"
            "<p style='margin: 0; color: #5A5A5A;'>%3</p>"
            "%4"
            "</body></html>")
            .arg(doc.purpose.toHtmlEscaped())
            .arg(doc.recommendedOrder.toHtmlEscaped())
            .arg(doc.relatedConcepts.toHtmlEscaped())
            .arg(kTipLine));
    layout->addWidget(browser, 1);

    auto* closeBtn = new PrimaryPushButton(mlTr("知道了"), dlg);
    closeBtn->setFixedWidth(120);
    auto* bottomBar = new QHBoxLayout;
    bottomBar->addStretch(1);
    bottomBar->addWidget(closeBtn);
    layout->addLayout(bottomBar);
    connect(closeBtn, &PushButton::clicked, dlg, &QDialog::accept);

    dlg->setStyleSheet(kDialogQss);
    adjustHelpDialogSize(dlg, browser);
    // QDialog 默认支持 Esc 关闭（触发 reject）
    dlg->exec();
    dlg->deleteLater();
}

void TeachingPanelHeader::adjustHelpDialogSize(QDialog* dlg, QTextBrowser* browser) {
    if (!dlg || !browser || !browser->document())
        return;

    // 强制布局计算，确保 viewport 拿到有效宽度
    if (auto* lay = dlg->layout())
        lay->activate();

    // 以 viewport 宽度作为文本换行宽度计算文档实际高度
    int viewportWidth = browser->viewport()->width();
    // 兜底：dialog 未 show 时 viewport 宽度可能无效，用对话框宽度推算
    // 减去布局水平边距(20+20) + 浏览器 padding(12+12) + 边框(1+1)
    if (viewportWidth <= 0) {
        viewportWidth = dlg->width() - 40 - 24 - 2;
    }
    if (viewportWidth <= 0)
        viewportWidth = 600; // 最终兜底

    browser->document()->setTextWidth(viewportWidth);
    int docHeight = static_cast<int>(browser->document()->size().height());
    // padding(12+12) + 标题行 + 按钮行 + 布局间距 + 边距
    int desiredHeight = docHeight + 80;

    // 钳制到 [minimum, maximum] 区间，宽度保持当前值不变
    int minHeight = dlg->minimumHeight();
    int maxHeight = dlg->maximumHeight();
    int finalHeight = qBound(minHeight, desiredHeight, maxHeight);
    int currentWidth = dlg->width();
    dlg->resize(currentWidth, finalHeight);
}
