// ============================================================
// CourseSystemPanel.cpp — 教学课程系统面板实现
// ------------------------------------------------------------
// 纯教学面板，不运行任何执行引擎。包含：
//   1. 课程库：QComboBox 选择预设/自定义课程，QTextBrowser 展示
//      课程大纲（章节/关卡/任务/知识点），可加载课程代码到编辑器
//   2. 课程编辑器：QTextEdit 编辑课程 JSON，解析预览后可加载到
//      课程库下拉框
//
// 用 QJsonDocument 解析 JSON，不引入第三方 YAML 依赖。
// expectedOutput 仅展示不校验。
// ============================================================

#include "gui/CourseSystemPanel.h"

#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QSettings>
#include <QVBoxLayout>

#include <utility>

// ============================================================
// 匿名命名空间辅助函数
// ============================================================
namespace {

/// 将字符串转为 HTML 安全文本（依赖 Qt 内置转义）
QString esc(const std::string& s) {
    return QString::fromUtf8(s.c_str()).toHtmlEscaped();
}

/// 将代码文本渲染为 <pre> 块（保留换行与缩进，HTML 转义）
QString codeBlock(const std::string& code) {
    QString html = QStringLiteral("<pre style='background:#f5f5f5;padding:6px;"
                                  "border-radius:4px;font-family:Consolas,monospace;"
                                  "white-space:pre-wrap;'>%1</pre>")
                       .arg(esc(code));
    return html;
}

} // namespace

// ============================================================
// CourseSystemLibrary — 静态教学数据 + JSON 序列化/反序列化
// ============================================================

