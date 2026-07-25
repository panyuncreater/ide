// ============================================================
// cli/pkg_core.cpp - minilang-pkg 包管理器核心逻辑实现
// ------------------------------------------------------------
// 实现 minilang.pkg 清单文件解析/生成 + 本地 registry 包安装 +
// 模块路径解析 + CLI 命令处理。
//
// 使用 Qt6::Core 的 QJsonDocument 处理 JSON，QFile/QDir 处理文件。
// ============================================================
#include "cli/pkg_core.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QTextStream>

#include <algorithm>

namespace minilang_pkg {

// ============================================================
// 辅助函数实现
// ============================================================

std::string versionString() {
    return "minilang-pkg 1.0.0 (R165)";
}

std::string helpString() {
    return R"(minilang-pkg - MiniLang 包管理器

用法:
  minilang-pkg <command> [options] [args]

命令:
  install [package]   安装包（从 registry 或 file: 协议）
                      无 package 参数时安装清单中所有依赖（含传递依赖）
  list                列出已安装的包
  init [name]         初始化 minilang.pkg 清单文件
  add <package>       添加依赖到清单并安装

选项:
  --registry <dir>        指定本地 registry 目录
  --packages-dir <dir>    指定包安装目录（默认 minilang_packages）
  --manifest <path>       指定清单文件路径（默认 minilang.pkg）
  --no-transitive         安装清单依赖时跳过传递依赖（默认安装传递依赖）
  --help                  显示帮助信息
  --version               显示版本信息

包描述符格式:
  name                  最新版本
  name@1.0.0            指定版本
  file:./path/to/pkg    从本地路径安装

版本约束格式（在清单 dependencies[].version 中使用）:
  1.0.0     精确匹配
  ^1.0.0    兼容版本（>=1.0.0 <2.0.0，同主版本）
  ~1.0.0    近似版本（>=1.0.0 <1.1.0，同次版本）
  >=1.0.0   最低版本
  >1.0.0    高于指定版本
  <=2.0.0   最高版本
  <2.0.0    低于指定版本
  *         任意版本

清单文件格式 (minilang.pkg, JSON):
  {
    "name": "my-project",
    "version": "0.1.0",
    "description": "项目描述",
    "dependencies": [
      { "name": "math-utils", "version": "^1.0.0" }
    ]
  }

示例:
  minilang-pkg init my-project
  minilang-pkg add math-utils@1.0.0
  minilang-pkg install
  minilang-pkg install --no-transitive
  minilang-pkg list

退出码:
  0  成功
  1  警告（包已安装等）
  2  错误（清单解析失败、包未找到等）
)";
}

PackageInfo parsePackageSpec(const std::string& spec) {
    PackageInfo info;
    // 检查 file: 协议前缀
    if (spec.substr(0, 5) == "file:") {
        info.source = spec;
        info.name = QFileInfo(QString::fromStdString(spec.substr(5))).fileName().toStdString();
        return info;
    }
    // 解析 name@version 格式
    size_t atPos = spec.find('@');
    if (atPos != std::string::npos) {
        info.name = spec.substr(0, atPos);
        info.version = spec.substr(atPos + 1);
    } else {
        info.name = spec;
    }
    info.source = "registry:" + info.name;
    return info;
}

// ============================================================
// JSON 解析/序列化
// ============================================================

Manifest parseManifest(const std::string& json, std::string* errorMessage) {
    Manifest manifest;
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(json), &parseError);

    if (parseError.error != QJsonParseError::NoError) {
        if (errorMessage) {
            *errorMessage = "JSON 解析错误: " + parseError.errorString().toStdString();
        }
        return manifest;
    }

    if (!doc.isObject()) {
        if (errorMessage) {
            *errorMessage = "清单文件根节点必须是 JSON 对象";
        }
        return manifest;
    }

    QJsonObject obj = doc.object();
    manifest.name = obj.value("name").toString().toStdString();
    manifest.version = obj.value("version").toString("0.1.0").toStdString();
    manifest.description = obj.value("description").toString().toStdString();

    QJsonArray deps = obj.value("dependencies").toArray();
    for (const QJsonValue& dep : deps) {
        if (!dep.isObject())
            continue;
        QJsonObject depObj = dep.toObject();
        PackageInfo pkg;
        pkg.name = depObj.value("name").toString().toStdString();
        pkg.version = depObj.value("version").toString().toStdString();
        pkg.source = depObj.value("source").toString().toStdString();
        pkg.description = depObj.value("description").toString().toStdString();
        if (!pkg.name.empty()) {
            manifest.dependencies.push_back(std::move(pkg));
        }
    }

    if (manifest.name.empty() && errorMessage) {
        *errorMessage = "清单文件缺少 name 字段";
    }

    return manifest;
}

