// ============================================================
// ExceptionFlowPanel.cpp — 异常流可视化面板实现（第三档 P2-3a）
// ============================================================

#include "gui/ExceptionFlowPanel.h"
#include "gui/MarkdownRenderer.h"
#include "gui/PanelAnimator.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QSplitter>
#include <QVBoxLayout>
#include <sstream>

// ============================================================
// ExceptionFlowLibrary — 静态教学场景库
// ============================================================
// 8 个典型异常流场景，覆盖：
//   - 简单 try/catch
//   - 未捕获异常（传播到顶层）
//   - 嵌套 try/catch
//   - finally 语义
//   - 跨函数异常传播
//   - 递归中的异常
//   - 异常对象字段访问
//   - 重新抛出
//
// 帮助学习者理解异常传播机制：
//   - throw 创建异常对象，沿调用栈向上搜索处理器
//   - catch 匹配基于类型（MiniLang 中为字符串标签）
//   - finally 块无论是否捕获都会执行
//   - 未捕获异常导致程序终止

/// 返回异常场景示例数据（静态数据）。
const std::vector<ExceptionScenario>& ExceptionFlowLibrary::scenarios() {
    static const std::vector<ExceptionScenario> kScenarios = {
        {"simple-try-catch",
         "🛡️ 简单 try/catch（单层捕获）",
         "🛡️ 最基础的异常处理形式。try 块中 throw 抛出异常对象，"
         "catch 块捕获并处理。异常对象通过 catch 变量名（如 e）访问。"
         "MiniLang 的异常对象是字符串值，catch 匹配不区分类型。",
         "try {\n    throw \"something went wrong\";\n} catch (e) {\n    print(\"caught: \" + e);\n}",
         {"main()", "try-block", "throw", "catch-block", "main()"},
         "💡 异常从 try 块抛出后，立即跳转到对应 catch 块。"
         "catch 变量 e 绑定异常对象（字符串值），执行完 catch 块后继续正常流程。"
         "try/catch 之后的代码会正常执行（除非再次抛出异常）。"},
        {"uncaught-exception",
         "🚨 未捕获异常（传播到顶层）",
         "🚨 异常沿调用栈向上传播，若没有找到任何匹配的 catch 块，"
         "最终传播到顶层导致程序终止。MiniLang 中未捕获异常会打印"
         "错误信息并设置 VM/Interpreter 的错误标志。",
         "fun risky() {\n    throw \"fatal error\";\n}\n\nrisky();\nprint(\"this line is never reached\");",
         {"main()", "risky()", "throw", "risky() (unwinding)", "main() (unwinding)", "PROGRAM TERMINATED"},
         "⚠️ 异常从 risky() 抛出，沿调用栈向上搜索。risky() 没有 try/catch，"
         "帧被弹出（栈展开）。main() 也没有 try/catch，继续弹出。"
         "最终传播到顶层，程序终止，print 语句不会执行。"},
        {"nested-try-catch",
         "🛡️ 嵌套 try/catch（内层捕获）",
         "🛡️ 嵌套的 try/catch 结构。内层 catch 优先匹配异常。"
         "若内层不匹配（或再次抛出），异常继续传播到外层 catch。"
         "MiniLang 的 catch 不区分异常类型，因此内层 catch 总是捕获异常。",
         "try {\n    try {\n        throw \"inner error\";\n    } catch (e) {\n        print(\"inner caught: \" + e);\n"
         "    }\n} catch (e2) {\n    print(\"outer caught: \" + e2);\n}",
         {"main()", "outer-try", "inner-try", "throw", "inner-catch", "inner-try-end", "outer-try-end", "main()"},
         "💡 异常从内层 try 块抛出，首先匹配内层 catch。内层 catch 捕获后执行，"
         "内层 try/catch 结束。控制流回到外层 try 块继续执行。"
         "外层 catch 不会被触发（除非内层 catch 中再次 throw）。"},
        {"finally-semantics",
         "✨ finally 语义（始终执行）",
         "✨ finally 块无论是否发生异常都会执行。常用于资源清理"
         "（如关闭文件、释放锁）。MiniLang 支持 try/catch/finally 三段式，"
         "finally 块在 catch 处理后或无异常时都会执行。",
         "var resource = \"opened\";\ntry {\n    throw \"error during processing\";\n} catch (e) {\n    print(\"error: "
         "\" + e);\n} finally {\n    print(\"cleanup: closing \" + resource);\n}",
         {"main()", "try-block", "throw", "catch-block", "finally-block", "main()"},
         "💡 MiniLang 的 finally 块在以下三种情况都会执行："
         "(1) try 块正常结束；(2) try 块抛出异常被 catch 捕获后；"
         "(3) try 块抛出异常但无匹配 catch（finally 仍执行，然后异常继续传播）。"
         "资源清理代码放在 finally 中可确保一定执行。"},
        {"cross-function-propagation",
         "🌊 跨函数异常传播",
         "🌊 异常从被调用函数抛出，沿调用栈跨帧传播到调用者的 catch 块。"
         "每个帧在栈展开时被弹出，局部变量被销毁。"
         "这是异常处理最常见的实际使用模式。",
         "fun validate(x) {\n    if (x < 0) {\n        throw \"negative value not allowed\";\n    }\n    return x * "
         "2;\n}\n\ntry {\n    var result = validate(-5);\n    print(result);\n} catch (e) {\n    print(\"validation "
         "failed: \" + e);\n}",
         {"main()", "validate(-5)", "throw", "validate() (unwinding)", "main() catch-block", "main()"},
         "💡 异常从 validate() 抛出，validate() 没有 try/catch，帧被弹出（栈展开）。"
         "控制流回到 main() 的 try 块，异常被 catch 块捕获。"
         "validate() 的局部变量（如 x）在栈展开时被销毁。"},
        {"recursive-exception",
         "🔄 递归中的异常传播",
         "🔄 递归调用中抛出异常，异常沿递归链向上传播。"
         "每一层递归帧都会被检查是否有 catch 块。"
         "若所有层都没有 catch，异常传播到顶层。",
         "fun countdown(n) {\n    if (n == 0) {\n        throw \"reached zero\";\n    }\n    countdown(n - "
         "1);\n}\n\ntry {\n    countdown(3);\n} catch (e) {\n    print(\"caught: \" + e);\n}",
         {"main()", "countdown(3)", "countdown(2)", "countdown(1)", "countdown(0)", "throw", "countdown(0) (unwinding)",
          "countdown(1) (unwinding)", "countdown(2) (unwinding)", "countdown(3) (unwinding)", "main() catch-block",
          "main()"},
         "💡 异常从最深层 countdown(0) 抛出，沿递归链向上传播。"
         "每一层 countdown() 都没有 try/catch，帧依次被弹出。"
         "最终传播到 main() 的 catch 块被捕获。"
         "这展示了异常传播的栈展开机制——每帧的局部变量都被销毁。"},
        {"exception-object-field",
         "📦 异常对象字段访问",
         "📦 异常对象可以是任意值（字符串、数字、字典、实例）。"
         "catch 变量绑定异常对象后，可通过字段访问或方法调用获取详情。"
         "常用字典或实例作为异常对象，携带结构化错误信息。",
         "try {\n    var error = {\"code\": 404, \"message\": \"not found\"};\n    throw error;\n} catch (e) {\n    "
         "print(\"error code: \" + e.code);\n    print(\"error message: \" + e.message);\n}",
         {"main()", "try-block", "throw (dict)", "catch-block", "main()"},
         "💡 异常对象是字典值，包含 code 和 message 字段。"
         "catch 变量 e 绑定字典后，可通过 e.code / e.message 访问字段。"
         "这展示了异常对象可以是结构化数据，而非仅字符串。"},
        {"rethrow",
         "↩️ 重新抛出（catch 中再次 throw）",
         "↩️ catch 块中可以再次 throw，将异常（或新异常）传播到外层。"
         "常用于：内层 catch 记录日志后重新抛出，或转换异常类型。"
         "重新抛出后，当前 catch 块剩余代码不执行，异常沿调用栈继续传播。",
         "try {\n    try {\n        throw \"original error\";\n    } catch (e) {\n        print(\"logging: \" + e);\n"
         "        throw \"rethrown: \" + e;\n    }\n} catch (e2) {\n    print(\"outer caught: \" + e2);\n}",
         {"main()", "outer-try", "inner-try", "throw", "inner-catch (logging)", "throw (rethrow)",
          "inner-catch (unwinding)", "outer-catch", "main()"},
         "💡 内层 catch 捕获异常后打印日志，然后重新 throw。"
         "重新 throw 后，内层 catch 块剩余代码不执行。"
         "异常沿调用栈传播到外层 catch 块被捕获。"
         "这展示了异常的转换与传播链。"},
    };
    return kScenarios;
}