const std::vector<Course>& CourseSystemLibrary::presetCourses() {
    static const std::vector<Course> kCourses = {
        // ---- 课程 1：MiniLang 入门 ----
        {"MiniLang 入门",
         "从零开始学习 MiniLang 基础语法，涵盖变量与类型、运算符、控制流。",
         {
             {"第一章：变量与类型",
              {
                  {"1.1 变量声明", "var x = 42;\nprint(x);",
                   "使用 var 关键字声明变量，MiniLang 是动态类型语言，变量类型由赋值决定。"
                   "Value 采用 NaN-boxing（8 字节）统一表示。",
                   "42"},
                  {"1.2 数据类型",
                   "var i = 10;\nvar f = 3.14;\nvar s = \"hi\";\nvar b = "
                   "true;\nprint(i);\nprint(f);\nprint(s);\nprint(b);",
                   "MiniLang 支持整数、浮点、字符串、布尔等基本类型。"
                   "print 语句输出值并换行。",
                   "10\n3.14\nhi\ntrue"},
              }},
             {"第二章：运算符",
              {
                  {"2.1 算术运算符", "print(7 + 3);\nprint(7 - 3);\nprint(7 * 3);\nprint(7 / 2);",
                   "算术运算符 + - * /。整数除法向零截断（7 / 2 = 3），"
                   "三后端（Interpreter / StackVM / RegisterVM）语义统一。",
                   "10\n4\n21\n3"},
                  {"2.2 逻辑运算符", "print(true and false);\nprint(true or false);\nprint(not true);",
                   "and / or 短路求值并返回操作数原值（非布尔），not 返回布尔取反。"
                   "这是 MiniLang 三后端一致性的关键约束之一。",
                   "false\ntrue\nfalse"},
              }},
             {"第三章：控制流",
              {
                  {"3.1 条件分支 if/else",
                   "var x = 5;\nif (x > 3) {\n  print(\"big\");\n} else {\n  print(\"small\");\n}",
                   "if/else 根据条件真假执行不同分支，支持块语句与无花括号单语句体。", "big"},
                  {"3.2 while 循环", "var i = 0;\nwhile (i < 3) {\n  print(i);\n  i = i + 1;\n}",
                   "while 循环重复执行块直到条件为假；break/continue 可控制流程。"
                   "VM 操作数栈为定长 Value[1024]。",
                   "0\n1\n2"},
              }},
         }},

        // ---- 课程 2：进阶语法 ----
        {"进阶语法",
         "深入学习函数、闭包与面向对象，理解 MiniLang 的一等公民函数与类继承模型。",
         {
             {"第一章：函数",
              {
                  {"1.1 函数定义", "fun add(a, b) {\n  return a + b;\n}\nprint(add(2, 3));",
                   "fun 关键字定义函数，return 返回值。函数是一等公民，可作为参数与返回值传递。", "5"},
                  {"1.2 函数参数",
                   "fun greet(name, greeting) {\n  return greeting + \", \" + name;\n}\nprint(greet(\"World\", "
                   "\"Hello\"));",
                   "函数参数按位置传入，支持字符串拼接与默认参数。"
                   "调用时参数顺序必须与声明一致。",
                   "Hello, World"},
              }},
             {"第二章：闭包",
              {
                  {"2.1 闭包基础",
                   "fun makeCounter() {\n  var count = 0;\n  fun inc() {\n    count = count + 1;\n    return count;\n  "
                   "}\n  return inc;\n}\nvar c = makeCounter();\nprint(c());\nprint(c());",
                   "闭包捕获外层变量 count，每次调用 inc 累加并保留状态。"
                   "upvalue 生命周期是历史高频 Bug 模式之一。",
                   "1\n2"},
                  {"2.2 闭包捕获",
                   "fun adder(x) {\n  fun add(y) {\n    return x + y;\n  }\n  return add;\n}\nvar add5 = "
                   "adder(5);\nprint(add5(3));\nprint(add5(10));",
                   "闭包捕获参数 x，形成偏函数；不同 x 产生独立闭包，"
                   "多嵌套层捕获需注意变量快照恢复。",
                   "8\n15"},
              }},
             {"第三章：类与继承",
              {
                  {"3.1 类定义",
                   "class Animal {\n  fun speak() {\n    return \"...\";\n  }\n}\nvar a = Animal();\nprint(a.speak());",
                   "class 定义类，类名调用创建实例（无 new 关键字），点号访问方法。"
                   "类实例通过 RefCounted 基类管理生命周期。",
                   "..."},
                  {"3.2 继承与 super",
                   "class Animal {\n  fun speak() {\n    return \"sound\";\n  }\n}\nclass Dog extends Animal {\n  fun "
                   "speak() {\n    return super.speak() + \": woof\";\n  }\n}\nvar d = Dog();\nprint(d.speak());",
                   "extends 继承父类，super 调用父类方法，子类可覆盖。"
                   "super 方法查找与 fieldSlotIndex 是类继承高频 Bug 模式。",
                   "sound: woof"},
              }},
         }},

        // ---- 课程 3：高级特性 ----
        {"高级特性",
         "探索模块系统、异常处理与模式匹配，掌握 MiniLang 的高级编程范式。",
         {
             {"第一章：模块系统",
              {
                  {"1.1 import 导入",
                   "// 导入整个模块\nimport \"utils\";\n// 导入指定名称\nimport { helper } from "
                   "\"utils\";\nprint(\"imported\");",
                   "import 语句只能在顶层使用，支持导入全部或指定名称。"
                   "模块路径用字符串字面量，需注意路径安全与循环导入延迟加载。",
                   "imported"},
                  {"1.2 export 导出",
                   "// 导出变量\nexport var PI = 3.14;\n// 导出函数\nexport fun area(r) {\n  return PI * r * "
                   "r;\n}\nprint(\"exported\");",
                   "export 导出 var/fun/class 声明供其他模块 import 使用，只能在顶层。"
                   "FunDecl/ExportStmt 预扫描遗漏是模块系统高频 Bug。",
                   "exported"},
              }},
             {"第二章：异常处理",
              {
                  {"2.1 try/catch", "try {\n  throw \"oops\";\n} catch (e) {\n  print(\"caught: \" + e);\n}",
                   "try 块捕获异常，catch (e) 绑定异常值。"
                   "catch 变量作用域限于 catch 块，需注意闭包未关闭与栈残留。",
                   "caught: oops"},
                  {"2.2 throw 与 finally",
                   "fun divide(a, b) {\n  if (b == 0) {\n    throw \"div by zero\";\n  }\n  return a / b;\n}\ntry {\n  "
                   "print(divide(10, 0));\n} catch (e) {\n  print(\"error: \" + e);\n} finally {\n  "
                   "print(\"done\");\n}",
                   "throw 抛出任意值，finally 无论是否异常都执行，用于资源清理。"
                   "三后端的异常语义需保持一致。",
                   "error: div by zero\ndone"},
              }},
             {"第三章：模式匹配",
              {
                  {"3.1 match 基础",
                   "var x = 2;\nmatch (x) {\n  case 1 => { print(\"one\"); }\n  case 2 => { print(\"two\"); }\n  "
                   "default => { print(\"other\"); }\n}",
                   "match 表达式对 scrutinee 模式匹配，case 分支用 => 连接，default 为兜底。"
                   "支持字面量、通配符 _、元组、变量绑定等模式。",
                   "two"},
                  {"3.2 模式绑定",
                   "var x = 5;\nmatch (x) {\n  case 0 => { print(\"zero\"); }\n  case n => { print(n); }\n}",
                   "变量模式 n 绑定整个 scrutinee 到变量 n，可作为兜底分支。"
                   "与字面量模式组合实现分类逻辑。",
                   "5"},
              }},
         }},
    };
    return kCourses;
}

