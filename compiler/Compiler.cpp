#include "compiler/Compiler.h"
#include <sstream>
#include <algorithm>
#include <cstdint>
#include <unordered_set>

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
    lastError_.clear();
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

    return result;
}

std::string Compiler::getLastError() const {
    return lastError_;
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

    // 根据 NodeType 枚举快速分发（替代 24 次 dynamic_cast）
    switch (node->nodeType) {
    case NodeType::NODE_BINARY_OP:     compileBinaryOp(*static_cast<BinaryOp*>(node)); return;
    case NodeType::NODE_UNARY_OP:      compileUnaryOp(*static_cast<UnaryOp*>(node)); return;
    case NodeType::NODE_NUMBER_LITERAL: compileNumberLiteral(*static_cast<NumberLiteral*>(node)); return;
    case NodeType::NODE_STRING_LITERAL: compileStringLiteral(*static_cast<StringLiteral*>(node)); return;
    case NodeType::NODE_BOOL_LITERAL:   compileBoolLiteral(*static_cast<BoolLiteral*>(node)); return;
    case NodeType::NODE_VAR_DECL:       compileVarDecl(*static_cast<VarDecl*>(node)); return;
    case NodeType::NODE_ASSIGNMENT:     compileAssignment(*static_cast<Assignment*>(node)); return;
    case NodeType::NODE_VAR_REF:       compileVarRef(*static_cast<VarRef*>(node)); return;
    case NodeType::NODE_IF_STMT:        compileIfStmt(*static_cast<IfStmt*>(node)); return;
    case NodeType::NODE_WHILE_STMT:     compileWhileStmt(*static_cast<WhileStmt*>(node)); return;
    case NodeType::NODE_FOR_STMT:       compileForStmt(*static_cast<ForStmt*>(node)); return;
    case NodeType::NODE_FUN_DECL:       compileFunDecl(*static_cast<FunDecl*>(node)); return;
    case NodeType::NODE_FUN_CALL:       compileFunCall(*static_cast<FunCall*>(node)); return;
    case NodeType::NODE_RETURN_STMT:    compileReturnStmt(*static_cast<ReturnStmt*>(node)); return;
    case NodeType::NODE_PRINT_STMT:     compilePrintStmt(*static_cast<PrintStmt*>(node)); return;
    case NodeType::NODE_BLOCK:          compileBlock(*static_cast<Block*>(node)); return;
    case NodeType::NODE_ARRAY_LITERAL:  compileArrayLiteral(*static_cast<ArrayLiteral*>(node)); return;
    case NodeType::NODE_DICT_LITERAL:   compileDictLiteral(*static_cast<DictLiteral*>(node)); return;
    case NodeType::NODE_INDEX_ACCESS:   compileIndexAccess(*static_cast<IndexAccess*>(node)); return;
    case NodeType::NODE_INDEX_ASSIGN:   compileIndexAssign(*static_cast<IndexAssign*>(node)); return;
    case NodeType::NODE_CLASS_DECL:     compileClassDecl(*static_cast<ClassDecl*>(node)); return;
    case NodeType::NODE_MEMBER_ACCESS: compileMemberAccess(*static_cast<MemberAccess*>(node)); return;
    case NodeType::NODE_MEMBER_ASSIGN: compileMemberAssign(*static_cast<MemberAssign*>(node)); return;
    case NodeType::NODE_METHOD_CALL:    compileMethodCall(*static_cast<MethodCall*>(node)); return;
    case NodeType::NODE_NULL_LITERAL:   compileNullLiteral(*static_cast<NullLiteral*>(node)); return;
    case NodeType::NODE_SUPER_EXPR:    compileSuperExpr(*static_cast<SuperExpr*>(node)); return;
    default:
        diagnostics_.addError("编译器内部错误: 未处理的 AST 节点类型", node->line, node->column, DiagSource::Compiler);
        return;
    }
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

void Compiler::compileBinaryOp(BinaryOp& node) {
    // 常量折叠：编译期求值常量表达式
    Value folded;
    if (tryFoldBinary(node.opType, node.left.get(), node.right.get(), folded, node.line)) {
        emitConstant(folded, node.line);
        return;
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
        return;
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
        return;
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
        return;
    }

    chunk_.writeOp(op, node.line);
}

void Compiler::compileUnaryOp(UnaryOp& node) {
    // 常量折叠：编译期求值常量表达式
    Value folded;
    if (tryFoldUnary(node.opType, node.operand.get(), folded, node.line)) {
        emitConstant(folded, node.line);
        return;
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
}

void Compiler::compileNumberLiteral(NumberLiteral& node) {
    uint16_t idx = chunk_.addConstant(node.value);
    if (node.value.isInt()) {
        chunk_.writeOp(OpCode::OP_INT, node.line);
    } else {
        chunk_.writeOp(OpCode::OP_FLOAT, node.line);
    }
    chunk_.writeShort(idx, node.line);
}

void Compiler::compileStringLiteral(StringLiteral& node) {
    uint16_t idx = chunk_.addConstant(Value(node.value));
    chunk_.writeOp(OpCode::OP_STRING, node.line);
    chunk_.writeShort(idx, node.line);
}

void Compiler::compileBoolLiteral(BoolLiteral& node) {
    chunk_.writeOp(node.value ? OpCode::OP_TRUE : OpCode::OP_FALSE, node.line);
}

void Compiler::compileVarDecl(VarDecl& node) {
    // 编译初始化表达式
    if (node.initializer) {
        compileNode(node.initializer.get());
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
}

void Compiler::compileAssignment(Assignment& node) {
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
                return;
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

void Compiler::compileVarRef(VarRef& node) {
    if (inFunction_) {
        auto it = currentLocals_.find(node.name);
        if (it != currentLocals_.end()) {
            chunk_.writeOp(OpCode::OP_GET_LOCAL, node.line);
            chunk_.write(static_cast<uint8_t>(it->second), node.line);
            return;
        }
        // VM-05/06: 尝试解析为 upvalue（闭包捕获外层变量）
        int uvIdx = resolveUpvalue(node.name, node.line);
        if (uvIdx >= 0) {
            chunk_.writeOp(OpCode::OP_GET_UPVALUE, node.line);
            chunk_.write(static_cast<uint8_t>(uvIdx), node.line);
            return;
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
}

void Compiler::compileIfStmt(IfStmt& node) {
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
}

void Compiler::compileWhileStmt(WhileStmt& node) {
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
}

void Compiler::compileForStmt(ForStmt& node) {
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
}

void Compiler::compileFunDecl(FunDecl& node) {
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
    // OP_CALL 通过函数名查找，不需要栈上的闭包值，弹出
    chunk_.writeOp(OpCode::OP_POP, node.line);

    // VM-05/06: 如果在函数内，将本函数注册到 outerLocals_ 和 outerFunctions_ 供更内层捕获
    if (inFunction_) {
        int slot = peakLocals_;  // 分配一个虚拟槽位（仅用于标识）
        outerLocals_[node.name] = slot;
        outerFunctions_[node.name] = slot;
    }
}

void Compiler::compileFunCall(FunCall& node) {
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
        return;
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
}

void Compiler::compileReturnStmt(ReturnStmt& node) {
    if (!inFunction_) {
        lastError_ = "return 只能在函数体内使用";
        diagnostics_.addError("return 只能在函数体内使用", node.line, 0, DiagSource::Compiler);
        return;
    }
    if (node.value) {
        compileNode(node.value.get());
    } else {
        chunk_.writeOp(OpCode::OP_NULL, node.line);
    }
    chunk_.writeOp(OpCode::OP_RETURN, node.line);
}

void Compiler::compilePrintStmt(PrintStmt& node) {
    for (auto& val : node.values) {
        compileNode(val.get());
        chunk_.writeOp(OpCode::OP_PRINT, node.line);
    }
}

void Compiler::compileBlock(Block& node) {
    auto savedLocals = currentLocals_;

    if (!inFunction_) {
        blockDepth_++;

        // 收集块作用域中将要声明的变量名，以便在编译前保存被遮蔽的全局变量
        std::vector<std::pair<std::string, std::string>> shadowedSaves; // (blockVarName, tempSaveName)
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
        // 删除未遮蔽任何全局变量的块作用域变量
        // P29: 用 unordered_set 替代线性扫描
        std::unordered_set<std::string> shadowedNames;
        for (auto& [vn, sn] : shadowedSaves) shadowedNames.insert(vn);
        for (auto& name : blockVars) {
            if (shadowedNames.find(name) == shadowedNames.end()) {
                uint16_t nameIdx = identifierIndex(name);
                chunk_.writeOp(OpCode::OP_DELETE_VAR, node.line);
                chunk_.writeShort(nameIdx, node.line);
            }
        }
    } else {
        // 函数内块作用域：局部变量使用栈槽，无需清理（VM 帧退出时自动释放）
        for (auto& stmt : node.statements) {
            compileStatement(stmt.get());
        }
        currentLocals_.swap(savedLocals);  // P28: swap
    }
}

// ---- 新增节点编译 ----

void Compiler::compileArrayLiteral(ArrayLiteral& node) {
    // 检查元素数量上限（uint8_t 编码限制）
    if (node.elements.size() > 255) {
        error("数组元素数量超过 255 个上限", node.line, node.column);
        return;
    }
    // 编译所有元素
    for (auto& elem : node.elements) {
        compileNode(elem.get());
    }
    // 构建数组指令
    chunk_.writeOp(OpCode::OP_BUILD_ARRAY, node.line);
    chunk_.write(static_cast<uint8_t>(node.elements.size()), node.line);
}

void Compiler::compileDictLiteral(DictLiteral& node) {
    // 检查键值对数量上限（uint8_t 编码限制）
    if (node.pairs.size() > 255) {
        error("字典键值对数量超过 255 个上限", node.line, node.column);
        return;
    }
    // 编译所有键值对（先键后值，与 OP_BUILD_DICT 消费顺序一致）
    for (auto& pair : node.pairs) {
        compileNode(pair.first.get());
        compileNode(pair.second.get());
    }
    // 构建字典指令：弹出 2*count 个值，构建字典，push 到栈
    chunk_.writeOp(OpCode::OP_BUILD_DICT, node.line);
    chunk_.write(static_cast<uint8_t>(node.pairs.size()), node.line);
}

void Compiler::compileIndexAccess(IndexAccess& node) {
    compileNode(node.object.get());
    compileNode(node.index.get());
    chunk_.writeOp(OpCode::OP_INDEX_GET, node.line);
}

void Compiler::compileIndexAssign(IndexAssign& node) {
    // 根据左值对象类型选择赋值策略
    VarRef* objVar = (node.object && node.object->nodeType == NodeType::NODE_VAR_REF)
                     ? static_cast<VarRef*>(node.object.get()) : nullptr;

    if (objVar) {
        // 先查局部变量（如方法内的 this）
        auto localIt = currentLocals_.find(objVar->name);
        if (localIt != currentLocals_.end()) {
            // 局部变量索引赋值：this.arr[i] = val → OP_INDEX_SET_LOCAL
            compileNode(node.index.get());
            compileNode(node.value.get());
            uint8_t slot = static_cast<uint8_t>(localIt->second);
            chunk_.writeOp(OpCode::OP_INDEX_SET_LOCAL, node.line);
            chunk_.write(slot, node.line);
        } else {
            // 全局变量索引赋值：arr[i] = val → OP_INDEX_SET_VAR
            compileNode(node.index.get());
            compileNode(node.value.get());
            uint16_t nameIdx = identifierIndex(objVar->name);
            chunk_.writeOp(OpCode::OP_INDEX_SET_VAR, node.line);
            chunk_.writeShort(nameIdx, node.line);
        }
    } else {
        // 通用路径（嵌套访问等）：推对象+索引+值，OP_INDEX_SET 不做写回（限制但安全）
        compileNode(node.object.get());
        compileNode(node.index.get());
        compileNode(node.value.get());
        chunk_.writeOp(OpCode::OP_INDEX_SET, node.line);
    }
}

void Compiler::compileClassDecl(ClassDecl& node) {
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
}

void Compiler::compileMemberAccess(MemberAccess& node) {
    compileNode(node.object.get());
    uint16_t nameIdx = identifierIndex(node.fieldName);
    // O1: super.field — 字段已在构造时通过继承链复制到实例中，与 this.field 等价
    // 方法调用走 OP_SUPER_CALL，此处仅处理字段读取
    bool isSuperAccess = (node.object && node.object->nodeType == NodeType::NODE_SUPER_EXPR);
    chunk_.writeOp(isSuperAccess ? OpCode::OP_SUPER_MEMBER_GET : OpCode::OP_MEMBER_GET, node.line);
    chunk_.writeShort(nameIdx, node.line);
}

void Compiler::compileMemberAssign(MemberAssign& node) {
    // 根据左值对象类型选择赋值策略
    VarRef* objVar = (node.object && node.object->nodeType == NodeType::NODE_VAR_REF)
                     ? static_cast<VarRef*>(node.object.get()) : nullptr;

    if (objVar) {
        // 先查局部变量（如方法内的 this）
        auto localIt = currentLocals_.find(objVar->name);
        if (localIt != currentLocals_.end()) {
            // 局部变量成员赋值：this.field = val → OP_MEMBER_SET_LOCAL
            compileNode(node.value.get());
            uint8_t slot = static_cast<uint8_t>(localIt->second);
            uint16_t fieldIdx = identifierIndex(node.fieldName);
            chunk_.writeOp(OpCode::OP_MEMBER_SET_LOCAL, node.line);
            chunk_.write(slot, node.line);
            chunk_.writeShort(fieldIdx, node.line);
            return;
        }
        // 全局变量成员赋值：obj.field = val → OP_MEMBER_SET_VAR
        compileNode(node.value.get());
        uint16_t varIdx = identifierIndex(objVar->name);
        uint16_t fieldIdx = identifierIndex(node.fieldName);
        chunk_.writeOp(OpCode::OP_MEMBER_SET_VAR, node.line);
        chunk_.writeShort(varIdx, node.line);
        chunk_.writeShort(fieldIdx, node.line);
    } else {
        // 通用路径：推对象+值，OP_MEMBER_SET 不做写回
        compileNode(node.object.get());
        compileNode(node.value.get());
        uint16_t nameIdx = identifierIndex(node.fieldName);
        chunk_.writeOp(OpCode::OP_MEMBER_SET, node.line);
        chunk_.writeShort(nameIdx, node.line);
    }
}

void Compiler::compileMethodCall(MethodCall& node) {
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

    // B6 fix: 如果接收者是 IndexAccess，预缓存索引值避免写回时重复求值
    // （对 arr[expr()].method() 这类调用，expr() 只执行一次）
    std::string cachedIndexVar;
    if (!objVar && node.object && node.object->nodeType == NodeType::NODE_INDEX_ACCESS) {
        auto* ia = static_cast<IndexAccess*>(node.object.get());
        if (ia->object && ia->object->nodeType == NodeType::NODE_VAR_REF && ia->index) {
            cachedIndexVar = "__wb_idx_" + std::to_string(writebackCounter_++);
            compileNode(ia->index.get());
            uint16_t cacheIdx = identifierIndex(cachedIndexVar);
            chunk_.writeOp(OpCode::OP_SET_VAR, node.line);
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
}

void Compiler::compileNullLiteral(NullLiteral& node) {
    chunk_.writeOp(OpCode::OP_NULL, node.line);
}

void Compiler::compileSuperExpr(SuperExpr& node) {
    // super 编译为 OP_GET_LOCAL 0（this），与方法中访问 this 相同
    // 调用点（compileMethodCall/compileMemberAccess）检测 super 对象并使用父类查找
    chunk_.writeOp(OpCode::OP_GET_LOCAL, node.line);
    chunk_.write(0, node.line);  // slot 0 = this
}

void Compiler::error(const std::string& msg, int line, int col) {
    std::ostringstream oss;
    oss << "编译错误 (行 " << line << ", 列 " << col << "): " << msg;
    lastError_ = oss.str();
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
            result = useFloat ? Value(ld + rd) : Value(li + ri);
            return true;
        case BinOpType::BIN_SUB:
            result = useFloat ? Value(ld - rd) : Value(li - ri);
            return true;
        case BinOpType::BIN_MUL:
            result = useFloat ? Value(ld * rd) : Value(li * ri);
            return true;
        case BinOpType::BIN_DIV: {
            double divisor = useFloat ? rd : static_cast<double>(ri);
            if (divisor == 0) return false;  // 除零不折叠，保留运行时错误
            // B3 fix: INT64_MIN / -1 = 溢出 UB，不折叠
            if (!useFloat && li == INT64_MIN && ri == -1) return false;
            result = useFloat ? Value(ld / rd) : Value(li / ri);
            return true;
        }
        case BinOpType::BIN_MOD:
            if (!useFloat && ri == 0) return false;
            // B3 fix: INT64_MIN % -1 = 溢出 UB，不折叠
            if (!useFloat && li == INT64_MIN && ri == -1) return false;
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

    // 布尔逻辑
    if (lv.isBool() && rv.isBool()) {
        if (opType == BinOpType::BIN_AND) { result = Value(lv.boolVal() && rv.boolVal()); return true; }
        if (opType == BinOpType::BIN_OR)  { result = Value(lv.boolVal() || rv.boolVal()); return true; }
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
            if (val.isInt() && val.intVal() == INT64_MIN) return false;
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
