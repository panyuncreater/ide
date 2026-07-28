// ============================================================
// tests/TestPkgCli.cpp - minilang-pkg 包管理器核心逻辑测试
// ------------------------------------------------------------
// 测试 minilang_pkg 命名空间下的可测试组件：
//   - JSON 解析/序列化（parseManifest/manifestToJson）
//   - 包描述符解析（parsePackageSpec）
//   - PackageManager（清单加载/保存、安装、列表、模块路径解析）
//   - CLI 参数解析（parseArgs）
//   - CLI 命令处理（processCommand）
//   - 辅助函数（isPackageInstalled/getPackageInstallPath）
//
// 文件系统测试使用 QTemporaryDir 避免污染项目目录。
// ============================================================
#include "cli/pkg_core.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

using namespace minilang_pkg;

// ============================================================
// 辅助函数：在临时目录中创建测试用的 registry 结构
// ============================================================

namespace {

/// 创建一个测试 registry 目录结构
/// registry/
///   math-utils/
///     pkg.json
///     math-utils.mini
///   string-utils/
///     pkg.json
///     string-utils.mini
void createTestRegistry(const QString& registryPath) {
    QDir registryDir(registryPath);
    (void)registryDir.mkpath(".");

    // math-utils 包
    QDir mathDir = registryDir.filePath("math-utils");
    (void)mathDir.mkpath(".");
    {
        QFile pkgJson(mathDir.filePath("pkg.json"));
        (void)pkgJson.open(QIODevice::WriteOnly | QIODevice::Text);
        QTextStream(&pkgJson) << "{\n"
                                 "  \"name\": \"math-utils\",\n"
                                 "  \"version\": \"1.0.0\",\n"
                                 "  \"description\": \"Math utilities\"\n"
                                 "}";
        pkgJson.close();

        QFile miniFile(mathDir.filePath("math-utils.mini"));
        (void)miniFile.open(QIODevice::WriteOnly | QIODevice::Text);
        QTextStream(&miniFile) << "export fun add(a, b) { return a + b; }\n"
                                  "export var PI = 3.14159;\n";
        miniFile.close();
    }

    // string-utils 包
    QDir strDir = registryDir.filePath("string-utils");
    (void)strDir.mkpath(".");
    {
        QFile pkgJson(strDir.filePath("pkg.json"));
        (void)pkgJson.open(QIODevice::WriteOnly | QIODevice::Text);
        QTextStream(&pkgJson) << "{\n"
                                 "  \"name\": \"string-utils\",\n"
                                 "  \"version\": \"2.1.0\",\n"
                                 "  \"description\": \"String utilities\"\n"
                                 "}";
        pkgJson.close();

        QFile miniFile(strDir.filePath("string-utils.mini"));
        (void)miniFile.open(QIODevice::WriteOnly | QIODevice::Text);
        QTextStream(&miniFile) << "export fun greet(name) { return \"Hello, \" + name; }\n";
        miniFile.close();
    }
}

/// 创建一个带版本子目录的 registry
/// registry/
///   ver-pkg/
///     1.0.0/
///       pkg.json
///       ver-pkg.mini
///     2.0.0/
///       pkg.json
///       ver-pkg.mini
void createVersionedRegistry(const QString& registryPath) {
    QDir registryDir(registryPath);
    (void)registryDir.mkpath(".");

    for (const char* ver : {"1.0.0", "2.0.0"}) {
        QDir verDir = registryDir.filePath(QString("ver-pkg/%1").arg(ver));
        (void)verDir.mkpath(".");
        QFile pkgJson(verDir.filePath("pkg.json"));
        (void)pkgJson.open(QIODevice::WriteOnly | QIODevice::Text);
        QTextStream(&pkgJson) << QString("{\n  \"name\": \"ver-pkg\",\n  \"version\": \"%1\"\n}").arg(ver);
        pkgJson.close();

        QFile miniFile(verDir.filePath("ver-pkg.mini"));
        (void)miniFile.open(QIODevice::WriteOnly | QIODevice::Text);
        QTextStream(&miniFile) << QString("export var version = \"%1\";\n").arg(ver);
        miniFile.close();
    }
}

/// 写入文件辅助
void writeFile(const QString& path, const std::string& content) {
    QFile f(path);
    (void)f.open(QIODevice::WriteOnly | QIODevice::Text);
    QTextStream(&f) << QString::fromStdString(content);
    f.close();
}

} // namespace

// ============================================================
// 1. JSON 解析/序列化
// ============================================================

TEST(PkgJsonTest, ParseValidManifest) {
    std::string json = R"({
        "name": "test-project",
        "version": "1.2.3",
        "description": "A test project",
        "dependencies": [
            { "name": "math-utils", "version": "1.0.0" },
            { "name": "string-utils", "version": "2.1.0", "source": "registry:string-utils" }
        ]
    })";
    std::string error;
    Manifest m = parseManifest(json, &error);
    EXPECT_TRUE(error.empty()) << "error: " << error;
    EXPECT_EQ(m.name, "test-project");
    EXPECT_EQ(m.version, "1.2.3");
    EXPECT_EQ(m.description, "A test project");
    ASSERT_EQ(m.dependencies.size(), 2u);
    EXPECT_EQ(m.dependencies[0].name, "math-utils");
    EXPECT_EQ(m.dependencies[0].version, "1.0.0");
    EXPECT_EQ(m.dependencies[1].name, "string-utils");
    EXPECT_EQ(m.dependencies[1].version, "2.1.0");
    EXPECT_EQ(m.dependencies[1].source, "registry:string-utils");
}

TEST(PkgJsonTest, ParseManifestWithDefaults) {
    std::string json = R"({ "name": "minimal" })";
    std::string error;
    Manifest m = parseManifest(json, &error);
    EXPECT_TRUE(error.empty()) << error;
    EXPECT_EQ(m.name, "minimal");
    EXPECT_EQ(m.version, "0.1.0"); // 默认版本
    EXPECT_TRUE(m.description.empty());
    EXPECT_TRUE(m.dependencies.empty());
}

TEST(PkgJsonTest, ParseInvalidJsonReturnsError) {
    std::string json = "{ invalid json }";
    std::string error;
    Manifest m = parseManifest(json, &error);
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(m.name.empty());
}

TEST(PkgJsonTest, ParseNonObjectRootReturnsError) {
    std::string json = "[1, 2, 3]";
    std::string error;
    Manifest m = parseManifest(json, &error);
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(m.name.empty());
}

TEST(PkgJsonTest, ParseManifestMissingNameReturnsError) {
    std::string json = R"({ "version": "1.0.0" })";
    std::string error;
    Manifest m = parseManifest(json, &error);
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(error, "清单文件缺少 name 字段");
}

TEST(PkgJsonTest, ManifestToJsonRoundTrip) {
    Manifest original;
    original.name = "round-trip";
    original.version = "3.1.4";
    original.description = "Round trip test";
    original.dependencies.push_back({"dep1", "1.0.0", "registry:dep1", "First dep"});
    original.dependencies.push_back({"dep2", "", "", ""});

    std::string json = manifestToJson(original);
    std::string error;
    Manifest parsed = parseManifest(json, &error);

    EXPECT_TRUE(error.empty()) << error;
    EXPECT_EQ(original, parsed);
}

TEST(PkgJsonTest, ManifestToJsonOmitsEmptyFields) {
    Manifest m;
    m.name = "test";
    m.version = "0.1.0";
    // description 和 dependencies 为空

    std::string json = manifestToJson(m);
    // 不应包含 description 字段（空时不输出）
    EXPECT_EQ(json.find("description"), std::string::npos);
    // 空 dependencies 应被省略（不输出 "dependencies" 字段和 "[]"）
    EXPECT_EQ(json.find("dependencies"), std::string::npos);
    EXPECT_EQ(json.find("[]"), std::string::npos);
}

TEST(PkgJsonTest, ParseManifestSkipsInvalidDependencyEntries) {
    std::string json = R"({
        "name": "test",
        "dependencies": [
            { "name": "valid-dep", "version": "1.0.0" },
            { "version": "no-name" },
            "not-an-object",
            { "name": "another-valid" }
        ]
    })";
    std::string error;
    Manifest m = parseManifest(json, &error);
    EXPECT_TRUE(error.empty()) << error;
    ASSERT_EQ(m.dependencies.size(), 2u);
    EXPECT_EQ(m.dependencies[0].name, "valid-dep");
    EXPECT_EQ(m.dependencies[1].name, "another-valid");
}

// ============================================================
// 2. 包描述符解析
// ============================================================

TEST(PkgPackageSpecTest, ParseNameOnly) {
    PackageInfo info = parsePackageSpec("math-utils");
    EXPECT_EQ(info.name, "math-utils");
    EXPECT_TRUE(info.version.empty());
    EXPECT_EQ(info.source, "registry:math-utils");
}

TEST(PkgPackageSpecTest, ParseNameWithVersion) {
    PackageInfo info = parsePackageSpec("math-utils@1.2.3");
    EXPECT_EQ(info.name, "math-utils");
    EXPECT_EQ(info.version, "1.2.3");
    EXPECT_EQ(info.source, "registry:math-utils");
}

TEST(PkgPackageSpecTest, ParseFileProtocol) {
    PackageInfo info = parsePackageSpec("file:./libs/my-lib");
    EXPECT_EQ(info.source, "file:./libs/my-lib");
    EXPECT_EQ(info.name, "my-lib");
    EXPECT_TRUE(info.version.empty());
}

TEST(PkgPackageSpecTest, ParseFileProtocolAbsolutePath) {
    PackageInfo info = parsePackageSpec("file:/absolute/path/to/pkg");
    EXPECT_EQ(info.source, "file:/absolute/path/to/pkg");
    EXPECT_EQ(info.name, "pkg");
}

TEST(PkgPackageSpecTest, ParseNameWithMultipleAtSigns) {
    // name@version 中 version 可以含 @
    PackageInfo info = parsePackageSpec("pkg@1.0.0@beta");
    EXPECT_EQ(info.name, "pkg");
    EXPECT_EQ(info.version, "1.0.0@beta");
}

