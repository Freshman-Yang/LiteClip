#include "ui/MainWindow.h"
#include "ui/HotkeyConflictDialog.h"
#include "platform/I18n.h"

#include <windowsx.h>
#include <shellscalingapi.h>
#include <dwmapi.h>
#include <algorithm>
#include <cmath>
#include <utility>

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

MainWindow::MainWindow() = default;
MainWindow::~MainWindow() { StopWorker(); }

bool MainWindow::StartWorker() {
    return worker_.Start(
        hwnd_,
        WM_APP_WORK_COMPLETE,
        [this] { return clipMonitor_.Start(hwnd_, false); },
        [this] {
            clipMonitor_.Stop();
            storage_.ClearAll(ActiveTab::History);
        },
        [this] {
            capturePending_ = false;
            pastePending_ = false;
            MessageBoxW(hwnd_, I18n::Get().errWorkerFailed, L"LiteClip", MB_OK | MB_ICONERROR);
        }
    );
}

void MainWindow::StopWorker() {
    worker_.Stop();
}

void MainWindow::QueueWork(std::function<void()> work) {
    worker_.QueueWork(std::move(work));
}

void MainWindow::CompleteWork(std::function<void()> completion) {
    worker_.CompleteWork(std::move(completion));
}

void MainWindow::DrainCompletions() {
    worker_.DrainCompletions();
}

void MainWindow::PublishItems() {
    if (worker_.IsStopping()) return;
    ClipItems history = storage_.GetHistoryItems();
    ClipItems pinned = storage_.GetPinnedItems();
    CompleteWork([this, history = std::move(history), pinned = std::move(pinned)]() mutable {
        historyView_ = std::move(history);
        pinnedView_ = std::move(pinned);
        renderer_.InvalidateLayoutCache();
        EnsureValidSelection();
        ClampScrollOffset();
        if (isVisible_ && !isMouseDown_ && !isDraggingScrollbar_ && !isMovingWindow_) {
            POINT cursorPt{};
            if (GetCursorPos(&cursorPt) && ScreenToClient(hwnd_, &cursorPt)) {
                const HitTestResult hit = renderer_.HitTest(
                    PxToDip(cursorPt.x),
                    PxToDip(cursorPt.y),
                    GetCurrentItems(),
                    state_);
                state_.hoveredItemId = hit.itemId;
                state_.hoveredRegion = hit.region;
            }
        }
        Render();
    });
}

void MainWindow::PasteItem(uint64_t id, bool plainTextOnly) {
    if (pastePending_) return;
    const HWND target = GetForegroundWindow();
    if (!target || target == hwnd_) return;
    pastePending_ = true;
    QueueWork([this, id, plainTextOnly, target] {
        if (worker_.IsStopping()) return;
        bool success = false;
        DWORD pasteSeq = 0;
        if (const ClipItem* item = storage_.FindItemById(id)) {
            if (!plainTextOnly && ClipboardMonitor::IsStillInSystemClipboard(item->clipboardSeqNum)) {
                success = true;
                pasteSeq = item->clipboardSeqNum;
            } else {
                RawClipboardPayload payload;
                if (storage_.LoadItemPayload(id, payload, plainTextOnly) && !worker_.IsStopping() &&
                    GetForegroundWindow() == target) {
                    success = clipMonitor_.WriteToClipboard(hwnd_, payload, plainTextOnly, target);
                    if (success) pasteSeq = GetClipboardSequenceNumber();
                }
            }
        }
        CompleteWork([this, target, success, pasteSeq] {
            pastePending_ = false;
            if (success && GetForegroundWindow() == target &&
                ClipboardMonitor::IsStillInSystemClipboard(pasteSeq)) {
                HidePopup();
                inputInjector_.SendPasteCommand(target);
            }
        });
    });
}

float MainWindow::PxToDip(int px) const noexcept {
    return static_cast<float>(px) * 96.0f / static_cast<float>(dpi_);
}

int MainWindow::DipToPx(float dip) const noexcept {
    return MulDiv(static_cast<int>(std::lround(dip)), static_cast<int>(dpi_), 96);
}

