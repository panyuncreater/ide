// ============================================================
// ExceptionFlowPanel.cpp — 异常流可视化面板实现（第三档 P2-3a）
// ============================================================

#include "gui/ExceptionFlowPanel.h"
#include "gui/PanelAnimator.h"
#include "gui/MarkdownRenderer.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QHeaderView>
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

const std::vector<ExceptionScenario>& ExceptionFlowLibrary::scenarios() {
    static const std::vector<ExceptionScenario> kScenarios = {
        {
            "simple-try-catch",
            "🛡️ 简单 try/catch（单层捕获）",
            "🛡️ 最基础的异常处理形式。try 块中 throw 抛出异常对象，"
            "catch 块捕获并处理。异常对象通过 catch 变量名（如 e）访问。"
            "MiniLang 的异常对象是字符串值，catch 匹配不区分类型。",
            "try {\n    throw \"something went wrong\";\n} catch (e) {\n    print \"caught: \" + e;\n}",
            {"main()", "try-block", "throw", "catch-block", "main()"},
            "💡 异常从 try 块抛出后，立即跳转到对应 catch 块。"
            "catch 变量 e 绑定异常对象（字符串值），执行完 catch 块后继续正常流程。"
            "try/catch 之后的代码会正常执行（除非再次抛出异常）。"
        },
        {
            "uncaught-exception",
            "🚨 未捕获异常（传播到顶层）",
            "🚨 异常沿调用栈向上传播，若没有找到任何匹配的 catch 块，"
            "最终传播到顶层导致程序终止。MiniLang 中未捕获异常会打印"
            "错误信息并设置 VM/Interpreter 的错误标志。",
            "fun risky() {\n    throw \"fatal error\";\n}\n\nrisky();\nprint \"this line is never reached\";",
            {"main()", "risky()", "throw", "risky() (unwinding)", "main() (unwinding)", "PROGRAM TERMINATED"},
            "⚠️ 异常从 risky() 抛出，沿调用栈向上搜索。risky() 没有 try/catch，"
            "帧被弹出（栈展开）。main() 也没有 try/catch，继续弹出。"
            "最终传播到顶层，程序终止，print 语句不会执行。"
        },
        {
            "nested-try-catch",
            "🛡️ 嵌套 try/catch（内层捕获）",
            "🛡️ 嵌套的 try/catch 结构。内层 catch 优先匹配异常。"
            "若内层不匹配（或再次抛出），异常继续传播到外层 catch。"
            "MiniLang 的 catch 不区分异常类型，因此内层 catch 总是捕获异常。",
            "try {\n    try {\n        throw \"inner error\";\n    } catch (e) {\n        print \"inner caught: \" + e;\n    }\n} catch (e2) {\n    print \"outer caught: \" + e2;\n}",
            {"main()", "outer-try", "inner-try", "throw", "inner-catch", "inner-try-end", "outer-try-end", "main()"},
            "💡 异常从内层 try 块抛出，首先匹配内层 catch。内层 catch 捕获后执行，"
            "内层 try/catch 结束。控制流回到外层 try 块继续执行。"
            "外层 catch 不会被触发（除非内层 catch 中再次 throw）。"
        },
        {
            "finally-semantics",
            "✨ finally 语义（始终执行）",
            "✨ finally 块无论是否发生异常都会执行。常用于资源清理"
            "（如关闭文件、释放锁）。MiniLang 的 try/catch 不支持 finally 关键字，"
            "但可通过 catch + 显式清理模拟类似语义。",
            "try {\n    var resource = \"opened\";\n    throw \"error during processing\";\n} catch (e) {\n    print \"cleanup: closing resource\";\n    print \"error: \" + e;\n}",
            {"main()", "try-block", "throw", "catch-block (cleanup)", "main()"},
            "💡 MiniLang 无 finally 关键字，但 catch 块可用于资源清理。"
            "若 try 块中抛出异常，catch 块执行清理逻辑。"
            "若无异常，catch 块不执行（需在 try 块末尾也放清理代码）。"
        },
        {
            "cross-function-propagation",
            "🌊 跨函数异常传播",
            "🌊 异常从被调用函数抛出，沿调用栈跨帧传播到调用者的 catch 块。"
            "每个帧在栈展开时被弹出，局部变量被销毁。"
            "这是异常处理最常见的实际使用模式。",
            "fun validate(x) {\n    if (x < 0) {\n        throw \"negative value not allowed\";\n    }\n    return x * 2;\n}\n\ntry {\n    var result = validate(-5);\n    print result;\n} catch (e) {\n    print \"validation failed: \" + e;\n}",
            {"main()", "validate(-5)", "throw", "validate() (unwinding)", "main() catch-block", "main()"},
            "💡 异常从 validate() 抛出，validate() 没有 try/catch，帧被弹出（栈展开）。"
            "控制流回到 main() 的 try 块，异常被 catch 块捕获。"
            "validate() 的局部变量（如 x）在栈展开时被销毁。"
        },
        {
            "recursive-exception",
            "🔄 递归中的异常传播",
            "🔄 递归调用中抛出异常，异常沿递归链向上传播。"
            "每一层递归帧都会被检查是否有 catch 块。"
            "若所有层都没有 catch，异常传播到顶层。",
            "fun countdown(n) {\n    if (n == 0) {\n        throw \"reached zero\";\n    }\n    countdown(n - 1);\n}\n\ntry {\n    countdown(3);\n} catch (e) {\n    print \"caught: \" + e;\n}",
            {"main()", "countdown(3)", "countdown(2)", "countdown(1)", "countdown(0)", "throw", "countdown(0) (unwinding)", "countdown(1) (unwinding)", "countdown(2) (unwinding)", "countdown(3) (unwinding)", "main() catch-block", "main()"},
            "💡 异常从最深层 countdown(0) 抛出，沿递归链向上传播。"
            "每一层 countdown() 都没有 try/catch，帧依次被弹出。"
            "最终传播到 main() 的 catch 块被捕获。"
            "这展示了异常传播的栈展开机制——每帧的局部变量都被销毁。"
        },
        {
            "exception-object-field",
            "📦 异常对象字段访问",
            "📦 异常对象可以是任意值（字符串、数字、字典、实例）。"
            "catch 变量绑定异常对象后，可通过字段访问或方法调用获取详情。"
            "常用字典或实例作为异常对象，携带结构化错误信息。",
            "try {\n    var error = {\"code\": 404, \"message\": \"not found\"};\n    throw error;\n} catch (e) {\n    print \"error code: \" + e.code;\n    print \"error message: \" + e.message;\n}",
            {"main()", "try-block", "throw (dict)", "catch-block", "main()"},
            "💡 异常对象是字典值，包含 code 和 message 字段。"
            "catch 变量 e 绑定字典后，可通过 e.code / e.message 访问字段。"
            "这展示了异常对象可以是结构化数据，而非仅字符串。"
        },
        {
            "rethrow",
            "↩️ 重新抛出（catch 中再次 throw）",
            "↩️ catch 块中可以再次 throw，将异常（或新异常）传播到外层。"
            "常用于：内层 catch 记录日志后重新抛出，或转换异常类型。"
            "重新抛出后，当前 catch 块剩余代码不执行，异常沿调用栈继续传播。",
            "try {\n    try {\n        throw \"original error\";\n    } catch (e) {\n        print \"logging: \" + e;\n        throw \"rethrown: \" + e;\n    }\n} catch (e2) {\n    print \"outer caught: \" + e2;\n}",
            {"main()", "outer-try", "inner-try", "throw", "inner-catch (logging)", "throw (rethrow)", "inner-catch (unwinding)", "outer-catch", "main()"},
            "💡 内层 catch 捕获异常后打印日志，然后重新 throw。"
            "重新 throw 后，内层 catch 块剩余代码不执行。"
            "异常沿调用栈传播到外层 catch 块被捕获。"
            "这展示了异常的转换与传播链。"
        },
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

const std::vector<ExceptionPhaseDoc>& ExceptionPhaseLibrary::phases() {
    static const std::vector<ExceptionPhaseDoc> kPhases = {
        {
            "throw",
            "throw",
            "⚠️ throw 语句创建异常对象（可以是任意值：字符串、数字、字典、实例），"
            "并立即中断当前控制流。异常对象被保存，用于后续 catch 变量绑定。"
            "MiniLang 中 throw 是语句而非表达式，不返回值。",
            "栈：弹出当前操作数，标记异常状态"
        },
        {
            "search",
            "search",
            "🔍 异常传播器沿调用栈向上搜索匹配的 catch 块。"
            "每一帧检查是否存在 try/catch 结构，若存在则跳转到 catch 块。"
            "MiniLang 中 catch 不区分异常类型，因此第一个 catch 块总是匹配。",
            "栈：逐帧搜索，未匹配的帧被标记为待展开"
        },
        {
            "catch",
            "catch",
            "🛡️ catch 块捕获异常，catch 变量绑定异常对象。"
            "catch 块执行完毕后，控制流回到 try/catch 之后的代码。"
            "catch 块中可以再次 throw（重新抛出），传播到外层。",
            "栈：异常对象弹出，绑定到 catch 变量，恢复正常执行"
        },
        {
            "finally",
            "finally",
            "✨ finally 块无论是否发生异常都会执行。MiniLang 不支持 finally 关键字，"
            "但可通过 catch 块中的显式清理代码模拟。"
            "资源清理（如关闭文件、释放锁）应放在 catch 块中确保执行。",
            "栈：finally/catch 块执行完毕后，栈状态恢复到 try 之前"
        },
        {
            "unwind",
            "unwind",
            "🌊 栈展开是异常传播的核心机制。未匹配的帧被弹出，"
            "局部变量被销毁（调用析构函数）。栈展开从 throw 点开始，"
            "逐帧向上直到找到 catch 块或传播到顶层。"
            "栈展开期间不可中断（除非再次抛出异常）。",
            "栈：逐帧弹出，局部变量销毁，帧计数器递减"
        },
        {
            "recovery",
            "recovery",
            "✅ catch 块执行完毕后，程序恢复正常控制流。"
            "try/catch 之后的代码继续执行。"
            "若异常未被捕获，传播到顶层导致程序终止，设置错误标志。",
            "栈：恢复正常执行状态，清除异常标志"
        },
    };
    return kPhases;
}

// ============================================================
// ExceptionFlowPanel 实现
// ============================================================

ExceptionFlowPanel::ExceptionFlowPanel(QWidget* parent) : QWidget(parent) {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // 顶部页面切换按钮
    auto* topBar = new QHBoxLayout();
    topBar->setContentsMargins(4, 4, 4, 4);
    pageScenarioBtn_ = new QPushButton("教学场景库", this);
    pagePhaseBtn_    = new QPushButton("传播图解", this);
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
        PanelAnimator::fadeInWidget(stack_->currentWidget());
    });
    connect(pagePhaseBtn_, &QPushButton::clicked, this, [this]() {
        stack_->setCurrentWidget(stack_->widget(1));
        pageScenarioBtn_->setChecked(false);
        pagePhaseBtn_->setChecked(true);
        PanelAnimator::fadeInWidget(stack_->currentWidget());
    });

    // 默认选中第一个
    if (!ExceptionFlowLibrary::scenarios().empty()) {
        scenarioList_->setCurrentRow(0);
    }
    if (!ExceptionPhaseLibrary::phases().empty()) {
        phaseList_->setCurrentRow(0);
    }
}

