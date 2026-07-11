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

#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>

#include <sstream>

#include "Label.h"             // QFluentKit（StrongBodyLabel）
#include "PushButton.h"        // QFluentKit（PrimaryPushButton）
#include "gui/TeachingTheme.h" // learningStageColor() 5 阶段统一配色

// ============================================================
// 阶段颜色与标题
// 阶段色统一走 TeachingTheme::learningStageColor()，保证
// LearningPathPanel / WelcomeWizard Step 4 / CodeJourneyInfoPanel 三处一致
// ============================================================
/// 返回指定阶段的主题配色（用于阶段标签）。
QString LearningPathPanel::stageColor(int stage) {
    return TeachingTheme::learningStageColor(stage).name();
}

/// 返回指定阶段的标题文案。
QString LearningPathPanel::stageTitle(int stage) {
    switch (stage) {
    case 0:
        return mlTr("阶段零：首次接触");
    case 1:
        return mlTr("阶段一：编译前端");
    case 2:
        return mlTr("阶段二：执行引擎");
    case 3:
        return mlTr("阶段三：深入理解");
    case 4:
        return mlTr("阶段四：实战训练");
    default:
        return mlTr("未知阶段");
    }
}

// ============================================================
// 构造函数
// ============================================================
/// 构造学习路径面板：初始化阶段列表容器与导航状态。
LearningPathPanel::LearningPathPanel(QWidget* parent) : QWidget(parent) {

    // M9: 让面板可接收键盘焦点，以支持 Up/Down/Enter 键盘导航
    setFocusPolicy(Qt::StrongFocus);

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
    // P1-2 fix (F8): 当前阶段提示 label，显示「你正处于：阶段 X — 标题」
    stageLabel_ = new QLabel(QString::fromUtf8(""), this);
    stageLabel_->setStyleSheet(QString::fromUtf8("QLabel { padding: 2px 8px; border-radius: 4px;"
                                                 "  background: %1; color: white; font-weight: bold; }")
                                   .arg(TeachingTheme::primary().name()));
    topBar->addWidget(overallLabel_, 0);
    topBar->addWidget(stageLabel_, 0);
    topBar->addWidget(overallProgress_, 1);

    // P2-3 fix (F9): 学情画像——薄弱点提示 + 预计剩余时间
    weakPointsLabel_ = new QLabel(this);
    weakPointsLabel_->setStyleSheet(QString::fromUtf8("QLabel { padding: 2px 8px; border-radius: 4px;"
                                                      "  background: %1; color: white; font-weight: bold; }")
                                        .arg(TeachingTheme::warning().name()));
    weakPointsLabel_->hide(); // 默认隐藏，仅在有薄弱点时显示
    remainingLabel_ = new QLabel(this);
    remainingLabel_->setStyleSheet(QString::fromUtf8("QLabel { padding: 2px 8px; border-radius: 4px;"
                                                     "  background: %1; color: white; font-weight: bold; }")
                                       .arg(TeachingTheme::success().name()));
    topBar->addWidget(weakPointsLabel_, 0);
    topBar->addWidget(remainingLabel_, 0);

    // 搜索框：按 id/title/description 小写包含过滤活动
    searchEdit_ = new QLineEdit(this);
    searchEdit_->setPlaceholderText(mlTr("搜索活动..."));
    searchEdit_->setClearButtonEnabled(true);
    searchEdit_->setMaximumWidth(200);
    searchEdit_->setStyleSheet(QString::fromUtf8("QLineEdit { padding: 4px 8px; border: 1px solid %1;"
                                                 "  border-radius: 4px; background: %2; color: %3; }"
                                                 "QLineEdit:focus { border: 1px solid %4; }")
                                   .arg(TeachingTheme::border().name(), TeachingTheme::surface().name(),
                                        TeachingTheme::textPrimary().name(), TeachingTheme::primary().name()));
    // 搜索框防抖：避免逐字符触发 refresh() 导致重建风暴。
    // 用户停止输入 250ms 后才真正刷新列表。
    searchDebounceTimer_ = new QTimer(this);
    searchDebounceTimer_->setSingleShot(true);
    searchDebounceTimer_->setInterval(250);
    connect(searchDebounceTimer_, &QTimer::timeout, this, [this]() { refresh(); });
    connect(searchEdit_, &QLineEdit::textChanged, this, [this](const QString&) {
        searchDebounceTimer_->start(); // 重启计时器（防抖）
    });
    topBar->addWidget(searchEdit_);
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
    resetBtn_ = new QPushButton(mlTr("重置进度"), this);
    bottomBar->addWidget(refreshBtn_);
    bottomBar->addStretch(1);
    bottomBar->addWidget(resetBtn_);
    mainLayout->addLayout(bottomBar);

    // ---- 信号连接 ----
    connect(refreshBtn_, &QPushButton::clicked, this, &LearningPathPanel::onRefresh);
    connect(resetBtn_, &QPushButton::clicked, this, &LearningPathPanel::onResetProgress);

    // 首次加载进度并刷新显示
    LearnerProgressStore::instance().load();
    refresh();
}

