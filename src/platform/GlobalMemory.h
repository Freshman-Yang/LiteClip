#pragma once

#include "platform/Win32Common.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class ScopedClipboard {
public:
    explicit ScopedClipboard(bool isOpen) noexcept : isOpen_(isOpen) {}
    ~ScopedClipboard() noexcept {
        if (isOpen_) {
            ::CloseClipboard();
        }
    }

    ScopedClipboard(const ScopedClipboard&) = delete;
    ScopedClipboard& operator=(const ScopedClipboard&) = delete;

    void CloseNow() noexcept {
        if (isOpen_) {
            ::CloseClipboard();
            isOpen_ = false;
        }
    }

private:
    bool isOpen_ = false;
};

class ScopedGlobalLock {
public:
    explicit ScopedGlobalLock(HGLOBAL hGlobal) noexcept
        : hGlobal_(hGlobal), ptr_(hGlobal ? ::GlobalLock(hGlobal) : nullptr) {}

    ~ScopedGlobalLock() noexcept {
        if (ptr_ && hGlobal_) {
            ::GlobalUnlock(hGlobal_);
        }
    }

    ScopedGlobalLock(const ScopedGlobalLock&) = delete;
    ScopedGlobalLock& operator=(const ScopedGlobalLock&) = delete;

    [[nodiscard]] void* Get() const noexcept { return ptr_; }
    [[nodiscard]] explicit operator bool() const noexcept { return ptr_ != nullptr; }

private:
    HGLOBAL hGlobal_ = nullptr;
    void* ptr_ = nullptr;
};

class ScopedGlobalMem {
public:
    explicit ScopedGlobalMem(HGLOBAL hMem = nullptr) noexcept : hMem_(hMem) {}
    ~ScopedGlobalMem() noexcept {
        Reset();
    }

    ScopedGlobalMem(const ScopedGlobalMem&) = delete;
    ScopedGlobalMem& operator=(const ScopedGlobalMem&) = delete;

    ScopedGlobalMem(ScopedGlobalMem&& other) noexcept : hMem_(other.Release()) {}

    ScopedGlobalMem& operator=(ScopedGlobalMem&& other) noexcept {
        if (this != &other) {
            Reset(other.Release());
        }
        return *this;
    }

    void Reset(HGLOBAL hMem = nullptr) noexcept {
        if (hMem_ && hMem_ != hMem) {
            ::GlobalFree(hMem_);
        }
        hMem_ = hMem;
    }

    [[nodiscard]] HGLOBAL Get() const noexcept { return hMem_; }
    [[nodiscard]] explicit operator bool() const noexcept { return hMem_ != nullptr; }

    HGLOBAL Release() noexcept {
        HGLOBAL tmp = hMem_;
        hMem_ = nullptr;
        return tmp;
    }

private:
    HGLOBAL hMem_ = nullptr;
};

class ScopedBoolReset {
public:
    explicit ScopedBoolReset(bool& flag) noexcept : flag_(flag) {}
    ~ScopedBoolReset() noexcept { flag_ = false; }

    ScopedBoolReset(const ScopedBoolReset&) = delete;
    ScopedBoolReset& operator=(const ScopedBoolReset&) = delete;

private:
    bool& flag_;
};

// 分配并拷贝通用 HGLOBAL 缓冲区，可选在末尾追加单字节 '\0'
HGLOBAL CreateGlobalBuffer(const void* data, size_t size, bool appendNullByte) noexcept;

// 深拷贝 HGLOBAL，确保目标程序多次调用 IDataObject::GetData 时互不干扰且安全释放
HGLOBAL DupGlobalMem(HGLOBAL hMem) noexcept;

// 构造 CF_HDROP 所需的 HGLOBAL（DROPFILES + 双 '\0' 结尾宽字符串）
HGLOBAL CreateHDropGlobal(const std::wstring& filePath) noexcept;

// 构造 4 字节 DWORD 的 HGLOBAL（用于 Preferred DropEffect）
HGLOBAL CreateDwordGlobal(DWORD value) noexcept;

// 构造 CF_UNICODETEXT 所需的 HGLOBAL（末尾确保 L'\0'）
HGLOBAL CreateUnicodeTextGlobal(const std::wstring& text) noexcept;

// 构造单字节流格式（HTML Format / Rich Text Format）所需的 HGLOBAL（末尾确保 '\0'）
HGLOBAL CreateNullTerminatedBytesGlobal(const std::vector<uint8_t>& bytes) noexcept;

// 将 ScopedGlobalMem 写入系统剪切板并在成功时移交句柄所有权
bool SetClipboardGlobalData(UINT format, ScopedGlobalMem& mem) noexcept;
