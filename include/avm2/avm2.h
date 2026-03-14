#ifndef LIBSWF_AVM2_AVM2_H
#define LIBSWF_AVM2_AVM2_H

#include "common/types.h"
#include <stack>
#include <vector>
#include <string>
#include <memory>
#include <variant>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <set>
#include <list>
#include <shared_mutex>

namespace libswf {

// Forward declarations
class ABCFile;
class ABCMethod;
class ABCClass;
class ABCInstance;
class ABCTrait;
class AVM2Namespace;
class AVM2NamespaceSet;
class ABCMultiname;
class AVM2GarbageCollector;

// AVM2 Value types
using AVM2Value = std::variant<
    std::monostate,    // Undefined
    bool,              // Boolean
    int32,             // Int
    double,           // Double
    std::string,      // String
    std::shared_ptr<class AVM2Object>,  // Object
    std::shared_ptr<class AVM2Array>,   // Array
    std::shared_ptr<class AVM2Function>, // Function
    std::shared_ptr<class AVM2Class>     // Class
>;

// Exception structure for try/catch
struct AVM2Exception {
    std::string message;
    std::string type;
    AVM2Value value;
    size_t pc;  // Program counter where exception occurred
};

// String interning for faster property lookups
class AVM2StringPool {
public:
    static AVM2StringPool& getInstance();
    const std::string* intern(const std::string& str);
    const std::string* intern(std::string&& str);
    
private:
    std::unordered_set<std::string> pool;
    std::shared_mutex mutex;
};

// Property lookup cache - inline cache for hot property accesses
struct PropertyCacheEntry {
    const std::string* name = nullptr;
    AVM2Value* valuePtr = nullptr;  // Direct pointer to value (unsafe but fast)
    uint64_t version = 0;
    static thread_local uint64_t globalVersion;
};

// AVM2 Object base class
class AVM2Object : public std::enable_shared_from_this<AVM2Object> {
public:
    virtual ~AVM2Object() = default;
    
    virtual AVM2Value getProperty(const std::string& name);
    virtual void setProperty(const std::string& name, const AVM2Value& value);
    virtual bool hasProperty(const std::string& name) const;
    virtual bool deleteProperty(const std::string& name);
    virtual AVM2Value call(const std::vector<AVM2Value>& args);
    virtual AVM2Value construct(const std::vector<AVM2Value>& args);
    virtual std::string toString() const;
    
    // Property access with multiname
    virtual AVM2Value getPropertyMultiname(const std::shared_ptr<ABCMultiname>& multiname);
    virtual void setPropertyMultiname(const std::shared_ptr<ABCMultiname>& multiname, const AVM2Value& value);
    
    // Fast property access with caching
    AVM2Value getPropertyFast(const std::string& name, PropertyCacheEntry& cache);
    void setPropertyFast(const std::string& name, const AVM2Value& value, PropertyCacheEntry& cache);
    
    // Internal properties - using pointer keys for interned strings
    std::unordered_map<const std::string*, AVM2Value> properties;
    std::shared_ptr<class AVM2Class> class_;
    std::shared_ptr<AVM2Object> prototype;
    
    // GC marking
    mutable bool gcMarked = false;
    void mark() const;
    void unmark() const;
    
    // Object version for cache invalidation
    mutable uint64_t version = 0;
    void bumpVersion() const { version++; }
};

// AVM2 Array
class AVM2Array : public AVM2Object {
public:
    std::vector<AVM2Value> elements;
    
    AVM2Value getProperty(const std::string& name) override;
    void setProperty(const std::string& name, const AVM2Value& value) override;
    AVM2Value call(const std::vector<AVM2Value>& args) override;
    std::string toString() const override;
    
    size_t length() const { return elements.size(); }
    void push(const AVM2Value& value) { elements.push_back(value); }
    AVM2Value pop();
    AVM2Value shift();
    void unshift(const AVM2Value& value);
    AVM2Value& operator[](size_t idx) { return elements[idx]; }
    
