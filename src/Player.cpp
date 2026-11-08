#include "swf/Player.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>

#include "swf/Audio.hpp"
#include "swf/Avm1.hpp"
#include "swf/Render.hpp"

namespace swf {

// 本地 SWF 是一次性完整加载的：Flash Player 不会插入固定时长的"模拟加载页 /
// 门户页"。历史上这里为特定样本复制整部影片、冻结主 Vm 并伪造 1.5s + 12.5s
// 启动时序，导致通用 AVM2 影片前数百帧画面静止、脚本不推进，与官方播放器差异很大。
// 默认按真实语义播放；SWF_STARTUP_SIM=1 可复现旧时序用于 A/B 对照。
static bool startupSimEnabled() {
    const char* e = std::getenv("SWF_STARTUP_SIM");
    return e && e[0] == '1';
}

// 精灵内部时间轴适配器：执行精灵内部帧脚本时，把无目标的 stop()/play()/gotoAndPlay()
// 等播放控制落到**该精灵自身**（而非主时间轴），对应 Flash 中"剪辑内的脚本作用于自身"。
// 仅在一次脚本执行期间存活（非拥有 item）。
class SpriteClip : public avm1::ITimeline {
public:
    SpriteClip(DisplayItem& item, int total, int curIdx, Player* owner = nullptr,
               avm1::IClip* parent = nullptr)
        : item_(item), total_(total < 1 ? 1 : total), cur_(curIdx), owner_(owner), parent_(parent) {}

    void gotoAndPlay(int frame) override { applyFrame(frame, true); }
    void gotoAndStop(int frame) override { applyFrame(frame, false); }
    void play() override { item_.rtPlaying = true; item_.spStopped = false; }
    void stop() override { item_.rtPlaying = false; item_.spStopped = true; }
    void nextFrame() override { applyFrame(cur_ + 2, false); }
    void prevFrame() override { applyFrame(cur_, false); }
    void stopAllSounds() override {}
    int currentFrame() const override { return cur_ + 1; }
    int totalFrames() const override { return total_; }
    int framesLoaded() const override { return total_; }
    std::string targetPath() const override { return "/"; }

private:
    // frame 为 1-based（与 Flash 的 gotoAndPlay/Stop 参数一致）。仅当播放头真正改变（重新
    // "进入"目标帧）才执行该帧脚本——否则像 bonus 反复 gotoAndStop 到同一帧会重复触发其动作。
    void applyFrame(int frame, bool play) {
        if (frame < 1) frame = 1;
        if (frame > total_) frame = total_;
        int idx = frame - 1;
        const bool changed = (idx != cur_);
        cur_ = idx;
        item_.rtFrame = idx;
        item_.rtPlaying = play;
        item_.spStopped = !play;
        if (changed && owner_) owner_->runClipFrameScript(item_, idx, parent_);
    }
    DisplayItem& item_;
    int total_;
    int cur_;
    Player* owner_;
    avm1::IClip* parent_;
};

// 命名/精灵实例剪辑：AVM1 通过 `_root.<name>` / `_parent` 得到的对象。指向当前显示列表中的
// DisplayItem（每帧重建，有效期仅限本帧），支持带帧标签的 gotoAndStop/gotoAndPlay 与常见属性读写。
// 作为 Player 的嵌套类，可直接访问 Player 的私有成员与辅助方法。
class Player::ItemClip : public avm1::IClip {
public:
    ItemClip(Player* owner, DisplayItem* item, std::string name, ItemClip* parent)
        : owner_(owner), item_(item), name_(std::move(name)), parent_(parent) {
        // 剪辑作用域变量落在 DisplayItem::clipVars（跨帧重建时持久）。无则绑定本对象的新表。
        if (item_) {
            if (!item_->clipVars) item_->clipVars = members;
            members = item_->clipVars;
        }
    }

    // 命名剪辑上的扩展方法转发给 Facade；此前走 IClip 默认实现恒返回 undefined。
    avm1::Value callExt(const std::string& method,
                        const std::vector<avm1::Value>& args) override {
        return owner_ ? owner_->callExt(method, args) : avm1::Value::undef();
    }

    bool getNumberProp(int propId, double& out) override {
        if (!item_) return false;
        switch (propId) {
            case avm1::PROP_X:           out = itemX();
                return true;
            case avm1::PROP_Y:           out = itemY();
                return true;
            case avm1::PROP_XSCALE:      out = itemScaleX() * 100.0; return true;
            case avm1::PROP_YSCALE:      out = itemScaleY() * 100.0; return true;
            case avm1::PROP_CURRENTFRAME:out = itemCurrentFrame(); return true;
            case avm1::PROP_TOTALFRAMES:
            case avm1::PROP_FRAMESLOADED:out = totalFrames(); return true;
            case avm1::PROP_ALPHA:       out = itemAlpha() * 100.0; return true;
            case avm1::PROP_ROTATION:    out = itemRotation(); return true;
            default: return false;
        }
    }
    void setNumberProp(int propId, double v) override {
        if (!item_) return;
        switch (propId) {
            case avm1::PROP_X:        item_->hasRt = true; item_->rtX = v; break;
            case avm1::PROP_Y: {
                item_->hasRt = true; item_->rtY = v;
                break; }
            case avm1::PROP_XSCALE:   item_->hasRt = true; item_->rtScaleX = v / 100.0; break;
            case avm1::PROP_YSCALE:   item_->hasRt = true; item_->rtScaleY = v / 100.0; break;
            case avm1::PROP_ALPHA:    item_->hasRt = true; item_->rtAlpha = v / 100.0; break;
            case avm1::PROP_ROTATION: item_->hasRt = true; item_->rtRotation = v; break;
            case avm1::PROP_VISIBLE:  setBoolProp(propId, v != 0); break;
            default: break;
        }
    }
    bool getBoolProp(int propId, bool& out) override {
        if (item_ && propId == avm1::PROP_VISIBLE) { out = item_->rtVisible; return true; }
        return false;
    }
    void setBoolProp(int propId, bool v) override {
        if (item_ && propId == avm1::PROP_VISIBLE) { item_->rtVisible = v; item_->hasRt = true; }
    }
    bool getStringProp(int propId, std::string& out) override {
        if (propId == avm1::PROP_NAME)   { out = name_; return true; }
        if (propId == avm1::PROP_TARGET) { out = getPath(); return true; }
        return false;
    }
    void callTimeline(const std::string& method, const std::vector<avm1::Value>& args) override;
    void* parentClip() override { return static_cast<void*>(parent_); }
    // 在自身的子显示列表中按实例名查找子剪辑（如 T1 的 "ob"）。
    void* findChildClip(const std::string& name) override {
        if (!item_) return nullptr;
        for (auto& c : item_->children) {
            if (c.name == name) {
                ItemClip* ic = owner_->clipForItem(&c);
                if (ic) return static_cast<void*>(ic);
            }
        }
        return nullptr;
    }
    std::string getPath() const override { return path_; }
    // MovieClip.hitTest(target[, shapeFlag])：target 为剪辑对象或目标路径字符串。
    // 以舞台坐标包围盒（AABB）相交判定（shapeFlag 略）。
    bool hitTest(const avm1::Value& target, bool shapeFlag) override {
        (void)shapeFlag;
        if (!item_) return false;
        double ax0 = 0, ay0 = 0, ax1 = 0, ay1 = 0;
        const bool okA = owner_->itemStageAABB(*item_, ax0, ay0, ax1, ay1);
        const DisplayItem* t = nullptr;
        if (target.iclip) {
            auto* oc = static_cast<ItemClip*>(target.iclip);
            t = oc ? oc->item_ : nullptr;
        } else if (target.type == avm1::Value::T::String) {
            t = owner_->findItemByPath(target.str, item_, this);
        }
        double bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
        const bool okC = okA && t && owner_->itemStageAABB(*t, bx0, by0, bx1, by1);
        return okC && !(ax1 < bx0 || bx1 < ax0 || ay1 < by0 || by1 < ay0);
    }
    DisplayItem* itemPtr() const { return item_; }

