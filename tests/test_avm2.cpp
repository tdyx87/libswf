#include <gtest/gtest.h>
#include "avm2/avm2.h"
#include <cmath>
#include <iostream>

using namespace libswf;

// ============================================================================
// AVM2 Tests
// ============================================================================

class AVM2Test : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// ============================================================================
// Basic VM Creation Tests
// ============================================================================

TEST_F(AVM2Test, AVM2Creation) {
    AVM2 vm;
    SUCCEED();
}

TEST_F(AVM2Test, ContextCreation) {
    AVM2Context ctx;
    EXPECT_NE(ctx.globalObject, nullptr);
    EXPECT_NE(ctx.scopeObject, nullptr);
}

// ============================================================================
// Stack Operations Tests
// ============================================================================

TEST_F(AVM2Test, StackPushPop) {
    AVM2Context ctx;
    
    ctx.push(42);
    ctx.push(3.14);
    ctx.push(std::string("test"));
    
    EXPECT_EQ(ctx.stackSize(), 3);
    
    auto val1 = ctx.pop();
    auto val2 = ctx.pop();
    auto val3 = ctx.pop();
    
    EXPECT_EQ(ctx.stackSize(), 0);
    
    if (auto* s = std::get_if<std::string>(&val1)) {
        EXPECT_EQ(*s, "test");
    }
    if (auto* d = std::get_if<double>(&val2)) {
        EXPECT_FLOAT_EQ(*d, 3.14);
    }
    if (auto* i = std::get_if<int32_t>(&val3)) {
        EXPECT_EQ(*i, 42);
    }
}

TEST_F(AVM2Test, StackTop) {
    AVM2Context ctx;
    
    ctx.push(42);
    EXPECT_EQ(ctx.stackSize(), 1);
    
    auto& top = ctx.top();
    if (auto* i = std::get_if<int32_t>(&top)) {
        EXPECT_EQ(*i, 42);
    }
}

TEST_F(AVM2Test, StackClear) {
    AVM2Context ctx;
    
    ctx.push(1);
    ctx.push(2);
    ctx.push(3);
    EXPECT_EQ(ctx.stackSize(), 3);
    
    ctx.clearStack();
    EXPECT_EQ(ctx.stackSize(), 0);
}

// ============================================================================
// Scope Operations Tests
// ============================================================================

TEST_F(AVM2Test, ScopePushPop) {
    AVM2Context ctx;
    
    auto obj1 = std::make_shared<AVM2Object>();
    auto obj2 = std::make_shared<AVM2Object>();
    
    ctx.pushScope(obj1);
    ctx.pushScope(obj2);
    
    EXPECT_EQ(ctx.scopeStack.size(), 2);
    EXPECT_EQ(ctx.scopeObject, obj2);
    
    auto popped = ctx.popScope();
    EXPECT_EQ(popped, obj2);
    EXPECT_EQ(ctx.scopeObject, obj1);
    
    popped = ctx.popScope();
    EXPECT_EQ(popped, obj1);
}

TEST_F(AVM2Test, ScopeGet) {
    AVM2Context ctx;
    
    auto obj0 = ctx.scopeObject;
    auto obj1 = std::make_shared<AVM2Object>();
    auto obj2 = std::make_shared<AVM2Object>();
    
    ctx.pushScope(obj1);
    ctx.pushScope(obj2);
    
    EXPECT_EQ(ctx.getScope(0), obj0);
    EXPECT_EQ(ctx.getScope(1), obj1);
    EXPECT_EQ(ctx.getScope(2), obj2);
}

// ============================================================================
// Native Functions Tests
// ============================================================================

TEST_F(AVM2Test, NativeFunctionTrace) {
    AVM2Context ctx;
    
    auto it = ctx.nativeFunctions.find("trace");
    EXPECT_NE(it, ctx.nativeFunctions.end());
    
    std::vector<AVM2Value> args;
    args.push_back(std::string("Hello"));
    args.push_back(static_cast<int32>(42));
    
    testing::internal::CaptureStdout();
    AVM2Value result = it->second(ctx, args);
    std::string output = testing::internal::GetCapturedStdout();
    
    EXPECT_TRUE(std::get_if<std::monostate>(&result));
    EXPECT_NE(output.find("Hello"), std::string::npos);
    EXPECT_NE(output.find("42"), std::string::npos);
}

