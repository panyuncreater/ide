#include "ast/ModuleIsolation.h" // BUG-AUDIT-MOD-2: VM 模块隔离（非导出顶层名前缀化）
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

// === CompilerClass: class visitors (split from Compiler.cpp) ===

void Compiler::visitClassDecl(ClassDecl& node) {
    // ── 类编译总策略 ───────────────────────────────────────────────────
    // 类在运行时是一个闭包值（携带方法表）。编译分三步：
    //   1) 字段布局：先收集父类字段（classFieldNames_ 中查 superClassName），再追加
    //      子类自有字段，得到 [父类字段..., 子类字段...] 完整顺序——这是运行时的实例
    //      槽布局，决定 OP_GET_FIELD/OP_SET_FIELD 的索引含义（见 VMContainers.cpp）。
    //   2) 发射 OP_CLASS_NEW <fieldCount> 创建空实例，随后对每个实例字段补发
    //      OP_INIT_FIELD <fieldIdx> 写入由字段初始化表达式求值得到的默认值。
    //   3) 方法编译：每个方法作为嵌套 FunDecl 编译为独立 chunk，并以闭包形式挂到实例
    //      方法表（供 OP_METHOD_CALL 通过 this 派发），同时记录到 classFieldNames_
    //      供其子类继承字段顺序。
    // 类声明本身 emit 一个"类对象值"到栈顶，由调用方（声明语句）消费或 POP。
    //
    // 拆分说明（原 221 行单函数 → orchestrator + 2 子阶段）：
    //   - compileClassMembers：方法编译循环，每迭代创建 CompileContextGuard 并调用
    //                          emitMethodBody；currentClassName_ 在循环内手动保存/恢复
    //   - emitMethodBody：单个方法体编译（chunk 设置 + this/字段/参数槽位 + body +
    //                     元数据保存 + 默认参数值 emit）
    // 不变量保留：R97 #9 fix CompileContextGuard、B1 fix currentClassName_ 保存/恢复、
    // C-P2-3/4 fix slot>255 early return、BUG 6a fix 默认参数折叠（由 emitDefaultValues 统一）。

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
            allFieldNames = it->second; // 父类字段在前
        } else {
            // C-P2-9 fix: 父类未找到时报错，而非静默丢失继承字段（导致 slot 布局错误）
            error("类 '" + node.name + "' 的父类 '" + node.superClassName +
                      "' 未定义（不支持前向引用，请确保父类在子类之前声明）",
                  node.line, node.column);
        }
    }
    for (const auto& fn : ownFieldNames) {
        if (std::find(allFieldNames.begin(), allFieldNames.end(), fn) == allFieldNames.end()) {
            allFieldNames.push_back(fn); // 子类字段在后（跳过覆盖的同名字段）
        }
    }

    // 注册类字段名（供子类编译时查找）
    classFieldNames_[node.name] = allFieldNames;

    // 先编译所有方法为独立 chunk
    compileClassMembers(node, allFieldNames);

    // 主 chunk 中：发射 OP_CLASS_NEW（0 参数构造，字段由 OP_INIT_FIELD 设置）
    chunk_.writeOp(OpCode::OP_CLASS_NEW, node.line);
    chunk_.writeShort(nameIdx, node.line);
    chunk_.write(static_cast<uint8_t>(0), node.line); // argCount = 0（字段由 OP_INIT_FIELD 初始化）

    // 此时栈顶是刚创建的空实例，发射 OP_INIT_FIELD 设置每个字段的默认值
    for (auto& member : node.members) {
        if (member->nodeType != NodeType::NODE_VAR_DECL)
            continue;
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
    //   superNameIdx == NO_INDEX 表示无父类；否则为父类名在常量池中的索引
    chunk_.writeOp(OpCode::OP_DEFINE_CLASS, node.line);
    chunk_.writeShort(nameIdx, node.line);
    if (node.superClassName.empty()) {
        chunk_.writeShort(RuntimeLimits::NO_INDEX, node.line); // 无父类标记
    } else {
        uint16_t superIdx = identifierIndex(node.superClassName);
        chunk_.writeShort(superIdx, node.line);
    }

    // 栈上的模板实例已被 OP_DEFINE_CLASS 消费，无需额外 OP_POP
    return;
}

// ============================================================
// visitClassDecl 子阶段实现
// ============================================================

void Compiler::compileClassMembers(ClassDecl& node, const std::vector<std::string>& allFieldNames) {
    for (auto& member : node.members) {
        if (member->nodeType != NodeType::NODE_FUN_DECL)
            continue;
        FunDecl* funDecl = static_cast<FunDecl*>(member.get());
        // R97 #9 fix: 使用 CompileContextGuard RAII 自动保存/恢复 15 个编译上下文成员变量
        // （含异常路径），替代原 15 行 std::move 保存 + 30 行手动恢复（正常路径 + slot 越界错误路径）。
        // currentClassName_ 不在 CompileContext 中（仅 visitClassDecl 使用），单独手动保存。
        CompileContextGuard guard(*this);
        std::string savedClassName = currentClassName_; // B1 fix

        emitMethodBody(*funDecl, node.name, allFieldNames, guard, node.typeParams);

        // R97 #9 fix: guard 析构自动恢复 15 个上下文变量；currentClassName_ 单独手动恢复。
        // emitMethodBody 可能 early return（slot 越界），currentClassName_ 必须在所有路径恢复，
        // 故放在 emitMethodBody 返回之后统一执行（原实现分别在正常路径与错误路径恢复）。
        currentClassName_ = savedClassName;
        compilingMethodBody_ = false; // AUDIT-R7 F5: 方法编译结束同步清除
    }
}

