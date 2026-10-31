#pragma once

#include "platform/Win32Common.h"
#include "core/Types.h"
#include "core/AsyncWorker.h"
#include "ui/ThemeAndLayout.h"
#include "storage/StorageManager.h"
#include "clipboard/ClipboardMonitor.h"
#include "interaction/CaretLocator.h"
#include "interaction/InputInjector.h"
#include "interaction/OleDragDrop.h"
#include "ui/RendererD2D.h"
#include <functional>

class MainWindow {
public:
    MainWindow();
    ~MainWindow();

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    bool Initialize(HINSTANCE hInstance);
    void ShowPopup();
    void HidePopup();
    void TogglePopup();

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);

    void Render();
    const ClipItems& GetCurrentItems() const;
    void ClampScrollOffset();
    void EnsureValidSelection();
    void ScrollItemIntoView(uint64_t itemId);
    void MoveSelection(int delta);
    void PasteSelectedItem(bool plainTextOnly);
    void DeleteSelectedItem();
    void DeleteItemById(uint64_t targetId);
    void UpdateScrollFromCursorY(float dipY);

    void OnLButtonDown(int pxX, int pxY);
    void OnMouseMove(int pxX, int pxY);
    void OnLButtonUp(int pxX, int pxY);
    void OnRButtonUp(int pxX, int pxY);
    void OnMouseWheel(short delta);
    void OnDragScrollTimer();
    void OnClipboardUpdated();
    bool OnInternalDragOver(POINT screenPt);
    void OnInternalDragLeave();
    bool OnInternalDrop(POINT screenPt);
    void EndInternalPinnedDrag();
    bool StartWorker();
    void StopWorker();
    void QueueWork(std::function<void()> work);
    void CompleteWork(std::function<void()> completion);
    void DrainCompletions();
    void PublishItems();
    void PasteItem(uint64_t id, bool plainTextOnly);

    float PxToDip(int px) const noexcept;
    int DipToPx(float dip) const noexcept;

private:
    static constexpr wchar_t WINDOW_CLASS_NAME[] = L"LiteClip_PopupWindow_Class";

    HINSTANCE hInstance_ = nullptr;
    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    bool isVisible_ = false;
    bool trackingMouseLeave_ = false;

    StorageManager storage_;
    ClipboardMonitor clipMonitor_;
    InputInjector inputInjector_;
    RendererD2D renderer_;
    AsyncWorker worker_;
    ClipItems historyView_;
    ClipItems pinnedView_;
    std::filesystem::path dataDir_;
    bool capturePending_ = false;
    bool captureAgain_ = false;
    bool pastePending_ = false;
    DWORD retrySeq_ = 0;
    unsigned retryCount_ = 0;

    UIViewState state_{};

    // 鼠标点击与拖拽状态机
    HitTestResult downHit_{ HitRegionType::None, 0 };
    POINT downPt_{ 0, 0 };
    bool isMouseDown_ = false;
    bool dragStarted_ = false;
    bool isInternalPinnedDrag_ = false;
    bool dragScrollTimerActive_ = false;

    // 顶部拖拽栏移动窗口与滚动条拖拽状态
    bool isMovingWindow_ = false;
    bool isDraggingScrollbar_ = false;
    float scrollbarDragOffsetDip_ = 0.0f;
    POINT winMoveStartCursor_{ 0, 0 };
    POINT winMoveStartOrigin_{ 0, 0 };
};
