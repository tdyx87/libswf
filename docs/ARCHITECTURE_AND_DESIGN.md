# Flash Player 重实现：开源架构分析与本项目设计方案

> 文档目的：系统分析 Ruffle（Rust）与 Lightspark（C++）两大开源 Flash Player 重实现项目的架构，
> 提炼出“重建 Flash 运行时”必须覆盖的子系统，并据此设计本项目 `swfparser` 的目标架构与落地路线。
>
> 核心结论先说在前：**SWF 文件格式规范定义了“如何读文件”，也定义了显示列表/时间轴的“构造模型”**
> （PlaceObject/RemoveObject/ShowFrame、时间轴推进——即 ShowFrame 时渲染、显示列表由 PlaceObject
> 构成的那部分，规范是明确的）。真正的差距在“运行时平台”四块，它们**不在任何公开规范里**：
> ① 渲染像素算法（矢量/字体/滤镜混合）；② Flash 运行时 API 即 `flash.*` 标准库的实际**行为**
> （规范只给 API 签名，不给实现）；③ 播放器的**事件/时序循环**（enterFrame 派发、帧率、加载时序）；
> ④ AVM1/AVM2 **原生方法**语义。补充：AVM2 字节码格式与 AS3 语言/对象模型由独立的《AVM2 Overview》
> 文档规定，故“执行字节码”本身有规范依据——缺口在字节码所调用的**运行时 DOM**，而非字节码本身。
> 开源项目正是把这四块在原生代码里完整重做了一遍。本文档是“分析 → 设计”的完整依据。

---

## 0. 为什么需要这份文档（问题背景）

基于 SWF 规范重建播放器时，常见的误解是“按代码执行应该一样”。实测与开源代码都表明：

- **AVM2 字节码执行本身是确定性的**（解释器对 opcode 的实现正确，则 `a+b` 结果一致）。
- **但 AS3 业务代码几乎不直接产生画面**，它绝大多数时候调用 Flash 运行时提供的原生类
  （`addChild` / `graphics.drawRect` / `gotoAndPlay` / `addEventListener("enterFrame", …)`）。
- 这些原生类的**行为**由 Flash Player 运行时定义。需澄清：SWF 文件格式规范只给 `flash.*` 类的
  **API 签名**（AS3 语言参考），并不规定其实现；且 AVM2 字节码格式与 AS3 对象模型由独立的
  《AVM2 Overview》规定——所以“字节码怎么执行”是有规范的，缺口在字节码调用的**运行时 DOM**。
- 此外，`enterFrame`/`TimerEvent`/`MouseEvent` 等事件由**播放器的事件循环**按节奏触发，
  规范里没有“播放器怎么调度事件循环”的规定。

因此：“代码执行一样，但结果不一样”的根源，是代码所依赖的**运行时平台**不一样。
本项目的差距也正在此处（详见第二部分审计）。

---

# 第一部分：开源项目架构分析

## 1.1 总体定位

| 维度 | Ruffle | Lightspark |
|---|---|---|
| 语言 | Rust + WASM | C++ / C |
| 成熟度 | 最活跃、最完整，生产可用 | Alpha；官方自述约 **88% Flash API** 覆盖（项目站点数据，可能偏旧） |
| 架构风格 | 三层（frontend / core / render 后端抽象） | 以 `SystemState` 为中心的**多线程流水线** |
| 脚本 | AVM1、AVM2 各自**完整虚拟机** | `abc.h` 的 `ABCVm`（AVM2）+ AVM1 解释器 |
| 原生类实现 | 用 **AS3 自身**写成数百个 `.as` 文件（数量随版本变动，约数百），构建期编译进 `playerglobal` | 直接映射为 **C++ 类**（与本项目同语言，最该参考） |
| 渲染 | `RenderBackend` trait，wgpu/opengl/software | OpenGL/GLES 硬件加速 + 缓存/无效化 |

两者共同印证：**重建 Flash = 重建运行时平台**，文件解析只占很小一层。

## 1.2 目录结构与模块职责

