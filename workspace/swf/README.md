# libswf - SWF 文件解析库

## 项目概述
libswf 是一个用于解析和操作 SWF (Shockwave Flash) 文件的 C++ 库。

## 功能特性

### 已完成功能 ✅

#### 1. 基础类型系统 (include/common/types.h)
- RGB/RGBA 颜色类型
- Rect 矩形区域 (TWIPS 坐标)
- Matrix 2D 变换矩阵
- ColorTransform 颜色变换
- Fixed8/Fixed16 定点数
- SWF 标签类型枚举

#### 2. BitStream 位流读取 (include/common/bitstream.h)
- 支持按位读取 (readUB/readSB)
- 字节对齐支持
- 小端字节序处理

#### 3. SWF 头部解析 (src/parser/swfheader.cpp)
- 支持 FWS (未压缩) 格式
- 支持 CWS (Zlib 压缩) 格式
- RECT 帧大小解析
- 版本、帧率、帧数读取

#### 4. Tag 数据结构 (include/parser/swftag.h)
- ShapeTag - 形状定义
- ImageTag - 图像数据 (JPEG/PNG)
- TextTag - 文本内容
- FontTag - 字体定义
- ActionTag - ActionScript 代码
- ABCFileTag - AS3 字节码

#### 5. SWF 解析器 (src/parser/swfparser.cpp)
- 文件加载和解析
- 基础 Tag 解析:
  - SetBackgroundColor (背景色)
  - DefineShape (形状定义)
  - DefineBits (图像数据)
  - DefineFont (字体)
  - DefineText (文本)
  - DoAction/DoABC (ActionScript)
  - ExportAssets (导出资源)
  - SymbolClass (符号类)
  - DefineSprite (MovieClip)
  - Button (按钮定义)
  - FrameLabel (帧标签)

#### 6. AVM2 虚拟机 (src/avm2/avm2.cpp) - 完整实现 ✅
- **完整的 VM 架构**
  - 操作数栈和局部寄存器
  - 作用域链管理
  - 调用栈支持
  
- **完整的 ABC 文件解析**
  - 常量池 (int/uint/double/string/namespace/multiname)
  - 方法定义和主体解析
  - 类和实例解析
  - 特性 (Traits) 解析
  - 脚本解析
  - 异常处理器解析
  
- **完整的字节码解释器** (所有主要操作码)
  - 栈操作 (push/pop/dup/swap)
  - 局部变量访问 (getlocal/setlocal)
  - 算术运算 (+, -, *, /, %)
  - 位运算 (&, |, ^, <<, >>, >>>)
  - 比较运算 (==, ===, <, <=, >, >=)
  - 类型转换 (convert_i, convert_d, convert_s, convert_b)
  - 条件跳转 (iftrue, iffalse, ifeq, ifne, iflt, etc.)
  - 循环控制 (jump, lookupswitch)
  - 对象/数组创建 (newobject, newarray)
  - 属性访问 (getproperty, setproperty, initproperty, deleteproperty)
  - 方法调用 (call, callmethod, callstatic, callproperty, callsuper)
  - 构造函数 (construct, constructsuper, constructprop)
  - 闭包支持 (newfunction)
  - 类操作 (newclass)
  
- **类继承系统**
  - 类定义和实例化
  - 继承链和方法分派
  - 原型链支持
  - 特性初始化 (slot/const/method/getter/setter)
  
- **异常处理**
  - try/catch/finally 机制
  - 异常处理器查找
  - 异常对象传播
  
- **垃圾回收**
  - 标记-清除算法
  - 根对象追踪
  - 自动内存管理
  
- **完整的原生函数库**
  - trace()
  - isNaN()/isFinite()
  - parseInt()/parseFloat()
  - Number 常量 (NaN, MAX_VALUE, MIN_VALUE, INFINITY)
  - escape()/unescape()
  - Array/Object 构造函数

#### 7. 渲染器框架 (src/renderer/renderer.cpp)
- Windows GDI+ 渲染支持
- Linux 跨平台位图渲染 (ImageBuffer)
- SoftwareRenderer - 纯软件渲染器
  - Shape 渲染 (直线、曲线)
  - 帧控制 (gotoFrame, advanceFrame)
  - PPM 图像导出
- DisplayObject 层次结构:
  - ShapeDisplayObject
  - BitmapDisplayObject
  - TextDisplayObject
  - MovieClipDisplayObject
