// ============================================================
// VariableInspectorPanel.cpp — 变量检查器面板实现（第三波 P0-2）
// ============================================================

#include "gui/VariableInspectorPanel.h"
#include "app/IdeController.h"
#include "common/MemoryInspectionAPI.h" // ARCH-10: NaNBox 位模式快照（替代直接依赖 interpreter/NaNBox.h）
#include "debug/DebugTypes.h"
#include "gui/GuidedTour.h"
#include "gui/MarkdownRenderer.h"
#include "gui/PanelAnimator.h"
#include "interpreter/Value.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog> // 拓展二期：双击改值输入对话框
#include <QSplitter>
#include <QVBoxLayout>
#include <algorithm> // AUDIT-P2 fix: std::sort 全局变量按名排序
#include <sstream>
#include <vector>

#include "Label.h" // QFluentKit（CaptionLabel）

// ============================================================
// VariableInspectorLibrary — 静态教学场景库
// ============================================================

/// 返回变量类型示例库数据（静态数据）。
const std::vector<VariableTypeExample>& VariableInspectorLibrary::examples() {
    static const std::vector<VariableTypeExample> kExamples = {
        VariableTypeExample{"type-int", "int", "🔢 int 类型", "var x = 42;", "42", "0x7ff800000000002a", "—",
                            "💡 标量内联：int48 直接存入 NaN-box 的低 48 位，无需堆分配。范围 |v| < 2^47。"},
        VariableTypeExample{
            "type-int-boundary", "int", "🔢 int 边界值", "var big = 70368744177663;", "70368744177663",
            "0x7ff8ffffffffffff", "—",
            "⚠️ int48 最大值 2^46-1 = 70368744177663（约 7×10^13）。超出此范围会触发装箱为 BoxedIntData*。"},
        VariableTypeExample{
            "type-float", "float", "🔢 float 类型", "var pi = 3.14;", "3.14", "0x40091EB851EB851F", "—",
            "💡 标量内联：IEEE 754 double 直接存入 NaN-box 的 64 位。注意 tag bits 与 NaN 模式不冲突。"},
        VariableTypeExample{"type-bool", "bool", "🔢 bool 类型", "var ok = true;", "true", "0x7ff9000000000001", "—",
                            "💡 标量内联：bool 编码为 int48（0/1），tag bits 与 int 不同（INT_TAG_BASE=0x7FF8 vs "
                            "BOOL_TAG_BASE=0x7FF9）。"},
        VariableTypeExample{"type-null", "null", "🔢 null 类型", "var n = null;", "null", "0x7ffa000000000000", "—",
                            "💡 标量内联：唯一编码 NULL_BITS=0x7FFA<<48，payload 全 0。"},
        VariableTypeExample{"type-string", "string", "📝 string 类型", "var s = \"hello\";", "hello",
                            "（堆指针，tag bits=0x7FFB）",
                            "StringData* (RefCounted) { refCount: 1; bytes: 'hello'; length: 5; }",
                            "📦 堆分配：Value 存 StringData* 指针，对象继承 RefCounted 维护引用计数。COW：写时检查 "
                            "refCount==1，否则深拷贝。"},
        VariableTypeExample{"type-array", "array", "📊 array 类型", "var arr = [1, 2, 3];", "[1, 2, 3]",
                            "（堆指针，tag bits=0x7FFB）",
                            "ArrayData* (RefCounted) { refCount: 1; elements: Value[3]; }",
                            "📦 堆分配：COW 容器。`var b = a` 共享所有权 refCount=2，`b.push(4)` 触发 detach 深拷贝。"},
        VariableTypeExample{"type-dict", "dict", "📊 dict 类型", "var d = {\"x\": 1, \"y\": 2};", "{x: 1, y: 2}",
                            "（堆指针，tag bits=0x7FFB）",
                            "DictData* (RefCounted) { refCount: 1; entries: HashMap<String,Value>; }",
                            "📦 堆分配：基于哈希表的 COW 容器。键需为 string 类型。"},
        VariableTypeExample{
            "type-instance", "instance", "📦 instance 类型", "var p = Point(3, 4);", "<instance of Point>",
            "（堆指针，tag bits=0x7FFB）",
            "InstanceData* (RefCounted) { refCount: 1; className: 'Point'; fields: {x:3, y:4}; }",
            "📦 堆分配：实例字段表通过 ClassInfo::flattenedFieldOrder 描述。方法查找经 methodCache_ 加速。"},
        VariableTypeExample{
            "type-closure", "closure", "🔗 closure 类型", "fun inc(x) { return x+1; }\nvar f = inc;", "<closure>",
            "（堆指针，tag bits=0x7FFB）",
            "ClosureData* (RefCounted) { refCount: 1; params: ['x']; env: Environment*; body: FunDecl*; }",
            "📦 堆分配：闭包捕获外层 Environment（弱引用链 parent）。env 链打破循环依赖。"},
        VariableTypeExample{"type-float-inf", "float", "⚠️ float 无穷大", "var inf = 1.0 / 0.0;", "inf",
                            "0x7FF0000000000000", "—",
                            "⚠️ IEEE 754 infinity：指数位全 1（0x7FF）、尾数全 0。+inf 与 -inf 仅符号位不同。"
                            "标量内联存入 NaN-box，注意与 NaN 的尾数区别（NaN 尾数非零）。"
                            "三后端除零行为需统一：1.0/0.0 产生 inf 而非抛异常。"},
        VariableTypeExample{"type-float-nan", "float", "⚠️ float NaN", "var nan = 0.0 / 0.0;", "nan",
                            "0x7FF8000000000000", "—",
                            "⚠️ IEEE 754 quiet NaN：指数位全 1（0x7FF）、尾数最高位为 1（0x8000000000000）。"
                            "关键性质：NaN != NaN，因此相等比较需用 isNaN() 而非 == 。"
                            "尾数非零使其与 infinity 区分；NaN-box 的 tag 模式需避开此位模式。"},
        VariableTypeExample{
            "type-empty-string", "string", "📝 空字符串", "var s = \"\";", "", "（堆指针，tag bits=0x7FFB）",
            "StringData* (RefCounted) { refCount: 1; bytes: ''; length: 0; }",
            "📦 堆分配：即使 length==0，StringData 仍分配在堆上（RefCounted 头 + 0 字节 payload）。"
            "Value 存指针而非内联，空字符串与非空字符串走同一类型路径，COW 检查 refCount==1 同样适用。"},
        VariableTypeExample{"type-empty-array", "array", "📊 空数组", "var arr = [];", "[]",
                            "（堆指针，tag bits=0x7FFB）",
                            "ArrayData* (RefCounted) { refCount: 1; elements: Value[0]; capacity: 0; }",
                            "📦 堆分配：空数组仍持有 ArrayData* 指针，elements 容量为 0。"
                            "COW 语义不变：`var b = a` 后两者共享同一空 ArrayData，refCount=2，"
                            "push 时才触发 detach 深拷贝。"},
        VariableTypeExample{"type-empty-dict", "dict", "📊 空字典", "var d = {};", "{}", "（堆指针，tag bits=0x7FFB）",
                            "DictData* (RefCounted) { refCount: 1; entries: HashMap<String,Value> (empty); }",
                            "📦 堆分配：空字典仍分配 DictData*，HashMap 桶数为 0 或初始容量。"
                            "键需为 string，空字典同样支持 COW 共享，put 时检查 refCount 决定是否深拷贝。"},
        VariableTypeExample{"type-nested-array", "array", "📊 嵌套数组", "var matrix = [[1, 2], [3, 4]];",
                            "[[1, 2], [3, 4]]", "（堆指针，tag bits=0x7FFB）",
                            "ArrayData* (RefCounted) { refCount: 1; elements: Value[2] -> ArrayData*; }",
                            "📦 嵌套引用语义：外层数组持有两个内层 ArrayData* 指针。"
                            "COW 仅复制最外层：`var m2 = m` 后 m2[0] 与 m[0] 共享同一内层数组（refCount=2），"
                            "修改 m2[0][0] 会影响 m[0][0]，除非对内层显式深拷贝。"
                            "三后端需统一此浅拷贝语义。"},
        VariableTypeExample{"type-type-annotation", "int", "⚠️ 类型注解错误", "var x: int = \"hi\";", "(类型错误)", "—",
                            "—",
                            "⚠️ 类型注解强制：`var x: int = \"hi\"` 中注解 int 与字面量 string 不匹配。"
                            "编译期生成 OP_TYPE_CHECK 指令，运行时若实际类型与注解不符则抛出 TypeError。"
                            "三后端（Interpreter / StackVM / RegisterVM）需统一此检查行为与错误消息文本。"},
        // ---- 需求 8 扩充：新语言特性类型（元组/枚举/协程）+ 装箱/COW/继承场景 ----
        VariableTypeExample{"type-tuple", "tuple", "📦 tuple 类型（R98）", "var t = (1, \"a\", true);", "(1, a, true)",
                            "（堆指针，tag bits=0x7FFB）",
                            "TupleData* (RefCounted) { refCount: 1; elements: Value[3] (immutable); }",
                            "📦 不可变容器：元素在构造（OP_BUILD_TUPLE）后禁止赋值，t[0] = 9 报错。"
                            "支持索引读 t[0] 与解构 var (a, b, c) = t。不可变性使共享无需 COW 检查——"
                            "多个引用永远看到相同内容。"},
        VariableTypeExample{"type-enum-variant", "enum", "📦 enum variant 类型（R99 ADT）",
                            "enum Color { Red(int) }\nvar c = Color.Red(255);", "Color.Red(255)",
                            "（堆指针，tag bits=0x7FFB）",
                            "EnumVariantData* (RefCounted) { refCount: 1; enumName: 'Color'; variantName: 'Red'; "
                            "fields: [255]; }",
                            "📦 代数数据类型：variant 携带字段（构造时 OP_BUILD_ENUM_VARIANT 校验字段数与类型）。"
                            "配合 match 模式匹配提取字段：match (c) { Color.Red(v) => v }。"
                            "无 default 且未穷举命中时抛异常，三后端统一。"},
        VariableTypeExample{"type-coroutine", "coroutine", "🔄 coroutine 类型（R164）",
                            "fun* gen() { yield 1; yield 2; }\nvar g = gen();", "<coroutine gen>",
                            "（堆指针，tag bits=0x7FFB）",
                            "CoroutineData* (RefCounted) { refCount: 1; targetYieldId: 0; done: false; }",
                            "🔄 生成器协程：调用 fun* 不执行函数体，而是返回协程对象。每次 g.next() 以重放"
                            "模式重新执行函数体到目标 yield 点（OP_YIELD 计数器命中即抛 YieldSignal）。"
                            "await 协程 = 循环 next() 到 done。四后端重放语义一致。"},
        VariableTypeExample{"type-boxed-int", "int", "⚠️ 装箱大整数（BoxedInt）", "var huge = 100000000000000000;",
                            "100000000000000000", "（堆指针，tag bits=0x7FFB）",
                            "BoxedIntData* (RefCounted) { refCount: 1; value: int64 = 100000000000000000; }",
                            "⚠️ 超出 int48 内联范围（|v| ≥ 2^47）的整数自动装箱为 BoxedIntData*：同一个 int "
                            "类型在 NaN-box 里有两种物理表示（内联标量 vs 堆指针）。算术运算对两种表示"
                            "透明，但装箱带来堆分配与引用计数开销——热循环中应避免超大整数。"},
        VariableTypeExample{"type-cow-shared", "array", "📊 COW 共享数组（refCount=2）",
                            "var a = [1, 2, 3];\nvar b = a;", "[1, 2, 3]",
                            "（两个 Value 持同一堆指针，tag bits=0x7FFB）",
                            "ArrayData* (RefCounted) { refCount: 2; elements: Value[3]; }  // a、b 共享",
                            "📊 写时复制实拍：`var b = a` 仅拷贝指针并 refCount++（O(1)）。当 b.push(4) 时"
                            "检查 refCount==2 非独占 → 深拷贝后再写（detach），a 不受影响且 refCount 回落为 1。"
                            "这是「值语义外观 + 引用共享实现」的核心机制，三后端统一。"},
        VariableTypeExample{"type-instance-inherit", "instance", "📦 继承实例（字段扁平化）",
                            "class Shape { var color = \"red\"; }\nclass Circle : Shape { var r = 1; }\n"
                            "var c = Circle();",
                            "<instance of Circle>", "（堆指针，tag bits=0x7FFB）",
                            "InstanceData* (RefCounted) { refCount: 1; className: 'Circle'; fields: {color:'red', "
                            "r:1}; }",
                            "📦 继承字段扁平化：ClassInfo::flattenedFieldOrder 将父类链全部字段按声明顺序展平，"
                            "父类字段默认值经 OP_INIT_FIELD 初始化（历史 Bug 高发点：继承时字段默认值丢失）。"
                            "方法查找沿父类链回退，并由 methodCache_ / 内联缓存加速。"},
    };
    return kExamples;
}