### Ruffle（按 `core/src/` 组织）
```
core/src/
├── player.rs              # Player::tick() 主循环 + 帧累积
├── frame_lifecycle.rs     # 帧生命周期阶段定义（Construct→FrameScripts→EnterFrame→ExitFrame→Render）
├── context.rs             # UpdateContext：连接 Player 与 GC/Stage/VM/Renderer/Audio 的共享状态
├── library.rs / loader.rs # 资源库与加载管理
├── display_object.rs      # DisplayObject 基类
├── display_object/        # 各显示类型实现
│   ├── stage.rs           # Stage 根
│   ├── movie_clip.rs      # MovieClip 时间轴（preload/run_frame/ChildContainer 深度排序）
│   ├── container.rs       # 容器 + AVM2 捕获-目标-冒泡事件
│   ├── interactive.rs     # InteractiveObject（命中测试、输入）
│   ├── shape.rs / graphic.rs / bitmap.rs / text.rs / edit_text.rs / video.rs / morph_shape.rs
├── avm2/                  # AVM2 虚拟机
│   ├── globals/flash/…    # 内置类的 AS3 源码（485 个 .as）+ 对应 Rust 绑定（.rs）
│   │   ├── events/        # MouseEvent/KeyboardEvent/TimerEvent… 等事件类
│   │   ├── text/engine/   # FTE（Flash Text Engine）
│   ├── object/            # AVM2 对象包装（TObject trait）
├── render/                # 渲染后端抽象（wgpu/opengl/software）
├── font.rs / html/        # 字体与文本布局
desktop/  web/  render/    # 前端与后端实现
```

### Lightspark（按 `src/` 组织）
```
src/
├── swf.cpp / swf.h        # SystemState 中央编排 + 生命周期 + 线程架构
├── parsing/
│   ├── tags.cpp / tags.h  # TagFactory：读 SWF 标签 → 实例化 C++ 显示对象
├── scripting/
│   ├── abc.h              # ABCVm：AVM2 核心（解释执行，可选 LLVM JIT 默认关）
│   ├── avm1_interpreter.cpp
│   ├── flash/display/     # 内置显示类的 C++ 实现（DisplayObject.h / flashdisplay.h …）
│   │   ├── DisplayObject.h / flashdisplay.h   # 类继承树 + 双显示列表系统
│   │   ├── RootMovieClip.cpp / DisplayObject.cpp
├── backends/input.h       # 输入后端
├── platforms/engineutils.h# 平台抽象层 EngineData
```

**关键共性**：两者都把“显示对象”做成**真正的 C++/Rust 类继承树**，并把 SWF 标签解析与
内置类实现分离到独立模块。

## 1.3 SWF 解析层（“读文件”）

- **Ruffle**：`swf` crate 把二进制流解析为 Rust 结构体；`MovieClip::preload()` 读标签、
  `run_frame()` 执行帧；标签 → 显示对象的转换由类似 `TagFactory` 的机制完成。
- **Lightspark**：`src/parsing/tags.cpp` 的 `TagFactory` 读 SWF 标签，在解析期就实例化对应
  C++ 显示对象（`MovieClip` 从时间轴或 `new` 创建）。

> 这一层正是 SWF 文件格式规范覆盖的部分，本项目（Parser/TagReader/Decoder）已具备基础。

## 1.4 AVM2 虚拟机（“执行 AS3 代码”，核心差距之一）

- **Ruffle**：
  - 完整 AVM2 VM：执行 ABC 字节码，对象模型基于 `TObject` trait + trait 集合。
  - **原生/内置类用 AS3 自身实现**：`core/src/avm2/globals/` 下数百个 `.as` 文件（数量随版本变动，约数百；早期资料记为 485）
    （如 `MovieClip.as`、`Graphics.as`、`TextField.as`、`MouseEvent.as`…），由
    `core/build_playerglobal/` 在**构建期编译成字节码**作为 `playerglobal` 标准库，
    直接运行在 Ruffle 自己的 AVM2 上。
  - 这是极重要的架构决策：**“用 AS3 实现 AS3 标准库”**，避免逐个 native 方法手搓语义，
    从而获得与原版更接近的内置行为。
