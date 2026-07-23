// ============================================================
// AstVisualizerPanel.cpp — AST 可视化编辑器教学面板实现
// ------------------------------------------------------------
// 实现 2 子页：源码 → AST 可视化 / AST 节点参考。
// 子页 1 面板内部独立完成 Lexer → Parser → AST 流程，
// 递归遍历 AST 节点的 nodeName() / children() 接口构建
// QTreeWidget 树形展示，点击节点时通过 dynamic_cast 提取
// 节点属性生成 HTML 详情。
// ============================================================

#include "gui/AstVisualizerPanel.h"

#include "ast/ASTNode.h"
#include "common/Diagnostic.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QSplitter>
#include <QVBoxLayout>

// ============================================================
// AstVisualizerLibrary — 内置示例代码库（4 个）
// ============================================================

/// 返回 4 个内置示例代码条目（标题 + 源码），覆盖变量声明、函数、数组、类。
const std::vector<std::pair<std::string, std::string>>& AstVisualizerLibrary::samples() {
    static const std::vector<std::pair<std::string, std::string>> kSamples = {
        {"变量声明 + 条件语句", "var x = 10; if (x > 5) { print(x); }"},
        {"函数定义 + 调用", "fun add(a, b) { return a + b; } print(add(3, 4));"},
        {"数组 + for-in 循环", "var arr = [1, 2, 3]; for (item in arr) { print(item); }"},
        {"类定义 + 实例化", "class Point { x: int; y: int; } var p = Point(1, 2);"},
    };
    return kSamples;
}

// ============================================================
// AstVisualizerLibrary — AST 节点类型文档库（约 25 条）
// ============================================================
// 文档分类与 ASTNode.h 实际类名对应：
//   声明类：FunDecl / VarDecl / ClassDecl / EnumDecl
//   语句类：IfStmt / WhileStmt / ForStmt / ReturnStmt / BreakStmt /
//           ContinueStmt / Block / PrintStmt / TryStmt / ThrowStmt /
//           ImportStmt / ExportStmt
//   表达式类：NumberLiteral / StringLiteral / BoolLiteral / NullLiteral /
//             BinaryOp / UnaryOp / FunCall / MemberAccess / MethodCall /
//             IndexAccess / IndexAssign / Assignment / VarRef /
//             ArrayLiteral / DictLiteral
// ============================================================

