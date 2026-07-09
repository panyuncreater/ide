// ============================================================
// MemoryModelPanel.cpp — 内存模型可视化面板实现（第二波 P0-2）
// ============================================================

#include "gui/MemoryModelPanel.h"
#include "app/IdeController.h"
#include "gui/MarkdownRenderer.h"
#include "gui/PanelAnimator.h"
#include "interpreter/GcManager.h"
#include "interpreter/NaNBox.h"
#include "interpreter/RefCounted.h"
#include "interpreter/Value.h"

#include <QApplication>
#include <QColor>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "Label.h"      // QFluentKit（StrongBodyLabel）
#include "PushButton.h" // QFluentKit（PrimaryPushButton）

// ============================================================
// MemoryModelLibrary — 静态教学场景库
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

} // namespace

/// 返回 NaN-boxing 编码教学示例（静态单例）：int48 内联（正/负/边界）、
/// float64、bool、null、指针（堆对象）六类。每个示例在构造时调用 NaNBox
/// 真实编码，记录 rawBits 与解码值，供位图表格与说明区展示「8 字节 Value 编码」。
const std::vector<NaNBoundingBox>& MemoryModelLibrary::nanBoxExamples() {
    static const std::vector<NaNBoundingBox> kExamples = {
        // int48 内联（正值）
        []() {
            NaNBox box = NaNBox::fromInt(42);
            NaNBoundingBox b;
            b.id = "int-inline-pos";
            b.title = "🔢 int 42 内联（int48 范围内）";
            b.description =
                "🔢 整数 42 在 int48 范围（|v| 小于 2^47）内，直接内联到 NaN-box 的低 48 位。"
                "高 16 位为 INT_TAG_BASE = 0x7FF8，标记类型为 VAL_INT。"
                "无需堆分配，Value 拷贝仅 8 字节赋值。"
                " **类比：** NaN-box 如同固定 8 字节的「万能收纳盒」，高 16 位是「类型标签」，低 48 位是「货物」。"
                "小整数 42 这种小件能直接装进盒子，不必去堆上单独开仓库，所以拷贝整型只复制这 8 字节。"
                " **输入输出：** 输入 `42`，输出 0x7FF8 开头的位模式（低 48 位存 42）。";
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
            b.title = "🔢 int -1 内联（int48 负值）";
            b.description =
                "🔢 整数 -1 以补码形式存储到低 48 位（全 1），"
                "解码时通过符号位扩展恢复 int64 值。"
                " **类比：** 负数就像把盒子里的货物按「补码规则」倒着装（全 1 表示 -1），取货时再按规则还原成 -1。"
                " **输入输出：** 输入 `-1`，输出低 48 位全 1，解码得 -1。";
            b.sourceExpr = "-1";
            b.bits = box.rawBits();
            b.intVal = -1;
            b.isInt = true;
            return b;
        }(),
        // int48 边界
        []() {
            int64_t boundary = (1LL << 46) - 1; // int48 最大正值
            NaNBox box = NaNBox::fromInt(boundary);
            NaNBoundingBox b;
            b.id = "int-inline-boundary";
            b.title = "🔢 int48 最大正值（2^46 - 1）";
            b.description =
                "🔢 int48 范围上限：|v| 小于 2^47。"
                "本例 v = 2^46 - 1 仍在范围内，仍内联存储。"
                "若 v 超出范围（|v| ≥ 2^47）则触发装箱为 BoxedIntData* 堆对象。"
                " **类比：** 盒子能装下的「小件」有大小上限，超过就要改寄大件、单独建仓库存放（BoxedIntData）。"
                " **输入输出：** 输入 `2^46 - 1` 仍内联；输入 `2^47` 则改为堆上装箱。";
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
            b.title = "🔢 float 3.14 直接存储（IEEE 754 double）";
            b.description = "🔢 浮点数直接以 IEEE 754 double 的 64 位原始位存储，"
                            "高 16 位是 exponent 字段。若 float 的位模式恰好落入 NaN-boxed tag 范围，"
                            "会被规范化为 NAN_BOXED_FLOAT_MARKER（罕见）。"
                            " **类比：** 浮点不像整数那样塞类型标签，而是原封不动把「双精度实数」整箱搬进去（高 16 "
                            "位是它自己的指数位）。"
                            " **输入输出：** 输入 `3.14`，输出即 IEEE 754 的 64 位位模式。";
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
            b.title = "🎭 bool true（BOOL_TAG_BASE）";
            b.description = "🎭 bool 类型用 BOOL_TAG_BASE = 0x7FF9 编码，低 48 位存储 0/1。"
                            "true 编码为 0x7FF9...0001，false 为 0x7FF9...0000。"
                            " **类比：** 布尔值像盒子里只放「0」或「1」两张小卡片，标签统一是 BOOL_TAG_BASE。"
                            " **输入输出：** 输入 `true`，输出 0x7FF9 开头、低 48 位为 1。";
            b.sourceExpr = "true";
            b.bits = box.rawBits();
            b.intVal = 1; // true
            b.isBool = true;
            return b;
        }(),
        // null
        []() {
            NaNBox box = NaNBox::null();
            NaNBoundingBox b;
            b.id = "null-value";
            b.title = "🎭 null（NULL_BITS）";
            b.description = "🎭 null 值编码为固定常量 NULL_BITS = 0x7FFA000000000000，"
                            "payload 全为 0。"
                            " **类比：** null 是「空盒子」专用款，标签和货物都固定为零，代表什么都没装。"
                            " **输入输出：** 输入 `null`，输出 0x7FFA000000000000。";
            b.sourceExpr = "null";
            b.bits = box.rawBits();
            b.isNull = true;
            return b;
        }(),
        // 指针类型（堆对象，演示 PTR_TAG_BASE，指针值仅用于演示）
        []() {
            // AUDIT-P2 fix: 原实现用栈上 int dummy 作为指针示例，IIFE 返回后 dummy
            // 出作用域，b.bits 持有悬垂栈地址（虽不 deref 无崩溃，但教学展示无效地址）。
            // 改用 static 变量保证生命周期，演示 PTR_TAG_BASE 编码的真实指针值。
            static int dummy = 0;
            NaNBox box = NaNBox::fromPtr(&dummy);
            NaNBoundingBox b;
            b.id = "ptr-string";
            b.title = "📦 string \"hello\" 装箱（PTR_TAG_BASE + 48 位指针）";
            b.description =
                "📦 字符串 「hello」 长度超过 ASCII 内联阈值，装箱为 StringData* 堆对象。"
                "高 16 位为 PTR_TAG_BASE = 0x7FFB，低 48 位为 StringData 对象的虚拟地址。"
                "x86-64 用户态虚拟地址空间为 48 位，可直接编码。"
                "本图示演示 PTR_TAG_BASE + 指针位模式，实际指针值会随运行而变化。"
                " **类比：** 大件货物放不进盒子，就改成在盒子里写一张「仓库提货单」（48 "
                "位指针），真正的东西存在堆仓库里。"
                " **输入输出：** 输入 `「hello」`（字符串字面量），输出 PTR_TAG_BASE 加上 StringData 的 48 位地址。";
            b.sourceExpr = "\"hello\"";
            b.bits = box.rawBits();
            b.isPointer = true;
            return b;
        }(),
    };
    return kExamples;
}

