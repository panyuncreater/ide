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
 *   - 不依赖任何标准库头文件，无循环依赖风险
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
//   2. 不依赖任何标准库头文件，无循环依赖风险
//   3. 常量值与原各自定义保持一致，纯重构无行为变更
// ============================================================

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
// Compiler 最大编译嵌套深度
constexpr int MAX_COMPILE_DEPTH = 512;
// Formatter 最大格式化嵌套深度
constexpr int MAX_FORMAT_DEPTH = 256;

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

} // namespace RuntimeLimits
