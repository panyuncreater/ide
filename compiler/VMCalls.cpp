// ============================================================
// VMCalls.cpp - split from VM.cpp (C1 fix: reduce single-file complexity)
// Contains: call/method/return/closure/class-related instruction execution
// (executeCallOps / executeReturn / executeCall / executeMethodCall /
//  executeClosure / executeClassNew / executeDefineClass)
// ============================================================

#include "common/ErrorFormat.h"   // Dedup-5A: ErrorFormat::format 替代 std::to_string 拼接
#include "common/ErrorMessages.h" // R97 #11 fix: 三后端共享错误消息常量
#include "common/Logger.h"
#include "common/RuntimeLimits.h" // P3-14: NO_INDEX/NO_SLOT 哨兵常量
#include "common/Utf8Utils.h"     // P0-4 fix: UTF-8 码位工具
#include "compiler/VM.h"
#include "interpreter/BuiltinMethods.h" // 共享纯函数层（len/contains/has）
#include "interpreter/NumericUtils.h"   // 共享溢出检查（B6 fix）
#include <algorithm>
#include <climits>
#include <cmath> // BUG 8.1 fix: std::fmod
#include <cstdint>
#include <sstream>

// ============================================================
// 调用相关类指令
// ------------------------------------------------------------
// 调用约定（栈式 VM 统一框架）
// ------------------------------------------------------------
// 每次函数/方法调用都在操作数栈上"就地"构造被调用者的活动记录，布局固定为：
//   [ this(仅方法，slot 0) | field0..fieldN(仅方法) | arg0..argM-1 | local0..localK-1 ]
// 其中：
//   · basePointer = 推入上述所有槽后，stack_.size() - localCount（即第一个槽的索引）。
//     被调用者用 stack_[basePointer+slot] 访问局部/参数/字段，与调用者栈完全隔离。
//   · returnIp = 调用指令的下一条指令地址，OP_RETURN 时恢复到调用者帧 .ip，
//     避免调用者重复执行 CALL 指令（否则会无限递归）。
//   · 参数在调用前已由调用者从右到左压栈（栈顶是最后一个实参）；被调用者弹出参数
//     后 push(this/字段/参数) 重建完整框架，再补 push 剩余 localCount 个 null 槽。
//   · extraSlots = localCount - 已推入槽数，若为负说明编译期帧布局与实际不一致（损坏）。
//   · 调用返回时 OP_RETURN 执行 stack_.resize(savedBp) 一次性回收整个被调用者框架，
//     并 push 返回值；方法调用还会把字段槽同步回 this / 接收者变量（见 writeBack 机制）。
//   · 深度防护：frames_.size() >= MAX_FRAMES 时报"调用栈溢出"；默认参数数量须在
//     [requiredArity, arity] 区间，缺失尾部参数从 defaultConstIndices 取字面量填充。
// ============================================================

VMResult VM::executeCallOps(OpCode op, size_t& ip) {
    // S1 fix: 拆分为 8 个独立方法，降低圈复杂度（原 978 行 → 调度器 + 各方法 < 200 行）
    switch (op) {
    case OpCode::OP_RETURN:
        return executeReturn(ip);
    case OpCode::OP_CALL:
        return executeCall(ip, false);
    case OpCode::OP_CALL_EXPR:
        return executeCall(ip, true);
    case OpCode::OP_SUPER_CALL:
    case OpCode::OP_METHOD_CALL:
        return executeMethodCall(ip, op);
    case OpCode::OP_CLOSURE:
        return executeClosure(ip, op);
    case OpCode::OP_CLASS_NEW:
        return executeClassNew(ip, op);
    case OpCode::OP_DEFINE_CLASS:
        return executeDefineClass(ip, op);
    default:
        return runtimeError(ErrorFormat::formatStd("未知操作码: {}", static_cast<int>(op)));
    }
}

// ============================================================
// S1 fix: executeCallOps 拆分实现
// ============================================================

