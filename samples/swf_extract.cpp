// SWF Resource Extractor Tool
// Extracts images, fonts, sounds, and other resources from SWF files

#include "parser/swfparser.h"
#include "parser/swftag.h"
#include <iostream>
#include <fstream>
#include <cstring>
#include <cstdlib>

#ifdef _WIN32
    // Prevent min/max macro conflicts
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    
    #include <direct.h>
    #include <windows.h>
    #define mkdir(path, mode) _mkdir(path)
#else
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <unistd.h>
#endif

using namespace libswf;

// Utility to create directory
bool createDirectory(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return mkdir(path.c_str(), 0755) == 0;
    }
    return true;
}

// Save binary data to file
bool saveBinaryFile(const std::string& filename, const std::vector<uint8>& data) {
    std::ofstream file(filename, std::ios::binary);
    if (!file) return false;
    file.write(reinterpret_cast<const char*>(data.data()), data.size());
    return file.good();
}

// Save text to file
bool saveTextFile(const std::string& filename, const std::string& content) {
    std::ofstream file(filename);
    if (!file) return false;
    file << content;
    return file.good();
}

// Extract JPEG from SWF
bool extractJPEG(const std::string& outputDir, uint16 charId, 
                 const std::vector<uint8>& jpegData) {
    std::string filename = outputDir + "/image_" + std::to_string(charId) + ".jpg";
    
    std::vector<uint8> fullJpeg;
    
    // Add image data
    fullJpeg.insert(fullJpeg.end(), jpegData.begin(), jpegData.end());
    
    // Ensure JPEG ends properly
    if (fullJpeg.size() < 2 || 
        !(fullJpeg[fullJpeg.size() - 2] == 0xFF && fullJpeg[fullJpeg.size() - 1] == 0xD9)) {
        fullJpeg.push_back(0xFF);
        fullJpeg.push_back(0xD9);
    }
    
    return saveBinaryFile(filename, fullJpeg);
}

// Extract PNG
bool extractPNG(const std::string& outputDir, uint16 charId, 
                const std::vector<uint8>& pngData) {
    std::string filename = outputDir + "/image_" + std::to_string(charId) + ".png";
    return saveBinaryFile(filename, pngData);
}

// Extract GIF
bool extractGIF(const std::string& outputDir, uint16 charId,
                const std::vector<uint8>& gifData) {
    std::string filename = outputDir + "/image_" + std::to_string(charId) + ".gif";
    return saveBinaryFile(filename, gifData);
}

// Extract sound
bool extractSound(const std::string& outputDir, uint16 charId,
                  const SoundTag* sound) {
    std::string ext;
    switch (sound->format) {
        case SoundFormat::UNCOMPRESSED_NATIVE_ENDIAN: ext = ".pcm"; break;
        case SoundFormat::ADPCM: ext = ".adpcm"; break;
        case SoundFormat::MP3: ext = ".mp3"; break;
        case SoundFormat::UNCOMPRESSED_LITTLE_ENDIAN: ext = ".pcm"; break;
        case SoundFormat::NELLYMOSER_16KHZ: ext = ".nelly"; break;
        case SoundFormat::NELLYMOSER_8KHZ: ext = ".nelly"; break;
        case SoundFormat::NELLYMOSER: ext = ".nelly"; break;
        case SoundFormat::SPEEX: ext = ".speex"; break;
        default: ext = ".bin"; break;
    }
    
    std::string filename = outputDir + "/sound_" + std::to_string(charId) + ext;
    return saveBinaryFile(filename, sound->soundData);
}

// Extract font information
std::string extractFontInfo(const FontTag* font) {
    std::string info = "Font Name: " + font->fontName + "\n";
    info += "Font ID: " + std::to_string(font->fontId) + "\n";
    info += "Is Font3 (Unicode): " + std::string(font->isFont3 ? "Yes" : "No") + "\n";
    info += "Has Layout: " + std::string(font->hasLayout ? "Yes" : "No") + "\n";
    info += "Glyph Count: " + std::to_string(font->glyphs.size()) + "\n";
    
    // List glyphs
    info += "\nGlyphs:\n";
    for (size_t i = 0; i < font->codeTable.size() && i < font->glyphs.size(); i++) {
        info += "  Code " + std::to_string(font->codeTable[i]) + 
                " -> Shape " + std::to_string(i) + "\n";
    }
    
    return info;
}

