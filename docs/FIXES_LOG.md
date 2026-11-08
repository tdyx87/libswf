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
| DOC-2026-001 | "缺全局 5 阶段播放循环；`Player.cpp:18` 自承前数百帧静止" | 那段注释描述的是**已废弃旧路径**（仅 `SWF_STARTUP_SIM=1` 启用）；`Player::tick`（`Player.cpp:594-677`）按每个 MovieClip 独立时间轴驱动，且复现官方"加载页→门户页→游戏"启动时序。**不是差距** |
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
     → **已由 FIX-2026-006 完成**：转正为默认（`SWF_LEGACY_ROOT_FRAME=1` 可 A/B）；
       假模拟已默认下线（保留 `SWF_STARTUP_SIM=1` 作对照，未删死代码）。
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
| `src/Fvm.cpp` 根时间轴语义 | `SWF_ROOT_TIMELINE` **转正**：默认从第 1 帧（索引 0）建根、默认允许根逐帧推进；新增 `legacyRootFrame()`（static-local 缓存、零每帧 getenv 开销），`SWF_LEGACY_ROOT_FRAME=1` 可复现旧"最后具名帧 + 冻结根"行为做 A/B |
| `src/Fvm.cpp` 两处 `loaderInfo` getter | 回退 TEMP-DIAG，`bytesLoaded` 恢复 100（整片本地加载语义） |

### 关键语义约定（改代码前必读）
1. **`root` = 文档类实例（MovieClip 子类），`stage` = Stage 单例**，二者不是同一个对象；
   任何事件/树构建路径都不得覆盖根 display 已有的 AS3 owner，否则 `root.play()` 失效。
2. `progreso` 的 **`porcentaje >= 100` 分支**才是门户切换分支；`<100` 分支只累加 `loadCount`。
3. `play()/stop()` 原生直接改 `self->display->playing`；根（`cid=0`）能否推进由
   `isStageRoot` 决定，现为默认开启。
4. `Player.cpp` 的假启动模拟早已由 `SWF_STARTUP_SIM=1` 门控（默认 `loadingTotal_=portalTotal_=0`
   时那段分支是死代码）。保留作 A/B 对照，未删除，避免无收益的大范围重构。

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

---

最近一次全量验证：**2026-11-08**（FIX-2026-012）——全量构建成功；ctest **21/21 通过**；
默认无环境变量 `--play-game 620` 四次点击全链回放通过
（开场百分比 20%→62% 且数字与 `%` 排版对齐官方 → 门户 Play → 标题屏 PLAY →
选角色 → SKIP/next → 第三关 `steps.GameStep3`，产物 `build\compare\framesPctFinal\`）。
上一条基线：FIX-2026-011 的 620 帧四次点击全链（产物 `build\compare\framesFixText\`）。
上上条基线：FIX-2026-010 的 620 帧四次点击全链（产物 `build\compare\framesFixNext\`）。
历史基线：FIX-2026-009 的 `--play-game 460 --click 310:593:574 --click 390:473:698`
（进度条 → 门户 Play → 标题屏 PLAY → 第一关 `steps.GameStep1`，
产物 `build\compare\framesFinalPlay\`）。
历史基线：FIX-2026-006 的 `--play-game 345 --click 310:593:574`
（进度条 → 门户 Play → 游戏标题屏，产物 `build\compare\framesDefault\`）。
历史基线：2026-11-07 全量构建 19 目标 + 21/21 + 620 步回放 9 组官方对比图。
