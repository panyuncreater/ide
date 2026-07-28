#pragma once

// ============================================================
// ExerciseGraderPanel — 交互式练习评分教学面板
// ------------------------------------------------------------
// 学生提交代码后，面板自动运行测试用例集 + 代码风格检查 +
// 编译/运行时错误检测，生成评分报告与改进建议。与
// BackendParallelPanel（三后端对比）互补：本面板侧重「代码
// 正确性校验 + 评分反馈」，执行详情仍展示三后端结果。
//
// 单页设计：
//   - 顶部：题目选择 + 题目描述 + 代码编辑器 + 提交评分按钮
//   - 中间：测试用例表 + 三后端执行详情
//   - 底部：评分表 + 反馈报告 + 加载样例按钮
//
// 设计约束：
//   - 面板构造函数不创建标题栏（Ide::wrapTeachingPanel 自动包裹）
//   - setController 内联实现（仅赋值，不注册监听器）
//   - 三后端在主线程串行执行（避免引擎层非线程安全问题）
// ============================================================

#include <QComboBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTextBrowser>
#include <QWidget>
#include <string>
#include <vector>

class IdeController;

// ---- 数据结构 ----

/// 单个测试用例（完整可执行代码 + 期望输出）
struct TestCase {
    std::string name;           // 用例名（如 "3+5"）
    std::string code;           // 完整代码（含输入）
    std::string expectedOutput; // 期望输出
    // 拓展二期：隐藏用例（真实判题模式）——表格中不展示输入与期望输出，
    // 失败时不泄漏期望值，防止学生针对公开用例硬编码输出骗分
    bool hidden = false;
};

/// 单个练习题目
struct Exercise {
    std::string title;               // 题目标题
    std::string description;         // 题目描述（HTML）
    std::string hint;                // 提示
    std::string starterCode;         // 起始代码
    std::vector<TestCase> testCases; // 测试用例（3 个公开 + 1 个隐藏）
};

/// 评分项
struct GradeItem {
    std::string item; // 评分项
    int score = 0;    // 得分
    int maxScore = 0; // 满分
    std::string note; // 说明
};

// ---- 题库 ----

/// 预设题库（4 个题目），静态访问
class ExerciseGraderLibrary {
public:
    static const std::vector<Exercise>& exercises();
};

// ---- 主面板 ----

class ExerciseGraderPanel : public QWidget {
    Q_OBJECT
public:
    explicit ExerciseGraderPanel(QWidget* parent = nullptr);

    /// 绑定 IdeController（仅赋值，不注册监听器——与 BackendParallelPanel 模式一致）
    void setController(IdeController* controller) { controller_ = controller; }

    // ARCH-10: ExecResult 公开为接口类型，供 convertServiceResult 辅助函数使用
    // 执行结果
    struct ExecResult {
        QString output;            // 标准输出（print 输出）
        QString status;            // 状态文本（"✅ 成功" / "❌ 错误: ..."）
        bool success = false;      // 是否成功（编译+运行均无错）
        QString errorMsg;          // 错误信息（不含状态前缀图标）
        bool compileError = false; // 编译阶段错误（词法/语法/编译）
        bool runtimeError = false; // 运行时错误
    };

signals:
    /// 请求将样例代码加载到主编辑器
    void loadSampleRequested(const QString& code);

private slots:
    void onExerciseSelected(int idx);
    void onSubmitGrade();
    void onLoadSample();

private:
    IdeController* controller_ = nullptr;

    // 顶部：题目选择 + 描述 + 代码编辑
    QComboBox* exerciseCombo_ = nullptr;
    QTextBrowser* descBrowser_ = nullptr;
    QPlainTextEdit* codeEdit_ = nullptr;
    QPushButton* submitBtn_ = nullptr;

    // 中间：测试用例表 + 三后端执行详情
    QTableWidget* testTable_ = nullptr;
    QTextBrowser* execDetail_ = nullptr;

    // 底部：评分表 + 反馈报告
    QTableWidget* gradeTable_ = nullptr;
    QTextBrowser* feedbackBrowser_ = nullptr;
    QPushButton* loadSampleBtn_ = nullptr;

    // ARCH-10: ExecResult 已上移至 public 区段

    // 构建界面、填充题目
    void buildUI();
    void populateExercises();

    // 运行单个测试用例（Interpreter 后端，用于评分校验输出）
    ExecResult runTestCase(const std::string& src);

    // 代码风格检查（返回得分 0-20，note 填写说明）
    int checkStyle(const std::string& code, QString& note);

    // 渲染评分表与反馈报告
    void renderGradeReport(const std::vector<GradeItem>& items, const QString& feedback);

    // 三后端执行（用于执行详情展示，参考 BackendParallelPanel）
    ExecResult runInterpreter(const std::string& src);
    ExecResult runStackVM_IR(const std::string& src);
    ExecResult runRegVM_IR(const std::string& src);
};
