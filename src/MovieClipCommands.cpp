#include "swf/MovieClipCommands.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace swf {

namespace {

using Args = MovieClipCommands::Args;

std::string argString(const Args& a, std::size_t i, std::string def = {}) {
    if (i >= a.size() || a[i].type != avm1::Value::T::String) return def;
    return a[i].str;
}

int argInt(const Args& a, std::size_t i, int def = 0) {
    if (i >= a.size() || a[i].type != avm1::Value::T::Number) return def;
    if (!std::isfinite(a[i].num) || a[i].num < -32768.0 || a[i].num > 65535.0) return def;
    return static_cast<int>(a[i].num);
}

avm1::Value makeObject() {
    avm1::Value v;
    v.type = avm1::Value::T::Object;
    v.obj = std::make_shared<std::map<std::string, avm1::Value>>();
    return v;
}

avm1::Value namedClip(const std::string& name) {
    avm1::Value v = makeObject();
    (*v.obj)["_name"] = avm1::Value(name);
    return v;
}

void insertByDepth(std::vector<DisplayItem>& items, DisplayItem d) {
    auto it = std::lower_bound(items.begin(), items.end(), d.depth,
        [](const DisplayItem& x, std::uint16_t dep) { return x.depth < dep; });
    if (it != items.end() && it->depth == d.depth) *it = std::move(d);
    else items.insert(it, std::move(d));
}

// 路径只取最后一段实例名（阶段 1 支持主时间轴一层；嵌套路径在阶段 2 扩展）。
std::string lastSegment(const std::string& path) {
    const std::size_t dot = path.rfind('.');
    std::string s = (dot == std::string::npos) ? path : path.substr(dot + 1);
    if (!s.empty() && s[0] == '/') s.erase(0, 1);
    return s;
}

DisplayItem* findByName(std::vector<DisplayItem>& items, const std::string& name) {
    for (auto& it : items) if (it.name == name) return &it;
    return nullptr;
}

DisplayItem* lastDynamic(std::vector<DisplayItem>& items) {
    for (auto it = items.rbegin(); it != items.rend(); ++it)
        if (it->dynamic) return &*it;
    return nullptr;
}

DisplayItem makeDynamicItem(const ExtCommandContext& ctx, const std::string& name, int depth) {
    DisplayItem d;
    d.depth = static_cast<std::uint16_t>(depth);
    d.name = name;
    d.dynamic = true;
    d.birthFrame = ctx.frame - 1;
    d.spInit = false;
    d.clipVars = std::make_shared<std::map<std::string, avm1::Value>>();
    return d;
}

}  // namespace

struct MovieClipCommands::Impl {
    bool active = false;
    bool lockCenter = true;
    double offX = 0.0, offY = 0.0;
    std::string path;
    bool bounded = false;
    double x1 = 0.0, y1 = 0.0, x2 = 0.0, y2 = 0.0;
};

MovieClipCommands::MovieClipCommands() : impl_(std::make_unique<Impl>()) {
    registry_.setUnknown(avm1::Value::undef());
    registry_.add("attachMovie", [this](const ExtCommandContext& c, const Args& a) {
        return cmdAttachMovie(c, a);
    });
    registry_.add("duplicateMovieClip", [this](const ExtCommandContext& c, const Args& a) {
        return cmdDuplicateMovieClip(c, a);
    });
    registry_.add("createEmptyMovieClip", [this](const ExtCommandContext& c, const Args& a) {
        return cmdCreateEmptyMovieClip(c, a);
    });
    registry_.add("startDrag", [this](const ExtCommandContext& c, const Args& a) {
        return cmdStartDrag(c, a);
    });
    registry_.add("stopDrag", [this](const ExtCommandContext& c, const Args& a) {
        return cmdStopDrag(c, a);
    });
    registry_.add("getBounds", [this](const ExtCommandContext& c, const Args& a) {
        return cmdGetBounds(c, a);
    });
    registry_.add("setMask", [this](const ExtCommandContext& c, const Args& a) {
        return cmdSetMask(c, a);
    });
}

MovieClipCommands::~MovieClipCommands() = default;

avm1::Value MovieClipCommands::call(const std::string& method,
                                   const ExtCommandContext& ctx, const Args& args) {
    return registry_.dispatch(method, ctx, args);
}

avm1::Value MovieClipCommands::cmdAttachMovie(const ExtCommandContext& ctx, const Args& args) {
    const std::string link = argString(args, 0);
    const std::string name = argString(args, 1);
    const int depth = argInt(args, 2, -1);
    if (link.empty() || name.empty() || depth < 0 || depth > 65535)
        return avm1::Value::undef();

    auto sym = ctx.movie.symbolClasses.find(link);
    if (sym == ctx.movie.symbolClasses.end()) return avm1::Value::undef();

    DisplayItem d = makeDynamicItem(ctx, name, depth);
    d.characterId = sym->second;
    auto ci = ctx.movie.characters.find(sym->second);
    if (ci != ctx.movie.characters.end()) d.character = ci->second;

    if (args.size() > 3 && args[3].obj) {
        auto xi = args[3].obj->find("_x");
        auto yi = args[3].obj->find("_y");
        if (xi != args[3].obj->end() || yi != args[3].obj->end()) {
            d.hasRt = true;
            d.rtX = (xi != args[3].obj->end()) ? xi->second.toNumber() : 0.0;
            d.rtY = (yi != args[3].obj->end()) ? yi->second.toNumber() : 0.0;
        }
    }

    insertByDepth(ctx.items, std::move(d));
    if (ctx.onItemsChanged) ctx.onItemsChanged();
    return namedClip(name);
}