QString CourseSystemLibrary::courseToJson(const Course& course) {
    QJsonObject root;
    root["title"] = QString::fromUtf8(course.title.c_str());
    root["description"] = QString::fromUtf8(course.description.c_str());

    QJsonArray chaptersArr;
    for (const auto& ch : course.chapters) {
        QJsonObject chObj;
        chObj["title"] = QString::fromUtf8(ch.title.c_str());
        QJsonArray lessonsArr;
        for (const auto& ls : ch.lessons) {
            QJsonObject lsObj;
            lsObj["title"] = QString::fromUtf8(ls.title.c_str());
            lsObj["code"] = QString::fromUtf8(ls.code.c_str());
            lsObj["explanation"] = QString::fromUtf8(ls.explanation.c_str());
            lsObj["expectedOutput"] = QString::fromUtf8(ls.expectedOutput.c_str());
            lessonsArr.append(lsObj);
        }
        chObj["lessons"] = lessonsArr;
        chaptersArr.append(chObj);
    }
    root["chapters"] = chaptersArr;

    QJsonDocument doc(root);
    return QString::fromUtf8(doc.toJson(QJsonDocument::Indented));
}

bool CourseSystemLibrary::parseCourse(const QString& json, Course& out, QString& errMsg) {
    out = Course{};
    QJsonParseError parseErr;
    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &parseErr);
    if (parseErr.error != QJsonParseError::NoError) {
        errMsg = QStringLiteral("JSON 解析失败：%1（偏移 %2）").arg(parseErr.errorString()).arg(parseErr.offset);
        return false;
    }
    if (!doc.isObject()) {
        errMsg = QStringLiteral("JSON 顶层必须是对象");
        return false;
    }
    QJsonObject root = doc.object();

    // title（必需，字符串）
    if (!root.contains("title")) {
        errMsg = QStringLiteral("缺少必需字段：title");
        return false;
    }
    QJsonValue titleVal = root.value("title");
    if (!titleVal.isString()) {
        errMsg = QStringLiteral("字段 title 必须是字符串");
        return false;
    }
    out.title = titleVal.toString().toUtf8().constData();

    // description（可选，字符串）
    if (root.contains("description")) {
        QJsonValue descVal = root.value("description");
        if (!descVal.isString()) {
            errMsg = QStringLiteral("字段 description 必须是字符串");
            return false;
        }
        out.description = descVal.toString().toUtf8().constData();
    }

    // chapters（必需，数组）
    if (!root.contains("chapters")) {
        errMsg = QStringLiteral("缺少必需字段：chapters");
        return false;
    }
    QJsonValue chaptersVal = root.value("chapters");
    if (!chaptersVal.isArray()) {
        errMsg = QStringLiteral("字段 chapters 必须是数组");
        return false;
    }
    QJsonArray chaptersArr = chaptersVal.toArray();
    if (chaptersArr.isEmpty()) {
        errMsg = QStringLiteral("chapters 不能为空");
        return false;
    }

    for (int i = 0; i < chaptersArr.size(); ++i) {
        QJsonValue chVal = chaptersArr.at(i);
        if (!chVal.isObject()) {
            errMsg = QStringLiteral("第 %1 个 chapter 必须是对象").arg(i + 1);
            return false;
        }
        QJsonObject chObj = chVal.toObject();
        CourseChapter chapter;
        if (!chObj.contains("title") || !chObj.value("title").isString()) {
            errMsg = QStringLiteral("第 %1 个 chapter 缺少字符串字段 title").arg(i + 1);
            return false;
        }
        chapter.title = chObj.value("title").toString().toUtf8().constData();

        if (!chObj.contains("lessons") || !chObj.value("lessons").isArray()) {
            errMsg = QStringLiteral("第 %1 个 chapter 缺少数组字段 lessons").arg(i + 1);
            return false;
        }
        QJsonArray lessonsArr = chObj.value("lessons").toArray();
        for (int j = 0; j < lessonsArr.size(); ++j) {
            QJsonValue lsVal = lessonsArr.at(j);
            if (!lsVal.isObject()) {
                errMsg = QStringLiteral("第 %1 章 第 %2 课必须是对象").arg(i + 1).arg(j + 1);
                return false;
            }
            QJsonObject lsObj = lsVal.toObject();
            CourseLesson lesson;
            // 四字段校验：title / code / explanation / expectedOutput
            static const char* kRequiredFields[] = {"title", "code", "explanation", "expectedOutput"};
            for (const char* f : kRequiredFields) {
                if (!lsObj.contains(f) || !lsObj.value(f).isString()) {
                    errMsg = QStringLiteral("第 %1 章 第 %2 课缺少字符串字段 %3")
                                 .arg(i + 1)
                                 .arg(j + 1)
                                 .arg(QString::fromUtf8(f));
                    return false;
                }
            }
            lesson.title = lsObj.value("title").toString().toUtf8().constData();
            lesson.code = lsObj.value("code").toString().toUtf8().constData();
            lesson.explanation = lsObj.value("explanation").toString().toUtf8().constData();
            lesson.expectedOutput = lsObj.value("expectedOutput").toString().toUtf8().constData();
            chapter.lessons.push_back(std::move(lesson));
        }
        if (chapter.lessons.empty()) {
            errMsg = QStringLiteral("第 %1 个 chapter 的 lessons 不能为空").arg(i + 1);
            return false;
        }
        out.chapters.push_back(std::move(chapter));
    }
    return true;
}

