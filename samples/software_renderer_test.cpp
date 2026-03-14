#include "renderer/renderer.h"
#include "parser/swfparser.h"
#include <iostream>

using namespace libswf;

int main(int argc, char* argv[]) {
    std::cout << "Software Renderer Test" << std::endl;
    std::cout << "======================" << std::endl;
    
    SoftwareRenderer renderer;
    
    // Try to load a SWF file if provided
    if (argc > 1) {
        std::string filename = argv[1];
        std::cout << "Loading: " << filename << std::endl;
        
        if (!renderer.loadSWF(filename)) {
            std::cerr << "Failed to load SWF: " << renderer.getError() << std::endl;
            return 1;
        }
        
        SWFDocument* doc = renderer.getDocument();
        if (!doc) {
            std::cerr << "No document loaded" << std::endl;
            return 1;
        }
        
        std::cout << "SWF loaded successfully!" << std::endl;
        std::cout << "  Version: " << doc->header.version << std::endl;
        std::cout << "  Frame size: " << doc->header.frameSize.width()
                  << "x" << doc->header.frameSize.height() << std::endl;
        std::cout << "  Frame count: " << doc->header.frameCount << std::endl;
        std::cout << "  Frame rate: " << doc->header.frameRate / 256.0f << " fps" << std::endl;
        std::cout << "  Tags: " << doc->tags.size() << std::endl;
        std::cout << "  Characters: " << doc->characters.size() << std::endl;
        
        // Render to image
        int width = 400;
        int height = 400;
        std::cout << std::endl << "Rendering to " << width << "x" << height << "..." << std::endl;
        
        if (!renderer.render(width, height)) {
            std::cerr << "Render failed: " << renderer.getError() << std::endl;
            return 1;
        }
        
        // Save output
        std::string outputFile = "software_render_output.ppm";
        if (renderer.saveToPPM(outputFile)) {
            std::cout << "Saved to: " << outputFile << std::endl;
        } else {
            std::cerr << "Failed to save image" << std::endl;
            return 1;
        }
        
    } else {
        // Demo mode - create a simple test pattern
        std::cout << "No SWF file provided, running in demo mode." << std::endl;
        std::cout << "Usage: " << argv[0] << " <swf_file>" << std::endl;
        std::cout << std::endl;
        
        // Just test the ImageBuffer functionality
        ImageBuffer img(300, 300);
        img.clear(RGBA(240, 240, 240, 255));
        
        // Draw some test patterns
        // Rectangle
        for (int y = 50; y < 150; ++y) {
            for (int x = 50; x < 150; ++x) {
                img.setPixel(x, y, RGBA(255, 100, 100, 255));
            }
        }
        
        // Circle
        img.drawCircle(225, 100, 50, RGBA(100, 255, 100, 255));
        
        // Lines
        img.drawLine(50, 200, 250, 250, RGBA(100, 100, 255, 255));
        img.drawLine(50, 250, 250, 200, RGBA(0, 0, 0, 255));
        
        if (img.savePPM("demo_output.ppm")) {
            std::cout << "Demo pattern saved to: demo_output.ppm" << std::endl;
        }
    }
    
    std::cout << std::endl << "Software renderer test completed!" << std::endl;
    return 0;
}
