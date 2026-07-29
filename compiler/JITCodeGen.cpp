/**
 * @file compiler/JITCodeGen.cpp
 * @brief JIT code generation core: compileAllChunks (extracted from JIT.cpp).
 * @see JIT.h JITInternal.h
 * @since P3 (JIT.cpp split)
 */

#include "compiler/JIT.h"

#ifdef MINILANG_USE_JIT

#include "common/Diagnostic.h"
#include "common/ErrorFormat.h"
#include "common/ErrorMessages.h"
#include "common/RuntimeLimits.h"
#include "common/Utf8Utils.h"
#include "compiler/JITInternal.h"
#include "interpreter/GcManager.h"
#include "interpreter/Value.h"
#include <algorithm>
#include <array>
#include <asmjit/asmjit.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

using namespace jit_internal;

JitEntryFn JITBackend::compileAllChunks(const CompileResult& result) {
    using namespace asmjit;

    CodeHolder code;
    Error err = code.init(runtime_.environment());
    if (err != kErrorOk) {
        compileError(std::string("asmjit CodeHolder::init 失败: ") + DebugUtils::error_as_string(err));
        return nullptr;
    }

    // 拓展二期·教学（字节码↔汇编对照）：按需挂 StringLogger 捕获发射的
    // 汇编文本。logger 生命周期覆盖整个编译过程（栈上对象，函数末尾
    // runtime_.add 前读出内容存入 capturedAsm_）。仅教学路径启用，
    // 常规执行零开销。
    StringLogger asmLogger;
    if (asmCaptureEnabled_) {
        asmLogger.add_flags(FormatFlags::kMachineCode); // 附带机器码字节，对照更直观
        code.set_logger(&asmLogger);
        capturedAsm_.clear();
    }

    x86::Assembler a(&code);

    // R160: 统计所有 chunk 的 OP_MEMBER_GET 总数，预分配 inline cache 数组
    // callSiteId 跨所有 chunk 统一编号，运行时通过 id 索引 memberGetIC_
    nextCallSiteId_ = 0;
    countMemberGetInChunk(result.mainChunk);
    for (const auto& [name, fc] : result.functionChunks) {
        countMemberGetInChunk(fc);
    }
    memberGetIC_.assign(nextCallSiteId_ > 0 ? nextCallSiteId_ : 1, MemberGetInlineCacheEntry{});
    nextCallSiteId_ = 0; // 重置，编译时按顺序分配

    // ---- 函数 prologue ----
    // Windows x64 ABI: 进入时 rsp = 16k+8（call 压入返回地址）
    // push rbp → rsp = 16k（16 字节对齐）
    a.push(x86::rbp);
    a.mov(x86::rbp, x86::rsp);
    // R161 fixup: 栈空间扩大到 48B（callee-saved: r12/r13/r14/r15/rbx + 8B 对齐填充）
    // + 8KB（操作数栈 1024 个 int64），共 8240 字节，保持 16 字节对齐。
    // 原 32B 仅保存 r12-r15，遗漏 rbx（OP_RETURN 路径使用 rbx 7 处但未保存，ABI 违规）。
    a.sub(x86::rsp, kJitCalleeSavedArea + kJitOperandStackBytes);

    // 保存 callee-saved 寄存器到 [rbp-8..rbp-40]
    a.mov(x86::qword_ptr(x86::rbp, -8), x86::r12);
    a.mov(x86::qword_ptr(x86::rbp, -16), x86::r13);
    a.mov(x86::qword_ptr(x86::rbp, -24), x86::r14);
    a.mov(x86::qword_ptr(x86::rbp, -32), x86::r15);
    a.mov(x86::qword_ptr(x86::rbp, -40), x86::rbx);

    // 初始化 r12 = JitContext* ctx
#ifdef _WIN32
    a.mov(x86::r12, x86::rcx);
#else
    a.mov(x86::r12, x86::rdi);
#endif

    // 初始化 r15 = operand stack top（向低地址增长）
    // 空栈时 r15 指向栈基址 [rbp-48]（48 = 5 个 callee-saved × 8 + 8B 对齐填充），
    // push: sub r15, 8; mov [r15], rax; pop: mov rax, [r15]; add r15, 8
    a.lea(x86::r15, x86::qword_ptr(x86::rbp, -kJitCalleeSavedArea));

    // R141: 初始化 r13 = 0（mainChunk 无参数，basePointer 无意义）
    a.xor_(x86::r13, x86::r13);

    // R161 perf: 用 callee-saved 寄存器缓存算术运算热路径常量，消除 6 处 OP_ADD/SUB/MUL/DIV/MOD/NEGATE
    // 各 2 条 movabs（共 12 条 movabs = 120 字节代码）。rbx/r14 仅在 OP_RETURN 瞬态使用，
    // OP_RETURN 入口保存 rbx 到 [rbp-48] 暂存槽、出口恢复；r14 returnAddr 改用 rdx（已死值）。
    // rbx = JIT_INT48_MASK（and 操作掩码），r14 = JIT_INT_TAG_BASE（or 操作 tag 基址）
    a.movabs(x86::rbx, JIT_INT48_MASK);
    a.movabs(x86::r14, JIT_INT_TAG_BASE);

    // 用于 OP_RETURN 末帧返回的 epilogue 标签
    Label epilogue = a.new_label();

    // ---- C++ 辅助函数调用代码生成 lambda 已提取为成员函数 ----
    // 详见 JITCodeGenHelpers.cpp：emitCallBinaryHelper / emitCallUnaryHelper /
    // emitCallOrderedCompare / emitBuildArray / emitIndexGet / emitIndexSetLocal /
    // emitBuildDict / emitBuildTuple / emitIndexSetGlobal / emitClassNew /
    // emitInitField / emitDefineClass / emitMemberGet / emitMemberSetVar /
    // emitMemberSetLocal / emitMethodCall / emitCheckInt / emitRecordTypeFeedback /
    // emitFloatBinaryArith / emitLen / emitDupN / emitLoadMutated / emitIndexSet /
    // emitWritebackVar / emitWritebackLocal

    // ---- 建立函数表：functionName → (entryLabel, localCount, arity) ----
    // R141: 编译期需要知道每个函数的 localCount（用于预分配局部变量栈槽）
    // 和入口 Label（用于 OP_CALL 的 jmp 指令）
    // P0 重构：FuncInfo 已提取为 JITBackend::JitFuncInfo（JIT.h），供 emitCallDispatch helper 使用
    std::unordered_map<std::string, JitFuncInfo> funcTable;
    // R149: 同时收集方法 chunk（name 含 '.'）的元信息，供 runtime_.add 后填充 methodEntries_
    struct MethodLabelInfo {
        std::string fullName; // "Class.method"
        Label entryLabel;
        int localCount = 0;
        int arity = 0;
        int requiredArity = 0;
        const BytecodeChunk* chunk = nullptr;
    };
    std::vector<MethodLabelInfo> methodLabels;
    for (const auto& [name, chunk] : result.functionChunks) {
        JitFuncInfo info;
        info.entryLabel = a.new_label();
        info.localCount = chunk.localCount;
        info.arity = chunk.arity;
        info.requiredArity = chunk.requiredArity;
        info.chunk = &chunk; // R155: OP_CLOSURE 构建闭包值时获取 chunkPtr 用
        // R161 perf: 扫描函数体是否含 OP_CLOSURE（用于 OP_CALL fast path 门控）
        // 若函数体无 OP_CLOSURE，则 frameUpvaluesStack_ 在此帧不会被访问，
        // 可跳过 jitCallByName 的 frameUpvaluesStack_ 初始化
        {
            const auto& chunkCode = chunk.code;
            for (size_t i = 0; i < chunkCode.size();) {
                OpCode op = static_cast<OpCode>(chunkCode[i]);
                if (op == OpCode::OP_CLOSURE) {
                    info.hasInnerClosures = true;
                    break;
                }
                i += chunk.instructionSizeAt(i);
            }
        }
        funcTable.emplace(name, std::move(info));
        // R149: 方法 chunk（name 含 '.'）单独收集，用于 methodEntries_ 填充
        if (name.find('.') != std::string::npos) {
            MethodLabelInfo mli;
            mli.fullName = name;
            mli.entryLabel = funcTable[name].entryLabel; // 复制 Label 引用
            mli.localCount = chunk.localCount;
            mli.arity = chunk.arity;
            mli.requiredArity = chunk.requiredArity;
            mli.chunk = &chunk;
            methodLabels.push_back(std::move(mli));
        }
    }

    // R149: 编译期扫描所有 chunk 的 OP_DEFINE_CLASS 指令，收集类名到 classNameSet_
    // 用途：OP_CALL handler 中 funcTable 找不到时检查此集合，命中则走 emitClassNew 路径
    // （MiniLang 语法 `Point(3, 4)` 编译为 OP_CALL 而非 OP_CLASS_NEW，需 fallback）
    // 与 StackVM executeCallByName 中 classInfo_.find(funName) fallback 对齐
    classNameSet_.clear();
    collectClassNamesFromChunk(result.mainChunk);
    for (const auto& [name, chunk] : result.functionChunks) {
        collectClassNamesFromChunk(chunk);
    }

    // ---- 收集所有 chunk：mainChunk 在前，functionChunks 在后 ----
    // 编译顺序不影响正确性（通过 Label 跳转），但 mainChunk 必须先编译
    // 作为 JIT 入口（prologue 后第一条指令属于 mainChunk）
    std::vector<const BytecodeChunk*> allChunks;
    allChunks.push_back(&result.mainChunk);
    for (const auto& [name, chunk] : result.functionChunks) {
        allChunks.push_back(&chunk);
    }

    // R150: 初始化 per-chunk 调用计数数组与名称映射（热点检测用）
    // 索引 0 = mainChunk（不计数），索引 1..N = functionChunks
    // chunkCallCounts_ 在 execute 入口清零，此处仅设置大小与名称
    chunkNames_.clear();
    chunkNames_.reserve(allChunks.size());
    for (const auto* c : allChunks) {
        chunkNames_.push_back(c->name);
    }
    chunkCallCounts_.assign(allChunks.size(), 0);
    jitContext_.chunkCallCounts = chunkCallCounts_.data();

    // R151: 初始化 per-chunk 热点阈值与重编译标志数组
    // 默认仅方法 chunk（name 含 '.'）启用热点检测，阈值 kDefaultHotThreshold
    // mainChunk 和普通函数 chunk 阈值 0（不触发）；customThresholds_ 覆盖默认值
    hotThresholds_.assign(allChunks.size(), 0);
    for (size_t i = 0; i < allChunks.size(); ++i) {
        const std::string& name = allChunks[i]->name;
        // 方法 chunk 的 name 格式 "ClassName.method"（含 '.'），普通函数无 '.'
        if (name.find('.') != std::string::npos) {
            hotThresholds_[i] = minilang::kDefaultHotThreshold;
        }
        // 应用测试用自定义阈值覆盖
        auto it = customThresholds_.find(name);
        if (it != customThresholds_.end()) {
            hotThresholds_[i] = it->second;
        }
    }
    recompiledFlags_.assign(allChunks.size(), 0);
    jitContext_.hotThresholds = hotThresholds_.data();
    jitContext_.recompiledFlags = recompiledFlags_.data();

    // R152: 初始化 per-chunk 类型反馈计数器数组（特化重编译决策依据）
    // 索引与 chunkCallCounts_ 对齐，在算术指令 genericXXX 路径递增
    typeFeedback_.assign(allChunks.size(), minilang::TypeFeedback{});
    jitContext_.typeFeedback = typeFeedback_.data();
    // R152: 设置 backendPtr 为 this，供 jitTriggerRecompile 访问私有成员（未来扩展）
    jitContext_.backendPtr = this;

    // R154: 初始化 lastMutatedReceiver_ 为 NaN-boxing NULL（嵌套左值赋值链中转邮箱）
    // OP_INDEX_SET 将变异后容器存入此，OP_LOAD_MUTATED push 此到栈顶，
    // OP_WRITEBACK_*_LOCAL/VAR 整体替换栈槽/全局变量为此值
    lastMutatedReceiver_ = static_cast<int64_t>(JIT_NULL_BITS);
    jitContext_.lastMutatedReceiverPtr = &lastMutatedReceiver_;

    // R157: 初始化 per-chunk OSR 循环回边计数/阈值/标志数组
    // 索引与 chunkCallCounts_ 对齐，OP_LOOP 回边时递增 osrLoopCounts_
    // 默认阈值 0（不触发 OSR），测试用 setOsrThreshold 设置 customOsrThresholds_
    osrLoopCounts_.assign(allChunks.size(), 0);
    osrLoopThresholds_.assign(allChunks.size(), 0);
    for (size_t i = 0; i < allChunks.size(); ++i) {
        const std::string& name = allChunks[i]->name;
        auto osrIt = customOsrThresholds_.find(name);
        if (osrIt != customOsrThresholds_.end()) {
            osrLoopThresholds_[i] = osrIt->second;
        }
    }
    osrRecompiledFlags_.assign(allChunks.size(), 0);
    jitContext_.osrLoopCountsPtr = osrLoopCounts_.data();
    jitContext_.osrLoopThresholdsPtr = osrLoopThresholds_.data();
    jitContext_.osrRecompiledFlagsPtr = osrRecompiledFlags_.data();

    // ---- 循环编译每个 chunk ----
    for (size_t chunkIdx = 0; chunkIdx < allChunks.size(); ++chunkIdx) {
        const BytecodeChunk& chunk = *allChunks[chunkIdx];
        const bool isMain = (chunkIdx == 0);
        currentChunkIdx_ = chunkIdx; // R159: 更新 emitCheckInt 使用的 chunk 索引

        // ---- 第一遍扫描：收集所有跳转目标位置，为每个位置创建 Label ----
        // R162: 新增 OP_TRY_BEGIN（catchOffset 相对偏移）和 OP_PUSH_JUMP_TARGET（绝对目标）
        const auto& bytecodes = chunk.code;
        std::unordered_map<size_t, Label> jumpLabels;
        {
            size_t scanIp = 0;
            while (scanIp < bytecodes.size()) {
                OpCode scanOp = static_cast<OpCode>(bytecodes[scanIp]);
                if (scanOp == OpCode::OP_JUMP || scanOp == OpCode::OP_JUMP_IF_FALSE || scanOp == OpCode::OP_LOOP ||
                    scanOp == OpCode::OP_PUSH_JUMP_TARGET) {
                    // 绝对偏移目标：OP_JUMP/OP_JUMP_IF_FALSE/OP_LOOP/OP_PUSH_JUMP_TARGET
                    if (scanIp + 2 < bytecodes.size()) {
                        uint16_t target = static_cast<uint16_t>(bytecodes[scanIp + 1]) |
                                          (static_cast<uint16_t>(bytecodes[scanIp + 2]) << 8);
                        if (target < bytecodes.size() && jumpLabels.find(target) == jumpLabels.end()) {
                            jumpLabels[target] = a.new_label();
                        }
                    }
                } else if (scanOp == OpCode::OP_TRY_BEGIN) {
                    // 相对偏移目标：catchIp = scanIp + 3 + catchOffset
                    if (scanIp + 2 < bytecodes.size()) {
                        uint16_t catchOffset = static_cast<uint16_t>(bytecodes[scanIp + 1]) |
                                               (static_cast<uint16_t>(bytecodes[scanIp + 2]) << 8);
                        size_t catchTarget = scanIp + 3 + catchOffset;
                        if (catchTarget < bytecodes.size() && jumpLabels.find(catchTarget) == jumpLabels.end()) {
                            jumpLabels[catchTarget] = a.new_label();
                        }
                    }
                }
                scanIp += chunk.instructionSizeAt(scanIp);
            }
        }

        // 非 mainChunk：绑定函数入口 Label（OP_CALL 通过此 Label 跳转）
        if (!isMain) {
            auto it = funcTable.find(chunk.name);
            if (it != funcTable.end()) {
                a.bind(it->second.entryLabel);
                // R150: 注入 per-chunk 调用计数器递增指令（热点检测用）
                // JitContext.chunkCallCounts (offset 104) 是指向 uint64_t 数组的指针，
                // 不能直接 inc [r12 + 104 + chunkIdx*8]（那会写穿 JitContext 结构体）。
                // 正确做法：先加载指针到临时寄存器，再通过指针递增数组元素。
                // 与 globalSlots 访问模式一致：mov rcx, [r12+24]; mov rax, [rcx + slot*8]
                // rcx 在函数入口处是空闲的临时寄存器（后续字节码会自行设置寄存器状态）。
                a.mov(x86::rcx, x86::qword_ptr(x86::r12, 104));
                a.inc(x86::qword_ptr(x86::rcx, static_cast<int32_t>(chunkIdx * 8)));

                // R151: 热点阈值检查（协作式安全点）
                // 检查当前 chunk 调用计数是否达到阈值，且尚未触发过重编译。
                // 仅当 hotThresholds_[chunkIdx] > 0 时注入阈值检查代码（阈值为 0 时
                // 完全跳过代码生成，运行时零开销）。默认仅方法 chunk（name 含 '.'）
                // 启用热点检测，mainChunk 和普通函数 chunk 阈值为 0。
                if (chunkIdx < hotThresholds_.size() && hotThresholds_[chunkIdx] > 0) {
                    Label skipRecompile = a.new_label();
                    // 1. 检查是否已触发过重编译（recompiledFlags[chunkIdx] == 0 才继续）
                    a.mov(x86::rcx, x86::qword_ptr(x86::r12, 120)); // rcx = recompiledFlags 指针
                    a.mov(x86::rax, x86::qword_ptr(x86::rcx, static_cast<int32_t>(chunkIdx * 8)));
                    a.test(x86::rax, x86::rax);
                    a.jnz(skipRecompile); // 已触发过，跳过
                    // 2. 比较调用计数与阈值
                    a.mov(x86::rcx, x86::qword_ptr(x86::r12, 104)); // rcx = chunkCallCounts 指针
                    a.mov(x86::rax, x86::qword_ptr(x86::rcx, static_cast<int32_t>(chunkIdx * 8)));
                    a.mov(x86::rcx, x86::qword_ptr(x86::r12, 112)); // rcx = hotThresholds 指针
                    a.cmp(x86::rax, x86::qword_ptr(x86::rcx, static_cast<int32_t>(chunkIdx * 8)));
                    a.jb(skipRecompile); // 未达阈值，跳过
                    // 3. 达到阈值：标记为已触发（避免重复触发）
                    a.mov(x86::rcx, x86::qword_ptr(x86::r12, 120)); // rcx = recompiledFlags 指针
                    a.mov(x86::qword_ptr(x86::rcx, static_cast<int32_t>(chunkIdx * 8)), 1);
                    // 4. 同步栈顶到 JitContext.stackTop（辅助函数可能读栈）
                    a.mov(x86::qword_ptr(x86::r12, jit_offset::stackTop), x86::r15);
                    // 5. 调用 jitTriggerRecompile(ctx, chunkIdx)
                    //    Windows x64: rcx=ctx, rdx=chunkIdx; 需要 shadow space 32B
                    //    栈对齐：方法 chunk 入口 rsp%16==8，sub 40 后 rsp%16==0，
                    //    call 后 rsp%16==8（标准入口对齐），与 emitMethodCall 一致
                    a.mov(x86::rcx, x86::r12);                       // arg1 = ctx
                    a.mov(x86::rdx, static_cast<int32_t>(chunkIdx)); // arg2 = chunkIdx
#ifdef _WIN32
                    a.sub(x86::rsp, 32); // shadow space (32) + 8B 对齐填充
#else
                    a.sub(x86::rsp, 16); // 16-byte alignment
#endif
                    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitTriggerRecompile));
                    a.call(x86::rax);
#ifdef _WIN32
                    a.add(x86::rsp, 32);
#else
                    a.add(x86::rsp, 16);
#endif
                    // 6. 恢复栈顶（辅助函数可能修改了 stackTop）
                    a.mov(x86::r15, x86::qword_ptr(x86::r12, jit_offset::stackTop));
                    a.bind(skipRecompile);
                }
            }
        }

        // ---- 第二遍扫描：字节码遍历与机器码生成 ----
        size_t ip = 0;

        while (ip < bytecodes.size()) {
            // 如果当前 ip 是跳转目标，绑定对应的 Label
            auto labelIt = jumpLabels.find(ip);
            if (labelIt != jumpLabels.end()) {
                a.bind(labelIt->second);
            }

            OpCode op = static_cast<OpCode>(bytecodes[ip]);

            switch (op) {
            case OpCode::OP_INT: {
                // R142 阶段 3a：编译期编码为 NaN-boxing INT
                // OP_INT + 2 字节常量池索引（小端序）
                if (ip + 2 >= bytecodes.size()) {
                    compileError("OP_INT 操作数越界");
                    return nullptr;
                }
                uint16_t idx =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                if (idx >= chunk.constants.size()) {
                    compileError("OP_INT 常量池索引越界: " + std::to_string(idx));
                    return nullptr;
                }
                int64_t value = chunk.constants[idx].intVal();
                uint64_t encoded;
                if (!encodeNanBoxInt(value, encoded)) {
                    compileError("JIT 阶段 3a 不支持超 int48 范围的整数: " + std::to_string(value));
                    return nullptr;
                }
                a.movabs(x86::rax, encoded);
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                ip += 3;
                break;
            }

            case OpCode::OP_FLOAT: {
                // R143 阶段 3b：编译期编码为 NaN-boxing FLOAT
                // 直接存储 double 的 64 位原始位，若落入 boxed NaN 范围则替换为 NAN_BOXED_FLOAT_MARKER
                if (ip + 2 >= bytecodes.size()) {
                    compileError("OP_FLOAT 操作数越界");
                    return nullptr;
                }
                uint16_t idx =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                if (idx >= chunk.constants.size()) {
                    compileError("OP_FLOAT 常量池索引越界: " + std::to_string(idx));
                    return nullptr;
                }
                double value = chunk.constants[idx].floatVal();
                uint64_t encoded = encodeNanBoxFloat(value);
                a.movabs(x86::rax, encoded);
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                ip += 3;
                break;
            }

            case OpCode::OP_STRING: {
                // R143 阶段 3b：编译期编码为 NaN-boxing STRING（POINTER tag + StringData* 指针）
                // 常量池中的 Value 已是 NaN-boxing 指针编码，直接提取 raw bits 作为立即数
                if (ip + 2 >= bytecodes.size()) {
                    compileError("OP_STRING 操作数越界");
                    return nullptr;
                }
                uint16_t idx =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                if (idx >= chunk.constants.size()) {
                    compileError("OP_STRING 常量池索引越界: " + std::to_string(idx));
                    return nullptr;
                }
                // Bug #21 安全性注释：此处 memcpy 绕过了 Value 的引用计数机制，但安全：
                //   1. 常量池字符串的生命周期由 CompileResult 持有，覆盖整个 JIT 代码执行期
                //   2. encoded 仅用于 movabs 立即数（编译期固定），不持有指针所有权
                //   3. 不能用 valueToBits()（会 detach 置 null 破坏常量池）
                // 结论：降级处理，无需代码修改，仅添加安全性说明。
                const Value& strVal = chunk.constants[idx];
                uint64_t encoded;
                std::memcpy(&encoded, &strVal, sizeof(uint64_t));
                a.movabs(x86::rax, encoded);
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                ip += 3;
                break;
            }

            case OpCode::OP_NULL: {
                // R142 阶段 3a：NaN-boxing NULL (0x7FFA000000000000)
                a.movabs(x86::rax, JIT_NULL_BITS);
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                ip += 1;
                break;
            }

            case OpCode::OP_TRUE: {
                // R142 阶段 3a：NaN-boxing BOOL true (0x7FF9000000000001)
                a.movabs(x86::rax, JIT_BOOL_TAG_BASE | 1ULL);
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                ip += 1;
                break;
            }

            case OpCode::OP_FALSE: {
                // R142 阶段 3a：NaN-boxing BOOL false (0x7FF9000000000000)
                a.movabs(x86::rax, JIT_BOOL_TAG_BASE);
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                ip += 1;
                break;
            }

            case OpCode::OP_ADD: {
                // R143 阶段 3b：类型分派——INT 走原生路径，FLOAT/STRING 走 C++ 辅助
                // R145 修复 Bug 2：INT 原生路径增加 INT64 + INT48 溢出检测，溢出时走 C++ 辅助
                //   （StackVM 通过 Value(int64_t) 自动装箱 BoxedIntData 保留完整值）
                // R161 perf: 非特化模式下 INT 检查失败先走 FLOAT 原生 SSE2 路径，非 FLOAT 再走 C++ 辅助
                // 栈布局：[left, right]（right 在栈顶）
                Label genericAdd = a.new_label();
                Label addEnd = a.new_label();
                // peek right(rax)/left(rcx) 不弹出
                a.mov(x86::rax, x86::qword_ptr(x86::r15));    // rax = right
                a.mov(x86::rcx, x86::qword_ptr(x86::r15, 8)); // rcx = left
                bool useFloatPathAdd = !specializeIntMode_ && !specializeFloatMode_;
                Label floatCheckAdd = useFloatPathAdd ? a.new_label() : Label();
                if (useFloatPathAdd) {
                    emitCheckInt(a, x86::rax, floatCheckAdd);
                    emitCheckInt(a, x86::rcx, floatCheckAdd);
                } else {
                    emitCheckInt(a, x86::rax, genericAdd);
                    emitCheckInt(a, x86::rcx, genericAdd);
                }
                // INT 原生路径：先解码运算（不 pop），检查溢出，再 pop + push
                // 栈上保留原始 raw bits，溢出时 emitCallBinaryHelper 会从栈重新 peek
                a.shl(x86::rax, 16);
                a.sar(x86::rax, 16);
                a.shl(x86::rcx, 16);
                a.sar(x86::rcx, 16);
                a.add(x86::rax, x86::rcx);
                a.jo(genericAdd); // INT64 溢出 → C++ 辅助（OverflowCheck::addOverflow）
                // INT48 范围检查：sign_extend(low48(result)) == result ?
                a.mov(x86::rcx, x86::rax);
                a.shl(x86::rcx, 16);
                a.sar(x86::rcx, 16);
                a.cmp(x86::rcx, x86::rax);
                a.jne(genericAdd); // INT48 溢出 → C++ 辅助（Value(int64_t) 自动 BoxedIntData）
                // 不溢出：pop 两个 + 重编码 + push
                a.add(x86::r15, 16);
                // R161 perf: 用缓存的 rbx(JIT_INT48_MASK)/r14(JIT_INT_TAG_BASE) 替代 movabs
                a.and_(x86::rax, x86::rbx);
                a.or_(x86::rax, x86::r14);
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                a.jmp(addEnd);
                // R161 perf: FLOAT 原生 SSE2 路径
                if (useFloatPathAdd) {
                    a.bind(floatCheckAdd);
                    emitFloatBinaryArith(a, 0 /*add*/, genericAdd, addEnd, chunkIdx);
                }
                // C++ 辅助路径：调用 jitAddGeneric(left, right)
                a.bind(genericAdd);
                emitRecordTypeFeedback(a, x86::rax, chunkIdx);
                emitCallBinaryHelper(a, epilogue, reinterpret_cast<void*>(&jitAddGeneric), /*needCtx*/ true,
                                     /*checkError*/ true);
                a.bind(addEnd);
                ip += 1;
                break;
            }

            case OpCode::OP_SUBTRACT: {
                // R145 修复 Bug 2：INT 原生路径增加 INT64 + INT48 溢出检测
                // R161 perf: 非特化模式下 INT 检查失败先走 FLOAT 原生 SSE2 路径
                Label genericSub = a.new_label();
                Label subEnd = a.new_label();
                a.mov(x86::rax, x86::qword_ptr(x86::r15));    // rax = right
                a.mov(x86::rcx, x86::qword_ptr(x86::r15, 8)); // rcx = left
                bool useFloatPathSub = !specializeIntMode_ && !specializeFloatMode_;
                Label floatCheckSub = useFloatPathSub ? a.new_label() : Label();
                if (useFloatPathSub) {
                    emitCheckInt(a, x86::rax, floatCheckSub);
                    emitCheckInt(a, x86::rcx, floatCheckSub);
                } else {
                    emitCheckInt(a, x86::rax, genericSub);
                    emitCheckInt(a, x86::rcx, genericSub);
                }
                // INT 原生路径：先解码运算（不 pop），检查溢出
                a.shl(x86::rax, 16);
                a.sar(x86::rax, 16);
                a.shl(x86::rcx, 16);
                a.sar(x86::rcx, 16);
                a.sub(x86::rcx, x86::rax); // rcx = left - right，设置 OF
                a.jo(genericSub);          // INT64 溢出 → C++ 辅助
                a.mov(x86::rax, x86::rcx); // 结果移到 rax
                // INT48 范围检查
                a.mov(x86::rcx, x86::rax);
                a.shl(x86::rcx, 16);
                a.sar(x86::rcx, 16);
                a.cmp(x86::rcx, x86::rax);
                a.jne(genericSub); // INT48 溢出 → C++ 辅助
                // 不溢出：pop 两个 + 重编码 + push
                a.add(x86::r15, 16);
                // R161 perf: 用缓存的 rbx(JIT_INT48_MASK)/r14(JIT_INT_TAG_BASE) 替代 movabs
                a.and_(x86::rax, x86::rbx);
                a.or_(x86::rax, x86::r14);
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                a.jmp(subEnd);
                // R161 perf: FLOAT 原生 SSE2 路径
                if (useFloatPathSub) {
                    a.bind(floatCheckSub);
                    emitFloatBinaryArith(a, 1 /*sub*/, genericSub, subEnd, chunkIdx);
                }
                a.bind(genericSub);
                emitRecordTypeFeedback(a, x86::rax, chunkIdx);
                emitCallBinaryHelper(a, epilogue, reinterpret_cast<void*>(&jitSubGeneric), /*needCtx*/ true,
                                     /*checkError*/ true);
                a.bind(subEnd);
                ip += 1;
                break;
            }

            case OpCode::OP_MULTIPLY: {
                // R145 修复 Bug 2：INT 原生路径增加 INT64 + INT48 溢出检测
                // R161 perf: 非特化模式下 INT 检查失败先走 FLOAT 原生 SSE2 路径
                Label genericMul = a.new_label();
                Label mulEnd = a.new_label();
                a.mov(x86::rax, x86::qword_ptr(x86::r15));    // rax = right
                a.mov(x86::rcx, x86::qword_ptr(x86::r15, 8)); // rcx = left
                bool useFloatPathMul = !specializeIntMode_ && !specializeFloatMode_;
                Label floatCheckMul = useFloatPathMul ? a.new_label() : Label();
                if (useFloatPathMul) {
                    emitCheckInt(a, x86::rax, floatCheckMul);
                    emitCheckInt(a, x86::rcx, floatCheckMul);
                } else {
                    emitCheckInt(a, x86::rax, genericMul);
                    emitCheckInt(a, x86::rcx, genericMul);
                }
                // INT 原生路径：先解码运算（不 pop），检查溢出
                a.shl(x86::rax, 16);
                a.sar(x86::rax, 16);
                a.shl(x86::rcx, 16);
                a.sar(x86::rcx, 16);
                a.imul(x86::rax, x86::rcx); // rax = right * left，设置 OF
                a.jo(genericMul);           // INT64 溢出 → C++ 辅助
                // INT48 范围检查
                a.mov(x86::rcx, x86::rax);
                a.shl(x86::rcx, 16);
                a.sar(x86::rcx, 16);
                a.cmp(x86::rcx, x86::rax);
                a.jne(genericMul); // INT48 溢出 → C++ 辅助
                // 不溢出：pop 两个 + 重编码 + push
                a.add(x86::r15, 16);
                // R161 perf: 用缓存的 rbx(JIT_INT48_MASK)/r14(JIT_INT_TAG_BASE) 替代 movabs
                a.and_(x86::rax, x86::rbx);
                a.or_(x86::rax, x86::r14);
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                a.jmp(mulEnd);
                // R161 perf: FLOAT 原生 SSE2 路径
                if (useFloatPathMul) {
                    a.bind(floatCheckMul);
                    emitFloatBinaryArith(a, 2 /*mul*/, genericMul, mulEnd, chunkIdx);
                }
                a.bind(genericMul);
                emitRecordTypeFeedback(a, x86::rax, chunkIdx);
                emitCallBinaryHelper(a, epilogue, reinterpret_cast<void*>(&jitMulGeneric), /*needCtx*/ true,
                                     /*checkError*/ true);
                a.bind(mulEnd);
                ip += 1;
                break;
            }

            case OpCode::OP_DIVIDE: {
                // P0 重构：提取为 emitDivide helper（JITCodeGenHelpers.cpp）
                // 含 INT 原生路径 + FLOAT SSE2 路径 + 除零错误 + C++ 辅助回退
                emitDivide(a, epilogue, chunkIdx);
                ip += 1;
                break;
            }

            case OpCode::OP_MODULO: {
                // P0 重构：提取为 emitModulo helper（JITCodeGenHelpers.cpp）
                // 含 INT 原生路径 + 除零错误 + C++ 辅助回退
                emitModulo(a, epilogue, chunkIdx);
                ip += 1;
                break;
            }

            case OpCode::OP_NEGATE: {
                // R143 阶段 3b：类型分派——INT 走原生路径，FLOAT 走 C++ 辅助
                // R145 修复 Bug 2：INT 原生路径增加 INT64 + INT48 溢出检测
                //   （INT64_MIN 取负会 INT64 溢出；INT48 边界值取负会 INT48 溢出）
                Label genericNeg = a.new_label();
                Label negEnd = a.new_label();
                a.mov(x86::rax, x86::qword_ptr(x86::r15)); // rax = value raw bits
                emitCheckInt(a, x86::rax, genericNeg);
                // INT 原生路径：先解码运算（不 pop），检查溢出
                a.shl(x86::rax, 16);
                a.sar(x86::rax, 16);
                a.neg(x86::rax);  // rax = -value，设置 OF
                a.jo(genericNeg); // INT64 溢出（INT64_MIN 取负）→ C++ 辅助
                // INT48 范围检查
                a.mov(x86::rcx, x86::rax);
                a.shl(x86::rcx, 16);
                a.sar(x86::rcx, 16);
                a.cmp(x86::rcx, x86::rax);
                a.jne(genericNeg); // INT48 溢出 → C++ 辅助
                // 不溢出：重编码 + 原地写回栈顶
                // R161 perf: 用缓存的 rbx(JIT_INT48_MASK)/r14(JIT_INT_TAG_BASE) 替代 movabs
                a.and_(x86::rax, x86::rbx);
                a.or_(x86::rax, x86::r14);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                a.jmp(negEnd);
                // C++ 辅助路径：调用 jitNegateGeneric(value)
                a.bind(genericNeg);
                emitRecordTypeFeedback(a, x86::rax, chunkIdx);
                emitCallUnaryHelper(a, reinterpret_cast<void*>(&jitNegateGeneric));
                a.bind(negEnd);
                ip += 1;
                break;
            }

            // ---- R143 阶段 3b: 比较指令（INT 原生路径 + FLOAT/STRING C++ 辅助路径） ----
            case OpCode::OP_EQUAL: {
                // R143: INT/BOOL/NULL 走 raw bits 比较路径，FLOAT/STRING 走 C++ 辅助
                // （FLOAT 需处理 +0.0/-0.0/NaN，STRING 需比较内容）
                // R144 优化：用 shr 48 + cmp edx, imm32 替代 movabs + and + cmp（消除 3 条 movabs）
                Label genericEq = a.new_label();
                Label eqEnd = a.new_label();
                a.mov(x86::rax, x86::qword_ptr(x86::r15));    // right
                a.mov(x86::rcx, x86::qword_ptr(x86::r15, 8)); // left
                // 检查是否都是 boxed NaN（INT/BOOL/NULL/POINTER）—— FLOAT 不是 boxed NaN
                a.mov(x86::rdx, x86::rax);
                a.shr(x86::rdx, 48);
                a.cmp(x86::edx, 0x7FF8);
                a.jb(genericEq); // tag < 0x7FF8 → FLOAT → 走 C++ 辅助
                a.mov(x86::rdx, x86::rcx);
                a.shr(x86::rdx, 48);
                a.cmp(x86::edx, 0x7FF8);
                a.jb(genericEq); // left 是 FLOAT → 走 C++ 辅助
                // INT/BOOL/NULL 路径：raw bits 比较（注意 STRING 也是 POINTER tag=0x7FFB，
                // 但 STRING 比较需要内容比较，故 POINTER 也走 C++ 辅助）
                a.mov(x86::rdx, x86::rax);
                a.shr(x86::rdx, 48);
                a.cmp(x86::edx, 0x7FFB);
                a.je(genericEq); // POINTER → 可能是 STRING → 走 C++ 辅助
                a.mov(x86::rdx, x86::rcx);
                a.shr(x86::rdx, 48);
                a.cmp(x86::edx, 0x7FFB);
                a.je(genericEq);
                // INT/BOOL/NULL 原生路径：raw bits 比较
                a.add(x86::r15, 16); // pop 两个
                a.cmp(x86::rcx, x86::rax);
                a.sete(x86::cl);
                a.movzx(x86::rax, x86::cl);
                a.movabs(x86::rcx, JIT_BOOL_TAG_BASE);
                a.or_(x86::rax, x86::rcx);
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                a.jmp(eqEnd);
                // C++ 辅助路径：调用 jitEqualGeneric(left, right)
                a.bind(genericEq);
                emitRecordTypeFeedback(a, x86::rax, chunkIdx);
                emitCallBinaryHelper(a, epilogue, reinterpret_cast<void*>(&jitEqualGeneric), /*needCtx*/ false,
                                     /*checkError*/ false);
                a.bind(eqEnd);
                ip += 1;
                break;
            }

            case OpCode::OP_NOT_EQUAL: {
                // R144 优化：用 shr 48 + cmp edx, imm32 替代 movabs + and + cmp（消除 3 条 movabs）
                Label genericNeq = a.new_label();
                Label neqEnd = a.new_label();
                a.mov(x86::rax, x86::qword_ptr(x86::r15));
                a.mov(x86::rcx, x86::qword_ptr(x86::r15, 8));
                a.mov(x86::rdx, x86::rax);
                a.shr(x86::rdx, 48);
                a.cmp(x86::edx, 0x7FF8);
                a.jb(genericNeq);
                a.mov(x86::rdx, x86::rcx);
                a.shr(x86::rdx, 48);
                a.cmp(x86::edx, 0x7FF8);
                a.jb(genericNeq);
                a.mov(x86::rdx, x86::rax);
                a.shr(x86::rdx, 48);
                a.cmp(x86::edx, 0x7FFB);
                a.je(genericNeq);
                a.mov(x86::rdx, x86::rcx);
                a.shr(x86::rdx, 48);
                a.cmp(x86::edx, 0x7FFB);
                a.je(genericNeq);
                // INT/BOOL/NULL 原生路径
                a.add(x86::r15, 16);
                a.cmp(x86::rcx, x86::rax);
                a.setne(x86::cl);
                a.movzx(x86::rax, x86::cl);
                a.movabs(x86::rcx, JIT_BOOL_TAG_BASE);
                a.or_(x86::rax, x86::rcx);
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                a.jmp(neqEnd);
                a.bind(genericNeq);
                emitRecordTypeFeedback(a, x86::rax, chunkIdx);
                emitCallBinaryHelper(a, epilogue, reinterpret_cast<void*>(&jitNotEqualGeneric), /*needCtx*/ false,
                                     /*checkError*/ false);
                a.bind(neqEnd);
                ip += 1;
                break;
            }

            case OpCode::OP_LESS: {
                // R143: INT 走原生路径，FLOAT/STRING 走 C++ 辅助（cmpType=0）
                Label genericLt = a.new_label();
                Label ltEnd = a.new_label();
                a.mov(x86::rax, x86::qword_ptr(x86::r15));
                a.mov(x86::rcx, x86::qword_ptr(x86::r15, 8));
                emitCheckInt(a, x86::rax, genericLt);
                emitCheckInt(a, x86::rcx, genericLt);
                // INT 原生路径
                a.add(x86::r15, 16);
                a.shl(x86::rax, 16);
                a.sar(x86::rax, 16);
                a.shl(x86::rcx, 16);
                a.sar(x86::rcx, 16);
                a.cmp(x86::rcx, x86::rax);
                a.setl(x86::cl);
                a.movzx(x86::rax, x86::cl);
                a.movabs(x86::rcx, JIT_BOOL_TAG_BASE);
                a.or_(x86::rax, x86::rcx);
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                a.jmp(ltEnd);
                // C++ 辅助路径：调用 jitOrderedCompare(left, right, 0)
                a.bind(genericLt);
                emitRecordTypeFeedback(a, x86::rax, chunkIdx);
                emitCallOrderedCompare(a, reinterpret_cast<void*>(&jitOrderedCompare), 0);
                a.bind(ltEnd);
                ip += 1;
                break;
            }

            case OpCode::OP_GREATER: {
                Label genericGt = a.new_label();
                Label gtEnd = a.new_label();
                a.mov(x86::rax, x86::qword_ptr(x86::r15));
                a.mov(x86::rcx, x86::qword_ptr(x86::r15, 8));
                emitCheckInt(a, x86::rax, genericGt);
                emitCheckInt(a, x86::rcx, genericGt);
                a.add(x86::r15, 16);
                a.shl(x86::rax, 16);
                a.sar(x86::rax, 16);
                a.shl(x86::rcx, 16);
                a.sar(x86::rcx, 16);
                a.cmp(x86::rcx, x86::rax);
                a.setg(x86::cl);
                a.movzx(x86::rax, x86::cl);
                a.movabs(x86::rcx, JIT_BOOL_TAG_BASE);
                a.or_(x86::rax, x86::rcx);
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                a.jmp(gtEnd);
                a.bind(genericGt);
                emitRecordTypeFeedback(a, x86::rax, chunkIdx);
                emitCallOrderedCompare(a, reinterpret_cast<void*>(&jitOrderedCompare), 1);
                a.bind(gtEnd);
                ip += 1;
                break;
            }

            case OpCode::OP_LESS_EQUAL: {
                Label genericLe = a.new_label();
                Label leEnd = a.new_label();
                a.mov(x86::rax, x86::qword_ptr(x86::r15));
                a.mov(x86::rcx, x86::qword_ptr(x86::r15, 8));
                emitCheckInt(a, x86::rax, genericLe);
                emitCheckInt(a, x86::rcx, genericLe);
                a.add(x86::r15, 16);
                a.shl(x86::rax, 16);
                a.sar(x86::rax, 16);
                a.shl(x86::rcx, 16);
                a.sar(x86::rcx, 16);
                a.cmp(x86::rcx, x86::rax);
                a.setle(x86::cl);
                a.movzx(x86::rax, x86::cl);
                a.movabs(x86::rcx, JIT_BOOL_TAG_BASE);
                a.or_(x86::rax, x86::rcx);
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                a.jmp(leEnd);
                a.bind(genericLe);
                emitRecordTypeFeedback(a, x86::rax, chunkIdx);
                emitCallOrderedCompare(a, reinterpret_cast<void*>(&jitOrderedCompare), 2);
                a.bind(leEnd);
                ip += 1;
                break;
            }

            case OpCode::OP_GREATER_EQUAL: {
                Label genericGe = a.new_label();
                Label geEnd = a.new_label();
                a.mov(x86::rax, x86::qword_ptr(x86::r15));
                a.mov(x86::rcx, x86::qword_ptr(x86::r15, 8));
                emitCheckInt(a, x86::rax, genericGe);
                emitCheckInt(a, x86::rcx, genericGe);
                a.add(x86::r15, 16);
                a.shl(x86::rax, 16);
                a.sar(x86::rax, 16);
                a.shl(x86::rcx, 16);
                a.sar(x86::rcx, 16);
                a.cmp(x86::rcx, x86::rax);
                a.setge(x86::cl);
                a.movzx(x86::rax, x86::cl);
                a.movabs(x86::rcx, JIT_BOOL_TAG_BASE);
                a.or_(x86::rax, x86::rcx);
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                a.jmp(geEnd);
                a.bind(genericGe);
                emitRecordTypeFeedback(a, x86::rax, chunkIdx);
                emitCallOrderedCompare(a, reinterpret_cast<void*>(&jitOrderedCompare), 3);
                a.bind(geEnd);
                ip += 1;
                break;
            }

            case OpCode::OP_NOT: {
                // R143 阶段 3b：使用 jitTruthy 修复 FLOAT/STRING 的 truthiness 错误
                // jitTruthy 返回 1 (truthy) 或 0 (falsy)，取反后编码为 BOOL
#ifdef _WIN32
                a.mov(x86::rcx, x86::qword_ptr(x86::r15)); // arg1 = value
                a.sub(x86::rsp, 32);
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitTruthy));
                a.call(x86::rax);
                a.add(x86::rsp, 32);
#else
                a.mov(x86::rdi, x86::qword_ptr(x86::r15));
                a.sub(x86::rsp, 16);
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitTruthy));
                a.call(x86::rax);
                a.add(x86::rsp, 16);