    const std::string& clipNameStr() const { return name_; }
    void setPath(std::string p) { path_ = std::move(p); }

private:
    double itemX() const { return item_->hasRt ? item_->rtX : item_->matrix.tx / 20.0; }
    double itemY() const { return item_->hasRt ? item_->rtY : item_->matrix.ty / 20.0; }
    double itemScaleX() const { return item_->hasRt ? item_->rtScaleX : item_->matrix.sx; }
    double itemScaleY() const { return item_->hasRt ? item_->rtScaleY : item_->matrix.sy; }
    double itemRotation() const {
        if (item_->hasRt) return item_->rtRotation;
        return std::atan2(item_->matrix.r1, item_->matrix.sx) * 180.0 / 3.14159265358979323846;
    }
    double itemAlpha() const {
        if (item_->hasRt) return item_->rtAlpha;
        return item_->cxform.hasMult ? item_->cxform.ma : 1.0;
    }
    double totalFrames() const {
        return (item_->character && item_->character->kind == Character::Kind::Sprite)
                   ? static_cast<double>(item_->character->spriteFrames.size()) : 0.0;
    }
    double itemCurrentFrame() const {
        if (!item_->character || item_->character->kind != Character::Kind::Sprite) return 0.0;
        int idx = owner_->itemFrameIndex(*item_, owner_->frame_ - 1);
        return idx < 0 ? 0.0 : idx + 1.0;
    }
    Player* owner_;
    DisplayItem* item_;
    std::string name_;
    ItemClip* parent_;
    std::string path_;
};

// ---- keyPress 派发：Windows 虚拟键码 -> Flash keyPress 键码 ----
// Flash 的 keyPress 专用键码表（与 Key.getCode() 不同）：Left=1 Right=2 Up=14 Down=15
// Space=32 Enter=13 Tab=18 Escape=19 Backspace=8 Home=3 End=4 Insert=5 Delete=6 PageUp=16
// PageDown=17；ASCII 32..126 直通。
static int vkToKeyPressCode(int vk) {
    switch (vk) {
        case 0x25: return 1;    // VK_LEFT
        case 0x27: return 2;    // VK_RIGHT
        case 0x24: return 3;    // VK_HOME
        case 0x23: return 4;    // VK_END
        case 0x2D: return 5;    // VK_INSERT
        case 0x2E: return 6;    // VK_DELETE
        case 0x08: return 8;    // VK_BACK
        case 0x0D: return 13;   // VK_RETURN
        case 0x26: return 14;   // VK_UP
        case 0x28: return 15;   // VK_DOWN
        case 0x21: return 16;   // VK_PRIOR (PageUp)
        case 0x22: return 17;   // VK_NEXT (PageDown)
        case 0x09: return 18;   // VK_TAB
        case 0x1B: return 19;   // VK_ESCAPE
        case 0x20: return 32;   // VK_SPACE
        default: break;
    }
    if (vk >= 0x30 && vk <= 0x5A) return vk;          // 0-9 / A-Z
    if (vk >= 0x60 && vk <= 0x69) return vk - 0x60 + 0x30;  // 小键盘数字
    return 0;
}

Player::Player() : audio_(), ext_(std::make_unique<MovieClipCommands>()) {}
Player::~Player() = default;

// Facade 只负责组装上下文：把私有能力（路径查找 / 包围盒）以回调形式注入 Command。
ExtCommandContext Player::makeExtContext() {
    ExtCommandContext ctx{movie_, curItems_, frame_, mouseX_, mouseY_};
    ctx.boundsOf = [this](const std::string& path, double& x0, double& y0,
                          double& x1, double& y1) -> bool {
        DisplayItem* it = findItemByPath(path, nullptr, nullptr);
        return it ? itemStageAABB(*it, x0, y0, x1, y1) : false;
    };
    return ctx;
}

// AVM1 ITimeline::callExt 的唯一入口：Facade -> Command 表分派。
avm1::Value Player::callExt(const std::string& method,
                            const std::vector<avm1::Value>& args) {
    if (!ext_) return avm1::Value::undef();
    return ext_->call(method, makeExtContext(), args);
}

// 模拟流式加载：AS3 preloader 的进度条通常是 100 帧的 MovieClip（一帧 1%），
// 百分比写在 Loader_txt 这类 TextField 里，两者由 loaderInfo.bytesLoaded 驱动。
// 本地一次解码完没有真实加载过程，按播放帧数推进即可复现官方加载界面。
static void setLoadingProgress(const std::shared_ptr<fvm::DisplayObject>& d,
                               const Movie& movie, int pct, const std::string& label,
                               const std::function<void(const std::shared_ptr<fvm::DisplayObject>&, int)>& gotoFrame) {
    if (!d) return;
    // 百分比数字：时间轴 EditText（如 Loader_txt）初始文本是 "0"，用 textOverride
    // 覆盖；旁边的 "%" 是独立的静态 Text，不要再输出一次。
    if (d->characterId &&
        (d->name.find("Loader") != std::string::npos ||
         d->name.find("percent") != std::string::npos)) {
        d->textOverride = label;
        d->hasTextOverride = true;
    }
    auto ci = movie.characters.find(d->characterId);
    if (ci != movie.characters.end() && ci->second &&
        ci->second->kind == Character::Kind::Sprite) {
        const std::size_t n = ci->second->spriteFrames.size();
        // 进度条必须按实例名命中：preloader 里还有其它 95~105 帧的剪辑
        // （本片 cid=67 是头像眨眼动画），只按帧数选中会把它们一起改写。
        const bool isBar = !d->name.empty() &&
            (d->name.find("Bar") != std::string::npos ||
             d->name.find("bar") != std::string::npos ||
             d->name.find("Progress") != std::string::npos ||
             d->name.find("progress") != std::string::npos);
        // SWF 标准做法：preloader 用 gotoAndStop(bytesLoaded 百分比) 让 Bar 的
        // 100 帧 MorphShape 按 ratio 插值出填充宽度。MorphShape 终止段几何已
        // 正确解码，中间帧可直接渲染；frame 41 的 ratio=27141/65535≈0.414 即 41%。
        if (isBar && n >= 95 && n <= 105) {
            d->playing = false;
            gotoFrame(d, pct);
        }
    }
    for (const auto& c : d->children) setLoadingProgress(c, movie, pct, label, gotoFrame);
}

void Player::updateLoadingProgress() {
    if (loadingTotal_ <= 0) return;
    // 官方播放器逐帧实测为 41/54/66/79/91 %（每 6 帧约 +12.5）：加载进度
    // 线性推进，到加载窗口 2/3 处约 91%，随后停在 99% 直到切入门户页。
    int pct = 41 + 75 * (loadingElapsed_ - 1) /
                       (loadingTotal_ > 0 ? loadingTotal_ : 1);
    if (pct < 41) pct = 41;
    if (pct > 99) pct = 99;
    const std::string label = std::to_string(pct);
    auto gotoFrame = [this](const std::shared_ptr<fvm::DisplayObject>& d, int frame) {
        if (!loadingVms_.empty()) loadingVms_.front()->setClipFrame(d, frame);
    };
    for (const auto& d : firstFrameStage_)
        setLoadingProgress(d, loadingMovie_, pct, label, gotoFrame);
}

bool Player::load(const std::vector<std::uint8_t>& data, std::string& err) {
    if (!decodeMovie(data.data(), data.size(), movie_, err)) return false;
    if (movie_.frames.empty()) { err = "SWF has no frames"; return false; }
    stageW_ = static_cast<int>((movie_.frameSize.x1 - movie_.frameSize.x0) / 20.0f);
    stageH_ = static_cast<int>((movie_.frameSize.y1 - movie_.frameSize.y0) / 20.0f);
    if (stageW_ <= 0) stageW_ = 550;
    if (stageH_ <= 0) stageH_ = 400;

    vms_.clear();
    if (!movie_.abcFiles.empty()) vms_ = fvm::runMovieScripts(movie_);

    // 用独立的影片副本以第 0 帧初始化 preloader 显示树；主 Vm 仍按原语义初始化，
    // 否则 AS3 文档类构造时取不到 main，游戏会卡在内部 loading。副本须长期存活，
    // 因为 loadingVms_ 持有对它的引用。
    firstFrameStage_.clear();
    loadingVms_.clear();
    loadingTotal_ = 0;
    loadingElapsed_ = 0;
    portalTotal_ = 0;
    portalElapsed_ = 0;
    portalStarted_ = false;
    preloaderRoot_.reset();
    if (startupSimEnabled() && !movie_.abcFiles.empty() && movie_.frames.size() > 1) {
        loadingMovie_ = movie_;
        auto firstVms = fvm::runMovieScripts(loadingMovie_, 0);
        // 官方播放器窗口出现时 preloader 入场动画已跑了一会儿：先推进若干帧，
        // 让头像与 logo 处于加载中而非初始位置。
        for (int i = 0; i < 16; ++i)
            for (auto& vm : firstVms) vm->tick();
        if (!loadingMovie_.runtimeStage.empty()) {
            firstFrameStage_ = loadingMovie_.runtimeStage;
            loadingVms_ = firstVms;
            loadingTotal_ = static_cast<int>(movie_.frameRate * 1.5f + 0.5f);
            if (loadingTotal_ < 1) loadingTotal_ = 1;
            std::function<std::shared_ptr<fvm::DisplayObject>(
                const std::shared_ptr<fvm::DisplayObject>&, const std::string&)> findByName =
                [&](const std::shared_ptr<fvm::DisplayObject>& d, const std::string& part)
                    -> std::shared_ptr<fvm::DisplayObject> {
                    if (!d) return nullptr;
                    if (d->name.find(part) != std::string::npos) return d;
                    for (const auto& c : d->children)
                        if (auto hit = findByName(c, part)) return hit;
                    return nullptr;
                };
            // 进度条按 SWF 语义由 gotoAndStop(pct) 驱动：Bar 每帧放置同一
            // MorphShape 并给出 ratio，Morph 终止段几何解码后中间帧可正确渲染。
            updateLoadingProgress();
            // 门户页位于 preloader 主容器的第 2 帧：该帧移除全部加载控件并放置
            // intro 剪辑。识别出这个容器后，加载完成即跳到该帧，品牌页（渐变
            // 背景、logo、星光、分类气泡、按钮）由其内部时间轴自行播放。
            std::function<std::shared_ptr<fvm::DisplayObject>(
                const std::shared_ptr<fvm::DisplayObject>&)> findPortalRoot =
                [&](const std::shared_ptr<fvm::DisplayObject>& d)
                    -> std::shared_ptr<fvm::DisplayObject> {
                    if (!d) return nullptr;
                    auto ci = loadingMovie_.characters.find(d->characterId);
                    if (ci != loadingMovie_.characters.end() && ci->second &&
                        ci->second->kind == Character::Kind::Sprite &&
                        ci->second->spriteFrames.size() >= 2) {
                        for (const auto& it : ci->second->spriteFrames[1].items)
                            if (it.name.find("intro") != std::string::npos) return d;
                    }
                    for (const auto& c : d->children)
                        if (auto hit = findPortalRoot(c)) return hit;
                    return nullptr;
                };
            for (const auto& r : firstFrameStage_)
                if (auto hit = findPortalRoot(r)) { preloaderRoot_ = hit; break; }
            if (preloaderRoot_ && !loadingVms_.empty()) {
                portalTotal_ = static_cast<int>(movie_.frameRate * 12.5f + 0.5f);
                if (portalTotal_ < 1) portalTotal_ = 1;
            }
        }
    }

    // 把 AVM1 解释器接到本时间轴：否则 Machine::run 会因 timeline==nullptr 直接返回，
    // 脚本中的 stop/play/gotoAndPlay 等全部失效（表现为"只推进时间轴、不执行脚本"）。
    avm1_.timeline = this;
    avm1_.frameLabels = movie_.frameLabels;

    frame_ = 1;
    playing_ = true;
    snapshotItems(0);
    syncChildrenTree(curItems_, 0);
    runSpriteFirstFrames(0);
    syncChildrenTree(curItems_, 0);
    buildClipPool();
    scriptFrame_ = 0;
    runFrameScript(0);
    fireClipEvents();   // 首帧：派发 ClipActions 的 Load/EnterFrame
    return true;
}

bool Player::loadFile(const char* path, std::string& err) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n < 0) { std::fclose(f); return false; }
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(n));
    bool ok = n == 0 || std::fread(buf.data(), 1, static_cast<std::size_t>(n), f) ==
                                     static_cast<std::size_t>(n);
    std::fclose(f);
    if (!ok) return false;
    return load(buf, err);
}