bool MainWindow::Initialize(HINSTANCE hInstance) {
    hInstance_ = hInstance;

    if (!storage_.Initialize()) {
        return false;
    }
    historyView_ = storage_.GetHistoryItems();
    pinnedView_ = storage_.GetPinnedItems();
    dataDir_ = storage_.GetDataDir();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = MainWindow::WindowProc;
    wc.hInstance = hInstance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hInstance_, MAKEINTRESOURCEW(IDI_APP_ICON));
    if (!wc.hIcon) {
        wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = WINDOW_CLASS_NAME;

    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    dpi_ = GetDpiForSystem();
    const int initWidthPx = DipToPx(static_cast<float>(Layout::WINDOW_WIDTH));
    const int initHeightPx = DipToPx(static_cast<float>(Layout::WINDOW_HEIGHT));

    const DWORD exStyle = WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
    const DWORD style = WS_POPUP;

    hwnd_ = CreateWindowExW(
        exStyle,
        WINDOW_CLASS_NAME,
        L"LiteClip",
        style,
        0, 0, initWidthPx, initHeightPx,
        nullptr,
        nullptr,
        hInstance_,
        this
    );

    if (!hwnd_) {
        return false;
    }

    dpi_ = GetDpiForWindow(hwnd_);
    const int actualWidthPx = DipToPx(static_cast<float>(Layout::WINDOW_WIDTH));
    const int actualHeightPx = DipToPx(static_cast<float>(Layout::WINDOW_HEIGHT));
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, actualWidthPx, actualHeightPx,
                 SWP_NOMOVE | SWP_NOACTIVATE | SWP_HIDEWINDOW);

    // 开启 Windows 11 原生圆角与阴影 (DWMWCP_ROUND = 2)
    const DWORD cornerPreference = 2;
    DwmSetWindowAttribute(
        hwnd_,
        DWMWA_WINDOW_CORNER_PREFERENCE,
        &cornerPreference,
        sizeof(cornerPreference)
    );

    if (!renderer_.Initialize(hwnd_)) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }
    renderer_.OnResize(static_cast<UINT>(actualWidthPx), static_cast<UINT>(actualHeightPx), dpi_);

    if (!AddClipboardFormatListener(hwnd_)) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }

    // 注册全局快捷键（优先读取注册表 HKCU\Software\LiteClip 下的 Hotkey 配置，单次检测，若冲突则弹出选择窗口）
    const AppHotkey preferredHotkey = I18n::ReadHotkeyFromRegistry();
    const UINT hotkeyModifiers = (preferredHotkey == AppHotkey::AltV)
        ? (MOD_ALT | MOD_NOREPEAT)
        : (MOD_WIN | MOD_NOREPEAT);

    bool hotkeyRegistered = (RegisterHotKey(hwnd_, HOTKEY_ID_WIN_V, hotkeyModifiers, 'V') != FALSE);
    if (!hotkeyRegistered) {
        const bool initialAltVOccupied = (preferredHotkey == AppHotkey::AltV);
        hotkeyRegistered = HotkeyConflictDialog::Show(hInstance_, hwnd_, initialAltVOccupied);
    }

    if (!hotkeyRegistered) {
        RemoveClipboardFormatListener(hwnd_);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }

    if (!StartWorker()) {
        RemoveClipboardFormatListener(hwnd_);
        UnregisterHotKey(hwnd_, HOTKEY_ID_WIN_V);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }

    OleDragDrop::RegisterInternalDropTarget(
        hwnd_,
        InternalDropTargetCallbacks{
            [this](POINT screenPt) { return OnInternalDragOver(screenPt); },
            [this](POINT screenPt) { return OnInternalDragOver(screenPt); },
            [this]() { OnInternalDragLeave(); },
            [this](POINT screenPt) { return OnInternalDrop(screenPt); }
        }
    );

    return true;
}

void MainWindow::ShowPopup() {
    state_.scrollOffsetY = 0.0f;
    state_.hoveredItemId = 0;
    state_.hoveredRegion = HitRegionType::None;
    const ClipItems& currentItems = GetCurrentItems();
    state_.selectedItemId = currentItems.empty() ? 0 : currentItems.front().id;
    isMouseDown_ = false;
    dragStarted_ = false;
    isMovingWindow_ = false;
    isDraggingScrollbar_ = false;
    isVisible_ = false;

    const UINT oldWindowDpi = GetDpiForWindow(hwnd_);
    dpi_ = oldWindowDpi;

    // 1. 先探测呼出位置所在的显示器，通过 GetDpiForMonitor 获取目标显示器的真实 DPI
    const int probeW = DipToPx(static_cast<float>(Layout::WINDOW_WIDTH));
    const int probeH = DipToPx(static_cast<float>(Layout::WINDOW_HEIGHT));
    const POINT probePt = inputInjector_.CalculatePopupPosition(probeW, probeH);
    HMONITOR targetMonitor = MonitorFromPoint(probePt, MONITOR_DEFAULTTONEAREST);

    UINT targetDpiX = oldWindowDpi;
    UINT targetDpiY = oldWindowDpi;
    if (targetMonitor &&
        SUCCEEDED(GetDpiForMonitor(targetMonitor, MDT_EFFECTIVE_DPI, &targetDpiX, &targetDpiY)) &&
        targetDpiX > 0) {
        dpi_ = targetDpiX;
    }

    // 2. 按目标显示器真实 DPI 计算物理像素宽高，并进行屏幕工作区边界钳制
    const UINT targetDpi = dpi_;
    int winW = DipToPx(static_cast<float>(Layout::WINDOW_WIDTH));
    int winH = DipToPx(static_cast<float>(Layout::WINDOW_HEIGHT));
    POINT pt = CaretLocator::CalculatePopupPosition(winW, winH);

    SetWindowPos(
        hwnd_,
        HWND_TOPMOST,
        pt.x,
        pt.y,
        winW,
        winH,
        SWP_SHOWWINDOW | SWP_NOACTIVATE
    );

    // 3. 兜底校验：对比快照 targetDpi / oldWindowDpi（避免依赖被 WM_DPICHANGED 同步修改的成员变量 dpi_）
    const UINT actualDpi = GetDpiForWindow(hwnd_);
    if (actualDpi != targetDpi || actualDpi != oldWindowDpi) {
        dpi_ = actualDpi;
        winW = DipToPx(static_cast<float>(Layout::WINDOW_WIDTH));
        winH = DipToPx(static_cast<float>(Layout::WINDOW_HEIGHT));
        pt = CaretLocator::CalculatePopupPosition(winW, winH);
        SetWindowPos(
            hwnd_,
            HWND_TOPMOST,
            pt.x,
            pt.y,
            winW,
            winH,
            SWP_SHOWWINDOW | SWP_NOACTIVATE
        );
    }

    renderer_.OnResize(static_cast<UINT>(winW), static_cast<UINT>(winH), dpi_);
    ClampScrollOffset();

    isVisible_ = true;
    inputInjector_.SetPopupMonitoring(hwnd_, true);
    Render();
}

void MainWindow::HidePopup() {
    if (!isVisible_ && !IsWindowVisible(hwnd_)) {
        return;
    }

    isVisible_ = false;
    isMouseDown_ = false;
    dragStarted_ = false;
    isMovingWindow_ = false;
    isDraggingScrollbar_ = false;
    EndInternalPinnedDrag();

    if (GetCapture() == hwnd_) {
        ReleaseCapture();
    }

    inputInjector_.SetPopupMonitoring(hwnd_, false);

    ShowWindow(hwnd_, SW_HIDE);
    renderer_.ReleaseCachedBitmaps();
}

