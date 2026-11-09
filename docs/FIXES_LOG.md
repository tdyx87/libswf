# 修复记录（FIXES LOG）

> 目的：**每一处已修复的问题都记录在此**——问题、根因、修复落点、验证方式、回归防护。
> 后续任何人（包括 AI 助手）修改代码前应先读本文件：不要把已修复的行为改回去，
> 不要重复做已完成的工作；改动后必须重跑"回归验证"一节列出的测试。
>
> 条目编号规则：`FIX-<年>-<序号>` 代码修复；`DOC-<年>-<序号>` 文档纠错；`NOTE-<年>-<序号>` 工程坑。

---

## FIX-2026-001：AVM2 `Graphics` 绘制 API 整组 no-op（AVM2 动态绘制内容完全不可见）

- **日期**：2026-11-07
- **状态**：已修复，已验证（21/21 测试通过）
- **严重度**：高（画面缺失的头号直接原因）

### 问题
`src/Fvm.cpp` 的 `Vm::registerFlashApi()` 中，`Graphics` 类的全部方法
（`beginFill` / `endFill` / `drawRect` / `drawCircle` / `lineStyle` / `moveTo` / `lineTo` / `clear`）
都是 `noop` 空实现。AVM2 游戏经 `sprite.graphics` 动态绘制的一切内容
（UI、粒子、进度条、矢量图形）**画不出来**。

### 根因
实现只搭了方法签名占位，没有绘制命令的数据通道，也没有渲染端的回放逻辑。

### 修复落点
| 文件 | 内容 |
|---|---|
| `include/swf/Fvm.hpp` | 新增 `fvm::GraphicsCmd`（绘制命令结构：`BeginFill/EndFill/LineStyle/MoveTo/LineTo/CurveTo/Rect/RoundRect/Circle/Ellipse`，颜色统一存 ARGB）；`DisplayObject::graphics`（命令列表）；`Object::gfxTarget`（Graphics 实例绑定宿主显示对象） |
| `src/Fvm.cpp` | ① `graphics` getter：按宿主**缓存同一实例**（Flash 语义）并设置 `gfxTarget`；② `Graphics` 全部方法改为真实现——把调用按序记录为 `GraphicsCmd` 追加到宿主 `DisplayObject::graphics`；`beginGradientFill` 最小近似取第一个色停做纯色填充；`clear()` 清空命令列表 |
| `src/Render.cpp` | 新增 `renderGraphicsInto()`：回放命令为**非零环绕多边形填充**（复用 `fillEdgesWinding`，子路径方向自然表达并集/洞）+ 按段垂直四边形描边；局部像素先乘 20 转 twips 再进世界矩阵；在 `renderDisplayObjectImpl` 中挂钩（绘制于角色内容之后、运行时文本与子对象之前）；新增公开入口 `renderRuntimeObject()` |
| `include/swf/Render.hpp` | 导出 `renderRuntimeObject()` 声明（测试/内嵌渲染用） |
| `tests/test_graphics.cpp` | 新增回归测试（双端验证，见下） |
| `CMakeLists.txt` | 注册 `test_graphics` 目标 + `swfparser_graphics` 测试（已并入全量测试列表） |

### 关键语义约定（改代码前必读）
1. `beginFill(color, alpha)` 的 `color` 是 **0xRRGGBB**，命令内转为 **ARGB**（高字节 alpha）。
2. `drawRect/drawCircle/...` 只是**追加子路径**到当前填充组，`endFill` 才提交填充——
   没有 `beginFill` 就 `drawCircle` 不会产生填充（Flash 语义，测试已覆盖）。
3. `clear()` 直接清空命令列表（VM 端），渲染端不会看到 `Clear` 命令。
4. `lineStyle` 厚度 ≤0 或 NaN（省略参数）= 关闭描边；描边按段生成垂直四边形，
   **无 join/cap 处理**（最小可用实现，见"已知限制"）。
5. 未调用 `endFill` 就结束命令流时，渲染端**隐式提交**当前填充组（对齐 Flash 行为）。

### 验证（已执行，全部通过）
`tests/test_graphics.cpp` 共 14 项断言，两端验证：
- **VM 端**：`constructObject("Sprite")` → `getProperty("graphics")` 返回实例且重复访问为同一实例；
  `beginFill/drawRect/endFill/lineStyle/drawCircle/clear` 命令按序、按值记录（含 ARGB 颜色换算）。
- **渲染端**：矩形填充中心像素为纯红、外部透明；圆形填充圆心为纯绿、圆外透明。

### 回归防护
```bash
# 单测
build\Release\test_graphics.exe
# 全量回归（21 项，含真实样本端到端 swfparser_real_swf）
cd build && ctest -C Release --output-on-failure
```
**修改 `Fvm.cpp` 的 `registerFlashApi`、`Render.cpp` 的运行时渲染路径后，必须重跑上述测试。**

### 已知限制（后续待办，勿误认为已完成）
- `beginBitmapFill` / `lineGradientStyle` 为无害 no-op；`beginGradientFill` 仅取第一个色停。
- 描边无 join/cap；`Graphics` 几何**未接入命中测试**（`runtimeSelfHit` 不含 graphics 包围盒），
  纯 Graphics 绘制的按钮还点不到——需要时按 `runtimeBoundsAccum` 思路补。
- `drawPath` / `drawTriangles` / `copyFrom` 等高级 API 未实现。

---

## DOC-2026-001~003：架构对比文档"伪短板"纠错（防止再次误判）

三处此前写进文档的"短板"经整体架构实证**均为误判**，已在
`docs/COMPARISON.md`、`docs/PROJECT_ANALYSIS.md` 更正。**不得再按旧结论安排工作**：

| 编号 | 原误判 | 实证结论（代码依据） |
|---|---|---|
| DOC-2026-001 | "缺全局 5 阶段播放循环；`Player.cpp:18` 自承前数百帧静止" | 那段注释描述的是**已删除的旧路径**（原 `SWF_STARTUP_SIM=1` 启用，已由 FIX-2026-028 彻底移除）；`Player::tick` 按每个 MovieClip 独立时间轴驱动，加载页→门户页→游戏由 SWF 自身 AS3 驱动、无需播放器伪造时序。**不是差距** |
| DOC-2026-002 | "事件模型捕获阶段待核" | `Vm::dispatchEvent`（`Fvm.cpp:4104-4167`）已实现**完整三段事件流**（捕获→目标→冒泡）+ stopPropagation/preventDefault/优先级/useCapture。**不是差距** |
| DOC-2026-003 | "原生类深度可能不全（泛泛）" | `MovieClip` 时间轴方法、显示列表操作、`DisplayObject` 变换属性（setter `Fvm.cpp:2173-2190`、getter `4427-4489`）、`EventDispatcher`、`Timer` 均已实质实现；确认的 no-op 只有 `Graphics`（已由 FIX-2026-001 修复）。剩余缺口：`TextField` 排版、未注册类（`BitmapData`/`Point`/`Rectangle`/`ColorTransform`/`Loader` 等） |

> 教训：**下"缺口"结论前必须先读对应实现的函数体**，不能拿开源对照表外推。

## NOTE-2026-001~002：本机工程坑（避免重复踩）

- **NOTE-2026-001**：`Fvm.cpp` 体量大，MSVC 2017 并行编译偶发 `C1060 编译器堆空间不足`。
  处理：单进程重试 `cmake --build build --config Release -- /m:1`，即可通过（非代码问题）。
- **NOTE-2026-002**：cmd 中 `命令 > log 2>&1 & echo %ERRORLEVEL%` 的 `%ERRORLEVEL%`
  在**整行解析时**就被展开，显示的是旧值（会误报成功）。判成败要看日志文件或用
  `& call echo %%ERRORLEVEL%%`。

## FIX-2026-002：`tools/fp_capture.py` 官方播放器点击驱动失效 + 抓帧安全防护

- **日期**：2026-11-07
- **状态**：已修复（点击驱动 + 前台防护），自动化会话内的官方抓帧仍有环境限制（见限制）

### 问题与根因
1. 旧实现用 `PostMessage(WM_LBUTTONDOWN/UP)` 向官方播放器窗口发合成消息——
   **官方播放器忽略合成消息**，点击全部无效（实测停留在门户页 20+ 秒不前进）。
2. 改用真实全局输入（`SetCursorPos`+`mouse_event`）后，若播放器窗口**不在前台**，
   点击/截屏会命中当时的前台窗口（实测误点并误截了其它应用窗口）——自动化会话中危险。

### 修复落点（`tools/fp_capture.py`）
- 点击改为真实全局输入（官方播放器只认真实输入）。
- 新增 `ensure_fg()` 前台防护：每次点击/截图前确认官方播放器窗口在前台
  （`GetForegroundWindow` 校验 + 尝试 `SetForegroundWindow`），不在前台则**跳过并打印警告**，
  杜绝误点/误截其它窗口。

### 使用限制（重要）
- 在无人值守的自动化会话中，`SetForegroundWindow` 常被 Windows 焦点策略拦截 →
  点击/截图会被防护跳过。**官方参照帧的抓取需要人工在前台运行本脚本**
  （历史参照帧即由此方式采集），或让脚本所在会话拥有前台权限。
- 联网环境下官方播放器会显示 YOKO 网络门户页（需点其 Play 按钮才进游戏）；
  本项目无头播放为离线回退（loading→PLAY 按钮）。两边流程基线不同，见 NOTE-2026-004。

## NOTE-2026-007：官方流程全图 + "场景不停等鼠标"根因方向（TOP TODO，2026-11-07）

