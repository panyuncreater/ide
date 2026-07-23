// ============================================================
// WatchPanel.cpp — 观察表达式面板实现（R117 调试器拓展）
// ============================================================

#include "gui/WatchPanel.h"
#include "app/IdeController.h"
#include "gui/GuidedTour.h"
#include "gui/MarkdownRenderer.h"
#include "gui/PanelAnimator.h"
#include "gui/TeachingTheme.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QSplitter>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <algorithm>
#include <sstream>

#include "Label.h" // QFluentKit（CaptionLabel）
#include "Theme.h" // QFluentKit（onThemeModeChanged 信号）

// Note: WatchExpressionLibrary::scenarios() 实现位于 gui/WatchExpressionLibrary.cpp
// （独立编译单元，仅依赖标准库 + Qt6::Core，可加入测试目标）

// ============================================================
// WatchPanel 实现
// ============================================================

WatchPanel::WatchPanel(QWidget* parent) : QWidget(parent) {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(4);

    // 顶部页面切换按钮
    auto* pageBar = new QHBoxLayout;
    pageLiveBtn_ = new QPushButton(QString::fromUtf8("实时 watch"), this);
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
        refreshAll();
    });
    connect(pageLibraryBtn_, &QPushButton::clicked, this, [this]() {
        stack_->setCurrentIndex(1);
        pageLiveBtn_->setChecked(false);
        pageLibraryBtn_->setChecked(true);
        PanelAnimator::slideInWidget(stack_->currentWidget());
    });

    // 填充教学场景列表
    const auto& items = WatchExpressionLibrary::scenarios();
    for (const auto& s : items) {
        scenarioList_->addItem(QString::fromUtf8(s.title.c_str()));
    }
    if (!items.empty())
        scenarioList_->setCurrentRow(0);

    // OPT-1: 安全网 QTimer（vmStateChanged 即时刷新的兜底）
    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(2000);
    connect(refreshTimer_, &QTimer::timeout, this, &WatchPanel::refreshAll);

    // 从 QSettings 加载持久化的表达式
    loadExpressions();

    // 主题切换时刷新教学场景详情 HTML
    Theme::onThemeModeChanged(this, [this](Fluent::ThemeMode) {
        if (scenarioList_ && scenarioDetail_) {
            populateScenarioDetail(scenarioList_->currentRow());
        }
    });
}

void WatchPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // 面板显示时启动安全网 QTimer，首次立即刷新一次
    if (refreshTimer_ && !refreshTimer_->isActive()) {
        refreshAll();
        refreshTimer_->start();
    }
}

void WatchPanel::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    // 面板隐藏时停止 QTimer，避免后台空转
    if (refreshTimer_ && refreshTimer_->isActive()) {
        refreshTimer_->stop();
    }
}

void WatchPanel::setController(IdeController* controller) {
    if (controller_ == controller)
        return;
    // 注册前若已有 controller，先反注册旧监听器避免悬垂
    if (controller_) {
        controller_->removeVmStateChangedListener(this);
    }
    controller_ = controller;
    if (controller_) {
        controller_->addVmStateChangedListener(this, [this] { onVmStateChanged(); });
    }
}

// 析构时反注册监听器，避免 controller_ 持有悬垂 this 回调
WatchPanel::~WatchPanel() {
    if (controller_) {
        controller_->removeVmStateChangedListener(this);
    }
}