    // Array methods
    AVM2Value join(const std::string& separator = ",") const;
    void splice(size_t start, size_t deleteCount, const std::vector<AVM2Value>& items);
    AVM2Array slice(size_t start, size_t end) const;
    void reverse();
    void sort();
};

// AVM2 Function
class AVM2Function : public AVM2Object {
public:
    std::shared_ptr<ABCMethod> method;
    std::shared_ptr<AVM2Object> scope;
    std::shared_ptr<AVM2Object> boundThis;
    std::vector<AVM2Value> boundArgs;
    bool isNative = false;
    std::function<AVM2Value(class AVM2Context&, const std::vector<AVM2Value>&)> nativeImpl;
    
    AVM2Value getProperty(const std::string& name) override;
    void setProperty(const std::string& name, const AVM2Value& value) override;
    AVM2Value call(const std::vector<AVM2Value>& args) override;
    AVM2Value construct(const std::vector<AVM2Value>& args) override;
    
    // Bind this object
    std::shared_ptr<AVM2Function> bind(std::shared_ptr<AVM2Object> thisObj);
};

// AVM2 Class
class AVM2Class : public AVM2Object {
public:
    std::string name;
    std::shared_ptr<AVM2Class> superClass;
    std::shared_ptr<ABCClass> abcClass;
    std::shared_ptr<ABCInstance> abcInstance;
    std::vector<std::shared_ptr<ABCTrait>> staticTraits;
    std::vector<std::shared_ptr<ABCTrait>> instanceTraits;
    std::shared_ptr<AVM2Function> constructor;
    std::shared_ptr<AVM2Function> staticInitializer;
    
    AVM2Value getProperty(const std::string& name) override;
    void setProperty(const std::string& name, const AVM2Value& value) override;
    AVM2Value call(const std::vector<AVM2Value>& args) override;
    AVM2Value construct(const std::vector<AVM2Value>& args) override;
    
    std::shared_ptr<AVM2Object> createInstance();
    void initializeInstance(std::shared_ptr<AVM2Object> obj);
    
    // Check if this class is a subclass of another
    bool isSubclassOf(const std::shared_ptr<AVM2Class>& other) const;
};

// AVM2 Namespace
class AVM2Namespace {
public:
    std::string name;
    uint8 kind;  // 0x08 = namespace, 0x16 = package namespace, etc.
    
    enum Kind {
        Namespace = 0x08,
        PackageNamespace = 0x16,
        PackageInternalNs = 0x17,
        ProtectedNamespace = 0x18,
        ExplicitNamespace = 0x19,
        StaticProtectedNs = 0x1A,
        PrivateNs = 0x05
    };
    
    bool isPackage() const { return kind == PackageNamespace; }
    bool isPrivate() const { return kind == PrivateNs; }
    bool isProtected() const { return kind == ProtectedNamespace || kind == StaticProtectedNs; }
};

// AVM2 Namespace Set
class AVM2NamespaceSet {
public:
    std::vector<std::shared_ptr<AVM2Namespace>> namespaces;
};

// ABC Multiname (name with namespace qualification)
class ABCMultiname {
public:
    uint8 kind;
    std::string name;
    
    // For QName
    std::shared_ptr<AVM2Namespace> ns;
    
    // For Multiname
    std::shared_ptr<AVM2NamespaceSet> nsSet;
    
    // For RTQName (runtime qualified name)
    bool hasRuntimeNamespace = false;
    bool hasRuntimeName = false;
    
    enum Kind {
        QName = 0x07,
        QNameA = 0x0D,
        RTQName = 0x0F,
        RTQNameA = 0x10,
        RTQNameL = 0x11,
        RTQNameLA = 0x12,
        Multiname = 0x09,
        MultinameA = 0x0E,
        MultinameL = 0x1B,
        MultinameLA = 0x1C,
        TypeName = 0x1D
    };
    
    bool isAttribute() const {
        return kind == QNameA || kind == RTQNameA || kind == RTQNameLA || 
               kind == MultinameA || kind == MultinameLA;
    }
    
    bool isQName() const { return kind == QName || kind == QNameA; }
    bool isMultiname() const { return kind == Multiname || kind == MultinameA; }
    bool isRuntime() const { return kind >= RTQName && kind <= RTQNameLA; }
};

// ABC Method
class ABCMethod {
public:
    uint32 paramCount = 0;
    uint32 returnType = 0;
    std::vector<uint32> paramTypes;
    std::vector<AVM2Value> optionalValues;
    uint8 flags = 0;
    std::string name;
    
