#include "gui/AstViewer.h"
#include "ast/ASTNode.h"
#include "gui/GuiTextUtils.h"  // Dedup-4A: monospaceFont()
#include <QGraphicsRectItem>
#include <QGraphicsTextItem>
#include <QGraphicsLineItem>
#include <QWheelEvent>
#include <QScrollBar>  // G-P2-20 fix: horizontalScrollBar() 需要 QScrollBar 完整定义
#include <algorithm>
#include <limits>

// ============================================================
// AstViewer AST 树形可视化实现
// ------------------------------------------------------------
// P1 fix: 采用完整的 Reingold-Tilford 算法（RT, 1981），
// 替代原先的"按子树宽度均分可用空间"启发式。
//
// RT 算法核心思想：
//   1. 后序遍历：先递归布局每个子树
//   2. 自底向上合并：相邻子树通过 rightContour vs leftContour 检测重叠，
//      平移右侧子树直至无重叠（保留 SIBLING_SPACING 间距）
//   3. 轮廓追踪：每棵子树维护 leftContour/rightContour（每层最左/最右 x），
//      父节点合并子树轮廓时延伸至自身层
//
// 优势：生成的树更紧凑、节点居中对齐父节点、宽度最优；
//       原"均分可用空间"方法在深度不均时会留出过多空白。
// ============================================================

AstViewer::AstViewer(QWidget* parent)
    : QGraphicsView(parent) {
    scene_ = new QGraphicsScene(this);
    scene_->setBackgroundBrush(QColor(255, 255, 255));
    setScene(scene_);
    setRenderHint(QPainter::Antialiasing);
    setDragMode(QGraphicsView::ScrollHandDrag);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
}

