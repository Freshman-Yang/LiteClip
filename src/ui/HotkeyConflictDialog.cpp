#include "ui/HotkeyConflictDialog.h"
#include "ui/VirtualListLayout.h"

#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <cmath>
#include <cwchar>

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

namespace {

constexpr float kCardRadius    = 8.0f;
constexpr float kControlRadius = 6.0f;

constexpr D2D1_RECT_F kPromptRect   = { 16.0f, 48.0f, 344.0f, 84.0f };
constexpr D2D1_RECT_F kCardAltVRect = { 12.0f, 90.0f, 348.0f, 162.0f };
constexpr D2D1_RECT_F kCardWinVRect = { 12.0f, 170.0f, 348.0f, 242.0f };
constexpr D2D1_RECT_F kCardExitRect = { 12.0f, 250.0f, 348.0f, 294.0f };

bool IsFontInstalled(IDWriteFactory* dwriteFactory, const wchar_t* familyName) {
    if (!dwriteFactory || !familyName) {
        return false;
    }
    ComPtr<IDWriteFontCollection> fontCollection;
    if (FAILED(dwriteFactory->GetSystemFontCollection(&fontCollection, FALSE)) || !fontCollection) {
        return false;
    }
    UINT32 index = 0;
    BOOL exists = FALSE;
    if (FAILED(fontCollection->FindFamilyName(familyName, &index, &exists))) {
        return false;
    }
    return exists != FALSE;
}

HRESULT CreateFormatWithFallback(
    IDWriteFactory* dwriteFactory,
    const wchar_t* primaryFamily,
    const wchar_t* fallbackFamily,
    DWRITE_FONT_WEIGHT weight,
    float fontSizeDip,
    IDWriteTextFormat** outFormat) {
    if (!dwriteFactory || !outFormat) {
        return E_INVALIDARG;
    }

    const wchar_t* chosenFamily = IsFontInstalled(dwriteFactory, primaryFamily)
        ? primaryFamily
        : fallbackFamily;
    const wchar_t* localeName = I18n::Get().localeName;

    HRESULT hr = dwriteFactory->CreateTextFormat(
        chosenFamily,
        nullptr,
        weight,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        fontSizeDip,
        localeName,
        outFormat);

    if (FAILED(hr) && chosenFamily != fallbackFamily) {
        hr = dwriteFactory->CreateTextFormat(
            fallbackFamily,
            nullptr,
            weight,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            fontSizeDip,
            localeName,
            outFormat);
    }
    return hr;
}

} // namespace

bool HotkeyConflictDialog::Show(HINSTANCE hInstance, HWND targetHwnd, bool initialAltVOccupied) {
    HotkeyConflictDialog dlg;
    return dlg.RunModal(hInstance, targetHwnd, initialAltVOccupied);
}

float HotkeyConflictDialog::PxToDip(int px) const noexcept {
    return static_cast<float>(px) * 96.0f / static_cast<float>(dpi_);
}

int HotkeyConflictDialog::DipToPx(float dip) const noexcept {
    return MulDiv(static_cast<int>(std::lround(dip)), static_cast<int>(dpi_), 96);
}

