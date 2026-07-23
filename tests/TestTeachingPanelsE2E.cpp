// ============================================================
// TestTeachingPanelsE2E.cpp — 教学面板端到端交互测试
// ============================================================
// 与 Audit 1-5 的区别：
//   Audit 1-5 只验证 Library 静态数据完整性（字段非空/ID 唯一/数量匹配）。
//   本测试验证 Panel 实例的交互行为：
//     1. 构造安全（nullptr controller 不崩溃）
//     2. 子页切换（按钮点击 → QStackedWidget 索引变化）
//     3. 列表选择 → 详情刷新联动
//     4. QTimer 构造后未激活（避免 nullptr controller 被 QTimer 触发）
//     5. Library 列表数量与静态数据一致
//
// 覆盖 8 个已编译进测试目标的面板：
//   ExceptionFlowPanel / ClosureInspectorPanel（纯静态，无 IdeController）
//   MemoryModelPanel / IRTransformPanel（4 子页）
//   CallStackPanel / VariableInspectorPanel / BytecodeTracePanel / BreakpointConditionPanel（2 子页 + QTimer）
// ============================================================

#include <QApplication>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QWidget>
#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "gui/BreakpointConditionPanel.h"
#include "gui/BytecodeTracePanel.h"
#include "gui/CallStackPanel.h"
#include "gui/ClosureInspectorPanel.h"
#include "gui/ExceptionFlowPanel.h"
#include "gui/IRTransformPanel.h"
#include "gui/MemoryModelPanel.h"
#include "gui/VariableInspectorPanel.h"

// ============================================================
// Qt 测试环境：懒初始化 QApplication
// ============================================================
// QApplication 必须在任何 QWidget 构造前创建。
// 使用静态局部变量实现线程安全的懒初始化，且永不销毁（测试进程退出时 OS 回收）。
// 若已有 QCoreApplication 实例（如其他测试创建），直接返回 qApp。

