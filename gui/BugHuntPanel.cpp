#include "gui/BugHuntPanel.h"
#include "app/IdeController.h"
#include "common/BackendExecutionService.h" // ARCH-10: 后端执行服务中间层
#include "gui/GuidedTour.h"
#include "gui/I18n.h"
#include "gui/LearnerProgress.h"
#include "gui/ProgressSaveFeedback.h" // P2-UX fix: save 失败 toast 通知

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSplitter>
#include <QTextBrowser>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <chrono>
#include <sstream>

#include "Label.h"      // QFluentKit（StrongBodyLabel）
#include "PushButton.h" // QFluentKit（PrimaryPushButton）

// 注：BugHuntVariantLibrary::variants() 实现已拆分到独立文件
// gui/BugHuntVariantLibrary.cpp 中，避免测试目标编译时引入 IdeController.h
// 依赖（IdeController 依赖 app/ 下多个文件）。
// ============================================================

// 本文件实现 BugHuntPanel：编译器 Bug 狩猎教学模式的主面板，整合题库
// 选择（芯片栏 + 难度筛选）、内嵌代码编辑、运行验证（含三后端对比）、
// 递进提示/答案与变体挑战模式。

namespace {
// AUDIT-R2 P1-4 fix: verifying_/tripleVerifying_ 守卫改为 RAII——原实现在函数
// 末尾手动恢复标志与按钮，中途任何异常（执行链/持久化/UI 调用）都会使标志卡死
// 为 true、按钮永久禁用。析构自动恢复保证异常安全。
struct VerifyScopeGuard {
    bool& flag;
    QPushButton* btn;
    VerifyScopeGuard(bool& f, QPushButton* b) : flag(f), btn(b) {
        flag = true;
        if (btn)
            btn->setEnabled(false);
    }
    ~VerifyScopeGuard() {
        flag = false;
        if (btn)
            btn->setEnabled(true);
    }
    VerifyScopeGuard(const VerifyScopeGuard&) = delete;
    VerifyScopeGuard& operator=(const VerifyScopeGuard&) = delete;
};
} // namespace

