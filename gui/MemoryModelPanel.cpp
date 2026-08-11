// ============================================================
// MemoryModelPanel.cpp — 内存模型可视化面板实现（第二波 P0-2）
// ============================================================

#include "gui/MemoryModelPanel.h"
#include "app/IdeController.h"
#include "common/MemoryInspectionAPI.h" // ARCH-10: NaNBox/GcManager/RefCounted 只读检视（替代直接依赖 interpreter/ 内部头文件）
#include "gui/MarkdownRenderer.h"
#include "gui/PanelAnimator.h"
#include "gui/TeachingSubPageBar.h" // UX-R2 fix: 统一子页切换组件
#include "gui/TeachingTheme.h"      // UX-R2 fix: 硬编码颜色迁移到语义色
#include "interpreter/Value.h"
#include "interpreter/ValueData.h" // R113 B 项：VMUpvalue 完整定义（isClosed/stackSlot/owningFrameIdx）

#include <QApplication>
#include <QColor>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "Label.h"      // QFluentKit（StrongBodyLabel）
#include "PushButton.h" // QFluentKit（PrimaryPushButton）

// ============================================================
// 第 4 子页匿名辅助（堆对象类型名 + 字段数）
// ============================================================
//
// P3-20 拆分：MemoryModelLibrary 与 MemoryAnimLibrary 的静态场景实现已迁移
// 至 gui/MemoryModelLibrary.cpp（与 BugHuntLibrary.cpp 拆分模式一致）。
// 本文件仅保留面板 UI 与交互逻辑所需的匿名命名空间辅助函数。
// ============================================================

namespace {

/// 将 64 位 NaN-box 原始位模式格式化为 0x 前缀的 16 位十六进制字符串。
std::string bitsToHex(uint64_t bits) {
    std::ostringstream os;
    os << "0x" << std::hex << std::setfill('0') << std::setw(16) << bits;
    return os.str();
}

/// 将 64 位位模式逐位展开为长度 64 的 "0/1" 字符串（最高位在索引 0）。
std::string bitsToBinary(uint64_t bits) {
    std::string s(64, '0');
    for (int i = 0; i < 64; ++i) {
        if (bits & (1ULL << (63 - i)))
            s[i] = '1';
    }
    return s;
}

/// 根据 ValueType 返回堆对象 C++ 结构名（用于"地址/类型"表格展示）
/// ARCH-10: 通过 MemoryInspectionAPI 获取堆对象元信息，消除对 RefCounted* 的直接访问。
std::string heapStructName(const Value& v) {
    return MemoryInspectionAPI::inspectHeap(v).structName;
}

/// 将指针格式化为 16 进制字符串
std::string ptrToHex(const void* p) {
    std::ostringstream os;
    os << "0x" << std::hex << std::setfill('0') << std::setw(16) << reinterpret_cast<uintptr_t>(p);
    return os.str();
}

} // namespace

// ============================================================
// MemoryModelPanel 实现
// ============================================================

/// 构造面板：组装 TeachingSubPageBar 子页切换组件（NaN-boxing / 引用计数&COW /
/// GcManager / 实时动画 / RegisterVM 寄存器帧），构建各子页、配置动画刷新定时器
/// （2s 安全网），并默认显示第一个子页。
MemoryModelPanel::MemoryModelPanel(QWidget* parent) : QWidget(parent) {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(4);

    // UX-R2 fix: 子页切换统一为 TeachingSubPageBar 组件（替代手写 5 按钮互斥逻辑）
    // 互斥选中态 / 主题色高亮 / 滑入动画由组件内置
    subPageBar_ = new TeachingSubPageBar(this);
    mainLayout->addLayout(subPageBar_->buttonBar());

    stack_ = subPageBar_->stack();
    mainLayout->addWidget(stack_, 1);

    auto* pageNanBox = new QWidget(this);
    auto* pageRefCount = new QWidget(this);
    auto* pageGc = new QWidget(this);
    auto* pageAnim = new QWidget(this);
    auto* pageRegVm = new QWidget(this);
    buildNanBoxPage(pageNanBox);
    buildRefCountPage(pageRefCount);
    buildGcPage(pageGc);
    buildAnimPage(pageAnim);
    buildRegVmPage(pageRegVm);
    subPageBar_->addPage(QString::fromUtf8("① NaN-boxing 编码"), pageNanBox);
    subPageBar_->addPage(QString::fromUtf8("② 引用计数 & COW"), pageRefCount);
    subPageBar_->addPage(QString::fromUtf8("③ GcManager mark-sweep"), pageGc);
    subPageBar_->addPage(QString::fromUtf8("④ 实时动画"), pageAnim);
    subPageBar_->addPage(QString::fromUtf8("⑤ RegisterVM 寄存器帧"), pageRegVm);

    // OPT-1: 第 4 子页自动刷新定时器降频 500ms→2000ms，状态变更由 vmStateChanged
    // 监听器即时触发 refreshAnimState（见 setController）。QTimer 作为安全网。
    // R113 A 项: 该定时器同时驱动第 5 子页 RegisterVM 寄存器帧刷新。
    animTimer_ = new QTimer(this);
    animTimer_->setInterval(2000);
    animTimer_->setSingleShot(false);
    connect(animTimer_, &QTimer::timeout, this, [this]() {
        refreshAnimState();
        refreshRegVmState();
    });

    // 子页切换时触发对应刷新（GC 统计 / 动画状态 / RegisterVM 状态）
    connect(subPageBar_, &TeachingSubPageBar::currentChanged, this, [this](int idx) {
        if (idx == 2) {
            refreshGcStats();
        } else if (idx == 3) {
            refreshAnimState();
        } else if (idx == 4) {
            refreshRegVmState();
        }
    });

    populateNanBoxList();
    populateRefCountScenarios();
    populateGcPhases();
}

/// 面板重新可见时：若用户已开启实时动画自动刷新，则立即刷新一次并重启 2s 定时器。
void MemoryModelPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // 面板可见时，若用户已开启第 4 子页自动刷新则恢复 QTimer
    if (animAutoRefreshBtn_ && animAutoRefreshBtn_->isChecked() && animTimer_ && !animTimer_->isActive()) {
        refreshAnimState();
        // AUDIT-P1 fix: 原 start(500) 会覆盖 setInterval(2000) 的降频优化。
        // 改为无参 start() 沿用已设的 2000ms interval，与 OPT-1 降频意图一致。
        animTimer_->start();
    }
}

/// 面板隐藏时停止实时动画定时器，避免后台空转。
void MemoryModelPanel::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    // 面板隐藏时停止动画 QTimer，避免后台空转
    if (animTimer_ && animTimer_->isActive()) {
        animTimer_->stop();
    }
}

