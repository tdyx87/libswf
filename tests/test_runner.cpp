#include <iostream>
#include <string>
#include <vector>
#include "parser/swfparser.h"
#include "parser/swfheader.h"
#include "avm2/avm2.h"
#include "common/types.h"

using namespace libswf;

int testsPassed = 0;
int testsFailed = 0;

#define TEST(name, expr) do { \
    std::cout << "Running " << name << "... "; \
    if (expr) { \
        std::cout << "PASSED" << std::endl; \
        testsPassed++; \
    } else { \
        std::cout << "FAILED" << std::endl; \
        testsFailed++; \
    } \
} while(0)

#define TEST_THROW(name, expr) do { \
    std::cout << "Running " << name << "... "; \
    try { \
        expr; \
        std::cout << "PASSED (no exception)" << std::endl; \
        testsPassed++; \
    } catch (const std::exception& e) { \
        std::cout << "FAILED (exception: " << e.what() << ")" << std::endl; \
        testsFailed++; \
    } \
} while(0)

void testTypes() {
    std::cout << "\n=== Testing Basic Types ===" << std::endl;
    
    // Test RGBA
    RGBA color(255, 128, 64, 200);
    TEST("RGBA creation", color.r == 255 && color.g == 128 && color.b == 64 && color.a == 200);
    
    // Test RGBA toUInt32
    uint32_t colorInt = color.toUInt32();
    TEST("RGBA toUInt32", colorInt != 0);
    
    // Test Rect
    Rect rect(0, 800, 0, 600);
    TEST("Rect width", rect.width() == 40.0f);
    TEST("Rect height", rect.height() == 30.0f);
    
    // Test Matrix
    Matrix matrix;
    matrix.translateX = 100;
    matrix.translateY = 200;
    float x = 50.0f, y = 100.0f;
    matrix.transformPoint(x, y);
    TEST("Matrix transform", x > 0 && y > 0);
    
    // Test ColorTransform
    ColorTransform cx;
    cx.mulR = 0.5f;
    cx.addR = 50;
    RGBA input(100, 100, 100, 255);
    RGBA output = cx.apply(input);
    TEST("ColorTransform apply", output.r == 100);
}

void testBitStream() {
    std::cout << "\n=== Testing BitStream ===" << std::endl;
    
    std::vector<uint8_t> data = {0b10110011, 0b01011100};
    BitStream bs(data.data(), data.size());
    
    uint32_t val1 = bs.readUB(4);
    TEST("BitStream readUB", val1 == 0b1011);
    
    uint32_t val2 = bs.readUB(4);
    TEST("BitStream readUB 2", val2 == 0b0011);
    
    bs.alignByte();
    TEST("BitStream alignByte", bs.bytePos() == 1);
    
    // Test signed reading
    std::vector<uint8_t> signedData = {0xFF, 0x00};
    BitStream ss(signedData.data(), signedData.size());
    int32_t signedVal = ss.readSB(8);
    TEST("BitStream readSB", signedVal == -1);
}

void testParser() {
    std::cout << "\n=== Testing SWF Parser ===" << std::endl;
    
    // Test empty data
    SWFParser parser1;
    std::vector<uint8_t> empty;
    TEST("Parse empty data", !parser1.parse(empty));
    
    // Test file not found
    SWFParser parser2;
    TEST("Parse nonexistent file", !parser2.parseFile("nonexistent.swf"));
    
    // Test valid header parsing
    std::vector<uint8_t> minimalSWF = {
        'F', 'W', 'S',
        10,  // version
        0x1C, 0x00, 0x00, 0x00,  // length
        0x33, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x78, 0x00,  // frame size
        0x18, 0x00,  // frame rate
        0x01, 0x00,  // frame count
        0x00, 0x00   // end tag
    };
    
    SWFParser parser3;
    TEST_THROW("Parse minimal SWF", parser3.parse(minimalSWF));
}

void testAVM2() {
    std::cout << "\n=== Testing AVM2 ===" << std::endl;
    
    // Test VM creation
    AVM2 vm;
    TEST("AVM2 creation", true);
    
    // Test context
    AVM2Context ctx;
    TEST("AVM2Context creation", ctx.globalObject != nullptr);
    
    // Test native functions
    auto it = ctx.nativeFunctions.find("trace");
    TEST("Native function trace exists", it != ctx.nativeFunctions.end());
    
    it = ctx.nativeFunctions.find("isNaN");
    TEST("Native function isNaN exists", it != ctx.nativeFunctions.end());
    
    it = ctx.nativeFunctions.find("Number.NaN");
    TEST("Native function Number.NaN exists", it != ctx.nativeFunctions.end());
    
    // Test stack operations
    ctx.push(42);
    ctx.push(3.14);
    TEST("Stack push", ctx.stackSize() == 2);
    
    auto val = ctx.pop();
    TEST("Stack pop", ctx.stackSize() == 1);
    
    // Test object
    auto obj = std::make_shared<AVM2Object>();
    obj->setProperty("test", std::string("value"));
    auto retrieved = obj->getProperty("test");
    TEST("Object property", !std::get_if<std::monostate>(&retrieved));
    
    // Test array
    auto arr = std::make_shared<AVM2Array>();
    arr->push(1);
    arr->push(2);
    arr->push(3);
    TEST("Array length", arr->length() == 3);
}

int main() {
    std::cout << "libswf Simple Test Runner" << std::endl;
    std::cout << "==========================" << std::endl;
    
    testTypes();
    testBitStream();
    testParser();
    testAVM2();
    
    std::cout << "\n=== Results ===" << std::endl;
    std::cout << "Passed: " << testsPassed << std::endl;
    std::cout << "Failed: " << testsFailed << std::endl;
    
    return testsFailed > 0 ? 1 : 0;
}