// ============================================================
// 辅助：从 Value 反推 NaN-boxing 位
// ============================================================

namespace {

std::string valueToBitsHex(const Value& v) {
    // ARCH-10: 通过 MemoryInspectionAPI 获取 NaN-box 位模式快照，
    // 消除面板对 interpreter/NaNBox.h 的直接依赖。
    // 内部实现已处理 BoxedIntData（堆指针）降级为 PTR_TAG_BASE 占位符的边界情况。
    return MemoryInspectionAPI::inspectValue(v).bitsHex;
}

// [[maybe_unused]]：位模式展示当前经 bitsHex 路径，二进制转换保留供
// 教学面板扩展使用，避免 GCC -Werror=unused-function（匿名命名空间）。
[[maybe_unused]] std::string bitsToBinary(uint64_t bits) {
    std::string s(64, '0');
    for (int i = 0; i < 64; ++i) {
        if (bits & (1ULL << (63 - i)))
            s[i] = '1';
    }
    return s;
}

} // namespace

// ============================================================
// VariableInspectorPanel 实现
// ============================================================

/// 构造变量检视面板：初始化实时页与示例库页。
VariableInspectorPanel::VariableInspectorPanel(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);
    outer->setSpacing(4);

    auto* pageBar = new QHBoxLayout;
    pageLiveBtn_ = new QPushButton(tr("实时变量树"));
    pageLibraryBtn_ = new QPushButton(tr("类型教学库"));
    pageLiveBtn_->setCheckable(true);
    pageLibraryBtn_->setCheckable(true);
    pageLiveBtn_->setChecked(true);
    pageBar->addWidget(pageLiveBtn_);
    pageBar->addWidget(pageLibraryBtn_);
    pageBar->addStretch();
    outer->addLayout(pageBar);

    stack_ = new QStackedWidget;
    auto* livePage = new QWidget;
    auto* libraryPage = new QWidget;
    buildLivePage(livePage);
    buildLibraryPage(libraryPage);
    stack_->addWidget(livePage);
    stack_->addWidget(libraryPage);
    outer->addWidget(stack_, 1);

    connect(pageLiveBtn_, &QPushButton::clicked, [this]() {
        stack_->setCurrentIndex(0);
        pageLibraryBtn_->setChecked(false);
        PanelAnimator::slideInWidget(stack_->currentWidget());
    });
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

