// ============================================================
// VMCalls.cpp - split from VM.cpp (C1 fix: reduce single-file complexity)
// Contains: call/method/return/closure/class-related instruction execution
// (executeCallOps / executeReturn / executeCall / executeMethodCall /
//  executeClosure / executeClassNew / executeDefineClass)
// ============================================================

#include "compiler/VM.h"
#include "interpreter/BuiltinMethods.h"  // 共享纯函数层（len/contains/has）
#include "interpreter/NumericUtils.h"    // 共享溢出检查（B6 fix）
#include "interpreter/ErrorFormat.h"     // Dedup-5A: ErrorFormat::format 替代 std::to_string 拼接
#include "common/Utf8Utils.h"            // P0-4 fix: UTF-8 码位工具
#include "Logger.h"
#include <sstream>
#include <climits>
#include <cstdint>
#include <cmath>  // BUG 8.1 fix: std::fmod
#include <algorithm>

// ============================================================
// 调用相关类指令
// ============================================================

VMResult VM::executeCallOps(OpCode op, size_t& ip) {
    // S1 fix: 拆分为 8 个独立方法，降低圈复杂度（原 978 行 → 调度器 + 各方法 < 200 行）
    switch (op) {
    case OpCode::OP_RETURN:     return executeReturn(ip);
    case OpCode::OP_CALL:       return executeCall(ip, false);
    case OpCode::OP_CALL_EXPR:  return executeCall(ip, true);
    case OpCode::OP_SUPER_CALL:
    case OpCode::OP_METHOD_CALL: return executeMethodCall(ip, op);
    case OpCode::OP_CLOSURE:    return executeClosure(ip, op);
    case OpCode::OP_CLASS_NEW:  return executeClassNew(ip, op);
    case OpCode::OP_DEFINE_CLASS: return executeDefineClass(ip, op);
    default:
        return runtimeError(ErrorFormat::format("未知操作码: %d", static_cast<int>(op)));
    }
}

// ============================================================
// S1 fix: executeCallOps 拆分实现
// ============================================================

