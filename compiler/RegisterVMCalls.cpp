// ============================================================
// RegisterVMCalls.cpp — 寄存器式虚拟机：调用相关方法
// ------------------------------------------------------------
// 从 RegisterVM.cpp 拆分而来（编译速度优化，无逻辑变更）。
// 包含：executeCalls（及子方法 executeCallOps/executeMethodCallOps/executeNewOps）,
//       executeCallImpl, executeReturnImpl, executeMethodCallImpl,
//       executeClosureImpl, executeClassNewImpl, captureMethodUpvalues,
//       fillDefaultArgs, callBuiltinMethod（及子方法）, invokeClosureSync,
//       createCoroutineValue, callCoroutineNext, dispatchCoroutineBuiltin。
// ============================================================

#include "compiler/RegisterVM.h"
#include "common/ErrorFormat.h"
#include "common/ErrorMessages.h"
#include "common/Logger.h"
#include "common/RuntimeLimits.h"
#include "common/TypeChecker.h"
#include "common/Utf8Utils.h"
#include "compiler/RegisterBytecode.h"
#include "interpreter/BuiltinMethods.h"
#include "interpreter/NumericUtils.h"
#include <algorithm>
#include <cassert>
#include <cmath>

// ============================================================
// 调用指令分发
// ============================================================

VMResult RegisterVM::executeCallOps(RegOp op, size_t& ip) {
    const RegBytecodeChunk& chunk = *currentFrame().chunk;

    switch (op) {
    case RegOp::REG_CALL: {
        uint8_t dst = chunk.code[ip + 1];
        uint16_t nameIdx = chunk.code[ip + 2] | (chunk.code[ip + 3] << 8);
        uint8_t argCount = chunk.code[ip + 4];
        if (nameIdx >= chunk.constants.size() || !chunk.constants[nameIdx].isString()) {
            return runtimeError("函数名索引无效");
        }
        const std::string& funName = chunk.constants[nameIdx].stringVal();
        SmallArgs<uint8_t> argRegs;
        for (uint8_t i = 0; i < argCount; ++i) {
            argRegs.push_back(chunk.code[ip + 5 + i]);
        }
        size_t newIp = ip; // 保存当前 ip（调用返回后更新）
        // C-8 fix: REG_CALL 指令长度 = 5 + argCount
        VMResult r = executeCallImpl(newIp, funName, argCount, dst, argRegs, 5u + argCount);
        if (r != VMResult::VM_OK)
            return r;
        // 调用未返回（同步调用）：ip 不变，由新帧接管执行
        ip = newIp;
        break;
    }
    case RegOp::REG_CALL_EXPR: {
        uint8_t dst = chunk.code[ip + 1];
        uint8_t calleeReg = chunk.code[ip + 2];
        uint8_t argCount = chunk.code[ip + 3];
        const Value& callee = reg(calleeReg);
        if (!callee.isClosure()) {
            return runtimeError("调用非闭包值", DiagCodes::kTypeMismatch);
        }
        SmallArgs<uint8_t> argRegs;
        for (uint8_t i = 0; i < argCount; ++i) {
            argRegs.push_back(chunk.code[ip + 4 + i]);
        }
        // C-2 fix: 传入闭包值，executeCallImpl 从其 vmClosure 提取 upvalues 填入新帧
        const std::string& funName = callee.closureName();
        size_t newIp = ip;
        // C-8 fix: REG_CALL_EXPR 指令长度 = 4 + argCount（原硬编码 5+argCount 偏移 +1）
        VMResult r = executeCallImpl(newIp, funName, argCount, dst, argRegs, 4u + argCount, &callee);
        if (r != VMResult::VM_OK)
            return r;
        ip = newIp;
        break;
    }
    case RegOp::REG_MAKE_CLOSURE: {
        uint8_t dst = chunk.code[ip + 1];
        uint16_t nameIdx = chunk.code[ip + 2] | (chunk.code[ip + 3] << 8);
        uint8_t uvCount = chunk.code[ip + 4];
        if (nameIdx >= chunk.constants.size() || !chunk.constants[nameIdx].isString()) {
            return runtimeError("闭包名索引无效");
        }
        const std::string& name = chunk.constants[nameIdx].stringVal();
        SmallArgs<uint8_t> uvSpecs;
        for (uint8_t i = 0; i < uvCount; ++i) {
            uvSpecs.push_back(chunk.code[ip + 5 + i * 2]);     // isLocal
            uvSpecs.push_back(chunk.code[ip + 5 + i * 2 + 1]); // idx
        }
        size_t newIp = ip;
        VMResult r = executeClosureImpl(newIp, name, uvCount, uvSpecs, dst);
        if (r != VMResult::VM_OK)
            return r;
        ip = newIp;
        break;
    }
    default:
        return runtimeError("executeCallOps: 未知操作码");
    }

    notifyStep(ip, op);
    return VMResult::VM_OK;
}

VMResult RegisterVM::executeMethodCallOps(RegOp op, size_t& ip) {
    const RegBytecodeChunk& chunk = *currentFrame().chunk;

    switch (op) {
    case RegOp::REG_METHOD_CALL: {
        uint8_t dst = chunk.code[ip + 1];
        uint8_t objReg = chunk.code[ip + 2];
        uint16_t methodIdx = chunk.code[ip + 3] | (chunk.code[ip + 4] << 8);
        uint8_t argCount = chunk.code[ip + 5];
        if (methodIdx >= chunk.constants.size() || !chunk.constants[methodIdx].isString()) {
            return runtimeError("方法名索引无效");
        }
        const std::string& methodName = chunk.constants[methodIdx].stringVal();
        SmallArgs<uint8_t> argRegs;
        for (uint8_t i = 0; i < argCount; ++i) {
            argRegs.push_back(chunk.code[ip + 6 + i]);
        }
        size_t newIp = ip;
        VMResult r = executeMethodCallImpl(newIp, methodName, argCount, dst, objReg, argRegs);
        if (r != VMResult::VM_OK)
            return r;
        ip = newIp;
        break;
    }
    default:
        return runtimeError("executeMethodCallOps: 未知操作码");
    }

    notifyStep(ip, op);
    return VMResult::VM_OK;
}

