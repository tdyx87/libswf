// Command 模式：把「名字 → 行为」的分派从 if/switch 链里抽出来。
//
// Context 由 Facade 构造并传入，Command 本身不持有 Facade，因此可以脱离宿主独立测试。
// 未注册的名字统一返回 unknown_（由 Facade 指定，例如 AVM1 的 undefined）。
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>

namespace swf {

template <class Context, class Result, class... Args>
class CommandRegistry {
public:
    using Command = std::function<Result(const Context&, Args...)>;

    void add(const std::string& name, Command cmd) {
        commands_[name] = std::move(cmd);
    }

    bool contains(const std::string& name) const {
        return commands_.find(name) != commands_.end();
    }

    void setUnknown(Result r) { unknown_ = std::move(r); }

    Result dispatch(const std::string& name, const Context& ctx, Args... args) const {
        auto it = commands_.find(name);
        if (it == commands_.end()) return unknown_;
        return it->second(ctx, args...);
    }

    std::size_t size() const { return commands_.size(); }

private:
    std::unordered_map<std::string, Command> commands_;
    Result unknown_{};
};

}  // namespace swf
