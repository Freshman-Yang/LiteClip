#pragma once

#include "platform/Win32Common.h"
#include <cstdint>

enum class AppLanguage : uint8_t {
    ZhCN = 0,
    EnUS = 1
};

enum class AppHotkey : uint8_t {
    WinV = 0,
    AltV = 1
};

struct I18nStrings {
    const wchar_t* localeName;            // DirectWrite 本地化标识
    const wchar_t* fallbackFontFamily;    // DirectWrite 回退字体
    const wchar_t* appTitle;              // 顶栏标题
    const wchar_t* clearAll;              // 导航栏“全部清除”按钮
    const wchar_t* emptyHint;             // 空列表状态提示
    const wchar_t* pinItem;               // 卡片右侧操作按钮提示：固定
    const wchar_t* unpinItem;             // 卡片右侧操作按钮提示：取消固定
    const wchar_t* plainTextPaste;        // 卡片右侧操作按钮提示：粘贴为纯文本
    const wchar_t* deleteItem;            // 卡片右侧操作按钮提示：删除
    const wchar_t* imgPreviewUnavailable; // 图片预览不可用占位文本
    const wchar_t* screenshotPrefix;      // 截图落盘文件名前缀
    const wchar_t* errWorkerFailed;       // 后台任务异常提示
    const wchar_t* errClearSaveFailed;    // 清空记录时保存固定索引失败提示
    const wchar_t* errPinSaveFailed;      // 切换固定状态时保存固定索引失败提示
    const wchar_t* errDeleteFailed;       // 删除记录失败提示
    const wchar_t* errLoadPinnedFailed;   // 启动时读取固定记录索引失败提示

    // 快捷键冲突弹窗文案
    const wchar_t* conflictTitle;         // 冲突弹窗顶栏标题
    const wchar_t* conflictPrompt;        // 冲突弹窗顶部说明
    const wchar_t* optUseAltVTitle;       // 选项 1 标题：使用 Alt + V
    const wchar_t* optUseAltVDesc;        // 选项 1 副标题
    const wchar_t* optOccupiedTag;        // 选项后标红：已占用
    const wchar_t* optUseWinVTitle;       // 选项 2 标题：使用 Win + V
    const wchar_t* optUseWinVDesc;        // 选项 2 副标题
    const wchar_t* optExitTitle;          // 选项 3 标题：退出程序
};

namespace I18n {

// 初始化语言配置：
// 1. 尝试从注册表 HKCU\Software\LiteClip 读取 Language (REG_SZ)。
// 2. 若首次启动（未配置或值无效），读取系统默认 UI 语言 (GetUserDefaultUILanguage)，
//    中文系统设为 "zh-CN"，其余系统设为 "en-US"，并写入注册表持久化。
void Initialize() noexcept;

AppLanguage GetLanguage() noexcept;

const I18nStrings& Get() noexcept;

// 从注册表 HKCU\Software\LiteClip 读取/写入快捷键配置（与 Language 同级，键名为 Hotkey）
AppHotkey ReadHotkeyFromRegistry() noexcept;
void WriteHotkeyToRegistry(AppHotkey hotkey) noexcept;

} // namespace I18n
