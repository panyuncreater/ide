// ============================================================
// R136 线程与并发原语测试套件
// ------------------------------------------------------------
// 验证 channel/mutex/rwlock/spawn 在四后端（Interpreter/StackVM/
// StackVM_IR/RegisterVM）上的语义一致性与边界场景处理。
//
// 覆盖点：
//   1. channel 基础：send/recv 同步通信、tryRecv 非阻塞、close 关闭
//   2. mutex 基础：lock/unlock/tryLock 互斥语义
//   3. rwlock 基础：readLock/readUnlock/writeLock/writeUnlock/tryReadLock/tryWriteLock
//   4. spawn + join：子线程执行闭包、主线程等待
//   5. spawn + channel：子线程通过 channel 与主线程通信
//   6. 同步对象相等性：同一 Inner 的 Value 相等
//   7. 错误处理：方法不支持/参数错误/重复 join/detach
//   8. spawn 参数传递：多参数闭包调用
//   9. 闭包捕获：spawn 子线程访问外层变量
//  10. detach：分离线程不再可 join
//
// 注意：spawn 子线程闭包调用通过 spawnMutex_ 序列化，实际用户级并发受限。
// 这里的测试主要验证语义正确性，不测真实并发性能。
// ============================================================

#include "compiler/Compiler.h"
#include "compiler/IR.h"
#include "compiler/RegisterVM.h"
#include "compiler/VM.h"
#include "interpreter/Interpreter.h"
#include "interpreter/Value.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <gtest/gtest.h>
#include <chrono>
#include <string>
#include <thread>

// ============================================================
// 辅助：四后端执行器
// ============================================================

static std::string runInterpreter(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    if (!ast)
        return "<parse-fail>";
    Interpreter interp;
    std::string out;
    interp.setOutputCallback([&](const std::string& s) { out += s; });
    try {
        interp.execute(*ast);
    } catch (const RuntimeError& e) {
        return out + "<runtime:" + std::string(e.what()) + ">";
    } catch (const std::exception& e) {
        return out + "<runtime:" + std::string(e.what()) + ">";
    }
    return out;
}

static std::string runStackVM(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    if (!ast)
        return "<parse-fail>";
    Compiler c;
    auto cr = c.compile(*ast);
    if (c.getDiagnostics().hasErrors())
        return "<compile:" + c.getLastError() + ">";
    VM vm;
    std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });
    vm.execute(cr);
    if (vm.hasError())
        return out + "<runtime:" + vm.getLastError() + ">";
    return out;
}

static std::string runStackVM_IR(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    if (!ast)
        return "<parse-fail>";
    Compiler c;
    c.setUseIR(true);
    auto cr = c.compile(*ast);
    if (c.getDiagnostics().hasErrors())
        return "<compile:" + c.getLastError() + ">";
    VM vm;
    std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });
    vm.execute(cr);
    if (vm.hasError())
        return out + "<runtime:" + vm.getLastError() + ">";
    return out;
}

static std::string runRegVM(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    if (!ast)
        return "<parse-fail>";
    Compiler c;
    c.setUseRegisterVM(true);
    c.compile(*ast);
    if (c.getDiagnostics().hasErrors())
        return "<compile:" + c.getLastError() + ">";
    RegisterVM vm;
    std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });
    vm.execute(c.getLastRegisterResult());
    if (vm.hasError())
        return out + "<runtime:" + vm.getLastError() + ">";
    return out;
}

/// 四后端一致性验证宏
#define EXPECT_FOUR_BACKENDS(src, expected)                                                                            \
    do {                                                                                                               \
        EXPECT_EQ(runInterpreter(src), expected) << "Interpreter failed";                                              \
        EXPECT_EQ(runStackVM(src), expected) << "StackVM failed";                                                      \
        EXPECT_EQ(runStackVM_IR(src), expected) << "StackVM_IR failed";                                                \
        EXPECT_EQ(runRegVM(src), expected) << "RegisterVM failed";                                                     \
    } while (0)