/// 返回引用计数 & COW 教学场景（静态单例）：数组基础生命周期、COW detach、
/// 字符串共享、实例字段引用。每个场景含按执行顺序的 refCount 变化步骤表
/// （动作 / refCount 值 / 备注），直观展示侵入式引用计数与写时复制语义。
const std::vector<RefCountScenario>& MemoryModelLibrary::refCountScenarios() {
    static const std::vector<RefCountScenario> kScenarios = {
        {"array-basic",
         "📦 数组基础生命周期",
         "📦 演示 var a = [1,2,3] 的构造、拷贝与释放。"
         "ArrayData 继承 RefCounted，构造时 refCount=1。"
         "var b = a 共享所有权（refCount=2），不进行深拷贝（COW 语义）。"
         "b 离开作用域时 release，refCount 降为 1；a 离开时 release，refCount=0 触发析构。"
         " **类比：** 就像几个人合租一套房，每多一个人住进来（addRef）计数加一，搬走一人（release）计数减一；"
         "最后一个人搬走，房子才退租拆除（析构）。"
         " **输入输出：** 输入 `var a=[1,2,3]; var b=a`，输出 a、b 共享同一 ArrayData，refCount 从 1 升到 2 再回到 0。",
         {
             {"var a = [1,2,3]", 1, "构造新 ArrayData，refCount=1"},
             {"var b = a", 2, "Value 拷贝：addRef → refCount=2（共享所有权，未深拷贝）"},
             {"（b 离开作用域）", 1, "Value 析构：release → refCount=1"},
             {"（a 离开作用域）", 0, "Value 析构：release → refCount=0，delete this"},
         }},
        {"cow-detach",
         "📦 COW 写时复制 detach",
         "📦 演示 b = a 之后修改 b 触发 COW detach。"
         "COW detach 在写前检查 refCount==1，否则深拷贝自身。"
         "本例 refCount=2 时调用 b.push(4)，触发 detach：先 release 原 refCount（→1），"
         "再深拷贝出新的 ArrayData（refCount=1），最后写入新对象。"
         " **类比：** 合租时想改造房间，得先「复印一份原稿」再改复印件，原住户 a 的房间保持原样（COW 写时复制）。"
         " **输入输出：** 输入 `b.push(4)`，输出 b 指向新数组 [1,2,3,4]，a 仍为 [1,2,3]。",
         {
             {"var a = [1,2,3]", 1, "构造新 ArrayData，refCount=1"},
             {"var b = a", 2, "共享所有权，refCount=2"},
             {"b.push(4)", 1, "COW detach：原 refCount-- (→1)，深拷贝出新对象 refCount=1，写入新对象"},
             {"（新对象中 b[3]=4）", 1, "深拷贝后修改不影响 a，a 仍是 [1,2,3]"},
         }},
        {"string-shared",
         "📦 字符串共享（无 COW）",
         "📦 字符串 StringData 同样使用 RefCounted，但字符串不可变，无需 COW detach。"
         "var s2 = s1 共享所有权，s2 += 'x' 实际是构造新 StringData 赋值给 s2。"
         " **类比：** 字符串像「只读的合租房」，谁都不能改原屋，要改就只能另租新房（构造新 StringData）并搬过去。"
         " **输入输出：** 输入 `s2 = s2 + 「!」`，输出 s2 指向新字符串 'hello!'，s1 仍是 'hello'。",
         {
             {"var s1 = \"hello\"", 1, "构造新 StringData，refCount=1"},
             {"var s2 = s1", 2, "共享所有权，refCount=2"},
             {"s2 = s2 + \"!\"", 1, "构造新 StringData（'hello!'），s2 旧值 release（→1）"},
         }},
        {"instance-fields",
         "📦 实例字段引用",
         "📦 实例 InstanceData 持有字段表，字段值是 Value（可能引用其它堆对象）。"
         "实例的 refCount 与字段值 refCount 相互独立。"
         " **类比：** 实例像一栋「带家具的房子」，家具（字段值）可能本身也是被合租的物件，"
         "房子和家具各有各的租赁计数，互不影响。"
         " **输入输出：** 输入 `var p=Point(); p.x=[1,2,3]; var q=p`，输出 p、q 共享实例（refCount "
         "2），其字段数组独立计数为 1。",
         {
             {"class Point { ... }", 0, "类定义不创建实例，仅注册到 classRegistry_"},
             {"var p = Point()", 1, "构造新 InstanceData，refCount=1"},
             {"p.x = [1,2,3]", 1, "p.x 字段持有 ArrayData（refCount=1）"},
             {"var q = p", 2, "InstanceData addRef → refCount=2"},
             {"（q 离开作用域）", 1, "InstanceData release → refCount=1（ArrayData 仍 refCount=1）"},
         }},
    };
    return kScenarios;
}

