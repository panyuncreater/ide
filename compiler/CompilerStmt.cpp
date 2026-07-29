#include "ast/ModuleIsolation.h" // BUG-AUDIT-MOD-2: VM 模块隔离（非导出顶层名前缀化）
#include "common/ConstFunEval.h" // L18 lang-constfun: 编译期沙箱求值
#include "common/Logger.h"
#include "common/RuntimeLimits.h"   // BUG-AUDIT-MOD-3: MAX_RECURSION_DEPTH
#include "common/TCO.h"             // R109 TCO: 尾递归自调用识别
#include "compiler/BytecodeCache.h" // P2-11: .minic 文件加载
#include "compiler/Compiler.h"
#include "compiler/IRSSA.h"                   // P2-10: gvnPass/licmPass/inlinePass
#include "compiler/RegisterBytecodeBackend.h" // PERF-14: 寄存器式后端
#include "interpreter/NumericUtils.h"         // 共享溢出检查（B6 fix）
#include "lexer/Lexer.h"                      // VM-IMPORT: 模块源码词法分析
#include "parser/Parser.h"                    // VM-IMPORT: 模块源码语法分析
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <sstream>

// === CompilerStmt: statement visitors (split from Compiler.cpp) ===

void Compiler::visitVarRef(VarRef& node) {
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
    return;
}

void Compiler::visitIfStmt(IfStmt& node) {
    // C17 fix: 死代码消除 — 若条件可在编译期求值为常量布尔值（无副作用），
    // 直接编译存活分支，跳过条件求值与跳转指令，消除死代码。
    Value condConst;
    if (extractConstant(node.condition.get(), condConst, node.line) && condConst.isBool()) {
        auto savedLocals = currentLocals_;
        size_t blockSlotBase = savedLocals.size();
        if (condConst.boolVal()) {
            // 条件恒真：仅编译 then 分支
            compileStatement(node.thenBranch.get());
        } else if (node.elseBranch) {
            // 条件恒假：仅编译 else 分支
            compileStatement(node.elseBranch.get());
        }
        closeSlotRanges(blockSlotBase); // L1 fix: 回填本块内声明的变量 range 的 endIp
        currentLocals_ = savedLocals;
        return;
    }

    compileNode(node.condition.get());

    // 条件为假跳转到 else 分支
    size_t elseJumpPatch = chunk_.code.size();
    chunk_.writeOp(OpCode::OP_JUMP_IF_FALSE, node.line);
    chunk_.writeShort(0, node.line);

    // 保存外层作用域，then 分支内声明的变量不泄漏
    auto savedLocals = currentLocals_;

    // BUG-AUDIT-CLOSE-1 fix: 记录 then/else 分支的 slot 基址，分支退出时关闭 upvalue。
    // 对齐 IR 路径 leaveBlockScope 的 CLOSE_UPVALUE 发射语义。
    // 仅函数内（inFunction_）需要——顶层 if 块用 OP_DEFINE_VAR/OP_DELETE_VAR 操作 globals_，
    // 闭包不会捕获全局变量为 upvalue。对齐 IR.cpp leaveBlockScope 的 inFunction_ 守卫。
    size_t branchSlotBase = currentLocals_.size();
    bool needCloseUpvalue = inFunction_;

    // 编译 then 分支
    chunk_.writeOp(OpCode::OP_POP, node.line); // 弹出条件值
    compileStatement(node.thenBranch.get());
    // BUG-AUDIT-CLOSE-1 fix: then 分支退出时关闭指向本分支 slot 的 open upvalues
    if (needCloseUpvalue && currentLocals_.size() > branchSlotBase && branchSlotBase <= 255) {
        chunk_.writeOp(OpCode::OP_CLOSE_UPVALUE, node.line);
        chunk_.write(static_cast<uint8_t>(branchSlotBase), node.line);
    }
    closeSlotRanges(branchSlotBase); // L1 fix: 回填 then 分支内变量 range 的 endIp

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

    chunk_.writeOp(OpCode::OP_POP, node.line); // 弹出条件值

    // 编译 else 分支（使用同样的 savedLocals，then 分支变量不可见）
    if (node.elseBranch) {
        compileStatement(node.elseBranch.get());
        // BUG-AUDIT-CLOSE-1 fix: else 分支退出时同样关闭 upvalue
        if (needCloseUpvalue && currentLocals_.size() > branchSlotBase && branchSlotBase <= 255) {
            chunk_.writeOp(OpCode::OP_CLOSE_UPVALUE, node.line);
            chunk_.write(static_cast<uint8_t>(branchSlotBase), node.line);
        }
        closeSlotRanges(branchSlotBase); // L1 fix: 回填 else 分支内变量 range 的 endIp
    }

    // else 分支变量也不泄漏
    currentLocals_ = savedLocals;

    // 修补 end 跳转
    uint16_t endTarget = safeCodeOffset();
    chunk_.code[endJumpPatch + 1] = static_cast<uint8_t>(endTarget & 0xFF);
    chunk_.code[endJumpPatch + 2] = static_cast<uint8_t>((endTarget >> 8) & 0xFF);
    return;
}

void Compiler::visitWhileStmt(WhileStmt& node) {
    // V3 fix: 保存局部变量映射，while 循环体内声明的变量不泄漏
    auto savedLocals = currentLocals_;

    size_t loopStart = chunk_.code.size();

    // 编译条件
    compileNode(node.condition.get());

    // 条件为假跳出循环
    size_t exitJumpPatch = chunk_.code.size();
    chunk_.writeOp(OpCode::OP_JUMP_IF_FALSE, node.line);
    chunk_.writeShort(0, node.line);

    chunk_.writeOp(OpCode::OP_POP, node.line); // 弹出条件值（true 路径）

    // break/continue 循环上下文：while 的 continue 跳回 loopStart（条件检查）
    // break 跳转目标在循环编译完成后回填（跳过出口 OP_POP，因 break 时条件值已弹出）
    loopStack_.push_back({loopStart, exitJumpPatch, {}, {}, false, 0, tryDepth_});

    // BUG-AUDIT-CLOSE-1 fix: 记录循环体 slot 基址，循环体每次迭代退出时关闭 upvalue。
    // 对齐 IR 路径 leaveBlockScope 的 CLOSE_UPVALUE 发射语义。
    // 闭包捕获循环局部变量时，每次迭代退出时关闭 upvalue 产生该次迭代的快照（by-value），
    // 否则所有闭包指向同一 slot，最终都返回最后一次迭代的值（by-reference）。
    size_t bodySlotBase = currentLocals_.size();
    bool needCloseUpvalue = inFunction_;
    // AUDIT-P2-CORRECT fix: 记录到 LoopContext 供 visitBreakStmt 发射 OP_CLOSE_UPVALUE
    loopStack_.back().bodySlotBase = bodySlotBase;
    loopStack_.back().needCloseUpvalue = needCloseUpvalue;

    // 编译循环体
    compileStatement(node.body.get());
    closeSlotRanges(bodySlotBase); // L1 fix: 回填循环体内变量 range 的 endIp

    // AUDIT-P1-CORRECT fix: continueTarget 必须在 OP_CLOSE_UPVALUE 之前，
    // 使 continue 跳到 OP_CLOSE_UPVALUE 执行后再 OP_LOOP。
    // 第三十八轮将 continueTarget 放在 OP_CLOSE_UPVALUE 之后是方向性错误——
    // continue 跳过 OP_CLOSE_UPVALUE 导致 upvalue 不关闭，闭包捕获变 by-reference。
    size_t continueTarget = chunk_.code.size();

    // BUG-AUDIT-CLOSE-1 fix: 循环体每次迭代退出时关闭 upvalue
    if (needCloseUpvalue && currentLocals_.size() > bodySlotBase && bodySlotBase <= 255) {
        chunk_.writeOp(OpCode::OP_CLOSE_UPVALUE, node.line);
        chunk_.write(static_cast<uint8_t>(bodySlotBase), node.line);
    }

    // 取出本层循环的 break/continue 跳转列表
    auto ctx = std::move(loopStack_.back());
    loopStack_.pop_back();

    // 回跳到条件检查
    uint16_t loopOffset = safeCodeOffset(loopStart);
    chunk_.writeOp(OpCode::OP_LOOP, node.line);
    chunk_.writeShort(loopOffset, node.line);

    // 修补退出跳转（正常退出：条件为假，条件值仍在栈上）
    uint16_t exitTarget = safeCodeOffset();
    chunk_.code[exitJumpPatch + 1] = static_cast<uint8_t>(exitTarget & 0xFF);
    chunk_.code[exitJumpPatch + 2] = static_cast<uint8_t>((exitTarget >> 8) & 0xFF);

    chunk_.writeOp(OpCode::OP_POP, node.line); // 弹出条件值（false 路径）

    // 回填 continue 跳转：跳到 continueTarget（OP_CLOSE_UPVALUE 之前，执行关闭后再 OP_LOOP）
    uint16_t contTarget = safeCodeOffset(continueTarget);
    for (size_t patch : ctx.continueJumps) {
        chunk_.code[patch + 1] = static_cast<uint8_t>(contTarget & 0xFF);
        chunk_.code[patch + 2] = static_cast<uint8_t>((contTarget >> 8) & 0xFF);
    }

    // 回填 break 跳转：跳到当前偏移（OP_POP 之后，break 时栈上无条件值）
    uint16_t breakTarget = safeCodeOffset();
    for (size_t patch : ctx.breakJumps) {
        chunk_.code[patch + 1] = static_cast<uint8_t>(breakTarget & 0xFF);
        chunk_.code[patch + 2] = static_cast<uint8_t>((breakTarget >> 8) & 0xFF);
    }

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
    return;
}

void Compiler::visitForStmt(ForStmt& node) {
    // ── for 循环编译形状 ───────────────────────────────────────────────
    //   <initializer>                (statement，自带栈平衡)
    // loopStart:
    //   <condition> | OP_TRUE        (无条件循环用永真)
    //   OP_JUMP_IF_FALSE <exit>      (假则跳出)
    //   OP_POP                        (弹条件值，true 路径)
    //   <body>
    //   <update>                      ← continue 跳转目标（先执行更新再判条件）
    //   OP_JUMP <loopStart>
    // exit:
    //   OP_POP                        (弹条件值，false 路径)
    // 通过 loopStack_ 登记 loopStart/exitJumpPatch，使循环体内的 break 回填到 exit、
    // continue 回填到 update 之后；tryDepth_ 一并记录，保证 try 内 break/continue
    // 先发射 OP_TRY_END 弹出异常处理器（见 visitTryStmt）。

    // V3 fix: 保存局部变量映射，for 循环内声明的变量不泄漏到外层作用域
    auto savedLocals = currentLocals_;

    // 编译初始化
    if (node.initializer) {
        compileStatement(node.initializer.get());
    }

    size_t loopStart = chunk_.code.size();

    // 编译条件（无条件循环使用 OP_TRUE 作为永真条件）
    if (node.condition) {
        compileNode(node.condition.get());
    } else {
        chunk_.writeOp(OpCode::OP_TRUE, node.line);
    }

    size_t exitJumpPatch = chunk_.code.size();
    chunk_.writeOp(OpCode::OP_JUMP_IF_FALSE, node.line);
    chunk_.writeShort(0, node.line);
    chunk_.writeOp(OpCode::OP_POP, node.line); // 弹出条件值（true 路径）

    // break/continue 循环上下文
    // continue 目标：有 update 时跳到 updateStart，否则跳到 loopStart
    // break 目标：循环编译完成后回填（跳过出口 OP_POP）
    bool hasUpdate = (node.update != nullptr);
    loopStack_.push_back({loopStart, exitJumpPatch, {}, {}, hasUpdate, 0, tryDepth_});

    // BUG-AUDIT-CLOSE-1 fix: 记录循环体 slot 基址，循环体每次迭代退出时关闭 upvalue。
    // 对齐 IR 路径 leaveBlockScope 的 CLOSE_UPVALUE 发射语义。
    size_t bodySlotBase = currentLocals_.size();
    bool needCloseUpvalue = inFunction_;
    // AUDIT-P2-CORRECT fix: 记录到 LoopContext 供 visitBreakStmt 发射 OP_CLOSE_UPVALUE
    loopStack_.back().bodySlotBase = bodySlotBase;
    loopStack_.back().needCloseUpvalue = needCloseUpvalue;

    // 编译循环体
    compileStatement(node.body.get());
    closeSlotRanges(bodySlotBase); // L1 fix: 回填循环体内变量 range 的 endIp

    // AUDIT-P1-CORRECT fix: updateStart（continue 目标）必须在 OP_CLOSE_UPVALUE 之前，
    // 使 continue 跳到 OP_CLOSE_UPVALUE 执行后再执行 update。
    // 第三十八轮将 updateStart 放在 OP_CLOSE_UPVALUE 之后是方向性错误——
    // continue 跳过 OP_CLOSE_UPVALUE 导致 upvalue 不关闭。
    size_t updateStart = chunk_.code.size();

    // BUG-AUDIT-CLOSE-1 fix: 循环体每次迭代退出时关闭 upvalue（在 update 之前）
    if (needCloseUpvalue && currentLocals_.size() > bodySlotBase && bodySlotBase <= 255) {
        chunk_.writeOp(OpCode::OP_CLOSE_UPVALUE, node.line);
        chunk_.write(static_cast<uint8_t>(bodySlotBase), node.line);
    }

    // 取出本层循环的 break/continue 跳转列表
    auto ctx = std::move(loopStack_.back());
    loopStack_.pop_back();

    // 编译更新表达式（compileStatement 会自动 POP 赋值留下的栈值）
    if (node.update) {
        compileStatement(node.update.get());
    }

    // 回跳到条件检查
    chunk_.writeOp(OpCode::OP_LOOP, node.line);
    chunk_.writeShort(safeCodeOffset(loopStart), node.line);

    // 修补退出跳转（正常退出：条件为假，条件值仍在栈上）
    uint16_t exitTarget = safeCodeOffset();
    chunk_.code[exitJumpPatch + 1] = static_cast<uint8_t>(exitTarget & 0xFF);
    chunk_.code[exitJumpPatch + 2] = static_cast<uint8_t>((exitTarget >> 8) & 0xFF);
    chunk_.writeOp(OpCode::OP_POP, node.line); // 弹出条件值（false 路径）

    // 回填 continue 跳转：有 update 跳到 updateStart，否则跳到 loopStart
    uint16_t contTarget = ctx.hasUpdate ? safeCodeOffset(updateStart) : safeCodeOffset(ctx.loopStart);
    for (size_t patch : ctx.continueJumps) {
        chunk_.code[patch + 1] = static_cast<uint8_t>(contTarget & 0xFF);
        chunk_.code[patch + 2] = static_cast<uint8_t>((contTarget >> 8) & 0xFF);
    }

    // 回填 break 跳转：跳到当前偏移（OP_POP 之后，break 时栈上无条件值）
    uint16_t breakTarget = safeCodeOffset();
    for (size_t patch : ctx.breakJumps) {
        chunk_.code[patch + 1] = static_cast<uint8_t>(breakTarget & 0xFF);
        chunk_.code[patch + 2] = static_cast<uint8_t>((breakTarget >> 8) & 0xFF);
    }

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

    // V3 fix: 恢复局部变量映射（P28: swap）
    currentLocals_.swap(savedLocals);
    return;
}