// ============================================================
// 3. 辅助函数
// ============================================================

TEST(PkgHelpersTest, IsPackageInstalledReturnsFalseForEmpty) {
    EXPECT_FALSE(isPackageInstalled("", "pkg"));
    EXPECT_FALSE(isPackageInstalled("some-dir", ""));
    EXPECT_FALSE(isPackageInstalled("nonexistent-dir-12345", "pkg"));
}

TEST(PkgHelpersTest, IsPackageInstalledReturnsTrueAfterInstall) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString packagesDir = tmpDir.path();

    // 创建包目录和 pkg.json
    QDir pkgDir = QDir(packagesDir).filePath("test-pkg");
    (void)pkgDir.mkpath(".");
    QFile pkgJson(pkgDir.filePath("pkg.json"));
    (void)pkgJson.open(QIODevice::WriteOnly);
    pkgJson.write("{}");
    pkgJson.close();

    EXPECT_TRUE(isPackageInstalled(packagesDir.toStdString(), "test-pkg"));
    EXPECT_FALSE(isPackageInstalled(packagesDir.toStdString(), "not-installed"));
}

TEST(PkgHelpersTest, GetPackageInstallPath) {
    std::string path = getPackageInstallPath("/some/dir", "my-pkg");
    EXPECT_NE(path.find("my-pkg"), std::string::npos);
}

TEST(PkgHelpersTest, VersionStringContainsName) {
    std::string v = versionString();
    EXPECT_NE(v.find("minilang-pkg"), std::string::npos);
}

TEST(PkgHelpersTest, HelpStringContainsCommands) {
    std::string h = helpString();
    EXPECT_NE(h.find("install"), std::string::npos);
    EXPECT_NE(h.find("list"), std::string::npos);
    EXPECT_NE(h.find("init"), std::string::npos);
    EXPECT_NE(h.find("add"), std::string::npos);
}

// ============================================================
// 4. PackageManager - 清单操作
// ============================================================

TEST(PkgManifestTest, LoadManifestSucceeds) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString manifestPath = QDir(tmpDir.path()).filePath("minilang.pkg");
    writeFile(manifestPath, R"({
        "name": "test-project",
        "version": "1.0.0",
        "dependencies": [
            { "name": "dep1", "version": "1.0.0" }
        ]
    })");

    PkgConfig config;
    config.manifestPath = manifestPath.toStdString();
    PackageManager pm(config);

    std::string error;
    EXPECT_TRUE(pm.loadManifest("", &error)) << error;
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(pm.hasManifest());
    EXPECT_EQ(pm.manifest().name, "test-project");
    EXPECT_EQ(pm.manifest().version, "1.0.0");
    ASSERT_EQ(pm.manifest().dependencies.size(), 1u);
    EXPECT_EQ(pm.manifest().dependencies[0].name, "dep1");
}

TEST(PkgManifestTest, LoadManifestFailsOnNonexistentFile) {
    PkgConfig config;
    config.manifestPath = "/nonexistent/path/minilang.pkg";
    PackageManager pm(config);

    std::string error;
    EXPECT_FALSE(pm.loadManifest("", &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(pm.hasManifest());
}

TEST(PkgManifestTest, LoadManifestFailsOnInvalidJson) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString manifestPath = QDir(tmpDir.path()).filePath("minilang.pkg");
    writeFile(manifestPath, "{ invalid }");

    PkgConfig config;
    config.manifestPath = manifestPath.toStdString();
    PackageManager pm(config);

    std::string error;
    EXPECT_FALSE(pm.loadManifest("", &error));
    EXPECT_FALSE(error.empty());
}

TEST(PkgManifestTest, SaveManifestCreatesFile) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString manifestPath = QDir(tmpDir.path()).filePath("minilang.pkg");

    PkgConfig config;
    config.manifestPath = manifestPath.toStdString();
    PackageManager pm(config);

    pm.manifest().name = "saved-project";
    pm.manifest().version = "2.0.0";
    pm.manifest().dependencies.push_back({"saved-dep", "1.5.0", "", ""});

    std::string error;
    EXPECT_TRUE(pm.saveManifest("", &error)) << error;
    EXPECT_TRUE(QFile::exists(manifestPath));

    // 重新加载验证往返等价性
    PackageManager pm2(config);
    EXPECT_TRUE(pm2.loadManifest("", &error));
    EXPECT_EQ(pm2.manifest().name, "saved-project");
    EXPECT_EQ(pm2.manifest().version, "2.0.0");
    ASSERT_EQ(pm2.manifest().dependencies.size(), 1u);
    EXPECT_EQ(pm2.manifest().dependencies[0].name, "saved-dep");
    EXPECT_EQ(pm2.manifest().dependencies[0].version, "1.5.0");
}

TEST(PkgManifestTest, LoadManifestWithCustomPath) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString customPath = QDir(tmpDir.path()).filePath("custom.pkg");
    writeFile(customPath, R"({ "name": "custom", "version": "0.9.0" })");

    PackageManager pm;
    std::string error;
    EXPECT_TRUE(pm.loadManifest(customPath.toStdString(), &error));
    EXPECT_EQ(pm.manifest().name, "custom");
    EXPECT_EQ(pm.manifest().version, "0.9.0");
}

// ============================================================
// 5. PackageManager - 安装
// ============================================================

TEST(PkgManagerTest, InstallFromRegistrySucceeds) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString registryPath = QDir(tmpDir.path()).filePath("registry");
    QString packagesPath = QDir(tmpDir.path()).filePath("packages");
    createTestRegistry(registryPath);

    PkgConfig config;
    config.registryDir = registryPath.toStdString();
    config.packagesDir = packagesPath.toStdString();
    PackageManager pm(config);

    InstallResult result = pm.install("math-utils");
    EXPECT_TRUE(result.ok) << result.errorMessage;
    EXPECT_EQ(result.packageName, "math-utils");
    EXPECT_FALSE(result.installPath.empty());
    EXPECT_FALSE(result.alreadyInstalled);

    // 验证文件已复制
    EXPECT_TRUE(QFile::exists(QDir(packagesPath).filePath("math-utils/math-utils.mini")));
    EXPECT_TRUE(QFile::exists(QDir(packagesPath).filePath("math-utils/pkg.json")));
}

TEST(PkgManagerTest, InstallWithVersionFromRegistry) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString registryPath = QDir(tmpDir.path()).filePath("registry");
    QString packagesPath = QDir(tmpDir.path()).filePath("packages");
    createTestRegistry(registryPath);

    PkgConfig config;
    config.registryDir = registryPath.toStdString();
    config.packagesDir = packagesPath.toStdString();
    PackageManager pm(config);

    InstallResult result = pm.install("math-utils@1.0.0");
    EXPECT_TRUE(result.ok) << result.errorMessage;
    EXPECT_EQ(result.packageName, "math-utils");
}

TEST(PkgManagerTest, InstallAlreadyInstalledReturnsWarning) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString registryPath = QDir(tmpDir.path()).filePath("registry");
    QString packagesPath = QDir(tmpDir.path()).filePath("packages");
    createTestRegistry(registryPath);

    PkgConfig config;
    config.registryDir = registryPath.toStdString();
    config.packagesDir = packagesPath.toStdString();
    PackageManager pm(config);

    // 第一次安装
    InstallResult r1 = pm.install("math-utils");
    EXPECT_TRUE(r1.ok);
    EXPECT_FALSE(r1.alreadyInstalled);

    // 第二次安装应跳过
    InstallResult r2 = pm.install("math-utils");
    EXPECT_TRUE(r2.ok);
    EXPECT_TRUE(r2.alreadyInstalled);
}

TEST(PkgManagerTest, InstallNonexistentPackageFails) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString registryPath = QDir(tmpDir.path()).filePath("registry");
    QString packagesPath = QDir(tmpDir.path()).filePath("packages");
    createTestRegistry(registryPath);

    PkgConfig config;
    config.registryDir = registryPath.toStdString();
    config.packagesDir = packagesPath.toStdString();
    PackageManager pm(config);

    InstallResult result = pm.install("nonexistent-pkg");
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.errorMessage.empty());
}

TEST(PkgManagerTest, InstallWithoutRegistryFails) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString packagesPath = QDir(tmpDir.path()).filePath("packages");

    PkgConfig config;
    config.packagesDir = packagesPath.toStdString();
    // 不设置 registryDir
    PackageManager pm(config);

    InstallResult result = pm.install("some-pkg");
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.errorMessage.find("未指定 --registry"), std::string::npos);
}

TEST(PkgManagerTest, InstallFromFileProtocol) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString srcPath = QDir(tmpDir.path()).filePath("src-pkg");
    QString packagesPath = QDir(tmpDir.path()).filePath("packages");

    // 创建源包
    QDir srcDir(srcPath);
    (void)srcDir.mkpath(".");
    writeFile(srcDir.filePath("src-pkg.mini"), "export var x = 42;\n");
    writeFile(srcDir.filePath("pkg.json"), R"({"name":"src-pkg","version":"1.0.0"})");

    PkgConfig config;
    config.packagesDir = packagesPath.toStdString();
    PackageManager pm(config);

    InstallResult result = pm.install("file:" + srcPath.toStdString());
    EXPECT_TRUE(result.ok) << result.errorMessage;
    EXPECT_EQ(result.packageName, "src-pkg");
    EXPECT_TRUE(QFile::exists(QDir(packagesPath).filePath("src-pkg/src-pkg.mini")));
}

TEST(PkgManagerTest, InstallFromFileProtocolNonexistentFails) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString packagesPath = QDir(tmpDir.path()).filePath("packages");

    PkgConfig config;
    config.packagesDir = packagesPath.toStdString();
    PackageManager pm(config);

    InstallResult result = pm.install("file:/nonexistent/path/to/pkg");
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.errorMessage.empty());
}

