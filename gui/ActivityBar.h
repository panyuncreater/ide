#pragma once

#include <QFrame>
#include <QList>
#include <QString>
#include "FluentGlobal.h"

/// @file ActivityBar.h
/// 活动栏（VS Code 风格左侧图标条）：以注册制管理图标项的添加与选中切换。

class TransparentToolButton;
class QVBoxLayout;

/// 活动栏（VS Code 风格）：主窗口最左侧 48px 宽的图标条
/// 放置「资源管理器」「调试」等图标，点击切换左侧面板
///
/// 注册制设计（P0.5 改进）：
/// - 每个活动项有唯一字符串 ID（如 "explorer"/"debug"/"learning-path"）
/// - 切换时分发 by ID 而非 index，便于新增面板扩展
/// - 保留 index 兼容旧代码
class ActivityBar : public QFrame {
    Q_OBJECT
public:
    explicit ActivityBar(QWidget* parent = nullptr);

    /// 添加一个活动栏图标项（注册制），返回索引
    /// id 必须唯一；若重复返回 -1
    int addItem(const QString& id, const QString& text, Fluent::IconType icon);

    /// 旧式添加（无 ID）：自动以索引转字符串作为 id
    int addItem(const QString& text, Fluent::IconType icon);

    /// 设置当前选中项（按索引）
    void setCurrentIndex(int index);

    /// 设置当前选中项（按 ID）
    bool setCurrentId(const QString& id);

    /// 获取当前选中项索引
    int currentIndex() const { return currentIndex_; }

    /// 获取当前选中项 ID
    QString currentId() const;

    /// 根据 ID 查找索引；不存在返回 -1
    int indexOf(const QString& id) const;

    /// 当前项总数
    int count() const { return items_.size(); }

signals:
    /// 用户点击某个图标项时发射（按索引）
    void currentChanged(int index);

    /// 用户点击某个图标项时发射（按 ID）—— 推荐使用
    void currentChangedById(const QString& id);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    struct ActivityItem {
        TransparentToolButton* button;
        QString id;
        QString text;
    };
    QList<ActivityItem> items_;
    QVBoxLayout* layout_;
    int currentIndex_ = -1;

    void updateSelection();
};
