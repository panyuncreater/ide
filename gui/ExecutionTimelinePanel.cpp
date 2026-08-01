// ============================================================
// ExecutionTimelinePanel.cpp — 可回放执行时间轴面板实现（R114 阶段 1）
// ============================================================

#include "gui/ExecutionTimelinePanel.h"

#include "gui/TeachingTheme.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>

// ============================================================
// 构造与布局
// ============================================================

ExecutionTimelinePanel::ExecutionTimelinePanel(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // 标题重复修复：面板不再自建 TeachingPanelHeader——Ide::wrapTeachingPanel 已在
    // 教学面板外壳顶部统一插入标题栏（含帮助 / 新手引导 / 返回编辑器按钮），面板内
    // 再建一个会导致「可回放执行时间轴」标题重复显示两行。返回编辑器 / 引导按钮由
    // 外壳标题栏统一处理（wrapTeachingPanel 已 connect），面板保留同名 signal 以兼容
    // 既有 connect 但不再需要自行发射。

    // 工具栏
    auto* toolbarHost = new QWidget(this);
    buildToolbar(toolbarHost);
    root->addWidget(toolbarHost);

    // 中央区域
    auto* centralHost = new QWidget(this);
    buildCentralArea(centralHost);
    root->addWidget(centralHost, 1);

    // 刷新定时器（录制期间 200ms 刷新）
    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(200);
    refreshTimer_->setSingleShot(false);
    connect(refreshTimer_, &QTimer::timeout, this, &ExecutionTimelinePanel::onRefreshTimer);

    updateStepLabel();
    updateSliderRange();
    updateControlsEnabled();
    showEmptyHint();
}

void ExecutionTimelinePanel::buildToolbar(QWidget* host) {
    auto* layout = new QHBoxLayout(host);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(6);

    recordBtn_ = new QPushButton(tr("录制"), host);
    recordBtn_->setCheckable(true);
    recordBtn_->setToolTip(tr("开始/停止录制执行轨迹"));
    recordBtn_->setStyleSheet("QPushButton { padding: 4px 12px; }"
                              "QPushButton:checked { background-color: #DC322F; color: white; }");

    clearBtn_ = new QPushButton(tr("清空"), host);
    clearBtn_->setToolTip(tr("清空所有已录制的快照"));

    firstBtn_ = new QPushButton(tr("⏮"), host);
    firstBtn_->setToolTip(tr("跳到首步"));
    firstBtn_->setFixedWidth(32);
    prevBtn_ = new QPushButton(tr("◀"), host);
    prevBtn_->setToolTip(tr("上一步"));
    prevBtn_->setFixedWidth(32);
    nextBtn_ = new QPushButton(tr("▶"), host);
    nextBtn_->setToolTip(tr("下一步"));
    nextBtn_->setFixedWidth(32);
    lastBtn_ = new QPushButton(tr("⏭"), host);
    lastBtn_->setToolTip(tr("跳到末步"));
    lastBtn_->setFixedWidth(32);

    slider_ = new QSlider(Qt::Horizontal, host);
    slider_->setMinimum(0);
    slider_->setMaximum(0);
    slider_->setEnabled(false);

    stepLabel_ = new QLabel("0 / 0", host);
    stepLabel_->setFixedWidth(80);
    stepLabel_->setAlignment(Qt::AlignCenter);

    backendLabel_ = new QLabel(backendName(backend_), host);
    backendLabel_->setStyleSheet(
        QString("QLabel { color: %1; padding: 2px 8px; border: 1px solid %2; border-radius: 3px; }")
            .arg(TeachingTheme::info().name(), TeachingTheme::border().name()));

    layout->addWidget(recordBtn_);
    layout->addWidget(clearBtn_);
    layout->addSpacing(12);
    layout->addWidget(firstBtn_);
    layout->addWidget(prevBtn_);
    layout->addWidget(slider_, 1);
    layout->addWidget(nextBtn_);
    layout->addWidget(lastBtn_);
    layout->addWidget(stepLabel_);
    layout->addSpacing(8);
    layout->addWidget(backendLabel_);

    connect(recordBtn_, &QPushButton::toggled, this, &ExecutionTimelinePanel::onRecordingToggled);
    connect(clearBtn_, &QPushButton::clicked, this, &ExecutionTimelinePanel::onClearClicked);
    connect(firstBtn_, &QPushButton::clicked, this, &ExecutionTimelinePanel::onFirstStep);
    connect(prevBtn_, &QPushButton::clicked, this, &ExecutionTimelinePanel::onPrevStep);
    connect(nextBtn_, &QPushButton::clicked, this, &ExecutionTimelinePanel::onNextStep);
    connect(lastBtn_, &QPushButton::clicked, this, &ExecutionTimelinePanel::onLastStep);
    connect(slider_, &QSlider::valueChanged, this, [this](int value) {
        if (!slider_->isSliderDown()) {
            navigateToStep(value);
        }
    });
    connect(slider_, &QSlider::sliderMoved, this, &ExecutionTimelinePanel::onSliderMoved);
}

