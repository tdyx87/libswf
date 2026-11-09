# 本项目（swfparser）结构分析文档

> 目的：把 `swfparser` 自身的架构、模块职责、设计亮点与已知短板完整梳理出来，
> 作为与采集到的开源项目文档（`ARCHITECTURE_AND_DESIGN.md`）做对比的“本端事实基础”。
>
> 结论先行：本项目**不是**一个“只会解析 SWF”的雏形，而是一个**针对真实 AS3 游戏的、
> 由逆向还原驱动的、双显示模型并存**的运行时节播放器；其 AVM2 虚拟机与配套原生类骨架
> 比“最小实现”要完整得多。真正的短板集中在**通用性、帧/启动时序、渲染保真与事件模型完整性**上。

---

## 0. 项目定位与来源

- **定位**：依据逆向还原的 Flash Player SWF 解析链重建的工具/播放器（`main.cpp:1-3` 注释明确写了
  `sub_490330` / `sub_4797C0` / `sub_48BA50` 等 IDA 函数偏移，即直接对标某个真实 Player 二进制
  的解析/解码/标签分派路径）。
- **目标样本**：围绕具体 AS3 游戏打磨（如 `swf2.swf`、`service_error_mc`、`bonus`、`/:busy`、`/:stick`
  等命名在代码注释与逻辑中反复出现），属于**针对性重实现**，而非“通吃所有 SWF”的通用播放器。
- **语言/构建**：纯 C++；`conan` 管理依赖（FFmpeg/nanovg/stb），Windows 用 `Visual Studio 15 2017`、
  Linux 取平台默认生成器，跨平台且自包含。

---

## 1. 目录与模块结构

### 1.1 头文件（`include/swf/`，数据模型与接口）
| 头文件 | 职责 |
|---|---|
| `Decoder.hpp` | **核心数据模型**：`Movie`/`Character`/`DisplayItem`/`Shape`/`Bitmap`/`Font`/`Text`/`EditText`/`Button`/`MorphShape`/`VideoStream`/`Sound`；TWIPS 坐标系、MATRIX/CXFORM、填充/滤镜、混合模式 |
| `Fvm.hpp` | **AVM2 虚拟机 + 运行时显示对象模型**：`Vm` 类、`Value`/`Object`/`ClassDef`/`Member`/`DisplayObject`、`registerFlashApi`/`registerNativeClasses` |
| `Abc.hpp` | ABC 字节码数据结构（method/body/instance/class/script/trait/multiname/constant pool） |
| `Avm1.hpp` / `Avm1Opcodes.hpp` | AVM1（AS1/2）解释器、85 条 opcode 表 |
| `Avm2Opcodes.hpp` | AVM2 opcode 元数据（编号/操作数/栈效应/标志）+ 标准 opcode 集合 |
| `Parser.hpp` / `TagReader.hpp` / `TagCode.hpp` | 标签解析、位流读取、tag 名称表 |
| `Render.hpp` | 软件光栅器接口（`renderMovieToPNG`/`renderMovieToBuffer`/`hitTest*`，含 AVM2 运行时树命中测试） |
| `Player.hpp` / `Audio.hpp` / `FFmpeg.hpp` / `Decompress.hpp` | 播放主循环、音频解码/播放、FFmpeg 封装、解压（zlib/lzma） |

### 1.2 源文件（`src/`）
| 文件 | 职责 |
|---|---|
| `main.cpp` | **CLI 与播放驱动器**：dump/渲染/`--avm2-opcodes` 覆盖率自检/音视频导出/无头播放（`--play-game`）与**脚本化输入模拟**（`--click`/`--hover`/`--drag`/`--key`） |
| `Parser.cpp` / `TagReader.cpp` / `Decoder.cpp` | SWF 解析与解码为 `Movie` 数据模型 |
| `Decompress.cpp` | zlib/lzma 解压 |
| `Abc.cpp` | ABC 解析 |
| `Avm1.cpp` / `MovieClipCommands.cpp` | AVM1 解释器与 MovieClip 时间轴命令 |
| `Avm2Opcodes.cpp` | AVM2 opcode 实现登记 |
| `Fvm.cpp` | **AVM2 解释器 + 显示对象模型 + 事件/时间轴/Tween/定时器/鼠标** 的完整实现（最大文件） |
| `Player.cpp` | **播放主循环**：以 `avm1::IClip`/`avm1::ITimeline` 为中心驱动 `DisplayItem` 树，含启动时序模拟开关 |
| `Render.cpp` | nanovg 软件光栅器（形状/文本/位图/按钮/滤镜/混合） |
| `Audio.cpp` / `FFmpeg.cpp` | 音频解码与实时播放 |
| `player_win.cpp` | Windows 平台播放前端 |