void Compiler::visitFunDecl(FunDecl& node) {
    // ── 函数编译总策略 ─────────────────────────────────────────────────
    // 每个 FunDecl 编译为一个独立的 BytecodeChunk（函数体），并被闭包化：
    //   emit OP_CLOSURE <arity> <upvalueCount> <upvalue表> 把函数 chunk 包成运行时闭包值。
    // 函数体帧布局遵循调用约定 [this][fields][args][locals]（见 VMCalls.cpp）：
    //   形参按声明顺序从局部槽 0 起分配；必需参数个数写入 chunk_.requiredArity，
    //   默认参数在调用侧（visitFunCall）按需补发，这里只登记其常量索引。
    // 嵌套函数（isInner）会先把外层 currentLocals_ 降级为 outerLocals_，供
    //   内层 collectFreeVars/resolveUpvalue 检测并透传闭包捕获（见上文文件头）。
    // 关键不变量：函数编译是完全可重入的——所有编译上下文（chunk_/currentLocals_/
    // currentUpvalues_/loopStack_/tryDepth_ 等）必须在进入/离开本函数时正确保存与
    // 恢复，否则嵌套定义会污染外层上下文。
    //
    // 拆分说明（原 224 行单函数 → orchestrator + 3 子阶段）：
    //   - declareFunction：设置 outerLocals_/outerUpvalues_、创建函数 chunk、参数槽位、
    //                      BUG-UV-1 前向自由变量分析（预建 upvalue）
    //   - compileFunctionBody：编译 body + 隐式 OP_NULL/OP_RETURN + 保存元数据
    //   - emitDefaultValues：F10 默认参数值常量化（与 emitMethodBody 共用）
    // 三子阶段均在 CompileContextGuard 作用域内执行，guard 析构自动恢复外层上下文。

    // H5 fix: 记录是否为内嵌函数（在函数体内定义的函数）
    bool isInner = inFunction_;

    // L18 lang-constfun: 顶层 const fun 注册到折叠表（后续 visitFunCall
    // 对实参全字面量的调用编译期沙箱求值）。仅顶层（非嵌套）声明参与。
    if (node.isConstFun && !isInner && !node.name.empty()) {
        constFunDecls_[node.name] = &node;
    }

    // R98 W3: 匿名 lambda（node.name 为空）使用合成名 `$lambda_N` 作为内部 key。
    // 合成名用于：functionChunks_ 存储、identifierIndex 常量池、innerFunctionSlots_ 注册。
    // 合成名不暴露给用户——错误消息中显示 `<lambda>`，不参与 OP_CALL 按名查找。
    // $ 不在标识符首字符集中（Lexer 不接受 $），合成名不会与用户变量名冲突。
    std::string effectiveName = node.name;
    bool isLambda = node.name.empty();
    if (isLambda) {
        effectiveName = "$lambda_" + std::to_string(lambdaCounter_++);
    }

    // C3 fix: 使用 CompileContextGuard RAII 自动保存/恢复 15 个编译上下文成员变量。
    // 原代码 22 处 std::move 保存 + 22 处手动恢复（含异常路径），极易遗漏。
    // 守卫在块作用域结束时自动恢复，包括 early return 和异常路径。
    int upvalueCount = 0;
    {
        CompileContextGuard guard(*this);
        // AUDIT-R7 F5 fix: 嵌套函数不是类方法——清除 compilingMethodBody_ 标志
        // （guard 不管理，手动恢复）。原 visitReturnStmt 用 currentClassName_ 非空
        // 判断 isMethod，类方法内嵌套命名函数编译时类名残留 → identifyTailCall
        // 拒绝 SelfFunction 尾调用 → 直接路径无 TCO 而 IR 路径有（实证：方法内
        // 嵌套函数深尾递归 StackVM 报深度超限而 StackVM-IR/RegisterVM 正常）。
        // 注：不清 currentClassName_——它还承担嵌套函数内 super 调用的类上下文
        // （visitSuperExpr L613 明确依赖其不被清除）。
        bool savedCompilingMethodF5 = compilingMethodBody_;
        compilingMethodBody_ = false;

        if (!declareFunction(node, guard.saved, effectiveName)) {
            compilingMethodBody_ = savedCompilingMethodF5; // AUDIT-R7 F5: 提前返回也恢复
            return;                                        // guard 自动恢复上下文（参数超限）
        }
        compileFunctionBody(node);
        emitDefaultValues(node);

        // R164 协程/生成器：标记生成器 chunk + 复制 yieldCount
        // VM 在 OP_CALL 时检测 isGenerator，若为 true 则创建协程值而非直接调用。
        // yieldCount 从 AST 复制（静态数或 kDynamicYieldCount=INT_MAX 表示循环内 yield）。
        chunk_.isGenerator = node.isGenerator;
        chunk_.yieldCount = node.yieldCount;

        // VM-05/06: 将 upvalue 描述符附加到函数 chunk
        chunk_.upvalues = std::move(currentUpvalues_);

        // 存储函数 chunk（用 effectiveName 作为 key，支持匿名 lambda）
        functionChunks_[effectiveName] = std::move(chunk_);

        // 记录 upvalue 数量（块外需要用于 OP_CLOSURE 发射）
        upvalueCount = static_cast<int>(functionChunks_[effectiveName].upvalues.size());

        // AUDIT-R7 F5: 恢复方法体标志（guard 不管理 compilingMethodBody_）
        compilingMethodBody_ = savedCompilingMethodF5;

        // guard 在块结束时自动恢复外层上下文
    }

    // 此时已恢复外层上下文，在主 chunk 中 emit OP_CLOSURE
    uint16_t nameIdx = identifierIndex(effectiveName);
    const BytecodeChunk& funChunk = functionChunks_[effectiveName];
    // AUDIT-BUG-C3 fix: upvalueCount 经 static_cast<uint8_t> 编码，
    // > 255 时静默截断低 8 位，解码端按截断值读取 upvalue 描述符导致闭包捕获错误变量集。
    // 与 RegisterBytecodeBackend.cpp:489 对齐，发射前显式检查上限，避免半成品字节码。
    if (upvalueCount > 255) {
        error("闭包 upvalue 数量超过 255 上限", node.line, 0);
        return;
    }
    for (int i = 0; i < upvalueCount; ++i) {
        if (funChunk.upvalues[i].index > 255) {
            error("闭包 upvalue 索引超过 255 上限", node.line, 0);
            return;
        }
    }
    chunk_.writeOp(OpCode::OP_CLOSURE, node.line);
    chunk_.writeShort(nameIdx, node.line);
    chunk_.write(static_cast<uint8_t>(upvalueCount), node.line);
    // 写入每个 upvalue 的描述符
    for (int i = 0; i < upvalueCount; ++i) {
        chunk_.write(funChunk.upvalues[i].isLocal ? 1 : 0, node.line);
        chunk_.write(static_cast<uint8_t>(funChunk.upvalues[i].index), node.line);
    }
    // R98 W3: 匿名 lambda 不存储到任何变量槽——闭包值留在栈上作为表达式结果。
    // 调用方（如 var f = fun(x){...}; 或 map(arr, fun(x){...});）从栈顶取值。
    if (isLambda) {
        // lambda 作为表达式求值，闭包值留在栈上，不 OP_POP 也不 OP_SET_LOCAL
        // 注意：若 lambda 出现在表达式语句上下文（如 `fun(x){...}(5);` 立即调用），
        // 调用结果会被外层 expressionStatement 的 POP 清理。
        return;
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
        // R98 W2 fix: 全局函数的闭包值存储到全局槽位（而非 POP 丢弃）。
        // 原 H5 fix 假设"顶层函数仅通过 OP_CALL 按名查找 functionChunks_/functionClosures_，
        // 永不作为值使用"，但 W2 高阶函数 map/filter/reduce/forEach/find 打破此假设——
        // map(arr, double) 中 double 作为参数值被 visitVarRef 编译为 OP_GET_GLOBAL <slot>，
        // 若全局槽位为 null（闭包值被 POP 丢弃），OP_GET_GLOBAL 取到 null，
        // invokeClosureSync 报"高阶函数的参数必须是函数"或上游报"未定义的变量: double"。
        // 修复：顶层函数也用 OP_DEFINE_GLOBAL 将闭包值存入全局槽位，使函数名可作为值引用。
        // 安全性：(1) OP_CALL 仍通过 functionClosures_ 按名查找，不读取 globalSlots_，不受影响；
        // (2) W1-BUG2 fix 的 isClassName/isInFunctionChunks 分流仍有效——直接调用 double(5)
        // 因 isInFunctionChunks=true 走 OP_CALL，不读 globalSlots_；
        // (3) 函数重定义 fun f(){} fun f(){} 第二次 OP_DEFINE_GLOBAL 覆盖第一次，语义正确。
        int globalSlot = lookupGlobalSlot(node.name);
        if (globalSlot >= 0) {
            chunk_.writeOp(OpCode::OP_DEFINE_GLOBAL, node.line);
            chunk_.writeShort(static_cast<uint16_t>(globalSlot), node.line);
        } else {
            // 无全局槽位（pre-scan 未注册）时退化为 OP_POP 保持向后兼容
            chunk_.writeOp(OpCode::OP_POP, node.line);
        }
    }

    // VM-05/06: 如果在函数内，将本函数注册到 outerLocals_ 和 outerFunctions_ 供更内层捕获
    if (inFunction_) {
        // H5 fix: 内嵌函数已有真实槽位，使用它；全局函数使用虚拟槽位
        int slot = isInner ? innerFunctionSlots_[node.name] : peakLocals_;
        outerLocals_[node.name] = slot;
        outerFunctions_[node.name] = slot;
    }
    return;
}

// ============================================================
// visitFunDecl 子阶段实现
// ============================================================

// R164 协程/生成器：yield 表达式编译
// 语义: 编译 yield 值表达式到栈顶（无值时 push null），然后发射 OP_YIELD。
// VM 在重放模式下比较运行时 yield 执行计数器与目标 yieldId：
//   - 命中目标: 抛出 VMYieldSignal 返回 yield 值
//   - 未命中: push yield 值回栈作为 yield 表达式结果，继续执行
void Compiler::visitYieldExpr(YieldExpr& node) {
    if (node.value) {
        compileNode(node.value.get());
    } else {
        chunk_.writeOp(OpCode::OP_NULL, node.line);
    }
    chunk_.writeOp(OpCode::OP_YIELD, node.line);
}

bool Compiler::declareFunction(FunDecl& node, const CompileContext& saved, const std::string& effectiveName) {
    // 如果当前在函数内，将当前函数的局部变量保存为外层局部变量（供嵌套函数检测闭包捕获）
    // C-P2-7 fix: 使用 saved.currentLocals（含外层函数局部变量）
    if (saved.inFunction) {
        outerLocals_ = saved.currentLocals;
        outerUpvalues_ = saved.currentUpvalues;
        outerUpvalueNames_ = saved.currentUpvalueNames;
    } else {
        outerLocals_.clear();
        outerUpvalues_.clear();
        outerUpvalueNames_.clear();
    }
    outerFunctions_.clear();
    if (saved.inFunction) {
        outerFunctions_ = saved.outerFunctions;
    }

    // 设置函数编译上下文
    // R161 fix: chunk_.name 必须用 effectiveName（匿名 lambda 为 `$lambda_N`），
    // 与 functionChunks_ 的 key 保持一致。JIT compileAllChunks 通过 chunk.name 反查
    // funcTable（key 同为 effectiveName），若 chunk.name 为空则找不到入口 Label 绑定
    // 导致 asmjit 跳转到未绑定 Label 崩溃。StackVM 不依赖 chunk.name（用 map key 查找），
    // 故此修改对 StackVM/RegisterVM/Interpreter 无影响。
    chunk_ = BytecodeChunk(effectiveName, static_cast<int>(node.params.size()));
    chunk_.reserveCode(256);
    // F10: 设置必需参数个数和默认值常量索引
    chunk_.requiredArity = node.requiredParamCount;
    varIndex_.clear();
    currentLocals_.clear();
    currentUpvalues_.clear();     // VM-05/06: 新的 upvalue 列表
    currentUpvalueNames_.clear(); // VM-05/06: 新的 upvalue 名称映射
    localSlotNames_.clear();      // BUG-IDE-12 fix: 清空槽位名映射
    slotNameRanges_.clear();      // L1 fix: 清空 IP 范围表
    stringConstIndex_.clear();    // R164 fixup2: 清空字符串常量去重缓存（新 chunk 有新常量池）
    inFunction_ = true;
    // C-P0-1/C-P0-3 fix: 函数体的循环栈和 try 深度从 0 开始
    loopStack_.clear();
    tryDepth_ = 0;
    // BUG-TYPE-1 fix (P1): 保存当前函数返回类型注解，供 visitReturnStmt 发射 OP_TYPE_CHECK。
    // 对齐 Interpreter 的 currentFunctionReturnType_ + CallFrameGuard 机制。
    currentFunctionReturnType_ = node.returnType;
    // R163 泛型扩展：记录当前函数的类型参数，供 emitTypeCheck 擦除类型参数注解
    currentTypeParams_ = node.typeParams;
    // TCO: 记录当前函数名与 FunDecl 指针，供 visitReturnStmt 识别 return f(args)。
    // 入口 IP 在 compileFunctionBody 中编译函数体首指令前记录。
    currentFunctionName_ = node.name;
    currentFunctionDecl_ = &node;

    // 编译参数到局部变量槽位
    // C-P1-2 fix: 参数数量上限 255（uint8_t 编码限制）
    if (node.params.size() > 255) {
        error("函数参数数量超过限制（最大 255 个）: " + node.name, node.line, 0);
        return false; // guard 自动恢复上下文
    }
    for (int i = 0; i < static_cast<int>(node.params.size()); ++i) {
        currentLocals_[node.params[i]] = i;
        // BUG-IDE-12 fix: 记录参数 slot→name
        if (static_cast<size_t>(i) >= localSlotNames_.size()) {
            localSlotNames_.resize(i + 1);
        }
        localSlotNames_[i] = node.params[i];
    }
    peakLocals_ = static_cast<int>(node.params.size());

    // BUG-UV-1 fix: 前向自由变量分析——在编译函数体前预建 upvalue。
    // 解决 3+ 层嵌套闭包问题：中间函数即使不直接引用外层变量，也需捕获供更内层函数透传。
    // 对齐 IR 路径 AstIRBuilder::visitFunDecl 的 computeFreeVars 调用。
    // 实现要点：computeFreeVars 递归遍历 AST 收集自由变量（含嵌套函数传播），
    // 然后对每个自由变量调用 resolveUpvalue 预建 upvalue 条目。
    // 注意：resolveUpvalue 依赖 outerLocals_/outerUpvalueNames_/outerFunctions_，
    // 这些已在上方 saved.inFunction 分支中正确设置。
    if (saved.inFunction) {
        auto freeVars = computeFreeVars(node);
        for (const auto& name : freeVars) {
            resolveUpvalue(name, node.line);
        }
    }

    return true;
}

void Compiler::compileFunctionBody(FunDecl& node) {
    // TCO: 记录函数体入口 IP（函数体首条字节码指令的偏移）。
    // 此时 chunk_.code 中可能仅有 OP_TRY_BEGIN 等外层指令（顶层函数无），
    // 但对函数体而言，compileNode(node.body) 写入的首条指令即为入口。
    // 必须在编译 body 之前记录，确保 visitReturnStmt 中 OP_JUMP 能回到此处。
    currentFunctionEntryIp_ = chunk_.code.size();

    // 编译函数体
    if (node.body) {
        compileNode(node.body.get());
    }

    // 末尾添加隐式返回 null
    chunk_.writeOp(OpCode::OP_NULL, node.line);
    chunk_.writeOp(OpCode::OP_RETURN, node.line);

    // 记录局部变量总槽位数
    chunk_.localCount = peakLocals_;
    // BUG-IDE-12 fix: 保存 slot→name 映射到 chunk，供 VM 条件断点求值
    chunk_.localSlotNames = localSlotNames_;
    // L1 fix: 保存基于 IP 范围的 slot→name 反查表，解决兄弟作用域槽位复用导致的
    // 变量名错位。调试器按 frame.ip 在 [startIp, endIp) 范围内反查变量名。
    chunk_.slotNameRanges = slotNameRanges_;
}

void Compiler::emitDefaultValues(FunDecl& node) {
    // F10: 编译默认参数值为常量
    // 仅支持字面量（Number/String/Bool/Null）和负数字面量，复杂表达式需通过 Interpreter 执行
    // 本函数由 visitFunDecl 与 emitMethodBody 共用，确保两条路径默认参数语义一致。
    for (size_t i = 0; i < node.defaultValues.size(); ++i) {
        if (node.defaultValues[i]) {
            const ASTNode* dv = node.defaultValues[i].get();
            Value constVal;
            bool isConst = false;

            if (dv->nodeType == NodeType::NODE_NUMBER_LITERAL) {
                constVal = static_cast<const NumberLiteral*>(dv)->getValue(); // A1 fix: getValue()
                isConst = true;
            } else if (dv->nodeType == NodeType::NODE_STRING_LITERAL) {
                constVal = static_cast<const StringLiteral*>(dv)->getValue(); // A1 fix: getValue()
                isConst = true;
            } else if (dv->nodeType == NodeType::NODE_BOOL_LITERAL) {
                constVal = static_cast<const BoolLiteral*>(dv)->getValue(); // A1 fix: getValue()
                isConst = true;
            } else if (dv->nodeType == NodeType::NODE_NULL_LITERAL) {
                constVal = Value::nullValue();
                isConst = true;
            } else if (dv->nodeType == NodeType::NODE_UNARY_OP) {
                // 支持负数字面量: -42, -3.14, --5 (双重否定)
                // C-P2-2 fix: 递归折叠嵌套一元运算，使 --5 等价于 5
                const ASTNode* cur = dv;
                int negateCount = 0;
                while (cur && cur->nodeType == NodeType::NODE_UNARY_OP) {
                    const auto* unary = static_cast<const UnaryOp*>(cur);
                    if (unary->opType != UnaryOp::UnaryOpType::UOP_NEGATE)
                        break;
                    ++negateCount;
                    cur = unary->operand.get();
                }
                if (cur && cur->nodeType == NodeType::NODE_NUMBER_LITERAL && negateCount > 0) {
                    // A1 fix: NumberLiteral 存储标量，用 isInt()/intVal() 直接访问
                    const NumberLiteral* numNode = static_cast<const NumberLiteral*>(cur);
                    if (numNode->isInt()) {
                        int64_t v = numNode->intVal();
                        // 奇数次取反为负，偶数次为正
                        constVal = Value((negateCount % 2 == 1) ? -v : v);
                    } else {
                        double v = numNode->floatVal(); // A1 fix: numNode 标量访问
                        constVal = Value((negateCount % 2 == 1) ? -v : v);
                    }
                    isConst = true;
                }
            }

            if (isConst) {
                uint16_t constIdx = chunk_.addConstant(constVal);
                chunk_.defaultConstIndices.push_back(constIdx);
            } else {
                // 复杂表达式默认值：VM 不支持，记录无效索引（NO_INDEX）
                // Interpreter 路径仍可正常执行
                chunk_.defaultConstIndices.push_back(RuntimeLimits::NO_INDEX);
            }
        }
    }
}

