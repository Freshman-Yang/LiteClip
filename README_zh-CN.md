<p align="center">
  <img src="assets/app.ico" alt="LiteClip Logo" width="96" height="96" />
</p>

<h1 align="center">LiteClip</h1>

<p align="center">
  Windows 11 轻量级原生剪切板历史管理工具。<br/>
  基于 Win32 与 Direct2D 构建，支持文本、富文本与图像的记录、固定与拖拽。
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Platform-Windows%2011-0078D6?style=flat-square&logo=windows11" alt="Platform" />
  <img src="https://img.shields.io/badge/Arch-ARM64%20%7C%20x64-success?style=flat-square" alt="Arch" />
  <img src="https://img.shields.io/badge/Language-C%2B%2B20-00599C?style=flat-square&logo=isocpp" alt="C++20" />
  <img src="https://img.shields.io/badge/Renderer-Direct2D-orange?style=flat-square" alt="Direct2D" />
</p>

<p align="center">
  <a href="README.md">English</a> | <b>简体中文</b>
</p>

---

## ✨ 特性

- **多格式记录**：支持纯文本、富文本（HTML / RTF）与图像（PNG / DIB）的捕获、去重及纯文本粘贴。
- **原生拖拽支持**：支持将图像卡片作为 PNG 文件直接拖入文件夹、聊天软件或浏览器；支持将文本直接拖入输入框或文档。
- **双页管理**：提供“历史”与“固定”双标签页，固定项保存在本地并在重启后恢复，固定页内支持拖拽调整顺序。
- **轻量常驻**：基于原生 Win32 与 Direct2D 构建，窗口隐藏时自动释放显存位图缓存与钩子资源。

---

## ⌨️ 交互说明

| 操作 | 说明 |
| :--- | :--- |
| `Win + V`（或备用 `Alt + V`） | 显示 / 隐藏剪切板面板 |
| `↑` / `↓` | 上下切换选中的卡片 |
| `Enter` / **左键单击**卡片 | 将内容写回剪切板并粘贴到目标窗口 |
| `Shift + Enter` | 纯文本粘贴当前选中的卡片 |
| `Delete` | 删除当前选中的卡片 |
| `Esc` / 点击窗口外部 | 隐藏剪切板面板 |
| **左键按住拖拽**卡片 | 将图片或文字拖出到外部程序（在“固定”页内可拖拽排序） |
| 卡片右侧按钮（悬停显示） | **固定 / 取消固定**、**粘贴为纯文本** 或 **删除** |
| 点击右上角 `×` | 隐藏窗口至后台 |
| `Shift + 左键` 或 `右键` 点击 `×` | 退出 LiteClip 进程并清理非固定缓存 |

---

## ⚠️ 前置说明：系统 `Win + V` 热键冲突处理

LiteClip 默认使用 `Win + V` 快捷键。若启动时检测到 `Win + V` 被 Windows 自带剪切板占用，将自动弹出冲突选择窗口：
1. **使用 `Alt + V`**：无需管理员权限，后续使用 `Alt + V` 呼出剪切板历史（配置保存在 `HKCU\Software\LiteClip\Hotkey`）。
2. **使用 `Win + V`**：请求管理员权限自动关闭系统自带剪切板并重启资源管理器，继续使用 `Win + V`。

也可在 PowerShell 或 CMD 中手动执行以下指令释放 `Win + V`：

```powershell
# 禁用系统剪切板历史并释放 Win+V 占用
reg add "HKCU\Software\Microsoft\Clipboard" /v EnableClipboardHistory /t REG_DWORD /d 0 /f
reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced" /v DisabledHotkeys /t REG_SZ /d "V" /f

# 重启资源管理器使配置生效
taskkill /f /im explorer.exe && start explorer.exe
```

---

## 🌐 语言切换

LiteClip 支持 **简体中文（`zh-CN`）** 与 **英文（`en-US`）** 界面，首次启动时会自动根据系统语言初始化。如需手动切换语言，可在 PowerShell 或 CMD 中修改注册表后重启程序：

```powershell
# 切换为英文界面
reg add "HKCU\Software\LiteClip" /v Language /t REG_SZ /d "en-US" /f

# 切换为简体中文界面
reg add "HKCU\Software\LiteClip" /v Language /t REG_SZ /d "zh-CN" /f

# 删除配置（下次启动时重新按系统默认语言设置）
reg delete "HKCU\Software\LiteClip" /v Language /f
```

---

## 🛠️ 编译构建

### 环境要求

- **操作系统**：Windows 10 / 11
- **开发工具**：Visual Studio 2022 / 2026（需安装 **使用 C++ 的桌面开发**）
- **SDK**：Windows 10 / 11 SDK
- **CMake**：`>= 3.21`
- **支持架构**：`x64`、`ARM64`

### 编译步骤

1. **使用 CMake Presets 构建**：
   - **Visual Studio**：通过“打开本地文件夹”打开项目根目录，在顶部配置下拉框选择 `x64-release` / `x64-debug` / `arm64-release` / `arm64-debug` 生成并构建。
   - **命令行**（在 **Visual Studio Developer PowerShell** 环境下执行）：
     ```powershell
     # Windows x64
     cmake --preset x64-release
     cmake --build --preset x64-release

     # Windows ARM64
     cmake --preset arm64-release
     cmake --build --preset arm64-release
     ```

2. **导出可执行文件**：
   ```powershell
   cmake --install out/build/x64-release
   ```

构建产物位于 `out/build/<preset>/LiteClip.exe`（或安装目录 `out/install/<preset>/LiteClip.exe`）。

---

## 📄 开源协议

本项目基于 [MIT License](LICENSE) 开源。

