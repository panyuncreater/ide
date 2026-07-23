#pragma once

// ============================================================
// ErrorMessages.h — 三后端共享的运行时错误消息常量
// ------------------------------------------------------------
// R97 #1 fix: 三后端（Interpreter / StackVM / RegisterVM）原本各自
// 维护错误消息字符串字面量，导致同一语义的错误消息文本不一致
// （如 "数组索引必须是整数" vs "数组索引需要整数类型"）。
// 本头文件集中定义三后端共享的运行时错误消息常量，三后端引用同一
// 常量保证文本一致性。TestThreeEnginesConsistency 中相关 EXPECT_NE
// 测试可改为 EXPECT_EQ 验证一致性。
//
// 使用原则：
// - 仅统一"三后端可能产生但消息文本不一致"的错误消息
// - 栈下溢等内部错误（各后端独有）不在此统一
// - 含动态参数的消息（如 "数组索引越界: %lld"）仍用 ErrorFormat::format
// ============================================================

namespace ErrorMessages {

// 索引访问类（OP_INDEX_GET / REG_INDEX_GET）
inline constexpr const char* kArrayIndexMustBeInt = "数组索引必须是整数";
inline constexpr const char* kStringIndexMustBeInt = "字符串索引必须是整数";
inline constexpr const char* kTypeNotIndexable = "该类型不支持索引访问";

// 索引赋值类（OP_INDEX_SET / OP_INDEX_SET_VAR / OP_INDEX_SET_LOCAL / REG_INDEX_SET）
inline constexpr const char* kTypeNotIndexAssignable = "该类型不支持索引赋值";

// 成员访问类（OP_MEMBER_GET / OP_MEMBER_SET / REG_MEMBER_GET / REG_MEMBER_SET）
// R161 fix: 三后端（含 JIT）统一成员访问/赋值错误消息
inline constexpr const char* kTypeNotMemberAccessible = "该类型不支持成员访问";
inline constexpr const char* kTypeNotMemberAssignable = "该类型不支持成员赋值";

// 字典键类（L4 fix 后三后端统一消息）
inline constexpr const char* kDictKeyInvalidType = "字典键必须是 string/int/bool/float 类型";

// 递归深度类（R97 #11 fix: 三后端消息统一）
// Interpreter 用 MAX_RECURSION_DEPTH=256，StackVM/RegisterVM 用 MAX_FRAMES=256（值相同）。
// 原 Interpreter 报 "递归深度超过限制 (256)"，VM 报 "调用栈溢出"——文本不一致。
// 统一为 "递归深度超过限制 (N)"，N 由 ErrorFormat::format(kRecursionDepthExceededFmt, limit) 注入。
inline constexpr const char* kRecursionDepthExceededFmt = "递归深度超过限制 (%d)";

// super 上下文类（R97 #11 fix: 三后端消息统一）
// 顶层 / 普通函数内使用 super 时，Interpreter 报 "super 只能在类方法中使用"，
// VM/IR 路径原报 "未定义的变量: this"（emitLoadVar 回退到 GLOBAL_NAME 查找失败）。
// 统一方案：VM::OP_GET_VAR / RegisterVM::REG_LOAD_GLOBAL 在 name=="this" 查找失败时
// 报 super 专用消息（而非 "未定义的变量: this"），与 Interpreter 消息一致。
inline constexpr const char* kSuperOutsideMethod = "super 只能在类方法中使用";

// 未定义函数调用类（R164 fix: 三后端消息统一，由 fuzz mutate 模式发现）
// Interpreter 原报 "X 不是函数，无法调用"（calleePtr 为 null 或非闭包时），
// StackVM/RegisterVM 统一报 "未定义的函数: X"。统一为后者——
// 三后端一致性优先于 Interpreter 的语义细分（VM 端不区分"未定义"与"非函数"）。
// 上下文可通过 runtimeError 的 line/col 定位。
inline constexpr const char* kUndefinedFunctionFmt = "未定义的函数: %s";

// 类型注解违反类（R164 fix: 三后端消息统一，由 fuzz mutate 模式发现）
// Interpreter 原报 "<context> 期望类型 X，实际为 Y"（contextBuilder 生成上下文前缀如"变量 s 的类型"），
// StackVM/RegisterVM 统一报 "类型注解违反: 期望类型 X，实际为 Y"。统一为后者——
// 历史决策（archive changelog round18 Bug#3）曾标记"暂不修"（理由：需扩展 OP_TYPE_CHECK
// 指令编码增加上下文字符串常量索引），但 fuzz 差分测试将其暴露为一致性障碍。
// 修复方向：Interpreter 放弃 contextBuilder 前缀对齐 VM（改动小），而非 VM 扩展指令编码（改动大）。
// contextBuilder 参数保留（避免改调用方签名），但不用于错误消息。
inline constexpr const char* kTypeAnnotationViolationFmt = "类型注解违反: 期望类型 %s，实际为 %s";

} // namespace ErrorMessages