// ============================================================
// 刷新整个面板
// ============================================================
/// 重新读取进度并刷新整个学习路径视图。
void LearningPathPanel::refresh() {
    // M9: 清空键盘导航状态（旧 row 即将被销毁，引用不再有效）
    activityRows_.clear();
    currentNavIndex_ = -1;
    highlightedSavedStyle_.clear();

    // 清空旧的阶段卡片（保留末尾的 stretch）
    // 注：必须用 deleteLater() 而非 delete，因为 refresh() 可能从
    // row->clicked 信号槽调用链中触发（onActivityClicked→onActivityRequested
    // →showTeachingPanel→markActivityCompleted→refresh），立即删除信号发送者
    // 会在 Qt 信号分发期间引发 use-after-free。旧 widget 从 layout 移除后
    // 不可见，deleteLater 在事件循环返回时安全清理。
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

    // P1-2 fix (F8): 更新当前阶段提示 label
    int curStage = store.data().currentStage;
    if (stageLabel_) {
        if (curStage >= LearningPathData::stageCount()) {
            // 全部通关
            stageLabel_->setText(QString::fromUtf8("🎉 %1").arg(mlTr("已通关所有阶段")));
            stageLabel_->setStyleSheet(
                QString::fromUtf8("QLabel { padding: 2px 8px; border-radius: 4px;"
                                  "  background: %1; color: white; font-weight: bold; }")
                    .arg(TeachingTheme::learningStageColor(LearningPathData::stageCount() - 1).name()));
        } else {
            // 显示当前阶段标题（标题已含「阶段X：标题」格式）
            stageLabel_->setText(QString::fromUtf8("📍 %1").arg(stageTitle(curStage)));
            stageLabel_->setStyleSheet(QString::fromUtf8("QLabel { padding: 2px 8px; border-radius: 4px;"
                                                         "  background: %1; color: white; font-weight: bold; }")
                                           .arg(TeachingTheme::learningStageColor(curStage).name()));
        }
    }

    // P2-3 fix (F9): 更新薄弱点提示
    if (weakPointsLabel_) {
        auto weakPoints = store.getWeakPoints(all);
        if (weakPoints.empty()) {
            weakPointsLabel_->hide();
        } else {
            // 取第 1 个薄弱点活动标题展示（点击可跳转）
            const LearningActivity* wp = nullptr;
            for (const auto& a : all) {
                if (a.id == weakPoints.front().activityId) {
                    wp = &a;
                    break;
                }
            }
            QString wpText;
            if (wp) {
                wpText = mlTr("⚠ 薄弱点: %1 (失败 %2 次)")
                             .arg(QString::fromStdString(wp->title))
                             .arg(weakPoints.front().fails);
            } else {
                wpText = mlTr("⚠ 薄弱点: %1 个").arg(weakPoints.size());
            }
            // 若有多个薄弱点，tooltip 列出全部
            if (weakPoints.size() > 1) {
                QStringList detailLines;
                for (const auto& wp2 : weakPoints) {
                    const LearningActivity* a2 = nullptr;
                    for (const auto& a : all) {
                        if (a.id == wp2.activityId) {
                            a2 = &a;
                            break;
                        }
                    }
                    QString title = a2 ? QString::fromStdString(a2->title) : QString::fromStdString(wp2.activityId);
                    detailLines
                        << QString::fromUtf8("%1 (尝试 %2 / 失败 %3)").arg(title).arg(wp2.attempts).arg(wp2.fails);
                }
                weakPointsLabel_->setToolTip(detailLines.join(QStringLiteral("\n")));
            } else {
                weakPointsLabel_->setToolTip(QString());
            }
            weakPointsLabel_->setText(wpText);
            weakPointsLabel_->show();
        }
    }

    // P2-3 fix (F9): 更新预计剩余时间
    if (remainingLabel_) {
        int remaining = store.estimatedRemainingMinutes(all);
        int spent = store.totalSpentMinutes();
        QString remText = mlTr("⏱ 剩余 ~%1 分钟").arg(remaining);
        if (spent > 0) {
            remText += mlTr(" · 已用 %1 分钟").arg(spent);
        }
        remainingLabel_->setText(remText);
        remainingLabel_->setToolTip(mlTr("剩余 = 所有未完成且已解锁活动的预计耗时之和"));
        remainingLabel_->show();
    }

    // 阶段卡片
    for (int stage = 0; stage < LearningPathData::stageCount(); ++stage) {
        auto* card = buildStageCard(stage);
        if (card) {
            stagesLayout_->insertWidget(stagesLayout_->count() - 1, card);
        }
    }

    // 不再调用 PanelAnimator::fadeInWidget(scrollContent_)：
    // QGraphicsOpacityEffect 会对含 100+ 子 widget 的 scrollContent_ 做
    // 离屏 pixmap 合成（每帧 O(n) 复杂度），是章节切换卡顿与偶发崩溃的根因。
    // 崩溃机制：deleteLater 延迟删除旧 widget 时，effect 的 pixmap 缓存仍
    // 引用已销毁子 widget → QPainter use-after-free。
    // 页面级过渡动画改由 Ide::showTeachingPanel 中的轻量 pos 滑入实现。
}