VMResult VM::executeReturn(size_t& ip) {
    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;
    (void)chunk;  // OP_RETURN 不直接使用 chunk

    Value result = pop();
    // pop() 在栈空时设置 hasError_ 并返回 nullValue()，继续执行会基于错误数据
    // 修改栈/帧状态。提前退出避免状态进一步损坏。
    if (hasError_) return VMResult::VM_RUNTIME_ERROR;
    // 提取标量字段 + move 字符串，避免拷贝整个 VMCallFrame（含 2 个 std::string）
    const size_t savedBp = frames_.back().basePointer;
    const size_t savedReturnIp = frames_.back().returnIp;
    const bool wasMethodCall = frames_.back().isMethodCall;
    const bool wasInitCall = frames_.back().isInitCall;
    const int recvLocalSlot = frames_.back().receiverLocalSlot;
    const BytecodeChunk* retChunk = frames_.back().chunk;
    const bool fieldsModified = frames_.back().fieldsModified;  // VM fix: 脏标记
    std::string recvVarName = std::move(frames_.back().receiverVarName);
    // 在 pop_back 之前保存 ip 值，避免悬空引用
    size_t savedIp = ip;
    // P0-3 fix: 记录当前帧索引，pop 后清理该帧的 tryStack_ handler
    size_t returningFrameIdx = frames_.size() - 1;
    frames_.pop_back();

    // P0-3 fix: 清理属于返回帧的 tryStack_ handler（return 跳出 try 块时 handler 会残留）
    while (!tryStack_.empty() && tryStack_.back().frameIndex >= returningFrameIdx) {
        tryStack_.pop_back();
    }

    // 方法调用字段同步：将方法内修改的字段槽（bp+1..N）同步回 this（bp）
    // B2 fix: 对齐 RegisterVM executeReturnImpl 的 (fieldsModified || isInitCall) 条件。
    // PERF-13 曾移除 wasInitCall 认为 fieldsModified 已覆盖 init 字段写入，但该论证在
    // 多层 super.init() 链中失效：中间层 init 仅调用 super.init() 不直接写字段
    // (fieldsModified=false)，父类 init 修改的 this 经父类帧返回时同步到中间层 this，
    // 但中间层返回时 fieldsModified=false 导致不同步 → caller 丢失父类 init 的修改。
    // 补回 || wasInitCall 保证 init 调用链的 this 修改逐层传播回 caller。
    if (wasMethodCall && savedBp < stack_.size() && (fieldsModified || wasInitCall)) {
        Value& modifiedThis = stack_[savedBp];

        // 先把方法内的字段槽（bp+1..N）同步回 this
        if (modifiedThis.isInstance() && retChunk && !retChunk->fieldOrder.empty()) {
            for (size_t fi = 0; fi < retChunk->fieldOrder.size(); ++fi) {
                size_t pos = savedBp + 1 + fi;
                if (pos < stack_.size()) {
                    modifiedThis.fields()[retChunk->fieldOrder[fi]] = stack_[pos];
                }
            }
        }

        // V-P2-18 fix: 后续均为只读访问，绑定 const 引用避免 COW 深拷贝
        const Value& modifiedThisC = modifiedThis;

        // 写回到接收者的原始位置
        if (!frames_.empty()) {
            VMCallFrame& callerFrame = currentFrame();
            size_t callerBp = callerFrame.basePointer;

            // 路径 A：接收者是全局变量 → 写回 globals_ 或 globalSlots_
            if (!recvVarName.empty() && modifiedThisC.isInstance()) {
                auto gsIt = globalNameToSlot_.find(recvVarName);
                if (gsIt != globalNameToSlot_.end() &&
                    gsIt->second >= 0 &&
                    gsIt->second < static_cast<int>(globalSlots_.size()) &&
                    globalSlots_[gsIt->second].isInstance()) {
                    for (const auto& field : modifiedThisC.fields()) {
                        globalSlots_[gsIt->second].fields()[field.first] = field.second;
                    }
                } else {
                    auto it = globals_.find(recvVarName);
                    if (it != globals_.end() && it->second.isInstance()) {
                        for (const auto& field : modifiedThisC.fields()) {
                            it->second.fields()[field.first] = field.second;
                        }
                    }
                }
            }

            // 路径 B：接收者是调用者帧中的局部变量 → 写回栈帧
            if (recvLocalSlot >= 0 &&
                callerBp + recvLocalSlot < stack_.size()) {
                size_t receiverPos = callerBp + recvLocalSlot;

                if (recvLocalSlot == 0) {
                    // 接收者是调用者的 this（slot 0）：同步所有字段和字段槽
                    // #19 fix: 原 2 次 O(fieldCount) 遍历（先写 this.fields()，再查 fieldOrder
                    // 写槽）合并为 1 次——遍历 modifiedThis.fields() 时同步写两处。slot 索引
                    // 用 #11 的 fieldSlotIndex O(1) 查找。语义等价：对 modifiedThis 中每个
                    // 字段，写入 caller this.fields()；若该字段在 caller 的 fieldOrder 中，
                    // 额外写入对应栈槽。
                    if (callerBp < stack_.size() && stack_[callerBp].isInstance()) {
                        Value& callerThis = stack_[callerBp];
                        const bool hasCallerFieldOrder = callerFrame.chunk && !callerFrame.chunk->fieldOrder.empty();
                        for (const auto& field : modifiedThisC.fields()) {
                            callerThis.fields()[field.first] = field.second;
                            if (hasCallerFieldOrder) {
                                size_t fi = callerFrame.chunk->fieldSlotIndex(field.first);
                                if (fi != SIZE_MAX) {
                                    size_t slotPos = callerBp + 1 + fi;
                                    if (slotPos < stack_.size()) {
                                        stack_[slotPos] = field.second;
                                    }
                                }
                            }
                        }
                    }
                } else {
                    // 接收者是调用者帧中的特定局部变量（字段、参数或局部变量）
                    stack_[receiverPos] = modifiedThis;

                    // 如果接收者是调用者 this 的字段（slot 1..N），也更新 this.fields()
                    if (callerBp < stack_.size() && stack_[callerBp].isInstance() && callerFrame.chunk &&
                        recvLocalSlot <= static_cast<int>(callerFrame.chunk->fieldOrder.size())) {
                        const std::string& fieldName = callerFrame.chunk->fieldOrder[recvLocalSlot - 1];
                        stack_[callerBp].fields()[fieldName] = modifiedThis;
                    }
                }
            }
        }
    }

    // CRITICAL-4 fix: 方法返回时记录 this 到 lastMutatedReceiver_，
    // 供 IR 路径中 visitMethodCall 的 LOAD_MUTATED + STORE 写回变异后接收者。
    // 对于只读方法（fieldsModified == false），stack_[savedBp] 持有原值（未被修改），
    // 写回原值等于不写，安全。对于变异方法，字段同步已将变异后实例写入 stack_[savedBp]。
    // 必须在所有 wasMethodCall 路径设置，否则只读方法的 LOAD_MUTATED 会读到
    // 上一次设置的错误值，导致写回错误对象（回归：ClassMethodNoThis 等）。
    if (wasMethodCall && savedBp < stack_.size()) {
        lastMutatedReceiver_ = stack_[savedBp];
    }

    // init 方法返回 this 实例而非 null（在字段同步之后读取）
    if (wasInitCall) {
        if (savedBp < stack_.size()) {
            result = stack_[savedBp];
        }
    }

    // VM-05/06: 关闭当前帧关联的 open upvalues（在 stack resize 之前）
    // P1-5 fix: 使用统一辅助函数，避免代码重复
    if (retChunk || !frames_.empty()) {
        closeUpvaluesFrom(savedBp);
    }

    if (frames_.empty()) {
        // 末帧返回：先截断栈清理 main 帧的局部变量/字段槽/参数，
        // 仅保留返回值。否则 getStack() 会返回残留垃圾，影响调试器/UI 可视化。
        stack_.resize(savedBp);
        // V-P2-21 fix: result 后续不再使用，std::move 入栈
        push(std::move(result));
        notifyStep(savedIp, OpCode::OP_RETURN);
        return VMResult::VM_OK;
    }
    // 恢复栈：清理当前帧的局部变量和参数
    stack_.resize(savedBp);
    // V-P2-21 fix: result 后续不再使用，std::move 入栈
    push(std::move(result));
    // 恢复 ip
    currentFrame().ip = savedReturnIp;
    notifyStep(savedIp, OpCode::OP_RETURN);
    return VMResult::VM_OK;
}

