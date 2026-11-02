cmake_minimum_required(VERSION 3.24)
project(fgb_psvr2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(fgb_dependency)

# SDK 先进来：Kit 在独立构建时才会自己拉 SDK。
fgb_fetch_from_lock(facegaze_sdk "${CMAKE_CURRENT_SOURCE_DIR}/sdk.lock.json")

# 本地开发可以用 -DFGB_KIT_DIR=<Kit 源码目录> 绕过归档下载；CI 走 kit.lock.json。
set(FGB_KIT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
if(FGB_KIT_DIR)
    add_subdirectory("${FGB_KIT_DIR}" "${CMAKE_BINARY_DIR}/fgb-kit")
else()
    fgb_fetch_from_lock(fgb_kit "${CMAKE_CURRENT_SOURCE_DIR}/kit.lock.json")
endif()

list(APPEND CMAKE_MODULE_PATH "${FGB_KIT_CMAKE_DIR}")
include(FgbModule)

# enable_testing 必须在顶层工程调用，ctest 才能在 build 目录里发现用例。
enable_testing()

fgb_add_module(
    NAME psvr2
    MAIN src/main.cpp
    SOURCES src/psvr2_protocol.cpp
    TEST_SOURCES tests/test_protocol.cpp
    SMOKE_SOURCE tests/abi_smoke.cpp
)

# 冒烟程序复用协议层（合成服务端与模块共享同一份布局代码），把 core 链进去。
if(TARGET psvr2-core)
    target_link_libraries(psvr2-abi-smoke PRIVATE psvr2-core)
endif()
