// ============================================================
// LearningPathPanel.cpp — 学习路径地图面板实现（功能 6）
// ============================================================
// 实现要点：
//   1. 顶部 QProgressBar 显示总进度
//   2. 主体 QScrollArea 内嵌 5 阶段卡片（0~4），每阶段：
//        - 阶段标题 + 阶段进度条
//        - 活动项列表（图标 + 标题 + 描述 + 预计耗时）
//   3. 推荐活动用主题色高亮 + "← 当前推荐" 标签
//   4. 底部刷新按钮 + 重置进度按钮（带确认对话框）
//   5. 所有用户可见文本用 mlTr() 包裹（i18n）
//   6. 已解锁活动行使用 QPushButton（flat 样式）实现点击
// ============================================================

#include "gui/LearningPathPanel.h"
#include "gui/I18n.h"
#include "gui/PanelAnimator.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QProgressBar>
#include <QLabel>
#include <QPushButton>
#include <QFrame>
#include <QMessageBox>
#include <QSizePolicy>

#include <sstream>

#include "PushButton.h"   // QFluentKit（PrimaryPushButton）
#include "Label.h"        // QFluentKit（StrongBodyLabel）

// ============================================================
// 阶段颜色与标题
// ============================================================
QString LearningPathPanel::stageColor(int stage) {
    switch (stage) {
        case 0: return QString::fromUtf8("#4CAF50"); // 绿
        case 1: return QString::fromUtf8("#FFC107"); // 黄
        case 2: return QString::fromUtf8("#2196F3"); // 蓝
        case 3: return QString::fromUtf8("#9C27B0"); // 紫
        case 4: return QString::fromUtf8("#F44336"); // 红
        default: return QString::fromUtf8("#9E9E9E");
    }
}

QString LearningPathPanel::stageTitle(int stage) {
    switch (stage) {
        case 0: return mlTr("阶段零：首次接触");
        case 1: return mlTr("阶段一：编译前端");
        case 2: return mlTr("阶段二：执行引擎");
        case 3: return mlTr("阶段三：深入理解");
        case 4: return mlTr("阶段四：实战训练");
        default: return mlTr("未知阶段");
    }
}

// ============================================================
// 构造函数
// ============================================================
LearningPathPanel::LearningPathPanel(QWidget* parent)
    : QWidget(parent) {

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(4);

    // ---- 顶部：总进度条 ----
    auto* topBar = new QHBoxLayout;
    overallLabel_ = new StrongBodyLabel(mlTr("总进度: 0%"), this);
    overallProgress_ = new QProgressBar(this);
    overallProgress_->setRange(0, 100);
    overallProgress_->setValue(0);
    overallProgress_->setTextVisible(false);
    overallProgress_->setFixedHeight(18);
    topBar->addWidget(overallLabel_, 0);
    topBar->addWidget(overallProgress_, 1);
    mainLayout->addLayout(topBar);

    // ---- 主体：滚动区域 + 阶段卡片 ----
    scrollArea_ = new QScrollArea(this);
    scrollArea_->setWidgetResizable(true);
    scrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    scrollContent_ = new QWidget(scrollArea_);
    stagesLayout_ = new QVBoxLayout(scrollContent_);
    stagesLayout_->setContentsMargins(2, 2, 2, 2);
    stagesLayout_->setSpacing(8);

    // 占位：实际填充在 refresh() 中完成
    stagesLayout_->addStretch(1);
    scrollArea_->setWidget(scrollContent_);
    mainLayout->addWidget(scrollArea_, 1);

    // ---- 底部：刷新 + 重置 ----
    auto* bottomBar = new QHBoxLayout;
    refreshBtn_ = new PrimaryPushButton(mlTr("刷新"), this);
    resetBtn_   = new QPushButton(mlTr("重置进度"), this);
    bottomBar->addWidget(refreshBtn_);
    bottomBar->addStretch(1);
    bottomBar->addWidget(resetBtn_);
    mainLayout->addLayout(bottomBar);

    // ---- 信号连接 ----
    connect(refreshBtn_, &QPushButton::clicked, this, &LearningPathPanel::onRefresh);
    connect(resetBtn_,   &QPushButton::clicked, this, &LearningPathPanel::onResetProgress);

    // 首次加载进度并刷新显示
    LearnerProgressStore::instance().load();
    refresh();
}