void MainWindow::TogglePopup() {
    if (isVisible_ && IsWindowVisible(hwnd_)) {
        HidePopup();
    } else {
        ShowPopup();
    }
}

const ClipItems& MainWindow::GetCurrentItems() const {
    return (state_.activeTab == ActiveTab::History)
        ? historyView_
        : pinnedView_;
}

void MainWindow::ClampScrollOffset() {
    const auto& items = GetCurrentItems();
    const float totalHeight = renderer_.CalculateTotalContentHeight(items);
    const float viewportHeight = Layout::VIEWPORT_HEIGHT;
    const float maxScroll = std::max(0.0f, totalHeight - viewportHeight);
    state_.scrollOffsetY = std::clamp(state_.scrollOffsetY, 0.0f, maxScroll);
}

void MainWindow::EnsureValidSelection() {
    const auto& items = GetCurrentItems();
    if (items.empty()) {
        state_.selectedItemId = 0;
        return;
    }
    if (state_.selectedItemId != 0) {
        for (const auto& item : items) {
            if (item.id == state_.selectedItemId) {
                return;
            }
        }
    }
    state_.selectedItemId = items.front().id;
}

void MainWindow::ScrollItemIntoView(uint64_t itemId) {
    const auto& items = GetCurrentItems();
    float itemTop = 0.0f;
    float itemBottom = 0.0f;
    if (!renderer_.GetItemVerticalBounds(itemId, items, itemTop, itemBottom)) {
        return;
    }

    const float viewTop = state_.scrollOffsetY;
    const float viewBottom = viewTop + Layout::VIEWPORT_HEIGHT;

    if (itemTop < viewTop + Layout::LIST_PAD_TOP) {
        state_.scrollOffsetY = itemTop - Layout::LIST_PAD_TOP;
    } else if (itemBottom > viewBottom - Layout::LIST_PAD_BOTTOM) {
        state_.scrollOffsetY = itemBottom + Layout::LIST_PAD_BOTTOM - Layout::VIEWPORT_HEIGHT;
    }
    ClampScrollOffset();
}

void MainWindow::MoveSelection(int delta) {
    if (!isVisible_ || isInternalPinnedDrag_ || dragStarted_ || delta == 0) {
        return;
    }
    const auto& items = GetCurrentItems();
    if (items.empty()) {
        state_.selectedItemId = 0;
        return;
    }

    size_t currentIndex = items.size();
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].id == state_.selectedItemId) {
            currentIndex = i;
            break;
        }
    }

    size_t nextIndex = 0;
    if (currentIndex >= items.size()) {
        nextIndex = (delta > 0) ? 0 : (items.size() - 1);
    } else if (delta < 0) {
        nextIndex = (currentIndex > 0) ? (currentIndex - 1) : 0;
    } else {
        nextIndex = (currentIndex + 1 < items.size()) ? (currentIndex + 1) : (items.size() - 1);
    }

    state_.selectedItemId = items[nextIndex].id;
    ScrollItemIntoView(state_.selectedItemId);
    Render();
}

void MainWindow::PasteSelectedItem(bool plainTextOnly) {
    if (!isVisible_ || isInternalPinnedDrag_ || dragStarted_) {
        return;
    }
    const auto& items = GetCurrentItems();
    if (items.empty()) {
        return;
    }
    EnsureValidSelection();
    const ClipItem* selectedItem = nullptr;
    for (const auto& item : items) {
        if (item.id == state_.selectedItemId) {
            selectedItem = &item;
            break;
        }
    }
    if (!selectedItem) {
        return;
    }
    if (plainTextOnly && !CanPlainTextPaste(*selectedItem)) {
        return;
    }
    PasteItem(selectedItem->id, plainTextOnly);
}

void MainWindow::DeleteItemById(uint64_t targetId) {
    if (targetId == 0) {
        return;
    }

    const auto& currentItems = GetCurrentItems();
    uint64_t nextSelectedId = state_.selectedItemId;
    if (state_.selectedItemId == targetId) {
        nextSelectedId = 0;
        for (size_t i = 0; i < currentItems.size(); ++i) {
            if (currentItems[i].id == targetId) {
                if (i + 1 < currentItems.size()) {
                    nextSelectedId = currentItems[i + 1].id;
                } else if (i > 0) {
                    nextSelectedId = currentItems[i - 1].id;
                }
                break;
            }
        }
    }

    auto eraseById = [targetId](ClipItems& list) {
        list.erase(
            std::remove_if(list.begin(), list.end(),
                [targetId](const ClipItem& it) { return it.id == targetId; }),
            list.end());
    };
    eraseById(historyView_);
    eraseById(pinnedView_);
    renderer_.InvalidateLayoutCache();

    state_.selectedItemId = nextSelectedId;
    EnsureValidSelection();
    ClampScrollOffset();
    if (state_.selectedItemId != 0) {
        ScrollItemIntoView(state_.selectedItemId);
    }
    Render();

    QueueWork([this, targetId] {
        if (!storage_.DeleteItem(targetId)) {
            CompleteWork([this] { MessageBoxW(hwnd_, I18n::Get().errDeleteFailed, L"LiteClip", MB_OK | MB_ICONERROR); });
        }
        PublishItems();
    });
}

void MainWindow::DeleteSelectedItem() {
    if (!isVisible_ || isInternalPinnedDrag_ || dragStarted_) {
        return;
    }
    const auto& items = GetCurrentItems();
    if (items.empty()) {
        return;
    }
    EnsureValidSelection();
    if (state_.selectedItemId != 0) {
        DeleteItemById(state_.selectedItemId);
    }
}

