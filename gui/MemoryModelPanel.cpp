// ============================================================
// MemoryModelPanel.cpp — 内存模型可视化面板实现（第二波 P0-2）
// ============================================================

#include "gui/MemoryModelPanel.h"
#include "app/IdeController.h"
#include "interpreter/Value.h"
#include "interpreter/NaNBox.h"
#include "interpreter/RefCounted.h"
#include "interpreter/GcManager.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QHeaderView>
#include <QApplication>
#include <sstream>
#include <iomanip>

// ============================================================
// MemoryModelLibrary — 静态教学场景库
// ============================================================

namespace {

std::string bitsToHex(uint64_t bits) {
    std::ostringstream os;
    os << "0x" << std::hex << std::setfill('0') << std::setw(16) << bits;
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

const std::vector<NaNBoundingBox>& MemoryModelLibrary::nanBoxExamples() {
    static const std::vector<NaNBoundingBox> kExamples = {
        // int48 内联（正值）
        []() {
            NaNBox box = NaNBox::fromInt(42);
            NaNBoundingBox b;
            b.id = "int-inline-pos";
            b.title = "int 42 内联（int48 范围内）";
            b.description = "整数 42 在 int48 范围（|v| < 2^47）内，直接内联到 NaN-box 的低 48 位。"
                            "高 16 位为 INT_TAG_BASE = 0x7FF8，标记类型为 VAL_INT。"
                            "无需堆分配，Value 拷贝仅 8 字节赋值。";
            b.sourceExpr = "42";
            b.bits = box.rawBits();
            b.intVal = 42;
            b.isInt = true;
            return b;
        }(),
        // int48 内联（负值）
        []() {
            NaNBox box = NaNBox::fromInt((int64_t)-1);
            NaNBoundingBox b;
            b.id = "int-inline-neg";
            b.title = "int -1 内联（int48 负值）";
            b.description = "整数 -1 以补码形式存储到低 48 位（全 1），"
                            "解码时通过符号位扩展恢复 int64 值。";
            b.sourceExpr = "-1";
            b.bits = box.rawBits();
            b.intVal = -1;
            b.isInt = true;
            return b;
        }(),
        // int48 边界
        []() {
            int64_t boundary = (1LL << 46) - 1;  // int48 最大正值
            NaNBox box = NaNBox::fromInt(boundary);
            NaNBoundingBox b;
            b.id = "int-inline-boundary";
            b.title = "int48 最大正值（2^46 - 1）";
            b.description = "int48 范围上限：|v| < 2^47。"
                            "本例 v = 2^46 - 1 仍在范围内，仍内联存储。"
                            "若 v 超出范围（|v| ≥ 2^47）则触发装箱为 BoxedIntData* 堆对象。";
            b.sourceExpr = "(1<<46) - 1";
            b.bits = box.rawBits();
            b.intVal = boundary;
            b.isInt = true;
            return b;
        }(),
        // float64 直接存储
        []() {
            NaNBox box = NaNBox::fromFloat(3.14);
            NaNBoundingBox b;
            b.id = "float-direct";
            b.title = "float 3.14 直接存储（IEEE 754 double）";
            b.description = "浮点数直接以 IEEE 754 double 的 64 位原始位存储，"
                            "高 16 位是 exponent 字段。若 float 的位模式恰好落入 NaN-boxed tag 范围，"
                            "会被规范化为 NAN_BOXED_FLOAT_MARKER（罕见）。";
            b.sourceExpr = "3.14";
            b.bits = box.rawBits();
            b.floatVal = 3.14;
            b.isFloat = true;
            return b;
        }(),
        // bool
        []() {
            NaNBox box = NaNBox::fromBool(true);
            NaNBoundingBox b;
            b.id = "bool-true";
            b.title = "bool true（BOOL_TAG_BASE）";
            b.description = "bool 类型用 BOOL_TAG_BASE = 0x7FF9 编码，低 48 位存储 0/1。"
                            "true 编码为 0x7FF9...0001，false 为 0x7FF9...0000。";
            b.sourceExpr = "true";
            b.bits = box.rawBits();
            b.intVal = 1;  // true
            b.isBool = true;
            return b;
        }(),
        // null
        []() {
            NaNBox box = NaNBox::null();
            NaNBoundingBox b;
            b.id = "null-value";
            b.title = "null（NULL_BITS）";
            b.description = "null 值编码为固定常量 NULL_BITS = 0x7FFA000000000000，"
                            "payload 全为 0。";
            b.sourceExpr = "null";
            b.bits = box.rawBits();
            b.isNull = true;
            return b;
        }(),
        // 指针类型（堆对象，演示 PTR_TAG_BASE，指针值仅用于演示）
        []() {
            // 用一个安全的栈上对象作为指针示例（演示 PTR_TAG_BASE 编码）
            int dummy = 0;
            NaNBox box = NaNBox::fromPtr(&dummy);
            NaNBoundingBox b;
            b.id = "ptr-string";
            b.title = "string \"hello\" 装箱（PTR_TAG_BASE + 48 位指针）";
            b.description = "字符串 \"hello\" 长度超过 ASCII 内联阈值，装箱为 StringData* 堆对象。"
                            "高 16 位为 PTR_TAG_BASE = 0x7FFB，低 48 位为 StringData 对象的虚拟地址。"
                            "x86-64 用户态虚拟地址空间为 48 位，可直接编码。"
                            "本图示演示 PTR_TAG_BASE + 指针位模式，实际指针值会随运行而变化。";
            b.sourceExpr = "\"hello\"";
            b.bits = box.rawBits();
            b.isPointer = true;
            return b;
        }(),
    };
    return kExamples;
}

const std::vector<RefCountScenario>& MemoryModelLibrary::refCountScenarios() {
    static const std::vector<RefCountScenario> kScenarios = {
        {
            "array-basic",
            "数组基础生命周期",
            "演示 var a = [1,2,3] 的构造、拷贝与释放。"
            "ArrayData 继承 RefCounted，构造时 refCount=1。"
            "var b = a 共享所有权（refCount=2），不进行深拷贝（COW 语义）。"
            "b 离开作用域时 release，refCount 降为 1；a 离开时 release，refCount=0 触发析构。",
            {
                {"var a = [1,2,3]", 1, "构造新 ArrayData，refCount=1"},
                {"var b = a",       2, "Value 拷贝：addRef → refCount=2（共享所有权，未深拷贝）"},
                {"（b 离开作用域）", 1, "Value 析构：release → refCount=1"},
                {"（a 离开作用域）", 0, "Value 析构：release → refCount=0，delete this"},
            }
        },
        {
            "cow-detach",
            "COW 写时复制 detach",
            "演示 b = a 之后修改 b 触发 COW detach。"
            "COW detach 在写前检查 refCount==1，否则深拷贝自身。"
            "本例 refCount=2 时调用 b.push(4)，触发 detach：先 release 原 refCount（→1），"
            "再深拷贝出新的 ArrayData（refCount=1），最后写入新对象。",
            {
                {"var a = [1,2,3]", 1, "构造新 ArrayData，refCount=1"},
                {"var b = a",       2, "共享所有权，refCount=2"},
                {"b.push(4)",       1, "COW detach：原 refCount-- (→1)，深拷贝出新对象 refCount=1，写入新对象"},
                {"（新对象中 b[3]=4）", 1, "深拷贝后修改不影响 a，a 仍是 [1,2,3]"},
            }
        },
        {
            "string-shared",
            "字符串共享（无 COW）",
            "字符串 StringData 同样使用 RefCounted，但字符串不可变，无需 COW detach。"
            "var s2 = s1 共享所有权，s2 += 'x' 实际是构造新 StringData 赋值给 s2。",
            {
                {"var s1 = \"hello\"", 1, "构造新 StringData，refCount=1"},
                {"var s2 = s1",        2, "共享所有权，refCount=2"},
                {"s2 = s2 + \"!\"",      1, "构造新 StringData（'hello!'），s2 旧值 release（→1）"},
            }
        },
        {
            "instance-fields",
            "实例字段引用",
            "实例 InstanceData 持有字段表，字段值是 Value（可能引用其它堆对象）。"
            "实例的 refCount 与字段值 refCount 相互独立。",
            {
                {"class Point { ... }",         0, "类定义不创建实例，仅注册到 classRegistry_"},
                {"var p = Point.new()",         1, "构造新 InstanceData，refCount=1"},
                {"p.x = [1,2,3]",                1, "p.x 字段持有 ArrayData（refCount=1）"},
                {"var q = p",                    2, "InstanceData addRef → refCount=2"},
                {"（q 离开作用域）",            1, "InstanceData release → refCount=1（ArrayData 仍 refCount=1）"},
            }
        },
    };
    return kScenarios;
}

const std::vector<GcPhaseInfo>& MemoryModelLibrary::gcPhases() {
    static const std::vector<GcPhaseInfo> kPhases = {
        {
            "1. 注册（registerTracked）",
            "所有新建的 ArrayData / DictData / InstanceData 在构造函数中调用 GcManager::instance().registerTracked(this)，"
            "加入 tracked_ 列表与 aliveSet_。StringData/ClosureData 不注册（无循环引用风险）。"
            "注册为 O(1) 操作（vector::push_back + unordered_set::insert）。"
        },
        {
            "2. 触发时机",
            "Interpreter::execute() 在 resetState 之后、runStatements 之前调用 collectCycle(空根集)。"
            "此时上一轮残留的循环容器 refCount>0 仍 aliveSet_，本轮新建容器尚未注册，安全。"
            "执行期间不再触发（性能权衡：mark-sweep 开销 O(节点数+边数)，仅起点触发）。"
        },
        {
            "3. Mark 阶段",
            "从 roots（globals / VM 栈 / 调用帧中的 Value）出发，递归 mark 所有可达容器节点。"
            "markValue 检查 Value 类型：ArrayData → 遍历 elements；DictData → 遍历 entries；"
            "InstanceData → 遍历 fields。递归标记直到所有可达节点 marked=true。"
        },
        {
            "4. Sweep 阶段",
            "迭代 tracked_ 列表，对 aliveSet_ 仍在但 marked 未标记的节点（即不可达的循环孤岛）执行："
            "清空其子元素打破循环 → refCount 自然降为 0 → 节点析构 → onDestroyed 从 aliveSet_ 移除。"
            "存活节点重置 marked=false，为下一轮收集做准备。"
        },
        {
            "5. UAF 防护",
            "tracked_ 列表中的 RefCounted* 可能在 collectCycle 期间被析构（如 sweep 清空子元素后 refCount→0）。"
            "通过 aliveSet_ 区分存活对象与已释放的悬垂指针，避免迭代时访问已释放内存。"
            "析构钩子 onDestroyed 同步从 aliveSet_ 移除本指针，保证 aliveSet_ 与实际存活状态一致。"
        },
        {
            "6. 已知限制",
            "环形容器泄漏：a.append(a) 形成自环，refCount ≥ 2 永不归零。"
            "GC 通过 mark-sweep 可回收（mark 时 a 已 marked，sweep 不会误清），"
            "但仅当 a 的根引用被丢弃时才会被回收。若 a 仍在 globals 中，GC 不会触碰。"
        },
    };
    return kPhases;
}

// ============================================================
// MemoryModelPanel 实现
// ============================================================

MemoryModelPanel::MemoryModelPanel(QWidget* parent)
    : QWidget(parent) {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(4);

    // 顶部页签按钮
    auto* pageBar = new QHBoxLayout;
    pageNanBoxBtn_   = new QPushButton(QString::fromUtf8("NaN-boxing 编码"), this);
    pageRefCountBtn_ = new QPushButton(QString::fromUtf8("引用计数 & COW"), this);
    pageGcBtn_        = new QPushButton(QString::fromUtf8("GcManager mark-sweep"), this);
    pageNanBoxBtn_->setCheckable(true);
    pageRefCountBtn_->setCheckable(true);
    pageGcBtn_->setCheckable(true);
    pageBar->addWidget(pageNanBoxBtn_);
    pageBar->addWidget(pageRefCountBtn_);
    pageBar->addWidget(pageGcBtn_);
    pageBar->addStretch();
    mainLayout->addLayout(pageBar);

    stack_ = new QStackedWidget(this);
    mainLayout->addWidget(stack_, 1);

    auto* pageNanBox = new QWidget(this);
    auto* pageRefCount = new QWidget(this);
    auto* pageGc = new QWidget(this);
    buildNanBoxPage(pageNanBox);
    buildRefCountPage(pageRefCount);
    buildGcPage(pageGc);
    stack_->addWidget(pageNanBox);
    stack_->addWidget(pageRefCount);
    stack_->addWidget(pageGc);

    // 默认显示第一个页面
    pageNanBoxBtn_->setChecked(true);
    stack_->setCurrentIndex(0);

    connect(pageNanBoxBtn_, &QPushButton::clicked, this, [this]() {
        stack_->setCurrentIndex(0);
        pageNanBoxBtn_->setChecked(true);
        pageRefCountBtn_->setChecked(false);
        pageGcBtn_->setChecked(false);
    });
    connect(pageRefCountBtn_, &QPushButton::clicked, this, [this]() {
        stack_->setCurrentIndex(1);
        pageNanBoxBtn_->setChecked(false);
        pageRefCountBtn_->setChecked(true);
        pageGcBtn_->setChecked(false);
    });
    connect(pageGcBtn_, &QPushButton::clicked, this, [this]() {
        stack_->setCurrentIndex(2);
        pageNanBoxBtn_->setChecked(false);
        pageRefCountBtn_->setChecked(false);
        pageGcBtn_->setChecked(true);
        refreshGcStats();
    });

    populateNanBoxList();
    populateRefCountScenarios();
    populateGcPhases();
}

void MemoryModelPanel::buildNanBoxPage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    auto* splitter = new QSplitter(Qt::Horizontal, host);
    nanBoxList_ = new QListWidget(host);
    nanBoxDesc_ = new QTextBrowser(host);

    // 位图表格：8 行 × 8 列 = 64 位
    nanBoxBitTable_ = new QTableWidget(8, 8, host);
    nanBoxBitTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    nanBoxBitTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    nanBoxBitTable_->horizontalHeader()->setVisible(false);
    nanBoxBitTable_->verticalHeader()->setVisible(false);
    nanBoxBitTable_->setFixedHeight(180);
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) {
            auto* item = new QTableWidgetItem("0");
            item->setTextAlignment(Qt::AlignCenter);
            nanBoxBitTable_->setItem(r, c, item);
        }
        nanBoxBitTable_->setRowHeight(r, 20);
    }

    auto* rightContainer = new QWidget(host);
    auto* rightLayout = new QVBoxLayout(rightContainer);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->addWidget(new QLabel(QString::fromUtf8("位模式（64 位，高 16 位为 tag）：")), 0);
    rightLayout->addWidget(nanBoxBitTable_, 0);
    rightLayout->addWidget(new QLabel(QString::fromUtf8("说明：")), 0);
    rightLayout->addWidget(nanBoxDesc_, 1);

    splitter->addWidget(nanBoxList_);
    splitter->addWidget(rightContainer);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);
    splitter->setSizes({200, 400});
    layout->addWidget(splitter, 1);

    connect(nanBoxList_, &QListWidget::currentRowChanged,
            this, &MemoryModelPanel::populateNanBoxDetail);
}

