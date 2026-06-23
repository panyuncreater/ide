#include "compiler/Compiler.h"
#include "interpreter/NumericUtils.h"  // 共享溢出检查（B6 fix）
#include "Logger.h"
#include <sstream>
#include <algorithm>
#include <cstdint>

// ============================================================
// Compiler 字节码编译器实现
// ============================================================

Compiler::Compiler() {}

CompileResult Compiler::compile(Block& program) {
    chunk_ = BytecodeChunk();
    chunk_.name = "main";
    chunk_.arity = 0;
    chunk_.reserveCode(1024);  // C21: 预分配字节码空间
    varIndex_.clear();
    diagnostics_.clear();
    functionChunks_.clear();
    currentLocals_.clear();
    inFunction_ = false;
    classFieldNames_.clear();
    outerLocals_.clear();
    writebackCounter_ = 0;  // #24: reset for clean variable names across compilations
    peakLocals_ = 0;
    blockDepth_ = 0;
    blockSaveCounter_ = 0;  // L11 fix: 编译间重置块保存计数器
    topLevelGlobals_.clear();
    globalSlots_.clear();
    slotNames_.clear();
    freeSlots_.clear();
    innerFunctions_.clear();        // H5 fix: 重置内嵌函数追踪
    innerFunctionSlots_.clear();    // H5 fix

    // A2: pre-scan top-level declarations to assign global slots (eliminates forward-reference issues)
    for (auto& stmt : program.statements) {
        if (stmt && stmt->nodeType == NodeType::NODE_VAR_DECL) {
            allocateGlobalSlot(static_cast<VarDecl*>(stmt.get())->name);
        } else if (stmt && stmt->nodeType == NodeType::NODE_CLASS_DECL) {
            allocateGlobalSlot(static_cast<ClassDecl*>(stmt.get())->name);
        }
    }

    // 编译所有顶层语句（不调用 compileBlock，确保 blockDepth_=0 为真正顶层）
    for (auto& stmt : program.statements) {
        compileStatement(stmt.get());
    }

    // 末尾添加 null + RETURN（main chunk 必须有返回值，否则 OP_RETURN 弹栈下溢）
    chunk_.writeOp(OpCode::OP_NULL, 0);
    chunk_.writeOp(OpCode::OP_RETURN, 0);

    CompileResult result;
    result.mainChunk = std::move(chunk_);
    result.functionChunks = std::move(functionChunks_);
    result.globalSlotCount = static_cast<int>(slotNames_.size());
    result.globalSlotNames = slotNames_;

    // 预计算 ip→指令索引映射（用于调试高亮 O(1) 查找）
    result.mainChunk.buildIpMap();
    for (auto& kv : result.functionChunks) {
        kv.second.buildIpMap();
    }

    Logger::Info("字节码编译完成: " + std::to_string(result.globalSlotCount) + " 全局槽, " +
        std::to_string(result.functionChunks.size()) + " 函数chunk", "Compiler");
    return result;
}

std::string Compiler::getLastError() const {
    // 从 diagnostics_ 派生最后一条错误信息
    const auto& diags = diagnostics_.all();
    for (auto it = diags.rbegin(); it != diags.rend(); ++it) {
        if (it->isError()) {
            return it->format();
        }
    }
    return {};
}

// A2: global slot management
int Compiler::allocateGlobalSlot(const std::string& name) {
    auto it = globalSlots_.find(name);
    if (it != globalSlots_.end()) return it->second;
    int slot;
    if (!freeSlots_.empty()) {
        slot = freeSlots_.back();
        freeSlots_.pop_back();
        slotNames_[slot] = name;
    } else {
        slot = static_cast<int>(slotNames_.size());
        slotNames_.push_back(name);
    }
    globalSlots_[name] = slot;
    return slot;
}

void Compiler::releaseGlobalSlot(const std::string& name) {
    auto it = globalSlots_.find(name);
    if (it != globalSlots_.end()) {
        freeSlots_.push_back(it->second);
        globalSlots_.erase(it);
    }
}

int Compiler::lookupGlobalSlot(const std::string& name) const {
    auto it = globalSlots_.find(name);
    return (it != globalSlots_.end()) ? it->second : -1;
}

uint16_t Compiler::identifierIndex(const std::string& name) {
    auto it = varIndex_.find(name);
    if (it != varIndex_.end()) return it->second;
    uint16_t idx = chunk_.addConstant(Value(name));
    varIndex_[name] = idx;
    return idx;
}

void Compiler::compileNode(ASTNode* node) {
    if (!node) return;

    // 通过 Visitor 模式分派：accept 会回调对应的 visit* 方法
    node->accept(*this);
}

void Compiler::compileStatement(ASTNode* node) {
    if (!node) return;

    compileNode(node);

    // 表达式语句（如 foo(); 或 a = 5;）会产生一个栈上返回值但不会被消费，
    // 若不弹出会导致栈无限累积（main chunk 无外层帧清理，循环内泄漏必然触发栈溢出）。
    // 声明/控制流节点（VarDecl/IfStmt/WhileStmt 等）已自行平衡栈，
    // IndexAssign/MemberAssign 也已自行平衡，此处对产生栈值的表达式补发 OP_POP。
    switch (node->nodeType) {
    case NodeType::NODE_ASSIGNMENT:
    case NodeType::NODE_FUN_CALL:
    case NodeType::NODE_METHOD_CALL:
    case NodeType::NODE_BINARY_OP:
    case NodeType::NODE_UNARY_OP:
    case NodeType::NODE_VAR_REF:
    case NodeType::NODE_MEMBER_ACCESS:
    case NodeType::NODE_INDEX_ACCESS:
    case NodeType::NODE_NUMBER_LITERAL:
    case NodeType::NODE_STRING_LITERAL:
    case NodeType::NODE_BOOL_LITERAL:
    case NodeType::NODE_NULL_LITERAL:
    case NodeType::NODE_SUPER_EXPR:
    case NodeType::NODE_ARRAY_LITERAL:
    case NodeType::NODE_DICT_LITERAL:
        chunk_.writeOp(OpCode::OP_POP, node->line);
        break;
    default:
        break;
    }
}

Value Compiler::visitBinaryOp(BinaryOp& node) {
    // 常量折叠：编译期求值常量表达式
    Value folded;
    if (tryFoldBinary(node.opType, node.left.get(), node.right.get(), folded, node.line)) {
        emitConstant(folded, node.line);
        return Value::nullValue();
    }

    // 短路运算特殊处理
    if (node.opType == BinOpType::BIN_AND) {
        compileNode(node.left.get());
        // 如果左操作数为假，跳过右操作数
        size_t jumpPatch = chunk_.code.size();
        chunk_.writeOp(OpCode::OP_JUMP_IF_FALSE, node.line);
        chunk_.writeShort(0, node.line);  // 占位
        chunk_.writeOp(OpCode::OP_POP, node.line);  // 弹出左操作数
        compileNode(node.right.get());
        // 修补跳转地址
        uint16_t jumpTarget = safeCodeOffset();
        chunk_.code[jumpPatch + 1] = static_cast<uint8_t>(jumpTarget & 0xFF);
        chunk_.code[jumpPatch + 2] = static_cast<uint8_t>((jumpTarget >> 8) & 0xFF);
        // M1 fix: 移除 NOT NOT 双重取反，保留操作数原始值（JS 语义）
        return Value::nullValue();
    }

    if (node.opType == BinOpType::BIN_OR) {
        compileNode(node.left.get());
        // 如果左操作数为真，跳过右操作数，保留左值
        size_t jumpPatch = chunk_.code.size();
        chunk_.writeOp(OpCode::OP_JUMP_IF_FALSE, node.line);
        chunk_.writeShort(0, node.line);  // 占位（先跳到 or 右侧）
        // 如果到这里，左操作数为真 — 保留左值，跳到末尾
        size_t jumpEnd = chunk_.code.size();
        chunk_.writeOp(OpCode::OP_JUMP, node.line);
        chunk_.writeShort(0, node.line);  // 占位
        // 修补第一个跳转：左操作数为假，求值右操作数
        uint16_t rightStart = safeCodeOffset();
        chunk_.code[jumpPatch + 1] = static_cast<uint8_t>(rightStart & 0xFF);
        chunk_.code[jumpPatch + 2] = static_cast<uint8_t>((rightStart >> 8) & 0xFF);
        // 弹出左操作数，求值右操作数
        chunk_.writeOp(OpCode::OP_POP, node.line);
        compileNode(node.right.get());
        // 修补第二个跳转
        uint16_t endTarget = safeCodeOffset();
        chunk_.code[jumpEnd + 1] = static_cast<uint8_t>(endTarget & 0xFF);
        chunk_.code[jumpEnd + 2] = static_cast<uint8_t>((endTarget >> 8) & 0xFF);
        // M1 fix: 移除 NOT NOT 双重取反，保留操作数原始值（JS 语义）
        return Value::nullValue();
    }

    // 普通二元运算
    compileNode(node.left.get());
    compileNode(node.right.get());

    OpCode op = OpCode::OP_ADD;
    switch (node.opType) {
    case BinOpType::BIN_ADD:  op = OpCode::OP_ADD; break;
    case BinOpType::BIN_SUB:  op = OpCode::OP_SUBTRACT; break;
    case BinOpType::BIN_MUL:  op = OpCode::OP_MULTIPLY; break;
    case BinOpType::BIN_DIV:  op = OpCode::OP_DIVIDE; break;
    case BinOpType::BIN_MOD:  op = OpCode::OP_MODULO; break;
    case BinOpType::BIN_EQ:   op = OpCode::OP_EQUAL; break;
    case BinOpType::BIN_NEQ:  op = OpCode::OP_NOT_EQUAL; break;
    case BinOpType::BIN_LT:   op = OpCode::OP_LESS; break;
    case BinOpType::BIN_GT:   op = OpCode::OP_GREATER; break;
    case BinOpType::BIN_LTE:  op = OpCode::OP_LESS_EQUAL; break;
    case BinOpType::BIN_GTE:  op = OpCode::OP_GREATER_EQUAL; break;
    default:
        error("不支持的运算符: " + std::string(BinaryOp::opTypeStr(node.opType)), node.line, node.column);
        return Value::nullValue();
    }

    chunk_.writeOp(op, node.line);
    return Value::nullValue();
}

