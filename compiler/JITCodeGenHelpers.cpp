/**
 * @file compiler/JITCodeGenHelpers.cpp
 * @brief JIT codegen 辅助函数实现（从 JITCodeGen.cpp 的 compileAllChunks lambda 提取）。
 *
 * 原 compileAllChunks 内嵌 25+ 个 lambda（~1100 行），新增 OpCode 支持需在 3000+ 行函数中
 * 定位正确位置，极易引入错误。本文件将所有 codegen 辅助 lambda 提取为 JITBackend 的成员函数，
 * 使 compileAllChunks 聚焦于 OpCode 分派逻辑，辅助代码在此独立维护。
 *
 * 拆分模式参考 VM.cpp → VMCalls.cpp / VMContainers.cpp 的成员函数提取方式。
 *
 * @see JIT.h JITInternal.h JITCodeGen.cpp
 * @since P3（JIT codegen 维护性重构）
 */

#include "compiler/JIT.h"

#ifdef MINILANG_USE_JIT

#include "common/RuntimeLimits.h" // NO_SLOT
#include "compiler/JITInternal.h"
#include <asmjit/asmjit.h>
#include <cassert> // Bug #53: emitFloatBinaryArith divByZeroLabel assert
#include <cstdint>

using namespace asmjit;
using namespace jit_internal;

// ============================================================
// R143 阶段 3b：C++ 辅助函数调用代码生成
// ============================================================

// 生成"peek 栈顶两个值 → 设置参数 → 调用辅助函数 → pop 两个 → push 结果"的机器码
// @param fnPtr 辅助函数指针（签名: uint64_t fn(uint64_t left, uint64_t right) 或
//              uint64_t fn(JitContext* ctx, uint64_t left, uint64_t right)）
// @param needCtx 是否传递 JitContext* 作为第一个参数（用于除法/取模的错误报告）
void JITBackend::emitCallBinaryHelper(x86::Assembler& a, Label epilogue, void* fnPtr, bool needCtx, bool checkError) {
    // 设置参数
    if (needCtx) {
#ifdef _WIN32
        a.mov(x86::rcx, x86::r12);                    // arg1 = ctx
        a.mov(x86::rdx, x86::qword_ptr(x86::r15, 8)); // arg2 = left
        a.mov(x86::r8, x86::qword_ptr(x86::r15));     // arg3 = right
#else
        a.mov(x86::rdi, x86::r12);
        a.mov(x86::rsi, x86::qword_ptr(x86::r15, 8));
        a.mov(x86::rdx, x86::qword_ptr(x86::r15));
#endif
    } else {
#ifdef _WIN32
        a.mov(x86::rcx, x86::qword_ptr(x86::r15, 8)); // arg1 = left
        a.mov(x86::rdx, x86::qword_ptr(x86::r15));    // arg2 = right
#else
        a.mov(x86::rdi, x86::qword_ptr(x86::r15, 8));
        a.mov(x86::rsi, x86::qword_ptr(x86::r15));
#endif
    }
    // 调用辅助函数（r15/r12 是 callee-saved，安全）
#ifdef _WIN32
    a.sub(x86::rsp, 32);
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(fnPtr));
    a.call(x86::rax);
    a.add(x86::rsp, 32);
#else
    a.sub(x86::rsp, 16);
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(fnPtr));
    a.call(x86::rax);
    a.add(x86::rsp, 16);
#endif
    // pop 两个操作数
    a.add(x86::r15, 16);
    // push 结果（rax = raw bits）
    a.sub(x86::r15, 8);
    a.mov(x86::qword_ptr(x86::r15), x86::rax);
    // 可选：检查错误标志（除法/取模的除零检查）
    if (checkError) {
        // R143 修复：hasError 是 bool（1 字节），不能用 mov rax,[rax] 读取 8 字节，
        // 否则会读到 bool 之后的 padding/垃圾数据，误判为有错误导致提前跳转 epilogue。
        // 必须用 movzx 读取 1 字节并零扩展到 64 位。
        // L14: 不再直接跳 epilogue，而是调用 jitCheckAndRethrow 将错误转为可捕获异常。
        Label noError = a.new_label();
        a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::hasError)); // rax = hasError 指针 (bool*)
        a.movzx(x86::rax, x86::byte_ptr(x86::rax));                      // rax = *hasError (1 字节零扩展)
        a.test(x86::rax, x86::rax);
        a.jz(noError);
        // 有错误：同步栈顶并调用 jitCheckAndRethrow(ctx, r13)
        a.mov(x86::qword_ptr(x86::r12, jit_offset::stackTop), x86::r15);
#ifdef _WIN32
        a.mov(x86::rcx, x86::r12);
        a.mov(x86::rdx, x86::r13);
        a.sub(x86::rsp, 32);
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitCheckAndRethrow));
        a.call(x86::rax);
        a.add(x86::rsp, 32);
#else
        a.mov(x86::rdi, x86::r12);
        a.mov(x86::rsi, x86::r13);
        a.sub(x86::rsp, 16);
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitCheckAndRethrow));
        a.call(x86::rax);
        a.add(x86::rsp, 16);
#endif
        // rax = catchAddr 或 nullptr（未捕获时 hasError 已由 jitThrow 设置）
        a.test(x86::rax, x86::rax);
        a.jz(epilogue);
        // 捕获异常：恢复 r13/r15 并跳转到 catch 块
        a.mov(x86::r13, x86::qword_ptr(x86::r12, jit_offset::currentBp));
        a.mov(x86::r15, x86::qword_ptr(x86::r12, jit_offset::stackTop));
        a.jmp(x86::rax);
        a.bind(noError);
    }
}

// 生成"peek 栈顶值 → 设置参数 → 调用辅助函数 → pop → push 结果"的机器码
void JITBackend::emitCallUnaryHelper(x86::Assembler& a, void* fnPtr) {
#ifdef _WIN32
    a.mov(x86::rcx, x86::qword_ptr(x86::r15)); // arg1 = value
#else
    a.mov(x86::rdi, x86::qword_ptr(x86::r15));
#endif
#ifdef _WIN32
    a.sub(x86::rsp, 32);
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(fnPtr));
    a.call(x86::rax);
    a.add(x86::rsp, 32);
#else
    a.sub(x86::rsp, 16);
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(fnPtr));
    a.call(x86::rax);
    a.add(x86::rsp, 16);
#endif
    // pop 操作数
    a.add(x86::r15, 8);
    // push 结果
    a.sub(x86::r15, 8);
    a.mov(x86::qword_ptr(x86::r15), x86::rax);
}

// 生成"peek 栈顶两个值 → 设置 (left, right, cmpType) 参数 → 调用 → pop 两个 → push 结果"的机器码
// 签名: uint64_t jitOrderedCompare(uint64_t left, uint64_t right, int64_t cmpType)
void JITBackend::emitCallOrderedCompare(x86::Assembler& a, void* fnPtr, int64_t cmpType) {
#ifdef _WIN32
    a.mov(x86::rcx, x86::qword_ptr(x86::r15, 8));  // arg1 = left
    a.mov(x86::rdx, x86::qword_ptr(x86::r15));     // arg2 = right
    a.mov(x86::r8, static_cast<int32_t>(cmpType)); // arg3 = cmpType
#else
    a.mov(x86::rdi, x86::qword_ptr(x86::r15, 8));
    a.mov(x86::rsi, x86::qword_ptr(x86::r15));
    a.mov(x86::rdx, static_cast<int32_t>(cmpType));
#endif
#ifdef _WIN32
    a.sub(x86::rsp, 32);
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(fnPtr));
    a.call(x86::rax);
    a.add(x86::rsp, 32);
#else
    a.sub(x86::rsp, 16);
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(fnPtr));
    a.call(x86::rax);
    a.add(x86::rsp, 16);
#endif
    // pop 两个操作数
    a.add(x86::r15, 16);
    // push 结果
    a.sub(x86::r15, 8);
    a.mov(x86::qword_ptr(x86::r15), x86::rax);
}

// ============================================================
// R146 阶段 4：数组/字典/元组/索引辅助函数调用
// ============================================================

// OP_BUILD_ARRAY: void jitBuildArray(JitContext* ctx, uint8_t count)
// 辅助函数内部 pop count 个 + push 1 个，通过 ctx->stackTop 操作栈
void JITBackend::emitBuildArray(x86::Assembler& a, Label epilogue, uint8_t count) {
    // 更新 ctx->stackTop = r15
    a.mov(x86::qword_ptr(x86::r12, jit_offset::stackTop), x86::r15);
    // 设置参数
#ifdef _WIN32
    a.mov(x86::rcx, x86::r12);                    // arg1 = ctx
    a.mov(x86::rdx, static_cast<int32_t>(count)); // arg2 = count
#else
    a.mov(x86::rdi, x86::r12);
    a.mov(x86::rsi, static_cast<int32_t>(count));
#endif
#ifdef _WIN32
    a.sub(x86::rsp, 32);
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitBuildArray));
    a.call(x86::rax);
    a.add(x86::rsp, 32);
#else
    a.sub(x86::rsp, 16);
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitBuildArray));
    a.call(x86::rax);
    a.add(x86::rsp, 16);
#endif
    // 恢复 r15 = ctx->stackTop（辅助函数可能修改了栈顶）
    a.mov(x86::r15, x86::qword_ptr(x86::r12, jit_offset::stackTop));
    // 检查错误标志
    a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::hasError)); // rax = hasError 指针
    a.movzx(x86::rax, x86::byte_ptr(x86::rax));
    a.test(x86::rax, x86::rax);
    a.jnz(epilogue);
}

// OP_INDEX_GET: uint64_t jitIndexGet(JitContext* ctx, uint64_t objBits, uint64_t idxBits)
// 栈布局：[obj, idx]（idx 在栈顶），辅助函数 pop 2 + push 1
void JITBackend::emitIndexGet(x86::Assembler& a, Label epilogue) {
    a.mov(x86::qword_ptr(x86::r12, jit_offset::stackTop), x86::r15);
#ifdef _WIN32
    a.mov(x86::rcx, x86::r12);                    // arg1 = ctx
    a.mov(x86::rdx, x86::qword_ptr(x86::r15, 8)); // arg2 = obj
    a.mov(x86::r8, x86::qword_ptr(x86::r15));     // arg3 = idx
#else
    a.mov(x86::rdi, x86::r12);
    a.mov(x86::rsi, x86::qword_ptr(x86::r15, 8));
    a.mov(x86::rdx, x86::qword_ptr(x86::r15));
#endif
#ifdef _WIN32
    a.sub(x86::rsp, 32);
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitIndexGet));
    a.call(x86::rax);
    a.add(x86::rsp, 32);
#else
    a.sub(x86::rsp, 16);
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitIndexGet));
    a.call(x86::rax);
    a.add(x86::rsp, 16);
#endif
    // 恢复 r15 + pop 2 + push 1（辅助函数未操作栈，JIT 代码自己处理）
    a.add(x86::r15, 16);
    a.sub(x86::r15, 8);
    a.mov(x86::qword_ptr(x86::r15), x86::rax);
    // 检查错误标志
    a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::hasError));
    a.movzx(x86::rax, x86::byte_ptr(x86::rax));
    a.test(x86::rax, x86::rax);
    a.jnz(epilogue);
}

// OP_INDEX_SET_LOCAL: void jitIndexSetLocal(JitContext* ctx, uint8_t slot, int64_t* frameBase)
// 栈布局：[idx, val]（val 在栈顶），辅助函数 pop 2，不 push
// frameBase = r13（当前帧 basePointer）
void JITBackend::emitIndexSetLocal(x86::Assembler& a, Label epilogue, uint8_t slot) {
    a.mov(x86::qword_ptr(x86::r12, jit_offset::stackTop), x86::r15);
#ifdef _WIN32
    a.mov(x86::rcx, x86::r12);                   // arg1 = ctx
    a.mov(x86::rdx, static_cast<int32_t>(slot)); // arg2 = slot
    a.mov(x86::r8, x86::r13);                    // arg3 = frameBase (r13)
#else
    a.mov(x86::rdi, x86::r12);
    a.mov(x86::rsi, static_cast<int32_t>(slot));
    a.mov(x86::rdx, x86::r13);
#endif
#ifdef _WIN32
    a.sub(x86::rsp, 32);
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitIndexSetLocal));
    a.call(x86::rax);
    a.add(x86::rsp, 32);
#else
    a.sub(x86::rsp, 16);
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitIndexSetLocal));
    a.call(x86::rax);
    a.add(x86::rsp, 16);