BugHuntPanel::BugHuntPanel(QWidget* parent) : QWidget(parent) {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(2, 2, 2, 2);
    mainLayout->setSpacing(2);

    // 顶部按钮栏（紧凑化：按钮固定高度 28px，减少垂直占用）
    auto* btnBar = new QHBoxLayout;
    btnBar->setSpacing(4);
    runBtn_ = new PrimaryPushButton(mlTr("运行验证"), this);
    tripleVerifyBtn_ = new QPushButton(mlTr("三后端对比"), this);
    hintBtn_ = new QPushButton(mlTr("下一提示"), this);
    answerBtn_ = new QPushButton(mlTr("查看答案"), this);
    loadBtn_ = new QPushButton(mlTr("加载到主编辑器"), this);
    variantBtn_ = new QPushButton(mlTr("变体模式"), this);
    variantBtn_->setCheckable(true);
    statusLabel_ = new StrongBodyLabel(mlTr("请选择题目"), this);
    variantStatusLabel_ = new QLabel(QString::fromUtf8(""), this);
    for (auto* btn : {runBtn_, tripleVerifyBtn_, hintBtn_, answerBtn_, loadBtn_, variantBtn_})
        btn->setFixedHeight(28);
    btnBar->addWidget(runBtn_);
    btnBar->addWidget(tripleVerifyBtn_);
    btnBar->addWidget(hintBtn_);
    btnBar->addWidget(answerBtn_);
    btnBar->addWidget(loadBtn_);
    btnBar->addStretch();
    btnBar->addWidget(variantStatusLabel_);
    btnBar->addWidget(statusLabel_);
    btnBar->addWidget(variantBtn_);
    mainLayout->addLayout(btnBar);

    // 难度筛选栏（紧凑化：间距 4px，按钮固定高度 26px）
    auto* diffBar = new QHBoxLayout;
    diffBar->setSpacing(4);
    diffBar->addWidget(new QLabel(mlTr("难度："), this));
    diffAllBtn_ = new QPushButton(mlTr("全部"), this);
    diffBeginnerBtn_ = new QPushButton(QString::fromUtf8("\xf0\x9f\x9f\xa2 ") + mlTr("入门"), this);
    diffIntermediateBtn_ = new QPushButton(QString::fromUtf8("\xf0\x9f\x9f\xa1 ") + mlTr("进阶"), this);
    diffExpertBtn_ = new QPushButton(QString::fromUtf8("\xf0\x9f\x9f\xa5 ") + mlTr("专家"), this);
    for (auto* btn : {diffAllBtn_, diffBeginnerBtn_, diffIntermediateBtn_, diffExpertBtn_}) {
        btn->setCheckable(true);
        btn->setFixedHeight(26);
    }
    auto* diffGroup = new QButtonGroup(this);
    diffGroup->setExclusive(true);
    // AUDIT-R2 P1-1 fix: QButtonGroup::addButton 的 id=-1 表示"自动分配"（Qt 文档：
    // 自动分配的 id 从 -2 起递减），原代码点击「全部」时 idClicked 发出 -2 而非 -1，
    // currentDifficultyFilter_ 被设为 -2 导致所有芯片被隐藏、状态栏误报"专家级"。
    // 改用正数哨兵 3 表示「全部」，onDifficultyChanged 内翻译为内部筛选值 -1。
    diffGroup->addButton(diffAllBtn_, 3);
    diffGroup->addButton(diffBeginnerBtn_, 0);
    diffGroup->addButton(diffIntermediateBtn_, 1);
    diffGroup->addButton(diffExpertBtn_, 2);
    diffBar->addWidget(diffAllBtn_);
    diffBar->addWidget(diffBeginnerBtn_);
    diffBar->addWidget(diffIntermediateBtn_);
    diffBar->addWidget(diffExpertBtn_);
    diffBar->addStretch();
    mainLayout->addLayout(diffBar);
    // 默认选中"入门级"
    diffBeginnerBtn_->setChecked(true);
    currentDifficultyFilter_ = 0;
    connect(diffGroup, &QButtonGroup::idClicked, this, &BugHuntPanel::onDifficultyChanged);

    // issue 5: 芯片栏替代大目录列表（参考 TokenPuzzlePanel 关卡芯片）
    // 水平排列的 Bug 芯片 + 右侧搜索框，紧凑一行，释放垂直空间给描述与代码区
    auto* chipRow = new QHBoxLayout;
    chipRow->setContentsMargins(0, 0, 0, 0);
    chipRow->setSpacing(6);
    // 芯片栏容器（含 Bug 芯片 + 变体芯片，互斥显示）
    chipBar_ = new QWidget(this);
    chipBar_->setObjectName("bugHuntChipBar");
    auto* chipLayout = new QHBoxLayout(chipBar_);
    chipLayout->setContentsMargins(0, 0, 0, 0);
    chipLayout->setSpacing(6);
    chipLayout->setAlignment(Qt::AlignLeft);
    // Bug 芯片与变体芯片在 populateBugChips/populateVariantChips 中动态创建
    chipRow->addWidget(chipBar_, 1);
    // 搜索框（右侧）
    searchEdit_ = new QLineEdit(this);
    searchEdit_->setPlaceholderText(mlTr("搜索题目..."));
    searchEdit_->setClearButtonEnabled(true);
    searchEdit_->setFixedHeight(26);
    searchEdit_->setFixedWidth(180);
    chipRow->addWidget(searchEdit_);
    mainLayout->addLayout(chipRow);

    // issue 5: 主体改为 2 栏 splitter（描述 | 代码+输出），移除原题目列表与变体列表两栏
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    descBrowser_ = new QTextBrowser(this);
    auto* rightContainer = new QWidget(this);
    auto* rightLayout = new QVBoxLayout(rightContainer);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(2);

    // === 调试流程指引（问题 4） ===
    // 教学目标：引导用户走「预测 → 观察 → 修复 → 验证」四步调试闭环，
    // 而非仅展示问题代码和错误原因。每步完成后打勾，增强可操作性。
    debugStepsLabel_ = new QLabel(this);
    debugStepsLabel_->setStyleSheet("QLabel { background: #F5F5F5; border: 1px solid #E0E0E0;"
                                    "  border-radius: 4px; padding: 6px 8px; font-size: 12px; }");
    debugStepsLabel_->setWordWrap(true);
    debugStepsLabel_->setTextFormat(Qt::RichText);
    rightLayout->addWidget(debugStepsLabel_);

    // 预测输入行
    auto* predLayout = new QHBoxLayout;
    predLayout->setSpacing(4);
    predLayout->addWidget(new QLabel(mlTr("我的预测："), this));
    predictionEdit_ = new QLineEdit(this);
    predictionEdit_->setPlaceholderText(mlTr("先预测程序输出或行为，再运行验证..."));
    predictionEdit_->setFixedHeight(26);
    submitPredictionBtn_ = new QPushButton(mlTr("提交预测"), this);
    submitPredictionBtn_->setFixedHeight(26);
    predLayout->addWidget(predictionEdit_, 1);
    predLayout->addWidget(submitPredictionBtn_);
    rightLayout->addLayout(predLayout);

    codeEditor_ = new QTextEdit(this);
    outputEdit_ = new QTextEdit(this);
    outputEdit_->setReadOnly(true);
    rightLayout->addWidget(new QLabel(mlTr("代码：")), 0);
    rightLayout->addWidget(codeEditor_, 2);
    rightLayout->addWidget(new QLabel(mlTr("运行结果：")), 0);
    rightLayout->addWidget(outputEdit_, 1);

    mainSplitter_ = splitter;
    splitter->addWidget(descBrowser_);
    splitter->addWidget(rightContainer);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 4);
    splitter->setSizes({420, 580});
    mainLayout->addWidget(splitter, 1);

    // issue 5: 中性浅色风格 QSS（Bug 芯片按钮，三态：默认/当前/隐藏）
    // 动态属性 [current='true'] 在 refreshBugChips() 中通过 setProperty + polish() 触发
    setStyleSheet(QString::fromUtf8("QPushButton#bugChip { background: #F5F5F5; border: 1px solid #E0E0E0; "
                                    "border-radius: 4px; font-size: 11px; padding: 2px 8px; }"
                                    "QPushButton#bugChip:hover { border-color: #268BD2; background: #E5F3FB; }"
                                    "QPushButton#bugChip[current='true'] { background: #268BD2; color: white; "
                                    "border-color: #1E6FA3; font-weight: bold; }"
                                    "QPushButton#bugChip[diff='0'] { border-color: #859900; }"
                                    "QPushButton#bugChip[diff='1'] { border-color: #B58900; }"
                                    "QPushButton#bugChip[diff='2'] { border-color: #CB4B16; }"));

    // OPT-2 fix: 搜索框 textChanged 改用 200ms 防抖定时器聚合输入，
    // 避免每输入一个字符就 O(n) 遍历 bugChips_ 重建 haystack。
    // 对齐 LearningPathPanel 的 250ms 防抖设计（此处取 200ms 更跟手）。
    searchDebounceTimer_ = new QTimer(this);
    searchDebounceTimer_->setSingleShot(true);
    searchDebounceTimer_->setInterval(200);
    connect(searchDebounceTimer_, &QTimer::timeout, this, [this]() { applySearchFilter(lastSearchText_); });
    // textChanged 仅记录最新文本并重启定时器，真正过滤逻辑在 timeout 中执行
    connect(searchEdit_, &QLineEdit::textChanged, this, [this](const QString& t) {
        lastSearchText_ = t;
        searchDebounceTimer_->start();
    });

    populateBugChips();
    populateVariantChips();
    // 默认显示 Bug 芯片，隐藏变体芯片
    for (auto* c : variantChips_)
        c->hide();

    connect(runBtn_, &QPushButton::clicked, this, &BugHuntPanel::onRunVerify);
    connect(tripleVerifyBtn_, &QPushButton::clicked, this, &BugHuntPanel::onTripleVerify);
    connect(hintBtn_, &QPushButton::clicked, this, &BugHuntPanel::onShowHint);
    connect(answerBtn_, &QPushButton::clicked, this, &BugHuntPanel::onShowAnswer);
    connect(variantBtn_, &QPushButton::clicked, this, &BugHuntPanel::onToggleVariantMode);
    // 调试流程指引（问题 4）
    connect(submitPredictionBtn_, &QPushButton::clicked, this, &BugHuntPanel::onSubmitPrediction);
    connect(codeEditor_, &QTextEdit::textChanged, this, &BugHuntPanel::onCodeModified);
    connect(loadBtn_, &QPushButton::clicked, this, [this]() {
        if (variantMode_) {
            const auto& variants = BugHuntVariantLibrary::variants();
            // AUDIT-R2 P2-5 fix: 补上界检查，与 showCurrentVariant 双侧检查口径一致
            if (currentVariantIndex_ < 0 || currentVariantIndex_ >= (int)variants.size())
                return;
            emit loadSampleRequested(QString::fromUtf8(variants[currentVariantIndex_].sourceCode.c_str()));
            statusLabel_->setText(mlTr("已请求加载变体到主编辑器"));
            return;
        }
        const auto& items = BugHuntLibrary::items();
        // AUDIT-R2 P2-5 fix: 补上界检查
        if (currentItemIndex_ < 0 || currentItemIndex_ >= (int)items.size())
            return;
        emit loadSampleRequested(QString::fromUtf8(items[currentItemIndex_].sourceCode.c_str()));
        statusLabel_->setText(mlTr("已请求加载到主编辑器"));
    });

    // AUDIT-P1 fix: 从 LearnerProgressStore 加载已解决题目索引，解决跨会话完成判定丢失。
    // 持久化键 "bughunt-solved-" + item.id，值 1=已解决。
    // 加载后检查各难度档是否已全部解决，若是则补发 challengeSolved 确保学习路径活动完成态一致。
    {
        const auto& items = BugHuntLibrary::items();
        for (int i = 0; i < (int)items.size(); ++i) {
            std::string key = "bughunt-solved-" + items[i].id;
            if (LearnerProgressStore::instance().getLevelStars(key) >= 1) {
                solvedItemIndices_.insert(i);
            }
        }
        // 检查各难度是否已全部解决，若是则发射 challengeSolved（主窗口连接后回写学习路径）
        for (int diffInt = 0; diffInt <= 2; ++diffInt) {
            int totalInDiff = 0;
            int solvedInDiff = 0;
            for (int i = 0; i < (int)items.size(); ++i) {
                if (static_cast<int>(items[i].difficulty) == diffInt) {
                    ++totalInDiff;
                    if (solvedItemIndices_.contains(i))
                        ++solvedInDiff;
                }
            }
            if (totalInDiff > 0 && solvedInDiff == totalInDiff) {
                // AUDIT-R2 P1-2 fix: 构造函数内直接 emit 时外部 connect 尚未建立
                // （ide.cpp 在 new BugHuntPanel 返回后才 connect），信号会被直接丢弃，
                // "跨会话补发"从未生效。延迟到事件循环首轮再发射，确保信号到达学习路径。
                QTimer::singleShot(0, this, [this, diffInt]() { emit challengeSolved(diffInt); });
            }
        }
    }
}

