<p align="center">
  <img src="assets/app.ico" alt="LiteClip Logo" width="96" height="96" />
</p>

<h1 align="center">LiteClip</h1>

<p align="center">
  A lightweight native clipboard history manager for Windows 11.<br/>
  Built with Win32 and Direct2D for capturing, pinning, and dragging text, rich text, and images.
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Platform-Windows%2011-0078D6?style=flat-square&logo=windows11" alt="Platform" />
  <img src="https://img.shields.io/badge/Arch-ARM64%20%7C%20x64-success?style=flat-square" alt="Arch" />
  <img src="https://img.shields.io/badge/Language-C%2B%2B20-00599C?style=flat-square&logo=isocpp" alt="C++20" />
  <img src="https://img.shields.io/badge/Renderer-Direct2D-orange?style=flat-square" alt="Direct2D" />
</p>

<p align="center">
  <b>English</b> | <a href="README_zh-CN.md">简体中文</a>
</p>

---

## ✨ Features

- **Multi-Format Capture**: Captures and deduplicates plain text, rich text (HTML / RTF), and images (PNG / DIB), with support for pasting as plain text.
- **Native Drag & Drop**: Drag image cards as PNG files into File Explorer, chat apps, or browsers; drag text cards directly into editors or input fields.
- **History & Pinned Tabs**: Organize items across **History** and **Pinned** tabs. Pinned items are saved locally across restarts and can be reordered by dragging.
- **Low Background Footprint**: Built with native Win32 and Direct2D; automatically releases cached GPU bitmaps and input hooks when the window is hidden.

---

## ⌨️ Shortcuts & Interactions

| Action | Description |
| :--- | :--- |
| `Win + V` (or fallback `Alt + V`) | Show / Hide the clipboard panel |
| `↑` / `↓` | Select the previous / next card |
| `Enter` / **Left-click** | Paste the card into the target window |
| `Shift + Enter` | Paste the selected card as plain text |
| `Delete` | Delete the selected card |
| `Esc` / Click outside | Hide the clipboard panel |
| **Left-click & drag** a card | Drag out as a file/text, or reorder items in the **Pinned** tab |
| Card hover buttons | **Pin / Unpin**, **Paste as plain text**, or **Delete** |
| Click `×` | Hide the window to the background |
| `Shift + Left-click` or `Right-click` on `×` | Exit LiteClip completely and clear unpinned cache |

---

## ⚠️ Prerequisite: System `Win + V` Hotkey Conflict

LiteClip uses `Win + V` by default. If `Win + V` is occupied by Windows Clipboard History on launch, LiteClip pops up a conflict dialog where you can:
1. **Use `Alt + V`**: Switch to `Alt + V` without administrator privileges (saved to `HKCU\Software\LiteClip\Hotkey`).
2. **Use `Win + V`**: Automatically disable the built-in Windows Clipboard History and restart Windows Explorer (requires administrator privileges).

You can also free the system `Win + V` binding manually in PowerShell or CMD:

```powershell
# Disable built-in Windows Clipboard History and free the Win+V hotkey
reg add "HKCU\Software\Microsoft\Clipboard" /v EnableClipboardHistory /t REG_DWORD /d 0 /f
reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced" /v DisabledHotkeys /t REG_SZ /d "V" /f

# Restart Windows Explorer to apply the changes
taskkill /f /im explorer.exe && start explorer.exe
```

---

## 🌐 Language Configuration

LiteClip supports **English (`en-US`)** and **Simplified Chinese (`zh-CN`)**. It automatically detects the system display language on first launch. To switch the UI language manually, update the registry and restart LiteClip:

```powershell
# Switch UI to English
reg add "HKCU\Software\LiteClip" /v Language /t REG_SZ /d "en-US" /f

# Switch UI to Simplified Chinese
reg add "HKCU\Software\LiteClip" /v Language /t REG_SZ /d "zh-CN" /f

# Reset to system default language on next launch
reg delete "HKCU\Software\LiteClip" /v Language /f
```

---

## 🛠️ Build Guide

### Prerequisites

- **OS**: Windows 10 / 11
- **IDE / Toolchain**: Visual Studio 2022 / 2026 (with **Desktop development with C++**)
- **Windows SDK**: Windows 10 / 11 SDK
- **CMake**: `>= 3.21`
- **Supported Architectures**: `x64`, `ARM64`

### Build Steps

1. **Build with CMake Presets**:
   - **Visual Studio**: Open the project root folder and select `x64-release` / `x64-debug` / `arm64-release` / `arm64-debug` from the configuration dropdown.
   - **Command Line** (run inside **Visual Studio Developer PowerShell**):
     ```powershell
     # Windows x64
     cmake --preset x64-release
     cmake --build --preset x64-release

     # Windows ARM64
     cmake --preset arm64-release
     cmake --build --preset arm64-release
     ```

2. **Install standalone executable**:
   ```powershell
   cmake --install out/build/x64-release
   ```

The compiled executable is generated at `out/build/<preset>/LiteClip.exe` (or `out/install/<preset>/LiteClip.exe`).

---

## 📄 License

This project is licensed under the [MIT License](LICENSE).

