/**
 * @file gui/I18n.h
 * @brief 国际化（i18n）辅助层。
 *
 * D2: 提供统一的字符串翻译入口，使 GUI 模块可在不继承 QObject 的情况下
 * 使用 Qt Linguist 翻译管线。当 MINILANG_ENABLE_I18N 未启用时，所有
 * mlTr 调用编译期退化为 QString::fromUtf8，保持向后兼容与零运行时开销。
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
 *      loadMiniLangTranslations() 加载 .qm 文件。
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
 */
#pragma once

#include <QString>

#ifdef MINILANG_ENABLE_I18N
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QTranslator>

/// 主翻译上下文（顶层 GUI 字符串）
#define ML_TR_CONTEXT "MiniLang"

/// 翻译宏（默认 "MiniLang" 上下文）
/// 展开为 QCoreApplication::translate，lupdate 自动提取
#define mlTr(string) QCoreApplication::translate(ML_TR_CONTEXT, string)

/// 翻译宏（带显式 context）
/// @param context 翻译上下文（如 "CodeEditor"），用于 lupdate 分组
#define mlTrCtx(context, string) QCoreApplication::translate(context, string)

/// 加载翻译文件（main.cpp 在 QCoreApplication 实例化后调用）
/// 查找路径顺序：
///   1. 可执行文件同目录/translations/
///   2. 可执行文件同目录
///   3. 当前工作目录
/// @param locale 目标 locale（如 "zh_CN"），默认自动检测 QLocale::system()
/// @return 加载成功返回 true；找不到 .qm 或翻译为空时返回 false
inline bool loadMiniLangTranslations(const QString& locale = QString()) {
    QString loc = locale.isEmpty() ? QLocale::system().name() : locale;
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
                QCoreApplication::installTranslator(translator);
                return true;
            }
            delete translator;
        }
    }
    return false;
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

#endif // MINILANG_ENABLE_I18N
