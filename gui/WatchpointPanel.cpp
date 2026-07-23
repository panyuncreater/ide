// ============================================================
// WatchpointPanel.cpp — 数据断点（Watchpoint）可视化面板实现（R161）
// ============================================================

#include "gui/WatchpointPanel.h"
#include "app/IdeController.h"
#include "debug/DebugTypes.h" // WatchpointInfo + WatchpointTargetKind
#include "gui/GuidedTour.h"
#include "gui/PanelAnimator.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QSplitter>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <algorithm>
#include <sstream>

#include "Label.h"                // QFluentKit（CaptionLabel）
#include "Theme.h"                // QFluentKit（onThemeModeChanged 信号）
#include "gui/GuiTextUtils.h"     // monospaceFont 工厂
#include "gui/MarkdownRenderer.h" // 散文式说明统一 Markdown 渲染
#include "gui/TeachingTheme.h"

// ============================================================
// WatchpointLibrary — 静态教学场景库
// ============================================================
// 6 个典型 watchpoint 场景，覆盖：
//   - 简单变量监视（写入即暂停）
//   - 循环计数器监视
//   - 字段监视（obj.field 写入）
//   - 条件 watchpoint（仅特定值写入时暂停）
//   - 多 watchpoint 协同
//   - 累加器监视（排查累加错误）
//
// 帮助学习者理解 watchpoint 机制：
//   - pre-execution peek：在 SET 类指令执行前检查写入目标
//   - 命中即暂停（与行断点不同，不依赖行号）
//   - 命中次数累计（即使条件不满足也计数）
//   - 支持变量监视与字段监视两种目标类型

const std::vector<WatchpointScenario>& WatchpointLibrary::scenarios() {
    static const std::vector<WatchpointScenario> kScenarios = {
        {"simple-variable", "🔴 简单变量监视（x 被修改时暂停）", "x", "",
         "🔴 最基础的 watchpoint 形式。监视变量 x，每当 x 被赋值（写入）时暂停。"
         "机制：VM 在每条 SET 类指令执行前调用 `peekWriteTarget` 解码写入目标，"
         "若目标变量名匹配则暂停。",
         "x 的每次赋值（var x = ...; x = ...;）都会触发暂停", "var x = 0;\nx = 1;\nx = 2;\nx = 3;\nprint(x);"},
        {"loop-counter", "🎯 循环计数器监视（i 写入时暂停）", "i", "",
         "🎯 监视循环计数器 i。每次迭代 `i = i + 1` 都会触发 watchpoint，"
         "便于观察循环变量的变化轨迹。watchpoint 不依赖行号，"
         "即便 i 在多个位置被写入也会统一捕获。",
         "循环每次 i = i + 1 都暂停，共触发 10 次（假设循环 10 次）",
         "var i = 0;\nwhile (i < 10) {\n    i = i + 1;\n}\nprint(i);"},
        {"field-watch", "🎯 字段监视（obj.field 写入时暂停）", "obj.field", "",
         "🎯 监视实例字段。当 `obj.field` 被赋值时暂停，"
         "通过 OP_MEMBER_SET 指令的 fieldNameIdx 操作数解码字段名。"
         "字段 watchpoint 用于追踪对象状态变化，排查字段被意外修改的 bug。",
         "obj.field 的每次赋值都触发暂停（包括构造器初始化 + 外部赋值）",
         "class Counter {\n    var count = 0;\n    fun inc() { count = count + 1; }\n}\n"
         "var obj = Counter();\nobj.inc();\nobj.inc();\nobj.inc();\nprint(obj.count);"},
        {"conditional-watchpoint", "⚙️ 条件 watchpoint（仅 i > 5 时暂停）", "i", "i > 5",
         "⚙️ 条件 watchpoint。仅当写入后的值满足条件时才暂停，"
         "避免在大循环中每次写入都暂停。条件在沙箱中求值，"
         "结果为 truthy 时暂停，falsy 时继续执行。",
         "i 写入且新值 > 5 时暂停（即 i = 6, 7, 8, 9, 10 时触发）",
         "var i = 0;\nwhile (i < 10) {\n    i = i + 1;\n}\nprint(i);"},
        {"accumulator-bug", "🛑 累加器 bug 排查（sum 写入时暂停）", "sum", "",
         "🛑 经典累加器 bug 场景。当 sum 计算结果不符合预期时，"
         "在 sum 上设置 watchpoint，观察每次写入的值是否正确。"
         "watchpoint 暂停时可查看调用栈与局部变量，定位逻辑错误。",
         "sum 的每次累加都暂停，便于排查计算过程",
         "var sum = 0;\nvar i = 1;\nwhile (i <= 5) {\n    sum = sum + i;\n    i = i + 1;\n}\nprint(sum);"},
        {"multi-watchpoint", "🔧 多 watchpoint 协同（a 和 b 同时监视）", "a", "",
         "🔧 同时监视多个变量。watchpoint 列表支持任意数量，"
         "命中任一 watchpoint 都会暂停。常用于追踪变量间的相互作用，"
         "如交换算法中 a/b 互相依赖的写入。",
         "a 和 b 的每次写入都暂停，便于观察交换过程的中间状态",
         "var a = 10;\nvar b = 20;\nvar t = a;\na = b;\nb = t;\nprint(a);\nprint(b);"},
    };
    return kScenarios;
}

