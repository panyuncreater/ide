#pragma once

// ============================================================
// CoroutineVisualizerPanel — 协程/生成器可视化教学面板
// ------------------------------------------------------------
// 可视化 MiniLang 协程/生成器（fun* / yield / .next() / .done()）
// 在 Interpreter / StackVM(IR) / RegisterVM(IR) 三条后端路径上的
// yield 重放机制与语义等价性。JIT 后端暂不支持协程，按钮文案
// 标注「运行四后端」但实际执行三后端（与 BackendParallelPanel 一致）。
//
// 面板内部独立完成 Lexer → Parser → 三后端执行流程，复用
// BackendParallelPanel 的 runInterpreter / runStackVM_IR / runRegVM_IR
// 辅助函数模式，并在表格与执行轨迹中额外展示协程状态摘要
// （yield 序列、done 终止态、生成器 chunk 的 isGenerator/yieldCount）。
//
// 单页设计：
//   - 顶部：QLineEdit 源码输入 + "运行四后端"按钮 + 样例 QComboBox
//   - 中间：QSplitter(Vertical)
//       上方 QTableWidget(5 列: 后端/输出/状态/协程状态/耗时ms)
//       下方 QSplitter(Horizontal) 三个 QTextBrowser 详细执行轨迹
//   - 底部：加载样例到主编辑器按钮 + 一致性提示标签
//
// 设计约束：
//   - 面板构造函数不创建标题栏（Ide::wrapTeachingPanel 自动包裹）
//   - setController 内联实现（仅赋值，不注册监听器）
//   - 三后端在主线程串行执行（避免引擎层非线程安全问题）
//   - 字体使用 GuiTextUtils::monospaceFont(N) 工厂
//   - 显示字符串用 mlTr() 包裹（注册时由 PanelCatalog 标注）
// ============================================================

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTextBrowser>
#include <QWidget>
#include <string>
#include <vector>

class IdeController;
class GuidedTour;

// ---- 内置协程示例代码条目 ----

/// 单个内置协程示例代码条目
struct CoroutineSample {
    std::string title;       // 示例标题（如 "基础三 yield"）
    std::string code;        // 示例源码
    std::string description; // 简短描述
};

// ---- 主面板 ----

class CoroutineVisualizerPanel : public QWidget {
    Q_OBJECT
public:
    explicit CoroutineVisualizerPanel(QWidget* parent = nullptr);

    /// 绑定 IdeController（仅赋值，不注册监听器——与 BackendParallelPanel/JitVisualizerPanel 一致）
    void setController(IdeController* controller) { controller_ = controller; }

    /// 创建该面板的新手引导（4 步），调用方负责持有并调用 start()
    GuidedTour* createGuidedTour(QWidget* host);

    // ARCH-10: BackendExecResult 公开为接口类型，供 convertDetailToPanelResult 辅助函数使用
    // 三后端执行结果
    struct BackendExecResult {
        QString output;         // 标准输出（print 输出）
        QString status;         // 状态文本（"✅ 成功" / "❌ 错误: ..."）
        QString coroutineState; // 协程状态摘要（"next×3, done=true" 等）
        qint64 elapsedMs = 0;   // 耗时（毫秒）
        QString instrCount;     // 指令数（"N/A" 或字节数，仅展示在轨迹 HTML 中）
        QString traceHtml;      // 详细执行轨迹 HTML
        bool success = false;   // 是否成功（编译+运行均无错）
    };

signals:
    /// 请求将样例代码加载到主编辑器（连接到 Ide::loadCodeIntoMainEditor）
    void loadSampleRequested(const QString& code);

private slots:
    void onRunAllBackends();
    void onLoadSample();
    void onSelectSample(int idx);

private:
    IdeController* controller_ = nullptr;

    // 顶部：源码输入 + 运行按钮 + 样例下拉
    QLineEdit* sourceEdit_ = nullptr;
    QPushButton* runBtn_ = nullptr;
    QComboBox* sampleCombo_ = nullptr;

    // 中间：结果表格 + 三后端执行轨迹
    QTableWidget* resultTable_ = nullptr;
    QTextBrowser* interpTrace_ = nullptr;
    QTextBrowser* stackVmTrace_ = nullptr;
    QTextBrowser* regVmTrace_ = nullptr;

    // 底部：加载样例按钮 + 一致性提示
    QPushButton* loadSampleBtn_ = nullptr;
    QLabel* consistencyLabel_ = nullptr;

    // 内置示例库（5 个）
    static const std::vector<CoroutineSample>& samples();

    // ARCH-10: BackendExecResult 已上移至 public 区段

    // 三后端执行函数（参考 BackendParallelPanel.cpp 辅助模式）
    BackendExecResult runInterpreter(const std::string& src);
    BackendExecResult runStackVM_IR(const std::string& src);
    BackendExecResult runRegVM_IR(const std::string& src);

    // 渲染表格行与执行轨迹
    void renderResult(int row, const QString& backendName, const BackendExecResult& r);
    void renderConsistency(const BackendExecResult& interp, const BackendExecResult& stackvm,
                           const BackendExecResult& regvm);

    // 解析源码与输出，提取协程状态摘要（"next×N, done=<value>"）
    QString extractCoroutineState(const std::string& src, const QString& output);
};