#endif
    // 恢复 r15 = ctx->stackTop（辅助函数 pop 了 2 个元素）
    a.mov(x86::r15, x86::qword_ptr(x86::r12, jit_offset::stackTop));
    // 检查错误标志
    a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::hasError));
    a.movzx(x86::rax, x86::byte_ptr(x86::rax));
    a.test(x86::rax, x86::rax);
    a.jnz(epilogue);
}

// OP_BUILD_DICT: void jitBuildDict(JitContext* ctx, uint8_t pairCount)
// 辅助函数内部 pop 2*pairCount 个 + push 1 个，通过 ctx->stackTop 操作栈
void JITBackend::emitBuildDict(x86::Assembler& a, Label epilogue, uint8_t pairCount) {
    a.mov(x86::qword_ptr(x86::r12, jit_offset::stackTop), x86::r15);
#ifdef _WIN32
    a.mov(x86::rcx, x86::r12);                        // arg1 = ctx
    a.mov(x86::rdx, static_cast<int32_t>(pairCount)); // arg2 = pairCount
#else
    a.mov(x86::rdi, x86::r12);
    a.mov(x86::rsi, static_cast<int32_t>(pairCount));
#endif
#ifdef _WIN32
    a.sub(x86::rsp, 32);
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitBuildDict));
    a.call(x86::rax);
    a.add(x86::rsp, 32);
#else
    a.sub(x86::rsp, 16);
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitBuildDict));
    a.call(x86::rax);
    a.add(x86::rsp, 16);
#endif
    a.mov(x86::r15, x86::qword_ptr(x86::r12, jit_offset::stackTop));
    a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::hasError));
    a.movzx(x86::rax, x86::byte_ptr(x86::rax));
    a.test(x86::rax, x86::rax);
    a.jnz(epilogue);
}

// OP_BUILD_TUPLE: void jitBuildTuple(JitContext* ctx, uint8_t count)
// 与 emitBuildArray 等价，只是调用 jitBuildTuple
void JITBackend::emitBuildTuple(x86::Assembler& a, Label epilogue, uint8_t count) {
    a.mov(x86::qword_ptr(x86::r12, jit_offset::stackTop), x86::r15);
#ifdef _WIN32
    a.mov(x86::rcx, x86::r12);
    a.mov(x86::rdx, static_cast<int32_t>(count));
#else
    a.mov(x86::rdi, x86::r12);
    a.mov(x86::rsi, static_cast<int32_t>(count));
#endif
#ifdef _WIN32
    a.sub(x86::rsp, 32);
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitBuildTuple));
    a.call(x86::rax);
    a.add(x86::rsp, 32);
#else
    a.sub(x86::rsp, 16);
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitBuildTuple));
    a.call(x86::rax);
    a.add(x86::rsp, 16);
#endif
    a.mov(x86::r15, x86::qword_ptr(x86::r12, jit_offset::stackTop));
    a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::hasError));
    a.movzx(x86::rax, x86::byte_ptr(x86::rax));
    a.test(x86::rax, x86::rax);
    a.jnz(epilogue);
}

// OP_INDEX_SET_VAR: void jitIndexSetGlobal(JitContext* ctx, int64_t* slotPtr)
// slotPtr = globalSlots + slot*8（编译期解析 nameIdx→slot，运行时计算地址）
void JITBackend::emitIndexSetGlobal(x86::Assembler& a, Label epilogue, int slot) {
    a.mov(x86::qword_ptr(x86::r12, jit_offset::stackTop), x86::r15);
#ifdef _WIN32
    a.mov(x86::rcx, x86::r12); // arg1 = ctx
    // arg2 = slotPtr = [r12+24]（globalSlots） + slot*8
    a.mov(x86::rdx, x86::qword_ptr(x86::r12, jit_offset::globalSlots));
    a.lea(x86::rdx, x86::qword_ptr(x86::rdx, static_cast<int32_t>(slot) * 8));
#else
    a.mov(x86::rdi, x86::r12);
    a.mov(x86::rsi, x86::qword_ptr(x86::r12, jit_offset::globalSlots));
    a.lea(x86::rsi, x86::qword_ptr(x86::rsi, static_cast<int32_t>(slot) * 8));
#endif
#ifdef _WIN32
    a.sub(x86::rsp, 32);
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitIndexSetGlobal));
    a.call(x86::rax);
    a.add(x86::rsp, 32);
#else
    a.sub(x86::rsp, 16);
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitIndexSetGlobal));
    a.call(x86::rax);
    a.add(x86::rsp, 16);
#endif
    a.mov(x86::r15, x86::qword_ptr(x86::r12, jit_offset::stackTop));
    a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::hasError));
    a.movzx(x86::rax, x86::byte_ptr(x86::rax));
    a.test(x86::rax, x86::rax);
    a.jnz(epilogue);
}

// ============================================================
// R148 阶段 4c：类支持辅助函数
// ============================================================

// OP_CLASS_NEW: void jitClassNew(JitContext* ctx, const char* className, uint8_t argCount)
// R149 扩展：argCount > 0 时 jitClassNew 内部调用 init 方法，并设置 ctx->methodEntryPtr。
// JIT 代码检测 methodEntryPtr 非 null 时设置 returnAddr + r13 + jmp 入口；否则继续。
void JITBackend::emitClassNew(x86::Assembler& a, Label epilogue, const char* className, uint8_t argCount) {
    // R149: 设置 ctx->callerBp = r13（init 帧需记录调用者 basePointer）
    a.mov(x86::qword_ptr(x86::r12, jit_offset::callerBp), x86::r13);
    // 清空 ctx->methodEntryPtr（jitClassNew 内部会按需设置）
    a.mov(x86::qword_ptr(x86::r12, jit_offset::methodEntryPtr), 0);

    a.mov(x86::qword_ptr(x86::r12, jit_offset::stackTop), x86::r15);
    a.movabs(x86::r10, reinterpret_cast<uint64_t>(className)); // r10 = className ptr
#ifdef _WIN32
    a.mov(x86::rcx, x86::r12);                      // arg1 = ctx
    a.mov(x86::rdx, x86::r10);                      // arg2 = className
    a.mov(x86::r8, static_cast<int32_t>(argCount)); // arg3 = argCount
#else
    a.mov(x86::rdi, x86::r12);
    a.mov(x86::rsi, x86::r10);
    a.mov(x86::rdx, static_cast<int32_t>(argCount));
#endif
#ifdef _WIN32
    a.sub(x86::rsp, 32);
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitClassNew));
    a.call(x86::rax);
    a.add(x86::rsp, 32);
#else
    a.sub(x86::rsp, 16);
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitClassNew));
    a.call(x86::rax);
    a.add(x86::rsp, 16);
#endif
    a.mov(x86::r15, x86::qword_ptr(x86::r12, jit_offset::stackTop));
    a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::hasError));
    a.movzx(x86::rax, x86::byte_ptr(x86::rax));
    a.test(x86::rax, x86::rax);
    a.jnz(epilogue);

    // R149: 检查 ctx->methodEntryPtr 是否非 null（init 帧已设置）
    Label noInit = a.new_label();
    a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::methodEntryPtr)); // rax = methodEntryPtr
    a.test(x86::rax, x86::rax);
    a.jz(noInit);

    // init 帧已设置：写 returnAddr + 设置 r13 + jmp 入口
    Label returnLabel = a.new_label();
    // 找到新 frame（frames[*frameCount - 1]）并写入 returnAddr
    a.mov(x86::rcx, x86::qword_ptr(x86::r12, jit_offset::frameCount)); // rcx = &frameCount_
    a.mov(x86::rdx, x86::qword_ptr(x86::rcx));                         // rdx = *frameCount
    a.sub(x86::rdx, 1);                                                // rdx = idx（新 frame 索引）
    a.mov(x86::rcx, x86::qword_ptr(x86::r12, jit_offset::frames));     // rcx = frames base
    a.imul(x86::rdx, x86::rdx, 72);
    a.add(x86::rcx, x86::rdx); // rcx = &frames[idx]
    a.lea(x86::rax, x86::qword_ptr(returnLabel));
    a.mov(x86::qword_ptr(x86::rcx, 16), x86::rax); // frame->returnAddr = returnLabel

    // 设置 r13 = r15 + (methodLocalCount - 1) * 8
    a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::methodLocalCount)); // rax = methodLocalCount
    a.dec(x86::rax);
    a.imul(x86::rax, x86::rax, 8);
    a.lea(x86::r13, x86::qword_ptr(x86::r15, x86::rax));

    // jmp methodEntryPtr
    a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::methodEntryPtr));
    a.jmp(x86::rax);

    a.bind(returnLabel);
    a.bind(noInit);
}

// OP_INIT_FIELD: void jitInitField(JitContext* ctx, const char* fieldName)
// 栈布局：[instance, val]（val 在栈顶），辅助函数 pop val 后修改栈顶 instance
void JITBackend::emitInitField(x86::Assembler& a, Label epilogue, const char* fieldName) {
    a.mov(x86::qword_ptr(x86::r12, jit_offset::stackTop), x86::r15);
    a.movabs(x86::r10, reinterpret_cast<uint64_t>(fieldName)); // r10 = fieldName ptr
#ifdef _WIN32
    a.mov(x86::rcx, x86::r12); // arg1 = ctx
    a.mov(x86::rdx, x86::r10); // arg2 = fieldName
#else
    a.mov(x86::rdi, x86::r12);
    a.mov(x86::rsi, x86::r10);
#endif
#ifdef _WIN32
    a.sub(x86::rsp, 32);
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitInitField));
    a.call(x86::rax);
    a.add(x86::rsp, 32);
#else
    a.sub(x86::rsp, 16);
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitInitField));
    a.call(x86::rax);
    a.add(x86::rsp, 16);
#endif
    a.mov(x86::r15, x86::qword_ptr(x86::r12, jit_offset::stackTop));
    a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::hasError));
    a.movzx(x86::rax, x86::byte_ptr(x86::rax));
    a.test(x86::rax, x86::rax);
    a.jnz(epilogue);
}

// OP_DEFINE_CLASS: void jitDefineClass(JitContext* ctx, const char* className, const char* superClassName)
// 栈布局：[templateInstance]（pop 消费）
void JITBackend::emitDefineClass(x86::Assembler& a, Label epilogue, const char* className, const char* superClassName) {
    a.mov(x86::qword_ptr(x86::r12, jit_offset::stackTop), x86::r15);
    a.movabs(x86::r10, reinterpret_cast<uint64_t>(className));      // r10 = className
    a.movabs(x86::r11, reinterpret_cast<uint64_t>(superClassName)); // r11 = superClassName
#ifdef _WIN32
    a.mov(x86::rcx, x86::r12); // arg1 = ctx
    a.mov(x86::rdx, x86::r10); // arg2 = className
    a.mov(x86::r8, x86::r11);  // arg3 = superClassName
#else
    a.mov(x86::rdi, x86::r12);
    a.mov(x86::rsi, x86::r10);
    a.mov(x86::rdx, x86::r11);
#endif
#ifdef _WIN32
    a.sub(x86::rsp, 32);
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitDefineClass));
    a.call(x86::rax);
    a.add(x86::rsp, 32);
#else
    a.sub(x86::rsp, 16);
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitDefineClass));
    a.call(x86::rax);
    a.add(x86::rsp, 16);
#endif
    a.mov(x86::r15, x86::qword_ptr(x86::r12, jit_offset::stackTop));
    a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::hasError));
    a.movzx(x86::rax, x86::byte_ptr(x86::rax));
    a.test(x86::rax, x86::rax);
    a.jnz(epilogue);
}

// ============================================================
// R149/R160：成员访问与方法调用
// ============================================================

// OP_MEMBER_GET: void jitMemberGet(JitContext* ctx, const char* fieldName)
// R160: OP_MEMBER_GET 带 inline cache 版本
// 调用 jitMemberGetWithIC(ctx, fieldName, callSiteId, backendPtr)
// callSiteId 编译期分配，运行时索引 memberGetIC_[callSiteId]
void JITBackend::emitMemberGet(x86::Assembler& a, Label epilogue, const char* fieldName) {
    uint64_t callSiteId = nextCallSiteId_++;
    a.mov(x86::qword_ptr(x86::r12, jit_offset::stackTop), x86::r15);
    a.movabs(x86::r10, reinterpret_cast<uint64_t>(fieldName)); // r10 = fieldName
#ifdef _WIN32
    a.mov(x86::rcx, x86::r12);                           // arg1 = ctx
    a.mov(x86::rdx, x86::r10);                           // arg2 = fieldName
    a.movabs(x86::r8, callSiteId);                       // arg3 = callSiteId
    a.movabs(x86::r9, reinterpret_cast<uint64_t>(this)); // arg4 = backendPtr
#else
    a.mov(x86::rdi, x86::r12);
    a.mov(x86::rsi, x86::r10);
    a.movabs(x86::rdx, callSiteId);
    a.movabs(x86::rcx, reinterpret_cast<uint64_t>(this));
#endif
#ifdef _WIN32
    a.sub(x86::rsp, 32);
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitMemberGetWithIC));
    a.call(x86::rax);
    a.add(x86::rsp, 32);