const std::vector<AstNodeInfo>& AstVisualizerLibrary::nodeDocs() {
    static const std::vector<AstNodeInfo> kDocs = {
        // ---- 声明类 ----
        {"FunDecl", "声明类", "函数声明节点。声明一个命名函数，含参数列表、类型注解、函数体。",
         "<li>name: 函数名</li><li>params: 参数名列表</li><li>paramTypes: 参数类型注解</li>"
         "<li>returnType: 返回值类型注解</li><li>body: 函数体 (Block)</li>"
         "<li>defaultValues: 默认参数值列表</li>",
         "fun add(a, b) { return a + b; }"},
        {"VarDecl", "声明类", "变量声明节点。声明一个变量并绑定初始值，可带类型注解。",
         "<li>name: 变量名</li><li>typeAnnotation: 类型注解（如 \"int\"，空表示动态类型）</li>"
         "<li>initializer: 初始值表达式</li>",
         "var x = 10;"},
        {"ClassDecl", "声明类", "类声明节点。声明一个类，可继承父类，包含字段与方法成员。",
         "<li>name: 类名</li><li>superClassName: 父类名（空表示无继承）</li>"
         "<li>members: 成员列表（FieldDecl / FunDecl）</li>",
         "class Point { x: int; y: int; }"},
        {"EnumDecl", "声明类",
         "枚举声明节点。声明一个 sealed enum 类型，含 0+ 类型参数和 1+ variant。"
         "支持简单枚举与 ADT（代数数据类型）。",
         "<li>name: enum 类型名</li><li>typeParams: 类型参数列表</li>"
         "<li>variants: variant 列表（每个含 name 与 paramTypes）</li>",
         "enum Option<T> { Some(T), None }"},
        // ---- 语句类 ----
        {"IfStmt", "语句类", "if 语句节点。条件分支：condition 为真执行 thenBranch，否则执行 elseBranch（若有）。",
         "<li>condition: 条件表达式</li><li>thenBranch: then 分支</li>"
         "<li>elseBranch: else 分支（可为 nullptr）</li>",
         "if (x > 5) { print(x); }"},
        {"WhileStmt", "语句类", "while 循环节点。每次迭代前求值 condition，为真执行 body。",
         "<li>condition: 循环条件表达式</li><li>body: 循环体</li>", "while (i < 10) { i = i + 1; }"},
        {"ForStmt", "语句类",
         "for 循环节点。MiniLang 支持 C 风格 for 与 for-in 两种形式。"
         "for-in 时 initializer 为 VarDecl，condition 为 VarRef（迭代变量）。",
         "<li>initializer: 初始化语句（可为 nullptr）</li>"
         "<li>condition: 循环条件（可为 nullptr）</li>"
         "<li>update: 更新表达式（可为 nullptr）</li><li>body: 循环体</li>",
         "for (item in arr) { print(item); }"},
        {"ReturnStmt", "语句类", "return 语句节点。从当前函数返回并携带返回值。",
         "<li>value: 返回值表达式（可为 nullptr，表示 return;）</li>", "return x + 1;"},
        {"BreakStmt", "语句类", "break 语句节点。跳出最近的 while/for 循环。", "<li>无属性</li>",
         "while (true) { break; }"},
        {"ContinueStmt", "语句类", "continue 语句节点。跳到最近 while/for 循环的下一次迭代。", "<li>无属性</li>",
         "while (true) { continue; }"},
        {"Block", "语句类", "代码块节点。包含一组顺序执行的语句序列，构成作用域单元。",
         "<li>statements: 语句列表</li><li>closingBraceLine: 闭合 '}' 行号</li>", "{ var x = 1; print(x); }"},
        {"PrintStmt", "语句类", "print 语句节点。输出一到多个值到标准输出（空格分隔）。",
         "<li>values: 待输出的值表达式列表</li>", "print(\"hello\", 42, true);"},
        {"TryStmt", "语句类", "try-catch-finally 语句节点。捕获 try 块中的运行时异常。",
         "<li>tryBlock: try 块</li><li>catchVarName: catch 绑定的变量名</li>"
         "<li>catchBlock: catch 块</li><li>finallyBlock: finally 块（可选）</li>",
         "try { foo(); } catch (e) { print(e); }"},
        {"ThrowStmt", "语句类", "throw 语句节点。主动抛出一个异常值。", "<li>expression: 抛出的值表达式</li>",
         "throw \"error\";"},
        {"ImportStmt", "语句类", "import 语句节点。从指定模块路径导入名称。",
         "<li>modulePath: 模块路径</li><li>names: 导入的名称列表</li>"
         "<li>importAll: 是否导入全部（import *）</li>",
         "import { foo } from \"utils\";"},
        {"ExportStmt", "语句类", "export 语句节点。导出一个声明供其他模块导入。",
         "<li>declaration: 被导出的声明节点（VarDecl / FunDecl / ClassDecl）</li>",
         "export fun add(a, b) { return a + b; }"},
        // ---- 表达式类 ----
        {"NumberLiteral", "表达式类", "数字字面量节点。支持整数（int64_t）与浮点数（double）两种形式。",
         "<li>isFloat_: 是否为浮点</li><li>intValue_: 整数值</li><li>floatValue_: 浮点值</li>", "42  或  3.14"},
        {"StringLiteral", "表达式类", "字符串字面量节点。表示一个字符串常量。", "<li>value: 字符串值</li>",
         "\"hello\""},
        {"BoolLiteral", "表达式类", "布尔字面量节点。表示 true 或 false。", "<li>value: 布尔值</li>",
         "true  或  false"},
        {"NullLiteral", "表达式类", "null 字面量节点。表示空值。", "<li>无属性</li>", "null"},
        {"BinaryOp", "表达式类",
         "二元运算节点。表示左右操作数与一个运算符的组合，如算术、比较、逻辑运算。"
         "运算符优先级由 Parser 递归下降结构隐式编码。",
         "<li>opType: 运算符枚举（BIN_ADD/BIN_SUB/BIN_LT 等）</li>"
         "<li>left: 左操作数</li><li>right: 右操作数</li>",
         "x + y  或  a > b  或  m and n"},
        {"UnaryOp", "表达式类", "一元运算节点。表示单操作数运算，如取负（-）与逻辑非（not）。",
         "<li>opType: 运算符类型（UOP_NEGATE / UOP_NOT / UOP_PLUS）</li>"
         "<li>operand: 操作数</li>",
         "-x  或  not flag"},
        {"FunCall", "表达式类", "函数调用节点。按名调用或链式调用（callee 非空时为表达式调用）。",
         "<li>name: 被调用函数名（链式调用时为空）</li>"
         "<li>callee: 被调用的表达式（链式调用时非空）</li>"
         "<li>arguments: 实参列表</li>",
         "add(3, 4)  或  getFn()(x)"},
        {"MemberAccess", "表达式类", "成员访问节点。访问对象的字段属性，如 obj.field。",
         "<li>object: 对象表达式</li><li>fieldName: 字段名</li>", "p.x"},
        {"MethodCall", "表达式类", "方法调用节点。调用对象的方法，如 obj.method(args)。",
         "<li>object: 对象表达式</li><li>methodName: 方法名</li>"
         "<li>arguments: 实参列表</li>",
         "arr.push(99);"},
        {"IndexAccess", "表达式类", "索引访问节点。通过索引访问数组元素或字典值，如 arr[i] 或 dict[key]。",
         "<li>object: 容器表达式</li><li>index: 索引表达式</li>", "arr[0]  或  d[\"key\"]"},
        {"IndexAssign", "表达式类", "索引赋值节点。通过索引写入数组元素或字典值，如 arr[i] = v。",
         "<li>object: 容器表达式</li><li>index: 索引表达式</li><li>value: 赋值表达式</li>", "arr[0] = 99;"},
        {"Assignment", "表达式类", "赋值节点。将右侧表达式的值赋给左侧变量。",
         "<li>name: 被赋值的变量名</li><li>value: 赋值表达式</li>", "x = 10;"},
        {"VarRef", "表达式类", "变量引用节点。读取一个已绑定的变量值。", "<li>name: 变量名</li>", "print(x);  中的  x"},
        {"ArrayLiteral", "表达式类", "数组字面量节点。构建一个数组，元素按顺序求值。",
         "<li>elements: 元素表达式列表</li>", "[1, 2, 3]"},
        {"DictLiteral", "表达式类", "字典字面量节点。构建一个字典，含若干键值对。",
         "<li>pairs: 键值对列表（每对为 key + value 表达式）</li>", "{\"x\": 1, \"y\": 2}"},
    };
    return kDocs;
}

// ============================================================
// 匿名命名空间：辅助函数
// ============================================================

namespace {

/// 格式化 DiagnosticBag 中的错误条目为 HTML（已转义）
QString formatDiagnosticErrors(const DiagnosticBag& bag) {
    QString result;
    for (const auto& d : bag.all()) {
        if (d.isError()) {
            result += QString::fromUtf8(d.format().c_str()).toHtmlEscaped() + QStringLiteral("<br>");
        }
    }
    if (result.isEmpty()) {
        result = QStringLiteral("（未知错误）");
    }
    return result;
}

/// HTML 转义辅助
QString esc(const std::string& s) {
    return QString::fromUtf8(s.c_str()).toHtmlEscaped();
}

} // anonymous namespace

// ============================================================
// AstVisualizerPanel 实现
// ============================================================

