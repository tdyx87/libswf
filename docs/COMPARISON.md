# 本项目（swfparser）与开源实现的对比分析

> 本文件把 `PROJECT_ANALYSIS.md`（本端事实）与 `ARCHITECTURE_AND_DESIGN.md`（采集的 Ruffle /
> Lightspark / Gnash / Shumway / Tamarin / OpenFL 架构）放在同一张桌子上，回答两件事：
> **① 差距在哪（相对开源项目缺什么）；② 哪里比开源项目更强 / 更独特。**
>
> 一句话：开源项目追求“通用、干净室、跨内容 100% 覆盖”；本项目追求“针对真实 AS3 游戏、逆向高保真、
> 工程化可回归”。两者目标不同，差距与优势都由此而来。

---

## 1. 总览对比矩阵

| 维度 | Ruffle(Rust) | Lightspark(C++) | Gnash(C++) | Shumway(JS) | Tamarin(C++) | OpenFL(Haxe) | **本项目 swfparser(C++)** |
|---|---|---|---|---|---|---|---|
| 目标定位 | 通用播放器(生产) | 通用播放器(Alpha) | AVM1 优先 | Canvas 兼容层 | AVM2 参考 VM | Flash API 重实现 | **针对性 AS3 游戏** |
| 语言 | Rust/WASM | C++ | C++ | TS/JS | C++ | Haxe | **C++** |
| 显示模型 | 单树(DisplayObject) | 单树+双列表 | 单树 | JS 对象树 | 无(FP专有) | API 表面 | **双模型(静态+运行时树)** |
| AVM2 VM | 完整 | ABCVm+JIT可选 | 弱 | 完整(avm2) | 完整(参考) | 借宿主 | **完整(类系统+事件+Tween+定时器)** |
| 原生类策略 | AS3 写 playerglobal | C++ 直接映射 | C++ 部分 | AS3→builtin.abc | 无 DOM | Haxe 全量 | **C++ 直接实现（仅 Graphics 为空壳）** |
| 帧/事件循环 | 5 阶段+帧累积 | 多线程+FramePhase | 弱 | 事件循环 | 无 | 借宿主 | **按剪辑驱动 + AVM2 tick（等效且更贴 Flash 语义）** |
| 渲染 | 多后端(wgpu/GL) | OpenGL+缓存 | AGG/GL | Canvas | 无 | 借宿主 | **nanovg 软件(矢量重光栅)** |
| 启动/帧时序 | 完整 | 较完整 | 弱 | 有 | 无 | 借宿主 | **已实现（加载→门户→游戏时序复现）** |
| 自诊断 | 有 | 有 | 有 | 有 | 有 | — | **AVM2 opcode 覆盖率自检 CLI** |
| 可脚本化回归 | 有测试框架 | 有 | 有 | 有 | 单测 | — | **无头播放+输入模拟+帧 PNG 导出** |
| 音视频 | 有 | 有(CURL/avcodec) | 部分 | 有限 | 无 | 借宿主 | **FFmpeg 全链路** |
| GPU/多线程 | wgpu | 多线程 GL | 有 | 单线程 | JIT | 借宿主 | **单线程 CPU** |
| 法律路线 | 干净室 | 干净室 | GPL | Apache | MPL/GPL | MIT | **逆向还原(函数偏移)** |

---

## 2. 差距分析（相对开源项目缺什么）

### 2.1 通用性 / 内容覆盖
- **Ruffle/Lightspark 的目标是“任何 SWF 都能播”**，覆盖 SWF v1–v20+、AVM1+AVM2、海量 `flash.*` 类。
- 本项目**曾用有限几个真实游戏打磨**（部分历史注释仍可见 `swf2.swf`/`service_error_mc` 等名字），
  对未覆盖的 tag / 类 / AVM1 边缘行为可能直接缺失。但播放主流程（`Player.cpp` / `Fvm.cpp`）
  已不含针对具体 SWF 的分支或时序迁就（FIX-2026-028）。