void MemoryModelPanel::buildRefCountPage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    auto* splitter = new QSplitter(Qt::Horizontal, host);
    refCountScenarioList_ = new QListWidget(host);
    refCountDesc_ = new QTextBrowser(host);

    refCountStepTable_ = new QTableWidget(0, 3, host);
    refCountStepTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    refCountStepTable_->setHorizontalHeaderLabels({
        QString::fromUtf8("动作"),
        QString::fromUtf8("refCount"),
        QString::fromUtf8("备注"),
    });
    refCountStepTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    refCountStepTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    refCountStepTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);

    auto* rightContainer = new QWidget(host);
    auto* rightLayout = new QVBoxLayout(rightContainer);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->addWidget(new QLabel(QString::fromUtf8("场景说明：")), 0);
    rightLayout->addWidget(refCountDesc_, 1);
    rightLayout->addWidget(new QLabel(QString::fromUtf8("引用计数变化步骤：")), 0);
    rightLayout->addWidget(refCountStepTable_, 2);

    splitter->addWidget(refCountScenarioList_);
    splitter->addWidget(rightContainer);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);
    splitter->setSizes({200, 400});
    layout->addWidget(splitter, 1);

    connect(refCountScenarioList_, &QListWidget::currentRowChanged,
            this, &MemoryModelPanel::populateRefCountDetail);
}

