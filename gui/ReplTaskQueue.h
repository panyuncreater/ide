/**
 * @file ReplTaskQueue.h
 * @brief 串行任务队列：所有任务固定在同一常驻线程执行。
 *
 * REPL 会话用本队列绑定单一执行线程，使协程 fiber 的 ownerThread 跨行恒定
 * （§3.8 防御禁止跨线程恢复；逐行 std::async 会为每行新建线程，必然误伤）。
 *
 * 线程模型：start/post 由宿主（GUI 主线程）调用，任务体在队列线程执行；
 * stop 会等待正在执行的任务结束后 join，未开始的排队任务被丢弃。
 */
#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

class ReplTaskQueue {
public:
    ReplTaskQueue() = default;
    ~ReplTaskQueue() { stop(); }

    ReplTaskQueue(const ReplTaskQueue&) = delete;
    ReplTaskQueue& operator=(const ReplTaskQueue&) = delete;

    /// 启动常驻线程（幂等）。线程创建失败返回 false。
    bool start() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (thread_.joinable())
            return true;
        if (stopping_)
            return false;
        try {
            thread_ = std::thread([this] { run(); });
        } catch (...) {
            return false;
        }
        workerId_ = thread_.get_id();
        return true;
    }

    /// 入队任务；未启动或已停止返回 false。
    bool post(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_ || !thread_.joinable())
                return false;
            tasks_.push_back(std::move(task));
        }
        cv_.notify_one();
        return true;
    }

    /// 停止线程并等待当前任务结束；排队中的任务被丢弃（其 future/promise 置为 broken）。
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!thread_.joinable())
                return;
            stopping_ = true;
            tasks_.clear();
        }
        cv_.notify_all();
        thread_.join();
    }

    /// 常驻线程 id（未启动返回默认 id）。
    std::thread::id workerId() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return workerId_;
    }

private:
    void run() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
                if (stopping_)
                    return;
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            task();
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> tasks_;
    std::thread thread_;
    std::thread::id workerId_{};
    bool stopping_ = false;
};