// ============================================================
// 构造阶段卡片
// ============================================================
QFrame* LearningPathPanel::buildStageCard(int stage) {
    auto* frame = new QFrame(scrollContent_);
    frame->setFrameShape(QFrame::StyledPanel);
    QString color = stageColor(stage);

    frame->setStyleSheet(
        QString::fromUtf8("QFrame#stageCard { border: 1px solid %1; border-radius: 6px; }").arg(color));
    frame->setObjectName(QString::fromUtf8("stageCard"));

    auto* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(4);

    // 阶段标题 + 阶段进度条
    auto* header = new QHBoxLayout;
    QString title = stageTitle(stage);
    auto* titleLabel = new QLabel(QString::fromUtf8("<b style='color:%1;'>%2</b>").arg(color).arg(title), frame);

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
    // 搜索过滤：按 id/title/description 大小写不敏感包含匹配
    QString filter = searchEdit_ ? searchEdit_->text().trimmed() : QString();
    for (const auto* act : stageActivities) {
        if (!filter.isEmpty()) {
            bool matches = QString::fromStdString(act->id).contains(filter, Qt::CaseInsensitive) ||
                           QString::fromStdString(act->title).contains(filter, Qt::CaseInsensitive) ||
                           QString::fromStdString(act->description).contains(filter, Qt::CaseInsensitive);
            if (!matches) {
                continue; // 跳过不匹配的活动
            }
        }

        bool unlocked = store.isUnlocked(act->id, all);
        bool completed = false;
        auto cit = store.data().completed.find(act->id);
        if (cit != store.data().completed.end() && cit->second)
            completed = true;
        bool recommended = (act->id == recommendedId);

        auto* row = buildActivityRow(*act, unlocked, completed, recommended);
        layout->addWidget(row);
    }

    return frame;
}

