// ============================================================
// cli/pkg_core.h - minilang-pkg 包管理器核心逻辑（可测试）
// ------------------------------------------------------------
// R165 工具链拓展：第 8 个 CLI 工具 minilang-pkg。
// 实现 minilang.pkg 清单文件 + 本地 registry 包安装 + 模块路径解析。
//
// 设计要点：
//   - Manifest: minilang.pkg 清单文件（JSON 格式，声明项目名/版本/依赖）
//   - PackageManager: 安装/列表/解析模块路径
//   - resolveModule: 供 Interpreter/Compiler 的 moduleLoader 注入使用
//   - 本地 registry 模式：从 registry 目录复制包到 packages 目录
//     （避免网络依赖，便于教学和单元测试）
//
// 与模块系统集成：
//   现有 moduleLoader 是 std::function<std::string(const std::string&)>，
//   包管理器通过 resolveModule 扩展搜索路径，零侵入核心代码：
//     interpreter->setModuleLoader([&pm](path) {
//         std::string resolved = pm.resolveModule(path);
//         if (!resolved.empty()) return readfile(resolved);
//         return std::string{};  // 回退到原有逻辑
//     });
//
// 退出码约定：
//   0  成功
//   1  警告（包已安装、版本不匹配等）
//   2  错误（清单解析失败、包未找到、文件 I/O 错误等）
// ============================================================
#pragma once

#include <string>
#include <vector>

namespace minilang_pkg {

/// 包描述符
struct PackageInfo {
    std::string name;        ///< 包名（如 "math-utils"）
    std::string version;     ///< 版本号（如 "1.0.0"，空表示最新）
    std::string source;      ///< 源路径（registry:name 或 file:./path）
    std::string description; ///< 描述（可选）

    bool operator==(const PackageInfo&) const = default;
};

/// 清单文件（minilang.pkg，JSON 格式）
struct Manifest {
    std::string name;                      ///< 项目名
    std::string version;                   ///< 项目版本（默认 "0.1.0"）
    std::string description;               ///< 项目描述（可选）
    std::vector<PackageInfo> dependencies; ///< 依赖列表

    bool operator==(const Manifest&) const = default;
};

/// 安装结果
struct InstallResult {
    bool ok = false;
    std::string packageName;
    std::string version;
    std::string installPath; ///< 安装后的包目录绝对路径
    std::string errorMessage;
    bool alreadyInstalled = false; ///< 包已安装（跳过）
};

/// 列表结果
struct ListResult {
    std::vector<PackageInfo> packages;
    std::string errorMessage;
    bool ok = false;
};

/// 包管理器配置
struct PkgConfig {
    std::string packagesDir = "minilang_packages"; ///< 包安装目录（项目级）
    std::string registryDir;                       ///< 本地仓库目录（可选）
    std::string manifestPath = "minilang.pkg";     ///< 清单文件路径
};

/// 包管理器
class PackageManager {
public:
    explicit PackageManager(PkgConfig config = {});

    // ---- 清单操作 ----

    /// 加载清单文件（JSON 格式）
    /// path 为空时使用 config_.manifestPath
    /// 成功返回 true，失败设置 errorMessage
    bool loadManifest(const std::string& path = "", std::string* errorMessage = nullptr);

    /// 保存清单文件（JSON 格式）
    /// path 为空时使用 config_.manifestPath
    bool saveManifest(const std::string& path = "", std::string* errorMessage = nullptr) const;

    /// 是否已加载清单
    bool hasManifest() const { return manifestLoaded_; }

    /// 获取清单（只读）
    const Manifest& manifest() const { return manifest_; }

    /// 获取清单（可变）
    Manifest& manifest() { return manifest_; }

    // ---- 安装 ----

    /// 安装单个包
    /// packageSpec 格式："name" 或 "name@version"
    /// 安装来源优先级：registry > file: 协议
    InstallResult install(const std::string& packageSpec);