- **Lightspark**：
  - `src/scripting/abc.h` 的 `ABCVm`：解释执行 ABC 字节码，支持可选 LLVM JIT（默认关闭）。
  - 内置类直接以 C++ 类实现（见 1.5 / 1.6）。

> 本项目对应的 `Fvm.cpp`（AVM2 解释器）+ `Abc.cpp` + `Avm2Opcodes.cpp` 已能执行字节码
> （实测 20 帧内 253 次 `addFrameScript`、20 次 `enterFrame`），但**内置类语义是最小实现/空壳**，
> 这是画面不对的主因之一。

## 1.5 显示对象模型 / 显示列表（“画什么”的地基）

这是回应“显示列表不就是 PlaceObject”误解的关键：开源项目把显示列表建成**带 parent/children、
深度、事件的真树**，且区分两套来源。

- **Ruffle**（`display_object/`）：
  - 基类 `DisplayObject` → `DisplayObjectContainer`/`InteractiveObject` →
    `MovieClip`/`Shape`/`Sprite`/`SimpleButton`/`TextField`/`Video`…
  - `Stage` 为根；每个对象有 `parent` 与 `children`；`MovieClip` 在 `ChildContainer` 中
    按深度排序管理子对象。
- **Lightspark**（`flashdisplay.h`）：
  - 严格 C++ 继承树：`DisplayObject` → `DisplayObjectContainer`/`InteractiveObject`
    → `MovieClip`/`Shape`/`Sprite`/`SimpleButton`。
  - **显示列表双系统**：
    - 动态列表：`std::vector<DisplayObject*>`（对应 AS3 的 `addChild`/`removeChild` 索引式 API）；
    - 遗留深度系统：`map<int32_t,DisplayObject*>`（对应 AVM1 的 `PlaceObject` 深度定位，
      如 `LEGACY_DEPTH_START = -16384`）。
  - 访问受 `mutexDisplayList` 互斥锁保护（线程安全）。

> 即“PlaceObject（时间轴）”与“addChild（脚本）”是**并存的两套机制**，由统一的
> `DisplayObjectContainer` 管理。本项目当前的 `DisplayItem` 模型（见第二部分）需向这种
> 稳定对象树演进。

## 1.6 播放循环 / 帧生命周期 / 事件循环（“何时、按什么顺序”，核心差距之二）

- **Ruffle**（`player.rs` + `frame_lifecycle.rs`）：
  - `Player::tick(dt)` 由宿主按时间增量驱动；内部 `frame_accumulator` **累积时间匹配帧率**：
    时间不够就攒着，够了才跑帧；宿主卡顿时可一次跑多帧或跳帧。
  - 每帧**严格分阶段**：
    1. `Construct`：按时间轴推进，实例化新显示对象并加入显示列表；
    2. `FrameScripts`：执行 `addFrameScript()` 注册的 AVM2 帧脚本；
    3. `EnterFrame`：**递归**向所有显示对象派发 `ENTER_FRAME` 事件；
    4. `ExitFrame`：仅 AVM2 派发 `EXIT_FRAME`；
    5. `Render`：遍历显示列表，向 `RenderBackend` 提交渲染命令。
  - 事件模型：AVM2 实现**捕获-目标-冒泡**（capture-target-bubble）；`ENTER_FRAME` 递归到所有子对象。
- **Lightspark**（`swf.cpp` 多线程）：
  - `SystemState` 编排 `ParseThread` / `RenderThread` / `InputThread` / `ABCVm` 四线程。
  - 显示对象通过 `FrameContainer` + `FramePhase` 参与帧更新；
    改了就 `requestInvalidation()` → `invalidate()` 生成 `IDrawable` 交给渲染线程。

> 本项目 `Player.cpp:18` 注释**自承**的“前数百帧静止、脚本不推进”，正是缺了这套
> 显式分阶段 + 帧率累积 + `ENTER_FRAME` 递归派发的后果。

## 1.7 渲染后端（“画成什么样”，核心差距之三）

- **Ruffle**：`RenderBackend` 抽象，后端有 wgpu / opengl / software；矢量、文本（freetype）、
  滤镜均在其上实现。
- **Lightspark**：OpenGL/GLES 硬件加速，配合缓存与无效化（`requestInvalidation`）机制。

