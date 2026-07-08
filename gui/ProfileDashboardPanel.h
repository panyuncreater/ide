/**
 * @file ProfileDashboardPanel.h
 * @brief 性能基准面板的类声明（功能：多后端性能对比）
 *
 * 提供场景选择、测速运行、结果渲染与逐 opcode 性能分析能力。
 * 信号 loadSampleRequested 用于向编辑器请求载入示例代码。
 */
#pragma once

// ============================================================
// ProfileDashboardPanel — 性能剖析仪表盘（第二波 P1-2）
// ------------------------------------------------------------
// 三后端执行性能对比与热点分析，让学习者理解不同执行模型的性能取舍。
//
// 设计要点：
//   - 三后端时间对比柱状图（C2 优化：编译时选择 QtCharts 或 QPainter 自绘）
//     * MINILANG_HAVE_QTCHARTS 定义时：使用 QChart + QBarSeries（视觉更专业，
//       自带 tooltip/legend/animation），链接 Qt6::Charts
//     * 否则：保留 QPainter 自绘柱状图（零额外依赖，默认行为）
//   - 热点场景库（ProfileLibrary）：内置 6 个典型性能场景
//   - 单次运行多次测量取平均 + 标准差
//   - GC tracked 节点数与 InstructionCount 统计
//
// 本面板顺序运行 Interpreter / StackVM / RegisterVM（与 BackendComparePanel 一致），
// 不修改引擎层 instrumentation。
// ============================================================

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QListWidget>
#include <QTextBrowser>
#include <QTableWidget>
#include <QTimer>
#include <chrono>
#include <functional>
#include <string>
#include <vector>
#include <array>
#include <utility>
#include "ast/ASTNode.h"  // Block 类型完整定义（measureXxxOnce 签名需要）

#ifdef MINILANG_HAVE_QTCHARTS
// C2: QtCharts 可用前向声明，避免头文件强依赖（仅 .cpp 中 include 完整头）
QT_BEGIN_NAMESPACE
class QChartView;
class QChart;
class QBarSeries;
QT_END_NAMESPACE
#endif

class IdeController;

// ---- 性能场景库数据结构 ----

/// 单个性能场景
struct ProfileScenario {
    std::string id;          // 场景 ID
    std::string title;       // 显示名
    std::string description;  // 文字说明（解释性能差异原因）
    std::string sourceCode;   // MiniLang 源码
    std::string category;     // 分类：arithmetic / loop / string / class / closure
    int         iterations;   // 运行迭代次数（取平均）
};

/// ProfileLibrary — 性能场景库
class ProfileLibrary {
public:
/// 返回预设测速场景（静态数据）。
    static const std::vector<ProfileScenario>& scenarios();
};

// ---- OpCode 性能文档库（P1-1 真实 instrumentation 配套）----

/// 单个 OpCode 的性能特征文档（教学用）
struct OpCodePerfDoc {
    std::string opCode;          // OpCode 名（如 OP_ADD）
    std::string category;        // 分类：constant/arith/compare/var/call/control/container
    std::string perfNote;        // 性能特征说明（为何该指令是热点 / 是否昂贵）
    std::string exampleCode;     // 触发该指令的示例代码
};

/// OpCodeProfileLibrary — OpCode 性能文档静态库
class OpCodeProfileLibrary {
public:
/// 返回 opcode 性能文档（静态数据）。
    static const std::vector<OpCodePerfDoc>& docs();
};

// ---- 指令计数结果结构 ----

/// 单个 OpCode 的执行计数（剖析后聚合）
struct OpCodeProfileEntry {
    std::string opCodeName;   // OpCode 名（OP_ADD / REG_ADD 等）
    uint64_t    count = 0;   // 执行次数
    double      ratio = 0.0; // 占总指令数的比例（0-1）
};

// ---- 主面板 ----

class ProfileDashboardPanel : public QWidget {
    Q_OBJECT
public:
/// 构造性能基准面板；parent 为父控件。
    explicit ProfileDashboardPanel(QWidget* parent = nullptr);

    void setController(IdeController* controller) { controller_ = controller; }

signals:
/// 信号：请求主窗口载入指定示例代码。
    void loadSampleRequested(const QString& code);

private:
    IdeController* controller_ = nullptr;