Value Compiler::visitUnaryOp(UnaryOp& node) {
    // 常量折叠：编译期求值常量表达式
    Value folded;
    if (tryFoldUnary(node.opType, node.operand.get(), folded, node.line)) {
        emitConstant(folded, node.line);
        return Value::nullValue();
    }

    compileNode(node.operand.get());
    switch (node.opType) {
    case UnaryOp::UnaryOpType::UOP_NEGATE:
        chunk_.writeOp(OpCode::OP_NEGATE, node.line);
        break;
    case UnaryOp::UnaryOpType::UOP_NOT:
        chunk_.writeOp(OpCode::OP_NOT, node.line);
        break;
    case UnaryOp::UnaryOpType::UOP_PLUS:
        break;  // 一元 + 恒等操作，操作数已在栈上
    default:
        break;
    }
    return Value::nullValue();
}

Value Compiler::visitNumberLiteral(NumberLiteral& node) {
    uint16_t idx = chunk_.addConstant(node.value);
    if (node.value.isInt()) {
        chunk_.writeOp(OpCode::OP_INT, node.line);
    } else {
        chunk_.writeOp(OpCode::OP_FLOAT, node.line);
    }
    chunk_.writeShort(idx, node.line);
    return Value::nullValue();
}

Value Compiler::visitStringLiteral(StringLiteral& node) {
    uint16_t idx = chunk_.addConstant(Value(node.value));
    chunk_.writeOp(OpCode::OP_STRING, node.line);
    chunk_.writeShort(idx, node.line);
    return Value::nullValue();
}

Value Compiler::visitBoolLiteral(BoolLiteral& node) {
    chunk_.writeOp(node.value ? OpCode::OP_TRUE : OpCode::OP_FALSE, node.line);
    return Value::nullValue();
}

Value Compiler::visitVarDecl(VarDecl& node) {
    // 编译初始化表达式
    if (node.initializer) {
        compileNode(node.initializer.get());
    } else if (!node.typeAnnotation.empty() &&
               classFieldNames_.find(node.typeAnnotation) != classFieldNames_.end()) {
        // S2 fix: 类名类型注解无初始化器 → 自动构造实例（与解释器 visitVarDecl 一致）
        uint16_t classNameIdx = identifierIndex(node.typeAnnotation);
        chunk_.writeOp(OpCode::OP_CLASS_NEW, node.line);
        chunk_.writeShort(classNameIdx, node.line);
        chunk_.write(static_cast<uint8_t>(0), node.line);  // argCount = 0
    } else {
        chunk_.writeOp(OpCode::OP_NULL, node.line);
    }

    // 在函数体内使用局部变量
    if (inFunction_) {
        auto it = currentLocals_.find(node.name);
        if (it == currentLocals_.end()) {
            // 新局部变量，分配槽位
            int slot = static_cast<int>(currentLocals_.size());
            currentLocals_[node.name] = slot;
            peakLocals_ = std::max(peakLocals_, static_cast<int>(currentLocals_.size()));
            chunk_.writeOp(OpCode::OP_SET_LOCAL, node.line);
            chunk_.write(static_cast<uint8_t>(slot), node.line);
        } else {
            // B2 fix: 与解释器行为一致，同作用域重复声明报错
            error("变量 '" + node.name + "' 已在当前作用域中定义", node.line, node.column);
            chunk_.writeOp(OpCode::OP_SET_LOCAL, node.line);
            chunk_.write(static_cast<uint8_t>(it->second), node.line);
        }
        // OP_SET_LOCAL 用 peek(0) 不消费栈顶值，补发 OP_POP 平衡栈，
        // 避免函数内循环 var 声明累积导致栈溢出。
        chunk_.writeOp(OpCode::OP_POP, node.line);
    } else {
        // A2: use integer slot for known globals
        // Bug2 fix: 块作用域内不使用预分配的全局槽位，避免覆盖外层全局变量
        int slot = (blockDepth_ > 0) ? -1 : lookupGlobalSlot(node.name);
        if (slot >= 0) {
            chunk_.writeOp(OpCode::OP_DEFINE_GLOBAL, node.line);
            chunk_.writeShort(static_cast<uint16_t>(slot), node.line);
        } else {
            uint16_t nameIdx = identifierIndex(node.name);
            chunk_.writeOp(OpCode::OP_DEFINE_VAR, node.line);
            chunk_.writeShort(nameIdx, node.line);
        }
        if (blockDepth_ == 0) {
            topLevelGlobals_.insert(node.name);
        }
    }
    return Value::nullValue();
}

Value Compiler::visitAssignment(Assignment& node) {
    compileNode(node.value.get());

    // C2 fix: 赋值作为表达式应产生值。先 DUP 保留一份在栈上，
    // SET 操作消费原始值后，DUP 的副本留在栈顶供外层表达式使用。
    chunk_.writeOp(OpCode::OP_DUP, node.line);

    if (inFunction_) {
        auto it = currentLocals_.find(node.name);
        if (it != currentLocals_.end()) {
            // OP_SET_LOCAL 用 peek(0) 不消费栈顶值，配合 OP_POP 清理 DUP 的副本
            chunk_.writeOp(OpCode::OP_SET_LOCAL, node.line);
            chunk_.write(static_cast<uint8_t>(it->second), node.line);
            chunk_.writeOp(OpCode::OP_POP, node.line);
        } else {
            // VM-05/06: 尝试解析为 upvalue（闭包捕获外层变量赋值）
            int uvIdx = resolveUpvalue(node.name, node.line);
            if (uvIdx >= 0) {
                chunk_.writeOp(OpCode::OP_SET_UPVALUE, node.line);
                chunk_.write(static_cast<uint8_t>(uvIdx), node.line);
                chunk_.writeOp(OpCode::OP_POP, node.line); // 清理 DUP 副本
                return Value::nullValue();
            }
            // A2: use integer slot for known globals
            int slot = lookupGlobalSlot(node.name);
            if (slot >= 0) {
                chunk_.writeOp(OpCode::OP_SET_GLOBAL, node.line);
                chunk_.writeShort(static_cast<uint16_t>(slot), node.line);
            } else {
                uint16_t nameIdx = identifierIndex(node.name);
                chunk_.writeOp(OpCode::OP_SET_VAR, node.line);
                chunk_.writeShort(nameIdx, node.line);
            }
        }
    } else {
        // A2: use integer slot for known globals
        int slot = lookupGlobalSlot(node.name);
        if (slot >= 0) {
            chunk_.writeOp(OpCode::OP_SET_GLOBAL, node.line);
            chunk_.writeShort(static_cast<uint16_t>(slot), node.line);
        } else {
            uint16_t nameIdx = identifierIndex(node.name);
            chunk_.writeOp(OpCode::OP_SET_VAR, node.line);
            chunk_.writeShort(nameIdx, node.line);
        }
    }
    return Value::nullValue();
}

// VM-05/06: 解析闭包捕获变量为 upvalue 索引
// 返回 upvalue 在 currentUpvalues_ 中的索引，-1 表示不是闭包变量
int Compiler::resolveUpvalue(const std::string& name, int line) {
    // 1. 检查是否已在当前 upvalue 列表中（去重）
    auto existIt = currentUpvalueNames_.find(name);
    if (existIt != currentUpvalueNames_.end()) {
        return existIt->second;
    }

    // 2. 检查外层局部变量（直接捕获）
    auto outerIt = outerLocals_.find(name);
    if (outerIt != outerLocals_.end()) {
        UpvalueDesc desc;
        desc.isLocal = true;
        desc.index = outerIt->second;

        // 3. 检查是否是外层函数的 upvalue（透传）
        auto outerUvIt = outerUpvalueNames_.find(name);
        if (outerUvIt != outerUpvalueNames_.end()) {
            // 外层函数本身也通过 upvalue 捕获该变量 → 透传
            desc.isLocal = false;
            desc.index = outerUvIt->second; // 外层 upvalue 索引
        }

        int idx = static_cast<int>(currentUpvalues_.size());
        currentUpvalues_.push_back(desc);
        currentUpvalueNames_[name] = idx;
        return idx;
    }
    return -1;
}

Value Compiler::visitVarRef(VarRef& node) {
    if (inFunction_) {
        auto it = currentLocals_.find(node.name);
        if (it != currentLocals_.end()) {
            chunk_.writeOp(OpCode::OP_GET_LOCAL, node.line);
            chunk_.write(static_cast<uint8_t>(it->second), node.line);
            return Value::nullValue();
        }
        // VM-05/06: 尝试解析为 upvalue（闭包捕获外层变量）
        int uvIdx = resolveUpvalue(node.name, node.line);
        if (uvIdx >= 0) {
            chunk_.writeOp(OpCode::OP_GET_UPVALUE, node.line);
            chunk_.write(static_cast<uint8_t>(uvIdx), node.line);
            return Value::nullValue();
        }
    }
    // A2: use integer slot for known globals
    int slot = lookupGlobalSlot(node.name);
    if (slot >= 0) {
        chunk_.writeOp(OpCode::OP_GET_GLOBAL, node.line);
        chunk_.writeShort(static_cast<uint16_t>(slot), node.line);
    } else {
        uint16_t nameIdx = identifierIndex(node.name);
        chunk_.writeOp(OpCode::OP_GET_VAR, node.line);
        chunk_.writeShort(nameIdx, node.line);
    }
    return Value::nullValue();
}