// ============================================================
// 拓展二期：课程内容外部化（文件 IO + 批量序列化）
// ============================================================

bool CourseSystemLibrary::loadCourseFromFile(const QString& path, Course& out, QString& errMsg) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        errMsg = QStringLiteral("无法读取文件：%1（%2）").arg(path, file.errorString());
        return false;
    }
    QString json = QString::fromUtf8(file.readAll());
    file.close();
    return parseCourse(json, out, errMsg);
}

bool CourseSystemLibrary::saveCourseToFile(const QString& path, const Course& course, QString& errMsg) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        errMsg = QStringLiteral("无法写入文件：%1（%2）").arg(path, file.errorString());
        return false;
    }
    file.write(courseToJson(course).toUtf8());
    file.close();
    return true;
}

QString CourseSystemLibrary::coursesToJsonArray(const std::vector<Course>& courses) {
    QJsonArray arr;
    for (const auto& c : courses) {
        // 复用 courseToJson → 再解为 QJsonObject，保证单课程序列化格式单一事实源
        QJsonDocument doc = QJsonDocument::fromJson(courseToJson(c).toUtf8());
        if (doc.isObject())
            arr.append(doc.object());
    }
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Indented));
}

bool CourseSystemLibrary::parseCoursesArray(const QString& json, std::vector<Course>& out, QString& errMsg) {
    out.clear();
    QJsonParseError parseErr;
    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &parseErr);
    if (parseErr.error != QJsonParseError::NoError) {
        errMsg = QStringLiteral("JSON 解析失败：%1").arg(parseErr.errorString());
        return false;
    }
    if (!doc.isArray()) {
        errMsg = QStringLiteral("JSON 顶层必须是数组");
        return false;
    }
    const QJsonArray arr = doc.array();
    for (int i = 0; i < arr.size(); ++i) {
        if (!arr[i].isObject()) {
            errMsg = QStringLiteral("第 %1 项不是对象").arg(i + 1);
            out.clear();
            return false;
        }
        // 逐项复用 parseCourse 完整校验（字段类型/必需字段）
        Course c;
        QString itemErr;
        QString itemJson = QString::fromUtf8(QJsonDocument(arr[i].toObject()).toJson(QJsonDocument::Compact));
        if (!parseCourse(itemJson, c, itemErr)) {
            errMsg = QStringLiteral("第 %1 项非法：%2").arg(i + 1).arg(itemErr);
            out.clear();
            return false;
        }
        out.push_back(std::move(c));
    }
    return true;
}

// ============================================================
// CourseSystemPanel 构造
// ============================================================