TEST(PkgManagerTest, InstallAllFromManifest) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString registryPath = QDir(tmpDir.path()).filePath("registry");
    QString packagesPath = QDir(tmpDir.path()).filePath("packages");
    QString manifestPath = QDir(tmpDir.path()).filePath("minilang.pkg");
    createTestRegistry(registryPath);

    // 创建清单
    writeFile(manifestPath, R"({
        "name": "test-project",
        "version": "1.0.0",
        "dependencies": [
            { "name": "math-utils", "version": "1.0.0" },
            { "name": "string-utils", "version": "2.1.0" }
        ]
    })");

    PkgConfig config;
    config.registryDir = registryPath.toStdString();
    config.packagesDir = packagesPath.toStdString();
    config.manifestPath = manifestPath.toStdString();
    PackageManager pm(config);

    std::string error;
    ASSERT_TRUE(pm.loadManifest("", &error)) << error;

    auto results = pm.installAll();
    ASSERT_EQ(results.size(), 2u);
    EXPECT_TRUE(results[0].ok);
    EXPECT_EQ(results[0].packageName, "math-utils");
    EXPECT_TRUE(results[1].ok);
    EXPECT_EQ(results[1].packageName, "string-utils");

    // 验证两个包都已安装
    EXPECT_TRUE(QFile::exists(QDir(packagesPath).filePath("math-utils/math-utils.mini")));
    EXPECT_TRUE(QFile::exists(QDir(packagesPath).filePath("string-utils/string-utils.mini")));
}

TEST(PkgManagerTest, InstallAllWithoutManifestFails) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    PkgConfig config;
    config.packagesDir = QDir(tmpDir.path()).filePath("packages").toStdString();
    PackageManager pm(config);

    auto results = pm.installAll();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_FALSE(results[0].ok);
    EXPECT_EQ(results[0].errorMessage, "未加载清单文件");
}

TEST(PkgManagerTest, InstallReadsVersionFromPkgJson) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString registryPath = QDir(tmpDir.path()).filePath("registry");
    QString packagesPath = QDir(tmpDir.path()).filePath("packages");
    createTestRegistry(registryPath);

    PkgConfig config;
    config.registryDir = registryPath.toStdString();
    config.packagesDir = packagesPath.toStdString();
    PackageManager pm(config);

    // 不指定版本安装，应从 pkg.json 读取版本
    InstallResult result = pm.install("math-utils");
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.version, "1.0.0"); // 从 pkg.json 读取
}

// ============================================================
// 6. PackageManager - 列表
// ============================================================

TEST(PkgListTest, ListEmptyPackagesDirReturnsEmpty) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    PkgConfig config;
    config.packagesDir = QDir(tmpDir.path()).filePath("packages").toStdString();
    PackageManager pm(config);

    ListResult result = pm.listInstalled();
    EXPECT_TRUE(result.ok);
    EXPECT_TRUE(result.packages.empty());
}

TEST(PkgListTest, ListReturnsInstalledPackages) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString registryPath = QDir(tmpDir.path()).filePath("registry");
    QString packagesPath = QDir(tmpDir.path()).filePath("packages");
    createTestRegistry(registryPath);

    PkgConfig config;
    config.registryDir = registryPath.toStdString();
    config.packagesDir = packagesPath.toStdString();
    PackageManager pm(config);

    pm.install("math-utils");
    pm.install("string-utils");

    ListResult result = pm.listInstalled();
    EXPECT_TRUE(result.ok);
    ASSERT_EQ(result.packages.size(), 2u);

    // 按名称排序，math-utils 在前
    EXPECT_EQ(result.packages[0].name, "math-utils");
    EXPECT_EQ(result.packages[0].version, "1.0.0");
    EXPECT_EQ(result.packages[0].description, "Math utilities");
    EXPECT_EQ(result.packages[1].name, "string-utils");
    EXPECT_EQ(result.packages[1].version, "2.1.0");
}

TEST(PkgListTest, ListPackageWithoutPkgJsonUsesDirName) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString packagesPath = QDir(tmpDir.path()).filePath("packages");
    QDir packagesDir(packagesPath);
    (void)packagesDir.mkpath(".");

    // 创建无 pkg.json 的包目录
    QDir pkgDir = packagesDir.filePath("bare-pkg");
    (void)pkgDir.mkpath(".");
    writeFile(pkgDir.filePath("bare-pkg.mini"), "export var x = 1;\n");

    PkgConfig config;
    config.packagesDir = packagesPath.toStdString();
    PackageManager pm(config);

    ListResult result = pm.listInstalled();
    EXPECT_TRUE(result.ok);
    ASSERT_EQ(result.packages.size(), 1u);
    EXPECT_EQ(result.packages[0].name, "bare-pkg");
    EXPECT_TRUE(result.packages[0].version.empty());
}

// ============================================================
// 7. PackageManager - 模块路径解析
// ============================================================

TEST(PkgResolveTest, ResolveModuleByPackageNameFindsEntryFile) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString registryPath = QDir(tmpDir.path()).filePath("registry");
    QString packagesPath = QDir(tmpDir.path()).filePath("packages");
    createTestRegistry(registryPath);

    PkgConfig config;
    config.registryDir = registryPath.toStdString();
    config.packagesDir = packagesPath.toStdString();
    PackageManager pm(config);
    pm.install("math-utils");

    // "math-utils" → packages/math-utils/math-utils.mini
    std::string resolved = pm.resolveModule("math-utils");
    EXPECT_FALSE(resolved.empty());
    EXPECT_NE(resolved.find("math-utils.mini"), std::string::npos);
}

TEST(PkgResolveTest, ResolveModuleByPathWithSlash) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString registryPath = QDir(tmpDir.path()).filePath("registry");
    QString packagesPath = QDir(tmpDir.path()).filePath("packages");
    createTestRegistry(registryPath);

    PkgConfig config;
    config.registryDir = registryPath.toStdString();
    config.packagesDir = packagesPath.toStdString();
    PackageManager pm(config);
    pm.install("math-utils");

    // "math-utils/math-utils.mini" → packages/math-utils/math-utils.mini
    std::string resolved = pm.resolveModule("math-utils/math-utils.mini");
    EXPECT_FALSE(resolved.empty());
    EXPECT_NE(resolved.find("math-utils.mini"), std::string::npos);
}

TEST(PkgResolveTest, ResolveModuleAutoAppendsMiniExtension) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString registryPath = QDir(tmpDir.path()).filePath("registry");
    QString packagesPath = QDir(tmpDir.path()).filePath("packages");
    createTestRegistry(registryPath);

    PkgConfig config;
    config.registryDir = registryPath.toStdString();
    config.packagesDir = packagesPath.toStdString();
    PackageManager pm(config);
    pm.install("math-utils");

    // "math-utils/math-utils" → 自动补 .mini
    std::string resolved = pm.resolveModule("math-utils/math-utils");
    EXPECT_FALSE(resolved.empty());
    EXPECT_NE(resolved.find("math-utils.mini"), std::string::npos);
}

TEST(PkgResolveTest, ResolveModuleReturnsEmptyForNonexistent) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    PkgConfig config;
    config.packagesDir = QDir(tmpDir.path()).filePath("packages").toStdString();
    PackageManager pm(config);

    EXPECT_TRUE(pm.resolveModule("nonexistent").empty());
    EXPECT_TRUE(pm.resolveModule("nonexistent/file.mini").empty());
}

TEST(PkgResolveTest, ResolveModuleReturnsEmptyForEmptyInput) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    PkgConfig config;
    config.packagesDir = QDir(tmpDir.path()).filePath("packages").toStdString();
    PackageManager pm(config);

    EXPECT_TRUE(pm.resolveModule("").empty());
}

TEST(PkgResolveTest, ResolveModuleFindsAnyMiniFileAsFallback) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString packagesPath = QDir(tmpDir.path()).filePath("packages");

    // 创建包目录，入口文件名与包名不同
    QDir pkgDir = QDir(packagesPath).filePath("custom-pkg");
    (void)pkgDir.mkpath(".");
    writeFile(pkgDir.filePath("utils.mini"), "export var x = 1;\n");
    // 不创建 custom-pkg.mini

    PkgConfig config;
    config.packagesDir = packagesPath.toStdString();
    PackageManager pm(config);

    // "custom-pkg" 应回退到第一个 .mini 文件
    std::string resolved = pm.resolveModule("custom-pkg");
    EXPECT_FALSE(resolved.empty());
    EXPECT_NE(resolved.find("utils.mini"), std::string::npos);
}

// ============================================================
// 8. CLI 参数解析
// ============================================================

TEST(PkgCliArgsTest, NoArgsShowsHelp) {
    char* argv[] = {const_cast<char*>("minilang-pkg")};
    CliArgs args = parseArgs(1, argv);
    EXPECT_TRUE(args.showHelp);
}

TEST(PkgCliArgsTest, HelpFlag) {
    char* argv[] = {const_cast<char*>("minilang-pkg"), const_cast<char*>("--help")};
    CliArgs args = parseArgs(2, argv);
    EXPECT_TRUE(args.showHelp);
}

TEST(PkgCliArgsTest, VersionFlag) {
    char* argv[] = {const_cast<char*>("minilang-pkg"), const_cast<char*>("--version")};
    CliArgs args = parseArgs(2, argv);
    EXPECT_TRUE(args.showVersion);
}

TEST(PkgCliArgsTest, ShortHelpFlag) {
    char* argv[] = {const_cast<char*>("minilang-pkg"), const_cast<char*>("-h")};
    CliArgs args = parseArgs(2, argv);
    EXPECT_TRUE(args.showHelp);
}

TEST(PkgCliArgsTest, ShortVersionFlag) {
    char* argv[] = {const_cast<char*>("minilang-pkg"), const_cast<char*>("-v")};
    CliArgs args = parseArgs(2, argv);
    EXPECT_TRUE(args.showVersion);
}

