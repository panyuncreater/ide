/**
 * @file common/RuntimeLimits.h
 * @brief 统一运行时限制常量。
 *
 * 消除 17 个限制常量散布在 7 个文件（Interpreter.h / VM.h / Parser.h /
 * Lexer.h / Formatter.h / Value.h / Compiler.h）的重复定义问题。
 * 所有模块共享同一组常量，避免对齐遗漏（如 MAX_INHERITANCE_DEPTH
 * 曾重复定义在 Interpreter.h 和 VM.h 两处）。
 *
 * 设计原则：
 *   - 所有常量为 constexpr，可供编译期常量折叠
 *   - 仅依赖 <cstdint> 和 <cstddef>，无循环依赖风险
 *   - 常量值与原各自定义保持一致，纯重构无行为变更
 *
 * 限制分类：
 *   - 递归/调用栈深度：MAX_RECURSION_DEPTH / MAX_FRAMES / MAX_STACK_SIZE
 *   - 继承链：MAX_INHERITANCE_DEPTH
 *   - DoS 防护：MAX_LOOP_ITERATIONS / MAX_INSTRUCTIONS / MAX_RANGE
 *   - 编译期深度：MAX_PARSE_DEPTH / MAX_BLOCK_DEPTH / MAX_COMPILE_DEPTH / MAX_FORMAT_DEPTH
 *   - 序列化/比较：MAX_TOSTRING_DEPTH / MAX_EQUALS_DEPTH / MAX_CLONE_DEPTH
 *   - Lexer 输入：MAX_SOURCE_SIZE / MAX_TOKEN_COUNT / MAX_INTERP_DEPTH
 *   - GUI 查找/替换：MAX_FIND_HIGHLIGHTS / MAX_REPLACE_ALL
 *   - JIT 调用参数：MAX_JIT_ARGS
 *   - 字节码哨兵值：NO_INDEX / NO_SLOT
 *
 * @see Interpreter VM Parser Lexer Formatter Value
 */
#pragma once

