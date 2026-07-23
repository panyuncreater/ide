#pragma once

// ============================================================
// BackendParallelPanel — 三后端并行可视化教学面板
// ------------------------------------------------------------
// 在面板内独立完成 Interpreter / StackVM(IR) / RegisterVM(IR)
// 三个后端执行同一份源码的状态对比，让学习者直观验证三后端
// 语义等价性。面板内部独立完成 Lexer → Parser → 各后端执行
// 流程（不依赖 IdeController），参考 TestNestedLvalueProbe.cpp
// 的 runInterp / runStackVM_IR / runRegVM_IR 辅助函数模式。
//
// 单页设计：
//   - 顶部：QLineEdit 源码输入 + 运行按钮 + 3 个内置样例切换
//   - 中间：QSplitter(Vertical)
//       上方 QTableWidget(5 列: 后端/输出/指令数/状态/耗时ms)
//       下方 QSplitter(Horizontal) 三个 QTextBrowser 详细执行轨迹
//   - 底部：加载样例到主编辑器按钮 + 一致性提示标签
//
// 设计约束：
//   - 面板构造函数不创建标题栏（Ide::wrapTeachingPanel 自动包裹）
//   - setController 内联实现（仅赋值，不注册监听器）
//   - 三后端在主线程串行执行（避免引擎层非线程安全问题）
// ============================================================

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTextBrowser>
#include <QWidget>
#include <string>
#include <vector>

class IdeController;

// ---- 内置示例代码条目 ----

/// 单个内置示例代码条目
struct BackendParallelSample {
    std::string title;       // 示例标题（如 "基本算术"）
    std::string code;        // 示例源码
    std::string description; // 简短描述
};

// ---- 主面板 ----

class BackendParallelPanel : public QWidget {
    Q_OBJECT
public:
    explicit BackendParallelPanel(QWidget* parent = nullptr);

    /// 绑定 IdeController（仅赋值，不注册监听器——与 JitVisualizerPanel 模式一致）
    void setController(IdeController* controller) { controller_ = controller; }

signals:
    /// 请求将样例代码加载到主编辑器
    void loadSampleRequested(const QString& code);

private slots:
    void onRunAllBackends();
    void onLoadSample();
    void onSelectSample(int idx);

private:
    IdeController* controller_ = nullptr;

    // 顶部：源码输入 + 运行按钮 + 样例切换
    QLineEdit* sourceEdit_ = nullptr;
    QPushButton* runBtn_ = nullptr;
    QPushButton* sample1Btn_ = nullptr;
    QPushButton* sample2Btn_ = nullptr;
    QPushButton* sample3Btn_ = nullptr;

    // 中间：结果表格 + 三后端执行轨迹
    QTableWidget* resultTable_ = nullptr;
    QTextBrowser* interpTrace_ = nullptr;
    QTextBrowser* stackVmTrace_ = nullptr;
    QTextBrowser* regVmTrace_ = nullptr;

    // 底部：加载样例按钮 + 一致性提示
    QPushButton* loadSampleBtn_ = nullptr;
    QLabel* consistencyLabel_ = nullptr;

    // 内置示例库（3 个）
    static const std::vector<BackendParallelSample>& samples();

    // 三后端执行结果
    struct BackendExecResult {
        QString output;       // 标准输出（print 输出）
        QString status;       // 状态文本（"✅ 成功" / "❌ 错误: ..."）
        qint64 elapsedMs = 0; // 耗时（毫秒）
        QString instrCount;   // 指令数（"N/A" 或字节数）
        QString traceHtml;    // 详细执行轨迹 HTML
        bool success = false; // 是否成功（编译+运行均无错）
    };

    // 三后端执行函数（参考 TestNestedLvalueProbe.cpp 辅助模式）
    BackendExecResult runInterpreter(const std::string& src);
    BackendExecResult runStackVM_IR(const std::string& src);
    BackendExecResult runRegVM_IR(const std::string& src);

    // 渲染表格行与执行轨迹
    void renderResult(int row, const QString& backendName, const BackendExecResult& r);
    void renderConsistency(const BackendExecResult& interp, const BackendExecResult& stackvm,
                           const BackendExecResult& regvm);
};