void ExecutionTimelinePanel::buildCentralArea(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* splitter = new QSplitter(Qt::Horizontal, host);
    splitter->setChildrenCollapsible(false);

    stepList_ = new QListWidget(splitter);
    stepList_->setSelectionMode(QAbstractItemView::SingleSelection);
    stepList_->setStyleSheet("QListWidget { border: none; border-right: 1px solid #E0E0E0; font-family: Consolas, "
                             "'Courier New', monospace; font-size: 11pt; }"
                             "QListWidget::item { padding: 2px 6px; }"
                             "QListWidget::item:selected { background-color: #CCE4F7; color: #1E1E1E; }");

    detailView_ = new QTextBrowser(splitter);
    detailView_->setOpenExternalLinks(false);
    detailView_->setStyleSheet("QTextBrowser { border: none; font-family: Consolas, 'Courier New', monospace; "
                               "font-size: 10pt; background-color: #FFFFFF; }");

    splitter->addWidget(stepList_);
    splitter->addWidget(detailView_);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);
    splitter->setSizes({300, 500});

    layout->addWidget(splitter, 1);

    connect(stepList_, &QListWidget::currentRowChanged, this, &ExecutionTimelinePanel::onStepSelected);
}

// ============================================================
// 公共 API
// ============================================================

void ExecutionTimelinePanel::setController(IdeController* controller) {
    controller_ = controller;
}

void ExecutionTimelinePanel::setBackendType(TraceBackend backend) {
    if (backend_ == backend)
        return;
    backend_ = backend;
    if (backendLabel_) {
        backendLabel_->setText(backendName(backend));
    }
}

// ============================================================
// 显示/隐藏事件
// ============================================================

void ExecutionTimelinePanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // 显示时若正在录制，启动刷新定时器
    if (recordBtn_ && recordBtn_->isChecked()) {
        refreshTimer_->start();
    }
}

void ExecutionTimelinePanel::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    // 隐藏时停止刷新定时器（录制继续，只是不刷新 UI）
    if (refreshTimer_) {
        refreshTimer_->stop();
    }
}

// ============================================================
// 槽函数
// ============================================================

void ExecutionTimelinePanel::onRecordingToggled(bool checked) {
    if (checked) {
        traceRecorder().startSession();
        refreshTimer_->start();
        lastSeenSize_ = 0;
        currentStepIdx_ = -1;
        stepList_->clear();
        // UX：录制中给出即时反馈，而非空白详情区
        detailView_->setHtml(tr("<div style='color:#859900; padding:16px;'>"
                                "<h3>\xE2\x8F\xBA 正在录制…</h3>"
                                "<p>请在主编辑器运行代码，执行轨迹会实时出现在左侧列表。</p>"
                                "<p>再次点击「录制」停止，即可开始回放。</p></div>"));
        if (recordBtn_)
            recordBtn_->setText(tr("\xE2\x8F\xBA 录制中"));
    } else {
        traceRecorder().endSession();
        refreshTimer_->stop();
        if (recordBtn_)
            recordBtn_->setText(tr("录制"));
        refreshStepList(); // 最终刷新
        if (traceRecorder().size() == 0)
            showEmptyHint();
    }
    updateControlsEnabled();
    // 通知 IdeController 联动 VmStepper / Interpreter 的 recorder 启停
    // backend_ 由 IdeController 在 setBackendType() 时同步，captureVmStep/captureInterpreterStep
    // 接收 backend 参数写入每步快照（recorder 本身不持有全局 backend 字段）
    emit recordingToggled(checked, backend_);
}