void Compiler::emitMethodBody(FunDecl& method, const std::string& className,
                              const std::vector<std::string>& allFieldNames, CompileContextGuard& guard,
                              const std::vector<std::string>& classTypeParams) {
    std::string methodKey = className + "." + method.name;

    chunk_ = BytecodeChunk(methodKey, static_cast<int>(method.params.size()));
    chunk_.reserveCode(256); // C21: 预分配方法字节码空间
    // F10: 设置必需参数个数
    chunk_.requiredArity = method.requiredParamCount;
    varIndex_.clear();
    currentLocals_.clear();
    currentUpvalues_.clear(); // C-P2-10 fix: 方法编译使用独立的 upvalue 列表
    currentUpvalueNames_.clear();
    localSlotNames_.clear();       // BUG-IDE-12 fix: 清空槽位名映射
    slotNameRanges_.clear();       // L1 fix: 清空 IP 范围表
    stringConstIndex_.clear();     // R164 fixup2: 清空字符串常量去重缓存（新 chunk 有新常量池）
    currentClassName_ = className; // B1 fix: 记录当前类名供 super 使用
    compilingMethodBody_ = true;   // AUDIT-R7 F5 fix: 方法体 TCO isMethod 判据（guard.saved
                                   // 不管理此字段；compileClassMembers 每轮迭代后由
                                   // visitFunDecl/方法编译入口重新设置，无残留风险）
    // R163 泛型扩展：合并类泛型参数 + 方法泛型参数，供 emitTypeCheck 擦除（对齐 Interpreter invokeMethod 的
    // mergedTypeParams）
    currentTypeParams_ = classTypeParams;
    for (const auto& tp : method.typeParams) {
        currentTypeParams_.push_back(tp);
    }
    // O5: 如果类定义在函数内，设置 outerLocals_ 以检测不支持的闭包捕获
    // C-P2-10 fix: 与 visitFunDecl 一致——使用 guard.saved 引用外层状态
    if (guard.saved.inFunction) {
        outerLocals_ = guard.saved.currentLocals;
        outerUpvalues_ = guard.saved.currentUpvalues;
        outerUpvalueNames_ = guard.saved.currentUpvalueNames;
    } else {
        outerLocals_.clear();
        outerUpvalues_.clear();
        outerUpvalueNames_.clear();
    }
    inFunction_ = true;
    // C-P0-1/C-P0-3 fix: 方法体的循环栈和 try 深度从 0 开始
    loopStack_.clear();
    tryDepth_ = 0;

    // 局部变量映射：slot 0 = this，slot 1..N = 字段（含继承字段），slot N+1.. = 参数
    int slot = 0;
    currentLocals_["this"] = slot++;   // slot 0: this
    localSlotNames_.push_back("this"); // BUG-IDE-12 fix
    for (const auto& fieldName : allFieldNames) {
        currentLocals_[fieldName] = slot++;   // slot 1..N: 实例字段（含继承）
        localSlotNames_.push_back(fieldName); // BUG-IDE-12 fix
    }
    for (int i = 0; i < static_cast<int>(method.params.size()); ++i) {
        currentLocals_[method.params[i]] = slot++;   // slot N+1..: 参数
        localSlotNames_.push_back(method.params[i]); // BUG-IDE-12 fix
    }
    peakLocals_ = slot;
    // C-P1-2 fix: 方法局部变量槽位上限 255（uint8_t 编码限制，含 this/字段/参数）
    // C-P2-3/4 fix: >256 改为 >255（slot==256 截断为 0 与 this 碰撞），并提前 return 避免用截断 slot 继续编译
    if (slot > 255) {
        error("类 '" + className + "' 方法 '" + method.name + "' 局部变量槽位超过限制（最大 256，当前 " +
                  std::to_string(slot) + "，含 this/字段/参数）",
              method.line, 0);
        // currentClassName_ 由 compileClassMembers 在迭代结束时恢复
        return;
    }

    // 记录字段声明顺序（含继承字段），供 VM OP_METHOD_CALL 按序推入
    chunk_.fieldOrder = allFieldNames;

    // L15 TCO: 设置方法 TCO 状态——currentFunctionName_ 用简单方法名（不含 "ClassName." 前缀），
    // 供 visitReturnStmt 识别 return this.method(args) 自调用。entry IP 在 body 编译前记录。
    currentFunctionName_ = method.name;
    currentFunctionDecl_ = &method;
    currentFunctionEntryIp_ = chunk_.code.size();

    if (method.body) {
        compileNode(method.body.get());
    }

    chunk_.writeOp(OpCode::OP_NULL, method.line);
    chunk_.writeOp(OpCode::OP_RETURN, method.line);

    // 记录局部变量总槽位数（含 this/字段/参数和方法体内 var 声明），供 VM 预分配栈空间
    chunk_.localCount = peakLocals_;
    // BUG-IDE-12 fix: 保存 slot→name 映射到 chunk，供 VM 条件断点求值
    chunk_.localSlotNames = localSlotNames_;
    // L1 fix: 保存 IP 范围表到 chunk，供 VM 调试器按 frame.ip 反查变量名
    chunk_.slotNameRanges = slotNameRanges_;
    // W3-2-Bug2 fix: 保存 upvalue 描述符到方法 chunk，供 VM 在类定义时捕获外层函数变量。
    // 对齐 visitFunDecl 的 chunk_.upvalues = std::move(currentUpvalues_)（Compiler.cpp L1623）
    // 和 IR 路径 emitFunctionEpilogue 的 ir_->upvalues 复制（IR.cpp L1951-1954）。
    // 仅当类定义在函数内时 currentUpvalues_ 非空（emitMethodBody 的 outerLocals_ 设置条件：
    // guard.saved.inFunction 为 true）。VM executeDefineClass 会在类定义时为有 upvalue 的
    // 方法创建 VMClosureData 捕获当前帧的栈槽/upvalue。
    chunk_.upvalues = currentUpvalues_;

    // F10: 编译默认参数值为常量（与 visitFunDecl 一致，复用 emitDefaultValues）
    // BUG 6a fix: 方法默认参数值同样递归折叠嵌套一元取反（由 emitDefaultValues 统一实现）
    emitDefaultValues(method);

    functionChunks_[methodKey] = std::move(chunk_);
}

