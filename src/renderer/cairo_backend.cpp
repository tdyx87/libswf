// Copyright (c) 2025 libswf authors. All rights reserved.
// Cairo Render Backend Implementation

#include "renderer/cairo_backend.h"
#include <algorithm>
#include <fstream>

namespace libswf {

#ifdef RENDERER_CAIRO

// ============== CairoRenderTarget ==============

CairoRenderTarget::CairoRenderTarget() = default;

CairoRenderTarget::~CairoRenderTarget() {
    if (cr_) {
        cairo_destroy(cr_);
        cr_ = nullptr;
    }
    if (surface_) {
        cairo_surface_destroy(surface_);
        surface_ = nullptr;
    }
}

bool CairoRenderTarget::initialize(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;
    
    // Create ARGB32 surface
    surface_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    if (cairo_surface_status(surface_) != CAIRO_STATUS_SUCCESS) {
        return false;
    }
    
    surfaceData_ = cairo_image_surface_get_data(surface_);
    
    // Create context
    cr_ = cairo_create(surface_);
    if (cairo_status(cr_) != CAIRO_STATUS_SUCCESS) {
        return false;
    }
    
    // Clear to white
    cairo_set_source_rgb(cr_, 1.0, 1.0, 1.0);
    cairo_paint(cr_);
    
    return true;
}

void CairoRenderTarget::clear(const RGBA& color) {
    if (!cr_) return;
    
    cairo_set_source_rgba(cr_, 
                          color.r / 255.0,
                          color.g / 255.0,
                          color.b / 255.0,
                          color.a / 255.0);
    cairo_paint(cr_);
}

bool CairoRenderTarget::saveToFile(const std::string& filename) const {
    if (!surface_) return false;
    
    // Determine format by extension
    if (filename.size() > 4) {
        std::string ext = filename.substr(filename.size() - 4);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        
        if (ext == ".png") {
            return cairo_surface_write_to_png(surface_, filename.c_str()) == CAIRO_STATUS_SUCCESS;
        }
    }
    
    // Default to PNG
    return cairo_surface_write_to_png(surface_, (filename + ".png").c_str()) == CAIRO_STATUS_SUCCESS;
}

// ============== CairoDrawContext ==============

CairoDrawContext::CairoDrawContext(CairoRenderTarget* target)
    : target_(target), cr_(target->getContext()) {
}

void CairoDrawContext::saveState() {
    if (cr_) cairo_save(cr_);
}

void CairoDrawContext::restoreState() {
    if (cr_) cairo_restore(cr_);
}

void CairoDrawContext::setTransform(const Matrix& matrix) {
    if (!cr_) return;
    
    cairo_matrix_t cm;
    convertMatrix(matrix, cm);
    cairo_set_matrix(cr_, &cm);
}

void CairoDrawContext::multiplyTransform(const Matrix& matrix) {
    if (!cr_) return;
    
    cairo_matrix_t cm, current;
    convertMatrix(matrix, cm);
    cairo_get_matrix(cr_, &current);
    cairo_matrix_multiply(&current, &cm, &current);
    cairo_set_matrix(cr_, &current);
}

void CairoDrawContext::setClipRect(const Rect& rect) {
    if (!cr_) return;
    
    cairo_reset_clip(cr_);
    cairo_rectangle(cr_, 
                    rect.xMin / 20.0, 
                    rect.yMin / 20.0,
                    (rect.xMax - rect.xMin) / 20.0,
                    (rect.yMax - rect.yMin) / 20.0);
    cairo_clip(cr_);
}

void CairoDrawContext::clearClip() {
    if (cr_) cairo_reset_clip(cr_);
}

void CairoDrawContext::drawRect(const Rect& rect, const RGBA& color) {
    if (!cr_) return;
    
    applyColor(color);
    cairo_rectangle(cr_, 
                    rect.xMin / 20.0, 
                    rect.yMin / 20.0,
                    (rect.xMax - rect.xMin) / 20.0,
                    (rect.yMax - rect.yMin) / 20.0);
    cairo_stroke(cr_);
}

void CairoDrawContext::fillRect(const Rect& rect, const RGBA& color) {
    if (!cr_) return;
    
    applyColor(color);
    cairo_rectangle(cr_, 
                    rect.xMin / 20.0, 
                    rect.yMin / 20.0,
                    (rect.xMax - rect.xMin) / 20.0,
                    (rect.yMax - rect.yMin) / 20.0);
    cairo_fill(cr_);
}

void CairoDrawContext::drawLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1, 
                                const RGBA& color, uint16_t width) {
    if (!cr_) return;
    
    applyColor(color);
    cairo_set_line_width(cr_, width);
    cairo_move_to(cr_, x0 / 20.0, y0 / 20.0);
    cairo_line_to(cr_, x1 / 20.0, y1 / 20.0);
    cairo_stroke(cr_);
}

void CairoDrawContext::beginPath() {
    if (cr_) cairo_new_path(cr_);
}

void CairoDrawContext::moveTo(float x, float y) {
    if (cr_) cairo_move_to(cr_, x / 20.0, y / 20.0);
}

void CairoDrawContext::lineTo(float x, float y) {
    if (cr_) cairo_line_to(cr_, x / 20.0, y / 20.0);
}

