// Copyright (c) 2025 libswf authors. All rights reserved.
// GDI+ Render Backend Implementation

#include "renderer/gdiplus_backend.h"
#include <algorithm>
#include <cstring>

namespace libswf {

// Static members
ULONG_PTR GdiplusRenderBackend::gdiplusToken_ = 0;
bool GdiplusRenderBackend::initialized_ = false;

#ifdef _WIN32
#ifdef RENDERER_GDIPLUS

// ============== GdiplusRenderTarget ==============

GdiplusRenderTarget::GdiplusRenderTarget() = default;

GdiplusRenderTarget::~GdiplusRenderTarget() {
    graphics_.reset();
    bitmap_.reset();
}

bool GdiplusRenderTarget::initialize(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;
    
    // Create bitmap
    bitmap_ = std::make_unique<Gdiplus::Bitmap>(width, height, PixelFormat32bppARGB);
    if (bitmap_->GetLastStatus() != Gdiplus::Ok) {
        return false;
    }
    
    // Create graphics context
    graphics_ = std::make_unique<Gdiplus::Graphics>(bitmap_.get());
    if (graphics_->GetLastStatus() != Gdiplus::Ok) {
        return false;
    }
    
    // Set high quality rendering
    graphics_->SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics_->SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics_->SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    
    // Clear to white
    clear(RGBA{255, 255, 255, 255});
    
    return true;
}

void GdiplusRenderTarget::clear(const RGBA& color) {
    if (!graphics_) return;
    
    Gdiplus::SolidBrush brush(Gdiplus::Color(color.a, color.r, color.g, color.b));
    graphics_->FillRectangle(&brush, 0, 0, width_, height_);
}

uint8_t* GdiplusRenderTarget::lock() {
    if (!bitmap_ || isLocked_) return nullptr;
    
    Gdiplus::BitmapData bitmapData;
    Gdiplus::Rect rect(0, 0, width_, height_);
    
    if (bitmap_->LockBits(&rect, Gdiplus::ImageLockModeRead | Gdiplus::ImageLockModeWrite,
                          PixelFormat32bppARGB, &bitmapData) != Gdiplus::Ok) {
        return nullptr;
    }
    
    lockedPixels_.resize(width_ * height_ * 4);
    
    // Copy data
    uint8_t* src = static_cast<uint8_t*>(bitmapData.Scan0);
    for (uint32_t y = 0; y < height_; y++) {
        std::memcpy(lockedPixels_.data() + y * width_ * 4,
                    src + y * bitmapData.Stride,
                    width_ * 4);
    }
    
    bitmap_->UnlockBits(&bitmapData);
    isLocked_ = true;
    
    return lockedPixels_.data();
}

void GdiplusRenderTarget::unlock() {
    if (!bitmap_ || !isLocked_) return;
    
    Gdiplus::BitmapData bitmapData;
    Gdiplus::Rect rect(0, 0, width_, height_);
    
    if (bitmap_->LockBits(&rect, Gdiplus::ImageLockModeWrite,
                          PixelFormat32bppARGB, &bitmapData) == Gdiplus::Ok) {
        // Copy data back
        uint8_t* dst = static_cast<uint8_t*>(bitmapData.Scan0);
        for (uint32_t y = 0; y < height_; y++) {
            std::memcpy(dst + y * bitmapData.Stride,
                        lockedPixels_.data() + y * width_ * 4,
                        width_ * 4);
        }
        bitmap_->UnlockBits(&bitmapData);
    }
    
    isLocked_ = false;
    lockedPixels_.clear();
}

bool GdiplusRenderTarget::saveToFile(const std::string& filename) const {
    if (!bitmap_) return false;
    
    // Determine format by extension
    std::wstring wfilename(filename.begin(), filename.end());
    
    CLSID clsid;
    UINT numEncoders = 0;
    UINT size = 0;
    
    // Get encoder for PNG
    Gdiplus::GetImageEncodersSize(&numEncoders, &size);
    if (size == 0) return false;
    
    std::vector<Gdiplus::ImageCodecInfo> encoders(numEncoders);
    Gdiplus::GetImageEncoders(numEncoders, size, encoders.data());
    
    // Find PNG encoder
    bool found = false;
    for (UINT i = 0; i < numEncoders; i++) {
        if (std::wstring(encoders[i].MimeType) == L"image/png") {
            clsid = encoders[i].Clsid;
            found = true;
            break;
        }
    }
    
    if (!found) return false;
    
    return bitmap_->Save(wfilename.c_str(), &clsid, nullptr) == Gdiplus::Ok;
}

uint8_t* GdiplusRenderTarget::getPixels() {
    // Lock and return pointer
    return lock();
}

const uint8_t* GdiplusRenderTarget::getPixels() const {
    // For const version, we need to make a copy
    // This is a limitation - caller should use non-const version
    return nullptr;
}

void GdiplusRenderTarget::renderToHDC(HDC hdc, int x, int y, int width, int height) {
    if (!graphics_) return;
    
    Gdiplus::Graphics destGraphics(hdc);
    destGraphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    
    Gdiplus::Rect destRect(x, y, width, height);
    Gdiplus::Rect srcRect(0, 0, width_, height_);
    
    destGraphics.DrawImage(bitmap_.get(), destRect, srcRect.X, srcRect.Y, 
                           srcRect.Width, srcRect.Height, Gdiplus::UnitPixel);
}

// ============== GdiplusDrawContext ==============

GdiplusDrawContext::GdiplusDrawContext(GdiplusRenderTarget* target)
    : target_(target), graphics_(target->getGraphics()) {
    currentPath_ = std::make_unique<Gdiplus::GraphicsPath>();
}

void GdiplusDrawContext::saveState() {
    if (!graphics_) return;
    
    State state;
    graphics_->GetTransform(&state.transform);
    
    Gdiplus::Region region;
    graphics_->GetClip(&region);
    state.clip = std::move(region);
    state.hasClip = true;
    
    stateStack_.push_back(std::move(state));
}

void GdiplusDrawContext::restoreState() {
    if (!graphics_ || stateStack_.empty()) return;
    
    State state = std::move(stateStack_.back());
    stateStack_.pop_back();
    
    graphics_->SetTransform(&state.transform);
    if (state.hasClip) {
        graphics_->SetClip(&state.clip);
    } else {
        graphics_->ResetClip();
    }
}

void GdiplusDrawContext::setTransform(const Matrix& matrix) {
    applyMatrix(matrix);
}

void GdiplusDrawContext::multiplyTransform(const Matrix& matrix) {
    if (!graphics_) return;
    
    Gdiplus::Matrix current;
    graphics_->GetTransform(&current);
    
    Gdiplus::Matrix newMatrix = toGdiplusMatrix(matrix);
    current.Multiply(&newMatrix, Gdiplus::MatrixOrderPrepend);
    
    graphics_->SetTransform(&current);
}

void GdiplusDrawContext::setClipRect(const Rect& rect) {
    if (!graphics_) return;
    
    Gdiplus::RectF clipRect(
        rect.xMin / 20.0f,
        rect.yMin / 20.0f,
        (rect.xMax - rect.xMin) / 20.0f,
        (rect.yMax - rect.yMin) / 20.0f
    );
    graphics_->SetClip(clipRect);
}

void GdiplusDrawContext::clearClip() {
    if (graphics_) graphics_->ResetClip();
}

void GdiplusDrawContext::drawRect(const Rect& rect, const RGBA& color) {
    if (!graphics_) return;
    
    Gdiplus::Pen pen(toGdiplusColor(color));
    Gdiplus::RectF rc(
        rect.xMin / 20.0f,
        rect.yMin / 20.0f,
        (rect.xMax - rect.xMin) / 20.0f,
        (rect.yMax - rect.yMin) / 20.0f
    );
    graphics_->DrawRectangle(&pen, rc);
}

void GdiplusDrawContext::fillRect(const Rect& rect, const RGBA& color) {
    if (!graphics_) return;
    
    Gdiplus::SolidBrush brush(toGdiplusColor(color));
    Gdiplus::RectF rc(
        rect.xMin / 20.0f,
        rect.yMin / 20.0f,
        (rect.xMax - rect.xMin) / 20.0f,
        (rect.yMax - rect.yMin) / 20.0f
    );
    graphics_->FillRectangle(&brush, rc);
}

void GdiplusDrawContext::drawLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1, 
                                  const RGBA& color, uint16_t width) {
    if (!graphics_) return;
    
    Gdiplus::Pen pen(toGdiplusColor(color), static_cast<Gdiplus::REAL>(width));
    graphics_->DrawLine(&pen, 
                        x0 / 20.0f, y0 / 20.0f,
                        x1 / 20.0f, y1 / 20.0f);
}

