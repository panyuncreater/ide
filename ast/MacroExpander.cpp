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
        auto cloned = std::make_shared<MacroCallExpr>(mc->name, std::move(args), std::move(exp), mc->line, mc->column);
        cloned->producesValue = mc->producesValue;
        return cloned;
    }
    // ============================================================
    // 七特性宏升级：语句/块体节点克隆（支持语句模板宏）
    // ------------------------------------------------------------
    // 赋值目标名（Assignment.name）的形参替换：若实参为简单 VarRef，则替换为
    // 实参变量名（支持 swap!(x,y) 中 `a=b` → `x=y` 的左值代入）；否则保留原名。
    // 引入型绑定名（VarDecl.name）不替换（同 C 宏：宏内部 var 可能与实参名碰撞，
    // 为已知局限，与文本替换宏的卫生性问题一致）。
    // ============================================================
    case NodeType::NODE_BLOCK: {
        const auto* blk = static_cast<const Block*>(node);
        std::vector<std::shared_ptr<ASTNode>> stmts;
        if (!cloneList(blk->statements, subst, d, unsupportedNodeName, stmts))
            return nullptr;
        auto cloned = std::make_shared<Block>(std::move(stmts), blk->line, blk->column);
        cloned->closingBraceLine = blk->closingBraceLine;
        return cloned;
    }
    case NodeType::NODE_VAR_DECL: {
        const auto* vd = static_cast<const VarDecl*>(node);
        std::shared_ptr<ASTNode> init;
        if (vd->initializer) {
            init = cloneWithSubstitution(vd->initializer.get(), subst, d, unsupportedNodeName);
            if (!init)
                return nullptr;
        }
        return std::make_shared<VarDecl>(vd->name, vd->typeAnnotation, std::move(init), vd->line, vd->column);
    }
    case NodeType::NODE_ASSIGNMENT: {
        const auto* as = static_cast<const Assignment*>(node);
        auto val = cloneWithSubstitution(as->value.get(), subst, d, unsupportedNodeName);
        if (!val)
            return nullptr;
        // 左值名替换：若形参对应实参为简单 VarRef，代入其变量名
        std::string targetName = as->name;
        auto it = subst.find(as->name);
        if (it != subst.end() && it->second && it->second->nodeType == NodeType::NODE_VAR_REF) {
            targetName = static_cast<const VarRef*>(it->second.get())->name;
        }
        return std::make_shared<Assignment>(targetName, std::move(val), as->line, as->column);
    }
    case NodeType::NODE_IF_STMT: {
        const auto* is = static_cast<const IfStmt*>(node);
        auto cond = cloneWithSubstitution(is->condition.get(), subst, d, unsupportedNodeName);
        if (!cond)
            return nullptr;
        auto thenB = cloneWithSubstitution(is->thenBranch.get(), subst, d, unsupportedNodeName);
        if (!thenB)
            return nullptr;
        std::shared_ptr<ASTNode> elseB;
        if (is->elseBranch) {
            elseB = cloneWithSubstitution(is->elseBranch.get(), subst, d, unsupportedNodeName);
            if (!elseB)
                return nullptr;
        }
        return std::make_shared<IfStmt>(std::move(cond), std::move(thenB), std::move(elseB), is->line, is->column);
    }
    case NodeType::NODE_WHILE_STMT: {
        const auto* ws = static_cast<const WhileStmt*>(node);
        auto cond = cloneWithSubstitution(ws->condition.get(), subst, d, unsupportedNodeName);
        if (!cond)
            return nullptr;
        auto body = cloneWithSubstitution(ws->body.get(), subst, d, unsupportedNodeName);
        if (!body)
            return nullptr;
        return std::make_shared<WhileStmt>(std::move(cond), std::move(body), ws->line, ws->column);
    }
    case NodeType::NODE_FOR_STMT: {
        const auto* fs = static_cast<const ForStmt*>(node);
        std::shared_ptr<ASTNode> init, cond, upd;
        if (fs->initializer) {
            init = cloneWithSubstitution(fs->initializer.get(), subst, d, unsupportedNodeName);
            if (!init)
                return nullptr;
        }
        if (fs->condition) {
            cond = cloneWithSubstitution(fs->condition.get(), subst, d, unsupportedNodeName);
            if (!cond)
                return nullptr;
        }
        if (fs->update) {
            upd = cloneWithSubstitution(fs->update.get(), subst, d, unsupportedNodeName);
            if (!upd)
                return nullptr;
        }
        auto body = cloneWithSubstitution(fs->body.get(), subst, d, unsupportedNodeName);
        if (!body)
            return nullptr;
        return std::make_shared<ForStmt>(std::move(init), std::move(cond), std::move(upd), std::move(body), fs->line,
                                         fs->column);
    }
    case NodeType::NODE_RETURN_STMT: {
        const auto* rs = static_cast<const ReturnStmt*>(node);
        std::shared_ptr<ASTNode> val;
        if (rs->value) {
            val = cloneWithSubstitution(rs->value.get(), subst, d, unsupportedNodeName);
            if (!val)
                return nullptr;
        }
        return std::make_shared<ReturnStmt>(std::move(val), rs->line, rs->column);
    }
    case NodeType::NODE_PRINT_STMT: {
        const auto* ps = static_cast<const PrintStmt*>(node);
        std::vector<std::shared_ptr<ASTNode>> vals;
        if (!cloneList(ps->values, subst, d, unsupportedNodeName, vals))
            return nullptr;
        return std::make_shared<PrintStmt>(std::move(vals), ps->line, ps->column);
    }
    case NodeType::NODE_BREAK_STMT:
        return std::make_shared<BreakStmt>(node->line, node->column);
    case NodeType::NODE_CONTINUE_STMT:
        return std::make_shared<ContinueStmt>(node->line, node->column);
    case NodeType::NODE_THROW_STMT: {
        const auto* ts = static_cast<const ThrowStmt*>(node);
        auto expr = cloneWithSubstitution(ts->expression.get(), subst, d, unsupportedNodeName);
        if (!expr)
            return nullptr;
        return std::make_shared<ThrowStmt>(std::move(expr), ts->line, ts->column);
    }
    case NodeType::NODE_INDEX_ASSIGN: {
        const auto* ia = static_cast<const IndexAssign*>(node);
        auto obj = cloneWithSubstitution(ia->object.get(), subst, d, unsupportedNodeName);
        if (!obj)
            return nullptr;
        auto idx = cloneWithSubstitution(ia->index.get(), subst, d, unsupportedNodeName);
        if (!idx)
            return nullptr;
        auto val = cloneWithSubstitution(ia->value.get(), subst, d, unsupportedNodeName);
        if (!val)
            return nullptr;
        return std::make_shared<IndexAssign>(std::move(obj), std::move(idx), std::move(val), ia->line, ia->column);
    }
    case NodeType::NODE_MEMBER_ASSIGN: {
        const auto* ma = static_cast<const MemberAssign*>(node);
        auto obj = cloneWithSubstitution(ma->object.get(), subst, d, unsupportedNodeName);
        if (!obj)
            return nullptr;
        auto val = cloneWithSubstitution(ma->value.get(), subst, d, unsupportedNodeName);
        if (!val)
            return nullptr;
        return std::make_shared<MemberAssign>(std::move(obj), ma->fieldName, std::move(val), ma->line, ma->column);
    }
    case NodeType::NODE_TRY_STMT: {
        const auto* ts = static_cast<const TryStmt*>(node);
        auto tryB = cloneWithSubstitution(ts->tryBlock.get(), subst, d, unsupportedNodeName);
        if (!tryB)
            return nullptr;
        std::shared_ptr<ASTNode> catchB, finallyB;
        if (ts->catchBlock) {
            catchB = cloneWithSubstitution(ts->catchBlock.get(), subst, d, unsupportedNodeName);
            if (!catchB)
                return nullptr;
        }
        if (ts->finallyBlock) {
            finallyB = cloneWithSubstitution(ts->finallyBlock.get(), subst, d, unsupportedNodeName);
            if (!finallyB)
                return nullptr;
        }
        auto cloned = std::make_shared<TryStmt>(std::move(tryB), ts->catchVarName, std::move(catchB),
                                                std::move(finallyB), ts->line, ts->column);
        cloned->catchKeywordLine = ts->catchKeywordLine;
        cloned->finallyKeywordLine = ts->finallyKeywordLine;
        return cloned;
    }
    default:
        // match/yield/await/lambda/class/enum 声明等仍不在宏体支持范围
        if (unsupportedNodeName)
            *unsupportedNodeName = node->nodeName();
        return nullptr;
    }
}

} // namespace MacroExpander
