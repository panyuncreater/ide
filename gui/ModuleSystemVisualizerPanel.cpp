// ============================================================
// ModuleSystemVisualizerPanel.cpp — 模块系统可视化教学面板实现
// ------------------------------------------------------------
// 纯教学/模拟面板，不运行 MiniLang 执行引擎。包含：
//   1. 模块系统原理静态文档（import/export 语义 + 路径解析规则 +
//      模块缓存 + 循环导入延迟加载 + 预扫描阶段 + 错误传播链）
//   2. 交互式模块依赖模拟器：4 个预设场景，纯 C++ 模拟加载栈与模块缓存
//
//   展示正常导入链 / 循环导入延迟加载 / 路径解析失败 /
//   预扫描遗漏的处理流程
//
// 模块系统是 MiniLang 的高频 Bug 模式，本面板以预设数据教学这些概念。
// ============================================================

#include "gui/ModuleSystemVisualizerPanel.h"
#include "gui/GuidedTour.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextBrowser>
#include <QVBoxLayout>

#include <string>
#include <vector>

#include "gui/GuiTextUtils.h"
#include "gui/I18n.h"
#include "gui/TeachingSubPageBar.h"

// ============================================================
// 匿名命名空间 — 辅助函数
// ============================================================

namespace {

/// HTML 转义（避免代码片段中的 < > & 破坏 HTML 结构）
QString htmlEscape(const std::string& s) {
    return QString::fromUtf8(s.c_str()).toHtmlEscaped();
}

/// 根据状态返回对应的显示颜色
QString statusColor(const std::string& status) {
    if (status == "成功") {
        return QStringLiteral("#0a7a28");
    }
    if (status == "循环") {
        return QStringLiteral("#b8860b");
    }
    if (status == "失败") {
        return QStringLiteral("#c0392b");
    }
    if (status == "跳过(已缓存)") {
        return QStringLiteral("#666666");
    }
    return QStringLiteral("#333333");
}

/// 构造一个模块节点 HTML 盒子（用 table 单元格 +
/// 边框/背景色模拟）
QString moduleNodeHtml(const std::string& name, const QString& bg, const QString& border) {
    return QStringLiteral("<td style='border:2px solid %1;background:%2;padding:8px 14px;"
                          "font-family:%3;font-size:13px;font-weight:bold;'>%4</td>")
        .arg(border, bg, GuiTextUtils::monoFontFamilyQss(), htmlEscape(name));
}

/// 构造一个箭头单元格（→ 或 ⇄）
QString arrowCell(const QString& sym) {
    return QStringLiteral("<td style='font-size:20px;padding:4px 8px;color:#555;font-weight:bold;'>%1</td>").arg(sym);
}

} // namespace

// ============================================================
// ModuleSystemLibrary — 静态教学数据
// ============================================================

const std::vector<ModuleConceptRow>& ModuleSystemLibrary::conceptRows() {
    static const std::vector<ModuleConceptRow> kRows = {
        {"import 语句",
         "导入模块：解析路径 → 预扫描目标模块 → 执行 → 绑定导出符号到当前作用域。"
         "import 在词法/语法阶段生成 ImportStmt AST 节点。",
         "Parser::importDecl / Interpreter/VM 执行 ImportStmt"},
        {"export 语句",
         "导出符号：将 FunDecl / VarDecl 标记为模块对外可见。预扫描阶段收集 ExportStmt 以"
         "便 import 端能正确绑定。漏收集会导致 import 端找不到符号。"
         "export 在词法/语法阶段生成 ExportStmt AST 节点。",
         "Parser::exportDecl / 模块预扫描 collectExports"},
        {"路径解析",
         "import 路径解析：相对路径基于当前模块目录，自动补全 .ml "
         "扩展名；禁止逃逸到项目根之外的绝对路径（路径安全）。解析失败立即报错并传播。"
         "路径安全策略防止逃逸到项目根目录之外。",
         "ModuleLoader::resolvePath"},
        {"模块缓存",
         "模块按「规范路径」缓存到 moduleCache_，避免重复加载与执行。"
         "二次 import 同一模块直接命中缓存（跳过已缓存），保证模块级单例语义。"
         "缓存命中（已加载完成）合法，与「正在加载中」是两种不同状态。",
         "ModuleLoader::moduleCache_ / loadOrCached"},
        {"循环导入延迟加载",
         "维护「加载中」栈 loadingStack_，进入 import 递归前压栈，完成出栈。"
         "若目标模块已在栈中则判定为循环导入，不再报错——返回已创建的部分 env（P2-14）。"
         "模块环境立即入缓存，内容按执行顺序填充。访问未定义导出名时由 Environment::get 抛\"未定义变量\"错误。"
         "语义参考 ES Modules + Python。",
         "ModuleLoader::loadingStack_ / moduleCache_ / moduleExports_"},
        {"预扫描",
         "模块执行前先遍历顶层 AST，收集 FunDecl 与 ExportStmt 声明并预注册到模块作用域，"
         "使函数定义顺序无关（前向引用）。VarDecl/ClassDecl/EnumDecl 不预扫描，运行时处理。"
         "预扫描遗漏某节点类型会导致运行时找不到符号（历史高频 Bug 模式）。",
         "ModuleLoader::prescan / preRegisterDeclarations"},
        {"错误传播",
         "模块加载任何阶段失败（路径解析/预扫描/执行）均抛出 ModuleError，"
         "沿 import 调用链向上传播，最终在顶层报告并停止执行。错误不被吞掉。"
         "若 import 失败后仍继续执行，会导致后续访问未定义符号的级联错误，掩盖根因。",
         "ImportStmt 执行路径 / ModuleError 异常类"},
        {"模块隔离",
         "每个模块拥有独立 Environment 作用域，模块顶层 VarDecl 不泄漏到 import 端，"
         "仅显式 export 的符号可见。boundInstance_ 缓存优化模块方法绑定。"
         "模块隔离保证未导出的顶层声明对 import 端不可见。",
         "ModuleScope / Environment 链式作用域"},
    };
    return kRows;
}

