/**
 * @file compiler/JITA64CodeGen.cpp
 * @brief ARM64 JIT 后端实现 — Phase 1 PoC 代码生成。
 *
 * 将 BytecodeChunk 编译为 ARM64 (AArch64) 本地机器码。
 * 仅实现核心整数算术 + 控制流 + 全局/局部变量子集。
 *
 * AAPCS64 调用约定：
 *   - 入口函数签名: int64_t jitA64Entry(JitContext* ctx)
 *   - ctx 通过 x0 传入，prologue 保存到 x19 (callee-saved)
 *   - 调用 C++ helper 时：x0=ctx, x1/x2=args
 *
 * @see JITA64.h JITInternal.h
 * @since P3（ARM64 JIT PoC）
 */

#include "compiler/JITA64.h"

#ifdef MINILANG_USE_JIT_A64

#include "common/RuntimeLimits.h"
#include "compiler/JITInternal.h"
#include "interpreter/Value.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

using namespace jit_internal;

// ============================================================
// extern "C" ARM64 运行时辅助函数（与 x86-64 版本共享逻辑）
// ============================================================

extern "C" {

/// ARM64 版 print：输出 NaN-boxed Value
void jitA64PrintValue(JitContext* ctx, uint64_t rawBits) {
    if (!ctx || !ctx->outputCallback)
        return;
    Value v = bitsToValue(rawBits);
    (*ctx->outputCallback)(v.toString());
}

/// ARM64 版错误报告
void jitA64ReportError(JitContext* ctx, const char* msg) {
    if (!ctx || !ctx->hasError || !ctx->errorBuffer)
        return;
    *ctx->hasError = true;
    *ctx->errorBuffer = msg ? msg : "未知错误";
}

} // extern "C"

// ============================================================
// JITA64Backend 基础方法
// ============================================================

JITA64Backend::JITA64Backend() = default;

JITA64Backend::~JITA64Backend() {
    if (currentEntry_ != nullptr) {
        runtime_.release(currentEntry_);
        currentEntry_ = nullptr;
    }
}

void JITA64Backend::setOutputCallback(std::function<void(const std::string&)> cb) {
    outputCallback_ = std::move(cb);
    jitContext_.outputCallback = &outputCallback_;
}

void JITA64Backend::setInputCallback(std::function<std::string(const std::string&)> cb) {
    inputCallback_ = std::move(cb);
}

void JITA64Backend::runtimeError(const std::string& msg) {
    hasError_ = true;
    diagnostics_.addError(msg, 0, 0, DiagSource::JIT);
}

void JITA64Backend::compileError(const std::string& msg) {
    hasError_ = true;
    diagnostics_.addError(msg, 0, 0, DiagSource::JIT);
}