void WatchPanel::buildLivePage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    liveStatusLabel_ = new CaptionLabel(QString::fromUtf8("未绑定控制器"), host);
    layout->addWidget(liveStatusLabel_);

    // 表达式输入栏
    auto* inputBar = new QHBoxLayout;
    exprInput_ = new QLineEdit(host);
    exprInput_->setPlaceholderText(QString::fromUtf8("输入表达式（如 i、arr[i]、this.value）后回车添加"));
    addBtn_ = new QPushButton(QString::fromUtf8("添加"), host);
    removeBtn_ = new QPushButton(QString::fromUtf8("删除选中"), host);
    clearBtn_ = new QPushButton(QString::fromUtf8("清空"), host);
    refreshBtn_ = new QPushButton(QString::fromUtf8("刷新"), host);
    inputBar->addWidget(exprInput_, 1);
    inputBar->addWidget(addBtn_);
    inputBar->addWidget(removeBtn_);
    inputBar->addWidget(clearBtn_);
    inputBar->addWidget(refreshBtn_);
    layout->addLayout(inputBar);

    // 4 列：表达式 / 类型 / 值 / 状态
    watchTable_ = new QTableWidget(0, 4, host);
    // 表达式列可编辑，其余列只读
    watchTable_->setHorizontalHeaderLabels({
        QString::fromUtf8("表达式"),
        QString::fromUtf8("类型"),
        QString::fromUtf8("值"),
        QString::fromUtf8("状态"),
    });
    watchTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    watchTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    watchTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    watchTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    // 仅表达式列可编辑
    watchTable_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
                                 QAbstractItemView::AnyKeyPressed);
    layout->addWidget(watchTable_, 1);

    // 信号连接
    connect(addBtn_, &QPushButton::clicked, this, &WatchPanel::onAddExpression);
    connect(removeBtn_, &QPushButton::clicked, this, &WatchPanel::onRemoveSelected);
    connect(clearBtn_, &QPushButton::clicked, this, &WatchPanel::onClearAll);
    connect(refreshBtn_, &QPushButton::clicked, this, &WatchPanel::onRefresh);
    connect(exprInput_, &QLineEdit::returnPressed, this, &WatchPanel::onAddExpression);
    // 单元格编辑（用户修改表达式）
    connect(watchTable_, &QTableWidget::cellChanged, this, &WatchPanel::onCellChanged);
}

void WatchPanel::buildLibraryPage(QWidget* host) {
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
    connect(loadSampleBtn_, &QPushButton::clicked, this, &WatchPanel::onLoadSampleCode);

    connect(scenarioList_, &QListWidget::currentRowChanged, this, &WatchPanel::onScenarioSelected);
}

void WatchPanel::onAddExpression() {
    QString text = exprInput_->text().trimmed();
    if (text.isEmpty())
        return;
    // 防重复：若已存在相同表达式则不重复添加
    for (int r = 0; r < watchTable_->rowCount(); ++r) {
        if (auto* item = watchTable_->item(r, 0)) {
            if (item->text() == text) {
                exprInput_->clear();
                return;
            }
        }
    }
    editingGuard_ = true;
    int row = watchTable_->rowCount();
    watchTable_->insertRow(row);
    auto* exprItem = new QTableWidgetItem(text);
    exprItem->setFlags(exprItem->flags() | Qt::ItemIsEditable);
    auto* typeItem = new QTableWidgetItem(QString::fromUtf8("—"));
    typeItem->setFlags(typeItem->flags() & ~Qt::ItemIsEditable);
    auto* valueItem = new QTableWidgetItem(QString::fromUtf8("—"));
    valueItem->setFlags(valueItem->flags() & ~Qt::ItemIsEditable);
    auto* statusItem = new QTableWidgetItem(QString::fromUtf8("待求值"));
    statusItem->setFlags(statusItem->flags() & ~Qt::ItemIsEditable);
    watchTable_->setItem(row, 0, exprItem);
    watchTable_->setItem(row, 1, typeItem);
    watchTable_->setItem(row, 2, valueItem);
    watchTable_->setItem(row, 3, statusItem);
    editingGuard_ = false;
    exprInput_->clear();
    saveExpressions();
    // 立即求值一次
    refreshAll();
}

void WatchPanel::onRemoveSelected() {
    int row = watchTable_->currentRow();
    if (row < 0)
        return;
    editingGuard_ = true;
    watchTable_->removeRow(row);
    editingGuard_ = false;
    saveExpressions();
}

void WatchPanel::onClearAll() {
    editingGuard_ = true;
    watchTable_->setRowCount(0);
    editingGuard_ = false;
    saveExpressions();
}

void WatchPanel::onRefresh() {
    refreshAll();
}