// Main extraction function
int extractResources(const std::string& swfPath, const std::string& outputDir) {
    // Create output directory
    if (!createDirectory(outputDir)) {
        std::cerr << "Failed to create output directory: " << outputDir << std::endl;
        return 1;
    }
    
    // Parse SWF file
    SWFParser parser;
    if (!parser.parseFile(swfPath)) {
        std::cerr << "Failed to parse SWF file: " << swfPath << std::endl;
        return 1;
    }
    
    auto& doc = parser.getDocument();
    
    // Create subdirectories
    std::string imagesDir = outputDir + "/images";
    std::string soundsDir = outputDir + "/sounds";
    std::string fontsDir = outputDir + "/fonts";
    std::string shapesDir = outputDir + "/shapes";
    
    createDirectory(imagesDir);
    createDirectory(soundsDir);
    createDirectory(fontsDir);
    createDirectory(shapesDir);
    
    // Statistics
    int imageCount = 0;
    int soundCount = 0;
    int fontCount = 0;
    int shapeCount = 0;
    
    // Extract resources
    for (const auto& [charId, character] : doc.characters) {
        if (!character) continue;
        
        // Extract images
        if (auto* imageTag = dynamic_cast<ImageTag*>(character.get())) {
            bool extracted = false;
            
            switch (imageTag->format) {
                case ImageTag::Format::JPEG:
                    extracted = extractJPEG(imagesDir, charId, imageTag->imageData);
                    break;
                case ImageTag::Format::LOSSLESS:
                    extracted = extractPNG(imagesDir, charId, imageTag->imageData);
                    break;
                case ImageTag::Format::LOSSLESS_ALPHA:
                    extracted = extractPNG(imagesDir, charId, imageTag->imageData);
                    break;
                default:
                    // Unknown format, save as binary
                    extracted = saveBinaryFile(
                        imagesDir + "/image_" + std::to_string(charId) + ".bin",
                        imageTag->imageData);
                    break;
            }
            
            if (extracted) {
                imageCount++;
                std::cout << "Extracted image " << charId << std::endl;
            }
        }
        
        // Extract sounds
        else if (auto* soundTag = dynamic_cast<SoundTag*>(character.get())) {
            if (extractSound(soundsDir, charId, soundTag)) {
                soundCount++;
                std::cout << "Extracted sound " << charId << std::endl;
            }
        }
        
        // Extract fonts
        else if (auto* fontTag = dynamic_cast<FontTag*>(character.get())) {
            std::string fontInfo = extractFontInfo(fontTag);
            std::string filename = fontsDir + "/font_" + std::to_string(charId) + ".txt";
            if (saveTextFile(filename, fontInfo)) {
                fontCount++;
                std::cout << "Extracted font " << charId << ": " << fontTag->fontName << std::endl;
            }
        }
        
        // Extract shapes (save as text description)
        else if (auto* shapeTag = dynamic_cast<ShapeTag*>(character.get())) {
            std::string shapeInfo = "Shape ID: " + std::to_string(charId) + "\n";
            shapeInfo += "Bounds: " + shapeTag->bounds.toString() + "\n";
            shapeInfo += "Record Count: " + std::to_string(shapeTag->records.size()) + "\n";
            shapeInfo += "Fill Styles: " + std::to_string(shapeTag->fillStyles.size()) + "\n";
            shapeInfo += "Line Styles: " + std::to_string(shapeTag->lineStyles.size()) + "\n";
            
            std::string filename = shapesDir + "/shape_" + std::to_string(charId) + ".txt";
            if (saveTextFile(filename, shapeInfo)) {
                shapeCount++;
                std::cout << "Extracted shape " << charId << std::endl;
            }
        }
    }
    
    // Generate summary report
    std::string report = "SWF Resource Extraction Report\n";
    report += "==============================\n\n";
    report += "Source: " + swfPath + "\n";
    report += "Output: " + outputDir + "\n\n";
    report += "SWF Version: " + std::to_string(doc.header.version) + "\n";
    report += "Frame Size: " + doc.header.frameSize.toString() + "\n";
    report += "Frame Rate: " + std::to_string(doc.header.frameRate) + " fps\n";
    report += "Frame Count: " + std::to_string(doc.header.frameCount) + "\n\n";
    report += "Resources Extracted:\n";
    report += "  Images: " + std::to_string(imageCount) + "\n";
    report += "  Sounds: " + std::to_string(soundCount) + "\n";
    report += "  Fonts: " + std::to_string(fontCount) + "\n";
    report += "  Shapes: " + std::to_string(shapeCount) + "\n";
    report += "  Total: " + std::to_string(imageCount + soundCount + fontCount + shapeCount) + "\n";
    
    saveTextFile(outputDir + "/report.txt", report);
    
    std::cout << "\nExtraction complete!\n";
    std::cout << report << std::endl;
    
    return 0;
}

// Print usage
void printUsage(const char* programName) {
    std::cout << "SWF Resource Extractor\n";
    std::cout << "Usage: " << programName << " <swf_file> [output_directory]\n";
    std::cout << "\nOptions:\n";
    std::cout << "  swf_file         Path to the SWF file to extract\n";
    std::cout << "  output_directory Directory to save extracted resources (default: ./extracted)\n";
    std::cout << "\nExtracted resources:\n";
    std::cout << "  - images/        JPEG, PNG, GIF images\n";
    std::cout << "  - sounds/        Audio files (MP3, PCM, etc.)\n";
    std::cout << "  - fonts/         Font information\n";
    std::cout << "  - shapes/        Shape descriptions\n";
    std::cout << "  - report.txt     Summary report\n";
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }
    
    std::string swfPath = argv[1];
    std::string outputDir = (argc >= 3) ? argv[2] : "./extracted";
    
    return extractResources(swfPath, outputDir);
}
