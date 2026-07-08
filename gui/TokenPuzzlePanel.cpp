// ============================================================
// TokenPuzzlePanel.cpp — 交互式 Token 拼图游戏面板实现（功能 3）
// ------------------------------------------------------------
// 纯前端游戏面板，不依赖引擎层（不调用 Lexer/Parser/Interpreter）。
// 仅依赖 Qt6 Widgets + TokenPuzzleData 静态数据。
//
// i18n：所有用户可见文本用 mlTr() 包裹（参考 gui/I18n.h）。
// ============================================================

#include "gui/TokenPuzzlePanel.h"
#include "gui/TokenPuzzleData.h"
#include "gui/I18n.h"
#include "gui/LearnerProgress.h"  // P0-2 fix (F7): 关卡星级持久化

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QComboBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QStandardItemModel>
#include <QBrush>
#include <QColor>
#include <QSizePolicy>
#include <QFont>
#include <QSignalBlocker>
#include <QStyle>  // style()->polish() / unpolish() 用于 QSS 动态属性刷新

#include "PushButton.h"   // QFluentKit（PrimaryPushButton）
#include "Label.h"        // QFluentKit（StrongBodyLabel）

// ============================================================
// 构造
// ============================================================

TokenPuzzlePanel::TokenPuzzlePanel(QWidget* parent)
    : QWidget(parent) {
    // 初始化完成记录：全部 -1（未完成）
    levelStars_.resize(TokenPuzzleLibrary::levelCount());
    for (int& s : levelStars_) s = -1;

    // P0-2 fix (F7): 从持久化存储加载已记录的关卡星级
    auto& store = LearnerProgressStore::instance();
    for (int i = 0; i < TokenPuzzleLibrary::levelCount(); ++i) {
        int s = store.getLevelStars(levelId(i).toStdString());
        if (s >= 0) levelStars_[i] = s;
    }

    buildUi();

    // P0-2 fix (F7): 已完成的关卡需在 UI 上解锁下一关
    // 第 1 关默认解锁，后续关卡仅当前一关已完成（stars >= 0）时解锁
    auto* model = qobject_cast<QStandardItemModel*>(levelCombo_->model());
    if (model) {
        for (int i = 1; i < TokenPuzzleLibrary::levelCount(); ++i) {
            if (levelStars_[i - 1] >= 0 && model->item(i)) {
                model->item(i)->setEnabled(true);
            }
        }
    }

    loadLevel(0);
    updateScoreDisplay();
}

// ============================================================
// UI 构建
// ============================================================