TEST_F(AVM2Test, NativeFunctionIsNaN) {
    AVM2Context ctx;
    
    auto it = ctx.nativeFunctions.find("isNaN");
    EXPECT_NE(it, ctx.nativeFunctions.end());
    
    // Test NaN
    std::vector<AVM2Value> args;
    args.push_back(std::nan(""));
    AVM2Value result = it->second(ctx, args);
    if (auto* b = std::get_if<bool>(&result)) {
        EXPECT_TRUE(*b);
    }
    
    // Test number
    args.clear();
    args.push_back(42.0);
    result = it->second(ctx, args);
    if (auto* b = std::get_if<bool>(&result)) {
        EXPECT_FALSE(*b);
    }
}

TEST_F(AVM2Test, NativeFunctionIsFinite) {
    AVM2Context ctx;
    
    auto it = ctx.nativeFunctions.find("isFinite");
    EXPECT_NE(it, ctx.nativeFunctions.end());
    
    // Test finite number
    std::vector<AVM2Value> args;
    args.push_back(42.0);
    AVM2Value result = it->second(ctx, args);
    if (auto* b = std::get_if<bool>(&result)) {
        EXPECT_TRUE(*b);
    }
    
    // Test infinity
    args.clear();
    args.push_back(std::numeric_limits<double>::infinity());
    result = it->second(ctx, args);
    if (auto* b = std::get_if<bool>(&result)) {
        EXPECT_FALSE(*b);
    }
}

TEST_F(AVM2Test, NativeFunctionParseInt) {
    AVM2Context ctx;
    
    auto it = ctx.nativeFunctions.find("parseInt");
    EXPECT_NE(it, ctx.nativeFunctions.end());
    
    std::vector<AVM2Value> args;
    args.push_back(std::string("42"));
    AVM2Value result = it->second(ctx, args);
    
    if (auto* i = std::get_if<int32_t>(&result)) {
        EXPECT_EQ(*i, 42);
    }
}

TEST_F(AVM2Test, NativeFunctionParseIntWithRadix) {
    AVM2Context ctx;
    
    auto it = ctx.nativeFunctions.find("parseInt");
    
    // Test hex
    std::vector<AVM2Value> args;
    args.push_back(std::string("FF"));
    args.push_back(static_cast<int32>(16));
    AVM2Value result = it->second(ctx, args);
    
    if (auto* i = std::get_if<int32_t>(&result)) {
        EXPECT_EQ(*i, 255);
    }
}

TEST_F(AVM2Test, NativeFunctionParseFloat) {
    AVM2Context ctx;
    
    auto it = ctx.nativeFunctions.find("parseFloat");
    EXPECT_NE(it, ctx.nativeFunctions.end());
    
    std::vector<AVM2Value> args;
    args.push_back(std::string("3.14159"));
    AVM2Value result = it->second(ctx, args);
    
    if (auto* d = std::get_if<double>(&result)) {
        EXPECT_FLOAT_EQ(*d, 3.14159);
    }
}

TEST_F(AVM2Test, NativeFunctionNumberConstants) {
    AVM2Context ctx;
    
    // NaN
    auto it = ctx.nativeFunctions.find("Number.NaN");
    EXPECT_NE(it, ctx.nativeFunctions.end());
    std::vector<AVM2Value> args;
    AVM2Value result = it->second(ctx, args);
    EXPECT_TRUE(std::isnan(std::get<double>(result)));
    
    // MAX_VALUE
    it = ctx.nativeFunctions.find("Number.MAX_VALUE");
    if (it != ctx.nativeFunctions.end()) {
        result = it->second(ctx, args);
        if (auto* d = std::get_if<double>(&result)) {
            EXPECT_GT(*d, 1e300);
        }
    }
    
    // MIN_VALUE
    it = ctx.nativeFunctions.find("Number.MIN_VALUE");
    if (it != ctx.nativeFunctions.end()) {
        result = it->second(ctx, args);
        if (auto* d = std::get_if<double>(&result)) {
            EXPECT_GT(*d, 0);
            EXPECT_LT(*d, 1e-300);
        }
    }
    
    // POSITIVE_INFINITY
    it = ctx.nativeFunctions.find("Number.POSITIVE_INFINITY");
    if (it != ctx.nativeFunctions.end()) {
        result = it->second(ctx, args);
        if (auto* d = std::get_if<double>(&result)) {
            EXPECT_TRUE(std::isinf(*d) && *d > 0);
        }
    }
    
    // NEGATIVE_INFINITY
    it = ctx.nativeFunctions.find("Number.NEGATIVE_INFINITY");
    if (it != ctx.nativeFunctions.end()) {
        result = it->second(ctx, args);
        if (auto* d = std::get_if<double>(&result)) {
            EXPECT_TRUE(std::isinf(*d) && *d < 0);
        }
    }
}

