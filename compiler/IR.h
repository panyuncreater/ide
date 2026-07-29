/**
 * @file compiler/IR.h
 * @brief 中间表示层（ARCH-06 完整实现）。
 *
 * ARCH-06 fix: 引入 IR 抽象层，打破 Compiler 直接生成字节码的紧耦合。
 * 长期目标：AST → IR → 后端（VM 字节码 / 寄存器字节码 / 未来 JS / WASM）。
 *
 * 数据结构层次：
 *   - IROp：IR 指令操作码（约 50 种，覆盖赋值/算术/比较/跳转/调用/
 *     返回/闭包/类/异常/模块等）
 *   - IROperand：操作数（vreg/常量/标签/全局槽位/字段名等）
 *   - IRInstruction：单条 IR 指令（op + operands + 行号）
 *   - IRBasicBlock：基本块（label + 指令列表 + 后继块）
 *   - IRFunction：函数 IR（基本块列表 + vreg 分配器 + 字段映射）
 *   - IRModule：模块 IR（函数表 + 全局变量 + 类信息 + 模块依赖）
 *
 * 接口层：
 *   - IRBuilder：抽象接口，将 AST 转换为 IRModule
 *   - AstIRBuilder：具体实现，AST → IRModule
 *   - IRBackend：抽象接口，将 IRModule 转换为目标后端字节码
 *   - BytecodeIRBackend：IR → 栈式 VM 字节码 BytecodeChunk
 *   - RegisterBytecodeBackend：IR → 寄存器式 VM 字节码 RegBytecodeChunk
 *
 * IR 优化 pass（C4 增强）：
 *   - constantFoldingPass：常量折叠（编译期可计算的算术/比较）
 *   - copyPropagationPass：复制传播（vreg_a = vreg_b 后续用 vreg_b 替代 vreg_a）
 *   - deadCodeEliminationPass：死代码消除（无副作用指令的 dest vreg 无引用则删除）
 *   - commonSubexpressionEliminationPass：公共子表达式消除（基本块内值编号）
 *   - loopUnrollingPass：循环展开（for 循环体 ≤20 指令且 N≤4 时展开）
 *   - optimizeIR：组合 pass，按 round 重复执行直到收敛（最多 3 轮）
 *
 * 设计原则：
 *   - IR 与现有字节码解耦，可独立扩展为 SSA / Dataflow 等高级形式
 *   - 现有 AST→字节码路径保持不变，IR 作为可选中间层（Compiler::setUseIR(true)）
 *   - IRBuilder/IRBackend 为抽象接口，便于未来添加新前端和新后端
 *
 * 安全约束：
 *   - Phase-5 fix: POP 指令现携带所消费的 vreg 操作数（对齐 IR spec `POP: [src_vreg]`），
 *     使 DCE 能看到 POP 对 vreg 的消费关系。因此 DCE/CSE 现在对栈式 VM 后端也是安全的。
 *   - DCE 不能删除带副作用的指令（除零/溢出/LOAD_GLOBAL 未定义等），
 *     仅删除 isPureCompute() 为 true 的纯计算指令（LOAD_CONST/算术/比较/DUP 等）。
 *   - CSE 安全性依赖于 POP-vreg 注解：CSE 替换 dest vreg 引用后原指令变为死代码，
 *     DCE 可安全删除（POP 不引用它，而是引用替代品的 vreg）。
 *
 * @see Compiler AstIRBuilder BytecodeIRBackend RegisterBytecodeBackend
 */
#pragma once

// ============================================================
// IR.h - 中间表示层（ARCH-06 完整实现）
// ============================================================
// ARCH-06 fix: 引入 IR 抽象层，打破 Compiler 直接生成字节码的紧耦合。
// 长期目标：AST → IR → 后端（VM 字节码 / 未来 JS / WASM）。
//
// 本次实现：
//   1. IROp/IROperand/IRInstruction/IRBasicBlock/IRFunction 数据结构
//   2. IRBuilder 接口 + AstIRBuilder 具体实现（AST → IR）
//   3. IRBackend 接口 + BytecodeIRBackend 具体实现（IR → VM 字节码）
//   4. IRToString 调试输出
//
// 设计原则：
//   - IR 与现有字节码解耦，可独立扩展为 SSA / Dataflow 等高级形式
//   - 现有 AST→字节码路径保持不变，IR 作为可选中间层（Compiler::setUseIR(true)）
//   - IRBuilder/IRBackend 为抽象接口，便于未来添加新前端和新后端
// ============================================================

#include "ast/ASTNode.h"                  // VM-IMPORT: Block 完整定义（moduleAsts_ 需要 unique_ptr<Block> 析构）
#include "common/Diagnostic.h"            // P2-12: AstIRBuilder 用 DiagnosticBag 替代私有三元组
#include "compiler/Bytecode.h"            // IRBackend lowering 到 BytecodeChunk + UpvalueDesc
#include "compiler/GlobalSlotAllocator.h" // B4: 全局槽位分配器
#include <cstdint>
#include <cstring>    // BUG-AUDIT-VAL-1: std::memcpy for scalarKey float 位模式编码
#include <functional> // VM-IMPORT: std::function for moduleLoader_
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map> // perf3 fix: addGlobal/addConstant hash 侧表
#include <unordered_set>
#include <utility> // R110: std::pair for emitMatchPattern 返回类型
#include <vector>

// ============================================================
// IR 操作数
// ============================================================

/// IR 操作数类型
enum class IROperandKind : uint8_t {
    CONSTANT,    // 常量（int/float/string/bool/null，索引到 IRFunction.constants）
    VIRTUAL,     // 虚拟寄存器（SSA-like，由 IRBuilder 分配，lowering 到栈槽）
    LABEL,       // 基本块标签（跳转目标）
    GLOBAL_NAME, // 全局变量名（索引到 IRFunction.globalNames）
    LOCAL_SLOT,  // 局部变量槽位（函数内的栈槽编号）
    UPVALUE_IDX, // upvalue 索引（闭包捕获描述符）
    FIELD_NAME,  // 字段名（索引到 IRFunction.globalNames，复用名称池）
    FUNC_NAME,   // 函数名（索引到 IRFunction.globalNames，复用名称池）
    IMM_UINT,    // 立即数（uint32_t，用于 arg_count/slot 等）
};

/// IR 操作数
struct IROperand {
    IROperandKind kind;
    uint32_t index = 0; // 由具体 kind 解释

    static IROperand constant(uint32_t idx) { return {IROperandKind::CONSTANT, idx}; }
    static IROperand vreg(uint32_t idx) { return {IROperandKind::VIRTUAL, idx}; }
    static IROperand label(uint32_t idx) { return {IROperandKind::LABEL, idx}; }
    static IROperand global(uint32_t idx) { return {IROperandKind::GLOBAL_NAME, idx}; }
    static IROperand local(uint32_t slot) { return {IROperandKind::LOCAL_SLOT, slot}; }
    static IROperand upvalue(uint32_t idx) { return {IROperandKind::UPVALUE_IDX, idx}; }
    static IROperand field(uint32_t idx) { return {IROperandKind::FIELD_NAME, idx}; }
    static IROperand funcName(uint32_t idx) { return {IROperandKind::FUNC_NAME, idx}; }
    static IROperand imm(uint32_t val) { return {IROperandKind::IMM_UINT, val}; }
};

// ============================================================
// IR 指令
// ============================================================

/// IR 操作码（三地址码风格，与 VM OpCode 解耦）
/// 覆盖 MiniLang 所有 AST 节点语义，支持完整 AST → IR → Bytecode 管线
enum class IROp : uint8_t {
    // ---- 常量加载 ----
    LOAD_CONST, // dest = constants[idx]       operands: [dest_vreg, const_idx]
    LOAD_NULL,  // dest = null                 operands: [dest_vreg]
    LOAD_TRUE,  // dest = true                 operands: [dest_vreg]
    LOAD_FALSE, // dest = false                operands: [dest_vreg]

    // ---- 变量访问（三态：local/global/upvalue/name）----
    LOAD_LOCAL,    // dest = local[slot]          operands: [dest_vreg, slot]
    STORE_LOCAL,   // local[slot] = src           operands: [slot, src_vreg]
    LOAD_GLOBAL,   // dest = globalNames[idx]     operands: [dest_vreg, global_idx]
    STORE_GLOBAL,  // globalNames[idx] = src      operands: [global_idx, src_vreg]
    DEFINE_GLOBAL, // define globalNames[idx] = src  operands: [global_idx, src_vreg]
    DELETE_VAR,    // delete global var by name   operands: [global_idx]  BUG-IR-TRY-1 fix
    LOAD_UPVALUE,  // dest = upvalue[idx]         operands: [dest_vreg, uv_idx]
    STORE_UPVALUE, // upvalue[idx] = src          operands: [uv_idx, src_vreg]
    CLOSE_UPVALUE, // close all open upvalues with slot >= slot_base  operands: [slot_base]

    // ---- 算术运算（三地址码：dest = src1 OP src2）----
    ADD,
    SUB,
    MUL,
    DIV,
    MOD,
    NEGATE, // dest = -src                 operands: [dest, src]

    // ---- 比较 ----
    EQ,
    NEQ,
    LT,
    GT,
    LTE,
    GTE,

    // ---- 逻辑（非短路，短路在 AST 层已展开为分支）----
    NOT,

    // ---- 控制流 ----
    JUMP,          // jump label                  operands: [label_idx]
    JUMP_IF_FALSE, // if !src jump label          operands: [src_vreg, label_idx]
    LABEL,         // 基本块标记                   operands: [label_idx]

    // ---- 调用 ----
    CALL,        // dest = call name(args)      operands: [dest, name_idx, arg_count, arg1, arg2, ...]
    CALL_EXPR,   // dest = call(closure, args)  operands: [dest, closure_vreg, arg_count, arg1, ...]
    RETURN,      // return src                  operands: [src_vreg]
    RETURN_NULL, // return null（隐式返回）

    // R164 协程/生成器：yield 表达式
    // operands: [dest_vreg, src_vreg]  dest = yield 表达式结果（重放模式下 = src）
    // 重放模式：若当前 yield 命中目标 yieldId，VM 抛出 VMYieldSignal 返回 src 值；
    //           否则 dest = src，继续执行函数体。
    YIELD,

    // ---- 闭包 ----
    MAKE_CLOSURE, // dest = closure(name, upvalues)  operands: [dest, name_idx, uv_count, uv1_isLocal, uv1_idx, ...]

