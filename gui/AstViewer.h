#pragma once

#include <QGraphicsView>
#include <QGraphicsScene>
#include <memory>

class ASTNode;

// ============================================================
// AstViewer AST 树形可视化
// ============================================================

/// AST 可视化查看器：将 AST 树渲染为图形
class AstViewer : public QGraphicsView {
    Q_OBJECT

public:
    explicit AstViewer(QWidget* parent = nullptr);

    /// 设置 AST 根节点并渲染
    void setAst(ASTNode* root);

    /// 清空视图
    void clearAst();

protected:
    /// 鼠标滚轮缩放
    void wheelEvent(QWheelEvent* event) override;

private:
    QGraphicsScene* scene_;

    /// 节点宽度
    static constexpr int NODE_WIDTH = 120;
    /// 节点高度
    static constexpr int NODE_HEIGHT = 40;
    /// 水平间距
    static constexpr int H_SPACING = 20;
    /// 垂直间距
    static constexpr int V_SPACING = 60;

    /// 子树宽度缓存
    struct SubtreeInfo {
        double width = 0;
        double height = 0;
    };

    /// 递归计算子树宽度
    SubtreeInfo computeSubtreeSize(ASTNode* node);

    /// 递归绘制 AST 节点
    void drawNode(ASTNode* node, double x, double y, double availableWidth);
};
