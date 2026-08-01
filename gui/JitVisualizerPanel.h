#pragma once

// ============================================================
// JitVisualizerPanel — JIT 编译可视化教学面板（教学板块拓展）
// ------------------------------------------------------------
// 可视化 MiniLang JIT 后端（R138-R159，基于 asmjit 的第四套执行引擎）
// 的核心工作原理，让学习者直观理解字节码 → 本地机器码的编译过程。
//
// 五个子页：
//   1. 编译原理概览：寄存器分配 / 栈帧布局 / JitContext 字段表 /
//      与 StackVM dispatch loop 对比
//   2. 类型反馈与特化：内置 3 场景（INT/FLOAT/不特化），面板内部
//      独立运行 JITBackend，展示 per-chunk 类型反馈与特化决策
//   3. 热点检测：内置场景展示 per-chunk 调用计数、重编译标志、
//      热点阈值机制
//   4. OpCode 覆盖与回退：58 种支持 OpCode 分类表 + 不支持 OpCode
//      的硬失败行为说明
//   5. 分层编译与 OSR（R160）：展示 tiered compilation 三级分层
//      (Interpreter→Baseline→Specialized)、OSR 栈帧迁移、lazy
//      compilation、deoptimization 四类运行时指标
//
// 设计约束：
//   - 面板内部独立创建 JITBackend 实例运行内置场景（参考 TestJIT.cpp
//     的 runJIT 模式），不修改 IdeController / 引擎层，避免破坏现有
//     性能与稳定性。
//   - JIT 默认启用（CMake MINILANG_USE_JIT=ON）。未启用时子页 2/3/5
//     运行按钮禁用并提示，子页 1/4 静态内容仍可查看。
//   - 所有 JIT getter 只在 execute() 返回后读取（线程安全契约）。
// ============================================================

#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTableWidget>
#include <QTextBrowser>
#include <QWidget>
#include <string>
#include <vector>

#include "gui/TeachingSubPageBar.h" // UX-R fix: 统一子页切换组件（互斥+主题色+动画）

class IdeController;

// ---- 静态教学数据结构 ----

/// 寄存器分配条目（子页 1）
struct JitRegisterInfo {
    std::string reg;  // 寄存器名（如 "r12"）
    std::string role; // 用途（如 "JitContext* ctx"）
    std::string kind; // 性质（如 "callee-saved"）
    std::string note; // 备注（如 "跨调用保留"）
};

/// JitContext 内存布局字段（子页 1）
struct JitContextField {
    int offset;       // 偏移（如 0）
    std::string name; // 字段名（如 "outputCallback"）
    std::string type; // 类型（如 "std::function*"）
    std::string note; // 引入版本/说明（如 "R146 新增"）
};

/// OpCode 分类条目（子页 4）
struct JitOpCodeCategory {
    std::string category;             // 分类名（如 "字面量/栈操作"）
    std::vector<std::string> opcodes; // 该分类下的 OpCode 列表
};

/// 类型反馈场景（子页 2）
struct TypeFeedbackScenario {
    std::string id;                     // 场景 ID
    std::string title;                  // 显示名
    std::string description;            // 说明
    std::string sourceCode;             // MiniLang 源码
    std::string methodChunkName;        // 方法 chunk 名（如 "Adder.inc"，用于调小热点阈值）
    std::string expectedSpecialization; // 预期特化（"INT" / "FLOAT" / "不特化"）
    std::string explanation;            // 特化决策解释
};

/// 热点检测场景（子页 3）
struct HotspotScenario {
    std::string id;              // 场景 ID
    std::string title;           // 显示名
    std::string description;     // 说明
    std::string sourceCode;      // MiniLang 源码
    std::string methodChunkName; // 方法 chunk 名
    std::string explanation;     // 热点检测机制解释
};

/// R160: 分层编译/OSR/deopt 场景（子页 5）
struct TierScenario {
    std::string id;                 // 场景 ID
    std::string title;              // 显示名
    std::string description;        // 说明
    std::string sourceCode;         // MiniLang 源码
    std::string osrChunkName;       // 调小 OSR 阈值的 chunk 名（空=不调）
    std::string expectedHighlights; // 预期亮点（"OSR 迁移" / "lazy 编译" / "deopt" 等）
    std::string explanation;        // 分层编译机制解释
};

