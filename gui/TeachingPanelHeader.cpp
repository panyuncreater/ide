// ============================================================
// TeachingPanelHeader.cpp — 教学面板统一标题栏组件实现
// ------------------------------------------------------------
// 内置教学面板的帮助文案（panelId → {purpose, order, concepts}）
// P2-C fix: 文案中的活动数 / 阶段数 / Bug 档位数从数据源派生，
//           不再硬编码魔法数字，避免活动集变化时帮助文案静默说谎。
// 文案支持 Markdown 子集（**粗体** / `行内代码` / 有序列表），
// 由 MarkdownRenderer 统一渲染为 HTML，与其余教学面板保持一致。
// ============================================================

#include "gui/TeachingPanelHeader.h"
#include "gui/I18n.h"
#include "gui/LearningPathData.h"  // P2-C fix: 派生活动数 / 阶段数
#include "gui/MarkdownRenderer.h" // 帮助文案 Markdown → HTML 统一渲染
#include "gui/PanelCatalog.h"     // UX-R fix: 帮助弹窗展示难度分级/所属分类
#include "gui/TeachingTheme.h"    // P3-18 fix: 硬编码颜色迁移到语义色

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
                          "第一次用，建议从「阶段零：首次接触」起步，照推荐顺序慢慢刷就行。"
                          "五个阶段对应一条完整的编译原理学习曲线：**先建立直觉，再动手实验，最后实战排错**。")
        .arg(stages)
        .arg(acts);
}

