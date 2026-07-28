#pragma once

#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTextEdit>
#include <QWidget>
#include <string>
#include <vector>

// ============================================================
// BackendComparePanel — 三后端并行对比面板（第一波 P0-3）
// ------------------------------------------------------------
// 在一个独立 widget 内顺序运行 Interpreter / StackVM / RegisterVM
// 三个后端，捕获各自输出 + 计时 + 状态，最后展示差异。
//
// 设计权衡：不在 worker 线程并行执行三后端（避免 WorkerManager
// 单 worker 互斥、AST/CompileResult 共享所有权问题），改为
// 在 GUI 线程顺序执行三个后端实例（每个后端独立创建），并通过
// ProcessEvents 防止 UI 冻结。
// ============================================================

class IdeController;

class BackendComparePanel : public QWidget {
    Q_OBJECT
public:
    explicit BackendComparePanel(QWidget* parent = nullptr);

    void setController(IdeController* controller) { controller_ = controller; }

    /// 触发一次三后端对比执行（从当前编辑器源码编译）
    void runComparison();

    // ARCH-10: BackendResult 公开为接口类型，供 convertServiceResult 辅助函数使用
    struct BackendResult {
        std::vector<std::string> outputLines;
        std::string status; // "OK" / "ERROR" / "NO_AST"
        std::string errorMessage;
        int64_t elapsedMicros = 0;
    };

private:
    IdeController* controller_ = nullptr;
    // AUDIT-P2 fix: runComparison 重入守卫——processEvents 期间定时器/信号链路可能重入
    bool comparing_ = false;

    QTextEdit* interpOutput_ = nullptr;
    QTextEdit* stackVmOutput_ = nullptr;
    QTextEdit* regVmOutput_ = nullptr;
    QLabel* interpStatus_ = nullptr;
    QLabel* stackVmStatus_ = nullptr;
    QLabel* regVmStatus_ = nullptr;
    QLabel* diffLabel_ = nullptr;
    QPushButton* runButton_ = nullptr;

    /// 三个后端分别执行
    BackendResult runInterpreter(const std::string& source);
    BackendResult runStackVM(const std::string& source);
    BackendResult runRegisterVM(const std::string& source);

    /// 渲染对比结果到 UI
    void renderComparison(const BackendResult& interp, const BackendResult& stackVm, const BackendResult& regVm);

    /// 计算三后端输出差异（行级比对，标记差异行）
    QString buildDiffSummary(const BackendResult& a, const BackendResult& b, const BackendResult& c);
};