    // ---- 容器 ----
    BUILD_ARRAY, // dest = [args...]            operands: [dest, count, arg1, arg2, ...]
    BUILD_DICT,  // dest = {k1:v1, k2:v2, ...}  operands: [dest, pair_count, k1, v1, k2, v2, ...]
    BUILD_TUPLE, // R98 元组与解构：dest = (args...)  operands: [dest, count, arg1, arg2, ...] (immutable)
    INDEX_GET,   // dest = obj[idx]             operands: [dest, obj_vreg, idx_vreg]
    INDEX_SET,   // obj[idx] = val              operands: [obj_vreg, idx_vreg, val_vreg]

    // R99 枚举与 ADT + match
    // 构造 enum variant：dest = EnumName.VariantName(arg1, arg2, ...)
    // operands: [dest, enumNameConstIdx, variantNameConstIdx, arg_count, arg1, arg2, ...]
    BUILD_ENUM_VARIANT,
    // 检查 scrutinee 是否为指定 enum variant：dest_bool = (scrut is EnumName.VariantName)
    // operands: [dest_bool, scrut_vreg, enumNameConstIdx, variantNameConstIdx]
    ENUM_VARIANT_NAME,
    // 取 enum variant 字段：dest = scrutinee.fields[idx]
    // operands: [dest, scrut_vreg, idx_imm]
    ENUM_VARIANT_FIELD,

    // ---- 成员访问 ----
    MEMBER_GET, // dest = obj.field            operands: [dest, obj_vreg, field_idx]
    MEMBER_SET, // obj.field = val             operands: [obj_vreg, field_idx, val_vreg]
    // L7 fix: 方法体内 this.field = val 直接修改局部槽位（slot 0 = this），
    // 避免 MEMBER_SET 通过栈副本修改导致 COW detach 后原 slot 不变。
    // operands: [slot(LOCAL_SLOT), field_idx(FIELD_NAME), val_vreg(VIRTUAL)]
    MEMBER_SET_LOCAL,
    SUPER_MEMBER_GET, // dest = super.field         operands: [dest, this_vreg, field_idx]

    // ---- 方法调用 ----
    METHOD_CALL, // dest = obj.method(args)     operands: [dest, obj_vreg, method_idx, arg_count, args...]
    SUPER_CALL,  // dest = super.method(args)   operands: [dest, this_vreg, method_idx, class_idx, arg_count, args...]

    // ---- 类 ----
    DEFINE_CLASS, // define class name           operands: [name_idx]
    CLASS_NEW,    // dest = new ClassName(args)  operands: [dest, name_idx, arg_count, args...]
    INIT_FIELD,   // init field name from stack  operands: [field_idx]

    // ---- 异常处理 ----
    TRY_BEGIN,      // try block begin             operands: [catch_label_idx]
    TRY_END,        // try block end
    THROW,          // throw src                   operands: [src_vreg]
    LOAD_EXCEPTION, // dest = pendingException     operands: [dest_vreg]  P1-4 fix: catch 块起始加载异常值

    // ---- 写回指令（嵌套左值变异，B1/B6 fix）----
    // 当左值为 a.b.c 或 a[i] 时，变异结果需写回到原始变量/字段
    WRITEBACK_MEMBER_VAR,   // 成员写回到全局变量   operands: [var_idx, field_idx]
    WRITEBACK_MEMBER_LOCAL, // 成员写回到局部变量   operands: [slot, field_idx]
    WRITEBACK_INDEX_VAR,    // 索引写回到全局变量   operands: [var_idx]
    WRITEBACK_INDEX_LOCAL,  // 索引写回到局部变量   operands: [slot]
    // #7 fix: upvalue 写回（嵌套左值变异 a[i]=.. / a.f=.. 其中 a 是 upvalue）
    WRITEBACK_MEMBER_UPVALUE, // 成员写回到 upvalue  operands: [uv_idx, field_idx]
    WRITEBACK_INDEX_UPVALUE,  // 索引写回到 upvalue  operands: [uv_idx]

    // ---- 其他 ----
    PRINT,        // print src                   operands: [src_vreg]
    POP,          // 释放 src                    operands: [src_vreg]
    DUP,          // 复制 src 到 dest             operands: [dest, src_vreg]
    LOAD_MUTATED, // dest = lastMutatedReceiver_  operands: [dest_vreg]
                  // MEDIUM-1/2 fix: 嵌套左值写回链中读取上一级 SET 产生的变异后容器。
                  // 不清除 lastMutatedReceiver_，后续 SET/WRITEBACK 会覆盖。

    // 2026-06-29: 运行时类型注解检查
    // operands: [src_vreg, type_const_idx]  type_const_idx 为 CONSTANT 类型（字符串注解）
    // lower 到 OP_TYPE_CHECK（栈式）/ REG_TYPE_CHECK（寄存器式）
    TYPE_CHECK,

    // AUDIT-P1.1 fix: break/continue finally 续跳机制
    PUSH_JUMP_TARGET, // push 跳转目标到 pendingJumpStack_  operands: [label_idx]
    FINALLY_END,      // 从 pendingJumpStack_ pop 目标并跳转；栈空则继续执行

    // R134 模式匹配扩展：获取容器长度（与 OP_LEN 对应）。
    // operands: [dest_vreg, src_vreg]  dest = len(src)
    // 支持 array/dict/string/tuple（与 StackVM OP_LEN 三后端一致）。
    // 用于 TUPLE pattern 编译期元素数检查。
    LEN,
    // R134 模式匹配扩展：软类型测试（与 OP_TYPE_TEST 对应）。
    // operands: [dest_vreg, src_vreg, type_const_idx]  dest = typeMatch(src, annotation)
    // 与 IROp::TYPE_CHECK 区别：不抛错，写 bool 到 dest。
    // 用于 TUPLE pattern 类型检查（不匹配时 fall through 而非抛错）。
    TYPE_TEST,

    // ============================================================
    // P2-10 IR SSA 基础设施：PHI 节点（真正的 SSA）
    // -----------------------------------------------------------
    // SSA 构造阶段（Cytron 支配边界算法）在合并点插入 PHI 合并多分支到达的
    // 局部变量值，使局部变量进入 SSA 形态，解锁跨分支值分析（GVN/LICM 等）。
    //
    // operands: [dest_vreg, slot(LOCAL_SLOT), pred1_nodeid(IMM_UINT), vreg1(VIRTUAL),
    //             pred2_nodeid(IMM_UINT), vreg2(VIRTUAL), ...]
    //   dest_vreg = 由前驱块到达时的合并值
    //   slot = 此 PHI 合并的局部变量槽位（ssaDestructPass 据此还原 STORE_LOCAL）
    //   (pred_nodeid, vreg) 对表示"从 pred_nodeid 前驱进入时取 vreg 的值"
    //   pred_nodeid 为 CFG node ID（IMM_UINT），非 labelIndex（LABEL）。
    //   原方案用 labelIndex 标识前驱，但 fall-through 块无 LABEL 时继承所在
    //   IRBasicBlock 的 labelIndex，导致同块内多 CFG 节点共享 labelIndex 产生歧义。
    //
    // 生命周期约束（重要）：
    //   - PHI 仅在 SSA 构造（ssaConstructPass）与 SSA 析构（ssaDestructPass）之间存在。
    //   - SSA 析构必须在 backend lowering 之前完成，将 PHI 拆解为前驱块尾部的
    //     STORE_LOCAL / copy 序列。因此 BytecodeIRBackend / RegisterBytecodeBackend
    //     永远不会看到 PHI 指令（ssaDestructPass 已消除）。
    //   - 若 lowering 意外遇到 PHI，视为编译器内部错误（assert + 返回失败）。
    PHI,

    // L18 eng-tailcall: 互递归尾调用 return g(args)（g != 当前函数）。
    // operands: [dest_vreg, name_idx(GLOBAL_NAME), arg_count(IMM), arg1_vreg, ...]
    // 与 CALL 同布局；后端 lowering 为 OP_TAIL_CALL / REG_TAIL_CALL，紧跟 RETURN dest。
    // 运行时若目标可帧复用则 TCO（跳过后随 RETURN），否则降级为普通调用
    //（返回后执行后随 RETURN，语义与 CALL+RETURN 完全等价）。
    TAIL_CALL,
};

/// IR 指令
struct IRInstruction {
    IROp op;
    std::vector<IROperand> operands;
    int line = 0; // 源码行号（用于调试）

    IRInstruction(IROp o, std::vector<IROperand> ops, int ln = 0) : op(o), operands(std::move(ops)), line(ln) {}
};

// ============================================================
// IR 基本块与函数
// ============================================================

/// IR 基本块（指令序列 + 终结指令）
struct IRBasicBlock {
    std::vector<IRInstruction> instructions;
    uint32_t labelIndex = 0; // 对应 IRFunction.labels 中的索引
};

/// IR 函数（含 main chunk）
struct IRFunction {
    std::string name;
    std::vector<IRBasicBlock> blocks;
    std::vector<Value> constants;         // 常量池
    std::vector<std::string> globalNames; // 全局变量名池（复用存储字段名/函数名）
    // perf3 fix: hash 侧表加速 addGlobal/addConstant 去重（O(n²)→O(n)）。
    // 仅经 addGlobal/addConstant 维护，constants/globalNames 不被外部直接修改，保持一致。
    std::unordered_map<std::string, uint32_t> globalNameIdx_;
    std::unordered_map<std::string, uint32_t> stringConstIdx_;
    // P2-1 fix: 非字符串常量（int/float/bool）hash 侧表。key 编码方式见 scalarKey()
    std::unordered_map<std::string, uint32_t> scalarConstIdx_;
    uint32_t nextVReg = 0;  // 下一个虚拟寄存器号
    uint32_t nextLabel = 0; // 下一个标签号

    // 函数元数据（lowering 时使用）
    int arity = 0;                             // 参数个数
    int requiredArity = 0;                     // 必需参数个数
    int localCount = 0;                        // 局部变量总槽位数
    std::vector<uint16_t> defaultConstIndices; // 默认参数值的常量索引
    std::vector<UpvalueDesc> upvalues;         // 闭包 upvalue 描述符列表
    // BUG-IDE-12 fix: 局部变量槽位→名称映射（索引即 slot），供 RegisterVM 条件断点求值反查。
    // 由 AstIRBuilder 在分配 LOCAL slot 时增量维护，函数最终化时复制到 ir_->localSlotNames。
    // 限制：槽位复用（兄弟作用域）时后声明的变量名覆盖先前的，属于已知限制。
    std::vector<std::string> localSlotNames;
    // L1 fix（2026-07-19）: 基于 IR 指令范围的 slot→name 反查表，解决兄弟作用域槽位
    // 复用导致的变量名错位。startInstr/endInstr 是 IR 指令展平序号（跨基本块）。
    // BytecodeIRBackend/RegisterBytecodeBackend lowering 时通过 irToBytecodeOffset_
    // 映射翻译为字节码 IP 范围，填入 chunk_->slotNameRanges。
    struct SlotNameRange {
        uint32_t slot = 0;
        std::string name;
        size_t startInstr = 0;
        size_t endInstr = 0; // 0 表示未关闭（函数级变量，fallback 到 localSlotNames）
    };
    std::vector<SlotNameRange> slotNameRanges;

