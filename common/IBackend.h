/**
 * @file common/IBackend.h
 * @brief MiniLang 后端抽象接口（ARCH-09）。
 *
 * 定义 IBackend 抽象基类，为 Interpreter / VM / RegisterVM 等执行后端
 * 提供统一的回调与诊断查询接口。上层（IdeController / WorkerManager）
 * 通过 IBackend* 操作公共能力，便于未来替换或新增后端（如纯编译到
 * WASM、JIT 后端等）。
 *
 * 设计原则：
 *   - 只抽象签名完全一致的方法（避免引入适配器层）
 *   - 执行入口不纳入接口（Interpreter 输入 Block(AST)，VM 输入 CompileResult）
 *   - 单步执行不纳入接口（粒度不同：Interpreter AST 节点级，VM 指令级）
 *
 * 已纳入接口的方法：
 *   - setOutputCallback：print 语句输出回调
 *   - setInputCallback：input 函数输入回调
 *   - getDiagnostics：错误/警告诊断包
 *   - hasError：是否发生错误（P2-12 统一错误查询）
 *   - getLastError：获取最后一条错误消息（P2-12，从 diagnostics_ 派生）
 *
 * @see DiagnosticBag
 * @since ARCH-09
 */
#pragma once

#include "common/Diagnostic.h"
#include "common/VmTypes.h" // P1-5 fix: VMResult 供 IVmBackend::stepOnce 返回值
#include "interpreter/Value.h"
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

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
//   3. 单步执行不纳入接口 — Interpreter 是 AST 节点级单步（由
//      DebugController 在 visit 回调中实现），VM 是指令级单步
//      （stepOnce），粒度不同。
//
// 已纳入接口的方法（5 个回调/诊断/错误查询方法，签名完全一致）：
//   - setOutputCallback：print 语句输出回调
//   - setInputCallback：input 函数输入回调
//   - getDiagnostics：错误/警告诊断包（共享 common/Diagnostic.h 类型）
//   - hasError：是否发生错误（P2-12，统一错误查询，从 diagnostics_ 派生）
//   - getLastError：获取最后一条错误消息（P2-12，从 diagnostics_ 派生）
//
// 未来扩展方向：
//   - IInterpreterBackend 子接口：executeRepl / saveReplState /
//     restoreReplState / setModuleLoader / setDebugger / evaluateCondition
//   - IVmBackend 子接口：initExecution / stepOnce / isFinished /
//     getStack / getGlobals / getCurrentIP / getCurrentLine / getFrameCount
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

    /// 是否发生错误（P2-12 统一错误查询）
    /// 从 diagnostics_ 派生，消除各后端独立的 hasError_ 标志位冗余。
    /// @return 是否有 Error 级别诊断
    virtual bool hasError() const { return getDiagnostics().hasErrors(); }

    /// 获取最后一条错误消息（P2-12 统一错误查询）
    /// 从 diagnostics_ 派生，消除各后端独立的 lastError_ 字段冗余。
    /// 返回**原始消息文本**（不含 [来源] 级别 (行 X) 前缀），保证三后端
    /// （Interpreter/VM/RegisterVM）返回相同消息——这是三后端一致性测试
    /// （TestThreeEnginesConsistency）的依赖。IDE 显示如需带位置信息，
    /// 应直接遍历 getDiagnostics().all() 调用 Diagnostic::format()。
    /// @return 最后一条 Error 级别诊断的 message 字段；无错误返回空字符串
    virtual std::string getLastError() const {
        const auto& diags = getDiagnostics().all();
        for (auto it = diags.rbegin(); it != diags.rend(); ++it) {
            if (it->isError())
                return it->message;
        }
        return {};
    }

    /// 获取最后一条错误的源码行号（P2-12 统一错误查询）
    /// 从 diagnostics_ 派生，消除各后端独立的 lastErrorLine_ 字段冗余。
    /// @return 最后一条 Error 级别诊断的 line 字段；无错误返回 0
    virtual int getLastErrorLine() const {
        const auto& diags = getDiagnostics().all();
        for (auto it = diags.rbegin(); it != diags.rend(); ++it) {
            if (it->isError())
                return it->line;
        }
        return 0;
    }
};

// ============================================================
// IVmBackend — VM 类后端子接口（P1-5 fix）
// ============================================================
// 设计目标：为支持单步执行的 VM 类后端（StackVM / RegisterVM）提供统一的
// 步进与状态检视接口。JITBackend 不实现此接口（JIT 一次性编译执行本地代码，
// 不支持指令级单步）。
//
// 与 IBackend 的关系：
//   - IVmBackend 继承 IBackend，拥有 IBackend 的全部回调/诊断方法
//   - 额外提供 8 个步进/检视方法，签名在 StackVM 与 RegisterVM 间完全一致
//   - 上层（VmStepper / ExecutionTraceRecorder / 调试面板）可通过 IVmBackend*
//     统一操作两种 VM 后端，无需模板化或类型分发
//
// initExecution 不纳入接口：
//   StackVM 输入 CompileResult，RegisterVM 输入 RegisterCompileResult，
//   输入类型不同。调用方需用具体类型调用 initExecution，之后通过 IVmBackend*
//   进行步进和检视。
//
// @since P1-5

/// VM 调用栈帧条目（IVmBackend::getCallStack 返回类型）
/// StackVM 与 RegisterVM 共享同一结构：函数名 + 当前行号 + 指令指针。
/// 不含 locals（需通过具体子类的 getFrameLocalsAt 单独查询，避免每个后端
/// 自行实现 locals 反查逻辑）；不含 depth（调用方按 vector 索引推导）。
/// @since P1-5 扩展（getCallStack 纳入接口）
struct VmCallStackEntry {
    std::string functionName; // "main" 或函数名
    int line = 0;             // 当前源码行号（1-based，0=无位置信息）
    size_t ip = 0;            // 当前指令指针
};

class IVmBackend : public IBackend {
public:
    /// 单步执行一条指令（需先调用具体子类的 initExecution）
    /// @return VM_OK 继续；VM_RUNTIME_ERROR 运行时错误；VM_STACK_OVERFLOW 栈溢出
    virtual VMResult stepOnce() = 0;

    /// 是否已完成（所有指令执行完毕或发生错误终止）
    virtual bool isFinished() const = 0;

    /// 获取操作数栈快照（RegisterVM 返回寄存器窗口值）
    /// @return 栈/寄存器值的拷贝（线程安全快照）
    virtual std::vector<Value> getStack() const = 0;

    /// 获取全局变量映射（合并 slot-based + map-based）
    /// @return 变量名→值映射拷贝
    virtual std::unordered_map<std::string, Value> getGlobals() const = 0;

    /// 获取当前指令指针（当前帧 IP）
    /// @return 当前 IP；无帧时返回 0
    virtual size_t getCurrentIP() const = 0;

    /// 获取当前源码行号（从当前 chunk 的 lines 数组获取）
    /// @return 1-based 行号；无位置信息返回 0
    virtual int getCurrentLine() const = 0;

    /// 获取当前调用栈深度（帧数）
    /// @return 帧数；无帧返回 0
    virtual size_t getFrameCount() const = 0;

    /// 获取调用栈快照（从栈底 main 到栈顶当前帧）
    /// @return 调用帧条目拷贝（functionName + line + ip）
    /// @since P1-5 扩展（原为 VM/RegisterVM 各自独立方法，现统一到接口）
    virtual std::vector<VmCallStackEntry> getCallStack() const = 0;
};