    // Method body (optional)
    bool hasBody = false;
    std::vector<uint8> code;
    uint32 maxStack = 0;
    uint32 maxLocals = 0;
    uint32 initScopeDepth = 0;
    uint32 maxScopeDepth = 0;
    
    // Exception handlers
    struct ExceptionInfo {
        uint32 from = 0;
        uint32 to = 0;
        uint32 target = 0;
        uint32 excType = 0;  // Multiname index
        uint32 varName = 0;  // Variable name index
        std::string typeName;
        std::string varNameStr;
    };
    std::vector<ExceptionInfo> exceptions;
    
    // Traits
    std::vector<std::shared_ptr<ABCTrait>> traits;
    
    // Flags
    static constexpr uint8 FLAG_NEED_REST = 0x01;
    static constexpr uint8 FLAG_NEED_ACTIVATION = 0x02;
    static constexpr uint8 FLAG_NEED_ARGUMENTS = 0x04;
    static constexpr uint8 FLAG_HAS_OPTIONAL = 0x08;
    static constexpr uint8 FLAG_SET_DXNS = 0x40;
    static constexpr uint8 FLAG_HAS_PARAM_NAMES = 0x80;
    
    bool needsRest() const { return flags & FLAG_NEED_REST; }
    bool needsActivation() const { return flags & FLAG_NEED_ACTIVATION; }
    bool needsArguments() const { return flags & FLAG_NEED_ARGUMENTS; }
    bool hasOptional() const { return flags & FLAG_HAS_OPTIONAL; }
};

// ABC Trait (property/method definition)
class ABCTrait {
public:
    uint32 nameIndex = 0;
    std::shared_ptr<ABCMultiname> name;
    uint8 kind = 0;
    uint8 dispKind = 0;  // Kind extracted from upper 4 bits
    
    // For slot/const
    uint32 slotId = 0;
    uint32 typeIndex = 0;
    uint32 valueIndex = 0;
    uint8 valueKind = 0;
    AVM2Value defaultValue;
    
    // For method/getter/setter/class/function
    uint32 methodIndex = 0;
    uint32 classIndex = 0;
    std::shared_ptr<ABCMethod> method;
    std::shared_ptr<ABCClass> cls;
    
    enum Kind {
        Slot = 0x00,
        Method = 0x01,
        Getter = 0x02,
        Setter = 0x03,
        Class = 0x04,
        Function = 0x05,
        Const = 0x06
    };
    
    enum Attr {
        Final = 0x01,
        Override = 0x02,
        Metadata = 0x04
    };
    
    bool isFinal() const { return dispKind & Final; }
    bool isOverride() const { return dispKind & Override; }
    bool hasMetadata() const { return dispKind & Metadata; }
    bool isSlot() const { return (kind & 0x0F) == Slot; }
    bool isConst() const { return (kind & 0x0F) == Const; }
    bool isMethod() const { return (kind & 0x0F) == Method; }
    bool isGetter() const { return (kind & 0x0F) == Getter; }
    bool isSetter() const { return (kind & 0x0F) == Setter; }
    bool isClass() const { return (kind & 0x0F) == Class; }
    bool isFunction() const { return (kind & 0x0F) == Function; }
};

// ABC Instance (class instance info)
class ABCInstance {
public:
    uint32 nameIndex = 0;
    std::shared_ptr<ABCMultiname> name;
    uint32 superIndex = 0;
    std::shared_ptr<AVM2Class> superClass;
    uint8 flags = 0;
    uint32 protectedNs = 0;
    std::shared_ptr<AVM2Namespace> protectedNamespace;
    std::vector<std::shared_ptr<ABCTrait>> traits;
    uint32 initIndex = 0;
    std::shared_ptr<ABCMethod> initializer;
    
    static constexpr uint8 FLAG_SEALED = 0x01;
    static constexpr uint8 FLAG_FINAL = 0x02;
    static constexpr uint8 FLAG_INTERFACE = 0x04;
    static constexpr uint8 FLAG_PROTECTEDNS = 0x08;
    
