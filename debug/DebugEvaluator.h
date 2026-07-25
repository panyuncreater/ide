#pragma once

// ============================================================
// DebugEvaluator — 条件断点求值器（A5 fix 抽取自 DebugController）
// ------------------------------------------------------------
// 职责：
//   - 持有条件断点求值回调（由 IDE 层注入）
//   - 封装异常处理与日志记录，避免 DebugController 直接持有 std::function
//   - 提供未来扩展点（如变量快照注入、类型安全求值等）
//
// 解耦动机（A5）：
//   原 DebugController 既管理断点状态又管理求值回调，且条件求值路径
//   跨层调用 VM/Interpreter。抽取 DebugEvaluator 后：
//   - DebugController 仅持有 evaluator_ 指针，关注断点状态机
//   - DebugEvaluator 独立维护求值回调 + 异常处理 + 日志
//   - 测试桩可注入假 evaluator 验证断点逻辑，无需真实求值回调
// ============================================================

#include "common/Logger.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class DebugEvaluator {
public:
    using ConditionCallback = std::function<bool(const std::string&)>;

    DebugEvaluator() = default;
    // AUDIT-P2-CORRECT fix: 析构等待所有锁外 callback 完成，避免析构期间
    // worker 线程仍在执行 evaluate 的锁外 callback_ 导致 UAF。
    ~DebugEvaluator() { waitCallbackIdle(); }

    /// 设置条件求值回调（由 IDE 注入，回调内部可达 VM/Interpreter 状态）
    /// 线程安全：mutex 保护，可与 evaluate() 并发调用
    void setCallback(ConditionCallback cb) {
        std::lock_guard<std::mutex> lock(mutex_);
        callback_ = std::move(cb);
    }

    /// 是否已注入求值回调
    bool hasCallback() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<bool>(callback_);
    }

    /// 求值条件表达式。异常被捕获并记录日志，失败时返回 false（视为条件不满足）
    /// @param condition 条件表达式字符串
    /// @param line 断点行号（仅用于日志上下文）
    /// @return 条件是否为真；异常或无回调时返回 false
    bool evaluate(const std::string& condition, int line) const {
        ConditionCallback snap;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snap = callback_;
        }
        if (!snap)
            return false;
        // P0-2 fix: 活跃计数改为 shared_ptr<atomic>。CountGuard 持有 shared_ptr 副本，
        // 即使 waitCallbackIdle 超时后 ~DebugEvaluator 析构（释放成员 shared_ptr），
        // worker 线程的 CountGuard 副本仍保持 atomic 存活，~CountGuard 的 fetch_sub 安全，
        // 彻底消除超时后继续析构导致的 UAF。snap 本身是锁内拷贝的局部 std::function，
        // 不依赖 this->callback_，故 ~DebugEvaluator 销毁 callback_ 不影响 snap。
        activeCallbackCount_->fetch_add(1, std::memory_order_acq_rel);
        struct CountGuard {
            std::shared_ptr<std::atomic<int>> cnt;
            ~CountGuard() { cnt->fetch_sub(1, std::memory_order_acq_rel); }
        } guard{activeCallbackCount_};
        try {
            return snap(condition);
        } catch (const std::exception& e) {
            // E1 fix: 求值异常记录告警便于用户排查（未定义变量、类型不匹配等）
            Logger::Warning("条件断点(行 " + std::to_string(line) + ") 求值异常: " + e.what() + "（条件: " + condition +
                                "），视为条件不满足",
                            "Debugger");
        } catch (...) {
            Logger::Warning("条件断点(行 " + std::to_string(line) + ") 求值发生未知异常（条件: " + condition +
                                "），视为条件不满足",
                            "Debugger");
        }
        return false;
    }

    /// 等待正在执行的 callback 完成（用于析构前安全等待）
    // AUDIT-P2-CORRECT fix: 添加超时上限（3 秒），对齐 DebugController::waitCallbacksIdle。
    // 原实现无限 spin-wait，若 callback 进入死循环，析构永久阻塞。
    void waitCallbackIdle() const {
        constexpr int MAX_WAIT_MS = 3000;
        auto start = std::chrono::steady_clock::now();
        // P0-2 fix: 超时后不再有 UAF 风险——worker 的 CountGuard 持有 shared_ptr 副本，
        // atomic 存活至最后一个 shared_ptr 释放。超时仅表示"放弃等待"，析构可安全继续。
        while (activeCallbackCount_->load(std::memory_order_acquire) > 0) {
            if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
                    .count() > MAX_WAIT_MS) {
                Logger::Warning("DebugEvaluator::waitCallbackIdle 超时（callback 可能挂死），"
                                "继续析构（计数器为 shared_ptr，worker 完成后安全释放，无 UAF）",
                                "Debugger");
                break;
            }
            std::this_thread::yield();
        }
    }

private:
    mutable std::mutex mutex_;
    ConditionCallback callback_;
    // P0-2 fix: 活跃 callback 计数改为 shared_ptr<atomic>，生命周期独立于 DebugEvaluator。
    // worker 线程 evaluate() 的 CountGuard 持有副本，析构超时后仍可安全 fetch_sub。
    mutable std::shared_ptr<std::atomic<int>> activeCallbackCount_{std::make_shared<std::atomic<int>>(0)};
};