Value Compiler::visitIfStmt(IfStmt& node) {
    compileNode(node.condition.get());

    // 条件为假跳转到 else 分支
    size_t elseJumpPatch = chunk_.code.size();
    chunk_.writeOp(OpCode::OP_JUMP_IF_FALSE, node.line);
    chunk_.writeShort(0, node.line);

    // 保存外层作用域，then 分支内声明的变量不泄漏
    auto savedLocals = currentLocals_;

    // 编译 then 分支
    chunk_.writeOp(OpCode::OP_POP, node.line);  // 弹出条件值
    compileStatement(node.thenBranch.get());

    // then 分支变量不泄漏到 else/后续代码
    currentLocals_ = savedLocals;

    // 跳过 else 分支
    size_t endJumpPatch = chunk_.code.size();
    chunk_.writeOp(OpCode::OP_JUMP, node.line);
    chunk_.writeShort(0, node.line);

    // 修补 else 跳转
    uint16_t elseStart = safeCodeOffset();
    chunk_.code[elseJumpPatch + 1] = static_cast<uint8_t>(elseStart & 0xFF);
    chunk_.code[elseJumpPatch + 2] = static_cast<uint8_t>((elseStart >> 8) & 0xFF);

    chunk_.writeOp(OpCode::OP_POP, node.line);  // 弹出条件值

    // 编译 else 分支（使用同样的 savedLocals，then 分支变量不可见）
    if (node.elseBranch) {
        compileStatement(node.elseBranch.get());
    }

    // else 分支变量也不泄漏
    currentLocals_ = savedLocals;

    // 修补 end 跳转
    uint16_t endTarget = safeCodeOffset();
    chunk_.code[endJumpPatch + 1] = static_cast<uint8_t>(endTarget & 0xFF);
    chunk_.code[endJumpPatch + 2] = static_cast<uint8_t>((endTarget >> 8) & 0xFF);
    return Value::nullValue();
}

Value Compiler::visitWhileStmt(WhileStmt& node) {
    // V3 fix: 保存局部变量映射，while 循环体内声明的变量不泄漏
    auto savedLocals = currentLocals_;

    size_t loopStart = chunk_.code.size();

    // 编译条件
    compileNode(node.condition.get());

    // 条件为假跳出循环
    size_t exitJumpPatch = chunk_.code.size();
    chunk_.writeOp(OpCode::OP_JUMP_IF_FALSE, node.line);
    chunk_.writeShort(0, node.line);

    chunk_.writeOp(OpCode::OP_POP, node.line);  // 弹出条件值

    // 编译循环体
    compileStatement(node.body.get());

    // 回跳到条件检查
    uint16_t loopOffset = safeCodeOffset(loopStart);
    chunk_.writeOp(OpCode::OP_LOOP, node.line);
    chunk_.writeShort(loopOffset, node.line);

    // 修补退出跳转
    uint16_t exitTarget = safeCodeOffset();
    chunk_.code[exitJumpPatch + 1] = static_cast<uint8_t>(exitTarget & 0xFF);
    chunk_.code[exitJumpPatch + 2] = static_cast<uint8_t>((exitTarget >> 8) & 0xFF);

    chunk_.writeOp(OpCode::OP_POP, node.line);  // 弹出条件值

    // V3+ fix: 顶层循环退出时清理循环体内声明的全局变量
    // Bug2 fix: 跳过有预分配全局槽位的变量（避免清空外层全局值）
    if (!inFunction_) {
        for (auto& [name, _] : currentLocals_) {
            if (savedLocals.find(name) == savedLocals.end()) {
                if (lookupGlobalSlot(name) < 0) {
                    uint16_t nameIdx = identifierIndex(name);
                    chunk_.writeOp(OpCode::OP_DELETE_VAR, node.line);
                    chunk_.writeShort(nameIdx, node.line);
                }
            }
        }
    }

    // V3 fix: 恢复局部变量映射（P28: swap 避免二次拷贝）
    currentLocals_.swap(savedLocals);
    return Value::nullValue();
}

Value Compiler::visitForStmt(ForStmt& node) {
    // V3 fix: 保存局部变量映射，for 循环内声明的变量不泄漏到外层作用域
    auto savedLocals = currentLocals_;

    // 编译初始化
    if (node.initializer) {
        compileStatement(node.initializer.get());
    }

    size_t loopStart = chunk_.code.size();

    // 编译条件
    if (node.condition) {
        compileNode(node.condition.get());
        size_t exitJumpPatch = chunk_.code.size();
        chunk_.writeOp(OpCode::OP_JUMP_IF_FALSE, node.line);
        chunk_.writeShort(0, node.line);
        chunk_.writeOp(OpCode::OP_POP, node.line);

        // 编译循环体
        compileStatement(node.body.get());

        // 编译更新表达式（compileStatement 会自动 POP 赋值留下的栈值）
        if (node.update) {
            compileStatement(node.update.get());
        }

        // 回跳
        chunk_.writeOp(OpCode::OP_LOOP, node.line);
        chunk_.writeShort(safeCodeOffset(loopStart), node.line);

        // 修补退出跳转
        uint16_t exitTarget = safeCodeOffset();
        chunk_.code[exitJumpPatch + 1] = static_cast<uint8_t>(exitTarget & 0xFF);
        chunk_.code[exitJumpPatch + 2] = static_cast<uint8_t>((exitTarget >> 8) & 0xFF);
        chunk_.writeOp(OpCode::OP_POP, node.line);

        // V3+ fix: 顶层循环退出时清理循环变量
        // Bug2 fix: 跳过有预分配全局槽位的变量（避免清空外层全局值）
        if (!inFunction_) {
            std::vector<std::string> cleanupVars;
            for (auto& [name, _] : currentLocals_) {
                if (savedLocals.find(name) == savedLocals.end()) {
                    cleanupVars.push_back(name);
                }
            }
            for (const auto& name : cleanupVars) {
                if (lookupGlobalSlot(name) < 0) {
                    uint16_t nameIdx = identifierIndex(name);
                    chunk_.writeOp(OpCode::OP_DELETE_VAR, node.line);
                    chunk_.writeShort(nameIdx, node.line);
                }
            }
        }
    } else {
        // 无条件循环（while true）
        chunk_.writeOp(OpCode::OP_TRUE, node.line);
        size_t exitJumpPatch = chunk_.code.size();
        chunk_.writeOp(OpCode::OP_JUMP_IF_FALSE, node.line);
        chunk_.writeShort(0, node.line);
        chunk_.writeOp(OpCode::OP_POP, node.line);

        compileStatement(node.body.get());

        // 编译更新表达式（compileStatement 会自动 POP 赋值留下的栈值）
        if (node.update) {
            compileStatement(node.update.get());
        }

        chunk_.writeOp(OpCode::OP_LOOP, node.line);
        chunk_.writeShort(safeCodeOffset(loopStart), node.line);

        uint16_t exitTarget = safeCodeOffset();
        chunk_.code[exitJumpPatch + 1] = static_cast<uint8_t>(exitTarget & 0xFF);
        chunk_.code[exitJumpPatch + 2] = static_cast<uint8_t>((exitTarget >> 8) & 0xFF);
        chunk_.writeOp(OpCode::OP_POP, node.line);

        // V3+ fix: 顶层循环退出时清理循环变量
        // Bug2 fix: 跳过有预分配全局槽位的变量
        if (!inFunction_) {
            std::vector<std::string> cleanupVars2;
            for (auto& [name, _] : currentLocals_) {
                if (savedLocals.find(name) == savedLocals.end()) {
                    cleanupVars2.push_back(name);
                }
            }
            for (const auto& name : cleanupVars2) {
                if (lookupGlobalSlot(name) < 0) {
                    uint16_t nameIdx = identifierIndex(name);
                    chunk_.writeOp(OpCode::OP_DELETE_VAR, node.line);
                    chunk_.writeShort(nameIdx, node.line);
                }
            }
        }
    }

    // V3 fix: 恢复局部变量映射（P28: swap）
    currentLocals_.swap(savedLocals);
    return Value::nullValue();
}