AstVisualizerPanel::AstVisualizerPanel(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);
    outer->setSpacing(4);

    // 子页切换按钮栏
    auto* pageBar = new QHBoxLayout;
    pageVisualizeBtn_ = new QPushButton(tr("源码 → AST 可视化"));
    pageReferenceBtn_ = new QPushButton(tr("AST 节点参考"));
    pageVisualizeBtn_->setCheckable(true);
    pageReferenceBtn_->setCheckable(true);
    pageVisualizeBtn_->setChecked(true);
    pageBar->addWidget(pageVisualizeBtn_);
    pageBar->addWidget(pageReferenceBtn_);
    pageBar->addStretch();
    outer->addLayout(pageBar);

    stack_ = new QStackedWidget;
    auto* visualizePage = new QWidget;
    auto* referencePage = new QWidget;
    buildVisualizePage(visualizePage);
    buildReferencePage(referencePage);
    stack_->addWidget(visualizePage);
    stack_->addWidget(referencePage);
    outer->addWidget(stack_, 1);

    connect(pageVisualizeBtn_, &QPushButton::clicked, [this]() {
        stack_->setCurrentIndex(0);
        pageReferenceBtn_->setChecked(false);
    });
    connect(pageReferenceBtn_, &QPushButton::clicked, [this]() {
        stack_->setCurrentIndex(1);
        pageVisualizeBtn_->setChecked(false);
    });

    // 预填示例代码（首个样例）
    sourceEdit_->setText(QString::fromUtf8(AstVisualizerLibrary::samples()[0].second.c_str()));

    populateNodeDocs();

    // 占位提示
    nodeDetail_->setHtml(QString::fromUtf8("<div style='color:#6E6E6E; padding:8px;'><i>（点击「解析并生成 AST」后，"
                                           "选中左侧树节点查看该节点的详细信息）</i></div>"));
}

AstVisualizerPanel::~AstVisualizerPanel() = default;

/// 构建「源码 → AST 可视化」子页：顶部源码输入 + 解析按钮 + 样例切换，
/// 中间水平 splitter（左侧 AST 树 + 右侧节点详情），底部加载样例按钮。
void AstVisualizerPanel::buildVisualizePage(QWidget* host) {
    auto* v = new QVBoxLayout(host);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);

    // 顶部：源码输入 + 解析按钮
    auto* topBar = new QHBoxLayout;
    topBar->addWidget(new QLabel(tr("源码：")));
    sourceEdit_ = new QLineEdit;
    sourceEdit_->setPlaceholderText(tr("输入 MiniLang 源码后点击解析"));
    parseBtn_ = new QPushButton(tr("解析并生成 AST"));
    topBar->addWidget(sourceEdit_, 1);
    topBar->addWidget(parseBtn_);
    v->addLayout(topBar);

    // 样例切换按钮栏
    const auto& samples = AstVisualizerLibrary::samples();
    auto* sampleBar = new QHBoxLayout;
    sampleBar->addWidget(new QLabel(tr("内置样例：")));
    sample1Btn_ = new QPushButton(QString::fromUtf8("样例1: ") + QString::fromUtf8(samples[0].first.c_str()));
    sample2Btn_ = new QPushButton(QString::fromUtf8("样例2: ") + QString::fromUtf8(samples[1].first.c_str()));
    sample3Btn_ = new QPushButton(QString::fromUtf8("样例3: ") + QString::fromUtf8(samples[2].first.c_str()));
    sample4Btn_ = new QPushButton(QString::fromUtf8("样例4: ") + QString::fromUtf8(samples[3].first.c_str()));
    sampleBar->addWidget(sample1Btn_);
    sampleBar->addWidget(sample2Btn_);
    sampleBar->addWidget(sample3Btn_);
    sampleBar->addWidget(sample4Btn_);
    sampleBar->addStretch();
    v->addLayout(sampleBar);

    // 中间：水平 splitter（AST 树 + 节点详情）
    auto* splitter = new QSplitter(Qt::Horizontal);
    astTree_ = new QTreeWidget;
    astTree_->setHeaderLabels({tr("AST 节点"), tr("行:列")});
    astTree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    astTree_->setSelectionBehavior(QAbstractItemView::SelectRows);
    astTree_->setSelectionMode(QAbstractItemView::SingleSelection);
    astTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    astTree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    splitter->addWidget(astTree_);

    nodeDetail_ = new QTextBrowser;
    nodeDetail_->setOpenExternalLinks(false);
    splitter->addWidget(nodeDetail_);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);
    v->addWidget(splitter, 1);

    // 底部：加载样例按钮
    auto* btnBar = new QHBoxLayout;
    btnBar->addStretch();
    loadSampleBtn_ = new QPushButton(tr("加载样例代码到主编辑器"));
    btnBar->addWidget(loadSampleBtn_);
    v->addLayout(btnBar);

    // 信号连接
    connect(parseBtn_, &QPushButton::clicked, this, &AstVisualizerPanel::onParse);
    connect(astTree_, &QTreeWidget::itemClicked, this, &AstVisualizerPanel::onAstNodeSelected);
    connect(loadSampleBtn_, &QPushButton::clicked, this, &AstVisualizerPanel::onLoadSample);
    connect(sample1Btn_, &QPushButton::clicked, [this]() { onSelectSample(0); });
    connect(sample2Btn_, &QPushButton::clicked, [this]() { onSelectSample(1); });
    connect(sample3Btn_, &QPushButton::clicked, [this]() { onSelectSample(2); });
    connect(sample4Btn_, &QPushButton::clicked, [this]() { onSelectSample(3); });
}

/// 构建「AST 节点参考」子页：水平 splitter（左侧节点类型列表 + 右侧文档详情）。
void AstVisualizerPanel::buildReferencePage(QWidget* host) {
    auto* v = new QVBoxLayout(host);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);

    auto* splitter = new QSplitter(Qt::Horizontal);
    nodeList_ = new QListWidget;
    nodeDocDetail_ = new QTextBrowser;
    nodeDocDetail_->setOpenExternalLinks(false);
    splitter->addWidget(nodeList_);
    splitter->addWidget(nodeDocDetail_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);
    v->addWidget(splitter, 1);

    connect(nodeList_, &QListWidget::currentRowChanged, this, &AstVisualizerPanel::onNodeDocSelected);
}