void Compiler::visitFunCall(FunCall& node) {
    // L18 lang-constfun: const fun 调用且实参全字面量 → 编译期沙箱求值，
    // 直接 emit 常量（求值失败/不纯/非原始类型结果 → 回退普通调用）。
    // 局部名遮蔽防护：当前作用域存在同名局部/upvalue 时不折叠。
    if (!constFunDecls_.empty() && node.callee == nullptr && currentLocals_.find(node.name) == currentLocals_.end()) {
        if (auto folded = ConstFunEval::tryEvaluate(constFunDecls_, node)) {
            emitConstant(*folded, node.line);
            return;
        }
    }
    // 链式调用 / 表达式调用: callee(args)
    if (node.callee) {
        // C-P1-1 fix: 参数数量检查移到编译参数之前，避免截断后栈损坏
        if (node.arguments.size() > 255) {
            error("函数调用参数数量超过限制（最大 255 个）", node.line, 0);
            return;
        }
        // 编译被调用表达式（结果应为闭包值，推入栈顶）
        compileNode(node.callee.get());
        // 编译参数
        for (auto& arg : node.arguments) {
            compileNode(arg.get());
        }
        // OP_CALL_EXPR: 栈顶 N 个参数下方为闭包值
        chunk_.writeOp(OpCode::OP_CALL_EXPR, node.line);
        chunk_.write(static_cast<uint8_t>(node.arguments.size()), node.line);
        return;
    }

    // H5 fix: 内嵌函数通过局部变量中的闭包值调用，避免 functionClosures_ 按名称覆盖
    auto innerIt = innerFunctionSlots_.find(node.name);
    if (innerIt != innerFunctionSlots_.end()) {
        // C-P1-1 fix: 参数数量检查移到编译参数之前
        if (node.arguments.size() > 255) {
            error("函数调用参数数量超过限制（最大 255 个）", node.line, 0);
            return;
        }
        // 先 push 闭包值（从局部变量获取）
        chunk_.writeOp(OpCode::OP_GET_LOCAL, node.line);
        chunk_.write(static_cast<uint8_t>(innerIt->second), node.line);
        // 再编译参数
        for (auto& arg : node.arguments) {
            compileNode(arg.get());
        }
        chunk_.writeOp(OpCode::OP_CALL_EXPR, node.line);
        chunk_.write(static_cast<uint8_t>(node.arguments.size()), node.line);
        return;
    }

    // W1 fix (R103): 对齐 IR.cpp CRITICAL-1 fix——若 node.name 是当前函数的
    // LOCAL 或 UPVALUE（持有闭包值，如 `var g = f; g();`），走 OP_CALL_EXPR 路径
    // 通过闭包值调用。原 StackVM 直接路径仅检查 innerFunctionSlots_（嵌套函数声明 slot），
    // 不覆盖 `var g = f;` 的普通局部变量闭包值场景，导致 StackVM 直接路径报
    // "未定义的函数: g"。IR 路径已通过 CRITICAL-1 fix 解决，直接路径未对齐。
    if (inFunction_) {
        auto localIt = currentLocals_.find(node.name);
        if (localIt != currentLocals_.end()) {
            if (node.arguments.size() > 255) {
                error("函数调用参数数量超过限制（最大 255 个）", node.line, 0);
                return;
            }
            chunk_.writeOp(OpCode::OP_GET_LOCAL, node.line);
            chunk_.write(static_cast<uint8_t>(localIt->second), node.line);
            for (auto& arg : node.arguments) {
                compileNode(arg.get());
            }
            chunk_.writeOp(OpCode::OP_CALL_EXPR, node.line);
            chunk_.write(static_cast<uint8_t>(node.arguments.size()), node.line);
            return;
        }
        int uvIdx = resolveUpvalue(node.name, node.line);
        if (uvIdx >= 0) {
            if (node.arguments.size() > 255) {
                error("函数调用参数数量超过限制（最大 255 个）", node.line, 0);
                return;
            }
            chunk_.writeOp(OpCode::OP_GET_UPVALUE, node.line);
            chunk_.write(static_cast<uint8_t>(uvIdx), node.line);
            for (auto& arg : node.arguments) {
                compileNode(arg.get());
            }
            chunk_.writeOp(OpCode::OP_CALL_EXPR, node.line);
            chunk_.write(static_cast<uint8_t>(node.arguments.size()), node.line);
            return;
        }
    } else {
        // W1 fix 扩展：顶层 var 持闭包值调用 (var g = makeAdder(10); g(32);)
        // 顶层 var 注册到 globalSlotAllocator_，此处通过 OP_GET_GLOBAL 加载闭包值
        // 后走 OP_CALL_EXPR。原 StackVM 直接路径走 OP_CALL 按名查找 functionChunks_，
        // 找不到 var 持有的闭包值，报"未定义的函数: g"。
        //
        // W1-BUG fix (R104): compile() pre-scan 步骤也将顶层 ClassDecl 注册到
        // globalSlotAllocator_（见第 142-143 行），但 `Foo()` 类实例化必须走 OP_CALL
        // （VM 运行时检测到名称是类时执行 OP_CLASS_NEW + 字段初始化）。
        // 原 W1 fix 扩展未区分 class 名与普通 var 闭包值名，导致 `Foo()` 被错误编译为
        // OP_GET_GLOBAL + OP_CALL_EXPR，VM 在 OP_CALL_EXPR 中找不到类构造入口而失败。
        // 修复：先用 classFieldNames_ 判定是否为类名，若是则跳过 W1 fix 扩展，
        // 落到下方 OP_CALL 路径（VM 运行时按名查找 functionChunks_ 或 classRegistry_）。
        //
        // W1-BUG2 fix (R104): 模块导入的 FunDecl（如 `import { counter } from "m";`）
        // 也被 preScanModuleGlobals 注册到 globalSlotAllocator_，但其闭包值在
        // visitFunDecl 顶层路径被 OP_POP 丢弃，全局槽位为 null。原 W1 fix 扩展未排除
        // 此场景，counter() 被错误编译为 OP_GET_GLOBAL + OP_CALL_EXPR，运行时
        // globalSlots_[slot] 为 null，OP_CALL_EXPR 报"表达式调用需要函数值"。
        // 修复：追加 functionChunks_ 查找——若名称在 functionChunks_ 中（已编译的
        // 函数声明，含模块导入的 FunDecl），走 OP_CALL 命名调用通过 functionChunks_
        // 查找。仅 var 持闭包值（不在 functionChunks_ 中）走 OP_CALL_EXPR。
        bool isClassName = (classFieldNames_.find(node.name) != classFieldNames_.end());
        bool isInFunctionChunks = (functionChunks_.find(node.name) != functionChunks_.end());
        int slot = (isClassName || isInFunctionChunks) ? -1 : lookupGlobalSlot(node.name);
        if (slot >= 0) {
            if (node.arguments.size() > 255) {
                error("函数调用参数数量超过限制（最大 255 个）", node.line, 0);
                return;
            }
            chunk_.writeOp(OpCode::OP_GET_GLOBAL, node.line);
            chunk_.writeShort(static_cast<uint16_t>(slot), node.line);
            for (auto& arg : node.arguments) {
                compileNode(arg.get());
            }
            chunk_.writeOp(OpCode::OP_CALL_EXPR, node.line);
            chunk_.write(static_cast<uint8_t>(node.arguments.size()), node.line);
            return;
        }
    }

    // C-P1-1 fix: 参数数量检查移到编译参数之前
    if (node.arguments.size() > 255) {
        error("函数调用参数数量超过限制（最大 255 个）", node.line, 0);
        return;
    }

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
    return;
}

void Compiler::visitReturnStmt(ReturnStmt& node) {
    if (!inFunction_) {
        error("return 只能在函数体内使用", node.line, 0);
        return;
    }
    // R109/L15 TCO: 尾调用优化。识别 return f(args) 或 return this.method(args) 形态的
    // 自递归调用，编译为"参数求值 + 逆序 OP_SET_LOCAL 覆盖参数槽 + OP_JUMP 函数入口"。
    // 跳过 OP_RETURN 的帧弹出，复用当前帧执行下一轮递归，深度无界。
    //
    // L15 扩展（与 IR 路径严格对齐）：
    //   - SelfFunction: 放宽 upvalue 限制（同函数重用闭包，upvalue 指向外层栈不变）
    //   - SelfFunction: 支持默认参数（args.size() < params.size() 时用默认值填充）
    //   - SelfMethod: 类方法自调用 return this.method(args)
    //     保留 slot 0 (this) 和 slot 1..N (字段)，仅覆盖参数槽 N+1..N+params
    //
    // 仍要求的条件：
    //   (1) AST 结构匹配（TCO::identifyTailCall 返回非 None）
    //   (2) 不在 try 块内（tryDepth_ == 0）
    //   (3) currentFunctionDecl_ 非空（用于获取形参/默认值）
    //   (4) args.size() <= params.size()（超出视为非尾调用）
    //
    // 参数求值顺序：先全部求值到栈顶，再逆序 OP_SET_LOCAL + OP_POP 覆盖参数槽。
    // 逆序保证：args[i] 可能引用 params[j]（j<i），若顺序赋值会破坏 params[j]。
    // 栈布局变化：[..., val_0, val_1, ..., val_N-1] → [...] → OP_JUMP（栈深度恢复）
    const bool isMethod = compilingMethodBody_; // AUDIT-R7 F5 fix: 独立标志（非 currentClassName_ 非空）
    TCO::TailCallInfo tco = TCO::identifyTailCall(&node, currentFunctionName_, isMethod);
    // B1-Shadow fix: SelfFunction 名称被局部变量/参数遮蔽（var f = other; return f(x);）
    // 时不是自递归——非 TCO 路径的局部优先解析会调用被赋值的闭包，而 TCO 会错误
    // 跳回自身入口形成死循环（实证：三 VM 路径均报"指令数超出上限"，Interpreter
    // 的运行时重绑定校验正确回退）。currentLocals_ 含当前作用域可见的局部变量与参数。
    if (tco.kind == TCO::TailCallInfo::Kind::SelfFunction &&
        currentLocals_.find(tco.call->name) != currentLocals_.end()) {
        tco.kind = TCO::TailCallInfo::Kind::None;
    }
    // L18 eng-tailcall: 互递归/一般尾调用 return g(args)。编译策略：先正常编译
    // 调用表达式（自动继承 visitFunCall 的遮蔽/upvalue/全局槽分流），若尾部
    // 恰为 OP_CALL（纯命名调用路径）则原地 patch 为 OP_TAIL_CALL，再补 OP_RETURN。
    // 运行时帧复用或降级为普通调用（语义与 CALL+RETURN 完全等价）。
    // 限制条件：
    //   - 非方法体（方法帧的 this/字段槽与 writeBack 语义不兼容帧复用）
    //   - 无返回类型注解（TCO 路径跳过 OP_TYPE_CHECK，目标函数类型未知）
    //   - 非生成器体（协程帧快照不可复用，运行时另有兑底）
    //   - 不在 try 块内（handler 清理语义）
    if (tco.kind == TCO::TailCallInfo::Kind::GeneralCall && tryDepth_ == 0 && inFunction_ && !compilingMethodBody_ &&
        currentFunctionReturnType_.empty() && (currentFunctionDecl_ == nullptr || !currentFunctionDecl_->isGenerator)) {
        compileNode(node.value.get());
        // 仅当尾部恰为 4 字节 OP_CALL 时 patch（CALL_EXPR 等路径保持普通调用）。
        // 三重校验防误判操作数字节：opcode 字节 + 常量池名称回查 + argCount 匹配。
        if (chunk_.code.size() >= 4 && chunk_.code[chunk_.code.size() - 4] == static_cast<uint8_t>(OpCode::OP_CALL)) {
            size_t callPos = chunk_.code.size() - 4;
            uint16_t nIdx = static_cast<uint16_t>(chunk_.code[callPos + 1] | (chunk_.code[callPos + 2] << 8));
            uint8_t ac = chunk_.code[callPos + 3];
            if (nIdx < chunk_.constants.size() && chunk_.constants[nIdx].isString() &&
                chunk_.constants[nIdx].stringVal() == tco.call->name && ac == tco.call->arguments.size()) {
                chunk_.code[callPos] = static_cast<uint8_t>(OpCode::OP_TAIL_CALL);
            }
        }
        chunk_.writeOp(OpCode::OP_RETURN, node.line);
        return;
    }
    if (tco.kind == TCO::TailCallInfo::Kind::GeneralCall) {
        tco.kind = TCO::TailCallInfo::Kind::None; // 条件不满足：回退普通 return 路径
    }
    if (tco.kind != TCO::TailCallInfo::Kind::None && tryDepth_ == 0 && currentFunctionDecl_ != nullptr) {
        const size_t paramCount = currentFunctionDecl_->params.size();
        // 计算参数槽位基址：SelfFunction=0，SelfMethod=1+fieldCount（跳过 this 和字段）
        size_t paramSlotBase = 0;
        const std::vector<std::shared_ptr<ASTNode>>* args = nullptr;
        if (tco.kind == TCO::TailCallInfo::Kind::SelfFunction) {
            args = &tco.call->arguments;
        } else { // SelfMethod
            paramSlotBase = 1 + chunk_.fieldOrder.size();
            args = &tco.methodCall->arguments;
        }
        if (args->size() <= paramCount && paramSlotBase + paramCount <= 255) {
            // 编译所有实参表达式，结果压栈
            for (auto& arg : *args) {
                compileNode(arg.get());
            }
            // L15: 缺失参数用默认值或 null 填充
            for (size_t i = args->size(); i < paramCount; ++i) {
                if (i < currentFunctionDecl_->defaultValues.size() && currentFunctionDecl_->defaultValues[i]) {
                    compileNode(currentFunctionDecl_->defaultValues[i].get());
                } else {
                    chunk_.writeOp(OpCode::OP_NULL, node.line);
                }
            }
            // 逆序 OP_SET_LOCAL + OP_POP 覆盖参数槽位
            // 栈布局：[..., val_0, val_1, ..., val_{N-1}]（val_{N-1} 在栈顶）
            // OP_SET_LOCAL 复制栈顶到 slot（不弹栈），OP_POP 弹栈。
            // 必须从栈顶（val_{N-1}）开始赋值到 slot N-1，逆序向下，
            // 否则正向迭代会把 val_{N-1-i} 赋给 slot i（参数顺序反转）。
            // AUDIT-R7 F3 fix: 覆盖前先关闭 slot >= paramSlotBase 的 open upvalues——
            // 帧复用覆盖仍被本轮闭包捕获的槽位时，闭包应快照当轮值（实证：原实现
            // 尾递归内逐轮存入数组的闭包全部读到末轮值 "111"，Interpreter 为 "321"）。
            // SelfMethod 的 this/字段槽（< paramSlotBase）不被覆盖不关闭。
            chunk_.writeOp(OpCode::OP_CLOSE_UPVALUE, node.line);
            chunk_.write(static_cast<uint8_t>(paramSlotBase), node.line);
            for (size_t i = paramCount; i > 0; --i) {
                chunk_.writeOp(OpCode::OP_SET_LOCAL, node.line);
                chunk_.write(static_cast<uint8_t>(paramSlotBase + i - 1), node.line);
                chunk_.writeOp(OpCode::OP_POP, node.line);
            }
            // 跳转到函数/方法入口（OP_JUMP 是绝对跳转）
            chunk_.writeOp(OpCode::OP_JUMP, node.line);
            chunk_.writeShort(static_cast<uint16_t>(currentFunctionEntryIp_), node.line);
            return;
        }
    }
    if (node.value) {
        compileNode(node.value.get());
    } else {
        chunk_.writeOp(OpCode::OP_NULL, node.line);
    }
    // BUG-TYPE-1 fix (P1): 函数有返回类型注解时，在 OP_RETURN 前发射 OP_TYPE_CHECK
    // 检查返回值类型兼容性。对齐 Interpreter::visitReturnStmt 的 checkType 逻辑。
    // 原实现仅 Interpreter 检查返回类型，StackVM/RegisterVM 静默通过，导致类型安全绕过。
    // R109 TCO: TCO 路径跳过 TYPE_CHECK 是安全的——递归调用的 return 会再次触发检查。
    if (!currentFunctionReturnType_.empty()) {
        emitTypeCheck(currentFunctionReturnType_, node.line);
    }
    // L4 fix: return 在 try-finally 块内时，续跳到 finally 入口执行 finally 块。
    // finally 块作为 Block 栈平衡（var 声明 OP_SET_LOCAL+OP_POP 不改操作数栈深度），
    // 返回值留在栈顶，finally 末尾 OP_FINALLY_END 续跳到紧邻的 OP_RETURN（return landing pad）。
    // finally 块内若再次 return/throw，新控制流覆盖原 return（与 Interpreter 对齐）。
    std::vector<size_t> returnFinallyPatches;
    if (emitFinallyJump(node.line, returnFinallyPatches)) {
        // return 在 try-finally 块内：回填 realTarget 到 OP_RETURN 位置
        size_t returnIp = chunk_.code.size();
        if (returnIp > 65535) {
            error("return 续跳目标溢出 65535", node.line, 0);
            return;
        }
        uint16_t target = static_cast<uint16_t>(returnIp);
        for (size_t patch : returnFinallyPatches) {
            // patch = OP_PUSH_JUMP_TARGET opcode 位置（emitFinallyJump 存 patch-1）
            // 回填操作数两字节到 patch+1, patch+2（与 breakJumps 回填一致）
            chunk_.code[patch + 1] = static_cast<uint8_t>(target & 0xFF);
            chunk_.code[patch + 2] = static_cast<uint8_t>((target >> 8) & 0xFF);
        }
    }
    chunk_.writeOp(OpCode::OP_RETURN, node.line);
    return;
}

void Compiler::visitBreakStmt(BreakStmt& node) {
    if (loopStack_.empty()) {
        error("break 只能在循环体内使用", node.line, 0);
        return;
    }
    // C-P0-2 fix: 只弹出循环内部的 try handler（与 continue 一致），
    // 之前使用 tryDepth_（全局深度）会错误弹出入层函数/外层循环的 try 帧
    int tryDepthInLoop = tryDepth_ - loopStack_.back().tryDepthAtStart;
    for (int i = 0; i < tryDepthInLoop; ++i) {
        chunk_.writeOp(OpCode::OP_TRY_END, node.line);
    }
    // AUDIT-P2-CORRECT fix: break 跳出循环时需关闭循环体内声明的闭包捕获变量的
    // upvalue，对齐正常迭代退出时的 OP_CLOSE_UPVALUE 发射。OP_CLOSE_UPVALUE
    // bodySlotBase 关闭 slot >= bodySlotBase 的全部 open upvalues（含嵌套块变量）。
    // 原 visitBreakStmt 直接发 OP_JUMP 跳到 breakTarget（在 OP_CLOSE_UPVALUE 之后），
    // 跳过 upvalue 关闭，导致闭包捕获变 by-reference（三后端不一致）。
    const LoopContext& loopCtx = loopStack_.back();
    if (loopCtx.needCloseUpvalue && loopCtx.bodySlotBase <= 255) {
        chunk_.writeOp(OpCode::OP_CLOSE_UPVALUE, node.line);
        chunk_.write(static_cast<uint8_t>(loopCtx.bodySlotBase), node.line);
    }
    // AUDIT-P1.1 fix: 若 break 在 try-finally 内，发射续跳字节码（先 push 真实目标，再 jump 到 finally 入口）。
    // finally 末尾的 OP_FINALLY_END 从 pendingJumpStack_ 取出真实目标续跳。
    // 若不在 try-finally 内，走常规路径（直接 OP_JUMP 到 breakTarget）。
    std::vector<size_t> realTargetPatches;
    if (emitFinallyJump(node.line, realTargetPatches)) {
        // 续跳路径：realTargetPatches 中的 patch 待回填到 breakTarget
        for (size_t patch : realTargetPatches) {
            loopStack_.back().breakJumps.push_back(patch);
        }
    } else {
        // 常规路径：发射 OP_JUMP，目标在循环编译完成后回填
        size_t patch = chunk_.code.size();
        chunk_.writeOp(OpCode::OP_JUMP, node.line);
        chunk_.writeShort(0, node.line);
        loopStack_.back().breakJumps.push_back(patch);
    }
    return;
}