#endif
                // rax = truthiness (0/1)，取反
                a.xor_(x86::rax, 1);
                a.movzx(x86::rax, x86::al);
                a.movabs(x86::rcx, JIT_BOOL_TAG_BASE);
                a.or_(x86::rax, x86::rcx);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                ip += 1;
                break;
            }

            case OpCode::OP_PRINT: {
                // R142 阶段 3a：调用 jitPrintValue(ctx, rawBits) 输出 NaN-boxing Value
                a.mov(x86::rax, x86::qword_ptr(x86::r15));
                a.add(x86::r15, 8);
#ifdef _WIN32
                a.mov(x86::rcx, x86::r12);
                a.mov(x86::rdx, x86::rax);
                a.sub(x86::rsp, 32);
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitPrintValue));
                a.call(x86::rax);
                a.add(x86::rsp, 32);
#else
                a.mov(x86::rdi, x86::r12);
                a.mov(x86::rsi, x86::rax);
                a.sub(x86::rsp, 16);
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitPrintValue));
                a.call(x86::rax);
                a.add(x86::rsp, 16);
#endif
                ip += 1;
                break;
            }

            case OpCode::OP_POP: {
                a.add(x86::r15, 8);
                ip += 1;
                break;
            }

            case OpCode::OP_DUP: {
                a.mov(x86::rax, x86::qword_ptr(x86::r15));
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                ip += 1;
                break;
            }

            // ---- R146 阶段 4：数组类型支持（C++ 辅助路径） ----
            // 设计：数组操作涉及动态 count 个元素 + 堆对象 COW，统一通过 C++ 辅助函数实现。
            // JIT 代码在调用前更新 ctx->stackTop = r15，辅助函数通过 ctx->stackTop 读写栈，
            // 调用后 JIT 代码从 ctx->stackTop 恢复 r15（辅助函数可能修改了栈顶）。
            case OpCode::OP_BUILD_ARRAY: {
                if (ip + 1 >= bytecodes.size()) {
                    compileError("OP_BUILD_ARRAY 操作数越界");
                    return nullptr;
                }
                uint8_t count = bytecodes[ip + 1];
                emitBuildArray(a, epilogue, count);
                ip += 2;
                break;
            }
            case OpCode::OP_INDEX_GET: {
                emitIndexGet(a, epilogue);
                ip += 1;
                break;
            }
            case OpCode::OP_INDEX_SET_LOCAL: {
                if (ip + 1 >= bytecodes.size()) {
                    compileError("OP_INDEX_SET_LOCAL 操作数越界");
                    return nullptr;
                }
                uint8_t slot = bytecodes[ip + 1];
                emitIndexSetLocal(a, epilogue, slot);
                ip += 2;
                break;
            }
            // ---- R147 阶段 4b：dict/tuple + OP_INDEX_SET_VAR ----
            case OpCode::OP_BUILD_DICT: {
                if (ip + 1 >= bytecodes.size()) {
                    compileError("OP_BUILD_DICT 操作数越界");
                    return nullptr;
                }
                uint8_t pairCount = bytecodes[ip + 1];
                emitBuildDict(a, epilogue, pairCount);
                ip += 2;
                break;
            }
            case OpCode::OP_BUILD_TUPLE: {
                if (ip + 1 >= bytecodes.size()) {
                    compileError("OP_BUILD_TUPLE 操作数越界");
                    return nullptr;
                }
                uint8_t count = bytecodes[ip + 1];
                emitBuildTuple(a, epilogue, count);
                ip += 2;
                break;
            }
            case OpCode::OP_INDEX_SET_VAR: {
                // 3B 操作数：opcode + nameIdx(2B)
                // nameIdx 是常量池索引，指向变量名字符串
                // 编译期通过 globalNameToSlot_ 解析 nameIdx→slot
                if (ip + 2 >= bytecodes.size()) {
                    compileError("OP_INDEX_SET_VAR 操作数越界");
                    return nullptr;
                }
                uint16_t nameIdx =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                if (nameIdx >= chunk.constants.size()) {
                    compileError("OP_INDEX_SET_VAR 常量池索引越界");
                    return nullptr;
                }
                const std::string& varName = chunk.constants[nameIdx].stringVal();
                auto it = globalNameToSlot_.find(varName);
                if (it == globalNameToSlot_.end()) {
                    compileError("OP_INDEX_SET_VAR 未定义的全局变量: " + varName);
                    return nullptr;
                }
                emitIndexSetGlobal(a, epilogue, it->second);
                ip += 3;
                break;
            }

            // ---- R148 阶段 4c-1+2：类支持（基础，不含方法调用） ----
            case OpCode::OP_CLASS_NEW: {
                // 4B 操作数：opcode + nameIdx(2B) + argCount(1B)
                if (ip + 3 >= bytecodes.size()) {
                    compileError("OP_CLASS_NEW 操作数越界");
                    return nullptr;
                }
                uint16_t nameIdx =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                uint8_t argCount = bytecodes[ip + 3];
                if (nameIdx >= chunk.constants.size()) {
                    compileError("OP_CLASS_NEW 常量池索引越界");
                    return nullptr;
                }
                emitClassNew(a, epilogue, chunk.constants[nameIdx].stringVal().c_str(), argCount);
                ip += 4;
                break;
            }
            case OpCode::OP_INIT_FIELD: {
                // 3B 操作数：opcode + fieldNameIdx(2B)
                if (ip + 2 >= bytecodes.size()) {
                    compileError("OP_INIT_FIELD 操作数越界");
                    return nullptr;
                }
                uint16_t fieldIdx =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                if (fieldIdx >= chunk.constants.size()) {
                    compileError("OP_INIT_FIELD 常量池索引越界");
                    return nullptr;
                }
                emitInitField(a, epilogue, chunk.constants[fieldIdx].stringVal().c_str());
                ip += 3;
                break;
            }
            case OpCode::OP_DEFINE_CLASS: {
                // 5B 操作数：opcode + nameIdx(2B) + superNameIdx(2B)
                // superNameIdx=NO_INDEX 表示无父类
                if (ip + 4 >= bytecodes.size()) {
                    compileError("OP_DEFINE_CLASS 操作数越界");
                    return nullptr;
                }
                uint16_t nameIdx =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                uint16_t superIdx =
                    static_cast<uint16_t>(bytecodes[ip + 3]) | (static_cast<uint16_t>(bytecodes[ip + 4]) << 8);
                if (nameIdx >= chunk.constants.size()) {
                    compileError("OP_DEFINE_CLASS 类名常量池索引越界");
                    return nullptr;
                }
                // R148 fix: 直接引用 chunk.constants 中的字符串，避免局部 std::string
                // 离开作用域后 c_str() 悬垂（emitDefineClass 通过 movabs 将指针嵌入
                // JIT 机器码，execute() 运行时才访问，chunk 随 CompileResult 存活）
                const char* className = chunk.constants[nameIdx].stringVal().c_str();
                const char* superClassName = nullptr;
                if (superIdx != RuntimeLimits::NO_INDEX) {
                    if (superIdx >= chunk.constants.size()) {
                        compileError("OP_DEFINE_CLASS 父类名常量池索引越界");
                        return nullptr;
                    }
                    superClassName = chunk.constants[superIdx].stringVal().c_str();
                }
                emitDefineClass(a, epilogue, className, superClassName);
                ip += 5;
                break;
            }
            case OpCode::OP_MEMBER_GET: {
                // 3B 操作数：opcode + fieldNameIdx(2B)
                if (ip + 2 >= bytecodes.size()) {
                    compileError("OP_MEMBER_GET 操作数越界");
                    return nullptr;
                }
                uint16_t fieldIdx =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                if (fieldIdx >= chunk.constants.size()) {
                    compileError("OP_MEMBER_GET 常量池索引越界");
                    return nullptr;
                }
                emitMemberGet(a, epilogue, chunk.constants[fieldIdx].stringVal().c_str());
                ip += 3;
                break;
            }
            case OpCode::OP_MEMBER_SET_VAR: {
                // 5B 操作数：opcode + varIdx(2B) + fieldNameIdx(2B)
                // varIdx 是常量池索引（变量名），编译期通过 globalNameToSlot_ 解析为 slot
                if (ip + 4 >= bytecodes.size()) {
                    compileError("OP_MEMBER_SET_VAR 操作数越界");
                    return nullptr;
                }
                uint16_t varIdx =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                uint16_t fieldIdx =
                    static_cast<uint16_t>(bytecodes[ip + 3]) | (static_cast<uint16_t>(bytecodes[ip + 4]) << 8);
                if (varIdx >= chunk.constants.size() || fieldIdx >= chunk.constants.size()) {
                    compileError("OP_MEMBER_SET_VAR 常量池索引越界");
                    return nullptr;
                }
                const std::string& varName = chunk.constants[varIdx].stringVal();
                auto varIt = globalNameToSlot_.find(varName);
                if (varIt == globalNameToSlot_.end()) {
                    compileError("OP_MEMBER_SET_VAR 未定义的全局变量: " + varName);
                    return nullptr;
                }
                emitMemberSetVar(a, epilogue, varIt->second, chunk.constants[fieldIdx].stringVal().c_str());
                ip += 5;
                break;
            }
            case OpCode::OP_MEMBER_SET_LOCAL: {
                // 4B 操作数：opcode + slot(1B) + fieldNameIdx(2B)
                if (ip + 3 >= bytecodes.size()) {
                    compileError("OP_MEMBER_SET_LOCAL 操作数越界");
                    return nullptr;
                }
                uint8_t slot = bytecodes[ip + 1];
                uint16_t fieldIdx =
                    static_cast<uint16_t>(bytecodes[ip + 2]) | (static_cast<uint16_t>(bytecodes[ip + 3]) << 8);
                if (fieldIdx >= chunk.constants.size()) {
                    compileError("OP_MEMBER_SET_LOCAL 常量池索引越界");
                    return nullptr;
                }
                emitMemberSetLocal(a, epilogue, slot, chunk.constants[fieldIdx].stringVal().c_str());
                ip += 4;
                break;
            }

            // ---- R148: OP_TYPE_CHECK 兜底（仅跳过，不做实际类型检查） ----
            // JIT 性能优先，类型错误在编译期已部分检查；运行时类型检查是兜底，
            // JIT 跳过以支持带类型注解的类实例化（var p: Foo; 触发 OP_CLASS_NEW）。
            // 栈语义：peek 栈顶值不弹出，JIT 不做任何操作直接跳过 3 字节操作数。
            case OpCode::OP_TYPE_CHECK: {
                if (ip + 2 >= bytecodes.size()) {
                    compileError("OP_TYPE_CHECK 操作数越界");
                    return nullptr;
                }
                ip += 3;
                break;
            }

            // ---- R141: 局部变量指令（r13 = basePointer，[r13 - slot*8]） ----
            // StackVM 语义：bp + slot 向上增长；JIT 栈向下增长，
            // r13 指向 slot 0（最高地址），slot N 在 [r13 - N*8]
            case OpCode::OP_GET_LOCAL: {
                if (ip + 1 >= bytecodes.size()) {
                    compileError("OP_GET_LOCAL 操作数越界");
                    return nullptr;
                }
                uint8_t slot = bytecodes[ip + 1];
                // mov rax, [r13 - slot*8]; push rax
                a.mov(x86::rax, x86::qword_ptr(x86::r13, -static_cast<int32_t>(slot) * 8));
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                ip += 2;
                break;
            }

            case OpCode::OP_SET_LOCAL: {
                // StackVM 语义：peek(0) 写入，不弹出（编译器后续生成 OP_POP）
                if (ip + 1 >= bytecodes.size()) {
                    compileError("OP_SET_LOCAL 操作数越界");
                    return nullptr;
                }
                uint8_t slot = bytecodes[ip + 1];
                // mov rax, [r15]; mov [r13 - slot*8], rax（不调整 r15）
                a.mov(x86::rax, x86::qword_ptr(x86::r15));
                a.mov(x86::qword_ptr(x86::r13, -static_cast<int32_t>(slot) * 8), x86::rax);
                ip += 2;
                break;
            }

            // ---- R141: 跳转指令（2B 绝对偏移，使用第一遍收集的 Label） ----
            case OpCode::OP_JUMP: {
                if (ip + 2 >= bytecodes.size()) {
                    compileError("OP_JUMP 操作数越界");
                    return nullptr;
                }
                uint16_t target =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                auto targetIt = jumpLabels.find(target);
                if (targetIt == jumpLabels.end()) {
                    compileError("OP_JUMP 目标位置无 Label: " + std::to_string(target));
                    return nullptr;
                }
                a.jmp(targetIt->second);
                ip += 3;
                break;
            }

            case OpCode::OP_JUMP_IF_FALSE: {
                // R143 阶段 3b：使用 jitTruthy 修复 FLOAT/STRING 的 truthiness 错误
                // StackVM 语义：不弹出条件值，编译器在后续显式生成 OP_POP
                if (ip + 2 >= bytecodes.size()) {
                    compileError("OP_JUMP_IF_FALSE 操作数越界");
                    return nullptr;
                }
                uint16_t target =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                auto targetIt = jumpLabels.find(target);
                if (targetIt == jumpLabels.end()) {
                    compileError("OP_JUMP_IF_FALSE 目标位置无 Label: " + std::to_string(target));
                    return nullptr;
                }
                // 调用 jitTruthy(value) → rax = 0 (falsy) 或 1 (truthy)
#ifdef _WIN32
                a.mov(x86::rcx, x86::qword_ptr(x86::r15)); // arg1 = value
                a.sub(x86::rsp, 32);
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitTruthy));
                a.call(x86::rax);
                a.add(x86::rsp, 32);