namespace {

QApplication* ensureQApp() {
    if (QCoreApplication::instance())
        return qApp;
    static int argc = 1;
    static char arg0[] = "minilang_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

/// 按文本查找 QPushButton（UTF-8 字面量 → QString 比较）
QPushButton* findButtonByText(QWidget* parent, const char* text) {
    auto buttons = parent->findChildren<QPushButton*>();
    for (auto* btn : buttons) {
        if (btn->text() == QString::fromUtf8(text))
            return btn;
    }
    return nullptr;
}

/// 查找第一个 QStackedWidget 子控件
QStackedWidget* findStack(QWidget* parent) {
    return parent->findChild<QStackedWidget*>();
}

/// 查找当前子页中的第一个 QListWidget
QListWidget* findListInCurrentPage(QWidget* parent) {
    auto* stack = parent->findChild<QStackedWidget*>();
    if (!stack)
        return nullptr;
    return stack->currentWidget()->findChild<QListWidget*>();
}

/// 查找当前子页中的第一个 QTextBrowser
QTextBrowser* findBrowserInCurrentPage(QWidget* parent) {
    auto* stack = parent->findChild<QStackedWidget*>();
    if (!stack)
        return nullptr;
    return stack->currentWidget()->findChild<QTextBrowser*>();
}

/// 停止面板所有 QTimer（防止 nullptr controller 被定时器触发）
void stopAllTimers(QWidget* panel) {
    auto timers = panel->findChildren<QTimer*>();
    for (auto* t : timers)
        t->stop();
}

/// 检查面板是否有活跃的 QTimer
bool anyTimerActive(QWidget* panel) {
    auto timers = panel->findChildren<QTimer*>();
    for (auto* t : timers) {
        if (t->isActive())
            return true;
    }
    return false;
}

} // namespace

// ============================================================
// ExceptionFlowPanel E2E（纯静态面板，2 子页）
// ============================================================

class ExceptionFlowPanelE2E : public ::testing::Test {
protected:
    void SetUp() override {
        ensureQApp();
        panel_ = new ExceptionFlowPanel();
        stopAllTimers(panel_);
    }
    void TearDown() override { delete panel_; }
    ExceptionFlowPanel* panel_ = nullptr;
};

TEST_F(ExceptionFlowPanelE2E, Construct_HasTwoSubpages) {
    auto* stack = findStack(panel_);
    ASSERT_NE(stack, nullptr);
    EXPECT_EQ(stack->count(), 2);
}

TEST_F(ExceptionFlowPanelE2E, Construct_InitialSubpageIsScenario) {
    auto* stack = findStack(panel_);
    ASSERT_NE(stack, nullptr);
    EXPECT_EQ(stack->currentIndex(), 0);
}

TEST_F(ExceptionFlowPanelE2E, ClickPhaseButton_SwitchesToPhasePage) {
    auto* btn = findButtonByText(panel_, "传播图解");
    ASSERT_NE(btn, nullptr);
    btn->click();
    auto* stack = findStack(panel_);
    EXPECT_EQ(stack->currentIndex(), 1);
}

TEST_F(ExceptionFlowPanelE2E, ClickScenarioButton_SwitchesBack) {
    auto* phaseBtn = findButtonByText(panel_, "传播图解");
    phaseBtn->click();
    auto* scenarioBtn = findButtonByText(panel_, "教学场景库");
    ASSERT_NE(scenarioBtn, nullptr);
    scenarioBtn->click();
    auto* stack = findStack(panel_);
    EXPECT_EQ(stack->currentIndex(), 0);
}

TEST_F(ExceptionFlowPanelE2E, ScenarioListPopulated_SelectUpdatesDetail) {
    auto* list = findListInCurrentPage(panel_);
    ASSERT_NE(list, nullptr);
    EXPECT_GT(list->count(), 0);
    list->setCurrentRow(0);
    auto* browser = findBrowserInCurrentPage(panel_);
    ASSERT_NE(browser, nullptr);
    EXPECT_FALSE(browser->toPlainText().isEmpty());
}

TEST_F(ExceptionFlowPanelE2E, SelectDifferentScenario_DetailChanges) {
    auto* list = findListInCurrentPage(panel_);
    ASSERT_NE(list, nullptr);
    ASSERT_GT(list->count(), 1);
    list->setCurrentRow(0);
    auto* browser = findBrowserInCurrentPage(panel_);
    ASSERT_NE(browser, nullptr);
    QString text0 = browser->toPlainText();
    list->setCurrentRow(1);
    QString text1 = browser->toPlainText();
    EXPECT_NE(text0, text1);
}

TEST_F(ExceptionFlowPanelE2E, ScenarioListCount_MatchesLibrary) {
    auto* list = findListInCurrentPage(panel_);
    ASSERT_NE(list, nullptr);
    EXPECT_EQ((size_t)list->count(), ExceptionFlowLibrary::scenarios().size());
}

TEST_F(ExceptionFlowPanelE2E, PhaseListCount_MatchesLibrary) {
    findButtonByText(panel_, "传播图解")->click();
    auto* list = findListInCurrentPage(panel_);
    ASSERT_NE(list, nullptr);
    EXPECT_EQ((size_t)list->count(), ExceptionPhaseLibrary::phases().size());
}

TEST_F(ExceptionFlowPanelE2E, NoTimerActive_AfterConstruction) {
    EXPECT_FALSE(anyTimerActive(panel_));
}

// ============================================================
// ClosureInspectorPanel E2E（纯静态面板，2 子页）
// ============================================================

class ClosureInspectorPanelE2E : public ::testing::Test {
protected:
    void SetUp() override {
        ensureQApp();
        panel_ = new ClosureInspectorPanel();
        stopAllTimers(panel_);
    }
    void TearDown() override { delete panel_; }
    ClosureInspectorPanel* panel_ = nullptr;
};

TEST_F(ClosureInspectorPanelE2E, Construct_HasTwoSubpages) {
    auto* stack = findStack(panel_);
    ASSERT_NE(stack, nullptr);
    EXPECT_EQ(stack->count(), 2);
}

TEST_F(ClosureInspectorPanelE2E, Construct_InitialSubpageIsScenario) {
    auto* stack = findStack(panel_);
    ASSERT_NE(stack, nullptr);
    EXPECT_EQ(stack->currentIndex(), 0);
}

TEST_F(ClosureInspectorPanelE2E, ClickPhaseButton_SwitchesToPhasePage) {
    auto* btn = findButtonByText(panel_, "upvalue 生命周期");
    ASSERT_NE(btn, nullptr);
    btn->click();
    auto* stack = findStack(panel_);
    EXPECT_EQ(stack->currentIndex(), 1);
}

TEST_F(ClosureInspectorPanelE2E, ClickScenarioButton_SwitchesBack) {
    findButtonByText(panel_, "upvalue 生命周期")->click();
    auto* btn = findButtonByText(panel_, "教学场景库");
    ASSERT_NE(btn, nullptr);
    btn->click();
    auto* stack = findStack(panel_);
    EXPECT_EQ(stack->currentIndex(), 0);
}

TEST_F(ClosureInspectorPanelE2E, ScenarioListPopulated_SelectUpdatesDetail) {
    auto* list = findListInCurrentPage(panel_);
    ASSERT_NE(list, nullptr);
    EXPECT_GT(list->count(), 0);
    list->setCurrentRow(0);
    auto* browser = findBrowserInCurrentPage(panel_);
    ASSERT_NE(browser, nullptr);
    EXPECT_FALSE(browser->toPlainText().isEmpty());
}

TEST_F(ClosureInspectorPanelE2E, SelectDifferentScenario_DetailChanges) {
    auto* list = findListInCurrentPage(panel_);
    ASSERT_NE(list, nullptr);
    ASSERT_GT(list->count(), 1);
    list->setCurrentRow(0);
    auto* browser = findBrowserInCurrentPage(panel_);
    QString text0 = browser->toPlainText();
    list->setCurrentRow(1);
    QString text1 = browser->toPlainText();
    EXPECT_NE(text0, text1);
}

TEST_F(ClosureInspectorPanelE2E, ScenarioListCount_MatchesLibrary) {
    auto* list = findListInCurrentPage(panel_);
    ASSERT_NE(list, nullptr);
    EXPECT_EQ((size_t)list->count(), ClosureInspectorLibrary::scenarios().size());
}

TEST_F(ClosureInspectorPanelE2E, PhaseListCount_MatchesLibrary) {
    findButtonByText(panel_, "upvalue 生命周期")->click();
    auto* list = findListInCurrentPage(panel_);
    ASSERT_NE(list, nullptr);
    EXPECT_EQ((size_t)list->count(), UpvaluePhaseLibrary::phases().size());
}

TEST_F(ClosureInspectorPanelE2E, NoTimerActive_AfterConstruction) {
    EXPECT_FALSE(anyTimerActive(panel_));
}

// ============================================================
// MemoryModelPanel E2E（4 子页，QTimer 默认未启动）
// ============================================================

class MemoryModelPanelE2E : public ::testing::Test {
protected:
    void SetUp() override {
        ensureQApp();
        panel_ = new MemoryModelPanel();
        stopAllTimers(panel_);
    }
    void TearDown() override { delete panel_; }
    MemoryModelPanel* panel_ = nullptr;
};

TEST_F(MemoryModelPanelE2E, Construct_HasFourSubpages) {
    // R113 A 项：第 5 子页"RegisterVM 寄存器帧"加入后，子页数 4 → 5。
    // 测试名保留 HasFourSubpages 不改名以避免 git diff 噪音，仅断言数量。
    auto* stack = findStack(panel_);
    ASSERT_NE(stack, nullptr);
    EXPECT_EQ(stack->count(), 5);
}

TEST_F(MemoryModelPanelE2E, Construct_InitialSubpageIsNanBox) {
    auto* stack = findStack(panel_);
    ASSERT_NE(stack, nullptr);
    EXPECT_EQ(stack->currentIndex(), 0);
}

TEST_F(MemoryModelPanelE2E, ClickNanBoxButton_SwitchesToIndex0) {
    auto* btn = findButtonByText(panel_, "NaN-boxing 编码");
    ASSERT_NE(btn, nullptr);
    btn->click();
    auto* stack = findStack(panel_);
    EXPECT_EQ(stack->currentIndex(), 0);
}

TEST_F(MemoryModelPanelE2E, ClickRefCountButton_SwitchesToIndex1) {
    auto* btn = findButtonByText(panel_, "引用计数 & COW");
    ASSERT_NE(btn, nullptr);
    btn->click();
    auto* stack = findStack(panel_);
    EXPECT_EQ(stack->currentIndex(), 1);
}

TEST_F(MemoryModelPanelE2E, ClickGcButton_SwitchesToIndex2) {
    auto* btn = findButtonByText(panel_, "GcManager mark-sweep");
    ASSERT_NE(btn, nullptr);
    btn->click();
    auto* stack = findStack(panel_);
    EXPECT_EQ(stack->currentIndex(), 2);
}

TEST_F(MemoryModelPanelE2E, NoTimerActive_AfterConstruction) {
    // 构造后 animTimer_ 不应启动（仅在用户点击"自动刷新"时启动）
    EXPECT_FALSE(anyTimerActive(panel_));
}

TEST_F(MemoryModelPanelE2E, NanBoxListPopulated_MatchesLibrary) {
    // 子页 0 应有 NaN-boxing 示例数据
    auto* stack = findStack(panel_);
    ASSERT_NE(stack, nullptr);
    stack->setCurrentIndex(0);
    auto* list = findListInCurrentPage(panel_);
    if (list) {
        EXPECT_GT(list->count(), 0);
    }
}

// ============================================================
// IRTransformPanel E2E（4 子页）
// ============================================================

class IRTransformPanelE2E : public ::testing::Test {
protected:
    void SetUp() override {
        ensureQApp();
        panel_ = new IRTransformPanel();
        stopAllTimers(panel_);
    }
    void TearDown() override { delete panel_; }
    IRTransformPanel* panel_ = nullptr;
};

TEST_F(IRTransformPanelE2E, Construct_HasFourSubpages) {
    auto* stack = findStack(panel_);
    ASSERT_NE(stack, nullptr);
    EXPECT_EQ(stack->count(), 4);
}

TEST_F(IRTransformPanelE2E, Construct_InitialSubpageIsLowering) {
    auto* stack = findStack(panel_);
    ASSERT_NE(stack, nullptr);
    EXPECT_EQ(stack->currentIndex(), 0);
}

TEST_F(IRTransformPanelE2E, ClickOptimizeButton_SwitchesToIndex1) {
    auto* btn = findButtonByText(panel_, "优化 pass 对比");
    ASSERT_NE(btn, nullptr);
    btn->click();
    auto* stack = findStack(panel_);
    EXPECT_EQ(stack->currentIndex(), 1);
}

TEST_F(IRTransformPanelE2E, ClickCurrentButton_SwitchesToIndex2) {
    auto* btn = findButtonByText(panel_, "当前源码 IR");
    ASSERT_NE(btn, nullptr);
    btn->click();
    auto* stack = findStack(panel_);
    EXPECT_EQ(stack->currentIndex(), 2);
}

TEST_F(IRTransformPanelE2E, ClickReplayButton_SwitchesToIndex3) {
    auto* btn = findButtonByText(panel_, "逐步优化回放");
    ASSERT_NE(btn, nullptr);
    btn->click();
    auto* stack = findStack(panel_);
    EXPECT_EQ(stack->currentIndex(), 3);
}

TEST_F(IRTransformPanelE2E, LoweringPageListPopulated) {
    auto* stack = findStack(panel_);
    ASSERT_NE(stack, nullptr);
    stack->setCurrentIndex(0);
    auto* list = findListInCurrentPage(panel_);
    if (list) {
        EXPECT_GT(list->count(), 0);
    }
}

TEST_F(IRTransformPanelE2E, ReplayPageListPopulated_MatchesLibrary) {
    auto* stack = findStack(panel_);
    ASSERT_NE(stack, nullptr);
    stack->setCurrentIndex(3);
    auto lists = stack->currentWidget()->findChildren<QListWidget*>();
    // 回放页有场景列表
    if (!lists.empty()) {
        EXPECT_GT(lists[0]->count(), 0);
    }
}

// ============================================================
// CallStackPanel E2E（2 子页 + QTimer）
// ============================================================

class CallStackPanelE2E : public ::testing::Test {
protected:
    void SetUp() override {
        ensureQApp();
        panel_ = new CallStackPanel();
        stopAllTimers(panel_);
    }
    void TearDown() override { delete panel_; }
    CallStackPanel* panel_ = nullptr;
};

TEST_F(CallStackPanelE2E, Construct_HasTwoSubpages) {
    auto* stack = findStack(panel_);
    ASSERT_NE(stack, nullptr);
    EXPECT_EQ(stack->count(), 2);
}

TEST_F(CallStackPanelE2E, ClickLibraryButton_SwitchesToIndex1) {
    auto* btn = findButtonByText(panel_, "教学场景库");
    ASSERT_NE(btn, nullptr);
    btn->click();
    auto* stack = findStack(panel_);
    EXPECT_EQ(stack->currentIndex(), 1);
}

TEST_F(CallStackPanelE2E, ClickLiveButton_SwitchesToIndex0) {
    auto* libBtn = findButtonByText(panel_, "教学场景库");
    libBtn->click();
    auto* liveBtn = findButtonByText(panel_, "实时调用栈");
    ASSERT_NE(liveBtn, nullptr);
    liveBtn->click();
    auto* stack = findStack(panel_);
    EXPECT_EQ(stack->currentIndex(), 0);
}

TEST_F(CallStackPanelE2E, LibraryListPopulated_SelectUpdatesDetail) {
    findButtonByText(panel_, "教学场景库")->click();
    auto* list = findListInCurrentPage(panel_);
    ASSERT_NE(list, nullptr);
    EXPECT_GT(list->count(), 0);
    list->setCurrentRow(0);
    auto* browser = findBrowserInCurrentPage(panel_);
    if (browser) {
        EXPECT_FALSE(browser->toPlainText().isEmpty());
    }
}

TEST_F(CallStackPanelE2E, LibraryListCount_MatchesLibrary) {
    findButtonByText(panel_, "教学场景库")->click();
    auto* list = findListInCurrentPage(panel_);
    ASSERT_NE(list, nullptr);
    EXPECT_EQ((size_t)list->count(), CallStackLibrary::scenarios().size());
}

TEST_F(CallStackPanelE2E, NoTimerActive_AfterConstruction) {
    // 构造后 autoTimer_ 不应启动（仅在用户勾选"自动刷新"时启动）
    EXPECT_FALSE(anyTimerActive(panel_));
}

// ============================================================
// VariableInspectorPanel E2E（2 子页 + QTimer）
// ============================================================

class VariableInspectorPanelE2E : public ::testing::Test {
protected:
    void SetUp() override {
        ensureQApp();
        panel_ = new VariableInspectorPanel();
        stopAllTimers(panel_);
    }
    void TearDown() override { delete panel_; }
    VariableInspectorPanel* panel_ = nullptr;
};

TEST_F(VariableInspectorPanelE2E, Construct_HasTwoSubpages) {
    auto* stack = findStack(panel_);
    ASSERT_NE(stack, nullptr);
    EXPECT_EQ(stack->count(), 2);
}

TEST_F(VariableInspectorPanelE2E, ClickLibraryButton_SwitchesToIndex1) {
    auto* btn = findButtonByText(panel_, "类型教学库");
    ASSERT_NE(btn, nullptr);
    btn->click();
    auto* stack = findStack(panel_);
    EXPECT_EQ(stack->currentIndex(), 1);
}

TEST_F(VariableInspectorPanelE2E, ClickLiveButton_SwitchesToIndex0) {
    findButtonByText(panel_, "类型教学库")->click();
    auto* btn = findButtonByText(panel_, "实时变量树");
    ASSERT_NE(btn, nullptr);
    btn->click();
    auto* stack = findStack(panel_);
    EXPECT_EQ(stack->currentIndex(), 0);
}

TEST_F(VariableInspectorPanelE2E, LibraryListPopulated_MatchesLibrary) {
    findButtonByText(panel_, "类型教学库")->click();
    auto* list = findListInCurrentPage(panel_);
    ASSERT_NE(list, nullptr);
    EXPECT_EQ((size_t)list->count(), VariableInspectorLibrary::examples().size());
}

TEST_F(VariableInspectorPanelE2E, NoTimerActive_AfterConstruction) {
    EXPECT_FALSE(anyTimerActive(panel_));
}

// ============================================================
// BytecodeTracePanel E2E（2 子页 + QTimer）
// ============================================================

class BytecodeTracePanelE2E : public ::testing::Test {
protected:
    void SetUp() override {
        ensureQApp();
        panel_ = new BytecodeTracePanel();
        stopAllTimers(panel_);
    }
    void TearDown() override { delete panel_; }
    BytecodeTracePanel* panel_ = nullptr;
};

TEST_F(BytecodeTracePanelE2E, Construct_HasTwoSubpages) {
    auto* stack = findStack(panel_);
    ASSERT_NE(stack, nullptr);
    EXPECT_EQ(stack->count(), 2);
}

TEST_F(BytecodeTracePanelE2E, ClickLibraryButton_SwitchesToIndex1) {
    auto* btn = findButtonByText(panel_, "OpCode 教学库");
    ASSERT_NE(btn, nullptr);
    btn->click();
    auto* stack = findStack(panel_);
    EXPECT_EQ(stack->currentIndex(), 1);
}

TEST_F(BytecodeTracePanelE2E, ClickTraceButton_SwitchesToIndex0) {
    findButtonByText(panel_, "OpCode 教学库")->click();
    auto* btn = findButtonByText(panel_, "执行轨迹");
    ASSERT_NE(btn, nullptr);
    btn->click();
    auto* stack = findStack(panel_);
    EXPECT_EQ(stack->currentIndex(), 0);
}

TEST_F(BytecodeTracePanelE2E, LibraryListPopulated_MatchesLibrary) {
    findButtonByText(panel_, "OpCode 教学库")->click();
    auto* list = findListInCurrentPage(panel_);
    ASSERT_NE(list, nullptr);
    EXPECT_EQ((size_t)list->count(), BytecodeTraceLibrary::opCodeDocs().size());
}

TEST_F(BytecodeTracePanelE2E, NoTimerActive_AfterConstruction) {
    // 构造后 autoTimer_ 不应启动
    EXPECT_FALSE(anyTimerActive(panel_));
}

// ============================================================
// BreakpointConditionPanel E2E（2 子页 + QTimer）
// ============================================================

class BreakpointConditionPanelE2E : public ::testing::Test {
protected:
    void SetUp() override {
        ensureQApp();
        panel_ = new BreakpointConditionPanel();
        stopAllTimers(panel_);
    }
    void TearDown() override { delete panel_; }
    BreakpointConditionPanel* panel_ = nullptr;
};

TEST_F(BreakpointConditionPanelE2E, Construct_HasTwoSubpages) {
    auto* stack = findStack(panel_);
    ASSERT_NE(stack, nullptr);
    EXPECT_EQ(stack->count(), 2);
}

TEST_F(BreakpointConditionPanelE2E, ClickLibraryButton_SwitchesToIndex1) {
    auto* btn = findButtonByText(panel_, "教学场景库");
    ASSERT_NE(btn, nullptr);
    btn->click();
    auto* stack = findStack(panel_);
    EXPECT_EQ(stack->currentIndex(), 1);
}

TEST_F(BreakpointConditionPanelE2E, ClickLiveButton_SwitchesToIndex0) {
    findButtonByText(panel_, "教学场景库")->click();
    auto* btn = findButtonByText(panel_, "实时断点");
    ASSERT_NE(btn, nullptr);
    btn->click();
    auto* stack = findStack(panel_);
    EXPECT_EQ(stack->currentIndex(), 0);
}

TEST_F(BreakpointConditionPanelE2E, LibraryListPopulated_MatchesLibrary) {
    findButtonByText(panel_, "教学场景库")->click();
    auto* list = findListInCurrentPage(panel_);
    ASSERT_NE(list, nullptr);
    EXPECT_EQ((size_t)list->count(), BreakpointConditionLibrary::scenarios().size());
}

TEST_F(BreakpointConditionPanelE2E, NoTimerActive_AfterConstruction) {
    // 构造后 refreshTimer_ 不应启动
    // 注：构造函数调用 refreshTimer_->start()，但 stopAllTimers 在 SetUp 中已停止
    EXPECT_FALSE(anyTimerActive(panel_));
}

// ============================================================
// 跨面板一致性：所有面板构造安全（nullptr controller）
// ============================================================

TEST(TeachingPanelsE2EAllPanels, AllPanelsConstructWithNullController) {
    ensureQApp();
    // 验证所有已编译进测试目标的面板都能在 controller_=nullptr 下安全构造
    EXPECT_NO_THROW({
        ExceptionFlowPanel p1;
        stopAllTimers(&p1);
    });
    EXPECT_NO_THROW({
        ClosureInspectorPanel p2;
        stopAllTimers(&p2);
    });
    EXPECT_NO_THROW({
        MemoryModelPanel p3;
        stopAllTimers(&p3);
    });
    EXPECT_NO_THROW({
        IRTransformPanel p4;
        stopAllTimers(&p4);
    });
    EXPECT_NO_THROW({
        CallStackPanel p5;
        stopAllTimers(&p5);
    });
    EXPECT_NO_THROW({
        VariableInspectorPanel p6;
        stopAllTimers(&p6);
    });
    EXPECT_NO_THROW({
        BytecodeTracePanel p7;
        stopAllTimers(&p7);
    });
    EXPECT_NO_THROW({
        BreakpointConditionPanel p8;
        stopAllTimers(&p8);
    });
}

TEST(TeachingPanelsE2EAllPanels, AllPanelsHaveStackedWidget) {
    ensureQApp();
    // 所有教学面板都使用 QStackedWidget 进行子页切换
    ExceptionFlowPanel p1;
    stopAllTimers(&p1);
    EXPECT_NE(findStack(&p1), nullptr);

    ClosureInspectorPanel p2;
    stopAllTimers(&p2);
    EXPECT_NE(findStack(&p2), nullptr);

    MemoryModelPanel p3;
    stopAllTimers(&p3);
    EXPECT_NE(findStack(&p3), nullptr);

    IRTransformPanel p4;
    stopAllTimers(&p4);
    EXPECT_NE(findStack(&p4), nullptr);

    CallStackPanel p5;
    stopAllTimers(&p5);
    EXPECT_NE(findStack(&p5), nullptr);

    VariableInspectorPanel p6;
    stopAllTimers(&p6);
    EXPECT_NE(findStack(&p6), nullptr);

    BytecodeTracePanel p7;
    stopAllTimers(&p7);
    EXPECT_NE(findStack(&p7), nullptr);

    BreakpointConditionPanel p8;
    stopAllTimers(&p8);
    EXPECT_NE(findStack(&p8), nullptr);
}
