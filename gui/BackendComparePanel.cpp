#include "gui/BackendComparePanel.h"
#include "app/IdeController.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "interpreter/Interpreter.h"
#include "compiler/Compiler.h"
#include "compiler/VM.h"
#include "compiler/RegisterVM.h"
#include "compiler/IR.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QTextEdit>
#include <QLabel>
#include <QPushButton>
#include <QApplication>
#include <chrono>
#include <sstream>
#include <algorithm>

BackendComparePanel::BackendComparePanel(QWidget* parent)
    : QWidget(parent) {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(4);

    // 顶部按钮栏
    auto* btnBar = new QHBoxLayout;
    runButton_ = new QPushButton(QString::fromUtf8("运行三后端对比"), this);
    diffLabel_ = new QLabel(QString::fromUtf8("尚未运行"), this);
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
        output->setFont(QFont("Consolas"));
        layout->addWidget(status);
        layout->addWidget(output, 1);
        return container;
    };

    splitter->addWidget(buildColumn(QString::fromUtf8("Interpreter"), interpOutput_, interpStatus_));
    splitter->addWidget(buildColumn(QString::fromUtf8("StackVM"), stackVmOutput_, stackVmStatus_));
    splitter->addWidget(buildColumn(QString::fromUtf8("RegisterVM"), regVmOutput_, regVmStatus_));
    splitter->setSizes({400, 400, 400});
    mainLayout->addWidget(splitter, 1);

    connect(runButton_, &QPushButton::clicked,
            this, &BackendComparePanel::runComparison);
}

void BackendComparePanel::runComparison() {
    if (!controller_) {
        diffLabel_->setText(QString::fromUtf8("未绑定控制器"));
        return;
    }
    // 直接从 controller 获取 AST（已在主编辑器编译完成）
    Block* ast = controller_->astRoot();
    if (!ast) {
        diffLabel_->setText(QString::fromUtf8("请先在主编辑器中输入并编译代码"));
        return;
    }
    runButton_->setEnabled(false);
    diffLabel_->setText(QString::fromUtf8("运行中..."));
    QApplication::processEvents();

    // 直接从 astRoot 调用三后端
    // 注：ast 是 controller 持有的，三后端只读取，安全
    BackendResult interp = runInterpreter("");
    BackendResult stackVm = runStackVM("");
    BackendResult regVm = runRegisterVM("");

    renderComparison(interp, stackVm, regVm);
    runButton_->setEnabled(true);
}

