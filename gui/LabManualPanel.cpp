#include "gui/LabManualPanel.h"
#include "app/IdeController.h"

#include <QFile>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QRegularExpression> // P2 fix (长章折叠): 识别 ## 标题级别
#include <QShowEvent>
#include <QSplitter>
#include <QTextStream>
#include <QUrl>
#include <QVBoxLayout>

#include "Label.h"                 // QFluentKit（CaptionLabel）
#include "PushButton.h"            // QFluentKit（PrimaryPushButton）
#include "gui/ErrorHintEngine.h"   // P1-F12 fix: buggy:tag 链接查表
#include "gui/GuiTextUtils.h"      // monospaceFont()
#include "gui/I18n.h"              // mlTr()
#include "gui/LearnerProgress.h"   // P2-3 fix (F9): 学情画像持久化
#include "gui/MarkdownRenderer.h"  // 统一 Markdown 渲染
#include "gui/SyntaxHighlighter.h" // MiniLang 语法高亮器

LabManualPanel::LabManualPanel(QWidget* parent) : QWidget(parent) {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(2, 2, 2, 2);
    mainLayout->setSpacing(2);

    // 顶部按钮栏（紧凑化）
    auto* btnBar = new QHBoxLayout;
    btnBar->setSpacing(4);
    loadBtn_ = new PrimaryPushButton(mlTr("加载到编辑器"), this);
    runBtn_ = new QPushButton(QString::fromUtf8("▶ ") + mlTr("运行"), this);
    runBtn_->setToolTip(mlTr("将样例代码加载到主编辑器并运行"));
    statusLabel_ = new CaptionLabel(mlTr("请选择章节"), this);

    // 字号调节 SpinBox
    auto* fontLabel = new QLabel(mlTr("字号"), this);
    fontSizeSpin_ = new QSpinBox(this);
    fontSizeSpin_->setRange(8, 24);
    fontSizeSpin_->setValue(11);
    fontSizeSpin_->setSuffix(QString::fromUtf8("pt"));
    fontSizeSpin_->setFixedWidth(60);
    fontSizeSpin_->setFixedHeight(26);
    fontSizeSpin_->setToolTip(mlTr("调节正文字号"));
    connect(fontSizeSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int size) {
        if (contentBrowser_) {
            QFont f = contentBrowser_->font();
            f.setPointSize(size);
            contentBrowser_->setFont(f);
        }
    });

    // 折叠次要章节按钮
    foldBtn_ = new QPushButton(QString::fromUtf8("📂 ") + mlTr("折叠"), this);
    foldBtn_->setToolTip(mlTr("折叠次要章节，仅保留主章节"));
    foldBtn_->setCheckable(false);
    foldBtn_->setFixedHeight(26);
    connect(foldBtn_, &QPushButton::clicked, this, &LabManualPanel::onToggleFold);

    loadBtn_->setFixedHeight(28);
    runBtn_->setFixedHeight(28);

    btnBar->addWidget(loadBtn_);
    btnBar->addWidget(runBtn_);
    btnBar->addStretch();
    btnBar->addWidget(statusLabel_);
    btnBar->addSpacing(4);
    btnBar->addWidget(foldBtn_);
    btnBar->addSpacing(4);
    btnBar->addWidget(fontLabel);
    btnBar->addWidget(fontSizeSpin_);
    mainLayout->addLayout(btnBar);

    // issue 6: 章节芯片栏替代大目录列表（参考 TokenPuzzlePanel 关卡芯片）
    // 水平排列的章节芯片，紧凑一行，释放水平空间给教学内容与样例代码
    chapterChipBar_ = new QWidget(this);
    chapterChipBar_->setObjectName("labChapterChipBar");
    auto* chipLayout = new QHBoxLayout(chapterChipBar_);
    chipLayout->setContentsMargins(0, 0, 0, 0);
    chipLayout->setSpacing(6);
    chipLayout->setAlignment(Qt::AlignLeft);
    mainLayout->addWidget(chapterChipBar_);

    // issue 6: 主体改为 2 栏 splitter（教学内容 | 样例代码+练习），移除原章节目录列
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    contentBrowser_ = new QTextBrowser(this);
    contentBrowser_->setOpenExternalLinks(false);

    // 左侧：contentBrowser_（全高，阅读区）
    auto* middleColumn = new QWidget(this);
    auto* middleLayout = new QVBoxLayout(middleColumn);
    middleLayout->setContentsMargins(0, 0, 0, 0);
    middleLayout->setSpacing(2);
    middleLayout->addWidget(contentBrowser_, 1);

    // 右侧：样例代码（上）+ 练习（下）
    auto* codeContainer = new QWidget(this);
    auto* codeLayout = new QVBoxLayout(codeContainer);
    codeLayout->setContentsMargins(0, 0, 0, 0);
    codeLayout->setSpacing(2);
    auto* codeHeader = new QLabel(mlTr("📝 样例代码"), codeContainer);
    codeHeader->setStyleSheet("font-weight:bold; padding:2px;");
    codeLayout->addWidget(codeHeader);
    sampleCodeEdit_ = new QPlainTextEdit(codeContainer);
    sampleCodeEdit_->setReadOnly(true);
    sampleCodeEdit_->setFont(GuiTextUtils::monospaceFont(10));
    sampleCodeEdit_->setPlaceholderText(mlTr("选择章节后显示样例代码"));
    new SyntaxHighlighter(sampleCodeEdit_->document());
    codeLayout->addWidget(sampleCodeEdit_, 3);

    // 练习区容器（样例代码下方）
    exercisesContainer_ = new QWidget(codeContainer);
    exercisesContainer_->setObjectName(QString::fromUtf8("labExercisesContainer"));
    exercisesLayout_ = new QVBoxLayout(exercisesContainer_);
    exercisesLayout_->setContentsMargins(4, 4, 4, 4);
    exercisesLayout_->setSpacing(4);
    auto* exercisesHeader = new QLabel(QString::fromUtf8("📝 ") + mlTr("本章练习"), exercisesContainer_);
    exercisesHeader->setStyleSheet(QString::fromUtf8("font-weight:bold; padding:2px; border-bottom: 1px solid #ccc;"));
    exercisesLayout_->addWidget(exercisesHeader);
    exercisesLayout_->addStretch(1);
    codeLayout->addWidget(exercisesContainer_, 2);

    mainSplitter_ = splitter;
    splitter->addWidget(middleColumn);
    splitter->addWidget(codeContainer);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    splitter->setSizes({560, 440});
    mainLayout->addWidget(splitter, 1);

    // issue 6: 中性浅色风格 QSS（章节芯片按钮，二态：默认/当前）
    setStyleSheet(QString::fromUtf8("QPushButton#labChapterChip { background: #F5F5F5; border: 1px solid #E0E0E0; "
                                    "border-radius: 4px; font-size: 11px; padding: 2px 10px; }"
                                    "QPushButton#labChapterChip:hover { border-color: #268BD2; background: #E5F3FB; }"
                                    "QPushButton#labChapterChip[current='true'] { background: #268BD2; color: white; "
                                    "border-color: #1E6FA3; font-weight: bold; }"));

    populateChapterChips();

    connect(loadBtn_, &QPushButton::clicked, this, &LabManualPanel::onLoadSampleToEditor);
    connect(runBtn_, &QPushButton::clicked, this, &LabManualPanel::onRunSample);
    // P1-4 fix (F16): Markdown 内 `panel:xxx` 链接 → 跳转到对应面板
    connect(contentBrowser_, &QTextBrowser::anchorClicked, this, &LabManualPanel::onAnchorClicked);

    // PERF: 不在构造时选中第一章，推迟到首次 showEvent。
    // 避免为隐藏 dock 渲染第一章 Markdown（省 6 次正则编译 + HTML 转换）。
}

void LabManualPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (!firstShowDone_) {
        firstShowDone_ = true;
        // issue 6: 首次可见时选中第一章并渲染（通过芯片点击触发）
        if (currentChapterIndex_ < 0 && !chapterChips_.isEmpty()) {
            onChapterSelected(0);
        }
    }
}

void LabManualPanel::populateChapterChips() {
    // issue 6: 构建章节芯片栏（每个芯片对应 LabManualContent::chapters() 一项）
    const auto& chs = LabManualContent::chapters();
    for (int i = 0; i < (int)chs.size(); ++i) {
        const auto& ch = chs[i];
        auto* chip = new QPushButton(chapterChipBar_);
        chip->setObjectName("labChapterChip");
        chip->setFixedHeight(28);
        // 芯片文本：章节编号（紧凑）
        QString text = QString::fromUtf8("%1").arg(i + 1);
        chip->setText(text);
        chip->setToolTip(QString::fromUtf8(ch.title.c_str()));
        chip->setCursor(Qt::PointingHandCursor);
        const int chapterIdx = i;
        connect(chip, &QPushButton::clicked, this, [this, chapterIdx]() { onChapterSelected(chapterIdx); });
        if (auto* lay = qobject_cast<QHBoxLayout*>(chapterChipBar_->layout())) {
            lay->addWidget(chip);
        }
        chapterChips_.append(chip);
    }
    refreshChapterChips();
}

void LabManualPanel::refreshChapterChips() {
    // issue 6: 刷新芯片状态（current 高亮）
    for (int i = 0; i < chapterChips_.size(); ++i) {
        QPushButton* chip = chapterChips_[i];
        if (!chip)
            continue;
        chip->setProperty("current", (i == currentChapterIndex_));
        chip->style()->unpolish(chip);
        chip->style()->polish(chip);
    }
}

void LabManualPanel::onChapterSelected(int row) {
    currentChapterIndex_ = row;
    showCurrentChapter();
    refreshChapterChips();
}