void ExecutionTimelinePanel::onClearClicked() {
    traceRecorder().clear();
    lastSeenSize_ = 0;
    currentStepIdx_ = -1;
    stepList_->clear();
    detailView_->clear();
    updateStepLabel();
    updateSliderRange();
    updateControlsEnabled();
    showEmptyHint();
}

void ExecutionTimelinePanel::onFirstStep() {
    navigateToStep(0);
}

void ExecutionTimelinePanel::onPrevStep() {
    navigateToStep(currentStepIdx_ - 1);
}

void ExecutionTimelinePanel::onNextStep() {
    navigateToStep(currentStepIdx_ + 1);
}

void ExecutionTimelinePanel::onLastStep() {
    size_t total = traceRecorder().size();
    navigateToStep(static_cast<int>(total) - 1);
}

void ExecutionTimelinePanel::onSliderMoved(int value) {
    navigateToStep(value);
}

void ExecutionTimelinePanel::onStepSelected(int row) {
    if (row < 0)
        return;
    navigateToStep(row);
}

void ExecutionTimelinePanel::onRefreshTimer() {
    refreshStepList();
}

// ============================================================
// 数据填充
// ============================================================

void ExecutionTimelinePanel::refreshStepList() {
    size_t total = traceRecorder().size();
    if (total == lastSeenSize_) {
        // 数量未变，仅更新步数标签
        updateStepLabel();
        return;
    }

    // 增量追加新步骤（避免全量重建）
    // 注意：ring buffer 满后旧快照被 erase(begin())，索引会偏移。
    // 简化处理：若总数量小于已显示数量（ring buffer 偏移），全量重建。
    if (total < lastSeenSize_ || total - lastSeenSize_ > 100) {
        // 全量重建
        stepList_->clear();
        auto snapshots = traceRecorder().allSnapshots();
        for (const auto& snap : snapshots) {
            stepList_->addItem(formatStepItem(snap));
        }
    } else {
        // 增量追加
        for (size_t i = lastSeenSize_; i < total; ++i) {
            auto snap = traceRecorder().stepAt(i);
            if (snap) {
                stepList_->addItem(formatStepItem(*snap));
            }
        }
    }
    lastSeenSize_ = total;
    updateStepLabel();
    updateSliderRange();
    updateControlsEnabled();

    // 若用户未选中任何步骤，自动选中最后一步（跟踪最新状态）
    if (currentStepIdx_ < 0 && total > 0) {
        navigateToStep(static_cast<int>(total) - 1);
    }
}

void ExecutionTimelinePanel::showStepDetail(size_t idx) {
    auto snap = traceRecorder().stepAt(idx);
    if (!snap) {
        detailView_->clear();
        return;
    }
    detailView_->setHtml(formatDetailHtml(*snap));
}

void ExecutionTimelinePanel::updateStepLabel() {
    size_t total = traceRecorder().size();
    int current = currentStepIdx_ >= 0 ? currentStepIdx_ + 1 : 0;
    stepLabel_->setText(QString("%1 / %2").arg(current).arg(total));
}

void ExecutionTimelinePanel::updateSliderRange() {
    size_t total = traceRecorder().size();
    if (total == 0) {
        slider_->setMinimum(0);
        slider_->setMaximum(0);
        slider_->setEnabled(false);
        return;
    }
    slider_->setMinimum(0);
    slider_->setMaximum(static_cast<int>(total) - 1);
    slider_->setEnabled(true);
}