Value Compiler::visitFunDecl(FunDecl& node) {
    // H5 fix: 记录是否为内嵌函数（在函数体内定义的函数）
    bool isInner = inFunction_;

    // 为函数体创建独立 BytecodeChunk
    BytecodeChunk savedChunk = std::move(chunk_);
    std::unordered_map<std::string, uint16_t> savedVarIndex = std::move(varIndex_);
    std::unordered_map<std::string, int> savedLocals = std::move(currentLocals_);
    bool savedInFunction = inFunction_;
    std::unordered_map<std::string, int> savedOuterLocals = std::move(outerLocals_);
    int savedPeakLocals = peakLocals_;
    // VM-05/06: 保存外层 upvalue 和函数状态
    std::vector<UpvalueDesc> savedOuterUpvalues = std::move(outerUpvalues_);
    std::unordered_map<std::string, int> savedOuterUpvalueNames = std::move(outerUpvalueNames_);
    std::unordered_map<std::string, int> savedOuterFunctions = std::move(outerFunctions_);
    // H5 fix: 保存内嵌函数追踪
    std::unordered_set<std::string> savedInnerFunctions = std::move(innerFunctions_);
    std::unordered_map<std::string, int> savedInnerFunctionSlots = std::move(innerFunctionSlots_);

    // 如果当前在函数内，将当前函数的局部变量保存为外层局部变量（供嵌套函数检测闭包捕获）
    if (inFunction_) {
        outerLocals_ = currentLocals_;
        outerUpvalues_ = currentUpvalues_;
        outerUpvalueNames_ = currentUpvalueNames_;
    } else {
        outerLocals_.clear();
        outerUpvalues_.clear();
        outerUpvalueNames_.clear();
    }
    outerFunctions_.clear();
    if (inFunction_) {
        outerFunctions_ = savedOuterFunctions;
    }

    // 设置函数编译上下文
    chunk_ = BytecodeChunk(node.name, static_cast<int>(node.params.size()));
    chunk_.reserveCode(256);
    varIndex_.clear();
    currentLocals_.clear();
    currentUpvalues_.clear();  // VM-05/06: 新的 upvalue 列表
    currentUpvalueNames_.clear(); // VM-05/06: 新的 upvalue 名称映射
    inFunction_ = true;

    // 编译参数到局部变量槽位
    for (int i = 0; i < static_cast<int>(node.params.size()); ++i) {
        currentLocals_[node.params[i]] = i;
    }
    peakLocals_ = static_cast<int>(node.params.size());

    // 编译函数体
    if (node.body) {
        compileNode(node.body.get());
    }

    // 末尾添加隐式返回 null
    chunk_.writeOp(OpCode::OP_NULL, node.line);
    chunk_.writeOp(OpCode::OP_RETURN, node.line);

    // 记录局部变量总槽位数
    chunk_.localCount = peakLocals_;

    // VM-05/06: 将 upvalue 描述符附加到函数 chunk
    chunk_.upvalues = std::move(currentUpvalues_);

    // 存储函数 chunk
    functionChunks_[node.name] = std::move(chunk_);

    // 恢复主 chunk
    chunk_ = std::move(savedChunk);
    varIndex_ = std::move(savedVarIndex);
    currentLocals_ = std::move(savedLocals);
    inFunction_ = savedInFunction;
    outerLocals_ = std::move(savedOuterLocals);
    peakLocals_ = savedPeakLocals;
    // VM-05/06: 恢复外层 upvalue 和函数状态
    outerUpvalues_ = std::move(savedOuterUpvalues);
    outerUpvalueNames_ = std::move(savedOuterUpvalueNames);
    outerFunctions_ = std::move(savedOuterFunctions);
    // H5 fix: 恢复内嵌函数追踪
    innerFunctions_ = std::move(savedInnerFunctions);
    innerFunctionSlots_ = std::move(savedInnerFunctionSlots);

    // 在主 chunk 中 emit OP_CLOSURE（扩展格式：含 upvalue 描述符）
    uint16_t nameIdx = identifierIndex(node.name);
    const BytecodeChunk& funChunk = functionChunks_[node.name];
    int upvalueCount = static_cast<int>(funChunk.upvalues.size());
    chunk_.writeOp(OpCode::OP_CLOSURE, node.line);
    chunk_.writeShort(nameIdx, node.line);
    chunk_.write(static_cast<uint8_t>(upvalueCount), node.line); // 改为 upvalue 数量
    // 写入每个 upvalue 的描述符
    for (int i = 0; i < upvalueCount; ++i) {
        chunk_.write(funChunk.upvalues[i].isLocal ? 1 : 0, node.line);
        chunk_.write(static_cast<uint8_t>(funChunk.upvalues[i].index), node.line);
    }
    // H5 fix: 内嵌函数存储为局部变量（供 OP_CALL_EXPR 使用），全局函数直接弹出
    if (isInner) {
        // 分配局部变量槽位存储闭包值
        int slot = static_cast<int>(currentLocals_.size());
        currentLocals_[node.name] = slot;
        peakLocals_ = std::max(peakLocals_, static_cast<int>(currentLocals_.size()));
        innerFunctions_.insert(node.name);
        innerFunctionSlots_[node.name] = slot;
        chunk_.writeOp(OpCode::OP_SET_LOCAL, node.line);
        chunk_.write(static_cast<uint8_t>(slot), node.line);
        chunk_.writeOp(OpCode::OP_POP, node.line);
    } else {
        // 全局函数：OP_CALL 通过函数名查找，不需要栈上的闭包值
        chunk_.writeOp(OpCode::OP_POP, node.line);
    }

    // VM-05/06: 如果在函数内，将本函数注册到 outerLocals_ 和 outerFunctions_ 供更内层捕获
    if (inFunction_) {
        // H5 fix: 内嵌函数已有真实槽位，使用它；全局函数使用虚拟槽位
        int slot = isInner ? innerFunctionSlots_[node.name] : peakLocals_;
        outerLocals_[node.name] = slot;
        outerFunctions_[node.name] = slot;
    }
    return Value::nullValue();
}

Value Compiler::visitFunCall(FunCall& node) {
    // 链式调用 / 表达式调用: callee(args)
    if (node.callee) {
        // 编译被调用表达式（结果应为闭包值，推入栈顶）
        compileNode(node.callee.get());
        // 编译参数
        for (auto& arg : node.arguments) {
            compileNode(arg.get());
        }
        if (node.arguments.size() > 255) {
            error("函数调用参数数量超过限制（最大 255 个）", node.line, 0);
        }
        // OP_CALL_EXPR: 栈顶 N 个参数下方为闭包值
        chunk_.writeOp(OpCode::OP_CALL_EXPR, node.line);
        chunk_.write(static_cast<uint8_t>(node.arguments.size()), node.line);
        return Value::nullValue();
    }

    // H5 fix: 内嵌函数通过局部变量中的闭包值调用，避免 functionClosures_ 按名称覆盖
    auto innerIt = innerFunctionSlots_.find(node.name);
    if (innerIt != innerFunctionSlots_.end()) {
        // 先 push 闭包值（从局部变量获取）
        chunk_.writeOp(OpCode::OP_GET_LOCAL, node.line);
        chunk_.write(static_cast<uint8_t>(innerIt->second), node.line);
        // 再编译参数
        for (auto& arg : node.arguments) {
            compileNode(arg.get());
        }
        if (node.arguments.size() > 255) {
            error("函数调用参数数量超过限制（最大 255 个）", node.line, 0);
        }
        chunk_.writeOp(OpCode::OP_CALL_EXPR, node.line);
        chunk_.write(static_cast<uint8_t>(node.arguments.size()), node.line);
        return Value::nullValue();
    }

    // 编译参数
    for (auto& arg : node.arguments) {
        compileNode(arg.get());
    }

    // 函数名作为常量
    uint16_t nameIdx = identifierIndex(node.name);

    if (node.arguments.size() > 255) {
        error("函数调用参数数量超过限制（最大 255 个）", node.line, 0);
    }
    // 使用 OP_CALL 指令
    chunk_.writeOp(OpCode::OP_CALL, node.line);
    chunk_.write(static_cast<uint8_t>(nameIdx & 0xFF), node.line);
    chunk_.write(static_cast<uint8_t>((nameIdx >> 8) & 0xFF), node.line);
    chunk_.write(static_cast<uint8_t>(node.arguments.size()), node.line);
    return Value::nullValue();
}

Value Compiler::visitReturnStmt(ReturnStmt& node) {
    if (!inFunction_) {
        error("return 只能在函数体内使用", node.line, 0);
        return Value::nullValue();
    }
    if (node.value) {
        compileNode(node.value.get());
    } else {
        chunk_.writeOp(OpCode::OP_NULL, node.line);
    }
    chunk_.writeOp(OpCode::OP_RETURN, node.line);
    return Value::nullValue();
}

Value Compiler::visitPrintStmt(PrintStmt& node) {
    // C2 fix: 与解释器行为对齐 — 多参数用空格拼接后单次输出
    if (node.values.empty()) {
        // print() → 输出空行（与解释器 output("") 一致）
        uint16_t emptyIdx = chunk_.addConstant(Value(std::string("")));
        chunk_.writeOp(OpCode::OP_CONSTANT, node.line);
        chunk_.writeShort(emptyIdx, node.line);
        chunk_.writeOp(OpCode::OP_PRINT, node.line);
    } else if (node.values.size() == 1) {
        compileNode(node.values[0].get());
        chunk_.writeOp(OpCode::OP_PRINT, node.line);
    } else {
        // 多值：用 OP_ADD 拼接为单字符串（OP_ADD 已支持 string+non-string 拼接）
        compileNode(node.values[0].get());
        for (size_t i = 1; i < node.values.size(); ++i) {
            // push " " + OP_ADD → 左侧被 toString 后与空格拼接
            uint16_t spaceIdx = chunk_.addConstant(Value(std::string(" ")));
            chunk_.writeOp(OpCode::OP_CONSTANT, node.line);
            chunk_.writeShort(spaceIdx, node.line);
            chunk_.writeOp(OpCode::OP_ADD, node.line);
            // push next value + OP_ADD
            compileNode(node.values[i].get());
            chunk_.writeOp(OpCode::OP_ADD, node.line);
        }
        chunk_.writeOp(OpCode::OP_PRINT, node.line);
    }
    return Value::nullValue();
}

