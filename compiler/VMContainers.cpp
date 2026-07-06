// ============================================================
// VMContainers.cpp - split from VM.cpp (C1 fix: reduce single-file complexity)
// Contains: container/member/writeback/misc instruction execution
// (executeContainerOps / executeWritebackOps / executeMiscOps)
// ============================================================

#include "compiler/VM.h"
#include "interpreter/BuiltinMethods.h"  // 共享纯函数层（len/contains/has）
#include "interpreter/NumericUtils.h"    // 共享溢出检查（B6 fix）
#include "interpreter/StringIntern.h"    // PERF-05 fix: 方法标记字符串驻留
#include "common/Utf8Utils.h"            // P0-4 fix: UTF-8 码位工具
#include "common/BoundsCheck.h"           // Dedup-7A: inBounds 替代重复的索引检查
#include "interpreter/ErrorFormat.h"    // P3 fix: runtimeErrorFmt 替代 std::to_string 拼接
#include "common/TypeChecker.h"        // 2026-06-29: typeMatchValue（OP_TYPE_CHECK）
#include "Logger.h"
#include <sstream>
#include <climits>
#include <cstdint>
#include <cmath>  // BUG 8.1 fix: std::fmod
#include <algorithm>


// ============================================================
// 容器与成员操作类指令
// ============================================================