// ============================================================
// 刷新整个面板
// ============================================================
void LearningPathPanel::refresh() {
    // 清空旧的阶段卡片（保留末尾的 stretch）
    while (stagesLayout_->count() > 1) {
        QLayoutItem* item = stagesLayout_->takeAt(0);
        if (item->widget()) {
            item->widget()->deleteLater();
        }
        delete item;
    }

    auto& store = LearnerProgressStore::instance();
    const auto& all = allActivities();

    // 顶部总进度
    int overall = store.overallProgress(all);
    overallProgress_->setValue(overall);
    overallLabel_->setText(mlTr("总进度: %1%").arg(overall));

    // 阶段卡片
    for (int stage = 0; stage < LearningPathData::stageCount(); ++stage) {
        auto* card = buildStageCard(stage);
        if (card) {
            stagesLayout_->insertWidget(stagesLayout_->count() - 1, card);
        }
    }

    PanelAnimator::fadeInWidget(scrollContent_);
}

// ============================================================
// 构造阶段卡片
// ============================================================
QFrame* LearningPathPanel::buildStageCard(int stage) {
    auto* frame = new QFrame(scrollContent_);
    frame->setFrameShape(QFrame::StyledPanel);
    QString color = stageColor(stage);

    frame->setStyleSheet(QString::fromUtf8(
        "QFrame#stageCard { border: 1px solid %1; border-radius: 6px; }"
    ).arg(color));
    frame->setObjectName(QString::fromUtf8("stageCard"));

    auto* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(4);

    // 阶段标题 + 阶段进度条
    auto* header = new QHBoxLayout;
    QString title = stageTitle(stage);
    auto* titleLabel = new QLabel(QString::fromUtf8("<b style='color:%1;'>%2</b>")
                                       .arg(color).arg(title), frame);

    auto& store = LearnerProgressStore::instance();
    const auto& all = allActivities();
    int sp = store.stageProgress(stage, all);

    auto* stageProgress = new QProgressBar(frame);
    stageProgress->setRange(0, 100);
    stageProgress->setValue(sp);
    stageProgress->setTextVisible(true);
    stageProgress->setFormat(QString::fromUtf8("%p%"));
    stageProgress->setFixedWidth(120);
    stageProgress->setFixedHeight(16);

    header->addWidget(titleLabel, 1);
    header->addWidget(stageProgress, 0);
    layout->addLayout(header);

    // 该阶段下所有活动项
    std::string recommendedId = store.nextRecommended(all);
    auto stageActivities = LearningPathData::byStage(stage);
    for (const auto* act : stageActivities) {
        bool unlocked = store.isUnlocked(act->id, all);
        bool completed = false;
        auto cit = store.data().completed.find(act->id);
        if (cit != store.data().completed.end() && cit->second) completed = true;
        bool recommended = (act->id == recommendedId);

        auto* row = buildActivityRow(*act, unlocked, completed, recommended);
        layout->addWidget(row);
    }

    return frame;
}