---

## 2. 各子系统剖析

### 2.1 解析 / 解码层（`Decoder`）
- 把 SWF 解码为 **“字典(角色) + 时间轴帧(显示列表)”**：`characters`（characterId→角色）、
  `frames`（每帧 `DisplayItem` 列表 + `DoAction` + 声音事件）。
- 几何解码**相当精细**：`Shape` 用**有向边 `FillEdge`（非零环绕规则）**而非简单轮廓，
  天然正确处理“并集（同向重叠轮廓）”与“洞（嵌套反向轮廓）”，无需启发式（`Decoder.hpp:101-110` 注释）。
- 角色类型齐全：Shape/Bitmap/Sprite/Font/Text/Morph/EditText/Video/Button。
- 滤镜数据模型完整：`FilterKind` 覆盖 DropShadow/Blur/Glow/Bevel/GradientBevel/Convolution/ColorMatrix。
- 混合模式：`BlendMode` 枚举覆盖 Normal/Layer/Multiply/Screen/…/HardLight 共 14 种。

### 2.2 双显示模型（关键架构特征）
项目**并存两套显示列表**，分别服务“时间轴”与“AVM2 运行时”：
1. **静态时间轴模型** `DisplayItem`（`Decoder.hpp:170`）：由 `PlaceObject` 构建，含矩阵/CXFORM/
   裁剪/遮罩/滤镜/实例名/ClipActions/运行时覆盖（`rtX/rtScaleX/rtAlpha/rtFrame…`）。注释明确
   “每帧重建，有效期仅限本帧”，适合**时间轴保真**（PlaceObject 语义）。
2. **运行时树模型** `fvm::DisplayObject`（`Fvm.hpp:123`）：AVM2 构建的**真树**，有 `parent`/
   `children`、属性（`x/y/scaleX/scaleY/rotation/alpha`）、`frameScripts` 映射、事件监听、
   混合/滤镜。挂在 `Movie::runtimeStage`（`Decoder.hpp:401`）。
- 二者通过 `SymbolClass`（AS3 类名→角色 id）与 `applyTimelineFrame` 协调：AVM2 `new` 出类时
  取对应角色绘制；时间轴 MovieClip 的内部帧由 `advanceTimeline` 推进。

### 2.3 AVM1（AS1/2）解释器（`Avm1`）
- 85 条 opcode 表（`main.cpp:68`），`Machine` 含 `timeline`（AVM1 的 `gotoAndPlay` 等映射到逐帧渲染）。
- 处理 `DoAction`/`DoInitAction`、ClipActions（load/enterFrame/mouse*/keyPress）、剪辑作用域变量。

### 2.4 AVM2 虚拟机（`Fvm`）—— 比“最小实现”完整得多
`Fvm.hpp` 的 `Vm` 类已具备：
- **值模型** `Value`（`VKind`：Undefined/Null/Bool/Int/UInt/Number/String/Object）+ 类型转换。
- **类系统** `ClassDef`/`Member`：trait 编译为 Slot/Const/Method/Getter/Setter/ClassMember，
  含父类链、`native` 方法优先调用（`Fvm.hpp:97-120`）——这是 Ruffle/Lightspark 同款的
  “AS3 类 + native 方法绑定”模式的 C++ 实现。
- **执行引擎**：`exec`/`invokeValue`/`constructObject`/`buildClass`/`ensureClassInit`，
  支持作用域链、闭包、getter/setter、属性访问链。
- **事件系统**：`Object::listeners`（事件类型→监听器），`dispatchToAll`，鼠标事件
  `dispatchMouseEvent(…, bubbles)`（含**冒泡**）。
- **时间轴推进**：`advanceTimeline`/`applyTimelineFrame`/`runFrameScripts`/`resolveFrameIndex`
  （帧号↔帧标签），`Vm::tick()` 推进所有 MovieClip 时间轴并派发 `enterFrame`。
- **Tween 补间引擎**：`TweenTask`/`advanceTweens`（注释写明 TweenLite/TweenMax）——直接仿真
  第三方补间库的插值行为（`Fvm.hpp:258-268`）。
- **虚拟定时器**：`setTimeout`/`setInterval`（按 `frameRate` 虚拟时钟，`TimerTask`）+ 
  `flash.utils.Timer` 对象（`TimerObjectTask`）。
- **鼠标输入**：`setMousePosition`/`mouseDown`/`mouseUp`/`mouseClick`、`hitChain`（自根到命中最深对象的命中链）、
  `startDrag`/`stopDrag`。