#else
                a.mov(x86::rdi, x86::qword_ptr(x86::r15));
                a.sub(x86::rsp, 16);
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitTruthy));
                a.call(x86::rax);
                a.add(x86::rsp, 16);
#endif
                // rax = truthiness，若 0 (falsy) 则跳转
                a.test(x86::rax, x86::rax);
                a.jz(targetIt->second);
                ip += 3;
                break;
            }

            case OpCode::OP_LOOP: {
                if (ip + 2 >= bytecodes.size()) {
                    compileError("OP_LOOP 操作数越界");
                    return nullptr;
                }
                uint16_t target =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                auto targetIt = jumpLabels.find(target);
                if (targetIt == jumpLabels.end()) {
                    compileError("OP_LOOP 目标位置无 Label: " + std::to_string(target));
                    return nullptr;
                }
                // P0 重构：提取为 emitLoop helper（JITCodeGenHelpers.cpp）
                // 含 safepoint GC 轮询 + OSR 回边计数 + OSR 入口点生成 + jmp target
                emitLoop(a, epilogue, targetIt->second, chunkIdx);
                ip += 3;
                break;
            }

            // ---- R139: 全局变量 slot 指令 ----
            case OpCode::OP_GET_GLOBAL: {
                if (ip + 2 >= bytecodes.size()) {
                    compileError("OP_GET_GLOBAL 操作数越界");
                    return nullptr;
                }
                uint16_t slot =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                a.mov(x86::rcx, x86::qword_ptr(x86::r12, jit_offset::globalSlots));
                a.mov(x86::rax, x86::qword_ptr(x86::rcx, static_cast<int32_t>(slot) * 8));
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                ip += 3;
                break;
            }

            case OpCode::OP_SET_GLOBAL: {
                if (ip + 2 >= bytecodes.size()) {
                    compileError("OP_SET_GLOBAL 操作数越界");
                    return nullptr;
                }
                uint16_t slot =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                a.mov(x86::rax, x86::qword_ptr(x86::r15));
                a.add(x86::r15, 8);
                a.mov(x86::rcx, x86::qword_ptr(x86::r12, jit_offset::globalSlots));
                a.mov(x86::qword_ptr(x86::rcx, static_cast<int32_t>(slot) * 8), x86::rax);
                ip += 3;
                break;
            }

            case OpCode::OP_DEFINE_GLOBAL: {
                // 语义与 OP_SET_GLOBAL 相同
                if (ip + 2 >= bytecodes.size()) {
                    compileError("OP_DEFINE_GLOBAL 操作数越界");
                    return nullptr;
                }
                uint16_t slot =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                a.mov(x86::rax, x86::qword_ptr(x86::r15));
                a.add(x86::r15, 8);
                a.mov(x86::rcx, x86::qword_ptr(x86::r12, jit_offset::globalSlots));
                a.mov(x86::qword_ptr(x86::rcx, static_cast<int32_t>(slot) * 8), x86::rax);
                ip += 3;
                break;
            }

            // ---- R141: 函数调用指令（OP_CALL，4B: op + nameIdx(2B) + argCount(1B)） ----
            // StackVM 语义：参数已按序 push（arg0 在底，argN-1 在顶），按名查找 functionChunks_
            // JIT 实现：保存调用者帧 → 预分配 extraSlots → 设置 r13 → jmp 函数 Label
            // L18 eng-tailcall: OP_TAIL_CALL 字节布局与 OP_CALL 完全相同且后随 OP_RETURN，
            // JIT 自管理帧栈无需帧复用，按普通调用处理即语义等价（call 后 RETURN）。
            case OpCode::OP_TAIL_CALL:
            case OpCode::OP_CALL: {
                if (ip + 3 >= bytecodes.size()) {
                    compileError("OP_CALL 操作数越界");
                    return nullptr;
                }
                uint16_t nameIdx =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                uint8_t argCount = bytecodes[ip + 3];
                if (nameIdx >= chunk.constants.size()) {
                    compileError("OP_CALL 常量池索引越界: " + std::to_string(nameIdx));
                    return nullptr;
                }
                std::string funName = chunk.constants[nameIdx].stringVal();
                auto funcIt = funcTable.find(funName);
                if (funcIt == funcTable.end()) {
                    // R149: funcTable 找不到时检查是否是类名构造（`Point(3, 4)` 语法）
                    // MiniLang 编译器将 `ClassName(args)` 编译为 OP_CALL 而非 OP_CLASS_NEW
                    // 与 StackVM executeCallByName 中 classInfo_.find(funName) fallback 对齐
                    if (classNameSet_.find(funName) != classNameSet_.end()) {
                        // 是类名构造：栈布局与 OP_CLASS_NEW 一致（[argN-1]...[arg0]）
                        // 直接复用 emitClassNew 路径（含 R149 init 方法调用支持）
                        emitClassNew(a, epilogue, chunk.constants[nameIdx].stringVal().c_str(), argCount);
                        ip += 4;
                        break;
                    }
                    compileError("JIT 不支持的函数调用（未注册）: " + funName);
                    return nullptr;
                }

                // P0 重构：OP_CALL fast path / mailbox path 已提取为 emitCallDispatch /
                // emitCallMailbox helper（JITCodeGenHelpers.cpp），此处仅做分派。
                //   emitCallDispatch：eager + 无默认参数 + 无 upvalues + 无内部闭包 → 编译期 jmp
                //   emitCallMailbox ：lazy mode / 有默认参数 / 有 upvalues / 有内部闭包 → jitCallByName
                // 详见 JITCodeGenHelpers.cpp 中两函数的实现注释（含 R161 perf 决策依据）。
                if (emitCallDispatch(a, epilogue, funcIt->second, argCount)) {
                    ip += 4;
                    break;
                }

                // R157 邮箱模式：jitCallByName 通过邮箱返回入口地址 + localCount，
                // lazyMode_ 下 entryPtr 为 null 时触发 compileSingleChunkLazy。
                emitCallMailbox(a, epilogue, chunk.constants[nameIdx].stringVal().c_str(), argCount);
                ip += 4;
                break;
            }

            // ---- R149: 方法调用指令（OP_METHOD_CALL，7B） ----
            // 操作数：opcode(1) + nameIdx(2) + argCount(1) + receiverVarIdx(2) + receiverLocalSlotByte(1)
            //   nameIdx              方法名常量池索引
            //   argCount             实际参数个数
            //   receiverVarIdx       接收者变量名常量池索引（NO_INDEX=无全局接收者 writeBack）
            //   receiverLocalSlotByte 接收者局部 slot（NO_SLOT=无局部接收者 writeBack）
            // 栈布局（调用前）：[argN-1]...[arg0][receiver] ← r15 指向 argN-1
            // 栈布局（调用后）：[extraSlots...][defaults...][args...][fields...][this]
            case OpCode::OP_METHOD_CALL: {
                if (ip + 6 >= bytecodes.size()) {
                    compileError("OP_METHOD_CALL 操作数越界");
                    return nullptr;
                }
                uint16_t nameIdx =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                uint8_t argCount = bytecodes[ip + 3];
                uint16_t receiverVarIdx =
                    static_cast<uint16_t>(bytecodes[ip + 4]) | (static_cast<uint16_t>(bytecodes[ip + 5]) << 8);
                uint8_t receiverLocalSlotByte = bytecodes[ip + 6];
                if (nameIdx >= chunk.constants.size()) {
                    compileError("OP_METHOD_CALL 方法名常量池索引越界");
                    return nullptr;
                }
                // R148 教训：直接引用 chunk.constants 中的字符串，避免局部 std::string
                // 析构导致 movabs 嵌入的指针悬垂
                const char* methodName = chunk.constants[nameIdx].stringVal().c_str();

                // 编译期解析 receiverVarIdx → 全局 slot（若 receiverVarIdx != RuntimeLimits::NO_INDEX）
                int receiverGlobalSlot = -1;
                if (receiverLocalSlotByte == RuntimeLimits::NO_SLOT && receiverVarIdx != RuntimeLimits::NO_INDEX) {
                    if (receiverVarIdx >= chunk.constants.size()) {
                        compileError("OP_METHOD_CALL 接收者变量名常量池索引越界");
                        return nullptr;
                    }
                    const std::string& varName = chunk.constants[receiverVarIdx].stringVal();
                    auto it = globalNameToSlot_.find(varName);
                    if (it == globalNameToSlot_.end()) {
                        compileError("OP_METHOD_CALL 未定义的全局接收者: " + varName);
                        return nullptr;
                    }
                    receiverGlobalSlot = it->second;
                }

                emitMethodCall(a, epilogue, methodName, argCount, receiverLocalSlotByte, receiverGlobalSlot,
                               /*isSuperCall=*/false, /*superClassName=*/nullptr);
                ip += 7;
                break;
            }

            // ---- R149: super 方法调用指令（OP_SUPER_CALL，9B） ----
            // 操作数：opcode(1) + nameIdx(2) + argCount(1) + receiverVarIdx(2) +
            //         receiverLocalSlotByte(1) + classIdx(2)
            //   classIdx 父类名常量池索引（编译期编码，避免运行时实例类名查找死循环）
            case OpCode::OP_SUPER_CALL: {
                if (ip + 8 >= bytecodes.size()) {
                    compileError("OP_SUPER_CALL 操作数越界");
                    return nullptr;
                }
                uint16_t nameIdx =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                uint8_t argCount = bytecodes[ip + 3];
                uint16_t receiverVarIdx =
                    static_cast<uint16_t>(bytecodes[ip + 4]) | (static_cast<uint16_t>(bytecodes[ip + 5]) << 8);
                uint8_t receiverLocalSlotByte = bytecodes[ip + 6];
                uint16_t classIdx =
                    static_cast<uint16_t>(bytecodes[ip + 7]) | (static_cast<uint16_t>(bytecodes[ip + 8]) << 8);
                if (nameIdx >= chunk.constants.size()) {
                    compileError("OP_SUPER_CALL 方法名常量池索引越界");
                    return nullptr;
                }
                if (classIdx >= chunk.constants.size()) {
                    compileError("OP_SUPER_CALL 父类名常量池索引越界");
                    return nullptr;
                }
                const char* methodName = chunk.constants[nameIdx].stringVal().c_str();
                const char* superClassName = chunk.constants[classIdx].stringVal().c_str();

                int receiverGlobalSlot = -1;
                if (receiverLocalSlotByte == RuntimeLimits::NO_SLOT && receiverVarIdx != RuntimeLimits::NO_INDEX) {
                    if (receiverVarIdx >= chunk.constants.size()) {
                        compileError("OP_SUPER_CALL 接收者变量名常量池索引越界");
                        return nullptr;
                    }
                    const std::string& varName = chunk.constants[receiverVarIdx].stringVal();
                    auto it = globalNameToSlot_.find(varName);
                    if (it == globalNameToSlot_.end()) {
                        compileError("OP_SUPER_CALL 未定义的全局接收者: " + varName);
                        return nullptr;
                    }
                    receiverGlobalSlot = it->second;
                }

                emitMethodCall(a, epilogue, methodName, argCount, receiverLocalSlotByte, receiverGlobalSlot,
                               /*isSuperCall=*/true, superClassName);
                ip += 9;
                break;
            }

            // ---- R156: 闭包创建指令（OP_CLOSURE，变长: 4 + 2*upvalueCount） ----
            // R156 完整版：支持 upvalue 捕获（isLocal 直接捕获 + passthrough 透传）
            // 与 StackVM executeClosure 对齐（VMCalls.cpp:1022-1083）
            case OpCode::OP_CLOSURE: {
                if (ip + 3 >= bytecodes.size()) {
                    compileError("OP_CLOSURE 操作数越界");
                    return nullptr;
                }
                uint16_t nameIdx =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                uint8_t upvalueCount = bytecodes[ip + 3];
                if (static_cast<size_t>(ip + 4 + 2 * upvalueCount) > bytecodes.size()) {
                    compileError("OP_CLOSURE upvalue 描述符越界");
                    return nullptr;
                }
                if (nameIdx >= chunk.constants.size()) {
                    compileError("OP_CLOSURE 常量池索引越界");
                    return nullptr;
                }
                std::string funName = chunk.constants[nameIdx].stringVal();
                auto funcIt = funcTable.find(funName);
                if (funcIt == funcTable.end() || !funcIt->second.chunk) {
                    compileError("OP_CLOSURE 未注册的函数: " + funName);
                    return nullptr;
                }
                // R148 教训：直接引用 chunk.name（result 生命期跨越 execute），避免局部 string 析构
                const char* funNamePtr = funcIt->second.chunk->name.c_str();
                const void* chunkPtr = funcIt->second.chunk;
                // R156: upvalue 描述符指针（指向字节码中第一个 [isLocal, index] 对）
                const uint8_t* upvalueDescs = bytecodes.data() + ip + 4;
                // P0 重构：提取为 emitClosure helper（JITCodeGenHelpers.cpp）
                // 含 jitCreateClosure 调用 + 闭包值压栈 + 错误检查
                emitClosure(a, epilogue, funNamePtr, chunkPtr, upvalueCount, upvalueDescs);
                // OP_CLOSURE(1) + nameIdx(2) + upvalueCount(1) + 2*upvalueCount = 4 + 2*upvalueCount
                ip += 4 + 2 * upvalueCount;
                break;
            }

            // ---- R156: OP_CLOSE_UPVALUE（2B: op + slotBase(1B)） ----
            // StackVM 语义：关闭所有指向 slot >= basePointer+slotBase 的 open upvalues
            // JIT 语义：关闭所有 address <= (r13 - slotBase*8) 的 open upvalues（栈向下增长方向反转）
            case OpCode::OP_CLOSE_UPVALUE: {
                if (ip + 1 >= bytecodes.size()) {
                    compileError("OP_CLOSE_UPVALUE 操作数越界");
                    return nullptr;
                }
                uint8_t slotBase = bytecodes[ip + 1];
                // 同步栈顶（jitCloseUpvalues 不操作栈，保持邮箱一致性）
                a.mov(x86::qword_ptr(x86::r12, jit_offset::stackTop), x86::r15);
                // 计算 fromAddr = r13 - slotBase*8
                // lea rdx, [r13 - slotBase*8]
                a.lea(x86::rdx, x86::qword_ptr(x86::r13, -static_cast<int32_t>(slotBase) * 8));
#ifdef _WIN32
                // Win32: rcx=ctx, rdx=fromAddr
                a.mov(x86::rcx, x86::r12);
                // rdx 已是 fromAddr
                a.sub(x86::rsp, 32); // shadow space + alignment
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitCloseUpvalues));
                a.call(x86::rax);
                a.add(x86::rsp, 32);
