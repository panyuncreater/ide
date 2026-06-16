#include "compiler/Compiler.h"
#include <sstream>
#include <algorithm>

// ============================================================
// Compiler 字节码编译器实现
// ============================================================

Compiler::Compiler() {}

CompileResult Compiler::compile(Block& program) {
    chunk_ = BytecodeChunk();
    chunk_.name = "main";
    chunk_.arity = 0;
    varIndex_.clear();
    lastError_.clear();
    diagnostics_.clear();
    functionChunks_.clear();
    currentLocals_.clear();
    inFunction_ = false;
    classFieldNames_.clear();
    outerLocals_.clear();

    // 编译所有顶层语句
    compileBlock(program);

    // 末尾添加 RETURN
    chunk_.writeOp(OpCode::OP_RETURN, 0);

    CompileResult result;
    result.mainChunk = std::move(chunk_);
    result.functionChunks = std::move(functionChunks_);

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
    }
}

void Compiler::compileStatement(ASTNode* node) {
    if (!node) return;

    compileNode(node);

    // 表达式语句（如 foo(); 或 obj.method();）会产生一个栈上返回值但不会被消费，
    // 若不弹出会导致栈无限累积（main chunk 无外层帧清理，循环内泄漏必然触发栈溢出）。
    // 赋值类节点（Assignment/IndexAssign/MemberAssign）以及声明/控制流节点已自行平衡栈，
    // 此处只对纯表达式补发 OP_POP，避免双重弹出。
    switch (node->nodeType) {
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
    if (tryFoldBinary(node.op, node.left.get(), node.right.get(), folded, node.line)) {
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
        uint16_t jumpTarget = static_cast<uint16_t>(chunk_.code.size());
        chunk_.code[jumpPatch + 1] = static_cast<uint8_t>(jumpTarget & 0xFF);
        chunk_.code[jumpPatch + 2] = static_cast<uint8_t>((jumpTarget >> 8) & 0xFF);
        // 统一为 bool 语义（对齐解释器 return Value(right.isTruthy())）
        chunk_.writeOp(OpCode::OP_NOT, node.line);
        chunk_.writeOp(OpCode::OP_NOT, node.line);
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
        uint16_t rightStart = static_cast<uint16_t>(chunk_.code.size());
        chunk_.code[jumpPatch + 1] = static_cast<uint8_t>(rightStart & 0xFF);
        chunk_.code[jumpPatch + 2] = static_cast<uint8_t>((rightStart >> 8) & 0xFF);
        // 弹出左操作数，求值右操作数
        chunk_.writeOp(OpCode::OP_POP, node.line);
        compileNode(node.right.get());
        // 修补第二个跳转
        uint16_t endTarget = static_cast<uint16_t>(chunk_.code.size());
        chunk_.code[jumpEnd + 1] = static_cast<uint8_t>(endTarget & 0xFF);
        chunk_.code[jumpEnd + 2] = static_cast<uint8_t>((endTarget >> 8) & 0xFF);
        // 统一为 bool 语义（对齐解释器 return Value(right.isTruthy())）
        chunk_.writeOp(OpCode::OP_NOT, node.line);
        chunk_.writeOp(OpCode::OP_NOT, node.line);
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
        error("不支持的运算符: " + node.op, node.line, node.column);
        return;
    }

    chunk_.writeOp(op, node.line);
}

void Compiler::compileUnaryOp(UnaryOp& node) {
    // 常量折叠：编译期求值常量表达式
    Value folded;
    if (tryFoldUnary(node.op, node.operand.get(), folded, node.line)) {
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
            chunk_.writeOp(OpCode::OP_SET_LOCAL, node.line);
            chunk_.write(static_cast<uint8_t>(slot), node.line);
        } else {
            chunk_.writeOp(OpCode::OP_SET_LOCAL, node.line);
            chunk_.write(static_cast<uint8_t>(it->second), node.line);
        }
        // OP_SET_LOCAL 用 peek(0) 不消费栈顶值，补发 OP_POP 平衡栈，
        // 避免函数内循环 var 声明累积导致栈溢出。
        chunk_.writeOp(OpCode::OP_POP, node.line);
    } else {
        uint16_t nameIdx = identifierIndex(node.name);
        chunk_.writeOp(OpCode::OP_DEFINE_VAR, node.line);
        chunk_.writeShort(nameIdx, node.line);
    }
}

