// Performance profiling test for libswf
// Build with: cmake .. -DENABLE_PROFILING=ON

#include <iostream>
#include <vector>
#include <cmath>

#include "common/types.h"
#include "common/bitstream.h"
#include "common/profiler.h"
#include "parser/swfparser.h"
#include "avm2/avm2.h"

using namespace libswf;

void testBitStreamPerformance() {
    std::cout << "\n=== BitStream Performance Test ===\n";
    
    // Create test data
    std::vector<uint8_t> data(1024 * 1024);  // 1MB
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>(i & 0xFF);
    }
    
    {
        ProfileScope scope("BitStream 1MB read");
        
        BitStream bs(data.data(), data.size());
        uint32_t sum = 0;
        
        // Read 32-bit values
        for (size_t i = 0; i < data.size() / 4; ++i) {
            sum += bs.readLE32();
        }
        
        std::cout << "  Sum: " << sum << " (prevent optimization)" << std::endl;
    }
    
    {
        ProfileScope scope("BitStream bit reads");
        
        BitStream bs(data.data(), data.size());
        uint32_t sum = 0;
        
        // Read individual bits
        for (size_t i = 0; i < 100000; ++i) {
            sum += bs.readUB(8);
            if (bs.bytePos() >= data.size() - 1) {
                bs.reset();
            }
        }
        
        std::cout << "  Sum: " << sum << " (prevent optimization)" << std::endl;
    }
}

void testMatrixPerformance() {
    std::cout << "\n=== Matrix Performance Test ===\n";
    
    {
        ProfileScope scope("Matrix transform 1M points");
        
        Matrix m;
        m.translateX = 100;
        m.translateY = 200;
        m.scaleX = 1.5f;
        m.scaleY = 1.5f;
        m.hasScale = true;
        m.hasRotate = true;
        // Rotation matrix components: cos(45°) = sin(45°) = 0.7071
        m.rotate0 = 0.7071f;
        m.rotate1 = 0.7071f;
        
        float sumX = 0, sumY = 0;
        
        for (int i = 0; i < 1000000; ++i) {
            float x = static_cast<float>(i % 1000);
            float y = static_cast<float>(i / 1000);
            m.transformPoint(x, y);
            sumX += x;
            sumY += y;
        }
        
        std::cout << "  Sum: " << sumX << ", " << sumY << " (prevent optimization)" << std::endl;
    }
}

void testAVM2Performance() {
    std::cout << "\n=== AVM2 Performance Test ===\n";
    
    {
        ProfileScope scope("AVM2 stack operations");
        
        AVM2Context ctx;
        
        // Push/pop 100k values
        for (int i = 0; i < 100000; ++i) {
            ctx.push(i);
            ctx.push(3.14159);
            ctx.push(std::string("test"));
            
            ctx.pop();
            ctx.pop();
            ctx.pop();
        }
    }
    
    {
        ProfileScope scope("AVM2 object creation");
        
        AVM2Context ctx;
        std::vector<std::shared_ptr<AVM2Object>> objects;
        objects.reserve(10000);
        
        for (int i = 0; i < 10000; ++i) {
            auto obj = std::make_shared<AVM2Object>();
            obj->setProperty("id", i);
            obj->setProperty("name", std::string("object_") + std::to_string(i));
            objects.push_back(obj);
        }
        
        std::cout << "  Created " << objects.size() << " objects" << std::endl;
    }
}

void testMemoryTracking() {
    std::cout << "\n=== Memory Tracking Test ===\n";
    
    MemoryTracker::instance().reset();
    
    // Simulate allocations
    MemoryTracker::instance().allocate(1024, "test1");
    MemoryTracker::instance().allocate(2048, "test2");
    MemoryTracker::instance().allocate(4096, "test3");
    
    MemoryTracker::instance().deallocate(1024);
    
    std::cout << "Current allocated: " << MemoryTracker::instance().getCurrentAllocated() << " bytes\n";
    std::cout << "Peak allocated: " << MemoryTracker::instance().getPeakAllocated() << " bytes\n";
    std::cout << "Total allocated: " << MemoryTracker::instance().getTotalAllocated() << " bytes\n";
}

void printUsage() {
    std::cout << "Usage: profile_test [options]\n"
              << "Options:\n"
              << "  --all       Run all tests (default)\n"
              << "  --bitstream Run BitStream performance test\n"
              << "  --matrix    Run Matrix performance test\n"
              << "  --avm2      Run AVM2 performance test\n"
              << "  --memory    Run memory tracking test\n"
              << "  --help      Show this help message\n";
}

int main(int argc, char** argv) {
    std::cout << "libswf Performance Profiling Test\n"
              << "=================================\n"
              << "Build with: cmake .. -DENABLE_PROFILING=ON\n\n";
    
    if (argc > 1 && std::string(argv[1]) == "--help") {
        printUsage();
        return 0;
    }
    
    bool runAll = (argc == 1);
    bool runBitStream = runAll || (argc > 1 && std::string(argv[1]) == "--bitstream");
    bool runMatrix = runAll || (argc > 1 && std::string(argv[1]) == "--matrix");
    bool runAVM2 = runAll || (argc > 1 && std::string(argv[1]) == "--avm2");
    bool runMemory = runAll || (argc > 1 && std::string(argv[1]) == "--memory");
    
    if (runBitStream) testBitStreamPerformance();
    if (runMatrix) testMatrixPerformance();
    if (runAVM2) testAVM2Performance();
    if (runMemory) testMemoryTracking();
    
    // Print performance report
    std::cout << "\n";
    PerformanceCounter::instance().printReport();
    
    std::cout << "\n=== All Tests Completed ===\n";
    return 0;
}
