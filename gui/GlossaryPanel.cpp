// ============================================================
// GlossaryPanel.cpp — 术语表面板实现
// ------------------------------------------------------------
// 集中展示 MiniLang IDE 核心术语，左侧字母序列表 + 顶部搜索框
// + 右侧 Markdown 详情。支持 term: 链接跨术语跳转。
//
// 复用：MarkdownRenderer（gui 命名空间）/ TeachingTheme /
// PanelAnimator，与 ExceptionFlowPanel.cpp 风格一致。
// ============================================================

#include "gui/GlossaryPanel.h"
#include "gui/PanelAnimator.h"
#include "gui/MarkdownRenderer.h"
#include "gui/TeachingTheme.h"
#include "gui/I18n.h"

#include <QVBoxLayout>
#include <QSplitter>
#include <QUrl>
#include <QHash>
#include <sstream>

// ============================================================
// GlossaryPanel 实现
// ============================================================

GlossaryPanel::GlossaryPanel(QWidget* parent) : QWidget(parent) {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(4);

    // 顶部搜索框
    searchEdit_ = new QLineEdit(this);
    searchEdit_->setPlaceholderText(mlTr("搜索术语..."));
    searchEdit_->setClearButtonEnabled(true);
    searchEdit_->setFixedHeight(28);
    searchEdit_->setStyleSheet(QString(
        "QLineEdit { border: 1px solid %1; border-radius: 4px;"
        "  padding: 2px 8px; background: %2; selection-background-color: %3; }"
        "QLineEdit:focus { border-color: %3; }"
    ).arg(TeachingTheme::border().name(),
          TeachingTheme::surface().name(),
          TeachingTheme::primary().name()));
    mainLayout->addWidget(searchEdit_);

    // 中间 splitter：左列表 + 右详情
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    listWidget_ = new QListWidget(splitter);
    listWidget_->setMinimumWidth(200);
    detailView_ = new QTextBrowser(splitter);
    detailView_->setOpenExternalLinks(false);
    detailView_->setPlaceholderText(mlTr("请在左侧选择一个术语以查看详情。"));
    splitter->addWidget(listWidget_);
    splitter->addWidget(detailView_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);
    splitter->setSizes({220, 580});
    mainLayout->addWidget(splitter, 1);

    // 加载静态术语数据
    loadEntries();

    // 填充列表（entries_ 已按 id 字母序排列）
    for (const auto& e : entries_) {
        listWidget_->addItem(e.term);
    }

    // 搜索框 textChanged → 过滤列表
    connect(searchEdit_, &QLineEdit::textChanged,
            this, &GlossaryPanel::onSearchChanged);

    // 列表选中 → 显示详情 + emit termActivated
    connect(listWidget_, &QListWidget::currentRowChanged,
            this, &GlossaryPanel::onTermSelected);

    // 双击 → 显式激活信号（用于跳转相关面板/ADR）
    connect(listWidget_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) {
        int row = listWidget_->currentRow();
        if (row >= 0 && row < entries_.size()) {
            emit termActivated(entries_[row].id);
        }
    });

    // 详情中 term: 链接 → 跳转到关联术语
    connect(detailView_, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
        QString full = url.toString();
        const QString prefix = "term:";
        if (!full.startsWith(prefix, Qt::CaseInsensitive)) return;
        QString id = full.mid(prefix.length());
        for (int i = 0; i < entries_.size(); ++i) {
            if (entries_[i].id == id) {
                // 清空搜索过滤，确保目标行可见（blockSignals 避免递归触发 populateList）
                if (!searchEdit_->text().isEmpty()) {
                    searchEdit_->blockSignals(true);
                    searchEdit_->clear();
                    searchEdit_->blockSignals(false);
                    populateList();
                }
                // 选中目标行（触发 currentRowChanged → onTermSelected）
                if (listWidget_->currentRow() != i) {
                    listWidget_->setCurrentRow(i);
                } else {
                    // 同一行已选中，手动刷新详情 + 发射激活信号
                    showDetail(i);
                    emit termActivated(entries_[i].id);
                }
                return;
            }
        }
    });

    // 默认选中第一个术语
    if (!entries_.isEmpty()) {
        listWidget_->setCurrentRow(0);
    }
}

// ============================================================
// loadEntries — 静态术语库（30 个核心术语，按 id 字母序排列）
// ============================================================
// 覆盖类别：
//   - 内存模型：nan-boxing / cow / refcounted / gc / mark-sweep
//   - 编译：lexer / token / parser / ast / ir / ir-module / ssa /
//           lowering
//   - 运行时：bytecode / bytecode-chunk / opstack / stack-frame /
//             stack-vm / register-vm / closure / closure-data /
//             upvalue / environment / bound-instance / field-slot /
//             module-system / short-circuit
//   - 类型系统：type-annotation / integer-division
//   - 测试：triple-backend
//
// 每条目含 id / term（中英对照）/ category / shortDef /
// fullDef（Markdown）/ related（关联术语 ID）。