// ============================================================
// ExceptionPhaseLibrary — 静态传播阶段图解库
// ============================================================
// 6 个异常传播关键阶段，覆盖：
//   - throw 创建异常对象
//   - 栈搜索（沿调用栈向上搜索处理器）
//   - catch 匹配（绑定 catch 变量）
//   - finally 清理（资源释放）
//   - 栈展开（局部变量销毁）
//   - 恢复（catch 后继续正常流程）

/// 返回异常传播各阶段说明数据（静态数据）。
const std::vector<ExceptionPhaseDoc>& ExceptionPhaseLibrary::phases() {
    static const std::vector<ExceptionPhaseDoc> kPhases = {
        {"throw", "throw",
         "⚠️ throw 语句创建异常对象（可以是任意值：字符串、数字、字典、实例），"
         "并立即中断当前控制流。异常对象被保存，用于后续 catch 变量绑定。"
         "MiniLang 中 throw 是语句而非表达式，不返回值。",
         "栈：弹出当前操作数，标记异常状态"},
        {"search", "search",
         "🔍 异常传播器沿调用栈向上搜索匹配的 catch 块。"
         "每一帧检查是否存在 try/catch 结构，若存在则跳转到 catch 块。"
         "MiniLang 中 catch 不区分异常类型，因此第一个 catch 块总是匹配。",
         "栈：逐帧搜索，未匹配的帧被标记为待展开"},
        {"catch", "catch",
         "🛡️ catch 块捕获异常，catch 变量绑定异常对象。"
         "catch 块执行完毕后，控制流回到 try/catch 之后的代码。"
         "catch 块中可以再次 throw（重新抛出），传播到外层。",
         "栈：异常对象弹出，绑定到 catch 变量，恢复正常执行"},
        {"finally", "finally",
         "✨ finally 块无论是否发生异常都会执行。MiniLang 支持 try/catch/finally "
         "三段式语法，资源清理（如关闭文件、释放锁）应放在 finally 中确保执行。"
         "finally 在 catch 之后、栈完全展开之前执行。",
         "栈：finally 块执行完毕后，栈状态恢复到 try 之前或继续传播异常"},
        {"unwind", "unwind",
         "🌊 栈展开是异常传播的核心机制。未匹配的帧被弹出，"
         "局部变量被销毁（调用析构函数）。栈展开从 throw 点开始，"
         "逐帧向上直到找到 catch 块或传播到顶层。"
         "栈展开期间不可中断（除非再次抛出异常）。",
         "栈：逐帧弹出，局部变量销毁，帧计数器递减"},
        {"recovery", "recovery",
         "✅ catch 块执行完毕后，程序恢复正常控制流。"
         "try/catch 之后的代码继续执行。"
         "若异常未被捕获，传播到顶层导致程序终止，设置错误标志。",
         "栈：恢复正常执行状态，清除异常标志"},
    };
    return kPhases;
}