VMResult VM::executeCall(size_t& ip, bool isExpr) {
    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;
    OpCode op = isExpr ? OpCode::OP_CALL_EXPR : OpCode::OP_CALL;

    if (!isExpr) {
        // ---- OP_CALL ----
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        uint8_t argCount = chunk.code[ip + 3];
        if (idx >= chunk.constants.size()) return runtimeError("常量池索引越界");
        const std::string& funName = chunk.constants[idx].stringVal();

        // P3 fix: 内联缓存快速路径（按指针比较，避免 hash 查找）
        // PERF-14 fix: unordered_map find O(1) 替代数组线性扫描
        const BytecodeChunk* cachedChunk = nullptr;
        const std::string* namePtr = &funName;
        auto ccIt = callCache_.find(namePtr);
        if (ccIt != callCache_.end()) {
            cachedChunk = ccIt->second;
        }

        auto it = cachedChunk
            ? functionChunks_.end()  // 缓存命中，跳过 hash 查找
            : functionChunks_.find(funName);

        if (!cachedChunk && it == functionChunks_.end()) {
            auto classIt = classInfo_.find(funName);
            if (classIt != classInfo_.end()) {
                VMClassInfo& cls = classIt->second;

                // 收集参数
                if (stack_.size() < static_cast<size_t>(argCount)) return runtimeError("栈下溢: OP_CALL ctor");
                SmallArgs<Value> args(argCount);
                for (int i = argCount - 1; i >= 0; --i) {
                    args[i] = pop();
                }

                // 创建新实例
                Value instance = Value::makeInstance(cls.name);
                instance.fields() = cls.fieldDefaults;  // 已含继承字段（OP_DEFINE_CLASS 合并）

                // 检查是否有 init 方法（沿继承链查找）
                const BytecodeChunk* initChunkPtr = findMethodChunk(funName, "init");

                if (initChunkPtr != nullptr) {
                    const BytecodeChunk& initChunk = *initChunkPtr;
                    // P0-1 fix: 使用范围检查支持默认参数，并填充缺失的默认值
                    std::vector<Value> defaults;
                    if (!fillDefaultArgs(initChunk, argCount, funName, defaults)) {
                        return runtimeError(ErrorFormat::format(
                            "构造函数 init 期望 %d-%d 个参数，但传入了 %d 个",
                            initChunk.requiredArity, initChunk.arity,
                            static_cast<int>(argCount)));
                    }
                    // 将默认参数追加到 args 末尾
                    for (auto& d : defaults) {
                        args.push_back(std::move(d));
                    }

                    if (frames_.size() >= MAX_FRAMES) {
                        return runtimeError("调用栈溢出");
                    }

                    // 推入 this
                    push(instance);
                    // 按方法 chunk 声明的字段顺序（含继承字段）推入字段值
                    // 性能修复: push(instance) 后 refCount=2，若用非 const fields() 会触发
                    // ensureUnique COW 深拷贝整个 fields unordered_map。改用 std::as_const
                    // 调用 const 重载，仅读不写时不触发 COW。类构造是高频热路径。
                    int fieldCount = 0;
                    if (initChunk.fieldOrder.empty()) {
                        // IR 路径方法不预留字段槽（见 executeClassNew 同名分支注释）
                        fieldCount = 0;
                    } else {
                        for (const auto& fieldName : initChunk.fieldOrder) {
                            auto fieldIt = std::as_const(instance).fields().find(fieldName);
                            if (fieldIt != std::as_const(instance).fields().end()) {
                                push(fieldIt->second);
                            } else {
                                push(Value::nullValue());
                            }
                        }
                        fieldCount = static_cast<int>(initChunk.fieldOrder.size());
                    }
                    // 推入参数
                    for (const auto& arg : args) {
                        push(arg);
                    }

                    // 预分配局部变量栈空间：方法体内 var 声明的局部变量需要栈槽
                    int preAllocated = 1 + fieldCount + argCount;  // this + 字段 + 参数
                    int extraSlots = initChunk.localCount - preAllocated;
                    // V-P2-1 fix: extraSlots 为负表示帧布局损坏（fieldCount 与编译期不一致）
                    if (extraSlots < 0) {
                        return runtimeError(ErrorFormat::format(
                            "类 %s 的 init 方法帧布局损坏: localCount=%d < preAllocated=%d",
                            funName.c_str(), initChunk.localCount, preAllocated));
                    }
                    for (int i = 0; i < extraSlots; ++i) {
                        push(Value::nullValue());
                    }

                    VMCallFrame newFrame;
                    newFrame.chunk = initChunkPtr;
                    newFrame.returnIp = ip + 4;
                    newFrame.basePointer = stack_.size() - initChunk.localCount;
                    newFrame.functionName = initChunkPtr->name;
                    newFrame.ip = 0;
                    newFrame.isMethodCall = true;  // 使 OP_RETURN 同步字段到 this
                    newFrame.isInitCall = true;     // init 返回 this 而非 null
                    size_t savedIp = ip;
                    frames_.push_back(std::move(newFrame));

                    notifyStep(savedIp, op);
                    return VMResult::VM_OK;
                }

                // 无 init 方法：检查是否有多余参数（与解释器行为保持一致）
                if (argCount > 0) {
                    return runtimeError(ErrorFormat::format(
                        "类 %s 没有 init 方法，但传入了 %d 个参数",
                        cls.name.c_str(), static_cast<int>(argCount)));
                }
                push(instance);
                notifyStep(ip, op);
                ip += 4;
                return VMResult::VM_OK;
            }

            // 既不是函数也不是类：检查是否为 input() 函数
            // E3 fix: 改用共享层 executeSharedInput，统一与 Interpreter 的 input() 语义。
            // WorkerManager 超时回调会抛 std::runtime_error，被 executeSharedInput
            // 捕获并返回 Result::err，此处转为 runtimeError 上报，避免静默返回空串。
            if (funName == "input") {
                // 收集参数（栈上顺序: [arg0]，栈顶是最后一个参数）
                if (stack_.size() < static_cast<size_t>(argCount)) {
                    return runtimeError("栈下溢: OP_CALL input");
                }
                SmallArgs<Value> args(argCount);
                for (int i = argCount - 1; i >= 0; --i) {
                    args[i] = pop();
                }

                int line = 0;
                if (!chunk.lines.empty() && ip < chunk.lines.size()) {
                    line = chunk.lines[ip];
                }

                auto r = executeSharedInput(inputCallback_,
                    args.begin(), argCount, line, 0);
                if (r.is_err()) {
                    return runtimeError(r.error().message);
                }
                push(std::move(r.value()));
                notifyStep(ip, op);
                ip += 4;
                return VMResult::VM_OK;
            }

            // 既不是函数也不是类：检查是否为顶层内置函数
            if (isBuiltinFunction(funName)) {
                // 收集参数（栈上顺序: [arg0, arg1, ..., argN-1]，栈顶是最后一个参数）
                if (stack_.size() < static_cast<size_t>(argCount)) {
                    return runtimeError("栈下溢: OP_CALL builtin");
                }
                SmallArgs<Value> args(argCount);
                for (int i = argCount - 1; i >= 0; --i) {
                    args[i] = pop();
                }

                int line = 0;
                if (!chunk.lines.empty() && ip < chunk.lines.size()) {
                    line = chunk.lines[ip];
                }

                auto r = executeSharedBuiltinFunction(
                    funName, args.begin(), argCount, line, 0);

                if (r.is_err()) {
                    return runtimeError(r.error().message);
                }
                push(std::move(r.value()));
                notifyStep(ip, op);
                ip += 4;
                return VMResult::VM_OK;
            }

            // PERF-12 fix: 批量 pop 用 popN 一次 resize
            popN(argCount);
            return runtimeError("未定义的函数: " + funName);
        }

        // P3: 缓存未命中时写入缓存
        // PERF-14 fix: unordered_map 直接 emplace
        if (!cachedChunk) {
            callCache_[namePtr] = &it->second;
        }

        const BytecodeChunk& targetChunk = cachedChunk ? *cachedChunk : it->second;
        // F10: 支持默认参数，参数数量可在 [requiredArity, arity] 范围内
        if (argCount < static_cast<uint8_t>(targetChunk.requiredArity) ||
            argCount > static_cast<uint8_t>(targetChunk.arity)) {
            return runtimeError(ErrorFormat::format(
                "函数 %s 期望 %d-%d 个参数，但传入了 %d 个",
                funName.c_str(), targetChunk.requiredArity, targetChunk.arity,
                static_cast<int>(argCount)));
        }

        // F10: 为缺失的尾部参数填充默认值
        if (argCount < static_cast<uint8_t>(targetChunk.arity)) {
            int missingCount = targetChunk.arity - argCount;
            int defaultStartIdx = static_cast<int>(targetChunk.defaultConstIndices.size()) - missingCount;
            if (defaultStartIdx < 0 || 
                static_cast<size_t>(defaultStartIdx + missingCount) > targetChunk.defaultConstIndices.size()) {
                return runtimeError("函数 " + funName + " 默认参数索引越界");
            }
            for (int i = defaultStartIdx; i < defaultStartIdx + missingCount; ++i) {
                uint16_t constIdx = targetChunk.defaultConstIndices[i];
                if (constIdx == 0xFFFF) {
                    return runtimeError("函数 " + funName + " 的默认参数包含非字面量表达式，VM 不支持");
                }
                if (constIdx >= targetChunk.constants.size()) {
                    return runtimeError("函数 " + funName + " 默认参数常量索引越界");
                }
                push(targetChunk.constants[constIdx]);
            }
            argCount = static_cast<uint8_t>(targetChunk.arity);
        }

        if (frames_.size() >= MAX_FRAMES) {
            return runtimeError("调用栈溢出");
        }

        // 预分配局部变量栈空间：函数体内 var 声明的局部变量需要栈槽，
        // 但帧创建时栈上只有参数，需补推 null 填充额外槽位
        int extraSlots = targetChunk.localCount - argCount;
        // V-P2-1 fix: extraSlots 为负表示帧布局损坏
        if (extraSlots < 0) {
            return runtimeError(ErrorFormat::format(
                "闭包调用帧布局损坏: localCount=%d < argCount=%d",
                targetChunk.localCount, static_cast<int>(argCount)));
        }
        for (int i = 0; i < extraSlots; ++i) {
            push(Value::nullValue());
        }

        VMCallFrame newFrame;
        newFrame.chunk = &targetChunk;
        newFrame.returnIp = ip + 4;
        newFrame.basePointer = stack_.size() - targetChunk.localCount;
        newFrame.functionName = funName;
        newFrame.ip = 0;
        // VM-05/06: 从闭包注册表附加 upvalues（如果该函数有闭包绑定）
        auto closureIt = functionClosures_.find(funName);
        if (closureIt != functionClosures_.end() && closureIt->second.vmClosure()) {
            newFrame.upvalues = closureIt->second.vmClosure()->upvalues;
        }
        size_t savedIp = ip;
        frames_.push_back(std::move(newFrame));

        notifyStep(savedIp, op);
        return VMResult::VM_OK;
    }

    // ---- OP_CALL_EXPR ----
    {
        uint8_t argCount = chunk.code[ip + 1];
        // VM-05/06: 从栈上获取闭包值和参数，执行调用
        if (stack_.size() < static_cast<size_t>(argCount) + 1) {
            return runtimeError("栈下溢: OP_CALL_EXPR");
        }

        // R3-1 fix: 只弹出闭包值（在栈顶），参数保留在栈上供新帧使用
        // 栈布局: [..., arg0, arg1, ..., argN-1, closure]
        // 弹出 closure 后: [..., arg0, arg1, ..., argN-1] — 参数就位
        Value callee = pop();

        if (!callee.isClosure()) {
            // R3-1 fix: 弹出参数以保持栈平衡
            for (int i = 0; i < argCount; ++i) pop();
            return runtimeError("表达式调用需要函数值");
        }

        // M3 fix: 优先使用闭包值中的 chunkPtr（直接指针，无哈希查找）
        // 回退到名称查找兼容旧闭包（chunkPtr 为 null 时）
        const BytecodeChunk* targetChunkPtr = nullptr;
        if (callee.vmClosure() && callee.vmClosure()->chunkPtr) {
            targetChunkPtr = callee.vmClosure()->chunkPtr;
        } else {
            auto chunkIt = functionChunks_.find(callee.closureName());
            if (chunkIt != functionChunks_.end()) {
                targetChunkPtr = &chunkIt->second;
            }
        }
        if (!targetChunkPtr) {
            // R3-1 fix: 弹出参数保持栈平衡
            for (int i = 0; i < argCount; ++i) pop();
            return runtimeError("未找到函数: " + callee.closureName());
        }

        const BytecodeChunk& targetChunk = *targetChunkPtr;
        // F10: 支持默认参数
        if (argCount < static_cast<uint8_t>(targetChunk.requiredArity) ||
            argCount > static_cast<uint8_t>(targetChunk.arity)) {
            // R3-1 fix: 弹出参数保持栈平衡
            for (int i = 0; i < argCount; ++i) pop();
            return runtimeError(ErrorFormat::format(
                "函数 %s 期望 %d-%d 个参数，但传入了 %d 个",
                callee.closureName().c_str(), targetChunk.requiredArity,
                targetChunk.arity, static_cast<int>(argCount)));
        }

        // F10: 为缺失的尾部参数填充默认值
        if (argCount < static_cast<uint8_t>(targetChunk.arity)) {
            int missingCount = targetChunk.arity - argCount;
            int defaultStartIdx = static_cast<int>(targetChunk.defaultConstIndices.size()) - missingCount;
            if (defaultStartIdx < 0 ||
                static_cast<size_t>(defaultStartIdx + missingCount) > targetChunk.defaultConstIndices.size()) {
                for (int i = 0; i < argCount; ++i) pop();
                return runtimeError("函数 " + callee.closureName() + " 默认参数索引越界");
            }
            for (int i = defaultStartIdx; i < defaultStartIdx + missingCount; ++i) {
                uint16_t constIdx = targetChunk.defaultConstIndices[i];
                if (constIdx == 0xFFFF) {
                    for (int j = 0; j < argCount; ++j) pop();
                    return runtimeError("函数 " + callee.closureName() + " 的默认参数包含非字面量表达式，VM 不支持");
                }
                if (constIdx >= targetChunk.constants.size()) {
                    for (int j = 0; j < argCount; ++j) pop();
                    return runtimeError("函数 " + callee.closureName() + " 默认参数常量索引越界");
                }
                push(targetChunk.constants[constIdx]);
            }
            argCount = static_cast<uint8_t>(targetChunk.arity);
        }

        if (frames_.size() >= MAX_FRAMES) {
            // R3-1 fix: 弹出参数保持栈平衡
            for (int i = 0; i < argCount; ++i) pop();
            return runtimeError("调用栈溢出");
        }

        // 预分配局部变量栈空间
        int extraSlots = targetChunk.localCount - argCount;
        // V-P2-1 fix: extraSlots 为负表示帧布局损坏
        if (extraSlots < 0) {
            return runtimeError(ErrorFormat::format(
                "函数调用帧布局损坏: localCount=%d < argCount=%d",
                targetChunk.localCount, static_cast<int>(argCount)));
        }
        for (int i = 0; i < extraSlots; ++i) {
            push(Value::nullValue());
        }

        VMCallFrame newFrame;
        newFrame.chunk = &targetChunk;
        newFrame.returnIp = ip + 2; // OP_CALL_EXPR 是 2 字节指令
        newFrame.basePointer = stack_.size() - targetChunk.localCount;
        newFrame.functionName = callee.closureName();
        newFrame.ip = 0;

        // VM-05/06: 绑定闭包 upvalues 到新帧
        if (callee.vmClosure()) {
            newFrame.upvalues = callee.vmClosure()->upvalues;
        }

        size_t savedIp = ip;
        frames_.push_back(std::move(newFrame));
        notifyStep(savedIp, op);
        return VMResult::VM_OK;
    }
}