// 把 old 实例的运行时状态（含子树）搬到 d：同一 depth/角色的实例在显示列表逐帧重建时
// 视为同一实例，需保留 AVM1 SetProperty 覆盖、按钮态、EditText 编辑值、精灵播放头/停止标志、
// 以及整棵 children 子树（供下一层继续继承运行态）。
static void adoptRuntime(DisplayItem& d, const DisplayItem& old) {
    d.hasRt = old.hasRt;
    d.rtX = old.rtX; d.rtY = old.rtY;
    d.rtScaleX = old.rtScaleX; d.rtScaleY = old.rtScaleY;
    d.rtRotation = old.rtRotation;
    d.rtAlpha = old.rtAlpha; d.rtVisible = old.rtVisible;
    d.rtFrame = old.rtFrame; d.rtPlaying = old.rtPlaying;
    d.buttonState = old.buttonState;
    d.textOverride = old.textOverride; d.hasTextOverride = old.hasTextOverride;
    d.spInit = old.spInit; d.spStopped = old.spStopped;
    d.clipVars = old.clipVars;   // 剪辑作用域变量跨帧持久（与 clipInited 一同继承）
    d.clipInited = old.clipInited;
    d.children = old.children;   // 继承整棵子树旧运行态
}

// 从静态快照 src 构建运行态列表 out：同 (depth, characterId) 者继承 old 的运行态，
// 否则为**新放置**实例（运行态取默认，spInit=false 时会执行其首帧脚本）。
static void mergeInto(std::vector<DisplayItem>& out, const std::vector<DisplayItem>& src,
                      const std::vector<DisplayItem>& old) {
    out.clear();
    out.reserve(src.size());
    for (const auto& s : src) {
        bool merged = false;
        for (const auto& o : old) {
            if (o.depth == s.depth && o.characterId == s.characterId) {
                DisplayItem d = s;
                adoptRuntime(d, o);
                out.push_back(std::move(d));
                merged = true;
                break;
            }
        }
        if (!merged) out.push_back(s);
    }
    // 运行期动态实例（attachMovie/duplicateMovieClip/createEmptyMovieClip）不出现在
    // 静态帧模板里：显示列表每 tick 重建时必须原样保留，否则 attachMovie 只在当帧可见。
    for (const auto& o : old) {
        if (!o.dynamic) continue;
        const bool exists = std::any_of(out.begin(), out.end(),
            [&o](const DisplayItem& d) { return d.depth == o.depth; });
        if (!exists) out.push_back(o);
    }
    std::sort(out.begin(), out.end(),
              [](const DisplayItem& a, const DisplayItem& b) { return a.depth < b.depth; });
}

void Player::snapshotItems(int frameIndex) {
    if (frameIndex < 0 || frameIndex >= static_cast<int>(movie_.frames.size())) return;
    std::vector<DisplayItem> next;
    mergeInto(next, movie_.frames[static_cast<std::size_t>(frameIndex)].items, curItems_);
    curItems_ = std::move(next);
}

