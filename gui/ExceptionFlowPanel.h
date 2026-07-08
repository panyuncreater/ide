/**
 * @file ExceptionFlowPanel.h
 * @brief 异常流转面板的类声明（功能：异常传播可视化）
 *
 * 提供场景与阶段两个视角的异常流程讲解。
 */
#pragma once

// ============================================================
// ExceptionFlowPanel — 异常流可视化面板（第三档 P2-3a）
// ------------------------------------------------------------
// 可视化 try/catch/throw 的异常传播路径与栈展开机制，
// 让学习者理解异常如何在调用栈中传播、跨帧搜索处理器、
// 以及 finally 语义与异常对象的传递。
//
// 两个子页：
//   1. 异常机制教学场景库：8 个典型异常场景，含源码 +
//      期望传播路径 + 栈展开步骤 + 教学注释
//   2. 异常传播图解：6 个图解条目，展示异常传播的
//      关键阶段（throw → 栈搜索 → catch 匹配 → finally → 恢复）
//
// 本面板为纯静态教学面板（Library 静态数据），不依赖
// IdeController，不需要引擎层改造。
// ============================================================

#include <QWidget>
#include <QStackedWidget>
#include <QLabel>
#include <QPushButton>
#include <QListWidget>
#include <QTextBrowser>
#include <string>
#include <vector>

// ---- 教学场景库数据结构 ----

/// 异常流教学场景
struct ExceptionScenario {
    std::string id;                  // 场景 ID
    std::string title;               // 显示名
    std::string description;         // 文字说明（异常机制）
    std::string sampleCode;          // 示例代码
    std::vector<std::string> propagationPath;  // 期望传播路径（帧序列）
    std::string teachingNote;       // 教学注释
};

/// ExceptionFlowLibrary — 静态教学场景库
class ExceptionFlowLibrary {
public:
/// 返回异常场景数据（静态数据）。
    static const std::vector<ExceptionScenario>& scenarios();
};

// ---- 异常传播图解条目 ----

/// 异常传播阶段图解
struct ExceptionPhaseDoc {
    std::string phase;       // 阶段名
    std::string category;    // 分类（throw/search/catch/finally/recovery）
    std::string description; // 文字说明
    std::string stackEffect;  // 栈效应
};

/// ExceptionPhaseLibrary — 静态传播阶段图解库
class ExceptionPhaseLibrary {
public:
/// 返回异常阶段文档（静态数据）。
    static const std::vector<ExceptionPhaseDoc>& phases();
};

// ---- 主面板 ----

class ExceptionFlowPanel : public QWidget {
    Q_OBJECT
public:
/// 构造异常流转面板；parent 为父控件。
    explicit ExceptionFlowPanel(QWidget* parent = nullptr);

signals:
/// 信号：请求载入示例代码。
    void loadSampleRequested(const QString& code);

private:
    // 顶部页面切换
    QPushButton* pageScenarioBtn_ = nullptr;
    QPushButton* pagePhaseBtn_     = nullptr;
    QStackedWidget* stack_         = nullptr;

    // 子页 1：异常机制教学场景库
    QListWidget* scenarioList_    = nullptr;
    QTextBrowser* scenarioDetail_ = nullptr;

    // 子页 2：异常传播图解
    QListWidget* phaseList_    = nullptr;
    QTextBrowser* phaseDetail_ = nullptr;

    /// 构造辅助
    void buildScenarioPage(QWidget* host);
/// 构建异常阶段子页。
    void buildPhasePage(QWidget* host);

    /// 填充详情
    void populateScenarioDetail(int index);
/// 填充阶段详情。
    void populatePhaseDetail(int index);
};
