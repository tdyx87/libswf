#include "writer/swfwriter.h"
#include "parser/swfparser.h"
#include <iostream>

using namespace libswf;

int main() {
    std::cout << "SWF Writer Test" << std::endl;
    std::cout << "===============" << std::endl;
    
    // Create a simple SWF with a rectangle
    SWFWriter writer;
    
    // Set properties
    writer.setFrameRate(24.0f);
    writer.setFrameCount(1);
    writer.setBackgroundColor(RGBA(255, 255, 200, 255));  // Light yellow
    
    // Create a rectangle shape
    auto rectRecords = ShapeBuilder::createRect(50, 50, 100, 80);
    std::vector<FillStyle> fills;
    std::vector<LineStyle> lines;
    
    uint16 shapeId = writer.defineShape(rectRecords, fills, lines);
    std::cout << "Created shape with ID: " << shapeId << std::endl;
    
    // Place the shape on stage
    writer.placeObject(1, shapeId);
    
    // Show frame
    writer.showFrame();
    
    // Export symbol
    writer.exportSymbol(shapeId, "MyRectangle");
    
    // Save to file
    std::string filename = "test_created.swf";
    if (writer.saveToFile(filename, 10)) {
        std::cout << "Saved SWF to: " << filename << std::endl;
    } else {
        std::cerr << "Failed to save: " << writer.getError() << std::endl;
        return 1;
    }
    
    // Try to load and verify
    std::cout << std::endl << "Verifying created SWF..." << std::endl;
    
    SWFParser parser;
    if (parser.parseFile(filename)) {
        std::cout << "SWF parsed successfully!" << std::endl;
        std::cout << "  Version: " << parser.getDocument().header.version << std::endl;
        std::cout << "  Compressed: " << (parser.getDocument().header.compressed ? "Yes" : "No") << std::endl;
        std::cout << "  Frame rate: " << parser.getDocument().header.frameRate << std::endl;
        std::cout << "  Frame count: " << parser.getDocument().header.frameCount << std::endl;
        std::cout << "  Tags: " << parser.getDocument().tags.size() << std::endl;
        std::cout << "  Characters: " << parser.getDocument().characters.size() << std::endl;
        std::cout << "  Exports: " << parser.getDocument().exports.size() << std::endl;
        
        for (const auto& exp : parser.getDocument().exports) {
            std::cout << "    Export: " << exp.name << " (ID: " << exp.characterId << ")" << std::endl;
        }
    } else {
        std::cerr << "Failed to parse: " << parser.getError() << std::endl;
        return 1;
    }
    
    std::cout << std::endl << "SWF writer test completed!" << std::endl;
    return 0;
}
