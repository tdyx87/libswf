// AVM2 ABC (ActionScript Byte Code) 解析器
// 对应 DoABC 标签(tag 82)内的 ABCFile 格式：常量池 / method_info /
// instance_info / class_info / script_info / method_body。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace swf {
namespace abc {

// method_info.option_detail / trait Slot 默认值使用的 ConstantKind（AVM2 Overview）。
enum class ConstKind {
    Undefined = 0x00, Utf8 = 0x01, Decimal = 0x02, Int = 0x03, UInt = 0x04,
    PrivateNs = 0x05, Double = 0x06, Namespace = 0x08, False = 0x0A,
    True = 0x0B, Null = 0x0C, PackageNs = 0x16, PackageInternalNs = 0x17,
    ProtectedNs = 0x18, ExplicitNs = 0x19, StaticProtectedNs = 0x1A
};

struct NamespaceInfo {
    uint8_t kind = 0;   // 0x08 const, 0x16 namespace, 0x17 packageinternal, 0x18 protected, 0x19 explicit, 0x1A static, 0x05 value
    uint32_t name = 0;  // 常量池索引
};

struct Multiname {
    enum class Kind : uint8_t {
        QName = 0x07, QNameA = 0x0D, RTQName = 0x0F, RTQNameA = 0x10,
        RTQNameL = 0x11, RTQNameLA = 0x12, Multiname = 0x09, MultinameA = 0x0E,
        MultinameL = 0x1B, MultinameLA = 0x1C, TypeName = 0x1D
    };
    Kind kind = Kind::QName;
    uint32_t ns = 0;
    uint32_t name = 0;
    std::vector<uint32_t> typeArgs;
};

struct Traits {
    uint32_t name = 0;      // multiname index
    uint8_t  kind = 0;      // Trait_Slot=0, Trait_Method=1, Trait_Getter=2, Trait_Setter=3, Trait_Class=4, Trait_Function=5, Trait_Const=6
    uint32_t typeName = 0;
    uint32_t slotId = 0;
    uint32_t functionName = 0;  // method/getter/setter: method index；Slot/Const: 默认值常量池索引(vindex)
    uint8_t  vkind = 0;         // Slot/Const 默认值的常量种类（vkind）
    int      dispId = 0;
    uint32_t metadataCount = 0;
};

struct MethodParam {
    uint8_t  reg = 0;
    uint8_t  optional = 0;
    uint32_t typeName = 0;
};

// 可选参数默认值（method_info 的 option_detail：常量池索引 + 常量种类）
struct OptionalDefault {
    uint32_t val = 0;    // 常量池索引
    uint8_t  kind = 0;   // ConstKind（Int/Uint/Double/String…）
};

struct MethodInfo {
    uint32_t param_count = 0;
    uint32_t returnType = 0;
    std::vector<MethodParam> params;
    std::vector<OptionalDefault> optionalDefaults;  // 末尾若干参数的默认值
    uint32_t nameIndex = 0;    // 方法名（常量池索引）
    std::string name;          // 由 nameIndex 解析出的名字（便于查找）
    uint16_t flags = 0;
    int bodyIndex = -1;        // -> MethodBody（解析后填充）
};

struct MethodBody {
    uint32_t methodIndex = 0;    // 所属 method_info 索引
    uint32_t maxStack = 0;
    uint32_t localCount = 0;
    uint32_t initScopeDepth = 0;
    uint32_t maxScopeDepth = 0;
    uint32_t codeLength = 0;
    std::vector<uint8_t> code;
    std::vector<uint32_t> exceptionHandlers;  // 每 5 个 u30: from,to,target,excType,varName
    std::vector<Traits> traits;
};

struct InstanceInfo {
    uint32_t name = 0;        // multiname
    uint16_t flags = 0;
    uint32_t superClass = 0;  // multiname index
    uint32_t protectedNs = 0;   // ClassProtectedNs：u30
    std::vector<uint32_t> interfaces;  // multiname
    uint32_t iinit = 0;       // 实例初始化方法 index
    std::vector<Traits> traits;
};

struct ClassInfo {
    uint32_t cinit = 0;
    std::vector<Traits> traits;
};

struct ScriptInfo {
    uint32_t init = 0;
    std::vector<Traits> traits;
};

struct AbcFile {
    uint16_t minor = 0, major = 0;
    std::vector<int32_t>  ints;
    std::vector<uint32_t> uints;
    std::vector<double>   doubles;
    std::vector<std::string> strings;
    std::vector<NamespaceInfo> namespaces;
    std::vector<std::vector<uint32_t>> nsSets;
    std::vector<Multiname> multinames;
    std::vector<MethodInfo> methods;
    std::vector<MethodBody>  methodBodies;
    std::vector<InstanceInfo> instances;
    std::vector<ClassInfo>    classes;
    std::vector<ScriptInfo>   scripts;

    const std::string& str(uint32_t i) const {
        static const std::string kEmpty;
        return (i < strings.size()) ? strings[i] : kEmpty;
    }
    const Multiname* mn(uint32_t i) const {
        return (i > 0 && i < multinames.size()) ? &multinames[i] : nullptr;
    }
};

// 解析一个 ABC 文件数据块。返回 false 表示格式非法或越界。
bool parseAbc(const uint8_t* data, size_t size, AbcFile& out, std::string& err);

}  // namespace abc
}  // namespace swf
