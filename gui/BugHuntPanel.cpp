#include "gui/BugHuntPanel.h"
#include "gui/I18n.h"
#include "app/IdeController.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "interpreter/Interpreter.h"
#include "compiler/Compiler.h"
#include "compiler/VM.h"
#include "compiler/RegisterVM.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QListWidget>
#include <QTextBrowser>
#include <QTextEdit>
#include <QPushButton>
#include <QLabel>
#include <QButtonGroup>
#include <sstream>
#include <chrono>

#include "PushButton.h"   // QFluentKit（PrimaryPushButton）
#include "Label.h"        // QFluentKit（StrongBodyLabel）

// 注：BugHuntVariantLibrary::variants() 实现已拆分到独立文件
// gui/BugHuntVariantLibrary.cpp 中，避免测试目标编译时引入 IdeController.h
// 依赖（IdeController 依赖 app/ 下多个文件）。
// ============================================================

BugHuntPanel::BugHuntPanel(QWidget* parent)
    : QWidget(parent) {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(4);

    // 顶部按钮栏
    auto* btnBar = new QHBoxLayout;
    runBtn_ = new PrimaryPushButton(mlTr("运行验证"), this);
    tripleVerifyBtn_ = new QPushButton(mlTr("三后端对比"), this);
    hintBtn_ = new QPushButton(mlTr("下一提示"), this);
    answerBtn_ = new QPushButton(mlTr("查看答案"), this);
    loadBtn_ = new QPushButton(mlTr("加载到主编辑器"), this);
    variantBtn_ = new QPushButton(mlTr("变体模式"), this);
    variantBtn_->setCheckable(true);
    statusLabel_ = new StrongBodyLabel(mlTr("请选择题目"), this);
    variantStatusLabel_ = new QLabel(QString::fromUtf8(""), this);
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

    // 难度筛选栏（功能 9）：4 个互斥按钮 — 全部 / 🟢入门 / 🟡进阶 / 🔴专家
    auto* diffBar = new QHBoxLayout;
    diffBar->addWidget(new QLabel(mlTr("难度筛选："), this));
    diffAllBtn_          = new QPushButton(mlTr("全部"), this);
    diffBeginnerBtn_     = new QPushButton(QString::fromUtf8("\xf0\x9f\x9f\xa2 ") + mlTr("入门级"), this);  // 🟢
    diffIntermediateBtn_ = new QPushButton(QString::fromUtf8("\xf0\x9f\x9f\xa1 ") + mlTr("进阶级"), this);  // 🟡
    diffExpertBtn_       = new QPushButton(QString::fromUtf8("\xf0\x9f\x9f\xa5 ") + mlTr("专家级"), this);  // 🔴
    diffAllBtn_->setCheckable(true);
    diffBeginnerBtn_->setCheckable(true);
    diffIntermediateBtn_->setCheckable(true);
    diffExpertBtn_->setCheckable(true);
    // 用 QButtonGroup 实现互斥切换；id 与 currentDifficultyFilter_ 对应：
    //   -1 = 全部，0 = BEGINNER，1 = INTERMEDIATE，2 = EXPERT
    auto* diffGroup = new QButtonGroup(this);
    diffGroup->setExclusive(true);
    diffGroup->addButton(diffAllBtn_,          -1);
    diffGroup->addButton(diffBeginnerBtn_,      0);
    diffGroup->addButton(diffIntermediateBtn_,  1);
    diffGroup->addButton(diffExpertBtn_,        2);
    diffBar->addWidget(diffAllBtn_);
    diffBar->addWidget(diffBeginnerBtn_);
    diffBar->addWidget(diffIntermediateBtn_);
    diffBar->addWidget(diffExpertBtn_);
    diffBar->addStretch();
    mainLayout->addLayout(diffBar);
    // 默认选中"入门级"（功能 9 要求：默认显示入门级）
    diffBeginnerBtn_->setChecked(true);
    currentDifficultyFilter_ = 0;
    connect(diffGroup, &QButtonGroup::idClicked,
            this, &BugHuntPanel::onDifficultyChanged);

    // 主体：三栏
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    itemList_ = new QListWidget(this);
    variantList_ = new QListWidget(this);  // 变体列表（默认隐藏）
    variantList_->hide();
    descBrowser_ = new QTextBrowser(this);
    auto* rightContainer = new QWidget(this);
    auto* rightLayout = new QVBoxLayout(rightContainer);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    codeEditor_ = new QTextEdit(this);
    outputEdit_ = new QTextEdit(this);
    outputEdit_->setReadOnly(true);
    rightLayout->addWidget(new QLabel(mlTr("代码：")), 0);
    rightLayout->addWidget(codeEditor_, 2);
    rightLayout->addWidget(new QLabel(mlTr("运行结果：")), 0);
    rightLayout->addWidget(outputEdit_, 1);

    splitter->addWidget(itemList_);
    splitter->addWidget(variantList_);
    splitter->addWidget(descBrowser_);
    splitter->addWidget(rightContainer);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    splitter->setStretchFactor(2, 2);
    splitter->setStretchFactor(3, 3);
    splitter->setSizes({160, 160, 300, 400});
    mainLayout->addWidget(splitter, 1);

    populateItemList();
    // 填充变体列表
    const auto& variants = BugHuntVariantLibrary::variants();
    for (const auto& v : variants) {
        QString text = QString::fromUtf8("[%1] %2")
            .arg(QString::fromUtf8(v.parentId.c_str()))
            .arg(QString::fromUtf8(v.title.c_str()));
        variantList_->addItem(text);
    }

    connect(itemList_, &QListWidget::currentRowChanged,
            this, &BugHuntPanel::onItemSelected);
    connect(variantList_, &QListWidget::currentRowChanged,
            this, &BugHuntPanel::onVariantSelected);
    connect(runBtn_, &QPushButton::clicked,
            this, &BugHuntPanel::onRunVerify);
    connect(tripleVerifyBtn_, &QPushButton::clicked,
            this, &BugHuntPanel::onTripleVerify);
    connect(hintBtn_, &QPushButton::clicked,
            this, &BugHuntPanel::onShowHint);
    connect(answerBtn_, &QPushButton::clicked,
            this, &BugHuntPanel::onShowAnswer);
    connect(variantBtn_, &QPushButton::clicked,
            this, &BugHuntPanel::onToggleVariantMode);
    connect(loadBtn_, &QPushButton::clicked, this, [this]() {
        if (variantMode_) {
            if (currentVariantIndex_ < 0) return;
            const auto& variants = BugHuntVariantLibrary::variants();
            emit loadSampleRequested(QString::fromUtf8(variants[currentVariantIndex_].sourceCode.c_str()));
            statusLabel_->setText(mlTr("已请求加载变体到主编辑器"));
            return;
        }
        if (currentItemIndex_ < 0) return;
        const auto& items = BugHuntLibrary::items();
        emit loadSampleRequested(QString::fromUtf8(items[currentItemIndex_].sourceCode.c_str()));
        statusLabel_->setText(mlTr("已请求加载到主编辑器"));
    });
}

