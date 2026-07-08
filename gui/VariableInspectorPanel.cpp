// ============================================================
// VariableInspectorPanel.cpp — 变量检查器面板实现（第三波 P0-2）
// ============================================================

#include "gui/VariableInspectorPanel.h"
#include "gui/GuidedTour.h"
#include "gui/PanelAnimator.h"
#include "gui/MarkdownRenderer.h"
#include "app/IdeController.h"
#include "interpreter/Value.h"
#include "interpreter/NaNBox.h"
#include "interpreter/RefCounted.h"
#include "debug/DebugTypes.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QHeaderView>
#include <sstream>
#include <iomanip>

#include "Label.h"   // QFluentKit（CaptionLabel）

// ============================================================
// VariableInspectorLibrary — 静态教学场景库
// ============================================================

const std::vector<VariableTypeExample>& VariableInspectorLibrary::examples() {
    static const std::vector<VariableTypeExample> kExamples = {
        VariableTypeExample{
            "type-int", "int", "🔢 int 类型",
            "var x = 42;", "42",
            "0x7ff800000000002a", "—",
            "💡 标量内联：int48 直接存入 NaN-box 的低 48 位，无需堆分配。范围 |v| < 2^47。"
        },
        VariableTypeExample{
            "type-int-boundary", "int", "🔢 int 边界值",
            "var big = 70368744177663;", "70368744177663",
            "0x7ff8ffffffffffff", "—",
            "⚠️ int48 最大值 2^46-1 = 70368744177663（约 7×10^13）。超出此范围会触发装箱为 BoxedIntData*。"
        },
        VariableTypeExample{
            "type-float", "float", "🔢 float 类型",
            "var pi = 3.14;", "3.14",
            "0x40091EB851EB851F", "—",
            "💡 标量内联：IEEE 754 double 直接存入 NaN-box 的 64 位。注意 tag bits 与 NaN 模式不冲突。"
        },
        VariableTypeExample{
            "type-bool", "bool", "🔢 bool 类型",
            "var ok = true;", "true",
            "0x7ff9000000000001", "—",
            "💡 标量内联：bool 编码为 int48（0/1），tag bits 与 int 不同（INT_TAG_BASE=0x7FF8 vs BOOL_TAG_BASE=0x7FF9）。"
        },
        VariableTypeExample{
            "type-null", "null", "🔢 null 类型",
            "var n = null;", "null",
            "0x7ffa000000000000", "—",
            "💡 标量内联：唯一编码 NULL_BITS=0x7FFA<<48，payload 全 0。"
        },
        VariableTypeExample{
            "type-string", "string", "📝 string 类型",
            "var s = \"hello\";", "hello",
            "（堆指针，tag bits=0x7FFB）",
            "StringData* (RefCounted) { refCount: 1; bytes: 'hello'; length: 5; }",
            "📦 堆分配：Value 存 StringData* 指针，对象继承 RefCounted 维护引用计数。COW：写时检查 refCount==1，否则深拷贝。"
        },
        VariableTypeExample{
            "type-array", "array", "📊 array 类型",
            "var arr = [1, 2, 3];", "[1, 2, 3]",
            "（堆指针，tag bits=0x7FFB）",
            "ArrayData* (RefCounted) { refCount: 1; elements: Value[3]; }",
            "📦 堆分配：COW 容器。`var b = a` 共享所有权 refCount=2，`b.push(4)` 触发 detach 深拷贝。"
        },
        VariableTypeExample{
            "type-dict", "dict", "📊 dict 类型",
            "var d = {\"x\": 1, \"y\": 2};", "{x: 1, y: 2}",
            "（堆指针，tag bits=0x7FFB）",
            "DictData* (RefCounted) { refCount: 1; entries: HashMap<String,Value>; }",
            "📦 堆分配：基于哈希表的 COW 容器。键需为 string 类型。"
        },
        VariableTypeExample{
            "type-instance", "instance", "📦 instance 类型",
            "var p = Point.new(3, 4);", "<instance of Point>",
            "（堆指针，tag bits=0x7FFB）",
            "InstanceData* (RefCounted) { refCount: 1; className: 'Point'; fields: {x:3, y:4}; }",
            "📦 堆分配：实例字段表通过 ClassInfo::flattenedFieldOrder 描述。方法查找经 methodCache_ 加速。"
        },
        VariableTypeExample{
            "type-closure", "closure", "🔗 closure 类型",
            "fun inc(x) { return x+1; }\nvar f = inc;", "<closure>",
            "（堆指针，tag bits=0x7FFB）",
            "ClosureData* (RefCounted) { refCount: 1; params: ['x']; env: Environment*; body: FunDecl*; }",
            "📦 堆分配：闭包捕获外层 Environment（弱引用链 parent）。env 链打破循环依赖。"
        },
    };
    return kExamples;
}