/// 绑定 IDE 控制器以订阅 VM 状态变化。
void VariableInspectorPanel::setController(IdeController* controller) {
    if (controller_ == controller)
        return;
    // AUDIT-P0 fix: 注册前若已有 controller，先反注册旧监听器避免悬垂。
    if (controller_) {
        controller_->removeVmStateChangedListener(this);
    }
    controller_ = controller;
    if (controller_) {
        controller_->addVmStateChangedListener(this, [this] { onVmStateChanged(); });
    }
}

// AUDIT-P0 fix: 析构时反注册监听器，避免 controller_ 持有悬垂 this 回调。
VariableInspectorPanel::~VariableInspectorPanel() {
    if (controller_) {
        controller_->removeVmStateChangedListener(this);
    }
}

/// 构建「实时变量」子页 UI。
void VariableInspectorPanel::buildLivePage(QWidget* host) {
    auto* v = new QVBoxLayout(host);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);

    auto* bar = new QHBoxLayout;
    liveStatusLabel_ = new CaptionLabel(tr("状态：未初始化"));
    refreshBtn_ = new QPushButton(tr("刷新"));
    autoRefreshCheck_ = new QCheckBox(tr("自动刷新 (2s)"));
    bar->addWidget(liveStatusLabel_);
    bar->addStretch();
    bar->addWidget(autoRefreshCheck_);
    bar->addWidget(refreshBtn_);
    v->addLayout(bar);

    auto* splitter = new QSplitter(Qt::Vertical);
    varTree_ = new QTreeWidget;
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
    connect(varTree_, &QTreeWidget::currentItemChanged,
            [this](QTreeWidgetItem*, QTreeWidgetItem*) { onVariableSelected(); });
    // 拓展二期：双击变量项修改值（setVariable）
    connect(varTree_, &QTreeWidget::itemDoubleClicked, this, &VariableInspectorPanel::onVariableDoubleClicked);
}