std::string manifestToJson(const Manifest& manifest) {
    QJsonObject obj;
    obj["name"] = QString::fromStdString(manifest.name);
    obj["version"] = QString::fromStdString(manifest.version);
    if (!manifest.description.empty()) {
        obj["description"] = QString::fromStdString(manifest.description);
    }

    // 空依赖列表时省略 dependencies 字段（与 description 省略逻辑一致）
    if (!manifest.dependencies.empty()) {
        QJsonArray deps;
        for (const auto& dep : manifest.dependencies) {
            QJsonObject depObj;
            depObj["name"] = QString::fromStdString(dep.name);
            if (!dep.version.empty()) {
                depObj["version"] = QString::fromStdString(dep.version);
            }
            if (!dep.source.empty()) {
                depObj["source"] = QString::fromStdString(dep.source);
            }
            if (!dep.description.empty()) {
                depObj["description"] = QString::fromStdString(dep.description);
            }
            deps.append(depObj);
        }
        obj["dependencies"] = deps;
    }

    QJsonDocument doc(obj);
    return QString::fromUtf8(doc.toJson(QJsonDocument::Indented)).toStdString();
}

// ============================================================
// 文件系统辅助
// ============================================================

bool isPackageInstalled(const std::string& packagesDir, const std::string& pkgName) {
    if (packagesDir.empty() || pkgName.empty())
        return false;
    QString pkgPath = QDir(QString::fromStdString(packagesDir)).filePath(QString::fromStdString(pkgName));
    QString pkgJsonPath = QDir(pkgPath).filePath("pkg.json");
    return QFile::exists(pkgJsonPath);
}

std::string getPackageInstallPath(const std::string& packagesDir, const std::string& pkgName) {
    QDir pkgDir = QDir(QString::fromStdString(packagesDir)).filePath(QString::fromStdString(pkgName));
    return pkgDir.absolutePath().toStdString();
}

// ============================================================
// PackageManager 实现
// ============================================================

PackageManager::PackageManager(PkgConfig config) : config_(std::move(config)) {}

bool PackageManager::loadManifest(const std::string& path, std::string* errorMessage) {
    std::string actualPath = path.empty() ? config_.manifestPath : path;
    if (actualPath.empty()) {
        if (errorMessage)
            *errorMessage = "清单文件路径为空";
        return false;
    }

    QFile file(QString::fromStdString(actualPath));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorMessage) {
            *errorMessage = "无法打开清单文件: " + actualPath + " (" + file.errorString().toStdString() + ")";
        }
        return false;
    }

    std::string content = QString::fromUtf8(file.readAll()).toStdString();
    file.close();

    std::string parseError;
    manifest_ = parseManifest(content, &parseError);
    if (!parseError.empty()) {
        if (errorMessage)
            *errorMessage = parseError;
        return false;
    }

    manifestLoaded_ = true;
    return true;
}

bool PackageManager::saveManifest(const std::string& path, std::string* errorMessage) const {
    std::string actualPath = path.empty() ? config_.manifestPath : path;
    if (actualPath.empty()) {
        if (errorMessage)
            *errorMessage = "清单文件路径为空";
        return false;
    }

    std::string json = manifestToJson(manifest_);
    return writeFile(actualPath, json, errorMessage);
}

std::string PackageManager::resolvePackagePath(const std::string& pkgName) const {
    QDir pkgDir = QDir(QString::fromStdString(config_.packagesDir)).filePath(QString::fromStdString(pkgName));
    return pkgDir.absolutePath().toStdString();
}

std::string PackageManager::findInRegistry(const std::string& pkgName, const std::string& version) const {
    if (config_.registryDir.empty())
        return {};

    QDir registryDir(QString::fromStdString(config_.registryDir));
    if (!registryDir.exists())
        return {};

    QString pkgDirName = QString::fromStdString(pkgName);
    QDir pkgDir = registryDir.filePath(pkgDirName);
    if (!pkgDir.exists())
        return {};

    // 如果指定了版本，检查版本目录
    if (!version.empty()) {
        QDir versionDir = pkgDir.filePath(QString::fromStdString(version));
        if (versionDir.exists()) {
            return versionDir.absolutePath().toStdString();
        }
        // 回退：检查包目录下的 pkg.json version 字段是否匹配
        // 支持无版本子目录的 registry（version 声明在 pkg.json 内）
        QString pkgJsonPath = pkgDir.filePath("pkg.json");
        if (QFile::exists(pkgJsonPath)) {
            QFile pkgJson(pkgJsonPath);
            if (pkgJson.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QJsonDocument doc = QJsonDocument::fromJson(pkgJson.readAll());
                pkgJson.close();
                if (doc.isObject()) {
                    QString pkgVersion = doc.object().value("version").toString();
                    if (pkgVersion == QString::fromStdString(version)) {
                        return pkgDir.absolutePath().toStdString();
                    }
                }
            }
        }
        return {};
    }

    // 未指定版本：优先读取 pkg.json 获取版本信息
    // 如果包目录直接在 registry/<pkgName>/ 下有 pkg.json，直接返回
    QString pkgJsonPath = pkgDir.filePath("pkg.json");
    if (QFile::exists(pkgJsonPath)) {
        return pkgDir.absolutePath().toStdString();
    }

    // 否则查找版本子目录，取第一个（按名称排序，最新版本优先）
    QStringList filters;
    filters << "1.*" << "0.*" << "2.*" << "3.*";
    QDir::Filters dirFilters = QDir::Dirs | QDir::NoSymLinks | QDir::NoDotAndDotDot;
    QStringList entries = pkgDir.entryList(filters, dirFilters, QDir::Name | QDir::Reversed);
    if (!entries.isEmpty()) {
        QDir versionDir = pkgDir.filePath(entries.first());
        return versionDir.absolutePath().toStdString();
    }

    // 兜底：直接返回包目录（无版本管理）
    return pkgDir.absolutePath().toStdString();
}

