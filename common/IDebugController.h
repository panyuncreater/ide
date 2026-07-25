/**
 * @file common/IDebugController.h
 * @brief 调试控制器抽象接口（P1-4 调试系统统一）。
 *
 * 为 DebugController（Interpreter 路径，跨线程阻塞模型）与
 * VmStepper（VM 路径，主线程 QTimer 异步模型）提供统一的类型契约。
 *
 * 设计原则（与 IBackend.h 一致）：
 *   - 只抽象签名完全一致的方法 — 避免引入适配器层增加复杂度
 *   - 步进操作不纳入接口 — DebugController 用 condition_variable 阻塞 worker 线程，
 *     VmStepper 用 QTimer 异步分批，调用语义根本不同
 *   - 状态快照查询不纳入接口 — 返回类型不同（Value vs VariableSnapshot）
 *   - 内部检查 hook 不纳入接口 — 输入类型不同（ASTNode* vs int line）
 *   - 单断点增删不纳入接口 — API 形态不同（增量 vs 批量）
 *
 * 已纳入接口的方法（29 个，分 4 类）：
 *   - 断点配置（13）：setBreakpoints / setBreakpointKind / setLogpointMessage /
 *     setFunctionBreakpoint / removeFunctionBreakpoint / setFunctionBreakpoints /
 *     setFunctionBreakpointCondition / setExceptionBreakpointEnabled /
 *     setWatchpoint / removeWatchpoint / clearWatchpoints /
 *     setLogCallback / setConditionEvaluator
 *   - 临时断点（3）：setTemporaryBreakpoint / clearTemporaryBreakpoint /
 *     getTemporaryBreakpoint
 *   - 状态查询（11）：getBreakpointHitCount / getBreakpointKind / getLogpointMessage /
 *     getFunctionBreakpoints / hasFunctionBreakpoint / getFunctionBreakpointHitCount /
 *     getFunctionBreakpointCondition / isExceptionBreakpointEnabled /
 *     getExceptionBreakpointHitCount / hasWatchpoints / getCallStack
 *   - 生命周期（2）：stop / reset
 *
 * 不纳入接口的部分（各子类提供，Facade 层分派）：
 *   - 步进操作：DebugController.stepIn/stepOver/stepOut/resume vs
 *     VmStepper.stepByMode(VmStepMode)
 *   - 状态快照：DebugController.getVariableSnapshot vs
 *     VmStepper.getStack/getGlobals/getCurrentFrameLocals
 *   - 内部 hook：DebugController.checkBreak(ASTNode*) vs
 *     VmStepper 私有 checkBreakpointHit(int)
 *   - VM 独有：setCompileResult / setUseRegister / getCurrentIP / getFrameCount / ...
 *   - Interpreter 独有：setBreakpoint(int) / toggleBreakpoint / setVariableCallback / ...
 *
 * 与 IBackend.h 的关系：
 *   IBackend 抽象执行后端（Interpreter/VM/RegisterVM），IDebugController 抽象调试控制器
 *   （DebugController/VmStepper）。两者正交，一个执行后端可对应一个调试控制器。
 *
 * @since P1-4
 */
#pragma once

#include "debug/DebugTypes.h" // BreakpointKind / WatchpointInfo / CallStackEntry
#include <QSet>
#include <functional>
#include <string>
#include <vector>

// ============================================================
// IDebugController 调试控制器抽象接口（P1-4）
// ============================================================
// 设计目标：为 Interpreter 路径（DebugController，跨线程阻塞）与 VM 路径
// （VmStepper，主线程异步）提供统一的类型契约，使上层 Facade（IdeController）
// 可通过 IDebugController* 统一操作断点配置与状态查询，无需类型分发。
//
// 继承关系：
//   - DebugController : public QObject, public IDebugController
//   - VmStepper : public QObject, public IDebugController
//   （QObject 不参与接口继承，避免 moc 钻石继承问题；IDebugController 是纯抽象类）
//
// 与 Facade 层的关系：
//   IdeController 当前按组合持有 DebugCoordinator + VmStepper 具体类，
//   通过双写（配置类）+ 分派读（查询类）覆盖全部场景。提取接口后行为不变，
//   仅类型契约化，为未来扩展（如 JIT 调试器）预留统一操作路径。
//
// @since P1-4

