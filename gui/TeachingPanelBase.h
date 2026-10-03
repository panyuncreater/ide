#pragma once

// ============================================================
// TeachingPanelBase — 教学面板公共基类（审计问题 3 落地第一步）
// ------------------------------------------------------------
// 审计报告（2026-08-03）问题 3："gui/ 缺少高层面板框架抽象，42 个
// 教学面板存在大量样板代码"。本基类开始收敛跨面板样板，迁移路径：
//   1. 主题切换接线（本次落地）：原先各面板手工调用
//      Theme::onThemeModeChanged(this, lambda) 重建含主题色的视图，
//      接线样板重复 6 处；现统一到基类构造，子类仅 override applyTheme()。
//   2. 后续扩展点：运行状态钩子（setRunning 联动禁用）、标题/help 文档
//      （TeachingPanelHeader 接线）、新手引导注册等，按面板家族分批迁移。
//
// 约定：面板自身布局继续由子类构造函数创建（各面板结构差异大，
// 强制统一布局反而是破坏性迁移）；基类只承接真正跨面板一致的行为。
// ============================================================

#include <QWidget>

class QVBoxLayout;

class TeachingPanelBase : public QWidget {
    Q_OBJECT
public:
    explicit TeachingPanelBase(QWidget* parent = nullptr);

protected:
    /// 主题切换钩子：QFluentKit 主题模式变化时调用（receiver=this 保证生命周期
    /// 安全，析构自动断开）。子类 override 以重建含主题色（TeachingTheme::*）的
    /// HTML/样式/视图；默认实现为空。
    virtual void applyTheme();

    /// 根布局访问器：惰性创建/采纳（42 面板的标准容器形态）。
    /// 子类构造函数若已自行 new QVBoxLayout(this)（历史面板的标准写法），直接
    /// 采纳该布局——QWidget 只允许一个顶层布局，基类若抢先创建，子类的二次
    /// 创建会被 Qt 拒绝（运行时警告 + 子控件失去布局管理，面板错乱）；
    /// 否则创建零边距、零间距的标准容器布局。可在基类构造完成后任意时机调用。
    QVBoxLayout* rootLayout();

private:
    QVBoxLayout* rootLayout_ = nullptr;
};
