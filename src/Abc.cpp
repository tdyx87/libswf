#include "swf/Abc.hpp"

#include <cstring>

namespace swf {
namespace abc {

namespace {

class Reader {
public:
    Reader(const uint8_t* d, size_t n) : d_(d), n_(n) {}

    bool eof() const { return p_ >= n_; }
    size_t pos() const { return p_; }
    void seek(size_t p) { p_ = p; }

    uint8_t u8() {
        if (p_ >= n_) { bad_ = true; return 0; }
        return d_[p_++];
    }
    uint16_t u16() {
        uint16_t a = u8();
        return static_cast<uint16_t>(a | (u8() << 8));
    }
    uint32_t u32() {
        uint32_t a = u16();
        return a | (static_cast<uint32_t>(u16()) << 16);
    }
    double d64() {
        uint64_t lo = u32();
        uint64_t hi = u32();
        uint64_t bits = (hi << 32) | lo;
        double d;
        std::memcpy(&d, &bits, sizeof(d));
        return d;
    }

    // U30：可变长（最多 5 字节，7 位一组）
    uint32_t u30() {
        uint32_t result = 0;
        for (int i = 0; i < 5; ++i) {
            uint8_t b = u8();
            if (i == 4) { result |= static_cast<uint32_t>(b & 0x0F) << 28; break; }
            result |= static_cast<uint32_t>(b & 0x7F) << (7 * i);
            if (!(b & 0x80)) break;
        }
        return result;
    }
    // S30：30 位二进制补码可变长编码（与 U30 同格式，按 30 位符号扩展）
    int32_t s30() {
        uint32_t v = u30();
        if (v & 0x20000000u) v |= 0xC0000000u;   // 从第 29 位符号扩展
        return static_cast<int32_t>(v);
    }