void Compiler::visitContinueStmt(ContinueStmt& node) {
    if (loopStack_.empty()) {
        error("continue 只能在循环体内使用", node.line, 0);
        return;
    }
    // BUG1 fix: 只弹出循环内部的 try handler
    int tryDepthInLoop = tryDepth_ - loopStack_.back().tryDepthAtStart;
    for (int i = 0; i < tryDepthInLoop; ++i) {
        chunk_.writeOp(OpCode::OP_TRY_END, node.line);
    }
    // AUDIT-P1.1 fix: 若 continue 在 try-finally 内，发射续跳字节码（同 visitBreakStmt）。
    std::vector<size_t> realTargetPatches;
    if (emitFinallyJump(node.line, realTargetPatches)) {
        for (size_t patch : realTargetPatches) {
            loopStack_.back().continueJumps.push_back(patch);
        }
    } else {
        // 常规路径：发射 OP_JUMP，目标在循环编译完成后回填
        size_t patch = chunk_.code.size();
        chunk_.writeOp(OpCode::OP_JUMP, node.line);
        chunk_.writeShort(0, node.line);
        loopStack_.back().continueJumps.push_back(patch);
    }
    return;
}

void Compiler::visitThrowStmt(ThrowStmt& node) {
    // 编译抛出表达式，将值推入栈顶
    if (node.expression) {
        compileNode(node.expression.get());
    } else {
        chunk_.writeOp(OpCode::OP_NULL, node.line);
    }
    // OP_THROW 弹出栈顶值并触发异常传播
    chunk_.writeOp(OpCode::OP_THROW, node.line);
    return;
}

void Compiler::visitImportStmt(ImportStmt& node) {
    // VM-IMPORT: 编译期模块内联——加载模块源码、解析 AST、预扫描全局槽位、
    // 内联编译模块语句。模块代码在编译期被"展开"到主程序中，VM 运行时无需模块加载机制。
    //
    // BUG-AUDIT-MOD-2 fix: 模块隔离（AST 重写 + 作用域分析）
    // 在内联编译前，对模块 AST 调用 ModuleTopLevelRenamer::rename，将模块的
    // 非导出顶层声明名前缀化为 `__mod_<hash>__<name>`，并递归重写模块内对这些
    // 名字的引用。导入方无法用原名访问模块的非导出名，与 Interpreter 的模块
    // 隔离语义（独立 Environment）对齐。导出名保持原名，导入方正常访问。
    // - run-once: 同一模块多次 import 时仅编译/执行一次（linkedModuleSet_ 保证）
    // - 安全保障: export 标记检查（BUG-AUDIT-MOD-1）+ 深度限制（BUG-AUDIT-MOD-3）
    //   + AST 隔离（BUG-AUDIT-MOD-2，本处实施）

    // 1. 路径规范化与安全校验（SEC-1: 路径遍历防护）
    // BUG-M4 fix: loaderPath 保留大小写（供 moduleLoader_/预编译加载）；modulePath 为去重键
    // （Windows 小写折叠），供 linkedModuleSet_/moduleLoadingSet_/moduleExports_/rename 等所有
    // 去重与隔离用途，使同一文件的不同大小写拼写去重为同一模块（对齐 Interpreter）。
    std::string loaderPath = normalizeModulePath(node.modulePath);
    if (loaderPath.empty()) {
        error("模块路径无效: " + node.modulePath, node.line, 0);
        return;
    }
    std::string modulePath = moduleCacheKey(loaderPath);

    // P2-11: 命名空间导入字典构造 lambda（run-once 与首次加载路径共用）
    // 对每个 export 名 emit: OP_STRING(key常量) + OP_GET_GLOBAL(value)，
    // 最后 OP_BUILD_DICT + OP_DEFINE_GLOBAL(namespaceAlias)。
    // 修复说明：
    //   - 使用 OP_STRING（非已废弃的 OP_CONSTANT）
    //   - 使用 writeShort（BytecodeChunk 无 writeUint16 方法）
    //   - 使用 addConstant(Value(name))（BytecodeChunk 无 addStringConstant 方法）
    //   - run-once 路径也需调用：全局槽位已存在，OP_GET_GLOBAL 可直接读取
    auto emitNamespaceDict = [this, &node, &modulePath]() {
        if (node.namespaceAlias.empty())
            return;
        auto expIt = moduleExports_.find(modulePath);
        if (expIt != moduleExports_.end() && !expIt->second.empty()) {
            // 收集导出名并排序（确保三后端字典 key 顺序一致）
            std::vector<std::string> sortedExports(expIt->second.begin(), expIt->second.end());
            std::sort(sortedExports.begin(), sortedExports.end());
            if (sortedExports.size() > 255) {
                error("命名空间导入的导出数量超过 255 上限", node.line, 0);
                return;
            }
            // 逐个 push key(字符串常量) + value(全局槽位值)
            for (const auto& name : sortedExports) {
                // push key: 字符串常量（D5 fix: OP_STRING 替代废弃的 OP_CONSTANT）
                uint16_t keyIdx = chunk_.addConstant(Value(name));
                chunk_.writeOp(OpCode::OP_STRING, node.line);
                chunk_.writeShort(keyIdx, node.line);
                // push value: OP_GET_GLOBAL
                // AUDIT-R5 R5 fix: 优先读模块导出快照槽位（detach 后 lookupGlobalSlot(name) 已失效），
                // 未记录时回退到 lookupGlobalSlot（循环导入等未 detach 场景）。
                int slot = -1;
                if (auto msIt = moduleExportSlots_.find(modulePath); msIt != moduleExportSlots_.end()) {
                    if (auto sIt = msIt->second.find(name); sIt != msIt->second.end())
                        slot = sIt->second;
                }
                if (slot < 0)
                    slot = lookupGlobalSlot(name);
                if (slot < 0) {
                    error("命名空间导入失败：导出名 " + name + " 未分配全局槽位", node.line, 0);
                    return;
                }
                chunk_.writeOp(OpCode::OP_GET_GLOBAL, node.line);
                chunk_.writeShort(static_cast<uint16_t>(slot), node.line);
            }
            // OP_BUILD_DICT: 弹出 2*count 个值，push 字典
            chunk_.writeOp(OpCode::OP_BUILD_DICT, node.line);
            chunk_.write(static_cast<uint8_t>(sortedExports.size()), node.line);
            // OP_DEFINE_GLOBAL: 将字典存入 namespaceAlias 全局槽位
            int nsSlot = allocateGlobalSlot(node.namespaceAlias);
            chunk_.writeOp(OpCode::OP_DEFINE_GLOBAL, node.line);
            chunk_.writeShort(static_cast<uint16_t>(nsSlot), node.line);
        } else {
            // 空模块：构造空字典
            chunk_.writeOp(OpCode::OP_BUILD_DICT, node.line);
            chunk_.write(0, node.line);
            int nsSlot = allocateGlobalSlot(node.namespaceAlias);
            chunk_.writeOp(OpCode::OP_DEFINE_GLOBAL, node.line);
            chunk_.writeShort(static_cast<uint16_t>(nsSlot), node.line);
        }
    };

    // 2. run-once 检查：已编译的模块跳过（全局槽位已定义）
    if (linkedModuleSet_.count(modulePath)) {
        // BUG-AUDIT-MOD-1 fix: 已编译模块的具名导入验证也检查 export 集合（对齐 Interpreter）
        if (!node.importAll && !node.names.empty()) {
            auto expIt = moduleExports_.find(modulePath);
            if (expIt != moduleExports_.end()) {
                for (const auto& name : node.names) {
                    if (expIt->second.find(name) == expIt->second.end()) {
                        error("模块 " + modulePath + " 中未导出名称: " + name, node.line, 0);
                        return;
                    }
                }
            }
        }
        // P2-11 fix: run-once 路径也需构造命名空间字典（全局槽位已存在，OP_GET_GLOBAL 可直接读取）
        // AUDIT-R5 R5 fix: run-once 重复导入也从已记录的模块槽位重新拷贝快照（对齐 Interpreter 重导入）
        emitImportSnapshotCopy(modulePath, node);
        emitNamespaceDict();
        return;
    }

    // 3. P2-14 循环导入延迟加载：不报错，跳过本次内联编译（避免无限递归）
    // 模块的全局槽位已由首次加载路径的 preScanModuleGlobals 预扫描分配（值为 null），
    // moduleExports_ 已由 collectModuleExports 填充。首次加载路径会继续内联编译模块语句，
    // 填充全局槽位。循环回路闭合时，访问未初始化的导出名运行时读到 null（保持现有全局槽位语义）。
    // 语义参考 ES Modules + Python：模块全局槽位立即分配，值按执行顺序填充。
    if (moduleLoadingSet_.count(modulePath)) {
        // 具名导入验证：检查 export 集合（对齐 run-once 路径）
        if (!node.importAll && !node.names.empty()) {
            auto expIt = moduleExports_.find(modulePath);
            if (expIt != moduleExports_.end()) {
                for (const auto& name : node.names) {
                    if (expIt->second.find(name) == expIt->second.end()) {
                        error("模块 " + modulePath + " 中未导出名称: " + name, node.line, 0);
                        return;
                    }
                }
            }
        }
        // P2-11: 循环导入场景也需构造命名空间字典（全局槽位已预扫描分配）
        // AUDIT-R5 R5 fix: 循环导入未 detach，emitImportSnapshotCopy 内部因同槽位而跳过自拷贝（保持共享）
        emitImportSnapshotCopy(modulePath, node);
        emitNamespaceDict();
        return;
    }

    // BUG-AUDIT-MOD-3 fix: 模块加载深度保护（对齐 InterpreterModules.cpp:76-78）
    // Interpreter 有 moduleLoadingStack_.size() >= MAX_RECURSION_DEPTH 检查，
    // VM/IR 路径原缺失此检查，深嵌套导入链可能 C++ 栈溢出崩溃。
    if (moduleLoadingStack_.size() >= RuntimeLimits::MAX_RECURSION_DEPTH) {
        error("模块导入深度超过限制 (" + std::to_string(RuntimeLimits::MAX_RECURSION_DEPTH) + ")", node.line, 0);
        return;
    }

    // P2-11 预编译模块：优先尝试加载 .minic 文件（跳过源码解析与编译）
    // 成功时直接合并预编译字节码到当前编译，无需 moduleLoader_
    // 失败时回退到下方源码编译路径（需要 moduleLoader_）
    if (precompiledModuleResolver_) {
        // 标记为正在加载（循环检测，.minic 路径也需要）
        moduleLoadingSet_.insert(modulePath);
        moduleLoadingStack_.push_back(modulePath);

        bool loaded = loadPrecompiledModule(loaderPath, node);

        // 无论成功与否，都从加载集移除（loadPrecompiledModule 内部不操作加载集）
        moduleLoadingSet_.erase(modulePath);
        moduleLoadingStack_.pop_back();

        if (loaded) {
            // 标记为已链接（run-once 语义）
            linkedModuleSet_.insert(modulePath);

            // AUDIT-R5 R5 fix: 记录导出槽位并 detach，为快照拷贝做准备（预编译路径与源码路径一致）
            recordModuleExportSlots(modulePath);
            detachModuleExportSlots(modulePath);

            // BUG-AUDIT-MOD-1 fix: 具名导入验证（检查 export 集合）
            if (!node.importAll && !node.names.empty()) {
                auto expIt = moduleExports_.find(modulePath);
                if (expIt != moduleExports_.end()) {
                    for (const auto& name : node.names) {
                        if (expIt->second.find(name) == expIt->second.end()) {
                            error("模块 " + modulePath + " 中未导出名称: " + name, node.line, 0);
                            return;
                        }
                    }
                }
            }

            // AUDIT-R5 R5 fix: 快照拷贝 + 命名空间字典（.minic 路径也需要）
            emitImportSnapshotCopy(modulePath, node);
            emitNamespaceDict();
            return; // .minic 加载成功，跳过源码编译
        }
        // .minic 加载失败，回退到源码编译路径
    }

    // 4. 检查模块加载器
    if (!moduleLoader_) {
        error("VM 编译需要模块加载器（moduleLoader 未设置），请通过文件路径运行", node.line, 0);
        return;
    }

    // 5. 标记为正在加载（循环检测 + 深度保护）
    moduleLoadingSet_.insert(modulePath);
    moduleLoadingStack_.push_back(modulePath);

    // 6. 加载并解析模块
    auto moduleAst = loadAndParseModule(loaderPath, node.line);
    if (!moduleAst) {
        moduleLoadingSet_.erase(modulePath);
        moduleLoadingStack_.pop_back(); // BUG-AUDIT-MOD-3
        return;                         // loadAndParseModule 已调用 error()
    }

    // 6.5 BUG-AUDIT-MOD-2 fix: 模块隔离——重命名非导出顶层名为 `__mod_<hash>__<name>`
    // 在预扫描前重写 AST，确保重命名后的名字进入全局槽位分配与 export 集合
    ModuleTopLevelRenamer::rename(*moduleAst, modulePath);

    // 7. 预扫描模块顶层声明，分配全局槽位
    preScanModuleGlobals(*moduleAst);

    // 7.5 BUG-AUDIT-MOD-1 fix: 收集模块导出名称（对齐 InterpreterModules.cpp:172-188）
    // 仅 ExportStmt 包装的声明名计入导出集合，普通顶层声明不算导出
    collectModuleExports(modulePath, *moduleAst);
    // AUDIT-R5 R5 fix: 记录导出名的模块槽位（preScan 已分配），供快照拷贝与命名空间字典读取；
    // 也使本模块内联期间的循环导入能命中已记录槽位。
    recordModuleExportSlots(modulePath);

    // 8. 内联编译模块语句（递归处理模块自身的 import）
    for (auto& stmt : moduleAst->statements) {
        if (stmt)
            compileStatement(stmt.get());
    }

    // 9. 保留模块 AST（函数/类定义指针在字节码中以常量池索引引用，AST 必须存活）
    moduleAsts_.push_back(std::move(moduleAst));

    // 10. 从加载集移除，标记为已链接
    moduleLoadingSet_.erase(modulePath);
    moduleLoadingStack_.pop_back(); // BUG-AUDIT-MOD-3
    linkedModuleSet_.insert(modulePath);

    // P2-11: import * as ns from "path" — 构造命名空间字典对象
    // 模块代码已内联编译，导出名对应的全局槽位已分配。
    // AUDIT-R5 R5 fix: 先 detach 导出名（使导入方可另分配新槽位承载快照副本），
    // 验证具名导入后 emit 快照拷贝，最后构造命名空间字典（均从记录的模块槽位读取）。
    // AUDIT-R6 B4 fix: detach 前重新 record（record 为先清后填，幂等）。若模块内联
    // 期间的嵌套导入把同名导出重新绑到新槽（嵌套模块同名导出的快照拷贝），
    // 内联前记录的槽号已过期：detachName 按名释放的是新槽，而拷贝仍从旧槽读——
    // 导入方拿到陈旧/他模块的值。重新 record 使槽号与当前名绑定一致。
    recordModuleExportSlots(modulePath);
    detachModuleExportSlots(modulePath);

    // 11. BUG-AUDIT-MOD-1 fix: 具名导入验证改为检查 export 集合（对齐 Interpreter）
    // 原实现仅检查 lookupGlobalSlot(name) < 0（名称存在即通过），
    // 导致非导出名称可被导入，违反模块封装语义。
    if (!node.importAll && !node.names.empty()) {
        auto expIt = moduleExports_.find(modulePath);
        if (expIt != moduleExports_.end()) {
            for (const auto& name : node.names) {
                if (expIt->second.find(name) == expIt->second.end()) {
                    // BUG-AUDIT-MOD-6 fix: 错误消息对齐 Interpreter（"模块 X 中未导出名称: Y"）
                    error("模块 " + modulePath + " 中未导出名称: " + name, node.line, 0);
                    return;
                }
            }
        }
    }

    // AUDIT-R5 R5 fix: 具名/全量导入的快照拷贝（模块槽位 → 导入方新槽位）
    emitImportSnapshotCopy(modulePath, node);
    emitNamespaceDict();
}

void Compiler::visitExportStmt(ExportStmt& node) {
    // VM-IMPORT: export 在 VM 中等价于普通声明编译（导出语义在编译期内联时自然满足——
    // 模块的所有顶层声明对导入方可见）。记录导出名供 import 验证使用。
    if (node.declaration) {
        compileNode(node.declaration.get());
    }
    return;
}

// ============================================================
// VM-IMPORT: 模块系统辅助方法
// ============================================================