void Compiler::visitMemberAccess(MemberAccess& node) {
    compileNode(node.object.get());
    uint16_t nameIdx = identifierIndex(node.fieldName);
    // O1: super.field — 字段已在构造时通过继承链复制到实例中，与 this.field 等价
    // 方法调用走 OP_SUPER_CALL，此处仅处理字段读取
    bool isSuperAccess = (node.object && node.object->nodeType == NodeType::NODE_SUPER_EXPR);
    chunk_.writeOp(isSuperAccess ? OpCode::OP_SUPER_MEMBER_GET : OpCode::OP_MEMBER_GET, node.line);
    chunk_.writeShort(nameIdx, node.line);
    return;
}

void Compiler::visitMemberAssign(MemberAssign& node) {
    // 根据左值对象类型选择赋值策略
    VarRef* objVar = (node.object && node.object->nodeType == NodeType::NODE_VAR_REF)
                         ? static_cast<VarRef*>(node.object.get())
                         : nullptr;

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
            return;
        }
        // W3-2 fix: upvalue 接收者的字段赋值（闭包内 b.field = val）
        // 原 bug: 只检查 currentLocals_，未检查 upvalue，导致 upvalue 接收者错误走全局变量路径
        // 报"未定义的变量"。修复: 用 OP_GET_UPVALUE 获取对象引用 + OP_MEMBER_SET 修改字段。
        // W3-2-Bug1b fix: fields() 调用 ensureUnique<InstanceData>() 做 COW detach。
        // 当 InstanceData 共享（refcount > 1，如 upvalue 与栈副本同时引用）时，COW 产生新副本，
        // lastMutatedReceiver_ 持有新副本但 upvalue 仍指向旧值。必须发射 OP_WRITEBACK_MEMBER_UPVALUE
        // 将变异后的新副本写回 upvalue。对齐 IR 路径 visitMemberAssign 的 #7 fix（IR.cpp L3084-3088）。
        if (inFunction_) {
            int uvIdx = resolveUpvalue(objVar->name, node.line);
            if (uvIdx >= 0) {
                // 栈序: [obj, val] — OP_MEMBER_SET 弹出 val 和 obj
                chunk_.writeOp(OpCode::OP_GET_UPVALUE, node.line);
                chunk_.write(static_cast<uint8_t>(uvIdx), node.line);
                compileNode(node.value.get());
                uint16_t fieldIdx = identifierIndex(node.fieldName);
                chunk_.writeOp(OpCode::OP_MEMBER_SET, node.line);
                chunk_.writeShort(fieldIdx, node.line);
                // COW detach 后写回 upvalue（整体替换语义，lastMutatedReceiver_ = 变异后基容器）
                chunk_.writeOp(OpCode::OP_WRITEBACK_MEMBER_UPVALUE, node.line);
                chunk_.write(static_cast<uint8_t>(uvIdx), node.line);
                chunk_.writeShort(fieldIdx, node.line);
                return;
            }
        }
        compileNode(node.value.get());
        uint16_t varIdx = identifierIndex(objVar->name);
        uint16_t fieldIdx = identifierIndex(node.fieldName);
        chunk_.writeOp(OpCode::OP_MEMBER_SET_VAR, node.line);
        chunk_.writeShort(varIdx, node.line);
        chunk_.writeShort(fieldIdx, node.line);
        return;
    }

    // M1 fix: 嵌套成员赋值（2 层）
    // 检测 node.object 是否是 IndexAccess(VarRef) 或 MemberAccess(VarRef)
    IndexAccess* outerIdx = (node.object && node.object->nodeType == NodeType::NODE_INDEX_ACCESS)
                                ? static_cast<IndexAccess*>(node.object.get())
                                : nullptr;
    MemberAccess* outerMem = (node.object && node.object->nodeType == NodeType::NODE_MEMBER_ACCESS)
                                 ? static_cast<MemberAccess*>(node.object.get())
                                 : nullptr;
    VarRef* baseVar = nullptr;
    if (outerIdx && outerIdx->object && outerIdx->object->nodeType == NodeType::NODE_VAR_REF)
        baseVar = static_cast<VarRef*>(outerIdx->object.get());
    else if (outerMem && outerMem->object && outerMem->object->nodeType == NodeType::NODE_VAR_REF)
        baseVar = static_cast<VarRef*>(outerMem->object.get());

    if (baseVar) {
        auto localIt = currentLocals_.find(baseVar->name);
        bool isLocal = (localIt != currentLocals_.end());

        // R156 fix: 移除 compileNode(baseVar) —— 不再 push base 变量本身。
        // WRITEBACK_*_LOCAL/VAR 和 MEMBER_SET_LOCAL/VAR / INDEX_SET_LOCAL/VAR 均不需要
        // base 在栈上，原 push base 会导致栈泄漏（每次嵌套赋值泄漏 1 个 Value）。
        // 对齐 visitIndexAssign 的修复模式（见上文 L3470-3471 注释）。
        // 编译外层表达式 → push base[outerIdx] 或 base.field
        compileNode(node.object.get());
        // 编译值 → push val
        compileNode(node.value.get());
        // OP_MEMBER_SET: 弹出 val/outerValue → 修改 → lastMutatedReceiver_
        uint16_t fieldNameIdx = identifierIndex(node.fieldName);
        chunk_.writeOp(OpCode::OP_MEMBER_SET, node.line);
        chunk_.writeShort(fieldNameIdx, node.line);
        // 将变异后的内层容器写回基变量。
        // 对齐 IR 路径（IR.cpp emitNestedAssignWriteback L2696-2708）与 visitIndexAssign 修复模式。
        // R156 fix: 原 outerIdx 路径用 WRITEBACK_INDEX_LOCAL/VAR 整体替换 base 为变异后的 base[outerIdx]
        // （而非 base[outerIdx] = mutated），导致 arr[i].field=val 在非 IR 路径失败。
        // 原 outerMem 路径用 WRITEBACK_MEMBER_LOCAL/VAR 整体替换 base 为变异后的 base.field
        // （而非 base.field = mutated），导致 obj.field.field=val 在非 IR 路径失败。
        // 两条路径均改用 LOAD_MUTATED + INDEX_SET_LOCAL/VAR 或 MEMBER_SET_LOCAL/VAR 直接原地修改。
        if (isLocal) {
            if (outerIdx) {
                // base[outerIdx] = mutated → 重新求值 outerIdx + LOAD_MUTATED + INDEX_SET_LOCAL
                compileNode(outerIdx->index.get());
                chunk_.writeOp(OpCode::OP_LOAD_MUTATED, node.line);
                chunk_.writeOp(OpCode::OP_INDEX_SET_LOCAL, node.line);
                chunk_.write(static_cast<uint8_t>(localIt->second), node.line);
            } else {
                // base.field = mutated → LOAD_MUTATED + MEMBER_SET_LOCAL
                uint16_t outerFieldIdx = identifierIndex(outerMem->fieldName);
                chunk_.writeOp(OpCode::OP_LOAD_MUTATED, node.line);
                chunk_.writeOp(OpCode::OP_MEMBER_SET_LOCAL, node.line);
                chunk_.write(static_cast<uint8_t>(localIt->second), node.line);
                chunk_.writeShort(outerFieldIdx, node.line);
            }
        } else {
            if (outerIdx) {
                // base[outerIdx] = mutated → 重新求值 outerIdx + LOAD_MUTATED + INDEX_SET_VAR
                uint16_t nameIdx = identifierIndex(baseVar->name);
                compileNode(outerIdx->index.get());
                chunk_.writeOp(OpCode::OP_LOAD_MUTATED, node.line);
                chunk_.writeOp(OpCode::OP_INDEX_SET_VAR, node.line);
                chunk_.writeShort(nameIdx, node.line);
            } else {
                // base.field = mutated → LOAD_MUTATED + MEMBER_SET_VAR
                uint16_t varIdx = identifierIndex(baseVar->name);
                uint16_t outerFieldIdx = identifierIndex(outerMem->fieldName);
                chunk_.writeOp(OpCode::OP_LOAD_MUTATED, node.line);
                chunk_.writeOp(OpCode::OP_MEMBER_SET_VAR, node.line);
                chunk_.writeShort(varIdx, node.line);
                chunk_.writeShort(outerFieldIdx, node.line);
            }
        }
        return;
    }

    // C-P2-2 fix: 3+ 层嵌套或复杂表达式：不支持，报错而非静默丢失修改
    error("不支持 3 层及以上嵌套成员赋值（如 a.b.c.d = e）", node.line, node.column);
    compileNode(node.object.get());
    compileNode(node.value.get());
    uint16_t nameIdx = identifierIndex(node.fieldName);
    chunk_.writeOp(OpCode::OP_MEMBER_SET, node.line);
    chunk_.writeShort(nameIdx, node.line);
    return;
}