// 递归为每个精灵实例重建内部帧运行态子树。取帧用 itemFrameIndex（与渲染/命中一致），
// 以静态帧快照为模板，按 (depth, characterId) 从旧子树继承运行态；随后递归子项。
void Player::syncChildrenTree(std::vector<DisplayItem>& items, int rootFrame) {
    for (auto& it : items) {
        if (!it.character || it.character->kind != Character::Kind::Sprite) {
            it.children.clear();
            continue;
        }
        const auto& sf = it.character->spriteFrames;
        int idx = itemFrameIndex(it, rootFrame);
        if (sf.empty() || idx < 0 || idx >= static_cast<int>(sf.size())) {
            it.children.clear();
            continue;
        }
        std::vector<DisplayItem> next;
        mergeInto(next, sf[static_cast<std::size_t>(idx)].items, it.children);
        it.children = std::move(next);
        syncChildrenTree(it.children, rootFrame);
    }
}

void Player::runFrameScript(int frameIndex) {
    if (frameIndex < 0 || frameIndex >= static_cast<int>(movie_.frames.size())) return;
    auto& frame = movie_.frames[static_cast<std::size_t>(frameIndex)];
    for (const auto& sc : frame.doActions) avm1_.run(sc);
}

// 执行本帧显示列表中"新放置"精灵（spInit==false）的内部第 1 帧脚本（递归含嵌套精灵）。
// Flash 语义：实例化精灵即运行其首帧脚本；脚本里无目标的 stop() 会把该精灵停在首帧
// （效果精灵 /:busy、/:stick 等"开关"正是靠此机制在放置瞬间自我抑制、不外播）。
void Player::runSpriteFirstFrames(int frameIndex) {
    (void)frameIndex;
    initTree(curItems_, nullptr);
}

void Player::initTree(std::vector<DisplayItem>& items, ItemClip* parent) {
    for (auto& it : items) {
        if (!it.spInit && it.character && it.character->kind == Character::Kind::Sprite) {
            const auto& sf = it.character->spriteFrames;
            if (!sf.empty()) {
                it.spInit = true;
                // Flash：新建实例的播放头从内部第 0 帧开始（与主时间轴当前位置无关）。
                it.rtFrame = 0;
                it.rtPlaying = true;
                it.spStopped = false;
                SpriteClip clip(it, static_cast<int>(sf.size()), 0, this, parent);
                ItemClip self(this, &it, it.name, parent);
                avm1::ITimeline* prev = avm1_.currentClip;
                avm1::IClip* prevP = avm1_.currentParent;
                avm1::IClip* prevT = avm1_.currentThis;
                avm1_.currentClip = &clip;
                avm1_.currentParent = parent;      // 首帧脚本里的 _parent 指向所在上级剪辑
                avm1_.currentThis = &self;
                for (const auto& sc : sf[0].doActions) avm1_.run(sc);
                avm1_.currentClip = prev;
                avm1_.currentParent = prevP;
                avm1_.currentThis = prevT;
                // 精灵首帧的 StartSound（进入该帧即触发）。
                for (const auto& ev : sf[0].soundEvents) playSoundEvent(ev);
            }
        }
        ItemClip* self = clipForItem(&it);
        initTree(it.children, self ? self : parent);
    }
}

// 每个 MovieClip 实例独立推进播放头（递归含嵌套精灵）：进入新内部帧时执行该帧脚本。
// 这是"游戏在剪辑里循环播放"的驱动机制——即使主时间轴 stop() 停住，剪辑仍继续动。
void Player::advanceClips() {
    advanceTree(curItems_, nullptr);
}

void Player::advanceTree(std::vector<DisplayItem>& items, ItemClip* parent) {
    for (auto& it : items) {
        if (it.character && it.character->kind == Character::Kind::Sprite) {
            const auto& sf = it.character->spriteFrames;
            if (!sf.empty() && it.spInit && it.rtPlaying && !it.spStopped) {
                if (it.rtFrame < 0) it.rtFrame = 0;
                int n = static_cast<int>(sf.size());
                int nxt = (it.rtFrame + 1) % n;
                it.rtFrame = nxt;
                // 进入新的内部帧：先触发该帧的 StartSound（无论是否有脚本）。
                for (const auto& ev : sf[static_cast<std::size_t>(nxt)].soundEvents)
                    playSoundEvent(ev);
                const auto& acts = sf[static_cast<std::size_t>(nxt)].doActions;
                if (!acts.empty()) {
                    SpriteClip clip(it, n, nxt, this, parent);
                    ItemClip self(this, &it, it.name, parent);
                    avm1::ITimeline* prev = avm1_.currentClip;
                    avm1::IClip* prevP = avm1_.currentParent;
                    avm1::IClip* prevT = avm1_.currentThis;
                    avm1_.currentClip = &clip;
                    avm1_.currentParent = parent;   // 内部帧脚本里的 _parent 指向所在上级剪辑
                    avm1_.currentThis = &self;
                    for (const auto& sc : acts) avm1_.run(sc);
                    avm1_.currentClip = prev;
                    avm1_.currentParent = prevP;
                    avm1_.currentThis = prevT;
                }
            }
        }
        ItemClip* self = clipForItem(&it);
        advanceTree(it.children, self ? self : parent);
    }
}

// 递归把变量值写入绑定了同名 VariableName 的 EditText（AVM1 文本框随变量实时显示）。
// 文本框的 variable 绑定到"所在剪辑的作用域"（Flash 语义）：先在包含它的剪辑的局部变量表里查，
// 再回落到根变量表（"/:x"、"_root.x" 这类显式根名只查根表）。scope 为最近外层精灵的 clipVars。
static void syncTextInItems(const std::map<std::string, avm1::Value>& globals,
                            std::vector<DisplayItem>& items,
                            const std::map<std::string, avm1::Value>* scope) {
    for (auto& it : items) {
        if (!it.character) continue;
        if (it.character->kind == Character::Kind::EditText && it.character->editText) {
            const std::string& vn = it.character->editText->variableName;
            if (vn.empty()) continue;
            const avm1::Value* v = nullptr;
            if (!avm1::isRootVarName(vn) && scope) v = avm1::findVar(*scope, vn);
            if (!v) v = avm1::findVar(globals, vn);
            if (v) { it.textOverride = v->toString(); it.hasTextOverride = true; }
        } else if (it.character->kind == Character::Kind::Sprite) {
            syncTextInItems(globals, it.children, it.clipVars.get());   // 递归运行态子树
        }
    }
}

void Player::syncTextFields() {
    syncTextInItems(avm1_.globals, curItems_, nullptr);
}