void LabManualPanel::showCurrentChapter() {
    const auto& chs = LabManualContent::chapters();
    if (currentChapterIndex_ < 0 || currentChapterIndex_ >= (int)chs.size()) {
        contentBrowser_->clear();
        if (sampleCodeEdit_)
            sampleCodeEdit_->clear();
        statusLabel_->setText(mlTr("无选中章节"));
        rebuildExercises();
        return;
    }
    const auto& ch = chs[currentChapterIndex_];
    // P1-F12 fix: 在每章末尾追加「常见错误速查」动态段——从 ErrorHintEngine::errorPatterns()
    // 生成 Markdown 表，每行附 `[▶ 触发](buggy:tag)` 链接。点击后 onAnchorClicked 处理
    // buggy: 协议，发射 runSampleRequested 加载错误示例代码并运行，让学员看到真实报错
    // + ErrorHintEngine 增强提示。引擎扩模式时手册自动同步，消除双份真相。
    std::string markdownWithErrors = ch.markdown;
    markdownWithErrors += "\n\n---\n\n## 🔧 常见错误速查（ErrorHintEngine 联动）\n";
    markdownWithErrors += "下表列出 MiniLang 常见错误模式，点击「▶ 触发」可加载错误代码到编辑器并"
                          "立即运行，观察 ErrorHintEngine 生成的友好提示。引擎扩模式时此表自动同步。\n\n";
    markdownWithErrors += "| 错误模式 | 类别 | 触发示例 |\n";
    markdownWithErrors += "| --- | --- | --- |\n";
    for (const auto& p : ErrorHintEngine::errorPatterns()) {
        markdownWithErrors += "| " + p.title + " | " + p.category +
                              " | "
                              "[▶ 触发](buggy:" +
                              p.tag + ") |\n";
    }
    markdownWithErrors += "\n> 提示：触发后请查看底部输出面板的错误信息，"
                          "ErrorHintEngine 会附加教学性提示与拼写建议。\n";

    // P2 fix (长章折叠): 折叠模式下，把次要章节内容替换为简短提示，降低长章滚动负担。
    // 设计：保留次要章节标题（让 TOC 链接仍可点击），仅把标题到下一个同级或更高级
    // 标题之间的内容替换为一行 `> ▶ 此节已折叠...` 提示。展开模式原样返回。
    const QString markdownForRender = applyFolding(markdownWithErrors);

    // 使用统一 Markdown 渲染器：支持标题/粗体/列表/代码块/分割线等
    // 替代早期 5 行 html.replace 的最小转换（无法处理 **bold**、列表、代码块）
    // R52-5 fix: 传入 codeBlockBg 对齐面板中性浅色主题，避免代码块用默认浅灰
    QString html = MarkdownRenderer::markdownToHtml(markdownForRender, QStringLiteral("#F5F5F5"));

    // P2-2 fix (F3): 在 Markdown 渲染结果前插入章节锚点目录（TOC）
    // 让学习者一目了然看到本章所有节标题，点击即可跳转。
    // P1-F12 fix: 用追加错误速查段后的 markdown 生成 TOC，让「常见错误速查」标题也出现在目录中。
    // P2 fix (长章折叠): 折叠模式下 TOC 仍含次要章节标题（点击跳转后看到「已折叠」提示）
    QString tocFragment = MarkdownRenderer::buildTableOfContents(markdownForRender);
    if (!tocFragment.isEmpty()) {
        const QString bodyStart = QStringLiteral("</head><body>");
        int bodyIdx = html.indexOf(bodyStart);
        if (bodyIdx >= 0) {
            int insertPos = bodyIdx + bodyStart.length();
            html.insert(insertPos, tocFragment);
        }
    }

    contentBrowser_->setHtml(html);

    // 右侧样例代码区显示当前章节的 sampleCode（头歌风格 - 右侧编译器）
    if (sampleCodeEdit_) {
        sampleCodeEdit_->setPlainText(QString::fromUtf8(ch.sampleCode.c_str()));
    }

    statusLabel_->setText(mlTr("当前章节：%1").arg(QString::fromUtf8(ch.title.c_str())));

    // P1-1 fix (F6): 重建当前章节的练习控件
    rebuildExercises();
}

