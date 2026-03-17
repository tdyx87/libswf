// Copyright (c) 2025 libswf authors. All rights reserved.
// Renderer Abstraction Demo - Shows how to use the new IRenderer interface

#include <renderer/irenderer.h>
#include <renderer/render_backend.h>
#include <iostream>
#include <memory>

using namespace libswf;

int main(int argc, char* argv[]) {
    std::cout << "========================================" << std::endl;
    std::cout << "Renderer Abstraction Demo" << std::endl;
    std::cout << "========================================" << std::endl;
    
    // Get available renderers
    auto available = RendererFactory::getAvailableRenderers();
    std::cout << "\nAvailable renderers:" << std::endl;
    for (const auto& name : available) {
        std::cout << "  - " << name;
        if (name == "gdiplus") {
            std::cout << " (Windows only)";
        } else if (name == "cairo") {
            std::cout << " (Linux/Unix)";
        } else if (name == "software") {
            std::cout << " (All platforms)";
        }
        std::cout << std::endl;
    }
    
    // Create software renderer (guaranteed to work on all platforms)
    std::cout << "\nCreating Software renderer..." << std::endl;
    auto renderer = RendererFactory::createSoftwareRenderer();
    
    if (!renderer) {
        std::cerr << "Failed to create renderer!" << std::endl;
        return 1;
    }
    
    // Configure renderer
    RenderConfig config;
    config.width = 800;
    config.height = 600;
    config.frameRate = 24.0f;
    config.enableAA = true;
    config.scaleMode = RenderConfig::ScaleMode::SHOW_ALL;
    
    std::cout << "Initializing renderer with " << config.width << "x" << config.height 
              << " @ " << config.frameRate << "fps" << std::endl;
    
    if (!renderer->initialize(config)) {
        std::cerr << "Failed to initialize renderer: " << renderer->getLastError() << std::endl;
        return 1;
    }
    
    // Show backend info
    auto backend = renderer->getBackend();
    if (backend) {
        std::cout << "\nBackend info:" << std::endl;
        std::cout << "  Name: " << backend->getName() << std::endl;
        std::cout << "  Hardware accel: " << (backend->supportsHardwareAccel() ? "Yes" : "No") << std::endl;
        std::cout << "  Vector graphics: " << (backend->supportsVectorGraphics() ? "Yes" : "No") << std::endl;
    }
    
    // Try to load a SWF file if provided
    if (argc > 1) {
        std::string filename = argv[1];
        std::cout << "\nLoading SWF: " << filename << std::endl;
        
        if (renderer->loadDocument(filename)) {
            std::cout << "  Loaded successfully!" << std::endl;
            std::cout << "  Total frames: " << renderer->getTotalFrames() << std::endl;
            
            // Render first frame
            std::cout << "\nRendering frame 0..." << std::endl;
            if (renderer->renderFrame(0)) {
                std::cout << "  Rendered successfully!" << std::endl;
                
                // Get stats
                auto stats = renderer->getStats();
                std::cout << "  Frame time: " << stats.frameTimeMs << "ms" << std::endl;
                std::cout << "  Draw calls: " << stats.drawCalls << std::endl;
                
                // Save output
                std::string outputFile = "output.ppm";
                if (renderer->getOutputTarget() && renderer->getOutputTarget()->saveToFile(outputFile)) {
                    std::cout << "  Saved to: " << outputFile << std::endl;
                }
            } else {
                std::cerr << "  Render failed: " << renderer->getLastError() << std::endl;
            }
        } else {
            std::cerr << "  Failed to load: " << renderer->getLastError() << std::endl;
        }
    } else {
        std::cout << "\nNo SWF file provided. Usage: " << argv[0] << " <swf_file>" << std::endl;
    }
    
    // Test animation controls
    std::cout << "\nAnimation controls test:" << std::endl;
    std::cout << "  Initial state: " << (renderer->isPlaying() ? "Playing" : "Stopped") << std::endl;
    
    renderer->play();
    std::cout << "  After play(): " << (renderer->isPlaying() ? "Playing" : "Stopped") << std::endl;
    
    renderer->stop();
    std::cout << "  After stop(): " << (renderer->isPlaying() ? "Playing" : "Stopped") << std::endl;
    
    std::cout << "\n========================================" << std::endl;
    std::cout << "Demo completed successfully!" << std::endl;
    std::cout << "========================================" << std::endl;
    
    return 0;
}
