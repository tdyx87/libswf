// AVM1 MovieClip 扩展方法：Facade(Player) + Command(MovieClipCommands)。
//
// Player 只保留一个 callExt 入口，把 7 个扩展方法委托给这里；命令对象通过
// ExtCommandContext 拿到所需的最小能力（由 Facade 注入回调），不反向依赖 Player 私有实现。
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "swf/Avm1.hpp"
#include "swf/Command.hpp"
#include "swf/Decoder.hpp"

namespace swf {

// Facade 提供给 Command 的最小上下文。boundsOf / onItemsChanged 特意做成回调，
// 避免 Command 为了调用 Player 私有辅助方法而变成 friend 或嵌套类。
struct ExtCommandContext {
    Movie& movie;
    std::vector<DisplayItem>& items;
    int frame = 1;
    double mouseX = 0.0;
    double mouseY = 0.0;
    std::function<bool(const std::string& path,
                       double& xMin, double& yMin, double& xMax, double& yMax)> boundsOf;
    std::function<void()> onItemsChanged;
};

class MovieClipCommands {
public:
    using Args = std::vector<avm1::Value>;
    using Registry = CommandRegistry<ExtCommandContext, avm1::Value, const Args&>;

    MovieClipCommands();
    ~MovieClipCommands();

    MovieClipCommands(const MovieClipCommands&) = delete;
    MovieClipCommands& operator=(const MovieClipCommands&) = delete;

    avm1::Value call(const std::string& method, const ExtCommandContext& ctx,
                     const Args& args);

    // 每帧 tick 由 Facade 调用，把拖拽状态应用到显示列表。
    void applyDrag(const ExtCommandContext& ctx);
    void endDrag();
    bool isDragging() const;

    std::size_t commandCount() const { return registry_.size(); }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Registry registry_;

    avm1::Value cmdAttachMovie(const ExtCommandContext& ctx, const Args& args);
    avm1::Value cmdDuplicateMovieClip(const ExtCommandContext& ctx, const Args& args);
    avm1::Value cmdCreateEmptyMovieClip(const ExtCommandContext& ctx, const Args& args);
    avm1::Value cmdStartDrag(const ExtCommandContext& ctx, const Args& args);
    avm1::Value cmdStopDrag(const ExtCommandContext& ctx, const Args& args);
    avm1::Value cmdGetBounds(const ExtCommandContext& ctx, const Args& args);
    avm1::Value cmdSetMask(const ExtCommandContext& ctx, const Args& args);
};

}  // namespace swf