- **结论**：若目标是“通用播放器”，本项目广度远不及 Ruffle；若目标是“把某几个游戏播得像”，
  这反而是专注优势（见第 3 节）。

### 2.2 启动 / 帧时序（FIX-2026-028 后完全由 AS3 驱动）
- 曾存在伪造 1.5s+12.5s 启动时序、冻结主 Vm 的旧路径（`updateLoadingProgress`
  伪造 41→99% 进度），FIX-2026-020 把它默认自动启用，导致开局 336 帧人为停留；
  FIX-2026-027 先把它降为可选开关，**FIX-2026-028 已把整套子系统从代码中彻底删除**
  （含 `SWF_STARTUP_SIM` / `SWF_SIM_DBG` / 影片副本 / 冻结 Vm / 门户计时器 /
  会改变语义的 `SWF_NO_HOLDFRAME`、`SWF_LEGACY_ROOT_FRAME` 开关）。
- **默认按 AS3 字节码顺序播放**：`Fvm.cpp` 的根时间轴首帧持有让第 0 帧
  `addFrameScript(0, ...)` 正常执行，preloader 首页 → 进度条（`progreso` ENTER_FRAME）
  → 门户页 → 标题屏 → 关卡页全部由 SWF 自身脚本驱动。默认无环境变量 120 帧回放已验证
  （`build/compare/pure2_strip.png`）；点击链 `--click 80:593:574 --click 140:473:698`
  仍完整（`build/compare/pure2_click_strip.png`），帧号比模拟时代提前是因为不再有人为停留。
- 实际的帧循环 `Player::tick`（`Player.cpp:594-677`）真实存在且精细：每帧“推进播放头 → 快照静态帧 →
  重建精灵运行态子树 → 各 MovieClip 独立推进并执行内部帧脚本 → 新精灵首帧脚本 → 帧脚本（AVM1）→
  ClipActions（Load/EnterFrame）→ AVM2 `vm->tick()`（enterFrame/补间/定时器）→ 流式音频”。
- **结论**：本项目按**每个 MovieClip 独立时间轴**驱动，比“全局统一 5 阶段”更贴近 Flash 语义
  （Flash 中每个 MovieClip 自带播放头）。此项**不是差距**，在针对性上反而优于简单全局循环。

### 2.3 原生类（多数已实质实现；真缺口：Graphics / TextField / 未注册类）
- **已实质实现（代码实证）**：`DisplayObject` 变换属性（`x/y/scaleX/scaleY/rotation/alpha/visible`，
  setter `Fvm.cpp:2173-2190`、getter `4427-4489`）；显示列表操作（`addChild/removeChild/addChildAt/
  removeChildAt/getChildAt/getChildByName/getChildIndex/setChildIndex`，`4202-4351`）；
  `MovieClip` 时间轴（`play/stop/gotoAndPlay/gotoAndStop/currentFrame/totalFrames/currentLabels/
  currentLabel`，`4566-4721`）；`EventDispatcher` 全套；`flash.utils.Timer`（完整，`4734-4797`）。
- **真 no-op（关键缺口）**：`Graphics` 的 `beginFill/endFill/drawRect/drawCircle/lineStyle/moveTo/
  lineTo/clear` 全部空实现（`Fvm.cpp:4799-4813`）——AVM2 动态绘制的内容画不出来。
- **部分实现**：`TextField` 仅 `text` get/set（`4722-4733`）；缺 `setTextFormat/defaultTextFormat/
  htmlText/autoSize` 与排版测量。
- **未注册的原生类**：`Bitmap/BitmapData`、`Point/Rectangle/Matrix/ColorTransform`、`Loader/URLLoader`、
  `Sound/SoundChannel`、`XML/ByteArray` 等（`ObjKind` 已声明但 `registerFlashApi` 未注册成员）。
- **结论**：补齐优先级 = `Graphics` > `TextField` 排版 > 高频未注册类；对照 Lightspark
  `flashdisplay.cpp` 与 OpenFL 的 API 语义逐项填实。