CourseSystemPanel::CourseSystemPanel(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);
    outer->setSpacing(4);

    // 顶部：子页切换按钮
    auto* pageBar = new QHBoxLayout;
    pageLibraryBtn_ = new QPushButton(tr("① 课程库"));
    pageEditorBtn_ = new QPushButton(tr("② 课程编辑器"));
    pageLibraryBtn_->setCheckable(true);
    pageEditorBtn_->setCheckable(true);
    pageLibraryBtn_->setChecked(true);
    pageBar->addWidget(pageLibraryBtn_);
    pageBar->addWidget(pageEditorBtn_);
    pageBar->addStretch();
    outer->addLayout(pageBar);

    // 子页堆栈
    stack_ = new QStackedWidget;
    auto* libraryPage = new QWidget;
    auto* editorPage = new QWidget;
    buildLibraryPage(libraryPage);
    buildEditorPage(editorPage);
    stack_->addWidget(libraryPage);
    stack_->addWidget(editorPage);
    outer->addWidget(stack_, 1);

    // 信号连接：子页切换
    connect(pageLibraryBtn_, &QPushButton::toggled, this, [this](bool checked) {
        if (checked) {
            stack_->setCurrentIndex(0);
            pageEditorBtn_->setChecked(false);
        }
    });
    connect(pageEditorBtn_, &QPushButton::toggled, this, [this](bool checked) {
        if (checked) {
            stack_->setCurrentIndex(1);
            pageLibraryBtn_->setChecked(false);
        }
    });

    // 拓展二期：恢复上次会话的自定义课程（须在 populateCourses 前，
    // 否则首次合并视图不含持久化课程）
    loadCustomCoursesFromSettings();

    // 初始数据填充：合并预设课程并刷新下拉框（内部渲染首项）
    populateCourses();

    // 课程编辑器：预填一个最小模板 JSON，便于用户参照格式编辑
    static const char* kTemplateJson = "{\n"
                                       "  \"title\": \"我的自定义课程\",\n"
                                       "  \"description\": \"在此填写课程描述\",\n"
                                       "  \"chapters\": [\n"
                                       "    {\n"
                                       "      \"title\": \"第一章：入门\",\n"
                                       "      \"lessons\": [\n"
                                       "        {\n"
                                       "          \"title\": \"1.1 第一课\",\n"
                                       "          \"code\": \"var x = 1;\\nprint(x);\",\n"
                                       "          \"explanation\": \"知识点讲解\",\n"
                                       "          \"expectedOutput\": \"1\"\n"
                                       "        }\n"
                                       "      ]\n"
                                       "    }\n"
                                       "  ]\n"
                                       "}";
    jsonEditor_->setPlainText(QString::fromUtf8(kTemplateJson));
}

// ============================================================
// 子页 1：课程库
// ============================================================

void CourseSystemPanel::buildLibraryPage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    // 顶部：课程选择 + 加载按钮
    auto* topBar = new QHBoxLayout;
    topBar->addWidget(new QLabel(tr("选择课程：")));
    courseCombo_ = new QComboBox;
    courseCombo_->setMinimumWidth(280);
    topBar->addWidget(courseCombo_, 1);
    loadCodeBtn_ = new QPushButton(tr("加载课程代码到编辑器"));
    topBar->addWidget(loadCodeBtn_);
    // 拓展二期：导出当前选中课程（预设课程也可导出作为编写模板）
    exportFileBtn_ = new QPushButton(tr("导出课程..."));
    topBar->addWidget(exportFileBtn_);
    layout->addLayout(topBar);

    // 课程详情
    courseDetail_ = new QTextBrowser;
    courseDetail_->setOpenExternalLinks(false);
    layout->addWidget(courseDetail_, 1);

    // 信号连接
    connect(courseCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &CourseSystemPanel::onCourseSelected);
    connect(loadCodeBtn_, &QPushButton::clicked, this, &CourseSystemPanel::onLoadCourseCode);
    connect(exportFileBtn_, &QPushButton::clicked, this, &CourseSystemPanel::onExportCourse);
}

// ============================================================
// 子页 2：课程编辑器
// ============================================================

