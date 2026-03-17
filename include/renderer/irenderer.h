// Copyright (c) 2025 libswf authors. All rights reserved.
// Abstract Renderer Interface

#ifndef LIBSWF_RENDERER_IRENDERER_H
#define LIBSWF_RENDERER_IRENDERER_H

#include <common/types.h>
#include <parser/swftag.h>
#include <memory>
#include <vector>
#include <string>
#include <functional>

namespace libswf {

// Forward declarations
class SWFDocument;
class IRenderTarget;
class IDrawContext;
class IRenderBackend;
class DisplayObject;
struct DisplayListItem;

// Render statistics
struct RenderStats {
    uint32_t shapesRendered = 0;
    uint32_t bitmapsRendered = 0;
    uint32_t textsRendered = 0;
    uint32_t drawCalls = 0;
    float frameTimeMs = 0.0f;
};

// Render configuration
struct RenderConfig {
    uint32_t width = 800;
    uint32_t height = 600;
    float frameRate = 24.0f;
    bool enableAA = true;
    bool enableCache = true;
    uint32_t maxCacheSize = 256 * 1024 * 1024; // 256MB
    
    enum class ScaleMode {
        SHOW_ALL,      // Show entire content with letterbox
        EXACT_FIT,     // Stretch to fill
        NO_BORDER,     // Crop to fill
        NO_SCALE       // 1:1 pixel mapping
    } scaleMode = ScaleMode::SHOW_ALL;
};

// Abstract renderer interface
class IRenderer {
public:
    virtual ~IRenderer() = default;
    
    // ========== Document Management ==========
    
    // Load SWF document
    virtual bool loadDocument(const std::string& filename) = 0;
    virtual bool loadDocument(const std::vector<uint8_t>& data) = 0;
    virtual bool loadDocument(std::unique_ptr<SWFDocument> document) = 0;
    
    // Get loaded document
    virtual SWFDocument* getDocument() const = 0;
    
    // Unload document
    virtual void unloadDocument() = 0;
    
    // ========== Rendering ==========
    
    // Initialize renderer with config
    virtual bool initialize(const RenderConfig& config) = 0;
    
    // Render current frame to target
    virtual bool renderFrame() = 0;
    
    // Render specific frame
    virtual bool renderFrame(uint16_t frameNumber) = 0;
    
    // Get render output
    virtual IRenderTarget* getOutputTarget() = 0;
    virtual const IRenderTarget* getOutputTarget() const = 0;
    
    // ========== Animation Control ==========
    
    // Playback control
    virtual void play() = 0;
    virtual void stop() = 0;
    virtual bool isPlaying() const = 0;
    
    // Frame navigation
    virtual void gotoFrame(uint16_t frameNumber) = 0;
    virtual uint16_t getCurrentFrame() const = 0;
    virtual uint16_t getTotalFrames() const = 0;
    
    // Advance to next frame (manual stepping)
    virtual void advanceFrame() = 0;
    
    // Update animation (call periodically)
    virtual void update(float deltaTimeMs) = 0;
    
    // ========== Input Handling ==========
    
    // Mouse events
    virtual void handleMouseMove(int x, int y) = 0;
    virtual void handleMouseDown(int x, int y, int button = 0) = 0;
    virtual void handleMouseUp(int x, int y, int button = 0) = 0;
    
    // Keyboard events
    virtual void handleKeyDown(int keyCode) = 0;
    virtual void handleKeyUp(int keyCode) = 0;
    
    // ========== Configuration ==========
    
    // Get/set configuration
    virtual RenderConfig getConfig() const = 0;
    virtual void setConfig(const RenderConfig& config) = 0;
    
    // Resize output
    virtual void resize(uint32_t width, uint32_t height) = 0;
    
    // ========== Statistics ==========
    
    // Get render statistics
    virtual RenderStats getStats() const = 0;
    virtual void resetStats() = 0;
    
    // ========== Error Handling ==========
    
    // Get last error
    virtual std::string getLastError() const = 0;
    virtual void clearError() = 0;
    
    // ========== Extension Points ==========
    
    // Register custom display object handler
    using DisplayObjectFactory = std::function<std::shared_ptr<DisplayObject>(uint16_t characterId)>;
    virtual void registerDisplayObjectFactory(uint16_t typeId, DisplayObjectFactory factory) = 0;
    
    // Get backend
    virtual IRenderBackend* getBackend() = 0;
};

// Renderer factory
class RendererFactory {
public:
    // Create default renderer (auto-selects best backend)
    static std::unique_ptr<IRenderer> createDefault();
    
    // Create renderer with specific backend
    static std::unique_ptr<IRenderer> createWithBackend(const std::string& backendName);
    
    // Create software renderer (CPU-only, cross-platform)
    static std::unique_ptr<IRenderer> createSoftwareRenderer();
    
    // Create Cairo renderer (if available)
    static std::unique_ptr<IRenderer> createCairoRenderer();
    
    // Create GDI+ renderer (Windows only, if available)
    static std::unique_ptr<IRenderer> createGdiplusRenderer();
    
    // Get available renderer types
    static std::vector<std::string> getAvailableRenderers();
};

} // namespace libswf

#endif // LIBSWF_RENDERER_IRENDERER_H
