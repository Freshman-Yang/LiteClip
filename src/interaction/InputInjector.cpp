#include "interaction/InputInjector.h"
#include "interaction/CaretLocator.h"
#include <atomic>
#include <cwchar>
#include <future>
#include <vector>

namespace {
    constexpr UINT WM_APP_SYNC_HOOKS = WM_APP + 0x40;
    constexpr UINT_PTR kHookFallbackTimerIntervalMs = 50;

    std::atomic<HWND> g_popupHwnd{ nullptr };
    std::atomic<HWND> g_initialForegroundHwnd{ nullptr };
    std::atomic<bool> g_isDragging{ false };
    std::atomic<bool> g_isInternalMouseDown{ false };
    std::atomic<bool> g_wantMouseHook{ false };
    std::atomic<bool> g_wantKeyboardHook{ false };
    std::atomic<bool> g_forceRehookMouse{ false };
    std::atomic<HHOOK> g_mouseHook{ nullptr };
    std::atomic<HHOOK> g_keyboardHook{ nullptr };
    std::atomic<uint32_t> g_syncReqSeq{ 0 };
    std::atomic<uint32_t> g_syncAckSeq{ 0 };
    std::atomic<bool> g_prevExternalMouseButtonsDown{ false };

    struct PopupHotkeySpec {
        int id;
        UINT modifiers;
        UINT vk;
        uint32_t maskBit;
    };

    constexpr PopupHotkeySpec kPopupHotkeys[] = {
        { HOTKEY_ID_ESC,         MOD_NOREPEAT,             VK_ESCAPE, 1u << 0 },
        { HOTKEY_ID_UP,          0,                        VK_UP,     1u << 1 },
        { HOTKEY_ID_DOWN,        0,                        VK_DOWN,   1u << 2 },
        { HOTKEY_ID_ENTER,       MOD_NOREPEAT,             VK_RETURN, 1u << 3 },
        { HOTKEY_ID_SHIFT_ENTER, MOD_SHIFT | MOD_NOREPEAT, VK_RETURN, 1u << 4 },
        { HOTKEY_ID_DELETE,      MOD_NOREPEAT,             VK_DELETE, 1u << 5 },
    };
    constexpr uint32_t kAllPopupHotkeysMask = (1u << 6) - 1u;

    bool IsAnyMouseButtonPhysicallyDown() noexcept {
        return (GetAsyncKeyState(VK_LBUTTON)  & 0x8000) != 0 ||
               (GetAsyncKeyState(VK_RBUTTON)  & 0x8000) != 0 ||
               (GetAsyncKeyState(VK_MBUTTON)  & 0x8000) != 0 ||
               (GetAsyncKeyState(VK_XBUTTON1) & 0x8000) != 0 ||
               (GetAsyncKeyState(VK_XBUTTON2) & 0x8000) != 0;
    }

    bool IsPopupThreadCaptured(HWND popupHwnd) noexcept {
        if (popupHwnd == nullptr) {
            return false;
        }
        const DWORD tid = GetWindowThreadProcessId(popupHwnd, nullptr);
        if (tid == 0) {
            return false;
        }
        GUITHREADINFO gti{};
        gti.cbSize = sizeof(GUITHREADINFO);
        return GetGUIThreadInfo(tid, &gti) && gti.hwndCapture != nullptr;
    }
}

InputInjector::InputInjector() = default;

InputInjector::~InputInjector() {
    SetPopupMonitoring(g_popupHwnd.load(std::memory_order_acquire), false);
    StopHookThread();
}

POINT InputInjector::CalculatePopupPosition(int winWidthPx, int winHeightPx) const {
    return CaretLocator::CalculatePopupPosition(winWidthPx, winHeightPx);
}