bool PackageManager::copyDirectory(const std::string& src, const std::string& dst, std::string* errorMessage) const {
    QDir srcDir(QString::fromStdString(src));
    if (!srcDir.exists()) {
        if (errorMessage)
            *errorMessage = "源目录不存在: " + src;
        return false;
    }

    QDir dstDir(QString::fromStdString(dst));
    if (!dstDir.exists()) {
        if (!dstDir.mkpath(".")) {
            if (errorMessage)
                *errorMessage = "无法创建目标目录: " + dst;
            return false;
        }
    }

    // 递归复制所有文件和子目录
    QDir::Filters filters = QDir::Files | QDir::Dirs | QDir::NoSymLinks | QDir::NoDotAndDotDot;
    QFileInfoList entries = srcDir.entryInfoList(filters);
    for (const QFileInfo& entry : entries) {
        QString srcPath = entry.absoluteFilePath();
        QString dstPath = dstDir.filePath(entry.fileName());

        if (entry.isDir()) {
            if (!copyDirectory(srcPath.toStdString(), dstPath.toStdString(), errorMessage)) {
                return false;
            }
        } else {
            if (QFile::exists(dstPath)) {
                QFile::remove(dstPath);
            }
            if (!QFile::copy(srcPath, dstPath)) {
                if (errorMessage) {
                    *errorMessage = "无法复制文件: " + srcPath.toStdString() + " -> " + dstPath.toStdString();
                }
                return false;
            }
        }
    }
    return true;
}

std::string PackageManager::readFile(const std::string& path) const {
    QFile file(QString::fromStdString(path));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    std::string content = QString::fromUtf8(file.readAll()).toStdString();
    file.close();
    return content;
}

bool PackageManager::writeFile(const std::string& path, const std::string& content, std::string* errorMessage) const {
    QFile file(QString::fromStdString(path));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (errorMessage) {
            *errorMessage = "无法写入文件: " + path + " (" + file.errorString().toStdString() + ")";
        }
        return false;
    }
    QTextStream stream(&file);
    // Qt6 QTextStream 默认 UTF-8，无需 setCodec
    stream << QString::fromStdString(content);
    stream.flush();
    file.close();
    return true;
}

InstallResult PackageManager::install(const std::string& packageSpec) {
    InstallResult result;
    PackageInfo pkgInfo = parsePackageSpec(packageSpec);
    result.packageName = pkgInfo.name;
    result.version = pkgInfo.version;

    if (pkgInfo.name.empty()) {
        result.errorMessage = "无效的包描述符: " + packageSpec;
        return result;
    }

    // 检查是否已安装
    if (isPackageInstalled(config_.packagesDir, pkgInfo.name)) {
        result.ok = true;
        result.alreadyInstalled = true;
        result.installPath = resolvePackagePath(pkgInfo.name);
        result.errorMessage = "包 " + pkgInfo.name + " 已安装";
        return result;
    }

    // 确定安装来源
    std::string srcPath;

    // file: 协议
    if (pkgInfo.source.substr(0, 5) == "file:") {
        srcPath = pkgInfo.source.substr(5);
        // 规范化路径
        QFileInfo srcInfo(QString::fromStdString(srcPath));
        if (!srcInfo.exists() || !srcInfo.isDir()) {
            result.errorMessage = "file: 协议源路径不存在或非目录: " + srcPath;
            return result;
        }
        srcPath = srcInfo.absoluteFilePath().toStdString();
    } else {
        // registry 模式
        // P2-11: 检测版本约束操作符（^/~/>=/>/<=/</*），使用约束匹配
        const std::string& ver = pkgInfo.version;
        bool isConstraint =
            !ver.empty() && (ver[0] == '^' || ver[0] == '~' || ver[0] == '>' || ver[0] == '<' || ver == "*");
        if (isConstraint) {
            srcPath = findInRegistryWithConstraint(pkgInfo.name, VersionConstraint::parse(ver));
        } else {
            srcPath = findInRegistry(pkgInfo.name, ver);
        }
        if (srcPath.empty()) {
            result.errorMessage = "在 registry 中未找到包: " + pkgInfo.name + (ver.empty() ? "" : "@" + ver);
            if (config_.registryDir.empty()) {
                result.errorMessage += "（未指定 --registry 目录）";
            }
            return result;
        }
    }

    // 复制到 packagesDir
    std::string dstPath = resolvePackagePath(pkgInfo.name);
    std::string copyError;
    if (!copyDirectory(srcPath, dstPath, &copyError)) {
        result.errorMessage = "安装失败: " + copyError;
        return result;
    }

    // 读取/更新 pkg.json 中的版本信息
    QString pkgJsonPath = QDir(QString::fromStdString(dstPath)).filePath("pkg.json");
    if (QFile::exists(pkgJsonPath)) {
        // 读取现有 pkg.json 获取版本
        QFile pkgJson(pkgJsonPath);
        if (pkgJson.open(QIODevice::ReadOnly | QIODevice::Text)) {
            std::string content = QString::fromUtf8(pkgJson.readAll()).toStdString();
            pkgJson.close();
            std::string parseError;
            QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(content));
            if (doc.isObject()) {
                QJsonObject obj = doc.object();
                // P2-11: 始终用 pkg.json 中的实际版本覆盖 result.version
                // （result.version 可能是约束字符串如 "^1.0.0"，非实际版本号）
                QString actualVer = obj.value("version").toString();
                if (!actualVer.isEmpty()) {
                    result.version = actualVer.toStdString();
                }
            }
        }
    }

    result.ok = true;
    result.installPath = dstPath;
    return result;
}