void Compiler::visitMethodCall(MethodCall& node) {
    // C-P2-11 fix: 参数数量检查移到最前面，避免字节码已发射后报错导致 __wb_idx_ 缓存变量泄漏
    if (node.arguments.size() > 255) {
        error("方法调用参数数量超过限制（最大 255 个）", node.line, 0);
        return;
    }
    // 检查接收者是否为简单变量（VarRef），用于 writeBack
    uint16_t receiverVarIdx = RuntimeLimits::NO_INDEX;  // NO_INDEX = 无全局变量 writeBack
    uint8_t receiverLocalSlot = RuntimeLimits::NO_SLOT; // NO_SLOT = 无局部变量 writeBack
    VarRef* objVar = (node.object && node.object->nodeType == NodeType::NODE_VAR_REF)
                         ? static_cast<VarRef*>(node.object.get())
                         : nullptr;
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
    if (!objVar && node.object && node.object->nodeType == NodeType::NODE_SUPER_EXPR && inFunction_) {
        receiverLocalSlot = 0; // slot 0 = this
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
            chunk_.writeOp(OpCode::OP_DEFINE_VAR, node.line); // #9 fix: DEFINE 而非 SET（首次创建变量）
            chunk_.writeShort(cacheIdx, node.line);
        }
    }

    // B6 fix: 如果索引已缓存，手动内联 IndexAccess 编译（用缓存值代替重新求值）
    if (!cachedIndexVar.empty()) {
        auto* ia = static_cast<IndexAccess*>(node.object.get());
        compileNode(ia->object.get()); // push base (e.g., arr)
        uint16_t cacheIdx = identifierIndex(cachedIndexVar);
        chunk_.writeOp(OpCode::OP_GET_VAR, node.line);
        chunk_.writeShort(cacheIdx, node.line);          // push cached index
        chunk_.writeOp(OpCode::OP_INDEX_GET, node.line); // base[cachedIndex]
    } else {
        compileNode(node.object.get());
    }
    // C-P2-1 fix: 方法调用参数数量检查（已前移到函数开头，C-P2-11 fix 避免缓存变量泄漏）
    for (auto& arg : node.arguments) {
        compileNode(arg.get());
    }
    uint16_t nameIdx = identifierIndex(node.methodName);
    // O1: super.method() 使用 OP_SUPER_CALL（从父类开始方法查找）
    bool isSuperCall = (node.object && node.object->nodeType == NodeType::NODE_SUPER_EXPR);
    chunk_.writeOp(isSuperCall ? OpCode::OP_SUPER_CALL : OpCode::OP_METHOD_CALL, node.line);
    chunk_.writeShort(nameIdx, node.line);
    chunk_.write(static_cast<uint8_t>(node.arguments.size()), node.line);
    chunk_.writeShort(receiverVarIdx, node.line); // 接收者全局变量名索引（NO_INDEX = 无全局 writeBack）
    chunk_.write(receiverLocalSlot, node.line);   // 接收者局部变量 slot（NO_SLOT = 无局部 writeBack）
    if (isSuperCall) {
        // B1 fix: 编码当前类名索引，VM 用它查找父类（而非运行时实例类名）
        uint16_t classIdx = identifierIndex(currentClassName_);
        chunk_.writeShort(classIdx, node.line);
    }

    // 嵌套访问变异方法写回：当接收者是 MemberAccess/IndexAccess 且基对象是 VarRef 时，
    // 方法调用后发射写回指令，确保 this.arr.push(42) 等嵌套调用的修改不丢失。
    // VM 在变异方法调用时将修改后的对象暂存到 lastMutatedReceiver_，
    // 写回指令从中取值写回基对象的字段/索引位置。
    // 若接收者是 VarRef（objVar != nullptr），helper 内部 MemberAccess/IndexAccess
    // 类型检查均不匹配，等价于 no-op（原 if (!objVar && node.object) 守卫的等价简化）。
    emitMethodCallWriteback(node.object.get(), node.line, cachedIndexVar);

    // #9 fix: 清理 __wb_idx_ 缓存变量，防止永久泄漏到 globals_
    if (!cachedIndexVar.empty()) {
        uint16_t cacheIdx = identifierIndex(cachedIndexVar);
        chunk_.writeOp(OpCode::OP_DELETE_VAR, node.line);
        chunk_.writeShort(cacheIdx, node.line);
    }
    return;
}