const std::vector<PathResolutionRow>& ModuleSystemLibrary::pathResolutionRows() {
    static const std::vector<PathResolutionRow> kRows = {
        {"./foo", "./foo.ml", "显式相对路径，基于当前模块目录，自动补全 .ml 扩展名。"},
        {"bar", "./bar.ml", "无前缀默认按相对路径处理，等价于 ./bar，补全扩展名。"},
        {"../utils", "../utils.ml", ".. 退到父目录，仍受路径安全约束（禁止逃逸项目根）。"},
        {"sub/mod", "./sub/mod.ml", "子目录路径，按层级解析，补全末端 .ml。"},
        {"nonexist", "(失败)", "目标文件不存在，路径解析失败，立即抛 ModuleError 并沿 import 链传播。"},
        {"/abs/path", "(失败)", "绝对路径被路径安全策略拒绝，禁止逃逸到项目根目录之外。"},
    };
    return kRows;
}

const std::vector<PrescanCheckRow>& ModuleSystemLibrary::prescanCheckRows() {
    static const std::vector<PrescanCheckRow> kRows = {
        {"FunDecl", "是", "函数声明预注册，支持前向引用（先调用后定义）；漏收集会导致调用时未定义。"},
        {"ExportStmt", "是", "导出声明预收集，import 端据此绑定符号；漏收集会导致 import 端找不到导出。"},
        {"VarDecl", "否", "变量声明运行时按顺序执行（初始化表达式有副作用），不预扫描。"},
        {"ClassDecl", "否", "类声明运行时按顺序求值基类与字段默认值，不预扫描。"},
        {"EnumDecl", "否", "枚举声明运行时按顺序构造 variant，不预扫描。"},
    };
    return kRows;
}

const std::vector<ModuleScenarioInfo>& ModuleSystemLibrary::scenarios() {
    static const std::vector<ModuleScenarioInfo> kScenarios = {
        {"场景 1：正常导入链",
         "a.ml imports b.ml imports c.ml，展示 DFS 深度优先加载顺序与后序执行（c→b→a）。"
         "模块缓存确保每个模块仅加载执行一次，预扫描使函数可前向引用。",
         "// a.ml\nimport b;\nfun main() { b.greet(); }\n\n"
         "// b.ml\nimport c;\nfun greet() { print(c.msg()); }\n\n"
         "// c.ml\nfun msg() { return \"hello from c\"; }"},
        {"场景 2：循环导入延迟加载",
         "a.ml imports b.ml imports a.ml，加载栈探测到 a.ml 正在加载中，"
         "不再报错——返回已创建的部分 env（P2-14 延迟加载语义）。"
         "模块环境立即入缓存，内容按执行顺序填充。访问未定义名时由 Environment::get 抛\"未定义变量\"错误。",
         "// a.ml\nimport b;\nexport fun a() { b.b(); }\n\n"
         "// b.ml\nimport a;   // 循环：a 正在加载，返回部分 env\nexport fun b() { a.a(); }"},
        {"场景 3：路径解析失败",
         "a.ml imports nonexist，路径解析失败（文件不存在），ModuleError 沿 import 链"
         "向上传播，a.ml 加载随之失败。",
         "// a.ml\nimport nonexist;  // 文件不存在\nfun main() { nonexist.x(); }"},
        {"场景 4：预扫描遗漏",
         "badprescan.ml 中 FunDecl helper 未被预扫描收集，main 调用 helper 时"
         "因未预注册而运行时失败。正确预扫描应 hoist 所有 FunDecl 以支持前向引用。",
         "// badprescan.ml（模拟预扫描遗漏）\nexport fun main() { helper(); }\nfun helper() { print(\"ok\"); }\n"
         "// 预扫描应收集 helper，遗漏后 main 调用 helper 失败"},
    };
    return kScenarios;
}

