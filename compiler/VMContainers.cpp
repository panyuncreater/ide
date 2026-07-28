// ============================================================
// VMContainers.cpp - split from VM.cpp (C1 fix: reduce single-file complexity)
// Contains: container/member/writeback/misc instruction execution
// (executeContainerOps / executeWritebackOps / executeMiscOps)
// ============================================================

#include "common/BoundsCheck.h"   // Dedup-7A: inBounds 替代重复的索引检查
#include "common/ErrorFormat.h"   // P3 fix: runtimeErrorFmt 替代 std::to_string 拼接
#include "common/ErrorMessages.h" // R97 #1: 三后端共享错误消息常量
#include "common/Logger.h"
#include "common/TypeChecker.h" // 2026-06-29: typeMatchValue（OP_TYPE_CHECK）
#include "common/Utf8Utils.h"   // P0-4 fix: UTF-8 码位工具
#include "compiler/VM.h"
#include "interpreter/BuiltinMethods.h" // 共享纯函数层（len/contains/has）
#include "interpreter/NumericUtils.h"   // 共享溢出检查（B6 fix）
#include "interpreter/StringIntern.h"   // PERF-05 fix: 方法标记字符串驻留
#include <algorithm>
#include <cassert> // BUG-017 fix: OP_BUILD_DICT 栈深度断言
#include <climits>
#include <cmath> // BUG 8.1 fix: std::fmod
#include <cstdint>
#include <sstream>

// ============================================================
// 容器与成员操作类指令
// ------------------------------------------------------------
// 本文件三类指令共享两条贯穿始终的设计主线：
//   1) COW（写时拷贝）安全：数组/字典/实例的值以 shared_ptr 持有底层数据。
//      只读访问（pop 出的 const Value）用 const 重载读取，不触发深拷贝；
//      原地修改（arr[i]=v / dict[k]=v / obj.f=v）会经 arrayVal()/dictVal()/
//      fields() 的 ensureUnique 自动 detach 出新副本。因此"变异后的新容器"
//      与"调用者栈/全局变量中的旧引用"不再是同一份——必须把新容器写回原处，
//      否则修改对外不可见。这正是 lastMutatedReceiver_ 与 OP_WRITEBACK_* 的由来。
//   2) 嵌套左值写回链：对 `arr[i][j]=v`、`obj.f.g=v` 这类嵌套赋值，IR 路径把
//      每一级变异后的容器存入 lastMutatedReceiver_，由 OP_WRITEBACK_* 整体替换
//      回基对象（全局变量/栈槽/upvalue）。注意语义是"整体替换"而非"写子字段"，
//      否则会产生意外嵌套（见各 BUG-NEW 注释）。
// 成员/索引"读"指令(OP_*_GET)从栈弹出 obj 与下标/字段名，结果压栈；
// "写"指令(OP_*_SET)弹出 val 与 obj，就地修改后存入 lastMutatedReceiver_，
// 由后续写回指令完成对外可见的更新。
// ============================================================

// R118 重构：原 executeContainerOps 524 行单 switch 拆为 thin dispatcher + 4 个 helper。
// 拆分依据：原函数 case 平均 30+ 行、有明确语义分组（BUILD/INDEX/MEMBER/ENUM）、
// helper 有语义聚合（executeIndexOps = 索引访问语义族），非 R107 准则定义的扁平
// dispatch switch。与 R107 IR 层 lowerContainerOp 拆分模式同构。
VMResult VM::executeContainerOps(OpCode op, size_t& ip) {
    switch (op) {
    case OpCode::OP_BUILD_ARRAY:
    case OpCode::OP_BUILD_DICT:
    case OpCode::OP_BUILD_TUPLE:
    case OpCode::OP_BUILD_ENUM_VARIANT:
        return executeContainerBuildOps(op, ip);
    case OpCode::OP_INDEX_GET:
    case OpCode::OP_INDEX_SET:
    case OpCode::OP_INDEX_SET_VAR:
    case OpCode::OP_INDEX_SET_LOCAL:
        return executeIndexOps(op, ip);
    case OpCode::OP_SUPER_MEMBER_GET:
    case OpCode::OP_MEMBER_GET:
    case OpCode::OP_MEMBER_SET:
    case OpCode::OP_MEMBER_SET_VAR:
    case OpCode::OP_MEMBER_SET_LOCAL:
        return executeMemberOps(op, ip);
    case OpCode::OP_ENUM_VARIANT_NAME:
    case OpCode::OP_ENUM_VARIANT_FIELD:
        return executeEnumOps(op, ip);
    default:
        return runtimeError(ErrorFormat::formatStd("未知操作码: {}",  static_cast<int>(op)));
    }
}

