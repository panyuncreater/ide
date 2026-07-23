// ============================================================
// doc/DocGenerator.h - MiniLang API 文档生成器
// ------------------------------------------------------------
// R162 B2 工具链拓展：从源码注释生成 API 文档（类似 rustdoc / javadoc）。
// 继承 DefaultVisitor 遍历 AST，收集顶层声明（函数/类/枚举/变量），
// 关联紧邻的文档注释（/// 或 /** */），输出 Markdown 或 HTML。
//
// 文档注释约定（与 Lexer 注释系统集成）：
//   /// 单行文档注释（连续 /// 合并为一段）
//   /** 多行文档注释 */
//   支持的标签：
//     @param <name> <描述>     参数说明
//     @return <描述>            返回值说明
//     @example <代码>           示例代码
//     @deprecated <描述>        标记为已弃用
//     @see <引用>               参见其他条目
//     @since <版本>             引入版本
//
// 设计要点：
//   - 复用 Lexer → Parser → AST 管线
//   - DocCommentExtractor 预处理注释流，按行号建立"声明前文档注释"映射
//   - 输出 Markdown（默认）或 HTML（便于 GitHub Pages 部署）
//   - 不依赖 Qt6 / Interpreter / Compiler，仅依赖 ast + common + lexer
//
// @see DefaultVisitor DiagnosticBag
// ============================================================
#pragma once

#include "common/Diagnostic.h"
#include "interpreter/Visitor.h"
#include "lexer/Token.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace minilang::doc {

/// 文档输出格式
enum class DocFormat {
    Markdown, ///< Markdown 格式（默认）
    Html,     ///< HTML 格式（带基础样式）
    Json      ///< JSON 格式（机器可读，便于 IDE 集成）
};

/// 文档标签种类
enum class DocTagKind {
    Param,      ///< @param
    Return,     ///< @return
    Example,    ///< @example
    Deprecated, ///< @deprecated
    See,        ///< @see
    Since,      ///< @since
    Other       ///< 其他自定义标签
};

/// 单个文档标签
struct DocTag {
    DocTagKind kind;   ///< 标签种类
    std::string name;  ///< 标签名（如 "param"）
    std::string value; ///< 标签值（@param 的参数名、@return 的描述等）
    std::string body;  ///< 标签正文描述
};

/// 解析后的文档注释
struct DocComment {
    std::string summary;      ///< 摘要（第一段，无标签部分）
    std::vector<DocTag> tags; ///< 标签列表
    std::string rawText;      ///< 原始注释文本（不含 /// 或 /** */ 标记）
    int line = 0;             ///< 注释起始行号

    /// 是否标记为已弃用
    bool isDeprecated() const;
    /// 是否有 @since 标签
    bool hasSince() const;
    /// 获取 @since 版本
    std::string sinceVersion() const;
    /// 获取所有 @param 标签
    std::vector<DocTag> params() const;
    /// 获取 @return 标签
    const DocTag* returnTag() const;
};

/// 文档条目种类
enum class DocEntryKind {
    Function,   ///< 函数
    Class,      ///< 类
    Enum,       ///< 枚举
    Variable,   ///< 全局变量
    EnumVariant ///< 枚举 variant
};

/// 单个文档条目（函数/类/枚举/变量）
struct DocEntry {
    DocEntryKind kind;     ///< 条目种类
    std::string name;      ///< 名称
    std::string signature; ///< 签名（如 "fun foo(a: int, b: int): int"）
    DocComment doc;        ///< 关联的文档注释（无则 summary 为空）
    int line = 0;          ///< 声明行号
    int column = 0;        ///< 声明列号

    // 函数特有
    std::vector<std::string> params;     ///< 参数名列表
    std::vector<std::string> paramTypes; ///< 参数类型列表
    std::string returnType;              ///< 返回类型

    // 类特有
    std::string superClass;           ///< 父类名
    std::vector<std::string> members; ///< 成员名称列表

    // 枚举/函数/类共有（R163 泛型扩展）
    std::vector<std::string> typeParams; ///< 类型参数（泛型，空=非泛型）
    // 枚举特有
    std::vector<std::string> variants;   ///< variant 名称列表
};

/// 文档生成结果
struct DocResult {
    std::vector<DocEntry> entries; ///< 所有顶层条目
    DiagnosticBag diagnostics;     ///< 诊断信息（解析/生成时的警告）
    bool ok = true;                ///< 是否成功
};

/// 文档注释提取器：从 Lexer 注释流中提取文档注释
///
/// 文档注释约定：
///   - TK_LINE_COMMENT 且 lexeme 以 "///" 开头（去掉 "//" 前缀后以 "/" 开头）
///   - TK_BLOCK_COMMENT 且 lexeme 以 "/**" 开头（非 "/*"）
///   - 连续的 /// 行合并为单个 DocComment
///   - 注释的"结束行号 + 1"用于匹配下一个声明的"起始行号"
class DocCommentExtractor {
public:
    /// 从 Token 列表提取文档注释
    /// @param comments Lexer::comments() 返回的注释 Token 列表
    /// @return 按结束行号索引的文档注释映射（endLine → DocComment）
    std::unordered_map<int, DocComment> extract(const std::vector<Token>& comments);

private:
    /// 解析单个文档注释的标签
    DocComment parseDocComment(const std::string& raw, int startLine);
};

/// DocGenerator 文档生成器
///
/// 继承 DefaultVisitor 遍历 AST，收集顶层声明条目，
/// 关联 DocCommentExtractor 提取的文档注释，输出文档。
class DocGenerator : public DefaultVisitor {
public:
    explicit DocGenerator(DocFormat format = DocFormat::Markdown);

    /// 生成文档
    /// @param program AST 根节点
    /// @param comments Lexer 注释 Token 列表
    DocResult generate(Block& program, const std::vector<Token>& comments);

    /// 格式化文档输出
    std::string formatOutput(const DocResult& result) const;

    // ---- DefaultVisitor override ----
    void defaultVisit(ASTNode& node) override;
    void visitFunDecl(FunDecl& node) override;
    void visitClassDecl(ClassDecl& node) override;
    void visitEnumDecl(EnumDecl& node) override;
    void visitVarDecl(VarDecl& node) override;

private:
    DocFormat format_;
    DocResult result_;
    std::unordered_map<int, DocComment> docComments_; ///< 按结束行号索引

    /// 查找声明前紧邻的文档注释
    /// @param declLine 声明行号
    /// @return 文档注释指针（无则 nullptr）
    const DocComment* findDocForDecl(int declLine) const;

    /// 构造函数签名
    std::string formatFunSignature(const FunDecl& node) const;
    /// 构造类签名
    std::string formatClassSignature(const ClassDecl& node) const;
    /// 构造枚举签名
    std::string formatEnumSignature(const EnumDecl& node) const;
    /// 构造变量签名
    std::string formatVarSignature(const VarDecl& node) const;

    /// Markdown 输出
    std::string formatMarkdown(const DocResult& result) const;
    /// HTML 输出
    std::string formatHtml(const DocResult& result) const;
    /// JSON 输出
    std::string formatJson(const DocResult& result) const;

    /// 单个条目的 Markdown 片段
    std::string entryMarkdown(const DocEntry& entry) const;
    /// 单个条目的 HTML 片段
    std::string entryHtml(const DocEntry& entry) const;
    /// 单个条目的 JSON 片段
    std::string entryJson(const DocEntry& entry) const;

    /// 条目种类名
    static std::string kindName(DocEntryKind kind);
    /// 条目 emoji 图标
    static std::string kindIcon(DocEntryKind kind);
};

} // namespace minilang::doc