// ============================================================
// 拓展二期：双击变量项修改值（仅调试暂停/VM 暂停时生效）
// ------------------------------------------------------------
// 交互：双击变量行 → QInputDialog 输入新值（int/float/bool/null/"字符串"）
// → parseDebugValueText 解析 → IdeController::setDebugVariableValue 写回
// （Interpreter 路径经 DebugController 写回调；VM 路径经 VmStepper 双后端）。
// 仅接受第一层变量项（分组的直接子项）；容器子元素编辑暂不支持。
// ============================================================
void VariableInspectorPanel::onVariableDoubleClicked(QTreeWidgetItem* item, int column) {
    Q_UNUSED(column);
    if (!controller_ || !item)
        return;
    // 仅第一层变量项：有父（分组）且父无父（分组是顶层）
    if (!item->parent() || item->parent()->parent())
        return;
    // 仅调试暂停或 VM 暂停（单步间隙）时可写
    bool interpPaused = controller_->isRunning() && controller_->isDebugRun() && controller_->isDebugPaused();
    bool vmPaused = !controller_->isRunning() && controller_->isVmInitialized() && !controller_->isVmRunning();
    if (!interpPaused && !vmPaused) {
        liveStatusLabel_->setText(tr("状态：仅调试暂停/VM 单步暂停时可修改变量"));
        return;
    }
    const QString name = item->text(0);
    bool ok = false;
    const QString text = QInputDialog::getText(
        this, tr("修改变量"), tr("变量 %1 的新值（int / float / true / false / null / \"字符串\"）：").arg(name),
        QLineEdit::Normal, item->text(2), &ok);
    if (!ok || text.isEmpty())
        return;
    Value newVal;
    if (!parseDebugValueText(text.toStdString(), newVal)) {
        liveStatusLabel_->setText(tr("状态：新值解析失败（支持 int/float/bool/null/\"字符串\"）"));
        return;
    }
    if (controller_->setDebugVariableValue(name.toStdString(), newVal)) {
        refreshLive(); // 写入成功：刷新树展示新值
    } else {
        liveStatusLabel_->setText(tr("状态：变量 %1 写入失败（不存在或当前后端不支持）").arg(name));
    }
}

