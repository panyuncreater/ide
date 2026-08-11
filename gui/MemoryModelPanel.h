#pragma once

// ============================================================
// MemoryModelPanel — 内存模型可视化面板（第二波 P0-2）
// ------------------------------------------------------------
// 可视化 MiniLang 的 NaN-boxing / COW / 引用计数 / GcManager 内存模型，
// 让学习者直观理解 Value 8 字节编码与堆对象生命周期。
//
// 三个子页：
//   1. NaN-boxing 编码：8 字节 Value 的 tag/payload 分段图示
//      + int48 范围 + 超范围装箱演示
//   2. RefCounted 引用计数：构造 / addRef / release / COW detach 动画
//   3. GcManager 周期清理：tracked 节点数 / mark-sweep 流程
//
// 本面板仅消费 IdeController 已有 API + 自带 MemoryModelLibrary 静态教学场景库，
// 不修改引擎层（避免破坏现有性能）。
// ============================================================

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QWidget>
#include <string>
#include <vector>

class IdeController;
class TeachingSubPageBar;
struct Value; // R113 B 项：currentHeapValues_ / renderHeapObjectDetail 使用 Value 副本

// ---- 教学场景库数据结构 ----

/// 单个 NaN-boxing 编码示例
struct NaNBoundingBox {
    std::string id;          // 示例 ID（如 "int-inline"）
    std::string title;       // 显示名
    std::string description; // 说明文字
    std::string sourceExpr;  // 表达式源码（如 "42" / "3.14" / "true" / "null"）
    uint64_t bits = 0;       // 编码后的 64 位原始位
    int64_t intVal = 0;      // 解码后的 int 值（若 applicable）
    double floatVal = 0.0;   // 解码后的 float 值（若 applicable）
    bool isInt = false;
    bool isFloat = false;
    bool isBool = false;
    bool isNull = false;
    bool isPointer = false;
};

/// 单个 RefCounted 场景步骤（构造 / 拷贝 / release / COW detach）
struct RefCountStep {
    std::string action; // 动作描述（如 "var a = [1,2,3]"）
    int refCount = 1;   // 操作后 refCount
    std::string note;   // 备注（如 "构造新对象 refCount=1"）
};

/// 单个 RefCounted 场景
struct RefCountScenario {
    std::string id;
    std::string title;
    std::string description;
    std::vector<RefCountStep> steps;
};

/// GcManager 阶段说明
struct GcPhaseInfo {
    std::string title;
    std::string description;
};

// ---- 第 4 子页：实时动画场景库（P4-4）----

/// GC 动画阶段描述（mark/sweep/reset/idle）
struct MemoryAnimPhase {
    std::string phase;       // 阶段 ID: "mark" / "sweep" / "reset" / "idle"
    std::string description; // 阶段说明
    std::string color;       // 显示颜色提示（用于 paintEvent 自绘）
};

/// MemoryAnimLibrary — 实时动画场景静态库
/// 提供 GC 动画 4 阶段说明 + 6 种堆对象类型说明，供第 4 子页消费。
class MemoryAnimLibrary {
public:
    static const std::vector<MemoryAnimPhase>& gcAnimPhases();
    static const std::vector<std::pair<std::string, std::string>>& heapObjectTypes();
};

/// MemoryModelLibrary — 静态教学场景库
class MemoryModelLibrary {
public:
    static const std::vector<NaNBoundingBox>& nanBoxExamples();
    static const std::vector<RefCountScenario>& refCountScenarios();
    static const std::vector<GcPhaseInfo>& gcPhases();
};

// ---- 主面板 ----

class MemoryModelPanel : public QWidget {
    Q_OBJECT
public:
    explicit MemoryModelPanel(QWidget* parent = nullptr);
    // AUDIT-P0 fix: 析构时反注册 IdeController 监听器。
    ~MemoryModelPanel() override;

    /// 绑定到 IdeController（OPT-1: 同时注册 vmStateChanged 监听器，替代 500ms 轮询）
    /// 实现移至 .cpp（调用 addVmStateChangedListener 需 IdeController 完整类型定义）
    void setController(IdeController* controller);

signals:
    /// 请求加载样例代码到主编辑器（与第一波教学面板信号一致）
    void loadSampleRequested(const QString& code);

protected:
    /// 面板显示时恢复第 4 子页动画 QTimer（若已开启），隐藏时停止
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    /// OPT-1: vmStateChanged 监听回调——仅当面板可见时刷新第 4 子页实时状态。
    /// 不检查 autoRefresh 按钮是因为 MemoryModelPanel 第 4 子页的"自动刷新"
    /// 由 animAutoRefreshBtn_ 的 checked 状态控制 animTimer_，监听器作为补充通道。
    /// R113 A 项: 同步刷新第 5 子页 RegisterVM 寄存器帧状态。
    void onVmStateChanged() {
        if (isVisible()) {
            refreshAnimState();
            refreshRegVmState();
        }
    }

    IdeController* controller_ = nullptr;

    // UX-R2 fix: 子页切换统一为 TeachingSubPageBar 组件（替代手写 5 按钮互斥逻辑）
    TeachingSubPageBar* subPageBar_ = nullptr;
    QStackedWidget* stack_ = nullptr; // 由 TeachingSubPageBar 持有，此处保留指针便于访问