void Player::tick() {
    // 1) 推进播放头：优先消费待处理的 gotoAndPlay/gotoAndStop；否则按 playing_ 前进。
    //    主时间轴到末帧后回到第 1 帧（MovieClip 默认循环）。
    if (pendingGoto_.active) {
        int t = pendingGoto_.frame;
        if (t < 1) t = 1;
        if (t > totalFrames()) t = totalFrames();
        frame_ = t;
        playing_ = pendingGoto_.play;
        pendingGoto_.active = false;
    } else if (playing_) {
        frame_ = (frame_ >= totalFrames()) ? 1 : frame_ + 1;
    }
    int f = frame_ - 1;
    snapshotItems(f);
    // 建立/刷新显示列表的精灵运行态子树（递归含嵌套精灵）。
    syncChildrenTree(curItems_, f);
    // 剪辑池必须与当前显示列表同步后才能执行脚本：`_root.<name>` 等解析依赖它。显示列表
    // 每 tick 都重建（DisplayItem 地址改变），若此处不重建，剪辑池中的 item_ 指针将指向
    // 上一 tick 已释放的内存，导致跨剪辑调用（如 `_root.C.gotoAndPlay()`）写入悬垂对象而丢失。
    buildClipPool();
    // 2a) 已存在的 MovieClip：独立播放头前进一帧并执行其内部帧脚本。
    advanceClips();
    // 2b) 新放置的精灵：执行其内部第 1 帧脚本（先于所在帧自身的 DoAction）。
    runSpriteFirstFrames(f);
    // 推进/初始化后按新播放头重建子树，使渲染与命中与本帧最终状态一致。
    syncChildrenTree(curItems_, f);
    buildClipPool();
    // startDrag 每帧把鼠标位置落到目标实例的运行态变换上。
    if (ext_) ext_->applyDrag(makeExtContext());
    // 3) 进入帧时执行一次该帧脚本（AVM1 的跳转只登记为待处理、下一帧生效，
    //    因此脚本不会重复执行，gotoAndPlay 也不会因越帧而跳过目标帧的脚本）。
    if (scriptFrame_ != f) {
        scriptFrame_ = f;
        runFrameScript(f);
    }
    // 4) ClipActions：放置时（首见）派发 Load/Initialize，随后每 tick 派发 EnterFrame。
    //    游戏抓取/移动/计分全靠各剪辑的 onClipEvent(enterFrame) 驱动。
    fireClipEvents();

    // 官方启动时间线：加载页 -> 品牌门户页 -> 游戏。前两段只推进 preloader
    // 副本的时间轴，主游戏 Vm 保持初始状态；门户阶段结束后再从第 1 步继续，
    // 内容不变、只是整体延后了这段启动时间。
    if (loadingElapsed_ < loadingTotal_) {
        for (auto& vm : loadingVms_) vm->tick();
        ++loadingElapsed_;
        updateLoadingProgress();
    } else if (portalElapsed_ < portalTotal_) {
        if (!portalStarted_) {
            portalStarted_ = true;
            if (preloaderRoot_ && !loadingVms_.empty())
                loadingVms_.front()->setClipFrame(preloaderRoot_, 1);
        }
        for (auto& vm : loadingVms_) vm->tick();
        ++portalElapsed_;
    } else {
        for (auto& vm : vms_) vm->tick();
    }

    const auto& frame = movie_.frames[static_cast<std::size_t>(f)];
    for (const auto& ev : frame.soundEvents) {
        if (ev.streamBlock >= 0 &&
            ev.streamBlock < static_cast<int>(movie_.streamAudio.blocks.size())) {
            const auto& blk = movie_.streamAudio.blocks[static_cast<std::size_t>(ev.streamBlock)];
            Sound tmp;
            tmp.format = movie_.streamAudio.format;
            tmp.rate = movie_.streamAudio.rate;
            tmp.bits = movie_.streamAudio.bits;
            tmp.channels = movie_.streamAudio.channels;
            tmp.data = blk;
            PcmData pcm;
            std::string err;
            if (decodeSound(tmp, pcm, err)) {
                audio_.open(pcm.rate, pcm.channels);
                audio_.queue(pcm.samples.data(), pcm.samples.size(), false);
            } else {
                fprintf(stderr, "stream decode fail: %s\n", err.c_str());
            }
        } else if (ev.soundId != 0) {
            playSoundEvent(ev);
        }
    }

}

bool Player::render(std::vector<std::uint8_t>& rgba, int& W, int& H,
                    int targetW, int targetH) {
    syncTextFields();
    if (loadingElapsed_ < loadingTotal_ || portalElapsed_ < portalTotal_) {
        auto saved = std::move(movie_.runtimeStage);
        movie_.runtimeStage = firstFrameStage_;
        bool ok = renderFrameWithItems(movie_, curItems_, 0, rgba, W, H, targetW, targetH);
        movie_.runtimeStage = std::move(saved);
        return ok;
    }
    return renderFrameWithItems(movie_, curItems_, frame_ - 1, rgba, W, H,
                                targetW, targetH);
}

void Player::play() {
    playing_ = true;
    if (pendingGoto_.active) pendingGoto_.play = true;   // 后执行者胜出
}
void Player::stop() {
    playing_ = false;
    if (pendingGoto_.active) pendingGoto_.play = false;
}
void Player::gotoAndPlay(int frame) {
    if (frame < 1) frame = 1;
    if (frame > totalFrames()) frame = totalFrames();
    pendingGoto_ = {true, frame, true};   // 下一 tick 生效并执行目标帧脚本
}
void Player::gotoAndStop(int frame) {
    if (frame < 1) frame = 1;
    if (frame > totalFrames()) frame = totalFrames();
    pendingGoto_ = {true, frame, false};
}
void Player::nextFrame() {
    if (frame_ < totalFrames()) gotoAndStop(frame_ + 1);   // Flash：前进一帧并停住
}
void Player::prevFrame() {
    if (frame_ > 1) gotoAndStop(frame_ - 1);
}
void Player::stopAllSounds() { audio_.stopAll(); }

static bool isButtonItem(const DisplayItem& it) {
    return it.character && it.character->kind == Character::Kind::Button && it.character->button;
}

// 执行按钮实例在给定条件位上的 AVM1 动作（找不到则无操作）。
// 注意：Decoder 以"条件位序号"(0..15) 为键，不是掩码。
// container 为包含该按钮的显示项（顶层按钮为 nullptr）；动作以该剪辑为作用域执行，
// _parent 指向该剪辑的上级——与 Flash 中按钮处理器的作用域一致。
void Player::fireButtonEvent(DisplayItem* btn, DisplayItem* container, int condBit) {
    if (!btn || !isButtonItem(*btn)) return;
    const ButtonDef& bd = *btn->character->button;
    // DefineButtonSound：按状态迁移播放对应音效（与是否有脚本无关）。
    std::uint16_t sid = 0;
    switch (condBit) {
        case 0: sid = bd.soundUp; break;     // IdleToOverUp
        case 1: sid = bd.soundOut; break;    // OverUpToIdle
        case 2: sid = bd.soundOver; break;   // OverUpToOverDown
        case 3: sid = bd.soundDown; break;   // OverDownToOverUp
        default: break;
    }
    if (sid) {
        SoundEvent ev;
        ev.soundId = sid;
        playSoundEvent(ev);
    }
    auto found = bd.actions.find(condBit);
    if (found == bd.actions.end()) return;
    runButtonAction(container, container ? clipForItem(container) : nullptr, found->second);
}

// 按 (depth, characterId) 链在当前显示列表（含嵌套精灵子树）里解析显示项。
DisplayItem* Player::resolveItemChain(const std::vector<std::pair<int, int>>& chain,
                                      DisplayItem** container) const {
    if (container) *container = nullptr;
    if (chain.empty()) return nullptr;
    auto findAt = [](std::vector<DisplayItem>& level, int depth, int cid) -> DisplayItem* {
        for (auto& it : level)
            if (static_cast<int>(it.depth) == depth && static_cast<int>(it.characterId) == cid)
                return &it;
        return nullptr;
    };
    std::vector<DisplayItem>* level = const_cast<std::vector<DisplayItem>*>(&curItems_);
    DisplayItem* found = nullptr;
    DisplayItem* parent = nullptr;
    for (std::size_t i = 0; i < chain.size(); ++i) {
        DisplayItem* hit = findAt(*level, chain[i].first, chain[i].second);
        if (!hit) return nullptr;
        found = hit;
        if (i + 1 < chain.size()) {
            if (!hit->character || hit->character->kind != Character::Kind::Sprite) return nullptr;
            level = &hit->children;
            parent = hit;
        }
    }
    if (container) *container = parent;
    return found;
}

