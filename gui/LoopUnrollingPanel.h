#pragma once

// ============================================================
// LoopUnrollingPanel — 循环展开可视化教学面板
// ------------------------------------------------------------
// 可视化循环展开（Loop Unrolling）的核心原理与性能收益，
// 让学习者直观理解编译器循环优化的经典技术。
//
// 与 jit-visualizer 面板的分工：
//   - jit-visualizer 侧重「运行真实 JIT 采集类型反馈数据」
//   - 本面板侧重「循环展开原理教学 + 交互式性能模拟器」，
//     不运行 JIT，纯教学/模拟
//
// 两个子页：
//   1. 循环展开原理：理论概览 + 展开前后对比表 +
//      MiniLang 循环展开实现对照（R154 提到 JIT 暂不支持循环展开）
//   2. 循环展开模拟器：输入循环次数与展开因子，
//      对比不同展开因子（1/2/4/8/16）的性能收益 +
//      展开后伪代码预览 + 最优展开因子推荐
//
// 设计约束：
//   - 面板构造函数不创建标题栏（Ide::wrapTeachingPanel 自动包裹）
//   - setController 内联实现（仅赋值，面板不依赖控制器运行）
//   - 纯教学/模拟面板，不运行任何执行引擎
// ============================================================

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTextBrowser>
#include <QWidget>
#include <string>
#include <vector>

class IdeController;

// ---- 静态教学数据结构 ----

/// 展开前后对比表条目（子页 1）
struct UnrollCompareRow {
    std::string metric;   // 指标名
    std::string original; // 原始值
    std::string unroll2;  // 展开 K=2
    std::string unroll4;  // 展开 K=4
    std::string unroll8;  // 展开 K=8
    std::string note;     // 说明
};

/// MiniLang 循环展开实现对照条目（子页 1）
struct MiniLangUnrollMapping {
    std::string conceptName;   // 经典概念
    std::string miniLangState; // MiniLang 状态
    std::string explanation;   // 说明
};

/// 循环展开模拟结果（子页 2）
struct UnrollSimResult {
    int factor;        // 展开因子
    int iterations;    // 迭代次数
    int jumps;         // 跳转次数
    int controlInstrs; // 循环控制指令数
    int bodyInstrs;    // 循环体指令数
    int totalInstrs;   // 总指令数
    double speedup;    // 加速比
};

/// LoopUnrollingLibrary — 静态教学数据与模拟算法
class LoopUnrollingLibrary {
public:
    static const std::vector<UnrollCompareRow>& compareRows();
    static const std::vector<MiniLangUnrollMapping>& miniLangMappings();
    static UnrollSimResult simulate(int loopCount, int factor, int bodyInstrCount = 5);
    static std::string generateUnrolledCode(int loopCount, int factor, const std::string& body = "sum += i;");
    static constexpr int kDefaultBodyInstrCount = 5;
};

// ---- 主面板 ----

class LoopUnrollingPanel : public QWidget {
    Q_OBJECT
public:
    explicit LoopUnrollingPanel(QWidget* parent = nullptr);

    /// 绑定 IdeController（仅赋值，面板不依赖控制器运行）
    void setController(IdeController* controller) { controller_ = controller; }

signals:
    /// 请求将样例代码加载到主编辑器
    void loadSampleRequested(const QString& code);

private slots:
    void onPageSwitch(int idx);
    void onRunSimulation();
    void onSelectPreset(int idx);
    void onLoadSample();

private:
    IdeController* controller_ = nullptr;

    // 子页切换
    QPushButton* pageTheoryBtn_ = nullptr;
    QPushButton* pageSimulatorBtn_ = nullptr;
    QStackedWidget* stack_ = nullptr;

    // 子页 1：循环展开原理
    QTextBrowser* theoryBrowser_ = nullptr;
    QTableWidget* compareTable_ = nullptr; // 展开前后对比表
    QTableWidget* mappingTable_ = nullptr; // MiniLang 实现对照表

    // 子页 2：循环展开模拟器
    QLineEdit* loopCountEdit_ = nullptr;      // 循环次数输入（默认 100）
    QSpinBox* factorSpin_ = nullptr;          // 展开因子选择（1/2/4/8/16，默认 4）
    QPushButton* runBtn_ = nullptr;           // 运行模拟
    QPushButton* presetBtn1_ = nullptr;       // 预设：简单累加 100
    QPushButton* presetBtn2_ = nullptr;       // 预设：嵌套循环
    QPushButton* presetBtn3_ = nullptr;       // 预设：大循环 10000
    QPushButton* presetBtn4_ = nullptr;       // 预设：小循环 4
    QTableWidget* simCompareTable_ = nullptr; // 展开因子对比表（1/2/4/8/16）
    QTextBrowser* codePreview_ = nullptr;     // 展开后伪代码预览
    QTextBrowser* simSummary_ = nullptr;      // 汇总（最优展开因子推荐 + 性能分析）

    // 构造辅助
    void buildTheoryPage(QWidget* host);
    void buildSimulatorPage(QWidget* host);

    // 数据填充
    void populateTheory();

    // 模拟器逻辑
    void renderSimulation(int loopCount);
    void renderUnrolledCode(int loopCount, int factor);
    void renderSummary(int loopCount);
};