// ============================================================
// ModuleSystemVisualizerPanel 构造
// ============================================================

ModuleSystemVisualizerPanel::ModuleSystemVisualizerPanel(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);
    outer->setSpacing(4);

    // 顶部：子页切换按钮（统一组件 TeachingSubPageBar）
    subPageBar_ = new TeachingSubPageBar(this);
    auto* theoryPage = new QWidget;
    auto* simulatorPage = new QWidget;
    buildTheoryPage(theoryPage);
    buildSimulatorPage(simulatorPage);
    subPageBar_->addPage(mlTr("① 模块系统原理"), theoryPage);
    subPageBar_->addPage(mlTr("② 交互式模块依赖模拟器"), simulatorPage);
    pageTheoryBtn_ = subPageBar_->buttonAt(0);
    pageSimulatorBtn_ = subPageBar_->buttonAt(1);
    stack_ = subPageBar_->stack();
    outer->addLayout(subPageBar_->buttonBar());
    outer->addWidget(stack_, 1);
    // 恢复上次子页选择
    subPageBar_->restoreFromSettings(QStringLiteral("teachingPanel/module-system/subPage"));

    // 信号连接：子页切换持久化 + 模拟器交互
    connect(subPageBar_, &TeachingSubPageBar::currentChanged, this, [this](int idx) {
        subPageBar_->saveToSettings(QStringLiteral("teachingPanel/module-system/subPage"));
        onSwitchPage(idx);
    });
    connect(runBtn_, &QPushButton::clicked, this, &ModuleSystemVisualizerPanel::onRunSimulation);
    connect(loadToMainBtn_, &QPushButton::clicked, this, &ModuleSystemVisualizerPanel::onLoadSample);

    // 初始数据填充
    populateTheory();
    populatePresets();

    // 默认显示子页 1
    onSwitchPage(0);
}

// ============================================================
// 子页 1：模块系统原理
// ============================================================

void ModuleSystemVisualizerPanel::buildTheoryPage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    // 理论概览
    theoryBrowser_ = new QTextBrowser;
    theoryBrowser_->setFont(GuiTextUtils::monospaceFont(10));
    layout->addWidget(theoryBrowser_, 2);

    // 概念表
    layout->addWidget(new QLabel(mlTr("<b>模块系统核心概念</b>")));
    conceptTable_ = new QTableWidget(0, 3);
    conceptTable_->setHorizontalHeaderLabels({mlTr("概念"), mlTr("说明"), mlTr("关键代码位置")});
    conceptTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    conceptTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    conceptTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    conceptTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    conceptTable_->verticalHeader()->setVisible(false);
    conceptTable_->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(conceptTable_, 2);

    // 路径解析规则表
    layout->addWidget(new QLabel(mlTr("<b>路径解析规则</b>")));
    pathTable_ = new QTableWidget(0, 3);
    pathTable_->setHorizontalHeaderLabels({mlTr("输入"), mlTr("解析结果"), mlTr("说明")});
    pathTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    pathTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    pathTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    pathTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    pathTable_->verticalHeader()->setVisible(false);
    pathTable_->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(pathTable_, 1);

    // 预扫描清单
    layout->addWidget(new QLabel(mlTr("<b>预扫描清单（顶层节点收集策略）</b>")));
    prescanTable_ = new QTableWidget(0, 2);
    prescanTable_->setHorizontalHeaderLabels({mlTr("节点类型"), mlTr("是否预扫描收集")});
    prescanTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    prescanTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    prescanTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    prescanTable_->verticalHeader()->setVisible(false);
    prescanTable_->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(prescanTable_, 1);
}

