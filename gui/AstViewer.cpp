#include "gui/AstViewer.h"
#include "ast/ASTNode.h"
#include "gui/GuiTextUtils.h" // Dedup-4A: monospaceFont()
#include <QGraphicsLineItem>
#include <QGraphicsRectItem>
#include <QGraphicsTextItem>
#include <QMouseEvent>
#include <QPainterPath>
#include <QScrollBar> // G-P2-20 fix: horizontalScrollBar() 需要 QScrollBar 完整定义
#include <QToolTip>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <limits>

// ============================================================
// AstViewer AST 树形可视化实现
// ------------------------------------------------------------
// P1 fix: 采用完整的 Reingold-Tilford 算法（RT, 1981），
// 替代原先的"按子树宽度均分可用空间"启发式。
//
// 第四轮迭代增强：
//   - Ctrl+滚轮缩放、无修饰滚轮平移、Shift+滚轮水平滚动
//   - 节点点击展开/折叠（折叠状态以 (line,col,name) 为键跨 AST 重建保留）
//   - 节点悬停 tooltip 显示完整信息（节点名 / 行:列 / 子节点数）
//   - 深色 / 浅色主题适配（setDarkTheme）
// ============================================================

AstViewer::AstViewer(QWidget* parent) : QGraphicsView(parent) {
    scene_ = new QGraphicsScene(this);
    scene_->setBackgroundBrush(sceneBackgroundColor());
    setScene(scene_);
    setRenderHint(QPainter::Antialiasing);
    setDragMode(QGraphicsView::ScrollHandDrag);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    // 鼠标悬停事件启用（tooltip 依赖）
    setMouseTracking(true);
    viewport()->setMouseTracking(true);
}

void AstViewer::setAst(ASTNode* root) {
    // BUG-AV-6 fix: 先用 swap 取出 itemToNode_，避免 scene_->clear() 销毁 QGraphicsRectItem
    // 后映射表中遗留悬垂键。swap 后 itemToNode_ 为空，oldMap 在场景清理后安全 clear。
    std::unordered_map<QGraphicsRectItem*, RtNode*> oldMap;
    oldMap.swap(itemToNode_);
    scene_->clear();
    rtPool_.clear();
    oldMap.clear(); // 场景已清空，oldMap 中的键已失效，仅清理表本身
    root_ = root;

    if (!root) {
        // R52-6 fix: 空状态提示，对齐 IrViewer/PipelineViewer 模式
        auto* placeholder = scene_->addText(QString::fromUtf8("（尚未解析 AST，请先在主编辑器输入代码并触发编译）"));
        placeholder->setDefaultTextColor(isDarkTheme_ ? QColor(0x93, 0xa1, 0xa1) : QColor(0x8C, 0x8C, 0x8C));
        return;
    }

    // PERF-24 fix: 大 AST 节点数上限保护，避免创建 O(3N) 个 QGraphicsItem 导致 UI 卡顿。
    const int MAX_AST_NODES = 5000;
    int nodeCount = 0;
    countNodes(root, nodeCount);
    if (nodeCount > MAX_AST_NODES) {
        QGraphicsTextItem* warning = scene_->addText(QString("AST 节点数 %1 超过上限 %2，已跳过渲染以避免 UI 卡顿。\n"
                                                             "请考虑简化代码或使用字节码视图查看。")
                                                         .arg(nodeCount)
                                                         .arg(MAX_AST_NODES));
        warning->setDefaultTextColor(isDarkTheme_ ? QColor(0xf4, 0x87, 0x71) : QColor(0xdc, 0x32, 0x2f));
        auto font = warning->font();
        font.setPointSize(12);
        font.setBold(true);
        warning->setFont(font);
        QRectF rect = scene_->itemsBoundingRect().adjusted(-30, -30, 30, 30);
        scene_->setSceneRect(rect);
        fitInView(rect, Qt::KeepAspectRatio);
        return;
    }

    // ---- P1 fix: Reingold-Tilford 布局 ----
    // Pass 1: 构建 RtNode 树结构
    RtNode* rtRoot = buildRtTree(root);
    if (!rtRoot)
        return;

    // 同步折叠状态（基于 line+col+name 键，跨 AST 重建保留）
    syncCollapsedState(rtRoot);

    // Pass 2: 递归布局（后序），填充各节点的 finalX（相对父）与 leftContour/rightContour
    layoutSubtree(rtRoot);
    // 根节点自身轮廓（顶层节点轮廓）
    buildParentContour(rtRoot);

    // Pass 3: 累加相对坐标为绝对坐标，并记录 x/y 边界用于居中
    std::vector<std::pair<double, double>> bounds; // 每层的 (minX, maxX)
    computeAbsoluteCoords(rtRoot, 0.0, 0, bounds);

    // 计算整体边界，平移使 min(x)=0（最左节点中心位于 NODE_WIDTH/2）
    double globalMinX = std::numeric_limits<double>::max();
    for (const auto& b : bounds) {
        if (b.first < globalMinX)
            globalMinX = b.first;
    }
    double shiftX = -globalMinX; // 让最左节点边缘位于 x=0

    // Pass 4: 绘制（带全局偏移 shiftX）
    drawRtNode(rtRoot, shiftX);

    // 适配视图
    QRectF rect = scene_->itemsBoundingRect().adjusted(-30, -30, 30, 30);
    scene_->setSceneRect(rect);
    fitInView(rect, Qt::KeepAspectRatio);
}