std::string Compiler::normalizeModulePath(const std::string& rawPath) const {
    // AUDIT-R5 R6 fix: 规范化改用 normalizeModulePathKey（common/ModulePath.h
    // 单一事实源，与 Interpreter/AstIRBuilder/pathHash 五处同步），补齐连续
    // 斜杠折叠与 Windows 大小写折叠。安全校验（绝对路径/".." 段）仍在下方。
    std::string path = normalizeModulePathKey(rawPath);
    // 空路径
    if (path.empty())
        return "";
    // 绝对路径检测（Unix '/' 或 Windows 驱动器路径 'X:...'）
    // BUG-MOD-1 fix: 原实现仅检测 'C:/' 形式，未拒绝 'C:foo'（Windows 驱动器相对路径），
    // 可能被 moduleLoader_ 解析到模块目录外的文件。修复：拒绝所有 'X:' 开头形式
    // （X 为任意字符），覆盖 'C:/'、'C:foo'、'D:path' 等。
    // 注：反斜杠已在上方统一转为正斜杠，无需再检测 '\\'。
    if (path[0] == '/' || (path.size() >= 2 && path[1] == ':')) {
        return "";
    }
    // ".." 路径段检测
    size_t pos = 0;
    while (pos < path.size()) {
        size_t next = path.find('/', pos);
        std::string segment = (next == std::string::npos) ? path.substr(pos) : path.substr(pos, next - pos);
        if (segment == "..")
            return "";
        if (next == std::string::npos)
            break;
        pos = next + 1;
    }
    return path;
}

std::unique_ptr<Block> Compiler::loadAndParseModule(const std::string& modulePath, int line) {
    // 调用模块加载器获取源码
    std::string source = moduleLoader_(modulePath);
    if (source.empty()) {
        error("无法加载模块: " + modulePath + "（文件不存在或为空）", line, 0);
        return nullptr;
    }
    // 词法分析
    Lexer lexer;
    auto tokens = lexer.scan(source);
    // BUG-FIX: 检查词法错误并转发到编译诊断（原实现吞掉 Lexer 错误）
    if (lexer.getDiagnostics().hasErrors()) {
        const auto& diags = lexer.getDiagnostics().all();
        if (!diags.empty()) {
            const auto& d = diags.front();
            error("模块 '" + modulePath + "' 词法错误: " + d.message, d.line, d.column);
        } else {
            error("模块 '" + modulePath + "' 词法错误", line, 0);
        }
        return nullptr;
    }
    // 语法分析
    Parser parser;
    auto moduleAst = parser.parse(tokens);
    // BUG-FIX: 检查语法错误并转发到编译诊断（原实现仅检查 AST 是否为空，
    // 可恢复错误的 AST 会被当作成功加载）
    if (parser.hasErrors()) {
        const auto& diags = parser.getDiagnostics().all();
        if (!diags.empty()) {
            const auto& d = diags.front();
            error("模块 '" + modulePath + "' 语法错误: " + d.message, d.line, d.column);
        } else {
            error("模块 '" + modulePath + "' 语法错误", line, 0);
        }
        return nullptr;
    }
    if (!moduleAst) {
        error("模块解析失败: " + modulePath, line, 0);
        return nullptr;
    }
    return moduleAst;
}

void Compiler::preScanModuleGlobals(Block& moduleAst) {
    // 预扫描模块顶层声明，分配全局槽位。
    // BUG-PRES-1 fix: 覆盖 ExportStmt（解包 VarDecl/ClassDecl/FunDecl 内部声明）。
    //
    // R98 W2 fix: 原"与 compile() 的 pre-scan 存在有意的不对称"已消除——
    // compile() 现在也预扫描 FunDecl（visitFunDecl 顶层路径已用 OP_DEFINE_GLOBAL
    // 写入 globalSlots_[slot]，使函数名可作为值引用，详见 compile() 的 pre-scan 注释）。
    // 两个 pre-scan 现在行为一致，均预扫描 VarDecl/ClassDecl/FunDecl。
    for (auto& stmt : moduleAst.statements) {
        if (!stmt)
            continue;
        switch (stmt->nodeType) {
        case NodeType::NODE_VAR_DECL:
            allocateGlobalSlot(static_cast<VarDecl*>(stmt.get())->name);
            break;
        case NodeType::NODE_CLASS_DECL:
            allocateGlobalSlot(static_cast<ClassDecl*>(stmt.get())->name);
            break;
        case NodeType::NODE_FUN_DECL:
            allocateGlobalSlot(static_cast<FunDecl*>(stmt.get())->name);
            break;
        case NodeType::NODE_EXPORT_STMT: {
            // ExportStmt 包装内部声明——提取声明名并分配槽位
            auto* exportNode = static_cast<ExportStmt*>(stmt.get());
            if (exportNode->declaration) {
                ASTNode* decl = exportNode->declaration.get();
                switch (decl->nodeType) {
                case NodeType::NODE_VAR_DECL:
                    allocateGlobalSlot(static_cast<VarDecl*>(decl)->name);
                    break;
                case NodeType::NODE_CLASS_DECL:
                    allocateGlobalSlot(static_cast<ClassDecl*>(decl)->name);
                    break;
                case NodeType::NODE_FUN_DECL:
                    allocateGlobalSlot(static_cast<FunDecl*>(decl)->name);
                    break;
                default:
                    break;
                }
            }
            break;
        }
        default:
            break;
        }
    }
}

void Compiler::collectModuleExports(const std::string& modulePath, Block& moduleAst) {
    // visitImportStmt 子阶段：收集模块导出名称集合
    // 仅 ExportStmt 包装的声明名（VarDecl/ClassDecl/FunDecl）计入导出集合，
    // 普通顶层声明不算导出。对齐 InterpreterModules.cpp:172-188 的语义。
    // 输出：moduleExports_[modulePath] = exports 集合
    std::unordered_set<std::string> exports;
    std::unordered_set<std::string> exportVars; // AUDIT-R5 R5 fix: 仅 VarDecl 导出（快照适用）
    for (auto& stmt : moduleAst.statements) {
        if (!stmt || stmt->nodeType != NodeType::NODE_EXPORT_STMT)
            continue;
        auto* exp = static_cast<ExportStmt*>(stmt.get());
        if (!exp->declaration)
            continue;
        ASTNode* decl = exp->declaration.get();
        switch (decl->nodeType) {
        case NodeType::NODE_VAR_DECL:
            exports.insert(static_cast<VarDecl*>(decl)->name);
            exportVars.insert(static_cast<VarDecl*>(decl)->name);
            break;
        case NodeType::NODE_CLASS_DECL:
            exports.insert(static_cast<ClassDecl*>(decl)->name);
            break;
        case NodeType::NODE_FUN_DECL:
            exports.insert(static_cast<FunDecl*>(decl)->name);
            break;
        default:
            break;
        }
    }
    moduleExports_[modulePath] = std::move(exports);
    moduleExportVars_[modulePath] = std::move(exportVars);
}

// ============================================================
// AUDIT-R5 R5 fix：可变导出量导入快照语义（与 Interpreter 对齐）
// ------------------------------------------------------------
// 背景：VM/IR 将模块内联至单一全局作用域，导出名与导入方共享同一全局槽，
// 导致模块内函数对导出变量的变异经导入名可见（共享/实时语义）；而 Interpreter
// 将导出值按值拷入导入方环境（快照语义）。AUDIT-R5 R5 实测确认三后端不一致，
// 统一到 Interpreter 基准（快照）：模块内联后记录导出名的模块槽位并 detach 名称，
// 导入方另分配新槽位并在导入点 emit OP_GET_GLOBAL(模块槽)+OP_DEFINE_GLOBAL(新槽) 拷贝。
// 函数调用经 functionChunks_ 按名解析（不受 detach 影响），故函数/类导出拷贝无害；
// 实际可观差异仅对可变变量生效。
// ============================================================

void Compiler::recordModuleExportSlots(const std::string& modulePath) {
    // AUDIT-R5 R5 fix: 仅记录导出“可变变量”的槽位（函数/类导出不快照，保持名称解析）。
    auto varIt = moduleExportVars_.find(modulePath);
    if (varIt == moduleExportVars_.end())
        return;
    auto& slotMap = moduleExportSlots_[modulePath];
    // AUDIT-R6 F6 fix: 先清空本模块旧条目再记录，防止实例复用/模块修改后残留
    // 已删除导出名的旧槽号（importAll 拷贝会遍历整个 slotMap）。
    slotMap.clear();
    for (const auto& name : varIt->second) {
        int slot = lookupGlobalSlot(name);
        if (slot >= 0)
            slotMap[name] = slot;
    }
}

void Compiler::detachModuleExportSlots(const std::string& modulePath) {
    auto slotIt = moduleExportSlots_.find(modulePath);
    if (slotIt == moduleExportSlots_.end())
        return;
    for (const auto& kv : slotIt->second) {
        // marker 含模块槽位号，保证全局唯一，避免 slot→name 表重复名。
        globalSlotAllocator_.detachName(kv.first, "__modexp_" + std::to_string(kv.second) + "_" + kv.first);
    }
}

void Compiler::emitImportSnapshotCopy(const std::string& modulePath, ImportStmt& node) {
    // namespace 导入由 emitNamespaceDict 构造字典快照，此处跳过。
    if (!node.namespaceAlias.empty())
        return;
    auto slotIt = moduleExportSlots_.find(modulePath);
    if (slotIt == moduleExportSlots_.end())
        return;
    const auto& slotMap = slotIt->second;
    auto emitCopy = [&](const std::string& name) {
        auto it = slotMap.find(name);
        if (it == slotMap.end())
            return;
        int moduleSlot = it->second;
        int mainSlot = allocateGlobalSlot(name); // name 已 detach，分配新槽位；循环未 detach 时返回原槽位
        if (mainSlot == moduleSlot)
            return; // 循环导入未 detach：同槽位，跳过自拷贝（保持既有共享行为）
        chunk_.writeOp(OpCode::OP_GET_GLOBAL, node.line);
        chunk_.writeShort(static_cast<uint16_t>(moduleSlot), node.line);
        chunk_.writeOp(OpCode::OP_DEFINE_GLOBAL, node.line);
        chunk_.writeShort(static_cast<uint16_t>(mainSlot), node.line);
    };
    if (node.importAll) {
        // 全量导入（bare import "m"）：拷贝所有导出（顺序不影响语义）
        for (const auto& kv : slotMap)
            emitCopy(kv.first);
    } else {
        for (const auto& name : node.names)
            emitCopy(name);
    }
}

// ============================================================
// P2-11 预编译模块：独立编译入口
// ============================================================
CompileResult Compiler::compileModule(const std::string& source, const std::string& modulePath) {
    // 独立编译模块源码，生成可序列化的 CompileResult。
    // 与 compile() 的关键区别：
    //   1. 模块全局槽位从 0 开始独立编号
    //   2. 收集 export 名称到 result.moduleExports
    //   3. 不设置 moduleLoader_（模块内 import 仍通过 moduleLoader_ 处理，但模块不应再 import 其他模块）
    //
    // 生成的 CompileResult 可通过 BytecodeCache::storeToFile 序列化为 .minic 文件。

    // 1. 词法分析
    Lexer lexer;
    auto tokens = lexer.scan(source);
    if (lexer.getDiagnostics().hasErrors()) {
        const auto& diags = lexer.getDiagnostics().all();
        if (!diags.empty()) {
            error("模块 '" + modulePath + "' 词法错误: " + diags.front().message, diags.front().line,
                  diags.front().column);
        } else {
            error("模块 '" + modulePath + "' 词法错误", 0, 0);
        }
        return {};
    }

    // 2. 语法分析
    Parser parser;
    auto moduleAst = parser.parse(tokens);
    if (parser.hasErrors() || !moduleAst) {
        const auto& diags = parser.getDiagnostics().all();
        if (!diags.empty()) {
            error("模块 '" + modulePath + "' 语法错误: " + diags.front().message, diags.front().line,
                  diags.front().column);
        } else {
            error("模块 '" + modulePath + "' 语法错误", 0, 0);
        }
        return {};
    }

    // 3. 重置编译器状态（与 compile() 相同的状态清理）
    chunk_ = BytecodeChunk();
    chunk_.name = "module:" + modulePath;
    chunk_.arity = 0;
    chunk_.reserveCode(1024);
    varIndex_.clear();
    stringConstIndex_.clear();
    diagnostics_.clear();
    functionChunks_.clear();
    currentLocals_.clear();
    inFunction_ = false;
    classFieldNames_.clear();
    outerLocals_.clear();
    writebackCounter_ = 0;
    peakLocals_ = 0;
    blockDepth_ = 0;
    blockSaveCounter_ = 0;
    compileDepth_ = 0;
    globalSlotAllocator_.clear();
    innerFunctions_.clear();
    innerFunctionSlots_.clear();
    linkedModuleSet_.clear();
    moduleLoadingSet_.clear();
    moduleLoadingStack_.clear();
    moduleExports_.clear();
    moduleExportSlots_.clear(); // AUDIT-R6 F6 fix: R5 新增成员同步清理（与 compile() 对齐）
    moduleExportVars_.clear();  // AUDIT-R6 F6 fix
    moduleAsts_.clear();
    pendingEnumInfos_.clear();

    // 4. 预扫描模块顶层声明，分配全局槽位
    preScanModuleGlobals(*moduleAst);

    // 5. 收集模块导出名称
    collectModuleExports(modulePath, *moduleAst);

    // 6. 内联编译模块语句
    for (auto& stmt : moduleAst->statements) {
        if (stmt)
            compileStatement(stmt.get());
    }

    // 7. 末尾添加 null + return（模块 mainChunk 必须有返回值）
    chunk_.writeOp(OpCode::OP_NULL, 0);
    chunk_.writeOp(OpCode::OP_RETURN, 0);

    // 8. 构建结果
    CompileResult result;
    result.mainChunk = std::move(chunk_);
    result.functionChunks = std::move(functionChunks_);
    result.globalSlotCount = globalSlotAllocator_.count();
    result.globalSlotNames = std::move(globalSlotAllocator_.mutableNames());
    result.enumInfos = std::move(pendingEnumInfos_);

    // 9. 填充 moduleExports（从 moduleExports_ map 提取，排序确保确定性）
    auto expIt = moduleExports_.find(modulePath);
    if (expIt != moduleExports_.end()) {
        result.moduleExports.reserve(expIt->second.size());
        for (const auto& name : expIt->second) {
            result.moduleExports.push_back(name);
        }
        std::sort(result.moduleExports.begin(), result.moduleExports.end());
    }

    // 10. 预计算 IP→指令索引映射
    result.mainChunk.buildIpMap();
    for (auto& kv : result.functionChunks) {
        kv.second.buildIpMap();
    }

    LOG_INFO("模块预编译完成: " + modulePath + " (" + std::to_string(result.globalSlotCount) + " 全局槽, " +
                 std::to_string(result.functionChunks.size()) + " 函数chunk, " +
                 std::to_string(result.moduleExports.size()) + " 导出)",
             "Compiler");

    return result;
}

// ============================================================
// L11 预编译模块（RegisterVM）：独立编译模块源码为 RegisterCompileResult
// ============================================================
RegisterCompileResult Compiler::compileModuleViaRegisterIR(const std::string& source, const std::string& modulePath) {
    // 独立编译模块源码，生成可序列化的 RegisterCompileResult。
    // 与 compileViaRegisterIR 的关键区别：
    //   1. 模块全局槽位从 0 开始独立编号（不与主程序共享）
    //   2. 收集 export 名称到 result.moduleExports
    //   3. 不设置 moduleLoader_（模块内 import 仍通过 moduleLoader_ 处理）
    //
    // 生成的 RegisterCompileResult 可通过 BytecodeCache::storeRegisterToFile
    // 序列化为 MLRC 格式 .minic 文件。

    // 1. 词法分析
    Lexer lexer;
    auto tokens = lexer.scan(source);
    if (lexer.getDiagnostics().hasErrors()) {
        const auto& diags = lexer.getDiagnostics().all();
        if (!diags.empty()) {
            error("模块 '" + modulePath + "' 词法错误: " + diags.front().message, diags.front().line,
                  diags.front().column);
        } else {
            error("模块 '" + modulePath + "' 词法错误", 0, 0);
        }
        return {};
    }

    // 2. 语法分析
    Parser parser;
    auto moduleAst = parser.parse(tokens);
    if (parser.hasErrors() || !moduleAst) {
        const auto& diags = parser.getDiagnostics().all();
        if (!diags.empty()) {
            error("模块 '" + modulePath + "' 语法错误: " + diags.front().message, diags.front().line,
                  diags.front().column);
        } else {
            error("模块 '" + modulePath + "' 语法错误", 0, 0);
        }
        return {};
    }

    // 3. 重置编译器状态（与 compileViaRegisterIR 相同的状态清理）
    diagnostics_.clear();
    linkedModuleSet_.clear();
    moduleLoadingSet_.clear();
    moduleLoadingStack_.clear();
    moduleExports_.clear();
    moduleAsts_.clear();
    pendingEnumInfos_.clear();
    globalSlotAllocator_.clear();

    // 3.5 收集模块导出名称（对齐 compileModule 的 collectModuleExports 调用）
    // IR builder 有自己的 moduleExports_，但 result.moduleExports 从 Compiler::moduleExports_ 提取。
    collectModuleExports(modulePath, *moduleAst);

    // 4. AstIRBuilder 构建模块 IR（独立全局槽位）
    AstIRBuilder irBuilder;
    // 模块编译不转发 precompiledModuleResolver_（模块内的 import 走源码编译）
    lastIR_ = irBuilder.build(*moduleAst);
    if (irBuilder.hasError()) {
        auto irDiags = irBuilder.takeDiagnostics();
        for (const auto& d : irDiags.all()) {
            diagnostics_.add(d);
        }
        if (!irDiags.hasErrors()) {
            error("IR 构建失败", 0, 0);
        }
        return {};
    }
    if (!lastIR_) {
        error("IR 构建失败", 0, 0);
        return {};
    }
    auto irModuleAsts = irBuilder.takeModuleAsts();
    for (auto& ast : irModuleAsts) {
        moduleAsts_.push_back(std::move(ast));
    }

    IRModule* module = irBuilder.getModule();
    module->mainFunction = std::move(lastIR_);
    module->globalSlotNames = irBuilder.getGlobalSlotNames();

    // 5. IR 优化（对齐 compileViaRegisterIR 的优化配置）
    if (irOptimize_) {
        if (irSSAOptimize_) {
            inlinePass(*module);
        }
        optimizeIR(*module->mainFunction, /*enableCopyPropagation=*/false, /*enableDCE=*/true);
        for (auto& fn : module->functions) {
            if (fn)
                optimizeIR(*fn, /*enableCopyPropagation=*/false, /*enableDCE=*/true);
        }
        if (irSSAOptimize_) {
            bool ssaBuilt = ssaConstructPass(*module->mainFunction);
            if (ssaBuilt) {
                gvnPass(*module->mainFunction);
                licmPass(*module->mainFunction);
                ssaDestructPass(*module->mainFunction);
                optimizeIR(*module->mainFunction, /*enableCopyPropagation=*/false, /*enableDCE=*/true);
            }
            for (auto& fn : module->functions) {
                if (fn) {
                    bool fnSsaBuilt = ssaConstructPass(*fn);
                    if (fnSsaBuilt) {
                        gvnPass(*fn);
                        licmPass(*fn);
                        ssaDestructPass(*fn);
                        optimizeIR(*fn, /*enableCopyPropagation=*/false, /*enableDCE=*/true);
                    }
                }
            }
        }
    }

    // 6. IR → RegisterBytecode
    RegisterBytecodeBackend backend;
    if (!backend.lowerModule(*module)) {
        error("Register IR lowering 失败", 0, 0);
        return {};
    }

    RegisterCompileResult result;
    auto mainChunk = backend.takeChunk();
    if (!mainChunk) {
        error("Register IR lowering 未生成 main 字节码", 0, 0);
        return {};
    }
    result.mainChunk = std::move(*mainChunk);
    result.functionChunks = backend.takeFunctionChunks();
    result.globalSlotNames = irBuilder.getGlobalSlotNames();
    result.globalSlotCount = static_cast<int>(result.globalSlotNames.size());
    result.enumInfos = irBuilder.takeEnumInfos();

    // 7. 填充 moduleExports（从 moduleExports_ map 提取，排序确保确定性）
    auto expIt = moduleExports_.find(modulePath);
    if (expIt != moduleExports_.end()) {
        result.moduleExports.reserve(expIt->second.size());
        for (const auto& name : expIt->second) {
            result.moduleExports.push_back(name);
        }
        std::sort(result.moduleExports.begin(), result.moduleExports.end());
    }

    // 8. 预计算 IP→指令索引映射
    result.mainChunk.buildIpMap();
    for (auto& kv : result.functionChunks) {
        kv.second.buildIpMap();
    }

    // 恢复 lastIR_ 供调试/可视化使用
    lastIR_ = std::move(module->mainFunction);

    LOG_INFO("Register VM 模块预编译完成: " + modulePath + " (" + std::to_string(result.globalSlotCount) + " 全局槽, " +
                 std::to_string(result.functionChunks.size()) + " 函数chunk, " +
                 std::to_string(result.moduleExports.size()) + " 导出)",
             "Compiler-RegModule");

    return result;
}