/// 绑定/换绑 IdeController：注册 vmStateChanged 监听器（owner=this），
/// 换绑前先反注册旧监听器，避免 controller 持有悬垂回调。
void MemoryModelPanel::setController(IdeController* controller) {
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
/// 析构时反注册 vmStateChanged 监听器，避免 controller 持有悬垂 this 回调。
MemoryModelPanel::~MemoryModelPanel() {
    if (controller_) {
        controller_->removeVmStateChangedListener(this);
    }
}

/// 构建「NaN-boxing 编码」子页：左侧示例列表 + 右侧 8×8 位图表格（高 16 位
/// tag 用红底区分）+ 说明浏览器，选中项变化时通过 populateNanBoxDetail 渲染。
/// R113 D 项：右侧顶部增加"自定义编码"区，支持用户输入 int/float/bool/hex 类型
/// + 值实时编码并显示位图与解读。
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

    // R113 D 项：自定义编码区——类型选择 + 值输入 + 编码按钮
    auto* customBox = new QGroupBox(QString::fromUtf8("🛠 自定义编码"), host);
    auto* customLayout = new QHBoxLayout(customBox);
    customLayout->addWidget(new QLabel(QString::fromUtf8("类型：")));
    nanBoxCustomTypeCombo_ = new QComboBox(host);
    nanBoxCustomTypeCombo_->addItem(QString::fromUtf8("int"), QVariant(QString::fromUtf8("int")));
    nanBoxCustomTypeCombo_->addItem(QString::fromUtf8("float"), QVariant(QString::fromUtf8("float")));
    nanBoxCustomTypeCombo_->addItem(QString::fromUtf8("bool"), QVariant(QString::fromUtf8("bool")));
    nanBoxCustomTypeCombo_->addItem(QString::fromUtf8("null"), QVariant(QString::fromUtf8("null")));
    nanBoxCustomTypeCombo_->addItem(QString::fromUtf8("hex (64位)"), QVariant(QString::fromUtf8("hex")));
    customLayout->addWidget(nanBoxCustomTypeCombo_);
    customLayout->addWidget(new QLabel(QString::fromUtf8("值：")));
    nanBoxCustomValueEdit_ = new QLineEdit(QString::fromUtf8("42"), host);
    customLayout->addWidget(nanBoxCustomValueEdit_, 1);
    nanBoxCustomEncodeBtn_ = new PrimaryPushButton(QString::fromUtf8("编码并添加"), host);
    customLayout->addWidget(nanBoxCustomEncodeBtn_);
    // 类型切换时同步默认值提示
    connect(nanBoxCustomTypeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int /*idx*/) {
        QString t = nanBoxCustomTypeCombo_->currentData().toString();
        if (t == QString::fromUtf8("int"))
            nanBoxCustomValueEdit_->setText(QString::fromUtf8("42"));
        else if (t == QString::fromUtf8("float"))
            nanBoxCustomValueEdit_->setText(QString::fromUtf8("3.14"));
        else if (t == QString::fromUtf8("bool"))
            nanBoxCustomValueEdit_->setText(QString::fromUtf8("true"));
        else if (t == QString::fromUtf8("null"))
            nanBoxCustomValueEdit_->setText(QString::fromUtf8(""));
        else if (t == QString::fromUtf8("hex"))
            nanBoxCustomValueEdit_->setText(QString::fromUtf8("0x7FF800000000002A"));
    });
    connect(nanBoxCustomEncodeBtn_, &QPushButton::clicked, this, &MemoryModelPanel::onNanBoxCustomEncode);

    auto* rightContainer = new QWidget(host);
    auto* rightLayout = new QVBoxLayout(rightContainer);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->addWidget(customBox, 0);
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

    connect(nanBoxList_, &QListWidget::currentRowChanged, this, &MemoryModelPanel::populateNanBoxDetail);
}

/// 构建「引用计数 & COW」子页：左侧场景列表 + 右侧场景说明 + 引用计数变化
/// 步骤表（动作 / refCount / 备注），选中项变化时通过 populateRefCountDetail 填充。
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

    connect(refCountScenarioList_, &QListWidget::currentRowChanged, this, &MemoryModelPanel::populateRefCountDetail);
}

/// 构建「GcManager mark-sweep」子页：顶部 tracked 节点数状态标签 + 刷新按钮 +
/// 阶段说明浏览器（内容由 populateGcPhases 静态填充）。
void MemoryModelPanel::buildGcPage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    auto* statusbar = new QHBoxLayout;
    gcTrackedCountLabel_ = new StrongBodyLabel(QString::fromUtf8("tracked 节点数：—"), host);
    gcRefreshBtn_ = new PrimaryPushButton(QString::fromUtf8("刷新统计"), host);
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

/// 用 NaN-box 示例标题填充左侧列表并默认选中首项。
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

/// 根据选中索引渲染 NaN-box 详情：将 rawBits 逐位填入 8×8 表格（高 16 位 tag
/// 红底区分），并显示表达式、hex/binary 位模式、Markdown 说明与解码值。
/// R113 D 项：列表 index >= 静态示例数时，从 customBoxes_ 取用户自定义编码。
void MemoryModelPanel::populateNanBoxDetail(int index) {
    const auto& items = MemoryModelLibrary::nanBoxExamples();
    if (index < 0)
        return;
    if (index < (int)items.size()) {
        renderNanBoxDetail(items[index]);
        return;
    }
    int customIdx = index - (int)items.size();
    if (customIdx >= 0 && customIdx < (int)customBoxes_.size()) {
        renderNanBoxDetail(customBoxes_[customIdx]);
    }
}

/// R113 D 项：将单个 NaNBoundingBox 渲染到位图表格 + 说明浏览器（供示例与自定义编码共用）。
void MemoryModelPanel::renderNanBoxDetail(const NaNBoundingBox& b) {
    // 位图填充
    uint64_t bits = b.bits;
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) {
            int bitIdx = r * 8 + c; // 0..63
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
    // R52-4 fix: title/sourceExpr 含 C++ 模板语法 < > 需 HTML 转义
    auto escMmp = [](const std::string& s) -> std::string {
        return QString::fromUtf8(s.c_str()).toHtmlEscaped().toStdString();
    };
    os << "<h3>" << escMmp(b.title) << "</h3>";
    os << "<p><b>表达式：</b> <code>" << escMmp(b.sourceExpr) << "</code></p>";
    os << "<p><b>原始位（hex）：</b> <code>" << bitsToHex(b.bits) << "</code></p>";
    os << "<p><b>原始位（binary）：</b> <code style='font-size:10pt;'>" << bitsToBinary(b.bits) << "</code></p>";
    os << "<hr>";
    os << MarkdownRenderer::markdownToHtmlFragment(b.description).toStdString();
    if (b.isInt)
        os << "<p><b>解码值：</b> int = " << b.intVal << "</p>";
    if (b.isFloat)
        os << "<p><b>解码值：</b> float = " << b.floatVal << "</p>";
    if (b.isBool)
        os << "<p><b>解码值：</b> bool = " << (b.intVal ? "true" : "false") << "</p>";
    if (b.isNull)
        os << "<p><b>解码值：</b> null</p>";
    if (b.isPointer)
        os << "<p><b>解码值：</b> RefCounted* 指针（48 位虚拟地址）</p>";
    nanBoxDesc_->setHtml(QString::fromUtf8(os.str().c_str()));
}