void MainWindow::UpdateScrollFromCursorY(float dipY) {
    const auto& items = GetCurrentItems();
    const float totalHeight = renderer_.CalculateTotalContentHeight(items);
    const ScrollbarMetrics metrics = CalculateScrollbarMetrics(totalHeight, state_.scrollOffsetY);
    if (!metrics.isVisible) {
        return;
    }

    const float targetThumbTop = dipY - scrollbarDragOffsetDip_;
    const float ratio = std::clamp(
        (targetThumbTop - metrics.trackTop) / metrics.scrollableTrackHeight,
        0.0f,
        1.0f);
    const float newScrollOffset = ratio * metrics.maxScroll;
    if (std::abs(newScrollOffset - state_.scrollOffsetY) > 0.01f) {
        state_.scrollOffsetY = newScrollOffset;
        ClampScrollOffset();
        Render();
    }
}

void MainWindow::Render() {
    if (!hwnd_ || !isVisible_) {
        return;
    }
    renderer_.Render(GetCurrentItems(), state_, dataDir_);
}

void MainWindow::OnLButtonDown(int pxX, int pxY) {
    const float dipX = PxToDip(pxX);
    const float dipY = PxToDip(pxY);

    const HitTestResult hit = renderer_.HitTest(dipX, dipY, GetCurrentItems(), state_);

    downHit_ = hit;
    downPt_ = { pxX, pxY };
    isMouseDown_ = true;
    dragStarted_ = false;
    isMovingWindow_ = false;
    isDraggingScrollbar_ = false;

    if (hit.itemId != 0 && hit.itemId != state_.selectedItemId) {
        state_.selectedItemId = hit.itemId;
        Render();
    }

    if (hit.region == HitRegionType::TopBarDragArea) {
        isMovingWindow_ = true;
        GetCursorPos(&winMoveStartCursor_);
        RECT rc{};
        GetWindowRect(hwnd_, &rc);
        winMoveStartOrigin_ = { rc.left, rc.top };
    } else if (hit.region == HitRegionType::Scrollbar) {
        const float totalHeight = renderer_.CalculateTotalContentHeight(GetCurrentItems());
        const ScrollbarMetrics metrics = CalculateScrollbarMetrics(totalHeight, state_.scrollOffsetY);
        if (metrics.isVisible) {
            isDraggingScrollbar_ = true;
            if (dipY >= metrics.thumbTop && dipY <= metrics.thumbBottom) {
                // 点击滑块本身：保持当前位置不跳动，记录鼠标相对于滑块顶部的抓取偏移量以便跟手拖动
                scrollbarDragOffsetDip_ = dipY - metrics.thumbTop;
            } else {
                // 点击滑块外的轨道区域：将滑块中心跳转到点击位置，并允许按住继续拖动
                scrollbarDragOffsetDip_ = metrics.thumbHeight * 0.5f;
                UpdateScrollFromCursorY(dipY);
                const ScrollbarMetrics updatedMetrics = CalculateScrollbarMetrics(totalHeight, state_.scrollOffsetY);
                scrollbarDragOffsetDip_ = dipY - updatedMetrics.thumbTop;
            }
        }
    }

    if (hit.region != HitRegionType::None) {
        SetCapture(hwnd_);
    }
}

