#pragma once

// ============================================================
// PipelineRunner — 编译管线（ARCH-11 拆分自 IdeController）
// ------------------------------------------------------------
// 职责：
//   - 词法分析（Lexer）
//   - 语法分析（Parser）
//   - 字节码编译（Compiler）
//   - 代码格式化（Formatter）
//   - 统一前端管线（runFrontendPipeline）
//
// 持有所有管线状态（lastTokens_/astRoot_/lastCompileResult_），
// 供 WorkerManager / VmStepper / GUI 消费。
// ============================================================

#include <QObject>
#include <QString>
#include <memory>
#include <string>
#include <vector>

#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "compiler/Compiler.h"
#include "compiler/Bytecode.h"
#include "formatter/Formatter.h"
#include "ast/ASTNode.h"
#include "Diagnostic.h"

class PipelineRunner : public QObject {
    Q_OBJECT

public:
    explicit PipelineRunner(QObject* parent = nullptr);

    // ---- 管线操作 ----
    /// 执行词法分析，返回 true 表示无错误
    bool runLexer(const std::string& source);
    /// 执行语法分析（使用上次 lastTokens_），返回 true 表示无错误
    bool runParser();
    /// 执行字节码编译，返回 true 表示无错误
    bool runCompiler();
    /// 格式化代码（需先 runLexer + runParser），成功时写入 formatted
    bool formatCode(std::string& formatted);

    /// C9 fix: 前端管线结果状态码
    enum class PipelineStatus {
        OK,            ///< Lexer+Parser 均成功
        LexerFailed,   ///< Lexer 异常或错误
        ParserFailed   ///< Parser 异常或错误
    };

    /// C9 fix: 前端管线结果
    struct PipelineResult {
        PipelineStatus status = PipelineStatus::OK;
        std::string errorMessage;  ///< 异常消息（status != OK 时有效）
        const DiagnosticBag* diagnostics = nullptr;  ///< 错误诊断包指针
    };

    /// C9 fix: 统一前端管线（Lexer + Parser），消除重复。
    /// 调用后 lastTokens_/astRoot_ 已就绪，caller 根据 status 决定后续动作。
    /// 错误诊断已 emit diagnosticsReady，caller 仅需更新 UI。
    PipelineResult runFrontendPipeline(const std::string& source);

    // ---- 引擎访问（供 IdeController 内部转发）----
    // ARCH-11 fix: 提供 const 重载，使 const 方法（如 getKeywords）可访问
    Formatter& formatter() { return formatter_; }
    const Formatter& formatter() const { return formatter_; }
    Lexer& lexer() { return lexer_; }
    const Lexer& lexer() const { return lexer_; }
    Parser& parser() { return parser_; }
    const Parser& parser() const { return parser_; }
    Compiler& compiler() { return compiler_; }
    const Compiler& compiler() const { return compiler_; }

    // ---- 状态访问 ----
    const std::vector<Token>& lastTokens() const { return lastTokens_; }
    const CompileResult& lastCompileResult() const { return lastCompileResult_; }
    /// 返回 AST 根节点（shared_ptr 共享所有权，worker 线程可安全持有）
    std::shared_ptr<Block> astRoot() const { return astRoot_; }
    /// AST 根节点裸指针（仅供主线程同步访问，不延长生命周期）
    Block* astRootPtr() const { return astRoot_.get(); }
    const DiagnosticBag& lexerDiagnostics() const { return lexer_.getDiagnostics(); }
    const DiagnosticBag& parserDiagnostics() const { return parser_.getDiagnostics(); }
    const DiagnosticBag& compilerDiagnostics() const { return compiler_.getDiagnostics(); }

    /// BUG-ORCH-7 fix: 失效前端管线缓存（Compiler 设置变更或外部强制刷新时调用）
    void invalidatePipelineCache() { cachedPipelineSource_.clear(); }

private:
    /// BUG-ORCH-7 fix: 缓存前端管线结果
    void cachePipelineResult(const std::string& source, const PipelineResult& result);

signals:
    void diagnosticsReady(const DiagnosticBag& bag);

private:
    Lexer lexer_;
    Parser parser_;
    Compiler compiler_;
    Formatter formatter_;
    // QT-R-03 fix: shared_ptr 使 worker 线程共享 AST 所有权，避免主线程重新 parse
    // 导致 astRoot_ 替换后 worker 持有悬垂引用。
    std::shared_ptr<Block> astRoot_;
    std::vector<Token> lastTokens_;
    CompileResult lastCompileResult_;
    // BUG-ORCH-7 fix: 前端管线源码级缓存，避免 blockIfHasErrors + prepareRun 重复执行 Lexer/Parser
    std::string cachedPipelineSource_;
    PipelineResult cachedPipelineResult_;
};