TEST(PkgCliArgsTest, InstallCommandWithPackage) {
    char* argv[] = {const_cast<char*>("minilang-pkg"), const_cast<char*>("install"), const_cast<char*>("math-utils")};
    CliArgs args = parseArgs(3, argv);
    EXPECT_EQ(args.command, "install");
    ASSERT_EQ(args.args.size(), 1u);
    EXPECT_EQ(args.args[0], "math-utils");
}

TEST(PkgCliArgsTest, InstallCommandWithoutPackage) {
    char* argv[] = {const_cast<char*>("minilang-pkg"), const_cast<char*>("install")};
    CliArgs args = parseArgs(2, argv);
    EXPECT_EQ(args.command, "install");
    EXPECT_TRUE(args.args.empty());
}

TEST(PkgCliArgsTest, ListCommand) {
    char* argv[] = {const_cast<char*>("minilang-pkg"), const_cast<char*>("list")};
    CliArgs args = parseArgs(2, argv);
    EXPECT_EQ(args.command, "list");
}

TEST(PkgCliArgsTest, InitCommandWithName) {
    char* argv[] = {const_cast<char*>("minilang-pkg"), const_cast<char*>("init"), const_cast<char*>("my-project")};
    CliArgs args = parseArgs(3, argv);
    EXPECT_EQ(args.command, "init");
    ASSERT_EQ(args.args.size(), 1u);
    EXPECT_EQ(args.args[0], "my-project");
}

TEST(PkgCliArgsTest, AddCommandWithPackageSpec) {
    char* argv[] = {const_cast<char*>("minilang-pkg"), const_cast<char*>("add"), const_cast<char*>("math-utils@1.0.0")};
    CliArgs args = parseArgs(3, argv);
    EXPECT_EQ(args.command, "add");
    ASSERT_EQ(args.args.size(), 1u);
    EXPECT_EQ(args.args[0], "math-utils@1.0.0");
}

TEST(PkgCliArgsTest, RegistryOption) {
    char* argv[] = {const_cast<char*>("minilang-pkg"), const_cast<char*>("--registry"),
                    const_cast<char*>("/path/to/registry"), const_cast<char*>("install"), const_cast<char*>("pkg")};
    CliArgs args = parseArgs(5, argv);
    EXPECT_EQ(args.registryDir, "/path/to/registry");
    EXPECT_EQ(args.command, "install");
    ASSERT_EQ(args.args.size(), 1u);
    EXPECT_EQ(args.args[0], "pkg");
}

TEST(PkgCliArgsTest, RegistryOptionAfterCommand) {
    char* argv[] = {const_cast<char*>("minilang-pkg"), const_cast<char*>("install"), const_cast<char*>("pkg"),
                    const_cast<char*>("--registry"), const_cast<char*>("/path/to/registry")};
    CliArgs args = parseArgs(5, argv);
    EXPECT_EQ(args.command, "install");
    EXPECT_EQ(args.registryDir, "/path/to/registry");
    ASSERT_EQ(args.args.size(), 1u);
    EXPECT_EQ(args.args[0], "pkg");
}

TEST(PkgCliArgsTest, PackagesDirOption) {
    char* argv[] = {const_cast<char*>("minilang-pkg"), const_cast<char*>("--packages-dir"),
                    const_cast<char*>("/custom/packages"), const_cast<char*>("list")};
    CliArgs args = parseArgs(4, argv);
    EXPECT_EQ(args.packagesDir, "/custom/packages");
    EXPECT_EQ(args.command, "list");
}

TEST(PkgCliArgsTest, ManifestOption) {
    char* argv[] = {const_cast<char*>("minilang-pkg"), const_cast<char*>("--manifest"),
                    const_cast<char*>("/custom/minilang.pkg"), const_cast<char*>("install")};
    CliArgs args = parseArgs(4, argv);
    EXPECT_EQ(args.manifestPath, "/custom/minilang.pkg");
    EXPECT_EQ(args.command, "install");
}

TEST(PkgCliArgsTest, RegistryOptionMissingValueReturnsError) {
    char* argv[] = {const_cast<char*>("minilang-pkg"), const_cast<char*>("--registry")};
    CliArgs args = parseArgs(2, argv);
    EXPECT_TRUE(args.parseError);
    EXPECT_FALSE(args.errorMessage.empty());
}

TEST(PkgCliArgsTest, UnknownOptionReturnsError) {
    char* argv[] = {const_cast<char*>("minilang-pkg"), const_cast<char*>("--unknown-option")};
    CliArgs args = parseArgs(2, argv);
    EXPECT_TRUE(args.parseError);
    EXPECT_NE(args.errorMessage.find("未知选项"), std::string::npos);
}

TEST(PkgCliArgsTest, UnknownCommandReturnsInProcessCommand) {
    char* argv[] = {const_cast<char*>("minilang-pkg"), const_cast<char*>("unknown-command")};
    CliArgs args = parseArgs(2, argv);
    EXPECT_EQ(args.command, "unknown-command");
    CommandResult result = processCommand(args);
    EXPECT_EQ(result.exitCode, 2);
    EXPECT_NE(result.output.find("未知命令"), std::string::npos);
}

// ============================================================
// 9. CLI 命令处理
// ============================================================

TEST(PkgCliCommandTest, HelpCommandReturnsHelpText) {
    CliArgs args;
    args.showHelp = true;
    CommandResult result = processCommand(args);
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_NE(result.output.find("minilang-pkg"), std::string::npos);
    EXPECT_NE(result.output.find("用法"), std::string::npos);
}

TEST(PkgCliCommandTest, VersionCommandReturnsVersion) {
    CliArgs args;
    args.showVersion = true;
    CommandResult result = processCommand(args);
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_NE(result.output.find("minilang-pkg"), std::string::npos);
}

TEST(PkgCliCommandTest, InitCommandCreatesManifest) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString manifestPath = QDir(tmpDir.path()).filePath("minilang.pkg");

    CliArgs args;
    args.command = "init";
    args.args.push_back("test-project");
    args.manifestPath = manifestPath.toStdString();

    CommandResult result = processCommand(args);
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_TRUE(QFile::exists(manifestPath));

    // 验证清单内容
    PackageManager pm;
    std::string error;
    ASSERT_TRUE(pm.loadManifest(manifestPath.toStdString(), &error));
    EXPECT_EQ(pm.manifest().name, "test-project");
    EXPECT_EQ(pm.manifest().version, "0.1.0");
}

TEST(PkgCliCommandTest, InitCommandExistingManifestReturnsWarning) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString manifestPath = QDir(tmpDir.path()).filePath("minilang.pkg");
    writeFile(manifestPath, R"({"name":"existing"})");

    CliArgs args;
    args.command = "init";
    args.manifestPath = manifestPath.toStdString();

    CommandResult result = processCommand(args);
    EXPECT_EQ(result.exitCode, 1); // 警告
    EXPECT_NE(result.output.find("已存在"), std::string::npos);
}

TEST(PkgCliCommandTest, InitCommandDefaultName) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString manifestPath = QDir(tmpDir.path()).filePath("minilang.pkg");

    CliArgs args;
    args.command = "init";
    args.manifestPath = manifestPath.toStdString();
    // 不提供 name 参数

    CommandResult result = processCommand(args);
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_TRUE(QFile::exists(manifestPath));

    PackageManager pm;
    std::string error;
    ASSERT_TRUE(pm.loadManifest(manifestPath.toStdString(), &error));
    EXPECT_EQ(pm.manifest().name, "my-project"); // 默认名
}

TEST(PkgCliCommandTest, InstallCommandFromRegistry) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString registryPath = QDir(tmpDir.path()).filePath("registry");
    QString packagesPath = QDir(tmpDir.path()).filePath("packages");
    QString manifestPath = QDir(tmpDir.path()).filePath("minilang.pkg");
    createTestRegistry(registryPath);

    CliArgs args;
    args.command = "install";
    args.args.push_back("math-utils");
    args.registryDir = registryPath.toStdString();
    args.packagesDir = packagesPath.toStdString();
    args.manifestPath = manifestPath.toStdString();

    CommandResult result = processCommand(args);
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_NE(result.output.find("成功安装"), std::string::npos);
    EXPECT_TRUE(QFile::exists(QDir(packagesPath).filePath("math-utils/math-utils.mini")));
}

TEST(PkgCliCommandTest, InstallCommandAllFromManifest) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString registryPath = QDir(tmpDir.path()).filePath("registry");
    QString packagesPath = QDir(tmpDir.path()).filePath("packages");
    QString manifestPath = QDir(tmpDir.path()).filePath("minilang.pkg");
    createTestRegistry(registryPath);

    writeFile(manifestPath, R"({
        "name": "test",
        "version": "1.0.0",
        "dependencies": [
            { "name": "math-utils" },
            { "name": "string-utils" }
        ]
    })");

    CliArgs args;
    args.command = "install";
    args.registryDir = registryPath.toStdString();
    args.packagesDir = packagesPath.toStdString();
    args.manifestPath = manifestPath.toStdString();

    CommandResult result = processCommand(args);
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_TRUE(QFile::exists(QDir(packagesPath).filePath("math-utils/math-utils.mini")));
    EXPECT_TRUE(QFile::exists(QDir(packagesPath).filePath("string-utils/string-utils.mini")));
}

TEST(PkgCliCommandTest, InstallCommandFailsWithoutRegistry) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    CliArgs args;
    args.command = "install";
    args.args.push_back("some-pkg");
    args.packagesDir = QDir(tmpDir.path()).filePath("packages").toStdString();

    CommandResult result = processCommand(args);
    EXPECT_EQ(result.exitCode, 2);
    EXPECT_NE(result.output.find("错误"), std::string::npos);
}

TEST(PkgCliCommandTest, ListCommandEmptyPackages) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    CliArgs args;
    args.command = "list";
    args.packagesDir = QDir(tmpDir.path()).filePath("packages").toStdString();

    CommandResult result = processCommand(args);
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_NE(result.output.find("未安装"), std::string::npos);
}