#else
    a.sub(x86::rsp, 16);
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitMemberGetWithIC));
    a.call(x86::rax);
    a.add(x86::rsp, 16);
#endif
    a.mov(x86::r15, x86::qword_ptr(x86::r12, jit_offset::stackTop));
    a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::hasError));
    a.movzx(x86::rax, x86::byte_ptr(x86::rax));
    a.test(x86::rax, x86::rax);
    a.jnz(epilogue);
}

// OP_MEMBER_SET_VAR: void jitMemberSetVar(JitContext* ctx, int64_t* slotPtr, const char* fieldName)
// slotPtr = globalSlots + slot*8（编译期解析 varIdx→slot）
// 栈布局：[val]，辅助函数 pop val 写入 *slotPtr.field
void JITBackend::emitMemberSetVar(x86::Assembler& a, Label epilogue, int slot, const char* fieldName) {
    a.mov(x86::qword_ptr(x86::r12, jit_offset::stackTop), x86::r15);
    a.movabs(x86::r10, reinterpret_cast<uint64_t>(fieldName)); // r10 = fieldName
#ifdef _WIN32
    a.mov(x86::rcx, x86::r12); // arg1 = ctx
    // arg2 = slotPtr = [r12+24]（globalSlots） + slot*8
    a.mov(x86::rdx, x86::qword_ptr(x86::r12, jit_offset::globalSlots));
    a.lea(x86::rdx, x86::qword_ptr(x86::rdx, static_cast<int32_t>(slot) * 8));
    a.mov(x86::r8, x86::r10); // arg3 = fieldName
#else
    a.mov(x86::rdi, x86::r12);
    a.mov(x86::rsi, x86::qword_ptr(x86::r12, jit_offset::globalSlots));
    a.lea(x86::rsi, x86::qword_ptr(x86::rsi, static_cast<int32_t>(slot) * 8));
    a.mov(x86::rdx, x86::r10);
#endif
#ifdef _WIN32
    a.sub(x86::rsp, 32);
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitMemberSetVar));
    a.call(x86::rax);
    a.add(x86::rsp, 32);
#else
    a.sub(x86::rsp, 16);
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitMemberSetVar));
    a.call(x86::rax);
    a.add(x86::rsp, 16);
#endif
    a.mov(x86::r15, x86::qword_ptr(x86::r12, jit_offset::stackTop));
    a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::hasError));
    a.movzx(x86::rax, x86::byte_ptr(x86::rax));
    a.test(x86::rax, x86::rax);
    a.jnz(epilogue);
}

// OP_MEMBER_SET_LOCAL: void jitMemberSetLocal(JitContext* ctx, uint8_t slot, int64_t* frameBase, const char*
// fieldName) frameBase = r13（当前帧 basePointer） 栈布局：[val]，辅助函数 pop val 写入 *(frameBase - slot).field
void JITBackend::emitMemberSetLocal(x86::Assembler& a, Label epilogue, uint8_t slot, const char* fieldName) {
    a.mov(x86::qword_ptr(x86::r12, jit_offset::stackTop), x86::r15);
    a.movabs(x86::r10, reinterpret_cast<uint64_t>(fieldName)); // r10 = fieldName
#ifdef _WIN32
    a.mov(x86::rcx, x86::r12);                   // arg1 = ctx
    a.mov(x86::rdx, static_cast<int32_t>(slot)); // arg2 = slot
    a.mov(x86::r8, x86::r13);                    // arg3 = frameBase (r13)
    a.mov(x86::r9, x86::r10);                    // arg4 = fieldName
#else
    a.mov(x86::rdi, x86::r12);
    a.mov(x86::rsi, static_cast<int32_t>(slot));
    a.mov(x86::rdx, x86::r13);
    a.mov(x86::rcx, x86::r10);
#endif
#ifdef _WIN32
    a.sub(x86::rsp, 32);
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitMemberSetLocal));
    a.call(x86::rax);
    a.add(x86::rsp, 32);
#else
    a.sub(x86::rsp, 16);
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitMemberSetLocal));
    a.call(x86::rax);
    a.add(x86::rsp, 16);
#endif
    a.mov(x86::r15, x86::qword_ptr(x86::r12, jit_offset::stackTop));
    a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::hasError));
    a.movzx(x86::rax, x86::byte_ptr(x86::rax));
    a.test(x86::rax, x86::rax);
    a.jnz(epilogue);
}

// OP_METHOD_CALL / OP_SUPER_CALL 代码生成
// 栈布局（调用前，向低地址增长）：[argN-1]...[arg0][receiver] ← r15 指向 argN-1
// 栈布局（调用后）：[extraSlots...][defaults...][args...][fields...][this] ← r15 指向最后一个 extraSlot
void JITBackend::emitMethodCall(x86::Assembler& a, Label epilogue, const char* methodName, uint8_t argCount,
                                uint8_t receiverLocalSlotByte, int receiverGlobalSlot, bool isSuperCall,
                                const char* superClassName) {
    Label returnLabel = a.new_label();

    // 方法调用 IC：分配 callSiteId 并确保 methodCallIC_ 容量
    uint64_t methodCallSiteId = nextMethodCallSiteId_++;
    if (methodCallSiteId >= methodCallIC_.size()) {
        methodCallIC_.resize(methodCallSiteId + 1);
    }

    // 1. 计算 receiverSlotPtr → r10
    if (receiverLocalSlotByte != RuntimeLimits::NO_SLOT) {
        // 接收者是当前帧的局部变量：r10 = r13 - receiverLocalSlotByte * 8
        a.lea(x86::r10, x86::qword_ptr(x86::r13, -static_cast<int32_t>(receiverLocalSlotByte) * 8));
    } else if (receiverGlobalSlot >= 0) {
        // 接收者是全局变量：r10 = globalSlots + receiverGlobalSlot * 8
        a.mov(x86::r10, x86::qword_ptr(x86::r12, jit_offset::globalSlots)); // r10 = globalSlots base
        a.lea(x86::r10, x86::qword_ptr(x86::r10, static_cast<int32_t>(receiverGlobalSlot) * 8));
    } else {
        a.xor_(x86::r10, x86::r10); // nullptr（无 writeBack）
    }

    // 2. 打包 packedArgs → r11
    //    bit 0-7: argCount | bit 8-15: receiverLocalSlotByte | bit 16: isInitCall(0) | bit 17: isSuperCall
    int64_t packedArgs = static_cast<int64_t>(argCount) | (static_cast<int64_t>(receiverLocalSlotByte) << 8) |
                         (static_cast<int64_t>(isSuperCall ? 1 : 0) << 17);
    a.mov(x86::r11, packedArgs);

    // 3. 存储 callerBp (r13) 到 ctx->callerBp (offset 96)
    a.mov(x86::qword_ptr(x86::r12, jit_offset::callerBp), x86::r13);

    // 4. 同步栈顶到 ctx->stackTop (offset 48)
    a.mov(x86::qword_ptr(x86::r12, jit_offset::stackTop), x86::r15);

    // 5. 调用 jitMethodCall(ctx, methodName, packedArgs, receiverSlotPtr, superClassName, callSiteId, backendPtr)
    //    Win32 ABI: rcx, rdx, r8, r9, [rsp+32], [rsp+40], [rsp+48]
    //    SysV ABI:  rdi, rsi, rdx, rcx, r8, r9, [rsp]
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(methodName));
#ifdef _WIN32
    a.sub(x86::rsp, 64);       // shadow space (32) + arg5/6/7 slots (24) + 8B 对齐填充
    a.mov(x86::rcx, x86::r12); // arg1 = ctx
    a.mov(x86::rdx, x86::rax); // arg2 = methodName
    a.mov(x86::r8, x86::r11);  // arg3 = packedArgs
    a.mov(x86::r9, x86::r10);  // arg4 = receiverSlotPtr
    if (isSuperCall) {
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(superClassName));
        a.mov(x86::qword_ptr(x86::rsp, 32), x86::rax); // arg5 = superClassName (stack)
    } else {
        a.mov(x86::qword_ptr(x86::rsp, 32), 0);
    }
    a.mov(x86::qword_ptr(x86::rsp, 40), methodCallSiteId); // arg6 = callSiteId
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(this));
    a.mov(x86::qword_ptr(x86::rsp, 48), x86::rax); // arg7 = backendPtr
#else
    a.sub(x86::rsp, 16);       // 16-byte alignment + arg7 slot
    a.mov(x86::rdi, x86::r12); // arg1 = ctx
    a.mov(x86::rsi, x86::rax); // arg2 = methodName
    a.mov(x86::rdx, x86::r11); // arg3 = packedArgs
    a.mov(x86::rcx, x86::r10); // arg4 = receiverSlotPtr
    if (isSuperCall) {
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(superClassName));
        a.mov(x86::r8, x86::rax); // arg5 = superClassName
    } else {
        a.xor_(x86::r8, x86::r8);
    }
    a.mov(x86::r9, methodCallSiteId); // arg6 = callSiteId
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(this));
    a.mov(x86::qword_ptr(x86::rsp, 0), x86::rax); // arg7 = backendPtr (stack)
#endif
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitMethodCall));
    a.call(x86::rax);
#ifdef _WIN32
    a.add(x86::rsp, 64);
#else
    a.add(x86::rsp, 16);
#endif

    // 6. 恢复 r15（jitMethodCall 修改了栈）
    a.mov(x86::r15, x86::qword_ptr(x86::r12, jit_offset::stackTop));

    // 7. 检查错误
    a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::hasError));
    a.movzx(x86::rax, x86::byte_ptr(x86::rax));
    a.test(x86::rax, x86::rax);
    a.jnz(epilogue);

    // 8. 在新 frame 上设置 returnAddr（jitMethodCall 无法访问 JIT Label，需在调用后写入）
    //    新 frame 在 frames[*frameCount - 1]
    a.mov(x86::rcx, x86::qword_ptr(x86::r12, jit_offset::frameCount)); // rcx = &frameCount_
    a.mov(x86::rdx, x86::qword_ptr(x86::rcx));                         // rdx = *frameCount
    a.sub(x86::rdx, 1);                                                // rdx = idx（新 frame 索引）
    a.mov(x86::rcx, x86::qword_ptr(x86::r12, jit_offset::frames));     // rcx = frames base
    a.imul(x86::rdx, x86::rdx, 72);
    a.add(x86::rcx, x86::rdx); // rcx = &frames[idx]
    a.lea(x86::rax, x86::qword_ptr(returnLabel));
    a.mov(x86::qword_ptr(x86::rcx, 16), x86::rax); // frame->returnAddr = returnLabel

    // 9. 设置 r13 = r15 + (methodLocalCount - 1) * 8（this 在 slot 0 = 最高地址）
    a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::methodLocalCount)); // rax = methodLocalCount
    a.dec(x86::rax);
    a.imul(x86::rax, x86::rax, 8);
    a.lea(x86::r13, x86::qword_ptr(x86::r15, x86::rax));

    // 10. jmp methodEntryPtr
    a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::methodEntryPtr)); // rax = methodEntryPtr
    // Bug #50 fix: methodEntryPtr 为空防御。若 jitMethodCall 未能解析出方法入口
    // （例如类注册表缺失/方法名不匹配）却未置 hasError，rax 可能为 0，
    // 直接 jmp rax 会跳到地址 0 崩溃。检查后跳转 epilogue 安全退出。
    a.test(x86::rax, x86::rax);
    a.jz(epilogue);
    a.jmp(x86::rax);

    // 11. 绑定返回 Label（方法 OP_RETURN 后跳回此处）
    a.bind(returnLabel);
}

// ============================================================
// R144/R152/R153/R161：类型检查与类型反馈
// ============================================================

