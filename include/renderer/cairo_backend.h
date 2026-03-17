// Copyright (c) 2025 libswf authors. All rights reserved.
// Cairo Render Backend Implementation

#ifndef LIBSWF_RENDERER_CAIRO_BACKEND_H
#define LIBSWF_RENDERER_CAIRO_BACKEND_H

#include "render_backend.h"

#ifdef RENDERER_CAIRO
#include <cairo.h>
#endif

namespace libswf {

#ifdef RENDERER_CAIRO

// Cairo render target
class CairoRenderTarget : public IRenderTarget {
public:
    CairoRenderTarget();
    ~CairoRenderTarget() override;
    
    bool initialize(uint32_t width, uint32_t height) override;
    uint32_t getWidth() const override { return width_; }
    uint32_t getHeight() const override { return height_; }
    void clear(const RGBA& color) override;
    uint8_t* lock() override { return surfaceData_; }
    void unlock() override {}
    bool saveToFile(const std::string& filename) const override;
    uint8_t* getPixels() override { return surfaceData_; }
    const uint8_t* getPixels() const override { return surfaceData_; }
    size_t getPitch() const override { return cairo_image_surface_get_stride(surface_); }
    
    // Cairo-specific
    cairo_surface_t* getSurface() { return surface_; }
    cairo_t* getContext() { return cr_; }

private:
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    cairo_surface_t* surface_ = nullptr;
    cairo_t* cr_ = nullptr;
    uint8_t* surfaceData_ = nullptr;
};

// Cairo draw context
class CairoDrawContext : public IDrawContext {
public:
    explicit CairoDrawContext(CairoRenderTarget* target);
    ~CairoDrawContext() override = default;
    
    void saveState() override;
    void restoreState() override;
    
    void setTransform(const Matrix& matrix) override;
    void multiplyTransform(const Matrix& matrix) override;
    
    void setClipRect(const Rect& rect) override;
    void clearClip() override;
    
    void drawRect(const Rect& rect, const RGBA& color) override;
    void fillRect(const Rect& rect, const RGBA& color) override;
    void drawLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1, 
                  const RGBA& color, uint16_t width = 1) override;
    
    void beginPath() override;
    void moveTo(float x, float y) override;
    void lineTo(float x, float y) override;
    void curveTo(float cx, float cy, float x, float y) override;
    void closePath() override;
    void fillPath(const RGBA& color) override;
    void strokePath(const RGBA& color, float width) override;
    
    void setFillColor(const RGBA& color) override;
    void setFillGradient(const std::vector<GradientStop>& records,
                         const Matrix& matrix, bool radial) override;
    void setFillBitmap(uint16_t bitmapId, const Matrix& matrix,
                       bool repeat, bool smooth) override;
    
    void drawText(const std::string& text, float x, float y, const RGBA& color) override;

private:
    CairoRenderTarget* target_ = nullptr;
    cairo_t* cr_ = nullptr;
    
    void applyColor(const RGBA& color);
    void applyMatrix(const Matrix& matrix);
    void convertMatrix(const Matrix& m, cairo_matrix_t& cm);
};

#endif // RENDERER_CAIRO

// Cairo backend (stub if Cairo not available)
class CairoRenderBackend : public IRenderBackend {
public:
    bool supportsHardwareAccel() const override;
    bool supportsVectorGraphics() const override;
    std::string getName() const override;
    
    std::unique_ptr<IRenderTarget> createRenderTarget() override;
    std::unique_ptr<IDrawContext> createDrawContext(IRenderTarget* target) override;
    
    bool initialize() override;
    void shutdown() override;
};

} // namespace libswf

#endif // LIBSWF_RENDERER_CAIRO_BACKEND_H
