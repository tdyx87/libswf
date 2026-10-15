cmake_minimum_required(VERSION 3.15)
project(swfparser LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ZLIB REQUIRED)

# xz_utils 在不同 conan 版本下生成的包名可能是 liblzma 或 xz_utils，
# 这里都尝试一次，只要能拿到 liblzma::liblzma 目标就启用 ZWS 支持。
# conan 的 xz_utils 生成的 CMake 包名可能是 LibLZMA / liblzma / xz_utils，逐个尝试。
find_package(LibLZMA QUIET)
if(NOT TARGET LibLZMA::LibLZMA)
    find_package(liblzma QUIET)
endif()
if(NOT TARGET LibLZMA::LibLZMA AND NOT TARGET liblzma::liblzma)
    find_package(xz_utils QUIET)
endif()

set(SWF_LZMA_TARGET "")
if(TARGET LibLZMA::LibLZMA)
    set(SWF_LZMA_TARGET LibLZMA::LibLZMA)
elseif(TARGET liblzma::liblzma)
    set(SWF_LZMA_TARGET liblzma::liblzma)
endif()

add_library(swf_core
    src/Decompress.cpp
    src/Parser.cpp
)
target_include_directories(swf_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(swf_core PUBLIC ZLIB::ZLIB)

if(SWF_LZMA_TARGET)
    target_link_libraries(swf_core PUBLIC ${SWF_LZMA_TARGET})
    target_compile_definitions(swf_core PUBLIC SWF_HAVE_LZMA=1)
    message(STATUS "LZMA (ZWS) support: ON (${SWF_LZMA_TARGET})")
else()
    message(STATUS "LZMA (ZWS) support: OFF (xz_utils not resolved)")
endif()

if(MSVC)
    # /utf-8：源码含中文注释（代码页 936 下的 C4819 警告）
    target_compile_options(swf_core PRIVATE /W3 /permissive- /utf-8)
    target_compile_definitions(swf_core PUBLIC _CRT_SECURE_NO_WARNINGS)
endif()

# —— 渲染 / 字体 / 图像：vendored（nanovg + stb），移动端 GPU 友好，无 conan 依赖链 ——
# cairo/1.18.0 在 conan-center 无条件依赖 glib/pcre2/libffi/libgettext->libiconv/1.17，
# 而 libiconv/1.17 配方在 conan 2.x + 新 Python 下 build() 必崩，且 glib 需巨量源码构建，
# 故改用 vendored nanovg（OpenGL/OpenGL ES，移动端 GPU 加速，比 cairo 纯软件更适合移动端）。
# 字体/图像用 vendored stb_truetype / stb_image（头文件库，跨平台、零构建风险）。
set(THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
# 软件光栅器渲染后端（纯 CPU，零 GL 依赖；nanoVG 头文件保留作将来 GPU 后端，暂不编译 nanovg.c）
add_library(swf_render
    src/Renderer.cpp
    src/Decoder.cpp
    src/Render.cpp
    src/Abc.cpp
    src/Fvm.cpp
)
target_include_directories(swf_render PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${THIRD_PARTY}
    ${THIRD_PARTY}/nanovg
    ${THIRD_PARTY}/stb
)
# nanovg.c 暂未编译（软件光栅器不依赖；若启用 GPU 后端需同时提供正确的 fontstash.c/.h）
if(MSVC)
    target_compile_options(swf_render PRIVATE /W3 /utf-8)
    target_compile_definitions(swf_render PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(swfparser src/main.cpp)
target_link_libraries(swfparser PRIVATE swf_core swf_render)
if(MSVC)
    target_compile_options(swfparser PRIVATE /W3 /utf-8)
    target_compile_definitions(swfparser PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

enable_testing()
add_test(NAME swfparser_help COMMAND swfparser --help)
add_test(NAME swfparser_opcodes COMMAND swfparser --opcodes)
add_test(NAME swfparser_sample_fws COMMAND swfparser ${CMAKE_CURRENT_SOURCE_DIR}/testswf/sample_fws.swf)
add_test(NAME swfparser_sample_cws COMMAND swfparser ${CMAKE_CURRENT_SOURCE_DIR}/testswf/sample_cws.swf)
set_tests_properties(swfparser_help PROPERTIES PASS_REGULAR_EXPRESSION "usage")
set_tests_properties(swfparser_opcodes PROPERTIES PASS_REGULAR_EXPRESSION "AVM1 opcode table")
set_tests_properties(swfparser_sample_fws PROPERTIES PASS_REGULAR_EXPRESSION "frameSize")
set_tests_properties(swfparser_sample_cws PROPERTIES PASS_REGULAR_EXPRESSION "frameSize")