/// R113 D 项：根据当前类型+值输入编码 NaN-box，将结果作为自定义示例追加到列表并选中显示。
/// 错误输入（int 超范围 / hex 格式错误 / float 解析失败）在说明浏览器中显示错误信息，
/// 不抛异常不弹窗，保持教学场景下的友好交互。
void MemoryModelPanel::onNanBoxCustomEncode() {
    QString type = nanBoxCustomTypeCombo_->currentData().toString();
    QString valueStr = nanBoxCustomValueEdit_->text().trimmed();

    NaNBoundingBox b;
    std::ostringstream errOs; // 错误信息（若有）

    try {
        if (type == QString::fromUtf8("int")) {
            int64_t v = std::stoll(valueStr.toStdString());
            // ARCH-10: 通过 MemoryInspectionAPI 封装 NaNBox 编码逻辑，消除直接依赖。
            bool canEncode = MemoryInspectionAPI::canEncodeInt(v);
            uint64_t bits = MemoryInspectionAPI::encodeInt(v); // 超范围会降级为 float 并 LOG_WARNING
            b.bits = bits;
            if (canEncode) {
                b.isInt = true;
                b.intVal = v;
                b.title = "🛠 自定义 int " + valueStr.toStdString();
                b.description = "🛠 用户自定义编码：int " + valueStr.toStdString() +
                                " 在 int48 范围内，直接内联到低 48 位。"
                                " 高 16 位为 INT_TAG_BASE = 0x7FF8。"
                                " **交互式学习：** 试试输入超出 int48 范围的值（如 2^47），"
                                "系统会自动降级为 float 编码并显示警告。";
            } else {
                // 降级为 float：显示警告
                b.isFloat = true;
                b.floatVal = MemoryInspectionAPI::decodeFloat(bits);
                b.title = "🛠 自定义 int " + valueStr.toStdString() + " (降级为 float)";
                b.description = "⚠️ **降级警告：** int " + valueStr.toStdString() +
                                " 超出 int48 范围（|v| < 2^47），NaNBox::fromInt 自动降级为 float 编码。"
                                " 这是 MiniLang 的设计决策：超大整数用 BoxedIntData 堆包装，"
                                "但 NaNBox::fromInt 直接降级为 float 以避免 abort。"
                                " **对比：** 对比 `int 42` 的内联编码，观察位模式差异。";
            }
            b.sourceExpr = valueStr.toStdString();
        } else if (type == QString::fromUtf8("float")) {
            double v = std::stod(valueStr.toStdString());
            // ARCH-10: 通过 MemoryInspectionAPI 封装 NaNBox 编码逻辑
            b.bits = MemoryInspectionAPI::encodeFloat(v);
            b.isFloat = true;
            b.floatVal = v;
            b.title = "🛠 自定义 float " + valueStr.toStdString();
            b.description = "🛠 用户自定义编码：float " + valueStr.toStdString() +
                            " 直接以 IEEE 754 double 存储。"
                            " **NaN-boxing 设计：** float 不需要 tag 标签，因为 IEEE 754 NaN 的位模式"
                            "天然不与 int/bool/null/pointer tag 冲突。"
                            " 若 float 的位模式落入 NaN-boxed 范围，会被规范化为 NAN_BOXED_FLOAT_MARKER。";
            b.sourceExpr = valueStr.toStdString();
        } else if (type == QString::fromUtf8("bool")) {
            bool v = (valueStr.toLower() == QString::fromUtf8("true") || valueStr == QString::fromUtf8("1"));
            // ARCH-10: 通过 MemoryInspectionAPI 封装 NaNBox 编码逻辑
            b.bits = MemoryInspectionAPI::encodeBool(v);
            b.isBool = true;
            b.intVal = v ? 1 : 0;
            b.title = std::string("🛠 自定义 bool ") + (v ? "true" : "false");
            b.description = std::string("🛠 用户自定义编码：bool ") + (v ? "true" : "false") +
                            " 编码到 BOOL_TAG_BASE = 0x7FF9，低 48 位为 0/1。"
                            " **设计精妙：** bool 只占 1 位，但与 int 共享 NaN-box 8 字节格式，"
                            "无需堆分配，Value 拷贝仅 8 字节赋值。";
            b.sourceExpr = v ? "true" : "false";
        } else if (type == QString::fromUtf8("null")) {
            // ARCH-10: 通过 MemoryInspectionAPI 封装 NaNBox 编码逻辑
            b.bits = MemoryInspectionAPI::encodeNull();
            b.isNull = true;
            b.title = "🛠 自定义 null";
            b.description = std::string("🛠 用户自定义编码：null 编码到 NULL_BITS = 0x7FFA000000000000。") +
                            " **设计：** null 是单一固定值，无 payload，整个 64 位都是 tag。"
                            " 与 C++ 的 nullptr 解码一致（isNull() && asPtr() == nullptr）。";
            b.sourceExpr = "null";
        } else if (type == QString::fromUtf8("hex")) {
            // 解析 64 位 hex（支持 0x 前缀或不带前缀）
            std::string s = valueStr.toStdString();
            if (s.empty()) {
                errOs << "<p style='color:red;'>❌ hex 输入为空，请输入 16 位十六进制（如 0x7FF800000000002A）</p>";
            } else {
                uint64_t v = std::stoull(s, nullptr, 16);
                b.bits = v;
                // NaNBox::bits_ 是 private，无 fromRawBits public 接口，
                // 此处通过 tag 字段（高 16 位）手动解码 + 符号位扩展。
                uint64_t tagField = v & 0xFFFF000000000000ULL;
                if (tagField == 0x7FF8000000000000ULL) {
                    b.isInt = true;
                    // 手动解码 int48（带符号扩展）
                    int64_t payload = v & 0x0000FFFFFFFFFFFFULL;
                    if (payload & 0x0000800000000000ULL) {
                        payload |= 0xFFFF000000000000ULL; // 符号位扩展
                    }
                    b.intVal = payload;
                    b.title = "🛠 自定义 hex " + bitsToHex(v) + " (int tag)";
                    b.description = "🛠 用户自定义 hex 位模式：tag = 0x7FF8 (INT)，payload = int48。"
                                    " **手动解码：** 通过符号位扩展恢复 int64 值。";
                } else if (tagField == 0x7FF9000000000000ULL) {
                    b.isBool = true;
                    b.intVal = (v & 0x1ULL) ? 1 : 0;
                    b.title = "🛠 自定义 hex " + bitsToHex(v) + " (bool tag)";
                    b.description = "🛠 用户自定义 hex 位模式：tag = 0x7FF9 (BOOL)，payload = 0/1。";
                } else if (tagField == 0x7FFA000000000000ULL) {
                    b.isNull = true;
                    b.title = "🛠 自定义 hex " + bitsToHex(v) + " (null tag)";
                    b.description = "🛠 用户自定义 hex 位模式：tag = 0x7FFA (NULL)。";
                } else if (tagField == 0x7FFB000000000000ULL) {
                    b.isPointer = true;
                    b.title = "🛠 自定义 hex " + bitsToHex(v) + " (pointer tag)";
                    b.description = "🛠 用户自定义 hex 位模式：tag = 0x7FFB (POINTER)，payload = 48 位虚拟地址。"
                                    " **注意：** 此位模式不对应真实堆对象，仅作教学演示。";
                } else {
                    // 可能是 float 或无效 tag
                    b.isFloat = true;
                    // 手动解码 double（通过 memcpy 避免严格别名）
                    double dv;
                    std::memcpy(&dv, &v, sizeof(double));
                    b.floatVal = dv;
                    b.title = "🛠 自定义 hex " + bitsToHex(v) + " (float/其他)";
                    b.description =
                        "🛠 用户自定义 hex 位模式：tag 不在 INT/BOOL/NULL/PTR 范围内，"
                        "按 IEEE 754 double 解码。"
                        " **若位模式落入 NaN-boxed 范围（0x7FFC...）：** 系统会规范化为 NAN_BOXED_FLOAT_MARKER。";
                }
                b.sourceExpr = bitsToHex(v);
            }
        } else {
            errOs << "<p style='color:red;'>❌ 未知类型：" << type.toStdString() << "</p>";
        }
    } catch (const std::exception& e) {
        errOs << "<p style='color:red;'>❌ 解析失败：" << e.what() << "</p>"
              << "<p>请检查输入格式：int 用十进制整数 / float 用十进制小数 / "
              << "bool 用 true 或 false / hex 用 16 位十六进制（如 0x7FF800000000002A）</p>";
    }

    if (!errOs.str().empty()) {
        nanBoxDesc_->setHtml(QString::fromUtf8(errOs.str().c_str()));
        return;
    }

    customBoxes_.push_back(b);
    nanBoxList_->addItem(QString::fromUtf8(b.title.c_str()));
    // 选中新增项触发渲染
    nanBoxList_->setCurrentRow(nanBoxList_->count() - 1);
}

/// 用引用计数场景标题填充左侧列表并默认选中首项。
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