BackendComparePanel::BackendResult BackendComparePanel::runInterpreter(const std::string& /*source*/) {
    BackendResult r;
    if (!controller_ || !controller_->astRoot()) {
        r.status = "NO_AST";
        return r;
    }
    std::ostringstream out;
    auto t0 = std::chrono::high_resolution_clock::now();
    try {
        Interpreter interp;
        interp.setOutputCallback([&out](const std::string& s) {
            out << s << "\n";
        });
        interp.execute(*controller_->astRoot());
        r.status = "OK";
    } catch (const std::exception& e) {
        r.status = "ERROR";
        r.errorMessage = e.what();
        out << "[ERROR] " << e.what();
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    r.elapsedMicros = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    std::string s = out.str();
    std::istringstream iss(s);
    std::string line;
    while (std::getline(iss, line)) r.outputLines.push_back(line);
    return r;
}

BackendComparePanel::BackendResult BackendComparePanel::runStackVM(const std::string& /*source*/) {
    BackendResult r;
    if (!controller_ || !controller_->astRoot()) {
        r.status = "NO_AST";
        return r;
    }
    std::ostringstream out;
    auto t0 = std::chrono::high_resolution_clock::now();
    try {
        Compiler compiler;
        auto result = compiler.compile(*controller_->astRoot());
        if (compiler.getDiagnostics().hasErrors()) {
            r.status = "ERROR";
            r.errorMessage = compiler.getDiagnostics().summary();
            out << "[Compile Error]\n" << r.errorMessage;
        } else {
            VM vm;
            vm.setOutputCallback([&out](const std::string& s) {
                out << s << "\n";
            });
            auto vmres = vm.execute(result);
            r.status = (vmres == VMResult::VM_OK) ? "OK" : "ERROR";
            if (vmres != VMResult::VM_OK) {
                out << "[VM Error] result=" << (int)vmres;
                r.errorMessage = "VM runtime error";
            }
        }
    } catch (const std::exception& e) {
        r.status = "ERROR";
        r.errorMessage = e.what();
        out << "[ERROR] " << e.what();
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    r.elapsedMicros = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    std::string s = out.str();
    std::istringstream iss(s);
    std::string line;
    while (std::getline(iss, line)) r.outputLines.push_back(line);
    return r;
}

BackendComparePanel::BackendResult BackendComparePanel::runRegisterVM(const std::string& /*source*/) {
    BackendResult r;
    if (!controller_ || !controller_->astRoot()) {
        r.status = "NO_AST";
        return r;
    }
    std::ostringstream out;
    auto t0 = std::chrono::high_resolution_clock::now();
    try {
        Compiler compiler;
        compiler.setUseRegisterVM(true);
        auto regResult = compiler.compileViaRegisterIR(*controller_->astRoot());
        if (compiler.getDiagnostics().hasErrors()) {
            r.status = "ERROR";
            r.errorMessage = compiler.getDiagnostics().summary();
            out << "[Compile Error]\n" << r.errorMessage;
        } else {
            RegisterVM vm;
            vm.setOutputCallback([&out](const std::string& s) {
                out << s << "\n";
            });
            auto vmres = vm.execute(regResult);
            r.status = (vmres == VMResult::VM_OK) ? "OK" : "ERROR";
            if (vmres != VMResult::VM_OK) {
                out << "[RegVM Error] result=" << (int)vmres;
                r.errorMessage = "RegisterVM runtime error";
            }
        }
    } catch (const std::exception& e) {
        r.status = "ERROR";
        r.errorMessage = e.what();
        out << "[ERROR] " << e.what();
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    r.elapsedMicros = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    std::string s = out.str();
    std::istringstream iss(s);
    std::string line;
    while (std::getline(iss, line)) r.outputLines.push_back(line);
    return r;
}

void BackendComparePanel::renderComparison(const BackendResult& interp,
                                            const BackendResult& stackVm,
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
        status->setText(QString::fromUtf8("%1 [%2] %3 ms")
            .arg(name)
            .arg(QString::fromUtf8(r.status.c_str()))
            .arg(ms, 0, 'f', 2));
    };
    renderResult(interp, interpOutput_, interpStatus_, QString::fromUtf8("Interpreter"));
    renderResult(stackVm, stackVmOutput_, stackVmStatus_, QString::fromUtf8("StackVM"));
    renderResult(regVm, regVmOutput_, regVmStatus_, QString::fromUtf8("RegisterVM"));

    diffLabel_->setText(buildDiffSummary(interp, stackVm, regVm));
}

QString BackendComparePanel::buildDiffSummary(const BackendResult& a,
                                                 const BackendResult& b,
                                                 const BackendResult& c) {
    // 三后端输出差异比对
    size_t maxLines = std::max({a.outputLines.size(), b.outputLines.size(), c.outputLines.size()});
    int diffCount = 0;
    for (size_t i = 0; i < maxLines; ++i) {
        std::string la = (i < a.outputLines.size()) ? a.outputLines[i] : "";
        std::string lb = (i < b.outputLines.size()) ? b.outputLines[i] : "";
        std::string lc = (i < c.outputLines.size()) ? c.outputLines[i] : "";
        if (la != lb || lb != lc) diffCount++;
    }
    if (diffCount == 0) {
        return QString::fromUtf8("三后端输出一致（%1 行）— 一致性 PASS").arg(maxLines);
    }
    return QString::fromUtf8("三后端差异 %1 行 / 共 %2 行 — 一致性 FAIL")
        .arg(diffCount).arg(maxLines);
}
