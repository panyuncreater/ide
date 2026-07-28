/* ============================================================
 * minilang_capi.h — libminilang 嵌入式 C API（拓展二期·平台）
 * ------------------------------------------------------------
 * 面向宿主应用嵌入场景的稳定 C ABI 接口：C/Python(ctypes)/
 * Rust(bindgen)/Go(cgo) 等任何支持 C FFI 的语言都可调用。
 *
 * MVP 范围（阶段 1）：
 *   - 上下文创建/销毁（不透明句柄，内部无全局状态）
 *   - 三后端选择（Interpreter / StackVM(IR) / RegisterVM(IR)）
 *   - 源码求值 + 捕获 print 输出 / 错误消息
 *   - 版本查询
 * 后续阶段（未实现）：宿主函数注册回调、Value 级别互操作、
 * 沙箱资源限额配置。
 *
 * 线程模型：一个 minilang_context 只能被一个线程使用；
 * 不同线程各自持有独立 context 并发调用是安全的
 * （eval 内部每次执行创建独立引擎对象，无跨 context 共享可变态）。
 *
 * 字符串生命周期：minilang_get_output / minilang_get_error /
 * minilang_version 返回的指针由 context（或静态存储）持有，
 * 在下一次对同一 context 调用 minilang_eval 或 minilang_destroy
 * 之前有效；调用方不得 free。
 *
 * 链接说明：本库以 minilang_capi 静态库形式构建，依赖
 * minilang_core（含 Qt6::Core）。嵌入方需同时链接两者
 * （Qt 依赖将在后续阶段通过核心层去 Qt 化移除）。
 * ============================================================ */
#ifndef MINILANG_CAPI_H
#define MINILANG_CAPI_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 不透明上下文句柄 ---- */
typedef struct minilang_context minilang_context;

/* ---- 后端枚举（与内部 BackendType 对齐） ---- */
typedef enum minilang_backend {
    MINILANG_BACKEND_INTERPRETER = 0, /* 树遍历解释器 */
    MINILANG_BACKEND_STACKVM = 1,     /* 栈式 VM（IR 路径） */
    MINILANG_BACKEND_REGISTERVM = 2   /* 寄存器式 VM（IR 路径） */
} minilang_backend;

/* ---- 求值结果状态码 ---- */
typedef enum minilang_status {
    MINILANG_OK = 0,            /* 成功 */
    MINILANG_ERR_COMPILE = 1,   /* 词法/语法/编译错误 */
    MINILANG_ERR_RUNTIME = 2,   /* 运行时错误 */
    MINILANG_ERR_INVALID = 3    /* 参数非法（NULL ctx/source 等） */
} minilang_status;

/* 创建执行上下文（默认后端 Interpreter）。失败返回 NULL。 */
minilang_context* minilang_create(void);

/* 销毁上下文（NULL 安全）。 */
void minilang_destroy(minilang_context* ctx);

/* 选择执行后端。非法枚举值返回 MINILANG_ERR_INVALID。 */
minilang_status minilang_set_backend(minilang_context* ctx, minilang_backend backend);

/* 求值 UTF-8 源码。返回状态码；输出/错误经 getter 读取。
 * 每次调用覆盖上一次的输出与错误缓冲。 */
minilang_status minilang_eval(minilang_context* ctx, const char* source);

/* 上次 eval 的 print 输出（UTF-8，可能为空串；ctx 为 NULL 返回 ""）。 */
const char* minilang_get_output(const minilang_context* ctx);

/* 上次 eval 的错误消息（含"语法错误:"等前缀；成功时为空串）。 */
const char* minilang_get_error(const minilang_context* ctx);

/* 当前后端。ctx 为 NULL 返回 MINILANG_BACKEND_INTERPRETER。 */
minilang_backend minilang_get_backend(const minilang_context* ctx);

/* 库版本字符串（静态存储，如 "libminilang 0.1.0"）。 */
const char* minilang_version(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MINILANG_CAPI_H */