void Compiler::emitMethodCallWriteback(const ASTNode* receiver, int line, const std::string& cachedIndexVar) {
    // visitMethodCall 子阶段：嵌套访问变异方法写回
    // 当接收者是 MemberAccess/IndexAccess 且基对象是 VarRef 时，发射写回指令，
    // 确保 this.arr.push(42) 等嵌套调用的修改不丢失。
    //
    // W3-2-Bug1 fix: 对齐 IR 路径 emitNestedAssignWriteback 的 3 步序列。
    // 原 bug: 直接发射 OP_WRITEBACK_MEMBER_*，但 lastMutatedReceiver_ 是方法接收者
    // （变异后的成员，如数组），而非整个基容器（如 Box 实例）。WRITEBACK 的"整体替换"
    // 语义会把基容器替换为成员，导致后续访问报"该类型不支持成员访问"。
    // 修复: 先加载基变量 + LOAD_MUTATED + MEMBER_SET/INDEX_SET 在基容器上设置字段/索引，
    // 产生新的变异基容器（OP_MEMBER_SET/OP_INDEX_SET 会更新 lastMutatedReceiver_ 为
    // 整个变异后的基容器），再 WRITEBACK 整体替换变量/upvalue。
    //
    // 若 receiver 为 nullptr / VarRef / SuperExpr 等非 MemberAccess/IndexAccess 类型，
    // 内部类型检查均不匹配，函数为 no-op。
    if (!receiver)
        return;
    if (receiver->nodeType == NodeType::NODE_MEMBER_ACCESS) {
        // MemberAccess 嵌套写回：如 this.arr.push(42)、obj.field.pop()
        auto* ma = static_cast<const MemberAccess*>(receiver);
        if (ma->object && ma->object->nodeType == NodeType::NODE_VAR_REF) {
            auto* baseVar = static_cast<VarRef*>(ma->object.get());
            uint16_t fieldIdx = identifierIndex(ma->fieldName);

            // 步骤 1-3: 加载基变量 + LOAD_MUTATED + MEMBER_SET
            // 栈序: [..., base, mutated_val] → MEMBER_SET → [...]
            // MEMBER_SET 弹出 val 和 obj，设置 obj.field=val，
            // 并将变异后的整个 obj 存入 lastMutatedReceiver_。
            emitLoadVariable(baseVar->name, line);         // push base (e.g., Box)
            chunk_.writeOp(OpCode::OP_LOAD_MUTATED, line); // push mutated member (e.g., array)
            chunk_.writeOp(OpCode::OP_MEMBER_SET, line);   // base.field = mutated; lastMutatedReceiver_ = new base
            chunk_.writeShort(fieldIdx, line);

            // 步骤 4: WRITEBACK 整体替换变量/upvalue 为 lastMutatedReceiver_（新的变异基容器）
            auto localIt = currentLocals_.find(baseVar->name);
            if (localIt != currentLocals_.end()) {
                // 局部变量成员写回：OP_WRITEBACK_MEMBER_LOCAL(slot, fieldIdx)
                chunk_.writeOp(OpCode::OP_WRITEBACK_MEMBER_LOCAL, line);
                chunk_.write(static_cast<uint8_t>(localIt->second), line);
                chunk_.writeShort(fieldIdx, line);
            } else if (inFunction_) {
                // upvalue 接收者的成员写回（闭包内 b.data.push(4)）
                int uvIdx = resolveUpvalue(baseVar->name, line);
                if (uvIdx >= 0) {
                    chunk_.writeOp(OpCode::OP_WRITEBACK_MEMBER_UPVALUE, line);
                    chunk_.write(static_cast<uint8_t>(uvIdx), line);
                    chunk_.writeShort(fieldIdx, line);
                } else {
                    // 全局变量成员写回：OP_WRITEBACK_MEMBER_VAR(varIdx, fieldIdx)
                    chunk_.writeOp(OpCode::OP_WRITEBACK_MEMBER_VAR, line);
                    chunk_.writeShort(identifierIndex(baseVar->name), line);
                    chunk_.writeShort(fieldIdx, line);
                }
            } else {
                // 全局变量成员写回：OP_WRITEBACK_MEMBER_VAR(varIdx, fieldIdx)
                chunk_.writeOp(OpCode::OP_WRITEBACK_MEMBER_VAR, line);
                chunk_.writeShort(identifierIndex(baseVar->name), line);
                chunk_.writeShort(fieldIdx, line);
            }
        }
    } else if (receiver->nodeType == NodeType::NODE_INDEX_ACCESS) {
        // IndexAccess 嵌套写回：如 arr[0].push(42)、dict["key"].remove("x")
        // B6 fix: 使用预缓存的索引值，避免重复求值（对 arr[expr()].method() 防止副作用执行两次）
        auto* ia = static_cast<const IndexAccess*>(receiver);
        if (ia->object && ia->object->nodeType == NodeType::NODE_VAR_REF) {
            auto* baseVar = static_cast<VarRef*>(ia->object.get());
            if (cachedIndexVar.empty())
                return; // 无缓存索引，无法写回（不应发生，visitMethodCall 已保证缓存）

            // 步骤 1-4: 加载基变量 + 加载缓存索引 + LOAD_MUTATED + INDEX_SET
            // 栈序: [..., base, idx, mutated_val] → INDEX_SET → [...]
            // INDEX_SET 弹出 val, idx, obj，设置 obj[idx]=val，
            // 并将变异后的整个 obj 存入 lastMutatedReceiver_。
            emitLoadVariable(baseVar->name, line); // push base (e.g., arr)
            uint16_t cacheIdx = identifierIndex(cachedIndexVar);
            chunk_.writeOp(OpCode::OP_GET_VAR, line); // push cached index
            chunk_.writeShort(cacheIdx, line);
            chunk_.writeOp(OpCode::OP_LOAD_MUTATED, line); // push mutated element
            chunk_.writeOp(OpCode::OP_INDEX_SET, line);    // base[idx] = mutated; lastMutatedReceiver_ = new base

            // 步骤 5: WRITEBACK 整体替换变量/upvalue 为 lastMutatedReceiver_（新的变异基容器）
            auto localIt = currentLocals_.find(baseVar->name);
            if (localIt != currentLocals_.end()) {
                // 局部变量索引写回：OP_WRITEBACK_INDEX_LOCAL(slot)
                chunk_.writeOp(OpCode::OP_WRITEBACK_INDEX_LOCAL, line);
                chunk_.write(static_cast<uint8_t>(localIt->second), line);
            } else if (inFunction_) {
                // upvalue 接收者的索引写回（闭包内 arr[i].push(42)）
                int uvIdx = resolveUpvalue(baseVar->name, line);
                if (uvIdx >= 0) {
                    chunk_.writeOp(OpCode::OP_WRITEBACK_INDEX_UPVALUE, line);
                    chunk_.write(static_cast<uint8_t>(uvIdx), line);
                } else {
                    // 全局变量索引写回：OP_WRITEBACK_INDEX_VAR(varIdx)
                    chunk_.writeOp(OpCode::OP_WRITEBACK_INDEX_VAR, line);
                    chunk_.writeShort(identifierIndex(baseVar->name), line);
                }
            } else {
                // 全局变量索引写回：OP_WRITEBACK_INDEX_VAR(varIdx)
                chunk_.writeOp(OpCode::OP_WRITEBACK_INDEX_VAR, line);
                chunk_.writeShort(identifierIndex(baseVar->name), line);
            }
        }
    }
}