void GlossaryPanel::loadEntries() {
    entries_ = {
        // ---- ast ----
        {
            "ast",
            "AST 抽象语法树",
            "编译",
            "Parser 吐出来的树：每个节点就是一个语法构造（表达式、语句、声明），谁包谁、谁先算一目了然。",
            "AST（Abstract Syntax Tree）就是源码被 Parser 消化后长出的那棵树。它不管你敲键盘的先后，只记一件事：哪个语法构造包着哪个、谁得先算。\n\n"
            "**💡 生活类比**：AST 像家谱图或文件夹树——它不记录你敲键盘的顺序，只记录「谁包含谁、谁先算谁」。同一个算式 `1+2*3` 和 `1+(2*3)` 在 AST 里形状不同，因为树表达的是计算优先级。\n\n"
            "- **表达式节点**：BinaryExpr / UnaryExpr / Literal / Identifier / Call\n"
            "- **语句节点**：VarDecl / FunDecl / ClassDecl / IfStmt / WhileStmt / ForStmt\n"
            "- **声明节点**：ImportStmt / ExportStmt / FunDecl / ClassDecl\n\n"
            "MiniLang 的 AST 节点继承自 `ASTNode` 基类，采用 intrusive 引用计数管理。三后端共享同一份 AST：Interpreter 直接遍历执行，Compiler/IRBuilder 将其翻译为字节码或 IR。\n\n"
            "> 注意：AST 节点结构对三后端一致性强约束——任何节点语义差异都会导致三后端结果不一致。",
            {"parser", "token", "ir", "triple-backend"}
        },
        // ---- bound-instance ----
        {
            "bound-instance",
            "boundInstance 绑定实例缓存",
            "运行时",
            "方法调用时，Environment 顺手把 this 指针记在小本本上，免得每次访问字段都回头找一遍。",
            "boundInstance_ 是 Environment 里的一个小抄：记下当前这个方法调用到底挂在哪个对象（this）上。执行 `obj.method()` 时，它就派上用场：\n\n"
            "1. 方法查找沿继承链进行，找到后创建新的 Environment 帧\n"
            "2. boundInstance_ 缓存 `obj` 指针，方法体内 `this` / `super` 直接引用\n"
            "3. 字段访问通过 fieldSlotIndex 索引到实例的字段数组\n\n"
            "**优化动机**：避免每次字段访问都沿 Environment 链查找实例。缓存命中后字段读写为 O(1)。\n\n"
            "**生命周期**：boundInstance_ 在方法帧 Environment 创建时设置，方法返回时随 Environment 销毁。",
            {"environment", "closure", "field-slot"}
        },
        // ---- bytecode ----
        {
            "bytecode",
            "bytecode 字节码",
            "运行时",
            "StackVM 照着念的「操作卡片」清单：每条指令 1 字节 OpCode 加一截操作数，紧凑得不行。",
            "bytecode 是 Compiler 把 AST 翻译出来的一叠超简指令，交给 StackVM 逐张照做。每条指令就是 1 字节 OpCode 加一截可变长度的操作数——短小，但够用。\n\n"
            "**💡 生活类比**：bytecode 是 MiniLang 的「目标代码」——像一组极简操作卡片（取 1、取 2、相加、打印），VM 照着一张张翻做。它比 AST 更接近机器，却仍运行在虚拟机上。\n\n"
            "**指令分类**（~60 OpCode）：\n\n"
            "| 类别 | 示例 | 说明 |\n"
            "|------|------|------|\n"
            "| 常量加载 | `OP_CONSTANT` | 推常量到操作数栈 |\n"
            "| 算术 | `OP_ADD` / `OP_SUBTRACT` | 弹出两个操作数计算后压回 |\n"
            "| 变量 | `OP_GET_LOCAL` / `OP_SET_LOCAL` | 局部变量槽位读写 |\n"
            "| 控制流 | `OP_JUMP` / `OP_JUMP_IF_FALSE` | 无条件/条件跳转 |\n"
            "| 闭包 | `OP_CLOSURE` / `OP_GET_UPVALUE` | 闭包创建与 upvalue 访问 |\n\n"
            "**特点**：\n"
            "- 紧凑：1 字节 OpCode + 1-3 字节操作数\n"
            "- 栈式语义：操作数隐式从栈顶取，结果压回栈顶\n"
            "- IR 路径与非 IR 路径共享 StackVM（中间表示不同：BytecodeChunk vs RegBytecodeChunk 仅寄存器后端用）",
            {"opstack", "bytecode-chunk", "stack-vm", "ir"}
        },
        // ---- bytecode-chunk ----
        {
            "bytecode-chunk",
            "BytecodeChunk 字节码块",
            "运行时",
            "Compiler 给每个函数或顶层作用域打的「字节码包裹」：指令、常量池、行号表、闭包信息，全装在一起。",
            "BytecodeChunk 是 Compiler 编译一个函数（或顶层作用域）时打包出来的字节码容器。它里头装着：\n\n"
            "- `code`：指令序列（`std::vector<uint8_t>`）\n"
            "- `constants`：常量池（`std::vector<Value>`，存字符串/数字字面量）\n"
            "- `lineInfo`：行号映射（指令偏移 → 源码行号，用于错误报告与调试）\n"
            "- `localCount`：局部变量数量\n"
            "- `upvalues`：upvalue 描述（用于闭包创建）\n\n"
            "**结构**：顶层 chunk 包含主程序字节码 + 函数 chunk 列表（嵌套函数各自一个 chunk）。\n\n"
            "**三后端关系**：\n"
            "- Interpreter 不使用 BytecodeChunk（直接遍历 AST）\n"
            "- StackVM（IR 路径与非 IR 路径）共用 BytecodeChunk\n"
            "- RegisterVM 使用 RegBytecodeChunk（寄存器式中间表示）",
            {"bytecode", "stack-vm", "closure-data", "ir-module"}
        },
        // ---- closure ----
        {
            "closure",
            "closure 闭包",
            "运行时",
            "会「随身带环境」的函数：即便跑到别处，也能回头够到定义它那时外面的变量。",
            "closure 就是一只攥着外层变量的函数。在 MiniLang 里，所有函数骨子里都是闭包——哪怕它啥也没捕获，也照样被 ClosureData 包了一层。\n\n"
            "**捕获机制**：\n"
            "- 函数定义时记录外层 Environment 中引用的变量为 upvalue\n"
            "- upvalue 分两种：open（变量仍在栈上）与 closed（变量已迁移到堆）\n"
            "- 函数返回时其栈帧销毁，捕获的局部变量自动 close 到堆\n\n"
            "**嵌套捕获**：支持 3+ 层嵌套——内层闭包可捕获外层闭包的 upvalue，形成捕获链。\n\n"
            "**已知高频 Bug 模式**：\n"
            "- 多层嵌套捕获顺序错误\n"
            "- 变量快照恢复遗漏\n"
            "- 闭包调用参数顺序\n"
            "- try/catch 中闭包未正确关闭",
            {"upvalue", "closure-data", "environment", "bound-instance"}
        },
        // ---- closure-data ----
        {
            "closure-data",
            "ClosureData 闭包数据",
            "运行时",
            "闭包干活时背的「背包」：装着函数字节码、要抓的 upvalue 列表，还有它认的那片环境。",
            "ClosureData 是闭包运行时的实体，继承自 RefCounted，里头装着：\n\n"
            "- `chunk`：指向 BytecodeChunk 的指针（函数字节码）\n"
            "- `upvalues`：已捕获的 upvalue 列表（每个 upvalue 是堆上 Value 的引用）\n"
            "- `name`：函数名（用于错误报告与调试）\n"
            "- `arity`：参数数量（含默认参数处理）\n\n"
            "**创建时机**：执行 `OP_CLOSURE` 指令时，从 BytecodeChunk 中读取 upvalue 描述，逐一捕获当前 Environment 中的变量。\n\n"
            "**调用语义**：调用 closure 时创建新栈帧，绑定参数到局部变量，将 upvalues 注入到帧 Environment 中。\n\n"
            "**GC 关系**：ClosureData 是 GC 管理的堆对象，引用了 BytecodeChunk 与 upvalue 目标变量，形成 GC 图中的可达性路径。",
            {"closure", "upvalue", "bytecode-chunk", "gc", "refcounted"}
        },
        // ---- cow ----
        {
            "cow",
            "COW 写时复制",
            "内存模型",
            "Copy-On-Write：几个引用先共用同一份数据，直到有人要改，才真的拷贝一份——省内存的偷懒妙招。",
            "COW（Copy-On-Write）是 MiniLang 给数组和字典用的一招省内存技巧：多个 Value 先指着同一份堆数据，相安无事；直到其中一个要动手改，才真去拷贝一份独立副本。\n\n"
            "**工作流程**：\n\n"
            "1. 赋值 `var b = a;`（a 是数组）→ b 与 a 共享同一份堆数据，引用计数 +1\n"
            "2. 读取 `b[0]` → 直接访问共享数据，无复制\n"
            "3. 写入 `b[0] = 1;` → 检查引用计数，若 >1 则深拷贝一份，a 仍指向原数据\n\n"
            "**独占所有权检查**：写前检查 `refCount == 1`，独占时直接原地修改，无需复制。\n\n"
            "**优点**：\n"
            "- 赋值/传参零拷贝（仅引用计数 +1）\n"
            "- 只读场景无内存浪费\n\n"
            "**注意**：COW 与 RefCounted 强耦合——任何忘记 COW 检查的写操作都会污染共享数据，导致三后端语义不一致。",
            {"nan-boxing", "refcounted", "gc"}
        },
        // ---- debugger ----
        {
            "debugger",
            "Debugger 调试器",
            "调试",
            "让你能把手伸进跑着的程序里、随时喊停看个究竟的 IDE 工具。",
            "## Debugger 调试器\n\n断点、单步、看变量——它是你钻进程序肚子里东摸西看的放大镜。底下这些，就是它的全套本事。\n\n"
            "MiniLang IDE 内置的调试器基于 `DebugController`，提供以下核心能力：\n\n"
            "- **断点（Breakpoint）**：在指定行暂停程序执行\n"
            "- **单步执行（Stepping）**：Step In / Step Over / Step Out 三种模式\n"
            "- **条件断点**：仅在条件为真时暂停（如 `i == 50`）\n"
            "- **变量检查**：查看当前作用域所有变量的值\n"
            "- **调用栈**：查看函数调用链\n\n"
            "调试器使用沙箱隔离——条件断点求值不污染程序状态（保存/恢复所有可变状态）。\n\n"
            "**快捷键**：F6 启动调试，F7 单步，F8 跳过，Shift+F8 跳出。",
            {"breakpoint-condition", "call-stack", "variable-inspector"}
        },
        // ---- environment ----
        {
            "environment",
            "Environment 作用域链",
            "运行时",
            "一串串起来的作用域：每个块、每个函数帧都挂着一个 Environment 节点，顺着 parent 一路往外找变量。",
            "Environment 就是 MiniLang 管「变量在哪」的那条链子。它用链表实现：每个块、每个函数帧都有一个 Environment 节点，靠 parent 指针一环扣一环，往外就是更大的作用域。\n\n"
            "**结构**：\n"
            "- `variables`：当前作用域的变量表（名称 → Value 槽位）\n"
            "- `parent`：外层作用域指针\n"
            "- `boundInstance_`：当前绑定的类实例缓存（见 bound-instance）\n"
            "- `enclosingClosure`：若为闭包帧，指向 ClosureData\n\n"
            "**变量查找**：从当前 Environment 开始，沿 parent 链向上搜索。找到则返回，未找到报“未定义变量”。\n\n"
            "**块作用域**：`{ }` 块创建新的 Environment 节点，块结束时销毁。for/while/if 体各自独立作用域。\n\n"
            "**性能**：boundInstance_ 缓存优化使方法体内字段访问为 O(1)，避免沿作用域链查找 this。",
            {"closure", "bound-instance", "stack-frame", "module-system"}
        },
        // ---- error-recovery ----
        {
            "error-recovery",
            "Error Recovery 错误恢复",
            "编译",
            "Parser 撞上语法错误不甩手不干，而是跳过那颗坏 token，继续往后解析——尽量多报几个错。",
            "## Error Recovery 错误恢复\n\n写代码谁没手滑过？这个机制让 Parser 就算踩到坑，也先跨过去把后面的问题一并揪出来，而不是一错就躺。\n\n"
            "MiniLang 的 Parser 使用 `synchronize()` 方法实现错误恢复：\n\n"
            "1. 遇到语法错误时不立即中止\n"
            "2. 跳过后续 token 直到找到语句边界（`;` 或 `}`）\n"
            "3. 从下一个语句继续解析\n\n"
            "这样做的好处是**一次报告多个错误**，而不是遇到第一个错就停下。\n\n"
            "**同步点**：`;`（语句结束）和 `}`（块结束）是主要的同步 token。\n"
            "注释 token 和插值 token 不能作为同步点——必须消耗跳过。",
            {"parser", "token"}
        },
        // ---- field-slot ----
        {
            "field-slot",
            "fieldSlotIndex 字段槽位",
            "运行时",
            "类实例的字段被拍平成一排编号格子：父类在前，子类往后接，访问时按号直取，快。",
            "fieldSlotIndex 把类实例的字段压成了一排连续编号的格子。顺着继承链展平：父类的字段占前面的号，子类的接在后面，于是访问字段就是按号直取，不用一层层爬。\n\n"
            "**布局示例**：\n\n"
            "```\n"
            "class A { var x; var y; }      // x=slot 0, y=slot 1\n"
            "class B extends A { var z; }   // z=slot 2\n"
            "```\n\n"
            "**编译期计算**：fieldSlotIndex 在 Parser 处理类声明时确定，存入字段元数据。运行时通过 `instance.fields[slot]` 直接索引访问。\n\n"
            "**已知 Bug 模式**：\n"
            "- 字段默认值丢失（继承链展平时遗漏父类字段初始化）\n"
            "- super 方法查找错误导致字段写到错误槽位\n"
            "- 三后端 fieldSlotIndex 计算不一致导致字段错位",
            {"bound-instance", "environment", "triple-backend"}
        },
        // ---- gc ----
        {
            "gc",
            "GC mark-sweep 垃圾回收",
            "内存模型",
            "自动收拾内存的清洁工：从「根」出发顺着引用走一遍，走不到的堆对象就当垃圾清掉。",
            "GC（Garbage Collector）是 MiniLang 的自动内存管家，用的就是 mark-sweep 算法。GcManager 会定期扫一遍堆：从根集合出发能摸到的都留着，摸不到的，清掉。\n\n"
            "**工作流程**：\n\n"
            "1. **标记阶段**（mark）：从根集合（VM 操作数栈、Environment 链、全局变量、闭包 upvalues）出发，深度优先遍历所有可达堆对象，打上标记\n"
            "2. **清除阶段**（sweep）：扫描堆中所有对象，未标记的释放内存，标记的重置标志位\n\n"
            "**根集合**：\n"
            "- VM 操作数栈中的所有 Value\n"
            "- 当前调用栈所有帧的 Environment\n"
            "- 全局变量表\n"
            "- 闭包的 upvalues（已 close 到堆的）\n\n"
            "**触发时机**：分配新对象时检查堆阈值，超过则触发 GC。也可手动触发。\n\n"
            "**与 RefCounted 关系**：MiniLang 同时使用引用计数与 GC——RefCounted 处理大多数引用语义释放，GC 兜底处理循环引用。",
            {"refcounted", "nan-boxing", "cow", "closure-data", "mark-sweep"}
        },
        // ---- integer-division ----
        {
            "integer-division",
            "整数除法截断向零",
            "类型系统",
            "整数相除只留整数部分，且朝零靠（和 C 一样）：`7/2=3`、`-7/2=-3`。三后端都得这么算。",
            "MiniLang 的整数除法走的是「截断向零」的路子，跟 C/C++ 一个脾气：`7 / 2 = 3`，`-7 / 2 = -3`（注意不是 -4）。这点三后端必须一致，否则结果就对不上。\n\n"
            "**三后端统一**：\n"
            "- Interpreter：直接使用 C++ `/` 运算符（int 类型，自然截断向零）\n"
            "- StackVM：`OP_DIVIDE` 指令对 int 操作数执行 C++ `/`\n"
            "- RegisterVM：寄存器除法指令同样截断向零\n\n"
            "**历史 Bug**：曾出现 RegisterVM 使用浮点除法再取整导致负数结果不一致（向负无穷取整而非向零）。修复后统一为 C++ int `/` 语义。\n\n"
            "**与 Python 的差异**：Python 的 `//` 是地板除（向负无穷），MiniLang 是截断除（向零）。教学时需明确区分。",
            {"triple-backend", "type-annotation", "short-circuit"}
        },
        // ---- ir ----
        {
            "ir",
            "IR 中间表示",
            "编译",
            "夹在 AST 和字节码中间的那层表示：用 SSA 风格的虚拟寄存器，方便做优化。",
            "IR（Intermediate Representation）是 MiniLang 架在 AST 和字节码中间的那一层。它长成 SSA-like（静态单赋值）的样子，用虚拟寄存器记账，好处是后面做优化时特别顺手。\n\n"
            "**💡 生活类比**：IR 像「世界语写的菜谱」——比 AST 具体（已是步骤指令），又不绑定任何料理机（栈式/寄存器式都能翻译）。引入 IR，优化和翻译就只需写一次，三后端共享。\n\n"
            "**🔍 直观演示**：\n"
            "```\n"
            "源码:  var y = 1 + 2;\n"
            "IR:    v0 = LOAD_CONST 1\n"
            "       v1 = LOAD_CONST 2\n"
            "       v2 = ADD v0, v1\n"
            "```\n\n"
            "**IR 指令**（52 个 IROp）：\n"
            "- `IRConst` / `IRLoadVar` / `IRStoreVar`\n"
            "- `IRAdd` / `IRSub` / `IRMul` / `IRDiv`\n"
            "- `IRBranch` / `IRJump` / `IRReturn`\n"
            "- `IRCall` / `IRCallMethod`\n"
            "- `IRClosure` / `IRGetUpvalue` / `IRSetUpvalue`\n\n"
            "**SSA 特性**：每个虚拟寄存器（vreg）只赋值一次，简化数据流分析。优化 pass 利用此特性：\n"
            "- 常量折叠：`v1 = const(2); v2 = const(3); v3 = add(v1, v2)` → `v3 = const(5)`\n"
            "- 死代码消除：未被引用的指令移除\n"
            "- 复制传播：`v2 = v1; use(v2)` → `use(v1)`\n\n"
            "**双后端 lowering**：IR 可 lowering 到 StackVM bytecode（BytecodeIRBackend）或 RegisterVM RegOp（RegisterBytecodeBackend）。",
            {"ssa", "ir-module", "ast", "lowering", "bytecode"}
        },
        // ---- ir-module ----
        {
            "ir-module",
            "IRModule IR 模块",
            "编译",
            "AstIRBuilder 吐出的 IR 大包裹：函数 IR、全局变量、类型信息，全塞在一块儿。",
            "IRModule 是 AstIRBuilder 走完 AST 后产出的 IR 容器，也是走 IR 路径时的顶层组织单位。\n\n"
            "**内容**：\n"
            "- `functions`：函数 IR 列表（每个函数含 vreg 指令序列 + 参数 + 局部变量）\n"
            "- `globals`：全局变量槽位（含 import 进来的符号）\n"
            "- `types`：类型信息（类型注解 + 类继承关系）\n"
            "- `entryFunction`：入口函数索引（main 或顶层）\n\n"
            "**生成流程**：\n"
            "1. AstIRBuilder 遍历 AST，每个 FunDecl 生成一个 IRFunction\n"
            "2. 函数内表达式翻译为 vreg 指令序列\n"
            "3. 闭包识别后插入 IRClosure 指令\n"
            "4. 全局变量收集到 globals 槽位\n\n"
            "**消费方**：BytecodeIRBackend / RegisterBytecodeBackend 读取 IRModule，lowering 到对应后端的字节码。",
            {"ir", "ast", "bytecode-chunk", "ssa"}
        },
        // ---- lexer ----
        {
            "lexer",
            "Lexer 词法分析器",
            "编译",
            "编译管线的第一关：把一串字符读成一颗颗带类型的 Token，顺手丢掉空格和注释。",
            "Lexer 是编译管线的第一道工序，干的事是把源码字符流扫成 Token 序列。它是手写的有限状态机（不是 lex/yacc 生成）——老派，但好调。\n\n"
            "**💡 生活类比**：Lexer 像工厂流水线的「分拣机」——原料（字符）从一头进去，另一头出来的是按种类分好类的零件（Token）。它不关心零件怎么组装，只负责「认出这是什么」。\n\n"
            "**扫描规则**：\n"
            "- 跳过空白与注释（`//` 行注释、`/* */` 块注释）\n"
            "- 识别关键字（var / fun / class / if / while / for / return / break / continue / and / or / not / true / false / null / try / catch / finally / throw / import / export / extends / super）\n"
            "- 识别标识符（字母/下划线开头）\n"
            "- 识别数字字面量（整数 / 浮点 / 负号在 Parser 处理）\n"
            "- 识别字符串字面量（含插值 `\"...{expr}...\"`）\n"
            "- 识别运算符与分隔符\n\n"
            "**插值处理**：字符串插值 `\"hello {name}\"` 由 Lexer 拆分为多段——字符串字面量 + 表达式 Token 序列 + 字符串字面量，嵌套深度限制 64 层。\n\n"
            "**安全约束**：Token 总数 ≤ 100 万（DoS 防护），列号与行号同步更新。",
            {"token", "parser", "ast"}
        },
        // ---- line-info ----
        {
            "line-info",
            "Line Info 行号映射",
            "编译",
            "一张「指令↔源码行号」对照表，报错和断点全靠它把位置翻回你写的哪一行。",
            "## Line Info 行号映射\n\n报错时它告诉你错在第几行，调试时它帮断点对齐源码——没有这张表，错误提示就只剩一脸懵。\n\n"
            "`BytecodeChunk` 中的 `lines` 数组记录了每条字节码指令对应的源码行号。\n\n"
            "当调试器在某个指令处暂停时，通过查询 `lines` 数组可以知道当前执行到源码的哪一行。\n"
            "当运行时错误发生时，行号映射让错误消息能报告准确的出错位置。\n\n"
            "**实现方式**：编译时每条 `emit()` 指令都同时记录当前 `currentLine`。\n"
            "运行时 `lineInfoAt(offset)` 根据偏移量查找对应行号。",
            {"bytecode-chunk", "debugger"}
        },
        // ---- lowering ----
        {
            "lowering",
            "lowering 降低",
            "编译",
            "把高层 IR「降」成具体后端听得懂的指令：要么栈式 bytecode，要么寄存器 RegOp。",
            "lowering 是编译器圈的黑话，意思就是把高层的 IR 翻译成更贴地的目标代码。在 MiniLang 里，IR 节点会被 lowering 到两种后端：\n\n"
            "**💡 生活类比**：lowering 像「把世界语菜谱翻译成某品牌料理机的专属卡片」——高层 IR 是通用描述，lowering 成栈式或寄存器式字节码，就是两种不同料理机的「执行卡片」。\n\n"
            "**StackVM 后端**（BytecodeIRBackend）：\n"
            "- `IRConst v1, c` → `OP_CONSTANT c; OP_STORE v1`（或仅 `OP_CONSTANT` 留栈上）\n"
            "- `IRAdd v3, v1, v2` → `OP_LOAD v1; OP_LOAD v2; OP_ADD; OP_STORE v3`\n"
            "- 栈式语义：操作数先 push 到栈，运算后 pop 弹出\n\n"
            "**RegisterVM 后端**（RegisterBytecodeBackend）：\n"
            "- `IRConst v1, c` → `REG_LOAD_CONST R_v1, c`\n"
            "- `IRAdd v3, v1, v2` → `REG_ADD R_v3, R_v1, R_v2`\n"
            "- 寄存器语义：直接指定源/目的寄存器，无 push/pop\n\n"
            "**优化机会**：lowering 时可应用优化 pass（常量折叠 / 死代码消除 / 复制传播），仅寄存器后端启用复制传播。",
            {"ir", "ssa", "bytecode", "register-vm"}
        },
        // ---- mark-sweep ----
        {
            "mark-sweep",
            "mark-sweep 标记清除",
            "内存模型",
            "GC 的干活两步走：先沿着引用画圈标记活对象，再把没被画到的内存腾出来。",
            "mark-sweep 是 MiniLang 这套 GC 用的算法，就两步走：\n\n"
            "**标记阶段**（mark）：\n"
            "- 从根集合（操作数栈 / Environment 链 / 全局变量 / 闭包 upvalues）出发\n"
            "- 深度优先遍历所有可达堆对象\n"
            "- 对每个访问到的对象打上 marked 标志\n\n"
            "**清除阶段**（sweep）：\n"
            "- 扫描堆中所有对象\n"
            "- marked 的对象保留并清除标志（供下一轮使用）\n"
            "- 未 marked 的对象不可达，释放内存\n\n"
            "**特点**：\n"
            "- 简单可靠，不移动对象（无内存整理）\n"
            "- 可能产生内存碎片\n"
            "- 暂停时间与堆大小成正比（STW）\n\n"
            "**对比**：与复制式 GC / 分代 GC 不同，mark-sweep 不需要预留 to-space，内存利用率高，但暂停时间较长。",
            {"gc", "refcounted", "closure-data"}
        },
        // ---- module-loader ----
        {
            "module-loader",
            "Module Loader 模块加载器",
            "运行时",
            "import 背后的搬运工：解析路径、把模块源码拉进来、还顺手缓存好下次直接用。",
            "## Module Loader 模块加载器\n\n你写一句 import，真正去翻文件、读源码、防重复加载的，就是这位。循环依赖它也能嗅出来。\n\n"
            "当解释器执行 `import` 语句时，模块加载器负责：\n\n"
            "1. **路径解析**：将相对路径转为绝对路径（基于当前文件目录）\n"
            "2. **安全校验**：拒绝 `..` 和绝对路径（防目录遍历）\n"
            "3. **缓存管理**：同一模块只加载一次（避免重复解析）\n"
            "4. **回调通知**：通过 `moduleLoader_` 接口支持热重载（REPL 的 `#reload` 命令）\n\n"
            "**路径遍历防护**：拒绝包含 `..` 的路径和绝对路径，确保 import 只能访问项目目录内的文件。",
            {"module-system"}
        },
        // ---- module-system ----
        {
            "module-system",
            "module system 模块系统",
            "运行时",
            "让你把代码拆到不同文件再用 import/export 拼回来的机制，还管着路径安全、循环依赖这些脏活。",
            "module system 让 MiniLang 的代码能跨文件复用，靠的就是 import / export 这套语法。\n\n"
            "**语法**：\n"
            "- 命名导入：`import {a, b} from \"module\";`\n"
            "- 全量导入：`import * as m from \"module\";`\n"
            "- 导出声明：`export var x = 1;` / `export fun f() {}`\n\n"
            "**安全约束**：\n"
            "- 路径非空校验\n"
            "- 路径遍历防护：拒绝 `..` 父目录引用与绝对路径\n"
            "- import/export 限制在顶层作用域（不可在函数/块内）\n\n"
            "**循环依赖检测**：加载模块时记录正在加载的路径，若再次遇到同一路径则报循环依赖错误。\n\n"
            "**预扫描**：模块加载前先扫描 FunDecl / ExportStmt，建立符号表后再执行。这避免了“函数提升”语义——MiniLang 函数无提升，必须先声明后使用。\n\n"
            "**已知 Bug 模式**：\n"
            "- 预扫描遗漏 FunDecl / ExportStmt 导致符号缺失\n"
            "- VM 路径模块内联与 Interpreter 模块加载行为不一致\n"
            "- 错误传播链中断导致模块加载失败被吞",
            {"environment", "parser", "ast", "triple-backend"}
        },
        // ---- nan-boxing ----
        {
            "nan-boxing",
            "NaN-boxing NaN 装箱",
            "内存模型",
            "把各种类型都塞进 8 字节的巧招：借 IEEE 754 里 NaN 的空闲位，顺带记下类型标签和真正的值。",
            "NaN-boxing 是 MiniLang 表示 Value 的巧劲儿：所有类型都编码成 8 字节（和 double 一样宽），再借 IEEE 754 里 NaN 那段没人用的比特，藏进类型标签和真正的数据。\n\n"
            "**💡 生活类比**：NaN-boxing 像把糖、书、箱子都塞进统一规格的 8 号储物柜——小物件直接放进柜子，大物件只留一张「指向仓库的提货卡」。柜子规格统一，管理极高效，这就是 MiniLang 用 8 字节装下所有值的秘诀。\n\n"
            "**编码布局**（8 字节 = 64 位）：\n\n"
            "```\n"
            "  高 16 位: 类型标签 (QNaN + 自定义位)\n"
            "  低 48 位: 指针/标量载荷\n"
            "```\n\n"
            "**类型分类**：\n"
            "- double：直接以 IEEE 754 double 存储（标志位非 QNaN）\n"
            "- int48：48 位补码整数，支持内联存储（无需堆分配）\n"
            "- bool / null：低 48 位编码为常量\n"
            "- 堆类型指针（string / array / dict / closure / instance）：低 48 位为堆指针\n\n"
            "**优势**：\n"
            "- 标量（int / float / bool / null）零堆分配，直接存于 Value\n"
            "- 8 字节定宽，利于缓存局部性\n"
            "- 类型检查仅需位运算\n\n"
            "**约束**：int 范围限制在 int48（±2^47），超出自动装箱为堆对象。",
            {"cow", "refcounted", "gc", "opstack"}
        },
        // ---- opstack ----
        {
            "opstack",
            "operand stack 操作数栈",
            "运行时",
            "StackVM 干活用的那摞盘子：`Value[1024]` 定长数组，指令靠 push/pop 把操作数传来传去。",
            "operand stack 是 StackVM 的心脏，说白了就是一个定长数组 `Value[1024]` 配一个栈顶指针 sp。指令不明说操作数在哪，全靠隐式 push/pop 来递来递去。\n\n"
            "**💡 生活类比**：操作数栈像一个「只从顶上拿放的托盘」——`OP_ADD` 就是「从托盘顶端取两个、算完放回去」。栈平衡不变量保证了托盘不会越堆越高（泄漏）也不会被掏空（下溢）。\n\n"
            "**操作示例**：\n"
            "- `OP_CONSTANT 3` → push(Value(3))，sp++\n"
            "- `OP_ADD` → b = pop(), a = pop(), push(a + b)，sp -= 1 净\n"
            "- `OP_GET_LOCAL 0` → push(locals[0])\n\n"
            "**栈平衡不变量**：\n"
            "- 表达式求值后栈净增长 1（结果压栈）\n"
            "- 语句执行后栈净增长 0（结果被 POP）\n"
            "- 函数调用后栈恢复到调用前状态（含返回值替换）\n\n"
            "**已知高频 Bug**：IR 路径表达式语句返回值未 POP 导致栈泄漏（曾出现 16 种节点类型遗漏）。\n\n"
            "**安全**：sp 越界检查（上溢 1024 / 下溢 0），越界时报栈溢出错误。",
            {"stack-vm", "stack-frame", "bytecode", "nan-boxing"}
        },
        // ---- output-capture ----
        {
            "output-capture",
            "Output Capture 输出捕获",
            "运行时",
            "Interpreter 里 print() 吐出的字，不直接飞走，而是经回调函数拐一道交给 GUI 显示。",
            "## Output Capture 输出捕获\n\n没有它，程序打印的东西就掉进黑洞了。它把 print 的输出接管过来，乖乖送进界面让你看见。\n\n"
            "MiniLang 的 `print()` 语句不直接写入 stdout，而是通过 **输出回调函数** 传递文本：\n\n"
            "```cpp\n"
            "interpreter->setOutputCallback([](const std::string& text) {\n"
            "    // text 被发送到 GUI 的输出面板\n"
            "});\n"
            "```\n\n"
            "这种设计让输出可以被不同消费者使用：\n"
            "- **GUI 模式**：回调将文本追加到输出面板的 QTextEdit\n"
            "- **测试模式**：回调将文本收集到字符串用于断言比对\n"
            "- **性能测试**：回调为空（丢弃输出），不影响计时\n\n"
            "`IdeController::runStringCaptureOutput()` 利用这一机制同步运行代码并捕获全部输出。",
            {"environment", "debugger"}
        },
        // ---- parser ----
        {
            "parser",
            "Parser 递归下降解析器",
            "编译",
            "把 Token 串拼成 AST 的工匠：手写的递归下降，文法里每个非终结符都对应一个函数。",
            "Parser 是手写的递归下降解析器（recursive descent parser），任务是把 Lexer 交来的 Token 序列，搭成 AST。\n\n"
            "**💡 生活类比**：Parser 像把一串「主谓宾」单词拼回句子结构树的工人。「猫 吃 鱼」不是三个孤立词，而是「吃(主语=猫, 宾语=鱼)」——Parser 把线性的 Token 串还原出程序的层次。\n\n"
            "**结构**：每个语法非终结符对应一个解析函数，如：\n"
            "- `parseExpression()` → 优先级 climb 调度到 `parseBinary/precedence`\n"
            "- `parseStatement()` → 分发到 `parseVarDecl / parseIfStmt / parseWhileStmt / ...`\n"
            "- `parseFunDecl()` / `parseClassDecl()`\n\n"
            "**优先级处理**：采用 Pratt parser 模式，按运算符优先级表（最低 → 最高）依次绑定。\n\n"
            "**错误恢复**：\n"
            "- 同步点（synchronization token）：`;` / `}` / 顶层声明关键字\n"
            "- 错误后跳到同步点继续解析，收集多个错误一次报告\n"
            "- 错误数量上限（防 DoS）\n\n"
            "**安全约束**：递归深度限制 512（防栈溢出），类型注解预检。\n\n"
            "**三后端一致性**：Parser 输出的 AST 是三后端的共享输入，任何解析歧义都会导致三后端分歧。",
            {"lexer", "token", "ast", "triple-backend"}
        },
        // ---- refcounted ----
        {
            "refcounted",
            "RefCounted 引用计数",
            "内存模型",
            "堆对象的「公用地基」：侵入式引用计数，像 shared_ptr 那样自动管生死，但更轻。",
            "RefCounted 是 MiniLang 堆对象共用的侵入式引用计数基类，干的是 shared_ptr 那类自动管内存的活儿。\n\n"
            "**工作原理**：\n"
            "- 每个堆对象内含 `refCount` 字段\n"
            "- 新引用建立时 `refCount++`，引用销毁时 `refCount--`\n"
            "- `refCount == 0` 时释放对象内存\n\n"
            "**NaN-boxing 协作**：Value 的低 48 位存储堆指针，赋值时通过 `incRef`/`decRef` 维护计数。\n\n"
            "**COW 协作**：数组/字典写前检查 `refCount == 1`，独占时直接原地修改（无需复制）。\n\n"
            "**循环引用问题**：\n"
            "- A 引用 B，B 引用 A → refCount 永不归零，内存泄漏\n"
            "- GC（mark-sweep）兜底处理循环引用\n\n"
            "**性能**：侵入式设计避免单独分配控制块（对比 std::shared_ptr 的两块分配）。",
            {"nan-boxing", "cow", "gc", "closure-data"}
        },
        // ---- register-vm ----
        {
            "register-vm",
            "RegisterVM 寄存器式虚拟机",
            "运行时",
            "用 32 个虚拟寄存器（R0–R31）的字节码 VM：指令直接拨寄存器，少了很多 push/pop 的来回。",
            "RegisterVM 是 MiniLang 的寄存器式字节码虚拟机，摆着 32 个虚拟寄存器 R0–R31，约 50 个 RegOp 指令。\n\n"
            "**💡 生活类比**：RegisterVM 像有「一排带编号的盒子 R0-R31」——食材分类放进固定盒子，指令直接说「把 R1 和 R2 相加放进 R3」，不用每次都往一个盘子上堆。更接近真实 CPU 的做事方式。\n\n"
            "**与 StackVM 对比**：\n\n"
            "| 特性 | StackVM | RegisterVM |\n"
            "|------|---------|------------|\n"
            "| 操作数传递 | 操作数栈 push/pop | 寄存器直接索引 |\n"
            "| 指令长度 | 1 字节 OpCode + 操作数 | 1 字节 RegOp + 寄存器号 |\n"
            "| 指令数 | ~60 OpCode | ~50 RegOp |\n"
            "| 帧开销 | 栈帧含局部变量区 | std::array<Value, 32> 寄存器帧 |\n\n"
            "**优势**：\n"
            "- 减少指令数（无需 push/pop 中间步骤）\n"
            "- 寄存器帧零堆分配（std::array<Value, 32>）\n"
            "- 更接近现代 CPU 架构（JIT 友好）\n\n"
            "**IR 直接 lowering**：IRModule 经 RegisterBytecodeBackend 直接 lowering 为 RegOp，无需经 StackVM bytecode。\n\n"
            "**约束**：32 寄存器上限，复杂表达式可能溢出（spill 到栈），由寄存器分配算法决定。",
            {"stack-vm", "ir", "lowering", "opstack", "triple-backend"}
        },
        // ---- short-circuit ----
        {
            "short-circuit",
            "and/or 短路求值",
            "运行时",
            "`and`/`or` 懒得算到底：能定胜负就停，而且返回的是操作数本身（不是硬塞的 true/false），三后端一致。",
            "MiniLang 的 `and` / `or` 走短路策略，而且返回的是操作数原值——不是非给你塞个布尔。这点三后端得保持一致。\n\n"
            "**语义**：\n"
            "- `a and b`：若 a 为 falsy（false / null / 0 / \"\"），直接返回 a，不计算 b\n"
            "- `a or b`：若 a 为 truthy（非 falsy），直接返回 a，不计算 b\n\n"
            "**示例**：\n"
            "```\n"
            "var x = 0 or \"default\";   // x = \"default\"\n"
            "var y = 1 and 2;           // y = 2 (返回原值, 非布尔 true)\n"
            "var z = false or 0;        // z = 0 (返回原值)\n"
            "```\n\n"
            "**三后端统一**：\n"
            "- Interpreter：直接 if 判断 + 返回原 Value\n"
            "- StackVM：`OP_JUMP_IF_FALSE` 跳过右操作数求值\n"
            "- RegisterVM：寄存器条件跳转指令\n\n"
            "**历史 Bug**：曾出现 StackVM 返回布尔值而非原值，与 Interpreter 不一致。修复后统一返回原操作数 Value。",
            {"triple-backend", "integer-division", "type-annotation"}
        },
        // ---- ssa ----
        {
            "ssa",
            "SSA 静态单赋值",
            "编译",
            "给变量立规矩「一生只赋一次值」的表示法，换来数据流分析和优化时的清清爽爽。",
            "SSA（Static Single Assignment）是编译器里一种中间表示形式，铁律是：每个变量只能被赋值一次。MiniLang 的 IR 走的是 SSA-like 的路子。\n\n"
            "**💡 生活类比**：SSA 像「每次给新盒子贴新标签」——变量 `x` 第一次赋值叫 `v1`，第二次叫 `v2`，绝不重用。这样「谁定义了谁、谁用了谁」一目了然，优化器找死代码、做常量折叠都变得简单。\n\n"
            "**特性**：\n"
            "- 每个虚拟寄存器（vreg）只被一条指令定义\n"
            "- 变量重新赋值时生成新的 vreg（如 v1 → v2 → v3）\n"
            "- 控制流汇合点使用 φ（phi）函数（MiniLang 简化未实现 phi，由后端处理）\n\n"
            "**优势**：\n"
            "- 数据流分析简化（use-def 链明确）\n"
            "- 优化 pass 易于实现：\n"
            "  - 常量折叠：追踪 vreg 到常量\n"
            "  - 死代码消除：未被引用的 vreg 定义可删\n"
            "  - 复制传播：`v2 = v1` 时用 v1 替换 v2 的使用\n\n"
            "**lowering**：SSA 形式 lowering 到 StackVM bytecode 时，vreg 映射到局部变量槽位；lowering 到 RegisterVM 时映射到物理寄存器。",
            {"ir", "lowering", "ir-module"}
        },
        // ---- stack-frame ----
        {
            "stack-frame",
            "stack frame 栈帧",
            "运行时",
            "函数被调用时当场扯出的一张「现场记录」：局部变量、栈顶位置、回来的地址，都记上。",
            "stack frame 是函数调用时在 VM 调用栈上现搭的上下文。MiniLang 的栈帧里头有：\n\n"
            "**内容**：\n"
            "- 局部变量区（locals[]，存参数 + 局部 var 声明）\n"
            "- 操作数栈顶快照（调用前 sp，用于返回时恢复）\n"
            "- 返回地址（指令指针 ip，调用前位置，用于返回时恢复）\n"
            "- Environment 指针（变量作用域链）\n"
            "- 闭包上下文（若是 closure 调用）\n\n"
            "**生命周期**：\n"
            "1. 函数调用时 push 新栈帧\n"
            "2. 参数绑定到 locals[0..arity-1]\n"
            "3. 局部 var 声明追加到 locals[arity..]\n"
            "4. 执行函数体字节码\n"
            "5. OP_RETURN 时弹出栈帧，恢复 ip 与 sp\n\n"
            "**RegisterVM 差异**：寄存器帧使用 `std::array<Value, 32>` 零堆分配，比 StackVM 的局部变量区更紧凑。\n\n"
            "**深度限制**：调用栈深度限制 1024（防递归栈溢出）。",
            {"opstack", "environment", "stack-vm", "register-vm"}
        },
        // ---- stack-vm ----
        {
            "stack-vm",
            "StackVM 栈式虚拟机",
            "运行时",
            "靠操作数栈传数的字节码 VM，约 60 个 OpCode，IR 和非 IR 两条路都靠它跑。",
            "StackVM 是 MiniLang 的栈式字节码虚拟机，操作数全靠那座操作数栈传来传去，约有 60 个 OpCode。\n\n"
            "**💡 生活类比**：StackVM 像只用「一个盘子」做饭——所有食材都堆在那一个盘子上，取料从顶上拿、做好放回去。简单但频繁地「放/取」，就是 push/pop。\n\n"
            "**🔍 直观演示**：\n"
            "```\n"
            "OP_INT 1   ; 盘子: [1]\n"
            "OP_INT 2   ; 盘子: [1, 2]\n"
            "OP_ADD     ; 盘子: [3]  (弹出 2,1 压回 3)\n"
            "```\n\n"
            "**执行模型**：\n"
            "- 取指 → 解码 → 执行 → 推进 ip\n"
            "- 操作数隐式从操作数栈顶 pop\n"
            "- 结果隐式 push 回栈顶\n\n"
            "**双路径共享**：\n"
            "- 非 IR 路径：Compiler 直接将 AST 编译为 BytecodeChunk，StackVM 执行\n"
            "- IR 路径：AstIRBuilder 生成 IRModule → BytecodeIRBackend lowering 为 BytecodeChunk → 同一 StackVM 执行\n\n"
            "**指令示例**：\n"
            "```\n"
            "OP_CONSTANT 3    ; push 3\n"
            "OP_CONSTANT 4    ; push 4\n"
            "OP_ADD           ; pop 4, pop 3, push 7\n"
            "OP_RETURN        ; pop 7 作为返回值\n"
            "```\n\n"
            "**调试集成**：单步执行时高亮当前指令，操作数栈与局部变量可检查。\n\n"
            "**对比 RegisterVM**：StackVM 指令数多但单条简单，RegisterVM 指令数少但需寄存器分配。",
            {"register-vm", "bytecode", "opstack", "bytecode-chunk", "triple-backend"}
        },
        // ---- string-interpolation ----
        {
            "string-interpolation",
            "String Interpolation 字符串插值",
            "编译",
            "字符串里直接塞表达式的写法（比如 \"hi ${name}\"），Lexer 最多能套 64 层，够野。",
            "## String Interpolation 字符串插值\n\n想在字符串里顺手算点东西？这个特性让你把表达式嵌进引号里，Lexer 连套 64 层嵌套都不带喘。\n\n"
            "MiniLang 支持在字符串中用 `${表达式}` 嵌入计算结果：\n\n"
            "```minilang\n"
            "var name = \"World\";\n"
            "print(\"Hello, ${name}!\");  // Hello, World!\n"
            "print(\"1+2 = ${1 + 2}\");    // 1+2 = 3\n"
            "```\n\n"
            "**Lexer 实现**：遇到 `${` 时进入插值状态，产出 `TK_INTERP_START` token；\n"
            "表达式结束后产出 `TK_INTERP_END`，回到字符串状态。\n\n"
            "**嵌套深度**：最多 64 层嵌套（`MAX_INTERP_DEPTH`），防止恶意输入导致栈溢出。\n\n"
            "**类型转换**：插值表达式的结果自动转为字符串（整数/浮点/布尔/null 均可）。",
            {"lexer", "token"}
        },
        // ---- token ----
        {
            "token",
            "Token 词法单元",
            "编译",
            "Lexer 吐出的最小语法颗粒：每颗都贴着类型标签，还记着自己来自第几行第几列。",
            "Token 是 Lexer 扫完源码后吐出的最小语法颗粒，每颗都带着：\n\n"
            "**💡 生活类比**：Token 就像句子里的「词」。我们说「我爱编程」，分词后得到「我 / 爱 / 编程」三个有意义的词——编译器也是先把字符流切成「词」（Token），后面的语法分析才认得出结构。\n\n"
            "**🔍 直观演示**：\n"
            "```\n"
            "输入:  var x = 42;\n"
            "Token: var(关键字) / x(标识符) / =(运算符) / 42(数字) / ;(分隔符)\n"
            "```\n"
            "注：`42` 作为一个整体被识别为「数字」Token，而不是四个独立字符 `4` `2`——Token 的最小单元是「有意义的词」，不是字符。\n\n"
            "- **类型**（TokenType）：关键字 / 标识符 / 数字 / 字符串 / 运算符 / 分隔符 / EOF\n"
            "- **字面量**（lexeme）：原始字符串（如 `\"hello\"` 整体作为一个 STRING token 的 lexeme）\n"
            "- **位置**（line / column）：行列号，用于错误报告与调试\n\n"
            "**类型分类**：\n"
            "- 关键字：var / fun / class / if / while / for / return / break / continue / and / or / not / true / false / null / try / catch / finally / throw / import / export / extends / super\n"
            "- 字面量：INT / FLOAT / STRING（含插值标记）\n"
            "- 标识符：IDENTIFIER\n"
            "- 运算符：PLUS / MINUS / STAR / SLASH / PERCENT / EQ / NE / LT / GT / LE / GE / ASSIGN / DOT / COMMA / SEMICOLON / LPAREN / RPAREN / LBRACE / RBRACE / LBRACKET / RBRACKET\n\n"
            "**约束**：Token 总数 ≤ 100 万（DoS 防护）。",
            {"lexer", "parser", "ast"}
        },
        // ---- triple-backend ----
        {
            "triple-backend",
            "三后端一致性",
            "测试",
            "MiniLang 的硬规矩：同一份源码，走 Interpreter、StackVM、RegisterVM 三条路，结果必须一模一样。",
            "三后端一致性是 MiniLang 的一条铁律：同一份源码，走 Interpreter、StackVM、RegisterVM 三条路，语义结果必须一模一样。\n\n"
            "**💡 生活类比**：三后端一致性像「同一道菜谱，人工现做 / A 牌机 / B 牌机三种做法，端上桌味道必须一模一样」。内部步骤可以天差地别，但结果必须一致——这是编译器正确性的金标准。\n\n"
            "**三条路径**：\n"
            "1. **Interpreter**：树遍历解释器，直接执行 AST\n"
            "2. **StackVM**：Compiler → BytecodeChunk → 栈式 VM\n"
            "3. **RegisterVM**：AstIRBuilder → IRModule → RegisterBytecodeBackend → 寄存器式 VM\n\n"
            "**统一约束**：\n"
            "- 整数除法截断向零（见 integer-division）\n"
            "- and/or 短路返回操作数原值（非布尔，见 short-circuit）\n"
            "- 类型注解强制（见 type-annotation）\n"
            "- super 调用语义\n"
            "- 错误消息文本一致\n"
            "- 边界条件处理一致\n\n"
            "**测试策略**：每个语义点编写三后端交叉验证测试，对比三路径输出。\n\n"
            "**已知 Bug 模式**：\n"
            "- 错误消息文本差异\n"
            "- 类型检查行为差异\n"
            "- 边界条件（如负数取模）处理差异\n"
            "- super 方法查找回退链不一致",
            {"stack-vm", "register-vm", "integer-division", "short-circuit", "type-annotation"}
        },
        // ---- type-annotation ----
        {
            "type-annotation",
            "type annotation 类型注解",
            "类型系统",
            "给变量/参数/字段随手贴个类型（像 `int a = 5;`），三后端都会统一帮你查。",
            "type annotation 是 MiniLang 里可选的类型标注，支持 `int` / `float` / `bool` / `string` / `null` 这些类型。\n\n"
            "**语法**：\n"
            "```\n"
            "var x: int = 5;          // 变量注解\n"
            "fun f(a: int, b: string) -> bool { ... }  // 参数与返回注解\n"
            "class A { var x: int = 0; }              // 字段注解\n"
            "```\n\n"
            "**兼容规则**：\n"
            "- null 兼容所有类型注解（`var x: int = null;` 合法）\n"
            "- float 注解接受 int 值（宽化，`var x: float = 5;` 合法）\n"
            "- int 注解拒绝 float 值（`var x: int = 5.5;` 报错）\n"
            "- 类型不匹配时三后端统一报错\n\n"
            "**三后端统一强制**：\n"
            "- Interpreter：运行时类型检查\n"
            "- StackVM：编译期注入类型检查指令\n"
            "- RegisterVM：IR 路径在 lowering 时注入检查\n\n"
            "**历史 Bug**：曾出现某后端类型检查遗漏，导致同源码在某后端通过、其他后端报错。",
            {"integer-division", "triple-backend", "short-circuit"}
        },
        // ---- upvalue ----
        {
            "upvalue",
            "upvalue 上值",
            "运行时",
            "闭包从外层「顺走」的变量。它还住在栈上叫 open，被迁到堆上就叫 closed。",
            "upvalue 就是闭包从外层作用域顺走的那个变量。内层函数一旦引用了外层的局部变量，它就成了 upvalue。\n\n"
            "**两种状态**：\n"
            "- **open upvalue**：变量仍在栈上（外层函数未返回），upvalue 直接指向栈槽位\n"
            "- **closed upvalue**：变量已迁移到堆（外层函数返回时栈帧销毁，变量自动 close 到堆）\n\n"
            "**close 时机**：\n"
            "1. 外层函数执行 OP_RETURN\n"
            "2. 遍历该帧的所有 open upvalue\n"
            "3. 将栈上变量复制到堆，更新 upvalue 指向堆地址\n"
            "4. 弹出栈帧\n\n"
            "**多层嵌套捕获**：内层闭包可捕获外层闭包的 upvalue，形成捕获链。MiniLang 支持 3+ 层嵌套。\n\n"
            "**已知 Bug 模式**：\n"
            "- 多层嵌套捕获顺序错误\n"
            "- 变量快照恢复遗漏（GC 后 upvalue 指针失效）\n"
            "- try/catch 中 upvalue 未正确 close",
            {"closure", "closure-data", "environment", "stack-frame"}
        },
    };
}