void CourseSystemPanel::buildEditorPage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    // 顶部提示
    layout->addWidget(new QLabel(tr("<b>课程 JSON 编辑器</b>：编辑课程 JSON，点击「解析预览」校验格式并预览大纲，"
                                    "再点击「加载到课程库」将自定义课程加入子页 1 的下拉框。")));

    // JSON 编辑器
    jsonEditor_ = new QTextEdit;
    jsonEditor_->setPlaceholderText(tr("在此编辑课程 JSON ..."));
    QFont monoFont(QStringLiteral("Consolas"));
    monoFont.setStyleHint(QFont::Monospace);
    jsonEditor_->setFont(monoFont);
    layout->addWidget(jsonEditor_, 2);

    // 按钮栏
    auto* btnBar = new QHBoxLayout;
    parsePreviewBtn_ = new QPushButton(tr("解析预览"));
    loadToLibraryBtn_ = new QPushButton(tr("加载到课程库"));
    loadToLibraryBtn_->setEnabled(false);
    // 拓展二期：从磁盘 JSON 课程包导入（外部化分发链路的入口）
    importFileBtn_ = new QPushButton(tr("从文件导入..."));
    btnBar->addWidget(importFileBtn_);
    btnBar->addWidget(parsePreviewBtn_);
    btnBar->addWidget(loadToLibraryBtn_);
    btnBar->addStretch();
    layout->addLayout(btnBar);

    // 预览结果
    layout->addWidget(new QLabel(tr("<b>预览结果</b>")));
    previewBrowser_ = new QTextBrowser;
    previewBrowser_->setHtml(tr("<p style='color:#666;'>点击「解析预览」后，课程大纲将在此展示。</p>"));
    layout->addWidget(previewBrowser_, 2);

    // 信号连接
    connect(parsePreviewBtn_, &QPushButton::clicked, this, &CourseSystemPanel::onParsePreview);
    connect(loadToLibraryBtn_, &QPushButton::clicked, this, &CourseSystemPanel::onLoadToLibrary);
    connect(importFileBtn_, &QPushButton::clicked, this, &CourseSystemPanel::onImportFromFile);
}

// ============================================================
// 数据填充与渲染
// ============================================================

void CourseSystemPanel::populateCourses() {
    // 初始仅填充预设课程；自定义课程通过 refreshMergedCourses 合并
    refreshMergedCourses();
}

void CourseSystemPanel::refreshMergedCourses() {
    mergedCourses_.clear();
    const auto& presets = CourseSystemLibrary::presetCourses();
    for (const auto& c : presets) {
        mergedCourses_.push_back(c);
    }
    for (const auto& c : customCourses_) {
        mergedCourses_.push_back(c);
    }

    // 刷新下拉框（保留当前选中索引，若仍有效）
    int prevIdx = courseCombo_->currentIndex();
    {
        // 阻塞信号：clear/addItem/setCurrentIndex 不触发 currentIndexChanged
        QSignalBlocker blocker(courseCombo_);
        courseCombo_->clear();
        for (const auto& c : mergedCourses_) {
            courseCombo_->addItem(QString::fromUtf8(c.title.c_str()));
        }
        if (prevIdx >= 0 && prevIdx < courseCombo_->count()) {
            courseCombo_->setCurrentIndex(prevIdx);
        } else if (courseCombo_->count() > 0) {
            courseCombo_->setCurrentIndex(0);
        }
    }

    // 手动渲染当前项（blocker 析构后）
    int curIdx = courseCombo_->currentIndex();
    if (curIdx >= 0 && curIdx < static_cast<int>(mergedCourses_.size())) {
        renderCourseDetail(mergedCourses_[curIdx]);
    }
}

void CourseSystemPanel::renderCourseDetail(const Course& course) {
    QString html = QStringLiteral("<h2>%1</h2>").arg(esc(course.title));
    if (!course.description.empty()) {
        html += QStringLiteral("<p style='color:#444;'>%1</p>").arg(esc(course.description));
    }

    int totalLessons = 0;
    for (const auto& ch : course.chapters) {
        totalLessons += static_cast<int>(ch.lessons.size());
    }
    html += QStringLiteral("<p style='color:#666;font-size:small;'>共 %1 章 %2 课</p>")
                .arg(static_cast<int>(course.chapters.size()))
                .arg(totalLessons);

    html += QStringLiteral("<hr>");
    for (size_t ci = 0; ci < course.chapters.size(); ++ci) {
        const auto& ch = course.chapters[ci];
        html += QStringLiteral("<h3>%1</h3>").arg(esc(ch.title));
        for (size_t li = 0; li < ch.lessons.size(); ++li) {
            const auto& ls = ch.lessons[li];
            html += QStringLiteral("<div style='margin:8px 0;padding:6px;"
                                   "border-left:3px solid #4a90d9;'>");
            html += QStringLiteral("<p><b>%1</b></p>").arg(esc(ls.title));
            html += QStringLiteral("<p><b>代码：</b></p>");
            html += codeBlock(ls.code);
            html += QStringLiteral("<p><b>知识点：</b>%1</p>").arg(esc(ls.explanation));
            html += QStringLiteral("<p><b>预期输出：</b>"
                                   "<span style='background:#e8f5e9;padding:2px 6px;"
                                   "border-radius:3px;font-family:Consolas,monospace;'>%1</span>"
                                   "<span style='color:#999;font-size:small;'>（仅展示，不校验）</span></p>")
                        .arg(esc(ls.expectedOutput));
            html += QStringLiteral("</div>");
        }
    }

    html += QStringLiteral("<hr><p style='color:#999;font-size:small;'>"
                           "提示：点击「加载课程代码到编辑器」可将首课代码加载到主编辑器。</p>");
    courseDetail_->setHtml(html);
}