    // R164 协程/生成器：标记此函数为生成器（fun* 声明）。
    // 后端 lowering 时复制到 BytecodeChunk::isGenerator / RegBytecodeChunk::isGenerator。
    // VM/RegisterVM 在 CALL 时检测此标志：若为 true，创建协程值而非直接调用。
    bool isGenerator = false;
    // R164 协程/生成器：yield 总数（从 FunDecl.yieldCount 复制）。
    // 静态 yield 数或 kDynamicYieldCount（INT_MAX，表示存在循环内 yield）。
    int yieldCount = 0;
    static constexpr int kDynamicYieldCount = 2147483647; // INT_MAX

    /// 分配虚拟寄存器
    IROperand allocVReg() { return IROperand::vreg(nextVReg++); }
    /// 分配标签
    uint32_t allocLabel() { return nextLabel++; }
    /// P2-1 fix: 将 int/float/bool 标量常量编码为字符串 key（含类型标识避免 int 1 == bool true 等误判）
    /// BUG-AUDIT-VAL-1 fix: float 改用位模式编码，避免 std::to_string(double) 仅 6 位小数导致
    /// 不同位模式的 double（如 0.1000001 与 0.1000002）被错误合并为同一常量。
    static std::string scalarKey(const Value& v) {
        if (v.isInt())
            return "I:" + std::to_string(v.intVal());
        if (v.isFloat()) {
            double d = v.floatVal();
            uint64_t bits;
            std::memcpy(&bits, &d, sizeof(double));
            return "F:" + std::to_string(bits);
        }
        if (v.isBool())
            return v.boolVal() ? "B:1" : "B:0";
        return {}; // 不会触达
    }
    /// 添加常量，返回索引（已存在则复用）
    uint32_t addConstant(const Value& v) {
        // P2-1 fix: 字符串与非字符串常量都用 hash 侧表 O(1) 查找。
        // 字符串：以 stringVal() 为 key（O(n) 内容比较的 equals 在大常量池下退化）
        // 数值/布尔：以 std::pair<int64_t,double>+类型 标识为 key，避免线性扫描。
        // null/Instance/Array/Dict 等复杂类型：保留线性扫描（此类常量极少出现）
        if (v.isString()) {
            auto it = stringConstIdx_.find(v.stringVal());
            if (it != stringConstIdx_.end())
                return it->second;
        } else if (v.isInt() || v.isFloat() || v.isBool()) {
            auto it = scalarConstIdx_.find(scalarKey(v));
            if (it != scalarConstIdx_.end())
                return it->second;
        } else {
            for (size_t i = 0; i < constants.size(); ++i) {
                if (constants[i].equals(v))
                    return static_cast<uint32_t>(i);
            }
        }
        // 与 BytecodeChunk/RegBytecodeChunk 一致：常量池索引需 fit 到 uint16_t，
        // 超限抛异常以避免调用方 static_cast<uint16_t> 静默截断。
        if (constants.size() >= 65535) {
            throw std::runtime_error("IR 常量池索引超出 65535 上限");
        }
        constants.push_back(v);
        uint32_t idx = static_cast<uint32_t>(constants.size() - 1);
        if (v.isString()) {
            stringConstIdx_[v.stringVal()] = idx;
        } else if (v.isInt() || v.isFloat() || v.isBool()) {
            scalarConstIdx_[scalarKey(v)] = idx;
        }
        return idx;
    }
    /// 添加全局变量名/字段名/函数名，返回索引
    uint32_t addGlobal(const std::string& globalName) {
        // perf3 fix: hash 侧表 O(1) 查找替代 O(n) 线性扫描
        auto it = globalNameIdx_.find(globalName);
        if (it != globalNameIdx_.end())
            return it->second;
        // P2-2 fix: 与 addConstant 对齐——globalNames 索引经 writeShort 编码为 uint16_t，
        // 超限抛异常以避免调用方 static_cast<uint16_t> 静默截断（读写错误全局名）。
        if (globalNames.size() >= 65535) {
            throw std::runtime_error("IR 全局名池索引超出 65535 上限");
        }
        globalNames.push_back(globalName);
        uint32_t idx = static_cast<uint32_t>(globalNames.size() - 1);
        globalNameIdx_[globalName] = idx;
        return idx;
    }
    /// 添加基本块，返回引用
    IRBasicBlock& addBlock(uint32_t labelIdx) {
        blocks.push_back({});
        blocks.back().labelIndex = labelIdx;
        return blocks.back();
    }
};

/// IR 模块（包含 main 函数和所有子函数）
/// 用于多函数 lowering：AstIRBuilder 收集所有函数 IR，BytecodeIRBackend 逐个 lowering
struct IRModule {
    std::unique_ptr<IRFunction> mainFunction;              // 主函数（顶层代码）
    std::vector<std::unique_ptr<IRFunction>> functions;    // 子函数列表
    std::unordered_map<std::string, size_t> functionIndex; // 函数名 → functions 索引
    // BUG-NEW fix: 全局槽位名表（slot → name），供 BytecodeIRBackend lowering 时
    // 将 WRITEBACK_*_VAR 的 GLOBAL_SLOT (IMM_UINT) 转换为名称常量索引。
    // 栈式 VM 的 OP_WRITEBACK_*_VAR 将 varIdx 当作常量池索引处理（取 stringVal()），
    // 若直接 emit 槽位号会误读为常量索引，导致 "未定义的变量" 或类型断言失败。
    // RegisterBytecodeBackend 用高 bit 标记区分 SLOT/NAME，无需此表。
    std::vector<std::string> globalSlotNames;

    void addFunction(std::unique_ptr<IRFunction> fn) {
        functionIndex[fn->name] = functions.size();
        functions.push_back(std::move(fn));
    }
    IRFunction* findFunction(const std::string& name) const {
        auto it = functionIndex.find(name);
        if (it == functionIndex.end())
            return nullptr;
        return functions[it->second].get();
    }
};

// ============================================================
// IRBuilder 接口（前端：AST → IR）
// ============================================================

class Block;

/// IRBuilder 抽象接口。
/// 子类实现具体 AST 节点到 IR 的转换逻辑。
class IRBuilder {
public:
    virtual ~IRBuilder() = default;
    /// 从 AST 构建 IR。返回 IRFunction（main chunk）。
    /// 子类必须实现具体转换逻辑。
    virtual std::unique_ptr<IRFunction> build(Block& program) = 0;
};

// ============================================================
// AstIRBuilder：AST → IR 具体实现
// ============================================================

/// 将 AST 转换为 IR 的具体构建器。
/// 覆盖核心 AST 节点：表达式（BinaryOp/UnaryOp/字面量/VarRef）、
/// 控制流（If/While/For）、函数（FunDecl/FunCall/Return）、
/// 变量声明（VarDecl/Assignment）、容器（Array/Dict/Index）、
/// 成员访问（MemberAccess/MethodCall）、类（ClassDecl）、异常（Try/Throw）。
///
/// 设计说明：
///   - 采用 Visitor 模式遍历 AST，但为避免与 Interpreter 的 Visitor 耦合，
///     使用独立的 visit* 方法（不继承 DefaultVisitor）。
///   - 变量解析四态（local/global/upvalue/name）在 IR 层用不同 IROp 表达，
///     lowering 时映射到对应的 VM OpCode。
///   - 闭包 upvalue 在 IR 层用 MAKE_CLOSURE 指令显式建模捕获列表。
///   - 全局变量支持槽位分配（OP_GET_GLOBAL/OP_SET_GLOBAL）和名称索引（OP_GET_VAR/OP_SET_VAR）。
///   - 块作用域：函数内块嵌套时跟踪深度，块退出回收局部变量槽位。
///   - 写回指令：嵌套左值（a.b.c = x / a[i] = x）编译为 WRITEBACK_* 指令。
class AstIRBuilder : public IRBuilder {
public:
    AstIRBuilder();
    ~AstIRBuilder() override = default;

    std::unique_ptr<IRFunction> build(Block& program) override;

    /// 获取构建的 IR 模块（含所有子函数）。build() 后有效。
    // D3 fix: 补 const 重载，便于编译后只读检查 IR 模块（如 IRTransformPanel 渲染）
    IRModule* getModule() { return module_.get(); }
    const IRModule* getModule() const { return module_.get(); }

    /// 获取全局槽位名表（build() 后有效，供 Compiler 填充 CompileResult）
    const std::vector<std::string>& getGlobalSlotNames() const { return globalSlotAllocator_.names(); }

    /// BUG-MOD-1 fix: IR 构建错误报告接口。
    /// P2-12 fix: 改用 DiagnosticBag 替代私有的 hasError_/errorMessage_/errorLine_ 三元组。
    /// 编译期错误统一收集到 irDiagnostics_，Compiler 在 compileViaIR/compileViaRegisterIR
    /// 中调用 takeDiagnostics() 转移并合并到 Compiler::diagnostics_，消除"私有字段→手动转化"
    /// 的冗余路径。hasError()/errorMessage()/errorLine() 保留为兼容接口，委托到 irDiagnostics_。
    bool hasError() const { return irDiagnostics_.hasErrors(); }
    /// P2-12 fix (错误恢复): 是否有致命错误——build() 顶层循环仅对致命错误中止。
    bool hasFatalError() const { return irDiagnostics_.hasFatalErrors(); }
    const std::string& errorMessage() const {
        static const std::string empty;
        const auto& all = irDiagnostics_.all();
        for (auto it = all.rbegin(); it != all.rend(); ++it) {
            if (it->isError())
                return it->message;
        }
        return empty;
    }
    int errorLine() const {
        const auto& all = irDiagnostics_.all();
        for (auto it = all.rbegin(); it != all.rend(); ++it) {
            if (it->isError())
                return it->line;
        }
        return 0;
    }
    /// P2-12: 获取 IR 构建期间收集的诊断包（只读）
    const DiagnosticBag& diagnostics() const { return irDiagnostics_; }
    /// P2-12: 转移诊断包所有权给 Compiler 合并
    DiagnosticBag takeDiagnostics() {
        DiagnosticBag result = std::move(irDiagnostics_);
        irDiagnostics_.clear();
        return result;
    }

