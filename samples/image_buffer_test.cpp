#include "renderer/renderer.h"
#include <iostream>

using namespace libswf;

int main() {
    std::cout << "ImageBuffer Test" << std::endl;
    std::cout << "================" << std::endl;
    
    // Create a 200x200 image buffer
    ImageBuffer img(200, 200);
    
    // Clear with white background
    img.clear(RGBA(255, 255, 255, 255));
    
    // Draw a red rectangle
    img.drawRect(20, 20, 60, 60, RGBA(255, 0, 0, 255));
    
    // Draw a green circle
    img.drawCircle(140, 80, 40, RGBA(0, 255, 0, 255));
    
    // Draw a blue line
    img.drawLine(20, 150, 180, 150, RGBA(0, 0, 255, 255));
    
    // Draw some diagonal lines
    img.drawLine(20, 120, 180, 180, RGBA(0, 0, 0, 255));
    img.drawLine(20, 180, 180, 120, RGBA(0, 0, 0, 255));
    
    // Save to PPM
    if (img.savePPM("test_output.ppm")) {
        std::cout << "Saved test_output.ppm (200x200)" << std::endl;
        std::cout << std::endl;
        std::cout << "Test patterns:" << std::endl;
        std::cout << "  - Red rectangle at (20,20) 60x60" << std::endl;
        std::cout << "  - Green circle at (140,80) radius 40" << std::endl;
        std::cout << "  - Blue horizontal line at y=150" << std::endl;
        std::cout << "  - Black X pattern at bottom" << std::endl;
    } else {
        std::cerr << "Failed to save image" << std::endl;
        return 1;
    }
    
    // Test pixel access
    std::cout << std::endl;
    std::cout << "Pixel tests:" << std::endl;
    RGBA pixel1 = img.getPixel(50, 50);
    std::cout << "  Pixel at (50,50): R=" << (int)pixel1.r 
              << " G=" << (int)pixel1.g 
              << " B=" << (int)pixel1.b 
              << " A=" << (int)pixel1.a << std::endl;
    
    RGBA pixel2 = img.getPixel(140, 80);
    std::cout << "  Pixel at (140,80): R=" << (int)pixel2.r 
              << " G=" << (int)pixel2.g 
              << " B=" << (int)pixel2.b 
              << " A=" << (int)pixel2.a << std::endl;
    
    std::cout << std::endl;
    std::cout << "ImageBuffer test completed successfully!" << std::endl;
    
    return 0;
}