void TokenPuzzlePanel::buildUi() {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(6);

    // ---- 关卡芯片栏（独立一行，放在 QComboBox 上方）----
    // 提升关卡切换控件的视觉识别度和可操作性，与下方 levelCombo_ 双向同步
    auto* chipRow = new QHBoxLayout;
    chipRow->setContentsMargins(0, 0, 0, 0);
    chipRow->setSpacing(6);
    const auto& chipLevels = TokenPuzzleLibrary::levels();
    for (int i = 0; i < (int)chipLevels.size(); ++i) {
        auto* chip = new QPushButton(this);
        chip->setObjectName("levelChip");
        chip->setFixedSize(48, 32);
        // 芯片点击 → 同步到 QComboBox（触发 onLevelChanged → loadLevel → refreshLevelChips）
        const int chipIndex = i;
        connect(chip, &QPushButton::clicked, this, [this, chipIndex]() {
            if (chipIndex >= 0 && chipIndex < levelCombo_->count()) {
                levelCombo_->setCurrentIndex(chipIndex);
            }
        });
        levelChips_.append(chip);
        chipRow->addWidget(chip);
    }
    chipRow->addStretch();
    mainLayout->addLayout(chipRow);

    // ---- 顶部栏：关卡选择 + 得分 + 进度 ----
    auto* topBar = new QHBoxLayout;
    topBar->addWidget(new QLabel(mlTr("关卡:")));
    levelCombo_ = new QComboBox(this);
    levelCombo_->setObjectName("levelCombo");  // QSS 选择器匹配

    // 使用 QStandardItemModel 支持逐项 enable/disable（解锁机制）
    auto* model = new QStandardItemModel(this);
    const auto& levels = TokenPuzzleLibrary::levels();
    for (int i = 0; i < (int)levels.size(); ++i) {
        QString diffStars;
        switch (levels[i].difficulty) {
            case 1:  diffStars = QString::fromUtf8("⭐");       break;
            case 2:  diffStars = QString::fromUtf8("⭐⭐");     break;
            case 3:  diffStars = QString::fromUtf8("⭐⭐⭐");   break;
            default: diffStars = QString::fromUtf8("⭐");       break;
        }
        QString text = mlTr("第 %1 关  %2").arg(i + 1).arg(diffStars);
        auto* item = new QStandardItem(text);
        item->setEnabled(i == 0);  // 初始仅第 1 关解锁
        model->appendRow(item);
    }
    levelCombo_->setModel(model);

    topBar->addWidget(levelCombo_);
    topBar->addStretch();
    scoreLabel_   = new StrongBodyLabel(mlTr("得分: 0 / 15"), this);
    progressLabel_ = new QLabel(mlTr("已完成 0 / 5 关"), this);
    topBar->addWidget(scoreLabel_);
    topBar->addSpacing(12);
    topBar->addWidget(progressLabel_);
    mainLayout->addLayout(topBar);

    // ---- 中部上：目标语句 ----
    mainLayout->addWidget(new QLabel(mlTr("目标语句:")));
    targetCodeLabel_ = new QLabel(this);
    QFont monoFont(QString::fromUtf8("Consolas"));
    monoFont.setStyleHint(QFont::Monospace);
    monoFont.setPointSize(12);
    targetCodeLabel_->setFont(monoFont);
    targetCodeLabel_->setStyleSheet(
        "background-color: #f5f5f5; padding: 8px; border: 1px solid #ddd;"
        "border-radius: 4px;");
    targetCodeLabel_->setTextFormat(Qt::PlainText);
    mainLayout->addWidget(targetCodeLabel_);

    teachingPointLabel_ = new QLabel(this);
    teachingPointLabel_->setWordWrap(true);
    teachingPointLabel_->setStyleSheet("color: #555; font-style: italic;");
    mainLayout->addWidget(teachingPointLabel_);

    // ---- 中部中：打乱的 token 按钮 ----
    mainLayout->addWidget(new QLabel(mlTr("打乱的 token（点击添加到答案区）:")));
    shuffledContainer_ = new QWidget(this);
    shuffledContainer_->setStyleSheet("background-color: #fafafa; border: 1px solid #eee;");
    shuffledContainer_->setMinimumHeight(50);
    mainLayout->addWidget(shuffledContainer_);

    // ---- 中部下：玩家答案区 ----
    mainLayout->addWidget(new QLabel(mlTr("你的答案（点击 token 可移除）:")));
    answerList_ = new QListWidget(this);
    answerList_->setFont(monoFont);
    answerList_->setFlow(QListView::LeftToRight);   // 横向排列
    answerList_->setWrapping(true);
    answerList_->setSpacing(4);
    answerList_->setMinimumHeight(80);
    answerList_->setResizeMode(QListView::Adjust);
    mainLayout->addWidget(answerList_);

    mainLayout->addStretch();

    // ---- 底部：操作按钮 + 反馈 ----
    auto* bottomBar = new QHBoxLayout;
    checkBtn_ = new PrimaryPushButton(QString::fromUtf8("✓ ") + mlTr("检查答案"), this);
    hintBtn_  = new QPushButton(QString::fromUtf8("💡 ") + mlTr("提示"), this);
    skipBtn_  = new QPushButton(QString::fromUtf8("⏭ ") + mlTr("跳过"), this);
    resetBtn_ = new QPushButton(QString::fromUtf8("🔄 ") + mlTr("重置"), this);
    bottomBar->addWidget(checkBtn_);
    bottomBar->addWidget(hintBtn_);
    bottomBar->addWidget(skipBtn_);
    bottomBar->addWidget(resetBtn_);
    bottomBar->addStretch();
    mainLayout->addLayout(bottomBar);

    feedbackLabel_ = new QLabel(this);
    feedbackLabel_->setWordWrap(true);
    feedbackLabel_->setStyleSheet("color: #2c3e50; padding: 4px;");
    mainLayout->addWidget(feedbackLabel_);

    // ---- 信号连接 ----
    connect(levelCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &TokenPuzzlePanel::onLevelChanged);
    connect(checkBtn_, &QPushButton::clicked, this, &TokenPuzzlePanel::onCheckAnswer);
    connect(hintBtn_,  &QPushButton::clicked, this, &TokenPuzzlePanel::onShowHint);
    connect(skipBtn_,  &QPushButton::clicked, this, &TokenPuzzlePanel::onSkipLevel);
    connect(resetBtn_, &QPushButton::clicked, this, &TokenPuzzlePanel::onResetLevel);
    connect(answerList_, &QListWidget::itemClicked,
            this, [this](QListWidgetItem* item) {
        if (!item) return;
        int row = answerList_->row(item);
        onAnswerItemClicked(row);
    });

    // ---- Solarized 风格 QSS（QComboBox + 关卡芯片按钮）----
    // 动态属性 [current='true'] / [locked='true'] 在 refreshLevelChips() 中
    // 通过 setProperty + style()->polish() 触发重新评估
    setStyleSheet(QString::fromUtf8(
        "QComboBox#levelCombo { background: #FDF6E3; border: 1px solid #93A1A1; "
        "border-radius: 4px; padding: 4px 8px; }"
        "QComboBox#levelCombo:hover { border-color: #268BD2; }"
        "QPushButton#levelChip { background: #EEE8D5; border: 1px solid #93A1A1; "
        "border-radius: 4px; font-size: 11px; }"
        "QPushButton#levelChip:hover { border-color: #268BD2; background: #E5F3FB; }"
        "QPushButton#levelChip[current='true'] { background: #268BD2; color: white; "
        "border-color: #1E6FA3; font-weight: bold; }"
        "QPushButton#levelChip[locked='true'] { background: #EDEDED; color: #AAA; "
        "border-color: #CCC; }"
    ));
}