void MainWindow::OnMouseMove(int pxX, int pxY) {
    if (isMouseDown_ && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) {
        isMouseDown_ = false;
        isMovingWindow_ = false;
        isDraggingScrollbar_ = false;
        dragStarted_ = false;
        if (GetCapture() == hwnd_) {
            ReleaseCapture();
        }
    }

    if (!trackingMouseLeave_) {
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(TRACKMOUSEEVENT);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd_;
        if (TrackMouseEvent(&tme)) {
            trackingMouseLeave_ = true;
        }
    }

    if (isMouseDown_ && isMovingWindow_) {
        POINT curPt{};
        if (GetCursorPos(&curPt)) {
            const int newX = winMoveStartOrigin_.x + (curPt.x - winMoveStartCursor_.x);
            const int newY = winMoveStartOrigin_.y + (curPt.y - winMoveStartCursor_.y);
            SetWindowPos(hwnd_, HWND_TOPMOST, newX, newY, 0, 0,
                         SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return;
    }

    const float dipX = PxToDip(pxX);
    const float dipY = PxToDip(pxY);

    if (isMouseDown_ && isDraggingScrollbar_) {
        UpdateScrollFromCursorY(dipY);
        return;
    }

    const HitTestResult hit = renderer_.HitTest(dipX, dipY, GetCurrentItems(), state_);
    if (hit.itemId != state_.hoveredItemId || hit.region != state_.hoveredRegion) {
        state_.hoveredItemId = hit.itemId;
        state_.hoveredRegion = hit.region;
        Render();
    }

    // 按在卡片本体上移动超过 5px 时触发拖拽
    if (isMouseDown_ && !dragStarted_ &&
        downHit_.region == HitRegionType::CardBody && downHit_.itemId != 0) {
        if (std::abs(pxX - downPt_.x) >= 5 || std::abs(pxY - downPt_.y) >= 5) {
            // 先调用 ReleaseCapture()（会同步触发 WM_CAPTURECHANGED 清零状态），再设置 dragStarted_ = true
            if (GetCapture() == hwnd_) {
                ReleaseCapture();
            }
            dragStarted_ = true;
            isMouseDown_ = false;

            const uint64_t dragId = downHit_.itemId;
            if (state_.activeTab == ActiveTab::Pinned) {
                isInternalPinnedDrag_ = true;
                state_.draggingItemId = dragId;
                POINT curScreenPt{};
                if (GetCursorPos(&curScreenPt)) {
                    OnInternalDragOver(curScreenPt);
                }
            }

            QueueWork([this, dragId] {
                if (worker_.IsStopping()) return;
                RawClipboardPayload payload;
                if (!storage_.LoadItemPayload(dragId, payload)) {
                    CompleteWork([this] {
                        dragStarted_ = false;
                        EndInternalPinnedDrag();
                        Render();
                    });
                    return;
                }
                std::wstring imagePath;
                if (const ClipItem* item = storage_.FindItemById(dragId)) {
                    imagePath = storage_.GetImageFullPath(item->imageFileName);
                }
                CompleteWork([this, payload = std::move(payload), imagePath = std::move(imagePath)] {
                    dragStarted_ = false;
                    if (!isVisible_) {
                        EndInternalPinnedDrag();
                        return;
                    }
                    if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) {
                        if (isInternalPinnedDrag_ && (GetAsyncKeyState(VK_ESCAPE) & 0x8000) == 0) {
                            POINT pt{};
                            RECT rc{};
                            if (GetCursorPos(&pt) && GetWindowRect(hwnd_, &rc) && PtInRect(&rc, pt)) {
                                OnInternalDrop(pt);
                                return;
                            }
                        }
                        EndInternalPinnedDrag();
                        Render();
                        return;
                    }
                    inputInjector_.SetDragInProgress(true);
                    OleDragDrop::PerformDragDrop(payload, imagePath);
                    inputInjector_.SetDragInProgress(false);
                    EndInternalPinnedDrag();
                    state_.hoveredItemId = 0;
                    state_.hoveredRegion = HitRegionType::None;
                    POINT pt{};
                    if (isVisible_ && GetCursorPos(&pt)) {
                        RECT rc{};
                        if (GetWindowRect(hwnd_, &rc) && PtInRect(&rc, pt)) {
                            POINT clientPt = pt;
                            ScreenToClient(hwnd_, &clientPt);
                            const HitTestResult postHit = renderer_.HitTest(
                                PxToDip(clientPt.x),
                                PxToDip(clientPt.y),
                                GetCurrentItems(),
                                state_);
                            state_.hoveredItemId = postHit.itemId;
                            state_.hoveredRegion = postHit.region;
                        }
                    }
                    Render();
                });
            });
            // 加载期间保持已开始拖拽状态，避免松手误触粘贴。
            state_.hoveredItemId = 0;
            state_.hoveredRegion = HitRegionType::None;

            if (isVisible_) {
                Render();
            }
        }
    }
}

void MainWindow::OnLButtonUp(int pxX, int pxY) {
    // 必须在调用 ReleaseCapture() 前保存局部变量快照（ReleaseCapture 会同步触发 WM_CAPTURECHANGED 清空成员变量）
    const bool wasMouseDown = isMouseDown_;
    const bool wasDragStarted = dragStarted_;
    const bool wasMoving = isMovingWindow_;
    const bool wasDraggingScroll = isDraggingScrollbar_;

    isMouseDown_ = false;
    dragStarted_ = false;
    isMovingWindow_ = false;
    isDraggingScrollbar_ = false;

    if (GetCapture() == hwnd_) {
        ReleaseCapture();
    }

    if (wasMoving || wasDraggingScroll) {
        return;
    }

    if (wasMouseDown && !wasDragStarted) {
        const float dipX = PxToDip(pxX);
        const float dipY = PxToDip(pxY);
        const HitTestResult hit = renderer_.HitTest(dipX, dipY, GetCurrentItems(), state_);

        if (hit.region == downHit_.region && hit.itemId == downHit_.itemId) {
            switch (hit.region) {
            case HitRegionType::CloseButton:
                // 按住 Shift 点击右上角 × 直接安全退出程序，普通点击仅隐藏弹窗
                if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
                    DestroyWindow(hwnd_);
                } else {
                    HidePopup();
                }
                break;

            case HitRegionType::TabHistory:
                if (state_.activeTab != ActiveTab::History) {
                    state_.activeTab = ActiveTab::History;
                    state_.scrollOffsetY = 0.0f;
                    const auto& items = GetCurrentItems();
                    state_.selectedItemId = items.empty() ? 0 : items.front().id;
                    Render();
                }
                break;

            case HitRegionType::TabPinned:
                if (state_.activeTab != ActiveTab::Pinned) {
                    state_.activeTab = ActiveTab::Pinned;
                    state_.scrollOffsetY = 0.0f;
                    const auto& items = GetCurrentItems();
                    state_.selectedItemId = items.empty() ? 0 : items.front().id;
                    Render();
                }
                break;

            case HitRegionType::ClearAllButton: {
                const ActiveTab tab = state_.activeTab;
                state_.selectedItemId = 0;
                QueueWork([this, tab] {
                    if (!storage_.ClearAll(tab)) {
                        CompleteWork([this] { MessageBoxW(hwnd_, I18n::Get().errClearSaveFailed, L"LiteClip", MB_OK | MB_ICONERROR); });
                    }
                    PublishItems();
                });
                state_.scrollOffsetY = 0.0f;
                Render();
                break;
            }
            case HitRegionType::CardPinButton: {
                const uint64_t id = hit.itemId;
                QueueWork([this, id] {
                    const ClipItem* item = storage_.FindItemById(id);
                    if (item && !storage_.SetItemPinned(id, !item->isPinned)) {
                        CompleteWork([this] { MessageBoxW(hwnd_, I18n::Get().errPinSaveFailed, L"LiteClip", MB_OK | MB_ICONERROR); });
                    }
                    PublishItems();
                });
                break;
            }
            case HitRegionType::CardDeleteButton: {
                DeleteItemById(hit.itemId);
                break;
            }
            case HitRegionType::CardPlainPasteButton:
                PasteItem(hit.itemId, true);
                break;
            case HitRegionType::CardBody:
                PasteItem(hit.itemId, false);
                break;
            default:
                break;
            }
        }
    }
}

void MainWindow::OnRButtonUp(int pxX, int pxY) {
    const float dipX = PxToDip(pxX);
    const float dipY = PxToDip(pxY);
    const HitTestResult hit = renderer_.HitTest(dipX, dipY, GetCurrentItems(), state_);
    if (hit.region == HitRegionType::CloseButton) {
        DestroyWindow(hwnd_);
    }
}