void ExceptionFlowPanel::buildScenarioPage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* splitter = new QSplitter(Qt::Horizontal, host);
    scenarioList_ = new QListWidget(splitter);
    scenarioDetail_ = new QTextBrowser(splitter);
    scenarioDetail_->setOpenExternalLinks(true);

    for (const auto& s : ExceptionFlowLibrary::scenarios()) {
        scenarioList_->addItem(QString::fromStdString(s.title));
    }

    splitter->addWidget(scenarioList_);
    splitter->addWidget(scenarioDetail_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);
    splitter->setSizes({200, 600});

    layout->addWidget(splitter);

    connect(scenarioList_, &QListWidget::currentRowChanged, this, [this](int row) {
        populateScenarioDetail(row);
    });
}

void ExceptionFlowPanel::buildPhasePage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* splitter = new QSplitter(Qt::Horizontal, host);
    phaseList_ = new QListWidget(splitter);
    phaseDetail_ = new QTextBrowser(splitter);
    phaseDetail_->setOpenExternalLinks(true);

    for (const auto& p : ExceptionPhaseLibrary::phases()) {
        phaseList_->addItem(QString::fromStdString(p.phase));
    }

    splitter->addWidget(phaseList_);
    splitter->addWidget(phaseDetail_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);
    splitter->setSizes({200, 600});

    layout->addWidget(splitter);

    connect(phaseList_, &QListWidget::currentRowChanged, this, [this](int row) {
        populatePhaseDetail(row);
    });
}

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

    oss << "<h3>示例代码</h3><pre>" << s.sampleCode << "</pre>";

    oss << "<h3>传播路径</h3><ol>";
    for (const auto& step : s.propagationPath) {
        oss << "<li>" << step << "</li>";
    }
    oss << "</ol>";

    oss << "<h3>教学注释</h3>";
    oss << MarkdownRenderer::markdownToHtmlFragment(s.teachingNote).toStdString();

    oss << "<hr><p><a href=\"#load\">载入到编辑器</a></p>";

    scenarioDetail_->setHtml(QString::fromStdString(oss.str()));

    // 连接载入信号
    disconnect(scenarioDetail_, nullptr, this, nullptr);
    connect(scenarioDetail_, &QTextBrowser::anchorClicked, this, [this, s](const QUrl&) {
        emit loadSampleRequested(QString::fromStdString(s.sampleCode));
    });

    PanelAnimator::fadeInWidget(scenarioDetail_);
}

void ExceptionFlowPanel::populatePhaseDetail(int index) {
    const auto& phases = ExceptionPhaseLibrary::phases();
    if (index < 0 || index >= static_cast<int>(phases.size())) {
        phaseDetail_->clear();
        return;
    }
    const auto& p = phases[index];

    std::ostringstream oss;
    oss << "<h2>" << p.phase << "</h2>";
    oss << "<p><b>分类:</b> " << p.category << "</p>";
    oss << "<p><b>说明:</b></p>";
    oss << MarkdownRenderer::markdownToHtmlFragment(p.description).toStdString();
    oss << "<p><b>栈效应:</b> " << p.stackEffect << "</p>";

    phaseDetail_->setHtml(QString::fromStdString(oss.str()));

    PanelAnimator::fadeInWidget(phaseDetail_);
}