// ============================================================
// 关卡加载
// ============================================================

void TokenPuzzlePanel::loadLevel(int index) {
    const auto& levels = TokenPuzzleLibrary::levels();
    if (index < 0 || index >= (int)levels.size()) return;
    const auto& lv = levels[index];

    targetCodeLabel_->setText(QString::fromUtf8(lv.targetCode.c_str()));
    teachingPointLabel_->setText(QString::fromUtf8(lv.teachingPoint.c_str()));

    rebuildShuffledButtons();
    clearAnswer();
    hintUsedCount_ = 0;
    setFeedback(mlTr("第 %1 关已加载，点击下方打乱的 token 按正确顺序排列").arg(index + 1));
    refreshLevelChips();  // 同步芯片栏状态（current 高亮等）
}

// ============================================================
// 打乱 token 按钮重建
// ============================================================

void TokenPuzzlePanel::rebuildShuffledButtons() {
    // 获取或创建网格布局
    auto* grid = qobject_cast<QGridLayout*>(shuffledContainer_->layout());
    if (!grid) {
        grid = new QGridLayout(shuffledContainer_);
        grid->setContentsMargins(6, 6, 6, 6);
        grid->setSpacing(6);
    }

    // 清除旧按钮
    while (grid->count() > 0) {
        QLayoutItem* item = grid->takeAt(0);
        if (item->widget()) {
            delete item->widget();
        }
        delete item;
    }
    shuffledButtons_.clear();

    const auto& levels = TokenPuzzleLibrary::levels();
    if (currentLevelIndex_ < 0 || currentLevelIndex_ >= (int)levels.size()) return;
    const auto& shuffled = levels[currentLevelIndex_].shuffledTokens;

    const int cols = (int)shuffled.size() > 6 ? 6 : (int)shuffled.size();
    for (int i = 0; i < (int)shuffled.size(); ++i) {
        auto* btn = new QPushButton(QString::fromUtf8(shuffled[i].c_str()), shuffledContainer_);
        btn->setMinimumHeight(36);
        btn->setMinimumWidth(40);
        btn->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        const int buttonIndex = i;
        connect(btn, &QPushButton::clicked, this, [this, buttonIndex]() {
            addShuffledTokenToAnswer(buttonIndex);
        });
        grid->addWidget(btn, i / cols, i % cols);
        shuffledButtons_.append(btn);
    }
}

