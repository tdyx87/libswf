@echo off
rem 构建 swfparser。
rem   - conan 只负责 zlib / liblzma
rem   - FFmpeg 用 third_party 下已下载好的**预编译shared 开发包**（不从源码构建）
rem 用法：build.bat [conan-profile] [FFMPEG_ROOT]
rem   FFMPEG_ROOT 缺省=third_party\ffmpeg-n8.1-latest-win64-gpl-shared-8.1
rem   Linux/macOS 请把 FFMPEG_ROOT 指向系统 ffmpeg 的前缀（含 include/ 与 lib/），
rem   或留空让CMake 自行 find。
setlocal
set PROFILE=%1
if "%PROFILE%"=="" set PROFILE=profiles/vs2017
set BUILD=build

rem ---- 1) 定位预编译FFmpeg 根目录 ----
set "FFMPEG_ROOT=%2"
if "%FFMPEG_ROOT%"=="" (
  set "FFMPEG_ROOT=%~dp0third_party\ffmpeg-n8.1-latest-win64-gpl-shared-8.1"
)
if exist "%FFMPEG_ROOT%\include\libavcodec\avcodec.h" (
  echo [build] 使用预编译 FFmpeg：%FFMPEG_ROOT%
) else (
  echo [build] 警告：在 "%FFMPEG_ROOT%" 未找到 FFmpeg 头文件，将以 SWF_WITH_FFMPEG=OFF 构建
  set "FFMPEG_ROOT="
)

rem ---- 2) 解析依赖（仅 zlib / liblzma）----
conan install . -pr=%PROFILE% -of=%BUILD% --build=missing
if errorlevel 1 exit /b 1

rem ---- 3) 定位 conan 生成的 toolchain（兼容不同 conan 版本的输出布局）----
set TOOLCHAIN=
for /r %BUILD% %%F in (conan_toolchain.cmake) do set TOOLCHAIN=%%F
if "%TOOLCHAIN%"=="" (echo 找不到 conan_toolchain.cmake & exit /b 1)

rem ---- 4) 配置 + 构建（找到 FFmpeg 后 CMake 自动定义 SWF_HAVE_FFMPEG）----
set CMAKE_FFMPEG=-DSWF_WITH_FFMPEG=OFF
if not "%FFMPEG_ROOT%"=="" set CMAKE_FFMPEG=-DFFMPEG_ROOT="%FFMPEG_ROOT%" -DSWF_WITH_FFMPEG=ON
cmake -B %BUILD% -S . -DCMAKE_TOOLCHAIN_FILE="%TOOLCHAIN%" -DCMAKE_BUILD_TYPE=Release %CMAKE_FFMPEG%
if errorlevel 1 exit /b 1
cmake --build %BUILD% --config Release
if errorlevel 1 exit /b 1

rem ---- 5) 运行期：共享版 FFmpeg 需 DLL，复制到构建输出目录 ----
if not "%FFMPEG_ROOT%"=="" if exist "%FFMPEG_ROOT%\bin" (
  copy /Y "%FFMPEG_ROOT%\bin\*.dll" "%BUILD%\Release\" >nul 2>&1
  echo [build] 已复制 FFmpeg 运行库 DLL 到 %BUILD%\Release
)
endlocal