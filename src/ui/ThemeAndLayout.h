#pragma once

#include "platform/Win32Common.h"
#include <d2d1.h>

// UI 尺寸规范 (单位: DIP, 设备无关像素)
namespace Layout {
    constexpr float WINDOW_WIDTH       = 360.0f;
    constexpr float TOP_BAR_HEIGHT     = 40.0f;
    constexpr float NAV_BAR_HEIGHT     = 36.0f;
    constexpr float LIST_PAD_TOP       = 8.0f;
    constexpr float LIST_PAD_BOTTOM    = 8.0f;
    constexpr float LIST_PAD_HORZ      = 12.0f;

    // 卡片高度严格遵循规格书 3.3：
    // 文本项 = 1 个标准单位高度 (76 DIP)，间距 = 8 DIP
    // 图片项 = 2 个文本项高度总和（含间距）= 76 * 2 + 8 = 160 DIP
    // 可视区域固定完整展示 4 个标准文本项高度（4 * 76 + 3 * 8 + 上下内边距 16 = 344 DIP）
    constexpr float CARD_TEXT_HEIGHT   = 76.0f;
    constexpr float CARD_GAP           = 8.0f;
    constexpr float CARD_IMAGE_HEIGHT  = CARD_TEXT_HEIGHT * 2.0f + CARD_GAP; // 160.0f
    constexpr float VIEWPORT_HEIGHT    = LIST_PAD_TOP + (CARD_TEXT_HEIGHT * 4.0f + CARD_GAP * 3.0f) + LIST_PAD_BOTTOM; // 344.0f
    constexpr float WINDOW_HEIGHT      = TOP_BAR_HEIGHT + NAV_BAR_HEIGHT + VIEWPORT_HEIGHT; // 420.0f

    constexpr float CARD_ACTION_WIDTH  = 32.0f;  // 卡片右侧操作区宽度
}

// Segoe Fluent Icons / Segoe MDL2 Assets 字符码
namespace FluentIcons {
    constexpr wchar_t Close[]          = L"\xE8BB"; // × 关闭
    constexpr wchar_t HistoryTab[]     = L"\xF0E3"; // 剪切板历史列表图标
    constexpr wchar_t PinUnpinned[]    = L"\xE718"; // 空心/常规图钉
    constexpr wchar_t PinPinned[]      = L"\xE840"; // 固定态图钉
    constexpr wchar_t PlainText[]      = L"\xE8A5"; // 纯文本粘贴图标
    constexpr wchar_t DeleteTrash[]    = L"\xE74D"; // 删除垃圾桶图标
}