Value Compiler::visitBlock(Block& node) {
    auto savedLocals = currentLocals_;

    if (!inFunction_) {
        blockDepth_++;

        // 收集块作用域中将要声明的变量名，以便在编译前保存被遮蔽的全局变量
        std::vector<std::pair<std::string, std::string>> shadowedSaves; // (blockVarName, tempSaveName)
        std::vector<std::pair<std::string, int>> removedSlots; // H1: (name, slot) 用于恢复
        for (auto& stmt : node.statements) {
            if (stmt && stmt->nodeType == NodeType::NODE_VAR_DECL) {
                VarDecl* vd = static_cast<VarDecl*>(stmt.get());
                int gsSlot = lookupGlobalSlot(vd->name);
                if (gsSlot >= 0) {
                    std::string saveName = "__blk_save_" + std::to_string(blockDepth_) + "_"
                        + std::to_string(blockSaveCounter_++) + "_" + vd->name;
                    shadowedSaves.push_back({vd->name, saveName});
                    // A2: 使用槽位操作码保存全局变量值
                    uint16_t saveIdx = identifierIndex(saveName);
                    chunk_.writeOp(OpCode::OP_GET_GLOBAL, vd->line);
                    chunk_.writeShort(static_cast<uint16_t>(gsSlot), vd->line);
                    chunk_.writeOp(OpCode::OP_DEFINE_VAR, vd->line);
                    chunk_.writeShort(saveIdx, vd->line);
                }
            }
        }

        // H1 fix: 保存全局值后，移除被遮蔽的全局槽位条目
        // 使块内 compileVarDecl/compileVarRef/compileAssignment 不命中全局槽位，
        // 改用 OP_DEFINE_VAR/OP_GET_VAR/OP_SET_VAR → globals_ 路径
        for (auto& [varName, saveName] : shadowedSaves) {
            auto it = globalSlots_.find(varName);
            if (it != globalSlots_.end()) {
                removedSlots.push_back({varName, it->second});
                globalSlots_.erase(it);
            }
        }

        // 编译块体
        for (auto& stmt : node.statements) {
            compileStatement(stmt.get());
        }

        // 找出块作用域内新声明的变量
        std::vector<std::string> blockVars;
        for (auto& [name, slot] : currentLocals_) {
            if (savedLocals.find(name) == savedLocals.end()) {
                blockVars.push_back(name);
            }
        }

        // 恢复外层作用域（P28: swap）
        currentLocals_.swap(savedLocals);
        blockDepth_--;

        // H1 fix: 恢复被移除的全局槽位条目（必须在恢复字节码之前，使 lookupGlobalSlot 正确）
        for (auto& [name, slot] : removedSlots) {
            globalSlots_[name] = slot;
        }

        // 清理块作用域变量并恢复被遮蔽的全局变量
        for (auto& [varName, saveName] : shadowedSaves) {
            // A2: 使用槽位操作码恢复全局变量值
            int gsSlot = lookupGlobalSlot(varName);
            uint16_t saveIdx = identifierIndex(saveName);
            chunk_.writeOp(OpCode::OP_GET_VAR, node.line);
            chunk_.writeShort(saveIdx, node.line);
            if (gsSlot >= 0) {
                chunk_.writeOp(OpCode::OP_SET_GLOBAL, node.line);
                chunk_.writeShort(static_cast<uint16_t>(gsSlot), node.line);
            } else {
                uint16_t origIdx = identifierIndex(varName);
                chunk_.writeOp(OpCode::OP_SET_VAR, node.line);
                chunk_.writeShort(origIdx, node.line);
            }
            // 删除临时保存变量
            chunk_.writeOp(OpCode::OP_DELETE_VAR, node.line);
            chunk_.writeShort(saveIdx, node.line);
        }

        // 删除块作用域变量（含被遮蔽变量的 globals_ 残留值）
        for (auto& name : blockVars) {
            uint16_t nameIdx = identifierIndex(name);
            chunk_.writeOp(OpCode::OP_DELETE_VAR, node.line);
            chunk_.writeShort(nameIdx, node.line);
        }
    } else {
        // 函数内块作用域：局部变量使用栈槽，无需清理（VM 帧退出时自动释放）
        for (auto& stmt : node.statements) {
            compileStatement(stmt.get());
        }
        currentLocals_.swap(savedLocals);  // P28: swap
    }
    return Value::nullValue();
}

// ---- 新增节点编译 ----

Value Compiler::visitArrayLiteral(ArrayLiteral& node) {
    // 检查元素数量上限（uint8_t 编码限制）
    if (node.elements.size() > 255) {
        error("数组元素数量超过 255 个上限", node.line, node.column);
        return Value::nullValue();
    }
    // 编译所有元素
    for (auto& elem : node.elements) {
        compileNode(elem.get());
    }
    // 构建数组指令
    chunk_.writeOp(OpCode::OP_BUILD_ARRAY, node.line);
    chunk_.write(static_cast<uint8_t>(node.elements.size()), node.line);
    return Value::nullValue();
}

Value Compiler::visitDictLiteral(DictLiteral& node) {
    // 检查键值对数量上限（uint8_t 编码限制）
    if (node.pairs.size() > 255) {
        error("字典键值对数量超过 255 个上限", node.line, node.column);
        return Value::nullValue();
    }
    // 编译所有键值对（先键后值，与 OP_BUILD_DICT 消费顺序一致）
    for (auto& pair : node.pairs) {
        compileNode(pair.first.get());
        compileNode(pair.second.get());
    }
    // 构建字典指令：弹出 2*count 个值，构建字典，push 到栈
    chunk_.writeOp(OpCode::OP_BUILD_DICT, node.line);
    chunk_.write(static_cast<uint8_t>(node.pairs.size()), node.line);
    return Value::nullValue();
}

Value Compiler::visitIndexAccess(IndexAccess& node) {
    compileNode(node.object.get());
    compileNode(node.index.get());
    chunk_.writeOp(OpCode::OP_INDEX_GET, node.line);
    return Value::nullValue();
}

Value Compiler::visitIndexAssign(IndexAssign& node) {
    // 根据左值对象类型选择赋值策略
    VarRef* objVar = (node.object && node.object->nodeType == NodeType::NODE_VAR_REF)
                     ? static_cast<VarRef*>(node.object.get()) : nullptr;

    if (objVar) {
        // 简单路径：arr[i] = val（基是 VarRef）
        auto localIt = currentLocals_.find(objVar->name);
        if (localIt != currentLocals_.end()) {
            compileNode(node.index.get());
            compileNode(node.value.get());
            uint8_t slot = static_cast<uint8_t>(localIt->second);
            chunk_.writeOp(OpCode::OP_INDEX_SET_LOCAL, node.line);
            chunk_.write(slot, node.line);
        } else {
            compileNode(node.index.get());
            compileNode(node.value.get());
            uint16_t nameIdx = identifierIndex(objVar->name);
            chunk_.writeOp(OpCode::OP_INDEX_SET_VAR, node.line);
            chunk_.writeShort(nameIdx, node.line);
        }
        return Value::nullValue();
    }

    // M1 fix: 嵌套索引赋值（2 层）
    // 检测 node.object 是否是 IndexAccess(VarRef) 或 MemberAccess(VarRef)
    IndexAccess* outerIdx = (node.object && node.object->nodeType == NodeType::NODE_INDEX_ACCESS)
                            ? static_cast<IndexAccess*>(node.object.get()) : nullptr;
    MemberAccess* outerMem = (node.object && node.object->nodeType == NodeType::NODE_MEMBER_ACCESS)
                             ? static_cast<MemberAccess*>(node.object.get()) : nullptr;
    VarRef* baseVar = nullptr;
    if (outerIdx && outerIdx->object && outerIdx->object->nodeType == NodeType::NODE_VAR_REF)
        baseVar = static_cast<VarRef*>(outerIdx->object.get());
    else if (outerMem && outerMem->object && outerMem->object->nodeType == NodeType::NODE_VAR_REF)
        baseVar = static_cast<VarRef*>(outerMem->object.get());

    if (baseVar) {
        auto localIt = currentLocals_.find(baseVar->name);
        bool isLocal = (localIt != currentLocals_.end());

        // 编译基变量 → push base
        compileNode(baseVar);
        // 编译外层表达式 → push base[outerIdx] 或 base.field（正确求值中间值）
        compileNode(node.object.get());
        // 编译内层索引 → push innerIdx
        compileNode(node.index.get());
        // 编译值 → push val
        compileNode(node.value.get());
        // OP_INDEX_SET: 弹出 val/innerIdx/outerValue → 修改 → lastMutatedReceiver_
        chunk_.writeOp(OpCode::OP_INDEX_SET, node.line);
        // 推外层索引（write-back 需要）
        if (outerIdx) {
            compileNode(outerIdx->index.get());
        } else {
            uint16_t fieldIdx = identifierIndex(outerMem->fieldName);
            chunk_.writeOp(OpCode::OP_STRING, node.line);
            chunk_.writeShort(fieldIdx, node.line);
        }
        // write-back: 将 lastMutatedReceiver_ 写回基变量
        if (isLocal) {
            if (outerIdx) {
                chunk_.writeOp(OpCode::OP_WRITEBACK_INDEX_LOCAL, node.line);
                chunk_.write(static_cast<uint8_t>(localIt->second), node.line);
            } else {
                chunk_.writeOp(OpCode::OP_WRITEBACK_MEMBER_LOCAL, node.line);
                chunk_.write(static_cast<uint8_t>(localIt->second), node.line);
                uint16_t fieldIdx = identifierIndex(outerMem->fieldName);
                chunk_.writeShort(fieldIdx, node.line);
            }
        } else {
            if (outerIdx) {
                uint16_t nameIdx = identifierIndex(baseVar->name);
                chunk_.writeOp(OpCode::OP_WRITEBACK_INDEX_VAR, node.line);
                chunk_.writeShort(nameIdx, node.line);
            } else {
                uint16_t varIdx = identifierIndex(baseVar->name);
                uint16_t fieldIdx = identifierIndex(outerMem->fieldName);
                chunk_.writeOp(OpCode::OP_WRITEBACK_MEMBER_VAR, node.line);
                chunk_.writeShort(varIdx, node.line);
                chunk_.writeShort(fieldIdx, node.line);
            }
        }
        return Value::nullValue();
    }

    // 3+ 层嵌套或复杂表达式：通用路径（仍报错）
    compileNode(node.object.get());
    compileNode(node.index.get());
    compileNode(node.value.get());
    chunk_.writeOp(OpCode::OP_INDEX_SET, node.line);
    return Value::nullValue();
}