    // 子页 1：NaN-boxing
    QListWidget* nanBoxList_ = nullptr;
    QTableWidget* nanBoxBitTable_ = nullptr; // 64 位分段显示
    QTextBrowser* nanBoxDesc_ = nullptr;
    // R113 D 项：NaN-box 交互式编辑器（子页 1 增强）
    QComboBox* nanBoxCustomTypeCombo_ = nullptr;   // 类型选择：int/float/bool/null/hex
    QLineEdit* nanBoxCustomValueEdit_ = nullptr;   // 值输入
    QPushButton* nanBoxCustomEncodeBtn_ = nullptr; // 编码按钮
    std::vector<NaNBoundingBox> customBoxes_;      // 用户自定义编码结果（动态追加到列表）

    // 子页 2：RefCounted
    QListWidget* refCountScenarioList_ = nullptr;
    QTextBrowser* refCountDesc_ = nullptr;
    QTableWidget* refCountStepTable_ = nullptr; // 步骤表

    // 子页 3：GC
    QTextBrowser* gcBrowser_ = nullptr;
    QLabel* gcTrackedCountLabel_ = nullptr;
    QPushButton* gcRefreshBtn_ = nullptr;

    // 子页 4：实时动画（与 VM 单步联动）
    QLabel* animStatusLabel_ = nullptr;               // VM 状态：运行中 / 已暂停 / 未初始化
    QLabel* animOpCodeLabel_ = nullptr;               // 当前 OpCode 名
    QLabel* animStackDepthLabel_ = nullptr;           // 操作数栈深度
    QLabel* animFrameCountLabel_ = nullptr;           // 栈帧数
    QLabel* animGcTrackedLabel_ = nullptr;            // GC tracked 节点数
    QLabel* animGcStatsLabel_ = nullptr;              // R113 C 项：GC 阶段 + 上次结果 + 累计次数
    QTableWidget* heapObjectTable_ = nullptr;         // 堆对象表：地址 / 类型 / refCount / 字段数
    QTextBrowser* heapObjectDetailBrowser_ = nullptr; // R113 B 项：堆对象详情（ClosureData upvalue 生命周期）
    QLabel* gcPhaseLabel_ = nullptr;                  // GC 阶段说明（来自 MemoryAnimLibrary）
    QTextBrowser* gcPhaseBrowser_ = nullptr;          // 4 阶段说明详细展示
    QPushButton* animRefreshBtn_ = nullptr;           // 手动刷新按钮
    QPushButton* animAutoRefreshBtn_ = nullptr;       // 切换自动刷新（500ms）
    QTimer* animTimer_ = nullptr;                     // 500ms 自动刷新定时器

    // 子页 5：RegisterVM 寄存器帧可视化（R113 A 项）
    // 基于 getVmStack()（RegisterVM 模式下返回当前帧寄存器窗口）+ getVmCurrentIP()
    // + getVmCallStack() 实现。栈式 VM 模式下显示提示并禁用刷新。
    QLabel* regVmModeLabel_ = nullptr;            // VM 模式：RegisterVM / StackVM
    QLabel* regVmFrameInfoLabel_ = nullptr;       // 当前帧：函数名/ip/帧索引/激活寄存器数
    QTableWidget* regVmCallStackTable_ = nullptr; // 调用栈：depth/functionName/line/locals 数
    QTableWidget* regVmRegisterTable_ = nullptr;  // 当前帧寄存器：编号/类型/值/地址
    QLabel* regVmHintLabel_ = nullptr;            // 教学提示文字
    QPushButton* regVmRefreshBtn_ = nullptr;      // 手动刷新
    QPushButton* regVmAutoRefreshBtn_ = nullptr;  // 自动刷新切换

    // R113 B 项：堆对象详情交互状态
    // currentHeapValues_ 与 heapObjectTable_ 的行号一一对应，保存选中行对应的
    // 裸 Value*（指向 controller_ 返回的 stack/globals 副本）。refreshAnimState
    // 每次刷新会重建 currentHeapValues_，并通过保留选中行号刷新详情浏览器。
    std::vector<Value> currentHeapValues_;

    // 构造辅助
    void buildNanBoxPage(QWidget* host);
    void buildRefCountPage(QWidget* host);
    void buildGcPage(QWidget* host);
    void buildAnimPage(QWidget* host);  // P4-4: 第 4 子页
    void buildRegVmPage(QWidget* host); // R113 A 项: 第 5 子页

    // 数据填充
    void populateNanBoxList();
    void populateNanBoxDetail(int index);
    void populateRefCountScenarios();
    void populateRefCountDetail(int index);
    void populateGcPhases();
    void refreshGcStats();
    void refreshAnimState();  // P4-4: 刷新第 4 子页状态
    void refreshRegVmState(); // R113 A 项: 刷新第 5 子页状态

    // R113 D 项：NaN-box 交互式编辑器
    /// 根据当前类型+值输入编码 NaN-box，将结果作为自定义示例追加到列表并选中显示。
    void onNanBoxCustomEncode();
    /// 将单个 NaNBoundingBox 渲染到位图表格 + 说明浏览器（供示例与自定义编码共用）。
    void renderNanBoxDetail(const NaNBoundingBox& b);

    // R113 B 项：闭包 upvalue 生命周期增强
    /// 堆对象表行选中回调——根据行号取出 currentHeapValues_ 中的 Value*，
    /// 调用 renderHeapObjectDetail 渲染详情到 heapObjectDetailBrowser_。
    void onHeapObjectSelected(int row);
    /// 按 ValueType 分发渲染堆对象详情。ClosureData 走 upvalue 生命周期详情；
    /// 其他堆类型给出简短结构摘要（地址/类型/refCount/关键字段），保持 UI 一致性。
    void renderHeapObjectDetail(const Value& v);
};
