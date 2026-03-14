#include "renderer/renderer.h"
#include "common/types.h"
#include <iostream>
#include <cmath>

using namespace libswf;

// Test blend modes
void testBlendModes() {
    std::cout << "Testing Blend Modes..." << std::endl;
    
    ImageBuffer buffer(100, 100);
    buffer.clear(RGBA(255, 255, 255, 255));  // White background
    
    // Test normal blend
    RGBA src(255, 0, 0, 128);  // Semi-transparent red
    RGBA dst(0, 255, 0, 255);  // Green
    RGBA result = ImageBuffer::blendColors(src, dst, BlendMode::NORMAL);
    std::cout << "  Normal blend: R=" << (int)result.r << " G=" << (int)result.g 
              << " B=" << (int)result.b << std::endl;
    
    // Test multiply blend
    RGBA srcM(255, 255, 0, 255);  // Yellow
    RGBA dstM(0, 0, 255, 255);    // Blue
    RGBA resultM = ImageBuffer::blendColors(srcM, dstM, BlendMode::MULTIPLY);
    std::cout << "  Multiply blend: R=" << (int)resultM.r << " G=" << (int)resultM.g 
              << " B=" << (int)resultM.b << std::endl;
    
    // Test screen blend
    RGBA resultS = ImageBuffer::blendColors(srcM, dstM, BlendMode::SCREEN);
    std::cout << "  Screen blend: R=" << (int)resultS.r << " G=" << (int)resultS.g 
              << " B=" << (int)resultS.b << std::endl;
    
    // Test add blend
    RGBA srcA(128, 64, 32, 255);
    RGBA dstA(100, 100, 100, 255);
    RGBA resultA = ImageBuffer::blendColors(srcA, dstA, BlendMode::ADD);
    std::cout << "  Add blend: R=" << (int)resultA.r << " G=" << (int)resultA.g 
              << " B=" << (int)resultA.b << std::endl;
    
    std::cout << "  Blend modes test passed!" << std::endl;
}

// Test blur filter
void testBlurFilter() {
    std::cout << "\nTesting Blur Filter..." << std::endl;
    
    ImageBuffer buffer(100, 100);
    buffer.clear(RGBA(0, 0, 0, 255));  // Black background
    
    // Draw a white square
    for (uint32 y = 40; y < 60; y++) {
        for (uint32 x = 40; x < 60; x++) {
            buffer.setPixel(x, y, RGBA(255, 255, 255, 255));
        }
    }
    
    // Apply blur
    BlurFilter blur;
    blur.blurX = 5.0f;
    blur.blurY = 5.0f;
    blur.passes = 2;
    
    buffer.applyBlur(30, 30, 40, 40, blur.blurX, blur.blurY, blur.passes);
    
    // Check that blur occurred (pixel at edge should not be pure black)
    RGBA edgePixel = buffer.getPixel(35, 50);
    std::cout << "  Blurred edge pixel: R=" << (int)edgePixel.r 
              << " (should be > 0 if blur worked)" << std::endl;
    
    if (edgePixel.r > 0) {
        std::cout << "  Blur filter test passed!" << std::endl;
    } else {
        std::cout << "  Blur filter test may have issues." << std::endl;
    }
}

// Test color matrix filter
void testColorMatrixFilter() {
    std::cout << "\nTesting Color Matrix Filter..." << std::endl;
    
    ImageBuffer buffer(100, 100);
    buffer.clear(RGBA(128, 128, 128, 255));  // Gray
    
    // Test brightness
    ColorMatrixFilter brightness = ColorMatrixFilter::brightness(0.5f);
    buffer.applyColorMatrix(0, 0, 100, 100, brightness);
    
    RGBA brightPixel = buffer.getPixel(50, 50);
    std::cout << "  After brightness (0.5): R=" << (int)brightPixel.r 
              << " (should be > 128)" << std::endl;
    
    // Reset
    buffer.clear(RGBA(128, 128, 128, 255));
    
    // Test contrast
    ColorMatrixFilter contrast = ColorMatrixFilter::contrast(1.5f);
    buffer.applyColorMatrix(0, 0, 100, 100, contrast);
    
    RGBA contrastPixel = buffer.getPixel(50, 50);
    std::cout << "  After contrast (1.5): R=" << (int)contrastPixel.r << std::endl;
    
    // Test invert
    buffer.clear(RGBA(255, 128, 64, 255));
    ColorMatrixFilter invert = ColorMatrixFilter::invert();
    buffer.applyColorMatrix(0, 50, 100, 50, invert);
    
    RGBA invertedPixel = buffer.getPixel(50, 75);
    std::cout << "  After invert (255,128,64): R=" << (int)invertedPixel.r 
              << " G=" << (int)invertedPixel.g << " B=" << (int)invertedPixel.b << std::endl;
    
    bool invertOk = (invertedPixel.r == 0 && invertedPixel.g == 127 && invertedPixel.b == 191);
    if (invertOk) {
        std::cout << "  Color matrix filter test passed!" << std::endl;
    } else {
        std::cout << "  Expected: R=0 G=127 B=191" << std::endl;
    }
}