// ============================================================
// 辅助：从 Value 反推 NaN-boxing 位
// ============================================================

namespace {

std::string valueToBitsHex(const Value& v) {
    std::ostringstream os;
    os << "0x" << std::hex << std::setfill('0') << std::setw(16);
    switch (v.getType()) {
        case ValueType::VAL_INT:    os << NaNBox::fromInt(v.intVal()).rawBits(); break;
        case ValueType::VAL_FLOAT:  os << NaNBox::fromFloat(v.floatVal()).rawBits(); break;
        case ValueType::VAL_BOOL:   os << NaNBox::fromBool(v.boolVal()).rawBits(); break;
        case ValueType::VAL_NULL:  os << NaNBox::null().rawBits(); break;
        default:
            os << "7ffb????????????";
            break;
    }
    return os.str();
}

std::string bitsToBinary(uint64_t bits) {
    std::string s(64, '0');
    for (int i = 0; i < 64; ++i) {
        if (bits & (1ULL << (63 - i))) s[i] = '1';
    }
    return s;
}

}  // namespace

// ============================================================
// VariableInspectorPanel 实现
// ============================================================

VariableInspectorPanel::VariableInspectorPanel(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);
    outer->setSpacing(4);

    auto* pageBar = new QHBoxLayout;
    pageLiveBtn_    = new QPushButton(tr("实时变量树"));
    pageLibraryBtn_ = new QPushButton(tr("类型教学库"));
    pageLiveBtn_->setCheckable(true);
    pageLibraryBtn_->setCheckable(true);
    pageLiveBtn_->setChecked(true);
    pageBar->addWidget(pageLiveBtn_);
    pageBar->addWidget(pageLibraryBtn_);
    pageBar->addStretch();
    outer->addLayout(pageBar);

    stack_ = new QStackedWidget;
    auto* livePage    = new QWidget;
    auto* libraryPage = new QWidget;
    buildLivePage(livePage);
    buildLibraryPage(libraryPage);
    stack_->addWidget(livePage);
    stack_->addWidget(libraryPage);
    outer->addWidget(stack_, 1);

    connect(pageLiveBtn_,    &QPushButton::clicked, [this]() { stack_->setCurrentIndex(0); pageLibraryBtn_->setChecked(false); PanelAnimator::slideInWidget(stack_->currentWidget()); });
    connect(pageLibraryBtn_, &QPushButton::clicked, [this]() {
        stack_->setCurrentIndex(1);
        pageLiveBtn_->setChecked(false);
        // 切到类型教学库时确保有选中项 —— 首次进入若 exampleList_ 无选中，
        // 显式调用 showExample(0) 让详情区立即有内容（而非空白等用户点击）。
        if (exampleList_->count() > 0 && exampleList_->currentRow() < 0) {
            exampleList_->setCurrentRow(0);
        }
        if (currentExampleIdx_ < 0 && exampleList_->count() > 0) {
            showExample(0);
        }
        PanelAnimator::slideInWidget(stack_->currentWidget());
    });

    autoTimer_ = new QTimer(this);
    // OPT-1: 500ms→2000ms 安全网，状态变更由 vmStateChanged 监听器即时触发。
    autoTimer_->setInterval(2000);
    connect(autoTimer_, &QTimer::timeout, this, &VariableInspectorPanel::onRefresh);

    populateExamples();
}

void VariableInspectorPanel::setController(IdeController* controller) {
    if (controller_ == controller) return;
    controller_ = controller;
    if (controller_) {
        controller_->addVmStateChangedListener([this] { onVmStateChanged(); });
    }
}