    // VM-IMPORT: 模块加载器（使 IR 路径也支持 import 语句的内联编译）
    // Compiler 在 compileViaIR/compileViaRegisterIR 中调用 setModuleLoader 注入。
    // build() 后调用 takeModuleAsts() 转移模块 AST 所有权给 Compiler 保留。
    void setModuleLoader(std::function<std::string(const std::string&)> loader) { moduleLoader_ = std::move(loader); }
    std::vector<std::unique_ptr<Block>> takeModuleAsts() { return std::move(moduleAsts_); }

    /// L11 预编译模块（RegisterVM）：设置 .minic 文件路径解析器。
    /// 仅在 compileViaRegisterIR 路径中转发（compileViaIR 不转发，保持 IR 路径无预编译支持）。
    /// 回调接收模块路径，返回对应的 .minic 文件系统路径（空表示无预编译文件）。
    void setPrecompiledModuleResolver(std::function<std::string(const std::string&)> resolver) {
        precompiledModuleResolver_ = std::move(resolver);
    }

    /// L11 预编译模块（RegisterVM）：pending 模块加载记录。
    /// handleImportStmt 检测到 .minic 时记录，Compiler 在 post-lowering 阶段
    /// 调用 loadPrecompiledRegisterModule 处理。
    struct PendingRegPrecompiledModule {
        std::string modulePath; // 规范化模块路径
        std::string initFnName; // 模块初始化函数名（__mod_<hash>___init）
        int line;               // 导入语句行号
    };
    std::vector<PendingRegPrecompiledModule> takePendingRegPrecompiledModules() {
        return std::move(pendingRegPrecompiledModules_);
    }

    /// R99 enum 校验：返回 build() 期间收集的 enum 元信息（编译期→运行时传递）。
    /// Compiler 在 compileViaIR/compileViaRegisterIR 中调用并写入 CompileResult.enumInfos。
    std::vector<VMEnumInfo> takeEnumInfos() { return std::move(enumInfos_); }

private:
    std::unique_ptr<IRFunction> ir_;
    IRBasicBlock* currentBlock_ = nullptr; // 当前基本块（指令追加目标）
    std::unique_ptr<IRModule> module_;     // IR 模块（收集所有函数）
    // P2-12: 替代原 hasError_/errorMessage_/errorLine_ 三元组，统一诊断收集
    DiagnosticBag irDiagnostics_;

    // 变量解析状态
    struct VarInfo {
        // L7 fix: 新增 MEMBER kind——方法体内裸字段访问解析为 this.field（MEMBER_GET/MEMBER_SET），
        // 不注册为 LOCAL slot，避免 StackVM（推字段槽）与 RegisterVM（不推字段槽）帧布局不一致。
        enum class Kind { LOCAL, GLOBAL_SLOT, GLOBAL_NAME, UPVALUE, MEMBER } kind;
        uint32_t
            index; // LOCAL→slot, GLOBAL_SLOT→槽位号, GLOBAL_NAME→globalNames idx, UPVALUE→uv idx, MEMBER→field name idx
    };
    std::unordered_map<std::string, VarInfo> varMap_;
    std::unordered_map<std::string, std::string> varTypes_; // 2026-06-29: 变量名→类型注解
    bool inFunction_ = false;
    std::string currentFunctionReturnType_; // BUG-TYPE-1 fix: 当前函数返回类型注解
    std::vector<std::string>
        currentTypeParams_; // R163 泛型扩展：当前函数/方法的类型参数（empty=非泛型），用于 emitTypeCheckIR 擦除
    // R109 TCO: 当前函数 TCO 状态。emitFunctionPrologue/visitFunDecl 设置，
    // visitReturnStmt 据此判断 return f(args) 是否可优化为"参数赋值 + JUMP 函数入口 label"。
    // 不变量：三字段由 CompileContext 统一保存/恢复，嵌套函数编译后外层状态复原。
    std::string currentFunctionName_;              // TCO: 当前函数名（空表示非函数体）
    const FunDecl* currentFunctionDecl_ = nullptr; // TCO: 当前函数 FunDecl 指针（非拥有，AST 生命周期内有效）
    uint32_t currentFunctionEntryLabel_ = 0;       // TCO: 当前函数体入口 basic block label（JUMP 目标）
    bool currentFunctionIsMethod_ = false;         // TCO: 当前函数是否为类方法（方法 slot 0 是 this 不能被覆盖）
    uint32_t nextLocalSlot_ = 0;
    // BUG-IDE-12 fix: 局部变量 slot→name 映射（索引即 slot），跨作用域累积（不随块退出清除）。
    // 函数最终化时复制到 ir_->localSlotNames，供 RegisterVM 条件断点求值反查变量名。
    std::vector<std::string> localSlotNames_;
    // L1 fix（2026-07-19）: 基于 IR 指令范围的 slot→name 映射，解决兄弟作用域槽位复用
    // 导致的变量名错位。每个 range 记录变量名在 [startInstr, endInstr) 范围内占用 slot。
    // 函数最终化时复制到 ir_->slotNameRanges，供 BytecodeIRBackend/RegisterBytecodeBackend
    // lowering 时翻译为字节码 IP 范围填入 chunk_->slotNameRanges。
    std::vector<IRFunction::SlotNameRange> slotNameRanges_;
    // C-9 fix: 编译类方法时为 true。visitFunDecl 检查此标记，
    // 预留 slot 0 给隐式 this 参数，并将 varMap_["this"] 绑定到 slot 0。
    // 调用方（executeMethodCallImpl/executeClassNewImpl）将 this 作为第一个参数传入。
    bool compilingMethod_ = false;
    // P1 fix: 当前编译的类名（visitClassDecl 设置，供 super 调用查找父类）。
    // compilingMethod_=true 时有效，编译完类方法后清空。
    std::string compilingClassName_;
    // R163 泛型扩展：当前编译类的类型参数列表（visitClassDecl 设置，供 emitFunctionBody 合并到 currentTypeParams_）。
    std::vector<std::string> compilingClassTypeParams_;
    // BUG-IR-VARDECL-1 fix: 已定义的类名集合（visitClassDecl 时填充）。
    // visitVarDecl 无初始化器时检查类型注解是否为类名，若是则自动构造实例，
    // 对齐 Compiler.cpp visitVarDecl L620-626 的 S2 fix 语义。
    std::unordered_set<std::string> definedClassNames_;
    // L7 fix: 类名 → 完整字段名列表（含继承字段，父类字段在前）映射。
    // visitClassDecl 时构建，供子类编译时查找父类字段顺序。
    // 对齐 Compiler.cpp classFieldNames_ 的语义。
    std::unordered_map<std::string, std::vector<std::string>> classFieldNames_;
    // L7 fix: 当前编译类的完整字段列表（含继承字段），由 visitClassDecl 设置，
    // emitFunctionPrologue 在 compilingMethod_ 分支中消费——将每个字段注册为局部变量，
    // 使方法体内裸访问字段（如 x = ax）解析为 this 的字段槽位而非全局变量。
    // 对齐 Compiler.cpp emitMethodBody L4635-4646 的字段→局部变量映射。
    std::vector<std::string> compilingClassFieldNames_;

    // 块作用域跟踪（限制5）
    struct BlockScope {
        std::vector<uint32_t> localSlots; // 本块声明的局部变量槽位（退出时回收）
        uint32_t slotBase = 0;            // 进入块时的 nextLocalSlot_ 值（退出时回收到此）
        bool hasNestedFunction = false;   // 本块内是否创建了嵌套函数（闭包），
                                          // 若有则不回收槽位（闭包可能捕获了本块的局部变量）
        // CRITICAL-2 fix: 块作用域遮蔽保存栈。当内块 var x 与外块同名时，
        // visitVarDecl 覆盖 varMap_ 前将旧条目压入此栈，leaveBlockScope 时恢复。
        // 保证外层绑定在块退出后可达，对齐 Interpreter 的作用域链语义。
        struct ShadowedVar {
            std::string name;
            VarInfo info;
            bool hadOld; // varMap_ 中是否已有同名旧条目（无则块退出时删除）
        };
        std::vector<ShadowedVar> shadowedVars;
        // R164 fixup2: 本块作用域内已声明的变量名集合（含参数，用于同作用域重定义检测）。
        // 嵌套块各有独立的 declaredNames，允许内块遮蔽外块变量（对齐 Interpreter
        // tryDefineNew 仅检查当前 Environment 的行为）。
        std::unordered_set<std::string> declaredNames;
    };
    std::vector<BlockScope> blockScopes_;
    int blockDepth_ = 0; // 当前块嵌套深度（仅在 inFunction_==true 时有效）

    // R164 fixup2: 待注入到函数体块作用域的参数名列表。
    // visitFunDecl 设置参数后填充，visitBlock 进入函数体块时消费（注入 declaredNames）。
    // 使 visitVarDecl 能检测参数重定义（如 `fun f(b) { returna b; }` 中 `returna b;`
    // 被解析为 VarDecl(name="b")，与参数 b 冲突）。
    std::vector<std::string> pendingFunctionParams_;

    // 全局槽位管理（限制3 / B4: 委托给 GlobalSlotAllocator）
    GlobalSlotAllocator globalSlotAllocator_;

    int allocateGlobalSlot(const std::string& name) { return globalSlotAllocator_.allocate(name); }
    int lookupGlobalSlot(const std::string& name) const { return globalSlotAllocator_.lookup(name); }

    // VM-IMPORT: 模块系统状态（对齐 Compiler 的 moduleLoadingSet_/linkedModuleSet_）
    std::function<std::string(const std::string&)> moduleLoader_;
    std::unordered_set<std::string> moduleLoadingSet_; // 正在编译中（循环检测）
    std::vector<std::string> moduleLoadingStack_;      // BUG-AUDIT-MOD-3: 深度保护栈
    std::unordered_set<std::string> linkedModuleSet_;  // 已完成（run-once）
    std::vector<std::unique_ptr<Block>> moduleAsts_;   // 保留模块 AST
    // BUG-AUDIT-MOD-1: 模块导出名称集合（对齐 Compiler::moduleExports_）
    std::unordered_map<std::string, std::unordered_set<std::string>> moduleExports_;
    // AUDIT-R5 R5 fix（快照导入）：模块导出名 → 模块内部全局槽位（内联期记录，detach 前）。
    // 对齐 Compiler::moduleExportSlots_。导入方从此槽位拷贝快照到自身新分配的槽位。
    std::unordered_map<std::string, std::unordered_map<std::string, int>> moduleExportSlots_;
    // AUDIT-R5 R5 fix：模块导出的“可变变量”名（仅 VarDecl）。快照只适用于变量导出；
    // 函数/类导出不拷贝不 detach（否则 varMap_ 重定向会损坏类实例化/函数调用）。
    std::unordered_map<std::string, std::unordered_set<std::string>> moduleExportVars_;
    // L11 预编译模块（RegisterVM）：.minic 文件路径解析器
    // 仅 compileViaRegisterIR 路径转发；compileViaIR 不转发保持无预编译支持
    std::function<std::string(const std::string&)> precompiledModuleResolver_;
    // L11 pending 预编译模块加载记录（handleImportStmt 填充，Compiler post-lowering 消费）
    std::vector<PendingRegPrecompiledModule> pendingRegPrecompiledModules_;
    // R99 enum 校验：build() 期间收集的 enum 元信息，takeEnumInfos() 转移给 Compiler。
    std::vector<VMEnumInfo> enumInfos_;