void MainWindow::OnMouseWheel(short delta) {
    if (!isVisible_ || delta == 0) {
        return;
    }

    const float steps = static_cast<float>(delta) / static_cast<float>(WHEEL_DELTA);
    state_.scrollOffsetY -= steps * 42.0f;
    ClampScrollOffset();

    if (isInternalPinnedDrag_ && state_.activeTab == ActiveTab::Pinned && state_.draggingItemId != 0) {
        POINT pt{};
        RECT rc{};
        if (GetCursorPos(&pt) && GetWindowRect(hwnd_, &rc) && PtInRect(&rc, pt)) {
            POINT clientPt = pt;
            ScreenToClient(hwnd_, &clientPt);
            const DropTargetSlot slot = renderer_.CalculateDropSlot(
                PxToDip(clientPt.y),
                state_.draggingItemId,
                pinnedView_,
                state_);
            state_.showDropIndicator = slot.isEffectiveMove;
            state_.dropIndicatorY = slot.indicatorContentY;
        }
    } else if (!isMouseDown_ && !isDraggingScrollbar_ && !isMovingWindow_) {
        POINT cursorPt{};
        if (GetCursorPos(&cursorPt) && ScreenToClient(hwnd_, &cursorPt)) {
            const HitTestResult hit = renderer_.HitTest(
                PxToDip(cursorPt.x),
                PxToDip(cursorPt.y),
                GetCurrentItems(),
                state_);
            state_.hoveredItemId = hit.itemId;
            state_.hoveredRegion = hit.region;
        }
    }

    Render();
}

bool MainWindow::OnInternalDragOver(POINT screenPt) {
    if (!isVisible_ || !isInternalPinnedDrag_ ||
        state_.activeTab != ActiveTab::Pinned || state_.draggingItemId == 0) {
        return false;
    }

    POINT clientPt = screenPt;
    ScreenToClient(hwnd_, &clientPt);
    const float dipY = PxToDip(clientPt.y);

    constexpr float kEdgeZoneDip = 32.0f;
    const float totalHeight = renderer_.CalculateTotalContentHeight(pinnedView_);
    const float maxScroll = std::max(0.0f, totalHeight - Layout::VIEWPORT_HEIGHT);
    const bool needScrollUp = (dipY < kViewportTopDip + kEdgeZoneDip) && (state_.scrollOffsetY > 0.0f);
    const bool needScrollDown = (dipY > Layout::WINDOW_HEIGHT - kEdgeZoneDip) && (state_.scrollOffsetY < maxScroll);

    if (needScrollUp || needScrollDown) {
        if (!dragScrollTimerActive_) {
            SetTimer(hwnd_, TIMER_ID_DRAG_SCROLL, 16, nullptr);
            dragScrollTimerActive_ = true;
        }
    } else if (dragScrollTimerActive_) {
        KillTimer(hwnd_, TIMER_ID_DRAG_SCROLL);
        dragScrollTimerActive_ = false;
    }

    const DropTargetSlot slot = renderer_.CalculateDropSlot(dipY, state_.draggingItemId, pinnedView_, state_);
    const bool changed = (state_.showDropIndicator != slot.isEffectiveMove) ||
                         (std::abs(state_.dropIndicatorY - slot.indicatorContentY) > 0.1f);
    state_.showDropIndicator = slot.isEffectiveMove;
    state_.dropIndicatorY = slot.indicatorContentY;

    if (changed) {
        Render();
    }
    return true;
}

void MainWindow::OnInternalDragLeave() {
    if (dragScrollTimerActive_) {
        KillTimer(hwnd_, TIMER_ID_DRAG_SCROLL);
        dragScrollTimerActive_ = false;
    }
    if (state_.showDropIndicator) {
        state_.showDropIndicator = false;
        Render();
    }
}

bool MainWindow::OnInternalDrop(POINT screenPt) {
    if (!isVisible_ || !isInternalPinnedDrag_ ||
        state_.activeTab != ActiveTab::Pinned || state_.draggingItemId == 0) {
        EndInternalPinnedDrag();
        return false;
    }

    POINT clientPt = screenPt;
    ScreenToClient(hwnd_, &clientPt);
    const float dipY = PxToDip(clientPt.y);
    const uint64_t dragId = state_.draggingItemId;
    const DropTargetSlot slot = renderer_.CalculateDropSlot(dipY, dragId, pinnedView_, state_);

    EndInternalPinnedDrag();

    if (slot.isEffectiveMove) {
        size_t fromIndex = pinnedView_.size();
        for (size_t i = 0; i < pinnedView_.size(); ++i) {
            if (pinnedView_[i].id == dragId) {
                fromIndex = i;
                break;
            }
        }

        const size_t toIndex = slot.targetItemIndex;
        if (fromIndex < pinnedView_.size() && toIndex < pinnedView_.size() && fromIndex != toIndex) {
            if (fromIndex < toIndex) {
                std::rotate(
                    pinnedView_.begin() + static_cast<std::ptrdiff_t>(fromIndex),
                    pinnedView_.begin() + static_cast<std::ptrdiff_t>(fromIndex + 1),
                    pinnedView_.begin() + static_cast<std::ptrdiff_t>(toIndex + 1));
            } else {
                std::rotate(
                    pinnedView_.begin() + static_cast<std::ptrdiff_t>(toIndex),
                    pinnedView_.begin() + static_cast<std::ptrdiff_t>(fromIndex),
                    pinnedView_.begin() + static_cast<std::ptrdiff_t>(fromIndex + 1));
            }
            renderer_.InvalidateLayoutCache();

            QueueWork([this, dragId, toIndex] {
                if (worker_.IsStopping()) return;
                if (!storage_.MovePinnedItem(dragId, toIndex)) {
                    CompleteWork([this] {
                        MessageBoxW(hwnd_, I18n::Get().errPinSaveFailed, L"LiteClip", MB_OK | MB_ICONERROR);
                    });
                }
                PublishItems();
            });
        }
    }

    Render();
    return true;
}