// ============================================================
// 答案区操作
// ============================================================

void TokenPuzzlePanel::clearAnswer() {
    answerList_->clear();
}

void TokenPuzzlePanel::addShuffledTokenToAnswer(int buttonIndex) {
    if (buttonIndex < 0 || buttonIndex >= shuffledButtons_.size()) return;
    QPushButton* btn = shuffledButtons_[buttonIndex];
    if (!btn || !btn->isEnabled()) return;

    const auto& levels = TokenPuzzleLibrary::levels();
    const auto& shuffled = levels[currentLevelIndex_].shuffledTokens;
    const QString token = QString::fromUtf8(shuffled[buttonIndex].c_str());

    auto* item = new QListWidgetItem(token, answerList_);
    item->setData(Qt::UserRole, buttonIndex);
    item->setTextAlignment(Qt::AlignCenter);
    answerList_->addItem(item);

    btn->setEnabled(false);
    setFeedback(mlTr("已添加 token「%1」，继续排列...").arg(token));
}

void TokenPuzzlePanel::onAnswerItemClicked(int row) {
    if (row < 0 || row >= answerList_->count()) return;
    QListWidgetItem* item = answerList_->item(row);
    if (!item) return;
    int buttonIndex = item->data(Qt::UserRole).toInt();
    if (buttonIndex >= 0 && buttonIndex < shuffledButtons_.size()) {
        shuffledButtons_[buttonIndex]->setEnabled(true);
    }
    delete answerList_->takeItem(row);
    setFeedback(mlTr("已移除一个 token"));
}

// ============================================================
// 检查答案
// ============================================================

void TokenPuzzlePanel::onCheckAnswer() {
    // AUDIT-P2 fix: 防重复守卫
    if (busy_) return;
    const auto& levels = TokenPuzzleLibrary::levels();
    if (currentLevelIndex_ < 0 || currentLevelIndex_ >= (int)levels.size()) return;
    // AUDIT-P2 fix: 通过 early return 后才禁用按钮+设标志，函数末尾恢复
    busy_ = true;
    checkBtn_->setEnabled(false);
    const auto& tokens = levels[currentLevelIndex_].tokens;

    // 收集玩家答案
    QStringList answer;
    for (int i = 0; i < answerList_->count(); ++i) {
        answer.append(answerList_->item(i)->text());
    }

    // 清除之前的高亮
    for (int i = 0; i < answerList_->count(); ++i) {
        answerList_->item(i)->setBackground(QBrush());
    }

    // 数量校验
    if (answer.size() != (int)tokens.size()) {
        setFeedback(mlTr("❌ 答案数量不对：你排了 %1 个 token，正确需要 %2 个")
            .arg(answer.size()).arg(tokens.size()), true);
        return;
    }

    // 逐 token 比对
    int firstWrong = -1;
    for (int i = 0; i < (int)tokens.size(); ++i) {
        if (answer[i] != QString::fromUtf8(tokens[i].c_str())) {
            if (firstWrong < 0) firstWrong = i;
        }
    }

    if (firstWrong >= 0) {
        // 高亮所有错误位置
        for (int i = 0; i < answerList_->count(); ++i) {
            if (answer[i] != QString::fromUtf8(tokens[i].c_str())) {
                answerList_->item(i)->setBackground(QColor(255, 200, 200));
            }
        }
        setFeedback(mlTr("❌ 第 %1 个 token 不对：你填了「%2」，应该是「%3」")
            .arg(firstWrong + 1)
            .arg(answer[firstWrong])
            .arg(QString::fromUtf8(tokens[firstWrong].c_str())), true);
        return;
    }

    // 完全正确
    int stars = computeStars(hintUsedCount_);
    if (stars > levelStars_[currentLevelIndex_]) {
        levelStars_[currentLevelIndex_] = stars;
    }
    // P0-2 fix (F7): 持久化关卡星级到 LearnerProgressStore
    LearnerProgressStore::instance().markLevelStars(
        levelId(currentLevelIndex_).toStdString(), stars);
    LearnerProgressStore::instance().save();
    setFeedback(mlTr("✅ 完全正确！获得 %1").arg(starsToText(stars)));
    unlockNextLevel();
    updateScoreDisplay();
    emit activityCompleted(levelId(currentLevelIndex_));
    // AUDIT-P2 fix: 恢复按钮+标志
    busy_ = false;
    checkBtn_->setEnabled(true);
}