- **官方实测全流程**（`ref_burst\` 400 帧 10fps 连拍 + `gif\swf2_official_full.gif`）：
  1. **preloader**（粉底 % 进度条，t=0）→ 2. **YOKO 门户页**（云朵飞入动画 ~8s，Play 按钮在
     stage ≈(581,540)，t≈10s 出现）→ 3. 点击 Play → **标题屏 "Baby care spa salon"**
     （logo 飞入，PLAY/MORE GAMES 出现后**停下等点击**）→ 4. 游戏（每场景等鼠标）。
  - 官方 20 秒无点击仍停在门户/标题屏——**每个环节都真的停下等鼠标**。
- **我方实测流程**（`rec\` 620 帧）：
  蓝星标题底（**无 logo**，<0.4s）→ "loading..."转圈（tick 15！）→ loading+PLAY（等待✓）→
  洗澡关（等待✓，仅眨眼 idle）→ SHOW 点击 → 结算（等待✓）。
  **门户页与标题屏 logo/PLAY 被整体跳过**；标题屏闪现即逝。
- **AS3 是否在跑？在跑**（实证，回应用户"看起来没处理as3"）：
  - trace 输出：`init`/`nextbtn`（PLAY 点击处理器）/`LV0-3`+MovieClip 枚举（SHOW 处理器）；
  - `SWF_AS_DEBUG=1` 显示 30+ 个 `Baby_Care_Spa_Salon_fla.*` 类全部正确 `addFrameScript` 注册，
    `runFrameScript frame=0/6` 也在执行；
  - 场景切换确实由点击驱动（470→520 无点击画面静止）。
- **根因方向（两个强嫌疑，下次会话先查）**：
  1. **gotoAndPlay 链被同步递归执行**：主控 `元件137_112`（帧脚本 6/7）在 40 步内就冲到
     frame=6（洗澡关），而官方每 tick 只走一帧——帧脚本里 goto 立即重入导致一次 tick
     连跳多帧，开场 intro（`intro_17`，脚本在 0/7/**221** 帧，共 222+ 帧 ≈9.2s 的门户+标题
     动画）被瞬间跳完。旁证：step 日志主时间轴 frame=2↔1 每 tick 循环（官方应 stop）。
  2. `intro_17` 的 frame=7/221 脚本**从未执行**（日志无对应 runFrameScript）——intro 子 MC
     停在首帧不前进（hold-frame 逻辑 vs playing 标志的交互？），门户/标题动画与"等点击"
     逻辑根本没机会运行。
- **诊断入口**（已验证可用）：
  `set SWF_AS_DEBUG=1 && swfparser swf2.swf --play-game 40 <dir> --no-render > log 2>&1`，
  看 `[as] addFrameScript`/`[as] runFrameScript` 配对情况；`SWF_AS_DUMP_TREE=1` 看对象帧号。
- **产物**：`build\compare\gif\swf2_official_full.gif`（官方 40s 全流程）、
  `swf2_swfplayer_full.gif`（我方）、`cmp_player_vs_headless.gif`（双路径一致性）。

## FIX-2026-004：根时间轴真播放——preloader 屏已由 SWF 数据驱动复现（2026-11-07）

- **结论先行**：按"先保证解析数据正确、再补 AS3"的路线，实现了根时间轴播放（`SWF_ROOT_TIMELINE=1`
  实验开关），**官方 preloader 屏（YOKO logo+头像+进度条+LOADING）已完整由 SWF 数据渲染出来**，
  与官方 flashplayer 首屏一致（`build\compare\rec_rt2\`）。
- **根因链**（修正 NOTE-2026-007 的两个嫌疑）：
  1. **VM 根时间轴从不播放**：`Vm::init` 用"最后一个含具名实例的帧"一次性建根（为让文档类
     构造时拿到 `main`），根第 1 帧的 preloader/门户内容被整体跳过；`advanceTimeline` 又要求
     `characterId!=0`，根（cid=0）永不推进。门户/标题并非被"跳帧冲过"，而是**从未上树**。
  2. **LoaderInfo 0/0**：`bytesLoaded/bytesTotal` 硬编码 0/0，且 `DisplayObject/Stage` 根本没有
     `loaderInfo` getter——依赖加载进度的 AS3 流程逻辑全部失效。
- **修复落点**：
  - `advanceTimeline`：允许 stage 根（parent==null）在 `SWF_ROOT_TIMELINE=1` 时按 `movie_.frames`
    推进（复用 `applyTimelineFrame`，其本就支持 characterId==0）；`stop()/play()` 原生对根生效。
  - `Vm::init`：`SWF_ROOT_TIMELINE=1` 时根从第 0 帧建树（默认路径不变，保历史兼容）。
  - `DisplayObject.loaderInfo` / `Stage.loaderInfo` getter：本地整片加载语义
    （bytesLoaded==bytesTotal=100）；`LoaderInfo` 构造器同步改为完整加载。
  - `Vm::fireLoaderEvents`：惰性派发 init/progress/complete（loaderInfo 一旦有监听器即补发，
    `ProgressEvent` 携带 bytes 值）。
- **验证**：全量 21/21 通过；`SWF_ROOT_TIMELINE=1` 下 swfplayer 录制显示真实 preloader 屏；
  树导出显示 `preloading_1`（cid=387，Bar/Loader_txt/Preloader_mclip 头像动画在推进）。
- **遗留（下一步，按序）**：
  1. **进度文本停在 0%**：preloader 从未访问 loaderInfo（getter 日志证明），% 推进另有机制
     （嫌疑：TweenLite 补间动画 Bar + Loader_txt，或 Timer，或 Preloader_mclip 循环帧脚本——
     `Preloader_mclip_09_7` 的 frame-19 脚本在每 tick 循环触发）。需追 preloading_1 frame-0
     脚本之后的推进路径。
  2. preloader 完成后 → 门户 intro（preloading 容器第 2 帧，222 帧动画）→ Play 点击 →
     根 gotoAndStop(2) 进游戏：整条链待打通验证。
  3. 打通后将 `SWF_ROOT_TIMELINE` 设为默认行为，移除/降级 `SWF_STARTUP_SIM` 假模拟
     （Player.cpp 634-651 的硬编码加载/门户计时器）。
     → **已由 FIX-2026-006 / FIX-2026-028 完成**：根时间轴语义转正为默认
       （`SWF_LEGACY_ROOT_FRAME` A/B 开关已随 FIX-2026-028 删除）；
       假启动模拟子系统已整体删除，首页由 SWF 自身 AS3 首帧脚本驱动。
  4. 回归确认：`--play-game` 全流程、swfplayer 窗口播放、GIF 对比全链路。
     → **已由 FIX-2026-006 完成**：默认无变量 `--play-game 345 --click 310:593:574`
       全链通过 + ctest 21/21；窗口播放/GIF 对比仍未重跑。
- **诊断工具沉淀**：`runFrameScript` 调试行含 owner 类名；`dispatch` 调试行含监听者与回调闭包名；
  `loaderInfo` getter 有访问日志（当前证明 preloader 不走此路径）。

## FIX-2026-005：按 SWF 数据 + AS3 字节码逐层解码 preloader 流程，打通 4 个系统性缺口（2026-11-08）

- **方法**（回应用户"先分析代码和 swf 数据，按开源库经验开发，先别对比官方"）：
  不再对照官方抓帧，改为**从 dump 到字节码的正向分析**：
  1. dump 增强（`Parser.cpp`）：PlaceObject2 浅解析（flags/depth/cid/name）、RemoveObject2、
     DefineSprite(id/frames) —— 主时间轴数据图由此建立；
  2. `SWF_AS_OPS` 升级：traceOps 附带 multiname/字符串操作数标签（方法名为空的帧脚本闭包
     变得可读）；支持按类名匹配（thisObj.className/cls.name）；事件派发路径也挂
     g_traceOpsActive 钩子（监听闭包类名匹配即跟踪）。
- **SWF 数据图（swf2.swf，根 2 帧）**：
  - 帧1: place depth=1 **cid=387 `preloading_1`**（3 帧 sprite = 启动序列容器）+
    depth=2244 cid=388（纯背景 Shape）+ SymbolClass(37)；
  - 帧2: **remove depth=1**（移除 preloader 容器）+ place depth=1 **cid=1280 `main`**（8 帧，
    游戏主控，帧脚本在 6/7）+ cid=1310 yokoGames + SymbolClass(33)；
  - `intro_17` = cid=386（223 帧，门户+标题动画，由 preloading 容器放置）；
  - `GameMain` = 文档类（char 0），构造即 `stop()`（GameMain.stop 实测调用）。
- **preloading_1 帧-0 脚本全文解码**（ops 跟踪）：`stop(); MovieClip(parent).stop();
  Bar.stop(); service_error_mc.visible=false; Service.init(secureKey,'1017',
  'Fairy Spa Salon & Makeover',...)` + peso/carga/porcentaje/delayCount/loadCount/delayper
  初始化 + btn_link mouseDown。**进度为假进度，由 Service 响应后的 `progreso` enterFrame 驱动**。
- **打通的 4 个系统性缺口**（每个都由字节码证据定位）：
  1. **`Security` 类缺失** → `Service.init` 里 `getlex [Security]` 得 undefined，
     `iffalse` 走错分支**跳过 pc=199..439 的整个 URLLoader 初始化块**。修复：补
     `Security` 内建类（sandboxType="application"（projector 本地语义）、allowDomain/allowInsecureDomain/
     loadPolicyFile no-op）。
  2. **原生父类链接断链**：ABC 类父类是限定名（`flash.display.MovieClip`），原生类只注册了
     短名 → `buildClass` 链接失败 → **继承链断在第一个用户类上**，原生 getter/方法对全部
     fla 子类不可见。修复：限定名查不到时按短名回退（Fvm.cpp 1951 区）。
  3. **`DisplayObject.root` 缺失**：progreso 用 `MovieClip(root).loaderInfo.bytesTotal`，
     无 root getter → 整链 null。修复：补 root getter（沿 parent 走到根，objectForDisplay 包装）。
  4. **IOErrorEvent.IO_ERROR / SecurityErrorEvent.SECURITY_ERROR 常量缺失** → 监听键为
     undefined。修复：补静态常量（ioError/securityError）。
- **验证（数据驱动流程实证）**：Security 修复后 init 完整走通（URLLoader 创建、COMPLETE/
  IO_ERROR/SECURITY_ERROR 监听、`loader.load(new URLRequest('key=...&id=...&rand=...'))`）→
  我方 URLLoader 同步 complete → `Service.onComplete` 执行 → **`dispatchEvent(SERVICE_READY)`
  实际发生** → **`preloading_1.onServiceReady` 触发** → 注册 `progreso` enterFrame +
  btn_link mouseDown → **进度条 0→99 平滑爬升（Bar.gotoAndStop 0,1,3,...,99 实测）**。
  全量 21/21 通过。
- **遗留（下一步，按序）——已由 FIX-2026-006 全部结案**：
  1. ~~**99%→100% 门槛**：porcentaje = floor(carga*99/peso) 上限即 99~~
     **此推断有误**：`porcentaje` 由 `delayper=floor(loadCount*100/delayCount)` 驱动到 100，
     >=100 分支（pc=155-200）才是门户切换，详见 FIX-2026-006「纠正一个此前的误判」。
  2. 100% → 门户 → intro_17 → Play 点击 → 根进游戏：**已打通并渲染验证**。
     Play 点击此前失效的根因是事件派发覆盖了根 display 的 AS3 owner（FIX-2026-006）。
  3. SWF_ROOT_TIMELINE 转正：**已完成**（默认行为）；假启动模拟默认下线，保留 A/B 开关。
- **新诊断工具**：`SWF_AS_OPS=<子串>` 现在同时匹配 方法名/类名/帧脚本闭包类名（事件派发
  路径也生效），trace 行附带 [multiname] 与 'string' 标签——读 AS3 逻辑的基本工具。

## NOTE-2026-003：`swfparser.exe` 无法向 flashplayer 根目录写文件

- **现象**：`--render`/`--play-game` 输出指到 `f:\project\product\flashplayer\` 根目录时，
  PNG 全部静默写入失败（`stbi_write_png` 返回 0、目录创建失败），但同一目录下
  cmd 重定向日志可以创建——**是本机安全策略对新构建未签名 exe 的写限制，非代码回归**。
- **对策**：所有渲染/对比产物统一放 `swfparser\build\compare\`（已验证可写）。

## NOTE-2026-004：官方/我方流程基线已脱节（对比时间表待重定）

- **现状**（2026-11-07 实测，`build\compare\` 下有对比图佐证）：
  - 官方播放器（联网）：YOKO 开屏 → **网络门户页**（大 YOKO logo + Play/More games，
    需点击门户 Play 才进游戏）；`DEFAULT_SCHEDULE` 的坐标不点门户 Play → 官方全程停在门户页。
  - 我方无头播放（离线回退）：loading → **PLAY 按钮**（有效点击点 **stage (654,449)**，
    见 NOTE-2026-005 的诊断；其下 `nextbtn` 命中盒 dev (575.7,374.4)-(731.0,529.8)）→
    游戏全流程可走通（620 步回放到达结算画面，bath/show/end 场景渲染完整）。
  - 旧 `DEFAULT_SCHEDULE`（click100:478:700 等）对两边都已失效：我方 PLAY 屏挡到 step 460，
    之后流程被压缩。
- **待办**：重定对比时间表——我方加 `--click 220:650:430`（PLAY，用 stage 坐标）；官方加
  门户 Play 点击（需人工前台会话）。重定前，与官方的可比对象是"场景级"对比
  （同屏名各取一侧对应帧），不要按帧号硬对齐。

## FIX-2026-003：swfplayer 程序化录制 + GIF 对比动画生成（2026-11-07）

- **需求**：帧级对比不够，要"能直接看的动画对比"；swfplayer 也要能自动录屏。
- **落点 1（`src/player_win.cpp`）**：swfplayer 新增
  - `--record <dir>`：每 tick 把**舞台原生尺寸**渲染写 `dir\rec_NNNN.png`（复用 swf_render
    的 stb PNG 编码；写失败即停录并打日志）；
  - `--click F:X:Y`：第 F 次 tick 注入舞台坐标左键（按下 2 帧后释放，与 swfparser 一致；
    时基用单调 tick 计数，不随掉帧对齐归零）；
  - `--max-frames N` / `--auto-exit`：录满即停 / 录满自动关窗（无人值守）。
- **落点 2（`tools/make_gif.py`，新增）**：PNG 帧序 → GIF（`--fps/--scale/--start/--count`）；
  `--stack dirA dirB out.gif` 生成**上下堆叠对比 GIF**（带 OURS/REF 标签）。
- **验证**：`swf2.swf` 620 帧录制成功（`build\compare\rec\`）；产物
  `build\compare\gif\swf2_swfplayer_full.gif`（15MB）与
  `cmp_player_vs_headless.gif`（窗口播放器 vs 无头回放堆叠，18MB）。

## NOTE-2026-005：诊断方法论坑——截图坐标 ≠ 舞台坐标（差点误判命中 bug）

- **现象**：播放 PLAY 屏时点"视觉按钮中心"无效、点右下一点才生效，疑似命中偏移 bug。
- **根因**：`read_file` 显示 PNG 时按 1080x675 **缩放展示**，而 `--click` 用**舞台原生
  1280x768** 坐标——在缩放图上量的坐标整体缩小 0.84 倍，"视觉中心 (550,385)"实际是
  stage (652,456)，与按钮完全吻合。**命中测试没有 bug**。
- **正确诊断路径**（本次实证有效，复用即可）：
  1. `SWF_AS_DEBUG=1 swfparser --play-game --no-render --click F:X:Y` →
     `[mouse] mouseDown (x,y) target=... chain=...` 直接给出命中目标与链条；
  2. `SWF_AS_DUMP_TREE=1 SWF_AS_DUMP_MS=<毫秒>` → `[tree]` 全树导出（含 world 坐标、
     形状 bounds dev 盒、按钮 btnHit 命中盒），定位对象世界位置。
- **实证结论**：PLAY 按钮 = `main > loading_mc(cid=1269) > nextbtn(cid=1268)`，
  world (654.1,448.9)，命中盒 dev (575.7,374.4)-(731.0,529.8)；点 (550,385) 命中的是
  背景网格圆点（cid=1245）——行为正确。

## NOTE-2026-006：swf2.swf 性能——"播放不对"观感的真正根因（待优化，ROI 最高）

- **实测**（swfplayer `--log` 心跳，swf2.swf 1280x768）：
  `tickUs/frame=205317 renderUs/frame=58337` → **tick 205ms + 渲染 58ms ≈ 263ms/帧 ≈ 3.8fps**，
  目标 24fps（41.7ms）——**慢约 6.3 倍**，动画卡成幻灯片，这就是"swfplayer 播放不对"的主观感受来源。
- **画面本身无错**：录制的 620 帧逐段目检——开屏/loading/PLAY/游戏/结算全部渲染正确
  （含 Toys 面板、按钮、字效），且与无头 `--play-game` 输出一致（堆叠 GIF 可证）。
- **优化方向**（按 suspected 成本排序，待做）：① AVM2 解释器每帧脚本开销（display list
  每帧重建 190+ 子对象）；② 全画布软件光栅无脏区/无缓存；③ 录制/窗口双份渲染可合一。
  **注意**：优化时勿破坏既有几何/事件语义，改完必须跑全量 21 项 ctest + 重新录制 GIF 对比。

---

## FIX-2026-006：门户 Play 按钮无法进入游戏——事件派发覆盖根 display 的 AS3 owner（2026-11-08）

- **状态**：已修复，已验证（21/21 测试通过；默认无环境变量全流程回放通过）
- **严重度**：高（preloader → 门户 → 游戏 全链最后一环断裂）

### 问题
`swf2.swf` 进度条走完、YOKO 门户与 Play 按钮都正常出现，点击 Play 后画面停在门户，
根时间轴始终 `frame=0 playing=0`，游戏标题屏（Baby Care spa salon）不出现。

### 纠正一个此前的误判（重要，别再走回头路）
曾判断「progreso 的 `porcentaje` 上限 99，永不进入门户分支」，并临时把两处
`loaderInfo.bytesLoaded` 改成 99 试图强制走 `<100` 分支——**方向是反的，已全部回退**。
按字节码实证：

- `porcentaje = max(floor(carga*100/peso), floor(loadCount*100/delayCount))`，
  `frame1` 初始化 `delayCount=48 / loadCount=0`；`delayper` 随 ENTER_FRAME 逐帧爬升，
  约 step 59 达到 100（`Bar.gotoAndStop` 实测 0,1,3,5,…,99,100）。
- `pc=154 op=0x0F` 是 **ifnge**：`if (!(porcentaje >= 100)) jump pc=201`。
  `pc=201` 只是 `loadCount++` 后返回；**`pc=155-200`（>=100 分支）才是门户切换**：
  `removeEventListener(ENTER_FRAME, progreso)` + `this.play()` → preloader 播到自身 frame 2
  → `doLaterLL`(578) → `initLL`(577)。所以 `bytesLoaded=99` 只会拖慢 `delayper`，毫无帮助。

### 真正根因
`Vm::dispatchEvent()` 构造冒泡链时，把显示树最外层 `top` 的包装无条件写成
`top->owner = st`（`st` 是 `ObjKind::Stage` 的 "Stage" 对象）。ENTER_FRAME / click 事件
每帧派发，文档类实例 `GameMain`（MovieClip 子类）作为根 display 的 owner 被顶掉。
于是 `root` 解析成 Stage 包装对象，其类链里**没有** `play/gotoAndStop` 等时间轴方法
（`Stage` 类只注册了 stageWidth/stageHeight/frameRate/loaderInfo），
`playGo`(552) / `iniciarJuego`(576) 的 `MovieClip(root).play()` 经 `callpropvoid`
静默失败（接收者非 `MovieClip.play` 原生、`[miss]` 也是 0，极易漏诊），
根的 `playing` 永远是 0，`advanceTimeline` 不推进根帧。

### 修复落点
| 文件 | 内容 |
|---|---|
| `src/Fvm.cpp` `Vm::dispatchEvent()` | 仅当 `!top->owner.lock()` 时才让 Stage 包装接管 owner；Stage 仍加入冒泡 targets，事件语义不变 |
| `src/Fvm.cpp` `DisplayObject.root` getter | 明确返回 `stage` 中 `characterId==0` 根的 AS3 实例（文档类），保留原"向上走到顶 + objectForDisplay"兜底 |
| `src/Fvm.cpp` 根时间轴语义 | `SWF_ROOT_TIMELINE` **转正**：默认从第 1 帧（索引 0）建根、默认允许根逐帧推进；当时的 `legacyRootFrame()` / `SWF_LEGACY_ROOT_FRAME=1` A/B 开关已在 FIX-2026-028 中删除，并补充根首帧持有语义 |
| `src/Fvm.cpp` 两处 `loaderInfo` getter | 回退 TEMP-DIAG，`bytesLoaded` 恢复 100（整片本地加载语义） |

### 关键语义约定（改代码前必读）
1. **`root` = 文档类实例（MovieClip 子类），`stage` = Stage 单例**，二者不是同一个对象；
   任何事件/树构建路径都不得覆盖根 display 已有的 AS3 owner，否则 `root.play()` 失效。
2. `progreso` 的 **`porcentaje >= 100` 分支**才是门户切换分支；`<100` 分支只累加 `loadCount`。
3. `play()/stop()` 原生直接改 `self->display->playing`；根（`cid=0`）同样按 `movie_.frames`
   推进，并且**必须首帧持有**以执行 `addFrameScript(0, ...)`（见 FIX-2026-028）。
4. **`Player.cpp` 已删除整套假启动模拟**（FIX-2026-028）：无 `SWF_STARTUP_SIM`、
   无影片副本/冻结 Vm/伪造进度/门户计时器。播放器只负责读资源、按时间轴播放、
   执行 AVM1/AVM2 脚本、输入与音视频输出；本条为当前有效约束，下面各历史条目中
   "保留 `SWF_STARTUP_SIM=1` 作 A/B" 的描述均已作废。

### 验证（已执行，全部通过）
```bash
cmake --build build --config Release -- /m:1
ctest --test-dir build -C Release --output-on-failure     # 21/21
# 不带任何环境变量的默认全流程（关键：转正生效）
swfparser.exe swf2.swf --play-game 345 build\compare\framesDefault --click 310:593:574
```
- 点击命中 `btn_play` → `iniciarJuego`(576) → `playGo`(552) →
  `MovieClip.play self=GameMain display=Y cid=0` → 根帧 `stop` →
  `[gotoStop] name=main cid=1280 ... keys=6,7`（`main` 精灵帧脚本执行）。
- 渲染关键帧（产物在 `build\compare\`）：
  `framesDefault/p_0030.png` 进度条 0%；`p_0280.png` 门户 Play / More games；
  `p_0344.png` **游戏标题屏 Baby Care spa salon**。
- 对照组 `framesWin/`（`SWF_ROOT_TIMELINE=1`）与默认组画面一致。

---

## NOTE-2026-008：点击 Play 的"下一步"分两段——第一段已通、第二段尚未打通（2026-11-08）

- **状态**：测试完成，第二段当时**未修复**；→ **已由 FIX-2026-009 修复**。
  下方保留当时的排查记录与"帧脚本/事件总线没触发"的表象判断，勿再按该方向重复排查。
- **测试命令**（默认无环境变量）：
  `swfparser.exe swf2.swf --play-game 400 build\compare\framesFinal --click 310:593:574 --click 390:473:698`

### 第一段（门户的 Play）：已通 ✓
`--click 310:593:574` 命中 `btn_play` → `iniciarJuego`(576) → `playGo`(552)
→ `MovieClip(root).play()` 作用于 `GameMain cid=0` → 根进 frame 2
→ `main`(cid=1280) 帧脚本 → 游戏标题屏 `Baby Care spa salon`（见 FIX-2026-006）。

### 第二段（标题屏的 PLAY）：命中但无响应 ✗
画面约 step 380 起稳定显示 `PLAY` / `MORE GAMES`。`--click 390:473:698`
命中 `btnPlay cid=576 depth=771`（父链 `stepMC cid=577` → `main cid=1280`）。

- VM 侧事件链是**通的**：`mouseDown/mouseUp` 各 1 个 listener，
  `OnMouseDown`(103) / `OnMouseMove`(105) / `OnMouseUp`(104) 均执行。
- 但 `click` 是 `listeners=0`，业务侧依赖 `mouseUp` 的 `OnMouseUp`。
- **AS3 字节码判据**（新增 `SWF_AS_DIS=104` 静态反汇编）：
  ```
  local2 = this.m_fun[MouseEvent.MOUSE_UP]   // pc=6..19
  if (local2 == null) return;                // pc=20..22 ifeq 72 → pc=98
  for (key in local2)
      if (m_enable || m_without.hasOwnProperty(key)) local2[key](event)  // pc=75..84
  ```
  实测 `MouseEvent.MOUSE_UP = "mouseUp"` 正确，但 `MouseManager.m_fun` 是**空对象**
  （`rkeys=[]`），所以 `pc=22` 直接跳 `returnvoid`，真正的回调分发从未执行。
- `MouseManager.Add`(mi=100，往 `m_fun[事件名]` 注册回调) 与 `getInstance`
  在整段 trace 里**从未被调用**；`Init`(mi=90) 构造已执行（初始化 `m_fun=new Object()`
  并向 `m_root` 注册 OnMouseDown/Up/Move/Over/Leave，故舞台上有监听）。
- 进一步：`main` 的帧脚本注册在 `frame=6/7`，但 `gotoAndStop(0)` 停在第 1 帧，
  从无 `runFrameScript` 记录；`stepMC`(元件87_114) 只跑过 `frame=57` 的 `stop()`。
  → **方向：标题屏相关的 AS3 初始化/注册代码没有执行**，不是点击坐标或事件派发问题。

### 新沉淀工具（长期可用）
`SWF_AS_DIS=<mi|方法名>[,...]`：`Vm::run()` 中用 `avm2::decode` 静态反汇编指定方法，
**不依赖该方法是否被执行**，可看到条件跳转后未进入的分支（本次就是靠它读出
`pc=25..97` 的回调分发逻辑）。`traceOps` / `SWF_AS_MI` 只能看到实际走过的路径。
`SWF_AS_FIND=<子串>`：静态列出所有引用该 multiname 的指令（方法 mi + pc + opcode），
用于回答"谁调用了某方法"。`SWF_AS_CLASS=<子串>` 现为子串匹配（原为精确等值）。

### 深挖：为什么"AS3 脚本没真正执行"（同日第二轮，逐层排除）
用静态反汇编 + 引用扫描 + 运行时调用记录交叉验证，结论是**两层问题**：

1. **执行了，但关键分支没进**：`OnMouseUp`(104) 确实运行，只是 `pc=22 ifeq`
   因 `m_fun["mouseUp"] == null` 跳到 `pc=98 returnvoid`，业务回调分发段
   `pc=25..97`（`local2[key](event)`）从未进入。
2. **初始化脚本整体没被触发**：`MouseManager.Add`(100，负责往 `m_fun[事件名]`
   写回调) 运行时零调用记录，静态上 `Add` 这个 multiname 在 ABC 里也零引用。
   定量：静态含 `addEventListener` 的方法 **75** 个，运行时总共只执行 **31** 个
   AS3 方法，其中只有 **10** 个属于注册监听的方法（`556/565/579/580/586/595/
   739/752/765/774`），**65 个从未执行**；标题屏类附近 `780/787/793/795/797/800`
   全部 `NOT-RUN`。点击后 `dispatch enterFrame listeners=0`、`click listeners=0`，
   除 `MouseManager.Init` 注册的舞台级鼠标监听外，运行时树没有任何 AS3 监听。

**已排除**（都有证据，勿重复排查）：点击坐标（命中 `btnPlay cid=576 depth=771`，
5 层冒泡链正确）／事件派发（`mouseUp listeners=1`）／`MOUSE_UP` 常量（`"mouseUp"`）／
`addFrameScript` 实现（支持多对参数、0 基索引，注册结果 `keys=6,7` 与 `frame=57` 正确）／
类构造（`元件137_112` iinit=627、`元件87_114` iinit=770 均完整执行，只注册帧脚本）／
`MouseManager` 单例（`getInstance` mi=339 只 `construct` 一次，后续都直接返回 `instance`）／
异常或未实现 opcode 中断（无记录）／按钮按名访问（`btnPlay` 在 ABC 里零引用，
业务代码按 `e.target.name` 用字符串常量分发，如 mi=788）。

**根因方向**：不是播放控制 API 坏了，而是**帧脚本/类初始化的触发链没满足**——
`main`(cid=1280) 构造注册了 `frame7`/`frame8`（帧索引 6/7），但运行时
`[gotoStop] name=main cid=1280 total=8 -> 0` 把它停在第 1 帧且 `playing=false`，
`runFrameScript` 里从无 main 记录；`stepMC`(cid=577) 只跑过 `frame=57` 的 `stop()`。
下一步要查 `main.gotoAndStop(0)` 的调用方，以及 Flash 语义下 main 应播放/停在哪一帧，
让承载按钮注册的帧脚本真正跑起来。**此环仍未修复。**

### 验证
全量构建 + ctest **21/21 通过**；产物 `build\compare\framesFinal\`、`framesNext\`
（460 步观察标题屏）、`disStatic_full.txt` / `dis_full.txt` 等诊断输出。

---

## FIX-2026-009：标题屏 PLAY 无响应——AS3 接口类型判定与原生类链缺口（2026-11-08）

- **状态**：已修复。默认无环境变量
  `swfparser.exe swf2.swf --play-game 460 <outdir> --click 310:593:574 --click 390:473:698`
  可走通 进度条 → 门户 Play → 标题屏 PLAY → 第一关 `steps.GameStep1`。
- **纠正 NOTE-2026-008 的方向**：「帧脚本/类初始化触发链没满足」只是表象。
  真正断点在 `GameMain.readyGame`(mi=969) 的 `this as IStepFactory`：
  接口转换被 VM 判为 null，关卡工厂整条链从源头就没有执行。

### 三处按 AS3 字节码定位的缺口

1. **`as IStepFactory` 判定失败**（`mi=969 pc=64..67`：`getlex [IStepFactory]` →
   `astypelate` → `setGameDate(..., this as IStepFactory)`）。
   `valueIsType` 只沿 `superClass` 链比较类名，而接口声明在
   `abc.instance_info.interfaces`，不在父类链上 → 转换结果 null
   → `GameMainController.m_stepFactory = null`。
   证据：`playGme`(mi=72) 的 `pc=72 callproperty [getClassNameByStep]` 无 `[call]` 记录，
   `pc=125 m_currentStep.play(m_main)` 同样没有；AS3 `trace` 打出
   `main:[object ...元件137_112] [object Object]`（关卡实例是空 Object），
   `SWF_AS_MI=384` 零条 opcode。
   修复：`ClassDef::interfaceNames` 记录 `instance_info.interfaces`，
   `valueIsType` 沿类链按接口短名匹配。修复后 trace 变为
   `main:[object ...元件137_112] [object com.wisp.webgame.utils.steps.HomeStep]`，
   并出现 `[call] HomeStep.play mi=384 body=374 argc=1`。

2. **`HomeStep.endScript`(mi=388) 的按钮注册分支被整段跳过**
   （`pc=57` / `pc=98` 两个 `iffalse` 直接跳到 `pc=94` / `pc=135`）。
   字节码：`m_stepMC.getChildByName("btnPlay") as InteractiveObject`，随后
   `pc=61..90` 注册 `addEventListener(MouseEvent.CLICK, __playGame)`，
   `pc=102..131` 注册 `__moreGame`。
   `InteractiveObject` / `DisplayObjectContainer` 两个原生类对象**从未注册**
   （`registerNativeClasses` 只有 `DisplayObject` / `Sprite` / `MovieClip` /
   `TextField`），`getlex [InteractiveObject]` 得到 undefined，`astypelate` 的
   type 为空 → `valueIsType` 直接 false → 两个 slot 为 null → CLICK 监听零注册。
   修复：补齐两个原生类对象，把 `Sprite` 的父类从 `DisplayObject` 改为
   `DisplayObjectContainer`（Flash 类链 Sprite → DisplayObjectContainer →
   InteractiveObject → DisplayObject → EventDispatcher），并让 `valueIsType`
   对没有类链的时间轴 / `getChildByName` 显示对象包装按 `ObjKind` 兜底。

3. **验证链路**：`mi=388` opcode 由 `0..57,94,98,135` 变为完整 `0..135`；
   点击时 `[mouse] click (473,698) target=btnPlay chain=5 listeners=1`；
   `invokeValue method=389 __playGame` → `BusEventListener.send(NEXT_GAME, 1)`
   → `__nextGame`(70) → `playGme`(72) → `getClassNameByStep(1)` → `steps.GameStep1`；
   AS3 trace 从 `gameStep 下一关卡是 : 0` 变为 `: 1`，
   `main:... [object steps.GameStep1]`。

### 验证
全量构建成功；ctest **21/21 通过**；默认无变量两次点击回放产物
`build\compare\framesFinalPlay\`（460 帧）；诊断输出 `trFinal_full.txt` /
`trFinal_stdout.txt`、`disHome_full.txt`、`disEnd_full.txt`、`mi968_full.txt` 等。

---

## 全量回归验证命令（每次代码改动后执行的完整工作流）

```bash
# 1) 全量构建：主程序（swfparser.exe / swfplayer.exe）+ 全部测试，一个都不能少
cmake --build build --config Release -- /m:1
#    验证产物存在：build\Release\swfparser.exe、swfplayer.exe 及 test_*.exe