// R144 优化：INT 类型标签检查
// R152: specializeIntMode_=true 时跳过类型检查（INT 特化版本）
// R153: specializeFloatMode_=true 时无条件跳转到 failLabel（跳过 INT 原生路径）
void JITBackend::emitCheckInt(x86::Assembler& a, x86::Gp val, Label failLabel) {
    if (specializeIntMode_) {
        // R159: INT 特化模式 — 类型守卫 + 即时反优化
        // 检查 tag == 0x7FF8 (INT)，不匹配则触发反优化降级到 Tier 1
        Label notInt = a.new_label();
        Label intPath = a.new_label();
        a.mov(x86::rdx, val);
        a.shr(x86::rdx, 48);
        a.cmp(x86::edx, 0x7FF8);
        a.jne(notInt);
        a.jmp(intPath); // INT → 跳过 deopt 代码块，进入 INT 原生路径
        a.bind(notInt);
        // 即时反优化：保存帧状态 → 调用 jitDeoptimize → 跳转 generic 路径
        a.mov(x86::qword_ptr(x86::r12, jit_offset::osrSavedBp), x86::r13); // osrSavedBp = r13
        a.mov(x86::qword_ptr(x86::r12, jit_offset::osrSavedSp), x86::r15); // osrSavedSp = r15
        a.mov(x86::qword_ptr(x86::r12, jit_offset::stackTop), x86::r15);   // 同步栈顶
        a.mov(x86::rcx, x86::r12);                                         // arg1 = ctx
        a.mov(x86::rdx, static_cast<int32_t>(currentChunkIdx_));           // arg2 = chunkIdx
#ifdef _WIN32
        a.sub(x86::rsp, 32); // shadow space (32) + 8B 对齐填充
#else
        a.sub(x86::rsp, 16); // 16-byte alignment
#endif
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitDeoptimize));
        a.call(x86::rax);
#ifdef _WIN32
        a.add(x86::rsp, 32);
#else
        a.add(x86::rsp, 16);
#endif
        // r13/r15 由 callee-saved 保证，无需恢复
        // Bug #19 fix: jitDeoptimize 调用破坏了 rax（用于 movabs+call），而 failLabel 处的
        // emitRecordTypeFeedback 读取 rax 作为操作数类型反馈。必须从栈重新加载原始值。
        a.mov(x86::rax, x86::qword_ptr(x86::r15)); // 恢复 rax = 栈顶操作数（right）
        a.jmp(failLabel);                          // 当前操作走 generic 路径正确处理
        a.bind(intPath);
        return;
    }
    if (specializeFloatMode_) {
        // R159: FLOAT 特化模式 — 类型守卫 + 即时反优化
        // FLOAT 判定：tag < 0x7FF8（普通 double）或 tag > 0x7FFB（NAN_BOXED_FLOAT_MARKER 0x7FFC）
        // 非 FLOAT（tag in [0x7FF8, 0x7FFB] = INT/BOOL/NULL/POINTER）则触发反优化降级
        a.mov(x86::rdx, val);
        a.shr(x86::rdx, 48);
        a.cmp(x86::edx, 0x7FF8);
        a.jb(failLabel); // tag < 0x7FF8 → FLOAT → generic 路径
        a.cmp(x86::edx, 0x7FFB);
        a.ja(failLabel); // tag > 0x7FFB → FLOAT → generic 路径
        // tag in [0x7FF8, 0x7FFB] → INT/BOOL/NULL/POINTER → 即时反优化
        a.mov(x86::qword_ptr(x86::r12, jit_offset::osrSavedBp), x86::r13); // osrSavedBp = r13
        a.mov(x86::qword_ptr(x86::r12, jit_offset::osrSavedSp), x86::r15); // osrSavedSp = r15
        a.mov(x86::qword_ptr(x86::r12, jit_offset::stackTop), x86::r15);   // 同步栈顶
        a.mov(x86::rcx, x86::r12);                                         // arg1 = ctx
        a.mov(x86::rdx, static_cast<int32_t>(currentChunkIdx_));           // arg2 = chunkIdx
#ifdef _WIN32
        a.sub(x86::rsp, 32); // shadow space (32) + 8B 对齐填充
#else
        a.sub(x86::rsp, 16); // 16-byte alignment
#endif
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitDeoptimize));
        a.call(x86::rax);
#ifdef _WIN32
        a.add(x86::rsp, 32);
#else
        a.add(x86::rsp, 16);
#endif
        // Bug #19 fix: jitDeoptimize 调用破坏了 rax，从栈重新加载操作数供类型反馈使用
        a.mov(x86::rax, x86::qword_ptr(x86::r15)); // 恢复 rax = 栈顶操作数（right）
        a.jmp(failLabel);                          // 反优化后走 generic 路径正确处理
        return;
    }
    a.mov(x86::rdx, val);
    a.shr(x86::rdx, 48);
    a.cmp(x86::edx, 0x7FF8);
    a.jne(failLabel);
}

// R152: 类型反馈收集
// 在算术/比较指令的 genericXXX 路径入口调用，统计操作数类型分布。
void JITBackend::emitRecordTypeFeedback(x86::Assembler& a, x86::Gp val, size_t chunkIdx) {
    // R153: FLOAT 特化模式下不生成类型反馈收集代码（类型反馈已收集完毕）
    if (specializeFloatMode_) {
        (void)val;
        (void)chunkIdx;
        return;
    }
    Label isFloat = a.new_label();
    Label done = a.new_label();
    // r10 = ctx->typeFeedback 数组指针（offset 128）
    a.mov(x86::r10, x86::qword_ptr(x86::r12, jit_offset::typeFeedback));
    // r11 = val 的高 16 位 tag
    a.mov(x86::r11, val);
    a.shr(x86::r11, 48);
    // 判断 tag 是否在 [0x7FF9, 0x7FFB] 范围（BOOL/NULL/POINTER）
    // tag < 0x7FF9 → FLOAT（含普通 double 位模式，如 1.5 的 tag=0x3FF8）
    a.cmp(x86::r11d, 0x7FF9);
    a.jb(isFloat);
    // tag > 0x7FFB → FLOAT（含 NAN_BOXED_FLOAT_MARKER 0x7FFC）
    a.cmp(x86::r11d, 0x7FFB);
    a.ja(isFloat);
    // tag in [0x7FF9, 0x7FFB] → otherCount++ (TypeFeedback offset 16)
    a.inc(x86::qword_ptr(x86::r10, static_cast<int32_t>(chunkIdx * 24 + 16)));
    a.jmp(done);
    // FLOAT → floatCount++ (TypeFeedback offset 8)
    a.bind(isFloat);
    a.inc(x86::qword_ptr(x86::r10, static_cast<int32_t>(chunkIdx * 24 + 8)));
    a.bind(done);
}

// R161 perf: FLOAT 原生算术路径
// 在 emitCheckInt 失败后（非特化模式）调用，检查两个操作数是否都是 FLOAT，
// 若是则执行原生 SSE2 算术（addsd/subsd/mulsd/divsd），避免 C++ 辅助函数调用开销。
// @param op 0=add, 1=sub, 2=mul, 3=div
// @param failLabel 非 FLOAT 类型时跳转的目标（genericXXX 标签）
// @param endLabel 成功后跳转的目标（xxxEnd 标签）
// @param chunkIdx 当前 chunk 索引（类型反馈收集用）
// @param divByZeroLabel 除零错误标签（仅 op=3 使用）
void JITBackend::emitFloatBinaryArith(x86::Assembler& a, int op, Label failLabel, Label endLabel, size_t chunkIdx,
                                      Label divByZeroLabel) {
    // Bug #53 fix: 除法路径必须提供有效的 divByZeroLabel。默认构造的 Label
    // 无效（isValid()==false），若调用方遗漏传入，op==3 分支的 a.jz(divByZeroLabel)
    // 会生成无效跳转目标。此处断言在编译期捕获调用错误。
    if (op == 3) {
        assert(divByZeroLabel.is_valid() && "divByZeroLabel required for division");
    }
    Label checkLeft = a.new_label();
    Label doArith = a.new_label();
    Label storeResult = a.new_label();
    Label replaceNan = a.new_label();

    // 检查 rax (right) 是否是 FLOAT
    a.mov(x86::rdx, x86::rax);
    a.shr(x86::rdx, 48);
    a.cmp(x86::edx, 0x7FF8);
    a.jb(checkLeft); // tag < 0x7FF8 → FLOAT
    a.cmp(x86::edx, 0x7FFB);
    a.ja(checkLeft);  // tag > 0x7FFB → FLOAT
    a.jmp(failLabel); // tag in [0x7FF8, 0x7FFB] → 非FLOAT → genericXXX

    a.bind(checkLeft);
    // 检查 rcx (left) 是否是 FLOAT
    a.mov(x86::rdx, x86::rcx);
    a.shr(x86::rdx, 48);
    a.cmp(x86::edx, 0x7FF8);
    a.jb(doArith); // tag < 0x7FF8 → FLOAT
    a.cmp(x86::edx, 0x7FFB);
    a.ja(doArith);    // tag > 0x7FFB → FLOAT
    a.jmp(failLabel); // tag in [0x7FF8, 0x7FFB] → 非FLOAT → genericXXX

    a.bind(doArith);
    // R161 perf: 收集类型反馈（FLOAT 操作走原生路径也需收集，以触发 FLOAT 特化重编译）
    // emitRecordTypeFeedback 检查 rax 的 tag 并递增 floatCount（FLOAT 类型），仅修改 r10/r11
    emitRecordTypeFeedback(a, x86::rax, chunkIdx);
    // 从 raw bits 提取 double 到 xmm 寄存器
    a.movq(x86::xmm0, x86::rcx); // xmm0 = left as double
    a.movq(x86::xmm1, x86::rax); // xmm1 = right as double

    // 除零检查（仅除法）
    if (op == 3) {
        // 检查 xmm1 (right/除数) 是否为 0.0（包括 +0.0 和 -0.0）
        // 清除符号位后检查是否为 0
        a.movq(x86::rax, x86::xmm1);
        a.movabs(x86::rdx, 0x7FFFFFFFFFFFFFFFULL);
        a.and_(x86::rax, x86::rdx);
        a.test(x86::rax, x86::rax);
        a.jz(divByZeroLabel);
    }

    // 执行算术运算
    switch (op) {
    case 0:
        a.addsd(x86::xmm0, x86::xmm1);
        break; // xmm0 = left + right
    case 1:
        a.subsd(x86::xmm0, x86::xmm1);
        break; // xmm0 = left - right
    case 2:
        a.mulsd(x86::xmm0, x86::xmm1);
        break; // xmm0 = left * right
    case 3:
        a.divsd(x86::xmm0, x86::xmm1);
        break; // xmm0 = left / right
    }

    // 检查结果是否落入 boxed NaN 范围
    a.movq(x86::rax, x86::xmm0); // rax = result raw bits
    a.mov(x86::rdx, x86::rax);
    a.shr(x86::rdx, 48);
    a.cmp(x86::edx, 0x7FF8);
    a.jb(storeResult); // tag < 0x7FF8 → 普通 double
    a.cmp(x86::edx, 0x7FFB);
    a.jbe(replaceNan); // tag in [0x7FF8, 0x7FFB] → 替换
    // tag > 0x7FFB → 普通 double（如 -inf），fall through 到 storeResult

    a.bind(storeResult);
    a.add(x86::r15, 16); // pop 两个操作数
    a.sub(x86::r15, 8);  // push 结果
    a.mov(x86::qword_ptr(x86::r15), x86::rax);
    a.jmp(endLabel);

    a.bind(replaceNan);
    a.movabs(x86::rax, JIT_NAN_BOXED_FLOAT_MARKER);
    a.jmp(storeResult);
}

// ============================================================
// R154 阶段 5d：嵌套左值赋值链 + OP_LEN + OP_DUP_N
// ============================================================

// OP_LEN: void jitLen(JitContext* ctx)
void JITBackend::emitLen(x86::Assembler& a, Label epilogue) {
    a.mov(x86::qword_ptr(x86::r12, jit_offset::stackTop), x86::r15);
#ifdef _WIN32
    a.mov(x86::rcx, x86::r12); // arg1 = ctx
#else
    a.mov(x86::rdi, x86::r12);
#endif
#ifdef _WIN32
    a.sub(x86::rsp, 32);
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitLen));
    a.call(x86::rax);
    a.add(x86::rsp, 32);
#else
    a.sub(x86::rsp, 16);
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitLen));
    a.call(x86::rax);
    a.add(x86::rsp, 16);
#endif
    a.mov(x86::r15, x86::qword_ptr(x86::r12, jit_offset::stackTop));
    a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::hasError));
    a.movzx(x86::rax, x86::byte_ptr(x86::rax));
    a.test(x86::rax, x86::rax);
    a.jnz(epilogue);
}

// OP_DUP_N: inline peek(depth) + push（无需 C++ helper）
// JIT 栈向下增长：peek(depth) = [r15 + depth*8]
void JITBackend::emitDupN(x86::Assembler& a, uint8_t depth) {
    a.mov(x86::rax, x86::qword_ptr(x86::r15, static_cast<int32_t>(depth) * 8)); // rax = peek(depth)
    a.sub(x86::r15, 8);                                                         // push
    a.mov(x86::qword_ptr(x86::r15), x86::rax);
}