// ============================================================
// WatchpointPanel 实现
// ============================================================

WatchpointPanel::WatchpointPanel(QWidget* parent) : QWidget(parent) {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(4);

    // 顶部页面切换按钮
    auto* pageBar = new QHBoxLayout;
    pageLiveBtn_ = new QPushButton(QString::fromUtf8("实时 Watchpoint"), this);
    pageLibraryBtn_ = new QPushButton(QString::fromUtf8("教学场景库"), this);
    pageLiveBtn_->setCheckable(true);
    pageLibraryBtn_->setCheckable(true);
    pageBar->addWidget(pageLiveBtn_);
    pageBar->addWidget(pageLibraryBtn_);
    pageBar->addStretch();
    mainLayout->addLayout(pageBar);

    stack_ = new QStackedWidget(this);
    mainLayout->addWidget(stack_, 1);

    auto* p1 = new QWidget(this);
    auto* p2 = new QWidget(this);
    buildLivePage(p1);
    buildLibraryPage(p2);
    stack_->addWidget(p1);
    stack_->addWidget(p2);

    pageLiveBtn_->setChecked(true);
    stack_->setCurrentIndex(0);

    connect(pageLiveBtn_, &QPushButton::clicked, this, [this]() {
        stack_->setCurrentIndex(0);
        pageLiveBtn_->setChecked(true);
        pageLibraryBtn_->setChecked(false);
        PanelAnimator::slideInWidget(stack_->currentWidget());
        refreshLive();
    });
    connect(pageLibraryBtn_, &QPushButton::clicked, this, [this]() {
        stack_->setCurrentIndex(1);
        pageLiveBtn_->setChecked(false);
        pageLibraryBtn_->setChecked(true);
        PanelAnimator::slideInWidget(stack_->currentWidget());
    });

    // 填充教学场景列表
    const auto& items = WatchpointLibrary::scenarios();
    for (const auto& s : items) {
        scenarioList_->addItem(QString::fromUtf8(s.title.c_str()));
    }
    if (!items.empty())
        scenarioList_->setCurrentRow(0);

    // 实时 watchpoint 列表轮询定时器降频 500ms→2000ms（安全网）。
    // watchpoint 变化/步进/暂停由 vmStateChanged 监听器即时触发 refreshLive。
    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(2000);
    connect(refreshTimer_, &QTimer::timeout, this, &WatchpointPanel::refreshLive);

    // 主题切换时刷新教学场景详情 HTML（populateScenarioDetail 中 <pre> 背景使用
    // TeachingTheme::surface()，需重新渲染以跟随新主题）。
    Theme::onThemeModeChanged(this, [this](Fluent::ThemeMode) {
        if (scenarioList_ && scenarioDetail_) {
            populateScenarioDetail(scenarioList_->currentRow());
        }
    });
}

void WatchpointPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (refreshTimer_ && !refreshTimer_->isActive()) {
        refreshLive();
        refreshTimer_->start();
    }
}

void WatchpointPanel::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    if (refreshTimer_ && refreshTimer_->isActive()) {
        refreshTimer_->stop();
    }
}

void WatchpointPanel::setController(IdeController* controller) {
    if (controller_ == controller)
        return;
    // 注册前若已有 controller，先反注册旧监听器避免悬垂。
    if (controller_) {
        controller_->removeVmStateChangedListener(this);
    }
    controller_ = controller;
    if (controller_) {
        controller_->addVmStateChangedListener(this, [this] { onVmStateChanged(); });
    }
}

WatchpointPanel::~WatchpointPanel() {
    if (controller_) {
        controller_->removeVmStateChangedListener(this);
    }
}

void WatchpointPanel::buildLivePage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);

    liveStatusLabel_ = new CaptionLabel(QString::fromUtf8("未绑定控制器"), host);
    layout->addWidget(liveStatusLabel_);

    // 6 列：类型 / 变量名 / 字段名 / 条件 / 命中次数 / 状态
    watchpointTable_ = new QTableWidget(0, 6, host);
    watchpointTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    watchpointTable_->setHorizontalHeaderLabels({
        QString::fromUtf8("类型"),
        QString::fromUtf8("变量名"),
        QString::fromUtf8("字段名"),
        QString::fromUtf8("条件"),
        QString::fromUtf8("命中次数"),
        QString::fromUtf8("状态"),
    });
    watchpointTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    watchpointTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    watchpointTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    watchpointTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    watchpointTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    watchpointTable_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    layout->addWidget(watchpointTable_, 2);

    // 添加 Watchpoint 控件区
    auto* addGroup = new QWidget(host);
    auto* addLayout = new QVBoxLayout(addGroup);
    addLayout->setContentsMargins(0, 4, 0, 4);
    auto* addLabel = new CaptionLabel(QString::fromUtf8("添加 Watchpoint（监视变量/字段被修改时暂停）"), addGroup);
    addLayout->addWidget(addLabel);

    // 第一行：类型选择 + 变量名 + 字段名
    auto* row1 = new QHBoxLayout();
    kindCombo_ = new QComboBox(addGroup);
    kindCombo_->addItem(QString::fromUtf8("变量"), static_cast<int>(WatchpointTargetKind::Variable));
    kindCombo_->addItem(QString::fromUtf8("字段"), static_cast<int>(WatchpointTargetKind::Field));
    row1->addWidget(new QLabel(QString::fromUtf8("类型:"), addGroup));
    row1->addWidget(kindCombo_);
    varNameEdit_ = new QLineEdit(addGroup);
    varNameEdit_->setPlaceholderText(QString::fromUtf8("变量名（如 i / obj）"));
    row1->addWidget(new QLabel(QString::fromUtf8("变量名:"), addGroup));
    row1->addWidget(varNameEdit_, 2);
    fieldNameEdit_ = new QLineEdit(addGroup);
    fieldNameEdit_->setPlaceholderText(QString::fromUtf8("字段名（仅字段类型，如 count）"));
    fieldNameEdit_->setEnabled(false);
    row1->addWidget(new QLabel(QString::fromUtf8("字段名:"), addGroup));
    row1->addWidget(fieldNameEdit_, 2);
    addLayout->addLayout(row1);

    // 第二行：条件 + 添加/移除/清除按钮
    auto* row2 = new QHBoxLayout();
    conditionEdit_ = new QLineEdit(addGroup);
    conditionEdit_->setPlaceholderText(QString::fromUtf8("条件表达式（可选，如 i > 5；空=无条件）"));
    row2->addWidget(new QLabel(QString::fromUtf8("条件:"), addGroup));
    row2->addWidget(conditionEdit_, 2);
    addBtn_ = new QPushButton(QString::fromUtf8("添加"), addGroup);
    removeBtn_ = new QPushButton(QString::fromUtf8("移除选中"), addGroup);
    clearBtn_ = new QPushButton(QString::fromUtf8("清空全部"), addGroup);
    row2->addWidget(addBtn_);
    row2->addWidget(removeBtn_);
    row2->addWidget(clearBtn_);
    addLayout->addLayout(row2);
    layout->addWidget(addGroup);

    // 类型切换时更新字段名输入控件可用性
    connect(kindCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &WatchpointPanel::updateFieldInputVisibility);

    // 添加按钮
    connect(addBtn_, &QPushButton::clicked, this, &WatchpointPanel::addWatchpointFromInput);

    // 移除选中行
    connect(removeBtn_, &QPushButton::clicked, this, [this]() {
        if (!controller_)
            return;
        int row = watchpointTable_->currentRow();
        if (row < 0 || row >= watchpointTable_->rowCount())
            return;
        // 从表格行读取变量名和字段名（与表格显示一致）
        QString varName = watchpointTable_->item(row, 1)->text();
        QString fieldName = watchpointTable_->item(row, 2)->text();
        controller_->removeWatchpoint(varName.toStdString(), fieldName.isEmpty() ? "" : fieldName.toStdString());
        refreshLive();
    });

    // 清空全部
    connect(clearBtn_, &QPushButton::clicked, this, [this]() {
        if (controller_) {
            controller_->clearWatchpoints();
            refreshLive();
        }
    });
}