/// 教学面板的帮助文案表（P2-C fix: 文案从数据源派生，不再硬编码计数）
const QHash<QString, HelpDoc>& helpDocs() {
    static const QHash<QString, HelpDoc> docs = {
        {QStringLiteral("code-journey"),
         {mlTr(
              "一行 MiniLang 代码从出生到结果，要走过 6 个阶段：`源码 → Token → AST → IR → 字节码 → "
              "输出`。这个面板就是把这趟旅程摊开给你看——每个阶段都带示例和跳转按钮，点一下就能去对应的专题面板动手玩。"
              "看完这张图，你脑子里就有了一张**心智地图**：之后的每个专题面板，都是这张图上某一站的放大镜。"),
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
               "输出数据结构和中间产物）。推进时留意数据形态的变化：`字符流 → Token 流 → AST → IR → 字节码 → "
               "值`——**理解了数据形态怎么变，就理解了编译的本质**。"),
          mlTr("1. 先在本面板看一遍完整流程\n2. 手动逐步推进管线\n3. 卡在哪个阶段，就跳去那个专题面板深挖"),
          mlTr("Lexer / Parser / AST / IR / Bytecode / VM")}},
        {QStringLiteral("token-puzzle"),
         {mlTr("Token 拼图：拖着一个个 Token "
               "把源码拼回去，直观感受词法分析器是怎么把字符流切成「有类型的零件」的。关卡通关后自动解锁下一关——像闯关"
               "，不像上课。**词法分析（Lexical Analysis）是编译的第一步**：它不关心含义，只负责断词——把 `var x = 1;` "
               "切成关键字、标识符、运算符、字面量、分隔符五类零件。"),
          mlTr("1. 先看「编译管线可视化」搞懂 Token 是啥\n2. 在本面板把拼图关卡刷了\n3. 顺便去「语法浏览器」看看 Token "
               "类型全家福"),
          mlTr("Token / 词法分析 / lexeme / token 类型")}},
        {QStringLiteral("ast-toy"),
         {mlTr("AST "
               "构建器：拖节点搭出一棵抽象语法树，亲眼看看运算符优先级和结合性是怎么决定树长什么样的。树长歪了，结果就"
               "歪了——这面板让你手动把树掰正。记住一句话：**树的形状就是计算的顺序**——`1+2*3` 和 `(1+2)*3` "
               "的差别，全在树根是 `+` 还是 `*`。"),
          mlTr(
              "1. 先把「Token 拼图」做了，理解词法\n2. 在本面板动手搭 AST\n3. 回头对照「编译管线可视化」里的 AST 阶段"),
          mlTr("AST / 优先级 / 结合性 / 语法树")}},
        {QStringLiteral("syntax-explorer"),
         {mlTr("语法浏览器：把 MiniLang 的所有语法结构和示例代码摊在一张表里。点一个语法节点，就能看到它对应的 AST "
               "长相和典型用法——相当于一本会动的语法说明书。背后的概念叫**产生式（Production）**：每条语法规则都在说"
               "「这个结构由哪些零件组成」，递归下降解析器就是照着产生式一条条写出来的函数。"),
          mlTr("1. 碰到不认识的语法，先来这翻翻\n2. 顺手去「AST 构建器」把对应语法试一遍"),
          mlTr("语法规则 / 产生式 / 语法节点 / AST")}},
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
               "看栈怎么变。核心心法：**表达式让栈净增 1，语句让栈净增 0**——栈平衡是字节码正确性的第一不变量，"
               "违反它就是「栈泄漏」Bug。"),
          mlTr("1. 先看「编译管线可视化」的字节码阶段\n2. 在本面板手动摆弄栈\n3. "
               "配合「字节码追踪」看真实代码跑起来后的轨迹"),
          mlTr("栈式 VM / OpCode / 操作数栈 / push/pop")}},
        {QStringLiteral("memory-model"),
         {mlTr("内存模型可视化：把 NaN-boxing 编码、写时复制（COW）容器、GC 的 mark-sweep "
               "全做成了实时动画。值到底在内存里怎么放、怎么共享、怎么回收，看动画比看文档直观十倍。"
               "三个关键词各管一件事：**NaN-boxing** 管「一个值怎么用 8 字节装下」，**COW** 管「共享的容器什么时候才复制」，"
               "**mark-sweep** 管「循环引用的垃圾怎么兜底回收」。"),
          mlTr("1. 先搞懂 Value 类型（看「变量检查器」）\n2. 在本面板盯着 GC 动画看\n3. "
               "配合「闭包检查器」看堆里对象的寿命"),
          mlTr("NaN-boxing / COW / GC / mark-sweep / RefCounted")}},
        {QStringLiteral("ir-transform"),
         {mlTr("IR 优化回放：把 const-fold / DCE / copy-prop / CSE / loop-unroll 这五种优化 pass "
               "一步步演给你看，每步都解释「为什么这么改」。编译器不是魔法，是一步步把代码变瘦变快。"
               "IR（中间表示）之所以存在，是因为**优化只想写一次**：在 IR 上做完优化，再 lowering "
               "到栈式或寄存器式后端，三个后端共享同一份优化成果。"),
          mlTr("1. 先看「编译管线可视化」的 IR 阶段\n2. 在本面板逐步推进优化\n3. "
               "配合「字节码追踪」看优化后的字节码长啥样"),
          mlTr("IR / 常量折叠 / 死代码消除 / 复制传播 / 公共子表达式 / 循环展开")}},
        {QStringLiteral("bytecode-trace"),
         {mlTr("字节码追踪：指令级执行回放，每条字节码跑完都记一下操作数栈、寄存器、IP "
               "的状态。想搞清楚「这条指令到底改了什么」，就靠它来回溯。先记住一个概念：**IP（指令指针）**永远指向"
               "下一条要执行的指令——顺序执行就是 IP+1，分支和循环就是把 IP 改成别的值，仅此而已。"),
          mlTr("1. 先在「VM 栈沙盒」搞懂基本指令\n2. 在本面板追踪真实代码\n3. 配合「调用栈检查器」看函数调用怎么发生"),
          mlTr("字节码 / 指令追踪 / 操作数栈 / IP")}},
        {QStringLiteral("call-stack"),
         {mlTr("调用栈检查器：把函数调用时的栈帧结构、参数怎么传、返回地址在哪，全画了出来。函数一层套一层时，谁调用了"
               "谁、返回去哪儿，一目了然。核心概念是**栈帧（Stack Frame）**：每次调用函数就压入一帧（装着参数、"
               "局部变量、返回地址），函数返回就弹出一帧——递归爆栈，爆的就是这个栈。"),
          mlTr("1. 先搞懂函数调用机制\n2. 在本面板看调用栈怎么长怎么消\n3. 配合「变量检查器」看局部变量"),
          mlTr("调用栈 / 栈帧 / 返回地址 / 参数传递")}},
        {QStringLiteral("variable-inspector"),
         {mlTr("变量检查器：把作用域链、闭包捕获、变量生命周期画出来。一个变量在哪个作用域看得见、活多久，这个面板替你"
               "盯着。记住一条规则：**找变量永远从最内层作用域开始，一层层往外找**——这条链就是 Environment 链，"
               "找到第一个同名变量就停，所以内层同名变量会「遮住」外层。"),
          mlTr("1. 先搞懂作用域规则\n2. 在本面板看变量怎么绑定\n3. 配合「闭包检查器」看捕获是怎么发生的"),
          mlTr("作用域 / 闭包捕获 / 变量生命周期 / Environment 链")}},
        {QStringLiteral("breakpoint-condition"),
         {mlTr("条件断点可视化：把断点条件的求值沙箱和命中次数摆出来。条件断点到底什么时候算、什么时候命中，不是玄学——"
               "这个面板把它摊开给你看。关键设计是**沙箱求值（Sandbox Evaluation）**：条件表达式在隔离环境里算，"
               "算崩了、死循环了都不能影响主程序——调试器自己不能比被调的程序先挂。"),
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
               "的传播路径和栈效应画出来。异常抛出后怎么沿着调用栈往上爬、最后在哪被抓住，看一眼就明白。"
               "这个过程的术语叫**栈展开（Stack Unwinding）**：异常从抛出点开始逐帧弹栈找 catch，"
               "沿途每层的 finally 都会被执行——这保证了资源清理不会被异常跳过。"),
          mlTr("1. 先搞懂 try/catch 语法\n2. 在本面板看传播路径\n3. 配合「调用栈检查器」看 unwind 过程"),
          mlTr("try/catch/finally / 异常传播 / 栈 unwind")}},
        {QStringLiteral("closure-inspector"),
         {mlTr("闭包检查器：把 upvalue 的一生（capture / heap / access / close / "
               "destroy）画出来。闭包到底是怎么抓住外面那个变量、又怎么一直抱着不放的，这个面板说清楚了。"
               "一句话建立直觉：**闭包 = 函数 + 它记住的外层变量（upvalue）**——外层函数返回后局部变量本该消亡，"
               "但被捕获的变量会「逃」到堆上继续活着，这就是 close upvalue 的意义。"),
          mlTr("1. 先搞懂闭包是什么\n2. 在本面板看 upvalue 的状态变化\n3. 配合「内存模型」看堆里的对象"),
          mlTr("闭包 / upvalue / 捕获 / 堆逃逸")}},
        {QStringLiteral("profile-dashboard"),
         {mlTr("性能仪表盘：把三后端的耗时摆一起比，再列个 opcode 执行次数 Top 10 "
               "热点。你的代码慢在哪、该从哪优化，看这张图就有方向了。背后是性能分析的第一原则："
               "**先测量，再优化**——热点通常集中在极少数代码上（二八律），凭感觉优化往往白忙。"),
          mlTr("1. 基础学完再进来\n2. 对比三后端谁快谁慢\n3. 盯着 opcode 热点找优化点"),
          mlTr("性能剖析 / opcode 计数 / 热点 / 三后端耗时")}},
        {QStringLiteral("lab-manual"),
         {mlTr("实验手册：一套从基础到进阶的结构化练习，每个实验都给了目标、步骤和验证标准。想系统学一遍，跟着它走就行"
               "。8 个实验正好对应编译原理的 8 个主题：**词法 → 语法 → 语义 → 目标代码 → 中间代码 → "
               "三后端一致性 → 内存模型 → Bug 狩猎**，每章还带迷你实验和章节练习题，学完能自测。"),
          mlTr("1. 按「学习路径地图」推荐的顺序进\n2. 一个实验一个实验做过去\n3. 卡住了就去对应的教学面板查"),
          mlTr("实验 / 练习题 / 验证标准")}},
        {QStringLiteral("glossary"),
         {mlTr("术语表：把 MiniLang IDE 的核心术语（词法 / 语法 / IR / 字节码 / VM / "
               "内存模型……）按类归在一起，每条都配简明释义、详细讲解和跳转按钮。碰到不认识的概念，随时来翻。"
               "几个高频术语先混个脸熟：**NaN-boxing**（用 8 字节 double 的 NaN 空闲位编码所有类型的值）、"
               "**COW**（写时复制，共享容器写前才复制）、**upvalue**（闭包捕获的外层变量）、"
               "**SSA**（静态单赋值，每个值只写一次，方便优化分析）、**lowering**（把 IR 降低成具体后端指令）。"
               "支持搜索和分类筛选，术语条目还能一键跳到对应教学面板动手验证。"),
          mlTr("1. 读面板遇到陌生词，打开本面板查\n2. 点术语条目跳去相关教学面板，边看释义边动手验证\n3. "
               "配合「学习路径地图」把术语系统过一遍\n4. 学完一个阶段，回来用搜索框自测：能不能用自己的话讲清每个术语"),
          mlTr("术语 / 释义 / 概念索引 / 跨面板跳转 / NaN-boxing / COW / upvalue / SSA")}},
        // ---- 入门导览（补齐缺失面板）----
        {QStringLiteral("welcome"),
         {mlTr("新手的第一站：用几张卡片介绍 MiniLang IDE 的核心功能和学习路径，让你快速知道「这工具能干"
               "嘛、我该从哪开始」。"),
          mlTr("1. 第一次打开 IDE 时浏览引导卡片\n2. 点「开始旅程」跳去学习路径地图\n3. "
               "跟着推荐顺序逐个面板探索"),
          mlTr("引导 / 入门 / 学习路径")}},
        {QStringLiteral("course-system"),
         {mlTr("把 MiniLang 的教学内容组织成结构化课程，每节课配目标、讲解和练习，照着学就能系统掌握从词"
               "法到 VM 的全链路。"),
          mlTr("1. 从课程列表挑一节感兴趣的\n2. 按章节顺序学下来\n3. 配合「实验手册」做配套实验"),
          mlTr("课程 / 章节 / 教学内容")}},
        // ---- 编译前端（补齐缺失面板）----
        {QStringLiteral("ast-editor"),
         {mlTr("直接在画布上拖拽、编辑 AST 节点，实时看到树结构变化对执行结果的影响。比「AST 构建器」更"
               "自由——随便改、随便试。"),
          mlTr("1. 先用「AST 构建器」理解基本结构\n2. 在本面板自由编辑节点\n3. 改完跑一下看结果对不对"),
          mlTr("AST / 节点编辑 / 实时执行")}},
        {QStringLiteral("lint-explorer"),
         {mlTr("把静态分析规则和它触发的警告摆出来，让你看到编译器在不跑代码的情况下能查出什么问题——"
               "未使用变量、类型不匹配、可能的 Bug。"),
          mlTr("1. 先搞懂语法规则\n2. 在本面板写点带「坑」的代码\n3. 看每条警告对应哪条规则"),
          mlTr("静态分析 / Lint / 警告 / 类型检查")}},
        // ---- 执行引擎（补齐缺失面板）----
        {QStringLiteral("jit-visualizer"),
         {mlTr("演示热点代码如何被 JIT 编译器识别并编译成更快的本地代码，让你看清「解释执行 → 热点检测 → "
               "编译优化」的完整链路。"),
          mlTr("1. 先搞懂 VM 基本执行\n2. 在本面板跑一段循环代码\n3. 观察热点被识别和编译的过程"),
          mlTr("JIT / 热点检测 / 即时编译 / 优化")}},
        {QStringLiteral("step-explainer"),
         {mlTr("把每条字节码执行时的「为什么这么做」用大白话讲出来——不只是显示状态，还解释指令的语义和"
               "设计意图。"),
          mlTr("1. 先在「字节码追踪」看状态变化\n2. 卡住的指令来本面板查讲解\n3. 配合「VM 沙盒」动手验证"),
          mlTr("指令语义 / 执行讲解 / 字节码")}},
        {QStringLiteral("backend-parallel"),
         {mlTr("让 Interpreter / StackVM / RegisterVM 三条路径同时跑同一份代码，并排显示各自的执行进度"
               "——谁先到、谁卡住，一目了然。"),
          mlTr("1. 先用「三后端对比」理解差异\n2. 在本面板观察并行执行\n3. 关注分叉点和汇合点"),
          mlTr("三后端 / 并行执行 / 进度对比")}},
        {QStringLiteral("inline-cache"),
         {mlTr("把内联缓存（IC）的命中 / 未命中 / 失效过程画出来，让你看清动态分派是怎么被加速的——为什"
               "么同样一个调用，第二次就变快了。"),
          mlTr("1. 先搞懂方法分派机制\n2. 在本面板反复调用同一方法\n3. "
               "观察缓存从 monomorphic 到 polymorphic 的演化"),
          mlTr("内联缓存 / 动态分派 / monomorphic / polymorphic")}},
        {QStringLiteral("loop-unrolling"),
         {mlTr("把循环展开优化前后的代码和执行轨迹摆在一起，让你看清为什么展开能减少分支开销——以及展开"
               "过度会怎样。"),
          mlTr("1. 先看「IR 优化回放」理解基本优化\n2. 在本面板对比展开前后\n3. 调整展开因子观察收益变化"),
          mlTr("循环展开 / 优化 / 分支开销")}},
        {QStringLiteral("escape-analysis"),
         {mlTr("演示逃逸分析如何判断对象该分配在栈上还是堆上——能栈上分配就不用堆，省得给 GC 增加负担。"),
          mlTr("1. 先搞懂栈 vs 堆的区别\n2. 在本面板写不同作用域的对象\n3. 观察哪些对象逃逸到堆"),
          mlTr("逃逸分析 / 栈上分配 / 堆分配 / GC")}},
        {QStringLiteral("register-allocator"),
         {mlTr("把寄存器分配的图着色算法过程演给你看——哪些变量挤同一个寄存器、哪些溢出到内存，看分配器"
               "怎么权衡。"),
          mlTr("1. 先搞懂寄存器 VM 的寄存器概念\n2. 在本面板观察着色过程\n3. 看溢出（spill）发生在哪里"),
          mlTr("寄存器分配 / 图着色 / 溢出 / 寄存器 VM")}},
        {QStringLiteral("performance-race"),
         {mlTr("让三后端跑同一段代码比谁快，用赛道动画直观呈现耗时差距——不只是数字，是看得见的速度差。"),
          mlTr("1. 先用「三后端对比」理解语义差异\n2. 在本面板选一段代码开赛\n3. 分析赢家为什么赢"),
          mlTr("性能竞赛 / 耗时对比 / 三后端")}},
        {QStringLiteral("memory-layout"),
         {mlTr("把结构体、数组、对象在内存里的字节布局画出来——对齐、填充、偏移量，一眼看清。"),
          mlTr("1. 先搞懂基本数据类型大小\n2. 在本面板查看不同结构的布局\n3. 注意对齐和填充的开销"),
          mlTr("内存布局 / 对齐 / 填充 / 偏移量")}},
        {QStringLiteral("gc-visualizer"),
         {mlTr("把 GC 的 mark / sweep / compact 过程做成动画，让你看清垃圾是怎么被找出来、怎么被回收的"
               "——配合「内存模型」看更清楚。"),
          mlTr("1. 先看「内存模型」搞懂堆\n2. 在本面板触发 GC\n3. 观察 mark 和 sweep 阶段"),
          mlTr("GC / mark-sweep / compact / 垃圾回收")}},
        {QStringLiteral("watch-expressions"),
         {mlTr("让你在调试时添加任意表达式实时求值，比单纯看变量更灵活——可以盯着 a + b * c 这种组合表达"
               "式的变化。"),
          mlTr("1. 先搞懂基本调试断点\n2. 在本面板添加想观察的表达式\n3. 单步执行看值怎么变"),
          mlTr("观察表达式 / 调试 / 实时求值")}},
        {QStringLiteral("execution-timeline"),
         {mlTr("把整个执行过程录成时间轴，可以拖回去看任意时刻的状态——调试时不再只能往前走，可以倒带。"),
          mlTr("1. 先跑一遍代码录制时间轴\n2. 拖动时间指针回到任意时刻\n3. 配合「变量检查器」看历史状态"),
          mlTr("时间轴 / 回放 / 历史状态 / 调试")}},
        // ---- 执行引擎（AUDIT-P2 fix: 补齐目录已登记但文案缺失的两个面板）----
        {QStringLiteral("watchpoint"),
         {mlTr("数据断点（Watchpoint）：盯住某个变量或字段，一旦它被修改就立刻暂停——不用猜「到底是谁"
               "改了我的变量」，让调试器当场抓住那只手。"),
          mlTr("1. 先搞懂普通断点怎么用\n2. 在本面板添加要监视的变量\n3. 运行代码，看哪行代码改了它"),
          mlTr("数据断点 / Watchpoint / 变量监视 / 调试")}},
        {QStringLiteral("reverse-timeline"),
         {mlTr("反向调试时间轴：把执行历史摆成一排可点击的历史步，点哪步就回滚到哪步——调试不再是"
               "单行道，走过头了可以倒回去重来。需先在「可回放执行时间轴」开启录制。"),
          mlTr("1. 先在「可回放执行时间轴」开启录制并跑一遍代码\n2. 回本面板点击任意历史步回滚\n3. "
               "回滚后继续单步，验证不同分支的行为"),
          mlTr("反向调试 / 状态回滚 / 快照 / FullState 录制")}},
        {QStringLiteral("coroutine-visualizer"),
         {mlTr("把协程和生成器的挂起 / 恢复过程画出来，让你看清 yield 时到底保存了什么、resume 时怎么回"
               "到原处。"),
          mlTr("1. 先搞懂函数调用栈\n2. 在本面板写个生成器\n3. 观察 yield/Resume 的状态切换"),
          mlTr("协程 / 生成器 / yield / 挂起恢复")}},
        // ---- 深入实战（补齐缺失面板）----
        {QStringLiteral("fuzz-playground"),
         {mlTr("自动生成大量随机输入喂给你的代码，看哪些输入能让代码崩溃或行为异常——找 Bug 不用全靠手"
               "，让机器帮你撞。"),
          mlTr("1. 先写一个目标函数\n2. 在本面板启动模糊测试\n3. 分析崩溃样本找根因"),
          mlTr("模糊测试 / 随机输入 / 崩溃样本 / Bug 发现")}},
        {QStringLiteral("module-system"),
         {mlTr("把模块之间的 import / export 依赖关系画成图，循环导入、缺失导出、路径解析错误一眼现形。"),
          mlTr("1. 先写几个互相 import 的模块\n2. 在本面板看依赖图\n3. 检查有没有循环导入"),
          mlTr("模块 / import / export / 依赖图 / 循环导入延迟加载")}},
        {QStringLiteral("exercise-grader"),
         {mlTr("给你一道题，你写代码，它当场判对错并给反馈——比「实验手册」更强调即时的对错验证。"),
          mlTr("1. 从题库挑一道题\n2. 在编辑器里写答案\n3. 点评分看反馈和提示"),
          mlTr("练习 / 评分 / 即时反馈 / 自动判题")}},
    };
    return docs;
}

} // namespace