/// 构建「类型示例库」子页 UI。
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

/// 手动刷新实时变量视图。
void VariableInspectorPanel::onRefresh() {
    refreshLive();
}

/// 自动刷新开关：开启后随 VM 状态变化自动刷新。
void VariableInspectorPanel::onAutoRefreshToggled(bool checked) {
    if (checked)
        autoTimer_->start();
    else
        autoTimer_->stop();
}

/// 面板显示时触发一次刷新（按需加载数据）。
void VariableInspectorPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (autoRefreshCheck_ && autoRefreshCheck_->isChecked() && autoTimer_ && !autoTimer_->isActive()) {
        refreshLive();
        autoTimer_->start();
    }
}

/// 面板隐藏时停止自动刷新以节省资源。
void VariableInspectorPanel::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    if (autoTimer_ && autoTimer_->isActive()) {
        autoTimer_->stop();
    }
}

/// 读取 VM 当前变量状态并刷新实时展示（含异常保护）。
/// R121: 当 controller_->getSelectedFrame() >= 0 时，locals 改为消费 getSelectedFrameLocals()
///       显示所选帧的局部变量；globals 保持显示当前后端的全局变量（VM 模式）。
///       selectedFrame_ == -1 时保持原行为（栈顶 locals + globals）。
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

    // AUDIT-P1 fix: 优先判断当前活跃引擎（Interpreter 调试 > VM 单步），
    // 避免 isVmInitialized 一次性永久 true 后 Interpreter 调试仍走 VM 分支显示陈旧数据。
    // 同时增加 isDebugPaused() 检查，避免调试 resume 期间 worker 活跃时并发访问解释器
    // 内部 unordered_map/vector 导致 UB（数据竞争）。
    if (controller_->isRunning() && controller_->isDebugRun()) {
        if (!controller_->isDebugPaused()) {
            liveStatusLabel_->setText(tr("状态：调试运行中（暂停后刷新）"));
            varDetail_->clear();
            return;
        }
        modeLabel = tr("Interpreter (debug)");
        locals = controller_->getDebugVariableSnapshot();
    } else if (controller_->isVmInitialized()) {
        // AUDIT-P1-CORRECT fix: VM RUN 模式（异步 QTimer 批量执行）期间 worker 可能
        // 修改 globalSlots_/globalNameToSlot_，并发读取触发 UB。添加 isVmRunning() 守卫，
        // 对齐 Interpreter 调试路径的 isDebugPaused() 守卫。
        if (controller_->isVmRunning()) {
            liveStatusLabel_->setText(tr("状态：VM 运行中（暂停后刷新）"));
            varDetail_->clear();
            return;
        }
        modeLabel = controller_->getUseRegisterVM() ? tr("RegisterVM") : tr("StackVM");
        globals = controller_->getVmGlobals();
    } else {
        liveStatusLabel_->setText(tr("状态：未运行（启动调试或 VM 单步以查看变量）"));
        varDetail_->clear();
        return;
    }

    // R121: 当用户选中调用栈某帧时（selectedFrame >= 0），用 getSelectedFrameLocals()
    // 覆盖 locals，显示该帧的局部变量。selectedFrame_ == -1 时保持原行为。
    int selFrame = controller_->getSelectedFrame();
    QString frameHint;
    if (selFrame >= 0) {
        auto selLocals = controller_->getSelectedFrameLocals();
        if (!selLocals.empty()) {
            // 用所选帧 locals 替换原 locals（原 locals 是栈顶帧 + 全局混合，已通过 globals 分支保留全局）
            // 对于 Interpreter 模式，原 locals 已含全局 + upvalue，需保留全局部分
            // 简化策略：直接清空 locals，用所选帧 locals 填充；globals 独立显示
            locals.clear();
            for (const auto& kv : selLocals) {
                locals.push_back(VariableSnapshot{kv.first, kv.second, "selected-frame"});
            }
            frameHint = tr(" | 已选帧: %1").arg(selFrame);
        }
    }

    int total = static_cast<int>(locals.size() + globals.size());
    liveStatusLabel_->setText(tr("状态：%1 | 变量数：%2%3").arg(modeLabel).arg(total).arg(frameHint));

    // 全局变量组
    auto* globalGroup = new QTreeWidgetItem(varTree_);
    globalGroup->setText(0, tr("全局变量 (%1)").arg(globals.size()));
    QFont globalFont = globalGroup->font(0);
    globalFont.setBold(true);
    globalGroup->setFont(0, globalFont);
    // AUDIT-P2 fix: 全局变量按名排序，与局部变量（std::map）排序一致，
    // 避免 unordered_map hash 桶序导致的非确定展示顺序（对齐 VmStackPanel 行 171-172）
    std::vector<std::pair<std::string, Value>> globalEntries(globals.begin(), globals.end());
    std::sort(globalEntries.begin(), globalEntries.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [name, val] : globalEntries) {
        auto* item = new QTreeWidgetItem(globalGroup);
        item->setText(0, QString::fromUtf8(name.c_str()));
        // AUDIT-P1 fix: toString()/typeName() 异常防护，与 VmStackPanel 一致。
        // P2-UX fix: 捕获异常原因并设为 tooltip，通知用户值可能已损坏。
        std::string valStr, typeStr;
        QString errorTooltip;
        try {
            typeStr = val.typeName();
        } catch (const std::exception& e) {
            typeStr = "<error>";
            errorTooltip = tr("typeName() 异常：%1（值可能已损坏）").arg(QString::fromUtf8(e.what()));
        } catch (...) {
            typeStr = "<error>";
            errorTooltip = tr("typeName() 抛出未知异常（值可能已损坏）");
        }
        try {
            valStr = val.toString();
        } catch (const std::exception& e) {
            valStr = "<error>";
            errorTooltip = errorTooltip.isEmpty()
                               ? tr("toString() 异常：%1（值可能已损坏）").arg(QString::fromUtf8(e.what()))
                               : errorTooltip + "\n" + tr("toString() 异常：%1").arg(QString::fromUtf8(e.what()));
        } catch (...) {
            valStr = "<error>";
            errorTooltip = errorTooltip.isEmpty() ? tr("toString() 抛出未知异常（值可能已损坏）")
                                                  : errorTooltip + "\n" + tr("toString() 抛出未知异常");
        }
        item->setText(1, QString::fromUtf8(typeStr.c_str()));
        item->setText(2, QString::fromUtf8(valStr.c_str()));
        if (!errorTooltip.isEmpty()) {
            item->setToolTip(0, errorTooltip);
            item->setToolTip(1, errorTooltip);
            item->setToolTip(2, errorTooltip);
        }
        item->setData(0, Qt::UserRole, QString::fromUtf8(name.c_str()));
    }
    globalGroup->setExpanded(true);

    // 局部变量组（按 scope 二次分组）
    std::map<std::string, std::vector<const VariableSnapshot*>> byScope;
    for (const auto& s : locals)
        byScope[s.scope].push_back(&s);

    for (const auto& [scope, vec] : byScope) {
        auto* group = new QTreeWidgetItem(varTree_);
        QString label = QString::fromUtf8(scope.c_str());
        if (label.isEmpty())
            label = tr("本地作用域");
        group->setText(0, tr("%1 (%2)").arg(label).arg(vec.size()));
        QFont f = group->font(0);
        f.setBold(true);
        group->setFont(0, f);
        for (const auto* s : vec) {
            auto* item = new QTreeWidgetItem(group);
            item->setText(0, QString::fromUtf8(s->name.c_str()));
            // AUDIT-P1 fix: toString()/typeName() 异常防护。
            // P2-UX fix: 捕获异常原因并设为 tooltip，通知用户值可能已损坏。
            std::string valStr, typeStr;
            QString errorTooltip;
            try {
                typeStr = s->value.typeName();
            } catch (const std::exception& e) {
                typeStr = "<error>";
                errorTooltip = tr("typeName() 异常：%1（值可能已损坏）").arg(QString::fromUtf8(e.what()));
            } catch (...) {
                typeStr = "<error>";
                errorTooltip = tr("typeName() 抛出未知异常（值可能已损坏）");
            }
            try {
                valStr = s->value.toString();
            } catch (const std::exception& e) {
                valStr = "<error>";
                errorTooltip = errorTooltip.isEmpty()
                                   ? tr("toString() 异常：%1（值可能已损坏）").arg(QString::fromUtf8(e.what()))
                                   : errorTooltip + "\n" + tr("toString() 异常：%1").arg(QString::fromUtf8(e.what()));
            } catch (...) {
                valStr = "<error>";
                errorTooltip = errorTooltip.isEmpty() ? tr("toString() 抛出未知异常（值可能已损坏）")
                                                      : errorTooltip + "\n" + tr("toString() 抛出未知异常");
            }
            item->setText(1, QString::fromUtf8(typeStr.c_str()));
            item->setText(2, QString::fromUtf8(valStr.c_str()));
            if (!errorTooltip.isEmpty()) {
                item->setToolTip(0, errorTooltip);
                item->setToolTip(1, errorTooltip);
                item->setToolTip(2, errorTooltip);
            }
            item->setData(0, Qt::UserRole, QString::fromUtf8(s->name.c_str()));
        }
        group->setExpanded(true);
    }
}