// ============================================================
// P2-11 预编译模块：重命名 OP_CLOSURE/OP_CALL 引用的函数名
// ============================================================
void Compiler::renameClosureRefs(BytecodeChunk& chunk, const std::string& prefix,
                                 const std::unordered_set<std::string>& exportSet,
                                 const std::unordered_set<std::string>& moduleFunctions) {
    // 遍历 code 中的 OP_CLOSURE / OP_CALL 指令，获取 nameIdx → constants[nameIdx].stringVal()，
    // 若名称在 moduleFunctions 中且不在 exportSet 中，则前缀化为 `prefix + name` 并更新常量。
    //
    // 必须同时处理 OP_CLOSURE 和 OP_CALL：
    //   - OP_CLOSURE 创建闭包值，引用函数名常量；非导出函数的闭包值需重命名，
    //     否则 functionChunks_.find(oldName) 在主程序中会找到错误的函数（如主程序同名函数）。
    //   - OP_CALL 按名查找函数；模块内调用非导出函数（如 `main` 调用 `helper`）也需重命名，
    //     否则运行时按原名查找会命中主程序的同名函数，破坏模块隔离语义。
    //
    // 通过 moduleFunctions 精确判断 OP_CALL 引用的是模块内函数还是内置函数：
    //   - 模块内非导出函数（在 moduleFunctions 中，不在 exportSet 中）→ 前缀化
    //   - 模块内导出函数（在 exportSet 中）→ 保持原名
    //   - 内置函数（不在 moduleFunctions 中）→ 保持原名
    //
    // 格式：
    //   OP_CLOSURE: [op(1B), nameIdx(2B), upvalueCount(1B), ...upvalueDescs]
    //   OP_CALL:    [op(1B), nameIdx(2B), argCount(1B)]
    size_t offset = 0;
    while (offset < chunk.code.size()) {
        if (offset >= chunk.code.size())
            break;
        OpCode op = static_cast<OpCode>(chunk.code[offset]);
        // L18 eng-tailcall: OP_TAIL_CALL 布局同 OP_CALL（nameIdx 在 offset+1..+2），
        // 同样需要模块隔离重命名，否则尾调用按原名命中主程序同名函数。
        if (op == OpCode::OP_CLOSURE || op == OpCode::OP_CALL || op == OpCode::OP_TAIL_CALL) {
            if (offset + 2 < chunk.code.size()) {
                uint16_t nameIdx = static_cast<uint16_t>(chunk.code[offset + 1]) |
                                   (static_cast<uint16_t>(chunk.code[offset + 2]) << 8);
                if (nameIdx < chunk.constants.size() && chunk.constants[nameIdx].isString()) {
                    const std::string& fnName = chunk.constants[nameIdx].stringVal();
                    // 仅重命名模块内非导出函数：在 moduleFunctions 中但不在 exportSet 中
                    if (moduleFunctions.count(fnName) > 0 && exportSet.count(fnName) == 0) {
                        chunk.constants[nameIdx] = Value(prefix + fnName);
                    }
                }
            }
        }
        offset += chunk.instructionSizeAt(offset);
    }
}

// ============================================================
// L11 预编译模块（RegisterVM）：重命名 REG_CALL/REG_MAKE_CLOSURE 引用的函数名
// ============================================================
void Compiler::renameRegClosureRefs(RegBytecodeChunk& chunk, const std::string& prefix,
                                    const std::unordered_set<std::string>& exportSet,
                                    const std::unordered_set<std::string>& moduleFunctions) {
    // 遍历 code 中的 REG_CALL / REG_MAKE_CLOSURE 指令，获取 nameIdx →
    // constants[nameIdx].stringVal()，若名称在 moduleFunctions 中且不在 exportSet 中，
    // 则前缀化为 `prefix + name` 并更新常量。
    //
    // 指令格式（nameIdx 位置相同）：
    //   REG_CALL:         [op(1B), dst(1B), nameIdx(2B LE), argCount(1B), args...]
    //   REG_MAKE_CLOSURE: [op(1B), dst(1B), nameIdx(2B LE), uvCount(1B), uvDescs...]
    //                     nameIdx 在 offset+2..offset+3
    size_t offset = 0;
    while (offset < chunk.code.size()) {
        RegOp op = static_cast<RegOp>(chunk.code[offset]);
        // L18 eng-tailcall: REG_TAIL_CALL 布局同 REG_CALL（nameIdx 在 offset+2..+3），
        // 同样需要模块隔离重命名。
        if (op == RegOp::REG_CALL || op == RegOp::REG_MAKE_CLOSURE || op == RegOp::REG_TAIL_CALL) {
            // nameIdx 在 offset+2..offset+3（2B LE）
            if (offset + 3 < chunk.code.size()) {
                uint16_t nameIdx = static_cast<uint16_t>(chunk.code[offset + 2]) |
                                   (static_cast<uint16_t>(chunk.code[offset + 3]) << 8);
                if (nameIdx < chunk.constants.size() && chunk.constants[nameIdx].isString()) {
                    const std::string& fnName = chunk.constants[nameIdx].stringVal();
                    // 仅重命名模块内非导出函数：在 moduleFunctions 中但不在 exportSet 中
                    if (moduleFunctions.count(fnName) > 0 && exportSet.count(fnName) == 0) {
                        chunk.constants[nameIdx] = Value(prefix + fnName);
                    }
                }
            }
        }
        offset += chunk.instructionSizeAt(offset);
    }
}

// ============================================================
// P2-11 预编译模块：加载 .minic 并合并到当前编译
// ============================================================
bool Compiler::loadPrecompiledModule(const std::string& modulePath, ImportStmt& node) {
    // 策略概述：
    //   1. 通过 precompiledModuleResolver_ 获取 .minic 文件路径
    //   2. BytecodeCache::tryLoadFromFile 加载预编译 CompileResult
    //   3. 为模块的 globalSlotNames 在主程序分配对应全局槽位
    //   4. 构建 relocationMap: moduleSlot → mainSlot
    //   5. 模块 mainChunk 转为函数 chunk (__mod_<hash>__init)，
    //      重命名非导出函数引用，重定位全局槽位
    //   6. 模块 functionChunks 重命名+重定位后合并到主程序
    //   7. 主 chunk emit OP_CLOSURE + OP_CALL_EXPR + OP_POP 调用模块初始化
    //   8. 填充 moduleExports_ 供后续具名导入验证

    if (!precompiledModuleResolver_)
        return false;

    std::string minicPath = precompiledModuleResolver_(modulePath);
    if (minicPath.empty())
        return false;

    // L12: 推导源码路径（与 .minic 同目录、同 stem、.mini 扩展名）
    // 用于 tryLoadFromFile 的源码失效校验：若源码 mtime/hash 与 .minic 头部
    // 嵌入值不匹配（源码已修改），返回 nullopt 回退到源码编译。
    // 推导失败（无 .mini 文件）→ sourcePath 为空，tryLoadFromFile 跳过校验（向后兼容）。
    std::string sourcePath;
    {
        namespace fs = std::filesystem;
        fs::path minicP(minicPath);
        if (minicP.has_stem()) {
            fs::path candidate = minicP.parent_path() / (minicP.stem().string() + ".mini");
            std::error_code ec;
            if (fs::exists(candidate, ec)) {
                sourcePath = candidate.string();
            }
        }
    }

    BytecodeCache cache;
    auto moduleResult = cache.tryLoadFromFile(minicPath, sourcePath);
    if (!moduleResult)
        return false; // 加载失败（文件不存在/损坏/校验失败/源码已修改），回退到源码编译

    // 生成模块前缀（用于非导出函数名重命名，避免与主程序冲突）
    // FNV-1a 哈希确保不同模块路径产生不同前缀
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (unsigned char c : modulePath) {
        hash ^= c;
        hash *= 0x100000001b3ULL;
    }
    std::string modPrefix = "__mod_" + std::to_string(hash) + "_";

    // 构建导出名称集合（用于决定哪些函数名不重命名）
    std::unordered_set<std::string> exportSet(moduleResult->moduleExports.begin(), moduleResult->moduleExports.end());

    // P2-11 fix: 构建模块所有函数名集合（导出+非导出），用于 renameClosureRefs
    // 精确判断 OP_CALL/OP_CLOSURE 引用的是模块内函数还是内置函数。
    // 仅模块内非导出函数需要前缀化；内置函数（如 print）保持原名。
    std::unordered_set<std::string> moduleFunctions;
    for (const auto& [fnName, _] : moduleResult->functionChunks) {
        moduleFunctions.insert(fnName);
    }

    // 1. 为模块的全局槽位在主程序分配对应槽位
    //    relocationMap[moduleSlot] = mainSlot（-1 表示不重定位）
    std::vector<int> relocationMap(moduleResult->globalSlotCount, -1);
    for (int i = 0; i < moduleResult->globalSlotCount; ++i) {
        const std::string& name = moduleResult->globalSlotNames[i];
        // 导出名直接在主程序分配槽位（导入方需通过此槽位访问）
        // 非导出名也分配槽位（模块内部代码需要），但名称被前缀化避免主程序访问
        std::string mainName = (exportSet.count(name) > 0) ? name : (modPrefix + name);
        int mainSlot = allocateGlobalSlot(mainName);
        relocationMap[i] = mainSlot;
    }

    // 2. 处理模块 mainChunk：转为函数 chunk
    std::string initFnName = modPrefix + "_init";
    BytecodeChunk& moduleMainChunk = moduleResult->mainChunk;
    moduleMainChunk.name = initFnName;
    // 重命名非导出函数引用（OP_CLOSURE/OP_CALL 的常量池条目）
    renameClosureRefs(moduleMainChunk, modPrefix, exportSet, moduleFunctions);
    // 重定位全局槽位引用
    moduleMainChunk.relocateGlobalSlots(relocationMap);

    // 3. 处理模块 functionChunks：重命名 + 重定位
    for (auto& [oldName, fnChunk] : moduleResult->functionChunks) {
        // 导出函数保持原名，非导出函数前缀化
        std::string newName = (exportSet.count(oldName) > 0) ? oldName : (modPrefix + oldName);
        fnChunk.name = newName;
        renameClosureRefs(fnChunk, modPrefix, exportSet, moduleFunctions);
        fnChunk.relocateGlobalSlots(relocationMap);
        functionChunks_[newName] = std::move(fnChunk);
    }

    // 4. 将模块 mainChunk 添加为函数 chunk
    functionChunks_[initFnName] = std::move(moduleMainChunk);

    // 5. 主 chunk emit 调用模块初始化函数
    //    OP_CLOSURE(nameIdx, upvalueCount=0) + OP_CALL_EXPR(0) + OP_POP
    // 注意：OP_CLOSURE 格式为 [op(1B), nameIdx(2B), upvalueCount(1B), upvalueDescs...]，
    // 没有 argCount 字段（与 visitFunDecl 中的 OP_CLOSURE emit 一致，参见本文件 visitFunDecl）。
    // 多 emit 一个字节会导致后续指令位置错位，触发"常量池索引越界"。
    uint16_t nameIdx = chunk_.addConstant(Value(initFnName));
    chunk_.writeOp(OpCode::OP_CLOSURE, node.line);
    chunk_.writeShort(nameIdx, node.line);
    chunk_.write(static_cast<uint8_t>(0), node.line); // upvalueCount = 0
    chunk_.writeOp(OpCode::OP_CALL_EXPR, node.line);
    chunk_.write(static_cast<uint8_t>(0), node.line); // argCount = 0
    chunk_.writeOp(OpCode::OP_POP, node.line);        // 丢弃返回值

    // 6. 填充 moduleExports_（供具名导入验证和命名空间字典构造）
    std::unordered_set<std::string> exports(moduleResult->moduleExports.begin(), moduleResult->moduleExports.end());
    moduleExports_[modulePath] = std::move(exports);

    // 7. 合并 enum 元信息（模块可能定义 enum，主程序需要知道以校验 OP_BUILD_ENUM_VARIANT）
    for (auto& enumInfo : moduleResult->enumInfos) {
        pendingEnumInfos_.push_back(std::move(enumInfo));
    }

    LOG_INFO("预编译模块加载成功: " + modulePath + " → " + minicPath + " (" +
                 std::to_string(moduleResult->functionChunks.size()) + " 函数chunk)",
             "Compiler");

    return true;
}