/// 根据选中索引渲染引用计数详情：场景说明 + 逐步 refCount 变化表
/// （动作 / refCount 值 / 备注），直观展示 addRef/release 与 COW detach 时机。
void MemoryModelPanel::populateRefCountDetail(int index) {
    const auto& items = MemoryModelLibrary::refCountScenarios();
    if (index < 0 || index >= (int)items.size())
        return;
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

/// 静态渲染 GcManager 阶段说明（注册/触发/Mark/Sweep/UAF 防护/已知限制）到
/// 浏览器；内容来自 MemoryModelLibrary::gcPhases()，仅初始化时填充一次。
void MemoryModelPanel::populateGcPhases() {
    std::ostringstream os;
    os << "<h2>GcManager — 循环引用垃圾收集器</h2>";
    os << "<p>侵入式引用计数无法回收循环引用（如 <code>a=[]; a.append(a)</code>），"
       << "因为它像「几个人合租，最后一人搬走才退租」——可若两人互相抓住对方、谁也不肯松手，"
       << "计数永远大于零、永远退不了租。GcManager 实现轻量级 mark-sweep，周期性扫描所有容器节点，"
       << "像警察挨家挨户查户口，标记从根集可达的对象，释放不可达的循环孤岛。</p>";
    os << "<hr>";
    for (const auto& p : MemoryModelLibrary::gcPhases()) {
        os << "<h3>" << p.title << "</h3>";
        os << MarkdownRenderer::markdownToHtmlFragment(p.description).toStdString();
    }
    gcBrowser_->setHtml(QString::fromUtf8(os.str().c_str()));
}

/// 刷新「tracked 节点数」状态标签：读取 GcManager 单例当前 tracked 容器数量。
/// ARCH-10: 通过 MemoryInspectionAPI 间接访问 GcManager，消除直接依赖。
void MemoryModelPanel::refreshGcStats() {
    if (!gcTrackedCountLabel_)
        return;
    size_t tracked = MemoryInspectionAPI::getGcStats().trackedCount;
    gcTrackedCountLabel_->setText(QString::fromUtf8("tracked 节点数：%1").arg(tracked));
}

// ============================================================
// 第 4 子页：实时动画（与 VM 单步执行联动）
// ============================================================

/// 构建「实时动画」子页：顶部 5 个状态标签（VM 状态/OpCode/栈深度/栈帧数/
/// GC tracked）+ 中间堆对象表（地址/类型/refCount/字段数）+ 底部 GC 阶段说明
/// 浏览器与自动刷新按钮。堆对象表与状态由 refreshAnimState 实时填充。
void MemoryModelPanel::buildAnimPage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    // ---- 顶部状态栏：5 个 QLabel 横向排列 ----
    auto* statusbar = new QHBoxLayout;
    animStatusLabel_ = new QLabel(QString::fromUtf8("VM 状态：—"), host);
    animOpCodeLabel_ = new QLabel(QString::fromUtf8("OpCode：—"), host);
    animStackDepthLabel_ = new QLabel(QString::fromUtf8("栈深度：—"), host);
    animFrameCountLabel_ = new QLabel(QString::fromUtf8("栈帧数：—"), host);
    animGcTrackedLabel_ = new QLabel(QString::fromUtf8("GC tracked：—"), host);
    // UX-R2 fix: 统一样式迁移到 TeachingTheme 语义色（替代硬编码 #F5F5F5/#CCC）
    const QString labelStyle = QStringLiteral("QLabel { background-color: %1; padding: 4px 8px; "
                                              "border: 1px solid %2; border-radius: 3px; }")
                                   .arg(TeachingTheme::statusBg().name(), TeachingTheme::statusBorder().name());
    for (auto* l :
         {animStatusLabel_, animOpCodeLabel_, animStackDepthLabel_, animFrameCountLabel_, animGcTrackedLabel_}) {
        l->setStyleSheet(labelStyle);
        l->setAlignment(Qt::AlignCenter);
    }
    statusbar->addWidget(animStatusLabel_);
    statusbar->addWidget(animOpCodeLabel_);
    statusbar->addWidget(animStackDepthLabel_);
    statusbar->addWidget(animFrameCountLabel_);
    statusbar->addWidget(animGcTrackedLabel_);
    layout->addLayout(statusbar);

    // R113 C 项：GC 进度统计栏——单行展示当前阶段 + 上次 GC 结果 + 累计次数
    animGcStatsLabel_ = new QLabel(QString::fromUtf8("GC 阶段：— | 上次标记 —, 回收 — | 累计 — 次"), host);
    // UX-R2 fix: GC 统计栏样式迁移到 TeachingTheme 警告卡片色系（替代硬编码 #FFFAEC/#E0C070/#5A4500）
    animGcStatsLabel_->setStyleSheet(
        QStringLiteral("QLabel { background-color: %1; padding: 4px 8px; border: 1px solid %2; "
                       "border-radius: 3px; color: %3; }")
            .arg(TeachingTheme::warningBg().name(), TeachingTheme::warningBorder().name(),
                 TeachingTheme::warningText().name()));
    layout->addWidget(animGcStatsLabel_);

    // ---- 中间堆对象表 + 详情浏览器：QSplitter 垂直分栏 ----
    // R113 B 项：选中堆对象表行 → 下方详情浏览器显示该对象的字段与 upvalue 生命周期。
    // 上半为堆对象表（4 列），下半为 QTextBrowser 详情区，可拖动分隔条调整比例。
    auto* heapSplitter = new QSplitter(Qt::Vertical, host);
    heapSplitter->setChildrenCollapsible(false);
    layout->addWidget(heapSplitter, 2);

    heapObjectTable_ = new QTableWidget(0, 4, host);
    heapObjectTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    heapObjectTable_->setHorizontalHeaderLabels({
        QString::fromUtf8("地址"),
        QString::fromUtf8("类型"),
        QString::fromUtf8("refCount"),
        QString::fromUtf8("字段数/元素数"),
    });
    heapObjectTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    heapObjectTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    heapObjectTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    heapObjectTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    heapObjectTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    heapObjectTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    heapSplitter->addWidget(heapObjectTable_);

    // R113 B 项：详情浏览器——选中堆对象表行后，此处展示该对象的字段详情；
    // ClosureData 重点展示 upvalue 生命周期（open/closed 状态、stackSlot、owningFrameIdx）。
    heapObjectDetailBrowser_ = new QTextBrowser(host);
    heapObjectDetailBrowser_->setOpenExternalLinks(false);
    heapObjectDetailBrowser_->setOpenLinks(false);
    heapObjectDetailBrowser_->setPlaceholderText(
        QString::fromUtf8("选中上方堆对象表中的一行，查看对象详情（闭包对象会显示 upvalue 生命周期状态）"));
    heapSplitter->addWidget(heapObjectDetailBrowser_);

    // 默认比例：堆对象表 3 : 详情 2
    heapSplitter->setStretchFactor(0, 3);
    heapSplitter->setStretchFactor(1, 2);
    heapSplitter->setSizes({300, 200});

    // 选中行变化时刷新详情浏览器
    connect(heapObjectTable_, &QTableWidget::currentItemChanged, this,
            [this](QTableWidgetItem* cur, QTableWidgetItem* /*prev*/) {
                int row = cur ? cur->row() : -1;
                onHeapObjectSelected(row);
            });

    // ---- 底部 GC 阶段说明（QLabel + QTextBrowser 显示 4 阶段说明）----
    gcPhaseLabel_ = new QLabel(QString::fromUtf8("GC 动画阶段说明（mark / sweep / reset / idle）："), host);
    layout->addWidget(gcPhaseLabel_);
    gcPhaseBrowser_ = new QTextBrowser(host);
    gcPhaseBrowser_->setOpenExternalLinks(false);
    gcPhaseBrowser_->setOpenLinks(false);
    layout->addWidget(gcPhaseBrowser_, 1);

    // 静态填充 4 阶段 + 6 类型说明（来自 MemoryAnimLibrary，仅初始化一次）
    {
        std::ostringstream os;
        os << "<h3>GC 动画 4 阶段</h3>";
        os << "<p>本面板与 VM 单步执行联动，实时展示堆对象图、refCount 变化与 GC 阶段。</p>";
        const auto& phases = MemoryAnimLibrary::gcAnimPhases();
        for (const auto& p : phases) {
            os << "<p><span style='color:" << p.color << ";font-weight:bold;font-size:14pt;'>●</span> "
               << "<b>" << p.phase << "</b>: " << p.description << "</p>";
        }
        os << "<hr><p><b>堆对象类型说明（6 种）：</b></p><ul>";
        const auto& types = MemoryAnimLibrary::heapObjectTypes();
        for (const auto& kv : types) {
            // R52-5 fix: kv.second 含 std::vector<Value> 等 C++ 模板语法 < > 需转义
            os << "<li><code>" << QString::fromUtf8(kv.first.c_str()).toHtmlEscaped().toStdString() << "</code> — "
               << QString::fromUtf8(kv.second.c_str()).toHtmlEscaped().toStdString() << "</li>";
        }
        os << "</ul>";
        gcPhaseBrowser_->setHtml(QString::fromUtf8(os.str().c_str()));
    }

    // ---- 底部按钮：刷新 + 自动刷新切换 ----
    auto* btnbar = new QHBoxLayout;
    animRefreshBtn_ = new QPushButton(QString::fromUtf8("刷新"), host);
    animAutoRefreshBtn_ = new QPushButton(QString::fromUtf8("自动刷新（2s）"), host);
    animAutoRefreshBtn_->setCheckable(true);
    btnbar->addWidget(animRefreshBtn_);
    btnbar->addWidget(animAutoRefreshBtn_);
    btnbar->addStretch();
    layout->addLayout(btnbar);

    connect(animRefreshBtn_, &QPushButton::clicked, this, [this]() {
        refreshAnimState();
        QApplication::beep();
    });
    connect(animAutoRefreshBtn_, &QPushButton::clicked, this, [this]() {
        if (animAutoRefreshBtn_->isChecked()) {
            // OPT-1: 500ms→2000ms，状态变更由 vmStateChanged 监听器即时触发。
            animTimer_->start(2000);
            animAutoRefreshBtn_->setText(QString::fromUtf8("停止自动刷新"));
            refreshAnimState();
        } else {
            // P1 #38 fix: 若第 5 子页（RegisterVM）仍开启自动刷新，则保持 animTimer_ 运行（对称启停）
            if (!(regVmAutoRefreshBtn_ && regVmAutoRefreshBtn_->isChecked()))
                animTimer_->stop();
            animAutoRefreshBtn_->setText(QString::fromUtf8("自动刷新（2s）"));
        }
    });
}