// OP_LOAD_MUTATED: inline push(lastMutatedReceiver_)（无需 C++ helper）
void JITBackend::emitLoadMutated(x86::Assembler& a) {
    a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::lastMutatedReceiverPtr)); // rax = lastMutatedReceiverPtr
    a.mov(x86::rax, x86::qword_ptr(x86::rax)); // rax = *lastMutatedReceiverPtr = lastMutatedReceiver_ bits
    a.sub(x86::r15, 8);                        // push
    a.mov(x86::qword_ptr(x86::r15), x86::rax);
}

// OP_INDEX_SET: void jitIndexSet(JitContext* ctx)
// 辅助函数内部 pop 3 个（obj/innerIdx/val），不 push；变异后容器存入 lastMutatedReceiver_
void JITBackend::emitIndexSet(x86::Assembler& a, Label epilogue) {
    a.mov(x86::qword_ptr(x86::r12, jit_offset::stackTop), x86::r15);
#ifdef _WIN32
    a.mov(x86::rcx, x86::r12); // arg1 = ctx
#else
    a.mov(x86::rdi, x86::r12);
#endif
#ifdef _WIN32
    a.sub(x86::rsp, 32);
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitIndexSet));
    a.call(x86::rax);
    a.add(x86::rsp, 32);
#else
    a.sub(x86::rsp, 16);
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitIndexSet));
    a.call(x86::rax);
    a.add(x86::rsp, 16);
#endif
    a.mov(x86::r15, x86::qword_ptr(x86::r12, jit_offset::stackTop));
    a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::hasError));
    a.movzx(x86::rax, x86::byte_ptr(x86::rax));
    a.test(x86::rax, x86::rax);
    a.jnz(epilogue);
}

// OP_WRITEBACK_*_VAR: inline *globalSlots[slot] = lastMutatedReceiver_; lastMutatedReceiver_ = NULL
void JITBackend::emitWritebackVar(x86::Assembler& a, int slot) {
    // 1. 加载 lastMutatedReceiver_ raw bits
    a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::lastMutatedReceiverPtr)); // rax = lastMutatedReceiverPtr
    a.mov(x86::rax, x86::qword_ptr(x86::rax)); // rax = *lastMutatedReceiverPtr = lastMutatedReceiver_ bits
    // 2. 写入 globalSlots[slot]
    a.mov(x86::rcx, x86::qword_ptr(x86::r12, jit_offset::globalSlots)); // rcx = globalSlots base
    a.mov(x86::qword_ptr(x86::rcx, static_cast<int32_t>(slot) * 8), x86::rax);
    // 3. 清空 lastMutatedReceiver_ = JIT_NULL_BITS
    a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::lastMutatedReceiverPtr)); // rax = lastMutatedReceiverPtr
    a.mov(x86::qword_ptr(x86::rax), static_cast<int64_t>(JIT_NULL_BITS));          // *lastMutatedReceiverPtr = NULL
}

// OP_WRITEBACK_*_LOCAL: inline stack_[bp - slot*8] = lastMutatedReceiver_; lastMutatedReceiver_ = NULL
void JITBackend::emitWritebackLocal(x86::Assembler& a, uint8_t slot) {
    // 1. 加载 lastMutatedReceiver_ raw bits
    a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::lastMutatedReceiverPtr)); // rax = lastMutatedReceiverPtr
    a.mov(x86::rax, x86::qword_ptr(x86::rax)); // rax = *lastMutatedReceiverPtr = lastMutatedReceiver_ bits
    // 2. 写入 [r13 - slot*8]（局部变量栈槽）
    a.mov(x86::qword_ptr(x86::r13, -static_cast<int32_t>(slot) * 8), x86::rax);
    // 3. 清空 lastMutatedReceiver_ = JIT_NULL_BITS
    a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::lastMutatedReceiverPtr)); // rax = lastMutatedReceiverPtr
    a.mov(x86::qword_ptr(x86::rax), static_cast<int64_t>(JIT_NULL_BITS));          // *lastMutatedReceiverPtr = NULL
}

// ============================================================
// Chunk 扫描辅助函数
// ============================================================

// 统计 chunk 中 OP_MEMBER_GET 数量，累加到 nextCallSiteId_
void JITBackend::countMemberGetInChunk(const BytecodeChunk& chunk) {
    for (size_t i = 0; i < chunk.code.size();) {
        uint8_t op = chunk.code[i];
        if (op == static_cast<uint8_t>(OpCode::OP_MEMBER_GET)) {
            nextCallSiteId_++;
        }
        i += chunk.instructionSizeAt(i);
    }
}

// 扫描 chunk 收集 OP_DEFINE_CLASS 类名到 classNameSet_
void JITBackend::collectClassNamesFromChunk(const BytecodeChunk& c) {
    const auto& code = c.code;
    for (size_t i = 0; i < code.size();) {
        OpCode op = static_cast<OpCode>(code[i]);
        if (op == OpCode::OP_DEFINE_CLASS && i + 4 < code.size()) {
            uint16_t nameIdx = static_cast<uint16_t>(code[i + 1]) | (static_cast<uint16_t>(code[i + 2]) << 8);
            if (nameIdx < c.constants.size()) {
                classNameSet_.insert(c.constants[nameIdx].stringVal());
            }
        }
        i += c.instructionSizeAt(i);
    }
}

// ============================================================
// P0 重构：大型 OpCode case 提取为 emit* 子方法
// ------------------------------------------------------------
// 原 compileAllChunks 单函数 ~3930 行（圈复杂度 >200），以下函数从 case body 原样提取。
// 验证（操作数越界/常量池越界）保留在 case body，helper 仅负责 codegen。
// ============================================================

// ------------------------------------------------------------------
// OP_RETURN：函数返回（含 close upvalue + 方法调用 writeBack + 帧恢复）
// ------------------------------------------------------------------
void JITBackend::emitReturn(x86::Assembler& a, Label epilogue) {
    // pop 返回值到 rax
    a.mov(x86::rax, x86::qword_ptr(x86::r15));
    a.add(x86::r15, 8);

    // 检查 *frameCount == 0 → epilogue（mainChunk 末帧返回）
    a.mov(x86::rcx, x86::qword_ptr(x86::r12, jit_offset::frameCount)); // rcx = &frameCount_
    a.mov(x86::rdx, x86::qword_ptr(x86::rcx));                         // rdx = *frameCount
    a.test(x86::rdx, x86::rdx);
    a.jz(epilogue);

    // R161 perf: 保存缓存的 rbx(JIT_INT48_MASK) 到 [rbp-48] 暂存槽。
    // [rbp-48] 是操作数栈基址边界（非栈元素），OP_RETURN 期间安全覆写。
    // r14(JIT_INT_TAG_BASE) 不被本路径覆写（returnAddr 改用 rdx），无需保存。
    a.mov(x86::qword_ptr(x86::rbp, -48), x86::rbx);

    // R156: close upvalue（在 *frameCount -= 1 之前，r13 仍是当前帧 basePtr）
    // P0 UAF 修复：关闭当前帧的 open upvalues，防止栈槽被回收后悬垂引用
    // 同时 pop frameUpvaluesStack_ 中属于当前帧的 upvalues
    a.mov(x86::rbx, x86::rax);                                       // 保存返回值到 rbx（callee-saved）
    a.mov(x86::qword_ptr(x86::r12, jit_offset::stackTop), x86::r15); // 同步栈顶
#ifdef _WIN32
    // Win32: rcx=ctx, rdx=fromAddr(r13), r8=newFrameCount(*frameCount-1)
    a.mov(x86::rcx, x86::r12);
    a.mov(x86::rdx, x86::r13);
    a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::frameCount)); // rax = &frameCount_
    a.mov(x86::r8, x86::qword_ptr(x86::rax));                          // r8 = *frameCount
    a.dec(x86::r8);                                                    // r8 = *frameCount - 1
#else
    // Linux: rdi=ctx, rsi=fromAddr(r13), rdx=newFrameCount
    a.mov(x86::rdi, x86::r12);
    a.mov(x86::rsi, x86::r13);
    a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::frameCount)); // rax = &frameCount_
    a.mov(x86::rdx, x86::qword_ptr(x86::rax));                         // rdx = *frameCount
    a.dec(x86::rdx);
#endif
    a.sub(x86::rsp, 32);
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitReturnCloseUpvalues));
    a.call(x86::rax);
    a.add(x86::rsp, 32);
    a.mov(x86::r15, x86::qword_ptr(x86::r12, jit_offset::stackTop)); // 恢复栈顶
    a.mov(x86::rax, x86::rbx);                                       // 恢复返回值

    // *frameCount -= 1，rdx = 新的 frameCount（重新加载，close upvalue 破坏了 rcx/rdx）
    a.mov(x86::rcx, x86::qword_ptr(x86::r12, jit_offset::frameCount)); // rcx = &frameCount_
    a.mov(x86::rdx, x86::qword_ptr(x86::rcx));                         // rdx = *frameCount
    a.sub(x86::rdx, 1);
    a.mov(x86::qword_ptr(x86::rcx), x86::rdx);

    // 读取 frames[rdx] 的字段
    a.mov(x86::rcx, x86::qword_ptr(x86::r12, jit_offset::frames)); // rcx = frames base
    a.imul(x86::rdx, x86::rdx, 72);                                // R149: 72 字节偏移
    a.add(x86::rcx, x86::rdx);                                     // rcx = &frames[返回帧]

    // R149: 检查 isMethodCall，如果是方法调用则调用 jitMethodReturn 处理 writeBack
    //   jitMethodReturn(ctx, framePtr, retvalBits) → 返回最终的返回值 bits
    //   辅助函数内部处理：字段槽同步回 this + this 同步回 receiverSlotPtr + init 返回 this
    Label notMethodCall = a.new_label();
    a.mov(x86::rdx, x86::qword_ptr(x86::rcx, 48)); // rdx = isMethodCall
    a.test(x86::rdx, x86::rdx);
    a.jz(notMethodCall);

    // 方法调用路径：调用 jitMethodReturn(ctx, framePtr, retvalBits)
    // 保存返回值 rax 到 rbx（callee-saved，跨调用保留）
    a.mov(x86::rbx, x86::rax);
    // 同步栈顶到 ctx->stackTop（虽然 jitMethodReturn 不操作栈，保持模式一致）
    a.mov(x86::qword_ptr(x86::r12, jit_offset::stackTop), x86::r15);
#ifdef _WIN32
    // Win32: rcx=ctx, rdx=framePtr, r8=retvalBits
    // Bug #20 fix: 删除死指令 mov rdx,rcx（立即被后续序列覆盖），直接计算 framePtr
    a.mov(x86::rcx, x86::qword_ptr(x86::r12, jit_offset::frameCount)); // rcx = &frameCount_
    a.mov(x86::rdx, x86::qword_ptr(x86::rcx));                         // rdx = *frameCount（已被减 1）
    a.mov(x86::rcx, x86::qword_ptr(x86::r12, jit_offset::frames));     // rcx = frames base
    a.imul(x86::rdx, x86::rdx, 72);
    a.add(x86::rcx, x86::rdx); // rcx = framePtr
    a.mov(x86::rdx, x86::rcx); // rdx = framePtr
    a.mov(x86::rcx, x86::r12); // rcx = ctx
    a.mov(x86::r8, x86::rbx);  // r8 = retvalBits
#else
    // Linux: rdi=ctx, rsi=framePtr, rdx=retvalBits
    a.mov(x86::rdi, x86::r12);
    a.mov(x86::rsi, x86::rcx); // rsi = framePtr
    a.mov(x86::rdx, x86::rbx); // rdx = retvalBits
