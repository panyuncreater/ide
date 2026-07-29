/**
 * @file common/BackendExecutionService.h
 * @brief 后端执行服务中间层（ARCH-10）。
 *
 * 为 GUI 教学面板（BackendParallelPanel / PerformanceRacePanel /
 * ProfileDashboardPanel 等）提供对编译器+执行后端的高层封装，
 * 消除面板对 compiler/Compiler.h / compiler/VM.h / compiler/RegisterVM.h /
 * compiler/IR.h / interpreter/Interpreter.h / lexer/Lexer.h / parser/Parser.h
 * 等内部头文件的直接依赖。
 *
 * 设计原则：
 *   - 面板通过 BackendType 枚举选择后端，服务负责完整的
 *     Lexer → Parser → Compiler → Backend 执行流程
 *   - 返回 BackendExecResult 结构体，包含输出/错误/耗时/指令数等只读数据
 *   - 错误处理统一：词法/语法/编译/运行时错误均通过 errorMsg + success 字段返回
 *   - 不持有状态：每次调用独立执行，面板无需管理对象生命周期
 *
 * 与 IBackend 的关系：
 *   - IBackend 抽象执行后端的调试/诊断接口（断点/单步/栈检查）
 *   - BackendExecutionService 抽象"源码 → 执行结果"的完整流程
 *   - 两者互补：面板用 BackendExecutionService 跑代码，用 IBackend 调试
 *
 * @since ARCH-10
 */
#pragma once

#include <array>
#include <cstdint>
#include <memory> // std::unique_ptr（createBackend 返回值）
#include <string>
#include <vector>

// 前向声明 IBackend，避免在此公共头文件中 #include "common/IBackend.h"
// （后者会传递引入 interpreter/Value.h 等重依赖）。调用方持有 createBackend
// 返回的 unique_ptr<IVmBackend> 时必然已 include IBackend.h。
class IVmBackend;

// ============================================================
// 后端类型枚举
// ============================================================
// 面板通过此枚举选择执行后端，无需引用具体 VM/Interpreter 类。
enum class BackendType : uint8_t {
    Interpreter,   // 树遍历解释器（直接执行 AST，无字节码）
    StackVM_IR,    // 栈式 VM（IR 路径：Compiler setUseIR(true) → VM::execute）
    RegisterVM_IR, // 寄存器式 VM（Compiler setUseRegisterVM(true) → RegisterVM::execute）
};

/// 后端类型转显示文本（"Interpreter" / "StackVM (IR)" / "RegisterVM (IR)"）
/// 公开工具函数，供面板渲染时使用。
const char* backendTypeToString(BackendType type);

/// 后端类型转简短标识（"interp" / "stackvm" / "regvm"），用于日志/列标识。
const char* backendTypeToShortName(BackendType type);

// ============================================================
// 执行结果
// ============================================================

/// 单次后端执行的完整结果（只读快照）。
/// 所有字段为值类型或 std::string，调用方无需依赖任何编译器/VM 内部类型。
struct BackendExecResult {
    bool success = false;                           // 是否成功（无词法/语法/编译/运行时错误）
    BackendType backend = BackendType::Interpreter; // 实际使用的后端
    std::string output;                             // 标准输出（print 累积）
    std::string errorMsg;                           // 错误信息（success=false 时有效，含错误前缀）
    std::string errorPrefix;                        // 错误前缀（"词法错误" / "语法错误" / "编译错误" / "运行时错误"）
    int64_t elapsedMs = 0;                          // 执行耗时（毫秒，= elapsedUs/1000，保留兼容旧调用方）
    // AUDIT-R2 P2-2 fix: 新增微秒精度耗时——教学题库代码多在 1ms 内完成，
    // 毫秒粒度下三后端耗时恒显 0，面板对比失去意义。
    // 注：VM/RegisterVM 路径的耗时仅覆盖执行阶段（不含编译），与 Interpreter
    // 路径口径一致（AUDIT-R2 P2-3 fix，此前 VM 路径含编译时间，对比不对称）。
    int64_t elapsedUs = 0;                // 执行耗时（微秒）
    int64_t instrCount = -1;              // 指令数（字节码字节数；-1 表示 N/A，如 Interpreter 无字节码）
    std::vector<std::string> diagnostics; // 编译诊断信息（warning/error 文本列表）

    /// 便捷访问：指令数显示文本（"N/A" 或数字字符串）
    std::string instrCountText() const;