#else
                // Linux: rdi=ctx, rsi=fromAddr
                a.mov(x86::rdi, x86::r12);
                a.mov(x86::rsi, x86::rdx);
                a.sub(x86::rsp, 16);
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitCloseUpvalues));
                a.call(x86::rax);
                a.add(x86::rsp, 16);
#endif
                ip += 2;
                break;
            }

            // ---- R156: OP_GET_UPVALUE（2B: op + uvIdx(1B)） ----
            // StackVM 语义：push frame.upvalues[uvIdx] 的值（closed 读 value，open 读栈槽）
            case OpCode::OP_GET_UPVALUE: {
                if (ip + 1 >= bytecodes.size()) {
                    compileError("OP_GET_UPVALUE 操作数越界");
                    return nullptr;
                }
                uint8_t uvIdx = bytecodes[ip + 1];
                // 同步栈顶
                a.mov(x86::qword_ptr(x86::r12, jit_offset::stackTop), x86::r15);
#ifdef _WIN32
                a.mov(x86::rcx, x86::r12); // arg1 = ctx
                a.xor_(x86::rdx, x86::rdx);
                a.mov(x86::dl, uvIdx); // arg2 = uvIdx
                a.sub(x86::rsp, 32);
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitGetUpvalue));
                a.call(x86::rax);
                a.add(x86::rsp, 32);