/// 解析源码：面板内独立完成 Lexer → Parser → AST，递归构建 QTreeWidget 树。
/// 解析失败时在详情区显示错误。
void AstVisualizerPanel::onParse() {
    astTree_->clear();
    itemNodeMap_.clear();
    astRoot_.reset();
    nodeDetail_->clear();

    std::string source = sourceEdit_->text().toStdString();
    if (source.empty()) {
        nodeDetail_->setHtml(QString::fromUtf8("<div style='color:#C0392B; padding:8px;'><b>请输入源码</b></div>"));
        return;
    }

    // Lexer → Parser → AST
    Lexer lex;
    std::vector<Token> tokens;
    try {
        tokens = lex.scan(source);
    } catch (const std::exception& e) {
        nodeDetail_->setHtml(QString::fromUtf8("<div style='color:#C0392B; padding:8px;'><b>词法错误:</b> %1</div>")
                                 .arg(QString::fromUtf8(e.what()).toHtmlEscaped()));
        return;
    }
    if (lex.getDiagnostics().hasErrors()) {
        nodeDetail_->setHtml(QString::fromUtf8("<div style='color:#C0392B; padding:8px;'><b>词法错误:</b> %1</div>")
                                 .arg(formatDiagnosticErrors(lex.getDiagnostics())));
        return;
    }

    Parser parser;
    std::unique_ptr<Block> ast;
    try {
        ast = parser.parse(tokens);
    } catch (const std::exception& e) {
        nodeDetail_->setHtml(QString::fromUtf8("<div style='color:#C0392B; padding:8px;'><b>语法错误:</b> %1</div>")
                                 .arg(QString::fromUtf8(e.what()).toHtmlEscaped()));
        return;
    }
    if (parser.hasErrors()) {
        nodeDetail_->setHtml(QString::fromUtf8("<div style='color:#C0392B; padding:8px;'><b>语法错误:</b> %1</div>")
                                 .arg(formatDiagnosticErrors(parser.getDiagnostics())));
        return;
    }

    // 转移所有权给 astRoot_（Block* → ASTNode*）
    astRoot_ = std::move(ast);
    populateAstTree(astRoot_.get());

    if (astTree_->topLevelItemCount() > 0) {
        astTree_->topLevelItem(0)->setSelected(true);
        onAstNodeSelected(astTree_->topLevelItem(0), 0);
    } else {
        nodeDetail_->setHtml(
            QString::fromUtf8("<div style='color:#6E6E6E; padding:8px;'><i>（解析产物为空）</i></div>"));
    }
}

/// 递归构建 AST 树：将 ASTNode 树映射为 QTreeWidgetItem 树。
void AstVisualizerPanel::populateAstTree(ASTNode* root) {
    if (!root) {
        return;
    }
    auto* rootItem = buildTreeItem(root);
    astTree_->addTopLevelItem(rootItem);
    astTree_->expandAll();
}

/// 递归构造 QTreeWidgetItem：节点名 + 行:列摘要，子节点按 AST 结构嵌套。
QTreeWidgetItem* AstVisualizerPanel::buildTreeItem(ASTNode* node) {
    if (!node) {
        return nullptr;
    }
    QString name = QString::fromStdString(node->nodeName());
    QString pos = QStringLiteral("%1:%2").arg(node->line).arg(node->column);
    auto* item = new QTreeWidgetItem({name, pos});
    itemNodeMap_.emplace_back(item, node);

    for (ASTNode* child : node->children()) {
        if (child) {
            item->addChild(buildTreeItem(child));
        }
    }
    return item;
}

/// 点击 AST 树节点时，在右侧详情区显示该节点的 HTML 详情。
void AstVisualizerPanel::onAstNodeSelected(QTreeWidgetItem* item, int /*column*/) {
    if (!item) {
        return;
    }
    // 查找对应的 ASTNode 裸指针
    ASTNode* node = nullptr;
    for (const auto& pair : itemNodeMap_) {
        if (pair.first == item) {
            node = pair.second;
            break;
        }
    }
    if (!node) {
        nodeDetail_->clear();
        return;
    }
    nodeDetail_->setHtml(buildNodeDetailHtml(node));
}

/// 构造节点详情 HTML：节点类型 + 行号/列号 + 属性列表 + 子节点列表。
QString AstVisualizerPanel::buildNodeDetailHtml(ASTNode* node) {
    QString html;
    html += QStringLiteral("<h3>%1</h3>").arg(QString::fromStdString(node->nodeName()).toHtmlEscaped());
    html += QStringLiteral("<p><b>行号:</b> %1 | <b>列号:</b> %2</p>").arg(node->line).arg(node->column);

    // 属性列表
    QStringList props = extractNodeProperties(node);
    html += QStringLiteral("<h4>属性:</h4>");
    if (props.isEmpty()) {
        html += QStringLiteral("<p><i>（无属性或未知节点类型）</i></p>");
    } else {
        html += QStringLiteral("<ul>");
        for (const QString& p : props) {
            html += QStringLiteral("<li>") + p + QStringLiteral("</li>");
        }
        html += QStringLiteral("</ul>");
    }

    // 子节点列表
    QStringList children = extractChildDescriptions(node);
    html += QStringLiteral("<h4>子节点:</h4>");
    if (children.isEmpty()) {
        html += QStringLiteral("<p><i>（无子节点）</i></p>");
    } else {
        html += QStringLiteral("<ul>");
        for (const QString& c : children) {
            html += QStringLiteral("<li>") + c + QStringLiteral("</li>");
        }
        html += QStringLiteral("</ul>");
    }
    return html;
}