- **原生类注册**：`registerNativeClasses`/`registerFlashApi`；`ObjKind` 已声明
  DisplayObject/Container/InteractiveObject/Sprite/MovieClip/Stage/TextField/Graphics/Bitmap/
  Event/EventDispatcher/Timer/Loader/ByteArray/XML 等大量类型。

> 说明：这推翻了“原生类基本是 no-op”的早期判断。本项目**已搭好原生类的类型与骨架**，并实现了
> 一批方法；差距在于**行为深度与覆盖率**（部分方法可能未完全对齐真实 Flash 语义），而非“没有”。

### 2.5 渲染（`Render` + nanovg）
- 软件光栅（无 GPU），跨平台可直接跑（含 CI/移动端）。
- 接口支持**矢量重光栅**：`renderMovieToBuffer(…, targetW, targetH)` 按设备尺寸重新光栅化，
  放大时矢量/字形保持清晰（`Render.hpp:13-18`）——这是相对“固定分辨率光栅”的优势。
- 命中测试分两层：`hitTestItemsDeep`（深入 Sprite 内部命中嵌套 Button）、
  `hitTestRuntimeStage`/`hitTestRuntimeObject`（AVM2 运行时树，对应 `hitTestPoint`）。
- 数据上已承载滤镜与混合模式，但**像素级合成精度**（blur/glow 算法、blend 模式、gradient spread）
  依赖 nanovg，保真度是已知短板。

### 2.6 音频 / 视频（`Audio` + `FFmpeg`）
- 解码 DefineSound（MP3/ADPCM/Speex/Nellymoser/未压缩）、流式背景音（`streamAudio`）、
  DefineVideoStream（ScreenVideo/VP6/AVC）并导出帧。
- 实时播放（`playPcm`）+ WAV 导出。音视频全链路在同类轻量重实现中算完备。

### 2.7 CLI / 播放驱动器（`main.cpp`）
功能矩阵（远超“解析器”）：
- `dump` / `--render` / `--render-all` / `--inspect`（深度结构自检）。
- `--avm2-opcodes`：**打印 AVM2 标准 opcode 覆盖率（已实现/缺失清单）**——罕见的自诊断能力。
- `--list-avm1` 反汇编、`--opcodes` AVM1 表。
- 音视频导出：`--extract-audio`/`--wav`/`--play-sound`/`--wav-stream`/`--render-video`。
- **无头全流程播放** `--play-game` + **脚本化输入模拟**：`--click/--hover/--drag/--key`
  以“第 N 帧触发”方式复现交互，逐帧导出 PNG——极适合**自动化回归 / 与真实 Player 输出做像素级对比**。
- **吞吐优化**：多缓冲槽（`SWF_PNG_SLOTS`）+ 异步 PNG 编码（`writePngAsync`），把编码耗时与渲染重叠；
  `SWF_PNG_LEVEL`/`SWF_PNG_FILTER`/`SWF_PERF` 调参与性能统计。

---

## 3. 关键设计亮点（即“优秀之处”的硬件基础）
1. **逆向还原驱动的解码链**：直接对标真实 Player 二进制函数，针对特定内容的解码保真度高。
2. **有向边非零环绕填充**：用 `FillEdge` 的 `sign` 直接表达 SWF 的“边两侧填充”语义，正确覆盖
   并集与洞，无需启发式。
3. **双显示模型并存**：静态时间轴（`DisplayItem`，保真 PlaceObject）+ 运行时树
   （`fvm::DisplayObject`，支撑 AVM2 动态搭建），各取所长。
4. **内置 Tween + 虚拟定时器 + flash.utils.Timer**：把游戏依赖的补间/计时直接仿真，
   降低对第三方 AS3 库完整执行的依赖。
5. **AVM2 opcode 覆盖率自检**：CLI 直接报告“哪些标准 opcode 未实现”，是精准补 VM 的依据。
6. **可脚本化无头播放 + 帧序列导出**：把“播放结果”变成可 diff 的 PNG 序列，工程化对比能力强。
7. **矢量重光栅**：任意分辨率清晰，适合高 DPI/缩放场景。
8. **异步多缓冲 PNG**：量产级帧导出吞吐优化。
9. **音视频全链路 + FFmpeg**：解码/播放/导出一体。
10. **小、可读、单语言**：~15 个源文件、纯 C++，相对 Ruffle 的巨型 Rust 或 Lightspark 的 C++ 庞杂，
    更易审计与针对特定游戏精修。

---

## 4. 当前已知短板（经整体架构实证修正）