#else
                a.mov(x86::rdi, x86::r12);
                a.xor_(x86::rsi, x86::rsi);
                a.mov(x86::sil, uvIdx);
                a.sub(x86::rsp, 16);
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitGetUpvalue));
                a.call(x86::rax);
                a.add(x86::rsp, 16);
#endif
                // rax = upvalue value bits，push 到 JIT 栈
                a.sub(x86::r15, 8);
                a.mov(x86::qword_ptr(x86::r15), x86::rax);
                // 检查错误
                a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::hasError));
                a.movzx(x86::rax, x86::byte_ptr(x86::rax));
                a.test(x86::rax, x86::rax);
                a.jnz(epilogue);
                ip += 2;
                break;
            }

            // ---- R156: OP_SET_UPVALUE（2B: op + uvIdx(1B)） ----
            // StackVM 语义：peek(0) 写入 upvalue（不消费栈顶，与 OP_SET_LOCAL 一致）
            case OpCode::OP_SET_UPVALUE: {
                if (ip + 1 >= bytecodes.size()) {
                    compileError("OP_SET_UPVALUE 操作数越界");
                    return nullptr;
                }
                uint8_t uvIdx = bytecodes[ip + 1];
                // 同步栈顶
                a.mov(x86::qword_ptr(x86::r12, jit_offset::stackTop), x86::r15);
                // 读取栈顶值（peek，不 pop）
                a.mov(x86::rax, x86::qword_ptr(x86::r15));
#ifdef _WIN32
                // Win32: rcx=ctx, rdx=uvIdx, r8=valueBits
                a.mov(x86::rcx, x86::r12); // arg1 = ctx
                a.xor_(x86::rdx, x86::rdx);
                a.mov(x86::dl, uvIdx);    // arg2 = uvIdx
                a.mov(x86::r8, x86::rax); // arg3 = valueBits（peek 值）
                a.sub(x86::rsp, 32);
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitSetUpvalue));
                a.call(x86::rax);
                a.add(x86::rsp, 32);