> 本项目用 `nanovg`（软件光栅）。矢量/文本/滤镜保真度是差距来源，但优先级低于
> 显示对象模型与事件循环（先把“画什么/何时画”做对）。

## 1.8 对比小结

| 子系统 | 是否在 SWF 规范内 | Ruffle 做法 | Lightspark 做法 | 本项目现状 |
|---|---|---|---|---|
| 文件解析 | ✅ 是 | swf crate | TagFactory | 有基础（Parser/TagReader） |
| AVM2 执行 | 部分（字节码格式） | 完整 VM + playerglobal(AS3写) | ABCVm + 可选JIT | VM 可跑，覆盖不足 |
| 原生类语义 | ❌ 否（仅 API 签名） | 数百 .as 编译为标准库 | C++ 类直接实现 | 最小实现/no-op |
| 显示对象树 | ❌ 否 | DisplayObject 继承树 | C++ 继承树 + 双列表 | DisplayItem（待重构） |
| 帧/事件循环 | ❌ 否 | 5 阶段 + 帧累积 + 递归派发 | 多线程 + FramePhase | 缺显式阶段（自承缺陷） |
| 渲染保真 | ❌ 否 | 多后端 | OpenGL + 缓存 | nanovg 软件 |

## 1.9 其他开源项目（Gnash / Shumway / Tamarin / OpenFL）

除 Ruffle、Lightspark 外，下列项目从额外角度补足架构认知：

### 1.9.1 Gnash（GNU，C++，AVM1 优先）
- 源自 `gameswf`，GNU 项目；历史上**以 AVM1/AS2 为主**（覆盖到 SWF v7、约 80% AS2），AVM2 支持较晚且不完整。
- 渲染用 AGG/OpenGL，GUI 为 GTK+/Qt。
- **启示**：印证了“先 AVM1 后 AVM2”的演进路径——本项目当前 `Player.cpp` 正是以 `avm1::IClip`/
  `avm1::ITimeline` 接口为中心，与 Gnash 的 AVM1-first 思路一致；也说明 **AVM2 才是难啃的部分**，
  不能停留在 AVM1 语义上。

### 1.9.2 Shumway（Mozilla，2012–2016，JS/TS，已停）
- 用 **TypeScript/JavaScript** 实现，把 SWF 当“兼容层”在浏览器 Canvas 上跑；AVM2 实现位于 `src/avm2`。
- **关键做法与 Ruffle 同源**：AS3 内置类（`Array`/`Object` 等）用 **AS3 写成 `.as`，编译为 `builtin.abc`**
  作为首个加载的字节码——证明“用 AS3 自身实现标准库”这一模式**与语言无关**（Rust/JS 都这么干）。
- **启示**：提供了“SWF → 目标平台图形 API（Canvas）”的映射样板；其 AVM2 运行时含对象模型、类系统、
  原生对象实现，可作为纯 JS 环境下运行 AS3 的参考。

### 1.9.3 Tamarin（Adobe + Mozilla，C++，AVM2 参考虚拟机）
- Adobe 于 2006 将 **AVM2 开源**（约 13.5 万行 C++，带 JIT），即 Flash Player 9 所用的虚拟机。
- **重要边界**：Tamarin 实现了 **ABC 字节码的执行 + AS3 对象模型**，但**不包含 Flash 运行时 DOM**
  （没有 `flash.*` 显示类、没有显示列表、没有事件循环）——那些仍属 Flash Player 专有。
- **启示**：这正是“字节码有规范/有开源实现，但运行时 API 没有”的鲜活例证；Tamarin 是校验本项目
  `Fvm.cpp` opcode 语义准确性的**权威参照**。

### 1.9.4 OpenFL（Haxe，Flash API 重实现）
- 跨平台开源项目，用 **Haxe 干净重实现了整套 `flash.*` API**，并能导入 SWF 资源。
- **启示**：直接回答“Flash API 表面到底有多大、各方法语义应是什么”——是本项目 Phase 3
  实现原生类时最贴近的“API 语义字典”，比逐个猜方法行为可靠得多。