TEST_F(AVM2Test, NativeFunctionEscape) {
    AVM2Context ctx;
    
    auto it = ctx.nativeFunctions.find("escape");
    EXPECT_NE(it, ctx.nativeFunctions.end());
    
    std::vector<AVM2Value> args;
    args.push_back(std::string("Hello World!"));
    AVM2Value result = it->second(ctx, args);
    
    if (auto* s = std::get_if<std::string>(&result)) {
        EXPECT_NE(s->find("%20"), std::string::npos);
    }
}

TEST_F(AVM2Test, NativeFunctionUnescape) {
    AVM2Context ctx;
    
    auto it = ctx.nativeFunctions.find("unescape");
    EXPECT_NE(it, ctx.nativeFunctions.end());
    
    std::vector<AVM2Value> args;
    args.push_back(std::string("Hello%20World"));
    AVM2Value result = it->second(ctx, args);
    
    if (auto* s = std::get_if<std::string>(&result)) {
        EXPECT_EQ(*s, "Hello World");
    }
}

// ============================================================================
// Object Tests
// ============================================================================

TEST_F(AVM2Test, ObjectCreation) {
    auto obj = std::make_shared<AVM2Object>();
    EXPECT_NE(obj, nullptr);
}

TEST_F(AVM2Test, ObjectProperties) {
    auto obj = std::make_shared<AVM2Object>();
    
    // Set properties
    obj->setProperty("name", std::string("test"));
    obj->setProperty("value", static_cast<int32>(42));
    
    // Get properties
    auto val1 = obj->getProperty("name");
    auto val2 = obj->getProperty("value");
    auto val3 = obj->getProperty("nonexistent");
    
    if (auto* s = std::get_if<std::string>(&val1)) {
        EXPECT_EQ(*s, "test");
    }
    if (auto* i = std::get_if<int32_t>(&val2)) {
        EXPECT_EQ(*i, 42);
    }
    EXPECT_TRUE(std::get_if<std::monostate>(&val3));
}

TEST_F(AVM2Test, ObjectHasProperty) {
    auto obj = std::make_shared<AVM2Object>();
    
    obj->setProperty("key", std::string("value"));
    
    EXPECT_TRUE(obj->hasProperty("key"));
    EXPECT_FALSE(obj->hasProperty("missing"));
}

TEST_F(AVM2Test, ObjectDeleteProperty) {
    auto obj = std::make_shared<AVM2Object>();
    
    obj->setProperty("key", std::string("value"));
    EXPECT_TRUE(obj->hasProperty("key"));
    
    EXPECT_TRUE(obj->deleteProperty("key"));
    EXPECT_FALSE(obj->hasProperty("key"));
    
    EXPECT_FALSE(obj->deleteProperty("missing"));
}

TEST_F(AVM2Test, ObjectToString) {
    auto obj = std::make_shared<AVM2Object>();
    EXPECT_EQ(obj->toString(), "[object Object]");
    
    auto cls = std::make_shared<AVM2Class>();
    cls->name = "MyClass";
    obj->class_ = cls;
    
    EXPECT_EQ(obj->toString(), "[object MyClass]");
}

// ============================================================================
// Array Tests
// ============================================================================

TEST_F(AVM2Test, ArrayCreation) {
    auto arr = std::make_shared<AVM2Array>();
    EXPECT_NE(arr, nullptr);
    EXPECT_EQ(arr->length(), 0u);
}