// ============================================================
// executeContainerBuildOps - 容器构造指令
// OP_BUILD_ARRAY / OP_BUILD_DICT / OP_BUILD_TUPLE / OP_BUILD_ENUM_VARIANT
// ============================================================
VMResult VM::executeContainerBuildOps(OpCode op, size_t& ip) {
    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;

    switch (op) {
    case OpCode::OP_BUILD_ARRAY: {
        uint8_t count = chunk.code[ip + 1];
        if (stack_.size() < count)
            return runtimeError("栈下溢: OP_BUILD_ARRAY");
        std::vector<Value> elements(count);
        // 逆序弹出直接填入预分配槽位，无需 reverse
        for (int i = count - 1; i >= 0; --i) {
            elements[i] = pop();
        }
        push(Value(std::move(elements)));
        notifyStep(ip, op);
        ip += 2;
        break;
    }

    case OpCode::OP_BUILD_DICT: {
        uint8_t pairCount = chunk.code[ip + 1];
        if (stack_.size() < static_cast<size_t>(pairCount) * 2)
            return runtimeError("栈下溢: OP_BUILD_DICT");
        // BUG-017 fix (2026-07-18): 记录入口栈深度，循环结束后断言
        // 栈净变化 = -2*pairCount + 1（弹出全部键值对，压入一个 dict）。
        const size_t stackDepthBefore = stack_.size();
        // L4 fix: 字典键支持 string/int/bool/float，使用 DictKey map
        // R97 #2 fix: 改用 Value::DictMap（含 DictKeyEqual 透明比较器）
        Value::DictMap dict;
        dict.reserve(pairCount);
        // 先入后出：倒序弹出键值对
        for (uint8_t i = 0; i < pairCount; ++i) {
            Value val = pop();
            Value key = pop();
            // L4 fix: 键必须是 string/int/bool/float
            auto dk = Value::dictKeyFromValue(key);
            if (!dk) {
                // BUG-VM-04 fix: 错误返回前清理栈上剩余未处理的键值对
                popN(static_cast<size_t>(pairCount - i - 1) * 2);
                return runtimeError(ErrorMessages::kDictKeyInvalidType);
            }
            dict.emplace(std::move(*dk), std::move(val));
        }
        push(Value(std::move(dict)));
        // BUG-017 fix: 防御性断言——成功路径栈净变化必须为 -2*pairCount + 1。
        assert(stack_.size() == stackDepthBefore - static_cast<size_t>(pairCount) * 2 + 1);
        notifyStep(ip, op);
        ip += 2;
        break;
    }

    case OpCode::OP_BUILD_TUPLE: {
        // R98 元组与解构：构建 immutable 元组
        uint8_t count = chunk.code[ip + 1];
        if (stack_.size() < count)
            return runtimeError("栈下溢: OP_BUILD_TUPLE");
        std::vector<Value> elements(count);
        for (int i = count - 1; i >= 0; --i) {
            elements[i] = pop();
        }
        push(Value::makeTuple(std::move(elements)));
        notifyStep(ip, op);
        ip += 2;
        break;
    }

    case OpCode::OP_BUILD_ENUM_VARIANT: {
        // 操作数: enumNameConstIdx(2B) + variantNameConstIdx(2B) + argCount(1B)
        // 栈布局（编译器按声明顺序 push 参数，最后一个参数在栈顶）：
        //   [..., arg0, arg1, ..., argN-1]  (argCount = N)
        // 消费 argCount 个参数，构造 EnumVariantData(enumName, variantName, fields) push。
        uint16_t enumIdx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        uint16_t varIdx = chunk.code[ip + 3] | (chunk.code[ip + 4] << 8);
        uint8_t argCount = chunk.code[ip + 5];
        if (enumIdx >= chunk.constants.size() || varIdx >= chunk.constants.size()) {
            return runtimeError("OP_BUILD_ENUM_VARIANT: 常量池索引越界");
        }
        if (stack_.size() < argCount) {
            return runtimeError("栈下溢: OP_BUILD_ENUM_VARIANT");
        }
        const std::string& enumName = chunk.constants[enumIdx].stringVal();
        const std::string& variantName = chunk.constants[varIdx].stringVal();

        // R99 enum 校验：运行时检查 enum 已声明、variant 存在、参数 arity 一致。
        // 对齐 Interpreter::visitEnumVariantExpr 的校验逻辑，保证三后端一致。
        auto enumIt = enumRegistry_.find(enumName);
        if (enumIt == enumRegistry_.end()) {
            return runtimeError(ErrorFormat::formatStd("未定义的 enum: {}",  enumName), "undefined-function");
        }
        const VMEnumInfo& info = enumIt->second;
        const VMEnumVariantInfo* varInfo = nullptr;
        for (const auto& v : info.variants) {
            if (v.name == variantName) {
                varInfo = &v;
                break;
            }
        }
        if (varInfo == nullptr) {
            return runtimeError(
                ErrorFormat::formatStd("enum '{}' 没有 variant '{}'",  enumName,  variantName),
                "undefined-function");
        }
        if (static_cast<int>(argCount) != varInfo->arity) {
            return runtimeError(ErrorFormat::formatStd("enum variant '{}.{}' 期望 {} 个参数，得到 {} 个",  enumName,
                                                    
                                                    variantName,  varInfo->arity,  static_cast<int>(argCount)),
                                DiagCodes::kArityMismatch);
        }

        std::vector<Value> fields(argCount);
        for (int i = argCount - 1; i >= 0; --i) {
            fields[i] = pop();
        }
        // AUDIT-R6 F3 fix: 字段类型注解校验（对齐 Interpreter::visitEnumVariantExpr）。
        // 原实现仅校验 arity，E.V("s") 对 V(int) 静默构造成功——与 Interpreter 报错不一致。
        // 泛型类型参数运行时擦除（isTypeParameter 豁免）；实例继承链 fallback 对齐
        // executeMiscTypeCheck 的 classInfo_ 查找模式。
        for (size_t fi = 0; fi < fields.size() && fi < varInfo->paramTypes.size(); ++fi) {
            const std::string& expectedType = varInfo->paramTypes[fi];
            if (expectedType.empty() || minilang::isTypeParameter(expectedType, info.typeParams))
                continue;
            if (!minilang::typeMatchValue(fields[fi], expectedType)) {
                bool inheritOk = false;
                if (fields[fi].isInstance()) {
                    auto classIt = classInfo_.find(fields[fi].className());
                    int depth = 0;
                    while (classIt != classInfo_.end() && depth < 64) {
                        if (classIt->second.name == expectedType) {
                            inheritOk = true;
                            break;
                        }
                        if (classIt->second.superClassName.empty())
                            break;
                        classIt = classInfo_.find(classIt->second.superClassName);
                        ++depth;
                    }
                }
                if (!inheritOk) {
                    return runtimeError(
                        ErrorFormat::formatStd("enum variant '{}.{}' 第 {} 个参数类型不匹配：期望 {}，得到 {}",
                                               enumName, variantName, fi + 1, expectedType, fields[fi].typeName()),
                        DiagCodes::kTypeMismatch);
                }
            }
        }
        push(Value::makeEnumVariant(enumName, variantName, std::move(fields)));
        notifyStep(ip, op);
        ip += 6;
        break;
    }

    default:
        return runtimeError(ErrorFormat::formatStd("未知操作码: {}",  static_cast<int>(op)));
    }

    return VMResult::VM_OK;
}

// ============================================================
// executeIndexOps - 索引访问指令（thin dispatcher）
// R122 重构：原 executeIndexOps 223 行单 switch 拆为 thin dispatcher + 4 个独立 helper。
// 拆分依据：与 R118/R119/R120 模式同属 VM 类 execute*Ops 系列 + 共享状态全成员，但采用
// "每 case 独立 helper"模式（比 R118/R119/R120 的"按语义分组多 case 到一个 helper"更细粒度）。
// 判别依据：4 个 case 之间无共享逻辑——GET 是 pop+push 读路径，SET 是 pop+lastMutatedReceiver_
// 嵌套赋值，SET_VAR 是 resolveMutableGlobal 全局变量路径，SET_LOCAL 是 stack_[bp+slot] 局部
// 路径含 fieldsModified 同步。case 数量 ≤4 且每 case >30 行时适合"每 case 独立 helper"。
// OP_INDEX_GET / OP_INDEX_SET / OP_INDEX_SET_VAR / OP_INDEX_SET_LOCAL
// ============================================================
VMResult VM::executeIndexOps(OpCode op, size_t& ip) {
    switch (op) {
    case OpCode::OP_INDEX_GET:
        return executeIndexGet(ip);
    case OpCode::OP_INDEX_SET:
        return executeIndexSet(ip);
    case OpCode::OP_INDEX_SET_VAR:
        return executeIndexSetVar(ip);
    case OpCode::OP_INDEX_SET_LOCAL:
        return executeIndexSetLocal(ip);
    default:
        return runtimeError(ErrorFormat::formatStd("未知操作码: {}",  static_cast<int>(op)));
    }
}