TEST(PkgCliCommandTest, ListCommandShowsInstalledPackages) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString registryPath = QDir(tmpDir.path()).filePath("registry");
    QString packagesPath = QDir(tmpDir.path()).filePath("packages");
    createTestRegistry(registryPath);

    // 先安装包
    {
        CliArgs args;
        args.command = "install";
        args.args.push_back("math-utils");
        args.registryDir = registryPath.toStdString();
        args.packagesDir = packagesPath.toStdString();
        processCommand(args);
    }

    // 列表
    CliArgs args;
    args.command = "list";
    args.packagesDir = packagesPath.toStdString();

    CommandResult result = processCommand(args);
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_NE(result.output.find("math-utils"), std::string::npos);
    EXPECT_NE(result.output.find("1.0.0"), std::string::npos);
}

TEST(PkgCliCommandTest, AddCommandAddsDependencyAndInstalls) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString registryPath = QDir(tmpDir.path()).filePath("registry");
    QString packagesPath = QDir(tmpDir.path()).filePath("packages");
    QString manifestPath = QDir(tmpDir.path()).filePath("minilang.pkg");
    createTestRegistry(registryPath);

    // 先 init
    {
        CliArgs args;
        args.command = "init";
        args.args.push_back("test-project");
        args.manifestPath = manifestPath.toStdString();
        processCommand(args);
    }

    // add 包
    CliArgs args;
    args.command = "add";
    args.args.push_back("math-utils@1.0.0");
    args.registryDir = registryPath.toStdString();
    args.packagesDir = packagesPath.toStdString();
    args.manifestPath = manifestPath.toStdString();

    CommandResult result = processCommand(args);
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_NE(result.output.find("已添加依赖"), std::string::npos);

    // 验证清单已更新
    PackageManager pm;
    std::string error;
    ASSERT_TRUE(pm.loadManifest(manifestPath.toStdString(), &error));
    ASSERT_EQ(pm.manifest().dependencies.size(), 1u);
    EXPECT_EQ(pm.manifest().dependencies[0].name, "math-utils");
    EXPECT_EQ(pm.manifest().dependencies[0].version, "1.0.0");

    // 验证包已安装
    EXPECT_TRUE(QFile::exists(QDir(packagesPath).filePath("math-utils/math-utils.mini")));
}

TEST(PkgCliCommandTest, AddCommandWithoutArgsReturnsError) {
    CliArgs args;
    args.command = "add";

    CommandResult result = processCommand(args);
    EXPECT_EQ(result.exitCode, 2);
    EXPECT_NE(result.output.find("需要包描述符参数"), std::string::npos);
}

TEST(PkgCliCommandTest, AddCommandUpdatesExistingDependency) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString registryPath = QDir(tmpDir.path()).filePath("registry");
    QString manifestPath = QDir(tmpDir.path()).filePath("minilang.pkg");
    createTestRegistry(registryPath);

    // 创建已有依赖的清单
    writeFile(manifestPath, R"({
        "name": "test",
        "version": "1.0.0",
        "dependencies": [
            { "name": "math-utils", "version": "0.9.0" }
        ]
    })");

    CliArgs args;
    args.command = "add";
    args.args.push_back("math-utils@1.0.0");
    args.registryDir = registryPath.toStdString();
    args.packagesDir = QDir(tmpDir.path()).filePath("packages").toStdString();
    args.manifestPath = manifestPath.toStdString();

    CommandResult result = processCommand(args);
    EXPECT_EQ(result.exitCode, 0);

    // 验证版本已更新（不应有两个 math-utils）
    PackageManager pm;
    std::string error;
    ASSERT_TRUE(pm.loadManifest(manifestPath.toStdString(), &error));
    ASSERT_EQ(pm.manifest().dependencies.size(), 1u);
    EXPECT_EQ(pm.manifest().dependencies[0].version, "1.0.0");
}

TEST(PkgCliCommandTest, ParseErrorReturnsErrorMessage) {
    CliArgs args;
    args.parseError = true;
    args.errorMessage = "测试错误";
    CommandResult result = processCommand(args);
    EXPECT_EQ(result.exitCode, 2);
    EXPECT_NE(result.output.find("测试错误"), std::string::npos);
}

TEST(PkgCliCommandTest, EmptyCommandShowsHelp) {
    CliArgs args;
    args.command = "";
    CommandResult result = processCommand(args);
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_NE(result.output.find("用法"), std::string::npos);
}

// ============================================================
// P2-11 版本约束测试
// ============================================================

TEST(PkgVersionTest, ParseValidSemVer) {
    Version v = Version::parse("1.2.3");
    EXPECT_EQ(v.major, 1);
    EXPECT_EQ(v.minor, 2);
    EXPECT_EQ(v.patch, 3);
}

TEST(PkgVersionTest, ParseTwoComponent) {
    Version v = Version::parse("2.0");
    EXPECT_EQ(v.major, 2);
    EXPECT_EQ(v.minor, 0);
    EXPECT_EQ(v.patch, 0);
}

TEST(PkgVersionTest, ParseSingleComponent) {
    Version v = Version::parse("5");
    EXPECT_EQ(v.major, 5);
    EXPECT_EQ(v.minor, 0);
    EXPECT_EQ(v.patch, 0);
}

TEST(PkgVersionTest, ParseEmptyReturnsZero) {
    Version v = Version::parse("");
    Version zero{0, 0, 0};
    EXPECT_EQ(v, zero);
}

TEST(PkgVersionTest, ParseStopsAtNonDigit) {
    Version v = Version::parse("1.2.3-beta");
    EXPECT_EQ(v.major, 1);
    EXPECT_EQ(v.minor, 2);
    EXPECT_EQ(v.patch, 3);
}

TEST(PkgVersionTest, ToStringRoundTrip) {
    Version v{3, 1, 4};
    EXPECT_EQ(v.toString(), "3.1.4");
}

TEST(PkgVersionTest, Comparisons) {
    Version v1{1, 0, 0};
    Version v2{1, 0, 1};
    Version v3{1, 1, 0};
    Version v4{2, 0, 0};

    EXPECT_TRUE(v1 < v2);
    EXPECT_TRUE(v2 < v3);
    EXPECT_TRUE(v3 < v4);
    EXPECT_TRUE(v1 <= v1);
    EXPECT_TRUE(v4 > v3);
    EXPECT_TRUE(v4 >= v4);
    EXPECT_TRUE(v1 != v2);
    Version one{1, 0, 0};
    EXPECT_TRUE(v1 == one);
}

// ============================================================
// P2-11 版本约束解析与匹配
// ============================================================

TEST(PkgVersionConstraintTest, ParseAny) {
    VersionConstraint c = VersionConstraint::parse("*");
    EXPECT_EQ(c.op, ConstraintOp::Any);
    EXPECT_TRUE(c.matches(Version{1, 0, 0}));
    EXPECT_TRUE(c.matches(Version{99, 99, 99}));
}

TEST(PkgVersionConstraintTest, ParseEmptyAsAny) {
    VersionConstraint c = VersionConstraint::parse("");
    EXPECT_EQ(c.op, ConstraintOp::Any);
}

TEST(PkgVersionConstraintTest, ParseExact) {
    VersionConstraint c = VersionConstraint::parse("1.2.3");
    EXPECT_EQ(c.op, ConstraintOp::Exact);
    EXPECT_TRUE(c.matches(Version{1, 2, 3}));
    EXPECT_FALSE(c.matches(Version{1, 2, 4}));
    EXPECT_FALSE(c.matches(Version{1, 3, 0}));
}

TEST(PkgVersionConstraintTest, ParseCaret) {
    VersionConstraint c = VersionConstraint::parse("^1.2.3");
    EXPECT_EQ(c.op, ConstraintOp::Caret);
    // ^1.2.3 → >=1.2.3 <2.0.0
    EXPECT_TRUE(c.matches(Version{1, 2, 3}));
    EXPECT_TRUE(c.matches(Version{1, 9, 9}));
    EXPECT_FALSE(c.matches(Version{2, 0, 0}));
    EXPECT_FALSE(c.matches(Version{1, 2, 2}));
}

TEST(PkgVersionConstraintTest, ParseCaretZeroMajor) {
    VersionConstraint c = VersionConstraint::parse("^0.2.3");
    EXPECT_EQ(c.op, ConstraintOp::Caret);
    // ^0.2.3 → >=0.2.3 <0.3.0
    EXPECT_TRUE(c.matches(Version{0, 2, 3}));
    EXPECT_TRUE(c.matches(Version{0, 2, 9}));
    EXPECT_FALSE(c.matches(Version{0, 3, 0}));
    EXPECT_FALSE(c.matches(Version{1, 0, 0}));
}

TEST(PkgVersionConstraintTest, ParseCaretZeroMajorMinor) {
    VersionConstraint c = VersionConstraint::parse("^0.0.1");
    EXPECT_EQ(c.op, ConstraintOp::Caret);
    // ^0.0.1 → >=0.0.1 <0.0.2
    EXPECT_TRUE(c.matches(Version{0, 0, 1}));
    EXPECT_FALSE(c.matches(Version{0, 0, 2}));
}

TEST(PkgVersionConstraintTest, ParseTilde) {
    VersionConstraint c = VersionConstraint::parse("~1.2.3");
    EXPECT_EQ(c.op, ConstraintOp::Tilde);
    // ~1.2.3 → >=1.2.3 <1.3.0
    EXPECT_TRUE(c.matches(Version{1, 2, 3}));
    EXPECT_TRUE(c.matches(Version{1, 2, 9}));
    EXPECT_FALSE(c.matches(Version{1, 3, 0}));
    EXPECT_FALSE(c.matches(Version{1, 1, 0}));
}

TEST(PkgVersionConstraintTest, ParseGreaterEq) {
    VersionConstraint c = VersionConstraint::parse(">=1.0.0");
    EXPECT_EQ(c.op, ConstraintOp::GreaterEq);
    EXPECT_TRUE(c.matches(Version{1, 0, 0}));
    EXPECT_TRUE(c.matches(Version{2, 0, 0}));
    EXPECT_FALSE(c.matches(Version{0, 9, 9}));
}