void BugHuntPanel::populateBugChips() {
    // issue 5: 构建 Bug 芯片栏（每个芯片对应 BugHuntLibrary::items() 一项）
    // 清理旧芯片
    for (auto* c : bugChips_) {
        if (c)
            c->deleteLater();
    }
    bugChips_.clear();

    const auto& items = BugHuntLibrary::items();
    for (int i = 0; i < (int)items.size(); ++i) {
        const auto& it = items[i];
        auto* chip = new QPushButton(chipBar_);
        chip->setObjectName("bugChip");
        chip->setFixedHeight(28);
        // 芯片文本：编号 + 严重性（紧凑）
        QString text = QString::fromUtf8("%1\n%2")
                           .arg(QString::fromUtf8(it.id.c_str()))
                           .arg(QString::fromUtf8(it.severity.c_str()));
        chip->setText(text);
        chip->setProperty("diff", static_cast<int>(it.difficulty));
        // tooltip 显示完整标题与类别
        QString tip = QString::fromUtf8("%1 — %2\n类别: %3")
                          .arg(QString::fromUtf8(it.id.c_str()))
                          .arg(QString::fromUtf8(it.title.c_str()))
                          .arg(QString::fromUtf8(it.category.c_str()));
        chip->setToolTip(tip);
        chip->setCursor(Qt::PointingHandCursor);
        const int itemIdx = i;
        connect(chip, &QPushButton::clicked, this, [this, itemIdx]() { onItemSelected(itemIdx); });
        if (auto* lay = qobject_cast<QHBoxLayout*>(chipBar_->layout())) {
            lay->addWidget(chip);
        }
        bugChips_.append(chip);
    }
    refreshBugChips();
}