// ============================================================
// 1. channel 基础测试
// ============================================================

TEST(R136Concurrency, ChannelBasicSendRecv) {
    // 同步 send/recv（同线程：send 后立即 recv）
    std::string src = R"(
var ch = channel();
ch.send(42);
print(ch.recv());
)";
    EXPECT_FOUR_BACKENDS(src, "42");
}

TEST(R136Concurrency, ChannelTryRecvEmpty) {
    // tryRecv 在空通道上返回 null
    std::string src = R"(
var ch = channel();
var v = ch.tryRecv();
if (v == null) {
    print("empty");
} else {
    print("got");
}
)";
    EXPECT_FOUR_BACKENDS(src, "empty");
}

TEST(R136Concurrency, ChannelTryRecvAfterSend) {
    std::string src = R"(
var ch = channel();
ch.send(100);
var v = ch.tryRecv();
print(v);
)";
    EXPECT_FOUR_BACKENDS(src, "100");
}

TEST(R136Concurrency, ChannelCloseAndRecv) {
    // 关闭通道后 recv 返回 null
    std::string src = R"(
var ch = channel();
ch.send(1);
ch.close();
print(ch.recv());
print(ch.recv());
)";
    EXPECT_FOUR_BACKENDS(src, "1null");
}

TEST(R136Concurrency, ChannelSendAfterClose) {
    std::string src = R"(
var ch = channel();
ch.close();
ch.send(1);
)";
    EXPECT_FOUR_BACKENDS(src, "<runtime:channel.send: 通道已关闭>");
}

// ============================================================
// 2. mutex 基础测试
// ============================================================

TEST(R136Concurrency, MutexLockUnlock) {
    std::string src = R"(
var m = mutex();
m.lock();
print("locked");
m.unlock();
print("unlocked");
)";
    EXPECT_FOUR_BACKENDS(src, "lockedunlocked");
}

TEST(R136Concurrency, MutexTryLock) {
    std::string src = R"(
var m = mutex();
if (m.tryLock()) {
    print("acquired");
    m.unlock();
} else {
    print("failed");
}
)";
    EXPECT_FOUR_BACKENDS(src, "acquired");
}

// ============================================================
// 3. rwlock 基础测试
// ============================================================

TEST(R136Concurrency, RwLockReadLock) {
    std::string src = R"(
var rw = rwlock();
rw.readLock();
print("read");
rw.readUnlock();
print("done");
)";
    EXPECT_FOUR_BACKENDS(src, "readdone");
}

TEST(R136Concurrency, RwLockWriteLock) {
    std::string src = R"(
var rw = rwlock();
rw.writeLock();
print("write");
rw.writeUnlock();
print("done");
)";
    EXPECT_FOUR_BACKENDS(src, "writedone");
}

TEST(R136Concurrency, RwLockTryReadLock) {
    std::string src = R"(
var rw = rwlock();
if (rw.tryReadLock()) {
    print("ok");
    rw.readUnlock();
} else {
    print("fail");
}
)";
    EXPECT_FOUR_BACKENDS(src, "ok");
}

TEST(R136Concurrency, RwLockTryWriteLock) {
    std::string src = R"(
var rw = rwlock();
if (rw.tryWriteLock()) {
    print("ok");
    rw.writeUnlock();
} else {
    print("fail");
}
)";
    EXPECT_FOUR_BACKENDS(src, "ok");
}

// ============================================================
// 4. spawn + join 测试
// ============================================================

TEST(R136Concurrency, SpawnAndJoin) {
    // spawn 启动子线程，join 等待结束
    std::string src = R"(
func worker() {
    print("worker");
}
var t = spawn(worker);
t.join();
print("done");
)";
    EXPECT_FOUR_BACKENDS(src, "workerdone");
}