void Player::setMouse(double x, double y) {
    mouseX_ = x; mouseY_ = y;
    pollMouseEvents(mouseDown_);
    // AVM2：运行时显示树的命中与事件派发由 Vm 维护，同一舞台坐标需同步转交。
    for (auto& vm : vms_) vm->setMousePosition(x, y);
}
void Player::mouseDown() {
    mouseDown_ = true;
    pollMouseEvents(true);
    for (auto& vm : vms_) vm->mouseDown(mouseX_, mouseY_);
}
void Player::mouseUp() {
    if (mouseDown_) pollMouseEvents(false);
    mouseDown_ = false;
    if (ext_) ext_->endDrag();  // Flash 语义：松开鼠标即结束 startDrag
    for (auto& vm : vms_) vm->mouseUp(mouseX_, mouseY_);
}
// 光标形状与 Flash 对齐：悬停目标是按钮时用系统手形光标（IDC_HAND）。
bool Player::handCursor() const {
    if (hoverChain_.empty()) return false;
    DisplayItem* container = nullptr;
    DisplayItem* leaf = resolveItemChain(hoverChain_, &container);
    return leaf && isButtonItem(*leaf);
}
void Player::keyDown(int vk) {
    int kc = vkToKeyPressCode(vk);
    if (kc <= 0) return;
    fireKeyButtons(curItems_, nullptr, nullptr, kc);
}
void Player::keyUp(int vk) { (void)vk; }

void Player::pollMouseEvents(bool down) {
    // 递归命中解析：链条自顶层到最上层可交互项（会深入精灵内部，定位嵌套按钮）。
    std::vector<const DisplayItem*> chain;
    hitTestItemsDeep(movie_, curItems_, mouseX_, mouseY_, frame_ - 1, chain);

    auto keysOf = [](const std::vector<const DisplayItem*>& c) {
        std::vector<std::pair<int, int>> k;
        k.reserve(c.size());
        for (auto* p : c) k.emplace_back(static_cast<int>(p->depth), static_cast<int>(p->characterId));
        return k;
    };
    auto leafButton = [](const std::vector<const DisplayItem*>& c) -> const DisplayItem* {
        return (!c.empty() && isButtonItem(*c.back())) ? c.back() : nullptr;
    };

    const auto newChain = keysOf(chain);

    // 1) 悬停迁移：rollOut -> 恢复 Up 态；rollOver -> 切 Over 态
    if (newChain != hoverChain_) {
        DisplayItem* oldContainer = nullptr;
        DisplayItem* old = resolveItemChain(hoverChain_, &oldContainer);
        if (old && isButtonItem(*old)) {
            old->buttonState = 0;
            fireButtonEvent(old, oldContainer, 1);     // bit1 OverUpToIdle（rollOut）
        }
        hoverChain_ = newChain;
        DisplayItem* newContainer = nullptr;
        DisplayItem* leaf = resolveItemChain(hoverChain_, &newContainer);
        if (leaf && isButtonItem(*leaf)) {
            leaf->buttonState = 1;
            fireButtonEvent(leaf, newContainer, 0);    // bit0 IdleToOverUp（rollOver）
        }
    }

    // 2) 按下 / 抬起
    if (down) {
        if (leafButton(chain) && pressChain_ != newChain) {
            DisplayItem* container = nullptr;
            DisplayItem* leaf = resolveItemChain(newChain, &container);
            pressChain_ = newChain;
            if (leaf && isButtonItem(*leaf)) {
                leaf->buttonState = 2;
                fireButtonEvent(leaf, container, 2);   // bit2 OverUpToOverDown（press）
            }
        }
    } else if (!pressChain_.empty()) {
        DisplayItem* container = nullptr;
        DisplayItem* leaf = resolveItemChain(pressChain_, &container);
        const bool over = (pressChain_ == newChain);
        pressChain_.clear();
        if (leaf && isButtonItem(*leaf)) {
            leaf->buttonState = over ? 1 : 0;
            fireButtonEvent(leaf, container, over ? 3 : 4);   // bit3 release / bit4 releaseOutside
        }
    }
}

void Player::playSoundEvent(const SoundEvent& ev) {
    if (ev.soundId == 0) return;
    auto it = movie_.sounds.find(ev.soundId);
    if (it == movie_.sounds.end()) return;
    const Sound& s = it->second;
    PcmData pcm;
    std::string err;
    if (!decodeSound(s, pcm, err)) {
        fprintf(stderr, "decodeSound fail id=%u %s\n", ev.soundId, err.c_str());
        return;
    }
    // 首次播放时才打开 waveOut（此前从未调用 open，导致后台线程空转、完全无声）。
    audio_.open(pcm.rate, pcm.channels);
    audio_.queue(pcm.samples.data(), pcm.samples.size(), ev.loop);
}

void* Player::findChildClip(const std::string& name) {
    for (auto& c : clipPool_) {
        if (c->clipNameStr() == name) return static_cast<void*>(c.get());
    }
    return nullptr;
}

// ---- 路径解析 / 包围盒命中（MovieClip.hitTest 与 GetVariable 路径用）----
// 返回某显示项在给定父变换下的世界变换（等价 Render::hitItem 的合成规则）。
static Matrix worldOfItem(const Matrix& parent, const DisplayItem& it) {
    if (it.hasRt) {
        double rad = it.rtRotation * 3.141592653589793 / 180.0;
        double ca = std::cos(rad), sa = std::sin(rad);
        Matrix rt;
        rt.sx = static_cast<float>(it.rtScaleX * ca);
        rt.r0 = static_cast<float>(-it.rtScaleX * sa);
        rt.r1 = static_cast<float>(it.rtScaleY * sa);
        rt.sy = static_cast<float>(it.rtScaleY * ca);
        rt.tx = static_cast<float>(it.rtX * 20.0);
        rt.ty = static_cast<float>(it.rtY * 20.0);
        return parent.post(rt);
    }
    return parent.post(it.matrix);
}

// 精灵运行态子树（无则退化为静态帧当前帧的项）。
static const std::vector<DisplayItem>* spriteChildItems(const DisplayItem& it, int rootFrame) {
    if (!it.children.empty()) return &it.children;
    const auto& sf = it.character->spriteFrames;
    if (sf.empty()) return nullptr;
    int n = static_cast<int>(sf.size());
    int idx;
    if (it.rtFrame >= 0) {
        idx = it.rtFrame % n;
    } else if (!it.spInit) {
        // 未初始化的新精灵：本帧首次出现（在第二次 syncChildrenTree 才加入），
        // 尚未执行 initTree，rtFrame 未设置 → 强制从第 0 帧开始渲染，下一 tick 会正常初始化。
        // 修复"伸钩先变长一闪"和"待机重叠发黑"：避免回退 rootFrame-birthFrame 得到错误大 idx。
        idx = 0;
    } else if (it.spStopped) {
        idx = 0;   // 首帧脚本 stop()：停在内部第 0 帧
    } else {
        idx = rootFrame - it.birthFrame;
        if (idx < 0) idx = 0;
        idx %= n;
    }
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;
    return &sf[static_cast<std::size_t>(idx)].items;
}