#endif
    a.sub(x86::rsp, 32); // shadow space: 3 个寄存器参数(rcx,rdx,r8) → Win64 ABI 要求 4×8=32 字节 shadow space
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitMethodReturn));
    a.call(x86::rax);
    a.add(x86::rsp, 32);
    // rax = 最终返回值 bits
    a.mov(x86::rbx, x86::rax); // 保存到 rbx
    // 检查错误（R149 fix: 用 movzx byte_ptr 读取 bool，原 mov qword_ptr 读取 8 字节
    //   会读到相邻 errorBuffer 指针的低 7 字节，导致误判 hasError=true）
    a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::hasError)); // hasError ptr
    a.movzx(x86::rax, x86::byte_ptr(x86::rax));                      // *hasError (1 字节零扩展)
    a.test(x86::rax, x86::rax);
    a.jnz(epilogue);
    // 恢复 r15（辅助函数可能未修改，但保持模式一致）
    a.mov(x86::r15, x86::qword_ptr(x86::r12, jit_offset::stackTop));
    // 最终返回值在 rbx
    a.mov(x86::rax, x86::rbx);

    a.bind(notMethodCall);

    // r14 = frames[idx].returnAddr（先读取，避免被后续覆盖）
    // rcx 仍指向 &frames[返回帧]（notMethodCall 路径）或已被覆盖（方法调用路径需重新计算）
    // 为简化，重新计算 framePtr
    a.mov(x86::rcx, x86::qword_ptr(x86::r12, jit_offset::frameCount)); // rcx = &frameCount_
    a.mov(x86::rdx, x86::qword_ptr(x86::rcx));                         // rdx = *frameCount（已减 1）
    a.mov(x86::rcx, x86::qword_ptr(x86::r12, jit_offset::frames));     // rcx = frames base
    a.imul(x86::rdx, x86::rdx, 72);
    a.add(x86::rcx, x86::rdx); // rcx = &frames[返回帧]
    // R161 perf: returnAddr 改用 rdx（rdx 在 imul+add 后已死，可安全复用），
    // 释放 r14 用于缓存 JIT_INT_TAG_BASE 常量
    a.mov(x86::rdx, x86::qword_ptr(x86::rcx, 16)); // rdx = returnAddr
    // 恢复 r13 = frames[idx].callerBp
    a.mov(x86::r13, x86::qword_ptr(x86::rcx));
    // 恢复 r15 = frames[idx].callerSp（丢弃当前帧的参数+局部变量）
    a.mov(x86::r15, x86::qword_ptr(x86::rcx, 8));

    // push 返回值到调用者栈
    a.sub(x86::r15, 8);
    a.mov(x86::qword_ptr(x86::r15), x86::rax);

    // R161 perf: 恢复缓存的 rbx(JIT_INT48_MASK)，调用者算术运算依赖此值
    a.mov(x86::rbx, x86::qword_ptr(x86::rbp, -48));

    // jmp returnAddr
    a.jmp(x86::rdx);
}

// ------------------------------------------------------------------
// OP_DIVIDE：INT 原生路径 + FLOAT SSE2 路径 + 除零错误 + C++ 辅助回退
// ------------------------------------------------------------------
void JITBackend::emitDivide(x86::Assembler& a, Label epilogue, size_t chunkIdx) {
    // R143 阶段 3b：类型分派——INT 走原生路径，FLOAT 走 C++ 辅助
    // R145 修复 Bug 2/3：INT 原生路径增加 INT64_MIN/-1 + INT48 溢出检测
    // R161 perf: 非特化模式下 INT 检查失败先走 FLOAT 原生 SSE2 路径（含除零检查）
    Label genericDiv = a.new_label();
    Label divEnd = a.new_label();
    Label divByZero = a.new_label();
    Label divSkipOvf = a.new_label();
    a.mov(x86::rax, x86::qword_ptr(x86::r15));    // rax = right(除数) raw bits
    a.mov(x86::rcx, x86::qword_ptr(x86::r15, 8)); // rcx = left(被除数) raw bits
    bool useFloatPathDiv = !specializeIntMode_ && !specializeFloatMode_;
    Label floatCheckDiv = useFloatPathDiv ? a.new_label() : Label();
    if (useFloatPathDiv) {
        emitCheckInt(a, x86::rax, floatCheckDiv);
        emitCheckInt(a, x86::rcx, floatCheckDiv);
    } else {
        emitCheckInt(a, x86::rax, genericDiv);
        emitCheckInt(a, x86::rcx, genericDiv);
    }
    // INT 原生路径：先解码运算（不 pop），检查溢出
    a.shl(x86::rax, 16);
    a.sar(x86::rax, 16);
    a.shl(x86::rcx, 16);
    a.sar(x86::rcx, 16);
    // 除零检查：必须检查 rax（除数）
    a.test(x86::rax, x86::rax);
    a.jz(divByZero);
    // Bug 3 修复：INT64_MIN / -1 溢出检查（idiv 会触发 #DE 异常）
    a.cmp(x86::rax, -1);
    a.jne(divSkipOvf);
    a.movabs(x86::rdx, 0x8000000000000000ULL); // INT64_MIN
    a.cmp(x86::rcx, x86::rdx);
    a.je(genericDiv); // INT64_MIN / -1 → C++ 辅助
    a.bind(divSkipOvf);
    // 执行除法：被除数 / 除数
    a.xchg(x86::rax, x86::rcx); // rax=被除数, rcx=除数
    a.cqo();
    a.idiv(x86::rcx);
    // Bug 2 修复：INT48 范围检查（商在 rax）
    a.mov(x86::rcx, x86::rax);
    a.shl(x86::rcx, 16);
    a.sar(x86::rcx, 16);
    a.cmp(x86::rcx, x86::rax);
    a.jne(genericDiv); // INT48 溢出 → C++ 辅助
    // 不溢出：pop 两个 + 重编码 + push
    a.add(x86::r15, 16);
    // R161 perf: 用缓存的 rbx(JIT_INT48_MASK)/r14(JIT_INT_TAG_BASE) 替代 movabs
    a.and_(x86::rax, x86::rbx);
    a.or_(x86::rax, x86::r14);
    a.sub(x86::r15, 8);
    a.mov(x86::qword_ptr(x86::r15), x86::rax);
    a.jmp(divEnd);
    // R161 perf: FLOAT 原生 SSE2 路径（含除零检查 → divByZero）
    if (useFloatPathDiv) {
        a.bind(floatCheckDiv);
        emitFloatBinaryArith(a, 3 /*div*/, genericDiv, divEnd, chunkIdx, divByZero);
    }
    // 除零错误路径（L14: 通过 jitRuntimeThrow 转为可捕获异常）
    a.bind(divByZero);
    a.mov(x86::qword_ptr(x86::r12, jit_offset::stackTop), x86::r15); // 同步栈顶到 ctx
    // Bug #52 note: "除零错误" 为字符串字面量，具有静态存储期，地址在整个
    // 程序生命期内有效，因此将其地址嵌入 JIT 机器码是安全的（不会悬垂）。
#ifdef _WIN32
    a.mov(x86::rcx, x86::r12);
    a.movabs(x86::rdx, reinterpret_cast<uint64_t>("除零错误"));
    a.mov(x86::r8, x86::r13);
    a.sub(x86::rsp, 32);
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitRuntimeThrow));
    a.call(x86::rax);
    a.add(x86::rsp, 32);
#else
    a.mov(x86::rdi, x86::r12);
    a.movabs(x86::rsi, reinterpret_cast<uint64_t>("除零错误"));
    a.mov(x86::rdx, x86::r13);
    a.sub(x86::rsp, 16);
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitRuntimeThrow));
    a.call(x86::rax);
    a.add(x86::rsp, 16);
#endif
    // rax = catchAddr 或 nullptr（未捕获时 hasError 已由 jitThrow 设置）
    a.test(x86::rax, x86::rax);
    a.jz(epilogue);
    // 捕获异常：恢复 r13/r15 并跳转到 catch 块
    a.mov(x86::r13, x86::qword_ptr(x86::r12, jit_offset::currentBp));
    a.mov(x86::r15, x86::qword_ptr(x86::r12, jit_offset::stackTop));
    a.jmp(x86::rax);
    // C++ 辅助路径：调用 jitDivGeneric(ctx, left, right)（含除零检查）
    a.bind(genericDiv);
    emitRecordTypeFeedback(a, x86::rax, chunkIdx);
    emitCallBinaryHelper(a, epilogue, reinterpret_cast<void*>(&jitDivGeneric), /*needCtx*/ true, /*checkError*/ true);
    a.bind(divEnd);
}

// ------------------------------------------------------------------
// OP_MODULO：INT 原生路径 + 除零错误 + C++ 辅助回退
// ------------------------------------------------------------------
void JITBackend::emitModulo(x86::Assembler& a, Label epilogue, size_t chunkIdx) {
    // R143 阶段 3b：类型分派——INT 走原生路径，FLOAT 走 C++ 辅助（std::fmod）
    // R145 修复 Bug 2/3：INT 原生路径增加 INT64_MIN/-1 + INT48 溢出检测
    Label genericMod = a.new_label();
    Label modEnd = a.new_label();
    Label modByZero = a.new_label();
    Label modSkipOvf = a.new_label();
    a.mov(x86::rax, x86::qword_ptr(x86::r15));    // rax = right(除数) raw bits
    a.mov(x86::rcx, x86::qword_ptr(x86::r15, 8)); // rcx = left(被除数) raw bits
    emitCheckInt(a, x86::rax, genericMod);
    emitCheckInt(a, x86::rcx, genericMod);
    // INT 原生路径：先解码运算（不 pop），检查溢出
    a.shl(x86::rax, 16);
    a.sar(x86::rax, 16);
    a.shl(x86::rcx, 16);
    a.sar(x86::rcx, 16);
    a.test(x86::rax, x86::rax);
    a.jz(modByZero);
    // Bug 3 修复：INT64_MIN % -1 溢出检查（idiv 会触发 #DE 异常，虽然余数为 0）
    a.cmp(x86::rax, -1);
    a.jne(modSkipOvf);
    a.movabs(x86::rdx, 0x8000000000000000ULL); // INT64_MIN
    a.cmp(x86::rcx, x86::rdx);
    a.je(genericMod); // INT64_MIN % -1 → C++ 辅助
    a.bind(modSkipOvf);
    a.xchg(x86::rax, x86::rcx); // rax=被除数, rcx=除数
    a.cqo();
    a.idiv(x86::rcx);
    a.mov(x86::rax, x86::rdx); // 余数在 rdx
    // Bug 2 修复：INT48 范围检查（余数在 rax）
    a.mov(x86::rcx, x86::rax);
    a.shl(x86::rcx, 16);
    a.sar(x86::rcx, 16);
    a.cmp(x86::rcx, x86::rax);
    a.jne(genericMod); // INT48 溢出 → C++ 辅助
    // 不溢出：pop 两个 + 重编码 + push
    a.add(x86::r15, 16);
    // R161 perf: 用缓存的 rbx(JIT_INT48_MASK)/r14(JIT_INT_TAG_BASE) 替代 movabs
    a.and_(x86::rax, x86::rbx);
    a.or_(x86::rax, x86::r14);
    a.sub(x86::r15, 8);
    a.mov(x86::qword_ptr(x86::r15), x86::rax);
    a.jmp(modEnd);
    // 除零错误路径（L14: 通过 jitRuntimeThrow 转为可捕获异常）
    a.bind(modByZero);
    a.mov(x86::qword_ptr(x86::r12, jit_offset::stackTop), x86::r15); // 同步栈顶到 ctx
    // Bug #52 note: "除零错误" 为字符串字面量，具有静态存储期，地址在整个
    // 程序生命期内有效，因此将其地址嵌入 JIT 机器码是安全的（不会悬垂）。
#ifdef _WIN32
    a.mov(x86::rcx, x86::r12);
    a.movabs(x86::rdx, reinterpret_cast<uint64_t>("除零错误"));
    a.mov(x86::r8, x86::r13);
    a.sub(x86::rsp, 32);
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitRuntimeThrow));
    a.call(x86::rax);
    a.add(x86::rsp, 32);
#else
    a.mov(x86::rdi, x86::r12);
    a.movabs(x86::rsi, reinterpret_cast<uint64_t>("除零错误"));
    a.mov(x86::rdx, x86::r13);
    a.sub(x86::rsp, 16);
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitRuntimeThrow));
    a.call(x86::rax);
    a.add(x86::rsp, 16);
#endif
    // rax = catchAddr 或 nullptr（未捕获时 hasError 已由 jitThrow 设置）
    a.test(x86::rax, x86::rax);
    a.jz(epilogue);
    // 捕获异常：恢复 r13/r15 并跳转到 catch 块
    a.mov(x86::r13, x86::qword_ptr(x86::r12, jit_offset::currentBp));
    a.mov(x86::r15, x86::qword_ptr(x86::r12, jit_offset::stackTop));
    a.jmp(x86::rax);
    // C++ 辅助路径：调用 jitModGeneric(ctx, left, right)（含除零检查）
    a.bind(genericMod);
    emitRecordTypeFeedback(a, x86::rax, chunkIdx);
    emitCallBinaryHelper(a, epilogue, reinterpret_cast<void*>(&jitModGeneric), /*needCtx*/ true, /*checkError*/ true);
    a.bind(modEnd);
}