    /// 便捷访问：状态文本（"✅ 成功" 或 "❌ <errorPrefix>: <errorMsg>"）
    std::string statusText() const;

    /// 便捷访问：编译阶段描述（"✅ 编译成功（IR 路径，字节码 N 字节）" 等）
    std::string compileStageText() const;
};

// ============================================================
// 生成器 chunk 信息（用于协程/生成器可视化）
// ============================================================
// 抽象 BytecodeChunk / RegBytecodeChunk 的 isGenerator/yieldCount 字段，
// 供 CoroutineVisualizerPanel 等教学面板展示生成器 chunk 概要，
// 无需面板直接依赖 compiler/Bytecode.h / compiler/RegisterBytecode.h。
struct GeneratorChunkInfo {
    std::string name;       // chunk 名（函数名）
    int yieldCount = 0;     // 静态 yield 数（动态时为 -1 标记）
    bool isDynamic = false; // 是否为 kDynamicYieldCount（INT_MAX）
};

// ============================================================
// BackendExecDetail — 扩展执行结果
// ============================================================
// 在 BackendExecResult 基础上增加：
//   - generatorChunks：生成器 chunk 信息（仅 StackVM/RegisterVM 路径有效）
//   - opcodeCounts：256 槽 opcode 频次计数（仅 StackVM/RegisterVM 路径有效；
//     Interpreter 路径无 opcode 概念，全为 0）
//   - peakTrackedCount：执行期间 GcManager 跟踪对象相对本次执行前基线的
//     净增峰值（AUDIT-R2 P1-5 fix：不再 reset 全局单例，避免破坏并发执行的
//     worker 线程 GC 跟踪状态）
struct BackendExecDetail : BackendExecResult {
    std::vector<GeneratorChunkInfo> generatorChunks;
    std::array<uint64_t, 256> opcodeCounts{};
    size_t peakTrackedCount = 0;
};

// ============================================================
// BackendExecutionService — 后端执行服务
// ============================================================
// 所有方法均为静态，无实例状态。Lexer/Parser/Compiler/VM/Interpreter 的
// 对象创建与执行封装在 .cpp 中，调用方仅依赖此头文件的纯数据结构。
//
// 线程安全：
//   - 单次 execute 调用在调用线程内同步执行（不创建新线程）
//   - 不同线程并发调用 execute 是安全的（每次调用创建独立的局部对象）
//   - 但执行期间会访问 GcManager 全局单例（与 worker 线程 collectCycle 互斥）
class BackendExecutionService {
public:
    /// 完整执行入口：Lexer → Parser → Compiler → 指定后端执行。
    /// @param src MiniLang 源码
    /// @param backend 后端类型（Interpreter / StackVM_IR / RegisterVM_IR）
    /// @return 执行结果（含输出/错误/耗时/指令数）
    static BackendExecResult execute(const std::string& src, BackendType backend);

    /// 扩展执行入口：返回详细执行信息（含生成器 chunk / opcode 计数 / GC 峰值）。
    /// 用于 CoroutineVisualizerPanel（生成器 chunk 信息）和
    /// ProfileDashboardPanel（opcode profile + GC peak tracked）。
    /// @param src MiniLang 源码
    /// @param backend 后端类型
    /// @return 执行详情（基类字段同 BackendExecResult，扩展字段见 BackendExecDetail）
    static BackendExecDetail executeWithDetail(const std::string& src, BackendType backend);

    /// ARCH-10 IVmBackend 工厂方法：创建指定类型的后端实例，返回多态指针。
    /// 用于教学面板需要通过 IVmBackend* 多态操作 VM 的场景（如 BytecodeTracePanel
    /// 需要步进执行 + 读取栈/寄存器/调用栈，通过统一接口避免 if(backend==StackVM)
    /// 静态分派）。面板持有 unique_ptr 自动管理生命周期，无需依赖具体 VM 类。
    /// @param backend 后端类型（仅 StackVM_IR / RegisterVM_IR 有效；Interpreter
    ///                不实现 IVmBackend，传入会返回 nullptr）
    /// @return 后端实例的 unique_ptr，或 nullptr（backend == Interpreter）。
    ///         本工厂不执行任何编译，不存在"编译失败"返回路径。
    /// 注意：调用方负责在创建后调用 IVmBackend 的子类特有方法（如 VM::execute
    ///       或 RegisterVM::execute）开始执行；本工厂仅创建对象，不执行代码。
    static std::unique_ptr<IVmBackend> createBackend(BackendType backend);
};
