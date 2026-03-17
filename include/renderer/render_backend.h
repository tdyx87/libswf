// Copyright (c) 2025 libswf authors. All rights reserved.
// Render Backend Abstraction Layer

#ifndef LIBSWF_RENDERER_RENDER_BACKEND_H
#define LIBSWF_RENDERER_RENDER_BACKEND_H

#include <common/types.h>
#include <parser/swftag.h>
#include <memory>
#include <vector>
#include <unordered_map>
#include <functional>
#include <string>

namespace libswf {

// Forward declarations
class ShapeTag;
class TextTag;
class BitmapTag;

// Render target abstraction (bitmap buffer, surface, etc.)
class IRenderTarget {
public:
    virtual ~IRenderTarget() = default;
    
    // Initialize target with dimensions
    virtual bool initialize(uint32_t width, uint32_t height) = 0;
    
    // Get dimensions
    virtual uint32_t getWidth() const = 0;
    virtual uint32_t getHeight() const = 0;
    
    // Clear target
    virtual void clear(const RGBA& color) = 0;
    
    // Lock/Unlock for pixel access
    virtual uint8_t* lock() = 0;
    virtual void unlock() = 0;
    
    // Save to file
    virtual bool saveToFile(const std::string& filename) const = 0;
    
    // Raw pixel access
    virtual uint8_t* getPixels() = 0;
    virtual const uint8_t* getPixels() const = 0;
    virtual size_t getPitch() const = 0;
};

// 2D Drawing context abstraction
class IDrawContext {
public:
    virtual ~IDrawContext() = default;
    
    // State management
    virtual void saveState() = 0;
    virtual void restoreState() = 0;
    
    // Transform
    virtual void setTransform(const Matrix& matrix) = 0;
    virtual void multiplyTransform(const Matrix& matrix) = 0;
    
    // Clipping
    virtual void setClipRect(const Rect& rect) = 0;
    virtual void clearClip() = 0;
    
    // Primitives
    virtual void drawRect(const Rect& rect, const RGBA& color) = 0;
    virtual void fillRect(const Rect& rect, const RGBA& color) = 0;
    virtual void drawLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1, 
                          const RGBA& color, uint16_t width = 1) = 0;
    
    // Paths
    virtual void beginPath() = 0;
    virtual void moveTo(float x, float y) = 0;
    virtual void lineTo(float x, float y) = 0;
    virtual void curveTo(float cx, float cy, float x, float y) = 0;
    virtual void closePath() = 0;
    virtual void fillPath(const RGBA& color) = 0;
    virtual void strokePath(const RGBA& color, float width) = 0;
    
    // Fill styles
    virtual void setFillColor(const RGBA& color) = 0;
    virtual void setFillGradient(const std::vector<GradientStop>& records,
                                  const Matrix& matrix, bool radial) = 0;
    virtual void setFillBitmap(uint16_t bitmapId, const Matrix& matrix,
                               bool repeat, bool smooth) = 0;
    
    // Text
    virtual void drawText(const std::string& text, float x, float y,
                          const RGBA& color) = 0;
};

// Render backend factory
class IRenderBackend {
public:
    virtual ~IRenderBackend() = default;
    
    // Backend capabilities
    virtual bool supportsHardwareAccel() const = 0;
    virtual bool supportsVectorGraphics() const = 0;
    virtual std::string getName() const = 0;
    
    // Create resources
    virtual std::unique_ptr<IRenderTarget> createRenderTarget() = 0;
    virtual std::unique_ptr<IDrawContext> createDrawContext(IRenderTarget* target) = 0;
    
    // Initialize/shutdown
    virtual bool initialize() = 0;
    virtual void shutdown() = 0;
};

// Backend registry
class RenderBackendRegistry {
public:
    static RenderBackendRegistry& instance();
    
    // Register a backend
    void registerBackend(const std::string& name, 
                         std::function<std::unique_ptr<IRenderBackend>()> factory);
    
    // Get available backends
    std::vector<std::string> getAvailableBackends() const;
    
    // Create backend by name
    std::unique_ptr<IRenderBackend> createBackend(const std::string& name);
    
    // Auto-select best backend
    std::unique_ptr<IRenderBackend> createBestBackend();

private:
    std::unordered_map<std::string, std::function<std::unique_ptr<IRenderBackend>()>> factories_;
};

} // namespace libswf

#endif // LIBSWF_RENDERER_RENDER_BACKEND_H
