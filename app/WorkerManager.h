#pragma once

// ============================================================
// WorkerManager — Worker 线程管理（ARCH-11 拆分自 IdeController）
// ------------------------------------------------------------
// 职责：
//   - 持有 Interpreter / DebugController 共享所有权
//   - 创建/启动/停止/清理 Worker 线程
//   - 设置模块加载器、调试模式、REPL 状态保存/恢复
//   - 构建跨线程安全的 input() 回调
//   - 管理主线程输出/输入回调
//
// 与 PipelineRunner 协作：prepareRun 接收 PipelineRunner 产出的 AST。
// 与 DebugCoordinator 协作：共享 Interpreter / DebugController 所有权。
// ============================================================

#include <QObject>
#include <QString>
#include <QThread>
#include <QMap>
#include <QSet>
#include <memory>
#include <functional>
#include <string>
#include <atomic>

#include "interpreter/Interpreter.h"
#include "debug/DebugController.h"
#include "InterpreterWorker.h"

class WorkerManager : public QObject {
    Q_OBJECT

public:
    WorkerManager(std::shared_ptr<Interpreter> interpreter,
                  std::shared_ptr<DebugController> debugger,
                  QObject* parent = nullptr);
    ~WorkerManager();

    // ---- Worker 线程管理 ----
    /// 准备运行（设置模块加载器 + 创建 worker），返回 true 表示已就绪。
    /// astRoot: PipelineRunner 产出的 AST（shared_ptr 共享所有权）
    /// filePath: 当前文件路径（用于模块加载的相对路径解析），为空表示未保存文件
    bool prepareRun(bool isDebug, std::shared_ptr<Block> astRoot, const std::string& filePath);
    /// 启动 worker 线程执行
    void startWorker();
    /// 关闭前安全停止。
    /// 返回 true: worker 已正常停止，状态已清理。
    /// 返回 false: worker 未在 timeoutMs 内响应，interpreter/debugger 状态未清理
    ///           （线程仍在运行，无法安全并发访问 interpreter）。
    ///           调用方 MUST 调用 forceStop() 完成清理，否则 UI 卡在运行态。
    bool stopForClose(int timeoutMs = 3000);
    /// 强制终止 worker 线程
    void forceStop();
    bool isRunning() const { return isRunning_; }
    bool isDebugRun() const { return isDebugRun_; }

    // ---- 回调管理 ----
    /// 恢复主线程输出+输入回调（worker 退出后调用）
    void setupMainCallbacks();
    /// D20 fix: 构建跨线程安全的 input() 回调（带 30 秒超时保护）
    // MEM-02 fix: 移除 const（lambda 通过 QPointer 捕获 this，有副作用）
    std::function<std::string(const std::string&)> buildInputCallback();

    // ---- 引擎访问（供 DebugCoordinator / IdeController 使用）----
    // D3 fix: 标记为 const，仅拷贝 shared_ptr 不修改成员状态
    std::shared_ptr<Interpreter> interpreter() const { return interpreter_; }
    std::shared_ptr<DebugController> debugger() const { return debugger_; }

signals:
    void outputReady(const QString& text);
    void runOk();
    void stoppedByUser();
    void runtimeError(const QString& msg, int line, int column);
    void genericError(const QString& msg);
    void workerFinished(bool wasDebug);

private:
    /// Worker 线程结束后的清理（删除 worker/thread、恢复回调、恢复 REPL 状态）
    void cleanupWorker();

    std::shared_ptr<Interpreter> interpreter_;
    std::shared_ptr<DebugController> debugger_;

    // 7.1 fix: 使用 unique_ptr 替代裸 new/delete，确保异常安全和资源释放
    // QThread 需自定义删除器：先 quit+wait 再 delete，避免 delete running QThread 的 UB
    struct QThreadDeleter {
        void operator()(QThread* thread) const;
    };
    std::unique_ptr<QThread, QThreadDeleter> workerThread_;
    std::unique_ptr<InterpreterWorker> worker_;

    // P2 fix: atomic<bool> 对齐 DebugController P0-9 fix 模式。
    // 主线程写（startWorker/forceStop/prepareRun/cleanupWorker）与潜在跨线程读之间
    // 需内存可见性保证。原子避免数据竞争导致的 UI 状态不一致。
    std::atomic<bool> isRunning_{false};
    std::atomic<bool> isDebugRun_{false};
};
