#pragma once

#include "platform/Win32Common.h"
#include "core/Types.h"
#include <functional>

struct InternalDropTargetCallbacks {
    std::function<bool(POINT screenPt)> onDragEnter;
    std::function<bool(POINT screenPt)> onDragOver;
    std::function<void()> onDragLeave;
    std::function<bool(POINT screenPt)> onDrop;
};

class OleDragDrop {
public:
    // 启动同步原生 OLE 拖拽 (DoDragDrop)。
    // 1. 若 payload.type == ClipItemType::Image：封装 imageFullPath 为 CF_HDROP 格式（文件名为 截图_YYYYMMDD_HHMMSS.png），
    //    且严格锁死仅允许 DROPEFFECT_COPY（严禁 DROPEFFECT_MOVE 剪切走本地缓存文件）。
    // 2. 若是文本或富文本：封装 CF_UNICODETEXT（以及可用的 HTML Format / Rich Text Format），支持拖入文档、输入框或浏览器地址栏。
    static bool PerformDragDrop(const RawClipboardPayload& payload, const std::wstring& imageFullPath);

    // 为剪切板自身窗口注册/注销 OLE IDropTarget，以支持固定页内部拖动排序与外部 OLE 拖拽无缝切换
    static bool RegisterInternalDropTarget(HWND hwnd, InternalDropTargetCallbacks callbacks);
    static void RevokeInternalDropTarget(HWND hwnd);
};