std::vector<InstallResult> PackageManager::installAll() {
    std::vector<InstallResult> results;
    if (!manifestLoaded_) {
        InstallResult r;
        r.errorMessage = "未加载清单文件";
        results.push_back(r);
        return results;
    }

    for (const auto& dep : manifest_.dependencies) {
        std::string spec = dep.name;
        if (!dep.version.empty()) {
            spec += "@" + dep.version;
        }
        if (dep.source.substr(0, 5) == "file:") {
            spec = dep.source;
        }
        results.push_back(install(spec));
    }
    return results;
}

ListResult PackageManager::listInstalled() const {
    ListResult result;
    QDir packagesDir(QString::fromStdString(config_.packagesDir));
    if (!packagesDir.exists()) {
        result.ok = true; // 空目录视为成功
        return result;
    }

    QDir::Filters filters = QDir::Dirs | QDir::NoSymLinks | QDir::NoDotAndDotDot;
    QStringList entries = packagesDir.entryList(filters, QDir::Name);
    for (const QString& entry : entries) {
        QString pkgJsonPath = packagesDir.filePath(entry + "/pkg.json");
        PackageInfo pkg;
        pkg.name = entry.toStdString();

        if (QFile::exists(pkgJsonPath)) {
            QFile pkgJson(pkgJsonPath);
            if (pkgJson.open(QIODevice::ReadOnly | QIODevice::Text)) {
                std::string content = QString::fromUtf8(pkgJson.readAll()).toStdString();
                pkgJson.close();
                QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(content));
                if (doc.isObject()) {
                    QJsonObject obj = doc.object();
                    pkg.version = obj.value("version").toString().toStdString();
                    pkg.description = obj.value("description").toString().toStdString();
                    pkg.name = obj.value("name").toString().toStdString();
                    if (pkg.name.empty()) {
                        pkg.name = entry.toStdString();
                    }
                }
            }
        }
        result.packages.push_back(std::move(pkg));
    }
    result.ok = true;
    return result;
}

std::string PackageManager::resolveModule(const std::string& modulePath) const {
    if (modulePath.empty() || config_.packagesDir.empty())
        return {};

    QDir packagesDir(QString::fromStdString(config_.packagesDir));
    QString qModulePath = QString::fromStdString(modulePath);

    // 含 "/" 的路径：直接按 packagesDir/modulePath 查找
    if (modulePath.find('/') != std::string::npos) {
        // 自动补 .mini 后缀
        QString candidate = packagesDir.filePath(qModulePath);
        if (QFile::exists(candidate)) {
            return QDir(candidate).absolutePath().toStdString();
        }
        if (!candidate.endsWith(".mini", Qt::CaseInsensitive)) {
            QString withExt = candidate + ".mini";
            if (QFile::exists(withExt)) {
                return QDir(withExt).absolutePath().toStdString();
            }
        }
        return {};
    }

    // 不含 "/" 的路径：按 packagesDir/modulePath/modulePath.mini 查找
    // 这是包的"入口模块"约定
    QDir pkgDir = packagesDir.filePath(qModulePath);
    if (!pkgDir.exists())
        return {};

    // 优先查找 packagesDir/modulePath/modulePath.mini
    QString entryFile = pkgDir.filePath(qModulePath + ".mini");
    if (QFile::exists(entryFile)) {
        return QDir(entryFile).absolutePath().toStdString();
    }

    // 兜底：查找 packagesDir/modulePath 下任意 .mini 文件
    QStringList miniFiles = pkgDir.entryList({"*.mini"}, QDir::Files);
    if (!miniFiles.isEmpty()) {
        QString first = pkgDir.filePath(miniFiles.first());
        return QDir(first).absolutePath().toStdString();
    }

    return {};
}

