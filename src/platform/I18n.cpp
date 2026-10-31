#include "platform/I18n.h"

#include <atomic>
#include <cwchar>
#include <string_view>

namespace {

constexpr wchar_t kRegSubKey[]          = L"Software\\LiteClip";
constexpr wchar_t kRegValueName[]       = L"Language";
constexpr wchar_t kRegHotkeyValueName[] = L"Hotkey";

constexpr I18nStrings kStringsZhCN{
    L"zh-CN",
    L"Microsoft YaHei UI",
    L"剪切板",
    L"全部清除",
    L"暂无剪切板记录",
    L"固定",
    L"取消固定",
    L"粘贴为纯文本",
    L"删除",
    L"[图片预览不可用]",
    L"截图_",
    L"后台操作失败，未完成本次操作。",
    L"无法保存固定记录索引，未清空记录。",
    L"无法保存固定记录索引，状态未改变。",
    L"无法删除记录，已保留原数据。",
    L"固定记录索引无法完整读取。为避免覆盖或误删，已保留所有缓存文件。",
    L"快捷键冲突",
    L"快捷键 Win + V 已被系统剪切板占用，请选择：",
    L"使用 Alt + V",
    L"后续使用 Alt + V 呼出剪切板历史",
    L"（已占用）",
    L"使用 Win + V",
    L"关闭系统自带剪切板并继续使用 Win + V，需管理员权限",
    L"退出程序"
};

constexpr I18nStrings kStringsEnUS{
    L"en-US",
    L"Segoe UI",
    L"Clipboard",
    L"Clear all",
    L"No clipboard history",
    L"Pin",
    L"Unpin",
    L"Paste as plain text",
    L"Delete",
    L"[Image preview unavailable]",
    L"Screenshot_",
    L"Background operation failed. The action could not be completed.",
    L"Failed to save pinned index. Items were not cleared.",
    L"Failed to save pinned index. Pin state was not changed.",
    L"Failed to delete item. Original data was kept.",
    L"Failed to read pinned index completely. All cache files have been preserved to prevent data loss.",
    L"Hotkey Conflict",
    L"Win + V is occupied by system clipboard. Choose an option:",
    L"Use Alt + V",
    L"Use Alt + V to open clipboard history from now on",
    L"(Occupied)",
    L"Use Win + V",
    L"Disable system clipboard and keep using Win + V (admin required)",
    L"Exit"
};

std::atomic<AppLanguage> g_currentLanguage{ AppLanguage::ZhCN };

AppLanguage DetectSystemLanguage() noexcept {
    const LANGID langId = ::GetUserDefaultUILanguage();
    if (PRIMARYLANGID(langId) == LANG_CHINESE) {
        return AppLanguage::ZhCN;
    }
    return AppLanguage::EnUS;
}

const wchar_t* LanguageToTag(AppLanguage lang) noexcept {
    return (lang == AppLanguage::EnUS) ? L"en-US" : L"zh-CN";
}

bool TryParseLanguageTag(std::wstring_view raw, AppLanguage& outLang) noexcept {
    while (!raw.empty() && (raw.front() == L' ' || raw.front() == L'\t')) {
        raw.remove_prefix(1);
    }
    while (!raw.empty() && (raw.back() == L' ' || raw.back() == L'\t' || raw.back() == L'\0')) {
        raw.remove_suffix(1);
    }
    if (raw.size() < 2) {
        return false;
    }

    if (_wcsnicmp(raw.data(), L"zh", 2) == 0) {
        outLang = AppLanguage::ZhCN;
        return true;
    }
    if (_wcsnicmp(raw.data(), L"en", 2) == 0) {
        outLang = AppLanguage::EnUS;
        return true;
    }
    return false;
}

bool ReadLanguageFromRegistry(AppLanguage& outLang) noexcept {
    HKEY hKey = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRegSubKey, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    wchar_t buffer[32]{};
    DWORD dataType = 0;
    DWORD dataSize = sizeof(buffer) - sizeof(wchar_t);
    const LSTATUS status = ::RegQueryValueExW(
        hKey,
        kRegValueName,
        nullptr,
        &dataType,
        reinterpret_cast<LPBYTE>(buffer),
        &dataSize);
    ::RegCloseKey(hKey);

    if (status != ERROR_SUCCESS || (dataType != REG_SZ && dataType != REG_EXPAND_SZ)) {
        return false;
    }

    buffer[31] = L'\0';
    return TryParseLanguageTag(buffer, outLang);
}

void WriteLanguageToRegistry(AppLanguage lang) noexcept {
    HKEY hKey = nullptr;
    if (::RegCreateKeyExW(
            HKEY_CURRENT_USER,
            kRegSubKey,
            0,
            nullptr,
            REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE,
            nullptr,
            &hKey,
            nullptr) != ERROR_SUCCESS) {
        return;
    }

    const wchar_t* tag = LanguageToTag(lang);
    const DWORD byteCount = static_cast<DWORD>((std::wcslen(tag) + 1) * sizeof(wchar_t));
    ::RegSetValueExW(
        hKey,
        kRegValueName,
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(tag),
        byteCount);
    ::RegCloseKey(hKey);
}

} // namespace