void InputInjector::SyncHooksOnCurrentThread(
    HHOOK& mouseHook,
    HHOOK& keyboardHook,
    HWINEVENTHOOK& winEventHook,
    UINT_PTR& pollTimerId
) noexcept {
    const HINSTANCE hMod = GetModuleHandleW(nullptr);
    const bool wantMouse = g_wantMouseHook.load(std::memory_order_acquire);
    const bool wantKeyboard = g_wantKeyboardHook.load(std::memory_order_acquire);
    const bool forceRehook = g_forceRehookMouse.exchange(false, std::memory_order_acq_rel);

    if (wantMouse) {
        if (forceRehook && mouseHook != nullptr) {
            UnhookWindowsHookEx(mouseHook);
            mouseHook = nullptr;
            g_mouseHook.store(nullptr, std::memory_order_release);
        }
        if (mouseHook == nullptr) {
            mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, hMod, 0);
            g_mouseHook.store(mouseHook, std::memory_order_release);
        }
        if (winEventHook == nullptr) {
            winEventHook = SetWinEventHook(
                EVENT_SYSTEM_FOREGROUND,
                EVENT_SYSTEM_CAPTURESTART,
                nullptr,
                WinEventProc,
                0,
                0,
                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
            );
        }
        if (pollTimerId == 0) {
            pollTimerId = SetTimer(nullptr, 0, kHookFallbackTimerIntervalMs, nullptr);
        }
    } else {
        if (mouseHook != nullptr) {
            UnhookWindowsHookEx(mouseHook);
            mouseHook = nullptr;
            g_mouseHook.store(nullptr, std::memory_order_release);
        }
        if (winEventHook != nullptr) {
            UnhookWinEvent(winEventHook);
            winEventHook = nullptr;
        }
        if (pollTimerId != 0) {
            KillTimer(nullptr, pollTimerId);
            pollTimerId = 0;
        }
    }

    if (wantKeyboard) {
        if (keyboardHook == nullptr) {
            keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, hMod, 0);
            g_keyboardHook.store(keyboardHook, std::memory_order_release);
        }
    } else if (keyboardHook != nullptr) {
        UnhookWindowsHookEx(keyboardHook);
        keyboardHook = nullptr;
        g_keyboardHook.store(nullptr, std::memory_order_release);
    }

    g_syncAckSeq.store(g_syncReqSeq.load(std::memory_order_acquire), std::memory_order_release);
    g_syncAckSeq.notify_all();
}

bool InputInjector::EnsureHookThread() {
    if (hookThread_.joinable() && hookThreadId_ != 0) {
        return true;
    }
    StopHookThread();

    try {
        std::promise<DWORD> ready;
        auto future = ready.get_future();

        hookThread_ = std::thread([ready = std::move(ready)]() mutable {
            // 强制提前创建线程消息队列，确保主线程随后调用 PostThreadMessageW 必定成功
            MSG msg{};
            PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
            ready.set_value(GetCurrentThreadId());

            HHOOK mouseHook = nullptr;
            HHOOK keyboardHook = nullptr;
            HWINEVENTHOOK winEventHook = nullptr;
            UINT_PTR pollTimerId = 0;
            SyncHooksOnCurrentThread(mouseHook, keyboardHook, winEventHook, pollTimerId);

            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                if (msg.message == WM_APP_SYNC_HOOKS) {
                    SyncHooksOnCurrentThread(mouseHook, keyboardHook, winEventHook, pollTimerId);
                } else if (msg.message == WM_TIMER) {
                    if (g_wantMouseHook.load(std::memory_order_acquire) && mouseHook == nullptr) {
                        const HINSTANCE hMod = GetModuleHandleW(nullptr);
                        mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, hMod, 0);
                        g_mouseHook.store(mouseHook, std::memory_order_release);
                    }
                    CheckExternalInteractionFallback();
                } else {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }

            if (pollTimerId != 0) {
                KillTimer(nullptr, pollTimerId);
            }
            if (winEventHook != nullptr) {
                UnhookWinEvent(winEventHook);
            }
            if (mouseHook != nullptr) {
                UnhookWindowsHookEx(mouseHook);
                g_mouseHook.store(nullptr, std::memory_order_release);
            }
            if (keyboardHook != nullptr) {
                UnhookWindowsHookEx(keyboardHook);
                g_keyboardHook.store(nullptr, std::memory_order_release);
            }
        });

        hookThreadId_ = future.get();
        return hookThreadId_ != 0;
    } catch (...) {
        hookThreadId_ = 0;
        return false;
    }
}

void InputInjector::StopHookThread() {
    g_wantMouseHook.store(false, std::memory_order_release);
    g_wantKeyboardHook.store(false, std::memory_order_release);
    SyncHooksOnCurrentThread(fallbackMouseHook_, fallbackKeyboardHook_, fallbackWinEventHook_, fallbackPollTimerId_);

    if (hookThread_.joinable()) {
        if (hookThreadId_ != 0) {
            PostThreadMessageW(hookThreadId_, WM_QUIT, 0, 0);
        }
        hookThread_.join();
    }
    hookThreadId_ = 0;
}