// ============================================================
// CLI 参数解析
// ============================================================

CliArgs parseArgs(int argc, char* argv[]) {
    CliArgs args;
    if (argc < 2) {
        args.showHelp = true;
        return args;
    }

    // 全局选项可以在命令之前或之后
    int i = 1;
    // 跳过命令前的全局选项
    while (i < argc) {
        QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == "--help" || arg == "-h") {
            args.showHelp = true;
            i++;
        } else if (arg == "--version" || arg == "-v") {
            args.showVersion = true;
            i++;
        } else if (arg == "--no-transitive") {
            args.noTransitive = true;
            i++;
        } else if (arg == "--registry") {
            if (i + 1 >= argc) {
                args.parseError = true;
                args.errorMessage = "--registry 需要参数";
                return args;
            }
            args.registryDir = QString::fromLocal8Bit(argv[++i]).toStdString();
            i++;
        } else if (arg == "--packages-dir") {
            if (i + 1 >= argc) {
                args.parseError = true;
                args.errorMessage = "--packages-dir 需要参数";
                return args;
            }
            args.packagesDir = QString::fromLocal8Bit(argv[++i]).toStdString();
            i++;
        } else if (arg == "--manifest") {
            if (i + 1 >= argc) {
                args.parseError = true;
                args.errorMessage = "--manifest 需要参数";
                return args;
            }
            args.manifestPath = QString::fromLocal8Bit(argv[++i]).toStdString();
            i++;
        } else if (arg.startsWith("--")) {
            args.parseError = true;
            args.errorMessage = "未知选项: " + arg.toStdString();
            return args;
        } else {
            // 第一个非选项参数是命令
            args.command = arg.toStdString();
            i++;
            break;
        }
    }

    // 收集命令参数
    while (i < argc) {
        QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == "--registry") {
            if (i + 1 >= argc) {
                args.parseError = true;
                args.errorMessage = "--registry 需要参数";
                return args;
            }
            args.registryDir = QString::fromLocal8Bit(argv[++i]).toStdString();
        } else if (arg == "--packages-dir") {
            if (i + 1 >= argc) {
                args.parseError = true;
                args.errorMessage = "--packages-dir 需要参数";
                return args;
            }
            args.packagesDir = QString::fromLocal8Bit(argv[++i]).toStdString();
        } else if (arg == "--manifest") {
            if (i + 1 >= argc) {
                args.parseError = true;
                args.errorMessage = "--manifest 需要参数";
                return args;
            }
            args.manifestPath = QString::fromLocal8Bit(argv[++i]).toStdString();
        } else if (arg == "--no-transitive") {
            args.noTransitive = true;
        } else if (arg == "--help" || arg == "-h") {
            args.showHelp = true;
        } else if (arg == "--version" || arg == "-v") {
            args.showVersion = true;
        } else if (arg.startsWith("--")) {
            args.parseError = true;
            args.errorMessage = "未知选项: " + arg.toStdString();
            return args;
        } else {
            args.args.push_back(arg.toStdString());
        }
        i++;
    }

    return args;
}

// ============================================================
// 命令处理
// ============================================================

CommandResult processCommand(const CliArgs& args) {
    CommandResult result;

    if (args.showHelp) {
        result.output = helpString();
        return result;
    }
    if (args.showVersion) {
        result.output = versionString();
        return result;
    }
    if (args.parseError) {
        result.exitCode = 2;
        result.output = "错误: " + args.errorMessage + "\n\n" + helpString();
        return result;
    }
    if (args.command.empty()) {
        result.output = helpString();
        return result;
    }

    if (args.command == "install") {
        return processInstall(args);
    } else if (args.command == "list") {
        return processList(args);
    } else if (args.command == "init") {
        return processInit(args);
    } else if (args.command == "add") {
        return processAdd(args);
    } else {
        result.exitCode = 2;
        result.output = "未知命令: " + args.command + "\n\n" + helpString();
        return result;
    }
}

PkgConfig buildConfig(const CliArgs& args) {
    PkgConfig config;
    if (!args.packagesDir.empty()) {
        config.packagesDir = args.packagesDir;
    }
    if (!args.registryDir.empty()) {
        config.registryDir = args.registryDir;
    }
    if (!args.manifestPath.empty()) {
        config.manifestPath = args.manifestPath;
    }
    return config;
}

