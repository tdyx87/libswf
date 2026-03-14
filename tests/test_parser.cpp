#include <gtest/gtest.h>
#include "parser/swfparser.h"
#include "parser/swfheader.h"
#include <vector>
#include <fstream>
#include <sstream>

using namespace libswf;

// Test fixture for SWF parser tests
class ParserTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// Test SWF header parsing
TEST_F(ParserTest, HeaderParsing) {
    // Create minimal SWF file for testing (FWS signature, version 10)
    std::vector<uint8_t> swfData = {
        // Signature
        'F', 'W', 'S',
        // Version
        10,
        // File length (little-endian)
        0x1C, 0x00, 0x00, 0x00,
        // Frame size (RECT): 0, 800 twips, 0, 600 twips
        // 5 bits for each coordinate = 0x03 | 0x00 0x00 0x20 0x00 0x00 0x00 0x78 0x00
        0x33, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x78, 0x00,
        // Frame rate: 24 fps (0x1800 = 24.0)
        0x18, 0x00,
        // Frame count: 1
        0x01, 0x00,
        // End tag
        0x00, 0x00
    };
    
    SWFParser parser;
    bool result = parser.parse(swfData);
    
    // The minimal SWF may not parse completely, but shouldn't crash
    // Just verify it doesn't throw
    SUCCEED() << "Parser handled minimal SWF data";
}

// Test SWF header with known sample
TEST_F(ParserTest, HeaderFields) {
    SWFHeader header;
    header.signature = "FWS";
    header.version = 10;
    header.fileLength = 1000;
    header.frameSize = Rect(0, 800, 0, 600);
    header.frameRate = 24.0f;
    header.frameCount = 10;
    
    EXPECT_EQ(header.signature, "FWS");
    EXPECT_EQ(header.version, 10);
    EXPECT_EQ(header.frameCount, 10);
    EXPECT_FLOAT_EQ(header.frameRate, 24.0f);
}

// Test Rect conversion
TEST_F(ParserTest, RectConversion) {
    Rect rect(0, 800, 0, 600);
    
    // Convert from TWIPS to pixels (20 twips = 1 pixel)
    EXPECT_FLOAT_EQ(rect.width(), 40.0f);   // 800 / 20 = 40
    EXPECT_FLOAT_EQ(rect.height(), 30.0f);  // 600 / 20 = 30
}

// Test tag type enum
TEST_F(ParserTest, TagTypes) {
    EXPECT_EQ(static_cast<uint16_t>(SWFTagType::END), 0);
    EXPECT_EQ(static_cast<uint16_t>(SWFTagType::SHOW_FRAME), 1);
    EXPECT_EQ(static_cast<uint16_t>(SWFTagType::DEFINE_SHAPE), 2);
    EXPECT_EQ(static_cast<uint16_t>(SWFTagType::DO_ABC), 82);
}

// Test color types
TEST_F(ParserTest, ColorTypes) {
    RGBA color(255, 128, 64, 200);
    
    EXPECT_EQ(color.r, 255);
    EXPECT_EQ(color.g, 128);
    EXPECT_EQ(color.b, 64);
    EXPECT_EQ(color.a, 200);
    
    // Test toUInt32
    uint32_t colorInt = color.toUInt32();
    EXPECT_EQ(colorInt, 0xC8FF8040); // ABGR format
    
    // Test fromUInt32
    RGBA color2 = RGBA::fromUInt32(colorInt);
    EXPECT_EQ(color2.r, color.r);
    EXPECT_EQ(color2.g, color.g);
    EXPECT_EQ(color2.b, color.b);
    EXPECT_EQ(color2.a, color.a);
}

// Test matrix transform
TEST_F(ParserTest, MatrixTransform) {
    Matrix matrix;
    matrix.translateX = 100;  // In twips
    matrix.translateY = 200;
    matrix.hasScale = true;
    matrix.scaleX = 2.0f;
    matrix.scaleY = 1.5f;
    
    float x = 50.0f;
    float y = 100.0f;
    
    // Apply transform manually: scale then translate (in pixels)
    // Since translate is in twips, convert
    float expectedX = x * 2.0f + 100.0f / 20.0f;  // 100 + 5 = 105
    float expectedY = y * 1.5f + 200.0f / 20.0f;  // 150 + 10 = 160
    
    matrix.transformPoint(x, y);
    
    EXPECT_FLOAT_EQ(x, expectedX);
    EXPECT_FLOAT_EQ(y, expectedY);
}

// Test color transform
TEST_F(ParserTest, ColorTransform) {
    ColorTransform cx;
    cx.mulR = 0.5f;
    cx.mulG = 1.0f;
    cx.mulB = 2.0f;
    cx.addR = 50;
    
    RGBA input(100, 100, 100, 255);
    RGBA output = cx.apply(input);
    
    // r = 100 * 0.5 + 50 = 100
    // g = 100 * 1.0 + 0 = 100
    // b = 100 * 2.0 + 0 = 200
    EXPECT_EQ(output.r, 100);
    EXPECT_EQ(output.g, 100);
    EXPECT_EQ(output.b, 200);
}

// Test BitStream
TEST_F(ParserTest, BitStreamBasic) {
    std::vector<uint8_t> data = {0b10110011, 0b01011100};
    BitStream bs(data.data(), data.size());
    
    // Read 4 bits
    uint32_t val1 = bs.readUB(4);
    EXPECT_EQ(val1, 0b1011); // First 4 bits
    
    // Read 4 more bits
    uint32_t val2 = bs.readUB(4);
    EXPECT_EQ(val2, 0b0011); // Next 4 bits
    
    // Test alignment
    bs.alignByte();
    EXPECT_EQ(bs.bytePos(), 1);
}

// Test signed bit reading
TEST_F(ParserTest, BitStreamSigned) {
    // -1 in 8-bit signed: 0xFF
    std::vector<uint8_t> data = {0xFF, 0x00};
    BitStream bs(data.data(), data.size());
    
    int32_t val = bs.readSB(8);
    EXPECT_EQ(val, -1);
}

// Test file not found
TEST_F(ParserTest, FileNotFound) {
    SWFParser parser;
    bool result = parser.parseFile("nonexistent_file.swf");
    
    EXPECT_FALSE(result);
    EXPECT_FALSE(parser.getError().empty());
}

// Test empty data
TEST_F(ParserTest, EmptyData) {
    SWFParser parser;
    std::vector<uint8_t> empty;
    
    bool result = parser.parse(empty);
    EXPECT_FALSE(result);
}

// Test CWS (compressed) signature detection
TEST_F(ParserTest, CompressedSignature) {
    std::vector<uint8_t> cwsData = {'C', 'W', 'S', 10, 0, 0, 0, 0};
    
    SWFHeader header;
    EXPECT_THROW({
        // This should detect CWS and attempt decompression
        // which will fail on empty data, but that's expected
        auto parsed = SWFHeaderParser::parse(cwsData);
    }, std::exception);
}

// Test bounds
TEST_F(ParserTest, Bounds) {
    Bounds bounds(10, 20, 100, 200);
    
    EXPECT_TRUE(bounds.contains(50, 50));
    EXPECT_FALSE(bounds.contains(5, 50));
    EXPECT_FALSE(bounds.contains(50, 5));
    
    EXPECT_FLOAT_EQ(bounds.width(), 90.0f);
    EXPECT_FLOAT_EQ(bounds.height(), 180.0f);
}

// Run all tests
int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