/// 返回 GcManager mark-sweep 阶段说明（静态单例）：注册 / 触发时机 / Mark /
/// Sweep / UAF 防护 / 已知限制。每阶段含 Markdown 说明，解释循环引用如何被
/// 标记-清除回收，作为第 3 子页的静态教学内容。
const std::vector<GcPhaseInfo>& MemoryModelLibrary::gcPhases() {
    static const std::vector<GcPhaseInfo> kPhases = {
        {"📝 1. 注册（registerTracked）",
         "📝 所有新建的 ArrayData / DictData / InstanceData 在构造函数中调用 "
         "GcManager::instance().registerTracked(this)，"
         "加入 tracked_ 列表与 aliveSet_。StringData/ClosureData 不注册（无循环引用风险）。"
         "注册为 O(1) 操作（vector::push_back + unordered_set::insert）。"
         " **类比：** "
         "就像去派出所「上户口」，新建的容器都要登记在册；只读的字符串和闭包没有互相牵绊的风险，不必登记。"},
        {"⏰ 2. 触发时机", "⏰ Interpreter::execute() 在 resetState 之后、runStatements 之前调用 collectCycle(空根集)。"
                           "此时上一轮残留的循环容器 refCount 大于 0 仍 aliveSet_，本轮新建容器尚未注册，安全。"
                           "执行期间不再触发（性能权衡：mark-sweep 开销 O(节点数+边数)，仅起点触发）。"
                           " **类比：** 就像每天开门营业前先扫一次地，营业中不再反复打扫，避免影响运行效率。"},
        {"🟢 3. Mark 阶段",
         "🟢 从 roots（globals / VM 栈 / 调用帧中的 Value）出发，递归 mark 所有可达容器节点。"
         "markValue 检查 Value 类型：ArrayData → 遍历 elements；DictData → 遍历 entries；"
         "InstanceData → 遍历 fields。递归标记直到所有可达节点 marked=true。"
         " **类比：** 像警察挨家挨户「查户口」，从已知活人（根集）出发，顺着人际关系找到的所有人都标记「在世」；"
         "找不到的空房子就是无人认领的孤岛。"},
        {"🧹 4. Sweep 阶段",
         "🧹 迭代 tracked_ 列表，对 aliveSet_ 仍在但 marked 未标记的节点（即不可达的循环孤岛）执行："
         "清空其子元素打破循环 → refCount 自然降为 0 → 节点析构 → onDestroyed 从 aliveSet_ 移除。"
         "存活节点重置 marked=false，为下一轮收集做准备。"
         " **类比：** "
         "查完户口后，凡是没有被标记「在世」的空房子，直接拆除并断开水电（清空子元素），让其自然退租销毁。"},
        {"🛡️ 5. UAF 防护",
         "🛡️ tracked_ 列表中的 RefCounted* 可能在 collectCycle 期间被析构（如 sweep 清空子元素后 refCount→0）。"
         "通过 aliveSet_ 区分存活对象与已释放的悬垂指针，避免迭代时访问已释放内存。"
         "析构钩子 onDestroyed 同步从 aliveSet_ 移除本指针，保证 aliveSet_ 与实际存活状态一致。"
         " **类比：** 拆除时要先核对「还在册名单」，避免误闯已经拆掉的危房（悬垂指针），保证施工安全。"},
        {"⚠️ 6. 已知限制",
         "⚠️ 环形容器泄漏：a.append(a) 形成自环，refCount ≥ 2 永不归零。"
         "GC 通过 mark-sweep 可回收（mark 时 a 已 marked，sweep 不会误清），"
         "但仅当 a 的根引用被丢弃时才会被回收。若 a 仍在 globals 中，GC 不会触碰。"
         " **类比：** "
         "就像两个人互相抓住对方、谁也不肯先松手（循环引用），光靠「最后一人搬走才退租」的引用计数永远解不开；"
         "mark-sweep 则像外部警察逐户核查，发现整圈人都与外界失联，就把这一圈一起回收。"},
    };
    return kPhases;
}