/// 通过 dynamic_cast 提取节点属性（key: value 形式）。
/// 仅处理常见节点类型，对不支持的类型返回空列表。
QStringList AstVisualizerPanel::extractNodeProperties(ASTNode* node) {
    QStringList props;
    if (!node) {
        return props;
    }
    // ---- 声明类 ----
    if (auto* v = dynamic_cast<VarDecl*>(node)) {
        props << QStringLiteral("name: %1").arg(esc(v->name));
        props << QStringLiteral("typeAnnotation: %1")
                     .arg(v->typeAnnotation.empty() ? QStringLiteral("(none)") : esc(v->typeAnnotation));
        props << QStringLiteral("hasInitializer: %1")
                     .arg(v->initializer ? QStringLiteral("true") : QStringLiteral("false"));
    } else if (auto* f = dynamic_cast<FunDecl*>(node)) {
        props << QStringLiteral("name: %1").arg(esc(f->name));
        QString params;
        for (size_t i = 0; i < f->params.size(); ++i) {
            if (i > 0)
                params += QStringLiteral(", ");
            params += QString::fromStdString(f->params[i]);
            if (i < f->paramTypes.size() && !f->paramTypes[i].empty()) {
                params += QStringLiteral(": ") + QString::fromStdString(f->paramTypes[i]);
            }
        }
        props << QStringLiteral("params: [%1]").arg(params);
        props << QStringLiteral("returnType: %1")
                     .arg(f->returnType.empty() ? QStringLiteral("(none)") : esc(f->returnType));
        props << QStringLiteral("requiredParamCount: %1").arg(f->requiredParamCount);
    } else if (auto* c = dynamic_cast<ClassDecl*>(node)) {
        props << QStringLiteral("name: %1").arg(esc(c->name));
        props << QStringLiteral("superClassName: %1")
                     .arg(c->superClassName.empty() ? QStringLiteral("(none)") : esc(c->superClassName));
        props << QStringLiteral("memberCount: %1").arg(c->members.size());
    } else if (auto* e = dynamic_cast<EnumDecl*>(node)) {
        props << QStringLiteral("name: %1").arg(esc(e->name));
        QString tparams;
        for (size_t i = 0; i < e->typeParams.size(); ++i) {
            if (i > 0)
                tparams += QStringLiteral(", ");
            tparams += QString::fromStdString(e->typeParams[i]);
        }
        props << QStringLiteral("typeParams: [%1]").arg(tparams);
        props << QStringLiteral("variantCount: %1").arg(e->variants.size());
        // ---- 语句类 ----
    } else if (dynamic_cast<IfStmt*>(node)) {
        props << QStringLiteral("hasElseBranch: %1")
                     .arg(static_cast<IfStmt*>(node)->elseBranch ? QStringLiteral("true") : QStringLiteral("false"));
    } else if (dynamic_cast<WhileStmt*>(node)) {
        // WhileStmt 无额外属性（condition/body 已在子节点列表）
    } else if (dynamic_cast<ForStmt*>(node)) {
        auto* f = static_cast<ForStmt*>(node);
        props << QStringLiteral("hasInitializer: %1")
                     .arg(f->initializer ? QStringLiteral("true") : QStringLiteral("false"));
        props
            << QStringLiteral("hasCondition: %1").arg(f->condition ? QStringLiteral("true") : QStringLiteral("false"));
        props << QStringLiteral("hasUpdate: %1").arg(f->update ? QStringLiteral("true") : QStringLiteral("false"));
    } else if (auto* r = dynamic_cast<ReturnStmt*>(node)) {
        props << QStringLiteral("hasValue: %1").arg(r->value ? QStringLiteral("true") : QStringLiteral("false"));
    } else if (dynamic_cast<BreakStmt*>(node)) {
        // 无属性
    } else if (dynamic_cast<ContinueStmt*>(node)) {
        // 无属性
    } else if (auto* b = dynamic_cast<Block*>(node)) {
        props << QStringLiteral("statementCount: %1").arg(b->statements.size());
    } else if (auto* p = dynamic_cast<PrintStmt*>(node)) {
        props << QStringLiteral("valueCount: %1").arg(p->values.size());
    } else if (auto* t = dynamic_cast<TryStmt*>(node)) {
        props << QStringLiteral("catchVarName: %1").arg(esc(t->catchVarName));
        props << QStringLiteral("hasFinallyBlock: %1")
                     .arg(t->finallyBlock ? QStringLiteral("true") : QStringLiteral("false"));
    } else if (auto* t = dynamic_cast<ThrowStmt*>(node)) {
        props << QStringLiteral("hasExpression: %1")
                     .arg(t->expression ? QStringLiteral("true") : QStringLiteral("false"));
    } else if (auto* i = dynamic_cast<ImportStmt*>(node)) {
        props << QStringLiteral("modulePath: %1").arg(esc(i->modulePath));
        props << QStringLiteral("importAll: %1").arg(i->importAll ? QStringLiteral("true") : QStringLiteral("false"));
        props << QStringLiteral("nameCount: %1").arg(i->names.size());
    } else if (auto* e = dynamic_cast<ExportStmt*>(node)) {
        props << QStringLiteral("hasDeclaration: %1")
                     .arg(e->declaration ? QStringLiteral("true") : QStringLiteral("false"));
        // ---- 表达式类 ----
    } else if (auto* n = dynamic_cast<NumberLiteral*>(node)) {
        if (n->isFloat_) {
            props << QStringLiteral("value: %1 (float)").arg(n->floatValue_);
        } else {
            props << QStringLiteral("value: %1 (int)").arg(n->intValue_);
        }
    } else if (auto* s = dynamic_cast<StringLiteral*>(node)) {
        props << QStringLiteral("value: \"%1\"").arg(esc(s->value));
    } else if (auto* b = dynamic_cast<BoolLiteral*>(node)) {
        props << QStringLiteral("value: %1").arg(b->value ? QStringLiteral("true") : QStringLiteral("false"));
    } else if (dynamic_cast<NullLiteral*>(node)) {
        props << QStringLiteral("value: null");
    } else if (auto* b = dynamic_cast<BinaryOp*>(node)) {
        props << QStringLiteral("opType: %1").arg(QString::fromUtf8(BinaryOp::opTypeStr(b->opType)));
        props << QStringLiteral("precedence: %1").arg(BinaryOp::precedence(b->opType));
    } else if (auto* u = dynamic_cast<UnaryOp*>(node)) {
        props << QStringLiteral("opType: %1").arg(QString::fromUtf8(UnaryOp::opTypeStr(u->opType)));
    } else if (auto* f = dynamic_cast<FunCall*>(node)) {
        if (f->callee) {
            props << QStringLiteral("callMode: 链式调用（callee 非空）");
        } else {
            props << QStringLiteral("name: %1").arg(esc(f->name));
        }
        props << QStringLiteral("argumentCount: %1").arg(f->arguments.size());
    } else if (auto* m = dynamic_cast<MemberAccess*>(node)) {
        props << QStringLiteral("fieldName: %1").arg(esc(m->fieldName));
    } else if (auto* m = dynamic_cast<MethodCall*>(node)) {
        props << QStringLiteral("methodName: %1").arg(esc(m->methodName));
        props << QStringLiteral("argumentCount: %1").arg(m->arguments.size());
    } else if (dynamic_cast<IndexAccess*>(node)) {
        // 无额外属性（object/index 在子节点列表）
    } else if (dynamic_cast<IndexAssign*>(node)) {
        // 无额外属性
    } else if (auto* a = dynamic_cast<Assignment*>(node)) {
        props << QStringLiteral("name: %1").arg(esc(a->name));
    } else if (auto* v = dynamic_cast<VarRef*>(node)) {
        props << QStringLiteral("name: %1").arg(esc(v->name));
    } else if (auto* a = dynamic_cast<ArrayLiteral*>(node)) {
        props << QStringLiteral("elementCount: %1").arg(a->elements.size());
    } else if (auto* d = dynamic_cast<DictLiteral*>(node)) {
        props << QStringLiteral("pairCount: %1").arg(d->pairs.size());
    } else if (auto* i = dynamic_cast<InterpolatedString*>(node)) {
        props << QStringLiteral("literalCount: %1").arg(i->literals.size());
        props << QStringLiteral("expressionCount: %1").arg(i->expressions.size());
    } else if (auto* d = dynamic_cast<DestructureBinding*>(node)) {
        QString names;
        for (size_t i = 0; i < d->names.size(); ++i) {
            if (i > 0)
                names += QStringLiteral(", ");
            names += QString::fromStdString(d->names[i]);
        }
        props << QStringLiteral("names: [%1]").arg(names);
    } else if (auto* e = dynamic_cast<EnumVariantExpr*>(node)) {
        props << QStringLiteral("enumName: %1").arg(esc(e->enumName));
        props << QStringLiteral("variantName: %1").arg(esc(e->variantName));
        props << QStringLiteral("argumentCount: %1").arg(e->arguments.size());
    } else if (auto* m = dynamic_cast<MatchExpr*>(node)) {
        props << QStringLiteral("caseCount: %1").arg(m->cases.size());
    } else if (auto* t = dynamic_cast<TupleLiteral*>(node)) {
        props << QStringLiteral("elementCount: %1").arg(t->elements.size());
    } else if (dynamic_cast<SuperExpr*>(node)) {
        props << QStringLiteral("note: 引用父类方法/字段");
    } else {
        // 未知节点类型：不显示属性（详情区已显示「无属性或未知节点类型」）
    }
    return props;
}