// ============================================================
// populateList — 按过滤词显隐列表行
// ============================================================
// entries_ 已按 id 字母序排列，listWidget_ 行号与 entries_ 索引一一对应。
// filter 非空时按 term/id/category/shortDef 不区分大小写包含匹配。
// 使用 setRowHidden（而非 clear+re-add）保留当前选中状态。

void GlossaryPanel::populateList(const QString& filter) {
    QString f = filter.toLower();
    int n = listWidget_->count();
    for (int i = 0; i < n && i < entries_.size(); ++i) {
        bool match = f.isEmpty();
        if (!match) {
            const TermEntry& e = entries_[i];
            match = e.term.toLower().contains(f) ||
                    e.id.toLower().contains(f) ||
                    e.category.toLower().contains(f) ||
                    e.shortDef.toLower().contains(f);
        }
        listWidget_->setRowHidden(i, !match);
    }
}

// ============================================================
// showDetail — 渲染术语详情 HTML
// ============================================================
// 格式：h2 标题 / p 分类 / p 定义 / h3 详细说明（Markdown 渲染）/
// h3 关联术语（term: 链接列表）。复用 MarkdownRenderer 渲染 fullDef，
// 复用 PanelAnimator::fadeInWidget 切换淡入。

void GlossaryPanel::showDetail(int index) {
    if (index < 0 || index >= entries_.size()) {
        detailView_->clear();
        return;
    }
    const TermEntry& e = entries_[index];

    std::ostringstream oss;
    oss << "<h2>" << e.term.toStdString() << "</h2>";
    oss << "<p><b>" << mlTr("分类:").toStdString() << "</b> "
        << e.category.toStdString() << "</p>";
    oss << "<p><b>" << mlTr("定义:").toStdString() << "</b> "
        << e.shortDef.toStdString() << "</p>";

    oss << "<h3>" << mlTr("详细说明").toStdString() << "</h3>";
    oss << MarkdownRenderer::markdownToHtmlFragment(e.fullDef).toStdString();

    if (!e.related.isEmpty()) {
        oss << "<h3>" << mlTr("关联术语").toStdString() << "</h3><ul>";
        for (const QString& rid : e.related) {
            // 查找关联术语的显示名（中英对照），找不到则显示 id
            QString display = rid;
            for (const TermEntry& re : entries_) {
                if (re.id == rid) {
                    display = re.term;
                    break;
                }
            }
            oss << "<li><a href=\"term:" << rid.toStdString() << "\">"
                << display.toStdString() << "</a></li>";
        }
        oss << "</ul>";
    }

    detailView_->setHtml(QString::fromStdString(oss.str()));
    // 注：移除 PanelAnimator::fadeInWidget —— 该函数对 QTextBrowser 施加
    // QGraphicsOpacityEffect 并将 opacity 置 0 再动画到 1。用户连续切换术语时
    // 旧动画被 stop+deleteLater 但新 effect 仍从 0 开始，若动画未完成（widget
    // 不可见/快速点击/动画被覆盖），opacity 卡在 0 导致详情区"切换后空白"。
    // 详情刷新无需动画，直接 setHtml 即可。
}