/// 变量列表选中项变化时的详情刷新。
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
        bool found = false;
        // AUDIT-P1 fix: 优先判断当前活跃引擎（与 refreshLive 一致），
        // 避免 isVmInitialized 一次性 true 后 Interpreter 调试仍走 VM 分支找不到局部变量。
        // 同时增加 isDebugPaused() 检查避免调试 resume 期间数据竞争。
        if (controller_->isRunning() && controller_->isDebugRun()) {
            if (controller_->isDebugPaused()) {
                auto locals = controller_->getDebugVariableSnapshot();
                for (const auto& s : locals) {
                    if (s.name == name.toStdString()) {
                        v = s.value;
                        found = true;
                        break;
                    }
                }
            }
        } else if (controller_->isVmInitialized() && !controller_->isVmRunning()) {
            // AUDIT-P1-CORRECT fix: VM RUN 期间并发读取 globals 触发 UB，添加 isVmRunning() 守卫
            auto globals = controller_->getVmGlobals();
            auto it = globals.find(name.toStdString());
            if (it != globals.end()) {
                v = it->second;
                found = true;
            }
        }
        // AUDIT-P1 fix: 原 `!v.isNull() || v.getType()==VAL_NULL` 是恒真表达式
        // （false||true=true 或 true||...=true），导致未找到变量时也显示 null 位模式。
        // 改为 found 标志判断，仅找到时显示位模式。
        if (found) {
            bitsHex = QString::fromUtf8(valueToBitsHex(v).c_str());
        }
    }

    QString html = QString("<h3>%1: %2</h3>"
                           "<p><b>类型:</b> %3</p>"
                           "<p><b>值 (toString):</b> %4</p>"
                           "<p><b>NaN-boxing 位:</b> <code>%5</code></p>")
                       .arg(name.toHtmlEscaped())
                       .arg(type.toHtmlEscaped())
                       .arg(type.toHtmlEscaped())
                       .arg(value.toHtmlEscaped())
                       .arg(bitsHex.toHtmlEscaped());
    varDetail_->setHtml(html);
}

