#pragma once

// ============================================================
// AstVisualizerPanel — AST 可视化编辑器教学面板
// ------------------------------------------------------------
// 将 MiniLang 源码解析为 AST 并以树形结构可视化展示。与现有
// ast-toy 面板（关卡式练习）互补：ast-toy 侧重动手构建，
// 本面板侧重"源码 → AST"的可视化对应关系与节点参考查询。
//
// 面板内部独立完成 Lexer → Parser → AST 流程（不依赖
// IdeController），AST 节点基类提供 nodeName() / children()
// 接口可直接递归遍历。
//
// 两个子页：
//   1. 源码 → AST 可视化：源码输入 + 解析按钮 + QSplitter
//      (左侧 QTreeWidget AST 树形结构 + 右侧 QTextBrowser
//      选中节点详情) + 加载样例到主编辑器按钮
//   2. AST 节点参考：QSplitter(左侧节点类型分类列表 +
//      右侧选中类型详细说明)
//
// 设计约束：
//   - 面板构造函数不创建标题栏（Ide::wrapTeachingPanel 自动包裹）
//   - setController 内联实现（仅赋值，不注册监听器）
// ============================================================

#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QWidget>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class ASTNode;
class IdeController;

// ---- AST 节点参考数据结构 ----

/// 单个 AST 节点类型的教学文档条目
struct AstNodeInfo {
    std::string typeName;         // 节点类型名（如 "VarDecl" / "IfStmt" / "BinaryOp"）
    std::string category;         // 分类（如 "声明类" / "语句类" / "表达式类"）
    std::string description;      // 节点语义描述
    std::string commonAttributes; // 常见属性列表（HTML 片段，<li>...</li>）
    std::string exampleCode;      // 触发该节点类型的样例代码
};

// ---- AST 节点参考库 ----

/// AstVisualizerLibrary — 静态 AST 节点类型教学文档库 + 内置示例代码
class AstVisualizerLibrary {
public:
    /// 返回内置示例代码库（4 个示例）
    static const std::vector<std::pair<std::string, std::string>>& samples();

    /// 返回 AST 节点类型文档库（约 20+ 条，按分类分组）
    static const std::vector<AstNodeInfo>& nodeDocs();
};

// ---- 主面板 ----

class AstVisualizerPanel : public QWidget {
    Q_OBJECT
public:
    explicit AstVisualizerPanel(QWidget* parent = nullptr);
    /// 析构函数定义在 .cpp 中（astRoot_ 的 unique_ptr 析构需要 ASTNode 完整定义）
    ~AstVisualizerPanel() override;

    /// 绑定 IdeController（仅赋值，不注册监听器——与 StepExplainerPanel 模式一致）
    void setController(IdeController* controller) { controller_ = controller; }

signals:
    /// 请求将样例代码加载到主编辑器
    void loadSampleRequested(const QString& code);

private slots:
    void onParse();
    void onAstNodeSelected(QTreeWidgetItem* item, int column);
    void onNodeDocSelected(int index);
    void onLoadSample();
    void onSelectSample(int idx);

private:
    IdeController* controller_ = nullptr;

    // 子页切换
    QPushButton* pageVisualizeBtn_ = nullptr;
    QPushButton* pageReferenceBtn_ = nullptr;
    QStackedWidget* stack_ = nullptr;

    // 子页 1：源码 → AST 可视化
    QLineEdit* sourceEdit_ = nullptr;
    QPushButton* parseBtn_ = nullptr;
    QPushButton* sample1Btn_ = nullptr;
    QPushButton* sample2Btn_ = nullptr;
    QPushButton* sample3Btn_ = nullptr;
    QPushButton* sample4Btn_ = nullptr;
    QTreeWidget* astTree_ = nullptr;
    QTextBrowser* nodeDetail_ = nullptr;
    QPushButton* loadSampleBtn_ = nullptr;

    // 子页 2：AST 节点参考
    QListWidget* nodeList_ = nullptr;
    QTextBrowser* nodeDocDetail_ = nullptr;

    // 解析产物（保留 AST 树的根节点所有权，供点击查询时遍历）
    std::unique_ptr<ASTNode> astRoot_;
    // QTreeWidgetItem → 对应 ASTNode 裸指针（所有权仍由 astRoot_ 持有）
    std::vector<std::pair<QTreeWidgetItem*, ASTNode*>> itemNodeMap_;

    // 构造辅助
    void buildVisualizePage(QWidget* host);
    void buildReferencePage(QWidget* host);

    // AST 树构建与详情渲染
    void populateAstTree(ASTNode* root);
    QTreeWidgetItem* buildTreeItem(ASTNode* node);
    QString buildNodeDetailHtml(ASTNode* node);
    QStringList extractNodeProperties(ASTNode* node);
    QStringList extractChildDescriptions(ASTNode* node);
    void populateNodeDocs();
    void showNodeDoc(int index);
};