// ============================================================
// RuntimeLimits.h — 统一运行时限制常量
// ------------------------------------------------------------
// 消除 17 个限制常量散布在 7 个文件的重复定义问题。
// 所有模块（Lexer/Parser/Interpreter/Compiler/VM/Formatter/Value）
// 共享同一组常量，避免对齐遗漏（如 MAX_INHERITANCE_DEPTH 曾重复
// 定义在 Interpreter.h 和 VM.h 两处）。
//
// 设计原则：
//   1. 所有常量为 constexpr，可供编译期常量折叠
//   2. 仅依赖 <cstdint> 和 <cstddef>，无循环依赖风险
//   3. 常量值与原各自定义保持一致，纯重构无行为变更
//
// L7 fix（2026-07-19）：引入 RuntimeConfig 运行时配置类。
//   - 原审计结论（R93 BUG-020/021）认为可配置化与 constexpr 设计原则冲突，
//     但用户要求修复此已知限制。折中方案：保留 constexpr 作为编译期默认值，
//     另引入 RuntimeConfig 单例允许 IDE 在运行时覆盖部分 DoS 防护限制。
//   - 仅 MAX_INSTRUCTIONS / MAX_PARSE_ERRORS / MAX_LOOP_ITERATIONS 三个
//     DoS 防护常量支持运行时覆盖（教学场景下可能需要调整）。
//   - 其他常量（递归深度、序列化深度等）保持 constexpr，因为它们关系到
//     栈空间分配和编译期优化，运行时修改可能导致栈溢出。
// ============================================================

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace RuntimeLimits {

// ---- 递归 / 调用栈深度 ----
// Interpreter 树遍历递归深度上限（与 VM MAX_FRAMES 对齐）
constexpr int MAX_RECURSION_DEPTH = 256;
// VM 调用帧最大深度（与 Interpreter MAX_RECURSION_DEPTH 对齐）
constexpr size_t MAX_FRAMES = 256;
// VM 操作数栈最大深度
constexpr size_t MAX_STACK_SIZE = 1024;

// ---- 继承链 ----
// 类继承链最大深度（Interpreter 和 VM 共享，避免重复定义）
constexpr int MAX_INHERITANCE_DEPTH = 64;

// ---- DoS 防护 ----
// Interpreter 循环迭代次数上限（约 0.3 秒 CPU 时间）
constexpr int64_t MAX_LOOP_ITERATIONS = 10000000;
// VM 指令执行总预算（≤1000 万次循环迭代，约 0.5 秒）
constexpr int64_t MAX_INSTRUCTIONS = 40000000;
// range() 函数参数上限（与 MAX_LOOP_ITERATIONS 对齐）
constexpr int64_t MAX_RANGE = 10000000;
// AUDIT-P1.2 fix: 条件断点求值步数上限。条件断点求值同步阻塞主线程，
// 需比 MAX_LOOP_ITERATIONS 严格 100 倍。100000 步在简单条件表达式下为毫秒级，
// 足以覆盖合理条件（如 i > 10 && arr.len() > 5），同时截断 while(true){} 等无限循环。
constexpr size_t MAX_CONDITION_STEPS = 100000;
// channel.recvTimeout(ms) 单次等待上限（毫秒）。spawn 为延迟执行模式，
// recvTimeout 阻塞期间不会有其他线程 send，过大的超时等价于卡死 worker；
// 60s 上限阻断恶意/误写的超长阻塞，同时覆盖合理的教学演示场景。
constexpr int64_t MAX_CHANNEL_TIMEOUT_MS = 60000;

// ---- 编译期深度保护 ----
// Parser 最大递归深度
// AUDIT-P0 fix: 从 512 降至 256。原值 512 在 Debug 模式下会先栈溢出：
// 每层括号递归经过 expression→assignment→or_→and_→equality→comparison→
// term→factor→unary→call→primary 共 11 个栈帧，但只有 expression/assignment
// 各 +1 depth。512/2=256 层括号 × 11 帧 × ~3KB ≈ 8MB，远超 1MB 默认栈。
// 256 在 Release 1MB 栈下安全（128 层 × 11 × 500B ≈ 700KB），在 Debug
// 8MB 栈下也安全（128 层 × 11 × 3KB ≈ 4.2MB）。
constexpr int MAX_PARSE_DEPTH = 256;
// Parser 最大块嵌套深度
constexpr int MAX_BLOCK_DEPTH = 256;
// BUG-PARSER-AUDIT-5 fix: Parser 错误数量上限。
// 原实现无上限，恶意输入（如 100 万个 ';'）可触发 O(N) 诊断内存膨胀（~200MB）。
// 主流编译器（gcc/clang/MSVC）均在错误数超阈值后停止解析。100 与 MSVC 默认一致。
constexpr int MAX_PARSE_ERRORS = 100;
// Compiler 最大编译嵌套深度
constexpr int MAX_COMPILE_DEPTH = 512;
// Formatter 最大格式化嵌套深度
constexpr int MAX_FORMAT_DEPTH = 256;
// 七特性 MVP 阶段 2：宏展开克隆递归深度上限。
// 宏 body 是单个表达式（受 MAX_PARSE_DEPTH 约束），嵌套宏调用在 body 解析时
// 已展开为子树，克隆深度≈展开后 AST 深度。与 MAX_PARSE_DEPTH 对齐。
constexpr int MAX_MACRO_EXPANSION_DEPTH = 256;

// ---- Value 序列化 / 比较 ----
// toString 递归深度上限（对齐 Formatter MAX_FORMAT_DEPTH）
constexpr int MAX_TOSTRING_DEPTH = 256;
// equals 递归深度上限
constexpr int MAX_EQUALS_DEPTH = 256;
// clone 递归深度上限（防止深嵌套/环形结构栈溢出）
constexpr int MAX_CLONE_DEPTH = 256;

// ---- Lexer 输入限制 ----
// 源代码最大大小（10MB）
constexpr size_t MAX_SOURCE_SIZE = 10 * 1024 * 1024;
// 最大 Token 数量（100万）
constexpr size_t MAX_TOKEN_COUNT = 1000000;
// 字符串插值最大嵌套深度
constexpr int MAX_INTERP_DEPTH = 64;

// ---- GUI 查找/替换限制 ----
// D14 fix: 统一 GUI 查找/替换上限到 RuntimeLimits，避免硬编码散布
// 高亮匹配上限（防止超大文档 UI 卡死）
constexpr int MAX_FIND_HIGHLIGHTS = 1000;
// 全部替换上限（防止超大文档 UI 卡死）
constexpr int MAX_REPLACE_ALL = 100000;

// ---- JIT 调用参数上限 ----
// P3-15 fix: JIT 运行时辅助函数（jitCallExpr/jitCallMethod/jitCallByName）从
// 操作数栈 pop 参数到本地缓冲区，argCount 编码为 uint8_t（上限 255），
// 256 是覆盖全部合法 argCount 的最小 2 的幂。原 JIT.cpp 4 处硬编码 Value args[256]，
// 现统一为 std::array<Value, MAX_JIT_ARGS>，便于静态边界检查与 clang-tidy 审计。
constexpr size_t MAX_JIT_ARGS = 256;
// P3-15 fix: JIT 默认参数填充缓冲区上限。默认参数是参数的子集，
// 64 足够覆盖实际场景（函数参数总数上限 MAX_JIT_ARGS=256，但默认参数数量
// 远小于此）。原 JIT.cpp 4 处硬编码 Value defaults[64]，现统一为
// std::array<Value, MAX_DEFAULT_PARAMS>，便于静态边界检查。
constexpr size_t MAX_DEFAULT_PARAMS = 64;

// ---- 字节码哨兵值（"无"标记）----
// P3-14 fix: 提取散布在 Compiler/VM/Bytecode/IR/JIT/RegisterVM 共 40+ 处的
// 魔法数字哨兵。0xFFFF 表示 uint16 槽位/常量索引"无"，0xFF 表示 uint8
// receiverLocalSlot"无槽位"。原裸数字既无语义又难以一致维护。
constexpr uint16_t NO_INDEX = 0xFFFF; ///< uint16 常量池/变量索引哨兵：无索引
constexpr uint8_t NO_SLOT = 0xFF;     ///< uint8 receiverLocalSlot 哨兵：无槽位

// ============================================================
// L7 fix: RuntimeConfig — 运行时可配置的 DoS 防护限制
// ------------------------------------------------------------
// 允许 IDE 在运行时覆盖部分 DoS 防护常量（教学场景下可能需要调整）。
// 设计要点：
//   1. 单例模式，线程安全（std::atomic 存储）
//   2. 默认值与上方 constexpr 保持一致
//   3. 仅暴露三个 DoS 防护常量，其他常量保持 constexpr 不变
//   4. 调用方应使用 RuntimeConfig::maxInstructions() 等方法，
//      而非直接引用 RuntimeLimits::MAX_INSTRUCTIONS
//   5. 测试场景可通过 setter 临时调小验证触发逻辑
// ============================================================
class RuntimeConfig {
public:
    static RuntimeConfig& instance() {
        static RuntimeConfig cfg;
        return cfg;
    }

    // DoS 防护限制（运行时可配置）
    int64_t maxInstructions() const { return maxInstructions_.load(std::memory_order_relaxed); }
    int maxParseErrors() const { return maxParseErrors_.load(std::memory_order_relaxed); }
    int64_t maxLoopIterations() const { return maxLoopIterations_.load(std::memory_order_relaxed); }

    void setMaxInstructions(int64_t v) { maxInstructions_.store(v, std::memory_order_relaxed); }
    void setMaxParseErrors(int v) { maxParseErrors_.store(v, std::memory_order_relaxed); }
    void setMaxLoopIterations(int64_t v) { maxLoopIterations_.store(v, std::memory_order_relaxed); }

    // 重置为编译期默认值（测试 tearDown 调用避免污染后续测试）
    void resetToDefaults() {
        maxInstructions_.store(MAX_INSTRUCTIONS, std::memory_order_relaxed);
        maxParseErrors_.store(MAX_PARSE_ERRORS, std::memory_order_relaxed);
        maxLoopIterations_.store(MAX_LOOP_ITERATIONS, std::memory_order_relaxed);
    }

private:
    RuntimeConfig()
        : maxInstructions_(MAX_INSTRUCTIONS), maxParseErrors_(MAX_PARSE_ERRORS),
          maxLoopIterations_(MAX_LOOP_ITERATIONS) {}
    RuntimeConfig(const RuntimeConfig&) = delete;
    RuntimeConfig& operator=(const RuntimeConfig&) = delete;

    std::atomic<int64_t> maxInstructions_;
    std::atomic<int> maxParseErrors_;
    std::atomic<int64_t> maxLoopIterations_;
};

} // namespace RuntimeLimits