    bool bad() const { return bad_; }

private:
    const uint8_t* d_;
    size_t n_;
    size_t p_ = 0;
    bool bad_ = false;
};

void readTraits(Reader& r, std::vector<Traits>& out) {
    uint32_t count = r.u30();
    for (uint32_t i = 0; i < count && !r.bad() && !r.eof(); ++i) {
        Traits t;
        t.name = r.u30();
        uint8_t kb = r.u8();
        t.kind = static_cast<uint8_t>(kb & 0x0F);
        switch (t.kind) {
            case 0x00:  // Slot
            case 0x06:  // Const
                t.slotId = r.u30();
                t.typeName = r.u30();
                t.functionName = r.u30();   // vindex（常量池索引）
                if (t.functionName != 0) r.u8();  // vkind
                break;
            case 0x01:  // Method
            case 0x02:  // Getter
            case 0x03:  // Setter
                t.functionName = r.u30();
                t.dispId = static_cast<int>(r.u30());
                break;
            case 0x04:  // Class
                t.slotId = r.u30();
                t.functionName = r.u30();
                break;
            case 0x05:  // Function
                t.slotId = r.u30();
                t.functionName = r.u30();
                break;
            default:
                break;
        }
        // metadata 仅当 ATTR_Metadata(0x40) 置位时存在，count 为 u30
        if (kb & 0x40) {
            uint32_t mc = r.u30();
            for (uint32_t m = 0; m < mc && !r.eof(); ++m) r.u30();
            t.metadataCount = static_cast<uint8_t>(mc);
        } else {
            t.metadataCount = 0;
        }
        out.push_back(t);
    }
}

}  // namespace

bool parseAbc(const uint8_t* data, size_t size, AbcFile& out, std::string& err) {
    Reader r(data, size);
    out.minor = r.u16();
    out.major = r.u16();
    if (r.bad()) { err = "abc header"; return false; }

    // ---- 常量池 ----
    // ints
    uint32_t intCount = r.u30();
    if (intCount >= 1) {
        out.ints.resize(intCount);
        for (uint32_t i = 1; i < intCount; ++i) out.ints[i] = r.s30();
    }
    // uints
    uint32_t uintCount = r.u30();
    if (uintCount >= 1) {
        out.uints.resize(uintCount);
        for (uint32_t i = 1; i < uintCount; ++i) out.uints[i] = r.u30();
    }
    // doubles
    uint32_t doubleCount = r.u30();
    if (doubleCount >= 1) {
        out.doubles.resize(doubleCount);
        for (uint32_t i = 1; i < doubleCount; ++i) out.doubles[i] = r.d64();
    }
    // strings
    uint32_t strCount = r.u30();
    if (strCount >= 1) {
        out.strings.resize(strCount);
        for (uint32_t i = 1; i < strCount && !r.eof() && !r.bad(); ++i) {
            // UTF-8，长度前缀 u30
            uint32_t len = r.u30();
            std::string s;
            if (r.pos() + len <= size) {
                s.assign(reinterpret_cast<const char*>(data + r.pos()), len);
                r.seek(r.pos() + len);
            } else { r.bad(); break; }
            out.strings[i] = std::move(s);
        }
    }
    // namespaces
    uint32_t nsCount = r.u30();
    if (nsCount >= 1) {
        out.namespaces.resize(nsCount);
        for (uint32_t i = 1; i < nsCount; ++i) {
            out.namespaces[i].kind = r.u8();
            out.namespaces[i].name = r.u30();
        }
    }
    // ns sets
    uint32_t nsSetCount = r.u30();
    if (nsSetCount >= 1) {
        out.nsSets.resize(nsSetCount);
        for (uint32_t i = 1; i < nsSetCount; ++i) {
            uint32_t c = r.u30();
            for (uint32_t k = 0; k < c; ++k) out.nsSets[i].push_back(r.u30());
        }
    }
    // multinames
    uint32_t mnCount = r.u30();
    if (mnCount >= 1) {
        out.multinames.resize(mnCount);
        for (uint32_t i = 1; i < mnCount && !r.eof() && !r.bad(); ++i) {
            Multiname& m = out.multinames[i];
            m.kind = static_cast<Multiname::Kind>(r.u8());
            switch (m.kind) {
                case Multiname::Kind::QName:
                case Multiname::Kind::QNameA:
                    m.ns = r.u30(); m.name = r.u30(); break;
                case Multiname::Kind::RTQName:
                case Multiname::Kind::RTQNameA:
                    m.name = r.u30(); break;
                case Multiname::Kind::RTQNameL:
                case Multiname::Kind::RTQNameLA:
                    break;
                case Multiname::Kind::Multiname:
                case Multiname::Kind::MultinameA:
                    m.name = r.u30(); m.ns = r.u30(); break;
                case Multiname::Kind::MultinameL:
                case Multiname::Kind::MultinameLA:
                    m.name = r.u30(); break;
                case Multiname::Kind::TypeName: {
                    m.name = r.u30();
                    uint8_t paramCount = r.u8();
                    for (int k = 0; k < paramCount; ++k) m.typeArgs.push_back(r.u30());
                    break;
                }
                default:
                    r.bad(); break;
            }
        }
    }

    // ---- method_info ----
    // 布局：param_count:u30, return_type:u30, param_type[param_count]:u30,
    //       name:u30, flags:u8, [options], [param_names]
    uint32_t methodCount = r.u30();
    out.methods.resize(methodCount);
    for (uint32_t i = 0; i < methodCount && !r.eof() && !r.bad(); ++i) {
        MethodInfo& m = out.methods[i];
        uint32_t paramCount = r.u30();
        m.param_count = paramCount;
        m.returnType = r.u30();
        // param_type 是单个 u30（multiname 索引），不是 {reg,optional,typeName}
        for (uint32_t k = 0; k < paramCount; ++k) {
            MethodParam p;
            p.reg = 0;
            p.optional = 0;
            p.typeName = r.u30();
            m.params.push_back(p);
        }
        m.nameIndex = r.u30();
        m.flags = r.u8();
        if (m.flags & 0x08) {  // HAS_OPTIONAL
            // option_detail 仅含 {val:u30(常量池索引), kind:u8(ConstantKind)}，无额外值字段
            uint32_t optCount = r.u30();
            for (uint32_t k = 0; k < optCount && !r.eof(); ++k) {
                r.u30();   // val
                r.u8();    // kind
            }
        }
        if (m.flags & 0x80) {  // HAS_PARAM_NAMES
            for (uint32_t k = 0; k < paramCount; ++k) r.u30();
        }
    }

    // ---- metadata ----
    uint32_t metaCount = r.u30();
    for (uint32_t i = 0; i < metaCount && !r.eof() && !r.bad(); ++i) {
        r.u30();  // name
        uint32_t ic = r.u30();
        for (uint32_t k = 0; k < ic; ++k) { r.u30(); r.u30(); }
    }

    // ---- instances / classes ----
    uint32_t instanceCount = r.u30();
    out.instances.resize(instanceCount);
    out.classes.resize(instanceCount);
    for (uint32_t i = 0; i < instanceCount && !r.eof() && !r.bad(); ++i) {
        InstanceInfo& inst = out.instances[i];
        inst.name = r.u30();             // multiname
        inst.superClass = r.u30();       // multiname（无条件）
        inst.flags = r.u8();
        if (inst.flags & 0x08) {         // ClassProtectedNs
            inst.protectedNs = r.u30();
        }
        uint32_t ifCount = r.u30();      // intrf_count 为 u30
        for (uint32_t k = 0; k < ifCount && !r.eof(); ++k)
            inst.interfaces.push_back(r.u30());
        inst.iinit = r.u30();
        readTraits(r, inst.traits);
    }
    for (uint32_t i = 0; i < instanceCount && !r.eof() && !r.bad(); ++i) {
        ClassInfo& ci = out.classes[i];
        ci.cinit = r.u30();
        readTraits(r, ci.traits);
    }

    // ---- scripts ----
    uint32_t scriptCount = r.u30();
    out.scripts.resize(scriptCount);
    for (uint32_t i = 0; i < scriptCount && !r.eof() && !r.bad(); ++i) {
        out.scripts[i].init = r.u30();
        readTraits(r, out.scripts[i].traits);
    }

    // ---- method_body ----
    uint32_t bodyCount = r.u30();
    out.methodBodies.resize(bodyCount);
    for (uint32_t i = 0; i < bodyCount && !r.eof() && !r.bad(); ++i) {
        MethodBody& b = out.methodBodies[i];
        b.methodIndex = r.u30();        // 所属 method_info 索引
        b.maxStack = r.u30();
        b.localCount = r.u30();
        b.initScopeDepth = r.u30();
        b.maxScopeDepth = r.u30();
        b.codeLength = r.u30();
        if (r.pos() + b.codeLength <= size) {
            b.code.assign(data + r.pos(), data + r.pos() + b.codeLength);
            r.seek(r.pos() + b.codeLength);
        } else { err = "abc code oob"; return false; }
        uint32_t excCount = r.u30();
        for (uint32_t k = 0; k < excCount; ++k) {
            b.exceptionHandlers.push_back(r.u30());
            b.exceptionHandlers.push_back(r.u30());
            b.exceptionHandlers.push_back(r.u30());
            b.exceptionHandlers.push_back(r.u30());
            b.exceptionHandlers.push_back(r.u30());
        }
        readTraits(r, b.traits);
    }

    // ---- 方法 <-> 方法体 关联 ----
    // method_body 数组只包含"有函数体的方法"，且顺序与这些方法在 method_info
    // 中的出现顺序一致。有函数体的方法 = param_count>0，或被 trait/init 引用。
    {
        std::vector<uint8_t> hasBody(out.methods.size(), 0);
        auto markTrait = [&](const std::vector<Traits>& ts) {
            for (const auto& t : ts)
                if ((t.kind == 0x01 || t.kind == 0x02 || t.kind == 0x03 || t.kind == 0x05) &&
                    t.functionName < out.methods.size())
                    hasBody[t.functionName] = 1;
        };
        for (const auto& inst : out.instances) {
            if (inst.iinit < out.methods.size()) hasBody[inst.iinit] = 1;
            markTrait(inst.traits);
        }
        for (const auto& c : out.classes) { if (c.cinit < out.methods.size()) hasBody[c.cinit] = 1; markTrait(c.traits); }
        for (const auto& s : out.scripts) { if (s.init < out.methods.size()) hasBody[s.init] = 1; markTrait(s.traits); }
        for (std::size_t i = 0; i < out.methods.size(); ++i)
            if (out.methods[i].param_count > 0) hasBody[i] = 1;
        // 优先用 method_body 自带的 methodIndex 建立权威映射
        for (std::size_t i = 0; i < out.methods.size(); ++i) out.methods[i].bodyIndex = -1;
        for (std::size_t bi = 0; bi < out.methodBodies.size(); ++bi) {
            uint32_t mi = out.methodBodies[bi].methodIndex;
            if (mi < out.methods.size()) out.methods[mi].bodyIndex = static_cast<int>(bi);
        }
        // 若 methodIndex 全部无效，回退到顺序推断
        bool anyLinked = false;
        for (const auto& m : out.methods) if (m.bodyIndex >= 0) { anyLinked = true; break; }
        if (!anyLinked) {
            std::size_t bi = 0;
            for (std::size_t i = 0; i < out.methods.size(); ++i) {
                if (hasBody[i]) {
                    out.methods[i].bodyIndex = (bi < out.methodBodies.size()) ? static_cast<int>(bi) : -1;
                    ++bi;
                }
            }
        }
        for (std::size_t i = 0; i < out.methods.size(); ++i)
            out.methods[i].name = out.str(out.methods[i].nameIndex);
    }

    if (r.bad()) { err = "abc truncated"; return false; }
    return true;
}

}  // namespace abc
}  // namespace swf
