// ============================================================
// AstBuilderToyPanel.h — AST 节点搭建玩具面板（功能 4）
// ------------------------------------------------------------
// 通过点击/组装 AST 节点，让学习者理解「AST 是代码的结构化表示」。
// 6 道题由浅入深，核心教学时刻：第 3 题 vs 第 4 题——同样数字和运算符，
// 仅括号不同就导致完全不同的树结构（优先级 vs 括号改变结构）。
//
// UI 布局：
//   顶部：QComboBox（题目选择）+ QLabel（进度：N/6 已完成）
//   主体（左右分栏）：
//     左侧：节点工具箱（QPushButton 列表：Number / Add / Sub / Mul / Div /
//           Print / VarDecl）
//     右侧：QTreeWidget（已搭建的 AST 树，支持选中/删除）
//   底部：[✓ 检查] [👁 查看标准答案] [➡ 下一题] [🔄 清空] [🗑 删除节点] +
//         QLabel 反馈区 + QLabel 题目说明
//
// 交互逻辑（简化版，不要求复杂拖拽）：
//   - 点击工具箱节点：
//       * 若 QTreeWidget 当前无选中项 AND 树为空 → 添加为根节点
//       * 若 QTreeWidget 当前无选中项 AND 树非空 → 反馈提示先选中父节点
//       * 若 QTreeWidget 当前有选中项 → 添加为选中项的子节点
//   - 节点工具箱中：Number / VarDecl 需要弹 QInputDialog 输入值/名称；
//     Add / Sub / Mul / Div / Print 直接创建对应标签节点
//   - 选中节点 + Delete 键 / 「删除节点」按钮：移除该节点（及其子树）
//   - 检查答案：递归比对玩家树与 answer 树的拓扑结构（label + children 顺序）
//   - 查看标准答案：将 answer 树渲染到 QTreeWidget（覆盖当前内容）
//   - 下一题：切换 QComboBox 到下一题，清空画布
//   - 清空：移除画布所有节点
//
// 信号：
//   activityCompleted(levelId) — 完成某题（检查通过）时发射，
//   levelId 格式为 "ast-toy-level-N"（N=1..6），供 LearningPathPanel
//   标记活动完成使用。
//
// 关键架构决策：
//   - 使用 QTreeWidget 而非 QGraphicsScene，实现简单、测试容易
//   - AstBuilderToyPanel.cpp 依赖 Qt6::Widgets，不加入测试目标
//     （与 LearningPathPanel.cpp 设计模式一致）
//   - 静态数据放在 AstToyLevels.cpp 独立编译单元，测试目标可单独链接
//   - 不依赖 IdeController / Lexer / Parser / Interpreter，纯前端游戏
//   - 所有用户可见文本用 mlTr() 包裹（i18n）
// ============================================================

#pragma once

#include "gui/AstToyLevels.h"
#include "gui/I18n.h" // mlTr() 国际化（头文件内联使用）

#include <QComboBox>
#include <QLabel>
#include <QList>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QWidget>
#include <vector>

class IdeController; // P1-3 fix (F14): 前向声明，setController 占位接口

class AstBuilderToyPanel : public QWidget {
    Q_OBJECT
public:
    explicit AstBuilderToyPanel(QWidget* parent = nullptr);

    /// P1-3 fix (F14): 绑定 IdeController（当前保留扩展点，未来可调用真实引擎）
    /// 当前 onVerifyWithRealParser 直接调用 Lexer + Parser，不依赖 controller_
    void setController(IdeController* /*controller*/) {}

signals:
    /// 完成某题（检查通过）时发射，levelId 格式 "ast-toy-level-N"
    void activityCompleted(const QString& levelId);

private slots:
    void onLevelChanged(int index);
    void onToolboxNumber();
    void onToolboxAdd();
    void onToolboxSub();
    void onToolboxMul();
    void onToolboxDiv();
    void onToolboxPrint();
    void onToolboxVarDecl();
    void onCheck();
    void onShowAnswer();
    void onNext();
    void onClear();
    void onDeleteSelected();
    void onTreeItemChanged(QTreeWidgetItem* current);
    /// P1-3 fix (F14): 用真实 Lexer + Parser 解析 targetExpression，
    /// 把生成的 AST 树 dump 到 feedback 区，供学员对比"自己搭建的树"
    /// 与"真实编译器生成的树"。
    void onVerifyWithRealParser();

private:
    // 顶部
    QComboBox* levelCombo_ = nullptr;
    QLabel* progressLabel_ = nullptr;
    QList<QPushButton*> levelChips_; // 关卡芯片按钮栏（与 levelCombo_ 双向同步）

    // 主体
    QTreeWidget* tree_ = nullptr;

    // 工具箱按钮
    QPushButton* btnNumber_ = nullptr;
    QPushButton* btnAdd_ = nullptr;
    QPushButton* btnSub_ = nullptr;
    QPushButton* btnMul_ = nullptr;
    QPushButton* btnDiv_ = nullptr;
    QPushButton* btnPrint_ = nullptr;
    QPushButton* btnVarDecl_ = nullptr;

    // 底部
    QPushButton* checkBtn_ = nullptr;
    QPushButton* answerBtn_ = nullptr;
    QPushButton* nextBtn_ = nullptr;
    QPushButton* clearBtn_ = nullptr;
    QPushButton* deleteBtn_ = nullptr;
    QPushButton* verifyBtn_ = nullptr; ///< P1-3 fix: 用真实 Parser 验证
    QLabel* feedback_ = nullptr;
    QLabel* descLabel_ = nullptr;

    int currentIndex_ = 0;             // 当前题目在 levels() 中的索引
    bool completed_ = false;           // 当前题目是否已检查通过
    std::vector<int> completedLevels_; // 已完成的 level 编号列表

    // ---- 内部辅助 ----
    /// 添加节点：树空 + 无选中 → 添加为根；有选中 → 添加为选中项的子节点
    void addNodeWithLabel(const QString& label);

    /// 弹出 QInputDialog 询问字符串，返回是否确认（确认时填写 outValue）
    bool promptValue(const QString& title, const QString& label, QString& outValue);

    /// 把 QTreeWidget 当前内容转为 TreeNode（用于与 answer 比对）
    /// root 为最顶层 item；返回 false 表示树为空
    bool treeToTreeNode(AstToyLevel::TreeNode& out) const;

    /// 递归：QTreeWidgetItem → AstToyLevel::TreeNode
    static AstToyLevel::TreeNode itemToTreeNode(const QTreeWidgetItem* item);

    /// 递归：AstToyLevel::TreeNode → QTreeWidgetItem（用于"查看标准答案"）
    static QTreeWidgetItem* treeNodeToItem(const AstToyLevel::TreeNode& node);

    /// 递归比对两棵 TreeNode 是否拓扑一致（label 严格相等 + children 顺序一致）
    static bool treeEquals(const AstToyLevel::TreeNode& a, const AstToyLevel::TreeNode& b);

    /// 把 TreeNode 序列化为可读字符串（用于反馈区显示差异）
    static void treeNodeToString(const AstToyLevel::TreeNode& node, QString& out, int indent = 0);

    /// 刷新进度标签
    void refreshProgress();

    /// 加载指定题目索引（更新描述、清空画布）
    void loadLevel(int index);

    /// 标记当前题目完成（加入 completedLevels_ + 发射信号）
    void markCurrentCompleted();

    /// 刷新关卡芯片按钮栏的状态（current 高亮等，所有题目均解锁）
    void refreshLevelChips();
};
