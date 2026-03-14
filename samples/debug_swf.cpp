#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <swf_file>" << std::endl;
        return 1;
    }
    
    std::ifstream file(argv[1], std::ios::binary);
    if (!file) {
        std::cerr << "Cannot open file" << std::endl;
        return 1;
    }
    
    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);
    
    std::vector<uint8_t> data(size);
    file.read(reinterpret_cast<char*>(data.data()), size);
    
    std::cout << "File size: " << size << " bytes" << std::endl;
    std::cout << "Raw bytes:" << std::endl;
    
    for (size_t i = 0; i < size && i < 32; ++i) {
        printf("%02x ", data[i]);
        if ((i + 1) % 16 == 0) std::cout << std::endl;
    }
    std::cout << std::endl;
    
    // Parse header manually
    std::string sig(data.begin(), data.begin() + 3);
    std::cout << "Signature: " << sig << std::endl;
    
    uint8_t version = data[3];
    std::cout << "Version: " << (int)version << std::endl;
    
    uint32_t fileLength = *reinterpret_cast<uint32_t*>(&data[4]);
    std::cout << "File length: " << fileLength << std::endl;
    
    // Parse RECT
    uint8_t rectByte = data[8];
    int nBits = (rectByte >> 3) & 0x1F;
    std::cout << "NBits: " << nBits << std::endl;
    
    // Print bytes 8-20 for debugging
    std::cout << "Bytes 8-20:" << std::endl;
    for (size_t i = 8; i < size && i < 21; ++i) {
        printf("%02x ", data[i]);
    }
    std::cout << std::endl;
    
    return 0;
}
