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
    src/Avm1.cpp
    src/Audio.cpp
    src/FFmpeg.cpp
    src/Player.cpp
)
target_include_directories(swf_render PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${THIRD_PARTY}
    ${THIRD_PARTY}/nanovg
    ${THIRD_PARTY}/stb
)
# swf_render 依赖 swf_core 的解压/解析工具（inflateZlib / decodeLzmaAlone 等），
# 让测试目标（仅链接 swf_render）也能正确解析 CWS/ZWS。
target_link_libraries(swf_render PUBLIC swf_core)
# nanovg.c 暂未编译（软件光栅器不依赖；若启用 GPU 后端需同时提供正确的 fontstash.c/.h）
if(MSVC)
    target_compile_options(swf_render PRIVATE /W3 /utf-8)
    target_compile_definitions(swf_render PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
# 音频实时播放（仅 Windows 用系统 waveOut）；跨平台导出走 WAV 文件。
# 注意：Media Foundation 已被 FFmpeg 取代，不再链接 mf* 系列库。
if(WIN32)
    target_link_libraries(swf_render PUBLIC winmm)
endif()

# ---- FFmpeg 跨平台音视频解码后端（替代 Windows-only 的 Media Foundation）----
# 覆盖全部 SWF 编码：MP3/Nellymoser/Speex/AAC 音频 + H.263/VP6/AVC 视频。
# ffmpeg 不通过 conan 源码编译（太重，本环境构建失败）；改为链接「预编译/系统」ffmpeg dev 库：
#   Linux  : apt install libavcodec-dev libavformat-dev libswresample-dev libswscale-dev
#   macOS  : brew install ffmpeg
#   Windows: 下载 ffmpeg.org 的 dev(shared) 包，或 vcpkg install ffmpeg，
#            并把安装根目录传给 cmake：-DFFMPEG_ROOT=<根目录>（含 include/ 与 lib/）。
option(SWF_WITH_FFMPEG "Use FFmpeg for cross-platform audio/video decoding" ON)
if(SWF_WITH_FFMPEG)
    # 1) 优先用 conan 生成的 CMake 目标（若将来用 conan 提供 ffmpeg）。
    #    conan 的 ffmpeg recipe 既可能暴露全局目标，也可能只暴露组件级目标
    #    （FFMPEG::avcodec 等），两种都识别。
    find_package(FFMPEG QUIET)
    find_package(ffmpeg QUIET)
    set(FFMPEG_TARGET "")
    if(TARGET FFMPEG::FFMPEG)
        set(FFMPEG_TARGET FFMPEG::FFMPEG)
    elseif(TARGET ffmpeg::ffmpeg)
        set(FFMPEG_TARGET ffmpeg::ffmpeg)
    elseif(TARGET FFmpeg::FFmpeg)
        set(FFMPEG_TARGET FFmpeg::FFmpeg)
    elseif(TARGET FFMPEG::avcodec AND TARGET FFMPEG::avutil AND
           TARGET FFMPEG::swresample AND TARGET FFMPEG::swscale)
        # 组件级目标：逐个链接（顺序不重要，均为 IMPORTED）
        set(FFMPEG_TARGET "FFMPEG::avcodec;FFMPEG::avutil;FFMPEG::avformat;FFMPEG::swresample;FFMPEG::swscale")
    endif()
    if(FFMPEG_TARGET)
        target_link_libraries(swf_render PUBLIC ${FFMPEG_TARGET})
        target_compile_definitions(swf_render PUBLIC SWF_HAVE_FFMPEG=1)
        message(STATUS "FFmpeg backend: ON (${FFMPEG_TARGET})")
    else()
        # 2) 回退：在系统/预编译路径里手动查找 ffmpeg 库与头文件。
        #    支持用 FFMPEG_ROOT / ffmpeg_ROOT 显式指定安装根目录（含 include/ 与 lib/）。
        set(_ff_hints "")
        if(FFMPEG_ROOT)
            list(APPEND _ff_hints "${FFMPEG_ROOT}")
        endif()
        if(ffmpeg_ROOT)
            list(APPEND _ff_hints "${ffmpeg_ROOT}")
        endif()
        if(DEFINED ENV{FFMPEG_ROOT})
            list(APPEND _ff_hints "$ENV{FFMPEG_ROOT}")
        endif()
        find_path(FFMPEG_INCLUDE_DIR NAMES libavcodec/avcodec.h
                  HINTS ${_ff_hints} PATH_SUFFIXES include)
        find_library(FFMPEG_AVCODEC    NAMES avcodec    HINTS ${_ff_hints} PATH_SUFFIXES lib bin)
        find_library(FFMPEG_AVUTIL     NAMES avutil     HINTS ${_ff_hints} PATH_SUFFIXES lib bin)
        find_library(FFMPEG_AVFORMAT   NAMES avformat   HINTS ${_ff_hints} PATH_SUFFIXES lib bin)
        find_library(FFMPEG_SWRESAMPLE NAMES swresample HINTS ${_ff_hints} PATH_SUFFIXES lib bin)
        find_library(FFMPEG_SWSCALE    NAMES swscale    HINTS ${_ff_hints} PATH_SUFFIXES lib bin)
        if(FFMPEG_INCLUDE_DIR AND FFMPEG_AVCODEC AND FFMPEG_AVUTIL AND FFMPEG_SWRESAMPLE AND FFMPEG_SWSCALE)
            target_include_directories(swf_render PUBLIC ${FFMPEG_INCLUDE_DIR})
            target_link_libraries(swf_render PUBLIC
                ${FFMPEG_AVCODEC} ${FFMPEG_AVUTIL} ${FFMPEG_AVFORMAT}
                ${FFMPEG_SWRESAMPLE} ${FFMPEG_SWSCALE})
            target_compile_definitions(swf_render PUBLIC SWF_HAVE_FFMPEG=1)
            message(STATUS "FFmpeg backend: ON (prebuilt/system at ${FFMPEG_INCLUDE_DIR})")
        else()
            message(STATUS "FFmpeg backend: OFF (ffmpeg not found; compressed A/V will report 'need FFmpeg')")
        endif()
    endif()
endif()

add_executable(swfparser src/main.cpp)
target_link_libraries(swfparser PRIVATE swf_core swf_render)
if(MSVC)
    target_compile_options(swfparser PRIVATE /W3 /utf-8)
    target_compile_definitions(swfparser PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ---- 窗口化播放器（仅 Windows）：双击/命令行打开 SWF 即播放 ----
# 用系统 Win32 + GDI 直接呈现软件光栅器的输出，不引入 SDL/GLFW 等新依赖。
if(WIN32)
    add_executable(swfplayer src/player_win.cpp)
    target_link_libraries(swfplayer PRIVATE swf_core swf_render user32 gdi32 winmm)
    # 仍是控制台子系统（保证 main()/错误输出可用），但程序启动后会立刻
    # FreeConsole() 关掉控制台窗口，只留播放窗口。比 /ENTRY:mainCRTStartup +
    # WIN32_EXECUTABLE 更稳妥（后者实测窗口创建不出来）。
    if(MSVC)
        target_compile_options(swfplayer PRIVATE /W3 /utf-8)
        target_compile_definitions(swfplayer PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
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

# AVM1 解释器单元测试（链接 swf_render 中的 Avm1）
add_executable(test_avm1 tests/test_avm1.cpp)
target_link_libraries(test_avm1 PRIVATE swf_render)
if(MSVC)
    target_compile_options(test_avm1 PRIVATE /W3 /utf-8)
    target_compile_definitions(test_avm1 PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME swfparser_avm1 COMMAND test_avm1)
set_tests_properties(swfparser_avm1 PROPERTIES PASS_REGULAR_EXPRESSION "AVM1 unit test: OK")

# DefineEditText(tag 37) 解析端到端测试
add_executable(test_edittext tests/test_edittext.cpp)
target_link_libraries(test_edittext PRIVATE swf_render)
if(MSVC)
    target_compile_options(test_edittext PRIVATE /W3 /utf-8)
    target_compile_definitions(test_edittext PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME swfparser_edittext COMMAND test_edittext)
set_tests_properties(swfparser_edittext PROPERTIES PASS_REGULAR_EXPRESSION "DefineEditText parse test: OK")

# PlaceObject3(tag 70) PlaceFlag2 位定义与字段顺序测试
add_executable(test_placeobject3 tests/test_placeobject3.cpp)
target_link_libraries(test_placeobject3 PRIVATE swf_render)
if(MSVC)
    target_compile_options(test_placeobject3 PRIVATE /W3 /utf-8)
    target_compile_definitions(test_placeobject3 PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME swfparser_placeobject3 COMMAND test_placeobject3)
set_tests_properties(swfparser_placeobject3 PROPERTIES PASS_REGULAR_EXPRESSION "PlaceObject3 parse test: OK")

# 精灵内 clipDepth 遮罩渲染测试（读回 PNG 像素验证裁剪）
add_executable(test_spriteclip tests/test_spriteclip.cpp)
target_link_libraries(test_spriteclip PRIVATE swf_render)
if(MSVC)
    target_compile_options(test_spriteclip PRIVATE /W3 /utf-8)
    target_compile_definitions(test_spriteclip PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME swfparser_spriteclip COMMAND test_spriteclip)
set_tests_properties(swfparser_spriteclip PROPERTIES PASS_REGULAR_EXPRESSION "Sprite clipDepth render test: OK")

# 位图滤镜（DropShadow/Blur/Glow）渲染测试
add_executable(test_filter tests/test_filter.cpp)
target_link_libraries(test_filter PRIVATE swf_render)
if(MSVC)
    target_compile_options(test_filter PRIVATE /W3 /utf-8)
    target_compile_definitions(test_filter PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME swfparser_filter COMMAND test_filter)
set_tests_properties(swfparser_filter PROPERTIES PASS_REGULAR_EXPRESSION "Filter render test: OK")

# 新增位图滤镜（Bevel/GradientGlow/GradientBevel/Convolution/ColorMatrix）渲染测试
add_executable(test_filter2 tests/test_filter2.cpp)
target_link_libraries(test_filter2 PRIVATE swf_render)
if(MSVC)
    target_compile_options(test_filter2 PRIVATE /W3 /utf-8)
    target_compile_definitions(test_filter2 PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME swfparser_filter2 COMMAND test_filter2)
set_tests_properties(swfparser_filter2 PROPERTIES PASS_REGULAR_EXPRESSION "New-filter render test: OK")

# ScreenVideo(codec 3) 解码测试
add_executable(test_screenvideo tests/test_screenvideo.cpp)
target_link_libraries(test_screenvideo PRIVATE swf_render)
if(MSVC)
    target_compile_options(test_screenvideo PRIVATE /W3 /utf-8)
    target_compile_definitions(test_screenvideo PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME swfparser_screenvideo COMMAND test_screenvideo)
set_tests_properties(swfparser_screenvideo PROPERTIES PASS_REGULAR_EXPRESSION "ScreenVideo decode test: OK")

# DefineFont2 代码表解析测试
add_executable(test_font tests/test_font.cpp)
target_link_libraries(test_font PRIVATE swf_render)
if(MSVC)
    target_compile_options(test_font PRIVATE /W3 /utf-8)
    target_compile_definitions(test_font PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME swfparser_font COMMAND test_font)
set_tests_properties(swfparser_font PROPERTIES PASS_REGULAR_EXPRESSION "DefineFont2 code-table test: OK")

# 旧字体（DefineFont(10) + DefineFontInfo(13/62)）解析测试
add_executable(test_font_old tests/test_font_old.cpp)
target_link_libraries(test_font_old PRIVATE swf_render)
if(MSVC)
    target_compile_options(test_font_old PRIVATE /W3 /utf-8)
    target_compile_definitions(test_font_old PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME swfparser_font_old COMMAND test_font_old)
set_tests_properties(swfparser_font_old PROPERTIES PASS_REGULAR_EXPRESSION "DefineFont\\(10\\)\\+DefineFontInfo test: OK")

# 文字渲染端到端像素验证（字形轮廓 -> 光栅化）
add_executable(test_render tests/test_render.cpp)
target_link_libraries(test_render PRIVATE swf_render)
if(MSVC)
    target_compile_options(test_render PRIVATE /W3 /utf-8)
    target_compile_definitions(test_render PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME swfparser_render COMMAND test_render)
set_tests_properties(swfparser_render PROPERTIES PASS_REGULAR_EXPRESSION "Text render test: OK")

# 音视频解码：PCM(0/3) / ADPCM(1) 解码为 PCM，MP3(2) 经 Media Foundation（仅在含真实样本时手动验证）
add_executable(test_audio tests/test_audio.cpp)
target_link_libraries(test_audio PRIVATE swf_render)
if(MSVC)
    target_compile_options(test_audio PRIVATE /W3 /utf-8)
    target_compile_definitions(test_audio PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME swfparser_audio COMMAND test_audio)
set_tests_properties(swfparser_audio PROPERTIES PASS_REGULAR_EXPRESSION "Audio decode test: OK")
