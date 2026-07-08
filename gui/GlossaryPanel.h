#pragma once

// ============================================================
// GlossaryPanel — 术语表面板（教学辅助）
// ------------------------------------------------------------
// 集中展示 MiniLang IDE 涉及的核心术语（NaN-boxing / COW /
// upvalue / SSA / lowering / mark-sweep / RefCounted /
// BytecodeChunk / IRModule / ClosureData 等），按字母排序，
// 支持实时搜索过滤与跨术语跳转。
//
// 解决问题：术语散布在各 ADR 与教学面板，新手需在多个文档间
// 跳转查找。本面板提供单一入口，含中英对照名 + 分类 +
// 一句话定义 + Markdown 详细说明 + 关联术语链接。
//
// 布局：顶部 QLineEdit 搜索框 + 中间 QSplitter(Horizontal)
//   左 QListWidget（术语列表，按字母排序）
//   右 QTextBrowser（选中术语详情，Markdown 渲染）
//
// 依赖：MarkdownRenderer（gui 命名空间）/ TeachingTheme /
// PanelAnimator。不依赖 IdeController / 引擎层，纯静态数据。
// ============================================================

#include <QLineEdit>
#include <QList>
#include <QListWidget>
#include <QStringList>
#include <QTextBrowser>
#include <QWidget>

class GlossaryPanel : public QWidget {
    Q_OBJECT
public:
    explicit GlossaryPanel(QWidget* parent = nullptr);

    /// P0-A fix: 根据 termId 返回关联的教学面板 id（如 "ast" → "ast-toy"）。
    /// 无映射时返回空字符串。供 ide.cpp 接线 termActivated → showTeachingPanel 使用，
    /// 让"点击术语跳转相关面板"真正可达（此前信号定义了却从未 connect）。
    static QString relatedPanelFor(const QString& termId);

signals:
    /// 选中/激活术语条目时发射，可用于跳转到相关面板/ADR。
    /// 由列表选中与详情中 term: 链接点击共同触发。
    void termActivated(const QString& termId);

private slots:
    void onSearchChanged(const QString& text);
    void onTermSelected(int row);

private:
    struct TermEntry {
        QString id;          // 术语 ID（如 "nan-boxing"）
        QString term;        // 术语名（中英对照，如 "NaN-boxing NaN 装箱"）
        QString category;    // 分类（"内存模型"/"编译"/"运行时"/"类型系统"/"测试"等）
        QString shortDef;    // 一句话定义
        QString fullDef;     // 详细解释（Markdown）
        QStringList related; // 关联术语 ID
    };

    QList<TermEntry> entries_;
    QListWidget* listWidget_ = nullptr;
    QTextBrowser* detailView_ = nullptr;
    QLineEdit* searchEdit_ = nullptr;

    void loadEntries();
    void populateList(const QString& filter = QString());
    void showDetail(int index);
};