void BugHuntPanel::refreshBugChips() {
    // issue 5: 刷新芯片状态（current 高亮 + 难度筛选可见性）
    // R51-2 fix: 可见性同时考虑难度筛选与搜索关键字，避免两者互相覆盖
    const auto& items = BugHuntLibrary::items();
    QString loweredSearch = lastSearchText_.toLower();
    int firstVisible = -1;
    for (int i = 0; i < bugChips_.size() && i < (int)items.size(); ++i) {
        QPushButton* chip = bugChips_[i];
        if (!chip)
            continue;
        int diffId = static_cast<int>(items[i].difficulty);
        bool visibleByDiff = (currentDifficultyFilter_ == -1 || diffId == currentDifficultyFilter_);
        // R51-2 fix: 同时检查搜索关键字
        bool visibleBySearch = true;
        if (!loweredSearch.isEmpty()) {
            QString haystack = QString::fromUtf8(items[i].title.c_str()).toLower() + " " +
                               QString::fromUtf8(items[i].id.c_str()).toLower() + " " +
                               QString::fromUtf8(items[i].category.c_str()).toLower();
            visibleBySearch = haystack.contains(loweredSearch);
        }
        bool visible = visibleByDiff && visibleBySearch;
        chip->setVisible(visible);
        chip->setProperty("current", (i == currentItemIndex_));
        chip->style()->unpolish(chip);
        chip->style()->polish(chip);
        if (visible && firstVisible < 0)
            firstVisible = i;
    }
    // 若当前题目被筛选隐藏，自动选中第一个可见芯片
    if (currentItemIndex_ < 0 && firstVisible >= 0) {
        onItemSelected(firstVisible);
    } else if (currentItemIndex_ >= 0) {
        // 注意：不能用 QWidget::isVisible() 判定难度筛选可见性——
        // 构造期间父 widget 未 show()，isVisible() 恒返回 false，会导致
        // refreshBugChips → onItemSelected → refreshBugChips 无限递归栈溢出。
        // 这里用难度筛选+搜索条件判定，与上面 firstVisible 口径一致。
        bool curVisible = false;
        if (currentItemIndex_ < (int)items.size()) {
            int curDiffId = static_cast<int>(items[currentItemIndex_].difficulty);
            curVisible = (currentDifficultyFilter_ == -1 || curDiffId == currentDifficultyFilter_);
            if (curVisible && !loweredSearch.isEmpty()) {
                QString haystack = QString::fromUtf8(items[currentItemIndex_].title.c_str()).toLower() + " " +
                                   QString::fromUtf8(items[currentItemIndex_].id.c_str()).toLower() + " " +
                                   QString::fromUtf8(items[currentItemIndex_].category.c_str()).toLower();
                curVisible = haystack.contains(loweredSearch);
            }
        }
        if (!curVisible && firstVisible >= 0 && firstVisible != currentItemIndex_) {
            onItemSelected(firstVisible);
        }
    }
}

// OPT-2 fix: 从原 textChanged lambda 提取的搜索过滤逻辑，供防抖定时器调用。
// AUDIT-R2 P2-4 fix: 原实现与 refreshBugChips 双轨重复且行为分叉——搜索路径
// 只设可见性，不自动重选被过滤掉的当前题、不刷新 current 高亮。
// refreshBugChips 已读取 lastSearchText_ 并处理难度+搜索交集与自动重选，
// 直接委托即可，消除重复的 haystack 构造逻辑。
void BugHuntPanel::applySearchFilter(const QString& filter) {
    lastSearchText_ = filter;
    refreshBugChips();
}

void BugHuntPanel::populateVariantChips() {
    // issue 5: 构建变体芯片栏（每个芯片对应 BugHuntVariantLibrary::variants() 一项）
    for (auto* c : variantChips_) {
        if (c)
            c->deleteLater();
    }
    variantChips_.clear();

    const auto& variants = BugHuntVariantLibrary::variants();
    for (int i = 0; i < (int)variants.size(); ++i) {
        const auto& v = variants[i];
        auto* chip = new QPushButton(chipBar_);
        chip->setObjectName("bugChip"); // 复用 bugChip QSS
        chip->setFixedHeight(28);
        QString text = QString::fromUtf8("V%1\n%2").arg(i + 1).arg(QString::fromUtf8(v.parentId.c_str()));
        chip->setText(text);
        chip->setToolTip(QString::fromUtf8(v.title.c_str()));
        chip->setCursor(Qt::PointingHandCursor);
        const int varIdx = i;
        connect(chip, &QPushButton::clicked, this, [this, varIdx]() { onVariantSelected(varIdx); });
        if (auto* lay = qobject_cast<QHBoxLayout*>(chipBar_->layout())) {
            lay->addWidget(chip);
        }
        variantChips_.append(chip);
    }
    refreshVariantChips();
}

void BugHuntPanel::refreshVariantChips() {
    for (int i = 0; i < variantChips_.size(); ++i) {
        QPushButton* chip = variantChips_[i];
        if (!chip)
            continue;
        chip->setProperty("current", (i == currentVariantIndex_));
        chip->style()->unpolish(chip);
        chip->style()->polish(chip);
    }
}

void BugHuntPanel::onItemSelected(int itemIndex) {
    // issue 5: 芯片点击 → 直接传入 items() 索引
    if (itemIndex < 0) {
        currentItemIndex_ = -1;
        return;
    }
    // 幂等保护：相同索引不重复刷新，避免 refreshBugChips 递归调用栈溢出
    // AUDIT-P2 fix: 移除 hintLevel_==0 约束——用户已查看提示后再次点击同一题目，
    // 原保护失效导致 hintLevel_ 被重置为 0，"下一提示"又从提示 1 开始，状态混乱。
    if (currentItemIndex_ == itemIndex) {
        return;
    }
    currentItemIndex_ = itemIndex;
    hintLevel_ = 0;
    showCurrentItem();
    refreshBugChips();
}

void BugHuntPanel::showCurrentItem() {
    const auto& items = BugHuntLibrary::items();
    if (currentItemIndex_ < 0 || currentItemIndex_ >= (int)items.size()) {
        descBrowser_->clear();
        codeEditor_->clear();
        return;
    }
    const auto& it = items[currentItemIndex_];
    // 难度标签文本（功能 9）
    QString diffLabel;
    if (it.difficulty == BugHuntDifficulty::BEGINNER) {
        diffLabel = QString::fromUtf8("\xf0\x9f\x9f\xa2 ") + mlTr("入门级"); // 🟢
    } else if (it.difficulty == BugHuntDifficulty::INTERMEDIATE) {
        diffLabel = QString::fromUtf8("\xf0\x9f\x9f\xa1 ") + mlTr("进阶级"); // 🟡
    } else {
        diffLabel = QString::fromUtf8("\xf0\x9f\x9f\xa5 ") + mlTr("专家级"); // 🔴
    }
    QString html = QString("<html><body>"
                           "<h2>[%1] %2</h2>"
                           "<p><b>难度:</b> %3 &nbsp;&nbsp; <b>严重性:</b> %4</p>"
                           "<p><b>类别:</b> %5</p>"
                           "<p><b>背景:</b></p><p>%6</p>"
                           "<p><b>期望行为:</b></p><p>%7</p>"
                           "<p><b>Bug 行为:</b></p><p>%8</p>"
                           "</body></html>")
                       .arg(QString::fromUtf8(it.id.c_str()))
                       .arg(QString::fromUtf8(it.title.c_str()))
                       .arg(diffLabel)
                       .arg(QString::fromUtf8(it.severity.c_str()))
                       .arg(QString::fromUtf8(it.category.c_str()))
                       .arg(QString::fromUtf8(it.background.c_str()).toHtmlEscaped())
                       .arg(QString::fromUtf8(it.expectedBehavior.c_str()).toHtmlEscaped())
                       .arg(QString::fromUtf8(it.buggyBehavior.c_str()).toHtmlEscaped());
    descBrowser_->setHtml(html);
    codeEditor_->setPlainText(QString::fromUtf8(it.sourceCode.c_str()));
    outputEdit_->clear();
    statusLabel_->setText(mlTr("当前题目：%1").arg(QString::fromUtf8(it.id.c_str())));
    // 重置调试流程状态
    originalCode_ = QString::fromUtf8(it.sourceCode.c_str());
    stepPredicted_ = false;
    stepObserved_ = false;
    stepAttemptedFix_ = false;
    stepVerified_ = false;
    if (predictionEdit_)
        predictionEdit_->clear();
    refreshDebugSteps();
}

