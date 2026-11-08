// 解析层基础设施：Strategy(TagHandler) + Registry(TagRegistry) + Template Method(TagPipeline)。
//
// 设计约束：
//   * 所有 handler 只能看到 TagReader —— 以**单个 tag 为边界**的视图，偏移相对 tag 起点；
//     handler 拿不到整文件 base/avail，从结构上消除跨 tag 越界读。
//   * TagPipeline 固定「校验长度 → 构造有界视图 → 查表 → 调用 → 检查 overflow → 推进」六步，
//     推进永远留在管道层，不允许 handler 干预。
//   * 渐进迁移：尚未抽成 Strategy 的 tag 通过 fallback 处理。fallback 拿到的是带**软边界**的
//     ByteView（偏移仍为绝对偏移），因此旧 switch 的 pos()/base+pos() 语义保持不变。
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <unordered_map>
#include <utility>

#include "swf/ByteView.hpp"
#include "swf/Format.hpp"

namespace swf {

enum class TagStatus {
    Ok,          // 正常处理
    Ignored,     // 未注册 / 未知 tag
    Stop,        // End tag：正常终止标签流
    Fail,        // 数据损坏：越界、长度非法、tag 数量异常
    Incomplete   // 数据不足（一次性解析中等价于正常结束）
};

// Parser 与 Decoder 共用的唯一 tag 头解析实现。
// 全程用减法判断边界，避免 pos + len 的无符号回绕。
TagStatus readTagHeader(const std::uint8_t* data, std::size_t pos,
                        std::size_t avail, TagHeader& out);

// 以单个 tag 数据区为边界的读取视图；handler 唯一可见的输入。
class TagReader {
public:
    // fileBase 为整份数据起点（绝对偏移基准），data 为当前 tag 数据区起点。
    // 单参数 data 形式保持向后兼容：fileBase 退化为 data，dataOffset 为 0。
    TagReader(const std::uint8_t* data, std::size_t size, std::uint16_t code = 0)
        : TagReader(data, data, size, code) {}

    TagReader(const std::uint8_t* fileBase, const std::uint8_t* data,
              std::size_t size, std::uint16_t code)
        : bytes_(data, size), code_(code), length_(static_cast<std::uint32_t>(size)),
          fileBase_(fileBase),
          dataOffset_(static_cast<std::size_t>(data - fileBase)) {}

    std::uint16_t code() const { return code_; }
    std::uint32_t length() const { return length_; }

    std::size_t pos() const { return bytes_.pos(); }
    std::size_t size() const { return bytes_.size(); }
    std::size_t remaining() const { return bytes_.remaining(); }
    bool has(std::size_t n) const { return bytes_.has(n); }
    bool overflow() const { return bytes_.overflow(); }
    void clearOverflow() { bytes_.clearOverflow(); }
    void noteOverflow() { bytes_.noteOverflow(); }
    const std::uint8_t* current() const { return bytes_.current(); }
    const std::uint8_t* base() const { return bytes_.base(); }
    // 整份数据起点与当前 tag 在其中的绝对偏移，供需要重建「绝对 bv」的 handler 使用。
    const std::uint8_t* fileBase() const { return fileBase_; }
    std::size_t dataOffset() const { return dataOffset_; }
    // 以文件头为基准、定位到本 tag 数据起点的绝对视图（等价于 fallback 里传入的 ByteView）。
    ByteView absView() const {
        ByteView v(fileBase_, dataOffset_ + length_);
        v.seek(dataOffset_);
        return v;
    }
    void seek(std::size_t p) { bytes_.seek(p); }
    void skip(std::size_t n) { bytes_.skip(n); }

    std::uint8_t readUI8() { return bytes_.readUI8(); }
    std::uint16_t readUI16() { return bytes_.readUI16(); }
    std::uint32_t readUI32() { return bytes_.readUI32(); }
    ByteView::RGBA readRGBA(bool withAlpha) { return bytes_.readRGBA(withAlpha); }