void BugHuntPanel::populateItemList() {
    itemList_->clear();
    const auto& items = BugHuntLibrary::items();
    // 根据当前难度筛选构建列表，并用 Qt::UserRole 记录实际 items() 索引
    // currentDifficultyFilter_: -1 = 全部，0 = BEGINNER，1 = INTERMEDIATE，2 = EXPERT
    int firstVisibleRow = -1;
    for (int i = 0; i < (int)items.size(); ++i) {
        const auto& it = items[i];
        int diffId = static_cast<int>(it.difficulty);
        if (currentDifficultyFilter_ != -1 && diffId != currentDifficultyFilter_) {
            continue;
        }
        // 难度前缀标识：🟢 入门 / 🟡 进阶 / 🔴 专家
        QString diffMark;
        if (diffId == 0)      diffMark = QString::fromUtf8("\xf0\x9f\x9f\xa2");  // 🟢
        else if (diffId == 1) diffMark = QString::fromUtf8("\xf0\x9f\x9f\xa1");  // 🟡
        else                  diffMark = QString::fromUtf8("\xf0\x9f\x9f\xa5");  // 🔴
        QString text = QString::fromUtf8("%1 [%2] %3 — %4")
            .arg(diffMark)
            .arg(QString::fromUtf8(it.severity.c_str()))
            .arg(QString::fromUtf8(it.id.c_str()))
            .arg(QString::fromUtf8(it.title.c_str()));
        auto* listItem = new QListWidgetItem(text, itemList_);
        listItem->setData(Qt::UserRole, i);  // 记录实际 items() 索引
        if (firstVisibleRow < 0) firstVisibleRow = itemList_->count() - 1;
    }
    if (firstVisibleRow >= 0) {
        itemList_->setCurrentRow(firstVisibleRow);
    } else {
        currentItemIndex_ = -1;
        descBrowser_->clear();
        codeEditor_->clear();
    }
}

