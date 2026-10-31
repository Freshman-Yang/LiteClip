#include "interaction/CaretLocator.h"
#include <oleacc.h>
#include <oleauto.h>

POINT CaretLocator::CalculatePopupPosition(int winWidthPx, int winHeightPx) {
    POINT anchorPt{};
    POINT caretTopPt{};
    bool fromCaret = false;

    HWND fgWnd = GetForegroundWindow();
    GUITHREADINFO gti{};
    gti.cbSize = sizeof(GUITHREADINFO);

    BOOL gotGti = GetGUIThreadInfo(0, &gti);
    if (!gotGti && fgWnd != nullptr) {
        DWORD fgThreadId = GetWindowThreadProcessId(fgWnd, nullptr);
        if (fgThreadId != 0) {
            gotGti = GetGUIThreadInfo(fgThreadId, &gti);
        }
    }

    if (gotGti && gti.hwndCaret != nullptr &&
        (gti.flags & GUI_CARETBLINKING) != 0 &&
        IsWindowVisible(gti.hwndCaret) &&
        (gti.rcCaret.left != 0 || gti.rcCaret.top != 0 ||
         gti.rcCaret.right != 0 || gti.rcCaret.bottom != 0)) {
        POINT bottomPt{ gti.rcCaret.left, gti.rcCaret.bottom };
        POINT topPt{ gti.rcCaret.left, gti.rcCaret.top };
        if (ClientToScreen(gti.hwndCaret, &bottomPt) &&
            ClientToScreen(gti.hwndCaret, &topPt) &&
            (topPt.x != 0 || topPt.y != 0)) {
            anchorPt = bottomPt;
            anchorPt.y += 8;
            caretTopPt = topPt;
            fromCaret = true;
        }
    }

    // 当传统 GetGUIThreadInfo 未能拿到有效光标时，回退使用轻量级 MSAA (oleacc.dll) 查询现代应用光标位置
    if (!fromCaret && fgWnd != nullptr) {
        auto queryAccCaret = [&](HWND targetWnd) -> bool {
            if (targetWnd == nullptr || !IsWindow(targetWnd) || IsHungAppWindow(targetWnd)) {
                return false;
            }

            // 补充 50ms 轻量探针，防止目标窗口刚卡死未满 5 秒（IsHungAppWindow 尚未置位）时阻塞主线程
            DWORD_PTR probeResult = 0;
            if (!SendMessageTimeoutW(targetWnd, WM_NULL, 0, 0,
                                     SMTO_ABORTIFHUNG | SMTO_BLOCK,
                                     50, &probeResult)) {
                return false;
            }

            ComPtr<IAccessible> pAcc;
            if (SUCCEEDED(AccessibleObjectFromWindow(targetWnd, static_cast<DWORD>(OBJID_CARET), IID_PPV_ARGS(&pAcc))) &&
                pAcc != nullptr) {
                VARIANT varSelf{};
                varSelf.vt = VT_I4;
                varSelf.lVal = CHILDID_SELF;

                VARIANT varState{};
                VariantInit(&varState);
                if (SUCCEEDED(pAcc->get_accState(varSelf, &varState))) {
                    if (varState.vt == VT_I4 &&
                        (varState.lVal & (STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_OFFSCREEN)) != 0) {
                        VariantClear(&varState);
                        return false;
                    }
                }
                VariantClear(&varState);

                LONG pxLeft = 0, pxTop = 0, pxWidth = 0, pxHeight = 0;
                if (pAcc->accLocation(&pxLeft, &pxTop, &pxWidth, &pxHeight, varSelf) == S_OK &&
                    (pxLeft != 0 || pxTop != 0)) {
                    caretTopPt = POINT{ pxLeft, pxTop };
                    anchorPt = POINT{ pxLeft, pxTop + (pxHeight > 0 ? pxHeight : 0) + 8 };
                    return true;
                }
            }
            return false;
        };

        if (queryAccCaret(fgWnd) ||
            (gti.hwndFocus != nullptr && gti.hwndFocus != fgWnd && queryAccCaret(gti.hwndFocus))) {
            fromCaret = true;
        }
    }

    if (!fromCaret) {
        if (GetCursorPos(&anchorPt)) {
            anchorPt.x += 12;
            anchorPt.y += 12;
        }
    }

    HMONITOR hMon = MonitorFromPoint(anchorPt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(MONITORINFO);

    RECT rcWork{};
    if (hMon != nullptr && GetMonitorInfoW(hMon, &mi)) {
        rcWork = mi.rcWork;
    } else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcWork, 0);
    }

    constexpr int kSafeMargin = 8;
    int x = anchorPt.x;
    int y = anchorPt.y;

    if (x + winWidthPx > rcWork.right - kSafeMargin) {
        x = rcWork.right - winWidthPx - kSafeMargin;
    }
    if (x < rcWork.left + kSafeMargin) {
        x = rcWork.left + kSafeMargin;
    }

    if (y + winHeightPx > rcWork.bottom - kSafeMargin) {
        if (fromCaret) {
            y = caretTopPt.y - winHeightPx - kSafeMargin;
            if (y + winHeightPx > rcWork.bottom - kSafeMargin) {
                y = rcWork.bottom - winHeightPx - kSafeMargin;
            }
        } else {
            y = rcWork.bottom - winHeightPx - kSafeMargin;
        }
    }
    if (y < rcWork.top + kSafeMargin) {
        y = rcWork.top + kSafeMargin;
    }

    return POINT{ x, y };
}