# 2) 自动化测试（当前 21 项：解析/AVM1/AVM2/渲染/滤镜/字体/音视频/边界/Graphics/真实样本端到端）
cd build && ctest -C Release --output-on-failure
# 期望：100% tests passed, 0 tests failed out of 21

# 3) 与官方 flashplayer 对比（产物一律放 build\compare\，见 NOTE-2026-003）
# 3a) 我方无头全流程回放（官方同款点击时间表）
swfparser.exe swf2.swf --play-game 620 ..\..\build\compare\our_frames ^
    --click 100:478:700 --click 150:225:375 --click 200:1160:550 ^
    --click 260:94:543 --click 460:650:430 --click 560:1169:596
# 3b) 官方参照帧：python tools\fp_capture.py <swf> build\compare\ref
#     （需人工前台会话运行，见 FIX-2026-002 限制）
# 3c) 并排对比图：python tools\cmp_sheet.py build\compare\ref build\compare\our_frames ^
#     build\compare\cmp <场景名:帧号>...
#     当前时间表基线已脱节（NOTE-2026-004），按场景级对比，勿按帧号硬对齐。
# 3d) GIF 动画对比（推荐，比逐帧直观）：
#     swfplayer.exe <swf> --record build\compare\rec --max-frames 620 --auto-exit ^
#         --click 220:650:430   （PLAY 有效点 = stage (654,449)，见 NOTE-2026-005）
#     python tools\make_gif.py build\compare\rec build\compare\gif\full.gif --fps 12 --scale 0.5
#     python tools\make_gif.py --stack build\compare\rec build\compare\our_frames ^
#         build\compare\gif\cmp.gif --fps 12 --scale 0.35
```

## FIX-2026-010：第一关 next / skip 点击无法进入下一步——SimpleButton 原生类缺失（2026-11-08）

- **状态**：已修复。默认无变量 `--play-game 620` 四次点击
  `--click 310:593:574`（门户 Play）→ `--click 390:473:698`（标题屏 PLAY）
  → `--click 430:150:300`（选角色 pp1）→ `--click 480:1190:550`（SKIP/next）
  可推进到 `steps.GameStep3`。
- **根因**（与 FIX-2026-009 同类，都是原生类对象没注册）：
  `GameStep2.play`(mi=398) 里三处
  `getChildByName("nextBtn" / "skipBtn" / "btnMore") as SimpleButton`
  （`pc=59..77`、`pc=154..161`、`pc=204..212`）。
  `flash.display.SimpleButton` 类对象在 `registerNativeClasses` 里从未创建，
  `getlex [SimpleButton] 373` 得到 undefined → `astypelate` 结果 null →
  `pc=85..114` / `pc=167..192` / `pc=219..244` 的 6 个
  `addEventListener(CLICK / MOUSE_OVER, onClick / onSkip / ...)` 全部注册不上。
  证据：修复前 `GameStep2.play` 之后只有 1 个 `[call] ...addEventListener`，
  修复后 7 个；`[as] getChildByName MISS` 只报 `progressBar`，
  说明 `nextBtn` / `skipBtn` 本身命中了，断点在类型转换而非找不到子实例。
- **修复**：注册 `SimpleButton` 原生类（继承 `InteractiveObject`）；
  `valueIsType` 对没有细分按钮子类型的显示对象包装放行 `as SimpleButton`。
- **验证链路**：`[mouse] click (1190,550) target=skipBtn ... listeners=1`
  → `GameStep2.onSkip`(mi=400) opcode 完整 `0..78`
  （含 `pc=74 send(GameType.NEXT_GAME, GameMain.playerIndex + 2)`）
  → `BusEventListener.send` → `__nextGame`(70) → `playGme`(72)
  → `getClassNameByStep(3)` → `steps.GameStep3.play`(mi=363)；
  AS3 trace `gameStep 下一关卡是 : 3`、`main:... [object steps.GameStep3]`；
  渲染帧 `p_0475`（洗澡关）→ `p_0500`（下一关）画面确实切换。
- **遗留**：`nextBtn`（NEXT 按钮）由 `FrameUtils.frameHandler`(mi=56) 注册到
  stepMC 第 314 帧的 `end`(mi=402) 才 `visible = true`；当前 stepMC 的
  frame1 脚本 `onGoToTimeline`(mi=915) 里 `stop()` 让动画停留，无头回放中
  该按钮不显示（只能点 SKIP）。这属于玩法动画推进链，本轮未改动。
- **验证**：全量构建成功；ctest **21/21 通过**；`read_lints` 0；
  产物 `build\compare\framesFixNext\`（620 帧）、`tr5_full.txt` /
  `tr5_stdout.txt`、`disGs2_full.txt`、`hoverBtns_full.txt` 等。

## FIX-2026-011：开场加载百分比不更新 + 具名实例 AS3 包装不唯一（2026-11-08）

- **状态**：开场加载进度（用户选 A）**已修复**；关卡内进度（C）与肥皂拖拽完成
  字节码级定位，另修掉一处会影响拖拽事件链的语义缺口；分段动画推进仍需后续处理。

### A：开场加载百分比恒为 `0 %`（已修复）

- **现象**：条填充随 `Bar.gotoAndStop(porcentaje)` 正常增长，但条上方的数字始终
  显示 `0 %`（`framesFixNext/p_0005`、`p_0030`、`p_0055`）。`mi=575 progreso`
  的 opcode 跟踪（`SWF_AS_MI=575`）确认每帧 `getproperty [Loader_txt]`(pc=105/121)
  与两次 `setproperty [text]`(pc=117/133) 都执行了，脚本侧无断点。
- **根因**：`Vm::setProperty` 对 `text/htmlText` 只写 `DisplayObject::textData/hasText`；
  `Render.cpp::renderDisplayObjectImpl` 一边照旧用 `DefineEditText` 的 InitialText
  绘制静态 `"0%"`，一边再叠加 `renderDynamicText`。Flash 语义里 `tf.text` 应该
  **替换** InitialText，而不是叠一层，所以初始文本一直留在画面上。
- **修复**：构造 `DisplayItem di` 时，若 `d->hasText` 且角色为 `EditText`，用
  `d->textData` 设置 `di.textOverride`（复用 `renderItemInto` 已有的
  `hasTextOverride` 路径）；同时置 `staticTextReplaced` 跳过 `renderDynamicText`，
  避免同一份文字画两遍。`d->hasTextOverride`（`Player.cpp` 假启动模拟）优先保留。
- **验证**：`framesPbA/p_0010 = 20 %`、`p_0030 = 62 %`，与条填充同步；
  默认 620 帧全链回放 `framesFixText/p_0030 = 62 %`。

### 语义缺口：同一显示对象可能产生多个 AS3 包装（已修复）

- `getPropertyImpl` 的 2.5 分支与 `DisplayObjectContainer.getChildByName` 原先在
  `c->owner` 失效时直接 `newObject(...); o->display = c;`，**没有回写 `c->owner`**。
  于是每次 `clip["gensui"]` / `getChildByName(...)` 都可能产生新包装；帧脚本在
  包装 A 上注册的监听，事件派发走 `objectForDisplay`（会回写 owner 得到包装 B）
  时会整体丢失。统一改为 `objectForDisplay(c)`，保证一个 display 只有一个 AS3 实例。
- 回归：`ownerCheck_full.txt` 中 `[mouse] mouseDown (199,440) target=aa ...
  listeners=2` → `invokeValue method=916 ... class=onPlay1`，`[miss]` 计数 0。

### C / 肥皂：字节码结论（避免重复排查）

- `gameProgressBar`(mi=74) = `m_main.getChildByName(m_factoryName) as ProgressBar`。
  `m_factoryName` 是 `GameMainController` 构造(mi=69)第 5 个**可选参数**，默认值
  `str[28]="progressBar"` 存在 method option 里（不是 `pushstring` 指令，所以
  `SWF_AS_FIND='progressBar'` 全量扫描 0 命中）。
- **SWF 数据里没有 `progressBar` 这个实例名**：`[as] named cid=...` 覆盖所有
  Sprite 的所有帧，主时间轴与 `cid=1280`（`m_main` = `元件137_112`，由 mi=969
  的 `getChildByName("main")` 得到）的具名清单中都没有它，`symbolClass` 也没有
  `ProgressBar` 绑定。当前帧 kids 为 1 个 Shape + 约 60 个无名 `cid391` +
  `stepMC(cid577)` + `btnSound(cid33)`。因此 `getChildByName MISS` 不是解码丢名，
  `gameProgressBar` 为 null 是这段通用模板代码在本 SWF 里的可选分支。
- 右上的关卡指示 `1/5 → 2/5` 由 `m_main.play(step)`（mi=72 `pc=123-125`）的
  帧切换驱动，实测 `p_0475 → p_0500` 正常推进，**不是** `gameProgressBar`。
- 肥皂：`第二关_253`(cid=750) 共 315 帧，`gensui` **从 frame 111 起**才被放置
  （另 cid=932 在 f0/32/81、cid=1042 在 f1..27、cid=1108 在 f234/308/311）。
  frame1 脚本 `mi=935` 调 `onGoToTimeline("aa")`；`mi=915` 的 `pc=40 iffalse`
  检查 `this["gensui"]`，frame0 为 undefined 属 SWF 数据决定的正常行为，
  **不是引擎漏建实例**，`pc=44..91` 的 `mouseChildren/mouseEnabled=false`、
  `startDrag(true)`、`ShakeHelp.setShakeData(...)` 要等 gensui 存在才执行。
- `aa` 实例 world=(198.7,440.1)、depth=509、cid=697。此前网格扫描点
  (280,280)/(720,580) 等都不在它上面，命中 `depth=347/488` 的无名对象，
  由此产生的「被无名覆盖层拦截」判断是**误判**。实测点击 `(199,440)`：
  `target=aa listeners=2` → `onPlay1`(mi=916) → stepMC 推进并执行
  frame 0/1/34/36 帧脚本。
- `addFrameScript` 在 stepMC 上注册于 0,1,34,36,52,106,110,111,112,144,146,
  147,184,186,187,232,233,234,269,270,... 关卡是「点道具 → 播一小段 → `stop()`
  等待下一次点击」的分段节奏；要走到含 `gensui` 的 frame 111，需要按帧脚本
  顺序构造完整点击序列。与 FIX-2026-010 的 `nextBtn`（frame 314 才显示）同属
  玩法动画推进链，本轮未改。

- **遗留**：①构造完整的 GameStep2 分步点击序列，验证 frame 111 之后
  `this["gensui"].startDrag` / `ShakeHelp.setShakeData` 实际执行与道具跟随渲染；
  ②C 若指关卡内圆点进度（`PROGRESS_STEP` → `__progressChange`(mi=73) →
  `gameProgressBar.currentStep++`），需先确认官方该分支是否有 UI 表现，
  按用户方针暂不与官方 Flash 对比。
- **验证**：全量构建成功；ctest **21/21 通过**；`read_lints` 0；
  默认无环境变量 `--play-game 620` 四次点击全链回放通过
  （`build\compare\framesFixText\`，620 帧）；产物另有 `framesPbA\`(60 帧)、
  `clickAa_full.txt`、`hoverScan_full.txt`、`disAll_full.txt`、`ownerCheck_full.txt`。

## FIX-2026-012：开场百分比数字与 `%` 的间距/基线与官方不一致（2026-11-08）

- **状态**：已修复。用户确认差异点是「数字与百分号的空格、上下基线、水平位置」，
  不是百分号字形也不是数值曲线。
- **定量证据**（同为 83%，在官方 GIF 的 640x384 舞台像素空间测量，裁剪原点 x=270/y=205））：

  | 对象 | 官方 `swf2_official_full.gif` frame 10 | 修复前本地 `p_0040` |
  |---|---|---|
  | 数字 | `rows 14-18, cols 49-56` | `rows 11-15, cols 50-58` |
  | `%` | `rows 14-18, cols 60-67` | `rows 14-18, cols 60-67` |

  即 `%` 位置完全一致（它是独立 `DefineText`，按 SWF 笔位绘制），而数字
  **上移 3px、右移 2px**，与 `%` 的间距从 3px 缩到 1px。对比
  `framesFixNext/p_0030`（A 修复前，恒为 `0`）数字也是 `rows 11-15`，
  说明偏移是 `renderEditText` 固有问题，不是 FIX-2026-011 引入。
- **SWF 字段**（`SWF_ET_DBG` dump `Loader_txt` 的 `DefineEditText(37)`）：
  `fontId=81, fontHeight=200tw, bounds=(-40,-40,820,478), align=1(Right),
  autoSize=0, hasLayout=1, L/R/indent=0, leading=40tw`；
  `textMetrics`: `ascent=148.2tw, descent=3.0tw, lineH=151.3tw`。
- **根因**：`Render.cpp::renderEditText` 两处偏离 Flash TextField 语义：
  1. 缺 2px（40 twips）文本框内边距 → 右对齐时数字右边界落在 `bounds.x1`
     而不是 `bounds.x1-40`，比官方右移 2px；
  2. 首行基线用字体 `tm.ascent`（148.2tw）落位，而 Flash 按 `FontHeight`
     （200tw）落位，差 `51.8tw ≈ 2.6px`，数字被抬高、`%` 却在下方 → 基线错开。
- **修复**：文本区域四边各内缩 `kPad=40tw`（`left`/`availW` 同步），
  首行基线改为 `bounds.y0 + kPad + li*lineH + et.fontHeight`；
  `leading`、对齐、换行逻辑不变。影响所有 `DefineEditText`，属语义纠正。
- **验证**：修正后同帧测量数字 `rows 14-18, cols 49-56`、`%`
  `rows 14-18, cols 60-67`，**与官方逐像素一致**；视觉对比
  `build\compare\pct_final_cmp.png`（上=官方、下=本地）。
  全量构建成功；ctest **21/21 通过**（含 `swfparser_edittext`）；
  `read_lints` 0；默认无环境变量 620 帧四次点击全链回放通过
  （`build\compare\framesPctFinal\`，`p_0030 = 62 %`、`p_0500` 关卡 `2/5` 正常）。

## FIX-2026-013：`startDrag` 生命周期 / `stopDrag` 作用域偏离 Flash 语义（2026-11-08）

- **状态**：已修复、已验证。同时**纠正上一轮误判**：「点 `hit` 不推进、
  `startDrag` 不触发」不是引擎缺口，是回放脚本点错了交互对象。
- **代码根因**（`src/Fvm.cpp`）：
  1. `Vm::mouseUp()` 派发 `mouseUp` 后无条件 `dragTarget_.reset()`，注释还写成
     「Flash 语义：松开鼠标即结束」。Flash/Ruffle 的 `startDrag()` 是**舞台级持续
     状态**，只有 `stopDrag()` 或另一次 `startDrag()` 接管才结束；松开鼠标后对象
     仍须跟随指针。本 SWF `frame112` 由帧脚本直接 `startDrag(true)`（没有
     `MOUSE_DOWN` 上下文），旧实现让 `gensui` 在后续 `mouseUp` 后立刻停住。
  2. `stopDrag()` native 原先只在 `dragTarget_ == self->display` 时清除，而 Flash
     允许在**任意** Sprite 上调用 `stopDrag()` 结束当前拖拽。
- **修复**：`mouseUp()` 删除自动 reset；`stopDrag()` 改为无条件清除舞台拖拽状态。
  另清理提交后新增的 `SWF_AS_STEPDBG` 临时诊断（帧坐标表已存
  `build\compare\stepdbg_err.txt`）。
- **字节码链（避免重复排查）**：
  - `mi=915` = `onGoToTimeline(name)`：`stop()` → `this[name].buttonMode=true` →
    `addEventListener(MOUSE_DOWN,onPlay1)` → `if (this["gensui"])` 才执行
    `mouseChildren/mouseEnabled=false`、`startDrag(true)`、
    `ShakeHelp.setShakeData(...)`。
  - `frame37`(mi=938) `onOneTargetDownUp("btn1","gensui2")`；`mi=917` 注册
    `MOUSE_DOWN → ond2`(mi=920)，`ond2` 里 `startDrag` 并给 `stage` 注册
    `ENTER_FRAME → onE`(mi=918) / `MOUSE_UP → onu2`(mi=919)。
    `onE` 用 `this["hit"].hitTestPoint(stage.mouseX, stage.mouseY, true)` 判定，
    命中才 `stopDrag()` + `play()` / `gotoAndPlay(currentFrame+1)`；所以**必须拖拽
    到 `hit` 区域**，单纯点击 `hit` 或点 `btn1` 不松手移动都不会推进。
  - `frame111`(mi=941) `onGoToTimeline("btn0")`；`frame112`(mi=942)
    `onAlphaMove("gensui","hit",onMM,"Ca2")`；`mi=689 onMM` 要求
    `event.target.name == curHitName` 才递减 alpha，需要鼠标在 `hit` 上来回移动。
  - `MouseManager.OnMouseDown`(mi=103) 只是 Stage 全局管理器（遍历
    `m_fun[MOUSE_DOWN]`），**不是**关卡拖拽入口；此前据此怀疑全局回调链是误判。
- **正确的分段交互序列**（时间轴帧坐标 → 舞台坐标：x 不变、`y_stage = y_frame - 336`）：

  | step | 输入 | 结果 |
  |---|---|---|
  | 310 / 390 / 430 | `--click 310:593:574` / `390:473:698` / `430:150:300` | 门户 Play → 标题 PLAY → 选角色 pp1 |
  | 440 | `--click 440:199:440` | `aa` → `onPlay1`(916) → frame 0/1/34/36 |
  | 480-492 | `--drag 480:492:294:122:534:461` | `btn1`→`hit`；`ond2`/`startDrag(gensui2)`/`onE` → frame 52 |
  | 565-577 | `--drag 565:577:1044:464:542:468` | `ren`→`hit` → frame 106 |
  | 580 | `--click 580:159:291` | `btn0` → frame 111，`gensui` 已放置 → `[as] startDrag ... name=gensui cid=716` |
  | 585-621 | 7 个 `--hover` 在 `(450,421)` 附近来回 | `onMM` 递减 alpha → frame 112/113/145/147 |

  实测：tick 581 触发 `this["gensui"].startDrag`，tick 630 `frame113` 再次
  `startDrag(cid729)`，tick 655 `frame147` 执行 `frame147`(mi=945)
  `onOneTargetDownUp("btn2","gensui2")`，等待下一次拖拽。
- **验证**：全量构建成功；ctest **21/21 通过**；`read_lints` 0；
  分步 760 帧带渲染回放 `build\compare\framesStepDrag\`（日志
  `framesStepDrag_err.txt`）推进到 frame 147；默认无环境变量 620 帧四次点击全链
  `build\compare\framesDragFix\` 的 `p_0030`/`p_0500` 与 `framesPctFinal\`
  **逐像素一致**（无回归）。
- **遗留**：frame 147 之后的 `btn2 → aa → hita..hitd` 等待帧序列、以及 frame 309
  `end` / 关卡 `2/5` 尚未构造完整；`lockCenter=true` 目前仍把对象原点锁到指针
  （Flash 应锁 bounds 中心），需要几何包围盒支持后再纠。

## FIX-2026-014：打通关卡内完整交互链 + 游戏内进度条逐条定位（2026-11-08）

- **状态**：FIX-2026-013 的分段链遗留已跑通；进度条的三条路径全部定位，**未发现
  引擎缺陷**。代码改动仅保留 013 的 `startDrag`/`stopDrag` 语义修复，另把硬编码的
  动态属性诊断改为通用开关 `SWF_AS_DYNGET=<属性名子串>`（打印 `r`/`props`/`kids`）。
- **完整的第二关交互序列**（`第二关_253` cid=750；舞台坐标 = 帧内 x、y-336）：

  | step | 输入 | 推进结果 |
  |---|---|---|
  | 310 / 390 / 430 / 440 | 门户 Play / 标题 PLAY / 选角色 pp1 / `aa`(199,440) | frame 0→1→34→36 |
  | 480-492 / 565-577 | `btn1`→`hit` / `ren`→`hit` 拖拽 | frame 52 / 106 |
  | 580 | `btn0`(159,291) | frame 111 `gensui` + `startDrag` |
  | 585-621 / 630 | 7 个 hover 在 `hit` 附近 / 移到角落 | `onMM` 把 `hit.alpha` 递增到 1 → frame 112/145/147 |
  | 660-672 / 710-722 | `btn2`→`hit` / `aa`→`hit` 拖拽 | frame 148/185/187/233 |
  | 770 / 780-810 / 913 | `btn3`(888,278)、`hita..hitd` 四点、`(380,440)` | `lv6`(mi=932) 的 4 次 `onClick`(933)/`onCallBack`(934) 集齐 → `arree.length==0` → `setTimeout(play,300)` |
  | 950-962 | `btn2`→`hit`(526,451) | frame 270/271/272/310 |
  | 1000 | `nextBtn`(1190,638) | frame 314 `end`(mi=402) → `send(nextGame,3)` → `第三关副本_310` |

  关键：`lv6` 注册的是 `MOUSE_MOVE → onClick`（`mi=951` 第 5 参 `pushtrue`），
  目标名 `"hit"+arree[i]` = `hita/hitb/hitc/hitd`；**必须让鼠标扫过 4 个不同目标**，
  重复点已移除监听的目标无效。第 4 个由 `(380,440)` 触发。
- **游戏内进度条排查结论**：
  1. **右上时钟 `1/5 → 2/5 → 3/5`**：由 `main.gotoAndStop(step)` / `m_main.play(step)`
     驱动。正常完成第二关后 `p_0996` 显示 NEXT 按钮 + `2/5`，点 NEXT 后 `p_1050`
     进入第三关并显示 `3/5`，与关卡切换同步，无需修复。
  2. **关卡内动作条 `loading`(cid724 / `进度条_283`)**：擦洗阶段 `onMM`(mi=924)
     逐帧驱动 `Tool.on_scale_Change(loading,cur,tot)`，日志
     `[gotoStop] name=loading` 共 307 次、从 0 递增到 99（走满）。`lv6`(mi=932) 与
     `onCallBack`(mi=934) 里也有 `if (this["loading"])` 保护：cid=750 的 f111/f112/f234
     放置了 `loading`，f233 没有（SWF 的 RemoveObject），所以 frame 233 期间这一段
     不更新是**数据事实**，FlashPlayer 同样如此。
  3. **`gameProgressBar`(mi=74) 恒 null**：`readyGame`(mi=969) 调 `setGameDate` 只传
     4 个参数，第 5 参 `m_factoryName` 取默认 `"progressBar"`；`main`(cid1280)
     没有该具名子实例（`getChildByName MISS want='progressBar'`）。`__progressChange`
     (mi=73) 全文只有 `if (gameProgressBar) gameProgressBar.currentStep++`，是通用
     模板在本 SWF 中的死分支。`PROGRESS_STEP` 由 `FetchGoodHelp1.__stageUp`(mi=129)
     在第三关道具拖放命中时 `send`，本轮序列未进入第三关拖放。
- **已实验并回退的错误假设**：把「已脱离显示列表的具名实例」保留为 `props[name]`
  可以让 `this["loading"]` 在 f233 返回旧对象，但会让 `Tool.on_scale_Change` 作用在
  不可见的旧实例上（`[gotoStop]` 307→312、所有帧逐像素一致），且会破坏
  `if (this["gensui"])` 这类「当前帧是否存在该实例」的判断，偏离 Flash 时间轴字段
  语义，故回退；FIX-2026-013 的 5548 行注释所说的「引用仍可继续使用」指脚本已保存到
  局部变量的 shared_ptr，不是 `clip["name"]` 动态查找。
- **验证**：全量构建成功；ctest **21/21 通过**；`read_lints` 0；默认无环境变量
  620 帧四次点击全链 `build\compare\framesFinal\` 的 `p_0030`/`p_0500` 与
  `framesDragFix\` **逐像素一致**；完整关卡链 1060 帧带渲染
  `build\compare\framesFullChain\`（日志 `stepEnd10_err.txt`）到 frame 314 / 第三关。
- **遗留**：`lockCenter=true` 是否应锁 bounds 中心仍需按 Ruffle / 官方实现核实后再改；
  第三关的 `PROGRESS_STEP` 拖放链尚未构造。

## FIX-2026-015：擦洗进度条遮罩失效（精灵型遮罩不生效）修复（2026-11-08）

- **现象**：用户确认的「游戏内进度条问题」——第二关擦洗阶段画面顶部的红黄条纹
  动作条长度不变（恒为满条），只在擦洗完成时整条消失。
- **根因**：`loading`(cid724) 是 100 帧精灵，其红黄条长度由 depth 2 的遮罩层
  cid721 逐帧平移控制（`--inspect`：`[721] Sprite frames=1`、`[724] Sprite frames=100`）。
  用独立脚本解析 DefineSprite 724 的 PlaceObject2 证实：depth2 = cid721、
  clipDepth=5，其 matrix tx 每帧递增约 28.5 twips（f1 tx=-2360 → f100 ≈ +460）。
  而静态 `renderItemsWithClip` 与运行时 `renderRuntimeChildren` 的遮罩提取
  （`maskShapeOf`）只认 `Kind::Shape`，cid721 是 **Sprite** → 遮罩被判定
  「不可用」，代码走进"组内对象直接画、不裁剪"分支 → 条恒为满长。
- **修复**：新增 `collectMaskShapes(const Character*, Matrix, out)`——遮罩本体
  可以是形状，也可以是"单帧精灵包着的形状"（取 `spriteFrames.front().items`
  中的所有 Shape，并复合各自 `sub.matrix`）。静态路径与运行时路径的
  包围盒计算与 `fillMask` 都改为遍历形状集合；删除只认 Shape 的 `maskShapeOf`。
- **效果**：修复前 p_0585/p_0620/p_0650 顶部条区域逐像素完全相同；修复后
  `p_0585`（progress 0）只剩空的金色槽、`p_0620`（progress≈40）红黄条纹填充至
  中段，随 `Tool.on_scale_Change → loading.gotoAndStop(0..99)` 从空到满渐变。
- **验证**：全量构建成功；ctest **21/21 通过**；`read_lints` 0；默认 620 帧四次
  点击全链 `framesMaskFull\` 的 `p_0030`/`p_0500` 与修复前 `framesFinal\`
  **逐像素一致**（无回归）；完整关卡链 1100 帧 `stepMaskFix_err.txt` 仍为
  tick 996 frame314 `end` → tick 1002 `send(nextGame,3)` → tick 1003 第三关。
- **遗留**：`lockCenter=true` 语义待核实；第三关 `PROGRESS_STEP` 拖放链未构造；
  遮罩精灵若内部还有嵌套精灵只展平一层（本 SWF 的遮罩均为一层形状）。

---

## NOTE-2026-016：「loading的movieclip显示不对」复查结论——无新代码缺陷（2026-11-08）

用户在 FIX-2026-015 提交后仍报告 loading MovieClip 显示不对。逐项复查后确认
**两处 loading 显示在当前构建中均正常**，此前"仍有问题"的判断来自陈旧产物与误报脚本：

1. **擦洗进度条（cid724 loading）正常**：`framesMaskFix` 逐列像素测量 + 放大裁剪
   （y 46-79 波段）证实填充从空槽（p_0585）→ 半条（p_0620）→ 满条（p_0650），
   红黄条纹样式正确。运行时遮罩几何自洽：父矩阵含 ~2.12 横向缩放（341px 遮罩窗
   = cid720 的 160px 局部宽），遮罩右缘扫描 [419→719] 与填充区 [418,717] 设备像素
   完全对齐（f100 lm.tx=+460 twips → 右缘 719）。
   **此前「left[277,419] progress0 恒满」是误报**：`analyze_loading.py`（已删除）的
   颜色谓词 `(r>180,g>180,b<120)` 把**未被遮罩的黄色空槽/端帽**（恒显元素）计入了
   "填充"，与遮罩行为无关。
2. **开屏 preloader 百分比正常**：`framesDefault\`（10:38，早于当日 19:19 构建的
   陈旧产物）中 "0 %" 卡死是旧版本现象。当前构建实测（`framesVerify\`）：
   文本 31% 与填充 ~31% 同步，`porcentaje = loadCount*100/delayCount(48)` 每 tick
   ≈ +2.08，`Bar.gotoAndStop(porcentaje)` 与 `Loader_txt.text`（progreso 处理器
   pc=117/133 两次赋值，cid82 EditText）同源同值；渲染侧经
   `renderDisplayObjectImpl` 的 `textData → textOverride` 分支覆盖 InitialText。
3. **清理**：删除 Render.cpp 中临时诊断 `[maskdbg]`/`[maskbb]`/`[txt82]`；
   删除 build\compare 临时脚本（analyze_loading.py / shapeparse.py / findshape.py）
   与 preDiag*/maskdbg*/inspect*/裁剪 PNG 等。
   **验证**：全量构建成功；ctest **21/21 通过**；标准 620 帧四次点击全链重放
   `build\compare\framesVerify\` 通过（preloader 百分比爬升 → 门户 → 标题 →
   选角色 → 第三关）。

---

## FIX-2026-017：无帧脚本精灵被"保持第 0 帧"逻辑永久锁死——关卡转场动画整体冻结（2026-11-08）

**用户报告**：「第二关与第三关之间 loading movieclip 还是没有修复」「换装的也有很多问题」。

**现象**：关卡转场屏（洗澡动画、关卡开场元素入场、标题屏婴儿逐个弹出等）全部静止；
上一会话（NOTE-2026-016）结论"无新代码缺陷"是**误判**——真正的问题是全局性的：
所有**没有注册任何帧脚本的纯补间动画精灵**一律冻结在第 0 帧。

**根因**（`Fvm.cpp` `advanceTimeline`）：

```cpp
// 修复前：
if (d->currentFrame == 0 && d->lastScriptFrame < 0 && !SWF_NO_HOLDFRAME)
    保持第 0 帧;            // 等本帧稍后的 runFrameScripts 执行首帧脚本
