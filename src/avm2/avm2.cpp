#include "avm2/avm2.h"
#include <cmath>
#include <sstream>
#include <algorithm>
#include <iostream>
#include <cstring>
#include <type_traits>
#include <mutex>

namespace libswf {

// ============================================================================
// String Interning Pool
// ============================================================================

AVM2StringPool& AVM2StringPool::getInstance() {
    static AVM2StringPool instance;
    return instance;
}

const std::string* AVM2StringPool::intern(const std::string& str) {
    std::shared_lock<std::shared_mutex> readLock(mutex);
    auto it = pool.find(str);
    if (it != pool.end()) {
        return &(*it);
    }
    readLock.unlock();
    
    std::unique_lock<std::shared_mutex> writeLock(mutex);
    // Double-check after acquiring write lock
    it = pool.find(str);
    if (it != pool.end()) {
        return &(*it);
    }
    auto [newIt, _] = pool.insert(str);
    return &(*newIt);
}

const std::string* AVM2StringPool::intern(std::string&& str) {
    std::shared_lock<std::shared_mutex> readLock(mutex);
    auto it = pool.find(str);
    if (it != pool.end()) {
        return &(*it);
    }
    readLock.unlock();
    
    std::unique_lock<std::shared_mutex> writeLock(mutex);
    it = pool.find(str);
    if (it != pool.end()) {
        return &(*it);
    }
    auto [newIt, _] = pool.insert(std::move(str));
    return &(*newIt);
}

// Initialize thread-local cache version
thread_local uint64_t PropertyCacheEntry::globalVersion = 1;

// ============================================================================
// AVM2Object Implementation
// ============================================================================

AVM2Value AVM2Object::getProperty(const std::string& name) {
    // Use string interning for faster lookup
    const std::string* interned = AVM2StringPool::getInstance().intern(name);
    auto it = properties.find(interned);
    if (it != properties.end()) {
        return it->second;
    }
    
    // Check prototype chain
    if (prototype) {
        return prototype->getProperty(name);
    }
    
    return std::monostate();
}

void AVM2Object::setProperty(const std::string& name, const AVM2Value& value) {
    const std::string* interned = AVM2StringPool::getInstance().intern(name);
    properties[interned] = value;
    bumpVersion();
}

bool AVM2Object::hasProperty(const std::string& name) const {
    const std::string* interned = AVM2StringPool::getInstance().intern(name);
    if (properties.find(interned) != properties.end()) {
        return true;
    }
    if (prototype) {
        return prototype->hasProperty(name);
    }
    return false;
}

bool AVM2Object::deleteProperty(const std::string& name) {
    const std::string* interned = AVM2StringPool::getInstance().intern(name);
    auto it = properties.find(interned);
    if (it != properties.end()) {
        properties.erase(it);
        bumpVersion();
        return true;
    }
    return false;
}

// Fast property access with inline caching
AVM2Value AVM2Object::getPropertyFast(const std::string& name, PropertyCacheEntry& cache) {
    // Get interned string pointer for consistent comparison
    const std::string* interned = AVM2StringPool::getInstance().intern(name);
    
    // Check cache validity
    if (cache.name == interned && cache.version == version) {
        return *cache.valuePtr;
    }
    
    // Cache miss - do lookup
    auto it = properties.find(interned);
    if (it != properties.end()) {
        // Update cache
        cache.name = interned;
        cache.valuePtr = &it->second;
        cache.version = version;
        return it->second;
    }
    
    // Check prototype chain (don't cache prototype lookups for simplicity)
    if (prototype) {
        return prototype->getProperty(name);
    }
    
    return std::monostate();
}

void AVM2Object::setPropertyFast(const std::string& name, const AVM2Value& value, PropertyCacheEntry& cache) {
    const std::string* interned = AVM2StringPool::getInstance().intern(name);
    auto it = properties.find(interned);
    if (it != properties.end()) {
        // Update existing property and cache
        it->second = value;
        cache.name = interned;
        cache.valuePtr = &it->second;
        cache.version = version;
    } else {
        // New property
        auto [newIt, _] = properties.emplace(interned, value);
        cache.name = interned;
        cache.valuePtr = &newIt->second;
        cache.version = version;
    }
    bumpVersion();
}

AVM2Value AVM2Object::call(const std::vector<AVM2Value>& args) {
    return std::monostate();
}

AVM2Value AVM2Object::construct(const std::vector<AVM2Value>& args) {
    return std::monostate();
}

std::string AVM2Object::toString() const {
    if (class_) {
        return "[object " + class_->name + "]";
    }
    return "[object Object]";
}

AVM2Value AVM2Object::getPropertyMultiname(const std::shared_ptr<ABCMultiname>& multiname) {
    if (!multiname) return std::monostate();
    return getProperty(multiname->name);
}

void AVM2Object::setPropertyMultiname(const std::shared_ptr<ABCMultiname>& multiname, const AVM2Value& value) {
    if (!multiname) return;
    setProperty(multiname->name, value);
}

void AVM2Object::mark() const {
    if (gcMarked) return;
    gcMarked = true;
    
    // Mark all reachable objects
    for (const auto& [name, val] : properties) {
        if (std::holds_alternative<std::shared_ptr<AVM2Object>>(val)) {
            std::get<std::shared_ptr<AVM2Object>>(val)->mark();
        }
    }
    if (prototype) prototype->mark();
    if (class_) class_->mark();
}

void AVM2Object::unmark() const {
    gcMarked = false;
}

// ============================================================================
// AVM2Array Implementation
// ============================================================================

AVM2Value AVM2Array::getProperty(const std::string& name) {
    // Fast path for "length" - compare pointers after interning
    static const std::string* lengthStr = AVM2StringPool::getInstance().intern("length");
    const std::string* interned = AVM2StringPool::getInstance().intern(name);
    
    if (interned == lengthStr) {
        return static_cast<int32>(elements.size());
    }
    
    // Try to parse as index
    char* endptr = nullptr;
    unsigned long idx = strtoul(name.c_str(), &endptr, 10);
    if (endptr != name.c_str() && *endptr == '\0' && idx < elements.size()) {
        return elements[idx];
    }
    
    return AVM2Object::getProperty(name);
}

void AVM2Array::setProperty(const std::string& name, const AVM2Value& value) {
    static const std::string* lengthStr = AVM2StringPool::getInstance().intern("length");
    const std::string* interned = AVM2StringPool::getInstance().intern(name);
    
    if (interned == lengthStr) {
        if (auto* len = std::get_if<int32>(&value)) {
            elements.resize(*len);
        }
        bumpVersion();
        return;
    }
    
    // Try to parse as index
    char* endptr = nullptr;
    unsigned long idx = strtoul(name.c_str(), &endptr, 10);
    if (endptr != name.c_str() && *endptr == '\0') {
        if (idx >= elements.size()) {
            elements.resize(idx + 1);
        }
        elements[idx] = value;
        bumpVersion();
        return;
    }
    
    AVM2Object::setProperty(name, value);
}

AVM2Value AVM2Array::call(const std::vector<AVM2Value>& args) {
    return std::monostate();
}

std::string AVM2Array::toString() const {
    std::string result;
    for (size_t i = 0; i < elements.size(); i++) {
        if (i > 0) result += ",";
        if (auto* s = std::get_if<std::string>(&elements[i])) {
            result += *s;
        } else if (auto* d = std::get_if<double>(&elements[i])) {
            result += std::to_string(*d);
        } else if (auto* n = std::get_if<int32>(&elements[i])) {
            result += std::to_string(*n);
        }
    }
    return result;
}

AVM2Value AVM2Array::pop() {
    if (elements.empty()) {
        return std::monostate();
    }
    AVM2Value val = elements.back();
    elements.pop_back();
    return val;
}

AVM2Value AVM2Array::shift() {
    if (elements.empty()) {
        return std::monostate();
    }
    AVM2Value val = elements.front();
    elements.erase(elements.begin());
    return val;
}

void AVM2Array::unshift(const AVM2Value& value) {
    elements.insert(elements.begin(), value);
}

AVM2Value AVM2Array::join(const std::string& separator) const {
    std::string result;
    for (size_t i = 0; i < elements.size(); i++) {
        if (i > 0) result += separator;
        if (auto* s = std::get_if<std::string>(&elements[i])) {
            result += *s;
        } else if (auto* d = std::get_if<double>(&elements[i])) {
            result += std::to_string(*d);
        } else if (auto* n = std::get_if<int32>(&elements[i])) {
            result += std::to_string(*n);
        }
    }
    return result;
}

void AVM2Array::splice(size_t start, size_t deleteCount, const std::vector<AVM2Value>& items) {
    if (start > elements.size()) start = elements.size();
    
    elements.erase(elements.begin() + start, elements.begin() + start + deleteCount);
    elements.insert(elements.begin() + start, items.begin(), items.end());
}

AVM2Array AVM2Array::slice(size_t start, size_t end) const {
    AVM2Array result;
    if (start >= elements.size()) return result;
    if (end > elements.size()) end = elements.size();
    
    result.elements.assign(elements.begin() + start, elements.begin() + end);
    return result;
}

void AVM2Array::reverse() {
    std::reverse(elements.begin(), elements.end());
}

void AVM2Array::sort() {
    // Simple sort - in real implementation would use comparator
    std::sort(elements.begin(), elements.end(), [](const AVM2Value& a, const AVM2Value& b) {
        if (auto* da = std::get_if<double>(&a)) {
            if (auto* db = std::get_if<double>(&b)) return *da < *db;
        }
        if (auto* ia = std::get_if<int32>(&a)) {
            if (auto* ib = std::get_if<int32>(&b)) return *ia < *ib;
        }
        return false;
    });
}

// ============================================================================
// AVM2Function Implementation
// ============================================================================

AVM2Value AVM2Function::getProperty(const std::string& name) {
    if (name == "length") {
        if (method) {
            return static_cast<int32>(method->paramCount);
        }
        return static_cast<int32>(0);
    }
    if (name == "prototype") {
        if (!prototype) {
            prototype = std::make_shared<AVM2Object>();
        }
        return prototype;
    }
    return AVM2Object::getProperty(name);
}

void AVM2Function::setProperty(const std::string& name, const AVM2Value& value) {
    AVM2Object::setProperty(name, value);
}

AVM2Value AVM2Function::call(const std::vector<AVM2Value>& args) {
    if (isNative && nativeImpl) {
        // Will be called with context in VM
        return std::monostate();
    }
    return std::monostate();
}

AVM2Value AVM2Function::construct(const std::vector<AVM2Value>& args) {
    // Create new object with prototype
    auto obj = std::make_shared<AVM2Object>();
    auto protoVal = getProperty("prototype");
    if (auto* proto = std::get_if<std::shared_ptr<AVM2Object>>(&protoVal)) {
        obj->prototype = *proto;
    }
    return obj;
}

std::shared_ptr<AVM2Function> AVM2Function::bind(std::shared_ptr<AVM2Object> thisObj) {
    auto bound = std::make_shared<AVM2Function>(*this);
    bound->boundThis = thisObj;
    return bound;
}

// ============================================================================
// AVM2Class Implementation
// ============================================================================

AVM2Value AVM2Class::getProperty(const std::string& name) {
    // Check static traits first
    for (auto& trait : staticTraits) {
        if (trait && trait->name && trait->name->name == name) {
            // Return trait value
            return std::monostate(); // Placeholder
        }
    }
    
    // Check properties using interned string
    const std::string* interned = AVM2StringPool::getInstance().intern(name);
    auto it = properties.find(interned);
    if (it != properties.end()) {
        return it->second;
    }
    
    if (name == "prototype") {
        if (!prototype) {
            prototype = std::make_shared<AVM2Object>();
        }
        return prototype;
    }
    
    return AVM2Object::getProperty(name);
}

void AVM2Class::setProperty(const std::string& name, const AVM2Value& value) {
    AVM2Object::setProperty(name, value);
}

AVM2Value AVM2Class::call(const std::vector<AVM2Value>& args) {
    // Calling a class as function - try to coerce or return undefined
    return std::monostate();
}

AVM2Value AVM2Class::construct(const std::vector<AVM2Value>& args) {
    auto instance = createInstance();
    
    // Call constructor if exists
    if (constructor) {
        // constructor->callWithThis(instance, args);
    }
    
    return instance;
}

std::shared_ptr<AVM2Object> AVM2Class::createInstance() {
    auto obj = std::make_shared<AVM2Object>();
    obj->class_ = std::static_pointer_cast<AVM2Class>(shared_from_this());
    
    // Set prototype
    if (prototype) {
        obj->prototype = prototype;
    } else if (superClass && superClass->prototype) {
        obj->prototype = superClass->prototype;
    }
    
    // Initialize instance traits
    initializeInstance(obj);
    
    return obj;
}

void AVM2Class::initializeInstance(std::shared_ptr<AVM2Object> obj) {
    // Initialize traits from super class first
    if (superClass) {
        superClass->initializeInstance(obj);
    }
    
    // Initialize this class's traits
    for (auto& trait : instanceTraits) {
        if (trait && (trait->isSlot() || trait->isConst())) {
            if (!std::get_if<std::monostate>(&trait->defaultValue)) {
                obj->setProperty(trait->name->name, trait->defaultValue);
            }
        }
    }
}

bool AVM2Class::isSubclassOf(const std::shared_ptr<AVM2Class>& other) const {
    if (!other) return false;
    if (this == other.get()) return true;
    if (superClass) return superClass->isSubclassOf(other);
    return false;
}

// ============================================================================
// AVM2Context Implementation
// ============================================================================

AVM2Context::AVM2Context() 
    : stackBuffer(std::make_unique<AVM2Value[]>(MAX_STACK_SIZE)) {
    stackBase = stackBuffer.get();
    stackTop = stackBase;
    globalObject = std::make_shared<AVM2Object>();
    scopeObject = globalObject;
    initNativeFunctions();
}

void AVM2Context::pushScope(std::shared_ptr<AVM2Object> obj) {
    if (obj) {
        scopeStack.push_back(obj);
        scopeObject = obj;
    }
}

std::shared_ptr<AVM2Object> AVM2Context::popScope() {
    if (scopeStack.empty()) return scopeObject;
    auto obj = scopeStack.back();
    scopeStack.pop_back();
    if (!scopeStack.empty()) {
        scopeObject = scopeStack.back();
    } else {
        scopeObject = globalObject;
    }
    return obj;
}

std::shared_ptr<AVM2Object> AVM2Context::getScope(uint32 index) const {
    if (index < scopeStack.size()) {
        return scopeStack[index];
    }
    return scopeObject;
}

void AVM2Context::saveState(CallFrame& frame) {
    frame.localRegisters = localRegisters;
    frame.savedStackSize = stackSize();
}

void AVM2Context::restoreState(const CallFrame& frame) {
    localRegisters = frame.localRegisters;
    // Restore stack pointer to saved position
    while (stackSize() > frame.savedStackSize) {
        pop();
    }
}

void AVM2Context::throwException(const AVM2Value& value) {
    AVM2Exception exc;
    exc.value = value;
    if (auto* s = std::get_if<std::string>(&value)) {
        exc.message = *s;
    }
    pendingException = exc;
}

void AVM2Context::throwException(const std::string& message, const std::string& type) {
    AVM2Exception exc;
    exc.message = message;
    exc.type = type;
    exc.value = message;
    pendingException = exc;
}

void AVM2Context::initNativeFunctions() {
    // trace()
    nativeFunctions["trace"] = [](AVM2Context& ctx, const std::vector<AVM2Value>& args) -> AVM2Value {
        for (size_t i = 0; i < args.size(); i++) {
            if (i > 0) std::cout << " ";
            if (auto* str = std::get_if<std::string>(&args[i])) {
                std::cout << *str;
            } else if (auto* d = std::get_if<double>(&args[i])) {
                std::cout << *d;
            } else if (auto* i32 = std::get_if<int32>(&args[i])) {
                std::cout << *i32;
            } else if (auto* b = std::get_if<bool>(&args[i])) {
                std::cout << (*b ? "true" : "false");
            } else if (std::get_if<std::monostate>(&args[i])) {
                std::cout << "undefined";
            } else if (auto* obj = std::get_if<std::shared_ptr<AVM2Object>>(&args[i])) {
                std::cout << (*obj)->toString();
            }
        }
        std::cout << std::endl;
        return AVM2Value();
    };
    
    // isNaN()
    nativeFunctions["isNaN"] = [](AVM2Context& ctx, const std::vector<AVM2Value>& args) -> AVM2Value {
        if (args.empty()) return true;
        if (auto* d = std::get_if<double>(&args[0])) {
            return std::isnan(*d);
        }
        return false;
    };
    
    // isFinite()
    nativeFunctions["isFinite"] = [](AVM2Context& ctx, const std::vector<AVM2Value>& args) -> AVM2Value {
        if (args.empty()) return false;
        if (auto* d = std::get_if<double>(&args[0])) {
            return std::isfinite(*d);
        }
        return false;
    };
    
    // parseInt()
    nativeFunctions["parseInt"] = [](AVM2Context& ctx, const std::vector<AVM2Value>& args) -> AVM2Value {
        if (args.empty()) return std::monostate();
        std::string str;
        if (auto* s = std::get_if<std::string>(&args[0])) {
            str = *s;
        } else if (auto* d = std::get_if<double>(&args[0])) {
            str = std::to_string(static_cast<int32>(*d));
        } else if (auto* i = std::get_if<int32>(&args[0])) {
            return *i;
        }
        
        int radix = 10;
        if (args.size() > 1) {
            if (auto* r = std::get_if<int32>(&args[1])) {
                radix = *r;
            }
        }
        
        try {
            return static_cast<int32>(std::stoi(str, nullptr, radix));
        } catch (...) {
            return std::nan("");
        }
    };
    
    // parseFloat()
    nativeFunctions["parseFloat"] = [](AVM2Context& ctx, const std::vector<AVM2Value>& args) -> AVM2Value {
        if (args.empty()) return std::nan("");
        std::string str;
        if (auto* s = std::get_if<std::string>(&args[0])) {
            str = *s;
        } else if (auto* d = std::get_if<double>(&args[0])) {
            return *d;
        } else if (auto* i = std::get_if<int32>(&args[0])) {
            return static_cast<double>(*i);
        }
        
        try {
            return std::stod(str);
        } catch (...) {
            return std::nan("");
        }
    };
    
    // Number constants
    nativeFunctions["Number.NaN"] = [](AVM2Context& ctx, const std::vector<AVM2Value>& args) -> AVM2Value {
        return std::nan("");
    };
    
    nativeFunctions["Number.MAX_VALUE"] = [](AVM2Context& ctx, const std::vector<AVM2Value>& args) -> AVM2Value {
        return 1.7976931348623157e+308;
    };
    
    nativeFunctions["Number.MIN_VALUE"] = [](AVM2Context& ctx, const std::vector<AVM2Value>& args) -> AVM2Value {
        return 5e-324;
    };
    
    nativeFunctions["Number.POSITIVE_INFINITY"] = [](AVM2Context& ctx, const std::vector<AVM2Value>& args) -> AVM2Value {
        return std::numeric_limits<double>::infinity();
    };
    
    nativeFunctions["Number.NEGATIVE_INFINITY"] = [](AVM2Context& ctx, const std::vector<AVM2Value>& args) -> AVM2Value {
        return -std::numeric_limits<double>::infinity();
    };
    
    // Escape functions
    nativeFunctions["escape"] = [](AVM2Context& ctx, const std::vector<AVM2Value>& args) -> AVM2Value {
        if (args.empty()) return std::string("");
        std::string str;
        if (auto* s = std::get_if<std::string>(&args[0])) {
            str = *s;
        } else {
            // Convert to string
            return std::string("");
        }
        
        std::string result;
        for (char c : str) {
            if (std::isalnum(c) || c == '*' || c == '@' || c == '.' || c == '_' || c == '-') {
                result += c;
            } else {
                char buf[4];
                snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
                result += buf;
            }
        }
        return result;
    };
    
    nativeFunctions["unescape"] = [](AVM2Context& ctx, const std::vector<AVM2Value>& args) -> AVM2Value {
        if (args.empty()) return std::string("");
        std::string str;
        if (auto* s = std::get_if<std::string>(&args[0])) {
            str = *s;
        } else {
            return std::string("");
        }
        
        std::string result;
        for (size_t i = 0; i < str.size(); i++) {
            if (str[i] == '%' && i + 2 < str.size()) {
                int val = 0;
                sscanf(str.substr(i + 1, 2).c_str(), "%x", &val);
                result += (char)val;
                i += 2;
            } else {
                result += str[i];
            }
        }
        return result;
    };
    
    // Array constructor
    nativeFunctions["Array"] = [](AVM2Context& ctx, const std::vector<AVM2Value>& args) -> AVM2Value {
        auto arr = std::make_shared<AVM2Array>();
        for (const auto& arg : args) {
            arr->push(arg);
        }
        return arr;
    };
    
    // Object constructor
    nativeFunctions["Object"] = [](AVM2Context& ctx, const std::vector<AVM2Value>& args) -> AVM2Value {
        return std::make_shared<AVM2Object>();
    };
}

// ============================================================================
// Garbage Collector Implementation (Generational)
// ============================================================================

void AVM2GarbageCollector::collect(const std::vector<std::shared_ptr<AVM2Object>>& roots) {
    // Full collection - both young and old
    mark(roots, false);
    sweepYoung();
    sweepOld();
}

void AVM2GarbageCollector::collectYoung(const std::vector<std::shared_ptr<AVM2Object>>& roots) {
    // Minor collection - only young generation
    mark(roots, true);
    sweepYoung();
}

void AVM2GarbageCollector::maybeCollectYoung(const std::vector<std::shared_ptr<AVM2Object>>& roots) {
    if (youngObjects.size() >= youngGenThreshold) {
        collectYoung(roots);
    }
}

void AVM2GarbageCollector::addObject(std::shared_ptr<AVM2Object> obj) {
    youngObjects.push_back(obj);
    allocationCount++;
}

void AVM2GarbageCollector::removeObject(std::shared_ptr<AVM2Object> obj) {
    auto removeFunc = [&obj](const std::weak_ptr<AVM2Object>& weak) {
        auto locked = weak.lock();
        return !locked || locked == obj;
    };
    youngObjects.remove_if(removeFunc);
    oldObjects.remove_if(removeFunc);
}

void AVM2GarbageCollector::mark(const std::vector<std::shared_ptr<AVM2Object>>& roots, bool youngOnly) {
    // Mark all reachable objects
    for (auto& root : roots) {
        if (root) root->mark();
    }
}

void AVM2GarbageCollector::sweepYoung() {
    // Remove unmarked young objects, promote survivors to old gen
    youngObjects.remove_if([this](const std::weak_ptr<AVM2Object>& weak) {
        auto obj = weak.lock();
        if (!obj) return true; // Already destroyed
        
        if (!obj->gcMarked) {
            // Young object is unreachable, remove it
            return true;
        }
        
        // Object survived young collection - promote to old generation
        obj->unmark();
        oldObjects.push_back(obj);
        return true; // Remove from young
    });
}

void AVM2GarbageCollector::sweepOld() {
    // Remove unmarked old objects
    oldObjects.remove_if([](const std::weak_ptr<AVM2Object>& weak) {
        auto obj = weak.lock();
        if (!obj) return true;
        
        if (!obj->gcMarked) {
            return true;
        }
        
        obj->unmark();
        return false;
    });
}

void AVM2GarbageCollector::promoteToOld(std::shared_ptr<AVM2Object> obj) {
    oldObjects.push_back(obj);
}

// ============================================================================
// ABCFile Parsing Implementation
// ============================================================================

uint8 ABCFile::readU8() {
    if (pos >= size) return 0;
    return data[pos++];
}

uint16 ABCFile::readU16() {
    uint16 val = readU8();
    val |= (readU8() << 8);
    return val;
}

uint32 ABCFile::readU32() {
    uint32 val = 0;
    uint32 shift = 0;
    uint8 byte;
    do {
        byte = readU8();
        val |= (byte & 0x7F) << shift;
        shift += 7;
    } while (byte & 0x80);
    return val;
}

int32 ABCFile::readS32() {
    return static_cast<int32>(readU32());
}

double ABCFile::readD64() {
    double val;
    if (pos + 8 <= size) {
        memcpy(&val, data + pos, 8);
        pos += 8;
    } else {
        val = 0.0;
    }
    return val;
}

std::string ABCFile::readString() {
    uint32 len = readU32();
    if (pos + len > size) len = size - pos;
    std::string str(reinterpret_cast<const char*>(data + pos), len);
    pos += len;
    return str;
}

bool ABCFile::parse(const uint8* inputData, size_t inputSize) {
    data = inputData;
    size = inputSize;
    pos = 0;
    
    if (size < 4) return false;
    
    minorVersion = readU16();
    majorVersion = readU16();
    
    parseConstantPool();
    parseMethods();
    parseMetadata();
    parseInstances();
    parseClasses();
    parseScripts();
    parseMethodBodies();
    
    return true;
}

void ABCFile::parseConstantPool() {
    // Integer pool
    uint32 intCount = readU32();
    intPool.resize(intCount);
    for (uint32 i = 1; i < intCount; i++) {
        intPool[i] = readS32();
    }
    
    // Unsigned integer pool
    uint32 uintCount = readU32();
    uintPool.resize(uintCount);
    for (uint32 i = 1; i < uintCount; i++) {
        uintPool[i] = readU32();
    }
    
    // Double pool
    uint32 doubleCount = readU32();
    doublePool.resize(doubleCount);
    for (uint32 i = 1; i < doubleCount; i++) {
        doublePool[i] = readD64();
    }
    
    // String pool
    uint32 stringCount = readU32();
    stringPool.resize(stringCount);
    for (uint32 i = 1; i < stringCount; i++) {
        stringPool[i] = readString();
    }
    
    // Namespace pool
    uint32 nsCount = readU32();
    namespacePool.resize(nsCount);
    for (uint32 i = 1; i < nsCount; i++) {
        auto ns = std::make_shared<AVM2Namespace>();
        ns->kind = readU8();
        ns->name = getString(readU32());
        namespacePool[i] = ns;
    }
    
    // Namespace set pool
    uint32 nsSetCount = readU32();
    namespaceSetPool.resize(nsSetCount);
    for (uint32 i = 1; i < nsSetCount; i++) {
        auto nsSet = std::make_shared<AVM2NamespaceSet>();
        uint32 count = readU32();
        for (uint32 j = 0; j < count; j++) {
            uint32 nsIndex = readU32();
            if (nsIndex < namespacePool.size()) {
                nsSet->namespaces.push_back(namespacePool[nsIndex]);
            }
        }
        namespaceSetPool[i] = nsSet;
    }
    
    // Multiname pool
    uint32 multinameCount = readU32();
    multinamePool.resize(multinameCount);
    for (uint32 i = 1; i < multinameCount; i++) {
        auto multiname = std::make_shared<ABCMultiname>();
        multiname->kind = readU8();
        
        switch (multiname->kind) {
            case ABCMultiname::QName:
            case ABCMultiname::QNameA: {
                uint32 nsIndex = readU32();
                uint32 nameIndex = readU32();
                multiname->ns = getNamespace(nsIndex);
                multiname->name = getString(nameIndex);
                break;
            }
            case ABCMultiname::RTQName:
            case ABCMultiname::RTQNameA: {
                uint32 nameIndex = readU32();
                multiname->name = getString(nameIndex);
                multiname->hasRuntimeNamespace = true;
                break;
            }
            case ABCMultiname::RTQNameL:
            case ABCMultiname::RTQNameLA: {
                multiname->hasRuntimeName = true;
                multiname->hasRuntimeNamespace = true;
                break;
            }
            case ABCMultiname::Multiname:
            case ABCMultiname::MultinameA: {
                uint32 nameIndex = readU32();
                uint32 nsSetIndex = readU32();
                multiname->name = getString(nameIndex);
                multiname->nsSet = getNamespaceSet(nsSetIndex);
                break;
            }
            case ABCMultiname::MultinameL:
            case ABCMultiname::MultinameLA: {
                uint32 nsSetIndex = readU32();
                multiname->nsSet = getNamespaceSet(nsSetIndex);
                multiname->hasRuntimeName = true;
                break;
            }
        }
        
        multinamePool[i] = multiname;
    }
}

void ABCFile::parseMethods() {
    uint32 methodCount = readU32();
    methods.resize(methodCount);
    
    for (uint32 i = 0; i < methodCount; i++) {
        auto method = std::make_shared<ABCMethod>();
        method->paramCount = readU32();
        method->returnType = readU32();
        
        method->paramTypes.resize(method->paramCount);
        for (uint32 j = 0; j < method->paramCount; j++) {
            method->paramTypes[j] = readU32();
        }
        
        method->name = getString(readU32());
        method->flags = readU8();
        
        if (method->hasOptional()) {
            uint32 optionalCount = readU32();
            method->optionalValues.resize(optionalCount);
            for (uint32 j = 0; j < optionalCount; j++) {
                uint32 valIndex = readU32();
                uint8 kind = readU8();
                // Convert based on kind
                method->optionalValues[j] = std::monostate();
            }
        }
        
        // Skip param names if present
        if (method->flags & ABCMethod::FLAG_HAS_PARAM_NAMES) {
            for (uint32 j = 0; j < method->paramCount; j++) {
                readU32(); // param name
            }
        }
        
        methods[i] = method;
    }
}

void ABCFile::parseMetadata() {
    uint32 metadataCount = readU32();
    for (uint32 i = 0; i < metadataCount; i++) {
        readU32(); // name index
        uint32 valuesCount = readU32();
        for (uint32 j = 0; j < valuesCount; j++) {
            readU32(); // key
            readU32(); // value
        }
    }
}

void ABCFile::parseInstances() {
    uint32 instanceCount = readU32();
    instances.resize(instanceCount);
    
    for (uint32 i = 0; i < instanceCount; i++) {
        auto instance = std::make_shared<ABCInstance>();
        instance->nameIndex = readU32();
        instance->name = getMultiname(instance->nameIndex);
        instance->superIndex = readU32();
        instance->flags = readU8();
        
        if (instance->hasProtectedNs()) {
            instance->protectedNs = readU32();
            instance->protectedNamespace = getNamespace(instance->protectedNs);
        }
        
        // Interfaces
        uint32 interfaceCount = readU32();
        for (uint32 j = 0; j < interfaceCount; j++) {
            readU32(); // interface
        }
        
        instance->initIndex = readU32();
        if (instance->initIndex < methods.size()) {
            instance->initializer = methods[instance->initIndex];
        }
        
        // Traits
        uint32 traitCount = readU32();
        instance->traits.resize(traitCount);
        for (uint32 j = 0; j < traitCount; j++) {
            auto trait = std::make_shared<ABCTrait>();
            trait->nameIndex = readU32();
            trait->name = getMultiname(trait->nameIndex);
            trait->kind = readU8();
            trait->dispKind = trait->kind >> 4;
            trait->kind &= 0x0F;
            
            switch (trait->kind) {
                case ABCTrait::Slot:
                case ABCTrait::Const: {
                    trait->slotId = readU32();
                    trait->typeIndex = readU32();
                    trait->valueIndex = readU32();
                    if (trait->valueIndex > 0) {
                        uint8 valueKind = readU8();
                        // Resolve value based on kind
                    }
                    break;
                }
                case ABCTrait::Method:
                case ABCTrait::Getter:
                case ABCTrait::Setter: {
                    trait->slotId = readU32();
                    trait->methodIndex = readU32();
                    if (trait->methodIndex < methods.size()) {
                        trait->method = methods[trait->methodIndex];
                    }
                    break;
                }
                case ABCTrait::Class: {
                    trait->slotId = readU32();
                    trait->classIndex = readU32();
                    break;
                }
                case ABCTrait::Function: {
                    trait->slotId = readU32();
                    trait->methodIndex = readU32();
                    if (trait->methodIndex < methods.size()) {
                        trait->method = methods[trait->methodIndex];
                    }
                    break;
                }
            }
            
            instance->traits[j] = trait;
        }
        
        instances[i] = instance;
    }
}

void ABCFile::parseClasses() {
    uint32 classCount = instances.size();
    classes.resize(classCount);
    avm2Classes.resize(classCount);
    
    for (uint32 i = 0; i < classCount; i++) {
        auto cls = std::make_shared<ABCClass>();
        cls->initIndex = readU32();
        if (cls->initIndex < methods.size()) {
            cls->staticInitializer = methods[cls->initIndex];
        }
        
        // Traits
        uint32 traitCount = readU32();
        cls->traits.resize(traitCount);
        for (uint32 j = 0; j < traitCount; j++) {
            auto trait = std::make_shared<ABCTrait>();
            trait->nameIndex = readU32();
            trait->name = getMultiname(trait->nameIndex);
            trait->kind = readU8();
            trait->dispKind = trait->kind >> 4;
            trait->kind &= 0x0F;
            
            switch (trait->kind) {
                case ABCTrait::Slot:
                case ABCTrait::Const: {
                    trait->slotId = readU32();
                    trait->typeIndex = readU32();
                    trait->valueIndex = readU32();
                    if (trait->valueIndex > 0) {
                        readU8(); // value kind
                    }
                    break;
                }
                case ABCTrait::Method:
                case ABCTrait::Getter:
                case ABCTrait::Setter: {
                    trait->slotId = readU32();
                    trait->methodIndex = readU32();
                    if (trait->methodIndex < methods.size()) {
                        trait->method = methods[trait->methodIndex];
                    }
                    break;
                }
                case ABCTrait::Class: {
                    trait->slotId = readU32();
                    trait->classIndex = readU32();
                    break;
                }
            }
            
            cls->traits[j] = trait;
        }
        
        classes[i] = cls;
        
        // Create AVM2Class
        auto avm2Class = std::make_shared<AVM2Class>();
        avm2Class->name = instances[i]->name ? instances[i]->name->name : "";
        avm2Class->abcClass = cls;
        avm2Class->abcInstance = instances[i];
        avm2Class->instanceTraits = instances[i]->traits;
        avm2Class->staticTraits = cls->traits;
        
        if (instances[i]->initializer) {
            auto ctor = std::make_shared<AVM2Function>();
            ctor->method = instances[i]->initializer;
            avm2Class->constructor = ctor;
        }
        
        if (cls->staticInitializer) {
            auto init = std::make_shared<AVM2Function>();
            init->method = cls->staticInitializer;
            avm2Class->staticInitializer = init;
        }
        
        avm2Classes[i] = avm2Class;
    }
    
    // Link superclasses
    for (uint32 i = 0; i < classCount; i++) {
        if (instances[i]->superIndex > 0 && instances[i]->superIndex < avm2Classes.size()) {
            avm2Classes[i]->superClass = avm2Classes[instances[i]->superIndex];
        }
    }
}

void ABCFile::parseScripts() {
    uint32 scriptCount = readU32();
    scripts.resize(scriptCount);
    
    for (uint32 i = 0; i < scriptCount; i++) {
        uint32 initIndex = readU32();
        if (initIndex < methods.size()) {
            scripts[i] = methods[initIndex];
        }
        
        // Script traits
        uint32 traitCount = readU32();
        for (uint32 j = 0; j < traitCount; j++) {
            auto trait = std::make_shared<ABCTrait>();
            trait->nameIndex = readU32();
            trait->name = getMultiname(trait->nameIndex);
            trait->kind = readU8();
            trait->dispKind = trait->kind >> 4;
            trait->kind &= 0x0F;
            
            switch (trait->kind) {
                case ABCTrait::Slot:
                case ABCTrait::Const: {
                    readU32(); // slot id
                    readU32(); // type
                    uint32 valIndex = readU32();
                    if (valIndex > 0) readU8();
                    break;
                }
                case ABCTrait::Class: {
                    readU32(); // slot id
                    readU32(); // class index
                    break;
                }
                case ABCTrait::Function: {
                    readU32(); // slot id
                    readU32(); // function index
                    break;
                }
            }
        }
    }
}

void ABCFile::parseMethodBodies() {
    uint32 bodyCount = readU32();
    methodBodies.resize(bodyCount);
    
    for (uint32 i = 0; i < bodyCount; i++) {
        uint32 methodIndex = readU32();
        if (methodIndex >= methods.size()) continue;
        
        auto method = methods[methodIndex];
        method->hasBody = true;
        method->maxStack = readU32();
        method->maxLocals = readU32();
        method->initScopeDepth = readU32();
        method->maxScopeDepth = readU32();
        
        uint32 codeLength = readU32();
        method->code.resize(codeLength);
        for (uint32 j = 0; j < codeLength; j++) {
            method->code[j] = readU8();
        }
        
        // Exception handlers
        uint32 excCount = readU32();
        method->exceptions.resize(excCount);
        for (uint32 j = 0; j < excCount; j++) {
            ABCMethod::ExceptionInfo exc;
            exc.from = readU32();
            exc.to = readU32();
            exc.target = readU32();
            exc.excType = readU32();
            exc.varName = readU32();
            if (exc.excType < multinamePool.size() && multinamePool[exc.excType]) {
                exc.typeName = multinamePool[exc.excType]->name;
            }
            if (exc.varName < stringPool.size()) {
                exc.varNameStr = stringPool[exc.varName];
            }
            method->exceptions[j] = exc;
        }
        
        // Method body traits
        uint32 traitCount = readU32();
        method->traits.resize(traitCount);
        for (uint32 j = 0; j < traitCount; j++) {
            auto trait = std::make_shared<ABCTrait>();
            trait->nameIndex = readU32();
            trait->name = getMultiname(trait->nameIndex);
            trait->kind = readU8();
            trait->dispKind = trait->kind >> 4;
            trait->kind &= 0x0F;
            
            switch (trait->kind) {
                case ABCTrait::Slot:
                case ABCTrait::Const: {
                    readU32();
                    readU32();
                    uint32 valIndex = readU32();
                    if (valIndex > 0) readU8();
                    break;
                }
            }
            method->traits[j] = trait;
        }
        
        methodBodies[i] = method;
    }
}

// ============================================================================
// AVM2 Implementation
// ============================================================================

AVM2::AVM2() {
    abcFile_ = std::make_shared<ABCFile>();
    initOpcodeTable();
}

void AVM2::initOpcodeTable() {
    // Opcode jump table initialization - reserved for future optimization
    // Currently using switch-based dispatch which is easier to maintain
    (void)opcodeTable; // Suppress unused warning
}

AVM2::~AVM2() {}

bool AVM2::loadABC(const std::vector<uint8>& data) {
    return loadABC(data.data(), data.size());
}

bool AVM2::loadABC(const uint8* data, size_t size) {
    try {
        abcFile_ = std::make_shared<ABCFile>();
        if (!abcFile_->parse(data, size)) {
            error_ = "Failed to parse ABC file";
            return false;
        }
        
        // Register all classes
        for (auto& cls : abcFile_->avm2Classes) {
            if (cls) {
                registerClass(cls);
            }
        }
        
        return true;
    } catch (const std::exception& e) {
        error_ = std::string("Error loading ABC: ") + e.what();
        return false;
    }
}

void AVM2::execute() {
    // Execute scripts
    for (auto& script : abcFile_->scripts) {
        if (script) {
            executeMethod(script, context_.globalObject, context_.globalObject, {});
            if (context_.pendingException) {
                error_ = "Uncaught exception: " + context_.pendingException->message;
                context_.pendingException.reset();
            }
        }
    }
}

AVM2Value AVM2::callMethod(const std::string& name, const std::vector<AVM2Value>& args) {
    // Find method by name
    for (auto& m : abcFile_->methods) {
        if (m && m->name == name) {
            return callMethod(m, context_.globalObject, args);
        }
    }
    return std::monostate();
}

AVM2Value AVM2::callMethod(uint32 methodIndex, const std::vector<AVM2Value>& args) {
    if (methodIndex >= abcFile_->methods.size()) {
        error_ = "Method index out of bounds";
        return std::monostate();
    }
    
    auto method = abcFile_->methods[methodIndex];
    return callMethod(method, context_.scopeObject, args);
}

AVM2Value AVM2::callMethod(std::shared_ptr<ABCMethod> method, 
                           std::shared_ptr<AVM2Object> thisObj,
                           const std::vector<AVM2Value>& args) {
    if (!method) return std::monostate();
    
    executeMethod(method, thisObj, context_.scopeObject, args);
    
    if (context_.stackSize() > 0) {
        return context_.pop();
    }
    return std::monostate();
}

std::shared_ptr<AVM2Class> AVM2::getClass(const std::string& name) {
    auto it = classMap_.find(name);
    if (it != classMap_.end()) {
        return it->second;
    }
    return nullptr;
}

std::shared_ptr<AVM2Class> AVM2::getClass(uint32 index) {
    if (index < abcFile_->avm2Classes.size()) {
        return abcFile_->avm2Classes[index];
    }
    return nullptr;
}

void AVM2::registerClass(std::shared_ptr<AVM2Class> cls) {
    if (cls && !cls->name.empty()) {
        classMap_[cls->name] = cls;
        context_.globalObject->setProperty(cls->name, cls);
    }
}

void AVM2::collectGarbage() {
    std::vector<std::shared_ptr<AVM2Object>> roots;
    roots.push_back(context_.globalObject);
    roots.push_back(context_.scopeObject);
    for (auto& scope : context_.scopeStack) {
        if (scope) roots.push_back(scope);
    }
    // Stack objects are temporary, they will be marked via scope or locals if needed
    gc_.collect(roots);
}

void AVM2::registerNativeFunction(const std::string& name, 
    std::function<AVM2Value(AVM2Context&, const std::vector<AVM2Value>&)> func) {
    context_.nativeFunctions[name] = func;
}

// ============================================================================
// Method Execution
// ============================================================================

void AVM2::executeMethod(std::shared_ptr<ABCMethod> method, 
                         std::shared_ptr<AVM2Object> thisObj,
                         std::shared_ptr<AVM2Object> scope,
                         const std::vector<AVM2Value>& args) {
    if (!method || !method->hasBody) return;
    
    // Save current state
    AVM2Context::CallFrame frame;
    frame.method = method;
    frame.scope = scope;
    frame.thisObj = thisObj;
    frame.pc = 0;
    frame.savedStackSize = context_.stackSize();
    
    context_.callStack.push_back(frame);
    
    // Set up execution context
    context_.scopeObject = scope;
    context_.localRegisters.resize(method->maxLocals);
    
    // Set up parameters
    context_.localRegisters[0] = thisObj ? AVM2Value(thisObj) : AVM2Value(std::monostate());
    for (size_t i = 0; i < args.size() && i < method->paramCount; i++) {
        context_.localRegisters[i + 1] = args[i];
    }
    
    // Execute code
    const uint8* code = method->code.data();
    size_t codeSize = method->code.size();
    size_t pc = 0;
    
    while (pc < codeSize) {
        if (!executeInstruction(code, pc, codeSize)) {
            break;
        }
        
        // Check for exceptions
        if (context_.pendingException) {
            if (!findExceptionHandler(code, pc, *context_.pendingException)) {
                // Exception not handled, propagate
                break;
            }
            context_.pendingException.reset();
        }
    }
    
    // Restore state
    context_.callStack.pop_back();
}

bool AVM2::findExceptionHandler(const uint8* code, size_t& pc, const AVM2Exception& exc) {
    if (context_.callStack.empty()) return false;
    
    auto& frame = context_.callStack.back();
    if (!frame.method) return false;
    
    for (auto& handler : frame.method->exceptions) {
        if (pc >= handler.from && pc < handler.to) {
            // Check exception type match
            if (handler.typeName.empty() || handler.typeName == exc.type ||
                handler.typeName == "*") {
                pc = handler.target;
                
                // Push exception object/value onto stack
                context_.push(exc.value);
                
                // Store in variable if specified
                if (!handler.varNameStr.empty()) {
                    // Find local register for varName
                }
                
                return true;
            }
        }
    }
    
    return false;
}

// ============================================================================
// Helper Functions for Instruction Execution
// ============================================================================

// Helper to read U8
static uint8_t readU8(const uint8_t* code, size_t& pc) {
    return code[pc++];
}

// Helper to read U30 (variable length encoded uint32)
static uint32_t readU30(const uint8_t* code, size_t& pc) {
    uint32_t val = 0;
    uint32_t shift = 0;
    uint8_t byte;
    do {
        byte = code[pc++];
        val |= (byte & 0x7F) << shift;
        shift += 7;
    } while (byte & 0x80);
    return val;
}

// Helper to read S24 (signed 24-bit)
static int32_t readS24(const uint8_t* code, size_t& pc) {
    int32_t val = code[pc++];
    val |= code[pc++] << 8;
    val |= code[pc++] << 16;
    if (val & 0x800000) val |= ~0xFFFFFF;
    return val;
}

// ============================================================================
// Instruction Execution
// ============================================================================

bool AVM2::executeInstruction(const uint8* code, size_t& pc, size_t codeSize) {
    if (pc >= codeSize) return false;
    
    uint8 opcode = code[pc++];
    
    try {
        switch (opcode) {
            // Control flow
            case 0x00: op_nop(); break;
            case 0x01: op_throw(); break;
            case 0x02: op_getsuper(code, pc); break;
            case 0x03: op_setsuper(code, pc); break;
            case 0x04: op_dup(); break;
            case 0x05: op_pop(); break;
            case 0x06: op_dup_scope(); break;
            case 0x07: op_push_scope(); break;
            case 0x08: op_pop_scope(); break;
            case 0x09: op_label(); break;
            case 0x0C: op_iftrue(code, pc); break;
            case 0x0D: op_iffalse(code, pc); break;
            case 0x10: op_jump(code, pc); break;
            case 0x11: op_ifeq(code, pc); break;
            case 0x12: op_ifne(code, pc); break;
            case 0x13: op_iflt(code, pc); break;
            case 0x14: op_ifle(code, pc); break;
            case 0x15: op_ifgt(code, pc); break;
            case 0x16: op_ifge(code, pc); break;
            case 0x17: op_ifstricteq(code, pc); break;
            case 0x18: op_ifstrictne(code, pc); break;
            case 0x1B: op_lookupswitch(code, pc); break;
            case 0x1C: op_push_with(); break;
            case 0x1D: op_pop_scope_alias(); break;
            case 0x1E: op_next_name(); break;
            case 0x1F: op_next_value(); break;
            
            // Literals
            case 0x20: op_push_null(); break;
            case 0x21: op_push_undefined(); break;
            case 0x22: op_push_true(); break;
            case 0x23: op_push_false(); break;
            case 0x24: op_push_nan(); break;
            case 0x25: op_push_int(code, pc); break;
            case 0x26: op_push_uint(code, pc); break;
            case 0x27: op_push_double(code, pc); break;
            case 0x28: op_push_string(code, pc); break;
            case 0x29: op_push_namespace(code, pc); break;
            
            // Operations (aliases)
            case 0x2A: op_jump_alias(code, pc); break;
            case 0x2B: op_iftrue_alias(code, pc); break;
            case 0x2C: op_iffalse_alias(code, pc); break;
            
            // Object/Array creation
            case 0x55: op_new_object(code, pc); break;
            case 0x56: op_new_array(code, pc); break;
            case 0x57: op_new_activation(); break;
            case 0x58: op_new_class(code, pc); break;
            case 0x59: op_new_catch(code, pc); break;
            
            // Property access
            case 0x5A: op_find_prop_strict(code, pc); break;
            case 0x5B: op_find_property(code, pc); break;
            case 0x5C: op_find_def(code, pc); break;
            case 0x60: op_get_lex(code, pc); break;
            case 0x61: op_set_property(code, pc); break;
            case 0x66: op_get_property(code, pc); break;
            case 0x68: op_init_property(code, pc); break;
            case 0x6A: op_delete_property(code, pc); break;
            
            // Local variables
            case 0x62: op_get_local(static_cast<uint32_t>(readU8(code, pc))); break;
            case 0x63: op_set_local(static_cast<uint32_t>(readU8(code, pc))); break;
            case 0x64: op_get_global_scope(); break;
            case 0x65: op_get_scope_object(code, pc); break;
            case 0x6C: op_get_slot(code, pc); break;
            case 0x6D: op_set_slot(code, pc); break;
            case 0x6E: op_get_global_slot(code, pc); break;
            case 0x6F: op_set_global_slot(code, pc); break;
            
            // Conversions
            case 0x70: op_convert_s(); break;
            case 0x71: op_esc_xelem(); break;
            case 0x72: op_esc_xattr(); break;
            case 0x73: op_convert_i(); break;
            case 0x74: op_convert_u(); break;
            case 0x75: op_convert_d(); break;
            case 0x76: op_convert_b(); break;
            case 0x77: op_convert_o(); break;
            case 0x78: op_check_filter(); break;
            
            // Coercion
            case 0x80: op_coerce(code, pc); break;
            case 0x81: op_coerce_a(); break;
            case 0x82: op_coerce_s(); break;
            case 0x85: op_as_type(code, pc); break;
            case 0x86: op_as_type_late(); break;
            case 0x87: op_coerce_o(); break;
            
            // Arithmetic
            case 0x90: op_negate(); break;
            case 0x91: op_increment(); break;
            case 0x92: op_inclocal(code, pc); break;
            case 0x93: op_decrement(); break;
            case 0x94: op_declocal(code, pc); break;
            case 0x95: op_type_of(); break;
            case 0x96: op_not(); break;
            case 0x97: op_bit_not(); break;
            case 0xA0: op_add(); break;
            case 0xA1: op_subtract(); break;
            case 0xA2: op_multiply(); break;
            case 0xA3: op_divide(); break;
            case 0xA4: op_modulo(); break;
            case 0xA5: op_lshift(); break;
            case 0xA6: op_rshift(); break;
            case 0xA7: op_urshift(); break;
            case 0xA8: op_bit_and(); break;
            case 0xA9: op_bit_or(); break;
            case 0xAA: op_bit_xor(); break;
            case 0xAB: op_equals(); break;
            case 0xAC: op_strictequals(); break;
            case 0xAD: op_lessthan(); break;
            case 0xAE: op_lessequals(); break;
            case 0xAF: op_greaterthan(); break;
            case 0xB0: op_greaterequals(); break;
            case 0xB1: op_instance_of(); break;
            case 0xB2: op_is_type(code, pc); break;
            case 0xB3: op_is_type_late(); break;
            case 0xB4: op_in(); break;
            
            // Integer operations
            case 0xC0: op_increment_i(); break;
            case 0xC1: op_inclocal_i(code, pc); break;
            case 0xC2: op_decrement_i(); break;
            case 0xC3: op_declocal_i(code, pc); break;
            case 0xC4: op_negate_i(); break;
            case 0xC5: op_add_i(); break;
            case 0xC6: op_subtract_i(); break;
            case 0xC7: op_multiply_i(); break;
            
            // Local register shortcuts
            case 0xD0: op_get_local0(); break;
            case 0xD1: op_get_local1(); break;
            case 0xD2: op_get_local2(); break;
            case 0xD3: op_get_local3(); break;
            case 0xD4: op_set_local0(); break;
            case 0xD5: op_set_local1(); break;
            case 0xD6: op_set_local2(); break;
            case 0xD7: op_set_local3(); break;
            
            // Debug
            case 0xEF: op_debug(); break;
            case 0xF0: op_debug_line(code, pc); break;
            case 0xF1: op_debug_file(code, pc); break;
            case 0xF2: op_bkpt_line(); break;
            case 0xF3: op_timestamp(); break;
            
            // Function/Method calls
            case 0x41: op_call(code, pc); break;
            case 0x42: op_construct(code, pc); break;
            case 0x43: op_call_method(code, pc); break;
            case 0x44: op_call_static(code, pc); break;
            case 0x45: op_call_super(code, pc); break;
            case 0x46: op_call_property(code, pc); break;
            case 0x47: op_return_void(); break;
            case 0x48: op_return_value(); break;
            case 0x49: op_construct_super(code, pc); break;
            case 0x4A: op_construct_prop(code, pc); break;
            case 0x4B: op_call_super_id(code, pc); break;
            case 0x4C: op_call_prop_void(code, pc); break;
            case 0x4D: op_call_super_void(code, pc); break;
            case 0x53: op_apply_type(code, pc); break;
            case 0x40: op_new_function(code, pc); break;
            case 0x52: op_new_class_alias(code, pc); break;
            
            default:
                // Unknown opcode - skip
                break;
        }
    } catch (const std::exception& e) {
        error_ = std::string("Execution error: ") + e.what();
        return false;
    }
    
    return true;
}

// ============================================================================
// Opcode Implementations
// ============================================================================

void AVM2::op_nop() {}
void AVM2::op_throw() {
    auto val = context_.pop();
    context_.throwException(val);
}

void AVM2::op_getsuper(const uint8* code, size_t& pc) {
    uint32_t multinameIndex = readU30(code, pc);
    auto multiname = abcFile_->getMultiname(multinameIndex);
    
    auto obj = context_.pop();
    // Get property from superclass
    context_.push(std::monostate());
}

void AVM2::op_setsuper(const uint8* code, size_t& pc) {
    uint32_t multinameIndex = readU30(code, pc);
    auto value = context_.pop();
    auto obj = context_.pop();
    // Set property on superclass
}

void AVM2::op_dup() {
    auto val = context_.top();
    context_.push(val);
}

void AVM2::op_pop() {
    context_.pop();
}

void AVM2::op_dup_scope() {
    auto val = context_.top();
    if (auto* obj = std::get_if<std::shared_ptr<AVM2Object>>(&val)) {
        context_.push(*obj);
    }
}

void AVM2::op_push_scope() {
    auto val = context_.pop();
    if (auto* obj = std::get_if<std::shared_ptr<AVM2Object>>(&val)) {
        context_.pushScope(*obj);
    }
}

void AVM2::op_pop_scope() {
    context_.popScope();
}

void AVM2::op_label() {}

void AVM2::op_jump(const uint8* code, size_t& pc) {
    int32_t offset = readS24(code, pc);
    pc += offset;
}

void AVM2::op_iftrue(const uint8* code, size_t& pc) {
    int32_t offset = readS24(code, pc);
    if (toBoolean(context_.pop())) {
        pc += offset;
    }
}

void AVM2::op_iffalse(const uint8* code, size_t& pc) {
    int32_t offset = readS24(code, pc);
    if (!toBoolean(context_.pop())) {
        pc += offset;
    }
}

void AVM2::op_ifeq(const uint8* code, size_t& pc) {
    int32_t offset = readS24(code, pc);
    auto b = context_.pop();
    auto a = context_.pop();
    
    bool result = false;
    if (auto* ad = std::get_if<double>(&a)) {
        if (auto* bd = std::get_if<double>(&b)) result = *ad == *bd;
        else if (auto* bi = std::get_if<int32>(&b)) result = *ad == *bi;
    } else if (auto* ai = std::get_if<int32>(&a)) {
        if (auto* bd = std::get_if<double>(&b)) result = *ai == *bd;
        else if (auto* bi = std::get_if<int32>(&b)) result = *ai == *bi;
    } else if (auto* as = std::get_if<std::string>(&a)) {
        if (auto* bs = std::get_if<std::string>(&b)) result = *as == *bs;
    }
    
    if (result) pc += offset;
}

void AVM2::op_ifne(const uint8* code, size_t& pc) {
    int32_t offset = readS24(code, pc);
    auto b = context_.pop();
    auto a = context_.pop();
    
    bool result = false;
    if (auto* ad = std::get_if<double>(&a)) {
        if (auto* bd = std::get_if<double>(&b)) result = *ad != *bd;
        else if (auto* bi = std::get_if<int32>(&b)) result = *ad != *bi;
    } else if (auto* ai = std::get_if<int32>(&a)) {
        if (auto* bd = std::get_if<double>(&b)) result = *ai != *bd;
        else if (auto* bi = std::get_if<int32>(&b)) result = *ai != *bi;
    } else if (auto* as = std::get_if<std::string>(&a)) {
        if (auto* bs = std::get_if<std::string>(&b)) result = *as != *bs;
    }
    
    if (result) pc += offset;
}

void AVM2::op_iflt(const uint8* code, size_t& pc) {
    int32_t offset = readS24(code, pc);
    auto b = context_.pop();
    auto a = context_.pop();
    
    if (toNumber(a) < toNumber(b)) {
        pc += offset;
    }
}

void AVM2::op_ifle(const uint8* code, size_t& pc) {
    int32_t offset = readS24(code, pc);
    auto b = context_.pop();
    auto a = context_.pop();
    
    if (toNumber(a) <= toNumber(b)) {
        pc += offset;
    }
}

void AVM2::op_ifgt(const uint8* code, size_t& pc) {
    int32_t offset = readS24(code, pc);
    auto b = context_.pop();
    auto a = context_.pop();
    
    if (toNumber(a) > toNumber(b)) {
        pc += offset;
    }
}

void AVM2::op_ifge(const uint8* code, size_t& pc) {
    int32_t offset = readS24(code, pc);
    auto b = context_.pop();
    auto a = context_.pop();
    
    if (toNumber(a) >= toNumber(b)) {
        pc += offset;
    }
}

void AVM2::op_ifstricteq(const uint8* code, size_t& pc) {
    int32_t offset = readS24(code, pc);
    auto b = context_.pop();
    auto a = context_.pop();
    
    // Strict equality - same type and value
    bool result = false;
    if (a.index() == b.index()) {
        result = (a == b);
    }
    
    if (result) pc += offset;
}

void AVM2::op_ifstrictne(const uint8* code, size_t& pc) {
    int32_t offset = readS24(code, pc);
    auto b = context_.pop();
    auto a = context_.pop();
    
    bool result = false;
    if (a.index() != b.index()) {
        result = true;
    } else {
        result = (a != b);
    }
    
    if (result) pc += offset;
}

void AVM2::op_lookupswitch(const uint8* code, size_t& pc) {
    int32_t defaultOffset = readS24(code, pc);
    uint32_t caseCount = readU30(code, pc);
    
    auto index = context_.pop();
    int32_t idx = 0;
    if (auto* i = std::get_if<int32>(&index)) idx = *i;
    else if (auto* d = std::get_if<double>(&index)) idx = static_cast<int32>(*d);
    
    if (idx >= 0 && idx <= static_cast<int32>(caseCount)) {
        // Calculate case offset position
        size_t casePos = pc + 3 * idx;
        int32_t caseOffset = readS24(code, casePos);
        pc = casePos + caseOffset;
    } else {
        pc += defaultOffset;
    }
}

void AVM2::op_push_with() {
    // Push scope with "with" semantics
    auto val = context_.pop();
    if (auto* obj = std::get_if<std::shared_ptr<AVM2Object>>(&val)) {
        context_.pushScope(*obj);
    }
}

void AVM2::op_pop_scope_alias() {
    context_.popScope();
}

void AVM2::op_next_name() {
    auto index = context_.pop();
    auto obj = context_.pop();
    // Return next property name
    context_.push(std::string(""));
}

void AVM2::op_next_value() {
    auto index = context_.pop();
    auto obj = context_.pop();
    // Return next property value
    context_.push(std::monostate());
}

void AVM2::op_push_null() {
    context_.push(std::monostate());
}

void AVM2::op_push_undefined() {
    context_.push(std::monostate());
}

void AVM2::op_push_true() {
    context_.push(true);
}

void AVM2::op_push_false() {
    context_.push(false);
}

void AVM2::op_push_nan() {
    context_.push(std::nan(""));
}

void AVM2::op_push_int(const uint8* code, size_t& pc) {
    uint32_t index = readU30(code, pc);
    context_.push(abcFile_->getInt(index));
}

void AVM2::op_push_uint(const uint8* code, size_t& pc) {
    uint32_t index = readU30(code, pc);
    context_.push(static_cast<int32>(abcFile_->getUInt(index)));
}

void AVM2::op_push_double(const uint8* code, size_t& pc) {
    uint32_t index = readU30(code, pc);
    context_.push(abcFile_->getDouble(index));
}

void AVM2::op_push_string(const uint8* code, size_t& pc) {
    uint32_t index = readU30(code, pc);
    context_.push(abcFile_->getString(index));
}

void AVM2::op_push_namespace(const uint8* code, size_t& pc) {
    uint32_t index = readU30(code, pc);
    // Push namespace object
    context_.push(std::monostate());
}

void AVM2::op_jump_alias(const uint8* code, size_t& pc) {
    op_jump(code, pc);
}

void AVM2::op_iftrue_alias(const uint8* code, size_t& pc) {
    op_iftrue(code, pc);
}

void AVM2::op_iffalse_alias(const uint8* code, size_t& pc) {
    op_iffalse(code, pc);
}

void AVM2::op_new_object(const uint8* code, size_t& pc) {
    uint32_t argCount = readU30(code, pc);
    auto obj = std::make_shared<AVM2Object>();
    
    for (uint32_t i = 0; i < argCount; i++) {
        auto value = context_.pop();
        auto name = context_.pop();
        if (auto* s = std::get_if<std::string>(&name)) {
            obj->setProperty(*s, value);
        }
    }
    
    context_.push(obj);
}

void AVM2::op_new_array(const uint8* code, size_t& pc) {
    uint32_t argCount = readU30(code, pc);
    auto arr = std::make_shared<AVM2Array>();
    
    for (uint32_t i = 0; i < argCount; i++) {
        arr->elements.insert(arr->elements.begin(), context_.pop());
    }
    
    context_.push(arr);
}

void AVM2::op_new_activation() {
    auto activation = std::make_shared<AVM2Object>();
    context_.push(activation);
}

void AVM2::op_new_class(const uint8* code, size_t& pc) {
    uint32_t classIndex = readU30(code, pc);
    auto baseClass = context_.pop();
    
    auto cls = getClass(classIndex);
    if (cls) {
        context_.push(cls);
    } else {
        context_.push(std::monostate());
    }
}

void AVM2::op_new_catch(const uint8* code, size_t& pc) {
    uint32_t excIndex = readU30(code, pc);
    auto catchObj = std::make_shared<AVM2Object>();
    context_.push(catchObj);
}

void AVM2::op_find_prop_strict(const uint8* code, size_t& pc) {
    uint32_t multinameIndex = readU30(code, pc);
    auto multiname = abcFile_->getMultiname(multinameIndex);
    
    if (multiname) {
        // Search scope chain
        for (auto it = context_.scopeStack.rbegin(); it != context_.scopeStack.rend(); ++it) {
            if ((*it)->hasProperty(multiname->name)) {
                context_.push(*it);
                return;
            }
        }
        
        // Check global
        if (context_.globalObject->hasProperty(multiname->name)) {
            context_.push(context_.globalObject);
            return;
        }
    }
    
    context_.push(std::monostate());
}

void AVM2::op_find_property(const uint8* code, size_t& pc) {
    op_find_prop_strict(code, pc);
}

void AVM2::op_find_def(const uint8* code, size_t& pc) {
    uint32_t multinameIndex = readU30(code, pc);
    // Find definition
    context_.push(context_.globalObject);
}

void AVM2::op_get_lex(const uint8* code, size_t& pc) {
    uint32_t multinameIndex = readU30(code, pc);
    auto multiname = abcFile_->getMultiname(multinameIndex);
    
    if (multiname) {
        // Search scope chain
        for (auto it = context_.scopeStack.rbegin(); it != context_.scopeStack.rend(); ++it) {
            auto val = (*it)->getProperty(multiname->name);
            if (!std::get_if<std::monostate>(&val)) {
                context_.push(val);
                return;
            }
        }
        
        // Check global
        auto val = context_.globalObject->getProperty(multiname->name);
        if (!std::get_if<std::monostate>(&val)) {
            context_.push(val);
            return;
        }
    }
    
    context_.push(std::monostate());
}

void AVM2::op_set_property(const uint8* code, size_t& pc) {
    uint32_t multinameIndex = readU30(code, pc);
    auto multiname = abcFile_->getMultiname(multinameIndex);
    
    auto value = context_.pop();
    auto obj = context_.pop();
    
    if (auto* o = std::get_if<std::shared_ptr<AVM2Object>>(&obj)) {
        if (multiname) {
            // Use inline cache for repeated property writes
            static thread_local PropertyCacheEntry cache;
            (*o)->setPropertyFast(multiname->name, value, cache);
        }
    }
}

void AVM2::op_get_property(const uint8* code, size_t& pc) {
    uint32_t multinameIndex = readU30(code, pc);
    auto multiname = abcFile_->getMultiname(multinameIndex);
    
    auto obj = context_.pop();
    
    if (auto* o = std::get_if<std::shared_ptr<AVM2Object>>(&obj)) {
        if (multiname) {
            // Use inline cache for repeated property access
            static thread_local PropertyCacheEntry cache;
            context_.push((*o)->getPropertyFast(multiname->name, cache));
        } else {
            context_.push(std::monostate());
        }
    } else {
        context_.push(std::monostate());
    }
}

void AVM2::op_init_property(const uint8* code, size_t& pc) {
    uint32_t multinameIndex = readU30(code, pc);
    auto multiname = abcFile_->getMultiname(multinameIndex);
    
    auto value = context_.pop();
    auto obj = context_.pop();
    
    if (auto* o = std::get_if<std::shared_ptr<AVM2Object>>(&obj)) {
        if (multiname) {
            (*o)->setProperty(multiname->name, value);
        }
    }
}

void AVM2::op_delete_property(const uint8* code, size_t& pc) {
    uint32_t multinameIndex = readU30(code, pc);
    auto multiname = abcFile_->getMultiname(multinameIndex);
    
    auto obj = context_.pop();
    
    bool result = false;
    if (auto* o = std::get_if<std::shared_ptr<AVM2Object>>(&obj)) {
        if (multiname) {
            result = (*o)->deleteProperty(multiname->name);
        }
    }
    
    context_.push(result);
}

void AVM2::op_get_local(uint32 index) {
    if (index < context_.localRegisters.size()) {
        context_.push(context_.localRegisters[index]);
    } else {
        context_.push(std::monostate());
    }
}

void AVM2::op_set_local(uint32 index) {
    if (index < context_.localRegisters.size()) {
        context_.localRegisters[index] = context_.pop();
    }
}

void AVM2::op_get_global_scope() {
    context_.push(context_.globalObject);
}

void AVM2::op_get_scope_object(const uint8* code, size_t& pc) {
    uint32_t index = readU8(code, pc);
    context_.push(context_.getScope(index));
}

void AVM2::op_get_slot(const uint8* code, size_t& pc) {
    uint32_t slotIndex = readU30(code, pc);
    auto obj = context_.pop();
    
    if (auto* o = std::get_if<std::shared_ptr<AVM2Object>>(&obj)) {
        // Get slot value
        context_.push((*o)->getProperty("slot" + std::to_string(slotIndex)));
    } else {
        context_.push(std::monostate());
    }
}

void AVM2::op_set_slot(const uint8* code, size_t& pc) {
    uint32_t slotIndex = readU30(code, pc);
    auto value = context_.pop();
    auto obj = context_.pop();
    
    if (auto* o = std::get_if<std::shared_ptr<AVM2Object>>(&obj)) {
        (*o)->setProperty("slot" + std::to_string(slotIndex), value);
    }
}

void AVM2::op_get_global_slot(const uint8* code, size_t& pc) {
    uint32_t slotIndex = readU30(code, pc);
    context_.push(context_.globalObject->getProperty("slot" + std::to_string(slotIndex)));
}

void AVM2::op_set_global_slot(const uint8* code, size_t& pc) {
    uint32_t slotIndex = readU30(code, pc);
    auto value = context_.pop();
    context_.globalObject->setProperty("slot" + std::to_string(slotIndex), value);
}

void AVM2::op_convert_s() {
    context_.push(toString(context_.pop()));
}

void AVM2::op_esc_xelem() {
    // XML escape
    auto val = context_.pop();
    context_.push(toString(val));
}

void AVM2::op_esc_xattr() {
    // XML attribute escape
    auto val = context_.pop();
    context_.push(toString(val));
}

void AVM2::op_convert_i() {
    auto& val = context_.top();
    val = toInt32(val);
}

void AVM2::op_convert_u() {
    auto& val = context_.top();
    val = static_cast<int32>(toUInt32(val));
}

void AVM2::op_convert_d() {
    auto& val = context_.top();
    val = toNumber(val);
}

void AVM2::op_convert_b() {
    auto& val = context_.top();
    val = toBoolean(val);
}

void AVM2::op_convert_o() {
    auto val = context_.pop();
    if (std::get_if<std::monostate>(&val)) {
        context_.throwException(std::string("TypeError: Cannot convert undefined to object"));
    } else {
        context_.push(val);
    }
}

void AVM2::op_check_filter() {
    // Check if value is valid for XML filtering
    auto val = context_.pop();
    context_.push(val);
}

void AVM2::op_coerce(const uint8* code, size_t& pc) {
    uint32_t typeIndex = readU30(code, pc);
    // Coerce value to type
    auto val = context_.pop();
    context_.push(val); // Simplified
}

void AVM2::op_coerce_a() {
    // Coerce to any - no-op
}

void AVM2::op_coerce_s() {
    auto val = context_.pop();
    if (std::get_if<std::monostate>(&val)) {
        context_.push(std::monostate());
    } else {
        context_.push(toString(val));
    }
}

void AVM2::op_as_type(const uint8* code, size_t& pc) {
    uint32_t typeIndex = readU30(code, pc);
    auto val = context_.pop();
    
    // Check if value is of type
    // Simplified - just push value
    context_.push(val);
}

void AVM2::op_as_type_late() {
    auto type = context_.pop();
    auto val = context_.pop();
    // Check type at runtime
    context_.push(val);
}

void AVM2::op_coerce_o() {
    auto val = context_.pop();
    if (std::get_if<std::monostate>(&val) || std::get_if<bool>(&val)) {
        context_.push(std::monostate());
    } else {
        context_.push(val);
    }
}

void AVM2::op_negate() {
    auto& val = context_.top();
    val = -toNumber(val);
}

void AVM2::op_increment() {
    auto& val = context_.top();
    val = toNumber(val) + 1.0;
}

void AVM2::op_inclocal(const uint8* code, size_t& pc) {
    uint32_t index = readU30(code, pc);
    if (index < context_.localRegisters.size()) {
        context_.localRegisters[index] = toNumber(context_.localRegisters[index]) + 1.0;
    }
}

void AVM2::op_decrement() {
    auto& val = context_.top();
    val = toNumber(val) - 1.0;
}

void AVM2::op_declocal(const uint8* code, size_t& pc) {
    uint32_t index = readU30(code, pc);
    if (index < context_.localRegisters.size()) {
        context_.localRegisters[index] = toNumber(context_.localRegisters[index]) - 1.0;
    }
}

void AVM2::op_type_of() {
    auto val = context_.pop();
    std::string type = "undefined";
    
    if (std::get_if<std::monostate>(&val)) {
        type = "undefined";
    } else if (std::get_if<bool>(&val)) {
        type = "boolean";
    } else if (std::get_if<int32>(&val) || std::get_if<double>(&val)) {
        type = "number";
    } else if (std::get_if<std::string>(&val)) {
        type = "string";
    } else if (auto* obj = std::get_if<std::shared_ptr<AVM2Object>>(&val)) {
        if (auto* fn = dynamic_cast<AVM2Function*>(obj->get())) {
            type = "function";
        } else if (auto* arr = dynamic_cast<AVM2Array*>(obj->get())) {
            type = "object"; // Array is object in typeof
        } else {
            type = "object";
        }
    }
    
    context_.push(type);
}

void AVM2::op_not() {
    auto& val = context_.top();
    val = !toBoolean(val);
}

void AVM2::op_bit_not() {
    auto& val = context_.top();
    val = ~toInt32(val);
}

void AVM2::op_add() {
    auto b = context_.pop();
    auto a = context_.pop();
    
    // Fast path: int + int
    if (auto* ai = std::get_if<int32>(&a)) {
        if (auto* bi = std::get_if<int32>(&b)) {
            context_.push(*ai + *bi);
            return;
        }
        if (auto* bd = std::get_if<double>(&b)) {
            context_.push(static_cast<double>(*ai) + *bd);
            return;
        }
    }
    // Fast path: double + double/int
    if (auto* ad = std::get_if<double>(&a)) {
        if (auto* bd = std::get_if<double>(&b)) {
            context_.push(*ad + *bd);
            return;
        }
        if (auto* bi = std::get_if<int32>(&b)) {
            context_.push(*ad + static_cast<double>(*bi));
            return;
        }
    }
    
    // String concatenation if either is string
    if (std::get_if<std::string>(&a) || std::get_if<std::string>(&b)) {
        context_.push(toString(a) + toString(b));
        return;
    }
    
    context_.push(toNumber(a) + toNumber(b));
}

void AVM2::op_subtract() {
    auto b = context_.pop();
    auto a = context_.pop();
    
    // Fast path: int - int
    if (auto* ai = std::get_if<int32>(&a)) {
        if (auto* bi = std::get_if<int32>(&b)) {
            context_.push(*ai - *bi);
            return;
        }
    }
    
    context_.push(toNumber(a) - toNumber(b));
}

void AVM2::op_multiply() {
    auto b = context_.pop();
    auto a = context_.pop();
    
    // Fast path: int * int
    if (auto* ai = std::get_if<int32>(&a)) {
        if (auto* bi = std::get_if<int32>(&b)) {
            context_.push(*ai * *bi);
            return;
        }
    }
    
    context_.push(toNumber(a) * toNumber(b));
}

void AVM2::op_divide() {
    auto b = context_.pop();
    auto a = context_.pop();
    double divisor = toNumber(b);
    if (divisor == 0.0) {
        double dividend = toNumber(a);
        if (dividend == 0.0) {
            context_.push(std::nan(""));
        } else if ((dividend > 0) == (divisor > 0)) {
            context_.push(std::numeric_limits<double>::infinity());
        } else {
            context_.push(-std::numeric_limits<double>::infinity());
        }
    } else {
        context_.push(toNumber(a) / divisor);
    }
}

void AVM2::op_modulo() {
    auto b = context_.pop();
    auto a = context_.pop();
    context_.push(fmod(toNumber(a), toNumber(b)));
}

void AVM2::op_lshift() {
    auto b = context_.pop();
    auto a = context_.pop();
    context_.push(toInt32(a) << (toUInt32(b) & 0x1F));
}

void AVM2::op_rshift() {
    auto b = context_.pop();
    auto a = context_.pop();
    context_.push(toInt32(a) >> (toUInt32(b) & 0x1F));
}

void AVM2::op_urshift() {
    auto b = context_.pop();
    auto a = context_.pop();
    context_.push(static_cast<int32>(toUInt32(a) >> (toUInt32(b) & 0x1F)));
}

void AVM2::op_bit_and() {
    auto b = context_.pop();
    auto a = context_.pop();
    context_.push(toInt32(a) & toInt32(b));
}

void AVM2::op_bit_or() {
    auto b = context_.pop();
    auto a = context_.pop();
    context_.push(toInt32(a) | toInt32(b));
}

void AVM2::op_bit_xor() {
    auto b = context_.pop();
    auto a = context_.pop();
    context_.push(toInt32(a) ^ toInt32(b));
}

void AVM2::op_equals() {
    auto b = context_.pop();
    auto a = context_.pop();
    
    // Coerce and compare
    if (auto* as = std::get_if<std::string>(&a)) {
        if (auto* bs = std::get_if<std::string>(&b)) {
            context_.push(*as == *bs);
            return;
        }
    }
    
    context_.push(toNumber(a) == toNumber(b));
}

void AVM2::op_strictequals() {
    auto b = context_.pop();
    auto a = context_.pop();
    
    // Same type and value
    if (a.index() != b.index()) {
        context_.push(false);
        return;
    }
    
    context_.push(a == b);
}

void AVM2::op_lessthan() {
    auto b = context_.pop();
    auto a = context_.pop();
    context_.push(toNumber(a) < toNumber(b));
}

void AVM2::op_lessequals() {
    auto b = context_.pop();
    auto a = context_.pop();
    context_.push(toNumber(a) <= toNumber(b));
}

void AVM2::op_greaterthan() {
    auto b = context_.pop();
    auto a = context_.pop();
    context_.push(toNumber(a) > toNumber(b));
}

void AVM2::op_greaterequals() {
    auto b = context_.pop();
    auto a = context_.pop();
    context_.push(toNumber(a) >= toNumber(b));
}

void AVM2::op_instance_of() {
    auto type = context_.pop();
    auto val = context_.pop();
    
    // Check if val is instance of type
    bool result = false;
    if (auto* typeClass = std::get_if<std::shared_ptr<AVM2Class>>(&type)) {
        if (auto* obj = std::get_if<std::shared_ptr<AVM2Object>>(&val)) {
            if ((*obj)->class_) {
                result = (*obj)->class_->isSubclassOf(*typeClass);
            }
        }
    }
    
    context_.push(result);
}

void AVM2::op_is_type(const uint8* code, size_t& pc) {
    uint32_t typeIndex = readU30(code, pc);
    auto val = context_.pop();
    
    auto type = abcFile_->getMultiname(typeIndex);
    // Check is type
    context_.push(true); // Simplified
}

void AVM2::op_is_type_late() {
    auto type = context_.pop();
    auto val = context_.pop();
    // Check is type at runtime
    context_.push(true); // Simplified
}

void AVM2::op_in() {
    auto name = context_.pop();
    auto obj = context_.pop();
    
    bool result = false;
    if (auto* o = std::get_if<std::shared_ptr<AVM2Object>>(&obj)) {
        if (auto* s = std::get_if<std::string>(&name)) {
            result = (*o)->hasProperty(*s);
        }
    }
    
    context_.push(result);
}

void AVM2::op_increment_i() {
    auto& val = context_.top();
    val = toInt32(val) + 1;
}

void AVM2::op_inclocal_i(const uint8* code, size_t& pc) {
    uint32_t index = readU30(code, pc);
    if (index < context_.localRegisters.size()) {
        context_.localRegisters[index] = toInt32(context_.localRegisters[index]) + 1;
    }
}

void AVM2::op_decrement_i() {
    auto& val = context_.top();
    val = toInt32(val) - 1;
}

void AVM2::op_declocal_i(const uint8* code, size_t& pc) {
    uint32_t index = readU30(code, pc);
    if (index < context_.localRegisters.size()) {
        context_.localRegisters[index] = toInt32(context_.localRegisters[index]) - 1;
    }
}

void AVM2::op_negate_i() {
    auto& val = context_.top();
    val = -toInt32(val);
}

void AVM2::op_add_i() {
    auto b = context_.pop();
    auto a = context_.pop();
    context_.push(toInt32(a) + toInt32(b));
}

void AVM2::op_subtract_i() {
    auto b = context_.pop();
    auto a = context_.pop();
    context_.push(toInt32(a) - toInt32(b));
}

void AVM2::op_multiply_i() {
    auto b = context_.pop();
    auto a = context_.pop();
    context_.push(toInt32(a) * toInt32(b));
}

void AVM2::op_get_local0() { op_get_local(0); }
void AVM2::op_get_local1() { op_get_local(1); }
void AVM2::op_get_local2() { op_get_local(2); }
void AVM2::op_get_local3() { op_get_local(3); }
void AVM2::op_set_local0() { op_set_local(0); }
void AVM2::op_set_local1() { op_set_local(1); }
void AVM2::op_set_local2() { op_set_local(2); }
void AVM2::op_set_local3() { op_set_local(3); }

void AVM2::op_debug() {
    // Skip debug info (7 bytes)
}

void AVM2::op_debug_line(const uint8* code, size_t& pc) {
    readU30(code, pc);
}

void AVM2::op_debug_file(const uint8* code, size_t& pc) {
    readU30(code, pc);
}

void AVM2::op_bkpt_line() {}
void AVM2::op_timestamp() {}

// Function/Method calls
void AVM2::op_call(const uint8* code, size_t& pc) {
    uint32_t argCount = readU30(code, pc);
    // Optimized: use stack-based array for small arg counts, avoid O(n²) insert
    std::vector<AVM2Value> args;
    if (argCount > 0) {
        args.resize(argCount);
        for (int32_t i = static_cast<int32_t>(argCount) - 1; i >= 0; --i) {
            args[i] = context_.pop();
        }
    }
    
    auto func = context_.pop();
    auto thisObj = context_.pop();
    
    if (auto* fn = std::get_if<std::shared_ptr<AVM2Function>>(&func)) {
        if ((*fn)->isNative && (*fn)->nativeImpl) {
            context_.push((*fn)->nativeImpl(context_, args));
        } else if ((*fn)->method) {
            auto obj = std::get_if<std::shared_ptr<AVM2Object>>(&thisObj);
            executeMethod((*fn)->method, obj ? *obj : nullptr, context_.scopeObject, args);
        }
    } else {
        auto funcName = toString(func);
        auto native = context_.nativeFunctions.find(funcName);
        if (native != context_.nativeFunctions.end()) {
            context_.push(native->second(context_, args));
        } else {
            context_.push(std::monostate());
        }
    }
}

void AVM2::op_construct(const uint8* code, size_t& pc) {
    uint32_t argCount = readU30(code, pc);
    std::vector<AVM2Value> args;
    if (argCount > 0) {
        args.resize(argCount);
        for (int32_t i = static_cast<int32_t>(argCount) - 1; i >= 0; --i) {
            args[i] = context_.pop();
        }
    }
    
    auto ctor = context_.pop();
    
    if (auto* cls = std::get_if<std::shared_ptr<AVM2Class>>(&ctor)) {
        context_.push((*cls)->construct(args));
    } else if (auto* fn = std::get_if<std::shared_ptr<AVM2Function>>(&ctor)) {
        context_.push((*fn)->construct(args));
    } else {
        context_.push(std::monostate());
    }
}

void AVM2::op_call_method(const uint8* code, size_t& pc) {
    uint32_t dispId = readU30(code, pc);
    uint32_t argCount = readU30(code, pc);
    
    std::vector<AVM2Value> args;
    if (argCount > 0) {
        args.resize(argCount);
        for (int32_t i = static_cast<int32_t>(argCount) - 1; i >= 0; --i) {
            args[i] = context_.pop();
        }
    }
    
    auto obj = context_.pop();
    // Call method by dispatch id
    context_.push(std::monostate());
}

void AVM2::op_call_static(const uint8* code, size_t& pc) {
    uint32_t methodIndex = readU30(code, pc);
    uint32_t argCount = readU30(code, pc);
    
    std::vector<AVM2Value> args;
    if (argCount > 0) {
        args.resize(argCount);
        for (int32_t i = static_cast<int32_t>(argCount) - 1; i >= 0; --i) {
            args[i] = context_.pop();
        }
    }
    
    if (methodIndex < abcFile_->methods.size()) {
        executeMethod(abcFile_->methods[methodIndex], context_.scopeObject, context_.scopeObject, args);
        if (context_.stackSize() > 0) {
            // Result already on stack
        }
    }
}

void AVM2::op_call_super(const uint8* code, size_t& pc) {
    uint32_t multinameIndex = readU30(code, pc);
    uint32_t argCount = readU30(code, pc);
    
    std::vector<AVM2Value> args;
    if (argCount > 0) {
        args.resize(argCount);
        for (int32_t i = static_cast<int32_t>(argCount) - 1; i >= 0; --i) {
            args[i] = context_.pop();
        }
    }
    
    auto base = context_.pop();
    // Call super method
    context_.push(std::monostate());
}

void AVM2::op_call_property(const uint8* code, size_t& pc) {
    uint32_t multinameIndex = readU30(code, pc);
    uint32_t argCount = readU30(code, pc);
    
    std::vector<AVM2Value> args;
    if (argCount > 0) {
        args.resize(argCount);
        for (int32_t i = static_cast<int32_t>(argCount) - 1; i >= 0; --i) {
            args[i] = context_.pop();
        }
    }
    
    auto obj = context_.pop();
    auto multiname = abcFile_->getMultiname(multinameIndex);
    
    if (auto* o = std::get_if<std::shared_ptr<AVM2Object>>(&obj)) {
        if (multiname) {
            // Use method cache for faster repeated calls
            auto fn = lookupMethodCached(*o, multiname->name);
            if (fn) {
                if (fn->isNative && fn->nativeImpl) {
                    context_.push(fn->nativeImpl(context_, args));
                } else {
                    context_.push(fn->call(args));
                }
            } else {
                // Fallback to normal property lookup
                static thread_local PropertyCacheEntry cache;
                auto prop = (*o)->getPropertyFast(multiname->name, cache);
                if (auto* fnPtr = std::get_if<std::shared_ptr<AVM2Function>>(&prop)) {
                    if ((*fnPtr)->isNative && (*fnPtr)->nativeImpl) {
                        context_.push((*fnPtr)->nativeImpl(context_, args));
                    } else {
                        context_.push((*fnPtr)->call(args));
                    }
                } else {
                    context_.push(prop);
                }
            }
        }
    } else {
        context_.push(std::monostate());
    }
}

void AVM2::op_return_void() {
    // Return undefined (already on stack or nothing)
}

void AVM2::op_return_value() {
    // Value already on stack
}

void AVM2::op_construct_super(const uint8* code, size_t& pc) {
    uint32_t argCount = readU30(code, pc);
    std::vector<AVM2Value> args;
    if (argCount > 0) {
        args.resize(argCount);
        for (int32_t i = static_cast<int32_t>(argCount) - 1; i >= 0; --i) {
            args[i] = context_.pop();
        }
    }
    
    auto obj = context_.pop();
    // Call super constructor
    context_.push(obj);
}

void AVM2::op_construct_prop(const uint8* code, size_t& pc) {
    uint32_t multinameIndex = readU30(code, pc);
    uint32_t argCount = readU30(code, pc);
    
    std::vector<AVM2Value> args;
    if (argCount > 0) {
        args.resize(argCount);
        for (int32_t i = static_cast<int32_t>(argCount) - 1; i >= 0; --i) {
            args[i] = context_.pop();
        }
    }
    
    auto obj = context_.pop();
    // Construct property
    context_.push(std::monostate());
}

void AVM2::op_call_super_id(const uint8* code, size_t& pc) {
    readU30(code, pc); // index
    readU30(code, pc); // arg count
}

void AVM2::op_call_prop_void(const uint8* code, size_t& pc) {
    uint32_t multinameIndex = readU30(code, pc);
    uint32_t argCount = readU30(code, pc);
    
    std::vector<AVM2Value> args;
    if (argCount > 0) {
        args.resize(argCount);
        for (int32_t i = static_cast<int32_t>(argCount) - 1; i >= 0; --i) {
            args[i] = context_.pop();
        }
    }
    
    auto obj = context_.pop();
    // Call property, discard result
}

void AVM2::op_call_super_void(const uint8* code, size_t& pc) {
    uint32_t multinameIndex = readU30(code, pc);
    uint32_t argCount = readU30(code, pc);
    
    std::vector<AVM2Value> args;
    if (argCount > 0) {
        args.resize(argCount);
        for (int32_t i = static_cast<int32_t>(argCount) - 1; i >= 0; --i) {
            args[i] = context_.pop();
        }
    }
    
    auto base = context_.pop();
    // Call super method, discard result
}

void AVM2::op_apply_type(const uint8* code, size_t& pc) {
    uint32_t typeCount = readU30(code, pc);
    for (uint32_t i = 0; i < typeCount; i++) {
        context_.pop();
    }
    auto factory = context_.pop();
    // Apply types
    context_.push(factory);
}

void AVM2::op_new_function(const uint8* code, size_t& pc) {
    uint32_t methodIndex = readU30(code, pc);
    
    if (methodIndex < abcFile_->methods.size()) {
        auto fn = std::make_shared<AVM2Function>();
        fn->method = abcFile_->methods[methodIndex];
        fn->scope = context_.scopeObject;
        context_.push(fn);
    } else {
        context_.push(std::monostate());
    }
}

void AVM2::op_new_class_alias(const uint8* code, size_t& pc) {
    op_new_class(code, pc);
}

void AVM2::op_get_descendants(const uint8* code, size_t& pc) {
    uint32_t multinameIndex = readU30(code, pc);
    auto obj = context_.pop();
    // Get descendants
    context_.push(std::make_shared<AVM2Array>());
}

void AVM2::op_get_super_method(const uint8* code, size_t& pc) {
    uint32_t multinameIndex = readU30(code, pc);
    auto obj = context_.pop();
    context_.push(std::monostate());
}

void AVM2::op_get_super_init(const uint8* code, size_t& pc) {
    uint32_t multinameIndex = readU30(code, pc);
    context_.push(std::monostate());
}

void AVM2::op_get_super_accessor(const uint8* code, size_t& pc) {
    uint32_t multinameIndex = readU30(code, pc);
    auto obj = context_.pop();
    context_.push(std::monostate());
}

void AVM2::op_get_slot_by_index(const uint8* code, size_t& pc) {
    readU30(code, pc);
    context_.pop();
    context_.push(std::monostate());
}

void AVM2::op_set_slot_by_index(const uint8* code, size_t& pc) {
    readU30(code, pc);
    context_.pop();
    context_.pop();
}

// Helper methods
AVM2Value AVM2::readConstant(uint32 index) {
    if (index == 0) return std::monostate();
    if (index < abcFile_->stringPool.size()) return abcFile_->stringPool[index];
    index -= abcFile_->stringPool.size();
    if (index < abcFile_->intPool.size()) return abcFile_->intPool[index];
    index -= abcFile_->intPool.size();
    if (index < abcFile_->doublePool.size()) return abcFile_->doublePool[index];
    return std::monostate();
}

std::shared_ptr<ABCMultiname> AVM2::readMultiname(const uint8* code, size_t& pc) {
    uint8 kind = code[pc++];
    auto multiname = std::make_shared<ABCMultiname>();
    multiname->kind = kind;
    
    if (kind == 0x07 || kind == 0x0D) {
        uint32_t ns = readU30(code, pc);
        uint32_t name = readU30(code, pc);
        multiname->ns = abcFile_->getNamespace(ns);
        multiname->name = abcFile_->getString(name);
    }
    
    return multiname;
}

AVM2Value AVM2::resolveProperty(const std::string& name) {
    auto it = context_.nativeFunctions.find(name);
    if (it != context_.nativeFunctions.end()) {
        auto fn = std::make_shared<AVM2Function>();
        fn->isNative = true;
        fn->nativeImpl = it->second;
        return fn;
    }
    return context_.globalObject->getProperty(name);
}

AVM2Value AVM2::resolveMultiname(std::shared_ptr<ABCMultiname> multiname) {
    if (!multiname) return std::monostate();
    return resolveProperty(multiname->name);
}

// Method cache lookup with polymorphic inline caching
std::shared_ptr<AVM2Function> AVM2::lookupMethodCached(
    std::shared_ptr<AVM2Object> obj, const std::string& name) {
    if (!obj) return nullptr;
    
    // Simple hash for cache index
    size_t hash = std::hash<std::string>{}(name);
    hash ^= std::hash<void*>{}(obj.get());
    size_t index = hash % METHOD_CACHE_SIZE;
    
    auto& entry = methodCache_[index];
    auto lockedObj = entry.object.lock();
    
    // Check cache hit
    if (lockedObj == obj && entry.methodName == &name) {
        entry.hitCount++;
        return entry.method.lock();
    }
    
    // Cache miss - resolve method
    auto prop = obj->getProperty(name);
    auto fn = std::get_if<std::shared_ptr<AVM2Function>>(&prop);
    if (fn) {
        // Update cache
        entry.object = obj;
        entry.methodName = &name;
        entry.method = *fn;
    }
    
    return fn ? *fn : nullptr;
}

AVM2Value AVM2::toPrimitive(const AVM2Value& val) {
    return val; // Simplified
}

double AVM2::toNumber(const AVM2Value& val) {
    if (auto* d = std::get_if<double>(&val)) return *d;
    if (auto* i = std::get_if<int32>(&val)) return static_cast<double>(*i);
    if (auto* b = std::get_if<bool>(&val)) return *b ? 1.0 : 0.0;
    if (auto* s = std::get_if<std::string>(&val)) {
        try {
            return std::stod(*s);
        } catch (...) {
            return std::nan("");
        }
    }
    if (std::get_if<std::monostate>(&val)) return std::nan("");
    return 0.0;
}

int32_t AVM2::toInt32(const AVM2Value& val) {
    return static_cast<int32>(toNumber(val));
}

uint32_t AVM2::toUInt32(const AVM2Value& val) {
    return static_cast<uint32_t>(toNumber(val));
}

std::string AVM2::toString(const AVM2Value& val) {
    if (auto* s = std::get_if<std::string>(&val)) return *s;
    if (auto* d = std::get_if<double>(&val)) {
        if (std::isnan(*d)) return "NaN";
        if (std::isinf(*d)) return *d > 0 ? "Infinity" : "-Infinity";
        std::ostringstream oss;
        oss << *d;
        return oss.str();
    }
    if (auto* i = std::get_if<int32>(&val)) return std::to_string(*i);
    if (auto* b = std::get_if<bool>(&val)) return *b ? "true" : "false";
    if (std::get_if<std::monostate>(&val)) return "undefined";
    if (auto* obj = std::get_if<std::shared_ptr<AVM2Object>>(&val)) {
        return (*obj)->toString();
    }
    return "";
}

bool AVM2::toBoolean(const AVM2Value& val) {
    if (auto* b = std::get_if<bool>(&val)) return *b;
    if (auto* d = std::get_if<double>(&val)) return *d != 0.0 && !std::isnan(*d);
    if (auto* i = std::get_if<int32>(&val)) return *i != 0;
    if (auto* s = std::get_if<std::string>(&val)) return !s->empty();
    if (std::get_if<std::monostate>(&val)) return false;
    if (auto* obj = std::get_if<std::shared_ptr<AVM2Object>>(&val)) return *obj != nullptr;
    return true;
}

} // namespace libswf