VMResult VM::executeMethodCall(size_t& ip, OpCode op) {
    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;
    bool isSuperCall = (op == OpCode::OP_SUPER_CALL);
    const int instrLen = isSuperCall ? 9 : 7;  // B1 fix: SUPER_CALL 多了 2 字节 classIdx
    uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        uint8_t argCount = chunk.code[ip + 3];
        uint16_t receiverVarIdx = chunk.code[ip + 4] | (chunk.code[ip + 5] << 8);
        uint8_t receiverLocalSlotByte = chunk.code[ip + 6];
        if (idx >= chunk.constants.size()) return runtimeError("常量池索引越界");
        const std::string& methodName = chunk.constants[idx].stringVal();

        // C8: 用 const 引用访问接收者，避免非变异方法的深拷贝
        const Value& obj = peek(argCount);
        // V-P2-25 fix: peek 栈下溢时已设置 hasError_ 并返回 null 哨兵，立即退出避免后续误报
        if (hasError_) return VMResult::VM_RUNTIME_ERROR;
        // C10: 方法名一次性分类为枚举
        BuiltinMethod method = classifyBuiltinMethod(methodName);

        // ---- 数组内置方法 ----
        if (obj.isArray()) {
            // B7 fix: 提取到 dispatchArrayBuiltin，降低 executeCallOps 圈复杂度
            VMResult r = dispatchArrayBuiltin(obj, method, methodName, argCount,
                                               receiverVarIdx, receiverLocalSlotByte,
                                               ip, op, instrLen);
            if (r != VMResult::VM_OK) return r;
            return VMResult::VM_OK;
        }

        // ---- 字典内置方法 ----
        if (obj.isDict()) {
            VMResult r = dispatchDictBuiltin(obj, method, methodName, argCount,
                                              receiverVarIdx, receiverLocalSlotByte,
                                              ip, op, instrLen);
            if (r != VMResult::VM_OK) return r;
            return VMResult::VM_OK;
        }

        // ---- 字符串内置方法（全部非变异，使用 const 引用）----
        if (obj.isString()) {
            VMResult r = dispatchStringBuiltin(obj, method, methodName, argCount,
                                                ip, op, instrLen);
            if (r != VMResult::VM_OK) return r;
            return VMResult::VM_OK;
        }

        // ---- 类实例方法调用 ----
        if (obj.isInstance()) {
            // 拷贝接收者，因为后续 pop() 会使 peek 引用失效
            // V-P2-12 fix: 使用 const 避免后续 fields()/className() 触发 COW 深拷贝
            const Value objCopy = obj;
            // B1 fix: super 调用使用编译时编码的类名（而非运行时实例类名）
            // 避免 3+ 级继承时 super 查找回到子类导致死循环
            std::string searchClassName = objCopy.className();
            if (isSuperCall) {
                uint16_t classIdx = chunk.code[ip + 7] | (chunk.code[ip + 8] << 8);
                // V-P1-7 fix: classIdx 越界应报错而非静默降级到运行时类名（可能导致错误的 super 查找）
                if (classIdx >= chunk.constants.size()) {
                    return runtimeError(ErrorFormat::format(
                        "内部错误: super 调用的类名常量索引越界 (%d)",
                        static_cast<int>(classIdx)));
                }
                searchClassName = chunk.constants[classIdx].stringVal();
                auto clsIt = classInfo_.find(searchClassName);
                if (clsIt == classInfo_.end() || clsIt->second.superClassName.empty()) {
                    return runtimeError("类 " + searchClassName + " 没有父类，不能使用 super");
                }
                searchClassName = clsIt->second.superClassName;
            }
            // 沿继承链查找方法（父类方法也可调用）
            const BytecodeChunk* targetChunkPtr = findMethodChunk(searchClassName, methodName);
            if (targetChunkPtr != nullptr) {
                const BytecodeChunk& targetChunk = *targetChunkPtr;

                // F10: 支持默认参数
                if (argCount < static_cast<uint8_t>(targetChunk.requiredArity) ||
                    argCount > static_cast<uint8_t>(targetChunk.arity)) {
                    // PERF-12 fix: 批量 pop 用 popN（参数 + 接收者）
                    popN(argCount + 1);
                    return runtimeError(ErrorFormat::format(
                        "方法 %s 期望 %d-%d 个参数，但传入了 %d 个",
                        methodName.c_str(), targetChunk.requiredArity,
                        targetChunk.arity, static_cast<int>(argCount)));
                }

                if (frames_.size() >= MAX_FRAMES) {
                    return runtimeError("调用栈溢出");
                }

                // 收集参数（反向填充，省去 reverse）
                if (stack_.size() < static_cast<size_t>(argCount) + 1) return runtimeError("栈下溢: OP_METHOD_CALL");
                SmallArgs<Value> args(argCount);
                for (int i = argCount - 1; i >= 0; --i) {
                    args[i] = pop();
                }
                pop();  // 移除栈上的原始实例

                // 推入 this（拷贝，方法内修改会被 writeBack 写回）
                push(objCopy);
                // 按方法 chunk 声明的字段顺序推入实例字段值
                int fieldCount = 0;
                if (targetChunk.fieldOrder.empty()) {
                    // IR 路径方法不预留字段槽（见 executeClassNew 同名分支注释）
                    fieldCount = 0;
                } else {
                    for (const auto& fieldName : targetChunk.fieldOrder) {
                        auto fieldIt = objCopy.fields().find(fieldName);
                        if (fieldIt != objCopy.fields().end()) {
                            push(fieldIt->second);
                        } else {
                            push(Value::nullValue());
                        }
                    }
                    fieldCount = static_cast<int>(targetChunk.fieldOrder.size());
                }
                // 推入参数
                for (const auto& arg : args) {
                    push(arg);
                }

                // F10: 为缺失的尾部参数填充默认值
                if (argCount < static_cast<uint8_t>(targetChunk.arity)) {
                    int missingCount = targetChunk.arity - argCount;
                    int defaultStartIdx = static_cast<int>(targetChunk.defaultConstIndices.size()) - missingCount;
                    if (defaultStartIdx < 0 ||
                        static_cast<size_t>(defaultStartIdx + missingCount) > targetChunk.defaultConstIndices.size()) {
                        return runtimeError("方法 " + methodName + " 默认参数索引越界");
                    }
                    for (int i = defaultStartIdx; i < defaultStartIdx + missingCount; ++i) {
                        uint16_t constIdx = targetChunk.defaultConstIndices[i];
                        if (constIdx == 0xFFFF) {
                            return runtimeError("方法 " + methodName + " 的默认参数包含非字面量表达式，VM 不支持");
                        }
                        if (constIdx >= targetChunk.constants.size()) {
                            return runtimeError("方法 " + methodName + " 默认参数常量索引越界");
                        }
                        push(targetChunk.constants[constIdx]);
                    }
                    argCount = static_cast<uint8_t>(targetChunk.arity);
                }

                // 预分配局部变量栈空间：方法体内 var 声明的局部变量需要栈槽
                int preAllocated = 1 + fieldCount + argCount;  // this + 字段 + 参数
                int extraSlots = targetChunk.localCount - preAllocated;
                // V-P2-1 fix: extraSlots 为负表示帧布局损坏
                if (extraSlots < 0) {
                    return runtimeError(ErrorFormat::format(
                        "方法 %s 帧布局损坏: localCount=%d < preAllocated=%d",
                        methodName.c_str(), targetChunk.localCount, preAllocated));
                }
                for (int i = 0; i < extraSlots; ++i) {
                    push(Value::nullValue());
                }

                VMCallFrame newFrame;
                newFrame.chunk = targetChunkPtr;
                newFrame.returnIp = ip + instrLen;  // B1 fix: SUPER_CALL 是 9 字节
                newFrame.basePointer = stack_.size() - targetChunk.localCount;
                newFrame.functionName = targetChunk.name;
                newFrame.ip = 0;
                newFrame.isMethodCall = true;
                newFrame.isInitCall = (methodName == "init");  // init 返回 this 而非 null
                // 记录接收者变量名（用于 writeBack 到 globals_）
                if (receiverVarIdx != 0xFFFF && receiverVarIdx < chunk.constants.size() &&
                    chunk.constants[receiverVarIdx].isString()) {
                    newFrame.receiverVarName = chunk.constants[receiverVarIdx].stringVal();
                }
                // 记录接收者局部变量 slot（用于 writeBack 到调用者栈帧）
                newFrame.receiverLocalSlot = (receiverLocalSlotByte == 0xFF) ? -1 : receiverLocalSlotByte;
                size_t savedIp = ip;
                frames_.push_back(std::move(newFrame));

                notifyStep(savedIp, op);
                return VMResult::VM_OK;
            }
        }

        // 方法未找到或对象非实例
        // 先保存类型信息，因为 pop 会使 obj 引用失效
        bool wasInstance = obj.isInstance();
        std::string clsName = wasInstance ? obj.className() : "";
        for (uint8_t i = 0; i < argCount; ++i) pop();
        pop();
        if (wasInstance) {
            return runtimeError("类 " + clsName + " 没有方法 " + methodName);
        } else {
            return runtimeError("方法调用需要类实例");
        }
}

