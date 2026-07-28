#include "gui/BackendComparePanel.h"
#include "app/IdeController.h"
#include "common/BackendExecutionService.h" // ARCH-10: 后端执行服务中间层
#include "gui/GuiTextUtils.h"               // R75: monospaceFont() 跨机器字体回退链

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSplitter>
#include <QTextEdit>
#include <QVBoxLayout>
#include <algorithm>
#include <chrono>
#include <sstream>

#include "Label.h"      // QFluentKit（CaptionLabel）
#include "PushButton.h" // QFluentKit（PrimaryPushButton）

// ============================================================
// BackendComparePanel 实现
// 顺序运行 Interpreter / StackVM / RegisterVM 三个后端，捕获各自输出、
// 耗时与状态，并对比三后端的输出一致性。
// ============================================================

/// 构造面板：组装顶部「运行三后端对比」按钮与差异标签，水平三列
/// （Interpreter / StackVM / RegisterVM）只读输出区与状态标签，
/// 并绑定运行按钮信号。
BackendComparePanel::BackendComparePanel(QWidget* parent) : QWidget(parent) {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(4);

    // 顶部按钮栏
    auto* btnBar = new QHBoxLayout;
    runButton_ = new PrimaryPushButton(QString::fromUtf8("运行三后端对比"), this);
    diffLabel_ = new CaptionLabel(QString::fromUtf8("尚未运行"), this);
    btnBar->addWidget(runButton_);
    btnBar->addStretch();
    btnBar->addWidget(diffLabel_);
    mainLayout->addLayout(btnBar);

    // 三列输出（垂直 splitter：上方状态 + 下方输出）
    auto* splitter = new QSplitter(Qt::Horizontal, this);

    auto buildColumn = [this](const QString& title, QTextEdit*& output, QLabel*& status) {
        auto* container = new QWidget(this);
        auto* layout = new QVBoxLayout(container);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(2);
        status = new QLabel(title + QString::fromUtf8(" [待运行]"), container);
        output = new QTextEdit(container);
        output->setReadOnly(true);
        output->setFont(GuiTextUtils::monospaceFont(10));
        layout->addWidget(status);
        layout->addWidget(output, 1);
        return container;
    };

    splitter->addWidget(buildColumn(QString::fromUtf8("Interpreter"), interpOutput_, interpStatus_));
    splitter->addWidget(buildColumn(QString::fromUtf8("StackVM"), stackVmOutput_, stackVmStatus_));
    splitter->addWidget(buildColumn(QString::fromUtf8("RegisterVM"), regVmOutput_, regVmStatus_));
    splitter->setSizes({400, 400, 400});
    mainLayout->addWidget(splitter, 1);

    connect(runButton_, &QPushButton::clicked, this, &BackendComparePanel::runComparison);
}