/// 填充类型示例库列表。
void VariableInspectorPanel::populateExamples() {
    exampleList_->clear();
    for (const auto& e : VariableInspectorLibrary::examples()) {
        exampleList_->addItem(QString::fromUtf8(e.displayName.c_str()));
    }
    if (exampleList_->count() > 0) {
        exampleList_->setCurrentRow(0);
    }
}

/// 示例库选中项变化回调。
void VariableInspectorPanel::onExampleSelected(int index) {
    showExample(index);
}

/// 展示指定索引的类型示例说明与代码。
void VariableInspectorPanel::showExample(int index) {
    currentExampleIdx_ = index;
    if (index < 0 || index >= static_cast<int>(VariableInspectorLibrary::examples().size())) {
        exampleDetail_->clear();
        return;
    }
    const auto& e = VariableInspectorLibrary::examples()[index];
    QString html = QString("<h2>%1</h2>"
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
                           "%8")
                       .arg(QString::fromUtf8(e.displayName.c_str()).toHtmlEscaped())
                       .arg(QString::fromUtf8(e.id.c_str()).toHtmlEscaped())
                       .arg(QString::fromUtf8(e.typeName.c_str()).toHtmlEscaped())
                       .arg(QString::fromUtf8(e.sourceExpr.c_str()).toHtmlEscaped())
                       .arg(QString::fromUtf8(e.valueRepr.c_str()).toHtmlEscaped())
                       .arg(QString::fromUtf8(e.nanboxBits.c_str()).toHtmlEscaped())
                       .arg(MarkdownRenderer::markdownToHtmlFragment(e.heapLayout))
                       .arg(MarkdownRenderer::markdownToHtmlFragment(e.teachingNote));
    exampleDetail_->setHtml(html);
}