void CairoDrawContext::curveTo(float cx, float cy, float x, float y) {
    if (cr_) cairo_curve_to(cr_, cx / 20.0, cy / 20.0, cx / 20.0, cy / 20.0, x / 20.0, y / 20.0);
    // Note: Using control point twice for quadratic bezier approximation
}

void CairoDrawContext::closePath() {
    if (cr_) cairo_close_path(cr_);
}

void CairoDrawContext::fillPath(const RGBA& color) {
    if (!cr_) return;
    
    applyColor(color);
    cairo_fill(cr_);
}

void CairoDrawContext::strokePath(const RGBA& color, float width) {
    if (!cr_) return;
    
    applyColor(color);
    cairo_set_line_width(cr_, width / 20.0);
    cairo_stroke(cr_);
}

void CairoDrawContext::setFillColor(const RGBA& color) {
    if (!cr_) return;
    applyColor(color);
}

void CairoDrawContext::setFillGradient(const std::vector<GradientStop>& records,
                                       const Matrix& matrix, bool radial) {
    if (!cr_ || records.size() < 2) return;
    
    // Create gradient pattern
    cairo_pattern_t* pattern;
    
    if (radial) {
        // Radial gradient
        pattern = cairo_pattern_create_radial(0, 0, 0, 0, 0, 16384 / 20.0);
    } else {
        // Linear gradient
        pattern = cairo_pattern_create_linear(-16384 / 20.0, 0, 16384 / 20.0, 0);
    }
    
    // Add color stops
    for (const auto& record : records) {
        RGBA color = record.color;
        cairo_pattern_add_color_stop_rgba(pattern, 
                                          record.ratio / 255.0,
                                          color.r / 255.0,
                                          color.g / 255.0,
                                          color.b / 255.0,
                                          color.a / 255.0);
    }
    
    // Apply gradient matrix
    cairo_matrix_t cm;
    convertMatrix(matrix, cm);
    cairo_pattern_set_matrix(pattern, &cm);
    
    cairo_set_source(cr_, pattern);
    cairo_pattern_destroy(pattern);
}

void CairoDrawContext::setFillBitmap(uint16_t bitmapId, const Matrix& matrix,
                                     bool repeat, bool smooth) {
    // TODO: Implement bitmap pattern fills
    // This would require loading the bitmap and creating a cairo_surface_pattern_t
}

void CairoDrawContext::drawText(const std::string& text, float x, float y, const RGBA& color) {
    if (!cr_) return;
    
    applyColor(color);
    cairo_move_to(cr_, x / 20.0, y / 20.0);
    
    // Set font (default sans-serif)
    cairo_select_font_face(cr_, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr_, 12.0);
    
    cairo_show_text(cr_, text.c_str());
}

void CairoDrawContext::applyColor(const RGBA& color) {
    if (!cr_) return;
    
    cairo_set_source_rgba(cr_,
                          color.r / 255.0,
                          color.g / 255.0,
                          color.b / 255.0,
                          color.a / 255.0);
}

void CairoDrawContext::applyMatrix(const Matrix& matrix) {
    if (!cr_) return;
    
    cairo_matrix_t cm;
    convertMatrix(matrix, cm);
    cairo_transform(cr_, &cm);
}

void CairoDrawContext::convertMatrix(const Matrix& m, cairo_matrix_t& cm) {
    // SWF matrix to Cairo matrix conversion
    // Cairo uses a different matrix format: xx, yx, xy, yy, x0, y0
    
    if (m.hasScale) {
        cm.xx = m.scaleX / 65536.0;
        cm.yy = m.scaleY / 65536.0;
    } else {
        cm.xx = 1.0;
        cm.yy = 1.0;
    }
    
    if (m.hasRotate) {
        cm.yx = m.rotate1 / 65536.0;
        cm.xy = m.rotate0 / 65536.0;
    } else {
        cm.yx = 0.0;
        cm.xy = 0.0;
    }
    
    cm.x0 = m.translateX / 20.0;
    cm.y0 = m.translateY / 20.0;
}

#endif // RENDERER_CAIRO

// ============== CairoRenderBackend (stub or implementation) ==============

bool CairoRenderBackend::supportsHardwareAccel() const {
#ifdef RENDERER_CAIRO
    return false; // Cairo is CPU-based
#else
    return false;
#endif
}

bool CairoRenderBackend::supportsVectorGraphics() const {
#ifdef RENDERER_CAIRO
    return true;
#else
    return false;
#endif
}

std::string CairoRenderBackend::getName() const {
    return "Cairo";
}

std::unique_ptr<IRenderTarget> CairoRenderBackend::createRenderTarget() {
#ifdef RENDERER_CAIRO
    return std::make_unique<CairoRenderTarget>();
#else
    return nullptr;
#endif
}

std::unique_ptr<IDrawContext> CairoRenderBackend::createDrawContext(IRenderTarget* target) {
#ifdef RENDERER_CAIRO
    auto* cairoTarget = dynamic_cast<CairoRenderTarget*>(target);
    if (!cairoTarget) return nullptr;
    return std::make_unique<CairoDrawContext>(cairoTarget);
#else
    return nullptr;
#endif
}

bool CairoRenderBackend::initialize() {
#ifdef RENDERER_CAIRO
    return true;
#else
    return false; // Cairo not available
#endif
}

void CairoRenderBackend::shutdown() {
    // Nothing to do
}

} // namespace libswf