static void aabbUnion(const DisplayItem& it, const Matrix& world, int rootFrame,
                      bool& any, double& x0, double& y0, double& x1, double& y1, int guard) {
    if (!it.character || guard > 24) return;
    auto addCorner = [&](const Matrix& w, double lx, double ly) {
        float ox = 0, oy = 0; w.apply(static_cast<float>(lx), static_cast<float>(ly), ox, oy);
        if (!any) { x0 = x1 = ox; y0 = y1 = oy; any = true; }
        else { x0 = std::min(x0, (double)ox); y0 = std::min(y0, (double)oy);
               x1 = std::max(x1, (double)ox); y1 = std::max(y1, (double)oy); }
    };
    auto addRect = [&](const Matrix& w, const RectB& r) {
        addCorner(w, r.x0, r.y0); addCorner(w, r.x1, r.y0);
        addCorner(w, r.x0, r.y1); addCorner(w, r.x1, r.y1);
    };
    const auto& ch = *it.character;
    switch (ch.kind) {
        case Character::Kind::Shape:  if (ch.shape) addRect(world, ch.shape->bounds); break;
        case Character::Kind::Morph:  if (ch.morph && ch.morph->start) addRect(world, ch.morph->start->bounds); break;
        case Character::Kind::Bitmap: if (ch.bitmap) { RectB r{0, 0, (std::int32_t)(ch.bitmap->width * 20), (std::int32_t)(ch.bitmap->height * 20)}; addRect(world, r); } break;
        case Character::Kind::Video:  if (ch.video)  { RectB r{0, 0, (std::int32_t)(ch.video->width * 20),  (std::int32_t)(ch.video->height * 20)};  addRect(world, r); } break;
        case Character::Kind::EditText: if (ch.editText) addRect(world, ch.editText->bounds); break;
        case Character::Kind::Button: {
            if (!ch.button) break;
            const auto& bucket = !ch.button->hit.empty() ? ch.button->hit : ch.button->up;
            for (const auto& sub : bucket)
                aabbUnion(sub, sub.hasRt ? worldOfItem(world, sub) : world.post(sub.matrix), rootFrame, any, x0, y0, x1, y1, guard + 1);
            break;
        }
        case Character::Kind::Sprite: {
            const auto* kids = spriteChildItems(it, rootFrame);
            if (!kids) break;
            for (const auto& sub : *kids)
                aabbUnion(sub, worldOfItem(world, sub), rootFrame, any, x0, y0, x1, y1, guard + 1);
            break;
        }
        default: break;
    }
}

bool Player::itemStageAABB(const DisplayItem& target, double& x0, double& y0, double& x1, double& y1) const {
    // 主时间轴 -> 设备像素的根变换（与 Render::hitTestItems 一致）。
    Matrix root;
    root.sx = 1.0f / 20.0f; root.sy = 1.0f / 20.0f;
    root.tx = -movie_.frameSize.x0 / 20.0f;
    root.ty = -movie_.frameSize.y0 / 20.0f;
    int rf = frame_ - 1;
    bool found = false, any = false;
    // 自根向下逐项累计变换，命中目标（指针相等）后收集其子树包围盒。
    std::function<void(const std::vector<DisplayItem>&, const Matrix&, int)> walk =
        [&](const std::vector<DisplayItem>& items, const Matrix& parent, int guard) {
            if (found || guard > 24) return;
            for (const auto& it : items) {
                if (!it.character) continue;
                Matrix world = worldOfItem(parent, it);
                if (&it == &target) { aabbUnion(it, world, rf, any, x0, y0, x1, y1, 0); found = true; return; }
                if (it.character->kind != Character::Kind::Sprite) continue;
                const auto* kids = spriteChildItems(it, rf);
                if (kids) {
                    // 注意：递归需用可写指针找到目标，但这里 items 为 const 引用；用 const_cast 仅用于比较指针。
                    walk(*kids, world, guard + 1);
                }
            }
        };
    walk(curItems_, root, 0);
    return found && any;
}

DisplayItem* Player::findItemByPath(const std::string& path, DisplayItem* base, ItemClip* baseClip) {
    (void)baseClip;
    std::string p = path;
    bool absolute = true;
    if (p.rfind("_root.", 0) == 0) p = p.substr(6);
    else if (p.rfind("_level0.", 0) == 0) p = p.substr(8);
    else if (!p.empty() && p[0] == '/') { if (p.size() > 1) p = p.substr(1); }
    else absolute = false;
    // 切分为实例名段（忽略空段）。
    std::vector<std::string> segs; std::string cur;
    for (char c : p) { if (c == '.') { if (!cur.empty()) { segs.push_back(cur); cur.clear(); } } else cur.push_back(c); }
    if (!cur.empty()) segs.push_back(cur);
    if (segs.empty()) return nullptr;

    std::vector<DisplayItem>* level = nullptr;
    if (absolute) level = &curItems_;
    else if (base && base->character && base->character->kind == Character::Kind::Sprite)
        level = const_cast<std::vector<DisplayItem>*>(spriteChildItems(*base, frame_ - 1));
    if (!level) return nullptr;

    DisplayItem* found = nullptr;
    for (std::size_t i = 0; i < segs.size(); ++i) {
        DisplayItem* hit = nullptr;
        for (auto& it : *level) if (it.name == segs[i]) { hit = &it; break; }
        if (!hit) return nullptr;
        found = hit;
        if (i + 1 < segs.size()) {
            if (!hit->character || hit->character->kind != Character::Kind::Sprite) return nullptr;
            level = const_cast<std::vector<DisplayItem>*>(spriteChildItems(*hit, frame_ - 1));
            if (!level) return nullptr;
        }
    }
    return found;
}

void* Player::resolveClipPath(const std::string& path) {
    DisplayItem* it = findItemByPath(path, nullptr, nullptr);
    return it ? static_cast<void*>(clipForItem(it)) : nullptr;
}

// ClipActions 事件派发：对显示列表（含嵌套精灵）中每个带 clipEvents 的实例，
// 首次（clipInited 未置）派发 Load/Initialize 一次，随后每次都派发 EnterFrame。
// 上下文：currentThis=该实例的 IClip（其 members 即实例属性），_parent=其父剪辑，
// currentClip=该实例自身的时间轴（精灵时）。
void Player::fireClipEvents() {
    std::function<void(std::vector<DisplayItem>&, ItemClip*, int)> walk =
        [&](std::vector<DisplayItem>& items, ItemClip* parent, int guard) {
            if (guard > 24) return;
            for (auto& it : items) {
                ItemClip* self = clipForItem(&it);
                if (!it.clipEvents.empty()) {
                    auto runEv = [&](std::uint32_t evBit) {
                        auto f = it.clipEvents.find(evBit);
                        if (f == it.clipEvents.end() || f->second.actions.empty()) return;
                        bool isSprite = it.character && it.character->kind == Character::Kind::Sprite;
                        std::unique_ptr<SpriteClip> hold;
                        avm1::ITimeline* selfTL = nullptr;
                        if (isSprite) {
                            int n = static_cast<int>(it.character->spriteFrames.size());
                            if (n > 0) {
                                int idx = itemFrameIndex(it, frame_ - 1);
                                hold = std::make_unique<SpriteClip>(it, n, idx < 0 ? 0 : idx, this, parent);
                                selfTL = hold.get();
                            }
                        }
                        avm1::ITimeline* prevClip = avm1_.currentClip;
                        avm1::IClip* prevParent = avm1_.currentParent;
                        avm1::IClip* prevThis = avm1_.currentThis;
                        avm1_.currentClip = selfTL;
                        avm1_.currentParent = parent;   // _parent = 该实例所在的上级剪辑
                        avm1_.currentThis = self;
                        avm1_.run(f->second);
                        avm1_.currentClip = prevClip;
                        avm1_.currentParent = prevParent;
                        avm1_.currentThis = prevThis;
                    };
                    if (!it.clipInited) {
                        it.clipInited = true;
                        runEv(AVM1_EVENT_LOAD);
                        runEv(AVM1_EVENT_INITIALIZE);
                    }
                    runEv(AVM1_EVENT_ENTER_FRAME);
                }
                if (it.character && it.character->kind == Character::Kind::Sprite && !it.children.empty()) {
                    walk(it.children, self ? self : parent, guard + 1);
                }
            }
        };
    walk(curItems_, nullptr, 0);
}

bool Player::getNumberProp(int propId, double& out) {
    (void)propId; (void)out;
    return false;
}
void Player::setNumberProp(int propId, double v) {
    (void)propId; (void)v;
}
bool Player::getBoolProp(int propId, bool& out) {
    (void)propId; (void)out;
    return false;
}
void Player::setBoolProp(int propId, bool v) { (void)propId; (void)v; }
bool Player::getStringProp(int propId, std::string& out) {
    (void)propId; (void)out;
    return false;
}