// ------------------------------------------------------------------
// OP_LOOP：循环回边（含 safepoint GC 轮询 + OSR 回边计数 + OSR 入口点生成）
// ------------------------------------------------------------------
void JITBackend::emitLoop(x86::Assembler& a, Label epilogue, Label jumpTarget, size_t chunkIdx) {
    // R157: OSR 回边计数器注入（仅当 osrLoopThresholds_[chunkIdx] > 0 时）
    // 每次循环回边递增计数器，超阈值时触发特化重编译。
    // 编译期过滤：阈值为 0 时完全跳过代码生成，运行时零开销（R151 教训）。

    // Safepoint GC 轮询：检查 gcNeededFlag，非零时调用 jitSafepointGc
    {
        Label skipGc = a.new_label();
        // 读取 ctx->gcNeededFlag (offset 248)
        a.mov(x86::rcx, x86::qword_ptr(x86::r12, 248)); // rcx = gcNeededFlag ptr
        a.test(x86::rcx, x86::rcx);
        a.jz(skipGc);                              // 指针为 null 则跳过
        a.mov(x86::eax, x86::dword_ptr(x86::rcx)); // eax = *gcNeededFlag
        a.test(x86::eax, x86::eax);
        a.jz(skipGc); // 标志为 0 则跳过
        // 同步栈顶到 ctx->stackTop
        a.mov(x86::qword_ptr(x86::r12, jit_offset::stackTop), x86::r15);
        // 调用 jitSafepointGc(ctx)
        a.mov(x86::rcx, x86::r12); // arg1 = ctx
#ifdef _WIN32
        a.sub(x86::rsp, 32);
#else
        a.mov(x86::rdi, x86::r12);
        a.sub(x86::rsp, 16);
#endif
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitSafepointGc));
        a.call(x86::rax);
#ifdef _WIN32
        a.add(x86::rsp, 32);
#else
        a.add(x86::rsp, 16);
#endif
        // 恢复栈顶
        a.mov(x86::r15, x86::qword_ptr(x86::r12, jit_offset::stackTop));
        a.bind(skipGc);
    }

    if (chunkIdx < osrLoopThresholds_.size() && osrLoopThresholds_[chunkIdx] > 0) {
        Label skipOsr = a.new_label();

        // 1. 递增 osrLoopCounts_[chunkIdx]
        a.mov(x86::rcx, x86::qword_ptr(x86::r12, 168)); // rcx = osrLoopCountsPtr
        a.inc(x86::qword_ptr(x86::rcx, static_cast<int32_t>(chunkIdx * 8)));

        // 2. 检查 osrRecompiledFlags_[chunkIdx] == 0（避免重复触发）
        a.mov(x86::rcx, x86::qword_ptr(x86::r12, 184)); // rcx = osrRecompiledFlagsPtr
        a.mov(x86::rax, x86::qword_ptr(x86::rcx, static_cast<int32_t>(chunkIdx * 8)));
        a.test(x86::rax, x86::rax);
        a.jnz(skipOsr); // 已触发过，跳过

        // 3. 比较 osrLoopCounts_[chunkIdx] 与 osrLoopThresholds_[chunkIdx]
        a.mov(x86::rcx, x86::qword_ptr(x86::r12, 168)); // rcx = osrLoopCountsPtr
        a.mov(x86::rax, x86::qword_ptr(x86::rcx, static_cast<int32_t>(chunkIdx * 8)));
        a.mov(x86::rcx, x86::qword_ptr(x86::r12, 176)); // rcx = osrLoopThresholdsPtr
        a.cmp(x86::rax, x86::qword_ptr(x86::rcx, static_cast<int32_t>(chunkIdx * 8)));
        a.jb(skipOsr); // 未达阈值，跳过

        // 4. 达到阈值：标记为已触发
        a.mov(x86::rcx, x86::qword_ptr(x86::r12, 184)); // rcx = osrRecompiledFlagsPtr
        a.mov(x86::qword_ptr(x86::rcx, static_cast<int32_t>(chunkIdx * 8)), 1);

        // 5. 根据 osrMigrationMode_ 选择 OSR 策略
        //    栈对齐：chunk 内部 rsp%16==0，sub 32 后保持 rsp%16==0（与 P0 栈对齐修复一致）
        if (osrMigrationMode_) {
            // R158: 真正 OSR 栈帧迁移
            // a. 保存 r13/r15 到邮箱（OSR 入口点将从此恢复栈帧）
            a.mov(x86::qword_ptr(x86::r12, jit_offset::osrSavedBp), x86::r13); // osrSavedBp = r13
            a.mov(x86::qword_ptr(x86::r12, jit_offset::osrSavedSp), x86::r15); // osrSavedSp = r15
            // b. 同步栈顶
            a.mov(x86::qword_ptr(x86::r12, jit_offset::stackTop), x86::r15);
            // c. 调用 jitTriggerOsrMigration(ctx, chunkIdx)
            //    触发特化重编译（含 OSR 入口点生成），通过 ctx->osrEntryPoint 返回
            a.mov(x86::rcx, x86::r12);                       // arg1 = ctx
            a.mov(x86::rdx, static_cast<int32_t>(chunkIdx)); // arg2 = chunkIdx
#ifdef _WIN32
            a.sub(x86::rsp, 32); // shadow space (32) + 8B 对齐填充
#else
            a.sub(x86::rsp, 16); // 16-byte alignment
#endif
            a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitTriggerOsrMigration));
            a.call(x86::rax);
#ifdef _WIN32
            a.add(x86::rsp, 32);
#else
            a.add(x86::rsp, 16);
#endif
            // d. 检查返回值（0=成功）
            a.test(x86::rax, x86::rax);
            a.jnz(skipOsr); // 失败，跳过迁移走正常回边
            // e. 加载 osrEntryPoint（offset 208）
            a.mov(x86::rax, x86::qword_ptr(x86::r12, 208));
            a.test(x86::rax, x86::rax);
            a.jz(skipOsr); // osrEntryPoint 为 null，跳过迁移
            // f. jmp osrEntryPoint（真正 OSR 栈帧迁移，跳到特化版本的 OSR 入口点）
            a.jmp(x86::rax);
        } else {
            // R157: 简化版 OSR（仅触发特化重编译，不迁移栈帧，下次 execute 生效）
            a.mov(x86::qword_ptr(x86::r12, jit_offset::stackTop), x86::r15); // 同步栈顶
            a.mov(x86::rcx, x86::r12);                                       // arg1 = ctx
            a.mov(x86::rdx, static_cast<int32_t>(chunkIdx));                 // arg2 = chunkIdx
#ifdef _WIN32
            a.sub(x86::rsp, 32); // shadow space (32) + 8B 对齐填充
#else
            a.sub(x86::rsp, 16); // 16-byte alignment
#endif
            a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitTriggerOsrRecompile));
            a.call(x86::rax);
#ifdef _WIN32
            a.add(x86::rsp, 32);
#else
            a.add(x86::rsp, 16);
#endif
            a.mov(x86::r15, x86::qword_ptr(x86::r12, jit_offset::stackTop)); // 恢复栈顶
        }

        a.bind(skipOsr);
    }

    a.jmp(jumpTarget);

    // R158: OSR 入口点生成（仅 osrEntryGenMode_ 且目标 chunk 匹配时）
    // 在正常 jmp loop 回边之后生成 OSR 入口 Label，仅可通过外部 jmp 到达。
    // OSR 入口点恢复 r13/r15 从邮箱，然后跳转到循环回边目标（特化版本中的循环起点）。
    if (osrEntryGenMode_ && chunkIdx == osrTargetChunkIdx_ && !osrEntryLabelGenerated_) {
        osrEntryLabel_ = a.new_label();
        a.bind(osrEntryLabel_);
        // 恢复 r13/r15 从邮箱（OSR 触发时由 JIT 代码保存）
        a.mov(x86::r13, x86::qword_ptr(x86::r12, jit_offset::osrSavedBp)); // r13 = osrSavedBp
        a.mov(x86::r15, x86::qword_ptr(x86::r12, jit_offset::osrSavedSp)); // r15 = osrSavedSp
        // Bug #22 fix: OSR 入口不经过 prologue，rbx/r14 可能已被 generic 版本修改，
        // 必须重新加载缓存常量（算术运算热路径依赖 rbx=JIT_INT48_MASK, r14=JIT_INT_TAG_BASE）
        a.movabs(x86::rbx, JIT_INT48_MASK);   // 恢复 and 操作掩码
        a.movabs(x86::r14, JIT_INT_TAG_BASE); // 恢复 or 操作 tag 基址
        // 跳转到循环回边目标（特化版本中的循环起点）
        a.jmp(jumpTarget);
        osrEntryLabelGenerated_ = true;
    }
    (void)epilogue; // OP_LOOP 不直接跳 epilogue，保留参数以与其他 helper 一致
}

// ------------------------------------------------------------------
// OP_CALL_EXPR：闭包值调用（邮箱模式）
// ------------------------------------------------------------------
void JITBackend::emitCallExpr(x86::Assembler& a, Label epilogue, uint8_t argCount) {
    Label returnLabel = a.new_label();

    // 1. 存储 callerBp (r13) 到 ctx->callerBp (offset 96)
    a.mov(x86::qword_ptr(x86::r12, jit_offset::callerBp), x86::r13);

    // 2. 同步栈顶到 ctx->stackTop (offset 48)
    a.mov(x86::qword_ptr(x86::r12, jit_offset::stackTop), x86::r15);

    // 3. 调用 jitCallExpr(ctx, argCount)
    //    辅助函数通过 ctx->stackTop 操作栈（pop args + pop closure + push args/defaults/extraSlots）
    //    通过邮箱返回：ctx->methodEntryPtr = 入口地址, ctx->methodLocalCount = localCount
#ifdef _WIN32
    a.mov(x86::rcx, x86::r12); // arg1 = ctx
    a.mov(x86::dl, argCount);  // arg2 = argCount (uint8_t)
    a.sub(x86::rsp, 32);       // shadow space (32) + 8B 对齐填充（R151 教训）
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitCallExpr));
    a.call(x86::rax);
    a.add(x86::rsp, 32);
#else
    a.mov(x86::rdi, x86::r12);
    a.mov(x86::sil, argCount);
    a.sub(x86::rsp, 16); // 16-byte alignment
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitCallExpr));
    a.call(x86::rax);
    a.add(x86::rsp, 16);
#endif

    // 4. 恢复 r15（jitCallExpr 修改了栈）
    a.mov(x86::r15, x86::qword_ptr(x86::r12, jit_offset::stackTop));

    // 5. 检查错误
    a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::hasError));
    a.movzx(x86::rax, x86::byte_ptr(x86::rax));
    a.test(x86::rax, x86::rax);
    a.jnz(epilogue);

    // 6. 在新 frame 上设置 returnAddr（jitCallExpr 无法访问 JIT Label，需在调用后写入）
    //    新 frame 在 frames[*frameCount - 1]
    a.mov(x86::rcx, x86::qword_ptr(x86::r12, jit_offset::frameCount)); // rcx = &frameCount_
    a.mov(x86::rdx, x86::qword_ptr(x86::rcx));                         // rdx = *frameCount
    a.sub(x86::rdx, 1);                                                // rdx = idx（新 frame 索引）
    a.mov(x86::rcx, x86::qword_ptr(x86::r12, jit_offset::frames));     // rcx = frames base
    a.imul(x86::rdx, x86::rdx, 72);
    a.add(x86::rcx, x86::rdx); // rcx = &frames[idx]
    a.lea(x86::rax, x86::qword_ptr(returnLabel));
    a.mov(x86::qword_ptr(x86::rcx, 16), x86::rax); // frame->returnAddr = returnLabel

    // 7. 设置 r13 = r15 + (methodLocalCount - 1) * 8
    //    普通函数帧无 this/fields，slot 0 是 arg0（最高地址）
    a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::methodLocalCount)); // rax = methodLocalCount
    a.dec(x86::rax);
    a.imul(x86::rax, x86::rax, 8);
    a.lea(x86::r13, x86::qword_ptr(x86::r15, x86::rax));

    // 8. jmp methodEntryPtr
    a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::methodEntryPtr)); // rax = methodEntryPtr
    a.jmp(x86::rax);

    // 9. 绑定返回 Label（函数 OP_RETURN 后跳回此处）
    a.bind(returnLabel);
}