VMResult VM::executeReturn(size_t& ip) {
    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;
    (void)chunk; // OP_RETURN 不直接使用 chunk

    Value result = pop();
    // pop() 在栈空时设置 hasError_ 并返回 nullValue()，继续执行会基于错误数据
    // 修改栈/帧状态。提前退出避免状态进一步损坏。
    if (hasError_)
        return VMResult::VM_RUNTIME_ERROR;
    // 提取标量字段 + move 字符串，避免拷贝整个 VMCallFrame（含 2 个 std::string）
    const size_t savedBp = frames_.back().basePointer;
    const size_t savedReturnIp = frames_.back().returnIp;
    const bool wasMethodCall = frames_.back().isMethodCall;
    const bool wasInitCall = frames_.back().isInitCall;
    const int recvLocalSlot = frames_.back().receiverLocalSlot;
    const BytecodeChunk* retChunk = frames_.back().chunk;
    const bool fieldsModified = frames_.back().fieldsModified; // VM fix: 脏标记
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
                if (gsIt != globalNameToSlot_.end() && gsIt->second >= 0 &&
                    gsIt->second < static_cast<int>(globalSlots_.size()) && globalSlots_[gsIt->second].isInstance()) {
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
            if (recvLocalSlot >= 0 && callerBp + recvLocalSlot < stack_.size()) {
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
    // AUDIT-P2.5 fix: closeUpvaluesFrom 返回错误时立即终止返回，避免在损坏状态上继续
    // BUG-005 fix: savedBp 越界检查前移到 closeUpvaluesFrom 之前——
    // 原 savedBp > stack_.size() 检查在末尾才执行，但 closeUpvaluesFrom(savedBp)
    // 会基于 savedBp 遍历 openUpvalues_，越界 savedBp 可能导致栈访问越界。
    // 检查前移后，若栈已损坏立即标记并清理为安全状态（避免后续查询返回错误数据）。
    if (savedBp > stack_.size()) {
        // 栈损坏：清理所有帧和栈，防止后续查询（getCallStack/getCurrentFrameLocals）
        // 在已损坏状态上返回错误数据。保留 diagnostics_ 供调用方诊断（P2-12）。
        frames_.clear();
        stack_.clear();
        tryStack_.clear();
        pendingJumpStack_.clear();
        openUpvalues_.clear();
        return runtimeError("返回时栈布局损坏: savedBp > stack size");
    }
    if (retChunk || !frames_.empty()) {
        if (closeUpvaluesFrom(savedBp) != VMResult::VM_OK)
            return VMResult::VM_RUNTIME_ERROR;
    }

    if (frames_.empty()) {
        // 末帧返回：先截断栈清理 main 帧的局部变量/字段槽/参数，
        // 仅保留返回值。否则 getStack() 会返回残留垃圾，影响调试器/UI 可视化。
        // AUDIT-BUG-V5 fix: 防御性检查 savedBp <= stack_.size()，防止栈损坏时 resize abort
        // BUG-005 fix: 检查已在上方前移执行，此处为兜底二次防御
        if (savedBp > stack_.size())
            return runtimeError("返回时栈布局损坏: savedBp > stack size");
        stack_.resize(savedBp);
        // V-P2-21 fix: result 后续不再使用，std::move 入栈
        push(std::move(result));
        notifyStep(savedIp, OpCode::OP_RETURN);
        return VMResult::VM_OK;
    }
    // 恢复栈：清理当前帧的局部变量和参数
    // AUDIT-BUG-V5 fix: 同末帧路径，防御性检查
    // BUG-005 fix: 检查已在上方前移执行，此处为兜底二次防御
    if (savedBp > stack_.size())
        return runtimeError("返回时栈布局损坏: savedBp > stack size");
    stack_.resize(savedBp);
    // V-P2-21 fix: result 后续不再使用，std::move 入栈
    push(std::move(result));
    // 恢复 ip
    currentFrame().ip = savedReturnIp;
    notifyStep(savedIp, OpCode::OP_RETURN);
    return VMResult::VM_OK;
}

// R123 重构：原 executeCall 470 行单函数拆为 thin dispatcher + 7 个 helper。
// 拆分模式：按 callee 类型分组（构造函数/input/高阶函数/内置函数/普通函数/闭包值）+ 共享 setupFunctionCallFrame。
// 与 R122 "每 case 独立 helper" 模式扩展：本函数按 callee 类型而非 OpCode 分发，但同样适合"每路径独立 helper"。
// 共享子任务提取：setupFunctionCallFrame 统一 executeCallFunction 和 executeCallExprValue 两路径的帧构造逻辑
// （默认参数填充 + MAX_FRAMES 检查 + extraSlots 预分配 + newFrame 构造 + push frame），消除约 70 行重复代码。
// 原两路径差异（错误消息文本"闭包调用帧布局损坏"vs"函数调用帧布局损坏"、pop 方式 popN vs for 循环 pop）已统一为
// "函数调用帧布局损坏" + popN(argCount)（更通用 + 更高效，无测试依赖具体文本）。
VMResult VM::executeCall(size_t& ip, bool isExpr) {
    OpCode op = isExpr ? OpCode::OP_CALL_EXPR : OpCode::OP_CALL;
    if (!isExpr) {
        return executeCallByName(ip, op);
    }
    return executeCallExprValue(ip, op);
}

// ============================================================
// executeCallByName - OP_CALL 路径分发器
// 按函数名查找缓存/classInfo_/input/higher-order/builtin/functionChunks_，
// 命中后分发到对应 callee 类型 helper
// ============================================================
VMResult VM::executeCallByName(size_t& ip, OpCode op) {
    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;
    uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
    uint8_t argCount = chunk.code[ip + 3];
    if (idx >= chunk.constants.size())
        return runtimeError("常量池索引越界");
    const std::string& funName = chunk.constants[idx].stringVal();

    // P3 fix: 内联缓存快速路径（按指针比较，避免 hash 查找）
    // PERF-14 fix: unordered_map find O(1) 替代数组线性扫描
    const BytecodeChunk* cachedChunk = nullptr;
    const std::string* namePtr = &funName;
    auto ccIt = callCache_.find(namePtr);
    if (ccIt != callCache_.end()) {
        cachedChunk = ccIt->second;
    }
    auto it = cachedChunk ? functionChunks_.end() // 缓存命中，跳过 hash 查找
                          : functionChunks_.find(funName);

    if (!cachedChunk && it == functionChunks_.end()) {
        auto classIt = classInfo_.find(funName);
        if (classIt != classInfo_.end()) {
            return executeCallConstructor(ip, op, classIt->second, funName, argCount);
        }
        // E3 fix: 改用共享层 executeSharedInput，统一与 Interpreter 的 input() 语义。
        if (funName == "input") {
            return executeCallBuiltinInput(ip, op, argCount, chunk);
        }
        // R98 W2: 高阶函数 map/filter/reduce/forEach/find 优先拦截
        // （需要在 isBuiltinFunction 之前，因为这些名字不在 isBuiltinFunction 注册表中）
        if (isHigherOrderBuiltin(funName)) {
            return executeCallHigherOrder(ip, op, funName, argCount, chunk);
        }
        // R136: spawn(fn, args...) — 需要后端注入 ClosureInvoker，走独立路径
        // 与 input()/高阶函数一样，在 isBuiltinFunction 之前拦截
        if (funName == "spawn") {
            return executeCallSpawn(ip, op, argCount, chunk);
        }
        if (isBuiltinFunction(funName)) {
            return executeCallBuiltinFunction(ip, op, funName, argCount, chunk);
        }
        // PERF-12 fix: 批量 pop 用 popN 一次 resize
        popN(argCount);
        // R164 fix: 三后端消息统一（ErrorMessages 单一真相源）
        return runtimeError(ErrorFormat::formatStd(ErrorMessages::kUndefinedFunctionFmtStd, funName),
                            "undefined-function");
    }

    // P3: 缓存未命中时写入缓存
    // PERF-14 fix: unordered_map 直接 emplace
    if (!cachedChunk) {
        callCache_[namePtr] = &it->second;
    }

    const BytecodeChunk& targetChunk = cachedChunk ? *cachedChunk : it->second;
    return executeCallFunction(ip, op, funName, argCount, targetChunk);
}

// ============================================================
// executeCallConstructor - 类构造调用
// OP_CALL 路径 classInfo_ 命中：创建实例 + findMethodChunk("init") + 默认参数填充 + 帧构造
// 注：此 helper 105 行，标记为二次拆分候选（init 命中分支可进一步提取 setupInitCallFrame）
// ============================================================
VMResult VM::executeCallConstructor(size_t& ip, OpCode op, VMClassInfo& cls, const std::string& funName,
                                    uint8_t argCount) {
    // 收集参数
    if (stack_.size() < static_cast<size_t>(argCount))
        return runtimeError("栈下溢: OP_CALL ctor");
    SmallArgs<Value> args(argCount);
    for (int i = argCount - 1; i >= 0; --i) {
        args[i] = pop();
    }

    // 创建新实例
    Value instance = Value::makeInstance(cls.name);
    instance.fields() = cls.fieldDefaults; // 已含继承字段（OP_DEFINE_CLASS 合并）

    // 检查是否有 init 方法（沿继承链查找）
    const BytecodeChunk* initChunkPtr = findMethodChunk(funName, "init");

    if (initChunkPtr != nullptr) {
        const BytecodeChunk& initChunk = *initChunkPtr;
        // P0-1 fix: 使用范围检查支持默认参数，并填充缺失的默认值
        std::vector<Value> defaults;
        if (!fillDefaultArgs(initChunk, argCount, funName, defaults)) {
            return runtimeError(ErrorFormat::formatStd("构造函数 init 期望 {}-{} 个参数，但传入了 {} 个",

                                                       initChunk.requiredArity, initChunk.arity,

                                                       static_cast<int>(argCount)),
                                DiagCodes::kArityMismatch);
        }
        // 将默认参数追加到 args 末尾
        for (auto& d : defaults) {
            args.push_back(std::move(d));
        }

        if (frames_.size() >= MAX_FRAMES) {
            // R97 #11 fix: 三后端递归深度消息统一为"递归深度超过限制 (N)"
            return runtimeError(
                ErrorFormat::formatStd(ErrorMessages::kRecursionDepthExceededFmtStd, static_cast<int>(MAX_FRAMES)),
                DiagCodes::kRecursionDepth);
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
        int preAllocated = 1 + fieldCount + argCount; // this + 字段 + 参数
        int extraSlots = initChunk.localCount - preAllocated;
        // V-P2-1 fix: extraSlots 为负表示帧布局损坏（fieldCount 与编译期不一致）
        if (extraSlots < 0) {
            // AUDIT-P1 fix: 错误返回前清理栈上已推入的 this+fields+args，
            // 与 executeCall OP_CALL 普通路径（BUG-VM-02 fix）保持栈平衡。
            // args 已含默认值追加，用 args.size() 反映栈上实际参数数。
            popN(1 + fieldCount + static_cast<int>(args.size()));
            return runtimeError(ErrorFormat::formatStd("类 {} 的 init 方法帧布局损坏: localCount={} < preAllocated={}",

                                                       funName, initChunk.localCount, preAllocated));
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
        newFrame.isMethodCall = true; // 使 OP_RETURN 同步字段到 this
        newFrame.isInitCall = true;   // init 返回 this 而非 null
        size_t savedIp = ip;
        frames_.push_back(std::move(newFrame));

        notifyStep(savedIp, op);
        return VMResult::VM_OK;
    }

    // 无 init 方法：检查是否有多余参数（与解释器行为保持一致）
    if (argCount > 0) {
        return runtimeError(ErrorFormat::formatStd("类 {} 没有 init 方法，但传入了 {} 个参数", cls.name,

                                                   static_cast<int>(argCount)),
                            DiagCodes::kArityMismatch);
    }
    push(instance);
    notifyStep(ip, op);
    ip += 4;
    return VMResult::VM_OK;
}

// ============================================================
// executeCallBuiltinInput - input() 内置函数
// OP_CALL 路径 funName=="input"：调用 executeSharedInput
// ============================================================
VMResult VM::executeCallBuiltinInput(size_t& ip, OpCode op, uint8_t argCount, const BytecodeChunk& chunk) {
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

    // E3 fix: 改用共享层 executeSharedInput，统一与 Interpreter 的 input() 语义。
    // WorkerManager 超时回调会抛 std::runtime_error，被 executeSharedInput
    // 捕获并返回 Result::err，此处转为 runtimeError 上报，避免静默返回空串。
    auto r = executeSharedInput(inputCallback_, args.begin(), argCount, line, 0);
    if (r.is_err()) {
        return runtimeError(r.error().message);
    }
    push(std::move(r.value()));
    notifyStep(ip, op);
    ip += 4;
    return VMResult::VM_OK;
}

// ============================================================
// executeCallHigherOrder - 高阶函数
// OP_CALL 路径 isHigherOrderBuiltin 命中：map/filter/reduce/forEach/find 分派
// ============================================================
VMResult VM::executeCallHigherOrder(size_t& ip, OpCode op, const std::string& funName, uint8_t argCount,
                                    const BytecodeChunk& chunk) {
    // 保存 ip 值——invokeClosureSync 会 push/pop frames，可能导致
    // frames_ 重分配使 ip 引用悬垂。后续用 currentFrame().ip 直接写入。
    size_t savedIp = ip;
    // 收集参数（栈上顺序: [arg0, arg1, ..., argN-1]，栈顶是最后一个参数）
    if (stack_.size() < static_cast<size_t>(argCount)) {
        return runtimeError("栈下溢: OP_CALL higher-order");
    }
    SmallArgs<Value> args(argCount);
    for (int i = argCount - 1; i >= 0; --i) {
        args[i] = pop();
    }
    // 构造闭包调用回调
    int hoLine = 0;
    if (!chunk.lines.empty() && savedIp < chunk.lines.size()) {
        hoLine = chunk.lines[savedIp];
    }
    ClosureInvoker invoke = [this, hoLine](const Value& closure, const Value* a, size_t ac, int /*ln*/,
                                           int /*col*/) -> Result<Value> {
        Value res;
        VMResult r = invokeClosureSync(closure, a, ac, hoLine, 0, res);
        if (r != VMResult::VM_OK) {
            // P2-12: 从 diagnostics_ 派生错误消息（替代已移除的 lastError_）
            return Result<Value>::err(getLastError(), hoLine, 0);
        }
        return Result<Value>::ok(std::move(res));
    };
    // 按函数名分派到共享算法层
    Result<Value> hoResult = Result<Value>::ok(Value::nullValue());
    if (funName == "map") {
        if (argCount != 2) {
            return runtimeError(ErrorFormat::formatStd("map 期望 2 个参数，但传入了 {} 个", argCount),
                                DiagCodes::kArityMismatch);
        }
        hoResult = executeSharedMap(args[0], args[1], invoke, hoLine, 0);
    } else if (funName == "filter") {
        if (argCount != 2) {
            return runtimeError(ErrorFormat::formatStd("filter 期望 2 个参数，但传入了 {} 个", argCount),
                                DiagCodes::kArityMismatch);
        }
        hoResult = executeSharedFilter(args[0], args[1], invoke, hoLine, 0);
    } else if (funName == "reduce") {
        if (argCount != 3) {
            return runtimeError(ErrorFormat::formatStd("reduce 期望 3 个参数，但传入了 {} 个", argCount),
                                DiagCodes::kArityMismatch);
        }
        hoResult = executeSharedReduce(args[0], args[1], args[2], invoke, hoLine, 0);
    } else if (funName == "forEach") {
        if (argCount != 2) {
            return runtimeError(ErrorFormat::formatStd("forEach 期望 2 个参数，但传入了 {} 个", argCount),
                                DiagCodes::kArityMismatch);
        }
        hoResult = executeSharedForEach(args[0], args[1], invoke, hoLine, 0);
    } else if (funName == "find") {
        if (argCount != 2) {
            return runtimeError(ErrorFormat::formatStd("find 期望 2 个参数，但传入了 {} 个", argCount),
                                DiagCodes::kArityMismatch);
        }
        hoResult = executeSharedFind(args[0], args[1], invoke, hoLine, 0);
    }
    if (hoResult.is_err()) {
        return runtimeError(hoResult.error().message);
    }
    push(std::move(hoResult.value()));
    // ip 引用可能因 invokeClosureSync 内部 frames_ 操作而悬垂，
    // 用 currentFrame().ip 直接写入（currentFrame() 重新获取 frames_.back()）
    notifyStep(savedIp, op);
    currentFrame().ip = savedIp + 4;
    return VMResult::VM_OK;
}

// ============================================================
// executeCallSpawn - spawn(fn, args...) 内置函数
// OP_CALL 路径 funName=="spawn"：构造 ClosureInvoker + executeSharedSpawn
// ============================================================
VMResult VM::executeCallSpawn(size_t& ip, OpCode op, uint8_t argCount, const BytecodeChunk& chunk) {
    if (argCount < 1) {
        // popN 保证栈平衡
        popN(argCount);
        return runtimeError("spawn 期望至少 1 个参数（函数），但传入了 0 个", DiagCodes::kArityMismatch);
    }
    size_t savedIp = ip;
    if (stack_.size() < static_cast<size_t>(argCount)) {
        return runtimeError("栈下溢: OP_CALL spawn");
    }
    SmallArgs<Value> args(argCount);
    for (int i = argCount - 1; i >= 0; --i) {
        args[i] = pop();
    }
    int spawnLine = 0;
    if (!chunk.lines.empty() && savedIp < chunk.lines.size()) {
        spawnLine = chunk.lines[savedIp];
    }
    // 构造闭包调用回调：通过 spawnMutex_ 序列化，避免栈/帧数据竞争
    ClosureInvoker invoke = [this, spawnLine](const Value& closure, const Value* a, size_t ac, int /*ln*/,
                                              int /*col*/) -> Result<Value> {
        std::lock_guard<std::mutex> lock(spawnMutex_);
        Value res;
        VMResult r = invokeClosureSync(closure, a, ac, spawnLine, 0, res);
        // P3-A1: VM_EXCEPTION_THROW 不是错误——闭包内 throw 已被外层 try/catch 捕获，
        // throwException 已就位 catchIp + 栈顶 thrownValue + 弹空 tryStack_ handler。
        // 若当作错误抛 RuntimeError，会破坏已就位的 catch 状态（覆盖 catchIp、栈被掏空）。
        // 返回 ok(null) 让 handleSyncObjectMethod 不抛、dispatchSyncObjectBuiltin 通过
        // tryStack_ 缩小检测返回 VM_EXCEPTION_THROW 让主循环继续 catch 块。
        if (r == VMResult::VM_EXCEPTION_THROW) {
            return Result<Value>::ok(Value::nullValue());
        }
        if (r != VMResult::VM_OK) {
            // P2-12: 从 diagnostics_ 派生错误消息（替代已移除的 lastError_）
            return Result<Value>::err(getLastError(), spawnLine, 0);
        }
        return Result<Value>::ok(std::move(res));
    };
    auto r = executeSharedSpawn(args[0], args.begin() + 1, argCount - 1, invoke, spawnLine, 0);
    if (r.is_err()) {
        return runtimeError(r.error().message);
    }
    push(std::move(r.value()));
    notifyStep(savedIp, op);
    currentFrame().ip = savedIp + 4;
    return VMResult::VM_OK;
}

// ============================================================
// executeCallBuiltinFunction - 内置函数
// OP_CALL 路径 isBuiltinFunction 命中：调用 executeSharedBuiltinFunction
// ============================================================
VMResult VM::executeCallBuiltinFunction(size_t& ip, OpCode op, const std::string& funName, uint8_t argCount,
                                        const BytecodeChunk& chunk) {
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

    auto r = executeSharedBuiltinFunction(funName, args.begin(), argCount, line, 0);

    if (r.is_err()) {
        return runtimeError(r.error().message);
    }
    push(std::move(r.value()));
    notifyStep(ip, op);
    ip += 4;
    return VMResult::VM_OK;
}

// ============================================================
// executeCallFunction - 普通函数调用
// OP_CALL 路径 functionChunks_ 命中：默认参数范围检查 + 准备 upvalues + 调用 setupFunctionCallFrame
// ============================================================
VMResult VM::executeCallFunction(size_t& ip, OpCode op, const std::string& funName, uint8_t argCount,
                                 const BytecodeChunk& targetChunk) {
    // F10: 支持默认参数，参数数量可在 [requiredArity, arity] 范围内
    // BUG-VM-02 fix: 错误返回前 popN(argCount) 清理栈上参数，与 OP_CALL_EXPR 路径一致
    if (argCount < static_cast<uint8_t>(targetChunk.requiredArity) ||
        argCount > static_cast<uint8_t>(targetChunk.arity)) {
        popN(argCount);
        return runtimeError(ErrorFormat::formatStd("函数 {} 期望 {}-{} 个参数，但传入了 {} 个", funName,

                                                   targetChunk.requiredArity, targetChunk.arity,

                                                   static_cast<int>(argCount)),
                            DiagCodes::kArityMismatch);
    }

    // R164 D.5: 生成器函数拦截——不直接调用，创建协程值返回调用方。
    // OP_CALL 是 4 字节指令。
    if (targetChunk.isGenerator) {
        Value closureVal;
        auto closureIt = functionClosures_.find(funName);
        if (closureIt != functionClosures_.end()) {
            closureVal = closureIt->second;
        }
        return createCoroutineValue(targetChunk, funName, argCount, std::move(closureVal), ip, 4);
    }

    // VM-05/06: 从闭包注册表附加 upvalues（如果该函数有闭包绑定）
    std::vector<std::shared_ptr<VMUpvalue>> upvalues;
    auto closureIt = functionClosures_.find(funName);
    if (closureIt != functionClosures_.end() && closureIt->second.vmClosure()) {
        upvalues = closureIt->second.vmClosure()->upvalues;
    }

    // OP_CALL 是 4 字节指令：opcode + idx(2B) + argCount(1B)
    return setupFunctionCallFrame(targetChunk, funName, argCount, ip + 4, ip, op, upvalues);
}

// ============================================================
// executeCallExprValue - 闭包值调用（OP_CALL_EXPR 路径）
// 栈布局调整 + chunkPtr 获取 + 调用 setupFunctionCallFrame
// ============================================================
VMResult VM::executeCallExprValue(size_t& ip, OpCode op) {
    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;

    uint8_t argCount = chunk.code[ip + 1];
    // VM-05/06: 从栈上获取闭包值和参数，执行调用
    if (stack_.size() < static_cast<size_t>(argCount) + 1) {
        return runtimeError("栈下溢: OP_CALL_EXPR");
    }

    // 编译器先 push 闭包值再 push 参数（Compiler.cpp:1150-1158, IR.cpp visitFunCall），
    // 实际栈布局: [..., closure, arg0, arg1, ..., argN-1]
    // 需从栈中移除闭包值（在参数下方），保留参数在栈顶供新帧使用。
    size_t closurePos = stack_.size() - argCount - 1;
    Value callee = std::move(stack_[closurePos]);
    for (size_t i = 0; i < static_cast<size_t>(argCount); ++i) {
        stack_[closurePos + i] = std::move(stack_[closurePos + 1 + i]);
    }
    stack_.pop_back();

    if (!callee.isClosure()) {
        // R3-1 fix: 弹出参数以保持栈平衡
        popN(argCount);
        return runtimeError("表达式调用需要函数值", DiagCodes::kTypeMismatch);
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
        popN(argCount);
        return runtimeError("未找到函数: " + callee.closureName(), "undefined-function");
    }

    const BytecodeChunk& targetChunk = *targetChunkPtr;
    // F10: 支持默认参数
    if (argCount < static_cast<uint8_t>(targetChunk.requiredArity) ||
        argCount > static_cast<uint8_t>(targetChunk.arity)) {
        // R3-1 fix: 弹出参数保持栈平衡
        popN(argCount);
        return runtimeError(ErrorFormat::formatStd("函数 {} 期望 {}-{} 个参数，但传入了 {} 个",

                                                   callee.closureName(), targetChunk.requiredArity,

                                                   targetChunk.arity, static_cast<int>(argCount)),
                            DiagCodes::kArityMismatch);
    }

    // R164 D.5: 生成器函数拦截——闭包值调用路径。
    // OP_CALL_EXPR 是 2 字节指令。
    if (targetChunk.isGenerator) {
        // D.7 fix: C++ 参数求值顺序不定——先复制 closureName 到局部变量，避免 std::move(callee)
        // 先求值导致 callee.box_=null，后续 callee.closureName() 触发 std::abort()。
        std::string closureNameCopy = callee.closureName();
        return createCoroutineValue(targetChunk, closureNameCopy, argCount, std::move(callee), ip, 2);
    }

    // VM-05/06: 绑定闭包 upvalues 到新帧
    std::vector<std::shared_ptr<VMUpvalue>> upvalues;
    if (callee.vmClosure()) {
        upvalues = callee.vmClosure()->upvalues;
    }

    // OP_CALL_EXPR 是 2 字节指令：opcode + argCount(1B)
    return setupFunctionCallFrame(targetChunk, callee.closureName(), argCount, ip + 2, ip, op, upvalues);
}

// ============================================================
// setupFunctionCallFrame - 共享帧构造 helper
// 默认参数填充 + MAX_FRAMES 检查 + extraSlots 预分配 + newFrame 构造 + push frame
// 被 executeCallFunction 和 executeCallExprValue 共享，统一两路径的帧构造逻辑
// 注：R123 修复了原两路径差异——错误消息文本"闭包调用帧布局损坏"vs"函数调用帧布局损坏"统一为后者，
// pop 方式 popN(argCount) vs for 循环 pop 统一为 popN（更高效）
// ============================================================
VMResult VM::setupFunctionCallFrame(const BytecodeChunk& targetChunk, const std::string& functionName,
                                    uint8_t& argCount, size_t returnIp, size_t savedIp, OpCode op,
                                    const std::vector<std::shared_ptr<VMUpvalue>>& upvalues) {
    // F10: 为缺失的尾部参数填充默认值
    if (argCount < static_cast<uint8_t>(targetChunk.arity)) {
        int missingCount = targetChunk.arity - argCount;
        int defaultStartIdx = static_cast<int>(targetChunk.defaultConstIndices.size()) - missingCount;
        if (defaultStartIdx < 0 ||
            static_cast<size_t>(defaultStartIdx + missingCount) > targetChunk.defaultConstIndices.size()) {
            popN(argCount);
            return runtimeError("函数 " + functionName + " 默认参数索引越界");
        }
        // BUG-023 fix (2026-07-18): 错误路径必须弹出已 push 的默认参数。
        // 原 popN(argCount) 只清理原始参数，循环中已 push 的 (i-defaultStartIdx)
        // 个默认参数残留在栈上，导致栈不平衡。修复：错误路径弹出
        // argCount + pushedDefaults 个，其中 pushedDefaults = i - defaultStartIdx。
        for (int i = defaultStartIdx; i < defaultStartIdx + missingCount; ++i) {
            uint16_t constIdx = targetChunk.defaultConstIndices[i];
            if (constIdx == RuntimeLimits::NO_INDEX) {
                int pushedDefaults = i - defaultStartIdx;
                popN(static_cast<size_t>(argCount) + static_cast<size_t>(pushedDefaults));
                return runtimeError("函数 " + functionName + " 的默认参数包含非字面量表达式，VM 不支持");
            }
            if (constIdx >= targetChunk.constants.size()) {
                int pushedDefaults = i - defaultStartIdx;
                popN(static_cast<size_t>(argCount) + static_cast<size_t>(pushedDefaults));
                return runtimeError("函数 " + functionName + " 默认参数常量索引越界");
            }
            push(targetChunk.constants[constIdx]);
        }
        argCount = static_cast<uint8_t>(targetChunk.arity);
    }

    if (frames_.size() >= MAX_FRAMES) {
        popN(argCount);
        // R97 #11 fix: 三后端递归深度消息统一
        return runtimeError(
            ErrorFormat::formatStd(ErrorMessages::kRecursionDepthExceededFmtStd, static_cast<int>(MAX_FRAMES)),
            DiagCodes::kRecursionDepth);
    }

    // 预分配局部变量栈空间：函数体内 var 声明的局部变量需要栈槽，
    // 但帧创建时栈上只有参数，需补推 null 填充额外槽位
    int extraSlots = targetChunk.localCount - argCount;
    // V-P2-1 fix: extraSlots 为负表示帧布局损坏
    if (extraSlots < 0) {
        popN(argCount);
        return runtimeError(ErrorFormat::formatStd("函数调用帧布局损坏: localCount={} < argCount={}",

                                                   targetChunk.localCount, static_cast<int>(argCount)));
    }
    for (int i = 0; i < extraSlots; ++i) {
        push(Value::nullValue());
    }

    VMCallFrame newFrame;
    newFrame.chunk = &targetChunk;
    newFrame.returnIp = returnIp;
    newFrame.basePointer = stack_.size() - targetChunk.localCount;
    newFrame.functionName = functionName;
    newFrame.ip = 0;
    newFrame.upvalues = upvalues; // 拷贝

    frames_.push_back(std::move(newFrame));
    notifyStep(savedIp, op);
    return VMResult::VM_OK;
}

VMResult VM::executeMethodCall(size_t& ip, OpCode op) {
    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;
    bool isSuperCall = (op == OpCode::OP_SUPER_CALL);
    const int instrLen = isSuperCall ? 9 : 7; // B1 fix: SUPER_CALL 多了 2 字节 classIdx
    uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
    uint8_t argCount = chunk.code[ip + 3];
    uint16_t receiverVarIdx = chunk.code[ip + 4] | (chunk.code[ip + 5] << 8);
    uint8_t receiverLocalSlotByte = chunk.code[ip + 6];
    if (idx >= chunk.constants.size())
        return runtimeError("常量池索引越界");
    const std::string& methodName = chunk.constants[idx].stringVal();

    // C8: 用 const 引用访问接收者，避免非变异方法的深拷贝
    const Value& obj = peek(argCount);
    // V-P2-25 fix: peek 栈下溢时已设置 hasError_ 并返回 null 哨兵，立即退出避免后续误报
    if (hasError_)
        return VMResult::VM_RUNTIME_ERROR;
    // C10: 方法名一次性分类为枚举
    BuiltinMethod method = classifyBuiltinMethod(methodName);

    // ---- 数组内置方法 ----
    if (obj.isArray()) {
        // B7 fix: 提取到 dispatchArrayBuiltin，降低 executeCallOps 圈复杂度
        VMResult r = dispatchArrayBuiltin(obj, method, methodName, argCount, receiverVarIdx, receiverLocalSlotByte, ip,
                                          op, instrLen);
        if (r != VMResult::VM_OK)
            return r;
        return VMResult::VM_OK;
    }

    // ---- 字典内置方法 ----
    if (obj.isDict()) {
        VMResult r = dispatchDictBuiltin(obj, method, methodName, argCount, receiverVarIdx, receiverLocalSlotByte, ip,
                                         op, instrLen);
        if (r != VMResult::VM_OK)
            return r;
        return VMResult::VM_OK;
    }

    // ---- 字符串内置方法（全部非变异，使用 const 引用）----
    if (obj.isString()) {
        VMResult r = dispatchStringBuiltin(obj, method, methodName, argCount, ip, op, instrLen);
        if (r != VMResult::VM_OK)
            return r;
        return VMResult::VM_OK;
    }

    // ---- R136 同步对象方法（channel/mutex/rwlock/thread）----
    // 同步对象内部状态通过 shared_ptr<Inner> 共享，方法调用不修改 Value 本身，无需 writeBack
    if (obj.isChannel() || obj.isMutex() || obj.isRwLock() || obj.isThread()) {
        VMResult r = dispatchSyncObjectBuiltin(obj, methodName, argCount, ip, op, instrLen);
        if (r != VMResult::VM_OK)
            return r;
        return VMResult::VM_OK;
    }

    // ---- R164 协程/生成器方法（.next() / .done()）----
    // 协程内部状态通过 CoroutineData* 共享指针修改，无需 writeBack（与同步对象语义一致）
    if (obj.isCoroutine()) {
        // C8: 传入 obj 的副本到 helper（值传递），避免 helper 内 pop() 后 peek 引用失效
        Value coroCopy = obj;
        return dispatchCoroutineBuiltin(coroCopy, methodName, argCount, ip, op, instrLen);
    }

    // ---- 类实例方法调用 ----
    if (obj.isInstance()) {
        // C8: 传入 obj 的副本到 helper（值传递），避免 helper 内 pop() 后 peek 引用失效
        return executeInstanceMethodCall(ip, op, obj, methodName, argCount, receiverVarIdx, receiverLocalSlotByte);
    }

    // 方法未找到或对象非实例
    // 先保存类型信息，因为 pop 会使 obj 引用失效
    bool wasInstance = obj.isInstance();
    // P2 fix (null-access): 捕获 null 标志，供下方发出 null-access 诊断码
    bool wasNull = obj.isNull();
    std::string clsName = wasInstance ? obj.className() : "";
    // 2026-06-29 BUG-1 fix (与 RegisterVM 对齐): 非 array/dict/string/instance
    // 类型（null/int/float/bool/closure）调用方法时，原消息"方法调用需要类实例"
    // 具有误导性。改为与 RegisterVM/Interpreter 一致的"类型 X 不支持方法 Y"。
    std::string typeName = wasInstance ? "" : obj.typeName();
    for (uint8_t i = 0; i < argCount; ++i)
        pop();
    pop();
    if (wasInstance) {
        return runtimeError("类 " + clsName + " 没有方法 " + methodName, "undefined-function");
    } else if (wasNull) {
        // P2 fix (null-access): null 值方法调用给出明确的 null-access 诊断码
        return runtimeError("不能在 null 值上访问属性或调用方法", DiagCodes::kNullAccess);
    } else {
        return runtimeError("类型 " + typeName + " 不支持方法 " + methodName, "undefined-function");
    }
}

// ============================================================
// executeInstanceMethodCall - 类实例方法调用（提取自 executeMethodCall 的 instance 分支）
// 含 super 调用解析、继承链查找、默认参数填充、字段槽位预填、局部变量预分配、帧构造
// ============================================================
VMResult VM::executeInstanceMethodCall(size_t& ip, OpCode op, Value obj, const std::string& methodName,
                                       uint8_t argCount, uint16_t receiverVarIdx, uint8_t receiverLocalSlotByte) {
    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;
    bool isSuperCall = (op == OpCode::OP_SUPER_CALL);
    const int instrLen = isSuperCall ? 9 : 7; // B1 fix: SUPER_CALL 多了 2 字节 classIdx

    // obj 已是值传递副本，无需再次拷贝（原代码 const Value objCopy = obj 已不再需要）
    // V-P2-12 fix: 使用 const 避免后续 fields()/className() 触发 COW 深拷贝
    const Value& objCopy = obj;
    // B1 fix: super 调用使用编译时编码的类名（而非运行时实例类名）
    // 避免 3+ 级继承时 super 查找回到子类导致死循环
    std::string searchClassName = objCopy.className();
    if (isSuperCall) {
        uint16_t classIdx = chunk.code[ip + 7] | (chunk.code[ip + 8] << 8);
        // V-P1-7 fix: classIdx 越界应报错而非静默降级到运行时类名（可能导致错误的 super 查找）
        if (classIdx >= chunk.constants.size()) {
            return runtimeError(
                ErrorFormat::formatStd("内部错误: super 调用的类名常量索引越界 ({})", static_cast<int>(classIdx)));
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
    if (targetChunkPtr == nullptr) {
        // 方法未找到：pop 参数和接收者，返回错误（与原 executeMethodCall 末尾错误路径一致）
        popN(argCount + 1);
        return runtimeError("类 " + objCopy.className() + " 没有方法 " + methodName, "undefined-function");
    }
    const BytecodeChunk& targetChunk = *targetChunkPtr;

    // F10: 支持默认参数
    if (argCount < static_cast<uint8_t>(targetChunk.requiredArity) ||
        argCount > static_cast<uint8_t>(targetChunk.arity)) {
        // PERF-12 fix: 批量 pop 用 popN（参数 + 接收者）
        popN(argCount + 1);
        return runtimeError(ErrorFormat::formatStd("方法 {} 期望 {}-{} 个参数，但传入了 {} 个", methodName,

                                                   targetChunk.requiredArity, targetChunk.arity,

                                                   static_cast<int>(argCount)),
                            DiagCodes::kArityMismatch);
    }

    if (frames_.size() >= MAX_FRAMES) {
        // BUG-VM-03 fix: 错误返回前 popN(argCount + 1) 清理栈上参数 + 接收者
        popN(argCount + 1);
        // R97 #11 fix: 三后端递归深度消息统一
        return runtimeError(
            ErrorFormat::formatStd(ErrorMessages::kRecursionDepthExceededFmtStd, static_cast<int>(MAX_FRAMES)),
            DiagCodes::kRecursionDepth);
    }

    // 收集参数（反向填充，省去 reverse）
    if (stack_.size() < static_cast<size_t>(argCount) + 1)
        return runtimeError("栈下溢: OP_METHOD_CALL");
    SmallArgs<Value> args(argCount);
    for (int i = argCount - 1; i >= 0; --i) {
        args[i] = pop();
    }
    pop(); // 移除栈上的原始实例

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
            // AUDIT-P1 fix: 错误返回前清理栈上已推入的 this+fields+args，
            // 与 executeCall OP_CALL 普通路径（BUG-VM-02 fix）保持栈平衡。
            // 此处 argCount 是原始传入参数数（尚未被默认值追加），fieldCount 已在上方计算。
            popN(1 + fieldCount + argCount);
            return runtimeError("方法 " + methodName + " 默认参数索引越界");
        }
        // BUG-023 fix (2026-07-18): 错误路径必须弹出已 push 的默认参数（见 OP_CALL 路径注释）
        for (int i = defaultStartIdx; i < defaultStartIdx + missingCount; ++i) {
            uint16_t constIdx = targetChunk.defaultConstIndices[i];
            if (constIdx == RuntimeLimits::NO_INDEX) {
                int pushedDefaults = i - defaultStartIdx;
                popN(static_cast<size_t>(1 + fieldCount + argCount) + static_cast<size_t>(pushedDefaults));
                return runtimeError("方法 " + methodName + " 的默认参数包含非字面量表达式，VM 不支持");
            }
            if (constIdx >= targetChunk.constants.size()) {
                int pushedDefaults = i - defaultStartIdx;
                popN(static_cast<size_t>(1 + fieldCount + argCount) + static_cast<size_t>(pushedDefaults));
                return runtimeError("方法 " + methodName + " 默认参数常量索引越界");
            }
            push(targetChunk.constants[constIdx]);
        }
        argCount = static_cast<uint8_t>(targetChunk.arity);
    }

    // 预分配局部变量栈空间：方法体内 var 声明的局部变量需要栈槽
    int preAllocated = 1 + fieldCount + argCount; // this + 字段 + 参数
    int extraSlots = targetChunk.localCount - preAllocated;
    // V-P2-1 fix: extraSlots 为负表示帧布局损坏
    if (extraSlots < 0) {
        // AUDIT-P3-ROUND50 fix: 错误路径未清理栈上已推入的 this + fields + args，
        // 与同函数其他错误路径（L705/L714/L759/L765/L769 的 popN）不一致。
        popN(1 + fieldCount + argCount);
        return runtimeError(ErrorFormat::formatStd("方法 {} 帧布局损坏: localCount={} < preAllocated={}",

                                                   methodName, targetChunk.localCount, preAllocated));
    }
    for (int i = 0; i < extraSlots; ++i) {
        push(Value::nullValue());
    }

    VMCallFrame newFrame;
    newFrame.chunk = targetChunkPtr;
    newFrame.returnIp = ip + instrLen; // B1 fix: SUPER_CALL 是 9 字节
    newFrame.basePointer = stack_.size() - targetChunk.localCount;
    newFrame.functionName = targetChunk.name;
    newFrame.ip = 0;
    newFrame.isMethodCall = true;
    newFrame.isInitCall = (methodName == "init"); // init 返回 this 而非 null
    // 记录接收者变量名（用于 writeBack 到 globals_）
    if (receiverVarIdx != RuntimeLimits::NO_INDEX && receiverVarIdx < chunk.constants.size() &&
        chunk.constants[receiverVarIdx].isString()) {
        newFrame.receiverVarName = chunk.constants[receiverVarIdx].stringVal();
    }
    // 记录接收者局部变量 slot（用于 writeBack 到调用者栈帧）
    newFrame.receiverLocalSlot = (receiverLocalSlotByte == RuntimeLimits::NO_SLOT) ? -1 : receiverLocalSlotByte;
    // W3-2-Bug2 fix: 若方法有捕获的 upvalue（函数内定义的类），填充帧的 upvalue 列表。
    // methodUpvalues_ 在 OP_DEFINE_CLASS 时为有 upvalue 描述符的方法创建 VMClosureData 捕获。
    auto upvIt = methodUpvalues_.find(targetChunk.name);
    if (upvIt != methodUpvalues_.end() && upvIt->second) {
        newFrame.upvalues = upvIt->second->upvalues;
    }
    size_t savedIp = ip;
    frames_.push_back(std::move(newFrame));

    notifyStep(savedIp, op);
    return VMResult::VM_OK;
}

VMResult VM::executeClosure(size_t& ip, OpCode op) {
    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;
    uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
    uint8_t upvalueCount = chunk.code[ip + 3]; // VM-05/06: 改为 upvalue 数量
    if (idx >= chunk.constants.size())
        return runtimeError("常量池索引越界");
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
    pendingFieldOrder_.clear(); // M3: 开始新的类定义，清空字段顺序记录
    uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
    uint8_t argCount = chunk.code[ip + 3];
    if (idx >= chunk.constants.size())
        return runtimeError("常量池索引越界");
    const std::string& className = chunk.constants[idx].stringVal();

    // 收集参数（反向填充，省去 reverse）
    if (stack_.size() < argCount)
        return runtimeError("栈下溢: OP_CLASS_NEW");
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
            return runtimeError("类 " + className + " 尚未定义，不能带参数构造", "undefined-function");
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
    instance.fields() = cls.fieldDefaults; // 已含继承字段

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
            shouldCallInit = true; // 0 必需参数（含全默认参数 init），自动构造时调用
        }
    }

    if (shouldCallInit) {
        // 创建 init 帧执行初始化
        const BytecodeChunk& initChunk = *initChunkPtr;
        // P0-1 fix: 使用范围检查支持默认参数，并填充缺失的默认值
        std::vector<Value> defaults;
        if (!fillDefaultArgs(initChunk, argCount, className, defaults)) {
            return runtimeError(ErrorFormat::formatStd("构造函数 init 期望 {}-{} 个参数，但传入了 {} 个",

                                                       initChunk.requiredArity, initChunk.arity,

                                                       static_cast<int>(argCount)),
                                DiagCodes::kArityMismatch);
        }
        // 将默认参数追加到 args 末尾
        for (auto& d : defaults) {
            args.push_back(std::move(d));
        }

        if (frames_.size() >= MAX_FRAMES) {
            // R97 #11 fix: 三后端递归深度消息统一
            return runtimeError(
                ErrorFormat::formatStd(ErrorMessages::kRecursionDepthExceededFmtStd, static_cast<int>(MAX_FRAMES)),
                DiagCodes::kRecursionDepth);
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
        int preAllocated = 1 + fieldCount + argCount; // this + 字段 + 参数
        int extraSlots = initChunk.localCount - preAllocated;
        // V-P2-1 fix: extraSlots 为负表示帧布局损坏
        if (extraSlots < 0) {
            return runtimeError(ErrorFormat::formatStd("类 {} 的 init 方法帧布局损坏: localCount={} < preAllocated={}",

                                                       className, initChunk.localCount, preAllocated));
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
        newFrame.isMethodCall = true; // 使 OP_RETURN 同步字段到 this
        newFrame.isInitCall = true;   // init 返回 this 而非 null
        size_t savedIp = ip;
        frames_.push_back(std::move(newFrame));

        notifyStep(savedIp, op);
        return VMResult::VM_OK;
    }

    // 无 init 或 init.arity 不匹配 argCount：推入实例（OP_INIT_FIELD 或手动 init 后续处理）
    // V-P1-8 fix: init 存在但参数不匹配（argCount==0 且 init.requiredArity>0）时报错，而非静默跳过
    if (initChunkPtr != nullptr && !shouldCallInit) {
        return runtimeError(ErrorFormat::formatStd("类 {} 的 init 期望至少 {} 个参数，但传入了 {} 个", cls.name,

                                                   initChunkPtr->requiredArity, static_cast<int>(argCount)),
                            DiagCodes::kArityMismatch);
    }

    // 无 init 但有参数：报错（与解释器一致）
    // BUG-VM-01 fix: 检查必须在 push(instance) 之前，错误返回时栈上不残留 instance。
    // 与同文件 executeCall 类构造路径（L327-336 检查在 push 之前）顺序一致。
    if (initChunkPtr == nullptr && argCount > 0) {
        return runtimeError(ErrorFormat::formatStd("类 {} 没有 init 方法，但传入了 {} 个参数", cls.name,

                                                   static_cast<int>(argCount)),
                            DiagCodes::kArityMismatch);
    }

    push(instance);

    notifyStep(ip, op);
    ip += 4;
    return VMResult::VM_OK;
}

VMResult VM::executeDefineClass(size_t& ip, OpCode op) {
    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;
    // 操作数: nameIdx(2B) + superNameIdx(2B)
    // superNameIdx == RuntimeLimits::NO_INDEX 表示无父类
    uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
    uint16_t superIdx = chunk.code[ip + 3] | (chunk.code[ip + 4] << 8);
    if (idx >= chunk.constants.size())
        return runtimeError("常量池索引越界");
    const std::string& className = chunk.constants[idx].stringVal();
    std::string superClassName;
    if (superIdx != RuntimeLimits::NO_INDEX && superIdx < chunk.constants.size()) {
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

    // BUG-INH-AUDIT-7 fix: 父类重定义时，所有子类的 fieldOrder/fieldDefaults/methodCache
    // 缓存失效（原实现仅更新当前类，子类仍用旧父类字段布局）。
    // 遍历所有已注册类，若其继承链包含当前重定义的类，清空其缓存。
    // methodCache 是 mutable 需单独清空，fieldOrder/fieldDefaults 在下次构造时
    // 由 executeClassNew 重新合并（实际上 executeDefineClass 已合并，这里清空
    // 子类缓存的 fieldOrder/fieldDefaults 强制下次重定义时重新计算）。
    // 注意：单次运行（单文件编译执行）下，类定义按声明顺序处理，子类总在父类
    // 之后定义，此循环通常是空操作。此修复主要针对 REPL 场景下的重定义。
    for (auto& kv : classInfo_) {
        if (kv.first == className)
            continue; // 跳过当前类
        // 沿继承链查找是否依赖当前重定义的类
        std::string cur = kv.second.superClassName;
        for (int guard = 0; guard < 64 && !cur.empty(); ++guard) {
            if (cur == className) {
                // 子类 kv.first 依赖重定义的父类 className，清空缓存
                kv.second.fieldOrder.clear();
                kv.second.fieldDefaults.clear();
                kv.second.methodCache.clear();
                break;
            }
            auto it = classInfo_.find(cur);
            if (it == classInfo_.end())
                break;
            cur = it->second.superClassName;
        }
    }

    // 沿继承链合并父类字段（父类字段在前，子类覆盖同名字段）
    // 先按"父→子"顺序收集字段名，子类已存在的字段不重复添加
    std::vector<std::string> mergedOrder;
    std::unordered_map<std::string, Value> mergedDefaults;

    std::vector<std::string> chain;
    std::string cur = superClassName;
    for (int guard = 0; guard < MAX_INHERITANCE_DEPTH && !cur.empty(); ++guard) {
        auto clsIt = classInfo_.find(cur);
        if (clsIt == classInfo_.end()) {
            return runtimeError("未定义的父类: " + cur, "undefined-function");
        }
        chain.push_back(cur);
        cur = clsIt->second.superClassName;
    }
    // V-P2 fix: 循环继承检测 — guard 耗尽但 cur 仍非空说明存在继承环
    if (!cur.empty()) {
        return runtimeError("类继承链过深或存在循环继承: " + className + " -> " + cur);
    }
    // AUDIT-BUG-C3 fix: 正序遍历链（直接父类在前，根祖先在后），
    // 配合 "if not found then add" 去重——直接父类同名字段先入表，根祖先被跳过。
    // 原倒序遍历导致根祖先先入表，直接父类同名字段被跳过，三后端不一致。
    // 与 Interpreter InterpreterCalls.cpp:317-337 语义对齐。
    for (const auto& clsName : chain) {
        auto clsIt = classInfo_.find(clsName);
        if (clsIt == classInfo_.end())
            continue;
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

    // W3-2-Bug2 fix: 为有 upvalue 的方法创建 upvalue 捕获。
    // 类定义在外层函数内时，方法可能引用外层函数的局部变量（upvalue）。
    // 此时仍在定义类的外层函数帧中，可以正确捕获栈槽/upvalue。
    // 方法调用时从 methodUpvalues_ 读取并填充方法帧的 upvalues。
    captureMethodUpvalues(className);

    notifyStep(ip, op);
    ip += 5; // nameIdx(2B) + superNameIdx(2B) + opcode(1B)
    return VMResult::VM_OK;
}

// W3-2-Bug2 fix: 在类定义时为有 upvalue 的方法创建 upvalue 捕获
void VM::captureMethodUpvalues(const std::string& className) {
    auto methodsIt = methodsByClass_.find(className);
    if (methodsIt == methodsByClass_.end())
        return;
    VMCallFrame& frame = currentFrame();
    for (const auto& [methodName, chunkPtr] : methodsIt->second) {
        if (!chunkPtr || chunkPtr->upvalues.empty())
            continue;
        auto closureData = std::make_shared<VMClosureData>();
        closureData->functionName = chunkPtr->name;
        closureData->chunkPtr = chunkPtr;
        closureData->upvalues.resize(chunkPtr->upvalues.size());
        for (size_t i = 0; i < chunkPtr->upvalues.size(); ++i) {
            const UpvalueDesc& desc = chunkPtr->upvalues[i];
            if (desc.isLocal) {
                // 直接捕获：创建 open upvalue 指向当前帧的栈槽
                auto uv = std::make_shared<VMUpvalue>();
                uv->stackSlot = frame.basePointer + static_cast<size_t>(desc.index);
                uv->isClosed = false;
                uv->owningFrameIdx = frames_.size() - 1;
                closureData->upvalues[i] = uv;
                openUpvalues_.emplace(uv->stackSlot, uv);
            } else {
                // 透传：复用当前帧的 upvalue
                if (static_cast<size_t>(desc.index) < frame.upvalues.size()) {
                    closureData->upvalues[i] = frame.upvalues[static_cast<size_t>(desc.index)];
                } else {
                    auto uv = std::make_shared<VMUpvalue>();
                    uv->value = Value::nullValue();
                    uv->isClosed = true;
                    closureData->upvalues[i] = uv;
                }
            }
        }
        methodUpvalues_[chunkPtr->name] = closureData;
    }
}

// ============================================================
// R98 W2: 同步调用闭包值（供高阶函数共享层回调）
// ============================================================
// 手动构造调用帧（模拟 OP_CALL_EXPR 的帧设置），压入 frames_，
// 运行内部指令循环直到该帧弹出，从栈顶读取返回值。
//
// 关键不变量：
//   1. 调用前后 frames_.size() 不变（push 一次，pop 一次）
//   2. 调用前后 stack_.size() 不变（push args + locals，pop 全部 + push result）
//   3. 调用者 ip 引用可能因 frames_.push_back 重分配而悬垂——调用方负责保存/恢复
//   4. DoS 防护：内部循环使用本地计数器，限制为 maxInstructions（与外层独立）
//   5. 错误传播：hasError_ + diagnostics_ 由内部 executeOneInstruction 设置，
//      调用方检测 VM_RUNTIME_ERROR 后通过 getLastError() 获取错误信息（P2-12）

VMResult VM::invokeClosureSync(const Value& closure, const Value* args, size_t argCount, int /*line*/, int /*column*/,
                               Value& result) {
    if (!closure.isClosure()) {
        return runtimeError("高阶函数的参数必须是函数", DiagCodes::kTypeMismatch);
    }

    // 查找闭包 chunk（与 OP_CALL_EXPR 路径一致）
    const BytecodeChunk* targetChunkPtr = nullptr;
    if (closure.vmClosure() && closure.vmClosure()->chunkPtr) {
        targetChunkPtr = closure.vmClosure()->chunkPtr;
    } else {
        auto chunkIt = functionChunks_.find(closure.closureName());
        if (chunkIt != functionChunks_.end()) {
            targetChunkPtr = &chunkIt->second;
        }
    }
    if (!targetChunkPtr) {
        return runtimeError("未找到函数: " + closure.closureName(), "undefined-function");
    }
    const BytecodeChunk& targetChunk = *targetChunkPtr;

    // 参数数量检查
    if (argCount < static_cast<size_t>(targetChunk.requiredArity) ||
        argCount > static_cast<size_t>(targetChunk.arity)) {
        return runtimeError(ErrorFormat::formatStd("函数 {} 期望 {}-{} 个参数，但传入了 {} 个",

                                                   closure.closureName(), targetChunk.requiredArity,

                                                   targetChunk.arity, argCount),
                            DiagCodes::kArityMismatch);
    }

    // MAX_FRAMES 检查
    if (frames_.size() >= MAX_FRAMES) {
        return runtimeError(
            ErrorFormat::formatStd(ErrorMessages::kRecursionDepthExceededFmtStd, static_cast<int>(MAX_FRAMES)),
            DiagCodes::kRecursionDepth);
    }

    // 压入参数（左到右，arg0 在栈低位）
    for (size_t i = 0; i < argCount; ++i) {
        push(args[i]);
    }
    // 填充默认参数
    size_t effectiveArgCount = argCount;
    if (argCount < static_cast<size_t>(targetChunk.arity)) {
        int missingCount = targetChunk.arity - static_cast<int>(argCount);
        int defaultStartIdx = static_cast<int>(targetChunk.defaultConstIndices.size()) - missingCount;
        if (defaultStartIdx < 0) {
            popN(argCount);
            return runtimeError("函数 " + closure.closureName() + " 默认参数索引越界");
        }
        for (int i = defaultStartIdx; i < defaultStartIdx + missingCount; ++i) {
            uint16_t constIdx = targetChunk.defaultConstIndices[i];
            if (constIdx == RuntimeLimits::NO_INDEX || constIdx >= targetChunk.constants.size()) {
                popN(effectiveArgCount);
                return runtimeError("函数 " + closure.closureName() + " 默认参数无效");
            }
            push(targetChunk.constants[constIdx]);
            ++effectiveArgCount;
        }
    }

    // 预分配局部变量槽
    int extraSlots = targetChunk.localCount - static_cast<int>(effectiveArgCount);
    if (extraSlots < 0) {
        popN(effectiveArgCount);
        return runtimeError("闭包调用帧布局损坏");
    }
    for (int i = 0; i < extraSlots; ++i) {
        push(Value::nullValue());
    }

    // 构造调用帧
    VMCallFrame newFrame;
    newFrame.chunk = &targetChunk;
    // R136 fix: 保存调用者帧的当前 IP，OP_RETURN 会用 returnIp 恢复调用者帧 ip。
    // 原设 returnIp=0 作哨兵、由调用方负责恢复，但 join 路径（dispatchSyncObjectBuiltin）
    // 只执行 ip += instrLen 而未覆盖 OP_RETURN 设入的 0，导致调用者帧 ip 被重置为 0，
    // 主线程从头重新执行字节码形成无限循环（spawn(worker)+join() 表现为 worker 反复执行）。
    // 高阶函数路径（executeCallHigherOrder）和 spawn 路径（executeCallSpawn）在
    // invokeClosureSync 返回后会显式设置 currentFrame().ip = savedIp + 4 覆盖此值，
    // 不受影响。设置 returnIp = currentFrame().ip 让 OP_RETURN 自动恢复调用者帧 ip。
    newFrame.returnIp = currentFrame().ip;
    newFrame.basePointer = stack_.size() - targetChunk.localCount;
    newFrame.functionName = closure.closureName();
    newFrame.ip = 0;
    if (closure.vmClosure()) {
        newFrame.upvalues = closure.vmClosure()->upvalues;
    }

    size_t savedFrameCount = frames_.size();
    size_t savedTryStackSize = tryStack_.size(); // P3-A1: 快照 tryStack_ 检测异常穿透
    frames_.push_back(std::move(newFrame));

    // 内部指令循环：执行直到帧弹出
    // DoS 防护：本地计数器限制为 maxInstructions（与外层 execute() 独立计数，
    // 总上限为 2x maxInstructions，对教学场景足够）
    int64_t localInstrCount = 0;
    int64_t maxInstr = RuntimeLimits::RuntimeConfig::instance().maxInstructions();
    while (frames_.size() > savedFrameCount && !hasError_) {
        if (++localInstrCount > maxInstr) {
            runtimeError("指令执行数超过上限，疑似无限循环");
            break;
        }
        VMResult r = executeOneInstruction();
        // L14: VM_EXCEPTION_THROW 表示异常被 try/catch 捕获，继续执行
        if (r == VMResult::VM_EXCEPTION_THROW)
            continue;
        if (r != VMResult::VM_OK || hasError_) {
            break;
        }
    }

    if (hasError_) {
        // P2-12: 错误已写入 diagnostics_，调用方检测 VM_RUNTIME_ERROR 后通过 getLastError() 读取
        return VMResult::VM_RUNTIME_ERROR;
    }

    // P3-A1: 异常穿透检测
    // spawn 闭包内 throw 触发 throwException 弹出闭包帧 + 弹出外层 try handler + push thrownValue
    // 到栈顶 + 设 main 帧 catchIp。此时 frames_ 已退回 savedFrameCount，tryStack_ 缩小，
    // 栈顶是 thrownValue（不应被当作闭包返回值 pop）。返回 VM_EXCEPTION_THROW 让调用方
    // （dispatchSyncObjectBuiltin / executeCallSpawn 等）不操作 ip/stack，由主循环保留 catchIp
    // 继续 catch 块。判别信号：spawn 闭包内 throw 后 join 报"栈下溢"-> 检查此处是否误 pop。
    if (tryStack_.size() < savedTryStackSize) {
        return VMResult::VM_EXCEPTION_THROW;
    }

    // 闭包返回值在栈顶（OP_RETURN 已 push）
    if (stack_.empty()) {
        return runtimeError("闭包调用未返回值");
    }
    result = pop();
    return VMResult::VM_OK;
}

// ============================================================
// R164 协程/生成器：OP_YIELD 指令执行（重放模式）
// ============================================================
// 语义: pop 栈顶 yield 值，递增运行时 yield 执行计数器。
//   - 若计数器 == 当前重放目标 yieldId：抛出 VMYieldSignal(yieldValue)，被 callCoroutineNext 捕获
//   - 若计数器 < 目标：push yieldValue 回栈（作为 yield 表达式的结果），继续执行
// 注：VM 使用与 Interpreter 相同的重放模式，保证四后端语义一致。
// ============================================================
VMResult VM::executeCoroutineOps(OpCode op, size_t& ip) {
    if (op != OpCode::OP_YIELD) {
        return runtimeError(ErrorFormat::formatStd("未知协程操作码: {}", static_cast<int>(op)));
    }
    // 防御性检查：yield 在非生成器函数体中出现（Compiler 已在 visitYieldExpr 拦截，
    // 但字节码注入路径或动态构造的 chunk 可能绕过——此处兜底报错而非崩溃）
    if (currentCoroutineTargetYieldId_ < 0) {
        return runtimeError("yield 只能在 fun* 生成器函数体内出现");
    }
    if (stack_.empty()) {
        return runtimeError("栈下溢: OP_YIELD");
    }
    Value yieldValue = pop();
    // 运行时 yield 执行计数：每次 OP_YIELD 调用递增
    // 用于区分循环内同一 yield 节点的多次执行（编译期 yieldId 无法区分）
    int thisExecutionId = currentYieldExecutionCount_++;
    if (thisExecutionId == currentCoroutineTargetYieldId_) {
        // 命中目标 yield：抛出 VMYieldSignal，被 callCoroutineNext 捕获
        throw VMYieldSignal(std::move(yieldValue));
    }
    // thisExecutionId < target：跳过此 yield（重放模式核心）
    // push yieldValue 回栈作为 yield 表达式的结果，继续执行
    push(std::move(yieldValue));
    ip += 1; // OP_YIELD 是 1 字节指令
    return VMResult::VM_OK;
}

// ============================================================
// R164 D.5: 生成器函数调用拦截——创建协程值
// ============================================================
// 从栈弹出参数，填充默认参数，构造 Coroutine 值并 push 到栈顶。
// 被 executeCallFunction（OP_CALL）和 executeCallExprValue（OP_CALL_EXPR）共用。
// instrLen 为调用指令长度（OP_CALL=4, OP_CALL_EXPR=2），用于推进 ip。
// ============================================================
VMResult VM::createCoroutineValue(const BytecodeChunk& genChunk, const std::string& funName, uint8_t argCount,
                                  Value closureVal, size_t& ip, int instrLen) {
    // 弹出参数
    if (stack_.size() < static_cast<size_t>(argCount))
        return runtimeError("栈下溢: 生成器调用 " + funName);
    std::vector<Value> args(argCount);
    for (int i = argCount - 1; i >= 0; --i) {
        args[i] = pop();
    }
    // 填充默认参数
    if (argCount < static_cast<uint8_t>(genChunk.arity)) {
        int missingCount = genChunk.arity - argCount;
        int defaultStartIdx = static_cast<int>(genChunk.defaultConstIndices.size()) - missingCount;
        if (defaultStartIdx < 0) {
            return runtimeError("生成器 " + funName + " 默认参数索引越界");
        }
        for (int i = defaultStartIdx; i < defaultStartIdx + missingCount; ++i) {
            uint16_t constIdx = genChunk.defaultConstIndices[i];
            if (constIdx == RuntimeLimits::NO_INDEX || constIdx >= genChunk.constants.size()) {
                return runtimeError("生成器 " + funName + " 默认参数无效");
            }
            args.push_back(genChunk.constants[constIdx]);
        }
    }
    // 创建协程值并 push 到栈顶
    Value coro = Value::makeCoroutineVM(&genChunk, std::move(closureVal), std::move(args), genChunk.yieldCount);
    push(std::move(coro));
    ip += instrLen;
    return VMResult::VM_OK;
}

// ============================================================
// R164 D.5: 协程方法分发（.next() / .done()）
// ============================================================
// 协程内部状态通过 CoroutineData* 共享指针修改，无需 writeBack（与同步对象语义一致）。
// 栈布局: [..., receiver, arg0, ..., argN-1]
// 执行后: [..., result]（pop receiver + args，push result）
// ============================================================
VMResult VM::dispatchCoroutineBuiltin(Value& obj, const std::string& methodName, uint8_t argCount, size_t& ip,
                                      OpCode /*op*/, int instrLen) {
    if (methodName == "next") {
        if (argCount != 0) {
            // 清理栈上参数和接收者
            popN(argCount);
            pop();
            return runtimeError("coroutine.next() 不接受参数", DiagCodes::kArityMismatch);
        }
        Value result = callCoroutineNext(obj);
        if (hasError_) {
            return VMResult::VM_RUNTIME_ERROR;
        }
        // 设置 lastMutatedReceiver_ 为接收者原值（对齐 finishSharedBuiltin 和 RegisterVM
        // 的 lastMutatedReceiverReg_ = objReg）。IR 路径对所有 isVarRef 方法调用无条件
        // 发射 LOAD_MUTATED + STORE，若不设置，LOAD_MUTATED 会读到过期/null 值，STORE
        // 覆盖接收者变量（如协程值被 null 覆盖）。
        lastMutatedReceiver_ = obj;
        pop();                   // pop receiver
        push(std::move(result)); // push .next() result
        ip += instrLen;
        return VMResult::VM_OK;
    }
    if (methodName == "done") {
        if (argCount != 0) {
            popN(argCount);
            pop();
            return runtimeError("coroutine.done() 不接受参数", DiagCodes::kArityMismatch);
        }
        auto* cd = obj.coroutineData();
        Value result(cd->done);
        // 同 .next()：设置 lastMutatedReceiver_ 为接收者原值
        lastMutatedReceiver_ = obj;
        pop();
        push(std::move(result));
        ip += instrLen;
        return VMResult::VM_OK;
    }
    // 未知方法：清理栈并报错
    popN(argCount);
    pop();
    return runtimeError("coroutine 类型不支持方法 " + methodName, "undefined-function");
}

// ============================================================
// R164 D.5: 协程 .next() 重放执行
// ============================================================
// 核心思路（与 Interpreter::callCoroutineNext 一致）：
//   1. 若已耗尽（done=true），返回 currentValueBox 中的最后值
//   2. 保存当前帧/栈状态，为生成器 body 设置新帧
//   3. 设置 currentCoroutineTargetYieldId_ = cd->currentYieldId，重置计数器
//   4. 运行内部指令循环，OP_YIELD 命中目标时抛出 VMYieldSignal
//   5. 捕获 VMYieldSignal → 保存 yield 值，递增 currentYieldId
//   6. 函数体自然结束（OP_RETURN）→ 标记 done=true，返回 return 值
//   7. 恢复帧/栈状态，返回 yield/return 值
//
// 重放模式：每次 .next() 从 ip=0 重新执行生成器 body，用运行时计数器跳过已返回的 yield。
// 对齐 Interpreter 重放模式语义，保证四后端一致性。
// ============================================================
Value VM::callCoroutineNext(Value& coroVal) {
    auto* cd = coroVal.coroutineData();

    // 已耗尽：返回最后一次 yield/return 的值
    if (cd->done) {
        return cd->currentValueBox.empty() ? Value::nullValue() : cd->currentValueBox.front();
    }

    const BytecodeChunk* genChunk = cd->vmChunk;
    if (!genChunk) {
        runtimeError("协程缺少生成器字节码块");
        return Value::nullValue();
    }

    // MAX_FRAMES 检查
    if (frames_.size() >= MAX_FRAMES) {
        runtimeError(ErrorFormat::formatStd(ErrorMessages::kRecursionDepthExceededFmtStd, static_cast<int>(MAX_FRAMES)),
                     DiagCodes::kRecursionDepth);
        return Value::nullValue();
    }

    // 保存调用方上下文
    size_t savedFrameCount = frames_.size();
    size_t savedStackSize = stack_.size();
    int savedTargetYieldId = currentCoroutineTargetYieldId_;
    int savedYieldExecCount = currentYieldExecutionCount_;

    // 压入参数（左到右，arg0 在栈低位）
    uint8_t argCount =
        static_cast<uint8_t>(std::min(cd->args.size(), static_cast<size_t>(std::numeric_limits<uint8_t>::max())));
    for (const auto& arg : cd->args) {
        push(arg);
    }

    // 填充默认参数
    uint8_t effectiveArgCount = argCount;
    if (argCount < static_cast<uint8_t>(genChunk->arity)) {
        int missingCount = genChunk->arity - argCount;
        int defaultStartIdx = static_cast<int>(genChunk->defaultConstIndices.size()) - missingCount;
        if (defaultStartIdx >= 0) {
            for (int i = defaultStartIdx; i < defaultStartIdx + missingCount; ++i) {
                uint16_t constIdx = genChunk->defaultConstIndices[i];
                if (constIdx != RuntimeLimits::NO_INDEX && constIdx < genChunk->constants.size()) {
                    push(genChunk->constants[constIdx]);
                    ++effectiveArgCount;
                }
            }
        }
    }

    // 预分配局部变量槽
    int extraSlots = genChunk->localCount - static_cast<int>(effectiveArgCount);
    if (extraSlots < 0) {
        // 帧布局损坏，清理并报错
        stack_.resize(savedStackSize);
        runtimeError("生成器 " + genChunk->name + " 帧布局损坏");
        return Value::nullValue();
    }
    for (int i = 0; i < extraSlots; ++i) {
        push(Value::nullValue());
    }

    // 构造调用帧
    VMCallFrame newFrame;
    newFrame.chunk = genChunk;
    newFrame.returnIp = currentFrame().ip;
    newFrame.basePointer = stack_.size() - genChunk->localCount;
    newFrame.functionName = genChunk->name;
    newFrame.ip = 0;
    // 绑定闭包 upvalues
    // R164 fixup: vmClosure 改为单元素容器（绕开 Value 不完整类型 C2079），
    // 访问时取首元素再调用 Value::vmClosure() 取 shared_ptr<VMClosureData>
    if (!cd->vmClosureBox.empty()) {
        auto& closureVal = cd->vmClosureBox.front();
        if (closureVal.isClosure() && closureVal.vmClosure()) {
            newFrame.upvalues = closureVal.vmClosure()->upvalues;
        }
    }
    frames_.push_back(std::move(newFrame));

    // 设置协程重放上下文
    currentCoroutineTargetYieldId_ = cd->currentYieldId;
    currentYieldExecutionCount_ = 0;

    Value result = Value::nullValue();
    bool needCleanup = false; // true = 需手动清理帧/栈（yield 信号或错误路径）

    try {
        // 内部指令循环：执行直到生成器帧弹出
        int64_t localInstrCount = 0;
        int64_t maxInstr = RuntimeLimits::RuntimeConfig::instance().maxInstructions();
        while (frames_.size() > savedFrameCount && !hasError_) {
            if (++localInstrCount > maxInstr) {
                runtimeError("指令执行数超过上限，疑似无限循环");
                break;
            }
            VMResult r = executeOneInstruction();
            // L14: VM_EXCEPTION_THROW 表示异常被 try/catch 捕获，继续执行
            if (r == VMResult::VM_EXCEPTION_THROW)
                continue;
            if (r != VMResult::VM_OK || hasError_) {
                break;
            }
        }

        if (hasError_) {
            needCleanup = true;
        } else if (stack_.size() > savedStackSize) {
            // 正常结束：OP_RETURN 已弹出帧并 push 返回值
            result = pop();
        }
        cd->done = true;
        cd->currentValueBox.clear();
        cd->currentValueBox.push_back(result);
    } catch (const VMYieldSignal& e) {
        // 命中目标 yield：保存 yield 值，递增 currentYieldId
        result = std::move(e.yieldValue);
        cd->currentValueBox.clear();
        cd->currentValueBox.push_back(result);
        cd->currentYieldId++;
        // 若递增后达到 yieldCount，标记 done（下次 .next() 将返回 currentValue）
        if (cd->currentYieldId >= cd->yieldCount) {
            cd->done = true;
        }
        needCleanup = true; // 生成器帧仍在 frames_ 上，需手动清理
    }

    // 手动清理（yield 信号或错误路径）：生成器帧可能仍在 frames_ 上
    if (needCleanup) {
        while (frames_.size() > savedFrameCount) {
            frames_.pop_back();
        }
        // 清理 try handler
        while (!tryStack_.empty() && tryStack_.back().frameIndex >= savedFrameCount) {
            tryStack_.pop_back();
        }
        // 关闭 open upvalues 指向生成器栈区域的
        closeUpvaluesFrom(savedStackSize);
        // 截断栈
        stack_.resize(savedStackSize);
    }

    // 恢复协程上下文
    currentCoroutineTargetYieldId_ = savedTargetYieldId;
    currentYieldExecutionCount_ = savedYieldExecCount;
    return result;
}