TEST_F(AVM2Test, ArrayPushPop) {
    auto arr = std::make_shared<AVM2Array>();
    
    arr->push(static_cast<int32>(1));
    arr->push(static_cast<int32>(2));
    arr->push(static_cast<int32>(3));
    
    EXPECT_EQ(arr->length(), 3u);
    
    auto val = arr->pop();
    EXPECT_EQ(arr->length(), 2u);
    
    if (auto* i = std::get_if<int32_t>(&val)) {
        EXPECT_EQ(*i, 3);
    }
}

TEST_F(AVM2Test, ArrayShiftUnshift) {
    auto arr = std::make_shared<AVM2Array>();
    
    arr->push(static_cast<int32>(1));
    arr->push(static_cast<int32>(2));
    
    arr->unshift(static_cast<int32>(0));
    EXPECT_EQ(arr->length(), 3u);
    
    auto val = arr->shift();
    EXPECT_EQ(arr->length(), 2u);
    
    if (auto* i = std::get_if<int32_t>(&val)) {
        EXPECT_EQ(*i, 0);
    }
}

TEST_F(AVM2Test, ArrayLengthProperty) {
    auto arr = std::make_shared<AVM2Array>();
    
    arr->push(static_cast<int32>(1));
    arr->push(static_cast<int32>(2));
    
    auto len = arr->getProperty("length");
    if (auto* i = std::get_if<int32_t>(&len)) {
        EXPECT_EQ(*i, 2);
    }
    
    // Set length (truncate)
    arr->setProperty("length", static_cast<int32>(1));
    EXPECT_EQ(arr->length(), 1u);
}

TEST_F(AVM2Test, ArrayAccessByIndex) {
    auto arr = std::make_shared<AVM2Array>();
    
    arr->push(static_cast<int32>(10));
    arr->push(static_cast<int32>(20));
    arr->push(static_cast<int32>(30));
    
    if (auto* i = std::get_if<int32_t>(&(*arr)[0])) {
        EXPECT_EQ(*i, 10);
    }
    if (auto* i = std::get_if<int32_t>(&(*arr)[1])) {
        EXPECT_EQ(*i, 20);
    }
}

TEST_F(AVM2Test, ArrayReverse) {
    auto arr = std::make_shared<AVM2Array>();
    
    arr->push(static_cast<int32>(1));
    arr->push(static_cast<int32>(2));
    arr->push(static_cast<int32>(3));
    
    arr->reverse();
    
    if (auto* i = std::get_if<int32_t>(&(*arr)[0])) {
        EXPECT_EQ(*i, 3);
    }
    if (auto* i = std::get_if<int32_t>(&(*arr)[2])) {
        EXPECT_EQ(*i, 1);
    }
}

TEST_F(AVM2Test, ArrayJoin) {
    auto arr = std::make_shared<AVM2Array>();
    
    arr->push(std::string("a"));
    arr->push(std::string("b"));
    arr->push(std::string("c"));
    
    auto result = arr->join("-");
    if (auto* s = std::get_if<std::string>(&result)) {
        EXPECT_EQ(*s, "a-b-c");
    }
}

TEST_F(AVM2Test, ArraySlice) {
    auto arr = std::make_shared<AVM2Array>();
    
    arr->push(static_cast<int32>(1));
    arr->push(static_cast<int32>(2));
    arr->push(static_cast<int32>(3));
    arr->push(static_cast<int32>(4));
    
    auto sliced = arr->slice(1, 3);
    EXPECT_EQ(sliced.length(), 2u);
}

TEST_F(AVM2Test, ArraySplice) {
    auto arr = std::make_shared<AVM2Array>();
    
    arr->push(static_cast<int32>(1));
    arr->push(static_cast<int32>(2));
    arr->push(static_cast<int32>(3));
    arr->push(static_cast<int32>(4));
    
    std::vector<AVM2Value> items;
    items.push_back(static_cast<int32>(99));
    arr->splice(1, 2, items);
    
    EXPECT_EQ(arr->length(), 3u);
}

// ============================================================================
// Function Tests
// ============================================================================