    bool isSealed() const { return flags & FLAG_SEALED; }
    bool isFinal() const { return flags & FLAG_FINAL; }
    bool isInterface() const { return flags & FLAG_INTERFACE; }
    bool hasProtectedNs() const { return flags & FLAG_PROTECTEDNS; }
};

// ABC Class (static class info)
class ABCClass {
public:
    uint32 nameIndex = 0;
    std::shared_ptr<ABCMultiname> name;
    uint32 superIndex = 0;
    std::shared_ptr<AVM2Class> superClass;
    uint8 flags = 0;
    std::vector<std::shared_ptr<ABCTrait>> traits;
    uint32 initIndex = 0;
    std::shared_ptr<ABCMethod> staticInitializer;
};

// ABC File structure (ActionScript Bytecode)
class ABCFile {
public:
    uint16 minorVersion = 0;
    uint16 majorVersion = 0;
    
    // Constant pool
    std::vector<int32> intPool;
    std::vector<uint32> uintPool;
    std::vector<double> doublePool;
    std::vector<std::string> stringPool;
    std::vector<std::shared_ptr<AVM2Namespace>> namespacePool;
    std::vector<std::shared_ptr<AVM2NamespaceSet>> namespaceSetPool;
    std::vector<std::shared_ptr<ABCMultiname>> multinamePool;
    
    // Methods and classes
    std::vector<std::shared_ptr<ABCMethod>> methods;
    std::vector<std::shared_ptr<ABCClass>> classes;
    std::vector<std::shared_ptr<ABCInstance>> instances;
    std::vector<std::shared_ptr<AVM2Class>> avm2Classes;
    
    // Scripts
    std::vector<std::shared_ptr<ABCMethod>> scripts;
    
    // Method bodies
    std::vector<std::shared_ptr<ABCMethod>> methodBodies;
    
    bool parse(const uint8* data, size_t size);
    
    // Constant pool access
    int32 getInt(uint32 index) const { return (index > 0 && index < intPool.size()) ? intPool[index] : 0; }
    uint32 getUInt(uint32 index) const { return (index > 0 && index < uintPool.size()) ? uintPool[index] : 0; }
    double getDouble(uint32 index) const { return (index > 0 && index < doublePool.size()) ? doublePool[index] : 0.0; }
    std::string getString(uint32 index) const { return (index > 0 && index < stringPool.size()) ? stringPool[index] : ""; }
    std::shared_ptr<AVM2Namespace> getNamespace(uint32 index) const { 
        return (index > 0 && index < namespacePool.size()) ? namespacePool[index] : nullptr; 
    }
    std::shared_ptr<AVM2NamespaceSet> getNamespaceSet(uint32 index) const {
        return (index > 0 && index < namespaceSetPool.size()) ? namespaceSetPool[index] : nullptr;
    }
    std::shared_ptr<ABCMultiname> getMultiname(uint32 index) const {
        return (index > 0 && index < multinamePool.size()) ? multinamePool[index] : nullptr;
    }
    
private:
    size_t pos = 0;
    const uint8* data = nullptr;
    size_t size = 0;
    
    uint8 readU8();
    uint16 readU16();
    uint32 readU32();
    int32 readS32();
    double readD64();
    std::string readString();
    
    void parseConstantPool();
    void parseMethods();
    void parseMetadata();
    void parseInstances();
    void parseClasses();
    void parseScripts();
    void parseMethodBodies();
};

// AVM2 Execution Context
class AVM2Context {
public:
    std::shared_ptr<AVM2Object> globalObject;
    std::shared_ptr<AVM2Object> scopeObject;
    std::vector<std::shared_ptr<AVM2Object>> scopeStack;
    
    // Optimized operand stack using fixed-size array + pointer (faster than vector)
    static constexpr size_t MAX_STACK_SIZE = 65536;
    std::unique_ptr<AVM2Value[]> stackBuffer;
    AVM2Value* stackTop = nullptr;
    AVM2Value* stackBase = nullptr;
    
    std::vector<AVM2Value> localRegisters;
    std::vector<AVM2Value> capturedScope;
    
    // Call stack
    struct CallFrame {
        std::shared_ptr<ABCMethod> method;
        std::shared_ptr<AVM2Object> scope;
        std::shared_ptr<AVM2Object> thisObj;
        size_t pc = 0;
        size_t savedStackSize = 0;
        std::vector<AVM2Value> localRegisters;
    };
    std::vector<CallFrame> callStack;
    