void CourseSystemPanel::renderPreview(const Course& course) {
    QString html = QStringLiteral("<h3>解析成功</h3>");
    html += QStringLiteral("<p><b>课程标题：</b>%1</p>").arg(esc(course.title));
    if (!course.description.empty()) {
        html += QStringLiteral("<p><b>描述：</b>%1</p>").arg(esc(course.description));
    }

    int totalLessons = 0;
    for (const auto& ch : course.chapters) {
        totalLessons += static_cast<int>(ch.lessons.size());
    }
    html += QStringLiteral("<p><b>规模：</b>%1 章 %2 课</p>")
                .arg(static_cast<int>(course.chapters.size()))
                .arg(totalLessons);

    html += QStringLiteral("<hr>");
    for (const auto& ch : course.chapters) {
        html += QStringLiteral("<p><b>%1</b></p>").arg(esc(ch.title));
        html += QStringLiteral("<ul>");
        for (const auto& ls : ch.lessons) {
            html += QStringLiteral("<li><b>%1</b><br>"
                                   "<span style='color:#666;'>知识点：</span>%2<br>"
                                   "<span style='color:#666;'>预期输出：</span>"
                                   "<code>%3</code></li>")
                        .arg(esc(ls.title))
                        .arg(esc(ls.explanation))
                        .arg(esc(ls.expectedOutput));
        }
        html += QStringLiteral("</ul>");
    }
    previewBrowser_->setHtml(html);
}

QString CourseSystemPanel::firstLessonCode(const Course& course) const {
    if (course.chapters.empty() || course.chapters[0].lessons.empty()) {
        return QString();
    }
    return QString::fromUtf8(course.chapters[0].lessons[0].code.c_str());
}

// ============================================================
// 槽函数
// ============================================================

void CourseSystemPanel::onCourseSelected(int idx) {
    if (idx < 0 || idx >= static_cast<int>(mergedCourses_.size())) {
        return;
    }
    renderCourseDetail(mergedCourses_[idx]);
}

void CourseSystemPanel::onLoadCourseCode() {
    int idx = courseCombo_->currentIndex();
    if (idx < 0 || idx >= static_cast<int>(mergedCourses_.size())) {
        courseDetail_->append(QStringLiteral("<p style='color:red;'>请先选择一个课程。</p>"));
        return;
    }
    const Course& c = mergedCourses_[idx];
    QString code = firstLessonCode(c);
    if (code.isEmpty()) {
        courseDetail_->append(QStringLiteral("<p style='color:red;'>该课程没有可加载的课代码。</p>"));
        return;
    }
    // 加上课程来源注释，便于在主编辑器中辨识
    QString labeled = QStringLiteral("// 来自课程：%1（首课）\n").arg(esc(c.title)) + code;
    emit loadSampleRequested(labeled);
}

void CourseSystemPanel::onParsePreview() {
    QString json = jsonEditor_->toPlainText();
    if (json.trimmed().isEmpty()) {
        previewBrowser_->setHtml(QStringLiteral("<p style='color:red;'>JSON 编辑器为空，请输入课程 JSON。</p>"));
        loadToLibraryBtn_->setEnabled(false);
        hasLastParsed_ = false;
        return;
    }
    Course parsed;
    QString errMsg;
    if (!CourseSystemLibrary::parseCourse(json, parsed, errMsg)) {
        previewBrowser_->setHtml(QStringLiteral("<p style='color:red;'><b>解析失败：</b>%1</p>").arg(errMsg));
        loadToLibraryBtn_->setEnabled(false);
        hasLastParsed_ = false;
        return;
    }
    lastParsedCourse_ = std::move(parsed);
    hasLastParsed_ = true;
    renderPreview(lastParsedCourse_);
    loadToLibraryBtn_->setEnabled(true);
}