void BugHuntPanel::onDifficultyChanged(int id) {
    // AUDIT-R2 P1-1 fix: 按钮组 id：3 = 全部（哨兵，因 QButtonGroup 禁用 -1），
    // 0/1/2 = BEGINNER/INTERMEDIATE/EXPERT；内部筛选值仍用 -1 表示全部。
    currentDifficultyFilter_ = (id == 3) ? -1 : id;
    refreshBugChips(); // issue 5: 刷新芯片可见性
    // 状态栏给出筛选反馈
    QString label;
    if (currentDifficultyFilter_ == -1)
        label = mlTr("难度筛选：全部");
    else if (currentDifficultyFilter_ == 0)
        label = mlTr("难度筛选：入门级");
    else if (currentDifficultyFilter_ == 1)
        label = mlTr("难度筛选：进阶级");
    else
        label = mlTr("难度筛选：专家级");
    statusLabel_->setText(label);
}

void BugHuntPanel::onRunVerify() {
    // AUDIT-P2 fix: 防重复守卫——快速双击会触发两次完整 Lexer→Parser→VM 链
    if (verifying_)
        return;
    if (currentItemIndex_ < 0) {
        statusLabel_->setText(mlTr("请先选择题目"));
        return;
    }
    std::string source = codeEditor_->toPlainText().toStdString();
    if (source.empty()) {
        outputEdit_->setPlainText(mlTr("（空代码）"));
        return;
    }

    // AUDIT-R2 P1-4 fix: RAII 守卫——异常抛出时也能恢复标志与按钮状态
    VerifyScopeGuard guard(verifying_, runBtn_);

    // ARCH-10: 通过 BackendExecutionService 触发 Interpreter + StackVM 两条路径，
    // 面板不再直接依赖 Lexer/Parser/Compiler/VM/Interpreter 等内部头文件。
    std::ostringstream out;
    auto interpRes = BackendExecutionService::execute(source, BackendType::Interpreter);
    if (!interpRes.output.empty()) {
        out << interpRes.output;
    }
    if (interpRes.success) {
        out << "\n[Interpreter 路径运行完成]";
    } else {
        out << "\n[Interpreter 异常] " << interpRes.errorPrefix << ": " << interpRes.errorMsg;
    }

    // AUDIT-R2 P1-3 fix: 原实现硬编码 "[StackVM 路径: 0]"（历史遗留退出码占位）
    // 且丢弃 stackRes.output，用户无法对比两后端输出——而"对比后端行为差异"
    // 正是本面板的教学核心（如 BUG-CP-1 需观察两后端 print 值差异）。
    auto stackRes = BackendExecutionService::execute(source, BackendType::StackVM_IR);
    if (stackRes.success) {
        out << "\n\n--- StackVM 路径输出 ---\n";
        if (!stackRes.output.empty())
            out << stackRes.output;
        out << "[StackVM 路径运行完成]";
    } else {
        out << "\n[StackVM 路径: " << stackRes.errorPrefix << " - " << stackRes.errorMsg << "]";
    }
    outputEdit_->setPlainText(QString::fromUtf8(out.str().c_str()));
    statusLabel_->setText(mlTr("验证完成 — 对比期望/Bug 行为"));
    // 调试流程：标记「观察」步骤完成
    if (!stepObserved_) {
        stepObserved_ = true;
        refreshDebugSteps();
    }
}

void BugHuntPanel::onShowHint() {
    if (currentItemIndex_ < 0)
        return;
    const auto& items = BugHuntLibrary::items();
    if (hintLevel_ >= (int)items[currentItemIndex_].hints.size()) {
        statusLabel_->setText(mlTr("已无更多提示"));
        return;
    }
    QString hint = QString::fromUtf8(items[currentItemIndex_].hints[hintLevel_].c_str());
    hintLevel_++;
    QString current = outputEdit_->toPlainText();
    if (!current.isEmpty())
        current += "\n\n";
    current += mlTr("=== 提示 %1 ===\n%2").arg(hintLevel_).arg(hint);
    outputEdit_->setPlainText(current);
    statusLabel_->setText(mlTr("已显示提示 %1/%2").arg(hintLevel_).arg((int)items[currentItemIndex_].hints.size()));
}

void BugHuntPanel::onShowAnswer() {
    if (currentItemIndex_ < 0)
        return;
    const auto& items = BugHuntLibrary::items();
    QString current = outputEdit_->toPlainText();
    if (!current.isEmpty())
        current += "\n\n";
    current += mlTr("=== 答案 ===\n%1").arg(QString::fromUtf8(items[currentItemIndex_].explanation.c_str()));
    outputEdit_->setPlainText(current);
    statusLabel_->setText(mlTr("答案已显示"));
}

// ============================================================
// 调试流程指引（问题 4）：预测 → 观察 → 修复 → 验证 闭环
// ============================================================