void BugHuntPanel::onItemSelected(int row) {
    if (row < 0 || row >= itemList_->count()) {
        currentItemIndex_ = -1;
        return;
    }
    // 从 Qt::UserRole 取出实际 items() 索引（避免被难度筛选打乱）
    QVariant data = itemList_->item(row)->data(Qt::UserRole);
    currentItemIndex_ = data.toInt();
    hintLevel_ = 0;
    showCurrentItem();
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
        diffLabel = QString::fromUtf8("\xf0\x9f\x9f\xa2 ") + mlTr("入门级");  // 🟢
    } else if (it.difficulty == BugHuntDifficulty::INTERMEDIATE) {
        diffLabel = QString::fromUtf8("\xf0\x9f\x9f\xa1 ") + mlTr("进阶级");  // 🟡
    } else {
        diffLabel = QString::fromUtf8("\xf0\x9f\x9f\xa5 ") + mlTr("专家级");  // 🔴
    }
    QString html = QString(
        "<html><body>"
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
    statusLabel_->setText(mlTr("当前题目：%1").arg(
        QString::fromUtf8(it.id.c_str())));
}

void BugHuntPanel::onDifficultyChanged(int id) {
    // id: -1 = 全部，0 = BEGINNER，1 = INTERMEDIATE，2 = EXPERT
    currentDifficultyFilter_ = id;
    populateItemList();
    // 状态栏给出筛选反馈
    QString label;
    if (id == -1)      label = mlTr("难度筛选：全部");
    else if (id == 0)  label = mlTr("难度筛选：入门级");
    else if (id == 1)  label = mlTr("难度筛选：进阶级");
    else               label = mlTr("难度筛选：专家级");
    statusLabel_->setText(label);
}

void BugHuntPanel::onRunVerify() {
    if (currentItemIndex_ < 0) {
        statusLabel_->setText(QString::fromUtf8("请先选择题目"));
        return;
    }
    std::string source = codeEditor_->toPlainText().toStdString();
    if (source.empty()) {
        outputEdit_->setPlainText(QString::fromUtf8("（空代码）"));
        return;
    }

    std::ostringstream out;
    // 运行 Interpreter 路径（最宽容，便于教学观察行为）
    try {
        Lexer lex;
        auto tokens = lex.scan(source);
        Parser parser;
        auto ast = parser.parse(tokens);
        if (parser.hasErrors()) {
            const auto& diags = parser.getDiagnostics();
            out << "[Parser 错误]\n" << diags.summary() << "\n";
            outputEdit_->setPlainText(QString::fromUtf8(out.str().c_str()));
            return;
        }
        Interpreter interp;
        interp.setOutputCallback([&out](const std::string& s) {
            out << s << "\n";
        });
        interp.execute(*ast);
        out << "\n[Interpreter 路径运行完成]";
    } catch (const std::exception& e) {
        out << "\n[Interpreter 异常] " << e.what();
    }

    // 也可选运行 StackVM 路径观察差异
    try {
        Lexer lex;
        auto tokens = lex.scan(source);
        Parser parser;
        auto ast = parser.parse(tokens);
        if (!parser.hasErrors()) {
            Compiler compiler;
            auto result = compiler.compile(*ast);
            if (!compiler.getDiagnostics().hasErrors()) {
                VM vm;
                vm.setOutputCallback([&out](const std::string& s) {
                    out << s << "\n";
                });
                auto vmres = vm.execute(result);
                out << "\n[StackVM 路径: " << (int)vmres << "]";
            }
        }
    } catch (const std::exception& e) {
        out << "\n[StackVM 异常] " << e.what();
    }
    outputEdit_->setPlainText(QString::fromUtf8(out.str().c_str()));
    statusLabel_->setText(QString::fromUtf8("验证完成 — 对比期望/Bug 行为"));
}