VMResult RegisterVM::executeNewOps(RegOp op, size_t& ip) {
    const RegBytecodeChunk& chunk = *currentFrame().chunk;

    switch (op) {
    case RegOp::REG_CLASS_NEW: {
        uint8_t dst = chunk.code[ip + 1];
        uint16_t nameIdx = chunk.code[ip + 2] | (chunk.code[ip + 3] << 8);
        uint8_t argCount = chunk.code[ip + 4];
        if (nameIdx >= chunk.constants.size() || !chunk.constants[nameIdx].isString()) {
            return runtimeError("类名索引无效");
        }
        const std::string& className = chunk.constants[nameIdx].stringVal();
        SmallArgs<uint8_t> argRegs;
        for (uint8_t i = 0; i < argCount; ++i) {
            argRegs.push_back(chunk.code[ip + 5 + i]);
        }
        size_t newIp = ip;
        VMResult r = executeClassNewImpl(newIp, className, argCount, dst, argRegs);
        if (r != VMResult::VM_OK)
            return r;
        ip = newIp;
        break;
    }
    case RegOp::REG_DEFINE_CLASS: {
        // C-9 fix: 完整填充 classInfo_ 的 name/parent/fieldOrder/methods。
        // 原实现仅设置 .name，导致方法调用/构造全部失败。
        // BUG-INH-1 fix: 新增字段默认值常量索引
        // BUG-INH-IR-1 fix: 新增字段表达式寄存器（非字面量默认值从寄存器读取）
        // 编码：op + nameIdx(2B) + parentIdx(2B) + fieldCount(1B)
        //      + [fieldIdx(2B) + defaultConstIdx(2B) + exprReg(1B)]×F
        //      + methodCount(1B) + [methodIdx(2B)+funIdx(2B)]×M
        // exprReg: NO_SLOT=使用常量/null, 否则从 reg(exprReg) 读取运行时求值结果
        uint16_t nameIdx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        uint16_t parentIdx = chunk.code[ip + 3] | (chunk.code[ip + 4] << 8);
        if (nameIdx >= chunk.constants.size() || !chunk.constants[nameIdx].isString()) {
            return runtimeError("类名索引无效");
        }
        const std::string& className = chunk.constants[nameIdx].stringVal();

        auto& info = classInfo_[className];
        info.name = className;
        info.fieldOrder.clear();
        info.methods.clear();
        info.fieldDefaults.clear();     // BUG-INH-1 fix
        info.flattenedComputed = false; // perf2 fix: 重定义时使预计算缓存失效

        // BUG-INH-AUDIT-7 fix: 父类重定义时，所有依赖此父类的子类缓存失效。
        // 遍历所有已注册类，若其继承链包含当前重定义的类，置 flattenedComputed=false
        // 强制下次访问时重新计算 flattenedFieldOrder/flattenedFieldDefaults/hasInit。
        // R133: 同步清空 methodCache（per-class 方法分发内联缓存），与 StackVM
        // BUG-INH-AUDIT-7 fix (VMCalls.cpp:1222-1240) 对齐。父类方法集合可能变化,
        // 子类缓存的 methodName→funName 映射可能失效（如父类新增/删除/重命名方法）。
        for (auto& kv : classInfo_) {
            if (kv.first == className)
                continue; // 跳过当前类
            std::string cur = kv.second.parent;
            for (int guard = 0; guard < 64 && !cur.empty(); ++guard) {
                if (cur == className) {
                    kv.second.flattenedComputed = false;
                    kv.second.methodCache.clear(); // R133: 失效方法分发缓存
                    break;
                }
                auto it = classInfo_.find(cur);
                if (it == classInfo_.end())
                    break;
                cur = it->second.parent;
            }
        }

        if (parentIdx != RuntimeLimits::NO_INDEX) {
            if (parentIdx >= chunk.constants.size() || !chunk.constants[parentIdx].isString()) {
                return runtimeError("父类名索引无效");
            }
            info.parent = chunk.constants[parentIdx].stringVal();
            // BUG-INH-AUDIT-8 fix: 父类存在性检查。StackVM 的 executeDefineClass
            // （VMCalls.cpp）在定义时立即检查父类是否已注册，RegisterVM 原实现静默通过，
            // 延迟到构造时才报错（或永不报错），三后端错误检测时机不一致。
            if (classInfo_.find(info.parent) == classInfo_.end()) {
                return runtimeError("未定义的父类: " + info.parent, "undefined-function");
            }
            // BUG-INH-AUDIT-9 fix: 循环继承检测。StackVM 在定义时沿继承链构建 chain，
            // guard 耗尽后 cur 仍非空则报循环。RegisterVM 原实现延迟到 lazy flattened
            // 计算时检测，若循环类从未构造则永不报错。此处对齐 StackVM 在定义时检测。
            std::string cur = info.parent;
            for (int guard = 0; guard < 64 && !cur.empty(); ++guard) {
                if (cur == className) {
                    return runtimeError("类继承链过深或存在循环继承: " + className + " -> " + cur);
                }
                auto it = classInfo_.find(cur);
                if (it == classInfo_.end())
                    break;
                cur = it->second.parent;
            }
        } else {
            info.parent.clear();
        }

        size_t cursor = ip + 5;
        if (cursor >= chunk.code.size()) {
            return runtimeError("DEFINE_CLASS: 字段计数截断");
        }
        uint8_t fieldCount = chunk.code[cursor];
        cursor += 1;
        for (uint8_t i = 0; i < fieldCount; ++i) {
            if (cursor + 4 >= chunk.code.size()) {
                return runtimeError("DEFINE_CLASS: 字段名/默认值/表达式寄存器截断");
            }
            uint16_t fIdx = chunk.code[cursor] | (chunk.code[cursor + 1] << 8);
            uint16_t defaultIdx = chunk.code[cursor + 2] | (chunk.code[cursor + 3] << 8);
            uint8_t exprReg = chunk.code[cursor + 4];
            if (fIdx >= chunk.constants.size() || !chunk.constants[fIdx].isString()) {
                return runtimeError("字段名索引无效");
            }
            info.fieldOrder.push_back(chunk.constants[fIdx].stringVal());
            // BUG-INH-IR-1 fix: 非字面量表达式从寄存器读取运行时求值结果
            if (exprReg != RuntimeLimits::NO_SLOT) {
                info.fieldDefaults.push_back(reg(exprReg));
            } else if (defaultIdx == RuntimeLimits::NO_INDEX) {
                info.fieldDefaults.push_back(Value::nullValue());
            } else {
                if (defaultIdx >= chunk.constants.size()) {
                    return runtimeError("字段默认值索引无效");
                }
                info.fieldDefaults.push_back(chunk.constants[defaultIdx]);
            }
            cursor += 5; // fieldIdx(2B) + defaultIdx(2B) + exprReg(1B)
        }

        if (cursor >= chunk.code.size()) {
            return runtimeError("DEFINE_CLASS: 方法计数截断");
        }
        uint8_t methodCount = chunk.code[cursor];
        cursor += 1;
        for (uint8_t i = 0; i < methodCount; ++i) {
            if (cursor + 3 >= chunk.code.size()) {
                return runtimeError("DEFINE_CLASS: 方法/函数名索引截断");
            }
            uint16_t mIdx = chunk.code[cursor] | (chunk.code[cursor + 1] << 8);
            uint16_t fIdx = chunk.code[cursor + 2] | (chunk.code[cursor + 3] << 8);
            if (mIdx >= chunk.constants.size() || !chunk.constants[mIdx].isString() || fIdx >= chunk.constants.size() ||
                !chunk.constants[fIdx].isString()) {
                return runtimeError("方法/函数名索引无效");
            }
            info.methods[chunk.constants[mIdx].stringVal()] = chunk.constants[fIdx].stringVal();
            cursor += 4;
        }

        // W3-2-Bug2 fix: 为有 upvalue 的方法创建 upvalue 捕获。
        // 类定义在外层函数内时，方法可能引用外层函数的局部变量（upvalue）。
        // 此时仍在定义类的外层函数帧中，可以正确捕获寄存器槽/upvalue。
        // 方法调用时从 methodUpvalues_ 读取并填充方法帧的 upvalues。
        captureMethodUpvalues(className);

        ip = cursor;
        break;
    }
    default:
        return runtimeError("executeNewOps: 未知操作码");
    }

    notifyStep(ip, op);
    return VMResult::VM_OK;
}

VMResult RegisterVM::executeCalls(RegOp op, size_t& ip) {
    const RegBytecodeChunk& chunk = *currentFrame().chunk;

    switch (op) {
    case RegOp::REG_RETURN: {
        uint8_t src = chunk.code[ip + 1];
        Value result = reg(src);
        // BUG-REGVM-2 fix: executeReturnImpl 成功返回时帧已弹出、ip 已更新到调用者，
        // 需调用 stepCallback_ 反映调用者帧状态，否则调试器单步丢失步进事件。
        VMResult r = executeReturnImpl(ip, std::move(result));
        if (r == VMResult::VM_OK)
            notifyStep(ip, op);
        return r;
    }
    case RegOp::REG_RETURN_NULL: {
        VMResult r = executeReturnImpl(ip, Value::nullValue());
        if (r == VMResult::VM_OK)
            notifyStep(ip, op);
        return r;
    }
    case RegOp::REG_CALL:
    case RegOp::REG_CALL_EXPR:
    case RegOp::REG_MAKE_CLOSURE:
        // 普通函数调用 / 闭包值调用 / 闭包构造
        return executeCallOps(op, ip);
    case RegOp::REG_METHOD_CALL:
        // 实例方法调用（含内置方法分发）
        return executeMethodCallOps(op, ip);
    case RegOp::REG_CLASS_NEW:
    case RegOp::REG_DEFINE_CLASS:
        // 类构造 / 类定义
        return executeNewOps(op, ip);
    default:
        return runtimeError("executeCalls: 未知操作码");
    }
}

// ============================================================
// 函数调用实现
// ============================================================