VMResult VM::executeClosure(size_t& ip, OpCode op) {
    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;
    uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        uint8_t upvalueCount = chunk.code[ip + 3]; // VM-05/06: 改为 upvalue 数量
        if (idx >= chunk.constants.size()) return runtimeError("常量池索引越界");
        const std::string& funName = chunk.constants[idx].stringVal();

        // 创建闭包值（参数名由 OP_CALL 按 arity 绑定，此处不填充假名）
        Value closure = Value::makeClosure(funName, nullptr, {});

        // VM-05/06: 创建 VM 闭包数据并绑定 upvalue
        auto vmClosureData = std::make_shared<VMClosureData>();
        vmClosureData->functionName = funName;
        vmClosureData->upvalues.resize(upvalueCount);
        // M3 fix: 闭包值直接持有函数 chunk 指针（OP_CALL_EXPR 使用，避免名称查找）
        auto chunkIt = functionChunks_.find(funName);
        if (chunkIt != functionChunks_.end()) {
            vmClosureData->chunkPtr = &chunkIt->second;
        }

        size_t instrBase = ip + 4; // OP_CLOSURE(1) + nameIdx(2) + upvalueCount(1)

        for (uint8_t i = 0; i < upvalueCount; ++i) {
            uint8_t isLocal = chunk.code[instrBase + i * 2];
            uint8_t uvIndex = chunk.code[instrBase + i * 2 + 1];

            if (isLocal) {
                // 直接捕获：创建新 upvalue 指向调用者帧的栈槽
                auto uv = std::make_shared<VMUpvalue>();
                uv->stackSlot = frame.basePointer + uvIndex;
                uv->isClosed = false;
                // #18 fix: 记录所属帧索引（当前帧 = frames_.size()-1），
                // 供 OP_SET_UPVALUE 跳过 O(frames_) 线性扫描
                uv->owningFrameIdx = frames_.size() - 1;
                vmClosureData->upvalues[i] = uv;
                // B5 fix: 插入有序索引（multimap，按 stackSlot 排序）
                openUpvalues_.emplace(uv->stackSlot, uv);
            } else {
                // 透传：复用调用者帧的 upvalue
                if (static_cast<size_t>(uvIndex) < frame.upvalues.size()) {
                    vmClosureData->upvalues[i] = frame.upvalues[uvIndex];
                } else {
                    // 降级：创建空 upvalue
                    vmClosureData->upvalues[i] = std::make_shared<VMUpvalue>();
                    vmClosureData->upvalues[i]->value = Value::nullValue();
                    vmClosureData->upvalues[i]->isClosed = true;
                }
            }
        }
        closure.vmClosure() = vmClosureData;

        // 注册到函数闭包表（OP_CALL 按名称查找时使用）
        functionClosures_[funName] = closure;

        // V-P2-22 fix: closure 后续不再使用，std::move 入栈（map 已持有拷贝）
        push(std::move(closure));
        notifyStep(ip, op);
        ip = instrBase + upvalueCount * 2; // 跳过 upvalue 描述符
        return VMResult::VM_OK;
}

