#pragma once

/**
 * @file compiler/JITA64.h
 * @brief ARM64 (AArch64) JIT 后端 — Phase 1 PoC。
 *
 * 将 BytecodeChunk 编译为 ARM64 本地机器码，直接在 Apple Silicon / ARM64 Linux 上执行。
 * 与 x86-64 JITBackend 共享 CompileResult 输入，是其 ARM64 平台等价物。
 *
 * Phase 1 PoC 范围（对齐 x86-64 R138-R140）：
 *   - 整数/浮点常量、算术运算（add/sub/mul/div/mod/negate）
 *   - 比较运算（eq/neq/lt/gt/lte/gte）、逻辑取反
 *   - 控制流（jump/jump_if_false/loop）
 *   - 局部/全局变量读写
 *   - print 输出、pop、return
 *
 * ARM64 寄存器分配：
 *   - x19 = JitContext* ctx（callee-saved）
 *   - x20 = current frame basePointer（callee-saved）
 *   - x21 = operand stack top（callee-saved，向低地址增长）
 *   - x0-x7 = 临时寄存器 / AAPCS64 函数调用参数
 *   - sp = 原生栈
 *
 * @see JIT.h JITInternal.h IBackend.h
 * @since P3（ARM64 JIT PoC）
 */

#include "common/Diagnostic.h"
#include "common/IBackend.h"

#ifdef MINILANG_USE_JIT_A64

#include "compiler/Bytecode.h"
#include "compiler/JITInternal.h"
#include <asmjit/a64.h>
#include <functional>
#include <string>
#include <vector>

/// ARM64 JIT 执行结果（与 x86-64 JitResult 对齐）
enum class JitA64Result {
    OK,           // 执行成功
    RuntimeError, // 运行时错误
    CompileError, // JIT 编译失败
};

/// ARM64 JIT 入口函数签名
/// @param ctx JitContext 指针（通过 x0 传入，prologue 保存到 x19）
/// @return int64_t（最后一个表达式的值）
using JitA64EntryFn = int64_t (*)(JitContext* ctx);

// ============================================================
// JITA64Backend — ARM64 JIT 后端（Phase 1 PoC）
// ============================================================
class JITA64Backend : public IBackend {
public:
    JITA64Backend();
    ~JITA64Backend() override;

    /// IBackend: 后端名称
    std::string backendName() const override { return "JIT-ARM64"; }

    /// IBackend: 设置输出回调（print 语句输出通道）
    void setOutputCallback(std::function<void(const std::string&)> cb) override;

    /// IBackend: 设置输入回调
    void setInputCallback(std::function<std::string(const std::string&)> cb) override;

    /// IBackend: 获取诊断包
    const DiagnosticBag& getDiagnostics() const override { return diagnostics_; }

    /// 执行编译结果
    JitA64Result execute(const CompileResult& result);

    /// 是否发生过错误
    bool hasError() const override { return hasError_ || diagnostics_.hasErrors(); }

    /// 获取最后的错误消息
    std::string getLastError() const override {
        const auto& diags = diagnostics_.all();
        for (auto it = diags.rbegin(); it != diags.rend(); ++it) {
            if (it->isError())
                return it->message;
        }
        return lastError_;
    }

    /// 获取最后错误的源码行号
    int getLastErrorLine() const override {
        const auto& diags = diagnostics_.all();
        for (auto it = diags.rbegin(); it != diags.rend(); ++it) {
            if (it->isError())
                return it->line;
        }
        return 0;
    }

private:
    /// 编译 mainChunk 到 ARM64 本地代码
    JitA64EntryFn compileMainChunk(const CompileResult& result);

    void runtimeError(const std::string& msg);
    void compileError(const std::string& msg);

    asmjit::JitRuntime runtime_;
    JitA64EntryFn currentEntry_ = nullptr;
    DiagnosticBag diagnostics_;
    std::function<void(const std::string&)> outputCallback_;
    std::function<std::string(const std::string&)> inputCallback_;
    std::string lastError_;
    bool hasError_ = false;

    // 运行时状态（与 x86-64 JITBackend 共用 JitContext 结构）
    JitContext jitContext_;
    std::vector<int64_t> globalSlots_;
};

#endif // MINILANG_USE_JIT_A64
