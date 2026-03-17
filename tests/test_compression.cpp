// Test SWF compression/decompression
#include <gtest/gtest.h>
#include "parser/swfparser.h"
#include "parser/swfheader.h"
#include <vector>
#include <cstring>
#include <zlib.h>

using namespace libswf;

// Helper: Compress data using zlib (raw deflate)
std::vector<uint8_t> compressZlib(const std::vector<uint8_t>& input) {
    std::vector<uint8_t> output(input.size() * 2);
    z_stream stream = {};
    stream.next_in = const_cast<uint8_t*>(input.data());
    stream.avail_in = input.size();
    stream.next_out = output.data();
    stream.avail_out = output.size();
    
    deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    deflate(&stream, Z_FINISH);
    deflateEnd(&stream);
    
    output.resize(stream.total_out);
    return output;
}

// Create a minimal valid uncompressed SWF
std::vector<uint8_t> createMinimalSWF() {
    return {
        'F', 'W', 'S',       // Signature
        10,                  // Version
        17, 0, 0, 0,        // File length (17 bytes)
        0x05, 0x00, 0x00,   // RECT (minimal)
        0x18, 0x00,         // Frame rate: 24.0
        0x01, 0x00,         // Frame count: 1
        0x00, 0x00          // End tag
    };
}

// Create CWS (compressed) version of a SWF
std::vector<uint8_t> createCWS(const std::vector<uint8_t>& uncompressed) {
    // Compress data after header (offset 8)
    std::vector<uint8_t> payload(uncompressed.begin() + 8, uncompressed.end());
    std::vector<uint8_t> compressed = compressZlib(payload);
    
    // Build CWS file
    std::vector<uint8_t> cws;
    cws.push_back('C');
    cws.push_back('W');
    cws.push_back('S');
    cws.push_back(uncompressed[3]);  // Version
    // File length (uncompressed)
    cws.push_back(uncompressed[4]);
    cws.push_back(uncompressed[5]);
    cws.push_back(uncompressed[6]);
    cws.push_back(uncompressed[7]);
    // Compressed payload
    cws.insert(cws.end(), compressed.begin(), compressed.end());
    
    return cws;
}

// Test CWS decompression
TEST(CompressionTest, DecompressCWS) {
    auto uncompressed = createMinimalSWF();
    auto cws = createCWS(uncompressed);
    
    SWFParser parser;
    ASSERT_TRUE(parser.parse(cws)) << "Parser failed: " << parser.getError();
    
    EXPECT_EQ(parser.getDocument().header.signature, "CWS");
    EXPECT_EQ(parser.getDocument().header.version, 10);
    EXPECT_EQ(parser.getDocument().header.frameCount, 1);
}

// Test CWS via parseFile
TEST(CompressionTest, DecompressCWSFromFile) {
    // Write temp file
    auto uncompressed = createMinimalSWF();
    auto cws = createCWS(uncompressed);
    
    FILE* fp = fopen("/tmp/test_cws.swf", "wb");
    ASSERT_NE(fp, nullptr);
    fwrite(cws.data(), 1, cws.size(), fp);
    fclose(fp);
    
    SWFParser parser;
    ASSERT_TRUE(parser.parseFile("/tmp/test_cws.swf")) << "Parser failed: " << parser.getError();
    
    EXPECT_EQ(parser.getDocument().header.signature, "CWS");
    EXPECT_EQ(parser.getDocument().header.frameCount, 1);
    
    unlink("/tmp/test_cws.swf");
}

// Test corrupted CWS (truncated)
TEST(CompressionTest, CorruptedCWSTruncated) {
    auto uncompressed = createMinimalSWF();
    auto cws = createCWS(uncompressed);
    
    // Truncate compressed data
    cws.resize(cws.size() - 5);
    
    SWFParser parser;
    EXPECT_FALSE(parser.parse(cws));
    EXPECT_NE(parser.getError().find("Decompression failed"), std::string::npos)
        << "Error message: " << parser.getError();
}

// Test corrupted CWS (wrong fileLength - too small)
TEST(CompressionTest, CorruptedCWSWrongLengthSmall) {
    auto uncompressed = createMinimalSWF();
    auto cws = createCWS(uncompressed);
    
    // Modify fileLength to be too small
    cws[4] = 10;  // Original is 17
    
    SWFParser parser;
    EXPECT_FALSE(parser.parse(cws));
    // Should fail due to size mismatch
}

// Test direct decompressZlib function
TEST(CompressionTest, DirectDecompressZlib) {
    const char* testData = "Hello, World!";
    std::vector<uint8_t> input(testData, testData + strlen(testData));
    
    // Compress
    std::vector<uint8_t> compressed = compressZlib(input);
    
    // Decompress using our function
    std::vector<uint8_t> output = SWFHeaderParser::decompressZlib(
        compressed.data(), compressed.size(), input.size());
    
    EXPECT_EQ(output.size(), input.size());
    EXPECT_EQ(memcmp(output.data(), input.data(), input.size()), 0);
}

// Test decompressZlib with wrong size
TEST(CompressionTest, DecompressZlibWrongSize) {
    const char* testData = "Hello, World!";
    std::vector<uint8_t> input(testData, testData + strlen(testData));
    std::vector<uint8_t> compressed = compressZlib(input);
    
    // Try to decompress with wrong (smaller) size - should fail
    EXPECT_THROW({
        SWFHeaderParser::decompressZlib(compressed.data(), compressed.size(), 5);
    }, std::exception);
}

// Test empty compressed data
TEST(CompressionTest, EmptyCompressedData) {
    std::vector<uint8_t> cws = {'C', 'W', 'S', 10, 17, 0, 0, 0};  // No compressed payload
    
    SWFParser parser;
    EXPECT_FALSE(parser.parse(cws));
}

// Test large fileLength (potential attack)
TEST(CompressionTest, SuspiciousFileLength) {
    auto uncompressed = createMinimalSWF();
    auto cws = createCWS(uncompressed);
    
    // Set huge fileLength (1GB)
    cws[4] = 0x00;
    cws[5] = 0x00;
    cws[6] = 0x00;
    cws[7] = 0x40;
    
    SWFParser parser;
    EXPECT_FALSE(parser.parse(cws));
    EXPECT_NE(parser.getError().find("size"), std::string::npos)
        << "Should report size mismatch: " << parser.getError();
}

// Test FWS (uncompressed) still works
TEST(CompressionTest, UncompressedFWS) {
    auto uncompressed = createMinimalSWF();
    
    SWFParser parser;
    ASSERT_TRUE(parser.parse(uncompressed));
    
    EXPECT_EQ(parser.getDocument().header.signature, "FWS");
    EXPECT_EQ(parser.getDocument().header.frameCount, 1);
}

// Run all tests
int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