TEST(PkgVersionConstraintTest, ParseGreater) {
    VersionConstraint c = VersionConstraint::parse(">1.0.0");
    EXPECT_EQ(c.op, ConstraintOp::Greater);
    EXPECT_FALSE(c.matches(Version{1, 0, 0}));
    EXPECT_TRUE(c.matches(Version{1, 0, 1}));
    EXPECT_FALSE(c.matches(Version{0, 9, 9}));
}

TEST(PkgVersionConstraintTest, ParseLessEq) {
    VersionConstraint c = VersionConstraint::parse("<=2.0.0");
    EXPECT_EQ(c.op, ConstraintOp::LessEq);
    EXPECT_TRUE(c.matches(Version{2, 0, 0}));
    EXPECT_TRUE(c.matches(Version{1, 9, 9}));
    EXPECT_FALSE(c.matches(Version{2, 0, 1}));
}

TEST(PkgVersionConstraintTest, ParseLess) {
    VersionConstraint c = VersionConstraint::parse("<2.0.0");
    EXPECT_EQ(c.op, ConstraintOp::Less);
    EXPECT_FALSE(c.matches(Version{2, 0, 0}));
    EXPECT_TRUE(c.matches(Version{1, 9, 9}));
}

TEST(PkgVersionConstraintTest, ToStringRoundTrip) {
    EXPECT_EQ(VersionConstraint::parse("^1.0.0").toString(), "^1.0.0");
    EXPECT_EQ(VersionConstraint::parse("~1.2.3").toString(), "~1.2.3");
    EXPECT_EQ(VersionConstraint::parse(">=1.0.0").toString(), ">=1.0.0");
    EXPECT_EQ(VersionConstraint::parse("*").toString(), "*");
    EXPECT_EQ(VersionConstraint::parse("1.2.3").toString(), "1.2.3");
}

// ============================================================
// P2-11 PackageManager 版本约束安装
// ============================================================

TEST(PkgManagerConstraintTest, InstallWithCaretConstraintPicksLatestMatching) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString registryPath = QDir(tmpDir.path()).filePath("registry");
    QString packagesPath = QDir(tmpDir.path()).filePath("packages");
    createVersionedRegistry(registryPath); // ver-pkg 1.0.0 + 2.0.0

    PkgConfig config;
    config.registryDir = registryPath.toStdString();
    config.packagesDir = packagesPath.toStdString();
    PackageManager pm(config);

    // ^1.0.0 应选择 1.0.0（而非 2.0.0）
    InstallResult result = pm.install("ver-pkg@^1.0.0");
    EXPECT_TRUE(result.ok) << result.errorMessage;
    EXPECT_EQ(result.version, "1.0.0");
}

TEST(PkgManagerConstraintTest, InstallWithGreaterEqConstraintPicksHighest) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString registryPath = QDir(tmpDir.path()).filePath("registry");
    QString packagesPath = QDir(tmpDir.path()).filePath("packages");
    createVersionedRegistry(registryPath);

    PkgConfig config;
    config.registryDir = registryPath.toStdString();
    config.packagesDir = packagesPath.toStdString();
    PackageManager pm(config);

    // >=1.0.0 应选择最高版本 2.0.0
    InstallResult result = pm.install("ver-pkg@>=1.0.0");
    EXPECT_TRUE(result.ok) << result.errorMessage;
    EXPECT_EQ(result.version, "2.0.0");
}

TEST(PkgManagerConstraintTest, InstallWithLessConstraintPicksMatchingVersion) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString registryPath = QDir(tmpDir.path()).filePath("registry");
    QString packagesPath = QDir(tmpDir.path()).filePath("packages");
    createVersionedRegistry(registryPath);

    PkgConfig config;
    config.registryDir = registryPath.toStdString();
    config.packagesDir = packagesPath.toStdString();
    PackageManager pm(config);

    // <2.0.0 应选择 1.0.0
    InstallResult result = pm.install("ver-pkg@<2.0.0");
    EXPECT_TRUE(result.ok) << result.errorMessage;
    EXPECT_EQ(result.version, "1.0.0");
}

TEST(PkgManagerConstraintTest, InstallWithUnsatisfiableConstraintFails) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString registryPath = QDir(tmpDir.path()).filePath("registry");
    QString packagesPath = QDir(tmpDir.path()).filePath("packages");
    createVersionedRegistry(registryPath); // 1.0.0 + 2.0.0

    PkgConfig config;
    config.registryDir = registryPath.toStdString();
    config.packagesDir = packagesPath.toStdString();
    PackageManager pm(config);

    // >=3.0.0 无匹配版本
    InstallResult result = pm.install("ver-pkg@>=3.0.0");
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.errorMessage.find("未找到"), std::string::npos);
}

TEST(PkgManagerConstraintTest, GetInstalledVersionReadsPkgJson) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString packagesPath = QDir(tmpDir.path()).filePath("packages");
    QDir pkgDir = QDir(packagesPath).filePath("test-ver");
    (void)pkgDir.mkpath(".");
    QFile pkgJson(pkgDir.filePath("pkg.json"));
    (void)pkgJson.open(QIODevice::WriteOnly | QIODevice::Text);
    QTextStream(&pkgJson) << "{\"name\":\"test-ver\",\"version\":\"3.7.2\"}";
    pkgJson.close();

    PkgConfig config;
    config.packagesDir = packagesPath.toStdString();
    PackageManager pm(config);

    auto ver = pm.getInstalledVersion("test-ver");
    ASSERT_TRUE(ver.has_value());
    EXPECT_EQ(ver->major, 3);
    EXPECT_EQ(ver->minor, 7);
    EXPECT_EQ(ver->patch, 2);
}

TEST(PkgManagerConstraintTest, GetInstalledVersionReturnsNulloptForUninstalled) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    PkgConfig config;
    config.packagesDir = QDir(tmpDir.path()).filePath("packages").toStdString();
    PackageManager pm(config);

    EXPECT_FALSE(pm.getInstalledVersion("nonexistent").has_value());
}

TEST(PkgManagerConstraintTest, CheckVersionConstraintReturnsTrueWhenNotInstalled) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    PkgConfig config;
    config.packagesDir = QDir(tmpDir.path()).filePath("packages").toStdString();
    PackageManager pm(config);

    // 未安装的包不构成冲突
    EXPECT_TRUE(pm.checkVersionConstraint("nonexistent", VersionConstraint::parse("^1.0.0")));
}

TEST(PkgManagerConstraintTest, CheckVersionConstraintDetectsConflict) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString packagesPath = QDir(tmpDir.path()).filePath("packages");
    QDir pkgDir = QDir(packagesPath).filePath("conflict-pkg");
    (void)pkgDir.mkpath(".");
    QFile pkgJson(pkgDir.filePath("pkg.json"));
    (void)pkgJson.open(QIODevice::WriteOnly | QIODevice::Text);
    QTextStream(&pkgJson) << "{\"name\":\"conflict-pkg\",\"version\":\"1.5.0\"}";
    pkgJson.close();

    PkgConfig config;
    config.packagesDir = packagesPath.toStdString();
    PackageManager pm(config);

    // 已安装 1.5.0，要求 ^2.0.0 → 冲突
    EXPECT_FALSE(pm.checkVersionConstraint("conflict-pkg", VersionConstraint::parse("^2.0.0")));
    // 已安装 1.5.0，要求 ^1.0.0 → 兼容
    EXPECT_TRUE(pm.checkVersionConstraint("conflict-pkg", VersionConstraint::parse("^1.0.0")));
}

// ============================================================
// P2-11 传递依赖安装测试
// ============================================================

namespace {
/// 创建带传递依赖的 registry
/// registry/
///   app-pkg/
///     pkg.json (依赖 lib-a)
///     minilang.pkg (声明依赖 lib-a)
///     app-pkg.mini
///   lib-a/
///     pkg.json
///     lib-a.mini
void createTransitiveRegistry(const QString& registryPath) {
    QDir registryDir(registryPath);
    (void)registryDir.mkpath(".");

    // app-pkg：有传递依赖
    QDir appDir = registryDir.filePath("app-pkg");
    (void)appDir.mkpath(".");
    {
        QFile pkgJson(appDir.filePath("pkg.json"));
        (void)pkgJson.open(QIODevice::WriteOnly | QIODevice::Text);
        QTextStream(&pkgJson) << "{\"name\":\"app-pkg\",\"version\":\"1.0.0\"}";
        pkgJson.close();

        // 声明传递依赖
        QFile manifest(appDir.filePath("minilang.pkg"));
        (void)manifest.open(QIODevice::WriteOnly | QIODevice::Text);
        QTextStream(&manifest) << "{\"name\":\"app-pkg\",\"version\":\"1.0.0\","
                                  "\"dependencies\":[{\"name\":\"lib-a\",\"version\":\"1.0.0\"}]}";
        manifest.close();

        QFile miniFile(appDir.filePath("app-pkg.mini"));
        (void)miniFile.open(QIODevice::WriteOnly | QIODevice::Text);
        QTextStream(&miniFile) << "export fun app() { return 42; }\n";
        miniFile.close();
    }

    // lib-a：被 app-pkg 依赖
    QDir libDir = registryDir.filePath("lib-a");
    (void)libDir.mkpath(".");
    {
        QFile pkgJson(libDir.filePath("pkg.json"));
        (void)pkgJson.open(QIODevice::WriteOnly | QIODevice::Text);
        QTextStream(&pkgJson) << "{\"name\":\"lib-a\",\"version\":\"1.0.0\"}";
        pkgJson.close();

        QFile miniFile(libDir.filePath("lib-a.mini"));
        (void)miniFile.open(QIODevice::WriteOnly | QIODevice::Text);
        QTextStream(&miniFile) << "export fun lib() { return 1; }\n";
        miniFile.close();
    }
}

/// 创建循环依赖的 registry
/// registry/
///   cycle-a/
///     pkg.json
///     minilang.pkg (依赖 cycle-b)
///   cycle-b/
///     pkg.json
///     minilang.pkg (依赖 cycle-a)
void createCycleRegistry(const QString& registryPath) {
    QDir registryDir(registryPath);
    (void)registryDir.mkpath(".");

    for (const auto& [name, dep] :
         std::vector<std::pair<QString, QString>>{{"cycle-a", "cycle-b"}, {"cycle-b", "cycle-a"}}) {
        QDir dir = registryDir.filePath(name);
        (void)dir.mkpath(".");
        QFile pkgJson(dir.filePath("pkg.json"));
        (void)pkgJson.open(QIODevice::WriteOnly | QIODevice::Text);
        QTextStream(&pkgJson) << QString("{\"name\":\"%1\",\"version\":\"1.0.0\"}").arg(name);
        pkgJson.close();

        QFile manifest(dir.filePath("minilang.pkg"));
        (void)manifest.open(QIODevice::WriteOnly | QIODevice::Text);
        QTextStream(&manifest) << QString("{\"name\":\"%1\",\"version\":\"1.0.0\","
                                          "\"dependencies\":[{\"name\":\"%2\",\"version\":\"1.0.0\"}]}")
                                      .arg(name, dep);
        manifest.close();

        QFile miniFile(dir.filePath(name + ".mini"));
        (void)miniFile.open(QIODevice::WriteOnly | QIODevice::Text);
        QTextStream(&miniFile) << "export var v = 1;\n";
        miniFile.close();
    }
}
} // namespace