Value Compiler::visitClassDecl(ClassDecl& node) {
    // 类声明：发射 OP_CLASS_NEW + OP_INIT_FIELD 初始化字段 + 编译方法
    uint16_t nameIdx = identifierIndex(node.name);

    // 收集当前类的自有字段名
    std::vector<std::string> ownFieldNames;
    for (auto& member : node.members) {
        if (member->nodeType == NodeType::NODE_VAR_DECL) {
            ownFieldNames.push_back(static_cast<VarDecl*>(member.get())->name);
        }
    }

    // 构建含继承字段的完整字段列表（父类字段在前，子类字段在后）
    std::vector<std::string> allFieldNames;
    if (!node.superClassName.empty()) {
        auto it = classFieldNames_.find(node.superClassName);
        if (it != classFieldNames_.end()) {
            allFieldNames = it->second;  // 父类字段在前
        }
    }
    for (const auto& fn : ownFieldNames) {
        if (std::find(allFieldNames.begin(), allFieldNames.end(), fn) == allFieldNames.end()) {
            allFieldNames.push_back(fn);  // 子类字段在后（跳过覆盖的同名字段）
        }
    }

    // 注册类字段名（供子类编译时查找）
    classFieldNames_[node.name] = allFieldNames;

    // 先编译所有方法为独立 chunk
    for (auto& member : node.members) {
        if (member->nodeType != NodeType::NODE_FUN_DECL) continue;
        FunDecl* funDecl = static_cast<FunDecl*>(member.get());
        std::string methodKey = node.name + "." + funDecl->name;
        BytecodeChunk savedChunk = std::move(chunk_);
        std::unordered_map<std::string, uint16_t> savedVarIndex = std::move(varIndex_);
        std::unordered_map<std::string, int> savedLocals = std::move(currentLocals_);
        std::unordered_map<std::string, int> savedOuterLocals = std::move(outerLocals_);
        bool savedInFunction = inFunction_;
        int savedPeakLocals = peakLocals_;
        std::string savedClassName = currentClassName_;  // B1 fix

        chunk_ = BytecodeChunk(methodKey, static_cast<int>(funDecl->params.size()));
        chunk_.reserveCode(256);  // C21: 预分配方法字节码空间
        varIndex_.clear();
        currentLocals_.clear();
        currentClassName_ = node.name;  // B1 fix: 记录当前类名供 super 使用
        // O5: 如果类定义在函数内，设置 outerLocals_ 以检测不支持的闭包捕获
        if (savedInFunction) {
            outerLocals_ = savedLocals;
        } else {
            outerLocals_.clear();
        }
        inFunction_ = true;

        // 局部变量映射：slot 0 = this，slot 1..N = 字段（含继承字段），slot N+1.. = 参数
        int slot = 0;
        currentLocals_["this"] = slot++;  // slot 0: this
        for (const auto& fieldName : allFieldNames) {
            currentLocals_[fieldName] = slot++;  // slot 1..N: 实例字段（含继承）
        }
        for (int i = 0; i < static_cast<int>(funDecl->params.size()); ++i) {
            currentLocals_[funDecl->params[i]] = slot++;  // slot N+1..: 参数
        }
        peakLocals_ = slot;

        // 记录字段声明顺序（含继承字段），供 VM OP_METHOD_CALL 按序推入
        chunk_.fieldOrder = allFieldNames;

        if (funDecl->body) {
            compileNode(funDecl->body.get());
        }

        chunk_.writeOp(OpCode::OP_NULL, funDecl->line);
        chunk_.writeOp(OpCode::OP_RETURN, funDecl->line);

        // 记录局部变量总槽位数（含 this/字段/参数和方法体内 var 声明），供 VM 预分配栈空间
        chunk_.localCount = peakLocals_;

        functionChunks_[methodKey] = std::move(chunk_);

        chunk_ = std::move(savedChunk);
        varIndex_ = std::move(savedVarIndex);
        currentLocals_ = std::move(savedLocals);
        outerLocals_ = std::move(savedOuterLocals);
        inFunction_ = savedInFunction;
        peakLocals_ = savedPeakLocals;
        currentClassName_ = savedClassName;  // B1 fix
    }

    // 主 chunk 中：发射 OP_CLASS_NEW（0 参数构造，字段由 OP_INIT_FIELD 设置）
    chunk_.writeOp(OpCode::OP_CLASS_NEW, node.line);
    chunk_.writeShort(nameIdx, node.line);
    chunk_.write(static_cast<uint8_t>(0), node.line);  // argCount = 0（字段由 OP_INIT_FIELD 初始化）

    // 此时栈顶是刚创建的空实例，发射 OP_INIT_FIELD 设置每个字段的默认值
    for (auto& member : node.members) {
        if (member->nodeType != NodeType::NODE_VAR_DECL) continue;
        VarDecl* varDecl = static_cast<VarDecl*>(member.get());
        // 编译字段默认值表达式
        if (varDecl->initializer) {
            compileNode(varDecl->initializer.get());
        } else {
            chunk_.writeOp(OpCode::OP_NULL, varDecl->line);
        }
        // 发射 OP_INIT_FIELD：pop 默认值，设置到栈顶实例的字段中
        uint16_t fieldIdx = identifierIndex(varDecl->name);
        chunk_.writeOp(OpCode::OP_INIT_FIELD, varDecl->line);
        chunk_.writeShort(fieldIdx, varDecl->line);
    }

    // 将类注册为全局变量（OP_DEFINE_CLASS 从栈上 pop 模板实例并注册类信息）
    // 操作数: nameIdx(2B) + superNameIdx(2B)
    //   superNameIdx == 0xFFFF 表示无父类；否则为父类名在常量池中的索引
    chunk_.writeOp(OpCode::OP_DEFINE_CLASS, node.line);
    chunk_.writeShort(nameIdx, node.line);
    if (node.superClassName.empty()) {
        chunk_.writeShort(0xFFFF, node.line);  // 无父类标记
    } else {
        uint16_t superIdx = identifierIndex(node.superClassName);
        chunk_.writeShort(superIdx, node.line);
    }

    // 栈上的模板实例已被 OP_DEFINE_CLASS 消费，无需额外 OP_POP
    return Value::nullValue();
}

Value Compiler::visitMemberAccess(MemberAccess& node) {
    compileNode(node.object.get());
    uint16_t nameIdx = identifierIndex(node.fieldName);
    // O1: super.field — 字段已在构造时通过继承链复制到实例中，与 this.field 等价
    // 方法调用走 OP_SUPER_CALL，此处仅处理字段读取
    bool isSuperAccess = (node.object && node.object->nodeType == NodeType::NODE_SUPER_EXPR);
    chunk_.writeOp(isSuperAccess ? OpCode::OP_SUPER_MEMBER_GET : OpCode::OP_MEMBER_GET, node.line);
    chunk_.writeShort(nameIdx, node.line);
    return Value::nullValue();
}