### 1.9.5 小结：六类项目的分工启示
| 项目 | 语言 | 强项 / 对应本项目的参考点 |
|---|---|---|
| Ruffle | Rust | 完整运行时 + playerglobal 模式（原生类用 AS3 写） |
| Lightspark | C++ | 同语言原生类映射（`flash/display`）、多线程渲染 |
| Gnash | C++ | AVM1-first 历史路径（对照本项目当前 AVM1 中心） |
| Shumway | JS/TS | SWF→Canvas 映射；同样的 AS3 内置类模式（语言无关证明） |
| Tamarin | C++ | AVM2 opcode/对象模型事实来源（校验 `Fvm`） |
| OpenFL | Haxe | `flash.*` API 语义字典（原生类实现对照） |

---

# 第二部分：本项目（swfparser）现状审计

## 2.1 现有结构

源码位于 `src/`，头文件集中在 `swf/` 包含目录下（`swf/Player.hpp`、`swf/Fvm.hpp`、
`swf/Decoder.hpp`、`swf/Render.hpp`、`swf/Avm1.hpp`、`swf/Audio.hpp`、`swf/Avm2Opcodes.hpp`）。

| 文件 | 职责（基于命名与已知实现推断） |
|---|---|
| `Parser.cpp` / `TagReader.cpp` | SWF 文件解析、Tag 读取 |
| `Decoder.cpp` | `Movie` 完整定义、角色（Character）解码 |
| `Decompress.cpp` | SWF 解压（zlib/lzma） |
| `Abc.cpp` / `Avm2Opcodes.cpp` | AVM2 字节码（ABC）与 opcode 表 |
| `Fvm.cpp` | **AVM2 解释器 + 显示对象模型**（`fvm` 命名空间；`Inst` 字节码游标；`runMovieScripts`/`addFrameScript`/`runFrameScripts`；`DisplayObject`/`DisplayItem`；命中测试） |
| `Avm1.cpp` / `MovieClipCommands.cpp` | AVM1 解释器 / MovieClip 时间轴命令 |
| `Player.cpp` | **播放主循环**：以 `avm1::IClip` / `avm1::ITimeline` 接口为中心驱动 `DisplayItem` 树；含 `SpriteClip`、`Player::ItemClip` 适配器；`runClipFrameScript`；启动模拟开关 |
| `Render.cpp` | 渲染（nanovg 软件光栅） |
| `Audio.cpp` / `FFmpeg.cpp` | 音频解码与播放 |
| `player_win.cpp` | Windows 平台播放前端 |
| `main.cpp` | CLI（含 `--play-game` 无头回放等模式） |

**架构要点**：`Player` 的播放控制以 **AVM1 接口语义**（`avm1::IClip`/`avm1::ITimeline`）为中心，
AVM2 通过 `Fvm` 接入（`runMovieScripts` 在启动跑 ABC、`addFrameScript`/`runFrameScripts`
驱动帧脚本）。即“主循环偏 AVM1，AVM2 是后接的脚本引擎”。

## 2.2 与开源实现的差距（逐子系统）

1. **显示对象模型**：当前 `DisplayItem` 更偏“每帧重建的扁平结构”（`Player::ItemClip` 注明
   “每帧重建，有效期仅限本帧”）。缺少稳定的 `parent/children` 树、深度系统、事件冒泡——
   与 Ruffle/Lightspark 的 `DisplayObject` 继承树差距明显。
2. **事件/帧循环**：未实现 Ruffle 式“Construct→FrameScripts→EnterFrame→ExitFrame→Render”
   显式阶段；`ENTER_FRAME` 递归派发、`EXIT_FRAME`、捕获-冒泡模型待补；帧率累积（`frame_accumulator`）
   未实现。`Player.cpp:18` 已自承“前数百帧静止”缺陷。
3. **原生类**：`MovieClip`/`Graphics`/`TextField`/`Sprite` 等在 `Fvm` 中多为最小实现或 no-op
   （此前调试已确认），导致 AVM2 搭出的显示列表是错的。
4. **AVM2 覆盖度**：VM 能执行（实测 253 `addFrameScript`/20 `enterFrame`），但 opcode 与原生方法
   绑定覆盖不足。