/// 实时刷新第 4 子页，与 VM 单步执行联动，分三步：
/// 1. 未绑定 controller 或 VM 未初始化时显示占位；
/// 2. 已初始化则从 controller 取 VM 状态（运行状态、当前 OpCode 名、操作数栈
///    getVmStack、栈帧数 getVmFrameCount）与 GcManager::trackedCount，
///    更新顶部 5 个状态标签；
/// 3. 收集堆对象：遍历操作数栈与全局变量中的指针型 Value，用裸 Value* 指向
///    原对象（避免拷贝使 refCount 虚高），按指针地址去重并升序排序，
///    填充堆对象表——地址列用 ptrToHex、类型列用 heapStructName、refCount
///    列用 useCount()、字段/元素数列用 heapFieldCount。
/// 该表展示的正是运行时「值/引用/GC 状态」：每个堆对象的类型、引用计数与大小，
/// 随 VM 单步推进实时变化。
void MemoryModelPanel::refreshAnimState() {
    if (!animStatusLabel_)
        return;

    // R113 C 项：刷新 GC 进度统计栏（不依赖 VM 状态，单例始终可读）
    // ARCH-10: 通过 MemoryInspectionAPI 一次性获取 GC 统计快照，消除对 GcManager 的直接依赖。
    auto refreshGcStats = [this]() {
        if (!animGcStatsLabel_)
            return;
        GcStatsSnapshot stats = MemoryInspectionAPI::getGcStats();
        QString phaseText = QString::fromUtf8("Idle");
        switch (stats.phase) {
        case GcPhase::Marking:
            phaseText = QString::fromUtf8("🟢 Marking");
            break;
        case GcPhase::Sweeping:
            phaseText = QString::fromUtf8("🧹 Sweeping");
            break;
        case GcPhase::Finalizing:
            phaseText = QString::fromUtf8("🔄 Finalizing");
            break;
        case GcPhase::Idle:
        default:
            phaseText = QString::fromUtf8("💤 Idle");
            break;
        }
        animGcStatsLabel_->setText(QString::fromUtf8("GC 阶段：%1 | 上次标记 %2, 回收 %3 | 累计 %4 次")
                                       .arg(phaseText)
                                       .arg(stats.lastMarkedCount)
                                       .arg(stats.lastCollectedCount)
                                       .arg(stats.totalGcCount));
    };
    refreshGcStats();

    // ---- 未绑定 controller 时显示占位 ----
    if (!controller_) {
        animStatusLabel_->setText(QString::fromUtf8("VM 状态：未绑定"));
        animOpCodeLabel_->setText(QString::fromUtf8("OpCode：—"));
        animStackDepthLabel_->setText(QString::fromUtf8("栈深度：—"));
        animFrameCountLabel_->setText(QString::fromUtf8("栈帧数：—"));
        animGcTrackedLabel_->setText(QString::fromUtf8("GC tracked：—"));
        if (heapObjectTable_)
            heapObjectTable_->setRowCount(0);
        return;
    }

    // ---- VM 未初始化时显示占位 ----
    if (!controller_->isVmInitialized()) {
        animStatusLabel_->setText(QString::fromUtf8("VM 状态：未初始化"));
        animOpCodeLabel_->setText(QString::fromUtf8("OpCode：—"));
        animStackDepthLabel_->setText(QString::fromUtf8("栈深度：—"));
        animFrameCountLabel_->setText(QString::fromUtf8("栈帧数：—"));
        animGcTrackedLabel_->setText(QString::fromUtf8("GC tracked：—"));
        if (heapObjectTable_)
            heapObjectTable_->setRowCount(0);
        return;
    }

    // ---- VM 状态：运行中 / 已暂停 ----
    QString status = controller_->isVmRunning() ? QString::fromUtf8("运行中") : QString::fromUtf8("已暂停");
    animStatusLabel_->setText(QString::fromUtf8("VM 状态：%1").arg(status));

    // AUDIT-P1 fix: VM 运行期间并发读 frames_/globalSlots_ 触发 UB。
    // 对齐 CallStackPanel/VariableInspectorPanel 的 isVmRunning() 守卫模式：
    // 运行态仅显示状态文本，不读 VM 内部状态，暂停后才刷新。
    if (controller_->isVmRunning()) {
        animOpCodeLabel_->setText(QString::fromUtf8("OpCode：运行中"));
        animStackDepthLabel_->setText(QString::fromUtf8("栈深度：运行中（暂停后刷新）"));
        animFrameCountLabel_->setText(QString::fromUtf8("栈帧数：—"));
        animGcTrackedLabel_->setText(QString::fromUtf8("GC tracked：—"));
        if (heapObjectTable_)
            heapObjectTable_->setRowCount(0);
        return;
    }

    // ---- 当前 OpCode 名 ----
    std::string opName = controller_->getVmCurrentOpCodeName();
    animOpCodeLabel_->setText(QString::fromUtf8("OpCode：%1")
                                  .arg(opName.empty() ? QString::fromUtf8("—") : QString::fromUtf8(opName.c_str())));

    // ---- 操作数栈深度（getVmStack 返回 by-value 拷贝，下方继续用于堆对象提取）----
    auto stack = controller_->getVmStack();
    animStackDepthLabel_->setText(QString::fromUtf8("栈深度：%1").arg(stack.size()));

    // ---- 栈帧数 ----
    size_t frameCount = controller_->getVmFrameCount();
    animFrameCountLabel_->setText(QString::fromUtf8("栈帧数：%1").arg(frameCount));

    // ---- GC tracked 节点数 ----
    // ARCH-10: 通过 MemoryInspectionAPI 间接访问 GcManager
    size_t tracked = MemoryInspectionAPI::getGcStats().trackedCount;
    animGcTrackedLabel_->setText(QString::fromUtf8("GC tracked：%1").arg(tracked));

    // ---- 收集堆对象：遍历 stack + globals 中的 Value，按地址去重 ----
    // 注意：使用裸 Value* 指向 stack/globals 中的元素，避免 Value 拷贝导致
    // useCount() 被自身临时引用膨胀（保持显示的 refCount 为真实值）。
    auto globals = controller_->getVmGlobals();

    std::vector<Value> heapValueCopies;
    heapValueCopies.reserve(stack.size() + globals.size());
    for (const auto& v : stack) {
        if (v.isPointer() && v.asPointer())
            heapValueCopies.push_back(v);
    }
    for (const auto& kv : globals) {
        if (kv.second.isPointer() && kv.second.asPointer())
            heapValueCopies.push_back(kv.second);
    }

    // 按地址去重（保留首次出现的 Value 引用）
    std::unordered_set<const void*> seenAddrs;
    std::vector<Value> uniqueValues;
    uniqueValues.reserve(heapValueCopies.size());
    for (const Value& v : heapValueCopies) {
        const void* addr = static_cast<const void*>(v.asPointer());
        if (seenAddrs.insert(addr).second) {
            uniqueValues.push_back(v);
        }
    }

    // 按地址升序排序
    std::sort(uniqueValues.begin(), uniqueValues.end(), [](const Value& a, const Value& b) {
        return static_cast<const void*>(a.asPointer()) < static_cast<const void*>(b.asPointer());
    });

    // ---- 填充堆对象表 ----
    if (!heapObjectTable_)
        return;
    // R113 B 项：保留当前选中行号，refreshAnimState 重建表后会自动恢复选中并刷新详情。
    int prevSelectedRow = heapObjectTable_->currentRow();
    // setCurrentItem(nullptr) 防止 setRowCount 时 currentItemChanged 误触发到旧行号
    heapObjectTable_->setCurrentItem(nullptr);
    heapObjectTable_->setRowCount((int)uniqueValues.size());
    // R113 B 项：保存当前堆对象表对应的 Value* 列表，供 onHeapObjectSelected 取用。
    // 注意：这些 Value* 指向 controller_ 返回的 stack/globals 副本，仅在本次
    // refreshAnimState 调用内有效；下次刷新会整体替换 currentHeapValues_。
    currentHeapValues_ = std::move(uniqueValues);
    for (int i = 0; i < (int)currentHeapValues_.size(); ++i) {
        const Value& vp = currentHeapValues_[i];
        // ARCH-10: 通过 MemoryInspectionAPI 一次性获取堆对象元信息快照，
        // 消除对 RefCounted* 的直接访问（地址/类型/refCount/字段数全部由快照提供）。
        HeapObjectSnapshot snap = MemoryInspectionAPI::inspectHeap(vp);

        // 地址列
        heapObjectTable_->setItem(i, 0, new QTableWidgetItem(QString::fromUtf8(snap.addressHex.c_str())));

        // 类型列
        heapObjectTable_->setItem(i, 1, new QTableWidgetItem(QString::fromUtf8(snap.structName.c_str())));

        // refCount 列
        auto* rcItem = new QTableWidgetItem(QString::number(snap.refCount));
        rcItem->setTextAlignment(Qt::AlignCenter);
        heapObjectTable_->setItem(i, 2, rcItem);

        // 字段数/元素数列
        heapObjectTable_->setItem(i, 3,
                                  new QTableWidgetItem(QString::number(static_cast<qulonglong>(snap.fieldCount))));
    }

    // R113 B 项：恢复选中行并刷新详情浏览器。若 prevSelectedRow 仍在新表范围内，
    // 复用同一行号；否则清空详情（避免显示已失效的 Value*）。
    // 注意：selectRow 不会可靠触发 currentItemChanged（Qt 仅在 selection 改变时发信号，
    // 若新旧行号相同则不发），故手动调用 onHeapObjectSelected 刷新详情浏览器。
    if (prevSelectedRow >= 0 && prevSelectedRow < (int)currentHeapValues_.size()) {
        heapObjectTable_->selectRow(prevSelectedRow);
        onHeapObjectSelected(prevSelectedRow);
    } else if (heapObjectDetailBrowser_) {
        heapObjectDetailBrowser_->clear();
    }
}