// ============================================================
// ExceptionFlowPanel 实现
// ============================================================

/// 构造异常流转面板：初始化场景页与阶段页。
ExceptionFlowPanel::ExceptionFlowPanel(QWidget* parent) : QWidget(parent) {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // 顶部页面切换按钮
    auto* topBar = new QHBoxLayout();
    topBar->setContentsMargins(4, 4, 4, 4);
    topBar->setSpacing(8);
    pageScenarioBtn_ = new QPushButton("教学场景库", this);
    pagePhaseBtn_ = new QPushButton("传播图解", this);
    pageScenarioBtn_->setCheckable(true);
    pagePhaseBtn_->setCheckable(true);
    pageScenarioBtn_->setChecked(true);
    topBar->addWidget(pageScenarioBtn_);
    topBar->addWidget(pagePhaseBtn_);
    topBar->addStretch();
    mainLayout->addLayout(topBar);

    stack_ = new QStackedWidget(this);
    mainLayout->addWidget(stack_);

    // 子页 1：教学场景库
    auto* scenarioPage = new QWidget(stack_);
    buildScenarioPage(scenarioPage);
    stack_->addWidget(scenarioPage);

    // 子页 2：传播图解
    auto* phasePage = new QWidget(stack_);
    buildPhasePage(phasePage);
    stack_->addWidget(phasePage);

    // 顶部按钮切换
    connect(pageScenarioBtn_, &QPushButton::clicked, this, [this]() {
        stack_->setCurrentWidget(stack_->widget(0));
        pageScenarioBtn_->setChecked(true);
        pagePhaseBtn_->setChecked(false);
        PanelAnimator::slideInWidget(stack_->currentWidget());
    });
    connect(pagePhaseBtn_, &QPushButton::clicked, this, [this]() {
        stack_->setCurrentWidget(stack_->widget(1));
        pageScenarioBtn_->setChecked(false);
        pagePhaseBtn_->setChecked(true);
        PanelAnimator::slideInWidget(stack_->currentWidget());
    });

    // 默认选中第一个
    if (!ExceptionFlowLibrary::scenarios().empty()) {
        scenarioList_->setCurrentRow(0);
    }
    if (!ExceptionPhaseLibrary::phases().empty()) {
        phaseList_->setCurrentRow(0);
    }
}