CommandResult processInstall(const CliArgs& args) {
    CommandResult result;
    PkgConfig config = buildConfig(args);
    PackageManager pm(config);

    // 如果无参数，安装清单中所有依赖
    if (args.args.empty()) {
        std::string manifestError;
        if (!pm.loadManifest("", &manifestError)) {
            result.exitCode = 2;
            result.output = "错误: 无法加载清单文件: " + manifestError;
            return result;
        }

        // 默认安装传递依赖，--no-transitive 时回退到扁平安装
        auto results = args.noTransitive ? pm.installAll() : pm.installAllWithTransitive();
        bool allOk = true;
        bool anyWarning = false;
        std::string output = "安装依赖" + std::string(args.noTransitive ? "" : "（含传递依赖）") + ":\n";
        for (const auto& r : results) {
            if (r.ok) {
                if (r.alreadyInstalled) {
                    output += "  [跳过] " + r.packageName + " (已安装)\n";
                    anyWarning = true;
                } else {
                    output += "  [成功] " + r.packageName;
                    if (!r.version.empty()) {
                        output += "@" + r.version;
                    }
                    output += " -> " + r.installPath + "\n";
                }
            } else {
                output += "  [失败] " + r.packageName + ": " + r.errorMessage + "\n";
                allOk = false;
            }
        }
        result.output = output;
        if (!allOk) {
            result.exitCode = 2;
        } else if (anyWarning) {
            result.exitCode = 1;
        }
        return result;
    }

    // 安装指定包
    std::string spec = args.args[0];
    InstallResult installResult = pm.install(spec);
    if (installResult.ok) {
        if (installResult.alreadyInstalled) {
            result.exitCode = 1;
            result.output = "包 " + installResult.packageName + " 已安装（跳过）";
        } else {
            result.output = "成功安装 " + installResult.packageName;
            if (!installResult.version.empty()) {
                result.output += "@" + installResult.version;
            }
            result.output += " -> " + installResult.installPath;
        }
    } else {
        result.exitCode = 2;
        result.output = "错误: " + installResult.errorMessage;
    }
    return result;
}

CommandResult processList(const CliArgs& args) {
    CommandResult result;
    PkgConfig config = buildConfig(args);
    PackageManager pm(config);

    ListResult listResult = pm.listInstalled();
    if (!listResult.ok) {
        result.exitCode = 2;
        result.output = "错误: " + listResult.errorMessage;
        return result;
    }

    if (listResult.packages.empty()) {
        result.output = "未安装任何包（目录: " + config.packagesDir + "）";
        return result;
    }

    std::string output = "已安装的包:\n";
    for (const auto& pkg : listResult.packages) {
        output += "  " + pkg.name;
        if (!pkg.version.empty()) {
            output += "@" + pkg.version;
        }
        if (!pkg.description.empty()) {
            output += " - " + pkg.description;
        }
        output += "\n";
    }
    result.output = output;
    return result;
}

CommandResult processInit(const CliArgs& args) {
    CommandResult result;
    PkgConfig config = buildConfig(args);
    PackageManager pm(config);

    // 检查清单文件是否已存在
    if (QFile::exists(QString::fromStdString(config.manifestPath))) {
        result.exitCode = 1;
        result.output = "清单文件已存在: " + config.manifestPath + "（跳过初始化）";
        return result;
    }

    // 创建默认清单
    Manifest manifest;
    manifest.name = args.args.empty() ? "my-project" : args.args[0];
    manifest.version = "0.1.0";
    manifest.description = "";

    pm.manifest() = manifest;

    std::string saveError;
    if (!pm.saveManifest("", &saveError)) {
        result.exitCode = 2;
        result.output = "错误: " + saveError;
        return result;
    }

    result.output =
        "已创建清单文件: " + config.manifestPath + "\n项目名: " + manifest.name + "\n版本: " + manifest.version;
    return result;
}

CommandResult processAdd(const CliArgs& args) {
    CommandResult result;
    if (args.args.empty()) {
        result.exitCode = 2;
        result.output = "错误: add 命令需要包描述符参数\n用法: minilang-pkg add <package>";
        return result;
    }

    PkgConfig config = buildConfig(args);
    PackageManager pm(config);

    // 加载现有清单（如果存在）
    std::string manifestError;
    if (QFile::exists(QString::fromStdString(config.manifestPath))) {
        if (!pm.loadManifest("", &manifestError)) {
            result.exitCode = 2;
            result.output = "错误: 加载清单失败: " + manifestError;
            return result;
        }
    } else {
        // 无清单则初始化
        pm.manifest().name = "my-project";
        pm.manifest().version = "0.1.0";
    }

    std::string spec = args.args[0];
    PackageInfo pkgInfo = parsePackageSpec(spec);

    // 检查是否已在清单中
    auto& deps = pm.manifest().dependencies;
    auto it = std::find_if(deps.begin(), deps.end(), [&](const PackageInfo& d) { return d.name == pkgInfo.name; });
    if (it != deps.end()) {
        // 更新版本
        it->version = pkgInfo.version;
        it->source = pkgInfo.source;
    } else {
        deps.push_back(pkgInfo);
    }

    // 保存清单
    if (!pm.saveManifest("", &manifestError)) {
        result.exitCode = 2;
        result.output = "错误: 保存清单失败: " + manifestError;
        return result;
    }

    // 安装包
    InstallResult installResult = pm.install(spec);
    if (installResult.ok) {
        result.output = "已添加依赖 " + pkgInfo.name;
        if (!pkgInfo.version.empty()) {
            result.output += "@" + pkgInfo.version;
        }
        result.output += " 到清单";
        if (!installResult.alreadyInstalled) {
            result.output += " 并安装 -> " + installResult.installPath;
        } else {
            result.output += "（包已安装）";
        }
    } else {
        result.exitCode = 2;
        result.output = "已添加依赖到清单，但安装失败: " + installResult.errorMessage;
    }
    return result;
}