    /// VM-IMPORT: 处理 import 语句（内联编译模块代码到当前 IR）
    void handleImportStmt(ImportStmt& node);
    /// VM-IMPORT: 路径解析后的下一步动作（resolveImportPath 返回值）
    enum class ImportPathStatus {
        kContinue,        // 路径已解析，继续加载模块
        kAlreadyLoaded,   // 模块已加载过（run-once），仅校验具名导入
        kCircularLoading, // P2-14: 循环导入延迟加载，跳过本次内联编译
        kError,           // 已设置 hasError_，调用方直接返回
    };
    /// VM-IMPORT: 路径规范化与安全校验 + run-once 检查 + 循环依赖检测 + 深度保护
    /// 成功时 outPath 填入规范化路径，返回 kContinue / kAlreadyLoaded / kCircularLoading；
    /// 失败时设置 hasError_/errorMessage_/errorLine_ 并返回 kError。
    /// BUG-M4 fix: outLoaderPath 保留大小写（供 loader/预编译解析器），outCacheKey 为去重键
    //（Windows 小写折叠），供所有去重/隔离用途。
    ImportPathStatus resolveImportPath(ImportStmt& node, std::string& outLoaderPath, std::string& outCacheKey);
    /// VM-IMPORT: 加载模块源码 + 解析为 AST + 模块隔离重命名
    /// BUG-M4 fix: loaderPath 供 moduleLoader_（保留大小写），cacheKey 供 rename 隔离前缀。
    bool loadImportedModule(ImportStmt& node, const std::string& loaderPath, const std::string& cacheKey,
                            std::unique_ptr<Block>& outAst);
    /// VM-IMPORT: 具名导入验证（检查 node.names 是否在模块 export 集合中）
    void bindImportedNames(ImportStmt& node, const std::string& path);
    /// P2-11: 命名空间导入 IR 生成（import * as ns from "path"）
    /// 对每个 export 名 emit LOAD_CONST(key) + LOAD_GLOBAL(value)，
    /// 然后 BUILD_DICT + DEFINE_GLOBAL(namespaceAlias)。
    /// 在 run-once 与首次加载路径均需调用。
    void emitNamespaceImportIR(ImportStmt& node, const std::string& path);

    /// AUDIT-R5 R5 fix（快照导入）：记录模块导出名的内部全局槽位到 moduleExportSlots_。
    /// 在槽位分配完成后（preScan+collect 之后 / 预编译分配之后）调用。
    void recordModuleExportSlots(const std::string& path);
    /// AUDIT-R5 R5 fix：detach 模块导出名（使导入方可另分配新槽位承载快照副本）。
    /// 在模块内联 lowering 完成后调用一次（模块内部引用已按槽位索引固化）。
    void detachModuleExportSlots(const std::string& path);
    /// AUDIT-R5 R5 fix：为具名/全量导入 emit 快照拷贝 IR（LOAD_GLOBAL 模块槽 → DEFINE_GLOBAL 新槽），
    /// 并更新 varMap_ 使导入方后续引用解析到新槽位。namespace 由 emitNamespaceImportIR 处理。
    void emitImportSnapshotCopyIR(ImportStmt& node, const std::string& path);

    // 闭包 upvalue 追踪（限制1）
    struct UpvalueInfo {
        uint32_t index;
        bool isLocal;
        int outerIdx;
        std::string name; // 条件断点修复(R75): upvalue变量名
    };
    std::vector<UpvalueInfo> currentUpvalues_;
    std::unordered_map<std::string, int> currentUpvalueNames_;
    std::unordered_map<std::string, int> outerLocalSlots_;
    std::unordered_map<std::string, int> outerUpvalueNames_;
    std::unordered_map<std::string, int> outerFunctions_;
    std::unordered_set<std::string> innerFunctions_;
    std::unordered_map<std::string, int> innerFunctionSlots_;
    // R98 W3: Lambda 表达式合成名计数器（对齐 Compiler::lambdaCounter_）
    int lambdaCounter_ = 0;
    // R103 W1 fix: 记录所有 visitFunDecl 顶层路径处理过的函数名（含主模块+导入模块）。
    // CRITICAL-1 fix 扩展 GLOBAL_SLOT 检查时排除此集合中的名字——FunDecl 顶层路径
    // 的闭包值被 POP 丢弃（全局槽位为 null），调用应走 CALL 命名调用（functionChunks_
    // 查找），不应走 CALL_EXPR 加载 null。仅 var 持闭包值的 GLOBAL_SLOT 走 CALL_EXPR。
    std::unordered_set<std::string> topLevelFunDeclNames_;

    // 循环上下文（break/continue 跳转目标）
    // BUG-EXC-2 fix: tryDepthAtStart 记录循环开始时的 try 嵌套深度，
    // break/continue 时需为差额层级的 try 发射 TRY_END 弹出 handler，
    // 对齐 Compiler.cpp visitBreakStmt 的 tryDepthInLoop 逻辑。
    struct LoopContext {
        uint32_t startLabel;
        uint32_t endLabel;
        uint32_t continueLabel; // continue 目标（for 的 update 块）
        int tryDepthAtStart = 0;
        // AUDIT-P2-CORRECT fix: break 需发射 CLOSE_UPVALUE 关闭循环体内声明的
        // 闭包捕获变量的 upvalue（对齐正常迭代退出时的 leaveBlockScope 发射）。
        // CLOSE_UPVALUE bodySlotBase 关闭 slot >= bodySlotBase 的全部 open upvalues，
        // 含循环体内嵌套块声明的变量（嵌套块 slot >= bodySlotBase）。
        uint32_t bodySlotBase = 0;     // 循环体 block scope slot 基址
        bool needCloseUpvalue = false; // 是否需要关闭 upvalue（inFunction_）
    };
    std::vector<LoopContext> loopStack_;
    int tryDepth_ = 0; // 当前 try 嵌套深度（BUG-EXC-2 fix）
    // AUDIT-P1.1 fix: try-finally 编译期上下文栈（IR 路径，用 label 而非 ip）
    struct TryFinallyContext {
        bool hasFinally = false;
        uint32_t finallyEntryLabel = 0;           // finally 块入口标签
        std::vector<size_t> pendingJumpPatches;   // JUMP 的 label 待回填
        std::vector<size_t> pendingTargetPatches; // PUSH_JUMP_TARGET 的 label 待回填
    };
    std::vector<TryFinallyContext> tryFinallyStack_;
    /// R98 重构：try/catch/finally 三阶段拆分的编译期上下文。
    /// 由 visitTryStmt 创建并传递给 emitTryBlock / emitCatchBlock / emitFinallyBlock。
    /// 字段语义：
    ///   - hasFinally: 是否有 finally 块（决定是否发射外层 TRY_BEGIN/TRY_END 包装）
    ///   - finallyCatchLabel: finally 异常路径入口 label（外层 TRY_BEGIN 的 catch 目标）
    ///   - finallyEndLabel: finally 块统一出口 label
    ///   - finallyEntryLabel: finally 正常路径入口 label（break/continue 续跳目标）
    ///   - endLabel: try-catch 整体结束 label（catch 块后跳转目标）
    ///   - catchLabel: 内层 catch 块入口 label（仅 catchVarName 非空时分配）
    struct TryEmitCtx {
        bool hasFinally = false;
        uint32_t finallyCatchLabel = 0;
        uint32_t finallyEndLabel = 0;
        uint32_t finallyEntryLabel = 0;
        uint32_t endLabel = 0;
        uint32_t catchLabel = 0;
    };
    /// R98 重构：visitFunDecl 三阶段拆分的编译期上下文。
    /// 进入子函数前保存父函数 22 个成员状态（避免 moved-from 状态被复用），
    /// 子函数编译完成或异常时由 restoreParentFunctionState 恢复。
    /// 对齐 Compiler.cpp 的 CompileContextGuard RAII 模式。
    struct FunctionEmitCtx {
        std::unique_ptr<IRFunction> savedIr;
        IRBasicBlock* savedBlock = nullptr;
        std::unordered_map<std::string, VarInfo> savedVarMap;
        std::unordered_map<std::string, std::string> savedVarTypes;
        bool savedInFunction = false;
        uint32_t savedLocalSlot = 0;
        std::vector<std::string> savedLocalSlotNames;
        std::vector<IRFunction::SlotNameRange> savedSlotNameRanges;
        std::vector<LoopContext> savedLoopStack;
        std::vector<BlockScope> savedBlockScopes;
        int savedBlockDepth = 0;
        int savedTryDepth = 0;
        // L4 fix: tryFinallyStack_ 必须跨函数边界保存/恢复，否则嵌套函数（如
        // try-finally 内声明的闭包）的 return 语句会误引用外层函数的 finallyEntryLabel，
        // 导致 patchJumps 找不到 label（finallyEntryLabel 在外层函数 IR 中）→ IR lowering 失败。
        std::vector<TryFinallyContext> savedTryFinallyStack;
        bool savedCompilingMethod = false;
        std::string savedCompilingClassName;
        std::string savedCurrentFunctionReturnType;
        std::vector<std::string> savedCurrentTypeParams;        // R163 泛型扩展
        std::vector<std::string> savedCompilingClassTypeParams; // R163 泛型扩展
        std::unordered_map<std::string, int> savedOuterLocalSlots;
        std::unordered_map<std::string, int> savedOuterUpvalueNames;
        std::unordered_map<std::string, int> savedOuterFunctions;
        std::vector<UpvalueInfo> savedCurrentUpvalues;
        std::unordered_map<std::string, int> savedCurrentUpvalueNames;
        std::unordered_set<std::string> savedInnerFunctions;
        std::unordered_map<std::string, int> savedInnerFunctionSlots;
        // R109 TCO: 父函数 TCO 状态快照（4 字段）。visitFunDecl 入口保存，
        // restoreParentFunctionState 恢复。子函数编译期间这 4 字段被子函数
        // 的 emitFunctionPrologue 覆盖，编译完成后必须复原为父函数状态，
        // 否则父函数后续 return 误判为子函数的 TCO。
        std::string savedCurrentFunctionName;
        const FunDecl* savedCurrentFunctionDecl = nullptr;
        uint32_t savedCurrentFunctionEntryLabel = 0;
        bool savedCurrentFunctionIsMethod = false;
        /// R98 W3 fix: 匿名 lambda 的合成名（$lambda_N）。在 visitFunDecl 入口提前生成
        /// 并递增 lambdaCounter_，避免嵌套 lambda 在 emitFunctionPrologue（读计数器）
        /// 与 emitFunctionClosureRegistration（递增计数器）之间产生重名。
        /// 具名函数此字段为空。
        std::string lambdaName;
    };
    // BUG-IR-SHADOW-SAVE fix: catch 变量遮蔽全局时，原值保存到临时 name-based 全局变量。
    // 不能用 vreg 保存——StackVM 后端的 LOAD_EXCEPTION 是 no-op（异常值已在栈上），
    // LOAD_GLOBAL 再 push 会使 DEFINE_GLOBAL pop 错误值（saved 而非 exception）。
    // 对齐 Compiler.cpp 的 __catch_save_<counter>_<name> 模式。
    uint32_t catchSaveCounter_ = 0;