bool HotkeyConflictDialog::RunModal(HINSTANCE hInstance, HWND targetHwnd, bool initialAltVOccupied) {
    hInstance_ = hInstance;
    targetHwnd_ = targetHwnd;
    altVOccupied_ = initialAltVOccupied;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = HotkeyConflictDialog::WindowProc;
    wc.hInstance = hInstance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hInstance_, MAKEINTRESOURCEW(IDI_APP_ICON));
    if (!wc.hIcon) {
        wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = DIALOG_CLASS_NAME;

    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    POINT cursorPt{};
    GetCursorPos(&cursorPt);
    HMONITOR hMon = MonitorFromPoint(cursorPt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(MONITORINFO);
    RECT workArea{ 0, 0, 1920, 1080 };
    if (GetMonitorInfoW(hMon, &mi)) {
        workArea = mi.rcWork;
    }

    dpi_ = GetDpiForSystem();
    const int initW = DipToPx(DIALOG_WIDTH_DIP);
    const int initH = DipToPx(DIALOG_HEIGHT_DIP);
    const int initX = workArea.left + ((workArea.right - workArea.left) - initW) / 2;
    const int initY = workArea.top + ((workArea.bottom - workArea.top) - initH) / 2;

    hwnd_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_APPWINDOW,
        DIALOG_CLASS_NAME,
        I18n::Get().conflictTitle,
        WS_POPUP,
        initX, initY, initW, initH,
        nullptr,
        nullptr,
        hInstance_,
        this);

    if (!hwnd_) {
        return false;
    }

    dpi_ = GetDpiForWindow(hwnd_);
    const int actualW = DipToPx(DIALOG_WIDTH_DIP);
    const int actualH = DipToPx(DIALOG_HEIGHT_DIP);
    const int actualX = workArea.left + ((workArea.right - workArea.left) - actualW) / 2;
    const int actualY = workArea.top + ((workArea.bottom - workArea.top) - actualH) / 2;
    SetWindowPos(hwnd_, HWND_TOPMOST, actualX, actualY, actualW, actualH, SWP_NOACTIVATE);

    const DWORD cornerPreference = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(
        hwnd_,
        DWMWA_WINDOW_CORNER_PREFERENCE,
        &cornerPreference,
        sizeof(cornerPreference));

    if (!InitD2D()) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    SetForegroundWindow(hwnd_);
    Render();

    MSG msg{};
    while (!closing_ && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE && !busy_) {
            closing_ = true;
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (hwnd_ && IsWindow(hwnd_)) {
        DestroyWindow(hwnd_);
    }
    hwnd_ = nullptr;
    return resolved_;
}

bool HotkeyConflictDialog::InitD2D() {
    if (!d2dFactory_) {
        D2D1_FACTORY_OPTIONS options{};
        HRESULT hr = D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory),
            &options,
            reinterpret_cast<void**>(d2dFactory_.ReleaseAndGetAddressOf()));
        if (FAILED(hr)) {
            return false;
        }
    }

    if (!dwriteFactory_) {
        HRESULT hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(dwriteFactory_.ReleaseAndGetAddressOf()));
        if (FAILED(hr)) {
            return false;
        }
    }

    if (!CreateTextFormats()) {
        return false;
    }

    return EnsureRenderTarget();
}