// ============================================================
// 构造单个活动项行（使用 QPushButton 实现可点击）
// ============================================================
/// 构建单个活动的可点击行控件。
QWidget* LearningPathPanel::buildActivityRow(const LearningActivity& activity, bool unlocked, bool completed,
                                             bool recommended) {
    // 使用 QPushButton 作为行容器，flat 模式去除按钮样式
    auto* row = new QPushButton(scrollContent_);
    row->setFlat(true);
    row->setCheckable(false);
    row->setFocusPolicy(Qt::NoFocus);
    row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    row->setCursor(unlocked ? Qt::PointingHandCursor : Qt::ArrowCursor);
    row->setEnabled(unlocked); // 未解锁的活动禁用点击

    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(4, 2, 4, 2);
    layout->setSpacing(8);

    // 图标：☑ / ☐ / 🔒
    QString icon;
    if (completed) {
        icon = QString::fromUtf8("\xe2\x98\x91"); // ☑ U+2611
    } else if (!unlocked) {
        icon = QString::fromUtf8("\xf0\x9f\x94\x92"); // 🔒 lock emoji
    } else {
        icon = QString::fromUtf8("\xe2\x98\x90"); // ☐ U+2610
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
        title = QString::fromUtf8("<span style='color:#6E6E6E;'>%1</span>").arg(title);
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
    auto& store = LearnerProgressStore::instance();
    int spent = store.getSpentMinutes(activity.id);
    if (spent > 0) {
        // P2-3 fix (F9): 已有实际耗时数据时，显示「~X 分钟 / 已用 Y 分钟」
        timeStr = mlTr("~%1 分钟 / 已用 %2").arg(activity.estimatedMinutes).arg(spent);
    }
    auto* timeLabel = new QLabel(timeStr, row);
    timeLabel->setStyleSheet(QString::fromUtf8("color:#6E6E6E;"));
    layout->addWidget(timeLabel, 0);

    // P2-3 fix (F9): 学情画像——显示得分/星级徽章（仅有记录时显示）
    int score = store.getScore(activity.id);
    int stars = store.getBestStars(activity.id);
    if (score > 0 || stars > 0) {
        QStringList parts;
        if (score > 0) {
            parts << mlTr("得分 %1").arg(score);
        }
        if (stars > 0) {
            // 用 ★ 字符显示星级
            QString starStr;
            for (int i = 0; i < stars && i < 3; ++i) {
                starStr += QString::fromUtf8("\xe2\x98\x85"); // ★ U+2605
            }
            parts << starStr;
        }
        if (!parts.isEmpty()) {
            auto* badge = new QLabel(parts.join(QString::fromUtf8(" · ")), row);
            badge->setStyleSheet(QString::fromUtf8("color: %1; font-weight: bold; padding: 1px 6px;"
                                                   "  border: 1px solid %2; border-radius: 3px;")
                                     .arg(TeachingTheme::success().name().left(7), TeachingTheme::success().name()));
            layout->addWidget(badge, 0);
        }
    }

    // 推荐标签
    if (recommended && unlocked && !completed) {
        auto* recLabel = new QLabel(QString::fromUtf8("<b style='color:%1;'>\xe2\x86\x90 %2</b>")
                                        .arg(stageColor(activity.stage))
                                        .arg(mlTr("当前推荐")),
                                    row);
        recLabel->setTextFormat(Qt::RichText);
        layout->addWidget(recLabel, 0);

        // 高亮整行背景
        // AUDIT-P2 fix: 原 rgba(%1, 0.18) 传 hex 字符串 (#859900) 给 QSS rgba() 函数
        // 不合法，QSS rgba 只接受数字参数 rgba(r, g, b, a)。改为展开为数字格式。
        QColor sc = stageColor(activity.stage);
        row->setStyleSheet(QString::fromUtf8("QPushButton { background-color: rgba(0,0,0,0); border-radius: 4px; }"
                                             "QPushButton:hover { background-color: rgba(%1, %2, %3, 46); }")
                               .arg(sc.red())
                               .arg(sc.green())
                               .arg(sc.blue()));
    } else if (unlocked) {
        row->setStyleSheet(QString::fromUtf8("QPushButton { background-color: rgba(0,0,0,0); border: none; }"
                                             "QPushButton:hover { background-color: rgba(0,0,0,0.06); }"));
    } else {
        row->setStyleSheet(
            QString::fromUtf8("QPushButton { background-color: rgba(0,0,0,0); border: none; color: #6E6E6E; }"));
        // 未解锁：tooltip 提示需要先完成的前置活动
        QStringList prereqTitles;
        for (const auto& prereqId : activity.prerequisites) {
            const LearningActivity* prereq = LearningPathData::findById(prereqId);
            if (prereq) {
                prereqTitles << QString::fromStdString(prereq->title);
            }
        }
        if (!prereqTitles.isEmpty()) {
            row->setToolTip(mlTr("需要先完成：%1 才能解锁").arg(prereqTitles.join(", ")));
        } else {
            row->setToolTip(mlTr("此活动尚未解锁"));
        }
    }

    // 连接点击信号
    if (unlocked) {
        QString actId = QString::fromStdString(activity.id);
        connect(row, &QPushButton::clicked, this, [this, actId]() { onActivityClicked(actId); });
        // M9: 收集到键盘导航列表中（仅已解锁项可被导航选中）
        activityRows_.append(row);

        // P2-3 fix (F9): 已解锁活动的 tooltip——展示完整学情画像
        int attempts = 0;
        {
            auto it = store.data().attemptCount.find(activity.id);
            if (it != store.data().attemptCount.end())
                attempts = it->second;
        }
        int fails = store.getFailCount(activity.id);
        if (attempts > 0 || fails > 0 || score > 0 || spent > 0 || stars > 0) {
            QStringList lines;
            lines << mlTr("尝试 %1 次").arg(attempts);
            if (fails > 0)
                lines << mlTr("失败 %1 次").arg(fails);
            if (score > 0)
                lines << mlTr("得分 %1").arg(score);
            if (stars > 0)
                lines << mlTr("星级 %1/3").arg(stars);
            if (spent > 0)
                lines << mlTr("已用 %1 分钟").arg(spent);
            row->setToolTip(lines.join(QStringLiteral(" · ")));
        }
    }

    return row;
}

// ============================================================
// M9: 键盘导航 — 高亮指定索引的活动行
// ============================================================
/// 高亮指定索引的活动行（键盘导航用）。
void LearningPathPanel::highlightActivityRow(int idx) {
    // 还原上一行样式
    if (currentNavIndex_ >= 0 && currentNavIndex_ < activityRows_.size()) {
        QPushButton* prev = activityRows_[currentNavIndex_];
        prev->setProperty("navHighlight", false);
        prev->setStyleSheet(highlightedSavedStyle_);
    }
    currentNavIndex_ = idx;
    if (idx >= 0 && idx < activityRows_.size()) {
        QPushButton* cur = activityRows_[idx];
        highlightedSavedStyle_ = cur->styleSheet();
        // 在原样式后追加蓝色边框高亮规则（属性选择器，仅当 navHighlight=true 时生效）
        cur->setProperty("navHighlight", true);
        cur->setStyleSheet(highlightedSavedStyle_ +
                           " QPushButton[navHighlight=\"true\"] { border: 2px solid #2196F3; border-radius: 4px; }");
        // 滚动到可见
        scrollArea_->ensureWidgetVisible(cur);
    }
}

// ============================================================
// M9: 键盘导航 — Up/Down 切换行，Enter 触发点击
// ============================================================
/// 键盘事件：上下键导航、回车进入活动。
void LearningPathPanel::keyPressEvent(QKeyEvent* event) {
    if (!activityRows_.isEmpty()) {
        if (event->key() == Qt::Key_Down) {
            int next = (currentNavIndex_ < 0) ? 0 : (currentNavIndex_ + 1) % activityRows_.size();
            highlightActivityRow(next);
            return;
        }
        if (event->key() == Qt::Key_Up) {
            int prev = (currentNavIndex_ < 0) ? activityRows_.size() - 1
                                              : (currentNavIndex_ - 1 + activityRows_.size()) % activityRows_.size();
            highlightActivityRow(prev);
            return;
        }
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            if (currentNavIndex_ >= 0 && currentNavIndex_ < activityRows_.size()) {
                activityRows_[currentNavIndex_]->click();
                return;
            }
        }
    }
    QWidget::keyPressEvent(event);
}

