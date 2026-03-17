// Copyright (c) 2025 libswf authors. All rights reserved.
// GDI+ Render Backend Implementation (Windows only)

#ifndef LIBSWF_RENDERER_GDIPLUS_BACKEND_H
#define LIBSWF_RENDERER_GDIPLUS_BACKEND_H

#include "render_backend.h"

// Define ULONG_PTR for non-Windows platforms
#ifndef _WIN32
typedef unsigned long ULONG_PTR;
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#ifdef RENDERER_GDIPLUS
#include <gdiplus.h>
#endif
#endif

namespace libswf {

#ifdef _WIN32
#ifdef RENDERER_GDIPLUS

// GDI+ render target
class GdiplusRenderTarget : public IRenderTarget {
public:
    GdiplusRenderTarget();
    ~GdiplusRenderTarget() override;
    
    bool initialize(uint32_t width, uint32_t height) override;
    uint32_t getWidth() const override { return width_; }
    uint32_t getHeight() const override { return height_; }
    void clear(const RGBA& color) override;
    uint8_t* lock() override;
    void unlock() override;
    bool saveToFile(const std::string& filename) const override;
    uint8_t* getPixels() override;
    const uint8_t* getPixels() const override;
    size_t getPitch() const override { return width_ * 4; }
    
    // GDI+ specific
    Gdiplus::Bitmap* getBitmap() { return bitmap_.get(); }
    Gdiplus::Graphics* getGraphics() { return graphics_.get(); }
    
    // Render to HDC
    void renderToHDC(HDC hdc, int x, int y, int width, int height);

private:
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    std::unique_ptr<Gdiplus::Bitmap> bitmap_;
    std::unique_ptr<Gdiplus::Graphics> graphics_;
    std::vector<uint8_t> lockedPixels_;
    bool isLocked_ = false;
};

// GDI+ draw context
class GdiplusDrawContext : public IDrawContext {
public:
    explicit GdiplusDrawContext(GdiplusRenderTarget* target);
    ~GdiplusDrawContext() override = default;
    
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
    GdiplusRenderTarget* target_ = nullptr;
    Gdiplus::Graphics* graphics_ = nullptr;
    
    // Current path
    std::unique_ptr<Gdiplus::GraphicsPath> currentPath_;
    Gdiplus::PointF pathStart_;
    Gdiplus::PointF pathCurrent_;
    bool pathInProgress_ = false;
    
    // State stack for save/restore
    struct State {
        Gdiplus::Matrix transform;
        Gdiplus::Region clip;
        bool hasClip = false;
    };
    std::vector<State> stateStack_;
    
    // Helpers
    void applyMatrix(const Matrix& matrix);
    void applyColor(const RGBA& color);
    Gdiplus::Color toGdiplusColor(const RGBA& rgba);
    Gdiplus::Matrix toGdiplusMatrix(const Matrix& matrix);
};

#endif // RENDERER_GDIPLUS
#endif // _WIN32

// GDI+ backend (stub if GDI+ not available)
class GdiplusRenderBackend : public IRenderBackend {
public:
    bool supportsHardwareAccel() const override;
    bool supportsVectorGraphics() const override;
    std::string getName() const override;
    
    std::unique_ptr<IRenderTarget> createRenderTarget() override;
    std::unique_ptr<IDrawContext> createDrawContext(IRenderTarget* target) override;
    
    bool initialize() override;
    void shutdown() override;
    
    // GDI+ specific: token for GDI+ startup/shutdown
    static ULONG_PTR getGdiplusToken() { return gdiplusToken_; }

private:
    static ULONG_PTR gdiplusToken_;
    static bool initialized_;
};

} // namespace libswf

#endif // LIBSWF_RENDERER_GDIPLUS_BACKEND_H