VMResult VM::executeClassNew(size_t& ip, OpCode op) {
    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;
    pendingFieldOrder_.clear();  // M3: 开始新的类定义，清空字段顺序记录
    uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
    uint8_t argCount = chunk.code[ip + 3];
    if (idx >= chunk.constants.size()) return runtimeError("常量池索引越界");
    const std::string& className = chunk.constants[idx].stringVal();

    // 收集参数（反向填充，省去 reverse）
    if (stack_.size() < argCount) return runtimeError("栈下溢: OP_CLASS_NEW");
    SmallArgs<Value> args(argCount);
    for (int i = argCount - 1; i >= 0; --i) {
        args[i] = pop();
    }

    // 查找类信息
    auto classIt = classInfo_.find(className);
    if (classIt == classInfo_.end()) {
        // 类定义阶段：类尚未注册（OP_DEFINE_CLASS 会随后注册）。
        // 创建空模板实例供 OP_INIT_FIELD 填充字段，不支持带参数构造。
        if (argCount > 0) {
            return runtimeError("类 " + className + " 尚未定义，不能带参数构造");
        }
        Value instance = Value::makeInstance(className);
        push(std::move(instance));
        notifyStep(ip, op);
        ip += 4;
        return VMResult::VM_OK;
    }
        VMClassInfo& cls = classIt->second;

        // 创建新实例
        Value instance = Value::makeInstance(cls.name);
        instance.fields() = cls.fieldDefaults;  // 已含继承字段

        // 检查是否有 init 方法（沿继承链查找）
        const BytecodeChunk* initChunkPtr = findMethodChunk(className, "init");

        // S2 fix: 也处理 argCount==0 且 init.arity==0 的自动构造场景
        // （与解释器 visitVarDecl 一致：Point p; 自动调用 0 参数 init）
        // P1-1 fix: 使用 requiredArity==0 判断自动构造，支持全默认参数 init
        bool shouldCallInit = false;
        if (initChunkPtr != nullptr) {
            if (argCount > 0) {
                shouldCallInit = true;
            } else if (initChunkPtr->requiredArity == 0) {
                shouldCallInit = true;  // 0 必需参数（含全默认参数 init），自动构造时调用
            }
        }

        if (shouldCallInit) {
            // 创建 init 帧执行初始化
            const BytecodeChunk& initChunk = *initChunkPtr;
            // P0-1 fix: 使用范围检查支持默认参数，并填充缺失的默认值
            std::vector<Value> defaults;
            if (!fillDefaultArgs(initChunk, argCount, className, defaults)) {
                return runtimeError(ErrorFormat::format(
                    "构造函数 init 期望 %d-%d 个参数，但传入了 %d 个",
                    initChunk.requiredArity, initChunk.arity,
                    static_cast<int>(argCount)));
            }
            // 将默认参数追加到 args 末尾
            for (auto& d : defaults) {
                args.push_back(std::move(d));
            }

            if (frames_.size() >= MAX_FRAMES) {
                return runtimeError("调用栈溢出");
            }

            // 推入 this
            push(instance);
            // 按方法 chunk 声明的字段顺序（含继承字段）推入字段值
            int fieldCount = 0;
            if (initChunk.fieldOrder.empty()) {
                // IR 路径方法不预留字段槽（字段通过 this.field 成员访问，非本地槽）。
                // Compiler 路径方法总有 fieldOrder（即使空类也设为空 vector，此时
                // instance.fields() 也为空），故此处推 0 个字段对两端均安全。
                fieldCount = 0;
            } else {
                for (const auto& fieldName : initChunk.fieldOrder) {
                    auto fieldIt = instance.fields().find(fieldName);
                    if (fieldIt != instance.fields().end()) {
                        push(fieldIt->second);
                    } else {
                        push(Value::nullValue());
                    }
                }
                fieldCount = static_cast<int>(initChunk.fieldOrder.size());
            }
            // 推入参数
            // V-P2-24 fix: args 后续不再使用，std::move 入栈
            for (auto& arg : args) {
                push(std::move(arg));
            }

            // 预分配局部变量栈空间：方法体内 var 声明的局部变量需要栈槽
            int preAllocated = 1 + fieldCount + argCount;  // this + 字段 + 参数
            int extraSlots = initChunk.localCount - preAllocated;
            // V-P2-1 fix: extraSlots 为负表示帧布局损坏
            if (extraSlots < 0) {
                return runtimeError(ErrorFormat::format(
                    "类 %s 的 init 方法帧布局损坏: localCount=%d < preAllocated=%d",
                    className.c_str(), initChunk.localCount, preAllocated));
            }
            for (int i = 0; i < extraSlots; ++i) {
                push(Value::nullValue());
            }

            VMCallFrame newFrame;
            newFrame.chunk = initChunkPtr;
            newFrame.returnIp = ip + 4;
            newFrame.basePointer = stack_.size() - initChunk.localCount;
            newFrame.functionName = initChunkPtr->name;
            newFrame.ip = 0;
            newFrame.isMethodCall = true;  // 使 OP_RETURN 同步字段到 this
            newFrame.isInitCall = true;     // init 返回 this 而非 null
            size_t savedIp = ip;
            frames_.push_back(std::move(newFrame));

            notifyStep(savedIp, op);
            return VMResult::VM_OK;
        }

        // 无 init 或 init.arity 不匹配 argCount：推入实例（OP_INIT_FIELD 或手动 init 后续处理）
        // V-P1-8 fix: init 存在但参数不匹配（argCount==0 且 init.requiredArity>0）时报错，而非静默跳过
        if (initChunkPtr != nullptr && !shouldCallInit) {
            return runtimeError(ErrorFormat::format(
                "类 %s 的 init 期望至少 %d 个参数，但传入了 %d 个",
                cls.name.c_str(), initChunkPtr->requiredArity,
                static_cast<int>(argCount)));
        }
        push(instance);

        // 无 init 但有参数：报错（与解释器一致）
        if (initChunkPtr == nullptr && argCount > 0) {
            return runtimeError(ErrorFormat::format(
                "类 %s 没有 init 方法，但传入了 %d 个参数",
                cls.name.c_str(), static_cast<int>(argCount)));
        }

        notifyStep(ip, op);
        ip += 4;
        return VMResult::VM_OK;
}