// ============================================================
// 刷新按钮槽
// ============================================================
/// 「刷新」按钮：重新加载进度数据。
void LearningPathPanel::onRefresh() {
    LearnerProgressStore::instance().load();
    refresh();
}

// ============================================================
// 重置进度按钮槽
// ============================================================
/// 「重置进度」按钮：清空学习者进度。
// 当前行为：reset 后 save。save 失败时内存已清空但磁盘旧数据保留，
// 弹窗告知用户（重启后旧数据回归，造成「假重置」）。这是已知的行为取舍。
void LearningPathPanel::onResetProgress() {
    // 带确认对话框
    QMessageBox::StandardButton reply =
        QMessageBox::question(this, mlTr("重置进度"), mlTr("确定要重置所有学习进度吗？此操作不可撤销。"),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply == QMessageBox::Yes) {
        LearnerProgressStore::instance().reset();
        if (!LearnerProgressStore::instance().save()) {
            // AUDIT-P2 fix: save 失败时告知用户，不静默丢失
            QMessageBox::warning(this, mlTr("重置失败"),
                                 mlTr("进度文件写入失败（磁盘满或权限不足），"
                                      "内存已重置但未落盘。重启后进度将恢复。"
                                      "请检查磁盘空间与写权限后重试。"));
        }
        refresh();
    }
}

