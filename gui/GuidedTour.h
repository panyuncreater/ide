// ============================================================
// GuidedTour.h — 轻量级新手引导组件
// ------------------------------------------------------------
// 解决问题：新手关闭 WelcomeWizard 后回到空白欢迎页找不到入口。
// 提供半透明遮罩 + 高亮目标 widget + 气泡提示的 4 步引导闭环，
// 实现"3 分钟 Hello World"引导。
//
// 设计：简化方案——不使用全屏遮罩层，而是给目标 widget 临时
// 设置蓝色边框高亮，并在其附近显示气泡（QFrame）。
// 气泡含标题/说明/步骤指示器/跳过+下一步按钮。
//
// 用法：
//   auto* tour = new GuidedTour(mainWindow, mainWindow);
//   tour->addStep(editorPanel, "代码编辑器", "在这里输入 MiniLang 代码");
//   tour->addStep(runBtn,     "运行",       "点击运行你的程序");
//   tour->addStep(replPanel,  "REPL",       "交互式求值");
//   tour->addStep(astView,    "AST 视图",   "查看语法树");
//   tour->start();
//
// 生命周期：bubble_ 作为 host_ 的子 widget，host_ 销毁时自动释放。
// 典型用法 host_ == parent（同为 MainWindow），~GuidedTour 调用 hideOverlay
// 恢复目标 widget 原样式并隐藏气泡，目标 widget 由 QPointer 安全跟踪。
// ============================================================

#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QRect> // positionBubble(const QRect&)
#include <QString>

class QWidget;
class QLabel;
class QTimer;
class QPushButton;

// 轻量级新手引导组件：半透明遮罩 + 高亮目标 widget + 气泡提示
// 用法：
//   auto* tour = new GuidedTour(parent);
//   tour->addStep(widget1, "标题1", "说明1");
//   tour->addStep(widget2, "标题2", "说明2");
//   tour->start();
class GuidedTour : public QObject {
    Q_OBJECT
public:
    /// 构造引导漫游；host 为被引导的宿主窗口。
    explicit GuidedTour(QWidget* host, QObject* parent = nullptr);
    ~GuidedTour() override;

    struct Step {
        QPointer<QWidget> target; // 要高亮的 widget（nullptr 表示无目标，居中显示）
        QString title;            // 气泡标题
        QString description;      // 气泡说明（支持简单 HTML）
        QString primaryBtnText;   // 主按钮文本（默认"下一步 →"）
        QString secondaryBtnText; // 次按钮文本（默认"跳过"）
    };

    void addStep(QWidget* target, const QString& title, const QString& description,
                 const QString& primaryBtnText = "下一步 →", const QString& secondaryBtnText = "跳过");

    void start(); // 开始引导，从第 0 步
    void next();  // 下一步
    void skip();  // 跳过引导

signals:
    /// 信号：当前步骤索引变化。
    void stepChanged(int index);
    void finished(bool completed); // completed=true 表示走完所有步骤，false 表示跳过

private:
    QWidget* host_; // 宿主窗口（通常为 MainWindow）
    QList<Step> steps_;
    int currentIndex_ = -1;

    // 遮罩层 widget，覆盖整个 host_，用 WS_Transparent 风格
    QWidget* overlay_ = nullptr;
    // 气泡 widget
    // AUDIT-P0 fix: 改为 QPointer 避免 IDE 析构期 UAF。
    // 原实现：bubble_ 是裸指针且为 host_ 子对象，Qt 子对象逆序析构时
    // bubble_ 先被销毁，~GuidedTour 后调用 hideOverlay 仍访问悬垂 bubble_。
    QPointer<QWidget> bubble_;
    class QLabel* bubbleTitle_ = nullptr;
    class QLabel* bubbleDesc_ = nullptr;
    class QPushButton* primaryBtn_ = nullptr;
    class QPushButton* secondaryBtn_ = nullptr;
    class QLabel* stepIndicator_ = nullptr;

    // 当前被高亮的目标及其原始样式表（用于步骤切换/结束时恢复）。
    // QPointer 保证目标 widget 被销毁后指针自动置空，避免 UAF。
    QPointer<QWidget> highlightedTarget_;
    QString savedStyleSheet_;

    /// 显示指定步骤。
    void showStep(int index);
    /// 隐藏遮罩结束引导。
    void hideOverlay();
    /// 定位气泡到目标矩形。
    void positionBubble(const QRect& targetRect);
};