void InputInjector::SyncHooks() {
    const uint32_t seq = g_syncReqSeq.fetch_add(1, std::memory_order_acq_rel) + 1;
    const bool needAnyHook =
        g_wantMouseHook.load(std::memory_order_acquire) ||
        g_wantKeyboardHook.load(std::memory_order_acquire);

    if (needAnyHook) {
        if (EnsureHookThread() && PostThreadMessageW(hookThreadId_, WM_APP_SYNC_HOOKS, 0, 0)) {
            while (g_syncAckSeq.load(std::memory_order_acquire) < seq) {
                g_syncAckSeq.wait(seq - 1, std::memory_order_acquire);
            }
            return;
        }
        // 极端情况下若独立钩子线程不可用，降级在当前线程同步挂载
        SyncHooksOnCurrentThread(fallbackMouseHook_, fallbackKeyboardHook_, fallbackWinEventHook_, fallbackPollTimerId_);
    } else {
        if (hookThread_.joinable() && hookThreadId_ != 0 &&
            PostThreadMessageW(hookThreadId_, WM_APP_SYNC_HOOKS, 0, 0)) {
            while (g_syncAckSeq.load(std::memory_order_acquire) < seq) {
                g_syncAckSeq.wait(seq - 1, std::memory_order_acquire);
            }
        }
        SyncHooksOnCurrentThread(fallbackMouseHook_, fallbackKeyboardHook_, fallbackWinEventHook_, fallbackPollTimerId_);
    }
}

bool InputInjector::RegisterPopupHotkeys(HWND popupHwnd) noexcept {
    if (popupHwnd == nullptr) {
        return false;
    }
    for (const auto& spec : kPopupHotkeys) {
        if ((registeredHotkeysMask_ & spec.maskBit) == 0) {
            if (RegisterHotKey(popupHwnd, spec.id, spec.modifiers, spec.vk)) {
                registeredHotkeysMask_ |= spec.maskBit;
            }
        }
    }
    return registeredHotkeysMask_ == kAllPopupHotkeysMask;
}

void InputInjector::UnregisterPopupHotkeys(HWND popupHwnd) noexcept {
    if (registeredHotkeysMask_ == 0) {
        return;
    }
    HWND targetHwnd = (popupHwnd != nullptr)
        ? popupHwnd
        : g_popupHwnd.load(std::memory_order_acquire);
    if (targetHwnd != nullptr) {
        for (const auto& spec : kPopupHotkeys) {
            if ((registeredHotkeysMask_ & spec.maskBit) != 0) {
                UnregisterHotKey(targetHwnd, spec.id);
            }
        }
    }
    registeredHotkeysMask_ = 0;
}

void InputInjector::SetPopupMonitoring(HWND popupHwnd, bool visible) {
    if (visible) {
        g_popupHwnd.store(popupHwnd, std::memory_order_release);
        g_initialForegroundHwnd.store(GetForegroundWindow(), std::memory_order_release);
        g_isDragging.store(false, std::memory_order_release);
        g_isInternalMouseDown.store(false, std::memory_order_release);
        g_prevExternalMouseButtonsDown.store(IsAnyMouseButtonPhysicallyDown(), std::memory_order_release);

        // 优先使用内核态 RegisterHotKey 监听 Esc 及面板键盘导航键（↑/↓/Enter/Shift+Enter/Delete），
        // 既保持不抢占底层窗口焦点，又完全避免挂载全局键盘低级钩子造成的击键上下文切换；
        // 仅当某按键被其他软件全局占用导致注册失败时，才降级启用 WH_KEYBOARD_LL 钩子兜底。
        const bool allHotkeysRegistered = RegisterPopupHotkeys(popupHwnd);

        // 每次呼出时强制重挂 WH_MOUSE_LL，防止此前因超时被系统静默摘除后失效
        g_forceRehookMouse.store(true, std::memory_order_release);
        g_wantMouseHook.store(true, std::memory_order_release);
        g_wantKeyboardHook.store(!allHotkeysRegistered, std::memory_order_release);
        SyncHooks();
    } else {
        g_wantMouseHook.store(false, std::memory_order_release);
        g_wantKeyboardHook.store(false, std::memory_order_release);
        SyncHooks();

        UnregisterPopupHotkeys(popupHwnd);

        g_popupHwnd.store(nullptr, std::memory_order_release);
        g_initialForegroundHwnd.store(nullptr, std::memory_order_release);
        g_isDragging.store(false, std::memory_order_release);
        g_isInternalMouseDown.store(false, std::memory_order_release);
        g_prevExternalMouseButtonsDown.store(false, std::memory_order_release);
    }
}