avm1::Value MovieClipCommands::cmdCreateEmptyMovieClip(const ExtCommandContext& ctx,
                                                      const Args& args) {
    const std::string name = argString(args, 0);
    const int depth = argInt(args, 1, -1);
    if (name.empty() || depth < 0 || depth > 65535) return avm1::Value::undef();

    auto ch = std::make_shared<Character>();
    ch->kind = Character::Kind::Sprite;
    ch->spriteFrames.emplace_back();  // 空剪辑至少 1 帧，否则运行态子树会被清空

    DisplayItem d = makeDynamicItem(ctx, name, depth);
    d.character = std::move(ch);
    insertByDepth(ctx.items, std::move(d));
    if (ctx.onItemsChanged) ctx.onItemsChanged();
    return namedClip(name);
}

avm1::Value MovieClipCommands::cmdDuplicateMovieClip(const ExtCommandContext& ctx,
                                                    const Args& args) {
    const std::string target = argString(args, 0);
    const std::string name = argString(args, 1);
    const int depth = argInt(args, 2, -1);
    if (target.empty() || name.empty() || depth < 0 || depth > 65535)
        return avm1::Value::undef();

    DisplayItem* src = findByName(ctx.items, lastSegment(target));
    if (!src) return avm1::Value::undef();

    DisplayItem d = *src;
    d.name = name;
    d.depth = static_cast<std::uint16_t>(depth);
    d.dynamic = true;
    d.birthFrame = ctx.frame - 1;
    d.spInit = false;
    d.clipVars = std::make_shared<std::map<std::string, avm1::Value>>();
    insertByDepth(ctx.items, std::move(d));
    if (ctx.onItemsChanged) ctx.onItemsChanged();
    return namedClip(name);
}

avm1::Value MovieClipCommands::cmdStartDrag(const ExtCommandContext& ctx, const Args& args) {
    (void)ctx;
    Impl& s = *impl_;
    s.active = true;
    s.lockCenter = args.empty() || args[0].type != avm1::Value::T::Bool || args[0].b;
    s.bounded = args.size() >= 5;
    if (s.bounded) {
        s.x1 = args[1].toNumber(); s.y1 = args[2].toNumber();
        s.x2 = args[3].toNumber(); s.y2 = args[4].toNumber();
    }
    // 记录当前位置作为非 lockCenter 时的偏移基准。
    DisplayItem* hit = lastDynamic(ctx.items);
    if (hit && !s.lockCenter && hit->hasRt) {
        s.offX = hit->rtX - ctx.mouseX;
        s.offY = hit->rtY - ctx.mouseY;
    }
    return avm1::Value::undef();
}

avm1::Value MovieClipCommands::cmdStopDrag(const ExtCommandContext& ctx, const Args& args) {
    (void)ctx; (void)args;
    impl_->active = false;
    return avm1::Value::undef();
}

avm1::Value MovieClipCommands::cmdGetBounds(const ExtCommandContext& ctx, const Args& args) {
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
    const std::string path = argString(args, 0, std::string("/"));
    if (ctx.boundsOf) ctx.boundsOf(path, x0, y0, x1, y1);

    avm1::Value v = makeObject();
    (*v.obj)["xMin"] = avm1::Value(x0);
    (*v.obj)["yMin"] = avm1::Value(y0);
    (*v.obj)["xMax"] = avm1::Value(x1);
    (*v.obj)["yMax"] = avm1::Value(y1);
    (*v.obj)["width"] = avm1::Value(x1 - x0);
    (*v.obj)["height"] = avm1::Value(y1 - y0);
    return v;
}

avm1::Value MovieClipCommands::cmdSetMask(const ExtCommandContext& ctx, const Args& args) {
    const std::string mask = argString(args, 0);
    if (mask.empty()) return avm1::Value::undef();
    const std::string name = lastSegment(mask);
    if (DisplayItem* source = findByName(ctx.items, name)) source->maskSource = true;
    // 阶段 1 只登记字段；渲染侧的 clipDepth 裁剪组转换在阶段 2 接入。
    if (DisplayItem* target = lastDynamic(ctx.items)) {
        target->maskName = name;
        target->hasMask = true;
    }
    return avm1::Value::undef();
}

void MovieClipCommands::applyDrag(const ExtCommandContext& ctx) {
    Impl& s = *impl_;
    if (!s.active) return;

    DisplayItem* hit = nullptr;
    if (!s.path.empty()) hit = findByName(ctx.items, lastSegment(s.path));
    if (!hit) hit = lastDynamic(ctx.items);
    if (!hit) return;

    double x = ctx.mouseX + (s.lockCenter ? 0.0 : s.offX);
    double y = ctx.mouseY + (s.lockCenter ? 0.0 : s.offY);
    if (s.bounded) {
        x = std::max(std::min(x, std::max(s.x1, s.x2)), std::min(s.x1, s.x2));
        y = std::max(std::min(y, std::max(s.y1, s.y2)), std::min(s.y1, s.y2));
    }
    if (!std::isfinite(x) || !std::isfinite(y)) return;
    hit->hasRt = true;
    hit->rtX = x;
    hit->rtY = y;
}

void MovieClipCommands::endDrag() { impl_->active = false; }
bool MovieClipCommands::isDragging() const { return impl_->active; }

}  // namespace swf
