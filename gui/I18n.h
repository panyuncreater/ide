/**
 * @file gui/I18n.h
 * @brief 国际化（i18n）辅助层。
 *
 * D2: 提供统一的字符串翻译入口，使 GUI 模块可在不继承 QObject 的情况下
 * 使用 Qt Linguist 翻译管线。当 MINILANG_ENABLE_I18N 未启用时，所有
 * mlTr 调用编译期退化为 QString::fromUtf8，保持向后兼容与零运行时开销。
 *
 * R117: 新增运行时语言切换 API（switchMiniLangLanguage / currentLocale /
 * availableLocales），支持菜单栏动态切换 UI 语言；新增 QSettings 持久化
 * 用户语言偏好（ui/locale 键），覆盖系统 locale 自动检测。
 *
 * 用法：
 *   1. CMake 启用 i18n：cmake -DMINILANG_ENABLE_I18N=ON ...
 *   2. 源码中包裹用户可见字符串：
 *        label->setText(mlTr("打开文件夹"));
 *      或在带 context 的场景：
 *        label->setText(mlTrCtx("CodeEditor", "行号"));
 *      或在 QObject 派生类中直接用 tr("打开文件夹")。
 *   3. lupdate 自动同步 .ts 条目，lrelease 编译为 .qm。
 *   4. main.cpp 在 QCoreApplication::setApplicationName 后调用
 *      loadMiniLangTranslations() 加载 .qm 文件（自动读 QSettings 优先级）。
 *   5. 运行时切换：switchMiniLangLanguage("en_US") 卸载旧 translator 加载新 locale。
 *      切换后需通知所有 widget retranslateUi() 刷新文本（由 Ide 类订阅实现）。
 *
 * 翻译上下文约定：
 *   - "MiniLang"     — 顶层菜单/窗口标题/全局对话框（默认）
 *   - "CodeEditor"   — 代码编辑器相关
 *   - "ReplPanel"    — REPL 面板
 *   - "AstViewer"    — AST 查看器
 *   - "IdeController"— 控制器层错误消息
 *
 * 实现说明：
 *   mlTr/mlTrCtx 是宏，i18n 启用时展开为 QCoreApplication::translate，
 *   使 lupdate 能正确识别并提取字符串（lupdate 不识别自定义函数调用）。
 *   未启用时展开为 QString::fromUtf8，行为与现有代码完全等价。
 *
 * @see app/translations/minilang_zh_CN.ts
 * @see app/translations/minilang_en_US.ts (R117 占位，待 lupdate 同步填充)
 */
#pragma once

#include <QString>
#include <QStringList> // availableMiniLangLocales() 返回 QStringList(=QList<QString>)；
                       // i18n 关闭分支也使用，故需无条件 include（仅 <QString> 时为不完全类型）。

#ifdef MINILANG_ENABLE_I18N
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QTranslator>
#include <QVector>

/// 主翻译上下文（顶层 GUI 字符串）
#define ML_TR_CONTEXT "MiniLang"

/// 翻译宏（默认 "MiniLang" 上下文）
/// 展开为 QCoreApplication::translate，lupdate 自动提取
#define mlTr(string) QCoreApplication::translate(ML_TR_CONTEXT, string)

/// 翻译宏（带显式 context）
/// @param context 翻译上下文（如 "CodeEditor"），用于 lupdate 分组
#define mlTrCtx(context, string) QCoreApplication::translate(context, string)

/// QSettings 中持久化用户语言偏好的键名
/// 值为 locale 字符串（如 "zh_CN" / "en_US"），空值表示使用系统 locale
inline constexpr const char* kLocaleSettingsKey = "ui/locale";

/// 已安装的 translator 全局指针（用于切换语言时卸载）
/// 生命周期挂到 QCoreApplication::instance()，进程退出时自动释放
inline QTranslator* g_minilang_translator = nullptr;

/// 当前已加载的 locale（空表示加载失败或 i18n 未启用）
inline QString g_minilang_current_locale;