VMResult RegisterVM::executeCallImpl(size_t& ip, const std::string& funName, uint8_t argCount, uint8_t dstReg,
                                     const SmallArgs<uint8_t>& argRegs, size_t returnOffset, const Value* closureValue,
                                     bool isMethodCall) {
    if (frames_.size() >= MAX_FRAMES) {
        // R97 #11 fix: 三后端递归深度消息统一
        return runtimeError(
            ErrorFormat::formatStd(ErrorMessages::kRecursionDepthExceededFmtStd, static_cast<int>(MAX_FRAMES)),
            DiagCodes::kRecursionDepth);
    }

    // C-2 fix: 若调用者通过 REG_CALL_EXPR 传入闭包值，优先使用其绑定的函数名查找 chunk，
    // 并在创建新帧时从 vmClosure 提取 upvalues 填入 newFrame.upvalues。
    // 若 closureValue 为空（REG_CALL 命名调用），回退到原查找路径。
    const std::shared_ptr<VMClosureData>& closureData =
        (closureValue && closureValue->isClosure()) ? closureValue->vmClosure() : nullptr;

    // PERF-AUDIT-5 fix: 内联缓存快速路径。仅对 REG_CALL（closureValue==nullptr）缓存，
    // 因 funName 来自常量池（稳定指针）；REG_CALL_EXPR 的 funName 来自闭包对象
    // （callee.closureName()），闭包销毁后指针悬垂，不可缓存。与 StackVM callCache_
    // 只在 OP_CALL 路径缓存、OP_CALL_EXPR 不缓存的模式一致。
    // R133 fix: 方法调用路径（isMethodCall=true）也不缓存,因 funName 来自 methods map/
    // classInfo_/局部变量,不是常量池稳定指针。用 &funName 作 callCache_ key 时,
    // 栈帧复用会导致错误命中（IC10 测试用例的 bug 根因）。
    const RegBytecodeChunk* cachedChunk = nullptr;
    const std::string* namePtr = &funName;
    if (!closureValue && !isMethodCall) {
        auto ccIt = callCache_.find(namePtr);
        if (ccIt != callCache_.end()) {
            cachedChunk = ccIt->second;
        }
    }

    // 查找函数 chunk
    auto it = cachedChunk ? functionChunks_.end() // 缓存命中，跳过 O(log n) 查找
                          : functionChunks_.find(funName);

    if (!cachedChunk && it == functionChunks_.end()) {
        // 函数名未命中 functionChunks_，尝试内建函数/类构造调用。
        // 提取到 tryCallBuiltinOrClass 子方法（原 ~154 行内联块，含 input/higher-order/
        // spawn/isBuiltinFunction/classInfo_ 五条路径，全部未命中返回"未定义的函数"错误）。
        return tryCallBuiltinOrClass(ip, funName, argCount, dstReg, argRegs, returnOffset);
    }

    // PERF-AUDIT-5: 缓存未命中时写入缓存（仅 REG_CALL 路径，funName 来自常量池）
    // R133 fix: 方法调用路径不写入,避免局部变量地址作 key 导致悬垂/错误命中。
    if (!cachedChunk && !closureValue && !isMethodCall) {
        callCache_[namePtr] = &it->second;
    }

    const RegBytecodeChunk& calleeChunk = cachedChunk ? *cachedChunk : it->second;

    // R164 协程/生成器：生成器函数调用拦截（fun* 声明的函数）
    // 命中 isGenerator 标志时不直接执行函数体，而是创建协程值写入 dstReg。
    // 与 StackVM::VM::createCoroutineValue 对称（VMCalls.cpp:1561）。
    // 关键：生成器调用不push新帧，必须手动推进 ip 越过 CALL 指令，
    // 否则主循环会反复执行同一条 CALL 指令导致无限循环。
    if (calleeChunk.isGenerator) {
        // Bug #48 fix: 先创建协程值，成功后再推进 ip。
        // 原顺序先 ip += returnOffset 再调用 createCoroutineValue，若后者内部
        // 返回错误（如参数打包/闭包捕获失败），ip 已被推进，错误恢复/诊断
        // 位置会偏移到 CALL 指令之后。生成器调用不 push 新帧，returnIp 机制
        // 不适用，必须手动推进 ip 越过 CALL 指令（否则主循环无限循环）。
        VMResult genResult = createCoroutineValue(calleeChunk, funName, argCount, dstReg, argRegs, closureValue);
        if (genResult == VMResult::VM_OK) {
            ip += returnOffset; // 推进 ip 到 CALL 指令之后
        }
        return genResult;
    }

    // C-2 fix: 从闭包值提取 upvalues（若有），填入新帧供 LOAD/STORE_UPVALUE 访问
    // W3-2-Bug2 fix: 方法调用时（closureData 为 null），从 methodUpvalues_ 读取
    // 在 REG_DEFINE_CLASS 时为函数内定义的类的方法捕获的 upvalue。
    // 提取到 populateCallFrameUpvalues 子方法（原 lambda，闭包/方法两路径合并）。

    // 检查参数数量
    // C-11 fix: 原条件 argCount < requiredArity 使 fillDefaultArgs 必返回 false（报错），
    // 而真正需要填默认值的区间 requiredArity <= argCount < arity 反而落入正常分支，
    // 导致默认参数寄存器保持 null（默认值表达式被静默丢弃）。改为 < arity。
    if (argCount < calleeChunk.arity) {
        // 尝试填充默认参数
        uint8_t adjustedArgCount = argCount;
        std::vector<Value> defaults;
        if (!fillDefaultArgs(calleeChunk, adjustedArgCount, funName, defaults)) {
            return runtimeError(formatParamError(isMethodCall, funName, argCount, calleeChunk), DiagCodes::kArityMismatch);
        }
        // 创建新帧
        RegCallFrame newFrame;
        newFrame.chunk = &calleeChunk;
        newFrame.ip = 0;
        newFrame.returnIp = ip + returnOffset; // C-8 fix: 用 returnOffset 替代硬编码 5+argCount
        newFrame.returnReg = dstReg;
        // #8 fix: 定长 array，仅记录激活数量
        newFrame.registerCount = static_cast<uint8_t>(calleeChunk.registerCount <= RegCallFrame::MAX_REGISTERS
                                                          ? calleeChunk.registerCount
                                                          : RegCallFrame::MAX_REGISTERS);
        // 填充参数
        for (uint8_t i = 0; i < argCount; ++i) {
            if (i < newFrame.registerCount) {
                newFrame.registers[i] = reg(argRegs[i]);
            }
        }
        for (uint8_t i = argCount; i < adjustedArgCount; ++i) {
            if (i < newFrame.registerCount && i - argCount < defaults.size()) {
                newFrame.registers[i] = defaults[i - argCount];
            }
        }
        populateCallFrameUpvalues(newFrame, closureData, funName, isMethodCall);
        frames_.push_back(std::move(newFrame));
        return VMResult::VM_OK;
    }

    // P1-1 fix: 缺失 argCount > arity 检查。栈式 VM (VMCalls.cpp:397-403) 有显式检查，
    // 原 RegisterVM 实现直接落入正常路径，虽然 for 循环的 i < newFrame.registerCount
    // 保护了寄存器不溢出，但语义错误——多出的参数被静默丢弃而非报错。
    if (argCount > calleeChunk.arity) {
        return runtimeError(formatParamError(isMethodCall, funName, argCount, calleeChunk), DiagCodes::kArityMismatch);
    }

    // 创建新帧
    RegCallFrame newFrame;
    newFrame.chunk = &calleeChunk;
    newFrame.ip = 0;
    newFrame.returnIp = ip + returnOffset; // C-8 fix: 用 returnOffset 替代硬编码 5+argCount
    newFrame.returnReg = dstReg;
    // #8 fix: 定长 array，仅记录激活数量
    newFrame.registerCount =
        static_cast<uint8_t>(calleeChunk.registerCount <= RegCallFrame::MAX_REGISTERS ? calleeChunk.registerCount
                                                                                      : RegCallFrame::MAX_REGISTERS);

    // 填充参数
    for (uint8_t i = 0; i < argCount && i < newFrame.registerCount; ++i) {
        newFrame.registers[i] = reg(argRegs[i]);
    }

    populateCallFrameUpvalues(newFrame, closureData, funName, isMethodCall);
    frames_.push_back(std::move(newFrame));
    return VMResult::VM_OK;
}

// ============================================================
// executeCallImpl 子阶段实现
// ============================================================