    // ---- 辅助方法 ----
    IRBasicBlock& newBlock();
    // 注意：方法名用 emitIR 而非 emit，避免与 Qt 的 emit 宏（Q_EMIT）冲突
    void emitIR(IROp op, std::vector<IROperand> operands = {}, int line = 0);
    IROperand emitConst(const Value& v, int line = 0);
    IROperand emitLoadVar(const std::string& name, int line = 0);
    void emitStoreVar(const std::string& name, IROperand val, int line = 0);
    VarInfo resolveVar(const std::string& name);

    /// 2026-06-29: 查找变量类型注解
    const std::string* findVarType(const std::string& name) const {
        auto it = varTypes_.find(name);
        return it != varTypes_.end() ? &it->second : nullptr;
    }
    /// 2026-06-29: 发射 TYPE_CHECK IR（检查 val 是否兼容类型注解）
    void emitTypeCheckIR(IROperand val, const std::string& typeAnnotation, int line);
    uint32_t addUpvalue(const std::string& name);
    void enterBlockScope();
    void leaveBlockScope();
    void preScanTopLevelDecls(Block& program, bool isMainModule = false);

    // CRITICAL-1 fix: 前向自由变量分析（对齐 Interpreter::computeFreeVariables）。
    // 在编译子函数体前，先收集所有自由变量名，为每个能在外层捕获的变量预建 upvalue。
    // 这确保中间函数即使不直接引用某变量，也会捕获它供更内层函数透传。
    std::unordered_set<std::string> computeFreeVars(const class FunDecl& fn);
    void collectFreeVars(const class ASTNode& node, std::vector<std::unordered_set<std::string>>& scopes,
                         std::unordered_set<std::string>& freeVars);
    bool isDefinedInScopes(const std::vector<std::unordered_set<std::string>>& scopes, const std::string& name) const;

    // ---- AST 节点转换 ----
    IROperand visitNode(class ASTNode* node);
    // BUG-IR-POP-2/3 fix: 统一处理语句上下文的表达式语句 POP。
    // 对齐 Compiler::compileStatement 的语义：调用 visitNode 后，
    // 若节点是表达式语句（needsPopForExprStmt 返回 true）则 emit POP 消费结果值。
    // 在所有"语句上下文"（if then/else、while body、for body）使用此方法，
    // 避免手动 POP 遗漏导致栈泄漏/不平衡。
    void visitStatement(class ASTNode* node);
    IROperand visitBinaryOp(class BinaryOp* node);
    // R111 重构：visitBinaryOp 提取 AND/OR 短路求值为两个独立单一职责 helper。
    ///@{
    IROperand emitShortCircuitAnd(class BinaryOp* node);
    IROperand emitShortCircuitOr(class BinaryOp* node);
    ///@}
    // R111 重构：嵌套左值变异写回共享 helper（visitIndexAssign / visitMemberAssign /
    // emitMethodCallWriteback 三处共用）。处理 IndexAccess(VarRef) | MemberAccess(VarRef)
    // 形态的接收者：LOAD_MUTATED + 外层 INDEX_SET/MEMBER_SET + WRITEBACK 链。
    // 返回 true 表示已处理写回（调用方据此 return），false 表示是复杂表达式未处理。
    bool emitNestedAssignWriteback(class ASTNode* object, int line);
    IROperand visitUnaryOp(class UnaryOp* node);
    IROperand visitNumberLiteral(class NumberLiteral* node);
    IROperand visitStringLiteral(class StringLiteral* node);
    IROperand visitBoolLiteral(class BoolLiteral* node);
    IROperand visitNullLiteral(class NullLiteral* node);
    IROperand visitVarRef(class VarRef* node);
    IROperand visitAssignment(class Assignment* node);
    void visitVarDecl(class VarDecl* node);
    void visitIfStmt(class IfStmt* node);
    void visitWhileStmt(class WhileStmt* node);
    void visitForStmt(class ForStmt* node);
    /// R98 W3: 返回 lambda 的 MAKE_CLOSURE dest vreg（供表达式上下文使用）；
    /// 具名函数返回 vreg(0) 哨兵
    IROperand visitFunDecl(class FunDecl* node);
    /// R164 协程/生成器：yield 表达式 → IR YIELD 指令
    IROperand visitYieldExpr(class YieldExpr* node);
    IROperand visitFunCall(class FunCall* node);
    /// R98 W3: 返回 lambda 的 MAKE_CLOSURE dest vreg（供表达式上下文使用）；
    /// 具名函数返回 vreg(0) 哨兵（具名函数不作为表达式求值）
    /// R98 W3 fix: overrideName 非空时用其替代 node.name 作为合成名，
    /// 由 visitFunDecl 入口提前生成（避免嵌套 lambda 重名）。
    IROperand emitFunctionClosureRegistration(class FunDecl& node, const std::string& overrideName = "");
    void visitReturnStmt(class ReturnStmt* node);
    void visitPrintStmt(class PrintStmt* node);
    void visitBlock(class Block* node);
    IROperand visitArrayLiteral(class ArrayLiteral* node);
    IROperand visitDictLiteral(class DictLiteral* node);
    // R98 元组与解构
    IROperand visitTupleLiteral(class TupleLiteral* node);
    void visitDestructureBinding(class DestructureBinding* node);
    // R99 枚举与 ADT + match
    void visitEnumDecl(class EnumDecl* node);
    IROperand visitEnumVariantExpr(class EnumVariantExpr* node);
    IROperand visitMatchExpr(class MatchExpr* node);
    // R110 重构：visitMatchExpr 提取两个单一职责 helper（pattern 检查 + case body 编译）。
    // R134 扩展：emitMatchPattern 现支持 6 种 pattern（WILDCARD/LITERAL/VARIABLE/VARIANT/TUPLE/OR）。
    // emitMatchCaseBody 处理 case body 的 Block 末尾表达式保留值语义（对齐 StackVM Compiler）。
    // 签名变更（R134）：返回 bool hasAnyCheck，接收 std::vector<uint32_t>& failLabels
    // ——failLabels 收集所有"此 case 匹配失败需跳过"的目标 label（OR pattern 会产生多个）。
    // 调用方需在每个 failLabel 处 emit LABEL + POP 残留 check 值。
    ///@{
    bool emitMatchPattern(const MatchPattern& p, uint32_t scrutSlot, int line, std::vector<uint32_t>& failLabels);
    IROperand emitMatchCaseBody(ASTNode* body, int nodeLine);
    ///@}
    IROperand visitIndexAccess(class IndexAccess* node);
    void visitIndexAssign(class IndexAssign* node);
    IROperand visitMemberAccess(class MemberAccess* node);
    void visitMemberAssign(class MemberAssign* node);
    IROperand visitMethodCall(class MethodCall* node);
    // R110 重构：visitMethodCall 提取嵌套接收者变异写回块为单一职责 helper（~60 行后置动作）。
    // 处理三类写回路径：VarRef 接收者 / super 调用 / 嵌套接收者（IndexAccess(VarRef) | MemberAccess(VarRef)）。
    // 逻辑对齐 compiler/Compiler.cpp::emitMethodCallWriteback，IR 路径与非 IR 路径保持镜像结构。
    void emitMethodCallWriteback(class MethodCall* node);
    IROperand visitInterpolatedString(class InterpolatedString* node);
    void visitClassDecl(class ClassDecl* node);
    void visitBreakStmt(class BreakStmt* node);
    void visitContinueStmt(class ContinueStmt* node);
    void visitTryStmt(class TryStmt* node);
    void visitThrowStmt(class ThrowStmt* node);

    /// AUDIT-P1.1 fix: 发射 break/continue 的 finally 续跳 IR。
    /// realTargetLabel 是 break/continue 的真实目标 label（endLabel/continueLabel）。
    /// 返回 true 表示已发射续跳 IR（调用方不应再发射常规 JUMP），false 表示无 finally。
    bool emitFinallyJumpIR(int line, uint32_t realTargetLabel);

    /// R98 重构：try/catch/finally 三阶段拆分（visitTryStmt 的辅助方法）。
    /// 三套 catch 路径（函数内 / 顶层遮蔽保护 / 顶层无遮蔽）在 emitCatchBlock 内部分发，
    /// 每个变体负责发射自己的 finally（如有）并 pop tryFinallyStack_。
    ///@{
    void emitTryBlock(class TryStmt& node, TryEmitCtx& ctx);
    void emitCatchBlock(class TryStmt& node, IROperand excVreg, TryEmitCtx& ctx);
    void emitCatchInFunction(class TryStmt& node, IROperand excVreg, TryEmitCtx& ctx);
    void emitCatchWithShadowSave(class TryStmt& node, IROperand excVreg, int existingSlot, TryEmitCtx& ctx);
    void emitCatchGlobal(class TryStmt& node, IROperand excVreg, TryEmitCtx& ctx);
    void emitFinallyBlock(class TryStmt& node, TryEmitCtx& ctx);
    ///@}