void InputInjector::SetDragInProgress(bool dragging) {
    g_isDragging.store(dragging, std::memory_order_release);

    HWND popupHwnd = g_popupHwnd.load(std::memory_order_acquire);
    if (dragging) {
        UnregisterPopupHotkeys(popupHwnd);
        if (g_wantKeyboardHook.exchange(false, std::memory_order_acq_rel)) {
            SyncHooks();
        }
    } else {
        if (popupHwnd != nullptr && IsWindowVisible(popupHwnd) &&
            g_wantMouseHook.load(std::memory_order_acquire)) {
            const bool allHotkeysRegistered = RegisterPopupHotkeys(popupHwnd);
            const bool needKeyboardFallback = !allHotkeysRegistered;
            if (g_wantKeyboardHook.exchange(needKeyboardFallback, std::memory_order_acq_rel) != needKeyboardFallback) {
                SyncHooks();
            }
        }
    }
}


void InputInjector::SendPasteCommand(HWND expectedTarget) const {
    // 按终端宿主窗口类识别，不依赖内部运行的是 CMD、PowerShell 还是其他 shell。
    HWND targetWnd = GetForegroundWindow();
    if (targetWnd == nullptr || targetWnd != expectedTarget || !IsWindow(expectedTarget)) {
        return;
    }
    wchar_t className[256]{};
    const bool useShiftInsert =
        GetClassNameW(targetWnd, className, static_cast<int>(sizeof(className) / sizeof(className[0]))) != 0 &&
        (std::wcscmp(className, L"ConsoleWindowClass") == 0 ||
         std::wcscmp(className, L"CASCADIA_HOSTING_WINDOW_CLASS") == 0);

    std::vector<INPUT> inputs;
    inputs.reserve(12);

    auto appendKey = [&inputs](WORD vk, DWORD flags) {
        if (vk == VK_LWIN || vk == VK_RWIN || vk == VK_RMENU ||
            vk == VK_RCONTROL || vk == VK_INSERT) {
            flags |= KEYEVENTF_EXTENDEDKEY;
        }
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = vk;
        input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
        input.ki.dwFlags = flags;
        inputs.push_back(input);
    };

    // 仅检查并释放具体的左右物理修饰键，避免重复释放；终端粘贴也不能残留 Ctrl 状态。
    constexpr WORD kModifiersToRelease[] = {
        VK_LWIN, VK_RWIN,
        VK_LSHIFT, VK_RSHIFT,
        VK_LCONTROL, VK_RCONTROL,
        VK_LMENU, VK_RMENU
    };

    for (WORD vk : kModifiersToRelease) {
        if ((GetAsyncKeyState(vk) & 0x8000) != 0) {
            appendKey(vk, KEYEVENTF_KEYUP);
        }
    }

    // 每次仅发送一套快捷键，避免目标已处理 Ctrl + V 后再次粘贴。
    const WORD modifier = useShiftInsert ? VK_SHIFT : VK_CONTROL;
    const WORD pasteKey = useShiftInsert ? VK_INSERT : static_cast<WORD>('V');
    appendKey(modifier, 0);
    appendKey(pasteKey, 0);
    appendKey(pasteKey, KEYEVENTF_KEYUP);
    appendKey(modifier, KEYEVENTF_KEYUP);

    const UINT inputCount = static_cast<UINT>(inputs.size());
    if (GetForegroundWindow() != expectedTarget) return;
    if (SendInput(inputCount, inputs.data(), sizeof(INPUT)) != inputCount) {
        // UIPI 可能阻止向管理员窗口注入输入；换快捷键或重试无法绕过权限限制。
        OutputDebugStringW(L"LiteClip: paste input injection incomplete; check target focus and integrity level.\n");
    }
}