void WatchpointPanel::buildLibraryPage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    auto* splitter = new QSplitter(Qt::Horizontal, host);
    scenarioList_ = new QListWidget(host);
    scenarioDetail_ = new QTextBrowser(host);
    scenarioDetail_->setOpenExternalLinks(false);
    splitter->addWidget(scenarioList_);
    splitter->addWidget(scenarioDetail_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);
    splitter->setSizes({200, 400});
    layout->addWidget(splitter, 1);

    loadSampleBtn_ = new QPushButton(QString::fromUtf8("加载示例代码到编辑器"), host);
    loadSampleBtn_->setFixedHeight(28);
    layout->addWidget(loadSampleBtn_);
    connect(loadSampleBtn_, &QPushButton::clicked, this, [this]() {
        if (!currentSampleCode_.empty()) {
            emit loadSampleRequested(QString::fromUtf8(currentSampleCode_.c_str()));
        }
    });

    connect(scenarioList_, &QListWidget::currentRowChanged, this, &WatchpointPanel::populateScenarioDetail);
}

void WatchpointPanel::refreshLive() {
    if (!controller_) {
        liveStatusLabel_->setText(QString::fromUtf8("未绑定控制器"));
        return;
    }
    // 双路径状态守卫：调试运行中（VM 或 Interpreter）且未暂停时不刷新
    if (controller_->isRunning() && controller_->isDebugRun() && !controller_->isDebugPaused()) {
        liveStatusLabel_->setText(QString::fromUtf8("状态：调试运行中（暂停后刷新）"));
        return;
    }
    if (controller_->isVmRunning() && !controller_->isDebugPaused()) {
        liveStatusLabel_->setText(QString::fromUtf8("状态：VM 运行中（暂停后刷新）"));
        return;
    }

    const QVector<WatchpointInfo>& wps = controller_->getWatchpoints();
    if (wps.isEmpty()) {
        liveStatusLabel_->setText(QString::fromUtf8("无 Watchpoint"));
        watchpointTable_->setRowCount(0);
        return;
    }

    // 状态文本
    bool isRunning = controller_->isRunning();
    bool isDebugRun = controller_->isDebugRun();
    bool isVmRunning = controller_->isVmRunning();
    QString modeText;
    if (isVmRunning) {
        modeText = controller_->isVmRegisterMode() ? QString::fromUtf8("RegisterVM 调试中")
                                                   : QString::fromUtf8("StackVM 调试中");
    } else if (isRunning && isDebugRun) {
        modeText = QString::fromUtf8("Interpreter 调试中");
    } else {
        modeText = QString::fromUtf8("未运行");
    }
    liveStatusLabel_->setText(QString::fromUtf8("共 %1 个 Watchpoint | %2").arg(wps.size()).arg(modeText));

    watchpointTable_->setRowCount(wps.size());
    for (int i = 0; i < wps.size(); ++i) {
        const WatchpointInfo& wp = wps[i];
        QString kindText =
            (wp.kind == WatchpointTargetKind::Field) ? QString::fromUtf8("字段") : QString::fromUtf8("变量");
        watchpointTable_->setItem(i, 0, new QTableWidgetItem(kindText));
        watchpointTable_->setItem(i, 1, new QTableWidgetItem(QString::fromUtf8(wp.varName.c_str())));
        watchpointTable_->setItem(i, 2,
                                  new QTableWidgetItem(wp.fieldName.empty() ? QString::fromUtf8("—")
                                                                            : QString::fromUtf8(wp.fieldName.c_str())));
        watchpointTable_->setItem(i, 3,
                                  new QTableWidgetItem(wp.condition.empty() ? QString::fromUtf8("（无条件）")
                                                                            : QString::fromUtf8(wp.condition.c_str())));
        watchpointTable_->setItem(i, 4, new QTableWidgetItem(QString::number(wp.hitCount)));
        // 状态：综合显示
        QString status;
        if (wp.isConditional()) {
            status = QString::fromUtf8("条件 Watchpoint");
        } else if (wp.kind == WatchpointTargetKind::Field) {
            status = QString::fromUtf8("字段 Watchpoint");
        } else {
            status = QString::fromUtf8("变量 Watchpoint");
        }
        watchpointTable_->setItem(i, 5, new QTableWidgetItem(status));
    }
}

