#pragma once

// ============================================================
// CourseSystemPanel — 教学课程系统面板
// ------------------------------------------------------------
// 可自定义的课程内容管理面板，用 JSON 格式（QJsonDocument）描述
// 章节 / 关卡 / 任务 / 答案校验。与 LearningPathPanel（学习路径
// 地图）互补：
//   - LearningPathPanel：固定 5 阶段 21 活动的进度跟踪
//   - CourseSystemPanel：可自定义的课程内容管理（3 个预设课程
//     + 用户自定义 JSON 课程）
//
// 两个子页：
//   1. 课程库：QComboBox 选择预设/自定义课程，QTextBrowser 展示
//      课程大纲（章节/关卡/任务/知识点），可加载课程代码到编辑器
//   2. 课程编辑器：QTextEdit 编辑课程 JSON，解析预览后可加载到
//      课程库下拉框
//
// 设计约束：
//   - 面板构造函数不创建标题栏（Ide::wrapTeachingPanel 自动包裹）
//   - setController 内联实现（仅赋值，面板不依赖控制器运行）
//   - 纯教学面板，不运行执行引擎，expectedOutput 只展示不校验
//   - 用 QJsonDocument 解析 JSON，不引入第三方 YAML 依赖
//   - 不使用 C++20 关键字 concept 作为字段名
//   - 字符串字面量中不使用中文引号 “”，用「」替代
// ============================================================

#include <QComboBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QString>
#include <QTextBrowser>
#include <QTextEdit>
#include <QWidget>
#include <string>
#include <vector>

class IdeController;

// ---- 静态教学数据结构 ----

/// 单课内容（一个关卡/任务）
struct CourseLesson {
    std::string title;          // 课程标题（如 "1.1 变量声明"）
    std::string code;           // 示例代码
    std::string explanation;    // 知识点讲解
    std::string expectedOutput; // 预期输出（只展示不校验）
};

/// 章节内容（包含多课）
struct CourseChapter {
    std::string title;                 // 章节标题
    std::vector<CourseLesson> lessons; // 章节下的课程列表
};

/// 完整课程
struct Course {
    std::string title;                   // 课程标题
    std::string description;             // 课程描述
    std::vector<CourseChapter> chapters; // 章节列表
};

/// CourseSystemLibrary — 静态教学数据 + JSON 序列化/反序列化
class CourseSystemLibrary {
public:
    /// 返回 3 个预设课程（MiniLang 入门 / 进阶语法 / 高级特性）
    static const std::vector<Course>& presetCourses();

    /// Course 转 JSON 字符串（紧凑格式，便于编辑器展示与编辑）
    static QString courseToJson(const Course& course);

    /// JSON 字符串解析为 Course，失败时返回 false 并填充 errMsg
    static bool parseCourse(const QString& json, Course& out, QString& errMsg);
};

// ---- 主面板 ----

class CourseSystemPanel : public QWidget {
    Q_OBJECT
public:
    explicit CourseSystemPanel(QWidget* parent = nullptr);

    /// 绑定 IdeController（仅赋值，面板不依赖控制器运行）
    void setController(IdeController* controller) { controller_ = controller; }

signals:
    /// 请求将样例代码加载到主编辑器
    void loadSampleRequested(const QString& code);

private slots:
    void onCourseSelected(int idx); // 课程库：下拉框切换课程
    void onLoadCourseCode();        // 课程库：加载当前课代码到编辑器
    void onParsePreview();          // 编辑器：解析 JSON 并预览
    void onLoadToLibrary();         // 编辑器：将自定义课程加入课程库

private:
    IdeController* controller_ = nullptr;

    // 子页切换
    QPushButton* pageLibraryBtn_ = nullptr;
    QPushButton* pageEditorBtn_ = nullptr;
    QStackedWidget* stack_ = nullptr;

    // 子页 1：课程库
    QComboBox* courseCombo_ = nullptr;     // 预设/自定义课程选择
    QTextBrowser* courseDetail_ = nullptr; // 课程大纲 HTML 展示
    QPushButton* loadCodeBtn_ = nullptr;   // 加载课程代码到编辑器

    // 子页 2：课程编辑器
    QTextEdit* jsonEditor_ = nullptr;         // JSON 课程编辑器
    QPushButton* parsePreviewBtn_ = nullptr;  // 解析预览按钮
    QTextBrowser* previewBrowser_ = nullptr;  // 解析预览结果
    QPushButton* loadToLibraryBtn_ = nullptr; // 加载到课程库按钮

    // 用户自定义课程（通过编辑器加载到课程库）
    std::vector<Course> customCourses_;
    // 当前选中的课程索引对应的 Course 缓存（预设 + 自定义合并视图）
    std::vector<Course> mergedCourses_;
    // 编辑器最近一次解析成功的课程（供「加载到课程库」使用）
    Course lastParsedCourse_;
    bool hasLastParsed_ = false;

    // 构造辅助
    void buildLibraryPage(QWidget* host);
    void buildEditorPage(QWidget* host);

    // 数据填充与渲染
    void populateCourses();
    void renderCourseDetail(const Course& course);
    void renderPreview(const Course& course);

    /// 合并预设课程与自定义课程到 mergedCourses_，并刷新下拉框
    void refreshMergedCourses();

    /// 将一个课程的首课代码作为加载样例（取第一章第一课）
    QString firstLessonCode(const Course& course) const;
};
