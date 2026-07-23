#pragma once

// ============================================================
// FuzzPlaygroundPanel — 模糊测试游乐场教学面板
// ------------------------------------------------------------
// 交互式展示 R164 模糊测试的三后端差分验证。复用 cli/fuzz_core.h
// 的公开 API（fuzzThreeAgree / runFuzzBatch / formatSummaryText），
// 支持三种模式：
//   (1) 单次差分：输入源码立即三后端对比
//   (2) 批量生成：基于种子模板化随机生成 N 段程序后统计
//   (3) 批量变异：对种子语料做字节级变异 N 次后统计
//
// 单页设计（不使用子页切换）：
//   - 顶部：QTextEdit 源码编辑器（多行，默认填入样例）
//           + 模式切换 QComboBox（单次差分 / 批量生成 / 批量变异）
//           + 运行按钮 + QSpinBox 种子（0-999999，默认 42）
//           + QSpinBox 迭代次数（1-200，默认 20，仅批量模式）
//           + 加载样例按钮
//   - 中间：QSplitter(Vertical)
//       上方 QTabWidget（3 个标签页）
//         Tab1 差分结果：QTableWidget
//                       单次模式 3 行 Interpreter/StackVM/RegisterVM
//                       4 列 后端/输出/状态/耗时
//                       批量模式 1 行汇总
//                       6 列 总次数/一致/分歧/崩溃/解析失败/运行时错误
//         Tab2 分歧详情：QTableWidget
//                       3 列 序号/源码片段/分歧类型（仅批量模式有数据）
//         Tab3 统计报告：QTextBrowser HTML 报告
//                        （差分摘要 + 错误归一化说明 + 教学解释）
//       下方 QTextBrowser 选中项详情
//                  （选中差分结果行或分歧行后展示完整源码+三后端输出对比 HTML）
//   - 底部：QLabel 一致性提示（"✅ 三后端一致 N 次" / "❌ 发现 M 个分歧"）
//           + 加载源码到主编辑器按钮
//
// 设计约束：
//   - 面板构造函数不创建标题栏（Ide::wrapTeachingPanel 自动包裹）
//   - setController 内联实现（仅赋值，不注册监听器）
//   - 批量模式在主线程串行执行（runFuzzBatch 阻塞调用，教学面板可接受；
//     不放到 Worker 线程避免复杂化）
//   - 批量模式迭代次数上限 200（避免 UI 卡死太久）
//   - 分歧用例最多展示前 20 个（disagreementCases 上限 50）
// ============================================================

#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTextEdit>
#include <QWidget>
#include <string>
#include <vector>

#include "cli/fuzz_core.h" // FuzzResult, FuzzSummary, fuzzThreeAgree, runFuzzBatch

class IdeController;
class GuidedTour;

// ---- 内置示例代码条目 ----

/// 单个模糊测试样例
struct FuzzSample {
    std::string title;       // 样例标题（如 "基本算术"）
    std::string code;        // 样例源码
    std::string description; // 简短描述
};

// ---- 主面板 ----

class FuzzPlaygroundPanel : public QWidget {
    Q_OBJECT
public:
    explicit FuzzPlaygroundPanel(QWidget* parent = nullptr);

    /// 绑定 IdeController（仅赋值，不注册监听器——与 BackendParallelPanel 模式一致）
    void setController(IdeController* controller) { controller_ = controller; }

    /// 创建该面板的新手引导（4 步），调用方负责持有并调用 start()
    GuidedTour* createGuidedTour(QWidget* host);

signals:
    /// 请求将样例代码加载到主编辑器
    void loadSampleRequested(const QString& code);

private slots:
    void onRun();
    void onLoadNextSample();
    void onLoadSampleToMain();
    void onModeChanged(int idx);
    void onDiffRowSelected(int row, int col);
    void onDisagreeRowSelected(int row, int col);

private:
    IdeController* controller_ = nullptr;

    // 顶部：源码编辑器 + 模式切换 + 运行 + 种子 + 迭代次数 + 加载样例
    QTextEdit* sourceEdit_ = nullptr;
    QComboBox* modeCombo_ = nullptr;
    QPushButton* runBtn_ = nullptr;
    QSpinBox* seedSpin_ = nullptr;
    QSpinBox* iterSpin_ = nullptr;
    QPushButton* loadSampleBtn_ = nullptr;

    // 中间：QTabWidget（3 个标签页）+ 详情视图
    QTabWidget* tabWidget_ = nullptr;
    QTableWidget* diffTable_ = nullptr;     // Tab1 差分结果
    QTableWidget* disagreeTable_ = nullptr; // Tab2 分歧详情
    QTextBrowser* reportView_ = nullptr;    // Tab3 统计报告
    QTextBrowser* detailView_ = nullptr;    // 选中项详情（源码+三后端输出对比）

    // 底部：一致性标签 + 加载到主编辑器按钮
    QLabel* consistencyLabel_ = nullptr;
    QPushButton* loadToMainBtn_ = nullptr;

    // 数据缓存（用于详情显示）
    minilang_fuzz::FuzzResult lastSingleResult_;  // 上次单次差分结果
    minilang_fuzz::FuzzSummary lastBatchSummary_; // 上次批量汇总
    bool hasSingleResult_ = false;                // 是否已有单次结果
    bool hasBatchResult_ = false;                 // 是否已有批量结果
    int currentSampleIdx_ = 0;                    // 当前样例索引（循环切换）

    // 内置样例库（5 个）
    static const std::vector<FuzzSample>& samples();

    // 表格设置（列数与表头切换）
    void setupDiffTableForSingle();
    void setupDiffTableForBatch();
    void setupDisagreeTable();

    // 渲染
    void renderSingleMode(const minilang_fuzz::FuzzResult& r);
    void renderBatchMode(const minilang_fuzz::FuzzSummary& s, const QString& modeName);
    void renderDisagreeTable(const minilang_fuzz::FuzzSummary& s);
    void renderReportSingle(const minilang_fuzz::FuzzResult& r);
    void renderReportBatch(const minilang_fuzz::FuzzSummary& s, const QString& modeName);
    void renderConsistencySingle(const minilang_fuzz::FuzzResult& r);
    void renderConsistencyBatch(const minilang_fuzz::FuzzSummary& s);

    // 详情视图
    void showSingleDetail();
    void showBatchDisagreeDetail(int idx);
    void clearDetail();

    // 辅助
    bool isBatchMode() const;
    QString statusFromOutput(const std::string& output);
    QString escapeHtml(const std::string& s);
    QString summarizeOutput(const std::string& output, int maxLen = 80);
};