void Compiler::emitLoadVariable(const std::string& name, int line) {
    // 发射变量加载指令，逻辑与 visitVarRef 一致。
    // 优先级: local → upvalue → global slot → global name。
    if (inFunction_) {
        auto it = currentLocals_.find(name);
        if (it != currentLocals_.end()) {
            chunk_.writeOp(OpCode::OP_GET_LOCAL, line);
            chunk_.write(static_cast<uint8_t>(it->second), line);
            return;
        }
        int uvIdx = resolveUpvalue(name, line);
        if (uvIdx >= 0) {
            chunk_.writeOp(OpCode::OP_GET_UPVALUE, line);
            chunk_.write(static_cast<uint8_t>(uvIdx), line);
            return;
        }
    }
    int slot = lookupGlobalSlot(name);
    if (slot >= 0) {
        chunk_.writeOp(OpCode::OP_GET_GLOBAL, line);
        chunk_.writeShort(static_cast<uint16_t>(slot), line);
    } else {
        uint16_t nameIdx = identifierIndex(name);
        chunk_.writeOp(OpCode::OP_GET_VAR, line);
        chunk_.writeShort(nameIdx, line);
    }
}

void Compiler::visitNullLiteral(NullLiteral& node) {
    chunk_.writeOp(OpCode::OP_NULL, node.line);
    return;
}