// ============================================================
// P1-1 fix (F6): 重建当前章节的练习控件
// 清空旧控件 → 按章节数据创建选择题/预期输出题 UI
// ============================================================
void LabManualPanel::rebuildExercises() {
    // 清空旧的按钮组与状态
    // AUDIT-P2 fix: 用 deleteLater 替代 delete——若在信号分发期间触发重建，
    // delete 直接销毁 QButtonGroup 会 UAF；同函数对 widget 已用 deleteLater。
    for (auto* grp : choiceGroups_) {
        grp->deleteLater();
    }
    choiceGroups_.clear();
    exercisePassed_.clear();

    // 清空 exercisesLayout_（保留 header + 末尾 stretch）
    while (exercisesLayout_->count() > 2) {
        // 从倒数第二个开始删除（保留末尾 stretch）
        QLayoutItem* item = exercisesLayout_->takeAt(exercisesLayout_->count() - 2);
        if (item->widget()) {
            item->widget()->deleteLater();
        }
        delete item;
    }

    const auto& chs = LabManualContent::chapters();
    if (currentChapterIndex_ < 0 || currentChapterIndex_ >= (int)chs.size()) {
        return;
    }
    const auto& exercises = chs[currentChapterIndex_].exercises;
    if (exercises.empty()) {
        // 无练习：显示提示
        auto* noExLabel = new QLabel(QString::fromUtf8("💡 ") + mlTr("本章暂无可机检练习"), exercisesContainer_);
        noExLabel->setStyleSheet(QString::fromUtf8("color:#6E6E6E; padding:8px;"));
        exercisesLayout_->insertWidget(exercisesLayout_->count() - 1, noExLabel);
        return;
    }

    int exerciseIdx = 0;
    for (const auto& ex : exercises) {
        auto* frame = new QFrame(exercisesContainer_);
        frame->setFrameShape(QFrame::StyledPanel);
        frame->setStyleSheet(QString::fromUtf8("QFrame { border: 1px solid #ddd; border-radius: 4px; padding: 6px; }"));
        auto* exLayout = new QVBoxLayout(frame);
        exLayout->setSpacing(4);

        // 题号 + 题干
        QString typeTag = (ex.type == LabExerciseType::CHOICE) ? mlTr("选择题") : mlTr("预期输出题");
        auto* promptLabel =
            new QLabel(QString::fromUtf8("<b>%1 %2.</b> %3")
                           .arg(typeTag, QString::number(exerciseIdx + 1), QString::fromUtf8(ex.prompt.c_str())),
                       frame);
        promptLabel->setTextFormat(Qt::RichText);
        promptLabel->setWordWrap(true);
        exLayout->addWidget(promptLabel);

        if (ex.type == LabExerciseType::CHOICE) {
            // 选择题：QRadioButton 选项 + QButtonGroup
            auto* grp = new QButtonGroup(exercisesContainer_);
            grp->setExclusive(true);
            for (int i = 0; i < (int)ex.options.size(); ++i) {
                auto* btn = new QRadioButton(QString::fromUtf8(ex.options[i].c_str()) + QString::fromUtf8("  ") +
                                                 QChar::fromLatin1('A' + i) + QString::fromUtf8("."),
                                             frame);
                btn->setProperty("optionIndex", i);
                grp->addButton(btn, i);
                exLayout->addWidget(btn);
            }
            choiceGroups_.push_back(grp);
            exercisePassed_.push_back(false);

            // 提交按钮
            auto* submitBtn = new QPushButton(QString::fromUtf8("✓ ") + mlTr("提交答案"), frame);
            // AUDIT-P1 fix: 捕获选择题内序号而非全局练习索引。
            // choiceGroups_ 按 CHOICE 出现顺序追加，onSubmitChoiceExercise 期望
            // 接收选择题内序号（0..choiceCount-1）才能与 choiceGroups_ 下标对齐。
            // 原实现捕获全局 exerciseIdx 会导致混排时下标越界/匹配错题。
            int capturedChoiceIdx = static_cast<int>(choiceGroups_.size()) - 1;
            // AUDIT-P2 fix: 设置 objectName，便于答对后查找按钮并禁用防重复提交
            submitBtn->setObjectName(QString::fromUtf8("submit_choice_%1").arg(capturedChoiceIdx));
            connect(submitBtn, &QPushButton::clicked, this,
                    [this, capturedChoiceIdx]() { onSubmitChoiceExercise(capturedChoiceIdx); });
            exLayout->addWidget(submitBtn);

            // 反馈区（初始隐藏）
            // AUDIT-P1 fix: feedbackLabel objectName 也用 choiceIdx 而非 exerciseIdx，
            // 与 onSubmitChoiceExercise 中的查找保持一致。
            auto* feedbackLabel = new QLabel(QString(), frame);
            feedbackLabel->setObjectName(QString::fromUtf8("feedback_choice_%1").arg(capturedChoiceIdx));
            feedbackLabel->setWordWrap(true);
            feedbackLabel->setVisible(false);
            exLayout->addWidget(feedbackLabel);
        } else {
            // N1 fix: EXPECTED_OUTPUT 型——同步运行样例代码并自动比对输出
            auto* codeLabel = new QLabel(QString::fromUtf8("<b>%1</b>").arg(mlTr("样例代码：")), frame);
            codeLabel->setTextFormat(Qt::RichText);
            exLayout->addWidget(codeLabel);
            auto* codeEdit = new QPlainTextEdit(frame);
            codeEdit->setPlainText(QString::fromUtf8(ex.sampleCode.c_str()));
            codeEdit->setFont(GuiTextUtils::monospaceFont(10));
            codeEdit->setReadOnly(true);
            codeEdit->setMaximumBlockCount(50);
            exLayout->addWidget(codeEdit);

            auto* expectedLabel =
                new QLabel(QString::fromUtf8("<b>%1</b> <code>%2</code>")
                               .arg(mlTr("预期输出："), QString::fromUtf8(ex.expectedOutput.c_str()).toHtmlEscaped()),
                           frame);
            expectedLabel->setTextFormat(Qt::RichText);
            exLayout->addWidget(expectedLabel);

            // N1 fix: 提交按钮（自动判分）
            auto* submitBtn = new QPushButton(QString::fromUtf8("✓ ") + mlTr("运行并验证"), frame);
            int capturedIdx = exerciseIdx;
            // AUDIT-P2 fix: 设置 objectName，便于答对后查找按钮并禁用防重复提交
            submitBtn->setObjectName(QString::fromUtf8("submit_output_%1").arg(capturedIdx));
            connect(submitBtn, &QPushButton::clicked, this,
                    [this, capturedIdx]() { onSubmitExpectedOutputExercise(capturedIdx); });
            exLayout->addWidget(submitBtn);

            // 反馈区（初始隐藏）
            auto* feedbackLabel = new QLabel(QString(), frame);
            feedbackLabel->setObjectName(QString::fromUtf8("output_feedback_%1").arg(exerciseIdx));
            feedbackLabel->setWordWrap(true);
            feedbackLabel->setVisible(false);
            exLayout->addWidget(feedbackLabel);

            exercisePassed_.push_back(false);
        }

        exercisesLayout_->insertWidget(exercisesLayout_->count() - 1, frame);
        ++exerciseIdx;
    }
}