void GdiplusDrawContext::beginPath() {
    currentPath_->Reset();
    pathInProgress_ = false;
}

void GdiplusDrawContext::moveTo(float x, float y) {
    pathStart_.X = x / 20.0f;
    pathStart_.Y = y / 20.0f;
    pathCurrent_ = pathStart_;
    pathInProgress_ = true;
}

void GdiplusDrawContext::lineTo(float x, float y) {
    if (!pathInProgress_) {
        moveTo(x, y);
        return;
    }
    
    Gdiplus::PointF pt(x / 20.0f, y / 20.0f);
    currentPath_->AddLine(pathCurrent_, pt);
    pathCurrent_ = pt;
}

void GdiplusDrawContext::curveTo(float cx, float cy, float x, float y) {
    if (!pathInProgress_) return;
    
    // Quadratic bezier to cubic bezier conversion
    Gdiplus::PointF cp(cx / 20.0f, cy / 20.0f);
    Gdiplus::PointF ep(x / 20.0f, y / 20.0f);
    
    // Control points for cubic bezier
    Gdiplus::PointF cp1(
        pathCurrent_.X + 2.0f/3.0f * (cp.X - pathCurrent_.X),
        pathCurrent_.Y + 2.0f/3.0f * (cp.Y - pathCurrent_.Y)
    );
    Gdiplus::PointF cp2(
        ep.X + 2.0f/3.0f * (cp.X - ep.X),
        ep.Y + 2.0f/3.0f * (cp.Y - ep.Y)
    );
    
    currentPath_->AddBezier(pathCurrent_, cp1, cp2, ep);
    pathCurrent_ = ep;
}