/// 将选中示例的代码载入编辑器供运行。
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
    tour->addStep(pageLiveBtn_, QString::fromUtf8("实时变量树"),
                  QString::fromUtf8("「实时变量」页在调试 / VM 运行时按作用域分组显示变量（global / local / upvalue）。"
                                    "勾选「自动刷新」每 2 秒刷新快照，点击变量可在右侧查看 NaN-boxing 位布局。"));
    tour->addStep(
        nullptr, QString::fromUtf8("示例代码：观察变量类型"),
        QString::fromUtf8("<p>将以下代码粘贴到编辑器，按 F5 调试，在变量树中观察各类型：</p>"
                          "<pre style='background:#F5F5F5;padding:8px;border-radius:4px;font-family:\"Cascadia "
                          "Code\",\"Cascadia Mono\",\"Consolas\",\"JetBrains Mono\",\"Source Code "
                          "Pro\",\"Menlo\",\"DejaVu Sans Mono\",\"Courier New\",monospace;'>"
                          "var x = 42;           // int\n"
                          "var pi = 3.14;        // float\n"
                          "var s = \"hello\";      // string\n"
                          "var arr = [1, 2, 3];  // array\n"
                          "fun add(a, b) {\n"
                          "    return a + b;\n"
                          "}\n"
                          "var f = add;          // closure\n"
                          "print(x, pi, s, arr, f);\n"
                          "</pre>"
                          "<p>调试时展开变量树节点，可看到 int/float 标量内联、string/array 堆指针的差异。</p>"));
    tour->addStep(pageLibraryBtn_, QString::fromUtf8("类型教学库"),
                  QString::fromUtf8("点击「类型教学库」切换到静态教学页，查看 int / string / array / closure "
                                    "等类型的 NaN-boxing 位布局与堆对象结构详解。"));
    tour->addStep(nullptr, QString::fromUtf8("开始实验"),
                  QString::fromUtf8("切换到类型教学库后，选中任一类型条目，点击「加载样例代码到主编辑器」，"
                                    "再按 F5 运行即可在实时变量树中对照观察。"));
    return tour;
}