// ============================================================
// 提示
// ============================================================

void TokenPuzzlePanel::onShowHint() {
    const auto& levels = TokenPuzzleLibrary::levels();
    if (currentLevelIndex_ < 0 || currentLevelIndex_ >= (int)levels.size()) return;
    hintUsedCount_++;
    const auto& hint = levels[currentLevelIndex_].hint;
    setFeedback(mlTr("💡 提示（第 %1 次使用，星级降低）：%2")
        .arg(hintUsedCount_)
        .arg(QString::fromUtf8(hint.c_str())));
}

// ============================================================
// 跳过
// ============================================================

void TokenPuzzlePanel::onSkipLevel() {
    // AUDIT-P2 fix: 防重复守卫——快速双击会触发两次关卡切换（第二次跳两关）
    if (busy_) return;
    busy_ = true;
    skipBtn_->setEnabled(false);
    // 跳过不计星（0 星），仅在未完成时标记
    if (levelStars_[currentLevelIndex_] < 0) {
        levelStars_[currentLevelIndex_] = 0;
    }
    // P0-2 fix (F7): 持久化跳过状态（0 星）
    LearnerProgressStore::instance().markLevelStars(
        levelId(currentLevelIndex_).toStdString(), 0);
    LearnerProgressStore::instance().save();
    setFeedback(mlTr("已跳过本关（不计星），下一关已解锁"));
    unlockNextLevel();
    updateScoreDisplay();
    // AUDIT-P2 fix: 跳过≠完成，不应发射 activityCompleted（否则 LearningPathPanel
    // 误标记该活动为已完成）。unlockNextLevel 已基于 levelStars_ 解锁下一关，
    // 不依赖此信号。0 星持久化由上方 markLevelStars(0) 完成。

    // 自动切换到下一关
    int next = currentLevelIndex_ + 1;
    if (next < TokenPuzzleLibrary::levelCount()) {
        levelCombo_->setCurrentIndex(next);
    }
    // AUDIT-P2 fix: 恢复按钮+标志
    busy_ = false;
    skipBtn_->setEnabled(true);
}

// ============================================================
// 重置
// ============================================================

void TokenPuzzlePanel::onResetLevel() {
    clearAnswer();
    for (auto* btn : shuffledButtons_) {
        if (btn) btn->setEnabled(true);
    }
    hintUsedCount_ = 0;
    setFeedback(mlTr("已重置，重新排列吧！"));
}

// ============================================================
// 关卡切换
// ============================================================

void TokenPuzzlePanel::onLevelChanged(int index) {
    auto* model = qobject_cast<QStandardItemModel*>(levelCombo_->model());
    if (!model || index < 0 || index >= model->rowCount()) return;

    // 禁止切换到未解锁的关卡
    if (!model->item(index)->isEnabled()) {
        QSignalBlocker blocker(levelCombo_);
        levelCombo_->setCurrentIndex(currentLevelIndex_);
        setFeedback(mlTr("该关卡尚未解锁，请先完成前一关"), true);
        return;
    }
    currentLevelIndex_ = index;
    hintUsedCount_ = 0;
    loadLevel(index);
}

// ============================================================
// 解锁下一关
// ============================================================

void TokenPuzzlePanel::unlockNextLevel() {
    int next = currentLevelIndex_ + 1;
    if (next >= TokenPuzzleLibrary::levelCount()) return;  // 已是最后一关
    auto* model = qobject_cast<QStandardItemModel*>(levelCombo_->model());
    if (model && next < model->rowCount()) {
        QStandardItem* it = model->item(next);
        if (it && !it->isEnabled()) {
            it->setEnabled(true);
        }
    }
    refreshLevelChips();  // 解锁后刷新芯片栏（locked → unlocked）
}

// ============================================================
// 得分 / 进度显示
// ============================================================