    // Exception handling
    std::optional<AVM2Exception> pendingException;
    bool isInExceptionHandler = false;
    
    // Native function handlers
    std::unordered_map<std::string, std::function<AVM2Value(AVM2Context&, const std::vector<AVM2Value>&)>> nativeFunctions;
    
    AVM2Context();
    
    void pushScope(std::shared_ptr<AVM2Object> obj);
    std::shared_ptr<AVM2Object> popScope();
    std::shared_ptr<AVM2Object> getScope(uint32 index) const;
    
    void push(const AVM2Value& val) { 
        if (stackTop < stackBase + MAX_STACK_SIZE) {
            *stackTop++ = val;
        }
    }
    AVM2Value pop() { 
        if (stackTop > stackBase) {
            return *--stackTop;
        }
        return AVM2Value();
    }
    AVM2Value& top() { return *(stackTop - 1); }
    size_t stackSize() const { return stackTop - stackBase; }
    void clearStack() { stackTop = stackBase; }
    
    void saveState(CallFrame& frame);
    void restoreState(const CallFrame& frame);
    
    void throwException(const AVM2Value& value);
    void throwException(const std::string& message, const std::string& type = "Error");
    
private:
    void initNativeFunctions();
};

// Garbage Collector with generational collection
class AVM2GarbageCollector {
public:
    void collect(const std::vector<std::shared_ptr<AVM2Object>>& roots);
    void collectYoung(const std::vector<std::shared_ptr<AVM2Object>>& roots);  // Minor GC
    void addObject(std::shared_ptr<AVM2Object> obj);
    void removeObject(std::shared_ptr<AVM2Object> obj);
    size_t getObjectCount() const { return youngObjects.size() + oldObjects.size(); }
    
    // GC tuning
    void setYoungGenSize(size_t size) { youngGenThreshold = size; }
    void maybeCollectYoung(const std::vector<std::shared_ptr<AVM2Object>>& roots);
    
private:
    // Young generation (nursery) - frequently collected
    std::list<std::weak_ptr<AVM2Object>> youngObjects;
    // Old generation - less frequently collected
    std::list<std::weak_ptr<AVM2Object>> oldObjects;
    
    size_t youngGenThreshold = 1024;  // Collect young gen when it reaches this size
    size_t allocationCount = 0;
    
    void mark(const std::vector<std::shared_ptr<AVM2Object>>& roots, bool youngOnly);
    void sweepYoung();
    void sweepOld();
    void promoteToOld(std::shared_ptr<AVM2Object> obj);
};

// AVM2 Virtual Machine
class AVM2 {
public:
    AVM2();
    ~AVM2();
    
    // Load ABC file
    bool loadABC(const std::vector<uint8>& data);
    bool loadABC(const uint8* data, size_t size);
    
    // Execute
    void execute();
    AVM2Value callMethod(const std::string& name, const std::vector<AVM2Value>& args);
    AVM2Value callMethod(uint32 methodIndex, const std::vector<AVM2Value>& args);
    AVM2Value callMethod(std::shared_ptr<ABCMethod> method, std::shared_ptr<AVM2Object> thisObj, 
                         const std::vector<AVM2Value>& args);
    
    // Get global object
    std::shared_ptr<AVM2Object> getGlobal() { return context_.globalObject; }
    
    // Get context
    AVM2Context& getContext() { return context_; }
    
    // Get ABC file
    std::shared_ptr<ABCFile> getABCFile() { return abcFile_; }
    
    // Error handling
    const std::string& getError() const { return error_; }
    bool hasError() const { return !error_.empty(); }
    void clearError() { error_.clear(); }
    
    // Class management
    std::shared_ptr<AVM2Class> getClass(const std::string& name);
    std::shared_ptr<AVM2Class> getClass(uint32 index);
    void registerClass(std::shared_ptr<AVM2Class> cls);
    
    // Garbage collection
    void collectGarbage();
    size_t getObjectCount() const { return gc_.getObjectCount(); }
    
    // Register native function
    void registerNativeFunction(const std::string& name, 
        std::function<AVM2Value(AVM2Context&, const std::vector<AVM2Value>&)> func);

private:
    AVM2Context context_;
    std::shared_ptr<ABCFile> abcFile_;
    std::string error_;
    AVM2GarbageCollector gc_;
    
