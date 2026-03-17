// Copyright (c) 2025 libswf authors. All rights reserved.
// Software Render Backend Implementation

#include "renderer/software_backend.h"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <fstream>

namespace libswf {

// ============== SoftwareRenderTarget ==============

SoftwareRenderTarget::SoftwareRenderTarget() = default;

bool SoftwareRenderTarget::initialize(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;
    pixels_.resize(width * height * 4);
    return true;
}

void SoftwareRenderTarget::clear(const RGBA& color) {
    size_t count = width_ * height_;
    uint8_t* ptr = pixels_.data();
    
    for (size_t i = 0; i < count; i++) {
        ptr[i * 4 + 0] = color.r;
        ptr[i * 4 + 1] = color.g;
        ptr[i * 4 + 2] = color.b;
        ptr[i * 4 + 3] = color.a;
    }
}

bool SoftwareRenderTarget::saveToFile(const std::string& filename) const {
    // Determine format by extension
    if (filename.size() > 4 && 
        (filename.substr(filename.size() - 4) == ".ppm" ||
         filename.substr(filename.size() - 4) == ".PPM")) {
        return saveToPPM(filename);
    }
    
    // Default to PPM for now
    return saveToPPM(filename + ".ppm");
}

bool SoftwareRenderTarget::saveToPPM(const std::string& filename) const {
    std::ofstream file(filename, std::ios::binary);
    if (!file) return false;
    
    // P6 format (binary PPM)
    file << "P6\n" << width_ << " " << height_ << "\n255\n";
    
    // Write RGB data (convert from RGBA)
    for (uint32_t y = 0; y < height_; y++) {
        for (uint32_t x = 0; x < width_; x++) {
            size_t idx = (y * width_ + x) * 4;
            file.write(reinterpret_cast<const char*>(&pixels_[idx]), 3);
        }
    }
    
    return file.good();
}

// ============== SoftwareDrawContext ==============

SoftwareDrawContext::SoftwareDrawContext(SoftwareRenderTarget* target)
    : target_(target) {
}

void SoftwareDrawContext::saveState() {
    stateStack_.push(currentState_);
}

void SoftwareDrawContext::restoreState() {
    if (!stateStack_.empty()) {
        currentState_ = stateStack_.top();
        stateStack_.pop();
    }
}

void SoftwareDrawContext::setTransform(const Matrix& matrix) {
    currentState_.transform = matrix;
}

void SoftwareDrawContext::multiplyTransform(const Matrix& matrix) {
    currentState_.transform = matrix.multiply(currentState_.transform);
}

void SoftwareDrawContext::setClipRect(const Rect& rect) {
    currentState_.clipRect = rect;
    currentState_.hasClip = true;
}

void SoftwareDrawContext::clearClip() {
    currentState_.hasClip = false;
}

void SoftwareDrawContext::drawRect(const Rect& rect, const RGBA& color) {
    // Draw rectangle outline
    int x1 = static_cast<int>(rect.xMin / 20.0f);
    int y1 = static_cast<int>(rect.yMin / 20.0f);
    int x2 = static_cast<int>(rect.xMax / 20.0f);
    int y2 = static_cast<int>(rect.yMax / 20.0f);
    
    drawLine(x1, y1, x2, y1, color, 1);
    drawLine(x2, y1, x2, y2, color, 1);
    drawLine(x2, y2, x1, y2, color, 1);
    drawLine(x1, y2, x1, y1, color, 1);
}

void SoftwareDrawContext::fillRect(const Rect& rect, const RGBA& color) {
    int x1 = static_cast<int>(rect.xMin / 20.0f);
    int y1 = static_cast<int>(rect.yMin / 20.0f);
    int x2 = static_cast<int>(rect.xMax / 20.0f);
    int y2 = static_cast<int>(rect.yMax / 20.0f);
    
    // Clamp to target bounds
    x1 = std::max(0, x1);
    y1 = std::max(0, y1);
    x2 = std::min(static_cast<int>(target_->getWidth()), x2);
    y2 = std::min(static_cast<int>(target_->getHeight()), y2);
    
    for (int y = y1; y < y2; y++) {
        for (int x = x1; x < x2; x++) {
            setPixelSafe(x, y, color);
        }
    }
}

void SoftwareDrawContext::drawLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1, 
                                   const RGBA& color, uint16_t width) {
    // Transform points
    float fx0 = x0, fy0 = y0, fx1 = x1, fy1 = y1;
    transformPoint(fx0, fy0);
    transformPoint(fx1, fy1);
    
    drawLineBresenham(static_cast<int>(fx0), static_cast<int>(fy0),
                      static_cast<int>(fx1), static_cast<int>(fy1), color);
}