/// 顺序运行三后端并对比：直接从 controller 取已编译的 AST，分别执行
/// Interpreter / StackVM / RegisterVM，每段执行前后插入 processEvents 让 UI
/// 重绘（避免长耗时场景冻结），最后渲染对比结果与一致性摘要。
void BackendComparePanel::runComparison() {
    if (!controller_) {
        diffLabel_->setText(QString::fromUtf8("未绑定控制器"));
        return;
    }
    // R51-1 fix: 重入守卫——processEvents 期间可能有其他代码路径触发 runComparison，
    // comparing_ 三段式（入口检查 + 置位 + 出口恢复）防止三后端被并发执行。
    if (comparing_) {
        return;
    }
    // ARCH-10: 通过 BackendExecutionService 触发三后端，面板不再依赖
    // Compiler/VM/RegisterVM/Interpreter 内部头文件。源码从 controller 的
    // lastSource() 获取（由 runFrontendPipeline 在主编辑器运行时填充）。
    if (controller_->lastSource().empty()) {
        diffLabel_->setText(QString::fromUtf8("请先在主编辑器中输入并编译代码"));
        return;
    }
    comparing_ = true;
    runButton_->setEnabled(false);
    diffLabel_->setText(QString::fromUtf8("运行中..."));
    // BUG-GUI-AUDIT-1 fix attempt: 原审计建议排除定时器事件，但 Qt 6 已移除通用
    // ExcludeTimers flag（仅保留 X11 平台特定的 X11ExcludeTimers，Windows 上无效）。
    // 保持 ExcludeUserInputEvents（防止用户点击重入），定时器重入风险作为已知限制。
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

    // 直接从 astRoot 调用三后端
    // 注：ast 是 controller 持有的，三后端只读取，安全
    // AUDIT-P2 fix: 三后端串行执行期间插入 processEvents 让 UI 重绘，
    // 避免长耗时场景（如 fib(20)）UI 完全冻结（Windows 标题栏"未响应"）。
    // 与 ProfileDashboardPanel 设计模式一致。
    diffLabel_->setText(QString::fromUtf8("运行 Interpreter 中..."));
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    BackendResult interp = runInterpreter("");
    diffLabel_->setText(QString::fromUtf8("运行 StackVM 中..."));
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    BackendResult stackVm = runStackVM("");
    diffLabel_->setText(QString::fromUtf8("运行 RegisterVM 中..."));
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    BackendResult regVm = runRegisterVM("");

    renderComparison(interp, stackVm, regVm);
    runButton_->setEnabled(true);
    comparing_ = false;
}

// ============================================================
// ARCH-10 重构：三后端执行通过 BackendExecutionService 触发
// ============================================================
// 面板不再直接依赖 Compiler/VM/RegisterVM/Interpreter/Lexer/Parser 等
// 内部头文件。源码从 IdeController::lastSource() 获取（由 runFrontendPipeline
// 在主编辑器运行时填充），交由 BackendExecutionService 完整执行 Lexer →
// Parser → Compiler → 指定后端的流程。

namespace {

/// 将服务层 BackendExecResult 转换为面板内部 BackendResult
BackendComparePanel::BackendResult convertServiceResult(const ::BackendExecResult& sr) {
    BackendComparePanel::BackendResult r;
    r.status = sr.success ? "OK" : "ERROR";
    r.errorMessage = sr.success ? std::string() : (sr.errorPrefix + ": " + sr.errorMsg);
    r.elapsedMicros = sr.elapsedMs * 1000; // ms → μs
    // 按行切分输出（保留原 BackendComparePanel 输出格式）
    std::string s = sr.output;
    std::istringstream iss(s);
    std::string line;
    while (std::getline(iss, line)) {
        r.outputLines.push_back(line);
    }
    // 若有错误，附加错误信息到输出末尾（保持原行为：错误也作为输出展示）
    if (!sr.success && !sr.errorMsg.empty()) {
        r.outputLines.push_back(std::string("[") + sr.errorPrefix + "] " + sr.errorMsg);
    }
    return r;
}

} // namespace

/// 运行 Interpreter 后端：通过 BackendExecutionService 触发。
BackendComparePanel::BackendResult BackendComparePanel::runInterpreter(const std::string& /*source*/) {
    BackendResult r;
    if (!controller_ || controller_->lastSource().empty()) {
        r.status = "NO_AST";
        return r;
    }
    return convertServiceResult(BackendExecutionService::execute(controller_->lastSource(), BackendType::Interpreter));
}

/// 运行 StackVM 后端：通过 BackendExecutionService 触发（IR 路径）。
BackendComparePanel::BackendResult BackendComparePanel::runStackVM(const std::string& /*source*/) {
    BackendResult r;
    if (!controller_ || controller_->lastSource().empty()) {
        r.status = "NO_AST";
        return r;
    }
    return convertServiceResult(BackendExecutionService::execute(controller_->lastSource(), BackendType::StackVM_IR));
}

