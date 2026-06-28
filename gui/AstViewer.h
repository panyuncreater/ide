#pragma once

#include <QGraphicsView>
#include <QGraphicsScene>
#include <unordered_map>
#include <memory>
#include <vector>

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
    QGraphicsScene* scene_ = nullptr;

    /// 节点宽度
    static constexpr int NODE_WIDTH = 120;
    /// 节点高度
    static constexpr int NODE_HEIGHT = 40;
    /// 兄弟节点间距（同层相邻节点最小水平距离）
    static constexpr int SIBLING_SPACING = 20;
    /// 层与层之间的垂直间距
    static constexpr int LEVEL_SPACING = 60;
    /// 子树间距（不同子树之间额外的水平距离）
    static constexpr int SUBTREE_SPACING = 40;

    /// P1 fix: Reingold-Tilford 布局节点信息。
    /// 每个节点维护自身坐标系下的 leftContour/rightContour（相对节点中心）。
    /// 布局阶段记录轮廓，绘制阶段读取 finalX/finalY。
    struct RtNode {
        ASTNode* astNode = nullptr;
        double finalX = 0;       // 绘制坐标（父坐标系下相对父节点中心的 x 偏移）
        double finalY = 0;       // 绘制坐标（相对根节点的 y，逐层累加 LEVEL_SPACING + NODE_HEIGHT）
        std::vector<double> leftContour;   // 左轮廓：每层最左 x（相对此节点中心）
        std::vector<double> rightContour;  // 右轮廓：每层最右 x（相对此节点中心）
        std::vector<RtNode*> children;     // 拥有的子 RtNode（不管理生命周期）
    };

    /// PERF-24 fix: 递归统计 AST 节点数（用于大 AST 上限保护）
    void countNodes(ASTNode* node, int& count);

    /// P1 fix: 构建 RtNode 树（不计算坐标，仅建立结构）
    RtNode* buildRtTree(ASTNode* node);

    /// P1 fix: Reingold-Tilford 第一遍——递归布局子树。
    /// 为每个子节点分配相对父节点的 x 偏移，并合并子树轮廓。
    /// 算法核心：相邻子树通过右轮廓 vs 左轮廓的最小间距检测，平移右侧子树直至无重叠。
    void layoutSubtree(RtNode* node);

    /// P1 fix: 合并相邻两棵子树的轮廓，返回右侧子树需要的平移量。
    /// - leftNode: 左侧子树，rightNode: 右侧子树
    /// - 比较左侧右轮廓 vs 右侧左轮廓，逐层取最大重叠宽度
    /// - 所需平移 = 最大重叠 + SIBLING_SPACING
    double computeShift(const RtNode* leftNode, const RtNode* rightNode) const;

    /// P1 fix: 平移子树（轮廓与 finalX 都加 offset），并提升浅子树轮廓以对齐深层。
    void shiftSubtree(RtNode* node, double offset);

    /// P1 fix: 合并多个子树的轮廓到父节点，生成父节点的 leftContour/rightContour。
    /// 父节点轮廓 = 自身节点矩形 ± 各子树轮廓的极值。
    void buildParentContour(RtNode* node);

    /// P1 fix: 第二遍——将相对坐标递归累加为绝对坐标。
    /// node->finalX += parentX, node->finalY 由 depth 决定。
    void computeAbsoluteCoords(RtNode* node, double parentAbsX, int depth,
                               std::vector<std::pair<double, double>>& bounds);

    /// P1 fix: 第三遍——按绝对坐标绘制节点与连线（带全局偏移使最左 x=0）。
    void drawRtNode(RtNode* node, double offsetX);

    /// RtNode 内存池（避免递归 new/delete，析构时统一释放）
    std::vector<std::unique_ptr<RtNode>> rtPool_;

    /// 从内存池分配一个 RtNode
    RtNode* allocRtNode();
};