// ============================================================
// P2-11 版本约束与传递依赖解析实现
// ============================================================

Version Version::parse(const std::string& str) {
    Version v;
    int field = 0; // 0=major, 1=minor, 2=patch
    int num = 0;
    bool hasNum = false;
    for (char c : str) {
        if (c >= '0' && c <= '9') {
            num = num * 10 + (c - '0');
            hasNum = true;
        } else if (c == '.') {
            if (hasNum) {
                if (field == 0)
                    v.major = num;
                else if (field == 1)
                    v.minor = num;
                num = 0;
                hasNum = false;
                ++field;
            }
        } else {
            break; // 非数字非点号，停止解析
        }
    }
    if (hasNum) {
        if (field == 0)
            v.major = num;
        else if (field == 1)
            v.minor = num;
        else if (field >= 2)
            v.patch = num;
    }
    return v;
}

std::string Version::toString() const {
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

VersionConstraint VersionConstraint::parse(const std::string& str) {
    VersionConstraint c;
    if (str.empty() || str == "*") {
        c.op = ConstraintOp::Any;
        return c;
    }
    if (str[0] == '^') {
        c.op = ConstraintOp::Caret;
        c.version = Version::parse(str.substr(1));
        return c;
    }
    if (str[0] == '~') {
        c.op = ConstraintOp::Tilde;
        c.version = Version::parse(str.substr(1));
        return c;
    }
    if (str.size() >= 2 && str[0] == '>' && str[1] == '=') {
        c.op = ConstraintOp::GreaterEq;
        c.version = Version::parse(str.substr(2));
        return c;
    }
    if (str.size() >= 2 && str[0] == '<' && str[1] == '=') {
        c.op = ConstraintOp::LessEq;
        c.version = Version::parse(str.substr(2));
        return c;
    }
    if (str[0] == '>') {
        c.op = ConstraintOp::Greater;
        c.version = Version::parse(str.substr(1));
        return c;
    }
    if (str[0] == '<') {
        c.op = ConstraintOp::Less;
        c.version = Version::parse(str.substr(1));
        return c;
    }
    // 默认：精确匹配
    c.op = ConstraintOp::Exact;
    c.version = Version::parse(str);
    return c;
}

bool VersionConstraint::matches(const Version& ver) const {
    switch (op) {
    case ConstraintOp::Any:
        return true;
    case ConstraintOp::Exact:
        return ver == version;
    case ConstraintOp::Caret:
        // ^1.0.0 → >=1.0.0 <2.0.0
        // ^0.1.0 → >=0.1.0 <0.2.0
        // ^0.0.1 → >=0.0.1 <0.0.2
        if (ver < version)
            return false;
        if (version.major > 0)
            return ver.major == version.major;
        if (version.minor > 0)
            return ver.major == 0 && ver.minor == version.minor;
        return ver.major == 0 && ver.minor == 0 && ver.patch == version.patch;
    case ConstraintOp::Tilde:
        // ~1.0.0 → >=1.0.0 <1.1.0
        if (ver < version)
            return false;
        return ver.major == version.major && ver.minor == version.minor;
    case ConstraintOp::GreaterEq:
        return ver >= version;
    case ConstraintOp::Greater:
        return ver > version;
    case ConstraintOp::LessEq:
        return ver <= version;
    case ConstraintOp::Less:
        return ver < version;
    }
    return false;
}

std::string VersionConstraint::toString() const {
    switch (op) {
    case ConstraintOp::Any:
        return "*";
    case ConstraintOp::Exact:
        return version.toString();
    case ConstraintOp::Caret:
        return "^" + version.toString();
    case ConstraintOp::Tilde:
        return "~" + version.toString();
    case ConstraintOp::GreaterEq:
        return ">=" + version.toString();
    case ConstraintOp::Greater:
        return ">" + version.toString();
    case ConstraintOp::LessEq:
        return "<=" + version.toString();
    case ConstraintOp::Less:
        return "<" + version.toString();
    }
    return "*";
}

std::optional<Version> PackageManager::getInstalledVersion(const std::string& pkgName) const {
    QString pkgJsonPath = QDir(QString::fromStdString(resolvePackagePath(pkgName))).filePath("pkg.json");
    if (!QFile::exists(pkgJsonPath))
        return std::nullopt;
    QFile pkgJson(pkgJsonPath);
    if (!pkgJson.open(QIODevice::ReadOnly | QIODevice::Text))
        return std::nullopt;
    QJsonDocument doc = QJsonDocument::fromJson(pkgJson.readAll());
    pkgJson.close();
    if (!doc.isObject())
        return std::nullopt;
    QString verStr = doc.object().value("version").toString();
    if (verStr.isEmpty())
        return std::nullopt;
    return Version::parse(verStr.toStdString());
}

bool PackageManager::checkVersionConstraint(const std::string& pkgName, const VersionConstraint& constraint) const {
    auto installed = getInstalledVersion(pkgName);
    if (!installed)
        return true; // 未安装，不构成冲突
    return constraint.matches(*installed);
}

std::string PackageManager::findInRegistryWithConstraint(const std::string& pkgName,
                                                         const VersionConstraint& constraint) const {
    if (config_.registryDir.empty())
        return {};

    QDir registryDir(QString::fromStdString(config_.registryDir));
    if (!registryDir.exists())
        return {};

    QDir pkgDir = registryDir.filePath(QString::fromStdString(pkgName));
    if (!pkgDir.exists())
        return {};

    // Any 约束：回退到现有 findInRegistry 逻辑
    if (constraint.op == ConstraintOp::Any)
        return findInRegistry(pkgName, "");

    // 收集所有版本子目录
    QDir::Filters dirFilters = QDir::Dirs | QDir::NoSymLinks | QDir::NoDotAndDotDot;
    QStringList entries = pkgDir.entryList(dirFilters, QDir::Name);

    Version bestVersion;
    std::string bestPath;
    bool found = false;

    for (const QString& entry : entries) {
        Version ver = Version::parse(entry.toStdString());
        if (constraint.matches(ver)) {
            if (!found || ver > bestVersion) {
                bestVersion = ver;
                bestPath = pkgDir.filePath(entry).toStdString();
                found = true;
            }
        }
    }

    if (found)
        return bestPath;

    // 回退：检查包目录本身的 pkg.json 版本是否匹配
    QString pkgJsonPath = pkgDir.filePath("pkg.json");
    if (QFile::exists(pkgJsonPath)) {
        QFile pkgJson(pkgJsonPath);
        if (pkgJson.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QJsonDocument doc = QJsonDocument::fromJson(pkgJson.readAll());
            pkgJson.close();
            if (doc.isObject()) {
                QString verStr = doc.object().value("version").toString();
                Version ver = Version::parse(verStr.toStdString());
                if (constraint.matches(ver))
                    return pkgDir.absolutePath().toStdString();
            }
        }
    }

    return {};
}

void PackageManager::installTransitiveDeps(const std::string& pkgPath, std::unordered_set<std::string>& visited,
                                           std::vector<InstallResult>& results) {
    // 读取已安装包的 minilang.pkg 清单
    QString manifestPath = QDir(QString::fromStdString(pkgPath)).filePath("minilang.pkg");
    if (!QFile::exists(manifestPath))
        return; // 无清单，无传递依赖

    QFile manifestFile(manifestPath);
    if (!manifestFile.open(QIODevice::ReadOnly | QIODevice::Text))
        return;

    std::string content = QString::fromUtf8(manifestFile.readAll()).toStdString();
    manifestFile.close();

    std::string parseError;
    Manifest depManifest = parseManifest(content, &parseError);
    if (!parseError.empty())
        return; // 清单解析失败，跳过传递依赖

    // 递归安装每个依赖
    for (const auto& dep : depManifest.dependencies) {
        // 循环依赖检测
        if (visited.count(dep.name))
            continue;
        visited.insert(dep.name);

        // 构建安装描述符
        std::string spec = dep.name;
        if (!dep.version.empty() && dep.source.substr(0, 5) != "file:") {
            spec += "@" + dep.version;
        }
        if (dep.source.substr(0, 5) == "file:") {
            spec = dep.source;
        }

        InstallResult result = install(spec);
        results.push_back(result);

        // P2-11: 即使包已安装也要检查传递依赖（它们可能尚未安装）
        if (result.ok && !result.installPath.empty()) {
            installTransitiveDeps(result.installPath, visited, results);
        }
    }
}

std::vector<InstallResult> PackageManager::installAllWithTransitive() {
    std::vector<InstallResult> results;
    if (!manifestLoaded_) {
        InstallResult r;
        r.errorMessage = "未加载清单文件";
        results.push_back(r);
        return results;
    }

    std::unordered_set<std::string> visited;

    for (const auto& dep : manifest_.dependencies) {
        if (visited.count(dep.name))
            continue;
        visited.insert(dep.name);

        std::string spec = dep.name;
        if (!dep.version.empty() && dep.source.substr(0, 5) != "file:") {
            spec += "@" + dep.version;
        }
        if (dep.source.substr(0, 5) == "file:") {
            spec = dep.source;
        }

        InstallResult result = install(spec);
        results.push_back(result);

        // P2-11: 即使包已安装也要检查传递依赖（它们可能尚未安装）
        if (result.ok && !result.installPath.empty()) {
            installTransitiveDeps(result.installPath, visited, results);
        }
    }

    return results;
}

} // namespace minilang_pkg