void Compiler::visitSuperExpr(SuperExpr& node) {
    // 注：super 在非方法上下文中的错误为运行时错误（非编译期）。
    // 直接 Compiler 路径 currentClassName_ 在 visitClassDecl 中设置，方法编译完后恢复。
    // 嵌套函数（FunDecl）内的 super 调用是合法的——visitFunDecl 入口保存 currentClassName_
    // 后不会清除，方法体内嵌套函数仍能继承外层方法名作为 super 上下文。
    // 顶层或非方法体内访问 super 时 currentClassName_ 为空。
    //
    // BUG-INH-AUDIT-6 fix: 原实现无条件 emit OP_GET_LOCAL 0，在顶层运行时
    // 报"内部错误: 局部变量槽越界 (slot 0)"（误导为 VM bug）。现改为：
    // - 方法上下文（currentClassName_ 非空）：emit OP_GET_LOCAL 0（this 槽）
    // - 非方法上下文（currentClassName_ 为空）：emit OP_GET_VAR "this"
    //   VM 在 GLOBAL_NAME 查找失败时报 "未定义的变量: this"，与 IR 路径对齐。
    // 此运行时错误不可被 try/catch 捕获（在 try 块进入前即触发）。
    // 顶层 / 普通函数内 super 的运行时错误行为由 ConsistencyDiff.H7b 与
    // AuditSuper_RuntimeErrorNotCatchableByTryCatch 测试覆盖。
    // 注：Interpreter 报 "super 只能在类方法中使用"（更友好），VM 路径报
    // "未定义的变量: this"——此 2-way 差异文档化为已知，由 H7b 测试 EXPECT_NE 覆盖。
    if (currentClassName_.empty()) {
        // 非方法上下文：emit OP_GET_VAR "this" → 运行时报 "未定义的变量: this"
        uint16_t nameIdx = chunk_.addConstant(Value(std::string("this")));
        chunk_.writeOp(OpCode::OP_GET_VAR, node.line);
        chunk_.write(static_cast<uint8_t>(nameIdx & 0xFF), node.line);
        chunk_.write(static_cast<uint8_t>((nameIdx >> 8) & 0xFF), node.line);
    } else {
        // 方法上下文：emit OP_GET_LOCAL 0（this 槽）
        chunk_.writeOp(OpCode::OP_GET_LOCAL, node.line);
        chunk_.write(0, node.line); // slot 0 = this
    }
    return;
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

bool Compiler::tryFoldBinary(BinOpType opType, ASTNode* left, ASTNode* right, Value& result, int line) {
    // D8 fix: 使用 extractConstant 递归提取常量值，支持嵌套常量表达式（如 (1+2)*3）
    Value lv, rv;
    if (!extractConstant(left, lv, line) || !extractConstant(right, rv, line)) {
        return false;
    }

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
            if (divisor == 0)
                return false; // 除零不折叠，保留运行时错误
            // B3 fix: INT64_MIN / -1 = 溢出 UB，不折叠
            if (!useFloat && OverflowCheck::divOverflow(li, ri))
                return false;
            result = useFloat ? Value(ld / rd) : Value(li / ri);
            return true;
        }
        case BinOpType::BIN_MOD:
            if (!useFloat && ri == 0)
                return false;
            // B3 fix: INT64_MIN % -1 = 溢出 UB，不折叠
            if (!useFloat && OverflowCheck::modOverflow(li, ri))
                return false;
            if (useFloat)
                return false;
            result = Value(li % ri);
            return true;
        case BinOpType::BIN_EQ:
            result = Value(useFloat ? (ld == rd) : (li == ri));
            return true;
        case BinOpType::BIN_NEQ:
            result = Value(useFloat ? (ld != rd) : (li != ri));
            return true;
        case BinOpType::BIN_LT:
            result = Value(useFloat ? (ld < rd) : (li < ri));
            return true;
        case BinOpType::BIN_GT:
            result = Value(useFloat ? (ld > rd) : (li > ri));
            return true;
        case BinOpType::BIN_LTE:
            result = Value(useFloat ? (ld <= rd) : (li <= ri));
            return true;
        case BinOpType::BIN_GTE:
            result = Value(useFloat ? (ld >= rd) : (li >= ri));
            return true;
        default:
            break;
        }
    }

    // 字符串拼接
    if (lv.isString() && rv.isString() && opType == BinOpType::BIN_ADD) {
        result = Value(lv.stringVal() + rv.stringVal());
        return true;
    }

    // 字符串比较
    if (lv.isString() && rv.isString()) {
        // PERF-30 fix: 补全字符串字典序比较折叠（<, >, <=, >=）
        // 原 BIN_EQ/BIN_NEQ 已支持，此处补齐 4 个关系运算符
        const std::string& ls = lv.stringVal();
        const std::string& rs = rv.stringVal();
        switch (opType) {
        case BinOpType::BIN_EQ:
            result = Value(ls == rs);
            return true;
        case BinOpType::BIN_NEQ:
            result = Value(ls != rs);
            return true;
        case BinOpType::BIN_LT:
            result = Value(ls < rs);
            return true;
        case BinOpType::BIN_GT:
            result = Value(ls > rs);
            return true;
        case BinOpType::BIN_LTE:
            result = Value(ls <= rs);
            return true;
        case BinOpType::BIN_GTE:
            result = Value(ls >= rs);
            return true;
        default:
            break;
        }
    }

    // 布尔逻辑（M1 fix: 短路语义，返回操作数原始值而非 bool）
    if (lv.isBool() && rv.isBool()) {
        if (opType == BinOpType::BIN_AND) {
            result = lv.isTruthy() ? rv : lv;
            return true;
        }
        if (opType == BinOpType::BIN_OR) {
            result = lv.isTruthy() ? lv : rv;
            return true;
        }
        if (opType == BinOpType::BIN_EQ) {
            result = Value(lv.boolVal() == rv.boolVal());
            return true;
        }
        if (opType == BinOpType::BIN_NEQ) {
            result = Value(lv.boolVal() != rv.boolVal());
            return true;
        }
    }

    return false;
}