void ModuleSystemVisualizerPanel::populateTheory() {
    // 理论概览 HTML
    QString html = QStringLiteral(
        "<h2>MiniLang 模块系统原理</h2>"
        "<p>模块系统通过 <b>import / export</b> 支持跨文件代码组织。一个模块从源码到可用"
        "需经历：<b>Lexer → Parser → 预扫描 → 模块缓存 → 循环导入延迟加载 → 执行</b>。"
        "模块系统是 MiniLang 的高频 Bug 模式（路径安全、循环导入、预扫描遗漏 FunDecl/ExportStmt、"
        "错误传播），理解每一阶段的不变量是排查此类 Bug 的关键。</p>"

        "<h3>一、加载管线</h3>"
        "<ol>"
        "<li><b>词法/语法分析</b>：import / export 生成 ImportStmt / ExportStmt AST 节点。</li>"
        "<li><b>路径解析</b>：import 路径基于当前模块目录，自动补全 .ml；路径安全禁止逃逸项目根。</li>"
        "<li><b>循环导入延迟加载</b>：目标模块入「加载栈」前检查是否已在栈中，是则返回部分 env（P2-14）。</li>"
        "<li><b>预扫描</b>：遍历顶层 AST，收集 FunDecl / ExportStmt 并预注册到模块作用域，"
        "支持前向引用。VarDecl/ClassDecl/EnumDecl 不预扫描。</li>"
        "<li><b>模块缓存</b>：以规范路径为 key 缓存已加载模块，二次 import 命中缓存跳过执行。</li>"
        "<li><b>执行</b>：后序执行模块顶层语句；import 端绑定 export 的符号到当前作用域。</li>"
        "</ol>"

        "<h3>二、import / export 语义</h3>"
        "<ul>"
        "<li><b>import \"foo\"</b>：加载 foo.ml，执行后将其 export 的符号绑定到当前作用域。</li>"
        "<li><b>export fun name() {...}</b>：声明并导出函数，预扫描阶段即被收集。</li>"
        "<li><b>export var x = ...</b>：导出变量，其值在模块执行后确定。</li>"
        "<li>未 export 的顶层声明对 import 端不可见（模块隔离）。</li>"
        "</ul>"

        "<h3>三、模块缓存与循环导入延迟加载</h3>"
        "<p>模块按规范路径缓存，保证「每个模块仅执行一次」的模块级单例语义。"
        "循环导入通过「加载栈」检测：进入 import 递归前压栈，完成出栈。"
        "若目标已在栈中即 a→b→a，P2-14 后不再报错——返回已创建的部分 env，"
        "模块环境立即入缓存，内容按执行顺序填充。访问未定义导出名时由 Environment::get 抛\"未定义变量\"错误。"
        "语义参考 ES Modules + Python。注意缓存命中（已加载完成）"
        "与「正在加载中」（栈中）是两种不同状态，前者返回完整 env 后者返回部分 env。</p>"

        "<h3>四、预扫描的必要性</h3>"
        "<p>预扫描收集 FunDecl/ExportStmt 使<b>函数定义顺序无关</b>——"
        "先调用后定义也能工作（前向引用）。若预扫描遗漏某节点类型（历史 Bug 模式），"
        "则调用方在运行时找不到该符号，报「未定义」错误。VarDecl "
        "不预扫描因为初始化表达式有副作用，必须按顺序执行。</p>"

        "<h3>五、错误传播链</h3>"
        "<p>模块加载任意阶段失败（路径解析 / 预扫描 / 执行）均抛 ModuleError，"
        "沿 import 调用链向上传播，最终在顶层报告并停止。错误<b>不应被吞掉</b>。"
        "若 import 失败后仍继续执行，会导致后续访问未定义符号的级联错误，掩盖根因。</p>"

        "<h3>教学价值</h3>"
        "<p>本面板可视化模块系统全流程，帮助理解：</p>"
        "<ul>"
        "<li>DFS 加载顺序与后序执行的因果</li>"
        "<li>加载栈如何实现循环导入延迟加载</li>"
        "<li>预扫描遗漏导致的运行时失败模式</li>"
        "<li>错误传播链对调试的重要性</li>"
        "</ul>"
        "<p style='color:#666;font-size:small;'>"
        "提示：切换到「② 交互式模块依赖模拟器」子页，选择预设场景体验加载流程。</p>");
    theoryBrowser_->setHtml(html);

    // 概念表
    const auto& concepts = ModuleSystemLibrary::conceptRows();
    conceptTable_->setRowCount(static_cast<int>(concepts.size()));
    for (int i = 0; i < static_cast<int>(concepts.size()); ++i) {
        const auto& c = concepts[i];
        conceptTable_->setItem(i, 0, new QTableWidgetItem(QString::fromUtf8(c.conceptName.c_str())));
        conceptTable_->setItem(i, 1, new QTableWidgetItem(QString::fromUtf8(c.description.c_str())));
        conceptTable_->setItem(i, 2, new QTableWidgetItem(QString::fromUtf8(c.keyLocation.c_str())));
        conceptTable_->item(i, 1)->setToolTip(QString::fromUtf8(c.description.c_str()));
        conceptTable_->item(i, 2)->setToolTip(QString::fromUtf8(c.keyLocation.c_str()));
    }

    // 路径解析规则表
    const auto& paths = ModuleSystemLibrary::pathResolutionRows();
    pathTable_->setRowCount(static_cast<int>(paths.size()));
    for (int i = 0; i < static_cast<int>(paths.size()); ++i) {
        const auto& p = paths[i];
        pathTable_->setItem(i, 0, new QTableWidgetItem(QString::fromUtf8(p.input.c_str())));
        pathTable_->setItem(i, 1, new QTableWidgetItem(QString::fromUtf8(p.resolved.c_str())));
        pathTable_->setItem(i, 2, new QTableWidgetItem(QString::fromUtf8(p.note.c_str())));
        pathTable_->item(i, 2)->setToolTip(QString::fromUtf8(p.note.c_str()));
        // 失败行高亮
        if (p.resolved == "(失败)") {
            for (int col = 0; col < 3; ++col) {
                pathTable_->item(i, col)->setForeground(QColor("#c0392b"));
            }
        }
    }

    // 预扫描清单
    const auto& checks = ModuleSystemLibrary::prescanCheckRows();
    prescanTable_->setRowCount(static_cast<int>(checks.size()));
    for (int i = 0; i < static_cast<int>(checks.size()); ++i) {
        const auto& ck = checks[i];
        prescanTable_->setItem(i, 0, new QTableWidgetItem(QString::fromUtf8(ck.nodeType.c_str())));
        prescanTable_->setItem(i, 1, new QTableWidgetItem(QString::fromUtf8(ck.prescanned.c_str())));
        // description 作为 tooltip 显示（鼠标悬停查看该详细信息）
        const QString desc = QString::fromUtf8(ck.description.c_str());
        prescanTable_->item(i, 0)->setToolTip(desc);
        prescanTable_->item(i, 1)->setToolTip(desc);
        // 「否」行用警告色提示不预扫描
        if (ck.prescanned == "否") {
            prescanTable_->item(i, 1)->setForeground(QColor("#b8860b"));
        } else {
            prescanTable_->item(i, 1)->setForeground(QColor("#0a7a28"));
        }
    }
}

