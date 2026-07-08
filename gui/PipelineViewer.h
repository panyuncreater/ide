#pragma once

#include <QWidget>
#include <QStackedWidget>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTextBrowser>
#include <QColor>
#include <QString>
#include <vector>
#include <string>
#include <sstream>
#include "ast/ASTNode.h"

// ============================================================
// PipelineViewer — 编译管线可视化面板（第一波 P0-1）
// ------------------------------------------------------------
// 顶部流程导航条：源码 → Token → AST → IR → 字节码
// 中部 QStackedWidget：根据当前步骤显示对应内容
// 底部状态条：当前光标位置 + 联动状态
//
// 设计目标：把分散的"Token 表 / AST 树 / IR / 字节码"四个子视图
// 整合为一条流水线叙事，让学习者直观看到数据如何在管线中变形。
// 本面板仅消费 IdeController 已有 API，不修改引擎层。
// ============================================================

class IdeController;

class PipelineViewer : public QWidget {
    Q_OBJECT
public:
    explicit PipelineViewer(QWidget* parent = nullptr);

    /// 绑定到 IdeController，所有数据从该 facade 获取
    void setController(IdeController* controller) { controller_ = controller; }

    /// 重新载入当前步骤的内容（用户切换 step 或编译完成后调用）
    void reloadCurrentStep();

    /// 切换到指定步骤（0=源码, 1=Token, 2=AST, 3=IR, 4=字节码）
    void switchToStep(int step);

    /// 由编辑器光标移动时联动调用，刷新当前面板的对应高亮
    void onCursorPositionChanged(int line, int column);

signals:
    void sourceLineRequested(int line);

private:
    IdeController* controller_ = nullptr;

    // 5 个步骤导航按钮
    QPushButton* stepSourceBtn_   = nullptr;
    QPushButton* stepTokenBtn_    = nullptr;
    QPushButton* stepAstBtn_      = nullptr;
    QPushButton* stepIrBtn_       = nullptr;
    QPushButton* stepBytecodeBtn_ = nullptr;

    QStackedWidget* stack_ = nullptr;

    // 步骤 0：源码（只读 QTextBrowser 显示当前编辑器内容快照）
    QTextBrowser* sourceBrowser_ = nullptr;

    // 步骤 1：Token 表格
    QTableWidget* tokenTable_ = nullptr;

    // 步骤 2：AST 文本摘要（避免依赖 AstViewer 的 QGraphicsScene）
    QTextBrowser* astSummary_ = nullptr;

    // 步骤 3：IR 文本（与 IrViewer 类似的 HTML 渲染）
    QTextBrowser* irBrowser_ = nullptr;

    // 步骤 4：字节码文本（直接 disassemble 输出）
    QTextBrowser* bytecodeBrowser_ = nullptr;

    // 底部状态条
    QLabel* statusLabel_ = nullptr;

    int currentStep_ = 0;
    int cursorLine_ = 1;
    int cursorColumn_ = 1;

    // 构造辅助
    void buildStepBar(QWidget* host);
    void buildStepPages();

    // 数据填充方法
    void populateSource();
    void populateTokens();
    void populateAstSummary();
    void populateIR();
    void populateBytecode();

    // 递归生成 AST 文本摘要
    void dumpAst(std::ostringstream& os, ASTNode* node, int depth, int maxDepth);

    // === UI 美化（第二十五轮） ===
    /// 5 阶段主题色（Solarized 色板：石墨灰 / 海蓝 / 森林绿 / 紫 / 橙）
    static QColor stageColor(int step);
    /// 5 阶段 emoji 图标（📄 / 🔢 / 🌳 / ⚙ / 📦）
    static QString stageIcon(int step);
    /// 5 阶段中文标题
    static QString stageTitle(int step);
    /// 5 阶段简短描述（header 条说明文字）
    static QString stageDesc(int step);
    /// 刷新底部状态条（带当前阶段主题色 + emoji 图标）
    void updateStatusBar();
};
