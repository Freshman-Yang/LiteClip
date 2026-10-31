#pragma once

#include "platform/Win32Common.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

class AsyncWorker {
public:
    AsyncWorker() = default;
    ~AsyncWorker();

    AsyncWorker(const AsyncWorker&) = delete;
    AsyncWorker& operator=(const AsyncWorker&) = delete;

    bool Start(
        HWND notifyHwnd,
        UINT completionMsg,
        std::function<bool()> onInit,
        std::function<void()> onShutdown = {},
        std::function<void()> onTaskError = {}
    );
    void Stop();

    void QueueWork(std::function<void()> work);
    void CompleteWork(std::function<void()> completion);
    void DrainCompletions();

    bool IsStopping() const noexcept { return stopping_.load(); }

private:
    HWND notifyHwnd_ = nullptr;
    UINT completionMsg_ = 0;
    std::thread worker_;
    HANDLE workerDone_ = nullptr;
    std::mutex workMutex_;
    std::condition_variable workReady_;
    std::deque<std::function<void()>> workQueue_;
    std::deque<std::function<void()>> completions_;
    std::atomic<bool> stopping_{ false };
};