void WatchPanel::onCellChanged(int row, int col) {
    // 仅表达式列（col==0）可编辑，且忽略程序化 setItem 触发
    if (col != 0 || editingGuard_)
        return;
    if (auto* item = watchTable_->item(row, 0)) {
        // 去除首尾空白
        QString trimmed = item->text().trimmed();
        if (trimmed != item->text()) {
            editingGuard_ = true;
            item->setText(trimmed);
            editingGuard_ = false;
        }
        // 空表达式：状态标记
        if (trimmed.isEmpty()) {
            if (auto* st = watchTable_->item(row, 3))
                st->setText(QString::fromUtf8("空表达式"));
            saveExpressions();
            return;
        }
    }
    saveExpressions();
    // 用户修改了表达式后立即求值
    refreshAll();
}

void WatchPanel::onScenarioSelected(int index) {
    currentScenarioIdx_ = index;
    populateScenarioDetail(index);
}

void WatchPanel::onLoadSampleCode() {
    if (!currentSampleCode_.empty()) {
        emit loadSampleRequested(QString::fromUtf8(currentSampleCode_.c_str()));
    }
}

void WatchPanel::populateScenarioDetail(int index) {
    const auto& scenarios = WatchExpressionLibrary::scenarios();
    if (index < 0 || index >= static_cast<int>(scenarios.size())) {
        scenarioDetail_->clear();
        currentSampleCode_.clear();
        return;
    }
    const auto& s = scenarios[index];
    currentSampleCode_ = s.sampleCode;

    // 用 Markdown 渲染场景说明
    std::ostringstream md;
    md << "## " << s.title << "\n\n";
    md << "**watch 表达式**：`" << s.expression << "`\n\n";
    md << "**预期类型**：" << s.expectedType << "\n\n";
    md << s.description << "\n\n";
    md << "### 示例代码\n\n";
    md << "```minilang\n" << s.sampleCode << "\n```\n";
    scenarioDetail_->setHtml(MarkdownRenderer::markdownToHtml(md.str(), TeachingTheme::surface().name()));
}

void WatchPanel::refreshAll() {
    if (!controller_) {
        liveStatusLabel_->setText(QString::fromUtf8("未绑定控制器"));
        return;
    }

    // 状态描述
    bool vmMode = controller_->isVmRunning() || controller_->isVmInitialized();
    bool interpPaused = controller_->isDebugPaused();
    // R121: 追加选中帧提示（用户在 CallStackPanel 选了某帧时显示）
    int selFrame = controller_->getSelectedFrame();
    QString frameHint;
    if ((vmMode || interpPaused) && selFrame >= 0) {
        frameHint = QString::fromUtf8(" | 已选帧: %1").arg(selFrame);
    }
    if (vmMode) {
        liveStatusLabel_->setText(QString::fromUtf8("VM 模式暂停中") + frameHint);
    } else if (interpPaused) {
        liveStatusLabel_->setText(QString::fromUtf8("Interpreter 调试暂停中") + frameHint);
    } else {
        liveStatusLabel_->setText(QString::fromUtf8("未在调试暂停状态（表达式将在暂停时求值）"));
    }

    // 逐行求值
    editingGuard_ = true;
    for (int r = 0; r < watchTable_->rowCount(); ++r) {
        auto* exprItem = watchTable_->item(r, 0);
        auto* typeItem = watchTable_->item(r, 1);
        auto* valueItem = watchTable_->item(r, 2);
        auto* statusItem = watchTable_->item(r, 3);
        if (!exprItem || !typeItem || !valueItem || !statusItem)
            continue;

        QString expr = exprItem->text().trimmed();
        if (expr.isEmpty()) {
            typeItem->setText(QString::fromUtf8("—"));
            valueItem->setText(QString::fromUtf8("—"));
            statusItem->setText(QString::fromUtf8("空表达式"));
            continue;
        }

        auto result = controller_->evaluateWatchExpression(expr.toStdString());
        if (result.ok) {
            typeItem->setText(QString::fromUtf8(result.typeName.c_str()));
            valueItem->setText(QString::fromUtf8(result.valueRepr.c_str()));
            statusItem->setText(QString::fromUtf8("OK"));
            // 类型着色：标量用绿色，容器用蓝色，实例用紫色，闭包/函数用橙色
            QString color = TeachingTheme::textPrimary().name();
            if (result.typeName == "int" || result.typeName == "float" || result.typeName == "bool" ||
                result.typeName == "null") {
                color = "#859900"; // Solarized green
            } else if (result.typeName == "string") {
                color = "#B58900"; // Solarized yellow
            } else if (result.typeName == "array" || result.typeName == "dict") {
                color = "#268BD2"; // Solarized blue
            } else if (result.typeName == "instance") {
                color = "#6C71C4"; // Solarized violet
            } else if (result.typeName == "closure" || result.typeName == "function" ||
                       result.typeName == "bound-method") {
                color = "#CB4B16"; // Solarized orange
            }
            typeItem->setForeground(QColor(color));
        } else {
            typeItem->setText(QString::fromUtf8("—"));
            valueItem->setText(QString::fromUtf8("—"));
            statusItem->setText(QString::fromUtf8(result.error.c_str()));
            statusItem->setForeground(QColor("#DC322F")); // Solarized red
        }
    }
    editingGuard_ = false;
}