bool Compiler::tryFoldUnary(UnaryOp::UnaryOpType opType, ASTNode* operand, Value& result, int line) {
    if (!operand)
        return false;

    // D8 fix: 使用 extractConstant 递归提取常量值，支持嵌套表达式（如 -(3+2)）
    Value val;
    if (!extractConstant(operand, val, line))
        return false;

    if (opType == UnaryOp::UnaryOpType::UOP_NEGATE) {
        if (val.isInt()) {
            // B3 fix: -INT64_MIN = 溢出 UB，不折叠
            if (OverflowCheck::negateOverflow(val.intVal()))
                return false;
            result = Value(-val.intVal());
            return true;
        }
        if (val.isFloat()) {
            result = Value(-val.floatVal());
            return true;
        }
    }
    if (opType == UnaryOp::UnaryOpType::UOP_NOT && val.isBool()) {
        result = Value(!val.boolVal());
        return true;
    }

    return false;
}

// D8 fix: 递归提取常量值。支持字面量 + 嵌套 BinaryOp/UnaryOp 常量表达式，
// 使 tryFoldBinary/tryFoldUnary 能折叠 (1+2)*3、-(-(5)) 等嵌套表达式。
bool Compiler::extractConstant(ASTNode* node, Value& result, int line) {
    if (!node)
        return false;

    switch (node->nodeType) {
    case NodeType::NODE_NUMBER_LITERAL:
        result = static_cast<NumberLiteral*>(node)->getValue(); // A1 fix: getValue()
        return true;
    case NodeType::NODE_STRING_LITERAL:
        result = static_cast<StringLiteral*>(node)->getValue(); // A1 fix: getValue()
        return true;
    case NodeType::NODE_BOOL_LITERAL:
        result = static_cast<BoolLiteral*>(node)->getValue(); // A1 fix: getValue()
        return true;
    case NodeType::NODE_NULL_LITERAL:
        result = Value::nullValue();
        return true;
    case NodeType::NODE_BINARY_OP: {
        auto* bin = static_cast<BinaryOp*>(node);
        return tryFoldBinary(bin->opType, bin->left.get(), bin->right.get(), result, line);
    }
    case NodeType::NODE_UNARY_OP: {
        auto* un = static_cast<UnaryOp*>(node);
        return tryFoldUnary(un->opType, un->operand.get(), result, line);
    }
    default:
        return false;
    }
}
