cmake_minimum_required(VERSION 3.16)
project(libswf VERSION 1.0.0 LANGUAGES CXX)

# C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Find packages
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

# Find LZMA (optional)
find_package(LibLZMA QUIET)
if(LibLZMA_FOUND)
    message(STATUS "LZMA support: Enabled")
    add_definitions(-DHAS_LZMA=1)
else()
    message(STATUS "LZMA support: Disabled (liblzma-dev not found)")
endif()

# Platform-specific settings
if(WIN32)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
    add_definitions(-DWIN32_LEAN_AND_MEAN)
    add_definitions(-DNOMINMAX)
    
    # Renderer backend selection: Software (default) or GDI+
    option(USE_GDI_PLUS "Use GDI+ for hardware-accelerated rendering" OFF)
    if(USE_GDI_PLUS)
        add_definitions(-DGDIPVER=0x0110)
        add_definitions(-DRENDERER_GDIPLUS=1)
        message(STATUS "Windows renderer: GDI+")
    else()
        message(STATUS "Windows renderer: Software rendering (default)")
    endif()
else()
    # Linux/Unix: Find Cairo for rendering
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(CAIRO cairo QUIET)
        if(CAIRO_FOUND)
            message(STATUS "Cairo renderer: Enabled")
            add_definitions(-DRENDERER_CAIRO=1)
        else()
            message(STATUS "Cairo renderer: Disabled (install libcairo2-dev)")
        endif()
    endif()
endif()

# Source files (common is header-only)
set(COMMON_SOURCES
)

set(PARSER_SOURCES
    src/parser/swfheader.cpp
    src/parser/swfparser.cpp
)

set(RENDERER_SOURCES
    src/renderer/renderer.cpp
    src/renderer/unified_renderer.cpp
    src/renderer/software_backend.cpp
    src/renderer/cairo_backend.cpp
    src/renderer/gdiplus_backend.cpp
)

set(AVM2_SOURCES
    src/avm2/avm2.cpp
    src/avm2/avm2_renderer_bridge.cpp
)

set(WRITER_SOURCES
    src/writer/swfwriter.cpp
)

set(AUDIO_SOURCES
    src/audio/audio_player.cpp
)

# Headers
set(COMMON_HEADERS
    include/common/types.h
    include/common/bitstream.h
    include/common/profiler.h
)

set(PARSER_HEADERS
    include/parser/swfheader.h
    include/parser/swftag.h
    include/parser/swfparser.h
)

set(RENDERER_HEADERS
    include/renderer/renderer.h
)

set(AVM2_HEADERS
    include/avm2/avm2.h
)

set(WRITER_HEADERS
    include/writer/swfwriter.h
)

set(AUDIO_HEADERS
    include/audio/audio_player.h
)

# Create main library
add_library(swf STATIC
    ${COMMON_SOURCES}
    ${PARSER_SOURCES}
    ${RENDERER_SOURCES}
    ${AVM2_SOURCES}
    ${WRITER_SOURCES}
    ${AUDIO_SOURCES}
    ${COMMON_HEADERS}
    ${PARSER_HEADERS}
    ${RENDERER_HEADERS}
    ${AVM2_HEADERS}
    ${WRITER_HEADERS}
    ${AUDIO_HEADERS}
)

target_include_directories(swf PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(swf PUBLIC
    ZLIB::ZLIB
    Threads::Threads
)

if(LibLZMA_FOUND)
    target_link_libraries(swf PUBLIC LibLZMA::LibLZMA)
endif()

# Platform-specific libraries for renderer and audio
if(WIN32)
    target_link_libraries(swf PUBLIC
        gdiplus
        msimg32
        winmm  # Windows multimedia for audio
    )
else()
    if(CAIRO_FOUND)
        target_include_directories(swf PUBLIC ${CAIRO_INCLUDE_DIRS})
        target_link_libraries(swf PUBLIC ${CAIRO_LIBRARIES})
    endif()
    
    # Find ALSA for Linux audio
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(ALSA alsa QUIET)
        if(ALSA_FOUND)
            message(STATUS "ALSA audio: Enabled")
            target_include_directories(swf PUBLIC ${ALSA_INCLUDE_DIRS})
            target_link_libraries(swf PUBLIC ${ALSA_LIBRARIES})
            add_definitions(-DHAS_ALSA=1)
        else()
            message(STATUS "ALSA audio: Disabled (install libasound2-dev)")
        endif()
    endif()
endif()

# Compiler warnings
if(MSVC)
    target_compile_options(swf PRIVATE /W4 /WX-)
else()
    target_compile_options(swf PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Generate compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Installation
install(TARGETS swf
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/ DESTINATION include)

# Build options
option(BUILD_TESTS "Build test programs" ON)
option(BUILD_SAMPLES "Build sample programs" ON)
option(ENABLE_PROFILING "Enable performance profiling" OFF)

# Profiling support
if(ENABLE_PROFILING)
    add_definitions(-DENABLE_PROFILING=1)
    message(STATUS "Profiling: Enabled")
else()
    message(STATUS "Profiling: Disabled")
endif()

# Tests
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Samples
if(BUILD_SAMPLES)
    add_subdirectory(samples)
endif()

message(STATUS "libswf configuration:")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build tests: ${BUILD_TESTS}")
message(STATUS "  Build samples: ${BUILD_SAMPLES}")