void WatchPanel::saveExpressions() const {
    QSettings settings;
    QStringList exprs;
    for (int r = 0; r < watchTable_->rowCount(); ++r) {
        if (auto* item = watchTable_->item(r, 0)) {
            QString t = item->text().trimmed();
            if (!t.isEmpty())
                exprs << t;
        }
    }
    settings.setValue(QStringLiteral("watch/expressions"), exprs);
}

void WatchPanel::loadExpressions() {
    QSettings settings;
    QStringList exprs = settings.value(QStringLiteral("watch/expressions")).toStringList();
    editingGuard_ = true;
    for (const QString& e : exprs) {
        int row = watchTable_->rowCount();
        watchTable_->insertRow(row);
        auto* exprItem = new QTableWidgetItem(e);
        exprItem->setFlags(exprItem->flags() | Qt::ItemIsEditable);
        auto* typeItem = new QTableWidgetItem(QString::fromUtf8("—"));
        typeItem->setFlags(typeItem->flags() & ~Qt::ItemIsEditable);
        auto* valueItem = new QTableWidgetItem(QString::fromUtf8("—"));
        valueItem->setFlags(valueItem->flags() & ~Qt::ItemIsEditable);
        auto* statusItem = new QTableWidgetItem(QString::fromUtf8("待求值"));
        statusItem->setFlags(statusItem->flags() & ~Qt::ItemIsEditable);
        watchTable_->setItem(row, 0, exprItem);
        watchTable_->setItem(row, 1, typeItem);
        watchTable_->setItem(row, 2, valueItem);
        watchTable_->setItem(row, 3, statusItem);
    }
    editingGuard_ = false;
}

// ============================================================
// 新手引导（5 步）
// ============================================================
GuidedTour* WatchPanel::createGuidedTour(QWidget* host) {
    auto* tour = new GuidedTour(host);
    tour->addStep(exprInput_, QString::fromUtf8("输入 watch 表达式"),
                  QString::fromUtf8("在此输入任意 MiniLang 表达式（如 i、arr[i]、this.value），回车或点击添加"));
    tour->addStep(addBtn_, QString::fromUtf8("添加表达式"),
                  QString::fromUtf8("将表达式加入观察列表。重复表达式会被自动忽略"));
    tour->addStep(watchTable_, QString::fromUtf8("观察列表"),
                  QString::fromUtf8("4 列展示：表达式（可编辑）/ 类型 / 值 / 状态。每次步进或断点命中自动刷新"));
    tour->addStep(refreshBtn_, QString::fromUtf8("手动刷新"),
                  QString::fromUtf8("强制重新求值所有表达式。也可在调试暂停期间等待自动刷新"));
    tour->addStep(pageLibraryBtn_, QString::fromUtf8("教学场景库"),
                  QString::fromUtf8("浏览 6 个典型 watch 表达式场景，含求值机制说明和可加载的示例代码"));
    return tour;
}