### 2.4 渲染保真（nanovg 软件 vs Flash 合成器）
- 本项目 `Render` 用 nanovg 软件光栅，**矢量重光栅**（任意分辨率清晰）是亮点；但滤镜（blur/glow/
  dropshadow）、14 种混合模式、渐变 spread/interp 的**像素级合成精度**弱于 Flash 自研合成器，
  也弱于 Lightspark 的 OpenGL 硬件合成与 Ruffle 的多后端。
- 数据模型已承载 `Filter` 与 `BlendMode`，缺口在**光栅化算法**而非“没有这些概念”。

### 2.5 事件模型（初版误判，已证实完整——非差距）
- **修正**：`Vm::dispatchEvent`（`Fvm.cpp:4104-4167`）已实现**完整三段事件流**（捕获→目标→冒泡），
  含 `stopPropagation`/`stopImmediatePropagation`/`preventDefault`、监听器优先级、`useCapture`；
  `addEventListener/removeEventListener/hasEventListener/willTrigger` 亦已实现（`4048-4200`）。
  与 Ruffle `container.rs` 的捕获-目标-冒泡模型对齐。
- **结论**：事件模型**不是差距**，与 Ruffle 相当；初版“待核”系未读实现所致。

### 2.6 性能（无 GPU / 单线程）
- Lightspark 多线程 OpenGL、Ruffle wgpu；本项目单线程 CPU 软件光栅。复杂内容（大量矢量/滤镜/
  大尺寸）吞吐落后。但本项目用**异步多缓冲 PNG** 把“导出”吞吐优化到了生产可用（`main.cpp`）。

### 2.7 法律清洁度
- Ruffle/Lightspark/Gnash 都是**干净室**（不依赖逆向 Adobe 代码），规避许可风险。
- 本项目注释直接引用**逆向得到的真实 Player 函数偏移**（`sub_490330` 等）。这换来解码保真，
  但带来合规/许可层面的不确定性，**需项目方自行评估**（这是相对开源项目最“弱”的非技术项）。

---

## 3. 本项目的优势 / 独特之处（相对开源项目强在哪）

### 3.1 逆向还原驱动的解码保真（最强差异点）
- 直接对标真实 Flash Player 二进制的解析/解码/标签分派路径（`main.cpp:1-3`，IDA 函数偏移）。
- 对**特定真实内容**的解码行为更贴近原版，而 Ruffle/Lightspark 靠干净室“重新猜测”某些历史行为，
  对怪异/版本特有 tag 的还原可能不如逆向精准。

### 3.2 内置 Tween + 虚拟定时器 + flash.utils.Timer
- `Fvm.hpp` 直接仿真 **TweenLite/TweenMax** 补间引擎（`TweenTask`/`advanceTweens`）、
  `setTimeout/setInterval`、`flash.utils.Timer`。
- 开源项目通常不内建第三方补间库——它们依赖库自身 AS3 代码跑起来；本项目把它**下沉为运行时原语**，
  对“用 Tween 做动画的游戏”更稳、更快收敛。

### 3.3 AVM2 opcode 覆盖率自检（工程化自诊断）
- `--avm2-opcodes` 直接打印“标准 opcode 已实现数 / 缺失清单”（`main.cpp:81-122`）。
- 开源项目多靠单测/CI 间接反映覆盖；本项目把“VM 还差哪些指令”变成**一个命令就能看的报告**，
  是精准补 VM 的利器。

### 3.4 可脚本化无头播放 + 输入模拟 + 帧序列导出（回归/对比神器）
- `--play-game` 配合 `--click/--hover/--drag/--key` 以“第 N 帧触发”复现交互，逐帧导出 PNG。
- 这等于把“播放结果”固化成**可像素级 diff 的帧序列**，可自动化对比“本项目 vs 真实 Player 截图”。
  Ruffle 有测试框架但未必提供如此直接的“脚本化交互 + 帧 dump”单命令管道；本项目在这方面更顺手。