5. **渲染保真**：nanovg 软件光栅，矢量/文本/滤镜保真度有限（优先级低于前四项）。
6. **构建**：已统一为显式生成器（Windows `Visual Studio 15 2017` 匹配 conan x64/v141 工具链；
   Linux `Ninja`），无自创生成器。

---

# 第三部分：目标架构设计（基于上述分析）

## 3.1 设计原则

1. **分层单一职责**：解析 / 虚拟机 / 运行时核心 / 原生类库 / 后端，边界清晰。
2. **显示对象为“稳定树”**：`DisplayObject` 拥有持久 `parent/children`，不再每帧重建；
   深度系统与动态列表并存（对齐 Lightspark 双系统）。
3. **帧循环显式分阶段**：严格实现 Ruffle 的 5 阶段 + 帧率累积，根治“前数百帧静止”。
4. **原生类用 C++ 完整实现**：参考 Lightspark `src/scripting/flash/` 的映射方式，
   优先补齐 swf2.swf 高频调用的类。
5. **AVM2 持续完善**：VM opcode 覆盖 + native method 绑定，使“代码执行”真正产出正确结果。
6. **Ruffle 的 playerglobal 思路可作为长期选项**：当 AVM2 足够完整后，可考虑把高频内置类
   用 AS3 实现并在本 VM 上编译运行，降低手搓语义的偏差。

## 3.2 总体分层

```
L5  后端层      Render(nanovg/多后端) · Audio(FFmpeg) · Input(平台)
L4  原生类库    MovieClip / Sprite / Graphics / TextField / Event / …（C++ 实现，参考 Lightspark）
L3  运行时核心  DisplayObject 树 · Player 帧循环 · 事件系统 · Library/Loader · Stage
L2  AVM 层      AVM2 VM(Fvm) · AVM1(兼容) · opcode 表
L1  解析层      SWF → Movie / Character / ABC（Parser/TagReader/Decoder）
```

## 3.3 各子系统设计

### 3.3.1 显示对象模型（重构 `DisplayItem` → `DisplayObject` 树）
- 基类 `DisplayObject`：`parent`、`children`（vector）、`depth`、`name`、`visible`、
  `x/y/scale/rotation`、`transform`、`bounds()`、`hitTest()`。
- 派生：`DisplayObjectContainer`（含 `addChild`/`removeChild`/`addChildAt`，维护 vector 与
  depth map 双系统）、`InteractiveObject`（事件/命中）、`MovieClip`/`Sprite`（时间轴）、
  `Shape`/`Graphic`、`TextField`、`Bitmap`、`Video`。
- `Stage` 为全局根，管理 `frameRate`、`align`、`requestInvalidation`。
- 事件冒泡：实现捕获-目标-冒泡三阶段（对齐 Ruffle `container.rs`）。

### 3.3.2 帧生命周期（对齐 Ruffle `frame_lifecycle.rs`）
- `Player::tick(dt)` + `frame_accumulator`：累积时间，达到 `1/frameRate` 才推进一帧，
  支持跳帧/补帧。
- 每帧顺序：`Construct`（时间轴实例化）→ `FrameScripts`（跑 `addFrameScript`）→
  `EnterFrame`（**递归**向整棵树派发 `ENTER_FRAME`）→ `ExitFrame`（AVM2）→ `Render`。
- 此设计直接消除 `Player.cpp:18` 自承的“启动静止”缺陷。

### 3.3.3 事件系统
- 统一事件类型（`Event`/`EventDispatcher`）：`ADDED` / `ADDED_TO_STAGE` / `REMOVED` /
  `ENTER_FRAME` / `EXIT_FRAME` / `MouseEvent` / `TimerEvent`…
- `ENTER_FRAME` 每帧递归派发至所有显示对象（游戏动画/Tween 依赖此驱动）。
- 输入线程/回调将鼠标键盘转化为 `MouseEvent`/`KeyboardEvent` 并命中测试后派发。