void Compiler::compileAssignment(Assignment& node) {
    compileNode(node.value.get());

    if (inFunction_) {
        auto it = currentLocals_.find(node.name);
        if (it != currentLocals_.end()) {
            // OP_SET_LOCAL 用 peek(0) 不消费栈顶值，与 OP_SET_VAR 的 pop() 语义不一致。
            // 这里补发 OP_POP 显式清理被赋值的值，避免函数内循环赋值累积导致栈溢出。
            chunk_.writeOp(OpCode::OP_SET_LOCAL, node.line);
            chunk_.write(static_cast<uint8_t>(it->second), node.line);
            chunk_.writeOp(OpCode::OP_POP, node.line);
        } else {
            // 检测闭包捕获外层局部变量（VM 不支持 upvalue 机制）
            if (!outerLocals_.empty()) {
                auto outerIt = outerLocals_.find(node.name);
                if (outerIt != outerLocals_.end()) {
                    error("VM 不支持闭包捕获外层局部变量 '" + node.name +
                          "'，请使用全局变量替代", node.line, node.column);
                    return;
                }
            }
            uint16_t nameIdx = identifierIndex(node.name);
            chunk_.writeOp(OpCode::OP_SET_VAR, node.line);
            chunk_.writeShort(nameIdx, node.line);
        }
    } else {
        uint16_t nameIdx = identifierIndex(node.name);
        chunk_.writeOp(OpCode::OP_SET_VAR, node.line);
        chunk_.writeShort(nameIdx, node.line);
    }
}

void Compiler::compileVarRef(VarRef& node) {
    if (inFunction_) {
        auto it = currentLocals_.find(node.name);
        if (it != currentLocals_.end()) {
            chunk_.writeOp(OpCode::OP_GET_LOCAL, node.line);
            chunk_.write(static_cast<uint8_t>(it->second), node.line);
            return;
        }
        // 检测闭包捕获外层局部变量（VM 不支持 upvalue 机制）
        if (!outerLocals_.empty()) {
            auto outerIt = outerLocals_.find(node.name);
            if (outerIt != outerLocals_.end()) {
                error("VM 不支持闭包捕获外层局部变量 '" + node.name +
                      "'，请使用全局变量替代", node.line, node.column);
                return;
            }
        }
    }
    uint16_t nameIdx = identifierIndex(node.name);
    chunk_.writeOp(OpCode::OP_GET_VAR, node.line);
    chunk_.writeShort(nameIdx, node.line);
}

void Compiler::compileIfStmt(IfStmt& node) {
    compileNode(node.condition.get());

    // 条件为假跳转到 else 分支
    size_t elseJumpPatch = chunk_.code.size();
    chunk_.writeOp(OpCode::OP_JUMP_IF_FALSE, node.line);
    chunk_.writeShort(0, node.line);

    // 编译 then 分支
    chunk_.writeOp(OpCode::OP_POP, node.line);  // 弹出条件值
    compileStatement(node.thenBranch.get());

    // 跳过 else 分支
    size_t endJumpPatch = chunk_.code.size();
    chunk_.writeOp(OpCode::OP_JUMP, node.line);
    chunk_.writeShort(0, node.line);

    // 修补 else 跳转
    uint16_t elseStart = static_cast<uint16_t>(chunk_.code.size());
    chunk_.code[elseJumpPatch + 1] = static_cast<uint8_t>(elseStart & 0xFF);
    chunk_.code[elseJumpPatch + 2] = static_cast<uint8_t>((elseStart >> 8) & 0xFF);

    chunk_.writeOp(OpCode::OP_POP, node.line);  // 弹出条件值

    // 编译 else 分支
    if (node.elseBranch) {
        compileStatement(node.elseBranch.get());
    }

    // 修补 end 跳转
    uint16_t endTarget = static_cast<uint16_t>(chunk_.code.size());
    chunk_.code[endJumpPatch + 1] = static_cast<uint8_t>(endTarget & 0xFF);
    chunk_.code[endJumpPatch + 2] = static_cast<uint8_t>((endTarget >> 8) & 0xFF);
}