// ============================================================
// P1-1 fix (F6): 提交选择题答案，即时判分
// ============================================================
void LabManualPanel::onSubmitChoiceExercise(int choiceIndex) {
    // AUDIT-P2 fix: 防重复提交守卫——快速双击会触发重复 save() 磁盘 I/O
    // 与 recordFailure 计数虚高（答错时每次点击 failCount++）
    if (submitting_)
        return;
    if (choiceIndex < 0 || choiceIndex >= (int)choiceGroups_.size())
        return;
    const auto& chs = LabManualContent::chapters();
    if (currentChapterIndex_ < 0 || currentChapterIndex_ >= (int)chs.size())
        return;
    const auto& exercises = chs[currentChapterIndex_].exercises;

    // AUDIT-P1 fix: choiceIndex 已是选择题内序号（与 choiceGroups_ 下标对齐），
    // 直接遍历 exercises 跳过 EXPECTED_OUTPUT 类型找到第 choiceIndex 个 CHOICE。
    int choiceCount = 0;
    int globalIdx = -1; // AUDIT-P2 fix: 记录全局练习索引用于 exercisePassed_
    const LabExercise* target = nullptr;
    for (int i = 0; i < (int)exercises.size(); ++i) {
        if (exercises[i].type == LabExerciseType::CHOICE) {
            if (choiceCount == choiceIndex) {
                target = &exercises[i];
                globalIdx = i;
                break;
            }
            ++choiceCount;
        }
    }
    if (!target)
        return;

    // AUDIT-P2 fix: 通过 early return 后才设标志，函数末尾恢复
    submitting_ = true;

    QButtonGroup* grp = choiceGroups_[choiceIndex];
    int selectedId = grp->checkedId();
    bool correct = (selectedId == target->correctIndex);

    // 找到对应的 feedback label
    // AUDIT-P1 fix: objectName 改为 feedback_choice_{choiceIdx} 与 rebuildExercises 一致。
    QLabel* feedbackLabel =
        exercisesContainer_->findChild<QLabel*>(QString::fromUtf8("feedback_choice_%1").arg(choiceIndex));
    if (feedbackLabel) {
        QString color = correct ? QString::fromUtf8("#4CAF50") : QString::fromUtf8("#F44336");
        QString icon = correct ? QString::fromUtf8("✅ ") : QString::fromUtf8("❌ ");
        QString msg = QString::fromUtf8("<span style='color:%1;'><b>%2</b></span><br>%3")
                          .arg(color, icon + (correct ? mlTr("答对了！") : mlTr("答错了，再想想")),
                               QString::fromUtf8(target->explanation.c_str()).toHtmlEscaped());
        feedbackLabel->setText(msg);
        feedbackLabel->setTextFormat(Qt::RichText);
        feedbackLabel->setVisible(true);
    }

    if (correct) {
        // AUDIT-P2 fix: exercisePassed_ 是全局练习索引（CHOICE + EXPECTED_OUTPUT
        // 均追加），用 globalIdx 而非 choiceIndex 索引。CHOICE/EXPECTED_OUTPUT
        // 混排时 choiceIndex 与全局索引不对齐，原代码会错误标记邻近的 EXPECTED_OUTPUT 题。
        exercisePassed_[globalIdx] = true;
        // P2-3 fix (F9): 记录得分到学情画像——单题正确记 100 分（按章节累计，
        // 通过 recordScore 取最大值语义保留历史最佳）。星级暂记 1，全章通过后再升 3。
        LearnerProgressStore::instance().recordScore(chs[currentChapterIndex_].id, 100, 1);
        LearnerProgressStore::instance().save();
        // AUDIT-P2 fix: 答对后禁用提交按钮，防止重复提交并视觉提示已通过
        QPushButton* submitBtn =
            exercisesContainer_->findChild<QPushButton*>(QString::fromUtf8("submit_choice_%1").arg(choiceIndex));
        if (submitBtn) {
            submitBtn->setEnabled(false);
            submitBtn->setText(QString::fromUtf8("✅ ") + mlTr("已通过"));
        }
        checkAllExercisesPassed();
    } else {
        // P2-3 fix (F9): 答错记录失败次数，用于薄弱点分析
        LearnerProgressStore::instance().recordFailure(chs[currentChapterIndex_].id);
        LearnerProgressStore::instance().save();
    }

    // AUDIT-P2 fix: 恢复守卫（答对时按钮已禁用，但守卫仍需复位以便其他题提交）
    submitting_ = false;
}