// ============================================================
// onSearchChanged — 搜索框文本变化时过滤列表
// ============================================================

void GlossaryPanel::onSearchChanged(const QString& text) {
    populateList(text);
}

// ============================================================
// onTermSelected — 列表选中行变化时仅显示详情
// ============================================================
// 注意：单击切换术语条目只刷新详情，不发射 termActivated。
// termActivated 仅在用户明确表达跳转意图时发射（双击列表项 / 点击
// 详情中的 term: 链接），避免每次切换术语就跳转到关联面板。
// 修复用户反馈：切换术语条时错误跳转到其他面板。

void GlossaryPanel::onTermSelected(int row) {
    if (row < 0 || row >= entries_.size()) {
        return;
    }
    showDetail(row);
}

// ============================================================
// P0-A fix: termId → 关联教学面板 id 静态映射
// ------------------------------------------------------------
// 复核版分析指出：GlossaryPanel::termActivated 信号三处 emit，
// 但 ide.cpp 从未 connect——点击术语只在面板内显示释义，无法跨
// 面板跳转。本映射表将 30 个核心术语对应到最相关的教学面板，
// 让"点击术语→跳转面板"真正可达。
//
// 映射原则：选择能"亲手把玩"该概念的玩具/沙盒/检查器面板，让
// 学员从定义过渡到可交互的实例。无明确对应面板的术语（如
// triple-backend 跨多后端概念）选择最贴近的面板。
// ============================================================
QString GlossaryPanel::relatedPanelFor(const QString& termId) {
    static const QHash<QString, QString> kTermToPanel = {
        // 编译前端概念 → pipeline / token-puzzle / ast-toy
        {QStringLiteral("lexer"),              QStringLiteral("pipeline")},
        {QStringLiteral("token"),              QStringLiteral("token-puzzle")},
        {QStringLiteral("parser"),             QStringLiteral("pipeline")},
        {QStringLiteral("ast"),                QStringLiteral("ast-toy")},
        {QStringLiteral("module-system"),      QStringLiteral("pipeline")},
        // IR / 后端 → ir-transform / backend-compare
        {QStringLiteral("ir"),                 QStringLiteral("ir-transform")},
        {QStringLiteral("ir-module"),          QStringLiteral("ir-transform")},
        {QStringLiteral("lowering"),           QStringLiteral("ir-transform")},
        {QStringLiteral("ssa"),                QStringLiteral("ir-transform")},
        {QStringLiteral("bytecode"),           QStringLiteral("bytecode-trace")},
        {QStringLiteral("bytecode-chunk"),     QStringLiteral("bytecode-trace")},
        {QStringLiteral("stack-vm"),           QStringLiteral("backend-compare")},
        {QStringLiteral("register-vm"),        QStringLiteral("backend-compare")},
        {QStringLiteral("triple-backend"),     QStringLiteral("backend-compare")},
        {QStringLiteral("integer-division"),   QStringLiteral("backend-compare")},
        {QStringLiteral("short-circuit"),      QStringLiteral("backend-compare")},
        {QStringLiteral("type-annotation"),    QStringLiteral("syntax-explorer")},
        // 运行时栈 / 闭包 / 内存 → call-stack / closure-inspector / memory-model
        {QStringLiteral("opstack"),            QStringLiteral("vm-sandbox")},
        {QStringLiteral("stack-frame"),        QStringLiteral("call-stack")},
        {QStringLiteral("environment"),        QStringLiteral("variable-inspector")},
        {QStringLiteral("field-slot"),         QStringLiteral("variable-inspector")},
        {QStringLiteral("closure"),            QStringLiteral("closure-inspector")},
        {QStringLiteral("closure-data"),       QStringLiteral("closure-inspector")},
        {QStringLiteral("upvalue"),            QStringLiteral("closure-inspector")},
        {QStringLiteral("bound-instance"),     QStringLiteral("closure-inspector")},
        {QStringLiteral("nan-boxing"),         QStringLiteral("memory-model")},
        {QStringLiteral("cow"),                QStringLiteral("memory-model")},
        {QStringLiteral("refcounted"),         QStringLiteral("memory-model")},
        {QStringLiteral("gc"),                 QStringLiteral("memory-model")},
        {QStringLiteral("mark-sweep"),         QStringLiteral("memory-model")},
    };
    auto it = kTermToPanel.constFind(termId);
    if (it == kTermToPanel.constEnd()) return QString();
    return it.value();
}