Value Compiler::visitMemberAssign(MemberAssign& node) {
    // 根据左值对象类型选择赋值策略
    VarRef* objVar = (node.object && node.object->nodeType == NodeType::NODE_VAR_REF)
                     ? static_cast<VarRef*>(node.object.get()) : nullptr;

    if (objVar) {
        // 简单路径：obj.field = val（基是 VarRef）
        auto localIt = currentLocals_.find(objVar->name);
        if (localIt != currentLocals_.end()) {
            compileNode(node.value.get());
            uint8_t slot = static_cast<uint8_t>(localIt->second);
            uint16_t fieldIdx = identifierIndex(node.fieldName);
            chunk_.writeOp(OpCode::OP_MEMBER_SET_LOCAL, node.line);
            chunk_.write(slot, node.line);
            chunk_.writeShort(fieldIdx, node.line);
            return Value::nullValue();
        }
        compileNode(node.value.get());
        uint16_t varIdx = identifierIndex(objVar->name);
        uint16_t fieldIdx = identifierIndex(node.fieldName);
        chunk_.writeOp(OpCode::OP_MEMBER_SET_VAR, node.line);
        chunk_.writeShort(varIdx, node.line);
        chunk_.writeShort(fieldIdx, node.line);
        return Value::nullValue();
    }

    // M1 fix: 嵌套成员赋值（2 层）
    // 检测 node.object 是否是 IndexAccess(VarRef) 或 MemberAccess(VarRef)
    IndexAccess* outerIdx = (node.object && node.object->nodeType == NodeType::NODE_INDEX_ACCESS)
                            ? static_cast<IndexAccess*>(node.object.get()) : nullptr;
    MemberAccess* outerMem = (node.object && node.object->nodeType == NodeType::NODE_MEMBER_ACCESS)
                             ? static_cast<MemberAccess*>(node.object.get()) : nullptr;
    VarRef* baseVar = nullptr;
    if (outerIdx && outerIdx->object && outerIdx->object->nodeType == NodeType::NODE_VAR_REF)
        baseVar = static_cast<VarRef*>(outerIdx->object.get());
    else if (outerMem && outerMem->object && outerMem->object->nodeType == NodeType::NODE_VAR_REF)
        baseVar = static_cast<VarRef*>(outerMem->object.get());

    if (baseVar) {
        auto localIt = currentLocals_.find(baseVar->name);
        bool isLocal = (localIt != currentLocals_.end());

        // 编译基变量 → push base
        compileNode(baseVar);
        // 编译外层表达式 → push base[outerIdx] 或 base.field
        compileNode(node.object.get());
        // 编译值 → push val
        compileNode(node.value.get());
        // OP_MEMBER_SET: 弹出 val/outerValue → 修改 → lastMutatedReceiver_
        uint16_t fieldNameIdx = identifierIndex(node.fieldName);
        chunk_.writeOp(OpCode::OP_MEMBER_SET, node.line);
        chunk_.writeShort(fieldNameIdx, node.line);
        // 推外层索引（write-back 需要）
        if (outerIdx) {
            compileNode(outerIdx->index.get());
        } else {
            uint16_t outerFieldIdx = identifierIndex(outerMem->fieldName);
            chunk_.writeOp(OpCode::OP_STRING, node.line);
            chunk_.writeShort(outerFieldIdx, node.line);
        }
        // write-back: 将 lastMutatedReceiver_ 写回基变量
        if (isLocal) {
            if (outerIdx) {
                chunk_.writeOp(OpCode::OP_WRITEBACK_INDEX_LOCAL, node.line);
                chunk_.write(static_cast<uint8_t>(localIt->second), node.line);
            } else {
                chunk_.writeOp(OpCode::OP_WRITEBACK_MEMBER_LOCAL, node.line);
                chunk_.write(static_cast<uint8_t>(localIt->second), node.line);
                uint16_t outerFieldIdx = identifierIndex(outerMem->fieldName);
                chunk_.writeShort(outerFieldIdx, node.line);
            }
        } else {
            if (outerIdx) {
                uint16_t nameIdx = identifierIndex(baseVar->name);
                chunk_.writeOp(OpCode::OP_WRITEBACK_INDEX_VAR, node.line);
                chunk_.writeShort(nameIdx, node.line);
            } else {
                uint16_t varIdx = identifierIndex(baseVar->name);
                uint16_t outerFieldIdx = identifierIndex(outerMem->fieldName);
                chunk_.writeOp(OpCode::OP_WRITEBACK_MEMBER_VAR, node.line);
                chunk_.writeShort(varIdx, node.line);
                chunk_.writeShort(outerFieldIdx, node.line);
            }
        }
        return Value::nullValue();
    }

    // 3+ 层嵌套或复杂表达式：通用路径
    compileNode(node.object.get());
    compileNode(node.value.get());
    uint16_t nameIdx = identifierIndex(node.fieldName);
    chunk_.writeOp(OpCode::OP_MEMBER_SET, node.line);
    chunk_.writeShort(nameIdx, node.line);
    return Value::nullValue();
}

Value Compiler::visitMethodCall(MethodCall& node) {
    // 检查接收者是否为简单变量（VarRef），用于 writeBack
    uint16_t receiverVarIdx = 0xFFFF;  // 0xFFFF = 无全局变量 writeBack
    uint8_t receiverLocalSlot = 0xFF;  // 0xFF = 无局部变量 writeBack
    VarRef* objVar = (node.object && node.object->nodeType == NodeType::NODE_VAR_REF)
                     ? static_cast<VarRef*>(node.object.get()) : nullptr;
    if (objVar) {
        auto localIt = currentLocals_.find(objVar->name);
        if (localIt == currentLocals_.end()) {
            // 全局变量：记录变量名索引供 VM writeBack 使用
            receiverVarIdx = identifierIndex(objVar->name);
        } else {
            // 局部变量：记录 slot 号供 VM writeBack 使用
            receiverLocalSlot = static_cast<uint8_t>(localIt->second);
        }
    }

    // H4 fix: super.method() 的接收者是 this（始终在 slot 0），设置写回目标
    if (!objVar && node.object && node.object->nodeType == NodeType::NODE_SUPER_EXPR
        && inFunction_) {
        receiverLocalSlot = 0;  // slot 0 = this
    }

    // B6 fix: 如果接收者是 IndexAccess，预缓存索引值避免写回时重复求值
    // （对 arr[expr()].method() 这类调用，expr() 只执行一次）
    std::string cachedIndexVar;
    if (!objVar && node.object && node.object->nodeType == NodeType::NODE_INDEX_ACCESS) {
        auto* ia = static_cast<IndexAccess*>(node.object.get());
        if (ia->object && ia->object->nodeType == NodeType::NODE_VAR_REF && ia->index) {
            cachedIndexVar = "__wb_idx_" + std::to_string(writebackCounter_++);
            compileNode(ia->index.get());
            uint16_t cacheIdx = identifierIndex(cachedIndexVar);
            chunk_.writeOp(OpCode::OP_DEFINE_VAR, node.line);  // #9 fix: DEFINE 而非 SET（首次创建变量）
            chunk_.writeShort(cacheIdx, node.line);
        }
    }

    // B6 fix: 如果索引已缓存，手动内联 IndexAccess 编译（用缓存值代替重新求值）
    if (!cachedIndexVar.empty()) {
        auto* ia = static_cast<IndexAccess*>(node.object.get());
        compileNode(ia->object.get());  // push base (e.g., arr)
        uint16_t cacheIdx = identifierIndex(cachedIndexVar);
        chunk_.writeOp(OpCode::OP_GET_VAR, node.line);
        chunk_.writeShort(cacheIdx, node.line);  // push cached index
        chunk_.writeOp(OpCode::OP_INDEX_GET, node.line);  // base[cachedIndex]
    } else {
        compileNode(node.object.get());
    }
    for (auto& arg : node.arguments) {
        compileNode(arg.get());
    }
    uint16_t nameIdx = identifierIndex(node.methodName);
    // O1: super.method() 使用 OP_SUPER_CALL（从父类开始方法查找）
    bool isSuperCall = (node.object && node.object->nodeType == NodeType::NODE_SUPER_EXPR);
    chunk_.writeOp(isSuperCall ? OpCode::OP_SUPER_CALL : OpCode::OP_METHOD_CALL, node.line);
    chunk_.writeShort(nameIdx, node.line);
    chunk_.write(static_cast<uint8_t>(node.arguments.size()), node.line);
    chunk_.writeShort(receiverVarIdx, node.line);  // 接收者全局变量名索引（0xFFFF = 无全局 writeBack）
    chunk_.write(receiverLocalSlot, node.line);    // 接收者局部变量 slot（0xFF = 无局部 writeBack）
    if (isSuperCall) {
        // B1 fix: 编码当前类名索引，VM 用它查找父类（而非运行时实例类名）
        uint16_t classIdx = identifierIndex(currentClassName_);
        chunk_.writeShort(classIdx, node.line);
    }

    // 嵌套访问变异方法写回：当接收者是 MemberAccess/IndexAccess 且基对象是 VarRef 时，
    // 方法调用后发射写回指令，确保 this.arr.push(42) 等嵌套调用的修改不丢失。
    // VM 在变异方法调用时将修改后的对象暂存到 lastMutatedReceiver_，
    // 写回指令从中取值写回基对象的字段/索引位置。
    if (!objVar && node.object) {
        // MemberAccess 嵌套写回：如 this.arr.push(42)、obj.field.pop()
        if (node.object->nodeType == NodeType::NODE_MEMBER_ACCESS) {
            auto* ma = static_cast<MemberAccess*>(node.object.get());
            if (ma->object && ma->object->nodeType == NodeType::NODE_VAR_REF) {
                auto* baseVar = static_cast<VarRef*>(ma->object.get());
                uint16_t fieldIdx = identifierIndex(ma->fieldName);
                auto localIt = currentLocals_.find(baseVar->name);
                if (localIt != currentLocals_.end()) {
                    // 局部变量成员写回：OP_WRITEBACK_MEMBER_LOCAL(slot, fieldIdx)
                    chunk_.writeOp(OpCode::OP_WRITEBACK_MEMBER_LOCAL, node.line);
                    chunk_.write(static_cast<uint8_t>(localIt->second), node.line);
                    chunk_.writeShort(fieldIdx, node.line);
                } else {
                    // 全局变量成员写回：OP_WRITEBACK_MEMBER_VAR(varIdx, fieldIdx)
                    chunk_.writeOp(OpCode::OP_WRITEBACK_MEMBER_VAR, node.line);
                    chunk_.writeShort(identifierIndex(baseVar->name), node.line);
                    chunk_.writeShort(fieldIdx, node.line);
                }
            }
        }
        // IndexAccess 嵌套写回：如 arr[0].push(42)、dict["key"].remove("x")
        // B6 fix: 使用预缓存的索引值，避免重复求值（对 arr[expr()].method() 防止副作用执行两次）
        else if (node.object->nodeType == NodeType::NODE_INDEX_ACCESS) {
            auto* ia = static_cast<IndexAccess*>(node.object.get());
            if (ia->object && ia->object->nodeType == NodeType::NODE_VAR_REF) {
                auto* baseVar = static_cast<VarRef*>(ia->object.get());
                auto localIt = currentLocals_.find(baseVar->name);
                // 推入缓存的索引值（OP_WRITEBACK_INDEX_LOCAL/VAR 需要索引在栈上）
                if (!cachedIndexVar.empty()) {
                    uint16_t cacheIdx = identifierIndex(cachedIndexVar);
                    chunk_.writeOp(OpCode::OP_GET_VAR, node.line);
                    chunk_.writeShort(cacheIdx, node.line);
                } else {
                    // 降级路径：不应到达此处（cachedIndexVar 总会在上方设置）
                    compileNode(ia->index.get());
                }
                if (localIt != currentLocals_.end()) {
                    // 局部变量索引写回：OP_WRITEBACK_INDEX_LOCAL(slot)
                    chunk_.writeOp(OpCode::OP_WRITEBACK_INDEX_LOCAL, node.line);
                    chunk_.write(static_cast<uint8_t>(localIt->second), node.line);
                } else {
                    // 全局变量索引写回：OP_WRITEBACK_INDEX_VAR(varIdx)
                    chunk_.writeOp(OpCode::OP_WRITEBACK_INDEX_VAR, node.line);
                    chunk_.writeShort(identifierIndex(baseVar->name), node.line);
                }
            }
        }
    }

    // #9 fix: 清理 __wb_idx_ 缓存变量，防止永久泄漏到 globals_
    if (!cachedIndexVar.empty()) {
        uint16_t cacheIdx = identifierIndex(cachedIndexVar);
        chunk_.writeOp(OpCode::OP_DELETE_VAR, node.line);
        chunk_.writeShort(cacheIdx, node.line);
    }
    return Value::nullValue();
}