void Compiler::compileWhileStmt(WhileStmt& node) {
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
    uint16_t loopOffset = static_cast<uint16_t>(loopStart);
    chunk_.writeOp(OpCode::OP_LOOP, node.line);
    chunk_.writeShort(loopOffset, node.line);

    // 修补退出跳转
    uint16_t exitTarget = static_cast<uint16_t>(chunk_.code.size());
    chunk_.code[exitJumpPatch + 1] = static_cast<uint8_t>(exitTarget & 0xFF);
    chunk_.code[exitJumpPatch + 2] = static_cast<uint8_t>((exitTarget >> 8) & 0xFF);

    chunk_.writeOp(OpCode::OP_POP, node.line);  // 弹出条件值
}

void Compiler::compileForStmt(ForStmt& node) {
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

        // 编译更新（OP_SET_VAR 不再 push 值，无需额外 OP_POP）
        if (node.update) {
            compileStatement(node.update.get());
        }

        // 回跳
        chunk_.writeOp(OpCode::OP_LOOP, node.line);
        chunk_.writeShort(static_cast<uint16_t>(loopStart), node.line);

        // 修补退出跳转
        uint16_t exitTarget = static_cast<uint16_t>(chunk_.code.size());
        chunk_.code[exitJumpPatch + 1] = static_cast<uint8_t>(exitTarget & 0xFF);
        chunk_.code[exitJumpPatch + 2] = static_cast<uint8_t>((exitTarget >> 8) & 0xFF);
        chunk_.writeOp(OpCode::OP_POP, node.line);
    } else {
        // 无条件循环（while true）
        chunk_.writeOp(OpCode::OP_TRUE, node.line);
        size_t exitJumpPatch = chunk_.code.size();
        chunk_.writeOp(OpCode::OP_JUMP_IF_FALSE, node.line);
        chunk_.writeShort(0, node.line);
        chunk_.writeOp(OpCode::OP_POP, node.line);

        compileStatement(node.body.get());

        // 编译更新（OP_SET_VAR 不再 push 值，无需额外 OP_POP）
        if (node.update) {
            compileStatement(node.update.get());
        }

        chunk_.writeOp(OpCode::OP_LOOP, node.line);
        chunk_.writeShort(static_cast<uint16_t>(loopStart), node.line);

        uint16_t exitTarget = static_cast<uint16_t>(chunk_.code.size());
        chunk_.code[exitJumpPatch + 1] = static_cast<uint8_t>(exitTarget & 0xFF);
        chunk_.code[exitJumpPatch + 2] = static_cast<uint8_t>((exitTarget >> 8) & 0xFF);
        chunk_.writeOp(OpCode::OP_POP, node.line);
    }
}