#else
                // Linux: rdi=ctx, rsi=uvIdx, rdx=valueBits
                a.mov(x86::rdi, x86::r12);
                a.xor_(x86::rsi, x86::rsi);
                a.mov(x86::sil, uvIdx);
                a.mov(x86::rdx, x86::rax); // arg3 = valueBits
                a.sub(x86::rsp, 16);
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitSetUpvalue));
                a.call(x86::rax);
                a.add(x86::rsp, 16);
#endif
                // 检查错误
                a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::hasError));
                a.movzx(x86::rax, x86::byte_ptr(x86::rax));
                a.test(x86::rax, x86::rax);
                a.jnz(epilogue);
                ip += 2;
                break;
            }

            // ---- R155: 闭包值调用指令（OP_CALL_EXPR，2B: op + argCount(1B)） ----
            // 栈布局（调用前）：[..., closure, arg0, arg1, ..., argN-1]（closure 在参数下方）
            // 栈布局（调用后）：新帧栈 [..., arg0, arg1, ..., argN-1, defaults..., extraSlots...]
            // 与 StackVM executeCallExprValue 对齐（VMCalls.cpp:647-709）
            // R155 简化版：仅支持 upvalueCount=0 的闭包值（无 upvalue 绑定），upvalue 捕获推迟到 R156
            // 实现模式：调用 jitCallExpr C++ 辅助函数（通过邮箱返回入口地址 + localCount），
            //          然后设置 returnAddr + r13 + jmp 入口（参考 emitMethodCall 模式）
            case OpCode::OP_CALL_EXPR: {
                if (ip + 1 >= bytecodes.size()) {
                    compileError("OP_CALL_EXPR 操作数越界");
                    return nullptr;
                }
                uint8_t argCount = bytecodes[ip + 1];
                // P0 重构：提取为 emitCallExpr helper（JITCodeGenHelpers.cpp）
                // 含邮箱模式调用 + returnAddr 设置 + jmp methodEntryPtr
                emitCallExpr(a, epilogue, argCount);
                ip += 2;
                break;
            }

            // ---- R141: 函数返回指令（OP_RETURN，1B） ----
            // StackVM 语义：pop 返回值; frames_.pop_back(); stack_.resize(savedBp); push 返回值
            // JIT 实现：
            //   - 末帧返回（*frameCount == 0）→ jmp epilogue（不 push 返回值）
            //   - 普通返回 → 检查 isMethodCall → 调用 jitMethodReturn 处理 writeBack + 返回值替换
            //              → 恢复 r13/r15 + push 返回值 + jmp returnAddr
            case OpCode::OP_RETURN: {
                // P0 重构：提取为 emitReturn helper（JITCodeGenHelpers.cpp）
                // 含 close upvalue + 方法调用 writeBack + 帧恢复 + jmp returnAddr
                emitReturn(a, epilogue);
                ip += 1;
                break;
            }

                // ---- R154 阶段 5d：嵌套左值赋值链 + OP_LEN + OP_DUP_N ----
                // 设计：补全 StackVM 容器/栈操作子集，使 JIT 支持嵌套索引赋值（arr[i][j]=v）
                // 及 len()/dup_n 操作。OP_CALL_EXPR/upvalue 推迟到 R155（需完整闭包基础设施）。

            case OpCode::OP_LEN: {
                // 1B 操作数：opcode
                // 栈布局：[..., v] → [..., len]（pop 1 + push 1）
                emitLen(a, epilogue);
                ip += 1;
                break;
            }

            case OpCode::OP_DUP_N: {
                // 2B 操作数：opcode + depth(1B)
                // 栈布局：[..., v_depth, ..., v_0] → [..., v_depth, ..., v_0, v_depth_copy]
                if (ip + 1 >= bytecodes.size()) {
                    compileError("OP_DUP_N 操作数越界");
                    return nullptr;
                }
                uint8_t depth = bytecodes[ip + 1];
                emitDupN(a, depth);
                ip += 2;
                break;
            }

            case OpCode::OP_LOAD_MUTATED: {
                // 1B 操作数：opcode
                // 栈布局：[...] → [..., lastMutatedReceiver_]
                emitLoadMutated(a);
                ip += 1;
                break;
            }

            case OpCode::OP_INDEX_SET: {
                // 1B 操作数：opcode
                // 栈布局：[..., obj, innerIdx, val] → [...]（pop 3，变异后容器存入 lastMutatedReceiver_）
                emitIndexSet(a, epilogue);
                ip += 1;
                break;
            }

            case OpCode::OP_WRITEBACK_MEMBER_VAR: {
                // 5B 操作数：opcode + varIdx(2B) + fieldIdx(2B)
                // fieldIdx 仅用于反汇编/调试，运行时不读取（整体替换语义）
                if (ip + 4 >= bytecodes.size()) {
                    compileError("OP_WRITEBACK_MEMBER_VAR 操作数越界");
                    return nullptr;
                }
                uint16_t varIdx =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                uint16_t fieldIdx =
                    static_cast<uint16_t>(bytecodes[ip + 3]) | (static_cast<uint16_t>(bytecodes[ip + 4]) << 8);
                if (varIdx >= chunk.constants.size() || fieldIdx >= chunk.constants.size()) {
                    compileError("OP_WRITEBACK_MEMBER_VAR 常量池索引越界");
                    return nullptr;
                }
                // fieldIdx 仅用于反汇编/调试，运行时不需要（整体替换语义）
                (void)chunk.constants[fieldIdx];
                {
                    const std::string& varName = chunk.constants[varIdx].stringVal();
                    auto it = globalNameToSlot_.find(varName);
                    if (it == globalNameToSlot_.end()) {
                        compileError("OP_WRITEBACK_MEMBER_VAR 未定义的全局变量: " + varName);
                        return nullptr;
                    }
                    emitWritebackVar(a, it->second);
                }
                ip += 5;
                break;
            }

            case OpCode::OP_WRITEBACK_INDEX_VAR: {
                // 3B 操作数：opcode + varIdx(2B)
                if (ip + 2 >= bytecodes.size()) {
                    compileError("OP_WRITEBACK_INDEX_VAR 操作数越界");
                    return nullptr;
                }
                uint16_t varIdx =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                if (varIdx >= chunk.constants.size()) {
                    compileError("OP_WRITEBACK_INDEX_VAR 常量池索引越界");
                    return nullptr;
                }
                {
                    const std::string& varName = chunk.constants[varIdx].stringVal();
                    auto it = globalNameToSlot_.find(varName);
                    if (it == globalNameToSlot_.end()) {
                        compileError("OP_WRITEBACK_INDEX_VAR 未定义的全局变量: " + varName);
                        return nullptr;
                    }
                    emitWritebackVar(a, it->second);
                }
                ip += 3;
                break;
            }

            case OpCode::OP_WRITEBACK_MEMBER_LOCAL: {
                // 4B 操作数：opcode + slot(1B) + fieldIdx(2B)
                // fieldIdx 仅用于反汇编/调试，运行时不读取（整体替换语义）
                if (ip + 3 >= bytecodes.size()) {
                    compileError("OP_WRITEBACK_MEMBER_LOCAL 操作数越界");
                    return nullptr;
                }
                uint8_t slot = bytecodes[ip + 1];
                uint16_t fieldIdx =
                    static_cast<uint16_t>(bytecodes[ip + 2]) | (static_cast<uint16_t>(bytecodes[ip + 3]) << 8);
                if (fieldIdx >= chunk.constants.size()) {
                    compileError("OP_WRITEBACK_MEMBER_LOCAL 常量池索引越界");
                    return nullptr;
                }
                // fieldIdx 仅用于反汇编/调试，运行时不需要（整体替换语义）
                (void)chunk.constants[fieldIdx];
                emitWritebackLocal(a, slot);
                ip += 4;
                break;
            }

            case OpCode::OP_WRITEBACK_INDEX_LOCAL: {
                // 2B 操作数：opcode + slot(1B)
                if (ip + 1 >= bytecodes.size()) {
                    compileError("OP_WRITEBACK_INDEX_LOCAL 操作数越界");
                    return nullptr;
                }
                uint8_t slot = bytecodes[ip + 1];
                emitWritebackLocal(a, slot);
                ip += 2;
                break;
            }

            // ---- R162: 异常处理指令（try/catch/throw + finally 续跳） ----
            // 与 StackVM executeMiscExceptionOps 语义对齐：
            //   OP_TRY_BEGIN: push JitTryHandler{catchAddr, stackBase=r15, frameIndex} 到 tryStack_
            //   OP_TRY_END: pop tryStack_ 顶部当前帧的 handler
            //   OP_THROW: pop 栈顶值 → jitThrow 搜索 handler → 截断栈/jmp catchAddr 或 jmp epilogue
            //   OP_PUSH_JUMP_TARGET: push 续跳目标地址到 pendingJumpStack_
            //   OP_FINALLY_END: pop 续跳目标 → jmp 或继续执行
            //
            // catch 块入口约定：thrown value 已被 jitThrow push 到 handler.stackBase - 1，
            // ctx->stackTop = handler.stackBase - 1（新栈顶），JIT 代码从 ctx 重载 r15/r13 后 jmp catchAddr。
            case OpCode::OP_TRY_BEGIN: {
                if (ip + 2 >= bytecodes.size()) {
                    compileError("OP_TRY_BEGIN 操作数越界");
                    return nullptr;
                }
                uint16_t catchOffset =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                size_t catchTarget = ip + 3 + catchOffset;
                auto catchIt = jumpLabels.find(catchTarget);
                if (catchIt == jumpLabels.end()) {
                    compileError("OP_TRY_BEGIN catch 目标位置无 Label: " + std::to_string(catchTarget));
                    return nullptr;
                }
                // 同步栈顶到 ctx（防御性，jitPushTryHandler 不操作栈但保持模式一致）
                a.mov(x86::qword_ptr(x86::r12, jit_offset::stackTop), x86::r15);
                // lea rdx/rsi, [catchLabel] — 获取 catch 块绝对地址
#ifdef _WIN32
                // Win32: rcx=ctx, rdx=catchAddr, r8=stackBase(r15)
                a.lea(x86::rdx, x86::qword_ptr(catchIt->second));
                a.mov(x86::rcx, x86::r12);
                a.mov(x86::r8, x86::r15);
                a.sub(x86::rsp, 32); // shadow space
#else
                // Linux: rdi=ctx, rsi=catchAddr, rdx=stackBase(r15)
                a.lea(x86::rsi, x86::qword_ptr(catchIt->second));
                a.mov(x86::rdi, x86::r12);
                a.mov(x86::rdx, x86::r15);
                a.sub(x86::rsp, 16); // 16-byte alignment
#endif
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitPushTryHandler));
                a.call(x86::rax);
