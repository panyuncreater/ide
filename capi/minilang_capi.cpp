// ============================================================
// minilang_capi.cpp — libminilang 嵌入式 C API 实现（拓展二期·平台）
// ------------------------------------------------------------
// 薄封装层：把 C ABI 映射到 BackendExecutionService::execute
// （ARCH-10 统一执行中间层，与 GUI 教学面板共用同一条
// Lexer → Parser → Compiler → Backend 流程，语义天然一致）。
//
// 设计要点：
//   - minilang_context 内部只存后端选择 + 上次执行的输出/错误快照，
//     不持有引擎对象（每次 eval 由服务层创建独立局部对象）
//   - 错误分类：BackendExecResult.errorPrefix 含"运行时错误"→
//     MINILANG_ERR_RUNTIME，其余（词法/语法/编译）→ MINILANG_ERR_COMPILE
//   - 所有入口 NULL 安全（返回 MINILANG_ERR_INVALID / 空串）
// ============================================================

#include "capi/minilang_capi.h"

#include "common/BackendExecutionService.h"

#include <string>

/// 不透明句柄的真实定义（仅本 TU 可见完整布局）
struct minilang_context {
    BackendType backend = BackendType::Interpreter;
    std::string lastOutput;
    std::string lastError;
};

extern "C" {

minilang_context* minilang_create(void) {
    // 嵌入方无异常语义约定：分配失败返回 NULL 而非抛 bad_alloc
    return new (std::nothrow) minilang_context();
}

void minilang_destroy(minilang_context* ctx) {
    delete ctx;
}

minilang_status minilang_set_backend(minilang_context* ctx, minilang_backend backend) {
    if (!ctx)
        return MINILANG_ERR_INVALID;
    switch (backend) {
    case MINILANG_BACKEND_INTERPRETER:
        ctx->backend = BackendType::Interpreter;
        return MINILANG_OK;
    case MINILANG_BACKEND_STACKVM:
        ctx->backend = BackendType::StackVM_IR;
        return MINILANG_OK;
    case MINILANG_BACKEND_REGISTERVM:
        ctx->backend = BackendType::RegisterVM_IR;
        return MINILANG_OK;
    }
    return MINILANG_ERR_INVALID;
}

minilang_status minilang_eval(minilang_context* ctx, const char* source) {
    if (!ctx || !source)
        return MINILANG_ERR_INVALID;
    ctx->lastOutput.clear();
    ctx->lastError.clear();

    BackendExecResult result = BackendExecutionService::execute(source, ctx->backend);
    ctx->lastOutput = result.output;
    if (result.success) {
        return MINILANG_OK;
    }
    // 错误消息带阶段前缀（"运行时错误: ..." / "语法错误: ..."），
    // 与 CLI/GUI 呈现口径一致
    ctx->lastError = result.errorPrefix.empty() ? result.errorMsg : (result.errorPrefix + ": " + result.errorMsg);
    return (result.errorPrefix == "运行时错误") ? MINILANG_ERR_RUNTIME : MINILANG_ERR_COMPILE;
}

const char* minilang_get_output(const minilang_context* ctx) {
    return ctx ? ctx->lastOutput.c_str() : "";
}

const char* minilang_get_error(const minilang_context* ctx) {
    return ctx ? ctx->lastError.c_str() : "";
}

minilang_backend minilang_get_backend(const minilang_context* ctx) {
    if (!ctx)
        return MINILANG_BACKEND_INTERPRETER;
    switch (ctx->backend) {
    case BackendType::StackVM_IR:
        return MINILANG_BACKEND_STACKVM;
    case BackendType::RegisterVM_IR:
        return MINILANG_BACKEND_REGISTERVM;
    case BackendType::Interpreter:
    default:
        return MINILANG_BACKEND_INTERPRETER;
    }
}

const char* minilang_version(void) {
    return "libminilang 0.1.0";
}

} // extern "C"