void AstViewer::countNodes(ASTNode* node, int& count, int depth) {
    if (!node)
        return;
    // BUG-AV-1 fix: 超深度时仍计数当前节点（让其计入 MAX_AST_NODES 上限触发跳过渲染），
    // 但不再递归子节点，避免病态深度 AST 触发栈溢出。
    ++count;
    if (depth > MAX_AST_DEPTH)
        return;
    auto children = node->children();
    for (ASTNode* child : children) {
        countNodes(child, count, depth + 1);
    }
}

void AstViewer::clearAst() {
    scene_->clear();
    rtPool_.clear();
    itemToNode_.clear();
    root_ = nullptr;
    // R54-12 fix: 对齐 setAst(nullptr) 的空状态提示（R52-6），让用户区分"已清空"与"未加载"
    auto* placeholder = scene_->addText(QString::fromUtf8("（尚未解析 AST，请先在主编辑器输入代码并触发编译）"));
    placeholder->setDefaultTextColor(isDarkTheme_ ? QColor(0x93, 0xa1, 0xa1) : QColor(0x8C, 0x8C, 0x8C));
}

void AstViewer::setDarkTheme(bool dark) {
    // BUG-AV-2 fix: root_ 是非拥有指针，外部 AST 失效后可能成为悬垂指针。
    // 此处先更新主题标志与背景色；若 root_ 为空则仅刷新视口，不重建场景。
    // 注意：完整防护需调用方在 AST 失效时调用 clearAst() 清空 root_，
    // 否则 root_ 仍可能为悬垂指针。本守卫仅覆盖 root_==nullptr 的常见路径。
    isDarkTheme_ = dark;
    scene_->setBackgroundBrush(sceneBackgroundColor());

    if (!root_) {
        // root_ 为空：仅更新主题标志与背景，不重建场景
        viewport()->update();
        return;
    }

    // 若已有 AST，重新渲染以应用主题配色到所有节点
    ASTNode* savedRoot = root_;
    root_ = nullptr;
    setAst(savedRoot);
}

// ============================================================
// 第四轮迭代：滚轮 / 点击 / 悬停交互
// ============================================================