// ============================================================
// L11 预编译模块（RegisterVM）：加载 MLRC 格式 .minic 并合并到 lastRegisterResult_
// ============================================================
bool Compiler::loadPrecompiledRegisterModule(const std::string& modulePath, const std::string& initFnName, int line) {
    // 策略概述（对齐 loadPrecompiledModule，操作 RegBytecodeChunk）：
    //   1. 通过 precompiledModuleResolver_ 获取 .minic 文件路径
    //   2. BytecodeCache::tryLoadRegisterFromFile 加载预编译 RegisterCompileResult
    //   3. 为模块的 globalSlotNames 在主程序分配对应全局槽位
    //   4. 构建 relocationMap: moduleSlot → mainSlot
    //   5. 模块 mainChunk 转为函数 chunk (initFnName)，
    //      重命名非导出函数引用，重定位全局槽位
    //   6. 模块 functionChunks 重命名+重定位后合并到 lastRegisterResult_.functionChunks
    //   7. 主 chunk 追加 REG_CALL 调用模块初始化函数
    //   8. 填充 moduleExports_ 供后续具名导入验证
    //
    // 注意：此方法在 compileViaRegisterIR 的 post-lowering 阶段调用，
    // lastRegisterResult_ 已包含主程序的 RegBytecodeChunk。模块的 chunks 合并到其中。

    if (!precompiledModuleResolver_)
        return false;

    std::string minicPath = precompiledModuleResolver_(modulePath);
    if (minicPath.empty())
        return false;

    // L12: 推导源码路径（与 .minic 同目录、同 stem、.mini 扩展名）
    std::string sourcePath;
    {
        namespace fs = std::filesystem;
        fs::path minicP(minicPath);
        if (minicP.has_stem()) {
            fs::path candidate = minicP.parent_path() / (minicP.stem().string() + ".mini");
            std::error_code ec;
            if (fs::exists(candidate, ec)) {
                sourcePath = candidate.string();
            }
        }
    }

    BytecodeCache cache;
    auto moduleResult = cache.tryLoadRegisterFromFile(minicPath, sourcePath);
    if (!moduleResult)
        return false; // 加载失败，回退到源码编译

    // 生成模块前缀（用于非导出函数名重命名，避免与主程序冲突）
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (unsigned char c : modulePath) {
        hash ^= c;
        hash *= 0x100000001b3ULL;
    }
    std::string modPrefix = "__mod_" + std::to_string(hash) + "_";

    // 构建导出名称集合（用于决定哪些函数名不重命名）
    std::unordered_set<std::string> exportSet(moduleResult->moduleExports.begin(), moduleResult->moduleExports.end());

    // 构建模块所有函数名集合（导出+非导出），用于 renameRegClosureRefs
    std::unordered_set<std::string> moduleFunctions;
    for (const auto& [fnName, _] : moduleResult->functionChunks) {
        moduleFunctions.insert(fnName);
    }

    // 1. 为模块的全局槽位在主程序分配对应槽位
    std::vector<int> relocationMap(moduleResult->globalSlotCount, -1);
    for (int i = 0; i < moduleResult->globalSlotCount; ++i) {
        const std::string& name = moduleResult->globalSlotNames[i];
        std::string mainName = (exportSet.count(name) > 0) ? name : (modPrefix + name);
        int mainSlot = allocateGlobalSlot(mainName);
        relocationMap[i] = mainSlot;
    }

    // 2. 处理模块 mainChunk：转为函数 chunk (initFnName)
    RegBytecodeChunk& moduleMainChunk = moduleResult->mainChunk;
    moduleMainChunk.name = initFnName;
    renameRegClosureRefs(moduleMainChunk, modPrefix, exportSet, moduleFunctions);
    moduleMainChunk.relocateGlobalSlots(relocationMap);

    // 3. 处理模块 functionChunks：重命名 + 重定位
    for (auto& [oldName, fnChunk] : moduleResult->functionChunks) {
        std::string newName = (exportSet.count(oldName) > 0) ? oldName : (modPrefix + oldName);
        fnChunk.name = newName;
        renameRegClosureRefs(fnChunk, modPrefix, exportSet, moduleFunctions);
        fnChunk.relocateGlobalSlots(relocationMap);
        lastRegisterResult_.functionChunks[newName] = std::move(fnChunk);
    }

    // 4. 将模块 mainChunk 添加为函数 chunk
    lastRegisterResult_.functionChunks[initFnName] = std::move(moduleMainChunk);

    // 5. 主 chunk 追加 REG_CALL 调用模块初始化函数
    //    REG_CALL 格式: [op(1B), dst(1B), nameIdx(2B LE), argCount(1B)]
    //    dst = 0（寄存器 0，返回值被丢弃），argCount = 0
    //    nameIdx 指向常量池中的 initFnName 字符串
    RegBytecodeChunk& mainChunk = lastRegisterResult_.mainChunk;
    uint16_t nameIdx = static_cast<uint16_t>(mainChunk.constants.size());
    mainChunk.constants.push_back(Value(initFnName));
    mainChunk.code.push_back(static_cast<uint8_t>(RegOp::REG_CALL));
    mainChunk.code.push_back(0); // dst = r0
    mainChunk.code.push_back(static_cast<uint8_t>(nameIdx & 0xFF));
    mainChunk.code.push_back(static_cast<uint8_t>((nameIdx >> 8) & 0xFF));
    mainChunk.code.push_back(0); // argCount = 0
    // lines/columns 对齐
    mainChunk.lines.push_back(line);
    mainChunk.lines.push_back(line);
    mainChunk.lines.push_back(line);
    mainChunk.lines.push_back(line);
    mainChunk.lines.push_back(line);
    mainChunk.columns.push_back(0);
    mainChunk.columns.push_back(0);
    mainChunk.columns.push_back(0);
    mainChunk.columns.push_back(0);
    mainChunk.columns.push_back(0);

    // 6. 填充 moduleExports_（供具名导入验证和命名空间字典构造）
    std::unordered_set<std::string> exports(moduleResult->moduleExports.begin(), moduleResult->moduleExports.end());
    moduleExports_[modulePath] = std::move(exports);

    // 7. 合并 enum 元信息
    for (auto& enumInfo : moduleResult->enumInfos) {
        pendingEnumInfos_.push_back(std::move(enumInfo));
    }

    LOG_INFO("Register VM 预编译模块加载成功: " + modulePath + " → " + minicPath + " (" +
                 std::to_string(moduleResult->functionChunks.size()) + " 函数chunk)",
             "Compiler-RegPrecompiled");

    return true;
}

void Compiler::visitTryStmt(TryStmt& node) {
    // 编译模式:
    //   OP_TRY_BEGIN <catchOffset>
    //   <try block>
    //   OP_TRY_END
    //   OP_JUMP <afterCatch>
    // catchIp:
    //   <bind exception to catchVar>
    //   <catch block>
    // afterCatch:
    //
    // BUG-AUDIT-FINALLY-1: 如果有 finally 块，外层再包一个 OP_TRY_BEGIN/END：
    //   OP_TRY_BEGIN <finallyCatchOffset>    ← 外层 try（捕获异常路径）
    //     <内层 try-catch>
    //   OP_TRY_END                            ← 弹出外层 handler
    //   <finally block>                       ← 正常路径执行 finally
    //   OP_JUMP <afterFinally>
    // finallyCatchIp:                         ← 异常路径
    //   <finally block>（重复一次）
    //   OP_THROW                              ← re-throw（异常值在栈顶）
    // afterFinally:
    //
    // break/continue 与 finally（AUDIT-P1.1 修复，三后端一致）：break/continue 在
    // try-finally 内时，先 push 真实跳转目标到 pendingJumpStack_，再 jump 到 finally
    // 入口，finally 末尾的 OP_FINALLY_END 从栈取出目标续跳——因此 StackVM/RegisterVM
    // 与 Interpreter（走 loopFlow_ 状态标志，try 块正常完成后继续执行 finally）行为一致。
    // return 三后端均执行 finally 后再传播（L4 fix，一致）。
    // 异常值在异常路径的 finally 执行期间保留在栈顶（Block 是栈平衡的），
    // OP_THROW pop 并 re-throw。若 finally 自身 throw，throwException 会截断
    // 栈到外层 handler 的 stackBase（丢弃原异常值），新异常正常传播。
    //
    // 拆分说明（原 357 行单函数 → orchestrator + 3 子阶段）：
    //   - emitTryBlock：OP_TRY_BEGIN + try body + OP_TRY_END + skip-catch OP_JUMP
    //   - emitCatchBlock：catchOffset 回填 + catch 变量绑定 + catch body（含 cleanup wrap）
    //                     + cleanup + OP_CLOSE_UPVALUE + 恢复 currentLocals_ + 回填 afterCatch
    //   - emitFinallyBlock：OP_TRY_END + finallyEntry 回填 + finally body + OP_FINALLY_END
    //                       + 异常路径 + 回填 afterFinally
    // 控制流不变量：emitCatchBlock 返回 false 时直接 return（不 pop tryFinallyStack_，
    // 对齐原实现 catch 块 early return 行为）；emitFinallyBlock 早返回时不 pop
    // tryFinallyStack_，由本函数统一 pop（单次 pop 语义）。

    // AUDIT-P1.1 fix: push try-finally 编译期上下文
    tryFinallyStack_.push_back({node.finallyBlock != nullptr, 0, {}, {}});

    // 0. 如果有 finally，发射外层 OP_TRY_BEGIN
    size_t outerTryBeginIp = 0;
    size_t finallyCatchOffsetPatch = std::string::npos;
    if (node.finallyBlock) {
        outerTryBeginIp = chunk_.code.size();
        chunk_.writeOp(OpCode::OP_TRY_BEGIN, node.line);
        finallyCatchOffsetPatch = chunk_.code.size();
        chunk_.writeShort(0, node.line); // 占位
        ++tryDepth_;                     // 外层 try 计入深度，使 break/continue 发射对应 OP_TRY_END
    }

    // BUG-AUDIT-FINALLY-1: try-finally（无 catch）路径。
    // catchVarName 为空表示无 catch 子句，异常不应被捕获。
    // 外层 OP_TRY_BEGIN（finallyCatchOffset）会捕获异常 → 执行 finally → rethrow。
    // 跳过内层 try-catch 的全部字节码（OP_TRY_BEGIN/catchOffset/catch 变量绑定/cleanup）。
    if (!node.catchVarName.empty()) {
        TryCatchPatchInfo patchInfo;
        emitTryBlock(node, patchInfo);
        if (!emitCatchBlock(node, patchInfo)) {
            // 保留原始控制流：原 catch 块 early return 不 pop tryFinallyStack_。
            // tryFinallyStack_ 在此路径下不 pop（与原实现一致）。
            return;
        }
    } else {
        // try-finally（无 catch）：只编译 try 块，不发射内层 try-catch。
        // 外层 OP_TRY_BEGIN（finallyCatchOffset）会捕获异常 → 执行 finally → rethrow。
        if (node.tryBlock) {
            compileNode(node.tryBlock.get());
        }
    }

    // 9. BUG-AUDIT-FINALLY-1: finally 块字节码
    if (node.finallyBlock) {
        emitFinallyBlock(node, outerTryBeginIp, finallyCatchOffsetPatch);
    }

    // AUDIT-P1.1 fix: pop try-finally 编译期上下文
    tryFinallyStack_.pop_back();

    return;
}

// ============================================================
// visitTryStmt 子阶段实现
// ============================================================

void Compiler::emitTryBlock(TryStmt& node, TryCatchPatchInfo& info) {
    // 1. 发射 OP_TRY_BEGIN（catchOffset 占位，稍后由 emitCatchBlock 回填）
    info.tryBeginIp = chunk_.code.size();
    chunk_.writeOp(OpCode::OP_TRY_BEGIN, node.line);
    info.catchOffsetPatch = chunk_.code.size();
    chunk_.writeShort(0, node.line); // 占位

    // 2. 编译 try 块
    // P0-4 fix: 增加 tryDepth_，使 break/continue 能发射 OP_TRY_END
    ++tryDepth_;
    if (node.tryBlock) {
        compileNode(node.tryBlock.get());
    }
    --tryDepth_;

    // 3. try 块正常结束：弹出 try 处理器，跳过 catch 块
    chunk_.writeOp(OpCode::OP_TRY_END, node.line);
    info.skipCatchJumpPatch = chunk_.code.size();
    chunk_.writeOp(OpCode::OP_JUMP, node.line);
    chunk_.writeShort(0, node.line); // 占位，由 emitCatchBlock 回填为 afterCatch
}

bool Compiler::emitCatchBlock(TryStmt& node, const TryCatchPatchInfo& info) {
    // L27 重构：原 200 行单函数按阶段拆分为 5 个子函数分发，降低圈复杂度。
    // 所有 BUG-7a/7b/7c/BUG-AUDIT-EXC-*/BUG-TRY-1/L1 fix 不变量原样保留。
    // 阶段顺序：回填 catchOffset → 绑定 catch 变量 → 编译 catch body（含 cleanup wrap）
    //          → 恢复作用域 → 回填 afterCatch 跳转。

    // 阶段 1：回填 catchOffset
    if (!patchCatchOffset(node, info)) {
        return false;
    }

    // 阶段 2：绑定 catch 变量（函数内/顶层，含遮蔽保护）
    CatchVarBindInfo bind;
    if (!bindCatchVariable(node, bind)) {
        return false;
    }

    // 阶段 3：编译 catch body（含 cleanup wrap），返回 cleanup 跳转 patch
    // 注：若 cleanupThrowOffset 溢出，emitCatchBodyWithCleanup 已完成 restoreMapping +
    // currentLocals_ 恢复并返回 false，此时不可再调用 restoreCatchScope（会重复恢复 +
    // 错误 emit OP_CLOSE_UPVALUE），直接返回 false 保持原 early-return 语义。
    size_t skipCleanupThrowJumpPatch = std::string::npos;
    if (!emitCatchBodyWithCleanup(node, bind, skipCleanupThrowJumpPatch)) {
        return false;
    }

    // 阶段 4：恢复 catch 作用域（restoreMapping + OP_CLOSE_UPVALUE + closeSlotRanges + 恢复 currentLocals_）
    restoreCatchScope(node, bind);

    // 阶段 5：回填 afterCatch 跳转目标
    if (!patchSkipCatchJumps(node, info, skipCleanupThrowJumpPatch)) {
        return false;
    }

    return true;
}

bool Compiler::patchCatchOffset(TryStmt& node, const TryCatchPatchInfo& info) {
    // 4. 回填 catchOffset
    size_t catchIp = chunk_.code.size();
    size_t catchOffset = catchIp - (info.tryBeginIp + 3);
    // P1-3 fix: 检查 catchOffset 是否溢出 uint16_t
    if (catchOffset > 65535) {
        error("try 块过大，catch 偏移溢出 65535", node.line, 0);
        return false;
    }
    chunk_.code[info.catchOffsetPatch] = static_cast<uint8_t>(catchOffset & 0xFF);
    chunk_.code[info.catchOffsetPatch + 1] = static_cast<uint8_t>((catchOffset >> 8) & 0xFF);
    return true;
}

bool Compiler::bindCatchVariable(TryStmt& node, CatchVarBindInfo& bind) {
    // 5. 在 catchIp 处：异常值已在栈顶，绑定到 catch 变量
    // BUG 7a/7b/7c fix: catch 变量应 shadow 外层同名变量，不覆盖其值；
    // 顶层 catch 变量在 catch 块结束后清理，不泄漏到外层作用域
    bind.savedCatchLocals = currentLocals_;
    bind.catchVarName = node.catchVarName;
    // BUG-AUDIT-EXC-CATCH-CLOSE fix: 记录 catch 变量 slot，catch 块退出时
    // 发射 OP_CLOSE_UPVALUE 关闭指向该 slot 的 open upvalue，防止 slot 复用后
    // 闭包读取错误值（对齐 IR 路径 leaveBlockScope 和 Interpreter closeCapturedVariables）。

    if (inFunction_) {
        // 函数内：始终分配新局部变量槽位（shadow 外层同名变量，不覆盖其值）
        int slot = static_cast<int>(currentLocals_.size());
        if (slot > 255) {
            error("函数局部变量数量超过限制", node.line, 0);
            return false;
        }
        currentLocals_[node.catchVarName] = slot;
        peakLocals_ = std::max(peakLocals_, static_cast<int>(currentLocals_.size()));
        bind.catchVarSlot = slot;
        // BUG-IDE-12 fix: 记录 catch 变量 slot→name
        if (static_cast<size_t>(slot) >= localSlotNames_.size()) {
            localSlotNames_.resize(slot + 1);
        }
        localSlotNames_[slot] = node.catchVarName;
        // L1 fix: 记录 catch 变量到 slotNameRanges_，endIp 暂设为 0（由 closeSlotRanges
        // 在 catch 块退出时回填）。catch 变量的 slot 在 catch 块退出后可能被复用，
        // 需按 IP 范围精确反查避免变量名错位。
        slotNameRanges_.push_back({static_cast<uint8_t>(slot), node.catchVarName, chunk_.code.size(), 0});
        chunk_.writeOp(OpCode::OP_SET_LOCAL, node.line);
        chunk_.write(static_cast<uint8_t>(slot), node.line);
        chunk_.writeOp(OpCode::OP_POP, node.line);
    } else {
        // 顶层：使用块作用域变量（OP_DEFINE_VAR），不覆盖已有全局变量
        bind.shadowedGlobalSlot = (blockDepth_ > 0) ? -1 : lookupGlobalSlot(node.catchVarName);
        if (bind.shadowedGlobalSlot >= 0) {
            // 保存被遮蔽的全局值到临时变量
            bind.hasShadowedGlobal = true;
            bind.shadowedSaveName = "__catch_save_" + std::to_string(blockSaveCounter_++) + "_" + node.catchVarName;
            uint16_t saveIdx = identifierIndex(bind.shadowedSaveName);
            chunk_.writeOp(OpCode::OP_GET_GLOBAL, node.line);
            chunk_.writeShort(static_cast<uint16_t>(bind.shadowedGlobalSlot), node.line);
            chunk_.writeOp(OpCode::OP_DEFINE_VAR, node.line);
            chunk_.writeShort(saveIdx, node.line);
            globalSlotAllocator_.removeMapping(node.catchVarName); // B4: 临时遮蔽
        }
        // 定义 catch 变量
        uint16_t nameIdx = identifierIndex(node.catchVarName);
        chunk_.writeOp(OpCode::OP_DEFINE_VAR, node.line);
        chunk_.writeShort(nameIdx, node.line);
        bind.needCatchVarCleanup = true;
    }
    return true;
}