// ============================================================
// N1 fix: 提交预期输出题答案——同步运行样例代码并自动比对
// ============================================================
void LabManualPanel::onSubmitExpectedOutputExercise(int exerciseIndex) {
    // AUDIT-P2 fix: 防重复提交守卫——同步运行样例代码期间快速双击会触发
    // 重复 runStringCaptureOutput + 重复 save() 磁盘 I/O
    if (submitting_)
        return;
    const auto& chs = LabManualContent::chapters();
    if (currentChapterIndex_ < 0 || currentChapterIndex_ >= (int)chs.size())
        return;
    const auto& exercises = chs[currentChapterIndex_].exercises;
    if (exerciseIndex < 0 || exerciseIndex >= (int)exercises.size())
        return;
    const auto& ex = exercises[exerciseIndex];
    if (ex.type != LabExerciseType::EXPECTED_OUTPUT)
        return;

    // AUDIT-P2 fix: 通过 early return 后才设标志，函数末尾恢复
    submitting_ = true;

    // 运行样例代码并捕获输出
    std::string actualOutput;
    bool controllerAvailable = (controller_ != nullptr);
    if (controllerAvailable) {
        actualOutput = controller_->runStringCaptureOutput(ex.sampleCode);
    } else {
        // AUDIT-P2 fix: controller 未就绪时给出友好提示而非暴露内部错误
        // 原实现 actualOutput = "!ERROR: No controller" 会显示给学员，让学员
        // 误以为是程序输出，且会与预期输出比对失败，反馈"输出不匹配"造成困惑。
        actualOutput = "";
    }

    // Trim 两端空白后比对
    auto trim = [](std::string s) {
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
            s.pop_back();
        size_t start = 0;
        while (start < s.size() && (s[start] == '\n' || s[start] == '\r' || s[start] == ' '))
            ++start;
        return s.substr(start);
    };

    std::string trimmedActual = trim(actualOutput);
    std::string trimmedExpected = trim(ex.expectedOutput);
    bool correct = controllerAvailable && (trimmedActual == trimmedExpected);

    // 更新反馈
    QLabel* feedbackLabel =
        exercisesContainer_->findChild<QLabel*>(QString::fromUtf8("output_feedback_%1").arg(exerciseIndex));
    if (feedbackLabel) {
        QString color = correct ? QString::fromUtf8("#4CAF50") : QString::fromUtf8("#F44336");
        QString icon = correct ? QString::fromUtf8("✅ ") : QString::fromUtf8("❌ ");
        QString msg;
        if (!controllerAvailable) {
            // AUDIT-P2 fix: controller 未就绪的友好提示
            msg = QString::fromUtf8("<span style='color:%1;'><b>%2</b></span><br>%3")
                      .arg(color, icon + mlTr("运行环境未就绪"),
                           mlTr("请先在主界面运行一次程序以初始化引擎，再回来验证此题。"));
        } else if (correct) {
            msg = QString::fromUtf8("<span style='color:%1;'><b>%2</b></span><br>%3")
                      .arg(color, icon + mlTr("输出匹配！"), QString::fromUtf8(ex.explanation.c_str()).toHtmlEscaped());
        } else {
            msg = QString::fromUtf8("<span style='color:%1;'><b>%2</b></span><br>"
                                    "<b>%3</b> <code>%4</code><br>"
                                    "<b>%5</b> <code>%6</code><br>%7")
                      .arg(color, icon + mlTr("输出不匹配"), mlTr("预期："),
                           QString::fromUtf8(trimmedExpected.c_str()).toHtmlEscaped(), mlTr("实际："),
                           QString::fromUtf8(trimmedActual.c_str()).toHtmlEscaped(),
                           QString::fromUtf8(ex.explanation.c_str()).toHtmlEscaped());
        }
        feedbackLabel->setText(msg);
        feedbackLabel->setTextFormat(Qt::RichText);
        feedbackLabel->setVisible(true);
    }

    if (correct) {
        exercisePassed_[exerciseIndex] = true;
        LearnerProgressStore::instance().recordScore(chs[currentChapterIndex_].id, 100, 1);
        LearnerProgressStore::instance().save();
        // AUDIT-P2 fix: 答对后禁用提交按钮，防止重复提交并视觉提示已通过
        QPushButton* submitBtn =
            exercisesContainer_->findChild<QPushButton*>(QString::fromUtf8("submit_output_%1").arg(exerciseIndex));
        if (submitBtn) {
            submitBtn->setEnabled(false);
            submitBtn->setText(QString::fromUtf8("✅ ") + mlTr("已通过"));
        }
        checkAllExercisesPassed();
    } else if (controllerAvailable) {
        // AUDIT-P2 fix: 仅在 controller 可用时才记录失败，避免误判污染学情
        LearnerProgressStore::instance().recordFailure(chs[currentChapterIndex_].id);
        LearnerProgressStore::instance().save();
    }

    // AUDIT-P2 fix: 恢复守卫
    submitting_ = false;
}

