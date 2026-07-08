/**
 * @file ReplPanel.h
 * @brief REPL 交互面板的类声明（功能：Read-Eval-Print Loop）
 *
 * 通过后台 future 异步执行代码，避免界面卡顿；支持多行输入与历史。
 */
#pragma once

#include <QWidget>
#include <QLineEdit>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QStringList>
#include <QTimer>
#include <QPointer>
#include <QCoreApplication>
#include <future>
#include <memory>
#include <atomic>

#include "interpreter/Value.h"  // QT-R-06 fix: std::future<Value> 需 Value 完整定义

// B6 fix: ReplPanel 不再直接持有 Interpreter*，改由 IdeController（业务层）
// 提供 retainReplAst + executeRepl 接口，避免 GUI 层直接接触引擎内部。
class IdeController;
class Lexer;
class Parser;

// ============================================================
// ReplPanel REPL 交互面板
// ============================================================

/// REPL（Read-Eval-Print Loop）交互面板
class ReplPanel : public QWidget {
    Q_OBJECT

public:
/// 构造 REPL 面板；parent 为父控件。
    explicit ReplPanel(QWidget* parent = nullptr);
    ~ReplPanel();

    /// 设置 IdeController（业务逻辑层，提供 REPL 执行接口）
    void setController(IdeController* controller);

    /// 追加输出文本
    void appendOutput(const QString& text);

    /// 追加错误文本
    void appendError(const QString& text);

    /// 清空历史
    void clearHistory();

    /// 启用/禁用输入（Run 期间禁用，防止与 worker 线程并发访问 Interpreter）
    void setInputEnabled(bool enabled);

    /// REPL 异步任务是否正在执行（用于 Run/Debug 前互斥检查，避免并发访问 Interpreter）
    bool isReplRunning() const { return replRunning_.load(); }

    /// AUDIT-LIFECYCLE fix: 显式等待 REPL 异步任务完成。
    /// 在 Ide::closeEvent 中析构开始前调用，消除对 Qt 子对象删除顺序的隐式依赖。
    /// 原实现仅在 ~ReplPanel 中 wait()，若 Qt 未来版本改变 children 删除顺序导致
    /// ~IdeController 先于 ~ReplPanel 运行，异步任务将访问半析构的 controller_ → UAF。
    void waitReplFuture();

private slots:
    /// 处理输入
    void onReturnPressed();

    /// QT-R-06 fix: 轮询异步执行状态的槽函数
    void pollReplFuture();

private:
    QTextEdit* outputArea_ = nullptr;   // 输出区域
    QLineEdit* inputLine_ = nullptr;    // 输入行
    IdeController* controller_ = nullptr; // 业务层指针（不拥有）

    QStringList history_;               // 命令历史
    int historyIndex_ = -1;             // 历史浏览索引
    // BUG-REPL-G7 (P2, 已知限制): history_ 仅存储单行首行，多行续行输入（如
    // 函数定义）不会被整体保存与重放。完整修复需引入多行历史编辑器（QPlainTextEdit
    // 替换 QLineEdit），工程量较大，作为 UX 增强暂不实现。当前行为：多行输入后
    // 通过 Up 键只能回溯到首行，用户需重新输入续行部分。

    QString pendingInput_;              // R4: 多行累积输入缓冲
    bool inContinuation_ = false;       // R4: 是否在续行模式

    // QT-R-06 fix: 异步执行支持
    // 用 std::future + QTimer 轮询替代 QtConcurrent（Qt6::Concurrent 模块未安装）
    std::future<Value> replFuture_;
    QTimer* pollTimer_ = nullptr;
    /// 异步执行期间禁用输入
    std::atomic<bool> replRunning_{false};
    /// AUDIT fix: 标记异步执行中是否已发生 RuntimeError/genericError，
    /// 避免错误信号已显示后又打印 "null" 结果造成重复输出
    std::atomic<bool> hadReplError_{false};

    /// 执行单行代码
    void executeLine(const QString& line);

    /// R4: 检查输入是否完整（括号/花括号/方括号是否匹配）
    static bool isInputComplete(const QString& input);

    /// 键盘事件过滤（支持上下键浏览历史）
    bool eventFilter(QObject* obj, QEvent* event) override;
};