bool HotkeyConflictDialog::CreateTextFormats() {
    const wchar_t* fallbackFont = I18n::Get().fallbackFontFamily;

    // 1. 顶部标题格式：14pt SemiBold
    HRESULT hr = CreateFormatWithFallback(
        dwriteFactory_.Get(),
        L"Segoe UI Variable Text",
        fallbackFont,
        DWRITE_FONT_WEIGHT_SEMI_BOLD,
        14.0f,
        titleFormat_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;
    titleFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    titleFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    titleFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    // 2. 顶部提示语格式：12.5pt Normal
    hr = CreateFormatWithFallback(
        dwriteFactory_.Get(),
        L"Segoe UI Variable Text",
        fallbackFont,
        DWRITE_FONT_WEIGHT_NORMAL,
        12.5f,
        promptFormat_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;
    promptFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    promptFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    promptFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);

    // 3. 卡片主标题格式：13pt SemiBold
    hr = CreateFormatWithFallback(
        dwriteFactory_.Get(),
        L"Segoe UI Variable Text",
        fallbackFont,
        DWRITE_FONT_WEIGHT_SEMI_BOLD,
        13.0f,
        cardTitleFormat_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;
    cardTitleFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    cardTitleFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    cardTitleFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    // 4. 卡片副标题格式：12pt Normal
    hr = CreateFormatWithFallback(
        dwriteFactory_.Get(),
        L"Segoe UI Variable Text",
        fallbackFont,
        DWRITE_FONT_WEIGHT_NORMAL,
        12.0f,
        cardDescFormat_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;
    cardDescFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    cardDescFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    cardDescFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);

    // 5. Fluent 图标格式
    hr = CreateFormatWithFallback(
        dwriteFactory_.Get(),
        L"Segoe Fluent Icons",
        L"Segoe MDL2 Assets",
        DWRITE_FONT_WEIGHT_NORMAL,
        13.0f,
        iconFormat_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;
    iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    iconFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    return true;
}

bool HotkeyConflictDialog::EnsureRenderTarget() {
    if (!hwnd_ || !d2dFactory_) {
        return false;
    }
    if (renderTarget_) {
        return true;
    }

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const UINT widthPx = static_cast<UINT>(std::max<LONG>(1, rc.right - rc.left));
    const UINT heightPx = static_cast<UINT>(std::max<LONG>(1, rc.bottom - rc.top));

    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties();
    rtProps.dpiX = static_cast<float>(dpi_);
    rtProps.dpiY = static_cast<float>(dpi_);

    D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProps = D2D1::HwndRenderTargetProperties(
        hwnd_,
        D2D1::SizeU(widthPx, heightPx));

    HRESULT hr = d2dFactory_->CreateHwndRenderTarget(
        rtProps,
        hwndProps,
        renderTarget_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        return false;
    }

    hr = renderTarget_->CreateSolidColorBrush(
        Palette::TextPrimary,
        solidBrush_.ReleaseAndGetAddressOf());
    return SUCCEEDED(hr);
}

void HotkeyConflictDialog::Render() {
    if (!EnsureRenderTarget()) {
        return;
    }

    const auto& s = I18n::Get();
    renderTarget_->BeginDraw();
    renderTarget_->Clear(Palette::WindowBg);

    // 1. 顶栏标题与关闭按钮
    const D2D1_RECT_F titleRect = D2D1::RectF(16.0f, 0.0f, 310.0f, Layout::TOP_BAR_HEIGHT);
    solidBrush_->SetColor(Palette::TextPrimary);
    renderTarget_->DrawText(
        s.conflictTitle,
        static_cast<UINT32>(std::wcslen(s.conflictTitle)),
        titleFormat_.Get(),
        titleRect,
        solidBrush_.Get(),
        D2D1_DRAW_TEXT_OPTIONS_NONE);

    const bool closeHovered = (hoveredZone_ == HitZone::CloseBtn);
    if (closeHovered) {
        solidBrush_->SetColor(Palette::CloseHoverBg);
        renderTarget_->FillRoundedRectangle(
            D2D1::RoundedRect(kCloseBtnRect, kControlRadius, kControlRadius),
            solidBrush_.Get());
    }
    solidBrush_->SetColor(closeHovered ? Palette::DangerRed : Palette::TextSecondary);
    renderTarget_->DrawText(
        FluentIcons::Close,
        static_cast<UINT32>(std::wcslen(FluentIcons::Close)),
        iconFormat_.Get(),
        kCloseBtnRect,
        solidBrush_.Get(),
        D2D1_DRAW_TEXT_OPTIONS_NONE);

    // 顶栏底部分隔线
    solidBrush_->SetColor(Palette::DividerLine);
    renderTarget_->DrawLine(
        D2D1::Point2F(Layout::LIST_PAD_HORZ, Layout::TOP_BAR_HEIGHT - 0.5f),
        D2D1::Point2F(DIALOG_WIDTH_DIP - Layout::LIST_PAD_HORZ, Layout::TOP_BAR_HEIGHT - 0.5f),
        solidBrush_.Get(),
        1.0f);

    // 2. 冲突提示说明文字
    solidBrush_->SetColor(Palette::TextSecondary);
    renderTarget_->DrawText(
        s.conflictPrompt,
        static_cast<UINT32>(std::wcslen(s.conflictPrompt)),
        promptFormat_.Get(),
        kPromptRect,
        solidBrush_.Get(),
        D2D1_DRAW_TEXT_OPTIONS_NONE);

    // 3. 选项卡片 1：使用 Alt + V
    {
        const bool hovered = (hoveredZone_ == HitZone::CardUseAltV);
        const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(kCardAltVRect, kCardRadius, kCardRadius);
        solidBrush_->SetColor(hovered ? Palette::CardHoverBg : Palette::CardBg);
        renderTarget_->FillRoundedRectangle(rr, solidBrush_.Get());
        solidBrush_->SetColor(hovered ? Palette::CardHoverBorder : Palette::CardBorder);
        renderTarget_->DrawRoundedRectangle(rr, solidBrush_.Get(), 1.0f);

        const UINT32 titleLen = static_cast<UINT32>(std::wcslen(s.optUseAltVTitle));
        const D2D1_RECT_F cardTitleRect = D2D1::RectF(
            kCardAltVRect.left + 14.0f,
            kCardAltVRect.top + 10.0f,
            kCardAltVRect.right - 14.0f,
            kCardAltVRect.top + 32.0f);

        solidBrush_->SetColor(Palette::TextPrimary);
        renderTarget_->DrawText(
            s.optUseAltVTitle,
            titleLen,
            cardTitleFormat_.Get(),
            cardTitleRect,
            solidBrush_.Get(),
            D2D1_DRAW_TEXT_OPTIONS_NONE);

        // 若 Alt + V 也被占用，在标题后方标红显示“（已占用）”/ "(Occupied)"
        if (altVOccupied_ && dwriteFactory_) {
            float occupiedOffsetX = 92.0f;
            ComPtr<IDWriteTextLayout> textLayout;
            if (SUCCEEDED(dwriteFactory_->CreateTextLayout(
                    s.optUseAltVTitle,
                    titleLen,
                    cardTitleFormat_.Get(),
                    300.0f,
                    22.0f,
                    textLayout.GetAddressOf())) && textLayout) {
                DWRITE_TEXT_METRICS metrics{};
                if (SUCCEEDED(textLayout->GetMetrics(&metrics))) {
                    occupiedOffsetX = metrics.widthIncludingTrailingWhitespace + 6.0f;
                }
            }

            const D2D1_RECT_F occupiedRect = D2D1::RectF(
                cardTitleRect.left + occupiedOffsetX,
                cardTitleRect.top,
                kCardAltVRect.right - 12.0f,
                cardTitleRect.bottom);
            solidBrush_->SetColor(Palette::DangerRed);
            renderTarget_->DrawText(
                s.optOccupiedTag,
                static_cast<UINT32>(std::wcslen(s.optOccupiedTag)),
                cardTitleFormat_.Get(),
                occupiedRect,
                solidBrush_.Get(),
                D2D1_DRAW_TEXT_OPTIONS_NONE);
        }

        const D2D1_RECT_F cardDescRect = D2D1::RectF(
            kCardAltVRect.left + 14.0f,
            kCardAltVRect.top + 35.0f,
            kCardAltVRect.right - 14.0f,
            kCardAltVRect.bottom - 8.0f);
        solidBrush_->SetColor(Palette::TextSecondary);
        renderTarget_->DrawText(
            s.optUseAltVDesc,
            static_cast<UINT32>(std::wcslen(s.optUseAltVDesc)),
            cardDescFormat_.Get(),
            cardDescRect,
            solidBrush_.Get(),
            D2D1_DRAW_TEXT_OPTIONS_NONE);
    }

    // 4. 选项卡片 2：使用 Win + V
    {
        const bool hovered = (hoveredZone_ == HitZone::CardUseWinV);
        const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(kCardWinVRect, kCardRadius, kCardRadius);
        solidBrush_->SetColor(hovered ? Palette::CardHoverBg : Palette::CardBg);
        renderTarget_->FillRoundedRectangle(rr, solidBrush_.Get());
        solidBrush_->SetColor(hovered ? Palette::CardHoverBorder : Palette::CardBorder);
        renderTarget_->DrawRoundedRectangle(rr, solidBrush_.Get(), 1.0f);

        const D2D1_RECT_F cardTitleRect = D2D1::RectF(
            kCardWinVRect.left + 14.0f,
            kCardWinVRect.top + 10.0f,
            kCardWinVRect.right - 14.0f,
            kCardWinVRect.top + 32.0f);
        solidBrush_->SetColor(Palette::TextPrimary);
        renderTarget_->DrawText(
            s.optUseWinVTitle,
            static_cast<UINT32>(std::wcslen(s.optUseWinVTitle)),
            cardTitleFormat_.Get(),
            cardTitleRect,
            solidBrush_.Get(),
            D2D1_DRAW_TEXT_OPTIONS_NONE);

        const D2D1_RECT_F cardDescRect = D2D1::RectF(
            kCardWinVRect.left + 14.0f,
            kCardWinVRect.top + 35.0f,
            kCardWinVRect.right - 14.0f,
            kCardWinVRect.bottom - 6.0f);
        solidBrush_->SetColor(Palette::TextSecondary);
        renderTarget_->DrawText(
            s.optUseWinVDesc,
            static_cast<UINT32>(std::wcslen(s.optUseWinVDesc)),
            cardDescFormat_.Get(),
            cardDescRect,
            solidBrush_.Get(),
            D2D1_DRAW_TEXT_OPTIONS_NONE);
    }

    // 5. 选项卡片 3：退出程序（单行无副标题）
    {
        const bool hovered = (hoveredZone_ == HitZone::CardExit);
        const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(kCardExitRect, kCardRadius, kCardRadius);
        solidBrush_->SetColor(hovered ? Palette::SlideDeleteHover : Palette::CardBg);
        renderTarget_->FillRoundedRectangle(rr, solidBrush_.Get());
        solidBrush_->SetColor(hovered ? Palette::CardHoverBorder : Palette::CardBorder);
        renderTarget_->DrawRoundedRectangle(rr, solidBrush_.Get(), 1.0f);

        const D2D1_RECT_F exitTitleRect = D2D1::RectF(
            kCardExitRect.left + 14.0f,
            kCardExitRect.top,
            kCardExitRect.right - 14.0f,
            kCardExitRect.bottom);
        solidBrush_->SetColor(hovered ? Palette::DangerRed : Palette::TextPrimary);
        renderTarget_->DrawText(
            s.optExitTitle,
            static_cast<UINT32>(std::wcslen(s.optExitTitle)),
            cardTitleFormat_.Get(),
            exitTitleRect,
            solidBrush_.Get(),
            D2D1_DRAW_TEXT_OPTIONS_NONE);
    }

    const HRESULT hr = renderTarget_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        renderTarget_.Reset();
        solidBrush_.Reset();
    }
}

HotkeyConflictDialog::HitZone HotkeyConflictDialog::HitTest(float dipX, float dipY) const noexcept {
    if (PointInRect(dipX, dipY, kCloseBtnRect)) {
        return HitZone::CloseBtn;
    }
    if (dipY >= 0.0f && dipY < Layout::TOP_BAR_HEIGHT) {
        return HitZone::TopBarDrag;
    }
    if (PointInRect(dipX, dipY, kCardAltVRect)) {
        return HitZone::CardUseAltV;
    }
    if (PointInRect(dipX, dipY, kCardWinVRect)) {
        return HitZone::CardUseWinV;
    }
    if (PointInRect(dipX, dipY, kCardExitRect)) {
        return HitZone::CardExit;
    }
    return HitZone::None;
}

void HotkeyConflictDialog::OnLButtonDown(int pxX, int pxY) {
    if (busy_) return;
    const float dipX = PxToDip(pxX);
    const float dipY = PxToDip(pxY);

    downZone_ = HitTest(dipX, dipY);
    isMouseDown_ = true;
    isMovingWindow_ = false;

    if (downZone_ == HitZone::TopBarDrag) {
        isMovingWindow_ = true;
        GetCursorPos(&winMoveStartCursor_);
        RECT rc{};
        GetWindowRect(hwnd_, &rc);
        winMoveStartOrigin_ = { rc.left, rc.top };
    }

    SetCapture(hwnd_);
}

void HotkeyConflictDialog::OnMouseMove(int pxX, int pxY) {
    if (busy_) return;

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
            SetWindowPos(hwnd_, nullptr, newX, newY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return;
    }

    const float dipX = PxToDip(pxX);
    const float dipY = PxToDip(pxY);
    const HitZone hit = HitTest(dipX, dipY);
    if (hit != hoveredZone_) {
        hoveredZone_ = hit;
        Render();
    }
}

void HotkeyConflictDialog::OnLButtonUp(int pxX, int pxY) {
    if (!isMouseDown_) return;

    const bool wasMoving = isMovingWindow_;
    const HitZone pressedZone = downZone_;

    isMouseDown_ = false;
    isMovingWindow_ = false;
    downZone_ = HitZone::None;

    if (GetCapture() == hwnd_) {
        ReleaseCapture();
    }

    if (wasMoving || busy_) {
        return;
    }

    const float dipX = PxToDip(pxX);
    const float dipY = PxToDip(pxY);
    const HitZone upZone = HitTest(dipX, dipY);

    if (upZone != pressedZone) {
        return;
    }

    switch (upZone) {
    case HitZone::CloseBtn:
    case HitZone::CardExit:
        resolved_ = false;
        closing_ = true;
        break;

    case HitZone::CardUseAltV:
        HandleSelectAltV();
        break;

    case HitZone::CardUseWinV:
        HandleSelectWinV();
        break;

    default:
        break;
    }
}

void HotkeyConflictDialog::HandleSelectAltV() {
    if (RegisterHotKey(targetHwnd_, HOTKEY_ID_WIN_V, MOD_ALT | MOD_NOREPEAT, 'V')) {
        I18n::WriteHotkeyToRegistry(AppHotkey::AltV);
        resolved_ = true;
        closing_ = true;
    } else {
        altVOccupied_ = true;
        Render();
    }
}

void HotkeyConflictDialog::HandleSelectWinV() {
    busy_ = true;
    HCURSOR hWait = LoadCursorW(nullptr, IDC_WAIT);
    HCURSOR hPrev = SetCursor(hWait);

    const bool executed = ExecuteFreeSystemWinV();
    bool registered = false;

    if (executed) {
        // 等待 explorer.exe 重启并释放对 Win + V 的占用
        for (int i = 0; i < 30; ++i) {
            if (RegisterHotKey(targetHwnd_, HOTKEY_ID_WIN_V, MOD_WIN | MOD_NOREPEAT, 'V')) {
                registered = true;
                break;
            }
            Sleep(100);
        }
    }

    SetCursor(hPrev);
    busy_ = false;

    if (registered) {
        I18n::WriteHotkeyToRegistry(AppHotkey::WinV);
        resolved_ = true;
        closing_ = true;
    } else {
        if (hwnd_ && IsWindow(hwnd_)) {
            SetForegroundWindow(hwnd_);
            Render();
        }
    }
}

bool HotkeyConflictDialog::ExecuteFreeSystemWinV() const {
    // 执行 README 中的指令：禁用系统剪切板历史、释放 Win+V 占用并重启 explorer.exe（请求管理员权限）
    constexpr wchar_t kCmdArgs[] =
        L"/c reg add \"HKCU\\Software\\Microsoft\\Clipboard\" /v EnableClipboardHistory /t REG_DWORD /d 0 /f "
        L"& reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced\" /v DisabledHotkeys /t REG_SZ /d \"V\" /f "
        L"& taskkill /f /im explorer.exe & start explorer.exe";

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(SHELLEXECUTEINFOW);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = hwnd_;
    sei.lpVerb = L"runas";
    sei.lpFile = L"cmd.exe";
    sei.lpParameters = kCmdArgs;
    sei.nShow = SW_HIDE;

    if (!ShellExecuteExW(&sei)) {
        return false;
    }

    // 同步确保当前用户会话 HKCU 下键值已写入（防止管理员账户与当前登录标准用户分离导致 HKCU 指向不同配置单元）
    {
        HKEY hClipKey = nullptr;
        if (RegCreateKeyExW(
                HKEY_CURRENT_USER,
                L"Software\\Microsoft\\Clipboard",
                0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &hClipKey, nullptr) == ERROR_SUCCESS) {
            const DWORD val = 0;
            RegSetValueExW(hClipKey, L"EnableClipboardHistory", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&val), sizeof(val));
            RegCloseKey(hClipKey);
        }

        HKEY hAdvKey = nullptr;
        if (RegCreateKeyExW(
                HKEY_CURRENT_USER,
                L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &hAdvKey, nullptr) == ERROR_SUCCESS) {
            constexpr wchar_t kVal[] = L"V";
            RegSetValueExW(hAdvKey, L"DisabledHotkeys", 0, REG_SZ, reinterpret_cast<const BYTE*>(kVal), sizeof(kVal));
            RegCloseKey(hAdvKey);
        }
    }

    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 8000);
        CloseHandle(sei.hProcess);
    }

    return true;
}