// ============================================================
// MemoryAnimLibrary — 第 4 子页"实时动画"场景静态库（P4-4）
// ============================================================

/// 返回第 4 子页「实时动画」的 GC 阶段说明（静态单例）：mark / sweep /
/// reset / idle 四阶段，各含描述与配色（蓝/红/绿/灰），用于动画状态展示。
const std::vector<MemoryAnimPhase>& MemoryAnimLibrary::gcAnimPhases() {
    static const std::vector<MemoryAnimPhase> kPhases = {
        {
            "mark",
            "🟢 Mark 阶段：从根集（VM 操作数栈、globals、调用帧）出发，"
            "递归标记所有可达的容器节点（ArrayData/DictData/InstanceData）。"
            "已标记节点 marked=true，避免重复遍历；环形引用在第二次相遇时跳过。",
            "#3079C0" // 蓝色：标记中
        },
        {
            "sweep",
            "🧹 Sweep 阶段：迭代 tracked_ 列表，对 aliveSet_ 仍在但 marked 未标记的节点"
            "（即不可达的循环孤岛）清空其子元素打破循环，refCount 自然降为 0 触发析构。"
            "存活节点重置 marked=false 为下一轮做准备。",
            "#C03030" // 红色：回收中
        },
        {
            "reset",
            "🔄 Reset 阶段：存活节点的 marked 标志复位为 false，aliveSet_ 与 tracked_ 保持一致。"
            "新一轮 collectCycle 触发前，所有节点状态干净，避免上一轮残留影响。",
            "#709030" // 绿色：复位
        },
        {
            "idle",
            "💤 Idle 阶段：GC 空闲。当前 collectCycle 已结束，下一次触发时机为"
            "Interpreter::execute() 起点（resetState 之后、runStatements 之前）。"
            "期间 refCount 正常管理对象生命周期，仅循环孤岛等待下轮回收。",
            "#909090" // 灰色：空闲
        },
    };
    return kPhases;
}