void GdiplusDrawContext::closePath() {
    if (pathInProgress_) {
        currentPath_->CloseFigure();
    }
}

void GdiplusDrawContext::fillPath(const RGBA& color) {
    if (!graphics_) return;
    
    Gdiplus::SolidBrush brush(toGdiplusColor(color));
    graphics_->FillPath(&brush, currentPath_.get());
}

void GdiplusDrawContext::strokePath(const RGBA& color, float width) {
    if (!graphics_) return;
    
    Gdiplus::Pen pen(toGdiplusColor(color), width / 20.0f);
    graphics_->DrawPath(&pen, currentPath_.get());
}

void GdiplusDrawContext::setFillColor(const RGBA& color) {
    // Used for brush creation when filling
    // GDI+ doesn't have a persistent fill color, we create brushes on demand
}

void GdiplusDrawContext::setFillGradient(const std::vector<GradientStop>& records,
                                         const Matrix& matrix, bool radial) {
    if (!graphics_ || records.size() < 2) return;
    
    // Create gradient brush
    std::vector<Gdiplus::Color> colors;
    std::vector<Gdiplus::REAL> positions;
    
    for (const auto& record : records) {
        colors.push_back(Gdiplus::Color(record.color.a, record.color.r, 
                                        record.color.g, record.color.b));
        positions.push_back(record.ratio / 255.0f);
    }
    
    // TODO: Apply matrix transform to gradient
    // For now, create simple gradient
    if (radial) {
        // Radial gradient - center to edge
        Gdiplus::PointF center(0, 0);
        Gdiplus::PointF edge(16384 / 20.0f, 0);
        // Gdiplus::PathGradientBrush would be needed for radial
    } else {
        // Linear gradient
        Gdiplus::PointF p1(-16384 / 20.0f, 0);
        Gdiplus::PointF p2(16384 / 20.0f, 0);
        // Gdiplus::LinearGradientBrush would be created here
    }
}

void GdiplusDrawContext::setFillBitmap(uint16_t bitmapId, const Matrix& matrix,
                                       bool repeat, bool smooth) {
    // TODO: Load bitmap and create texture brush
}