void MemoryModelPanel::buildGcPage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    auto* statusbar = new QHBoxLayout;
    gcTrackedCountLabel_ = new QLabel(QString::fromUtf8("tracked 节点数：—"), host);
    gcRefreshBtn_ = new QPushButton(QString::fromUtf8("刷新统计"), host);
    statusbar->addWidget(gcTrackedCountLabel_);
    statusbar->addStretch();
    statusbar->addWidget(gcRefreshBtn_);
    layout->addLayout(statusbar);

    gcBrowser_ = new QTextBrowser(host);
    layout->addWidget(gcBrowser_, 1);

    connect(gcRefreshBtn_, &QPushButton::clicked, this, [this]() {
        refreshGcStats();
        QApplication::beep();
    });
}

void MemoryModelPanel::populateNanBoxList() {
    nanBoxList_->clear();
    const auto& items = MemoryModelLibrary::nanBoxExamples();
    for (const auto& b : items) {
        nanBoxList_->addItem(QString::fromUtf8(b.title.c_str()));
    }
    if (!items.empty()) {
        nanBoxList_->setCurrentRow(0);
    }
}

void MemoryModelPanel::populateNanBoxDetail(int index) {
    const auto& items = MemoryModelLibrary::nanBoxExamples();
    if (index < 0 || index >= (int)items.size()) return;
    const auto& b = items[index];

    // 位图填充
    uint64_t bits = b.bits;
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) {
            int bitIdx = r * 8 + c;  // 0..63
            bool on = (bits >> (63 - bitIdx)) & 1ULL;
            auto* item = nanBoxBitTable_->item(r, c);
            item->setText(on ? "1" : "0");
            // tag bits 高 16 位用底色区分
            if (bitIdx < 16) {
                item->setBackground(on ? QColor(0xCC, 0x66, 0x66) : QColor(0xFF, 0xEE, 0xEE));
            } else {
                item->setBackground(on ? QColor(0x66, 0x99, 0xCC) : QColor(0xEE, 0xF5, 0xFF));
            }
        }
    }

    // 描述
    std::ostringstream os;
    os << "<h3>" << b.title << "</h3>";
    os << "<p><b>表达式：</b> <code>" << b.sourceExpr << "</code></p>";
    os << "<p><b>原始位（hex）：</b> <code>" << bitsToHex(b.bits) << "</code></p>";
    os << "<p><b>原始位（binary）：</b> <code style='font-size:10pt;'>" << bitsToBinary(b.bits) << "</code></p>";
    os << "<hr>";
    os << "<p>" << b.description << "</p>";
    if (b.isInt)   os << "<p><b>解码值：</b> int = " << b.intVal << "</p>";
    if (b.isFloat) os << "<p><b>解码值：</b> float = " << b.floatVal << "</p>";
    if (b.isBool)  os << "<p><b>解码值：</b> bool = " << (b.intVal ? "true" : "false") << "</p>";
    if (b.isNull)  os << "<p><b>解码值：</b> null</p>";
    if (b.isPointer) os << "<p><b>解码值：</b> RefCounted* 指针（48 位虚拟地址）</p>";
    nanBoxDesc_->setHtml(QString::fromUtf8(os.str().c_str()));
}