/// 运行 RegisterVM 后端：通过 BackendExecutionService 触发（寄存器 IR 路径）。
BackendComparePanel::BackendResult BackendComparePanel::runRegisterVM(const std::string& /*source*/) {
    BackendResult r;
    if (!controller_ || controller_->lastSource().empty()) {
        r.status = "NO_AST";
        return r;
    }
    return convertServiceResult(
        BackendExecutionService::execute(controller_->lastSource(), BackendType::RegisterVM_IR));
}

/// 将三后端结果渲染到对应列：填充输出文本与状态标签（状态 + 耗时），
/// 并调用 buildDiffSummary 生成一致性汇总写入差异标签。
void BackendComparePanel::renderComparison(const BackendResult& interp, const BackendResult& stackVm,
                                           const BackendResult& regVm) {
    auto renderResult = [](const BackendResult& r, QTextEdit* output, QLabel* status, const QString& name) {
        if (r.status == "NO_AST") {
            output->setPlainText(QString::fromUtf8("（无 AST）"));
            status->setText(name + QString::fromUtf8(" [无 AST]"));
            return;
        }
        QString text;
        for (const auto& line : r.outputLines) {
            text += QString::fromUtf8(line.c_str()) + "\n";
        }
        output->setPlainText(text);
        double ms = r.elapsedMicros / 1000.0;
        status->setText(
            QString::fromUtf8("%1 [%2] %3 ms").arg(name).arg(QString::fromUtf8(r.status.c_str())).arg(ms, 0, 'f', 2));
    };
    renderResult(interp, interpOutput_, interpStatus_, QString::fromUtf8("Interpreter"));
    renderResult(stackVm, stackVmOutput_, stackVmStatus_, QString::fromUtf8("StackVM"));
    renderResult(regVm, regVmOutput_, regVmStatus_, QString::fromUtf8("RegisterVM"));

    diffLabel_->setText(buildDiffSummary(interp, stackVm, regVm));
}

/// 逐行比对三后端输出（按最大行数对齐，缺失行视为空），统计差异行数，
/// 生成「三后端输出一致 / 差异 N 行」的汇总文本，供差异标签显示。
// AUDIT-P2 fix: 三后端全错误时不能误报「一致性 PASS」——原实现只比较 outputLines，
// 不检查 status。三后端均报相同错误时 diffCount=0 误报 PASS，用户以为代码正确。
QString BackendComparePanel::buildDiffSummary(const BackendResult& a, const BackendResult& b, const BackendResult& c) {
    // 先检查三后端是否全部成功
    bool allOk = (a.status == "OK" && b.status == "OK" && c.status == "OK");
    // 三后端状态是否一致
    bool statusConsistent = (a.status == b.status && b.status == c.status);

    // 三后端输出差异比对
    size_t maxLines = std::max({a.outputLines.size(), b.outputLines.size(), c.outputLines.size()});
    int diffCount = 0;
    for (size_t i = 0; i < maxLines; ++i) {
        std::string la = (i < a.outputLines.size()) ? a.outputLines[i] : "";
        std::string lb = (i < b.outputLines.size()) ? b.outputLines[i] : "";
        std::string lc = (i < c.outputLines.size()) ? c.outputLines[i] : "";
        if (la != lb || lb != lc)
            diffCount++;
    }

    // 三后端均失败时不应报 PASS
    if (!allOk) {
        if (statusConsistent && diffCount == 0) {
            return QString::fromUtf8("三后端均失败（错误一致）— 请修复代码");
        }
        return QString::fromUtf8("三后端均失败且错误不一致 — 请修复代码");
    }

    if (diffCount == 0) {
        return QString::fromUtf8("三后端输出一致（%1 行）— 一致性 PASS").arg(maxLines);
    }
    return QString::fromUtf8("三后端差异 %1 行 / 共 %2 行 — 一致性 FAIL").arg(diffCount).arg(maxLines);
}