    /// R98 重构：visitFunDecl 三阶段拆分（prologue/body/epilogue + 状态恢复 + 闭包注册）。
    /// 进入子函数前 visitFunDecl 保存父函数 22 个成员状态到 FunctionEmitCtx，
    /// 调用 emitFunctionPrologue/Body/Epilogue 编译子函数，
    /// 最终由 restoreParentFunctionState 恢复父状态、emitFunctionClosureRegistration
    /// 在父 IR 中 emit MAKE_CLOSURE 并注册该函数。
    ///@{
    void emitFunctionPrologue(class FunDecl& node, FunctionEmitCtx& ctx);
    void emitFunctionBody(class FunDecl& node, FunctionEmitCtx& ctx);
    void emitFunctionEpilogue(class FunDecl& node, FunctionEmitCtx& ctx);
    void restoreParentFunctionState(FunctionEmitCtx& ctx);
    // R98 W3: emitFunctionClosureRegistration 已在上方声明（返回 IROperand），
    // 此处不再重复声明，避免返回类型不同导致的重定义错误。
    ///@}
};

// ============================================================
// IRBackend 接口（后端：IR → 目标代码）
// ============================================================

/// IRBackend 抽象接口。
/// 子类实现 IR 到具体后端（VM 字节码 / JS / WASM）的 lowering。
class IRBackend {
public:
    virtual ~IRBackend() = default;
    /// 将 IRFunction lowering 为目标代码。返回成功标志。
    virtual bool lower(const IRFunction& ir) = 0;
};

// ============================================================
// BytecodeIRBackend：IR → VM 字节码 lowering
// ============================================================

/// 将 IRFunction lowering 为 BytecodeChunk。
/// 虚拟寄存器通过栈槽映射：lowering 时维护 vreg→栈深度映射。
/// 支持完整的 IR 指令集到 VM OpCode 的映射，包括写回指令和全局槽位。
class BytecodeIRBackend : public IRBackend {
public:
    BytecodeIRBackend();
    ~BytecodeIRBackend() override = default;

    bool lower(const IRFunction& ir) override;

    /// 取生成的字节码 chunk（lower 成功后有效）
    std::unique_ptr<BytecodeChunk> takeChunk() { return std::move(chunk_); }

    /// lower 整个 IRModule（main + 子函数），返回 CompileResult 兼容的结构
    /// 成功后 takeChunk() 返回 main chunk，takeFunctionChunks() 返回函数 chunks
    bool lowerModule(const IRModule& module); // BUG-NEW: module.globalSlotNames 用于 WRITEBACK_*_VAR slot→name 转换

    /// 取生成的函数 chunks（lowerModule 后有效）
    std::map<std::string, BytecodeChunk> takeFunctionChunks() { return std::move(functionChunks_); }

    /// 方向四：取 main 函数的 IR 指令 → 字节码偏移映射（lower 后有效）
    /// first = IR 指令展平序号（与 IrViewer 的 rowToInstrIndex_ 一致）
    /// second = 该 IR 指令 lowering 后在 main chunk 中的字节码偏移
    /// 用于 VM 单步执行时高亮对应的 IR 指令
    const std::vector<std::pair<size_t, size_t>>& irToBytecodeOffset() const { return irToBytecodeOffset_; }

private:
    std::unique_ptr<BytecodeChunk> chunk_;
    std::map<std::string, BytecodeChunk> functionChunks_;       // 函数名 → chunk
    std::vector<std::pair<size_t, size_t>> irToBytecodeOffset_; // 方向四：IR→字节码偏移映射

    // vreg → 栈深度映射
    std::unordered_map<uint32_t, uint32_t> vregStackDepth_;
    // 标签 → 字节码偏移映射
    std::unordered_map<uint32_t, size_t> labelToOffset_;
    // 待回填跳转
    // BUG-EXC-1 fix: isTryBegin=true 时 patchJumps 写相对偏移（target - (codeOffset+2)），
    // 因为 StackVM OP_TRY_BEGIN 用 `catchIp = ip + 3 + catchOffset` 解码（相对偏移），
    // 而 OP_JUMP/OP_JUMP_IF_FALSE 用 `ip = jump`（绝对偏移）。
    struct PendingJump {
        size_t codeOffset;
        uint32_t targetLabel;
        bool isLoop;
        bool isTryBegin = false;
    };
    std::vector<PendingJump> pendingJumps_;
    // BUG-NEW fix: 全局槽位名表指针（lowerModule 设置，lowerInstruction 中
    // WRITEBACK_*_VAR IMM_UINT 分支用其将 slot→name 转为字符串常量索引）
    const std::vector<std::string>* globalSlotNames_ = nullptr;

    // 辅助
    void emitUint16(std::vector<uint8_t>& code, uint16_t v);
    uint16_t addStringConstant(const std::string& s, const IRFunction& ir);
    // BUG-NEW fix: 将全局槽位号转换为变量名字符串常量索引。
    // 栈式 VM 的 OP_WRITEBACK_*_VAR 把 varIdx 当作常量池索引取 stringVal()，
    // GLOBAL_SLOT 路径需查 globalSlotNames_ 得到变量名再入常量池。
    // BUG-IR-SLOTNAME-1 fix: 返回 bool，失败时不再生成占位名静默产生坏字节码。
    // 调用方需检查返回值，false 时中止 lowering 并向上传递错误。
    bool slotToNameConstant(uint32_t slot, const IRFunction& ir, uint16_t& outIdx);
    bool lowerInstruction(const IRInstruction& instr, const IRFunction& ir);
    bool patchJumps();
    void resetState();

    // ---- lowerInstruction 类别拆分辅助（R98 重构：原 841 行单 switch 拆为 8 个 helper）----
    // 主 lowerInstruction 仅保留 switch 外壳做类别分发，每个 helper 处理一组相关 IROp。
    // 语义零变更：所有边界检查（BUG-IR-BOUND-1）、栈平衡（AUDIT-STACKCLOSURE）、
    // 异常处理（BUG-EXC-1/AUDIT-P1.1）、字段默认值（BUG-INH-1/BUG-INH-IR-1）、
    // 写回 slot→name 转换（BUG-NEW/BUG-IR-SLOTNAME-1）等审计点原样保留至各 helper。
    /// 从 globalNames 取名（索引越界时返回空串）。原 lowerInstruction 中的 lambda，重构为静态方法。
    static std::string globalNameOf(const IRFunction& ir, uint32_t idx);
    /// 常量加载：LOAD_CONST/LOAD_NULL/LOAD_TRUE/LOAD_FALSE
    bool lowerConstOp(const IRInstruction& instr, const IRFunction& ir);
    /// 变量访问：LOAD_LOCAL/STORE_LOCAL/LOAD_GLOBAL/STORE_GLOBAL/DEFINE_GLOBAL/DELETE_VAR/
    /// LOAD_UPVALUE/STORE_UPVALUE/CLOSE_UPVALUE
    bool lowerVarOp(const IRInstruction& instr, const IRFunction& ir);
    /// 算术 + 逻辑：ADD/SUB/MUL/DIV/MOD/NEGATE/NOT（无操作数，单字节 opcode）
    bool lowerArithOp(const IRInstruction& instr, const IRFunction& ir);
    /// 比较：EQ/NEQ/LT/GT/LTE/GTE（无操作数，单字节 opcode）
    bool lowerCompareOp(const IRInstruction& instr, const IRFunction& ir);
    /// 控制流：LABEL/JUMP/JUMP_IF_FALSE（LABEL 不产生字节码，跳转类占位 2B 待 patchJumps 回填）
    bool lowerControlOp(const IRInstruction& instr, const IRFunction& ir);
    /// 调用 + 闭包：CALL/CALL_EXPR/RETURN/RETURN_NULL/MAKE_CLOSURE
    bool lowerCallOp(const IRInstruction& instr, const IRFunction& ir);
    /// 容器 + 成员访问 + 方法调用：BUILD_ARRAY/BUILD_DICT/BUILD_TUPLE/INDEX_GET/INDEX_SET/
    /// MEMBER_GET/MEMBER_SET/SUPER_MEMBER_GET/METHOD_CALL/SUPER_CALL
    /// 主 lowerContainerOp 按 instr.op 分派到下方 3 个子 helper（R106 重构：原 181 行单 switch
    /// 拆为分派器 + 3 个单一职责 helper）。语义零变更：所有边界检查（BUG-IR-BOUND-1）、
    /// 栈深度映射（vregStackDepth_）等审计点原样保留至各 helper。
    bool lowerContainerOp(const IRInstruction& instr, const IRFunction& ir);
    /// lowerContainerOp 子阶段：容器构造与索引 IROp（BUILD_ARRAY/BUILD_DICT/BUILD_TUPLE/
    /// BUILD_ENUM_VARIANT/ENUM_VARIANT_NAME/ENUM_VARIANT_FIELD/INDEX_GET/INDEX_SET）。
    /// 含 count/pairCount/argCount 8 位上限检查（BUG-IR-BOUND-1）。依赖成员：chunk_、
    /// vregStackDepth_、emitUint16。
    bool lowerContainerBuildOps(const IRInstruction& instr, const IRFunction& ir);
    /// lowerContainerOp 子阶段：成员访问 IROp（MEMBER_GET/MEMBER_SET/SUPER_MEMBER_GET）。
    /// emit OP_MEMBER_GET/SET/SUPER_MEMBER_GET + nameConstIdx(2B)。SUPER_MEMBER_GET 与
    /// MEMBER_GET 格式相同，仅 opcode 不同。依赖成员：chunk_、addStringConstant、
    /// globalNameOf、vregStackDepth_。
    bool lowerMemberAccessOps(const IRInstruction& instr, const IRFunction& ir);
    /// lowerContainerOp 子阶段：方法调用 IROp（METHOD_CALL/SUPER_CALL）。
    /// emit OP_METHOD_CALL/SUPER_CALL + nameConstIdx(2B) + argCount(1B) + recvVarIdx(2B)
    /// + recvSlot(1B) [+ classIdx(2B) for SUPER_CALL]。含 argCount 8 位上限检查
    /// （BUG-IR-BOUND-1，含 this 共 255）。依赖成员：chunk_、addStringConstant、
    /// globalNameOf、vregStackDepth_。
    bool lowerMethodCallOps(const IRInstruction& instr, const IRFunction& ir);
    /// 类/异常/写回/杂项：DEFINE_CLASS/CLASS_NEW/INIT_FIELD/TRY_BEGIN/TRY_END/THROW/
    /// LOAD_EXCEPTION/PUSH_JUMP_TARGET/FINALLY_END/WRITEBACK_*/PRINT/POP/DUP/LOAD_MUTATED/TYPE_CHECK
    /// 主 lowerMiscOp 按 instr.op 分派到下方 4 个子 helper（R106 重构：原 324 行单 switch 拆为
    /// 分派器 + 4 个单一职责 helper）。语义零变更：所有边界检查（BUG-IR-BOUND-1）、栈平衡
    /// （AUDIT-STACKCLOSURE）、异常处理（BUG-EXC-1/AUDIT-P1.1）、字段默认值（BUG-INH-1/BUG-INH-IR-1）、
    /// 写回 slot→name 转换（BUG-NEW/BUG-IR-SLOTNAME-1）等审计点原样保留至各 helper。
    bool lowerMiscOp(const IRInstruction& instr, const IRFunction& ir);
    /// lowerMiscOp 子阶段：类相关 IROp（DEFINE_CLASS/CLASS_NEW/INIT_FIELD）
    /// DEFINE_CLASS 处理字段默认值常量 + 字段表达式临时局部变量槽位（BUG-INH-1/BUG-INH-IR-1），
    /// emit OP_CLASS_NEW + 每个 OP_INIT_FIELD + OP_DEFINE_CLASS。依赖成员：chunk_、
    /// addStringConstant、globalNameOf、ir.constants。
    bool lowerClassOps(const IRInstruction& instr, const IRFunction& ir);
    /// lowerMiscOp 子阶段：异常相关 IROp（TRY_BEGIN/TRY_END/THROW/LOAD_EXCEPTION/
    /// PUSH_JUMP_TARGET/FINALLY_END）。TRY_BEGIN 与 PUSH_JUMP_TARGET 注册 pendingJumps_
    /// 待 patchJumps 第二遍回填。LOAD_EXCEPTION 为 no-op（栈式 VM throwException 已推栈）。
    bool lowerExceptionOps(const IRInstruction& instr, const IRFunction& ir);
    /// lowerMiscOp 子阶段：写回指令 IROp（WRITEBACK_MEMBER_VAR/LOCAL/UPVALUE、
    /// WRITEBACK_INDEX_VAR/LOCAL/UPVALUE）。处理 IMM_UINT slot→name 常量索引转换
    /// （BUG-NEW/BUG-IR-SLOTNAME-1）与 slot/uvIdx 255 上限检查（Bug-14 同型修复）。
    bool lowerWritebackOps(const IRInstruction& instr, const IRFunction& ir);
    /// lowerMiscOp 子阶段：杂项 IROp（PRINT/POP/DUP/LOAD_MUTATED/TYPE_CHECK）。
    /// DUP/LOAD_MUTATED/TYPE_CHECK 更新 vregStackDepth_ 栈深度映射。TYPE_CHECK 是 peek
    /// 不 push，但仍记录 src_vreg 在此处"仍然有效"的栈深度（AUDIT-P2 fix）。
    bool lowerMiscPureOps(const IRInstruction& instr, const IRFunction& ir);
};