void MemoryModelPanel::populateRefCountScenarios() {
    refCountScenarioList_->clear();
    const auto& items = MemoryModelLibrary::refCountScenarios();
    for (const auto& s : items) {
        refCountScenarioList_->addItem(QString::fromUtf8(s.title.c_str()));
    }
    if (!items.empty()) {
        refCountScenarioList_->setCurrentRow(0);
    }
}

void MemoryModelPanel::populateRefCountDetail(int index) {
    const auto& items = MemoryModelLibrary::refCountScenarios();
    if (index < 0 || index >= (int)items.size()) return;
    const auto& s = items[index];

    refCountDesc_->setText(QString::fromUtf8(s.description.c_str()));

    refCountStepTable_->setRowCount((int)s.steps.size());
    for (int i = 0; i < (int)s.steps.size(); ++i) {
        const auto& st = s.steps[i];
        refCountStepTable_->setItem(i, 0, new QTableWidgetItem(QString::fromUtf8(st.action.c_str())));
        auto* rcItem = new QTableWidgetItem(QString::number(st.refCount));
        rcItem->setTextAlignment(Qt::AlignCenter);
        refCountStepTable_->setItem(i, 1, rcItem);
        refCountStepTable_->setItem(i, 2, new QTableWidgetItem(QString::fromUtf8(st.note.c_str())));
    }
}

void MemoryModelPanel::populateGcPhases() {
    std::ostringstream os;
    os << "<h2>GcManager — 循环引用垃圾收集器</h2>";
    os << "<p>侵入式引用计数无法回收循环引用（如 <code>a=[]; a.append(a)</code>）。"
       << "GcManager 实现轻量级 mark-sweep，周期性扫描所有容器节点，"
       << "标记从根集可达的对象，释放不可达的循环孤岛。</p>";
    os << "<hr>";
    for (const auto& p : MemoryModelLibrary::gcPhases()) {
        os << "<h3>" << p.title << "</h3>";
        os << "<p>" << p.description << "</p>";
    }
    gcBrowser_->setHtml(QString::fromUtf8(os.str().c_str()));
}

void MemoryModelPanel::refreshGcStats() {
    if (!gcTrackedCountLabel_) return;
    size_t tracked = GcManager::instance().trackedCount();
    gcTrackedCountLabel_->setText(QString::fromUtf8("tracked 节点数：%1").arg(tracked));
}