// ============================================================
// UX-R fix: 帮助文案对外查询 + 引导锚点按钮访问器
// ------------------------------------------------------------
// 通用兜底引导（Ide::createGenericPanelTour）复用 helpDocs 数据源，
// 保证「每个有帮助文档的面板都有新手引导」的不变量。
// ============================================================

bool TeachingPanelHeader::helpDocFor(const QString& panelId, HelpDocView* out) {
    const auto& docs = helpDocs();
    auto it = docs.find(panelId);
    if (it == docs.end())
        return false;
    if (out) {
        out->purpose = it.value().purpose;
        out->recommendedOrder = it.value().recommendedOrder;
        out->relatedConcepts = it.value().relatedConcepts;
    }
    return true;
}

QWidget* TeachingPanelHeader::helpButton() const {
    return helpBtn_;
}

QWidget* TeachingPanelHeader::learningPathButton() const {
    return learningPathBtn_;
}

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
    // P3-18 fix: 颜色从硬编码迁移到 TeachingTheme 语义色（主题色变更自动跟随）
    setStyleSheet(QStringLiteral("#teachingPanelHeader {"
                                 "  background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
                                 "    stop:0 %1, stop:1 %2);"
                                 "  border-bottom: 1px solid %3;"
                                 "}"
                                 "#teachingPanelTitle {"
                                 "  font-size: 14px;"
                                 "  font-weight: 600;"
                                 "  color: %4;"
                                 "}"
                                 "#teachingHeaderBtn:hover {"
                                 "  background: %1;"
                                 "  border-radius: 4px;"
                                 "}"
                                 // 「新手引导」按钮用主题色强调，提示新手可点击获取引导
                                 "#teachingTourBtn {"
                                 "  background: %5;"
                                 "  color: %6;"
                                 "  border: 1px solid %7;"
                                 "  border-radius: 4px;"
                                 "  padding: 2px 10px;"
                                 "  font-weight: 600;"
                                 "}"
                                 "#teachingTourBtn:hover {"
                                 "  background: %7;"
                                 "}"
                                 "#teachingTourBtn:pressed {"
                                 "  background: %8;"
                                 "}"
                                 // 「返回编辑器」按钮用浅色边框强调，方便用户从教学面板切回编辑器
                                 "#teachingBackBtn {"
                                 "  background: %2;"
                                 "  color: %4;"
                                 "  border: 1px solid %3;"
                                 "  border-radius: 4px;"
                                 "  padding: 2px 10px;"
                                 "  font-weight: 600;"
                                 "}"
                                 "#teachingBackBtn:hover {"
                                 "  background: %9;"
                                 "  border-color: %5;"
                                 "  color: %5;"
                                 "}"
                                 "#teachingBackBtn:pressed {"
                                 "  background: %9;"
                                 "}")
                      .arg(TeachingTheme::primaryHoverBg().name(),   // %1 渐变起点/hover 淡蓝
                           TeachingTheme::surface().name(),          // %2 渐变终点/返回钮背景
                           TeachingTheme::border().name(),           // %3 分隔线/边框
                           TeachingTheme::textPrimary().name(),      // %4 标题/正文色
                           TeachingTheme::primary().name(),          // %5 主题色
                           TeachingTheme::onPrimary().name(),        // %6 主色上前景
                           TeachingTheme::primaryHover().name(),     // %7 主色 hover
                           TeachingTheme::primaryPressed().name(),   // %8 主色 pressed
                           TeachingTheme::surfaceHover().name()));   // %9 返回钮 hover 背景
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
    // UX-R fix: 硬编码颜色迁移到 TeachingTheme 语义色
    const QString kTipLine =
        QStringLiteral("<hr style='border: none; border-top: 1px dashed %1; margin: 14px 0 8px 0;'/>"
                       "<p style='margin: 0; font-size: 12px; color: %2;'>%3 提示：按 Esc 关闭，"
                       "拖拽角落可调整窗口大小</p>")
            .arg(TeachingTheme::border().name(), TeachingTheme::textHint().name(), kTipEmoji);

    // 中性白 QSS：背景 surface，文字 textPrimary，padding 12px（R74: 回退 Solarized 米黄）
    // 「知道了」按钮保持 PrimaryButton 自带 Fluent 主色样式，不覆盖
    // UX-R fix: 硬编码颜色迁移到 TeachingTheme 语义色
    const QString kDialogQss = QStringLiteral("QDialog#helpDialog {"
                                              "  background: %1;"
                                              "}"
                                              "QTextBrowser#helpBrowser {"
                                              "  background: %1;"
                                              "  color: %2;"
                                              "  border: 1px solid %3;"
                                              "  border-radius: 6px;"
                                              "  padding: 12px;"
                                              "}")
                                   .arg(TeachingTheme::surface().name(), TeachingTheme::textPrimary().name(),
                                        TeachingTheme::border().name());

    const auto& docs = helpDocs();
    auto it = docs.find(panelId_);
    // 非模态复用：若已有打开的帮助对话框，先关闭再重建（内容可能随面板切换更新）
    if (helpDlg_) {
        helpDlg_->close();
        helpDlg_->deleteLater();
        helpDlg_ = nullptr;
    }
    if (it == docs.end()) {
        // 兜底情况：未知 panelId，改用 QTextBrowser 替代 BodyLabel，确保长文本可滚动
        helpDlg_ = new QDialog(this);
        QDialog* dlg = helpDlg_;
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
        browser->setHtml(QStringLiteral("<html><body style='font-family: \"Microsoft YaHei\",\"PingFang SC\",\"Noto "
                                        "Sans CJK SC\",\"Source Han Sans SC\",\"Segoe UI\",\"SF Pro Text\",\"Arial "
                                        "Unicode MS\",\"Arial\",sans-serif; font-size: 13px; line-height: 1.7;'>"
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
        // 非模态显示（用户可边看帮助边操作面板）
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();
        return;
    }

    const HelpDoc& doc = it.value();

    // UX-R fix: 难度分级 + 所属分类提示行（从 PanelCatalog 派生，与导航树徽标一致），
    // 初学者打开帮助即知该面板处于学习曲线的哪一段、是否适合现在学。
    QString levelLine;
    if (const PanelEntry* entry = PanelCatalog::findById(panelId_.toStdString())) {
        const QString levelName = mlTr(PanelCatalog::levelName(entry->level));
        const QString catTitle = QString::fromStdString(PanelCatalog::categoryTitleOf(panelId_.toStdString()));
        levelLine = QStringLiteral("<p style='margin: 0 0 10px 0; font-size: 12px; color: %1;'>"
                                   "🎯 难度：%2 · 分类：%3</p>")
                        .arg(TeachingTheme::textHint().name(), levelName,
                             catTitle.isEmpty() ? mlTr("教学面板") : mlTr(catTitle.toUtf8().constData()));
    }

    helpDlg_ = new QDialog(this);
    QDialog* dlg = helpDlg_;
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
    // 帮助文案通过 MarkdownRenderer 统一渲染（**粗体** / `代码` / 有序列表），
    // 与其余教学面板的富文本阅读体验保持一致（替代旧的 toHtmlEscaped 纯文本）。
    // UX-R fix: 标题色/次要色从硬编码（#268BD2/#5A5A5A）迁移到 TeachingTheme 语义色。
    const QString kHeadColor = TeachingTheme::primary().name();
    browser->setHtml(QStringLiteral("<html><body style='font-family: \"Microsoft YaHei\",\"PingFang SC\",\"Noto Sans "
                                    "CJK SC\",\"Source Han Sans SC\",\"Segoe UI\",\"SF Pro Text\",\"Arial Unicode "
                                    "MS\",\"Arial\",sans-serif; font-size: 13px; line-height: 1.7;'>"
                                    "%1"
                                    "<h3 style='color: %2; margin-bottom: 4px;'>📋 面板用途</h3>"
                                    "<div style='margin: 0 0 12px 0;'>%3</div>"
                                    "<h3 style='color: %2; margin-bottom: 4px;'>🔢 推荐使用顺序</h3>"
                                    "<div style='margin: 0 0 12px 0;'>%4</div>"
                                    "<h3 style='color: %2; margin-bottom: 4px;'>💡 关联概念</h3>"
                                    "<div style='margin: 0; color: %5;'>%6</div>"
                                    "%7"
                                    "</body></html>")
                         .arg(levelLine)
                         .arg(kHeadColor)
                         .arg(MarkdownRenderer::markdownToHtmlFragment(doc.purpose))
                         .arg(MarkdownRenderer::markdownToHtmlFragment(doc.recommendedOrder))
                         .arg(TeachingTheme::textSecondary().name())
                         .arg(MarkdownRenderer::markdownToHtmlFragment(doc.relatedConcepts))
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
    // 非模态显示（用户可边看帮助边操作面板）
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
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