void AstViewer::wheelEvent(QWheelEvent* event) {
    // 第八轮：直接滚轮缩放（无需 Ctrl），对齐 VS Code Magnus/AST 视图行为
    // Ctrl/无修饰 → 缩放；Shift → 水平滚动；Alt → 垂直滚动
    if (event->modifiers() & Qt::AltModifier) {
        QScrollBar* vBar = verticalScrollBar();
        if (vBar) {
            int delta = event->angleDelta().y();
            vBar->setValue(vBar->value() - delta);
        }
        event->accept();
        return;
    }
    if (event->modifiers() & Qt::ShiftModifier) {
        QScrollBar* hBar = horizontalScrollBar();
        if (hBar) {
            int delta = event->angleDelta().y();
            hBar->setValue(hBar->value() - delta);
        }
        event->accept();
        return;
    }
    // 默认：滚轮直接缩放（GUI-13 fix: 范围限制 0.1x ~ 10x）
    double factor = 1.15;
    double currentScale = transform().m11();
    if (event->angleDelta().y() > 0) {
        if (currentScale * factor <= 10.0)
            scale(factor, factor);
    } else {
        if (currentScale / factor >= 0.1)
            scale(1.0 / factor, 1.0 / factor);
    }
    event->accept();
}

void AstViewer::mousePressEvent(QMouseEvent* event) {
    // 左键按下：记录起点，若未拖动则视为点击（在 mouseReleaseEvent 中处理）
    // 仍交给基类以维持 ScrollHandDrag 拖拽平移行为
    if (event->button() == Qt::LeftButton) {
        pressPos_ = event->pos();
        pressWasClick_ = true;
    }
    QGraphicsView::mousePressEvent(event);
}

void AstViewer::mouseMoveEvent(QMouseEvent* event) {
    // 拖动距离超过阈值则取消"点击"判定（避免拖动平移误触发折叠）
    if (pressWasClick_ && (event->buttons() & Qt::LeftButton)) {
        QPoint diff = event->pos() - pressPos_;
        if (diff.manhattanLength() > 4) {
            pressWasClick_ = false;
        }
    }
    // 委托给基类处理鼠标移动（含 ScrollHandDrag 拖拽）
    QGraphicsView::mouseMoveEvent(event);
}

void AstViewer::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && pressWasClick_) {
        // 视为点击：检测是否命中节点
        QPointF scenePos = mapToScene(event->pos());
        QGraphicsItem* item = scene_->itemAt(scenePos, QTransform());
        if (item && item->type() == QGraphicsRectItem::Type) {
            auto it = itemToNode_.find(static_cast<QGraphicsRectItem*>(item));
            if (it != itemToNode_.end() && it->second) {
                RtNode* rt = it->second;
                if (!rt->children.empty()) {
                    // 有子节点：切换展开/折叠
                    rt->collapsed = !rt->collapsed;
                    // 同步到 collapsedKeys_ 以跨重建保留
                    CollapseKey key = makeCollapseKey(rt->astNode);
                    if (rt->collapsed) {
                        collapsedKeys_.insert(key);
                    } else {
                        collapsedKeys_.erase(key);
                    }
                    // 重新渲染（保留场景视图变换）
                    QTransform savedTransform = transform();
                    QPointF savedCenter = mapToScene(viewport()->rect().center());
                    rebuildSceneKeepingView(savedTransform, savedCenter);
                    event->accept();
                    return;
                } else {
                    // 第十二轮：叶子节点点击——发射 nodeClicked 信号，携带源码行号
                    if (rt->astNode) {
                        int line = rt->astNode->line;
                        emit nodeClicked(line);
                    }
                }
            }
        }
    }
    pressWasClick_ = false;
    QGraphicsView::mouseReleaseEvent(event);
}