TEST(R136Concurrency, SpawnWithArgs) {
    std::string src = R"(
func worker(n) {
    print(n);
}
var t = spawn(worker, 99);
t.join();
print("done");
)";
    EXPECT_FOUR_BACKENDS(src, "99done");
}

TEST(R136Concurrency, SpawnMultiArgs) {
    std::string src = R"(
func add(a, b) {
    print(a + b);
}
var t = spawn(add, 3, 4);
t.join();
)";
    EXPECT_FOUR_BACKENDS(src, "7");
}

TEST(R136Concurrency, SpawnIsJoinable) {
    std::string src = R"(
func worker() {
    print("work");
}
var t = spawn(worker);
if (t.isJoinable()) {
    print("joinable");
}
t.join();
if (t.isJoinable()) {
    print("still");
} else {
    print("done");
}
)";
    EXPECT_FOUR_BACKENDS(src, "joinableworkdone");
}

// ============================================================
// 5. spawn + channel 通信
// ============================================================

TEST(R136Concurrency, SpawnChannelCommunication) {
    // 子线程通过 channel 发送消息给主线程
    // 注：由于 spawnMutex_ 序列化，子线程闭包在主线程 join 时才真正执行
    std::string src = R"(
var ch = channel();
func sender() {
    ch.send("hello");
}
var t = spawn(sender);
t.join();
print(ch.recv());
)";
    EXPECT_FOUR_BACKENDS(src, "hello");
}

// ============================================================
// 6. 同步对象相等性测试
// ============================================================

TEST(R136Concurrency, ChannelIdentity) {
    // 同一 channel 的拷贝相等
    std::string src = R"(
var ch1 = channel();
var ch2 = ch1;
if (ch1 == ch2) {
    print("same");
} else {
    print("diff");
}
)";
    EXPECT_FOUR_BACKENDS(src, "same");
}

TEST(R136Concurrency, DifferentChannelNotEqual) {
    std::string src = R"(
var ch1 = channel();
var ch2 = channel();
if (ch1 == ch2) {
    print("same");
} else {
    print("diff");
}
)";
    EXPECT_FOUR_BACKENDS(src, "diff");
}

TEST(R136Concurrency, MutexIdentity) {
    std::string src = R"(
var m1 = mutex();
var m2 = m1;
if (m1 == m2) {
    print("same");
} else {
    print("diff");
}
)";
    EXPECT_FOUR_BACKENDS(src, "same");
}

// ============================================================
// 7. 错误处理测试
// ============================================================

TEST(R136Concurrency, ChannelRecvWrongArgs) {
    std::string src = R"(
var ch = channel();
ch.recv(1);
)";
    // 四后端错误消息可能略有差异（含参数个数），统一前缀
    auto check = [](const std::string& s) {
        return s.find("channel.recv") != std::string::npos && s.find("0") != std::string::npos;
    };
    EXPECT_TRUE(check(runInterpreter(src)));
    EXPECT_TRUE(check(runStackVM(src)));
    EXPECT_TRUE(check(runStackVM_IR(src)));
    EXPECT_TRUE(check(runRegVM(src)));
}

TEST(R136Concurrency, UnknownMethodOnChannel) {
    std::string src = R"(
var ch = channel();
ch.foo();
)";
    auto check = [](const std::string& s) { return s.find("不支持方法") != std::string::npos; };
    EXPECT_TRUE(check(runInterpreter(src)));
    EXPECT_TRUE(check(runStackVM(src)));
    EXPECT_TRUE(check(runStackVM_IR(src)));
    EXPECT_TRUE(check(runRegVM(src)));
}

TEST(R136Concurrency, SpawnNonFunction) {
    std::string src = R"(
var x = 42;
spawn(x);
)";
    auto check = [](const std::string& s) { return s.find("函数") != std::string::npos; };
    EXPECT_TRUE(check(runInterpreter(src)));
    EXPECT_TRUE(check(runStackVM(src)));
    EXPECT_TRUE(check(runStackVM_IR(src)));
    EXPECT_TRUE(check(runRegVM(src)));
}