> 修正说明：本节初版有多处**未读通整体架构、拿开源对照表外推出的“伪短板”**，现已逐条以代码实证
> 修正：① 启动/帧时序与播放循环并非缺口（见本节末“已证实的强项”）；② 事件模型已完整实现三段流
> （原“待核”为误判）；③ 原生类多数已实质实现，确认的 no-op 只有 `Graphics`。

1. **`Graphics` 绘制 API 是真 no-op（最关键缺口）**：`Fvm.cpp:4799-4813` 的
   `beginFill/endFill/drawRect/drawCircle/lineStyle/moveTo/lineTo/clear` 全部为空实现。
   AVM2 游戏凡经 Graphics 动态绘制的内容（UI、粒子、进度条等）都画不出来——这是“画面不对”
   的头号直接原因，也是补齐 ROI 最高的一项。
2. **`TextField` 仅部分实现**：只有 `text` 的 get/set（`Fvm.cpp:4726-4733`）；缺
   `setTextFormat/defaultTextFormat/appendText/htmlText/autoSize` 与按字号的排版测量（width/height）。
3. **未注册的原生类（按需补齐）**：`Bitmap/BitmapData`、`Point/Rectangle/Matrix/ColorTransform`、
   `Loader/URLLoader`、`Sound/SoundChannel`、`XML/ByteArray` 等——`ObjKind` 已声明类型，
   但 `registerFlashApi` 未注册其成员；依赖它们的脚本会走 undefined 路径。
4. **通用性弱**：围绕特定游戏打磨，非“通吃所有 SWF”的通用播放器（Ruffle/Lightspark 的目标）。
5. **渲染保真**：nanovg 软件光栅；滤镜/混合/渐变/字体 hinting 的像素级精度弱于 Flash 合成器。
6. **无 GPU / 多线程**：单线程 CPU 渲染，复杂内容吞吐与 Lightspark 的多线程 OpenGL 有差距。
7. **法律清洁度**：源自逆向真实 Player 二进制（函数偏移），与 Ruffle/Lightspark 的“干净室”
   路线不同——换取解码保真，但带来许可/合规层面的风险（需项目方自行评估）。

### 已证实的强项（初版误列为短板，现更正）
- **播放循环真实且精细**：`Player::tick`（`Player.cpp:594-677`）每帧完成“推进播放头 → 快照静态帧 →
  重建精灵运行态子树 → 各 MovieClip 独立推进并执行其内部帧脚本（`advanceClips`）→ 新精灵首帧脚本 →
  帧脚本（AVM1）→ ClipActions（Load/EnterFrame，`fireClipEvents`）→ AVM2 `vm->tick()`
  （enterFrame/补间/定时器）→ 流式音频”。这是**按每个 MovieClip 独立时间轴驱动**的，
  比“全局统一 5 阶段”更贴近 Flash 真实语义（Flash 中每个 MovieClip 自带播放头）。
- **启动时序完全由 SWF 自身 AS3 驱动（FIX-2026-028）**：`Player.cpp` 不含任何
  "迁就 swf" 的阶段模拟——无配置开关、影片副本、冻结 Vm、伪造进度或门户计时器；
  只做解码资源、按时间轴播放、执行 AVM1/AVM2 脚本。`Fvm.cpp` 修正通用首帧语义
  （根时间轴 `cid=0` 同样在第 0 帧持有，使 `addFrameScript(0, ...)` 得以执行），
  于是 preloader 首页 → 进度条（`progreso` 的 ENTER_FRAME）→ 门户页 → 标题屏
  全部按 SWF 字节码顺序出现，播放器不插入任何等待。默认无环境变量回放 120 帧已验证。
- **事件系统已完整**：`Vm::dispatchEvent`（`Fvm.cpp:4104-4167`）实现捕获→目标→冒泡三段流 +
  `stopPropagation/stopImmediatePropagation/preventDefault`/优先级/`useCapture`；
  `addEventListener/removeEventListener/hasEventListener/willTrigger` 亦已实现（`4048-4200`）。
- **变换属性齐全**：`x/y/scaleX/scaleY/rotation/alpha/visible/name` 的 setter（`Fvm.cpp:2173-2190`）
  与 getter（`4427-4489`）均已实现。

---

## 5. 一句话总结
本项目是一个**由逆向还原驱动、针对真实 AS3 游戏、双显示模型并存、内置 AVM2 虚拟机（类系统/
事件三段流/Tween/定时器）+ 按剪辑驱动的精细播放循环 + 工程化自诊断与可脚本化回归**的 C++ 播放器。
经代码实证，真正的短板是 **`Graphics` 绘制 API（no-op）、`TextField` 排版、若干未注册原生类、
渲染像素保真与通用性**，而非“缺运行时”。