void AstViewer::setAst(ASTNode* root) {
    scene_->clear();
    rtPool_.clear();

    if (!root) return;

    // PERF-24 fix: 大 AST 节点数上限保护，避免创建 O(3N) 个 QGraphicsItem 导致 UI 卡顿。
    const int MAX_AST_NODES = 2000;
    int nodeCount = 0;
    countNodes(root, nodeCount);
    if (nodeCount > MAX_AST_NODES) {
        QGraphicsTextItem* warning = scene_->addText(
            QString("AST 节点数 %1 超过上限 %2，已跳过渲染以避免 UI 卡顿。\n"
                    "请考虑简化代码或使用字节码视图查看。")
                .arg(nodeCount).arg(MAX_AST_NODES));
        warning->setDefaultTextColor(QColor(200, 0, 0));
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
    if (!rtRoot) return;

    // Pass 2: 递归布局（后序），填充各节点的 finalX（相对父）与 leftContour/rightContour
    layoutSubtree(rtRoot);
    // 根节点自身轮廓（顶层节点轮廓）
    buildParentContour(rtRoot);

    // Pass 3: 累加相对坐标为绝对坐标，并记录 x/y 边界用于居中
    std::vector<std::pair<double, double>> bounds;  // 每层的 (minX, maxX)
    computeAbsoluteCoords(rtRoot, 0.0, 0, bounds);

    // 计算整体边界，平移使 min(x)=0（最左节点中心位于 NODE_WIDTH/2）
    double globalMinX = std::numeric_limits<double>::max();
    for (const auto& b : bounds) {
        if (b.first < globalMinX) globalMinX = b.first;
    }
    double shiftX = -globalMinX;  // 让最左节点边缘位于 x=0

    // Pass 4: 绘制（带全局偏移 shiftX）
    drawRtNode(rtRoot, shiftX);

    // 适配视图
    QRectF rect = scene_->itemsBoundingRect().adjusted(-30, -30, 30, 30);
    scene_->setSceneRect(rect);
    fitInView(rect, Qt::KeepAspectRatio);
}

void AstViewer::countNodes(ASTNode* node, int& count) {
    if (!node) return;
    ++count;
    auto children = node->children();
    for (ASTNode* child : children) {
        countNodes(child, count);
    }
}

void AstViewer::clearAst() {
    scene_->clear();
    rtPool_.clear();
}

void AstViewer::wheelEvent(QWheelEvent* event) {
    // G-P2-20 fix: Shift+滚轮 → 水平滚动；无修饰 → 缩放（保持原行为）
    if (event->modifiers() & Qt::ShiftModifier) {
        QScrollBar* hBar = horizontalScrollBar();
        if (hBar) {
            int delta = event->angleDelta().y();
            hBar->setValue(hBar->value() - delta);
        }
        event->accept();
        return;
    }
    // GUI-13 fix: 缩放范围限制 (0.1x ~ 10x)
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

// ============================================================
// P1 fix: Reingold-Tilford 算法实现
// ============================================================

AstViewer::RtNode* AstViewer::allocRtNode() {
    rtPool_.push_back(std::make_unique<RtNode>());
    return rtPool_.back().get();
}

AstViewer::RtNode* AstViewer::buildRtTree(ASTNode* node) {
    if (!node) return nullptr;
    RtNode* rt = allocRtNode();
    rt->astNode = node;
    auto children = node->children();
    for (ASTNode* child : children) {
        if (!child) continue;  // G-P2-2 fix: 跳过空子节点
        RtNode* rtChild = buildRtTree(child);
        if (rtChild) rt->children.push_back(rtChild);
    }
    return rt;
}

void AstViewer::layoutSubtree(RtNode* node) {
    if (!node) return;

    // 先递归布局每个子树（后序）
    for (RtNode* child : node->children) {
        layoutSubtree(child);
        buildParentContour(child);  // 子树布局完成后构建其轮廓
    }

    // 无子节点：finalX=0（自身即子树中心），轮廓由 buildParentContour 填充
    if (node->children.empty()) {
        node->finalX = 0;
        return;
    }

    // 第一个子节点放在 x=0（相对父中心）
    // 后续子节点依次根据前一个子树的右轮廓与自身的左轮廓计算平移量
    double currentRightEdge = 0;  // 已布局部分的右边界（相对父中心）
    double firstChildFinalX = 0;  // 记录第一个子节点的 finalX
    double lastChildFinalX = 0;    // 记录最后一个子节点的 finalX
    for (size_t i = 0; i < node->children.size(); ++i) {
        RtNode* child = node->children[i];
        if (i == 0) {
            // 第一个子节点：finalX 即相对父中心的偏移，初始为 0
            // 但需考虑子树自身宽度：让其中心对齐父中心
            child->finalX = 0;
            firstChildFinalX = child->finalX;
            // 更新 currentRightEdge 为该子树右轮廓的最大值
            if (!child->rightContour.empty()) {
                currentRightEdge = *std::max_element(child->rightContour.begin(),
                                                     child->rightContour.end());
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

            // 更新 currentRightEdge
            double childRightMax = 0;
            if (!child->rightContour.empty()) {
                childRightMax = *std::max_element(child->rightContour.begin(),
                                                  child->rightContour.end());
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

    (void)currentRightEdge;  // 父轮廓由 buildParentContour 统一构建
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
        if (i < contour.size()) return contour[i];
        if (contour.empty()) return 0.0;
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

void AstViewer::shiftSubtree(RtNode* node, double offset) {
    // 平移子树：finalX 与轮廓都加 offset
    // 注意：本实现中 finalX 是相对父中心的坐标，平移子树时若 offset != 0 需更新
    // 但 computeShift 后我们直接设 child->finalX，不再调用 shiftSubtree(offset!=0)，
    // 故 offset 参数实际为 0，仅保留接口供未来扩展（如 Walker 算法居中）。
    if (offset != 0) {
        node->finalX += offset;
        for (double& v : node->leftContour) v += offset;
        for (double& v : node->rightContour) v += offset;
        for (RtNode* child : node->children) {
            shiftSubtree(child, offset);
        }
    }
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
                if (left < layerMin) layerMin = left;
                if (right > layerMax) layerMax = right;
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
    if (!node) return;
    // 绝对 x = 父绝对 x + 本节点相对父的 finalX
    double absX = parentAbsX + node->finalX;
    node->finalX = absX;
    node->finalY = depth * (NODE_HEIGHT + LEVEL_SPACING);

    // 记录边界（节点矩形 ± NODE_WIDTH/2）
    double left = absX - NODE_WIDTH / 2.0;
    double right = absX + NODE_WIDTH / 2.0;
    if (static_cast<size_t>(depth) >= bounds.size()) {
        bounds.resize(depth + 1, {std::numeric_limits<double>::max(),
                                  std::numeric_limits<double>::lowest()});
    }
    if (left < bounds[depth].first) bounds[depth].first = left;
    if (right > bounds[depth].second) bounds[depth].second = right;

    for (RtNode* child : node->children) {
        computeAbsoluteCoords(child, absX, depth + 1, bounds);
    }
}

void AstViewer::drawRtNode(RtNode* node, double offsetX) {
    if (!node || !node->astNode) return;

    // G-P2-1 fix: 缓存 nodeName，避免重复调用
    const std::string nodeNameStr = node->astNode->nodeName();
    const QString name = QString::fromStdString(nodeNameStr);

    // 节点中心坐标 = (finalX + offsetX, finalY)，矩形左上角 = 中心 - (W/2, 0)
    double centerX = node->finalX + offsetX;
    double centerY = node->finalY;
    double nodeX = centerX - NODE_WIDTH / 2.0;
    double nodeY = centerY;

    // 根据节点类型选择颜色
    QColor bgColor;
    if (name.startsWith("BinaryOp") || name.startsWith("UnaryOp")) {
        bgColor = QColor(255, 230, 200);   // 运算：浅橙
    } else if (name.startsWith("Number") || name.startsWith("String") || name.startsWith("Bool")) {
        bgColor = QColor(200, 230, 255);   // 字面量：浅蓝
    } else if (name.startsWith("VarDecl") || name.startsWith("Assign") || name.startsWith("VarRef")) {
        bgColor = QColor(200, 255, 200);   // 变量：浅绿
    } else if (name.startsWith("If") || name.startsWith("While") || name.startsWith("For")) {
        bgColor = QColor(255, 220, 255);   // 控制流：浅紫
    } else if (name.startsWith("FunDecl") || name.startsWith("FunCall")) {
        bgColor = QColor(255, 255, 200);   // 函数：浅黄
    } else if (name.startsWith("Return") || name.startsWith("Print")) {
        bgColor = QColor(220, 255, 255);   // 语句：浅青
    } else {
        bgColor = QColor(240, 240, 240);   // 默认：浅灰
    }

    QRectF rect(nodeX, nodeY, NODE_WIDTH, NODE_HEIGHT);
    QGraphicsRectItem* rectItem = scene_->addRect(rect,
        QPen(QColor(100, 100, 100), 1.5), QBrush(bgColor));
    rectItem->setZValue(1);

    // 绘制文本
    QString displayText = name;
    if (displayText.length() > 14) {
        displayText = displayText.left(12) + "..";
    }
    QGraphicsTextItem* textItem = scene_->addText(displayText);
    textItem->setPos(nodeX + 4, nodeY + 8);
    // Dedup-4A: monospaceFont 共享缓存（原 static const QFont astFont("Consolas", 8)）
    textItem->setFont(GuiTextUtils::monospaceFont(8));
    textItem->setZValue(2);

    // 父节点底部中心
    double parentCenterX = centerX;
    double parentBottomY = nodeY + NODE_HEIGHT;

    // 递归绘制子节点 + 连线
    for (RtNode* child : node->children) {
        if (!child || !child->astNode) continue;
        double childCenterX = child->finalX + offsetX;
        double childTopY = child->finalY;

        scene_->addLine(parentCenterX, parentBottomY,
                        childCenterX, childTopY,
                        QPen(QColor(150, 150, 150), 1.5))->setZValue(0);

        drawRtNode(child, offsetX);
    }
}
