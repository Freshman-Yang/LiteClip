#include "ui/MainWindow.h"
#include "platform/I18n.h"
#include <ole2.h>

int WINAPI wWinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE /*hPrevInstance*/,
    _In_ LPWSTR /*lpCmdLine*/,
    _In_ int /*nCmdShow*/
) {
    // 开启 Per-Monitor V2 高分屏 DPI 感知
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // 全局单实例命名互斥体检查
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Local\\LiteClip_SingleInstance_Mutex");
    if (!hMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (hMutex) {
            CloseHandle(hMutex);
        }
        return 0;
    }

    // 初始化多语言支持（初次启动读取系统语言并写入注册表 HKCU\Software\LiteClip）
    I18n::Initialize();

    // 初始化 OLE 库（支持 OLE Drag & Drop 和 WIC / D2D 组件）
    const HRESULT hr = OleInitialize(nullptr);
    if (FAILED(hr)) {
        CloseHandle(hMutex);
        return -1;
    }

    int exitCode = 0;
    {
        MainWindow mainWindow;
        if (mainWindow.Initialize(hInstance)) {
            MSG msg{};
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            exitCode = static_cast<int>(msg.wParam);
        } else {
            exitCode = -1;
        }
    }

    OleUninitialize();
    ReleaseMutex(hMutex);
    CloseHandle(hMutex);

    return exitCode;
}