void BugHuntPanel::refreshDebugSteps() {
    if (!debugStepsLabel_)
        return;
    // 4 步检查清单，完成打 ✅ 未完成打 ⬜
    auto check = [](bool done) -> QString {
        return done ? QString::fromUtf8("\xe2\x9c\x85") : QString::fromUtf8("\xe2\xac\x9c");
    };
    QString html = QString::fromUtf8("<b>调试流程：</b> %1 预测 → %2 观察 → %3 修复 → %4 验证"
                                     "<br><span style='color:#5A5A5A;font-size:11px;'>"
                                     "教学目标：通过「预测-观察-修复-验证」四步闭环培养系统化调试能力。"
                                     "先在下方输入框写下你对代码行为的预测，运行后对比差异，"
                                     "修改代码尝试修复，最后用「三后端对比」验证修复效果。"
                                     "</span>")
                       .arg(check(stepPredicted_))
                       .arg(check(stepObserved_))
                       .arg(check(stepAttemptedFix_))
                       .arg(check(stepVerified_));
    debugStepsLabel_->setText(html);
}

void BugHuntPanel::onSubmitPrediction() {
    if (currentItemIndex_ < 0 && !variantMode_) {
        statusLabel_->setText(mlTr("请先选择题目"));
        return;
    }
    QString pred = predictionEdit_->text().trimmed();
    if (pred.isEmpty()) {
        statusLabel_->setText(mlTr("请输入你的预测后再提交"));
        return;
    }
    stepPredicted_ = true;
    refreshDebugSteps();
    // 将预测记录到输出区
    QString current = outputEdit_->toPlainText();
    if (!current.isEmpty())
        current += "\n\n";
    current += mlTr("=== 我的预测 ===\n%1").arg(pred);
    outputEdit_->setPlainText(current);
    statusLabel_->setText(mlTr("预测已记录 — 现在点击「运行验证」观察实际行为"));
}

void BugHuntPanel::onCodeModified() {
    // 检测用户是否修改了代码（与原始代码不同）
    if (originalCode_.isEmpty())
        return;
    if (codeEditor_->toPlainText() != originalCode_) {
        if (!stepAttemptedFix_) {
            stepAttemptedFix_ = true;
            refreshDebugSteps();
        }
    }
}

// ============================================================
// 三后端对比验证 + 变体模式实现
// ============================================================

namespace {
// 单后端运行结果（供 onTripleVerify 使用）
struct BackendRunResult {
    std::string output;        // 标准输出
    std::string exceptionName; // 异常名（空 = 无异常）
    long long micros = 0;      // 耗时（微秒）
};

// ARCH-10: 通过 BackendExecutionService 触发三后端，面板不再直接依赖
// Lexer/Parser/Compiler/VM/RegisterVM/Interpreter 等内部头文件。
// 服务返回的 BackendExecResult 已包含 output/success/errorPrefix/errorMsg/elapsedMs，
// 这里转换为面板内部使用的 BackendRunResult 结构。

// Interpreter 路径：树遍历解释器
BackendRunResult runInterpreter(const std::string& source) {
    BackendRunResult r;
    auto sr = BackendExecutionService::execute(source, BackendType::Interpreter);
    r.output = sr.output;
    r.micros = sr.elapsedUs; // AUDIT-R2 P2-2 fix: 直接使用微秒精度字段，不再 ms×1000 冒充 μs
    if (!sr.success) {
        r.exceptionName = sr.errorPrefix + ": " + sr.errorMsg;
    }
    return r;
}

// StackVM 路径：栈式字节码虚拟机
BackendRunResult runStackVM(const std::string& source) {
    BackendRunResult r;
    auto sr = BackendExecutionService::execute(source, BackendType::StackVM_IR);
    r.output = sr.output;
    r.micros = sr.elapsedUs; // AUDIT-R2 P2-2 fix: 微秒精度
    if (!sr.success) {
        r.exceptionName = sr.errorPrefix + ": " + sr.errorMsg;
    }
    return r;
}

// RegisterVM 路径：寄存器式虚拟机
BackendRunResult runRegisterVM(const std::string& source) {
    BackendRunResult r;
    auto sr = BackendExecutionService::execute(source, BackendType::RegisterVM_IR);
    r.output = sr.output;
    r.micros = sr.elapsedUs; // AUDIT-R2 P2-2 fix: 微秒精度
    if (!sr.success) {
        r.exceptionName = sr.errorPrefix + ": " + sr.errorMsg;
    }
    return r;
}
} // namespace