// ============================================================
// executeIndexGet - 索引读取（OP_INDEX_GET，跨 array/dict/string/tuple 多态）
// 含 ASCII 快速路径 + UTF-8 慢路径（与 R97 #3 fix 一致）
// ============================================================
VMResult VM::executeIndexGet(size_t& ip) {
    if (stack_.size() < 2)
        return runtimeError("栈下溢: OP_INDEX_GET");
    Value idx = pop();
    // V-P2-15 fix: obj 只读不写，改为 const 避免 arrayVal/dictVal/stringVal 触发 COW 深拷贝
    const Value obj = pop();
    if (obj.isArray() && idx.isInt()) {
        int64_t i = idx.intVal();
        if (BoundsCheck::inBounds(i, obj.arrayVal().size())) {
            push(obj.arrayVal()[static_cast<size_t>(i)]);
        } else {
            return runtimeError(ErrorFormat::formatStd("数组索引越界: {}, 有效范围 [0, {})",  static_cast<long long>(i),
                                                    
                                                    obj.arrayVal().size()),
                                DiagCodes::kIndexOutOfBounds);
        }
    } else if (obj.isDict()) {
        // L4 fix: 字典键支持 string/int/bool/float
        auto dk = Value::dictKeyFromValue(idx);
        if (!dk)
            return runtimeError(ErrorMessages::kDictKeyInvalidType, DiagCodes::kTypeMismatch);
        auto it = obj.dictVal().find(*dk);
        if (it != obj.dictVal().end()) {
            push(it->second);
        } else {
            push(Value::nullValue()); // 字典访问不存在的键返回 null（与解释器一致）
        }
    } else if (obj.isArray()) {
        return runtimeError(ErrorMessages::kArrayIndexMustBeInt);
    } else if (obj.isString() && idx.isInt()) {
        // V3 fix: 基于 UTF-8 码位索引，与解释器 M6 fix 一致
        int64_t i = idx.intVal();
        const std::string& s = obj.stringVal();

        // P7 fix: ASCII 快速路径 — 纯 ASCII 字符串直接按字节索引 O(1)
        // R97 #3 fix: 用 StringData 内缓存的 isAscii 标志替代 lastAsciiStr* 4 字段缓存。
        // isAscii 与字符串生命周期绑定，O(1) 读取，无堆地址复用误命中风险。
        if (BoundsCheck::inBounds(i, s.size()) && obj.isAsciiString()) {
            push(Value(s.substr(static_cast<size_t>(i), 1)));
            notifyStep(ip, OpCode::OP_INDEX_GET);
            ip += 1;
            return VMResult::VM_OK; // 跳过慢路径，直接完成 OP_INDEX_GET
        }

        size_t charCount = 0;
        size_t bytePos = 0;
        size_t targetBytePos = 0;
        size_t targetByteLen = 0;
        bool found = false;
        while (bytePos < s.size()) {
            size_t charLen = Utf8::byteLength(static_cast<unsigned char>(s[bytePos]));
            if (static_cast<size_t>(i) == charCount) {
                targetBytePos = bytePos;
                targetByteLen = charLen;
                found = true;
            }
            bytePos += charLen;
            charCount++;
        }
        if (i < 0 || !found) {
            return runtimeError(ErrorFormat::formatStd("字符串索引越界: {}, 有效范围 [0, {})",
                                                    
                                                    static_cast<long long>(i),  static_cast<long long>(charCount)),
                                DiagCodes::kIndexOutOfBounds);
        }
        push(Value(s.substr(targetBytePos, targetByteLen)));
    } else if (obj.isString()) {
        return runtimeError(ErrorMessages::kStringIndexMustBeInt, DiagCodes::kTypeMismatch);
    } else if (obj.isDict()) {
        // R97 #1 fix: 对齐 RegisterVM/Interpreter——dict 非 string 索引原落入通用
        // ErrorMessages::kTypeNotIndexable 分支，现显式报字典键类型错误。
        // L4 fix 遗漏的旧字符串 "字典键必须是字符串" 已修正为统一消息。
        return runtimeError(ErrorMessages::kDictKeyInvalidType, DiagCodes::kTypeMismatch);
    } else if (obj.isTuple() && idx.isInt()) {
        // R98 元组与解构：元组索引访问（immutable，与数组语义一致）
        int64_t i = idx.intVal();
        if (BoundsCheck::inBounds(i, obj.tupleVal().size())) {
            push(obj.tupleVal()[static_cast<size_t>(i)]);
        } else {
            return runtimeError(ErrorFormat::formatStd("元组索引越界: {}, 有效范围 [0, {})",  static_cast<long long>(i),
                                                    
                                                    obj.tupleVal().size()),
                                DiagCodes::kIndexOutOfBounds);
        }
    } else if (obj.isTuple()) {
        return runtimeError("元组索引必须是整数", DiagCodes::kTypeMismatch);
    } else {
        return runtimeError(ErrorMessages::kTypeNotIndexable);
    }
    notifyStep(ip, OpCode::OP_INDEX_GET);
    ip += 1;
    return VMResult::VM_OK;
}

// ============================================================
// executeIndexSet - 嵌套索引赋值（OP_INDEX_SET，栈顶 obj+idx+val）
// M1 fix: 用于 arr[i][j] = val 形态，pop obj 后存入 lastMutatedReceiver_
// ============================================================
VMResult VM::executeIndexSet(size_t& ip) {
    // M1 fix: 嵌套索引赋值（如 arr[i][j] = val）
    // 栈序: [..., obj, outerIdx, innerIdx, val]
    // 弹出 val, innerIdx, obj → 修改 obj[innerIdx] → 存入 lastMutatedReceiver_
    if (stack_.size() < 3)
        return runtimeError("栈下溢: OP_INDEX_SET");
    Value val = pop();
    Value innerIdx = pop();
    Value obj = pop();
    if (obj.isArray() && innerIdx.isInt()) {
        int64_t i = innerIdx.intVal();
        // Perf-Finding: 越界错误路径用 std::as_const 避免 COW detach（pop 出的 obj 与原栈槽共享 ArrayData）
        if (BoundsCheck::inBounds(i, std::as_const(obj).arrayVal().size())) {
            obj.arrayVal()[static_cast<size_t>(i)] = val;
        } else {
            return runtimeError(ErrorFormat::formatStd("数组索引越界: {}, 有效范围 [0, {})",  static_cast<long long>(i),
                                                    
                                                    std::as_const(obj).arrayVal().size()),
                                DiagCodes::kIndexOutOfBounds);
        }
    } else if (obj.isDict()) {
        // L4 fix: 字典键支持 string/int/bool/float
        auto dk = Value::dictKeyFromValue(innerIdx);
        if (!dk)
            return runtimeError(ErrorMessages::kDictKeyInvalidType, DiagCodes::kTypeMismatch);
        obj.dictVal()[*dk] = val;
    } else if (obj.isArray()) {
        return runtimeError(ErrorMessages::kArrayIndexMustBeInt, DiagCodes::kTypeMismatch);
    } else {
        return runtimeError(ErrorMessages::kTypeNotIndexAssignable);
    }
    lastMutatedReceiver_ = std::move(obj);
    notifyStep(ip, OpCode::OP_INDEX_SET);
    ip += 1;
    return VMResult::VM_OK;
}

// ============================================================
// executeIndexSetVar - 全局变量索引赋值（OP_INDEX_SET_VAR）
// 通过 resolveMutableGlobal 取全局变量引用直接修改（A2 Dedup-7B）
// ============================================================
VMResult VM::executeIndexSetVar(size_t& ip) {
    const BytecodeChunk& chunk = *currentFrame().chunk;
    // 直接修改全局变量中的数组/字典元素
    uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
    if (idx >= chunk.constants.size())
        return runtimeError("常量池索引越界");
    const std::string& varName = chunk.constants[idx].stringVal();
    if (stack_.size() < 2)
        return runtimeError("栈下溢: OP_INDEX_SET_VAR");
    Value val = pop();
    Value index = pop();
    // A2: Dedup-7B resolveMutableGlobal 统一全局变量解析（先 globalSlots_ 再 globals_）
    Value* objPtr = resolveMutableGlobal(varName);
    if (!objPtr)
        return runtimeError("未定义的变量: " + varName, DiagCodes::kUndefinedVariable);
    Value& obj = *objPtr; // 引用，直接修改
    if (obj.isArray() && index.isInt()) {
        int64_t i = index.intVal();
        // Perf-Finding: 越界错误路径用 std::as_const 避免 COW detach（全局数组 refCount 常 >1）
        if (BoundsCheck::inBounds(i, std::as_const(obj).arrayVal().size())) {
            obj.arrayVal()[static_cast<size_t>(i)] = val;
        } else {
            return runtimeError(ErrorFormat::formatStd("数组索引越界: {}, 有效范围 [0, {})",  static_cast<long long>(i),
                                                    
                                                    std::as_const(obj).arrayVal().size()),
                                DiagCodes::kIndexOutOfBounds);
        }
    } else if (obj.isDict()) {
        // L4 fix: 字典键支持 string/int/bool/float
        auto dk = Value::dictKeyFromValue(index);
        if (!dk)
            return runtimeError(ErrorMessages::kDictKeyInvalidType, DiagCodes::kTypeMismatch);
        obj.dictVal()[*dk] = val;
    } else if (obj.isArray()) {
        return runtimeError(ErrorMessages::kArrayIndexMustBeInt, DiagCodes::kTypeMismatch);
    } else {
        return runtimeError(ErrorMessages::kTypeNotIndexAssignable);
    }
    notifyStep(ip, OpCode::OP_INDEX_SET_VAR);
    ip += 3;
    return VMResult::VM_OK;
}

