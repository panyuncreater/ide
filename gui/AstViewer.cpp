#include "gui/AstViewer.h"
#include "ast/ASTNode.h"
#include <QGraphicsRectItem>
#include <QGraphicsTextItem>
#include <QGraphicsLineItem>
#include <QWheelEvent>
#include <QScrollBar>  // G-P2-20 fix: horizontalScrollBar() 需要 QScrollBar 完整定义
#include <algorithm>

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

    // PERF-24 fix: 大 AST 节点数上限保护，避免创建 O(3N) 个 QGraphicsItem 导致 UI 卡顿。
    // 阈值 2000 节点 ≈ 6000 QGraphicsItem，是 QGraphicsScene 流畅渲染的合理上限。
    // 超过时仅渲染前 2000 节点并在场景顶部添加警告文本。
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
    sizeCache_.clear();
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
    double currentScale = transform().m11();  // 当前水平缩放因子
    if (event->angleDelta().y() > 0) {
        if (currentScale * factor <= 10.0)
            scale(factor, factor);
    } else {
        if (currentScale / factor >= 0.1)
            scale(1.0 / factor, 1.0 / factor);
    }
    event->accept();
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

    // G-P2-1 fix: 缓存 nodeName/children，避免重复调用（children() 可能返回拷贝）
    const std::string nodeNameStr = node->nodeName();
    const QString name = QString::fromStdString(nodeNameStr);

    // 节点位置：水平居中
    double nodeX = x + (availableWidth - NODE_WIDTH) / 2.0;
    double nodeY = y;

    // 绘制圆角矩形节点
    QRectF rect(nodeX, nodeY, NODE_WIDTH, NODE_HEIGHT);

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

    QGraphicsRectItem* rectItem = scene_->addRect(rect,
        QPen(QColor(100, 100, 100), 1.5), QBrush(bgColor));
    rectItem->setZValue(1);

    // 圆角效果（通过额外绘制一个圆角矩形叠加）

    // 绘制文本
    QString displayText = name;
    // 截断过长文本
    if (displayText.length() > 14) {
        displayText = displayText.left(12) + "..";
    }

    QGraphicsTextItem* textItem = scene_->addText(displayText);
    textItem->setPos(nodeX + 4, nodeY + 8);
    // G-P2-3 fix: QFont 静态化，避免每次 drawNode 都构造
    static const QFont astFont("Consolas", 8);
    textItem->setFont(astFont);
    textItem->setZValue(2);

    // 绘制子节点
    auto children = node->children();
    if (children.empty()) return;

    // 计算子节点的总宽度（从预计算缓存中查找）
    double totalChildWidth = 0;
    std::vector<SubtreeInfo> childInfos;
    childInfos.reserve(children.size());
    for (ASTNode* child : children) {
        // G-P2-2 fix: 子节点空指针检查，避免 sizeCache_[nullptr] 插入空条目
        if (!child) {
            childInfos.push_back({NODE_WIDTH, NODE_HEIGHT});
            totalChildWidth += NODE_WIDTH;
            continue;
        }
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
        // G-P2-2 fix: 跳过空子节点
        if (!child) {
            currentX += NODE_WIDTH + H_SPACING;
            continue;
        }
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