    ByteView& bytes() { return bytes_; }
    const ByteView& bytes() const { return bytes_; }

    // 越界读取返回 false 且不推进；用于替代 base + doff + n 的裸指针算术。
    bool readBytes(std::uint8_t* dst, std::size_t n) {
        if (!has(n)) { bytes_.noteOverflow(); return false; }
        for (std::size_t i = 0; i < n; ++i) dst[i] = bytes_.readUI8();
        return true;
    }

    // 切出子视图（DefineSprite 子标签流用）。越界时夹紧并标记 overflow。
    TagReader slice(std::size_t off, std::size_t len) const {
        const std::size_t s = bytes_.size();
        if (off > s) {
            TagReader empty(bytes_.base(), 0, code_);
            empty.noteOverflow();
            return empty;
        }
        const std::size_t room = s - off;
        if (len > room) {
            TagReader clamped(bytes_.base() + off, room, code_);
            clamped.noteOverflow();
            return clamped;
        }
        return TagReader(bytes_.base() + off, len, code_);
    }

private:
    ByteView bytes_;
    std::uint16_t code_;
    std::uint32_t length_;
    const std::uint8_t* fileBase_ = nullptr;
    std::size_t dataOffset_ = 0;
};

// Strategy：code -> 处理策略。Context 由调用方提供（模板化，无需把它提升到头文件）。
template <class Context>
class TagRegistry {
public:
    using Handler = std::function<TagStatus(TagReader&, Context&, std::uint16_t)>;

    void add(std::uint16_t code, Handler h) { handlers_[code] = std::move(h); }

    void add(std::initializer_list<std::uint16_t> codes, Handler h) {
        for (std::uint16_t c : codes) add(c, h);
    }

    bool contains(std::uint16_t code) const {
        return handlers_.find(code) != handlers_.end();
    }

    TagStatus dispatch(TagReader& r, Context& ctx, std::uint16_t code) const {
        auto it = handlers_.find(code);
        if (it == handlers_.end()) return TagStatus::Ignored;
        return it->second(r, ctx, code);
    }

    std::size_t size() const { return handlers_.size(); }

private:
    std::unordered_map<std::uint16_t, Handler> handlers_;
};

// Template Method：标签流的骨架算法。子类行为由 registry 与 fallback 注入。
template <class Context>
class TagPipeline {
public:
    using Fallback = std::function<TagStatus(ByteView&, Context&, const TagHeader&)>;

    TagPipeline(const TagRegistry<Context>& registry, Fallback fallback)
        : registry_(registry), fallback_(std::move(fallback)) {}

    TagStatus run(const std::uint8_t* base, std::size_t avail, std::size_t startPos,
                  Context& ctx, std::size_t maxTags = 1u << 20,
                  std::size_t* lastPos = nullptr) {
        if (!base) return TagStatus::Fail;
        std::size_t pos = startPos;
        for (std::size_t n = 0; n < maxTags; ++n) {
            TagHeader th;
            TagStatus st = readTagHeader(base, pos, avail, th);
            if (st != TagStatus::Ok) {
                if (lastPos) *lastPos = pos;
                return st;
            }

            TagReader reader(base, base + th.dataOffset, th.length, th.code);
            st = registry_.dispatch(reader, ctx, th.code);

            if (st == TagStatus::Ignored && fallback_) {
                ByteView bv(base, avail);
                bv.seek(th.dataOffset);
                bv.setLimit(th.dataEnd);
                st = fallback_(bv, ctx, th);
                if (bv.overflow() && st != TagStatus::Stop) st = TagStatus::Fail;
            }
            if (reader.overflow() && st != TagStatus::Stop) st = TagStatus::Fail;

            pos = th.dataEnd;
            if (lastPos) *lastPos = pos;
            if (st == TagStatus::Stop || st == TagStatus::Fail) return st;
        }
        return TagStatus::Fail;  // tag 数量异常：防止畸形流让解析器无限运转
    }

private:
    const TagRegistry<Context>& registry_;
    Fallback fallback_;
};

}  // namespace swf