TEST(PkgManagerTransitiveTest, InstallAllWithTransitiveInstallsDependencies) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString registryPath = QDir(tmpDir.path()).filePath("registry");
    QString packagesPath = QDir(tmpDir.path()).filePath("packages");
    createTransitiveRegistry(registryPath);

    // 创建顶层清单
    QString manifestPath = QDir(tmpDir.path()).filePath("minilang.pkg");
    writeFile(manifestPath,
              R"({"name":"root","version":"0.1.0","dependencies":[{"name":"app-pkg","version":"1.0.0"}]})");

    PkgConfig config;
    config.registryDir = registryPath.toStdString();
    config.packagesDir = packagesPath.toStdString();
    config.manifestPath = manifestPath.toStdString();
    PackageManager pm(config);

    std::string error;
    ASSERT_TRUE(pm.loadManifest("", &error)) << error;

    auto results = pm.installAllWithTransitive();
    // 应安装 app-pkg + lib-a（传递依赖）
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].packageName, "app-pkg");
    EXPECT_TRUE(results[0].ok);
    EXPECT_EQ(results[1].packageName, "lib-a");
    EXPECT_TRUE(results[1].ok);

    // 验证两个包都已安装
    EXPECT_TRUE(isPackageInstalled(packagesPath.toStdString(), "app-pkg"));
    EXPECT_TRUE(isPackageInstalled(packagesPath.toStdString(), "lib-a"));
}

TEST(PkgManagerTransitiveTest, InstallAllWithoutTransitiveSkipsTransitiveDeps) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString registryPath = QDir(tmpDir.path()).filePath("registry");
    QString packagesPath = QDir(tmpDir.path()).filePath("packages");
    createTransitiveRegistry(registryPath);

    QString manifestPath = QDir(tmpDir.path()).filePath("minilang.pkg");
    writeFile(manifestPath,
              R"({"name":"root","version":"0.1.0","dependencies":[{"name":"app-pkg","version":"1.0.0"}]})");

    PkgConfig config;
    config.registryDir = registryPath.toStdString();
    config.packagesDir = packagesPath.toStdString();
    config.manifestPath = manifestPath.toStdString();
    PackageManager pm(config);

    std::string error;
    ASSERT_TRUE(pm.loadManifest("", &error)) << error;

    // 扁平安装：只装 app-pkg，不装 lib-a
    auto results = pm.installAll();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].packageName, "app-pkg");
    EXPECT_TRUE(results[0].ok);

    EXPECT_TRUE(isPackageInstalled(packagesPath.toStdString(), "app-pkg"));
    EXPECT_FALSE(isPackageInstalled(packagesPath.toStdString(), "lib-a"));
}

TEST(PkgManagerTransitiveTest, InstallAllWithTransitiveHandlesCycle) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString registryPath = QDir(tmpDir.path()).filePath("registry");
    QString packagesPath = QDir(tmpDir.path()).filePath("packages");
    createCycleRegistry(registryPath);

    QString manifestPath = QDir(tmpDir.path()).filePath("minilang.pkg");
    writeFile(manifestPath,
              R"({"name":"root","version":"0.1.0","dependencies":[{"name":"cycle-a","version":"1.0.0"}]})");

    PkgConfig config;
    config.registryDir = registryPath.toStdString();
    config.packagesDir = packagesPath.toStdString();
    config.manifestPath = manifestPath.toStdString();
    PackageManager pm(config);

    std::string error;
    ASSERT_TRUE(pm.loadManifest("", &error)) << error;

    // 循环依赖不应导致无限递归（visited 集合检测）
    auto results = pm.installAllWithTransitive();
    // 至少安装 cycle-a，可能也安装 cycle-b（取决于顺序）
    EXPECT_GE(results.size(), 1u);
    EXPECT_EQ(results[0].packageName, "cycle-a");
    EXPECT_TRUE(results[0].ok);
    // 两包都应安装（cycle-b 作为 cycle-a 的传递依赖）
    EXPECT_TRUE(isPackageInstalled(packagesPath.toStdString(), "cycle-a"));
    EXPECT_TRUE(isPackageInstalled(packagesPath.toStdString(), "cycle-b"));
}

TEST(PkgManagerTransitiveTest, InstallAllWithTransitiveWithoutManifestFails) {
    PkgConfig config;
    PackageManager pm(config);
    auto results = pm.installAllWithTransitive();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_FALSE(results[0].ok);
    EXPECT_NE(results[0].errorMessage.find("未加载清单"), std::string::npos);
}

TEST(PkgManagerTransitiveTest, InstallAllWithTransitiveSkipsAlreadyInstalled) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString registryPath = QDir(tmpDir.path()).filePath("registry");
    QString packagesPath = QDir(tmpDir.path()).filePath("packages");
    createTransitiveRegistry(registryPath);

    QString manifestPath = QDir(tmpDir.path()).filePath("minilang.pkg");
    writeFile(manifestPath,
              R"({"name":"root","version":"0.1.0","dependencies":[{"name":"app-pkg","version":"1.0.0"}]})");

    PkgConfig config;
    config.registryDir = registryPath.toStdString();
    config.packagesDir = packagesPath.toStdString();
    config.manifestPath = manifestPath.toStdString();
    PackageManager pm(config);

    std::string error;
    ASSERT_TRUE(pm.loadManifest("", &error));

    // 第一次安装
    auto results1 = pm.installAllWithTransitive();
    EXPECT_EQ(results1.size(), 2u); // app-pkg + lib-a

    // 第二次安装：都应标记为已安装
    auto results2 = pm.installAllWithTransitive();
    EXPECT_EQ(results2.size(), 2u);
    for (const auto& r : results2) {
        EXPECT_TRUE(r.ok);
        EXPECT_TRUE(r.alreadyInstalled);
    }
}

// ============================================================
// P2-11 CLI --no-transitive 选项测试
// ============================================================

TEST(PkgCliTransitiveFlagTest, ParseNoTransitiveBeforeCommand) {
    const char* argv[] = {"minilang-pkg", "--no-transitive", "install"};
    CliArgs args = parseArgs(3, const_cast<char**>(argv));
    EXPECT_TRUE(args.noTransitive);
    EXPECT_EQ(args.command, "install");
}

TEST(PkgCliTransitiveFlagTest, ParseNoTransitiveAfterCommand) {
    const char* argv[] = {"minilang-pkg", "install", "--no-transitive"};
    CliArgs args = parseArgs(3, const_cast<char**>(argv));
    EXPECT_TRUE(args.noTransitive);
    EXPECT_EQ(args.command, "install");
}

TEST(PkgCliTransitiveFlagTest, DefaultIsTransitive) {
    const char* argv[] = {"minilang-pkg", "install"};
    CliArgs args = parseArgs(2, const_cast<char**>(argv));
    EXPECT_FALSE(args.noTransitive);
}

TEST(PkgCliTransitiveFlagTest, InstallCommandUsesTransitiveByDefault) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString registryPath = QDir(tmpDir.path()).filePath("registry");
    QString packagesPath = QDir(tmpDir.path()).filePath("packages");
    createTransitiveRegistry(registryPath);

    QString manifestPath = QDir(tmpDir.path()).filePath("minilang.pkg");
    writeFile(manifestPath,
              R"({"name":"root","version":"0.1.0","dependencies":[{"name":"app-pkg","version":"1.0.0"}]})");

    // 保持 QByteArray 活跃，避免 const char* 悬垂
    QByteArray regArg = registryPath.toLocal8Bit();
    QByteArray pkgArg = packagesPath.toLocal8Bit();
    QByteArray manArg = manifestPath.toLocal8Bit();
    const char* argv[] = {"minilang-pkg",   "install",          "--registry", regArg.constData(),
                          "--packages-dir", pkgArg.constData(), "--manifest", manArg.constData()};
    CliArgs args = parseArgs(8, const_cast<char**>(argv));
    CommandResult result = processInstall(args);
    EXPECT_EQ(result.exitCode, 0) << result.output;
    // 传递依赖 lib-a 应被安装
    EXPECT_TRUE(isPackageInstalled(packagesPath.toStdString(), "lib-a"));
    EXPECT_NE(result.output.find("含传递依赖"), std::string::npos);
}

