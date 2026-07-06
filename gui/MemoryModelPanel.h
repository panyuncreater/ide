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

#include <QWidget>
#include <QStackedWidget>
#include <QLabel>
#include <QPushButton>
#include <QListWidget>
#include <QTextBrowser>
#include <QTableWidget>
#include <QTimer>
#include <string>
#include <vector>

class IdeController;

// ---- 教学场景库数据结构 ----

/// 单个 NaN-boxing 编码示例
struct NaNBoundingBox {
    std::string id;          // 示例 ID（如 "int-inline"）
    std::string title;       // 显示名
    std::string description; // 说明文字
    std::string sourceExpr;  // 表达式源码（如 "42" / "3.14" / "true" / "null"）
    uint64_t    bits = 0;    // 编码后的 64 位原始位
    int64_t     intVal = 0;  // 解码后的 int 值（若 applicable）
    double      floatVal = 0.0;  // 解码后的 float 值（若 applicable）
    bool        isInt = false;
    bool        isFloat = false;
    bool        isBool = false;
    bool        isNull = false;
    bool        isPointer = false;
};

/// 单个 RefCounted 场景步骤（构造 / 拷贝 / release / COW detach）
struct RefCountStep {
    std::string action;       // 动作描述（如 "var a = [1,2,3]"）
    int         refCount = 1; // 操作后 refCount
    std::string note;        // 备注（如 "构造新对象 refCount=1"）
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
    std::string phase;          // 阶段 ID: "mark" / "sweep" / "reset" / "idle"
    std::string description;    // 阶段说明
    std::string color;          // 显示颜色提示（用于 paintEvent 自绘）
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

    /// 绑定到 IdeController（保留接口以与其它面板一致，本面板主要消费静态库）
    void setController(IdeController* controller) { controller_ = controller; }

signals:
    /// 请求加载样例代码到主编辑器（与第一波教学面板信号一致）
    void loadSampleRequested(const QString& code);

protected:
    /// 面板显示时恢复第 4 子页动画 QTimer（若已开启），隐藏时停止
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    IdeController* controller_ = nullptr;

    // 子页切换按钮（4 个子页）
    QPushButton* pageNanBoxBtn_   = nullptr;
    QPushButton* pageRefCountBtn_ = nullptr;
    QPushButton* pageGcBtn_       = nullptr;
    QPushButton* pageAnimBtn_     = nullptr;  // P4-4: 第 4 子页"实时动画"
    QStackedWidget* stack_         = nullptr;

    // 子页 1：NaN-boxing
    QListWidget* nanBoxList_       = nullptr;
    QTableWidget* nanBoxBitTable_  = nullptr;  // 64 位分段显示
    QTextBrowser* nanBoxDesc_      = nullptr;

    // 子页 2：RefCounted
    QListWidget* refCountScenarioList_ = nullptr;
    QTextBrowser* refCountDesc_        = nullptr;
    QTableWidget* refCountStepTable_   = nullptr;  // 步骤表

    // 子页 3：GC
    QTextBrowser* gcBrowser_       = nullptr;
    QLabel* gcTrackedCountLabel_   = nullptr;
    QPushButton* gcRefreshBtn_     = nullptr;

    // 子页 4：实时动画（与 VM 单步联动）
    QLabel* animStatusLabel_         = nullptr;  // VM 状态：运行中 / 已暂停 / 未初始化
    QLabel* animOpCodeLabel_         = nullptr;  // 当前 OpCode 名
    QLabel* animStackDepthLabel_     = nullptr;  // 操作数栈深度
    QLabel* animFrameCountLabel_     = nullptr;  // 栈帧数
    QLabel* animGcTrackedLabel_      = nullptr;  // GC tracked 节点数
    QTableWidget* heapObjectTable_   = nullptr;  // 堆对象表：地址 / 类型 / refCount / 字段数
    QLabel* gcPhaseLabel_            = nullptr;  // GC 阶段说明（来自 MemoryAnimLibrary）
    QTextBrowser* gcPhaseBrowser_    = nullptr;  // 4 阶段说明详细展示
    QPushButton* animRefreshBtn_     = nullptr;  // 手动刷新按钮
    QPushButton* animAutoRefreshBtn_ = nullptr;  // 切换自动刷新（500ms）
    QTimer* animTimer_               = nullptr;  // 500ms 自动刷新定时器

    // 构造辅助
    void buildNanBoxPage(QWidget* host);
    void buildRefCountPage(QWidget* host);
    void buildGcPage(QWidget* host);
    void buildAnimPage(QWidget* host);  // P4-4: 第 4 子页

    // 数据填充
    void populateNanBoxList();
    void populateNanBoxDetail(int index);
    void populateRefCountScenarios();
    void populateRefCountDetail(int index);
    void populateGcPhases();
    void refreshGcStats();
    void refreshAnimState();  // P4-4: 刷新第 4 子页状态
};