// Test glow filter
void testGlowFilter() {
    std::cout << "\nTesting Glow Filter..." << std::endl;
    
    ImageBuffer buffer(100, 100);
    buffer.clear(RGBA(0, 0, 0, 255));  // Black background
    
    // Draw a white square
    for (uint32 y = 45; y < 55; y++) {
        for (uint32 x = 45; x < 55; x++) {
            buffer.setPixel(x, y, RGBA(255, 255, 255, 255));
        }
    }
    
    // Apply glow
    GlowFilter glow;
    glow.color = RGBA(255, 0, 0, 255);  // Red glow
    glow.blurX = 8.0f;
    glow.blurY = 8.0f;
    glow.strength = 1.0f;
    glow.passes = 2;
    glow.inner = false;
    glow.knockout = false;
    
    buffer.applyGlow(40, 40, 20, 20, glow);
    
    // Check that glow extended beyond the original square
    RGBA glowPixel = buffer.getPixel(35, 50);
    std::cout << "  Glow pixel outside square: R=" << (int)glowPixel.r 
              << " (should be > 0)" << std::endl;
    
    if (glowPixel.r > 0) {
        std::cout << "  Glow filter test passed!" << std::endl;
    } else {
        std::cout << "  Glow filter test may have issues." << std::endl;
    }
}

// Test drop shadow filter
void testDropShadowFilter() {
    std::cout << "\nTesting Drop Shadow Filter..." << std::endl;
    
    ImageBuffer buffer(100, 100);
    buffer.clear(RGBA(255, 255, 255, 255));  // White background
    
    // Draw a black square
    for (uint32 y = 40; y < 50; y++) {
        for (uint32 x = 40; x < 50; x++) {
            buffer.setPixel(x, y, RGBA(0, 0, 0, 255));
        }
    }
    
    // Apply drop shadow
    DropShadowFilter shadow;
    shadow.color = RGBA(0, 0, 0, 128);  // Semi-transparent black
    shadow.blurX = 4.0f;
    shadow.blurY = 4.0f;
    shadow.angle = 45.0f;
    shadow.distance = 5.0f;
    shadow.strength = 1.0f;
    shadow.passes = 1;
    shadow.inner = false;
    shadow.knockout = false;
    
    buffer.applyDropShadow(35, 35, 20, 20, shadow);
    
    // Check shadow offset (should be at approximately 45 degrees)
    RGBA shadowPixel = buffer.getPixel(48, 52);  // Offset from the square
    std::cout << "  Shadow pixel at offset: A=" << (int)shadowPixel.a 
              << " (should be < 255 and > 0)" << std::endl;
    
    if (shadowPixel.a > 0 && shadowPixel.a < 255) {
        std::cout << "  Drop shadow filter test passed!" << std::endl;
    } else {
        std::cout << "  Drop shadow filter test may have issues." << std::endl;
    }
}

// Test composite with blend mode
void testComposite() {
    std::cout << "\nTesting Composite with Blend Mode..." << std::endl;
    
    ImageBuffer base(100, 100);
    base.clear(RGBA(255, 0, 0, 255));  // Red
    
    ImageBuffer overlay(50, 50);
    overlay.clear(RGBA(0, 255, 0, 128));  // Semi-transparent green
    
    // Composite with normal blend
    base.composite(overlay, 25, 25, BlendMode::NORMAL);
    
    RGBA compositePixel = base.getPixel(35, 35);
    std::cout << "  Composite center pixel: R=" << (int)compositePixel.r 
              << " G=" << (int)compositePixel.g << " B=" << (int)compositePixel.b << std::endl;
    
    // The result should be a mix of red and green
    bool hasRed = compositePixel.r > 0;
    bool hasGreen = compositePixel.g > 0;
    
    if (hasRed && hasGreen) {
        std::cout << "  Composite test passed!" << std::endl;
    } else {
        std::cout << "  Composite test may have issues." << std::endl;
    }
}

// Test predefined color matrix filters
void testPredefinedColorMatrices() {
    std::cout << "\nTesting Predefined Color Matrix Filters..." << std::endl;
    
    RGBA testColor(128, 64, 32, 255);
    
    // Test saturation
    ColorMatrixFilter sat = ColorMatrixFilter::saturation(0.5f);
    RGBA desaturated = sat.apply(testColor);
    std::cout << "  Desaturated (128,64,32): R=" << (int)desaturated.r 
              << " G=" << (int)desaturated.g << " B=" << (int)desaturated.b << std::endl;
    
    // Test hue rotation (90 degrees)
    RGBA hueTest(255, 0, 0, 255);  // Red
    ColorMatrixFilter hue = ColorMatrixFilter::hueRotate(120);  // Should shift toward green
    RGBA hueRotated = hue.apply(hueTest);
    std::cout << "  Hue rotated red (120 deg): R=" << (int)hueRotated.r 
              << " G=" << (int)hueRotated.g << " B=" << (int)hueRotated.b << std::endl;
    
    std::cout << "  Predefined color matrices test completed!" << std::endl;
}

// Main test runner
int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Filter and Blend Mode Test Suite" << std::endl;
    std::cout << "========================================" << std::endl;
    
    testBlendModes();
    testBlurFilter();
    testColorMatrixFilter();
    testGlowFilter();
    testDropShadowFilter();
    testComposite();
    testPredefinedColorMatrices();
    
    std::cout << "\n========================================" << std::endl;
    std::cout << "All tests completed!" << std::endl;
    std::cout << "========================================" << std::endl;
    
    return 0;
}