// Windows 11 Fluent 2 Light 现代简约调色板
namespace Palette {
    constexpr D2D1_COLOR_F WindowBg            = { 0xF4 / 255.0f, 0xF5 / 255.0f, 0xF7 / 255.0f, 1.0f }; // #F4F5F7 柔和冷灰画布
    constexpr D2D1_COLOR_F CardBg              = { 1.0f,          1.0f,          1.0f,          1.0f }; // #FFFFFF
    constexpr D2D1_COLOR_F CardHoverBg         = { 0xFC / 255.0f, 0xFD / 255.0f, 0xFE / 255.0f, 1.0f }; // #FCFDFE
    constexpr D2D1_COLOR_F CardSelectedBg      = { 0xF4 / 255.0f, 0xF8 / 255.0f, 0xFD / 255.0f, 1.0f }; // #F4F8FD 极淡冰蓝选中底
    constexpr D2D1_COLOR_F CardSelectedHoverBg = { 0xEE / 255.0f, 0xF5 / 255.0f, 0xFC / 255.0f, 1.0f }; // #EEF5FC
    constexpr D2D1_COLOR_F CardBorder          = { 0.0f,          0.0f,          0.0f,          0.058f };
    constexpr D2D1_COLOR_F CardHoverBorder     = { 0.0f,          0.0f,          0.0f,          0.115f };
    constexpr D2D1_COLOR_F CardSelectedBorder  = { 0x00 / 255.0f, 0x67 / 255.0f, 0xC0 / 255.0f, 0.24f };
    constexpr D2D1_COLOR_F CardShadow          = { 0.0f,          0.0f,          0.0f,          0.032f };
    constexpr D2D1_COLOR_F NavTrackBg          = { 0.0f,          0.0f,          0.0f,          0.052f };
    constexpr D2D1_COLOR_F NavActivePillBg     = { 1.0f,          1.0f,          1.0f,          1.0f };
    constexpr D2D1_COLOR_F NavActivePillBorder = { 0.0f,          0.0f,          0.0f,          0.06f };
    constexpr D2D1_COLOR_F SlideDeleteHover    = { 0xFD / 255.0f, 0xE7 / 255.0f, 0xE9 / 255.0f, 1.0f }; // #FDE7E9 危险操作柔和红底
    constexpr D2D1_COLOR_F TextPrimary         = { 0x1F / 255.0f, 0x23 / 255.0f, 0x28 / 255.0f, 1.0f }; // #1F2328
    constexpr D2D1_COLOR_F TextSecondary       = { 0x63 / 255.0f, 0x68 / 255.0f, 0x70 / 255.0f, 1.0f }; // #636870
    constexpr D2D1_COLOR_F TextMuted           = { 0x91 / 255.0f, 0x96 / 255.0f, 0x9E / 255.0f, 1.0f }; // #91969E
    constexpr D2D1_COLOR_F AccentBlue          = { 0x00 / 255.0f, 0x67 / 255.0f, 0xC0 / 255.0f, 1.0f }; // #0067C0
    constexpr D2D1_COLOR_F AccentBlueSubtle    = { 0x00 / 255.0f, 0x67 / 255.0f, 0xC0 / 255.0f, 0.09f };
    constexpr D2D1_COLOR_F PinnedBadgeIdle     = { 0x00 / 255.0f, 0x67 / 255.0f, 0xC0 / 255.0f, 0.68f };
    constexpr D2D1_COLOR_F DangerRed           = { 0xC4 / 255.0f, 0x2B / 255.0f, 0x1C / 255.0f, 1.0f }; // #C42B1C
    constexpr D2D1_COLOR_F SubtleHoverFill     = { 0.0f,          0.0f,          0.0f,          0.048f };
    constexpr D2D1_COLOR_F ActionHoverFill     = { 0.0f,          0.0f,          0.0f,          0.065f };
    constexpr D2D1_COLOR_F CloseHoverBg        = { 0xE8 / 255.0f, 0x11 / 255.0f, 0x23 / 255.0f, 0.12f };
    constexpr D2D1_COLOR_F ScrollbarThumb      = { 0x5C / 255.0f, 0x61 / 255.0f, 0x68 / 255.0f, 0.36f };
    constexpr D2D1_COLOR_F DividerLine         = { 0.0f,          0.0f,          0.0f,          0.06f };
    constexpr D2D1_COLOR_F ImagePlaceholder    = { 0xF2 / 255.0f, 0xF4 / 255.0f, 0xF7 / 255.0f, 1.0f };
    constexpr D2D1_COLOR_F TooltipBg           = { 0x24 / 255.0f, 0x29 / 255.0f, 0x2F / 255.0f, 0.94f }; // #24292F 悬浮提示深色底
    constexpr D2D1_COLOR_F TooltipBorder       = { 1.0f,          1.0f,          1.0f,          0.12f }; // 悬浮提示微光描边
    constexpr D2D1_COLOR_F TooltipShadow       = { 0.0f,          0.0f,          0.0f,          0.16f }; // 悬浮提示投影
    constexpr D2D1_COLOR_F TooltipText         = { 1.0f,          1.0f,          1.0f,          1.0f };  // #FFFFFF 悬浮提示文字
}