// ============================================================
// P1-1 fix (F6): 检查所有练习是否全部通过，是则 emit exerciseCompleted
// ============================================================
void LabManualPanel::checkAllExercisesPassed() {
    bool allPassed = true;
    for (bool p : exercisePassed_) {
        if (!p) {
            allPassed = false;
            break;
        }
    }
    if (!allPassed)
        return;

    const auto& chs = LabManualContent::chapters();
    if (currentChapterIndex_ < 0 || currentChapterIndex_ >= (int)chs.size())
        return;
    const auto& ch = chs[currentChapterIndex_];
    // P2-3 fix (F9): 全章通过——升级星级为 3 星（满星），保留历史最佳
    LearnerProgressStore::instance().recordScore(ch.id, 100, 3);
    LearnerProgressStore::instance().save();
    emit exerciseCompleted(QString::fromUtf8(ch.id.c_str()));
    statusLabel_->setText(QString::fromUtf8("🎉 %1").arg(mlTr("本章练习全部通过！")));
}

void LabManualPanel::onLoadSampleToEditor() {
    const auto& chs = LabManualContent::chapters();
    if (currentChapterIndex_ < 0 || currentChapterIndex_ >= (int)chs.size()) {
        statusLabel_->setText(mlTr("请先选择章节"));
        return;
    }
    const auto& ch = chs[currentChapterIndex_];
    if (!controller_) {
        statusLabel_->setText(mlTr("未绑定控制器"));
        return;
    }
    // 通过 outputReady 信号让 Ide 主窗口加载到编辑器
    // 这里简化：直接 emit 信号由 Ide 接管
    // 实际加载由 Ide 监听并处理
    QString code = QString::fromUtf8(ch.sampleCode.c_str());
    emit loadSampleRequested(code);
    statusLabel_->setText(mlTr("已请求加载示例（请切到主编辑器查看）"));
}

void LabManualPanel::onRunSample() {
    const auto& chs = LabManualContent::chapters();
    if (currentChapterIndex_ < 0 || currentChapterIndex_ >= (int)chs.size()) {
        statusLabel_->setText(mlTr("请先选择章节"));
        return;
    }
    const auto& ch = chs[currentChapterIndex_];
    if (ch.sampleCode.empty()) {
        statusLabel_->setText(mlTr("当前章节无样例代码"));
        return;
    }
    // P0-1 fix (F5): 一键加载并运行，让学员在手册旁直接看到运行结果
    QString code = QString::fromUtf8(ch.sampleCode.c_str());
    emit runSampleRequested(code);
    statusLabel_->setText(mlTr("已运行示例，查看底部输出面板"));
}

// ============================================================
// P1-4 fix (F16): 处理 Markdown 内 panel: 协议链接
// Markdown 写法: `[编译管线可视化](panel:pipeline)`
// 渲染后 anchor 的 href 是 "panel:pipeline"，QUrl 解析 scheme="panel"
// ============================================================
void LabManualPanel::onAnchorClicked(const QUrl& url) {
    // P2-2 fix (F3): 章节锚点目录跳转——URL fragment 为 #anchor 时滚动到对应标题
    // QUrl 解析 "#xxx" 时 scheme 为空，fragment 为 "xxx"
    if (url.scheme().isEmpty()) {
        QString frag = url.fragment();
        if (!frag.isEmpty() && contentBrowser_) {
            contentBrowser_->scrollToAnchor(frag);
            return;
        }
    }

    if (url.scheme() == QString::fromUtf8("panel")) {
        QString panelId = url.path();
        if (panelId.isEmpty()) {
            panelId = url.host();
        }
        if (!panelId.isEmpty()) {
            emit jumpToPanelRequested(panelId);
        }
        return;
    }

    // P1-F12 fix: `buggy:tag` 链接——触发 ErrorHintEngine 中对应 tag 的错误示例代码。
    // 手册「常见错误」表的每一行附 `[▶ 触发](buggy:missing-semicolon)` 链接，
    // 点击后从 ErrorHintEngine::errorPatterns() 查找 tag 对应的 buggyCode，
    // 发射 runSampleRequested 加载到编辑器并立即运行，让学员看到真实报错 +
    // ErrorHintEngine 增强提示。引擎扩模式时手册自动同步，消除双份真相。
    if (url.scheme() == QString::fromUtf8("buggy")) {
        QString tag = url.path();
        if (tag.isEmpty()) {
            tag = url.host();
        }
        if (!tag.isEmpty()) {
            const auto& patterns = ErrorHintEngine::errorPatterns();
            for (const auto& p : patterns) {
                if (QString::fromStdString(p.tag) == tag) {
                    emit runSampleRequested(QString::fromStdString(p.buggyCode));
                    if (statusLabel_) {
                        statusLabel_->setText(
                            QString::fromUtf8("已触发错误示例 [%1] — %2，请查看输出面板的增强报错提示")
                                .arg(QString::fromStdString(p.tag), QString::fromStdString(p.title)));
                    }
                    return;
                }
            }
            if (statusLabel_) {
                statusLabel_->setText(QString::fromUtf8("未知错误标签: %1").arg(tag));
            }
        }
    }
    // 其他协议（http/https/file 等）忽略——contentBrowser 已 setOpenExternalLinks(false)
}