// ============================================================
// compileMainChunk — 将 mainChunk 编译为 ARM64 本地代码
// ============================================================
JitA64EntryFn JITA64Backend::compileMainChunk(const CompileResult& result) {
    using namespace asmjit;

    CodeHolder code;
    Error err = code.init(runtime_.environment());
    if (err != kErrorOk) {
        compileError("asmjit ARM64 CodeHolder::init 失败");
        return nullptr;
    }

    a64::Assembler a(&code);

    // --- Prologue ---
    // 保存 callee-saved 寄存器：x19(ctx), x20(bp), x21(sp_stack), fp, lr
    a.stp(a64::regs::x29, a64::regs::x30, a64::ptr_pre(a64::regs::sp, -16));
    a.stp(a64::regs::x19, a64::regs::x20, a64::ptr_pre(a64::regs::sp, -16));
    a.str(a64::regs::x21, a64::ptr_pre(a64::regs::sp, -16));
    a.mov(a64::regs::x29, a64::regs::sp); // frame pointer

    // ctx (x0) -> x19 (callee-saved)
    a.mov(a64::regs::x19, a64::regs::x0);

    // 分配操作数栈空间（8KB = 1024 个 int64_t）
    a.sub(a64::regs::sp, a64::regs::sp, 8192);
    // x21 = operand stack top（初始指向栈底 = sp + 8192，向下增长）
    a.add(a64::regs::x21, a64::regs::sp, 8192);

    // x20 = base pointer（mainChunk 无局部变量帧，设为 x21 初始值）
    a.mov(a64::regs::x20, a64::regs::x21);

    // --- Epilogue label ---
    Label epilogue = a.new_label();

    // --- 两遍扫描：第一遍收集跳转目标，第二遍生成代码 ---
    const auto& chunk = result.mainChunk;
    const auto& codeBytes = chunk.code;
    const auto& constants = chunk.constants;

    // 第一遍：为每个字节码位置创建 Label
    std::unordered_map<size_t, Label> labels;
    for (size_t ip = 0; ip < codeBytes.size();) {
        labels[ip] = a.new_label();
        ip += chunk.instructionSizeAt(ip);
    }

    // 第二遍：为每条 OpCode 生成 ARM64 机器码
    for (size_t ip = 0; ip < codeBytes.size();) {
        // 绑定当前位置的 Label
        a.bind(labels[ip]);

        auto op = static_cast<OpCode>(codeBytes[ip]);
        size_t nextIp = ip + chunk.instructionSizeAt(ip);

        switch (op) {
        case OpCode::OP_INT: {
            // 从常量池加载整数，NaN-box 编码后 push
            uint16_t constIdx =
                static_cast<uint16_t>(codeBytes[ip + 1]) | (static_cast<uint16_t>(codeBytes[ip + 2]) << 8);
            int64_t val = constants[constIdx].intVal();
            uint64_t encoded = 0;
            encodeNanBoxInt(val, encoded);
            // mov x0, encoded（可能需要 movz+movk 序列）
            a.mov(a64::regs::x0, encoded);
            // push: x21 -= 8; str x0, [x21]
            a.sub(a64::regs::x21, a64::regs::x21, 8);
            a.str(a64::regs::x0, a64::ptr(a64::regs::x21));
            break;
        }

        case OpCode::OP_ADD: {
            // pop 两个值到 x0, x1（NaN-boxed INT 快速路径）
            a.ldr(a64::regs::x1, a64::ptr(a64::regs::x21)); // rhs
            a.add(a64::regs::x21, a64::regs::x21, 8);
            a.ldr(a64::regs::x0, a64::ptr(a64::regs::x21)); // lhs
            // 提取 int48 payload（低 48 位）并相加
            a.and_(a64::regs::x2, a64::regs::x0, JIT_INT48_MASK); // lhs payload
            a.and_(a64::regs::x3, a64::regs::x1, JIT_INT48_MASK); // rhs payload
            a.add(a64::regs::x2, a64::regs::x2, a64::regs::x3);   // sum
            // 重新编码为 NaN-boxed INT
            a.mov(a64::regs::x3, JIT_INT_TAG_BASE);
            a.and_(a64::regs::x2, a64::regs::x2, JIT_INT48_MASK); // 截断到 48 位
            a.orr(a64::regs::x0, a64::regs::x2, a64::regs::x3);   // tag | payload
            // 写回栈顶
            a.str(a64::regs::x0, a64::ptr(a64::regs::x21));
            break;
        }

        case OpCode::OP_SUBTRACT: {
            a.ldr(a64::regs::x1, a64::ptr(a64::regs::x21));
            a.add(a64::regs::x21, a64::regs::x21, 8);
            a.ldr(a64::regs::x0, a64::ptr(a64::regs::x21));
            a.and_(a64::regs::x2, a64::regs::x0, JIT_INT48_MASK);
            a.and_(a64::regs::x3, a64::regs::x1, JIT_INT48_MASK);
            a.sub(a64::regs::x2, a64::regs::x2, a64::regs::x3);
            a.mov(a64::regs::x3, JIT_INT_TAG_BASE);
            a.and_(a64::regs::x2, a64::regs::x2, JIT_INT48_MASK);
            a.orr(a64::regs::x0, a64::regs::x2, a64::regs::x3);
            a.str(a64::regs::x0, a64::ptr(a64::regs::x21));
            break;
        }

        case OpCode::OP_MULTIPLY: {
            a.ldr(a64::regs::x1, a64::ptr(a64::regs::x21));
            a.add(a64::regs::x21, a64::regs::x21, 8);
            a.ldr(a64::regs::x0, a64::ptr(a64::regs::x21));
            a.and_(a64::regs::x2, a64::regs::x0, JIT_INT48_MASK);
            a.and_(a64::regs::x3, a64::regs::x1, JIT_INT48_MASK);
            // 符号扩展 48->64 位后相乘
            a.sbfx(a64::regs::x2, a64::regs::x2, 0, 48);
            a.sbfx(a64::regs::x3, a64::regs::x3, 0, 48);
            a.mul(a64::regs::x2, a64::regs::x2, a64::regs::x3);
            a.mov(a64::regs::x3, JIT_INT_TAG_BASE);
            a.and_(a64::regs::x2, a64::regs::x2, JIT_INT48_MASK);
            a.orr(a64::regs::x0, a64::regs::x2, a64::regs::x3);
            a.str(a64::regs::x0, a64::ptr(a64::regs::x21));
            break;
        }

        case OpCode::OP_POP: {
            // pop: x21 += 8
            a.add(a64::regs::x21, a64::regs::x21, 8);
            break;
        }

        case OpCode::OP_PRINT: {
            // 调用 jitA64PrintValue(ctx, stack_top_value)
            a.ldr(a64::regs::x1, a64::ptr(a64::regs::x21)); // arg2 = top value bits
            a.add(a64::regs::x21, a64::regs::x21, 8);       // pop
            a.mov(a64::regs::x0, a64::regs::x19);           // arg1 = ctx
            a.mov(a64::regs::x2, reinterpret_cast<uint64_t>(&jitA64PrintValue));
            a.blr(a64::regs::x2);
            break;
        }

        case OpCode::OP_RETURN: {
            a.b(epilogue);
            break;
        }

        case OpCode::OP_JUMP: {
            uint16_t offset =
                static_cast<uint16_t>(codeBytes[ip + 1]) | (static_cast<uint16_t>(codeBytes[ip + 2]) << 8);
            if (labels.count(offset)) {
                a.b(labels[offset]);
            }
            break;
        }

        case OpCode::OP_JUMP_IF_FALSE: {
            uint16_t offset =
                static_cast<uint16_t>(codeBytes[ip + 1]) | (static_cast<uint16_t>(codeBytes[ip + 2]) << 8);
            // pop 栈顶值，检查 truthiness
            a.ldr(a64::regs::x0, a64::ptr(a64::regs::x21));
            a.add(a64::regs::x21, a64::regs::x21, 8);
            // 简化检查：值为 0（NaN-boxed false=0x7FF9000000000000 payload=0）或 null
            a.mov(a64::regs::x1, JIT_NULL_BITS);
            a.cmp(a64::regs::x0, a64::regs::x1);
            if (labels.count(offset)) {
                a.b(asmjit::arm::CondCode::kEQ, labels[offset]);
            }
            // 检查 bool false（tag=0x7FF9, payload=0）
            a.mov(a64::regs::x1, static_cast<uint64_t>(0x7FF9000000000000ULL));
            a.cmp(a64::regs::x0, a64::regs::x1);
            if (labels.count(offset)) {
                a.b(asmjit::arm::CondCode::kEQ, labels[offset]);
            }
            // 检查 int 0（tag=0x7FF8, payload=0）
            a.mov(a64::regs::x1, JIT_INT_TAG_BASE);
            a.cmp(a64::regs::x0, a64::regs::x1);
            if (labels.count(offset)) {
                a.b(asmjit::arm::CondCode::kEQ, labels[offset]);
            }
            break;
        }

        case OpCode::OP_LOOP: {
            uint16_t offset =
                static_cast<uint16_t>(codeBytes[ip + 1]) | (static_cast<uint16_t>(codeBytes[ip + 2]) << 8);
            if (labels.count(offset)) {
                a.b(labels[offset]);
            }
            break;
        }

        case OpCode::OP_GET_LOCAL: {
            uint16_t slot = static_cast<uint16_t>(codeBytes[ip + 1]) | (static_cast<uint16_t>(codeBytes[ip + 2]) << 8);
            // ldr x0, [x20 - slot*8]
            a.ldr(a64::regs::x0, a64::ptr(a64::regs::x20, -static_cast<int32_t>(slot * 8)));
            // push
            a.sub(a64::regs::x21, a64::regs::x21, 8);
            a.str(a64::regs::x0, a64::ptr(a64::regs::x21));
            break;
        }

        case OpCode::OP_SET_LOCAL: {
            uint16_t slot = static_cast<uint16_t>(codeBytes[ip + 1]) | (static_cast<uint16_t>(codeBytes[ip + 2]) << 8);
            // peek (不 pop)
            a.ldr(a64::regs::x0, a64::ptr(a64::regs::x21));
            // str x0, [x20 - slot*8]
            a.str(a64::regs::x0, a64::ptr(a64::regs::x20, -static_cast<int32_t>(slot * 8)));
            break;
        }

        case OpCode::OP_GET_GLOBAL: {
            uint16_t slot = static_cast<uint16_t>(codeBytes[ip + 1]) | (static_cast<uint16_t>(codeBytes[ip + 2]) << 8);
            // ldr x1, [x19 + offsetof(JitContext, globalSlots)]  // ctx->globalSlots
            a.ldr(a64::regs::x1, a64::ptr(a64::regs::x19, 24));
            // ldr x0, [x1 + slot*8]
            a.ldr(a64::regs::x0, a64::ptr(a64::regs::x1, static_cast<int32_t>(slot * 8)));
            // push
            a.sub(a64::regs::x21, a64::regs::x21, 8);
            a.str(a64::regs::x0, a64::ptr(a64::regs::x21));
            break;
        }

        case OpCode::OP_SET_GLOBAL: {
            uint16_t slot = static_cast<uint16_t>(codeBytes[ip + 1]) | (static_cast<uint16_t>(codeBytes[ip + 2]) << 8);
            // peek
            a.ldr(a64::regs::x0, a64::ptr(a64::regs::x21));
            // ctx->globalSlots
            a.ldr(a64::regs::x1, a64::ptr(a64::regs::x19, 24));
            // str x0, [x1 + slot*8]
            a.str(a64::regs::x0, a64::ptr(a64::regs::x1, static_cast<int32_t>(slot * 8)));
            break;
        }

        case OpCode::OP_DEFINE_GLOBAL: {
            uint16_t slot = static_cast<uint16_t>(codeBytes[ip + 1]) | (static_cast<uint16_t>(codeBytes[ip + 2]) << 8);
            // pop
            a.ldr(a64::regs::x0, a64::ptr(a64::regs::x21));
            a.add(a64::regs::x21, a64::regs::x21, 8);
            // ctx->globalSlots
            a.ldr(a64::regs::x1, a64::ptr(a64::regs::x19, 24));
            a.str(a64::regs::x0, a64::ptr(a64::regs::x1, static_cast<int32_t>(slot * 8)));
            break;
        }

        case OpCode::OP_NULL: {
            a.mov(a64::regs::x0, JIT_NULL_BITS);
            a.sub(a64::regs::x21, a64::regs::x21, 8);
            a.str(a64::regs::x0, a64::ptr(a64::regs::x21));
            break;
        }

        case OpCode::OP_TRUE: {
            uint64_t trueBits = 0x7FF9000000000001ULL; // BOOL tag + payload 1
            a.mov(a64::regs::x0, trueBits);
            a.sub(a64::regs::x21, a64::regs::x21, 8);
            a.str(a64::regs::x0, a64::ptr(a64::regs::x21));
            break;
        }

        case OpCode::OP_FALSE: {
            uint64_t falseBits = 0x7FF9000000000000ULL; // BOOL tag + payload 0
            a.mov(a64::regs::x0, falseBits);
            a.sub(a64::regs::x21, a64::regs::x21, 8);
            a.str(a64::regs::x0, a64::ptr(a64::regs::x21));
            break;
        }

        // ---- 算术完善（INT 原生路径，对齐 x86-64 R140） ----
        case OpCode::OP_NEGATE: {
            // pop 栈顶，对 int48 payload 取负，重新 NaN-box
            a.ldr(a64::regs::x0, a64::ptr(a64::regs::x21));
            a.sbfx(a64::regs::x1, a64::regs::x0, 0, 48); // 符号扩展 payload
            a.neg(a64::regs::x1, a64::regs::x1);
            a.and_(a64::regs::x1, a64::regs::x1, JIT_INT48_MASK);
            a.mov(a64::regs::x2, JIT_INT_TAG_BASE);
            a.orr(a64::regs::x0, a64::regs::x1, a64::regs::x2);
            a.str(a64::regs::x0, a64::ptr(a64::regs::x21));
            break;
        }

        case OpCode::OP_DIVIDE:
        case OpCode::OP_MODULO: {
            // pop rhs/lhs，符号扩展后 sdiv（截断向零，与 StackVM 一致）；
            // rhs==0 → jitA64ReportError + b epilogue（与 x86-64 除零路径对齐）
            Label divOk = a.new_label();
            a.ldr(a64::regs::x1, a64::ptr(a64::regs::x21)); // rhs
            a.add(a64::regs::x21, a64::regs::x21, 8);
            a.ldr(a64::regs::x0, a64::ptr(a64::regs::x21)); // lhs
            a.sbfx(a64::regs::x2, a64::regs::x0, 0, 48);    // lhs payload
            a.sbfx(a64::regs::x3, a64::regs::x1, 0, 48);    // rhs payload
            a.cmp(a64::regs::x3, 0);
            a.b(asmjit::arm::CondCode::kNE, divOk);
            // 除零错误路径：消息字面量静态存储，地址长期有效
            a.mov(a64::regs::x0, a64::regs::x19); // arg1 = ctx
            a.mov(a64::regs::x1, reinterpret_cast<uint64_t>("除以零"));
            a.mov(a64::regs::x2, reinterpret_cast<uint64_t>(&jitA64ReportError));
            a.blr(a64::regs::x2);
            a.b(epilogue);
            a.bind(divOk);
            if (op == OpCode::OP_DIVIDE) {
                a.sdiv(a64::regs::x4, a64::regs::x2, a64::regs::x3); // q = lhs / rhs
            } else {
                a.sdiv(a64::regs::x4, a64::regs::x2, a64::regs::x3);           // q
                a.msub(a64::regs::x4, a64::regs::x4, a64::regs::x3, a64::regs::x2); // rem = lhs - q*rhs
            }
            a.and_(a64::regs::x4, a64::regs::x4, JIT_INT48_MASK);
            a.mov(a64::regs::x5, JIT_INT_TAG_BASE);
            a.orr(a64::regs::x0, a64::regs::x4, a64::regs::x5);
            a.str(a64::regs::x0, a64::ptr(a64::regs::x21)); // 栈顶已指向 lhs 槽（pop rhs 后）
            break;
        }

        // ---- 比较指令（INT payload 符号扩展比较 → bool，分支式生成） ----
        // 分支式（cmp + b(cond) → mov bool）避开 cset 的 Imm 编码复杂性，
        // 与此文件已有 OP_JUMP_IF_FALSE 的条件分支风格一致。
        case OpCode::OP_EQUAL:
        case OpCode::OP_NOT_EQUAL:
        case OpCode::OP_LESS:
        case OpCode::OP_GREATER:
        case OpCode::OP_LESS_EQUAL:
        case OpCode::OP_GREATER_EQUAL: {
            Label setTrue = a.new_label();
            Label cmpDone = a.new_label();
            a.ldr(a64::regs::x1, a64::ptr(a64::regs::x21)); // rhs
            a.add(a64::regs::x21, a64::regs::x21, 8);
            a.ldr(a64::regs::x0, a64::ptr(a64::regs::x21)); // lhs（栈顶）
            asmjit::arm::CondCode cc = asmjit::arm::CondCode::kEQ;
            if (op == OpCode::OP_EQUAL || op == OpCode::OP_NOT_EQUAL) {
                // 相等性：比较完整 raw bits（同型 INT 值等价于位相等）
                a.cmp(a64::regs::x0, a64::regs::x1);
                cc = (op == OpCode::OP_EQUAL) ? asmjit::arm::CondCode::kEQ : asmjit::arm::CondCode::kNE;
            } else {
                // 大小比较：int48 payload 符号扩展后比较
                a.sbfx(a64::regs::x2, a64::regs::x0, 0, 48);
                a.sbfx(a64::regs::x3, a64::regs::x1, 0, 48);
                a.cmp(a64::regs::x2, a64::regs::x3);
                if (op == OpCode::OP_LESS)
                    cc = asmjit::arm::CondCode::kLT;
                else if (op == OpCode::OP_GREATER)
                    cc = asmjit::arm::CondCode::kGT;
                else if (op == OpCode::OP_LESS_EQUAL)
                    cc = asmjit::arm::CondCode::kLE;
                else
                    cc = asmjit::arm::CondCode::kGE;
            }
            a.b(cc, setTrue);
            a.mov(a64::regs::x0, static_cast<uint64_t>(0x7FF9000000000000ULL)); // false
            a.b(cmpDone);
            a.bind(setTrue);
            a.mov(a64::regs::x0, static_cast<uint64_t>(0x7FF9000000000001ULL)); // true
            a.bind(cmpDone);
            a.str(a64::regs::x0, a64::ptr(a64::regs::x21)); // push bool（栈顶已指向 lhs 槽）
            break;
        }

        case OpCode::OP_NOT: {
            // 逻辑取反：push bool(!truthy)。falsy = bool false / null / int 0
            Label isFalsy = a.new_label();
            Label notDone = a.new_label();
            a.ldr(a64::regs::x0, a64::ptr(a64::regs::x21));
            a.mov(a64::regs::x1, static_cast<uint64_t>(0x7FF9000000000000ULL)); // bool false
            a.cmp(a64::regs::x0, a64::regs::x1);
            a.b(asmjit::arm::CondCode::kEQ, isFalsy);
            a.mov(a64::regs::x1, JIT_NULL_BITS); // null
            a.cmp(a64::regs::x0, a64::regs::x1);
            a.b(asmjit::arm::CondCode::kEQ, isFalsy);
            a.mov(a64::regs::x1, JIT_INT_TAG_BASE); // int 0
            a.cmp(a64::regs::x0, a64::regs::x1);
            a.b(asmjit::arm::CondCode::kEQ, isFalsy);
            // truthy → NOT = false
            a.mov(a64::regs::x0, static_cast<uint64_t>(0x7FF9000000000000ULL));
            a.b(notDone);
            a.bind(isFalsy);
            a.mov(a64::regs::x0, static_cast<uint64_t>(0x7FF9000000000001ULL));
            a.bind(notDone);
            a.str(a64::regs::x0, a64::ptr(a64::regs::x21));
            break;
        }

        // ---- 栈操作 ----
        case OpCode::OP_DUP: {
            a.ldr(a64::regs::x0, a64::ptr(a64::regs::x21));
            a.sub(a64::regs::x21, a64::regs::x21, 8);
            a.str(a64::regs::x0, a64::ptr(a64::regs::x21));
            break;
        }

        case OpCode::OP_SWAP: {
            // R99 match 表达式：交换栈顶两个值（仅交换 raw bits）
            a.ldr(a64::regs::x0, a64::ptr(a64::regs::x21));    // top
            a.ldr(a64::regs::x1, a64::ptr(a64::regs::x21, 8)); // top-1
            a.str(a64::regs::x1, a64::ptr(a64::regs::x21));
            a.str(a64::regs::x0, a64::ptr(a64::regs::x21, 8));
            break;
        }

        default:
            // 未实现的 opcode：跳过（Phase 1 PoC 不支持的指令静默忽略）
            break;
        }

        ip = nextIp;
    }

    // --- Epilogue ---
    a.bind(epilogue);
    // 返回值 = 0（PoC 不使用返回值）
    a.mov(a64::regs::x0, 0);
    // 释放操作数栈空间
    a.add(a64::regs::sp, a64::regs::sp, 8192);
    // 恢复 callee-saved 寄存器
    a.ldr(a64::regs::x21, a64::ptr_post(a64::regs::sp, 16));
    a.ldp(a64::regs::x19, a64::regs::x20, a64::ptr_post(a64::regs::sp, 16));
    a.ldp(a64::regs::x29, a64::regs::x30, a64::ptr_post(a64::regs::sp, 16));
    a.ret(a64::regs::x30);

    // --- 提交到 JitRuntime ---
    JitA64EntryFn entry = nullptr;
    err = runtime_.add(&entry, &code);
    if (err != kErrorOk) {
        compileError("asmjit ARM64 runtime_.add 失败");
        return nullptr;
    }
    return entry;
}

// ============================================================
// execute — 编译 + 执行
// ============================================================
JitA64Result JITA64Backend::execute(const CompileResult& result) {
    hasError_ = false;
    lastError_.clear();
    diagnostics_.clear();

    // 初始化全局变量 slot 存储
    globalSlots_.assign(result.globalSlotCount > 0 ? result.globalSlotCount : 0, static_cast<int64_t>(JIT_NULL_BITS));
    jitContext_.globalSlots = globalSlots_.data();
    jitContext_.hasError = &hasError_;
    jitContext_.errorBuffer = &lastError_;

    // 释放上一次编译的代码
    if (currentEntry_ != nullptr) {
        runtime_.release(currentEntry_);
        currentEntry_ = nullptr;
    }

    // 编译
    JitA64EntryFn entry = compileMainChunk(result);
    if (!entry) {
        return JitA64Result::CompileError;
    }
    currentEntry_ = entry;

    // 执行
    entry(&jitContext_);

    if (hasError_) {
        if (!lastError_.empty()) {
            diagnostics_.addError(lastError_, 0, 0, DiagSource::JIT);
        }
        return JitA64Result::RuntimeError;
    }

    return JitA64Result::OK;
}

#endif // MINILANG_USE_JIT_A64
