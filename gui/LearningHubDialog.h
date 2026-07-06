#pragma once

// ============================================================
// LearningHubDialog.h — 学习中心对话框（教学面板统一入口）
// ------------------------------------------------------------
// 解决问题：原视图菜单 24 项平铺，新手无从下手。
// 改为 ActivityBar 「学习」入口 → 弹出本对话框 → 4 分组卡片导航。
//
// 4 分组：
//   1. 入门导览  — WelcomeWizard / CodeJourney / LearningPath
//   2. 编译前端  — Pipeline / TokenPuzzle / AstBuilderToy / SyntaxExplorer
//   3. 执行引擎  — BackendCompare / VmStackSandbox / MemoryModel /
//                  IRTransform / BytecodeTrace / CallStack /
//                  VariableInspector / BreakpointCondition
//   4. 深入实战  — BugHunt / ExceptionFlow / ClosureInspector /
//                  ProfileDashboard / LabManual
// ============================================================

#include <QDialog>
#include <QString>

class QVBoxLayout;
class QPushButton;

class LearningHubDialog : public QDialog {
    Q_OBJECT
public:
    explicit LearningHubDialog(QWidget* parent = nullptr);

signals:
    /// 请求打开某个教学面板（panelId 见实现中的 kPanels 表）
    void panelRequested(const QString& panelId);
    /// 请求重新弹出欢迎向导
    void welcomeWizardRequested();

private:
    struct PanelEntry {
        QString id;
        QString name;
        QString description;
        QString iconHint;  // 简短图标提示（emoji 或字符），用于按钮前缀
    };

    void buildGroup(QVBoxLayout* parentLayout,
                    const QString& groupTitle,
                    const QString& groupSubtitle,
                    const std::vector<PanelEntry>& entries);
};
