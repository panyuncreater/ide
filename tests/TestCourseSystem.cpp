// ============================================================
// tests/TestCourseSystem.cpp - 课程系统外部化测试（拓展二期·教学）
// ------------------------------------------------------------
// 验证 CourseSystemLibrary 的：
//   - 预设课程完整性（3 门课，每课至少 1 章 1 课）
//   - courseToJson/parseCourse 往返等价
//   - loadCourseFromFile/saveCourseToFile 磁盘 IO 往返（QTemporaryDir）
//   - coursesToJsonArray/parseCoursesArray 批量往返（QSettings 持久化格式）
//   - 错误路径（文件不存在/非法 JSON/数组含非法项）
// ============================================================
#include "gui/CourseSystemPanel.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <gtest/gtest.h>

namespace {

/// 构造一个最小合法课程
Course makeCourse(const std::string& title) {
    Course c;
    c.title = title;
    c.description = "描述：" + title;
    CourseChapter ch;
    ch.title = "第一章";
    ch.lessons.push_back({"1.1 示例课", "var x = 1;\nprint(x);", "知识点讲解", "1"});
    c.chapters.push_back(std::move(ch));
    return c;
}

/// 课程结构相等断言（结构体无 operator==，逐字段比较关键内容）
void expectCourseEq(const Course& a, const Course& b) {
    EXPECT_EQ(a.title, b.title);
    EXPECT_EQ(a.description, b.description);
    ASSERT_EQ(a.chapters.size(), b.chapters.size());
    for (size_t i = 0; i < a.chapters.size(); ++i) {
        EXPECT_EQ(a.chapters[i].title, b.chapters[i].title);
        ASSERT_EQ(a.chapters[i].lessons.size(), b.chapters[i].lessons.size());
        for (size_t j = 0; j < a.chapters[i].lessons.size(); ++j) {
            EXPECT_EQ(a.chapters[i].lessons[j].title, b.chapters[i].lessons[j].title);
            EXPECT_EQ(a.chapters[i].lessons[j].code, b.chapters[i].lessons[j].code);
            EXPECT_EQ(a.chapters[i].lessons[j].explanation, b.chapters[i].lessons[j].explanation);
            EXPECT_EQ(a.chapters[i].lessons[j].expectedOutput, b.chapters[i].lessons[j].expectedOutput);
        }
    }
}

} // namespace

TEST(CourseSystemTest, PresetCoursesComplete) {
    const auto& presets = CourseSystemLibrary::presetCourses();
    ASSERT_EQ(presets.size(), 3u);
    for (const auto& c : presets) {
        EXPECT_FALSE(c.title.empty());
        ASSERT_FALSE(c.chapters.empty());
        for (const auto& ch : c.chapters) {
            EXPECT_FALSE(ch.lessons.empty());
        }
    }
}

TEST(CourseSystemTest, JsonRoundTripSingleCourse) {
    Course original = makeCourse("往返测试课程（含中文）");
    QString json = CourseSystemLibrary::courseToJson(original);

    Course parsed;
    QString errMsg;
    ASSERT_TRUE(CourseSystemLibrary::parseCourse(json, parsed, errMsg)) << errMsg.toStdString();
    expectCourseEq(original, parsed);
}

TEST(CourseSystemTest, PresetCoursesJsonRoundTrip) {
    // 全部预设课程都能无损往返（导出即模板的前提保证）
    for (const auto& original : CourseSystemLibrary::presetCourses()) {
        QString json = CourseSystemLibrary::courseToJson(original);
        Course parsed;
        QString errMsg;
        ASSERT_TRUE(CourseSystemLibrary::parseCourse(json, parsed, errMsg))
            << original.title << ": " << errMsg.toStdString();
        expectCourseEq(original, parsed);
    }
}

TEST(CourseSystemTest, FileIoRoundTrip) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString path = QDir(tmpDir.path()).filePath("course.json");

    Course original = makeCourse("文件往返课程");
    QString errMsg;
    ASSERT_TRUE(CourseSystemLibrary::saveCourseToFile(path, original, errMsg)) << errMsg.toStdString();
    ASSERT_TRUE(QFile::exists(path));

    Course loaded;
    ASSERT_TRUE(CourseSystemLibrary::loadCourseFromFile(path, loaded, errMsg)) << errMsg.toStdString();
    expectCourseEq(original, loaded);
}

TEST(CourseSystemTest, LoadFromMissingFileFails) {
    Course out;
    QString errMsg;
    EXPECT_FALSE(CourseSystemLibrary::loadCourseFromFile(QStringLiteral("Z:/nonexistent/course.json"), out, errMsg));
    EXPECT_FALSE(errMsg.isEmpty());
}

TEST(CourseSystemTest, LoadFromInvalidJsonFileFails) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString path = QDir(tmpDir.path()).filePath("bad.json");
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
        f.write("{ 这不是合法课程 JSON !!!");
        f.close();
    }
    Course out;
    QString errMsg;
    EXPECT_FALSE(CourseSystemLibrary::loadCourseFromFile(path, out, errMsg));
    EXPECT_FALSE(errMsg.isEmpty());
}

TEST(CourseSystemTest, CoursesArrayRoundTrip) {
    std::vector<Course> original;
    original.push_back(makeCourse("课程 A"));
    original.push_back(makeCourse("课程 B"));

    QString json = CourseSystemLibrary::coursesToJsonArray(original);
    std::vector<Course> parsed;
    QString errMsg;
    ASSERT_TRUE(CourseSystemLibrary::parseCoursesArray(json, parsed, errMsg)) << errMsg.toStdString();
    ASSERT_EQ(parsed.size(), 2u);
    expectCourseEq(original[0], parsed[0]);
    expectCourseEq(original[1], parsed[1]);
}

TEST(CourseSystemTest, EmptyCoursesArrayRoundTrip) {
    QString json = CourseSystemLibrary::coursesToJsonArray({});
    std::vector<Course> parsed;
    QString errMsg;
    ASSERT_TRUE(CourseSystemLibrary::parseCoursesArray(json, parsed, errMsg)) << errMsg.toStdString();
    EXPECT_TRUE(parsed.empty());
}

TEST(CourseSystemTest, CoursesArrayRejectsInvalidItem) {
    // 数组第 2 项缺少必需字段 title → 整体失败且输出被清空（不静默丢课）
    QString json = QStringLiteral(
        "[{\"title\":\"合法课程\",\"chapters\":[]},{\"description\":\"缺 title\",\"chapters\":[]}]");
    std::vector<Course> parsed;
    QString errMsg;
    EXPECT_FALSE(CourseSystemLibrary::parseCoursesArray(json, parsed, errMsg));
    EXPECT_TRUE(parsed.empty());
    EXPECT_FALSE(errMsg.isEmpty());
}

TEST(CourseSystemTest, CoursesArrayRejectsNonArrayTopLevel) {
    std::vector<Course> parsed;
    QString errMsg;
    EXPECT_FALSE(CourseSystemLibrary::parseCoursesArray(QStringLiteral("{\"title\":\"对象非数组\"}"), parsed, errMsg));
    EXPECT_FALSE(errMsg.isEmpty());
}