void GdiplusDrawContext::drawText(const std::string& text, float x, float y, const RGBA& color) {
    if (!graphics_) return;
    
    Gdiplus::SolidBrush brush(toGdiplusColor(color));
    Gdiplus::FontFamily fontFamily(L"Arial");
    Gdiplus::Font font(&fontFamily, 12.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    
    Gdiplus::PointF pt(x / 20.0f, y / 20.0f);
    std::wstring wtext(text.begin(), text.end());
    
    graphics_->DrawString(wtext.c_str(), -1, &font, pt, &brush);
}

void GdiplusDrawContext::applyMatrix(const Matrix& matrix) {
    if (!graphics_) return;
    graphics_->SetTransform(&toGdiplusMatrix(matrix));
}

void GdiplusDrawContext::applyColor(const RGBA& color) {
    // Helper for creating GDI+ color
}

Gdiplus::Color GdiplusDrawContext::toGdiplusColor(const RGBA& rgba) {
    return Gdiplus::Color(rgba.a, rgba.r, rgba.g, rgba.b);
}

Gdiplus::Matrix GdiplusDrawContext::toGdiplusMatrix(const Matrix& m) {
    Gdiplus::Matrix matrix;
    
    if (m.hasScale || m.hasRotate) {
        Gdiplus::REAL elements[6] = {
            m.hasScale ? m.scaleX : 1.0f,  // m11
            m.hasRotate ? m.rotate1 : 0.0f, // m12
            m.hasRotate ? m.rotate0 : 0.0f, // m21
            m.hasScale ? m.scaleY : 1.0f,  // m22
            m.translateX / 20.0f,           // dx
            m.translateY / 20.0f            // dy
        };
        matrix.SetElements(elements[0], elements[1], elements[2], 
                          elements[3], elements[4], elements[5]);
    } else {
        matrix.Translate(m.translateX / 20.0f, m.translateY / 20.0f);
    }
    
    return matrix;
}

#endif // RENDERER_GDIPLUS
#endif // _WIN32

// ============== GdiplusRenderBackend (stub or implementation) ==============

bool GdiplusRenderBackend::supportsHardwareAccel() const {
#ifdef _WIN32
#ifdef RENDERER_GDIPLUS
    return false; // GDI+ is CPU-based with some GPU acceleration
#else
    return false;
#endif
#else
    return false; // Not available on non-Windows
#endif
}

bool GdiplusRenderBackend::supportsVectorGraphics() const {
#ifdef _WIN32
#ifdef RENDERER_GDIPLUS
    return true;
#else
    return false;
#endif
#else
    return false;
#endif
}

std::string GdiplusRenderBackend::getName() const {
    return "GDI+";
}

std::unique_ptr<IRenderTarget> GdiplusRenderBackend::createRenderTarget() {
#ifdef _WIN32
#ifdef RENDERER_GDIPLUS
    return std::make_unique<GdiplusRenderTarget>();
#else
    return nullptr;
#endif
#else
    return nullptr;
#endif
}

std::unique_ptr<IDrawContext> GdiplusRenderBackend::createDrawContext(IRenderTarget* target) {
#ifdef _WIN32
#ifdef RENDERER_GDIPLUS
    auto* gdiTarget = dynamic_cast<GdiplusRenderTarget*>(target);
    if (!gdiTarget) return nullptr;
    return std::make_unique<GdiplusDrawContext>(gdiTarget);
#else
    return nullptr;
#endif
#else
    return nullptr;
#endif
}

bool GdiplusRenderBackend::initialize() {
#ifdef _WIN32
#ifdef RENDERER_GDIPLUS
    if (initialized_) return true;
    
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartupOutput output;
    ULONG_PTR token;
    
    Gdiplus::Status status = Gdiplus::GdiplusStartup(&token, &input, &output);
    if (status == Gdiplus::Ok) {
        gdiplusToken_ = token;
        initialized_ = true;
        return true;
    }
    return false;
#else
    return false; // GDI+ not available
#endif
#else
    return false; // Not Windows
#endif
}

void GdiplusRenderBackend::shutdown() {
#ifdef _WIN32
#ifdef RENDERER_GDIPLUS
    if (initialized_ && gdiplusToken_ != 0) {
        Gdiplus::GdiplusShutdown(gdiplusToken_);
        gdiplusToken_ = 0;
        initialized_ = false;
    }
#endif
#endif
}

} // namespace libswf
