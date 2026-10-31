#pragma once

#include "platform/Win32Common.h"

class CaretLocator {
public:
    // 计算窗口呼出时的屏幕物理像素左上角坐标 (X, Y)
    // winWidthPx, winHeightPx 为当前 DPI 下剪切板窗口的物理像素宽高
    static POINT CalculatePopupPosition(int winWidthPx, int winHeightPx);
};