#ifdef _WIN32
                a.add(x86::rsp, 32);
#else
                a.add(x86::rsp, 16);
#endif
                a.mov(x86::r15, x86::qword_ptr(x86::r12, jit_offset::stackTop));
                ip += 3;
                break;
            }

            case OpCode::OP_TRY_END: {
                // 1B 操作数：opcode
                // 弹出 tryStack_ 顶部当前帧的 handler（正常路径）
                a.mov(x86::qword_ptr(x86::r12, jit_offset::stackTop), x86::r15);
#ifdef _WIN32
                a.mov(x86::rcx, x86::r12);
                a.sub(x86::rsp, 32);
#else
                a.mov(x86::rdi, x86::r12);
                a.sub(x86::rsp, 16);
#endif
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitPopTryHandler));
                a.call(x86::rax);
#ifdef _WIN32
                a.add(x86::rsp, 32);
#else
                a.add(x86::rsp, 16);
#endif
                a.mov(x86::r15, x86::qword_ptr(x86::r12, jit_offset::stackTop));
                ip += 1;
                break;
            }

            case OpCode::OP_THROW: {
                // 1B 操作数：opcode
                // 栈布局：[..., thrownValue] → []
                // pop thrownValue 到 rax（作为 arg2 传递）
                a.mov(x86::rax, x86::qword_ptr(x86::r15));
                a.add(x86::r15, 8);
                // 同步栈顶到 ctx（jitThrow 通过 ctx->stackTop 截断栈）
                a.mov(x86::qword_ptr(x86::r12, jit_offset::stackTop), x86::r15);
                // 调用 jitThrow(ctx, thrownValueBits, currentR13)
                // 返回值 rax = catchAddr（找到）或 nullptr（未捕获，hasError 已设置）
#ifdef _WIN32
                // Win32: rcx=ctx, rdx=thrownValueBits, r8=currentR13
                a.mov(x86::rcx, x86::r12);
                a.mov(x86::rdx, x86::rax);
                a.mov(x86::r8, x86::r13);
                a.sub(x86::rsp, 32);
#else
                // Linux: rdi=ctx, rsi=thrownValueBits, rdx=currentR13
                a.mov(x86::rdi, x86::r12);
                a.mov(x86::rsi, x86::rax);
                a.mov(x86::rdx, x86::r13);
                a.sub(x86::rsp, 16);
#endif
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitThrow));
                a.call(x86::rax);
#ifdef _WIN32
                a.add(x86::rsp, 32);
#else
                a.add(x86::rsp, 16);
#endif
                // rax = catchAddr 或 nullptr
                a.test(x86::rax, x86::rax);
                a.jz(epilogue); // 未捕获异常，跳到 epilogue（hasError 已由 jitThrow 设置）
                // 捕获异常：从 ctx 恢复 r13（可能跨帧传播）和 r15（被 jitThrow 截断）
                // ctx->currentBp (offset 160) = catch 块所在帧的 basePointer
                // ctx->stackTop (offset 48) = catch 块所在帧的栈顶（thrown value 已 push）
                a.mov(x86::r13, x86::qword_ptr(x86::r12, jit_offset::currentBp));
                a.mov(x86::r15, x86::qword_ptr(x86::r12, jit_offset::stackTop));
                // jmp catchAddr（thrown value 在栈顶，catch 块代码消费）
                a.jmp(x86::rax);
                // 不可达：上述 jz(epilogue) 或 jmp(rax) 已转移控制流
                ip += 1;
                break;
            }

            case OpCode::OP_PUSH_JUMP_TARGET: {
                // 3B 操作数：opcode + target(2B, 绝对偏移)
                if (ip + 2 >= bytecodes.size()) {
                    compileError("OP_PUSH_JUMP_TARGET 操作数越界");
                    return nullptr;
                }
                uint16_t target =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                auto targetIt = jumpLabels.find(target);
                if (targetIt == jumpLabels.end()) {
                    compileError("OP_PUSH_JUMP_TARGET 目标位置无 Label: " + std::to_string(target));
                    return nullptr;
                }
                a.mov(x86::qword_ptr(x86::r12, jit_offset::stackTop), x86::r15);
#ifdef _WIN32
                // Win32: rcx=ctx, rdx=targetAddr
                a.lea(x86::rdx, x86::qword_ptr(targetIt->second));
                a.mov(x86::rcx, x86::r12);
                a.sub(x86::rsp, 32);
#else
                // Linux: rdi=ctx, rsi=targetAddr
                a.lea(x86::rsi, x86::qword_ptr(targetIt->second));
                a.mov(x86::rdi, x86::r12);
                a.sub(x86::rsp, 16);
#endif
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitPushJumpTarget));
                a.call(x86::rax);
#ifdef _WIN32
                a.add(x86::rsp, 32);
#else
                a.add(x86::rsp, 16);
#endif
                a.mov(x86::r15, x86::qword_ptr(x86::r12, jit_offset::stackTop));
                ip += 3;
                break;
            }

            case OpCode::OP_FINALLY_END: {
                // 1B 操作数：opcode
                // 从 pendingJumpStack_ pop 目标地址：null = 继续执行，非 null = jmp 目标
                a.mov(x86::qword_ptr(x86::r12, jit_offset::stackTop), x86::r15);
#ifdef _WIN32
                a.mov(x86::rcx, x86::r12);
                a.sub(x86::rsp, 32);
#else
                a.mov(x86::rdi, x86::r12);
                a.sub(x86::rsp, 16);
#endif
                a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitPopJumpTarget));
                a.call(x86::rax);
#ifdef _WIN32
                a.add(x86::rsp, 32);
#else
                a.add(x86::rsp, 16);