class IDebugController {
public:
    virtual ~IDebugController() = default;

    // ============================================================
    // 断点配置（双写语义，线程模型不敏感）
    // ============================================================

    /// 批量设置行断点（替换全部行断点）
    virtual void setBreakpoints(const QSet<int>& breakpoints) = 0;

    /// 设置断点类型（Line / Logpoint）
    virtual void setBreakpointKind(int line, BreakpointKind kind) = 0;

    /// 设置 Logpoint 日志消息模板
    virtual void setLogpointMessage(int line, const std::string& msg) = 0;

    /// 添加函数断点
    virtual void setFunctionBreakpoint(const std::string& name) = 0;

    /// 移除函数断点
    virtual void removeFunctionBreakpoint(const std::string& name) = 0;

    /// 批量设置函数断点（替换全部函数断点，保留已有条件）
    virtual void setFunctionBreakpoints(const QSet<std::string>& names) = 0;

    /// 设置函数断点条件表达式
    virtual void setFunctionBreakpointCondition(const std::string& name, const std::string& condition) = 0;

    /// 启用/禁用异常断点
    virtual void setExceptionBreakpointEnabled(bool enabled) = 0;

    /// 添加数据断点（watchpoint）
    virtual void setWatchpoint(const WatchpointInfo& wp) = 0;

    /// 移除数据断点（fieldName 为空时按 varName 匹配）
    virtual void removeWatchpoint(const std::string& varName, const std::string& fieldName = "") = 0;

    /// 清空全部数据断点
    virtual void clearWatchpoints() = 0;

    /// 设置日志回调（Logpoint 命中时输出）
    virtual void setLogCallback(std::function<void(const std::string&)> cb) = 0;

    /// 设置条件求值器回调
    /// @note 签名一致但实现语义不同：DebugController 在 worker 线程复用 Interpreter
    /// 求值；VmStepper 在主线程创建临时 Interpreter 注入 VM 状态求值。接口仅约束签名。
    virtual void setConditionEvaluator(std::function<bool(const std::string&)> evaluator) = 0;

    // ============================================================
    // 临时断点（runToCursor 一次性断点）
    // ============================================================

    /// 设置一次性临时断点（仅命中一次后自动清除）
    virtual void setTemporaryBreakpoint(int line) = 0;

    /// 清除临时断点
    virtual void clearTemporaryBreakpoint() = 0;

    /// 查询当前临时断点行号（无临时断点返回 -1）
    virtual int getTemporaryBreakpoint() const = 0;

    // ============================================================
    // 状态查询（分派读语义，const 方法无副作用）
    // ============================================================

    /// 行断点命中次数
    virtual int getBreakpointHitCount(int line) const = 0;

    /// 查询断点类型
    virtual BreakpointKind getBreakpointKind(int line) const = 0;

    /// 查询 Logpoint 日志消息模板
    virtual std::string getLogpointMessage(int line) const = 0;

    /// 查询全部函数断点名
    virtual QSet<std::string> getFunctionBreakpoints() const = 0;

    /// 查询是否存在指定函数断点
    virtual bool hasFunctionBreakpoint(const std::string& name) const = 0;

    /// 函数断点命中次数
    virtual int getFunctionBreakpointHitCount(const std::string& name) const = 0;

    /// 查询函数断点条件表达式
    virtual std::string getFunctionBreakpointCondition(const std::string& name) const = 0;

    /// 查询异常断点是否启用
    virtual bool isExceptionBreakpointEnabled() const = 0;

    /// 异常断点命中次数
    virtual int getExceptionBreakpointHitCount() const = 0;

    /// 是否有数据断点（快速路径判断）
    virtual bool hasWatchpoints() const = 0;

    /// 获取调用栈快照（从栈底 main 到栈顶当前帧）
    virtual std::vector<CallStackEntry> getCallStack() const = 0;

    // ============================================================
    // 生命周期
    // ============================================================

    /// 停止调试执行
    virtual void stop() = 0;

    /// 重置调试状态（清除断点/条件/计数器）
    virtual void reset() = 0;
};