- 动画控制 (play, stop, gotoFrame)
- 缩放模式 (SHOW_ALL, EXACT_FIT, NO_BORDER, NO_SCALE)
- ImageBuffer 功能:
  - 像素级绘图 (setPixel/getPixel)
  - 基本图形绘制 (drawLine, drawRect, drawCircle)
  - PPM 图像导出

#### 8. SWF 文件生成 (src/writer/swfwriter.cpp)
- SWFWriter - 程序化创建 SWF 文件
  - 设置帧率、帧数、背景色
  - 创建 Shape、Image、Text、Sprite
  - 导出符号 (exportSymbol)
  - 放置对象 (placeObject)
  - 保存为 FWS 格式
- ShapeBuilder - 常用形状工厂
  - createRect - 创建矩形
  - createLine - 创建直线
  - createTriangle - 创建三角形

#### 9. 示例程序
- swf_info - 显示 SWF 文件信息
- as3_test - ActionScript 3 测试
- test_simple - 单元测试
- image_buffer_test - 跨平台图像渲染测试
- software_renderer_test - 软件渲染器测试
- swf_create_test - SWF 文件生成测试

### 待完成功能 📝

#### 1. 解析器增强 ✅ 已完成
- [x] ZWS (LZMA) 压缩支持
- [x] 完整的 ShapeRecord 解析
- [x] DefineSprite (MovieClip) 解析
- [x] Button 定义解析
- [x] **音频播放** - Windows (WinMM) / Linux (ALSA) 跨平台支持
  - PCM/ADPCM 解码和播放
  - 音量控制、声像控制
  - 循环播放
  - 音频包络控制
- [x] Sound 标签解析（框架）
- [x] Video 标签解析 (DefineVideoStream, VideoFrame)

#### 2. AVM2 完善 ✅ 已完成
- [x] 完整的字节码解释器 (所有主要操作码)
- [x] 类继承和方法调用
- [x] 异常处理 (try/catch/finally)
- [x] 垃圾回收 (标记-清除)
- [x] 与渲染器的集成 (AVM2RendererBridge)

##### AVM2 性能优化 (2026-03-14)
- **操作数栈优化**: 使用固定大小数组+指针替代 `std::vector`，消除动态内存分配开销
- **参数传递优化**: 将 `O(n²)` 的 `vector::insert` 改为 `O(n)` 的 `resize+reverse index`，显著提升方法调用性能
- **跳转表预留**: 添加 `OpcodeHandler` 函数指针类型，为未来 JIT/AOT 编译预留扩展点

##### AVM2 进一步优化 (2026-03-14)
- **字符串驻留 (String Interning)**: 全局字符串池，相同字符串共享内存，属性比较从 `O(n)` 降为 `O(1)` 指针比较
- **属性内联缓存 (Inline Cache)**: 为重复属性访问添加单级缓存，缓存命中时直接返回，无需哈希查找
- **方法调用缓存**: 256 槽位的方法缓存表，缓存 `(对象, 方法名)` 到方法的映射
- **算术运算快速路径**: `add/subtract/multiply` 对 `int32` 和 `double` 类型有专门的快速分支，避免通用类型转换
- **分代垃圾回收**: Young/Old 分代收集，新对象在 Young 代快速回收，存活对象晋升到 Old 代

##### Graphics API 增强 ✅ 2026-03-14
- **填充样式**:
  - `beginFill(color, alpha)` - 纯色填充 ✅
  - `beginGradientFill(type, colors, alphas, ratios, matrix)` - 渐变填充 ✅
  - `beginBitmapFill(bitmapData, matrix, repeat, smooth)` - 位图填充 ✅
  - `endFill()` - 结束填充 ✅
  
- **绘制形状**:
  - `drawRect(x, y, width, height)` - 矩形 ✅
  - `drawCircle(x, y, radius)` - 圆形 ✅
  - `drawEllipse(x, y, width, height)` - 椭圆 ✅
  - `drawRoundRect(x, y, width, height, ellipseWidth, ellipseHeight)` - 圆角矩形 ✅
  - `drawTriangles(vertices, indices, uvtData)` - 三角形 ✅
  
- **路径绘制**:
  - `moveTo(x, y)` - 移动画笔 ✅
  - `lineTo(x, y)` - 画直线 ✅
  - `curveTo(controlX, controlY, anchorX, anchorY)` - 画二次贝塞尔曲线 ✅
  - `lineStyle(thickness, color, alpha)` - 设置线条样式 ✅
  - `clear()` - 清除绘图 ✅