// ============================================================
// P2 fix (长章折叠): 次要章节折叠实现
// ------------------------------------------------------------
// 长章 Markdown 数千字，进阶/思考题/常见错误等次要区块仍要大量滚动。
// 折叠模式下保留次要章节标题（让 TOC 链接仍可点击），仅把标题到下一个
// 同级或更高级标题之间的内容替换为一行 `> ▶ 此节已折叠...` 提示。
// ============================================================

bool LabManualPanel::isMinorSection(const QString& headingText) {
    // 关键字表：识别"次要章节"——主章节（目标/概念/步骤/验证/概述/简介/示例/原理）
    // 始终展开，次章节（进阶/思考题/常见错误/延伸/拓展/参考/扩展/挑战/习题）可折叠
    static const QStringList kMinorKeywords = {
        QStringLiteral("进阶"),     QStringLiteral("思考题"),   QStringLiteral("延伸"), QStringLiteral("拓展"),
        QStringLiteral("扩展"),     QStringLiteral("参考"),     QStringLiteral("挑战"), QStringLiteral("习题"),
        QStringLiteral("常见错误"), QStringLiteral("常见问题"), QStringLiteral("FAQ"),  QStringLiteral("补充"),
        QStringLiteral("附录"),     QStringLiteral("深入")};
    for (const auto& kw : kMinorKeywords) {
        if (headingText.contains(kw, Qt::CaseInsensitive))
            return true;
    }
    return false;
}

QString LabManualPanel::applyFolding(const std::string& markdown) const {
    if (!foldMinorSections_) {
        // 展开模式：原样返回
        return QString::fromUtf8(markdown.c_str());
    }

    static const QRegularExpression reHeading(QStringLiteral("^(#{1,6})\\s+(.+?)\\s*$"));
    const QString md = QString::fromUtf8(markdown.c_str());
    const QStringList lines = md.split('\n');

    QString result;
    result.reserve(md.size());

    bool inCodeBlock = false;
    bool inFoldableSection = false;
    int foldableLevel = 0; // 触发折叠的标题级别（仅 ## 视为可折叠，# 一级始终展开）

    for (int i = 0; i < lines.size(); ++i) {
        const QString& line = lines[i];
        const QString trimmed = line.trimmed();

        // 跟踪围栏代码块状态（``` 或 ~~~）
        if (trimmed.startsWith(QStringLiteral("```")) || trimmed.startsWith(QStringLiteral("~~~"))) {
            // AUDIT-P2 fix: 折叠区内不切换 inCodeBlock。折叠区内容已被跳过，
            // 若切换 inCodeBlock 会导致状态泄漏到折叠区外——未闭合的代码块
            // 会让 inCodeBlock 恒为 true，后续所有行（含真正标题）被当作
            // 代码块内容跳过，文档剩余部分全部消失。
            if (inFoldableSection) {
                continue;
            }
            inCodeBlock = !inCodeBlock;
            result += line + '\n';
            continue;
        }
        if (inCodeBlock) {
            // 代码块内的 # 不是标题，原样输出（折叠区已跳过）
            if (!inFoldableSection)
                result += line + '\n';
            continue;
        }

        // 识别标题
        QRegularExpressionMatch hm = reHeading.match(line);
        if (hm.hasMatch()) {
            const int level = hm.captured(1).length();
            const QString headingText = hm.captured(2);

            // 遇到任何标题，先关闭当前折叠区
            if (inFoldableSection) {
                // 仅当新标题级别 <= foldableLevel（同级或更高级）时才结束折叠
                if (level <= foldableLevel) {
                    inFoldableSection = false;
                }
            }

            if (!inFoldableSection && level == 2 && isMinorSection(headingText)) {
                // 进入新的折叠区：保留标题行（让 TOC 链接仍可点击），
                // 紧跟一行提示，然后跳过后续内容直到同级或更高级标题
                result += line + '\n';
                result += QStringLiteral("> ▶ 此节已折叠，点击右上角「展开全部章节」按钮查看完整内容\n");
                inFoldableSection = true;
                foldableLevel = level;
                continue;
            }

            if (!inFoldableSection) {
                result += line + '\n';
            }
            // 折叠区内的标题：跳过
            continue;
        }

        // 普通行：折叠区内跳过，否则原样输出
        if (!inFoldableSection) {
            result += line + '\n';
        }
    }

    // 移除末尾多余换行（split + join 会引入）
    if (result.endsWith('\n'))
        result.chop(1);
    return result;
}

void LabManualPanel::onToggleFold() {
    foldMinorSections_ = !foldMinorSections_;
    if (foldBtn_) {
        foldBtn_->setText(foldMinorSections_ ? (QString::fromUtf8("📂 ") + mlTr("展开全部章节"))
                                             : (QString::fromUtf8("📂 ") + mlTr("折叠次要章节")));
    }
    if (statusLabel_) {
        statusLabel_->setText(foldMinorSections_ ? mlTr("已折叠次要章节（进阶/思考题/常见错误等）")
                                                 : mlTr("已展开全部章节"));
    }
    // 重新渲染当前章节（应用/取消折叠）
    showCurrentChapter();
}