namespace I18n {

void Initialize() noexcept {
    AppLanguage lang = AppLanguage::ZhCN;
    if (!ReadLanguageFromRegistry(lang)) {
        lang = DetectSystemLanguage();
        WriteLanguageToRegistry(lang);
    }
    g_currentLanguage.store(lang, std::memory_order_release);
}

AppLanguage GetLanguage() noexcept {
    return g_currentLanguage.load(std::memory_order_acquire);
}

const I18nStrings& Get() noexcept {
    return (GetLanguage() == AppLanguage::EnUS) ? kStringsEnUS : kStringsZhCN;
}

AppHotkey ReadHotkeyFromRegistry() noexcept {
    HKEY hKey = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRegSubKey, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) {
        return AppHotkey::WinV;
    }

    wchar_t buffer[32]{};
    DWORD dataType = 0;
    DWORD dataSize = sizeof(buffer) - sizeof(wchar_t);
    const LSTATUS status = ::RegQueryValueExW(
        hKey,
        kRegHotkeyValueName,
        nullptr,
        &dataType,
        reinterpret_cast<LPBYTE>(buffer),
        &dataSize);
    ::RegCloseKey(hKey);

    if (status != ERROR_SUCCESS || (dataType != REG_SZ && dataType != REG_EXPAND_SZ)) {
        return AppHotkey::WinV;
    }

    buffer[31] = L'\0';
    std::wstring_view raw(buffer);
    while (!raw.empty() && (raw.front() == L' ' || raw.front() == L'\t')) {
        raw.remove_prefix(1);
    }
    if (raw.size() >= 3 && _wcsnicmp(raw.data(), L"Alt", 3) == 0) {
        return AppHotkey::AltV;
    }
    return AppHotkey::WinV;
}

void WriteHotkeyToRegistry(AppHotkey hotkey) noexcept {
    HKEY hKey = nullptr;
    if (::RegCreateKeyExW(
            HKEY_CURRENT_USER,
            kRegSubKey,
            0,
            nullptr,
            REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE,
            nullptr,
            &hKey,
            nullptr) != ERROR_SUCCESS) {
        return;
    }

    const wchar_t* value = (hotkey == AppHotkey::AltV) ? L"Alt+V" : L"Win+V";
    const DWORD byteCount = static_cast<DWORD>((std::wcslen(value) + 1) * sizeof(wchar_t));
    ::RegSetValueExW(
        hKey,
        kRegHotkeyValueName,
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(value),
        byteCount);
    ::RegCloseKey(hKey);
}

} // namespace I18n
