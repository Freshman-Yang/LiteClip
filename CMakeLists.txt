cmake_minimum_required(VERSION 3.21)

project(LiteClip
    VERSION 1.0.0
    DESCRIPTION "Lightweight Native Windows 11 Clipboard History Manager"
    LANGUAGES CXX RC
)

if(NOT WIN32)
    message(FATAL_ERROR "LiteClip only supports Windows platforms.")
endif()

# ==============================================================================
# 分层模块化源文件定义（L1 Core -> L2 Platform -> L3 Clipboard -> L4 Storage -> L5 Interaction -> L6 UI -> L7 App）
# ==============================================================================

set(LITECLIP_CORE_SOURCES
    src/core/Types.h
    src/core/Hash.h
    src/core/AsyncWorker.h
    src/core/AsyncWorker.cpp
)

set(LITECLIP_PLATFORM_SOURCES
    src/platform/Win32Common.h
    src/platform/I18n.h
    src/platform/I18n.cpp
    src/platform/GlobalMemory.h
    src/platform/GlobalMemory.cpp
    src/platform/FileUtils.h
    src/platform/FileUtils.cpp
)

set(LITECLIP_CLIPBOARD_SOURCES
    src/clipboard/HtmlFormatProcessor.h
    src/clipboard/HtmlFormatProcessor.cpp
    src/clipboard/OfficePolicy.h
    src/clipboard/OfficePolicy.cpp
    src/clipboard/ImageCodecWic.h
    src/clipboard/ImageCodecWic.cpp
    src/clipboard/ClipboardMonitor.h
    src/clipboard/ClipboardMonitor.cpp
)

set(LITECLIP_STORAGE_SOURCES
    src/storage/StorageManager.h
    src/storage/StorageManager.cpp
)

set(LITECLIP_INTERACTION_SOURCES
    src/interaction/CaretLocator.h
    src/interaction/CaretLocator.cpp
    src/interaction/InputInjector.h
    src/interaction/InputInjector.cpp
    src/interaction/OleDragDrop.h
    src/interaction/OleDragDrop.cpp
)

set(LITECLIP_UI_SOURCES
    src/ui/ThemeAndLayout.h
    src/ui/VirtualListLayout.h
    src/ui/VirtualListLayout.cpp
    src/ui/RendererD2D.h
    src/ui/RendererD2D.cpp
    src/ui/HotkeyConflictDialog.h
    src/ui/HotkeyConflictDialog.cpp
    src/ui/MainWindow.h
    src/ui/MainWindow.cpp
)

set(LITECLIP_APP_SOURCES
    src/app/main.cpp
)

set(LITECLIP_ALL_SRC_FILES
    ${LITECLIP_CORE_SOURCES}
    ${LITECLIP_PLATFORM_SOURCES}
    ${LITECLIP_CLIPBOARD_SOURCES}
    ${LITECLIP_STORAGE_SOURCES}
    ${LITECLIP_INTERACTION_SOURCES}
    ${LITECLIP_UI_SOURCES}
    ${LITECLIP_APP_SOURCES}
)

add_executable(LiteClip WIN32)

target_sources(LiteClip
    PRIVATE
        ${LITECLIP_ALL_SRC_FILES}
        assets/app.rc
)

# Visual Studio 解决方案资源管理器自动映射物理目录树
source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PREFIX "src"
    FILES ${LITECLIP_ALL_SRC_FILES}
)
source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}/assets"
    PREFIX "assets"
    FILES assets/app.rc
)

target_compile_features(LiteClip PRIVATE cxx_std_20)

set_target_properties(LiteClip PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    INTERPROCEDURAL_OPTIMIZATION_RELEASE ON
)

target_include_directories(LiteClip
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_definitions(LiteClip
    PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        WINAPI_FAMILY=WINAPI_FAMILY_DESKTOP_APP
)

if(MSVC)
    target_compile_options(LiteClip
        PRIVATE
            /W4
            /utf-8
            /permissive-
            $<$<CONFIG:Release>:/Oi /Gy>
    )

    target_link_options(LiteClip
        PRIVATE
            $<$<CONFIG:Release>:/OPT:REF /OPT:ICF /DEBUG:NONE>
    )
endif()

target_link_libraries(LiteClip
    PRIVATE
        advapi32
        user32
        gdi32
        shell32
        ole32
        oleaut32
        oleacc
        uuid
        dwmapi
        d2d1
        dwrite
        windowscodecs
        shcore
)

include(GNUInstallDirs)
install(TARGETS LiteClip
    RUNTIME DESTINATION .
)