VMResult RegisterVM::tryCallBuiltinOrClass(size_t& ip, const std::string& funName, uint8_t argCount, uint8_t dstReg,
                                           const SmallArgs<uint8_t>& argRegs, size_t returnOffset) {
    // BUG-REGVM-3 fix: 删除 functionClosures_ 死代码路径。该字段无任何写入点
    // （仅 resetState clear、此处 find），是死代码。若被激活，REG_CALL 命中此路径
    // 时 closureData 为 null，populateUpvalues 不填充任何 upvalue，方法体内
    // REG_LOAD_UPVALUE/REG_STORE_UPVALUE 报"upvalue 索引越界"。
    // 闭包调用通过 REG_CALL_EXPR + MAKE_CLOSURE 正确处理（closureValue 非空）。

    // 收集参数（寄存器顺序: argRegs[0..argCount-1]）
    SmallArgs<Value> args;
    for (uint8_t i = 0; i < argCount; ++i) {
        args.push_back(reg(argRegs[i]));
    }
    // AUDIT-BUG-F9 fix: 传入实际源码行号，对齐 StackVM 路径（VMCalls.cpp:380-386）。
    const RegBytecodeChunk& curChunk = *currentFrame().chunk;
    int line = (ip < curChunk.lines.size()) ? curChunk.lines[ip] : 0;

    // BUG-IBACKEND-1 fix: input() 函数特殊处理（镜像 VMCalls.cpp:343-367）。
    // 原实现 inputCallback_ 字段仅在 setInputCallback 赋值，无任何调用点（死代码），
    // 导致 RegisterVM 路径下 input() 报"未定义的函数: input"。
    if (funName == "input") {
        auto r = executeSharedInput(inputCallback_, args.begin(), argCount, line, 0);
        if (r.is_err()) {
            return runtimeError(r.error().message);
        }
        reg(dstReg) = std::move(r.value());
        ip += returnOffset;
        return VMResult::VM_OK;
    }

    // R98 W2: 高阶函数 map/filter/reduce/forEach/find 优先拦截
    // （在 executeSharedBuiltinFunction 之前，因为这 5 个名字不在其注册表中，
    // 且需要通过 invokeClosureSync 调用用户传入的闭包值）。
    // 镜像 VMCalls.cpp:419-482 的 StackVM 拦截模式。
    if (isHigherOrderBuiltin(funName)) {
        // 构造闭包调用回调——dstReg 作为返回值中转寄存器（调用者已分配，
        // 闭包执行期间 caller 帧不执行指令，无副作用）
        ClosureInvoker invoke = [this, dstReg, line](const Value& closure, const Value* a, size_t ac,
                                                     int /*ln*/, int /*col*/) -> Result<Value> {
            Value res;
            VMResult r = invokeClosureSync(closure, a, ac, dstReg, line, 0, res);
            if (r != VMResult::VM_OK) {
                // P2-12: 从 diagnostics_ 派生错误消息（替代已移除的 lastError_）
                return Result<Value>::err(getLastError(), line, 0);
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
            hoResult = executeSharedMap(args[0], args[1], invoke, line, 0);
        } else if (funName == "filter") {
            if (argCount != 2) {
                return runtimeError(ErrorFormat::formatStd("filter 期望 2 个参数，但传入了 {} 个", argCount),
                                    DiagCodes::kArityMismatch);
            }
            hoResult = executeSharedFilter(args[0], args[1], invoke, line, 0);
        } else if (funName == "reduce") {
            if (argCount != 3) {
                return runtimeError(ErrorFormat::formatStd("reduce 期望 3 个参数，但传入了 {} 个", argCount),
                                    DiagCodes::kArityMismatch);
            }
            hoResult = executeSharedReduce(args[0], args[1], args[2], invoke, line, 0);
        } else if (funName == "forEach") {
            if (argCount != 2) {
                return runtimeError(ErrorFormat::formatStd("forEach 期望 2 个参数，但传入了 {} 个", argCount),
                                    DiagCodes::kArityMismatch);
            }
            hoResult = executeSharedForEach(args[0], args[1], invoke, line, 0);
        } else if (funName == "find") {
            if (argCount != 2) {
                return runtimeError(ErrorFormat::formatStd("find 期望 2 个参数，但传入了 {} 个", argCount),
                                    DiagCodes::kArityMismatch);
            }
            hoResult = executeSharedFind(args[0], args[1], invoke, line, 0);
        }
        if (hoResult.is_err()) {
            return runtimeError(hoResult.error().message);
        }
        reg(dstReg) = std::move(hoResult.value());
        ip += returnOffset;
        return VMResult::VM_OK;
    }

    // R136: spawn(fn, args...) — 需要后端注入 ClosureInvoker，走独立路径
    // 镜像 VMCalls.cpp executeCallSpawn 的 StackVM 拦截模式
    if (funName == "spawn") {
        if (argCount < 1) {
            return runtimeError("spawn 期望至少 1 个参数（函数），但传入了 0 个", DiagCodes::kArityMismatch);
        }
        // 构造闭包调用回调：通过 spawnMutex_ 序列化，避免寄存器帧数据竞争
        ClosureInvoker invoke = [this, line](const Value& closure, const Value* a, size_t ac, int /*ln*/,
                                             int /*col*/) -> Result<Value> {
            std::lock_guard<std::mutex> lock(spawnMutex_);
            Value res;
            // R136 fix: 保存调用者帧 reg(0)，因为 invokeClosureSync 会把闭包返回值
            // 写入 reg(dstReg=0)。若调用者帧 reg(0) 持有线程对象（如 spawn 返回的 t），
            // join() 路径会破坏线程对象导致后续 t.isJoinable() 报类型错误。
            // 闭包返回值已由 invokeClosureSync 末尾 result = reg(dstReg) 拷入 res，
            // 此处恢复 reg(0) 不影响 res 的正确性。
            Value savedReg0 = reg(0);
            VMResult r = invokeClosureSync(closure, a, ac, 0, line, 0, res);
            reg(0) = std::move(savedReg0);
            // P3-A1: VM_EXCEPTION_THROW 不是错误（与 StackVM invoker 对齐）。
            // 闭包内 throw 已被外层 try/catch 捕获，throwException 已就位 catchIp +
            // pendingException_ + 弹空 tryStack_ handler。返回 ok(null) 让
            // handleSyncObjectMethod 不抛、executeMethodCallImpl 通过 tryStack_ 缩小
            // 检测返回 VM_EXCEPTION_THROW 让主循环继续 catch 块的 REG_LOAD_EXCEPTION。
            if (r == VMResult::VM_EXCEPTION_THROW) {
                return Result<Value>::ok(Value::nullValue());
            }
            if (r != VMResult::VM_OK) {
                // P2-12: 从 diagnostics_ 派生错误消息（替代已移除的 lastError_）
                return Result<Value>::err(getLastError(), line, 0);
            }
            return Result<Value>::ok(std::move(res));
        };
        auto r = executeSharedSpawn(args[0], args.data() + 1, argCount - 1, invoke, line, 0);
        if (r.is_err()) {
            return runtimeError(r.error().message);
        }
        reg(dstReg) = std::move(r.value());
        ip += returnOffset;
        return VMResult::VM_OK;
    }

    // R164 fixup3: 用 isBuiltinFunction 过滤后再调用 executeSharedBuiltinFunction，
    // 并添加 is_err() 分支。原代码对所有未命中 functionChunks_ 的函数名都调用
    // executeSharedBuiltinFunction，但缺失 is_err() 检查，导致内置函数运行时错误
    // （如 int("hello") 的"无法将字符串转换为整数"）被吞掉，最终报"未定义的函数: int"。
    // 与 StackVM（VMCalls.cpp:312-314）的拦截顺序对齐：builtin → class → 未定义。
    if (isBuiltinFunction(funName)) {
        auto result = executeSharedBuiltinFunction(funName, args.data(), argCount, line, 0);
        if (result.is_err()) {
            return runtimeError(result.error().message);
        }
        reg(dstReg) = result.value();
        // C-8 fix: 内建函数同步返回，用调用者传入的 returnOffset 前进 ip
        ip += returnOffset;
        return VMResult::VM_OK;
    }
    // C-9 fix: 当函数名与已注册类名匹配时，视为类构造调用。
    // visitFunCall 对 ClassName() 语法发射普通 CALL，这里回退到 executeClassNewImpl，
    // 与旧 VM 在 executeCallOps 中检查 classInfo_ 的行为一致。
    auto classIt = classInfo_.find(funName);
    if (classIt != classInfo_.end()) {
        return executeClassNewImpl(ip, funName, argCount, dstReg, argRegs);
    }
    return runtimeError(ErrorFormat::formatStd(ErrorMessages::kUndefinedFunctionFmtStd, funName),
                        "undefined-function");
}

void RegisterVM::populateCallFrameUpvalues(RegCallFrame& newFrame, const std::shared_ptr<VMClosureData>& closureData,
                                           const std::string& funName, bool isMethodCall) {
    // C-2 fix: 从闭包值提取 upvalues（若有），填入新帧供 LOAD/STORE_UPVALUE 访问
    // W3-2-Bug2 fix: 方法调用时（closureData 为 null），从 methodUpvalues_ 读取
    // 在 REG_DEFINE_CLASS 时为函数内定义的类的方法捕获的 upvalue。
    if (closureData && !closureData->upvalues.empty()) {
        newFrame.upvalues = closureData->upvalues;
    } else if (isMethodCall) {
        auto uvIt = methodUpvalues_.find(funName);
        if (uvIt != methodUpvalues_.end() && uvIt->second) {
            newFrame.upvalues = uvIt->second->upvalues;
        }
    }
}

std::string RegisterVM::formatParamError(bool isMethodCall, const std::string& funName, uint8_t argCount,
                                         const RegBytecodeChunk& calleeChunk) const {
    // R164 fixup2: 方法/构造函数调用的参数计数错误消息对齐 StackVM。
    // RegisterVM 的 argCount/arity/requiredArity 包含 this（首个寄存器），
    // 但用户可见的参数计数不应包含 this。StackVM 的方法/构造函数路径
    // 均不将 this 计入参数（receiver 单独处理）。
    // 此方法在报错时减去 this（1），并使用 "构造函数 init"/"方法 X" 替代 "函数 ClassName.X"。
    int offset = isMethodCall ? 1 : 0;
    int userArgCount = std::max(0, static_cast<int>(argCount) - offset);
    int userRequired = std::max(0, static_cast<int>(calleeChunk.requiredArity) - offset);
    int userArity = std::max(0, static_cast<int>(calleeChunk.arity) - offset);
    if (isMethodCall) {
        auto dotPos = funName.rfind('.');
        std::string methodName = (dotPos != std::string::npos) ? funName.substr(dotPos + 1) : funName;
        if (methodName == "init") {
            return ErrorFormat::formatStd("构造函数 init 期望 {}-{} 个参数，但传入了 {} 个", userRequired, userArity,
                                          userArgCount);
        }
        return ErrorFormat::formatStd("方法 {} 期望 {}-{} 个参数，但传入了 {} 个", methodName, userRequired, userArity,
                                      userArgCount);
    }
    return ErrorFormat::formatStd("函数 {} 期望 {}-{} 个参数，但传入了 {} 个", funName,
                                  static_cast<int>(calleeChunk.requiredArity), static_cast<int>(calleeChunk.arity),
                                  static_cast<int>(argCount));
}

// ============================================================
// R98 W2: 高阶函数闭包同步调用
// ------------------------------------------------------------
// 镜像 VM::invokeClosureSync（VMCalls.cpp:1289）的寄存器版实现。
// 手动构造 RegCallFrame + 内部指令循环，执行闭包体直到帧弹出。
// 返回值通过 returnReg=dstReg 由 executeReturnImpl 写入调用者寄存器，
// 循环结束后从 reg(dstReg) 读取到 result。
//
// 关键不变量：
//   1. 调用前后 frames_.size() 不变（push 一次，pop 一次）
//   2. 调用前后 caller 的寄存器状态：仅 dstReg 被写入（作为闭包返回值中转）；
//      闭包执行期间 caller 帧不执行指令，dstReg 不被其他路径读取
//   3. DoS 防护：内部循环使用本地计数器，限制为 maxInstructions
//      （与外层 execute() 独立计数，总上限 2x maxInstructions）
//   4. 错误传播：hasError_ + diagnostics_ 由内部 executeOneInstruction 设置，
//      调用方检测 VM_RUNTIME_ERROR 后通过 getLastError() 获取错误信息（P2-12）
// ============================================================
VMResult RegisterVM::invokeClosureSync(const Value& closure, const Value* args, size_t argCount, uint8_t dstReg,
                                       int /*line*/, int /*column*/, Value& result) {
    if (!closure.isClosure()) {
        return runtimeError("高阶函数的参数必须是函数");
    }

    // 查找闭包 chunk（RegisterVM 的 VMClosureData 不设置 regChunkPtr，
    // 与 StackVM 不同——通过函数名查找 RegBytecodeChunk）
    const RegBytecodeChunk* targetChunkPtr = nullptr;
    auto chunkIt = functionChunks_.find(closure.closureName());
    if (chunkIt != functionChunks_.end()) {
        targetChunkPtr = &chunkIt->second;
    }
    if (!targetChunkPtr) {
        return runtimeError("未找到函数: " + closure.closureName(), "undefined-function");
    }
    const RegBytecodeChunk& targetChunk = *targetChunkPtr;

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

    // 检查 dstReg 在调用者帧中有效（用于接收返回值）
    if (frames_.empty() || dstReg >= currentFrame().registerCount) {
        return runtimeError("invokeClosureSync: dstReg 越界");
    }

    // 构造调用帧
    RegCallFrame newFrame;
    newFrame.chunk = &targetChunk;
    newFrame.ip = 0;
    newFrame.returnIp = 0;                         // 哨兵值——内部循环检测帧弹出，不依赖 returnIp
    newFrame.returnReg = static_cast<int>(dstReg); // 闭包返回值写入调用者 dstReg
    newFrame.registerCount =
        static_cast<uint8_t>(targetChunk.registerCount <= RegCallFrame::MAX_REGISTERS ? targetChunk.registerCount
                                                                                      : RegCallFrame::MAX_REGISTERS);
    // 填充参数
    uint8_t effectiveArgCount = static_cast<uint8_t>(argCount);
    for (uint8_t i = 0; i < effectiveArgCount && i < newFrame.registerCount; ++i) {
        newFrame.registers[i] = args[i];
    }
    // 填充默认参数
    if (effectiveArgCount < targetChunk.arity) {
        uint8_t missingCount = static_cast<uint8_t>(targetChunk.arity - effectiveArgCount);
        int defaultStartIdx = static_cast<int>(targetChunk.defaultConstIndices.size()) - missingCount;
        if (defaultStartIdx < 0) {
            return runtimeError("函数 " + closure.closureName() + " 默认参数索引越界");
        }
        for (int i = defaultStartIdx; i < defaultStartIdx + missingCount; ++i) {
            uint16_t constIdx = targetChunk.defaultConstIndices[i];
            if (constIdx == RuntimeLimits::NO_INDEX || constIdx >= targetChunk.constants.size()) {
                return runtimeError("函数 " + closure.closureName() + " 默认参数无效");
            }
            uint8_t regIdx = effectiveArgCount++;
            if (regIdx < newFrame.registerCount) {
                newFrame.registers[regIdx] = targetChunk.constants[constIdx];
            }
        }
    }
    // 填充 upvalues
    if (closure.vmClosure() && !closure.vmClosure()->upvalues.empty()) {
        newFrame.upvalues = closure.vmClosure()->upvalues;
    }

    size_t savedFrameCount = frames_.size();
    size_t savedTryStackSize = tryStack_.size(); // P3-A1: 快照 tryStack_ 检测异常穿透
    frames_.push_back(std::move(newFrame));

    // 内部指令循环：执行直到帧弹出
    // DoS 防护：本地计数器限制为 maxInstructions（与外层 execute() 独立计数）
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

    // P3-A1: 异常穿透检测（同 StackVM invokeClosureSync 逻辑）
    // spawn 闭包内 throw 触发 throwException 弹出闭包帧 + 弹出外层 try handler（tryStack_ 缩小）
    // + 设 main 帧 curFrame.ip = catchIp + 写 pendingException_。此时 frames_ 已退回
    // savedFrameCount，bomb 帧的 OP_RETURN 从未执行（reg(dstReg) 仍是 invoker 保存的
    // savedReg0 = t）。返回 VM_EXCEPTION_THROW 让调用方不操作 ip/寄存器，主循环保留
    // catchIp 继续 catch 块的 REG_LOAD_EXCEPTION。判别信号：spawn join 后输出"no-throw"
    // -> 检查此处是否漏掉穿透检测导致 ip 被 ip = newIp 覆盖。
    if (tryStack_.size() < savedTryStackSize) {
        return VMResult::VM_EXCEPTION_THROW;
    }

    // 闭包返回值已由 executeReturnImpl 写入 caller.registers[dstReg]
    result = reg(dstReg);
    return VMResult::VM_OK;
}

VMResult RegisterVM::executeReturnImpl(size_t& ip, Value result) {
    if (frames_.empty()) {
        return runtimeError("空帧返回");
    }

    int returnReg = frames_.back().returnReg;
    size_t returnIp = frames_.back().returnIp;
    bool wasMethodCall = frames_.back().isMethodCall;
    bool isInitCall = frames_.back().isInitCall;
    bool fieldsModified = frames_.back().fieldsModified;
    int receiverReg = frames_.back().receiverReg;
    std::string receiverVarName = std::move(frames_.back().receiverVarName);

    // CRITICAL-4 fix: 方法返回时记录 receiverReg 到 lastMutatedReceiverReg_，
    // 供 IR 路径中 visitMethodCall 的 LOAD_MUTATED + STORE 写回变异后接收者。
    // 对于只读方法，receiverReg 持有原值（未被修改），写回原值等于不写，安全。
    // 对于变异方法，executeReturnImpl 的字段同步已将变异后实例写入 receiverReg。
    // 必须在所有 wasMethodCall 路径设置，否则只读方法的 LOAD_MUTATED 会读到
    // 上一次设置的错误值，导致写回错误对象（回归：ClassMethodNoThis 等）。
    if (wasMethodCall && receiverReg >= 0) {
        lastMutatedReceiverReg_ = static_cast<uint8_t>(receiverReg);
    }

    // C-9/C-6 fix: 捕获方法帧的 this（slot 0）用于字段同步。
    // - init 方法：隐式返回 this 实例（而非 null），避免 caller 的 dstReg 被 null 覆盖。
    //   无论是否直接修改字段都需捕获——super.init 可能已通过字段同步更新了 this。
    // - 普通方法：仅当 fieldsModified 时捕获（避免不必要的拷贝），用于同步字段修改。
    // 原实现仅依赖 result.isInstance() 判断，导致返回非实例的方法修改 this 字段后被丢弃。
    Value methodThis;
    bool hasMethodThis = false;
    bool shouldCaptureThis = isInitCall || fieldsModified;
    if (wasMethodCall && shouldCaptureThis && frames_.back().registerCount > 0 &&
        frames_.back().registers[0].isInstance()) {
        methodThis = frames_.back().registers[0]; // copy（not move），frame 仍需 closeUpvalues
        hasMethodThis = true;
    }

    // C-9 fix: init 方法隐式返回 this 实例（而非 null）。
    if (isInitCall && !result.isInstance() && hasMethodThis) {
        result = methodThis;
    }

    size_t returningFrameIdx = frames_.size() - 1;
    // C-3 fix: 弹帧前关闭指向该帧寄存器的 open upvalues，捕获当前值到 uv->value，
    // 防止帧销毁后闭包持有悬垂引用。编码范围 [idx*32, (idx+1)*32) 覆盖该帧全部寄存器槽。
    // AUDIT-P2.5 fix: closeUpvaluesFrom 返回错误时立即终止返回，避免在损坏状态上继续
    if (closeUpvaluesFrom(returningFrameIdx * RegCallFrame::MAX_REGISTERS) != VMResult::VM_OK)
        return VMResult::VM_RUNTIME_ERROR;
    frames_.pop_back();

    // 清理属于返回帧的 tryStack_ handler
    while (!tryStack_.empty() && tryStack_.back().frameIndex >= returningFrameIdx) {
        tryStack_.pop_back();
    }

    // C-6 fix: 方法调用字段同步。
    // 原实现仅当 result.isInstance() 时同步，导致返回非实例值的方法（如 increment 返回数字）
    // 修改 this 字段后被丢弃。改为从 methodThis（方法帧 slot 0 的 this）同步字段。
    // 优先使用 methodThis（C-6），回退到 result（保留原行为供返回实例的方法使用）。
    // P0-1 fix: isInitCall 也需同步——多层 super.init() 链中中间层可能不直接写字段
    // (fieldsModified=false)，但父类 init 修改的 this 必须传播回 caller。
    if (wasMethodCall && (fieldsModified || isInitCall) && !frames_.empty()) {
        const Value* syncSource = nullptr;
        if (hasMethodThis) {
            syncSource = &methodThis;
        } else if (result.isInstance()) {
            syncSource = &result;
        }

        if (syncSource) {
            // Bug4 fix: 当 syncSource 是 methodThis 时，直接替换接收者 Value，
            // 避免 thisVal.fields() 触发 ensureUnique 深拷贝整个 InstanceData
            // （大字段类+循环方法调用场景的高频分配热点）。
            // methodThis 是方法帧 slot 0 的 COW 副本，已包含方法的字段修改，
            // 直接替换与逐字段同步语义等价（MiniLang 不支持字段删除）。
            // 当 syncSource 是 result（回退路径，方法返回实例）时，
            // 保留逐字段同步以维持原语义（result 可能是不同实例）。
            bool directReplace = hasMethodThis;

            // 同步字段到 caller 帧的接收者寄存器
            if (receiverReg >= 0 && static_cast<size_t>(receiverReg) < currentFrame().registerCount) {
                Value& thisVal = currentFrame().registers[receiverReg];
                if (thisVal.isInstance()) {
                    if (directReplace) {
                        thisVal = *syncSource; // 零 COW，仅指针+refCount 操作
                    } else {
                        auto& targetFields = thisVal.fields(); // ensureUnique 一次
                        for (const auto& field : syncSource->fields()) {
                            targetFields[field.first] = field.second;
                        }
                    }
                }
            }
            // 写回到接收者的原始位置（全局变量）
            if (!receiverVarName.empty()) {
                auto gsIt = globalNameToSlot_.find(receiverVarName);
                if (gsIt != globalNameToSlot_.end() && gsIt->second >= 0 && gsIt->second < static_cast<int>(globalSlots_.size())) {
                    Value& globalVal = globalSlots_[gsIt->second];
                    if (globalVal.isInstance()) {
                        if (directReplace) {
                            globalVal = *syncSource; // 零 COW
                        } else {
                            auto& targetFields = globalVal.fields(); // ensureUnique 一次
                            for (const auto& field : syncSource->fields()) {
                                targetFields[field.first] = field.second;
                            }
                        }
                    }
                } else {
                    // AUDIT-P1 fix: 对齐 StackVM VMCalls.cpp 的 globals_.find 回退分支。
                    // 当 globalNameToSlot_ 未命中时，尝试从 globals_ map 写回。
                    auto it = globals_.find(receiverVarName);
                    if (it != globals_.end() && it->second.isInstance()) {
                        if (directReplace) {
                            it->second = *syncSource;
                        } else {
                            auto& targetFields = it->second.fields();
                            for (const auto& field : syncSource->fields()) {
                                targetFields[field.first] = field.second;
                            }
                        }
                    }
                }
            }
        }
    }

    // 写入返回值到调用者的目标寄存器
    if (!frames_.empty() && returnReg >= 0) {
        if (static_cast<size_t>(returnReg) < currentFrame().registerCount) {
            currentFrame().registers[returnReg] = std::move(result);
        }
    } else if (returnReg < 0) {
        // R164 D.7 fix: 协程重放模式下生成器帧 returnReg=-1，返回值保存到邮箱供 callCoroutineNext 读取。
        // 对齐 StackVM 的 OP_RETURN 将返回值 push 到栈、callCoroutineNext pop 获取的语义。
        coroutineReturnValue_ = std::move(result);
    }

    // 恢复调用者 ip
    if (!frames_.empty()) {
        currentFrame().ip = returnIp;
        // W4 fix / P2 debug fix: 同步更新调用方传入的 ip 局部变量，使 stepCallback_
        // 在 REG_RETURN 后收到调用者的 ip（原实现仅更新 currentFrame().ip，
        // 导致 executeCalls 的 stepCallback_({ip, ...}) 读到 OP_RETURN 处的陈旧 ip）。
        ip = returnIp;
    }

    return VMResult::VM_OK;
}

VMResult RegisterVM::executeMethodCallImpl(size_t& ip, const std::string& methodName, uint8_t argCount, uint8_t dstReg,
                                           uint8_t objReg, const SmallArgs<uint8_t>& argRegs) {
    Value& obj = reg(objReg);
    SmallArgs<Value> args;
    for (uint8_t i = 0; i < argCount; ++i) {
        args.push_back(reg(argRegs[i]));
    }
    size_t savedTryStackSize = tryStack_.size();
    Value builtinResult;
    bool handled = callBuiltinMethod(obj, methodName, args, builtinResult);
    if (handled) {
        if (hasError_) return VMResult::VM_RUNTIME_ERROR;
        if (tryStack_.size() < savedTryStackSize) return VMResult::VM_EXCEPTION_THROW;
        lastMutatedReceiverReg_ = objReg;
        reg(dstReg) = std::move(builtinResult);
        ip += 6 + argCount;
        return VMResult::VM_OK;
    }
    if (obj.isInstance()) {
        const std::string& className = obj.className();
        auto classInfoIt = classInfo_.find(className);
        if (classInfoIt != classInfo_.end()) {
            auto cacheIt = classInfoIt->second.methodCache.find(methodName);
            if (cacheIt != classInfoIt->second.methodCache.end()) {
                const std::string& cachedFunName = cacheIt->second;
                if (cachedFunName.empty()) {
                    return runtimeError("类 " + className + " 没有方法 " + methodName, "undefined-function");
                }
                SmallArgs<uint8_t> fullArgRegs;
                fullArgRegs.push_back(objReg);
                for (uint8_t i = 0; i < argCount; ++i) fullArgRegs.push_back(argRegs[i]);
                size_t newIp = ip;
                if (argCount >= 255) return runtimeError("方法调用参数数量超过上限");
                VMResult cr = executeCallImpl(newIp, cachedFunName, argCount + 1, dstReg, fullArgRegs, 6u + argCount, nullptr, true);
                if (cr != VMResult::VM_OK) return cr;
                if (!frames_.empty()) {
                    frames_.back().isMethodCall = true;
                    frames_.back().isInitCall = (methodName == "init");
                    frames_.back().receiverReg = objReg;
                    frames_.back().receiverVarName.clear();
                }
                ip = newIp;
                return VMResult::VM_OK;
            }
        }
        std::string searchClass = className;
        std::string foundFunName;
        bool methodFound = false;
        for (int guard = 0; guard < 64 && !searchClass.empty(); ++guard) {
            auto classIt = classInfo_.find(searchClass);
            if (classIt == classInfo_.end()) break;
            auto methodIt = classIt->second.methods.find(methodName);
            if (methodIt != classIt->second.methods.end()) {
                foundFunName = methodIt->second;
                methodFound = true;
                break;
            }
            searchClass = classIt->second.parent;
        }
        if (classInfoIt != classInfo_.end()) {
            classInfoIt->second.methodCache[methodName] = methodFound ? foundFunName : std::string{};
        }
        if (!methodFound) {
            return runtimeError("类 " + className + " 没有方法 " + methodName, "undefined-function");
        }
        SmallArgs<uint8_t> fullArgRegs;
        fullArgRegs.push_back(objReg);
        for (uint8_t i = 0; i < argCount; ++i) fullArgRegs.push_back(argRegs[i]);
        size_t newIp = ip;
        if (argCount >= 255) return runtimeError("方法调用参数数量超过上限");
        VMResult cr = executeCallImpl(newIp, foundFunName, argCount + 1, dstReg, fullArgRegs, 6u + argCount, nullptr, true);
        if (cr != VMResult::VM_OK) return cr;
        if (!frames_.empty()) {
            frames_.back().isMethodCall = true;
            frames_.back().isInitCall = (methodName == "init");
            frames_.back().receiverReg = objReg;
            frames_.back().receiverVarName.clear();
        }
        ip = newIp;
        return VMResult::VM_OK;
    }
    if (obj.isNull()) {
        return runtimeError("不能在 null 值上访问属性或调用方法", DiagCodes::kNullAccess);
    }
    return runtimeError("类型 " + obj.typeName() + " 不支持方法 " + methodName);
}

VMResult RegisterVM::executeClosureImpl(size_t& ip, const std::string& name, uint8_t uvCount,
                                        const SmallArgs<uint8_t>& uvSpecs, uint8_t dstReg) {
    auto& current = currentFrame();
    size_t currentFrameIdx = frames_.size() - 1;
    auto upvalues = std::make_shared<VMClosureData>();
    upvalues->functionName = name;
    upvalues->upvalues.reserve(uvCount);
    for (uint8_t i = 0; i < uvCount; ++i) {
        uint8_t isLocal = uvSpecs[i * 2];
        uint8_t idx = uvSpecs[i * 2 + 1];
        if (isLocal) {
            auto uv = std::make_shared<VMUpvalue>();
            uv->isClosed = false;
            uv->stackSlot = currentFrameIdx * RegCallFrame::MAX_REGISTERS + idx;
            upvalues->upvalues.push_back(uv);
            // AUDIT-R5 R1 fix: 携带开启序号，供同帧 catch 命中时精确关闭 try 期间开启的 upvalue
            openUpvalues_.insert({uv->stackSlot, {std::weak_ptr<VMUpvalue>(uv), upvalueOpenSeq_++}});
        } else {
            if (idx < current.upvalues.size()) {
                upvalues->upvalues.push_back(current.upvalues[idx]);
            } else {
                auto uv = std::make_shared<VMUpvalue>();
                uv->isClosed = true;
                upvalues->upvalues.push_back(uv);
            }
        }
    }
    Value closureVal = Value::makeClosure(name, nullptr, {}, nullptr);
    closureVal.vmClosure() = upvalues;
    reg(dstReg) = std::move(closureVal);
    ip += 5 + uvCount * 2;
    return VMResult::VM_OK;
}

void RegisterVM::captureMethodUpvalues(const std::string& className) {
    auto classIt = classInfo_.find(className);
    if (classIt == classInfo_.end()) return;
    RegCallFrame& frame = currentFrame();
    size_t currentFrameIdx = frames_.size() - 1;
    for (const auto& [methodName, funName] : classIt->second.methods) {
        auto chunkIt = functionChunks_.find(funName);
        if (chunkIt == functionChunks_.end()) continue;
        const RegBytecodeChunk& chunk = chunkIt->second;
        if (chunk.upvalues.empty()) continue;
        auto closureData = std::make_shared<VMClosureData>();
        closureData->functionName = funName;
        closureData->upvalues.resize(chunk.upvalues.size());
        for (size_t i = 0; i < chunk.upvalues.size(); ++i) {
            const UpvalueDesc& desc = chunk.upvalues[i];
            if (desc.isLocal) {
                auto uv = std::make_shared<VMUpvalue>();
                uv->stackSlot = currentFrameIdx * RegCallFrame::MAX_REGISTERS + static_cast<size_t>(desc.index);
                uv->isClosed = false;
                closureData->upvalues[i] = uv;
                // AUDIT-R5 R1 fix: 携带开启序号（同 executeClosureImpl）
                openUpvalues_.insert({uv->stackSlot, {std::weak_ptr<VMUpvalue>(uv), upvalueOpenSeq_++}});
            } else {
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
        methodUpvalues_[funName] = closureData;
    }
}

VMResult RegisterVM::executeClassNewImpl(size_t& ip, const std::string& className, uint8_t argCount, uint8_t dstReg,
                                         const SmallArgs<uint8_t>& argRegs) {
    auto classIt = classInfo_.find(className);
    if (classIt == classInfo_.end()) {
        return runtimeError("未定义的类: " + className);
    }
    RegClassInfo& info = classIt->second;
    if (!info.flattenedComputed) {
        std::vector<std::string> chain;
        std::string cur = className;
        for (int guard = 0; guard < 64 && !cur.empty(); ++guard) {
            auto it = classInfo_.find(cur);
            if (it == classInfo_.end()) break;
            chain.push_back(cur);
            cur = it->second.parent;
        }
        if (!cur.empty()) {
            return runtimeError("类继承链过深或存在循环继承: " + className + " -> " + cur);
        }
        info.flattenedFieldOrder.clear();
        info.flattenedFieldDefaults.clear();
        std::unordered_map<std::string, size_t> fieldIndexMap;
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            auto clsIt = classInfo_.find(*it);
            if (clsIt != classInfo_.end()) {
                const auto& cls = clsIt->second;
                for (size_t i = 0; i < cls.fieldOrder.size(); ++i) {
                    const std::string& fieldName = cls.fieldOrder[i];
                    auto mapIt = fieldIndexMap.find(fieldName);
                    if (mapIt == fieldIndexMap.end()) {
                        fieldIndexMap[fieldName] = info.flattenedFieldOrder.size();
                        info.flattenedFieldOrder.push_back(fieldName);
                        if (i < cls.fieldDefaults.size()) {
                            info.flattenedFieldDefaults.push_back(cls.fieldDefaults[i]);
                        } else {
                            info.flattenedFieldDefaults.push_back(Value::nullValue());
                        }
                    } else {
                        if (i < cls.fieldDefaults.size()) {
                            info.flattenedFieldDefaults[mapIt->second] = cls.fieldDefaults[i];
                        }
                    }
                }
            }
        }
        info.hasInit = false;
        info.resolvedInitFunName.clear();
        std::string searchClass = className;
        for (int guard = 0; guard < 64 && !searchClass.empty(); ++guard) {
            auto clsIt = classInfo_.find(searchClass);
            if (clsIt == classInfo_.end()) break;
            auto mIt = clsIt->second.methods.find("init");
            if (mIt != clsIt->second.methods.end()) {
                info.resolvedInitFunName = mIt->second;
                info.hasInit = true;
                break;
            }
            searchClass = clsIt->second.parent;
        }
        info.flattenedComputed = true;
    }
    Value instance = Value::makeInstance(className);
    for (size_t i = 0; i < info.flattenedFieldOrder.size(); ++i) {
        instance.fields()[info.flattenedFieldOrder[i]] = info.flattenedFieldDefaults[i];
    }
    reg(dstReg) = instance;
    if (info.hasInit) {
        SmallArgs<uint8_t> fullArgRegs;
        fullArgRegs.push_back(dstReg);
        for (uint8_t i = 0; i < argCount; ++i) fullArgRegs.push_back(argRegs[i]);
        size_t newIp = ip;
        if (argCount >= 255) return runtimeError("类构造参数数量超过上限");
        VMResult r = executeCallImpl(newIp, info.resolvedInitFunName, argCount + 1, dstReg, fullArgRegs, 5u + argCount, nullptr, true);
        if (r != VMResult::VM_OK) return r;
        if (!frames_.empty()) {
            frames_.back().isMethodCall = true;
            frames_.back().isInitCall = true;
            frames_.back().receiverReg = dstReg;
        }
        ip = newIp;
    } else {
        if (argCount > 0) {
            return runtimeError(ErrorFormat::formatStd("类 {} 没有 init 方法，但传入了 {} 个参数", className, static_cast<int>(argCount)),
                                DiagCodes::kArityMismatch);
        }
        ip += 5 + argCount;
    }
    return VMResult::VM_OK;
}

bool RegisterVM::fillDefaultArgs(const RegBytecodeChunk& chunk, uint8_t& argCount, const std::string& /*funName*/,
                                 std::vector<Value>& defaults) {
    if (argCount >= chunk.arity) return true;
    if (argCount < chunk.requiredArity) return false;
    for (size_t i = argCount; i < chunk.defaultConstIndices.size() + chunk.requiredArity; ++i) {
        size_t defaultIdx = i - chunk.requiredArity;
        if (defaultIdx >= chunk.defaultConstIndices.size()) break;
        uint16_t constIdx = chunk.defaultConstIndices[defaultIdx];
        if (constIdx == RuntimeLimits::NO_INDEX) return false;
        if (constIdx >= chunk.constants.size()) return false;
        defaults.push_back(chunk.constants[constIdx]);
    }
    argCount = static_cast<uint8_t>(chunk.arity);
    return true;
}

// ============================================================
// 内建方法
// ============================================================

bool RegisterVM::callArrayBuiltinMethod(Value& obj, BuiltinMethod method, const std::string& methodName,
                                        SmallArgs<Value>& args, Value& result) {
    switch (method) {
    case BuiltinMethod::ARR_LEN: {
        auto r = executeSharedLen(obj, args.data(), args.size());
        if (r.is_ok()) { result = r.value(); return true; }
        runtimeError(r.error().message); return true;
    }
    case BuiltinMethod::ARR_PUSH:
        if (args.size() != 1) { runtimeError("push 需要 1 个参数", DiagCodes::kArityMismatch); return true; }
        obj.arrayVal().push_back(args[0]);
        result = Value::nullValue(); return true;
    case BuiltinMethod::ARR_POP: {
        auto& arr = obj.arrayVal();
        if (arr.empty()) { runtimeError("对空数组调用 pop"); return true; }
        result = arr.back(); arr.pop_back(); return true;
    }
    case BuiltinMethod::ARR_CONTAINS: {
        auto r = executeSharedArrayContains(obj, args.data(), args.size());
        if (r.is_ok()) { result = r.value(); return true; }
        runtimeError(r.error().message); return true;
    }
    case BuiltinMethod::ARR_JOIN: {
        auto r = executeSharedArrayJoin(obj, args.data(), args.size());
        if (r.is_ok()) { result = r.value(); return true; }
        runtimeError(r.error().message); return true;
    }
    case BuiltinMethod::ARR_REMOVE: {
        if (args.size() != 1) { runtimeError("remove 期望 1 个参数(索引)", DiagCodes::kArityMismatch); return true; }
        if (!args[0].isInt()) { runtimeError("remove 参数必须是整数索引", DiagCodes::kTypeMismatch); return true; }
        int64_t ri = args[0].intVal();
        auto& arr = obj.arrayVal();
        if (ri < 0 || static_cast<size_t>(ri) >= arr.size()) {
            // AUDIT-R5 BUG-07 fix: 补齐“有效范围”后缀，对齐 Interpreter/StackVM 统一格式。
            runtimeError(ErrorFormat::formatStd("数组索引越界: {}, 有效范围 [0, {})", static_cast<long long>(ri), arr.size()), DiagCodes::kIndexOutOfBounds);
            return true;
        }
        arr.erase(arr.begin() + static_cast<size_t>(ri));
        result = Value::nullValue(); return true;
    }
    default:
        runtimeError("数组没有方法 " + methodName, "undefined-function"); return true;
    }
}

bool RegisterVM::callDictBuiltinMethod(Value& obj, BuiltinMethod method, const std::string& methodName,
                                       SmallArgs<Value>& args, Value& result) {
    switch (method) {
    case BuiltinMethod::DICT_LEN:
    case BuiltinMethod::ARR_LEN: {
        auto r = executeSharedLen(obj, args.data(), args.size());
        if (r.is_ok()) { result = r.value(); return true; }
        runtimeError(r.error().message); return true;
    }
    case BuiltinMethod::DICT_KEYS: {
        auto r = executeSharedDictKeys(obj, args.data(), args.size());
        if (r.is_ok()) { result = r.value(); return true; }
        runtimeError(r.error().message); return true;
    }
    case BuiltinMethod::DICT_VALUES: {
        auto r = executeSharedDictValues(obj, args.data(), args.size());
        if (r.is_ok()) { result = r.value(); return true; }
        runtimeError(r.error().message); return true;
    }
    case BuiltinMethod::DICT_HAS: {
        auto r = executeSharedDictHas(obj, methodName, args.data(), args.size());
        if (r.is_ok()) { result = r.value(); return true; }
        runtimeError(r.error().message); return true;
    }
    case BuiltinMethod::DICT_GET: {
        auto r = executeSharedDictGet(obj, args.data(), args.size());
        if (r.is_ok()) { result = r.value(); return true; }
        runtimeError(r.error().message); return true;
    }
    case BuiltinMethod::DICT_REMOVE: {
        if (args.size() != 1) { runtimeError("remove 期望 1 个参数(键)", DiagCodes::kArityMismatch); return true; }
        auto dk = Value::dictKeyFromValue(args[0]);
        if (!dk) { runtimeError(ErrorMessages::kDictKeyInvalidType, DiagCodes::kTypeMismatch); return true; }
        obj.dictVal().erase(*dk);
        result = Value::nullValue(); return true;
    }
    case BuiltinMethod::DICT_SET: {
        if (args.size() != 2) { runtimeError("set 期望 2 个参数(键, 值)", DiagCodes::kArityMismatch); return true; }
        auto dk = Value::dictKeyFromValue(args[0]);
        if (!dk) { runtimeError(ErrorMessages::kDictKeyInvalidType, DiagCodes::kTypeMismatch); return true; }
        obj.dictVal()[*dk] = args[1];
        result = Value::nullValue(); return true;
    }
    default:
        runtimeError("字典没有方法 " + methodName, "undefined-function"); return true;
    }
}

bool RegisterVM::callStringBuiltinMethod(Value& obj, BuiltinMethod method, const std::string& methodName,
                                         SmallArgs<Value>& args, Value& result) {
    switch (method) {
    case BuiltinMethod::STR_LEN:
    case BuiltinMethod::ARR_LEN: {
        auto r = executeSharedLen(obj, args.data(), args.size());
        if (r.is_ok()) { result = r.value(); return true; }
        runtimeError(r.error().message); return true;
    }
    case BuiltinMethod::STR_UPPER: {
        auto r = executeSharedStrUpper(obj, args.data(), args.size());
        if (r.is_ok()) { result = r.value(); return true; }
        runtimeError(r.error().message); return true;
    }
    case BuiltinMethod::STR_LOWER: {
        auto r = executeSharedStrLower(obj, args.data(), args.size());
        if (r.is_ok()) { result = r.value(); return true; }
        runtimeError(r.error().message); return true;
    }
    case BuiltinMethod::STR_CONTAINS:
    case BuiltinMethod::ARR_CONTAINS: {
        auto r = executeSharedStrContains(obj, args.data(), args.size());
        if (r.is_ok()) { result = r.value(); return true; }
        runtimeError(r.error().message); return true;
    }
    case BuiltinMethod::STR_STARTS_WITH: {
        auto r = executeSharedStrStartsWith(obj, args.data(), args.size());
        if (r.is_ok()) { result = r.value(); return true; }
        runtimeError(r.error().message); return true;
    }
    case BuiltinMethod::STR_ENDS_WITH: {
        auto r = executeSharedStrEndsWith(obj, args.data(), args.size());
        if (r.is_ok()) { result = r.value(); return true; }
        runtimeError(r.error().message); return true;
    }
    case BuiltinMethod::STR_REPLACE: {
        auto r = executeSharedStrReplace(obj, args.data(), args.size());
        if (r.is_ok()) { result = r.value(); return true; }
        runtimeError(r.error().message); return true;
    }
    case BuiltinMethod::STR_SUBSTR: {
        auto r = executeSharedStrSubstr(obj, args.data(), args.size());
        if (r.is_ok()) { result = r.value(); return true; }
        runtimeError(r.error().message); return true;
    }
    case BuiltinMethod::STR_INDEX_OF: {
        auto r = executeSharedStrIndexOf(obj, args.data(), args.size());
        if (r.is_ok()) { result = r.value(); return true; }
        runtimeError(r.error().message); return true;
    }
    case BuiltinMethod::STR_SPLIT: {
        auto r = executeSharedStrSplit(obj, args.data(), args.size());
        if (r.is_ok()) { result = r.value(); return true; }
        runtimeError(r.error().message); return true;
    }
    case BuiltinMethod::STR_TRIM: {
        auto r = executeSharedStrTrim(obj, args.data(), args.size());
        if (r.is_ok()) { result = r.value(); return true; }
        runtimeError(r.error().message); return true;
    }
    default:
        runtimeError("字符串没有方法 " + methodName, "undefined-function"); return true;
    }
}

bool RegisterVM::callBuiltinMethod(Value& obj, const std::string& methodName, SmallArgs<Value>& args, Value& result) {
    BuiltinMethod method = classifyBuiltinMethod(methodName);
    if (obj.isChannel() || obj.isMutex() || obj.isRwLock() || obj.isThread()) {
        try {
            std::vector<Value> argsVec(args.begin(), args.end());
            auto br = handleSyncObjectMethod(methodName, obj, argsVec, 0, 0);
            result = std::move(br.result);
            return true;
        } catch (const RuntimeError& e) {
            // AUDIT-R6 B3 fix: 透传共享层的稳定诊断码（如 channel-closed），
            // 原实现丢弃 e.code，ErrorHintEngine 无法按码精确匹配（与 StackVM 对齐）
            runtimeError(e.what(), e.code);
            return true;
        }
    }
    if (obj.isCoroutine()) {
        return dispatchCoroutineBuiltin(obj, methodName, args, result);
    }
    if (method == BuiltinMethod::UNKNOWN) return false;
    if (obj.isArray()) return callArrayBuiltinMethod(obj, method, methodName, args, result);
    if (obj.isDict()) return callDictBuiltinMethod(obj, method, methodName, args, result);
    if (obj.isString()) return callStringBuiltinMethod(obj, method, methodName, args, result);
    if (obj.isInstance()) return false;
    runtimeError("类型 " + obj.typeName() + " 不支持方法 " + methodName);
    return true;
}

// ============================================================
// R164 协程/生成器
// ============================================================

VMResult RegisterVM::createCoroutineValue(const RegBytecodeChunk& genChunk, const std::string& funName,
                                          uint8_t argCount, uint8_t dstReg, const SmallArgs<uint8_t>& argRegs,
                                          const Value* closureValue) {
    std::vector<Value> args;
    args.reserve(argCount);
    for (uint8_t i = 0; i < argCount; ++i) args.push_back(reg(argRegs[i]));
    if (argCount < static_cast<uint8_t>(genChunk.arity)) {
        int missingCount = genChunk.arity - argCount;
        int defaultStartIdx = static_cast<int>(genChunk.defaultConstIndices.size()) - missingCount;
        if (defaultStartIdx < 0) return runtimeError("生成器 " + funName + " 默认参数索引越界");
        for (int i = defaultStartIdx; i < defaultStartIdx + missingCount; ++i) {
            uint16_t constIdx = genChunk.defaultConstIndices[i];
            if (constIdx == RuntimeLimits::NO_INDEX || constIdx >= genChunk.constants.size())
                return runtimeError("生成器 " + funName + " 默认参数无效");
            args.push_back(genChunk.constants[constIdx]);
        }
    }
    Value closureVal = closureValue ? *closureValue : Value::nullValue();
    Value coro = Value::makeCoroutineRegVM(&genChunk, std::move(closureVal), std::move(args), genChunk.yieldCount);
    reg(dstReg) = std::move(coro);
    return VMResult::VM_OK;
}

bool RegisterVM::dispatchCoroutineBuiltin(Value& obj, const std::string& methodName, SmallArgs<Value>& args,
                                          Value& result) {
    if (methodName == "next") {
        if (!args.empty()) { runtimeError("coroutine.next() 不接受参数", DiagCodes::kArityMismatch); return true; }
        result = callCoroutineNext(obj);
        // AUDIT-R7 F6 fix: 无条件返回 true（已处理）。原 `return !hasError_` 在生成器
        // 体报错时返回 false，executeMethodCallImpl 误入后续分发分支，最终用
        // "类型 coroutine 不支持方法 next" 覆盖真实错误消息（三后端错误文本分歧）。
        // hasError_ 由调用方 executeMethodCallImpl 的 hasError_ 检查正确处理。
        return true;
    }
    if (methodName == "done") {
        if (!args.empty()) { runtimeError("coroutine.done() 不接受参数", DiagCodes::kArityMismatch); return true; }
        auto* cd = obj.coroutineData();
        result = Value(cd->done);
        return true;
    }
    runtimeError("coroutine 类型不支持方法 " + methodName, "undefined-function");
    return true;
}

Value RegisterVM::callCoroutineNext(Value& coroVal) {
    auto* cd = coroVal.coroutineData();
    if (cd->done) {
        return cd->currentValueBox.empty() ? Value::nullValue() : cd->currentValueBox.front();
    }
    const RegBytecodeChunk* genChunk = cd->regChunk;
    if (!genChunk) { runtimeError("协程缺少生成器字节码块"); return Value::nullValue(); }
    if (frames_.size() >= MAX_FRAMES) {
        runtimeError(ErrorFormat::formatStd(ErrorMessages::kRecursionDepthExceededFmtStd, static_cast<int>(MAX_FRAMES)),
                     DiagCodes::kRecursionDepth);
        return Value::nullValue();
    }
    size_t savedFrameCount = frames_.size();
    int savedTargetYieldId = currentCoroutineTargetYieldId_;
    int savedYieldExecCount = currentYieldExecutionCount_;
    RegCallFrame newFrame;
    newFrame.chunk = genChunk;
    newFrame.ip = 0;
    newFrame.returnIp = currentFrame().ip;
    newFrame.returnReg = -1;
    newFrame.registerCount = static_cast<uint8_t>(
        genChunk->registerCount <= RegCallFrame::MAX_REGISTERS ? genChunk->registerCount : RegCallFrame::MAX_REGISTERS);
    uint8_t argCount = static_cast<uint8_t>(std::min(cd->args.size(), static_cast<size_t>(std::numeric_limits<uint8_t>::max())));
    for (uint8_t i = 0; i < argCount && i < newFrame.registerCount; ++i) newFrame.registers[i] = cd->args[i];
    if (argCount < static_cast<uint8_t>(genChunk->arity)) {
        int missingCount = genChunk->arity - argCount;
        int defaultStartIdx = static_cast<int>(genChunk->defaultConstIndices.size()) - missingCount;
        if (defaultStartIdx >= 0) {
            for (int i = defaultStartIdx; i < defaultStartIdx + missingCount; ++i) {
                uint8_t regIdx = static_cast<uint8_t>(argCount + (i - defaultStartIdx));
                if (regIdx >= newFrame.registerCount) break;
                uint16_t constIdx = genChunk->defaultConstIndices[i];
                if (constIdx != RuntimeLimits::NO_INDEX && constIdx < genChunk->constants.size())
                    newFrame.registers[regIdx] = genChunk->constants[constIdx];
            }
        }
    }
    for (uint8_t i = argCount; i < newFrame.registerCount; ++i) {
        if (i >= genChunk->arity) newFrame.registers[i] = Value::nullValue();
    }
    if (!cd->vmClosureBox.empty()) {
        auto& closureVal = cd->vmClosureBox.front();
        if (closureVal.isClosure() && closureVal.vmClosure()) newFrame.upvalues = closureVal.vmClosure()->upvalues;
    }
    frames_.push_back(std::move(newFrame));
    currentCoroutineTargetYieldId_ = cd->currentYieldId;
    currentYieldExecutionCount_ = 0;
    // AUDIT-R7 F2 fix: 快照 tryStack_ 检测异常穿透（生成器体内未捕获 throw 被外层
    // catch 捕获时，hasError_ 为 false 且帧数回落，原实现误入 normalReturn 分支：
    // 读取陈旧的 coroutineReturnValue_ 邮箱并误置 done=true）。
    size_t savedTryStackSize = tryStack_.size();
    Value result = Value::nullValue();
    bool needCleanup = false;
    bool normalReturn = false;
    try {
        int64_t localInstrCount = 0;
        int64_t maxInstr = RuntimeLimits::RuntimeConfig::instance().maxInstructions();
        while (frames_.size() > savedFrameCount && !hasError_) {
            if (++localInstrCount > maxInstr) { runtimeError("指令执行数超过上限，疑似无限循环"); break; }
            VMResult r = executeOneInstruction();
            if (r == VMResult::VM_EXCEPTION_THROW) continue;
            if (r != VMResult::VM_OK || hasError_) break;
        }
        if (hasError_) { needCleanup = true; }
        else if (tryStack_.size() < savedTryStackSize) {
            // AUDIT-R7 F2 fix: 异常穿透——不置 done、不读邮箱、不清理（throwException
            // 已就位 catch 状态）。executeMethodCallImpl 的 savedTryStackSize 检测
            // 会返回 VM_EXCEPTION_THROW 让主循环继续 catch 块。
            currentCoroutineTargetYieldId_ = savedTargetYieldId;
            currentYieldExecutionCount_ = savedYieldExecCount;
            return Value::nullValue();
        }
        else {
            result = std::move(coroutineReturnValue_);
            coroutineReturnValue_ = Value::nullValue();
            normalReturn = true;
        }
    } catch (const VMYieldSignal& e) {
        result = std::move(e.yieldValue);
        cd->currentValueBox.clear();
        cd->currentValueBox.push_back(result);
        cd->currentYieldId++;
        if (cd->currentYieldId >= cd->yieldCount) cd->done = true;
        needCleanup = true;
    }
    if (normalReturn) {
        cd->done = true;
        cd->currentValueBox.clear();
        cd->currentValueBox.push_back(result);
    }
    if (needCleanup) {
        while (frames_.size() > savedFrameCount) frames_.pop_back();
        while (!tryStack_.empty() && tryStack_.back().frameIndex >= savedFrameCount) tryStack_.pop_back();
        closeUpvaluesFrom(savedFrameCount * RegCallFrame::MAX_REGISTERS);
    }
    currentCoroutineTargetYieldId_ = savedTargetYieldId;
    currentYieldExecutionCount_ = savedYieldExecCount;
    return result;
}
