#pragma once

// ============================================================
// PerformanceRacePanel — 三后端性能竞赛教学面板
// ------------------------------------------------------------
// 在面板内独立完成 Interpreter / StackVM(IR) / RegisterVM(IR)
// 三个后端执行同一份源码的性能对比，让学习者直观理解三后端
// 性能差异。每个后端运行 N 次（1-10，默认 5），记录平均/最小/
// 最大耗时，并用 HTML/CSS 绘制水平柱状图可视化对比。
//
// 与 backend-parallel 面板互补：
//   - backend-parallel 侧重「执行轨迹文本对比」
//   - 本面板侧重「性能数据可视化（柱状图）+ 多次运行取平均
//     + 性能分析报告」
//
// 单页设计（不使用子页切换）：
//   - 顶部：QLineEdit 源码输入 + 运行次数选择（QSpinBox 1-10）
//           + 竞赛按钮 + 3 个内置样例切换（基本算术/递归fib/循环累加）
//   - 中间：QSplitter(Vertical)
//       上方 QTableWidget 性能对比表（5列：后端/平均/最小/最大/指令数）
//         行：Interpreter / StackVM(IR) / RegisterVM(IR)
//       中间 QTextBrowser 柱状图（HTML/CSS 水平柱状图）
//       下方 QTextBrowser 性能分析报告
//   - 底部：加载样例到主编辑器按钮 + 一致性检查标签
//
// 设计约束：
//   - 面板构造函数不创建标题栏（Ide::wrapTeachingPanel 自动包裹）
//   - setController 内联实现（仅赋值，不注册监听器）
//   - 三后端在主线程串行执行（避免引擎层非线程安全问题）
//   - 每个后端运行 N 次（运行次数由 QSpinBox 选择），取平均/最小/最大
//   - 复用 BackendParallelPanel.cpp 的三后端执行模式（Compiler
//     setUseIR(true) / setUseRegisterVM(true) 触发 IR 路径）
// ============================================================

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTextBrowser>
#include <QWidget>
#include <string>
#include <vector>

class IdeController;

// ---- 内置示例代码条目 ----

/// 单个性能竞赛样例
struct PerfSample {
    std::string title;       // 样例标题
    std::string code;        // 样例代码
    std::string description; // 描述
};

// ---- 单后端性能结果 ----

/// 三后端之一的性能结果（多次运行汇总）
struct BackendPerfResult {
    QString backendName;      // 后端名
    double avgMs = 0.0;       // 平均耗时
    double minMs = 0.0;       // 最小耗时
    double maxMs = 0.0;       // 最大耗时
    QString instrCount;       // 指令数（"N/A" 或数字）
    QString output;           // 输出
    QString status;           // 状态
    bool success = false;     // 是否成功
    std::vector<double> runs; // 每次运行的耗时（ms）
};

// ---- 静态教学数据 ----

/// PerformanceRaceLibrary — 静态教学样例库
class PerformanceRaceLibrary {
public:
    static const std::vector<PerfSample>& samples();
};

// ---- 主面板 ----

class PerformanceRacePanel : public QWidget {
    Q_OBJECT
public:
    explicit PerformanceRacePanel(QWidget* parent = nullptr);

    /// 绑定 IdeController（仅赋值，不注册监听器——与 BackendParallelPanel 模式一致）
    void setController(IdeController* controller) { controller_ = controller; }

signals:
    /// 请求将样例代码加载到主编辑器
    void loadSampleRequested(const QString& code);

private slots:
    void onRunRace();
    void onLoadSample();
    void onSelectSample(int idx);

private:
    IdeController* controller_ = nullptr;

    // 顶部：源码输入 + 运行次数 + 竞赛按钮 + 样例切换
    QLineEdit* sourceEdit_ = nullptr;
    QSpinBox* runCountSpin_ = nullptr;
    QPushButton* raceBtn_ = nullptr;
    QPushButton* sample1Btn_ = nullptr;
    QPushButton* sample2Btn_ = nullptr;
    QPushButton* sample3Btn_ = nullptr;

    // 中间：性能对比表 + 柱状图 + 分析报告
    QTableWidget* resultTable_ = nullptr;
    QTextBrowser* barChartView_ = nullptr;
    QTextBrowser* analysisView_ = nullptr;

    // 底部：加载样例按钮 + 一致性标签
    QPushButton* loadSampleBtn_ = nullptr;
    QLabel* consistencyLabel_ = nullptr;

    // 单次运行结果（参考 BackendParallelPanel::BackendExecResult，去掉 traceHtml）
    struct SingleRunResult {
        QString output;       // 标准输出（print 输出）
        QString status;       // 状态文本（"✅ 成功" / "❌ 错误: ..."）
        qint64 elapsedMs = 0; // 单次耗时（毫秒）
        QString instrCount;   // 指令数（"N/A" 或字节数）
        bool success = false; // 是否成功（编译+运行均无错）
    };

    // 三后端单次执行函数（参考 BackendParallelPanel.cpp 的
    // runInterpreter / runStackVM_IR / runRegVM_IR 实现模式）
    SingleRunResult runInterpreterOnce(const std::string& src);
    SingleRunResult runStackVM_IR_Once(const std::string& src);
    SingleRunResult runRegVM_IR_Once(const std::string& src);

    // 多次运行汇总：调用指定单次执行函数 N 次，计算平均/最小/最大
    BackendPerfResult runBackendMultiple(const std::string& src, const QString& backendName, int runCount,
                                         SingleRunResult (PerformanceRacePanel::*runner)(const std::string&));

    // 渲染
    void renderResultRow(int row, const BackendPerfResult& r);
    void renderBarChart(const BackendPerfResult& interp, const BackendPerfResult& stackvm,
                        const BackendPerfResult& regvm);
    void renderAnalysis(const BackendPerfResult& interp, const BackendPerfResult& stackvm,
                        const BackendPerfResult& regvm);
    void renderConsistency(const BackendPerfResult& interp, const BackendPerfResult& stackvm,
                           const BackendPerfResult& regvm);
};