void VariableInspectorPanel::buildLivePage(QWidget* host) {
    auto* v = new QVBoxLayout(host);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);

    auto* bar = new QHBoxLayout;
    liveStatusLabel_ = new CaptionLabel(tr("状态：未初始化"));
    refreshBtn_       = new QPushButton(tr("刷新"));
    autoRefreshCheck_ = new QCheckBox(tr("自动刷新 (2s)"));
    bar->addWidget(liveStatusLabel_);
    bar->addStretch();
    bar->addWidget(autoRefreshCheck_);
    bar->addWidget(refreshBtn_);
    v->addLayout(bar);

    auto* splitter = new QSplitter(Qt::Vertical);
    varTree_   = new QTreeWidget;
    varTree_->setHeaderLabels({tr("变量"), tr("类型"), tr("值")});
    varTree_->header()->setStretchLastSection(false);
    varTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    varTree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    varTree_->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    splitter->addWidget(varTree_);

    varDetail_ = new QTextBrowser;
    varDetail_->setOpenExternalLinks(false);
    splitter->addWidget(varDetail_);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    v->addWidget(splitter, 1);

    connect(refreshBtn_, &QPushButton::clicked, this, &VariableInspectorPanel::onRefresh);
    connect(autoRefreshCheck_, &QCheckBox::toggled, this, &VariableInspectorPanel::onAutoRefreshToggled);
    connect(varTree_, &QTreeWidget::currentItemChanged, [this](QTreeWidgetItem*, QTreeWidgetItem*) { onVariableSelected(); });
}

void VariableInspectorPanel::buildLibraryPage(QWidget* host) {
    auto* v = new QVBoxLayout(host);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);

    auto* splitter = new QSplitter(Qt::Horizontal);
    exampleList_ = new QListWidget;
    exampleDetail_ = new QTextBrowser;
    exampleDetail_->setOpenExternalLinks(false);
    splitter->addWidget(exampleList_);
    splitter->addWidget(exampleDetail_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);
    v->addWidget(splitter, 1);

    auto* btnBar = new QHBoxLayout;
    loadCodeBtn_ = new QPushButton(tr("加载样例代码到主编辑器"));
    btnBar->addStretch();
    btnBar->addWidget(loadCodeBtn_);
    v->addLayout(btnBar);

    connect(exampleList_, &QListWidget::currentRowChanged, this, &VariableInspectorPanel::onExampleSelected);
    connect(loadCodeBtn_, &QPushButton::clicked, this, &VariableInspectorPanel::onLoadExampleCode);
}

void VariableInspectorPanel::onRefresh() {
    refreshLive();
}

void VariableInspectorPanel::onAutoRefreshToggled(bool checked) {
    if (checked) autoTimer_->start();
    else         autoTimer_->stop();
}

void VariableInspectorPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (autoRefreshCheck_ && autoRefreshCheck_->isChecked() &&
        autoTimer_ && !autoTimer_->isActive()) {
        refreshLive();
        autoTimer_->start();
    }
}

void VariableInspectorPanel::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    if (autoTimer_ && autoTimer_->isActive()) {
        autoTimer_->stop();
    }
}

