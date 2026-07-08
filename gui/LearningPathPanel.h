// ============================================================
// LearningPathPanel.h — 学习路径地图面板（功能 6）
// ------------------------------------------------------------
// 中央导航枢纽：4 阶段学习路径地图 + 进度持久化 + 当前推荐标记。
//
// UI 结构：
//   顶部：总进度条 + "总进度: XX%"
//   主体：垂直滚动的 4 阶段卡片列表
//     每个阶段：
//       - 阶段标题 + 阶段进度条
//       - 该阶段所有活动项（图标 + 标题 + 描述 + 预计耗时）
//       - 推荐活动高亮 + "← 当前推荐" 标签
//   底部：刷新按钮 + 重置进度按钮（带确认对话框）
//
// 信号：
//   activityRequested(activityId) — 点击活动项时发射，主窗口连接打开对应面板
//
// 关键架构决策：
//   - LearningPathPanel.cpp 依赖 Qt Widgets（QWidget/QPushButton 等），
//     测试目标无法链接（Qt6::Widgets 在测试目标中可用但 Panel.cpp 引入
//     IdeController 依赖会破坏独立编译）。因此 LearnerProgress.cpp
//     与 LearningPathData.cpp 拆分为独立编译单元，测试仅链接这两者。
// ============================================================

#pragma once

#include "gui/LearningPathData.h"
#include "gui/LearnerProgress.h"

#include <QWidget>
#include <QScrollArea>
#include <QProgressBar>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QFrame>
#include <QList>
#include <vector>
#include <string>

class LearningPathPanel : public QWidget {
    Q_OBJECT
public:
/// 构造学习路径面板；parent 为父控件。
    explicit LearningPathPanel(QWidget* parent = nullptr);

public slots:
    /// 刷新整个面板的显示（从 LearnerProgressStore 重新读取进度）
    void refresh();

    /// 标记指定活动为已完成（外部面板完成后调用）
    void markActivityCompleted(const QString& activityId);

signals:
    /// 请求打开某个活动对应的面板（主窗口连接后路由到具体 Panel）
    void activityRequested(const QString& activityId);

private slots:
/// 刷新按钮回调。
    void onRefresh();
/// 重置进度按钮回调。
    void onResetProgress();
/// 活动点击回调。
    void onActivityClicked(const QString& activityId);

protected:
    // M9: 键盘导航 — Up/Down 在已解锁活动行间循环，Enter 触发当前行点击
/// 键盘导航事件。
    void keyPressEvent(QKeyEvent* event) override;

private:
    // 顶部
    QProgressBar* overallProgress_ = nullptr;
    QLabel*       overallLabel_    = nullptr;
    QLabel*       stageLabel_      = nullptr;  ///< P1-2 fix: 当前阶段提示
    QLabel*       weakPointsLabel_ = nullptr;  ///< P2-3 fix (F9): 薄弱点提示
    QLabel*       remainingLabel_  = nullptr;  ///< P2-3 fix (F9): 预计剩余时间
    QLineEdit*    searchEdit_      = nullptr;  // 活动搜索过滤框

    // 主体
    QScrollArea* scrollArea_   = nullptr;
    QWidget*     scrollContent_ = nullptr;
    QVBoxLayout* stagesLayout_ = nullptr;

    // 底部
    QPushButton* refreshBtn_ = nullptr;
    QPushButton* resetBtn_   = nullptr;

    // M9: 键盘导航状态——仅收集已解锁的活动行
    QList<QPushButton*> activityRows_;
    int                 currentNavIndex_ = -1;
    QString             highlightedSavedStyle_;
/// 高亮指定活动行。
    void highlightActivityRow(int idx);

    // 搜索框防抖定时器——避免逐字符触发 refresh() 导致重建风暴
    QTimer* searchDebounceTimer_ = nullptr;

    // 阶段颜色（绿/黄/蓝/紫/红）
/// 阶段主题配色。
    static QString stageColor(int stage);
/// 阶段标题文案。
    static QString stageTitle(int stage);

    /// 构造单个阶段卡片
    QFrame* buildStageCard(int stage);

    /// 构造单个活动项行
    QWidget* buildActivityRow(const LearningActivity& activity,
                              bool unlocked, bool completed,
                              bool recommended);

    /// 收集所有活动数据
    const std::vector<LearningActivity>& allActivities() const {
        return LearningPathData::activities();
    }
};