void Compiler::compileFunDecl(FunDecl& node) {
    // 为函数体创建独立 BytecodeChunk
    BytecodeChunk savedChunk = std::move(chunk_);
    std::unordered_map<std::string, uint16_t> savedVarIndex = std::move(varIndex_);
    std::unordered_map<std::string, int> savedLocals = std::move(currentLocals_);
    bool savedInFunction = inFunction_;
    std::unordered_map<std::string, int> savedOuterLocals = std::move(outerLocals_);

    // 如果当前在函数内，将当前函数的局部变量保存为外层局部变量（供嵌套函数检测闭包捕获）
    if (inFunction_) {
        outerLocals_ = currentLocals_;
    } else {
        outerLocals_.clear();
    }

    // 设置函数编译上下文
    chunk_ = BytecodeChunk(node.name, static_cast<int>(node.params.size()));
    varIndex_.clear();
    currentLocals_.clear();
    inFunction_ = true;

    // 编译参数到局部变量槽位
    for (int i = 0; i < static_cast<int>(node.params.size()); ++i) {
        currentLocals_[node.params[i]] = i;
    }

    // 编译函数体
    if (node.body) {
        compileNode(node.body.get());
    }

    // 末尾添加隐式返回 null
    chunk_.writeOp(OpCode::OP_NULL, node.line);
    chunk_.writeOp(OpCode::OP_RETURN, node.line);

    // 记录局部变量总槽位数（含参数和函数体内 var 声明），供 VM 预分配栈空间
    chunk_.localCount = static_cast<int>(currentLocals_.size());

    // 存储函数 chunk
    functionChunks_[node.name] = std::move(chunk_);

    // 恢复主 chunk
    chunk_ = std::move(savedChunk);
    varIndex_ = std::move(savedVarIndex);
    currentLocals_ = std::move(savedLocals);
    inFunction_ = savedInFunction;
    outerLocals_ = std::move(savedOuterLocals);

    // 在主 chunk 中 emit OP_CLOSURE
    uint16_t nameIdx = identifierIndex(node.name);
    chunk_.writeOp(OpCode::OP_CLOSURE, node.line);
    chunk_.writeShort(nameIdx, node.line);
    chunk_.write(static_cast<uint8_t>(node.params.size()), node.line);
    // OP_CALL 通过函数名查找，不需要栈上的闭包值，弹出
    chunk_.writeOp(OpCode::OP_POP, node.line);
}

void Compiler::compileFunCall(FunCall& node) {
    // 编译参数
    for (auto& arg : node.arguments) {
        compileNode(arg.get());
    }

    // 函数名作为常量
    uint16_t nameIdx = identifierIndex(node.name);

    // 使用 OP_CALL 指令
    chunk_.writeOp(OpCode::OP_CALL, node.line);
    chunk_.write(static_cast<uint8_t>(nameIdx & 0xFF), node.line);
    chunk_.write(static_cast<uint8_t>((nameIdx >> 8) & 0xFF), node.line);
    chunk_.write(static_cast<uint8_t>(node.arguments.size()), node.line);
}