void VariableInspectorPanel::refreshLive() {
    varTree_->clear();
    if (!controller_) {
        liveStatusLabel_->setText(tr("状态：未绑定 controller"));
        return;
    }

    // 收集变量，按来源分组
    std::vector<VariableSnapshot> locals;
    std::unordered_map<std::string, Value> globals;
    QString modeLabel;

    if (controller_->isVmInitialized()) {
        modeLabel = controller_->getUseRegisterVM() ? tr("RegisterVM") : tr("StackVM");
        globals   = controller_->getVmGlobals();
    } else if (controller_->isRunning() && controller_->isDebugRun()) {
        modeLabel = tr("Interpreter (debug)");
        locals    = controller_->getDebugVariableSnapshot();
    } else {
        liveStatusLabel_->setText(tr("状态：未运行（启动调试或 VM 单步以查看变量）"));
        varDetail_->clear();
        return;
    }

    int total = static_cast<int>(locals.size() + globals.size());
    liveStatusLabel_->setText(tr("状态：%1 | 变量数：%2").arg(modeLabel).arg(total));

    // 全局变量组
    auto* globalGroup = new QTreeWidgetItem(varTree_);
    globalGroup->setText(0, tr("全局变量 (%1)").arg(globals.size()));
    QFont globalFont = globalGroup->font(0);
    globalFont.setBold(true);
    globalGroup->setFont(0, globalFont);
    for (const auto& [name, val] : globals) {
        auto* item = new QTreeWidgetItem(globalGroup);
        item->setText(0, QString::fromUtf8(name.c_str()));
        item->setText(1, QString::fromUtf8(val.typeName().c_str()));
        item->setText(2, QString::fromUtf8(val.toString().c_str()));
        item->setData(0, Qt::UserRole, QString::fromUtf8(name.c_str()));
    }
    globalGroup->setExpanded(true);

    // 局部变量组（按 scope 二次分组）
    std::map<std::string, std::vector<const VariableSnapshot*>> byScope;
    for (const auto& s : locals) byScope[s.scope].push_back(&s);

    for (const auto& [scope, vec] : byScope) {
        auto* group = new QTreeWidgetItem(varTree_);
        QString label = QString::fromUtf8(scope.c_str());
        if (label.isEmpty()) label = tr("本地作用域");
        group->setText(0, tr("%1 (%2)").arg(label).arg(vec.size()));
        QFont f = group->font(0); f.setBold(true); group->setFont(0, f);
        for (const auto* s : vec) {
            auto* item = new QTreeWidgetItem(group);
            item->setText(0, QString::fromUtf8(s->name.c_str()));
            item->setText(1, QString::fromUtf8(s->value.typeName().c_str()));
            item->setText(2, QString::fromUtf8(s->value.toString().c_str()));
            item->setData(0, Qt::UserRole, QString::fromUtf8(s->name.c_str()));
        }
        group->setExpanded(true);
    }
}

void VariableInspectorPanel::onVariableSelected() {
    auto* cur = varTree_->currentItem();
    if (!cur || !cur->parent()) {
        varDetail_->clear();
        return;
    }
    // 仅变量节点（非 scope group）显示详情
    QString name = cur->text(0);
    QString type = cur->text(1);
    QString value = cur->text(2);

    // 从 controller 取实际 Value（保证位字段精确）
    QString bitsHex = tr("（未知）");
    if (controller_) {
        Value v;
        // 优先从全局查
        if (controller_->isVmInitialized()) {
            auto globals = controller_->getVmGlobals();
            auto it = globals.find(name.toStdString());
            if (it != globals.end()) v = it->second;
        } else if (controller_->isRunning() && controller_->isDebugRun()) {
            auto locals = controller_->getDebugVariableSnapshot();
            for (const auto& s : locals) {
                if (s.name == name.toStdString()) { v = s.value; break; }
            }
        }
        if (!v.isNull() || v.getType() == ValueType::VAL_NULL) {
            bitsHex = QString::fromUtf8(valueToBitsHex(v).c_str());
        }
    }

    QString html = QString(
        "<h3>%1: %2</h3>"
        "<p><b>类型:</b> %3</p>"
        "<p><b>值 (toString):</b> %4</p>"
        "<p><b>NaN-boxing 位:</b> <code>%5</code></p>"
    ).arg(name).arg(type).arg(type).arg(value.toHtmlEscaped()).arg(bitsHex);
    varDetail_->setHtml(html);
}

void VariableInspectorPanel::populateExamples() {
    exampleList_->clear();
    for (const auto& e : VariableInspectorLibrary::examples()) {
        exampleList_->addItem(QString::fromUtf8(e.displayName.c_str()));
    }
    if (exampleList_->count() > 0) {
        exampleList_->setCurrentRow(0);
    }
}

void VariableInspectorPanel::onExampleSelected(int index) {
    showExample(index);
}