void BugHuntPanel::onShowHint() {
    if (currentItemIndex_ < 0) return;
    const auto& items = BugHuntLibrary::items();
    if (hintLevel_ >= (int)items[currentItemIndex_].hints.size()) {
        statusLabel_->setText(QString::fromUtf8("已无更多提示"));
        return;
    }
    QString hint = QString::fromUtf8(items[currentItemIndex_].hints[hintLevel_].c_str());
    hintLevel_++;
    QString current = outputEdit_->toPlainText();
    if (!current.isEmpty()) current += "\n\n";
    current += QString::fromUtf8("=== 提示 %1 ===\n%2").arg(hintLevel_).arg(hint);
    outputEdit_->setPlainText(current);
    statusLabel_->setText(QString::fromUtf8("已显示提示 %1/%2").arg(hintLevel_).arg(
        (int)items[currentItemIndex_].hints.size()));
}

void BugHuntPanel::onShowAnswer() {
    if (currentItemIndex_ < 0) return;
    const auto& items = BugHuntLibrary::items();
    QString current = outputEdit_->toPlainText();
    if (!current.isEmpty()) current += "\n\n";
    current += QString::fromUtf8("=== 答案 ===\n%1").arg(
        QString::fromUtf8(items[currentItemIndex_].explanation.c_str()));
    outputEdit_->setPlainText(current);
    statusLabel_->setText(QString::fromUtf8("答案已显示"));
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

// Interpreter 路径：树遍历解释器
BackendRunResult runInterpreter(const std::string& source) {
    BackendRunResult r;
    auto t0 = std::chrono::high_resolution_clock::now();
    try {
        Lexer lex;
        auto tokens = lex.scan(source);
        Parser parser;
        auto ast = parser.parse(tokens);
        if (parser.hasErrors()) {
            r.exceptionName = "ParseError: " + parser.getDiagnostics().summary();
        } else {
            Interpreter interp;
            interp.setOutputCallback([&r](const std::string& s) {
                r.output += s;
                r.output += "\n";
            });
            interp.execute(*ast);
        }
    } catch (const std::exception& e) {
        r.exceptionName = e.what();
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    r.micros = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    return r;
}

// StackVM 路径：栈式字节码虚拟机
BackendRunResult runStackVM(const std::string& source) {
    BackendRunResult r;
    auto t0 = std::chrono::high_resolution_clock::now();
    try {
        Lexer lex;
        auto tokens = lex.scan(source);
        Parser parser;
        auto ast = parser.parse(tokens);
        if (parser.hasErrors()) {
            r.exceptionName = "ParseError: " + parser.getDiagnostics().summary();
        } else {
            Compiler compiler;
            auto result = compiler.compile(*ast);
            if (compiler.getDiagnostics().hasErrors()) {
                r.exceptionName = "CompileError: " + compiler.getLastError();
            } else {
                VM vm;
                vm.setOutputCallback([&r](const std::string& s) {
                    r.output += s;
                    r.output += "\n";
                });
                vm.execute(result);
                if (vm.hasError()) {
                    r.exceptionName = vm.getLastError();
                }
            }
        }
    } catch (const std::exception& e) {
        r.exceptionName = e.what();
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    r.micros = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    return r;
}

// RegisterVM 路径：寄存器式虚拟机
BackendRunResult runRegisterVM(const std::string& source) {
    BackendRunResult r;
    auto t0 = std::chrono::high_resolution_clock::now();
    try {
        Lexer lex;
        auto tokens = lex.scan(source);
        Parser parser;
        auto ast = parser.parse(tokens);
        if (parser.hasErrors()) {
            r.exceptionName = "ParseError: " + parser.getDiagnostics().summary();
        } else {
            Compiler compiler;
            compiler.setUseRegisterVM(true);
            compiler.compile(*ast);
            if (compiler.getDiagnostics().hasErrors()) {
                r.exceptionName = "CompileError: " + compiler.getLastError();
            } else {
                RegisterVM vm;
                vm.setOutputCallback([&r](const std::string& s) {
                    r.output += s;
                    r.output += "\n";
                });
                vm.execute(compiler.getLastRegisterResult());
                if (vm.hasError()) {
                    r.exceptionName = vm.getLastError();
                }
            }
        }
    } catch (const std::exception& e) {
        r.exceptionName = e.what();
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    r.micros = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    return r;
}
} // namespace

void BugHuntPanel::onTripleVerify() {
    // 变体模式下验证当前变体；标准模式下验证当前题目
    if (variantMode_) {
        if (currentVariantIndex_ < 0) {
            statusLabel_->setText(QString::fromUtf8("请先选择变体"));
            return;
        }
    } else {
        if (currentItemIndex_ < 0) {
            statusLabel_->setText(QString::fromUtf8("请先选择题目"));
            return;
        }
    }

    std::string source = codeEditor_->toPlainText().toStdString();
    if (source.empty()) {
        outputEdit_->setPlainText(QString::fromUtf8("（空代码）"));
        return;
    }

    // 三条路径独立执行
    auto ir = runInterpreter(source);
    auto sv = runStackVM(source);
    auto rv = runRegisterVM(source);

    // 一致性判定
    bool outputConsistent = (ir.output == sv.output) && (sv.output == rv.output);
    bool exceptionConsistent =
        !ir.exceptionName.empty() &&
        !sv.exceptionName.empty() &&
        !rv.exceptionName.empty() &&
        ir.exceptionName == sv.exceptionName &&
        sv.exceptionName == rv.exceptionName;

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
    statusLabel_->setText(QString::fromUtf8("三后端对比验证完成"));
    if (variantMode_) {
        variantStatusLabel_->setText(QString::fromUtf8("变体验证完成 — 输出%1")
            .arg(outputConsistent ? QString::fromUtf8("一致") : QString::fromUtf8("不一致")));
    }
}

void BugHuntPanel::onToggleVariantMode() {
    variantMode_ = variantBtn_->isChecked();
    if (variantMode_) {
        // 切换到变体模式：隐藏难度筛选（变体不分级）
        itemList_->hide();
        variantList_->show();
        hintBtn_->hide();
        answerBtn_->hide();
        diffAllBtn_->hide();
        diffBeginnerBtn_->hide();
        diffIntermediateBtn_->hide();
        diffExpertBtn_->hide();
        variantBtn_->setText(mlTr("返回标准"));
        variantStatusLabel_->setText(mlTr("变体模式 — 选择变体后点击三后端对比"));
        if (variantList_->count() > 0 && currentVariantIndex_ < 0) {
            variantList_->setCurrentRow(0);
        }
        if (currentVariantIndex_ >= 0) {
            showCurrentVariant();
        }
    } else {
        // 切换回标准模式：恢复难度筛选按钮
        variantList_->hide();
        itemList_->show();
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
    statusLabel_->setText(variantMode_
        ? mlTr("变体模式")
        : mlTr("标准模式"));
}

void BugHuntPanel::onVariantSelected(int row) {
    currentVariantIndex_ = row;
    showCurrentVariant();
}

void BugHuntPanel::showCurrentVariant() {
    const auto& variants = BugHuntVariantLibrary::variants();
    if (currentVariantIndex_ < 0 || currentVariantIndex_ >= (int)variants.size()) {
        descBrowser_->clear();
        codeEditor_->clear();
        return;
    }
    const auto& v = variants[currentVariantIndex_];
    QString html = QString(
        "<html><body>"
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
    variantStatusLabel_->setText(QString::fromUtf8("当前变体：%1").arg(
        QString::fromUtf8(v.id.c_str())));
}