/// 提取子节点描述列表（使用 ASTNode::children() 接口）。
QStringList AstVisualizerPanel::extractChildDescriptions(ASTNode* node) {
    QStringList childrenDesc;
    if (!node) {
        return childrenDesc;
    }
    // 通过 dynamic_cast 给子节点附上角色标签（如 VarDecl 的 initializer、IfStmt 的
    // condition/thenBranch/elseBranch），便于学习者理解每个子节点的语义角色。
    if (auto* v = dynamic_cast<VarDecl*>(node)) {
        if (v->initializer)
            childrenDesc << QStringLiteral("initializer: %1")
                                .arg(QString::fromStdString(v->initializer->nodeName()).toHtmlEscaped());
    } else if (auto* f = dynamic_cast<FunDecl*>(node)) {
        if (f->body)
            childrenDesc << QStringLiteral("body: %1").arg(QString::fromStdString(f->body->nodeName()).toHtmlEscaped());
        for (size_t i = 0; i < f->defaultValues.size(); ++i) {
            if (f->defaultValues[i]) {
                childrenDesc << QStringLiteral("defaultValue[%1]: %2")
                                    .arg(i)
                                    .arg(QString::fromStdString(f->defaultValues[i]->nodeName()).toHtmlEscaped());
            }
        }
    } else if (auto* c = dynamic_cast<ClassDecl*>(node)) {
        for (size_t i = 0; i < c->members.size(); ++i) {
            if (c->members[i]) {
                childrenDesc << QStringLiteral("member[%1]: %2")
                                    .arg(i)
                                    .arg(QString::fromStdString(c->members[i]->nodeName()).toHtmlEscaped());
            }
        }
    } else if (auto* i = dynamic_cast<IfStmt*>(node)) {
        if (i->condition)
            childrenDesc << QStringLiteral("condition: %1")
                                .arg(QString::fromStdString(i->condition->nodeName()).toHtmlEscaped());
        if (i->thenBranch)
            childrenDesc << QStringLiteral("thenBranch: %1")
                                .arg(QString::fromStdString(i->thenBranch->nodeName()).toHtmlEscaped());
        if (i->elseBranch)
            childrenDesc << QStringLiteral("elseBranch: %1")
                                .arg(QString::fromStdString(i->elseBranch->nodeName()).toHtmlEscaped());
    } else if (auto* w = dynamic_cast<WhileStmt*>(node)) {
        if (w->condition)
            childrenDesc << QStringLiteral("condition: %1")
                                .arg(QString::fromStdString(w->condition->nodeName()).toHtmlEscaped());
        if (w->body)
            childrenDesc << QStringLiteral("body: %1").arg(QString::fromStdString(w->body->nodeName()).toHtmlEscaped());
    } else if (auto* f = dynamic_cast<ForStmt*>(node)) {
        if (f->initializer)
            childrenDesc << QStringLiteral("initializer: %1")
                                .arg(QString::fromStdString(f->initializer->nodeName()).toHtmlEscaped());
        if (f->condition)
            childrenDesc << QStringLiteral("condition: %1")
                                .arg(QString::fromStdString(f->condition->nodeName()).toHtmlEscaped());
        if (f->update)
            childrenDesc
                << QStringLiteral("update: %1").arg(QString::fromStdString(f->update->nodeName()).toHtmlEscaped());
        if (f->body)
            childrenDesc << QStringLiteral("body: %1").arg(QString::fromStdString(f->body->nodeName()).toHtmlEscaped());
    } else if (auto* r = dynamic_cast<ReturnStmt*>(node)) {
        if (r->value)
            childrenDesc
                << QStringLiteral("value: %1").arg(QString::fromStdString(r->value->nodeName()).toHtmlEscaped());
    } else if (auto* b = dynamic_cast<BinaryOp*>(node)) {
        if (b->left)
            childrenDesc << QStringLiteral("left: %1").arg(QString::fromStdString(b->left->nodeName()).toHtmlEscaped());
        if (b->right)
            childrenDesc
                << QStringLiteral("right: %1").arg(QString::fromStdString(b->right->nodeName()).toHtmlEscaped());
    } else if (auto* u = dynamic_cast<UnaryOp*>(node)) {
        if (u->operand)
            childrenDesc
                << QStringLiteral("operand: %1").arg(QString::fromStdString(u->operand->nodeName()).toHtmlEscaped());
    } else if (auto* f = dynamic_cast<FunCall*>(node)) {
        if (f->callee)
            childrenDesc
                << QStringLiteral("callee: %1").arg(QString::fromStdString(f->callee->nodeName()).toHtmlEscaped());
        for (size_t i = 0; i < f->arguments.size(); ++i) {
            if (f->arguments[i]) {
                childrenDesc << QStringLiteral("arg[%1]: %2")
                                    .arg(i)
                                    .arg(QString::fromStdString(f->arguments[i]->nodeName()).toHtmlEscaped());
            }
        }
    } else if (auto* m = dynamic_cast<MemberAccess*>(node)) {
        if (m->object)
            childrenDesc
                << QStringLiteral("object: %1").arg(QString::fromStdString(m->object->nodeName()).toHtmlEscaped());
    } else if (auto* m = dynamic_cast<MethodCall*>(node)) {
        if (m->object)
            childrenDesc
                << QStringLiteral("object: %1").arg(QString::fromStdString(m->object->nodeName()).toHtmlEscaped());
        for (size_t i = 0; i < m->arguments.size(); ++i) {
            if (m->arguments[i]) {
                childrenDesc << QStringLiteral("arg[%1]: %2")
                                    .arg(i)
                                    .arg(QString::fromStdString(m->arguments[i]->nodeName()).toHtmlEscaped());
            }
        }
    } else if (auto* i = dynamic_cast<IndexAccess*>(node)) {
        if (i->object)
            childrenDesc
                << QStringLiteral("object: %1").arg(QString::fromStdString(i->object->nodeName()).toHtmlEscaped());
        if (i->index)
            childrenDesc
                << QStringLiteral("index: %1").arg(QString::fromStdString(i->index->nodeName()).toHtmlEscaped());
    } else if (auto* i = dynamic_cast<IndexAssign*>(node)) {
        if (i->object)
            childrenDesc
                << QStringLiteral("object: %1").arg(QString::fromStdString(i->object->nodeName()).toHtmlEscaped());
        if (i->index)
            childrenDesc
                << QStringLiteral("index: %1").arg(QString::fromStdString(i->index->nodeName()).toHtmlEscaped());
        if (i->value)
            childrenDesc
                << QStringLiteral("value: %1").arg(QString::fromStdString(i->value->nodeName()).toHtmlEscaped());
    } else if (auto* a = dynamic_cast<Assignment*>(node)) {
        if (a->value)
            childrenDesc
                << QStringLiteral("value: %1").arg(QString::fromStdString(a->value->nodeName()).toHtmlEscaped());
    } else if (auto* m = dynamic_cast<MemberAssign*>(node)) {
        if (m->object)
            childrenDesc
                << QStringLiteral("object: %1").arg(QString::fromStdString(m->object->nodeName()).toHtmlEscaped());
        if (m->value)
            childrenDesc
                << QStringLiteral("value: %1").arg(QString::fromStdString(m->value->nodeName()).toHtmlEscaped());
    } else if (auto* b = dynamic_cast<Block*>(node)) {
        for (size_t i = 0; i < b->statements.size(); ++i) {
            if (b->statements[i]) {
                childrenDesc << QStringLiteral("stmt[%1]: %2")
                                    .arg(i)
                                    .arg(QString::fromStdString(b->statements[i]->nodeName()).toHtmlEscaped());
            }
        }
    } else if (auto* p = dynamic_cast<PrintStmt*>(node)) {
        for (size_t i = 0; i < p->values.size(); ++i) {
            if (p->values[i]) {
                childrenDesc << QStringLiteral("value[%1]: %2")
                                    .arg(i)
                                    .arg(QString::fromStdString(p->values[i]->nodeName()).toHtmlEscaped());
            }
        }
    } else if (auto* t = dynamic_cast<TryStmt*>(node)) {
        if (t->tryBlock)
            childrenDesc
                << QStringLiteral("tryBlock: %1").arg(QString::fromStdString(t->tryBlock->nodeName()).toHtmlEscaped());
        if (t->catchBlock)
            childrenDesc << QStringLiteral("catchBlock: %1")
                                .arg(QString::fromStdString(t->catchBlock->nodeName()).toHtmlEscaped());
        if (t->finallyBlock)
            childrenDesc << QStringLiteral("finallyBlock: %1")
                                .arg(QString::fromStdString(t->finallyBlock->nodeName()).toHtmlEscaped());
    } else if (auto* t = dynamic_cast<ThrowStmt*>(node)) {
        if (t->expression)
            childrenDesc << QStringLiteral("expression: %1")
                                .arg(QString::fromStdString(t->expression->nodeName()).toHtmlEscaped());
    } else if (auto* e = dynamic_cast<ExportStmt*>(node)) {
        if (e->declaration)
            childrenDesc << QStringLiteral("declaration: %1")
                                .arg(QString::fromStdString(e->declaration->nodeName()).toHtmlEscaped());
    } else if (auto* a = dynamic_cast<ArrayLiteral*>(node)) {
        for (size_t i = 0; i < a->elements.size(); ++i) {
            if (a->elements[i]) {
                childrenDesc << QStringLiteral("element[%1]: %2")
                                    .arg(i)
                                    .arg(QString::fromStdString(a->elements[i]->nodeName()).toHtmlEscaped());
            }
        }
    } else if (auto* d = dynamic_cast<DictLiteral*>(node)) {
        for (size_t i = 0; i < d->pairs.size(); ++i) {
            if (d->pairs[i].first) {
                childrenDesc << QStringLiteral("key[%1]: %2")
                                    .arg(i)
                                    .arg(QString::fromStdString(d->pairs[i].first->nodeName()).toHtmlEscaped());
            }
            if (d->pairs[i].second) {
                childrenDesc << QStringLiteral("value[%1]: %2")
                                    .arg(i)
                                    .arg(QString::fromStdString(d->pairs[i].second->nodeName()).toHtmlEscaped());
            }
        }
    } else if (auto* d = dynamic_cast<DestructureBinding*>(node)) {
        if (d->initializer)
            childrenDesc << QStringLiteral("initializer: %1")
                                .arg(QString::fromStdString(d->initializer->nodeName()).toHtmlEscaped());
    } else if (auto* e = dynamic_cast<EnumVariantExpr*>(node)) {
        for (size_t i = 0; i < e->arguments.size(); ++i) {
            if (e->arguments[i]) {
                childrenDesc << QStringLiteral("arg[%1]: %2")
                                    .arg(i)
                                    .arg(QString::fromStdString(e->arguments[i]->nodeName()).toHtmlEscaped());
            }
        }
    } else if (auto* m = dynamic_cast<MatchExpr*>(node)) {
        if (m->scrutinee)
            childrenDesc << QStringLiteral("scrutinee: %1")
                                .arg(QString::fromStdString(m->scrutinee->nodeName()).toHtmlEscaped());
        for (size_t i = 0; i < m->cases.size(); ++i) {
            if (m->cases[i].body)
                childrenDesc << QStringLiteral("case[%1].body: %2")
                                    .arg(i)
                                    .arg(QString::fromStdString(m->cases[i].body->nodeName()).toHtmlEscaped());
            if (m->cases[i].guard)
                childrenDesc << QStringLiteral("case[%1].guard: %2")
                                    .arg(i)
                                    .arg(QString::fromStdString(m->cases[i].guard->nodeName()).toHtmlEscaped());
        }
    } else if (auto* t = dynamic_cast<TupleLiteral*>(node)) {
        for (size_t i = 0; i < t->elements.size(); ++i) {
            if (t->elements[i]) {
                childrenDesc << QStringLiteral("element[%1]: %2")
                                    .arg(i)
                                    .arg(QString::fromStdString(t->elements[i]->nodeName()).toHtmlEscaped());
            }
        }
    } else {
        // 兜底：直接遍历 children() 接口，不附角色标签
        auto ch = node->children();
        for (size_t i = 0; i < ch.size(); ++i) {
            if (ch[i]) {
                childrenDesc << QStringLiteral("child[%1]: %2")
                                    .arg(i)
                                    .arg(QString::fromStdString(ch[i]->nodeName()).toHtmlEscaped());
            }
        }
    }
    return childrenDesc;
}