TEST(PkgCliTransitiveFlagTest, InstallCommandWithNoTransitiveSkipsTransitiveDeps) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString registryPath = QDir(tmpDir.path()).filePath("registry");
    QString packagesPath = QDir(tmpDir.path()).filePath("packages");
    createTransitiveRegistry(registryPath);

    QString manifestPath = QDir(tmpDir.path()).filePath("minilang.pkg");
    writeFile(manifestPath,
              R"({"name":"root","version":"0.1.0","dependencies":[{"name":"app-pkg","version":"1.0.0"}]})");

    QByteArray regArg = registryPath.toLocal8Bit();
    QByteArray pkgArg = packagesPath.toLocal8Bit();
    QByteArray manArg = manifestPath.toLocal8Bit();
    const char* argv[] = {"minilang-pkg",     "install",          "--no-transitive",
                          "--registry",       regArg.constData(), "--packages-dir",
                          pkgArg.constData(), "--manifest",       manArg.constData()};
    CliArgs args = parseArgs(9, const_cast<char**>(argv));
    CommandResult result = processInstall(args);
    EXPECT_EQ(result.exitCode, 0) << result.output;
    // 传递依赖 lib-a 不应被安装
    EXPECT_FALSE(isPackageInstalled(packagesPath.toStdString(), "lib-a"));
    EXPECT_EQ(result.output.find("含传递依赖"), std::string::npos);
}

TEST(PkgCliHelpTest, HelpDocumentsVersionConstraints) {
    std::string h = helpString();
    EXPECT_NE(h.find("^1.0.0"), std::string::npos);
    EXPECT_NE(h.find("~1.0.0"), std::string::npos);
    EXPECT_NE(h.find("--no-transitive"), std::string::npos);
}

// ============================================================
// 拓展二期：Lockfile + 哈希校验
// ============================================================

namespace {

/// lockfile 测试环境：registry + packages + lockfile 路径组装
struct LockfileTestEnv {
    QString registryPath;
    QString packagesPath;
    QString lockfilePath;
    QByteArray regArg, pkgArg, lockArg;

    explicit LockfileTestEnv(const QTemporaryDir& tmpDir) {
        registryPath = QDir(tmpDir.path()).filePath("registry");
        packagesPath = QDir(tmpDir.path()).filePath("packages");
        lockfilePath = QDir(tmpDir.path()).filePath("minilang.lock");
        regArg = registryPath.toLocal8Bit();
        pkgArg = packagesPath.toLocal8Bit();
        lockArg = lockfilePath.toLocal8Bit();
    }

    /// 构建 install <spec> 参数
    CliArgs installArgs(const char* spec) {
        const char* argv[] = {"minilang-pkg",     "install",        spec,
                              "--registry",       regArg.constData(), "--packages-dir",
                              pkgArg.constData(), "--lockfile",       lockArg.constData()};
        return parseArgs(9, const_cast<char**>(argv));
    }

    /// 构建 verify 参数
    CliArgs verifyArgs() {
        const char* argv[] = {"minilang-pkg",     "verify",     "--registry",       regArg.constData(),
                              "--packages-dir",   pkgArg.constData(), "--lockfile", lockArg.constData()};
        return parseArgs(8, const_cast<char**>(argv));
    }
};

} // namespace

TEST(PkgLockfileTest, InstallCreatesLockfileWithHash) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    LockfileTestEnv env(tmpDir);
    createTestRegistry(env.registryPath);

    CommandResult result = processInstall(env.installArgs("math-utils"));
    EXPECT_EQ(result.exitCode, 0) << result.output;

    // lockfile 应已生成，含包名/版本/sha256 哈希
    ASSERT_TRUE(QFile::exists(env.lockfilePath));
    PkgConfig config;
    config.lockfilePath = env.lockfilePath.toStdString();
    PackageManager pm(config);
    ASSERT_TRUE(pm.loadLockfile());
    const LockEntry* entry = pm.lockfile().find("math-utils");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->version, "1.0.0");
    EXPECT_EQ(entry->hash.substr(0, 7), "sha256:");
}

TEST(PkgLockfileTest, ComputePackageHashDeterministicAndContentSensitive) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString pkgDir = QDir(tmpDir.path()).filePath("pkg");
    (void)QDir(pkgDir).mkpath(".");
    writeFile(QDir(pkgDir).filePath("a.mini"), "var x = 1;\n");
    writeFile(QDir(pkgDir).filePath("b.mini"), "var y = 2;\n");

    std::string h1 = PackageManager::computePackageHash(pkgDir.toStdString());
    std::string h2 = PackageManager::computePackageHash(pkgDir.toStdString());
    EXPECT_EQ(h1, h2);          // 确定性
    EXPECT_EQ(h1.substr(0, 7), "sha256:");

    // 内容变化 → 哈希变化
    writeFile(QDir(pkgDir).filePath("a.mini"), "var x = 999;\n");
    std::string h3 = PackageManager::computePackageHash(pkgDir.toStdString());
    EXPECT_NE(h1, h3);

    // 目录不存在 → 空字符串
    EXPECT_TRUE(PackageManager::computePackageHash((pkgDir + "_nonexistent").toStdString()).empty());
}

TEST(PkgLockfileTest, VersionPinningReproducesLockedVersion) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    LockfileTestEnv env(tmpDir);
    createVersionedRegistry(env.registryPath);

    // 首次安装 ^1.0.0 → 锁定 1.0.0（^1.0.0 不匹配 2.0.0）
    CommandResult r1 = processInstall(env.installArgs("ver-pkg@^1.0.0"));
    EXPECT_EQ(r1.exitCode, 0) << r1.output;

    // 删除已安装包，无版本重新安装：若无 lockfile 会选最新 2.0.0，
    // 有 lockfile 则锁定回 1.0.0（可重现安装）
    ASSERT_TRUE(QDir(QDir(env.packagesPath).filePath("ver-pkg")).removeRecursively());
    CommandResult r2 = processInstall(env.installArgs("ver-pkg"));
    EXPECT_EQ(r2.exitCode, 0) << r2.output;
    EXPECT_NE(r2.output.find("@1.0.0"), std::string::npos) << r2.output;
}

TEST(PkgLockfileTest, HashMismatchRejectsTamperedRegistry) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    LockfileTestEnv env(tmpDir);
    createTestRegistry(env.registryPath);

    // 首次安装锁定哈希
    CommandResult r1 = processInstall(env.installArgs("math-utils"));
    EXPECT_EQ(r1.exitCode, 0) << r1.output;

    // 篡改 registry 内容，删除已安装包后重新安装 → 哈希不匹配拒绝
    writeFile(QDir(QDir(env.registryPath).filePath("math-utils")).filePath("math-utils.mini"),
              "export fun add(a, b) { return a - b; } // 恶意篡改\n");
    ASSERT_TRUE(QDir(QDir(env.packagesPath).filePath("math-utils")).removeRecursively());

    CommandResult r2 = processInstall(env.installArgs("math-utils"));
    EXPECT_EQ(r2.exitCode, 2);
    EXPECT_NE(r2.output.find("哈希校验失败"), std::string::npos) << r2.output;
    // 回滚：不可信内容不得落盘
    EXPECT_FALSE(isPackageInstalled(env.packagesPath.toStdString(), "math-utils"));
}

TEST(PkgLockfileTest, VerifyPassesOnIntactInstall) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    LockfileTestEnv env(tmpDir);
    createTestRegistry(env.registryPath);

    ASSERT_EQ(processInstall(env.installArgs("math-utils")).exitCode, 0);
    CommandResult result = processVerify(env.verifyArgs());
    EXPECT_EQ(result.exitCode, 0) << result.output;
    EXPECT_NE(result.output.find("校验通过"), std::string::npos);
}

TEST(PkgLockfileTest, VerifyDetectsTamperedInstalledPackage) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    LockfileTestEnv env(tmpDir);
    createTestRegistry(env.registryPath);

    ASSERT_EQ(processInstall(env.installArgs("math-utils")).exitCode, 0);
    // 篡改已安装包内容
    writeFile(QDir(QDir(env.packagesPath).filePath("math-utils")).filePath("math-utils.mini"),
              "export fun add(a, b) { return 0; }\n");

    CommandResult result = processVerify(env.verifyArgs());
    EXPECT_EQ(result.exitCode, 2);
    EXPECT_NE(result.output.find("哈希不匹配"), std::string::npos) << result.output;
}

TEST(PkgLockfileTest, VerifyDetectsMissingPackage) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    LockfileTestEnv env(tmpDir);
    createTestRegistry(env.registryPath);

    ASSERT_EQ(processInstall(env.installArgs("math-utils")).exitCode, 0);
    ASSERT_TRUE(QDir(QDir(env.packagesPath).filePath("math-utils")).removeRecursively());

    CommandResult result = processVerify(env.verifyArgs());
    EXPECT_EQ(result.exitCode, 2);
    EXPECT_NE(result.output.find("未安装"), std::string::npos) << result.output;
}

TEST(PkgLockfileTest, VerifyWithoutLockfileFails) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    LockfileTestEnv env(tmpDir);

    CommandResult result = processVerify(env.verifyArgs());
    EXPECT_EQ(result.exitCode, 2);
    EXPECT_NE(result.output.find("lockfile 不存在"), std::string::npos) << result.output;
}

TEST(PkgLockfileTest, CorruptLockfileRejected) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    LockfileTestEnv env(tmpDir);
    createTestRegistry(env.registryPath);
    writeFile(env.lockfilePath, "{ 损坏的 json !!!");

    CommandResult result = processInstall(env.installArgs("math-utils"));
    EXPECT_EQ(result.exitCode, 2);
    EXPECT_NE(result.output.find("lockfile 解析失败"), std::string::npos) << result.output;
}

TEST(PkgLockfileTest, HelpDocumentsLockfileAndVerify) {
    std::string h = helpString();
    EXPECT_NE(h.find("--lockfile"), std::string::npos);
    EXPECT_NE(h.find("verify"), std::string::npos);
    EXPECT_NE(h.find("minilang.lock"), std::string::npos);
}