##### Native 方法完善
- **String**: `fromCharCode`, `charAt`, `charCodeAt`, `indexOf`, `lastIndexOf`, `substr`, `substring`, `split`, `toLowerCase`, `toUpperCase` 等
- **Array**: `push`, `pop`, `shift`, `unshift`, `join`, `slice`, `splice`, `reverse`, `sort`, `concat`, `indexOf`, `forEach` 等

#### 3. 渲染器完善 ✅ 已完成
- [x] Linux Cairo 渲染实现 (PNG/PDF 导出)
- [x] 性能分析工具 (Profiler/MemoryTracker)
- [x] 形状填充样式 (渐变/位图)
- [x] 文本渲染
- [x] 混合模式 (NORMAL/MULTIPLY/SCREEN/ADD 等)
- [x] 滤镜效果 (Blur/Glow/DropShadow/ColorMatrix)

#### 4. 工具链
- [x] **资源提取工具** (swf_extract) - 从 SWF 中提取图片、声音、字体、形状
- [ ] SWF 编辑器
- [ ] SWF 优化器

## 构建说明

### 依赖
- CMake 3.16+
- C++20 编译器
- Zlib
- (Windows) GDI+
- (Linux 可选) Cairo (libcairo2-dev) - 用于高质量渲染
- (可选) LZMA (liblzma-dev) - 用于 ZWS 压缩支持
- (Linux 可选) ALSA (libasound2-dev) - 用于音频播放

### 构建步骤

#### 基础构建
```bash
mkdir build && cd build
cmake ..
make -j4
```

#### 启用性能分析
```bash
cmake .. -DENABLE_PROFILING=ON
```

#### Linux 启用 Cairo 渲染
```bash
# 安装依赖
sudo apt-get install libcairo2-dev liblzma-dev

# 构建
cmake ..
make -j4
```

### 运行测试
```bash
./bin/test_simple    # 单元测试
./bin/swf_info file.swf  # 查看 SWF 信息
```

### 资源提取工具
```bash
# 提取 SWF 中的所有资源
./bin/swf_extract input.swf ./output_dir

# 提取的资源包括:
# - output_dir/images/    - JPEG, PNG 图片
# - output_dir/sounds/    - 音频文件 (MP3, PCM, ADPCM 等)
# - output_dir/fonts/     - 字体信息
# - output_dir/shapes/    - 形状描述
# - output_dir/report.txt - 提取报告
```

## 使用示例

```cpp
#include "parser/swfparser.h"
#include <iostream>

int main() {
    libswf::SWFParser parser;
    if (parser.parseFile("test.swf")) {
        auto& doc = parser.getDocument();
        std::cout << "Version: " << (int)doc.header.version << std::endl;
        std::cout << "Frames: " << doc.header.frameCount << std::endl;
        std::cout << "Tags: " << doc.tags.size() << std::endl;
    }
    return 0;
}
```

## 项目结构
```
include/
  common/       # 基础类型和工具
    types.h
    bitstream.h
    profiler.h    # 性能分析工具
  parser/       # SWF 解析
    swfheader.h
    swfparser.h
    swftag.h
  avm2/         # ActionScript 虚拟机
    avm2.h
  renderer/     # 渲染引擎
    renderer.h
src/
  parser/       # 解析器实现
  avm2/         # AVM2 实现
  renderer/     # 渲染器实现
samples/        # 示例程序
tests/          # 测试代码
```

## 性能分析工具

libswf 内置了性能分析工具，需要在构建时启用：

```cpp
#include "common/profiler.h"

// 方法 1: 自动作用域计时
void parseFile() {
    PROFILE_FUNCTION();  // 自动测量整个函数
    
    // 或手动命名
    PROFILE_SCOPE("Parse SWF Header");
    
    // 你的代码...
}

// 方法 2: 精确计时
void heavyOperation() {
    ScopedTimer timer("Heavy Operation");
    
    // 你的代码...
    
    double elapsed = timer.elapsedMs();  // 获取耗时
}

// 方法 3: 全局性能报告
PerformanceCounter::instance().printReport();

// 方法 4: 内存追踪
MemoryTracker::instance().printReport();
```

### Cairo 渲染器使用 (Linux)

```cpp
#include "renderer/renderer.h"

// 使用 Cairo 渲染 SWF 并导出为 PNG
CairoRenderer renderer;
if (renderer.loadSWF("input.swf")) {
    renderer.render(800, 600);
    renderer.saveToPNG("output.png");
    renderer.saveToPDF("output.pdf", 800, 600);
}
```

## 许可证
MIT License