void BugHuntPanel::onTripleVerify() {
    // AUDIT-P2 fix: 防重复守卫——三后端验证执行 3 条串行链，快速双击触发 6 次执行
    if (tripleVerifying_)
        return;
    // 变体模式下验证当前变体；标准模式下验证当前题目
    if (variantMode_) {
        if (currentVariantIndex_ < 0) {
            statusLabel_->setText(mlTr("请先选择变体"));
            return;
        }
    } else {
        if (currentItemIndex_ < 0) {
            statusLabel_->setText(mlTr("请先选择题目"));
            return;
        }
    }

    std::string source = codeEditor_->toPlainText().toStdString();
    if (source.empty()) {
        outputEdit_->setPlainText(mlTr("（空代码）"));
        return;
    }

    // AUDIT-R2 P1-4 fix: RAII 守卫——异常抛出时也能恢复标志与按钮状态
    VerifyScopeGuard guard(tripleVerifying_, tripleVerifyBtn_);

    // 三条路径独立执行
    auto ir = runInterpreter(source);
    auto sv = runStackVM(source);
    auto rv = runRegisterVM(source);

    // 一致性判定
    bool outputConsistent = (ir.output == sv.output) && (sv.output == rv.output);
    // AUDIT-R2 P2-1 fix: 原判定要求三后端均有异常才可能"一致"，导致最常见的
    // 成功场景（三后端均无异常）被误报"异常行为 [不一致]"。正确语义：
    // 三后端异常名完全相同（含"均为空"）即一致。
    bool exceptionConsistent = (ir.exceptionName == sv.exceptionName) && (sv.exceptionName == rv.exceptionName);

    std::ostringstream out;
    out << "============= 三后端对比验证 =============\n";
    out << "[Interpreter] 输出: " << (ir.output.empty() ? "（无）" : ir.output);
    out << "              异常: " << (ir.exceptionName.empty() ? "无" : ir.exceptionName) << "\n";
    out << "              耗时: " << ir.micros << " μs\n\n";
    out << "[StackVM]     输出: " << (sv.output.empty() ? "（无）" : sv.output);
    out << "              异常: " << (sv.exceptionName.empty() ? "无" : sv.exceptionName) << "\n";
    out << "              耗时: " << sv.micros << " μs\n\n";
    out << "[RegisterVM]  输出: " << (rv.output.empty() ? "（无）" : rv.output);
    out << "              异常: " << (rv.exceptionName.empty() ? "无" : rv.exceptionName) << "\n";
    out << "              耗时: " << rv.micros << " μs\n\n";
    out << "============= 一致性结论 =============\n";
    out << "三后端输出 [" << (outputConsistent ? "一致" : "不一致") << "]\n";
    out << "异常行为 [" << (exceptionConsistent ? "一致" : "不一致") << "]\n";

    // 教学注解
    out << "教学注解: ";
    if (outputConsistent && ir.exceptionName.empty() && sv.exceptionName.empty() && rv.exceptionName.empty()) {
        out << "三后端输出完全一致且无异常——该代码路径在三套引擎上语义等价。";
    } else if (outputConsistent && exceptionConsistent) {
        out << "三后端均抛出相同异常且输出一致——异常路径语义等价，关注异常本身是否为预期行为。";
    } else if (!outputConsistent) {
        out << "三后端输出不一致——存在语义差异，需逐行对比 Interpreter/StackVM/RegisterVM 实现。";
        if (!ir.exceptionName.empty() && sv.exceptionName.empty() && rv.exceptionName.empty()) {
            out << "（Interpreter 抛异常但 VM 路径未抛——可能 Interpreter 更严格的检查）";
        } else if (ir.exceptionName.empty() && (!sv.exceptionName.empty() || !rv.exceptionName.empty())) {
            out << "（VM 路径抛异常但 Interpreter 未抛——可能 VM 编译/执行有缺陷）";
        } else if (!sv.exceptionName.empty() && rv.exceptionName.empty() && ir.exceptionName.empty()) {
            out << "（仅 StackVM 抛异常——栈式 VM 路径存在 bug）";
        } else if (!rv.exceptionName.empty() && sv.exceptionName.empty() && ir.exceptionName.empty()) {
            out << "（仅 RegisterVM 抛异常——寄存器式 VM 路径存在 bug）";
        }
    } else if (!exceptionConsistent) {
        out << "异常行为不一致——三后端对异常的处理存在差异（异常类型/消息/是否抛出）。";
    }
    out << "\n";

    outputEdit_->setPlainText(QString::fromUtf8(out.str().c_str()));
    statusLabel_->setText(mlTr("三后端对比验证完成"));
    // 调试流程：标记「验证」步骤完成
    if (!stepVerified_) {
        stepVerified_ = true;
        refreshDebugSteps();
    }
    if (variantMode_) {
        variantStatusLabel_->setText(
            mlTr("变体验证完成 — 输出%1").arg(outputConsistent ? mlTr("一致") : mlTr("不一致")));
    }

    // P0-B fix: 三后端输出一致且均无异常 → 视为"狩猎"成功，发射 challengeSolved。
    // 学员修复 Bug 后三后端应产生一致输出且无异常——这与原 Bug 代码三后端不一致
    // 或抛异常的状态形成对照，构成判分依据。仅标准模式（非变体）发射，因变体本身
    // 是探索性挑战而非可机检修复。
    if (!variantMode_ && currentItemIndex_ >= 0) {
        const auto& items = BugHuntLibrary::items();
        if (currentItemIndex_ < (int)items.size()) {
            bool allClean = ir.exceptionName.empty() && sv.exceptionName.empty() && rv.exceptionName.empty();
            // AUDIT-P2 fix: 排除"三后端均无输出"的退化解——学员删除所有 print
            // 即可让 outputConsistent 平凡为 true，从而作弊通关。要求至少一个
            // 后端有非空输出，确保学员确实修复了 Bug 并产生预期输出。
            bool hasOutput = !ir.output.empty() || !sv.output.empty() || !rv.output.empty();
            if (outputConsistent && allClean && hasOutput) {
                int diffInt = static_cast<int>(items[currentItemIndex_].difficulty);
                // AUDIT-P1 fix: 跟踪已解决题目，仅当当前难度所有题目全部解决时
                // 才发射 challengeSolved。原实现单题通过即发射，与游戏面板
                // "全部子关卡完成"语义不一致（TokenPuzzle 要求 5 关全通、
                // AstToy 要求 6 关全通）。BEGINNER 5 道、INTERMEDIATE 4 道、EXPERT 6 道。
                solvedItemIndices_.insert(currentItemIndex_);
                // AUDIT-P1 fix: 持久化已解决题目到 LearnerProgressStore，解决跨会话完成判定丢失。
                std::string solvedKey = "bughunt-solved-" + items[currentItemIndex_].id;
                LearnerProgressStore::instance().markLevelStars(solvedKey, 1);
                saveLearnerProgressWithFeedback(this); // P2-UX fix: 失败时弹 toast 避免静默丢失
                int totalInDiff = 0;
                int solvedInDiff = 0;
                for (int i = 0; i < (int)items.size(); ++i) {
                    if (static_cast<int>(items[i].difficulty) == diffInt) {
                        ++totalInDiff;
                        if (solvedItemIndices_.contains(i))
                            ++solvedInDiff;
                    }
                }
                if (solvedInDiff == totalInDiff) {
                    emit challengeSolved(diffInt);
                    out << "\n✅ 狩猎成功！本档全部 " << totalInDiff << " 道题已解决，活动标记完成。\n";
                } else {
                    out << "\n✅ 本题狩猎成功！本档已解决 " << solvedInDiff << "/" << totalInDiff << " 道。\n";
                }
                // 重新刷新输出（追加了成功提示）
                outputEdit_->setPlainText(QString::fromUtf8(out.str().c_str()));
            }
        }
    }
    // AUDIT-R2 P1-4 fix: 标志与按钮由 VerifyScopeGuard 析构自动恢复
}