void AstViewer::rebuildSceneKeepingView(const QTransform& savedTransform, const QPointF& savedCenter) {
    // 清空场景但保留 rtPool_ 与 collapsed 状态
    scene_->clear();
    itemToNode_.clear();

    if (rtPool_.empty() || !rtPool_[0])
        return;
    RtNode* rtRoot = rtPool_[0].get();

    // 重新布局（折叠后子树不参与布局，整体更紧凑）
    // 先重置轮廓与 finalX，因为上次布局的值已被修改
    resetLayoutState(rtRoot);
    layoutSubtree(rtRoot);
    buildParentContour(rtRoot);

    std::vector<std::pair<double, double>> bounds;
    computeAbsoluteCoords(rtRoot, 0.0, 0, bounds);

    double globalMinX = std::numeric_limits<double>::max();
    for (const auto& b : bounds) {
        if (b.first < globalMinX)
            globalMinX = b.first;
    }
    double shiftX = -globalMinX;

    drawRtNode(rtRoot, shiftX);

    QRectF rect = scene_->itemsBoundingRect().adjusted(-30, -30, 30, 30);
    scene_->setSceneRect(rect);

    // 恢复视图变换与中心点
    setTransform(savedTransform);
    centerOn(savedCenter);
}

void AstViewer::resetLayoutState(RtNode* node, int depth) {
    if (!node)
        return;
    node->finalX = 0;
    node->finalY = 0;
    node->leftContour.clear();
    node->rightContour.clear();
    // BUG-AV-1 fix: 超深度时不再递归子节点
    if (depth > MAX_AST_DEPTH)
        return;
    // 折叠节点的子树不重置（不参与布局）
    if (!node->collapsed) {
        for (RtNode* child : node->children) {
            resetLayoutState(child, depth + 1);
        }
    }
}

AstViewer::CollapseKey AstViewer::makeCollapseKey(ASTNode* node) const {
    if (!node)
        return CollapseKey{0, 0, std::string(), 0};
    // BUG-AV-4 fix: 折叠键增加子节点数维度，避免同位置同类型不同子节点数节点碰撞
    int childCount = 0;
    auto children = node->children();
    for (ASTNode* c : children)
        if (c)
            ++childCount;
    return CollapseKey{node->line, node->column, node->nodeName(), childCount};
}

void AstViewer::syncCollapsedState(RtNode* node, int depth) {
    if (!node)
        return;
    CollapseKey key = makeCollapseKey(node->astNode);
    node->collapsed = (collapsedKeys_.find(key) != collapsedKeys_.end());
    // BUG-AV-1 fix: 超深度时不再递归子节点
    if (depth > MAX_AST_DEPTH)
        return;
    for (RtNode* child : node->children) {
        syncCollapsedState(child, depth + 1);
    }
}

// ============================================================
// 主题配色
// ============================================================

QColor AstViewer::sceneBackgroundColor() const {
    return isDarkTheme_ ? QColor(0x1e, 0x1e, 0x1e) : QColor(0xFF, 0xFF, 0xFF);
}

QColor AstViewer::nodeBorderColor() const {
    return isDarkTheme_ ? QColor(0x55, 0x55, 0x55) : QColor(0xE0, 0xE0, 0xE0);
}

QColor AstViewer::lineColor() const {
    return isDarkTheme_ ? QColor(0x5a, 0x5a, 0x5a) : QColor(0xE0, 0xE0, 0xE0);
}

QColor AstViewer::textColor() const {
    return isDarkTheme_ ? QColor(0xd4, 0xd4, 0xd4) : QColor(0x1E, 0x1E, 0x1E);
}