// 将导航按钮 / 清空按钮的可用性与当前数据 / 位置同步，避免空数据时可点
// 、已在首尾时方向键无意义。UX 优化的一部分。
void ExecutionTimelinePanel::updateControlsEnabled() {
    const size_t total = traceRecorder().size();
    const bool hasData = total > 0;
    const bool recording = recordBtn_ && recordBtn_->isChecked();
    const int last = static_cast<int>(total) - 1;
    if (clearBtn_)
        clearBtn_->setEnabled(hasData && !recording);
    // 首 / 上一步：当前不在第 0 步且有数据时可用
    if (firstBtn_)
        firstBtn_->setEnabled(hasData && currentStepIdx_ != 0);
    if (prevBtn_)
        prevBtn_->setEnabled(hasData && currentStepIdx_ != 0);
    // 下一 / 末步：当前不在末步且有数据时可用
    if (nextBtn_)
        nextBtn_->setEnabled(hasData && currentStepIdx_ != last);
    if (lastBtn_)
        lastBtn_->setEnabled(hasData && currentStepIdx_ != last);
}

// 空状态提示：未录制 / 已清空时在详情区展示引导文案，避免空白困惑。
void ExecutionTimelinePanel::showEmptyHint() {
    if (!detailView_)
        return;
    detailView_->setHtml(tr("<div style='color:#8C8C8C; padding:16px;'>"
                            "<h3>可回放执行时间轴</h3>"
                            "<p>点击左上角「录制」按钮开始记录执行轨迹，然后在主编辑器运行代码。</p>"
                            "<p>录制结束后，可通过滑块 / 步进按钮 / 点击列表项，随机访问任意一步的栈、"
                            "局部变量、全局变量与调用栈快照。</p></div>"));
}

void ExecutionTimelinePanel::navigateToStep(int idx) {
    size_t total = traceRecorder().size();
    if (total == 0) {
        currentStepIdx_ = -1;
        updateStepLabel();
        return;
    }
    // 钳制到合法范围
    if (idx < 0)
        idx = 0;
    if (idx >= static_cast<int>(total))
        idx = static_cast<int>(total) - 1;
    currentStepIdx_ = idx;

    // 同步 UI 控件（避免信号回环）
    if (slider_->value() != idx) {
        slider_->blockSignals(true);
        slider_->setValue(idx);
        slider_->blockSignals(false);
    }
    if (stepList_->currentRow() != idx) {
        stepList_->blockSignals(true);
        stepList_->setCurrentRow(idx);
        stepList_->blockSignals(false);
    }
    // 滚动到当前行可见
    stepList_->scrollToItem(stepList_->item(idx), QAbstractItemView::PositionAtCenter);

    showStepDetail(static_cast<size_t>(idx));
    updateStepLabel();
    updateControlsEnabled();
}

// ============================================================
// 格式化辅助
// ============================================================

QString ExecutionTimelinePanel::backendName(TraceBackend b) {
    switch (b) {
    case TraceBackend::Interpreter:
        return tr("Interpreter");
    case TraceBackend::StackVM:
        return tr("StackVM");
    case TraceBackend::RegisterVM:
        return tr("RegisterVM");
    }
    return tr("Unknown");
}

QString ExecutionTimelinePanel::formatStepItem(const TraceSnapshot& snap) {
    // 格式：[step] line:X op:Y (frame:Z)
    return QString("#%1  L%2  %3  [f%4]")
        .arg(snap.step)
        .arg(snap.line)
        .arg(QString::fromStdString(snap.opOrNodeName))
        .arg(snap.frameCount);
}

