#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// 资源 ID
constexpr int IDI_APP_ICON = 101;

// 跨模块窗口通知消息、热键与定时器 ID
constexpr UINT WM_APP_HIDE_WINDOW         = WM_APP + 1;
constexpr UINT WM_APP_MOUSE_WHEEL         = WM_APP + 2;
constexpr UINT WM_APP_WORK_COMPLETE       = WM_APP + 3;

constexpr int HOTKEY_ID_WIN_V             = 1001;
constexpr int HOTKEY_ID_ESC               = 1002;
constexpr int HOTKEY_ID_UP                = 1003;
constexpr int HOTKEY_ID_DOWN              = 1004;
constexpr int HOTKEY_ID_ENTER             = 1005;
constexpr int HOTKEY_ID_SHIFT_ENTER       = 1006;
constexpr int HOTKEY_ID_DELETE            = 1007;
constexpr UINT_PTR TIMER_ID_CLIP_DEBOUNCE = 2002;
constexpr UINT_PTR TIMER_ID_DRAG_SCROLL   = 2003;