QColor AstViewer::nodeBgColor(const QString& name) const {
    // 深色主题使用低饱和度深色，浅色主题使用低饱和度浅色
    // 保证文字与背景对比度 >= 4.5:1 (WCAG)
    if (name.startsWith("BinaryOp") || name.startsWith("UnaryOp")) {
        return isDarkTheme_ ? QColor(0x6b, 0x4a, 0x2a) : QColor(0xfd, 0xf0, 0xd5);
    } else if (name.startsWith("Number") || name.startsWith("String") || name.startsWith("Bool")) {
        return isDarkTheme_ ? QColor(0x2a, 0x4a, 0x6b) : QColor(0xd5, 0xec, 0xf5);
    } else if (name.startsWith("VarDecl") || name.startsWith("Assign") || name.startsWith("VarRef")) {
        return isDarkTheme_ ? QColor(0x2a, 0x55, 0x2a) : QColor(0xe8, 0xf0, 0xd0);
    } else if (name.startsWith("If") || name.startsWith("While") || name.startsWith("For")) {
        return isDarkTheme_ ? QColor(0x55, 0x2a, 0x55) : QColor(0xec, 0xdf, 0xf0);
    } else if (name.startsWith("FunDecl") || name.startsWith("FunCall")) {
        return isDarkTheme_ ? QColor(0x55, 0x55, 0x2a) : QColor(0xf5, 0xf0, 0xd0);
    } else if (name.startsWith("Return") || name.startsWith("Print")) {
        return isDarkTheme_ ? QColor(0x2a, 0x55, 0x55) : QColor(0xd5, 0xf0, 0xec);
    }
    return isDarkTheme_ ? QColor(0x38, 0x38, 0x38) : QColor(0xF5, 0xF5, 0xF5);
}

// ============================================================
// P1 fix: Reingold-Tilford 算法实现
// ============================================================

AstViewer::RtNode* AstViewer::allocRtNode() {
    rtPool_.push_back(std::make_unique<RtNode>());
    return rtPool_.back().get();
}

AstViewer::RtNode* AstViewer::buildRtTree(ASTNode* node, int depth) {
    if (!node)
        return nullptr;
    RtNode* rt = allocRtNode();
    rt->astNode = node;
    // BUG-AV-1 fix: 超深度时不再递归子节点，仅保留当前节点
    if (depth > MAX_AST_DEPTH)
        return rt;
    auto children = node->children();
    for (ASTNode* child : children) {
        if (!child)
            continue; // G-P2-2 fix: 跳过空子节点
        RtNode* rtChild = buildRtTree(child, depth + 1);
        if (rtChild)
            rt->children.push_back(rtChild);
    }
    return rt;
}

