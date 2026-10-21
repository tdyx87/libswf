#!/usr/bin/env bash
# 构建 swfparser。
#   - conan 只负责 zlib / liblzma
#   - FFmpeg 用**预编译**开发包（不从源码构建；源码构建在本环境会失败）
# 前置：conan（pip install "conan>=2"）以及预编译 FFmpeg：
#   Debian/Ubuntu: sudo apt install libavcodec-dev libavformat-dev \
#       libswresample-dev libswscale-dev
#   macOS        : brew install ffmpeg
#   Windows      : 见 build.bat（缺省用 third_party 下的预编译包）
# 用法：./build.sh [conan-profile] [FFMPEG_ROOT]
set -euo pipefail
PROFILE="${1:-default}"
BUILD=build

# ---- 1) 定位预编译 FFmpeg 根目录 ----
FFMPEG_ROOT="${2:-}"
if [ -z "$FFMPEG_ROOT" ]; then
  for cand in "$PWD/third_party/ffmpeg-n8.1-latest-win64-gpl-shared-8.1" \
              /usr/local /usr; do
    if [ -f "$cand/include/libavcodec/avcodec.h" ]; then FFMPEG_ROOT="$cand"; break; fi
  done
fi
if [ -n "$FFMPEG_ROOT" ] && [ -f "$FFMPEG_ROOT/include/libavcodec/avcodec.h" ]; then
  echo "[build] 使用预编译 FFmpeg：$FFMPEG_ROOT"
else
  echo "[build] 警告：未找到预编译 FFmpeg，将以 SWF_WITH_FFMPEG=OFF 构建"
  FFMPEG_ROOT=""
fi

# 2) 解析依赖（仅 zlib / liblzma）
conan install . -pr="$PROFILE" -of="$BUILD" --build=missing

# 3) 定位 conan 生成的 toolchain（兼容不同 conan 版本的输出布局）
TOOLCHAIN="$(find "$BUILD" -name conan_toolchain.cmake | head -n1)"
[ -n "$TOOLCHAIN" ] || { echo "找不到 conan_toolchain.cmake"; exit 1; }

# 4) 配置 + 构建（找到 FFmpeg 后 CMake 自动定义 SWF_HAVE_FFMPEG）
CMAKE_FFMPEG=(-DSWF_WITH_FFMPEG=OFF)
[ -n "$FFMPEG_ROOT" ] && CMAKE_FFMPEG=(-DFFMPEG_ROOT="$FFMPEG_ROOT" -DSWF_WITH_FFMPEG=ON)
cmake -B "$BUILD" -S . -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DCMAKE_BUILD_TYPE=Release "${CMAKE_FFMPEG[@]}"
cmake --build "$BUILD" -j"$(nproc)"

# 5) 运行期：共享版 ffmpeg 需动态库；Linux 下提示库路径
if [ -n "$FFMPEG_ROOT" ] && [ -d "$FFMPEG_ROOT/lib" ]; then
  echo "[build] 运行前请设置：export LD_LIBRARY_PATH=$FFMPEG_ROOT/lib:\$LD_LIBRARY_PATH"
fi