#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include "avm2/avm2.h"

using namespace libswf;

void printUsage(const char* progName) {
    std::cout << "Usage: " << progName << " <abc_file>" << std::endl;
    std::cout << std::endl;
    std::cout << "Executes an ActionScript 3 ABC file." << std::endl;
}

std::vector<uint8_t> readFile(const std::string& filename) {
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file) {
        return {};
    }
    
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);
    
    std::vector<uint8_t> data(size);
    file.read(reinterpret_cast<char*>(data.data()), size);
    
    return data;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }
    
    std::string filename = argv[1];
    
    std::cout << "Loading ABC file: " << filename << std::endl;
    
    std::vector<uint8_t> data = readFile(filename);
    if (data.empty()) {
        std::cerr << "Error: Could not read file" << std::endl;
        return 1;
    }
    
    std::cout << "File size: " << data.size() << " bytes" << std::endl;
    
    // Create VM
    AVM2 vm;
    
    std::cout << "Loading ABC..." << std::endl;
    if (!vm.loadABC(data)) {
        std::cerr << "Error loading ABC: " << vm.getError() << std::endl;
        return 1;
    }
    
    std::cout << "ABC loaded successfully!" << std::endl;
    std::cout << "Executing..." << std::endl;
    
    try {
        vm.execute();
        std::cout << "Execution complete!" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Execution error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