void MainWindow::EndInternalPinnedDrag() {
    if (dragScrollTimerActive_) {
        KillTimer(hwnd_, TIMER_ID_DRAG_SCROLL);
        dragScrollTimerActive_ = false;
    }
    isInternalPinnedDrag_ = false;
    state_.draggingItemId = 0;
    state_.showDropIndicator = false;
    state_.dropIndicatorY = 0.0f;
}

void MainWindow::OnDragScrollTimer() {
    if (!isVisible_ || !isInternalPinnedDrag_ ||
        state_.activeTab != ActiveTab::Pinned || state_.draggingItemId == 0) {
        KillTimer(hwnd_, TIMER_ID_DRAG_SCROLL);
        dragScrollTimerActive_ = false;
        return;
    }

    POINT screenPt{};
    RECT rc{};
    if (!GetCursorPos(&screenPt) || !GetWindowRect(hwnd_, &rc) || !PtInRect(&rc, screenPt)) {
        OnInternalDragLeave();
        return;
    }

    POINT clientPt = screenPt;
    ScreenToClient(hwnd_, &clientPt);
    const float dipY = PxToDip(clientPt.y);

    constexpr float kEdgeZoneDip = 32.0f;
    float deltaScroll = 0.0f;
    if (dipY < kViewportTopDip + kEdgeZoneDip) {
        const float intensity = std::clamp((kViewportTopDip + kEdgeZoneDip - dipY) / kEdgeZoneDip, 0.15f, 1.0f);
        deltaScroll = -8.0f * intensity;
    } else if (dipY > Layout::WINDOW_HEIGHT - kEdgeZoneDip) {
        const float intensity = std::clamp((dipY - (Layout::WINDOW_HEIGHT - kEdgeZoneDip)) / kEdgeZoneDip, 0.15f, 1.0f);
        deltaScroll = 8.0f * intensity;
    } else {
        KillTimer(hwnd_, TIMER_ID_DRAG_SCROLL);
        dragScrollTimerActive_ = false;
        return;
    }

    const float oldScroll = state_.scrollOffsetY;
    state_.scrollOffsetY += deltaScroll;
    ClampScrollOffset();

    const DropTargetSlot slot = renderer_.CalculateDropSlot(dipY, state_.draggingItemId, pinnedView_, state_);
    const bool indicatorChanged = (state_.showDropIndicator != slot.isEffectiveMove) ||
                                  (std::abs(state_.dropIndicatorY - slot.indicatorContentY) > 0.1f);
    state_.showDropIndicator = slot.isEffectiveMove;
    state_.dropIndicatorY = slot.indicatorContentY;

    if (std::abs(state_.scrollOffsetY - oldScroll) > 0.01f || indicatorChanged) {
        Render();
    }
}

void MainWindow::OnClipboardUpdated() {
    if (worker_.IsStopping()) return;
    if (capturePending_) {
        captureAgain_ = true;
        return;
    }
    const DWORD seq = GetClipboardSequenceNumber();
    if (seq != retrySeq_) {
        retrySeq_ = seq;
        retryCount_ = 0;
    }
    // 探活剪贴板宿主窗口（如 WINWORD.EXE）：
    // 若用户刚刚在 Word 中复制了 50 张高清大图，Word 主线程可能正处于构建内部 OLE 树的短暂繁忙状态，
    // 此时若强行请求剪贴板数据会造成双方互锁卡顿。用 15ms 超时的 WM_NULL 探测，若超时则有限次顺延退避。
    // 注意：
    // 1. 若因 UIPI 权限隔离（如管理员窗口 ERROR_ACCESS_DENIED）或无效句柄导致失败，非忙碌状态，直接放行；
    // 2. 若外部软件使用无消息循环的后台线程持有 ClipboardOwner 导致每次均 ERROR_TIMEOUT，最多退避 3 次后必须放行捕获，防止丢单。
    if (HWND clipOwner = ::GetClipboardOwner()) {
        DWORD_PTR dummyResult = 0;
        ::SetLastError(ERROR_SUCCESS);
        if (::SendMessageTimeoutW(
                clipOwner,
                WM_NULL,
                0,
                0,
                SMTO_ABORTIFHUNG | SMTO_NORMAL,
                15,
                &dummyResult) == 0) {
            const DWORD err = ::GetLastError();
            if (err == ERROR_TIMEOUT && retryCount_ < 3) {
                const UINT delay = 150U << retryCount_++;
                SetTimer(hwnd_, TIMER_ID_CLIP_DEBOUNCE, delay, nullptr);
                return;
            }
        }
    }

    capturePending_ = true;
    QueueWork([this, startSeq = seq] {
        if (worker_.IsStopping()) return;
        RawClipboardPayload payload;
        bool transientRetry = false;
        if (clipMonitor_.OnClipboardUpdate(payload, &transientRetry) && !worker_.IsStopping()) {
            const bool addedOrMoved = storage_.AddCapturedItem(payload);
            const auto& history = storage_.GetHistoryItems();
            if (addedOrMoved || (!history.empty() && history.front().clipboardSeqNum == payload.clipboardSeqNum)) {
                if (!history.empty()) {
                    ClipItem newest = history.front();
                    CompleteWork([this, newest = std::move(newest)]() mutable {
                        const auto existing = std::find_if(historyView_.begin(), historyView_.end(),
                            [&newest](const ClipItem& item) { return item.id == newest.id; });
                        if (existing == historyView_.begin() && existing != historyView_.end()) {
                            *existing = newest;
                        } else {
                            if (existing != historyView_.end()) historyView_.erase(existing);
                            historyView_.push_front(newest);
                        }
                        if (newest.isPinned) {
                            for (auto& item : pinnedView_) {
                                if (item.id == newest.id) { item = newest; break; }
                            }
                        }
                        ClampScrollOffset();
                        Render();
                    });
                }
            }
        }
        CompleteWork([this, startSeq, transientRetry] {
            capturePending_ = false;
            const DWORD currentSeq = GetClipboardSequenceNumber();
            if (captureAgain_ || (currentSeq != 0 && currentSeq != startSeq)) {
                captureAgain_ = false;
                SetTimer(hwnd_, TIMER_ID_CLIP_DEBOUNCE, 180, nullptr);
            } else if (transientRetry && retryCount_ < 5) {
                const UINT delay = 120U << retryCount_++;
                SetTimer(hwnd_, TIMER_ID_CLIP_DEBOUNCE, delay, nullptr);
            }
        });
    });
}