void WatchpointPanel::populateScenarioDetail(int index) {
    const auto& items = WatchpointLibrary::scenarios();
    if (index < 0 || index >= (int)items.size()) {
        currentSampleCode_.clear();
        return;
    }
    const auto& s = items[index];
    currentSampleCode_ = s.sampleCode;

    // 散文式字段（description）走 Markdown 渲染
    QString descFragment = MarkdownRenderer::markdownToHtmlFragment(s.description);

    // HTML 转义——sampleCode/condition 含 < > 字符
    auto escapeHtml = [](const std::string& str) -> std::string {
        std::string out;
        out.reserve(str.size());
        for (char c : str) {
            switch (c) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&#39;";
                break;
            default:
                out += c;
                break;
            }
        }
        return out;
    };

    std::ostringstream os;
    os << "<h3>" << escapeHtml(s.title) << "</h3>";
    os << "<p><b>监视目标：</b> <code>" << escapeHtml(s.target) << "</code></p>";
    if (!s.condition.empty()) {
        os << "<p><b>条件表达式：</b> <code>" << escapeHtml(s.condition) << "</code></p>";
    }
    os << "<hr>";
    os << descFragment.toStdString();
    os << "<p><b>触发行为：</b> " << escapeHtml(s.expectedBehavior) << "</p>";
    os << "<h4>示例代码：</h4>";
    os << "<pre style='background:" << TeachingTheme::surface().name().toStdString()
       << "; padding:8px; font-family:\"Cascadia Code\",\"Cascadia Mono\",\"Consolas\",\"JetBrains Mono\",\"Source "
          "Code Pro\",\"Menlo\",\"DejaVu Sans Mono\",\"Courier New\",monospace;'>"
       << escapeHtml(s.sampleCode) << "</pre>";
    scenarioDetail_->setHtml(QString::fromUtf8(os.str().c_str()));
}

void WatchpointPanel::addWatchpointFromInput() {
    if (!controller_)
        return;
    std::string varName = varNameEdit_->text().toStdString();
    if (varName.empty()) {
        liveStatusLabel_->setText(QString::fromUtf8("错误：变量名不能为空"));
        return;
    }
    WatchpointInfo wp;
    int kindData = kindCombo_->currentData().toInt();
    wp.kind = static_cast<WatchpointTargetKind>(kindData);
    wp.varName = varName;
    if (wp.kind == WatchpointTargetKind::Field) {
        std::string fieldName = fieldNameEdit_->text().toStdString();
        if (fieldName.empty()) {
            liveStatusLabel_->setText(QString::fromUtf8("错误：字段类型需指定字段名"));
            return;
        }
        wp.fieldName = fieldName;
    }
    std::string cond = conditionEdit_->text().toStdString();
    if (!cond.empty()) {
        wp.condition = cond;
    }
    controller_->setWatchpoint(wp);
    // 清空输入
    varNameEdit_->clear();
    fieldNameEdit_->clear();
    conditionEdit_->clear();
    refreshLive();
}

