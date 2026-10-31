#pragma once

#include "platform/Win32Common.h"
#include <thread>

class InputInjector {
public:
    InputInjector();
    ~InputInjector();

    InputInjector(const InputInjector&) = delete;
    InputInjector& operator=(const InputInjector&) = delete;

    // 计算窗口呼出时的屏幕物理像素左上角坐标 (X, Y)
    // winWidthPx, winHeightPx 为当前 DPI 下剪切板窗口的物理像素宽高
    POINT CalculatePopupPosition(int winWidthPx, int winHeightPx) const;

    // 仅在窗口显示时开启外部鼠标点击监听与面板键盘导航快捷键（后台零开销）
    // 低级鼠标钩子运行于独立轻量线程，绝不因主线程 D2D 渲染或资源重建阻塞系统输入
    void SetPopupMonitoring(HWND popupHwnd, bool visible);

    // 当处于 OLE 拖拽过程中时设为 true，暂停“外部点击自动收起窗口”的判定及面板快捷键拦截
    void SetDragInProgress(bool dragging);

    // 向底层活动窗口模拟粘贴：传统控制台 / Windows Terminal 使用 Shift + Insert，其他应用使用 Ctrl + V
    void SendPasteCommand(HWND expectedTarget) const;

private:
    bool EnsureHookThread();
    void StopHookThread();
    void SyncHooks();
    bool RegisterPopupHotkeys(HWND popupHwnd) noexcept;
    void UnregisterPopupHotkeys(HWND popupHwnd) noexcept;

    static void SyncHooksOnCurrentThread(HHOOK& mouseHook, HHOOK& keyboardHook, HWINEVENTHOOK& winEventHook, UINT_PTR& pollTimerId) noexcept;
    static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
    static void CALLBACK WinEventProc(
        HWINEVENTHOOK hWinEventHook,
        DWORD event,
        HWND hwnd,
        LONG idObject,
        LONG idChild,
        DWORD idEventThread,
        DWORD dwmsEventTime
    ) noexcept;
    static void CheckExternalInteractionFallback() noexcept;

    std::thread hookThread_;
    DWORD hookThreadId_ = 0;
    HHOOK fallbackMouseHook_ = nullptr;
    HHOOK fallbackKeyboardHook_ = nullptr;
    HWINEVENTHOOK fallbackWinEventHook_ = nullptr;
    UINT_PTR fallbackPollTimerId_ = 0;
    uint32_t registeredHotkeysMask_ = 0;
};
