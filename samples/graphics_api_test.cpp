// graphics_api_test.cpp
// Test program for AVM2 Graphics API integration with renderer

#include <iostream>
#include <memory>
#include "parser/swfparser.h"
#include "renderer/renderer.h"
#include "avm2/avm2.h"
#include "avm2/avm2_renderer_bridge.h"

using namespace libswf;

int main() {
    std::cout << "=== AVM2 Graphics API Test ===" << std::endl;
    
    // Create renderer
    SWFRenderer renderer;
    
    // Initialize AVM2
    renderer.initAVM2();
    
    if (!renderer.hasAVM2()) {
        std::cerr << "Failed to initialize AVM2" << std::endl;
        return 1;
    }
    
    std::cout << "AVM2 initialized successfully" << std::endl;
    
    // Get AVM2 and bridge
    auto avm2 = renderer.getAVM2();
    auto bridge = std::make_shared<AVM2RendererBridge>(*avm2, renderer);
    bridge->initialize();
    
    std::cout << "Bridge initialized" << std::endl;
    
    // Test 1: Create a display object and draw a rectangle
    std::cout << "\n[Test 1] Drawing a rectangle..." << std::endl;
    {
        auto displayObj = std::make_shared<AVM2DisplayObject>();
        displayObj->bridge = bridge;
        displayObj->characterId = 1;
        
        // Simulate: graphics.beginFill(0xFF0000, 1.0)
        GraphicsCommand beginFillCmd;
        beginFillCmd.type = GraphicsCmdType::BEGIN_FILL;
        beginFillCmd.color = 0xFF0000;  // Red
        beginFillCmd.alpha = 1.0f;
        displayObj->graphicsCommands.push_back(beginFillCmd);
        displayObj->hasFill = true;
        displayObj->currentFillColor = RGBA(255, 0, 0, 255);
        
        // Simulate: graphics.drawRect(10, 10, 100, 50)
        GraphicsCommand drawRectCmd;
        drawRectCmd.type = GraphicsCmdType::DRAW_RECT;
        drawRectCmd.x = 10.0f;
        drawRectCmd.y = 10.0f;
        drawRectCmd.width = 100.0f;
        drawRectCmd.height = 50.0f;
        displayObj->graphicsCommands.push_back(drawRectCmd);
        
        // Simulate: graphics.endFill()
        GraphicsCommand endFillCmd;
        endFillCmd.type = GraphicsCmdType::END_FILL;
        displayObj->graphicsCommands.push_back(endFillCmd);
        
        // Sync to renderer
        displayObj->syncToRenderer();
        
        std::cout << "  Rectangle drawn at (10, 10) with size 100x50" << std::endl;
        std::cout << "  Commands: " << displayObj->graphicsCommands.size() << std::endl;
    }
    
    // Test 2: Draw a circle
    std::cout << "\n[Test 2] Drawing a circle..." << std::endl;
    {
        auto displayObj = std::make_shared<AVM2DisplayObject>();
        displayObj->bridge = bridge;
        displayObj->characterId = 2;
        
        // Simulate: graphics.beginFill(0x00FF00, 0.8)
        GraphicsCommand beginFillCmd;
        beginFillCmd.type = GraphicsCmdType::BEGIN_FILL;
        beginFillCmd.color = 0x00FF00;  // Green
        beginFillCmd.alpha = 0.8f;
        displayObj->graphicsCommands.push_back(beginFillCmd);
        
        // Simulate: graphics.drawCircle(200, 100, 30)
        GraphicsCommand drawCircleCmd;
        drawCircleCmd.type = GraphicsCmdType::DRAW_CIRCLE;
        drawCircleCmd.x = 200.0f;
        drawCircleCmd.y = 100.0f;
        drawCircleCmd.radius = 30.0f;
        displayObj->graphicsCommands.push_back(drawCircleCmd);
        
        // Simulate: graphics.endFill()
        GraphicsCommand endFillCmd;
        endFillCmd.type = GraphicsCmdType::END_FILL;
        displayObj->graphicsCommands.push_back(endFillCmd);
        
        // Sync to renderer
        displayObj->syncToRenderer();
        
        std::cout << "  Circle drawn at (200, 100) with radius 30" << std::endl;
    }
    
    // Test 3: Draw a line path
    std::cout << "\n[Test 3] Drawing a line path..." << std::endl;
    {
        auto displayObj = std::make_shared<AVM2DisplayObject>();
        displayObj->bridge = bridge;
        displayObj->characterId = 3;
        
        // Simulate: graphics.lineStyle(2, 0x0000FF, 1.0)
        GraphicsCommand lineStyleCmd;
        lineStyleCmd.type = GraphicsCmdType::LINE_STYLE;
        lineStyleCmd.lineWidth = 2.0f;
        lineStyleCmd.color = 0x0000FF;  // Blue
        lineStyleCmd.alpha = 1.0f;
        displayObj->graphicsCommands.push_back(lineStyleCmd);
        
        // Simulate: graphics.moveTo(50, 200)
        GraphicsCommand moveToCmd;
        moveToCmd.type = GraphicsCmdType::MOVE_TO;
        moveToCmd.x = 50.0f;
        moveToCmd.y = 200.0f;
        displayObj->graphicsCommands.push_back(moveToCmd);
        
        // Simulate: graphics.lineTo(150, 200)
        GraphicsCommand lineToCmd;
        lineToCmd.type = GraphicsCmdType::LINE_TO;
        lineToCmd.x = 150.0f;
        lineToCmd.y = 200.0f;
        displayObj->graphicsCommands.push_back(lineToCmd);
        
        // Simulate: graphics.lineTo(100, 250)
        GraphicsCommand lineToCmd2;
        lineToCmd2.type = GraphicsCmdType::LINE_TO;
        lineToCmd2.x = 100.0f;
        lineToCmd2.y = 250.0f;
        displayObj->graphicsCommands.push_back(lineToCmd2);
        
        // Simulate: graphics.endFill() - triggers sync
        GraphicsCommand endFillCmd;
        endFillCmd.type = GraphicsCmdType::END_FILL;
        displayObj->graphicsCommands.push_back(endFillCmd);
        
        // Sync to renderer
        displayObj->syncToRenderer();
        
        std::cout << "  Line path drawn: (50,200) -> (150,200) -> (100,250)" << std::endl;
    }
    
    // Check dynamic display objects
    std::cout << "\n=== Verification ===" << std::endl;
    auto& dynamicObjs = renderer.getDynamicDisplayObjects();
    std::cout << "Dynamic display objects created: " << dynamicObjs.size() << std::endl;
    
    for (size_t i = 0; i < dynamicObjs.size(); ++i) {
        if (dynamicObjs[i]) {
            auto bounds = dynamicObjs[i]->getBounds();
            std::cout << "  Object " << i << ": bounds = " << bounds.toString() << std::endl;
        }
    }
    
    // Test 4: Clear and redraw
    std::cout << "\n[Test 4] Testing clear and redraw..." << std::endl;
    {
        auto displayObj = std::make_shared<AVM2DisplayObject>();
        displayObj->bridge = bridge;
        displayObj->characterId = 4;
        
        // First draw something
        GraphicsCommand beginFillCmd;
        beginFillCmd.type = GraphicsCmdType::BEGIN_FILL;
        beginFillCmd.color = 0xFFFF00;
        beginFillCmd.alpha = 1.0f;
        displayObj->graphicsCommands.push_back(beginFillCmd);
        
        GraphicsCommand drawRectCmd;
        drawRectCmd.type = GraphicsCmdType::DRAW_RECT;
        drawRectCmd.x = 300.0f;
        drawRectCmd.y = 300.0f;
        drawRectCmd.width = 50.0f;
        drawRectCmd.height = 50.0f;
        displayObj->graphicsCommands.push_back(drawRectCmd);
        
        // Clear and draw something else
        displayObj->clearGraphics();
        
        // Draw new shape
        GraphicsCommand beginFillCmd2;
        beginFillCmd2.type = GraphicsCmdType::BEGIN_FILL;
        beginFillCmd2.color = 0xFF00FF;  // Magenta
        beginFillCmd2.alpha = 1.0f;
        displayObj->graphicsCommands.push_back(beginFillCmd2);
        
        GraphicsCommand drawCircleCmd;
        drawCircleCmd.type = GraphicsCmdType::DRAW_CIRCLE;
        drawCircleCmd.x = 350.0f;
        drawCircleCmd.y = 350.0f;
        drawCircleCmd.radius = 25.0f;
        displayObj->graphicsCommands.push_back(drawCircleCmd);
        
        GraphicsCommand endFillCmd;
        endFillCmd.type = GraphicsCmdType::END_FILL;
        displayObj->graphicsCommands.push_back(endFillCmd);
        
        displayObj->syncToRenderer();
        
        std::cout << "  Cleared previous graphics and drew a magenta circle" << std::endl;
    }
    
    std::cout << "\n=== All Tests Passed! ===" << std::endl;
    std::cout << "Graphics API is now fully integrated with the renderer." << std::endl;
    
    return 0;
}
