#pragma once

#include <functional>
#include <string>
#include "common/Diagnostic.h"

// ============================================================
// IBackend 后端抽象接口（ARCH-09）
// ============================================================
// 设计目标：为 MiniLang 的两种执行后端（Interpreter 树遍历后端、
// VM 字节码栈机后端）提供统一的抽象基类，使上层（IdeController /
// WorkerManager）可通过 IBackend* 操作公共能力，便于未来替换或
// 新增后端（如纯编译到 WASM、JIT 后端等）。
//
// 设计原则：
//   1. 只抽象签名完全一致的方法 — 避免引入适配器层增加复杂度
//   2. 执行入口（execute）不纳入接口 — Interpreter 输入 Block（AST），
//      VM 输入 CompileResult（字节码），输入类型根本不同，强行统一
//      会牺牲类型安全。执行入口由具体后端类提供。
//   3. 错误查询不纳入接口 — Interpreter 通过抛异常报告错误，
//      VM 通过返回码 + hasError_ 标志报告错误，错误通道不同。
//      统一错误查询需要在 Interpreter 中新增错误缓存适配器，
//      暂不实现，未来如需统一可扩展。
//   4. 单步执行不纳入接口 — Interpreter 是 AST 节点级单步（由
//      DebugController 在 visit 回调中实现），VM 是指令级单步
//      （stepOnce），粒度不同。
//
// 已纳入接口的方法（3 个回调/诊断方法，签名完全一致）：
//   - setOutputCallback：print 语句输出回调
//   - setInputCallback：input 函数输入回调
//   - getDiagnostics：错误/警告诊断包（共享 common/Diagnostic.h 类型）
//
// 未来扩展方向：
//   - IInterpreterBackend 子接口：executeRepl / saveReplState /
//     restoreReplState / setModuleLoader / setDebugger / evaluateCondition
//   - IVmBackend 子接口：initExecution / stepOnce / isFinished /
//     getStack / getGlobals / getCurrentIP / getCurrentLine / getFrameCount
//   - 如需统一错误查询：在子类中重写 hasError/getLastError，
//     Interpreter 适配器需在 catch 异常后缓存到内部 lastError_ 字段
//
// 与 ARCH-11（IdeController Facade 拆分）的关系：
//   ARCH-11 已通过 WorkerManager / VmStepper 持有具体后端类型实现解耦，
//   本接口进一步提供类型抽象层。IdeController 当前不直接持有后端，
//   故本接口主要作为类型契约和未来扩展点存在。

class IBackend {
public:
    virtual ~IBackend() = default;

    /// 后端名称（用于日志/诊断显示）
    /// @return "Interpreter" / "VM" 等后端标识
    virtual std::string backendName() const = 0;

    /// 设置输出回调（print 语句、异常信息等输出通道）
    /// @param cb 回调函数，接收输出文本
    virtual void setOutputCallback(std::function<void(const std::string&)> cb) = 0;

    /// 设置输入回调（input 函数等输入通道）
    /// @param cb 回调函数，接收提示文本，返回用户输入
    virtual void setInputCallback(std::function<std::string(const std::string&)> cb) = 0;

    /// 获取诊断包（错误/警告/信息收集器）
    /// @return 只读诊断包引用
    virtual const DiagnosticBag& getDiagnostics() const = 0;
};
