cmake_minimum_required(VERSION 3.16)
project(xiaoyu_loader CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 游戏是 64 位进程，DLL 位数不对会注入失败（甚至崩游戏）
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "必须构建 x64：-A x64")
endif()

# 静态 CRT：不依赖目标机装的 VC 运行库，注入更稳
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/embedded_bootstrap.h")
    message(FATAL_ERROR "缺少 src/embedded_bootstrap.h —— 先跑 python tools/embed.py")
endif()

add_library(xiaoyu_loader SHARED
    src/dllmain.cpp
    src/log.cpp
    src/python_host.cpp
)

target_compile_definitions(xiaoyu_loader PRIVATE
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

set_target_properties(xiaoyu_loader PROPERTIES
    PREFIX ""
    OUTPUT_NAME "xiaoyu_loader"
)

target_link_libraries(xiaoyu_loader PRIVATE kernel32)