// ------------------------------------------------------------------
// OP_CLOSURE：调用 jitCreateClosure 创建闭包值并压栈
// ------------------------------------------------------------------
void JITBackend::emitClosure(x86::Assembler& a, Label epilogue, const char* funNamePtr, const void* chunkPtr,
                             uint8_t upvalueCount, const uint8_t* upvalueDescs) {
    // 同步栈顶 + 当前帧 basePointer 到 ctx
    a.mov(x86::qword_ptr(x86::r12, jit_offset::stackTop), x86::r15);  // stackTop
    a.mov(x86::qword_ptr(x86::r12, jit_offset::currentBp), x86::r13); // currentBp（R156）

    // 调用 jitCreateClosure(ctx, funName, chunkPtr, upvalueCount, upvalueDescs)
    // → rax = 闭包值 bits
    a.movabs(x86::r10, reinterpret_cast<uint64_t>(funNamePtr));
    a.movabs(x86::r11, reinterpret_cast<uint64_t>(chunkPtr));
#ifdef _WIN32
    // Win32: rcx=ctx, rdx=funName, r8=chunkPtr, r9=upvalueCount, [rsp+32]=upvalueDescs
    a.mov(x86::rcx, x86::r12); // arg1 = ctx
    a.mov(x86::rdx, x86::r10); // arg2 = funName
    a.mov(x86::r8, x86::r11);  // arg3 = chunkPtr
    a.xor_(x86::r9, x86::r9);
    a.mov(x86::r9b, upvalueCount); // arg4 = upvalueCount
    a.sub(x86::rsp, 48);           // shadow space (32) + 8B arg5 + 8B 对齐（R151 教训）
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(upvalueDescs));
    a.mov(x86::qword_ptr(x86::rsp, 32), x86::rax); // arg5 = upvalueDescs
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitCreateClosure));
    a.call(x86::rax);
    a.add(x86::rsp, 48);
#else
    // Linux: rdi=ctx, rsi=funName, rdx=chunkPtr, rcx=upvalueCount, r8=upvalueDescs
    a.mov(x86::rdi, x86::r12);
    a.mov(x86::rsi, x86::r10);
    a.mov(x86::rdx, x86::r11);
    a.xor_(x86::rcx, x86::rcx);
    a.mov(x86::cl, upvalueCount);
    a.movabs(x86::r8, reinterpret_cast<uint64_t>(upvalueDescs));
    a.sub(x86::rsp, 16); // 16-byte alignment
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitCreateClosure));
    a.call(x86::rax);
    a.add(x86::rsp, 16);
#endif
    // push 闭包值 bits（rax）
    a.sub(x86::r15, 8);
    a.mov(x86::qword_ptr(x86::r15), x86::rax);
    // 检查错误
    a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::hasError));
    a.movzx(x86::rax, x86::byte_ptr(x86::rax));
    a.test(x86::rax, x86::rax);
    a.jnz(epilogue);
}

// ------------------------------------------------------------------
// OP_CALL mailbox 路径（lazy mode / 有默认参数 / 有 upvalues / 有内部闭包）
// ------------------------------------------------------------------
void JITBackend::emitCallMailbox(x86::Assembler& a, Label epilogue, const char* funNamePtr, uint8_t argCount) {
    Label returnLabel = a.new_label();

    // 1. 存储 callerBp (r13) 到 ctx->callerBp (offset 96)
    a.mov(x86::qword_ptr(x86::r12, jit_offset::callerBp), x86::r13);

    // 2. 同步栈顶到 ctx->stackTop (offset 48)
    a.mov(x86::qword_ptr(x86::r12, jit_offset::stackTop), x86::r15);

    // 3. 调用 jitCallByName(ctx, funName, argCount)
    //    辅助函数通过 ctx->stackTop 操作栈（pop args + push args/defaults/extraSlots）
    //    通过邮箱返回：ctx->methodEntryPtr = 入口地址, ctx->methodLocalCount = localCount
    //    lazyMode_ 下 entryPtr 为 null 时触发 compileSingleChunkLazy
#ifdef _WIN32
    a.mov(x86::rcx, x86::r12);                                  // arg1 = ctx
    a.movabs(x86::rdx, reinterpret_cast<uint64_t>(funNamePtr)); // arg2 = funName
    a.mov(x86::r8, static_cast<int32_t>(argCount));             // arg3 = argCount
    a.sub(x86::rsp, 32);                                        // shadow space (32) + 8B 对齐填充
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitCallByName));
    a.call(x86::rax);
    a.add(x86::rsp, 32);
#else
    a.mov(x86::rdi, x86::r12);
    a.movabs(x86::rsi, reinterpret_cast<uint64_t>(funNamePtr));
    a.mov(x86::edx, static_cast<int32_t>(argCount));
    a.sub(x86::rsp, 16); // 16-byte alignment
    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitCallByName));
    a.call(x86::rax);
    a.add(x86::rsp, 16);
#endif

    // 4. 恢复 r15（jitCallByName 修改了栈）
    a.mov(x86::r15, x86::qword_ptr(x86::r12, jit_offset::stackTop));

    // 5. 检查错误
    a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::hasError));
    a.movzx(x86::rax, x86::byte_ptr(x86::rax));
    a.test(x86::rax, x86::rax);
    a.jnz(epilogue);

    // 6. 在新 frame 上设置 returnAddr（jitCallByName 无法访问 JIT Label，需在调用后写入）
    //    新 frame 在 frames[*frameCount - 1]
    a.mov(x86::rcx, x86::qword_ptr(x86::r12, jit_offset::frameCount)); // rcx = &frameCount_
    a.mov(x86::rdx, x86::qword_ptr(x86::rcx));                         // rdx = *frameCount
    a.sub(x86::rdx, 1);                                                // rdx = idx（新 frame 索引）
    a.mov(x86::rcx, x86::qword_ptr(x86::r12, jit_offset::frames));     // rcx = frames base
    a.imul(x86::rdx, x86::rdx, 72);
    a.add(x86::rcx, x86::rdx); // rcx = &frames[idx]
    a.lea(x86::rax, x86::qword_ptr(returnLabel));
    a.mov(x86::qword_ptr(x86::rcx, 16), x86::rax); // frame->returnAddr = returnLabel

    // 7. 设置 r13 = r15 + (methodLocalCount - 1) * 8
    //    普通函数帧无 this/fields，slot 0 是 arg0（最高地址）
    a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::methodLocalCount)); // rax = methodLocalCount
    a.dec(x86::rax);
    a.imul(x86::rax, x86::rax, 8);
    a.lea(x86::r13, x86::qword_ptr(x86::r15, x86::rax));

    // 8. jmp methodEntryPtr
    a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::methodEntryPtr)); // rax = methodEntryPtr
    a.jmp(x86::rax);

    // 9. 绑定返回 Label（函数 OP_RETURN 后跳回此处）
    a.bind(returnLabel);
}

// ------------------------------------------------------------------
// OP_CALL fast path（非 lazy / 无默认参数 / 无 upvalues / 无内部闭包）
// 返回 true 走快速路径（ip += 4 由调用方处理），false 交由调用方走 mailbox
// ------------------------------------------------------------------
bool JITBackend::emitCallDispatch(x86::Assembler& a, Label epilogue, const JitFuncInfo& info, uint8_t argCount) {
    // R161 perf: 混合模式优化 — 非 lazy 模式 + 无默认参数 + 无 upvalues + 无内部闭包时
    // 走编译期 jmp entryLabel 快速路径（恢复 R141 风格），消除 jitCallByName 的
    // C++ 调用 + unordered_map 查找 + std::string 构造 + arg pop/push 开销。
    // fib(30) 约 1.6M 次 OP_CALL，邮箱模式导致 0.60x 加速比，快速路径恢复到 >1x。
    // 条件说明：
    //   !lazyMode_           — eager 模式所有 chunk 已编译，entryLabel 已绑定
    //   defaultConstIndices.empty() — 无默认参数，argCount == arity（编译器保证）
    //   upvalues.empty()     — 函数不捕获 upvalue，无 OP_GET_UPVALUE/OP_SET_UPVALUE
    //   !hasInnerClosures    — 函数体无 OP_CLOSURE，不访问 frameUpvaluesStack_
    // 不满足条件时返回 false，调用方走 emitCallMailbox
    if (!lazyMode_ && info.chunk && info.chunk->defaultConstIndices.empty() && info.chunk->upvalues.empty() &&
        !info.hasInnerClosures) {

        Label returnLabel = a.new_label();
        Label overflowLabel = a.new_label();
        int localCount = info.localCount;
        int arityVal = info.arity;
        int extraSlots = localCount - arityVal;

        // 1. 存储 callerBp (r13) 到 ctx->callerBp (offset 96)
        a.mov(x86::qword_ptr(x86::r12, jit_offset::callerBp), x86::r13);

        // 2. MAX_FRAMES 检查（与 jitCallByName 对齐）
        a.mov(x86::rcx, x86::qword_ptr(x86::r12, jit_offset::frameCount)); // rcx = &frameCount
        a.mov(x86::rdx, x86::qword_ptr(x86::rcx));                         // rdx = *frameCount
        a.mov(x86::rax, static_cast<int32_t>(RuntimeLimits::MAX_FRAMES));
        a.cmp(x86::rdx, x86::rax);
        a.jae(overflowLabel);

        // 3. 预分配 extraSlots（填充 JIT_NULL_BITS）
        //    参数已在栈上（r15 指向 argN-1），只需在参数下方 push extraSlots
        if (extraSlots > 0) {
            a.sub(x86::r15, static_cast<int32_t>(extraSlots * 8));
            a.movabs(x86::rax, static_cast<int64_t>(JIT_NULL_BITS));
            for (int i = 0; i < extraSlots; ++i) {
                a.mov(x86::qword_ptr(x86::r15, static_cast<int32_t>(i * 8)), x86::rax);
            }
        }

        // 4. 构造 JitFrame at frames[*frameCount]
        //    rdx 仍持有 *frameCount（idx），重新加载（被 cmp 覆盖过）
        a.mov(x86::rcx, x86::qword_ptr(x86::r12, jit_offset::frameCount)); // rcx = &frameCount
        a.mov(x86::rdx, x86::qword_ptr(x86::rcx));                         // rdx = idx
        a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::frames));     // rax = frames base
        a.imul(x86::rdx, x86::rdx, 72);
        a.add(x86::rax, x86::rdx); // rax = &frames[idx]

        // frame->callerBp (offset 0) = r13
        a.mov(x86::qword_ptr(x86::rax, 0), x86::r13);

        // frame->callerSp (offset 8) = callerSpAfterPop = r15 + (extraSlots + argCount) * 8
        //    当前 r15 指向 first extraSlot（或 argN-1 若 extraSlots==0）
        //    callerSpAfterPop 是参数上方地址（OP_RETURN 恢复调用者 r15 用）
        int callerSpOff = (extraSlots + argCount) * 8;
        a.lea(x86::rcx, x86::qword_ptr(x86::r15, static_cast<int32_t>(callerSpOff)));
        a.mov(x86::qword_ptr(x86::rax, 8), x86::rcx);

        // frame->returnAddr (offset 16) = returnLabel
        a.lea(x86::rcx, x86::qword_ptr(returnLabel));
        a.mov(x86::qword_ptr(x86::rax, 16), x86::rcx);

        // frame->receiverSlotPtr/methodFieldOrder/methodBp = nullptr (offset 24/32/40)
        a.mov(x86::qword_ptr(x86::rax, 24), 0);
        a.mov(x86::qword_ptr(x86::rax, 32), 0);
        a.mov(x86::qword_ptr(x86::rax, 40), 0);
        // frame->isMethodCall/isInitCall/fieldCount = 0 (offset 48/56/64)
        a.mov(x86::qword_ptr(x86::rax, 48), 0);
        a.mov(x86::qword_ptr(x86::rax, 56), 0);
        a.mov(x86::qword_ptr(x86::rax, 64), 0);

        // 5. *frameCount = idx + 1
        a.mov(x86::rcx, x86::qword_ptr(x86::r12, jit_offset::frameCount)); // rcx = &frameCount
        a.mov(x86::rdx, x86::qword_ptr(x86::rcx));                         // rdx = idx
        a.inc(x86::rdx);
        a.mov(x86::qword_ptr(x86::rcx), x86::rdx);

        // 6. 设置 r13 = r15 + (localCount - 1) * 8
        //    普通函数帧无 this/fields，slot 0 是 arg0（最高地址）
        a.mov(x86::rax, static_cast<int32_t>((localCount - 1) * 8));
        a.lea(x86::r13, x86::qword_ptr(x86::r15, x86::rax));

        // 7. jmp entryLabel（直接跳转，0 次 C++ 调用！）
        a.jmp(info.entryLabel);

        // 8. 栈溢出错误路径（必须在 returnLabel 之前，避免 fall-through 误触发）
        //    罕见路径，C++ 调用开销可忽略；错误消息与 jitCallByName 对齐
        a.bind(overflowLabel);
        a.mov(x86::rcx, x86::r12); // arg1 = ctx
        a.sub(x86::rsp, 32);       // shadow space + 对齐
        a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitReportStackOverflow));
        a.call(x86::rax);
        a.add(x86::rsp, 32);
        a.jmp(epilogue);

        // 9. 绑定返回 Label（函数 OP_RETURN 后跳回此处）
        a.bind(returnLabel);
        return true;
    }
    (void)epilogue;
    (void)argCount;
    return false;
}

#endif // MINILANG_USE_JIT
