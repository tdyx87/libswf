// Copyright (c) 2025 libswf authors. All rights reserved.
// Software Render Backend Implementation

#ifndef LIBSWF_RENDERER_SOFTWARE_BACKEND_H
#define LIBSWF_RENDERER_SOFTWARE_BACKEND_H

#include "render_backend.h"
#include <vector>
#include <stack>

namespace libswf {

// Software render target - simple RGBA buffer
class SoftwareRenderTarget : public IRenderTarget {
public:
    SoftwareRenderTarget();
    ~SoftwareRenderTarget() override = default;
    
    bool initialize(uint32_t width, uint32_t height) override;
    uint32_t getWidth() const override { return width_; }
    uint32_t getHeight() const override { return height_; }
    void clear(const RGBA& color) override;
    uint8_t* lock() override { return pixels_.data(); }
    void unlock() override {}
    bool saveToFile(const std::string& filename) const override;
    bool saveToPPM(const std::string& filename) const;
    uint8_t* getPixels() override { return pixels_.data(); }
    const uint8_t* getPixels() const override { return pixels_.data(); }
    size_t getPitch() const override { return width_ * 4; }

private:
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    std::vector<uint8_t> pixels_; // RGBA format
};

// Software draw context - CPU-based rasterization
class SoftwareDrawContext : public IDrawContext {
public:
    explicit SoftwareDrawContext(SoftwareRenderTarget* target);
    ~SoftwareDrawContext() override = default;
    
    // State
    void saveState() override;
    void restoreState() override;
    
    // Transform
    void setTransform(const Matrix& matrix) override;
    void multiplyTransform(const Matrix& matrix) override;
    
    // Clipping
    void setClipRect(const Rect& rect) override;
    void clearClip() override;
    
    // Primitives
    void drawRect(const Rect& rect, const RGBA& color) override;
    void fillRect(const Rect& rect, const RGBA& color) override;
    void drawLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1, 
                  const RGBA& color, uint16_t width = 1) override;
    
    // Paths
    void beginPath() override;
    void moveTo(float x, float y) override;
    void lineTo(float x, float y) override;
    void curveTo(float cx, float cy, float x, float y) override;
    void closePath() override;
    void fillPath(const RGBA& color) override;
    void strokePath(const RGBA& color, float width) override;
    
    // Fill styles
    void setFillColor(const RGBA& color) override;
    void setFillGradient(const std::vector<GradientStop>& records,
                         const Matrix& matrix, bool radial) override;
    void setFillBitmap(uint16_t bitmapId, const Matrix& matrix,
                       bool repeat, bool smooth) override;
    
    // Text
    void drawText(const std::string& text, float x, float y, const RGBA& color) override;

private:
    SoftwareRenderTarget* target_ = nullptr;
    
    // Current state
    struct State {
        Matrix transform;
        Rect clipRect;
        bool hasClip = false;
        RGBA fillColor;
    };
    std::stack<State> stateStack_;
    State currentState_;
    
    // Path building
    struct PathPoint {
        float x, y;
        bool isCurve; // if true, this is a control point
    };
    std::vector<PathPoint> currentPath_;
    bool pathInProgress_ = false;
    
    // Rasterization helpers
    void setPixel(int x, int y, const RGBA& color);
    void setPixelSafe(int x, int y, const RGBA& color);
    void drawLineBresenham(int x0, int y0, int x1, int y1, const RGBA& color);
    void fillScanline(int y, int x1, int x2, const RGBA& color);
    void fillPolygonScanline(const std::vector<std::pair<int32_t, int32_t>>& points, const RGBA& color);
    
    // Transform point
    void transformPoint(float& x, float& y) const;
    
    // Clip test
    bool clipTest(int x, int y) const;
    Rect getEffectiveClip() const;
    
    // Edge structures for scanline filling
    struct Edge {
        int yMax;
        float x;
        float dx;
    };
};

// Software backend implementation
class SoftwareRenderBackend : public IRenderBackend {
public:
    bool supportsHardwareAccel() const override { return false; }
    bool supportsVectorGraphics() const override { return true; }
    std::string getName() const override { return "Software"; }
    
    std::unique_ptr<IRenderTarget> createRenderTarget() override;
    std::unique_ptr<IDrawContext> createDrawContext(IRenderTarget* target) override;
    
    bool initialize() override;
    void shutdown() override;
};

} // namespace libswf

#endif // LIBSWF_RENDERER_SOFTWARE_BACKEND_H