void VariableInspectorPanel::showExample(int index) {
    currentExampleIdx_ = index;
    if (index < 0 || index >= static_cast<int>(VariableInspectorLibrary::examples().size())) {
        exampleDetail_->clear();
        return;
    }
    const auto& e = VariableInspectorLibrary::examples()[index];
    QString html = QString(
        "<h2>%1</h2>"
        "<p><b>ID:</b> <code>%2</code></p>"
        "<p><b>类型名:</b> <code>%3</code></p>"
        "<h3>源码</h3>"
        "<pre>%4</pre>"
        "<h3>值表示</h3>"
        "<p><code>%5</code></p>"
        "<h3>NaN-boxing 位</h3>"
        "<p><code>%6</code></p>"
        "<h3>堆布局</h3>"
        "%7"
        "<h3>教学注解</h3>"
        "%8"
    ).arg(QString::fromUtf8(e.displayName.c_str()))
     .arg(QString::fromUtf8(e.id.c_str()))
     .arg(QString::fromUtf8(e.typeName.c_str()))
     .arg(QString::fromUtf8(e.sourceExpr.c_str()).toHtmlEscaped())
     .arg(QString::fromUtf8(e.valueRepr.c_str()).toHtmlEscaped())
     .arg(QString::fromUtf8(e.nanboxBits.c_str()))
     .arg(MarkdownRenderer::markdownToHtmlFragment(e.heapLayout))
     .arg(MarkdownRenderer::markdownToHtmlFragment(e.teachingNote));
    exampleDetail_->setHtml(html);
}

void VariableInspectorPanel::onLoadExampleCode() {
    if (currentExampleIdx_ < 0 || currentExampleIdx_ >= static_cast<int>(VariableInspectorLibrary::examples().size())) {
        return;
    }
    const auto& e = VariableInspectorLibrary::examples()[currentExampleIdx_];
    emit loadSampleRequested(QString::fromUtf8(e.sourceExpr.c_str()));
}

// ============================================================
// createGuidedTour — 新手引导（5 步）
// ============================================================

GuidedTour* VariableInspectorPanel::createGuidedTour(QWidget* host) {
    auto* tour = new GuidedTour(host, host);
    // 注：只高亮「始终可见」的页切换按钮（pageLiveBtn_/pageLibraryBtn_），
    // 不高亮 autoRefreshCheck_/varTree_/loadCodeBtn_ 等位于 QStackedWidget
    // 某一页的控件——当目标页未显示时 mapTo 返回错误坐标导致气泡定位混乱。
    // 概念性步骤用 nullptr（居中气泡）+ 内嵌完整示例代码。
    tour->addStep(pageLiveBtn_,
                  QString::fromUtf8("实时变量树"),
                  QString::fromUtf8("「实时变量」页在调试 / VM 运行时按作用域分组显示变量（global / local / upvalue）。"
                                    "勾选「自动刷新」每 2 秒刷新快照，点击变量可在右侧查看 NaN-boxing 位布局。"));
    tour->addStep(nullptr,
                  QString::fromUtf8("示例代码：观察变量类型"),
                  QString::fromUtf8(
                      "<p>将以下代码粘贴到编辑器，按 F5 调试，在变量树中观察各类型：</p>"
                      "<pre style='background:#EEE8D5;padding:8px;border-radius:4px;font-family:Consolas,monospace;'>"
                      "var x = 42;           // int\n"
                      "var pi = 3.14;        // float\n"
                      "var s = \"hello\";      // string\n"
                      "var arr = [1, 2, 3];  // array\n"
                      "fun add(a, b) {\n"
                      "    return a + b;\n"
                      "}\n"
                      "var f = add;          // closure\n"
                      "print x, pi, s, arr, f;\n"
                      "</pre>"
                      "<p>调试时展开变量树节点，可看到 int/float 标量内联、string/array 堆指针的差异。</p>"));
    tour->addStep(pageLibraryBtn_,
                  QString::fromUtf8("类型教学库"),
                  QString::fromUtf8("点击「类型教学库」切换到静态教学页，查看 int / string / array / closure "
                                    "等类型的 NaN-boxing 位布局与堆对象结构详解。"));
    tour->addStep(nullptr,
                  QString::fromUtf8("开始实验"),
                  QString::fromUtf8("切换到类型教学库后，选中任一类型条目，点击「加载样例代码到主编辑器」，"
                                    "再按 F5 运行即可在实时变量树中对照观察。"));
    return tour;
}