/// 构建「异常场景」子页 UI。
void ExceptionFlowPanel::buildScenarioPage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* splitter = new QSplitter(Qt::Horizontal, host);
    scenarioList_ = new QListWidget(splitter);
    scenarioDetail_ = new QTextBrowser(splitter);
    scenarioDetail_->setOpenExternalLinks(false);

    for (const auto& s : ExceptionFlowLibrary::scenarios()) {
        scenarioList_->addItem(QString::fromStdString(s.title));
    }

    splitter->addWidget(scenarioList_);
    splitter->addWidget(scenarioDetail_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);
    splitter->setSizes({200, 600});

    layout->addWidget(splitter);

    connect(scenarioList_, &QListWidget::currentRowChanged, this, [this](int row) { populateScenarioDetail(row); });
}

/// 构建「异常阶段」子页 UI。
/// ROUND56 fix (issue 4): 增大列表宽度 + 添加阶段图标 + 彩色背景条，提升可读性。
void ExceptionFlowPanel::buildPhasePage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* splitter = new QSplitter(Qt::Horizontal, host);
    phaseList_ = new QListWidget(splitter);
    phaseDetail_ = new QTextBrowser(splitter);
    phaseDetail_->setOpenExternalLinks(false);

    // ROUND56 fix: 列表项添加图标 + 彩色标签，提升视觉引导
    static const char* kPhaseIcons[] = {"⚡ throw", "🔍 search", "🛡️ catch", "✨ finally", "🌊 unwind", "✅ recovery"};
    static const char* kPhaseDesc[] = {
        "异常创建与抛出",
        "沿栈搜索处理器",
        "捕获并绑定变量",
        "资源清理始终执行",
        "栈帧弹出与销毁",
        "恢复正常控制流",
    };
    const auto& phases = ExceptionPhaseLibrary::phases();
    for (int i = 0; i < (int)phases.size(); ++i) {
        auto* item = new QListWidgetItem(QString::fromUtf8(kPhaseIcons[i]) + "\n" +
                                         QString::fromUtf8(kPhaseDesc[i]));
        item->setSizeHint(QSize(160, 48));
        phaseList_->addItem(item);
    }

    splitter->addWidget(phaseList_);
    splitter->addWidget(phaseDetail_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 4);
    splitter->setSizes({220, 780});

    layout->addWidget(splitter);

    connect(phaseList_, &QListWidget::currentRowChanged, this, [this](int row) { populatePhaseDetail(row); });
}