void CourseSystemPanel::onLoadToLibrary() {
    if (!hasLastParsed_) {
        previewBrowser_->append(QStringLiteral("<p style='color:red;'>请先点击「解析预览」并成功解析后再加载。</p>"));
        return;
    }
    // 避免重复加载同标题课程
    std::string newTitle = lastParsedCourse_.title;
    for (const auto& c : customCourses_) {
        if (c.title == newTitle) {
            previewBrowser_->append(QStringLiteral("<p style='color:#F57F17;'>课程库中已存在同名自定义课程「%1」，"
                                                   "未重复添加。请修改 title 后重试。</p>")
                                        .arg(esc(newTitle)));
            return;
        }
    }
    customCourses_.push_back(lastParsedCourse_);
    refreshMergedCourses();
    // 拓展二期：自定义课程入库后立即持久化（重启 IDE 不丢失）
    saveCustomCoursesToSettings();

    // 选中新加入的课程（末尾）
    int newIdx = static_cast<int>(mergedCourses_.size()) - 1;
    if (newIdx >= 0) {
        courseCombo_->setCurrentIndex(newIdx);
    }
    // 切换到课程库子页，让用户看到结果
    pageLibraryBtn_->setChecked(true);

    previewBrowser_->append(QStringLiteral("<p style='color:#2E7D32;'><b>已加载到课程库：</b>%1（共 %2 章）</p>")
                                .arg(esc(newTitle))
                                .arg(static_cast<int>(lastParsedCourse_.chapters.size())));
}

// ============================================================
// 拓展二期：文件导入/导出 + QSettings 持久化
// ============================================================

void CourseSystemPanel::onImportFromFile() {
    QString path = QFileDialog::getOpenFileName(this, tr("从文件导入课程"), QString(),
                                                tr("课程 JSON 文件 (*.json);;所有文件 (*)"));
    if (path.isEmpty())
        return;

    Course course;
    QString errMsg;
    if (!CourseSystemLibrary::loadCourseFromFile(path, course, errMsg)) {
        previewBrowser_->setHtml(
            QStringLiteral("<p style='color:red;'><b>导入失败：</b>%1</p>").arg(errMsg.toHtmlEscaped()));
        loadToLibraryBtn_->setEnabled(false);
        hasLastParsed_ = false;
        return;
    }

    // 导入成功：回填编辑器（便于二次编辑）+ 直接预览 + 允许入库
    jsonEditor_->setPlainText(CourseSystemLibrary::courseToJson(course));
    lastParsedCourse_ = std::move(course);
    hasLastParsed_ = true;
    renderPreview(lastParsedCourse_);
    loadToLibraryBtn_->setEnabled(true);
    previewBrowser_->append(QStringLiteral("<p style='color:#2E7D32;'>已从文件导入：%1，点击「加载到课程库」入库。</p>")
                                .arg(QFileInfo(path).fileName().toHtmlEscaped()));
}

void CourseSystemPanel::onExportCourse() {
    int idx = courseCombo_->currentIndex();
    if (idx < 0 || idx >= static_cast<int>(mergedCourses_.size())) {
        courseDetail_->append(QStringLiteral("<p style='color:red;'>请先选择一个课程。</p>"));
        return;
    }
    const Course& c = mergedCourses_[idx];

    // 默认文件名：课程标题.json
    QString defaultName = QString::fromUtf8(c.title.c_str()) + QStringLiteral(".json");
    QString path =
        QFileDialog::getSaveFileName(this, tr("导出课程"), defaultName, tr("课程 JSON 文件 (*.json);;所有文件 (*)"));
    if (path.isEmpty())
        return;

    QString errMsg;
    if (!CourseSystemLibrary::saveCourseToFile(path, c, errMsg)) {
        courseDetail_->append(
            QStringLiteral("<p style='color:red;'><b>导出失败：</b>%1</p>").arg(errMsg.toHtmlEscaped()));
        return;
    }
    courseDetail_->append(QStringLiteral("<p style='color:#2E7D32;'>已导出课程到：%1</p>").arg(path.toHtmlEscaped()));
}

void CourseSystemPanel::loadCustomCoursesFromSettings() {
    QSettings settings(QStringLiteral("MiniLang"), QStringLiteral("CourseSystem"));
    QString json = settings.value(QStringLiteral("customCoursesJson")).toString();
    if (json.trimmed().isEmpty())
        return;
    std::vector<Course> courses;
    QString errMsg;
    // 解析失败静默忽略（旧版本/损坏数据不阻塞面板启动），下次保存会覆盖
    if (CourseSystemLibrary::parseCoursesArray(json, courses, errMsg)) {
        customCourses_ = std::move(courses);
    }
}

void CourseSystemPanel::saveCustomCoursesToSettings() const {
    QSettings settings(QStringLiteral("MiniLang"), QStringLiteral("CourseSystem"));
    settings.setValue(QStringLiteral("customCoursesJson"), CourseSystemLibrary::coursesToJsonArray(customCourses_));
}