TEST_F(AVM2Test, FunctionCreation) {
    auto fn = std::make_shared<AVM2Function>();
    EXPECT_NE(fn, nullptr);
}

TEST_F(AVM2Test, FunctionLengthProperty) {
    auto fn = std::make_shared<AVM2Function>();
    
    auto method = std::make_shared<ABCMethod>();
    method->paramCount = 3;
    fn->method = method;
    
    auto len = fn->getProperty("length");
    if (auto* i = std::get_if<int32_t>(&len)) {
        EXPECT_EQ(*i, 3);
    }
}

TEST_F(AVM2Test, FunctionPrototype) {
    auto fn = std::make_shared<AVM2Function>();
    
    auto proto = fn->getProperty("prototype");
    EXPECT_TRUE(std::holds_alternative<std::shared_ptr<AVM2Object>>(proto));
}

TEST_F(AVM2Test, FunctionBind) {
    auto fn = std::make_shared<AVM2Function>();
    auto thisObj = std::make_shared<AVM2Object>();
    
    auto bound = fn->bind(thisObj);
    EXPECT_EQ(bound->boundThis, thisObj);
}

// ============================================================================
// Class Tests
// ============================================================================

TEST_F(AVM2Test, ClassCreation) {
    auto cls = std::make_shared<AVM2Class>();
    cls->name = "TestClass";
    
    EXPECT_EQ(cls->name, "TestClass");
}

TEST_F(AVM2Test, ClassCreateInstance) {
    auto cls = std::make_shared<AVM2Class>();
    cls->name = "TestClass";
    
    auto instance = cls->createInstance();
    EXPECT_NE(instance, nullptr);
    EXPECT_EQ(instance->class_, cls);
}

TEST_F(AVM2Test, ClassPrototype) {
    auto cls = std::make_shared<AVM2Class>();
    
    auto proto = cls->getProperty("prototype");
    EXPECT_TRUE(std::holds_alternative<std::shared_ptr<AVM2Object>>(proto));
}

TEST_F(AVM2Test, ClassInheritance) {
    auto parent = std::make_shared<AVM2Class>();
    parent->name = "Parent";
    
    auto child = std::make_shared<AVM2Class>();
    child->name = "Child";
    child->superClass = parent;
    
    EXPECT_TRUE(child->isSubclassOf(parent));
    EXPECT_TRUE(child->isSubclassOf(child));
    EXPECT_FALSE(parent->isSubclassOf(child));
}

TEST_F(AVM2Test, ClassInitializeInstance) {
    auto cls = std::make_shared<AVM2Class>();
    cls->name = "TestClass";
    
    auto trait = std::make_shared<ABCTrait>();
    auto traitName = std::make_shared<ABCMultiname>();
    traitName->name = "field";
    trait->name = traitName;
    trait->kind = ABCTrait::Slot;
    trait->defaultValue = static_cast<int32>(42);
    cls->instanceTraits.push_back(trait);
    
    auto instance = cls->createInstance();
    auto val = instance->getProperty("field");
    
    if (auto* i = std::get_if<int32_t>(&val)) {
        EXPECT_EQ(*i, 42);
    }
}

// ============================================================================
// ABCFile Parsing Tests
// ============================================================================

TEST_F(AVM2Test, ABCFileCreation) {
    ABCFile abc;
    EXPECT_EQ(abc.intPool.size(), 0u);
    EXPECT_EQ(abc.stringPool.size(), 0u);
}

TEST_F(AVM2Test, ABCFileParseMinimal) {
    // Minimal ABC data (just header)
    std::vector<uint8_t> abcData = {
        0x00, 0x10,  // Minor version (16)
        0x00, 0x2E,  // Major version (46 = AVM2)
        // Constant pool - all empty counts
        0x00,        // int count = 0
        0x00,        // uint count = 0
        0x00,        // double count = 0
        0x00,        // string count = 0
        0x00,        // namespace count = 0
        0x00,        // namespace set count = 0
        0x00,        // multiname count = 0
        // Methods
        0x00,        // method count = 0
        // Metadata
        0x00,        // metadata count = 0
        // Instances/Classes
        0x00,        // instance count = 0
        // Scripts
        0x00,        // script count = 0
        // Method bodies
        0x00         // method body count = 0
    };
    
    ABCFile abc;
    EXPECT_TRUE(abc.parse(abcData.data(), abcData.size()));
    EXPECT_EQ(abc.minorVersion, 16);
    EXPECT_EQ(abc.majorVersion, 46);
}