LRESULT CALLBACK HotkeyConflictDialog::WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    HotkeyConflictDialog* self = nullptr;
    if (uMsg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<HotkeyConflictDialog*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<HotkeyConflictDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        return self->HandleMessage(uMsg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

LRESULT HotkeyConflictDialog::HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd_, &ps);
        Render();
        EndPaint(hwnd_, &ps);
        return 0;
    }

    case WM_DPICHANGED: {
        dpi_ = HIWORD(wParam);
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        if (suggested) {
            const int width = suggested->right - suggested->left;
            const int height = suggested->bottom - suggested->top;
            SetWindowPos(
                hwnd_,
                nullptr,
                suggested->left,
                suggested->top,
                width,
                height,
                SWP_NOZORDER | SWP_NOACTIVATE);
            if (renderTarget_) {
                renderTarget_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
                renderTarget_->Resize(D2D1::SizeU(static_cast<UINT>(width), static_cast<UINT>(height)));
            }
            Render();
        }
        return 0;
    }

    case WM_LBUTTONDOWN:
        OnLButtonDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_MOUSEMOVE:
        OnMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_LBUTTONUP:
        OnLButtonUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_CAPTURECHANGED:
        isMouseDown_ = false;
        isMovingWindow_ = false;
        downZone_ = HitZone::None;
        return 0;

    case WM_MOUSELEAVE:
        trackingMouseLeave_ = false;
        if (hoveredZone_ != HitZone::None) {
            hoveredZone_ = HitZone::None;
            Render();
        }
        return 0;

    case WM_CLOSE:
        if (!busy_) {
            resolved_ = false;
            closing_ = true;
        }
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd_, uMsg, wParam, lParam);
}





