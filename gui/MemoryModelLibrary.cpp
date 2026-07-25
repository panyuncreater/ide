// ============================================================
// MemoryModelLibrary.cpp — 内存模型教学场景静态库（P3-20 拆分自 MemoryModelPanel.cpp）
// ------------------------------------------------------------
// 本文件仅含静态教学场景数据：NaN-boxing 编码示例、RefCounted 场景、
// GcManager 阶段说明、GC 动画阶段说明、堆对象类型说明。
// 数据无副作用、无依赖面板状态，单独编译可加速增量构建。
//
// 拆分动机（P3-20）：MemoryModelPanel.cpp 原 1769 行，其中静态库实现
// 占约 320 行（约 18%）。拆分后 MemoryModelPanel.cpp 降至约 1450 行，
// 聚焦于面板 UI 与交互逻辑；本文件负责纯数据。
// ============================================================

#include "gui/MemoryModelPanel.h"

#include "interpreter/NaNBox.h"

#include <cstdint>
#include <vector>

// ============================================================
// MemoryModelLibrary — 静态教学场景库
// ============================================================

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
        {"DictData", "🏗️ 字典容器（持有 Value::DictMap entries，键支持 string/int/bool/float）"},
        {"InstanceData", "🏗️ 类实例（持有 className + fields 字段表）"},
        {"StringData", "🏗️ 字符串堆数据（持有 std::string + UTF-8 码位缓存 + isAscii 标志缓存）"},
        {"ClosureData", "🏗️ 闭包数据（持有 env weak_ptr + params + capturedVars + body）"},
        {"BoundMethodData", "🏗️ 绑定的方法（实例 + 方法指针的绑定载体）"},
    };
    return kTypes;
}