/// 返回堆对象类型说明（静态单例）：ArrayData / DictData / InstanceData /
/// StringData / ClosureData / BoundMethodData 六种，各含中文结构描述，
/// 作为第 4 子页「堆对象类型说明」列表的静态内容。
const std::vector<std::pair<std::string, std::string>>& MemoryAnimLibrary::heapObjectTypes() {
    static const std::vector<std::pair<std::string, std::string>> kTypes = {
        {"ArrayData", "🏗️ 数组容器（持有 std::vector<Value> elements）"},
        {"DictData", "🏗️ 字典容器（持有 std::unordered_map<string,Value> entries）"},
        {"InstanceData", "🏗️ 类实例（持有 className + fields 字段表）"},
        {"StringData", "🏗️ 字符串堆数据（持有 std::string + UTF-8 码位缓存）"},
        {"ClosureData", "🏗️ 闭包数据（持有 env weak_ptr + params + capturedVars + body）"},
        {"BoundMethodData", "🏗️ 绑定的方法（实例 + 方法指针的绑定载体）"},
    };
    return kTypes;
}

// ============================================================
// 第 4 子页匿名辅助（堆对象类型名 + 字段数）
// ============================================================

namespace {

/// 根据 ValueType 返回堆对象 C++ 结构名（用于"地址/类型"表格展示）
std::string heapStructName(const Value& v) {
    if (!v.isPointer())
        return "<scalar>";
    RefCounted* p = v.asPointer();
    if (!p)
        return "<null-ptr>";
    switch (p->type) {
    case ValueType::VAL_INT:
        return "BoxedIntData";
    case ValueType::VAL_STRING:
        return "StringData";
    case ValueType::VAL_ARRAY:
        return "ArrayData";
    case ValueType::VAL_DICT:
        return "DictData";
    case ValueType::VAL_INSTANCE:
        return "InstanceData";
    case ValueType::VAL_CLOSURE:
        return "ClosureData";
    case ValueType::VAL_NULL:
    case ValueType::VAL_FLOAT:
    case ValueType::VAL_BOOL:
        return "<scalar-in-heap>";
    }
    return "<unknown>";
}

/// 返回堆对象的字段数/元素数（用于"字段数/元素数"列展示）
/// 对 Array 返回元素数，Dict/Instance 返回字段数，Closure 返回 capturedVars 数，
/// String 返回字符数，BoxedInt 返回 1（仅 value 字段）。
size_t heapFieldCount(const Value& v) {
    if (!v.isPointer())
        return 0;
    RefCounted* p = v.asPointer();
    if (!p)
        return 0;
    switch (p->type) {
    case ValueType::VAL_INT:
        return 1; // BoxedIntData.value
    case ValueType::VAL_STRING:
        return static_cast<size_t>(v.codepointCount());
    case ValueType::VAL_ARRAY:
        return v.arrayVal().size();
    case ValueType::VAL_DICT:
        return v.dictVal().size();
    case ValueType::VAL_INSTANCE:
        return v.fields().size();
    case ValueType::VAL_CLOSURE:
        return v.capturedVars().size();
    case ValueType::VAL_NULL:
    case ValueType::VAL_FLOAT:
    case ValueType::VAL_BOOL:
        return 0;
    }
    return 0;
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

/// 构造面板：组装顶部 4 个子页按钮（NaN-boxing / 引用计数&COW /
/// GcManager / 实时动画）与 QStackedWidget，构建各子页、配置动画刷新定时器
/// （2s 安全网），并默认显示第一个子页。
MemoryModelPanel::MemoryModelPanel(QWidget* parent) : QWidget(parent) {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(4);

    // 顶部页签按钮（4 个子页）
    auto* pageBar = new QHBoxLayout;
    pageNanBoxBtn_ = new QPushButton(QString::fromUtf8("NaN-boxing 编码"), this);
    pageRefCountBtn_ = new QPushButton(QString::fromUtf8("引用计数 & COW"), this);
    pageGcBtn_ = new QPushButton(QString::fromUtf8("GcManager mark-sweep"), this);
    pageAnimBtn_ = new QPushButton(QString::fromUtf8("实时动画"), this);
    pageNanBoxBtn_->setCheckable(true);
    pageRefCountBtn_->setCheckable(true);
    pageGcBtn_->setCheckable(true);
    pageAnimBtn_->setCheckable(true);
    pageBar->addWidget(pageNanBoxBtn_);
    pageBar->addWidget(pageRefCountBtn_);
    pageBar->addWidget(pageGcBtn_);
    pageBar->addWidget(pageAnimBtn_);
    pageBar->addStretch();
    mainLayout->addLayout(pageBar);

    stack_ = new QStackedWidget(this);
    mainLayout->addWidget(stack_, 1);

    auto* pageNanBox = new QWidget(this);
    auto* pageRefCount = new QWidget(this);
    auto* pageGc = new QWidget(this);
    auto* pageAnim = new QWidget(this);
    buildNanBoxPage(pageNanBox);
    buildRefCountPage(pageRefCount);
    buildGcPage(pageGc);
    buildAnimPage(pageAnim);
    stack_->addWidget(pageNanBox);
    stack_->addWidget(pageRefCount);
    stack_->addWidget(pageGc);
    stack_->addWidget(pageAnim);

    // OPT-1: 第 4 子页自动刷新定时器降频 500ms→2000ms，状态变更由 vmStateChanged
    // 监听器即时触发 refreshAnimState（见 setController）。QTimer 作为安全网。
    animTimer_ = new QTimer(this);
    animTimer_->setInterval(2000);
    animTimer_->setSingleShot(false);
    connect(animTimer_, &QTimer::timeout, this, [this]() { refreshAnimState(); });

    // 默认显示第一个页面
    pageNanBoxBtn_->setChecked(true);
    stack_->setCurrentIndex(0);

    connect(pageNanBoxBtn_, &QPushButton::clicked, this, [this]() {
        stack_->setCurrentIndex(0);
        pageNanBoxBtn_->setChecked(true);
        pageRefCountBtn_->setChecked(false);
        pageGcBtn_->setChecked(false);
        pageAnimBtn_->setChecked(false);
        PanelAnimator::slideInWidget(stack_->currentWidget());
    });
    connect(pageRefCountBtn_, &QPushButton::clicked, this, [this]() {
        stack_->setCurrentIndex(1);
        pageNanBoxBtn_->setChecked(false);
        pageRefCountBtn_->setChecked(true);
        pageGcBtn_->setChecked(false);
        pageAnimBtn_->setChecked(false);
        PanelAnimator::slideInWidget(stack_->currentWidget());
    });
    connect(pageGcBtn_, &QPushButton::clicked, this, [this]() {
        stack_->setCurrentIndex(2);
        pageNanBoxBtn_->setChecked(false);
        pageRefCountBtn_->setChecked(false);
        pageGcBtn_->setChecked(true);
        pageAnimBtn_->setChecked(false);
        refreshGcStats();
        PanelAnimator::slideInWidget(stack_->currentWidget());
    });
    connect(pageAnimBtn_, &QPushButton::clicked, this, [this]() {
        stack_->setCurrentIndex(3);
        pageNanBoxBtn_->setChecked(false);
        pageRefCountBtn_->setChecked(false);
        pageGcBtn_->setChecked(false);
        pageAnimBtn_->setChecked(true);
        refreshAnimState();
        PanelAnimator::slideInWidget(stack_->currentWidget());
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
void MemoryModelPanel::populateNanBoxDetail(int index) {
    const auto& items = MemoryModelLibrary::nanBoxExamples();
    if (index < 0 || index >= (int)items.size())
        return;
    const auto& b = items[index];

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
void MemoryModelPanel::refreshGcStats() {
    if (!gcTrackedCountLabel_)
        return;
    size_t tracked = GcManager::instance().trackedCount();
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
    // 统一样式：浅灰底 + 边框，便于视觉区分
    const char* labelStyle = "QLabel { background-color: #F5F5F5; padding: 4px 8px; "
                             "border: 1px solid #CCC; border-radius: 3px; }";
    for (auto* l :
         {animStatusLabel_, animOpCodeLabel_, animStackDepthLabel_, animFrameCountLabel_, animGcTrackedLabel_}) {
        l->setStyleSheet(QString::fromUtf8(labelStyle));
        l->setAlignment(Qt::AlignCenter);
    }
    statusbar->addWidget(animStatusLabel_);
    statusbar->addWidget(animOpCodeLabel_);
    statusbar->addWidget(animStackDepthLabel_);
    statusbar->addWidget(animFrameCountLabel_);
    statusbar->addWidget(animGcTrackedLabel_);
    layout->addLayout(statusbar);

    // ---- 中间堆对象表：4 列（地址 / 类型 / refCount / 字段数·元素数）----
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
    layout->addWidget(heapObjectTable_, 2);

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
    size_t tracked = GcManager::instance().trackedCount();
    animGcTrackedLabel_->setText(QString::fromUtf8("GC tracked：%1").arg(tracked));

    // ---- 收集堆对象：遍历 stack + globals 中的 Value，按地址去重 ----
    // 注意：使用裸 Value* 指向 stack/globals 中的元素，避免 Value 拷贝导致
    // useCount() 被自身临时引用膨胀（保持显示的 refCount 为真实值）。
    auto globals = controller_->getVmGlobals();

    std::vector<const Value*> heapValuePtrs;
    heapValuePtrs.reserve(stack.size() + globals.size());
    for (const auto& v : stack) {
        if (v.isPointer() && v.asPointer())
            heapValuePtrs.push_back(&v);
    }
    for (const auto& kv : globals) {
        if (kv.second.isPointer() && kv.second.asPointer())
            heapValuePtrs.push_back(&kv.second);
    }

    // 按地址去重（保留首次出现的 Value 引用）
    std::unordered_set<const void*> seenAddrs;
    std::vector<const Value*> uniqueValues;
    uniqueValues.reserve(heapValuePtrs.size());
    for (const Value* vp : heapValuePtrs) {
        const void* addr = static_cast<const void*>(vp->asPointer());
        if (seenAddrs.insert(addr).second) {
            uniqueValues.push_back(vp);
        }
    }

    // 按地址升序排序
    std::sort(uniqueValues.begin(), uniqueValues.end(), [](const Value* a, const Value* b) {
        return static_cast<const void*>(a->asPointer()) < static_cast<const void*>(b->asPointer());
    });

    // ---- 填充堆对象表 ----
    if (!heapObjectTable_)
        return;
    heapObjectTable_->setRowCount((int)uniqueValues.size());
    for (int i = 0; i < (int)uniqueValues.size(); ++i) {
        const Value* vp = uniqueValues[i];
        RefCounted* p = vp->asPointer();

        // 地址列
        heapObjectTable_->setItem(i, 0, new QTableWidgetItem(QString::fromUtf8(ptrToHex(p).c_str())));

        // 类型列
        heapObjectTable_->setItem(i, 1, new QTableWidgetItem(QString::fromUtf8(heapStructName(*vp).c_str())));

        // refCount 列
        auto* rcItem = new QTableWidgetItem(QString::number(p->useCount()));
        rcItem->setTextAlignment(Qt::AlignCenter);
        heapObjectTable_->setItem(i, 2, rcItem);

        // 字段数/元素数列
        heapObjectTable_->setItem(i, 3,
                                  new QTableWidgetItem(QString::number(static_cast<qulonglong>(heapFieldCount(*vp)))));
    }
}
