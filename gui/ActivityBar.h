#pragma once

#include <QFrame>
#include <QList>
#include "FluentGlobal.h"

class TransparentToolButton;
class QVBoxLayout;

/// 活动栏（VS Code 风格）：主窗口最左侧 48px 宽的图标条
/// 放置「资源管理器」「调试」等图标，点击切换左侧面板
class ActivityBar : public QFrame {
    Q_OBJECT
public:
    explicit ActivityBar(QWidget* parent = nullptr);

    /// 添加一个活动栏图标项，返回索引
    int addItem(const QString& text, Fluent::IconType icon);

    /// 设置当前选中项
    void setCurrentIndex(int index);

    /// 获取当前选中项
    int currentIndex() const { return currentIndex_; }

signals:
    /// 用户点击某个图标项时发射
    void currentChanged(int index);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    struct ActivityItem {
        TransparentToolButton* button;
        QString text;
    };
    QList<ActivityItem> items_;
    QVBoxLayout* layout_;
    int currentIndex_ = -1;

    void updateSelection();
};