#endif
                a.mov(x86::r15, x86::qword_ptr(x86::r12, jit_offset::stackTop));
                // rax = target addr 或 nullptr
                a.test(x86::rax, x86::rax);
                Label continueLabel = a.new_label();
                a.jz(continueLabel); // null = 无续跳，继续执行下一条指令
                a.jmp(x86::rax);     // 非空 = 续跳到 break/continue 目标
                a.bind(continueLabel);
                ip += 1;
                break;
            }

            // ---- R162: 名称变量指令（OP_DEFINE_VAR/OP_GET_VAR/OP_SET_VAR/OP_DELETE_VAR） ----
            // 3B 操作数：opcode + nameIdx(2B, 常量池索引指向变量名字符串)
            // 通过 C++ helper 操作 globals_ map（或 globalSlots_ 快速路径）
            case OpCode::OP_DEFINE_VAR: {
                if (ip + 2 >= bytecodes.size()) {
                    compileError("OP_DEFINE_VAR 操作数越界");
                    return nullptr;
                }
                uint16_t nameIdx =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                if (nameIdx >= chunk.constants.size() || !chunk.constants[nameIdx].isString()) {
                    compileError("OP_DEFINE_VAR 常量池索引越界或类型错误");
                    return nullptr;
                }
                {
                    const char* namePtr = chunk.constants[nameIdx].stringVal().c_str();
                    a.mov(x86::qword_ptr(x86::r12, jit_offset::stackTop), x86::r15); // sync stackTop
                    a.movabs(x86::r10, reinterpret_cast<uint64_t>(namePtr));
#ifdef _WIN32
                    a.mov(x86::rcx, x86::r12); // arg1 = ctx
                    a.mov(x86::rdx, x86::r10); // arg2 = name
                    a.sub(x86::rsp, 32);
#else
                    a.mov(x86::rdi, x86::r12);
                    a.mov(x86::rsi, x86::r10);
                    a.sub(x86::rsp, 16);
#endif
                    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitDefineVar));
                    a.call(x86::rax);
#ifdef _WIN32
                    a.add(x86::rsp, 32);
#else
                    a.add(x86::rsp, 16);
#endif
                    a.mov(x86::r15, x86::qword_ptr(x86::r12, jit_offset::stackTop)); // restore stackTop
                    a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::hasError)); // check hasError
                    a.movzx(x86::rax, x86::byte_ptr(x86::rax));
                    a.test(x86::rax, x86::rax);
                    a.jnz(epilogue);
                }
                ip += 3;
                break;
            }

            case OpCode::OP_GET_VAR: {
                if (ip + 2 >= bytecodes.size()) {
                    compileError("OP_GET_VAR 操作数越界");
                    return nullptr;
                }
                uint16_t nameIdx =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                if (nameIdx >= chunk.constants.size() || !chunk.constants[nameIdx].isString()) {
                    compileError("OP_GET_VAR 常量池索引越界或类型错误");
                    return nullptr;
                }
                {
                    const char* namePtr = chunk.constants[nameIdx].stringVal().c_str();
                    a.mov(x86::qword_ptr(x86::r12, jit_offset::stackTop), x86::r15);
                    a.movabs(x86::r10, reinterpret_cast<uint64_t>(namePtr));
#ifdef _WIN32
                    a.mov(x86::rcx, x86::r12);
                    a.mov(x86::rdx, x86::r10);
                    a.sub(x86::rsp, 32);
#else
                    a.mov(x86::rdi, x86::r12);
                    a.mov(x86::rsi, x86::r10);
                    a.sub(x86::rsp, 16);
#endif
                    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitGetVar));
                    a.call(x86::rax);
#ifdef _WIN32
                    a.add(x86::rsp, 32);
#else
                    a.add(x86::rsp, 16);
#endif
                    // rax = raw bits（错误时为 JIT_NULL_BITS，hasError 已设置）
                    // push result to stack
                    a.sub(x86::r15, 8);
                    a.mov(x86::qword_ptr(x86::r15), x86::rax);
                    // restore stackTop（jitGetVar 不修改栈，但保持模式一致）
                    a.mov(x86::qword_ptr(x86::r12, jit_offset::stackTop), x86::r15);
                    a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::hasError));
                    a.movzx(x86::rax, x86::byte_ptr(x86::rax));
                    a.test(x86::rax, x86::rax);
                    a.jnz(epilogue);
                }
                ip += 3;
                break;
            }

            case OpCode::OP_SET_VAR: {
                if (ip + 2 >= bytecodes.size()) {
                    compileError("OP_SET_VAR 操作数越界");
                    return nullptr;
                }
                uint16_t nameIdx =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                if (nameIdx >= chunk.constants.size() || !chunk.constants[nameIdx].isString()) {
                    compileError("OP_SET_VAR 常量池索引越界或类型错误");
                    return nullptr;
                }
                {
                    const char* namePtr = chunk.constants[nameIdx].stringVal().c_str();
                    a.mov(x86::qword_ptr(x86::r12, jit_offset::stackTop), x86::r15);
                    a.movabs(x86::r10, reinterpret_cast<uint64_t>(namePtr));
#ifdef _WIN32
                    a.mov(x86::rcx, x86::r12);
                    a.mov(x86::rdx, x86::r10);
                    a.sub(x86::rsp, 32);
#else
                    a.mov(x86::rdi, x86::r12);
                    a.mov(x86::rsi, x86::r10);
                    a.sub(x86::rsp, 16);
#endif
                    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitSetVar));
                    a.call(x86::rax);
#ifdef _WIN32
                    a.add(x86::rsp, 32);
#else
                    a.add(x86::rsp, 16);
#endif
                    a.mov(x86::r15, x86::qword_ptr(x86::r12, jit_offset::stackTop));
                    a.mov(x86::rax, x86::qword_ptr(x86::r12, jit_offset::hasError));
                    a.movzx(x86::rax, x86::byte_ptr(x86::rax));
                    a.test(x86::rax, x86::rax);
                    a.jnz(epilogue);
                }
                ip += 3;
                break;
            }

            case OpCode::OP_DELETE_VAR: {
                if (ip + 2 >= bytecodes.size()) {
                    compileError("OP_DELETE_VAR 操作数越界");
                    return nullptr;
                }
                uint16_t nameIdx =
                    static_cast<uint16_t>(bytecodes[ip + 1]) | (static_cast<uint16_t>(bytecodes[ip + 2]) << 8);
                if (nameIdx >= chunk.constants.size() || !chunk.constants[nameIdx].isString()) {
                    compileError("OP_DELETE_VAR 常量池索引越界或类型错误");
                    return nullptr;
                }
                {
                    const char* namePtr = chunk.constants[nameIdx].stringVal().c_str();
                    a.movabs(x86::r10, reinterpret_cast<uint64_t>(namePtr));
#ifdef _WIN32
                    a.mov(x86::rcx, x86::r12);
                    a.mov(x86::rdx, x86::r10);
                    a.sub(x86::rsp, 32);
#else
                    a.mov(x86::rdi, x86::r12);
                    a.mov(x86::rsi, x86::r10);
                    a.sub(x86::rsp, 16);
#endif
                    a.movabs(x86::rax, reinterpret_cast<uint64_t>(&jitDeleteVar));
                    a.call(x86::rax);
#ifdef _WIN32
                    a.add(x86::rsp, 32);
#else
                    a.add(x86::rsp, 16);
#endif
                }
                ip += 3;
                break;
            }

            default:
                // P2-9: 当前 JIT 不支持的指令。
                compileError("JIT 不支持的 OpCode: " + std::string(opCodeName(op)) + " (ip=" + std::to_string(ip) +
                             ")");
                return nullptr;
            }
        }
    }

    // ---- 函数 epilogue ----
    // 所有末帧 OP_RETURN 跳转到此处
    a.bind(epilogue);

    // 设置返回值为 0（PoC 不使用返回值）
    a.xor_(x86::rax, x86::rax);

    // 恢复 callee-saved 寄存器
    a.mov(x86::r12, x86::qword_ptr(x86::rbp, -8));
    a.mov(x86::r13, x86::qword_ptr(x86::rbp, -16));
    a.mov(x86::r14, x86::qword_ptr(x86::rbp, -24));
    a.mov(x86::r15, x86::qword_ptr(x86::rbp, -32));
    a.mov(x86::rbx, x86::qword_ptr(x86::rbp, -40));

    // 释放栈帧并返回
    a.mov(x86::rsp, x86::rbp);
    a.pop(x86::rbp);
    a.ret();

    // ---- 编译并加入 runtime ----
    JitEntryFn entry = nullptr;
    // 拓展二期：在 runtime_.add 前读出捕获的汇编文本（add 后 code 内容被搬迁）
    if (asmCaptureEnabled_) {
        capturedAsm_ = asmLogger.data();
    }
    Error addErr = runtime_.add(&entry, &code);
    if (addErr != kErrorOk) {
        compileError(std::string("asmjit JitRuntime::add 失败: ") + DebugUtils::error_as_string(addErr));
        return nullptr;
    }

    // R149: 填充 methodEntries_（"Class.method" → JitMethodInfo）
    // runtime_.add 内部调用 code.flatten()，之后 label_offset_from_base 返回正确偏移
    // 方法入口运行时地址 = (char*)entry + code.label_offset_from_base(entryLabel)
    methodEntries_.clear();
    for (const auto& mli : methodLabels) {
        JitMethodInfo info;
        uint64_t offset = code.label_offset_from_base(mli.entryLabel);
        info.entryPtr = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(entry) + offset);
        info.localCount = mli.localCount;
        info.arity = mli.arity;
        info.requiredArity = mli.requiredArity;
        info.fieldOrder = &mli.chunk->fieldOrder;
        info.defaultConstIndices = &mli.chunk->defaultConstIndices;
        info.constants = &mli.chunk->constants;
        methodEntries_.emplace(mli.fullName, std::move(info));
    }

    // R155: 填充 funcEntries_（普通函数名 → JitMethodInfo，OP_CALL_EXPR 闭包值调用用）
    // 仅包含不含 '.' 的函数名（方法 chunk 由 methodEntries_ 处理）
    // 键是普通函数名，jitCallExpr 通过 callee.closureName() 查找
    funcEntries_.clear();
    for (const auto& [name, finfo] : funcTable) {
        if (name.find('.') != std::string::npos) {
            continue; // 跳过方法 chunk
        }
        if (!finfo.chunk) {
            continue; // 跳过无 chunk 指针的条目
        }
        JitMethodInfo jmi;
        uint64_t offset = code.label_offset_from_base(finfo.entryLabel);
        jmi.entryPtr = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(entry) + offset);
        jmi.localCount = finfo.localCount;
        jmi.arity = finfo.arity;
        jmi.requiredArity = finfo.requiredArity;
        jmi.fieldOrder = nullptr; // 普通函数无 fieldOrder
        jmi.defaultConstIndices = &finfo.chunk->defaultConstIndices;
        jmi.constants = &finfo.chunk->constants;
        funcEntries_.emplace(name, std::move(jmi));
    }

    // R157: lazy compilation 模式 — 将函数/方法 chunk 的 entryPtr 置空，
    // 使 jitCallByName/jitCallExpr 在首次调用时发现 entryPtr 为 null，
    // 通过 backendPtr 回调 triggerLazyCompile 按需编译该 chunk。
    // mainChunk（索引 0）不参与 lazy 编译，始终保留正常入口。
    // 注意：lazy 编译触发后 entryPtr 会被 compileSingleChunkLazy 更新为真实入口，
    // 后续同 chunk 调用直接走已更新映射，无再次 lazy 触发。
    if (lazyMode_) {
        for (auto& [name, info] : funcEntries_) {
            info.entryPtr = nullptr;
        }
        for (auto& [name, info] : methodEntries_) {
            info.entryPtr = nullptr;
        }
    }

    // R158: OSR 入口点地址计算（仅 osrEntryGenMode_ 且 Label 已生成时）
    // runtime_.add 后 code.flatten() 已完成，label_offset_from_base 返回正确偏移。
    // OSR 入口点运行时地址 = (char*)entry + code.label_offset_from_base(osrEntryLabel_)
    if (osrEntryGenMode_ && osrEntryLabelGenerated_ && osrEntryPointOut_ != nullptr) {
        uint64_t osrOffset = code.label_offset_from_base(osrEntryLabel_);
        *osrEntryPointOut_ = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(entry) + osrOffset);
    }

    // R158: 初始化 per-chunk Tier 跟踪（分层编译状态）
    // 首次编译时所有 chunk 为 Tier 1 (Baseline)，OSR 迁移后升级为 Tier 2 (Specialized)。
    // 反优化时回退为 Tier 1。
    // R159: 分层编译模式下，非 main chunk 初始为 Tier 0 (Interpreter)，
    // 首次调用时通过 lazy compilation 自动升级到 Tier 1 (Baseline)。
    if (chunkTiers_.size() != allChunks.size()) {
        if (tieredMode_) {
            chunkTiers_.assign(allChunks.size(), minilang::JitTier::Interpreter);
            chunkTiers_[0] = minilang::JitTier::Baseline; // mainChunk 始终为 Baseline
        } else {
            chunkTiers_.assign(allChunks.size(), minilang::JitTier::Baseline);
        }
    }

    // R158: 备份 Tier 1 baseline 入口（反优化用）
    // 在特化重编译前备份 baseline 入口，反优化时从备份恢复。
    // 仅在非特化模式（specializeIntMode_==false && specializeFloatMode_==false）时备份，
    // 避免特化版本入口覆盖 baseline 备份。
    if (!specializeIntMode_ && !specializeFloatMode_) {
        for (const auto& [name, info] : methodEntries_) {
            baselineMethodEntries_[name] = info.entryPtr;
        }
        for (const auto& [name, info] : funcEntries_) {
            baselineFuncEntries_[name] = info.entryPtr;
        }
    }

    return entry;
}

// ============================================================
// R150: getHotChunkStats — 返回 per-chunk 调用计数统计（热点检测用）
// ============================================================

#endif // MINILANG_USE_JIT
