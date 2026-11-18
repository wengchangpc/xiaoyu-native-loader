cmake_minimum_required(VERSION 3.16)
project(xiaoyu_native CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 游戏是 64 位进程，位数不对注入会失败甚至崩客户端
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "必须构建 x64：cmake -A x64")
endif()

# 静态 CRT：目标机不需要装 VC 运行库
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# hook.cpp 已从构建里摘掉。
#   原先摘掉的理由写的是"这个客户端是 OpenGL 渲染，没有 DXGI 交换链"，
#   那个判断是错的：目录里有 dxcompiler.dll / dxil.dll / nvngx_dlss.dll，
#   .exp 里有一整套 NVSDK_NGX_D3D12_* 导入（CreateFeature / EvaluateFeature /
#   GetParameters 共 24 个），渲染是 D3D12。
#   但摘掉的决定本身是对的 —— 覆盖窗口方案和渲染方式解耦，不需要挂 Present。
#   文件留着做参考，不参与编译。
add_library(xiaoyu_client SHARED
    src/bridge.cpp
    src/dllmain.cpp
    src/esp.cpp
    src/explorer.cpp
    src/input.cpp
    src/log.cpp
    src/memory.cpp
    src/probe.cpp
    src/render.cpp
    src/xyrpc.cpp
    src/self.cpp
    src/config.cpp
    src/gui.cpp
    src/modules.cpp
    src/overlay.cpp
)

target_include_directories(xiaoyu_client PRIVATE src)

target_compile_definitions(xiaoyu_client PRIVATE
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
)

# /utf-8：源码里的中文宽字符串必须按 UTF-8 解析，否则面板文字会是乱码
target_compile_options(xiaoyu_client PRIVATE /utf-8 /W3 /EHsc)

set_target_properties(xiaoyu_client PROPERTIES
    PREFIX ""
    OUTPUT_NAME "xiaoyu_client"
)

target_link_libraries(xiaoyu_client PRIVATE
    d2d1
    dwrite
    ole32          # CoInitializeEx
    kernel32
    user32
    gdi32
)
