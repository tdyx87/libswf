#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "swf/Audio.hpp"
#include "swf/Avm1.hpp"
#include "swf/Decoder.hpp"
#include "swf/FFmpeg.hpp"
#include "swf/Fvm.hpp"
#include "swf/MovieClipCommands.hpp"
#include "swf/Render.hpp"

namespace swf {

class Player : public avm1::ITimeline {
public:
    Player();
    ~Player();

    bool load(const std::vector<std::uint8_t>& data, std::string& err);
    bool loadFile(const char* path, std::string& err);

    void play(); void stop(); void gotoAndPlay(int frame); void gotoAndStop(int frame);
    void nextFrame(); void prevFrame();
    void stopAllSounds();
    int currentFrame() const { return frame_; }
    int totalFrames() const { return (int)movie_.frames.size(); }
    int framesLoaded() const override { return (int)movie_.frames.size(); }
    float frameRate() const { return movie_.frameRate; }
    int stageW() const { return stageW_; }
    int stageH() const { return stageH_; }
    // 诊断（窗口播放器日志用）：AVM2 虚拟机数量与运行时舞台根节点数量。
    size_t vmCount() const { return vms_.size(); }
    size_t runtimeRoots() const {
        size_t n = 0;
        for (const auto& vm : vms_) n += vm->stage.size();
        return n;
    }

    void tick();
    // targetW/targetH > 0：按该设备分辨率重新光栅化（窗口放大时保持矢量/文字清晰）；
    // 传 0 或不传则输出舞台原生尺寸（CLI 导出 PNG / 像素断言）。
    bool render(std::vector<std::uint8_t>& rgba, int& W, int& H,
                int targetW = 0, int targetH = 0);

    void setMouse(double x, double y); void mouseDown(); void mouseUp(); void keyDown(int vk); void keyUp(int vk);

    // 鼠标当前是否悬停在按钮上（Flash 对按钮使用手形光标）
    bool handCursor() const;

    void* findChildClip(const std::string& name) override;
    void* resolveClipPath(const std::string& path) override;
    std::string clipName() const override { return "_root"; }
    std::string targetPath() const override { return "/"; }
    bool getNumberProp(int propId, double& out);
    void setNumberProp(int propId, double v);
    bool getBoolProp(int propId, bool& out);
    void setBoolProp(int propId, bool v);
    bool getStringProp(int propId, std::string& out);
    // 供 SpriteClip（精灵自身时间轴适配器）在 gotoAndPlay/gotoAndStop 使播放头真正改变时调用：
    // 执行目标内部帧的脚本（Flash 语义：进入目标帧即运行该帧动作）。parent 为该精灵实例的上级剪辑。
    void runClipFrameScript(DisplayItem& it, int idx, avm1::IClip* parent);
    // AVM1 MovieClip 扩展方法（根时间轴版本）：attachMovie/duplicateMovieClip/
    // createEmptyMovieClip/startDrag/stopDrag/getBounds/setMask 作用于顶层显示列表。
    avm1::Value callExt(const std::string& method, const std::vector<avm1::Value>& args) override;

private:
    swf::Movie movie_;
    swf::avm1::Machine avm1_;
    std::vector<std::shared_ptr<fvm::Vm>> vms_;
    AudioOut audio_;
    bool playing_ = true;
    int frame_ = 1;
    int stageW_ = 550, stageH_ = 400;
    double mouseX_ = 0, mouseY_ = 0;
    bool mouseDown_ = false;
    // 当前悬停 / 按下的鼠标目标身份：自顶层向下的 (depth, characterId) 链。
    // 用链而非顶层下标，使"按钮嵌套在精灵内部"也能被稳定追踪（显示列表每 tick 重建）。
    std::vector<std::pair<int, int>> hoverChain_;
    std::vector<std::pair<int, int>> pressChain_;
    std::vector<DisplayItem> curItems_;
    // 命名/精灵实例剪辑对象池（每帧按当前显示列表重建）。AVM1 的 _root.<name> 通过
    // findChildClip 取得这里的 IClip*，从而支持带帧标签的 gotoAndStop/gotoAndPlay 与属性访问。
    class ItemClip;
    std::vector<std::unique_ptr<ItemClip>> clipPool_;
    std::map<const DisplayItem*, ItemClip*> clipByItem_;
    // 时间轴执行语义：帧脚本"进入该帧时执行一次"。
    // scriptFrame_ = 已执行过脚本的帧（0-based）；-1 表示尚未执行。
    int scriptFrame_ = -1;
    // AVM1 的 gotoAndPlay/gotoAndStop 不立即改变播放头，而是登记为待跳转，
    // 在下一个 tick 生效（与 Flash 一致：跳转从下一帧开始，且会执行目标帧脚本）。
    struct PendingGoto { bool active = false; int frame = 1; bool play = true; };
    PendingGoto pendingGoto_;
    // AVM2 影片的模拟加载阶段。官方 Flash Player 流式加载时 preloader 会持续显示约
    // 1.5 秒并让进度条递增；本地一次解码完没有加载过程，播放器第一次 tick 就把主
    // 时间轴推到第 2 帧，加载画面只会一闪而过。这里用独立的影片副本 + Vm 维持一段
    // preloader 播放，期间不推进游戏 Vm（其具名实例 main 只在第 2 帧，改动初始帧
    // 会让 AS3 初始化失败、游戏卡在内部 loading）。
    Movie loadingMovie_;
    std::vector<std::shared_ptr<fvm::DisplayObject>> firstFrameStage_;
    std::vector<std::shared_ptr<fvm::Vm>> loadingVms_;
    int loadingTotal_ = 0;
    int loadingElapsed_ = 0;
    // 加载完成后的品牌门户阶段：preloader 主容器的第 2 帧（intro_mc）。
    int portalTotal_ = 0;
    int portalElapsed_ = 0;
    bool portalStarted_ = false;
    std::shared_ptr<fvm::DisplayObject> preloaderRoot_;
    void updateLoadingProgress();

