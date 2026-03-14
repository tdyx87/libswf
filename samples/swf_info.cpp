#include <iostream>
#include <string>
#include <map>
#include "parser/swfparser.h"

using namespace libswf;

void printUsage(const char* progName) {
    std::cout << "Usage: " << progName << " <swf_file>" << std::endl;
    std::cout << std::endl;
    std::cout << "Displays information about an SWF file." << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }
    
    std::string filename = argv[1];
    
    std::cout << "Parsing SWF file: " << filename << std::endl;
    std::cout << "========================================" << std::endl;
    
    SWFParser parser;
    
    if (!parser.parseFile(filename)) {
        std::cerr << "Error: " << parser.getError() << std::endl;
        return 1;
    }
    
    SWFDocument& doc = parser.getDocument();
    
    // Print header info
    std::cout << std::endl << "Header:" << std::endl;
    std::cout << "  Signature:  " << doc.header.signature << std::endl;
    std::cout << "  Version:    " << (int)doc.header.version << std::endl;
    std::cout << "  File Size:  " << doc.header.fileLength << " bytes" << std::endl;
    std::cout << "  Frame Size: " << doc.header.frameSize.toString() << " (TWIPS)" << std::endl;
    std::cout << "              " << doc.header.frameSize.width() << " x " 
              << doc.header.frameSize.height() << " (pixels)" << std::endl;
    std::cout << "  Frame Rate: " << doc.header.frameRate << " fps" << std::endl;
    std::cout << "  Frame Count: " << doc.header.frameCount << std::endl;
    
    // Print background color
    if (doc.hasBackgroundColor) {
        std::cout << std::endl << "Background Color: #" 
                  << std::hex 
                  << (int)doc.backgroundColor.r 
                  << (int)doc.backgroundColor.g 
                  << (int)doc.backgroundColor.b 
                  << std::dec << std::endl;
    }
    
    // Print tags
    std::cout << std::endl << "Tags: " << doc.tags.size() << std::endl;
    
    // Count by type
    std::map<uint16_t, int> tagCounts;
    for (auto& tag : doc.tags) {
        tagCounts[tag.code()]++;
    }
    
    for (auto& [code, count] : tagCounts) {
        std::cout << "  " << code << ": " << count << std::endl;
    }
    
    // Print characters
    std::cout << std::endl << "Characters: " << doc.characters.size() << std::endl;
    for (auto& [id, tag] : doc.characters) {
        std::cout << "  ID " << id << ": " << (int)tag->type << std::endl;
    }
    
    // Print ABC files (ActionScript 3)
    if (!doc.abcFiles.empty()) {
        std::cout << std::endl << "ABC Files (AS3): " << doc.abcFiles.size() << std::endl;
        for (size_t i = 0; i < doc.abcFiles.size(); i++) {
            std::cout << "  ABC " << i << ": " << doc.abcFiles[i]->abcData.size() << " bytes";
            if (!doc.abcFiles[i]->name.empty()) {
                std::cout << " - " << doc.abcFiles[i]->name;
            }
            std::cout << std::endl;
        }
    }
    
    // Print exports
    if (!doc.exports.empty()) {
        std::cout << std::endl << "Exported Assets:" << std::endl;
        for (auto& exp : doc.exports) {
            std::cout << "  " << exp.name << " (ID: " << exp.characterId << ")" << std::endl;
        }
    }
    
    // Print symbol class
    if (!doc.symbolClass.empty()) {
        std::cout << std::endl << "Symbol Classes:" << std::endl;
        for (auto& [id, name] : doc.symbolClass) {
            std::cout << "  ID " << id << ": " << name << std::endl;
        }
    }
    
    // Print video streams
    int videoCount = 0;
    for (auto& tag : doc.tags) {
        if (auto* videoTag = tag.as<VideoStreamTag>()) {
            if (videoCount == 0) {
                std::cout << std::endl << "Video Streams:" << std::endl;
            }
            std::cout << "  Stream ID " << videoTag->characterId 
                      << ": " << videoTag->width << "x" << videoTag->height
                      << ", " << videoTag->numFrames << " frames"
                      << ", Codec " << (int)videoTag->codec << std::endl;
            videoCount++;
        }
    }
    
    std::cout << std::endl << "Parsing complete!" << std::endl;
    
    return 0;
}