VMResult VM::executeContainerOps(OpCode op, size_t& ip) {
    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;

    switch (op) {
    case OpCode::OP_BUILD_ARRAY: {
        uint8_t count = chunk.code[ip + 1];
        if (stack_.size() < count) return runtimeError("栈下溢: OP_BUILD_ARRAY");
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
        if (stack_.size() < static_cast<size_t>(pairCount) * 2) return runtimeError("栈下溢: OP_BUILD_DICT");
        std::unordered_map<std::string, Value> dict;
        dict.reserve(pairCount);
        // 先入后出：倒序弹出键值对
        for (uint8_t i = 0; i < pairCount; ++i) {
            Value val = pop();
            Value key = pop();
            // B6 fix: 对齐 RegisterVM REG_BUILD_DICT——非 string 键显式报错，
            // 不再静默 toString() 转换（与索引访问 d[k] 要求 string 键一致）。
            if (!key.isString()) {
                // BUG-VM-04 fix: 错误返回前清理栈上剩余未处理的键值对，
                // 避免错误路径栈残留 2 * (pairCount - i - 1) 个 Value
                popN(static_cast<size_t>(pairCount - i - 1) * 2);
                return runtimeError("字典键必须是字符串");
            }
            dict.emplace(key.stringVal(), std::move(val));
        }
        push(Value(std::move(dict)));
        notifyStep(ip, op);
        ip += 2;
        break;
    }

    case OpCode::OP_INDEX_GET: {
        if (stack_.size() < 2) return runtimeError("栈下溢: OP_INDEX_GET");
        Value idx = pop();
        // V-P2-15 fix: obj 只读不写，改为 const 避免 arrayVal/dictVal/stringVal 触发 COW 深拷贝
        const Value obj = pop();
        if (obj.isArray() && idx.isInt()) {
            int64_t i = idx.intVal();
            if (BoundsCheck::inBounds(i, obj.arrayVal().size())) {
                push(obj.arrayVal()[static_cast<size_t>(i)]);
            } else {
                return runtimeError(ErrorFormat::format("数组索引越界: %lld, 有效范围 [0, %zu)",
                    static_cast<long long>(i), obj.arrayVal().size()));
            }
        } else if (obj.isDict() && idx.isString()) {
            auto it = obj.dictVal().find(idx.stringVal());
            if (it != obj.dictVal().end()) {
                push(it->second);
            } else {
                push(Value::nullValue());  // 字典访问不存在的键返回 null（与解释器一致）
            }
        } else if (obj.isArray()) {
            return runtimeError("数组索引需要整数类型");
        } else if (obj.isString() && idx.isInt()) {
            // V3 fix: 基于 UTF-8 码位索引，与解释器 M6 fix 一致
            int64_t i = idx.intVal();
            const std::string& s = obj.stringVal();

            // P7 fix: ASCII 快速路径 — 纯 ASCII 字符串直接按字节索引 O(1)
            // S5 fix: 缓存 StringData* 指针，O(1) 指针比较替代 O(n) 字符串内容比较
            //         安全性：StringData 由 shared_ptr 持有，Value 在栈上时指针有效
            if (BoundsCheck::inBounds(i, s.size())) {
                const void* strPtr = static_cast<const void*>(&s);
                bool isAscii;
                // BUGFIX-P2 fix: 同时比对指针 + size，防止堆地址复用导致非 ASCII 字符串误判为 ASCII
                if (lastAsciiStrPtr_ == strPtr && lastAsciiStrSize_ == s.size()) {
                    isAscii = lastAsciiStrIsAscii_;
                } else {
                    isAscii = true;
                    for (size_t b = 0; b < s.size(); ++b) {
                        if (static_cast<unsigned char>(s[b]) >= 0x80) { isAscii = false; break; }
                    }
                    lastAsciiStrPtr_ = strPtr;
                    lastAsciiStrSize_ = s.size();
                    lastAsciiStrIsAscii_ = isAscii;
                }
                if (isAscii) {
                    push(Value(s.substr(static_cast<size_t>(i), 1)));
                    notifyStep(ip, op);
                    ip += 1;
                    break;  // 跳过慢路径，直接完成 OP_INDEX_GET
                }
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
                return runtimeError(ErrorFormat::format("字符串索引越界: %lld, 有效范围 [0, %lld)",
                    static_cast<long long>(i), static_cast<long long>(charCount)));
            }
            push(Value(s.substr(targetBytePos, targetByteLen)));
        } else if (obj.isString()) {
            return runtimeError("字符串索引必须是整数");
        } else if (obj.isDict()) {
            // B5 fix: 对齐 RegisterVM/Interpreter——dict 非 string 索引原落入通用
            // "该类型不支持索引访问" 分支，现显式报 "字典键必须是字符串"。
            return runtimeError("字典键必须是字符串");
        } else {
            return runtimeError("该类型不支持索引访问");
        }
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_INDEX_SET: {
        // M1 fix: 嵌套索引赋值（如 arr[i][j] = val）
        // 栈序: [..., obj, outerIdx, innerIdx, val]
        // 弹出 val, innerIdx, obj → 修改 obj[innerIdx] → 存入 lastMutatedReceiver_
        if (stack_.size() < 3) return runtimeError("栈下溢: OP_INDEX_SET");
        Value val = pop();
        Value innerIdx = pop();
        Value obj = pop();
        if (obj.isArray() && innerIdx.isInt()) {
            int64_t i = innerIdx.intVal();
            // Perf-Finding: 越界错误路径用 std::as_const 避免 COW detach（pop 出的 obj 与原栈槽共享 ArrayData）
            if (BoundsCheck::inBounds(i, std::as_const(obj).arrayVal().size())) {
                obj.arrayVal()[static_cast<size_t>(i)] = val;
            } else {
                return runtimeError(ErrorFormat::format("数组索引越界: %lld, 有效范围 [0, %zu)",
                    static_cast<long long>(i), std::as_const(obj).arrayVal().size()));
            }
        } else if (obj.isDict() && innerIdx.isString()) {
            obj.dictVal()[innerIdx.stringVal()] = val;
        } else if (obj.isArray()) {
            return runtimeError("数组索引需要整数类型");
        } else {
            return runtimeError("该类型不支持索引赋值");
        }
        lastMutatedReceiver_ = std::move(obj);
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_INDEX_SET_VAR: {
        // 直接修改全局变量中的数组/字典元素
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (idx >= chunk.constants.size()) return runtimeError("常量池索引越界");
        const std::string& varName = chunk.constants[idx].stringVal();
        if (stack_.size() < 2) return runtimeError("栈下溢: OP_INDEX_SET_VAR");
        Value val = pop();
        Value index = pop();
        // A2: Dedup-7B resolveMutableGlobal 统一全局变量解析（先 globalSlots_ 再 globals_）
        Value* objPtr = resolveMutableGlobal(varName);
        if (!objPtr) return runtimeError("未定义的变量: " + varName);
        Value& obj = *objPtr;  // 引用，直接修改
        if (obj.isArray() && index.isInt()) {
            int64_t i = index.intVal();
            // Perf-Finding: 越界错误路径用 std::as_const 避免 COW detach（全局数组 refCount 常 >1）
            if (BoundsCheck::inBounds(i, std::as_const(obj).arrayVal().size())) {
                obj.arrayVal()[static_cast<size_t>(i)] = val;
            } else {
                return runtimeError(ErrorFormat::format("数组索引越界: %lld, 有效范围 [0, %zu)",
                    static_cast<long long>(i), std::as_const(obj).arrayVal().size()));
            }
        } else if (obj.isDict() && index.isString()) {
            obj.dictVal()[index.stringVal()] = val;
        } else if (obj.isArray()) {
            return runtimeError("数组索引需要整数类型");
        } else {
            return runtimeError("该类型不支持索引赋值");
        }
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    case OpCode::OP_INDEX_SET_LOCAL: {
        // 直接修改 stack_[bp+slot] 中的数组/字典元素（用于方法内 this.arr[i] = val）
        uint8_t slot = chunk.code[ip + 1];
        if (stack_.size() < 2) return runtimeError("栈下溢: OP_INDEX_SET_LOCAL");
        Value val = pop();
        Value index = pop();
        size_t bp = currentFrame().basePointer;
        if (bp + slot >= stack_.size()) {
            return runtimeError("内部错误: 局部变量槽越界");
        }
        Value& obj = stack_[bp + slot];  // 栈引用，直接修改
        // BUG-INH-AUDIT-4 fix: slot==0 谓词对 OP_INDEX_SET_LOCAL 是死代码——slot 0 是
        // this 实例（非数组/字典），数组/字典分支永不命中。正确谓词应与 OP_SET_LOCAL
        // 对齐：slot 在字段范围 [1, fieldOrder.size()] 时标记 fieldsModified，确保
        // this.arr[i]=val 的 COW detach 后字段同步回 this.fields()。
        bool isFieldSlot = (slot > 0 && currentFrame().chunk &&
                            slot <= currentFrame().chunk->fieldOrder.size());
        if (obj.isArray() && index.isInt()) {
            int64_t i = index.intVal();
            // Perf-Finding: 越界错误路径用 std::as_const 避免 COW detach（栈槽 obj 来自 this.arr 时 refCount 常 >1）
            if (BoundsCheck::inBounds(i, std::as_const(obj).arrayVal().size())) {
                obj.arrayVal()[static_cast<size_t>(i)] = val;
                if (isFieldSlot) currentFrame().fieldsModified = true;
            } else {
                return runtimeError(ErrorFormat::format("数组索引越界: %lld, 有效范围 [0, %zu)",
                    static_cast<long long>(i), std::as_const(obj).arrayVal().size()));
            }
        } else if (obj.isDict() && index.isString()) {
            obj.dictVal()[index.stringVal()] = val;
            if (isFieldSlot) currentFrame().fieldsModified = true;
        } else if (obj.isArray()) {
            return runtimeError("数组索引需要整数类型");
        } else {
            return runtimeError("该类型不支持索引赋值");
        }
        notifyStep(ip, op);
        ip += 2;
        break;
    }

    case OpCode::OP_SUPER_MEMBER_GET:
    case OpCode::OP_MEMBER_GET: {
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (idx >= chunk.constants.size()) return runtimeError("常量池索引越界");
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
                    push(Value(StringIntern::internConcat(
                        StringIntern::internConcat("method:", obj.className()),
                        std::string(".") + fieldName)));
                } else {
                    return runtimeError("类 " + obj.className() + " 没有字段或方法 '" + fieldName + "'");
                }
            }
        } else if (obj.isDict()) {
            auto it = obj.dictVal().find(fieldName);
            if (it != obj.dictVal().end()) {
                push(it->second);
            } else {
                push(Value::nullValue());
            }
        } else {
            return runtimeError("该类型不支持成员访问");
        }
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    case OpCode::OP_MEMBER_SET: {
        // M1 fix: 嵌套成员赋值（如 obj.field.subfield = val 或 arr[i].field = val）
        // 栈序: [..., obj, val]
        // 弹出 val, obj → 修改 obj.field → 存入 lastMutatedReceiver_
        if (stack_.size() < 2) return runtimeError("栈下溢: OP_MEMBER_SET");
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (idx >= chunk.constants.size()) return runtimeError("常量池索引越界");
        const std::string& fieldName = chunk.constants[idx].stringVal();
        Value val = pop();
        Value obj = pop();
        if (obj.isInstance()) {
            obj.fields()[fieldName] = val;
        } else if (obj.isDict()) {
            obj.dictVal()[fieldName] = val;
        } else {
            return runtimeError("该类型不支持成员赋值");
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
        if (varIdx >= chunk.constants.size() || fieldIdx >= chunk.constants.size()) return runtimeError("常量池索引越界");
        const std::string& varName = chunk.constants[varIdx].stringVal();
        const std::string& fieldName = chunk.constants[fieldIdx].stringVal();
        Value val = pop();
        // A2: Dedup-7B resolveMutableGlobal 统一全局变量解析
        Value* objPtr = resolveMutableGlobal(varName);
        if (!objPtr) return runtimeError("未定义的变量: " + varName);
        Value& obj = *objPtr;  // 引用，直接修改
        if (obj.isInstance()) {
            obj.fields()[fieldName] = val;
        } else if (obj.isDict()) {
            obj.dictVal()[fieldName] = val;
        } else {
            return runtimeError("该类型不支持成员赋值");
        }
        notifyStep(ip, op);
        ip += 5;
        break;
    }

    case OpCode::OP_MEMBER_SET_LOCAL: {
        // 直接修改 stack_[bp+slot].fields()[fieldName]（用于方法内 this.field = val）
        uint8_t slot = chunk.code[ip + 1];
        uint16_t fieldIdx = chunk.code[ip + 2] | (chunk.code[ip + 3] << 8);
        if (fieldIdx >= chunk.constants.size()) return runtimeError("常量池索引越界");
        const std::string& fieldName = chunk.constants[fieldIdx].stringVal();
        Value val = pop();
        size_t bp = currentFrame().basePointer;
        if (bp + slot >= stack_.size()) {
            return runtimeError("内部错误: 局部变量槽越界");
        }
        Value& obj = stack_[bp + slot];  // 栈引用，直接修改
        if (obj.isInstance()) {
            obj.fields()[fieldName] = val;
            // VM fix: 标记字段已修改，OP_RETURN 可跳过只读方法的字段同步
            if (slot == 0) currentFrame().fieldsModified = true;
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
            obj.dictVal()[fieldName] = val;
        } else {
            return runtimeError("该类型不支持成员赋值");
        }
        notifyStep(ip, op);
        ip += 4;
        break;
    }

    default:
        return runtimeError(ErrorFormat::format("未知操作码: %d", static_cast<int>(op)));
    }

    return VMResult::VM_OK;
}

// ============================================================
// 嵌套访问写回类指令
// ============================================================

VMResult VM::executeWritebackOps(OpCode op, size_t& ip) {
    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;

    // ---- 嵌套访问变异方法写回指令 ----
    // 从 lastMutatedReceiver_ 取值，写回基对象的字段或索引位置
    switch (op) {
    case OpCode::OP_WRITEBACK_MEMBER_VAR: {
        // 操作数: varIdx(2B) + fieldIdx(2B)
        // BUG-NEW fix: 语义对齐 RegisterVM 的 REG_WRITEBACK_MEMBER_VAR ——
        // 将 lastMutatedReceiver_（MEMBER_SET 产生的变异后整个容器）整体替换全局变量，
        // 而非写入 obj.field。原实现 obj.field = lastMutatedReceiver_ 导致
        // d.x = 42 把整个变异后 d 赋给 d["x"]，产生嵌套字典。
        uint16_t varIdx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        uint16_t fieldIdx = chunk.code[ip + 3] | (chunk.code[ip + 4] << 8);
        if (varIdx >= chunk.constants.size() || fieldIdx >= chunk.constants.size()) return runtimeError("常量池索引越界");
        const std::string& varName = chunk.constants[varIdx].stringVal();
        // fieldIdx 仅用于反汇编/调试，运行时不需要（整体替换语义）
        (void)chunk.constants[fieldIdx];
        // A2: Dedup-7B resolveMutableGlobal 统一全局变量解析
        Value* objPtr = resolveMutableGlobal(varName);
        if (!objPtr) {
            lastMutatedReceiver_ = Value::nullValue();
            return runtimeError("未定义的变量: " + varName);
        }
        *objPtr = std::move(lastMutatedReceiver_);
        lastMutatedReceiver_ = Value::nullValue();
        notifyStep(ip, op);
        ip += 5;
        break;
    }

    case OpCode::OP_WRITEBACK_MEMBER_LOCAL: {
        // 操作数: slot(1B) + fieldIdx(2B)
        // BUG-NEW fix: 语义对齐 RegisterVM —— 整体替换栈槽为 lastMutatedReceiver_，
        // 而非写入 obj.field。原实现 obj.field = mutated 导致局部 d.x = 42 把整个
        // 变异后 d 赋给 d["x"]。IR 路径不使用字段槽（fieldSlot），故移除 slot==0
        // 的字段槽同步逻辑（该逻辑仅服务于 Compiler.cpp 非 IR 路径，但本指令仅由
        // IR 路径发射，不会冲突）。
        //
        // BUG-INH-AUDIT-4 fix: 若 slot 是字段槽（this.field 的字段位置），
        // 标记 fieldsModified=true，确保 OP_RETURN 时字段同步回 this 实例。
        // 否则 this.obj.field = val 的 COW detach 后字段不会写回 this.fields()。
        uint8_t slot = chunk.code[ip + 1];
        uint16_t fieldIdx = chunk.code[ip + 2] | (chunk.code[ip + 3] << 8);
        if (fieldIdx >= chunk.constants.size()) return runtimeError("常量池索引越界");
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
        if (slot > 0 && currentFrame().chunk &&
            slot <= currentFrame().chunk->fieldOrder.size()) {
            currentFrame().fieldsModified = true;
        }
        notifyStep(ip, op);
        ip += 4;
        break;
    }

    case OpCode::OP_WRITEBACK_INDEX_VAR: {
        // 操作数: varIdx(2B)
        // BUG-NEW fix: 语义对齐 RegisterVM 的 REG_WRITEBACK_INDEX_VAR ——
        // 整体替换全局变量为 lastMutatedReceiver_，不 pop 索引。
        // 原实现 pop 索引后做 obj[index] = mutated，但 IR 路径不向栈推入索引，
        // 导致栈下溢；且语义应为整体替换而非写入 obj[index]。
        uint16_t varIdx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (varIdx >= chunk.constants.size()) {
            lastMutatedReceiver_ = Value::nullValue();
            return runtimeError("常量池索引越界");
        }
        const std::string& varName = chunk.constants[varIdx].stringVal();
        // A2: Dedup-7B resolveMutableGlobal 统一全局变量解析
        Value* objPtr = resolveMutableGlobal(varName);
        if (!objPtr) {
            lastMutatedReceiver_ = Value::nullValue();
            return runtimeError("未定义的变量: " + varName);
        }
        *objPtr = std::move(lastMutatedReceiver_);
        lastMutatedReceiver_ = Value::nullValue();
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    case OpCode::OP_WRITEBACK_INDEX_LOCAL: {
        // 操作数: slot(1B)
        // BUG-NEW fix: 同 OP_WRITEBACK_INDEX_VAR，整体替换栈槽，不 pop 索引。
        // BUG-INH-AUDIT-4 fix: 若 slot 是字段槽（this.arr[i]=val 的字段位置），
        // 标记 fieldsModified=true，确保 OP_RETURN 时字段同步回 this 实例。
        // 否则 OP_INDEX_SET 的 COW detach 后新数组虽写回 stack_[bp+slot]，
        // 但 this.fields() 仍指向旧数组，OP_RETURN 不会同步。
        uint8_t slot = chunk.code[ip + 1];
        size_t bp = currentFrame().basePointer;
        if (bp + slot >= stack_.size()) {
            lastMutatedReceiver_ = Value::nullValue();
            return runtimeError("OP_WRITEBACK_INDEX_LOCAL: 栈槽越界");
        }
        stack_[bp + slot] = std::move(lastMutatedReceiver_);
        lastMutatedReceiver_ = Value::nullValue();
        // BUG-INH-AUDIT-4 fix: 字段槽写回需标记 fieldsModified
        if (slot > 0 && currentFrame().chunk &&
            slot <= currentFrame().chunk->fieldOrder.size()) {
            currentFrame().fieldsModified = true;
        }
        notifyStep(ip, op);
        ip += 2;
        break;
    }

    case OpCode::OP_WRITEBACK_MEMBER_UPVALUE: {
        // #7 fix: 操作数 uvIdx(1B) + fieldIdx(2B)（fieldIdx 仅用于反汇编，运行时不用）
        // 语义：将 lastMutatedReceiver_（MEMBER_SET 产生的变异后整个容器）整体替换 upvalue 槽位。
        // 注意：与 WRITEBACK_MEMBER_LOCAL/VAR 的"写入 obj.field"语义不同——
        // 简单 1 级 `obj.f = v`（obj 是 upvalue）经 IR 路径 MEMBER_SET 已完成字段写入，
        // 此处只需把 COW 产生的新容器替换回 upvalue 即可。
        uint8_t uvIdx = chunk.code[ip + 1];
        if (static_cast<size_t>(uvIdx) >= frame.upvalues.size()) {
            lastMutatedReceiver_ = Value::nullValue();
            return runtimeError("OP_WRITEBACK_MEMBER_UPVALUE: upvalue 索引越界");
        }
        auto& uv = frame.upvalues[uvIdx];
        if (uv->isClosed) {
            uv->value = std::move(lastMutatedReceiver_);
        } else {
            if (uv->stackSlot >= stack_.size()) {
                lastMutatedReceiver_ = Value::nullValue();
                return runtimeError("OP_WRITEBACK_MEMBER_UPVALUE: upvalue 栈槽越界");
            }
            stack_[uv->stackSlot] = std::move(lastMutatedReceiver_);
            // P0-3 fix: 与 OP_SET_UPVALUE 对齐——若修改的是某外层帧的字段槽，
            // 标记该帧 fieldsModified，确保 OP_RETURN 时字段同步回实例。
            // 闭包内 `capturedField.subfield = v` 的变异必须传播回 this 实例。
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
        ip += 4;
        break;
    }

    case OpCode::OP_WRITEBACK_INDEX_UPVALUE: {
        // #7 fix: 操作数 uvIdx(1B)
        // 语义：将 lastMutatedReceiver_（INDEX_SET 产生的变异后整个容器）整体替换 upvalue 槽位。
        // 不 pop 索引——简单 1 级 `arr[i] = v`（arr 是 upvalue）经 IR 路径 INDEX_SET 已完成元素写入，
        // 此处只需把 COW 产生的新容器替换回 upvalue。
        uint8_t uvIdx = chunk.code[ip + 1];
        if (static_cast<size_t>(uvIdx) >= frame.upvalues.size()) {
            lastMutatedReceiver_ = Value::nullValue();
            return runtimeError("OP_WRITEBACK_INDEX_UPVALUE: upvalue 索引越界");
        }
        auto& uv = frame.upvalues[uvIdx];
        if (uv->isClosed) {
            uv->value = std::move(lastMutatedReceiver_);
        } else {
            if (uv->stackSlot >= stack_.size()) {
                lastMutatedReceiver_ = Value::nullValue();
                return runtimeError("OP_WRITEBACK_INDEX_UPVALUE: upvalue 栈槽越界");
            }
            stack_[uv->stackSlot] = std::move(lastMutatedReceiver_);
            // P0-3 fix: 与 OP_SET_UPVALUE 对齐——若修改的是某外层帧的字段槽，
            // 标记该帧 fieldsModified，确保 OP_RETURN 时字段同步回实例。
            // 闭包内 `capturedArr[i] = v` 的变异必须传播回 this 实例。
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
        ip += 2;
        break;
    }

    default:
        return runtimeError(ErrorFormat::format("未知操作码: %d", static_cast<int>(op)));
    }

    return VMResult::VM_OK;
}

// ============================================================
// 其他指令（输出、栈操作、跳转、字段初始化）
// ============================================================

VMResult VM::executeMiscOps(OpCode op, size_t& ip) {
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

    case OpCode::OP_LOAD_MUTATED: {
        // MEDIUM-1/2 fix: 读取 lastMutatedReceiver_ 到栈顶（不清除）。
        // 嵌套左值写回链中，上一级 MEMBER_SET/INDEX_SET 将变异后容器存入
        // lastMutatedReceiver_，此处读取供下一级 SET 作为 val 使用。
        push(lastMutatedReceiver_);
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    // 2026-06-29: 运行时类型注解检查（三后端统一强制）
    case OpCode::OP_TYPE_CHECK: {
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
                    if (classIt->second.name == annotation) { found = true; break; }
                    if (classIt->second.superClassName.empty()) break;
                    classIt = classInfo_.find(classIt->second.superClassName);
                    ++depth;
                }
                if (found) { notifyStep(ip, op); ip += 3; break; }  // 匹配，通过
            }
            return runtimeError(ErrorFormat::format(
                "类型注解违反: 期望类型 %s，实际为 %s",
                annotation.c_str(), val.typeName().c_str()));
        }
        notifyStep(ip, op);
        ip += 3;  // opcode(1B) + typeIdx(2B)
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

    case OpCode::OP_JUMP: {
        uint16_t jump = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (jump >= chunk.code.size()) return runtimeError("跳转目标越界: OP_JUMP");
        notifyStep(ip, op);
        ip = jump;
        break;
    }

    case OpCode::OP_JUMP_IF_FALSE: {
        uint16_t jump = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (jump >= chunk.code.size()) return runtimeError("跳转目标越界: OP_JUMP_IF_FALSE");
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
        if (loop >= chunk.code.size()) return runtimeError("跳转目标越界: OP_LOOP");
        notifyStep(ip, op);
        ip = loop;
        break;
    }

    case OpCode::OP_INIT_FIELD: {
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (idx >= chunk.constants.size()) return runtimeError("常量池索引越界");
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

    case OpCode::OP_TRY_BEGIN: {
        // F11: push try handler，记录 catch 目标和当前栈深度
        uint16_t catchOffset = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        size_t catchIp = ip + 3 + catchOffset;
        if (catchIp >= chunk.code.size()) return runtimeError("OP_TRY_BEGIN: catch 目标越界");
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

    default:
        return runtimeError(ErrorFormat::format("未知操作码: %d", static_cast<int>(op)));
    }

    return VMResult::VM_OK;
}