    std::unordered_map<std::string, std::shared_ptr<AVM2Class>> classMap_;
    
    // Method call cache - cache resolved methods by (object, method_name) pair
    struct MethodCacheEntry {
        std::weak_ptr<AVM2Object> object;
        const std::string* methodName = nullptr;
        std::weak_ptr<AVM2Function> method;
        uint64_t hitCount = 0;
    };
    static constexpr size_t METHOD_CACHE_SIZE = 256;
    std::vector<MethodCacheEntry> methodCache_{METHOD_CACHE_SIZE};
    
    // Property access cache (per-thread)
    PropertyCacheEntry propertyCache_[16];  // Small inline cache
    
    // Execution
    void executeMethod(std::shared_ptr<ABCMethod> method, 
                       std::shared_ptr<AVM2Object> thisObj,
                       std::shared_ptr<AVM2Object> scope,
                       const std::vector<AVM2Value>& args);
    
    bool executeInstruction(const uint8* code, size_t& pc, size_t codeSize);
    
    // Exception handling
    bool findExceptionHandler(const uint8* code, size_t& pc, const AVM2Exception& exc);
    
    // Opcode handlers
    void op_nop();
    void op_throw();
    void op_getsuper(const uint8* code, size_t& pc);
    void op_setsuper(const uint8* code, size_t& pc);
    void op_dup();
    void op_pop();
    void op_dup_scope();
    void op_push_scope();
    void op_pop_scope();
    void op_label();
    void op_jump(const uint8* code, size_t& pc);
    void op_iftrue(const uint8* code, size_t& pc);
    void op_iffalse(const uint8* code, size_t& pc);
    void op_ifeq(const uint8* code, size_t& pc);
    void op_ifne(const uint8* code, size_t& pc);
    void op_iflt(const uint8* code, size_t& pc);
    void op_ifle(const uint8* code, size_t& pc);
    void op_ifgt(const uint8* code, size_t& pc);
    void op_ifge(const uint8* code, size_t& pc);
    void op_ifstricteq(const uint8* code, size_t& pc);
    void op_ifstrictne(const uint8* code, size_t& pc);
    void op_lookupswitch(const uint8* code, size_t& pc);
    void op_push_with();
    void op_pop_scope_alias();
    void op_next_name();
    void op_next_value();
    void op_push_null();
    void op_push_undefined();
    void op_push_true();
    void op_push_false();
    void op_push_nan();
    void op_push_int(const uint8* code, size_t& pc);
    void op_push_uint(const uint8* code, size_t& pc);
    void op_push_double(const uint8* code, size_t& pc);
    void op_push_string(const uint8* code, size_t& pc);
    void op_push_namespace(const uint8* code, size_t& pc);
    void op_jump_alias(const uint8* code, size_t& pc);
    void op_iftrue_alias(const uint8* code, size_t& pc);
    void op_iffalse_alias(const uint8* code, size_t& pc);
    void op_new_object(const uint8* code, size_t& pc);
    void op_new_array(const uint8* code, size_t& pc);
    void op_new_activation();
    void op_new_class(const uint8* code, size_t& pc);
    void op_new_catch(const uint8* code, size_t& pc);
    void op_find_prop_strict(const uint8* code, size_t& pc);
    void op_find_property(const uint8* code, size_t& pc);
    void op_find_def(const uint8* code, size_t& pc);
    void op_get_lex(const uint8* code, size_t& pc);
    void op_set_property(const uint8* code, size_t& pc);
    void op_get_local(uint32 index);
    void op_set_local(uint32 index);
    void op_get_global_scope();
    void op_get_scope_object(const uint8* code, size_t& pc);
    void op_get_property(const uint8* code, size_t& pc);
    void op_init_property(const uint8* code, size_t& pc);
    void op_delete_property(const uint8* code, size_t& pc);
    void op_get_slot(const uint8* code, size_t& pc);
    void op_set_slot(const uint8* code, size_t& pc);
    void op_get_global_slot(const uint8* code, size_t& pc);
    void op_set_global_slot(const uint8* code, size_t& pc);
    void op_convert_s();
    void op_esc_xelem();
    void op_esc_xattr();
    void op_convert_i();
    void op_convert_u();
    void op_convert_d();
    void op_convert_b();
    void op_convert_o();
    void op_check_filter();
    void op_coerce(const uint8* code, size_t& pc);
    void op_coerce_a();
    void op_coerce_s();
    void op_as_type(const uint8* code, size_t& pc);
    void op_as_type_late();
    void op_coerce_o();
    void op_negate();
    void op_increment();
    void op_inclocal(const uint8* code, size_t& pc);
    void op_decrement();
    void op_declocal(const uint8* code, size_t& pc);
    void op_type_of();
    void op_not();
    void op_bit_not();
    void op_add();
    void op_subtract();
    void op_multiply();
    void op_divide();
    void op_modulo();
    void op_lshift();
    void op_rshift();
    void op_urshift();
    void op_bit_and();
    void op_bit_or();
    void op_bit_xor();
    void op_equals();
    void op_strictequals();
    void op_lessthan();
    void op_lessequals();
    void op_greaterthan();
    void op_greaterequals();
    void op_instance_of();
    void op_is_type(const uint8* code, size_t& pc);
    void op_is_type_late();
    void op_in();
    void op_increment_i();
    void op_decrement_i();
    void op_inclocal_i(const uint8* code, size_t& pc);
    void op_declocal_i(const uint8* code, size_t& pc);
    void op_negate_i();
    void op_add_i();
    void op_subtract_i();
    void op_multiply_i();
    void op_get_local0();
    void op_get_local1();
    void op_get_local2();
    void op_get_local3();
    void op_set_local0();
    void op_set_local1();
    void op_set_local2();
    void op_set_local3();
    void op_debug();
    void op_debug_line(const uint8* code, size_t& pc);
    void op_debug_file(const uint8* code, size_t& pc);
    void op_bkpt_line();
    void op_timestamp();
    
