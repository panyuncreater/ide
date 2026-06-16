#include "gui/AstViewer.h"
#include "ast/ASTNode.h"
#include <QGraphicsRectItem>
#include <QGraphicsTextItem>
#include <QGraphicsLineItem>
#include <QWheelEvent>
#include <cmath>

// ============================================================
// AstViewer AST 树形可视化实现
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
    sizeCache_.clear();

    if (!root) return;

    // 单次遍历预计算所有子树尺寸（O(N) 代替 O(N²)）
    precomputeSubtreeSizes(root);

    // 从缓存中获取根节点尺寸
    SubtreeInfo info = sizeCache_[root];

    // 绘制整棵树
    drawNode(root, 0, 0, info.width);

    // 适配视图
    QRectF rect = scene_->itemsBoundingRect().adjusted(-30, -30, 30, 30);
    scene_->setSceneRect(rect);
    fitInView(rect, Qt::KeepAspectRatio);
}

void AstViewer::clearAst() {
    scene_->clear();
}

void AstViewer::wheelEvent(QWheelEvent* event) {
    double factor = 1.15;
    if (event->angleDelta().y() > 0) {
        scale(factor, factor);
    } else {
        scale(1.0 / factor, 1.0 / factor);
    }
    event->accept();
}

AstViewer::SubtreeInfo AstViewer::computeSubtreeSize(ASTNode* node) {
    if (!node) return {0, 0};

    auto children = node->children();

    if (children.empty()) {
        // 叶节点
        return {NODE_WIDTH, NODE_HEIGHT};
    }

    // 计算所有子树的宽度之和
    double totalChildWidth = 0;
    double maxChildHeight = 0;

    for (ASTNode* child : children) {
        SubtreeInfo childInfo = computeSubtreeSize(child);
        totalChildWidth += childInfo.width;
        maxChildHeight = std::max(maxChildHeight, childInfo.height);
    }

    // 加上子树之间的间距
    totalChildWidth += H_SPACING * (children.size() - 1);

    double width = std::max(static_cast<double>(NODE_WIDTH), totalChildWidth);
    double height = NODE_HEIGHT + V_SPACING + maxChildHeight;

    return {width, height};
}

void AstViewer::precomputeSubtreeSizes(ASTNode* node) {
    if (!node) return;

    auto children = node->children();

    if (children.empty()) {
        sizeCache_[node] = {NODE_WIDTH, NODE_HEIGHT};
        return;
    }

    double totalChildWidth = 0;
    double maxChildHeight = 0;

    for (ASTNode* child : children) {
        precomputeSubtreeSizes(child);
        SubtreeInfo childInfo = sizeCache_[child];
        totalChildWidth += childInfo.width;
        maxChildHeight = std::max(maxChildHeight, childInfo.height);
    }

    totalChildWidth += H_SPACING * (children.size() - 1);

    double width = std::max(static_cast<double>(NODE_WIDTH), totalChildWidth);
    double height = NODE_HEIGHT + V_SPACING + maxChildHeight;

    sizeCache_[node] = {width, height};
}

void AstViewer::drawNode(ASTNode* node, double x, double y, double availableWidth) {
    if (!node) return;

    // 节点位置：水平居中
    double nodeX = x + (availableWidth - NODE_WIDTH) / 2.0;
    double nodeY = y;

    // 绘制圆角矩形节点
    QRectF rect(nodeX, nodeY, NODE_WIDTH, NODE_HEIGHT);

    // 根据节点类型选择颜色
    QColor bgColor;
    QString name = QString::fromStdString(node->nodeName());
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

    QGraphicsRectItem* rectItem = scene_->addRect(rect,
        QPen(QColor(100, 100, 100), 1.5), QBrush(bgColor));
    rectItem->setZValue(1);

    // 圆角效果（通过额外绘制一个圆角矩形叠加）

    // 绘制文本
    QString displayText = QString::fromStdString(node->nodeName());
    // 截断过长文本
    if (displayText.length() > 14) {
        displayText = displayText.left(12) + "..";
    }

    QGraphicsTextItem* textItem = scene_->addText(displayText);
    textItem->setPos(nodeX + 4, nodeY + 8);
    QFont font("Consolas", 8);
    textItem->setFont(font);
    textItem->setZValue(2);

    // 绘制子节点
    auto children = node->children();
    if (children.empty()) return;

    // 计算子节点的总宽度（从预计算缓存中查找）
    double totalChildWidth = 0;
    std::vector<SubtreeInfo> childInfos;
    for (ASTNode* child : children) {
        SubtreeInfo info = sizeCache_[child];
        childInfos.push_back(info);
        totalChildWidth += info.width;
    }
    totalChildWidth += H_SPACING * (children.size() - 1);

    // 子节点起始位置
    double childStartX = x + (availableWidth - totalChildWidth) / 2.0;
    double childY = nodeY + NODE_HEIGHT + V_SPACING;

    // 父节点底部中心
    double parentCenterX = nodeX + NODE_WIDTH / 2.0;
    double parentBottomY = nodeY + NODE_HEIGHT;

    double currentX = childStartX;
    for (size_t i = 0; i < children.size(); ++i) {
        ASTNode* child = children[i];
        double childWidth = childInfos[i].width;

        // 子节点顶部中心
        double childCenterX = currentX + childWidth / 2.0;
        double childTopY = childY;

        // 绘制连线
        scene_->addLine(parentCenterX, parentBottomY,
                        childCenterX, childTopY,
                        QPen(QColor(150, 150, 150), 1.5))->setZValue(0);

        // 递归绘制子节点
        drawNode(child, currentX, childY, childWidth);

        currentX += childWidth + H_SPACING;
    }
}