// ============================================================
// 子页 2：交互式模块依赖模拟器
// ============================================================

void ModuleSystemVisualizerPanel::buildSimulatorPage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    // 顶部：预设场景选择 + 运行
    auto* inputBar = new QHBoxLayout;
    inputBar->addWidget(new QLabel(mlTr("预设场景：")));
    presetCombo_ = new QComboBox;
    presetCombo_->setMinimumWidth(220);
    inputBar->addWidget(presetCombo_);
    runBtn_ = new QPushButton(mlTr("运行模拟"));
    inputBar->addWidget(runBtn_);
    inputBar->addStretch();
    layout->addLayout(inputBar);

    // 中间：垂直分割器（上方时间轴 + 下方模拟过程 HTML）
    auto* vSplitter = new QSplitter(Qt::Vertical);

    // 上方：模块加载顺序时间轴（4 列）
    timelineTable_ = new QTableWidget(0, 4);
    timelineTable_->setHorizontalHeaderLabels({mlTr("步骤"), mlTr("模块"), mlTr("操作"), mlTr("状态")});
    timelineTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    timelineTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    timelineTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    timelineTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    timelineTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    timelineTable_->verticalHeader()->setVisible(false);
    timelineTable_->horizontalHeader()->setStretchLastSection(true);
    timelineTable_->setFont(GuiTextUtils::monospaceFont(10));
    vSplitter->addWidget(timelineTable_);

    // 下方：模拟过程详情（HTML 依赖图 + 加载日志）
    simBrowser_ = new QTextBrowser;
    simBrowser_->setFont(GuiTextUtils::monospaceFont(10));
    simBrowser_->setHtml(mlTr("<p style='color:#666;'>选择预设场景，点击「运行模拟」查看模块加载时间轴与"
                              "依赖图。</p>"));
    vSplitter->addWidget(simBrowser_);

    // 设置分割比例
    vSplitter->setStretchFactor(0, 2);
    vSplitter->setStretchFactor(1, 3);
    layout->addWidget(vSplitter, 1);

    // 底部：加载样例代码到主编辑器
    auto* bottomBar = new QHBoxLayout;
    bottomBar->addStretch();
    loadToMainBtn_ = new QPushButton(mlTr("加载样例到主编辑器"));
    bottomBar->addWidget(loadToMainBtn_);
    layout->addLayout(bottomBar);
}

void ModuleSystemVisualizerPanel::populatePresets() {
    presetCombo_->clear();
    for (const auto& s : ModuleSystemLibrary::scenarios()) {
        presetCombo_->addItem(QString::fromUtf8(s.title.c_str()));
    }
}

// ============================================================
// 模拟逻辑 — 4 个预设场景
// ============================================================