// ============================================================
// executeIndexSetLocal - 局部变量索引赋值（OP_INDEX_SET_LOCAL）
// 修改 stack_[bp+slot] 中的数组/字典元素，含 fieldsModified 标记同步（BUG-INH-AUDIT-4 fix）
// ============================================================
VMResult VM::executeIndexSetLocal(size_t& ip) {
    const BytecodeChunk& chunk = *currentFrame().chunk;
    // 直接修改 stack_[bp+slot] 中的数组/字典元素（用于方法内 this.arr[i] = val）
    uint8_t slot = chunk.code[ip + 1];
    if (stack_.size() < 2)
        return runtimeError("栈下溢: OP_INDEX_SET_LOCAL");
    Value val = pop();
    Value index = pop();
    size_t bp = currentFrame().basePointer;
    if (bp + slot >= stack_.size()) {
        return runtimeError("内部错误: 局部变量槽越界");
    }
    Value& obj = stack_[bp + slot]; // 栈引用，直接修改
    // BUG-INH-AUDIT-4 fix: slot==0 谓词对 OP_INDEX_SET_LOCAL 是死代码——slot 0 是
    // this 实例（非数组/字典），数组/字典分支永不命中。正确谓词应与 OP_SET_LOCAL
    // 对齐：slot 在字段范围 [1, fieldOrder.size()] 时标记 fieldsModified，确保
    // this.arr[i]=val 的 COW detach 后字段同步回 this.fields()。
    bool isFieldSlot = (slot > 0 && currentFrame().chunk && slot <= currentFrame().chunk->fieldOrder.size());
    if (obj.isArray() && index.isInt()) {
        int64_t i = index.intVal();
        // Perf-Finding: 越界错误路径用 std::as_const 避免 COW detach（栈槽 obj 来自 this.arr 时 refCount 常 >1）
        if (BoundsCheck::inBounds(i, std::as_const(obj).arrayVal().size())) {
            obj.arrayVal()[static_cast<size_t>(i)] = val;
            if (isFieldSlot)
                currentFrame().fieldsModified = true;
        } else {
            return runtimeError(ErrorFormat::formatStd("数组索引越界: {}, 有效范围 [0, {})",  static_cast<long long>(i),
                                                    
                                                    std::as_const(obj).arrayVal().size()),
                                DiagCodes::kIndexOutOfBounds);
        }
    } else if (obj.isDict()) {
        // L4 fix: 字典键支持 string/int/bool/float
        auto dk = Value::dictKeyFromValue(index);
        if (!dk)
            return runtimeError(ErrorMessages::kDictKeyInvalidType, DiagCodes::kTypeMismatch);
        obj.dictVal()[*dk] = val;
        if (isFieldSlot)
            currentFrame().fieldsModified = true;
    } else if (obj.isArray()) {
        return runtimeError(ErrorMessages::kArrayIndexMustBeInt, DiagCodes::kTypeMismatch);
    } else {
        return runtimeError(ErrorMessages::kTypeNotIndexAssignable);
    }
    notifyStep(ip, OpCode::OP_INDEX_SET_LOCAL);
    ip += 2;
    return VMResult::VM_OK;
}