    /// 安装清单中所有依赖
    /// 返回每个依赖的安装结果
    std::vector<InstallResult> installAll();

    // ---- 列表 ----

    /// 列出已安装的包（扫描 packagesDir 下的 pkg.json 元数据）
    ListResult listInstalled() const;

    // ---- 模块路径解析 ----

    /// 解析模块路径（供 moduleLoader 注入使用）
    /// 输入: modulePath（如 "math-utils/utils.mini" 或 "math-utils"）
    /// 输出: packagesDir 中的实际文件绝对路径，空字符串表示未找到
    ///
    /// 解析规则：
    ///   1. 若 modulePath 含 "/"，按 packagesDir/modulePath 查找
    ///   2. 若 modulePath 不含 "/"，按 packagesDir/modulePath/modulePath.mini 查找
    ///   3. 自动补 .mini 后缀（若未带后缀）
    std::string resolveModule(const std::string& modulePath) const;

    // ---- 配置 ----

    const PkgConfig& config() const { return config_; }
    void setConfig(const PkgConfig& config) { config_ = config; }

private:
    PkgConfig config_;
    Manifest manifest_;
    bool manifestLoaded_ = false;

    /// 获取包在 packagesDir 中的目录路径
    std::string resolvePackagePath(const std::string& pkgName) const;

    /// 在 registry 中查找包
    /// 返回 registry 中包目录的绝对路径，空字符串表示未找到
    std::string findInRegistry(const std::string& pkgName, const std::string& version) const;

    /// 递归复制目录
    bool copyDirectory(const std::string& src, const std::string& dst, std::string* errorMessage = nullptr) const;

    /// 读取文件内容
    std::string readFile(const std::string& path) const;

    /// 写入文件
    bool writeFile(const std::string& path, const std::string& content, std::string* errorMessage = nullptr) const;
};

// ============================================================
// CLI 参数解析与命令处理
// ============================================================

/// CLI 参数
struct CliArgs {
    std::string command;           ///< 命令（install/list/init/add/help/version）
    std::vector<std::string> args; ///< 命令参数
    std::string registryDir;       ///< --registry <dir>
    std::string packagesDir;       ///< --packages-dir <dir>
    std::string manifestPath;      ///< --manifest <path>
    bool showHelp = false;
    bool showVersion = false;
    bool parseError = false;
    std::string errorMessage;
};

/// 解析命令行参数
CliArgs parseArgs(int argc, char* argv[]);

/// 命令处理结果
struct CommandResult {
    int exitCode = 0;   ///< 0=成功, 1=警告, 2=错误
    std::string output; ///< 输出内容
};

/// 处理 CLI 命令
CommandResult processCommand(const CliArgs& args);

/// 处理 install 命令
CommandResult processInstall(const CliArgs& args);

/// 处理 list 命令
CommandResult processList(const CliArgs& args);

/// 处理 init 命令
CommandResult processInit(const CliArgs& args);

/// 处理 add 命令
CommandResult processAdd(const CliArgs& args);

// ============================================================
// 辅助函数
// ============================================================

/// 打印帮助信息
std::string helpString();

/// 返回版本字符串
std::string versionString();

/// 清单文件解析（JSON → Manifest）
/// 解析失败返回空 Manifest（name 为空），errorMessage 输出错误信息
Manifest parseManifest(const std::string& json, std::string* errorMessage = nullptr);

/// 清单文件序列化（Manifest → JSON）
std::string manifestToJson(const Manifest& manifest);

/// 解析包描述符
/// "name" -> {name: "name", version: ""}
/// "name@1.0.0" -> {name: "name", version: "1.0.0"}
PackageInfo parsePackageSpec(const std::string& spec);

/// 判断包是否已安装（检查 packagesDir/<name>/pkg.json 是否存在）
bool isPackageInstalled(const std::string& packagesDir, const std::string& pkgName);

/// 获取包的安装路径
std::string getPackageInstallPath(const std::string& packagesDir, const std::string& pkgName);

} // namespace minilang_pkg
