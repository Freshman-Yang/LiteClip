#include "core/AsyncWorker.h"

#include <objbase.h>
#include <future>
#include <utility>

AsyncWorker::~AsyncWorker() {
    Stop();
}

bool AsyncWorker::Start(
    HWND notifyHwnd,
    UINT completionMsg,
    std::function<bool()> onInit,
    std::function<void()> onShutdown,
    std::function<void()> onTaskError
) {
    Stop();

    notifyHwnd_ = notifyHwnd;
    completionMsg_ = completionMsg;
    stopping_ = false;

    workerDone_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!workerDone_) {
        return false;
    }

    std::promise<bool> ready;
    auto result = ready.get_future();

    worker_ = std::thread([this,
                           onInit = std::move(onInit),
                           onShutdown = std::move(onShutdown),
                           onTaskError = std::move(onTaskError),
                           ready = std::move(ready)]() mutable {
        // WIC 在后台 MTA 创建、使用和释放；OLE 拖拽仍留在 UI STA。
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool initialized = SUCCEEDED(hr) && (!onInit || onInit());
        ready.set_value(initialized);

        if (initialized) {
            for (;;) {
                std::function<void()> work;
                {
                    std::unique_lock lock(workMutex_);
                    workReady_.wait(lock, [this] { return stopping_ || !workQueue_.empty(); });
                    if (stopping_ && workQueue_.empty()) {
                        break;
                    }
                    work = std::move(workQueue_.front());
                    workQueue_.pop_front();
                }
                try {
                    work();
                } catch (...) {
                    if (onTaskError) {
                        CompleteWork(onTaskError);
                    }
                }
            }
            if (onShutdown) {
                onShutdown();
            }
        }

        if (SUCCEEDED(hr)) {
            CoUninitialize();
        }
        SetEvent(workerDone_);
    });

    if (!result.get()) {
        Stop();
        return false;
    }
    return true;
}

void AsyncWorker::Stop() {
    stopping_ = true;
    workReady_.notify_one();
    if (worker_.joinable()) {
        // Clipboard API 可能同步通知拥有者（包括本窗口）；等待时只泵送
        // 跨线程 SendMessage，不执行新 UI 操作或后台完成回调。
        while (WaitForSingleObject(workerDone_, 0) == WAIT_TIMEOUT) {
            MsgWaitForMultipleObjects(1, &workerDone_, FALSE, INFINITE, QS_SENDMESSAGE);
            MSG msg{};
            PeekMessageW(&msg, nullptr, WM_NULL, WM_NULL, PM_NOREMOVE);
        }
        worker_.join();
    }
    if (workerDone_) {
        CloseHandle(workerDone_);
        workerDone_ = nullptr;
    }
    std::lock_guard lock(workMutex_);
    workQueue_.clear();
    completions_.clear();
}

void AsyncWorker::QueueWork(std::function<void()> work) {
    std::lock_guard lock(workMutex_);
    if (stopping_) {
        return;
    }
    workQueue_.push_back(std::move(work));
    workReady_.notify_one();
}

void AsyncWorker::CompleteWork(std::function<void()> completion) {
    std::lock_guard lock(workMutex_);
    if (stopping_) {
        return;
    }
    completions_.push_back(std::move(completion));
    if (notifyHwnd_ && completionMsg_ != 0) {
        PostMessageW(notifyHwnd_, completionMsg_, 0, 0);
    }
}

void AsyncWorker::DrainCompletions() {
    std::deque<std::function<void()>> ready;
    {
        std::lock_guard lock(workMutex_);
        ready.swap(completions_);
    }
    for (auto& completion : ready) {
        completion();
    }
}