LRESULT CALLBACK InputInjector::LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && lParam != 0 && g_wantMouseHook.load(std::memory_order_acquire)) {
        if (wParam == WM_LBUTTONUP || wParam == WM_NCLBUTTONUP) {
            g_isInternalMouseDown.store(false, std::memory_order_release);
            return CallNextHookEx(g_mouseHook.load(std::memory_order_relaxed), nCode, wParam, lParam);
        }

        const bool isClickDown = (
            wParam == WM_LBUTTONDOWN   || wParam == WM_RBUTTONDOWN   || wParam == WM_MBUTTONDOWN   ||
            wParam == WM_NCLBUTTONDOWN || wParam == WM_NCRBUTTONDOWN || wParam == WM_NCMBUTTONDOWN ||
            wParam == WM_XBUTTONDOWN   || wParam == WM_NCXBUTTONDOWN
        );
        const bool isWheel = (wParam == WM_MOUSEWHEEL);

        // 严格快速放行 WM_MOUSEMOVE 等高频无关鼠标消息
        if (!isClickDown && !isWheel) {
            return CallNextHookEx(g_mouseHook.load(std::memory_order_relaxed), nCode, wParam, lParam);
        }

        HWND popupHwnd = g_popupHwnd.load(std::memory_order_acquire);
        bool isDragging = g_isDragging.load(std::memory_order_acquire);

        if (popupHwnd != nullptr) {
            const auto* pMouse = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
            RECT rc{};
            if (GetWindowRect(popupHwnd, &rc)) {
                const bool insideWindow = (PtInRect(&rc, pMouse->pt) != FALSE);

                if (isWheel) {
                    if (insideWindow) {
                        short delta = GET_WHEEL_DELTA_WPARAM(pMouse->mouseData);
                        PostMessageW(
                            popupHwnd,
                            WM_APP_MOUSE_WHEEL,
                            MAKEWPARAM(0, delta),
                            MAKELPARAM(pMouse->pt.x, pMouse->pt.y)
                        );
                        return 1;
                    }
                    return CallNextHookEx(g_mouseHook.load(std::memory_order_relaxed), nCode, wParam, lParam);
                }

                if (!isDragging) {
                    if (wParam == WM_LBUTTONDOWN || wParam == WM_NCLBUTTONDOWN) {
                        g_isInternalMouseDown.store(insideWindow, std::memory_order_release);
                    } else if (!insideWindow && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) {
                        g_isInternalMouseDown.store(false, std::memory_order_release);
                    }

                    // 若 UI 线程持有鼠标捕获（如窗口移动/滚动条拖拽）或内部左键按压拖拽尚未释放，则跳过外部点击收起
                    const bool isCapturedOrInternalDrag =
                        (g_isInternalMouseDown.load(std::memory_order_acquire) &&
                         (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) ||
                        (!insideWindow && IsPopupThreadCaptured(popupHwnd));

                    if (isClickDown && !insideWindow && !isCapturedOrInternalDrag) {
                        PostMessageW(popupHwnd, WM_APP_HIDE_WINDOW, 0, 0);
                    }
                }
            }
        }
    }

    return CallNextHookEx(g_mouseHook.load(std::memory_order_relaxed), nCode, wParam, lParam);
}