TEST_F(AVM2Test, ABCFileGetters) {
    ABCFile abc;
    abc.intPool = {0, 42, 100};
    abc.stringPool = {"", "hello", "world"};
    
    EXPECT_EQ(abc.getInt(0), 0);
    EXPECT_EQ(abc.getInt(1), 42);
    EXPECT_EQ(abc.getInt(2), 100);
    
    EXPECT_EQ(abc.getString(0), "");
    EXPECT_EQ(abc.getString(1), "hello");
    EXPECT_EQ(abc.getString(2), "world");
}

// ============================================================================
// AVM2 Integration Tests
// ============================================================================

TEST_F(AVM2Test, AVM2LoadABC) {
    AVM2 vm;
    
    std::vector<uint8_t> abcData = {
        0x00, 0x10, 0x00, 0x2E,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00
    };
    
    EXPECT_TRUE(vm.loadABC(abcData));
}

TEST_F(AVM2Test, AVM2GetGlobal) {
    AVM2 vm;
    auto global = vm.getGlobal();
    EXPECT_NE(global, nullptr);
}

TEST_F(AVM2Test, AVM2RegisterNativeFunction) {
    AVM2 vm;
    
    bool called = false;
    vm.registerNativeFunction("testFunc", [&called](AVM2Context& ctx, const std::vector<AVM2Value>& args) -> AVM2Value {
        called = true;
        return static_cast<int32>(42);
    });
    
    auto& ctx = vm.getContext();
    EXPECT_NE(ctx.nativeFunctions.find("testFunc"), ctx.nativeFunctions.end());
}

// ============================================================================
// Type Conversion Tests
// ============================================================================

TEST_F(AVM2Test, TypeConversionHelpers) {
    AVM2 vm;
    
    // toNumber
    EXPECT_DOUBLE_EQ(vm.toNumber(static_cast<int32>(42)), 42.0);
    EXPECT_DOUBLE_EQ(vm.toNumber(3.14), 3.14);
    EXPECT_DOUBLE_EQ(vm.toNumber(true), 1.0);
    EXPECT_DOUBLE_EQ(vm.toNumber(false), 0.0);
    EXPECT_TRUE(std::isnan(vm.toNumber(std::monostate())));
    
    // toInt32
    EXPECT_EQ(vm.toInt32(42.9), 42);
    EXPECT_EQ(vm.toInt32(static_cast<int32>(100)), 100);
    
    // toString
    EXPECT_EQ(vm.toString(std::string("hello")), "hello");
    EXPECT_EQ(vm.toString(static_cast<int32>(42)), "42");
    EXPECT_EQ(vm.toString(true), "true");
    EXPECT_EQ(vm.toString(false), "false");
    EXPECT_EQ(vm.toString(std::monostate()), "undefined");
    
    // toBoolean
    EXPECT_TRUE(vm.toBoolean(true));
    EXPECT_FALSE(vm.toBoolean(false));
    EXPECT_TRUE(vm.toBoolean(static_cast<int32>(1)));
    EXPECT_FALSE(vm.toBoolean(static_cast<int32>(0)));
    EXPECT_TRUE(vm.toBoolean(3.14));
    EXPECT_FALSE(vm.toBoolean(0.0));
    EXPECT_TRUE(vm.toBoolean(std::string("hello")));
    EXPECT_FALSE(vm.toBoolean(std::string("")));
    EXPECT_FALSE(vm.toBoolean(std::monostate()));
}

// ============================================================================
// Garbage Collection Tests
// ============================================================================

TEST_F(AVM2Test, GarbageCollectorAddRemove) {
    AVM2GarbageCollector gc;
    
    auto obj = std::make_shared<AVM2Object>();
    gc.addObject(obj);
    
    EXPECT_EQ(gc.getObjectCount(), 1u);
}