ModuleSimResult ModuleSystemVisualizerPanel::runScenario(int idx) {
    ModuleSimResult result;
    const auto& scenarios = ModuleSystemLibrary::scenarios();
    if (idx < 0 || idx >= static_cast<int>(scenarios.size())) {
        result.summary = "无效的场景索引";
        return result;
    }
    result.scenarioTitle = scenarios[idx].title;
    result.scenarioCode = scenarios[idx].codeSnippet;

    // 辅助 lambda：追加一步
    int stepNo = 0;
    auto addStep = [&](const std::string& mod, const std::string& op, const std::string& status) {
        ModuleSimStep s;
        s.step = ++stepNo;
        s.module = mod;
        s.operation = op;
        s.status = status;
        result.steps.push_back(s);
    };

    switch (idx) {
    case 0: {
        // 场景 1：正常导入链 a → b → c
        // DFS 深度优先加载，后序执行（c→b→a）
        addStep("a.ml", "开始加载", "成功");
        addStep("a.ml", "预扫描(收集 FunDecl/ExportStmt)", "成功");
        addStep("b.ml", "开始加载(import ./b)", "成功");
        addStep("b.ml", "预扫描", "成功");
        addStep("c.ml", "开始加载(import ./c)", "成功");
        addStep("c.ml", "预扫描", "成功");
        addStep("c.ml", "执行(后序:无依赖)", "成功");
        addStep("b.ml", "执行(c 已缓存)", "成功");
        addStep("a.ml", "执行(b 已缓存)", "成功");
        result.success = true;
        result.summary = "✅ 正常导入链加载成功：DFS 深度优先加载 a→b→c，"
                         "后序执行 c→b→a。模块缓存确保 c 仅执行一次，"
                         "预扫描使函数支持前向引用。";
        // 依赖图：a → b → c（绿色节点表示成功）
        result.dependencyGraphHtml = (QStringLiteral("<table cellpadding='0' cellspacing='0'><tr>") +
                                      moduleNodeHtml("a.ml", "#e8f5e9", "#0a7a28") + arrowCell("→") +
                                      moduleNodeHtml("b.ml", "#e3f2fd", "#1565c0") + arrowCell("→") +
                                      moduleNodeHtml("c.ml", "#fff3e0", "#ef6c00") +
                                      QStringLiteral("</tr></table>"
                                                     "<p style='color:#0a7a28;font-size:small;'>"
                                                     "绿色边框=成功加载，箭头=import 依赖方向（加载顺序），"
                                                     "执行顺序为 c→b→a（后序）</p>"))
                                         .toStdString();
        break;
    }
    case 1: {
        // 场景 2：循环导入延迟加载 a ⇄ b（P2-14）
        // 加载栈：[a] → import b → [a,b] → import a → a 已在栈中 → 返回部分 env
        addStep("a.ml", "开始加载", "成功");
        addStep("a.ml", "预扫描 + 立即入缓存", "成功");
        addStep("b.ml", "开始加载(import ./b)", "成功");
        addStep("b.ml", "预扫描 + 立即入缓存", "成功");
        addStep("a.ml", "import ./a → 已在加载栈中", "延迟加载");
        addStep("b.ml", "返回部分 env（a 尚未填充完）", "延迟加载");
        addStep("b.ml", "执行完毕", "成功");
        addStep("a.ml", "执行完毕", "成功");
        result.success = true;
        result.summary = "✅ 循环导入延迟加载（P2-14）：加载栈 [a, b] 中再次遇到 a.ml，"
                         "不再报错——返回已创建的部分 env。模块环境立即入缓存，内容按执行顺序填充。"
                         "访问未定义导出名时由 Environment::get 抛\"未定义变量\"错误。"
                         "语义参考 ES Modules + Python。";
        // 依赖图：a ⇄ b（橙色高亮表示循环延迟加载）
        result.dependencyGraphHtml = (QStringLiteral("<table cellpadding='0' cellspacing='0'><tr>") +
                                      moduleNodeHtml("a.ml", "#fff3e0", "#ef6c00") + arrowCell("⇄") +
                                      moduleNodeHtml("b.ml", "#fff3e0", "#ef6c00") +
                                      QStringLiteral("</tr></table>"
                                                     "<p style='color:#ef6c00;font-size:small;'>"
                                                     "橙色边框=循环导入延迟加载，⇄ 表示双向 import。"
                                                     "加载栈 [a.ml, b.ml] 中再次遇到 a.ml 返回部分 env</p>"))
                                         .toStdString();
        break;
    }
    case 2: {
        // 场景 3：路径解析失败 a → nonexist
        addStep("a.ml", "开始加载", "成功");
        addStep("a.ml", "预扫描", "成功");
        addStep("nonexist.ml", "路径解析失败(文件不存在)", "失败");
        addStep("a.ml", "错误传播:import 失败", "失败");
        result.success = false;
        result.summary = "❌ 路径解析失败：nonexist.ml 不存在，ModuleError 沿 import 链"
                         "向上传播，a.ml 加载随之失败。错误不被吞掉，避免后续访问"
                         "未定义符号的级联错误掩盖根因。";
        // 依赖图：a → nonexist（红色虚线边框表示未找到）
        result.dependencyGraphHtml =
            (QStringLiteral("<table cellpadding='0' cellspacing='0'><tr>") +
             moduleNodeHtml("a.ml", "#e8f5e9", "#0a7a28") + arrowCell("→") +
             QStringLiteral("<td style='border:2px dashed #c0392b;background:#ffebee;padding:8px 14px;"
                            "font-family:%1;font-size:13px;font-weight:bold;color:#c0392b;'>nonexist.ml ✗</td>")
                 .arg(GuiTextUtils::monoFontFamilyQss()) +
             QStringLiteral("</tr></table>"
                            "<p style='color:#c0392b;font-size:small;'>"
                            "虚线红边框=文件未找到，→ import 依赖方向。"
                            "路径解析失败立即抛错并沿 import 链传播</p>"))
                .toStdString();
        break;
    }
    case 3: {
        // 场景 4：预扫描遗漏 FunDecl
        // badprescan.ml 中 helper 未被预扫描收集，main 调用 helper 时失败
        addStep("badprescan.ml", "开始加载", "成功");
        addStep("badprescan.ml", "预扫描(遗漏 FunDecl helper)", "成功");
        addStep("badprescan.ml", "执行 export fun main()", "成功");
        addStep("badprescan.ml", "main 调用 helper() → 未定义", "失败");
        addStep("badprescan.ml", "错误传播:运行时未定义函数", "失败");
        result.success = false;
        result.summary = "❌ 预扫描遗漏 FunDecl helper：main 调用 helper 时因未预注册"
                         "而运行时失败。正确预扫描应 hoist 所有 FunDecl 以支持前向引用"
                         "（先调用后定义）。这是模块系统历史高频 Bug 模式。";
        // 依赖图：单节点 badprescan（橙色警告）
        result.dependencyGraphHtml =
            (QStringLiteral("<table cellpadding='0' cellspacing='0'><tr>") +
             moduleNodeHtml("badprescan.ml", "#fff8e1", "#b8860b") +
             QStringLiteral("</tr></table>"
                            "<table cellpadding='0' cellspacing='0' style='margin-top:8px;'><tr>"
                            "<td style='border:1px solid #bbb;background:#f7f7f7;padding:6px 10px;"
                            "font-family:%1;font-size:12px;'>export fun main() { helper(); }"
                            "  →  <span style='color:#c0392b;'>helper() 未预扫描收集</span>"
                            "  → 运行时失败</td></tr></table>"
                            "<p style='color:#b8860b;font-size:small;'>"
                            "橙色边框=预扫描异常。漏收集 FunDecl 导致前向引用失败</p>")
                 .arg(GuiTextUtils::monoFontFamilyQss()))
                .toStdString();
        break;
    }
    default:
        result.summary = "未知场景索引";
        break;
    }

    return result;
}