void Compiler::compileReturnStmt(ReturnStmt& node) {
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
    for (auto& stmt : node.statements) {
        compileStatement(stmt.get());
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

        chunk_ = BytecodeChunk(methodKey, static_cast<int>(funDecl->params.size()));
        varIndex_.clear();
        currentLocals_.clear();
        outerLocals_.clear();
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

        // 记录字段声明顺序（含继承字段），供 VM OP_METHOD_CALL 按序推入
        chunk_.fieldOrder = allFieldNames;

        if (funDecl->body) {
            compileNode(funDecl->body.get());
        }

        chunk_.writeOp(OpCode::OP_NULL, funDecl->line);
        chunk_.writeOp(OpCode::OP_RETURN, funDecl->line);

        // 记录局部变量总槽位数（含 this/字段/参数和方法体内 var 声明），供 VM 预分配栈空间
        chunk_.localCount = static_cast<int>(currentLocals_.size());

        functionChunks_[methodKey] = std::move(chunk_);

        chunk_ = std::move(savedChunk);
        varIndex_ = std::move(savedVarIndex);
        currentLocals_ = std::move(savedLocals);
        outerLocals_ = std::move(savedOuterLocals);
        inFunction_ = savedInFunction;
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
    chunk_.writeOp(OpCode::OP_MEMBER_GET, node.line);
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

    compileNode(node.object.get());
    for (auto& arg : node.arguments) {
        compileNode(arg.get());
    }
    uint16_t nameIdx = identifierIndex(node.methodName);
    chunk_.writeOp(OpCode::OP_METHOD_CALL, node.line);
    chunk_.writeShort(nameIdx, node.line);
    chunk_.write(static_cast<uint8_t>(node.arguments.size()), node.line);
    chunk_.writeShort(receiverVarIdx, node.line);  // 接收者全局变量名索引（0xFFFF = 无全局 writeBack）
    chunk_.write(receiverLocalSlot, node.line);    // 接收者局部变量 slot（0xFF = 无局部 writeBack）

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
        else if (node.object->nodeType == NodeType::NODE_INDEX_ACCESS) {
            auto* ia = static_cast<IndexAccess*>(node.object.get());
            if (ia->object && ia->object->nodeType == NodeType::NODE_VAR_REF) {
                auto* baseVar = static_cast<VarRef*>(ia->object.get());
                auto localIt = currentLocals_.find(baseVar->name);
                // 先推入索引值（OP_WRITEBACK_INDEX_LOCAL/VAR 需要索引在栈上）
                compileNode(ia->index.get());
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

bool Compiler::tryFoldBinary(const std::string& op, ASTNode* left, ASTNode* right,
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

        if (op == "+") {
            result = useFloat ? Value(ld + rd) : Value(li + ri);
            return true;
        }
        if (op == "-") {
            result = useFloat ? Value(ld - rd) : Value(li - ri);
            return true;
        }
        if (op == "*") {
            result = useFloat ? Value(ld * rd) : Value(li * ri);
            return true;
        }
        if (op == "/") {
            double divisor = useFloat ? rd : static_cast<double>(ri);
            if (divisor == 0) return false;  // 除零不折叠，保留运行时错误
            result = useFloat ? Value(ld / rd) : Value(li / ri);
            return true;
        }
        if (op == "%") {
            if (!useFloat && ri == 0) return false;  // 模零不折叠
            if (useFloat) return false;  // float 模运算不常见，不折叠
            result = Value(li % ri);
            return true;
        }

        // 比较运算
        if (op == "==") { result = Value(useFloat ? (ld == rd) : (li == ri)); return true; }
        if (op == "!=") { result = Value(useFloat ? (ld != rd) : (li != ri)); return true; }
        if (op == "<")  { result = Value(useFloat ? (ld < rd)  : (li < ri));  return true; }
        if (op == ">")  { result = Value(useFloat ? (ld > rd)  : (li > ri));  return true; }
        if (op == "<=") { result = Value(useFloat ? (ld <= rd) : (li <= ri)); return true; }
        if (op == ">=") { result = Value(useFloat ? (ld >= rd) : (li >= ri)); return true; }
    }

    // 字符串拼接
    if (lv.isString() && rv.isString() && op == "+") {
        result = Value(lv.stringVal() + rv.stringVal());
        return true;
    }

    // 字符串比较
    if (lv.isString() && rv.isString()) {
        if (op == "==") { result = Value(lv.stringVal() == rv.stringVal()); return true; }
        if (op == "!=") { result = Value(lv.stringVal() != rv.stringVal()); return true; }
    }

    // 布尔逻辑
    if (lv.isBool() && rv.isBool()) {
        if (op == "and") { result = Value(lv.boolVal() && rv.boolVal()); return true; }
        if (op == "or")  { result = Value(lv.boolVal() || rv.boolVal()); return true; }
        if (op == "==") { result = Value(lv.boolVal() == rv.boolVal()); return true; }
        if (op == "!=") { result = Value(lv.boolVal() != rv.boolVal()); return true; }
    }

    return false;
}

bool Compiler::tryFoldUnary(const std::string& op, ASTNode* operand,
                             Value& result, int /*line*/) {
    if (!operand) return false;

    if (operand->nodeType == NodeType::NODE_NUMBER_LITERAL) {
        Value val = static_cast<NumberLiteral*>(operand)->value;
        if (op == "-") {
            result = val.isFloat() ? Value(-val.floatVal()) : Value(-val.intVal());
            return true;
        }
    }
    if (operand->nodeType == NodeType::NODE_BOOL_LITERAL) {
        bool val = static_cast<BoolLiteral*>(operand)->value;
        if (op == "not") {
            result = Value(!val);
            return true;
        }
    }

    return false;
}
