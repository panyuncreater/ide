/**
 * @file ast/MacroExpander.cpp
 * @brief 七特性 MVP 阶段 2：宏展开器实现——表达式子集克隆 + 参数替换。
 *
 * 设计说明见 MacroExpander.h。实现要点：
 *   - VarRef 命中替换表 → 直接返回实参 shared_ptr（共享，不克隆）
 *   - 其他节点 → 深克隆（新节点，line/column 沿用模板节点，便于错误定位）
 *   - 不支持的节点类型（语句/声明/match/yield 等）→ 返回 nullptr 报告节点名
 */
#include "ast/MacroExpander.h"

#include "common/RuntimeLimits.h"

namespace MacroExpander {

namespace {

/// 克隆表达式列表的辅助函数。任一元素失败返回 false。
bool cloneList(const std::vector<std::shared_ptr<ASTNode>>& src, const SubstMap& subst, int depth,
               std::string* unsupportedNodeName, std::vector<std::shared_ptr<ASTNode>>& out) {
    out.reserve(src.size());
    for (const auto& e : src) {
        auto cloned = cloneWithSubstitution(e.get(), subst, depth, unsupportedNodeName);
        if (!cloned)
            return false;
        out.push_back(std::move(cloned));
    }
    return true;
}

} // anonymous namespace

std::shared_ptr<ASTNode> cloneWithSubstitution(const ASTNode* node, const SubstMap& subst, int depth,
                                               std::string* unsupportedNodeName) {
    if (!node) {
        if (unsupportedNodeName)
            *unsupportedNodeName = "<null>";
        return nullptr;
    }
    if (depth >= RuntimeLimits::MAX_MACRO_EXPANSION_DEPTH) {
        if (unsupportedNodeName)
            *unsupportedNodeName = "<depth-limit>";
        return nullptr;
    }
    const int d = depth + 1;

    switch (node->nodeType) {
    case NodeType::NODE_VAR_REF: {
        const auto* v = static_cast<const VarRef*>(node);
        auto it = subst.find(v->name);
        if (it != subst.end()) {
            // 形参命中：返回实参子树（shared_ptr 共享，同 C 宏的文本替换语义）
            return it->second;
        }
        return std::make_shared<VarRef>(v->name, v->line, v->column);
    }
    case NodeType::NODE_NUMBER_LITERAL: {
        const auto* n = static_cast<const NumberLiteral*>(node);
        if (n->isInt())
            return std::make_shared<NumberLiteral>(n->intVal(), n->line, n->column);
        return std::make_shared<NumberLiteral>(n->floatVal(), n->line, n->column);
    }
    case NodeType::NODE_STRING_LITERAL: {
        const auto* s = static_cast<const StringLiteral*>(node);
        return std::make_shared<StringLiteral>(s->value, s->line, s->column);
    }
    case NodeType::NODE_BOOL_LITERAL: {
        const auto* b = static_cast<const BoolLiteral*>(node);
        return std::make_shared<BoolLiteral>(b->value, b->line, b->column);
    }
    case NodeType::NODE_NULL_LITERAL:
        return std::make_shared<NullLiteral>(node->line, node->column);
    case NodeType::NODE_BINARY_OP: {
        const auto* b = static_cast<const BinaryOp*>(node);
        auto l = cloneWithSubstitution(b->left.get(), subst, d, unsupportedNodeName);
        if (!l)
            return nullptr;
        auto r = cloneWithSubstitution(b->right.get(), subst, d, unsupportedNodeName);
        if (!r)
            return nullptr;
        return std::make_shared<BinaryOp>(b->opType, std::move(l), std::move(r), b->line, b->column);
    }
    case NodeType::NODE_UNARY_OP: {
        const auto* u = static_cast<const UnaryOp*>(node);
        auto o = cloneWithSubstitution(u->operand.get(), subst, d, unsupportedNodeName);
        if (!o)
            return nullptr;
        return std::make_shared<UnaryOp>(u->opType, std::move(o), u->line, u->column);
    }
    case NodeType::NODE_FUN_CALL: {
        const auto* f = static_cast<const FunCall*>(node);
        std::vector<std::shared_ptr<ASTNode>> args;
        if (!cloneList(f->arguments, subst, d, unsupportedNodeName, args))
            return nullptr;
        if (f->callee) {
            auto callee = cloneWithSubstitution(f->callee.get(), subst, d, unsupportedNodeName);
            if (!callee)
                return nullptr;
            return std::make_shared<FunCall>(std::move(callee), std::move(args), f->line, f->column);
        }
        return std::make_shared<FunCall>(f->name, std::move(args), f->line, f->column);
    }
    case NodeType::NODE_INDEX_ACCESS: {
        const auto* ia = static_cast<const IndexAccess*>(node);
        auto obj = cloneWithSubstitution(ia->object.get(), subst, d, unsupportedNodeName);
        if (!obj)
            return nullptr;
        auto idx = cloneWithSubstitution(ia->index.get(), subst, d, unsupportedNodeName);
        if (!idx)
            return nullptr;
        return std::make_shared<IndexAccess>(std::move(obj), std::move(idx), ia->line, ia->column);
    }
    case NodeType::NODE_MEMBER_ACCESS: {
        const auto* ma = static_cast<const MemberAccess*>(node);
        auto obj = cloneWithSubstitution(ma->object.get(), subst, d, unsupportedNodeName);
        if (!obj)
            return nullptr;
        return std::make_shared<MemberAccess>(std::move(obj), ma->fieldName, ma->line, ma->column);
    }
    case NodeType::NODE_METHOD_CALL: {
        const auto* mc = static_cast<const MethodCall*>(node);
        auto obj = cloneWithSubstitution(mc->object.get(), subst, d, unsupportedNodeName);
        if (!obj)
            return nullptr;
        std::vector<std::shared_ptr<ASTNode>> args;
        if (!cloneList(mc->arguments, subst, d, unsupportedNodeName, args))
            return nullptr;
        return std::make_shared<MethodCall>(std::move(obj), mc->methodName, std::move(args), mc->line, mc->column);
    }
    case NodeType::NODE_ARRAY_LITERAL: {
        const auto* a = static_cast<const ArrayLiteral*>(node);
        std::vector<std::shared_ptr<ASTNode>> elems;
        if (!cloneList(a->elements, subst, d, unsupportedNodeName, elems))
            return nullptr;
        return std::make_shared<ArrayLiteral>(std::move(elems), a->line, a->column);
    }
    case NodeType::NODE_TUPLE_LITERAL: {
        const auto* t = static_cast<const TupleLiteral*>(node);
        std::vector<std::shared_ptr<ASTNode>> elems;
        if (!cloneList(t->elements, subst, d, unsupportedNodeName, elems))
            return nullptr;
        return std::make_shared<TupleLiteral>(std::move(elems), t->line, t->column);
    }
    case NodeType::NODE_DICT_LITERAL: {
        const auto* dl = static_cast<const DictLiteral*>(node);
        std::vector<std::pair<std::shared_ptr<ASTNode>, std::shared_ptr<ASTNode>>> pairs;
        pairs.reserve(dl->pairs.size());
        for (const auto& p : dl->pairs) {
            auto k = cloneWithSubstitution(p.first.get(), subst, d, unsupportedNodeName);
            if (!k)
                return nullptr;
            auto v = cloneWithSubstitution(p.second.get(), subst, d, unsupportedNodeName);
            if (!v)
                return nullptr;
            pairs.emplace_back(std::move(k), std::move(v));
        }
        return std::make_shared<DictLiteral>(std::move(pairs), dl->line, dl->column);
    }
    case NodeType::NODE_INTERPOLATED_STRING: {
        const auto* is = static_cast<const InterpolatedString*>(node);
        auto cloned = std::make_shared<InterpolatedString>(is->line, is->column);
        cloned->literals = is->literals;
        cloned->literalsTotalLen = is->literalsTotalLen;
        cloned->endLine = is->endLine;
        if (!cloneList(is->expressions, subst, d, unsupportedNodeName, cloned->expressions))
            return nullptr;
        return cloned;
    }
    case NodeType::NODE_ENUM_VARIANT_EXPR: {
        const auto* ev = static_cast<const EnumVariantExpr*>(node);
        std::vector<std::shared_ptr<ASTNode>> args;
        if (!cloneList(ev->arguments, subst, d, unsupportedNodeName, args))
            return nullptr;
        return std::make_shared<EnumVariantExpr>(ev->enumName, ev->variantName, std::move(args), ev->line, ev->column);
    }
    case NodeType::NODE_MACRO_CALL: {
        // 嵌套宏调用：body 模板解析时内层宏已展开（expanded 非空），
        // 克隆原始实参与 expanded 子树，保持 Formatter 打印形式。
        const auto* mc = static_cast<const MacroCallExpr*>(node);
        std::vector<std::shared_ptr<ASTNode>> args;
        if (!cloneList(mc->arguments, subst, d, unsupportedNodeName, args))
            return nullptr;
        auto exp = cloneWithSubstitution(mc->expanded.get(), subst, d, unsupportedNodeName);
        if (!exp)
            return nullptr;
        return std::make_shared<MacroCallExpr>(mc->name, std::move(args), std::move(exp), mc->line, mc->column);
    }
    default:
        // 语句/声明/match/yield/lambda 等不在表达式模板宏 MVP 支持范围
        if (unsupportedNodeName)
            *unsupportedNodeName = node->nodeName();
        return nullptr;
    }
}

} // namespace MacroExpander
