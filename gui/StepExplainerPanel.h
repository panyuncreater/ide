#pragma once

// ============================================================
// StepExplainerPanel — 执行步骤讲解生成器教学面板
// ------------------------------------------------------------
// 为每条字节码指令生成自然语言讲解，帮助学习者理解栈式 VM 的
// 指令格式、操作数编码、栈效果与语义。面板内部独立完成
// Lexer → Parser → Compiler 流程（不依赖 IdeController）。
//
// 两个子页：
//   1. 指令讲解生成器：源码输入 + 编译 + 字节码列表（步骤/IP/OpCode/讲解摘要）
//      + 选中指令的详细讲解（HTML：指令名/操作数值/栈效果/语义/源码行号）
//   2. OpCode 分类速查：82 个 OpCode 按分类分组列表 + 选中 OpCode 详细语义
//
// 本面板仅消费自带 StepExplainerLibrary 静态文档库 + 面板内编译产物，
// 不修改引擎层，不注册 vmStateChanged 监听器。
// ============================================================

#include "compiler/Bytecode.h"

#include <QListWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTextBrowser>
#include <QWidget>
#include <cstddef>
#include <string>
#include <vector>

class IdeController;

// ---- 教学文档数据结构 ----

/// 单个 OpCode 教学文档条目
struct StepOpCodeDocEntry {
    std::string name;          // OpCode 名（如 "OP_ADD"）
    std::string category;      // 分类（如 "算术运算"）
    std::string operandFormat; // 操作数格式说明（如 "常量池索引(2B)" / "无"）
    std::string stackEffect;   // 栈效果（如 "弹出 2 个值，压入 1 个结果"）
    std::string semantics;     // 自然语言语义讲解
    std::string exampleCode;   // 触发该 OpCode 的样例代码片段
};

/// StepExplainerLibrary — 静态 OpCode 教学文档库 + 动态讲解生成器
class StepExplainerLibrary {
public:
    /// 返回 82 个 OpCode 的静态文档数据（按分类分组）
    static const std::vector<StepOpCodeDocEntry>& opCodeDocs();

    /// 为 BytecodeChunk 中 ip 处的单条指令生成动态讲解（HTML 片段）。
    /// 解析操作数字节、查表栈效果与语义、附加源码行号。
    static QString generateStepExplanation(const BytecodeChunk& chunk, size_t ip, OpCode op);
};

// ---- 主面板 ----

class StepExplainerPanel : public QWidget {
    Q_OBJECT
public:
    explicit StepExplainerPanel(QWidget* parent = nullptr);

    /// 绑定 IdeController（仅赋值，不注册监听器——与 JitVisualizerPanel 模式一致）
    void setController(IdeController* controller) { controller_ = controller; }

signals:
    /// 请求将样例代码加载到主编辑器
    void loadSampleRequested(const QString& code);

private slots:
    void onCompile();
    void onStepSelected();
    void onOpCodeSelected(int index);
    void onLoadSample();

private:
    IdeController* controller_ = nullptr;

    // 子页切换
    QPushButton* pageExplainBtn_ = nullptr;
    QPushButton* pageOpCodeBtn_ = nullptr;
    QStackedWidget* stack_ = nullptr;

    // 子页 1：指令讲解生成器
    QLineEdit* sourceEdit_ = nullptr;
    QPushButton* compileBtn_ = nullptr;
    QTableWidget* stepTable_ = nullptr;
    QTextBrowser* stepDetail_ = nullptr;
    QPushButton* loadSampleBtn_ = nullptr;

    // 子页 2：OpCode 分类速查
    QListWidget* opCodeList_ = nullptr;
    QTextBrowser* opCodeDetail_ = nullptr;
    int currentOpCodeIdx_ = -1;

    // 编译产物（用于选中行时生成详细讲解）
    struct StepInfo {
        int step = 0;     // 指令序号（从 0 起）
        size_t ip = 0;    // 字节偏移
        OpCode op;        // 操作码
        QString summary;  // 讲解摘要（操作数简述）
    };
    std::vector<StepInfo> steps_;
    BytecodeChunk lastChunk_;

    // 构造辅助
    void buildExplainPage(QWidget* host);
    void buildOpCodePage(QWidget* host);

    // 数据填充
    void populateOpCodes();
    void showOpCode(int index);
};