// ============================================================
// IR 优化 Pass（方向二）
// ============================================================

/// 常量折叠：对常量间的算术/比较/逻辑运算在编译期求值
/// 例如：ADD(LOAD_CONST 1, LOAD_CONST 2) → LOAD_CONST 3
/// 支持：ADD/SUB/MUL/DIV/MOD（int/float）、NEGATE、EQ/NEQ/LT/GT/LTE/GTE、NOT
/// 折叠条件：两个操作数均来自 LOAD_CONST/LOAD_TRUE/LOAD_FALSE/LOAD_NULL 且结果可表示
/// 返回：是否修改了 IR（true 表示有折叠发生）
bool constantFoldingPass(IRFunction& ir);

/// 死代码消除：删除结果从未被使用的纯计算指令
/// 规则：
///   1. 收集所有被引用的 vreg（作为其他指令的操作数）
///   2. 删除 LOAD_CONST/LOAD_LOCAL/LOAD_GLOBAL/ADD/SUB 等结果无引用的指令
///   3. 保留有副作用的指令：STORE_*/CALL/RETURN/PRINT/JUMP/JUMP_IF_FALSE/THROW 等
/// 返回：是否修改了 IR（true 表示有指令被删除）
bool deadCodeEliminationPass(IRFunction& ir);

/// 复制传播：将 vreg = LOAD_* 后续使用替换为直接引用源
/// 规则：
///   1. 记录每个 vreg 的"定义"：LOAD_CONST c → 该 vreg 等价于 constant c
///   2. 后续指令引用该 vreg 时，直接用 constant 替换
///   3. 跨 STORE_LOCAL 后不再传播（局部变量可能被修改）
/// 返回：是否修改了 IR
bool copyPropagationPass(IRFunction& ir);

/// C4: 公共子表达式消除（CSE，局部 — 基本块内）
/// 规则：
///   1. 在每个基本块内维护"值编号"表：hash((op, operand1_kind, operand1_idx, operand2_kind, operand2_idx)) → dest_vreg
///   2. 遇到纯计算指令（EQ/NEQ/LT/GT/LTE/GTE/NOT/DUP/NEGATE/算术）时，若表达式已存在，
///      将本指令 dest vreg 的所有后续引用替换为已存在的 dest vreg，本指令标记为待删除
///   3. 块边界（JUMP/JUMP_IF_FALSE/RETURN/THROW 等）清空值编号表
/// 安全性：
///   - SSA 风格 vreg 不会被重定义，相同 operand 必产生相同结果
///   - 仅在基本块内有效（避免跨块分析复杂度）
///   - 算术指令也参与 CSE（与 BUG-IR-DCE-1 不同：CSE 不删除指令，只替换引用，副作用保留）
///     副作用指令即使 dest 已被替换引用，仍保留原指令执行以触发除零/溢出错误
/// 返回：是否修改了 IR（true 表示有引用被替换）
bool commonSubexpressionEliminationPass(IRFunction& ir);

/// C4: 循环展开（保守策略 — 仅展开常量边界的小循环）
/// 规则：
///   1. 识别模式：LABEL L1; cond_load; LOAD_CONST N; LT; JUMP_IF_FALSE L_exit; POP;
///      <body>; counter_load; LOAD_CONST 1; ADD; STORE_LOCAL slot; JUMP L1; LABEL L_exit; POP
///   2. 当 N 为常量且 1 ≤ N ≤ kMaxUnrollCount(默认 4) 时，展开 N 次循环体
///      （展开后删除原循环结构，直接生成 N 份顺序 body）
///   3. body 必须不包含 break/continue/return/throw（保守安全门）
///   4. body 指令数 ≤ kMaxUnrollBodySize(默认 20)，避免代码膨胀失控
/// 限制：
///   - 仅支持步长为 1 的整数计数 for 循环（最常见模式）
///   - 不支持嵌套循环展开（外层展开后内层不变）
///   - 不支持含 break/continue 的循环（保守跳过）
/// 返回：是否修改了 IR（true 表示有循环被展开）
bool loopUnrollingPass(IRFunction& ir);

/// 运行全部优化 pass（常量折叠 → [复制传播] → [CSE] → [循环展开] → 死代码消除）
/// @param enableCopyPropagation 是否启用复制传播。
///   - 栈式 VM 后端：false（删除 LOAD_CONST 会导致栈下溢）
///   - 寄存器式 VM 后端：true（LOAD_CONST 写寄存器，无引用时 DCE 安全删除）
/// @param enableDCE 是否启用死代码消除。
///   - 栈式 VM 后端：false（POP operands 为空，DCE 看不到 POP 对 vreg 的消费关系，
///     会删除仅被 POP 消费的 LOAD_CONST 等纯计算指令，导致栈式 VM OP_POP 栈下溢）
///   - 寄存器式 VM 后端：true（vreg 物化为寄存器，无引用时安全删除）
///   - BUG-IR-DCE-2 fix: 解耦 DCE 与复制传播控制，避免禁用复制传播时连带禁用 DCE
/// @param enableCSE 是否启用公共子表达式消除（C4 新增）。
///   - 栈式 VM 后端：必须 false（CSE 替换后续指令对 dest vreg 的引用，原 dest 变为
///     死指令但仍 emit，导致栈上残留未被 POP 的值，与 BUG-IR-DCE-1 同源问题）
///   - 寄存器式 VM 后端：可 true（vreg 物化为寄存器，死指令无栈影响，配合 DCE 安全清理）
///   - 默认 false：与 enableDCE 解耦（仅 DCE=true 的寄存器式后端显式传 true 启用）
/// @param enableLoopUnroll 是否启用循环展开（C4 新增）。
///   - 默认 false：循环展开改变代码结构，可能影响调试器行号映射，默认关闭
///   - 性能场景（ProfileDashboardPanel）可显式启用
/// 返回：是否修改了 IR
bool optimizeIR(IRFunction& ir, bool enableCopyPropagation = false, bool enableDCE = false, bool enableCSE = false,
                bool enableLoopUnroll = false);

// ============================================================
// P2-10 IR SSA 基础设施（见 compiler/IRSSA.h）
// ============================================================
// 支配树 / 支配边界 / 自然循环检测 / PHI 节点 / GVN / LICM / 函数内联。
// 详见 compiler/IRSSA.h 的完整接口文档。
/// 全局值编号：基于支配树将 CSE 从基本块内扩展到全局（dest vreg 引用替换）。
/// 仅对寄存器式后端安全（与 CSE 同源约束：栈式后端替换引用后栈残留 → OP_POP 栈下溢）。
bool gvnPass(IRFunction& ir);

/// 循环不变代码外提：将循环体内不依赖循环变量的纯计算提升到循环前置块。
/// 依赖自然循环检测（回边识别）。仅对寄存器式后端安全。
bool licmPass(IRFunction& ir);

/// 函数内联：按成本模型内联小函数（getter / 简单算术）到调用点。
/// 操作 IRModule（需访问被调用函数 IR）。返回是否修改。
bool inlinePass(IRModule& module);

// ============================================================
// IR 打印（调试用）
// ============================================================

/// 将 IRFunction 格式化为可读字符串（用于调试和 IR 可视化）
std::string IRToString(const IRFunction& ir);

/// IR 操作码 → 字符串名称（用于 IR 可视化面板，避免 IrViewer 重复维护一份映射）
/// Dedup-4E: 原本此函数在 IR.cpp 匿名命名空间中，IrViewer.cpp 复制了一份 localIrOpName
/// 因缺失 SUPER_MEMBER_GET/SUPER_CALL case 而产生显示 bug，现统一为公共 API。
const char* irOpName(IROp op);

/// 格式化单条 IR 指令为可读字符串（与 IRToString 中 per-instruction 格式一致）。
/// Dedup-4F: IrViewer.cpp 曾复制此实现，现统一为公共 API，避免格式漂移。
std::string formatIRInstruction(const IRInstruction& instr);