/// 加载翻译文件（main.cpp 在 QCoreApplication 实例化后调用）
/// 查找路径顺序：
///   1. 可执行文件同目录/translations/
///   2. 可执行文件同目录
///   3. 当前工作目录
/// @param locale 目标 locale（如 "zh_CN"），默认读 QSettings → 系统 locale
/// @return 加载成功返回 true；找不到 .qm 或翻译为空时返回 false
inline bool loadMiniLangTranslations(const QString& locale = QString()) {
    // 优先级：参数 > QSettings 持久化偏好 > 系统 locale
    QString loc = locale;
    if (loc.isEmpty()) {
        QSettings s;
        loc = s.value(kLocaleSettingsKey).toString();
    }
    if (loc.isEmpty()) {
        loc = QLocale::system().name();
    }
    QString qmName = QString("minilang_%1.qm").arg(loc);

    QDir appDir(QCoreApplication::applicationDirPath());
    QStringList searchPaths = {
        appDir.absoluteFilePath("translations/" + qmName),
        appDir.absoluteFilePath(qmName),
        QDir::current().absoluteFilePath(qmName),
    };

    for (const QString& path : searchPaths) {
        QFileInfo fi(path);
        if (fi.exists() && fi.isFile()) {
            auto* translator = new QTranslator(QCoreApplication::instance());
            if (translator->load(path)) {
                // 卸载旧的 translator（若存在）
                if (g_minilang_translator) {
                    QCoreApplication::removeTranslator(g_minilang_translator);
                    delete g_minilang_translator;
                }
                QCoreApplication::installTranslator(translator);
                g_minilang_translator = translator;
                g_minilang_current_locale = loc;
                return true;
            }
            delete translator;
        }
    }
    // 加载失败时仍记录 locale（用于 availableLocales 报告与持久化）
    // 不卸载已有 translator，保留上次成功加载的语言
    g_minilang_current_locale = loc;
    return false;
}

/// 运行时切换 UI 语言（R117）
/// @param locale 目标 locale（如 "en_US"），空字符串表示恢复系统 locale
/// @return 切换成功（找到并加载 .qm）返回 true
/// @note 切换后需调用方通知所有 widget retranslateUi() 刷新文本
inline bool switchMiniLangLanguage(const QString& locale) {
    QSettings s;
    if (locale.isEmpty()) {
        s.remove(kLocaleSettingsKey);
    } else {
        s.setValue(kLocaleSettingsKey, locale);
    }
    return loadMiniLangTranslations(locale);
}

/// 查询当前已加载的 locale（R117）
/// @return locale 字符串（如 "zh_CN"），未加载返回空字符串
inline QString currentMiniLangLocale() {
    return g_minilang_current_locale;
}

/// 扫描 translations/ 目录中可用的 locale 列表（R117）
/// @return locale 字符串列表（如 ["zh_CN", "en_US"]），不含 .qm 扩展名
/// @note 始终包含 "zh_CN"（源语言，即使无 .qm 也回退到源字符串可用）
inline QStringList availableMiniLangLocales() {
    QStringList result;
    QDir appDir(QCoreApplication::applicationDirPath());
    QDir trDir(appDir.absoluteFilePath("translations"));
    QStringList filters;
    filters << "minilang_*.qm";
    QFileInfoList files = trDir.entryInfoList(filters, QDir::Files);
    for (const QFileInfo& fi : files) {
        // minilang_zh_CN.qm → zh_CN
        QString base = fi.completeBaseName(); // minilang_zh_CN
        QString loc = base.mid(QString("minilang_").length());
        if (!loc.isEmpty() && !result.contains(loc)) {
            result.append(loc);
        }
    }
    // 始终包含源语言（zh_CN）作为兜底
    if (!result.contains("zh_CN")) {
        result.prepend("zh_CN");
    }
    return result;
}

#else // MINILANG_ENABLE_I18N 未启用 — 编译期退化为 QString::fromUtf8

#define ML_TR_CONTEXT "MiniLang"

/// 翻译宏（默认上下文，i18n 关闭时退化为 fromUtf8）
#define mlTr(string) QString::fromUtf8(string)

/// 翻译宏（带 context，i18n 关闭时退化为 fromUtf8，context 被忽略）
#define mlTrCtx(context, string) QString::fromUtf8(string)

/// 加载翻译文件（i18n 关闭时为 no-op）
inline bool loadMiniLangTranslations(const QString& /*locale*/ = QString()) {
    return false;
}

/// 切换语言（i18n 关闭时为 no-op）
inline bool switchMiniLangLanguage(const QString& /*locale*/) {
    return false;
}

/// 当前 locale（i18n 关闭时返回空字符串）
inline QString currentMiniLangLocale() {
    return QString();
}

/// 可用 locale 列表（i18n 关闭时仅返回源语言 zh_CN）
inline QStringList availableMiniLangLocales() {
    return QStringList{"zh_CN"};
}

#endif // MINILANG_ENABLE_I18N
