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

#include <string>
#include <functional>
#include <mutex>
#include "common/Logger.h"

class DebugEvaluator {
public:
    using ConditionCallback = std::function<bool(const std::string&)>;

    DebugEvaluator() = default;
    ~DebugEvaluator() = default;

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
        if (!snap) return false;
        try {
            return snap(condition);
        } catch (const std::exception& e) {
            // E1 fix: 求值异常记录告警便于用户排查（未定义变量、类型不匹配等）
            Logger::Warning("条件断点(行 " + std::to_string(line) +
                            ") 求值异常: " + e.what() + "（条件: " +
                            condition + "），视为条件不满足", "Debugger");
        } catch (...) {
            Logger::Warning("条件断点(行 " + std::to_string(line) +
                            ") 求值发生未知异常（条件: " + condition +
                            "），视为条件不满足", "Debugger");
        }
        return false;
    }

private:
    mutable std::mutex mutex_;
    ConditionCallback callback_;
};