TEST(R136Concurrency, DoubleJoinFails) {
    std::string src = R"(
func w() {}
var t = spawn(w);
t.join();
t.join();
)";
    auto check = [](const std::string& s) { return s.find("已 join") != std::string::npos; };
    EXPECT_TRUE(check(runInterpreter(src)));
    EXPECT_TRUE(check(runStackVM(src)));
    EXPECT_TRUE(check(runStackVM_IR(src)));
    EXPECT_TRUE(check(runRegVM(src)));
}

// ============================================================
// 8. spawn 闭包捕获测试
// ============================================================

TEST(R136Concurrency, SpawnClosureCapture) {
    // spawn 子线程捕获外层变量
    std::string src = R"(
var msg = "captured";
func worker() {
    print(msg);
}
var t = spawn(worker);
t.join();
)";
    EXPECT_FOUR_BACKENDS(src, "captured");
}

TEST(R136Concurrency, SpawnClosureCaptureAndArg) {
    std::string src = R"(
var prefix = ">";
func worker(n) {
    print(prefix + n);
}
var t = spawn(worker, 5);
t.join();
)";
    EXPECT_FOUR_BACKENDS(src, ">5");
}

// ============================================================
// 9. detach 测试
// ============================================================

TEST(R136Concurrency, SpawnDetach) {
    std::string src = R"(
func w() {
    print("detached");
}
var t = spawn(w);
t.detach();
if (t.isJoinable()) {
    print("joinable");
} else {
    print("not-joinable");
}
)";
    // detach 后 isJoinable 返回 false
    // 注意：detach 的子线程可能尚未执行，输出可能不包含 "detached"
    // 但 isJoinable 必须返回 false
    auto check = [](const std::string& s) { return s.find("not-joinable") != std::string::npos; };
    EXPECT_TRUE(check(runInterpreter(src)));
    EXPECT_TRUE(check(runStackVM(src)));
    EXPECT_TRUE(check(runStackVM_IR(src)));
    EXPECT_TRUE(check(runRegVM(src)));
}

TEST(R136Concurrency, DetachThenJoinFails) {
    std::string src = R"(
func w() {}
var t = spawn(w);
t.detach();
t.join();
)";
    auto check = [](const std::string& s) { return s.find("detach") != std::string::npos; };
    EXPECT_TRUE(check(runInterpreter(src)));
    EXPECT_TRUE(check(runStackVM(src)));
    EXPECT_TRUE(check(runStackVM_IR(src)));
    EXPECT_TRUE(check(runRegVM(src)));
}

// ============================================================
// 10. 类型测试
// ============================================================

TEST(R136Concurrency, TypeName) {
    std::string src = R"(
var ch = channel();
var m = mutex();
var rw = rwlock();
print(type(ch));
print(type(m));
print(type(rw));
)";
    EXPECT_FOUR_BACKENDS(src, "channelmutexrwlock");
}

TEST(R136Concurrency, ToStringForm) {
    std::string src = R"(
var ch = channel();
print(str(ch));
)";
    EXPECT_FOUR_BACKENDS(src, "<channel>");
}

// ============================================================
// 11. channel FIFO 顺序测试
// ============================================================

TEST(R136Concurrency, ChannelFIFOOrder) {
    std::string src = R"(
var ch = channel();
ch.send(1);
ch.send(2);
ch.send(3);
print(ch.recv());
print(ch.recv());
print(ch.recv());
)";
    EXPECT_FOUR_BACKENDS(src, "123");
}

// ============================================================
// 12. channel 引用语义测试
// ============================================================

TEST(R136Concurrency, ChannelSharedViaCopy) {
    // 两个变量指向同一通道，一个 send 另一个 recv
    std::string src = R"(
var ch1 = channel();
var ch2 = ch1;
ch1.send("msg");
print(ch2.recv());
)";
    EXPECT_FOUR_BACKENDS(src, "msg");
}