// ============================================================
// R113 B 项：堆对象详情（ClosureData upvalue 生命周期增强）
// ============================================================

/// 堆对象表行选中回调：根据行号从 currentHeapValues_ 取出 Value*，
/// 调用 renderHeapObjectDetail 渲染详情。无效行号或空指针时清空详情区。
void MemoryModelPanel::onHeapObjectSelected(int row) {
    if (!heapObjectDetailBrowser_)
        return;
    if (row < 0 || row >= (int)currentHeapValues_.size()) {
        heapObjectDetailBrowser_->clear();
        return;
    }
    const Value& v = currentHeapValues_[row];
    renderHeapObjectDetail(v);
}

/// 按 ValueType 分发渲染堆对象详情到 heapObjectDetailBrowser_。
/// ClosureData 走 upvalue 生命周期详情（重点增强）；其他堆类型给出
/// 简短结构摘要（地址/类型/refCount/关键字段），保持 UI 一致性。
void MemoryModelPanel::renderHeapObjectDetail(const Value& v) {
    if (!heapObjectDetailBrowser_)
        return;
    std::ostringstream os;
    if (!v.isPointer() || !v.asPointer()) {
        os << "<p style='color:#888;'>该值不是堆对象（标量类型：int/float/bool/null 内联存储于 NaNBox）。</p>";
        heapObjectDetailBrowser_->setHtml(QString::fromUtf8(os.str().c_str()));
        return;
    }
    // ARCH-10: 通过 MemoryInspectionAPI 获取堆对象元信息快照（地址/类型/refCount），
    // 消除对 RefCounted* 的直接访问。后续 switch 仍用 v.getType()（Value 公开 API）分发。
    HeapObjectSnapshot snap = MemoryInspectionAPI::inspectHeap(v);
    os << "<h3>📦 " << snap.structName << " 详情</h3>";
    os << "<p><b>地址：</b><code>" << snap.addressHex << "</code></p>";
    os << "<p><b>refCount：</b>" << snap.refCount << "</p>";

    switch (v.getType()) {
    case ValueType::VAL_CLOSURE: {
        // R113 B 项核心：ClosureData upvalue 生命周期详情
        os << "<h4>闭包元数据</h4>";
        os << "<p><b>函数名：</b>" << QString::fromUtf8(v.closureName().c_str()).toHtmlEscaped().toStdString()
           << "</p>";
        const auto& params = v.closureParams();
        if (params.empty()) {
            os << "<p><b>参数列表：</b><i>(无参数)</i></p>";
        } else {
            os << "<p><b>参数列表：</b>(";
            for (size_t i = 0; i < params.size(); ++i) {
                if (i > 0)
                    os << ", ";
                os << QString::fromUtf8(params[i].c_str()).toHtmlEscaped().toStdString();
            }
            os << ")</p>";
        }

        // ---- 捕获变量（解释器路径 capturedVars）----
        const auto& captured = v.capturedVars();
        os << "<h4>📋 捕获变量（capturedVars, 解释器路径）</h4>";
        if (captured.empty()) {
            os << "<p><i>无捕获变量</i></p>";
        } else {
            os << "<table border='1' cellpadding='4' cellspacing='0' style='border-collapse:collapse;'>";
            os << "<tr><th>变量名</th><th>值</th></tr>";
            for (const auto& kv : captured) {
                os << "<tr><td><code>" << QString::fromUtf8(kv.first.c_str()).toHtmlEscaped().toStdString()
                   << "</code></td><td><code>"
                   << QString::fromUtf8(kv.second.toString().c_str()).toHtmlEscaped().toStdString()
                   << "</code></td></tr>";
            }
            os << "</table>";
        }

        // ---- VM 闭包 upvalue 生命周期（栈式 VM 路径）----
        const auto& vmc = v.vmClosure();
        os << "<h4>🔁 VM upvalue 生命周期（栈式 VM 路径）</h4>";
        if (!vmc) {
            os << "<p><i>该闭包未绑定 VM 闭包数据（vmClosure 为 nullptr，"
               << "可能由解释器路径创建而未走 OP_CLOSURE 指令）</i></p>";
        } else {
            os << "<p><b>VM functionName：</b>"
               << QString::fromUtf8(vmc->functionName.c_str()).toHtmlEscaped().toStdString() << "</p>";
            os << "<p><b>upvalue 数量：</b>" << vmc->upvalues.size() << "</p>";
            if (vmc->chunkPtr) {
                os << "<p><b>chunkPtr：</b><code>" << ptrToHex(vmc->chunkPtr) << "</code></p>";
            } else {
                os << "<p><b>chunkPtr：</b><i>nullptr</i></p>";
            }

            if (!vmc->upvalues.empty()) {
                os << "<table border='1' cellpadding='4' cellspacing='0' style='border-collapse:collapse;'>";
                os << "<tr><th>#</th><th>状态</th><th>stackSlot</th><th>owningFrameIdx</th><th>值</th></tr>";
                for (size_t i = 0; i < vmc->upvalues.size(); ++i) {
                    const auto& uv = vmc->upvalues[i];
                    os << "<tr>";
                    os << "<td>" << i << "</td>";
                    if (uv->isClosed) {
                        os << "<td style='color:#2A7A2A;font-weight:bold;'>✅ closed</td>";
                    } else {
                        os << "<td style='color:#C07030;font-weight:bold;'>🟢 open</td>";
                    }
                    os << "<td>" << uv->stackSlot << "</td>";
                    os << "<td>" << uv->owningFrameIdx << "</td>";
                    os << "<td><code>" << QString::fromUtf8(uv->value.toString().c_str()).toHtmlEscaped().toStdString()
                       << "</code></td>";
                    os << "</tr>";
                }
                os << "</table>";
                os << "<p style='color:#888;font-size:9pt;'>"
                   << "说明：<b>open</b> 表示外层栈帧尚未返回，upvalue 直接指向栈槽（stackSlot/owningFrameIdx 有效，"
                   << "value 字段为占位）；<b>closed</b> 表示外层栈帧已返回，upvalue 已将栈槽值拷贝到自身 value 字段"
                   << "（stackSlot/owningFrameIdx 仅作历史记录）。"
                   << "单步执行闭包调用时可观察 upvalue 从 open → closed 的迁移过程。"
                   << "</p>";
            }
        }
        break;
    }
    case ValueType::VAL_ARRAY: {
        const auto& arr = v.arrayVal();
        os << "<h4>元素（" << arr.size() << " 项）</h4>";
        if (arr.size() > 20) {
            os << "<p><i>元素数较多，仅展示前 20 项</i></p>";
        }
        size_t showN = std::min<size_t>(arr.size(), 20);
        os << "<ul>";
        for (size_t i = 0; i < showN; ++i) {
            os << "<li>[" << i << "] <code>"
               << QString::fromUtf8(arr[i].toString().c_str()).toHtmlEscaped().toStdString() << "</code></li>";
        }
        os << "</ul>";
        break;
    }
    case ValueType::VAL_DICT: {
        const auto& dict = v.dictVal();
        os << "<h4>键值对（" << dict.size() << " 项）</h4>";
        if (dict.size() > 20) {
            os << "<p><i>键值对较多，仅展示前 20 项</i></p>";
        }
        size_t cnt = 0;
        os << "<table border='1' cellpadding='4' cellspacing='0' style='border-collapse:collapse;'>";
        os << "<tr><th>键</th><th>值</th></tr>";
        for (const auto& kv : dict) {
            if (cnt >= 20)
                break;
            os << "<tr><td><code>"
               << QString::fromUtf8(Value::dictKeyToString(kv.first).c_str()).toHtmlEscaped().toStdString()
               << "</code></td><td><code>"
               << QString::fromUtf8(kv.second.toString().c_str()).toHtmlEscaped().toStdString() << "</code></td></tr>";
            ++cnt;
        }
        os << "</table>";
        break;
    }
    case ValueType::VAL_INSTANCE: {
        const auto& fields = v.fields();
        os << "<h4>实例字段（" << fields.size() << " 项）</h4>";
        os << "<table border='1' cellpadding='4' cellspacing='0' style='border-collapse:collapse;'>";
        os << "<tr><th>字段名</th><th>值</th></tr>";
        for (const auto& kv : fields) {
            os << "<tr><td><code>" << QString::fromUtf8(kv.first.c_str()).toHtmlEscaped().toStdString()
               << "</code></td><td><code>"
               << QString::fromUtf8(kv.second.toString().c_str()).toHtmlEscaped().toStdString() << "</code></td></tr>";
        }
        os << "</table>";
        break;
    }
    case ValueType::VAL_STRING: {
        const std::string& s = v.stringVal();
        os << "<h4>字符串内容</h4>";
        os << "<p><code>\"" << QString::fromUtf8(s.c_str()).toHtmlEscaped().toStdString() << "\"</code></p>";
        os << "<p><b>UTF-8 码位数：</b>" << v.codepointCount() << "</p>";
        break;
    }
    case ValueType::VAL_INT: {
        os << "<h4>装箱整数值</h4>";
        os << "<p><code>" << v.toString() << "</code>（超出 int48 范围，装箱为 BoxedIntData）</p>";
        break;
    }
    default:
        os << "<p><i>该堆类型暂无详情展示</i></p>";
        break;
    }

    heapObjectDetailBrowser_->setHtml(QString::fromUtf8(os.str().c_str()));
}