```

"保持第 0 帧"的本意是给 `addFrameScript(0,…)` 的首帧脚本一拍执行机会；释放条件是
`lastScriptFrame >= 0`，而它**只有真正执行过帧脚本才会被置位**。于是：

- 有首帧脚本的精灵（如第三关副本_310 的 stepMC，frame1 执行 `stop()` 等待交互）：
  第一拍被 runFrameScripts 置位 `lastScriptFrame=0`，第二拍起正常推进（且 stop() 后
  按官方语义停在原地）——行为正确；
- **无任何帧脚本的纯补间精灵**（转场屏的 30 个泡泡 cid770、14 朵云 cid767、20 个
  水瓶 cid552、标题屏入场元素……）：`lastScriptFrame` 永远为 -1 → **永远保持第 0 帧**，
  整个 loading MovieClip 冻结。这正是代码注释里 `SWF_NO_HOLDFRAME` 实验想验证
  但从未收尾的问题。

**修复**：保持首帧的条件收紧为「注册了第 0 帧脚本的对象」：

```cpp
if (d->currentFrame == 0 && d->lastScriptFrame < 0 &&
    d->characterId && d->frameScripts.count(0) && !SWF_NO_HOLDFRAME)
```

无帧脚本精灵从创建当 tick 起正常推进（与 Flash「时间轴自动播放」语义一致）；
带首帧脚本的对象保持原有"先执行首帧脚本再推进"的时序。

**排查过程要点**（供后续参考）：
1. 重构第二关完整交互链需用 `--inspect` + 新增诊断 `SWF_SPRITE_DUMP`（保留）：
   `SWF_SPRITE_DUMP=allnames` 打印全部精灵命名放置的 depth/char/tx/ty，
   `SWF_SPRITE_DUMP=<cid>` 打印单精灵各帧放置。由此推得第一关各按钮/热点
   舞台坐标（舞台 y = 帧 y-336）与第二关 hita..hitd、thita..thitd 坐标。
2. `getChildByName('progressBar')` MISS（`gameProgressBar -> null`）经全 SWF
   命名放置扫描证实：**SWF 里根本不存在名为 progressBar 的实例**（4266 个命名
   放置中 0 个），返回 null 是正确行为，游戏代码自行容错，不是缺陷。
3. stepMC（第三关副本_310，cid932，148 帧，'end' 标签在 147）frame1 脚本
   （mi=897，`SWF_AS_MI=897` 转储确认）确实调用 `stop()` 并注册 hita..hitd
   click/gensui startDrag——这是官方等待交互的语义，不是 bug。
4. `frameHandler`（mi=56）只做 `mc.addFrameScript(getFrameIndexByLabel(mc,label), cb)`，
   不负责 play()；动画启动依赖时间轴自动播放（即本修复）。

**修复后验证**（`build\compare\framesVerify17\`，1080 帧交互链重放渲染）：
- 标题屏入场动画恢复（婴儿逐个弹出，tick 20→25 像素差 156750，修复前 1812）；
- 第一关水面注入动画恢复（p_0465→0470 差 4771）；
- 第二关开场元素入场恢复（纸巾盒→草莓→SKIP 依次出现，p_1006→1011 差 18610）；
- 第二关结束点 NEXT → 转场 → 第三关正常推进；
- 交互链未回归：第一关拖拽序列、第二关 lv6/hover 序列、NEXT 按钮链全部照常；
- 全量构建成功；ctest **21/21 通过**。

**遗留**：
- 第二关（点子关）黑点点击命中未打通：可见黑点（cid510/513 等，depth 142/8）盖在
  hita..hitd（d122-139）之上并吞掉点击（`[mouse] click target=stepMC listeners=0`），
  需要进一步确认黑点的点击监听注册机制（疑为动态注册在生成实例上）；
  打通后才能重放「第二关→第三关（换装）」转场与验证换装屏；
- `lockCenter=true` 语义待核实；第一关 `PROGRESS_STEP` 拖放链未构造（遗留延续）；
- `--play-game` 会吞掉其后紧跟的两个参数，`--no-render` 必须写在 outDir 之后。

---

## FIX-2026-019：MorphShape 遮罩不支持——loading 转盘扇形动画冻结（2026-11-08）

**用户报告**：「loading 里的转盘应该是有动画的 但是现在没有显示动画」。

**现象**：SKIP→loading 屏的黄圆转盘完全静止（应为扇形扫过动画）；"loading..." 文字
与 PLAY 按钮出现/缩放正常。逐 tick 像素差证实：转盘区域在遮罩切换 tick（833/876）
之外全为 0 变化。

**根因**（`Render.cpp` `collectMaskShapes`）：遮罩收集器只认 `Shape` 与「单帧精灵
包着的 Shape」两种本体，**不认 `Kind::Morph`**——loading_mc（cid1269）d4 层的扇形
遮罩（c1250→c1252→c1253→c1254→c1258→c1259，每个窗口 20 帧内 ratio 0→1 平滑插值）
是 MorphShape，被静默丢弃 → maskShapes 为空 → 被遮罩的黄饼层（d6 c1251）**整层
直显**，呈现为一个静态满黄圆盘。时间轴侧 ratio 每帧都在更新（`applyTimelineFrame`
的 `child->ratio = di.ratio`），SWF 数据也正常（`SWF_SPRITE_DUMP=1269` 可见逐帧
ratio），卡点纯粹在渲染器遮罩收集。

**修复**：`collectMaskShapes` 增加 Morph 分支——用 `interpolateMorph(morph, ratio)`
插值出中间形状加入遮罩（插值结果由新增的 `owned` 向量持有，存活期覆盖 fillMask）；
精灵遮罩内部的 Morph 子项同样支持。两个调用点分别传入静态项 `it.ratio` 与运行时
`c->ratio`。同时 `SWF_SPRITE_DUMP` 的逐项输出补充了矩阵与 ratio（诊断增强，保留）。

**验证**（`build\compare\nr41\`）：转盘区域 91/185 tick 有连续动画——每帧 ~1500-2100px
的扇形扫过插值变化 + 遮罩切换大跳变（803/819/861）+ 圆盘缩小消失与 PLAY 弹入
（892-913）；PLAY 在 960 正常出现；换装关可达。全量构建成功；ctest **21/21 通过**。

**排查教训**：用 `ImageChops.difference().histogram()` 做帧差时，直方图是 256×3=768 bin，
按"单通道阈值 30"取样必须用 `sum(h[31:256])+sum(h[287:512])+sum(h[543:768])`；
此前误用 `h[798:]`（越界恒空）得到全 0 的假阴性，差点把"转盘无动画"误判为已有动画。

---

## NOTE-2026-018：「SKIP→loading→PLAY→换装」全链验证通过——用户主诉确认由 FIX-2026-017 修复（2026-11-08）

用户提供关键路径：「点击前两关的 skip 就可以跳到 loading，然后继续点击 play 就可以到换装关」。
按此路径逐环节验证，**全部打通**，并确认 FIX-2026-017 正是「第二关与第三关之间 loading
MovieClip 没修复」的根因修复：

1. **SKIP 按钮命中区**（engine 忠实于 SWF 定义，非缺陷）：skipBtn/nextBtn（c753/c756）
   是 DefineButton2，hit 区 = hit 记录形状并集。c751（主 hit 形状）局部包围盒
   [43,32]-[123,57]px，**比可视按钮（c36 大椭圆 + 亮星装饰）小**——SWF 作者本来就把
   热区做小了。网格实测命中点：第一关 SKIP = **(1120,550)**，第二关（点子关）SKIP =
   **(80,515)**，RESET = (1150,548)。点击热区外的可视区域会命中下层 stepMC（与 Flash
   一致）。诊断工具：`SWF_BTN_DBG=1` 打印 ButtonRecord 矩阵与 hit 形状包围盒（已移除）。
2. **loading 屏结构**（main frame 6）：SKIP 第二关 → `main gotoStop 6`。frame 6 的
   stepMC = **换装_448（cid1236，换装关本体）**，其上叠 **midloadingMC_511 蒙层**
   （cid1273，d1842，"loading..." 文本 + 黄色转盘，转盘动画经 FIX-2026-017 正常旋转）。
   蒙层内 loading_mc（c1269）**自动播放**，第 **130** 帧起出现命名实例
   `nextbtn`（c1268，黄底 PLAY 圆钮，舞台 (640,434)）。midloadingMC frame1（mi=561）
   注册 Stage mouseDown → 闭包 onDown（mi=560）：**仅当 `e.target.name=='nextbtn'`**
   才继续（隐藏蒙层、露出换装关）；点击其它位置/过早点击 PLAY 出现前均无效——
   与官方语义一致。**修复前 loading_mc 被锁死在第 0 帧（无帧脚本精灵）→ PLAY 永远
   不出现 → loading 屏永久卡住**，这正是用户看到的现象；FIX-2026-017 后 loading_mc
   正常推进，PLAY 在 ~170 tick 后出现。
3. **换装关交互全部响应**：frame1（mi=789）注册的页签 btn0-3（c1232-1235，星形，
   舞台 ≈(320,280) 附近有 btn1 热区）、List（c1231）各分类的 a1-a6 物品位
   （如 a5@(240,250)、a1@(240,400)）、btnReset（(1150,548)，2 个监听）均可命中并触发
   gotoAndStop/换装。实测：点击物品 a5 → 婴儿戴上蓝帽子+青蛙挂件；点击页签 btn1 →
   左侧面板切换为发饰分类；RESET 点击正常派发。
4. **回放链**（产物 `build\compare\framesVerify18\`，1080 帧）：4 次选角/关卡点击 +
   2 拖拽 + lv6 hover 序列（480-630）→ `--click 660:1120:550`（第一关 SKIP）→
   `--click 770:80:515`（第二关 SKIP → loading 屏）→ `--click 950:640:434`（PLAY）→
   `--click 1005:240:250`（物品 a5）→ `--click 1015:320:280`（页签 btn1）→
   `--click 1025:240:400`（物品 a1）。p_0940 = loading+PLAY，p_1079 = 换装关已戴帽。
5. **清理**：移除 Decoder.cpp 临时诊断 `SWF_BTN_DBG`。全量构建成功；ctest **21/21 通过**。

**遗留**：换装关的深交互（拖拽衣物到婴儿、SHOW 按钮、四分类完整物品、换装关自身的
结题转场）尚未系统回放；点子关（第二关）的正常通关路径仍需打通黑点点击（现用 SKIP
绕过）。若用户在换装关发现具体视觉/交互问题，可按上述元素坐标逐项排查。

---

最近一次全量验证：**2026-11-08**（FIX-2026-017）——全量构建成功；ctest **21/21 通过**；
1080 帧交互链 `build\compare\framesVerify17\`：标题入场/第一关水面/第二关开场动画
全部恢复，关卡推进至第三关；620 帧基线 `build\compare\framesVerify\`、
擦洗进度条 `build\compare\framesMaskFix\`、上一轮全链
`build\compare\framesMaskFull\` / `stepMaskFix_err.txt` 仍有效。
上一次（FIX-2026-014）：全量构建成功；ctest **21/21 通过**；
`read_lints` 0；默认 620 帧四次点击全链 `build\compare\framesFinal\`
（`p_0030`/`p_0500` 与 `framesDragFix\` 逐像素一致）；完整关卡链 1060 帧带渲染
`build\compare\framesFullChain\` 推进至 frame 314 `end` → 第三关。
上一次（FIX-2026-013）：全量构建成功；ctest **21/21 通过**；
`read_lints` 0；默认 620 帧四次点击全链 `build\compare\framesDragFix\`
（`p_0030`/`p_0500` 与 `framesPctFinal\` 逐像素一致）；分步交互 760 帧
`build\compare\framesStepDrag\` 推进至 frame 147。
上一条基线：FIX-2026-012 的 620 帧四次点击全链（产物 `build\compare\framesPctFinal\`，
开场百分比 20%→62% 且数字与 `%` 排版对齐官方 → 门户 Play → 标题屏 PLAY →
选角色 → SKIP/next → 第三关 `steps.GameStep3`）。
上上条基线：FIX-2026-011 的 620 帧四次点击全链（产物 `build\compare\framesFixText\`）。
历史基线：FIX-2026-010 的 620 帧四次点击全链（产物 `build\compare\framesFixNext\`）。
历史基线：FIX-2026-009 的 `--play-game 460 --click 310:593:574 --click 390:473:698`
（进度条 → 门户 Play → 标题屏 PLAY → 第一关 `steps.GameStep1`，
产物 `build\compare\framesFinalPlay\`）。
历史基线：FIX-2026-006 的 `--play-game 345 --click 310:593:574`
（进度条 → 门户 Play → 游戏标题屏，产物 `build\compare\framesDefault\`）。
历史基线：2026-11-07 全量构建 19 目标 + 21/21 + 620 步回放 9 组官方对比图。

## FIX-2026-022：启动进度页/门户 Play 按钮点不中——mouseDown 误派发给冻结的真实游戏 Vm（2026-11-09）

**用户报告**：「开始进度页 的按钮又无法点击了」（FIX-2026-020 重写启动模拟后引入的回归；
用户明确要求"修复 bug 时禁止把之前能用的功能给修改了"。）

- **状态**：已修复、已验证。默认无环境变量 `--play-game 1480` 完整回放：门户 Play `btn_play`、
  标题 `btnPlay`、关卡 `skipBtn`、换装页签 `btn3`、物品 `a6`/`a4` 全部 `listeners=1` 命中；
  12 个测试可执行文件 **全部 OK（0 failures）**；`read_lints` 0。`String.replace`（FIX-2026-020）
  与关卡交互不受影响。

- **根因**：FIX-2026-020 把启动模拟改为「独立影片副本 `loadingMovie_` + `loadingVms_` 维持
  preloader/门户播放，期间不推进真实游戏 Vm（`vms_`）」。门户阶段渲染用的显示树是
  `loadingVms_` 的 `firstFrameStage_`（`render()` 第 699 行把 `movie_.runtimeStage` 临时换成
  `firstFrameStage_`）；但 `Player::mouseDown/mouseUp/setMouse` 仍把 AVM2 鼠标事件**无条件**
  派发给 `vms_`（真实游戏，门户阶段被冻结在第 0 帧）。于是命中解析落在冻结的 `vms_` 第 0 帧
  显示树上——门户 `btn_play` 所在容器（`preloaderRoot_`）从未被 `setClipFrame(...,1)` 推进到
  门户帧，副本里可见的按钮在 `vms_` 树里不存在 → `[mouse] click ... listeners=0`，按钮点不中。

- **修复**（`Player.cpp`）——按当前阶段把 AVM2 鼠标事件派发给「正在渲染」的那组 Vm：
  ```cpp
  std::vector<std::shared_ptr<fvm::Vm>>& Player::activeVms() {
      if (!loadingVms_.empty() &&
          (loadingElapsed_ < loadingTotal_ || portalElapsed_ < portalTotal_))
          return loadingVms_;
      return vms_;
  }
  // mouseDown / mouseUp / setMouse 内 for (auto& vm : vms_) 改为 for (auto& vm : activeVms())
  ```
  AVM1 按钮命中（`pollMouseEvents` 对 `curItems_` 的解析）本就正确，无需改动。改动只影响
  AVM2 事件派发目标，且门户结束后 `activeVms()` 回到 `vms_`，与 FIX-2026-020 的换装交互逻辑
  完全隔离，未触碰任何已可用功能。

- **验证**：`--play-game 360 --click 310:593:574` 修复前 `target= depth=123 chain=5
  listeners=0`；修复后 `target=btn_play depth=231 chain=5 listeners=1`。完整回放全链点击
  命中列表见上。

## FIX-2026-020：换装关部分装饰点击无反应——`String.replace` 未实现（2026-11-09）

**用户报告**：「换装所有的衣服装饰都点击一下，就是装饰有的就是没反应的」+「开始的进度怎么没有了」。
（进度条问题见 FIX-2026-020 之前的 Player.cpp 启动模拟修复；本条只针对装饰点击。）

- **状态**：已修复、已验证。默认无环境变量 `--play-game 1450` 完整回放通过；12 个 avm2/
  avm1/边界/图形/字体/音频测试可执行文件 **全部 OK（0 failures）**；`read_lints` 0。

- **根因**：换装关页签列表点击处理器 `onListClick1`(mi=788) 取物品序号的写法为
  `int(e.target.name.replace('a',''))`（例如按钮实例名 `a6` → 去掉 `a` → `6`）。
  引擎的 `String` 类**从未注册 `replace` 成员**，于是 `getProperty(String,'replace')` 取到
  `undefined`，对其 `callprop` 返回 `undefined`；`int(undefined)`=NaN，`convert_i`→`0`。
  结果处理器用 `this.m_show['LV3'].gotoAndStop(0)` 把该 LV 部件**重置回底帧**，与"点击没反应 /
  装饰消失"完全吻合。所有分类的物品都走同一写法，所以"有的有反应有的没反应"其实取决于该分类
  默认帧是否恰好等于 0，并非交互逻辑本身有选择性分支。

- **修复**（`Fvm.cpp`，String 实例成员）：
  ```cpp
  Member rp; rp.kind = Member::Method; rp.name = "replace";
  rp.native = [](shared_ptr<Object> self, const vector<Value>& a) -> Value {
      auto p = self->props.find("$str");
      string s = (p != self->props.end()) ? p->second.s : string();
      if (a.size() < 2) return Value::str(s);
      string pat = a[0].toString();
      if (pat.empty()) return Value::str(s);
      string rep = a[1].toString();
      auto pos = s.find(pat);
      if (pos == string::npos) return Value::str(s);
      return Value::str(s.substr(0, pos) + rep + s.substr(pos + pat.size()));
  };
  c->classDef->instanceMembers.push_back(rp);
  ```
  AS3 语义：`pattern` 为字符串时**只替换第一处**（与官方一致）；`pattern` 为空直接返回原串。

- **验证**：在 gotoStop 原生加临时 `SWF_GOTO_ARG` 诊断，对比修复前后同一分类4 点击
  （`(120,250)` 命中实例 `a6`）的实参：
  - 修复前：`[gotoArg] name=LV3 raw='0' kind=3 -> 0`（重置底帧，装饰无变化）。
  - 修复后：`[gotoArg] name=LV3 raw='6' kind=3 -> 5`（正确跳到该物品对应帧 idx5）。
  说明 `e.target.name.replace('a','')` 现在正确得 `6`，处理器按物品序号切换 LV 部件帧。
  （诊断代码已清理，未保留在最终提交中。）

- **排错工具沉淀**：`SWF_AS_OPS=<mi>` 强制只追踪指定 method body；`SWF_AS_DEBUG` 下
  `gotoStop` 原生打印 `name/cid/total/->idx` 便于核对部件帧切换。

---

## FIX-2026-023：加载转盘"卡住"诊断 + 通用 MorphShape 补间修复（2026-11-09）

**用户报告**：Flash 播放器加载转盘（loading 转盘）"卡住"——不旋转 / 视觉冻结。

- **状态**：已诊断、已部分修复（morph 补间）、**无回归验证通过（21/21 全绿）**。
- **核心结论**：加载 / midloading 转盘的"卡住"**不是引擎 bug**，属**进度驱动的预期行为**；
  真正修到的通用缺陷是快照解码未展开 shape-tween 跨度（见下）。

### 一、转盘"卡住"诊断结论（非 bug）

分两层，均与 pct 进度相关：

1. **启动进度转盘（Bar 扇形扫描 + 中心盘）**：`updateLoadingProgress()`（`Player.cpp` ~286-300）
   的 pct 公式为 `41 + 75*(loadingElapsed_-1)/loadingTotal_`，**封顶 99%** 直到切入门户页
   （注释明确"停在 99% 直到切入门户页"——对齐官方行为，**非冻结**）。pct 到 99% plateau 即停转。
2. **关卡内 midloading 黄色转盘**：仅转 ~3-6 帧即冻。旋转由**运行时脚本按加载进度设 `_rotation`
   （`turntable._rotation = progress*360`）**驱动，进度一完成即停转——脚本行为，非静态 tween。
   A/B 测试（`SWF_NO_TWEEN=1` 开/关 morph 补间）确认 midloading 转盘在有无 morph 补间时行为一致，
   **证明转盘旋转与 morph tween 无关**。

### 二、通用缺陷修复：MorphShape 跨度补间（保留）

- **根因**：本引擎快照式解码只把显示列表逐帧拷贝（ShowFrame 时快照），不展开 tween 跨度。
  Flash 的 shape tween 是"跨度"——MorphShape 仅在首尾关键帧各 `PlaceObject(HasRatio)` 一次，
  中间帧 ratio 由播放器按播放头线性插值生成。快照解码导致 tween 跨度内所有中间帧的 morph 比值
  停留在首关键帧 → **变形动画冻结**（这才是真正的"卡住"类 bug）。
- **修复落点**：
  | 文件 | 内容 |
  |---|---|
  | `src/Decoder.cpp` | 新增 `expandShapeTweens(std::map<...>& chars)`：对同一 depth 上连续帧内比值恒定的 Morph，按首尾关键帧比值线性插值中间帧 ratio；`decodeMovie` 末尾调用（`!std::getenv("SWF_NO_TWEEN")` 守卫，便于 A/B）。 |
- **不覆盖项**：loading 转盘旋转由运行时进度驱动（见上），此处**刻意不覆盖**。

### 三、已回退的危险改动（matrix / motion tween 插值）

- 曾尝试在 `expandShapeTweens` 中追加**矩阵/运动补间插值**（旋转/位移中间帧矩阵插值）。
- **回退原因**：加载屏时序发生变化（transition 从 1341→1350 偏移），说明移动了交互剪辑，
  **存在破坏已验证按钮流程的回归风险**。依据"修复 bug 时禁止把之前能用的功能改坏"规则，**回退
  matrix 部分，仅保留 MorphShape ratio 补间**。matrix 运动补间属独立特性，需另立任务、独立验证后再做。

### 四、验证

```bash
cmake --build build --config Release -- /m:1   # exit 0
ctest --test-dir build -C Release --output-on-failure   # 21/21 passed, 0 failed
```
测试可执行文件全部 OK，`bad=0`；`read_lints` 0（仅 `Decoder.cpp(2473)` 一处预存无关的 C4065 warning）。

### 五、可选后续（非必须，会改变官方行为）

若用户坚持转盘应"持续旋转"而非进度完成即停：需改为**运行时按模拟进度持续驱动 `_rotation`**，
但这会偏离官方 Flash 行为，须明确与用户确认后再做。

---

## FIX-2026-024：加载转盘"卡住"真正根因——首帧持有逻辑永久锁死带第 0 帧脚本的剪辑（2026-11-09）

**用户报告 / 上下文**：上一轮（FIX-2026-023）把 loading 转盘"卡住"误判为"进度驱动、非 bug"，
但本轮用 `fp_capture.py` 驱动**真实官方 Adobe Flash Player 21 projector** 与本项目 `swfparser.exe`
做了逐帧对比（双方均 1280×768），运动信号（相邻帧平均绝对差）证明：

- **官方 REF**（每 10 帧）：`0.0 0.1 0.4 0.7 0.3 0.6 0.6 0.8 0.8 1.1 1.2 0.7 ...` —— 加载转盘**全程持续转动**。
- **本方 OURS（修复前，默认）**（每 5 帧）：`0.0 0.0 3.4 0.0 0.0 0.0 0.0 0.0 0.0 0.0 0.4 0.3` ——
  加载期 step 0–49 整屏**完全静止**，仅首尾微动。即 swf2 加载转盘在本方确实**冻住**，用户主诉属实，
  上一轮"不是 bug"结论**有误**。

### 根因（已用 `SWF_SIM_DBG` 诊断 + `SWF_NO_HOLDFRAME` A/B 双重实锤）

- 加载期渲染用 `firstFrameStage_`（preloader 副本的 AVM2 树，`Fvm.cpp:6607` 确认
  `vm->stage` 与 `movie.runtimeStage` 同一对象图，故 `vm->tick()` 推进的就是被渲染的树）。
- 诊断显示 `cid=387`（preloader 容器）之子树中多个**带第 0 帧脚本的剪辑被永久钉在 frame 0**
  （`playing=1` 却 `frame` 不前进），其子树（含转盘）整体冻结。
- `Vm::advanceTimeline`（`Fvm.cpp:5800`）对子对象**无条件递归**，子剪辑默认 `playing=true`，
  本应推进；但 FIX-2026-017 的"持有首帧"分支（`Fvm.cpp:5835`）：
  ```cpp
  if (d->currentFrame == 0 && d->lastScriptFrame < 0 &&
      d->characterId && d->frameScripts.count(0) &&
      !std::getenv("SWF_NO_HOLDFRAME")) { /* hold frame 0 */ } else { ++d->currentFrame; ... }
  ```
  把带第 0 帧脚本的剪辑（及其承载转盘的子树）**无限期锁死在 frame 0**——`runFrameScripts`
  虽独立执行帧脚本，但持有逻辑把该剪辑的播放头钉死，导致官方在转的加载转盘在本方静止。
- A/B：`SWF_NO_HOLDFRAME=1` 后本方运动信号变为全程连续 `0.43 0.29 0.27 0.40 0.37 ...`，
  与官方模式一致 → **首帧持有即元凶**。

### 修复落点

| 文件 | 内容 |
|---|---|
| `src/Fvm.cpp` | 将首帧持有改为**默认关闭**（仅 `SWF_HOLDFRAME=1` 显式启用；`SWF_NO_HOLDFRAME=1` 仍作显式关闭别名）。`runFrameScripts` 本就独立执行第 0 帧脚本，故关闭持有不影响帧脚本语义；纯补间剪辑（无第 0 帧脚本）本就不受持有影响，FIX-2026-017 初衷无损。 |

### 验证（全绿）

```bash
cmake --build build --config Release -- /m:1              # exit 0
ctest --test-dir build -C Release --output-on-failure    # 21/21 passed, 0 failed
# 加载转盘运动信号（默认即生效，无需开关）：
#   OURS(default) motion (every5): 0.43 0.29 0.27 0.40 0.37 0.32 0.38 0.29 0.27 0.40 0.38 0.37
#   ← 修复前为 0.0 0.0 3.4 0.0 ...（冻结）；现与官方"持续转动"一致
python tools/cmp_loading.py   # 重新生成 build/compare/cmp_loading/ 下 our/ref 并排与缩略图
```
- 回放回归：`--play-game 220 --click 100:478:700 --click 150:225:375 --click 200:1160:550`
  干净跑完 220 步，`bad=0`。
- 产物：`build/compare/cmp_ours/`（本方）、`build/compare/cmp_ref/`（官方连拍）、
  `build/compare/cmp_loading/loading_sidebyside.png`（上排官方、下排本方，各 12 帧取样）。

### 注

- 此修复**直接对齐官方 Flash Player 行为**（转盘在加载期持续转动），非偏离官方的"持续旋转"改造；
  FIX-2026-023「五、可选后续」关于"进度完成即停 vs 持续旋转"的取舍仍另议，与本修复无关。
- 若某些第三方 SWF 确需首帧持有以保证第 0 帧脚本时序，可 `SWF_HOLDFRAME=1` 回退本默认。

### ⚠️ 已回退（2026-11-09 当日）

用户反馈"**又出现更多问题了，很多都没有 stop**"——本修复（全局默认关闭首帧持有）是**过度修正**：

- 显示树诊断实锤：preloader 树中 `cid=41/42/80/83/85`（fs0=0）卡 frame 0 是**无 Sprite 帧**（静态图，
  与持有无关）；`cid=67`（星光，cid=75 之下）在持有开启下**本来就在推进**（8→10）。
- 被持有真正影响的是**全游戏所有带第 0 帧脚本的剪辑**——大量剪辑的 `addFrameScript(0,..)` 调用
  `stop()` 让自己停在首帧（正确 AS3 语义）。关掉持有会让它们**越过第 0 帧、`stop()` 永不执行**，
  全部失控播放 → "很多都没有 stop"。
- **回退动作**：`src/Fvm.cpp` 恢复首帧持有默认开启（原 FIX-2026-017 语义，`SWF_NO_HOLDFRAME`
  仍可显式关闭）。"转盘转动"的正确路径是**脚本驱动旋转**（enterFrame 里 `_rotation` 按进度驱动，
  官方即如此），见 FIX-2026-025。

```bash
cmake --build build --config Release -- /m:1              # exit 0
ctest --test-dir build -C Release --output-on-failure    # 21/21 passed, 0 failed（回退后）
```

### FIX-2026-025（2026-11-09）：加载转盘(intro_mc) 在加载期即出现

**根因（用 SWF_AS_OPS / addFrameScript / gotoPlay 跟踪 + 持有开/关子树对比实证）**：
- 全片唯一 enterFrame 处理器是 `progreso`（挂在 `preloading_1` cid=387），它只读
  `root.loaderInfo.bytesLoaded/bytesTotal`（引擎硬编码 100/100，故 `porcentaje` 恒=100）、
  `Bar.gotoAndStop(porcentaje)`、`Loader_txt.text=…`，**既不设 `_rotation` 也不调用 gotoAndPlay**。
- `preloading_1` 第 0 帧脚本 `this.stop(); parent.stop(); Bar.stop();` 把自己钉在 frame 0，
  并 `Service.getInstance().init(…)` + 监听 `SERVICE_READY/onServiceReady`。**转盘 `intro_mc`
  (cid=386) 位于 `preloading_1` 的第 1 帧，只有 `SERVICE_READY → onServiceReady → gotoAndPlay(intro)`
  才会进入**。本引擎无网络，Service 永不就绪 → 转盘永远停在 frame 0、从未被实例化。
- 关持有（SWF_NO_HOLDFRAME=1）只是让 `preloading_1` 越过第 0 帧、时间轴自然推进到第 1 帧才"碰巧"
  放置 intro_mc，但同时破坏全游戏 stop() 语义（即用户报的"很多都没有 stop"），故不能走这条路。

**修复（初版，已回归）**：加载初始化时（`Player.cpp` load() 检测到 `preloaderRoot_` 后）调用
`loadingVms_.front()->setClipFrame(preloaderRoot_, 1)`，把 preloader 推进到第 1 帧，使转盘在
**加载期**即出现。但用户实测反馈：**进度条消失**——因为 `preloading_1` 是两段式，
第 0 帧含进度条 `Bar`(cid=79)/logo/按钮，第 1 帧才放 `intro_mc`(转盘)，**两者是不同帧**，
单条 `setClipFrame(…,1)` 会用第 1 帧内容整体替换第 0 帧，`Bar`(cid=79) 被移除 → 进度条不见。

**修复（终版）**：撤销加载期跳帧，恢复两段式——
- 加载期：`preloading_1` 停第 0 帧（进度条 `Bar` cid=79 可见），由 `progreso` 每帧 `Bar.gotoAndStop(100)`。
- 门户阶段（portal，`Player.cpp` 既有代码）：`setClipFrame(preloaderRoot_, 1)` 跳到第 1 帧放 `intro_mc`(转盘)。
实测 `loadtick1 preloaderFrame=0 Bar(cid79) present=1`，进度条已恢复；转盘在门户阶段出现。
**不可**用单条 `setClipFrame` 同时显示两段内容；若要转盘与进度条在加载期同屏，需叠加渲染（见下）。

**验证**：`ctest 21/21` 通过；`preloaderFrame=0 Bar(cid79) present=1`（进度条恢复）。

**剩余 gap（后经 FIX-2026-026 复核，两条均不成立，见下）**：
1. ~~转盘旋转是经典 motion tween，快照解码器不插值 → 像素不变、静止~~ → **证伪**：
   sandboxType 修正后 SWF 自身 AS3 已驱动门户动画，实测相邻帧差分 0.25~0.99、
   intro_mc 各徽章逐个缩放入场，运动信号与官方 0.4~1.2 量级一致，无需额外插值。
2. ~~加载期需进度条与转盘同屏~~ → **不成立**：SWF 本就是"加载页→门户页"两段式顺序播放，
   加载期只显示进度条是官方语义，不是缺陷。

### FIX-2026-026（2026-11-09）：按 SWF 自身 AS3 顺序驱动转盘，撤销 Player.cpp 跳帧 hack

**背景（方法论纠正）**：上一版 FIX-2026-025 在 `Player.cpp` 加载/门户两处 `setClipFrame(preloaderRoot_,1)`
强制把 preloader 跳到第 1 帧——这是"改通用代码迁就 swf"，违反"应让 AVM2 解释器按 SWF 自身 AS3
顺序正确执行"的原则；且加载期跳帧会整体替换第 0 帧、反把进度条(Bar)删掉（用户报"进度条又不见了"）。
正确做法：修解释器，让 SWF 自己把转盘放出来。

**根因（用 SWF_AS_DIS / SWF_AS_OPS / SWF_AS_FIND 反汇编 Service 与 preloading_1 全部相关方法）**：
- `Service.init` 构造 `URLLoader` 并 `loader.load(new URLRequest(url))`；`onComplete`(mi=343) 解析响应、
  `avaiable = !(response=="failure")`、`dispatchEvent(SERVICE_READY)`。`onServiceReady`(mi=570) 校验
  `isServiceAvaiable`(返回 `this.avaiable`) 真→`start_loading`(mi=571)=`addEventListener(ENTER_FRAME,progreso)`；
  `progreso`(mi=575) 每帧算 `porcentaje=floor(carga/peso*100)`(引擎 loaderInfo 硬编码100/100→恒100)、
  `Bar.gotoAndStop(porcentaje)`，`porcentaje>=100` 时 `removeEventListener` 并 `MovieClip(root).play()`。
- SWF 内**没有 `gotoAndPlay("intro")`**；转盘(intro_mc cid=386，在 preloading_1 第1帧) 是靠
  `root.play()` 推进主时间轴到转盘帧而出现的。`Security.sandboxType` 原值 `"application"`（非 Flash
  合法沙箱类型）：`init` 只在 `== "localWithFile"` 时走离线分支（直接 dispatchEvent 却**不设 avaiable**
  → 守卫失败→不出转盘），否则走 URLLoader 网络分支。引擎 `URLLoader.load()`(Fvm.cpp:4039) 已实现会
  `fire COMPLETE` → `onComplete` 设 `avaiable=true` 并 `dispatchEvent(SERVICE_READY)` → 后续链自然驱动转盘。

**修复（仅改解释器/AS3 语义，不动 Player.cpp）**：`Security.sandboxType` 由 `"application"` 改为
`"remote"`（本游戏经 HTTP 提供，对应 served 沙箱）。`init` 走 URLLoader 分支 → 引擎 fire COMPLETE →
`onComplete` 设 `avaiable=true` 并 dispatch SERVICE_READY → `onServiceReady`→`start_loading`→`progreso`
→ `porcentaje>=100` 调 `MovieClip(root).play()` → preloading_1 推进到第1帧 → intro_mc 出现。
**同时撤销** `Player.cpp` 加载期与门户期两处 `setClipFrame(preloaderRoot_,1)` hack（SWF 自身已驱动，无需）。

**验证**：`preloaderFrame`/`intro_mc` tick dump 显示 tick1-33 `preloaderFrame=0`（进度条 Bar 可见）、
tick34+ `preloaderFrame=1 intro_mc=1`（转盘由 SWF 自身 AS3 放置）。`ctest 21/21` 通过。全程无 Player.cpp
跳帧 hack，符合"修解释器而非改通用代码"的方针。

**复核（2026-11-09，本轮）：前述"motion-tween 插值" gap 证伪，无需实现**。
- `SWF_AS_DIS`/运行时 dump 复核：`intro_mc`(cid=386) 时间轴**正常逐帧推进**（frame 0→222、
  playing=1，子精灵 334 自身 currentFrame 亦同步推进），门户页由"独立播放的 MovieClip"驱动，
  并非"只在关键帧放置、中间帧静止"的快照。
- 实拍门户页：logo + 星光 + 5 个分类徽章（Makeover/Celebrity/Management/Dress up/Cooking）
  **逐个缩放/淡入入场**（f150 时 Celebrity 为缩小态、f180 时 Cooking 为缩小态），星光持续闪烁。
- 400 帧回放运动信号：门户阶段相邻帧差分 0.25~0.99、变化像素 0.48%~1.55%，
  与官方基准 0.4~1.2 同量级；peak 帧 132/134/149/164/179 对应各徽章入场。
- 结论：把"画面看着不动"归因为"解码器不做 motion-tween 插值"是**误判**——真实原因是
  修复前 `Service` 分支走错（`sandboxType` 非 `remote`）、门户页根本没被 SWF 放出来。
  FIX-2026-026 修正后动画已完整驱动，**不追加插值特性**（属过度工程且有回归风险）。
- 本轮仅做：更正注释、`portalStarted_` 死成员清理、临时 debug 全清；`ctest 21/21`。

### FIX-2026-027（2026-11-09）：移除开局"停一会"——撤销默认自动启用的伪造启动时序

**用户报告**：「flashplayer里并没有停止一会」（官方播放器开局没有停顿）。

**根因（最后一处"通用代码迁就 swf"）**：
- `Player.cpp` 头部注释本就写明「本地 SWF 一次性完整加载，Flash Player 不会插入固定时长的
  *模拟加载页/门户页*」，但 `FIX-2026-020` 违背它：`load()` 用
  `const bool simForceOff = (SWF_STARTUP_SIM=="0"); if (!simForceOff && ...)`，
  **无环境变量即默认自动启用**启动模拟（检测到 preloader+intro 结构就复制整部影片、
  冻结主 Vm、硬编码 `loading=36 帧(1.5s) + portal=300 帧(12.5s)` 共 336 帧 ≈ 14 秒，
  并伪造进度百分比 41→99）。`startupSimEnabled()` 定义了却**从未被调用**。
- 官方文档与真实语义一致：本地整片加载时 `loaderInfo.bytesLoaded` 立即等于
  `bytesTotal`，`progreso` 的 `porcentaje` 一帧即满，SWF 自身 AS3 连续推进，不暂停。
- A/B 实测（400 帧回放）：默认模式前 30 帧几乎全静止（相邻帧差分 `0.02~0.07`），
  frame 340 才切进游戏；`SWF_STARTUP_SIM=0` 开局即连续动画（`[0:40] mean=9.88`），
  frame 60 已是游戏标题页。根时间轴仅 `frames=2`，证实 336 帧停留纯属人为。

**修复**：`load()` 启用条件由 `!simForceOff`（默认启用）改为显式
`startupSimEnabled()`（仅 `SWF_STARTUP_SIM=1` 启用），恢复真实语义为默认。
同时展开已失效的 `if (portal || simForceOn)` 判断（`portal` 现仅用于定位
preloader 容器/计算门户时长）、修正自相矛盾的头部注释。**保留**整套模拟代码与
`SWF_STARTUP_SIM=1` 作 A/B 对照，不删已验证的进度条 / 门户 Play 流程。

**验证**：
- 默认 `SWF_SIM_DBG=1` 无 `[sim] engaged`；`SWF_STARTUP_SIM=1` 仍
  `engaged loading=36 portal=300`（A/B 完好）。
- 默认 400 帧：`[0:40] mean=9.88 / [40:340] mean=0.10 / max=38.64`，与
  `SWF_STARTUP_SIM=0` 一致，无 336 帧停留。
- 点击链路不回归：默认 `--play-game 160 --click 60:473:698`（标题屏 PLAY）
  step 61 画面差分 `91.84`，进入三选一关卡页。
- `ctest 21/21` 通过。

> **本条已作废（由 FIX-2026-028 取代）**：仅翻转默认值，仍然保留了"用配置开关迁就 swf"
> 的子系统，且默认关闭后 `首页的又没有了`。FIX-2026-027 中关于 A/B 开关保留、
> "默认不插入加载页/门户页"的结论**均不再成立**。

### FIX-2026-028（2026-11-09）：删除全部"迁就 swf"的启动模拟，改由通用 AS3 首帧语义还原首页

**用户要求**：「首页的又没有了 代码内不能又任何迁就swf的功能 就是读资源播放读as3执行」。
即播放器只做通用能力（解码资源、按时间轴播放、执行 AVM1/AVM2 脚本、输入/音视频），
不得为特定 SWF 伪造时序；首页必须由 SWF 自身 AS3 放出来。

**首页消失的真正根因（此前误判为"默认不该有加载页"）**：
- `Vm::advanceTimeline()` 的首帧持有条件要求 `d->characterId` 且注册了
  `frameScripts[0]`；根时间轴 `cid=0` 被排除。第一次 tick 根即从 frame 0 推进到 frame 1，
  主时间轴第 0 帧上的 `preloading_1` 的 `addFrameScript(0, ...)` 从未执行，
  因而 `stop(); parent.stop(); Bar.stop(); Service.init(...)` 整条初始化链缺失，
  preloader 首页/进度条根本没被建立。
- FIX-2026-027 只把模拟默认关掉，掩盖而非修复该语义缺陷，导致首页消失。

**修复（全部落在通用解释器中）**：
- `src/Fvm.cpp` `Vm::advanceTimeline()`：`holdFirst` 增加 `isStageRoot` 分支
  （根 `cid=0` 也在第 0 帧停一拍），帧脚本由同帧稍后的 `runFrameScripts` 执行；
  若脚本调用 `parent.stop()`，根的 `playing` 变 false，后续按 SWF 自身逻辑停在首页。
- 删除 `SWF_NO_HOLDFRAME`、`SWF_LEGACY_ROOT_FRAME` 两个会改变语义的 A/B 开关及
  `legacyRootFrame()`：正确的 AS3 语义无条件生效，不再由环境变量挑选行为。
- `src/Fvm.cpp` 的 `Security` 注释改为通用运行时 API 描述（`sandboxType="remote"`
  的值不变，属内建类实现，不是按 SWF 分支）。

**删除的整套模拟子系统（"迁就 swf"部分）**：
- `SWF_STARTUP_SIM` / `SWF_SIM_DBG` / `[sim]` 诊断输出、`startupSimEnabled()`。
- `setLoadingProgress()` / `updateLoadingProgress()`（伪造 41→99% 进度）。
- `load()` 中复制影片、冻结根、预热 16 帧、`findPortalRoot`、硬编码
  `loading=36 / portal=300` 计时、`firstFrameStage_` 快照等全部逻辑。
- `tick()` 的加载期/门户期分支 → 统一 `for (auto& vm : vms_) vm->tick();`。
- `render()` 临时替换 `movie_.runtimeStage` 的分支 → 统一按当前帧渲染。
- `activeVms()` 及鼠标事件派发的阶段判断 → 统一派发给 `vms_`。
- `include/swf/Player.hpp` 中 `loadingMovie_` / `firstFrameStage_` / `loadingVms_` /
  `loadingTotal_` / `loadingElapsed_` / `portalTotal_` / `portalElapsed_` /
  `preloaderRoot_` / `updateLoadingProgress()` / `activeVms()` 全部移除。
- 全仓 `src/`、`include/` 已无上述标识符残留（grep 0 命中）。

**验证（默认、无任何环境变量）**：
- 全量构建 `cmake --build build --config Release -- /m:1` 通过；`ctest` **21/21** 通过。
- `--play-game 120 --click 100000:0:0`（不注入点击）→ `build/compare/cmp_pure2/`，
  缩略条 `build/compare/pure2_strip.png`：
  step 0 显示 YOKO GAMES loading 首页；step 5/20/30/40/48 进度条逐帧递增；
  step 55 进度条消失；step 70/90/119 为品牌门户页（星光 + 徽章）。
  首页与进度条完全由 SWF 的 `progreso`（ENTER_FRAME）驱动，无 Player 层计时。
- `SWF_AS_DEBUG=1` 日志确认 `runFrameScript frame=0 owner=preloading_1`、
  `dispatch enterFrame ... [progreso]` 正常执行。
- 点击链路不回归：`--play-game 160 --click 80:593:574 --click 140:473:698`
  → `build/compare/cmp_pure2_click/`，缩略条 `build/compare/pure2_click_strip.png`：
  step 80 命中 `btn_play` → `iniciarJuego`(576)，step 85/110 标题屏完整入场，
  step 140 命中 `btnPlay`（click listeners=1）→ step 145/159 三选一关卡页。
  与模拟时代 `--click 310:... / 390:...` 相比只是帧号提前（不再有 336 帧人为停留），
  链路本身完好。