bool Compiler::emitCatchBodyWithCleanup(TryStmt& node, const CatchVarBindInfo& bind,
                                        size_t& outSkipCleanupThrowJumpPatch) {
    // 6. 编译 catch 块
    // BUG-TRY-1 fix: 若 catch 块内 throw，原实现跳过清理代码，导致 catch 变量泄漏、
    // 被遮蔽的全局值未恢复。修复：用 OP_TRY_BEGIN 包装 catch 块，捕获内层 throw，
    // 跳到 cleanupThrowIp 执行清理代码后 OP_THROW rethrow。
    // 字节码布局：
    //   catchIp: <bind exception>
    //     OP_TRY_BEGIN <cleanupThrowOffset>
    //     <catch block>
    //     OP_TRY_END
    //     <cleanup code>           ← 正常路径
    //     OP_JUMP <afterCatch>
    //   cleanupThrowIp:
    //     <cleanup code>           ← 异常路径（复制）
    //     OP_THROW                 ← rethrow（异常值已在栈顶）
    //   afterCatch:
    //
    // cleanup 字节码栈平衡为 0（OP_DELETE_VAR 不影响栈；OP_GET_VAR+OP_SET_GLOBAL+OP_DELETE_VAR = 0），
    // 异常值保持在栈顶，OP_THROW 可正确 rethrow。
    outSkipCleanupThrowJumpPatch = std::string::npos;
    bool needsCleanupWrap = bind.needCatchVarCleanup || bind.hasShadowedGlobal;
    size_t innerTryBeginIp = 0;
    size_t innerCatchOffsetPatch = std::string::npos;
    if (needsCleanupWrap) {
        innerTryBeginIp = chunk_.code.size();
        chunk_.writeOp(OpCode::OP_TRY_BEGIN, node.line);
        innerCatchOffsetPatch = chunk_.code.size();
        chunk_.writeShort(0, node.line); // 占位，稍后回填为 cleanupThrowOffset
        // BUG-AUDIT-EXC-CLEANUP-TRYDEPTH fix: cleanup wrap 的内层 OP_TRY_BEGIN
        // 必须计入 tryDepth_，使 catch 块内的 break/continue 能发射对应的 OP_TRY_END，
        // 避免 tryStack_ handler 残留导致后续异常被错误捕获到已失效的 cleanupThrowIp。
        ++tryDepth_;
    }
    if (node.catchBlock) {
        compileNode(node.catchBlock.get());
    }
    if (needsCleanupWrap) {
        // BUG-AUDIT-EXC-CLEANUP-TRYDEPTH fix: 对应的 --tryDepth_
        --tryDepth_;
        chunk_.writeOp(OpCode::OP_TRY_END, node.line);
    }

    // 7. 清理顶层 catch 变量并恢复被遮蔽的全局值（正常路径）
    emitCatchCleanupBytecode(bind, node.line);

    if (needsCleanupWrap) {
        // 正常路径：跳过 cleanupThrow 块
        outSkipCleanupThrowJumpPatch = chunk_.code.size();
        chunk_.writeOp(OpCode::OP_JUMP, node.line);
        chunk_.writeShort(0, node.line); // 占位，稍后回填为 afterCatch

        // 异常路径：cleanupThrowIp
        size_t cleanupThrowIp = chunk_.code.size();
        size_t cleanupThrowOffset = cleanupThrowIp - (innerTryBeginIp + 3);
        if (cleanupThrowOffset > 65535) {
            error("catch 块过大，cleanupThrow 偏移溢出 65535", node.line, 0);
            // BUG-TRY-LEAK-1 fix: early return 前必须恢复编译期状态，否则
            // globalSlotAllocator_ 状态不一致 + currentLocals_ 泄漏 catch 变量。
            // 对齐 L1840-L1845 的正常路径恢复逻辑。
            if (bind.hasShadowedGlobal) {
                globalSlotAllocator_.restoreMapping(node.catchVarName, bind.shadowedGlobalSlot);
            }
            currentLocals_ = std::move(bind.savedCatchLocals);
            return false;
        }
        chunk_.code[innerCatchOffsetPatch] = static_cast<uint8_t>(cleanupThrowOffset & 0xFF);
        chunk_.code[innerCatchOffsetPatch + 1] = static_cast<uint8_t>((cleanupThrowOffset >> 8) & 0xFF);

        // 异常路径：发射 cleanup 字节码 + OP_THROW rethrow
        // 此时异常值在栈顶，cleanup 字节码栈平衡为 0，异常值保持栈顶
        emitCatchCleanupBytecode(bind, node.line);
        chunk_.writeOp(OpCode::OP_THROW, node.line);
    }
    return true;
}

void Compiler::emitCatchCleanupBytecode(const CatchVarBindInfo& bind, int line) {
    // cleanup 字节码：清理顶层 catch 变量 + 恢复被遮蔽的全局值
    // 栈平衡为 0（OP_DELETE_VAR 不影响栈；OP_GET_VAR+OP_SET_GLOBAL+OP_DELETE_VAR = 0）
    // 正常路径和异常路径各调用一次（原 emitCleanupBytecode lambda 语义）。
    if (bind.needCatchVarCleanup) {
        uint16_t nameIdx = identifierIndex(bind.catchVarName);
        chunk_.writeOp(OpCode::OP_DELETE_VAR, line);
        chunk_.writeShort(nameIdx, line);
    }
    if (bind.hasShadowedGlobal) {
        uint16_t saveIdx = identifierIndex(bind.shadowedSaveName);
        chunk_.writeOp(OpCode::OP_GET_VAR, line);
        chunk_.writeShort(saveIdx, line);
        chunk_.writeOp(OpCode::OP_SET_GLOBAL, line);
        chunk_.writeShort(static_cast<uint16_t>(bind.shadowedGlobalSlot), line);
        chunk_.writeOp(OpCode::OP_DELETE_VAR, line);
        chunk_.writeShort(saveIdx, line);
    }
}

void Compiler::restoreCatchScope(TryStmt& node, const CatchVarBindInfo& bind) {
    // restoreMapping 是编译期操作（修改 slots_ map），不影响运行时字节码，只调用一次
    if (bind.hasShadowedGlobal) {
        globalSlotAllocator_.restoreMapping(node.catchVarName, bind.shadowedGlobalSlot); // B4: 恢复遮蔽
    }

    // BUG-AUDIT-EXC-CATCH-CLOSE fix: 函数内 catch 变量 slot 在恢复 currentLocals_ 前
    // 必须发射 OP_CLOSE_UPVALUE 关闭指向该 slot 的 open upvalue。否则后续代码声明新
    // 局部变量会复用该 slot 覆盖原值，逃逸的闭包通过 upvalue 读取到错误值（等价悬垂引用）。
    // 对齐 IR 路径 leaveBlockScope（AstIRBuilder.cpp leaveBlockScope）和 Interpreter CatchEnvGuard 析构
    // 调用 closeCapturedVariables 的语义。顶层 catch 变量用 OP_DELETE_VAR 清理，无需此处理。
    if (inFunction_ && bind.catchVarSlot >= 0 && bind.catchVarSlot <= 255) {
        chunk_.writeOp(OpCode::OP_CLOSE_UPVALUE, node.line);
        chunk_.write(static_cast<uint8_t>(bind.catchVarSlot), node.line);
    }

    // L1 fix: 回填 catch 变量及 catch 块内声明的变量 range 的 endIp。
    // savedCatchLocals.size() 是 catch 作用域的 slot 基址，关闭 slot >= 该基址
    // 的全部 open ranges。closeSlotRanges 必须在 currentLocals_ 恢复前调用，
    // 否则 endIp 会被错误地保留为 0（未关闭）。
    closeSlotRanges(bind.savedCatchLocals.size());

    // 恢复 currentLocals_，使 catch 变量不泄漏到外层作用域
    currentLocals_ = std::move(bind.savedCatchLocals);
}

bool Compiler::patchSkipCatchJumps(TryStmt& node, const TryCatchPatchInfo& info, size_t skipCleanupThrowJumpPatch) {
    // 8. 回填跳过 catch 块的跳转目标（OP_JUMP 使用绝对地址）
    size_t afterCatch = chunk_.code.size();
    // P1-3 fix: 检查 afterCatch 是否溢出 uint16_t
    if (afterCatch > 65535) {
        error("代码量过大，跳转目标溢出 65535", node.line, 0);
        return false;
    }
    uint16_t afterCatchTarget = static_cast<uint16_t>(afterCatch);
    chunk_.code[info.skipCatchJumpPatch + 1] = static_cast<uint8_t>(afterCatchTarget & 0xFF);
    chunk_.code[info.skipCatchJumpPatch + 2] = static_cast<uint8_t>((afterCatchTarget >> 8) & 0xFF);
    if (skipCleanupThrowJumpPatch != std::string::npos) {
        chunk_.code[skipCleanupThrowJumpPatch + 1] = static_cast<uint8_t>(afterCatchTarget & 0xFF);
        chunk_.code[skipCleanupThrowJumpPatch + 2] = static_cast<uint8_t>((afterCatchTarget >> 8) & 0xFF);
    }
    return true;
}

void Compiler::emitFinallyBlock(TryStmt& node, size_t outerTryBeginIp, size_t finallyCatchOffsetPatch) {
    --tryDepth_;
    chunk_.writeOp(OpCode::OP_TRY_END, node.line); // 弹出外层 try 处理器

    // AUDIT-P1.1 fix: 记录 finally 入口地址，回填 break/continue 的续跳 patches。
    // finallyEntryIp 是正常路径 finally 块的入口（OP_TRY_END 之后的第一条指令）。
    size_t finallyEntryIp = chunk_.code.size();
    tryFinallyStack_.back().finallyEntryIp = finallyEntryIp;
    // 回填所有 pendingJumpPatches（break/continue 的 OP_JUMP 目标 = finallyEntryIp）
    for (size_t patch : tryFinallyStack_.back().pendingJumpPatches) {
        if (finallyEntryIp > 65535) {
            error("finally 块入口偏移溢出 65535", node.line, 0);
            // 不 pop tryFinallyStack_，由 visitTryStmt 统一 pop（单次 pop 语义）
            return;
        }
        uint16_t target = static_cast<uint16_t>(finallyEntryIp);
        chunk_.code[patch] = static_cast<uint8_t>(target & 0xFF);
        chunk_.code[patch + 1] = static_cast<uint8_t>((target >> 8) & 0xFF);
    }
    // 回填所有 pendingTargetPatches（内层 break/continue 的 OP_PUSH_JUMP_TARGET 目标 = finallyEntryIp）
    for (size_t patch : tryFinallyStack_.back().pendingTargetPatches) {
        if (finallyEntryIp > 65535) {
            error("finally 块入口偏移溢出 65535", node.line, 0);
            // 不 pop tryFinallyStack_，由 visitTryStmt 统一 pop（单次 pop 语义）
            return;
        }
        uint16_t target = static_cast<uint16_t>(finallyEntryIp);
        chunk_.code[patch] = static_cast<uint8_t>(target & 0xFF);
        chunk_.code[patch + 1] = static_cast<uint8_t>((target >> 8) & 0xFF);
    }

    // AUDIT-R6 B1 fix(A): 发射 finally 体（两个副本）期间暂时弹出自身条目。
    // 原实现中 finally 体内的 break/continue/return 经 emitFinallyJump 把自身当作
    // enclosing finally：新增的 pendingJumpPatches 永不回填（回填点已过），OP_JUMP
    // 目标保持占位 0 → 运行时跳回程序开头无限重执行（实证：异常路径每轮泄漏
    // 异常值至“栈溢出”）。弹出后 finally 体内的 break 正确链到更外层 finally 或
    // 直走常规 breakJumps 回填路径；结束后压回，保持调用方统一 pop 语义。
    TryFinallyContext selfFinallyCtx = std::move(tryFinallyStack_.back());
    tryFinallyStack_.pop_back();

    // 正常路径：执行 finally
    compileNode(node.finallyBlock.get());
    // AUDIT-P1.1 fix: finally 块末尾追加 OP_FINALLY_END。
    // 若 pendingJumpStack_ 非空（break/continue 触发），pop 目标并跳转；
    // 否则继续执行（正常完成）。异常路径的 finally 末尾保持 OP_THROW（re-throw）。
    chunk_.writeOp(OpCode::OP_FINALLY_END, node.line);

    // 跳过异常路径
    size_t skipFinallyExceptionJumpPatch = chunk_.code.size();
    chunk_.writeOp(OpCode::OP_JUMP, node.line);
    chunk_.writeShort(0, node.line); // 占位

    // 异常路径：finallyCatchIp
    size_t finallyCatchIp = chunk_.code.size();
    size_t finallyCatchOffset = finallyCatchIp - (outerTryBeginIp + 3);
    if (finallyCatchOffset > 65535) {
        error("try-finally 块过大，finallyCatch 偏移溢出 65535", node.line, 0);
        // AUDIT-R6 B1: 提前返回前压回自身条目，保持调用方统一 pop 语义
        tryFinallyStack_.push_back(std::move(selfFinallyCtx));
        return;
    }
    chunk_.code[finallyCatchOffsetPatch] = static_cast<uint8_t>(finallyCatchOffset & 0xFF);
    chunk_.code[finallyCatchOffsetPatch + 1] = static_cast<uint8_t>((finallyCatchOffset >> 8) & 0xFF);

    // 执行 finally（异常路径，重复一次）
    // AUDIT-R6 B1 fix(B): 异常值先从栈上暂存到唯一临时全局（OP_DEFINE_VAR pop），
    // finally 体在干净栈上执行；正常走完后重新压回并 rethrow。若 finally 体内
    // break/continue/return 跳出，则 reload+rethrow 被跳过——待处理异常被丢弃
    // （Java 式语义，与 Interpreter/IR 路径统一）且无栈残留（原实现异常值留在
    // 栈上，循环内每轮泄漏一个）。临时全局按 try 站点命名（非每次迭代增长），
    // break 跳出时残留一个值可接受（下次经过覆盖，正常路径 DELETE 清理）。
    std::string finallyExcName = "__finally_exc_" + std::to_string(blockSaveCounter_++);
    uint16_t finallyExcIdx = identifierIndex(finallyExcName);
    chunk_.writeOp(OpCode::OP_DEFINE_VAR, node.line);
    chunk_.writeShort(finallyExcIdx, node.line);
    compileNode(node.finallyBlock.get());
    chunk_.writeOp(OpCode::OP_GET_VAR, node.line);
    chunk_.writeShort(finallyExcIdx, node.line);
    chunk_.writeOp(OpCode::OP_DELETE_VAR, node.line);
    chunk_.writeShort(finallyExcIdx, node.line);
    chunk_.writeOp(OpCode::OP_THROW, node.line);

    // AUDIT-R6 B1: 压回自身条目（调用方 visitTryStmt 统一 pop）。
    // 后续 afterFinally 溢出检查的提前 return 也在压回之后，均保持平衡。
    tryFinallyStack_.push_back(std::move(selfFinallyCtx));

    // afterFinally
    size_t afterFinally = chunk_.code.size();
    if (afterFinally > 65535) {
        error("代码量过大，跳转目标溢出 65535", node.line, 0);
        // 不 pop tryFinallyStack_，由 visitTryStmt 统一 pop（单次 pop 语义）
        return;
    }
    uint16_t afterFinallyTarget = static_cast<uint16_t>(afterFinally);
    chunk_.code[skipFinallyExceptionJumpPatch + 1] = static_cast<uint8_t>(afterFinallyTarget & 0xFF);
    chunk_.code[skipFinallyExceptionJumpPatch + 2] = static_cast<uint8_t>((afterFinallyTarget >> 8) & 0xFF);
}

void Compiler::visitPrintStmt(PrintStmt& node) {
    // C2 fix: 与解释器行为对齐 — 多参数用空格拼接后单次输出
    if (node.values.empty()) {
        // print() → 输出空行（与解释器 output("") 一致）
        uint16_t emptyIdx = chunk_.addConstant(Value(std::string("")));
        // D5 fix: 使用 OP_STRING 替代 OP_CONSTANT（二者功能相同，OP_CONSTANT 已废弃）
        chunk_.writeOp(OpCode::OP_STRING, node.line);
        chunk_.writeShort(emptyIdx, node.line);
        chunk_.writeOp(OpCode::OP_PRINT, node.line);
    } else if (node.values.size() == 1) {
        compileNode(node.values[0].get());
        chunk_.writeOp(OpCode::OP_PRINT, node.line);
    } else {
        // 多值：用 OP_ADD 拼接为单字符串（OP_ADD 已支持 string+non-string 拼接）
        compileNode(node.values[0].get());
        // C-P2-6 fix: 空格常量索引提升到循环外，避免每次迭代重复构造和哈希查找
        uint16_t spaceIdx = chunk_.addConstant(Value(std::string(" ")));
        for (size_t i = 1; i < node.values.size(); ++i) {
            // push " " + OP_ADD → 左侧被 toString 后与空格拼接
            // D5 fix: 使用 OP_STRING 替代 OP_CONSTANT
            chunk_.writeOp(OpCode::OP_STRING, node.line);
            chunk_.writeShort(spaceIdx, node.line);
            chunk_.writeOp(OpCode::OP_ADD, node.line);
            // push next value + OP_ADD
            compileNode(node.values[i].get());
            chunk_.writeOp(OpCode::OP_ADD, node.line);
        }
        chunk_.writeOp(OpCode::OP_PRINT, node.line);
    }
    return;
}

void Compiler::visitBlock(Block& node) {
    auto savedLocals = currentLocals_;

    if (!inFunction_) {
        blockDepth_++;

        // 收集块作用域中将要声明的变量名，以便在编译前保存被遮蔽的全局变量
        std::vector<std::pair<std::string, std::string>> shadowedSaves; // (blockVarName, tempSaveName)
        std::vector<std::pair<std::string, int>> removedSlots;          // H1: (name, slot) 用于恢复
        for (auto& stmt : node.statements) {
            if (stmt && stmt->nodeType == NodeType::NODE_VAR_DECL) {
                VarDecl* vd = static_cast<VarDecl*>(stmt.get());
                int gsSlot = lookupGlobalSlot(vd->name);
                if (gsSlot >= 0) {
                    std::string saveName = "__blk_save_" + std::to_string(blockDepth_) + "_" +
                                           std::to_string(blockSaveCounter_++) + "_" + vd->name;
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
            int slot = globalSlotAllocator_.removeMapping(varName); // B4: 临时遮蔽
            if (slot >= 0) {
                removedSlots.push_back({varName, slot});
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
            globalSlotAllocator_.restoreMapping(name, slot); // B4: 恢复遮蔽
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
        // 函数内块作用域：局部变量使用栈槽。
        // BUG-AUDIT-CLOSE-1 fix: 块退出时需发射 OP_CLOSE_UPVALUE，关闭指向本块 slot 的
        // open upvalues，使闭包捕获块退出时刻的值快照（by-value），对齐 IR 路径
        // leaveBlockScope（AstIRBuilder.cpp leaveBlockScope）和 Interpreter 的 closeCapturedVariables。
        // 原实现注释"VM 帧退出时自动释放"是误解——closeUpvaluesFrom 在 OP_RETURN 时
        // 关闭 upvalue 产生的是函数返回时刻的快照（by-reference），与块退出快照语义不一致。
        size_t slotBase = currentLocals_.size(); // 块内第一个新 slot 的编号
        for (auto& stmt : node.statements) {
            compileStatement(stmt.get());
        }
        // 块退出时关闭指向 slot >= slotBase 的全部 open upvalues
        if (currentLocals_.size() > slotBase && slotBase <= 255) {
            chunk_.writeOp(OpCode::OP_CLOSE_UPVALUE, node.line);
            chunk_.write(static_cast<uint8_t>(slotBase), node.line);
        }
        closeSlotRanges(slotBase);        // L1 fix: 回填本块内变量 range 的 endIp
        currentLocals_.swap(savedLocals); // P28: swap
    }
    return;
}

// ---- 新增节点编译 ----
