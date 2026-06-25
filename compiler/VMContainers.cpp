// ============================================================
// VMContainers.cpp - split from VM.cpp (C1 fix: reduce single-file complexity)
// Contains: container/member/writeback/misc instruction execution
// (executeContainerOps / executeWritebackOps / executeMiscOps)
// ============================================================

#include "compiler/VM.h"
#include "interpreter/BuiltinMethods.h"  // 共享纯函数层（len/contains/has）
#include "interpreter/NumericUtils.h"    // 共享溢出检查（B6 fix）
#include "common/Utf8Utils.h"            // P0-4 fix: UTF-8 码位工具
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
            // S5 fix: 字符串键直接用 stringVal() 引用，避免 toString() 中间临时对象
            if (key.isString()) {
                dict.emplace(key.stringVal(), std::move(val));
            } else {
                dict.emplace(key.toString(), std::move(val));
            }
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
            if (i >= 0 && static_cast<size_t>(i) < obj.arrayVal().size()) {
                push(obj.arrayVal()[static_cast<size_t>(i)]);
            } else {
                return runtimeError("数组索引越界: " + std::to_string(i) + ", 有效范围 [0, " + std::to_string(obj.arrayVal().size()) + ")");
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
            if (i >= 0 && static_cast<size_t>(i) < s.size()) {
                const void* strPtr = static_cast<const void*>(&s);
                bool isAscii;
                if (lastAsciiStrPtr_ == strPtr) {
                    isAscii = lastAsciiStrIsAscii_;
                } else {
                    isAscii = true;
                    for (size_t b = 0; b < s.size(); ++b) {
                        if (static_cast<unsigned char>(s[b]) >= 0x80) { isAscii = false; break; }
                    }
                    lastAsciiStrPtr_ = strPtr;
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
                return runtimeError("字符串索引越界: " + std::to_string(i)
                           + ", 有效范围 [0, " + std::to_string(charCount) + ")");
            }
            push(Value(s.substr(targetBytePos, targetByteLen)));
        } else if (obj.isString()) {
            return runtimeError("字符串索引需要整数类型");
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
            if (i >= 0 && static_cast<size_t>(i) < obj.arrayVal().size()) {
                obj.arrayVal()[static_cast<size_t>(i)] = val;
            } else {
                return runtimeError("数组索引越界: " + std::to_string(i));
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
        // A2: 先查 globalSlots_，再查 globals_
        Value* objPtr = nullptr;
        auto gsIt = globalNameToSlot_.find(varName);
        if (gsIt != globalNameToSlot_.end()) {
            objPtr = &globalSlots_[gsIt->second];
        } else {
            auto it = globals_.find(varName);
            if (it == globals_.end()) {
                return runtimeError("未定义的变量: " + varName);
            }
            objPtr = &it->second;
        }
        Value& obj = *objPtr;  // 引用，直接修改
        if (obj.isArray() && index.isInt()) {
            int64_t i = index.intVal();
            if (i >= 0 && static_cast<size_t>(i) < obj.arrayVal().size()) {
                obj.arrayVal()[static_cast<size_t>(i)] = val;
            } else {
                return runtimeError("数组索引越界: " + std::to_string(i) +
                             ", 有效范围 [0, " + std::to_string(obj.arrayVal().size()) + ")");
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
        if (obj.isArray() && index.isInt()) {
            int64_t i = index.intVal();
            if (i >= 0 && static_cast<size_t>(i) < obj.arrayVal().size()) {
                obj.arrayVal()[static_cast<size_t>(i)] = val;
                if (slot == 0) currentFrame().fieldsModified = true;  // VM fix
            } else {
                return runtimeError("数组索引越界: " + std::to_string(i) +
                             ", 有效范围 [0, " + std::to_string(obj.arrayVal().size()) + ")");
            }
        } else if (obj.isDict() && index.isString()) {
            obj.dictVal()[index.stringVal()] = val;
            if (slot == 0) currentFrame().fieldsModified = true;  // VM fix
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
                    push(Value(std::string("method:") + obj.className() + "." + fieldName));
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
        // A2: 先查 globalSlots_，再查 globals_
        Value* objPtr = nullptr;
        auto gsIt = globalNameToSlot_.find(varName);
        if (gsIt != globalNameToSlot_.end()) {
            objPtr = &globalSlots_[gsIt->second];
        } else {
            auto it = globals_.find(varName);
            if (it == globals_.end()) {
                return runtimeError("未定义的变量: " + varName);
            }
            objPtr = &it->second;
        }
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
                const auto& fieldOrder = currentFrame().chunk->fieldOrder;
                for (size_t fi = 0; fi < fieldOrder.size(); ++fi) {
                    if (fieldOrder[fi] == fieldName) {
                        size_t slotPos = bp + 1 + fi;
                        if (slotPos < stack_.size()) {
                            stack_[slotPos] = val;
                        }
                        break;
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
        return runtimeError("未知操作码: " + std::to_string(static_cast<int>(op)));
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
        uint16_t varIdx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        uint16_t fieldIdx = chunk.code[ip + 3] | (chunk.code[ip + 4] << 8);
        if (varIdx >= chunk.constants.size() || fieldIdx >= chunk.constants.size()) return runtimeError("常量池索引越界");
        const std::string& varName = chunk.constants[varIdx].stringVal();
        const std::string& fieldName = chunk.constants[fieldIdx].stringVal();
        // A2: 先查 globalSlots_，再查 globals_
        Value* objPtr = nullptr;
        auto gsIt = globalNameToSlot_.find(varName);
        if (gsIt != globalNameToSlot_.end()) {
            objPtr = &globalSlots_[gsIt->second];
        } else {
            auto it = globals_.find(varName);
            if (it == globals_.end()) {
                lastMutatedReceiver_ = Value::nullValue();
                return runtimeError("未定义的变量: " + varName);
            }
            objPtr = &it->second;
        }
        Value& obj = *objPtr;
        if (obj.isInstance()) {
            obj.fields()[fieldName] = std::move(lastMutatedReceiver_);
        } else if (obj.isDict()) {
            obj.dictVal()[fieldName] = std::move(lastMutatedReceiver_);
        } else {
            lastMutatedReceiver_ = Value::nullValue();
            return runtimeError("类型 " + obj.typeName() + " 不支持成员赋值");
        }
        lastMutatedReceiver_ = Value::nullValue();
        notifyStep(ip, op);
        ip += 5;
        break;
    }

    case OpCode::OP_WRITEBACK_MEMBER_LOCAL: {
        // 操作数: slot(1B) + fieldIdx(2B)
        uint8_t slot = chunk.code[ip + 1];
        uint16_t fieldIdx = chunk.code[ip + 2] | (chunk.code[ip + 3] << 8);
        if (fieldIdx >= chunk.constants.size()) return runtimeError("常量池索引越界");
        const std::string& fieldName = chunk.constants[fieldIdx].stringVal();
        size_t bp = currentFrame().basePointer;
        if (bp + slot >= stack_.size()) {
            lastMutatedReceiver_ = Value::nullValue();
            return runtimeError("OP_WRITEBACK_MEMBER_LOCAL: 栈槽越界");
        }
        Value& obj = stack_[bp + slot];
        if (obj.isInstance()) {
            obj.fields()[fieldName] = std::move(lastMutatedReceiver_);
            // 如果 slot==0（this），也同步更新对应字段槽
            if (slot == 0) {
                VMCallFrame& curFrame = currentFrame();
                if (curFrame.chunk) {
                    for (size_t i = 0; i < curFrame.chunk->fieldOrder.size(); ++i) {
                        if (curFrame.chunk->fieldOrder[i] == fieldName) {
                            size_t fieldSlot = bp + 1 + i;
                            if (fieldSlot < stack_.size()) {
                                stack_[fieldSlot] = std::move(lastMutatedReceiver_);
                            }
                            break;
                        }
                    }
                }
            }
        } else if (obj.isDict()) {
            obj.dictVal()[fieldName] = std::move(lastMutatedReceiver_);
        } else {
            lastMutatedReceiver_ = Value::nullValue();
            return runtimeError("类型 " + obj.typeName() + " 不支持成员赋值");
        }
        lastMutatedReceiver_ = Value::nullValue();
        notifyStep(ip, op);
        ip += 4;
        break;
    }

    case OpCode::OP_WRITEBACK_INDEX_VAR: {
        // 操作数: varIdx(2B)，索引从栈顶 pop
        uint16_t varIdx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        Value index = pop();
        if (varIdx >= chunk.constants.size()) {
            lastMutatedReceiver_ = Value::nullValue();
            return runtimeError("常量池索引越界");
        }
        const std::string& varName = chunk.constants[varIdx].stringVal();
        // A2: 先查 globalSlots_，再查 globals_
        Value* objPtr = nullptr;
        auto gsIt = globalNameToSlot_.find(varName);
        if (gsIt != globalNameToSlot_.end()) {
            objPtr = &globalSlots_[gsIt->second];
        } else {
            auto it = globals_.find(varName);
            if (it == globals_.end()) {
                lastMutatedReceiver_ = Value::nullValue();
                return runtimeError("未定义的变量: " + varName);
            }
            objPtr = &it->second;
        }
        Value& obj = *objPtr;
        if (obj.isArray() && index.isInt()) {
            int64_t i = index.intVal();
            if (i >= 0 && static_cast<size_t>(i) < obj.arrayVal().size()) {
                obj.arrayVal()[static_cast<size_t>(i)] = std::move(lastMutatedReceiver_);
            } else {
                lastMutatedReceiver_ = Value::nullValue();
                return runtimeError("数组索引越界: " + std::to_string(i));
            }
        } else if (obj.isDict() && index.isString()) {
            obj.dictVal()[index.stringVal()] = std::move(lastMutatedReceiver_);
        } else {
            lastMutatedReceiver_ = Value::nullValue();
            return runtimeError("该类型不支持索引赋值");
        }
        lastMutatedReceiver_ = Value::nullValue();
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    case OpCode::OP_WRITEBACK_INDEX_LOCAL: {
        // 操作数: slot(1B)，索引从栈顶 pop
        uint8_t slot = chunk.code[ip + 1];
        Value index = pop();
        size_t bp = currentFrame().basePointer;
        if (bp + slot >= stack_.size()) {
            lastMutatedReceiver_ = Value::nullValue();
            return runtimeError("OP_WRITEBACK_INDEX_LOCAL: 栈槽越界");
        }
        Value& obj = stack_[bp + slot];
        if (obj.isArray() && index.isInt()) {
            int64_t i = index.intVal();
            if (i >= 0 && static_cast<size_t>(i) < obj.arrayVal().size()) {
                obj.arrayVal()[static_cast<size_t>(i)] = std::move(lastMutatedReceiver_);
            } else {
                lastMutatedReceiver_ = Value::nullValue();
                return runtimeError("数组索引越界: " + std::to_string(i));
            }
        } else if (obj.isDict() && index.isString()) {
            obj.dictVal()[index.stringVal()] = std::move(lastMutatedReceiver_);
        } else {
            lastMutatedReceiver_ = Value::nullValue();
            return runtimeError("该类型不支持索引赋值");
        }
        lastMutatedReceiver_ = Value::nullValue();
        notifyStep(ip, op);
        ip += 2;
        break;
    }

    default:
        return runtimeError("未知操作码: " + std::to_string(static_cast<int>(op)));
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
        return runtimeError("未知操作码: " + std::to_string(static_cast<int>(op)));
    }

    return VMResult::VM_OK;
}
