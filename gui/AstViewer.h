#pragma once

#include <QGraphicsView>
#include <QGraphicsScene>
#include <QColor>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <vector>
#include <tuple>

class ASTNode;
class QGraphicsRectItem;

// ============================================================
// AstViewer AST 树形可视化
// ============================================================

/// AST 可视化查看器：将 AST 树渲染为图形
class AstViewer : public QGraphicsView {
    Q_OBJECT

signals:
    /// 节点被点击时发射，参数为源码行号（0表示无效）
    void nodeClicked(int line);

public:
    explicit AstViewer(QWidget* parent = nullptr);

    /// 设置 AST 根节点并渲染
    void setAst(ASTNode* root);

    /// 清空视图
    void clearAst();

    /// 主题切换：true=深色，false=浅色
    void setDarkTheme(bool dark);

protected:
    /// 鼠标滚轮：Ctrl+滚轮缩放，无修饰滚动平移，Shift+滚轮水平滚动
    void wheelEvent(QWheelEvent* event) override;

    /// 鼠标按下：记录按下位置，判断是否为点击（vs 拖动）
    void mousePressEvent(QMouseEvent* event) override;

    /// 鼠标移动：检测拖动距离以区分点击与拖动
    void mouseMoveEvent(QMouseEvent* event) override;

    /// 鼠标释放：若判定为点击且命中节点，切换展开/折叠
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    QGraphicsScene* scene_ = nullptr;
    bool isDarkTheme_ = false;
    /// 当前 AST 根节点（非拥有指针，用于主题切换时重新渲染）
    ASTNode* root_ = nullptr;

    /// 节点宽度
    static constexpr int NODE_WIDTH = 120;
    /// 节点高度
    static constexpr int NODE_HEIGHT = 40;
    /// 兄弟节点间距（同层相邻节点最小水平距离）
    static constexpr int SIBLING_SPACING = 20;
    /// 层与层之间的垂直间距
    static constexpr int LEVEL_SPACING = 60;

    /// BUG-AV-1 fix: 递归遍历最大深度保护，避免病态 AST 触发栈溢出。
    /// 超过此深度的节点自身会被处理（如计数/绘制），但其子节点不再递归。
    static constexpr int MAX_AST_DEPTH = 400;

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
        bool collapsed = false;            // 用户是否折叠了此节点
    };

    /// PERF-24 fix: 递归统计 AST 节点数（用于大 AST 上限保护）
    /// BUG-AV-1 fix: depth 参数用于深度守卫，超深度时当前节点仍计数但不再递归子节点
    void countNodes(ASTNode* node, int& count, int depth = 0);

    /// P1 fix: 构建 RtNode 树（不计算坐标，仅建立结构）
    /// BUG-AV-1 fix: depth 参数用于深度守卫
    RtNode* buildRtTree(ASTNode* node, int depth = 0);

    /// P1 fix: Reingold-Tilford 第一遍——递归布局子树。
    /// 为每个子节点分配相对父节点的 x 偏移，并合并子树轮廓。
    /// 算法核心：相邻子树通过右轮廓 vs 左轮廓的最小间距检测，平移右侧子树直至无重叠。
    /// BUG-AV-1 fix: depth 参数用于深度守卫
    void layoutSubtree(RtNode* node, int depth = 0);

    /// P1 fix: 合并相邻两棵子树的轮廓，返回右侧子树需要的平移量。
    /// - leftNode: 左侧子树，rightNode: 右侧子树
    /// - 比较左侧右轮廓 vs 右侧左轮廓，逐层取最大重叠宽度
    /// - 所需平移 = 最大重叠 + SIBLING_SPACING
    double computeShift(const RtNode* leftNode, const RtNode* rightNode) const;

    /// P1 fix: 合并多个子树的轮廓到父节点，生成父节点的 leftContour/rightContour。
    /// 父节点轮廓 = 自身节点矩形 ± 各子树轮廓的极值。
    void buildParentContour(RtNode* node);

    /// P1 fix: 第二遍——将相对坐标递归累加为绝对坐标。
    /// node->finalX += parentX, node->finalY 由 depth 决定。
    /// BUG-AV-1 fix: depth 同时作为深度守卫（超 MAX_AST_DEPTH 不再递归子节点）
    void computeAbsoluteCoords(RtNode* node, double parentAbsX, int depth,
                               std::vector<std::pair<double, double>>& bounds);

    /// P1 fix: 第三遍——按绝对坐标绘制节点与连线（带全局偏移使最左 x=0）。
    /// 折叠节点的子树不绘制，节点上显示 [+N] 折叠指示。
    /// BUG-AV-1 fix: depth 参数用于深度守卫
    void drawRtNode(RtNode* node, double offsetX, int depth = 0);

    /// RtNode 内存池（避免递归 new/delete，析构时统一释放）
    std::vector<std::unique_ptr<RtNode>> rtPool_;

    /// 从内存池分配一个 RtNode
    RtNode* allocRtNode();

    /// 折叠状态键：(line, column, nodeName, childCount) —— 在 AST 重建后仍可恢复折叠状态
    /// BUG-AV-4 fix: 增加子节点数维度，避免同位置同类型不同子节点数的节点折叠键碰撞
    using CollapseKey = std::tuple<int, int, std::string, int>;
    struct CollapseKeyHash {
        size_t operator()(const CollapseKey& k) const noexcept {
            size_t h1 = std::hash<int>{}(std::get<0>(k));
            size_t h2 = std::hash<int>{}(std::get<1>(k));
            size_t h3 = std::hash<std::string>{}(std::get<2>(k));
            size_t h4 = std::hash<int>{}(std::get<3>(k));
            return h1 ^ (h2 << 1) ^ (h3 << 2) ^ (h4 << 3);
        }
    };
    std::unordered_set<CollapseKey, CollapseKeyHash> collapsedKeys_;

    /// 点击命中检测：QGraphicsRectItem → RtNode 映射
    std::unordered_map<QGraphicsRectItem*, RtNode*> itemToNode_;

    /// 点击 vs 拖动判定状态
    QPoint pressPos_;
    bool pressWasClick_ = false;

    /// 折叠/展开后重新渲染（保留视图变换与中心点）
    void rebuildSceneKeepingView(const QTransform& savedTransform, const QPointF& savedCenter);

    /// 重置节点的布局状态（finalX/finalY/contour）以便重新布局
    /// BUG-AV-1 fix: depth 参数用于深度守卫
    void resetLayoutState(RtNode* node, int depth = 0);

    /// 生成节点的折叠键
    CollapseKey makeCollapseKey(ASTNode* node) const;

    /// 根据折叠键同步 RtNode 的 collapsed 状态
    /// BUG-AV-1 fix: depth 参数用于深度守卫
    void syncCollapsedState(RtNode* node, int depth = 0);

    /// 主题相关颜色：根据 isDarkTheme_ 返回对应配色
    QColor sceneBackgroundColor() const;
    QColor nodeBorderColor() const;
    QColor lineColor() const;
    QColor textColor() const;
    /// 节点背景色：根据节点名 + 主题
    QColor nodeBgColor(const QString& name) const;
};