void CALLBACK InputInjector::WinEventProc(
    HWINEVENTHOOK /*hWinEventHook*/,
    DWORD event,
    HWND hwnd,
    LONG /*idObject*/,
    LONG /*idChild*/,
    DWORD /*idEventThread*/,
    DWORD /*dwmsEventTime*/
) noexcept {
    if (!g_wantMouseHook.load(std::memory_order_acquire) ||
        g_isDragging.load(std::memory_order_acquire)) {
        return;
    }

    const HWND popupHwnd = g_popupHwnd.load(std::memory_order_acquire);
    if (popupHwnd == nullptr || hwnd == popupHwnd || !IsWindowVisible(popupHwnd)) {
        return;
    }

    const bool isCapturedOrInternalDrag =
        (g_isInternalMouseDown.load(std::memory_order_acquire) &&
         (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) ||
        IsPopupThreadCaptured(popupHwnd);
    if (isCapturedOrInternalDrag) {
        return;
    }

    if (event == EVENT_SYSTEM_FOREGROUND) {
        const HWND initialFg = g_initialForegroundHwnd.load(std::memory_order_acquire);
        if (hwnd != nullptr && hwnd != initialFg) {
            PostMessageW(popupHwnd, WM_APP_HIDE_WINDOW, 0, 0);
        }
    } else if (event == EVENT_SYSTEM_CAPTURESTART) {
        POINT pt{};
        RECT rc{};
        if (GetCursorPos(&pt) && GetWindowRect(popupHwnd, &rc) && !PtInRect(&rc, pt)) {
            PostMessageW(popupHwnd, WM_APP_HIDE_WINDOW, 0, 0);
        }
    }
}

void InputInjector::CheckExternalInteractionFallback() noexcept {
    if (!g_wantMouseHook.load(std::memory_order_acquire) ||
        g_isDragging.load(std::memory_order_acquire)) {
        g_prevExternalMouseButtonsDown.store(false, std::memory_order_release);
        return;
    }

    const HWND popupHwnd = g_popupHwnd.load(std::memory_order_acquire);
    if (popupHwnd == nullptr || !IsWindowVisible(popupHwnd)) {
        g_prevExternalMouseButtonsDown.store(false, std::memory_order_release);
        return;
    }

    if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) {
        g_isInternalMouseDown.store(false, std::memory_order_release);
    }

    const bool isCapturedOrInternalDrag =
        (g_isInternalMouseDown.load(std::memory_order_acquire) &&
         (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) ||
        IsPopupThreadCaptured(popupHwnd);

    // 1. 兜底检测前台窗口切换（如点击任务栏、管理员窗口或 Alt+Tab 切走）
    const HWND curFg = GetForegroundWindow();
    const HWND initFg = g_initialForegroundHwnd.load(std::memory_order_acquire);
    if (curFg != nullptr && curFg != popupHwnd && curFg != initFg && !isCapturedOrInternalDrag) {
        PostMessageW(popupHwnd, WM_APP_HIDE_WINDOW, 0, 0);
        return;
    }

    // 2. 兜底检测 UIPI 屏蔽（如点击本就处于前台的管理员权限窗口内部）或 WH_MOUSE_LL 失效时的外部点击
    const bool anyDown = IsAnyMouseButtonPhysicallyDown();
    const bool wasDown = g_prevExternalMouseButtonsDown.exchange(anyDown, std::memory_order_acq_rel);
    if (anyDown && !wasDown && !isCapturedOrInternalDrag) {
        POINT pt{};
        RECT rc{};
        if (GetCursorPos(&pt) && GetWindowRect(popupHwnd, &rc) && !PtInRect(&rc, pt)) {
            PostMessageW(popupHwnd, WM_APP_HIDE_WINDOW, 0, 0);
        }
    }
}

LRESULT CALLBACK InputInjector::LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && lParam != 0 && wParam == WM_KEYDOWN &&
        g_wantKeyboardHook.load(std::memory_order_acquire)) {
        const auto* pKey = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);

        // 忽略 SendInput 等注入的模拟按键，避免自激干扰
        if ((pKey->flags & LLKHF_INJECTED) != 0 || (pKey->flags & LLKHF_LOWER_IL_INJECTED) != 0) {
            return CallNextHookEx(g_keyboardHook.load(std::memory_order_relaxed), nCode, wParam, lParam);
        }

        HWND popupHwnd = g_popupHwnd.load(std::memory_order_acquire);
        bool isDragging = g_isDragging.load(std::memory_order_acquire);

        if (popupHwnd != nullptr && !isDragging && IsWindowVisible(popupHwnd)) {
            // 当 Ctrl/Alt/Win 按下时不拦截，防止抢占系统或应用组合快捷键
            const bool ctrlDown  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool altDown   = (GetAsyncKeyState(VK_MENU)    & 0x8000) != 0;
            const bool shiftDown = (GetAsyncKeyState(VK_SHIFT)   & 0x8000) != 0;
            const bool winDown   = (GetAsyncKeyState(VK_LWIN)    & 0x8000) != 0 ||
                                   (GetAsyncKeyState(VK_RWIN)    & 0x8000) != 0;

            if (!ctrlDown && !altDown && !winDown) {
                if (!shiftDown) {
                    switch (pKey->vkCode) {
                    case VK_ESCAPE:
                        PostMessageW(popupHwnd, WM_APP_HIDE_WINDOW, 0, 0);
                        return 1;
                    case VK_UP:
                        PostMessageW(popupHwnd, WM_HOTKEY, HOTKEY_ID_UP, 0);
                        return 1;
                    case VK_DOWN:
                        PostMessageW(popupHwnd, WM_HOTKEY, HOTKEY_ID_DOWN, 0);
                        return 1;
                    case VK_RETURN:
                        PostMessageW(popupHwnd, WM_HOTKEY, HOTKEY_ID_ENTER, 0);
                        return 1;
                    case VK_DELETE:
                        PostMessageW(popupHwnd, WM_HOTKEY, HOTKEY_ID_DELETE, 0);
                        return 1;
                    default:
                        break;
                    }
                } else if (pKey->vkCode == VK_RETURN) {
                    PostMessageW(popupHwnd, WM_HOTKEY, HOTKEY_ID_SHIFT_ENTER, 0);
                    return 1;
                }
            }
        }
    }

    return CallNextHookEx(g_keyboardHook.load(std::memory_order_relaxed), nCode, wParam, lParam);
}