void ModuleSystemVisualizerPanel::renderScenarioResult(const ModuleSimResult& result) {
    // 1. 填充时间轴 QTableWidget
    timelineTable_->setRowCount(static_cast<int>(result.steps.size()));
    for (int i = 0; i < static_cast<int>(result.steps.size()); ++i) {
        const auto& s = result.steps[i];
        auto* stepItem = new QTableWidgetItem(QString::number(s.step));
        auto* modItem = new QTableWidgetItem(QString::fromUtf8(s.module.c_str()));
        auto* opItem = new QTableWidgetItem(QString::fromUtf8(s.operation.c_str()));
        auto* statusItem = new QTableWidgetItem(QString::fromUtf8(s.status.c_str()));
        stepItem->setTextAlignment(Qt::AlignCenter);
        statusItem->setTextAlignment(Qt::AlignCenter);
        statusItem->setForeground(QColor(statusColor(s.status)));
        timelineTable_->setItem(i, 0, stepItem);
        timelineTable_->setItem(i, 1, modItem);
        timelineTable_->setItem(i, 2, opItem);
        timelineTable_->setItem(i, 3, statusItem);
        timelineTable_->item(i, 2)->setToolTip(QString::fromUtf8(s.operation.c_str()));
    }

    // 2. 渲染模拟过程详情 HTML（依赖图 + 加载日志 + 代码）
    QString monoFamily = GuiTextUtils::monoFontFamilyQss();
    QString html;
    html += QStringLiteral("<html><body style='font-family:%1; font-size:14px;'>").arg(monoFamily);
    html += QStringLiteral("<h2>%1</h2>").arg(htmlEscape(result.scenarioTitle));

    if (!result.summary.empty()) {
        QString color = result.success ? QStringLiteral("#0a7a28") : QStringLiteral("#c0392b");
        html += QStringLiteral("<p style='color:%1;font-weight:bold;'>%2</p>").arg(color, htmlEscape(result.summary));
    }

    // 依赖图
    if (!result.dependencyGraphHtml.empty()) {
        html += QStringLiteral("<h3>模块依赖图</h3>");
        html += QString::fromUtf8(result.dependencyGraphHtml.c_str());
    }

    // 加载日志（步骤表）
    if (!result.steps.empty()) {
        html += QStringLiteral("<h3>加载日志</h3>");
        html += QStringLiteral("<table border='1' cellpadding='5' cellspacing='0' "
                               "style='font-family:%1;font-size:12px;border-collapse:collapse;'>")
                    .arg(monoFamily);
        html += QStringLiteral("<tr style='background:#f0f0f0;'><th>步骤</th><th>模块</th>"
                               "<th>操作</th><th>状态</th></tr>");
        for (const auto& s : result.steps) {
            html += QStringLiteral("<tr><td style='text-align:center;'>%1</td><td>%2</td><td>%3</td>"
                                   "<td style='text-align:center;color:%4;'>%5</td></tr>")
                        .arg(QString::number(s.step), htmlEscape(s.module), htmlEscape(s.operation),
                             statusColor(s.status), htmlEscape(s.status));
        }
        html += QStringLiteral("</table>");
    }

    // 模拟代码片段
    if (!result.scenarioCode.empty()) {
        html += QStringLiteral("<h3>场景代码</h3>");
        html += QStringLiteral("<pre style='background:#f7f7f7;padding:8px;border:1px solid #ddd;"
                               "font-family:%1;font-size:12px;white-space:pre-wrap;'>")
                    .arg(monoFamily);
        html += htmlEscape(result.scenarioCode);
        html += QStringLiteral("</pre>");
    }

    html += QStringLiteral("</body></html>");
    simBrowser_->setHtml(html);
}