/// JitVisualizerLibrary — 静态教学场景库
class JitVisualizerLibrary {
public:
    static const std::vector<JitRegisterInfo>& registerInfos();
    static const std::vector<JitContextField>& contextFields();
    static const std::vector<JitOpCodeCategory>& opCodeCategories();
    static const std::vector<TypeFeedbackScenario>& typeFeedbackScenarios();
    static const std::vector<HotspotScenario>& hotspotScenarios();
    static const std::vector<TierScenario>& tierScenarios();
};

// ---- 主面板 ----

class JitVisualizerPanel : public QWidget {
    Q_OBJECT
public:
    explicit JitVisualizerPanel(QWidget* parent = nullptr);

    void setController(IdeController* controller) { controller_ = controller; }

signals:
    void loadSampleRequested(const QString& code);

private:
    IdeController* controller_ = nullptr;

    // UX-R fix: 子页切换统一用 TeachingSubPageBar（替代手写 page*Btn_ + stack_）
    TeachingSubPageBar* subPageBar_ = nullptr;
    QLabel* jitAvailLabel_ = nullptr; // JIT 启用状态提示

    // 子页 1：编译原理概览
    QTextBrowser* overviewBrowser_ = nullptr;

    // 子页 2：类型反馈与特化
    QListWidget* tfScenarioList_ = nullptr;
    QTextBrowser* tfSourceView_ = nullptr;
    QPushButton* tfRunBtn_ = nullptr;
    QTextBrowser* tfOutputView_ = nullptr;
    QTableWidget* tfTable_ = nullptr;   // per-chunk 类型反馈表
    QLabel* tfDecisionLabel_ = nullptr; // 特化决策结果

    // 子页 3：热点检测
    QListWidget* hotScenarioList_ = nullptr;
    QTextBrowser* hotSourceView_ = nullptr;
    QPushButton* hotRunBtn_ = nullptr;
    QTableWidget* hotCallCountTable_ = nullptr; // per-chunk 调用计数
    QTableWidget* hotRecompileTable_ = nullptr; // per-chunk 重编译标志
    QTextBrowser* hotNoteView_ = nullptr;

    // 子页 4：OpCode 覆盖与回退
    QTextBrowser* opCodeBrowser_ = nullptr;

    // 子页 5：分层编译与 OSR（R160）
    QListWidget* tierScenarioList_ = nullptr;
    QTextBrowser* tierSourceView_ = nullptr;
    QPushButton* tierRunBtn_ = nullptr;
    QTextBrowser* tierOutputView_ = nullptr;  // JIT 输出
    QTableWidget* tierTable_ = nullptr;       // per-chunk tier/OSR/deopt 指标表
    QTextBrowser* tierSummaryView_ = nullptr; // 全局汇总（lazy/osr/deopt 列表 + 总 deopt 次数）

    // 拓展二期·子页 6：字节码↔汇编对照（JITBackend::setAsmCapture 链路）
    class QPlainTextEdit* asmSrcEdit_ = nullptr; // 可编辑源码（预填示例）
    QPushButton* asmRunBtn_ = nullptr;           // 编译并对照按钮
    QTextBrowser* asmBytecodeView_ = nullptr;    // 左：字节码反汇编
    QTextBrowser* asmAsmView_ = nullptr;         // 右：JIT 发射的 x86-64 汇编
    QLabel* asmStatusLabel_ = nullptr;           // 状态/输出摘要

    // 构造辅助
    void buildOverviewPage(QWidget* host);
    void buildTypeFeedbackPage(QWidget* host);
    void buildHotspotPage(QWidget* host);
    void buildOpCodePage(QWidget* host);
    void buildTierMetricsPage(QWidget* host); // R160: 子页 5
    void buildAsmComparePage(QWidget* host);  // 拓展二期: 子页 6

    // 数据填充
    void populateOverview();
    void populateOpCode();
    void populateTfScenarios();
    void populateHotScenarios();
    void populateTierScenarios(); // R160: 子页 5
    void showTfScenario(int index);
    void showHotScenario(int index);
    void showTierScenario(int index); // R160: 子页 5
    void runTypeFeedbackScenario(int index);
    void runHotspotScenario(int index);
    void runTierScenario(int index); // R160: 子页 5
    void runAsmCompare();            // 拓展二期: 子页 6（编译+捕获汇编+双栏展示）
};