    // 当前阶段应接收鼠标事件的 Vm 集合：加载/门户阶段渲染的是启动副本
    // （loadingVms_ 的 firstFrameStage_），AVM2 点击必须派发给该副本，否则会落到
    // 仍冻结在第 0 帧的真实游戏 Vm（vms_），导致门户 Play 等按钮点不中（FIX-2026-022）。
    std::vector<std::shared_ptr<fvm::Vm>>& activeVms();

    void snapshotItems(int frameIndex);
    // 递归为显示列表中的每个精灵实例重建其内部帧运行态子树（children）：以静态帧快照
    // 为模板，按 (depth, characterId) 从旧子树继承运行态（rtFrame/按钮态/是否初始化）。
    void syncChildrenTree(std::vector<DisplayItem>& items, int rootFrame);
    // 递归推进播放头（含嵌套精灵）：每个 MovieClip 实例拥有独立播放头。
    // parent 为该层实例所在上级剪辑（供脚本内 _parent 解析；顶层为 nullptr）。
    void advanceTree(std::vector<DisplayItem>& items, ItemClip* parent);
    // 递归执行"新放置"精灵的内部第 1 帧脚本（含嵌套）。parent 语义同 advanceTree。
    void initTree(std::vector<DisplayItem>& items, ItemClip* parent);
    void runFrameScript(int frameIndex);
    // 新放置精灵执行其内部第 1 帧脚本（Flash 语义）；脚本 stop() 会把精灵停在首帧。
    void runSpriteFirstFrames(int frameIndex);
    // 每个 MovieClip 实例拥有独立播放头：每帧前进一帧并执行该内部帧的脚本
    // （Flash 语义：剪辑时间轴独立于主时间轴推进；这是游戏循环/动画的驱动机制）。
    void advanceClips();
    // 重新建立命名剪辑对象池（递归遍历当前显示列表，含精灵内部）
    void buildClipPool();
    void collectClips(std::vector<DisplayItem>& items, ItemClip* parent, const std::string& pathPrefix);
    // ClipActions 事件派发：放置时发 Load/Initialize（一次性，clipInited 标记），每 tick 递归发 EnterFrame。
    void fireClipEvents();
    // 在显示列表（含嵌套精灵）里按路径查找 DisplayItem（供 hitTest / 路径解析）。
    DisplayItem* findItemByPath(const std::string& path, DisplayItem* base, ItemClip* baseClip);
    // 计算某显示项（含其子树）在舞台坐标下的包围盒（像素，左上原点）。失败返回 false。
    bool itemStageAABB(const DisplayItem& it, double& x0, double& y0, double& x1, double& y1) const;
    // 计算某显示项当前正在显示的内部帧下标（Sprite；非 Sprite 返回 -1）
    int itemFrameIndex(const DisplayItem& it, int rootFrame) const;
    ItemClip* clipForItem(const DisplayItem* it) const;
    // 在某精灵剪辑上下文里执行其内部第 idx 帧脚本（供 gotoAndPlay/Stop 用）
    void runClipFrame(DisplayItem& it, int idx, ItemClip* parent);
    // keyPress 派发：递归（含嵌套精灵）收集带该键码 keyActions 的按钮并执行其 AVM1 动作
    void fireKeyButtons(std::vector<DisplayItem>& items, DisplayItem* parentItem,
                        ItemClip* parentClip, int keyCode);
    void runButtonAction(DisplayItem* parentItem, ItemClip* parentClip, const avm1::Script& sc);
    // 把 _root 变量值同步到绑定了同名 VariableName 的 EditText 实例（含嵌套精灵内的文本框）
    void syncTextFields();
    void playSoundEvent(const SoundEvent& ev);
    // 鼠标事件派发：把命中项当作按钮，切换 Up/Over/Down 态并执行其 AVM1 动作
    // （Button2 CondAction 条件位：0x01 rollOver 0x02 rollOut 0x04 press 0x08 release 0x40 releaseOutside）
    void pollMouseEvents(bool down);
    // 执行按钮实例 btn 在条件位 condBit 上的 AVM1 动作。container 为包含该按钮的显示项
    // （按钮挂在顶层时间轴上时为 nullptr）：作为脚本作用域（this）与 _parent 的基准。
    void fireButtonEvent(DisplayItem* btn, DisplayItem* container, int condBit);
    // 按 (depth, characterId) 链在当前显示列表（含嵌套精灵）里解析显示项；
    // container 输出其直接容器项（顶层项时为 nullptr）。解析失败返回 nullptr。
    DisplayItem* resolveItemChain(const std::vector<std::pair<int, int>>& chain,
                                  DisplayItem** container) const;
    // ---- Facade：AVM1 扩展方法统一委托给 MovieClipCommands（Command 表）----
    // 旧的 makeSymbolItem/insertDynamicItem/findContainerOf/boundsObject/applyDrag
    // 与 DragState 都是只有声明、无实现也无调用点的死代码，已由命令对象取代。
    std::unique_ptr<MovieClipCommands> ext_;
    // 构造命令上下文：只暴露命令需要的最小能力（含由 Facade 注入的回调）。
    ExtCommandContext makeExtContext();
};

using PlayerPtr = std::unique_ptr<Player>;

}  // namespace swf

