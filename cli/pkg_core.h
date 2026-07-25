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

#include <optional>
#include <string>
#include <unordered_set>
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

// ============================================================
// P2-11 版本约束（SemVer 简化版）
// ------------------------------------------------------------
// 支持的约束格式：
//   "1.0.0"     精确匹配
//   "^1.0.0"    兼容版本（>=1.0.0 <2.0.0）
//   "~1.0.0"    近似版本（>=1.0.0 <1.1.0）
//   ">=1.0.0"   最低版本
//   ">1.0.0"    高于指定版本
//   "<=2.0.0"   最高版本
//   "<2.0.0"    低于指定版本
//   "*"         任意版本
// ============================================================

/// SemVer 版本号（major.minor.patch）
struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;

    /// 从字符串解析（如 "1.2.3" → {1, 2, 3}）。解析失败返回 {0,0,0}。
    static Version parse(const std::string& str);

    /// 转回字符串
    std::string toString() const;

    bool operator==(const Version& o) const { return major == o.major && minor == o.minor && patch == o.patch; }
    bool operator!=(const Version& o) const { return !(*this == o); }
    bool operator<(const Version& o) const {
        return major != o.major ? major < o.major : (minor != o.minor ? minor < o.minor : patch < o.patch);
    }
    bool operator<=(const Version& o) const { return *this < o || *this == o; }
    bool operator>(const Version& o) const { return !(*this <= o); }
    bool operator>=(const Version& o) const { return !(*this < o); }
};

/// 版本约束类型
enum class ConstraintOp {
    Exact,     // "1.0.0" — 精确匹配
    Caret,     // "^1.0.0" — 兼容版本（>=1.0.0 <2.0.0）
    Tilde,     // "~1.0.0" — 近似版本（>=1.0.0 <1.1.0）
    GreaterEq, // ">=1.0.0"
    Greater,   // ">1.0.0"
    LessEq,    // "<=2.0.0"
    Less,      // "<2.0.0"
    Any,       // "*"
};

/// 版本约束
struct VersionConstraint {
    ConstraintOp op = ConstraintOp::Any;
    Version version; ///< 对 Any 无意义

    /// 从字符串解析约束（如 "^1.0.0" → {Caret, {1,0,0}}）
    static VersionConstraint parse(const std::string& str);

    /// 检查版本是否满足约束
    bool matches(const Version& ver) const;

    /// 转回字符串（用于显示）
    std::string toString() const;
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
    bool versionConflict = false;  ///< P2-11: 版本冲突（已安装不同版本）
    std::string conflictDetail;    ///< P2-11: 冲突详情
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

    /// P2-11: 安装清单中所有依赖（含传递依赖）
    /// 递归安装每个依赖包自身的 minilang.pkg 中声明的依赖。
    /// 使用 visited 集合检测循环依赖，避免无限递归。
    /// 返回所有安装结果（含传递依赖的安装结果）。
    std::vector<InstallResult> installAllWithTransitive();

    /// P2-11: 检查已安装包的版本是否满足约束
    /// 返回 true 如果版本满足约束或包未安装
    bool checkVersionConstraint(const std::string& pkgName, const VersionConstraint& constraint) const;

    /// P2-11: 获取已安装包的版本（从 pkg.json 读取）
    /// 返回空 optional 表示包未安装或无版本信息
    std::optional<Version> getInstalledVersion(const std::string& pkgName) const;

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

    /// P2-11: 在 registry 中查找满足约束的最新版本包
    /// 遍历 registry/<pkgName>/ 下的版本子目录，选择满足约束的最高版本
    std::string findInRegistryWithConstraint(const std::string& pkgName, const VersionConstraint& constraint) const;

    /// P2-11: 递归安装传递依赖
    /// pkgPath: 已安装包的目录路径，读取其中的 minilang.pkg 获取依赖
    /// visited: 已处理的包名集合（循环依赖检测）
    /// results: 累积的安装结果
    void installTransitiveDeps(const std::string& pkgPath, std::unordered_set<std::string>& visited,
                               std::vector<InstallResult>& results);

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
    bool noTransitive = false;     ///< --no-transitive（跳过传递依赖）
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
