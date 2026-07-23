// ============================================================
// doc/DocGenerator.cpp - MiniLang API 文档生成器实现
// ------------------------------------------------------------
// R162 B2 工具链拓展：8 项实现要点
//   1. DocComment 解析：从 /// 或 /** */ 提取摘要 + 标签
//   2. 行号关联：文档注释的结束行号 + 1 = 声明起始行号
//   3. 签名构造：函数/类/枚举/变量的签名生成
//   4. Markdown 输出：标题层级 + 表格 + 代码块
//   5. HTML 输出：基础样式 + 锚点 + 折叠
//   6. JSON 输出：结构化数据
//   7. 标签处理：@param/@return/@example/@deprecated/@see/@since
//   8. 多行摘要合并：连续 /// 合并为一段
// ============================================================
#include "doc/DocGenerator.h"

#include "ast/ASTNode.h"

#include <cctype>
#include <sstream>

namespace minilang::doc {

// ============================================================
// DocComment 方法
// ============================================================

bool DocComment::isDeprecated() const {
    for (const auto& tag : tags) {
        if (tag.kind == DocTagKind::Deprecated) {
            return true;
        }
    }
    return false;
}

bool DocComment::hasSince() const {
    for (const auto& tag : tags) {
        if (tag.kind == DocTagKind::Since) {
            return true;
        }
    }
    return false;
}

std::string DocComment::sinceVersion() const {
    for (const auto& tag : tags) {
        if (tag.kind == DocTagKind::Since) {
            return tag.value;
        }
    }
    return "";
}

std::vector<DocTag> DocComment::params() const {
    std::vector<DocTag> result;
    for (const auto& tag : tags) {
        if (tag.kind == DocTagKind::Param) {
            result.push_back(tag);
        }
    }
    return result;
}

const DocTag* DocComment::returnTag() const {
    for (const auto& tag : tags) {
        if (tag.kind == DocTagKind::Return) {
            return &tag;
        }
    }
    return nullptr;
}

// ============================================================
// DocCommentExtractor
// ============================================================

std::unordered_map<int, DocComment> DocCommentExtractor::extract(const std::vector<Token>& comments) {
    std::unordered_map<int, DocComment> result;
    std::vector<Token> pendingLineComments; // 连续的 /// 待合并

    auto flushLineComments = [&]() {
        if (pendingLineComments.empty()) {
            return;
        }
        // 合并连续 /// 为单个 DocComment
        std::string raw;
        int startLine = pendingLineComments.front().line;
        for (const auto& tok : pendingLineComments) {
            // 去掉文档注释前缀：/// 去掉 3 字符，// 去掉 2 字符
            std::string text = tok.lexeme;
            if (text.size() >= 3 && text[0] == '/' && text[1] == '/' && text[2] == '/') {
                text = text.substr(3); // 去掉 ///
            } else if (text.size() >= 2 && text[0] == '/' && text[1] == '/') {
                text = text.substr(2); // 去掉 //
            }
            // 去掉前导空格
            size_t firstNonSpace = text.find_first_not_of(" \t");
            if (firstNonSpace != std::string::npos) {
                text = text.substr(firstNonSpace);
            }
            if (!raw.empty()) {
                raw += "\n";
            }
            raw += text;
        }
        int endLine = pendingLineComments.back().line;
        DocComment doc = parseDocComment(raw, startLine);
        doc.line = startLine;
        result[endLine] = doc;
        pendingLineComments.clear();
    };

    for (const auto& tok : comments) {
        if (tok.type == TokenType::TK_LINE_COMMENT) {
            // 检查是否为 /// 文档注释（lexeme 以 "///" 开头）
            if (tok.lexeme.size() >= 3 && tok.lexeme[0] == '/' && tok.lexeme[1] == '/' && tok.lexeme[2] == '/') {
                // 检查是否与上一个连续（行号 +1）
                if (!pendingLineComments.empty() && pendingLineComments.back().line + 1 != tok.line) {
                    flushLineComments();
                }
                pendingLineComments.push_back(tok);
            } else {
                // 非 /// 行注释，打断连续性
                flushLineComments();
            }
        } else if (tok.type == TokenType::TK_BLOCK_COMMENT) {
            // 检查是否为 /** 文档注释（lexeme 以 "/**" 开头且非 "/*"）
            flushLineComments(); // 块注释打断行注释连续性
            if (tok.lexeme.size() >= 3 && tok.lexeme[0] == '/' && tok.lexeme[1] == '*' && tok.lexeme[2] == '*') {
                // 提取块注释内容（去掉 /** 和 */ ）
                std::string raw = tok.lexeme;
                if (raw.size() >= 4 && raw.back() == '/' && raw[raw.size() - 2] == '*') {
                    raw = raw.substr(3, raw.size() - 5); // 去掉 /** 和 */
                } else {
                    raw = raw.substr(3); // 去掉 /**
                }
                // 去掉每行前导 * 和空格
                std::istringstream iss(raw);
                std::string line;
                std::string cleaned;
                while (std::getline(iss, line)) {
                    size_t firstNonSpace = line.find_first_not_of(" \t");
                    if (firstNonSpace != std::string::npos && line[firstNonSpace] == '*') {
                        line = line.substr(firstNonSpace + 1);
                        firstNonSpace = line.find_first_not_of(" \t");
                        if (firstNonSpace != std::string::npos) {
                            line = line.substr(firstNonSpace);
                        }
                    } else if (firstNonSpace != std::string::npos) {
                        line = line.substr(firstNonSpace);
                    }
                    if (!cleaned.empty()) {
                        cleaned += "\n";
                    }
                    cleaned += line;
                }
                DocComment doc = parseDocComment(cleaned, tok.line);
                doc.line = tok.line;
                // 计算 endLine：起始行号 + 注释内容中的换行符数量
                int newlineCount = 0;
                for (char c : tok.lexeme) {
                    if (c == '\n') {
                        ++newlineCount;
                    }
                }
                int endLine = tok.line + newlineCount;
                result[endLine] = doc; // key = endLine（findDocForDecl 用 declLine - 1 查找）
            }
        }
    }
    flushLineComments();
    return result;
}

DocComment DocCommentExtractor::parseDocComment(const std::string& raw, int /*startLine*/) {
    DocComment doc;
    doc.rawText = raw;

    // 解析摘要 + 标签：摘要是不含 @tag 的前缀部分，标签以 @ 开头
    std::istringstream iss(raw);
    std::string line;
    std::string summary;
    bool inSummary = true;
    DocTag* currentTag = nullptr;

    while (std::getline(iss, line)) {
        // 检测标签行（以 @ 开头，跳过前导空格）
        size_t firstNonSpace = line.find_first_not_of(" \t");
        if (firstNonSpace != std::string::npos && line[firstNonSpace] == '@') {
            inSummary = false;
            // 解析标签名
            size_t nameStart = firstNonSpace + 1;
            size_t nameEnd = nameStart;
            while (nameEnd < line.size() && (std::isalnum(static_cast<unsigned char>(line[nameEnd])))) {
                ++nameEnd;
            }
            std::string tagName = line.substr(nameStart, nameEnd - nameStart);
            std::string rest = line.substr(nameEnd);
            // 去掉 rest 前导空格
            size_t restStart = rest.find_first_not_of(" \t");
            if (restStart != std::string::npos) {
                rest = rest.substr(restStart);
            } else {
                rest.clear();
            }

            DocTag tag;
            tag.name = tagName;
            if (tagName == "param") {
                tag.kind = DocTagKind::Param;
                // @param <name> <描述>
                size_t spacePos = rest.find_first_of(" \t");
                if (spacePos != std::string::npos) {
                    tag.value = rest.substr(0, spacePos);
                    tag.body = rest.substr(spacePos + 1);
                } else {
                    tag.value = rest;
                }
            } else if (tagName == "return") {
                tag.kind = DocTagKind::Return;
                tag.body = rest;
            } else if (tagName == "example") {
                tag.kind = DocTagKind::Example;
                tag.body = rest;
            } else if (tagName == "deprecated") {
                tag.kind = DocTagKind::Deprecated;
                tag.body = rest;
            } else if (tagName == "see") {
                tag.kind = DocTagKind::See;
                tag.value = rest;
            } else if (tagName == "since") {
                tag.kind = DocTagKind::Since;
                tag.value = rest;
            } else {
                tag.kind = DocTagKind::Other;
                tag.body = rest;
            }
            doc.tags.push_back(tag);
            currentTag = &doc.tags.back();
        } else if (inSummary) {
            if (!summary.empty()) {
                summary += "\n";
            }
            summary += line;
        } else if (currentTag) {
            // 标签的续行
            if (!currentTag->body.empty()) {
                currentTag->body += "\n";
            }
            currentTag->body += line;
        }
    }

    // 去掉摘要首尾空白
    size_t first = summary.find_first_not_of(" \t\n");
    size_t last = summary.find_last_not_of(" \t\n");
    if (first != std::string::npos) {
        doc.summary = summary.substr(first, last - first + 1);
    }
    return doc;
}

// ============================================================
// DocGenerator
// ============================================================

DocGenerator::DocGenerator(DocFormat format) : format_(format) {}

DocResult DocGenerator::generate(Block& program, const std::vector<Token>& comments) {
    result_ = DocResult{};
    DocCommentExtractor extractor;
    docComments_ = extractor.extract(comments);
    program.accept(*this);
    return std::move(result_);
}

std::string DocGenerator::formatOutput(const DocResult& result) const {
    switch (format_) {
    case DocFormat::Markdown:
        return formatMarkdown(result);
    case DocFormat::Html:
        return formatHtml(result);
    case DocFormat::Json:
        return formatJson(result);
    }
    return "";
}

void DocGenerator::defaultVisit(ASTNode& node) {
    // 手动遍历子节点（ASTNode::children() 返回原始指针，需调用 accept）
    for (auto* child : node.children()) {
        if (child) {
            child->accept(*this);
        }
    }
}

void DocGenerator::visitFunDecl(FunDecl& node) {
    DocEntry entry;
    entry.kind = DocEntryKind::Function;
    entry.name = node.name;
    entry.line = node.line;
    entry.column = node.column;
    entry.params = node.params;
    entry.paramTypes = node.paramTypes;
    entry.returnType = node.returnType;
    entry.typeParams = node.typeParams; // R163 泛型扩展
    entry.signature = formatFunSignature(node);
    if (auto* doc = findDocForDecl(node.line)) {
        entry.doc = *doc;
    }
    result_.entries.push_back(std::move(entry));
    // 不遍历函数体（文档只记录顶层声明）
}

void DocGenerator::visitClassDecl(ClassDecl& node) {
    DocEntry entry;
    entry.kind = DocEntryKind::Class;
    entry.name = node.name;
    entry.line = node.line;
    entry.column = node.column;
    entry.superClass = node.superClassName;
    entry.typeParams = node.typeParams; // R163 泛型扩展
    entry.signature = formatClassSignature(node);
    // 收集成员名
    for (const auto& m : node.members) {
        if (m) {
            entry.members.push_back(m->nodeName());
        }
    }
    if (auto* doc = findDocForDecl(node.line)) {
        entry.doc = *doc;
    }
    result_.entries.push_back(std::move(entry));
    // 不遍历类成员（文档只记录顶层声明）
}

void DocGenerator::visitEnumDecl(EnumDecl& node) {
    DocEntry entry;
    entry.kind = DocEntryKind::Enum;
    entry.name = node.name;
    entry.line = node.line;
    entry.column = node.column;
    entry.typeParams = node.typeParams;
    entry.signature = formatEnumSignature(node);
    for (const auto& v : node.variants) {
        entry.variants.push_back(v.name);
    }
    if (auto* doc = findDocForDecl(node.line)) {
        entry.doc = *doc;
    }
    result_.entries.push_back(std::move(entry));
}

void DocGenerator::visitVarDecl(VarDecl& node) {
    DocEntry entry;
    entry.kind = DocEntryKind::Variable;
    entry.name = node.name;
    entry.line = node.line;
    entry.column = node.column;
    entry.signature = formatVarSignature(node);
    if (auto* doc = findDocForDecl(node.line)) {
        entry.doc = *doc;
    }
    result_.entries.push_back(std::move(entry));
}

const DocComment* DocGenerator::findDocForDecl(int declLine) const {
    // 查找结束行号 = declLine - 1 的文档注释
    auto it = docComments_.find(declLine - 1);
    if (it != docComments_.end()) {
        return &it->second;
    }
    return nullptr;
}

std::string DocGenerator::formatFunSignature(const FunDecl& node) const {
    std::string sig = "fun " + node.name;
    // R163 泛型扩展：输出类型参数列表（与 formatEnumSignature 一致）
    if (!node.typeParams.empty()) {
        sig += "<";
        for (size_t i = 0; i < node.typeParams.size(); ++i) {
            if (i > 0)
                sig += ", ";
            sig += node.typeParams[i];
        }
        sig += ">";
    }
    sig += "(";
    for (size_t i = 0; i < node.params.size(); ++i) {
        if (i > 0)
            sig += ", ";
        sig += node.params[i];
        if (i < node.paramTypes.size() && !node.paramTypes[i].empty()) {
            sig += ": " + node.paramTypes[i];
        }
    }
    sig += ")";
    if (!node.returnType.empty()) {
        sig += ": " + node.returnType;
    }
    return sig;
}

std::string DocGenerator::formatClassSignature(const ClassDecl& node) const {
    std::string sig = "class " + node.name;
    // R163 泛型扩展：输出类型参数列表（与 formatEnumSignature 一致）
    if (!node.typeParams.empty()) {
        sig += "<";
        for (size_t i = 0; i < node.typeParams.size(); ++i) {
            if (i > 0)
                sig += ", ";
            sig += node.typeParams[i];
        }
        sig += ">";
    }
    if (!node.superClassName.empty()) {
        sig += " : " + node.superClassName;
    }
    return sig;
}

std::string DocGenerator::formatEnumSignature(const EnumDecl& node) const {
    std::string sig = "enum " + node.name;
    if (!node.typeParams.empty()) {
        sig += "<";
        for (size_t i = 0; i < node.typeParams.size(); ++i) {
            if (i > 0)
                sig += ", ";
            sig += node.typeParams[i];
        }
        sig += ">";
    }
    return sig;
}

std::string DocGenerator::formatVarSignature(const VarDecl& node) const {
    return "var " + node.name;
}

// ============================================================
// 格式化输出
// ============================================================

std::string DocGenerator::formatMarkdown(const DocResult& result) const {
    std::ostringstream oss;
    oss << "# API 文档\n\n";
    if (result.entries.empty()) {
        oss << "_无顶层声明_\n";
        return oss.str();
    }

    // 按种类分组
    auto writeGroup = [&](DocEntryKind kind, const std::string& title) {
        bool hasAny = false;
        for (const auto& e : result.entries) {
            if (e.kind == kind) {
                if (!hasAny) {
                    oss << "## " << title << "\n\n";
                    hasAny = true;
                }
                oss << entryMarkdown(e);
            }
        }
        if (hasAny) {
            oss << "\n";
        }
    };

    writeGroup(DocEntryKind::Function, "函数");
    writeGroup(DocEntryKind::Class, "类");
    writeGroup(DocEntryKind::Enum, "枚举");
    writeGroup(DocEntryKind::Variable, "变量");
    return oss.str();
}

std::string DocGenerator::formatHtml(const DocResult& result) const {
    std::ostringstream oss;
    oss << "<!DOCTYPE html>\n<html lang=\"zh-CN\">\n<head>\n<meta charset=\"UTF-8\">\n";
    oss << "<title>MiniLang API 文档</title>\n";
    oss << "<style>\n";
    oss << "body { font-family: 'Segoe UI', sans-serif; max-width: 960px; margin: 0 auto; padding: 20px; }\n";
    oss << ".entry { border-left: 3px solid #268BD2; padding: 12px; margin: 12px 0; background: #F5F5F5; }\n";
    oss << ".signature { font-family: Consolas, monospace; font-weight: bold; color: #268BD2; }\n";
    oss << ".summary { color: #555; margin: 8px 0; }\n";
    oss << ".deprecated { color: #DC322F; font-weight: bold; }\n";
    oss << ".tag { margin: 4px 0; }\n";
    oss << ".tag-name { font-weight: bold; color: #B58900; }\n";
    oss << "code { background: #EEE8D5; padding: 2px 4px; border-radius: 3px; }\n";
    oss << "pre { background: #EEE8D5; padding: 12px; border-radius: 5px; overflow-x: auto; }\n";
    oss << "</style>\n</head>\n<body>\n";
    oss << "<h1>MiniLang API 文档</h1>\n";

    if (result.entries.empty()) {
        oss << "<p><em>无顶层声明</em></p>\n";
    } else {
        for (const auto& e : result.entries) {
            oss << entryHtml(e);
        }
    }
    oss << "</body>\n</html>\n";
    return oss.str();
}

std::string DocGenerator::formatJson(const DocResult& result) const {
    std::ostringstream oss;
    oss << "{\n  \"entries\": [";
    for (size_t i = 0; i < result.entries.size(); ++i) {
        if (i > 0)
            oss << ",";
        oss << entryJson(result.entries[i]);
    }
    oss << "\n  ]\n}\n";
    return oss.str();
}

std::string DocGenerator::entryMarkdown(const DocEntry& entry) const {
    std::ostringstream oss;
    oss << "### " << kindIcon(entry.kind) << " " << entry.signature << "\n\n";
    if (!entry.doc.summary.empty()) {
        oss << entry.doc.summary << "\n\n";
    }
    if (entry.doc.isDeprecated()) {
        oss << "> ⚠️ **已弃用**";
        for (const auto& t : entry.doc.tags) {
            if (t.kind == DocTagKind::Deprecated && !t.body.empty()) {
                oss << ": " << t.body;
            }
        }
        oss << "\n\n";
    }
    // @param 标签
    auto params = entry.doc.params();
    if (!params.empty()) {
        oss << "**参数:**\n\n";
        for (const auto& p : params) {
            oss << "- `" << p.value << "`";
            if (!p.body.empty()) {
                oss << " — " << p.body;
            }
            oss << "\n";
        }
        oss << "\n";
    }
    // @return 标签
    if (auto* ret = entry.doc.returnTag()) {
        oss << "**返回:** " << ret->body << "\n\n";
    }
    // @example 标签
    for (const auto& t : entry.doc.tags) {
        if (t.kind == DocTagKind::Example) {
            oss << "**示例:**\n```minilang\n" << t.body << "\n```\n\n";
        }
    }
    // @see 标签
    for (const auto& t : entry.doc.tags) {
        if (t.kind == DocTagKind::See) {
            oss << "**参见:** " << t.value << "\n\n";
        }
    }
    // @since 标签
    if (entry.doc.hasSince()) {
        oss << "**引入版本:** " << entry.doc.sinceVersion() << "\n\n";
    }
    // 枚举 variant 列表
    if (entry.kind == DocEntryKind::Enum && !entry.variants.empty()) {
        oss << "**Variants:** ";
        for (size_t i = 0; i < entry.variants.size(); ++i) {
            if (i > 0)
                oss << ", ";
            oss << "`" << entry.variants[i] << "`";
        }
        oss << "\n\n";
    }
    // 类成员列表
    if (entry.kind == DocEntryKind::Class && !entry.members.empty()) {
        oss << "**成员:** ";
        for (size_t i = 0; i < entry.members.size(); ++i) {
            if (i > 0)
                oss << ", ";
            oss << "`" << entry.members[i] << "`";
        }
        oss << "\n\n";
    }
    oss << "---\n\n";
    return oss.str();
}

std::string DocGenerator::entryHtml(const DocEntry& entry) const {
    std::ostringstream oss;
    oss << "<div class=\"entry\">\n";
    oss << "  <div class=\"signature\">" << kindIcon(entry.kind) << " " << entry.signature << "</div>\n";
    if (!entry.doc.summary.empty()) {
        oss << "  <div class=\"summary\">" << entry.doc.summary << "</div>\n";
    }
    if (entry.doc.isDeprecated()) {
        oss << "  <div class=\"deprecated\">⚠️ 已弃用";
        for (const auto& t : entry.doc.tags) {
            if (t.kind == DocTagKind::Deprecated && !t.body.empty()) {
                oss << ": " << t.body;
            }
        }
        oss << "</div>\n";
    }
    auto params = entry.doc.params();
    if (!params.empty()) {
        oss << "  <div><strong>参数:</strong><ul>\n";
        for (const auto& p : params) {
            oss << "    <li><code>" << p.value << "</code>";
            if (!p.body.empty()) {
                oss << " — " << p.body;
            }
            oss << "</li>\n";
        }
        oss << "  </ul></div>\n";
    }
    if (auto* ret = entry.doc.returnTag()) {
        oss << "  <div><strong>返回:</strong> " << ret->body << "</div>\n";
    }
    for (const auto& t : entry.doc.tags) {
        if (t.kind == DocTagKind::Example) {
            oss << "  <div><strong>示例:</strong><pre>" << t.body << "</pre></div>\n";
        }
    }
    oss << "</div>\n";
    return oss.str();
}

std::string DocGenerator::entryJson(const DocEntry& entry) const {
    std::ostringstream oss;
    oss << "\n    {";
    oss << "\"kind\": \"" << kindName(entry.kind) << "\", ";
    oss << "\"name\": \"" << entry.name << "\", ";
    oss << "\"signature\": \"" << entry.signature << "\", ";
    oss << "\"line\": " << entry.line << ", ";
    oss << "\"column\": " << entry.column << ", ";
    oss << "\"summary\": \"" << entry.doc.summary << "\", ";
    oss << "\"deprecated\": " << (entry.doc.isDeprecated() ? "true" : "false");
    if (entry.doc.hasSince()) {
        oss << ", \"since\": \"" << entry.doc.sinceVersion() << "\"";
    }
    // @param 标签
    auto params = entry.doc.params();
    if (!params.empty()) {
        oss << ", \"params\": [";
        for (size_t i = 0; i < params.size(); ++i) {
            if (i > 0)
                oss << ", ";
            oss << "{\"name\": \"" << params[i].value << "\", \"desc\": \"" << params[i].body << "\"}";
        }
        oss << "]";
    }
    if (auto* ret = entry.doc.returnTag()) {
        oss << ", \"return\": \"" << ret->body << "\"";
    }
    oss << "}";
    return oss.str();
}

std::string DocGenerator::kindName(DocEntryKind kind) {
    switch (kind) {
    case DocEntryKind::Function:
        return "function";
    case DocEntryKind::Class:
        return "class";
    case DocEntryKind::Enum:
        return "enum";
    case DocEntryKind::Variable:
        return "variable";
    case DocEntryKind::EnumVariant:
        return "enum-variant";
    }
    return "unknown";
}

std::string DocGenerator::kindIcon(DocEntryKind kind) {
    switch (kind) {
    case DocEntryKind::Function:
        return "🔧";
    case DocEntryKind::Class:
        return "📦";
    case DocEntryKind::Enum:
        return "🏷";
    case DocEntryKind::Variable:
        return "📝";
    case DocEntryKind::EnumVariant:
        return "🔹";
    }
    return "❓";
}

} // namespace minilang::doc
