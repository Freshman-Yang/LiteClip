#pragma once

#include "platform/Win32Common.h"
#include "platform/I18n.h"
#include "ui/ThemeAndLayout.h"
#include <d2d1.h>
#include <dwrite.h>

class HotkeyConflictDialog {
public:
    HotkeyConflictDialog() = default;
    ~HotkeyConflictDialog() = default;

    HotkeyConflictDialog(const HotkeyConflictDialog&) = delete;
    HotkeyConflictDialog& operator=(const HotkeyConflictDialog&) = delete;

    // 弹出快捷键冲突解决对话框：
    // targetHwnd 为主窗口句柄（用于直接尝试绑定 HOTKEY_ID_WIN_V）；
    // initialAltVOccupied 为启动时若已配置 Alt+V 但被占用，则初始直接标红；
    // 返回 true 表示已成功注册快捷键（Alt+V 或释放后的 Win+V），返回 false 表示用户选择退出。
    static bool Show(HINSTANCE hInstance, HWND targetHwnd, bool initialAltVOccupied = false);

private:
    enum class HitZone : uint8_t {
        None = 0,
        TopBarDrag,
        CloseBtn,
        CardUseAltV,
        CardUseWinV,
        CardExit
    };

    bool RunModal(HINSTANCE hInstance, HWND targetHwnd, bool initialAltVOccupied);

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);

    bool InitD2D();
    bool CreateTextFormats();
    bool EnsureRenderTarget();
    void Render();

    HitZone HitTest(float dipX, float dipY) const noexcept;
    void OnLButtonDown(int pxX, int pxY);
    void OnMouseMove(int pxX, int pxY);
    void OnLButtonUp(int pxX, int pxY);

    void HandleSelectAltV();
    void HandleSelectWinV();
    bool ExecuteFreeSystemWinV() const;

    float PxToDip(int px) const noexcept;
    int DipToPx(float dip) const noexcept;

private:
    static constexpr wchar_t DIALOG_CLASS_NAME[] = L"LiteClip_HotkeyConflictDialog_Class";
    static constexpr float DIALOG_WIDTH_DIP  = Layout::WINDOW_WIDTH; // 360.0f
    static constexpr float DIALOG_HEIGHT_DIP = 306.0f;

    HINSTANCE hInstance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND targetHwnd_ = nullptr;
    UINT dpi_ = 96;

    bool resolved_ = false;
    bool closing_ = false;
    bool altVOccupied_ = false;
    bool busy_ = false;

    HitZone hoveredZone_ = HitZone::None;
    HitZone downZone_ = HitZone::None;
    bool isMouseDown_ = false;
    bool isMovingWindow_ = false;
    bool trackingMouseLeave_ = false;
    POINT winMoveStartCursor_{ 0, 0 };
    POINT winMoveStartOrigin_{ 0, 0 };

    ComPtr<ID2D1Factory> d2dFactory_;
    ComPtr<ID2D1HwndRenderTarget> renderTarget_;
    ComPtr<IDWriteFactory> dwriteFactory_;
    ComPtr<IDWriteTextFormat> titleFormat_;
    ComPtr<IDWriteTextFormat> promptFormat_;
    ComPtr<IDWriteTextFormat> cardTitleFormat_;
    ComPtr<IDWriteTextFormat> cardDescFormat_;
    ComPtr<IDWriteTextFormat> iconFormat_;
    ComPtr<ID2D1SolidColorBrush> solidBrush_;
};