// ============================================================
// 槽函数
// ============================================================

void ModuleSystemVisualizerPanel::onRunSimulation() {
    int idx = presetCombo_->currentIndex();
    if (idx < 0) {
        simBrowser_->setHtml(mlTr("<p style='color:red;'>请先选择一个预设场景</p>"));
        return;
    }
    ModuleSimResult result = runScenario(idx);
    renderScenarioResult(result);
}

void ModuleSystemVisualizerPanel::onLoadSample() {
    // 加载一个展示模块系统 import/export 的 MiniLang 样例到主编辑器
    QString sample = QStringLiteral("// 模块系统（import / export）样例\n"
                                    "// 演示跨文件代码组织、前向引用、模块缓存\n"
                                    "// 实际使用时需将下列各模块内容拆分到对应 .ml 文件\n"
                                    "\n"
                                    "// ---- math_utils.ml ----\n"
                                    "export fun square(x) {\n"
                                    "  return x * x;\n"
                                    "}\n"
                                    "export fun cube(x) {\n"
                                    "  return x * x * x;\n"
                                    "}\n"
                                    "\n"
                                    "// ---- format.ml ----\n"
                                    "import math_utils;\n"
                                    "\n"
                                    "// 前向引用：preScan 收集 FunDecl 使先调用后定义也能工作\n"
                                    "export fun report(n) {\n"
                                    "  return \"square=\" + str(math_utils.square(n)) +\n"
                                    "         \", cube=\" + str(cube_local(n));\n"
                                    "}\n"
                                    "\n"
                                    "fun cube_local(n) {  // 未 export，仅模块内可见\n"
                                    "  return math_utils.cube(n);\n"
                                    "}\n"
                                    "\n"
                                    "// ---- main.ml ----\n"
                                    "import format;\n"
                                    "\n"
                                    "print(format.report(3));   // square=9, cube=27\n"
                                    "\n"
                                    "// 模块缓存：重复 import 同一模块仅执行一次\n"
                                    "// （此处 import 已在上方完成，缓存命中）\n"
                                    "// import format;  // 跳过(已缓存)\n"
                                    "\n"
                                    "// 注意：以下写法会触发模块系统错误\n"
                                    "// import nonexist;        // 路径解析失败\n"
                                    "// 循环导入：a imports b imports a → 延迟加载返回部分 env（P2-14）\n"
                                    "\n"
                                    "print(\"模块系统支持前向引用、模块缓存与循环导入延迟加载\");");
    emit loadSampleRequested(sample);
}

// ============================================================
// createGuidedTour — 新手引导（4 步）
// ============================================================

/// 构建 4 步新手引导：模块系统原理、路径解析规则、交互式模拟器、4 个预设场景。
GuidedTour* ModuleSystemVisualizerPanel::createGuidedTour(QWidget* host) {
    auto* tour = new GuidedTour(host, host);
    tour->addStep(pageTheoryBtn_, mlTr("模块系统原理"),
                  mlTr("点击「模块原理」查看 import/export 语义、模块缓存、预扫描阶段"
                       "（FunDecl/ExportStmt）与错误传播链——这些是模块系统的高频 Bug 模式。"));
    tour->addStep(pathTable_, mlTr("路径解析规则"),
                  mlTr("路径解析规则表展示 import 字符串如何映射到磁盘模块文件，"
                       "以及解析失败时 ModuleError 如何沿调用栈传播。"));
    tour->addStep(pageSimulatorBtn_, mlTr("交互式模拟器"),
                  mlTr("点击「依赖模拟器」切到子页 2：纯 C++ 模拟模块加载栈与缓存，"
                       "可视化展示加载顺序、循环导入延迟加载与预扫描遗漏。"));
    tour->addStep(presetCombo_, mlTr("4 个预设场景"),
                  mlTr("用此下拉框依次运行 4 个预设场景：正常导入链 / 循环导入延迟加载 / "
                       "路径解析失败 / 预扫描遗漏，对照时间轴与依赖图理解每种情况。"));
    return tour;
}

void ModuleSystemVisualizerPanel::onSwitchPage(int idx) {
    if (idx < 0 || idx >= stack_->count()) {
        return;
    }
    // 子页切换与样式由 TeachingSubPageBar 统一管理，这里仅做面板特定逻辑
    // （simulator 子页进入时无需额外处理，保持空实现占位）
}