/// 填充指定异常场景的详细说明与示例代码。
/// ROUND56 fix (issue 4): 传播路径从纯 <ol> 列表升级为彩色流程图，
/// 每个步骤根据内容自动着色（throw 红 / catch 绿 / unwind 橙 / 终止 紫 / 正常 蓝）。
void ExceptionFlowPanel::populateScenarioDetail(int index) {
    const auto& scenarios = ExceptionFlowLibrary::scenarios();
    if (index < 0 || index >= static_cast<int>(scenarios.size())) {
        scenarioDetail_->clear();
        return;
    }
    const auto& s = scenarios[index];

    std::ostringstream oss;
    oss << "<h2>" << s.title << "</h2>";
    oss << "<p><b>ID:</b> " << s.id << "</p>";
    oss << "<p><b>说明:</b></p>";
    oss << MarkdownRenderer::markdownToHtmlFragment(s.description).toStdString();

    // 示例代码（HTML 转义）
    oss << "<h3>💻 示例代码</h3>";
    oss << "<pre style='background:#F8F9FA;border-left:4px solid #3498DB;padding:8px 12px;"
           "font-size:12px;white-space:pre-wrap;'>";
    {
        std::string code = s.sampleCode;
        for (size_t i = 0; (i = code.find('<', i)) != std::string::npos;)
            code.replace(i, 1, "&lt;");
        for (size_t i = 0; (i = code.find('>', i)) != std::string::npos;)
            code.replace(i, 1, "&gt;");
        oss << code;
    }
    oss << "</pre>";

    // 传播路径：彩色流程图（替代原 <ol> 纯文字列表）
    oss << "<h3>🌊 传播路径</h3>";
    oss << "<table cellspacing='0' cellpadding='0' style='width:100%;margin:8px 0;'>";
    for (size_t i = 0; i < s.propagationPath.size(); ++i) {
        const std::string& step = s.propagationPath[i];
        // 根据步骤内容自动着色
        const char* bg = "#ECF0F1";
        const char* border = "#BDC3C7";
        const char* icon = "▶";
        if (step.find("throw") != std::string::npos || step.find("异常创建") != std::string::npos) {
            bg = "#FADBD8"; border = "#E74C3C"; icon = "⚡";
        } else if (step.find("catch") != std::string::npos) {
            bg = "#D5F5E3"; border = "#27AE60"; icon = "🛡️";
        } else if (step.find("unwinding") != std::string::npos || step.find("展开") != std::string::npos) {
            bg = "#FDEBD0"; border = "#F39C12"; icon = "🌊";
        } else if (step.find("TERMINATED") != std::string::npos || step.find("终止") != std::string::npos) {
            bg = "#E8DAEF"; border = "#9B59B6"; icon = "🚨";
        } else if (step.find("finally") != std::string::npos) {
            bg = "#D6EAF8"; border = "#3498DB"; icon = "✨";
        }
        // 步骤号
        oss << "<tr>";
        oss << "<td style='background:" << bg << ";border:1px solid " << border
            << ";border-right:none;padding:6px 8px;width:30px;text-align:center;color:" << border
            << ";font-weight:bold;font-size:14px;'>" << (i + 1) << "</td>";
        oss << "<td style='background:" << bg << ";border:1px solid " << border
            << ";border-left:none;padding:6px 10px;color:#2C3E50;font-family:monospace;font-size:12px;'>"
            << icon << " " << step << "</td>";
        oss << "</tr>";
        if (i < s.propagationPath.size() - 1) {
            oss << "<tr><td colspan='2' style='text-align:center;color:#6E6E6E;padding:1px 0;font-size:12px;'>↓</td></tr>";
        }
    }
    oss << "</table>";

    oss << "<h3>📖 教学注释</h3>";
    oss << MarkdownRenderer::markdownToHtmlFragment(s.teachingNote).toStdString();

    oss << "<hr><p><a href=\"#load\" style='background:#27AE60;color:white;padding:6px 16px;border-radius:4px;"
           "text-decoration:none;font-weight:bold;'>📥 载入到编辑器</a></p>";

    scenarioDetail_->setHtml(QString::fromStdString(oss.str()));

    // 连接载入信号
    disconnect(scenarioDetail_, nullptr, this, nullptr);
    connect(scenarioDetail_, &QTextBrowser::anchorClicked, this,
            [this, s](const QUrl&) { emit loadSampleRequested(QString::fromStdString(s.sampleCode)); });
    // 注：移除 fadeInWidget —— opacity 卡 0 导致切换后详情区空白
}