### 3.3.4 原生类库（参考 Lightspark `flash/display`）
- 按 Flash API 分类实现 C++ 类：
  - `display/`：`MovieClip`（play/stop/gotoAndPlay/gotoAndStop/当前帧）、`Sprite`、
    `Graphics`（beginFill/drawRect/drawCircle/lineTo… 生成几何）、`TextField`（text/appendText/
    defaultTextFormat）、`Stage`、`DisplayObjectContainer`。
  - `events/`：`Event`/`EventDispatcher`/`MouseEvent`/`KeyboardEvent`/`TimerEvent`。
  - `geom/`：`Point`/`Rectangle`/`Matrix`/`Transform`。
- 每个类的方法绑定到 AVM2 native method 调度（对齐 Lightspark 在 `flashdisplay.cpp` 的做法）。
- **横向参考 OpenFL**：OpenFL 是用 Haxe 对 `flash.*` API 的干净重实现，可作为“Flash API 表面
  长什么样、方法语义应如何”的权威对照（尤其 `Graphics`/`TextField`/`MovieClip`）。
- **优先项**（以 swf2.swf 实测调用为准）：`Graphics` 绘制 API、`MovieClip` 时间轴方法、
  `TextField`、`Sprite`。

### 3.3.5 AVM2 完善
- 补齐 `Avm2Opcodes` 覆盖度；将 native 方法（如 `MovieClip.gotoAndPlay`）对接到 3.3.4 的 C++ 实现。
- **对照 Tamarin**：Adobe 开源的 AVM2 参考虚拟机（C++、带 JIT，约 13.5 万行，Flash Player 9 所用），
  是 ABC opcode 与 AS3 对象模型语义的“事实来源”，可用于校验 `Fvm.cpp` 的 opcode 实现是否准确。
- 保证 `addFrameScript`/`enterFrame` 驱动链正确（已验证可触发，需保证语义正确）。

### 3.3.6 渲染与后端
- 短期：nanovg 维持，补矢量填充/描边/文本/基础滤镜的保真。
- 中期（可选）：抽象 `RenderBackend`（参考 Ruffle），便于接入硬件加速后端。
- 音频：`FFmpeg` 解码保持不变；输入：`player_win.cpp` 接入事件系统。

## 3.4 落地路线图（分阶段，每阶段可验证）

| 阶段 | 目标 | 关键改动 | 验证样本 |
|---|---|---|---|
| **Phase 0** | 统一构建 | Windows VS2017 / Linux Ninja（已完成） | `swf2.swf` 能构建运行 |
| **Phase 1** | 显示对象树 | `DisplayItem` → `DisplayObject` 继承树 + 双列表 + parent/children | 显示列表结构正确 |
| **Phase 2** | 帧/事件循环 | `tick`+`frame_accumulator`+5 阶段 + `ENTER_FRAME` 递归派发 + 冒泡 | 前数百帧不再静止 |
| **Phase 3** | 高频原生类 | `Graphics`/`MovieClip`/`TextField`/`Sprite` C++ 实现 | swf2.swf 画面明显改善 |
| **Phase 4** | AVM2 覆盖 + 渲染保真 | opcode 补齐、native 绑定、nanovg 矢量/文本 | 行为更接近官方 |
| **Phase 5** | 启动/加载时序 | 治 `Player.cpp:18` 的预加载模拟 | 与官方一致的首屏 |

## 3.5 验证策略

- 以 `swf2.swf` 作为回归样本（AS3 游戏，能暴露显示对象/事件/原生类全链路）。
- 沿用 `SWF_AS_DEBUG` 诊断（AVM2 执行标记：`addFrameScript`/`runFrameScript`/`enterFrame`/
  `instantiate`），量化每阶段是否被正确驱动。
- 逐阶段对比官方 Flash Player 输出（渲染帧 / 运行时 trace），定位剩余差距。

---

## 4. 一句话总结

开源项目证明：要让“按代码执行”得出和官方一样的结果，**必须重建代码所依赖的运行时平台**——
显示对象树、分阶段帧/事件循环、原生类语义、渲染保真。这四块都不在 SWF 文件规范里。
本方案据此把 `swfparser` 从“能解析+部分执行 AVM2”演进为“分层完整、显示对象稳定、
帧循环规范、原生类齐备”的运行时，并以 `swf2.swf` 为持续验证样本分阶段落地。