LRESULT CALLBACK MainWindow::WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = nullptr;

    if (uMsg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        return self->HandleMessage(uMsg, wParam, lParam);
    }

    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

LRESULT MainWindow::HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_MOUSEACTIVATE:
        // 核心红线：强制不激活窗口，绝不抢占底层活动窗口焦点
        return MA_NOACTIVATE;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd_, &ps);
        Render();
        EndPaint(hwnd_, &ps);
        return 0;
    }

    case WM_SIZE: {
        const UINT width = LOWORD(lParam);
        const UINT height = HIWORD(lParam);
        if (width > 0 && height > 0) {
            renderer_.OnResize(width, height, dpi_);
        }
        return 0;
    }

    case WM_DPICHANGED: {
        dpi_ = HIWORD(wParam);
        // 若当前正处于 ShowPopup 定位阶段（!isVisible_），由 ShowPopup 统一完成尺寸计算与边界钳制
        if (!isVisible_) {
            return 0;
        }
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        if (suggested) {
            const int width = DipToPx(static_cast<float>(Layout::WINDOW_WIDTH));
            const int height = DipToPx(static_cast<float>(Layout::WINDOW_HEIGHT));
            SetWindowPos(
                hwnd_,
                HWND_TOPMOST,
                suggested->left,
                suggested->top,
                width,
                height,
                SWP_NOACTIVATE | SWP_NOZORDER
            );
            renderer_.OnResize(static_cast<UINT>(width), static_cast<UINT>(height), dpi_);
            ClampScrollOffset();
            Render();
        }
        return 0;
    }

    case WM_HOTKEY:
        switch (wParam) {
        case HOTKEY_ID_WIN_V:
            TogglePopup();
            break;
        case HOTKEY_ID_ESC:
            HidePopup();
            break;
        case HOTKEY_ID_UP:
            MoveSelection(-1);
            break;
        case HOTKEY_ID_DOWN:
            MoveSelection(1);
            break;
        case HOTKEY_ID_ENTER:
            PasteSelectedItem(false);
            break;
        case HOTKEY_ID_SHIFT_ENTER:
            PasteSelectedItem(true);
            break;
        case HOTKEY_ID_DELETE:
            DeleteSelectedItem();
            break;
        default:
            break;
        }
        return 0;

    case WM_CLIPBOARDUPDATE:
        if (capturePending_) {
            captureAgain_ = true;
        }
        // 180ms 防抖延迟：避让用户极速连按 Ctrl+C -> Ctrl+V，确保绝对不抢占系统剪切板锁
        SetTimer(hwnd_, TIMER_ID_CLIP_DEBOUNCE, 180, nullptr);
        return 0;

    case WM_APP_WORK_COMPLETE:
        DrainCompletions();
        return 0;

    case WM_LBUTTONDOWN:
        OnLButtonDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_MOUSEMOVE:
        OnMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_LBUTTONUP:
        OnLButtonUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_RBUTTONUP:
        OnRButtonUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_CAPTURECHANGED:
        isMouseDown_ = false;
        isMovingWindow_ = false;
        isDraggingScrollbar_ = false;
        dragStarted_ = false;
        return 0;

    case WM_MOUSELEAVE:
        trackingMouseLeave_ = false;
        if (state_.hoveredItemId != 0 || state_.hoveredRegion != HitRegionType::None) {
            state_.hoveredItemId = 0;
            state_.hoveredRegion = HitRegionType::None;
            Render();
        }
        return 0;

    case WM_MOUSEWHEEL:
    case WM_APP_MOUSE_WHEEL:
        OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;

    case WM_APP_HIDE_WINDOW:
        HidePopup();
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_ID_DRAG_SCROLL) {
            OnDragScrollTimer();
        } else if (wParam == TIMER_ID_CLIP_DEBOUNCE) {
            KillTimer(hwnd_, TIMER_ID_CLIP_DEBOUNCE);
            OnClipboardUpdated();
        }
        return 0;

    case WM_QUERYENDSESSION:
        return TRUE;

    case WM_ENDSESSION:
        if (wParam != FALSE) {
            HidePopup();
            KillTimer(hwnd_, TIMER_ID_CLIP_DEBOUNCE);
            KillTimer(hwnd_, TIMER_ID_DRAG_SCROLL);
            OleDragDrop::RevokeInternalDropTarget(hwnd_);
            RemoveClipboardFormatListener(hwnd_);
            StopWorker();
            UnregisterHotKey(hwnd_, HOTKEY_ID_WIN_V);
        }
        return 0;

    case WM_DESTROY:
        HidePopup();
        KillTimer(hwnd_, TIMER_ID_CLIP_DEBOUNCE);
        KillTimer(hwnd_, TIMER_ID_DRAG_SCROLL);
        OleDragDrop::RevokeInternalDropTarget(hwnd_);
        RemoveClipboardFormatListener(hwnd_);
        StopWorker();
        inputInjector_.SetPopupMonitoring(hwnd_, false);
        UnregisterHotKey(hwnd_, HOTKEY_ID_WIN_V);
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd_, uMsg, wParam, lParam);
}