QString ExecutionTimelinePanel::formatDetailHtml(const TraceSnapshot& snap) {
    QString html;
    html += "<html><body style='font-family: Consolas, monospace; font-size: 10pt;'>";
    html += QString("<h3 style='color:#268BD2; margin:0 0 8px 0;'>Step #%1 — %2</h3>")
                .arg(snap.step)
                .arg(backendName(snap.backend));
    html += QString("<table style='margin-bottom:8px;'>"
                    "<tr><td style='color:#8C8C8C;'>行号:</td><td>%1</td>"
                    "<td style='color:#8C8C8C; padding-left:16px;'>IP:</td><td>%2</td>"
                    "<td style='color:#8C8C8C; padding-left:16px;'>帧深:</td><td>%3</td></tr>"
                    "<tr><td style='color:#8C8C8C;'>操作:</td><td colspan='5'>%4</td></tr>"
                    "</table>")
                .arg(snap.line)
                .arg(snap.ip)
                .arg(snap.frameCount)
                .arg(QString::fromStdString(snap.opOrNodeName));

    // 操作数栈 / 寄存器
    const auto& stack = (snap.backend == TraceBackend::RegisterVM && !snap.registerStrings.empty())
                            ? snap.registerStrings
                            : snap.stackStrings;
    if (!stack.empty()) {
        html += "<h4 style='color:#859900; margin:8px 0 4px 0;'>栈 / 寄存器</h4><div style='background:#F5F5F5; "
                "padding:4px; border:1px solid #E0E0E0;'>";
        for (size_t i = 0; i < stack.size(); ++i) {
            html += QString("<div><span style='color:#8C8C8C;'>[%1]</span> %2</div>")
                        .arg(i)
                        .arg(QString::fromStdString(stack[i]).toHtmlEscaped());
        }
        html += "</div>";
    }

    // 可见变量
    if (!snap.visibleVarsStrings.empty()) {
        html += "<h4 style='color:#B58900; margin:8px 0 4px 0;'>可见变量</h4><table style='width:100%;'>";
        for (const auto& kv : snap.visibleVarsStrings) {
            html += QString("<tr><td style='color:#268BD2; width:35%;'>%1</td><td>%2</td></tr>")
                        .arg(QString::fromStdString(kv.first).toHtmlEscaped())
                        .arg(QString::fromStdString(kv.second).toHtmlEscaped());
        }
        html += "</table>";
    }

    // 调用栈
    if (!snap.callStack.empty()) {
        html += "<h4 style='color:#DC322F; margin:8px 0 4px 0;'>调用栈</h4>";
        for (size_t i = 0; i < snap.callStack.size(); ++i) {
            const auto& f = snap.callStack[i];
            html += QString("<div style='margin:2px 0; padding:2px 4px; background:%1;'>"
                            "<b>%2</b> @ L%3 (depth=%4)</div>")
                        .arg(i == snap.callStack.size() - 1 ? "#CCE4F7" : "#F5F5F5")
                        .arg(QString::fromStdString(f.functionName).toHtmlEscaped())
                        .arg(f.line)
                        .arg(f.depth);
            if (!f.locals.empty()) {
                html += "<table style='margin-left:16px;'>";
                for (const auto& kv : f.locals) {
                    html += QString("<tr><td style='color:#8C8C8C;'>%1</td><td>%2</td></tr>")
                                .arg(QString::fromStdString(kv.first).toHtmlEscaped())
                                .arg(QString::fromStdString(kv.second.toString()).toHtmlEscaped());
                }
                html += "</table>";
            }
        }
    }

    // 全局变量
    if (!snap.globalsStrings.empty()) {
        html += "<h4 style='color:#8C8C8C; margin:8px 0 4px 0;'>全局变量</h4><table style='width:100%;'>";
        for (const auto& kv : snap.globalsStrings) {
            html += QString("<tr><td style='color:#268BD2; width:35%;'>%1</td><td>%2</td></tr>")
                        .arg(QString::fromStdString(kv.first).toHtmlEscaped())
                        .arg(QString::fromStdString(kv.second).toHtmlEscaped());
        }
        html += "</table>";
    }

    html += "</body></html>";
    return html;
}