/// 填充指定异常传播阶段的说明与图示。
/// ROUND56 fix (issue 4): 原实现仅显示纯文字（phase/category/description/stackEffect），
/// 用户反馈"传播图解太简陋了，根本没有达到效果"。
/// 新实现生成富 HTML 可视化：
///   1. 顶部流程条：6 个阶段彩色节点 + 箭头，当前阶段高亮
///   2. 调用栈状态图：用 HTML 表格模拟栈帧，标注异常对象位置与帧状态
///   3. 示例代码片段：MiniLang 代码高亮显示当前阶段对应的代码行
///   4. 关键操作列表：阶段内执行的关键步骤
///   5. 原有说明文字与栈效应保留
void ExceptionFlowPanel::populatePhaseDetail(int index) {
    const auto& phases = ExceptionPhaseLibrary::phases();
    if (index < 0 || index >= static_cast<int>(phases.size())) {
        phaseDetail_->clear();
        return;
    }
    const auto& p = phases[index];

    // ---- 阶段元数据（代码片段 + 关键操作 + 栈帧状态）----
    // 每个阶段的栈帧可视化数据：frameLabel + state（active/unwinding/caught/cleanup/destroyed/normal）
    struct StackFrame {
        const char* label;
        const char* annotation; // 右侧标注（如"← 异常创建于此"）
        const char* state;      // active / unwinding / caught / cleanup / destroyed / normal / throw-point
    };
    struct PhaseMeta {
        const char* codeSnippet;
        const char* codeHighlight; // 高亮行关键词
        std::vector<StackFrame> frames;
        std::vector<std::string> keyOps;
    };
    // 示例代码统一使用跨函数异常传播场景，6 个阶段展示同一场景的不同时刻
    static const char* kCommonCode =
        "fun validate(x) {\n"
        "    if (x < 0) {\n"
        "        throw \"negative value\";\n"
        "    }\n"
        "    return x * 2;\n"
        "}\n"
        "try {\n"
        "    var result = validate(-5);\n"
        "    print(result);\n"
        "} catch (e) {\n"
        "    print(\"caught: \" + e);\n"
        "} finally {\n"
        "    print(\"cleanup\");\n"
        "}";

    static const std::vector<PhaseMeta> kPhaseMeta = {
        {"throw", "\"negative value\"",
         {{"main()", "try { validate(-5); }", "active"},
          {"validate(-5)", "throw \"negative value\" ← 异常创建", "throw-point"}},
         {"创建异常对象（字符串 \"negative value\"）",
          "立即中断 validate() 函数的当前控制流",
          "异常对象保存到 VM 异常槽位",
          "return 语句不会执行（已被 throw 抢占）"}},
        {"search", "validate(-5)",
         {{"main()", "try { ... } ← 搜索 catch", "searching"},
          {"validate(-5)", "无 try/catch，帧待展开", "unwinding"},
          {"⚡ 异常对象", "沿调用栈向上搜索 ↑", "exception"}},
         {"异常传播器沿调用栈向上搜索匹配的 catch 块",
          "检查 validate() 帧：无 try/catch → 标记为待展开",
          "检查 main() 帧：发现 try/catch → 准备跳转到 catch",
          "MiniLang 中 catch 不区分类型，第一个 catch 总是匹配"}},
        {"catch", "catch (e)",
         {{"main()", "catch (e) { ... } ← 捕获成功", "caught"},
          {"validate(-5)", "已弹出（栈展开完成）", "destroyed"},
          {"⚡ 异常对象 → e", "绑定到 catch 变量", "bound"}},
         {"catch 块捕获异常，控制流跳转到 catch 块",
          "catch 变量 e 绑定异常对象（字符串 \"negative value\"）",
          "validate() 帧已完全展开，局部变量 x 已销毁",
          "执行 catch 块：print(\"caught: \" + e)"}},
        {"finally", "finally {",
         {{"main()", "finally { print(\"cleanup\"); }", "cleanup"},
          {"catch 块", "已执行完毕", "done"},
          {"⚡ 异常已处理", "finally 无论是否异常都执行", "resolved"}},
         {"finally 块在 catch 之后执行（无论是否发生异常）",
          "资源清理代码放在 finally 中确保执行",
          "执行：print(\"cleanup\")",
          "若 catch 中再次 throw，finally 执行后异常继续传播"}},
        {"unwind", "validate(-5)",
         {{"main()", "等待异常到达", "active"},
          {"validate(-5) ✗", "帧弹出 → 局部变量销毁", "destroyed"},
          {"⚡ 异常", "逐帧向上传播", "exception"}},
         {"栈展开从 throw 点开始，逐帧向上",
          "validate() 帧被弹出：局部变量 x 被销毁",
          "帧弹出顺序：throw 帧 → 调用帧 → ... → catch 帧",
          "栈展开期间不可中断（除非再次抛出异常）"}},
        {"recovery", "print(result);",
         {{"main()", "try/catch 之后的代码 ← 正常继续", "normal"},
          {"catch 块", "已执行完毕", "done"},
          {"✅ 异常已恢复", "程序正常控制流", "resolved"}},
         {"catch 块执行完毕，程序恢复正常控制流",
          "try/catch/finally 之后的代码继续执行",
          "异常标志已清除，VM 恢复正常运行状态",
          "若异常未被捕获，传播到顶层导致程序终止"}},
    };

    // ---- 生成流程条 HTML ----
    // 6 个阶段彩色节点 + 箭头，当前阶段高亮（加粗 + 边框 + 放大）
    static const char* kPhaseColors[] = {
        "#E74C3C", // throw - 红
        "#F39C12", // search - 橙
        "#27AE60", // catch - 绿
        "#3498DB", // finally - 蓝
        "#9B59B6", // unwind - 紫
        "#1ABC9C", // recovery - 青
    };
    static const char* kPhaseIcons[] = {"⚡", "🔍", "🛡️", "✨", "🌊", "✅"};

    std::ostringstream oss;
    oss << "<div style='margin-bottom:8px;'>";

    // 流程条表格
    oss << "<table cellspacing='0' cellpadding='0' style='width:100%;margin-bottom:12px;'><tr>";
    for (int i = 0; i < 6; ++i) {
        bool isCurrent = (i == index);
        bool isPast = (i < index);
        const char* opacity = isCurrent ? "1.0" : (isPast ? "0.5" : "0.3");
        const char* border = isCurrent ? "3px solid #2C3E50" : "1px solid #BDC3C7";
        const char* fontWeight = isCurrent ? "bold" : "normal";
        const char* padding = isCurrent ? "10px 6px" : "6px 4px";
        const char* fontSize = isCurrent ? "13px" : "11px";
        oss << "<td style='background:" << kPhaseColors[i] << ";color:white;padding:" << padding
            << ";text-align:center;border:" << border << ";border-radius:4px;opacity:" << opacity
            << ";font-weight:" << fontWeight << ";font-size:" << fontSize << ";'>"
            << kPhaseIcons[i] << "<br>" << phases[i].phase << "</td>";
        if (i < 5) {
            oss << "<td style='color:#6E6E6E;padding:0 2px;text-align:center;font-size:14px;'>→</td>";
        }
    }
    oss << "</tr></table>";

    // 阶段标题
    oss << "<h2 style='color:" << kPhaseColors[index] << ";'>" << kPhaseIcons[index] << " "
        << p.phase << " 阶段</h2>";
    oss << "<p><b>分类:</b> <span style='background:" << kPhaseColors[index]
        << ";color:white;padding:2px 8px;border-radius:3px;font-size:11px;'>" << p.category
        << "</span></p>";

    // ---- 调用栈状态图 ----
    oss << "<h3 style='border-bottom:2px solid " << kPhaseColors[index] << ";padding-bottom:4px;'>"
        << "📊 调用栈状态</h3>";
    const auto& meta = kPhaseMeta[index];
    oss << "<table cellspacing='0' cellpadding='0' style='width:100%;margin:8px 0;border:1px solid #DDD;'>";
    // 栈从顶到底（高地址 → 低地址）
    for (auto it = meta.frames.rbegin(); it != meta.frames.rend(); ++it) {
        const char* bg = "#ECF0F1";
        const char* border = "#BDC3C7";
        const char* textColor = "#2C3E50";
        if (std::string(it->state) == "throw-point") {
            bg = "#FADBD8"; border = "#E74C3C";
        } else if (std::string(it->state) == "unwinding") {
            bg = "#FDEBD0"; border = "#F39C12";
        } else if (std::string(it->state) == "caught") {
            bg = "#D5F5E3"; border = "#27AE60";
        } else if (std::string(it->state) == "cleanup") {
            bg = "#D6EAF8"; border = "#3498DB";
        } else if (std::string(it->state) == "destroyed") {
            bg = "#F2F3F4"; border = "#BDC3C7"; textColor = "#95A5A6";
        } else if (std::string(it->state) == "exception") {
            bg = "#FDF2E9"; border = "#E67E22"; textColor = "#D35400";
        } else if (std::string(it->state) == "searching") {
            bg = "#FEF9E7"; border = "#F1C40F";
        } else if (std::string(it->state) == "normal" || std::string(it->state) == "resolved") {
            bg = "#E8F8F5"; border = "#1ABC9C";
        }
        oss << "<tr>";
        // 栈帧标签
        oss << "<td style='background:" << bg << ";border:1px solid " << border
            << ";padding:6px 8px;width:40%;color:" << textColor << ";font-family:monospace;font-weight:bold;'>"
            << it->label << "</td>";
        // 栈帧标注
        oss << "<td style='background:" << bg << ";border:1px solid " << border
            << ";border-left:none;padding:6px 8px;color:" << textColor << ";font-size:12px;'>"
            << it->annotation << "</td>";
        oss << "</tr>";
    }
    oss << "</table>";
    oss << "<p style='color:#6E6E6E;font-size:11px;margin:2px 0 8px 0;'>"
           "↑ 栈顶（高地址） | ↓ 栈底（低地址）</p>";

    // ---- 关键操作列表 ----
    oss << "<h3 style='border-bottom:2px solid " << kPhaseColors[index] << ";padding-bottom:4px;'>"
        << "🔑 关键操作</h3>";
    oss << "<ul style='margin:4px 0 8px 0;'>";
    for (const auto& op : meta.keyOps) {
        oss << "<li style='margin:2px 0;font-size:12px;'>" << op << "</li>";
    }
    oss << "</ul>";

    // ---- 示例代码 ----
    oss << "<h3 style='border-bottom:2px solid " << kPhaseColors[index] << ";padding-bottom:4px;'>"
        << "💻 示例代码（当前阶段: " << p.phase << "）</h3>";
    oss << "<pre style='background:#F8F9FA;border-left:4px solid " << kPhaseColors[index]
        << ";padding:8px 12px;font-size:12px;overflow-x:auto;white-space:pre-wrap;'>";
    // 高亮当前阶段对应代码行
    {
        std::string code = kCommonCode;
        std::string highlight = meta.codeHighlight;
        if (!highlight.empty()) {
            size_t pos = code.find(highlight);
            if (pos != std::string::npos) {
                std::string before = code.substr(0, pos);
                std::string matched = code.substr(pos, highlight.size());
                std::string after = code.substr(pos + highlight.size());
                // HTML 转义
                auto escapeHtml = [](std::string& s) {
                    for (size_t i = 0; (i = s.find('<', i)) != std::string::npos;)
                        s.replace(i, 1, "&lt;");
                    for (size_t i = 0; (i = s.find('>', i)) != std::string::npos;)
                        s.replace(i, 1, "&gt;");
                };
                escapeHtml(before);
                escapeHtml(matched);
                escapeHtml(after);
                oss << before << "<span style='background:" << kPhaseColors[index]
                    << ";color:white;padding:1px 2px;border-radius:2px;font-weight:bold;'>" << matched
                    << "</span>" << after;
            } else {
                auto escapeHtml = [](std::string& s) {
                    for (size_t i = 0; (i = s.find('<', i)) != std::string::npos;)
                        s.replace(i, 1, "&lt;");
                    for (size_t i = 0; (i = s.find('>', i)) != std::string::npos;)
                        s.replace(i, 1, "&gt;");
                };
                escapeHtml(code);
                oss << code;
            }
        } else {
            auto escapeHtml = [](std::string& s) {
                for (size_t i = 0; (i = s.find('<', i)) != std::string::npos;)
                    s.replace(i, 1, "&lt;");
                for (size_t i = 0; (i = s.find('>', i)) != std::string::npos;)
                    s.replace(i, 1, "&gt;");
            };
            escapeHtml(code);
            oss << code;
        }
    }
    oss << "</pre>";

    // ---- 原有说明文字 ----
    oss << "<h3 style='border-bottom:2px solid " << kPhaseColors[index] << ";padding-bottom:4px;'>"
        << "📖 详细说明</h3>";
    oss << MarkdownRenderer::markdownToHtmlFragment(p.description).toStdString();
    oss << "<p style='background:#F8F9FA;padding:6px 10px;border-radius:4px;margin:8px 0;'><b>⚡ 栈效应:</b> "
        << p.stackEffect << "</p>";

    oss << "</div>";

    phaseDetail_->setHtml(QString::fromStdString(oss.str()));
}