    // 左侧场景列表
    QListWidget* scenarioList_ = nullptr;
    QTextBrowser* scenarioDesc_ = nullptr;

    // 右侧三后端柱状图 + 性能表
    QPushButton* runProfileBtn_ = nullptr;
    QLabel* statusLabel_        = nullptr;
    QTableWidget* resultTable_   = nullptr;  // 后端 / 平均时间 / 标准差 / 比值
    QTextBrowser* analysisView_ = nullptr;  // 文字分析
#ifdef MINILANG_HAVE_QTCHARTS
    // C2: QtCharts 模式下的图表视图（替代 paintEvent 自绘）
    QChartView* chartView_ = nullptr;
    QChart*     chart_     = nullptr;
    QBarSeries* barSeries_ = nullptr;
#endif

    // 单后端测量
    struct BackendTiming {
        std::string name;
        double avgMicros = 0.0;
        double stddevMicros = 0.0;
        bool success = true;
        std::string errorMessage;
    };

    /// P1-1: 指令计数结果（仅 StackVM / RegisterVM，Interpreter 无指令概念）
    std::vector<OpCodeProfileEntry> lastStackVMOpProfile_;
    std::vector<OpCodeProfileEntry> lastRegisterVMOpProfile_;

    /// P1-1: 指令计数 UI（与时间柱状图通过 tab 切换）
    QTableWidget* stackVmOpTable_     = nullptr;  // StackVM Top N 热点 opcode
    QTableWidget* registerVmOpTable_ = nullptr;  // RegisterVM Top N 热点 opcode
    QTextBrowser* opCodeDocView_     = nullptr;  // OpCode 性能文档说明

    /// 测量单后端单次执行时间（微秒）
    double measureInterpreterOnce(Block& ast);
    double measureStackVMOnce(Block& ast);
    double measureRegisterVMOnce(Block& ast);

    /// P1-1: 在 StackVM/RegisterVM 测量期间累加 opcode 计数到本地数组
    /// 返回 (微秒, opcodeCounts[256])；测量期间 stepCallback 启用
    std::pair<double, std::array<uint64_t, 256>> measureStackVMWithProfile(Block& ast);
    std::pair<double, std::array<uint64_t, 256>> measureRegisterVMWithProfile(Block& ast);

    /// 多次测量取平均 + 标准差
    BackendTiming measureBackend(const std::string& name,
                                 std::function<double(Block&)> measure,
                                 Block& ast, int iterations);

    /// 渲染柱状图（QPainter 自绘模式；QtCharts 模式下走 renderChart）
    void paintEvent(QPaintEvent* event) override;

    /// P1-1: 渲染指令计数表格（StackVM / RegisterVM Top N 热点 opcode）
    void renderOpCodeProfile(const std::vector<OpCodeProfileEntry>& stackVmProfile,
                              const std::vector<OpCodeProfileEntry>& registerVmProfile);

#ifdef MINILANG_HAVE_QTCHARTS
    /// C2: QtCharts 模式下用 QChart + QBarSeries 渲染柱状图
    void renderChart(const std::vector<BackendTiming>& results);
#endif

    /// 当前的测量结果（用于 paintEvent 绘制）
    std::vector<BackendTiming> lastResults_;

    /// 问题 6: "运行中"状态动画 — 循环显示 "运行中." → "运行中.." → "运行中..."
    QTimer* statusAnimTimer_ = nullptr;
    int statusAnimDots_ = 0;
    QString statusRunningBase_;  // "运行中" 基础文本（含进度）
/// 启动测速状态动画。
    void startStatusAnimation(const QString& base);
/// 停止测速状态动画。
    void stopStatusAnimation();

    /// 跑场景
    void runProfile(int scenarioIndex);

    /// 刷新表格 + 分析文字
    void renderResults(const std::vector<BackendTiming>& results,
                        const ProfileScenario& scenario);

    /// 文字分析：哪个后端最快 / 比值 / 原因
    QString buildAnalysis(const std::vector<BackendTiming>& results,
                           const ProfileScenario& scenario);

    /// 工具：标准化均值
    static double mean(const std::vector<double>& xs);
/// 计算样本标准差（统计辅助）。
    static double stddev(const std::vector<double>& xs);
};