void SoftwareDrawContext::drawLineBresenham(int x0, int y0, int x1, int y1, const RGBA& color) {
    int dx = std::abs(x1 - x0);
    int dy = std::abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;
    
    while (true) {
        setPixelSafe(x0, y0, color);
        
        if (x0 == x1 && y0 == y1) break;
        
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void SoftwareDrawContext::beginPath() {
    currentPath_.clear();
    pathInProgress_ = false;
}

void SoftwareDrawContext::moveTo(float x, float y) {
    transformPoint(x, y);
    currentPath_.push_back({x, y, false});
    pathInProgress_ = true;
}

void SoftwareDrawContext::lineTo(float x, float y) {
    transformPoint(x, y);
    currentPath_.push_back({x, y, false});
}

void SoftwareDrawContext::curveTo(float cx, float cy, float x, float y) {
    // Add control point and end point
    transformPoint(cx, cy);
    transformPoint(x, y);
    currentPath_.push_back({cx, cy, true});  // control point
    currentPath_.push_back({x, y, false});   // end point
}

void SoftwareDrawContext::closePath() {
    if (currentPath_.size() >= 2) {
        // Add line from last point to first
        currentPath_.push_back(currentPath_[0]);
    }
}

void SoftwareDrawContext::fillPath(const RGBA& color) {
    if (currentPath_.size() < 3) return;
    
    // Convert to integer points for scanline fill
    std::vector<std::pair<int32_t, int32_t>> points;
    points.reserve(currentPath_.size());
    
    for (const auto& p : currentPath_) {
        if (!p.isCurve) {
            points.push_back({static_cast<int32_t>(p.x), static_cast<int32_t>(p.y)});
        }
    }
    
    fillPolygonScanline(points, color);
}

void SoftwareDrawContext::strokePath(const RGBA& color, float width) {
    if (currentPath_.size() < 2) return;
    
    // Draw lines between consecutive points
    for (size_t i = 0; i < currentPath_.size() - 1; i++) {
        const auto& p0 = currentPath_[i];
        const auto& p1 = currentPath_[i + 1];
        
        if (!p0.isCurve && !p1.isCurve) {
            drawLineBresenham(static_cast<int>(p0.x), static_cast<int>(p0.y),
                              static_cast<int>(p1.x), static_cast<int>(p1.y), color);
        }
    }
}

void SoftwareDrawContext::fillPolygonScanline(
    const std::vector<std::pair<int32_t, int32_t>>& points, const RGBA& color) {
    
    if (points.size() < 3) return;
    
    // Find Y bounds
    int32_t yMin = points[0].second;
    int32_t yMax = points[0].second;
    for (const auto& p : points) {
        yMin = std::min(yMin, p.second);
        yMax = std::max(yMax, p.second);
    }
    
    yMin = std::max(yMin, 0);
    yMax = std::min(yMax, static_cast<int32_t>(target_->getHeight()) - 1);
    
    // Scanline fill
    for (int32_t y = yMin; y <= yMax; y++) {
        std::vector<int32_t> intersections;
        
        // Find intersections with this scanline
        for (size_t i = 0; i < points.size(); i++) {
            size_t j = (i + 1) % points.size();
            const auto& p0 = points[i];
            const auto& p1 = points[j];
            
            // Check if edge crosses this scanline
            if ((p0.second <= y && p1.second > y) || (p1.second <= y && p0.second > y)) {
                // Calculate intersection X
                float t = static_cast<float>(y - p0.second) / (p1.second - p0.second);
                int32_t x = static_cast<int32_t>(p0.first + t * (p1.first - p0.first));
                intersections.push_back(x);
            }
        }
        
        // Sort intersections
        std::sort(intersections.begin(), intersections.end());
        
        // Fill between pairs of intersections
        for (size_t i = 0; i + 1 < intersections.size(); i += 2) {
            int32_t x1 = std::max(intersections[i], 0);
            int32_t x2 = std::min(intersections[i + 1], static_cast<int32_t>(target_->getWidth()) - 1);
            
            for (int32_t x = x1; x <= x2; x++) {
                setPixelSafe(x, y, color);
            }
        }
    }
}

void SoftwareDrawContext::setFillColor(const RGBA& color) {
    currentState_.fillColor = color;
}

void SoftwareDrawContext::setFillGradient(const std::vector<GradientStop>& records,
                                          const Matrix& matrix, bool radial) {
    // TODO: Implement gradient fills
}

void SoftwareDrawContext::setFillBitmap(uint16_t bitmapId, const Matrix& matrix,
                                        bool repeat, bool smooth) {
    // TODO: Implement bitmap fills
}

void SoftwareDrawContext::drawText(const std::string& text, float x, float y, const RGBA& color) {
    // TODO: Implement basic text rendering (optional)
    // For now, draw a placeholder rectangle
    int ix = static_cast<int>(x);
    int iy = static_cast<int>(y);
    int w = static_cast<int>(text.size() * 8);
    
    for (int dy = 0; dy < 12; dy++) {
        for (int dx = 0; dx < w; dx++) {
            setPixelSafe(ix + dx, iy + dy, color);
        }
    }
}

void SoftwareDrawContext::setPixel(int x, int y, const RGBA& color) {
    uint8_t* pixels = target_->getPixels();
    size_t idx = (y * target_->getWidth() + x) * 4;
    
    // Simple alpha blend
    if (color.a == 255) {
        pixels[idx + 0] = color.r;
        pixels[idx + 1] = color.g;
        pixels[idx + 2] = color.b;
        pixels[idx + 3] = color.a;
    } else if (color.a > 0) {
        uint8_t alpha = color.a;
        uint8_t invAlpha = 255 - alpha;
        
        pixels[idx + 0] = (color.r * alpha + pixels[idx + 0] * invAlpha) / 255;
        pixels[idx + 1] = (color.g * alpha + pixels[idx + 1] * invAlpha) / 255;
        pixels[idx + 2] = (color.b * alpha + pixels[idx + 2] * invAlpha) / 255;
        pixels[idx + 3] = std::min(255, pixels[idx + 3] + alpha);
    }
}

void SoftwareDrawContext::setPixelSafe(int x, int y, const RGBA& color) {
    if (x < 0 || y < 0 || x >= static_cast<int>(target_->getWidth()) || 
        y >= static_cast<int>(target_->getHeight())) {
        return;
    }
    
    // Clip test
    if (currentState_.hasClip) {
        float fx = x, fy = y;
        // Inverse transform to check clip
        if (fx < currentState_.clipRect.xMin || fx > currentState_.clipRect.xMax ||
            fy < currentState_.clipRect.yMin || fy > currentState_.clipRect.yMax) {
            return;
        }
    }
    
    setPixel(x, y, color);
}

void SoftwareDrawContext::transformPoint(float& x, float& y) const {
    const Matrix& m = currentState_.transform;
    
    if (m.hasScale) {
        x = x * m.scaleX / 65536.0f;
        y = y * m.scaleY / 65536.0f;
    }
    if (m.hasRotate) {
        float rx = x * m.rotate0 / 65536.0f - y * m.rotate1 / 65536.0f;
        float ry = x * m.rotate1 / 65536.0f + y * m.rotate0 / 65536.0f;
        x = rx;
        y = ry;
    }
    x += m.translateX / 20.0f;
    y += m.translateY / 20.0f;
}

bool SoftwareDrawContext::clipTest(int x, int y) const {
    if (!currentState_.hasClip) return true;
    
    return x >= static_cast<int>(currentState_.clipRect.xMin) &&
           x <= static_cast<int>(currentState_.clipRect.xMax) &&
           y >= static_cast<int>(currentState_.clipRect.yMin) &&
           y <= static_cast<int>(currentState_.clipRect.yMax);
}

// ============== SoftwareRenderBackend ==============

std::unique_ptr<IRenderTarget> SoftwareRenderBackend::createRenderTarget() {
    return std::make_unique<SoftwareRenderTarget>();
}

std::unique_ptr<IDrawContext> SoftwareRenderBackend::createDrawContext(IRenderTarget* target) {
    auto* swTarget = dynamic_cast<SoftwareRenderTarget*>(target);
    if (!swTarget) return nullptr;
    return std::make_unique<SoftwareDrawContext>(swTarget);
}

bool SoftwareRenderBackend::initialize() {
    // Software backend doesn't need special initialization
    return true;
}

void SoftwareRenderBackend::shutdown() {
    // Nothing to clean up
}

} // namespace libswf