TEST_F(AVM2Test, GarbageCollectorCollect) {
    AVM2GarbageCollector gc;
    
    auto obj1 = std::make_shared<AVM2Object>();
    auto obj2 = std::make_shared<AVM2Object>();
    
    gc.addObject(obj1);
    gc.addObject(obj2);
    
    // Mark and sweep with root
    std::vector<std::shared_ptr<AVM2Object>> roots = {obj1};
    gc.collect(roots);
    
    // obj1 should still be tracked, obj2 should be removed
    EXPECT_EQ(gc.getObjectCount(), 1u);
}

TEST_F(AVM2Test, AVM2GarbageCollection) {
    AVM2 vm;
    
    // Initial collection
    vm.collectGarbage();
    size_t initialCount = vm.getObjectCount();
    
    // Create objects
    auto global = vm.getGlobal();
    global->setProperty("obj", std::make_shared<AVM2Object>());
    
    // Collect again - objects in use should remain
    vm.collectGarbage();
    
    // Objects should still be there since global is a root
    EXPECT_GE(vm.getObjectCount(), initialCount);
}

// ============================================================================
// Exception Handling Tests
// ============================================================================

TEST_F(AVM2Test, ExceptionThrow) {
    AVM2Context ctx;
    
    ctx.throwException("Test error", "Error");
    
    EXPECT_TRUE(ctx.pendingException.has_value());
    EXPECT_EQ(ctx.pendingException->message, "Test error");
    EXPECT_EQ(ctx.pendingException->type, "Error");
}

TEST_F(AVM2Test, ExceptionThrowValue) {
    AVM2Context ctx;
    
    ctx.throwException(std::string("Error message"));
    
    EXPECT_TRUE(ctx.pendingException.has_value());
}

// ============================================================================
// Multiname Tests
// ============================================================================

TEST_F(AVM2Test, MultinameKinds) {
    auto qname = std::make_shared<ABCMultiname>();
    qname->kind = ABCMultiname::QName;
    EXPECT_TRUE(qname->isQName());
    EXPECT_FALSE(qname->isAttribute());
    
    auto qnameA = std::make_shared<ABCMultiname>();
    qnameA->kind = ABCMultiname::QNameA;
    EXPECT_TRUE(qnameA->isQName());
    EXPECT_TRUE(qnameA->isAttribute());
    
    auto multiname = std::make_shared<ABCMultiname>();
    multiname->kind = ABCMultiname::Multiname;
    EXPECT_TRUE(multiname->isMultiname());
}

// ============================================================================
// Namespace Tests
// ============================================================================

TEST_F(AVM2Test, NamespaceKinds) {
    auto ns = std::make_shared<AVM2Namespace>();
    ns->kind = AVM2Namespace::PackageNamespace;
    ns->name = "";
    
    EXPECT_TRUE(ns->isPackage());
    EXPECT_FALSE(ns->isPrivate());
    EXPECT_FALSE(ns->isProtected());
    
    ns->kind = AVM2Namespace::PrivateNs;
    EXPECT_TRUE(ns->isPrivate());
}

// ============================================================================
// Trait Tests
// ============================================================================

TEST_F(AVM2Test, TraitKinds) {
    auto trait = std::make_shared<ABCTrait>();
    
    trait->kind = ABCTrait::Slot;
    EXPECT_TRUE(trait->isSlot());
    EXPECT_FALSE(trait->isMethod());
    
    trait->kind = ABCTrait::Method;
    EXPECT_TRUE(trait->isMethod());
    EXPECT_FALSE(trait->isSlot());
    
    trait->kind = ABCTrait::Getter;
    EXPECT_TRUE(trait->isGetter());
    
    trait->kind = ABCTrait::Const;
    EXPECT_TRUE(trait->isConst());
}

TEST_F(AVM2Test, TraitAttributes) {
    auto trait = std::make_shared<ABCTrait>();
    
    trait->dispKind = ABCTrait::Final;
    EXPECT_TRUE(trait->isFinal());
    
    trait->dispKind = ABCTrait::Override;
    EXPECT_TRUE(trait->isOverride());
}

// ============================================================================
// Method Tests
// ============================================================================

