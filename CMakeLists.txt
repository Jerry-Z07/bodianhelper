cmake_minimum_required(VERSION 3.25)

project(BodianSmtcBridge LANGUAGES CXX ASM_MASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(BODIAN_ORIGINAL_DIR "C:/Program Files (x86)/bodian" CACHE PATH "原始波点音乐安装目录")
set(BODIAN_ORIGINAL_MPV "${BODIAN_ORIGINAL_DIR}/libmpv-2.dll" CACHE FILEPATH "原始 libmpv-2.dll 路径")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
set(PROXY_DEF "${GENERATED_DIR}/libmpv_proxy.def")
set(PROXY_ASM "${GENERATED_DIR}/libmpv_forward_thunks.asm")
set(PROXY_TABLE "${GENERATED_DIR}/libmpv_forward_table.cpp")

file(MAKE_DIRECTORY "${GENERATED_DIR}")

add_custom_command(
    OUTPUT "${PROXY_DEF}" "${PROXY_ASM}" "${PROXY_TABLE}"
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tools/generate_proxy_def.py"
            "--input" "${BODIAN_ORIGINAL_MPV}"
            "--output" "${PROXY_DEF}"
            "--asm-output" "${PROXY_ASM}"
            "--table-output" "${PROXY_TABLE}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/generate_proxy_def.py"
    COMMENT "生成 libmpv-2.dll 代理导出表"
    VERBATIM
)

add_library(mpv_proxy SHARED
    src/proxy/mpv_proxy.cpp
    src/bridge/bridge_core.cpp
    "${PROXY_ASM}"
    "${PROXY_TABLE}"
    "${PROXY_DEF}"
)

set_target_properties(mpv_proxy PROPERTIES
    OUTPUT_NAME "libmpv-2"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

target_compile_definitions(mpv_proxy PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

target_compile_options(mpv_proxy PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/utf-8>")

target_include_directories(mpv_proxy PRIVATE src)

target_link_libraries(mpv_proxy PRIVATE
    advapi32
    runtimeobject
    urlmon
    windowsapp
)