// ============================================================
// R113 A 项：第 5 子页 RegisterVM 寄存器帧可视化
// ============================================================

/// 构建「RegisterVM 寄存器帧」子页：顶部 VM 模式状态 + 当前帧信息 +
/// 中部 QSplitter（左调用栈表 / 右当前帧寄存器表） + 底部教学提示与刷新按钮。
/// 栈式 VM 模式下页面显示提示并禁用刷新，引导用户切换到 RegisterVM 模式。
void MemoryModelPanel::buildRegVmPage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    // ---- 顶部：VM 模式 + 当前帧信息 ----
    regVmModeLabel_ = new QLabel(QString::fromUtf8("VM 模式：—"), host);
    // UX-R2 fix: VM 模式标签样式迁移到 TeachingTheme 状态色系（替代硬编码 #F5F5F5/#CCC）
    regVmModeLabel_->setStyleSheet(
        QStringLiteral("QLabel { background-color: %1; padding: 6px 10px; border: 1px solid %2; "
                       "border-radius: 3px; font-weight: bold; }")
            .arg(TeachingTheme::statusBg().name(), TeachingTheme::statusBorder().name()));
    layout->addWidget(regVmModeLabel_);

    regVmFrameInfoLabel_ = new QLabel(QString::fromUtf8("当前帧：— | ip — | 帧索引 — | 激活寄存器 —"), host);
    // UX-R2 fix: 帧信息标签样式迁移到 TeachingTheme 信息卡片色系（替代硬编码 #EEF6FF/#88B0E0/#1A3A6A）
    regVmFrameInfoLabel_->setStyleSheet(
        QStringLiteral("QLabel { background-color: %1; padding: 6px 10px; border: 1px solid %2; "
                       "border-radius: 3px; color: %3; }")
            .arg(TeachingTheme::infoBg().name(), TeachingTheme::infoBorder().name(), TeachingTheme::infoText().name()));
    layout->addWidget(regVmFrameInfoLabel_);

    // ---- 中部 QSplitter：左调用栈 / 右寄存器表 ----
    auto* splitter = new QSplitter(Qt::Horizontal, host);
    splitter->setChildrenCollapsible(false);
    layout->addWidget(splitter, 2);

    // 左：调用栈表（depth / functionName / line / locals 数）
    regVmCallStackTable_ = new QTableWidget(0, 4, host);
    regVmCallStackTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    regVmCallStackTable_->setHorizontalHeaderLabels({
        QString::fromUtf8("depth"),
        QString::fromUtf8("函数名"),
        QString::fromUtf8("行"),
        QString::fromUtf8("局部变量数"),
    });
    regVmCallStackTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    regVmCallStackTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    regVmCallStackTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    regVmCallStackTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    splitter->addWidget(regVmCallStackTable_);

    // 右：当前帧寄存器表（编号 / 类型 / 值 / 地址）
    regVmRegisterTable_ = new QTableWidget(0, 4, host);
    regVmRegisterTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    regVmRegisterTable_->setHorizontalHeaderLabels({
        QString::fromUtf8("寄存器#"),
        QString::fromUtf8("类型"),
        QString::fromUtf8("值"),
        QString::fromUtf8("堆地址"),
    });
    regVmRegisterTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    regVmRegisterTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    regVmRegisterTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    regVmRegisterTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    splitter->addWidget(regVmRegisterTable_);

    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);
    splitter->setSizes({300, 600});

    // ---- 教学提示 ----
    regVmHintLabel_ =
        new QLabel(QString::fromUtf8("💡 RegisterVM 使用 32 个虚拟寄存器（std::array<Value, 32>）的栈帧布局，"
                                     "零堆分配。表中显示当前栈顶帧激活的寄存器窗口（按 registerCount 截取）。"
                                     "切换到 RegisterVM 模式（顶部工具栏）后单步执行可观察寄存器值随指令变化。"),
                   host);
    regVmHintLabel_->setWordWrap(true);
    // UX-R2 fix: 教学提示标签样式迁移到 TeachingTheme 警告卡片色系（替代硬编码 #FFF8E0/#E0C070/#5A4500）
    regVmHintLabel_->setStyleSheet(QStringLiteral("QLabel { background-color: %1; padding: 8px; border: 1px solid %2; "
                                                  "border-radius: 3px; color: %3; }")
                                       .arg(TeachingTheme::warningBg().name(), TeachingTheme::warningBorder().name(),
                                            TeachingTheme::warningText().name()));
    layout->addWidget(regVmHintLabel_);

    // ---- 底部按钮 ----
    auto* btnbar = new QHBoxLayout;
    regVmRefreshBtn_ = new QPushButton(QString::fromUtf8("刷新"), host);
    regVmAutoRefreshBtn_ = new QPushButton(QString::fromUtf8("自动刷新（2s）"), host);
    regVmAutoRefreshBtn_->setCheckable(true);
    btnbar->addWidget(regVmRefreshBtn_);
    btnbar->addWidget(regVmAutoRefreshBtn_);
    btnbar->addStretch();
    layout->addLayout(btnbar);

    // R113 A 项: 自动刷新复用 animTimer_（第 4 子页已创建），两按钮状态联动。
    // 启停时调用 animTimer_ 的 start/stop，与第 4 子页共用 2s 间隔。
    connect(regVmRefreshBtn_, &QPushButton::clicked, this, [this]() { refreshRegVmState(); });
    connect(regVmAutoRefreshBtn_, &QPushButton::clicked, this, [this]() {
        if (regVmAutoRefreshBtn_->isChecked()) {
            if (animTimer_ && !animTimer_->isActive())
                animTimer_->start();
            regVmAutoRefreshBtn_->setText(QString::fromUtf8("停止自动刷新"));
            refreshRegVmState();
        } else {
            // 若第 4 子页未开启自动刷新，则停止 animTimer_；否则保持运行
            if (animTimer_ && animTimer_->isActive() && !(animAutoRefreshBtn_ && animAutoRefreshBtn_->isChecked())) {
                animTimer_->stop();
            }
            regVmAutoRefreshBtn_->setText(QString::fromUtf8("自动刷新（2s）"));
        }
    });
}