void AstViewer::layoutSubtree(RtNode* node, int depth) {
    if (!node)
        return;

    // 折叠节点：不布局子树，轮廓仅含自身
    if (node->collapsed) {
        node->finalX = 0;
        return;
    }

    // BUG-AV-1 fix: 超深度时不再递归布局子树
    if (depth > MAX_AST_DEPTH) {
        node->finalX = 0;
        return;
    }

    // 先递归布局每个子树（后序）
    for (RtNode* child : node->children) {
        layoutSubtree(child, depth + 1);
        buildParentContour(child); // 子树布局完成后构建其轮廓
    }

    // 无子节点：finalX=0（自身即子树中心），轮廓由 buildParentContour 填充
    if (node->children.empty()) {
        node->finalX = 0;
        return;
    }

    // 第一个子节点放在 x=0（相对父中心）
    // 后续子节点依次根据前一个子树的右轮廓与自身的左轮廓计算平移量
    double currentRightEdge = 0; // 已布局部分的右边界（相对父中心）
    double firstChildFinalX = 0; // 记录第一个子节点的 finalX
    double lastChildFinalX = 0;  // 记录最后一个子节点的 finalX
    for (size_t i = 0; i < node->children.size(); ++i) {
        RtNode* child = node->children[i];
        if (i == 0) {
            // 第一个子节点：finalX 即相对父中心的偏移，初始为 0
            // 但需考虑子树自身宽度：让其中心对齐父中心
            child->finalX = 0;
            firstChildFinalX = child->finalX;
            // 更新 currentRightEdge 为该子树右轮廓的最大值
            if (!child->rightContour.empty()) {
                currentRightEdge = *std::max_element(child->rightContour.begin(), child->rightContour.end());
            }
        } else {
            // 计算与上一个子树的平移量
            RtNode* prevChild = node->children[i - 1];
            double shift = computeShift(prevChild, child);
            // child 的 finalX = prevChild.finalX + shift（相对父中心）
            // 但 computeShift 返回的是 child 需要相对自身当前 finalX 的额外平移
            // 当前 child.finalX=0，故直接设为 prevChild.finalX + shift
            child->finalX = prevChild->finalX + shift;
            // Bug-9 fix: 移除 shiftSubtree(child, 0) no-op 调用。offset=0 时函数
            // 因 `if (offset != 0)` 守卫提前返回，是纯无效调用。finalX 已直接赋值，
            // 轮廓在 buildContour 阶段基于 child 中心计算，无需此平移。
            // BUG-AV-5 fix: shiftSubtree 已作为死代码删除。

            // 更新 currentRightEdge
            double childRightMax = 0;
            if (!child->rightContour.empty()) {
                childRightMax = *std::max_element(child->rightContour.begin(), child->rightContour.end());
            }
            // child 右轮廓相对 child 中心，需加上 child.finalX 转换到父坐标系
            double childAbsoluteRight = child->finalX + childRightMax;
            if (childAbsoluteRight > currentRightEdge) {
                currentRightEdge = childAbsoluteRight;
            }
        }
        lastChildFinalX = child->finalX;
    }

    // Bug1 fix: 父节点位置应在所有子节点的中间。
    // 当前子节点布局基于第一个子节点在 x=0，最后一个子节点在 lastChildFinalX。
    // 父节点应居中于 [firstChildFinalX, lastChildFinalX] 的中点。
    // 通过平移所有子节点使中点对齐到 x=0（父中心），父节点的 finalX 保持 0 即可
    // 处于子节点群的中间位置。
    if (!node->children.empty()) {
        double childrenMidX = (firstChildFinalX + lastChildFinalX) / 2.0;
        // 平移所有子节点使中点对齐到父中心（x=0）
        if (childrenMidX != 0) {
            for (RtNode* child : node->children) {
                child->finalX -= childrenMidX;
                // 同步更新子树轮廓（轮廓相对子节点中心，平移子节点不影响轮廓相对值，
                // 但 buildParentContour 会用 child.finalX 转换到父坐标系，故无需改轮廓）
            }
        }
    }

    (void)currentRightEdge; // 父轮廓由 buildParentContour 统一构建
}

double AstViewer::computeShift(const RtNode* leftNode, const RtNode* rightNode) const {
    // 比较 leftNode 的右轮廓 vs rightNode 的左轮廓（逐层）
    // 所需最小间距 = max(右轮廓[i] - 左轮廓[i]) + SIBLING_SPACING
    // 注意：轮廓相对各自节点中心，比较时需在同一坐标系。
    // 这里 leftNode 的右轮廓相对 leftNode 中心，rightNode 的左轮廓相对 rightNode 中心。
    // 平移 rightNode 使两者间距满足 SIBLING_SPACING。
    // 假设 leftNode 中心在原点，rightNode 中心平移到 x，
    // 则层 i 的间距 = (x + rightNode.leftContour[i]) - leftNode.rightContour[i]
    // 需满足 >= SIBLING_SPACING → x >= leftNode.rightContour[i] - rightNode.leftContour[i] + SIBLING_SPACING
    // 取所有层最大值。

    const auto& lc = leftNode->rightContour;
    const auto& rc = rightNode->leftContour;

    // Bug fix: 不等深子树轮廓延伸。
    // 原实现用 commonDepth = min(lc.size(), rc.size()) 只比较到浅侧长度，
    // 忽略深层比较。当左子树比右子树深时，左子树深层右轮廓可能比层 0 还靠右，
    // 但右子树（浅）只在层 0 被推开，左子树深层穿透右子树位置 → 叶子重合。
    //
    // 修复（经典 Reingold-Tilford 做法）：遍历 maxDepth 所有层，
    // 浅侧轮廓用最后一个已知值延伸——等价于浅子树在深层仍占自身最后一层的宽度。
    // 这是保守做法（可能比必要间距略大），但保证不重合。
    auto getOrExtend = [](const std::vector<double>& contour, size_t i) -> double {
        if (i < contour.size())
            return contour[i];
        if (contour.empty())
            return 0.0;
        return contour.back();
    };

    double minShift = 0;
    size_t maxDepth = std::max(lc.size(), rc.size());
    for (size_t i = 0; i < maxDepth; ++i) {
        double lcVal = getOrExtend(lc, i);
        double rcVal = getOrExtend(rc, i);
        double required = lcVal - rcVal + SIBLING_SPACING;
        if (required > minShift) {
            minShift = required;
        }
    }
    return minShift;
}