// ============================================================
// 活动点击槽
// ============================================================
/// 活动行点击回调：请求打开对应面板。
void LearningPathPanel::onActivityClicked(const QString& activityId) {
    if (activityId.isEmpty())
        return;
    auto& store = LearnerProgressStore::instance();
    store.recordAttempt(activityId.toStdString());
    store.save();
    emit activityRequested(activityId);
}

// ============================================================
// 外部调用：标记活动完成
// ============================================================
/// 将某活动标记为已完成并刷新视图。
void LearningPathPanel::markActivityCompleted(const QString& activityId) {
    if (activityId.isEmpty())
        return;
    auto& store = LearnerProgressStore::instance();
    // AUDIT-P2 fix: 活动已完成则跳过 save，避免重复磁盘 I/O。
    // R52-8 fix: 幂等检查仅跳过 save，不跳过 refresh——跨面板首次完成场景下
    // store 已被发起面板（如 CodeJourneyInfoPanel）直接写入，但本面板 UI 尚未
    // 刷新，仍需 refresh() 更新进度条/活动状态显示。
    auto it = store.data().completed.find(activityId.toStdString());
    if (it != store.data().completed.end() && it->second) {
        refresh();
        return;
    }
    store.markCompleted(activityId.toStdString());
    store.save();
    refresh();
}