// ============================================================
// 构造单个活动项行（使用 QPushButton 实现可点击）
// ============================================================
QWidget* LearningPathPanel::buildActivityRow(const LearningActivity& activity,
                                              bool unlocked, bool completed,
                                              bool recommended) {
    // 使用 QPushButton 作为行容器，flat 模式去除按钮样式
    auto* row = new QPushButton(scrollContent_);
    row->setFlat(true);
    row->setCheckable(false);
    row->setFocusPolicy(Qt::NoFocus);
    row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    row->setCursor(unlocked ? Qt::PointingHandCursor : Qt::ArrowCursor);
    row->setEnabled(unlocked);  // 未解锁的活动禁用点击

    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(4, 2, 4, 2);
    layout->setSpacing(8);

    // 图标：☑ / ☐ / 🔒
    QString icon;
    if (completed) {
        icon = QString::fromUtf8("\xe2\x98\x91");  // ☑ U+2611
    } else if (!unlocked) {
        icon = QString::fromUtf8("\xf0\x9f\x94\x92");  // 🔒 lock emoji
    } else {
        icon = QString::fromUtf8("\xe2\x98\x90");  // ☐ U+2610
    }
    auto* iconLabel = new QLabel(icon, row);
    iconLabel->setFixedSize(22, 22);
    iconLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(iconLabel, 0);

    // 中间：标题 + 描述
    auto* mid = new QVBoxLayout;
    mid->setSpacing(1);

    QString title = QString::fromStdString(activity.title);
    if (!unlocked) {
        title = QString::fromUtf8("<span style='color:#999;'>%1</span>").arg(title);
    } else if (completed) {
        title = QString::fromUtf8("<span style='color:#4CAF50;'><b>%1</b> ✓</span>").arg(title);
    } else {
        title = QString::fromUtf8("<b>%1</b>").arg(title);
    }

    auto* titleLabel = new QLabel(title, row);
    titleLabel->setTextFormat(Qt::RichText);
    mid->addWidget(titleLabel);

    QString desc = QString::fromStdString(activity.description);
    desc = QString::fromUtf8("<span style='color:#666; font-size:90%%;'>%1</span>").arg(desc);
    auto* descLabel = new QLabel(desc, row);
    descLabel->setTextFormat(Qt::RichText);
    descLabel->setWordWrap(true);
    mid->addWidget(descLabel);

    layout->addLayout(mid, 1);

    // 预计耗时
    QString timeStr = mlTr("~%1 分钟").arg(activity.estimatedMinutes);
    auto* timeLabel = new QLabel(timeStr, row);
    timeLabel->setStyleSheet(QString::fromUtf8("color:#888;"));
    layout->addWidget(timeLabel, 0);

    // 推荐标签
    if (recommended && unlocked && !completed) {
        auto* recLabel = new QLabel(QString::fromUtf8("<b style='color:%1;'>\xe2\x86\x90 %2</b>")
            .arg(stageColor(activity.stage))
            .arg(mlTr("当前推荐")), row);
        recLabel->setTextFormat(Qt::RichText);
        layout->addWidget(recLabel, 0);

        // 高亮整行背景
        row->setStyleSheet(QString::fromUtf8(
            "QPushButton { background-color: rgba(0,0,0,0); border-radius: 4px; }"
            "QPushButton:hover { background-color: rgba(%1, 0.18); }"
        ).arg(stageColor(activity.stage)));
    } else if (unlocked) {
        row->setStyleSheet(QString::fromUtf8(
            "QPushButton { background-color: rgba(0,0,0,0); border: none; }"
            "QPushButton:hover { background-color: rgba(0,0,0,0.06); }"
        ));
    } else {
        row->setStyleSheet(QString::fromUtf8(
            "QPushButton { background-color: rgba(0,0,0,0); border: none; color: #999; }"
        ));
    }

    // 连接点击信号
    if (unlocked) {
        QString actId = QString::fromStdString(activity.id);
        connect(row, &QPushButton::clicked, this, [this, actId]() {
            onActivityClicked(actId);
        });
    }

    return row;
}

// ============================================================
// 刷新按钮槽
// ============================================================
void LearningPathPanel::onRefresh() {
    LearnerProgressStore::instance().load();
    refresh();
}

// ============================================================
// 重置进度按钮槽
// ============================================================
void LearningPathPanel::onResetProgress() {
    // 带确认对话框
    QMessageBox::StandardButton reply = QMessageBox::question(
        this,
        mlTr("重置进度"),
        mlTr("确定要重置所有学习进度吗？此操作不可撤销。"),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No
    );
    if (reply == QMessageBox::Yes) {
        LearnerProgressStore::instance().reset();
        LearnerProgressStore::instance().save();
        refresh();
    }
}

// ============================================================
// 活动点击槽
// ============================================================
void LearningPathPanel::onActivityClicked(const QString& activityId) {
    if (activityId.isEmpty()) return;
    auto& store = LearnerProgressStore::instance();
    store.recordAttempt(activityId.toStdString());
    store.save();
    emit activityRequested(activityId);
}

// ============================================================
// 外部调用：标记活动完成
// ============================================================
void LearningPathPanel::markActivityCompleted(const QString& activityId) {
    if (activityId.isEmpty()) return;
    auto& store = LearnerProgressStore::instance();
    store.markCompleted(activityId.toStdString());
    store.save();
    refresh();
}