    // Function/Method calls
    void op_call(const uint8* code, size_t& pc);
    void op_construct(const uint8* code, size_t& pc);
    void op_call_method(const uint8* code, size_t& pc);
    void op_call_static(const uint8* code, size_t& pc);
    void op_call_super(const uint8* code, size_t& pc);
    void op_call_property(const uint8* code, size_t& pc);
    void op_return_void();
    void op_return_value();
    void op_construct_super(const uint8* code, size_t& pc);
    void op_construct_prop(const uint8* code, size_t& pc);
    void op_call_super_id(const uint8* code, size_t& pc);
    void op_call_prop_void(const uint8* code, size_t& pc);
    void op_call_super_void(const uint8* code, size_t& pc);
    void op_apply_type(const uint8* code, size_t& pc);
    void op_new_function(const uint8* code, size_t& pc);
    void op_new_class_alias(const uint8* code, size_t& pc);
    void op_get_descendants(const uint8* code, size_t& pc);
    void op_get_super_method(const uint8* code, size_t& pc);
    void op_get_super_init(const uint8* code, size_t& pc);
    void op_get_super_accessor(const uint8* code, size_t& pc);
    void op_get_slot_by_index(const uint8* code, size_t& pc);
    void op_set_slot_by_index(const uint8* code, size_t& pc);
    
    // Helper methods
    AVM2Value readConstant(uint32 index);
    std::shared_ptr<ABCMultiname> readMultiname(const uint8* code, size_t& pc);
    AVM2Value resolveProperty(const std::string& name);
    AVM2Value resolveMultiname(std::shared_ptr<ABCMultiname> multiname);
    AVM2Value toPrimitive(const AVM2Value& val);
    double toNumber(const AVM2Value& val);
    int32_t toInt32(const AVM2Value& val);
    uint32_t toUInt32(const AVM2Value& val);
    std::string toString(const AVM2Value& val);
    bool toBoolean(const AVM2Value& val);
    
    // Method cache
    std::shared_ptr<AVM2Function> lookupMethodCached(std::shared_ptr<AVM2Object> obj, const std::string& name);
    
    // Opcode dispatch optimization
    using OpcodeHandler = void (AVM2::*)(const uint8* code, size_t& pc);
    OpcodeHandler opcodeTable[256];
    void initOpcodeTable();
};

} // namespace libswf

#endif // LIBSWF_AVM2_AVM2_H