void AstViewer::buildParentContour(RtNode* node) {
    // 父节点轮廓：
    // - 层 0（自身）：leftContour[0] = -NODE_WIDTH/2, rightContour[0] = NODE_WIDTH/2
    // - 层 i+1（子树层 i）：取所有子树轮廓层 i 的最小/最大值
    //   子树轮廓相对子节点中心，需加上 child.finalX 转换到父坐标系
    node->leftContour.clear();
    node->rightContour.clear();

    // 层 0：自身节点
    node->leftContour.push_back(-NODE_WIDTH / 2.0);
    node->rightContour.push_back(NODE_WIDTH / 2.0);

    // 折叠节点：轮廓仅含自身（无子树参与）
    if (node->collapsed) {
        return;
    }

    // 计算所有子树的最大深度（决定父轮廓层数）
    size_t maxChildDepth = 0;
    for (const RtNode* child : node->children) {
        if (child->leftContour.size() > maxChildDepth) {
            maxChildDepth = child->leftContour.size();
        }
    }

    // 逐层合并子树轮廓
    for (size_t layer = 0; layer < maxChildDepth; ++layer) {
        double layerMin = std::numeric_limits<double>::max();
        double layerMax = std::numeric_limits<double>::lowest();
        bool hasValue = false;
        for (const RtNode* child : node->children) {
            if (layer < child->leftContour.size()) {
                double left = child->finalX + child->leftContour[layer];
                double right = child->finalX + child->rightContour[layer];
                if (left < layerMin)
                    layerMin = left;
                if (right > layerMax)
                    layerMax = right;
                hasValue = true;
            }
        }
        if (hasValue) {
            node->leftContour.push_back(layerMin);
            node->rightContour.push_back(layerMax);
        } else {
            // 无子树覆盖此层（理论上不应发生，因 maxChildDepth 取自子树）
            node->leftContour.push_back(0);
            node->rightContour.push_back(0);
        }
    }
}

void AstViewer::computeAbsoluteCoords(RtNode* node, double parentAbsX, int depth,
                                      std::vector<std::pair<double, double>>& bounds) {
    if (!node)
        return;
    // 绝对 x = 父绝对 x + 本节点相对父的 finalX
    double absX = parentAbsX + node->finalX;
    node->finalX = absX;
    node->finalY = depth * (NODE_HEIGHT + LEVEL_SPACING);

    // 记录边界（节点矩形 ± NODE_WIDTH/2）
    double left = absX - NODE_WIDTH / 2.0;
    double right = absX + NODE_WIDTH / 2.0;
    if (static_cast<size_t>(depth) >= bounds.size()) {
        bounds.resize(depth + 1, {std::numeric_limits<double>::max(), std::numeric_limits<double>::lowest()});
    }
    if (left < bounds[depth].first)
        bounds[depth].first = left;
    if (right > bounds[depth].second)
        bounds[depth].second = right;

    // 折叠节点：不递归子节点
    if (node->collapsed)
        return;

    // BUG-AV-1 fix: 超深度时不再递归子节点，避免栈溢出
    if (depth > MAX_AST_DEPTH)
        return;

    for (RtNode* child : node->children) {
        computeAbsoluteCoords(child, absX, depth + 1, bounds);
    }
}