void BugHuntPanel::onToggleVariantMode() {
    variantMode_ = variantBtn_->isChecked();
    if (variantMode_) {
        // issue 5: 切换到变体模式 — 隐藏 Bug 芯片与难度筛选，显示变体芯片
        for (auto* c : bugChips_)
            if (c)
                c->hide();
        for (auto* c : variantChips_)
            if (c)
                c->show();
        hintBtn_->hide();
        answerBtn_->hide();
        diffAllBtn_->hide();
        diffBeginnerBtn_->hide();
        diffIntermediateBtn_->hide();
        diffExpertBtn_->hide();
        variantBtn_->setText(mlTr("返回标准"));
        variantStatusLabel_->setText(mlTr("变体模式 — 选择变体后点击三后端对比"));
        if (variantChips_.size() > 0 && currentVariantIndex_ < 0) {
            onVariantSelected(0);
        } else if (currentVariantIndex_ >= 0) {
            showCurrentVariant();
            refreshVariantChips();
        }
    } else {
        // issue 5: 切换回标准模式 — 隐藏变体芯片，恢复 Bug 芯片与难度筛选
        for (auto* c : variantChips_)
            if (c)
                c->hide();
        refreshBugChips(); // 恢复 Bug 芯片可见性（受难度筛选控制）
        hintBtn_->show();
        answerBtn_->show();
        diffAllBtn_->show();
        diffBeginnerBtn_->show();
        diffIntermediateBtn_->show();
        diffExpertBtn_->show();
        variantBtn_->setText(mlTr("变体模式"));
        variantStatusLabel_->clear();
        if (currentItemIndex_ >= 0) {
            showCurrentItem();
        }
    }
    statusLabel_->setText(variantMode_ ? mlTr("变体模式") : mlTr("标准模式"));
}

void BugHuntPanel::onVariantSelected(int variantIndex) {
    // issue 5: 芯片点击 → 直接传入 variants() 索引
    // AUDIT-R2 P2-5 fix: 拒绝越界索引，避免 currentVariantIndex_ 残留非法值
    // 后被其他路径（如 loadBtn_）直接用于下标访问
    if (variantIndex < 0 || variantIndex >= (int)BugHuntVariantLibrary::variants().size())
        return;
    currentVariantIndex_ = variantIndex;
    showCurrentVariant();
    refreshVariantChips();
}

void BugHuntPanel::showCurrentVariant() {
    const auto& variants = BugHuntVariantLibrary::variants();
    if (currentVariantIndex_ < 0 || currentVariantIndex_ >= (int)variants.size()) {
        descBrowser_->clear();
        codeEditor_->clear();
        return;
    }
    const auto& v = variants[currentVariantIndex_];
    QString html = QString("<html><body>"
                           "<h2>%1</h2>"
                           "<p><b>父题:</b> %2</p>"
                           "<p><b>挑战目标:</b></p><p>%3</p>"
                           "<p><b>说明:</b></p><p>%4</p>"
                           "<p><b>期望行为:</b></p><p>%5</p>"
                           "<p><b>提示:</b></p><p>%6</p>"
                           "</body></html>")
                       .arg(QString::fromUtf8(v.title.c_str()))
                       .arg(QString::fromUtf8(v.parentId.c_str()))
                       .arg(QString::fromUtf8(v.challengeGoal.c_str()).toHtmlEscaped())
                       .arg(QString::fromUtf8(v.description.c_str()).toHtmlEscaped())
                       .arg(QString::fromUtf8(v.expectedBehavior.c_str()).toHtmlEscaped())
                       .arg(QString::fromUtf8(v.hint.c_str()).toHtmlEscaped());
    descBrowser_->setHtml(html);
    codeEditor_->setPlainText(QString::fromUtf8(v.sourceCode.c_str()));
    outputEdit_->clear();
    variantStatusLabel_->setText(mlTr("当前变体：%1").arg(QString::fromUtf8(v.id.c_str())));
    // 重置调试流程状态
    originalCode_ = QString::fromUtf8(v.sourceCode.c_str());
    stepPredicted_ = false;
    stepObserved_ = false;
    stepAttemptedFix_ = false;
    stepVerified_ = false;
    if (predictionEdit_)
        predictionEdit_->clear();
    refreshDebugSteps();
}

// ============================================================
// createGuidedTour — 新手引导（5 步）
// ============================================================

GuidedTour* BugHuntPanel::createGuidedTour(QWidget* host) {
    auto* tour = new GuidedTour(host, host);
    tour->addStep(
        chipBar_, QString::fromUtf8("题目芯片栏"),
        QString::fromUtf8(
            "这里水平排列所有 Bug "
            "题目芯片，点击编号即可选择题目。可搭配右侧搜索框按关键字筛选。芯片边框颜色对应难度（绿/黄/红）。"));
    tour->addStep(diffBeginnerBtn_, QString::fromUtf8("难度筛选"),
                  QString::fromUtf8("入门 / 进阶 / 专家三档，建议从「入门」开始，逐步挑战更高难度。"));
    tour->addStep(codeEditor_, QString::fromUtf8("代码编辑器"),
                  QString::fromUtf8("这里展示有 Bug 的代码，可直接修改后点击「运行验证」查看输出。"));
    tour->addStep(tripleVerifyBtn_, QString::fromUtf8("三后端对比"),
                  QString::fromUtf8(
                      "同时运行 Interpreter / StackVM / RegisterVM 三条路径，把输出并排对比，快速定位不一致的 Bug。"));
    tour->addStep(hintBtn_, QString::fromUtf8("递进提示"),
                  QString::fromUtf8("卡住时点击获取逐步提示，多次点击可查看更多线索，最后可查看完整答案与根因分析。"));
    return tour;
}