void WatchpointPanel::updateFieldInputVisibility() {
    int kindData = kindCombo_->currentData().toInt();
    bool isField = (static_cast<WatchpointTargetKind>(kindData) == WatchpointTargetKind::Field);
    fieldNameEdit_->setEnabled(isField);
    if (!isField) {
        fieldNameEdit_->clear();
    }
    // 变量名 placeholder 跟随类型变化
    if (isField) {
        varNameEdit_->setPlaceholderText(QString::fromUtf8("接收者变量名（如 obj）"));
    } else {
        varNameEdit_->setPlaceholderText(QString::fromUtf8("变量名（如 i / sum）"));
    }
}

// ============================================================
// createGuidedTour — 新手引导（5 步）
// ============================================================

GuidedTour* WatchpointPanel::createGuidedTour(QWidget* host) {
    auto* tour = new GuidedTour(host, host);
    tour->addStep(pageLiveBtn_, QString::fromUtf8("实时 Watchpoint"),
                  QString::fromUtf8("「实时 Watchpoint」页显示当前所有数据断点的类型 / 变量名 / "
                                    "字段名 / 条件 / 命中次数 / 状态。每行一个 watchpoint。"));
    tour->addStep(nullptr, QString::fromUtf8("示例代码：变量监视"),
                  QString::fromUtf8("<p>将以下代码粘贴到编辑器，添加 watchpoint 观察变量写入：</p>"
                                    "<pre style='background:#F5F5F5;padding:8px;border-radius:4px;font-family:"
                                    "\"Cascadia Code\",\"Cascadia Mono\",\"Consolas\",\"JetBrains Mono\","
                                    "\"Source Code Pro\",\"Menlo\",\"DejaVu Sans Mono\",\"Courier New\",monospace;'>"
                                    "var x = 0;\n"
                                    "x = 1;\n"
                                    "x = 2;\n"
                                    "print(x);\n"
                                    "</pre>"
                                    "<p>在 Watchpoint 面板添加变量 x 的 watchpoint，按 F5 调试，"
                                    "观察每次 x 被赋值时暂停。</p>"));
    tour->addStep(pageLibraryBtn_, QString::fromUtf8("教学场景库"),
                  QString::fromUtf8("点击「教学场景库」切换到静态教学页，查看变量监视 / "
                                    "字段监视 / 条件 watchpoint 等典型场景。"));
    tour->addStep(nullptr, QString::fromUtf8("字段 Watchpoint"),
                  QString::fromUtf8("<p>字段 watchpoint 用于追踪对象状态变化：</p>"
                                    "<pre style='background:#F5F5F5;padding:8px;border-radius:4px;font-family:"
                                    "\"Cascadia Code\",\"Cascadia Mono\",\"Consolas\",\"JetBrains Mono\","
                                    "\"Source Code Pro\",\"Menlo\",\"DejaVu Sans Mono\",\"Courier New\",monospace;'>"
                                    "class C {\n"
                                    "    var count = 0;\n"
                                    "    fun inc() { count = count + 1; }\n"
                                    "}\n"
                                    "var obj = C();\n"
                                    "obj.inc();\n"
                                    "</pre>"
                                    "<p>添加字段 watchpoint（类型=字段，变量名=obj，字段名=count），"
                                    "每次 count 被写入时暂停。</p>"));
    tour->addStep(nullptr, QString::fromUtf8("开始实验"),
                  QString::fromUtf8("切换到教学场景库后，选中场景，点击「加载示例代码到编辑器」载入编辑器，"
                                    "在 Watchpoint 面板添加对应 watchpoint，按 F5 调试，"
                                    "观察变量/字段被修改时暂停的行为。"));
    return tour;
}