Value Compiler::visitNullLiteral(NullLiteral& node) {
    chunk_.writeOp(OpCode::OP_NULL, node.line);
    return Value::nullValue();
}

Value Compiler::visitSuperExpr(SuperExpr& node) {
    // super 编译为 OP_GET_LOCAL 0（this），与方法中访问 this 相同
    // 调用点（visitMethodCall/visitMemberAccess）检测 super 对象并使用父类查找
    chunk_.writeOp(OpCode::OP_GET_LOCAL, node.line);
    chunk_.write(0, node.line);  // slot 0 = this
    return Value::nullValue();
}

void Compiler::error(const std::string& msg, int line, int col) {
    diagnostics_.addError(msg, line, col, DiagSource::Compiler);
}

// ---- 常量折叠 ----

void Compiler::emitConstant(const Value& val, int line) {
    if (val.isInt()) {
        uint16_t idx = chunk_.addConstant(val);
        chunk_.writeOp(OpCode::OP_INT, line);
        chunk_.writeShort(idx, line);
    } else if (val.isFloat()) {
        uint16_t idx = chunk_.addConstant(val);
        chunk_.writeOp(OpCode::OP_FLOAT, line);
        chunk_.writeShort(idx, line);
    } else if (val.isString()) {
        uint16_t idx = chunk_.addConstant(val);
        chunk_.writeOp(OpCode::OP_STRING, line);
        chunk_.writeShort(idx, line);
    } else if (val.isBool()) {
        chunk_.writeOp(val.boolVal() ? OpCode::OP_TRUE : OpCode::OP_FALSE, line);
    } else {
        chunk_.writeOp(OpCode::OP_NULL, line);
    }
}

bool Compiler::tryFoldBinary(BinOpType opType, ASTNode* left, ASTNode* right,
                              Value& result, int line) {
    // 提取左/右常量值（仅支持字面量节点）
    Value lv, rv;
    bool lConst = false, rConst = false;

    if (left && left->nodeType == NodeType::NODE_NUMBER_LITERAL) {
        lv = static_cast<NumberLiteral*>(left)->value;
        lConst = true;
    } else if (left && left->nodeType == NodeType::NODE_STRING_LITERAL) {
        lv = Value(static_cast<StringLiteral*>(left)->value);
        lConst = true;
    } else if (left && left->nodeType == NodeType::NODE_BOOL_LITERAL) {
        lv = Value(static_cast<BoolLiteral*>(left)->value);
        lConst = true;
    }

    if (right && right->nodeType == NodeType::NODE_NUMBER_LITERAL) {
        rv = static_cast<NumberLiteral*>(right)->value;
        rConst = true;
    } else if (right && right->nodeType == NodeType::NODE_STRING_LITERAL) {
        rv = Value(static_cast<StringLiteral*>(right)->value);
        rConst = true;
    } else if (right && right->nodeType == NodeType::NODE_BOOL_LITERAL) {
        rv = Value(static_cast<BoolLiteral*>(right)->value);
        rConst = true;
    }

    if (!lConst || !rConst) return false;

    // 数值运算
    if (lv.isNumber() && rv.isNumber()) {
        // 类型提升：int+float → float
        bool useFloat = lv.isFloat() || rv.isFloat();
        double ld = useFloat ? (lv.isFloat() ? lv.floatVal() : static_cast<double>(lv.intVal())) : 0;
        double rd = useFloat ? (rv.isFloat() ? rv.floatVal() : static_cast<double>(rv.intVal())) : 0;
        int64_t li = lv.isInt() ? lv.intVal() : 0;
        int64_t ri = rv.isInt() ? rv.intVal() : 0;

        switch (opType) {
        case BinOpType::BIN_ADD:
            if (!useFloat) {
                // B6 fix: 统一使用 OverflowCheck
                if (OverflowCheck::addOverflow(li, ri))
                    return false;
                result = Value(li + ri);
            } else {
                result = Value(ld + rd);
            }
            return true;
        case BinOpType::BIN_SUB:
            if (!useFloat) {
                if (OverflowCheck::subOverflow(li, ri))
                    return false;
                result = Value(li - ri);
            } else {
                result = Value(ld - rd);
            }
            return true;
        case BinOpType::BIN_MUL:
            if (!useFloat) {
                if (OverflowCheck::mulOverflow(li, ri))
                    return false;
                result = Value(li * ri);
            } else {
                result = Value(ld * rd);
            }
            return true;
        case BinOpType::BIN_DIV: {
            double divisor = useFloat ? rd : static_cast<double>(ri);
            if (divisor == 0) return false;  // 除零不折叠，保留运行时错误
            // B3 fix: INT64_MIN / -1 = 溢出 UB，不折叠
            if (!useFloat && OverflowCheck::divOverflow(li, ri)) return false;
            result = useFloat ? Value(ld / rd) : Value(li / ri);
            return true;
        }
        case BinOpType::BIN_MOD:
            if (!useFloat && ri == 0) return false;
            // B3 fix: INT64_MIN % -1 = 溢出 UB，不折叠
            if (!useFloat && OverflowCheck::modOverflow(li, ri)) return false;
            if (useFloat) return false;
            result = Value(li % ri);
            return true;
        case BinOpType::BIN_EQ:  result = Value(useFloat ? (ld == rd) : (li == ri)); return true;
        case BinOpType::BIN_NEQ: result = Value(useFloat ? (ld != rd) : (li != ri)); return true;
        case BinOpType::BIN_LT:  result = Value(useFloat ? (ld < rd)  : (li < ri));  return true;
        case BinOpType::BIN_GT:  result = Value(useFloat ? (ld > rd)  : (li > ri));  return true;
        case BinOpType::BIN_LTE: result = Value(useFloat ? (ld <= rd) : (li <= ri)); return true;
        case BinOpType::BIN_GTE: result = Value(useFloat ? (ld >= rd) : (li >= ri)); return true;
        default: break;
        }
    }

    // 字符串拼接
    if (lv.isString() && rv.isString() && opType == BinOpType::BIN_ADD) {
        result = Value(lv.stringVal() + rv.stringVal());
        return true;
    }

    // 字符串比较
    if (lv.isString() && rv.isString()) {
        if (opType == BinOpType::BIN_EQ)  { result = Value(lv.stringVal() == rv.stringVal()); return true; }
        if (opType == BinOpType::BIN_NEQ) { result = Value(lv.stringVal() != rv.stringVal()); return true; }
    }

    // 布尔逻辑（M1 fix: 短路语义，返回操作数原始值而非 bool）
    if (lv.isBool() && rv.isBool()) {
        if (opType == BinOpType::BIN_AND) { result = lv.isTruthy() ? rv : lv; return true; }
        if (opType == BinOpType::BIN_OR)  { result = lv.isTruthy() ? lv : rv; return true; }
        if (opType == BinOpType::BIN_EQ)  { result = Value(lv.boolVal() == rv.boolVal()); return true; }
        if (opType == BinOpType::BIN_NEQ) { result = Value(lv.boolVal() != rv.boolVal()); return true; }
    }

    return false;
}

bool Compiler::tryFoldUnary(UnaryOp::UnaryOpType opType, ASTNode* operand,
                             Value& result, int /*line*/) {
    if (!operand) return false;

    if (operand->nodeType == NodeType::NODE_NUMBER_LITERAL) {
        Value val = static_cast<NumberLiteral*>(operand)->value;
        if (opType == UnaryOp::UnaryOpType::UOP_NEGATE) {
            // B3 fix: -INT64_MIN = 溢出 UB，不折叠
            if (val.isInt() && OverflowCheck::negateOverflow(val.intVal())) return false;
            result = val.isFloat() ? Value(-val.floatVal()) : Value(-val.intVal());
            return true;
        }
    }
    if (operand->nodeType == NodeType::NODE_BOOL_LITERAL) {
        bool val = static_cast<BoolLiteral*>(operand)->value;
        if (opType == UnaryOp::UnaryOpType::UOP_NOT) {
            result = Value(!val);
            return true;
        }
    }

    return false;
}