TEST_F(AVM2Test, MethodFlags) {
    auto method = std::make_shared<ABCMethod>();
    
    method->flags = ABCMethod::FLAG_NEED_REST;
    EXPECT_TRUE(method->needsRest());
    
    method->flags = ABCMethod::FLAG_NEED_ACTIVATION;
    EXPECT_TRUE(method->needsActivation());
    
    method->flags = ABCMethod::FLAG_HAS_OPTIONAL;
    EXPECT_TRUE(method->hasOptional());
}

// ============================================================================
// Instance Tests
// ============================================================================

TEST_F(AVM2Test, InstanceFlags) {
    auto instance = std::make_shared<ABCInstance>();
    
    instance->flags = ABCInstance::FLAG_SEALED;
    EXPECT_TRUE(instance->isSealed());
    
    instance->flags = ABCInstance::FLAG_FINAL;
    EXPECT_TRUE(instance->isFinal());
    
    instance->flags = ABCInstance::FLAG_INTERFACE;
    EXPECT_TRUE(instance->isInterface());
}

// ============================================================================
// Integration / Complex Tests
// ============================================================================

TEST_F(AVM2Test, ComplexObjectGraph) {
    AVM2 vm;
    
    // Create a complex object graph
    auto parent = std::make_shared<AVM2Object>();
    auto child1 = std::make_shared<AVM2Object>();
    auto child2 = std::make_shared<AVM2Object>();
    
    parent->setProperty("child1", child1);
    parent->setProperty("child2", child2);
    child1->setProperty("sibling", child2);
    
    // Set on global
    vm.getGlobal()->setProperty("parent", parent);
    
    // Access through properties
    auto retrieved = vm.getGlobal()->getProperty("parent");
    if (auto* p = std::get_if<std::shared_ptr<AVM2Object>>(&retrieved)) {
        auto c1 = (*p)->getProperty("child1");
        EXPECT_TRUE(std::holds_alternative<std::shared_ptr<AVM2Object>>(c1));
    }
}

TEST_F(AVM2Test, ArrayAsPropertyValue) {
    AVM2 vm;
    
    auto arr = std::make_shared<AVM2Array>();
    arr->push(static_cast<int32>(1));
    arr->push(static_cast<int32>(2));
    arr->push(static_cast<int32>(3));
    
    vm.getGlobal()->setProperty("myArray", arr);
    
    auto retrieved = vm.getGlobal()->getProperty("myArray");
    EXPECT_TRUE(std::holds_alternative<std::shared_ptr<AVM2Array>>(retrieved));
}

TEST_F(AVM2Test, FunctionAsPropertyValue) {
    AVM2 vm;
    
    auto fn = std::make_shared<AVM2Function>();
    fn->isNative = true;
    fn->nativeImpl = [](AVM2Context& ctx, const std::vector<AVM2Value>& args) -> AVM2Value {
        return static_cast<int32>(42);
    };
    
    vm.getGlobal()->setProperty("myFunc", fn);
    
    auto retrieved = vm.getGlobal()->getProperty("myFunc");
    EXPECT_TRUE(std::holds_alternative<std::shared_ptr<AVM2Function>>(retrieved));
}

TEST_F(AVM2Test, PrototypeChain) {
    auto parent = std::make_shared<AVM2Object>();
    parent->setProperty("inherited", static_cast<int32>(100));
    
    auto child = std::make_shared<AVM2Object>();
    child->prototype = parent;
    
    // Should find property through prototype chain
    auto val = child->getProperty("inherited");
    if (auto* i = std::get_if<int32_t>(&val)) {
        EXPECT_EQ(*i, 100);
    }
}

TEST_F(AVM2Test, ClassRegistration) {
    AVM2 vm;
    
    auto cls = std::make_shared<AVM2Class>();
    cls->name = "MyClass";
    
    vm.registerClass(cls);
    
    auto retrieved = vm.getClass("MyClass");
    EXPECT_EQ(retrieved, cls);
    
    // Should also be on global object
    auto globalVal = vm.getGlobal()->getProperty("MyClass");
    EXPECT_TRUE(std::holds_alternative<std::shared_ptr<AVM2Class>>(globalVal));
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