// ============================================================
// executeMemberOps - 成员访问指令
// OP_SUPER_MEMBER_GET / OP_MEMBER_GET / OP_MEMBER_SET / OP_MEMBER_SET_VAR / OP_MEMBER_SET_LOCAL
// ============================================================
VMResult VM::executeMemberOps(OpCode op, size_t& ip) {
    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;

    switch (op) {
    case OpCode::OP_SUPER_MEMBER_GET:
    case OpCode::OP_MEMBER_GET: {
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (idx >= chunk.constants.size())
            return runtimeError("常量池索引越界");
        const std::string& fieldName = chunk.constants[idx].stringVal();
        // V-P2-16 fix: obj 只读不写，改为 const 避免 fields/dictVal 触发 COW 深拷贝
        const Value obj = pop();
        if (obj.isInstance()) {
            auto it = obj.fields().find(fieldName);
            if (it != obj.fields().end()) {
                push(it->second);
            } else {
                // V-P2 fix: 字段未找到时查找方法（与 Interpreter visitMemberAccess 一致），
                // 返回 "method:ClassName.methodName" 标记字符串，而非直接报错。
                // 对于 super.field，字段在构造时已合并到实例，方法查找也从当前类开始即可
                // （super.method() 调用走 OP_SUPER_CALL 直接指定起始类，不经过此路径）
                const BytecodeChunk* methodChunk = findMethodChunk(obj.className(), fieldName);
                if (methodChunk != nullptr) {
                    // PERF-05 fix: 驻留 "method:Class.field" 字符串，避免每次成员访问重复构造。
                    // 同一类+方法的成员访问频繁发生（如 obj.x 多次读取），驻留后池中复用同一 std::string。
                    push(Value(StringIntern::internConcat(StringIntern::internConcat("method:", obj.className()),
                                                          std::string(".") + fieldName)));
                } else {
                    return runtimeError("类 " + obj.className() + " 没有字段或方法 '" + fieldName + "'");
                }
            }
        } else if (obj.isDict()) {
            // L4 fix: member access on dict wraps fieldName as DictKey{string}
            auto it = obj.dictVal().find(Value::DictKey{fieldName});
            if (it != obj.dictVal().end()) {
                push(it->second);
            } else {
                push(Value::nullValue());
            }
        } else if (obj.isNull()) {
            // P2 fix (null-access): null 值成员访问给出明确的 null-access 诊断码
            return runtimeError("不能在 null 值上访问属性或调用方法", DiagCodes::kNullAccess);
        } else {
            return runtimeError(ErrorMessages::kTypeNotMemberAccessible);
        }
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    case OpCode::OP_MEMBER_SET: {
        // M1 fix: 嵌套成员赋值（如 obj.field.subfield = val 或 arr[i].field = val）
        // 栈序: [..., obj, val]
        // 弹出 val, obj → 修改 obj.field → 存入 lastMutatedReceiver_
        if (stack_.size() < 2)
            return runtimeError("栈下溢: OP_MEMBER_SET");
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (idx >= chunk.constants.size())
            return runtimeError("常量池索引越界");
        const std::string& fieldName = chunk.constants[idx].stringVal();
        Value val = pop();
        Value obj = pop();
        if (obj.isInstance()) {
            obj.fields()[fieldName] = val;
        } else if (obj.isDict()) {
            // L4 fix: 字典 member-set 用 DictKey{string}
            obj.dictVal()[Value::DictKey{fieldName}] = val;
        } else {
            return runtimeError(ErrorMessages::kTypeNotMemberAssignable);
        }
        lastMutatedReceiver_ = std::move(obj);
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    case OpCode::OP_MEMBER_SET_VAR: {
        // 直接修改全局变量.fields()[fieldName]
        uint16_t varIdx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        uint16_t fieldIdx = chunk.code[ip + 3] | (chunk.code[ip + 4] << 8);
        if (varIdx >= chunk.constants.size() || fieldIdx >= chunk.constants.size())
            return runtimeError("常量池索引越界");
        const std::string& varName = chunk.constants[varIdx].stringVal();
        const std::string& fieldName = chunk.constants[fieldIdx].stringVal();
        Value val = pop();
        // A2: Dedup-7B resolveMutableGlobal 统一全局变量解析
        Value* objPtr = resolveMutableGlobal(varName);
        if (!objPtr)
            return runtimeError("未定义的变量: " + varName, DiagCodes::kUndefinedVariable);
        Value& obj = *objPtr; // 引用，直接修改
        if (obj.isInstance()) {
            obj.fields()[fieldName] = val;
        } else if (obj.isDict()) {
            // L4 fix: 字典 member-set-var 用 DictKey{string}
            obj.dictVal()[Value::DictKey{fieldName}] = val;
        } else {
            return runtimeError(ErrorMessages::kTypeNotMemberAssignable);
        }
        notifyStep(ip, op);
        ip += 5;
        break;
    }

    case OpCode::OP_MEMBER_SET_LOCAL: {
        // 直接修改 stack_[bp+slot].fields()[fieldName]（用于方法内 this.field = val）
        uint8_t slot = chunk.code[ip + 1];
        uint16_t fieldIdx = chunk.code[ip + 2] | (chunk.code[ip + 3] << 8);
        if (fieldIdx >= chunk.constants.size())
            return runtimeError("常量池索引越界");
        const std::string& fieldName = chunk.constants[fieldIdx].stringVal();
        Value val = pop();
        size_t bp = currentFrame().basePointer;
        if (bp + slot >= stack_.size()) {
            return runtimeError("内部错误: 局部变量槽越界");
        }
        Value& obj = stack_[bp + slot]; // 栈引用，直接修改
        if (obj.isInstance()) {
            obj.fields()[fieldName] = val;
            // VM fix: 标记字段已修改，OP_RETURN 可跳过只读方法的字段同步
            if (slot == 0)
                currentFrame().fieldsModified = true;
            // 当 slot==0（写 this.field）时，也需同步更新对应的字段槽
            // 否则 OP_RETURN 会用字段槽的旧值覆盖 this.fields()，导致 this.field 赋值丢失
            if (slot == 0 && currentFrame().chunk && !currentFrame().chunk->fieldOrder.empty()) {
                // #11 fix: 用 BytecodeChunk::fieldSlotIndex O(1) 查找替代线性扫描
                size_t fi = currentFrame().chunk->fieldSlotIndex(fieldName);
                if (fi != SIZE_MAX) {
                    size_t slotPos = bp + 1 + fi;
                    if (slotPos < stack_.size()) {
                        stack_[slotPos] = val;
                    }
                }
            }
        } else if (obj.isDict()) {
            // L4 fix: 字典 member-set-local 用 DictKey{string}
            obj.dictVal()[Value::DictKey{fieldName}] = val;
        } else {
            return runtimeError(ErrorMessages::kTypeNotMemberAssignable);
        }
        notifyStep(ip, op);
        ip += 4;
        break;
    }

    default:
        return runtimeError(ErrorFormat::formatStd("未知操作码: {}",  static_cast<int>(op)));
    }

    return VMResult::VM_OK;
}

// ============================================================
// executeEnumOps - 枚举查询指令
// OP_ENUM_VARIANT_NAME / OP_ENUM_VARIANT_FIELD
// ============================================================
VMResult VM::executeEnumOps(OpCode op, size_t& ip) {
    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;

    switch (op) {
    case OpCode::OP_ENUM_VARIANT_NAME: {
        // 操作数: enumNameConstIdx(2B) + variantNameConstIdx(2B)
        // 栈: [..., scrutinee] → pop scrutinee，检查是否为 enum variant 且 enum/variant 名匹配，
        // push bool。不匹配（包括非 enum variant 类型）push false。
        uint16_t enumIdx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        uint16_t varIdx = chunk.code[ip + 3] | (chunk.code[ip + 4] << 8);
        if (enumIdx >= chunk.constants.size() || varIdx >= chunk.constants.size()) {
            return runtimeError("OP_ENUM_VARIANT_NAME: 常量池索引越界");
        }
        if (stack_.empty()) {
            return runtimeError("栈下溢: OP_ENUM_VARIANT_NAME");
        }
        Value scrut = pop();
        const std::string& enumName = chunk.constants[enumIdx].stringVal();
        const std::string& variantName = chunk.constants[varIdx].stringVal();
        bool matched = false;
        if (scrut.isEnumVariant()) {
            matched = (scrut.enumVariantEnumName() == enumName) && (scrut.enumVariantName() == variantName);
        }
        push(Value(matched));
        notifyStep(ip, op);
        ip += 5;
        break;
    }

    case OpCode::OP_ENUM_VARIANT_FIELD: {
        // 无操作数。栈: [..., scrutinee, index] → pop index, pop scrutinee,
        // push scrutinee.fields[index]。scrutinee 非 enum variant 或 index 越界 → runtimeError。
        if (stack_.size() < 2) {
            return runtimeError("栈下溢: OP_ENUM_VARIANT_FIELD");
        }
        Value idx = pop();
        Value scrut = pop();
        if (!scrut.isEnumVariant()) {
            return runtimeError("OP_ENUM_VARIANT_FIELD: 栈顶不是 enum variant");
        }
        if (!idx.isInt()) {
            return runtimeError("OP_ENUM_VARIANT_FIELD: 索引必须是整数");
        }
        int64_t i = idx.intVal();
        const auto& fields = scrut.enumVariantFields();
        if (i < 0 || static_cast<size_t>(i) >= fields.size()) {
            return runtimeError(ErrorFormat::formatStd("OP_ENUM_VARIANT_FIELD: 索引越界 {}, 有效范围 [0, {})",
                                                    
                                                    static_cast<long long>(i),  fields.size()),
                                DiagCodes::kIndexOutOfBounds);
        }
        push(fields[static_cast<size_t>(i)]);
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    default:
        return runtimeError(ErrorFormat::formatStd("未知操作码: {}",  static_cast<int>(op)));
    }

    return VMResult::VM_OK;
}

// ============================================================
// 嵌套访问写回类指令
// ============================================================

VMResult VM::executeWritebackOps(OpCode op, size_t& ip) {
    // R132-D fix: 按"写回目标"分组到 3 个 helper（每个 helper 处理 MEMBER+INDEX 两 op），
    // 原函数 200 行 → thin dispatcher ~15 行 + 3 helper。
    // MEMBER 与 INDEX 在同一存储类下语义等价（BUG-NEW fix 后均为整体替换 lastMutatedReceiver_），
    // 区别仅在操作数编码（MEMBER 多 2B fieldIdx 用于反汇编）和 ip 增量。
    switch (op) {
    case OpCode::OP_WRITEBACK_MEMBER_VAR:
    case OpCode::OP_WRITEBACK_INDEX_VAR:
        return writebackToGlobalVar(op, ip);
    case OpCode::OP_WRITEBACK_MEMBER_LOCAL:
    case OpCode::OP_WRITEBACK_INDEX_LOCAL:
        return writebackToStackSlot(op, ip);
    case OpCode::OP_WRITEBACK_MEMBER_UPVALUE:
    case OpCode::OP_WRITEBACK_INDEX_UPVALUE:
        return writebackToUpvalue(op, ip);
    default:
        return runtimeError(ErrorFormat::formatStd("未知操作码: {}",  static_cast<int>(op)));
    }
}

// ============================================================
// R132-D fix: executeWritebackOps 拆分 helper
// ============================================================

VMResult VM::writebackToGlobalVar(OpCode op, size_t& ip) {
    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;
    bool isMember = (op == OpCode::OP_WRITEBACK_MEMBER_VAR);

    // 操作数: varIdx(2B) [+ fieldIdx(2B) 仅 MEMBER]
    uint16_t varIdx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);

    if (isMember) {
        // BUG-NEW fix: 语义对齐 RegisterVM 的 REG_WRITEBACK_MEMBER_VAR ——
        // 将 lastMutatedReceiver_（MEMBER_SET 产生的变异后整个容器）整体替换全局变量，
        // 而非写入 obj.field。原实现 obj.field = lastMutatedReceiver_ 导致
        // d.x = 42 把整个变异后 d 赋给 d["x"]，产生嵌套字典。
        uint16_t fieldIdx = chunk.code[ip + 3] | (chunk.code[ip + 4] << 8);
        if (varIdx >= chunk.constants.size() || fieldIdx >= chunk.constants.size())
            return runtimeError("常量池索引越界");
        const std::string& varName = chunk.constants[varIdx].stringVal();
        // fieldIdx 仅用于反汇编/调试，运行时不需要（整体替换语义）
        (void)chunk.constants[fieldIdx];
        // A2: Dedup-7B resolveMutableGlobal 统一全局变量解析
        Value* objPtr = resolveMutableGlobal(varName);
        if (!objPtr) {
            lastMutatedReceiver_ = Value::nullValue();
            return runtimeError("未定义的变量: " + varName, DiagCodes::kUndefinedVariable);
        }
        *objPtr = std::move(lastMutatedReceiver_);
        lastMutatedReceiver_ = Value::nullValue();
        notifyStep(ip, op);
        ip += 5;
        return VMResult::VM_OK;
    }

    // OP_WRITEBACK_INDEX_VAR
    // BUG-NEW fix: 语义对齐 RegisterVM 的 REG_WRITEBACK_INDEX_VAR ——
    // 整体替换全局变量为 lastMutatedReceiver_，不 pop 索引。
    // 原实现 pop 索引后做 obj[index] = mutated，但 IR 路径不向栈推入索引，
    // 导致栈下溢；且语义应为整体替换而非写入 obj[index]。
    if (varIdx >= chunk.constants.size()) {
        lastMutatedReceiver_ = Value::nullValue();
        return runtimeError("常量池索引越界");
    }
    const std::string& varName = chunk.constants[varIdx].stringVal();
    // A2: Dedup-7B resolveMutableGlobal 统一全局变量解析
    Value* objPtr = resolveMutableGlobal(varName);
    if (!objPtr) {
        lastMutatedReceiver_ = Value::nullValue();
        return runtimeError("未定义的变量: " + varName, DiagCodes::kUndefinedVariable);
    }
    *objPtr = std::move(lastMutatedReceiver_);
    lastMutatedReceiver_ = Value::nullValue();
    notifyStep(ip, op);
    ip += 3;
    return VMResult::VM_OK;
}

VMResult VM::writebackToStackSlot(OpCode op, size_t& ip) {
    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;
    bool isMember = (op == OpCode::OP_WRITEBACK_MEMBER_LOCAL);

    // 操作数: slot(1B) [+ fieldIdx(2B) 仅 MEMBER]
    uint8_t slot = chunk.code[ip + 1];

    if (isMember) {
        // BUG-NEW fix: 语义对齐 RegisterVM —— 整体替换栈槽为 lastMutatedReceiver_，
        // 而非写入 obj.field。原实现 obj.field = mutated 导致局部 d.x = 42 把整个
        // 变异后 d 赋给 d["x"]。IR 路径不使用字段槽（fieldSlot），故移除 slot==0
        // 的字段槽同步逻辑（该逻辑仅服务于 Compiler.cpp 非 IR 路径，但本指令仅由
        // IR 路径发射，不会冲突）。
        //
        // BUG-INH-AUDIT-4 fix: 若 slot 是字段槽（this.field 的字段位置），
        // 标记 fieldsModified=true，确保 OP_RETURN 时字段同步回 this 实例。
        // 否则 this.obj.field = val 的 COW detach 后字段不会写回 this.fields()。
        uint16_t fieldIdx = chunk.code[ip + 2] | (chunk.code[ip + 3] << 8);
        if (fieldIdx >= chunk.constants.size())
            return runtimeError("常量池索引越界");
        // fieldIdx 仅用于反汇编/调试，运行时不需要（整体替换语义）
        (void)chunk.constants[fieldIdx];
        size_t bp = currentFrame().basePointer;
        if (bp + slot >= stack_.size()) {
            lastMutatedReceiver_ = Value::nullValue();
            return runtimeError("OP_WRITEBACK_MEMBER_LOCAL: 栈槽越界");
        }
        stack_[bp + slot] = std::move(lastMutatedReceiver_);
        lastMutatedReceiver_ = Value::nullValue();
        // BUG-INH-AUDIT-4 fix: 字段槽写回需标记 fieldsModified
        if (slot > 0 && currentFrame().chunk && slot <= currentFrame().chunk->fieldOrder.size()) {
            currentFrame().fieldsModified = true;
        }
        notifyStep(ip, op);
        ip += 4;
        return VMResult::VM_OK;
    }

    // OP_WRITEBACK_INDEX_LOCAL
    // BUG-NEW fix: 同 OP_WRITEBACK_INDEX_VAR，整体替换栈槽，不 pop 索引。
    // BUG-INH-AUDIT-4 fix: 若 slot 是字段槽（this.arr[i]=val 的字段位置），
    // 标记 fieldsModified=true，确保 OP_RETURN 时字段同步回 this 实例。
    // 否则 OP_INDEX_SET 的 COW detach 后新数组虽写回 stack_[bp+slot]，
    // 但 this.fields() 仍指向旧数组，OP_RETURN 不会同步。
    size_t bp = currentFrame().basePointer;
    if (bp + slot >= stack_.size()) {
        lastMutatedReceiver_ = Value::nullValue();
        return runtimeError("OP_WRITEBACK_INDEX_LOCAL: 栈槽越界");
    }
    stack_[bp + slot] = std::move(lastMutatedReceiver_);
    lastMutatedReceiver_ = Value::nullValue();
    // BUG-INH-AUDIT-4 fix: 字段槽写回需标记 fieldsModified
    if (slot > 0 && currentFrame().chunk && slot <= currentFrame().chunk->fieldOrder.size()) {
        currentFrame().fieldsModified = true;
    }
    notifyStep(ip, op);
    ip += 2;
    return VMResult::VM_OK;
}

VMResult VM::writebackToUpvalue(OpCode op, size_t& ip) {
    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;
    bool isMember = (op == OpCode::OP_WRITEBACK_MEMBER_UPVALUE);

    // 操作数: uvIdx(1B) [+ fieldIdx(2B) 仅 MEMBER，运行时不读取]
    uint8_t uvIdx = chunk.code[ip + 1];

    // 注意：原 MEMBER_UPVALUE 实现不检查 fieldIdx 越界（fieldIdx 仅用于反汇编），
    // 与 MEMBER_VAR/MEMBER_LOCAL 行为不同。此处保留原行为。
    if (static_cast<size_t>(uvIdx) >= frame.upvalues.size()) {
        lastMutatedReceiver_ = Value::nullValue();
        return runtimeError(isMember ? "OP_WRITEBACK_MEMBER_UPVALUE: upvalue 索引越界"
                                     : "OP_WRITEBACK_INDEX_UPVALUE: upvalue 索引越界");
    }
    auto& uv = frame.upvalues[uvIdx];
    if (uv->isClosed) {
        uv->value = std::move(lastMutatedReceiver_);
    } else {
        if (uv->stackSlot >= stack_.size()) {
            lastMutatedReceiver_ = Value::nullValue();
            return runtimeError(isMember ? "OP_WRITEBACK_MEMBER_UPVALUE: upvalue 栈槽越界"
                                         : "OP_WRITEBACK_INDEX_UPVALUE: upvalue 栈槽越界");
        }
        stack_[uv->stackSlot] = std::move(lastMutatedReceiver_);
        // P0-3 fix: 与 OP_SET_UPVALUE 对齐——若修改的是某外层帧的字段槽，
        // 标记该帧 fieldsModified，确保 OP_RETURN 时字段同步回 this 实例。
        // 闭包内 `capturedField.subfield = v`（MEMBER）或 `capturedArr[i] = v`（INDEX）
        // 的变异必须传播回 this 实例。
        if (uv->owningFrameIdx < frames_.size()) {
            VMCallFrame& of = frames_[uv->owningFrameIdx];
            if (of.chunk && !of.chunk->fieldOrder.empty()) {
                size_t fieldStart = of.basePointer + 1;
                size_t fieldEnd = fieldStart + of.chunk->fieldOrder.size();
                if (uv->stackSlot >= fieldStart && uv->stackSlot < fieldEnd) {
                    of.fieldsModified = true;
                }
            }
        }
    }
    lastMutatedReceiver_ = Value::nullValue();
    notifyStep(ip, op);
    ip += isMember ? 4 : 2;
    return VMResult::VM_OK;
}

// ============================================================
// 其他指令（输出、栈操作、跳转、字段初始化）
// ============================================================

// R131 重构：原 executeMiscOps 218 行单函数拆为 thin dispatcher + 4 个 helper。
// 拆分模式：按语义分组多 case 到一个 helper（延续 R118/R119/R120 模式）。
// 4 个 helper 按"栈操作/类型检查/跳转/异常处理"四类语义分组：
//   · executeMiscStackOps: 7 个 case（PRINT/POP/DUP/SWAP/LOAD_MUTATED/DUP_N/INIT_FIELD）均围绕栈顶操作
//   · executeMiscTypeCheck: 1 个 case（TYPE_CHECK）独立复杂，含实例继承链 64 层查找
//   · executeMiscJumpOps: 3 个 case（JUMP/JUMP_IF_FALSE/LOOP）共享 jump 目标越界检查模式
//   · executeMiscExceptionOps: 5 个 case 围绕 tryStack_/pendingJumpStack_ 两个异常机制栈管理
// thin dispatcher 按 OpCode 直接分发，frame/chunk 由各 helper 内部获取（与 R118/R122 helper 风格一致）。
VMResult VM::executeMiscOps(OpCode op, size_t& ip) {
    switch (op) {
    case OpCode::OP_PRINT:
    case OpCode::OP_POP:
    case OpCode::OP_DUP:
    case OpCode::OP_SWAP:
    case OpCode::OP_LEN:
    case OpCode::OP_LOAD_MUTATED:
    case OpCode::OP_DUP_N:
    case OpCode::OP_INIT_FIELD:
        return executeMiscStackOps(op, ip);
    case OpCode::OP_TYPE_CHECK:
        return executeMiscTypeCheck(op, ip);
    case OpCode::OP_TYPE_TEST: // R134: 软类型测试（push bool 而非抛错）
        return executeMiscTypeTest(op, ip);
    case OpCode::OP_JUMP:
    case OpCode::OP_JUMP_IF_FALSE:
    case OpCode::OP_LOOP:
        return executeMiscJumpOps(op, ip);
    case OpCode::OP_TRY_BEGIN:
    case OpCode::OP_TRY_END:
    case OpCode::OP_THROW:
    case OpCode::OP_PUSH_JUMP_TARGET:
    case OpCode::OP_FINALLY_END:
        return executeMiscExceptionOps(op, ip);
    default:
        return runtimeError(ErrorFormat::formatStd("未知操作码: {}",  static_cast<int>(op)));
    }
}

// ============================================================
// executeMiscStackOps - 栈操作 + 字段初始化指令（7 个 case）
// 围绕栈顶元素操作：消费（POP/PRINT）、复制（DUP/DUP_N）、交换（SWAP）、
// 读取变异接收者（LOAD_MUTATED）、字段写入栈顶实例（INIT_FIELD）
// ============================================================
VMResult VM::executeMiscStackOps(OpCode op, size_t& ip) {
    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;
    switch (op) {
    case OpCode::OP_PRINT: {
        Value val = pop();
        // S5 fix: 字符串类型直接用 stringVal() 引用，避免 toString() 拷贝
        outputCallback_(val.isString() ? val.stringVal() : val.toString());
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_POP:
        pop();
        notifyStep(ip, op);
        ip += 1;
        break;

    case OpCode::OP_DUP:
        push(peek(0));
        notifyStep(ip, op);
        ip += 1;
        break;

    case OpCode::OP_SWAP: {
        // R99 match 表达式：交换栈顶两个值
        if (stack_.size() < 2)
            return runtimeError("栈下溢: OP_SWAP");
        // 仅交换 top 和 top-1（通过 std::swap_refs 或手动）
        Value tmp = std::move(stack_[stack_.size() - 1]);
        stack_[stack_.size() - 1] = std::move(stack_[stack_.size() - 2]);
        stack_[stack_.size() - 2] = std::move(tmp);
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_LEN: {
        // R134 模式匹配扩展：获取容器长度（array/dict/string/tuple）
        if (stack_.empty())
            return runtimeError("栈下溢: OP_LEN");
        const Value& v = peek(0);
        int64_t len = 0;
        if (v.isArray()) {
            len = static_cast<int64_t>(v.arrayVal().size());
        } else if (v.isDict()) {
            len = static_cast<int64_t>(v.dictVal().size());
        } else if (v.isString()) {
            // 字符串长度按 codepoint 计算（与 RegisterVM REG_LEN / print/字符串索引语义一致）
            len = static_cast<int64_t>(v.codepointCount());
        } else if (v.isTuple()) {
            len = static_cast<int64_t>(v.tupleVal().size());
        } else {
            return runtimeError("OP_LEN: 操作数必须是 array/dict/string/tuple，实际为 " + v.typeName());
        }
        pop();
        push(Value(len));
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_LOAD_MUTATED: {
        // MEDIUM-1/2 fix: 读取 lastMutatedReceiver_ 到栈顶（不清除）。
        // 嵌套左值写回链中，上一级 MEMBER_SET/INDEX_SET 将变异后容器存入
        // lastMutatedReceiver_，此处读取供下一级 SET 作为 val 使用。
        push(lastMutatedReceiver_);
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_DUP_N: {
        uint8_t depth = chunk.code[ip + 1];
        if (depth >= stack_.size()) {
            return runtimeError("OP_DUP_N: 栈深度不足");
        }
        push(peek(depth));
        notifyStep(ip, op);
        ip += 2;
        break;
    }

    case OpCode::OP_INIT_FIELD: {
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (idx >= chunk.constants.size())
            return runtimeError("常量池索引越界");
        const std::string& fieldName = chunk.constants[idx].stringVal();
        Value val = pop();
        // 栈顶是实例（OP_CLASS_NEW 推入的），直接修改
        if (!stack_.empty() && stack_.back().isInstance()) {
            stack_.back().fields()[fieldName] = val;
        } else {
            return runtimeError("OP_INIT_FIELD: 栈顶不是实例");
        }
        // M3 fix: 记录字段声明顺序（OP_INIT_FIELD 按 AST 声明顺序执行）
        pendingFieldOrder_.push_back(fieldName);
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    default:
        return runtimeError(ErrorFormat::formatStd("executeMiscStackOps 未知操作码: {}",  static_cast<int>(op)));
    }

    return VMResult::VM_OK;
}

// ============================================================
// executeMiscTypeCheck - 运行时类型注解检查（1 个 case，独立复杂）
// 精确类型匹配 + 实例继承链查找（最深 64 层，防循环继承）
// ============================================================
VMResult VM::executeMiscTypeCheck(OpCode op, size_t& ip) {
    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;
    // 2026-06-29: 运行时类型注解检查（三后端统一强制）
    uint16_t typeIdx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
    if (typeIdx >= chunk.constants.size()) {
        return runtimeError("OP_TYPE_CHECK: 类型注解常量索引越界");
    }
    const std::string& annotation = chunk.constants[typeIdx].stringVal();
    const Value& val = peek(0);
    if (!minilang::typeMatchValue(val, annotation)) {
        // 实例继承链检查（typeMatchValue 仅做精确类名匹配）
        if (val.isInstance() && !annotation.empty()) {
            auto classIt = classInfo_.find(val.className());
            int depth = 0;
            bool found = false;
            while (classIt != classInfo_.end() && depth < 64) {
                if (classIt->second.name == annotation) {
                    found = true;
                    break;
                }
                if (classIt->second.superClassName.empty())
                    break;
                classIt = classInfo_.find(classIt->second.superClassName);
                ++depth;
            }
            if (found) {
                notifyStep(ip, op);
                ip += 3;
                return VMResult::VM_OK;
            } // 匹配，通过
        }
        return runtimeError(
            ErrorFormat::formatStd(ErrorMessages::kTypeAnnotationViolationFmtStd,  annotation,  val.typeName()),
            DiagCodes::kTypeMismatch);
    }
    notifyStep(ip, op);
    ip += 3; // opcode(1B) + typeIdx(2B)
    return VMResult::VM_OK;
}

// ============================================================
// executeMiscTypeTest - R134 软类型测试（OP_TYPE_TEST）
// 与 OP_TYPE_CHECK 同语义（精确类型匹配 + 实例继承链查找）但 push bool 而非抛错。
// 用于 TUPLE pattern 类型检查（不匹配时 fall through 而非抛错）。
// 栈行为：pop val, push bool
// 操作数：typeAnnotationConstIdx(2B)
// ============================================================
VMResult VM::executeMiscTypeTest(OpCode op, size_t& ip) {
    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;
    uint16_t typeIdx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
    if (typeIdx >= chunk.constants.size()) {
        return runtimeError("OP_TYPE_TEST: 类型注解常量索引越界");
    }
    const std::string& annotation = chunk.constants[typeIdx].stringVal();
    if (stack_.size() < 1) {
        return runtimeError("栈下溢: OP_TYPE_TEST");
    }
    Value val = pop();
    bool matched = minilang::typeMatchValue(val, annotation);
    // 实例继承链检查（typeMatchValue 仅做精确类名匹配）
    if (!matched && val.isInstance() && !annotation.empty()) {
        auto classIt = classInfo_.find(val.className());
        int depth = 0;
        while (classIt != classInfo_.end() && depth < 64) {
            if (classIt->second.name == annotation) {
                matched = true;
                break;
            }
            if (classIt->second.superClassName.empty())
                break;
            classIt = classInfo_.find(classIt->second.superClassName);
            ++depth;
        }
    }
    push(Value(matched));
    notifyStep(ip, op);
    ip += 3; // opcode(1B) + typeIdx(2B)
    return VMResult::VM_OK;
}

// ============================================================
// executeMiscJumpOps - 无条件/条件跳转指令（3 个 case）
// 共享 jump 目标越界检查模式：读取 2B 偏移 → 越界检查 → 设置 ip
// ============================================================
VMResult VM::executeMiscJumpOps(OpCode op, size_t& ip) {
    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;
    switch (op) {
    case OpCode::OP_JUMP: {
        uint16_t jump = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (jump >= chunk.code.size())
            return runtimeError("跳转目标越界: OP_JUMP");
        notifyStep(ip, op);
        ip = jump;
        break;
    }

    case OpCode::OP_JUMP_IF_FALSE: {
        uint16_t jump = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (jump >= chunk.code.size())
            return runtimeError("跳转目标越界: OP_JUMP_IF_FALSE");
        // 注意：不弹出条件值——编译器在 OP_JUMP_IF_FALSE 后显式生成 OP_POP
        // 如果这里也 pop，会导致所有条件/短路表达式的栈操作双重弹出
        notifyStep(ip, op);
        if (!peek(0).isTruthy()) {
            ip = jump;
        } else {
            ip += 3;
        }
        break;
    }

    case OpCode::OP_LOOP: {
        uint16_t loop = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (loop >= chunk.code.size())
            return runtimeError("跳转目标越界: OP_LOOP");
        notifyStep(ip, op);
        ip = loop;
        break;
    }

    default:
        return runtimeError(ErrorFormat::formatStd("executeMiscJumpOps 未知操作码: {}",  static_cast<int>(op)));
    }

    return VMResult::VM_OK;
}

// ============================================================
// executeMiscExceptionOps - 异常处理 + finally 跳转栈指令（5 个 case）
// 围绕 tryStack_（try/catch handler 栈）和 pendingJumpStack_（finally 末尾跳转目标栈）管理
// ============================================================
VMResult VM::executeMiscExceptionOps(OpCode op, size_t& ip) {
    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;
    switch (op) {
    case OpCode::OP_TRY_BEGIN: {
        // F11: push try handler，记录 catch 目标和当前栈深度
        uint16_t catchOffset = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        size_t catchIp = ip + 3 + catchOffset;
        if (catchIp >= chunk.code.size())
            return runtimeError("OP_TRY_BEGIN: catch 目标越界");
        tryStack_.push_back({catchIp, stack_.size(), frames_.size() - 1});
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    case OpCode::OP_TRY_END: {
        // F11: try 块正常结束，弹出 try 处理器
        if (!tryStack_.empty() && tryStack_.back().frameIndex == frames_.size() - 1) {
            tryStack_.pop_back();
        }
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_THROW: {
        // F11: 弹出栈顶值作为异常，搜索 try 处理器
        // Bug1 fix: 空栈检查 — pop() 在空栈时设置 hasError_ 并返回 null，
        // 若继续 throwException(null) 会使 catch handler 以错误数据执行
        if (stack_.empty()) {
            return runtimeError("throw 语句在空栈上执行");
        }
        Value thrownValue = pop();
        // P2-2 fix: 调试模式下通知步进（throwException 可能改变 ip）
        notifyStep(ip, op);
        return throwException(std::move(thrownValue));
    }

    case OpCode::OP_PUSH_JUMP_TARGET: {
        // AUDIT-P1.1 fix: push 跳转目标到 pendingJumpStack_，供 finally 末尾的 OP_FINALLY_END 取出。
        // AUDIT-R7 F1 fix: 附带当前帧索引，使 FINALLY_END 能识别并丢弃跨帧残留。
        uint16_t target = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (target >= chunk.code.size())
            return runtimeError("OP_PUSH_JUMP_TARGET: 跳转目标越界");
        pendingJumpStack_.push_back({static_cast<size_t>(target), frames_.size() - 1});
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    case OpCode::OP_FINALLY_END: {
        // AUDIT-P1.1 fix: finally 块正常路径末尾。若 pendingJumpStack_ 非空，
        // 说明是 break/continue 触发的 finally，pop 目标并跳转；否则继续执行（正常完成）。
        // AUDIT-R7 F1 fix: 只消费本帧条目——先惰性丢弃已返回深帧的残留（finally 体内
        // return/TCO 跳过了其 FINALLY_END）；若栈顶属于更浅帧（调用方在途续跳，本帧
        // 是 finally 体内被调函数）则不消费，视为正常完成继续执行。
        notifyStep(ip, op);
        size_t curFrameIdx = frames_.size() - 1;
        while (!pendingJumpStack_.empty() && pendingJumpStack_.back().frameIndex > curFrameIdx) {
            pendingJumpStack_.pop_back();
        }
        if (!pendingJumpStack_.empty() && pendingJumpStack_.back().frameIndex == curFrameIdx) {
            size_t target = pendingJumpStack_.back().target;
            pendingJumpStack_.pop_back();
            if (target >= chunk.code.size())
                return runtimeError("OP_FINALLY_END: 跳转目标越界");
            ip = target;
        } else {
            ip += 1;
        }
        break;
    }

    default:
        return runtimeError(ErrorFormat::formatStd("executeMiscExceptionOps 未知操作码: {}",  static_cast<int>(op)));
    }

    return VMResult::VM_OK;
}