/// 用 AST 节点类型文档库填充「AST 节点参考」子页左侧列表，并默认选中首项。
void AstVisualizerPanel::populateNodeDocs() {
    nodeList_->clear();
    for (const auto& doc : AstVisualizerLibrary::nodeDocs()) {
        // 列表项显示：[分类] 类型名
        QString text = QStringLiteral("[%1] %2").arg(QString::fromUtf8(doc.category.c_str()),
                                                     QString::fromUtf8(doc.typeName.c_str()));
        nodeList_->addItem(text);
    }
    if (nodeList_->count() > 0) {
        nodeList_->setCurrentRow(0);
    }
}

/// 节点参考列表选中项变化时委托 showNodeDoc 渲染该类型的文档详情。
void AstVisualizerPanel::onNodeDocSelected(int index) {
    showNodeDoc(index);
}

/// 渲染节点类型文档：标题、分类、描述、常见属性与样例代码（HTML 转义）。
void AstVisualizerPanel::showNodeDoc(int index) {
    if (index < 0 || index >= static_cast<int>(AstVisualizerLibrary::nodeDocs().size())) {
        nodeDocDetail_->clear();
        return;
    }
    const auto& d = AstVisualizerLibrary::nodeDocs()[index];
    QString html = QString("<h2>%1</h2>"
                           "<p><b>分类:</b> %2</p>"
                           "<h3>节点描述</h3>"
                           "<p>%3</p>"
                           "<h3>常见属性</h3>"
                           "<ul>%4</ul>"
                           "<h3>样例代码</h3>"
                           "<pre>%5</pre>")
                       .arg(QString::fromUtf8(d.typeName.c_str()).toHtmlEscaped())
                       .arg(QString::fromUtf8(d.category.c_str()).toHtmlEscaped())
                       .arg(QString::fromUtf8(d.description.c_str()).toHtmlEscaped())
                       .arg(QString::fromUtf8(d.commonAttributes.c_str()))
                       .arg(QString::fromUtf8(d.exampleCode.c_str()).toHtmlEscaped());
    nodeDocDetail_->setHtml(html);
}

/// 将当前源码输入框的内容 emit loadSampleRequested，加载到主编辑器运行观察。
void AstVisualizerPanel::onLoadSample() {
    emit loadSampleRequested(sourceEdit_->text());
}

/// 切换内置样例：将样例代码填入源码输入框。
void AstVisualizerPanel::onSelectSample(int idx) {
    const auto& samples = AstVisualizerLibrary::samples();
    if (idx < 0 || idx >= static_cast<int>(samples.size())) {
        return;
    }
    sourceEdit_->setText(QString::fromUtf8(samples[idx].second.c_str()));
}