void AstViewer::drawRtNode(RtNode* node, double offsetX, int depth) {
    if (!node || !node->astNode)
        return;

    // G-P2-1 fix: 缓存 nodeName，避免重复调用
    const std::string nodeNameStr = node->astNode->nodeName();
    const QString name = QString::fromStdString(nodeNameStr);

    // 节点中心坐标 = (finalX + offsetX, finalY)，矩形左上角 = 中心 - (W/2, 0)
    double centerX = node->finalX + offsetX;
    double centerY = node->finalY;
    double nodeX = centerX - NODE_WIDTH / 2.0;
    double nodeY = centerY;

    // 根据节点类型 + 主题选择颜色
    QColor bgColor = nodeBgColor(name);
    QColor borderColor = nodeBorderColor();
    QColor txtColor = textColor();

    QRectF rect(nodeX, nodeY, NODE_WIDTH, NODE_HEIGHT);
    QGraphicsRectItem* rectItem = scene_->addRect(rect, QPen(borderColor, 1.5), QBrush(bgColor));
    rectItem->setZValue(1);
    // 注册到 itemToNode_ 以支持点击命中检测
    itemToNode_[rectItem] = node;
    // 设置 tooltip：完整节点名 + 行:列 + 子节点数
    QString tooltip = QString::fromUtf8("节点: %1\n位置: %2:%3\n子节点: %4")
                          .arg(name)
                          .arg(node->astNode->line)
                          .arg(node->astNode->column)
                          .arg(static_cast<int>(node->children.size()));
    rectItem->setToolTip(tooltip);

    // 绘制文本（折叠节点附加 [+N] 指示）
    QString displayText = name;
    if (displayText.length() > 14) {
        displayText = displayText.left(12) + "..";
    }
    if (node->collapsed && !node->children.empty()) {
        displayText += QString(" [+%1]").arg(node->children.size());
    }
    QGraphicsTextItem* textItem = scene_->addText(displayText);
    textItem->setPos(nodeX + 4, nodeY + 8);
    // Dedup-4A: monospaceFont 共享缓存（原 static const QFont astFont("Consolas", 8)）
    textItem->setFont(GuiTextUtils::monospaceFont(8));
    textItem->setDefaultTextColor(txtColor);
    textItem->setZValue(2);
    // 文本也参与 tooltip（覆盖在 rect 上方时鼠标在 textItem 上）
    textItem->setToolTip(tooltip);
    // BUG-AV-3 fix: textItem 未注册到 itemToNode_，点击文字无法触发折叠。
    // 让 textItem 不可选中、不接收鼠标按钮，使点击事件穿透到下层的 rectItem，
    // 由 rectItem 的命中检测处理折叠/展开。
    textItem->setFlag(QGraphicsItem::ItemIsSelectable, false);
    textItem->setAcceptedMouseButtons(Qt::NoButton);

    // 折叠节点：不绘制子树连线与子节点
    if (node->collapsed)
        return;

    // BUG-AV-1 fix: 超深度时不再递归绘制子节点
    if (depth > MAX_AST_DEPTH)
        return;

    // 父节点底部中心
    double parentCenterX = centerX;
    double parentBottomY = nodeY + NODE_HEIGHT;

    // 递归绘制子节点 + 平滑连线（第八轮：三次贝塞尔曲线替代直线）
    for (RtNode* child : node->children) {
        if (!child || !child->astNode)
            continue;
        double childCenterX = child->finalX + offsetX;
        double childTopY = child->finalY;

        // 三次贝塞尔：控制点位于父子垂直中点，形成 S 形平滑过渡
        double midY = (parentBottomY + childTopY) / 2.0;
        QPainterPath path;
        path.moveTo(parentCenterX, parentBottomY);
        path.cubicTo(parentCenterX, midY, childCenterX, midY, childCenterX, childTopY);
        auto* lineItem = scene_->addPath(path, QPen(lineColor(), 1.5));
        lineItem->setZValue(0);

        drawRtNode(child, offsetX, depth + 1);
    }
}
