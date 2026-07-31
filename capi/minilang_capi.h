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
 * 七特性 MVP 阶段 6/7 扩展：沙箱模式开关（minilang_set_sandbox）、
 * 宿主函数注册（minilang_register_function）、插件加载（minilang_load_plugin）。
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

/* 七特性 MVP 阶段 6：启用/禁用沙箱模式（能力级限制）。
 * enabled != 0 时，后续 minilang_eval 在沙箱下执行：禁止 input 读取、
 * 禁止导入文件模块（std/ 内建模块仍放行）、禁止插件加载。
 * 标志存于 context，eval 前设置全局 RuntimeConfig、eval 后恢复（不污染其他 context）。
 * ctx 为 NULL 返回 MINILANG_ERR_INVALID。 */
minilang_status minilang_set_sandbox(minilang_context* ctx, int enabled);

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

/* ============================================================
 * 七特性 MVP 阶段 7：插件系统（宿主函数注册 + DLL 加载）
 * ============================================================ */

/* ---- 宿主互操作值（tagged union，仅 5 种标量） ---- */
typedef enum minilang_value_type {
    MINILANG_VAL_NULL = 0,
    MINILANG_VAL_BOOL = 1,
    MINILANG_VAL_INT = 2,
    MINILANG_VAL_DOUBLE = 3,
    MINILANG_VAL_STRING = 4 /* utf8 c-string；返回值时由宿主保证在回调返回前有效 */
} minilang_value_type;

typedef struct minilang_value {
    minilang_value_type type;
    int b;             /* MINILANG_VAL_BOOL：0/非0 */
    long long i;       /* MINILANG_VAL_INT */
    double d;          /* MINILANG_VAL_DOUBLE */
    const char* s;     /* MINILANG_VAL_STRING：utf8 c-string */
} minilang_value;

/* 宿主函数指针类型：接收标量参数数组，返回标量结果。
 * userdata 为注册时提供的上下文指针，回调时原样传回。
 * 返回的 minilang_value 若为 STRING，其 s 指针需在回调返回后仍有效
 * （建议指向静态/宿主持有的缓冲，或字面量）。 */
typedef minilang_value (*minilang_host_fn)(const minilang_value* args, int argc, void* userdata);

/* 注册宿主函数，使 MiniLang 源码可用 name(...) 调用（三后端一致）。
 * 同名覆盖。全局注册表（跨 context 共享），注册在 eval 前完成。
 * ctx/name/fn 任一为 NULL 返回 MINILANG_ERR_INVALID。 */
minilang_status minilang_register_function(minilang_context* ctx, const char* name, minilang_host_fn fn,
                                           void* userdata);

/* 插件注册回调类型：与 minilang_register_function 签名一致。
 * 加载方将此函数指针传给插件，使插件无需在链接期依赖 minilang 符号
 * （Windows DLL 需在链接期解析全部外部符号，因此采用回调表 ABI）。 */
typedef minilang_status (*minilang_register_fn)(minilang_context* ctx, const char* name, minilang_host_fn fn,
                                                void* userdata);

/* 加载插件动态库（Windows .dll / *nix .so）。
 * 插件须导出 `void minilang_plugin_init(minilang_context*, minilang_register_fn)`，
 * 加载后被调用，插件用传入的注册回调注册能力（避免插件链接期依赖 minilang 符号）。
 * 沙箱模式（minilang_set_sandbox 启用）下拒绝加载，返回 MINILANG_ERR_INVALID。
 * 加载失败（找不到库/找不到入口）返回 MINILANG_ERR_INVALID，错误经 get_error 读取。
 * 成功返回 MINILANG_OK。加载的库句柄由 context 持有，minilang_destroy 时释放。 */
minilang_status minilang_load_plugin(minilang_context* ctx, const char* path);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MINILANG_CAPI_H */
