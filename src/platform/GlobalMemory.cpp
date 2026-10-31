#include "platform/GlobalMemory.h"

#include <shlobj.h>
#include <cstring>

HGLOBAL CreateGlobalBuffer(const void* data, size_t size, bool appendNullByte) noexcept {
    if (!data && size > 0) {
        return nullptr;
    }

    const size_t allocSize = size + (appendNullByte ? 1ULL : 0ULL);
    HGLOBAL hMem = ::GlobalAlloc(GMEM_MOVEABLE, allocSize);
    if (!hMem) {
        return nullptr;
    }

    ScopedGlobalLock lock(hMem);
    if (!lock) {
        ::GlobalFree(hMem);
        return nullptr;
    }

    auto* dst = static_cast<uint8_t*>(lock.Get());
    if (size > 0) {
        std::memcpy(dst, data, size);
    }
    if (appendNullByte) {
        dst[size] = 0;
    }

    return hMem;
}

HGLOBAL DupGlobalMem(HGLOBAL hMem) noexcept {
    if (!hMem) {
        return nullptr;
    }

    const SIZE_T size = ::GlobalSize(hMem);
    if (size == 0) {
        return nullptr;
    }

    const void* src = ::GlobalLock(hMem);
    if (!src) {
        return nullptr;
    }

    HGLOBAL hDest = ::GlobalAlloc(GMEM_MOVEABLE, size);
    if (hDest) {
        void* dst = ::GlobalLock(hDest);
        if (dst) {
            std::memcpy(dst, src, size);
            ::GlobalUnlock(hDest);
        } else {
            ::GlobalFree(hDest);
            hDest = nullptr;
        }
    }

    ::GlobalUnlock(hMem);
    return hDest;
}

HGLOBAL CreateHDropGlobal(const std::wstring& filePath) noexcept {
    if (filePath.empty()) {
        return nullptr;
    }

    const SIZE_T pathCharsWithDoubleNull = filePath.size() + 2;
    const SIZE_T totalBytes = sizeof(DROPFILES) + pathCharsWithDoubleNull * sizeof(wchar_t);

    HGLOBAL hGlobal = ::GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, totalBytes);
    if (!hGlobal) {
        return nullptr;
    }

    void* rawPtr = ::GlobalLock(hGlobal);
    if (!rawPtr) {
        ::GlobalFree(hGlobal);
        return nullptr;
    }

    auto* dropFiles = static_cast<DROPFILES*>(rawPtr);
    dropFiles->pFiles = sizeof(DROPFILES);
    dropFiles->pt.x = 0;
    dropFiles->pt.y = 0;
    dropFiles->fNC = FALSE;
    dropFiles->fWide = TRUE;

    auto* dstPath = reinterpret_cast<wchar_t*>(static_cast<uint8_t*>(rawPtr) + sizeof(DROPFILES));
    std::memcpy(dstPath, filePath.c_str(), (filePath.size() + 1) * sizeof(wchar_t));
    dstPath[filePath.size() + 1] = L'\0'; // 双 '\0' 结尾

    ::GlobalUnlock(hGlobal);
    return hGlobal;
}

HGLOBAL CreateDwordGlobal(DWORD value) noexcept {
    HGLOBAL hGlobal = ::GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD));
    if (!hGlobal) {
        return nullptr;
    }

    auto* ptr = static_cast<DWORD*>(::GlobalLock(hGlobal));
    if (!ptr) {
        ::GlobalFree(hGlobal);
        return nullptr;
    }

    *ptr = value;
    ::GlobalUnlock(hGlobal);
    return hGlobal;
}

HGLOBAL CreateUnicodeTextGlobal(const std::wstring& text) noexcept {
    const SIZE_T totalBytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL hGlobal = ::GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, totalBytes);
    if (!hGlobal) {
        return nullptr;
    }

    auto* dst = static_cast<wchar_t*>(::GlobalLock(hGlobal));
    if (!dst) {
        ::GlobalFree(hGlobal);
        return nullptr;
    }

    if (!text.empty()) {
        std::memcpy(dst, text.c_str(), text.size() * sizeof(wchar_t));
    }
    dst[text.size()] = L'\0';

    ::GlobalUnlock(hGlobal);
    return hGlobal;
}

HGLOBAL CreateNullTerminatedBytesGlobal(const std::vector<uint8_t>& bytes) noexcept {
    if (bytes.empty()) {
        return nullptr;
    }

    const bool hasNullTerminator = (bytes.back() == 0);
    const SIZE_T totalBytes = hasNullTerminator ? bytes.size() : (bytes.size() + 1);

    HGLOBAL hGlobal = ::GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, totalBytes);
    if (!hGlobal) {
        return nullptr;
    }

    auto* dst = static_cast<uint8_t*>(::GlobalLock(hGlobal));
    if (!dst) {
        ::GlobalFree(hGlobal);
        return nullptr;
    }

    std::memcpy(dst, bytes.data(), bytes.size());
    dst[totalBytes - 1] = 0;

    ::GlobalUnlock(hGlobal);
    return hGlobal;
}

bool SetClipboardGlobalData(UINT format, ScopedGlobalMem& mem) noexcept {
    if (format == 0 || !mem) {
        return false;
    }
    if (::SetClipboardData(format, mem.Get()) != nullptr) {
        mem.Release(); // 句柄所有权已移交系统剪切板
        return true;
    }
    return false;
}
