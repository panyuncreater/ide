#pragma once

// ============================================================
// IRTransformPanel — IR 变换过程可视化面板（第二波 P1-1）
// ------------------------------------------------------------
// 可视化 AST → IR 三地址码 lowering 过程与优化 pass 前后对比，
// 让学习者理解 SSA-like 虚拟寄存器分配与优化变换。
//
// 三个子页：
//   1. 场景库：内置 8 个典型 AST 节点的 lowering 演示
//   2. 优化 pass 对比：常量折叠 / 死代码消除 / 复制传播前后 IR
//   3. 当前源码 IR：从 IdeController 获取当前 AST 的 IR 反汇编
//
// 本面板消费 IdeController + IR.h 公共 API（IRToString / irOpName），
// 不修改引擎层。
// ============================================================

#include <QWidget>
#include <QStackedWidget>
#include <QLabel>
#include <QPushButton>
#include <QListWidget>
#include <QTextBrowser>
#include <string>
#include <vector>

class IdeController;

// ---- 教学场景库数据结构 ----

/// IR lowering 演示场景（AST → IR 三地址码）
struct IRLoweringExample {
    std::string id;          // 场景 ID
    std::string title;       // 显示名
    std::string description; // 文字说明
    std::string astSummary;  // AST 节点摘要（如 "BinaryOp(ADD, 1, 2)"）
    std::string sourceCode;  // MiniLang 源码
    std::string irBefore;    // lowering 后的 IR 文本（IRToString 格式）
};

/// 优化 pass 前后对比场景
struct IROptimizationExample {
    std::string id;
    std::string title;
    std::string description;     // 优化 pass 文字说明
    std::string passName;        // 优化 pass 名（如 "常量折叠"）
    std::string irBefore;        // 优化前 IR 文本
    std::string irAfter;         // 优化后 IR 文本
    int         instrBefore = 0; // 优化前指令数
    int         instrAfter = 0;  // 优化后指令数
};

/// IRTransformLibrary — 静态教学场景库
class IRTransformLibrary {
public:
    static const std::vector<IRLoweringExample>& loweringExamples();
    static const std::vector<IROptimizationExample>& optimizationExamples();
};

// ---- 主面板 ----

class IRTransformPanel : public QWidget {
    Q_OBJECT
public:
    explicit IRTransformPanel(QWidget* parent = nullptr);

    void setController(IdeController* controller) { controller_ = controller; }

    /// 重新载入"当前源码 IR"页（用户切换到该页或编译完成后调用）
    void reloadCurrentIR();

signals:
    void loadSampleRequested(const QString& code);

private:
    IdeController* controller_ = nullptr;

    QPushButton* pageLoweringBtn_ = nullptr;
    QPushButton* pageOptimizeBtn_ = nullptr;
    QPushButton* pageCurrentBtn_   = nullptr;
    QStackedWidget* stack_         = nullptr;

    // 子页 1：lowering 演示
    QListWidget* loweringList_    = nullptr;
    QTextBrowser* loweringDetail_ = nullptr;

    // 子页 2：优化 pass 对比
    QListWidget* optList_         = nullptr;
    QTextBrowser* optBefore_      = nullptr;
    QTextBrowser* optAfter_       = nullptr;
    QLabel* optSummary_          = nullptr;

    // 子页 3：当前源码 IR
    QTextBrowser* currentIrBrowser_ = nullptr;
    QPushButton* refreshBtn_         = nullptr;
    QLabel* currentStatusLabel_     = nullptr;

    // 构造辅助
    void buildLoweringPage(QWidget* host);
    void buildOptimizePage(QWidget* host);
    void buildCurrentPage(QWidget* host);

    // 数据填充
    void populateLoweringList();
    void populateLoweringDetail(int index);
    void populateOptList();
    void populateOptDetail(int index);
    void populateCurrentIR();
};