### 3.5 矢量重光栅（任意分辨率清晰）
- `renderMovieToBuffer(…, targetW, targetH)` 按设备尺寸重新光栅化，放大仍清晰（`Render.hpp:13-18`）。
- 许多重实现按固定舞台尺寸光栅；本项目的矢量保留使其在**高 DPI / 缩放 / 比对**时更优。

### 3.6 有向边非零环绕填充（几何正确性）
- `Decoder.hpp` 用 `FillEdge` 的 `sign` 直接表达 SWF“边两侧填充”语义，正确覆盖并集与洞，无需启发式。
- 这是相对“直接光栅轮廓”的**正确性优势**，且实现精巧。

### 3.7 双显示模型并存（针对性架构）
- 静态时间轴 `DisplayItem`（保真 PlaceObject，每帧重建）+ 运行时树 `fvm::DisplayObject`
  （支撑 AVM2 动态搭建）。二者分工明确，比“强行用一棵树同时服务时间轴与脚本”更贴合 SWF 双来源现实。
- Ruffle/Lightspark 用单树覆盖两者；本项目的双模型虽增加协调成本，但对“时间轴动画 + 脚本搭建”
  混合内容更直观。

### 3.8 音视频全链路 + FFmpeg
- 解码/播放/导出 MP3/Speex/Nellymoser、流式背景音、VideoStream（ScreenVideo/VP6/AVC）一体。
- 在“轻量重实现”里算完备；Shumway 音视频有限，Gnash 也非强项。

### 3.9 小、可读、单语言
- ~15 个源文件、纯 C++、无 Rust/web 工具链负担；相对 Ruffle 的巨型 Rust 仓库或 Lightspark 的 C++ 庞杂，
  **更容易审计、针对特定游戏精修**。

---

## 4. 结论：定位与取长补短

### 4.1 定位差异（决定差距与优势的根源）
- **开源项目**：通用、干净室、跨内容 100% 覆盖、长期生态。
- **本项目**：针对真实 AS3 游戏的、逆向高保真的、工程化可回归的“精准复刻”。

### 4.2 该向开源学的（补差距，按 ROI 排序）
1. **Lightspark 的 `Graphics` 实现**：本项目唯一确认的 no-op 原生类；对照 `flashdisplay.cpp` 把
   `beginFill/drawRect/drawCircle/lineStyle/moveTo/lineTo` 接到渲染几何生成——**ROI 最高的一项**
   （直接决定 AVM2 动态绘制内容可见与否）。
2. **OpenFL / Lightspark 的 `TextField` 排版**：补 `setTextFormat/defaultTextFormat/htmlText/autoSize`
   与字号测量（width/height）。
3. **按需补未注册原生类**：`BitmapData`/`Point`/`Rectangle`/`ColorTransform`/`Loader` 等高频类，
   参考 OpenFL 的 API 语义字典。
4. **Lightspark 的渲染后端**：评估 OpenGL/缓存/无效化，替代或增强 nanovg 软件光栅。

### 4.3 本项目的独特资产（应保留并放大）
- 逆向还原的解码链、Tween/定时器内建、AVM2 覆盖率自检、脚本化无头回归、矢量重光栅、有向边填充、
  音视频全链路。这些在“把某个游戏播得像”的任务上，比通用播放器更高效、更可控。

### 4.4 一句话
差距主要在**`Graphics` 绘制 API（no-op）、`TextField` 排版、未注册的原生类、渲染像素保真、
通用性、GPU/多线程、法律清洁度**；优势主要在**逆向保真、按剪辑驱动的精细帧循环、完整三段事件流、
内置 Tween/定时器、自诊断、可脚本化回归、矢量重光栅、几何正确性、音视频全链路**。
正确的策略不是“变成另一个 Ruffle”，而是**保留针对性优势，优先补 `Graphics` 与 `TextField`**。