void TokenPuzzlePanel::updateScoreDisplay() {
    int total = 0;
    int completed = 0;
    for (int s : levelStars_) {
        if (s > 0) total += s;
        if (s >= 0) completed++;   // 0（跳过）或 1-3（星级）均算"已通过"
    }
    int maxStars = TokenPuzzleLibrary::levelCount() * 3;
    scoreLabel_->setText(mlTr("得分: %1 / %2").arg(total).arg(maxStars));
    progressLabel_->setText(mlTr("已完成 %1 / %2 关")
        .arg(completed).arg(TokenPuzzleLibrary::levelCount()));
}

// ============================================================
// 辅助
// ============================================================

QString TokenPuzzlePanel::levelId(int index) const {
    return QString::fromUtf8("token-puzzle-%1").arg(index + 1);
}

QString TokenPuzzlePanel::starsToText(int stars) const {
    switch (stars) {
        case 3:  return QString::fromUtf8("⭐⭐⭐");
        case 2:  return QString::fromUtf8("⭐⭐☆");
        case 1:  return QString::fromUtf8("⭐☆☆");
        case 0:  return QString::fromUtf8("☆☆☆（跳过）");
        default: return QString::fromUtf8("☆☆☆");
    }
}

int TokenPuzzlePanel::computeStars(int hintUsed) const {
    if (hintUsed <= 0) return 3;
    if (hintUsed == 1) return 2;
    return 1;
}

void TokenPuzzlePanel::setFeedback(const QString& text, bool isError) {
    feedbackLabel_->setText(text);
    if (isError) {
        feedbackLabel_->setStyleSheet("color: #c0392b; font-weight: bold; padding: 4px;");
    } else {
        feedbackLabel_->setStyleSheet("color: #2c3e50; padding: 4px;");
    }
}

// ============================================================
// 关卡芯片栏状态刷新
// ------------------------------------------------------------
// 三种状态：
//   locked   — 未解锁：灰色背景 + 🔒，不可点击
//   unlocked — 已解锁未选中：浅色背景 + 边框，显示关卡号
//   current  — 当前选中：主题色填充 + 白字，显示关卡号 + ⭐星数
// dynamic property 改变后必须 style()->polish() 才能让 QSS 重新评估
// ============================================================

void TokenPuzzlePanel::refreshLevelChips() {
    auto* model = qobject_cast<QStandardItemModel*>(levelCombo_->model());
    if (!model) return;
    const auto& levels = TokenPuzzleLibrary::levels();
    for (int i = 0; i < levelChips_.size(); ++i) {
        QPushButton* chip = levelChips_[i];
        if (!chip) continue;

        bool isLocked = false;
        if (i < model->rowCount()) {
            QStandardItem* it = model->item(i);
            if (it) isLocked = !it->isEnabled();
        }
        bool isCurrent = (i == currentLevelIndex_);

        chip->setProperty("locked", isLocked);
        chip->setProperty("current", isCurrent);
        chip->setEnabled(!isLocked);

        // 文本：locked 显示锁图标；current 且已获星显示关卡号+⭐；其余显示关卡号
        QString text;
        if (isLocked) {
            // 🔒 = U+1F512 = UTF-8: F0 9F 94 92
            text = QString::fromUtf8("\xF0\x9F\x94\x92");
        } else if (isCurrent && i < levelStars_.size() && levelStars_[i] > 0) {
            // ⭐ = U+2B50 = UTF-8: E2 AD 90
            text = QString::fromUtf8("%1 \xE2\xAD\x90").arg(i + 1);
        } else {
            text = QString::number(i + 1);
        }
        chip->setText(text);

        // 工具提示：显示关卡标题和教学点（目标）
        if (i < (int)levels.size()) {
            QString tip = mlTr("第 %1 关").arg(i + 1);
            if (!levels[i].teachingPoint.empty()) {
                tip += "\n" + QString::fromUtf8(levels[i].teachingPoint.c_str());
            }
            if (i < levelStars_.size() && levelStars_[i] > 0) {
                tip += "\n" + mlTr("已获星：%1").arg(levelStars_[i]);
            }
            chip->setToolTip(tip);
        }

        // 强制 QSS 重新评估 dynamic property 选择器
        chip->style()->unpolish(chip);
        chip->style()->polish(chip);
    }
}