// 计算某显示项当前正在显示的内部帧下标（0-based）。与渲染器/文本同步的取帧规则一致。
int Player::itemFrameIndex(const DisplayItem& it, int rootFrame) const {
    if (!it.character || it.character->kind != Character::Kind::Sprite) return -1;
    int n = static_cast<int>(it.character->spriteFrames.size());
    if (n <= 0) return -1;
    int idx;
    if (it.rtFrame >= 0) idx = it.rtFrame % n;
    else if (!it.spInit) idx = 0;   // 新精灵尚未初始化：按首帧渲染（见 spriteChildItems 说明）
    else if (it.spStopped) idx = 0;
    else idx = (rootFrame - it.birthFrame) % n;
    if (idx < 0) idx = 0;
    return idx;
}

Player::ItemClip* Player::clipForItem(const DisplayItem* it) const {
    auto f = clipByItem_.find(it);
    return f == clipByItem_.end() ? nullptr : f->second;
}

void Player::buildClipPool() {
    clipPool_.clear();
    clipByItem_.clear();
    collectClips(curItems_, nullptr, "/");
}

void Player::collectClips(std::vector<DisplayItem>& items, ItemClip* parent,
                          const std::string& pathPrefix) {
    for (auto& it : items) {
        bool isSprite = it.character && it.character->kind == Character::Kind::Sprite;
        ItemClip* self = parent;
        std::string path = pathPrefix + it.name;
        if (!it.name.empty() || isSprite) {
            auto c = std::make_unique<ItemClip>(this, &it, it.name, parent);
            c->setPath(path);
            self = c.get();
            clipByItem_[&it] = c.get();
            clipPool_.push_back(std::move(c));
        }
        if (isSprite) collectClips(it.children, self, path + ".");   // 递归运行态子树
    }
}

void Player::runClipFrameScript(DisplayItem& it, int idx, avm1::IClip* parent) {
    runClipFrame(it, idx, static_cast<ItemClip*>(parent));
}

// 在指定精灵剪辑上下文中执行其内部第 idx 帧脚本（供 gotoAndPlay/Stop 触发目标帧脚本用）。
void Player::runClipFrame(DisplayItem& it, int idx, ItemClip* parent) {
    if (!it.character || it.character->kind != Character::Kind::Sprite) return;
    auto& sf = it.character->spriteFrames;
    if (idx < 0 || idx >= static_cast<int>(sf.size())) return;
    // 进入新帧即触发该帧的 StartSound（与主时间轴、advanceTree 的语义一致）。
    // 脚本驱动的剪辑（gotoAndStop/nextFrame/play）不经过 advanceTree，之前会漏掉音效，
    // 例如钩子抓取、炸药爆炸、获取力量等声音都挂在被脚本跳转的精灵帧上。
    for (const auto& ev : sf[static_cast<std::size_t>(idx)].soundEvents) playSoundEvent(ev);
    if (sf[static_cast<std::size_t>(idx)].doActions.empty()) return;
    SpriteClip clip(it, static_cast<int>(sf.size()), idx, this, parent);
    avm1::ITimeline* prevClip = avm1_.currentClip;
    avm1::IClip* prevParent = avm1_.currentParent;
    avm1_.currentClip = &clip;
    avm1_.currentParent = parent;
    ItemClip self(this, &it, it.name, parent);
    avm1::IClip* prevThis = avm1_.currentThis;
    avm1_.currentThis = &self;
    for (const auto& sc : sf[static_cast<std::size_t>(idx)].doActions) avm1_.run(sc);
    avm1_.currentClip = prevClip;
    avm1_.currentParent = prevParent;
    avm1_.currentThis = prevThis;
}

// 命名实例剪辑的时间轴方法：把 gotoAndPlay/gotoAndStop 的（帧号或帧标签）解析到该精灵内部帧。
void Player::ItemClip::callTimeline(const std::string& method, const std::vector<avm1::Value>& args) {
    if (!item_) return;
    if (method == "stop") { item_->rtPlaying = false; item_->spStopped = true; return; }
    if (method == "play") {
        item_->rtPlaying = true;
        item_->spStopped = false;
        if (item_->rtFrame < 0) item_->rtFrame = 0;
        return;
    }
    if (method == "stopAllSounds") { owner_->stopAllSounds(); return; }
    if (!item_->character || item_->character->kind != Character::Kind::Sprite) return;
    int n = static_cast<int>(item_->character->spriteFrames.size());
    if (n <= 0) return;
    int cur = owner_->itemFrameIndex(*item_, owner_->frame_ - 1);   // 0-based
    if (cur < 0) cur = 0;
    int target = -1;
    bool play = false;
    if (method == "gotoAndPlay" || method == "gotoAndStop") {
        play = (method == "gotoAndPlay");
        if (!args.empty()) {
            if (args[0].type == avm1::Value::T::Number) target = static_cast<int>(args[0].toNumber());
            else if (args[0].type == avm1::Value::T::String) {
                auto it = item_->character->frameLabels.find(args[0].str);
                if (it != item_->character->frameLabels.end()) target = it->second;
            }
        }
    } else if (method == "nextFrame") { target = cur + 2; }
    else if (method == "prevFrame") { target = cur; }
    else return;
    if (target < 1) target = 1;
    if (target > n) target = n;
    // Flash 语义：只有播放头真正改变了帧（重新"进入"目标帧）才执行该帧动作。
    // 若目标帧就是当前帧（典型：剪辑已被停在该帧，脚本又反复 gotoAndStop 到同一帧），
    // 不会重复执行帧脚本——否则像本 SWF 中 cl 每次被回收驱动器推到末帧时都会重新触发计分。
    const bool frameChanged = (target - 1) != cur;
    item_->rtFrame = target - 1;
    item_->rtPlaying = play;
    item_->spStopped = !play;
    if (frameChanged) owner_->runClipFrame(*item_, target - 1, parent_);
}

// keyPress 按钮派发：递归遍历显示列表（含嵌套精灵），对匹配该键码的按钮执行其 AVM1 动作。
void Player::fireKeyButtons(std::vector<DisplayItem>& items, DisplayItem* parentItem,
                            ItemClip* parentClip, int keyCode) {
    for (auto& it : items) {
        if (isButtonItem(it)) {
            auto f = it.character->button->keyActions.find(keyCode);
            if (f != it.character->button->keyActions.end()) {
                runButtonAction(parentItem, parentClip, f->second);
            }
        }
        if (it.character && it.character->kind == Character::Kind::Sprite) {
            ItemClip* self = clipForItem(&it);
            fireKeyButtons(it.children, &it, self, keyCode);   // 递归运行态子树
        }
    }
}

// 执行按钮动作：上下文为该按钮**所在剪辑**（无目标 stop()/play() 与无前缀变量均作用于它），
// 而 _parent 指向该剪辑的上级——与 Flash 中按钮处理器的作用域一致。
void Player::runButtonAction(DisplayItem* parentItem, ItemClip* parentClip, const avm1::Script& sc) {
    avm1::ITimeline* prevClip = avm1_.currentClip;
    avm1::IClip* prevParent = avm1_.currentParent;
    avm1::IClip* prevThis = avm1_.currentThis;
    std::unique_ptr<SpriteClip> hold;
    if (parentItem && parentItem->character && parentItem->character->kind == Character::Kind::Sprite) {
        int n = static_cast<int>(parentItem->character->spriteFrames.size());
        if (n > 0) {
            int idx = itemFrameIndex(*parentItem, frame_ - 1);
            hold = std::make_unique<SpriteClip>(*parentItem, n, idx < 0 ? 0 : idx, this, parentClip);
        }
    }
    avm1_.currentClip = hold ? static_cast<avm1::ITimeline*>(hold.get()) : nullptr;
    avm1_.currentParent = parentClip ? static_cast<avm1::IClip*>(parentClip->parentClip()) : nullptr;
    avm1_.currentThis = parentClip ? static_cast<avm1::IClip*>(parentClip) : nullptr;
    avm1_.run(sc);
    avm1_.currentClip = prevClip;
    avm1_.currentParent = prevParent;
    avm1_.currentThis = prevThis;
}

}  // namespace swf