VMResult VM::executeDefineClass(size_t& ip, OpCode op) {
    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;
    // 操作数: nameIdx(2B) + superNameIdx(2B)
    // superNameIdx == 0xFFFF 表示无父类
    uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
    uint16_t superIdx = chunk.code[ip + 3] | (chunk.code[ip + 4] << 8);
    if (idx >= chunk.constants.size()) return runtimeError("常量池索引越界");
    const std::string& className = chunk.constants[idx].stringVal();
    std::string superClassName;
    if (superIdx != 0xFFFF && superIdx < chunk.constants.size()) {
        superClassName = chunk.constants[superIdx].stringVal();
    }
    // V-P2-17 fix: templateInstance 只读不写，改为 const 避免 fields() 触发 COW 深拷贝
    const Value templateInstance = pop();

    // 从模板实例提取当前类字段（M3 fix: 使用 pendingFieldOrder_ 保持声明顺序）
    std::vector<std::string> ownFieldOrder = std::move(pendingFieldOrder_);
    std::unordered_map<std::string, Value> ownFieldDefaults;
    if (templateInstance.isInstance()) {
        for (const auto& fieldName : ownFieldOrder) {
            auto it = templateInstance.fields().find(fieldName);
            if (it != templateInstance.fields().end()) {
                ownFieldDefaults[fieldName] = it->second;
            }
        }
    } else {
        return runtimeError("OP_DEFINE_CLASS: 模板值不是实例");
    }

    // 注册类信息（先注册以支持循环引用安全查找）
    VMClassInfo info;
    info.name = className;
    info.superClassName = superClassName;
    classInfo_[className] = info;

    // 沿继承链合并父类字段（父类字段在前，子类覆盖同名字段）
    // 先按"父→子"顺序收集字段名，子类已存在的字段不重复添加
    std::vector<std::string> mergedOrder;
    std::unordered_map<std::string, Value> mergedDefaults;

    std::vector<std::string> chain;
    std::string cur = superClassName;
    for (int guard = 0; guard < MAX_INHERITANCE_DEPTH && !cur.empty(); ++guard) {
        auto clsIt = classInfo_.find(cur);
        if (clsIt == classInfo_.end()) {
            return runtimeError("未定义的父类: " + cur);
        }
        chain.push_back(cur);
        cur = clsIt->second.superClassName;
    }
    // V-P2 fix: 循环继承检测 — guard 耗尽但 cur 仍非空说明存在继承环
    if (!cur.empty()) {
        return runtimeError("类继承链过深或存在循环继承: " + className + " -> " + cur);
    }
    // 倒序遍历链（最远的祖先在前），保证子类字段覆盖父类字段
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        auto clsIt = classInfo_.find(*it);
        if (clsIt == classInfo_.end()) continue;
        for (const auto& fieldName : clsIt->second.fieldOrder) {
            if (mergedDefaults.find(fieldName) == mergedDefaults.end()) {
                mergedOrder.push_back(fieldName);
                mergedDefaults[fieldName] = clsIt->second.fieldDefaults[fieldName];
            }
        }
    }
    // 当前类字段最后处理（覆盖父类同名字段）
    for (const auto& fieldName : ownFieldOrder) {
        if (mergedDefaults.find(fieldName) == mergedDefaults.end()) {
            mergedOrder.push_back(fieldName);
        }
        mergedDefaults[fieldName] = ownFieldDefaults[fieldName];
    }

    VMClassInfo& registered = classInfo_[className];
    registered.fieldOrder = std::move(mergedOrder);
    registered.fieldDefaults = std::move(mergedDefaults);

    // 在全局变量中存储类标记（与解释器语义一致：类名是类型标识，不是实例）
    Value classVal(std::string("class:") + className);
    auto classGsIt = globalNameToSlot_.find(className);
    // V-P2-23 fix: classVal 后续不再使用，std::move 写入（两分支互斥）
    if (classGsIt != globalNameToSlot_.end()) {
        globalSlots_[classGsIt->second] = std::move(classVal);
    } else {
        globals_[className] = std::move(classVal);
    }

    notifyStep(ip, op);
    ip += 5;  // nameIdx(2B) + superNameIdx(2B) + opcode(1B)
    return VMResult::VM_OK;
}