/// 刷新第 5 子页状态：
/// 1. 未绑定 controller / VM 未初始化 / 栈式 VM 模式时显示占位并清空表格；
/// 2. RegisterVM 模式下：
///    - 模式标签显示 "RegisterVM"；
///    - 当前帧信息标签显示 函数名/ip/帧索引/激活寄存器数；
///    - 调用栈表填充 getVmCallStack() 返回的帧列表（depth/functionName/line/locals 数）；
///    - 寄存器表填充 getVmStack() 返回的当前帧寄存器窗口（编号/类型/值/堆地址）。
/// AUDIT-P1 fix: VM 运行期间并发读 frames_/registers_ 触发 UB，
/// 与第 4 子页一致地用 isVmRunning() 守卫——运行态仅显示状态文本。
void MemoryModelPanel::refreshRegVmState() {
    if (!regVmModeLabel_)
        return;

    // ---- 未绑定 controller 时显示占位 ----
    if (!controller_) {
        regVmModeLabel_->setText(QString::fromUtf8("VM 模式：未绑定"));
        regVmFrameInfoLabel_->setText(QString::fromUtf8("当前帧：— | ip — | 帧索引 — | 激活寄存器 —"));
        if (regVmCallStackTable_)
            regVmCallStackTable_->setRowCount(0);
        if (regVmRegisterTable_)
            regVmRegisterTable_->setRowCount(0);
        return;
    }

    // ---- VM 未初始化时显示占位 ----
    if (!controller_->isVmInitialized()) {
        regVmModeLabel_->setText(QString::fromUtf8("VM 模式：未初始化"));
        regVmFrameInfoLabel_->setText(QString::fromUtf8("当前帧：— | ip — | 帧索引 — | 激活寄存器 —"));
        if (regVmCallStackTable_)
            regVmCallStackTable_->setRowCount(0);
        if (regVmRegisterTable_)
            regVmRegisterTable_->setRowCount(0);
        return;
    }

    // ---- 栈式 VM 模式：提示用户切换 ----
    if (!controller_->isVmRegisterMode()) {
        regVmModeLabel_->setText(QString::fromUtf8("VM 模式：栈式 VM（此子页仅 RegisterVM 模式可用）"));
        regVmFrameInfoLabel_->setText(
            QString::fromUtf8("当前帧：— | ip — | 帧索引 — | 激活寄存器 —（请切换到 RegisterVM 模式）"));
        if (regVmCallStackTable_)
            regVmCallStackTable_->setRowCount(0);
        if (regVmRegisterTable_)
            regVmRegisterTable_->setRowCount(0);
        return;
    }

    // ---- RegisterVM 模式：填充数据 ----
    regVmModeLabel_->setText(QString::fromUtf8("VM 模式：🟢 RegisterVM"));

    // AUDIT-P1 fix: VM 运行期间并发读 frames_/registers_ 触发 UB，
    // 与第 4 子页一致地用 isVmRunning() 守卫。
    if (controller_->isVmRunning()) {
        regVmFrameInfoLabel_->setText(
            QString::fromUtf8("当前帧：运行中（暂停后刷新） | ip — | 帧索引 — | 激活寄存器 —"));
        if (regVmCallStackTable_)
            regVmCallStackTable_->setRowCount(0);
        if (regVmRegisterTable_)
            regVmRegisterTable_->setRowCount(0);
        return;
    }

    // ---- 当前帧信息 ----
    size_t ip = controller_->getVmCurrentIP();
    size_t frameCount = controller_->getVmFrameCount();
    size_t currentFrameIdx = frameCount > 0 ? frameCount - 1 : 0;
    // 当前帧函数名从 getVmCallStack() 取栈顶帧
    auto callStack = controller_->getVmCallStack();
    QString curFuncName =
        callStack.empty() ? QString::fromUtf8("—") : QString::fromUtf8(callStack.back().functionName.c_str());
    int curLine = callStack.empty() ? -1 : callStack.back().line;

    // ---- 寄存器窗口（RegisterVM 模式下 getVmStack 返回当前帧寄存器）----
    auto registers = controller_->getVmStack();
    regVmFrameInfoLabel_->setText(QString::fromUtf8("当前帧：%1 | 行 %2 | ip %3 | 帧索引 %4 | 激活寄存器 %5")
                                      .arg(curFuncName)
                                      .arg(curLine)
                                      .arg(ip)
                                      .arg(currentFrameIdx)
                                      .arg(registers.size()));

    // ---- 填充调用栈表 ----
    if (regVmCallStackTable_) {
        regVmCallStackTable_->setRowCount((int)callStack.size());
        for (int i = 0; i < (int)callStack.size(); ++i) {
            const auto& entry = callStack[i];
            // depth 从 0 开始（栈底为 0），与帧索引对应
            auto* dItem = new QTableWidgetItem(QString::number(i));
            dItem->setTextAlignment(Qt::AlignCenter);
            regVmCallStackTable_->setItem(i, 0, dItem);
            regVmCallStackTable_->setItem(i, 1, new QTableWidgetItem(QString::fromUtf8(entry.functionName.c_str())));
            auto* lItem = new QTableWidgetItem(QString::number(entry.line));
            lItem->setTextAlignment(Qt::AlignCenter);
            regVmCallStackTable_->setItem(i, 2, lItem);
            auto* vItem = new QTableWidgetItem(QString::number((qulonglong)entry.locals.size()));
            vItem->setTextAlignment(Qt::AlignCenter);
            regVmCallStackTable_->setItem(i, 3, vItem);
        }
    }

    // ---- 填充寄存器表 ----
    if (regVmRegisterTable_) {
        regVmRegisterTable_->setRowCount((int)registers.size());
        for (int i = 0; i < (int)registers.size(); ++i) {
            const Value& v = registers[i];
            // 寄存器编号
            auto* nItem = new QTableWidgetItem(QString::fromUtf8("r%1").arg(i));
            nItem->setTextAlignment(Qt::AlignCenter);
            regVmRegisterTable_->setItem(i, 0, nItem);
            // 类型（标量显示 NaNBox tag 名，堆对象显示结构名）
            std::string typeName;
            if (v.isPointer() && v.asPointer()) {
                typeName = heapStructName(v);
            } else {
                // 标量类型：从 ValueType 推断
                switch (v.getType()) {
                case ValueType::VAL_INT:
                    typeName = "int";
                    break;
                case ValueType::VAL_FLOAT:
                    typeName = "float";
                    break;
                case ValueType::VAL_BOOL:
                    typeName = "bool";
                    break;
                case ValueType::VAL_NULL:
                    typeName = "null";
                    break;
                default:
                    typeName = "<unknown>";
                    break;
                }
            }
            regVmRegisterTable_->setItem(i, 1, new QTableWidgetItem(QString::fromUtf8(typeName.c_str())));
            // 值
            regVmRegisterTable_->setItem(i, 2, new QTableWidgetItem(QString::fromUtf8(v.toString().c_str())));
            // 堆地址（仅堆对象显示）
            QString addrText = QString::fromUtf8("—");
            if (v.isPointer() && v.asPointer()) {
                addrText = QString::fromUtf8(ptrToHex(v.asPointer()).c_str());
            }
            auto* aItem = new QTableWidgetItem(addrText);
            aItem->setTextAlignment(Qt::AlignCenter);
            regVmRegisterTable_->setItem(i, 3, aItem);
        }
    }
}
